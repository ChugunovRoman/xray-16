////////////////////////////////////////////////////////////////////////////
//	Module 		: movement_manager.h
//	Created 	: 02.10.2001
//  Modified 	: 12.11.2003
//	Author		: Dmitriy Iassenev
//	Description : Movement manager
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "ai_monster_space.h"
#include "xrAICore/Navigation/graph_engine_space.h"
#include "xrAICore/Navigation/game_graph_space.h"
#include "Common/Noncopyable.hpp"

class IGameObject;
class CSE_Abstract;

namespace MovementManager
{
enum EPathType : u32;
};

namespace DetailPathManager
{
enum EDetailPathType : u32;
};

template <typename _Graph, typename _VertexEvaluator, typename _vertex_id_type>
class CBaseLocationSelector;

template <typename _VertexEvaluator, typename _vertex_id_type, typename _index_type>
class CBaseGamePathManager;

template <typename _VertexEvaluator, typename _vertex_id_type, typename _index_type>
class CBaseLevelPathManager;

template <typename _dist_type, typename _index_type, typename _iteration_type>
struct SVertexType;

template <typename _dist_type, typename _index_type, typename _iteration_type>
struct SBaseParameters;

template <typename _dist_type, typename _index_type, typename _iteration_type>
struct SGameVertex;

class CEnemyLocationPredictor;
class CPatrolPathManager;
class CDetailPathManager;
class CPHMovementControl;
class CGameGraph;
class CLevelGraph;
class CRestrictedObject;
class CLocationManager;
class CCustomMonster;

namespace steering_behaviour
{
class manager;
}

namespace DetailPathManager
{
struct STravelPathPoint;
};

class CLevelPathBuilder;
class CDetailPathBuilder;

class CMovementManager
{
private:
    friend class CLevelPathBuilder;
    friend class CDetailPathBuilder;

protected:
    typedef MonsterSpace::SBoneRotation CBoneRotation;
    typedef MovementManager::EPathType EPathType;
    typedef DetailPathManager::STravelPathPoint CTravelPathPoint;
    typedef GraphEngineSpace::CBaseParameters CBaseParameters;
    typedef GraphEngineSpace::CGameVertexParams CGameVertexParams;

    typedef CBaseLocationSelector<CGameGraph, SGameVertex<float, u32, u32>, u32> CGameLocationSelector;

    using CGamePathManager  = CBaseGamePathManager<SGameVertex<float, u32, u32>, u32, u32>;
    using CLevelPathManager = CBaseLevelPathManager<SBaseParameters<float, u32, u32>, u32, u32>;

private:
    enum EPathState
    {
        ePathStateSelectGameVertex = u32(0),
        ePathStateBuildGamePath,
        ePathStateContinueGamePath,

        ePathStateSelectPatrolPoint,

        ePathStateBuildLevelPath,
        ePathStateContinueLevelPath,

        ePathStateBuildDetailPath,

        ePathStatePathVerification,

        ePathStatePathCompleted,

        ePathStateTeleport,

        ePathStateDummy = u32(-1),
    };

protected:
    typedef xr_vector<IGameObject*> NEAREST_OBJECTS;

protected:
    NEAREST_OBJECTS m_nearest_objects;
    // Ф1 (PHYSICS_ACTIVE_SET_PLAN, far-NPC kinematic): hysteresis state for should_force_far_kinematic() -
    // engage beyond ph_npc_kinematic_far_dist, release only once back under (dist - ph_npc_kinematic_far_hyst).
    // Transient, not saved: safely starts false, next tick recomputes correctly from actual distance.
    bool m_kinematic_far_forced = false;
    // Ф3 (plans/optimization_mt/21-physics-step.md, ph_npc_kinematic_unseen): the distance latch of the "far enough and
    // unseen" rule (engage beyond ph_npc_kinematic_unseen_dist, release under it minus a band), and whether the last
    // true of should_force_far_kinematic() came from that rule (physics/npc_kinematic_unseen_forced). Transient.
    bool m_kinematic_unseen_band = false;
    bool m_kinematic_by_unseen = false;
    // Stage 4 of plans/optimization_arch/NPC_UPDATECL_WORKERS_2026-10-08.md (TryPosition coherence, cvars
    // npc_perf_move_try_skip_*): the per-NPC state of try_position_skip(). Transient, not saved: a fresh NPC starts
    // with no budget and no streak, so its first frames with neighbours are full checks.
    u32 m_try_skip_frame = 0; // Device.dwFrame of the last skip decision (a gap = the NPC did not move meanwhile)
    float m_try_skip_dist = 0.f; // path walked since the last TryPosition by the skipped frames
    u8 m_try_skip_left = 0; // frames that may still skip TryPosition before the next full check
    u8 m_try_skip_streak = 0; // successful TryPosition calls in a row (the hysteresis after a failure)
    u8 m_try_skip_k = 0; // K of this NPC as of the last decision (refills m_try_skip_left on a success)
    bool m_try_skip_after_skip = false; // the frame before the next check skipped TryPosition

protected:
    float m_speed;

public:
    CBoneRotation m_body;

protected:
    bool m_path_actuality;

private:
    EPathState m_path_state;
    EPathType m_path_type;
    bool m_enabled;
    Fvector m_on_disable_object_position;
    float m_old_desirable_speed;
    bool m_extrapolate_path;
    bool m_build_at_once;
    bool m_wait_for_distributed_computation;

public:
    CGameVertexParams* m_base_game_selector;
    CBaseParameters* m_base_level_selector;
    CGameLocationSelector* m_game_location_selector;
    CGamePathManager* m_game_path_manager;
    CLevelPathManager* m_level_path_manager;
    CDetailPathManager* m_detail_path_manager;
    CPatrolPathManager* m_patrol_path_manager;
    CRestrictedObject* m_restricted_object;
    CLocationManager* m_location_manager;
    CLevelPathBuilder* m_level_path_builder;
    CDetailPathBuilder* m_detail_path_builder;
    CCustomMonster* m_object;

private:
    void process_game_path();
    void process_level_path();
    void process_patrol_path();
#ifdef USE_FREE_IN_RESTRICTIONS
    void verify_detail_path();
#endif // USE_FREE_IN_RESTRICTIONS
    void apply_collision_hit(CPHMovementControl* movement_control);
    // Ф1 (PHYSICS_ACTIVE_SET_PLAN, far-NPC kinematic): true if this NPC is far enough from the actor (with
    // hysteresis via m_kinematic_far_forced) to skip GetNearest()/TryPosition() entirely this tick and fall
    // straight into move_along_path's existing kinematic (SetPosition+DisableCharacter) branch. See
    // movement_manager_physic.cpp for the full rationale and the bypass conditions.
    bool should_force_far_kinematic(CPHMovementControl* movement_control, const Fvector& dest_position);
    // Ф3 (ph_npc_kinematic_unseen): the second rule of should_force_far_kinematic() - farther than
    // ph_npc_kinematic_unseen_dist from the actor and not drawn in the main pass for a while; updates
    // m_kinematic_unseen_band. dist_sq: squared distance from the destination to the actor.
    bool unseen_kinematic(float dist_sq, const IGameObject* actor);
    // the kinematic branch of move_along_path taken on should_force_far_kinematic(): count it by the rule
    void count_forced_kinematic() const;
    // Stage 4 (TryPosition coherence): true if this frame may move the NPC kinematically instead of paying
    // TryPosition although it has neighbours; try_position_skip_result() feeds the outcome of each real
    // TryPosition back. Called only while npc_perf_move_try_skip_frames or _far is non-zero. See
    // movement_manager_physic.cpp for the rules and the reasoning.
    bool try_position_skip(CPHMovementControl* movement_control, const Fvector& dest_position, float motion);
    void try_position_skip_result(bool success);
    void try_position_skip_reset();

protected:
    virtual void teleport(u32 game_vertex_id);

public:
    CMovementManager(CCustomMonster* object);
    virtual ~CMovementManager();
    virtual void Load(LPCSTR caSection);
    virtual void reinit();
    virtual void reload(LPCSTR caSection);
    virtual BOOL net_Spawn(CSE_Abstract* data);
    virtual void net_Destroy();
    virtual void on_frame(CPHMovementControl* movement_control, Fvector& dest_position);
    IC bool actual() const;
    bool actual_all() const;
    IC void set_path_type(EPathType path_type);
    void set_game_dest_vertex(const GameGraph::_GRAPH_ID& game_vertex_id);
    virtual void set_level_dest_vertex(u32 const& level_vertex_id);
    IC void set_build_path_at_once();
    void enable_movement(bool enabled);
    EPathType path_type() const;
    GameGraph::_GRAPH_ID game_dest_vertex_id() const;
    u32 level_dest_vertex_id() const;
    IC bool enabled() const;
    IC bool path_completed() const;
    IC const float& old_desirable_speed() const;
    IC void set_desirable_speed(float speed);
    const xr_vector<CTravelPathPoint>& path() const;
    IC void set_body_orientation(const MonsterSpace::SBoneRotation& orientation);
    IC const CBoneRotation& body_orientation() const;
    void update_path();
    virtual void move_along_path(CPHMovementControl* movement_control, Fvector& dest_position, float time_delta);

    IC float speed() const;
    float speed(CPHMovementControl* movement_control) const;

    virtual void on_travel_point_change(const u32& previous_travel_point_index);
    virtual void on_build_path() {}
    template <typename T>
    IC bool accessible(T position_or_vertex_id, float radius = EPS_L) const;

    IC void extrapolate_path(bool value);
    IC bool extrapolate_path() const;

    bool distance_to_destination_greater(const float& distance_to_check) const;

    IC bool wait_for_distributed_computation() const;
    virtual bool can_use_distributed_computations(u32 option) const;

    void clear_path();

public:
    IC CGameVertexParams* base_game_params() const;
    IC CBaseParameters* base_level_params() const;
    IC CGameLocationSelector& game_selector() const;
    IC CGamePathManager& game_path() const;
    IC CLevelPathManager& level_path() const;
    IC CDetailPathManager& detail() const;
    IC CPatrolPathManager& patrol() const;
    IC CRestrictedObject& restrictions() const;
    IC CLocationManager& locations() const;
    IC CCustomMonster& object() const;
    IC CLevelPathBuilder& level_path_builder() const;
    IC CDetailPathBuilder& detail_path_builder() const;

public:
    virtual void on_restrictions_change();

protected:
    bool move_along_path() const;

protected:
    Fvector path_position(const float& time_to_check);
    Fvector path_position(const float& velocity, const Fvector& position, const float& time_delta,
        u32& current_travel_point, float& dist, float& dist_to_target, Fvector& dir_to_target);

protected:
    virtual CRestrictedObject* create_restricted_object();
    xr_vector<u32>& level_path_path();

public:
    virtual void build_level_path();

private:
    void show_game_path_info();

public:
    virtual const float& prediction_speed() const;
    Fvector predict_position(const float& time_delta, const Fvector& position, u32& current_travel_point,
        const float& prediction_speed) const;
    Fvector predict_position(const float& time_delta) const;
    Fvector target_position() const;
};

#include "movement_manager_inline.h"
