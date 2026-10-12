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
    Count
};

namespace npc_cpp_profile
{
bool enabled();
void add(ENpcCppProfileStage stage, u64 qpc_delta);
// A pure counter for a stage: how many times something happened, with no per-call timing. Use this instead of
// add() when there is nothing to time (add() would record a zero-length sample, which flush_snapshots keeps,
// but at the cost of a meaningless avg/max of zero on every such row).
void count(ENpcCppProfileStage stage, u64 n = 1);
void add_script_evaluator(pcstr evaluator_name, u64 qpc_delta);
void add_script_evaluator_cache_hit(pcstr evaluator_name);
void add_script_evaluator_cache_miss(pcstr evaluator_name);
// A step of a Lua action ("initialize", "execute", "finalize") by the name of the action: script_action_top
void add_script_action(pcstr action_name, pcstr step, u64 qpc_delta);
void flush_if_needed();
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
