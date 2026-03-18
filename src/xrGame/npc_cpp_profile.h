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
    Count
};

namespace npc_cpp_profile
{
bool enabled();
void add(ENpcCppProfileStage stage, u64 qpc_delta);
void add_script_evaluator(pcstr evaluator_name, u64 qpc_delta);
void add_script_evaluator_cache_hit(pcstr evaluator_name);
void add_script_evaluator_cache_miss(pcstr evaluator_name);
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
