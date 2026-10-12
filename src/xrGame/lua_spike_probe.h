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
    Count
};

extern int g_threshold_us; // npc_perf_lua_spike_us: 0 = off
extern int g_paced_mul; // lua_gc_paced_mul: % of the LuaJIT pace (100 = one step per KB allocated)

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
void GcStepFrame(lua_State* L, int step_kb) noexcept;
} // namespace gw::lua_spike
