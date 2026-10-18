////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_schedule_registry.h
//	Created 	: 15.01.2003
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife schedule registry
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "safe_map_iterator.h"
#include "xrServer_Objects_ALife.h"
#include "ai_debug.h"
#include "xrEngine/profiler.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "npc_cpp_profile.h"

class CALifeScheduleRegistry
    : public CSafeMapIterator<ALife::_OBJECT_ID, CSE_ALifeSchedulable, std::less<ALife::_OBJECT_ID>, false>
{
private:
    // -npc_cpp_profile: the update() of an object timed by its kind (alife/scheduled/*), out of line so the casts and
    // their includes stay in alife_schedule_registry.cpp
    static void update_profiled(CSE_ALifeSchedulable& object);

    struct CUpdatePredicate
    {
        u32 m_count;
        mutable u32 m_current;

        IC CUpdatePredicate(const u32& count)
        {
            m_count = count;
            m_current = 0;
        }

        IC bool operator()(_iterator& i, u64 cycle_count, bool) const
        {
            if ((*i).second->m_schedule_counter == cycle_count)
                return (false);

            if (m_current >= m_count)
                return (false);

            ++m_current;
            (*i).second->m_schedule_counter = cycle_count;

            return (true);
        }

        IC void operator()(_iterator& i, u64 cycle_count) const
        {
            START_PROFILE("ALife/scheduled/update")
            if (npc_cpp_profile::enabled())
                update_profiled(*(*i).second);
            else
                (*i).second->update();
            STOP_PROFILE
        }
    };

    // plans/optimization_mt/32-frame-spikes.md, A1: a part of a pass on a frame. As
    // CUpdatePredicate (at most m_count objects, never the same one twice in a call), and besides it stops once
    // CPU::QPC() reaches m_deadline - after the first object: one object always goes, so the pass moves on even when
    // the frame budget was spent before it. m_wrapped: the call got around the whole registry (stopped on an object
    // it had updated already). An object over perf_spike_us goes to the log as "[perf-spike] alife/object" with its
    // name, section and ID (describe_spike).
    struct CUpdateSpreadPredicate : public CUpdatePredicate
    {
        u64 m_deadline;
        mutable bool m_wrapped;

        IC CUpdateSpreadPredicate(const u32& count, u64 deadline) : CUpdatePredicate(count)
        {
            m_deadline = deadline;
            m_wrapped = false;
        }

        IC bool operator()(_iterator& i, u64 cycle_count, bool) const
        {
            if ((*i).second->m_schedule_counter == cycle_count)
            {
                m_wrapped = true;
                return (false);
            }

            if (m_current > 0 && CPU::QPC() >= m_deadline)
                return (false);

            return (CUpdatePredicate::operator()(i, cycle_count, true));
        }

        IC void operator()(_iterator& i, u64 cycle_count) const
        {
            PERF_SPIKE_NAMED(object_spike, "alife/object", static_cast<const void*>((*i).second), &describe_spike, false);
            CUpdatePredicate::operator()(i, cycle_count);
        }
    };

    // The detail of an "alife/object" spike line: ctx is the CSE_ALifeSchedulable updated. Called after its update()
    // returned, still inside the pass: an object released by it is deleted only when the pass ends (destruction_lock).
    static void describe_spike(const void* ctx, char* buf, size_t size) noexcept;

protected:
    typedef CSafeMapIterator<ALife::_OBJECT_ID, CSE_ALifeSchedulable, std::less<ALife::_OBJECT_ID>, false> inherited;

protected:
    u32 m_objects_per_update;

public:
    IC CALifeScheduleRegistry();
    virtual ~CALifeScheduleRegistry();
    void add(CSE_ALifeDynamicObject* object);
    void remove(CSE_ALifeDynamicObject* object, bool no_assert = false);
    IC void update();
    // 32-frame-spikes.md, A1: a part of a pass (CUpdateSpreadPredicate): at most `count` objects, no new one once
    // CPU::QPC() reaches `deadline` (the first always goes). Returns how many were updated; `wrapped` = the registry is
    // empty or the call got around all of it.
    IC u32 update_limited(u32 count, u64 deadline, bool& wrapped);
    IC CSE_ALifeSchedulable* object(const ALife::_OBJECT_ID& id, bool no_assert = false) const;
    IC const u32& objects_per_update() const;
    IC void objects_per_update(const u32& objects_per_update);
};

#include "alife_schedule_registry_inline.h"
