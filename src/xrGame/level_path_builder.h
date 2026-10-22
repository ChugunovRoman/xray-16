////////////////////////////////////////////////////////////////////////////
//	Module 		: level_path_builder.h
//  Modified 	: 21.02.2005
//  Modified 	: 21.02.2005
//	Author		: Dmitriy Iassenev
//	Description : Level path builder
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "movement_manager.h"
#include "level_path_manager.h"
#include "detail_path_builder.h"
#include "ai_space.h"
#include "xrAICore/Navigation/level_graph.h"
#include "xrCore/Profiling/PerfMetrics.hpp"

class CLevelPathBuilder : public CDetailPathBuilder
{
private:
    typedef CDetailPathBuilder inherited;

private:
    Fvector m_temp;
    u32 m_start_vertex_id;
    u32 m_dest_vertex_id;
    const Fvector* m_precise_position;
    u32 m_last_fail_time;
    bool m_extrapolate_path;
    bool m_use_delay_after_fail;
    // 33-level-path.md, item b (ai_level_path_budget_us): process() put itself into the next frame's seqParallel
    bool m_deferred = false;
    u32 m_deferred_frame = u32(-1); // Device.dwFrame of that push (one copy for the next frame per frame)
    // 33-level-path.md, sliced A*: builds of this builder put aside in a row (the A* did not finish in its slice)
    u32 m_put_aside = 0;
    u32 m_restarts = 0; // of them: the search started anew (its call changed) after a build put aside
    bool m_invalid_vertex_reported = false; // 50y: setup() refused a vertex of another level, reported once

private:
    enum
    {
        time_to_wait_after_fail = u32(2000),
    };

public:
    IC CLevelPathBuilder(CMovementManager* object)
        : inherited(object), m_last_fail_time(0), m_use_delay_after_fail(true)
    {
    }

    IC const u32& dest_vertex_id() const { return (m_dest_vertex_id); }
    IC const u32& start_vertex_id() const { return (m_start_vertex_id); }
    IC void use_delay_after_fail(bool const value) { m_use_delay_after_fail = value; }
    IC void reinit() { m_invalid_vertex_reported = false; } // 50y: CMovementManager::reinit, once per spawn
    // false: a vertex is not on this level graph (it was an R_ASSERT2 "Can't setup path for object=..."), nothing is
    // set up and the caller does not build the path (50y, plans/optimization_mt/50y-invalid-dest-level-vertex.md)
    IC bool setup(
        const u32& start_vertex_id, const u32& dest_vertex_id, bool extrapolate_path, const Fvector* precise_position)
    {
        const CLevelGraph& level_graph = ai().level_graph();
        if (!level_graph.valid_vertex_id(start_vertex_id) || !level_graph.valid_vertex_id(dest_vertex_id))
        {
            // once per object: the caller tries again on its next update
            if (!m_invalid_vertex_reported)
            {
                m_invalid_vertex_reported = true;
                Msg("! [GW] level path of [%s]: start level vertex %u / dest level vertex %u, one is not on this level "
                    "(vertices %u), the path is not built",
                    m_object->object().cName().c_str(), start_vertex_id, dest_vertex_id,
                    level_graph.header().vertex_count());
            }
            return false;
        }
        m_start_vertex_id = start_vertex_id;
        m_dest_vertex_id = dest_vertex_id;

        m_extrapolate_path = extrapolate_path;
        if (!precise_position)
            m_precise_position = 0;
        else
        {
            m_temp = *precise_position;
            m_precise_position = &m_temp;

            if (precise_position->x == flt_max || precise_position->y == flt_max || precise_position->z == flt_max)
                GEnv.ScriptEngine->script_log(LuaMessageType::Error,
                    "! ERROR: object=[%s] start_vertex_id=[%d] dest_vertex_id=[%d] extrapolate_path=[%d] "
                    "precise_position=[%.8f,%.8f,%.8f]",
                    m_object->object().cName().c_str(),
                    start_vertex_id, dest_vertex_id, extrapolate_path, precise_position->x, precise_position->y,
                    precise_position->z);
        }
        return true;
    }

    void register_to_process()
    {
        m_object->m_wait_for_distributed_computation = true;
        if (Device.dwTimeGlobal < m_last_fail_time + time_to_wait_after_fail)
        {
            PERF_INC(path_builder_delayed);
            return;
        }

        // 33-level-path.md, item b: a delegate moved to the next frame is in seqParallel already and builds with what
        // setup() gave it just now - a second copy would build the path twice
        if (m_deferred)
            return;

        PERF_INC(path_builder_queued);
        Device.seqParallel.push_back(fastdelegate::FastDelegate0<>(this, &CLevelPathBuilder::process));
    }

    void process_impl()
    {
        m_object->m_wait_for_distributed_computation = false;
        // 33-level-path.md, M: the A* of this build_path for the [path-spike] line (none when the cache of the last
        // failed start/destination pair answered)
        level_path_trace* const trace = level_path_trace::current();
        const u32 search_seq = trace ? CLevelGraph::last_search_stats().seq : 0;
        m_object->level_path().build_path(m_start_vertex_id, m_dest_vertex_id);
        if (trace)
            trace_search(*trace, search_seq);

        // 33-level-path.md, sliced A*: the A* was put aside on the time slice of process(), the level path is as it
        // was. The build waits for the next frame (process() queues it): the NPC keeps its current path meanwhile
        if (CLevelGraph::search_put_aside())
        {
            m_object->m_wait_for_distributed_computation = true;
            return;
        }

        if (m_object->level_path().failed())
        {
            if (m_use_delay_after_fail)
                m_last_fail_time = Device.dwTimeGlobal;

            m_object->m_path_state = CMovementManager::ePathStateBuildLevelPath;
            return;
        }

        m_object->level_path().select_intermediate_vertex();

        m_object->m_path_state = CMovementManager::ePathStateBuildDetailPath;

        m_object->detail().set_state_patrol_path(m_extrapolate_path);
        m_object->detail().set_start_position(m_object->object().Position());
        m_object->detail().set_start_direction(Fvector().setHP(-m_object->m_body.current.yaw, 0));

        if (m_precise_position)
            m_object->detail().set_dest_position(*m_precise_position);

        inherited::setup(m_object->level_path().path(), m_object->level_path().intermediate_index());
        inherited::process_impl(false);
    }

    // the seqParallel delegate (mt_level_path): the path build, its Tracy zone and metrics, the frame budget (item b)
    // and the [path-spike] line of 33-level-path.md; movement_manager.cpp
    void process();

#if XR_PERF_METRICS
    // B5: level_path/search_max_us - the longest single search of the frame, level_path/searches_max - searches in the
    // frame (both per-frame maxima, UpdateMax). The game logic thread only (seqParallel)
    static void SearchMetrics(u64 ticks)
    {
        static u32 s_frame = u32(-1);
        static s64 s_searches = 0;
        if (s_frame != Device.dwFrame)
        {
            s_frame = Device.dwFrame;
            s_searches = 0;
        }
        ++s_searches;
        xray::perf::UpdateMax(xray::perf::Metric::level_path_searches_max, s_searches);
        xray::perf::UpdateMax(xray::perf::Metric::level_path_search_max_us,
            static_cast<s64>(xray::perf::TicksToMilliseconds(ticks) * 1000.0));
    }
#endif

private:
    // 33-level-path.md, M (movement_manager.cpp): adds the A* that ran since search_seq to the trace; the line
    static void trace_search(level_path_trace& trace, u32 search_seq);
    void report_spike(const level_path_trace& trace, u64 ticks) const;

public:
    IC void remove()
    {
        m_deferred = false;
        m_put_aside = 0;
        m_restarts = 0;
        if (m_object->m_wait_for_distributed_computation)
            m_object->m_wait_for_distributed_computation = false;

        Device.remove_from_seq_parallel(fastdelegate::FastDelegate0<>(this, &CLevelPathBuilder::process));
    }
};
