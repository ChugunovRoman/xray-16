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
    CustomMonsterUpdateCLProcessSoundCallbacks,
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
