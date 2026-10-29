#pragma once

enum class ENpcCppProfileStage : u32
{
    GameObjectScheduleUpdate = 0,
    ScriptBinderUpdate,
    ScriptBinderLuabindUpdate,
    ScriptEntityProcessScripts,
    ScriptEntityProcessSoundCallbacks,
    ScriptEntitySoundCallbackDispatch,
    StalkerScheduleUpdate,
    StalkerUpdateCL,
    StalkerThink,
    StalkerBrainUpdate,
    StalkerPlannerSolve,
    StalkerPlannerActualityCheck,
    StalkerPlannerActualityFastPath,
    StalkerPlannerActualitySkipped,
    StalkerPlannerStateClear,
    StalkerPlannerGraphSearch,
    StalkerPlannerTransition,
    StalkerPlannerExecute,
    StalkerPlannerExecuteDeath,
    StalkerPlannerExecuteALife,
    StalkerPlannerExecuteCombat,
    StalkerPlannerExecuteDanger,
    StalkerPlannerExecuteAnomaly,
    StalkerPlannerExecuteGatherItems,
    StalkerPlannerExecuteOther,
    StalkerThinkMovementUpdate,
    StalkerMemoryUpdate,
    StalkerMemoryVisualUpdate,
    StalkerMemorySoundUpdate,
    StalkerMemoryHitUpdate,
    StalkerMemoryCollectObjects,
    StalkerMemoryCollectVisualObjects,
    StalkerMemoryCollectSoundObjects,
    StalkerMemoryCollectHitObjects,
    StalkerMemoryCollectDangerAdd,
    StalkerMemoryCollectEnemyAdd,
    StalkerMemoryCollectItemAdd,
    StalkerDangerAddVisible,
    StalkerDangerAddSound,
    StalkerDangerAddHit,
    StalkerDangerAddDangerObject,
    StalkerDangerUsefulCheck,
    StalkerDangerFindExisting,
    StalkerEnemyUsefulCheck,
    StalkerEnemyUsefulAliveCheck,
    StalkerEnemyUsefulSpatialCheck,
    StalkerEnemyUsefulRelationCheck,
    StalkerEnemyUsefulVertexCheck,
    StalkerEnemyUsefulMonsterFilter,
    StalkerEnemyUsefulCallback,
    StalkerEnemyEvaluate,
    StalkerEnemyExpedient,
    StalkerEnemyTryChange,
    StalkerEnemyProcessWounded,
    StalkerEnemyNeedUpdate,
    StalkerEnemyInertedUpdate,
    StalkerMemoryUpdateEnemies,
    StalkerMemoryItemUpdate,
    StalkerMemoryDangerUpdate,
    StalkerObjectHandlerUpdate,
    StalkerUpdateCLObjectHandlerDispatch,
    StalkerUpdateCLInherited,
    StalkerUpdateCLPhysics,
    StalkerUpdateCLSightManager,
    StalkerUpdateCLExecLook,
    StalkerUpdateCLStepManager,
    StalkerUpdateCLWeaponEffector,
    // CBaseMonster::UpdateCL: control().update_frame(), the per-frame work of the monster controllers
    MonsterUpdateCLControl,
    StalkerScheduleVisibility,
    StalkerScheduleThinkApply,
    CharacterPhysicsUpdateCL,
    CharacterPhysicsAnimationCollision,
    CharacterPhysicsCalculateTimeDelta,
    CharacterPhysicsShellSetRagdoll,
    CharacterPhysicsShellInterpolate,
    CharacterPhysicsInteractiveMotionUpdate,
    CharacterPhysicsDeathAnims,
    CharacterPhysicsFriction,
    CharacterPhysicsUpdateInteractiveAnims,
    CharacterPhysicsIkUpdate,
    CustomMonsterScheduleUpdate,
    CustomMonsterUpdateCL,
    CustomMonsterUpdateCLInherited,
    CustomMonsterUpdateCLNetworkExtrapolation,
    CustomMonsterUpdateCLUpdatePositionAnimation,
    CustomMonsterUpdateCLApplyNetState,
    CustomMonsterUpdateCLUpdateCamera,
    CustomMonsterUpdateCLAnimationController,
    CustomMonsterThink,
    CustomMonsterMemoryUpdate,
    CustomMonsterExecVisibility,
    CustomMonsterVisibilityS0,
    CustomMonsterVisibilityS1,
    CustomMonsterVisibilityS2,
    CustomMonsterSoundPlayerUpdate,
    PhysicsShellHolderUpdateCL,
    PhysicsShellHolderUpdateParticles,
    GameObjectUpdateCLSpatial,
    GameObjectUpdateCLCrow,
    GameObjectUpdateCLMatrixChange,
    ScriptEvaluatorEvaluate,
    ScriptActionUpdate,
    ScriptActionInitialize,
    InventoryOwnerHasInfo,
    AddonBinderUpdate, // on_update batches of the plugin binders, delivered after the scheduler pass
    // The movement of a monster / stalker in UpdateCL (CMovementManager::on_frame): the path update, the step along
    // the path, inside it the query of the physical objects around the destination and the collision branch it opens
    MovementUpdatePath,
    MovementMoveAlongPath,
    MovementNearestQuery,
    MovementCollisionMove,
    CustomMonsterSelectAnimation,
    // The sounds an NPC heard (CScriptEntity::process_sound_callbacks): the Lua callback and the plugin event apart
    ScriptEntitySoundLuaCallback,
    ScriptEntitySoundPluginEvent,
    // CStepManager::update: the whole step pass, the material pick, the step sound, particles and the step event
    StepUpdate,
    StepMaterialPick,
    StepSound,
    StepParticles,
    StepEvent,
    // CAgentEnemyManager::distribute_enemies of a squad (once a frame): the whole distribution, its fill and its
    // assignment
    AgentEnemyDistribute,
    AgentEnemyFill,
    AgentEnemyAssign,
    ScriptBinderUpdateStalker,
    ScriptBinderUpdateMonster,
    ScriptBinderUpdateSmartZone,
    ScriptBinderUpdateItem,
    ScriptBinderUpdateOther,
    MovementTryPositionFail,
    MovementKinematic,
    CharacterPhysicsCorpseEnabled,
    CharacterPhysicsCorpseAge0_3,
    CharacterPhysicsCorpseAge3_15,
    CharacterPhysicsCorpseAge15_60,
    CharacterPhysicsCorpseAge60p,
    EntityAliveDie,
    StalkerDie,
    CharacterPhysicsActivateShell,
    CharacterPhysicsInDie,
    CharacterPhysicsInHit,
    ExplosiveExplode,
    // Stage 0 of plans/optimization_arch/NPC_UPDATECL_WORKERS_2026-10-08.md (count() rows, no time):
    // IK updates by the rule that picked their interval, those of NPCs not seen in the last main pass, foot rays
    IkRunEnemySelected,
    IkRunDistance,
    IkRunOffscreen,
    IkRays,
    // move_along_path with neighbours: the calls where one of them has a body within contact reach this frame
    // (an estimate from the bounding spheres) - the rest pay TryPosition to learn there is no contact
    MovementNearestCloseContact,
    // Stalker UpdateCL calls of an NPC not seen in the last main pass, and those also farther than 25 m from the
    // camera without an enemy: what an update-rate LOD (U1) could skip
    UclLodOffscreen,
    UclLodCandidate,
    // Stage 3 (the UpdateCL LOD, CAI_Stalker::TrySkipUpdateCLLod), stalkers beyond npc_perf_ucl_lod_dist only:
    // UpdateCL skipped in the 2nd-frame tier (D1) and in the 3rd-frame tier (D2); full UpdateCL on the own frame of
    // the tier; skips refused by a fight (enemy, recent hit, weapon firing), by any other rule, and by the time cap
    // (npc_perf_ucl_lod_max_delta_ms); full UpdateCL right after a skip and the sum of their time deltas in ms
    // (avg = after_skip_ms / after_skip); those of them whose real elapsed time was clamped by the time cap; idle
    // weapons in the hands skipped together with their owner
    UclLodSkipD1,
    UclLodSkipD2,
    UclLodFull,
    UclLodDenyCombat,
    UclLodDenyOther,
    UclLodDenyTimeCap,
    UclLodAfterSkip,
    UclLodAfterSkipMs,
    UclLodDeltaCapHit,
    UclLodWeaponSkip,
    // Weapon UpdateCL of NPC weapons: the one in the hands and the hidden ones in the slots
    WeaponUclNpcActive,
    WeaponUclNpcHidden,
    // Stage 2b: of those hidden ones, the UpdateCL calls skipped (CWeapon::TrySkipHiddenUpdateCL)
    WeaponUclNpcHiddenSkipped,
    // Stage 4 (move_along_path, npc_perf_move_try_skip_*): TryPosition calls skipped by the near K (on screen,
    // close to the camera) and by the far K (off screen / far); checks the "never skip" rules took from the skip
    // budget - by the NPC's own state and by the actor or a body close by; and TryPosition failures on the first
    // check after a run of skips (what the skipping let in)
    MovementTrySkipNear,
    MovementTrySkipFar,
    MovementTrySkipForcedState,
    MovementTrySkipForcedBody,
    MovementTrySkipFailAfterSkip,
    // Stage 7 of plans/optimization_arch/NPC_UPDATECL_WORKERS_2026-10-08.md: the parts of CAI_Stalker::shedule_Update
    // that had no row of their own (with visibility, memory_update and think_apply they cover the whole of it)
    StalkerSchedulePrecalc,
    StalkerScheduleAgentManager,
    StalkerScheduleProcessEnemies,
    StalkerScheduleInherited,
    StalkerScheduleScriptControl,
    StalkerScheduleFeelTouch,
    StalkerScheduleInventoryOwner,
    StalkerSchedulePhysics,
    // The useful callback of the enemy manager split: the Lua callback of the NPC and the enemy filters of plugins
    StalkerEnemyUsefulCallbackLua,
    StalkerEnemyUsefulCallbackPlugin,
    // P16 (plans/optimization_mt/11-ragdoll-spikes.md): death -> ragdoll, the parts of character_physics/in_die and
    // activate_shell. kill_hit is the in_Hit path (KillHitImpl, also run deferred from the post-step flush, then outside
    // in_die); create_shell with its parts create_skeleton (the shell built at death), bones (CalculateBones around the
    // start), run_simulation (into the world); weapon_collision (the held weapon's geoms moved into the ragdoll);
    // end_activate (start of a free ragdoll) with death_correct (CollisionCorrectObjPos - the box pushed out in a frozen
    // world) and fly_to (the ragdoll flown back after it, 10 steps); death_motion (death animation set up and started);
    // death_deferred - deaths queued by defer_scheduler_mutation (count); skeleton_prebuild (TryPrebuildSkeleton,
    // on the scheduler) and, at death, prebuilt skeletons used / found stale (counts)
    CharacterPhysicsKillHit,
    CharacterPhysicsCreateShell,
    CharacterPhysicsCreateSkeleton,
    CharacterPhysicsCreateShellBones,
    CharacterPhysicsCreateShellRun,
    CharacterPhysicsWeaponCollision,
    CharacterPhysicsEndActivate,
    CharacterPhysicsDeathCorrect,
    CharacterPhysicsFlyTo,
    CharacterPhysicsDeathMotion,
    CharacterPhysicsDeathDeferred,
    CharacterPhysicsSkeletonPrebuild,
    CharacterPhysicsSkeletonPrebuiltUsed,
    CharacterPhysicsSkeletonPrebuiltStale,
    // E0 of plans/lua_to_cpp/27-gw-eoc-e0.md, the offline simulation: CALifeUpdateManager::update and its two parts;
    // the update() of every scheduled server object (CALifeScheduleRegistry::CUpdatePredicate) by its kind - offline
    // squads, smart terrains, offline monsters/stalkers, the rest; counters: the size of the scheduled registry on
    // every update_scheduled (count/frame = the average size when it runs once a frame) and the updates that took
    // longer than 2 / 5 ms (a stand-in for p95, the profiler has no histograms)
    AlifeUpdate,
    AlifeUpdateSwitch,
    AlifeUpdateScheduled,
    AlifeScheduledSquad,
    AlifeScheduledSmartTerrain,
    AlifeScheduledMonster,
    AlifeScheduledOther,
    AlifeScheduledRegistrySize,
    AlifeUpdateOver2ms,
    AlifeUpdateOver5ms,
    // 45-engine-ai-audit.md, F11 (diagnostics only): CAI_Stalker::update_best_item_info_impl - it runs on every
    // best_weapon() and in four item evaluators; update = the whole call (calls = how often), lua_lookup = the
    // lookup of the Lua function ai_stalker.update_best_weapon by its name (CScriptEngine::functor), lua_call = the
    // call of it; the rest of update is the C++ scan of the inventory and of the remembered items
    StalkerBestItemUpdate,
    StalkerBestItemLuaLookup,
    StalkerBestItemLuaCall,
    // plans/lua_to_cpp/33-gw-eoc-alife-switch.md (diagnostics only): the switch pass (alife/update/switch) by object.
    // switch_pass/<kind> - CALifeSwitchManager::switch_object of one object of the level registry by its kind (calls =
    // the objects visited; the time is the whole call: the decision and, when it switches, the switch itself);
    // sync_location, go_online, go_offline - the synchronize_location and the switches themselves, nested in the kinds;
    // can_online/<k>, can_offline/<k> - the can_switch_online / can_switch_offline calls of the decisions
    // (CSE_ALifeDynamicObject / CSE_ALifeOnlineOfflineGroup::try_switch_*), nested in the kinds: a Lua call (luabind
    // call_member) for an object of a script class (se_*, sim_squad_scripted); k = squad, creature (the stalkers and
    // monsters out of squads), other (everything else); keep_saved - keep_saved_data_anyway of clear_client_data and of
    // CALifeSwitchManager::try_switch_online (a Lua call too, for every offline object and every member of an offline
    // squad a pass), nested in the kinds; keep_saved_empty (a counter) - those of them on an object with no client data,
    // whose answer changes nothing (clear() of an empty vector): not asked since N1; can_skipped (a counter) - the
    // can_switch_* calls N2 did not need (the distance settled them). N1/N2/N3 always on since 2026-10-25
    AlifeSwitchSquad,
    AlifeSwitchStalker,
    AlifeSwitchMonster,
    AlifeSwitchItem,
    AlifeSwitchSmartTerrain,
    AlifeSwitchAnomaly,
    AlifeSwitchOther,
    AlifeSwitchSyncLocation,
    AlifeSwitchGoOnline,
    AlifeSwitchGoOffline,
    AlifeSwitchCanOnlineSquad,
    AlifeSwitchCanOnlineCreature,
    AlifeSwitchCanOnlineOther,
    AlifeSwitchCanOfflineSquad,
    AlifeSwitchCanOfflineCreature,
    AlifeSwitchCanOfflineOther,
    AlifeSwitchKeepSaved,
    AlifeSwitchKeepSavedEmpty,
    AlifeSwitchCanSkipped,
    // 33-gw-eoc-alife-switch.md section 9 (2026-10-25): the switch_pass/* stages above are gathered on the thread of the pass
    // without atomics and flushed once a part of the pass (a frame's part of update_spread, or update_switch); the timed
    // ones (the kinds, sync_location, go_*, can_*, keep_saved) only in every 8th part - part_sampled (its calls = such
    // parts, the time = the whole part with the timers), the counters in every part. part_clean - the other parts, with
    // no timer inside: the time of the pass without the profiler. sync_fast / sync_memo / sync_full (counters): the
    // synchronize_location of the pass - settled by the pure checks of the position (no Lua call, no graph search), by
    // the memo of an unchanged object, or the full call; sync_full_unchanged - full calls that changed nothing
    AlifeSwitchPartSampled,
    AlifeSwitchPartClean,
    AlifeSwitchSyncFast,
    AlifeSwitchSyncMemo,
    AlifeSwitchSyncFull,
    AlifeSwitchSyncFullUnchanged,
    // 33-gw-eoc-alife-switch.md section 10 (counters, in every part of the pass):
    // n3_native/<method> - a call of used_ai_locations / can_switch_online / can_switch_offline / keep_saved_data_anyway
    // on an object whose Lua class does not override the method: the C++ default without luabind::call_member (N3);
    // n3_lua/<method> - the call went to a Lua override; n3_luabind - an unusual lookup (no method, a C++ function Lua
    // assigned): luabind::call_member as before. sync_exit_* - the synchronize_location of the pass that neither the
    // fast check nor the memo settled and that stopped at a check: attached, offline on an invalid level vertex (both
    // read only fields, asked first since section 10), used_ai_locations false
    AlifeSwitchN3NativeCanOnline,
    AlifeSwitchN3NativeCanOffline,
    AlifeSwitchN3NativeKeepSaved,
    AlifeSwitchN3NativeUsedAi,
    AlifeSwitchN3LuaCanOnline,
    AlifeSwitchN3LuaCanOffline,
    AlifeSwitchN3LuaKeepSaved,
    AlifeSwitchN3LuaUsedAi,
    AlifeSwitchN3Luabind,
    AlifeSwitchSyncExitAttached,
    AlifeSwitchSyncExitOfflineInvalid,
    AlifeSwitchSyncExitNoAi,
    // 35-gw-eoc-remaining.md, E11-v (diagnostics only): CALifeOnlineOfflineGroupBrain::update - the C++ update of an
    // offline squad (squad_brain/update: the whole of it) and the virtual get_current_task it asks first (a Lua call:
    // sim_squad_scripted:get_current_task) by the class of the commander - stalker / monster
    AlifeSquadBrainUpdate,
    AlifeSquadTaskStalker,
    AlifeSquadTaskMonster,
    // 53-create-squad-engine.md (diagnostics only): the server side of a created object.
    // alife/create/<kind> - CALifeSimulatorBase::spawn_item (alife():create of Lua: entity, register, supplies, on_spawn)
    // by the kind of the object; the items a stalker spawns (PDA, supplies) are alife/create/other nested in its row.
    // alife/create/<stalker|monster>/register and /spawn_supplies - the two parts of it.
    // alife/on_register/<kind> - object->on_register() in register_object (every path, the load too; for a stalker the
    // Lua se_stalker:on_register and the first specific_character() in it); alife/on_register/event - the
    // server_object_on_register event of these kinds (Lua and plugins).
    // spec_char/select - a selection of a specific character by a template profile (CSE_ALifeTraderAbstract::
    // specific_character); spec_char/select/scanned (a counter) - the profiles it looked at; spec_char/select/indexed (a
    // counter) - the selections (by the index); /full - always 0 since 2026-10-25 (the full scan and gw_spec_char_index
    // removed; the stage kept so that the order of the stages stays); spec_char/index_build -
    // the index (once); spec_char/set - set_specific_character; spec_char/spawn_supplies - CSE_ALifeTraderAbstract::
    // spawn_supplies (PDA, the second selection, the supplies of the character)
    AlifeCreateStalker,
    AlifeCreateMonster,
    AlifeCreateSquad,
    AlifeCreateOther,
    AlifeCreateStalkerRegister,
    AlifeCreateStalkerSupplies,
    AlifeCreateMonsterRegister,
    AlifeCreateMonsterSupplies,
    AlifeOnRegisterStalker,
    AlifeOnRegisterMonster,
    AlifeOnRegisterSquad,
    AlifeOnRegisterEvent,
    SpecCharSelect,
    SpecCharSelectScanned,
    SpecCharSelectIndexed,
    SpecCharSelectFull,
    SpecCharIndexBuild,
    SpecCharSet,
    SpecCharSpawnSupplies,
    // plans/lua_to_cpp/55-timeskip-squad-movement.md (alife_time_skip.cpp): the catch-up of the offline movement after a
    // jump of the clock. alife/time_skip - the whole catch-up (one sample per skip), /step - one step (the event
    // included), /retarget - the C++ updates of the squads of a tick step (get_current_task inside), /move - the
    // positions of a step, /event - alife_on_time_skip_step (the plugins and the Lua they call), /steps (a counter),
    // /squads_moved (a counter: squad moves to another game vertex), /held (a counter: squads held at the online radius
    // of the actor); alife/fresh_offline_update - the scheduled updates of the squads just gone offline (E2)
    AlifeTimeSkip,
    AlifeTimeSkipStep,
    AlifeTimeSkipRetarget,
    AlifeTimeSkipMove,
    AlifeTimeSkipEvent,
    AlifeTimeSkipSteps,
    AlifeTimeSkipSquadsMoved,
    AlifeTimeSkipHeld,
    AlifeFreshOfflineUpdate,
    // plans/lua_to_cpp/57-gw-eoc-next-steps.md, E2 (diagnostics only): script_binder/update/smartzone split by the class
    // of the zone - smart_terrain (SMRTTRRN, bind_smart_terrain: se_smart_terrain:update with its own 500 ms throttle
    // inside), restrictor (SPC_RS_S: bind_restrictor, bind_anomaly_zone, bind_camp) and other; each smart zone
    // binder is also in binder_section_top of the report by the section of the zone
    ScriptBinderUpdateSmartZoneSmartTerrain,
    ScriptBinderUpdateSmartZoneRestrictor,
    ScriptBinderUpdateSmartZoneOther,
    // plans/lua_to_cpp/60-npc-xr-logic-pick-section.md, P2 (measurement only): the native condlist interpreter
    // (gw_condlist_native.cpp, LuaPickSection). condlist/pick - the whole
    // native call; /prepare - the checks before the evaluation (registry tables, the compiled condlist, the argument
    // types, the identities of the functions and xr_actions, the zones), every call, its fallbacks included; /eval -
    // the evaluation and the result string of a call the native side decides; /fallback (a counter) - the calls that
    // went back to the Lua body (pick_section_lua). Lua zone xr_logic/pick_section minus condlist/pick = the bridge
    CondlistPick,
    CondlistPickPrepare,
    CondlistPickEval,
    CondlistPickFallback,
    // plans/lua_to_cpp/58-npc-motivator.md sec. 7 (counters, measurement only): the phase gate of the Lua binder of a
    // stalker (CGameObject::binder_gate_update, a binder that reads the mask).
    // skipped - no phase due, the Lua binder is not called; medium_only - only the medium phase is due (what plan 58
    // M6 would take off the Lua binder); slow / switch / callback - the passes with that phase due
    BinderGateSkipped,
    BinderGateMediumOnly,
    BinderGateSlow,
    BinderGateSwitch,
    BinderGateCallback,
    // plans/lua_to_cpp/57-gw-eoc-next-steps.md, E0 + E4 (diagnostics only). The task of an offline squad
    // (alife_online_offline_group_brain.cpp, alife_squad_task_native.h), counters by the source:
    // alife/squad_brain/task/native/<job|smart|squad|self> - decided in C++;
    // /lua/<actor|other_class|odd|no_script> - the cases left to the Lua sim_squad_scripted:get_current_task.
    // AlifeSquadTaskLua (task/lua) - unused since C2 (was gw_squad_task_native 0); AlifeSquadTaskVerify /
    // AlifeSquadTaskVerifyMismatch (task/verify*) - unused since C2 (was gw_squad_task_native 2). Kept to preserve the
    // enum layout. Its time stays in alife/squad_brain/get_current_task/<stalker|monster>.
    // alife/squad_update/members - CSE_ALifeOnlineOfflineGroup::update around the brain (sanitize_members, the copy of
    // the position to the members). alife/offline_path/* - CALifeMonsterDetailPathManager::update (the movement part of
    // squad_brain/update): first (a counter: the first update after a load or a spawn - only the clock starts), idle (a
    // counter: at the destination), search (the A* of the game graph), search_failed (a counter), follow (the walk;
    // vertex_change and vertex_hook nested), vertex_change (the move to the next game vertex: graph registry,
    // on_location_change - also from update_position), vertex_hook (the time skip rule at a vertex).
    // alife/time_skip/move/squad - update_position of the squads in the move pass of a step (add_many per step, the
    // max is the longest squad), /move/other - the rest of the pass (the other objects and the loop, max 0);
    // /move/squad_moves and /retarget/moves (counters) - the squad moves of each pass (their sum is /squads_moved)
    AlifeSquadTaskNativeJob,
    AlifeSquadTaskNativeSmart,
    AlifeSquadTaskNativeSquad,
    AlifeSquadTaskNativeSelf,
    AlifeSquadTaskLua, // unused since C2 (was mode 0)
    AlifeSquadTaskLuaActor,
    AlifeSquadTaskLuaOtherClass,
    AlifeSquadTaskLuaOdd,
    AlifeSquadTaskLuaNoScript,
    AlifeSquadTaskVerify, // unused since C2 (was mode 2)
    AlifeSquadTaskVerifyMismatch, // unused since C2 (was mode 2)
    AlifeSquadUpdateMembers,
    AlifeOfflinePathFirst,
    AlifeOfflinePathIdle,
    AlifeOfflinePathSearch,
    AlifeOfflinePathSearchFailed,
    AlifeOfflinePathFollow,
    AlifeOfflinePathVertexChange,
    AlifeOfflinePathVertexHook,
    AlifeTimeSkipMoveSquad,
    AlifeTimeSkipMoveOther,
    AlifeTimeSkipMoveSquadMoves,
    AlifeTimeSkipRetargetMoves,
    // plans/lua_to_cpp/57-gw-eoc-next-steps.md, L2 step D (counters, measurement only): the binder interval
    // (CGameObject::binder_interval_due, game_object:set_binder_interval: idle restrictors, camps without a story).
    // skipped - the Lua binder updates not called (the interval has not elapsed); called - the due calls of the
    // binders with an interval. The skipped share = skipped / (skipped + called)
    BinderIntervalSkipped,
    BinderIntervalCalled,
    // 63-wave3-fixes.md S3/S4 (renamed from 61-wave3 E6 "alife/offline_path/dup_scan*"): CALifeGraphRegistry keeps
    // an id -> vertex index (at most one registration per id, fd6cecd15 / #2058) and every add/remove consults it in
    // O(1); the all-vertices sweeps of the wave-3 code are gone. /graph/add - every registration of an object into a
    // vertex map (load, spawn, the offline switch of an inventory item, detach) - counts ALL adds, unlike the old
    // dup_scan which counted only the ones with a sweep; /graph/add_stale - among those, add found a stale
    // registration (the index pointed at another vertex, or a conflicting entry at the target) and dropped it - the
    // #2058 enforcement firing (expect 0 in a clean run); /graph/remove_desync - remove found the object not at its
    // claimed m_tGraphID but at another vertex (the #2058 desync, the "was at vertex ..." log line)
    AlifeGraphAdd,
    AlifeGraphAddStale,
    AlifeGraphRemoveDesync,
    // 61-wave3-spec.md, E7 (a counter): the game path search (actualize) put off to the next update of the object,
    // the search budget of the ALife update is spent (offline_path_search_budget_ms of alife.ltx)
    AlifeOfflinePathSearchDeferred,
    // 61-wave3-spec.md, P-fb (counters): the fallback map of the native condlist interpreter
    // (gw_condlist_native.cpp) - one counter per reason a call went back to pick_section_lua. fb_unknown /
    // fb_pattern / fb_effect / fb_param are the named structure kinds (the condition or effect name is in
    // _gw_internal.logic.stats()); fb_structure is the rest of the compile rules; fb_identity / fb_actions /
    // fb_types / fb_prng are the checks after the compile. All are counted per call alongside condlist/pick/fallback
    CondlistPickFbStructure,
    CondlistPickFbUnknown,
    CondlistPickFbPattern,
    CondlistPickFbEffect,
    CondlistPickFbParam,
    CondlistPickFbIdentity,
    CondlistPickFbActions,
    CondlistPickFbTypes,
    CondlistPickFbPrng,
    // plan 63 S1 (counter): the native calls that passed without any identity verification - the
    // once-per-frame global sweep (gw_condlist_native.cpp SweepIdentityBits) had already filled
    // g_okMask for their frame and the call only tested its bits. With the sweep working as
    // intended this counter is close to condlist/pick minus one sweep per frame
    CondlistPickIdentityCached,
    // plan 64 S1 (review2-condlist-lua.md SF-1, measurement only): sub-stages of condlist/pick/prepare
    // and condlist/pick/eval, to see where the rest of /prepare goes once the identity sweep is
    // incremental. /prepare/identity - SweepIdentityBits plus the funcMask test (gw_condlist_native.cpp
    // LuaPickSection); /prepare/rest - everything else timed under /prepare (argument/type marshalling,
    // the W4-2.2/W4-2.3 state chains, the zone stash, the PRNG stock check); /eval/actions_check - the
    // xr_actions[section] cache lookup for the matched section, now done after evaluation (SF-1, "as
    // Lua does") so it falls inside /eval rather than /prepare
    CondlistPickPrepareIdentity,
    CondlistPickPrepareRest,
    CondlistPickEvalActions,
    Count
};

namespace npc_cpp_profile
{
bool enabled();
void add(ENpcCppProfileStage stage, u64 qpc_delta);
// A pure counter for a stage: how many times something happened, with no per-call timing. Use this instead of
// add() when there is nothing to time (add() would record a zero-length sample, which flush_snapshots keeps,
// but at the cost of a meaningless avg/max of zero on every such row). Never flushes the periodic report:
// count() runs inside the QPC interval of a timed stage (the graph registry counters sit inside
// alife/offline_path/vertex_change) and a flush there printed the whole 5 s report into that sample and skewed
// its max (63-wave3-fixes.md S4). add()/add_many() keep flushing - their own sample is already recorded by then
void count(ENpcCppProfileStage stage, u64 n = 1);
// add() of a batch gathered by the caller: calls samples with the sum total_qpc and the longest one max_qpc
void add_many(ENpcCppProfileStage stage, u64 total_qpc, u64 calls, u64 max_qpc);
void add_script_evaluator(pcstr evaluator_name, u64 qpc_delta);
void add_script_evaluator_cache_hit(pcstr evaluator_name);
void add_script_evaluator_cache_miss(pcstr evaluator_name);
// A step of a Lua action ("initialize", "execute", "finalize") by the name of the action: script_action_top
void add_script_action(pcstr action_name, pcstr step, u64 qpc_delta);
void flush_if_needed();
// plans/lua_to_cpp/57-gw-eoc-next-steps.md, E2: the update of a smart zone binder by the section of the zone
// (binder_section_top of the report)
void add_binder_section(pcstr section, u64 qpc_delta);
// plans/lua_to_cpp/57-gw-eoc-next-steps.md, E2, "alife peak" (only with -npc_cpp_profile): alife/update opens a capture
// on its thread (peak_begin); while it is open every timed stage of that thread (add / add_many), every plugin stage
// (peak_add_plugin, addon_api_console.cpp) and every scheduled object (peak_object, alife_schedule_registry.cpp) is
// summed into it as well. peak_end closes it and, when the update took longer than -alife_peak_ms N (5 ms by
// default), prints one line "- alife peak: ..." with the parts of the update, the most expensive object and the top 5
// stages. One capture at a time (the ALife thread); without the profiler none of this runs.
bool peak_active();
void peak_begin();
void peak_object(u16 id, pcstr name, pcstr section, ENpcCppProfileStage kind, u64 qpc_delta);
void peak_add_plugin(pcstr name, u64 qpc_delta);
void peak_end(pcstr where, u64 total_qpc);
}

class ScopedNpcCppProfile
{
public:
    explicit ScopedNpcCppProfile(ENpcCppProfileStage stage);
    ~ScopedNpcCppProfile();

private:
    ENpcCppProfileStage m_stage;
    u64 m_start_qpc;
    bool m_enabled;
};

#define NPC_CPP_PROFILE_SCOPE(stage) \
    ScopedNpcCppProfile CONCATENIZE(__npc_cpp_profile_scope_, __LINE__)(stage)
