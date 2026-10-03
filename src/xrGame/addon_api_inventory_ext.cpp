#include "StdAfx.h"

// Plugin API, groups "items" (an inventory and its items: find, list, move, drop, slots of the actor, attachable items,
// condition, magazine, upgrades) and "object_ext" (zones, a physics impulse, the model of an object). Each function
// does what the Lua method named in gwp_api.h does, through the same engine calls and network events, without the
// script errors Lua logs and without its crashes (a bad slot, an ammo index outside the list, an upgrade of another
// tree are refused here).
// Docs: wiki/doc/plugins/api/items.md, wiki/doc/plugins/api/object_ext.md

#include "addon_api_inventory_ext.h"
#include "addon_api_common.h"

#include "Actor.h"
#include "CharacterPhysicsSupport.h"
#include "Include/xrRender/Kinematics.h"
#include "Inventory.h"
#include "InventoryBox.h"
#include "InventoryOwner.h"
#include "Level.h"
#include "PHMovementControl.h"
#include "PhysicsShellHolder.h"
#include "Weapon.h"
#include "ai_space.h"
#include "alife_object_registry.h"
#include "alife_simulator.h"
#include "attachable_item.h"
#include "entity_alive.h"
#include "inventory_item.h"
#include "inventory_upgrade_manager.h"
#include "inventory_upgrade_root.h"
#include "space_restrictor.h"
#include "xrPhysics/IPHWorld.h"
#include "xrPhysics/PhysicsShell.h"
#include "xrServerEntities/inventory_space.h"
#include "xrServerEntities/xrServer_Objects_Abstract.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gw::addons::items
{
namespace
{
constexpr pcstr kItemsGroup = "items";
constexpr pcstr kObjectExtGroup = "object_ext";

// A reading function: the main thread only (no plugin handle to name)
bool ItemsReadAllowed(pcstr function, pcstr group = kItemsGroup)
{
    if (IsMainThread())
        return true;
    Msg("! [%s] %s called outside the main thread, ignored", group, function);
    return false;
}

// Copies a string into (out, cap), cut to cap - 1; the full length is the result
uint32_t ItemsCopyString(pcstr text, char* out, uint32_t cap)
{
    const size_t length = text ? xr_strlen(text) : 0;
    if (out && cap)
    {
        const size_t copied = std::min<size_t>(length, cap - 1);
        if (copied)
            std::memcpy(out, text, copied);
        out[copied] = 0;
    }
    return static_cast<uint32_t>(length);
}

bool ItemsIsFiniteVector(const float* xyz)
{
    return xyz && std::isfinite(xyz[0]) && std::isfinite(xyz[1]) && std::isfinite(xyz[2]);
}

// The online object that directly holds the item (its owner or a box), or nullptr
CGameObject* ItemHolder(CInventoryItem& item)
{
    IGameObject* parent = item.object().H_Parent();
    return parent ? smart_cast<CGameObject*>(parent) : nullptr;
}

// ---------------------------------------------------------------------------------------------
// What an inventory holds
// ---------------------------------------------------------------------------------------------

GwpObjectId GWP_CALL ApiInventoryFind(GwpObjectId owner_id, const char* section)
{
    if (!ItemsReadAllowed("inventory_find") || !section || !*section)
        return GWP_INVALID_OBJECT_ID;
    CGameObject* object = api::FindOnlineObject(owner_id);
    if (!object)
        return GWP_INVALID_OBJECT_ID;
    if (CInventoryOwner* owner = smart_cast<CInventoryOwner*>(object))
    {
        // obj:object(section) of Lua: the first item of m_all with the section
        const PIItem item = owner->inventory().GetItemFromInventory(section);
        return item ? item->object().ID() : GWP_INVALID_OBJECT_ID;
    }
    if (const CInventoryBox* box = smart_cast<CInventoryBox*>(object))
    {
        for (const u16 id : box->m_items)
        {
            const CGameObject* item = api::FindOnlineObject(id);
            if (item && xr_strcmp(item->cNameSect(), section) == 0)
                return id;
        }
    }
    return GWP_INVALID_OBJECT_ID;
}

uint32_t GWP_CALL ApiInventoryItems(GwpObjectId owner_id, GwpObjectId* out, uint32_t max)
{
    if (!ItemsReadAllowed("inventory_items"))
        return 0;
    CGameObject* object = api::FindOnlineObject(owner_id);
    if (!object)
        return 0;
    uint32_t count = 0;
    if (CInventoryOwner* owner = smart_cast<CInventoryOwner*>(object))
    {
        // iterate_inventory of Lua: every item, slots and quest items too
        for (const PIItem item : owner->inventory().m_all)
        {
            if (out && count < max)
                out[count] = item->object().ID();
            ++count;
        }
    }
    else if (const CInventoryBox* box = smart_cast<CInventoryBox*>(object))
    {
        // iterate_inventory_box of Lua: the ids the box keeps, the online ones
        for (const u16 id : box->m_items)
        {
            if (!api::FindOnlineObject(id))
                continue;
            if (out && count < max)
                out[count] = id;
            ++count;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------------------------
// Moving items
// ---------------------------------------------------------------------------------------------

GwpResult GWP_CALL ApiInventoryTransfer(const GwpPlugin* self, GwpObjectId item_id, GwpObjectId from, GwpObjectId to)
{
    constexpr pcstr function = "inventory_transfer";
    if (!api::CheckPluginMutation(self, kItemsGroup, function))
        return api::PluginCallRefused(self);
    CInventoryItem* item = api::FindOnline<CInventoryItem>(item_id);
    if (!item)
        return api::PluginRefusal(self, function, api::MissingObjectCode(item_id), "not an online inventory item");
    CGameObject* source = api::FindOnlineObject(from);
    if (!source)
        return api::PluginRefusal(self, function, api::MissingObjectCode(from), "`from` is not online");
    CGameObject* target = api::FindOnlineObject(to);
    if (!target)
        return api::PluginRefusal(self, function, api::MissingObjectCode(to), "`to` is not online");
    if (ItemHolder(*item) != source)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "the item is not held by `from`");
    // Other holders (the trunk of a car, an anomaly with its artefact) do not handle GE_TRADE_SELL: the server would
    // move the item while the client keeps it in the old holder too
    if (!smart_cast<CInventoryOwner*>(source) && !smart_cast<CInventoryBox*>(source))
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "`from` is no inventory owner or box");
    if (from == to)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "source and target are the same object");
    if (!smart_cast<CInventoryOwner*>(target) && !smart_cast<CInventoryBox*>(target))
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "`to` holds no inventory");
    // transfer_item of Lua: the source lets the item go, the target takes it (the server attaches it to the target
    // only after it is detached, so the order of the two events matters)
    NET_Packet packet;
    CGameObject::u_EventGen(packet, GE_TRADE_SELL, from);
    packet.w_u16(item_id);
    CGameObject::u_EventSend(packet);
    CGameObject::u_EventGen(packet, GE_TRADE_BUY, to);
    packet.w_u16(item_id);
    CGameObject::u_EventSend(packet);
    return GWP_OK;
}

GwpResult GWP_CALL ApiInventoryDrop(const GwpPlugin* self, GwpObjectId owner_id, GwpObjectId item_id,
    const float* position)
{
    constexpr pcstr function = "inventory_drop";
    if (!api::CheckPluginMutation(self, kItemsGroup, function))
        return api::PluginCallRefused(self);
    CInventoryOwner* owner = api::FindOnline<CInventoryOwner>(owner_id);
    if (!owner)
        return api::PluginRefusal(self, function, api::MissingObjectCode(owner_id), "not an online inventory owner");
    CInventoryItem* item = api::FindOnline<CInventoryItem>(item_id);
    if (!item)
        return api::PluginRefusal(self, function, api::MissingObjectCode(item_id), "not an online inventory item");
    if (!ItemHolder(*item) || ItemHolder(*item)->ID() != owner_id)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "the item is not held by `owner`");
    if (position && !ItemsIsFiniteVector(position))
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "a position with NaN or infinity");
    // drop_item of Lua
    NET_Packet packet;
    CGameObject::u_EventGen(packet, GE_OWNERSHIP_REJECT, owner_id);
    packet.w_u16(item_id);
    CGameObject::u_EventSend(packet);
    if (position)
    {
        // drop_item_and_teleport of Lua: the item moves to the position after the drop
        CGameObject::u_EventGen(packet, GE_CHANGE_POS, item_id);
        packet.w_vec3(Fvector().set(position[0], position[1], position[2]));
        CGameObject::u_EventSend(packet);
    }
    return GWP_OK;
}

// ---------------------------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------------------------

uint32_t GWP_CALL ApiInventoryActiveSlot(GwpObjectId owner_id)
{
    if (!ItemsReadAllowed("inventory_active_slot"))
        return GWP_SLOT_NONE;
    CInventoryOwner* owner = api::FindOnline<CInventoryOwner>(owner_id);
    return owner ? owner->inventory().GetActiveSlot() : GWP_SLOT_NONE;
}

GwpResult GWP_CALL ApiInventoryActivateSlot(const GwpPlugin* self, GwpObjectId owner_id, uint32_t slot)
{
    constexpr pcstr function = "inventory_activate_slot";
    if (!api::CheckPluginMutation(self, kItemsGroup, function))
        return api::PluginCallRefused(self);
    CActor* actor = api::FindOnline<CActor>(owner_id);
    if (!actor)
        return api::PluginRefusal(self, function, api::MissingObjectCode(owner_id), "not the actor");
    CInventory& inventory = actor->inventory();
    // CInventory::Activate asserts on a slot past the last one (activate_slot of Lua checks it too). The last slot
    // comes from system.ltx (slot_persistent_N): a mod may add slots above the named GWP_SLOT_* (GlobalWar: 14)
    if (slot > inventory.LastSlot())
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "no such slot");
    inventory.Activate(static_cast<u16>(slot));
    // Activate skips what it cannot do silently: the switch started when the slot is active or next now
    if (inventory.GetActiveSlot() != slot && inventory.GetNextActiveSlot() != slot)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_STATE, "the engine did not start the switch");
    return GWP_OK;
}

// ---------------------------------------------------------------------------------------------
// Items
// ---------------------------------------------------------------------------------------------

GwpResult GWP_CALL ApiItemSetAttachable(const GwpPlugin* self, GwpObjectId item_id, int enable)
{
    constexpr pcstr function = "item_set_attachable";
    if (!api::CheckPluginMutation(self, kItemsGroup, function))
        return api::PluginCallRefused(self);
    CAttachableItem* item = api::FindOnline<CAttachableItem>(item_id);
    if (!item)
        return api::PluginRefusal(self, function, api::MissingObjectCode(item_id), "not an attachable item");
    item->enable(enable != 0); // enable_attachable_item of Lua
    return GWP_OK;
}

int GWP_CALL ApiItemAttachableEnabled(GwpObjectId item_id)
{
    if (!ItemsReadAllowed("item_attachable_enabled"))
        return 0;
    const CAttachableItem* item = api::FindOnline<CAttachableItem>(item_id);
    return item && item->enabled() ? 1 : 0;
}

float GWP_CALL ApiItemCondition(GwpObjectId item_id)
{
    if (!ItemsReadAllowed("item_condition"))
        return -1.f;
    const CInventoryItem* item = api::FindOnline<CInventoryItem>(item_id);
    return item ? item->GetCondition() : -1.f;
}

GwpResult GWP_CALL ApiItemSetCondition(const GwpPlugin* self, GwpObjectId item_id, float condition)
{
    constexpr pcstr function = "item_set_condition";
    if (!api::CheckPluginMutation(self, kItemsGroup, function))
        return api::PluginCallRefused(self);
    CInventoryItem* item = api::FindOnline<CInventoryItem>(item_id);
    if (!item)
        return api::PluginRefusal(self, function, api::MissingObjectCode(item_id), "not an online inventory item");
    if (!std::isfinite(condition))
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "NaN or infinity");
    // set_condition of Lua: the change goes through ChangeCondition, which clamps to 0 .. 1
    item->ChangeCondition(std::clamp(condition, 0.f, 1.f) - item->GetCondition());
    return GWP_OK;
}

int32_t GWP_CALL ApiItemAmmoElapsed(GwpObjectId item_id)
{
    if (!ItemsReadAllowed("item_ammo_elapsed"))
        return -1;
    const CWeapon* weapon = api::FindOnline<CWeapon>(item_id);
    return weapon ? weapon->GetAmmoElapsed() : -1;
}

int32_t GWP_CALL ApiItemMagazineSize(GwpObjectId item_id)
{
    if (!ItemsReadAllowed("item_magazine_size"))
        return -1;
    const CWeapon* weapon = api::FindOnline<CWeapon>(item_id);
    return weapon ? weapon->GetAmmoMagSize() : -1;
}

GwpResult GWP_CALL ApiItemSetAmmoElapsed(const GwpPlugin* self, GwpObjectId item_id, uint32_t count)
{
    constexpr pcstr function = "item_set_ammo_elapsed";
    if (!api::CheckPluginMutation(self, kItemsGroup, function))
        return api::PluginCallRefused(self);
    CWeapon* weapon = api::FindOnline<CWeapon>(item_id);
    if (!weapon)
        return api::PluginRefusal(self, function, api::MissingObjectCode(item_id), "not an online weapon");
    if (count > static_cast<uint32_t>(std::max(weapon->GetAmmoMagSize(), 0)))
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "more than the magazine holds");
    // SetAmmoElapsed loads cartridges of m_ammoTypes[m_ammoType] without a check of the index
    if (count > static_cast<uint32_t>(std::max(weapon->GetAmmoElapsed(), 0)) &&
        weapon->m_ammoType >= weapon->m_ammoTypes.size())
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "the weapon has no current ammo type");
    weapon->SetAmmoElapsed(static_cast<int>(count)); // set_ammo_elapsed of Lua
    return GWP_OK;
}

int32_t GWP_CALL ApiItemAmmoType(GwpObjectId item_id)
{
    if (!ItemsReadAllowed("item_ammo_type"))
        return -1;
    const CWeapon* weapon = api::FindOnline<CWeapon>(item_id);
    return weapon ? weapon->m_ammoType : -1;
}

GwpResult GWP_CALL ApiItemSetAmmoType(const GwpPlugin* self, GwpObjectId item_id, uint32_t type)
{
    constexpr pcstr function = "item_set_ammo_type";
    if (!api::CheckPluginMutation(self, kItemsGroup, function))
        return api::PluginCallRefused(self);
    CWeapon* weapon = api::FindOnline<CWeapon>(item_id);
    if (!weapon)
        return api::PluginRefusal(self, function, api::MissingObjectCode(item_id), "not an online weapon");
    if (type >= weapon->m_ammoTypes.size() || type > 0xFFu)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "an index outside the ammo list");
    weapon->SetAmmoType(static_cast<u8>(type)); // set_ammo_type of Lua: the magazine stays as it is
    return GWP_OK;
}

uint32_t GWP_CALL ApiItemAmmoTypeCount(GwpObjectId item_id)
{
    if (!ItemsReadAllowed("item_ammo_type_count"))
        return 0;
    const CWeapon* weapon = api::FindOnline<CWeapon>(item_id);
    return weapon ? static_cast<uint32_t>(weapon->m_ammoTypes.size()) : 0;
}

uint32_t GWP_CALL ApiItemAmmoSection(GwpObjectId item_id, uint32_t type, char* out, uint32_t cap)
{
    if (out && cap)
        out[0] = 0;
    if (!ItemsReadAllowed("item_ammo_section"))
        return 0;
    const CWeapon* weapon = api::FindOnline<CWeapon>(item_id);
    if (!weapon || type >= weapon->m_ammoTypes.size())
        return 0;
    return ItemsCopyString(weapon->m_ammoTypes[type].c_str(), out, cap);
}

// ---------------------------------------------------------------------------------------------
// Upgrades
// ---------------------------------------------------------------------------------------------

int GWP_CALL ApiItemHasUpgrade(GwpObjectId item_id, const char* upgrade)
{
    if (!ItemsReadAllowed("item_has_upgrade") || !upgrade || !*upgrade || !pSettings->section_exist(upgrade))
        return 0;
    CInventoryItem* item = api::FindOnline<CInventoryItem>(item_id);
    return item && item->has_upgrade(upgrade) ? 1 : 0;
}

uint32_t GWP_CALL ApiItemUpgradeCount(GwpObjectId item_id)
{
    if (!ItemsReadAllowed("item_upgrade_count"))
        return 0;
    CInventoryItem* item = api::FindOnline<CInventoryItem>(item_id);
    return item ? static_cast<uint32_t>(item->get_upgrades().size()) : 0;
}

uint32_t GWP_CALL ApiItemUpgradeAt(GwpObjectId item_id, uint32_t index, char* out, uint32_t cap)
{
    if (out && cap)
        out[0] = 0;
    if (!ItemsReadAllowed("item_upgrade_at"))
        return 0;
    CInventoryItem* item = api::FindOnline<CInventoryItem>(item_id);
    if (!item)
        return 0;
    const CInventoryItem::Upgrades_type upgrades = item->get_upgrades();
    return index < upgrades.size() ? ItemsCopyString(upgrades[index].c_str(), out, cap) : 0;
}

GwpResult GWP_CALL ApiItemInstallUpgrade(const GwpPlugin* self, GwpObjectId item_id, const char* upgrade,
    uint32_t flags)
{
    constexpr pcstr function = "item_install_upgrade";
    if (!api::CheckPluginMutation(self, kItemsGroup, function))
        return api::PluginCallRefused(self);
    if (flags & ~(GWP_UPGRADE_NO_CHECKS | GWP_UPGRADE_CHECK_ONLY))
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "an unknown flag");
    CInventoryItem* item = api::FindOnline<CInventoryItem>(item_id);
    if (!item)
        return api::PluginRefusal(self, function, api::MissingObjectCode(item_id), "not an online inventory item");
    if (!ai().get_alife())
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_STATE, "no game");
    if (!upgrade || !*upgrade || !pSettings->section_exist(upgrade))
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "no such upgrade section");
    // Manager::upgrade_verify only verifies these in a debug build: a release build dereferences null for an item
    // without an upgrade tree or an upgrade the manager does not know
    inventory::upgrade::Manager& manager = ai().alife().inventory_upgrade_manager();
    inventory::upgrade::Root* root = manager.get_root(item->m_section_id);
    if (!root)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "the item has no upgrade tree");
    const shared_str upgrade_id = upgrade;
    if (!manager.get_upgrade(upgrade_id) || !root->contain_upgrade(upgrade_id))
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "the upgrade is not of the item's tree");
    const bool no_checks = (flags & GWP_UPGRADE_NO_CHECKS) != 0;
    const bool possible =
        no_checks ? manager.can_add_upgrade(*item, upgrade_id) : manager.can_install_upgrade(*item, upgrade_id);
    if (!possible)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_STATE, "the upgrade cannot be installed now");
    if (flags & GWP_UPGRADE_CHECK_ONLY)
        return GWP_OK;
    // add_upgrade / install_upgrade of Lua; both send GE_INSTALL_UPGRADE to the server object
    const bool installed =
        no_checks ? manager.upgrade_add(*item, upgrade_id) : manager.upgrade_install(*item, upgrade_id, false);
    return installed ? GWP_OK : api::PluginRefusal(self, function, GWP_ERROR_INVALID_STATE, "the engine refused it");
}

// ---------------------------------------------------------------------------------------------
// Zones (group object_ext)
// ---------------------------------------------------------------------------------------------

int GWP_CALL ApiZoneInside(GwpObjectId zone_id, const float position[3], float radius)
{
    if (!ItemsReadAllowed("zone_inside", kObjectExtGroup) || !ItemsIsFiniteVector(position) || !std::isfinite(radius))
        return 0;
    const CSpaceRestrictor* zone = api::FindOnline<CSpaceRestrictor>(zone_id);
    if (!zone)
        return 0;
    // zone:inside(position) of Lua: a sphere with the radius EPS_L by default
    Fsphere sphere;
    sphere.P.set(position[0], position[1], position[2]);
    sphere.R = radius > 0.f ? radius : EPS_L;
    return zone->inside(sphere) ? 1 : 0;
}

int GWP_CALL ApiObjectInZone(GwpObjectId zone_id, GwpObjectId id)
{
    if (!ItemsReadAllowed("object_in_zone", kObjectExtGroup))
        return 0;
    const CSpaceRestrictor* zone = api::FindOnline<CSpaceRestrictor>(zone_id);
    const CGameObject* object = zone ? api::FindOnlineObject(id) : nullptr;
    if (!object)
        return 0;
    Fsphere sphere;
    sphere.P = object->Position();
    sphere.R = EPS_L;
    return zone->inside(sphere) ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// Physics impulse (group object_ext)
// ---------------------------------------------------------------------------------------------

// The character controller of a living creature, or nullptr
CPHMovementControl* ItemsCreatureMovement(CPhysicsShellHolder& holder)
{
    const CEntityAlive* creature = smart_cast<CEntityAlive*>(&holder);
    if (!creature || !creature->g_Alive())
        return nullptr;
    CCharacterPhysicsSupport* support = holder.character_physics_support();
    CPHMovementControl* movement = support ? support->movement() : nullptr;
    // The actor in a car or at a mounted gun, an invisible poltergeist: the controller is destroyed, the pointer stays
    return movement && movement->CharacterExist() ? movement : nullptr;
}

bool ItemsHasActiveShell(CPhysicsShellHolder& holder)
{
    const CPhysicsShell* shell = holder.PPhysicsShell();
    return shell && shell->isActive();
}

// The physics element that moves the bone: its own or the one of the nearest parent bone with an element; nullptr
// for a shell without a skeleton (a simple box shell of a physics object) - CPHShell::get_PhysicsParrentElement and
// applyImpulseTrace only verify the skeleton
CPhysicsElement* ItemsBoneElement(CPhysicsShell& shell, u16 bone)
{
    IKinematics* kinematics = shell.PKinematics();
    if (!kinematics || bone >= kinematics->LL_BoneCount())
        return nullptr;
    return shell.get_PhysicsParrentElement(bone);
}

// Applies the impulse to the object `id` as it is now; called at once or after the physics step
void ItemsApplyImpulse(GwpObjectId id, Fvector dir, float power, u16 bone)
{
    CPhysicsShellHolder* holder = api::FindOnline<CPhysicsShellHolder>(id);
    if (!holder)
        return; // went offline before the deferred impulse came
    if (ItemsHasActiveShell(*holder))
    {
        CPhysicsShell* shell = holder->PPhysicsShell();
        if (bone == BI_NONE)
        {
            shell->applyImpulse(dir, power); // the root element
            return;
        }
        if (CPhysicsElement* element = ItemsBoneElement(*shell, bone))
            element->applyImpulse(dir, power);
        return;
    }
    if (CPHMovementControl* movement = ItemsCreatureMovement(*holder))
        movement->ApplyImpulse(dir, power);
}

GwpResult GWP_CALL ApiPhysicsApplyImpulse(const GwpPlugin* self, GwpObjectId id, const float dir[3], float power,
    const char* bone)
{
    constexpr pcstr function = "physics_apply_impulse";
    if (!api::CheckPluginMutation(self, kObjectExtGroup, function))
        return api::PluginCallRefused(self);
    CPhysicsShellHolder* holder = api::FindOnline<CPhysicsShellHolder>(id);
    if (!holder)
        return api::PluginRefusal(self, function, api::MissingObjectCode(id), "not an online physical object");
    if (!ItemsIsFiniteVector(dir) || !std::isfinite(power))
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "NaN or infinity");
    Fvector direction;
    direction.set(dir[0], dir[1], dir[2]);
    const float length = direction.magnitude();
    if (length < EPS)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "a zero direction");
    direction.div(length);
    u16 bone_id = BI_NONE;
    if (bone && *bone)
    {
        IKinematics* kinematics = holder->Visual() ? smart_cast<IKinematics*>(holder->Visual()) : nullptr;
        bone_id = kinematics ? kinematics->LL_BoneID(bone) : BI_NONE;
        if (bone_id == BI_NONE)
            return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "no such bone");
    }
    if (!ItemsHasActiveShell(*holder) && !ItemsCreatureMovement(*holder))
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_STATE, "no active shell, no character controller");
    if (bone_id != BI_NONE && ItemsHasActiveShell(*holder) && !ItemsBoneElement(*holder->PPhysicsShell(), bone_id))
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "no physics element moves the bone");
    // The engine defers its own hits on shells while the physics step runs beside the logic
    // (CCharacterPhysicsSupport::in_Hit): the same here, the object is found again by id after the step
    IPHWorld* world = physics_world();
    if (world && world->ShouldDeferSchedulerPhysicsMutation() &&
        world->defer_scheduler_mutation([id, direction, power, bone_id] { ItemsApplyImpulse(id, direction, power, bone_id); }))
        return GWP_OK;
    ItemsApplyImpulse(id, direction, power, bone_id);
    return GWP_OK;
}

// ---------------------------------------------------------------------------------------------
// The model of an object (group object_ext)
// ---------------------------------------------------------------------------------------------

GwpResult GWP_CALL ApiObjectSetVisual(const GwpPlugin* self, GwpObjectId id, const char* visual)
{
    constexpr pcstr function = "object_set_visual";
    if (!api::CheckPluginMutation(self, kObjectExtGroup, function))
        return api::PluginCallRefused(self);
    if (!visual || !*visual || xr_strlen(visual) + 5 > sizeof(string_path))
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "an empty or too long name");
    string_path file;
    strconcat(file, visual, ".ogf");
    if (!FS.exist("$game_meshes$", file))
        return api::PluginRefusal(self, function, GWP_ERROR_NOT_FOUND, "no such model file");
    CSE_Abstract* server = ai().get_alife() ? ai().alife().objects().object(id, true) : nullptr;
    CSE_Visual* server_visual = server ? smart_cast<CSE_Visual*>(server) : nullptr;
    if (CGameObject* object = api::FindOnlineObject(id))
    {
        if (!object->Visual())
            return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "an object without a model");
        if (CActor* actor = smart_cast<CActor*>(object))
            actor->ChangeVisual(visual); // bones, animations, physics: the path of an outfit
        else if (smart_cast<CEntityAlive*>(object))
            return api::PluginRefusal(self, function, GWP_ERROR_NOT_SUPPORTED,
                "a creature changes its model only offline (its skeleton is cached by its managers)");
        else if (CPhysicsShellHolder* holder = smart_cast<CPhysicsShellHolder*>(object); holder && holder->PPhysicsShell())
            // cNameVisual_set deletes the old model, the shell keeps its kinematics and bone callbacks (it is rebuilt
            // only for a null visual): a box, a barrel, an item on the ground, a car change their model only offline
            return api::PluginRefusal(self, function, GWP_ERROR_NOT_SUPPORTED,
                "an object with a physics shell changes its model only offline");
        else
            object->cNameVisual_set(visual); // set_visual_name of Lua
        if (server_visual)
            server_visual->set_visual(visual); // what the save keeps and the next spawn loads
        return GWP_OK;
    }
    if (!server)
        return api::PluginRefusal(self, function, ai().get_alife() ? GWP_ERROR_NOT_FOUND : GWP_ERROR_INVALID_STATE,
            "neither online nor in ALife");
    if (!server_visual)
        return api::PluginRefusal(self, function, GWP_ERROR_INVALID_ARGUMENT, "an object without a model");
    server_visual->set_visual(visual);
    return GWP_OK;
}
} // namespace

void FillEngineApi(GwpEngineApi& api)
{
    api.inventory_find = &ApiInventoryFind;
    api.inventory_items = &ApiInventoryItems;
    api.inventory_transfer = &ApiInventoryTransfer;
    api.inventory_drop = &ApiInventoryDrop;
    api.inventory_active_slot = &ApiInventoryActiveSlot;
    api.inventory_activate_slot = &ApiInventoryActivateSlot;
    api.item_set_attachable = &ApiItemSetAttachable;
    api.item_attachable_enabled = &ApiItemAttachableEnabled;
    api.item_condition = &ApiItemCondition;
    api.item_set_condition = &ApiItemSetCondition;
    api.item_ammo_elapsed = &ApiItemAmmoElapsed;
    api.item_magazine_size = &ApiItemMagazineSize;
    api.item_set_ammo_elapsed = &ApiItemSetAmmoElapsed;
    api.item_ammo_type = &ApiItemAmmoType;
    api.item_set_ammo_type = &ApiItemSetAmmoType;
    api.item_ammo_type_count = &ApiItemAmmoTypeCount;
    api.item_ammo_section = &ApiItemAmmoSection;
    api.item_has_upgrade = &ApiItemHasUpgrade;
    api.item_upgrade_count = &ApiItemUpgradeCount;
    api.item_upgrade_at = &ApiItemUpgradeAt;
    api.item_install_upgrade = &ApiItemInstallUpgrade;
    api.zone_inside = &ApiZoneInside;
    api.object_in_zone = &ApiObjectInZone;
    api.physics_apply_impulse = &ApiPhysicsApplyImpulse;
    api.object_set_visual = &ApiObjectSetVisual;
}
} // namespace gw::addons::items
