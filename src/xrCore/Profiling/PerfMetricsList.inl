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
// B2.5: blend events (end callbacks, blend destroy callbacks) a render thread reached while the Game thread ran in
// parallel - queued for main after the join instead of run there (blend_cb/svp|main|worker above count the end
// callbacks among them, by thread); fired by that dispatch; dropped there because the game logic changed the blend
// first (closed, faded, rewound, reused - as if the game logic ran before the render, like a sequential frame).
PERF_COUNTER(blend_cb_deferred, "bones/blend_cb/deferred")
PERF_COUNTER(blend_cb_dispatched, "bones/blend_cb/dispatched")
PERF_COUNTER(blend_cb_stale, "bones/blend_cb/stale")
// UpdateTracks skipped (the time catches up on the next call): the model lock was busy and this thread could only
// TryEnter it (the render's phase A holds the global lock) - normally the game thread changing the model's blends.
PERF_COUNTER(tracks_lock_skip, "tracks/lock_skip")
// An animation mutator (PlayCycle & co., Bone_GetAnimPos) went on without the model lock: this thread held another
// model's lock or the global one and the TryEnter failed (deadlock avoidance; unserialized with the render as before).
PERF_COUNTER(anim_mut_unlocked, "tracks/mut_unlocked")
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
// render/pass/combine split into its steps (plans/optimization_mt/18-combine-pass.md), MAIN pass only (the thread-local
// g_perf_combine_main of R_Backend.h is set around phase_combine), in phase_combine order. The steps add up to
// render/pass/combine minus a few state calls between them.
PERF_TIMER(rcomb_ssao, "render/combine/ssao")                   // phase_hdao / phase_downsamp / phase_ssao
PERF_TIMER(rcomb_sky, "render/combine/sky")                     // RenderSky + RenderClouds
PERF_TIMER(rcomb_quad1, "render/combine/combine_1")             // stencil optimize + combine_1 quad (ambient, sun, fog)
PERF_TIMER(rcomb_forward, "render/combine/forward")             // render_forward: render_graph(1) + faded portals
PERF_TIMER(rcomb_sorted, "render/combine/sorted")               // render_forward: render_sorted (particles, blended)
PERF_TIMER(rcomb_rain, "render/combine/rain_last")              // render_forward: RenderLast (rain, thunderbolts)
PERF_TIMER(rcomb_ppui, "render/combine/ppui")                   // OnRenderPPUI_main
PERF_TIMER(rcomb_volumetric, "render/combine/volumetric")       // phase_combine_volumetric
PERF_TIMER(rcomb_bloom, "render/combine/bloom")                 // phase_bloom (bloom, luminance)
PERF_TIMER(rcomb_distort, "render/combine/distort")             // distortion mask: render_distort (+ SVP deferred flush)
PERF_TIMER(rcomb_quad2, "render/combine/combine_2")             // AA / DOF / mblur quad
PERF_TIMER(rcomb_glows, "render/combine/glows")                 // CGlowManager::Render: occlusion rays + sprites
PERF_TIMER(rcomb_flares, "render/combine/flares")               // lens flares
PERF_TIMER(rcomb_pp, "render/combine/pp")                       // phase_pp (+ depth upscale under r__render_scale)
PERF_COUNTER(rcomb_draws, "render/combine/draws")               // all draw calls of the main combine
PERF_COUNTER(rcomb_forward_draws, "render/combine/forward_draws")
PERF_COUNTER(rcomb_sorted_items, "render/combine/sorted_items") // mapSorted + mapHUDSorted entries drained
PERF_COUNTER(rcomb_sorted_draws, "render/combine/sorted_draws")
PERF_COUNTER(rcomb_distort_items, "render/combine/distort_items")
// Sprite particles drawn by render_sorted (CParticleEffect::Render).
PERF_TIMER(rcomb_ps_effect, "render/combine/ps_effect")         // one effect: VB lock + vertex build + draw, calls = effects
PERF_TIMER(rcomb_ps_fill, "render/combine/ps_fill")             // ParticleRenderStream (with the xr_parallel_for wait)
PERF_COUNTER(rcomb_ps_empty, "render/combine/ps_empty")         // effects reaching the combine with no particles (emptied after the build)
PERF_COUNTER(rcomb_ps_sprites, "render/combine/ps_sprites")     // particles turned into sprites
PERF_COUNTER(rcomb_ps_parallel, "render/combine/ps_parallel")   // effects built by xr_parallel_for (r__particle_render_parallel)
// The dynamic VB (_VertexStream::Lock: the stream mutex + Map) from anything inside the main combine.
PERF_TIMER(rcomb_vb_lock, "render/combine/vb_lock")             // calls = locks
PERF_COUNTER(rcomb_vb_discard, "render/combine/vb_discard")     // locks that wrapped the stream (Map DISCARD)
PERF_COUNTER(rcomb_vb_bytes, "render/combine/vb_bytes")
// CGlowManager::Render (GW glows: torches, searchlights, car lights, LSA lasers).
PERF_COUNTER(rcomb_glows_active, "render/combine/glows_active") // drawable registered glows walked
PERF_TIMER(rcomb_glows_occ, "render/combine/glows_occ")         // occlusion RayTest (rqtBoth) of one glow, calls = glows tested
PERF_COUNTER(rcomb_glows_world, "render/combine/glows_world")   // of them world glows: 5 rays instead of 1
PERF_COUNTER(rcomb_glows_drawn, "render/combine/glows_drawn")
// C1.0 (plans/optimization_mt/17-c1-semi-pipeline.md, §2 and §4): the main thread's time inside DoRender split along the
// future hand-off point. Each moment goes to the innermost PERF_SPLIT scope (PerfMetrics.hpp, ScopedSplit), never twice.
// build = what touches game objects or waits for build tasks: Calculate (lights, HOM wait, icons, task kicks),
// ZPrefill/Build, MainBuildWait, r_rain/r_sun.sync waits, shadowed lights selection and their task waits, SVP build (kick,
// join, BeginSecondViewportRender, post-calculate). submit = everything else of DoRender: draws from the built lists, HUD,
// wallmarks, accumulation, combine, tracers, UI, seqRender subscribers, stats, ImGui, Present. Not counted: the join with
// the Game thread (frame/game_wait). build + submit + frame/game_wait ~ frame/render. calls = time segments.
PERF_TIMER(rsplit_build, "render/split/build")
PERF_TIMER(rsplit_submit, "render/split/submit")
// render_lights, MAIN pass only (as render/pass/*): shadowed point/spot lights (smaps) and the accumulation.
PERF_TIMER(rlights_prep, "render/lights/prep")                 // main: occq visibility, distance/budget cut, smap packing, common q_sphere
PERF_TIMER(rlights_build_cpu, "render/lights/build_cpu")       // build_subspace of one light, any thread (sum = CPU), calls = lights
PERF_TIMER(rlights_record_cpu, "render/lights/record_cpu")     // smap recording of one light: in its task (mt) or in flush on main (serial)
PERF_TIMER(rlights_build_wait, "render/lights/build_wait")     // main waits for a light task (its build + its recording in mt mode)
PERF_TIMER(rlights_execute, "render/lights/execute")           // main after that wait: ExecuteCommandList (mt) / recording (serial), release
PERF_TIMER(rlights_accum, "render/lights/accum")               // main: accumulator, accum_point/spot/volumetric, indirect, unshadowed lights
// Sun cascades and rain, every pass (the scope pass too when it rebuilds them: calls double on scope frames).
PERF_TIMER(rsun_build_cpu, "render/sun/build_cpu")             // build_subspace of one cascade, any thread (sum = CPU), calls = cascades
PERF_TIMER(rsun_record_cpu, "render/sun/record_cpu")           // recording of one cascade (draw task worker, or main inside r_sun.sync)
PERF_TIMER(rsun_wait, "render/sun/wait")                       // main waits in r_sun.sync for the build task and the draw subtask
PERF_TIMER(rsun_flush, "render/sun/flush")                     // main, in r_sun.sync: cascades accumulation (rt arrays), xforms restore
PERF_TIMER(rrain_build_cpu, "render/rain/build_cpu")           // render_rain::calculate (build_subspace inside), any thread
PERF_TIMER(rrain_record_cpu, "render/rain/record_cpu")         // render_rain::render: rain smap recording
PERF_TIMER(rrain_wait, "render/rain/wait")                     // main waits in r_rain.sync
PERF_TIMER(rrain_flush, "render/rain/flush")                   // main, in r_rain.sync: rain smap list submit, wet surfaces accumulation
// Game calls inside the render (main). Calculate: render/frame/icons; CLevel::OnRender after the 3D world: render/level/*.
PERF_TIMER(rframe_icons, "render/frame/icons")                 // OnWeaponIconRenderPass in the main Calculate (draws live objects on IMM)
PERF_TIMER(rlvl_game_on_render, "render/level/game_on_render") // Game().OnRender
PERF_TIMER(rlvl_bullets, "render/level/bullets")               // bullet tracers: BulletManager().Render or the two SVP-frame calls
PERF_TIMER(rlvl_after_world, "render/level/after_world")       // AfterWorldRender (scope image copy)
PERF_TIMER(rlvl_hud_overlay, "render/level/hud_overlay")       // RenderHudOverlayToTexture + CompositeHudOverlay
PERF_TIMER(rlvl_ui, "render/level/ui")                         // HUD().RenderUI, after the join with the Game thread
PERF_TIMER(rhud_active_item_ui, "render/hud/active_item_ui")   // render_hud_ui -> pHUD->RenderActiveItemUI, every pass (part of pass/hud_ui)
PERF_TIMER(frame_render_seq_level, "frame/render/seq_level")   // seqRender: the level subscriber (CLevel::OnRender, whole)
PERF_TIMER(frame_render_seq_other, "frame/render/seq_other")   // seqRender: every other subscriber (cursor, console, cell items, stats...)
// Scope pass (SVP), next to render/frame/svp_*: what render/frame/svp_join waits for, and the main-side build steps.
PERF_TIMER(rsvp_kick, "render/svp/kick")                       // main: BeginSecondVPCalculateParallel (snapshot, wake of the SVP thread)
PERF_TIMER(rsvp_begin_vp, "render/svp/begin_vp")               // main: BeginSecondViewportRender (Device camera to the scope)
PERF_TIMER(rsvp_worker_build, "render/svp/worker_build")       // SVP thread: visibility build (calculate_into)
PERF_TIMER(rsvp_worker_geom, "render/svp/worker_geom")         // SVP thread: render_graph(0) recording into the deferred list
PERF_TIMER(rsvp_worker_lights_wait, "render/svp/worker_lights_wait") // SVP thread idles until main finished its own lighting
PERF_TIMER(rsvp_worker_shadows, "render/svp/worker_shadows")   // SVP thread: record_second_vp_shadows (worker shadow build mode)
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
// Physics step Ф0/Ф1 (plans/optimization_mt/21-physics-step.md). physics/step/*: the frame step on the physics thread only
// (CPHWorld::FrameStep, b_processing) - the world steps on main (CPHActivationShape, FlyTo, MovementBoxDynamicActivate)
// stay out of them, unlike physics/collide|phtune|solve|post_integrate above.
PERF_TIMER(ph_step_substep, "physics/step/substep")         // one Step() of FrameStep: avg = cost of a substep, calls = substeps
PERF_TIMER(ph_step_collide, "physics/step/collide")
PERF_TIMER(ph_step_phtune, "physics/step/phtune")
PERF_TIMER(ph_step_solve, "physics/step/solve")             // island solve, wall time (parallel at >= ph_mt_island_min islands)
PERF_TIMER(ph_step_solve_cpu, "physics/step/solve_cpu")     // the same, summed over the threads: solve_cpu / solve = parallelism
PERF_TIMER(ph_step_post, "physics/step/post_integrate")
// physics/step/collide split, per object (calls = objects collided): q_box + DoCollide filter / dynamic pairs (ray
// motions, NearCallback: dCollide, contacts, island merges, wakes) / the level mesh (CollideStatic).
PERF_TIMER(ph_step_broadphase, "physics/step/broadphase")
PERF_TIMER(ph_step_narrow, "physics/step/narrow")
PERF_TIMER(ph_step_static, "physics/step/static")
PERF_GAUGE(ph_island_max_nb, "physics/island/max_bodies")  // largest island of the frame (all substeps): bodies
PERF_GAUGE(ph_island_max_nj, "physics/island/max_joints")  // most joints in one island (contact joints included)
// Who wakes force-slept corpses (CPHWorld::SleepCorpses, CPHShell::m_force_slept).
PERF_COUNTER(ph_corpse_wake, "physics/corpse_wake/total")  // any path (CPHShell::EnableObject): contact, hit, script, grab
// ... of them by a contact on the physics thread (NearCallback), by the type of the other object
PERF_COUNTER(ph_corpse_wake_by_actor, "physics/corpse_wake/by/actor")
PERF_COUNTER(ph_corpse_wake_by_ai, "physics/corpse_wake/by/ai")           // a live NPC controller
PERF_COUNTER(ph_corpse_wake_by_ragdoll, "physics/corpse_wake/by/ragdoll") // a corpse that was not force-slept
PERF_COUNTER(ph_corpse_wake_by_vehicle, "physics/corpse_wake/by/vehicle")
PERF_COUNTER(ph_corpse_wake_by_item, "physics/corpse_wake/by/item")       // an inventory item, weapons included
PERF_COUNTER(ph_corpse_wake_by_prop, "physics/corpse_wake/by/prop")       // any other shell (physic objects, doors)
PERF_COUNTER(ph_corpse_wake_by_other, "physics/corpse_wake/by/other")
// Ф1: a loose body against a force-slept corpse, the corpse acting as static geometry
PERF_COUNTER(ph_oneway_pairs, "physics/corpse_oneway/pairs")       // NearCallback pairs taken one-sided
PERF_COUNTER(ph_oneway_contacts, "physics/corpse_oneway/contacts") // contact joints made on the loose body only
// The same wakes (physics/corpse_wake/total) by the path that reached CPHShell::EnableObject: the ph_wake::* tag of the
// calling thread (PerfMetrics.hpp; carried over to the post-step flush for mutations deferred from other threads).
PERF_COUNTER(ph_corpse_wake_path_contact, "physics/corpse_wake/path/contact")             // NearCallback, frame step (~ sum of by/*)
PERF_COUNTER(ph_corpse_wake_path_contact_local, "physics/corpse_wake/path/contact_local") // NearCallback elsewhere: TryPosition, activation shape, FlyTo
PERF_COUNTER(ph_corpse_wake_path_hit, "physics/corpse_wake/path/hit")                     // applyHit: bullets, melee, hit events (zone hits too)
PERF_COUNTER(ph_corpse_wake_path_explosion, "physics/corpse_wake/path/explosion")         // ExplosionHit
PERF_COUNTER(ph_corpse_wake_path_impulse, "physics/corpse_wake/path/impulse")             // applyImpulse/applyForce/setForce from C++ game code
PERF_COUNTER(ph_corpse_wake_path_script, "physics/corpse_wake/path/script")               // Lua physics API
PERF_COUNTER(ph_corpse_wake_path_grab, "physics/corpse_wake/path/grab")                   // CPHCapture, CTelekinesis
PERF_COUNTER(ph_corpse_wake_path_zone, "physics/corpse_wake/path/zone")                   // CCustomZone update (gravi pull start, ...)
PERF_COUNTER(ph_corpse_wake_path_activation, "physics/corpse_wake/path/activation")       // CPHShell/CPHElement::Enable, net state
PERF_COUNTER(ph_corpse_wake_path_other, "physics/corpse_wake/path/other")                 // untagged
// NPC one-way: a live NPC controller against a force-slept corpse, the corpse acting as static
PERF_COUNTER(ph_oneway_npc_pairs, "physics/corpse_oneway/npc_pairs")
PERF_COUNTER(ph_oneway_npc_contacts, "physics/corpse_oneway/npc_contacts")
// Ф8 (sleep thresholds scaled to 100 Hz): bodies auto-disabled by CBaseDisableData::Disabling (elements and
// controllers, all paths) - the natural sleep rate
PERF_COUNTER(ph_auto_disable, "physics/sleep/auto_disable")
// ph_corpse_sleep_settled 1: corpses CPHWorld::SleepCorpses put to sleep before ph_corpse_sleep_ms because they had
// settled (a part of physics/corpse_force_slept)
PERF_COUNTER(ph_corpse_sleep_settled, "physics/corpse_sleep/settled")
// ph_corpse_sleep_settled 1, why a corpse did (not) settle (CPHWorld::ShouldForceSleepCorpse / UpdateCorpseSettled), per
// frame step and corpse - summed over the substeps of a frame:
// candidates = active corpses old enough (ph_corpse_settle_min_age_ms), younger than ph_corpse_sleep_ms, farther than
// ph_corpse_sleep_far_dist; vel_fail = of them some element faster than ph_corpse_settle_vel / _ang_vel (the run starts
// anew), ang_fail = of vel_fail the first fast element was over the angular limit only; run = slow, the run still shorter
// than ph_corpse_settle_ms; stretch_fail = the run complete but a joint stretched beyond ph_corpse_sleep_max_stretch.
// candidates = vel_fail + run + stretch_fail + settled.
PERF_COUNTER(ph_corpse_settle_candidates, "physics/corpse_sleep/settle_candidates")
PERF_COUNTER(ph_corpse_settle_vel_fail, "physics/corpse_sleep/settle_vel_fail")
PERF_COUNTER(ph_corpse_settle_ang_fail, "physics/corpse_sleep/settle_ang_fail")
PERF_COUNTER(ph_corpse_settle_run, "physics/corpse_sleep/settle_run")
PERF_COUNTER(ph_corpse_settle_stretch_fail, "physics/corpse_sleep/settle_stretch_fail")
// The natural sleep of ragdolls (CPHShell::PhDataUpdate: every element asleep -> the shell leaves the active list), any
// path: all / younger than ph_corpse_sleep_ms (asleep before the age rule) / younger than ph_corpse_settle_min_age_ms
// (asleep before the settled rule may start). Woken old corpses that fall asleep again count in the first one only.
PERF_COUNTER(ph_auto_disable_ragdoll, "physics/sleep/auto_disable_ragdoll")
PERF_COUNTER(ph_auto_disable_ragdoll_young, "physics/sleep/auto_disable_ragdoll_young")
PERF_COUNTER(ph_auto_disable_ragdoll_early, "physics/sleep/auto_disable_ragdoll_early")
// Corpses picked by CPHWorld::ShouldForceSleepCorpse (pre-solve state) but left active by SleepCorpses, because after the
// substep's solve a joint was stretched beyond ph_corpse_sleep_max_stretch or an element was faster than
// ph_corpse_sleep_max_vel (a hit / an explosion integrated in that very substep). Not counted in corpse_force_slept
// (a settled candidate rejected here is still counted in corpse_sleep/settled).
PERF_COUNTER(ph_corpse_sleep_post_reject, "physics/corpse_sleep/post_reject")
// physics/corpse_wake/path/contact_local split by the other object (obj1 of NearCallback outside the frame step on the
// physics thread): activation = CPHActivationShape (a death, a movement box switch, FlyTo), ai = an NPC controller that
// is not one-sided there (a sleeping or not live one)
PERF_COUNTER(ph_corpse_wake_local_by_actor, "physics/corpse_wake/local_by/actor")
PERF_COUNTER(ph_corpse_wake_local_by_ai, "physics/corpse_wake/local_by/ai")
PERF_COUNTER(ph_corpse_wake_local_by_ragdoll, "physics/corpse_wake/local_by/ragdoll")
PERF_COUNTER(ph_corpse_wake_local_by_item, "physics/corpse_wake/local_by/item")
PERF_COUNTER(ph_corpse_wake_local_by_prop, "physics/corpse_wake/local_by/prop")
PERF_COUNTER(ph_corpse_wake_local_by_activation, "physics/corpse_wake/local_by/activation")
PERF_COUNTER(ph_corpse_wake_local_by_other, "physics/corpse_wake/local_by/other")
// Ф3 (plans/optimization_mt/21-physics-step.md), ph_npc_kinematic_unseen 1: live NPCs moved kinematically because they
// are farther than ph_npc_kinematic_unseen_dist from the actor and not drawn in the main pass for a while (counted like
// physics/npc_kinematic_far_forced: per NPC update that took the kinematic branch; not included in it);
// skip_combat = the same NPCs kept on the physics path because the actor is their selected enemy.
PERF_COUNTER(ph_npc_kinematic_unseen_forced, "physics/npc_kinematic_unseen_forced")
PERF_COUNTER(ph_npc_kinematic_unseen_skip_combat, "physics/npc_kinematic_unseen_skip_combat")
// F1/F4 (plans/optimization_mt/20-framemove-reduction.md): CMapManager::Update - locations updated a frame (all levels;
// the position only for the current level) and visible relation locations matched against them in one pass
PERF_COUNTER(mapmgr_locations, "map/locations")
PERF_COUNTER(mapmgr_relation_checks, "map/relation")
// E3 (plans/optimization_mt/18-combine-pass.md): empty particle effects kept out of the render lists by
// add_leafs_dynamic, main view build only. ps_empty_skip + render/combine/ps_empty ~ the former render/combine/ps_empty.
PERF_COUNTER(rmain_ps_empty_skip, "render/main/ps_empty_skip")

// 22-game-thread.md (plans/optimization_mt): where the Game logic thread goes in heavy scenes. Diagnostics only, no
// behaviour change. CAI_Stalker::shedule_Update by stage, calls = stalker updates that ran the stage (the LOD of
// ai_stalker.cpp skips a stage between its intervals): inherited = CEntityAlive..CGameObject incl. the Lua binder,
// think = brain + movement (or script_control instead of it), dead_think = the Think of dead stalkers.
PERF_TIMER(stk_shed_anim_callbacks, "stalker/shed/anim_callbacks")
PERF_TIMER(stk_shed_agent_manager, "stalker/shed/agent_manager")
PERF_TIMER(stk_shed_vision, "stalker/shed/vision_prepare")
PERF_TIMER(stk_shed_process_enemies, "stalker/shed/process_enemies")
PERF_TIMER(stk_shed_memory, "stalker/shed/memory")
PERF_TIMER(stk_shed_inherited, "stalker/shed/inherited")
PERF_TIMER(stk_shed_script_control, "stalker/shed/script_control")
PERF_TIMER(stk_shed_think, "stalker/shed/think")
PERF_TIMER(stk_shed_dead_think, "stalker/shed/dead_think")
PERF_TIMER(stk_shed_feel_touch, "stalker/shed/feel_touch")
PERF_TIMER(stk_shed_inventory_owner, "stalker/shed/inventory_owner")
PERF_TIMER(stk_shed_physics, "stalker/shed/physics")
PERF_TIMER(stk_think_brain, "stalker/think/brain")
PERF_TIMER(stk_think_movement, "stalker/think/movement")
// CStalkerPlanner::update (= stalker/think/brain): solve = actuality check (evaluators) + graph_search; execute = the
// current action, the three planners apart (the rest: death, anomaly, gather items, other)
PERF_TIMER(stk_brain_solve, "stalker/brain/solve")
PERF_TIMER(stk_brain_graph_search, "stalker/brain/graph_search")
PERF_TIMER(stk_brain_transition, "stalker/brain/transition")
PERF_TIMER(stk_brain_execute, "stalker/brain/execute")
PERF_TIMER(stk_brain_exec_alife, "stalker/brain/exec_alife")
PERF_TIMER(stk_brain_exec_combat, "stalker/brain/exec_combat")
PERF_TIMER(stk_brain_exec_danger, "stalker/brain/exec_danger")
// The stalker updates by LOD tier (near <= 35 m or an enemy selected, medium <= 80 m, far) and dead; dt_ms = sum of
// the DT they got: dt_ms / (near + medium + far + dead) = the real update period, the scheduler lateness included
PERF_COUNTER(stk_shed_lod_near, "stalker/shed/lod_near")
PERF_COUNTER(stk_shed_lod_medium, "stalker/shed/lod_medium")
PERF_COUNTER(stk_shed_lod_far, "stalker/shed/lod_far")
PERF_COUNTER(stk_shed_dead, "stalker/shed/dead")
PERF_COUNTER(stk_shed_dt_ms, "stalker/shed/dt_ms")
// The real gap between two shedule_Update of a live stalker (own timestamp, not the clamped DT):
// alive_period_ms / alive_period_n = the real update period
PERF_COUNTER(stk_shed_alive_period_ms, "stalker/shed/alive_period_ms")
PERF_COUNTER(stk_shed_alive_period_n, "stalker/shed/alive_period_n")
// CMemoryManager::update by part, stalkers and monsters together (stalker/shed/memory is the stalker total)
PERF_TIMER(ai_mem_visual, "ai/memory/visual")
PERF_TIMER(ai_mem_sound, "ai/memory/sound")
PERF_TIMER(ai_mem_hit, "ai/memory/hit")
PERF_TIMER(ai_mem_collect, "ai/memory/collect")
PERF_TIMER(ai_mem_enemies, "ai/memory/enemies")
PERF_TIMER(ai_mem_item, "ai/memory/item")
PERF_TIMER(ai_mem_danger, "ai/memory/danger")
// CGameObject::shedule_Update: the Lua binder update (xr_motivator, bind_monster...) by class, and the engine
// npc_on_update emit of the native binder gate
PERF_TIMER(go_shed_binder_stalker, "game_object/shed/binder/stalker")
PERF_TIMER(go_shed_binder_monster, "game_object/shed/binder/monster")
PERF_TIMER(go_shed_binder_other, "game_object/shed/binder/other")
PERF_TIMER(go_shed_npc_update_emit, "game_object/shed/npc_update_emit")
// game/parallel_seq (Device.seqParallel, run one by one on the game logic thread) by delegate: items = delegates a
// frame; the rest of it is lua/gc_step (script_gc), alife/update and CUILogsWnd (PDA open)
PERF_GAUGE(game_pseq_items, "game/pseq/items")
PERF_TIMER(pseq_object_handler, "game/pseq/object_handler") // CAI_Stalker::update_object_handler, per stalker UpdateCL
PERF_TIMER(pseq_sound_player, "game/pseq/sound_player")     // CCustomMonster::update_sound_player, per NPC UpdateCL
PERF_TIMER(pseq_bullets, "game/pseq/bullets")               // CBulletManager::UpdateWorkload, once a frame
PERF_TIMER(pseq_level_sounds, "game/pseq/level_sounds")     // CLevelSoundManager::Update, once a frame
PERF_TIMER(pseq_level_path, "game/pseq/level_path")         // CLevelPathBuilder::process, on demand
PERF_TIMER(pseq_detail_path, "game/pseq/detail_path")       // CDetailPathBuilder::process, on demand
// 22-game-thread.md, step 1 (plans/optimization_mt): the object handler LOD (npc_perf_object_handler_lod) and the
// corpse scheduler LOD (npc_perf_dead_shed_lod), ai_stalker.cpp / ai_stalker_fire.cpp. full = object handler runs from
// UpdateCL (with the LOD off too), skipped = runs left out by the LOD; keep_* = why a run was not skipped while the LOD
// is on (busy: the planner or the item in the hands mid-action, a new goal; fight: an enemy, a recent hit, recoil;
// near: within npc_perf_object_handler_lod_dist m, the view entity, the first frames); pick_full / pick_reused =
// friendly-fire picks cast (up to 5 ray queries each) / kept by the pick LOD
PERF_COUNTER(npc_oh_full, "npc/object_handler/full")
PERF_COUNTER(npc_oh_skipped, "npc/object_handler/skipped")
PERF_COUNTER(npc_oh_keep_busy, "npc/object_handler/keep_busy")
PERF_COUNTER(npc_oh_keep_fight, "npc/object_handler/keep_fight")
PERF_COUNTER(npc_oh_keep_near, "npc/object_handler/keep_near")
PERF_COUNTER(npc_oh_pick_full, "npc/object_handler/pick_full")
PERF_COUNTER(npc_oh_pick_reused, "npc/object_handler/pick_reused")
// The whole CAI_Stalker::shedule_Update of a dead stalker (stalker/shed/dead counts them); dead_lod = the dead updates
// after which the long corpse interval holds
PERF_TIMER(stk_shed_dead_total, "stalker/shed/dead_total")
PERF_COUNTER(stk_shed_dead_lod, "stalker/shed/dead_lod")

// 22-game-thread.md, step 2 (npc_perf_sound_dispatch): the sound delegates of IGame_Level::SoundEvent_Dispatch.
// delivered = feel_sound_new calls (with the switch off too); capped / deduped = delegates CLevel::SoundEvent_Filter
// dropped (beyond the loudest npc_perf_sound_dispatch_max[_combat] listeners / the same source and type to a listener
// again in the frame); filter = its own time; below_threshold = deliveries too quiet for CSoundMemoryManager (only the
// hear callbacks get them)
PERF_COUNTER(npc_sound_delivered, "npc/sound/delivered")
PERF_COUNTER(npc_sound_capped, "npc/sound/capped")
PERF_COUNTER(npc_sound_deduped, "npc/sound/deduped")
PERF_TIMER(npc_sound_filter, "npc/sound/filter")
PERF_COUNTER(npc_sound_below_threshold, "npc/sound/below_threshold")

// 22-game-thread.md, step 1b (npc_perf_friendly_fire_fast, ai_stalker_fire.cpp): the friendly-fire pick of
// CAI_Stalker::update_can_kill_info. pick = the time of the cast picks in every mode (calls = picks; with the switch at
// 0 the same picks as npc/object_handler/pick_full); checks = fast picks (one q_frustum each, spatial/q_frustum/ray);
// rays = their rays (up to 5 a pick); candidates = objects the cone gathered; precise_rays = precise (bone-level) tests
// of single objects (RayQuery tests every object whose sphere a ray crosses); mismatch = shadow mode (2) picks whose
// fast answer differs from the original one
PERF_TIMER(npc_ff_pick, "npc/ff/pick")
PERF_COUNTER(npc_ff_checks, "npc/ff/checks")
PERF_COUNTER(npc_ff_rays, "npc/ff/rays")
PERF_COUNTER(npc_ff_candidates, "npc/ff/candidates")
PERF_COUNTER(npc_ff_precise_rays, "npc/ff/precise_rays")
PERF_COUNTER(npc_ff_mismatch, "npc/ff/mismatch")

// 22-game-thread.md, step 4 (shed_priority, xrEngine/xrSheduler.cpp): the near/far classes of the non-RT scheduler.
// near_* / far_* = executed steps by class: *_late_ms / *_late_n = their average lateness (with shed_priority 0 too:
// then each step is classified only for these metrics); *_stalker_* = the same for stalkers only. near_items = the near
// heap (0 with shed_priority 0; shed/items is both heaps), near_creatures = the living stalkers and monsters in it;
// promoted = far items moved to the near heap by the scan
// (promote_scan = its time); phase_near / phase_far = the time of the two passes (shed_priority 1); dt_clamped /
// dt_lost_ms = steps whose real elapsed time was cut to max(t_max, 1000) ms for shedule_Update, and the time cut off
PERF_COUNTER(shed_near_late_ms, "shed/near_late_ms")
PERF_COUNTER(shed_near_late_n, "shed/near_late_n")
PERF_COUNTER(shed_far_late_ms, "shed/far_late_ms")
PERF_COUNTER(shed_far_late_n, "shed/far_late_n")
PERF_COUNTER(shed_near_stalker_late_ms, "shed/near_stalker_late_ms")
PERF_COUNTER(shed_near_stalker_late_n, "shed/near_stalker_late_n")
PERF_COUNTER(shed_far_stalker_late_ms, "shed/far_stalker_late_ms")
PERF_COUNTER(shed_far_stalker_late_n, "shed/far_stalker_late_n")
PERF_GAUGE(shed_near_items, "shed/near_items")
PERF_GAUGE(shed_near_creatures, "shed/near_creatures")
PERF_COUNTER(shed_promoted, "shed/promoted")
PERF_TIMER(shed_promote_scan, "shed/promote_scan")
PERF_TIMER(shed_phase_near, "shed/phase_near")
PERF_TIMER(shed_phase_far, "shed/phase_far")
PERF_COUNTER(shed_dt_clamped, "shed/dt_clamped")
PERF_COUNTER(shed_dt_lost_ms, "shed/dt_lost_ms")
