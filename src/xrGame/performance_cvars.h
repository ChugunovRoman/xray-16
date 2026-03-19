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
extern u32 npc_perf_planner_actuality_interval_combat_ms;
extern u32 npc_perf_planner_actuality_interval_danger_ms;
extern u32 npc_perf_planner_actuality_interval_near_idle_ms;
extern u32 npc_perf_planner_graph_search_max_nodes;

// GameObject script binder (GameObject.cpp)
extern u32 npc_perf_long_dead_skip_binder_ms;
extern float npc_perf_binder_far_dist;
extern u32 npc_perf_binder_far_interval_ms;
extern u32 npc_perf_binder_far_phases;
extern u32 npc_perf_binder_near_interval_ms;   // throttle: min interval between scriptBinder.shedule_Update for near NPCs (ms)
extern u32 npc_perf_binder_far_throttle_ms;    // throttle: min interval between scriptBinder.shedule_Update for far NPCs (ms)

// Stalker visibility (ai_stalker.cpp)
extern u32 npc_perf_stalker_vis_interval_medium_ms;
extern u32 npc_perf_stalker_vis_interval_far_ms;

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

#endif // PERFORMANCE_CVARS_H_INCLUDED
