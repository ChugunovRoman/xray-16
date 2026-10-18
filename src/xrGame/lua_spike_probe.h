#pragma once

// P18 (plans/optimization_mt/13-lua-spikes.md): long Lua calls and the Lua GC.
//
// 1. Probe - a Lua entry point of the engine (the enemy callback of an NPC, the Lua and native subscribers of the event
//    bus, script_call of a plugin, the update of an object binder, the GC step of the frame). A call longer than
//    npc_perf_lua_spike_us goes to the log as one "! [lua-spike]" line: the time, the Lua function (source:line),
//    the IDs, the heap before/after and whether the atomic phase of the GC ran inside the call.
//    The atomic phase is seen through a sentinel: a weak-keyed table in the registry holds one key nothing else
//    refers to. LuaJIT clears the weak tables only in atomic() (lj_gc.c), so the key disappears exactly once per GC
//    cycle; an emptied table counts an atomic phase (lua/gc_atomic) and is armed again. Public Lua API only: the
//    LuaJIT of Externals is the upstream OpenXRay submodule, the project does not patch it.
// 2. Paced GC (always, with lua_gc_method gc_step): the GC works only in CLevel::script_gc, as many single steps as
//    the allocations since the previous frame would have made implicitly (LuaJIT: one step per KB, scaled by
//    lua_gc_paced_mul), and the implicit steps in random Lua calls are held off (threshold = heap * 1.5 - a safety net
//    for the frames without script_gc). The atomic phase then lands in script_gc (seqParallel: the Game thread in a
//    game/render overlap frame) and lua/gc_step shows the whole GC cost. The other methods set the threshold
//    themselves (gc_disable: LUA_GCSTOP, gc_timeout: LJ_MAX_MEM after its step); the paced GC is not involved there.
//
// With npc_perf_lua_spike_us 0 (not saved) a probe costs one load and a branch.

struct lua_State;

namespace gw::lua_spike
{
enum class ECategory : u8
{
    EnemyCallback, // CEnemyManager::useful -> the Lua enemy callback (xr_combat_ignore enemy_callback)
    EventLua, // the event bus -> its Lua subscribers
    EventNative, // the event bus -> the native subscribers (plugins; they call Lua through script_call)
    ScriptCall, // a plugin calls a Lua function by name
    Binder, // CScriptBinder::shedule_Update -> the update of the Lua binder of an object
    GcStep, // CLevel::script_gc - the GC step of the frame
    // 30-game-events.md: a callback of a Lua binder from the engine's event handling (eDeath, eHit, eOnItemDrop)
    BinderCallback,
    // 32-frame-spikes.md: the whole phase level/add_call (level.add_call, set_fastcall, CreateTimeEvent through
    // AddUniqueCall), named by its slowest call (CPHCommander::slowest_call)
    AddCall,
    // 32-frame-spikes.md: one Lua subscriber of a bus dispatch (inside event_lua), named by its source:line
    EventHandler,
    Count
};

extern int g_threshold_us; // npc_perf_lua_spike_us: 0 = off
extern int g_paced_mul; // lua_gc_paced_mul: % of the LuaJIT pace (100 = one step per KB allocated)
// lua_gc_step_budget_us (plans/optimization_mt/26-framemove-game-next.md, K2a; temporary, not saved): > 0 - the paced
// steps of a frame stop after this many us, the rest of the debt goes to the next frames (bounded, see GcStepFrame).
// One step is never cut: the atomic phase of LuaJIT (lj_gc.c atomic()) is one step and stays as long as it is.
// 0 = all steps of the frame at once, as before.
extern int g_step_budget_us;
// G1 (plans/optimization_mt/32-frame-spikes.md; temporary, not saved), with lua_gc_step_budget_us > 0:
// lua_gc_budget_max_us > 0 - the time cap of a frame grows with the debt smoothly instead of the K2a cliff (debt over
// 16K steps -> all of it uncapped in one frame): budget = clamp(base * (1 + debt / lua_gc_debt_soft), base, max).
// Uncapped only when the debt passes 64K steps or the heap grew past lua_gc_heap_guard_pct % of its size at the start
// of the GC cycle (the heap must not run away from the GC). 0 = as before (K2a).
extern int g_budget_max_us;
extern int g_debt_soft; // lua_gc_debt_soft: steps of debt per one more base budget
extern int g_heap_guard_pct; // lua_gc_heap_guard_pct: heap growth (% of the cycle start) that lifts the cap, 0 = off

// What GcStepFrame did in its last call: the detail of the "lua/script_gc" spike line (perf_spike_us)
struct GcFrameStats
{
    u64 steps = 0;
    s64 due_before = 0; // debt before the steps of the frame (after the allocations of the frame were added)
    s64 due_after = 0;
    u32 budget_us = 0; // the time cap of the frame, 0 = none (lua_gc_step_budget_us 0)
    u32 max_step_us = 0; // the longest single step (0 unless perf_metrics, the probe or the spike logger is on)
    int heap_kb = 0; // after the steps
    int cycle_start_kb = 0; // the heap when the current GC cycle started
    s8 atomic = -1; // 1 = the atomic phase ran in the steps of the frame, 0 = not, -1 = not checked
    bool uncapped = false; // the debt was paid off without the time cap
    bool heap_guard = false; // ... because of the heap growth (G1)
};
const GcFrameStats& LastGcFrame() noexcept;

inline bool Enabled() noexcept { return g_threshold_us > 0; }

class Probe
{
public:
    // L - the Lua thread the call runs on (its registry and heap are those of the main state)
    Probe(lua_State* L, ECategory category) noexcept
    {
        if (Enabled() && L)
            Begin(L, category);
    }
    ~Probe() noexcept
    {
        if (m_L)
            Leave();
    }

    // Stops the clock. True when the call took at least npc_perf_lua_spike_us: then the caller describes it by Report()
    bool Finish() noexcept { return m_L ? FinishSlow() : false; }
    // Writes the line and counts the spike. what - the event/function/object name (may be null); npc_id/other_id -
    // u32(-1) = none. function_on_top: a Lua function is on the top of the stack of L, it is named by its debug info
    // (source:line) and popped.
    void Report(pcstr what, u32 npc_id, u32 other_id, bool function_on_top = false) noexcept;

    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;

private:
    void Begin(lua_State* L, ECategory category) noexcept;
    bool FinishSlow() noexcept;
    void Leave() noexcept;

    lua_State* m_L{};
    u64 m_start{}; // CPU::QPC() - the threshold and the log line
    u64 m_ticks{};
    u64 m_perf_start{}; // xray::perf::Ticks() - the perf_metrics timers
    u64 m_perf_ticks{};
    u32 m_epoch{};
    u32 m_epoch_after{};
    int m_kb_before{};
    int m_kb_after{};
    u32 m_depth{};
    ECategory m_category{ECategory::Count};
};

constexpr u32 kNoId = u32(-1);

// Checks the sentinel (see above): returns the number of atomic phases seen so far. An emptied sentinel counts one
// (lua/gc_atomic) and is armed again. Only from the thread that runs Lua now; touches the registry of L only.
u32 PollAtomic(lua_State* L) noexcept;

// The GC step of CLevel::script_gc for the method gc_step (1): the paced steps, step_kb KB on top of the allocations.
// With perf_metrics a single step of at least 2 ms is timed as lua/gc_long_step (in practice the atomic phase).
void GcStepFrame(lua_State* L, int step_kb) noexcept;

// G2 (plans/optimization_mt/32-frame-spikes.md): what the atomic phase of the GC spends its time on and what the heap
// is made of. Needs the local patch of LuaJIT (luajit.h: LUAJIT_GW_GCSTATS); without it they only say so in the log.
// lua_gc_atomic_stats 1 (not saved): one "[lua-gc-atomic]" line after every atomic phase - each part of lj_gc.c atomic()
// timed (the weak tables and roots again, the grayagain list, separateudata, the marks of the dead userdata with __gc,
// clearweak) with what it walked, and the gauges lua/gc_atomic/*. 0 = off: atomic() pays one load and a branch per cycle.
extern int g_atomic_stats;
// A/B, not saved, 0 = untouched: lua_gc_pause - LUA_GCSETPAUSE of the state, and the paced GC (which starts a new cycle
// right after the previous one) waits until the heap passes lua_gc_pause % of the heap at the end of the last cycle
// (100 = no wait, as with 0); lua_gc_stepmul - LUA_GCSETSTEPMUL, the work of one GC step (LuaJIT default 200): more =
// shorter cycles, less garbage at the atomic phase.
extern int g_gc_pause;
extern int g_gc_stepmul;
// 35-lua-gc-finobj.md: LuaJIT keeps the userdata with __gc in a list of their own (luajit.h luaJIT_gw_finobj, a local
// patch; LUAJIT_GW_FINOBJ) and the atomic phase walks only it, not all userdata. Always on: CScriptEngine::init
// switches it on for a new state. With lua_gc_atomic_stats 1 the "[lua-gc-atomic]" line gets "finobj=" (what was
// walked in each list).
// 42-lua-gc-prewalk.md: work moved out of the atomic phase into the incremental steps (luajit.h luaJIT_gw_prewalk;
// LUAJIT_GW_PREWALK): the live userdata with __gc are checked before it (the atomic phase walks only the dead ones and
// the late ones), the tables written behind the barrier (grayagain) are traversed again before it. Always on
// (CScriptEngine::init; its emergency switch lua_gc_prewalk removed 2026-10-18, see plan 41). The "[lua-gc-atomic]"
// line gets "pre=" (what was done before the phase).
// lua_heap_census [full]: the census of the Lua heap ("[lua-census]" lines) at the next CLevel::script_gc - objects by
// type, luabind instances by class, userdata/tables by metatable, the largest tables with their path from _G or the
// registry, the size of db.storage per entry. full: a full GC first (only the live objects; a hitch of up to a second).
void RequestHeapCensus(bool full_gc) noexcept;
// CLevel::script_gc, on the thread that runs Lua, before the GC step: applies lua_gc_pause / lua_gc_stepmul and
// lua_gc_atomic_stats, logs an atomic phase that ran outside the paced steps, runs a requested census
void ServiceGcDiagnostics(lua_State* L) noexcept;
} // namespace gw::lua_spike
