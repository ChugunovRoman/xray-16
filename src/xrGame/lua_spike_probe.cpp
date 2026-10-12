#include "pch_script.h"
#include "lua_spike_probe.h"

#include "Level.h"
#include "xrEngine/xr_object.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "xrScriptEngine/script_engine.hpp"

#include <algorithm>
#include <iterator>

// P18 (plans/optimization_mt/13-lua-spikes.md). See lua_spike_probe.h.

namespace gw::lua_spike
{
int g_threshold_us = 0;
int g_paced_mul = 100;

namespace probe_detail
{
// The registry key of the sentinel table: the address of this variable (a light userdata, nothing to intern)
char sentinel_key = 0;
u32 atomic_epoch = 0; // atomic phases seen (all Lua states)
u32 open_probes = 0; // probes open right now: the depth of the nested Lua entry points

constexpr pcstr category_names[] = { "enemy_callback", "event_lua", "event_native", "script_call", "binder", "gc_step" };
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
};
PacedState paced;

// A gap without script_gc longer than this (loading, menu, a hitch) is not paid back in one frame
constexpr u32 paced_gap_reset_ms = 1000;
// Safety net between the paced frames: the implicit GC resumes only if the heap grows by half
constexpr int paced_hold_pct = 150;

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
    }
    // What LuaJIT would have stepped implicitly for the allocations since the previous frame (lj_gc_step per KB,
    // lj_gc.c) plus the explicit step of lua_gcstep as before
    const int allocated_kb = std::max(now_kb - paced.last_kb, 0);
    paced.due += static_cast<s64>(allocated_kb) * std::max(g_paced_mul, 1) / 100 + std::max(step_kb, 0);

    const bool probe = Enabled();
    u32 epoch = probe ? PollAtomic(L) : 0;
    u64 steps = 0;
    while (paced.due > 0)
    {
        const u64 t0 = probe ? xray::perf::Ticks() : 0;
        // data 0: exactly one lj_gc_step (more only to pay an old debt of LuaJIT); 1 = the cycle has finished
        const int finished = lua_gc(L, LUA_GCSTEP, 0);
        --paced.due;
        ++steps;
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
            break;
        }
    }
    PERF_ADD(lua_gc_paced_steps, steps);
    paced.last_kb = lua_gc(L, LUA_GCCOUNT, 0);
    paced.last_ms = now_ms;
    HoldImplicitSteps(L);
    paced.holding = true;
}
} // namespace gw::lua_spike
