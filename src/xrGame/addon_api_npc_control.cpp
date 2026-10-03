#include "StdAfx.h"

// Plugin API, groups npc_body, npc_sight, npc_anim, inventory, smart_cover: what a native plugin needs to drive a
// stalker the way the Lua state manager does (plans/lua_to_cpp/09-stage-d-native-npc.md, wave W1).
// Every function calls the same CScriptGameObject method as the Lua call it replaces, so the engine does exactly
// what it does for Lua, internal checks included. Before the call the host checks what the Lua method would only
// log: the thread, the class of the object, the enum value, a missing target object. A wrong call then returns the
// documented default or GWP_ERROR_INVALID_ARGUMENT, with no script error in the log.
// Docs: wiki/doc/plugins/api/npc_body.md, npc_sight.md, npc_anim.md, inventory.md, smart_cover.md

#include "addon_api_npc_control.h"
#include "addon_event_bus.h"
#include "addon_host.h"

#include "ai_monster_space.h"
#include "detail_path_manager_space.h"
#include "game_location_selector.h"
#include "movement_manager_space.h"
#include "sight_manager_space.h"
#include "alife_space.h"
#include "ai_space.h"
#include "xrAICore/Navigation/level_graph.h"
#include "CustomMonster.h"
#include "InventoryOwner.h"
#include "Weapon.h"
#include "Missile.h"
#include "ai/stalker/ai_stalker.h"
#include "stalker_movement_manager_smart_cover.h"
#include "cover_manager.h"
#include "smart_cover.h"
#include "smart_cover_description.h"
#include "movement_manager.h"
#include "restricted_object.h"
#include "stalker_animation_manager.h"
#include "Level.h"
#include "script_game_object.h"
#include "script_hit.h"
#include "Include/xrRender/Kinematics.h"
#include "xrAICore/Navigation/PatrolPath/patrol_path.h"
#include "xrAICore/Navigation/PatrolPath/patrol_path_storage.h"
#include "patrol_path_manager.h"
#include "ai/stalker/ai_stalker_space.h"

#include <cmath>

// The constants of gwp_api.h are the engine's own numbers (and so the numbers of the Lua tables move.*, anim.*,
// look.*, object.*, hit.*, CSightParams.*): a plugin passes them through unchanged. Checked here, at compile time.
static_assert(GWP_MOVEMENT_WALK == MonsterSpace::eMovementTypeWalk);
static_assert(GWP_MOVEMENT_RUN == MonsterSpace::eMovementTypeRun);
static_assert(GWP_MOVEMENT_STAND == MonsterSpace::eMovementTypeStand);
static_assert(GWP_BODY_CROUCH == MonsterSpace::eBodyStateCrouch);
static_assert(GWP_BODY_STAND == MonsterSpace::eBodyStateStand);
static_assert(GWP_MENTAL_DANGER == MonsterSpace::eMentalStateDanger);
static_assert(GWP_MENTAL_FREE == MonsterSpace::eMentalStateFree);
static_assert(GWP_MENTAL_PANIC == MonsterSpace::eMentalStatePanic);
static_assert(GWP_PATH_GAME == MovementManager::ePathTypeGamePath);
static_assert(GWP_PATH_LEVEL == MovementManager::ePathTypeLevelPath);
static_assert(GWP_PATH_PATROL == MovementManager::ePathTypePatrolPath);
static_assert(GWP_PATH_NONE == MovementManager::ePathTypeNoPath);
static_assert(GWP_DETAIL_PATH_SMOOTH == DetailPathManager::eDetailPathTypeSmooth);
static_assert(GWP_DETAIL_PATH_SMOOTH_DODGE == DetailPathManager::eDetailPathTypeSmoothDodge);
static_assert(GWP_DETAIL_PATH_SMOOTH_CRITERIA == DetailPathManager::eDetailPathTypeSmoothCriteria);
static_assert(GWP_ALIFE_MOVEMENT_MASK == eSelectionTypeMask);
static_assert(GWP_ALIFE_MOVEMENT_RANDOM == eSelectionTypeRandomBranching);
static_assert(GWP_PATROL_START_FIRST == ePatrolStartTypeFirst);
static_assert(GWP_PATROL_START_LAST == ePatrolStartTypeLast);
static_assert(GWP_PATROL_START_NEAREST == ePatrolStartTypeNearest);
static_assert(GWP_PATROL_START_POINT == ePatrolStartTypePoint);
static_assert(GWP_PATROL_START_NEXT == ePatrolStartTypeNext);
static_assert(GWP_PATROL_ROUTE_STOP == ePatrolRouteTypeStop);
static_assert(GWP_PATROL_ROUTE_CONTINUE == ePatrolRouteTypeContinue);
static_assert(GWP_STALKER_SOUND_ALARM == StalkerSpace::eStalkerSoundAlarm);
static_assert(GWP_SIGHT_CURRENT_DIRECTION == SightManager::eSightTypeCurrentDirection);
static_assert(GWP_SIGHT_PATH_DIRECTION == SightManager::eSightTypePathDirection);
static_assert(GWP_SIGHT_DIRECTION == SightManager::eSightTypeDirection);
static_assert(GWP_SIGHT_POSITION == SightManager::eSightTypePosition);
static_assert(GWP_SIGHT_OBJECT == SightManager::eSightTypeObject);
static_assert(GWP_SIGHT_COVER == SightManager::eSightTypeCover);
static_assert(GWP_SIGHT_SEARCH == SightManager::eSightTypeSearch);
static_assert(GWP_SIGHT_LOOK_OVER == SightManager::eSightTypeLookOver);
static_assert(GWP_SIGHT_COVER_LOOK_OVER == SightManager::eSightTypeCoverLookOver);
static_assert(GWP_SIGHT_FIRE_OBJECT == SightManager::eSightTypeFireObject);
static_assert(GWP_SIGHT_FIRE_POSITION == SightManager::eSightTypeFirePosition);
static_assert(GWP_SIGHT_ANIMATION_DIRECTION == SightManager::eSightTypeAnimationDirection);
static_assert(GWP_SIGHT_DUMMY == SightManager::eSightTypeDummy);
static_assert(GWP_OBJECT_SWITCH1 == MonsterSpace::eObjectActionSwitch1);
static_assert(GWP_OBJECT_SWITCH2 == MonsterSpace::eObjectActionSwitch2);
static_assert(GWP_OBJECT_RELOAD1 == MonsterSpace::eObjectActionReload1);
static_assert(GWP_OBJECT_RELOAD2 == MonsterSpace::eObjectActionReload2);
static_assert(GWP_OBJECT_AIM1 == MonsterSpace::eObjectActionAim1);
static_assert(GWP_OBJECT_AIM2 == MonsterSpace::eObjectActionAim2);
static_assert(GWP_OBJECT_FIRE1 == MonsterSpace::eObjectActionFire1);
static_assert(GWP_OBJECT_FIRE2 == MonsterSpace::eObjectActionFire2);
static_assert(GWP_OBJECT_IDLE == MonsterSpace::eObjectActionIdle);
static_assert(GWP_OBJECT_STRAP == MonsterSpace::eObjectActionStrapped);
static_assert(GWP_OBJECT_DROP == MonsterSpace::eObjectActionDrop);
static_assert(GWP_OBJECT_ACTIVATE == MonsterSpace::eObjectActionActivate);
static_assert(GWP_OBJECT_DEACTIVATE == MonsterSpace::eObjectActionDeactivate);
static_assert(GWP_OBJECT_USE == MonsterSpace::eObjectActionUse);
static_assert(GWP_OBJECT_TURN_ON == MonsterSpace::eObjectActionTurnOn);
static_assert(GWP_OBJECT_TURN_OFF == MonsterSpace::eObjectActionTurnOff);
static_assert(GWP_OBJECT_SHOW == MonsterSpace::eObjectActionShow);
static_assert(GWP_OBJECT_HIDE == MonsterSpace::eObjectActionHide);
static_assert(GWP_OBJECT_TAKE == MonsterSpace::eObjectActionTake);
static_assert(GWP_HIT_BURN == ALife::eHitTypeBurn);
static_assert(GWP_HIT_SHOCK == ALife::eHitTypeShock);
static_assert(GWP_HIT_CHEMICAL_BURN == ALife::eHitTypeChemicalBurn);
static_assert(GWP_HIT_RADIATION == ALife::eHitTypeRadiation);
static_assert(GWP_HIT_TELEPATIC == ALife::eHitTypeTelepatic);
static_assert(GWP_HIT_WOUND == ALife::eHitTypeWound);
static_assert(GWP_HIT_FIRE_WOUND == ALife::eHitTypeFireWound);
static_assert(GWP_HIT_STRIKE == ALife::eHitTypeStrike);
static_assert(GWP_HIT_EXPLOSION == ALife::eHitTypeExplosion);
static_assert(GWP_HIT_WOUND_2 == ALife::eHitTypeWound_2);
static_assert(GWP_HIT_LIGHT_BURN == ALife::eHitTypeLightBurn);
static_assert(GWP_HIT_PHYSIC_STRIKE == ALife::eHitTypePhysicStrike);
static_assert(GWP_HIT_PHYSIC_STRIKE + 1 == ALife::eHitTypeMax); // a new hit type needs a constant

namespace gw::addons
{
namespace
{
// The names of the helpers differ from those of the other addon_api_*.cpp files: a unity build puts several of
// these files into one translation unit, and their anonymous namespaces merge there.
bool CheckControlCall(pcstr group, pcstr function)
{
    if (IsMainThread())
        return true;
    Msg("! [%s] %s called outside the main thread, ignored", group, function);
    return false;
}

CGameObject* FindControlObject(GwpObjectId id)
{
    if (!g_pGameLevel || id == GWP_INVALID_OBJECT_ID)
        return nullptr;
    CGameObject* object = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
    return object && !object->getDestroy() ? object : nullptr;
}

// An online stalker, the class almost every method here requires (the Lua method logs a script error for any
// other object and does nothing).
CAI_Stalker* FindStalker(GwpObjectId id, pcstr group, pcstr function)
{
    if (!CheckControlCall(group, function))
        return nullptr;
    return smart_cast<CAI_Stalker*>(FindControlObject(id));
}

// A stalker or a monster (is_body_turning, movement_enabled).
CCustomMonster* FindMonster(GwpObjectId id, pcstr group, pcstr function)
{
    if (!CheckControlCall(group, function))
        return nullptr;
    return smart_cast<CCustomMonster*>(FindControlObject(id));
}

// The Lua view of a second object (an item, a target), or nullptr for a missing one: several Lua methods
// dereference that argument without a check, so the host must not pass nullptr where Lua would crash.
CScriptGameObject* FindArgument(GwpObjectId id)
{
    CGameObject* object = FindControlObject(id);
    return object ? object->lua_game_object() : nullptr;
}

// Not ToVector: addon_api_level.cpp has one in the same namespace, a unity build would see both
Fvector ControlVector(const float xyz[3]) { return Fvector().set(xyz[0], xyz[1], xyz[2]); }

// A vector of a plugin with no NaN or infinity: such a value would reach the yaw of the head, the XFORM of the
// object or the path builder (a VERIFY in a debug build, garbage in release)
bool Finite3(const float xyz[3]) { return std::isfinite(xyz[0]) && std::isfinite(xyz[1]) && std::isfinite(xyz[2]); }

// ---------------------------------------------------------------------------------------------
// Plugin API (group npc_body)
// ---------------------------------------------------------------------------------------------

constexpr pcstr kBody = "npc_body";

GwpResult GWP_CALL ApiNpcSetMovementType(GwpObjectId id, uint32_t movement_type)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_set_movement_type");
    if (!stalker || movement_type > GWP_MOVEMENT_STAND)
        return IsMainThread() ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_MAIN_THREAD;
    stalker->lua_game_object()->set_movement_type(static_cast<MonsterSpace::EMovementType>(movement_type));
    return GWP_OK;
}

GwpResult GWP_CALL ApiNpcSetBodyState(GwpObjectId id, uint32_t body_state)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_set_body_state");
    // Another value is a THROW inside the Lua method: refused here
    if (!stalker || (body_state != GWP_BODY_CROUCH && body_state != GWP_BODY_STAND))
        return IsMainThread() ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_MAIN_THREAD;
    stalker->lua_game_object()->set_body_state(static_cast<MonsterSpace::EBodyState>(body_state));
    return GWP_OK;
}

GwpResult GWP_CALL ApiNpcSetMentalState(GwpObjectId id, uint32_t mental_state)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_set_mental_state");
    if (!stalker || mental_state > GWP_MENTAL_PANIC)
        return IsMainThread() ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_MAIN_THREAD;
    stalker->lua_game_object()->set_mental_state(static_cast<MonsterSpace::EMentalState>(mental_state));
    return GWP_OK;
}

GwpResult GWP_CALL ApiNpcSetPathType(GwpObjectId id, uint32_t path_type)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_set_path_type");
    if (!stalker || path_type > GWP_PATH_NONE)
        return IsMainThread() ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_MAIN_THREAD;
    stalker->lua_game_object()->set_path_type(static_cast<MovementManager::EPathType>(path_type));
    return GWP_OK;
}

// The getters return what the Lua method returns for a wrong object, without its script error.
uint32_t GWP_CALL ApiNpcMovementType(GwpObjectId id)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_movement_type");
    return stalker ? static_cast<uint32_t>(stalker->lua_game_object()->movement_type()) : GWP_MOVEMENT_STAND;
}

uint32_t GWP_CALL ApiNpcTargetMovementType(GwpObjectId id)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_target_movement_type");
    return stalker ? static_cast<uint32_t>(stalker->lua_game_object()->target_movement_type()) : GWP_MOVEMENT_STAND;
}

uint32_t GWP_CALL ApiNpcTargetBodyState(GwpObjectId id)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_target_body_state");
    return stalker ? static_cast<uint32_t>(stalker->lua_game_object()->target_body_state()) : GWP_BODY_STAND;
}

uint32_t GWP_CALL ApiNpcTargetMentalState(GwpObjectId id)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_target_mental_state");
    return stalker ? static_cast<uint32_t>(stalker->lua_game_object()->target_mental_state()) : GWP_MENTAL_DANGER;
}

int GWP_CALL ApiNpcIsBodyTurning(GwpObjectId id)
{
    CCustomMonster* monster = FindMonster(id, kBody, "npc_is_body_turning");
    return monster && monster->lua_game_object()->is_body_turning() ? 1 : 0;
}

int GWP_CALL ApiNpcMovementEnabled(GwpObjectId id)
{
    CCustomMonster* monster = FindMonster(id, kBody, "npc_movement_enabled");
    return monster && monster->lua_game_object()->movement_enabled() ? 1 : 0;
}

GwpResult GWP_CALL ApiNpcEnableMovement(GwpObjectId id, int enable)
{
    CCustomMonster* monster = FindMonster(id, kBody, "npc_enable_movement");
    if (!monster)
        return IsMainThread() ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_MAIN_THREAD;
    monster->lua_game_object()->enable_movement(enable != 0);
    return GWP_OK;
}

int GWP_CALL ApiNpcSpecialDangerMove(GwpObjectId id)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_special_danger_move");
    return stalker && stalker->lua_game_object()->special_danger_move() ? 1 : 0;
}

GwpResult GWP_CALL ApiNpcSetSpecialDangerMove(GwpObjectId id, int value)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_set_special_danger_move");
    if (!stalker)
        return IsMainThread() ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_MAIN_THREAD;
    stalker->lua_game_object()->special_danger_move(value != 0);
    return GWP_OK;
}

GwpResult GWP_CALL ApiNpcInactualizePatrolPath(GwpObjectId id)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_inactualize_patrol_path");
    if (!stalker)
        return IsMainThread() ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_MAIN_THREAD;
    stalker->lua_game_object()->inactualize_patrol_path();
    return GWP_OK;
}

int GWP_CALL ApiNpcAccessible(GwpObjectId id, uint32_t level_vertex)
{
    CCustomMonster* monster = FindMonster(id, kBody, "npc_accessible");
    // accessible_vertex_id checks the vertex as well, but not that the level has an AI graph at all
    const CLevelGraph* graph = monster ? ai().get_level_graph() : nullptr;
    if (!graph || !graph->valid_vertex_id(level_vertex))
        return 0;
    return monster->lua_game_object()->accessible_vertex_id(level_vertex) ? 1 : 0;
}

// The path of a stalker (the movement setters of the scheme actions). The checks are the ones the Lua methods
// only log, plus the ones Lua would crash on (set_movement_selection_type of a non-stalker, a NULL position).

GwpResult Refused() { return IsMainThread() ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_MAIN_THREAD; }

GwpResult GWP_CALL ApiNpcSetDestLevelVertex(GwpObjectId id, uint32_t level_vertex)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_set_dest_level_vertex");
    // set_dest_level_vertex_id returns without a change for an invalid vertex and for one the restrictors forbid
    const CLevelGraph* graph = stalker ? ai().get_level_graph() : nullptr;
    if (!graph || !graph->valid_vertex_id(level_vertex) || !stalker->movement().restrictions().accessible(level_vertex))
        return Refused();
    stalker->lua_game_object()->set_dest_level_vertex_id(level_vertex);
    return GWP_OK;
}

GwpResult GWP_CALL ApiNpcSetDestGameVertex(GwpObjectId id, uint32_t game_vertex)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_set_dest_game_vertex");
    if (!stalker || game_vertex > u32(u16(-1)) || !ai().get_game_graph() ||
        !ai().game_graph().valid_vertex_id(static_cast<GameGraph::_GRAPH_ID>(game_vertex)))
        return Refused();
    stalker->lua_game_object()->set_dest_game_vertex_id(static_cast<GameGraph::_GRAPH_ID>(game_vertex));
    return GWP_OK;
}

GwpResult GWP_CALL ApiNpcSetDesiredPosition(GwpObjectId id, const float* xyz)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_set_desired_position");
    if (!stalker)
        return Refused();
    if (!xyz)
    {
        stalker->lua_game_object()->set_desired_position(); // the overload without a position: clears
        return GWP_OK;
    }
    if (!Finite3(xyz))
        return GWP_ERROR_INVALID_ARGUMENT;
    const Fvector position = ControlVector(xyz);
    stalker->lua_game_object()->set_desired_position(&position);
    return GWP_OK;
}

GwpResult GWP_CALL ApiNpcSetDesiredDirection(GwpObjectId id, const float* dir)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_set_desired_direction");
    if (!stalker)
        return Refused();
    if (!dir)
    {
        stalker->lua_game_object()->set_desired_direction();
        return GWP_OK;
    }
    Fvector direction = ControlVector(dir);
    if (!std::isfinite(direction.x) || !std::isfinite(direction.y) || !std::isfinite(direction.z) ||
        fsimilar(direction.magnitude(), 0.f))
        return Refused(); // Lua logs "you passed zero direction" and still sets it: refused here on purpose
    // The engine method normalizes on its own (outside the SoC mode) and logs a non-unit vector first: only such a
    // vector is normalized here, to spare the log - a unit one goes through untouched, one normalization as in Lua
    if (!ShadowOfChernobylMode && !fsimilar(direction.magnitude(), 1.f))
        direction.normalize_safe();
    stalker->lua_game_object()->set_desired_direction(&direction);
    return GWP_OK;
}

GwpResult GWP_CALL ApiNpcSetDetailPathType(GwpObjectId id, uint32_t detail_path_type)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_set_detail_path_type");
    if (!stalker || detail_path_type > GWP_DETAIL_PATH_SMOOTH_CRITERIA)
        return Refused();
    stalker->lua_game_object()->set_detail_path_type(
        static_cast<DetailPathManager::EDetailPathType>(detail_path_type));
    return GWP_OK;
}

GwpResult GWP_CALL ApiNpcSetMovementSelectionType(GwpObjectId id, uint32_t selection_type)
{
    // The Lua method dereferences the stalker even after its "cannot access" log: refused here for anything else
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_set_movement_selection_type");
    if (!stalker || selection_type > GWP_ALIFE_MOVEMENT_RANDOM)
        return Refused();
    stalker->lua_game_object()->set_movement_selection_type(static_cast<ESelectionType>(selection_type));
    return GWP_OK;
}

uint32_t GWP_CALL ApiNpcPathType(GwpObjectId id)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_path_type");
    return stalker ? static_cast<uint32_t>(stalker->lua_game_object()->path_type()) : GWP_PATH_NONE;
}

uint32_t GWP_CALL ApiNpcDetailPathType(GwpObjectId id)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_detail_path_type");
    // The movement manager itself: CScriptGameObject::detail_path_type always answers eDetailPathTypeSmooth
    return stalker ? static_cast<uint32_t>(stalker->movement().detail_path_type()) : GWP_DETAIL_PATH_SMOOTH;
}

uint32_t GWP_CALL ApiNpcTargetPathType(GwpObjectId id)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_target_path_type");
    return stalker ? static_cast<uint32_t>(stalker->movement().target_params().m_path_type) : GWP_PATH_NONE;
}

uint32_t GWP_CALL ApiNpcTargetDetailPathType(GwpObjectId id)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_target_detail_path_type");
    return stalker ? static_cast<uint32_t>(stalker->movement().target_params().m_detail_path_type) :
                     GWP_DETAIL_PATH_SMOOTH;
}

uint32_t GWP_CALL ApiNpcMentalState(GwpObjectId id)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_mental_state");
    return stalker ? static_cast<uint32_t>(stalker->lua_game_object()->mental_state()) : GWP_MENTAL_DANGER;
}

int GWP_CALL ApiNpcAccessiblePosition(GwpObjectId id, const float xyz[3])
{
    CCustomMonster* monster = FindMonster(id, kBody, "npc_accessible_position");
    if (!monster || !xyz || !Finite3(xyz))
        return 0;
    return monster->lua_game_object()->accessible_position(ControlVector(xyz)) ? 1 : 0;
}

uint32_t GWP_CALL ApiNpcAccessibleNearest(GwpObjectId id, const float xyz[3], float out_xyz[3])
{
    CCustomMonster* monster = FindMonster(id, kBody, "npc_accessible_nearest");
    if (!monster || !xyz || !out_xyz || !Finite3(xyz))
        return GWP_INVALID_LEVEL_VERTEX;
    const Fvector position = ControlVector(xyz);
    // Lua logs "already accessible" and answers u32(-1) for an allowed position: the same answer, no log
    if (monster->movement().restrictions().accessible(position))
        return GWP_INVALID_LEVEL_VERTEX;
    Fvector result{};
    const u32 vertex = monster->lua_game_object()->accessible_nearest(position, result);
    if (vertex == u32(-1))
        return GWP_INVALID_LEVEL_VERTEX;
    out_xyz[0] = result.x;
    out_xyz[1] = result.y;
    out_xyz[2] = result.z;
    return vertex;
}

uint32_t GWP_CALL ApiNpcLocationOnPath(GwpObjectId id, float distance, float out_xyz[3])
{
    CCustomMonster* monster = FindMonster(id, kBody, "npc_location_on_path");
    if (!monster || !out_xyz || !std::isfinite(distance))
        return GWP_INVALID_LEVEL_VERTEX;
    Fvector location{};
    const u32 vertex = monster->lua_game_object()->location_on_path(distance, &location);
    out_xyz[0] = location.x;
    out_xyz[1] = location.y;
    out_xyz[2] = location.z;
    return vertex == u32(-1) ? GWP_INVALID_LEVEL_VERTEX : vertex;
}

uint32_t GWP_CALL ApiNpcDestLevelVertex(GwpObjectId id)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_dest_level_vertex");
    if (!stalker)
        return GWP_INVALID_LEVEL_VERTEX;
    const u32 vertex = stalker->movement().level_dest_vertex_id();
    return vertex == u32(-1) ? GWP_INVALID_LEVEL_VERTEX : vertex;
}

uint32_t GWP_CALL ApiNpcDestGameVertex(GwpObjectId id)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_dest_game_vertex");
    if (!stalker)
        return GWP_INVALID_GAME_VERTEX;
    const GameGraph::_GRAPH_ID vertex = stalker->movement().game_dest_vertex_id();
    return vertex == GameGraph::_GRAPH_ID(-1) ? GWP_INVALID_GAME_VERTEX : static_cast<uint32_t>(vertex);
}

int GWP_CALL ApiNpcPathCompleted(GwpObjectId id)
{
    CCustomMonster* monster = FindMonster(id, kBody, "npc_path_completed");
    return monster && monster->lua_game_object()->path_completed() ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group patrol): the patrol paths of the level. A handle is an interned path name, resolved on every
// call: the storage of the paths is rebuilt with every level, a pointer kept across it would dangle.
// ---------------------------------------------------------------------------------------------

constexpr pcstr kPatrol = "patrol";

struct PatrolNames
{
    xr_vector<shared_str> names; // handle - 1 -> name
    xr_map<shared_str, GwpPatrol> handles;
};

PatrolNames& Patrols()
{
    static PatrolNames patrols;
    return patrols;
}

// The path of the current level for a handle, nullptr for none (no level, no path of that name now)
const CPatrolPath* PatrolOf(GwpPatrol handle, pcstr function)
{
    if (!CheckControlCall(kPatrol, function) || handle == GWP_INVALID_PATROL || !g_pGameLevel)
        return nullptr;
    const PatrolNames& patrols = Patrols();
    if (handle > patrols.names.size())
        return nullptr;
    return ai().patrol_paths().path(patrols.names[handle - 1], true);
}

// A point of the path: nullptr out of the path (Lua logs the bad index or asserts on it)
const CPatrolPath::CVertex* PatrolVertexOf(GwpPatrol handle, uint32_t point, pcstr function)
{
    const CPatrolPath* path = PatrolOf(handle, function);
    return path ? path->vertex(point) : nullptr;
}

// Copies a name into a caller buffer (cut, zero-terminated) and returns its full length
uint32_t CopyName(pcstr name, char* out, uint32_t cap)
{
    const size_t length = name ? xr_strlen(name) : 0;
    if (out && cap)
    {
        const size_t copied = std::min<size_t>(length, cap - 1);
        if (copied)
            std::memcpy(out, name, copied);
        out[copied] = 0;
    }
    return static_cast<uint32_t>(length);
}

GwpPatrol GWP_CALL ApiPatrolFind(const char* name)
{
    if (!CheckControlCall(kPatrol, "patrol_find") || !name || !name[0] || !g_pGameLevel)
        return GWP_INVALID_PATROL;
    const shared_str key(name);
    if (!ai().patrol_paths().path(key, true))
        return GWP_INVALID_PATROL; // the level has no such path: the same answer as patrol(name) asserting
    PatrolNames& patrols = Patrols();
    const auto found = patrols.handles.find(key);
    if (found != patrols.handles.end())
        return found->second;
    patrols.names.push_back(key);
    const GwpPatrol handle = static_cast<GwpPatrol>(patrols.names.size());
    patrols.handles.emplace(key, handle);
    return handle;
}

uint32_t GWP_CALL ApiPatrolPointCount(GwpPatrol path)
{
    const CPatrolPath* patrol = PatrolOf(path, "patrol_point_count");
    return patrol ? static_cast<uint32_t>(patrol->vertices().size()) : 0;
}

int GWP_CALL ApiPatrolPointPosition(GwpPatrol path, uint32_t point, float out_xyz[3])
{
    const CPatrolPath::CVertex* vertex = PatrolVertexOf(path, point, "patrol_point_position");
    if (!vertex || !out_xyz)
        return 0;
    const Fvector& position = vertex->data().position();
    out_xyz[0] = position.x;
    out_xyz[1] = position.y;
    out_xyz[2] = position.z;
    return 1;
}

uint32_t GWP_CALL ApiPatrolPointLevelVertex(GwpPatrol path, uint32_t point)
{
    const CPatrolPath::CVertex* vertex = PatrolVertexOf(path, point, "patrol_point_level_vertex");
    return vertex ? vertex->data().level_vertex_id() : GWP_INVALID_LEVEL_VERTEX;
}

uint32_t GWP_CALL ApiPatrolPointGameVertex(GwpPatrol path, uint32_t point)
{
    const CPatrolPath::CVertex* vertex = PatrolVertexOf(path, point, "patrol_point_game_vertex");
    if (!vertex)
        return GWP_INVALID_GAME_VERTEX;
    const GameGraph::_GRAPH_ID game_vertex = vertex->data().game_vertex_id();
    return game_vertex == GameGraph::_GRAPH_ID(-1) ? GWP_INVALID_GAME_VERTEX : static_cast<uint32_t>(game_vertex);
}

uint32_t GWP_CALL ApiPatrolPointFlags(GwpPatrol path, uint32_t point)
{
    const CPatrolPath::CVertex* vertex = PatrolVertexOf(path, point, "patrol_point_flags");
    return vertex ? static_cast<uint32_t>(vertex->data().flags()) : 0;
}

int GWP_CALL ApiPatrolPointTerminal(GwpPatrol path, uint32_t point)
{
    const CPatrolPath::CVertex* vertex = PatrolVertexOf(path, point, "patrol_point_terminal");
    return vertex && vertex->edges().empty() ? 1 : 0;
}

uint32_t GWP_CALL ApiPatrolPointName(GwpPatrol path, uint32_t point, char* out, uint32_t cap)
{
    const CPatrolPath::CVertex* vertex = PatrolVertexOf(path, point, "patrol_point_name");
    if (!vertex)
        return CopyName(nullptr, out, cap);
    return CopyName(vertex->data().name().c_str(), out, cap);
}

uint32_t GWP_CALL ApiPatrolPointIndex(GwpPatrol path, const char* point_name)
{
    const CPatrolPath* patrol = PatrolOf(path, "patrol_point_index");
    if (!patrol || !point_name)
        return GWP_INVALID_PATROL_POINT;
    const CPatrolPath::CVertex* vertex = patrol->point(shared_str(point_name));
    return vertex ? vertex->vertex_id() : GWP_INVALID_PATROL_POINT;
}

uint32_t GWP_CALL ApiPatrolNearestPoint(GwpPatrol path, const float xyz[3])
{
    const CPatrolPath* patrol = PatrolOf(path, "patrol_nearest_point");
    if (!patrol || !xyz || patrol->vertices().empty())
        return GWP_INVALID_PATROL_POINT;
    const CPatrolPath::CVertex* vertex = patrol->point(ControlVector(xyz));
    return vertex ? vertex->vertex_id() : GWP_INVALID_PATROL_POINT;
}

// The camper part (group level and npc_body): cover data of the AI graph, the current pose, the sound player, the
// sniper update rate

// level.high_cover_in_direction / low_cover_in_direction: the yaw of the direction, the cover of the vertex there
float CoverInDirection(uint32_t level_vertex, const float dir[3], bool high, pcstr function)
{
    if (!CheckControlCall("level", function) || !dir)
        return -1.f;
    const CLevelGraph* graph = ai().get_level_graph();
    const Fvector direction = ControlVector(dir);
    if (!graph || !graph->valid_vertex_id(level_vertex))
        return -1.f;
    // A zero direction is not refused: getHP gives yaw 0 for it, as for Lua (level_script.cpp)
    float yaw, pitch;
    direction.getHP(yaw, pitch);
    return high ? graph->high_cover_in_direction(yaw, level_vertex) : graph->low_cover_in_direction(yaw, level_vertex);
}

float GWP_CALL ApiLevelHighCoverInDirection(uint32_t level_vertex, const float dir[3])
{
    return CoverInDirection(level_vertex, dir, true, "level_high_cover_in_direction");
}

float GWP_CALL ApiLevelLowCoverInDirection(uint32_t level_vertex, const float dir[3])
{
    return CoverInDirection(level_vertex, dir, false, "level_low_cover_in_direction");
}

uint32_t GWP_CALL ApiNpcBodyState(GwpObjectId id)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_body_state");
    return stalker ? static_cast<uint32_t>(stalker->lua_game_object()->body_state()) : GWP_BODY_STAND;
}

GwpResult GWP_CALL ApiNpcPlaySound(GwpObjectId id, uint32_t sound_type, uint32_t max_start_ms, uint32_t min_start_ms,
    uint32_t max_stop_ms, uint32_t min_stop_ms)
{
    CCustomMonster* monster = FindMonster(id, kBody, "npc_play_sound");
    if (!monster)
        return Refused();
    // CSoundPlayer::play VERIFYs the intervals (a debug build stops there)
    if (max_start_ms < min_start_ms || max_stop_ms < min_stop_ms)
        return GWP_ERROR_INVALID_ARGUMENT;
    // CSoundPlayer::play checks the type itself (a type without sounds is a no-op, as in Lua)
    monster->lua_game_object()->play_sound(sound_type, max_start_ms, min_start_ms, max_stop_ms, min_stop_ms);
    return GWP_OK;
}

int GWP_CALL ApiNpcSniperUpdateRate(GwpObjectId id)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_sniper_update_rate");
    return stalker && stalker->lua_game_object()->sniper_update_rate() ? 1 : 0;
}

GwpResult GWP_CALL ApiNpcSetSniperUpdateRate(GwpObjectId id, int value)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_set_sniper_update_rate");
    if (!stalker)
        return Refused();
    stalker->lua_game_object()->sniper_update_rate(value != 0);
    return GWP_OK;
}

// The patrol path of an NPC (group npc_body)

GwpResult GWP_CALL ApiNpcSetPatrolPath(GwpObjectId id, const char* name, uint32_t start_type, uint32_t route_type,
    int random)
{
    CAI_Stalker* stalker = FindStalker(id, kBody, "npc_set_patrol_path");
    // patrol().set_path asserts on a name the level has no path of
    if (!stalker || !name || !name[0] || start_type > GWP_PATROL_START_NEXT || route_type > GWP_PATROL_ROUTE_CONTINUE ||
        !ai().patrol_paths().path(shared_str(name), true))
        return Refused();
    stalker->lua_game_object()->set_patrol_path(name, static_cast<EPatrolStartType>(start_type),
        static_cast<EPatrolRouteType>(route_type), random != 0);
    return GWP_OK;
}

GwpResult GWP_CALL ApiNpcSetStartPoint(GwpObjectId id, uint32_t point)
{
    CCustomMonster* monster = FindMonster(id, kBody, "npc_set_start_point");
    const CPatrolPath* path = monster ? monster->movement().patrol().get_path() : nullptr;
    if (!path || !path->vertex(point))
        return Refused(); // Lua logs "Path not specified" / the missing point
    monster->lua_game_object()->set_start_point(static_cast<int>(point));
    return GWP_OK;
}

uint32_t GWP_CALL ApiNpcPatrolPointIndex(GwpObjectId id)
{
    CCustomMonster* monster = FindMonster(id, kBody, "npc_patrol_point_index");
    if (!monster || !monster->movement().patrol().get_path())
        return GWP_INVALID_PATROL_POINT;
    const u32 point = monster->movement().patrol().get_current_point_index();
    return point == u32(-1) ? GWP_INVALID_PATROL_POINT : point;
}

uint32_t GWP_CALL ApiNpcPatrolPathName(GwpObjectId id, char* out, uint32_t cap)
{
    CCustomMonster* monster = FindMonster(id, kBody, "npc_patrol_path_name");
    // path_name() logs "Path not specified" without a path: asked only with one
    if (!monster || !monster->movement().patrol().get_path())
        return CopyName(nullptr, out, cap);
    return CopyName(monster->movement().patrol().path_name().c_str(), out, cap);
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group objects): the two object methods the state manager needs
// ---------------------------------------------------------------------------------------------

uint32_t GWP_CALL ApiObjectGameVertexId(GwpObjectId id)
{
    if (!CheckControlCall(kBody, "object_game_vertex_id"))
        return GWP_INVALID_GAME_VERTEX;
    CGameObject* object = FindControlObject(id);
    if (!object)
        return GWP_INVALID_GAME_VERTEX;
    const GameGraph::_GRAPH_ID vertex = object->ai_location().game_vertex_id();
    return vertex == GameGraph::_GRAPH_ID(-1) ? GWP_INVALID_GAME_VERTEX : static_cast<uint32_t>(vertex);
}

uint32_t GWP_CALL ApiObjectLevelVertexId(GwpObjectId id)
{
    if (!CheckControlCall("objects", "object_level_vertex_id"))
        return GWP_INVALID_LEVEL_VERTEX;
    CScriptGameObject* object = FindArgument(id);
    return object ? object->level_vertex_id() : GWP_INVALID_LEVEL_VERTEX;
}

GwpResult GWP_CALL ApiObjectHit(GwpObjectId id, GwpObjectId who, uint32_t hit_type, float power, float impulse,
    const float dir[3], const char* bone)
{
    if (!CheckControlCall("objects", "object_hit"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    CGameObject* object = FindControlObject(id);
    CScriptGameObject* draftsman = FindArgument(who);
    // Hit THROWs without the initiator, and resolves a bone name through the skeleton without checking there is one
    const bool has_bone = bone && bone[0];
    if (!object || !draftsman || !dir || !Finite3(dir) || !std::isfinite(power) || !std::isfinite(impulse) ||
        hit_type > GWP_HIT_PHYSIC_STRIKE || (has_bone && !smart_cast<IKinematics*>(object->Visual())))
        return GWP_ERROR_INVALID_ARGUMENT;
    CScriptHit hit;
    hit.m_fPower = power;
    hit.m_tDirection = ControlVector(dir);
    hit.set_bone_name(has_bone ? bone : "");
    hit.m_tpDraftsman = draftsman;
    hit.m_fImpulse = impulse;
    hit.m_tHitType = static_cast<int>(hit_type);
    object->lua_game_object()->Hit(&hit);
    return GWP_OK;
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group npc_sight)
// ---------------------------------------------------------------------------------------------

constexpr pcstr kSight = "npc_sight";

// The forms without an object: OBJECT and FIRE_OBJECT look at an object nobody passes here (the sight action would
// dereference a null object on its next update) - npc_set_sight_object is the call for them. LOOK_OVER has no
// branch in CSightAction (NODEFAULT on its next update), and FIRE_POSITION has one only as the POSITION the
// form with a point turns it into: the other forms refuse it (with_point false)
bool ValidSightType(uint32_t sight_type, bool with_point)
{
    return sight_type <= GWP_SIGHT_ANIMATION_DIRECTION && sight_type != GWP_SIGHT_OBJECT &&
        sight_type != GWP_SIGHT_FIRE_OBJECT && sight_type != GWP_SIGHT_LOOK_OVER &&
        (with_point || sight_type != GWP_SIGHT_FIRE_POSITION);
}

// A direction the sight manager normalizes: a zero or non-finite one gives NaN there
bool ValidSightDirection(const Fvector& direction)
{
    const float magnitude = direction.magnitude();
    return std::isfinite(magnitude) && magnitude > EPS_S;
}

GwpResult GWP_CALL ApiNpcSetSightType(GwpObjectId id, uint32_t sight_type, int torso_look, int path)
{
    CAI_Stalker* stalker = FindStalker(id, kSight, "npc_set_sight_type");
    if (!stalker || !ValidSightType(sight_type, false))
        return IsMainThread() ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_MAIN_THREAD;
    // set_sight(type, torso_look, path), e.g. (CSightParams.eSightTypeAnimationDirection, false, false)
    stalker->lua_game_object()->set_sight(
        static_cast<SightManager::ESightType>(sight_type), torso_look != 0, path != 0);
    return GWP_OK;
}

GwpResult GWP_CALL ApiNpcSetSightPosition(GwpObjectId id, uint32_t sight_type, const float* point_xyz)
{
    CAI_Stalker* stalker = FindStalker(id, kSight, "npc_set_sight_position");
    if (!stalker || !ValidSightType(sight_type, true))
        return IsMainThread() ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_MAIN_THREAD;
    if (point_xyz && !Finite3(point_xyz))
        return GWP_ERROR_INVALID_ARGUMENT;
    // set_sight(type, point or nil, 0): the form of state_mgr_direction.look_position_type
    Fvector point = point_xyz ? ControlVector(point_xyz) : Fvector().set(0.f, 0.f, 0.f);
    // DIRECTION takes the point as a direction: the same check as npc_set_sight_direction
    if (sight_type == GWP_SIGHT_DIRECTION && point_xyz && !ValidSightDirection(point))
        return GWP_ERROR_INVALID_ARGUMENT;
    stalker->lua_game_object()->set_sight(
        static_cast<SightManager::ESightType>(sight_type), point_xyz ? &point : nullptr, 0u);
    return GWP_OK;
}

GwpResult GWP_CALL ApiNpcSetSightDirection(GwpObjectId id, uint32_t sight_type, const float dir[3], int torso_look)
{
    CAI_Stalker* stalker = FindStalker(id, kSight, "npc_set_sight_direction");
    if (!stalker || !ValidSightType(sight_type, false) || !dir)
        return IsMainThread() ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_MAIN_THREAD;
    if (!Finite3(dir))
        return GWP_ERROR_INVALID_ARGUMENT;
    Fvector direction = ControlVector(dir);
    // The method normalizes a longer vector (in place: Lua sees its vector changed; here it is a copy). A zero or
    // non-finite one would give NaN in the sight manager, the state manager never passes it: refused.
    if (sight_type == GWP_SIGHT_DIRECTION && !ValidSightDirection(direction))
        return GWP_ERROR_INVALID_ARGUMENT;
    stalker->lua_game_object()->set_sight(
        static_cast<SightManager::ESightType>(sight_type), direction, torso_look != 0);
    return GWP_OK;
}

GwpResult GWP_CALL ApiNpcSetSightObject(GwpObjectId id, GwpObjectId target, int torso_look, int fire_object,
    int no_pitch)
{
    CAI_Stalker* stalker = FindStalker(id, kSight, "npc_set_sight_object");
    CScriptGameObject* object = stalker ? FindArgument(target) : nullptr;
    if (!object) // the Lua method dereferences a nil object
        return IsMainThread() ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_MAIN_THREAD;
    // The 3-argument Lua form set_sight(obj, torso, fire) is this one with no_pitch = false (the same CSightAction)
    stalker->lua_game_object()->set_sight(object, torso_look != 0, fire_object != 0, no_pitch != 0);
    return GWP_OK;
}

int GWP_CALL ApiNpcSightParams(GwpObjectId id, GwpSightParams* out)
{
    if (out)
        *out = GwpSightParams{ GWP_SIGHT_DUMMY, GWP_INVALID_OBJECT_ID, 0, { flt_max, flt_max, flt_max } };
    CAI_Stalker* stalker = FindStalker(id, kSight, "npc_sight_params");
    if (!stalker || !out)
        return 0;
    const CSightParams params = stalker->lua_game_object()->sight_params();
    out->sight_type = static_cast<uint32_t>(params.m_sight_type);
    out->object = params.m_object ? params.m_object->ID() : GWP_INVALID_OBJECT_ID;
    out->vector[0] = params.m_vector.x;
    out->vector[1] = params.m_vector.y;
    out->vector[2] = params.m_vector.z;
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group npc_anim)
// ---------------------------------------------------------------------------------------------

constexpr pcstr kAnim = "npc_anim";

// The two add_animation forms share the checks and the answer: GWP_ERROR when the queue did not grow. The Lua
// method returns silently then (an unknown motion is logged as Info by the engine).
template <typename AddFn>
GwpResult AddAnimation(GwpObjectId id, pcstr function, const char* name, AddFn add)
{
    CAI_Stalker* stalker = FindStalker(id, kAnim, function);
    if (!stalker || !name)
        return IsMainThread() ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_MAIN_THREAD;
    // With a global selector the Lua method logs a script error and adds nothing: refused without the log
    if (stalker->animation().global_selector())
        return GWP_ERROR;
    CScriptGameObject* npc = stalker->lua_game_object();
    const int before = npc->animation_count();
    add(*npc);
    return npc->animation_count() > before ? GWP_OK : GWP_ERROR;
}

GwpResult GWP_CALL ApiNpcAddAnimation(GwpObjectId id, const char* name, int hand_usage, int use_movement_controller)
{
    return AddAnimation(id, "npc_add_animation", name, [&](CScriptGameObject& npc) {
        npc.add_animation(name, hand_usage != 0, use_movement_controller != 0);
    });
}

GwpResult GWP_CALL ApiNpcAddAnimationAt(GwpObjectId id, const char* name, int hand_usage, const float position[3],
    const float rotation[3], int local_animation)
{
    if (!position || !rotation || !Finite3(position) || !Finite3(rotation))
        return CheckControlCall(kAnim, "npc_add_animation_at") ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_MAIN_THREAD;
    return AddAnimation(id, "npc_add_animation_at", name, [&](CScriptGameObject& npc) {
        npc.add_animation(name, hand_usage != 0, ControlVector(position), ControlVector(rotation), local_animation != 0);
    });
}

GwpResult GWP_CALL ApiNpcClearAnimations(GwpObjectId id)
{
    CAI_Stalker* stalker = FindStalker(id, kAnim, "npc_clear_animations");
    if (!stalker)
        return IsMainThread() ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_MAIN_THREAD;
    stalker->lua_game_object()->clear_animations();
    return GWP_OK;
}

int GWP_CALL ApiNpcHasScriptAnimationCallback(GwpObjectId id)
{
    CAI_Stalker* stalker = FindStalker(id, kAnim, "npc_has_script_animation_callback");
    return stalker && stalker->callback(GameObject::eScriptAnimation) ? 1 : 0;
}

int32_t GWP_CALL ApiNpcAnimationCount(GwpObjectId id)
{
    CAI_Stalker* stalker = FindStalker(id, kAnim, "npc_animation_count");
    return stalker ? stalker->lua_game_object()->animation_count() : -1; // -1: what Lua gets for a non-stalker
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group inventory)
// ---------------------------------------------------------------------------------------------

constexpr pcstr kInventory = "inventory";

GwpObjectId GWP_CALL ApiNpcBestWeapon(GwpObjectId id)
{
    CAI_Stalker* stalker = FindStalker(id, kInventory, "npc_best_weapon");
    const CScriptGameObject* weapon = stalker ? stalker->lua_game_object()->best_weapon() : nullptr;
    return weapon ? weapon->ID() : GWP_INVALID_OBJECT_ID;
}

GwpObjectId GWP_CALL ApiNpcItemInSlot(GwpObjectId id, uint32_t slot)
{
    if (!CheckControlCall(kInventory, "npc_item_in_slot"))
        return GWP_INVALID_OBJECT_ID;
    // Any inventory owner, as in Lua; the method applies the minus_one_slot_ordering shift of the Lua slot numbers
    CGameObject* object = FindControlObject(id);
    if (!object || !smart_cast<CInventoryOwner*>(object) || slot > u16(-1))
        return GWP_INVALID_OBJECT_ID;
    const CScriptGameObject* item = object->lua_game_object()->item_in_slot(slot);
    return item ? item->ID() : GWP_INVALID_OBJECT_ID;
}

int GWP_CALL ApiNpcWeaponStrapped(GwpObjectId id)
{
    CAI_Stalker* stalker = FindStalker(id, kInventory, "npc_weapon_strapped");
    return stalker && stalker->lua_game_object()->weapon_strapped() ? 1 : 0;
}

int GWP_CALL ApiNpcWeaponUnstrapped(GwpObjectId id)
{
    CAI_Stalker* stalker = FindStalker(id, kInventory, "npc_weapon_unstrapped");
    return stalker && stalker->lua_game_object()->weapon_unstrapped() ? 1 : 0;
}

int GWP_CALL ApiNpcIsWeaponGoingToBeStrapped(GwpObjectId id, GwpObjectId item)
{
    CAI_Stalker* stalker = FindStalker(id, kInventory, "npc_is_weapon_going_to_be_strapped");
    const CScriptGameObject* object = stalker ? FindArgument(item) : nullptr;
    return object && stalker->lua_game_object()->is_weapon_going_to_be_strapped(object) ? 1 : 0;
}

// The actions the object handler takes, as bits: CObjectHandlerPlanner::object_property has a case only for these
// (RELOAD*, TURN_*, SHOW, HIDE and TAKE reach its NODEFAULT, with an item or without). 7 and 12-15 are FireNoReload,
// AimReady1/2 and AimForceFull1/2 of MonsterSpace, without a GWP_OBJECT_* name.
constexpr u32 ObjectActionBit(uint32_t action) { return 1u << action; }
constexpr u32 kHandledObjectActions = ObjectActionBit(GWP_OBJECT_SWITCH1) | ObjectActionBit(GWP_OBJECT_SWITCH2) |
    ObjectActionBit(GWP_OBJECT_AIM1) | ObjectActionBit(GWP_OBJECT_AIM2) | ObjectActionBit(GWP_OBJECT_FIRE1) |
    ObjectActionBit(7) | ObjectActionBit(GWP_OBJECT_FIRE2) | ObjectActionBit(GWP_OBJECT_IDLE) |
    ObjectActionBit(GWP_OBJECT_STRAP) | ObjectActionBit(GWP_OBJECT_DROP) | ObjectActionBit(12) | ObjectActionBit(13) |
    ObjectActionBit(14) | ObjectActionBit(15) | ObjectActionBit(GWP_OBJECT_ACTIVATE) |
    ObjectActionBit(GWP_OBJECT_DEACTIVATE) | ObjectActionBit(GWP_OBJECT_USE);
// With an item the goal is a property of that item, and only a weapon and a missile have evaluators
// (CObjectHandlerPlanner::add_item): a goal without one fails every solve (a crash where THROW is off).
// DEACTIVATE puts the item away whatever it is (the goal is "no items idle").
constexpr u32 kWeaponObjectActions = kHandledObjectActions & ~ObjectActionBit(GWP_OBJECT_USE); // no "used" property
constexpr u32 kMissileObjectActions = ObjectActionBit(GWP_OBJECT_FIRE1) | ObjectActionBit(GWP_OBJECT_IDLE) |
    ObjectActionBit(GWP_OBJECT_DROP) | ObjectActionBit(GWP_OBJECT_ACTIVATE) | ObjectActionBit(GWP_OBJECT_DEACTIVATE);

// An action the object handler of `stalker` can take with `item` (nullptr: nothing in the hands)
bool ValidItemAction(const CAI_Stalker& stalker, uint32_t action, const CGameObject* item)
{
    if (action > GWP_OBJECT_TAKE || !(kHandledObjectActions & ObjectActionBit(action)))
        return false;
    if (!item || action == GWP_OBJECT_DEACTIVATE)
        return true;
    // An item of someone else has no operators in the object handler of this NPC either
    if (item->H_Parent() != &stalker)
        return false;
    if (smart_cast<const CWeapon*>(item))
        return (kWeaponObjectActions & ObjectActionBit(action)) != 0;
    if (smart_cast<const CMissile*>(item))
        return (kMissileObjectActions & ObjectActionBit(action)) != 0;
    return false;
}

GwpResult GWP_CALL ApiNpcSetItem(
    GwpObjectId id, uint32_t action, GwpObjectId item, uint32_t queue_size, uint32_t queue_interval)
{
    CAI_Stalker* stalker = FindStalker(id, kInventory, "npc_set_item");
    if (!stalker)
        return IsMainThread() ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_MAIN_THREAD;
    // An item id that is not online is an error, not nil: nil means "nothing in the hands" to the object handler
    CScriptGameObject* object = item != GWP_INVALID_OBJECT_ID ? FindArgument(item) : nullptr;
    const CGameObject* item_object = object ? &object->object() : nullptr;
    if ((item != GWP_INVALID_OBJECT_ID && !object) || !ValidItemAction(*stalker, action, item_object) ||
        (queue_size == GWP_QUEUE_DEFAULT && queue_interval != GWP_QUEUE_DEFAULT))
        return GWP_ERROR_INVALID_ARGUMENT;
    CScriptGameObject* npc = stalker->lua_game_object();
    const auto object_action = static_cast<MonsterSpace::EObjectAction>(action);
    // The overload of the Lua call with the same arguments: they differ in the queue defaults of set_goal
    if (queue_size == GWP_QUEUE_DEFAULT)
        npc->set_item(object_action, object);
    else if (queue_interval == GWP_QUEUE_DEFAULT)
        npc->set_item(object_action, object, queue_size);
    else
        npc->set_item(object_action, object, queue_size, queue_interval);
    return GWP_OK;
}

// The weapon argument of aim_time is dereferenced by the Lua method: an online CWeapon is required here, one the
// NPC holds - the operator of another one is not found exactly (get_operator answers a neighbouring one)
CScriptGameObject* FindWeapon(const CAI_Stalker& owner, GwpObjectId id)
{
    CGameObject* object = FindControlObject(id);
    const bool held = object && smart_cast<CWeapon*>(object) && object->H_Parent() == &owner;
    return held ? object->lua_game_object() : nullptr;
}

uint32_t GWP_CALL ApiNpcAimTime(GwpObjectId id, GwpObjectId weapon)
{
    CAI_Stalker* stalker = FindStalker(id, kInventory, "npc_aim_time");
    CScriptGameObject* object = stalker ? FindWeapon(*stalker, weapon) : nullptr;
    return object ? stalker->lua_game_object()->aim_time(object) : u32(-1); // u32(-1): what Lua gets on an error
}

GwpResult GWP_CALL ApiNpcSetAimTime(GwpObjectId id, GwpObjectId weapon, uint32_t aim_time_ms)
{
    CAI_Stalker* stalker = FindStalker(id, kInventory, "npc_set_aim_time");
    CScriptGameObject* object = stalker ? FindWeapon(*stalker, weapon) : nullptr;
    if (!object)
        return IsMainThread() ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_MAIN_THREAD;
    stalker->lua_game_object()->aim_time(object, aim_time_ms);
    return GWP_OK;
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group smart_cover)
// ---------------------------------------------------------------------------------------------

constexpr pcstr kCover = "smart_cover";

int GWP_CALL ApiNpcUseSmartCoversOnly(GwpObjectId id)
{
    CAI_Stalker* stalker = FindStalker(id, kCover, "npc_use_smart_covers_only");
    const CScriptGameObject* npc = stalker ? stalker->lua_game_object() : nullptr;
    return npc && npc->use_smart_covers_only() ? 1 : 0;
}

GwpResult GWP_CALL ApiNpcSetUseSmartCoversOnly(GwpObjectId id, int value)
{
    CAI_Stalker* stalker = FindStalker(id, kCover, "npc_set_use_smart_covers_only");
    if (!stalker)
        return IsMainThread() ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_MAIN_THREAD;
    stalker->lua_game_object()->use_smart_covers_only(value != 0);
    return GWP_OK;
}

GwpResult GWP_CALL ApiNpcSetDestSmartCover(GwpObjectId id, const char* cover_name)
{
    CAI_Stalker* stalker = FindStalker(id, kCover, "npc_set_dest_smart_cover");
    if (!stalker)
        return IsMainThread() ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_MAIN_THREAD;
    // An unknown name: CCoverManager::smart_cover dereferences end() in release (the Lua call crashes the same way)
    if (cover_name && cover_name[0] && !ai().cover_manager().find_smart_cover(shared_str(cover_name)))
        return GWP_ERROR_INVALID_ARGUMENT;
    if (cover_name && cover_name[0])
        stalker->lua_game_object()->set_dest_smart_cover(cover_name);
    else
        stalker->lua_game_object()->set_dest_smart_cover(); // the Lua call without arguments: no destination
    return GWP_OK;
}

const char* GWP_CALL ApiNpcDestSmartCoverName(GwpObjectId id)
{
    CAI_Stalker* stalker = FindStalker(id, kCover, "npc_dest_smart_cover_name");
    return stalker ? stalker->lua_game_object()->get_dest_smart_cover_name() : nullptr;
}

GwpResult GWP_CALL ApiNpcSetDestLoophole(GwpObjectId id, const char* loophole_name)
{
    CAI_Stalker* stalker = FindStalker(id, kCover, "npc_set_dest_loophole");
    if (!stalker)
        return IsMainThread() ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_MAIN_THREAD;
    // A loophole needs the smart cover set first and one of its loopholes: stalker_movement_params::cover_loophole_id
    // dereferences the cover and end() of its loopholes in release
    if (loophole_name && loophole_name[0])
    {
        const smart_cover::cover* cover = stalker->movement().target_params().cover();
        if (!cover || !cover->get_description()->get_loophole(shared_str(loophole_name)))
            return GWP_ERROR_INVALID_ARGUMENT;
    }
    if (loophole_name && loophole_name[0])
        stalker->lua_game_object()->set_dest_loophole(loophole_name);
    else
        stalker->lua_game_object()->set_dest_loophole();
    return GWP_OK;
}

GwpResult GWP_CALL ApiNpcSetSmartCoverTarget(GwpObjectId id, const float* point_xyz)
{
    CAI_Stalker* stalker = FindStalker(id, kCover, "npc_set_smart_cover_target");
    if (!stalker)
        return IsMainThread() ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_MAIN_THREAD;
    if (point_xyz && !Finite3(point_xyz))
        return GWP_ERROR_INVALID_ARGUMENT;
    if (point_xyz)
        stalker->lua_game_object()->set_smart_cover_target(ControlVector(point_xyz));
    else
        stalker->lua_game_object()->set_smart_cover_target(); // the state manager's exit from a cover
    return GWP_OK;
}

GwpResult GWP_CALL ApiNpcSetSmartCoverTargetObject(GwpObjectId id, GwpObjectId target)
{
    CAI_Stalker* stalker = FindStalker(id, kCover, "npc_set_smart_cover_target_object");
    CScriptGameObject* object = stalker ? FindArgument(target) : nullptr;
    if (!object) // the Lua method dereferences a nil object
        return IsMainThread() ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_MAIN_THREAD;
    stalker->lua_game_object()->set_smart_cover_target(object);
    return GWP_OK;
}

GwpResult GWP_CALL ApiNpcClearSmartCoverTargetSelector(GwpObjectId id)
{
    CAI_Stalker* stalker = FindStalker(id, kCover, "npc_clear_smart_cover_target_selector");
    if (!stalker)
        return IsMainThread() ? GWP_ERROR_INVALID_ARGUMENT : GWP_ERROR_NOT_MAIN_THREAD;
    stalker->lua_game_object()->set_smart_cover_target_selector();
    return GWP_OK;
}

// ---------------------------------------------------------------------------------------------
// The end of a script animation (engine hook, addon_api_npc_control.h)
// ---------------------------------------------------------------------------------------------

constexpr pcstr kScriptAnimationEnd = "npc_on_script_animation_end";

// Id of the event, declared once with its schema: (npc, remaining animations). The bus lives for the whole process.
GwpEventId ScriptAnimationEndEvent()
{
    static const GwpEventId id = [] {
        events::Declare(kScriptAnimationEnd, 0, "oI");
        return events::Intern(kScriptAnimationEnd);
    }();
    return id;
}
} // namespace

namespace npcctl
{
void ScriptAnimationEnd(CAI_Stalker& npc)
{
    // Emitted with or without subscribers, as the other engine events: event_trace sees it either way
    const GwpValue args[] = { events::Object(npc.ID()),
        events::Int(static_cast<s64>(npc.animation().script_animations().size())) };
    events::Emit(ScriptAnimationEndEvent(), args, 2);
}

bool WantsScriptAnimationEnd() { return events::HasSubscribers(ScriptAnimationEndEvent()); }

namespace
{
GwpEventId PatrolPointEvent()
{
    static const GwpEventId id = [] {
        events::Declare("npc_on_patrol_point", 0, "oII");
        return events::Intern("npc_on_patrol_point");
    }();
    return id;
}

GwpEventId PatrolExtrapolateEvent()
{
    static const GwpEventId id = [] {
        events::Declare("npc_on_patrol_extrapolate", 0, "oI");
        return events::Intern("npc_on_patrol_extrapolate");
    }();
    return id;
}
} // namespace

// Every NPC on a patrol path calls these, often: the arguments are built only for a subscriber
void PatrolPoint(u16 npc, u32 action_type, u32 point)
{
    const GwpEventId event = PatrolPointEvent();
    if (!events::HasSubscribers(event))
        return;
    const GwpValue args[] = { events::Object(npc), events::Int(static_cast<s64>(action_type)),
        events::Int(static_cast<s64>(point)) };
    events::Emit(event, args, 3);
}

void PatrolExtrapolate(u16 npc, u32 point)
{
    const GwpEventId event = PatrolExtrapolateEvent();
    if (!events::HasSubscribers(event))
        return;
    const GwpValue args[] = { events::Object(npc), events::Int(static_cast<s64>(point)) };
    events::Emit(event, args, 2);
}
} // namespace npcctl

void FillNpcControlApi(GwpEngineApi& api)
{
    // group npc_body
    api.npc_set_movement_type = &ApiNpcSetMovementType;
    api.npc_set_body_state = &ApiNpcSetBodyState;
    api.npc_set_mental_state = &ApiNpcSetMentalState;
    api.npc_set_path_type = &ApiNpcSetPathType;
    api.npc_movement_type = &ApiNpcMovementType;
    api.npc_target_movement_type = &ApiNpcTargetMovementType;
    api.npc_target_body_state = &ApiNpcTargetBodyState;
    api.npc_target_mental_state = &ApiNpcTargetMentalState;
    api.npc_is_body_turning = &ApiNpcIsBodyTurning;
    api.npc_movement_enabled = &ApiNpcMovementEnabled;
    api.npc_enable_movement = &ApiNpcEnableMovement;
    api.npc_special_danger_move = &ApiNpcSpecialDangerMove;
    api.npc_set_special_danger_move = &ApiNpcSetSpecialDangerMove;
    api.npc_inactualize_patrol_path = &ApiNpcInactualizePatrolPath;
    api.npc_accessible = &ApiNpcAccessible;
    api.npc_set_dest_level_vertex = &ApiNpcSetDestLevelVertex;
    api.npc_set_dest_game_vertex = &ApiNpcSetDestGameVertex;
    api.npc_set_desired_position = &ApiNpcSetDesiredPosition;
    api.npc_set_desired_direction = &ApiNpcSetDesiredDirection;
    api.npc_set_detail_path_type = &ApiNpcSetDetailPathType;
    api.npc_set_movement_selection_type = &ApiNpcSetMovementSelectionType;
    api.npc_path_type = &ApiNpcPathType;
    api.npc_detail_path_type = &ApiNpcDetailPathType;
    api.npc_mental_state = &ApiNpcMentalState;
    api.npc_accessible_position = &ApiNpcAccessiblePosition;
    api.npc_accessible_nearest = &ApiNpcAccessibleNearest;
    api.npc_location_on_path = &ApiNpcLocationOnPath;
    api.npc_path_completed = &ApiNpcPathCompleted;
    api.object_game_vertex_id = &ApiObjectGameVertexId;
    api.npc_dest_level_vertex = &ApiNpcDestLevelVertex;
    api.npc_dest_game_vertex = &ApiNpcDestGameVertex;
    api.npc_set_patrol_path = &ApiNpcSetPatrolPath;
    api.npc_set_start_point = &ApiNpcSetStartPoint;
    api.npc_patrol_point_index = &ApiNpcPatrolPointIndex;
    api.npc_patrol_path_name = &ApiNpcPatrolPathName;
    api.npc_body_state = &ApiNpcBodyState;
    api.npc_play_sound = &ApiNpcPlaySound;
    api.npc_sniper_update_rate = &ApiNpcSniperUpdateRate;
    api.npc_set_sniper_update_rate = &ApiNpcSetSniperUpdateRate;
    api.level_high_cover_in_direction = &ApiLevelHighCoverInDirection;
    api.level_low_cover_in_direction = &ApiLevelLowCoverInDirection;
    api.npc_target_path_type = &ApiNpcTargetPathType;
    api.npc_target_detail_path_type = &ApiNpcTargetDetailPathType;
    // group patrol
    api.patrol_find = &ApiPatrolFind;
    api.patrol_point_count = &ApiPatrolPointCount;
    api.patrol_point_position = &ApiPatrolPointPosition;
    api.patrol_point_level_vertex = &ApiPatrolPointLevelVertex;
    api.patrol_point_game_vertex = &ApiPatrolPointGameVertex;
    api.patrol_point_flags = &ApiPatrolPointFlags;
    api.patrol_point_terminal = &ApiPatrolPointTerminal;
    api.patrol_point_name = &ApiPatrolPointName;
    api.patrol_point_index = &ApiPatrolPointIndex;
    api.patrol_nearest_point = &ApiPatrolNearestPoint;
    // group objects
    api.object_level_vertex_id = &ApiObjectLevelVertexId;
    api.object_hit = &ApiObjectHit;
    // group npc_sight
    api.npc_set_sight_type = &ApiNpcSetSightType;
    api.npc_set_sight_position = &ApiNpcSetSightPosition;
    api.npc_set_sight_direction = &ApiNpcSetSightDirection;
    api.npc_set_sight_object = &ApiNpcSetSightObject;
    api.npc_sight_params = &ApiNpcSightParams;
    // group npc_anim
    api.npc_add_animation = &ApiNpcAddAnimation;
    api.npc_add_animation_at = &ApiNpcAddAnimationAt;
    api.npc_clear_animations = &ApiNpcClearAnimations;
    api.npc_animation_count = &ApiNpcAnimationCount;
    api.npc_has_script_animation_callback = &ApiNpcHasScriptAnimationCallback;
    // group inventory
    api.npc_best_weapon = &ApiNpcBestWeapon;
    api.npc_item_in_slot = &ApiNpcItemInSlot;
    api.npc_weapon_strapped = &ApiNpcWeaponStrapped;
    api.npc_weapon_unstrapped = &ApiNpcWeaponUnstrapped;
    api.npc_is_weapon_going_to_be_strapped = &ApiNpcIsWeaponGoingToBeStrapped;
    api.npc_set_item = &ApiNpcSetItem;
    api.npc_aim_time = &ApiNpcAimTime;
    api.npc_set_aim_time = &ApiNpcSetAimTime;
    // group smart_cover
    api.npc_use_smart_covers_only = &ApiNpcUseSmartCoversOnly;
    api.npc_set_use_smart_covers_only = &ApiNpcSetUseSmartCoversOnly;
    api.npc_set_dest_smart_cover = &ApiNpcSetDestSmartCover;
    api.npc_dest_smart_cover_name = &ApiNpcDestSmartCoverName;
    api.npc_set_dest_loophole = &ApiNpcSetDestLoophole;
    api.npc_set_smart_cover_target = &ApiNpcSetSmartCoverTarget;
    api.npc_set_smart_cover_target_object = &ApiNpcSetSmartCoverTargetObject;
    api.npc_clear_smart_cover_target_selector = &ApiNpcClearSmartCoverTargetSelector;
}
} // namespace gw::addons
