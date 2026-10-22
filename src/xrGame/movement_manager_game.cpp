////////////////////////////////////////////////////////////////////////////
//	Module 		: movement_manager_game.cpp
//	Created 	: 03.12.2003
//  Modified 	: 03.12.2003
//	Author		: Dmitriy Iassenev
//	Description : Movement manager for game paths
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "movement_manager.h"
#include "alife_simulator.h"
#include "alife_graph_registry.h"
#include "alife_level_registry.h"
#include "xrEngine/profiler.h"
#include "game_location_selector.h"
#include "game_path_manager.h"
#include "level_location_selector.h"
#include "level_path_manager.h"
#include "detail_path_manager.h"
#include "xrAICore/Navigation/ai_object_location.h"
#include "xrAICore/Navigation/game_level_cross_table.h"
#include "CustomMonster.h"
#include "level_path_builder.h"
#include "detail_path_builder.h"
#include "mt_config.h"

void CMovementManager::show_game_path_info()
{
    const auto& game_graph = ai().game_graph();

    Msg("! Cannot build GAME path! (object %s)", object().cName().c_str());

    Msg("! CURRENT LEVEL : %s", Level().name().c_str());

    Fvector temp = game_graph.vertex(object().ai_location().game_vertex_id())->level_point();
    Msg("! CURRENT game point position : [%f][%f][%f]", VPUSH(temp));

    const auto game_dst_vertex_id = game_dest_vertex_id();

    if (game_graph.valid_vertex_id(game_dst_vertex_id))
    {
        const GameGraph::CGameVertex* vertex = game_graph.vertex(game_dst_vertex_id);

        Msg("! TARGET LEVEL : %s", game_graph.header().level(vertex->level_id()).name().c_str());

        temp = vertex->level_point();
        Msg("! TARGET  game point position : [%f][%f][%f]", VPUSH(temp));

        const u8* target_vertex_type = vertex->vertex_type();
        Msg("! Target point mask [%d][%d][%d][%d]", target_vertex_type[0], target_vertex_type[1], target_vertex_type[2],
            target_vertex_type[3]);
    }
    else
    {
        Log("! Game destination vertex id is invalid! No info about the target.");
        Log("! TARGET LEVEL :");
        Log("! TARGET  game point position :");
        Log("! TARGET LEVEL :");
        Log("! Target point mask [][][][]");
    }

    Msg("! Object masks (%d) :", m_location_manager->vertex_types().size());

    typedef GameGraph::TERRAIN_VECTOR::const_iterator const_iterator;
    const_iterator I = m_location_manager->vertex_types().begin();
    const_iterator E = m_location_manager->vertex_types().end();
    for (; I != E; ++I)
        Msg("!   [%d][%d][%d][%d]", (*I).tMask[0], (*I).tMask[1], (*I).tMask[2], (*I).tMask[3]);
}

// 50y (plans/optimization_mt/50y-invalid-dest-level-vertex.md): the level path of a game path got a game vertex of
// another level (or one whose level vertex the level graph has not). It was an R_ASSERT2 "Can't setup path ...
// dest_vertex_id" in CLevelPathBuilder::setup. The case seen: a squad member teleported offline through a level
// changer was spawned with its squad on this level with the game vertex of the other one, the game path from it stayed
// there and the level vertex of that vertex is not of this level graph. The game vertex of the object is taken from
// the cross table of this level at its level vertex and the game path is built anew (a vertex of another level on it
// is a teleport then, ePathStateContinueGamePath)
void CMovementManager::reset_foreign_game_path(u32 game_vertex_id, u32 dest_level_vertex_id)
{
    const CGameGraph& game_graph = ai().game_graph();
    const CLevelGraph& level_graph = ai().level_graph();
    const bool foreign = !game_graph.valid_vertex_id(game_vertex_id) ||
        game_graph.vertex(game_vertex_id)->level_id() != level_graph.level_id();

    const GameGraph::_GRAPH_ID object_game_vertex_id = object().ai_location().game_vertex_id();
    GameGraph::_GRAPH_ID own_game_vertex_id = object_game_vertex_id;
    const u32 level_vertex_id = object().ai_location().level_vertex_id();
    if (level_graph.valid_vertex_id(level_vertex_id))
        own_game_vertex_id = ai().cross_table().vertex(level_vertex_id).game_vertex_id();
    const bool corrected = (own_game_vertex_id != object_game_vertex_id) && game_graph.valid_vertex_id(own_game_vertex_id);

    if (!m_foreign_game_path_reported)
    {
        m_foreign_game_path_reported = true;
        Msg("! [GW] game path of [%s]: dest level vertex %u is not on this level (vertices %u), path reset (game vertex "
            "%u of level [%s], the game vertex of the object %u -> %u)",
            object().cName().c_str(), dest_level_vertex_id, level_graph.header().vertex_count(), game_vertex_id,
            game_graph.valid_vertex_id(game_vertex_id) ?
                game_graph.header().level(game_graph.vertex(game_vertex_id)->level_id()).name().c_str() :
                "-",
            u32(object_game_vertex_id), u32(corrected ? own_game_vertex_id : object_game_vertex_id));
    }

    if (corrected)
        object().ai_location().game_vertex(own_game_vertex_id);

    game_path().make_inactual();
    // a vertex of this level that the level graph has not (level.ai and all.spawn of different builds) would come
    // again from the same game path: the NPC stays until its game destination changes
    m_path_state = (corrected || foreign) ? ePathStateBuildGamePath : ePathStatePathCompleted;
}

void CMovementManager::process_game_path()
{
    START_PROFILE("Build Path/Process Game Path");

    if (m_path_state != ePathStateTeleport)
    {
        if (!level_path().actual() && (m_path_state > ePathStateBuildLevelPath))
            m_path_state = ePathStateBuildLevelPath;

        if (!game_path().actual() && (m_path_state > ePathStateBuildGamePath))
            m_path_state = ePathStateBuildGamePath;
    }

    switch (m_path_state)
    {
    case ePathStateSelectGameVertex:
    {
        u32 current = game_path().m_dest_vertex_id;
        game_selector().select_location(object().ai_location().game_vertex_id(), game_path().m_dest_vertex_id);
        if ((current == game_path().m_dest_vertex_id) && game_selector().used())
        {
            m_path_state = ePathStatePathCompleted;
            break;
        }

        if (game_selector().failed())
            break;

        m_path_state = ePathStateBuildGamePath;
        [[fallthrough]];
    }
    case ePathStateBuildGamePath:
    {
        game_path().build_path(object().ai_location().game_vertex_id(), game_dest_vertex_id());

        if (game_path().failed())
        {
            show_game_path_info();
            break;
        }

        m_path_state = ePathStateContinueGamePath;
        [[fallthrough]];
    }
    case ePathStateContinueGamePath:
    {
        game_path().select_intermediate_vertex();
        if (ai().game_graph().vertex(object().ai_location().game_vertex_id())->level_id() !=
            ai().game_graph().vertex(game_path().intermediate_vertex_id())->level_id())
        {
            m_path_state = ePathStateTeleport;
            VERIFY(ai().get_alife());
            VERIFY(ai().alife().graph().level().level_id() ==
                ai().game_graph().vertex(object().ai_location().game_vertex_id())->level_id());
            teleport(game_path().intermediate_vertex_id());
            break;
        }

        m_path_state = ePathStateBuildLevelPath;
        [[fallthrough]];
    }
    case ePathStateBuildLevelPath:
    {
        VERIFY(ai().game_graph().vertex(object().ai_location().game_vertex_id())->level_id() ==
            ai().game_graph().vertex(game_path().intermediate_vertex_id())->level_id());

        // 50y: the intermediate vertex must be of this level and its level vertex on this level graph
        const u32 intermediate_vertex_id = (game_path().intermediate_index() < game_path().path().size()) ?
            game_path().intermediate_vertex_id() :
            u32(-1);
        u32 dest_level_vertex_id = ai().game_graph().valid_vertex_id(intermediate_vertex_id) ?
            ai().game_graph().vertex(intermediate_vertex_id)->level_vertex_id() :
            u32(-1);
        if (!ai().game_graph().valid_vertex_id(intermediate_vertex_id) ||
            (ai().game_graph().vertex(intermediate_vertex_id)->level_id() != ai().level_graph().level_id()) ||
            !ai().level_graph().valid_vertex_id(dest_level_vertex_id))
        {
            reset_foreign_game_path(intermediate_vertex_id, dest_level_vertex_id);
            break;
        }

        if (!accessible(dest_level_vertex_id))
        {
            Fvector dest_pos;
            dest_level_vertex_id =
                restrictions().accessible_nearest(ai().level_graph().vertex_position(dest_level_vertex_id), dest_pos);
        }

        Fvector temp =
            ai().level_graph().vertex_position(dest_level_vertex_id /**level_path().intermediate_vertex_id()**/);
        // 50y: a vertex not on this level graph (accessible_nearest) is reported by setup(), the path is not built
        if (!level_path_builder().setup(object().ai_location().level_vertex_id(), dest_level_vertex_id, true, &temp))
            break;

        if (can_use_distributed_computations(mtLevelPath))
        {
            level_path_builder().register_to_process();
            break;
        }

        build_level_path();

        break;
    }
    case ePathStateContinueLevelPath:
    {
        VERIFY(!level_path().failed());

        level_path().select_intermediate_vertex();

        m_path_state = ePathStateBuildDetailPath;
        [[fallthrough]];
    }
    case ePathStateBuildDetailPath:
    {
        detail().set_state_patrol_path(true);
        detail().set_start_position(object().Position());
        detail().set_start_direction(Fvector().setHP(-m_body.current.yaw, 0));
        detail().set_dest_position(ai().level_graph().vertex_position(level_path().intermediate_vertex_id()));

        detail_path_builder().setup(level_path().path(), level_path().intermediate_index());

        if (can_use_distributed_computations(mtDetailPath))
        {
            detail_path_builder().register_to_process();
            break;
        }

        detail_path_builder().process();

        break;
    }
    case ePathStatePathVerification:
    {
        if (!game_selector().actual(object().ai_location().game_vertex_id(), path_completed()))
            m_path_state = ePathStateSelectGameVertex;
        else if (!game_path().actual())
            m_path_state = ePathStateBuildGamePath;
        else if (!level_path().actual())
            m_path_state = ePathStateBuildLevelPath;
        else if (!detail().actual())
            m_path_state = ePathStateBuildLevelPath;
        else if (detail().completed(object().Position(), !detail().state_patrol_path()))
        {
            m_path_state = ePathStateContinueLevelPath;
            if (level_path().completed())
            {
                m_path_state = ePathStateContinueGamePath;
                if (game_path().completed())
                    m_path_state = ePathStatePathCompleted;
            }
        }
        break;
    }
    case ePathStatePathCompleted:
    {
        if (!game_selector().actual(object().ai_location().game_vertex_id(), path_completed()))
            m_path_state = ePathStateSelectGameVertex;
        break;
    }
    case ePathStateTeleport: { break;
    }
    default: NODEFAULT;
    }

    STOP_PROFILE
}
