#include "pch_script.h"
#include "lua_spike_probe.h"

#include "Level.h"
#include "xrEngine/xr_object.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "xrScriptEngine/script_engine.hpp"

#ifdef LUAJIT_GW_GCSTATS
// G2 (32-frame-spikes.md): the census names the luabind instances by their class (object_rep -> class_rep)
#include <luabind/detail/class_registry.hpp>
#include <luabind/detail/class_rep.hpp>
#include <luabind/detail/object_rep.hpp>
#endif

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <unordered_set>

// P18 (plans/optimization_mt/13-lua-spikes.md). See lua_spike_probe.h.

namespace gw::lua_spike
{
int g_threshold_us = 0;
int g_paced_mul = 200; // 2026-10-18 (42-lua-gc-prewalk.md): shorter GC cycles, fewer dead udata at the atomic phase
int g_step_budget_us = 2000; // 1000 -> 2000 (2026-10-17, 32-frame-spikes.md G0: lua/gc_uncapped 1-2 per 5 s -> 1 per session); a tunable
int g_budget_max_us = 5000; // G1 (32-frame-spikes.md): 0 = the K2a cliff as before; a tunable, the default since 2026-10-17
int g_debt_soft = 2000;
int g_heap_guard_pct = 130;
int g_atomic_stats = 0;
int g_gc_pause = 0;
int g_gc_stepmul = 0;

namespace probe_detail
{
// The registry key of the sentinel table: the address of this variable (a light userdata, nothing to intern)
char sentinel_key = 0;
u32 atomic_epoch = 0; // atomic phases seen (all Lua states)
u32 open_probes = 0; // probes open right now: the depth of the nested Lua entry points

constexpr pcstr category_names[] = { "enemy_callback", "event_lua", "event_native", "script_call", "binder", "gc_step",
    "binder_cb", "add_call", "event_handler" };
static_assert(std::size(category_names) == static_cast<size_t>(ECategory::Count), "a name for every category");
u32 spikes[static_cast<size_t>(ECategory::Count)] = {};
u32 spikes_gc[static_cast<size_t>(ECategory::Count)] = {};

// A call that freed this much was sweeping: the GC ran inside it even without the atomic phase
constexpr int sweep_drop_kb = 1024;
// At most this many lines per second (a low threshold must not flood the log); the rest are counted only
constexpr u32 lines_per_second = 20;
u32 lines_second_start = 0;
u32 lines_in_second = 0;
u32 lines_dropped = 0;

// Pushes the sentinel table; creates it on the first call in this Lua state (a new game or a load makes a new state,
// the light userdata key is simply missing in its registry). created = true for a new table.
void PushSentinel(lua_State* L, bool& created)
{
    created = false;
    lua_pushlightuserdata(L, &sentinel_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_istable(L, -1))
        return;
    lua_pop(L, 1);
    lua_newtable(L); // W
    lua_newtable(L); // W, mt
    lua_pushliteral(L, "k");
    lua_setfield(L, -2, "__mode"); // weak keys
    lua_setmetatable(L, -2); // W
    lua_pushlightuserdata(L, &sentinel_key);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX); // registry[&sentinel_key] = W; W stays on the stack
    created = true;
}

// W on the top: puts in a fresh key that nothing else refers to - the next atomic phase clears it
void Arm(lua_State* L)
{
    lua_newtable(L);
    lua_pushboolean(L, 1);
    lua_rawset(L, -3);
}

pcstr ThreadName()
{
    switch (xray::perf::CurrentThreadKind())
    {
    case xray::perf::ThreadKind::Main: return "main";
    case xray::perf::ThreadKind::Game: return "game";
    default: return "worker";
    }
}

// "id 'name'" of a level object, "-" for none
void DescribeObject(string128& out, u32 id)
{
    if (id == kNoId)
    {
        xr_strcpy(out, "-");
        return;
    }
    IGameObject* object = g_pGameLevel ? Level().Objects.net_Find(static_cast<u16>(id)) : nullptr;
    if (!object)
    {
        xr_sprintf(out, "%u", id);
        return;
    }
    const shared_str name = object->cName();
    xr_sprintf(out, "%u '%s'", id, name.c_str() ? name.c_str() : "");
}

struct PacedState
{
    lua_State* L = nullptr; // the state the numbers below belong to
    s64 due = 0; // GC steps owed (one per KB allocated at mul 100)
    int last_kb = 0; // heap after the previous paced frame
    u32 last_ms = 0; // CPU::GetTicks() of the previous paced frame
    bool holding = false; // the implicit steps are held off (the threshold is far ahead)
    int cycle_start_kb = 0; // G1: the heap when the current GC cycle started (a finished step or a reset)
    // G2, lua_gc_pause > 0: the heap at the end of the last cycle; the next one waits until the heap passes
    // lua_gc_pause % of it (0 = not waiting)
    int wait_base_kb = 0;
};
PacedState paced;
GcFrameStats last_frame;

// A gap without script_gc longer than this (loading, menu, a hitch) is not paid back in one frame
constexpr u32 paced_gap_reset_ms = 1000;
// Safety net between the paced frames: the implicit GC resumes only if the heap grows by half
constexpr int paced_hold_pct = 150;
// K2a (26-framemove-game-next.md), lua_gc_step_budget_us > 0: a debt above this many steps (~16 MB allocated at mul
// 100, tens of frames of a fight) is paid off in one frame without the time cap - the cap must not let the heap run
// away from the GC (spawn waves, a long series of capped frames)
constexpr s64 paced_debt_uncapped = 16 * 1024;
// G1 (lua_gc_budget_max_us > 0): the growing budget replaces that cliff; uncapped only past this debt (~64 MB allocated)
constexpr s64 paced_debt_uncapped_smooth = 4 * paced_debt_uncapped;
// A single step at least this long is timed as lua/gc_long_step: the atomic phase (40-230 ms with a 230-270 MB heap in
// the logs of 2026-10-16) or the traversal of one huge table; ordinary steps take a few us
constexpr u64 paced_long_step_us = 2000;

// threshold = heap * paced_hold_pct / 100: LUA_GCRESTART with -1 sets it to (heap / 100) * pause
void HoldImplicitSteps(lua_State* L)
{
    const int pause = lua_gc(L, LUA_GCSETPAUSE, paced_hold_pct);
    lua_gc(L, LUA_GCRESTART, -1);
    lua_gc(L, LUA_GCSETPAUSE, pause);
}
} // namespace probe_detail

u32 PollAtomic(lua_State* L) noexcept
{
    using namespace probe_detail;
    if (!L || !lua_checkstack(L, 4))
        return atomic_epoch;
    bool created = false;
    PushSentinel(L, created); // W
    if (!created)
    {
        lua_pushnil(L);
        if (lua_next(L, -2)) // W, k, v: still armed
        {
            lua_pop(L, 3);
            return atomic_epoch;
        }
        // W only: the key is gone - atomic() cleared the weak tables since the previous poll
        ++atomic_epoch;
        PERF_INC(lua_gc_atomic);
    }
    Arm(L);
    lua_pop(L, 1);
    return atomic_epoch;
}

void Probe::Begin(lua_State* L, ECategory category) noexcept
{
    m_epoch = PollAtomic(L);
    m_kb_before = lua_gc(L, LUA_GCCOUNT, 0);
    m_category = category;
    m_depth = probe_detail::open_probes++;
    m_L = L;
    m_perf_start = xray::perf::Ticks();
    m_start = CPU::QPC();
}

bool Probe::FinishSlow() noexcept
{
    m_ticks = CPU::QPC() - m_start;
    m_perf_ticks = xray::perf::Ticks() - m_perf_start;
    if (g_threshold_us <= 0 || m_ticks * 1000000 < static_cast<u64>(g_threshold_us) * CPU::qpc_freq)
        return false;
    m_epoch_after = PollAtomic(m_L);
    m_kb_after = lua_gc(m_L, LUA_GCCOUNT, 0);
    return true;
}

void Probe::Leave() noexcept
{
    --probe_detail::open_probes;
    m_L = nullptr;
}

void Probe::Report(pcstr what, u32 npc_id, u32 other_id, bool function_on_top) noexcept
{
    using namespace probe_detail;
    string256 function;
    function[0] = 0;
    if (function_on_top && m_L)
    {
        lua_Debug ar{};
        if (lua_isfunction(m_L, -1))
        {
            // '>' takes the function from the top of the stack
            if (lua_getinfo(m_L, ">S", &ar))
                xr_sprintf(function, " fn=%s:%d", ar.short_src, ar.linedefined);
        }
        else
            lua_pop(m_L, 1);
    }
    if (!m_L || m_category >= ECategory::Count)
        return;

    const size_t category = static_cast<size_t>(m_category);
    const bool atomic = m_epoch_after != m_epoch;
    const int delta_kb = m_kb_after - m_kb_before;
    const bool gc = atomic || delta_kb <= -sweep_drop_kb;
    ++spikes[category];
    if (gc)
        ++spikes_gc[category];
    if (m_depth == 0) // the outermost probe only: a nested one is a part of the same time
    {
        PERF_ADD(lua_spike, m_perf_ticks);
        if (gc)
            PERF_ADD(lua_spike_gc, m_perf_ticks);
    }

    const u32 now_ms = CPU::GetTicks();
    if (now_ms - lines_second_start >= 1000)
    {
        lines_second_start = now_ms;
        lines_in_second = 0;
    }
    if (++lines_in_second > lines_per_second)
    {
        ++lines_dropped;
        return;
    }
    string32 dropped;
    dropped[0] = 0;
    if (lines_dropped)
    {
        xr_sprintf(dropped, " (+%u not logged)", lines_dropped);
        lines_dropped = 0;
    }
    string128 npc, other;
    DescribeObject(npc, npc_id);
    DescribeObject(other, other_id);
    const u64 us = m_ticks * 1000000 / CPU::qpc_freq;
    Msg("! [lua-spike] %s %llu us '%s'%s npc=%s other=%s heap %d->%d KB (%+d) gc_atomic=%d thread=%s depth=%u "
        "frame=%u | %s: %u spikes, %u with gc%s",
        category_names[category], us, what ? what : "", function, npc, other, m_kb_before, m_kb_after, delta_kb,
        atomic ? 1 : 0, ThreadName(), m_depth, Device.dwFrame, category_names[category], spikes[category],
        spikes_gc[category], dropped);
}

namespace gc_diag
{
// G2: logs the atomic phase recorded since the last call (lua_gc_atomic_stats 1); paced = it ran in the paced steps
void LogAtomicIfNew(bool paced) noexcept;
} // namespace gc_diag

void GcStepFrame(lua_State* L, int step_kb) noexcept
{
    using namespace probe_detail;
    if (!L)
        return;

    const int now_kb = lua_gc(L, LUA_GCCOUNT, 0);
    const u32 now_ms = CPU::GetTicks();
    if (paced.L != L || !paced.holding || now_ms - paced.last_ms > paced_gap_reset_ms)
    {
        // The first paced frame, a new Lua state or a long gap: start counting from here
        paced.L = L;
        paced.due = 0;
        paced.last_kb = now_kb;
        paced.cycle_start_kb = now_kb;
        paced.wait_base_kb = 0;
    }
    // What LuaJIT would have stepped implicitly for the allocations since the previous frame (lj_gc_step per KB,
    // lj_gc.c) plus the explicit step of lua_gcstep as before
    const int allocated_kb = std::max(now_kb - paced.last_kb, 0);
    paced.due += static_cast<s64>(allocated_kb) * std::max(g_paced_mul, 1) / 100 + std::max(step_kb, 0);
    // G2 (A/B), lua_gc_pause > 0: the pause of LuaJIT between the cycles - nothing is owed until the heap passes
    // lua_gc_pause % of the heap at the end of the last cycle; the new cycle starts from the heap of that frame
    if (g_gc_pause <= 0)
        paced.wait_base_kb = 0;
    else if (paced.wait_base_kb > 0)
    {
        if (s64(now_kb) * 100 < s64(paced.wait_base_kb) * g_gc_pause)
            paced.due = 0;
        else
        {
            paced.wait_base_kb = 0;
            paced.cycle_start_kb = now_kb; // the heap guard (G1) counts from here, not from the end of the last cycle
        }
    }

    const bool probe = Enabled();
    // The spike logger (perf_spike_us) describes the frame: the atomic phase is checked once around the steps
    const bool spike = xray::perf::SpikeActive();
    u32 epoch = probe || spike ? PollAtomic(L) : 0;
    const u32 epoch_start = epoch;
    // K2a: the time cap (QPC: works without XR_PERF_METRICS too). A debt over paced_debt_uncapped goes uncapped.
    // G1 (lua_gc_budget_max_us > 0): the cap grows with the debt, uncapped only far beyond (or the heap runs away).
    const bool smooth = g_step_budget_us > 0 && g_budget_max_us > 0;
    const int heap_growth_pct = paced.cycle_start_kb > 0 ? static_cast<int>(s64(now_kb) * 100 / paced.cycle_start_kb) : 100;
    const bool heap_guard = smooth && g_heap_guard_pct > 0 && heap_growth_pct > std::max(g_heap_guard_pct, 100); // 1-99 = 100
    bool capped;
    u64 budget_us = 0;
    if (smooth)
    {
        capped = paced.due <= paced_debt_uncapped_smooth && !heap_guard;
        const s64 base = g_step_budget_us;
        const s64 grown = base + base * paced.due / std::max(g_debt_soft, 1);
        budget_us = static_cast<u64>(std::clamp<s64>(grown, base, std::max<s64>(g_budget_max_us, base)));
        xray::perf::SetGauge(xray::perf::Metric::lua_gc_heap_growth_pct, heap_growth_pct);
        if (heap_guard)
            PERF_INC(lua_gc_uncapped_heap);
    }
    else
    {
        capped = g_step_budget_us > 0 && paced.due <= paced_debt_uncapped;
        budget_us = static_cast<u64>(std::max(g_step_budget_us, 0));
    }
    if (g_step_budget_us > 0 && !capped)
        PERF_INC(lua_gc_uncapped);
    xray::perf::SetGauge(xray::perf::Metric::lua_gc_budget_us, capped ? static_cast<s64>(budget_us) : 0);
    last_frame.due_before = paced.due;
    last_frame.budget_us = capped ? static_cast<u32>(budget_us) : 0;
    last_frame.uncapped = g_step_budget_us > 0 && !capped;
    last_frame.heap_guard = heap_guard && !capped;
    const u64 budget_qpc = capped ? budget_us * CPU::qpc_freq / 1000000 : 0;
    const u64 frame_start = capped ? CPU::QPC() : 0;
    // lua/gc_long_step: perf_metrics ticks of a single step (rdtsc - cheap enough for every step)
    const bool metrics = xray::perf::g_perf_metrics_active;
    const bool timed = probe || metrics || spike;
    const double ms_per_tick = timed ? xray::perf::TicksToMilliseconds(1) : 0.0;
    const u64 long_ticks = ms_per_tick > 0.0 ? static_cast<u64>(paced_long_step_us / 1000.0 / ms_per_tick) : u64(-1);
    u64 max_step_ticks = 0;
    u64 steps = 0;
    while (paced.due > 0)
    {
        const u64 t0 = timed ? xray::perf::Ticks() : 0;
        // data 0: exactly one lj_gc_step (more only to pay an old debt of LuaJIT); 1 = the cycle has finished
        const int finished = lua_gc(L, LUA_GCSTEP, 0);
        --paced.due;
        ++steps;
        if (timed)
        {
            const u64 dt = xray::perf::Ticks() - t0;
            if (dt > max_step_ticks)
                max_step_ticks = dt;
            if (metrics && dt >= long_ticks)
                PERF_ADD(lua_gc_long_step, dt);
        }
        if (probe)
        {
            const u32 now_epoch = PollAtomic(L);
            if (now_epoch != epoch)
            {
                // This one step ran atomic(): its time is the atomic phase (+ at most a few sweep steps)
                PERF_ADD(lua_gc_atomic_step, xray::perf::Ticks() - t0);
                epoch = now_epoch;
            }
        }
        if (finished)
        {
            // As lua_gc(LUA_GCSTEP) of gc_step: the next cycle starts with the next frame
            paced.due = 0;
            paced.cycle_start_kb = lua_gc(L, LUA_GCCOUNT, 0);
            paced.wait_base_kb = g_gc_pause > 0 ? paced.cycle_start_kb : 0; // G2: lua_gc_pause
            break;
        }
        if (capped && paced.due > 0 && CPU::QPC() - frame_start >= budget_qpc)
        {
            // The rest of the debt waits for the next frames (paced.due lives between them, see above)
            PERF_INC(lua_gc_capped);
            break;
        }
    }
    if (spike && !probe)
        epoch = PollAtomic(L);
    last_frame.steps = steps;
    last_frame.due_after = paced.due;
    last_frame.max_step_us = static_cast<u32>(ms_per_tick * 1000.0 * static_cast<double>(max_step_ticks));
    last_frame.atomic = probe || spike ? (epoch != epoch_start ? 1 : 0) : -1;
    PERF_ADD(lua_gc_paced_steps, steps);
    xray::perf::SetGauge(xray::perf::Metric::lua_gc_debt, static_cast<s64>(paced.due));
    paced.last_kb = lua_gc(L, LUA_GCCOUNT, 0);
    paced.last_ms = now_ms;
    last_frame.heap_kb = paced.last_kb;
    last_frame.cycle_start_kb = paced.cycle_start_kb;
    HoldImplicitSteps(L);
    paced.holding = true;
    gc_diag::LogAtomicIfNew(true); // G2, lua_gc_atomic_stats 1: the atomic phase of these steps
}

const GcFrameStats& LastGcFrame() noexcept { return probe_detail::last_frame; }

// --- G2 (plans/optimization_mt/32-frame-spikes.md): the atomic phase and the heap -------------------------------------
namespace gc_diag
{
using ull = unsigned long long;

double QpcMs(u64 ticks) { return CPU::qpc_freq ? static_cast<double>(ticks) * 1000.0 / CPU::qpc_freq : 0.0; }

// lua_gc_pause / lua_gc_stepmul applied to a Lua state (a new state starts with the LuaJIT defaults)
struct Tuning
{
    lua_State* L = nullptr;
    int pause_set = 0; // the cvar value now in L, 0 = untouched
    int stepmul_set = 0;
    int pause_orig = -1; // the value of L before the first change
    int stepmul_orig = -1;
};
Tuning tuning;

// want > 0: LUA_GCSETPAUSE / LUA_GCSETSTEPMUL = want; 0: back to the value before the first change
void ApplyOne(lua_State* L, int what, int want, int& set, int& orig, pcstr name)
{
    if (want == set)
        return;
    if (want > 0)
    {
        const int prev = lua_gc(L, what, want);
        if (set == 0)
            orig = prev;
        Msg("* [lua-gc] %s = %d (was %d)", name, want, prev);
    }
    else if (orig >= 0)
    {
        lua_gc(L, what, orig);
        Msg("* [lua-gc] %s = %d (restored)", name, orig);
    }
    set = want;
}

void ApplyTuning(lua_State* L)
{
    if (tuning.L != L)
    {
        tuning = Tuning{};
        tuning.L = L;
    }
    // HoldImplicitSteps (paced_hold_pct) swaps the pause only for one call and puts this value back
    ApplyOne(L, LUA_GCSETPAUSE, std::max(g_gc_pause, 0), tuning.pause_set, tuning.pause_orig, "pause");
    ApplyOne(L, LUA_GCSETSTEPMUL, std::max(g_gc_stepmul, 0), tuning.stepmul_set, tuning.stepmul_orig, "stepmul");
}

#ifdef LUAJIT_GW_FINOBJ
// 35-lua-gc-finobj.md: finobj is always on. CScriptEngine::init switches it on for a new state; this one is a fallback
// (a state made elsewhere, out of memory then) and collects the counters (on the thread that runs Lua, outside the GC)
struct FinobjState
{
    lua_State* L = nullptr;
    int failed = -1; // the mode that failed for L (out of memory): not tried again every frame
    luaJIT_gw_finobj_info seen{}; // the counters at the last look (lua/gc_finobj/* get the growth)
};
FinobjState finobj;

void ApplyFinobj(lua_State* L)
{
    constexpr int want = 1;
    if (finobj.L != L)
    {
        finobj = FinobjState{};
        finobj.L = L;
        luaJIT_gw_finobj_stats(L, &finobj.seen);
    }
    if (luaJIT_gw_finobj(L, -1) != want && finobj.failed != want)
    {
        const int got = luaJIT_gw_finobj(L, want);
        if (got != want)
        {
            finobj.failed = want;
            Msg("! [lua-gc] finobj = %d failed (out of memory), stays %d", want, got < 0 ? 0 : got);
        }
        else
            Msg("* [lua-gc] finobj = %d%s", want, want ? " (the next atomic phase walks all userdata once more)" : "");
    }
    luaJIT_gw_finobj_info now{};
    luaJIT_gw_finobj_stats(L, &now);
    if (now.anchors)
    {
        const auto grow = [](unsigned long long a, unsigned long long b) { return a > b ? a - b : 0ull; };
        PERF_ADD(lua_finobj_moved_head, grow(now.moved_head, finobj.seen.moved_head));
        PERF_ADD(lua_finobj_pending, grow(now.pending, finobj.seen.pending));
        PERF_ADD(lua_finobj_moved_sweep, grow(now.moved_sweep, finobj.seen.moved_sweep));
        PERF_ADD(lua_finobj_moved_full, grow(now.moved_full, finobj.seen.moved_full));
        PERF_ADD(lua_finobj_full_walks, grow(now.full_walks, finobj.seen.full_walks));
        PERF_ADD(lua_finobj_full_mt, grow(now.full_mt, finobj.seen.full_mt));
        PERF_ADD(lua_finobj_kept, grow(now.kept, finobj.seen.kept));
        xray::perf::SetGauge(xray::perf::Metric::lua_finobj_watched, static_cast<s64>(now.watched_now));
    }
    finobj.seen = now;
#ifdef LUAJIT_GW_PREWALK
    // 42-lua-gc-prewalk.md: CScriptEngine::init switches it on; here a state made elsewhere (always on since
    // 2026-10-18, its emergency switch lua_gc_prewalk removed, see plan 41). Needs the anchors of finobj (now.anchors):
    // without them (out of memory above) the next frame.
    constexpr int want_pw = LUAJIT_GW_PREWALK_UDATA | LUAJIT_GW_PREWALK_GRAYAGAIN;
    if (now.anchors && now.pw_flags != want_pw)
    {
        const int got = luaJIT_gw_prewalk(L, want_pw);
        Msg("* [lua-gc] prewalk = %d (from the next GC cycle)", got);
    }
#endif
}
#endif

bool census_requested = false;
bool census_full = false;

#ifdef LUAJIT_GW_GCSTATS
luaJIT_gw_atomic atomic_stats{}; // filled by lj_gc.c atomic() while the hook is set
lua_State* stats_L = nullptr; // the state the hook is set for, nullptr = off
unsigned int stats_seen = 0; // atomic_stats.seq already logged

ull QpcClock() { return CPU::QPC(); }

void UpdateAtomicHook(lua_State* L)
{
    const bool want = g_atomic_stats > 0;
    if (want && stats_L != L)
    {
        luaJIT_gw_gcstats(L, &atomic_stats, &QpcClock);
        stats_L = L;
        stats_seen = atomic_stats.seq;
        Msg("* [lua-gc-atomic] on");
    }
    else if (!want && stats_L)
    {
        luaJIT_gw_gcstats(L, nullptr, nullptr);
        stats_L = nullptr;
        Msg("* [lua-gc-atomic] off");
    }
}

s64 Us(ull ticks) { return static_cast<s64>(QpcMs(ticks) * 1000.0); }
#endif

#ifdef LUAJIT_GW_GCSTATS
// 35-lua-gc-finobj.md: " finobj=..." for the "[lua-gc-atomic]" line (empty without the patch)
xr_string FinobjPart(const luaJIT_gw_atomic& s)
{
#ifdef LUAJIT_GW_FINOBJ
    // finobj=0: all userdata walked (plain=), as upstream. finobj=1: fin= the list with __gc, plain= the rest walked
    // too (full=1 switched on / out of memory, full=2 __gc added to a watched metatable; moved= live ones with __gc
    // found there), pending= set __gc while not the newest one (the sweep moves them), kept= of them dead (finalized a
    // cycle later), mts= metatables without __gc watched
    string256 buf;
    xr_sprintf(buf, " finobj=%d (fin=%llu plain=%llu full=%d moved=%llu pending=%llu kept=%llu mts=%llu)", s.fin_mode,
        static_cast<ull>(s.fin_walked), static_cast<ull>(s.plain_walked), s.fin_full, static_cast<ull>(s.fin_moved),
        static_cast<ull>(s.fin_pending), static_cast<ull>(s.fin_kept), static_cast<ull>(s.fin_mts));
#ifdef LUAJIT_GW_PREWALK
    // 42-lua-gc-prewalk.md, before this phase: state 2 = the userdata with __gc walked in the steps (walked=, of them
    // checked= live ones the phase skipped; tail= the part not reached, walked here), 0 = no prewalk this cycle (off,
    // all walked anyway); ga= grayagain drains / tables drained (the "grayagain" part above is what was left)
    string128 pre;
    xr_sprintf(pre, " pre=%d (walked=%llu checked=%llu tail=%llu ga=%llu/%llu)", s.pw_state,
        static_cast<ull>(s.pw_walked), static_cast<ull>(s.pw_checked), static_cast<ull>(s.pw_tail),
        static_cast<ull>(s.ga_drains), static_cast<ull>(s.ga_drained));
    xr_strcat(buf, pre);
#endif
    return buf;
#else
    (void)s;
    return {};
#endif
}
#endif

void LogAtomicIfNew(bool paced) noexcept
{
#ifdef LUAJIT_GW_GCSTATS
    if (!stats_L || atomic_stats.seq == stats_seen)
        return;
    const luaJIT_gw_atomic& s = atomic_stats;
    const unsigned int missed = s.seq - stats_seen - 1; // more than one since the last look (not logged)
    stats_seen = s.seq;
    const auto U = [](size_t v) { return static_cast<ull>(v); };
    const ull other = s.uv.t + s.left.t + s.t_rest;
    // remark = the weak tables again + the running thread + the GC roots; grayagain = the tables written behind the
    // barrier since they were traversed + all threads (always), with all they reach; separateudata = the walk over ALL
    // userdata (live and dead); mmudata = marking the dead userdata with __gc and what they reach
    Msg("[lua-gc-atomic] #%u total=%.2fms remark=%.2fms (weak=%llu objs=%llu) grayagain=%.2fms (tables=%llu "
        "entries=%llu threads=%llu stack=%llu; traversed objs=%llu tables=%llu entries=%llu KB=%llu) "
        "separateudata=%.2fms (udata=%llu white=%llu with_gc=%llu) mmudata=%.2fms (queued=%llu objs=%llu KB=%llu) "
        "clearweak=%.2fms (weak=%llu entries=%llu cleared=%llu) other=%.2fms (uv=%.2fms open=%llu left=%.2fms "
        "objs=%llu rest=%.2fms) strings=%llu/%llu stack_main=%llu stack_cur=%llu heap=%lluKB est=%lluKB where=%s%s%s",
        s.seq, QpcMs(s.t_total), QpcMs(s.remark.t), U(s.remark_weak), U(s.remark.objs), QpcMs(s.grayagain.t),
        U(s.ga_tabs), U(s.ga_tabslots), U(s.ga_threads), U(s.ga_stackslots), U(s.grayagain.objs),
        U(s.grayagain.tabs), U(s.grayagain.tabslots), U(s.grayagain.bytes >> 10), QpcMs(s.separate.t),
        U(s.separate.objs), U(s.ud_white), U(s.ud_gc), QpcMs(s.mmudata.t), U(s.mm_count), U(s.mmudata.objs),
        U(s.mmudata.bytes >> 10), QpcMs(s.clearweak.t), U(s.clearweak.objs), U(s.clearweak.tabslots),
        U(s.weak_cleared), QpcMs(other), QpcMs(s.uv.t), U(s.uv.objs), QpcMs(s.left.t), U(s.left.objs),
        QpcMs(s.t_rest), U(s.strnum), U(s.strslots), U(s.mainstack), U(s.curstack), U(s.total >> 10),
        U(s.estimate >> 10), paced ? "paced" : "outside", missed ? " (+missed)" : "", FinobjPart(s).c_str());
    using xray::perf::Metric;
    using xray::perf::SetGauge;
    PERF_INC(lua_gca_recorded);
    SetGauge(Metric::lua_gca_total_us, Us(s.t_total));
    SetGauge(Metric::lua_gca_remark_us, Us(s.remark.t));
    SetGauge(Metric::lua_gca_grayagain_us, Us(s.grayagain.t));
    SetGauge(Metric::lua_gca_separate_us, Us(s.separate.t));
    SetGauge(Metric::lua_gca_mmudata_us, Us(s.mmudata.t));
    SetGauge(Metric::lua_gca_clearweak_us, Us(s.clearweak.t));
    SetGauge(Metric::lua_gca_other_us, Us(other));
    SetGauge(Metric::lua_gca_ga_tables, static_cast<s64>(s.ga_tabs));
    SetGauge(Metric::lua_gca_ga_entries, static_cast<s64>(s.ga_tabslots));
    SetGauge(Metric::lua_gca_ga_threads, static_cast<s64>(s.ga_threads));
    SetGauge(Metric::lua_gca_udata, static_cast<s64>(s.separate.objs));
    SetGauge(Metric::lua_gca_udata_gc, static_cast<s64>(s.ud_gc));
    SetGauge(Metric::lua_gca_weak_tables, static_cast<s64>(s.clearweak.objs));
    SetGauge(Metric::lua_gca_weak_entries, static_cast<s64>(s.clearweak.tabslots));
#ifdef LUAJIT_GW_FINOBJ
    SetGauge(Metric::lua_gca_fin_udata, static_cast<s64>(s.fin_walked));
    SetGauge(Metric::lua_gca_plain_udata, static_cast<s64>(s.plain_walked));
    SetGauge(Metric::lua_gca_fin_pending, static_cast<s64>(s.fin_pending));
    if (s.fin_full)
        PERF_INC(lua_gca_fin_full);
#endif
#else
    (void)paced;
#endif
}

#ifdef LUAJIT_GW_GCSTATS
// --- lua_heap_census ---
namespace census
{
using luabind::detail::class_rep;
using luabind::detail::object_rep;

struct Agg
{
    u64 count = 0;
    u64 bytes = 0;
    void Add(u64 b)
    {
        ++count;
        bytes += b;
    }
};

struct BigTable
{
    const void* t;
    u64 bytes;
    u32 asize;
    u32 hslots;
    const void* mt;
};
constexpr size_t top_tables = 30;
constexpr size_t top_classes = 30;
constexpr size_t top_mts = 15;
constexpr size_t top_entries = 5;
// Lua API walks (paths, db.storage) stop after this long each; the walk over the heap itself is not limited
constexpr u64 walk_budget_ms = 250;

// Filled by the walk over the heap (luaJIT_gw_census); the callbacks do not touch the Lua state
struct Ctx
{
    const void* instance_mt = nullptr; // luabind: the metatable of all class instances (object_rep)
    const void* instance_nogc_mt = nullptr; // ... its twin without __gc (instances over a raw pointer)
    const void* cpp_class_mt = nullptr; // ... of the class_rep userdata of C++ classes
    const void* lua_class_mt = nullptr; // ... of Lua classes (class "x" (y))
    xr_unordered_map<const class_rep*, Agg> by_class; // luabind instances by class
    xr_unordered_map<const void*, Agg> ud_by_mt; // other userdata by metatable (nullptr = none)
    Agg ud_finalized; // __gc already run, not swept yet
    xr_vector<const class_rep*> classes; // class_rep userdata met
    xr_unordered_map<const void*, Agg> tab_by_mt; // tables by metatable (nullptr = none)
    xr_vector<BigTable> top; // the largest tables, unordered
    size_t top_min = 0; // the smallest in top
};

void UpdateTopMin(Ctx& x)
{
    x.top_min = 0;
    for (size_t i = 1; i < x.top.size(); ++i)
        if (x.top[i].bytes < x.top[x.top_min].bytes)
            x.top_min = i;
}

void OnUdata(void* c, const void* p, size_t bytes, const void* mt, const void* /*env*/, int finalized)
{
    Ctx& x = *static_cast<Ctx*>(c);
    if (finalized)
    {
        x.ud_finalized.Add(bytes);
        return;
    }
    if (mt && (mt == x.instance_mt || mt == x.instance_nogc_mt))
    {
        // push_new_instance (object_rep.cpp): the payload of an instance is its object_rep
        x.by_class[static_cast<const object_rep*>(p)->crep()].Add(bytes);
        return;
    }
    if (mt && (mt == x.cpp_class_mt || mt == x.lua_class_mt))
        x.classes.push_back(static_cast<const class_rep*>(p));
    x.ud_by_mt[mt].Add(bytes);
}

void OnTable(void* c, const void* t, size_t bytes, unsigned int asize, unsigned int hslots, const void* mt)
{
    Ctx& x = *static_cast<Ctx*>(c);
    x.tab_by_mt[mt].Add(bytes);
    const BigTable big{ t, bytes, asize, hslots, mt };
    if (x.top.size() < top_tables)
    {
        x.top.push_back(big);
        if (x.top.size() == top_tables)
            UpdateTopMin(x);
        return;
    }
    if (bytes <= x.top[x.top_min].bytes)
        return;
    x.top[x.top_min] = big;
    UpdateTopMin(x);
}

using Names = xr_unordered_map<const void*, xr_string>;
using PtrSet = std::unordered_set<const void*>;

// Paths from a root (_G, the registry) to the wanted tables/userdata: table keys, the environment of userdata
// (":env", a luabind instance keeps its fields there), the upvalues of functions ("^name", the locals of a module).
// Lua API only, nothing is called or converted in place.
class PathFinder
{
public:
    PathFinder(lua_State* L, const PtrSet& wanted, Names& found, u64 deadline)
        : m_L(L), m_wanted(wanted), m_found(found), m_deadline(deadline) {}

    // The root on the top of the stack
    void Run(pcstr root, int depth)
    {
        xr_strcpy(m_path, root);
        Visit(depth, xr_strlen(m_path));
    }
    bool TimedOut() const { return m_timeout; }
    u64 Nodes() const { return m_nodes; }

private:
    void Hit(const void* p, size_t len, pcstr suffix)
    {
        if (!p || !m_wanted.count(p) || m_found.count(p))
            return;
        xr_string path(m_path, len);
        path += suffix;
        m_found.emplace(p, std::move(path));
    }

    size_t Append(size_t len, pcstr fmt, pcstr a, pcstr b = "")
    {
        if (len + 2 >= sizeof(m_path))
            return len;
        const int n = std::snprintf(m_path + len, sizeof(m_path) - len, fmt, a, b);
        return n > 0 ? std::min(len + static_cast<size_t>(n), sizeof(m_path) - 1) : len;
    }

    // The key at -2 (lua_next): strings as ".name", the rest in brackets
    size_t AppendKey(size_t len)
    {
        string64 key;
        switch (lua_type(m_L, -2))
        {
        case LUA_TSTRING:
        {
            size_t size = 0;
            const char* s = lua_tolstring(m_L, -2, &size); // a string key: nothing is converted
            std::snprintf(key, sizeof(key), "%.*s", static_cast<int>(std::min<size_t>(size, 48)), s);
            return Append(len, ".%s", key);
        }
        case LUA_TNUMBER: std::snprintf(key, sizeof(key), "%.14g", lua_tonumber(m_L, -2)); break;
        case LUA_TBOOLEAN: xr_strcpy(key, lua_toboolean(m_L, -2) ? "true" : "false"); break;
        default: xr_strcpy(key, lua_typename(m_L, lua_type(m_L, -2))); break;
        }
        return Append(len, "[%s]", key);
    }

    bool Tick()
    {
        if ((++m_nodes & 1023) == 0 && CPU::QPC() > m_deadline)
            m_timeout = true;
        return !m_timeout;
    }

    // The value on the top, its path in m_path[0, len)
    void Visit(int depth, size_t len)
    {
        const int type = lua_type(m_L, -1);
        if (type != LUA_TTABLE && type != LUA_TUSERDATA && type != LUA_TFUNCTION)
            return;
        const void* p = lua_topointer(m_L, -1);
        if (!m_seen.insert(p).second)
            return;
        Hit(p, len, "");
        if (!lua_checkstack(m_L, 4))
            return;
        if (type != LUA_TFUNCTION && lua_getmetatable(m_L, -1))
        {
            Hit(lua_topointer(m_L, -1), len, "<mt>");
            lua_pop(m_L, 1);
        }
        if (depth <= 0 || m_timeout)
            return;
        if (type == LUA_TTABLE)
        {
            lua_pushnil(m_L);
            while (lua_next(m_L, -2))
            {
                if (!Tick())
                {
                    lua_pop(m_L, 2);
                    return;
                }
                Visit(depth - 1, AppendKey(len));
                lua_pop(m_L, 1);
            }
        }
        else if (type == LUA_TUSERDATA)
        {
            lua_getfenv(m_L, -1);
            Visit(depth - 1, Append(len, "%s%s", ":env"));
            lua_pop(m_L, 1);
        }
        else
        {
            for (int i = 1; Tick(); ++i)
            {
                const char* name = lua_getupvalue(m_L, -1, i);
                if (!name)
                    break;
                Visit(depth - 1, Append(len, "^%s%s", *name ? name : "?"));
                lua_pop(m_L, 1);
            }
        }
    }

    lua_State* m_L;
    const PtrSet& m_wanted;
    Names& m_found;
    u64 m_deadline;
    PtrSet m_seen;
    u64 m_nodes = 0;
    bool m_timeout = false;
    char m_path[512]{};
};

// The size of everything reachable from a value (table keys/values/metatables, the own field table of a luabind
// instance), each object once over all calls: the objects seeded before (the modules, the classes) are not counted.
// Functions are counted without their upvalues.
class DeepSizer
{
public:
    DeepSizer(lua_State* L, u64 deadline) : m_L(L), m_deadline(deadline) {}
    void Seed(const void* p) { m_seen.insert(p); }
    // The value on the top
    u64 Size(int depth)
    {
        const int type = lua_type(m_L, -1);
        if (type != LUA_TTABLE && type != LUA_TUSERDATA && type != LUA_TFUNCTION && type != LUA_TSTRING &&
            type != LUA_TTHREAD)
            return 0;
        if (!m_seen.insert(lua_topointer(m_L, -1)).second)
            return 0;
        ++m_objects;
        u64 size = luaJIT_gw_objsize(m_L, -1);
        if (depth <= 0 || m_timeout || !lua_checkstack(m_L, 4))
            return size;
        if (type == LUA_TTABLE)
        {
            if (lua_getmetatable(m_L, -1))
            {
                size += Size(depth - 1);
                lua_pop(m_L, 1);
            }
            lua_pushnil(m_L);
            while (lua_next(m_L, -2))
            {
                if ((++m_nodes & 1023) == 0 && CPU::QPC() > m_deadline)
                    m_timeout = true;
                if (m_timeout)
                {
                    lua_pop(m_L, 2);
                    break;
                }
                size += Size(depth - 1); // the value
                lua_pushvalue(m_L, -2);
                size += Size(depth - 1); // the key
                lua_pop(m_L, 2);
            }
        }
        else if (type == LUA_TUSERDATA)
        {
            // A luabind instance with fields of its own keeps them in its environment, a table whose metatable is
            // the class table (set_instance_value, object_rep.cpp); the class table itself is shared
            lua_getfenv(m_L, -1);
            if (lua_istable(m_L, -1) && lua_getmetatable(m_L, -1))
            {
                lua_pop(m_L, 1);
                size += Size(depth - 1);
            }
            lua_pop(m_L, 1);
        }
        return size;
    }
    bool TimedOut() const { return m_timeout; }
    u64 Objects() const { return m_objects; }

private:
    lua_State* m_L;
    u64 m_deadline;
    PtrSet m_seen;
    u64 m_nodes = 0;
    u64 m_objects = 0;
    bool m_timeout = false;
};

// The pointer of registry[ref], nullptr for none
const void* RegistryRefPointer(lua_State* L, int ref)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    const void* p = lua_istable(L, -1) ? lua_topointer(L, -1) : nullptr;
    lua_pop(L, 1);
    return p;
}

// The class tables of luabind: a table whose metatable is one of them is the own field table of an instance
void NameClassTables(lua_State* L, const Ctx& x, Names& names)
{
    for (const class_rep* crep : x.classes)
    {
        const char* name = crep->name() ? crep->name() : "?";
        crep->get_table(L);
        if (lua_istable(L, -1))
            names.emplace(lua_topointer(L, -1), xr_string("class ") + name);
        lua_pop(L, 1);
        crep->get_default_table(L);
        if (lua_istable(L, -1))
            names.emplace(lua_topointer(L, -1), xr_string("class ") + name + " (default)");
        lua_pop(L, 1);
    }
    if (x.instance_mt)
        names.emplace(x.instance_mt, "luabind instance metatable");
    if (x.instance_nogc_mt)
        names.emplace(x.instance_nogc_mt, "luabind instance metatable without __gc");
    if (x.cpp_class_mt)
        names.emplace(x.cpp_class_mt, "luabind C++ class metatable");
    if (x.lua_class_mt)
        names.emplace(x.lua_class_mt, "luabind Lua class metatable");
}

pcstr NameOf(const Names& names, const void* p, string64& buf)
{
    if (!p)
        return "-";
    const auto it = names.find(p);
    if (it != names.end())
        return it->second.c_str();
    xr_sprintf(buf, "%p", p);
    return buf;
}

template <typename Key>
xr_vector<std::pair<Key, Agg>> TopByBytes(const xr_unordered_map<Key, Agg>& map, size_t n)
{
    xr_vector<std::pair<Key, Agg>> v(map.begin(), map.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
    if (v.size() > n)
        v.resize(n);
    return v;
}

void Run(lua_State* L, bool full_gc)
{
    using namespace probe_detail;
    const auto KB = [](u64 b) { return static_cast<ull>(b >> 10); };
    const u64 t_start = CPU::QPC();
    if (full_gc)
        lua_gc(L, LUA_GCCOLLECT, 0);
    const u64 t_gc = CPU::QPC();

    Ctx x;
    if (const auto* reg = luabind::detail::class_registry::get_registry(L))
    {
        x.instance_mt = RegistryRefPointer(L, reg->cpp_instance());
        // The luabind instances without __gc: the twin is kept in the instance metatable (luabind object_rep.hpp),
        // made on first use
        lua_rawgeti(L, LUA_REGISTRYINDEX, reg->cpp_instance());
        if (lua_istable(L, -1))
        {
            lua_rawgeti(L, -1, luabind::detail::instance_metatable_nogc_twin_key);
            x.instance_nogc_mt = lua_istable(L, -1) ? lua_topointer(L, -1) : nullptr;
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        x.cpp_class_mt = RegistryRefPointer(L, reg->cpp_class());
        x.lua_class_mt = RegistryRefPointer(L, reg->lua_class());
    }
    luaJIT_gw_heap heap{};
    luaJIT_gw_census(L, &heap, &OnUdata, &OnTable, &x);
    const u64 t_walk = CPU::QPC();

    // Names: the luabind class tables, then the paths from _G and the registry to the largest tables and to the
    // metatables of the largest groups
    Names names;
    NameClassTables(L, x, names);
    const auto ud_mts = TopByBytes(x.ud_by_mt, top_mts);
    const auto tab_mts = TopByBytes(x.tab_by_mt, top_mts);
    PtrSet wanted;
    for (const BigTable& t : x.top)
        wanted.insert(t.t);
    for (const auto& [mt, agg] : ud_mts)
        if (mt && !names.count(mt))
            wanted.insert(mt);
    for (const auto& [mt, agg] : tab_mts)
        if (mt && !names.count(mt))
            wanted.insert(mt);
    Names paths;
    PathFinder finder(L, wanted, paths, CPU::QPC() + walk_budget_ms * CPU::qpc_freq / 1000);
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    finder.Run("_G", 5);
    lua_pop(L, 1);
    lua_pushvalue(L, LUA_REGISTRYINDEX);
    finder.Run("REG", 4);
    lua_pop(L, 1);
    for (const auto& [p, path] : paths)
        names.emplace(p, path);
    const u64 t_names = CPU::QPC();

    // db.storage: the entries of the NPCs (and the other online objects with a binder), everything reachable from
    // each, without the modules and the classes
    DeepSizer sizer(L, CPU::QPC() + walk_budget_ms * CPU::qpc_freq / 1000);
    sizer.Seed(lua_topointer(L, LUA_REGISTRYINDEX));
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    sizer.Seed(lua_topointer(L, -1));
    lua_pushnil(L);
    while (lua_next(L, -2)) // the modules, the global functions and the classes
    {
        sizer.Seed(lua_topointer(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    for (const class_rep* crep : x.classes)
    {
        crep->get_table(L);
        sizer.Seed(lua_topointer(L, -1));
        lua_pop(L, 1);
        crep->get_default_table(L);
        sizer.Seed(lua_topointer(L, -1));
        lua_pop(L, 1);
    }
    u64 entries = 0;
    u64 storage_bytes = 0;
    u64 storage_map_bytes = 0;
    xr_vector<std::pair<u64, u32>> entry_sizes; // bytes, id
    // Raw reads: no metamethod of _G or of the module runs here
    lua_pushliteral(L, "db");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1))
    {
        lua_pushliteral(L, "storage");
        lua_rawget(L, -2);
        if (lua_istable(L, -1))
        {
            storage_map_bytes = luaJIT_gw_objsize(L, -1);
            sizer.Seed(lua_topointer(L, -1));
            lua_pushnil(L);
            while (lua_next(L, -2))
            {
                if (sizer.TimedOut())
                {
                    lua_pop(L, 2);
                    break;
                }
                const u64 bytes = sizer.Size(16);
                const u32 id = lua_type(L, -2) == LUA_TNUMBER ? static_cast<u32>(lua_tonumber(L, -2)) : kNoId;
                ++entries;
                storage_bytes += bytes;
                entry_sizes.emplace_back(bytes, id);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    std::sort(entry_sizes.begin(), entry_sizes.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const u64 t_end = CPU::QPC();

    // The log: one block
    static constexpr pcstr state_names[] = { "pause", "propagate", "atomic", "sweepstring", "sweep", "finalize" };
    static constexpr pcstr kind_names[LUAJIT_GW__MAX] = { "string", "upvalue", "thread", "proto", "function",
        "trace", "cdata", "table", "userdata" };
    const pcstr state = heap.gcstate >= 0 && heap.gcstate < static_cast<int>(std::size(state_names)) ?
        state_names[heap.gcstate] : "?";
    Msg("[lua-census] heap=%lluKB gc_state=%s full_gc=%d (%.1fms) walk=%.1fms names=%.1fms (nodes=%llu%s) "
        "storage=%.1fms (objects=%llu%s) total=%.1fms", KB(heap.total), state, full_gc ? 1 : 0,
        QpcMs(t_gc - t_start), QpcMs(t_walk - t_gc), QpcMs(t_names - t_walk), static_cast<ull>(finder.Nodes()),
        finder.TimedOut() ? ", timeout" : "", QpcMs(t_end - t_names), static_cast<ull>(sizer.Objects()),
        sizer.TimedOut() ? ", timeout" : "", QpcMs(t_end - t_start));
    string1024 line;
    line[0] = 0;
    u64 sum = 0;
    for (int k = 0; k < LUAJIT_GW__MAX; ++k)
    {
        string128 part;
        xr_sprintf(part, "%s%s %llu/%lluKB", line[0] ? ", " : "", kind_names[k], static_cast<ull>(heap.count[k]),
            KB(heap.bytes[k]));
        xr_strcat(line, part);
        sum += heap.bytes[k];
    }
    Msg("[lua-census] live: %s | sum=%lluKB; dead not swept %llu/%lluKB; waiting __gc %llu/%lluKB; userdata finalized "
        "%llu/%lluKB", line, KB(sum), static_cast<ull>(heap.dead), KB(heap.dead_bytes),
        static_cast<ull>(heap.finalize), KB(heap.finalize_bytes), static_cast<ull>(x.ud_finalized.count),
        KB(x.ud_finalized.bytes));
#ifdef LUAJIT_GW_FINOBJ
    // 35-lua-gc-finobj.md: the live userdata by list (all "plain" while finobj was never on)
    Msg("[lua-census] userdata lists: finobj=%d with __gc %llu, plain %llu (pending %llu), metatables watched %llu",
        heap.finobj_mode, static_cast<ull>(heap.ud_finobj), static_cast<ull>(heap.ud_plain),
        static_cast<ull>(heap.ud_pending), static_cast<ull>(heap.fin_mts));
#ifdef LUAJIT_GW_PREWALK
    // 42-lua-gc-prewalk.md: of the ones with __gc, those in the checked part (found live by the last prewalk)
    Msg("[lua-census] userdata with __gc checked by the prewalk: %llu", static_cast<ull>(heap.ud_checked));
#endif
#endif

    u64 instances = 0, instance_bytes = 0;
    for (const auto& [crep, agg] : x.by_class)
    {
        instances += agg.count;
        instance_bytes += agg.bytes;
    }
    Msg("[lua-census] luabind instances: %llu/%lluKB in %llu classes; top %llu:", static_cast<ull>(instances),
        KB(instance_bytes), static_cast<ull>(x.by_class.size()), static_cast<ull>(top_classes));
    xr_vector<std::pair<const class_rep*, Agg>> classes(x.by_class.begin(), x.by_class.end());
    std::sort(classes.begin(), classes.end(), [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
    if (classes.size() > top_classes)
        classes.resize(top_classes);
    for (const auto& [crep, agg] : classes)
    {
        // The own field tables of the instances of this class (their metatable is the class table)
        Agg fields;
        if (crep)
        {
            crep->get_table(L);
            const auto it = lua_istable(L, -1) ? x.tab_by_mt.find(lua_topointer(L, -1)) : x.tab_by_mt.end();
            if (it != x.tab_by_mt.end())
                fields = it->second;
            lua_pop(L, 1);
        }
        Msg("[lua-census]   %s: %llu/%lluKB, field tables %llu/%lluKB", crep && crep->name() ? crep->name() : "?",
            static_cast<ull>(agg.count), KB(agg.bytes), static_cast<ull>(fields.count), KB(fields.bytes));
    }

    string64 buf;
    Msg("[lua-census] other userdata by metatable (top %llu):", static_cast<ull>(top_mts));
    for (const auto& [mt, agg] : ud_mts)
        Msg("[lua-census]   %s: %llu/%lluKB", mt ? NameOf(names, mt, buf) : "(none)", static_cast<ull>(agg.count),
            KB(agg.bytes));
    Msg("[lua-census] tables by metatable (top %llu):", static_cast<ull>(top_mts));
    for (const auto& [mt, agg] : tab_mts)
        Msg("[lua-census]   %s: %llu/%lluKB", mt ? NameOf(names, mt, buf) : "(none)", static_cast<ull>(agg.count),
            KB(agg.bytes));

    std::sort(x.top.begin(), x.top.end(), [](const BigTable& a, const BigTable& b) { return a.bytes > b.bytes; });
    Msg("[lua-census] largest tables (top %llu):", static_cast<ull>(top_tables));
    for (const BigTable& t : x.top)
    {
        string64 mt_buf;
        const auto it = names.find(t.t);
        Msg("[lua-census]   %lluKB array=%u hash=%u mt=%s %s", KB(t.bytes), t.asize, t.hslots,
            NameOf(names, t.mt, mt_buf), it != names.end() ? it->second.c_str() : "(path not found)");
    }

    Msg("[lua-census] db.storage: entries=%llu reachable=%lluKB (each object once, without the modules and the "
        "classes) avg=%.1fKB/entry map=%lluKB%s", static_cast<ull>(entries), KB(storage_bytes),
        entries ? static_cast<double>(storage_bytes) / 1024.0 / static_cast<double>(entries) : 0.0,
        KB(storage_map_bytes), sizer.TimedOut() ? " (partial: timeout)" : "");
    for (size_t i = 0; i < entry_sizes.size() && i < top_entries; ++i)
    {
        string128 object;
        DescribeObject(object, entry_sizes[i].second);
        Msg("[lua-census]   %s: %.1fKB", object, static_cast<double>(entry_sizes[i].first) / 1024.0);
    }
}
} // namespace census
#endif
} // namespace gc_diag

void RequestHeapCensus(bool full_gc) noexcept
{
#ifdef LUAJIT_GW_GCSTATS
    gc_diag::census_requested = true;
    gc_diag::census_full = full_gc;
    Msg("* [lua-census] requested%s: runs at the next GC step of a level (CLevel::script_gc)", full_gc ? " (full GC)" : "");
#else
    (void)full_gc;
    Msg("! [lua-census] unavailable: LuaJIT without the GlobalWar patch (luajit.h LUAJIT_GW_GCSTATS)");
#endif
}

void ServiceGcDiagnostics(lua_State* L) noexcept
{
    if (!L)
        return;
    gc_diag::ApplyTuning(L);
#ifdef LUAJIT_GW_FINOBJ
    gc_diag::ApplyFinobj(L);
#endif
#ifdef LUAJIT_GW_GCSTATS
    gc_diag::UpdateAtomicHook(L);
    gc_diag::LogAtomicIfNew(false); // an atomic phase outside the paced steps (implicit steps, other methods)
    if (gc_diag::census_requested)
    {
        gc_diag::census_requested = false;
        gc_diag::census::Run(L, gc_diag::census_full);
    }
#else
    static bool said = false;
    if (g_atomic_stats > 0 && !said)
    {
        said = true;
        Msg("! [lua-gc-atomic] unavailable: LuaJIT without the GlobalWar patch (luajit.h LUAJIT_GW_GCSTATS)");
    }
#endif
}
} // namespace gw::lua_spike
