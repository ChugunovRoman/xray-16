////////////////////////////////////////////////////////////////////////////
//	Module 		: movement_manager.cpp
//	Created 	: 02.10.2001
//  Modified 	: 12.11.2003
//	Author		: Dmitriy Iassenev
//	Description : Movement manager
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "movement_manager.h"
#include "npc_cpp_profile.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "movement_manager_space.h"
#include "game_location_selector.h"
#include "level_location_selector.h"
#include "game_path_manager.h"
#include "level_path_manager.h"
#include "detail_path_manager.h"
#include "patrol_path_manager.h"
#include "xrMessages.h"
#include "xrAICore/Navigation/ai_object_location.h"
#include "CustomMonster.h"
#include "location_manager.h"
#include "level_path_builder.h"
#include "detail_path_builder.h"
#include "xrEngine/profiler.h"
#include "mt_config.h"
#include "xrNetServer/NET_Messages.h"

// Lain: added
#include "steering_behaviour.h"

using namespace MovementManager;

// 33-level-path.md (plans/optimization_mt), temporary console switches (not saved), 0 = as before; read by
// CLevelPathBuilder::process (level_path_builder.h):
// ai_level_path_spike_us N > 0: a [path-spike] line for a level path delegate that took more than N us (4 a second);
// ai_level_path_budget_us N > 0: once the level path delegates of a frame took N us, the next ones go to the next
// frame's seqParallel (the first of a frame always runs); a level A* longer than what is left of N (at least N / 4)
// is put aside and goes on in the next frame (sliced A*, CLevelPathBuilder::process). 0 = both off
int ai_level_path_spike_us = 0;
int ai_level_path_budget_us = 3000; // a tunable, not saved (the default since 2026-10-17, 41-cvar-cleanup.md)

#ifdef USE_FREE_IN_RESTRICTIONS
const float verify_distance = 15.f;
#endif

CMovementManager::CMovementManager(CCustomMonster* object)
    : m_path_actuality(true), m_enabled(true), m_extrapolate_path(true)
{
    VERIFY(object);
    m_object = object;
}

CMovementManager::~CMovementManager()
{
    xr_delete(m_base_game_selector);
    xr_delete(m_base_level_selector);

    xr_delete(m_restricted_object);
    xr_delete(m_location_manager);

    xr_delete(m_game_location_selector);
    xr_delete(m_game_path_manager);
    xr_delete(m_level_path_manager);
    xr_delete(m_detail_path_manager);
    xr_delete(m_patrol_path_manager);

    xr_delete(m_level_path_builder);
    xr_delete(m_detail_path_builder);
}

void CMovementManager::Load(LPCSTR section)
{
    m_restricted_object = create_restricted_object();
    m_location_manager = xr_new<CLocationManager>(m_object);

    m_base_game_selector = xr_new<CGameVertexParams>(locations().vertex_types());
    m_base_level_selector = xr_new<CBaseParameters>();

    m_game_location_selector = xr_new<CGameLocationSelector>(m_restricted_object, m_location_manager);
    m_game_path_manager = xr_new<CGamePathManager>(m_restricted_object);
    m_level_path_manager = xr_new<CLevelPathManager>(m_restricted_object);
    m_detail_path_manager = xr_new<CDetailPathManager>(m_restricted_object);
    m_patrol_path_manager = xr_new<CPatrolPathManager>(m_restricted_object, m_object);

    m_level_path_builder = xr_new<CLevelPathBuilder>(this);
    m_detail_path_builder = xr_new<CDetailPathBuilder>(this);

    extrapolate_path(false);

    m_wait_for_distributed_computation = false;

    locations().Load(section);
}

void CMovementManager::reinit()
{
    m_speed = 0.f;
    m_path_type = ePathTypeNoPath;
    m_path_state = ePathStateDummy;
    m_path_actuality = true;
    m_speed = 0.f;
    m_old_desirable_speed = 0.f;
    m_build_at_once = false;
    m_foreign_game_path_reported = false; // 50y: once per spawn (a pooled object is spawned again)
    level_path_builder().reinit();

    enable_movement(true);
    CGameGraph* game_graph = ai().get_game_graph();
    game_selector().reinit(game_graph);
    detail().reinit();
    game_path().reinit(game_graph);
    level_path().reinit(ai().get_level_graph());
    patrol().reinit();

    game_selector().set_dest_path(game_path().m_path);
}

void CMovementManager::reload(LPCSTR section) { locations().reload(section); }
BOOL CMovementManager::net_Spawn(CSE_Abstract* data) { return (restrictions().net_Spawn(data)); }
void CMovementManager::net_Destroy()
{
    level_path_builder().remove();
    detail_path_builder().remove();
    restrictions().net_Destroy();
}

CMovementManager::EPathType CMovementManager::path_type() const
{
    VERIFY(m_path_type != MovementManager::ePathTypeDummy);
    return (m_path_type);
}

void CMovementManager::set_game_dest_vertex(const GameGraph::_GRAPH_ID& game_vertex_id)
{
    game_path().set_dest_vertex(game_vertex_id);
    m_path_actuality = m_path_actuality && game_path().actual();
}

GameGraph::_GRAPH_ID CMovementManager::game_dest_vertex_id() const
{
    return (GameGraph::_GRAPH_ID(game_path().dest_vertex_id()));
}

void CMovementManager::set_level_dest_vertex(u32 const& level_vertex_id)
{
    VERIFY2(restrictions().accessible(level_vertex_id), object().cName().c_str());
    if (level_path().dest_vertex_id() != level_vertex_id)
        m_level_dest_source = level_dest_engine; // 33-level-path.md, M: the Lua/plugin callers mark theirs after this
    level_path().set_dest_vertex(level_vertex_id);
    m_path_actuality = m_path_actuality && level_path().actual();
}

u32 CMovementManager::level_dest_vertex_id() const { return (level_path().dest_vertex_id()); }
const xr_vector<DetailPathManager::STravelPathPoint>& CMovementManager::path() const { return (detail().path()); }
void CMovementManager::update_path()
{
    if (!enabled() || wait_for_distributed_computation())
        return;

    START_PROFILE("Build Path::update")

    if (!game_path().evaluator())
        game_path().set_evaluator(base_game_params());

    if (!level_path().evaluator())
        level_path().set_evaluator(base_level_params());

#pragma todo("Optimize this in case of slowdown or not intended behaviour")
    if (!restrictions().actual())
    {
        m_path_actuality = false;
    }

    restrictions().actual(true);

    if (!actual())
    {
        game_path().make_inactual();
        level_path().make_inactual();
        patrol().make_inactual();
        switch (m_path_type)
        {
        case ePathTypeGamePath:
        {
            m_path_state = ePathStateSelectGameVertex;
            break;
        }
        case ePathTypeLevelPath:
        {
            m_path_state = ePathStateBuildLevelPath;
            if (!restrictions().accessible(level_path().dest_vertex_id()))
            {
                Fvector temp;
                level_path().set_dest_vertex(restrictions().accessible_nearest(
                    ai().level_graph().vertex_position(level_path().dest_vertex_id()), temp));
                detail().set_dest_position(temp);
            }
            else
            {
                if (!restrictions().accessible(detail().dest_position()))
                {
                    detail().set_dest_position(ai().level_graph().vertex_position(level_path().dest_vertex_id()));
                }
            }
            break;
        }
        case ePathTypePatrolPath:
        {
            //				Msg				("[%6d][%s] actuality is false",Device.dwFrame,*object().cName());
            m_path_state = ePathStateSelectPatrolPoint;
            break;
        }
        case ePathTypeNoPath:
        {
            m_path_state = ePathStateDummy;
            break;
        }
        default: NODEFAULT;
        }
        m_path_actuality = true;
    }

    switch (m_path_type)
    {
    case ePathTypeGamePath:
    {
        process_game_path();
        break;
    }
    case ePathTypeLevelPath:
    {
        process_level_path();
        break;
    }
    case ePathTypePatrolPath:
    {
        process_patrol_path();
        break;
    }
    case ePathTypeNoPath: { break;
    }
    default: NODEFAULT;
    }

#ifdef USE_FREE_IN_RESTRICTIONS
    if (restrictions().accessible(object().Position()))
        verify_detail_path();
#endif // USE_FREE_IN_RESTRICTIONS

    m_build_at_once = false;

    STOP_PROFILE
}

bool CMovementManager::actual_all() const
{
    if (!m_path_actuality)
        return (false);
    switch (m_path_type)
    {
    case ePathTypeGamePath: return (game_path().actual() && level_path().actual() && detail().actual());
    case ePathTypeLevelPath: return (level_path().actual() && detail().actual());
    case ePathTypePatrolPath: return (patrol().actual() && level_path().actual() && detail().actual());
    case ePathTypeNoPath: return (detail().actual());
    default: NODEFAULT;
    }
#ifdef DEBUG
    return (true);
#endif
}

void CMovementManager::teleport(u32 game_vertex_id)
{
    NET_Packet net_packet;
    GameGraph::_GRAPH_ID _game_vertex_id = (GameGraph::_GRAPH_ID)game_vertex_id;
    u32 _level_vertex_id = ai().game_graph().vertex(_game_vertex_id)->level_vertex_id();
    Fvector position = ai().game_graph().vertex(_game_vertex_id)->level_point();
    object().u_EventGen(net_packet, GE_TELEPORT_OBJECT, object().ID());
    net_packet.w(&_game_vertex_id, sizeof(_game_vertex_id));
    net_packet.w(&_level_vertex_id, sizeof(_level_vertex_id));
    net_packet.w_vec3(position);
    Level().Send(net_packet, net_flags(TRUE, TRUE));
}

void CMovementManager::clear_path()
{
    m_detail_path_manager->m_path.clear();
    detail_path_builder().remove();
}

bool CMovementManager::distance_to_destination_greater(const float& distance_to_check) const
{
    if (path().size() < 2)
        return (true);

    if (path_completed())
        return (true);

    float accumulator = 0.f;
    for (u32 i = detail().curr_travel_point_index(), n = detail().path().size() - 1; i < n; ++i)
    {
        accumulator += detail().path()[i].position.distance_to(detail().path()[i + 1].position);
        if (accumulator >= distance_to_check)
            return (true);
    }

    return (false);
}

#ifdef USE_FREE_IN_RESTRICTIONS
void CMovementManager::verify_detail_path()
{
    if (detail().path().empty() || !detail().actual() || detail().completed(detail().dest_position()))
        return;

    if (restrictions().out_restrictions().size())
        return;

    float distance = 0.f;
    for (u32 i = detail().curr_travel_point_index() + 1, n = detail().path().size(); i < n; ++i)
    {
        if (!restrictions().accessible(detail().path()[i].position, EPS_L))
        {
            m_path_actuality = false;
            return;
        }

        distance += detail().path()[i].position.distance_to(detail().path()[i - 1].position);
        if (distance >= verify_distance)
            break;
    }
}
#endif // USE_FREE_IN_RESTRICTIONS

void CMovementManager::on_restrictions_change()
{
    //	Msg								("[%6d][%s][on_restrictions_change]",Device.dwTimeGlobal,*object().cName());
    m_path_actuality = false;
    level_path_builder().remove();
    detail_path_builder().remove();
    level_path().on_restrictions_change();
}

bool CMovementManager::can_use_distributed_computations(u32 option) const
{
    return (!m_build_at_once && g_mt_config.test(option) && !object().getDestroy());
}

void CMovementManager::on_frame(CPHMovementControl* movement_control, Fvector& dest_position)
{
    if (enabled() && (m_path_state != ePathStatePathVerification) && (m_path_state != ePathStatePathCompleted))
    {
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::MovementUpdatePath);
        PERF_SCOPE(mv_update_path); // 28-updatecl-main.md: movement/update_path
        update_path();
    }

    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::MovementMoveAlongPath);
    move_along_path(movement_control, dest_position, object().client_update_fdelta());
}

void CMovementManager::on_travel_point_change(const u32& previous_travel_point_index)
{
    detail().on_travel_point_change(previous_travel_point_index);
}

void CMovementManager::enable_movement(bool enabled)
{
    //	m_path_actuality					= m_path_actuality && (m_enabled == enabled);
    if (!enabled && m_enabled)
        m_on_disable_object_position = object().Position();
    else
    {
        if (enabled && !m_enabled && !object().Position().similar(m_on_disable_object_position))
            m_path_actuality = false;
    }

    m_enabled = enabled;
}

CRestrictedObject* CMovementManager::create_restricted_object() { return (xr_new<CRestrictedObject>(m_object)); }
CMovementManager::CLevelPathManager::PATH& CMovementManager::level_path_path() { return (level_path().m_path); }
void CMovementManager::build_level_path()
{
    //	CTimer								timer;
    //	timer.Start							();
    level_path_builder().process_impl();
    //	static int i=0;
    //	Msg									("[%6d][%6d][%4d][%f]
    // build_level_path",Device.dwTimeGlobal,Device.dwFrame,++i,timer.GetElapsed_sec()*1000.f);
}

// 33-level-path.md (plans/optimization_mt): CLevelPathBuilder's seqParallel delegate and its diagnostics
namespace
{
pcstr level_path_type_name(MovementManager::EPathType type)
{
    switch (type)
    {
    case MovementManager::ePathTypeGamePath: return "game";
    case MovementManager::ePathTypeLevelPath: return "level";
    case MovementManager::ePathTypePatrolPath: return "patrol";
    case MovementManager::ePathTypeNoPath: return "none";
    default: return "?";
    }
}

pcstr level_path_dest_source_name(u8 source)
{
    switch (source)
    {
    case CMovementManager::level_dest_engine: return "engine";
    case CMovementManager::level_dest_lua: return "lua";
    case CMovementManager::level_dest_plugin: return "plugin";
    default: return "?";
    }
}

// item b (ai_level_path_budget_us): the time of the level path delegates of the frame. They run one after another on
// the thread of RunParallelSequence (the Game thread, main when it is off), so plain statics
struct level_path_frame_budget
{
    u32 frame = u32(-1);
    u64 ticks = 0;
    u32 searches = 0;
};
level_path_frame_budget s_level_path_budget;

// [path-spike]: at most level_path_spike_lines lines a second (real time), the rest counted in level_path/spikes_dropped
constexpr u32 level_path_spike_lines = 4;
u32 s_level_path_spike_window = 0;
u32 s_level_path_spike_lines = 0;
u32 s_level_path_spike_dropped = 0;
} // namespace

void CLevelPathBuilder::process()
{
    ZoneScopedN("level_path_build");
    PERF_SCOPE(pseq_level_path); // 22-game-thread.md: a seqParallel delegate (mt_level_path)
    if (Device.dwTimeGlobal < m_last_fail_time + time_to_wait_after_fail)
    {
        PERF_INC(path_builder_delayed);
        return;
    }

    // A second copy of this delegate in the same queue (a planner's clear_path() on the Game thread reset
    // m_wait_for_distributed_computation and update_path() registered the builder again before the queue ran) after
    // this frame queued the copy for the next one (deferred below, or its A* put aside): that copy is enough -
    // remove() takes only one copy out of Device.seqParallel, a second one would run after net_Destroy of the NPC
    if (m_deferred && m_deferred_frame == Device.dwFrame)
        return;

    // item b: once the delegates of this frame took ai_level_path_budget_us, this one goes to the next frame's
    // seqParallel (RunParallelSequence took this frame's queue, so the push lands in the next one). The first delegate
    // of a frame always runs. m_wait_for_distributed_computation stays true meanwhile: the NPC keeps its current path
    // and update_path() does not ask for another build; remove() takes the delegate out of the queue as before, and
    // register_to_process() does not queue a second copy (m_deferred)
    level_path_frame_budget& budget = s_level_path_budget;
    if (budget.frame != Device.dwFrame)
    {
        budget.frame = Device.dwFrame;
        budget.ticks = 0;
        budget.searches = 0;
    }
    const int budget_us = ai_level_path_budget_us;
    const double budget_used_us = xray::perf::TicksToMilliseconds(budget.ticks) * 1000.0;
    if (budget_us > 0 && budget.searches > 0 && budget_used_us >= static_cast<double>(budget_us))
    {
        m_deferred = true;
        m_deferred_frame = Device.dwFrame;
        PERF_INC(level_path_deferred);
        Device.seqParallel.push_back(fastdelegate::FastDelegate0<>(this, &CLevelPathBuilder::process));
        return;
    }
    m_deferred = false;

    PERF_INC(path_builder_done);
    // M: what the build does goes to the trace (the A* of process_impl, the detail paths, the obstacle loop)
    level_path_trace trace;
    level_path_trace*& current = level_path_trace::current();
    level_path_trace* const outer = current;
    current = &trace;
    const u64 search_t0 = xray::perf::Ticks();

    // Sliced A* (33-level-path.md, journal; CLevelGraph::SearchSlice): the A* of this build may stop at the end of
    // the slice - what is left of ai_level_path_budget_us in this frame, at least a quarter of it - and go on in the
    // next frame from where it stopped (the same path, a frame or a few later). The build is then dropped as if it
    // had not run (process_impl, the stalker obstacle loop) and this delegate goes to the next frame's seqParallel,
    // m_wait_for_distributed_computation stays true: the NPC keeps its current path and setup() is not called again,
    // so the next build repeats the same call. ai_level_path_budget_us 0 turns it off with the budget. A search
    // goes on until it ends (64 pops a slice at least; a flood to the 65500 cap is ~1.8 M pops, ~30 frames of 3 ms).
    // When the call keeps changing under it instead (a new destination after clear_path(), a new mask: the search
    // starts again), after level_path_max_restarts such builds in a row the build runs to the end, as before;
    // level_path_max_put_aside is a safety net
    constexpr u32 level_path_max_restarts = 8;
    constexpr u32 level_path_max_put_aside = 600;
    CLevelGraph::SearchSlice slice;
    const bool sliced =
        budget_us > 0 && m_restarts < level_path_max_restarts && m_put_aside < level_path_max_put_aside;
    PERF_ADD_IF(budget_us > 0 && !sliced, level_path_slice_forced, 1);
    if (sliced)
    {
        const double slice_us = std::max(static_cast<double>(budget_us) - budget_used_us, budget_us / 4.0);
        const double us_per_tick = xray::perf::TicksToMilliseconds(1000000) * 1e-3;
        slice.deadline = us_per_tick > 0.0 ? search_t0 + static_cast<u64>(slice_us / us_per_tick) : 0;
        // the restricted object: its level path manager (abstract_path_manager_inline.h) slices only its own searches
        slice.owner = &m_object->restrictions();
        slice.frame = Device.dwFrame;
    }
    CLevelGraph::SearchSlice*& thread_slice = CLevelGraph::search_slice();
    CLevelGraph::SearchSlice* const outer_slice = thread_slice;
    thread_slice = sliced ? &slice : nullptr;

    m_object->build_level_path();

    thread_slice = outer_slice;
    const u64 ticks = xray::perf::Ticks() - search_t0;
    current = outer;

    ++budget.searches;
    budget.ticks += ticks;

    if (slice.put_aside)
    {
        const CLevelGraph::SearchStats& stats = CLevelGraph::last_search_stats();
        if (m_put_aside > 0 && stats.put_aside && stats.slice == 1)
            ++m_restarts; // the last build was put aside too, but this search started anew: its call changed
        ++m_put_aside;
        m_deferred = true;
        m_deferred_frame = Device.dwFrame;
        Device.seqParallel.push_back(fastdelegate::FastDelegate0<>(this, &CLevelPathBuilder::process));
    }
    else
    {
        m_put_aside = 0;
        m_restarts = 0;
    }

    ZoneValue(trace.visited_total);
    ZoneText(m_object->object().cName().c_str(), m_object->object().cName().size());

#if XR_PERF_METRICS
    // 29-game-thread-next.md, B5 (metric only): one search = one call here (level path + its detail path)
    if (xray::perf::g_perf_metrics_active)
        SearchMetrics(ticks);
#endif

    const int spike_us = ai_level_path_spike_us;
    if (spike_us > 0 && xray::perf::TicksToMilliseconds(ticks) * 1000.0 >= static_cast<double>(spike_us))
        report_spike(trace, ticks);
}

void CLevelPathBuilder::trace_search(level_path_trace& trace, u32 search_seq)
{
    const CLevelGraph::SearchStats& stats = CLevelGraph::last_search_stats();
    if (stats.seq == search_seq)
    {
        ++trace.cached_fails;
        return;
    }

    if (trace.search_count < level_path_trace::max_searches)
    {
        level_path_trace::search& search = trace.searches[trace.search_count];
        search.visited = stats.visited;
        search.limit = stats.visited_limit;
        search.ticks = stats.ticks;
        search.slice = stats.slice;
        search.pops = stats.pops;
        search.success = stats.success;
        search.cap_hit = stats.cap_hit;
        search.put_aside = stats.put_aside;
    }
    ++trace.search_count;
    trace.visited_total += stats.visited;
}

// ! [path-spike] <NPC> id=<ID> <ms of the delegate> frame=<N> astar=<A* count> [<each A*: ok|FAIL|ASIDE (sliced A*: put
// aside at the end of its slice), cap = the visited limit was hit, visited/limit, p<heap pops so far>, ms, #k = its
// k-th slice>]
// visited=<sum> cached_fail=<build_path answered by the failed pair cache>
// detail=<ms>/<detail paths> simulate=<ms> iters=<obstacle loop passes> pure=<the search without obstacles was tried>
// iters_capped=<stopped by ai_obstacle_path_iters_max> obstacle_cap=<ai_obstacle_path_visited_cap used, 0 = none>
// start=<vertex> dest=<vertex> dist=<straight line, m> level_path=<vertices> detail_path=<points> ok=<the level path
// was built> type=<game|level|patrol: the path type of the NPC> src=<engine|lua|plugin: who set the level destination,
// meaningful for type=level> fast=<1: the fast A*, always since 2026-10-18>
void CLevelPathBuilder::report_spike(const level_path_trace& trace, u64 ticks) const
{
    PERF_INC(level_path_spikes);
    const u32 now = Device.dwTimeContinual;
    if (now - s_level_path_spike_window >= 1000)
    {
        if (s_level_path_spike_dropped)
            Msg("! [path-spike] %u more over the limit of %u lines a second", s_level_path_spike_dropped,
                level_path_spike_lines);
        s_level_path_spike_window = now;
        s_level_path_spike_lines = 0;
        s_level_path_spike_dropped = 0;
    }
    if (s_level_path_spike_lines >= level_path_spike_lines)
    {
        ++s_level_path_spike_dropped;
        PERF_INC(level_path_spikes_dropped);
        return;
    }
    ++s_level_path_spike_lines;

    string512 searches;
    searches[0] = 0;
    int length = 0;
    const u32 listed = std::min(trace.search_count, level_path_trace::max_searches);
    for (u32 i = 0; i < listed && length >= 0 && length < int(sizeof(searches)); ++i)
    {
        const level_path_trace::search& search = trace.searches[i];
        length += xr_sprintf(searches + length, sizeof(searches) - length, "%s%s%s %u/%u p%u %.2fms", i ? ", " : "",
            search.put_aside ? "ASIDE" : search.success ? "ok" : "FAIL", search.cap_hit ? " cap" : "",
            search.visited, search.limit, search.pops, xray::perf::TicksToMilliseconds(search.ticks));
        if (search.slice > 1 && length >= 0 && length < int(sizeof(searches)))
            length += xr_sprintf(searches + length, sizeof(searches) - length, " #%u", search.slice);
    }
    if (trace.search_count > listed && length >= 0 && length < int(sizeof(searches)))
        xr_sprintf(searches + length, sizeof(searches) - length, ", +%u", trace.search_count - listed);

    const CLevelGraph& graph = ai().level_graph();
    const float distance = graph.vertex_position(m_start_vertex_id).distance_to(graph.vertex_position(m_dest_vertex_id));
    Msg("! [path-spike] %s id=%u %.2fms frame=%u astar=%u [%s] visited=%u cached_fail=%u detail=%.2fms/%u "
        "simulate=%.2fms iters=%u pure=%d iters_capped=%d obstacle_cap=%u start=%u dest=%u dist=%.1fm level_path=%u "
        "detail_path=%u ok=%d type=%s src=%s fast=%d",
        m_object->object().cName().c_str(), static_cast<u32>(m_object->object().ID()),
        xray::perf::TicksToMilliseconds(ticks), Device.dwFrame, trace.search_count, searches, trace.visited_total,
        trace.cached_fails, xray::perf::TicksToMilliseconds(trace.detail_ticks), trace.detail_count,
        xray::perf::TicksToMilliseconds(trace.simulate_ticks), trace.obstacle_iters, trace.pure_fallback ? 1 : 0,
        trace.iters_capped ? 1 : 0, trace.visited_cap, m_start_vertex_id, m_dest_vertex_id, distance,
        static_cast<u32>(m_object->level_path().path().size()), static_cast<u32>(m_object->detail().path().size()),
        m_object->level_path().failed() ? 0 : 1, level_path_type_name(m_object->m_path_type),
        level_path_dest_source_name(m_object->m_level_dest_source), CLevelGraph::last_search_stats().fast ? 1 : 0);
}

Fvector CMovementManager::predict_position(
    const float& time_delta, const Fvector& start_position, u32& current_travel_point, const float& velocity) const
{
    typedef xr_vector<DetailPathManager::STravelPathPoint> PATH;
    const PATH& path = detail().path();
    if (path.empty())
        return (start_position);

    float distance_to_check = velocity * time_delta;

    const u32& path_size = path.size();
    if (current_travel_point == path_size - 1)
        return (path.back().position);

    {
        const Fvector& next = path[current_travel_point + 1].position;
        float distance = start_position.distance_to(next);
        if (distance >= distance_to_check)
        {
            if (next.similar(start_position))
                return (next);

            Fvector result;
            result.sub(next, start_position);
            result.normalize();
            result.mul(distance_to_check);
            result.add(start_position);
            return (result);
        }

        distance_to_check -= distance;
        ++current_travel_point;
    }

    while (current_travel_point < (path_size - 1))
    {
        const Fvector& current = path[current_travel_point].position;
        const Fvector& next = path[current_travel_point + 1].position;
        float distance = current.distance_to(next);
        if (distance > distance_to_check)
            break;

        distance_to_check -= distance;
        ++current_travel_point;
    }

    if (current_travel_point == path_size - 1)
        return (path.back().position);

    const Fvector& current = path[current_travel_point].position;
    const Fvector& next = path[current_travel_point + 1].position;

    VERIFY(current.distance_to(next) > distance_to_check);

    if (next.similar(current))
        return (next);

    Fvector direction = Fvector().sub(next, current);
    direction.normalize();
    direction.mul(distance_to_check);
    direction.add(current);
    return (direction);
}

Fvector CMovementManager::predict_position(const float& time_delta) const
{
    u32 travel_point = detail().m_current_travel_point;
    return (predict_position(time_delta, object().Position(), travel_point, prediction_speed()));
}

const float& CMovementManager::prediction_speed() const { return (old_desirable_speed()); }
Fvector CMovementManager::target_position() const
{
    if (detail().path().empty())
        return (object().Position());

    return (detail().path()[detail().last_patrol_point()].position);
}
