////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_monster_detail_path_manager.h
//	Created 	: 01.11.2005
//  Modified 	: 22.11.2005
//	Author		: Dmitriy Iassenev
//	Description : ALife monster detail path manager class
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "xrAICore/Navigation/game_graph_space.h"
#include "alife_space.h"

class CMovementManagerHolder;
class CALifeSmartTerrainTask;
class CSE_ALifeDynamicObject;

class CALifeMonsterDetailPathManager
{
public:
    typedef CMovementManagerHolder object_type;
    typedef xr_vector<u32> PATH;

private:
    struct parameters
    {
        GameGraph::_GRAPH_ID m_game_vertex_id;
        u32 m_level_vertex_id;
        Fvector m_position;
    };

private:
    object_type* m_object;
    ALife::_TIME_ID m_last_update_time;
    parameters m_destination;
    float m_walked_distance;
    float m_speed;
    /** Last interpolated position for draw_level_position(); used when path is empty/cross-level to avoid spot jumping backward. */
    mutable Fvector m_cached_draw_position;
    mutable bool m_cached_draw_position_valid;

private:
    PATH m_path;
    // this is INVERTED path, i.e.
    // start vertex is the last one
    // destination vertex is the first one.
    // this is useful, since iterating back
    // on this vector during path following
    // we just repeatedly remove the last
    // vertex, and this operation is
    // efficiently implemented in std::vector

private:
    void actualize();
    void setup_current_speed();
    void follow_path(const ALife::_TIME_ID& time_delta);
    void update(const ALife::_TIME_ID& time_delta);

public:
    CALifeMonsterDetailPathManager(object_type* object);
    IC object_type& object() const;

public:
    void target(const GameGraph::_GRAPH_ID& game_vertex_id, const u32& level_vertex_id, const Fvector& position);
    void target(const GameGraph::_GRAPH_ID& game_vertex_id);
    void target(const CALifeSmartTerrainTask& task);
    void target(const CALifeSmartTerrainTask* task);

public:
    void update();
    /** Lightweight: advances position only. Call every frame for smooth map spots. */
    void update_position();
    void on_switch_online();
    void on_switch_offline();
    IC void speed(const float& speed);
    // plans/lua_to_cpp/55-timeskip-squad-movement.md: the movement already counted up to `time` (a squad held at the
    // online radius of the actor during a time skip does not walk the rest of the skip later)
    void catch_up_to(const ALife::_TIME_ID& time);
    // Plan 55: asked by follow_path after every change of the game vertex while set (alife_time_skip.cpp sets it for
    // the steps of a time skip): true - the object stops at this vertex, the rest of the time delta is dropped
    using VertexHook = bool (*)(CSE_ALifeDynamicObject& object);
    static VertexHook s_vertex_hook;

public:
    IC const float& speed() const;
    bool completed() const;
    bool actual() const;
    void make_inactual();
    bool failed() const;
    IC const PATH& path() const;
    IC const float& walked_distance() const;
    Fvector draw_level_position() const;

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

#include "alife_monster_detail_path_manager_inline.h"

// 63-wave3-fixes.md M2: the count of the path searches the budget has deferred (offline_path_search_budget_ms of
// alife.ltx), profiler-independent. alife_time_skip.cpp (UpdateFreshOffline) compares it around a squad update to
// see whether the search actually ran
u32 alife_offline_path_search_deferred_total();
