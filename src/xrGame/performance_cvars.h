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

// CCoverManager::best_cover (cover_manager_inline.h): cap evaluate() calls per search; 0 = vanilla. When >0, candidates sorted by distance first.
extern u32 npc_perf_cover_best_max_evaluate;
/** After quadtree nearest(): keep only this many closest points (0 = all). Cuts expensive accessible() loops when n is huge. */
extern u32 npc_perf_cover_nearest_max_points;
/** Max evaluator.accessible() calls per best_cover scan (0 = unlimited). Use with nearest_max when eval cap alone is not enough. */
extern u32 npc_perf_cover_best_max_accessible;
/** Shared cap on evaluate() across both 10m and 30m passes inside find_best_cover (0 = off). */
extern u32 npc_perf_cover_find_eval_budget_total;
/** 1 = skip the 10m pass in find_best_cover (only 30m). */
extern int npc_perf_cover_find_skip_near_10m;
/** Internal: remaining evals for find budget; ~0u = inactive (see npc_perf_cover_find_eval_budget_total). */
extern u32 g_npc_perf_cover_find_eval_budget_remaining;

// GameObject script binder (GameObject.cpp)
extern int npc_perf_skip_script_net_relcase; // 1 = do not call scriptBinder.net_Relcase (O(N*M) broadcast); 0 = vanilla
extern u32 npc_perf_long_dead_skip_binder_ms;
extern float npc_perf_binder_far_dist;
extern u32 npc_perf_binder_far_interval_ms;
extern u32 npc_perf_binder_far_phases;
extern u32 npc_perf_binder_near_interval_ms;   // throttle: min interval between scriptBinder.shedule_Update for near NPCs (ms)
extern u32 npc_perf_binder_far_throttle_ms;    // throttle: min interval between scriptBinder.shedule_Update for far NPCs (ms)

// Stalker visibility (ai_stalker.cpp)
extern u32 npc_perf_stalker_vis_interval_near_ms; // 0 = every schedule tick when Near; >0 when no selected enemy (crowd LOS throttle)
extern u32 npc_perf_stalker_vis_interval_medium_ms;
extern u32 npc_perf_stalker_vis_interval_far_ms;
/** 0 = unlimited total LOS rays per frame across NPCs; >0 = cap shared by `CVisionUpdateOrchestrator`. */
extern u32 npc_perf_vision_global_ray_budget;
/** When a live stalker has a selected enemy, `TakeRayBudget` requests at least this many rays (still clamped by global). */
extern u32 npc_perf_vision_combat_min_rays;
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
/** 0 = vanilla (IK every tick when enemy selected). >0 = min interval ms for stalker IK with selected enemy (crowds). */
extern u32 npc_perf_ik_interval_enemy_selected_ms;

// IK foot rays (ik_foot_collider.cpp): parallel first-hit batch via CObjectSpace::RayPickBatch when TaskScheduler active
extern int npc_perf_ik_foot_raypick_batch;

/**
 * Reserved. Full cross-NPC parallel in_UpdateCL would require a two-phase stalker UpdateCL (physics barrier before sight).
 * seqParallel runs after FrameMove; deferring physics there breaks ordering with sight in the same UpdateCL.
 */
extern int npc_perf_mt_stalker_physics;

// CAI_Stalker::UpdateCL — Tracy ucl_stalker_physics / ucl_stalker_step_manager; 1 = skip that stage (profiling / stress only).
extern int npc_perf_disable_ucl_stalker_physics;
extern int npc_perf_disable_ucl_stalker_step_manager;
/** When physics/step are disabled above, dead stalkers still run those stages for this many ms after Die() (0 = no override). */
extern u32 npc_perf_disable_ucl_stalker_postdeath_grace_ms;

// Lua script TTL/interval (read via get_console():get_integer in scripts)
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
extern u32 npc_perf_sim_brain_actor_update_interval;

/** 1 = Tracy zone per combat member in CAgentEnemyManager::fill_enemies (fill_from_squads/member). Default 0: thousands of zones hurt Tracy and add overhead. */
extern int npc_perf_agent_enemy_fill_squads_trace_members;

#endif // PERFORMANCE_CVARS_H_INCLUDED
