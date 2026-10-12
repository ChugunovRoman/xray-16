#if !defined(PERF_COUNTER) || !defined(PERF_TIMER)
#   error "PERF_COUNTER and PERF_TIMER must be defined before including PerfMetricsList.inl"
#endif
#ifndef PERF_GAUGE
#   define PERF_GAUGE(id, name) PERF_COUNTER(id, name)
#endif

// TaskManager
PERF_COUNTER(task_exec_own, "task/exec_own")
PERF_COUNTER(task_exec_worker0, "task/exec_worker0")
PERF_COUNTER(task_exec_steal, "task/exec_steal")
PERF_COUNTER(task_exec_miss, "task/exec_miss")
PERF_COUNTER(task_steal_attempt, "task/steal_attempt")
PERF_COUNTER(task_steal_success, "task/steal_success")
PERF_COUNTER(task_sleep_count, "task/sleep_count")
PERF_TIMER(task_sleep_time, "task/sleep_time")
PERF_COUNTER(task_wait_spin, "task/wait_spin")
PERF_COUNTER(task_wake_event, "task/wake_event")
PERF_COUNTER(task_wait_game_on_main, "task/wait_game_on_main")
// Outermost Wait() on main only (thread_local depth guard): total loop time vs. time actually
// spent running a stolen/own task inside it. spin = total - exec (derived at analysis time).
PERF_TIMER(task_wait_total, "task/wait/total")
PERF_TIMER(task_wait_exec, "task/wait/exec")
// Which tl_wait_site the outermost main-thread Wait() above was tagged with when it returned.
PERF_COUNTER(task_wait_site_game, "task/wait/site/game")
PERF_COUNTER(task_wait_site_prerender, "task/wait/site/prerender")
PERF_COUNTER(task_wait_site_hom, "task/wait/site/hom")
PERF_COUNTER(task_wait_site_rmain, "task/wait/site/rmain")
PERF_COUNTER(task_wait_site_rrain, "task/wait/site/rrain")
PERF_COUNTER(task_wait_site_rsun, "task/wait/site/rsun")
PERF_COUNTER(task_wait_site_light, "task/wait/site/light")
PERF_COUNTER(task_wait_site_detail, "task/wait/site/detail")
PERF_COUNTER(task_wait_site_other, "task/wait/site/other")
// task_wait_game_on_main above, broken down by tl_wait_site at the moment GameThread() started.
PERF_COUNTER(task_wait_game_on_main_game, "task/wait_game_on_main/game")
PERF_COUNTER(task_wait_game_on_main_prerender, "task/wait_game_on_main/prerender")
PERF_COUNTER(task_wait_game_on_main_hom, "task/wait_game_on_main/hom")
PERF_COUNTER(task_wait_game_on_main_rmain, "task/wait_game_on_main/rmain")
PERF_COUNTER(task_wait_game_on_main_rrain, "task/wait_game_on_main/rrain")
PERF_COUNTER(task_wait_game_on_main_rsun, "task/wait_game_on_main/rsun")
PERF_COUNTER(task_wait_game_on_main_light, "task/wait_game_on_main/light")
PERF_COUNTER(task_wait_game_on_main_detail, "task/wait_game_on_main/detail")
PERF_COUNTER(task_wait_game_on_main_other, "task/wait_game_on_main/other")

// Physics (§2)
PERF_COUNTER(ph_substeps, "physics/substeps")
PERF_COUNTER(ph_substeps_clipped, "physics/substeps_clipped")
// How many substeps the ph_max_substeps clamp actually cut this frame (0 when not clamped) - without this,
// physics/substeps already reports the post-clamp count and the death-spiral depth is invisible.
PERF_COUNTER(ph_substeps_discarded, "physics/substeps_discarded")
PERF_TIMER(ph_collide, "physics/collide")
PERF_TIMER(ph_tune, "physics/phtune")
PERF_TIMER(ph_solve, "physics/solve")
PERF_TIMER(ph_post_integrate, "physics/post_integrate")
PERF_TIMER(ph_contact_cleanup, "physics/contact_cleanup")
// Phase 3: time main spends waiting for the dedicated physics thread to finish this frame's step,
// measured around PhysicsJoinOverlapCallback() in ProcessFrame.
PERF_TIMER(ph_overlap_join, "physics/overlap_join")
// B6/B8 (plans/optimization_mt/09-physics-earlier-parallel.md):
PERF_TIMER(ph_step_wall, "physics/step_wall")         // one frame's step on the physics thread, wake..done (wall time)
PERF_TIMER(ph_kick_delay, "physics/kick_delay")       // overlap window opened (after FrameMove + camera) .. step kicked
PERF_COUNTER(ph_islands, "physics/islands")           // active islands solved, summed over the substeps
PERF_COUNTER(ph_islands_offthread, "physics/islands_offthread") // of them solved by a pool worker, not the stepping thread
// B6 early physics kick (09-physics-earlier-parallel.md, "Режим 2: доделка", xrPhysics/PHStepDefer.h): mutations of simulated bodies
// made on another thread while the step runs, queued for the post-join flush instead of racing with the step.
PERF_COUNTER(ph_defer_lua, "physics/defer/lua")           // queued from a Lua call of the physics API / game_object
PERF_COUNTER(ph_defer_cpp, "physics/defer/cpp")           // queued from engine code (scheduler, telekinesis, cars, ...)
PERF_COUNTER(ph_defer_force, "physics/defer/force")       // by kind: applyForce / applyImpulse / gravity accel
PERF_COUNTER(ph_defer_velocity, "physics/defer/velocity") // set_LinearVel / set_AngularVel / setForce / setTorque
PERF_COUNTER(ph_defer_fix, "physics/defer/fix")           // Fix / ReleaseFixed / set_ApplyByGravity
PERF_COUNTER(ph_defer_joint, "physics/defer/joint")       // joint motors, limits, axes, spring/damping
PERF_COUNTER(ph_defer_enable, "physics/defer/enable")     // waking a sleeping shell / element (activation)
PERF_COUNTER(ph_defer_contact, "physics/defer/contact")   // contact callback lists (doors), collision on/off
PERF_COUNTER(ph_defer_teleport, "physics/defer/teleport") // Lua teleports
PERF_COUNTER(ph_defer_object, "physics/defer/object")     // owner-level: monster capture, update objects, physics_world
PERF_COUNTER(ph_defer_transform, "physics/defer/transform") // teleports of bodies in the world (SetTransform, ToBonePos,
                                                            // SetGlTransformDynamic): one per element and window
PERF_COUNTER(ph_defer_transform_coalesced, "physics/defer/transform_coalesced") // later teleports of an element already
                                                            // queued in the window: only the last pose is applied
PERF_COUNTER(ph_defer_dropped, "physics/defer/dropped")   // queued, but the target was gone at the flush
PERF_COUNTER(ph_defer_flushed, "physics/defer/flushed")   // everything run by the flush (with the older Request* intents)
PERF_COUNTER(ph_defer_hazard, "physics/defer/hazard")     // structural mutations NOT deferred (see PH_STEP_HAZARD)
PERF_COUNTER(ph_defer_hazard_transform, "physics/defer/hazard_transform") // of them: moves of bodies in the world
                                                            // (no owner ID, TransformPosition, SetGlobalPositionDynamic)
PERF_COUNTER(ph_obj_actor, "physics/obj/actor")
PERF_COUNTER(ph_obj_ai, "physics/obj/ai")
PERF_COUNTER(ph_obj_ragdoll, "physics/obj/ragdoll")
PERF_COUNTER(ph_obj_vehicle, "physics/obj/vehicle")
PERF_COUNTER(ph_obj_item, "physics/obj/item")
PERF_COUNTER(ph_obj_static, "physics/obj/static")
PERF_COUNTER(ph_obj_other, "physics/obj/other")
PERF_GAUGE(ph_update_objects, "physics/update_objects")
PERF_GAUGE(ph_island_batch, "physics/island_batch")
PERF_GAUGE(ph_recently_disabled, "physics/recently_disabled")
PERF_GAUGE(ph_frozen, "physics/frozen")
// Sticky all-time max (xray::perf::UpdateMax), not a per-window peak - see the worst linear speed among
// enabled, dead (corpse) ragdoll shells. Scaled mm/s so it fits SetGauge/UpdateMax's s64 (no raw float gauge).
PERF_GAUGE(ph_corpse_max_vel_mm_s, "physics/corpse_max_vel_mm_s")
// Ф2 (PHYSICS_ACTIVE_SET_PLAN, distance-based): corpses force-disabled by CPHWorld::SleepCorpses()
// (distinct from ph_shell_disabled, which also counts ODE's own natural all-elements auto-sleep for
// every shell type, not just this forced corpse path).
PERF_COUNTER(ph_corpse_force_slept, "physics/corpse_force_slept")
// Cost of the post-ReinitMerged SleepCorpses() apply step (the decision itself runs inside ph_solve's
// snapshot loop and isn't separately timed).
PERF_TIMER(ph_corpse_sleep_apply, "physics/corpse_sleep_apply")
// Ф1 (PHYSICS_ACTIVE_SET_PLAN): live AI NPCs forced into pure kinematic (skip GetNearest/TryPosition)
// by movement_manager_physic.cpp when farther than ph_npc_kinematic_far_dist from the actor.
PERF_COUNTER(ph_npc_kinematic_far_forced, "physics/npc_kinematic_far_forced")
PERF_TIMER(ph_collide_static, "physics/collide_static")
PERF_COUNTER(ph_collide_static_contacts, "physics/collide_static/contacts")
PERF_COUNTER(ph_near_calls, "physics/near/calls")
PERF_COUNTER(ph_near_merge_reject, "physics/near/merge_reject")
PERF_COUNTER(ph_near_dyn_contacts, "physics/near/dyn_contacts")
PERF_COUNTER(ph_wake, "physics/wake")
// obj2's type (the object a live contact just woke up), mirroring the ph_obj_* classification above.
PERF_COUNTER(ph_wake_actor, "physics/wake/actor")
PERF_COUNTER(ph_wake_ai, "physics/wake/ai")
PERF_COUNTER(ph_wake_ragdoll, "physics/wake/ragdoll")
PERF_COUNTER(ph_wake_vehicle, "physics/wake/vehicle")
PERF_COUNTER(ph_wake_item, "physics/wake/item")
PERF_COUNTER(ph_wake_static, "physics/wake/static")
PERF_COUNTER(ph_wake_other, "physics/wake/other")
PERF_COUNTER(ph_broadphase_calls, "physics/broadphase/calls")
PERF_COUNTER(ph_broadphase_results, "physics/broadphase/results")
PERF_COUNTER(ph_broadphase_candidates, "physics/broadphase/candidates")
PERF_COUNTER(ph_tri_new, "physics/tri/new")
PERF_COUNTER(ph_tri_reused, "physics/tri/reused")
PERF_COUNTER(ph_tri_walk, "physics/tri/walk")
PERF_COUNTER(ph_tri_aabb, "physics/tri/aabb_pass")
PERF_COUNTER(ph_try_position_calls, "physics/try_position/calls")
PERF_COUNTER(ph_try_position_steps, "physics/try_position/steps")
PERF_COUNTER(ph_try_cycle_fail, "physics/try_position/cycle_fail")
PERF_COUNTER(ph_try_final_fail, "physics/try_position/final_fail")
PERF_COUNTER(ph_ctrl_block_control, "physics/ctrl/block_is_control")
PERF_COUNTER(ph_ctrl_block_no_contact, "physics/ctrl/block_no_contact")
PERF_COUNTER(ph_ctrl_block_lose_ground, "physics/ctrl/block_lose_ground")
PERF_COUNTER(ph_shell_disabled, "physics/shell/disabled")
PERF_COUNTER(ph_shell_active, "physics/shell/active")
PERF_COUNTER(ph_shell_ragdoll_bodies, "physics/shell/ragdoll_bodies")
// P16 (plans/optimization_mt/11-ragdoll-spikes.md): CPHActivationShape::Activate - a box pushed out of everything around
// in a frozen world (spawn of a character, start of a free ragdoll, items, explosives). Each of its world steps also shows
// in physics/collide etc. (calls above physics/substeps). Who called it - npc_cpp_profile character_physics/death_correct.
PERF_TIMER(ph_act_shape, "physics/activation_shape")
PERF_TIMER(ph_act_shape_freeze, "physics/activation_shape/freeze")       // CPHWorld::Freeze + UnFreeze (all objects)
PERF_COUNTER(ph_act_shape_steps, "physics/activation_shape/steps")       // world steps of the push-out loop
PERF_COUNTER(ph_act_shape_early_exit, "physics/activation_shape/early_exit") // no penetration - no loop
PERF_COUNTER(ph_act_shape_unresolved, "physics/activation_shape/unresolved") // loop ended still penetrating
PERF_COUNTER(ph_act_shape_capped, "physics/activation_shape/capped")     // of them stopped by ph_death_correct_max_steps

// Sound (§3)
PERF_COUNTER(snd_event_queued, "sound/event/queued")
PERF_COUNTER(snd_event_dropped, "sound/event/dropped")
PERF_GAUGE(snd_events_frame, "sound/events_frame")
PERF_COUNTER(snd_reg_not_loaded, "sound/reg/not_loaded")
PERF_COUNTER(snd_reg_destroyed, "sound/reg/destroyed")
PERF_COUNTER(snd_reg_no_feedback, "sound/reg/no_feedback")
PERF_COUNTER(snd_reg_merged, "sound/reg/merged")
PERF_COUNTER(snd_reg_burst, "sound/reg/burst")
PERF_COUNTER(snd_reg_budget, "sound/reg/budget")
PERF_COUNTER(snd_reg_budget_priority, "sound/reg/budget_priority")
PERF_COUNTER(snd_reg_passed, "sound/reg/passed")
PERF_GAUGE(snd_reg_listeners, "sound/reg/listeners")
PERF_COUNTER(snd_lst_not_feel, "sound/listener/not_feel")
PERF_COUNTER(snd_lst_destroyed, "sound/listener/destroyed")
PERF_COUNTER(snd_lst_too_far, "sound/listener/too_far")
PERF_COUNTER(snd_lst_power_eps, "sound/listener/power_eps")
PERF_COUNTER(snd_lst_occ_skip, "sound/listener/occ_skip")
PERF_COUNTER(snd_lst_occ_ray, "sound/listener/occ_ray")
PERF_COUNTER(snd_lst_occ_power_eps, "sound/listener/occ_power_eps")
PERF_COUNTER(snd_lst_dispatched, "sound/listener/dispatched")
PERF_COUNTER(snd_occ_ai, "sound/occ/ai")
PERF_COUNTER(snd_occ_emitter, "sound/occ/emitter")
PERF_COUNTER(snd_occ_tris, "sound/occ/tris")
PERF_GAUGE(snd_delegates, "sound/delegates")
PERF_GAUGE(snd_emitters, "sound/emitters")
PERF_GAUGE(snd_targets_busy, "sound/targets_busy")
PERF_COUNTER(snd_target_steal, "sound/target_steal")
PERF_COUNTER(snd_pcm_cache_bytes, "sound/pcm/cache_bytes")
PERF_COUNTER(snd_pcm_decode_bytes, "sound/pcm/decode_bytes")
PERF_TIMER(snd_decode_main, "sound/decode/main")
PERF_TIMER(snd_decode_game, "sound/decode/game")
PERF_TIMER(snd_decode_worker, "sound/decode/worker")
PERF_COUNTER(snd_pcm_reject_len, "sound/pcm/reject_len")
PERF_COUNTER(snd_pcm_reject_budget, "sound/pcm/reject_budget")
PERF_COUNTER(snd_pcm_accept, "sound/pcm/accept")
PERF_GAUGE(snd_pcm_cached_kb, "sound/pcm/cached_kb")
PERF_COUNTER(snd_ogg_open, "sound/ogg/open")
PERF_COUNTER(snd_ogg_open_lazy, "sound/ogg/open_lazy")
PERF_COUNTER(snd_ogg_open_cache, "sound/ogg/open_cache")

// Scheduler / AI (§4)
PERF_COUNTER(shed_steps, "shed/steps")
PERF_COUNTER(shed_pop_stale, "shed/pop_stale")
PERF_COUNTER(shed_exit_limit, "shed/exit_limit")
PERF_COUNTER(shed_exit_time, "shed/exit_time")
PERF_COUNTER(shed_exit_empty, "shed/exit_empty")
PERF_GAUGE(shed_budget, "shed/budget")
PERF_GAUGE(shed_current, "shed/ps_current")
PERF_GAUGE(shed_items, "shed/items")
PERF_GAUGE(shed_items_rt, "shed/items_rt")
PERF_GAUGE(shed_behind, "shed/behind")
PERF_COUNTER(shed_late_0_10, "shed/late/0_10")
PERF_COUNTER(shed_late_10_50, "shed/late/10_50")
PERF_COUNTER(shed_late_50_100, "shed/late/50_100")
PERF_COUNTER(shed_late_100_250, "shed/late/100_250")
PERF_COUNTER(shed_late_250_500, "shed/late/250_500")
PERF_COUNTER(shed_late_500_1000, "shed/late/500_1000")
PERF_COUNTER(shed_late_1000p, "shed/late/1000p")
PERF_TIMER(shed_cat_stalker, "shed/cat/stalker")
PERF_TIMER(shed_cat_monster, "shed/cat/monster")
PERF_TIMER(shed_cat_actor, "shed/cat/actor")
PERF_TIMER(shed_cat_item, "shed/cat/item")
PERF_TIMER(shed_cat_other, "shed/cat/other")
// Lateness by category (sum of "late" ms + a call count, since late is already milliseconds - not a tick
// duration PERF_TIMER could time itself): average lateness per category = *_late_ms / *_late_n.
PERF_COUNTER(shed_late_cat_stalker_ms, "shed/late_cat/stalker_ms")
PERF_COUNTER(shed_late_cat_stalker_n, "shed/late_cat/stalker_n")
PERF_COUNTER(shed_late_cat_monster_ms, "shed/late_cat/monster_ms")
PERF_COUNTER(shed_late_cat_monster_n, "shed/late_cat/monster_n")
PERF_COUNTER(shed_late_cat_actor_ms, "shed/late_cat/actor_ms")
PERF_COUNTER(shed_late_cat_actor_n, "shed/late_cat/actor_n")
PERF_COUNTER(shed_late_cat_item_ms, "shed/late_cat/item_ms")
PERF_COUNTER(shed_late_cat_item_n, "shed/late_cat/item_n")
PERF_COUNTER(shed_late_cat_other_ms, "shed/late_cat/other_ms")
PERF_COUNTER(shed_late_cat_other_n, "shed/late_cat/other_n")
PERF_GAUGE(vision_entries, "ai/vision/entries")
PERF_GAUGE(vision_rays, "ai/vision/rays")
PERF_COUNTER(vision_parallel, "ai/vision/parallel")
PERF_COUNTER(vision_serial, "ai/vision/serial")
PERF_COUNTER(vision_dyn_ray, "ai/vision/dyn_ray")
PERF_COUNTER(vision_dyn_cache, "ai/vision/dyn_cache")
PERF_TIMER(ai_path_search, "ai/path/search")
PERF_COUNTER(ai_path_ok, "ai/path/ok")
// Index-graph search above (3 CGraphEngine::search overloads); the shared_str-graph overload is timed apart,
// since it is a structurally different search (CStringAlgorithm) the plan wants broken out "by kind".
PERF_TIMER(ai_path_search_string, "ai/path/search_string")
PERF_COUNTER(ai_path_ok_string, "ai/path/ok_string")
PERF_TIMER(ai_goap_solve, "ai/goap/solve")
PERF_COUNTER(path_builder_queued, "ai/path_builder/queued")
PERF_COUNTER(path_builder_done, "ai/path_builder/done")
PERF_COUNTER(path_builder_delayed, "ai/path_builder/delayed")
PERF_COUNTER(binder_skip_gate, "ai/binder/skip_gate")
PERF_COUNTER(binder_skip_throttle, "ai/binder/skip_throttle")
PERF_GAUGE(lua_gc_kb, "lua/gc_kb")
PERF_TIMER(lua_gc_step, "lua/gc_step")
// P18 (plans/optimization_mt/13-lua-spikes.md), xrGame/lua_spike_probe.cpp, only with npc_perf_lua_spike_us > 0:
// spike = a probed Lua entry point (outermost) over the threshold, calls = their count, max_frame ~ the longest;
// spike_gc = those with the GC inside (atomic phase or the heap shrank by >= 1 MB); gc_atomic = atomic phases seen,
// gc_atomic_in_step = of them inside CLevel::script_gc; gc_atomic_step = the one GC step with the atomic phase
// (the paced GC of lua_gc_method gc_step: its time is the atomic phase itself); gc_paced_steps = single GC steps of
// the paced GC (counted even when the probe is off).
PERF_TIMER(lua_spike, "lua/spike")
PERF_TIMER(lua_spike_gc, "lua/spike_gc")
PERF_COUNTER(lua_gc_atomic, "lua/gc_atomic")
PERF_COUNTER(lua_gc_atomic_in_step, "lua/gc_atomic_in_step")
PERF_TIMER(lua_gc_atomic_step, "lua/gc_atomic_step")
PERF_COUNTER(lua_gc_paced_steps, "lua/gc_paced_steps")
PERF_COUNTER(alife_objects, "alife/objects")
PERF_COUNTER(alife_switches, "alife/switches")
PERF_TIMER(alife_update, "alife/update")
PERF_COUNTER(events_emit, "events/emit")
PERF_COUNTER(events_emit_subs, "events/emit_subs")
PERF_TIMER(events_dispatch_lua, "events/dispatch_lua")
PERF_TIMER(events_dispatch_native, "events/dispatch_native")

// Spatial DB (§5). Caller buckets (physics/sound/nearest/vision/actor/render/ray/rain/explode/other) are each
// their own named PERF_COUNTER, dispatched by an explicit switch (ISpatial.h: SpatialIncQBox/QSphere/QFrustum/
// QRay) - not AddHist offset arithmetic, so enum reordering here cannot silently misattribute a bucket.
// db0/db1 bucket by ISpatial_DB::db_index (0 = SpatialSpace, 1 = SpatialSpacePhysic; see ISpatial.cpp ctor).
PERF_COUNTER(spatial_q_box, "spatial/q_box")
PERF_COUNTER(spatial_q_box_physics, "spatial/q_box/physics")
PERF_COUNTER(spatial_q_box_sound, "spatial/q_box/sound")
PERF_COUNTER(spatial_q_box_nearest, "spatial/q_box/nearest")
PERF_COUNTER(spatial_q_box_vision, "spatial/q_box/vision")
PERF_COUNTER(spatial_q_box_actor, "spatial/q_box/actor")
PERF_COUNTER(spatial_q_box_render, "spatial/q_box/render")
PERF_COUNTER(spatial_q_box_ray, "spatial/q_box/ray")
PERF_COUNTER(spatial_q_box_rain, "spatial/q_box/rain")
PERF_COUNTER(spatial_q_box_explode, "spatial/q_box/explode")
PERF_COUNTER(spatial_q_box_other, "spatial/q_box/other")
PERF_COUNTER(spatial_q_box_db0, "spatial/q_box/db0")
PERF_COUNTER(spatial_q_box_db1, "spatial/q_box/db1")
PERF_TIMER(spatial_q_box_time, "spatial/q_box_time")
PERF_COUNTER(spatial_q_box_results, "spatial/q_box/results")
PERF_TIMER(spatial_q_sphere_time, "spatial/q_sphere_time")
PERF_COUNTER(spatial_q_sphere, "spatial/q_sphere")
PERF_COUNTER(spatial_q_sphere_physics, "spatial/q_sphere/physics")
PERF_COUNTER(spatial_q_sphere_sound, "spatial/q_sphere/sound")
PERF_COUNTER(spatial_q_sphere_nearest, "spatial/q_sphere/nearest")
PERF_COUNTER(spatial_q_sphere_vision, "spatial/q_sphere/vision")
PERF_COUNTER(spatial_q_sphere_actor, "spatial/q_sphere/actor")
PERF_COUNTER(spatial_q_sphere_render, "spatial/q_sphere/render")
PERF_COUNTER(spatial_q_sphere_ray, "spatial/q_sphere/ray")
PERF_COUNTER(spatial_q_sphere_rain, "spatial/q_sphere/rain")
PERF_COUNTER(spatial_q_sphere_explode, "spatial/q_sphere/explode")
PERF_COUNTER(spatial_q_sphere_other, "spatial/q_sphere/other")
PERF_COUNTER(spatial_q_sphere_db0, "spatial/q_sphere/db0")
PERF_COUNTER(spatial_q_sphere_db1, "spatial/q_sphere/db1")
PERF_COUNTER(spatial_q_sphere_results, "spatial/q_sphere/results")
PERF_TIMER(spatial_q_frustum_time, "spatial/q_frustum_time")
PERF_COUNTER(spatial_q_frustum, "spatial/q_frustum")
PERF_COUNTER(spatial_q_frustum_physics, "spatial/q_frustum/physics")
PERF_COUNTER(spatial_q_frustum_sound, "spatial/q_frustum/sound")
PERF_COUNTER(spatial_q_frustum_nearest, "spatial/q_frustum/nearest")
PERF_COUNTER(spatial_q_frustum_vision, "spatial/q_frustum/vision")
PERF_COUNTER(spatial_q_frustum_actor, "spatial/q_frustum/actor")
PERF_COUNTER(spatial_q_frustum_render, "spatial/q_frustum/render")
PERF_COUNTER(spatial_q_frustum_ray, "spatial/q_frustum/ray")
PERF_COUNTER(spatial_q_frustum_rain, "spatial/q_frustum/rain")
PERF_COUNTER(spatial_q_frustum_explode, "spatial/q_frustum/explode")
PERF_COUNTER(spatial_q_frustum_other, "spatial/q_frustum/other")
PERF_COUNTER(spatial_q_frustum_db0, "spatial/q_frustum/db0")
PERF_COUNTER(spatial_q_frustum_db1, "spatial/q_frustum/db1")
PERF_COUNTER(spatial_q_frustum_results, "spatial/q_frustum/results")
PERF_TIMER(spatial_q_ray_time, "spatial/q_ray_time")
PERF_COUNTER(spatial_q_ray, "spatial/q_ray")
PERF_COUNTER(spatial_q_ray_physics, "spatial/q_ray/physics")
PERF_COUNTER(spatial_q_ray_sound, "spatial/q_ray/sound")
PERF_COUNTER(spatial_q_ray_nearest, "spatial/q_ray/nearest")
PERF_COUNTER(spatial_q_ray_vision, "spatial/q_ray/vision")
PERF_COUNTER(spatial_q_ray_actor, "spatial/q_ray/actor")
PERF_COUNTER(spatial_q_ray_render, "spatial/q_ray/render")
PERF_COUNTER(spatial_q_ray_ray, "spatial/q_ray/ray")
PERF_COUNTER(spatial_q_ray_rain, "spatial/q_ray/rain")
PERF_COUNTER(spatial_q_ray_explode, "spatial/q_ray/explode")
PERF_COUNTER(spatial_q_ray_other, "spatial/q_ray/other")
PERF_COUNTER(spatial_q_ray_db0, "spatial/q_ray/db0")
PERF_COUNTER(spatial_q_ray_db1, "spatial/q_ray/db1")
PERF_COUNTER(spatial_q_ray_results, "spatial/q_ray/results")
PERF_COUNTER(spatial_lock_shared_contend, "spatial/lock/shared_contend")
PERF_TIMER(spatial_lock_shared_wait, "spatial/lock/shared_wait")
PERF_COUNTER(spatial_lock_unique_contend, "spatial/lock/unique_contend")
PERF_TIMER(spatial_lock_unique_wait, "spatial/lock/unique_wait")
PERF_COUNTER(spatial_move_calls, "spatial/move/calls")
PERF_COUNTER(spatial_move_idle, "spatial/move/idle")
PERF_COUNTER(spatial_move_rebuild, "spatial/move/rebuild")
PERF_TIMER(spatial_move_time, "spatial/move_time")
PERF_COUNTER(nearest_cache_hit, "spatial/nearest/cache_hit")
PERF_COUNTER(nearest_cache_miss, "spatial/nearest/cache_miss")

// Frame / render / crows / bones (§8)
PERF_GAUGE(objects_crows, "objects/crows")
PERF_GAUGE(objects_updated, "objects/updated")
PERF_COUNTER(crow_early_already, "crow/early/already")
PERF_COUNTER(crow_early_processing, "crow/early/processing")
PERF_COUNTER(crow_early_corpse, "crow/early/corpse")
PERF_COUNTER(crow_early_same_frame, "crow/early/same_frame")
PERF_COUNTER(crow_src_main, "crow/src/main")
PERF_COUNTER(crow_src_sun, "crow/src/sun")
PERF_COUNTER(crow_src_light, "crow/src/light")
PERF_COUNTER(crow_src_svp, "crow/src/svp")
PERF_COUNTER(crow_src_shedule, "crow/src/shedule")
PERF_COUNTER(crow_src_ucl_view, "crow/src/ucl_view")
PERF_COUNTER(crow_src_ucl_r30, "crow/src/ucl_r30")
PERF_COUNTER(crow_src_ucl_hom60, "crow/src/ucl_hom60")
PERF_COUNTER(crow_src_spawn, "crow/src/spawn")
PERF_COUNTER(crow_src_setparent, "crow/src/setparent")
PERF_COUNTER(crow_src_ol_act, "crow/src/ol_act")
PERF_COUNTER(crow_src_ol_sleep, "crow/src/ol_sleep")
PERF_COUNTER(crow_src_other, "crow/src/other")
PERF_COUNTER(crow_shadow_only_stalker, "crow/shadow_only/stalker")
PERF_COUNTER(crow_shadow_only_monster, "crow/shadow_only/monster")
PERF_COUNTER(crow_shadow_only_other, "crow/shadow_only/other")
PERF_TIMER(ucl_actor, "ucl/actor")
PERF_TIMER(ucl_stalker, "ucl/stalker")
PERF_TIMER(ucl_monster, "ucl/monster")
PERF_TIMER(ucl_custom_monster, "ucl/custom_monster")
PERF_TIMER(ucl_weapon, "ucl/weapon")
PERF_TIMER(ucl_artefact, "ucl/artefact")
PERF_TIMER(ucl_item, "ucl/item")
PERF_TIMER(ucl_zone, "ucl/zone")
PERF_TIMER(ucl_restrictor, "ucl/restrictor")
PERF_TIMER(ucl_psh, "ucl/psh")
PERF_TIMER(ucl_other, "ucl/other")
PERF_TIMER(dsg_build_subspace, "render/build_subspace")
PERF_COUNTER(render_in, "render/renderables_in")
PERF_COUNTER(render_precomputed, "render/precomputed")
PERF_COUNTER(render_sector_cull, "render/sector_cull")
PERF_COUNTER(render_sphere_miss, "render/sphere_miss")
PERF_COUNTER(render_hom_cull, "render/hom_cull")
PERF_COUNTER(render_shadow_owner_skip, "render/shadow_owner_skip")
PERF_COUNTER(renderable_render, "render/renderable_render")
PERF_COUNTER(render_corpse_smap_skip, "render/corpse_smap_skip")
PERF_COUNTER(render_blob_skip, "render/blob_skip")
PERF_COUNTER(render_blob_add, "render/blob_add")
PERF_COUNTER(render_skel_lod, "render/skel_lod")
PERF_COUNTER(render_skel_full, "render/skel_full")
PERF_COUNTER(render_corpse_pose_skip, "render/corpse_pose_skip")
PERF_COUNTER(render_skin_from_parent, "render/skin_from_parent")
PERF_COUNTER(render_static, "render/static")
// Main-view render breakdown (build vs submit, static vs dynamic): only the main view's dsgraph
// (o.is_main_pass && !o.second_vp_pass) counts here, so sun cascades / lights / scope pass don't mix in.
// Build side (worker task that main waits for in r_main.sync()):
PERF_TIMER(rmain_build_static, "render/main/build_static")      // add_static over traversed sectors (traversal: Tracy dsg_portal_traverse)
PERF_TIMER(rmain_build_dynamic, "render/main/build_dynamic")    // q_frustum..Render_Last (whole dynamic part)
PERF_TIMER(rmain_build_sort, "render/main/build_sort")          // O_ORDERED front-to-back sort of renderables
PERF_TIMER(rmain_build_hom, "render/main/build_hom")            // per-renderable HOM test (+vis writeback)
PERF_TIMER(rmain_build_renderable, "render/main/build_renderable") // renderable_Render: bones, attachments, insert_dynamic
PERF_TIMER(rmain_build_ros, "render/main/build_ros")            // CROS update of view entity + 1 tracked object
PERF_TIMER(rmain_build_query, "render/main/build_query")        // q_frustum over the spatial DB
PERF_TIMER(rmain_build_sector, "render/main/build_sector")      // per renderable: detect_sector + spatial_updatesector
PERF_COUNTER(rmain_sector_detect, "render/main/sector_detect")  // detect_sector calls (2-4 ray queries each)
PERF_TIMER(rmain_build_hud, "render/main/build_hud")            // pHUD->Render_Last (HUD models of the view entity)
// Parallel main build (plans/optimization_mt/08-parallel-main-build.md): the dynamic loop split over task workers.
// build_sector/build_hom/build_renderable above become CPU sums over the workers; the wall time is par_wall.
PERF_TIMER(rmain_par_wall, "render/main/par_wall")              // spawn of the parts .. all parts done (wall time)
PERF_TIMER(rmain_par_part, "render/main/par_part")              // one part (sum over workers = CPU; calls = parts)
PERF_TIMER(rmain_par_tail, "render/main/par_tail")              // build thread out of parts, waits for the last ones
PERF_TIMER(rmain_par_merge, "render/main/par_merge")            // shards -> main dsgraph, lights, tracked ROS
PERF_COUNTER(rmain_par_parts, "render/main/par_parts")
PERF_COUNTER(rmain_par_threads, "render/main/par_threads")      // threads that ran at least one part (build thread too)
PERF_COUNTER(rmain_par_objects, "render/main/par_objects")      // spatial entries split into parts
PERF_COUNTER(rmain_par_fallback, "render/main/par_fallback")    // frames with the cvar on that took the sequential loop
// Submit side (main thread, immediate context) - R_dsgraph_structure::render_graph:
PERF_TIMER(rmain_submit_static, "render/main/submit_static")
PERF_TIMER(rmain_submit_dynamic, "render/main/submit_dynamic")
PERF_TIMER(rmain_dyn_apply_object, "render/main/dyn_apply_object") // per item: CROS update_smooth + hemi/sun constants
PERF_TIMER(rmain_dyn_visual_render, "render/main/dyn_visual_render") // per item: pVisual->Render (skinning upload + draws)
PERF_COUNTER(rmain_static_items, "render/main/static_items")
PERF_COUNTER(rmain_static_passes, "render/main/static_passes")  // set_Pass groups
PERF_COUNTER(rmain_static_draws, "render/main/static_draws")    // backend draw calls (stat.render.calls delta)
PERF_COUNTER(rmain_dyn_passes, "render/main/dyn_passes")
PERF_COUNTER(rmain_dyn_draws, "render/main/dyn_draws")
PERF_COUNTER(rmain_dyn_states, "render/main/dyn_states")        // shader-state changes (stat.states delta)
PERF_COUNTER(rmain_dyn_textures, "render/main/dyn_textures")    // texture binds (stat.textures delta)
// Lighting tracker CROS_impl (every pass/thread that renders dynamics):
PERF_COUNTER(ros_smooth, "ros/smooth")                          // update_smooth that did work (1/object/frame)
PERF_TIMER(ros_update, "ros/update")                            // full recompute: rays + light list
PERF_COUNTER(ros_rays, "ros/rays")                              // ObjectSpace.RayTest calls (hemi + sun + lights)
// Skinned draws, CSkeletonX::_Render (every pass):
PERF_COUNTER(skin_draw_hw, "skin/draw_hw")                      // GPU skinning (RM_SKINNING_*B)
PERF_COUNTER(skin_draw_single, "skin/draw_single")              // rigid, single bone
PERF_COUNTER(skin_draw_soft, "skin/draw_soft")                  // CPU skinning (locks the shared vertex stream)
PERF_COUNTER(skin_bone_rows, "skin/bone_rows")                  // set_ca rows uploaded (3 per bone per draw)
PERF_TIMER(skin_bone_upload, "skin/bone_upload")                // the set_ca loop itself
// P15 (plans/optimization_mt/10-submit-dynamic.md): the same CSkeletonX::_Render draws, but only inside the main view's
// dynamic submit (render_graph, g_perf_rmain_dyn) - the share of render/main/dyn_draws that is skinned, and how often
// the bone palette written for a draw belongs to a model whose palette was already written.
PERF_COUNTER(rmain_skin_hw, "render/main/skin_hw")              // GPU-skinned draws (palette written for each)
PERF_COUNTER(rmain_skin_single, "render/main/skin_single")      // single-bone draws (no palette, W = object * bone)
PERF_COUNTER(rmain_skin_soft, "render/main/skin_soft")          // CPU-skinned draws
PERF_COUNTER(rmain_skin_bone_bytes, "render/main/skin_bone_bytes") // palette bytes written (48 per bone per draw)
PERF_COUNTER(rmain_skin_same_prev, "render/main/skin_same_prev") // hw draw of the same model as the previous hw draw
PERF_COUNTER(rmain_skin_repeat, "render/main/skin_repeat")      // hw draw of a model already drawn this frame (incl. same_prev)
PERF_COUNTER(rmain_skin_models, "render/main/skin_models")      // distinct GPU-skinned models per frame (= hw - repeat)
PERF_COUNTER(rmain_skel_full, "render/main/skel_full")          // skeletons the main-view build added with their full mesh
PERF_COUNTER(rmain_skel_lod, "render/main/skel_lod")            // ...with the character LOD mesh (m_lod)
PERF_COUNTER(bones_calls, "bones/calls")
PERF_COUNTER(bones_fast_out, "bones/fast_out")
PERF_COUNTER(bones_slow_skip, "bones/slow_skip")
PERF_COUNTER(bones_recompute, "bones/recompute")
PERF_COUNTER(bones_forced, "bones/forced")
PERF_COUNTER(bones_visbox, "bones/visbox")
PERF_COUNTER(bones_visbox_parallel, "bones/visbox_parallel")
PERF_COUNTER(bones_callback, "bones/callback")
PERF_COUNTER(bones_lock_contend, "bones/lock_contend")
PERF_TIMER(bones_lock_wait, "bones/lock_wait")
PERF_TIMER(bones_lock_hold, "bones/lock_hold")
// Per-model lock (phase B of CalculateBones): wait/contention, and recomputes skipped because TryEnter failed
// (this thread already held another model's lock or the global one - deadlock avoidance).
PERF_COUNTER(bones_model_lock_contend, "bones/model_lock_contend")
PERF_TIMER(bones_model_lock_wait, "bones/model_lock_wait")
PERF_COUNTER(bones_lock_try_fail, "bones/lock_try_fail")
PERF_TIMER(bones_on_calc, "bones/on_calc")
PERF_TIMER(bones_caller_eye_pp, "bones/caller/eye_pp")
PERF_TIMER(bones_caller_ccf, "bones/caller/ccf")
PERF_TIMER(bones_caller_weapon, "bones/caller/weapon")
PERF_TIMER(bones_caller_torch, "bones/caller/torch")
PERF_TIMER(bones_caller_other, "bones/caller/other")
// Stage 0 of the render main-view plan: who recomputes bones and who waits for UCalc_Mutex.
// bones_recompute split by the PERF_PASS tag of the calling thread (other = no render pass: game code).
PERF_COUNTER(bones_recompute_main, "bones/recompute/main")
PERF_COUNTER(bones_recompute_smap, "bones/recompute/smap")      // sun cascades, lights, rain, svp shadows
PERF_COUNTER(bones_recompute_svp, "bones/recompute/svp")
PERF_COUNTER(bones_recompute_other, "bones/recompute/other")
PERF_COUNTER(bones_recompute_prepass, "bones/recompute/prepass") // bones prepass phase B (pass::Prepass)
// bones_lock_wait / bones_lock_hold split by thread kind (main / GameThread / TaskManager or other worker).
PERF_TIMER(bones_lock_wait_main, "bones/lock_wait/main")
PERF_TIMER(bones_lock_wait_game, "bones/lock_wait/game")
PERF_TIMER(bones_lock_wait_worker, "bones/lock_wait/worker")
PERF_TIMER(bones_lock_hold_main, "bones/lock_hold/main")
PERF_TIMER(bones_lock_hold_game, "bones/lock_hold/game")
PERF_TIMER(bones_lock_hold_worker, "bones/lock_hold/worker")
// What the lock is held for: UpdateTracks = bones/on_calc (existing); the bone math itself (Visibility_Update +
// Bone_Calculate + visbox) and the game-side Update_Callback are timed separately.
PERF_TIMER(bones_bone_calc, "bones/bone_calc")
PERF_TIMER(bones_update_cb, "bones/update_cb")
// Bones prepass (always, plans/optimization_mt/04-bones-prepass.md): before the render, the bones of
// the models the render computed in the previous frame - phase A (UpdateTracks) on main, phase B on the workers.
PERF_TIMER(prepass_time, "prepass/time")                        // wall time on main (between game_wait and render)
PERF_TIMER(prepass_phase_a, "prepass/phase_a")                  // main: collect candidates + UpdateTracks
PERF_TIMER(prepass_phase_b, "prepass/phase_b")                  // bone math + callbacks, CPU summed over the workers
PERF_COUNTER(prepass_noted, "prepass/noted")                    // objects the render drew last frame (any pass)
PERF_COUNTER(prepass_candidates, "prepass/candidates")          // ...whose bones the render computed (BonesPrepass_Wanted)
PERF_COUNTER(prepass_computed, "prepass/computed")              // ...recomputed by the prepass
PERF_COUNTER(prepass_cooldown, "prepass/cooldown")              // rendered, but skipped after a recent miss
// A recompute of bones the prepass already computed this frame (they were invalidated after it): by the render
// (miss_render: render recompute after prepass) or by game code (miss_other). Each miss parks the model for a while.
PERF_COUNTER(prepass_miss_render, "prepass/miss_render")
PERF_COUNTER(prepass_miss_other, "prepass/miss_other")
// Render prepare of held items (always, plans/optimization_mt/05-render-prepare-weapons.md): what renderable_Render of weapons and other held items used to
// write in every render pass (XFORM, shot light, weapon bones) is done once per frame inside the bones prepass.
PERF_TIMER(prepare_time, "prepare/time")                        // main: phase C (items' XFORM + shot light)
PERF_COUNTER(prepare_items, "prepare/items")                    // held / dropped items prepared (CHudItem)
PERF_COUNTER(prepare_light, "prepare/light")                    // ...of them with a shot light positioned
PERF_COUNTER(prepare_addon_models, "prepare/addon_models")      // weapon addon models handed to the bones prepass
// What the render still does itself (any pass, counted per renderable_Render call): an item not prepared this frame
// ran UpdateXForm, an attachment-system weapon invalidated and recomputed its bones. With the prepare on, both
// should be close to 0 (new in the frame, inventory icons).
PERF_COUNTER(prepare_render_xform, "prepare/render_xform")
PERF_COUNTER(prepare_render_weapon_bones, "prepare/render_weapon_bones")
// Inventory items handing their own world model to the render (plans/optimization_mt/07-item-single-render.md), any
// pass, counted per add_Visual: once per item drawn (CGameObject branch only).
PERF_COUNTER(item_render_add_visual, "render/item/add_visual")
// DX11 constant buffers: every dx11ConstantBuffer::Flush that actually maps (Map WRITE_DISCARD + copy + Unmap).
PERF_TIMER(cb_flush, "cb/flush")
PERF_COUNTER(cb_flush_bytes, "cb/flush_bytes")
PERF_COUNTER(cb_write_same, "cb/write_same")                    // sets skipped because the bytes were unchanged
// ...and the share of them issued by the main view's render_graph (main thread, immediate context).
PERF_COUNTER(rmain_static_cb_flush, "render/main/static_cb_flush")
PERF_COUNTER(rmain_dyn_cb_flush, "render/main/dyn_cb_flush")
PERF_COUNTER(rmain_dyn_cb_bytes, "render/main/dyn_cb_bytes")
// P15: render/main/dyn_cb_flush split by buffer (dx11ConstantBuffer name): per-object transforms + hemi/material,
// $Globals (in skinned vertex shaders - the sbones_array palette), everything else; bytes actually copied.
PERF_COUNTER(rmain_dyn_cb_transforms, "render/main/dyn_cb_transforms") // cbuffer dynamic_transforms
PERF_COUNTER(rmain_dyn_cb_globals, "render/main/dyn_cb_globals")       // cbuffer $Globals
PERF_COUNTER(rmain_dyn_cb_globals_bytes, "render/main/dyn_cb_globals_bytes")
PERF_COUNTER(rmain_dyn_cb_other, "render/main/dyn_cb_other")
PERF_COUNTER(rmain_dyn_cb_other_bytes, "render/main/dyn_cb_other_bytes")
// Partial palette upload: flushes that copied only the used prefix of the bone palette (any pass), and bytes not copied.
PERF_COUNTER(cb_flush_partial, "cb/flush_partial")
PERF_COUNTER(cb_flush_partial_saved, "cb/flush_partial_saved")
// P15b (shared cbuffers): *SSetConstantBuffers calls in CBackend::set_Constants (any pass / main view's dynamic submit).
PERF_COUNTER(cb_bind, "cb/bind")
PERF_COUNTER(rmain_dyn_cb_bind, "render/main/dyn_cb_bind")
PERF_COUNTER(tracks_calls, "tracks/calls")
PERF_COUNTER(tracks_early, "tracks/early")
PERF_COUNTER(tracks_cb, "tracks/callback")
PERF_COUNTER(tracks_ll, "tracks/ll")
PERF_COUNTER(blend_cb_offlogic_svp, "bones/blend_cb/svp")
PERF_COUNTER(blend_cb_offlogic_main, "bones/blend_cb/main")
PERF_COUNTER(blend_cb_offlogic_worker, "bones/blend_cb/worker")
// sphere.R histogram of alive stalkers/monsters, both radius sources (visual vs spatial sphere), each
// split by class, per §8.6: "Брать оба радиуса ... сталкеры и монстры раздельно". gt1 is IXray's R>1 cull
// rule reference fraction, not an 8th bucket - it overlaps 1_15/15_2/2_3/3p by design (same >=1.0 test).
PERF_COUNTER(npc_r_vis_stlk_0_05, "npc/r_vis/stalker/0_05")
PERF_COUNTER(npc_r_vis_stlk_05_1, "npc/r_vis/stalker/05_1")
PERF_COUNTER(npc_r_vis_stlk_1_15, "npc/r_vis/stalker/1_15")
PERF_COUNTER(npc_r_vis_stlk_15_2, "npc/r_vis/stalker/15_2")
PERF_COUNTER(npc_r_vis_stlk_2_3, "npc/r_vis/stalker/2_3")
PERF_COUNTER(npc_r_vis_stlk_3p, "npc/r_vis/stalker/3p")
PERF_COUNTER(npc_r_vis_stlk_gt1, "npc/r_vis/stalker/gt1")
PERF_COUNTER(npc_r_vis_mon_0_05, "npc/r_vis/monster/0_05")
PERF_COUNTER(npc_r_vis_mon_05_1, "npc/r_vis/monster/05_1")
PERF_COUNTER(npc_r_vis_mon_1_15, "npc/r_vis/monster/1_15")
PERF_COUNTER(npc_r_vis_mon_15_2, "npc/r_vis/monster/15_2")
PERF_COUNTER(npc_r_vis_mon_2_3, "npc/r_vis/monster/2_3")
PERF_COUNTER(npc_r_vis_mon_3p, "npc/r_vis/monster/3p")
PERF_COUNTER(npc_r_vis_mon_gt1, "npc/r_vis/monster/gt1")
PERF_COUNTER(npc_r_spt_stlk_0_05, "npc/r_spatial/stalker/0_05")
PERF_COUNTER(npc_r_spt_stlk_05_1, "npc/r_spatial/stalker/05_1")
PERF_COUNTER(npc_r_spt_stlk_1_15, "npc/r_spatial/stalker/1_15")
PERF_COUNTER(npc_r_spt_stlk_15_2, "npc/r_spatial/stalker/15_2")
PERF_COUNTER(npc_r_spt_stlk_2_3, "npc/r_spatial/stalker/2_3")
PERF_COUNTER(npc_r_spt_stlk_3p, "npc/r_spatial/stalker/3p")
PERF_COUNTER(npc_r_spt_stlk_gt1, "npc/r_spatial/stalker/gt1")
PERF_COUNTER(npc_r_spt_mon_0_05, "npc/r_spatial/monster/0_05")
PERF_COUNTER(npc_r_spt_mon_05_1, "npc/r_spatial/monster/05_1")
PERF_COUNTER(npc_r_spt_mon_1_15, "npc/r_spatial/monster/1_15")
PERF_COUNTER(npc_r_spt_mon_15_2, "npc/r_spatial/monster/15_2")
PERF_COUNTER(npc_r_spt_mon_2_3, "npc/r_spatial/monster/2_3")
PERF_COUNTER(npc_r_spt_mon_3p, "npc/r_spatial/monster/3p")
PERF_COUNTER(npc_r_spt_mon_gt1, "npc/r_spatial/monster/gt1")
PERF_TIMER(ui_dialog_game, "ui/dialog/game")
PERF_TIMER(ui_dialog_menu, "ui/dialog/menu")
PERF_TIMER(ui_script_wnd, "ui/script_wnd")
PERF_COUNTER(ui_script_wnd_calls, "ui/script_wnd/calls")
PERF_COUNTER(ui_script_wnd_ex, "ui/script_wnd/exceptions")
PERF_GAUGE(hw_imm_foreign, "hw/imm_foreign")

// Hot spots (§7)
PERF_COUNTER(ol_destroy, "objects/destroy")
PERF_COUNTER(ol_destroy_active, "objects/destroy_active")
PERF_COUNTER(ol_destroy_sleeping, "objects/destroy_sleeping")
PERF_COUNTER(ol_destroy_cb, "objects/destroy_callbacks")
// P17 (plans/optimization_mt/12-relcase.md): the destroy phase of CObjectList::Update by step, calls = frames with a destroy.
// The per-class cost and what was destroyed (section, origin) go to the log as "* relcase ..." every 5 s (log bit).
PERF_TIMER(ol_relcase_act, "objects/relcase/active")           // net_RelcaseBatch of active objects
PERF_TIMER(ol_relcase_sleep, "objects/relcase/sleeping")       // the same for sleeping objects
PERF_TIMER(ol_relcase_sound, "objects/relcase/sound")          // ISoundScene::object_relcase
PERF_TIMER(ol_relcase_cb, "objects/relcase/callbacks")         // m_relcase_callbacks (Feel::Touch, Feel::Vision...) + HUD
PERF_TIMER(ol_relcase_net, "objects/relcase/net_destroy")      // net_Destroy + Destroy of the destroyed objects
PERF_COUNTER(ol_relcase_cb_calls, "objects/relcase/cb_calls")  // callback calls (a batch callback counts once)
PERF_COUNTER(ol_relcase_hud_calls, "objects/relcase/hud_calls") // CCustomHUD::net_Relcase calls
// xrGame: containers shared by several objects. Group-owned visual/sound/hit memory lists (one per team/squad/group,
// pointed to by every member) and the agent manager of a group: the first member cleans them of the whole batch.
PERF_COUNTER(relcase_shared_items, "objects/relcase/shared_items")   // elements walked in group-owned memory lists
PERF_COUNTER(relcase_shared_purges, "objects/relcase/shared_purges") // such lists cleaned of the whole batch at once
PERF_COUNTER(relcase_shared_skips, "objects/relcase/shared_skips")   // calls that found their list already cleaned
PERF_COUNTER(relcase_agent_runs, "objects/relcase/agent_runs")       // CAgentManager::remove_links runs
PERF_COUNTER(relcase_agent_members, "objects/relcase/agent_members") // agent members walked by them
PERF_COUNTER(relcase_agent_skips, "objects/relcase/agent_skips")     // member calls whose agent was already cleaned
// P17b (12-relcase.md, "P17b"): frames with a destroy (avg destroyed per such frame = objects/destroy / this) and the
// fixed cost of each of them by receiver; the P17b shortcuts are always on.
PERF_COUNTER(ol_relcase_frames, "objects/relcase/frames")            // frames with a destroy phase
PERF_TIMER(relcase_monsters, "objects/relcase/monsters")             // net_RelcaseBatch of CBaseMonster, calls = monsters
PERF_TIMER(relcase_monster_memory, "objects/relcase/monster_memory") // its UpdateMemory after the batch, calls = runs
PERF_COUNTER(relcase_monster_memory_skips, "objects/relcase/monster_memory_skips") // shortcut: batch not in the memories
PERF_TIMER(relcase_cb_vision, "objects/relcase/cb_vision")           // Feel::Vision batch callbacks, calls = subscribers
PERF_COUNTER(relcase_vision_visible_skips, "objects/relcase/vision_visible_skips") // shortcut: feel_visible not walked
PERF_TIMER(relcase_cb_touch, "objects/relcase/cb_touch")             // Feel::Touch batch callbacks, calls = subscribers
PERF_COUNTER(relcase_touch_skips, "objects/relcase/touch_skips")     // shortcut: subscribers with empty lists skipped
PERF_COUNTER(ge_spawn, "ge/spawn")
PERF_COUNTER(ge_event, "ge/event")
PERF_COUNTER(ge_other, "ge/other")
// Total events drained by the single ProcessGameEvents() call this frame - ge_spawn+ge_event+ge_other already
// give this as a sum across the whole 5s window, but the plan explicitly wants a per-frame reading too.
PERF_GAUGE(ge_total_frame, "ge/total_frame")
PERF_COUNTER(explode_hit, "explode/hit")
PERF_COUNTER(explode_rays, "explode/rays")
PERF_GAUGE(particles_batch, "particles/batch")
PERF_TIMER(particles_lock_wait, "particles/lock_wait")
PERF_COUNTER(rain_born, "rain/born")
PERF_COUNTER(rain_raypick, "rain/raypick")
PERF_TIMER(rain_lock_wait, "rain/lock_wait")

// GAME_RENDER_OVERLAP_PLAN P1 locks, added ahead of any real contention (Phase 1/2: atomicity and
// locks only, no dedicated GameThread thread yet) so there is a zero-contention baseline to compare
// against once Phase 3 introduces real overlap.
PERF_COUNTER(modelpool_lock_contend, "modelpool/lock_contend")
PERF_TIMER(modelpool_lock_wait, "modelpool/lock_wait")
PERF_TIMER(modelpool_lock_hold, "modelpool/lock_hold")
PERF_COUNTER(texrepl_lock_contend, "texrepl/lock_contend")
PERF_TIMER(texrepl_lock_wait, "texrepl/lock_wait")
PERF_TIMER(texrepl_lock_hold, "texrepl/lock_hold")
PERF_COUNTER(msgreg_lock_contend, "msgreg/lock_contend")
PERF_TIMER(msgreg_lock_wait, "msgreg/lock_wait")

// Frame breakdown (RENDER_MAIN_VIEW_PLAN, 2026-10-10): where the main-thread frame goes, top-down.
// frame/* = CRenderDevice::ProcessFrame on the main thread, in order; frame/total = all of it except the fps-limit sleep.
PERF_TIMER(frame_total, "frame/total")
PERF_TIMER(frame_before_prerender, "frame/before_prerender")   // seqParallelBeforRender + OnFrameBeforePreRender
PERF_TIMER(frame_environment, "frame/environment")             // CEnvironment::OnFrame
PERF_TIMER(frame_model_clear, "frame/model_clear")             // ModelDeferredClear
PERF_TIMER(frame_move, "frame/frame_move")                     // FrameMove: seqFrame (level OnFrame, objects UpdateCL, HUD...)
PERF_TIMER(frame_camera, "frame/camera_updated")               // OnCameraUpdated
// Game/render overlap frames only: RT scheduler part (the actor) on main before the Game thread is woken.
PERF_TIMER(frame_rt_sheduler, "frame/rt_sheduler")
// main idles until GameThread (scheduler) ends. Game/render overlap frame: wait at the join before the UI
// (inside frame/render, CLevel::OnRender) or at a safety join - not a separate step of the frame there.
PERF_TIMER(frame_game_wait, "frame/game_wait")
PERF_TIMER(frame_render, "frame/render")                       // DoRender, split below
PERF_TIMER(frame_render_seq, "frame/render/seq")               // seqRender: level OnRender (world + scope) + UI
PERF_TIMER(frame_render_stats, "frame/render/stats")
PERF_TIMER(frame_render_imgui, "frame/render/imgui")
PERF_TIMER(frame_render_present, "frame/render/present")       // End + Present: blocks when the GPU is behind
PERF_TIMER(frame_render_viewports, "frame/render/viewports")
PERF_TIMER(frame_prerender_wait, "frame/prerender_wait")       // join of PreRenderThread (particles, rain)
PERF_TIMER(frame_ph_flush, "frame/ph_flush")                   // deferred physics mutations flush
PERF_TIMER(frame_fps_sleep, "frame/fps_sleep")                 // fps-limit sleep (not part of frame/total)
// GameThread (runs while main sits in frame/game_wait).
PERF_TIMER(game_total, "game/total")
PERF_TIMER(game_sound_dispatch, "game/sound_dispatch")
PERF_TIMER(game_sheduler, "game/sheduler")
PERF_TIMER(game_vision_batch, "game/vision_batch")
PERF_TIMER(game_parallel_seq, "game/parallel_seq")
PERF_TIMER(game_seq_frame_mt, "game/seq_frame_mt")
// 06-framemove-offload.md (B4/B5): CLevel::OnGameLogicFrame on the logic thread (both modes), and its two parts.
PERF_TIMER(game_logic_frame, "game/logic_frame")
PERF_TIMER(game_hear_callbacks, "game/hear_callbacks")         // queued NPC sound callbacks
PERF_COUNTER(game_hear_npcs, "game/hear_npcs")                 // NPCs whose sound callbacks ran there
PERF_TIMER(game_level_on_frame, "game/level_on_frame")         // level_on_frame emit
// IGame_Level::OnRender (inside frame/render/seq).
PERF_TIMER(rframe_calculate, "render/frame/calculate")         // main-view Calculate (kicks the async build)
PERF_TIMER(rframe_render, "render/frame/render")               // main-view CRender::Render, split in render/pass/*
PERF_TIMER(rframe_svp_join, "render/frame/svp_join")           // EndSecondVPCalculateParallel
PERF_TIMER(rframe_svp, "render/frame/svp")                     // whole scope pass (tracers, calculate, render)
PERF_TIMER(rframe_svp_calculate, "render/frame/svp_calculate")
PERF_TIMER(rframe_svp_render, "render/frame/svp_render")
// CRender::Render phases of the MAIN pass only (the scope pass is render/frame/svp_render as a whole).
PERF_TIMER(rpass_zprefill_build, "render/pass/zprefill_build")
PERF_TIMER(rpass_sync_point, "render/pass/sync_point")         // q_sync_point.Wait: CPU waits for the GPU
PERF_TIMER(rpass_build_wait, "render/pass/build_wait")         // r_main.sync(): waits for the main-view build
PERF_TIMER(rpass_zprefill_flush, "render/pass/zprefill_flush")
PERF_TIMER(rpass_part0, "render/pass/part0")                   // world geometry submit (render_graph(0), lods, details, hud)
PERF_TIMER(rpass_occq, "render/pass/occq")                     // light occlusion prepare + vis prepare
PERF_TIMER(rpass_part1, "render/pass/part1")                   // split-scene second part (R2FLAG_EXP_SPLIT_SCENE)
PERF_TIMER(rpass_hud_ui, "render/pass/hud_ui")
PERF_TIMER(rpass_wallmarks, "render/pass/wallmarks")
PERF_TIMER(rpass_occq_flush, "render/pass/occq_flush")
PERF_TIMER(rpass_msaa, "render/pass/msaa")
PERF_TIMER(rpass_rain_sync, "render/pass/rain_sync")
PERF_TIMER(rpass_sun, "render/pass/sun")                       // r_sun.sync (cascade build wait) + accumulate
PERF_TIMER(rpass_selfillum, "render/pass/selfillum")
PERF_TIMER(rpass_lights_noocc, "render/pass/lights_noocc")     // render_lights: shadowed lights' smaps + accumulate
PERF_TIMER(rpass_lights_occ, "render/pass/lights_occ")
PERF_TIMER(rpass_combine, "render/pass/combine")               // phase_combine: forward, sorted, distort, postprocess
PERF_TIMER(rpass_tail, "render/pass/tail")                     // after combine (MainScaleEnd, gpu_diag census)
// P19 (plans/optimization_mt/14-spike-instrumentation.md): where the rare FrameMove / Game-thread peaks come from.
// Spikes of the spike logger (perf_spike_us), all sites and threads; per site - "! [perf-spike]" lines in the log.
PERF_COUNTER(perf_spike_hits, "spike/hits")
// CGameObject::UpdateCL stages, the actor apart from all other objects (Tracy 14:08: ucl_CActor -> ucl_CGameObject
// self 20.9 ms). spatial_update includes ai_locations and the spatial rebuild below.
PERF_TIMER(go_ucl_spatial_actor, "game_object/ucl/spatial_update/actor")
PERF_TIMER(go_ucl_spatial_other, "game_object/ucl/spatial_update/other")
PERF_TIMER(go_ucl_crow_actor, "game_object/ucl/crow/actor")
PERF_TIMER(go_ucl_crow_other, "game_object/ucl/crow/other")
PERF_TIMER(go_ucl_matrix_actor, "game_object/ucl/matrix_change/actor")
PERF_TIMER(go_ucl_matrix_other, "game_object/ucl/matrix_change/other")
// CGameObject::spatial_move, the ai-location part (setup_parent_ai_locations / validate_ai_locations ->
// CLevelGraph::vertex), every caller (UpdateCL, physics PhDataUpdate, teleports...)
PERF_TIMER(go_ai_locations_actor, "game_object/ai_locations/actor")
PERF_TIMER(go_ai_locations_other, "game_object/ai_locations/other")
// SpatialBase::spatial_move: restructure of the octree (unique lock wait + remove + insert); count = spatial/move/rebuild
PERF_TIMER(spatial_move_rebuild_time, "spatial/move/rebuild_time")
// CLevelGraph::vertex(current, position) (xrAICore level_graph.cpp, VertexInternal), every caller
PERF_TIMER(lg_vertex, "ai/level_graph/vertex")                  // every call
PERF_TIMER(lg_vertex_full_search, "ai/level_graph/full_search") // current vertex invalid: O(N) over all the vertices
PERF_TIMER(lg_vertex_guess, "ai/level_graph/guess")             // guess_vertex_id: no vertex under the position
PERF_TIMER(lg_vertex_nearest, "ai/level_graph/nearest")         // then the iterative search over the neighbours
// CLevel::OnFrame (main, part of frame/frame_move) by phase; level_on_frame is game/level_on_frame, Lua GC lua/gc_step
PERF_TIMER(lvl_frame_total, "frame/level/total")
PERF_TIMER(lvl_frame_bullets, "frame/level/bullets")            // BulletManager CommitEvents + CommitRenderSet
PERF_TIMER(lvl_frame_client_receive, "frame/level/client_receive")
PERF_TIMER(lvl_frame_game_events, "frame/level/game_events")    // ProcessGameEvents: spawn / destroy / hit... events
PERF_TIMER(lvl_frame_alife_positions, "frame/level/alife_positions") // alife().update_positions_only()
PERF_TIMER(lvl_frame_map_manager, "frame/level/map_manager")
PERF_TIMER(lvl_frame_tasks, "frame/level/update_tasks")         // GameTaskManager().UpdateTasks(): Lua task conditions
PERF_TIMER(lvl_frame_objects, "frame/level/objects")            // IGame_Level::OnFrame: objects UpdateCL + destroy, HUD
PERF_TIMER(lvl_frame_plugin_timers, "frame/level/plugin_timers") // gw::addons::timers::Update
PERF_TIMER(lvl_frame_script_process, "frame/level/script_process") // script_process(Level)->update()
PERF_TIMER(lvl_frame_ph_commander, "frame/level/ph_commander")  // m_ph_commander (engine calls)
PERF_TIMER(lvl_frame_add_call, "frame/level/add_call")          // m_ph_commander_scripts: Lua level.add_call
PERF_TIMER(lvl_frame_level_sounds, "frame/level/level_sounds")  // CLevelSoundManager::Update when not mtLevelSounds
// P19b (plans/optimization_mt/14-spike-instrumentation.md, "FrameMove и актор: разметка для Tracy"): what FrameMove and
// the actor's 8-10 ms of CPU in UpdateCL consist of. Stages of the actor, the HUD and the persistent are also spike sites.
// FrameMove (CRenderDevice::FrameMove): the ImGui frame and every seqFrame subscriber (pure.h call hook, device.cpp)
PERF_TIMER(fm_imgui, "frame/move/imgui")                       // ImGui render Frame + NewFrame and EndFrame (calls = 2)
PERF_TIMER(fm_level, "frame/move/level")                       // CLevel::OnFrame (= frame/level/total)
PERF_TIMER(fm_persistent, "frame/move/persistent")             // CGamePersistent::OnFrame, split in frame/persistent/*
PERF_TIMER(fm_engine, "frame/move/engine")                     // CEngine::OnFrame: engine events (Engine.Event)
PERF_TIMER(fm_sound, "frame/move/sound")                       // SoundProcessor: GEnv.Sound->update
PERF_TIMER(fm_input, "frame/move/input")                       // CInput::OnFrame
PERF_TIMER(fm_console, "frame/move/console")                   // CConsole::OnFrame
PERF_TIMER(fm_render, "frame/move/render")                     // CRender::OnFrame
PERF_TIMER(fm_ui, "frame/move/ui")                             // CUI* / CMainMenu subscribers (menu, drag item, tutorial)
PERF_TIMER(fm_other, "frame/move/other")                       // the rest (net processor, editor, load screen...)
// CRenderDevice::OnCameraUpdated runs once per frame and its first call is inside the actor's UpdateCL (CActor::UpdateCL ->
// Device.OnCameraUpdated); frame/camera_updated after FrameMove is then the no-op second call.
PERF_TIMER(frame_camera_render, "frame/camera_updated/render") // GEnv.Render->OnCameraUpdated: HOM task, details task (waits for the previous)
// CGamePersistent::OnFrame
PERF_TIMER(persist_addons, "frame/persistent/addons")          // gw::addons::OnFrame + console::OnFrame (calls = 2)
PERF_TIMER(persist_event_batches, "frame/persistent/event_batches") // gw::addons::events::FlushBatches (batch subscribers of plugins)
PERF_TIMER(persist_weathers, "frame/persistent/weathers")      // WeathersUpdate
PERF_TIMER(persist_dof, "frame/persistent/dof")                // UpdateDof
// CLevel::OnFrame: the parts P19 left unmarked
PERF_TIMER(lvl_frame_feel_deny, "frame/level/feel_deny")       // m_feel_deny.feel_touch_update (spatial query of radius 0)
PERF_TIMER(lvl_frame_game_time, "frame/level/game_time")       // Environment().SetGameTime
PERF_TIMER(lvl_frame_hud, "frame/level/hud")                   // IGame_Level::OnFrame: pHUD->OnFrame (part of frame/level/objects)
// CObjectList::Update (part of frame/level/objects)
PERF_TIMER(ol_upd_prepare, "objects/update/prepare")           // merge crows, copy the workload, reset crow flags (calls = 3)
PERF_TIMER(ol_upd_pre, "objects/update/pre")                   // PreUpdateCL of the workload
PERF_TIMER(ol_upd_single, "objects/update/single")             // SingleUpdate of the workload = all UpdateCL (sum of ucl/*)
PERF_TIMER(ol_upd_post, "objects/update/post")                 // PostUpdateCL of active and sleeping objects (calls = 2)
// UpdateCL of the actor's own items (the weapon in hands, detector, hidden slots...), not counted in ucl/weapon, ucl/item...
PERF_TIMER(ucl_actor_item, "ucl/actor_item")
// CHUDManager::OnFrame (inside frame/level/hud)
PERF_TIMER(hud_cursor_pick, "hud/cursor_pick")                 // CHUDTarget::CursorOnFrame: RayQuery up to the far plane
// CUIGameCustom::OnFrame: once a frame from Device.seqFrame, not inside frame/level/hud (its dialogs: ui/dialog/game)
PERF_TIMER(hud_ui, "hud/ui")
PERF_TIMER(hud_ui_statics, "hud/ui/statics")                   // custom statics: Update, sort, delete
PERF_TIMER(hud_ui_window, "hud/ui/window")                     // Window->Update
PERF_TIMER(hud_ui_main_wnd, "hud/ui/main_wnd")                 // UIMainIngameWnd->Update
PERF_TIMER(hud_ui_messages, "hud/ui/messages")                 // m_pMessagesWnd->Update
// CActor::UpdateCL by stage (their sum is ~ucl/actor)
PERF_TIMER(actor_ucl_inventory, "actor/ucl/inventory")         // UpdateInventoryOwner: inventory().Update, trade, talk
PERF_TIMER(actor_ucl_touch_boxes, "actor/ucl/touch_boxes")     // UpdateObjectBox of the characters in feel_touch
PERF_TIMER(actor_ucl_holder, "actor/ucl/holder")               // m_holder->UpdateEx (vehicle, mounted weapon)
PERF_TIMER(actor_ucl_base, "actor/ucl/base")                   // inherited UpdateCL: CPhysicsShellHolder (particles), CGameObject (game_object/ucl/*)
PERF_TIMER(actor_ucl_ph_support, "actor/ucl/ph_support")       // CCharacterPhysicsSupport::in_UpdateCL
PERF_TIMER(actor_ucl_pickup, "actor/ucl/pickup")               // PickupModeUpdate (USE held): feel_touch_update, rays
PERF_TIMER(actor_ucl_pickup_cod, "actor/ucl/pickup_cod")       // PickupModeUpdate_COD: q_frustum of all collideables, filter, ray
PERF_COUNTER(actor_ucl_pickup_cod_found, "actor/ucl/pickup_cod/found") // objects that q_frustum returned there
PERF_TIMER(actor_ucl_cam, "actor/ucl/cam_update")              // cam_Update: camera collision, camera managers and effectors
PERF_TIMER(actor_ucl_cam_collide, "actor/ucl/cam_collide")     // collide_camera inside it (the ODE shell of the camera)
PERF_TIMER(actor_ucl_camera_updated, "actor/ucl/camera_updated") // Device.OnCameraUpdated (frame/camera_updated/render inside)
PERF_TIMER(actor_ucl_weapon, "actor/ucl/weapon_hud")           // dispersion, crosshair, SecondVP, HUD overlay, shader constants
PERF_TIMER(actor_ucl_messages, "actor/ucl/messages")           // UpdateDefferedMessages
PERF_TIMER(actor_ucl_steps, "actor/ucl/steps")                 // CStepManager::update: step sounds and particles
PERF_TIMER(actor_ucl_effects, "actor/ucl/effects")             // sound shock effector, controller feedback
PERF_TIMER(actor_ucl_player_hud, "actor/ucl/player_hud")       // g_player_hud[0/1]->update, split in actor/hud/*
// player_hud::update (two calls per frame: g_player_hud[0] and [1])
PERF_TIMER(actor_hud_inertion, "actor/hud/inertion")           // update_inertion
PERF_TIMER(actor_hud_additional, "actor/hud/additional")       // update_additional (aim offsets)
PERF_TIMER(actor_hud_hands, "actor/hud/hands_bones")           // 3 hand models: UpdateTracks + CalculateBones
PERF_TIMER(actor_hud_item, "actor/hud/item_bones")             // attachable_hud_item::update: transforms, 3 item models' bones
PERF_TIMER(actor_hud_laser, "actor/hud/laser_dots")            // CWeapon::UpdateLaserDots
// CCameraManager::Update (the actor's and the level's camera managers)
PERF_TIMER(cam_effectors, "camera/effectors")                  // UpdateCamEffectors
PERF_TIMER(cam_pp_effectors, "camera/pp_effectors")            // UpdatePPEffectors
// CActor::shedule_Update by stage (RT scheduler: on main in overlap frames - frame/rt_sheduler; sequential - Game)
PERF_TIMER(actor_shed_controls, "actor/shed/controls")         // g_cl_CheckControls
PERF_TIMER(actor_shed_physics, "actor/shed/physics")           // g_cl_Orientate, g_Orientate, g_Physics (the character body)
PERF_TIMER(actor_shed_mstate, "actor/shed/mstate_anim")        // g_cl_ValidateMState + g_SetAnimation
PERF_TIMER(actor_shed_feel_touch, "actor/shed/feel_touch")     // feel_touch_update (spatial query around the actor)
PERF_TIMER(actor_shed_grenade, "actor/shed/feel_grenade")      // Feel_Grenade_Update (GetNearest, HUD marks)
PERF_TIMER(actor_shed_base, "actor/shed/base")                 // inherited shedule_Update: conditions ... CGameObject (Lua binder)
PERF_TIMER(actor_shed_sounds, "actor/shed/sounds")             // bobbing effector, heavy breath / blood / danger sounds
PERF_TIMER(actor_shed_look_at, "actor/shed/look_at")           // what the actor looks at (the HUD ray result), tip text
PERF_TIMER(actor_shed_artefacts, "actor/shed/artefacts")       // UpdateArtefactsOnBeltAndOutfit
PERF_TIMER(actor_shed_ph_support, "actor/shed/ph_support")     // in_shedule_Update + Check_for_AutoPickUp
// xrCore log (AddOne): every log line. It calls OutputDebugString: with a debugger attached (the game started from
// Visual Studio with F5) each line is a debug event that suspends ALL threads of the process until the debugger continues.
PERF_COUNTER(log_lines, "log/lines")
PERF_TIMER(log_add, "log/add")                                 // the whole AddOne: logCS wait, OutputDebugString, console callback, file
PERF_GAUGE(log_debugger, "log/debugger_attached")              // 1 = IsDebuggerPresent()
// -npc_cpp_profile: its 5-second report (flush_snapshots + plugin profile stages, hundreds of log lines). The first
// profiled scope that ends after the 5 s mark prints it - in the 15:09 session always inside the actor's UpdateCL.
PERF_TIMER(npc_profile_flush, "log/npc_cpp_profile_flush")
// P20 (plans/optimization_mt/15-ini-cache.md): ltx parsing while the game runs. CInifile::Load, the outermost call only
// (a file or a text with all its #include files): calls = parsed CInifile objects, time = the parse with the includes.
// The Tracy zone CInifile::Load counts the includes as calls of their own: its count = ini/load calls + ini/load_include.
PERF_TIMER(ini_load, "ini/load")
PERF_COUNTER(ini_load_include, "ini/load_include")             // #include files parsed inside them
PERF_COUNTER(ini_destroy, "ini/destroy")                       // CInifile destroyed (any, also those made before)
// ini/load by the code that created the CInifile (xray::ini_stats::Layer, INI_SOURCE)
PERF_TIMER(ini_load_engine, "ini/load/engine")                 // not tagged: engine configs, model user data...
PERF_TIMER(ini_load_lua_file, "ini/load/lua_file")             // Lua ini_file(path), ini_file_ex
PERF_TIMER(ini_load_lua_string, "ini/load/lua_string")         // Lua create_ini_file(text)
PERF_TIMER(ini_load_obj_custom_data, "ini/load/obj_custom_data") // CGameObject::net_Spawn: custom data, object online
PERF_TIMER(ini_load_se_custom_data, "ini/load/se_custom_data") // CSE_Abstract::spawn_ini (lazy, once per server object)
PERF_TIMER(ini_load_spawn_supplies, "ini/load/spawn_supplies") // CSE_ALifeObject::spawn_supplies: custom data, new object
PERF_TIMER(ini_load_trader_custom_data, "ini/load/trader_custom_data") // set_specific_character: custom data of an NPC
PERF_TIMER(ini_load_plugin, "ini/load/plugin")                 // native addon ini_open
// utils.ini_cached (Lua): a call answered by the cached ini_file object, without a parse.
// The misses are the ini/load/lua_file parses whose caller in the [ini-load] table is utils.script.
PERF_COUNTER(ini_lua_cache_hit, "ini/lua_cache_hit")
// Spawn, stage 0 (plans/spawn_optimization/SPEC.md): the client materialization of an M_SPAWN (CLevel::g_sv_Spawn, main,
// inside frame/level/game_events; ge/spawn = their count). calls = spawns or calls of the step. Each step below is also a
// Tracy zone and a perf_spike site of the same name. Per class of the object: "* [spawn] ..." every 5 s (log bit).
PERF_TIMER(spawn_total, "spawn/total")                         // g_sv_Spawn as a whole (the steps below are inside it)
PERF_COUNTER(spawn_loading, "spawn/loading")                   // of them while loading (loading screen / precache frames)
PERF_TIMER(spawn_client_cse, "spawn/client_cse")               // cl_Process_Spawn: client copy of the CSE (F_entity_Create + Spawn_Read)
PERF_TIMER(spawn_create, "spawn/create")                       // Objects.Create: NEW_INSTANCE + Load(section) + PostLoad
PERF_TIMER(spawn_create_new, "spawn/create/new_instance")      // NEW_INSTANCE (constructors of the class chain)
PERF_TIMER(spawn_create_load, "spawn/create/load")             // IGameObject::Load(section)
PERF_TIMER(spawn_create_post_load, "spawn/create/post_load")   // IGameObject::PostLoad(section)
PERF_TIMER(spawn_net_spawn, "spawn/net_spawn")                 // O->net_Spawn, the whole class chain
PERF_TIMER(spawn_client_cb, "spawn/client_callback")           // client_spawn_manager().callback (Lua)
PERF_TIMER(spawn_parent_ownership, "spawn/parent_ownership")   // GE_OWNERSHIP_TAKE to the parent, synchronous (inventory, Lua)
PERF_TIMER(spawn_game_on_spawn, "spawn/game_on_spawn")         // Game().OnSpawn
// CGameObject::net_Spawn by phase (Tracy go_spawn/*); the custom data ini is ini/load/obj_custom_data (P20)
PERF_TIMER(spawn_go_visual, "spawn/go/visual")                 // cNameVisual_set -> model_Create (CModelPool, see modelpool/*)
PERF_TIMER(spawn_go_reload, "spawn/go/reload")                 // reload(section) of the class chain
PERF_TIMER(spawn_go_script_reload, "spawn/go/script_reload")   // Lua binder reload + plugin ObjectBinder Init
PERF_TIMER(spawn_go_reinit, "spawn/go/reinit")                 // reinit() of the class chain
PERF_TIMER(spawn_go_script_reinit, "spawn/go/script_reinit")   // Lua binder reinit + plugin ObjectBinder Reinit
PERF_TIMER(spawn_go_net_load, "spawn/go/net_load")             // net_Load of the client data
PERF_TIMER(spawn_go_ai_location, "spawn/go/ai_location")       // level / game vertex, position on the AI map
PERF_TIMER(spawn_go_cform_spatial, "spawn/go/cform_spatial_shedule") // CForm, spatial_register, shedule_register, processing
PERF_TIMER(spawn_go_crow, "spawn/go/crow")                     // MakeMeCrow
PERF_TIMER(spawn_go_supplies, "spawn/go/spawn_supplies")       // spawn_supplies
PERF_TIMER(spawn_go_script_net_spawn, "spawn/go/script_net_spawn") // Lua binder net_spawn
PERF_TIMER(spawn_go_addon_events, "spawn/go/addon_events")     // bus events *_on_net_spawn + plugin ObjectBinder NetSpawn
// Own part of class net_Spawn (without inherited::net_Spawn)
PERF_TIMER(spawn_cls_physics_holder, "spawn/cls/physics_holder") // CPhysicsShellHolder: shell transform/enable after the chain
PERF_TIMER(spawn_cls_physic_item, "spawn/cls/physic_item")     // CPhysicItem: bones + setup_physic_shell (items on the ground)
PERF_TIMER(spawn_cls_character_physics, "spawn/cls/character_physics") // CCharacterPhysicsSupport::in_NetSpawn (NPC, monsters, actor)
PERF_TIMER(spawn_cls_entity_alive, "spawn/cls/entity_alive")   // CEntityAlive: wound particles
PERF_TIMER(spawn_cls_stalker, "spawn/cls/stalker")             // CAI_Stalker: animations, sight, locations, character physics
PERF_TIMER(spawn_cls_inventory_owner, "spawn/cls/inventory_owner") // CInventoryOwner: trade, inventory, character info
PERF_TIMER(spawn_cls_weapon, "spawn/cls/weapon")               // CWeapon after the chain: state, magazine, addons
PERF_TIMER(spawn_cls_weapon_addons, "spawn/cls/weapon_addons") // of it SpawnDefaultAddons (addon models)
PERF_TIMER(spawn_cls_inventory_item, "spawn/cls/inventory_item") // CInventoryItem
// spawn/total by the category of the server entity (sum = spawn/total)
PERF_TIMER(spawn_cat_stalker, "spawn/cat/stalker")
PERF_TIMER(spawn_cat_monster, "spawn/cat/monster")
PERF_TIMER(spawn_cat_weapon, "spawn/cat/weapon")
PERF_TIMER(spawn_cat_ammo, "spawn/cat/ammo")
PERF_TIMER(spawn_cat_item, "spawn/cat/item")
PERF_TIMER(spawn_cat_other, "spawn/cat/other")
// The queue of game events (CLevel::ProcessGameEvents) and the reuse of an ID inside one pass (SPEC.md 8.6)
PERF_GAUGE(ge_queue_len, "ge/queue_len")                       // events in game_events when ProcessGameEvents starts
PERF_GAUGE(ge_spawn_delay_ms, "ge/spawn_delay_ms")             // max "server sent M_SPAWN -> materialized" of this frame, ms
PERF_COUNTER(ge_unknown_dest, "ge/unknown_dest")               // M_EVENT to an object the client does not have (dropped)
PERF_COUNTER(spawn_id_reuse_deferred, "spawn/id_reuse_deferred") // M_SPAWN found its ID held by an object being destroyed: waits a frame
PERF_GAUGE(spawn_deferred_frames_max, "spawn/deferred_frames_max") // frames the head M_SPAWN has been waiting (0 = none waits)
PERF_COUNTER(spawn_id_reuse_lost, "spawn/id_reuse_lost")       // gave up waiting (the old object was not destroyed): spawn skipped
PERF_COUNTER(spawn_duplicate_skipped, "spawn/duplicate_skipped") // ID held by a live object: spawn skipped
PERF_COUNTER(spawn_parent_missing, "spawn/parent_missing")     // child spawned, but its parent is not on the client
PERF_COUNTER(spawn_failed, "spawn/failed")                     // Objects.Create / net_Spawn failed
// Spawn stage 2: the budget of the materialization per frame (spawn_budget_us > 0, SPEC.md 5.A, section 12). A group =
// a root M_SPAWN + the M_SPAWN of its children right behind it, materialized whole in one frame
PERF_COUNTER(spawn_budget_frames, "spawn/budget_frames")       // frames the budget stopped: groups left for the next frame
PERF_COUNTER(spawn_budget_deferred, "spawn/budget_deferred")   // groups left in the queue by those frames (once per frame)
PERF_GAUGE(spawn_queue_wait_ms, "spawn/queue_wait_ms")         // longest "entered the client queue -> materialized / still waits", ms
PERF_COUNTER(spawn_forced_loading, "spawn/forced/loading")     // groups past the spent budget, no budget: loading screen / precache
PERF_COUNTER(spawn_forced_pause, "spawn/forced/pause")         // ... the game is paused
PERF_COUNTER(spawn_catchup_stopped, "spawn/catchup_stopped")    // ... an old head left for the next frame: spawn_catchup_us spent
PERF_COUNTER(spawn_forced_delay, "spawn/forced/delay")         // ... the head waited spawn_max_delay_ms
PERF_GAUGE(spawn_groups_frame, "spawn/groups_frame")           // groups materialized this frame (any spawn_budget_us)
PERF_GAUGE(spawn_group_size_max, "spawn/group_size_max")       // M_SPAWN of the largest group this frame
// Source of the wave (server, ALife, Game thread or seqParallel)
PERF_COUNTER(alife_online_objects, "alife/switch/online_objects") // CALifeSwitchManager::add_online: an object goes online
PERF_COUNTER(alife_online_items, "alife/switch/online_items")  // + an M_SPAWN per inventory item (add_online_impl)
PERF_GAUGE(alife_switch_pass_spawns, "alife/switch/pass_spawns") // M_SPAWN of the last update_switch pass (max = biggest pass)
// Render resources (CModelPool, CTexture), every caller; *_play = not while loading, *_spawn = inside g_sv_Spawn
PERF_TIMER(modelpool_create, "modelpool/create")               // CModelPool::Create (visuals, particles...)
PERF_COUNTER(modelpool_pool_hit, "modelpool/pool_hit")         // instance taken from Pool (no copy)
PERF_TIMER(modelpool_find, "modelpool/find")                   // Instance_Find: linear search of the base model
PERF_TIMER(modelpool_instance_load, "modelpool/instance_load") // cold load of an ogf (base model not loaded yet)
PERF_TIMER(modelpool_instance_load_play, "modelpool/instance_load_play")
PERF_TIMER(modelpool_instance_load_spawn, "modelpool/instance_load_spawn")
PERF_TIMER(modelpool_duplicate, "modelpool/duplicate")         // Instance_Duplicate: copy of the base model
PERF_TIMER(tex_load_main, "texture/load/main")                 // CTexture::Load on main (any caller)
PERF_TIMER(tex_load_game, "texture/load/game")                 // ... on the Game thread
PERF_TIMER(tex_load_worker, "texture/load/worker")             // ... on workers (DeferredUpload at level load)
PERF_TIMER(tex_create_sync, "texture/create_sync")             // _CreateTexture loading at once (device ready, no deferred load)
PERF_TIMER(tex_create_sync_play, "texture/create_sync_play")
PERF_TIMER(tex_create_sync_spawn, "texture/create_sync_spawn")
PERF_COUNTER(tex_apply_load, "texture/apply_load")             // loaded by its first bind in the render
PERF_TIMER(tex_deferred_upload, "texture/deferred_upload")     // DeferredUpload at level load (wall time)
// Sound sources and ogg files (stage 1: lazy ogg open). sound/ogg/open = every CSoundRender_Source::open call
PERF_TIMER(snd_source_load, "sound/source/load")               // i_create_source miss: LoadWave + PCM cache of a new source
PERF_COUNTER(snd_ogg_open_skipped, "sound/ogg/open_skipped")   // a start of a cached-PCM sound that did not open the ogg
PERF_TIMER(snd_ogg_open_main, "sound/ogg/open_time/main")      // CSoundRender_Source::open by thread
PERF_TIMER(snd_ogg_open_game, "sound/ogg/open_time/game")
PERF_TIMER(snd_ogg_open_worker, "sound/ogg/open_time/worker")
