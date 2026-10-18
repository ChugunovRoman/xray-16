////////////////////////////////////////////////////////////////////////////
//	Module 		: movement_manager_physic.cpp
//	Created 	: 03.12.2003
//  Modified 	: 03.12.2003
//	Author		: Dmitriy Iassenev
//	Description : Movement manager : physic character movement
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "movement_manager.h"
#include "npc_cpp_profile.h"
#include "entity_alive.h"
#include "PhysicsShellHolder.h"
#include "PHMovementControl.h"
#include "detail_path_manager.h"
#include "Level.h"
#include "CustomMonster.h"
#include "Actor.h"
#include "xrPhysics/IColisiondamageInfo.h"
#include "xrPhysics/PhysicsShell.h"
#include "performance_cvars.h"
#include "memory_manager.h"
#include "enemy_manager.h"

#include "ai_space.h"
#include "xrAICore/Navigation/level_graph.h"
#include "xrPhysics/IElevatorState.h"

#include "xrEngine/profiler.h"
#include "xrCore/Profiling/PerfMetrics.hpp"

#include <atomic>

// Lain: added
#include "steering_behaviour.h"

#ifdef DEBUG
#include "PHDebug.h"

#define DBG_PH_MOVE_CONDITIONS(c) c
#else // DEBUG
#define DBG_PH_MOVE_CONDITIONS(c)
#endif // DEBUG

#define DISTANCE_PHISICS_ENABLE_CHARACTERS 2.f

// Ф1 (PHYSICS_ACTIVE_SET_PLAN, far-NPC kinematic): distance to the actor beyond which a live AI NPC is
// forced into pure kinematic movement (SetPosition+DisableCharacter, see should_force_far_kinematic()
// below), regardless of nearby dynamic physics objects. 0 = feature off (old behavior). Registered in
// console_commands.cpp.
float ph_npc_kinematic_far_dist = 100.f;
float ph_npc_kinematic_far_hyst = 20.f;
// Ф3 (plans/optimization_mt/21-physics-step.md): nearer NPCs that nobody sees -> the same kinematic movement, see
// unseen_kinematic(). Always on; the distance is a cvar.
float ph_npc_kinematic_unseen_dist = 30.f;
// Live NPCs left high in the air (plans/optimization_mt/21-physics-step.md, 2026-10-15): see ground_snap(). Always on;
// the threshold and the speed are saved cvars, registered in console_commands.cpp.
float ai_npc_ground_snap_height = 1.f;
float ai_npc_ground_snap_speed = 10.f;

namespace
{
// How move_along_path() moved the NPC this update (CMovementManager::m_ground_prev_branch, the "! [npc_ground]" lines).
enum ground_branch : u8
{
    gb_none, // returned before moving it (no character, no time, the travel point reached)
    gb_idle, // not on a path, the character disabled: left where it is
    gb_idle_physics, // not on a path, the character enabled: Calculate(), the frame step
    gb_idle_kinematic, // not on a path, the character disabled by the far / unseen rule
    gb_kinematic_forced, // on a path, SetPosition by the far / unseen rule (no neighbour query)
    gb_kinematic_free, // on a path, SetPosition: no physical neighbour within reach
    gb_try_skip, // on a path, SetPosition: TryPosition skipped (stage 4 budget)
    gb_try_ok, // on a path, a successful TryPosition (local steps without gravity)
    gb_physics, // on a path, TryPosition failed: Calculate(), the frame step
    gb_count
};

const char* const ground_branch_names[gb_count] = {"none", "idle", "idle_physics", "idle_kinematic_forced",
    "kinematic_forced", "kinematic_no_neighbours", "try_position_skipped", "try_position_ok", "physics"};

// m_ground_air_updates / lift metrics: 0 kinematic (SetPosition), 1 TryPosition, 2 physics, 3 other
u8 ground_branch_kind(u8 branch)
{
    switch (branch)
    {
    case gb_idle_kinematic:
    case gb_kinematic_forced:
    case gb_kinematic_free:
    case gb_try_skip: return 0;
    case gb_try_ok: return 1;
    case gb_idle_physics:
    case gb_physics: return 2;
    default: return 3;
    }
}

// The diagnostics: "high" above the floor, the release of that latch, "stands on the floor" (resets the air counts)
constexpr float ground_high = 3.f;
constexpr float ground_high_release = 2.f;
constexpr float ground_on_floor = 0.5f;
// ground_snap(): the lowering goes on until the NPC is this close to the floor
constexpr float ground_snap_done = 0.05f;
// the first lifts of a session that go to the log
constexpr u32 ground_log_max = 16;
std::atomic<u32> s_ground_logged{0};
} // namespace

float CMovementManager::speed(CPHMovementControl* movement_control) const
{
    VERIFY(movement_control);
    if (fis_zero(m_speed))
        return (0.f);

    if (movement_control->IsCharacterEnabled())
        return (movement_control->GetXZActVelInGoingDir());

    return (m_speed);
}
#ifdef DEBUG
BOOL dbg_dump_collision_hit = FALSE;
void dump_collision_hit(CPHMovementControl* movement_control)
{
    if (!dbg_dump_collision_hit)
        return;
    VERIFY(movement_control);
    // CPHCharacter * phch = movement_control->PHCharacter();
    // VERIFY( phch );

    IPhysicsShellHolder* iobj = movement_control->PhysicsRefObject();
    VERIFY(iobj);
    VERIFY(smart_cast<CPhysicsShellHolder*>(iobj));
    CPhysicsShellHolder* obj = static_cast<CPhysicsShellHolder*>(iobj);
    Msg("ai unit: %s hited by collision; power: %f, spawn frame %d, current frame %d ", obj->cName().c_str(),
        movement_control->gcontact_HealthLost, obj->spawn_time(), Device.dwFrame);
    // CPhysicsShellHolder* object
    // =static_cast<CPhysicsShellHolder*>(Level().Objects.net_Find(m_collision_damage_info.m_obj_id));
    // const ICollisionDamageInfo * di=movement_control->CollisionDamageInfo();
    // VERIFY( di );
    // di->
}
#endif
void CMovementManager::apply_collision_hit(CPHMovementControl* movement_control)
{
    VERIFY(movement_control);
    if (object().g_Alive() && !fsimilar(0.f, movement_control->gcontact_HealthLost))
    {
#ifdef DEBUG
        dump_collision_hit(movement_control);
#endif
        const ICollisionDamageInfo* di = movement_control->CollisionDamageInfo();
        VERIFY(di);
        Fvector dir;
        di->HitDir(dir);

        SHit HDS = SHit(movement_control->gcontact_HealthLost, .0, dir, di->DamageInitiator(),
            movement_control->ContactBone(), di->HitPos(), 0.f, di->HitType(), 0.0f, false);
        object().Hit(&HDS);
    }
    // Consume the value: gcontact_HealthLost is recomputed only inside Calculate() (UpdateCollisionDamage),
    // which is skipped while the character is disabled/kinematic (including the Ф1 far-kinematic branches).
    // Left as is, the last collision's damage was re-applied on every idle tick until the next Calculate().
    movement_control->gcontact_HealthLost = 0.f;
}

// Ф1 (PHYSICS_ACTIVE_SET_PLAN, far-NPC kinematic): see move_along_path() for the call site. Deliberately a
// fresh, independent distance-to-actor check rather than reusing CAI_Stalker::get_cached_update_lod() -
// that LOD is stalker-only (ai_stalker.h/.cpp), while move_along_path() is shared by every CCustomMonster
// mover (stalkers, mutants, rats); reusing it would silently leave non-stalker AI unoptimized.
bool CMovementManager::should_force_far_kinematic(CPHMovementControl* movement_control, const Fvector& dest_position)
{
    m_kinematic_by_unseen = false;

    CActor* actor = smart_cast<CActor*>(Level().CurrentEntity());
    if (!actor)
    {
        m_kinematic_far_forced = false;
        m_kinematic_unseen_band = false;
        return false; // fail-safe: no actor (e.g. certain non-gameplay states) -> behave as before
    }

    // Hysteresis tracks distance independently of bypass flags, so a one-tick impulse/jump inside
    // the band does not lose the "already far" latch.
    const float dist_sq = dest_position.distance_to_sqr(actor->Position());
    if (ph_npc_kinematic_far_dist > 0.f)
    {
        float const engage_dist = ph_npc_kinematic_far_dist;
        float const release_dist = _max(0.f, engage_dist - ph_npc_kinematic_far_hyst);
        float const threshold = m_kinematic_far_forced ? release_dist : engage_dist;
        m_kinematic_far_forced = dist_sq > threshold * threshold;
    }
    else
        m_kinematic_far_forced = false;

    // Ф3: nearer than the far rule, but unseen. Evaluated on every call, so its latch stays current.
    const bool unseen = unseen_kinematic(dist_sq, actor);
    if (!m_kinematic_far_forced && !unseen)
        return false;
    m_kinematic_by_unseen = !m_kinematic_far_forced;

    // Never override while something else needs the real physics step this tick - forcing kinematic here
    // would snap a jump arc, cut a NoGravityZone/impulse short, or teleport an NPC that just lost footing.
    if (movement_control->JumpState() || movement_control->ForcedPhysicsControl() ||
        movement_control->bExernalImpulse || movement_control->IsLosingControl())
        return false;

    return true;
}

// Ф3 (plans/optimization_mt/21-physics-step.md): with hundreds of NPCs around the player the
// far rule (ph_npc_kinematic_far_dist, 100 m) catches few of them, and the live NPC controllers dominate the physics
// step (session 2026-10-14 17:02:53: 250-690 active controllers, 17-35 ms of step a frame). Nobody sees most of them:
// behind the camera, behind walls. Such an NPC moves the way the far ones do (move_along_path's kinematic branch:
// SetPosition + DisableCharacter, no GetNearest / TryPosition, no Calculate), if
// - farther than ph_npc_kinematic_unseen_dist (at least 10 m, see console_commands.cpp) from the actor - an NPC right
//   behind the player is never taken: the actor collides with it and pushes it as before. Engages beyond the distance,
//   releases under it minus a band, so an NPC walking along the boundary does not flip every frame;
// - not drawn in the main pass for unseen_frames frames in a row (CGameObject::rendered_in_main_pass: the frustum and
//   occlusion result of the render's own main pass, stamped per object, no extra rays). Shadows and the second viewport
//   do not count; while a second viewport (scope) renders, the rule is off - a scope shows far NPCs up close. Becoming
//   visible again releases it on the next update;
// - the actor is not its selected enemy (memory().enemy()): an NPC fighting the player keeps its full movement, cover
//   approach included, even off screen (physics/npc_kinematic_unseen_skip_combat).
// The safety exclusions of the far rule (jump, forced physics control, a pending impulse, losing ground) are applied by
// the caller. What is given up while kinematic: NPC-vs-NPC and NPC-vs-prop/door/corpse pushing (they pass through each
// other along the AI-map path; the level geometry is respected by the path itself). Hits and explosions still reach the
// NPC (bExernalImpulse puts it back on the physics path), bullets do not use the controller.
bool CMovementManager::unseen_kinematic(float dist_sq, const IGameObject* actor)
{
    // ~0.3-0.7 s at 15-30 FPS: a look around does not flip NPCs that only left the screen for a moment
    constexpr u32 unseen_frames = 10;
    // the distance band of the latch
    constexpr float release_band = 5.f;

    const float engage_dist = ph_npc_kinematic_unseen_dist;
    const float release_dist = _max(0.f, engage_dist - release_band);
    const float threshold = m_kinematic_unseen_band ? release_dist : engage_dist;
    m_kinematic_unseen_band = engage_dist > 0.f && dist_sq > threshold * threshold;
    if (!m_kinematic_unseen_band)
        return false;
    if (Device.m_SecondViewport.IsSVPActive() || object().rendered_in_main_pass(unseen_frames))
        return false;
    const CEntityAlive* enemy = object().memory().enemy().selected();
    if (enemy && static_cast<const IGameObject*>(enemy) == actor)
    {
        PERF_INC(ph_npc_kinematic_unseen_skip_combat);
        return false;
    }
    return true;
}

void CMovementManager::count_forced_kinematic() const
{
    if (m_kinematic_by_unseen)
        PERF_INC(ph_npc_kinematic_unseen_forced);
    else
        PERF_INC(ph_npc_kinematic_far_forced);
}

// Stage 4 of plans/optimization_arch/NPC_UPDATECL_WORKERS_2026-10-08.md (U2b + U2c, TryPosition coherence).
//
// Measured in a crowd: almost every moving NPC has a neighbour within contact reach, yet TryPosition (1-15
// step_single calls of 15-25 us) nearly always succeeds - it only confirms that nothing is in the way.
// A successful CPHAICharacter::TryPosition ends with the character disabled at the destination (it calls
// Disable() itself), i.e. in the very state the "no neighbours" branch of move_along_path leaves it in
// (SetPosition + DisableCharacter): the body is out of the active list of the physics world, while its geoms
// stay in the physics spatial space (SetPosition -> spatial_move; only CollisionEnable(FALSE) unregisters them).
// So the actor, awake props and awake characters still collide with a disabled NPC and wake it up
// (NearCallback -> EnableObject). A skipped frame therefore takes that same kinematic branch; what it gives up
// is the check against bodies that are asleep as well: other skipped / kinematic NPCs and resting props.
// The rules:
// - budget: after a success up to K frames skip TryPosition, then a full check. A failure empties the budget
//   and the streak; skipping resumes after npc_perf_move_try_skip_recover successes in a row;
// - K per NPC: the near K (on screen within npc_perf_move_try_skip_far_dist of the camera, and every NPC while
//   a second viewport renders - a scope shows far ones up close) or the far K (off screen or farther);
// - distance budget: the path a run of skips walks unchecked is capped, so the overlap depth stays bounded at
//   a low FPS too (at 80 ms a frame a running NPC walks ~0.35 m);
// - never skip (a full check): the NPC is the current entity; its character is awake (woken by a contact,
//   external impulse, non-interactive mode - IsCharacterEnabled), jumping, losing ground, under forced physics
//   control or carrying a capture; a big height step; a frame gap (the first frame of moving after standing or
//   after the NPC was not updated); the current entity within npc_perf_move_try_skip_actor_dist; the actor, an
//   awake physics shell (a prop, a car, a falling corpse) among the neighbours, or a resting shell within
//   contact reach. DoCollideObj() itself is the collision query TryPosition pays for - it cannot be asked
//   for less; an awake body touching the NPC shows up as IsCharacterEnabled() on the next frame instead.
// Two skipped neighbours may walk into each other for up to K frames. The next TryPosition of either one then
// collides (DoCollideObj -> merged island -> false), that NPC goes through Calculate() with a real physics step
// that pushes them apart and wakes the other one (NearCallback -> EnableObject -> a forced check on its next
// frame), and the hysteresis keeps both on full checks until they are clear again.
bool CMovementManager::try_position_skip(
    CPHMovementControl* movement_control, const Fvector& dest_position, float motion)
{
    // An update gap longer than this: the NPC stood still or was not updated (offline, update LOD) - check.
    constexpr u32 max_frame_gap = 4;
    // A height change in one frame above this (stairs, a ledge, a path over an obstacle) - check.
    constexpr float max_step_y = 0.3f;
    // The path a run of skips may walk unchecked: about a capsule diameter near, more where nobody sees it.
    constexpr float max_unchecked_near = 1.f;
    constexpr float max_unchecked_far = 3.f;
    // The contact reach of a resting body, as in the stage 0 close_contact estimate in move_along_path.
    constexpr float self_radius = 0.4f;
    constexpr float body_margin = 0.2f;

    const u32 frame = Device.dwFrame;
    const bool continuous =
        m_try_skip_frame != 0 && frame >= m_try_skip_frame && frame - m_try_skip_frame <= max_frame_gap;
    m_try_skip_frame = frame;

    // K of this NPC now: it also refills the budget if this frame ends up checking and the check succeeds
    bool far_tier = false;
    if (!Device.m_SecondViewport.IsSVPActive())
    {
        const float far_dist = float(npc_perf_move_try_skip_far_dist);
        far_tier = !object().rendered_in_main_pass() ||
            (far_dist > 0.f && Device.vCameraPosition.distance_to_sqr(dest_position) > far_dist * far_dist);
    }
    const int k = far_tier ? npc_perf_move_try_skip_frames_far : npc_perf_move_try_skip_frames;
    m_try_skip_k = u8(_max(0, _min(k, 255)));

    // The NPC came from the far tier into the near one: the shorter budget applies at once
    if (m_try_skip_left > m_try_skip_k)
        m_try_skip_left = m_try_skip_k;
    if (!m_try_skip_left)
        return false; // the budget is spent (or K = 0 here): a regular full check

    const bool profile = npc_cpp_profile::enabled();
    IGameObject* const current = Level().CurrentEntity();

    // From here on the budget would allow a skip: every early return below is a check the rules force
    if (m_try_skip_dist + motion > (far_tier ? max_unchecked_far : max_unchecked_near))
    {
        m_try_skip_left = 0; // the distance budget is spent: a regular check, not counted as forced
        return false;
    }

    if (!continuous || current == &object() || movement_control->IsCharacterEnabled() ||
        movement_control->bExernalImpulse || movement_control->ForcedPhysicsControl() ||
        movement_control->JumpState() || movement_control->IsLosingControl() || movement_control->PHCapture() ||
        _abs(dest_position.y - object().Position().y) > max_step_y)
    {
        if (profile)
            npc_cpp_profile::count(ENpcCppProfileStage::MovementTrySkipForcedState);
        m_try_skip_left = 0;
        return false;
    }

    bool body_near = false;
    const float actor_dist = float(npc_perf_move_try_skip_actor_dist);
    if (current && actor_dist > 0.f && current->Position().distance_to_sqr(dest_position) < actor_dist * actor_dist)
        body_near = true;
    for (auto it = m_nearest_objects.begin(); !body_near && it != m_nearest_objects.end(); ++it)
    {
        IGameObject* const neighbour = *it;
        if (neighbour == current || smart_cast<CActor*>(neighbour))
        {
            body_near = true;
            break;
        }
        // Living characters have no physics shell (their capsule is the CPHMovementControl character): they are
        // what the skipping is for. A shell is a prop, a car, a door, an item or a corpse.
        CPhysicsShellHolder* const holder = smart_cast<CPhysicsShellHolder*>(neighbour);
        CPhysicsShell* const shell = holder ? holder->PPhysicsShell() : nullptr;
        if (!shell || !shell->isActive())
            continue;
        if (shell->isEnabled())
        {
            body_near = true; // moving: it may come into the NPC this frame
            break;
        }
        // Asleep: it would not wake for a disabled NPC walking into it, so only TryPosition sees it
        const float reach = self_radius + neighbour->Radius() + motion + body_margin;
        const float dx = neighbour->Position().x - dest_position.x;
        const float dz = neighbour->Position().z - dest_position.z;
        if (dx * dx + dz * dz < reach * reach)
            body_near = true;
    }
    if (body_near)
    {
        if (profile)
            npc_cpp_profile::count(ENpcCppProfileStage::MovementTrySkipForcedBody);
        m_try_skip_left = 0;
        return false;
    }

    --m_try_skip_left;
    m_try_skip_dist += motion;
    m_try_skip_after_skip = true;
    if (profile)
    {
        npc_cpp_profile::count(
            far_tier ? ENpcCppProfileStage::MovementTrySkipFar : ENpcCppProfileStage::MovementTrySkipNear);
    }
    return true;
}

// Stage 4: the outcome of a real TryPosition. A success refills the budget with this NPC's K once the streak
// after the last failure is long enough; a failure empties both, so the next frames are full checks.
void CMovementManager::try_position_skip_result(bool success)
{
    if (success)
    {
        if (m_try_skip_streak != u8(-1))
            ++m_try_skip_streak;
        m_try_skip_left = int(m_try_skip_streak) >= npc_perf_move_try_skip_recover ? m_try_skip_k : 0;
    }
    else
    {
        // The main sign of harm: the skipped frames let the NPC into something the check now has to resolve
        if (m_try_skip_after_skip && npc_cpp_profile::enabled())
            npc_cpp_profile::count(ENpcCppProfileStage::MovementTrySkipFailAfterSkip);
        m_try_skip_streak = 0;
        m_try_skip_left = 0;
    }
    m_try_skip_dist = 0.f;
    m_try_skip_after_skip = false;
}

// Stage 4: the NPC stands or has no neighbours (the old kinematic branch) - its next frame with neighbours
// is a full check. The streak is kept: standing does not mean a collision.
void CMovementManager::try_position_skip_reset()
{
    m_try_skip_left = 0;
    m_try_skip_dist = 0.f;
    m_try_skip_after_skip = false;
}

bool CMovementManager::move_along_path() const
{
    if (!enabled())
        return (false);

    if (!actual())
        return (false);

    //	if (path_completed())
    //		return			(true);

    if (detail().path().empty())
        return (false);

    if (detail().completed(object().Position(), true))
        return (false);

    if (detail().curr_travel_point_index() >= detail().path().size() - 1)
        return (false);

    if (fis_zero(old_desirable_speed()))
        return (false);

    return (true);
}

Fvector CMovementManager::path_position(const float& velocity, const Fvector& position, const float& time_delta,
    u32& current_travel_point, float& dist, float& dist_to_target, Fvector& dir_to_target)
{
    VERIFY(current_travel_point < (detail().path().size() - 1));

    Fvector dest_position = position;

    // Вычислить пройденную дистанцию, определить целевую позицию на маршруте,
    //			 изменить detail().m_current_travel_point

    float desirable_speed = velocity; // желаемая скорость объекта
    dist = desirable_speed * time_delta; // пройденное расстояние в соостветствие с желаемой скоростью

    // определить целевую точку
    Fvector target;

    // обновить detail().m_current_travel_point в соответствие с текущей позицией
    while (current_travel_point < detail().path().size() - 2)
    {
        float pos_dist_to_cur_point = dest_position.distance_to(detail().path()[current_travel_point].position);
        float pos_dist_to_next_point = dest_position.distance_to(detail().path()[current_travel_point + 1].position);
        float cur_point_dist_to_next_point = detail().path()[current_travel_point].position.distance_to(
            detail().path()[current_travel_point + 1].position);

        if ((pos_dist_to_cur_point > cur_point_dist_to_next_point) && (pos_dist_to_cur_point > pos_dist_to_next_point))
        {
            ++current_travel_point;
        }
        else
            break;
    }

    target.set(detail().path()[current_travel_point + 1].position);
    // определить направление к целевой точке
    dir_to_target.sub(target, dest_position);

    // дистанция до целевой точки
    dist_to_target = dir_to_target.magnitude();

    while (dist > dist_to_target)
    {
        dest_position.set(target);
        dist -= dist_to_target;

        if (current_travel_point + 1 >= detail().path().size())
        {
            //			VERIFY				(dist <= dist_to_target);
            return (dest_position);
        }

        ++current_travel_point;
        if ((current_travel_point + 1) >= detail().path().size())
        {
            //			VERIFY				(dist <= dist_to_target);
            dist = 0.f;
            return (dest_position);
        }

        target.set(detail().path()[current_travel_point + 1].position);
        dir_to_target.sub(target, dest_position);
        dist_to_target = dir_to_target.magnitude();
    }

    VERIFY(dist <= dist_to_target);
    return (dest_position);
}

Fvector CMovementManager::path_position(const float& time_to_check)
{
    if (path_completed())
        return (object().Position());

    if (detail().path().empty())
        return (object().Position());

    if (detail().completed(object().Position(), true))
        return (object().Position());

    Fvector dir_to_target;
    float dist_to_target;
    float dist;
    u32 current_travel_point = detail().m_current_travel_point;
    return (path_position(old_desirable_speed(), object().Position(), time_to_check, current_travel_point, dist,
        dist_to_target, dir_to_target));
}

// Live NPCs left high in the air (plans/optimization_mt/21-physics-step.md, 2026-10-15). In a dense crowd living NPCs
// ended up tens of metres up and kept walking there, sinking slowly. Every branch of move_along_path() below except a
// failed TryPosition moves the NPC without gravity: SetPosition (far / unseen / no neighbours / TryPosition skipped) puts
// it at the path position, and a successful CPHAICharacter::TryPosition steps it with gravity off; the path position
// comes from the NPC's own position moved toward the next travel point in 3D (path_position), so an NPC that got high
// stays there and only descends along the slope to the travel point - and not at all while it stands. Whatever lifted it,
// such an NPC is now lowered to the AI floor (ground_snap). The diagnostics (ground_diag) tell what lifts it.

// The AI floor under a position: the plane of the NPC's level vertex. Extrapolated outside its cell (the vertex lags
// behind a moving NPC by a cell or so) unless that says the position is higher than refine_above - then the vertex under
// the position is looked up (a few cells around; at the height of the extrapolated floor, so it is the same storey and a
// position above the level box is still found) and must contain it, else there is no reliable floor (off the AI map).
bool CMovementManager::ground_floor_y(const Fvector& position, float refine_above, float& floor_y) const
{
    if (!ai().get_level_graph())
        return false;
    const CLevelGraph& graph = ai().level_graph();
    u32 vertex_id = object().ai_location().level_vertex_id();
    if (!graph.valid_vertex_id(vertex_id))
        return false;
    floor_y = graph.vertex_plane_y(vertex_id, position.x, position.z);
    if (position.y - floor_y <= refine_above)
        return true;
    const Fvector probe = {position.x, floor_y, position.z};
    if (graph.inside(vertex_id, probe))
        return true;
    vertex_id = graph.vertex(vertex_id, probe);
    if (!graph.valid_vertex_id(vertex_id) || !graph.inside(vertex_id, probe))
        return false;
    floor_y = graph.vertex_plane_y(vertex_id, position.x, position.z);
    return true;
}

// The NPC is legitimately off the floor or its position is not ours to change: a jump, physics control (script,
// NoGravityZone), a pending impulse, losing ground (falls by itself under physics), a capture, a ladder, an animation
// moving it (smart cover transitions, animpoints), a smart cover or a poltergeist (ground_snap_allowed), attached to a
// parent, the current entity (a possessed NPC).
bool CMovementManager::ground_snap_excluded(CPHMovementControl* movement_control) const
{
    if (!movement_control->CharacterExist() || movement_control->JumpState() ||
        movement_control->ForcedPhysicsControl() || movement_control->bExernalImpulse ||
        movement_control->IsLosingControl() || movement_control->PHCapture())
        return true;
    if (IElevatorState* elevator = movement_control->ElevatorState())
    {
        const Estate state = elevator->State();
        if (state == clbClimbingUp || state == clbClimbingDown || state == clbDepart)
            return true;
    }
    CCustomMonster& npc = object();
    return !npc.g_Alive() || npc.animation_movement_controlled() || npc.H_Parent() ||
        Level().CurrentEntity() == static_cast<IGameObject*>(&npc) || !ground_snap_allowed();
}

// Ground snap (always on): a destination the NPC is about to be moved to without gravity that hangs more than
// ai_npc_ground_snap_height above the AI floor (the floor of the NPC's own level vertex - the right storey in a building,
// the plane the path itself lies on) is lowered toward it at ai_npc_ground_snap_speed (0 = at once), and the lowering
// goes on until the NPC is on the floor. Why not "leave it to physics": a fall from tens of metres under the frame step
// is a collision hit that kills or cripples the NPC for a bug it did not cause, and a hanging NPC far away or off screen
// would need its controller enabled and stepped - the very cost the kinematic branches remove. A controlled descent at
// about a falling speed reads as a fall where it is seen and costs nothing. Lowered by up to a frame's descent before
// TryPosition too: it steps the NPC with gravity off, and its collisions keep it on whatever it really stands on
// (a TryPosition that touches another object fails and the frame step with gravity takes over).
// Returns true if dest_position was lowered.
bool CMovementManager::ground_snap(CPHMovementControl* movement_control, Fvector& dest_position, float time_delta)
{
    const float threshold = m_ground_snapping ? ground_snap_done : ai_npc_ground_snap_height;
    float floor_y;
    if (!ground_floor_y(dest_position, threshold, floor_y))
    {
        PERF_INC(ai_npc_ground_no_floor);
        m_ground_snapping = false;
        return false;
    }
    if (dest_position.y - floor_y <= threshold)
    {
        m_ground_snapping = false;
        return false;
    }
    if (ground_snap_excluded(movement_control))
    {
        PERF_INC(ai_npc_ground_snap_excluded);
        m_ground_snapping = false;
        return false;
    }
    m_ground_snapping = true;
    float y = floor_y;
    if (ai_npc_ground_snap_speed > 0.f)
        y = _max(floor_y, dest_position.y - ai_npc_ground_snap_speed * _max(time_delta, 0.f));
    dest_position.y = y;
    return true;
}

// The lift diagnostics (every update of a live NPC): the height of the final position above the
// AI floor; crossing 3 m counts ai/npc_ground/lift/<how this update moved it>, and the first crossings of a session are
// logged with the previous update, the updates spent in the air by kind and the live NPCs within 1 m (a crowd, standing
// on someone's head) - what lifts the NPC.
void CMovementManager::ground_diag(CPHMovementControl* movement_control, const Fvector& position, u8 branch)
{
    float floor_y;
    if (!ground_floor_y(position, ground_high_release, floor_y))
    {
        m_ground_prev_valid = false;
        return;
    }
    const float height = position.y - floor_y;
    const u8 kind = ground_branch_kind(branch);
    if (height > ground_on_floor)
    {
        if (m_ground_air_updates[kind] != u16(-1))
            ++m_ground_air_updates[kind];
    }
    else
    {
        for (u16& updates : m_ground_air_updates)
            updates = 0;
    }

    if (height > ground_high)
    {
        PERF_INC(ai_npc_ground_high);
#if XR_PERF_METRICS
        if (xray::perf::g_perf_metrics_active)
            xray::perf::UpdateMax(xray::perf::Metric::ai_npc_ground_max_height_mm, s64(height * 1000.f));
#endif
        if (!m_ground_high)
        {
            m_ground_high = true;
            switch (kind)
            {
            case 0: PERF_INC(ai_npc_ground_lift_kinematic); break;
            case 1: PERF_INC(ai_npc_ground_lift_try); break;
            case 2: PERF_INC(ai_npc_ground_lift_physics); break;
            default: PERF_INC(ai_npc_ground_lift_other); break;
            }

            // The neighbours at the moment of the lift (once per crossing): live NPCs within 1 m in xz and 3 m in height,
            // those of them at least 0.5 m below (this one stands on them), corpses there
            u32 live = 0, below = 0, corpses = 0;
            xr_vector<IGameObject*> nearest;
            Level().ObjectSpace.GetNearest(nearest, position, 1.5f, &object());
            for (IGameObject* neighbour : nearest)
            {
                CEntityAlive* alive = smart_cast<CEntityAlive*>(neighbour);
                if (!alive)
                    continue;
                const Fvector& p = neighbour->Position();
                const float dx = p.x - position.x;
                const float dz = p.z - position.z;
                const float dy = p.y - position.y;
                if (dx * dx + dz * dz > 1.f || _abs(dy) > 3.f)
                    continue;
                if (!alive->g_Alive())
                {
                    ++corpses;
                    continue;
                }
                ++live;
                if (dy < -0.5f)
                    ++below;
            }
            if (live)
                PERF_INC(ai_npc_ground_lift_crowd);
            if (below)
                PERF_INC(ai_npc_ground_lift_stacked);

            if (s_ground_logged.fetch_add(1, std::memory_order_relaxed) < ground_log_max)
            {
                Fvector velocity;
                movement_control->GetCharacterVelocity(velocity);
                Msg("! [npc_ground] '%s' %.1f m above the AI floor (vertex %u, floor %.2f, pos %.1f %.1f %.1f): this update "
                    "%s, the previous %s at %.1f m %u frames ago; updates in the air since the floor: kinematic %u, "
                    "try_position %u, physics %u, other %u; character %s, vy %.2f, jump %d, losing control %d, "
                    "impulse %d, forced physics %d; live NPCs within 1 m: %u (%u at least 0.5 m below), corpses %u",
                    object().cName().c_str(), height, object().ai_location().level_vertex_id(), floor_y, position.x,
                    position.y, position.z, ground_branch_names[branch < gb_count ? branch : gb_none],
                    m_ground_prev_valid ? ground_branch_names[m_ground_prev_branch] : "-",
                    m_ground_prev_valid ? m_ground_prev_height : 0.f,
                    m_ground_prev_valid ? Device.dwFrame - m_ground_prev_frame : 0u, u32(m_ground_air_updates[0]),
                    u32(m_ground_air_updates[1]), u32(m_ground_air_updates[2]), u32(m_ground_air_updates[3]),
                    movement_control->IsCharacterEnabled() ? "enabled" : "disabled", velocity.y,
                    int(movement_control->JumpState()), int(movement_control->IsLosingControl()),
                    int(movement_control->bExernalImpulse), int(movement_control->ForcedPhysicsControl()), live, below,
                    corpses);
            }
        }
    }
    else if (height < ground_high_release)
        m_ground_high = false;

    m_ground_prev_height = height;
    m_ground_prev_frame = Device.dwFrame;
    m_ground_prev_branch = branch < gb_count ? branch : u8(gb_none);
    m_ground_prev_valid = true;
}

void CMovementManager::move_along_path(CPHMovementControl* movement_control, Fvector& dest_position, float time_delta)
{
    u8 branch = gb_none;
    move_along_path_body(movement_control, dest_position, time_delta, branch);
    PERF_SCOPE(mv_ground); // 28-updatecl-main.md: movement/ground (ground_diag here, ground_snap in the body)
    ground_diag(movement_control, dest_position, branch);
}

void CMovementManager::move_along_path_body(
    CPHMovementControl* movement_control, Fvector& dest_position, float time_delta, u8& branch)
{
    START_PROFILE("Build Path/Move Along Path")

    VERIFY(movement_control);

    Fvector motion;
    dest_position = object().Position();

    float precision = 0.5f;

    // Stage 4 (TryPosition coherence): with both K = 0 nothing below touches its state - the old behaviour
    const bool try_skip_on = npc_perf_move_try_skip_frames > 0 || npc_perf_move_try_skip_frames_far > 0;

    // Если нет движения по пути
    if (!move_along_path())
    {
        m_speed = 0.f;
        if (try_skip_on)
            try_position_skip_reset(); // the first frame of moving again is a full check

        DBG_PH_MOVE_CONDITIONS(if (ph_dbg_draw_mask.test(phDbgNeverUseAiPhMove)) {
            movement_control->SetPosition(dest_position);
            if (movement_control->CharacterExist())
                movement_control->DisableCharacter();
        })
        branch = gb_idle;
        if (movement_control->IsCharacterEnabled())
        {
            // Ф1: idle far NPCs still pay a full physics step via Calculate() while enabled - force them
            // out of m_objects the same way the moving kinematic branch does (except jump/impulse/etc.).
            if (should_force_far_kinematic(movement_control, dest_position))
            {
                movement_control->DisableCharacter();
                count_forced_kinematic();
                branch = gb_idle_kinematic;
            }
            else
            {
                PERF_SCOPE(mv_idle_calc); // 28-updatecl-main.md: the frame step of a standing NPC with its character on
                movement_control->Calculate(detail().path(), 0.f, detail().m_current_travel_point, precision);
                movement_control->GetPosition(dest_position);
                branch = gb_idle_physics;
            }
        }
        // Ground snap: a standing NPC with its character disabled is never moved at all - one left in the air
        // would hang there until it walks again
        bool idle_snapped;
        {
            PERF_SCOPE(mv_ground);
            idle_snapped =
                !movement_control->IsCharacterEnabled() && ground_snap(movement_control, dest_position, time_delta);
        }
        if (idle_snapped)
        {
            movement_control->SetPosition(dest_position);
            PERF_INC(ai_npc_ground_snap_kinematic);
        }

        // проверка на хит
        apply_collision_hit(movement_control);
        //		Msg				("[%6d][%s] no move,
        // curr_tp=%d",Device.dwFrame,*object().cName(),detail().m_current_travel_point);
        return;
    }

    //. 	VERIFY2(movement_control->CharacterExist() || object().animation_movement_controlled() , "! Can not move -
    // physics movement shell does not exist. Try to move in wonded state?" );
    if (!movement_control->CharacterExist())
        return;

    if (time_delta < EPS)
        return;

    float desirable_speed = old_desirable_speed(); // желаемая скорость объекта
    float desirable_dist = desirable_speed * time_delta;
    float dist;

    // position_computation
    Fvector dir_to_target;
    float dist_to_target;
    u32 current_travel_point = detail().m_current_travel_point;
    {
        PERF_SCOPE(mv_path_position); // 28-updatecl-main.md: movement/path_position
        dest_position = path_position(old_desirable_speed(), object().Position(), time_delta, current_travel_point,
            dist, dist_to_target, dir_to_target);
    }

    // Lain: added steering behaviour
    // 	Fvector target;
    // 	target.add(dest_position, dir_to_target);
    // 	Fvector steer_offs = m_steer_manager->calc_acceleration();
    // 	steer_offs.mul(time_delta*10.f);
    // 	target.add(steer_offs);
    // 	dir_to_target.sub(target, dest_position);
    // 	dist_to_target = dir_to_target.magnitude();
    // 	Fvector steer_offs = m_steer_manager->calc_acceleration();
    // 	steer_offs.mul(time_delta*1000.f);
    // 	movement_control->AddControlVel(steer_offs);

    if (detail().m_current_travel_point != current_travel_point)
        on_travel_point_change(detail().m_current_travel_point);
    detail().m_current_travel_point = current_travel_point;

    if (dist_to_target < EPS_L)
    {
#pragma todo("Dima to ? : is this correct?")
        if (current_travel_point + 1 < detail().path().size())
            detail().m_current_travel_point = current_travel_point + 1;
        else
            detail().m_current_travel_point = detail().path().size() - 1;
        m_speed = 0.f;
        // Msg				("[%6d][%s] strange exit,
        // curr_tp=%d",Device.dwFrame,*object().cName(),detail().m_current_travel_point);
        return;
    }
    //	Msg					("[%6d][%s] curr_tp=%d",Device.dwFrame,*object().cName(),detail().m_current_travel_point);

    // Физика устанавливает новую позицию
    // получить физ. объекты в радиусе
    m_nearest_objects.clear();
    const bool forced_kinematic = should_force_far_kinematic(movement_control, dest_position);
    if (!forced_kinematic)
    {
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::MovementNearestQuery);
        PERF_SCOPE(mv_nearest); // 26-framemove-game-next.md, M3: movement/*
        Level().ObjectSpace.GetNearest(m_nearest_objects, dest_position,
            DISTANCE_PHISICS_ENABLE_CHARACTERS + (movement_control->IsCharacterEnabled() ? 0.5f : 0.f), &object());
    }
    else
    {
        // Ф1: skip the neighbor query entirely - m_nearest_objects stays empty, which already routes into
        // the existing kinematic branch below (SetPosition+DisableCharacter), same code path the engine
        // already takes whenever no dynamic physics object is nearby. Ф3: or unseen, see unseen_kinematic().
        count_forced_kinematic();
    }

    // установить позицию
    VERIFY(dist >= 0.f);
    VERIFY(dist_to_target >= 0.f);
    //	VERIFY				(dist <= dist_to_target);
    motion.mul(dir_to_target, dist / dist_to_target);
    dest_position.add(motion);

    // Ground snap: every branch below but a failed TryPosition moves the NPC to this point without gravity
    bool ground_snapped;
    {
        PERF_SCOPE(mv_ground);
        ground_snapped = ground_snap(movement_control, dest_position, time_delta);
    }

    Fvector velocity = dir_to_target;
    velocity.normalize_safe();
    if (velocity.y > 0.9f)
        velocity.y = 0.8f;
    if (velocity.y < -0.9f)
        velocity.y = -0.8f;
    velocity.normalize_safe(); //как не странно, mdir - не нормирован
    velocity.mul(desirable_speed); //*1.25f

    if (!movement_control->PhysicsOnlyMode())
        movement_control->SetCharacterVelocity(velocity);

    // -npc_cpp_profile only (stage 0 of plans/optimization_arch/NPC_UPDATECL_WORKERS_2026-10-08.md): does a
    // neighbour with a body reach the destination this frame? Living ones: two character capsules (~0.4 m each);
    // bodies with a physics shell: their bounding sphere. A margin on top: the estimate errs to "contact"
    if (!m_nearest_objects.empty() && npc_cpp_profile::enabled())
    {
        constexpr float self_radius = 0.4f;
        constexpr float margin = 0.2f;
        const float reach_motion = motion.magnitude();
        for (IGameObject* neighbour : m_nearest_objects)
        {
            CEntityAlive* alive = smart_cast<CEntityAlive*>(neighbour);
            CPhysicsShellHolder* holder = smart_cast<CPhysicsShellHolder*>(neighbour);
            const bool character = alive && alive->g_Alive();
            if (!character && !(holder && holder->PPhysicsShell()))
                continue;
            const float reach = self_radius + (character ? 0.4f : neighbour->Radius()) + reach_motion + margin;
            const float dx = neighbour->Position().x - dest_position.x;
            const float dz = neighbour->Position().z - dest_position.z;
            if (dx * dx + dz * dz < reach * reach)
            {
                npc_cpp_profile::count(ENpcCppProfileStage::MovementNearestCloseContact);
                break;
            }
        }
    }

    // Stage 4: a frame inside the TryPosition budget of this NPC moves it the way the "no neighbours" branch
    // below does - the same state a successful TryPosition leaves (see try_position_skip for why that is safe)
    if (try_skip_on && DBG_PH_MOVE_CONDITIONS(!ph_dbg_draw_mask.test(phDbgAlwaysUseAiPhMove) &&)
        !m_nearest_objects.empty() && try_position_skip(movement_control, dest_position, motion.magnitude()))
    {
        movement_control->SetPosition(dest_position);
        movement_control->DisableCharacter();
        movement_control->b_exect_position = true;
        branch = gb_try_skip;
        PERF_INC(mv_try_skip);
        if (ground_snapped)
            PERF_INC(ai_npc_ground_snap_kinematic);
    }
    else if (DBG_PH_MOVE_CONDITIONS(ph_dbg_draw_mask.test(phDbgNeverUseAiPhMove) ||
            !ph_dbg_draw_mask.test(phDbgAlwaysUseAiPhMove) &&) !(m_nearest_objects.empty()))
    { //  физ. объект
        // calls = the frames a physical object was near: the share of the query that pays off
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::MovementCollisionMove);
        PERF_SCOPE(mv_collision_move); // TryPosition, and Calculate when it fails (movement/try_fail)

        if (DBG_PH_MOVE_CONDITIONS(!ph_dbg_draw_mask.test(phDbgNeverUseAiPhMove) &&) !movement_control->TryPosition(
                dest_position))
        {
            NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::MovementTryPositionFail);
            PERF_SCOPE(mv_try_fail);
            branch = gb_physics;
            if (try_skip_on)
                try_position_skip_result(false);
            movement_control->GetPosition(dest_position);
            movement_control->Calculate(detail().path(), desirable_speed, detail().m_current_travel_point, precision);

            // проверка на хит
            apply_collision_hit(movement_control);
        }
        else
        {
            DBG_PH_MOVE_CONDITIONS(if (ph_dbg_draw_mask.test(phDbgNeverUseAiPhMove)) {
                movement_control->SetPosition(dest_position);
                movement_control->DisableCharacter();
            })
            movement_control->b_exect_position = true;
            branch = gb_try_ok;
            if (ground_snapped)
                PERF_INC(ai_npc_ground_snap_try);
            if (try_skip_on)
                try_position_skip_result(true);
        }
        movement_control->GetPosition(dest_position);
    }
    else
    {
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::MovementKinematic);
        PERF_SCOPE(mv_kinematic); // 28-updatecl-main.md: the branch without neighbours (SetPosition, DisableCharacter)
        if (try_skip_on)
            try_position_skip_reset(); // no neighbours: the next frame with some is a full check
        // DBG_PH_MOVE_CONDITIONS(
        // if(ph_dbg_draw_mask.test(phDbgNeverUseAiPhMove)){movement_control->SetPosition(dest_position);movement_control->DisableCharacter();})
        movement_control->SetPosition(dest_position);
        movement_control->DisableCharacter();
        movement_control->b_exect_position = true;
        branch = forced_kinematic ? gb_kinematic_forced : gb_kinematic_free;
        if (ground_snapped)
            PERF_INC(ai_npc_ground_snap_kinematic);
    }
    /*
    } else { // есть физ. объекты

        movement_control->Calculate				(detail().path(), desirable_speed, detail().m_current_travel_point,
    precision);
        movement_control->GetPosition			(dest_position);

        // проверка на хит
        apply_collision_hit						(movement_control);
    }
        */

    // установить скорость
    float real_motion = motion.magnitude() + desirable_dist - dist;
    float real_speed = real_motion / time_delta;

    m_speed = 0.5f * desirable_speed + 0.5f * real_speed;

    // Физика устанавливает позицию в соответствии с нулевой скоростью
    if (detail().completed(dest_position, true))
    {
        if (!movement_control->PhysicsOnlyMode())
        {
            Fvector velocity = {0.f, 0.f, 0.f};
            movement_control->SetVelocity(velocity);
            m_speed = 0.f;
        }
    }
    STOP_PROFILE
}
