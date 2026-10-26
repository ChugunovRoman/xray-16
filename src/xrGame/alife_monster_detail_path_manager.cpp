////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_monster_detail_path_manager.cpp
//	Created 	: 01.11.2005
//  Modified 	: 22.11.2005
//	Author		: Dmitriy Iassenev
//	Description : ALife monster detail path manager class
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "alife_monster_detail_path_manager.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_time_manager.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "xrAICore/Navigation/game_graph.h"
#include "xrAICore/Navigation/level_graph.h"
#include "xrAICore/Navigation/game_level_cross_table.h"
#include "alife_smart_terrain_task.h"
#include "alife_graph_registry.h"
#include "xrAICore/Navigation/graph_engine.h"
#include "alife_monster_brain.h"
#include "npc_cpp_profile.h"
#include "alife_time_skip.h"

CALifeMonsterDetailPathManager::VertexHook CALifeMonsterDetailPathManager::s_vertex_hook = nullptr;

namespace
{
// 61-wave3-spec.md, E7: the search budget of one ALife update for the A* of actualize(). After a load every offline
// squad needs a path at once (100+ mks a search; the whole second pass was 28-30 ms); a squad that does not fit the
// budget searches on its next update and stands still meanwhile. The budget is per frame: one ALife update part runs
// per frame (alife_update_manager.cpp spread_frame / shedule_Update). Not applied while a time skip runs (plan 55:
// the catch-up is synchronous). The setting is offline_path_search_budget_ms of configs/alife.ltx, [alife]
// (0 - no budget), read at the first search.
struct OfflinePathSearchBudget
{
    u64 m_frame = u64(-1);
    u64 m_used_qpc = 0;
    u64 m_limit_qpc = 0;
    u32 m_budget_ms = 4;
    bool m_loaded = false;

    void load()
    {
        if (m_loaded)
            return;
        m_loaded = true;
        int value = pSettings->read_if_exists<int>("alife", "offline_path_search_budget_ms", 4);
        if (value < 0)
            value = 0;
        if (value > 1000)
            value = 1000;
        m_budget_ms = (u32)value;
        m_limit_qpc = m_budget_ms ? (u64)m_budget_ms * CPU::qpc_freq / 1000 : 0;
        Msg("* [alife] offline path search budget: %u ms an update (0 - no budget)", m_budget_ms);
    }
    bool active()
    {
        load();
        return (m_budget_ms > 0 && !alife_time_skip::Pending());
    }
    bool allows()
    {
        if (Device.dwFrame != m_frame)
        {
            m_frame = Device.dwFrame;
            m_used_qpc = 0;
        }
        return (m_used_qpc < m_limit_qpc);
    }
    void spend(const u64 qpc) { m_used_qpc += qpc; }
};
OfflinePathSearchBudget g_offline_path_search_budget;
// 63-wave3-fixes.md M2: how many searches the budget has deferred in total (not profiler-gated).
// alife_time_skip.cpp compares it around an update to tell "the search ran" from "the budget put it off"
u32 g_search_deferred_total = 0;
} // namespace

u32 alife_offline_path_search_deferred_total() { return g_search_deferred_total; }

CALifeMonsterDetailPathManager::CALifeMonsterDetailPathManager(object_type* object)
{
    VERIFY(object);
    m_object = object;
    m_last_update_time = 0;
    m_destination.m_game_vertex_id = this->object().get_object().m_tGraphID;
    m_destination.m_level_vertex_id = this->object().get_object().m_tNodeID;
    m_destination.m_position = this->object().get_object().o_Position;
    m_walked_distance = 0.f;
    m_cached_draw_position_valid = false;
}

void CALifeMonsterDetailPathManager::target(
    const GameGraph::_GRAPH_ID& game_vertex_id, const u32& level_vertex_id, const Fvector& position)
{
    m_cached_draw_position_valid = false;
    VERIFY(ai().game_graph().valid_vertex_id(game_vertex_id));
    VERIFY((ai().game_graph().vertex(game_vertex_id)->level_id() != ai().alife().graph().level().level_id()) ||
        ai().level_graph().valid_vertex_id(level_vertex_id));
    VERIFY((ai().game_graph().vertex(game_vertex_id)->level_id() != ai().alife().graph().level().level_id()) ||
        (ai().cross_table().vertex(level_vertex_id).game_vertex_id() == game_vertex_id));
    VERIFY((ai().game_graph().vertex(game_vertex_id)->level_id() != ai().alife().graph().level().level_id()) ||
        ai().level_graph().inside(level_vertex_id, position));

    m_destination.m_game_vertex_id = game_vertex_id;
    m_destination.m_level_vertex_id = level_vertex_id;
    m_destination.m_position = position;

    //	Msg
    //("[%6d][%s][%f][%f][%f]",Device.dwTimeGlobal,object().name_replace(),VPUSH(m_destination.m_position));
}

void CALifeMonsterDetailPathManager::target(const GameGraph::_GRAPH_ID& game_vertex_id)
{
    VERIFY(ai().game_graph().valid_vertex_id(game_vertex_id));
    target(game_vertex_id, ai().game_graph().vertex(game_vertex_id)->level_vertex_id(),
        ai().game_graph().vertex(game_vertex_id)->level_point());
}

void CALifeMonsterDetailPathManager::target(const CALifeSmartTerrainTask& task)
{
    target(task.game_vertex_id(), task.level_vertex_id(), task.position());
}

void CALifeMonsterDetailPathManager::target(const CALifeSmartTerrainTask* task)
{
    //	Msg
    //("[%6d][%s][%s]",Device.dwTimeGlobal,object().name_replace(),*task->patrol_path_name());
    target(*task);
}

bool CALifeMonsterDetailPathManager::completed() const
{
    if (m_destination.m_game_vertex_id != object().get_object().m_tGraphID)
        return (false);

    if (m_destination.m_level_vertex_id != object().get_object().m_tNodeID)
        return (false);

    return (true);
}

bool CALifeMonsterDetailPathManager::actual() const
{
    if (failed())
        return (false);

    if (m_destination.m_game_vertex_id != m_path.front())
        return (false);

    return (true);
}

bool CALifeMonsterDetailPathManager::failed() const { return (m_path.empty()); }
// plans/lua_to_cpp/55-timeskip-squad-movement.md: the offline movement runs on movement_time() - game_time(), or the
// cursor of a running time skip (alife_time_skip.h): the clock jumps at once, the movement catches up step by step
void CALifeMonsterDetailPathManager::update()
{
    const ALife::_TIME_ID current_time = ai().alife().time_manager().movement_time();
    if (current_time <= m_last_update_time)
        return;

    ALife::_TIME_ID time_delta = current_time - m_last_update_time;
    update(time_delta);
    m_last_update_time = current_time;
}

void CALifeMonsterDetailPathManager::update_position()
{
    const CALifeTimeManager& clock = ai().alife().time_manager();
    const ALife::_TIME_ID current_time = clock.movement_time();
    if (path().empty())
    {
        // Plan 55, E2: no path - the object stands (at its destination, or it has no target yet: a squad that has just
        // gone offline waits for its update). The standing time is no walking time: before, it was kept and walked
        // all at once by the first update with a path (a squad back offline after an hour online jumped ~1 km)
        if (m_last_update_time && current_time > m_last_update_time)
            m_last_update_time = current_time;
        return;
    }
    if (!actual())
    {
        // 63-wave3-fixes.md M1: the path is stale - the destination changed and the search is still pending (the
        // budget put it off in update()). The object stands, the same rule the deferred case of update() already
        // has ("a squad that does not fit searches on its next update and stands still meanwhile"): walking the old
        // path to the old target can double the route when the new target is behind, and follow_path() would snap
        // o_Position/m_tNodeID to the NEW destination at the end of the OLD path (its size==1 branch) leaving them
        // against m_tGraphID. The clock accounting is the plan-55 one: standing time is no walking time (the clock
        // moves on, nothing is banked and walked later), so the time is neither lost nor doubled - the stand lasts
        // at most until the next scheduled update searches (~1-1.5 s; one frame with the M2 fix)
        if (m_last_update_time && current_time > m_last_update_time)
            m_last_update_time = current_time;
        return;
    }
    if (!m_last_update_time)
    {
        m_last_update_time = current_time;
        return;
    }
    ALife::_TIME_ID time_delta;
    if (current_time > m_last_update_time)
        time_delta = current_time - m_last_update_time;
    else
    {
        // A time skip moves its cursor itself: nothing to add here (plan 55)
        if (clock.skip_cursor())
            return;
        // Game time didn't advance (e.g. map open, pause). Use real time so position still updates every frame.
        time_delta = (ALife::_TIME_ID)(Device.dwTimeDelta * clock.time_factor());
        if (time_delta == 0)
            return;
    }
    follow_path(time_delta);
    m_last_update_time = current_time;
}

void CALifeMonsterDetailPathManager::catch_up_to(const ALife::_TIME_ID& time)
{
    if (m_last_update_time && m_last_update_time < time)
        m_last_update_time = time;
}

void CALifeMonsterDetailPathManager::make_inactual() { m_path.clear(); }
void CALifeMonsterDetailPathManager::actualize()
{
    m_path.clear();

    typedef GraphEngineSpace::CGameVertexParams CGameVertexParams;
    CGameVertexParams temp = CGameVertexParams(object().m_tpaTerrain);
    bool failed = !ai().graph_engine().search(
        ai().game_graph(), object().get_object().m_tGraphID, m_destination.m_game_vertex_id, &m_path, temp);

#ifdef DEBUG
    if (failed)
    {
        Msg("! %s couldn't build game path from", object().get_object().name_replace());
        {
            const CGameGraph::CGameVertex* vertex = ai().game_graph().vertex(object().get_object().m_tGraphID);
            Msg("! [%d][%s][%f][%f][%f]", object().get_object().m_tGraphID,
                ai().game_graph().header().level(vertex->level_id()).name().c_str(), VPUSH(vertex->level_point()));
            Msg("! game_graph_mask -> [ %d, %d, %d, %d]", vertex->vertex_type()[0], vertex->vertex_type()[1],
                vertex->vertex_type()[2], vertex->vertex_type()[3]);
        }

        {
            const CGameGraph::CGameVertex* vertex = ai().game_graph().vertex(m_destination.m_game_vertex_id);
            Msg("! [%d][%s][%f][%f][%f]", m_destination.m_game_vertex_id,
                ai().game_graph().header().level(vertex->level_id()).name().c_str(), VPUSH(vertex->level_point()));
            Msg("! game_graph_mask -> [ %d, %d, %d, %d]", vertex->vertex_type()[0], vertex->vertex_type()[1],
                vertex->vertex_type()[2], vertex->vertex_type()[3]);
        }
        Msg("! List of available game_graph masks:");
        xr_vector<GameGraph::STerrainPlace>::iterator I = object().m_tpaTerrain.begin();
        xr_vector<GameGraph::STerrainPlace>::iterator E = object().m_tpaTerrain.end();
        for (; I != E; ++I)
        {
            Msg("! [%d , %d , %d , %d]", (*I).tMask[0], (*I).tMask[1], (*I).tMask[2], (*I).tMask[3]);
        };
    }
#endif
    if (failed)
        return;

    VERIFY(!m_path.empty());

    if (m_path.size() == 1)
    {
        VERIFY(m_path.back() == object().get_object().m_tGraphID);
        return;
    }

    m_walked_distance = 0.f;
    std::reverse(m_path.begin(), m_path.end());
    VERIFY(m_path.back() == object().get_object().m_tGraphID);
}

// plans/lua_to_cpp/57-gw-eoc-next-steps.md, E0 (diagnostics only, -npc_cpp_profile): the parts of the update of an
// offline object that walks the game graph (the squads; CALifeMonsterBrain too, but the members of a squad are not
// scheduled). alife/offline_path/first (a counter) - the first update after a load or a spawn: the clock of the path
// starts, nothing is searched or walked; /idle (a counter) - at its destination; /search - the game path search
// (actualize, the A* of the game graph: a new destination, a path lost by the switch online/offline, the second update
// after a load); /search_failed (a counter); /follow - the walk (follow_path of the update; /vertex_change and
// /vertex_hook nested in it)
void CALifeMonsterDetailPathManager::update(const ALife::_TIME_ID& time_delta)
{
    const bool profile = npc_cpp_profile::enabled();
    if (!m_last_update_time)
    {
        if (profile)
            npc_cpp_profile::count(ENpcCppProfileStage::AlifeOfflinePathFirst);
        return;
    }

    if (completed())
    {
        if (profile)
            npc_cpp_profile::count(ENpcCppProfileStage::AlifeOfflinePathIdle);
        return;
    }

    if (!actual())
    {
        // 61-wave3-spec.md, E7: the search budget of the ALife update (offline_path_search_budget_ms of alife.ltx,
        // 0 - no budget): a squad that does not fit searches on its next update and stands still meanwhile
        const bool budgeted = g_offline_path_search_budget.active();
        if (budgeted && !g_offline_path_search_budget.allows())
        {
            // 63-wave3-fixes.md M2: the plain total is what UpdateFreshOffline reads (not profiler-gated)
            ++g_search_deferred_total;
            if (profile)
                npc_cpp_profile::count(ENpcCppProfileStage::AlifeOfflinePathSearchDeferred);
            return;
        }
        const u64 search_start = (profile || budgeted) ? CPU::QPC() : 0;
        actualize();
        const u64 search_qpc = (profile || budgeted) ? CPU::QPC() - search_start : 0;
        if (profile)
            npc_cpp_profile::add(ENpcCppProfileStage::AlifeOfflinePathSearch, search_qpc);
        if (budgeted)
            g_offline_path_search_budget.spend(search_qpc);

        if (failed())
        {
            if (profile)
                npc_cpp_profile::count(ENpcCppProfileStage::AlifeOfflinePathSearchFailed);
            return;
        }
    }

    const u64 follow_start = profile ? CPU::QPC() : 0;
    follow_path(time_delta);
    if (profile)
        npc_cpp_profile::add(ENpcCppProfileStage::AlifeOfflinePathFollow, CPU::QPC() - follow_start);
}

void CALifeMonsterDetailPathManager::setup_current_speed()
{
    if (ai().game_graph().vertex(object().get_object().m_tGraphID)->level_id() == ai().level_graph().level_id())
        speed(object().m_fCurrentLevelGoingSpeed);
    else
        speed(object().m_fGoingSpeed);
}

void CALifeMonsterDetailPathManager::follow_path(const ALife::_TIME_ID& time_delta)
{
    VERIFY(!completed());
    VERIFY(!failed());
    VERIFY(actual());
    VERIFY(!m_path.empty());
    if (m_path.back() != object().get_object().m_tGraphID)
    {
        make_inactual();
    }

    if (m_path.size() == 1)
    {
        VERIFY(object().get_object().m_tGraphID == m_destination.m_game_vertex_id);
        m_walked_distance = 0.f;
        object().get_object().m_tNodeID = m_destination.m_level_vertex_id;
        object().get_object().o_Position = m_destination.m_position;
#ifdef DEBUG
        object().m_fDistanceFromPoint = 0.f;
        object().m_fDistanceToPoint = 0.f;
        object().m_tNextGraphID = object().get_object().m_tGraphID;
#endif
        return;
    }

    float last_time_delta = float(time_delta) / 1000.f;
    for (; m_path.size() > 1;)
    {
        setup_current_speed();
        float update_distance = (last_time_delta / ai().alife().time_manager().normal_time_factor()) * speed();

        float distance_between = ai().game_graph().distance(
            object().get_object().m_tGraphID, (GameGraph::_GRAPH_ID)m_path[m_path.size() - 2]);
        if (distance_between > (update_distance + m_walked_distance))
        {
            m_walked_distance += update_distance;
#ifdef DEBUG
            object().m_fDistanceFromPoint = m_walked_distance;
            object().m_fDistanceToPoint = distance_between;
            object().m_tNextGraphID = (GameGraph::_GRAPH_ID)m_path[m_path.size() - 2];
#endif
            return;
        }

        update_distance += m_walked_distance;
        update_distance -= distance_between;

        last_time_delta = update_distance * ai().alife().time_manager().normal_time_factor() / speed();

        m_walked_distance = 0.f;
        m_path.pop_back();
        //		Msg									("%6d %s changes graph point from %d to
        //%d",Device.dwTimeGlobal,object().name_replace(),object().m_tGraphID,(GameGraph::_GRAPH_ID)m_path.back());
        // 57 E0 (-npc_cpp_profile): alife/offline_path/vertex_change - the move to the next game vertex (the graph
        // registry, on_location_change); /vertex_hook - the rule of a time skip at the vertex (Lua can_switch_online
        // once per squad)
        const bool profile = npc_cpp_profile::enabled();
        const u64 change_start = profile ? CPU::QPC() : 0;
        object().get_object().alife().graph().change(
            &object().get_object(), object().get_object().m_tGraphID, (GameGraph::_GRAPH_ID)m_path.back());
        VERIFY(m_path.back() == object().get_object().m_tGraphID);

        object().on_location_change();
        VERIFY(m_path.back() == object().get_object().m_tGraphID);
        if (profile)
            npc_cpp_profile::add(ENpcCppProfileStage::AlifeOfflinePathVertexChange, CPU::QPC() - change_start);

        // Plan 55: a step of a time skip may stop the object at this vertex (the online radius of the actor)
        if (s_vertex_hook)
        {
            const u64 hook_start = profile ? CPU::QPC() : 0;
            const bool stop = s_vertex_hook(object().get_object());
            if (profile)
                npc_cpp_profile::add(ENpcCppProfileStage::AlifeOfflinePathVertexHook, CPU::QPC() - hook_start);
            if (stop)
                return;
        }
    }
}

void CALifeMonsterDetailPathManager::on_switch_online()
{
    m_path.clear();
    m_cached_draw_position_valid = false;
}
void CALifeMonsterDetailPathManager::on_switch_offline()
{
    m_path.clear();
    m_cached_draw_position_valid = false;
    // Plan 55, E2: the time online is no walking time (the first update with a path walked it all at once)
    if (m_last_update_time)
        m_last_update_time = ai().alife().time_manager().movement_time();
}
Fvector CALifeMonsterDetailPathManager::draw_level_position() const
{
    if (path().empty())
    {
        if (m_cached_draw_position_valid)
            return (m_cached_draw_position);
        return (object().get_object().Position());
    }

    u32 path_size = path().size();
    if (path_size == 1)
    {
        Fvector pos = object().get_object().Position();
        m_cached_draw_position = pos;
        m_cached_draw_position_valid = true;
        return (pos);
    }

    VERIFY(m_path.back() == object().get_object().m_tGraphID);

    const GameGraph::CGameVertex* current = ai().game_graph().vertex(object().get_object().m_tGraphID);
    const GameGraph::CGameVertex* next = ai().game_graph().vertex(m_path[path_size - 2]);
    if (current->level_id() != next->level_id())
    {
        if (m_cached_draw_position_valid)
            return (m_cached_draw_position);
        return (object().get_object().Position());
    }

    Fvector current_vertex = current->level_point();
    Fvector next_vertex = next->level_point();
    Fvector direction = Fvector().sub(next_vertex, current_vertex);
    direction.normalize();
    Fvector result = current_vertex.mad(direction, walked_distance());
    m_cached_draw_position = result;
    m_cached_draw_position_valid = true;
    return (result);
}
