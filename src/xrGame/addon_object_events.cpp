#include "StdAfx.h"

#include "addon_object_events.h"
#include "addon_event_bus.h"

#include "Actor.h"
#include "GameObject.h"
#include "HangingLamp.h"
#include "InventoryBox.h"
#include "ai/stalker/ai_stalker.h"
#include "PhysicObject.h"
#include "ai/monsters/basemonster/base_monster.h"
#include "ai/monsters/poltergeist/poltergeist.h"

#include <iterator>

namespace gw::addons::objevents
{
namespace
{
constexpr pcstr kGroupNames[] = { "actor", "input", "monster", "physic", "npc", "lifecycle" };
static_assert(std::size(kGroupNames) == static_cast<size_t>(EGroup::Count_), "kGroupNames != EGroup");

constexpr u32 Bit(EGroup group) { return 1u << static_cast<u32>(group); }
constexpr u32 kAllGroups = (1u << static_cast<u32>(EGroup::Count_)) - 1;

// Groups whose events the engine emits in this build. Grows with the iterations of stage B (06 sec. 8):
// B-1 actor + input, B-2 monster + physic, B-3 npc, B-4 lifecycle.
constexpr u32 kImplementedGroups =
    Bit(EGroup::Actor) | Bit(EGroup::Input) | Bit(EGroup::Monster) | Bit(EGroup::Physic) | Bit(EGroup::Npc) |
    Bit(EGroup::Lifecycle);

u32 g_enabled_groups = kAllGroups; // gw_event_engine_sources

struct EngineEvent
{
    pcstr name;
    EGroup group;
};

// Events that move from Lua binders to the engine (06 sec. 5, sec. 8). The argument schemas live in axr_main.script
// (intercepts) and describe what is sent now; they change together with the source.
constexpr EngineEvent kEngineEvents[] = {
    // B-1: actor callbacks
    { "actor_on_hit_callback", EGroup::Actor },
    { "actor_on_before_death", EGroup::Actor },
    { "actor_on_item_take", EGroup::Actor },
    { "actor_on_item_drop", EGroup::Actor },
    { "actor_on_item_use", EGroup::Actor },
    { "actor_on_item_take_from_box", EGroup::Actor },
    { "actor_item_to_belt", EGroup::Actor },
    { "actor_item_to_ruck", EGroup::Actor },
    { "actor_item_to_slot", EGroup::Actor },
    { "actor_on_trade", EGroup::Actor },
    { "actor_on_info_callback", EGroup::Actor },
    { "on_key_press", EGroup::Actor },
    { "on_key_release", EGroup::Actor },
    { "on_key_hold", EGroup::Actor },
    { "actor_on_weapon_jammed", EGroup::Actor },
    { "actor_on_weapon_zoom_in", EGroup::Actor },
    { "actor_on_weapon_zoom_out", EGroup::Actor },
    { "actor_on_weapon_reload", EGroup::Actor },
    { "actor_on_weapon_no_ammo", EGroup::Actor },
    { "actor_on_hud_animation_end", EGroup::Actor },
    { "actor_on_torch_enabled", EGroup::Actor },
    { "actor_on_torch_disabled", EGroup::Actor },
    { "actor_on_attach_vehicle", EGroup::Actor },
    { "actor_on_detach_vehicle", EGroup::Actor },
    { "actor_on_use_vehicle", EGroup::Actor },
    { "level_input_on_key_press", EGroup::Input },
    { "on_before_item_use", EGroup::Input },
    // B-2: monsters and physics
    { "monster_on_hit_callback", EGroup::Monster },
    { "monster_on_death_callback", EGroup::Monster },
    { "monster_on_actor_use_callback", EGroup::Monster },
    { "monster_on_net_spawn", EGroup::Monster },
    { "physic_object_on_hit_callback", EGroup::Physic },
    { "physic_object_on_use_callback", EGroup::Physic },
    { "heli_on_hit_callback", EGroup::Physic },
    // B-3: NPC (npc_on_update stays a Lua source until W3)
    { "npc_on_hit_callback", EGroup::Npc },
    { "npc_on_death_callback", EGroup::Npc },
    { "npc_on_use", EGroup::Npc },
    { "npc_on_item_take", EGroup::Npc },
    { "npc_on_item_drop", EGroup::Npc },
    { "npc_on_net_spawn", EGroup::Npc },
    { "npc_on_net_destroy", EGroup::Npc },
    { "npc_on_hear_callback", EGroup::Npc },
    // B-4: actor lifecycle
    { "actor_on_init", EGroup::Lifecycle },
    { "actor_on_reinit", EGroup::Lifecycle },
    { "on_game_load", EGroup::Lifecycle },
    { "actor_on_net_destroy", EGroup::Lifecycle },
    { "actor_on_first_update", EGroup::Lifecycle },
    { "actor_on_update", EGroup::Lifecycle },
    { "actor_on_update_fast", EGroup::Lifecycle },
    { "actor_on_update_slow", EGroup::Lifecycle },
    { "on_level_changing", EGroup::Lifecycle },
};

void Apply() { events::SetActiveEngineGroups(kImplementedGroups & g_enabled_groups); }

void AppendGroups(pstr out, size_t size, u32 mask)
{
    if (mask == 0)
    {
        xr_strcat(out, size, "none");
        return;
    }
    if (mask == kAllGroups)
    {
        xr_strcat(out, size, "all");
        return;
    }
    bool first = true;
    for (u32 i = 0; i < static_cast<u32>(EGroup::Count_); ++i)
    {
        if (!(mask & (1u << i)))
            continue;
        if (!first)
            xr_strcat(out, size, ",");
        xr_strcat(out, size, kGroupNames[i]);
        first = false;
    }
}
} // namespace

void RegisterEngineEvents()
{
    for (const EngineEvent& event : kEngineEvents)
        events::DeclareEngineSource(event.name, static_cast<u32>(event.group));
    Apply();
}

bool SetEnabledGroups(pcstr text)
{
    if (!text)
        return false;
    string256 buffer;
    xr_strcpy(buffer, text);
    _Trim(buffer);
    if (!xr_stricmp(buffer, "all"))
        g_enabled_groups = kAllGroups;
    else if (!xr_stricmp(buffer, "none") || !buffer[0])
        g_enabled_groups = 0;
    else
    {
        u32 mask = 0;
        const u32 count = _GetItemCount(buffer, ',');
        for (u32 i = 0; i < count; ++i)
        {
            string64 name;
            _GetItem(buffer, i, name, ',');
            _Trim(name);
            u32 group = 0;
            while (group < static_cast<u32>(EGroup::Count_) && xr_stricmp(name, kGroupNames[group]) != 0)
                ++group;
            if (group == static_cast<u32>(EGroup::Count_))
            {
                Msg("! [events] gw_event_engine_sources: unknown group '%s' (known: actor, input, monster, physic, "
                    "npc, lifecycle)", name);
                return false;
            }
            mask |= 1u << group;
        }
        g_enabled_groups = mask;
    }
    Apply();
    return true;
}

void GetStatus(pstr out, size_t size)
{
    out[0] = 0;
    AppendGroups(out, size, g_enabled_groups);
    xr_strcat(out, size, " (implemented: ");
    AppendGroups(out, size, kImplementedGroups);
    xr_strcat(out, size, ")");
}

bool IsActive(EGroup group) { return (kImplementedGroups & g_enabled_groups & Bit(group)) != 0; }

// ---------------------------------------------------------------------------------------------
// Emitters
// ---------------------------------------------------------------------------------------------

namespace
{
bool g_actor_spawned = false;

// Id of an event name, resolved once: the bus lives for the whole process (Shutdown only at exit).
#define GW_EVENT_ID(name)                                                    \
    []() {                                                                   \
        static const GwpEventId id = events::Intern(name);                   \
        return id;                                                           \
    }()

bool ActorReady(EGroup group) { return IsActive(group) && g_actor_spawned && g_actor; }

bool IsActor(const IGameObject* object) { return object && g_actor && object == static_cast<const IGameObject*>(g_actor); }

GwpValue ObjectOrNil(const IGameObject* object) { return object ? events::Object(object->ID()) : events::Nil(); }
} // namespace

void SetActorSpawned(bool spawned) { g_actor_spawned = spawned; }

void ActorHit(const CGameObject* actor, float amount, const Fvector& local_dir, const IGameObject* who, s16 bone)
{
    // amount > 0: the Lua binder dropped zero hits
    if (!ActorReady(EGroup::Actor) || !actor || amount <= 0.0f)
        return;
    const GwpValue args[] = { events::Object(actor->ID()), events::Number(amount), events::Vec3(local_dir),
        ObjectOrNil(who), events::Int(bone) };
    events::Emit(GW_EVENT_ID("actor_on_hit_callback"), args, 5);
}

bool ActorBeforeDeath(u16 who_id)
{
    GwpValue result = events::Bool(true);
    const GwpValue args[] = { events::Int(who_id) };
    events::Emit(GW_EVENT_ID("actor_on_before_death"), args, 1, &result);
    return result.type != GWP_T_BOOL || result.u.b != 0;
}

void OwnerItem(EItem kind, const CGameObject* owner, const CGameObject* item)
{
    if (!owner || !item)
        return;
    if (smart_cast<const CAI_Stalker*>(owner))
    {
        // npc_on_item_take / npc_on_item_drop (B-3); the Lua binder had no belt/ruck/slot events for NPCs
        if (!IsActive(EGroup::Npc) || (kind != EItem::Take && kind != EItem::Drop))
            return;
        const GwpValue args[] = { events::Object(owner->ID()), events::Object(item->ID()) };
        events::Emit(kind == EItem::Take ? GW_EVENT_ID("npc_on_item_take") : GW_EVENT_ID("npc_on_item_drop"), args, 2);
        return;
    }
    if (!ActorReady(EGroup::Actor) || !IsActor(owner))
        return;
    GwpEventId id = GWP_INVALID_EVENT_ID;
    switch (kind)
    {
    case EItem::Take: id = GW_EVENT_ID("actor_on_item_take"); break;
    case EItem::Drop: id = GW_EVENT_ID("actor_on_item_drop"); break;
    case EItem::ToBelt: id = GW_EVENT_ID("actor_item_to_belt"); break;
    case EItem::ToRuck: id = GW_EVENT_ID("actor_item_to_ruck"); break;
    case EItem::ToSlot: id = GW_EVENT_ID("actor_item_to_slot"); break;
    }
    const GwpValue args[] = { events::Object(item->ID()) };
    events::Emit(id, args, 1);
}

void ActorItemUse(const CGameObject* item)
{
    if (!ActorReady(EGroup::Actor) || !item)
        return;
    const GwpValue args[] = { events::Object(item->ID()), events::String(item->cNameSect().c_str()) };
    events::Emit(GW_EVENT_ID("actor_on_item_use"), args, 2);
}

void ActorItemTakeFromBox(const CGameObject* box, const CGameObject* item)
{
    if (!ActorReady(EGroup::Actor) || !box || !item)
        return;
    const GwpValue args[] = { events::Object(box->ID()), events::Object(item->ID()) };
    events::Emit(GW_EVENT_ID("actor_on_item_take_from_box"), args, 2);
}

void ActorTrade(const CGameObject* item, bool sell, u32 money)
{
    if (!ActorReady(EGroup::Actor) || !item)
        return;
    const GwpValue args[] = { events::Object(item->ID()), events::Bool(sell), events::Int(money) };
    events::Emit(GW_EVENT_ID("actor_on_trade"), args, 3);
}

void ActorInfo(const CGameObject* actor, pcstr info_id)
{
    if (!ActorReady(EGroup::Actor) || !IsActor(actor))
        return;
    const GwpValue args[] = { events::Object(actor->ID()), events::String(info_id) };
    events::Emit(GW_EVENT_ID("actor_on_info_callback"), args, 2);
}

void ActorKey(EKey kind, int key)
{
    if (!ActorReady(EGroup::Actor))
        return;
    GwpEventId id = GWP_INVALID_EVENT_ID;
    switch (kind)
    {
    case EKey::Press: id = GW_EVENT_ID("on_key_press"); break;
    case EKey::Release: id = GW_EVENT_ID("on_key_release"); break;
    case EKey::Hold: id = GW_EVENT_ID("on_key_hold"); break;
    }
    const GwpValue args[] = { events::Int(key) };
    events::Emit(id, args, 1);
}

void ActorWeapon(EWeapon kind, const IGameObject* owner, const CGameObject* weapon, int ammo_total)
{
    if (!ActorReady(EGroup::Actor) || !IsActor(owner) || !weapon)
        return;
    const GwpValue args[] = { events::Object(weapon->ID()), events::Int(ammo_total) };
    switch (kind)
    {
    case EWeapon::Jammed: events::Emit(GW_EVENT_ID("actor_on_weapon_jammed"), args, 1); break;
    case EWeapon::ZoomIn: events::Emit(GW_EVENT_ID("actor_on_weapon_zoom_in"), args, 1); break;
    case EWeapon::ZoomOut: events::Emit(GW_EVENT_ID("actor_on_weapon_zoom_out"), args, 1); break;
    case EWeapon::Reload: events::Emit(GW_EVENT_ID("actor_on_weapon_reload"), args, 2); break;
    case EWeapon::NoAmmo: events::Emit(GW_EVENT_ID("actor_on_weapon_no_ammo"), args, 2); break;
    }
}

void ActorHudAnimationEnd(const CGameObject* item, pcstr hud_section, pcstr motion, u32 state, u32 slot)
{
    if (!ActorReady(EGroup::Actor) || !item)
        return;
    const GwpValue args[] = { events::Object(item->ID()), events::String(hud_section), events::String(motion),
        events::Int(state), events::Int(slot) };
    events::Emit(GW_EVENT_ID("actor_on_hud_animation_end"), args, 5);
}

void ActorTorch(const CGameObject* torch, bool on)
{
    if (!ActorReady(EGroup::Actor) || !torch)
        return;
    const GwpValue args[] = { events::Object(torch->ID()) };
    events::Emit(on ? GW_EVENT_ID("actor_on_torch_enabled") : GW_EVENT_ID("actor_on_torch_disabled"), args, 1);
}

void ActorVehicle(EVehicle kind, const IGameObject* vehicle)
{
    if (!ActorReady(EGroup::Actor) || !vehicle)
        return;
    const GwpValue args[] = { events::Object(vehicle->ID()) };
    switch (kind)
    {
    case EVehicle::Attach: events::Emit(GW_EVENT_ID("actor_on_attach_vehicle"), args, 1); break;
    case EVehicle::Detach: events::Emit(GW_EVENT_ID("actor_on_detach_vehicle"), args, 1); break;
    case EVehicle::Use: events::Emit(GW_EVENT_ID("actor_on_use_vehicle"), args, 1); break;
    }
}

bool LevelInputKeyPress(int key, int action)
{
    // level.present() and db.actor: the condition of level_input.on_key_press
    if (!ActorReady(EGroup::Input))
        return false;
    GwpValue result = events::Bool(false);
    const GwpValue args[] = { events::Int(key), events::Int(action) };
    events::Emit(GW_EVENT_ID("level_input_on_key_press"), args, 2, &result);
    return result.type == GWP_T_BOOL && result.u.b != 0;
}

bool BeforeItemUse(const CGameObject* owner, const CGameObject* item)
{
    if (!IsActive(EGroup::Input) || !owner || !item)
        return true;
    GwpValue result = events::Bool(true);
    const GwpValue args[] = { events::Object(owner->ID()), events::Object(item->ID()) };
    events::Emit(GW_EVENT_ID("on_before_item_use"), args, 2, &result);
    return result.type != GWP_T_BOOL || result.u.b != 0;
}

// ---------------------------------------------------------------------------------------------
// Monsters and physic objects (B-2)
// ---------------------------------------------------------------------------------------------

namespace
{
bool IsMonster(const CGameObject* object) { return smart_cast<const CBaseMonster*>(object) != nullptr; }
bool IsStalker(const CGameObject* object) { return smart_cast<const CAI_Stalker*>(object) != nullptr; }

bool IsAlive(const CGameObject* object)
{
    const CEntity* entity = smart_cast<const CEntity*>(object);
    return entity && entity->g_Alive();
}

// The Lua actor binder in an event: no native form (GWP_T_LUA_REF); the Lua adapter puts db.actor_binder there.
GwpValue BinderPlaceholder()
{
    GwpValue value = events::Nil();
    value.type = GWP_T_LUA_REF;
    return value;
}

bool g_actor_first_update = false; // actor_on_first_update is due: set by on_game_load, like bCheckStart in Lua

// The Lua binder of physic objects was attached by section (script_binding = bind_physic_object.init): inventory
// boxes (stashes: coc_treasure_manager waits for their use), devices and quest items with it, objects with [logic].
// The engine covers the same set: physic object classes, boxes, and any object with that binder in its section.
bool IsPhysic(const CGameObject* object)
{
    if (smart_cast<const CPhysicObject*>(object) || smart_cast<const CHangingLamp*>(object) ||
        smart_cast<const CInventoryBox*>(object))
        return true;
    const shared_str& section = object->cNameSect();
    return pSettings->line_exist(section, "script_binding") &&
        !xr_strcmp(pSettings->r_string(section, "script_binding"), "bind_physic_object.init");
}
} // namespace

void ObjectHit(const CGameObject* object, float amount, const Fvector& local_dir, const IGameObject* who, s16 bone)
{
    if (!object)
        return;
    GwpEventId id = GWP_INVALID_EVENT_ID;
    if (IsStalker(object))
    {
        // amount > 0: the Lua binder dropped zero hits
        if (!IsActive(EGroup::Npc) || amount <= 0.0f)
            return;
        id = GW_EVENT_ID("npc_on_hit_callback");
    }
    else if (IsMonster(object))
    {
        if (!IsActive(EGroup::Monster) || amount <= 0.0f)
            return;
        id = GW_EVENT_ID("monster_on_hit_callback");
    }
    else if (IsPhysic(object))
    {
        if (!IsActive(EGroup::Physic))
            return;
        id = GW_EVENT_ID("physic_object_on_hit_callback");
    }
    else
        return;
    // bone is always a number: the synthetic "from_death_callback" hit of the Lua binder is not sent (B3.1),
    // death comes as *_on_death_callback.
    const GwpValue args[] = { events::Object(object->ID()), events::Number(amount), events::Vec3(local_dir),
        ObjectOrNil(who), events::Int(bone) };
    events::Emit(id, args, 5);
}

void ObjectDeath(const CGameObject* victim, const IGameObject* who)
{
    if (victim && IsStalker(victim))
    {
        // Every death of an online NPC (B3.2): the Lua binder sent it only for kills by a stalker or a monster.
        if (!IsActive(EGroup::Npc))
            return;
        const GwpValue args[] = { events::Object(victim->ID()), ObjectOrNil(who) };
        events::Emit(GW_EVENT_ID("npc_on_death_callback"), args, 2);
        return;
    }
    // Poltergeists are skipped, like the Lua binder did (their body is released at once).
    if (!IsActive(EGroup::Monster) || !victim || !IsMonster(victim) || smart_cast<const CPoltergeist*>(victim))
        return;
    // who may be nil: death without a killer (the Lua binder failed on who:id() then and sent nothing)
    const GwpValue args[] = { events::Object(victim->ID()), ObjectOrNil(who) };
    events::Emit(GW_EVENT_ID("monster_on_death_callback"), args, 2);
}

void ObjectUse(const CGameObject* object, const IGameObject* user)
{
    if (!object)
        return;
    if (IsStalker(object))
    {
        // npc_on_use: alive NPCs only, like the Lua binder (a dead one is looted through the inventory)
        if (!IsActive(EGroup::Npc) || !IsAlive(object))
            return;
        const GwpValue args[] = { events::Object(object->ID()), ObjectOrNil(user) };
        events::Emit(GW_EVENT_ID("npc_on_use"), args, 2);
        return;
    }
    if (IsMonster(object))
    {
        // The Lua binder set the slot for dead monsters only (to loot them); alive ones use a kick handler.
        const CEntity* entity = smart_cast<const CEntity*>(object);
        if (!IsActive(EGroup::Monster) || !entity || entity->g_Alive())
            return;
        const GwpValue args[] = { events::Object(object->ID()), ObjectOrNil(user) };
        events::Emit(GW_EVENT_ID("monster_on_actor_use_callback"), args, 2);
    }
    else if (IsPhysic(object))
    {
        if (!IsActive(EGroup::Physic))
            return;
        const GwpValue args[] = { events::Object(object->ID()), ObjectOrNil(user) };
        events::Emit(GW_EVENT_ID("physic_object_on_use_callback"), args, 2);
    }
}

void ObjectNetSpawn(const CGameObject* object)
{
    if (!object)
        return;
    const GwpValue args[] = { events::Object(object->ID()), events::ServerObject(object->ID()) };
    if (IsStalker(object))
    {
        // alive NPCs only: the Lua binder returned early for a dead one
        if (IsActive(EGroup::Npc) && IsAlive(object))
            events::Emit(GW_EVENT_ID("npc_on_net_spawn"), args, 2);
    }
    else if (IsMonster(object))
    {
        if (IsActive(EGroup::Monster))
            events::Emit(GW_EVENT_ID("monster_on_net_spawn"), args, 2);
    }
}

void ObjectNetDestroy(const CGameObject* object)
{
    if (!object)
        return;
    if (IsActor(object))
    {
        // actor_on_net_destroy(binder): before the Lua binder, db.actor is still valid (B3.9)
        if (IsActive(EGroup::Lifecycle))
        {
            const GwpValue args[] = { BinderPlaceholder() };
            events::Emit(GW_EVENT_ID("actor_on_net_destroy"), args, 1);
        }
        return;
    }
    if (!IsActive(EGroup::Npc) || !IsStalker(object))
        return;
    const GwpValue args[] = { events::Object(object->ID()) };
    events::Emit(GW_EVENT_ID("npc_on_net_destroy"), args, 1);
}

void ObjectHear(const CGameObject* object, u16 who_id, int sound_type, const Fvector& position, float power)
{
    // The Lua binders ignored sounds of dead listeners and own sounds.
    if (!IsActive(EGroup::Npc) || !object || who_id == object->ID() || !IsAlive(object))
        return;
    // (obj, who_id, sound_type, dist_sqr, power, position). For plugins sound_type is the engine bit mask;
    // Lua subscribers get the name ("WPN_shoot", ...) through the Lua adapter set in axr_main.script (B3.5).
    const GwpValue args[] = { events::Object(object->ID()), events::Int(who_id), events::Int(sound_type),
        events::Number(position.distance_to_sqr(object->Position())), events::Number(power), events::Vec3(position) };
    events::Emit(GW_EVENT_ID("npc_on_hear_callback"), args, 6);
}

bool WantsHear() { return IsActive(EGroup::Npc) && events::HasSubscribers(GW_EVENT_ID("npc_on_hear_callback")); }

void HeliHit(const CGameObject* heli, float damage, float impulse, u32 hit_type, const IGameObject* who)
{
    if (!IsActive(EGroup::Physic) || !heli)
        return;
    // (heli, power, impulse, who, hit_type): the Lua binder sent nil for impulse and hit_type (B3.4)
    const GwpValue args[] = { events::Object(heli->ID()), events::Number(damage), events::Number(impulse),
        ObjectOrNil(who), events::Int(hit_type) };
    events::Emit(GW_EVENT_ID("heli_on_hit_callback"), args, 5);
}

// ---------------------------------------------------------------------------------------------
// Actor lifecycle (B-4)
// ---------------------------------------------------------------------------------------------

void ObjectBinder(EBinder kind, const CGameObject* object)
{
    // CActor::net_Spawn has not finished at these points: check the class, not g_actor.
    if (!object || !smart_cast<const CActor*>(object))
        return;
    // The actor group is live from here on, like db.actor in Lua (set by actor_binder:net_spawn, which just ran):
    // on_game_load handlers give info portions, take items, ... and those events must not be dropped.
    if (kind == EBinder::NetSpawn)
        SetActorSpawned(true);
    if (!IsActive(EGroup::Lifecycle))
        return;
    const GwpValue args[] = { BinderPlaceholder() };
    switch (kind)
    {
    case EBinder::Init: events::Emit(GW_EVENT_ID("actor_on_init"), args, 1); break;
    case EBinder::Reinit: events::Emit(GW_EVENT_ID("actor_on_reinit"), args, 1); break;
    case EBinder::NetSpawn:
        g_actor_first_update = true;
        events::Emit(GW_EVENT_ID("on_game_load"), args, 1);
        break;
    }
}

void ObjectBinderUpdate(const CGameObject* object, u32 dt_ms)
{
    // Every update of the actor binder while the actor is alive (the Lua binder returned early for a dead one).
    if (!IsActive(EGroup::Lifecycle) || !object || !smart_cast<const CActor*>(object) || !IsAlive(object))
        return;
    const GwpValue args[] = { BinderPlaceholder(), events::Number(dt_ms) };
    if (g_actor_first_update)
    {
        g_actor_first_update = false;
        events::Emit(GW_EVENT_ID("actor_on_first_update"), args, 2);
    }
    events::Emit(GW_EVENT_ID("actor_on_update_fast"), args, 2);
    events::Emit(GW_EVENT_ID("actor_on_update_slow"), args, 2);
    events::Emit(GW_EVENT_ID("actor_on_update"), args, 2);
}

void LevelChanging()
{
    if (!IsActive(EGroup::Lifecycle))
        return;
    events::Emit(GW_EVENT_ID("on_level_changing"));
}

#undef GW_EVENT_ID
} // namespace gw::addons::objevents
