#pragma once

#include "xrCommon/xr_unordered_map.h"
#include "xrCore/Threading/Lock.hpp"

class CPHReqBase;
class CPHReqComparerV;
class CPhysicsShell;

class CPHReqBase
{
public:
    virtual ~CPHReqBase() {}
    virtual bool obsolete() const = 0;
    virtual bool compare(const CPHReqComparerV* v) const { return false; };
    // GAME_RENDER_OVERLAP_PLAN Phase 3: true for any subclass that touches a lua_State (PHScriptCall.h).
    // CPHWorld::AddCall() refuses such calls, because that commander is drained on CPHWorld's dedicated
    // physics thread (PHWorld.cpp Step() -> update_threadsafety()), and invoking a Lua callback off
    // GameThread is a lua_State/LuaJIT-GC thread-safety violation.
    // Level().ph_commander()/ph_commander_scripts() are a *different* CPHCommander instance, always
    // drained on GameThread -- script-wrapping conditions/actions are the correct, intended use there,
    // and this flag must not (and does not) gate those.
    virtual bool touches_script() const { return false; }
    // 32-frame-spikes.md: what this request runs, for a Tracy zone and a spike line of the profiled commander
    // (CPHCommander::set_profiled): "file.script:123" of its Lua function, cached on the first call. Main thread only;
    // nullptr = name it by its class.
    virtual pcstr debug_source() const { return nullptr; }
};

class CPHCondition : public CPHReqBase
{
public:
    virtual bool is_true() = 0;
};

class CPHAction : public CPHReqBase
{
public:
    virtual void run() = 0;
};

class CPHOnesCondition : public CPHCondition
{
    bool b_called;

public:
    CPHOnesCondition() { b_called = false; }
    virtual bool is_true()
    {
        b_called = true;
        return true;
    }
    virtual bool obsolete() const { return b_called; }
};

class CPHDummiAction : public CPHAction
{
public:
    virtual void run() { ; }
    virtual bool obsolete() const { return false; }
};

class XRPHYSICS_API CPHCall
{
    CPHAction* m_action;
    CPHCondition* m_condition;

public:
    CPHCall(CPHCondition* condition, CPHAction* action);
    ~CPHCall();
    void check();
    bool obsolete();
    bool equal(CPHReqComparerV* cmp_condition, CPHReqComparerV* cmp_action);
    bool is_any(CPHReqComparerV* v);
    // 32-frame-spikes.md (CPHCommander::set_profiled): check() timing the condition and the action apart, in
    // xray::perf::Ticks(); timed = false - a plain check(). The tick counts stay as they were for a part that did not run.
    void check_timed(bool timed, u64& cond_ticks, u64& action_ticks);
    bool condition_touches_script() const { return m_condition->touches_script(); }
    bool action_touches_script() const { return m_action->touches_script(); }
    // "cond=<what> act=<what>": debug_source() of each part, its class name otherwise. Main thread only.
    void describe(char* buf, size_t size) const;
#ifdef DEBUG
    const CPHAction* action() const { return m_action; }
    const CPHCondition* condition() const { return m_condition; }
#endif
};

using PHCALL_STORAGE = xr_vector<CPHCall*>;
using PHCALL_I = PHCALL_STORAGE::iterator;

class XRPHYSICS_API CPHCommander
{
    Lock                             lock;
    PHCALL_STORAGE                   m_calls;
    xr_unordered_map<CPHCall*, bool> m_callsUpdateDeferred;

public:
    ~CPHCommander();

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    bool add_call_unique(
        CPHCondition* condition, CPHReqComparerV* cmp_condition, CPHAction* action, CPHReqComparerV* cmp_action);
    void add_call(CPHCondition* condition, CPHAction* action);
    void add_call_threadsafety(CPHCondition* condition, CPHAction* action);
    void AddCallDeferred(CPHCondition* condition, CPHAction* action);

    void remove_call(PHCALL_I i);
    bool has_call(CPHReqComparerV* cmp_condition, CPHReqComparerV* cmp_action);
    PHCALL_I find_call(CPHReqComparerV* cmp_condition, CPHReqComparerV* cmp_action);
    void remove_call(CPHReqComparerV* cmp_condition, CPHReqComparerV* cmp_action);
    void remove_calls(CPHReqComparerV* cmp_object);
    void remove_calls_threadsafety(CPHReqComparerV* cmp_object);
    void RemoveCallsDeferred(CPHReqComparerV* comparer);

    void update();
    void update_threadsafety();

    void clear();

    // 32-frame-spikes.md: per-call diagnostics of update() for the commander of level.add_call and set_fastcall
    // (Level().ph_commander_scripts(), drained on main): a Tracy zone per call named by what it runs, the
    // ph_commander/* metrics, the perf_spike_us site "level/add_call/call" and the slowest call of the last update.
    // Off for the other commanders (the physics thread, Level().ph_commander()): their update() is as before.
    void set_profiled(bool value) { m_profiled = value; }
    // The next update() of a profiled commander also names its slowest call (slowest_call()), e.g. for the
    // "[lua-spike] add_call" line of npc_perf_lua_spike_us. Set before each update().
    void set_describe_slowest(bool value) { m_describe_slowest = value; }
    // The slowest call of the last update() of a profiled commander: "cond=... act=... <us>us", "" when not named
    pcstr slowest_call() const { return m_slowest_call; }

private:
    void UpdateDeferred();
    void UpdateProfiled();

    bool m_profiled{};
    bool m_describe_slowest{};
    string256 m_slowest_call{};
};
