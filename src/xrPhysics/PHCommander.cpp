#include "StdAfx.h"

#include "PHCommander.h"
#include "PHSimpleCalls.h"
#include "xrCore/Profiling/PerfMetrics.hpp"

#include <typeinfo>

#ifdef DEBUG
//#include "PHWorld.h"
#endif

CPHCall::CPHCall(CPHCondition* condition, CPHAction* action)
{
    m_action = action;
    m_condition = condition;
}

CPHCall::~CPHCall()
{
    xr_delete(m_action);
    xr_delete(m_condition);
}

// Crashes in this file appeared after we removed memory pools
// XXX: Recheck if there's memory problems with CPHCommander and CPHCall that were hidden in the original engine
// XXX: Check if checking m_action and m_condition for nullptr is correct and only way to fix crashes

bool CPHCall::obsolete()
{
    return m_action->obsolete() || m_condition->obsolete();
}

void CPHCall::check()
{
    if (m_condition->is_true())
        m_action->run();
}

void CPHCall::check_timed(bool timed, u64& cond_ticks, u64& action_ticks)
{
    if (!timed)
    {
        check();
        return;
    }
    const u64 t0 = xray::perf::Ticks();
    const bool is_true = m_condition->is_true();
    const u64 t1 = xray::perf::Ticks();
    cond_ticks = t1 - t0;
    if (is_true)
    {
        m_action->run();
        action_ticks = xray::perf::Ticks() - t1;
    }
}

namespace
{
pcstr DescribeReq(const CPHReqBase* req)
{
    if (!req)
        return "-";
    if (const pcstr source = req->debug_source())
        return source;
    return xray::perf::SpikeClassName(typeid(*req));
}
} // namespace

void CPHCall::describe(char* buf, size_t size) const
{
    // each part capped: vsprintf_s of a longer line is a fatal invalid-parameter error
    xr_sprintf(buf, size, "cond=%.110s act=%.110s", DescribeReq(m_condition), DescribeReq(m_action));
}

bool CPHCall::equal(CPHReqComparerV* cmp_condition, CPHReqComparerV* cmp_action)
{
    return m_action->compare(cmp_action) && m_condition->compare(cmp_condition);
}

bool CPHCall::is_any(CPHReqComparerV* v)
{
    return m_action->compare(v) || m_condition->compare(v);
}

void delete_call(CPHCall*& call)
{
    try
    {
        xr_delete(call);
    }
    catch (...)
    {
        call = nullptr;
    }
}
/////////////////////////////////////////////////////////////////////////////////
CPHCommander::~CPHCommander() { clear(); }

void CPHCommander::clear()
{
    for (auto& it : m_calls)
        delete_call(it);
    m_calls.clear();

    std::for_each(m_callsUpdateDeferred.begin(), m_callsUpdateDeferred.end(), [](std::pair<CPHCall*, bool> pair)
    {
        if (pair.second)
            delete_call(pair.first);
    });
    m_callsUpdateDeferred.clear();
}

void CPHCommander::UpdateDeferred()
{
    for (auto it : m_callsUpdateDeferred)
    {
        if (it.second)
        {
            m_calls.push_back(it.first);
        }
        else
        {
            auto callToDeleteIt = std::find(m_calls.begin(), m_calls.end(), it.first);
            if (callToDeleteIt != m_calls.end())
            {
                delete_call(*callToDeleteIt);
                m_calls.erase(callToDeleteIt);
            }
        }
    }

    m_callsUpdateDeferred.clear();
}

void CPHCommander::update()
{
    ZoneScoped;

    UpdateDeferred();

    if (m_profiled)
    {
        UpdateProfiled();
        return;
    }

    // One by one, using old style index-based cycle,
    // because when we call m_calls[i]->check()
    // scripts may call add_call or remove_call
    // and iterators will be invalidated
    for (u32 i = 0; i < m_calls.size(); i++)
    {
        try
        {
            m_calls[i]->check();
        }
        catch (...)
        {
            remove_call(m_calls.begin() + i);
            i--;
            continue;
        }

        if (m_calls[i]->obsolete())
        {
            remove_call(m_calls.begin() + i);
            i--;
            continue;
        }
    }
}

#if XR_PERF_METRICS
namespace
{
// The detail of a "[perf-spike] level/add_call/call" line: the description written before the call (the call itself
// may be gone after it: a Lua callback can remove it, an obsolete one is deleted)
void DescribeCallSpike(const void* ctx, char* buf, size_t size) { xr_strcpy(buf, size, static_cast<pcstr>(ctx)); }
} // namespace
#endif

// 32-frame-spikes.md: update() of the commander of level.add_call / set_fastcall (set_profiled). The same calls in the
// same order with the same removal rules as the plain loop of update(); on top of it, per call: a Tracy zone
// "level/add_call/call" (text = cond/act, while Tracy is connected), the perf_spike_us site of the same name, the
// ph_commander/* metrics (perf_metrics) and the slowest call of this update (set_describe_slowest). The description
// (the cached source of the Lua functions, see debug_source) is written only when one of them wants it.
void CPHCommander::UpdateProfiled()
{
#ifdef TRACY_ENABLE
    const bool tracy_text = TracyIsConnected;
#else
    constexpr bool tracy_text = false;
#endif
    const bool describe = m_describe_slowest || xray::perf::SpikeActive() || tracy_text;
    const bool timed = describe || xray::perf::g_perf_metrics_active;
    m_slowest_call[0] = 0;
    u64 slowest_ticks = 0;

    for (u32 i = 0; i < m_calls.size(); i++)
    {
        CPHCall* call = m_calls[i];
        string256 name;
        name[0] = 0;
        if (describe)
            call->describe(name, sizeof(name));
        // read before the call: it may delete itself (level.remove_call from its own Lua callback)
        const bool lua_cond = call->condition_touches_script();
        const bool lua_action = call->action_touches_script();
        u64 cond_ticks = 0;
        u64 action_ticks = 0;
        bool failed = false;
        {
            ZoneScopedN("level/add_call/call");
#ifdef TRACY_ENABLE
            if (tracy_text)
                ZoneText(name, xr_strlen(name));
#endif
            PERF_SPIKE_NAMED(spike, "level/add_call/call", name, &DescribeCallSpike, false);
            try
            {
                call->check_timed(timed, cond_ticks, action_ticks);
            }
            catch (...)
            {
                failed = true;
            }
        }

        if (timed)
        {
            const u64 ticks = cond_ticks + action_ticks;
#if XR_PERF_METRICS
            if (xray::perf::g_perf_metrics_active)
            {
                xray::perf::AddSlow(xray::perf::Metric::ph_commander_calls, 1);
                if (lua_cond)
                    xray::perf::AddTimeSlow(xray::perf::Metric::ph_commander_lua_cond, cond_ticks);
                if (lua_action && action_ticks)
                    xray::perf::AddTimeSlow(xray::perf::Metric::ph_commander_lua_action, action_ticks);
                xray::perf::UpdateMax(xray::perf::Metric::ph_commander_max_us,
                    static_cast<s64>(xray::perf::TicksToMilliseconds(ticks) * 1000.0));
            }
#else
            (void)lua_cond;
            (void)lua_action;
#endif
            if (describe && ticks > slowest_ticks)
            {
                slowest_ticks = ticks;
                xr_sprintf(m_slowest_call, "%.200s %.0fus", name, xray::perf::TicksToMilliseconds(ticks) * 1000.0);
            }
        }

        if (failed)
        {
            remove_call(m_calls.begin() + i);
            i--;
            continue;
        }

        if (m_calls[i]->obsolete())
        {
            remove_call(m_calls.begin() + i);
            i--;
            continue;
        }
    }
}

void CPHCommander::update_threadsafety()
{
    lock.Enter();
    update();
    lock.Leave();
}

void CPHCommander::add_call_threadsafety(CPHCondition* condition, CPHAction* action)
{
    lock.Enter();
    add_call(condition, action);
    lock.Leave();
}

void CPHCommander::add_call(CPHCondition* condition, CPHAction* action)
{
    m_calls.emplace_back(xr_new<CPHCall>(condition, action));
}

void CPHCommander::AddCallDeferred(CPHCondition* condition, CPHAction* action)
{
    m_callsUpdateDeferred.emplace(xr_new<CPHCall>(condition, action), true);
}

void CPHCommander::remove_call(PHCALL_I i)
{
#ifdef DEBUG
    const CPHCallOnStepCondition* esc = smart_cast<const CPHCallOnStepCondition*>((*i)->condition());
    const CPHConstForceAction* cfa = smart_cast<const CPHConstForceAction*>((*i)->action());
    if (esc && cfa)
    {
        Fvector f = cfa->force();
        float m = f.magnitude();
        if (m > EPS_S)
            f.mul(1.f / m);
        //Msg(" const force removed: force: %f,  remove step: %u  world step: %u, dir(%f, %f, %f) ",
        //    m, esc->step(), (u32)ph_world->StepsNum(), VPUSH(f));
    }
#endif
    delete_call(*i);
    m_calls.erase(i);
}

struct SFEqualPred
{
    CPHReqComparerV *cmp_condition, *cmp_action;
    SFEqualPred(CPHReqComparerV* cmp_c, CPHReqComparerV* cmp_a)
    {
        cmp_condition = cmp_c;
        cmp_action = cmp_a;
    }
    bool operator()(CPHCall* call) { return call->equal(cmp_condition, cmp_action); }
};
struct SFRemovePred2
{
    CPHReqComparerV *cmp_condition, *cmp_action;
    SFRemovePred2(CPHReqComparerV* cmp_c, CPHReqComparerV* cmp_a)
    {
        cmp_condition = cmp_c;
        cmp_action = cmp_a;
    }
    bool operator()(CPHCall* call)
    {
        if (call->equal(cmp_condition, cmp_action))
        {
            delete_call(call);
            return true;
        }
        return false;
    }
};

PHCALL_I CPHCommander::find_call(CPHReqComparerV* cmp_condition, CPHReqComparerV* cmp_action)
{
    return std::find_if(m_calls.begin(), m_calls.end(), SFEqualPred(cmp_condition, cmp_action));
}

bool CPHCommander::has_call(CPHReqComparerV* cmp_condition, CPHReqComparerV* cmp_action)
{
    return find_call(cmp_condition, cmp_action) != m_calls.end();
}

void CPHCommander::remove_call(CPHReqComparerV* cmp_condition, CPHReqComparerV* cmp_action)
{
    m_calls.erase(
        std::remove_if(m_calls.begin(), m_calls.end(), SFRemovePred2(cmp_condition, cmp_action)), m_calls.end());
}

bool CPHCommander::add_call_unique(
    CPHCondition* condition, CPHReqComparerV* cmp_condition, CPHAction* action, CPHReqComparerV* cmp_action)
{
    if (m_calls.end() == find_call(cmp_condition, cmp_action))
    {
        add_call(condition, action);
        return true;
    }
    return false;
}

struct SRemovePred1
{
    CPHReqComparerV* cmp_object;
    SRemovePred1(CPHReqComparerV* cmp_o) { cmp_object = cmp_o; }
    bool operator()(CPHCall* call)
    {
        if (call->is_any(cmp_object))
        {
            delete_call(call);
            return true;
        }
        else
            return false;
    }
};

void CPHCommander::remove_calls_threadsafety(CPHReqComparerV* cmp_object)
{
    lock.Enter();
    remove_calls(cmp_object);
    lock.Leave();
}

void CPHCommander::remove_calls(CPHReqComparerV* cmp_object)
{
    m_calls.erase(std::remove_if(m_calls.begin(), m_calls.end(), SRemovePred1(cmp_object)), m_calls.end());
}

void CPHCommander::RemoveCallsDeferred(CPHReqComparerV* comparer)
{
    for (const auto& call : m_calls)
    {
        if (call->is_any(comparer))
        {
            m_callsUpdateDeferred.emplace(call, false);
        }
    }
}
