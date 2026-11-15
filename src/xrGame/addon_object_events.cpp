#include "StdAfx.h"

#include "addon_object_events.h"
#include "addon_binders.h"
#include "addon_event_bus.h"
#include "addon_goap.h"
#include "addon_goap_planner.h"
#include "addon_host.h"
#include "addon_storage.h"

#include "Actor.h"
#include "GameObject.h"
#include "HangingLamp.h"
#include "InventoryBox.h"
#include "ai/stalker/ai_stalker.h"
#include "ai_sounds.h"

// The sound type bits of gwp_api.h are the engine's ESoundTypes (the mask npc_on_hear_callback passes on)
static_assert(GWP_SOUND_WEAPON == SOUND_TYPE_WEAPON);
static_assert(GWP_SOUND_ITEM == SOUND_TYPE_ITEM);
static_assert(GWP_SOUND_MONSTER == SOUND_TYPE_MONSTER);
static_assert(GWP_SOUND_ANOMALY == SOUND_TYPE_ANOMALY);
static_assert(GWP_SOUND_WORLD == SOUND_TYPE_WORLD);
static_assert(GWP_SOUND_PICKING_UP == SOUND_TYPE_PICKING_UP);
static_assert(GWP_SOUND_DROPPING == SOUND_TYPE_DROPPING);
static_assert(GWP_SOUND_HIDING == SOUND_TYPE_HIDING);
static_assert(GWP_SOUND_TAKING == SOUND_TYPE_TAKING);
static_assert(GWP_SOUND_USING == SOUND_TYPE_USING);
static_assert(GWP_SOUND_SHOOTING == SOUND_TYPE_SHOOTING);
static_assert(GWP_SOUND_EMPTY_CLICKING == SOUND_TYPE_EMPTY_CLICKING);
static_assert(GWP_SOUND_BULLET_HIT == SOUND_TYPE_BULLET_HIT);
static_assert(GWP_SOUND_RECHARGING == SOUND_TYPE_RECHARGING);
static_assert(GWP_SOUND_DYING == SOUND_TYPE_DYING);
static_assert(GWP_SOUND_INJURING == SOUND_TYPE_INJURING);
static_assert(GWP_SOUND_STEP == SOUND_TYPE_STEP);
static_assert(GWP_SOUND_TALKING == SOUND_TYPE_TALKING);
static_assert(GWP_SOUND_ATTACKING == SOUND_TYPE_ATTACKING);
static_assert(GWP_SOUND_EATING == SOUND_TYPE_EATING);
static_assert(GWP_SOUND_IDLE == SOUND_TYPE_IDLE);
static_assert(GWP_SOUND_OBJECT_BREAKING == SOUND_TYPE_OBJECT_BREAKING);
static_assert(GWP_SOUND_OBJECT_COLLIDING == SOUND_TYPE_OBJECT_COLLIDING);
static_assert(GWP_SOUND_OBJECT_EXPLODING == SOUND_TYPE_OBJECT_EXPLODING);
static_assert(GWP_SOUND_AMBIENT == SOUND_TYPE_AMBIENT);
#include "PhysicObject.h"
#include "ai/monsters/basemonster/base_monster.h"
#include "ai/monsters/poltergeist/poltergeist.h"
#include "xrEngine/Environment.h"
#include "xrEngine/IGame_Persistent.h"

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
    { "actor_on_money_change", EGroup::Actor }, // plans/addon_achievements 7.2: no Lua source
    { "actor_on_trade_deal", EGroup::Actor },   // plans/addon_quests: no Lua source
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
    { "level_input_on_mouse_wheel", EGroup::Input }, // D1 (plans/addon_base_building): no Lua source
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
    { "actor_on_frame", EGroup::Lifecycle }, // no Lua source: every frame, unlike the throttled actor_on_update
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
// actor_on_money_change is sent from the end of the first update of the actor binder after its spawn on: the spawn
// (a new game, a loaded save, a level change) sets the money of the saved actor - not a change of the game
bool g_actor_money_live = false;

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

void SetActorSpawned(bool spawned)
{
    g_actor_spawned = spawned;
    g_actor_money_live = false; // a spawn or the end of the actor: live again after its first update
}

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

namespace
{
// The deal being transferred (ActorTradeDealBegin .. ActorTradeDealEnd): the items of actor_on_trade in order
struct TradeDeal
{
    bool active = false;
    u64 money_got = 0;  // the sum of the sold items: the actor got it
    u64 money_paid = 0; // the sum of the bought items: the actor paid it
    xr_vector<u16> sold;
    xr_vector<u16> bought;
    xr_vector<u32> sold_prices;   // the money of actor_on_trade of each sold item, parallel to sold
    xr_vector<u32> bought_prices; // the money of actor_on_trade of each bought item, parallel to bought
};
TradeDeal g_trade_deal;

GwpValue IdArray(const xr_vector<GwpValue>& items)
{
    GwpValue value = events::Nil();
    value.type = GWP_T_ARRAY;
    value.u.a.items = items.empty() ? nullptr : items.data();
    value.u.a.count = static_cast<uint32_t>(items.size());
    return value;
}
} // namespace

void ActorTrade(const CGameObject* item, bool sell, u32 money)
{
    if (!ActorReady(EGroup::Actor) || !item)
        return;
    if (g_trade_deal.active)
    {
        // sell == true: the actor sold the item (CTrade::TransferItem, bDir)
        (sell ? g_trade_deal.sold : g_trade_deal.bought).push_back(item->ID());
        (sell ? g_trade_deal.sold_prices : g_trade_deal.bought_prices).push_back(money);
        (sell ? g_trade_deal.money_got : g_trade_deal.money_paid) += money;
    }
    const GwpValue args[] = { events::Object(item->ID()), events::Bool(sell), events::Int(money) };
    events::Emit(GW_EVENT_ID("actor_on_trade"), args, 3);
}

void ActorTradeDealBegin()
{
    g_trade_deal.active = true;
    g_trade_deal.money_got = 0;
    g_trade_deal.money_paid = 0;
    g_trade_deal.sold.clear();
    g_trade_deal.bought.clear();
    g_trade_deal.sold_prices.clear();
    g_trade_deal.bought_prices.clear();
}

void ActorTradeDealEnd(const CGameObject* partner)
{
    if (!g_trade_deal.active)
        return;
    g_trade_deal.active = false;
    if ((g_trade_deal.sold.empty() && g_trade_deal.bought.empty()) || !ActorReady(EGroup::Actor) || !partner)
        return;
    // an array holds at most kMaxArrayItems values: a longer list is cut (a trade window never moves that many)
    // the price arrays are parallel to the id arrays (the same length, the same order): ActorTrade pushes both
    xr_vector<GwpValue> sold;
    xr_vector<GwpValue> bought;
    xr_vector<GwpValue> sold_prices;
    xr_vector<GwpValue> bought_prices;
    const size_t sold_count = std::min<size_t>(g_trade_deal.sold.size(), events::kMaxArrayItems);
    const size_t bought_count = std::min<size_t>(g_trade_deal.bought.size(), events::kMaxArrayItems);
    sold.reserve(sold_count);
    bought.reserve(bought_count);
    sold_prices.reserve(sold_count);
    bought_prices.reserve(bought_count);
    for (size_t i = 0; i < sold_count; ++i)
    {
        sold.push_back(events::Int(g_trade_deal.sold[i]));
        sold_prices.push_back(events::Int(g_trade_deal.sold_prices[i]));
    }
    for (size_t i = 0; i < bought_count; ++i)
    {
        bought.push_back(events::Int(g_trade_deal.bought[i]));
        bought_prices.push_back(events::Int(g_trade_deal.bought_prices[i]));
    }
    // the prices are appended after the ids: the first five arguments keep their order (plugins and Lua built for
    // the five-argument event still work)
    const GwpValue args[] = { events::Object(partner->ID()), events::Int(static_cast<s64>(g_trade_deal.money_got)),
        events::Int(static_cast<s64>(g_trade_deal.money_paid)), IdArray(sold), IdArray(bought), IdArray(sold_prices),
        IdArray(bought_prices) };
    events::Emit(GW_EVENT_ID("actor_on_trade_deal"), args, static_cast<u32>(std::size(args)));
}

void ActorMoneyChange(u32 old_money, u32 new_money)
{
    if (old_money == new_money || !g_actor_money_live || !ActorReady(EGroup::Actor))
        return;
    // (new balance, signed delta); the delta of two u32 always fits an s64
    const GwpValue args[] = { events::Int(new_money), events::Int(static_cast<s64>(new_money) - old_money) };
    events::Emit(GW_EVENT_ID("actor_on_money_change"), args, 2);
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

bool LevelInputMouseWheel(float y, float x)
{
    // The same condition as level_input_on_key_press: a level with the actor
    if (!ActorReady(EGroup::Input))
        return false;
    GwpValue result = events::Bool(false);
    const GwpValue args[] = { events::Number(y), events::Number(x) };
    events::Emit(GW_EVENT_ID("level_input_on_mouse_wheel"), args, 2, &result);
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

namespace
{
void EmitNetDestroy(const CGameObject* object);
}

void ObjectNetDestroy(const CGameObject* object)
{
    if (!object)
        return;
    binders::OnObjectDestroy(object); // plugin binders (W0.1): on_destroy, before the Lua binder like the event
    EmitNetDestroy(object);           // npc_on_net_destroy / actor_on_net_destroy
    // After the binders and the events, which may still read them; before the Lua object of the object goes
    storage::OnObjectDestroy(object);      // the db.storage entry ends (W0.4)
    goap::OnObjectDestroyPlanners(object); // plugin planners of the object
}

namespace
{
void EmitNetDestroy(const CGameObject* object)
{
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
} // namespace

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
    if (!object)
        return;
    // Plugin binders (W0.1) of every object: chosen at Init, on_reinit and on_spawn after the Lua binder's calls.
    switch (kind)
    {
    case EBinder::Init: binders::OnObjectInit(object); break;
    case EBinder::Reinit: binders::OnObjectReinit(object); break;
    case EBinder::NetSpawn:
        binders::OnObjectSpawn(object);
        goap::OnObjectSpawn(object); // native evaluators: installed at the next point without a running planner
        break;
    }
    // CActor::net_Spawn has not finished at these points: check the class, not g_actor.
    if (!smart_cast<const CActor*>(object))
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
    binders::OnObjectUpdate(object, dt_ms); // plugin binders (W0.1): collected, delivered after the scheduler pass
    // actor_on_money_change: live after this first update (and its actor_on_first_update below) - the money given
    // by the scripts of the start (actor_on_first_update) is not a change either
    struct MoneyLive
    {
        bool actor;
        ~MoneyLive()
        {
            if (actor)
                g_actor_money_live = g_actor_spawned;
        }
    } money_live{ IsActor(object) };
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

void ActorFrame(const CGameObject* object, u32 dt_ms)
{
    // Every frame: the cheap checks first, the bus lookup only with the group on - nothing else without subscribers.
    // Not before actor_on_first_update (the binder runs after this frame's first scheduler pass of the actor).
    if (!IsActive(EGroup::Lifecycle) || !g_actor_spawned || g_actor_first_update || !IsActor(object))
        return;
    if (!events::HasSubscribers(GW_EVENT_ID("actor_on_frame")) || !IsAlive(object))
        return;
    const GwpValue args[] = { events::Int(dt_ms) };
    events::Emit(GW_EVENT_ID("actor_on_frame"), args, 1);
}

void NpcUpdate(const CGameObject* npc)
{
    if (!npc || !IsStalker(npc) || !IsAlive(npc))
        return;
    if (!events::HasSubscribers(GW_EVENT_ID("npc_on_update")))
        return;
    // (npc, storage): the second argument is db.storage[id] in Lua, which the engine does not know; plugins get
    // GWP_T_LUA_REF, Lua subscribers the table through the adapter of axr_main.script
    const GwpValue args[] = { events::Object(npc->ID()), BinderPlaceholder() };
    events::Emit(GW_EVENT_ID("npc_on_update"), args, 2);
}

void LevelChanging()
{
    if (!IsActive(EGroup::Lifecycle))
        return;
    events::Emit(GW_EVENT_ID("on_level_changing"));
}

// engine gaps P09: GUI_on_show / GUI_on_hide. Not tied to a group or to the actor: the main menu has dialogs too.
void GuiDialog(pcstr name, pcstr window_class, bool shown)
{
    const GwpEventId id = shown ? GW_EVENT_ID("GUI_on_show") : GW_EVENT_ID("GUI_on_hide");
    if (!events::HasSubscribers(id))
        return;
    const pcstr type = window_class ? window_class : "";
    const GwpValue args[] = { events::String(name && name[0] ? name : type), events::String(type) };
    events::Emit(id, args, 2);
}

// engine gaps P38: map_spot_on_removed
bool WantsMapSpotRemoved() { return g_actor_spawned && events::HasSubscribers(GW_EVENT_ID("map_spot_on_removed")); }

void MapSpotRemoved(u16 id, pcstr spot_type)
{
    if (!WantsMapSpotRemoved())
        return;
    const GwpValue args[] = { events::Int(id), events::String(spot_type ? spot_type : "") };
    events::Emit(GW_EVENT_ID("map_spot_on_removed"), args, 2);
}

// ---------------------------------------------------------------------------------------------
// engine gaps P08/P10/P30 (plans/addon_api_ideas/spec.md): trade price, cancellable deal, take, drop, news; upgrades
// ---------------------------------------------------------------------------------------------

namespace
{
// The bool result of a cancellable event: anything but an explicit false lets the action go on
bool Allowed(const GwpValue& result) { return result.type != GWP_T_BOOL || result.u.b != 0; }

GwpValue IdList(xr_vector<GwpValue>& items, const xr_vector<u16>& ids)
{
    const size_t count = std::min<size_t>(ids.size(), events::kMaxArrayItems);
    items.clear();
    items.reserve(count);
    for (size_t i = 0; i < count; ++i)
        items.push_back(events::Int(ids[i]));
    return IdArray(items);
}
} // namespace

u32 TradeItemPrice(const CGameObject* item, const CGameObject* partner, bool actor_buys, u32 price)
{
    // GetItemPrice runs for every cell the trade window prices: nothing without subscribers
    const GwpEventId id = GW_EVENT_ID("trade_on_item_price");
    if (!ActorReady(EGroup::Actor) || !item || !partner || !events::HasSubscribers(id))
        return price;
    GwpValue result = events::Int(price);
    const GwpValue args[] = { events::Object(item->ID()), events::Object(partner->ID()), events::Bool(actor_buys),
        events::Int(price) };
    events::Emit(id, args, 4, &result);
    double value;
    if (result.type == GWP_T_INT)
        value = static_cast<double>(result.u.i);
    else if (result.type == GWP_T_NUMBER && std::isfinite(result.u.n))
        value = result.u.n;
    else
        return price;
    // the clamp of CTrade::GetItemPrice: a price is never 0 (0 there means "not traded") nor above a million
    clamp(value, 1.0, 1000000.0);
    return static_cast<u32>(iFloor(static_cast<float>(value)));
}

bool ActorBeforeTradeDeal(const CGameObject* partner, u32 money_got, u32 money_paid, const xr_vector<u16>& sold,
    const xr_vector<u16>& bought)
{
    const GwpEventId id = GW_EVENT_ID("actor_on_before_trade_deal");
    if (!ActorReady(EGroup::Actor) || !partner || !events::HasSubscribers(id))
        return true;
    xr_vector<GwpValue> sold_items;
    xr_vector<GwpValue> bought_items;
    GwpValue result = events::Bool(true);
    const GwpValue args[] = { events::Object(partner->ID()), events::Int(money_got), events::Int(money_paid),
        IdList(sold_items, sold), IdList(bought_items, bought) };
    events::Emit(id, args, static_cast<u32>(std::size(args)), &result);
    return Allowed(result);
}

void ItemUpgrade(const CGameObject* item, pcstr upgrade)
{
    const GwpEventId id = GW_EVENT_ID("item_on_upgrade");
    if (!g_actor_spawned || !item || !upgrade || !events::HasSubscribers(id))
        return;
    const GwpValue args[] = { events::Object(item->ID()), events::String(upgrade) };
    events::Emit(id, args, 2);
}

namespace
{
bool ActorBeforeItem(GwpEventId id, const CGameObject* item)
{
    if (!ActorReady(EGroup::Actor) || !item || !events::HasSubscribers(id))
        return true;
    GwpValue result = events::Bool(true);
    const GwpValue args[] = { events::Object(item->ID()) };
    events::Emit(id, args, 1, &result);
    return Allowed(result);
}
} // namespace

bool ActorBeforeItemTake(const CGameObject* item)
{
    return ActorBeforeItem(GW_EVENT_ID("actor_on_before_item_take"), item);
}

bool ActorBeforeItemDrop(const CGameObject* item)
{
    return ActorBeforeItem(GW_EVENT_ID("actor_on_before_item_drop"), item);
}

bool NewsBeforeShow(pcstr caption, pcstr text, pcstr icon, int show_ms, int type)
{
    const GwpEventId id = GW_EVENT_ID("news_on_before_show");
    if (!ActorReady(EGroup::Actor) || !events::HasSubscribers(id))
        return true;
    GwpValue result = events::Bool(true);
    const GwpValue args[] = { events::String(caption ? caption : ""), events::String(text ? text : ""),
        events::String(icon ? icon : ""), events::Int(show_ms), events::Int(type) };
    events::Emit(id, args, 5, &result);
    return Allowed(result);
}

// ---------------------------------------------------------------------------------------------
// engine gaps P11 (plans/addon_api_ideas/spec.md): weather_on_changed
// ---------------------------------------------------------------------------------------------

namespace
{
// The weather seen at the previous frame. The names point into the keys of CEnvironment::WeatherCycles /
// WeatherFXs (shared_str values that live as long as the environment), so they stay valid as event arguments
const void* g_weather_level = nullptr; // the level the state belongs to: a new level starts it again
pcstr g_weather_name = nullptr;
bool g_weather_fx = false;
} // namespace

void WeatherFrame()
{
    if (!IsMainThread()) // as FlushBatches next to it: the events go from the game logic thread
        return;
    if (!g_pGameLevel || !g_pGamePersistent)
    {
        g_weather_level = nullptr;
        return;
    }
    CEnvironment& environment = g_pGamePersistent->Environment();
    // The pointers only: the shared_str is not copied (the environment may switch an effect off on another thread
    // in the frame of the game/render overlap; a pointer read sees the old value or the new one)
    const pcstr name = environment.CurrentWeatherName.c_str();
    const bool fx = environment.IsWFXPlaying();
    if (g_weather_level != g_pGameLevel)
    {
        g_weather_level = g_pGameLevel;
        g_weather_name = nullptr; // the first weather of a level comes with previous = nil
        g_weather_fx = false;
    }
    if (!name || (name == g_weather_name && fx == g_weather_fx))
        return;
    const pcstr previous = g_weather_name;
    g_weather_name = name;
    g_weather_fx = fx;
    const GwpEventId id = GW_EVENT_ID("weather_on_changed");
    if (!events::HasSubscribers(id))
        return;
    const pcstr cycle = environment.CurrentCycleName.c_str();
    // (weather, previous, effect playing, cycle): weather - the name level.get_weather() gives (the effect while one
    // plays), cycle - the weather cycle under it
    const GwpValue args[] = { events::String(name), previous ? events::String(previous) : events::Nil(),
        events::Bool(fx), cycle ? events::String(cycle) : events::Nil() };
    events::Emit(id, args, 4);
}

#undef GW_EVENT_ID
} // namespace gw::addons::objevents
