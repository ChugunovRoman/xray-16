#include "StdAfx.h"

// Plugin API, groups "npc" (what an NPC knows) and "script" (calling Lua from a plugin).
// The npc functions go through CScriptGameObject, the object Lua sees, so a plugin gets exactly the values of
// npc:best_enemy(), npc:best_danger(), npc:memory_time(...) - including their filters of dead or unloaded objects.
// Docs: wiki/doc/plugins/api/npc.md, wiki/doc/plugins/api/script.md

#include "addon_event_bus.h"
#include "addon_host.h"

#include "ai_space.h"
#include "alife_object_registry.h"
#include "alife_simulator.h"
#include "xrCore/clsid.h"
#include "xrAICore/Navigation/game_graph.h"
#include <luabind/wrapper_base.hpp>
#include <cstdint>
#include <cstring>
#include "CustomMonster.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "danger_object.h"

// The danger types of gwp_api.h are CDangerObject::EDangerType (npc_best_danger hands the engine value on)
static_assert(GWP_DANGER_BULLET_RICOCHET == CDangerObject::eDangerTypeBulletRicochet);
static_assert(GWP_DANGER_ATTACK_SOUND == CDangerObject::eDangerTypeAttackSound);
static_assert(GWP_DANGER_ENTITY_ATTACKED == CDangerObject::eDangerTypeEntityAttacked);
static_assert(GWP_DANGER_ENTITY_DEATH == CDangerObject::eDangerTypeEntityDeath);
static_assert(GWP_DANGER_FRESH_ENTITY_CORPSE == CDangerObject::eDangerTypeFreshEntityCorpse);
static_assert(GWP_DANGER_ATTACKED == CDangerObject::eDangerTypeAttacked);
static_assert(GWP_DANGER_GRENADE == CDangerObject::eDangerTypeGrenade);
static_assert(GWP_DANGER_ENEMY_SOUND == CDangerObject::eDangerTypeEnemySound);
#include "entity_alive.h"
#include "EntityCondition.h"
#include "memory_manager.h"
#include "memory_space_impl.h"
#include "visual_memory_manager.h"
#include "Include/xrRender/Kinematics.h"
#include "InventoryOwner.h"
#include "HudItem.h"
#include "Actor.h"
#include "ai/stalker/ai_stalker.h"
#include "stalker_movement_manager_smart_cover.h"
#include "Level.h"
#include "script_game_object.h"
#include "stalker_planner.h"
#include "xrScriptEngine/script_engine.hpp"

namespace gw::addons
{
namespace
{
bool CheckNpcCall(pcstr group, pcstr function)
{
    if (IsMainThread())
        return true;
    Msg("! [%s] %s called outside the main thread, ignored", group, function);
    return false;
}

CGameObject* FindOnline(GwpObjectId id)
{
    if (!g_pGameLevel || id == GWP_INVALID_OBJECT_ID)
        return nullptr;
    CGameObject* object = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
    return object && !object->getDestroy() ? object : nullptr;
}

// The Lua view of an NPC (stalker or monster), or nullptr.
CScriptGameObject* FindNpc(GwpObjectId id, pcstr function)
{
    if (!CheckNpcCall("npc", function))
        return nullptr;
    CGameObject* object = FindOnline(id);
    return object && smart_cast<CCustomMonster*>(object) ? object->lua_game_object() : nullptr;
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group npc)
// ---------------------------------------------------------------------------------------------

GwpObjectId GWP_CALL ApiNpcBestEnemy(GwpObjectId id)
{
    CScriptGameObject* npc = FindNpc(id, "npc_best_enemy");
    const CScriptGameObject* enemy = npc ? npc->GetBestEnemy() : nullptr;
    return enemy ? enemy->ID() : GWP_INVALID_OBJECT_ID;
}

GwpObjectId GWP_CALL ApiNpcCurrentEnemy(GwpObjectId id)
{
    CScriptGameObject* npc = FindNpc(id, "npc_current_enemy");
    auto* monster = npc ? smart_cast<CCustomMonster*>(&npc->object()) : nullptr;
    // GetEnemy logs a script error for a dead NPC: answered "none" here without it
    if (!monster || !monster->g_Alive())
        return GWP_INVALID_OBJECT_ID;
    const CScriptGameObject* enemy = npc->GetEnemy();
    return enemy ? enemy->ID() : GWP_INVALID_OBJECT_ID;
}

int GWP_CALL ApiNpcBestDanger(GwpObjectId id, GwpDanger* out)
{
    if (out)
        *out = GwpDanger{ 0, 0, GWP_INVALID_OBJECT_ID, 0, { 0.f, 0.f, 0.f } };
    CScriptGameObject* npc = FindNpc(id, "npc_best_danger");
    const CDangerObject* danger = npc && out ? npc->GetBestDanger() : nullptr;
    if (!danger)
        return 0;
    out->type = static_cast<uint32_t>(danger->type());
    out->time = danger->time();
    out->object = danger->object() ? danger->object()->ID() : GWP_INVALID_OBJECT_ID;
    const Fvector& position = danger->position();
    out->position[0] = position.x;
    out->position[1] = position.y;
    out->position[2] = position.z;
    return 1;
}

uint32_t GWP_CALL ApiNpcMemoryTime(GwpObjectId id, GwpObjectId other)
{
    CScriptGameObject* npc = FindNpc(id, "npc_memory_time");
    CGameObject* object = npc ? FindOnline(other) : nullptr;
    return object ? npc->memory_time(*object->lua_game_object()) : 0;
}

int GWP_CALL ApiNpcMemoryPosition(GwpObjectId id, GwpObjectId other, float out_xyz[3])
{
    if (out_xyz)
        out_xyz[0] = out_xyz[1] = out_xyz[2] = 0.f;
    CScriptGameObject* npc = FindNpc(id, "npc_memory_position");
    CGameObject* object = npc && out_xyz ? FindOnline(other) : nullptr;
    if (!object || !npc->memory_time(*object->lua_game_object()))
        return 0; // never seen, heard or hit: Lua gets a zero vector here
    const Fvector position = npc->memory_position(*object->lua_game_object());
    out_xyz[0] = position.x;
    out_xyz[1] = position.y;
    out_xyz[2] = position.z;
    return 1;
}

const char* GWP_CALL ApiNpcCommunity(GwpObjectId id)
{
    if (!CheckNpcCall("npc", "npc_community"))
        return nullptr;
    // An inventory owner only: CharacterCommunity logs a script error for anything else.
    const CInventoryOwner* owner = smart_cast<const CInventoryOwner*>(FindOnline(id));
    return owner ? owner->CharacterInfo().Community().id().c_str() : nullptr;
}

GwpObjectId GWP_CALL ApiNpcActiveItem(GwpObjectId id)
{
    if (!CheckNpcCall("npc", "npc_active_item"))
        return GWP_INVALID_OBJECT_ID;
    // An inventory owner only: GetActiveItem logs a script error for anything else.
    CInventoryOwner* owner = smart_cast<CInventoryOwner*>(FindOnline(id));
    const PIItem item = owner ? owner->inventory().ActiveItem() : nullptr;
    return item ? item->object().ID() : GWP_INVALID_OBJECT_ID;
}

uint32_t GWP_CALL ApiNpcFeelTouch(GwpObjectId id, GwpObjectId* out, uint32_t max)
{
    if (!CheckNpcCall("npc", "npc_feel_touch"))
        return 0;
    Feel::Touch* touch = smart_cast<Feel::Touch*>(FindOnline(id));
    if (!touch)
        return 0;
    const u32 count = static_cast<u32>(touch->feel_touch.size());
    for (u32 i = 0; out && i < count && i < max; ++i)
        out[i] = touch->feel_touch[i]->ID();
    return count;
}

int GWP_CALL ApiNpcCriticallyWounded(GwpObjectId id)
{
    if (!CheckNpcCall("npc", "npc_critically_wounded"))
        return 0;
    CCustomMonster* npc = smart_cast<CCustomMonster*>(FindOnline(id)); // critically_wounded is not const
    return npc && npc->critically_wounded() ? 1 : 0;
}

int GWP_CALL ApiNpcInSmartCover(GwpObjectId id)
{
    if (!CheckNpcCall("npc", "npc_in_smart_cover"))
        return 0;
    // A stalker only: the Lua method answers "" (true in Lua) with a script error for anything else
    CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(FindOnline(id));
    return stalker && stalker->movement().in_smart_cover() ? 1 : 0;
}

int GWP_CALL ApiNpcIsTalking(GwpObjectId id)
{
    if (!CheckNpcCall("npc", "npc_is_talking"))
        return 0;
    // CScriptGameObject::IsTalking: an inventory owner in a dialog (the actor answers here too), false for
    // anything else, without the script error Lua logs. IsTalking is not const there either
    CInventoryOwner* owner = smart_cast<CInventoryOwner*>(FindOnline(id));
    return owner && owner->IsTalking() ? 1 : 0;
}

uint32_t GWP_CALL ApiNpcMainAction(GwpObjectId id)
{
    if (!CheckNpcCall("npc", "npc_main_action"))
        return GWP_INVALID_ACTION_ID;
    CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(FindOnline(id));
    if (!stalker || !stalker->g_Alive())
        return GWP_INVALID_ACTION_ID;
    // motivation_action_manager() of Lua is this planner; current_action_id() asserts on an idle planner
    const CStalkerPlanner& brain = stalker->brain();
    return brain.initialized() ? static_cast<uint32_t>(brain.current_action_id()) : GWP_INVALID_ACTION_ID;
}

int GWP_CALL ApiNpcSee(GwpObjectId id, GwpObjectId other)
{
    if (!CheckNpcCall("npc", "npc_see"))
        return 0;
    CGameObject* npc = FindOnline(id);
    CGameObject* object = npc ? FindOnline(other) : nullptr;
    if (!object)
        return 0;
    // CheckObjectVisibility logs a script error for a dead NPC and for an object that is neither a script entity
    // nor the actor: answered here as it answers, without the log
    const CEntityAlive* alive = smart_cast<const CEntityAlive*>(npc);
    if (alive && !alive->g_Alive())
        return 0;
    if (!smart_cast<CCustomMonster*>(npc) && !smart_cast<CActor*>(npc))
        return 0;
    return npc->lua_game_object()->CheckObjectVisibility(object->lua_game_object()) ? 1 : 0;
}

uint32_t GWP_CALL ApiNpcMemoryVisibleObjects(GwpObjectId id, GwpObjectId* out, uint32_t max)
{
    if (!CheckNpcCall("npc", "npc_memory_visible_objects"))
        return 0;
    const CCustomMonster* monster = smart_cast<const CCustomMonster*>(FindOnline(id));
    // The list belongs to the engine group of the NPC: a member outside a group has none (Lua would crash there)
    if (!monster || !monster->memory().visual().has_objects())
        return 0;
    u32 count = 0;
    for (const MemorySpace::CVisibleObject& visible : monster->memory().visual().objects())
    {
        // An entry may outlive its object until the visual memory drops it: object_id reads the id without
        // trusting the pointer, and the entry counts only while that id names the same online object (the check
        // of SRemoveOfflinePredicate in the visual memory manager)
        const CGameObject* object = FindOnline(object_id(visible.m_object));
        if (!object || object != visible.m_object)
            continue;
        if (out && count < max)
            out[count] = object->ID();
        ++count;
    }
    return count;
}

int GWP_CALL ApiNpcRelation(GwpObjectId id, GwpObjectId other)
{
    if (!CheckNpcCall("npc", "npc_relation"))
        return -1;
    // GetRelationType logs a script error unless both are creatures: checked here, without the log
    CEntityAlive* npc = smart_cast<CEntityAlive*>(FindOnline(id));
    CEntityAlive* who = npc ? smart_cast<CEntityAlive*>(FindOnline(other)) : nullptr;
    if (!who)
        return -1;
    const ALife::ERelationType relation = npc->lua_game_object()->GetRelationType(who->lua_game_object());
    return relation == ALife::eRelationTypeDummy ? -1 : static_cast<int>(relation);
}

int GWP_CALL ApiNpcWounded(GwpObjectId id)
{
    if (!CheckNpcCall("npc", "npc_wounded"))
        return 0;
    // A stalker only: wounded() logs a script error for anything else
    const CAI_Stalker* stalker = smart_cast<const CAI_Stalker*>(FindOnline(id));
    return stalker && stalker->wounded() ? 1 : 0;
}

GwpObjectId GWP_CALL ApiNpcBestDangerDependent(GwpObjectId id)
{
    CScriptGameObject* npc = FindNpc(id, "npc_best_danger_dependent");
    // GetBestDanger drops a danger whose objects no longer resolve in the level registry, so the pointer is live
    const CDangerObject* danger = npc ? npc->GetBestDanger() : nullptr;
    const IGameObject* dependent = danger ? danger->dependent_object() : nullptr;
    const CGameObject* object = dependent ? FindOnline(dependent->ID()) : nullptr;
    return object && object == dependent ? object->ID() : GWP_INVALID_OBJECT_ID;
}

// npc.health / npc.psy_health: -1 for anything but a creature, as the Lua properties answer (with a script error)
float GWP_CALL ApiNpcHealth(GwpObjectId id)
{
    if (!CheckNpcCall("npc", "npc_health"))
        return -1.f;
    const CEntityAlive* alive = smart_cast<const CEntityAlive*>(FindOnline(id));
    return alive ? alive->conditions().GetHealth() : -1.f;
}

float GWP_CALL ApiNpcPsyHealth(GwpObjectId id)
{
    if (!CheckNpcCall("npc", "npc_psy_health"))
        return -1.f;
    const CEntityAlive* alive = smart_cast<const CEntityAlive*>(FindOnline(id));
    return alive ? alive->conditions().GetPsyHealth() : -1.f;
}

uint32_t GWP_CALL ApiItemState(GwpObjectId id)
{
    if (!CheckNpcCall("inventory", "item_state"))
        return 65535;
    CGameObject* object = FindOnline(id);
    return object ? object->lua_game_object()->GetState() : 65535;
}

uint32_t GWP_CALL ApiItemAnimationSlot(GwpObjectId id)
{
    if (!CheckNpcCall("inventory", "item_animation_slot"))
        return 0xFFFFFFFFu;
    // A hud item only: animation_slot logs a script error for anything else
    CHudItem* item = smart_cast<CHudItem*>(FindOnline(id)); // animation_slot is not const
    return item ? static_cast<uint32_t>(item->animation_slot()) : 0xFFFFFFFFu;
}

const char* GWP_CALL ApiObjectVisual(GwpObjectId id)
{
    if (!CheckNpcCall("objects", "object_visual"))
        return nullptr;
    const CGameObject* object = FindOnline(id);
    return object ? object->cNameVisual().c_str() : nullptr;
}

int GWP_CALL ApiObjectBonePosition(GwpObjectId id, const char* bone, float out_xyz[3])
{
    if (!CheckNpcCall("objects", "object_bone_position") || !out_xyz)
        return 0;
    CGameObject* object = FindOnline(id);
    IKinematics* kinematics = object ? smart_cast<IKinematics*>(object->Visual()) : nullptr;
    if (!kinematics)
        return 0;
    // bone_position of Lua: no name is the root bone; it does not check the skeleton or the bone id, both are here
    const u16 bone_id = bone && bone[0] ? kinematics->LL_BoneID(bone) : kinematics->LL_GetBoneRoot();
    if (bone_id == BI_NONE || bone_id >= kinematics->LL_BoneCount())
        return 0;
    Fmatrix matrix;
    matrix.mul_43(object->XFORM(), kinematics->LL_GetBoneInstance(bone_id).mTransform);
    out_xyz[0] = matrix.c.x;
    out_xyz[1] = matrix.c.y;
    out_xyz[2] = matrix.c.z;
    return 1;
}

GwpObjectId GWP_CALL ApiObjectParent(GwpObjectId id)
{
    if (!CheckNpcCall("objects", "object_parent"))
        return GWP_INVALID_OBJECT_ID;
    const CGameObject* object = FindOnline(id);
    const CGameObject* parent = object ? smart_cast<const CGameObject*>(object->H_Parent()) : nullptr;
    return parent && !parent->getDestroy() ? parent->ID() : GWP_INVALID_OBJECT_ID;
}

uint32_t GWP_CALL ApiObjectDeathTime(GwpObjectId id)
{
    if (!CheckNpcCall("objects", "object_death_time"))
        return 0;
    // death_time of Lua: Device.dwTimeGlobal at the death (level_time_ms), 0 while alive; a script error and 0
    // for anything but an entity
    const CEntity* entity = smart_cast<const CEntity*>(FindOnline(id));
    return entity ? entity->GetLevelDeathTime() : 0;
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group alife)
// ---------------------------------------------------------------------------------------------

CSE_Abstract* FindServerObject(GwpObjectId id, pcstr function)
{
    if (!CheckNpcCall("alife", function) || id == GWP_INVALID_OBJECT_ID || !ai().get_alife())
        return nullptr;
    return ai().alife().objects().object(id, true); // true: no fatal error for a missing id
}

GwpObjectId GWP_CALL ApiAlifeObjectSquad(GwpObjectId id)
{
    const auto* monster = smart_cast<CSE_ALifeMonsterAbstract*>(FindServerObject(id, "alife_object_squad"));
    return monster && monster->m_group_id != ALife::_OBJECT_ID(-1) ? monster->m_group_id : GWP_INVALID_OBJECT_ID;
}

GwpObjectId GWP_CALL ApiAlifeSquadCommander(GwpObjectId squad)
{
    auto* group = smart_cast<CSE_ALifeOnlineOfflineGroup*>(FindServerObject(squad, "alife_squad_commander"));
    const ALife::_OBJECT_ID commander = group ? group->commander_id() : ALife::_OBJECT_ID(-1); // -1: no members
    return commander != ALife::_OBJECT_ID(-1) ? commander : GWP_INVALID_OBJECT_ID;
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group alife): the squad fields of the simulation. The squads of the mod are script classes
// (sim_squad_scripted), their state lives as Lua fields of the server object: Lua stays the only writer, these read
// the live values on demand, as fresh as the dot in Lua and without any synchronization.
// ---------------------------------------------------------------------------------------------

// The Lua instance of a script server object (sim_squad_scripted, se_smart_terrain, ...): luabind keeps it in the
// wrapper the script class factory created, the same reference the engine's own virtual dispatch reads (the
// dynamic_cast to the wrapper base is the way of luabind itself, luabind/back_reference.hpp). Pushes it onto the
// Lua stack and returns the state; pushes nothing and returns nullptr for an object of a pure C++ class (no Lua
// fields to read).
lua_State* PushScriptSelf(CSE_Abstract* object)
{
    luabind::wrap_base* base = object ? dynamic_cast<luabind::wrap_base*>(object) : nullptr;
    if (!base)
        return nullptr;
    const luabind::weak_ref& self = luabind::detail::wrap_access::ref(*base);
    lua_State* L = self.state();
    if (!L)
        return nullptr;
    self.get(L); // exactly one value: the table of the instance
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 1); // the wrapper of a class the script state lost: no Lua side to read
        return nullptr;
    }
    return L;
}

// A numeric field of the Lua instance, read as the dot in Lua reads it (the class metatables are in the chain).
// False when there is no instance or the field is not a number (nil included): the caller answers its own "no".
bool ScriptSelfNumber(CSE_Abstract* object, pcstr field, double& out)
{
    lua_State* L = object ? PushScriptSelf(object) : nullptr;
    if (!L)
        return false;
    const int top = lua_gettop(L);
    lua_pushstring(L, field);
    lua_gettable(L, -2);
    const bool result = lua_isnumber(L, -1) != 0;
    if (result)
        out = lua_tonumber(L, -1);
    lua_settop(L, top - 1); // top was captured AFTER the self push of PushScriptSelf: pop the self too
    return result;
}

// A field of the Lua instance with the truthiness of Lua (nil and false are the falses, everything else is true).
bool ScriptSelfTruthy(CSE_Abstract* object, pcstr field)
{
    lua_State* L = object ? PushScriptSelf(object) : nullptr;
    if (!L)
        return false;
    const int top = lua_gettop(L);
    lua_pushstring(L, field);
    lua_gettable(L, -2);
    const bool result = lua_toboolean(L, -1) != 0;
    lua_settop(L, top - 1); // top was captured AFTER the self push of PushScriptSelf: pop the self too
    return result;
}

// squad.<field> read as an object id: 0 is the actor and valid; GWP_INVALID_OBJECT_ID for nil and anything else
GwpObjectId ApiSquadFieldId(GwpObjectId squad, pcstr function, pcstr field)
{
    double id = 0.;
    if (!ScriptSelfNumber(FindServerObject(squad, function), field, id))
        return GWP_INVALID_OBJECT_ID;
    return id >= 0. && id < static_cast<double>(GWP_INVALID_OBJECT_ID) ? static_cast<GwpObjectId>(id)
                                                                       : GWP_INVALID_OBJECT_ID;
}

GwpObjectId GWP_CALL ApiAlifeSquadAssignedTarget(GwpObjectId squad)
{
    return ApiSquadFieldId(squad, "alife_squad_assigned_target", "assigned_target_id");
}

GwpObjectId GWP_CALL ApiAlifeSquadCurrentTarget(GwpObjectId squad)
{
    return ApiSquadFieldId(squad, "alife_squad_current_target", "current_target_id");
}

GwpObjectId GWP_CALL ApiAlifeSquadSmart(GwpObjectId squad)
{
    return ApiSquadFieldId(squad, "alife_squad_smart", "smart_id");
}

int32_t GWP_CALL ApiAlifeSquadCurrentAction(GwpObjectId squad)
{
    double value = 0.;
    return ScriptSelfNumber(FindServerObject(squad, "alife_squad_current_action"), "current_action", value)
        ? static_cast<int32_t>(value)
        : INT32_MIN; // nil (and anything not a number): the "no action" the plugin reads as nil
}

GwpObjectId GWP_CALL ApiAlifeSquadScriptTarget(GwpObjectId squad)
{
    CSE_Abstract* object = FindServerObject(squad, "alife_squad_script_target");
    lua_State* L = object ? PushScriptSelf(object) : nullptr;
    if (!L)
        return GWP_INVALID_OBJECT_ID;
    const int top = lua_gettop(L);
    GwpObjectId result = GWP_INVALID_OBJECT_ID; // the nil the method answers
    lua_pushstring(L, "get_script_target");
    lua_gettable(L, -2);
    if (lua_isfunction(L, -1))
    {
        lua_pushvalue(L, -2); // the instance as the self argument
        // The Lua call itself, every time: the method draws from the shared math.random of the save, the number of
        // calls is the parity that keeps the stream of the Lua mode
        if (lua_pcall(L, 1, 1, 0) != 0)
        {
            const pcstr message = lua_isstring(L, -1) ? lua_tostring(L, -1) : "error without a message";
            Msg("! [alife] alife_squad_script_target: get_script_target: %s", message);
        }
        else if (lua_isnumber(L, -1))
        {
            const double id = lua_tonumber(L, -1);
            if (id >= 0. && id < static_cast<double>(GWP_INVALID_OBJECT_ID))
                result = static_cast<GwpObjectId>(id); // 0 is the actor and valid
        }
    }
    lua_settop(L, top - 1); // top was captured AFTER the self push of PushScriptSelf: pop the self too
    return result;
}

int GWP_CALL ApiAlifeSquadAlwaysArrived(GwpObjectId squad)
{
    return ScriptSelfTruthy(FindServerObject(squad, "alife_squad_always_arrived"), "always_arrived") ? 1 : 0;
}

int GWP_CALL ApiAlifeSquadIsMonster(GwpObjectId squad)
{
    CSE_Abstract* object = FindServerObject(squad, "alife_squad_is_monster");
    lua_State* L = object ? PushScriptSelf(object) : nullptr;
    if (!L)
        return 0;
    const int top = lua_gettop(L);
    int result = 0;
    lua_pushstring(L, "player_id");
    lua_gettable(L, -2);
    if (lua_isstring(L, -1))
    {
        lua_getglobal(L, "is_squad_monster"); // the table _g.script fills at start
        if (lua_istable(L, -1))
        {
            lua_pushvalue(L, -2); // the player id, as the key
            lua_gettable(L, -2);
            result = lua_toboolean(L, -1); // Lua truthiness of the value the table holds
        }
    }
    lua_settop(L, top - 1); // top was captured AFTER the self push of PushScriptSelf: pop the self too
    return result;
}

uint32_t GWP_CALL ApiAlifeSquadMembers(GwpObjectId squad, GwpObjectId* out, uint32_t max)
{
    const auto* group = smart_cast<const CSE_ALifeOnlineOfflineGroup*>(FindServerObject(squad, "alife_squad_members"));
    if (!group)
        return 0;
    const auto& members = group->squad_members();
    uint32_t written = 0;
    for (const auto& member : members)
    {
        if (!out || written >= max)
            break;
        out[written++] = member.first;
    }
    return static_cast<uint32_t>(members.size());
}

uint32_t GWP_CALL ApiAlifeSquadNpcCount(GwpObjectId squad)
{
    auto* group = smart_cast<CSE_ALifeOnlineOfflineGroup*>(FindServerObject(squad, "alife_squad_npc_count"));
    return group ? static_cast<uint32_t>(group->npc_count()) : 0;
}

GwpObjectId GWP_CALL ApiAlifeObjectSmart(GwpObjectId id)
{
    const auto* monster = smart_cast<const CSE_ALifeMonsterAbstract*>(FindServerObject(id, "alife_object_smart"));
    // u16(-1) is MAX_COUNT_ID_SIZE of the scripts: no smart terrain
    return monster && monster->m_smart_terrain_id != ALife::_OBJECT_ID(-1) ? monster->m_smart_terrain_id
                                                                           : GWP_INVALID_OBJECT_ID;
}

int GWP_CALL ApiAlifeObjectPosition(GwpObjectId id, float out_xyz[3])
{
    if (out_xyz)
        out_xyz[0] = out_xyz[1] = out_xyz[2] = 0.f;
    CSE_Abstract* object = FindServerObject(id, "alife_object_position");
    if (!object || !out_xyz)
        return 0;
    const Fvector& position = object->Position();
    out_xyz[0] = position.x;
    out_xyz[1] = position.y;
    out_xyz[2] = position.z;
    return 1;
}

uint32_t GWP_CALL ApiAlifeObjectGameVertex(GwpObjectId id)
{
    const auto* alife = smart_cast<const CSE_ALifeObject*>(FindServerObject(id, "alife_object_game_vertex"));
    // m_tGraphID is a u16: its "no vertex" is 0xFFFF, the API promises 0xFFFFFFFF for both "no object" and
    // "no AI location"
    if (!alife || alife->m_tGraphID == GameGraph::_GRAPH_ID(-1))
        return 0xFFFFFFFFu;
    return static_cast<uint32_t>(alife->m_tGraphID);
}

uint32_t GWP_CALL ApiAlifeObjectLevelVertex(GwpObjectId id)
{
    const auto* alife = smart_cast<const CSE_ALifeObject*>(FindServerObject(id, "alife_object_level_vertex"));
    return alife ? alife->m_tNodeID : 0xFFFFFFFFu;
}

int32_t GWP_CALL ApiAlifeObjectLevelId(GwpObjectId id)
{
    const auto* alife = smart_cast<const CSE_ALifeObject*>(FindServerObject(id, "alife_object_level_id"));
    const CGameGraph* graph = ai().get_game_graph();
    if (!alife || !graph || alife->m_tGraphID >= graph->header().vertex_count())
        return -1; // Lua errors on a bad vertex: answered as "no level" without the error
    return static_cast<int32_t>(graph->vertex(alife->m_tGraphID)->level_id());
}

int GWP_CALL ApiAlifeObjectClsid(GwpObjectId id, char* out, uint32_t cap)
{
    // The fourcc of the class id, the CLSID2TEXT form (8 characters): the stable class identity, the same bytes
    // the saves hold ("SMRTTRRN" - the script smart terrain, "ON_OFF_S" - a squad, "S_ACTOR" - the actor). The
    // script class names of class_registrator.script are private to the object factory; the fourcc names the same
    // classes one to one.
    if (out && cap)
        out[0] = '\0';
    CSE_Abstract* object = FindServerObject(id, "alife_object_clsid");
    if (!object || !out || cap < 9)
        return 0;
    CLSID2TEXT(object->m_tClassID, out); // writes exactly 8 characters and the terminator
    return 1;
}

float GWP_CALL ApiAlifeSmartArriveDist(GwpObjectId smart)
{
    double value = 0.;
    return ScriptSelfNumber(FindServerObject(smart, "alife_smart_arrive_dist"), "arrive_dist", value)
        ? static_cast<float>(value)
        : -1.f; // -1.f: no such object or field (a smart of the mod always has one: its class reads it from ltx)
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group script)
// ---------------------------------------------------------------------------------------------

constexpr u32 kMaxScriptDepth = 16;
constexpr u32 kMaxErrorsPerFunction = 5;
u32 g_script_depth = 0;
xr_string* g_script_result = nullptr; // the text of the last string result
xr_unordered_map<xr_string, u32>* g_script_errors = nullptr;

// A Lua function called by name: its registry ref in the current Lua state and the counters of script_call_list.
// The counters outlive a restart of the Lua state (new game, load): only the ref is dropped then.
struct ScriptFunction
{
    int ref = LUA_NOREF; // LUA_NOREF: not resolved in this Lua state yet (or not found last time)
    u64 calls = 0;
    u64 errors = 0;
    u64 ticks = 0; // CPU::QPC ticks inside the Lua call, nested script_call of it included
};
xr_unordered_map<xr_string, ScriptFunction>* g_script_functions = nullptr; // function name -> function
u32 g_script_stats_since_ms = 0; // Device.dwTimeGlobal of the last reset of the counters

void ResetScriptCache()
{
    if (g_script_functions)
    {
        for (auto& [name, function] : *g_script_functions)
            function.ref = LUA_NOREF; // the refs die with the old state; the counters stay
    }
    if (g_script_errors)
        g_script_errors->clear();
}

// The entry of the function by name (created on the first call, a missing function included: its failed calls
// are counted too). The pointer stays valid: an unordered map does not move its elements on a rehash.
ScriptFunction& FunctionEntry(pcstr name)
{
    if (!g_script_functions)
        g_script_functions = xr_new<xr_unordered_map<xr_string, ScriptFunction>>();
    return (*g_script_functions)[name];
}

// The function by name, cached in the registry of the main state; pushes it and returns true, or pushes nothing
// and returns false. The refs are dropped for every new Lua state (CNpcScriptCache below): a restarted state may
// get the address of the old one, so the pointer alone does not tell them apart.
bool PushFunction(lua_State* L, pcstr name, ScriptFunction& entry)
{
    if (entry.ref != LUA_NOREF)
    {
        lua_rawgeti(L, LUA_REGISTRYINDEX, entry.ref);
        return true;
    }
    luabind::object function;
    // function_object loads the script of the namespace when needed, the same path as the engine's own calls.
    // A miss is not remembered: a function declared later (on_game_start) is found by the next call
    if (!GEnv.ScriptEngine->function_object(name, function, LUA_TFUNCTION))
        return false;
    function.push(L);
    lua_pushvalue(L, -1);
    entry.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return true;
}

void LogScriptError(const GwpPlugin* self, pcstr function, pcstr message)
{
    if (!g_script_errors)
        g_script_errors = xr_new<xr_unordered_map<xr_string, u32>>();
    u32& count = (*g_script_errors)[function];
    if (++count > kMaxErrorsPerFunction)
        return;
    Msg("! [plugin:%s] script_call '%s': %s%s", PluginAddonId(self), function, message,
        count == kMaxErrorsPerFunction ? " (further errors of this function are not logged)" : "");
}

GwpResult GWP_CALL ApiScriptCall(
    const GwpPlugin* self, const char* function, uint32_t argc, const GwpValue* argv, GwpValue* result)
{
    if (result)
        *result = events::Nil();
    if (!CheckNpcCall("script", "script_call"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    // The name goes into 256-byte buffers of the script engine (parse_script_namespace), checked by VERIFY only
    if (!self || !function || !function[0] || xr_strlen(function) >= 256 || (argc && !argv))
        return GWP_ERROR_INVALID_ARGUMENT;
    if (!GEnv.ScriptEngine || !GEnv.ScriptEngine->lua())
        return GWP_ERROR;
    if (g_script_depth >= kMaxScriptDepth)
    {
        LogScriptError(self, function, "calls nested too deep");
        return GWP_ERROR;
    }

    // The coroutine of a Lua emit when a handler of it calls us, the main state otherwise (the registry is shared).
    // The thread of the plugins.call that led here, else the coroutine of a Lua emit, else the main state (the
    // registry is shared by all of them)
    lua_State* L = CallingLuaThread();
    if (!L)
        L = events::ActiveLuaThread();
    if (!L)
        L = GEnv.ScriptEngine->lua();
    if (argc > 64 || !lua_checkstack(L, static_cast<int>(argc) + 4))
    {
        LogScriptError(self, function, "too many arguments");
        return GWP_ERROR;
    }
    const int top = lua_gettop(L);
    ScriptFunction& entry = FunctionEntry(function);
    ++entry.calls;
    if (!PushFunction(L, function, entry))
    {
        ++entry.errors;
        LogScriptError(self, function, "no such function");
        return GWP_ERROR;
    }
    for (uint32_t i = 0; i < argc; ++i)
        events::PushLuaValue(L, argv[i]); // GWP_T_OBJECT becomes the game object, as for event arguments
    ++g_script_depth;
    ZoneScopedN("plugin/script_call"); // Tracy: the Lua function a plugin calls, text = its name
    ZoneTextF("%s", function);
    const u64 start = CPU::QPC();
    const int status = lua_pcall(L, static_cast<int>(argc), 1, 0);
    entry.ticks += CPU::QPC() - start;
    --g_script_depth;
    if (status != 0)
    {
        ++entry.errors;
        const pcstr message = lua_isstring(L, -1) ? lua_tostring(L, -1) : "error without a message";
        LogScriptError(self, function, message);
        lua_settop(L, top);
        return GWP_ERROR;
    }
    if (result)
    {
        GwpValue value = events::LuaToValue(L, -1);
        if (value.type == GWP_T_STRING)
        {
            // the Lua string goes away with the stack slot: keep a copy until the next call
            if (!g_script_result)
                g_script_result = xr_new<xr_string>();
            g_script_result->assign(value.u.s.ptr ? value.u.s.ptr : "", value.u.s.ptr ? value.u.s.len : 0);
            value.u.s.ptr = g_script_result->c_str();
        }
        *result = value;
    }
    lua_settop(L, top);
    return GWP_OK;
}

// The talk calls of the Lua meet scripts (xr_meet), one engine method each: CScriptGameObject::EnableTalk /
// DisableTalk / StopTalk / IsTalkEnabled / SetTipText; an object that is not an inventory owner is skipped silently,
// as by the Lua methods
void GWP_CALL ApiNpcEnableTalk(GwpObjectId id)
{
    if (!CheckNpcCall("npc", "npc_enable_talk"))
        return;
    CInventoryOwner* owner = smart_cast<CInventoryOwner*>(FindOnline(id));
    if (owner)
        owner->EnableTalk();
}

void GWP_CALL ApiNpcDisableTalk(GwpObjectId id)
{
    if (!CheckNpcCall("npc", "npc_disable_talk"))
        return;
    CInventoryOwner* owner = smart_cast<CInventoryOwner*>(FindOnline(id));
    if (owner)
        owner->DisableTalk();
}

void GWP_CALL ApiNpcStopTalk(GwpObjectId id)
{
    if (!CheckNpcCall("npc", "npc_stop_talk"))
        return;
    CInventoryOwner* owner = smart_cast<CInventoryOwner*>(FindOnline(id));
    if (owner)
        owner->StopTalk();
}

int GWP_CALL ApiNpcIsTalkEnabled(GwpObjectId id)
{
    if (!CheckNpcCall("npc", "npc_is_talk_enabled"))
        return 0;
    CInventoryOwner* owner = smart_cast<CInventoryOwner*>(FindOnline(id));
    return owner && owner->IsTalkEnabled() ? 1 : 0;
}

void GWP_CALL ApiNpcSetTipText(GwpObjectId id, const char* text)
{
    if (!CheckNpcCall("npc", "npc_set_tip_text"))
        return;
    CGameObject* object = FindOnline(id);
    if (object)
        object->set_tip_text(text ? text : "");
}
} // namespace

// Registered through the script export list: called for every new Lua state, before any script is loaded.
// Not in the anonymous namespace: the export node must have a normal, always-emitted definition.
struct CNpcScriptCache
{
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

void CNpcScriptCache::script_register(lua_State* /*L*/) { ResetScriptCache(); }

void FillNpcApi(GwpEngineApi& api)
{
    api.npc_best_enemy = &ApiNpcBestEnemy;
    api.npc_current_enemy = &ApiNpcCurrentEnemy;
    api.npc_best_danger = &ApiNpcBestDanger;
    api.npc_memory_time = &ApiNpcMemoryTime;
    api.npc_memory_position = &ApiNpcMemoryPosition;
    api.npc_community = &ApiNpcCommunity;
    api.npc_active_item = &ApiNpcActiveItem;
    api.npc_feel_touch = &ApiNpcFeelTouch;
    api.object_visual = &ApiObjectVisual;
    api.alife_object_squad = &ApiAlifeObjectSquad;
    api.alife_squad_commander = &ApiAlifeSquadCommander;
    api.alife_squad_assigned_target = &ApiAlifeSquadAssignedTarget;
    api.alife_squad_current_target = &ApiAlifeSquadCurrentTarget;
    api.alife_squad_smart = &ApiAlifeSquadSmart;
    api.alife_squad_current_action = &ApiAlifeSquadCurrentAction;
    api.alife_squad_script_target = &ApiAlifeSquadScriptTarget;
    api.alife_squad_always_arrived = &ApiAlifeSquadAlwaysArrived;
    api.alife_squad_is_monster = &ApiAlifeSquadIsMonster;
    api.alife_squad_npc_count = &ApiAlifeSquadNpcCount;
    api.alife_squad_members = &ApiAlifeSquadMembers;
    api.alife_object_smart = &ApiAlifeObjectSmart;
    api.alife_object_position = &ApiAlifeObjectPosition;
    api.alife_object_game_vertex = &ApiAlifeObjectGameVertex;
    api.alife_object_level_vertex = &ApiAlifeObjectLevelVertex;
    api.alife_object_level_id = &ApiAlifeObjectLevelId;
    api.alife_object_clsid = &ApiAlifeObjectClsid;
    api.alife_smart_arrive_dist = &ApiAlifeSmartArriveDist;
    api.npc_critically_wounded = &ApiNpcCriticallyWounded;
    api.npc_in_smart_cover = &ApiNpcInSmartCover;
    api.npc_is_talking = &ApiNpcIsTalking;
    api.npc_see = &ApiNpcSee;
    api.npc_main_action = &ApiNpcMainAction;
    api.npc_memory_visible_objects = &ApiNpcMemoryVisibleObjects;
    api.npc_relation = &ApiNpcRelation;
    api.npc_wounded = &ApiNpcWounded;
    api.npc_best_danger_dependent = &ApiNpcBestDangerDependent;
    api.npc_health = &ApiNpcHealth;
    api.npc_psy_health = &ApiNpcPsyHealth;
    api.object_bone_position = &ApiObjectBonePosition;
    api.object_parent = &ApiObjectParent;
    api.object_death_time = &ApiObjectDeathTime;
    api.item_state = &ApiItemState;
    api.item_animation_slot = &ApiItemAnimationSlot;
    api.script_call = &ApiScriptCall;
    api.npc_enable_talk = &ApiNpcEnableTalk;
    api.npc_disable_talk = &ApiNpcDisableTalk;
    api.npc_stop_talk = &ApiNpcStopTalk;
    api.npc_is_talk_enabled = &ApiNpcIsTalkEnabled;
    api.npc_set_tip_text = &ApiNpcSetTipText;
}

void PrintScriptCallList(bool reset)
{
    const u32 now = Device.dwTimeGlobal;
    const float seconds = std::max(0.001f, (now - g_script_stats_since_ms) / 1000.f);
    xr_vector<std::pair<const xr_string*, const ScriptFunction*>> rows;
    if (g_script_functions)
    {
        for (const auto& [name, function] : *g_script_functions)
        {
            if (function.calls)
                rows.emplace_back(&name, &function);
        }
    }
    // The most expensive first: total time, then the number of calls
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        return a.second->ticks != b.second->ticks ? a.second->ticks > b.second->ticks : a.second->calls > b.second->calls;
    });
    Msg("- [script] script_call, plugin -> Lua: %u function(s) over %.1f s", static_cast<u32>(rows.size()), seconds);
    Msg("-   %10s %9s %7s %10s %8s  %s", "calls", "calls/s", "errors", "total_ms", "avg_us", "function");
    const double ms_per_tick = 1000.0 / static_cast<double>(CPU::qpc_freq);
    for (const auto& [name, function] : rows)
    {
        const double total_ms = function->ticks * ms_per_tick;
        Msg("-   %10llu %9.1f %7llu %10.2f %8.2f  %s", static_cast<unsigned long long>(function->calls),
            function->calls / seconds, static_cast<unsigned long long>(function->errors), total_ms,
            total_ms * 1000.0 / static_cast<double>(function->calls), name->c_str());
    }
    if (!reset)
        return;
    if (g_script_functions)
    {
        for (auto& [name, function] : *g_script_functions)
            function.calls = function.errors = function.ticks = 0;
    }
    g_script_stats_since_ms = now;
}

void ShutdownNpcApi()
{
    xr_delete(g_script_result);
    xr_delete(g_script_functions); // the refs die with the Lua state
    xr_delete(g_script_errors);
    g_script_stats_since_ms = 0;
}
} // namespace gw::addons
