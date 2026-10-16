////////////////////////////////////////////////////////////////////////////
// Performance / NPC throttling and cache console variables (user.ltx)
////////////////////////////////////////////////////////////////////////////

#ifndef PERFORMANCE_CVARS_H_INCLUDED
#define PERFORMANCE_CVARS_H_INCLUDED

// Stalker planner (stalker_planner.cpp)
extern float npc_perf_planner_near_dist;       // meters, LOD "near"
extern float npc_perf_planner_medium_dist;    // meters, LOD "medium"
extern u32 npc_perf_planner_solve_interval_near_idle_ms;
extern u32 npc_perf_planner_solve_interval_medium_ms;
extern u32 npc_perf_planner_solve_interval_far_ms;
extern u32 npc_perf_planner_solve_interval_combat_near_ms;
/** Danger memory active, selected enemy nullptr: min interval between top-level solves (0 = vanilla, use near/LOD rules). */
extern u32 npc_perf_planner_solve_interval_danger_only_ms;
extern u32 npc_perf_planner_actuality_interval_combat_ms;
extern u32 npc_perf_planner_actuality_interval_danger_ms;
extern u32 npc_perf_planner_actuality_interval_near_idle_ms;
extern u32 npc_perf_planner_graph_search_max_nodes;
/** Alternative max nodes when enemy is selected (combat) (0 = use npc_perf_planner_graph_search_max_nodes). */
extern u32 npc_perf_planner_graph_search_max_nodes_combat;
/** Alternative max nodes when danger memory is active (0 = use npc_perf_planner_graph_search_max_nodes). */
extern u32 npc_perf_planner_graph_search_max_nodes_danger;
/** Nested GOAP (combat/danger sub-planners): xrAICore global g_ai_nested_planner_graph_search_max_nodes; console npc_perf_planner_nested_graph_search_max_nodes. */

/** Animation LOD (always on, ai_stalker.cpp): animation update interval (ms) for far (>80m), non-combat NPCs. */
extern int npc_anim_lod_far_interval_ms;

// CCoverManager::best_cover (cover_manager_inline.h): cap evaluate() calls per search; 0 = vanilla. When >0, candidates sorted by distance first.
extern int npc_perf_cover_best_max_evaluate;
/** After quadtree nearest(): keep only this many closest points (0 = all). Cuts expensive accessible() loops when n is huge. */
extern int npc_perf_cover_nearest_max_points;
/** Max evaluator.accessible() calls per best_cover scan (0 = unlimited). Use with nearest_max when eval cap alone is not enough. */
extern int npc_perf_cover_best_max_accessible;
/** Cap on evaluate() inside find_best_cover (the 30m pass; the vanilla 10m pass is gone) (0 = off). */
extern int npc_perf_cover_find_eval_budget_total;
/** Internal: remaining evals for find budget; ~0u = inactive (see npc_perf_cover_find_eval_budget_total). */
extern u32 g_npc_perf_cover_find_eval_budget_remaining;

// GameObject script binder (GameObject.cpp; scriptBinder.net_Relcase, an O(N*M) broadcast, is never called)
extern u32 npc_perf_long_dead_skip_binder_ms;
extern float npc_perf_binder_far_dist;
extern u32 npc_perf_binder_far_interval_ms;
extern u32 npc_perf_binder_far_phases;
extern u32 npc_perf_binder_near_interval_ms;   // throttle: min interval between scriptBinder.shedule_Update for near NPCs (ms)
extern u32 npc_perf_binder_far_throttle_ms;    // throttle: min interval between scriptBinder.shedule_Update for far NPCs (ms)

// Stalker visibility (ai_stalker.cpp)
extern int npc_perf_stalker_vis_interval_near_ms; // 0 = every schedule tick when Near; >0 when no selected enemy (crowd LOS throttle)
extern int npc_perf_stalker_vis_interval_medium_ms;
extern int npc_perf_stalker_vis_interval_far_ms;
/** P2: vision rays of all NPCs go to one batch (always); minimum rays in it to run it in parallel via TaskScheduler
    (below -> serial). */
extern int npc_perf_vision_parallel_batch_min_rays;
/** Physics objects with spatial radius below this threshold (meters) do not register as STYPE_VISIBLEFORAI. 0 = disabled. */
extern float npc_perf_vision_small_physics_radius;
// npc_perf_vision_trace_* etc.: ENGINE_API in xrEngine/Feel_Vision.h (defined in Feel_Vision.cpp)

// CustomMonster visibility (CustomMonster.cpp)
extern float npc_perf_monster_vis_near_dist;
extern float npc_perf_monster_vis_medium_dist;
extern u32 npc_perf_monster_vis_interval_medium_ms;
extern u32 npc_perf_monster_vis_interval_far_ms;

// Character IK (CharacterPhysicsSupport.cpp)
extern u32 npc_perf_ik_interval_near_idle_ms;
extern u32 npc_perf_ik_interval_medium_ms;
extern u32 npc_perf_ik_interval_far_ms;
extern u32 npc_perf_ik_interval_disabled_ms;
/** 0 = vanilla (IK every tick when enemy selected). >0 = min interval ms for stalker IK with selected enemy (crowds).
    Default 100 (K10 of plans/optimization_mt/26-framemove-game-next.md: the tested value; was 0). */
extern u32 npc_perf_ik_interval_enemy_selected_ms;

// IK foot rays (IKLimbsController.cpp): all rays of an NPC in one batch (always), static geometry only (always);
// minimum rays to cast them with CObjectSpace::RayPickBatch (below -> one RayPick each)
extern int npc_perf_ik_foot_raypick_batch_min_rays;

// Lua script TTL/interval (read via get_console():get_integer in scripts)
extern int ai_evaluator_ttl_ms; // C++ script GOAP evaluator time-based cache TTL (0 = disabled)
extern u32 npc_perf_state_mgr_animstate_ttl_ms;
extern u32 npc_perf_script_combat_ttl_ms;
extern u32 npc_perf_evaluator_combat_enemy_cache_ttl_ms;
extern u32 npc_perf_danger_eval_ttl_ms;
extern u32 npc_perf_script_danger_eval_ttl_ms;
extern u32 npc_perf_state_mgr_idle_combat_ttl_ms;
extern u32 npc_perf_state_mgr_end_ttl_ms;
extern u32 npc_perf_planner_update_min_interval_ms;
extern u32 npc_perf_planner_update_min_interval_far_ms;
extern u32 npc_perf_evaluator_abuse_ttl_ms;
extern u32 npc_perf_gather_items_eval_ttl_ms;
extern u32 npc_perf_state_mgr_movement_ttl_ms;
extern u32 npc_perf_motivator_medium_interval;
extern u32 npc_perf_motivator_far_interval;
extern u32 npc_perf_motivator_slow_near_interval;
extern u32 npc_perf_motivator_slow_medium_interval;
extern u32 npc_perf_motivator_slow_far_interval;
extern u32 npc_perf_motivator_switch_near_interval;
extern u32 npc_perf_motivator_switch_medium_interval;
extern u32 npc_perf_motivator_switch_far_interval;
extern u32 npc_perf_motivator_callback_near_interval;
extern u32 npc_perf_motivator_callback_medium_interval;
extern u32 npc_perf_motivator_callback_far_interval;
extern u32 npc_perf_motivator_dead_interval;
// Stage D, W3-1: 0 = the Lua NPC binder computes its update lane and phases itself (old); 1 = the engine does,
// with the same rules and the npc_perf_motivator_* intervals, and skips the Lua update when no phase is due.
extern u32 npc_perf_motivator_native_gate;
extern u32 npc_perf_sim_brain_actor_update_interval;

/** 1 = Tracy zone per combat member in CAgentEnemyManager::fill_enemies (fill_from_squads/member). Default 0: thousands of zones hurt Tracy and add overhead. */
extern int npc_perf_agent_enemy_fill_squads_trace_members;

/** 1 = visual_memory_manager.get_visible_value computed in C++ (the same formula); 0 = the Lua function, as before
    (an addon that changes the script formula needs 0). */
extern int npc_perf_visible_value_native;

// Stage D, W4-2 (gw_condlist_native.cpp): xr_logic.pick_section_from_condlist in the engine.
// 0 = pure Lua (default); 1 = the native interpreter decides (falls back per call when the condlist
// has anything it cannot evaluate); 2 = shadow: a dry native pass runs first (no infop_set, no
// effects, the LuaJIT math.random state is restored after it), then Lua decides and mismatches are
// counted (_gw_internal.logic.stats prints them).
extern int npc_perf_condlist_native;

/** IK of NPCs in a fight (CharacterPhysicsSupport.cpp, get_character_ik_update_interval). 0 = as before:
    a stalker with a selected enemy runs IK at npc_perf_ik_interval_enemy_selected_ms at any distance. N > 0 = that
    interval only within N m of the actor; farther the distance scale (near / medium / far / disabled) applies.
    Default 20 (K10 of 26-framemove-game-next.md: the tested value; was 0). */
extern int npc_perf_ik_enemy_near_dist;

/** IK of NPCs off screen. 0 = as before. N > 0 = an NPC not seen in the last main render pass runs IK at
    least N ms apart, whatever the other rules say (the feet keep the last ground contact meanwhile); not while a
    second viewport (scope) renders. Default 450 (K10 of 26-framemove-game-next.md: the tested value; was 0). */
extern int npc_perf_ik_offscreen_interval_ms;

/** UpdateCL rate LOD of living stalkers by the distance to the camera, on screen too (ai_stalker.cpp,
    CAI_Stalker::TrySkipUpdateCLLod; stage 3 of plans/optimization_arch/NPC_UPDATECL_WORKERS_2026-10-08.md), always
    on in single player: a stalker farther than npc_perf_ucl_lod_dist runs its UpdateCL every 2nd frame,
    farther than npc_perf_ucl_lod_dist2 every 3rd one; a skipped frame only keeps it in the spatial DB and in the
    update list (the crow rules), the next full UpdateCL gets the whole time since the last one (movement, look,
    sounds), up to npc_perf_ucl_lod_max_delta_ms. Never skipped: fight (enemy, recent hit, weapon firing), jump /
    lost control / impulse, the current (view) entity, a second viewport, the first frames after the spawn,
    animation-driven movement, active bone particles, the dead. The weapon in the hands skips with its owner when
    idle (CWeapon::TrySkipOwnerLodUpdateCL). */
/** Distance to the camera in m beyond which the UpdateCL LOD runs a stalker every 2nd frame (5..500, default 30).
    A zoomed camera (smaller FOV) shrinks the distance accordingly. */
extern int npc_perf_ucl_lod_dist;
/** Distance to the camera in m beyond which the UpdateCL LOD runs a stalker every 3rd frame; 0 = no such tier
    (0..500, default 45). Below npc_perf_ucl_lod_dist it has no effect. */
extern int npc_perf_ucl_lod_dist2;
/** The most time in ms a stalker may go without a full UpdateCL under the LOD (100..500, default 250): a frame is
    not skipped if the time since the last full UpdateCL plus this frame's delta would exceed it, and the first
    full UpdateCL after a skip gets up to this much time instead of the usual 100 ms clamp. */
extern int npc_perf_ucl_lod_max_delta_ms;
/** The combat tier of the UpdateCL LOD (plans/optimization_mt/24-critical-path.md, F2; always on, saved tunable,
    default 2): N = 2..4 = a stalker within npc_perf_ucl_lod_dist or in a fight runs its UpdateCL every N-th frame
    if it was not drawn in the main pass last frame (no second viewport), is not drawn in shadows / lights within
    npc_perf_ucl_lod_dist, the actor is not its enemy and it was not hit in the last 1.5 s; 0 / 1 = every frame. The
    other refusals of the LOD (jump, physics control, animation movement...) and its time cap
    npc_perf_ucl_lod_max_delta_ms stay. */
extern int npc_ucl_lod_combat;

/* The object handler LOD (plans/optimization_mt/22-game-thread.md, step 1; ai_stalker.cpp,
    CAI_Stalker::TrySkipObjectHandlerLod / can_kill_pick_lod_reuse), always on in single player. Two things:
    - an idle stalker farther than npc_perf_object_handler_lod_dist m from the camera runs its object handler (the
      weapon planner, a seqParallel delegate of UpdateCL) on every npc_perf_object_handler_lod_every_seen-th UpdateCL
      when on screen and every npc_perf_object_handler_lod_every_unseen-th one when not, but at least once in
      npc_perf_object_handler_lod_max_ms. Idle = the planner is in an idle / strapped / aiming-ready action with the
      goal reached and nothing changed since its last solve, the item in the hands is idle or hidden with no pending
      state, no enemy, no hit in the last 5 s, no recoil; everything else (firing, reloading, switching, a new goal,
      a new item) runs every UpdateCL as before;
    - a firing stalker that nobody sees (off screen, farther than npc_perf_object_handler_lod_dist m, not fighting
      the actor) keeps its friendly-fire pick (CAI_Stalker::update_can_kill_info, up to 5 ray queries a frame) for up
      to npc_perf_object_handler_lod_pick_ms instead of casting it every frame.
    The tunables below. */
/** Distance to the camera in m within which the object handler LOD never applies (5..500, default 20). A zoomed
    camera shrinks the distance the way the UpdateCL LOD does. */
extern int npc_perf_object_handler_lod_dist;
/** The object handler of an idle stalker on screen runs on every N-th UpdateCL (1..16, default 3). */
extern int npc_perf_object_handler_lod_every_seen;
/** The object handler of an idle stalker off screen runs on every N-th UpdateCL (1..32, default 12). */
extern int npc_perf_object_handler_lod_every_unseen;
/** The most time in ms between two object handler runs of an idle stalker under the LOD (50..1000, default 200). */
extern int npc_perf_object_handler_lod_max_ms;
/** How long in ms a firing stalker nobody sees may keep its friendly-fire pick (0..500, default 100; 0 = the pick
    every frame, as before). */
extern int npc_perf_object_handler_lod_pick_ms;

/** The scheduler interval of corpses (22-game-thread.md, step 1; ai_stalker.cpp, CAI_Stalker::UpdateDeadShedLod):
    a dead stalker whose death is older than 10 s, whose ragdoll sleeps and whose hands are empty (the weapon dropped
    by the dead action) gets npc_perf_dead_shed_interval_ms instead of the living 100..625 ms; once it stops being
    settled (the ragdoll woken, an item in the hands, revived) it gets the living interval back.
    npc_perf_dead_shed_interval_ms: the interval in ms of a settled corpse (1000..8000, default 5000; the upper bound
    comes from the 14-bit shedule.t_max). */
extern int npc_perf_dead_shed_interval_ms;

/** TryPosition coherence of moving NPCs in a crowd (movement_manager_physic.cpp, CMovementManager::try_position_skip;
    stage 4 of plans/optimization_arch/NPC_UPDATECL_WORKERS_2026-10-08.md). A successful TryPosition is followed by
    up to K frames that move the NPC the way the "no neighbours" branch does (SetPosition + DisableCharacter), then a
    full check again; after a failed one every frame is checked until npc_perf_move_try_skip_recover successes in a
    row. Never skipped: the actor or a body with a physics shell next to the NPC, the actor within
    npc_perf_move_try_skip_actor_dist m, an awake / pushed / jumping character, the first frame after standing.
    npc_perf_move_try_skip_frames: K of an NPC on screen within npc_perf_move_try_skip_far_dist m of the camera
    (and of every NPC while a second viewport renders). npc_perf_move_try_skip_frames_far: K of an NPC off screen or
    farther. Both 0 (default) = the whole mechanism is off, as before. */
extern int npc_perf_move_try_skip_frames;
extern int npc_perf_move_try_skip_frames_far;
extern int npc_perf_move_try_skip_far_dist;
extern int npc_perf_move_try_skip_recover;
extern int npc_perf_move_try_skip_actor_dist;

#endif // PERFORMANCE_CVARS_H_INCLUDED
