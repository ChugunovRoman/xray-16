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
PERF_TIMER(rcomb_ps_fill, "render/combine/ps_fill")             // ParticleRenderStream (sprite vertex build)
PERF_COUNTER(rcomb_ps_empty, "render/combine/ps_empty")         // effects reaching the combine with no particles (emptied after the build)
PERF_COUNTER(rcomb_ps_sprites, "render/combine/ps_sprites")     // particles turned into sprites
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
// The natural sleep of ragdolls (CPHShell::PhDataUpdate: every element asleep -> the shell leaves the active list), any
// path: all / younger than ph_corpse_sleep_ms (asleep before the age rule) / younger than 2 s. Woken old corpses that
// fall asleep again count in the first one only.
PERF_COUNTER(ph_auto_disable_ragdoll, "physics/sleep/auto_disable_ragdoll")
PERF_COUNTER(ph_auto_disable_ragdoll_young, "physics/sleep/auto_disable_ragdoll_young")
PERF_COUNTER(ph_auto_disable_ragdoll_early, "physics/sleep/auto_disable_ragdoll_early")
// Corpses picked by CPHWorld::ShouldForceSleepCorpse (pre-solve state) but left active by SleepCorpses, because after the
// substep's solve a joint was stretched beyond ph_corpse_sleep_max_stretch or an element was faster than
// ph_corpse_sleep_max_vel (a hit / an explosion integrated in that very substep). Not counted in corpse_force_slept.
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
// Ф3 (plans/optimization_mt/21-physics-step.md): live NPCs moved kinematically because they
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
// 22-game-thread.md, step 1 (plans/optimization_mt): the object handler LOD and the corpse scheduler LOD,
// ai_stalker.cpp / ai_stalker_fire.cpp. full = object handler runs from UpdateCL, skipped = runs left out by the LOD;
// keep_* = why a run was not skipped (busy: the planner or the item in the hands mid-action, a new goal; fight: an
// enemy, a recent hit, recoil; near: within npc_perf_object_handler_lod_dist m, the view entity, the first frames);
// pick_full / pick_reused = friendly-fire picks cast (up to 5 rays each) / kept by the pick LOD
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

// 22-game-thread.md, step 2: the sound delegates of IGame_Level::SoundEvent_Dispatch.
// delivered = feel_sound_new calls; capped / deduped = delegates CLevel::SoundEvent_Filter
// dropped (beyond the loudest npc_perf_sound_dispatch_max[_combat] listeners / the same source and type to a listener
// again in the frame); filter = its own time; below_threshold = deliveries too quiet for CSoundMemoryManager (only the
// hear callbacks get them)
PERF_COUNTER(npc_sound_delivered, "npc/sound/delivered")
PERF_COUNTER(npc_sound_capped, "npc/sound/capped")
PERF_COUNTER(npc_sound_deduped, "npc/sound/deduped")
PERF_TIMER(npc_sound_filter, "npc/sound/filter")
PERF_COUNTER(npc_sound_below_threshold, "npc/sound/below_threshold")

// 22-game-thread.md, step 1b (ai_stalker_fire.cpp): the friendly-fire pick of CAI_Stalker::update_can_kill_info.
// pick = the time of the cast picks (calls = picks, the same as npc/object_handler/pick_full); checks = picks (one
// q_frustum each, spatial/q_frustum/ray); rays = their rays (up to 5 a pick); candidates = objects the cone gathered;
// precise_rays = precise (bone-level) tests of single objects (RayQuery tests every object whose sphere a ray crosses)
PERF_TIMER(npc_ff_pick, "npc/ff/pick")
PERF_COUNTER(npc_ff_checks, "npc/ff/checks")
PERF_COUNTER(npc_ff_rays, "npc/ff/rays")
PERF_COUNTER(npc_ff_candidates, "npc/ff/candidates")
PERF_COUNTER(npc_ff_precise_rays, "npc/ff/precise_rays")

// 22-game-thread.md, step 4 (xrEngine/xrSheduler.cpp): the near/far classes of the non-RT scheduler.
// near_* / far_* = executed steps by class (the heap they came from): *_late_ms / *_late_n = their average lateness;
// *_stalker_* = the same for stalkers only. near_items = the near heap (shed/items is both heaps), near_creatures = the
// living stalkers and monsters in it; promoted = far items moved to the near heap by the scan
// (promote_scan = the time of both scans); phase_near / phase_far = the time of the two passes; dt_clamped /
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

// 22-game-thread.md, step 4c (the adaptive near radius, xrEngine/xrSheduler.cpp; permanent): the adaptive near radius of
// the living creatures. near_radius = the effective plain radius in cm (shed_near_dist times the scale of the
// controller); demoted = near items moved back to the
// far heap by the scan of the near heap (the radius shrank, the camera moved away, the creature died)
PERF_GAUGE(shed_near_radius, "shed/near_radius")
PERF_COUNTER(shed_demoted, "shed/demoted")

// 19-part0-submit.md, step 2 (f): DX11 backend per-draw redundancy (CBackend::_stats). render/main/* - the main view's
// render_graph (static + dynamic, same scope as render/main/submit_*); render/backend/* - all backends of the frame
// (immediate + pooled deferred, flushed in CBackend::OnFrameEnd). layout_skip = input-layout lookups answered by the
// last (declaration, VS signature) pair (no xr_map search), layout_set = IASetInputLayout calls; sampler_set /
// sampler_skip = per-stage *SetSamplers of set_States issued / left out (the stage already holds those samplers);
// cb_flush_slots = constant buffers visited by constants.flush() (was 84 slots per draw, now the distinct bound ones).
PERF_COUNTER(rmain_layout_skip, "render/main/layout_skip")
PERF_COUNTER(rmain_layout_set, "render/main/layout_set")
PERF_COUNTER(rmain_sampler_skip, "render/main/sampler_skip")
PERF_COUNTER(rmain_sampler_set, "render/main/sampler_set")
PERF_COUNTER(rmain_cb_flush_slots, "render/main/cb_flush_slots")
PERF_COUNTER(rbackend_layout_skip, "render/backend/layout_skip")
PERF_COUNTER(rbackend_layout_set, "render/backend/layout_set")
PERF_COUNTER(rbackend_sampler_skip, "render/backend/sampler_skip")
PERF_COUNTER(rbackend_sampler_set, "render/backend/sampler_set")
PERF_COUNTER(rbackend_cb_flush_slots, "render/backend/cb_flush_slots")

// 19-part0-submit.md, step 4 (d): the main view's render_graph(0) recorded in parallel on D3D11 deferred contexts
// (R_dsgraph_structure::render_graph_main_parallel). par_submit_wall - the whole replacement of
// render_graph(0) on main (plan + recording + execution); par_submit_prepare - sorting, the safety scan and the CROS
// pre-pass on main; par_submit_record_cpu - one unit recorded (sum over threads = CPU, calls = units); par_submit_execute -
// ExecuteCommandList + re-seeding the immediate context on main; par_submit_tail - main waiting for units recorded on
// workers; par_submit_parts - units (deferred + inline), par_submit_lists - command lists executed, par_submit_main_items -
// items kept on the immediate context (particles, LODs, CPU skinning, video or not yet bound textures); par_submit_fallback -
// frames that took the serial render_graph(0); par_submit_ctx_short - frames that got fewer pooled
// contexts than r__main_parallel_parts - 1. In this mode render/main/submit_static / submit_dynamic are CPU sums over units.
PERF_TIMER(rmain_par_submit_wall, "render/main/par_submit_wall")
PERF_TIMER(rmain_par_submit_prepare, "render/main/par_submit_prepare")
PERF_TIMER(rmain_par_submit_record_cpu, "render/main/par_submit_record_cpu")
PERF_TIMER(rmain_par_submit_execute, "render/main/par_submit_execute")
PERF_TIMER(rmain_par_submit_tail, "render/main/par_submit_tail")
PERF_COUNTER(rmain_par_submit_parts, "render/main/par_submit_parts")
PERF_COUNTER(rmain_par_submit_lists, "render/main/par_submit_lists")
PERF_COUNTER(rmain_par_submit_main_items, "render/main/par_submit_main_items")
PERF_COUNTER(rmain_par_submit_fallback, "render/main/par_submit_fallback")
PERF_COUNTER(rmain_par_submit_ctx_short, "render/main/par_submit_ctx_short")

// 24-critical-path.md, M10 + G6b: vision of NPCs (Feel::Vision / CCustomMonster), in practice stalkers.
// s1_query - q_frustum of eye_pp_s1 (Feel::Vision::feel_vision_query), s1_filter - the feel_vision_isRelevant filter
// + sort + unique of its result; s2_merge - feel_vision_merge_after_frustum (the diff of seen / query, adding, removal);
// s2_rays - preparing / tracing the rays after the merge (eye_pp_s2_prepare loop, o_trace). query_results - objects
// returned by q_frustum, query_relevant - left after the filter, o_new / o_delete - feel_visible items added / removed
// by the merge. G6b: s2_o_new / s2_o_delete - the adding / removal parts of s2_merge; mesh_point_lazy - mesh points
// chosen on the first use (the merge adds items without them), mesh_point_skipped - items removed before any ray at
// them (the get_new/get_last_local_point_on_mesh calls saved outright).
PERF_TIMER(stk_vision_s1_query, "stalker/vision/s1_query")
PERF_TIMER(stk_vision_s1_filter, "stalker/vision/s1_filter")
PERF_TIMER(stk_vision_s2_merge, "stalker/vision/s2_merge")
PERF_TIMER(stk_vision_s2_rays, "stalker/vision/s2_rays")
PERF_COUNTER(vision_query_results, "ai/vision/query_results")
PERF_COUNTER(vision_query_relevant, "ai/vision/query_relevant")
PERF_COUNTER(vision_o_new, "ai/vision/o_new")
PERF_COUNTER(vision_o_delete, "ai/vision/o_delete")
PERF_TIMER(stk_vision_s2_o_new, "stalker/vision/s2_o_new")
PERF_TIMER(stk_vision_s2_o_delete, "stalker/vision/s2_o_delete")
PERF_COUNTER(vision_mesh_point_lazy, "ai/vision/mesh_point_lazy")
PERF_COUNTER(vision_mesh_point_skipped, "ai/vision/mesh_point_skipped")

// 24-critical-path.md, M8 / M9 / M11 / M12: what one stalker scheduler pass spends in a fight (diagnostics only).
// M8, stalker/brain/exec_combat by part (CStalkerCombatPlanner::update): solve = the combat planner's actuality check +
// graph search (its evaluators, Lua ones included), transition = finalize/initialize of its actions, exec = the
// current action, react = react_on_grenades + react_on_member_death. act/* = exec by the current action of the combat
// planner (calls = combat updates that ran it): kill_enemy (+ kill_if_not_visible / _critically_wounded /
// _player_on_the_path), ready_to_kill (get_ready_to_kill / _detour), hold = hold_position, sudden = sudden attack,
// smart_cover = in smart cover, search / subplanner = the nested planners (search enemy; get distance, low cover,
// kill wounded), post_combat = post_combat_wait (Lua in GW), other = the rest (grenades, items, retreat, actions
// added from Lua). Only the combat planner's own update is timed: nested planners take the tag (action_planner_inline.h)
// and time nothing, so exec = sum of act/*. danger/* = the same for CStalkerDangerPlanner (exec_danger).
PERF_TIMER(stk_combat_solve, "stalker/combat/solve")
PERF_TIMER(stk_combat_transition, "stalker/combat/transition")
PERF_TIMER(stk_combat_exec, "stalker/combat/exec")
PERF_TIMER(stk_combat_react, "stalker/combat/react")
PERF_TIMER(stk_combat_act_kill_enemy, "stalker/combat/act/kill_enemy")
PERF_TIMER(stk_combat_act_take_cover, "stalker/combat/act/take_cover")
PERF_TIMER(stk_combat_act_look_out, "stalker/combat/act/look_out")
PERF_TIMER(stk_combat_act_hold, "stalker/combat/act/hold")
PERF_TIMER(stk_combat_act_ready_to_kill, "stalker/combat/act/ready_to_kill")
PERF_TIMER(stk_combat_act_detour, "stalker/combat/act/detour")
PERF_TIMER(stk_combat_act_sudden, "stalker/combat/act/sudden")
PERF_TIMER(stk_combat_act_smart_cover, "stalker/combat/act/smart_cover")
PERF_TIMER(stk_combat_act_search, "stalker/combat/act/search")
PERF_TIMER(stk_combat_act_subplanner, "stalker/combat/act/subplanner")
PERF_TIMER(stk_combat_act_post_combat, "stalker/combat/act/post_combat")
PERF_TIMER(stk_combat_act_other, "stalker/combat/act/other")
PERF_TIMER(stk_danger_solve, "stalker/danger/solve")
PERF_TIMER(stk_danger_transition, "stalker/danger/transition")
PERF_TIMER(stk_danger_exec, "stalker/danger/exec")
// Covers. stalker/cover/best = CAI_Stalker::best_cover (actuality check + search); find (find_best_cover) and value
// (best_cover_value) are nested in it (value is also called on its own). ai/cover/query = CCoverManager::best_cover,
// every caller (stalker actions, danger actions, monsters, Lua), nested in the stalker ones; nearest = cover points the
// quadtree returned, evaluated = points scored by the evaluator, inertia = queries answered by the inertia (no scan).
PERF_TIMER(stk_cover_best, "stalker/cover/best")
PERF_TIMER(stk_cover_find, "stalker/cover/find")
PERF_TIMER(stk_cover_value, "stalker/cover/value")
PERF_TIMER(ai_cover_query, "ai/cover/query")
PERF_COUNTER(ai_cover_nearest, "ai/cover/nearest")
PERF_COUNTER(ai_cover_evaluated, "ai/cover/evaluated")
PERF_COUNTER(ai_cover_inertia, "ai/cover/inertia")
// M9, Lua in the action planners (all planners, all threads): script_action/execute = CScriptActionWrapper::execute
// into Lua, init_fin = its initialize + finalize; script_evaluator/evaluate = Lua calls of
// CScriptPropertyEvaluatorWrapper (calls = real Lua calls), cached = evaluate() answered from its cache without Lua.
// The two overlap where a Lua action updates a planner of its own (state_mgr): do not add them up.
// stalker/combat/lua_exec / lua_eval = the same Lua time inside CStalkerCombatPlanner::update (nested planners too):
// lua_eval is part of combat/solve (and of exec when a nested planner solves), lua_exec of exec/transition.
PERF_TIMER(ai_script_action_execute, "ai/script_action/execute")
PERF_TIMER(ai_script_action_init_fin, "ai/script_action/init_fin")
PERF_TIMER(ai_script_evaluator_evaluate, "ai/script_evaluator/evaluate")
PERF_COUNTER(ai_script_evaluator_cached, "ai/script_evaluator/cached")
PERF_TIMER(stk_combat_lua_exec, "stalker/combat/lua_exec")
PERF_TIMER(stk_combat_lua_eval, "stalker/combat/lua_eval")
// M11, npc/ff/pick by the caller that paid for the pick (the first one in the frame): pick_oh = from
// update_object_handler (UpdateCL, game/pseq/object_handler), pick_think = from CAI_Stalker::Think (shedule_Update:
// fire_make_sense, combat actions, movement), pick_other = anything else. pick_oh + pick_think + pick_other = pick.
// gather = the spatial part of a pick (cone q_frustum + candidate list), the rest of npc/ff/pick is the rays.
PERF_TIMER(npc_ff_pick_oh, "npc/ff/pick_oh")
PERF_TIMER(npc_ff_pick_think, "npc/ff/pick_think")
PERF_TIMER(npc_ff_pick_other, "npc/ff/pick_other")
PERF_TIMER(npc_ff_gather, "npc/ff/gather")
// M12, CEnemyManager::useful past the native checks: useful_lua = the Lua useful callback of the NPC (calls = Lua
// calls), useful_plugin = the enemy filters of plugins, useful_cached = answers from the per-enemy callback cache.
PERF_TIMER(stk_enemy_useful_lua, "stalker/enemy/useful_lua")
PERF_TIMER(stk_enemy_useful_plugin, "stalker/enemy/useful_plugin")
PERF_COUNTER(stk_enemy_useful_cached, "stalker/enemy/useful_cached")
// 22-game-thread.md, step 4d (urgent near heap, xrEngine/xrSheduler.cpp): inside the near class those who have to
// react go first. urgent_items = the urgent near heap; urgent_creatures = the living near creatures urgent right now
// (ISheduled::shedule_Urgent) in both near heaps; urgent_steps = steps from the urgent heap; phase_urgent = its pass
// (inside the budget, before phase_near). The lateness of the living near creatures by their urgency when they run
// (not by their heap): urgent_late_ms / urgent_late_n and near_calm_late_ms / near_calm_late_n. urgent_in /
// urgent_out = moved into / out of the urgent heap by the scans (not the requeue).
PERF_GAUGE(shed_urgent_items, "shed/urgent_items")
PERF_GAUGE(shed_urgent_creatures, "shed/urgent_creatures")
PERF_COUNTER(shed_urgent_steps, "shed/urgent_steps")
PERF_TIMER(shed_phase_urgent, "shed/phase_urgent")
PERF_COUNTER(shed_urgent_late_ms, "shed/urgent_late_ms")
PERF_COUNTER(shed_urgent_late_n, "shed/urgent_late_n")
PERF_COUNTER(shed_calm_late_ms, "shed/near_calm_late_ms")
PERF_COUNTER(shed_calm_late_n, "shed/near_calm_late_n")
PERF_COUNTER(shed_urgent_in, "shed/urgent_in")
PERF_COUNTER(shed_urgent_out, "shed/urgent_out")
// Crowd broadphase (plans/optimization_mt/21-physics-step.md, journal 2026-10-15; CPHObject::CollideDynamicsBroadphase).
// aabb_reject = q_box results dropped by the exact AABB test. pair/* = the candidates that go on to NearCallback by
// pair type, the frame step only: ai_ai, ai_ragdoll, ai_other (an NPC controller x the actor, items, props, static
// shells), ragdoll (a ragdoll x anything but an NPC controller), other. collide_ai = Collide() of
// live NPC controllers (q_box + pairs + level mesh), static_ai = their level mesh part (inside physics/step/static).
PERF_COUNTER(ph_broadphase_aabb_reject, "physics/broadphase/aabb_reject")
PERF_COUNTER(ph_bp_pair_ai_ai, "physics/broadphase/pair/ai_ai")
PERF_COUNTER(ph_bp_pair_ai_ragdoll, "physics/broadphase/pair/ai_ragdoll")
PERF_COUNTER(ph_bp_pair_ai_other, "physics/broadphase/pair/ai_other")
PERF_COUNTER(ph_bp_pair_ragdoll, "physics/broadphase/pair/ragdoll")
PERF_COUNTER(ph_bp_pair_other, "physics/broadphase/pair/other")
PERF_TIMER(ph_step_collide_ai, "physics/step/collide_ai")
PERF_TIMER(ph_step_static_ai, "physics/step/static_ai")
// Stretched ragdolls (plans/optimization_mt/21-physics-step.md, 2026-10-15). collide_static/starved = level mesh
// collisions of an object whose island had no joint budget left (JOINTS_LIMIT 1500: no level contacts at all before
// the reserve of 32 level contacts in CollideStatic); reserve_contacts = the level contacts added over the limit by it.
// corpse_vel_cap = corpses whose root speed is >= 99% of default_l_limit (150 m/s), counted per frame in
// CCharacterPhysicsSupport::in_UpdateCL (avg/frame = such corpses per frame). corpse_stretch/*: the watchdog
// (CPHWorld::CheckActiveCorpses) - vel_cap = ragdoll substeps with an element >= 95% of the cap, detected = series of
// stretched checks started, repaired = ragdolls put back together, gave_up = put to sleep after 3 repairs in 5 s;
// corpse_check = its pass. corpse_near/awake_old (gauge) = active ragdolls older than ph_corpse_sleep_ms in the last
// substep of the frame (counted by the watchdog pass; the name is kept for log comparisons).
PERF_COUNTER(ph_static_starved, "physics/collide_static/starved")
PERF_COUNTER(ph_static_reserve_contacts, "physics/collide_static/reserve_contacts")
PERF_COUNTER(ph_corpse_vel_cap, "physics/corpse_vel_cap")
PERF_COUNTER(ph_corpse_stretch_vel_cap, "physics/corpse_stretch/vel_cap")
PERF_COUNTER(ph_corpse_stretch_detected, "physics/corpse_stretch/detected")
PERF_COUNTER(ph_corpse_stretch_repaired, "physics/corpse_stretch/repaired")
PERF_COUNTER(ph_corpse_stretch_gave_up, "physics/corpse_stretch/gave_up")
PERF_TIMER(ph_corpse_check, "physics/corpse_check")
PERF_GAUGE(ph_corpse_near_awake_old, "physics/corpse_near/awake_old")
// F1 (plans/optimization_mt/24-critical-path.md, 21-physics-step.md): CPHActivationShape::Activate.
// local_path = activations pushed out on the shape alone; fallback = the frozen-world path ran instead (un_freeze_later,
// a step in flight). step_time = one push-out step (both paths, the
// physics/activation_shape/steps loop); step_objects = objects that ran Collide() in those steps (world path: the shape +
// everything woken so far; local: 1); step_neighbours = neighbours the local shape collided as static obstacles;
// woken = objects the world path left awake when it ended (the shape not counted).
PERF_COUNTER(ph_act_shape_local_path, "physics/activation_shape/local")
PERF_COUNTER(ph_act_shape_fallback, "physics/activation_shape/fallback")
PERF_TIMER(ph_act_shape_step_time, "physics/activation_shape/step_time")
PERF_COUNTER(ph_act_shape_step_objects, "physics/activation_shape/step_objects")
PERF_COUNTER(ph_act_shape_step_neighbours, "physics/activation_shape/step_neighbours")
PERF_COUNTER(ph_act_shape_woken, "physics/activation_shape/woken")
// Launched NPCs and corpses (plans/optimization_mt/21-physics-step.md, 2026-10-15; the frame step only, except
// ai_rise_capped). near/same_island_fixed = pairs already in one island that got contacts only because CanMerge no longer
// counts that island twice; near/merge_reject_same = the refused pairs (inside
// near/merge_reject) that were already in one island. merge_reject_fix/*: refused pairs collided anyway
// (not two live NPC controllers) - pairs (= same + static + split), same = one island without budget (a contact between the
// bodies over JOINTS_LIMIT), static = the other object asleep (one-sided for obj1), split = two active islands (one
// one-sided joint per body), contacts = their contact points, oneway_full = one-sided pairs against a force-slept corpse
// that got the same reserve in a full island. contact/vel_capped = contact joints whose depth was cut to the push-out
// cap (ph_contact_max_correct_vel); contact/max_depth_mm = per-frame maximum of the geometric depth of the contacts that
// got a joint (level mesh included). ai_rise_capped = live NPC controller steps whose rise speed was cut
// (ph_ai_max_rise_vel), any step.
PERF_COUNTER(ph_near_same_island_fixed, "physics/near/same_island_fixed")
PERF_COUNTER(ph_near_merge_reject_same, "physics/near/merge_reject_same")
PERF_COUNTER(ph_reject_fix_pairs, "physics/merge_reject_fix/pairs")
PERF_COUNTER(ph_reject_fix_same, "physics/merge_reject_fix/same")
PERF_COUNTER(ph_reject_fix_static, "physics/merge_reject_fix/static")
PERF_COUNTER(ph_reject_fix_split, "physics/merge_reject_fix/split")
PERF_COUNTER(ph_reject_fix_contacts, "physics/merge_reject_fix/contacts")
PERF_COUNTER(ph_reject_fix_oneway_full, "physics/merge_reject_fix/oneway_full")
PERF_COUNTER(ph_contact_vel_capped, "physics/contact/vel_capped")
PERF_GAUGE(ph_contact_max_depth_mm, "physics/contact/max_depth_mm")
PERF_COUNTER(ph_ai_rise_capped, "physics/ai_rise_capped")
// Island list guards (21-physics-step.md, 2026-10-15; crash AV 0xB4 in quickstep compute_invM_JT). island/bad_lists_skipped
// = island solves skipped by CPHIsland::Step because a joint in the island list has no body 1 (a detached contact) or
// a body of another island, or a list is longer than nb / nj (any step; the first lines are also logged).
// contact/foreign_body_dropped = contacts CollideIntoGroup did not attach because a body of the pair is in neither of the
// islands the joint may go to (any step, logged too).
PERF_COUNTER(ph_island_bad_lists_skipped, "physics/island/bad_lists_skipped")
PERF_COUNTER(ph_contact_foreign_body_dropped, "physics/contact/foreign_body_dropped")
// Live NPCs left high in the air (plans/optimization_mt/21-physics-step.md, 2026-10-15; xrGame movement_manager_physic.cpp,
// ground_snap(), always on). npc_ground/snap_kinematic = kinematic moves (far / unseen / no neighbours / TryPosition skipped,
// and an idle disabled NPC) whose destination hung more than ai_npc_ground_snap_height above the AI floor and was
// lowered toward it; snap_try = the same for a TryPosition destination that then succeeded (no gravity in its local
// step either); snap_excluded = such a hanging NPC left as is (jump, physics control, impulse, losing control, capture,
// ladder, animation movement, smart cover, poltergeist, attached, the current entity); no_floor = no reliable AI floor
// under it (no level vertex, off the AI map). npc_ground/high = updates of live NPCs more than 3 m above the floor;
// max_height_mm = the highest of them in the frame. lift/* = an NPC crossing 3 m above the floor, by how the crossing
// update moved it: kinematic (SetPosition branches), try_position (a successful TryPosition), physics (Calculate, the
// frame step), other (not moved by move_along_path); lift/crowd = crossings with another live NPC within 1 m (xz),
// lift/stacked = with one of them at least 0.5 m below (standing on it). The first 16 crossings of a session are logged
// ("! [npc_ground]"). physics/ai_rise_slow = live NPC controller steps rising faster than 1.5 m/s under the
// ph_ai_max_rise_vel cap without a jump, impulse or ladder (a slow lift ai_rise_capped misses); _on_object = of them
// standing on another object or character (b_on_object), _climb = of them in the step-up climb (b_clamb_jump).
PERF_COUNTER(ai_npc_ground_snap_kinematic, "ai/npc_ground/snap_kinematic")
PERF_COUNTER(ai_npc_ground_snap_try, "ai/npc_ground/snap_try")
PERF_COUNTER(ai_npc_ground_snap_excluded, "ai/npc_ground/snap_excluded")
PERF_COUNTER(ai_npc_ground_no_floor, "ai/npc_ground/no_floor")
PERF_COUNTER(ai_npc_ground_high, "ai/npc_ground/high")
PERF_GAUGE(ai_npc_ground_max_height_mm, "ai/npc_ground/max_height_mm")
PERF_COUNTER(ai_npc_ground_lift_kinematic, "ai/npc_ground/lift/kinematic")
PERF_COUNTER(ai_npc_ground_lift_try, "ai/npc_ground/lift/try_position")
PERF_COUNTER(ai_npc_ground_lift_physics, "ai/npc_ground/lift/physics")
PERF_COUNTER(ai_npc_ground_lift_other, "ai/npc_ground/lift/other")
PERF_COUNTER(ai_npc_ground_lift_crowd, "ai/npc_ground/lift/crowd")
PERF_COUNTER(ai_npc_ground_lift_stacked, "ai/npc_ground/lift/stacked")
PERF_COUNTER(ph_ai_rise_slow, "physics/ai_rise_slow")
PERF_COUNTER(ph_ai_rise_slow_on_object, "physics/ai_rise_slow_on_object")
PERF_COUNTER(ph_ai_rise_slow_climb, "physics/ai_rise_slow_climb")
// Live NPCs thrown up in a dense crowd (plans/optimization_mt/21-physics-step.md, 2026-10-16; xrPhysics
// PHSimpleCharacter.cpp / PHAICharacter.cpp). ai_rise_rollback = ph_ai_max_rise_vel cuts that also took back the rise of
// the substep; ai_rise_rollback_max_mm = the largest rise taken back in one substep, in the frame;
// ai_climb_char_skipped = contacts of a live NPC controller with another character's capsule left without the
// step-climb override; ai_step_rise_restored = live NPC controllers put back after rising more
// than ph_ai_step_max_rise in one frame substep; try_position/jump_rejected = TryPosition results refused by
// ph_ai_try_max_dy. The first 16 rollbacks over 0.5 m, restores and rejections are logged ("! [ai_jump]").
PERF_COUNTER(ph_ai_rise_rollback, "physics/ai_rise_rollback")
PERF_GAUGE(ph_ai_rise_rollback_max_mm, "physics/ai_rise_rollback_max_mm")
PERF_COUNTER(ph_ai_climb_char_skipped, "physics/ai_climb_char_skipped")
PERF_COUNTER(ph_ai_step_rise_restored, "physics/ai_step_rise_restored")
PERF_COUNTER(ph_try_jump_rejected, "physics/try_position/jump_rejected")
// 24-critical-path.md, G2 (xrGame ai_stalker_fire.cpp): the friendly-fire pick. npc/ff/cache_hit = picks served by the
// pick cache (not in npc/ff/pick, not in npc/object_handler/pick_full); cache_miss_* = why a pick was cast: age
// (npc_ff_pick_cache_ms / _frames), target (another enemy or item, no pick yet), moved (muzzle > 0.5 m, aim turned
// more than the side ray angle), actor (the actor's fight, the view entity, the actor near the line of fire).
// npc/ff/static = the level geometry part of the rays; precise_behind = precise tests of objects past the enemy
// distance + npc_ff_pick_cut_behind (the actor's full-length rays); cut_skipped = objects a ray crossed but left out
// by the cut; cut_full = rays kept full length for the actor.
PERF_COUNTER(npc_ff_cache_hit, "npc/ff/cache_hit")
PERF_COUNTER(npc_ff_cache_miss_age, "npc/ff/cache_miss_age")
PERF_COUNTER(npc_ff_cache_miss_target, "npc/ff/cache_miss_target")
PERF_COUNTER(npc_ff_cache_miss_moved, "npc/ff/cache_miss_moved")
PERF_COUNTER(npc_ff_cache_miss_actor, "npc/ff/cache_miss_actor")
PERF_TIMER(npc_ff_static, "npc/ff/static")
PERF_COUNTER(npc_ff_precise_behind, "npc/ff/precise_behind")
PERF_COUNTER(npc_ff_cut_skipped, "npc/ff/cut_skipped")
PERF_COUNTER(npc_ff_cut_full, "npc/ff/cut_full")
// AI sound dispatch (plans/optimization_mt/24-critical-path.md, G3; xrGame Level.cpp CLevel::SoundEvent_Filter,
// sound_memory_manager.cpp; xrEngine IGame_Level.cpp). kept_combat / kept_other / kept_actor = the deliveries that go
// out (after all cuts) by the class of the sound: combat (shot, bullet hit, explosion, monster attack / injury / death),
// other (steps, voices, reloads, items, world), the actor's own; noncombat_cut = non-combat deliveries dropped by
// npc_sound_noncombat_dist that the old rules (near_dist, actor distance) would keep; listener_capped = dropped by
// npc_sound_noncombat_per_listener. Timers of one delivery: t_memory = CSoundMemoryManager::feel_sound_new, inside
// it t_callback = CScriptEntity::sound_callback (the hear callback queue) and t_add = the sound memory insertion, inside
// that t_visible = the visual memory search (visible_now); calls = deliveries reaching each part. The rest of
// game/sound_dispatch is npc/sound/filter and the dispatch loop with CCustomMonster::feel_sound_new (cache misses on
// the listener). sound/reg/remote_split = remote sounds (bullet hits) let through the coalescence by their own key
// that the object key alone would have merged (the burst / budget gates may still drop them).
PERF_COUNTER(npc_sound_kept_combat, "npc/sound/kept_combat")
PERF_COUNTER(npc_sound_kept_other, "npc/sound/kept_other")
PERF_COUNTER(npc_sound_kept_actor, "npc/sound/kept_actor")
PERF_COUNTER(npc_sound_noncombat_cut, "npc/sound/noncombat_cut")
PERF_COUNTER(npc_sound_listener_capped, "npc/sound/listener_capped")
PERF_TIMER(npc_sound_t_memory, "npc/sound/t_memory")
PERF_TIMER(npc_sound_t_callback, "npc/sound/t_callback")
PERF_TIMER(npc_sound_t_add, "npc/sound/t_add")
PERF_TIMER(npc_sound_t_visible, "npc/sound/t_visible")
PERF_COUNTER(snd_reg_remote_split, "sound/reg/remote_split")
// Dense crowd of live NPCs (plans/optimization_mt/21-physics-step.md, 2026-10-16; xrPhysics Physics.cpp), diagnostics
// only. wake/ai_by_ai = sleeping live NPCs woken by another live NPC's contact in the frame step (a part of
// physics/wake/ai); wake/ai_by_ai_probe = the same in the TryPosition probes on main (DoCollideObj / step_single, not
// in physics/wake/*).
PERF_COUNTER(ph_wake_ai_by_ai, "physics/wake/ai_by_ai")
PERF_COUNTER(ph_wake_ai_by_ai_probe, "physics/wake/ai_by_ai_probe")
// UpdateCL of stalkers and weapons on main (plans/optimization_mt/24-critical-path.md, M1 / M2 / M4 and F2 / F3,
// 2026-10-16). stalker/ucl_lod/* = one decision per CAI_Stalker::TrySkipUpdateCLLod of a living stalker without a
// parent: near (within npc_perf_ucl_lod_dist, full), full (the own frame of its tier), skip_d1 / skip_d2 (skipped by
// the distance tiers), skip_combat (skipped by the combat tier npc_ucl_lod_combat), deny_combat (a fight, the combat
// tier refused or npc_ucl_lod_combat < 2), deny_other (jump, physics control, view entity, svp, spawn, animation
// movement, bone particles, a busy weapon outside the combat tier), deny_timecap. stalker/ucl_lod/combat/* = the near
// or fighting ones checked for the combat tier (with npc_ucl_lod_combat < 2 too while perf_metrics runs): cand = would
// skip by it, deny_seen = drawn in the main pass last frame (or svp), deny_shadow_near = drawn only in shadows /
// lights within npc_perf_ucl_lod_dist, deny_actor = its enemy is the actor, deny_hit = hit within 1.5 s, deny_other =
// the list above. stalker/ucl/n_* = full UpdateCL runs by what the render did with the stalker last frame (dead apart).
// stalker/ucl/{object_handler,inherited,physics,sight,exec_look,step_manager} = the stages of a full UpdateCL.
PERF_COUNTER(stk_ucl_lod_near, "stalker/ucl_lod/near")
PERF_COUNTER(stk_ucl_lod_full, "stalker/ucl_lod/full")
PERF_COUNTER(stk_ucl_lod_skip_d1, "stalker/ucl_lod/skip_d1")
PERF_COUNTER(stk_ucl_lod_skip_d2, "stalker/ucl_lod/skip_d2")
PERF_COUNTER(stk_ucl_lod_skip_combat, "stalker/ucl_lod/skip_combat")
PERF_COUNTER(stk_ucl_lod_deny_combat, "stalker/ucl_lod/deny_combat")
PERF_COUNTER(stk_ucl_lod_deny_other, "stalker/ucl_lod/deny_other")
PERF_COUNTER(stk_ucl_lod_deny_timecap, "stalker/ucl_lod/deny_timecap")
PERF_COUNTER(stk_ucl_lod_combat_cand, "stalker/ucl_lod/combat/cand")
PERF_COUNTER(stk_ucl_lod_combat_seen, "stalker/ucl_lod/combat/deny_seen")
PERF_COUNTER(stk_ucl_lod_combat_shadow, "stalker/ucl_lod/combat/deny_shadow_near")
PERF_COUNTER(stk_ucl_lod_combat_actor, "stalker/ucl_lod/combat/deny_actor")
PERF_COUNTER(stk_ucl_lod_combat_hit, "stalker/ucl_lod/combat/deny_hit")
PERF_COUNTER(stk_ucl_lod_combat_other, "stalker/ucl_lod/combat/deny_other")
PERF_COUNTER(stk_ucl_n_onscreen, "stalker/ucl/n_onscreen")
PERF_COUNTER(stk_ucl_n_shadow_only, "stalker/ucl/n_shadow_only")
PERF_COUNTER(stk_ucl_n_offscreen, "stalker/ucl/n_offscreen")
PERF_COUNTER(stk_ucl_n_dead, "stalker/ucl/n_dead")
PERF_TIMER(stk_ucl_object_handler, "stalker/ucl/object_handler")
PERF_TIMER(stk_ucl_inherited, "stalker/ucl/inherited")
PERF_TIMER(stk_ucl_physics, "stalker/ucl/physics")
PERF_TIMER(stk_ucl_sight, "stalker/ucl/sight")
PERF_TIMER(stk_ucl_exec_look, "stalker/ucl/exec_look")
PERF_TIMER(stk_ucl_step_manager, "stalker/ucl/step_manager")
// weapon/ucl/* = the parts of a full CWeapon / CWeaponMagazined UpdateCL: inherited (CHudItemObject chain: spatial,
// crow, HUD motions), addons_vis (HUD addons, laser dots), light_flame (shot light, flame particles), state (the
// CWeaponMagazined state machine, state_Fire), sounds (UpdateSounds). bones/caller/weapon/{sounds,fire,flame,other} =
// the owner's CalculateBones in CWeapon::UpdateXForm (the whole call: UpdateTracks + the bone math when due) by who
// asked: UpdateSounds, the state machine (state_Fire), the flame / light, anything else (render, Game thread, events).
// sounds_* = the UpdateSounds calls that need the owner's bones, by the owner: prepass = the bones prepass computes
// them this frame anyway, unseen = drawn in no pass last frame (both left to F3), seen = drawn but not by the prepass,
// exact = the actor, the view entity, a dead owner, svp (both computed). weapon/xform_lazy/* = F3 (always on): XFORMs
// built from the owner's last bones (prepass / offscreen), reused in the same frame, upgraded later to the exact one
// (render prepare, Game thread).
PERF_TIMER(wpn_ucl_inherited, "weapon/ucl/inherited")
PERF_TIMER(wpn_ucl_addons_vis, "weapon/ucl/addons_vis")
PERF_TIMER(wpn_ucl_light_flame, "weapon/ucl/light_flame")
PERF_TIMER(wpn_ucl_state, "weapon/ucl/state")
PERF_TIMER(wpn_ucl_sounds, "weapon/ucl/sounds")
PERF_TIMER(wpn_bones_sounds, "bones/caller/weapon/sounds")
PERF_TIMER(wpn_bones_fire, "bones/caller/weapon/fire")
PERF_TIMER(wpn_bones_flame, "bones/caller/weapon/flame")
PERF_TIMER(wpn_bones_other, "bones/caller/weapon/other")
PERF_COUNTER(wpn_bones_sounds_prepass, "bones/caller/weapon/sounds_prepass")
PERF_COUNTER(wpn_bones_sounds_unseen, "bones/caller/weapon/sounds_unseen")
PERF_COUNTER(wpn_bones_sounds_seen, "bones/caller/weapon/sounds_seen")
PERF_COUNTER(wpn_bones_sounds_exact, "bones/caller/weapon/sounds_exact")
PERF_COUNTER(wpn_xform_lazy_prepass, "weapon/xform_lazy/prepass")
PERF_COUNTER(wpn_xform_lazy_offscreen, "weapon/xform_lazy/offscreen")
PERF_COUNTER(wpn_xform_lazy_reused, "weapon/xform_lazy/reused")
PERF_COUNTER(wpn_xform_lazy_upgraded, "weapon/xform_lazy/upgraded")
// physics/broadphase/skip/* (21-physics-step.md, 2026-10-16; permanent, limits ph_corpse_vs_corpse_dist / _npc_dist):
// candidates dropped in CPHObject::CollideDynamicsBroadphase after the exact AABB filter, i.e. NearCallback calls saved.
// corpse_corpse - corpse x corpse; corpse_npc - corpse x live NPC controller beyond ph_corpse_vs_npc_dist (both: the
// frame step, the same condition as pair/*, so pair/ragdoll_ragdoll + skip/corpse_corpse and pair/ai_ragdoll +
// skip/corpse_npc are the counts without the rules); corpse_npc_local - the same on the other paths (TryPosition probes,
// world steps on main). pair/ragdoll_ragdoll - the corpse x corpse part of pair/ragdoll that still reaches NearCallback.
PERF_COUNTER(ph_bp_skip_corpse_corpse, "physics/broadphase/skip/corpse_corpse")
PERF_COUNTER(ph_bp_skip_corpse_npc, "physics/broadphase/skip/corpse_npc")
PERF_COUNTER(ph_bp_skip_corpse_npc_local, "physics/broadphase/skip/corpse_npc_local")
PERF_COUNTER(ph_bp_pair_ragdoll_ragdoll, "physics/broadphase/pair/ragdoll_ragdoll")
// Parallel post-integrate (plans/optimization_mt/21-physics-step.md, journal 2026-10-16; permanent, frame
// step only). post_mt/cpu = the parallel part (IslandReinit + PhDataUpdate + spatial bounds) in the island-solve tasks,
// summed over the threads - it is inside physics/step/solve then, and physics/step/post_integrate is the serial rest.
// objects = objects whose post-integrate ran there, offthread = of them on a pool worker; serial = objects left wholly
// to the serial loop (not a shell / controller, or not in a solved group); deactivate / rebuild = deferred list removals
// of controllers and octree rebuilds applied by the serial pass.
PERF_TIMER(ph_post_mt_cpu, "physics/post_mt/cpu")
PERF_COUNTER(ph_post_mt_objects, "physics/post_mt/objects")
PERF_COUNTER(ph_post_mt_offthread, "physics/post_mt/offthread")
PERF_COUNTER(ph_post_mt_serial, "physics/post_mt/serial")
PERF_COUNTER(ph_post_mt_deactivate, "physics/post_mt/deactivate")
PERF_COUNTER(ph_post_mt_rebuild, "physics/post_mt/rebuild")
PERF_COUNTER(ph_post_mt_split, "physics/post_mt/split")     // groups of > 16 objects whose objects went to the pool in chunks
// K5 of plans/optimization_mt/26-framemove-game-next.md (1-B, temporary switch wpn_npc_ucl_lite; counted only while it
// is on): weapon/lite/sounds_quiet - CWeaponMagazined::UpdateSounds of a weapon not in the actor's hands with nothing of
// m_sounds playing (the SetPosition lookups left out); fire_lazy - state_Fire frames of such a weapon between the shots
// (no fire point, no g_fireParams); fire_exact - its state_Fire frames that may shoot; addons_npc - CWeapon::UpdateCL
// without UpdateHUDAddonsVisibility / LaserDotsAllowed. The time is in weapon/ucl/{sounds,state,addons_vis}.
PERF_COUNTER(wpn_lite_sounds_quiet, "weapon/lite/sounds_quiet")
PERF_COUNTER(wpn_lite_fire_lazy, "weapon/lite/fire_lazy")
PERF_COUNTER(wpn_lite_fire_exact, "weapon/lite/fire_exact")
PERF_COUNTER(wpn_lite_addons_npc, "weapon/lite/addons_npc")
// 26-framemove-game-next.md, package 1-A (plans/optimization_mt, 2026-10-16): the parts of FrameMove and of the Game
// thread that M1-M12 left unmeasured. Diagnostics only, no behaviour change; every timer is per stage, calls = runs.
// cm/ucl/* (M3) = CCustomMonster::UpdateCL of stalkers and monsters (for stalkers inside stalker/ucl/inherited):
// inherited = CEntityAlive..CGameObject::UpdateCL, position_anim = UpdatePositionAnimation (the movement on the frame and
// the animation selection), select_anim = SelectAnimation inside it (calls = selections the animation LOD let run),
// anim_mov_ctrl = update_animation_movement_controller.
PERF_TIMER(cm_ucl_inherited, "cm/ucl/inherited")
PERF_TIMER(cm_ucl_position_anim, "cm/ucl/position_anim")
PERF_TIMER(cm_ucl_select_anim, "cm/ucl/select_anim")
PERF_TIMER(cm_ucl_anim_mov_ctrl, "cm/ucl/anim_mov_ctrl")
// movement/* (M3) = CMovementManager::move_along_path_body on a path (inside cm/ucl/position_anim): nearest = the neighbour
// query (ObjectSpace.GetNearest); collision_move = the branch with neighbours, calls = such frames (TryPosition, and the
// frame step Calculate when it fails); try_fail = that failed part (Calculate + the collision hit), calls = failures, so
// TryPosition itself ~ collision_move - try_fail; try_skip = frames whose TryPosition the npc_perf_move_try_skip_frames
// budget skipped.
PERF_TIMER(mv_nearest, "movement/nearest")
PERF_TIMER(mv_collision_move, "movement/collision_move")
PERF_TIMER(mv_try_fail, "movement/try_fail")
PERF_COUNTER(mv_try_skip, "movement/try_skip")
// CCharacterPhysicsSupport::in_UpdateCL (M3; stalker/ucl/physics for stalkers, the actor and monsters too): ik/update =
// CIKLimbsController::Update, calls = IK runs; ik/interval_skip = calls with an IK controller whose IK interval did not
// come; cph/ucl/anim_collision = update_animation_collision; cph/ucl/ragdoll = the ragdoll of the dead (SetRagDoll, the
// interpolation, death animations, friction).
PERF_TIMER(ik_update, "ik/update")
PERF_COUNTER(ik_interval_skip, "ik/interval_skip")
PERF_TIMER(cph_ucl_anim_collision, "cph/ucl/anim_collision")
PERF_TIMER(cph_ucl_ragdoll, "cph/ucl/ragdoll")
// The rest of ucl/stalker (CAI_Stalker::UpdateCL): lod_decide = TrySkipUpdateCLLod of every call, lod_skip_path inside it
// = a skipped frame (UpdateCLSkippedRoot + the recoil relax of the combat tier); dead = the whole full UpdateCL of a dead
// stalker (its stalker/ucl/inherited and stalker/ucl/physics included); danger_sound = the run-in-danger voice;
// shot_effector = the recoil effector of a full UpdateCL.
PERF_TIMER(stk_ucl_lod_decide, "stalker/ucl/lod_decide")
PERF_TIMER(stk_ucl_lod_skip_path, "stalker/ucl/lod_skip_path")
PERF_TIMER(stk_ucl_dead, "stalker/ucl/dead")
PERF_TIMER(stk_ucl_danger_sound, "stalker/ucl/danger_sound")
PERF_TIMER(stk_ucl_shot_effector, "stalker/ucl/shot_effector")
// The rest of ucl/weapon (declared for the weapon files, K5): skip_check = TrySkipHiddenUpdateCL() ||
// TrySkipOwnerLodUpdateCL() at the top of CWeapon / CWeaponMagazined::UpdateCL (the check and the work of a skipped
// frame), skipped = the UpdateCL calls it ended.
PERF_TIMER(wpn_ucl_skip_check, "weapon/ucl/skip_check")
PERF_COUNTER(wpn_ucl_skipped, "weapon/ucl/skipped")
// CObjectList::SingleUpdate (inside objects/update/single): classify = choosing the ucl/* bucket (the cast chain, only
// while perf_metrics runs); visits = SingleUpdate calls (the workload and the parents reached from their children),
// skip_done = of them already updated this frame, skip_disabled = processing disabled. objects/update/single - ucl/* -
// classify = the walk itself (the first touch of every object, the parent recursion).
PERF_TIMER(ol_upd_classify, "objects/update/classify")
PERF_COUNTER(ol_upd_visits, "objects/update/visits")
PERF_COUNTER(ol_upd_skip_done, "objects/update/skip_done")
PERF_COUNTER(ol_upd_skip_disabled, "objects/update/skip_disabled")
// ge/time/* (M6) = CLevel::ProcessGameEvents (frame/level/game_events) by message, calls = messages: spawn = M_SPAWN;
// M_EVENT by type: hit = GE_HIT, die = GE_DIE / GE_ASSIGN_KILLER, take = GE_OWNERSHIP_TAKE* / GE_TRADE_BUY, reject =
// GE_OWNERSHIP_REJECT / GE_TRADE_SELL, destroy = GE_DESTROY / GE_DESTROY_REJECT, weapon = GE_WPN_* / GE_ADDON_* /
// GE_INSTALL_UPGRADE / GE_TRANSFER_AMMO / GE_GRENADE_EXPLODE, event_other = any other GE_*; other = other messages.
PERF_TIMER(ge_time_spawn, "ge/time/spawn")
PERF_TIMER(ge_time_hit, "ge/time/hit")
PERF_TIMER(ge_time_die, "ge/time/die")
PERF_TIMER(ge_time_take, "ge/time/take")
PERF_TIMER(ge_time_reject, "ge/time/reject")
PERF_TIMER(ge_time_destroy, "ge/time/destroy")
PERF_TIMER(ge_time_weapon, "ge/time/weapon")
PERF_TIMER(ge_time_event_other, "ge/time/event_other")
PERF_TIMER(ge_time_other, "ge/time/other")
// map/* (M7) = CMapManager::Update (frame/level/map_manager): locations_time = CMapLocation::Update of every location
// (map/locations = their count), relation_time = ResolveRelationSpotChecks (map/relation = the checks), tail = the
// deferred deletes of the previous frame, the sort and the removal of expired locations (calls = 2 a frame).
PERF_TIMER(map_locations_time, "map/locations_time")
PERF_TIMER(map_relation_time, "map/relation_time")
PERF_TIMER(map_tail, "map/tail")
// The maps of the UI (M7; the minimap is inside hud/ui/main_wnd): ui/minimap/update_spots = CUIMiniMap::UpdateSpots
// (DetachAll + UpdateMiniMap of every location), ui/minimap/detach = its DetachAll; ui/map/update = CUICustomMap::Update
// of any map (the minimap every frame, the PDA maps while open), ui/map/children = its CUIStatic::Update (spot windows).
PERF_TIMER(ui_minimap_update_spots, "ui/minimap/update_spots")
PERF_TIMER(ui_minimap_detach, "ui/minimap/detach")
PERF_TIMER(ui_map_update, "ui/map/update")
PERF_TIMER(ui_map_children, "ui/map/children")
// sound/update/* (M7) = CSoundRender_Core::update (frame/move/sound): lock_wait = waiting for m_emitters_lock (held by the
// physics thread's play_at_pos, the Game thread's render), targets = the emitters on voices, emitters = the rest of the
// scene emitters (and the deletion of stopped ones; calls = scenes), listener = update_listener, register = the events of
// the scenes (SoundEvent_Register: q_box, occlusion rays). sound/render/lock_wait = the same lock in
// CSoundRender_Core::render.
PERF_TIMER(snd_update_lock_wait, "sound/update/lock_wait")
PERF_TIMER(snd_update_targets, "sound/update/targets")
PERF_TIMER(snd_update_emitters, "sound/update/emitters")
PERF_TIMER(snd_update_listener, "sound/update/listener")
PERF_TIMER(snd_update_register, "sound/update/register")
PERF_TIMER(snd_render_lock_wait, "sound/render/lock_wait")
// game/seq_frame_mt/* = the subscribers of seqFrameMT (game/seq_frame_mt): sound = GEnv.Sound->render(), net =
// CLevel::net_Update (the client send and the server update; nearly empty in single player; on main with mtNetwork off).
PERF_TIMER(game_seq_frame_mt_sound, "game/seq_frame_mt/sound")
PERF_TIMER(game_seq_frame_mt_net, "game/seq_frame_mt/net")
// npc/sound/* inside t_memory (CSoundMemoryManager::feel_sound_new): t_visitor = the sound user data visitor
// (CStalkerSoundDataVisitor: the danger and the enemy of the sounding NPC), t_hit_add = the hit memory entry of an enemy
// shot. t_memory - t_callback - t_add - t_visitor - t_hit_add = the threshold decay and the type factors.
PERF_TIMER(npc_sound_t_visitor, "npc/sound/t_visitor")
PERF_TIMER(npc_sound_t_hit_add, "npc/sound/t_hit_add")
// M13: log/add by the thread that logs (main / Game thread / workers and the rest); log/perf_dump = the 5 s dump of
// perf_metrics itself on main (its lines are counted in log/add and log/add/main of the frame after the dump).
PERF_TIMER(log_add_main, "log/add/main")
PERF_TIMER(log_add_game, "log/add/game")
PERF_TIMER(log_add_worker, "log/add/worker")
PERF_TIMER(log_perf_dump, "log/perf_dump")
// K7 (plans/optimization_mt/26-framemove-game-next.md, 1-C). CAI_Stalker::best_cover went to search: why/none = no
// current cover, why/bad = the current cover failed the checks against the threat (too close, worse by 1, no
// loophole), why/event = an event dropped it (enemy change, danger location, blocked by a member, restrictions,
// planner). scan = searches that scanned the cover points (the advance 10 m search not counted), none_found = scans
// that found no cover, inertia_kept = inertia answers taken without a scan, inertia_rescan = inertia answers refused
// (another cover, or the current one is bad now) and scanned, none_hold = searches skipped after a scan that found
// nothing (npc_cover_none_hold_ms). Searches avoided = inertia_kept + none_hold.
PERF_COUNTER(stk_cover_why_none, "stalker/cover/why/none")
PERF_COUNTER(stk_cover_why_bad, "stalker/cover/why/bad")
PERF_COUNTER(stk_cover_why_event, "stalker/cover/why/event")
PERF_COUNTER(stk_cover_scan, "stalker/cover/scan")
PERF_COUNTER(stk_cover_none_found, "stalker/cover/none_found")
PERF_COUNTER(stk_cover_inertia_kept, "stalker/cover/inertia_kept")
PERF_COUNTER(stk_cover_inertia_rescan, "stalker/cover/inertia_rescan")
PERF_COUNTER(stk_cover_none_hold, "stalker/cover/none_hold")
// The useful cache of CEnemyManager: useful_miss = lookups not answered by it (hit rate = useful_cached /
// (useful_cached + useful_miss)), useful_drop = entries dropped by relcase and enemy change (the whole cache is
// cleared on both).
PERF_COUNTER(stk_enemy_useful_miss, "stalker/enemy/useful_miss")
PERF_COUNTER(stk_enemy_useful_drop, "stalker/enemy/useful_drop")
// K8 (plans/optimization_mt/26-framemove-game-next.md, 2-B; ai/stalker/ai_stalker.cpp). K8a: take_seen_far = fighters
// drawn in the main pass last frame, at least npc_ucl_lod_combat_seen_dist m from the camera (zoomed distance; 50 m
// while the switch is 0 - how many it would take, they stay in deny_seen then), not refused for the actor as the enemy,
// a recent hit or an every-frame case; skip_seen_far = their skipped UpdateCL (inside skip_combat). K8c (npc_oh_lod_combat):
// object handler runs of fighters farther than npc_perf_object_handler_lod_dist with a settled weapon and an actual
// plan - cand_unseen / cand_seen (counted with the switch 0 too, inside keep_fight then), skip_unseen / skip_seen = runs
// skipped (not in npc/object_handler/full).
PERF_COUNTER(stk_ucl_lod_combat_seen_far, "stalker/ucl_lod/combat/take_seen_far")
PERF_COUNTER(stk_ucl_lod_skip_seen_far, "stalker/ucl_lod/skip_seen_far")
PERF_COUNTER(npc_oh_combat_cand_unseen, "npc/object_handler/combat/cand_unseen")
PERF_COUNTER(npc_oh_combat_cand_seen, "npc/object_handler/combat/cand_seen")
PERF_COUNTER(npc_oh_combat_skip_unseen, "npc/object_handler/combat/skip_unseen")
PERF_COUNTER(npc_oh_combat_skip_seen, "npc/object_handler/combat/skip_seen")
// 26-framemove-game-next.md, package 2-A (K2/K3/K4/K9; Level.cpp, lua_spike_probe.cpp, xrEngine EngineThreading.cpp).
// K2a: gc_long_step = single paced GC steps of at least 2 ms (always with perf_metrics; in practice the atomic phase of
// LuaJIT, so max_frame ~ its time and calls ~ GC cycles); gc_capped = frames where lua_gc_step_budget_us stopped the
// steps with debt left, gc_uncapped = frames over the debt limit (paid off without the cap); gc_debt = the steps owed
// after the frame. K2b (lua_gc_overlap): gc_in_early / gc_in_seq = GC steps run in phase 0 of the Game thread / by the
// seqParallel delegate (frames without phase 0). K3 (mt_sound_dispatch_early) / K2b: game/early = phase 0 on the Game
// thread (dispatch + GC; parallel to phase B/C of the prepass and the physics kick on main), early_dispatch = frames
// whose dispatch ran there (game/sound_dispatch then counts it, outside game/total); game/start_delay = from
// KickGameLogicThread to the start of the body (wake latency + what phase 0 did not finish before the kick).
// K4 (mt_prepass_main_slot): prepass/map_update = CMapManager::Update in the phase B window (or on the way out of the
// prepass), prepass/main_slot_wait = main waiting for the rest of phase B after it, map_update_late = updates left to
// the next OnFrame (no prepass in that frame). K9 (mt_sound_render_task): game/sound_render_wait = the Game thread
// waiting for the sound render task before seqFrameMT (the task itself is game/seq_frame_mt/sound).
PERF_TIMER(lua_gc_long_step, "lua/gc_long_step")
PERF_COUNTER(lua_gc_capped, "lua/gc_capped")
PERF_COUNTER(lua_gc_uncapped, "lua/gc_uncapped")
PERF_GAUGE(lua_gc_debt, "lua/gc_debt")
PERF_COUNTER(lua_gc_in_early, "lua/gc_in_early")
PERF_COUNTER(lua_gc_in_seq, "lua/gc_in_seq")
PERF_TIMER(game_early, "game/early")
PERF_COUNTER(game_early_dispatch, "game/early_dispatch")
PERF_TIMER(game_start_delay, "game/start_delay")
PERF_TIMER(game_sound_render_wait, "game/sound_render_wait")
PERF_TIMER(prepass_map_update, "prepass/map_update")
PERF_TIMER(prepass_main_slot_wait, "prepass/main_slot_wait")
PERF_COUNTER(prepass_map_update_late, "prepass/map_update_late")
// 26-framemove-game-next.md, early main build (r__main_build_early; r2_R_calculate.cpp, r__dsgraph_build.cpp): the static
// half of the main view's build (portal traversal + static geometry) runs as a task from right after FrameMove, beside
// the RT scheduler part, the bones prepass and the physics kick. build_early = that task (wall, on its worker; includes
// render/main/build_static), build_late = the dynamic half launched by Calculate() (wall, on its worker; includes
// render/main/build_dynamic), build_early_wait = main waiting for the static half in Calculate() (normally ~0; the
// rest is still render/pass/build_wait). early_frames = frames that took the early path, early_discard = static halves
// dropped unconsumed (an icon/preview render on the immediate dsgraph, a frame without Calculate, reset),
// early_skip_icons = frames kept on the old path because inventory icons were pending, early_lod_diff = frames whose LOD
// thresholds (g_fSCREEN) at Calculate() differed from the ones the static half used.
PERF_TIMER(rmain_build_early, "render/main/build_early")
PERF_TIMER(rmain_build_late, "render/main/build_late")
PERF_TIMER(rmain_build_early_wait, "render/main/build_early_wait")
PERF_COUNTER(rmain_early_frames, "render/main/early_frames")
PERF_COUNTER(rmain_early_discard, "render/main/early_discard")
PERF_COUNTER(rmain_early_skip_icons, "render/main/early_skip_icons")
PERF_COUNTER(rmain_early_lod_diff, "render/main/early_lod_diff")
// Pipelined bones prepass (r__bones_prepass_pipeline, 26-framemove-game-next.md; xrGame/Level.cpp, SkeletonRigid.cpp):
// pipelined = models whose phase B ran on the workers during the UpdateCL loop (the prepass then skips them); redone =
// early bones computed again in the same frame (dirty - by the prepass, invalidated - by the next reader); dirty = early
// bones the prepass recomputed because the pose input changed after them (animation mutator, the owner's XFORM);
// tail_wait = main waiting for the early tasks at the end of the UpdateCL loop; early_main = main's part of the pipeline
// inside FrameMove (root list, phase A after each batch, kicks); early_phase_b = CPU of the early tasks summed over the
// workers; early_cooldown = models left to the prepass after a recent redo; early_nested_skip = nested CalculateBones of
// another model on an early task that would need its phase A (skipped, previous pose).
PERF_COUNTER(prepass_pipelined, "prepass/pipelined")
PERF_COUNTER(prepass_redone, "prepass/redone")
PERF_COUNTER(prepass_dirty, "prepass/dirty")
PERF_TIMER(prepass_tail_wait, "prepass/tail_wait")
PERF_TIMER(prepass_early_main, "prepass/early_main")
PERF_TIMER(prepass_early_phase_b, "prepass/early_phase_b")
PERF_COUNTER(prepass_early_cooldown, "prepass/early_cooldown")
PERF_COUNTER(prepass_early_nested_skip, "prepass/early_nested_skip")
// Why the prepass still computed a model itself in a pipelined frame (per model; with prepass/pipelined it gives the
// pipelined share): tail_own = its object's UpdateCL is in the last chunk of the loop; tail_holder = the object is
// earlier, but its root or another object under the root is in the last chunk; physics = a physics shell the pipeline
// may not touch (moving/activating ragdoll, physic object, item on the ground, vehicle); cooldown = recently redone;
// lost = handed to a task and still recomputed (lock fail, dirty, invalidated); filtered = gone, reparented or the view
// entity's at its turn; phase_a = nothing to do at its turn (computed or started this tick); full = no room in the
// model list; unseen = not examined by the pipeline. early_corpses = corpses with a fully active shell (no activation,
// no interactive death motion) the pipeline took; early_guard_join = main joined the early tasks before it changed a
// physics shell or a visual inside the loop (KillHit, corpse removal, visual swap, item drop); early_guard_skip = it
// could not (this thread held a bone lock).
PERF_COUNTER(prepass_skip_tail_own, "prepass/skip/tail_own")
PERF_COUNTER(prepass_skip_tail_holder, "prepass/skip/tail_holder")
PERF_COUNTER(prepass_skip_physics, "prepass/skip/physics")
PERF_COUNTER(prepass_skip_cooldown, "prepass/skip/cooldown")
PERF_COUNTER(prepass_skip_lost, "prepass/skip/lost")
PERF_COUNTER(prepass_skip_filtered, "prepass/skip/filtered")
PERF_COUNTER(prepass_skip_phase_a, "prepass/skip/phase_a")
PERF_COUNTER(prepass_skip_full, "prepass/skip/full")
PERF_COUNTER(prepass_skip_unseen, "prepass/skip/unseen")
PERF_COUNTER(prepass_early_corpses, "prepass/early_corpses")
PERF_TIMER(prepass_early_guard_join, "prepass/early_guard_join")
PERF_COUNTER(prepass_early_guard_skip, "prepass/early_guard_skip")
// Phase C owners (pipeline on): owners of held items the render drew without computing the owner's bones (an NPC
// drawn only as a shadow blob) get them in the prepass - phase A on main, phase B on the workers - instead of
// serially in the item's UpdateXForm in phase C. owner_lazy = such owners handed to phase B; owner_bones_main = phase C
// items whose owner's bones were not computed this tick when the item came to its UpdateXForm (calls = items, time =
// their whole render_prepare; measured with the pipeline off too).
PERF_COUNTER(prepare_owner_lazy, "prepare/owner_lazy")
PERF_TIMER(prepare_owner_bones_main, "prepare/owner_bones_main")
// 26-framemove-game-next.md, the Game thread after package 2 (net_Update, scheduler overrun). net/client_send and
// net/server_update = the two halves of CLevel::net_Update (Level_network.cpp; inside game/seq_frame_mt/net);
// net/export_slice = one slice of the sliced single player object export (net_sp_export_slices > 1), export_objects =
// objects exported by the slices. agent/dist = CAgentEnemyManager::distribute_enemies of a group (inside
// ai/memory/enemies or stalker/shed/agent_manager - whoever calls it first in the frame), its stages fill / danger /
// assign / permutate / masks; agent/dist/eval = victory probabilities computed (ef_storage), eval_hit = answered by
// the per-call memo (npc_agent_distribute_fast 1), eval_pairs = distinct pairs of the memo; members / enemies = the
// group size and its enemies summed over the calls. stalker/cover/frame_defer = cover searches put off by
// npc_cover_scan_frame_budget_us (ai_stalker_cover.cpp).
PERF_TIMER(net_client_send, "net/client_send")
PERF_TIMER(net_server_update, "net/server_update")
PERF_TIMER(net_export_slice, "net/export_slice")
PERF_COUNTER(net_export_objects, "net/export_objects")
PERF_TIMER(agent_dist, "agent/dist")
PERF_TIMER(agent_dist_fill, "agent/dist/fill")
PERF_TIMER(agent_dist_danger, "agent/dist/danger")
PERF_TIMER(agent_dist_assign, "agent/dist/assign")
PERF_TIMER(agent_dist_permutate, "agent/dist/permutate")
PERF_TIMER(agent_dist_masks, "agent/dist/masks")
PERF_COUNTER(agent_dist_eval, "agent/dist/eval")
PERF_COUNTER(agent_dist_eval_hit, "agent/dist/eval_hit")
PERF_COUNTER(agent_dist_eval_pairs, "agent/dist/eval_pairs")
PERF_COUNTER(agent_dist_members, "agent/dist/members")
PERF_COUNTER(agent_dist_enemies, "agent/dist/enemies")
PERF_COUNTER(stk_cover_frame_defer, "stalker/cover/frame_defer")
// 26-framemove-game-next.md, the group's distribute_enemies and the cover scan (items 1-4 of the developer's list).
// agent/dist/throttled = distributions a member without an enemy asked for and npc_agent_distribute_min_ms put off,
// agent/dist/urgent = asked inside the interval and run at once (an enemy the last one knew died or went offline, or a
// member selected one it did not know); agent/dist/masks_batched = (enemy, member) pairs of assign_enemy_masks done by
// an OR into the shared record (npc_agent_masks_batch 1) instead of the full make_object_visible_somewhen;
// agent/group_overflow = stalkers registered in an overflow group because theirs had 64 (Entity.cpp, always on).
// ai/cover/suitable_snapshot = cover scans that read the group from a snapshot, ai/cover/accessible_{hit,miss} = the
// answers of CSpaceRestriction::accessible from its cache / computed (npc_cover_scan_cache 1/2).
PERF_COUNTER(agent_dist_throttled, "agent/dist/throttled")
PERF_COUNTER(agent_dist_urgent, "agent/dist/urgent")
PERF_COUNTER(agent_dist_masks_batched, "agent/dist/masks_batched")
PERF_COUNTER(agent_group_overflow, "agent/group_overflow")
PERF_COUNTER(ai_cover_suitable_snapshot, "ai/cover/suitable_snapshot")
PERF_COUNTER(ai_cover_accessible_hit, "ai/cover/accessible_hit")
PERF_COUNTER(ai_cover_accessible_miss, "ai/cover/accessible_miss")
// 26-framemove-game-next.md, FrameMove after the pipelined prepass. map/lod_skipped = map location updates skipped by
// map_update_lod (map_manager.cpp), ui/minimap/culled = minimap spots skipped by ui_minimap_cull (ui/UIMap.cpp),
// stalker/ucl_lod/combat/take_actor_far = unseen fighters of the actor the combat tier takes (or, with
// npc_ucl_lod_combat_actor_dist 0, would take at 20 m - they stay in deny_actor then), skip_actor_far = of the skip_combat
// frames those of them (ai/stalker/ai_stalker.cpp).
PERF_COUNTER(map_lod_skipped, "map/lod_skipped")
PERF_COUNTER(ui_minimap_culled, "ui/minimap/culled")
PERF_COUNTER(stk_ucl_lod_combat_actor_far, "stalker/ucl_lod/combat/take_actor_far")
PERF_COUNTER(stk_ucl_lod_skip_actor_far, "stalker/ucl_lod/skip_actor_far")
// 26-framemove-game-next.md, early kick of the pipelined bones prepass (r__bones_prepass_early_kick, xrGame/Level.cpp).
// prepass/early_kick = the overlapped window, from the end of the kick (end of CLevel::OnFrame) to its join at the
// prepass (sound update, UI, RT scheduler beside the kicked phase B); prepass/early_kick_wait = main's wait there;
// prepass/redone_after_kick = kicked models (and phase C owners) the prepass computed again (a mutator or a moved owner
// after the kick, or a lost task); prepass/skip/kick_excluded = models the kick left to the prepass: the root is in the
// RT scheduler (the actor) or script-controlled;
// game/early_start_ms = how much sooner the Game thread's phase 0 starts (the phase A the kick took off the prepass, less
// the wait it added there - an estimate).
PERF_TIMER(prepass_early_kick, "prepass/early_kick")
PERF_TIMER(prepass_early_kick_wait, "prepass/early_kick_wait")
PERF_COUNTER(prepass_redone_after_kick, "prepass/redone_after_kick")
PERF_COUNTER(prepass_skip_kick_excluded, "prepass/skip/kick_excluded")
PERF_TIMER(game_early_start_ms, "game/early_start_ms")
// 28-updatecl-main.md (ol_UpdateCL on main in a heavy fight). ik/* = the parts of CIKLimbsController's feet IK (in
// ik/update on main; with npc_ik_in_bones 1 gather / rays / process run in phase B of the bones, on whatever thread
// computes them): tracks = UpdateTracks, gather = GatherRays of the limbs, rays = the rays (rays_n = how many), process =
// ProcessGroup; rays_batch = the RayPickBatch of npc_perf_ik_foot_raypick_batch_min_rays or more rays on main (wall
// time, calls = batches); task/wait/site/ik, task/wait_game_on_main/ik = main's waits of that batch (wait_site::Ik),
// task/wait/ik_exec = what main ran inside them (its rays, phase B of other models - stolen pipeline tasks).
// prepass/early_phase_b/{main,game,worker} = prepass/early_phase_b by the thread that ran the early task (main: inside
// a Wait - the IK batch, a join); tracks/ll_time/* = LL_UpdateTracks of CKinematicsAnimated::UpdateTracks by thread
// (blend callbacks included); prepass/early_main/{take,kick} = inside prepass/early_main: the phase A of the taken
// models (EarlyTake, the phase C owners of the kick), the kick of their phase B (task creation).
PERF_TIMER(ik_tracks, "ik/tracks")
PERF_TIMER(ik_gather, "ik/gather")
PERF_TIMER(ik_rays, "ik/rays")
PERF_COUNTER(ik_rays_n, "ik/rays_n")
PERF_TIMER(ik_rays_batch, "ik/rays_batch")
PERF_TIMER(ik_process, "ik/process")
PERF_COUNTER(task_wait_site_ik, "task/wait/site/ik")
PERF_COUNTER(task_wait_game_on_main_ik, "task/wait_game_on_main/ik")
PERF_TIMER(task_wait_ik_exec, "task/wait/ik_exec")
PERF_TIMER(prepass_early_phase_b_main, "prepass/early_phase_b/main")
PERF_TIMER(prepass_early_phase_b_game, "prepass/early_phase_b/game")
PERF_TIMER(prepass_early_phase_b_worker, "prepass/early_phase_b/worker")
PERF_TIMER(tracks_ll_time_main, "tracks/ll_time/main")
PERF_TIMER(tracks_ll_time_game, "tracks/ll_time/game")
PERF_TIMER(tracks_ll_time_worker, "tracks/ll_time/worker")
PERF_TIMER(prepass_early_main_take, "prepass/early_main/take")
PERF_TIMER(prepass_early_main_kick, "prepass/early_main/kick")
// The unmarked rest of cm/ucl/position_anim (CMovementManager::on_frame): update_path = update_path of the frame,
// path_position = the point on the path, ground = ground_snap + ground_diag (calls = up to 2 per update), idle_calc = the
// frame step (Calculate) of a standing NPC with its character on, kinematic = the branch without neighbours (SetPosition,
// DisableCharacter), obstacles = the dynamic / static obstacle queries of stalkers and the path rebuild they ask for
// (calls = up to 2 per update).
PERF_TIMER(mv_update_path, "movement/update_path")
PERF_TIMER(mv_path_position, "movement/path_position")
PERF_TIMER(mv_ground, "movement/ground")
PERF_TIMER(mv_idle_calc, "movement/idle_calc")
PERF_TIMER(mv_kinematic, "movement/kinematic")
PERF_TIMER(mv_obstacles, "movement/obstacles")
// weapon/shot/* = a shot of CWeaponMagazined (state_Fire, inside weapon/ucl/state): sound = the shot sound (OnShot),
// fx = the shell, the flame and the smoke (OnShot), trace = FireTrace (the bullets). weapon/ucl/dead_owner = UpdateCL
// calls skipped by W1 (wpn_npc_dead_owner_skip; with it off, while perf_metrics runs, the ones it would skip).
// weapon/xform_exact/* = bones/caller/weapon/sounds_exact by the reason (the first that applies): other (the actor's,
// the view entity's, HUD mode, no owner model), frozen (a frozen corpse pose), dead, svp (a second viewport), first (the
// first XFORM in these hands).
PERF_TIMER(wpn_shot_sound, "weapon/shot/sound")
PERF_TIMER(wpn_shot_fx, "weapon/shot/fx")
PERF_TIMER(wpn_shot_trace, "weapon/shot/trace")
PERF_COUNTER(wpn_ucl_dead_owner, "weapon/ucl/dead_owner")
PERF_COUNTER(wpn_xform_exact_dead, "weapon/xform_exact/dead")
PERF_COUNTER(wpn_xform_exact_frozen, "weapon/xform_exact/frozen")
PERF_COUNTER(wpn_xform_exact_svp, "weapon/xform_exact/svp")
PERF_COUNTER(wpn_xform_exact_first, "weapon/xform_exact/first")
PERF_COUNTER(wpn_xform_exact_other, "weapon/xform_exact/other")
// stalker/ucl_lod/combat/deny_seen_* = of deny_seen, the fighters on screen refused by the distance alone (the other
// conditions of K8a hold), by the distance: < 30 m, 30-40 m, 40-50 m - what npc_ucl_lod_combat_seen_dist 40 / 30 adds.
PERF_COUNTER(stk_ucl_lod_combat_seen_lt30, "stalker/ucl_lod/combat/deny_seen_lt30")
PERF_COUNTER(stk_ucl_lod_combat_seen_30_40, "stalker/ucl_lod/combat/deny_seen_30_40")
PERF_COUNTER(stk_ucl_lod_combat_seen_40_50, "stalker/ucl_lod/combat/deny_seen_40_50")
// U1 (npc_ik_in_bones): ik/deferred_run/{main,game,worker} = the feet IK run in phase B of the bones (GatherRays, the
// rays one by one, ProcessGroup) by the thread (main: game code asked for the bones first, e.g. the fire point);
// ik/deferred_late = of them, requests of an earlier frame (no bones computed in their frame).
PERF_TIMER(ik_deferred_run_main, "ik/deferred_run/main")
PERF_TIMER(ik_deferred_run_game, "ik/deferred_run/game")
PERF_TIMER(ik_deferred_run_worker, "ik/deferred_run/worker")
PERF_COUNTER(ik_deferred_late, "ik/deferred_late")
// 27-render-main.md (main thread in CRender::Render on R4). M: render/part0/hud, lods, details = those steps of part0
// (render_hud, render_lods, Details->Render; main view, wherever they run), render/part0/static_early = R2
// (r__part0_static_early 1): scene prepare + static geometry + LODs + grass before r_main.sync() (it then takes the
// lods/details timers too). render/main/par_submit_main_unit = the main thread recording its own (inline) units of the
// parallel G-buffer submit (part of par_submit_record_cpu, calls = units); render/main/par_merge_matrix, par_merge_sorted
// = the two halves of render/main/par_merge (calls = shards). render/combine/sorted_other = render_item of the
// non-particle items of mapSorted in the combine (calls = items), ps_adjacent_same = neighbours in the sorted order one
// draw could take (sprite effects, same ShaderElement and cull mode), ps_hud = HUD-mode effects there.
// render/main/ps_small_{4,8,16}px = non-empty particle effects of the main view under that many pixels on screen
// (bounding sphere diameter, cumulative). render/main/skel_dist/<m> = skeletal models of the main view (not the HUD, not
// the LOD mesh as a model of its own) closer than <m> metres (and further than the previous bucket; far = 100 m and
// more), skel_parts/<m> = their meshes: children with the full mesh, 1 with the LOD mesh (~ draws per pass).
// R1 (r__ps_sorted_batch 1/2, render_sorted of the combine): ps_prefill = snapshot + one Lock + fill + Unlock (wall, main),
// ps_prefill_cpu = the fill of one chunk (sum over the main thread and workers, calls = chunks), ps_prepared = effects
// drawn from the batch, ps_runs = their draw calls (= ps_prepared with 1, fewer with 2), ps_run_max = the longest run
// of a frame (summed per frame: divide by frames), ps_fallback = effects that went back to Render() (the batch over half
// of the stream, or the stream wrapped after the fill), ps_sim_late = frames whose PreRender task (the particle step)
// had not finished when the batch read the particles.
PERF_TIMER(rpart0_hud, "render/part0/hud")
PERF_TIMER(rpart0_lods, "render/part0/lods")
PERF_TIMER(rpart0_details, "render/part0/details")
PERF_TIMER(rpart0_static_early, "render/part0/static_early")
PERF_TIMER(rmain_par_submit_main_unit, "render/main/par_submit_main_unit")
PERF_TIMER(rmain_par_merge_matrix, "render/main/par_merge_matrix")
PERF_TIMER(rmain_par_merge_sorted, "render/main/par_merge_sorted")
PERF_TIMER(rcomb_sorted_other, "render/combine/sorted_other")
PERF_COUNTER(rcomb_ps_adjacent_same, "render/combine/ps_adjacent_same")
PERF_COUNTER(rcomb_ps_hud, "render/combine/ps_hud")
PERF_COUNTER(rmain_ps_small_4, "render/main/ps_small_4px")
PERF_COUNTER(rmain_ps_small_8, "render/main/ps_small_8px")
PERF_COUNTER(rmain_ps_small_16, "render/main/ps_small_16px")
PERF_COUNTER(rmain_skel_dist_10, "render/main/skel_dist/10")
PERF_COUNTER(rmain_skel_dist_25, "render/main/skel_dist/25")
PERF_COUNTER(rmain_skel_dist_50, "render/main/skel_dist/50")
PERF_COUNTER(rmain_skel_dist_100, "render/main/skel_dist/100")
PERF_COUNTER(rmain_skel_dist_far, "render/main/skel_dist/far")
PERF_COUNTER(rmain_skel_parts_10, "render/main/skel_parts/10")
PERF_COUNTER(rmain_skel_parts_25, "render/main/skel_parts/25")
PERF_COUNTER(rmain_skel_parts_50, "render/main/skel_parts/50")
PERF_COUNTER(rmain_skel_parts_100, "render/main/skel_parts/100")
PERF_COUNTER(rmain_skel_parts_far, "render/main/skel_parts/far")
PERF_TIMER(rcomb_ps_prefill, "render/combine/ps_prefill")
PERF_TIMER(rcomb_ps_prefill_cpu, "render/combine/ps_prefill_cpu")
PERF_COUNTER(rcomb_ps_prepared, "render/combine/ps_prepared")
PERF_COUNTER(rcomb_ps_runs, "render/combine/ps_runs")
PERF_COUNTER(rcomb_ps_run_max, "render/combine/ps_run_max")
PERF_COUNTER(rcomb_ps_fallback, "render/combine/ps_fallback")
PERF_COUNTER(rcomb_ps_sim_late, "render/combine/ps_sim_late")
// r__svp_lod_fix: skeletal models of the scope view (parallel SVP build on the SVP worker only) drawn with the character
// LOD mesh (m_lod) - compare with r__svp_lod_fix 0 (the main view's thresholds) at zoom.
PERF_COUNTER(rsvp_skel_lod, "render/svp/skel_lod")
// 27-render-main.md R10 (r__main_build_par_merge 1): the merge of the parallel main build by jobs, one per destination
// list - render/main/par_merge_matrix and par_merge_sorted then sum the jobs' CPU (calls = jobs: 4 matrix, 7 sorted maps
// a frame), render/main/par_merge stays the wall on the build's thread; par_merge_par = frames merged by jobs.
// par_jobs_tail = the build's thread waiting for the jobs other workers took (R10 merge, R3 sort).
// R3 (r__main_par_prepare_in_build 1): par_prep_smooth = the CROS refresh (update_smooth) of the dynamic objects in the
// parts of the parallel build (CPU, calls = parts), par_prep_sort = sorting the priority-0 dynamic groups and their
// items at the end of the build (wall), par_prep_used = passes part0 took sorted, par_prep_stale = presorts dropped
// because the map changed after the build (expected 0). render/main/par_submit_smooth_late = objects part0's prepare
// (render_graph_main_parallel) still refreshed itself - with R3 only the tracked object and the late (HUD) inserts.
PERF_COUNTER(rmain_par_merge_par, "render/main/par_merge_par")
PERF_TIMER(rmain_par_jobs_tail, "render/main/par_jobs_tail")
PERF_TIMER(rmain_par_prep_smooth, "render/main/par_prep_smooth")
PERF_TIMER(rmain_par_prep_sort, "render/main/par_prep_sort")
PERF_COUNTER(rmain_par_prep_used, "render/main/par_prep_used")
PERF_COUNTER(rmain_par_prep_stale, "render/main/par_prep_stale")
PERF_COUNTER(rmain_par_submit_smooth_late, "render/main/par_submit_smooth_late")
// 29-game-thread-next.md (the Game thread on the critical path), temporary switches, not saved, 0 = as before.
// A1 (mt_phase0_split): game/early_dispatch_wait = the Game thread waiting at the end of phase 0 for the dispatch task
// after its own work (the GC step); game/sound_dispatch then is the task itself, game/early = the whole phase 0;
// game/early_dispatch_on_main = dispatch tasks main took in one of its Wait()s (main's path got longer by them).
// game/early_dispatch_self = dispatch tasks no worker took before the Game thread's work ended (it ran them itself in
// the Wait: no overlap that frame).
// A3 (mt_sound_render_early): game/sound_render_early = frames whose K9 sound render task started in phase 0, right
// after the dispatch (game/sound_render_wait stays the body's wait for it before seqFrameMT).
// A2 (mt_bullets_task): game/bullets_task = UpdateWorkload of the bullets in m_Bullets as a task (wherever it ran),
// game/bullets_wait = the Game thread waiting for it at its old place in seqParallel (game/pseq/bullets = that wait plus
// the bullets fired during the task, moved there), game/bullets_pending = bullets AddBullet put aside meanwhile,
// game/bullets_on_main = tasks main took in a Wait() of its render, game/bullets_self = tasks no worker took until the
// Game thread reached their join (it ran them itself there: no overlap that frame).
// A4: task/wait_game/total = the outermost TaskManager::Wait() of the dedicated Game thread (phase 0 and the body);
// exec_own = tasks of its own queue or pieces of the awaited task it ran inside, exec_foreign = other threads' tasks it
// took (render, sound decoding...) - with mt_game_wait_own_only 0 the cost of stealing; the rest of total is spinning.
// task/wait_game/fallback = with mt_game_wait_own_only 1, steals after 1 ms without own work (deadlock guard).
// B5 (metric only): level_path/search_max_us = the longest single path search of the frame (CLevelPathBuilder::process:
// the level path and its detail path, seqParallel) in us, level_path/searches_max = searches in the frame; both gauges
// of per-frame maxima (the window prints their average and peak).
PERF_TIMER(game_early_dispatch_wait, "game/early_dispatch_wait")
PERF_COUNTER(game_early_dispatch_on_main, "game/early_dispatch_on_main")
PERF_COUNTER(game_early_dispatch_self, "game/early_dispatch_self")
PERF_COUNTER(game_sound_render_early, "game/sound_render_early")
PERF_TIMER(game_bullets_task, "game/bullets_task")
PERF_TIMER(game_bullets_wait, "game/bullets_wait")
PERF_COUNTER(game_bullets_pending, "game/bullets_pending")
PERF_COUNTER(game_bullets_on_main, "game/bullets_on_main")
PERF_COUNTER(game_bullets_self, "game/bullets_self")
PERF_TIMER(task_wait_game_total, "task/wait_game/total")
PERF_TIMER(task_wait_game_exec_own, "task/wait_game/exec_own")
PERF_TIMER(task_wait_game_exec_foreign, "task/wait_game/exec_foreign")
PERF_COUNTER(task_wait_game_fallback, "task/wait_game/fallback")
PERF_GAUGE(level_path_search_max_us, "level_path/search_max_us")
PERF_GAUGE(level_path_searches_max, "level_path/searches_max")
// A1 fix (mt_phase0_split): the dispatch task runs beside the Lua GC step, so the Lua it would call - the hit callback
// and plugin event of the "fake" hit of an enemy shot an NPC hears (CHitMemoryManager::add) - is queued.
// game/early_dispatch_deferred = the hit callbacks queued (a frame), game/early_dispatch_flush = the Game thread running
// that queue right after the task (calls = frames with a queue), game/early_dispatch_dropped = queued ones whose NPC or
// shooter was gone (or dead) by then - skipped, expected ~0.
PERF_COUNTER(game_early_dispatch_deferred, "game/early_dispatch_deferred")
PERF_TIMER(game_early_dispatch_flush, "game/early_dispatch_flush")
PERF_COUNTER(game_early_dispatch_dropped, "game/early_dispatch_dropped")
// 29-game-thread-next.md, the adaptive budget of the scheduler (shed_budget_adaptive, xrEngine/xrSheduler.cpp, see the
// comment at ps_shed_budget_adaptive). shed/budget_effective_us = the effective budget of the non-RT pass in us (=
// shed/budget * 1000 with shed_budget_adaptive 0); shed/budget_cut_frames = passes with the budget below the base;
// frame/game_wait_ema_us = the EMA (~4 frames) of the wait of the main thread for the Game thread at its join, the
// signal of the controller (measured in every overlap frame, also with the switch off). The freshness of the AI under a
// cut: shed/urgent_late_ms / _n, shed/near_calm_late_ms / _n, shed/far_late_ms / _n (and shed/*_stalker_late_*).
PERF_GAUGE(shed_budget_effective_us, "shed/budget_effective_us")
PERF_COUNTER(shed_budget_cut_frames, "shed/budget_cut_frames")
PERF_GAUGE(frame_game_wait_ema_us, "frame/game_wait_ema_us")
// 32-frame-spikes.md (lua_spike_probe.cpp, xr_object_list.cpp, ai_stalker.cpp, CustomMonster.cpp, entity_alive.cpp,
// Feel_Vision.cpp). G1 (lua_gc_budget_max_us > 0): lua/gc_budget_us = the time cap of the paced GC steps of the frame
// in us (0 = uncapped that frame; with lua_gc_budget_max_us 0 it is lua_gc_step_budget_us), lua/gc_heap_growth_pct =
// the heap in % of its size at the start of the GC cycle (G1 only), lua/gc_uncapped_heap = frames of lua/gc_uncapped
// lifted by that growth (lua_gc_heap_guard_pct) rather than by the 64K debt. R1 (ol_relcase_npc_batch 1):
// objects/relcase/npc_batch = net_RelcaseBatch of CAI_Stalker / CCustomMonster done in one pass over the personal lists
// (calls = NPCs; part of objects/relcase/active|sleeping). R2 (ol_relcase_vision_sorted 1):
// objects/relcase/vision_sorted = Feel::Vision subscribers whose query/seen were searched by binary search.
PERF_GAUGE(lua_gc_budget_us, "lua/gc_budget_us")
PERF_GAUGE(lua_gc_heap_growth_pct, "lua/gc_heap_growth_pct")
PERF_COUNTER(lua_gc_uncapped_heap, "lua/gc_uncapped_heap")
PERF_TIMER(relcase_npc_batch, "objects/relcase/npc_batch")
PERF_COUNTER(relcase_vision_sorted, "objects/relcase/vision_sorted")
// 31-svp-scope.md (3D scope, SVP). M - the scope pass on main, by phase (CRender::Render with m_SecondViewportPass,
// next to render/pass/*, which are main-pass only): sync_point = q_sync_point.Wait; part0 = the geometry part (the
// sequential path records render_graph(0) here, the parallel one only lods/details) incl. submit0 = SubmitSVPDeferred of
// the big list the SVP worker recorded (Finish/ExecuteCommandList); hud_ui, sun, selfillum; lights = both render_lights
// calls (calls = 2 a frame); combine = phase_combine.
PERF_TIMER(rsvp_pass_sync_point, "render/svp/pass/sync_point")
PERF_TIMER(rsvp_pass_part0, "render/svp/pass/part0")
PERF_TIMER(rsvp_pass_submit0, "render/svp/pass/submit0")
PERF_TIMER(rsvp_pass_hud_ui, "render/svp/pass/hud_ui")
PERF_TIMER(rsvp_pass_sun, "render/svp/pass/sun")
PERF_TIMER(rsvp_pass_selfillum, "render/svp/pass/selfillum")
PERF_TIMER(rsvp_pass_lights, "render/svp/pass/lights")
PERF_TIMER(rsvp_pass_combine, "render/svp/pass/combine")
// The scope view's geometry recording (render_graph(0) of the scope dsgraph: the SVP worker, or main on the sequential
// path): static / dynamic halves, their backend draw calls, GPU-skinned (palette written) and single-bone skin draws.
PERF_TIMER(rsvp_geom_static, "render/svp/geom_static")
PERF_TIMER(rsvp_geom_dynamic, "render/svp/geom_dynamic")
PERF_COUNTER(rsvp_geom_static_draws, "render/svp/geom_static_draws")
PERF_COUNTER(rsvp_geom_dyn_draws, "render/svp/geom_dyn_draws")
PERF_COUNTER(rsvp_geom_skin_hw, "render/svp/geom_skin_hw")
PERF_COUNTER(rsvp_geom_skin_single, "render/svp/geom_skin_single")
// The scope view's visibility build (build_subspace of the scope dsgraph): traverse = sector/portal traversal, static =
// add_static over the traversed sectors, query = q_frustum, dynamic = the whole dynamic part (query included);
// objects = renderables it submitted (renderable_Render).
PERF_TIMER(rsvp_build_traverse, "render/svp/build_traverse")
PERF_TIMER(rsvp_build_static, "render/svp/build_static")
PERF_TIMER(rsvp_build_query, "render/svp/build_query")
PERF_TIMER(rsvp_build_dynamic, "render/svp/build_dynamic")
PERF_COUNTER(rsvp_objects, "render/svp/objects")
// Lights of the scope pass: lights_in = light entries the scope pass would accumulate (an OMNIPART = 1 of 6; the
// frame driver: unshadowed packages + the transferred shadow pages), lights_kept = after r__svp_light_filter2 (= in
// with the switch at 0).
PERF_COUNTER(rsvp_lights_in, "render/svp/lights_in")
PERF_COUNTER(rsvp_lights_kept, "render/svp/lights_kept")
// The main thread waiting for the Game thread (frame/game_wait) in frames that rendered the scope pass.
PERF_TIMER(frame_game_wait_svp, "frame/game_wait_svp")
// L0 (r__svp_lens_cull 2): the scope build's frustum cropped to the square around the lens circle. lens_frames =
// scope frames built with the crop, lens_area_pct = sum of the cropped square's share of the scope image in % (average
// = lens_area_pct / lens_frames), lens_fallback = scope frames with the switch on but the crop not applied (not aimed
// through the scope / not fully raised / alternative sight).
PERF_COUNTER(rsvp_lens_frames, "render/svp/lens_frames")
PERF_COUNTER(rsvp_lens_area_pct, "render/svp/lens_area_pct")
PERF_COUNTER(rsvp_lens_fallback, "render/svp/lens_fallback")
// H (r__svp_hom): dynamic renderables of the scope build culled by the main view's HOM (a copy of vis_data, no writes).
PERF_COUNTER(rsvp_hom_cull, "render/svp/hom_cull")
// 32-frame-spikes.md, A1 (alife_update_spread, xrGame alife_update_manager.cpp): the ALife update spread over the frames.
// alife/spread_ticks = turns of the scheduler (shedule_Update; each gives objects_per_update tokens and one switch pass);
// alife/spread_frames = frames that did a part of it (update_spread; alife/update then counts these parts: calls =
// frames, max = the longest part); alife/update_switch, alife/update_scheduled = its two halves (the switch pass, the
// scheduled objects); alife/spread_tokens = scheduled objects still due after the part of the frame (the bucket);
// alife/spread_dropped = tokens over the cap of the bucket (4 turns) - the budget does not keep up;
// alife/spread_switch_unfinished = turns that came before the switch pass of the previous one was done. The slowest
// object of a part: "[perf-spike] alife/object" (perf_spike_us) with its name, section and ID.
PERF_COUNTER(alife_spread_ticks, "alife/spread_ticks")
PERF_COUNTER(alife_spread_frames, "alife/spread_frames")
PERF_TIMER(alife_update_switch, "alife/update_switch")
PERF_TIMER(alife_update_scheduled, "alife/update_scheduled")
PERF_GAUGE(alife_spread_tokens, "alife/spread_tokens")
PERF_COUNTER(alife_spread_dropped, "alife/spread_dropped")
PERF_COUNTER(alife_spread_switch_unfinished, "alife/spread_switch_unfinished")
// 30-game-events.md (plans/optimization_mt): the peaks of ProcessGameEvents on main (deaths, destructions).
// ge/max_event_us = the longest single message of the frame's game event queue in us (gauge of per-frame maxima);
// ge/max_event_type = what it was: the GE_* type of an M_EVENT, 1000 + the message ID otherwise (Tracy plot per frame;
// in the log the "* ge: longest event" line of perf_metrics bit 2 names the longest event of the 5 s window).
// ge/n_frame/* = events of the frame by kind (die = GE_DIE only, destroy = GE_DESTROY + GE_DESTROY_REJECT, reject =
// GE_OWNERSHIP_REJECT + GE_TRADE_SELL + GE_DESTROY_REJECT, hit = GE_HIT). ge/time/explode = GE_GRENADE_EXPLODE (before
// 2026-10-17 it was in ge/time/weapon, which no longer has it).
// Death (CAI_Stalker::Die -> CEntityAlive::Die): death/stalker_pre = CAI_Stalker::Die before inherited::Die (movement,
// notify_on_wounded_or_killed, the death animation, the death sound), death/lua_binder = the Lua eDeath callback (binder),
// death/bus = the death event of the bus (npc_on_death_callback: Lua and plugin subscribers), death/in_die =
// CCharacterPhysicsSupport::in_Die (shell or kill hit), death/create_shell = CreateShell, death/correct =
// CollisionCorrectObjPos of EndActivateFreeShell, death/fly_to = FlyTo (Freeze + 10 steps of the world + UnFreeze),
// physics/fly_to/freeze = its Freeze and UnFreeze of the world (calls = 2 per FlyTo).
// Reject (CAI_Stalker::on_ownership_reject): reject/bones = in_UpdateCL + CalculateBones(true) of the owner,
// reject/bones_skipped = rejects with just_before_destroy that skipped it (npc_reject_destroy_skip_bones 1),
// reject/drop = inventory().DropItem; item_drop/lua_binder = the Lua eOnItemDrop callback, item_drop/bus = the item
// drop event of the bus (npc_on_item_drop) - of any owner, not only in a reject.
// objects/destroy_child_scan = the walk of all objects for live children of the destroyed ones (calls = walks: one per
// register_object_to_destroy with ol_destroy_child_scan_batch 0, one per batch with 1), _objects = objects walked.
// Hit: hit/kill_entity = CEntity::KillEntity in CEntity::Hit (GE_DIE to the server, its Lua on_death), hit/bus_before =
// npc_on_before_hit of the bus (CAI_Stalker::Hit), hit/lua_binder = the Lua eHit callback of CHitMemoryManager::add,
// hit/bus = its hit event of the bus, hit/wallmarks = BloodyWallmarks of CEntityAlive::Hit.
// ge/cold/sound_load, ge/cold/model_load = cold loads (sound/source/load, modelpool/instance_load) on the thread that
// runs ProcessGameEvents while inside it (the spawns of the queue included: modelpool/instance_load_spawn is that part).
PERF_GAUGE(ge_max_event_us, "ge/max_event_us")
PERF_GAUGE(ge_max_event_type, "ge/max_event_type")
PERF_GAUGE(ge_n_frame_die, "ge/n_frame/die")
PERF_GAUGE(ge_n_frame_destroy, "ge/n_frame/destroy")
PERF_GAUGE(ge_n_frame_reject, "ge/n_frame/reject")
PERF_GAUGE(ge_n_frame_hit, "ge/n_frame/hit")
PERF_TIMER(ge_time_explode, "ge/time/explode")
PERF_TIMER(death_stalker_pre, "death/stalker_pre")
PERF_TIMER(death_lua_binder, "death/lua_binder")
PERF_TIMER(death_bus, "death/bus")
PERF_TIMER(death_in_die, "death/in_die")
PERF_TIMER(death_create_shell, "death/create_shell")
PERF_TIMER(death_correct, "death/correct")
PERF_TIMER(death_fly_to, "death/fly_to")
PERF_TIMER(ph_fly_to_freeze, "physics/fly_to/freeze")
PERF_TIMER(reject_bones, "reject/bones")
PERF_COUNTER(reject_bones_skipped, "reject/bones_skipped")
PERF_TIMER(reject_drop, "reject/drop")
PERF_TIMER(item_drop_lua_binder, "item_drop/lua_binder")
PERF_TIMER(item_drop_bus, "item_drop/bus")
PERF_TIMER(ol_destroy_child_scan, "objects/destroy_child_scan")
PERF_COUNTER(ol_destroy_child_scan_objects, "objects/destroy_child_scan_objects")
PERF_TIMER(hit_kill_entity, "hit/kill_entity")
PERF_TIMER(hit_bus_before, "hit/bus_before")
PERF_TIMER(hit_lua_binder, "hit/lua_binder")
PERF_TIMER(hit_bus, "hit/bus")
PERF_TIMER(hit_wallmarks, "hit/wallmarks")
PERF_TIMER(ge_cold_sound_load, "ge/cold/sound_load")
PERF_TIMER(ge_cold_model_load, "ge/cold/model_load")
// 27-render-main.md, R13 (r__dsgraph_reserve, Layers/xrRender): the node arrays of the dsgraph maps (xr_fixed_map - its
// only users). render/dsgraph/resize_us/{main,game,worker} = xr_fixed_map::resize() (a map outgrows its array: a new one
// of twice the size, the nodes carried over) by the thread that grows the map, calls = resizes; resize_count = all of
// them; resize_nodes = nodes carried over (copy relocation, r__dsgraph_reserve without bit 2: every item vector of a
// matrix/normal node deep-copied and its old buffer leaked); resize_kb = KB of the new arrays. capacity_nodes = nodes
// allocated in all pooled dsgraphs + the immediate one after the end-of-frame cleanup (0 without bit 1: freed every
// frame, grown again by the next one); capacity_max = the largest single map of them (nodes); shrink = maps cut down to
// the high-water mark of the last window by the slow decay (bit 1).
PERF_TIMER(rdsg_resize_main, "render/dsgraph/resize_us/main")
PERF_TIMER(rdsg_resize_game, "render/dsgraph/resize_us/game")
PERF_TIMER(rdsg_resize_worker, "render/dsgraph/resize_us/worker")
PERF_COUNTER(rdsg_resize_count, "render/dsgraph/resize_count")
PERF_COUNTER(rdsg_resize_nodes, "render/dsgraph/resize_nodes")
PERF_COUNTER(rdsg_resize_kb, "render/dsgraph/resize_kb")
PERF_GAUGE(rdsg_capacity_nodes, "render/dsgraph/capacity_nodes")
PERF_GAUGE(rdsg_capacity_max, "render/dsgraph/capacity_max")
PERF_COUNTER(rdsg_shrink, "render/dsgraph/shrink")
// 32-frame-spikes.md (xrPhysics/PHCommander.cpp UpdateProfiled): the commander of level.add_call and set_fastcall
// (frame/level/add_call, main). ph_commander/calls = calls checked (each condition every frame until it is true),
// ph_commander/lua_cond = the time of their Lua conditions (calls = conditions called), ph_commander/lua_action = the
// time of the Lua actions that ran (once per call), ph_commander/max_us = the longest single call of the frame in us
// (per-frame maximum). The call itself: Tracy zone and "[perf-spike] level/add_call/call" (perf_spike_us) with cond=/act=
// (the Lua source of each part; "(up ...)" = the callback inside a wrapper such as AddUniqueCall of _g.script).
PERF_COUNTER(ph_commander_calls, "ph_commander/calls")
PERF_TIMER(ph_commander_lua_cond, "ph_commander/lua_cond")
PERF_TIMER(ph_commander_lua_action, "ph_commander/lua_action")
PERF_GAUGE(ph_commander_max_us, "ph_commander/max_us")
// 33-level-path.md (plans/optimization_mt), M: the level paths (CLevelGraph::Search, CLevelPathBuilder).
// level_path/astar_us = every level A* of every thread (calls = A* calls; monsters search synchronously, stalkers in
// seqParallel), astar_calls = the same count, astar_fail = A* that found no path, astar_cap_hit = A* that refused a
// vertex because of max_visited_node_count (65500, or ai_obstacle_path_visited_cap), astar_visited_max / astar_max_us =
// per-frame maxima of one A* (vertices with a cost / us), fast_storage = A* on the fast data (ai_level_search_fast 1).
// detail_us = the detail paths (CDetailPathBuilder::process_impl: inside the level path delegate and on their own),
// simulate_us = simulate_path_navigation of the stalkers' obstacle loop, obstacle_iters_max = the passes of that loop
// in one build (per-frame max), pure_fallback = builds where the search with obstacles failed and the one without
// them was tried, iters_capped = builds stopped by ai_obstacle_path_iters_max, deferred = level path delegates moved
// to the next frame by ai_level_path_budget_us, spikes = [path-spike] lines (ai_level_path_spike_us), spikes_dropped =
// spikes over the line limit (4 a second).
PERF_TIMER(level_path_astar_us, "level_path/astar_us")
PERF_COUNTER(level_path_astar_calls, "level_path/astar_calls")
PERF_COUNTER(level_path_astar_fail, "level_path/astar_fail")
PERF_COUNTER(level_path_astar_cap_hit, "level_path/astar_cap_hit")
PERF_GAUGE(level_path_astar_visited_max, "level_path/astar_visited_max")
PERF_GAUGE(level_path_astar_max_us, "level_path/astar_max_us")
PERF_COUNTER(level_path_fast_storage, "level_path/fast_storage")
PERF_TIMER(level_path_detail_us, "level_path/detail_us")
PERF_TIMER(level_path_simulate_us, "level_path/simulate_us")
PERF_GAUGE(level_path_obstacle_iters_max, "level_path/obstacle_iters_max")
PERF_COUNTER(level_path_pure_fallback, "level_path/pure_fallback")
PERF_COUNTER(level_path_iters_capped, "level_path/iters_capped")
PERF_COUNTER(level_path_deferred, "level_path/deferred")
PERF_COUNTER(level_path_spikes, "level_path/spikes")
PERF_COUNTER(level_path_spikes_dropped, "level_path/spikes_dropped")
// 32-frame-spikes.md, the offline wave (2026-10-17 12:22: 120-138 destroyed objects a frame, 70-77 ms frames).
// alife/switch_offline_frame = client objects (each with all its children) taken offline by ALife (remove_online, any
// path); max_frame = the most in a frame of the Game thread. alife/switch_offline_deferred = spread switch passes stopped
// by alife_switch_offline_per_frame_max (the rest went on the next frame).
// objects/relcase/cb_hud = the HUD part of objects/relcase/callbacks (the rest: cb_vision, cb_touch, the loop);
// vision_scan = Feel::Vision batch callbacks with ol_relcase_cb_scan 1, touch_scan_skips = Feel::Touch subscribers
// whose lists held no batch object (the per-object walk skipped); active_skipped / sleeping_skipped = objects whose
// net_RelcaseBatch returned at once (ol_relcase_skip_noop 1: the empty relcase of CGameObject).
PERF_COUNTER(alife_switch_offline_frame, "alife/switch_offline_frame")
PERF_COUNTER(alife_switch_offline_deferred, "alife/switch_offline_deferred")
PERF_TIMER(relcase_cb_hud, "objects/relcase/cb_hud")
PERF_COUNTER(relcase_vision_scan, "objects/relcase/vision_scan")
PERF_COUNTER(relcase_touch_scan_skips, "objects/relcase/touch_scan_skips")
PERF_COUNTER(relcase_active_skipped, "objects/relcase/active_skipped")
PERF_COUNTER(relcase_sleeping_skipped, "objects/relcase/sleeping_skipped")
// 32-frame-spikes.md, ol_relcase_parallel 1 (the fixed cost of a frame with a destroy). objects/relcase/parallel_wall =
// the phase right after the active pass that runs the stalkers' own parts (CAI_Stalker::net_RelcaseBatchOwn) on the
// task workers (wall time on the relcase thread; a part of objects/relcase/active), parallel_tasks = the parts run there
// (stalkers; npc_batch is then the sum of their time over the threads, without the shared part); serial_tail = the
// rest of the active pass in that mode (other classes, the stalkers' agent manager and physics). cb_parallel_wall =
// the Feel::Vision batch callbacks on the workers (wall; a part of objects/relcase/callbacks, cb_vision = their sum),
// cb_parallel_tasks = those callbacks.
PERF_TIMER(relcase_parallel_wall, "objects/relcase/parallel_wall")
PERF_COUNTER(relcase_parallel_tasks, "objects/relcase/parallel_tasks")
PERF_TIMER(relcase_serial_tail, "objects/relcase/serial_tail")
PERF_TIMER(relcase_cb_parallel_wall, "objects/relcase/cb_parallel_wall")
PERF_COUNTER(relcase_cb_parallel_tasks, "objects/relcase/cb_parallel_tasks")
// 32-frame-spikes.md, G2 (lua_gc_atomic_stats 1; LuaJIT with the GlobalWar patch, luajit.h LUAJIT_GW_GCSTATS): the
// last atomic phase of the Lua GC part by part, in us - remark = the weak tables, the running thread and the roots
// again; grayagain = the tables written behind the barrier since their traversal + all threads, with all they reach;
// separateudata = the walk over all userdata; mmudata = marking the dead userdata with __gc; other = open upvalues,
// the left-overs of the gray list, the sweep setup. The counts: grayagain_tables/entries (array + hash slots)/threads
// = the 2nd chance list, udata = all userdata walked, udata_with_gc = moved to finalization, weak_tables/entries = the
// weak tables cleared. lua/gc_atomic/recorded = atomic phases recorded (the log line "[lua-gc-atomic]").
PERF_COUNTER(lua_gca_recorded, "lua/gc_atomic/recorded")
PERF_GAUGE(lua_gca_total_us, "lua/gc_atomic/total_us")
PERF_GAUGE(lua_gca_remark_us, "lua/gc_atomic/remark_us")
PERF_GAUGE(lua_gca_grayagain_us, "lua/gc_atomic/grayagain_us")
PERF_GAUGE(lua_gca_separate_us, "lua/gc_atomic/separateudata_us")
PERF_GAUGE(lua_gca_mmudata_us, "lua/gc_atomic/mmudata_us")
PERF_GAUGE(lua_gca_clearweak_us, "lua/gc_atomic/clearweak_us")
PERF_GAUGE(lua_gca_other_us, "lua/gc_atomic/other_us")
PERF_GAUGE(lua_gca_ga_tables, "lua/gc_atomic/grayagain_tables")
PERF_GAUGE(lua_gca_ga_entries, "lua/gc_atomic/grayagain_entries")
PERF_GAUGE(lua_gca_ga_threads, "lua/gc_atomic/grayagain_threads")
PERF_GAUGE(lua_gca_udata, "lua/gc_atomic/udata")
PERF_GAUGE(lua_gca_udata_gc, "lua/gc_atomic/udata_with_gc")
PERF_GAUGE(lua_gca_weak_tables, "lua/gc_atomic/weak_tables")
PERF_GAUGE(lua_gca_weak_entries, "lua/gc_atomic/weak_entries")
// 33-level-path.md, journal 34 (ai_level_vertex_fast 1): CLevelGraph::vertex(current, position) on the table of the
// rows of the packed xz. vertex_fast_hits = calls answered with the table (the vertex under the position, guess,
// nearest, the fast full search; the current vertex needs no table), vertex_fallback = the cvar on, the old code
// answered (no table, or nothing finite near the position), full_search_fast = the full search over the nearest rows
// (current vertex invalid, nothing under the position; ai/level_graph/full_search is then the old loop only).
PERF_COUNTER(lg_vertex_fast_hits, "ai/level_graph/vertex_fast_hits")
PERF_COUNTER(lg_vertex_fallback, "ai/level_graph/vertex_fallback")
PERF_TIMER(lg_vertex_full_fast, "ai/level_graph/full_search_fast")
// 33-level-path.md, journal 35: the cross table lookup of CGameObject::update_ai_locations (level vertex -> game
// vertex; the cells are in the mapped all.spawn, or in the memory with ai_cross_table_copy 1). lookup = the whole
// lookup (= the cross_ms of [perf-spike] go/ai_locations), read = the read of the cell only (a page fault is here),
// slow = the reads of 0.5 ms and more (the page read from the disk; calls = how many), ws_miss = the page of the cell
// was not in the working set of the process before the read (Windows, a page fault followed).
PERF_TIMER(ai_cross_lookup, "ai/cross_table/lookup")
PERF_TIMER(ai_cross_read, "ai/cross_table/read")
PERF_TIMER(ai_cross_slow, "ai/cross_table/slow")
PERF_COUNTER(ai_cross_ws_miss, "ai/cross_table/ws_miss")
// 32-frame-spikes.md, lua_wrapper_cache 1 (xrScriptEngine/script_wrapper_cache.hpp): one constant luabind wrapper per
// game_object / alife() / device() instead of a new userdata on every pass to Lua. hits = passes answered with the
// cached userdata, misses = wrappers made (the first pass of an object, then cached), live = cached wrappers alive (in
// the Lua registry; their owners not dead yet).
PERF_COUNTER(lua_wrapper_cache_hits, "lua/wrapper_cache_hits")
PERF_COUNTER(lua_wrapper_cache_misses, "lua/wrapper_cache_misses")
PERF_GAUGE(lua_wrapper_cache_live, "lua/wrapper_cache_live")
// 32-frame-spikes.md, U1 (lua_udata_profile 1; luabind with the GlobalWar hook set_instance_create_hook, object_rep.hpp;
// xrGame/lua_udata_profile.h): lua/udata/created = luabind class instances created (each a new Lua userdata: a C++
// object pushed by pointer or by value, a constructor called from Lua); the same for the classes game_object, vector,
// CTime, render_device (device()), alife_simulator (alife()). sampled = instances whose Lua call site was taken (one in
// lua_udata_profile_sample), sample_time = the cost of those samples. All 0 with lua_udata_profile 0.
PERF_COUNTER(lua_udata_created, "lua/udata/created")
PERF_COUNTER(lua_udata_game_object, "lua/udata/game_object")
PERF_COUNTER(lua_udata_vector, "lua/udata/vector")
PERF_COUNTER(lua_udata_ctime, "lua/udata/CTime")
PERF_COUNTER(lua_udata_device, "lua/udata/render_device")
PERF_COUNTER(lua_udata_alife, "lua/udata/alife_simulator")
PERF_COUNTER(lua_udata_sampled, "lua/udata/sampled")
PERF_TIMER(lua_udata_sample_time, "lua/udata/sample_time")
// 32-frame-spikes.md, U2 (lua_udata_profile 1; LuaJIT with the GlobalWar hook luaJIT_gw_udata_hook, luajit.h):
// lua/udata/all = every Lua userdata created (luabind instances, luabind iterators and function objects, engine holders,
// newproxy, io files); all_sampled = those whose creator was taken (one in lua_udata_profile_sample), all_sample_time =
// the cost of those samples. All 0 with lua_udata_profile 0.
PERF_COUNTER(lua_udata_all, "lua/udata/all")
PERF_COUNTER(lua_udata_all_sampled, "lua/udata/all_sampled")
PERF_TIMER(lua_udata_all_sample_time, "lua/udata/all_sample_time")
// 31-svp-scope.md, r__svp_light_dedup (Light_DB.cpp, CLight_DB::add_light): the lights the sequential scope pass
// (GL, RenderDoc, no free dsgraph context) tried to export into Lights.package again after the main pass of the same
// frame had exported them (with r__svp_light_dedup 0 they are exported again and accumulated twice in the lens).
PERF_COUNTER(rsvp_light_dup, "render/svp/light_dup")
// 33-level-path.md, journal 36, ai_game_graph_copy 1 (AISpaceBase::SetGameGraph): copy = the copy of the game graph
// (vertices, edges, death points) from the mapped all.spawn into the memory, once per ALife session; copy_kb = the size
// of that copy while its graph lives (0 without it).
PERF_TIMER(ai_game_graph_copy, "ai/game_graph/copy")
PERF_GAUGE(ai_game_graph_copy_kb, "ai/game_graph/copy_kb")
// 32-frame-spikes.md, lua_wrapper_nogc 1 (luabind with the GlobalWar patch set_nonowning_instance_nogc, object_rep.hpp):
// lua/udata/nogc_created = luabind instances over a raw pointer (pointer_holder<T*>: game_object, alife(), device(), a
// reference returned by a method) made without __gc - LuaJIT frees them in the GC cycle they die in, no finalizer
// queue (mmudata). 0 with lua_wrapper_nogc 0.
PERF_COUNTER(lua_udata_nogc_created, "lua/udata/nogc_created")
// 34-sound-cold-load.md (xrSound, SoundRender_Source.cpp / SoundRender_Core_SourceManager.cpp). Where a cold load of a
// sound source waits, any thread: sound/source/load_open = LoadWave's FS.r_open + ov_open_callbacks (headers, seek to
// the end: the first reads of a file not in the OS cache), load_r_open = the FS.r_open alone (lookup, CreateFile,
// mapping), load_pcm = cache_pcm_if_short (a second open + the full decode); ge/cold/sound_open, ge/cold/sound_pcm = the
// same inside game events (parts of ge/cold/sound_load). sound/source/load_dup = a source loaded twice at once (two
// threads), the later copy freed.
PERF_TIMER(snd_load_open, "sound/source/load_open")
PERF_TIMER(snd_load_r_open, "sound/source/load_r_open")
PERF_TIMER(snd_load_pcm, "sound/source/load_pcm")
PERF_TIMER(ge_cold_sound_open, "ge/cold/sound_open")
PERF_TIMER(ge_cold_sound_pcm, "ge/cold/sound_pcm")
PERF_COUNTER(snd_source_load_dup, "sound/source/load_dup")
// snd_source_prefetch (temporary): queued = folders put in the queue (a script's sound_prefetch_dir and auto), auto = of
// them the folder of a sound loaded cold in a game event, queue = folders waiting; files/kb = .ogg files read ahead
// into the OS cache; loaded = sources loaded by the prefetch thread (2), src_hit = the first ask for such a source, dup =
// one main loaded first (freed); warm_hit / warm_miss = cold loads (s_sources misses) whose folder had / had not been
// read ahead, ge/cold/sound_warm_hit / _miss = the same inside game events; time = the thread's time per folder;
// dir_missing / dir_skipped = no such folder / more than 256 .ogg in it; not_indexed = a file not in the FS index (2).
PERF_COUNTER(snd_prefetch_queued, "sound/prefetch/queued")
PERF_COUNTER(snd_prefetch_auto, "sound/prefetch/auto")
PERF_GAUGE(snd_prefetch_queue, "sound/prefetch/queue")
PERF_COUNTER(snd_prefetch_files, "sound/prefetch/files")
PERF_COUNTER(snd_prefetch_kb, "sound/prefetch/kb")
PERF_COUNTER(snd_prefetch_loaded, "sound/prefetch/loaded")
PERF_COUNTER(snd_prefetch_src_hit, "sound/prefetch/src_hit")
PERF_COUNTER(snd_prefetch_dup, "sound/prefetch/dup")
PERF_COUNTER(snd_prefetch_warm_hit, "sound/prefetch/warm_hit")
PERF_COUNTER(snd_prefetch_warm_miss, "sound/prefetch/warm_miss")
PERF_COUNTER(ge_cold_sound_warm_hit, "ge/cold/sound_warm_hit")
PERF_COUNTER(ge_cold_sound_warm_miss, "ge/cold/sound_warm_miss")
PERF_TIMER(snd_prefetch_time, "sound/prefetch/time")
PERF_COUNTER(snd_prefetch_dir_missing, "sound/prefetch/dir_missing")
PERF_COUNTER(snd_prefetch_dir_skipped, "sound/prefetch/dir_skipped")
PERF_COUNTER(snd_prefetch_not_indexed, "sound/prefetch/not_indexed")
// 35-lua-gc-finobj.md, always on (LuaJIT with the GlobalWar patch luaJIT_gw_finobj, luajit.h LUAJIT_GW_FINOBJ):
// the userdata with __gc in a list of their own. With lua_gc_atomic_stats 1, the last atomic phase: fin_udata = walked
// in that list, plain_udata = in the other one (0 unless walked all: finobj just switched on, __gc added to a
// watched metatable; without the patch = all userdata), fin_pending = userdata that got __gc while not the newest
// one (moved by the sweep); fin_full = atomic phases that walked all userdata. Always (per frame
// in CLevel::script_gc): lua/gc_finobj/moved_head = moved at lua_setmetatable (the newest userdata), pending = flagged
// for the sweep, moved_sweep = moved by the sweep, moved_full = moved by a walk of all userdata, full_walks/full_mt =
// such walks / of them for __gc added to a watched metatable, kept = flagged ones dead at the atomic phase (finalized a
// cycle later); watched = metatables without __gc watched now.
PERF_GAUGE(lua_gca_fin_udata, "lua/gc_atomic/fin_udata")
PERF_GAUGE(lua_gca_plain_udata, "lua/gc_atomic/plain_udata")
PERF_GAUGE(lua_gca_fin_pending, "lua/gc_atomic/fin_pending")
PERF_COUNTER(lua_gca_fin_full, "lua/gc_atomic/fin_full")
PERF_COUNTER(lua_finobj_moved_head, "lua/gc_finobj/moved_head")
PERF_COUNTER(lua_finobj_pending, "lua/gc_finobj/pending")
PERF_COUNTER(lua_finobj_moved_sweep, "lua/gc_finobj/moved_sweep")
PERF_COUNTER(lua_finobj_moved_full, "lua/gc_finobj/moved_full")
PERF_COUNTER(lua_finobj_full_walks, "lua/gc_finobj/full_walks")
PERF_COUNTER(lua_finobj_full_mt, "lua/gc_finobj/full_mt")
PERF_COUNTER(lua_finobj_kept, "lua/gc_finobj/kept")
PERF_GAUGE(lua_finobj_watched, "lua/gc_finobj/watched")
// 36-spawn-model-load.md (plans/optimization_mt), Layers/xrRender. What a cold model load (CModelPool::Create had to
// load the ogf: modelpool/instance_load) is made of, any thread, counted only inside such a load (its LOD and children
// included, so the parts of one nested load add up, they do not overlap): open = FS.exist + FS.r_open of an ogf (the
// base, its LOD); shader = dxRender_Visual::Load's shader.create (blender, passes, constants, textures - a texture not
// loaded yet is texture/create_sync inside it); geom = Fvisual::Load (vertex / index buffers); skin = CSkeletonX _Load +
// _Load_hw (bone weights, the conversion of the vertices to the HW format, its vertex buffer); child_find =
// CreateChild's Instance_Find (a linear walk of all base models for every child); userdata = the CInifile of
// OGF_S_USERDATA; bones = bone names, IK data, AfterLoad and the unique bone faces; motions = CKinematicsAnimated's
// motions (all omf of the model), omf_file = of it an omf not in the motions container yet (r_open + read + parse),
// partition = of it CPartition::load (the CInifile of meshes\<model>.ltx, mostly a missing file); materials = the bone
// game materials of Instance_Load.
PERF_TIMER(mload_open, "modelpool/load/open")
PERF_TIMER(mload_shader, "modelpool/load/shader")
PERF_TIMER(mload_geom, "modelpool/load/geom")
PERF_TIMER(mload_skin, "modelpool/load/skin")
PERF_TIMER(mload_child_find, "modelpool/load/child_find")
PERF_TIMER(mload_userdata, "modelpool/load/userdata")
PERF_TIMER(mload_bones, "modelpool/load/bones")
PERF_TIMER(mload_motions, "modelpool/load/motions")
PERF_TIMER(mload_omf_file, "modelpool/load/omf_file")
PERF_TIMER(mload_partition, "modelpool/load/partition")
PERF_TIMER(mload_materials, "modelpool/load/materials")
// ge/cold/model_frame = cold model loads of one ProcessGameEvents pass (max per frame); spawn/cold_stopped = passes the
// spawn_cold_models limit stopped in front of a root M_SPAWN (temporary cvar, 0 = off).
PERF_GAUGE(ge_cold_model_frame, "ge/cold/model_frame")
PERF_COUNTER(spawn_cold_stopped, "spawn/cold_stopped")
// model_prefetch bit 1 (temporary): the visuals of the creatures of the level (ALife objects, offline ones included)
// loaded into the model pool on the loading screen (net_start_client5). models = loaded, already = were in the pool,
// time = the whole preload.
PERF_COUNTER(mpreload_models, "modelpool/preload/models")
PERF_COUNTER(mpreload_already, "modelpool/preload/already")
PERF_TIMER(mpreload_time, "modelpool/preload/time")
// model_prefetch bit 2 (temporary): the "Model prefetch" thread reads the files of a model (ogf, its LOD ogf, its omf,
// its textures) into the OS cache before the model comes online. queued = models queued (each once per session),
// alife = asks from ALife (an offline object or squad nearer than the online distance + model_prefetch_margin %),
// deferred = asks from spawns the budget left in the queue, queue = models waiting (gauge); files / kb = files read;
// time = the thread's time per model; missing = a file of a model not found; cold_warm / cold_miss = cold loads of
// a model the thread had / had not read before.
PERF_COUNTER(mprefetch_queued, "modelpool/prefetch/queued")
PERF_COUNTER(mprefetch_alife, "modelpool/prefetch/alife")
PERF_COUNTER(mprefetch_deferred, "modelpool/prefetch/deferred")
PERF_GAUGE(mprefetch_queue, "modelpool/prefetch/queue")
PERF_COUNTER(mprefetch_files, "modelpool/prefetch/files")
PERF_COUNTER(mprefetch_kb, "modelpool/prefetch/kb")
PERF_TIMER(mprefetch_time, "modelpool/prefetch/time")
PERF_COUNTER(mprefetch_missing, "modelpool/prefetch/missing")
PERF_COUNTER(mprefetch_cold_warm, "modelpool/prefetch/cold_warm")
PERF_COUNTER(mprefetch_cold_miss, "modelpool/prefetch/cold_miss")
// 37-destroy-relcase-smooth.md: objects/relcase/total = the whole destroy phase of CObjectList::Update (child scan,
// relcase, sound, callbacks, net_Destroy), calls = frames with a destroy, max_frame = the tallest such frame. With
// ol_relcase_sleep_parallel 1: sleep_deferred = the own parts of the sleeping stalkers put off (corpses mostly; also in
// parallel_tasks), sleep_parallel_wall = their phase (wall, a part of objects/relcase/sleeping). With
// ol_relcase_skip_noop_script 1: script_noop_skipped = script factory objects that skipped the batch (also in
// active_skipped / sleeping_skipped).
PERF_TIMER(relcase_total, "objects/relcase/total")
PERF_COUNTER(relcase_sleep_deferred, "objects/relcase/sleep_deferred")
PERF_TIMER(relcase_sleep_parallel_wall, "objects/relcase/sleep_parallel_wall")
PERF_COUNTER(relcase_script_noop_skipped, "objects/relcase/script_noop_skipped")
// 39-ucl-stalker-spike.md: the time the main thread was off the CPU inside a batch of the UpdateCL loop (wall minus
// the thread CPU time of ol_Batch256; calls = batches). ~0 normally; a stall (OS preemption, the debugger, a hard page
// fault) shows as max_frame, with a "! [ucl-stall]" line naming the object.
PERF_TIMER(ol_upd_offcpu, "objects/update/offcpu")
// 37-destroy-relcase-smooth.md (offline wave): alife/switch_offline_spread = client objects the spread switch pass took
// offline (a part of alife/switch_offline_frame - the rest came another way); alife/switch_offline_unestimated = those
// of them whose object switch_offline_estimate gave 0 (the per frame limit could not foresee them).
// objects/relcase/monster_own (M1) = the own parts of the monsters put off to the parallel phase (sum over the threads,
// calls = parts; objects/relcase/monsters is then the part on the thread of the pass).
PERF_COUNTER(alife_switch_offline_spread, "alife/switch_offline_spread")
PERF_COUNTER(alife_switch_offline_unestimated, "alife/switch_offline_unestimated")
PERF_TIMER(relcase_monster_own, "objects/relcase/monster_own")
// 33-level-path.md, sliced A* (CLevelGraph::SearchSlice, CLevelPathBuilder::process): astar_put_aside = level A* slices
// that stopped at the end of their time slice (the build goes on in the next frame), astar_resumed = slices that
// went on with a search put aside, astar_no_slot = searches over their slice that ran to the end (every slot busy),
// astar_dropped = searches put aside and never finished (the owner changed its call or went away), astar_slices_max /
// astar_sliced_us_max = slices and the time of all of them of a sliced search finished in the frame (per-frame
// maxima; the time is what that search would have taken in one go), slice_forced = level path builds run unsliced
// (their call changed under 8 searches put aside in a row, or 600 builds put aside in a row).
PERF_COUNTER(level_path_astar_put_aside, "level_path/astar_put_aside")
PERF_COUNTER(level_path_astar_resumed, "level_path/astar_resumed")
PERF_COUNTER(level_path_astar_no_slot, "level_path/astar_no_slot")
PERF_COUNTER(level_path_astar_dropped, "level_path/astar_dropped")
PERF_GAUGE(level_path_astar_slices_max, "level_path/astar_slices_max")
PERF_GAUGE(level_path_astar_sliced_us_max, "level_path/astar_sliced_us_max")
PERF_COUNTER(level_path_slice_forced, "level_path/slice_forced")
// 33-level-path.md, sliced A*: astar_pops_max = heap pops of a level A* that finished in the frame (all its slices;
// per-frame maximum). The heuristic (Manhattan x2) is not consistent, a vertex is expanded again when a cheaper way to
// it turns up: pops / astar_visited_max is the work per vertex (1-2 in the open, 10-27 behind walls on the stand).
PERF_GAUGE(level_path_astar_pops_max, "level_path/astar_pops_max")
