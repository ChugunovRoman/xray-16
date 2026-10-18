////////////////////////////////////////////////////////////////////////////
//	Module 		: detail_path_builder.h
//  Modified 	: 21.02.2005
//  Modified 	: 21.02.2005
//	Author		: Dmitriy Iassenev
//	Description : Detail path builder
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "movement_manager.h"
#include "detail_path_manager.h"
#include "xrCore/Profiling/PerfMetrics.hpp"

// 33-level-path.md (plans/optimization_mt), M: what one CLevelPathBuilder::process (one level path delegate of
// seqParallel) did, for its Tracy zone and the [path-spike] line (ai_level_path_spike_us). process() points current()
// of its thread at one for its length; the code under it adds to it while it is set: CLevelPathBuilder::process_impl
// (each level A*), CDetailPathBuilder::process_impl (the detail paths), the obstacle loop of a stalker
// (stalker_movement_manager_obstacles_path.cpp). Nothing outlives the call.
struct level_path_trace
{
    static constexpr u32 max_searches = 6; // A* listed one by one in the line (the count goes on)
    struct search
    {
        u32 visited;
        u32 limit;
        u64 ticks;
        u32 slice; // sliced A*: the slice of the search this call ran (1 = the first one, or the whole search)
        u32 pops; // heap pops of the search so far (re-expansions included)
        bool success;
        bool cap_hit;
        bool put_aside; // sliced A*: stopped at the end of its slice, goes on in the next frame
    };
    search searches[max_searches];
    u32 search_count = 0; // level A* (CLevelGraph::Search) of the call
    u32 visited_total = 0;
    u32 cached_fails = 0; // build_path answered "failed" from the cache of the last failed pair, without A*
    u64 detail_ticks = 0;
    u32 detail_count = 0;
    u64 simulate_ticks = 0;
    u32 obstacle_iters = 0; // passes of the do..while (!simulate_path_navigation()) loop
    u32 visited_cap = 0; // ai_obstacle_path_visited_cap applied to the searches with obstacles (0 = none)
    bool pure_fallback = false; // the search with obstacles failed, the one without them was tried
    bool iters_capped = false; // the loop was stopped by ai_obstacle_path_iters_max

    static level_path_trace*& current()
    {
        static thread_local level_path_trace* s_current = nullptr;
        return s_current;
    }
};

class CDetailPathBuilder
{
private:
    typedef CMovementManager::CLevelPathManager CLevelPathManager;
    typedef CLevelPathManager::PATH PATH;

protected:
    CMovementManager* m_object;

private:
    const PATH* m_level_path;
    u32 m_path_vertex_index;

public:
    IC CDetailPathBuilder(CMovementManager* object)
    {
        VERIFY(object);
        m_object = object;
    }

    IC void setup(const PATH& level_path, const u32& path_vertex_index)
    {
        m_level_path = &level_path;
        m_path_vertex_index = path_vertex_index;
    }

    void register_to_process()
    {
        m_object->m_wait_for_distributed_computation = true;
        Device.seqParallel.push_back(fastdelegate::FastDelegate0<>(this, &CDetailPathBuilder::process));
    }

    void process_impl(bool separate_computing = true)
    {
        if (separate_computing)
            m_object->m_wait_for_distributed_computation = false;

        {
            // 33-level-path.md, M: level_path/detail_us, and the detail part of a [path-spike] line
            ZoneScopedN("detail_path_build");
            PERF_SCOPE(level_path_detail_us);
            level_path_trace* const trace = level_path_trace::current();
            const u64 t0 = trace ? xray::perf::Ticks() : 0;
            m_object->detail().build_path(*m_level_path, m_path_vertex_index);
            if (trace)
            {
                trace->detail_ticks += xray::perf::Ticks() - t0;
                ++trace->detail_count;
            }
        }

        m_object->on_build_path();

        if (m_object->detail().failed())
            m_object->m_path_state = CMovementManager::ePathStateBuildLevelPath;
        else
            m_object->m_path_state = CMovementManager::ePathStatePathVerification;
    }

    void process()
    {
        PERF_SCOPE(pseq_detail_path); // 22-game-thread.md: a seqParallel delegate (mt_detail_path)
        process_impl();
    }
    IC void remove()
    {
        if (m_object->m_wait_for_distributed_computation)
            m_object->m_wait_for_distributed_computation = false;

        Device.remove_from_seq_parallel(fastdelegate::FastDelegate0<>(this, &CDetailPathBuilder::process));
    }
};
