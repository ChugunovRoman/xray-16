#pragma once

// Engine sources of object events (stage B, plans/lua_to_cpp/06-stage-b-engine-event-sources.md).
// Events such as actor_on_hit_callback or npc_on_death_callback were sent by Lua binders through
// SendScriptCallback. Stage B moves their source into the engine, group by group. An event whose group is active
// is emitted by the engine; the same event sent from Lua is dropped (logged once), so nothing is delivered twice.
//  - A group is active when it is implemented in this build AND enabled by the console command
//    gw_event_engine_sources (all / none / comma-separated group names). Disabling a group is a diagnostic: the
//    engine stops emitting its events and the Lua lines (while they exist) work again.

class CGameObject;
class IGameObject;

namespace gw::addons::objevents
{
enum class EGroup : u32
{
    Actor,     // actor_on_hit_callback, actor_on_item_*, on_key_*, actor_on_weapon_*, ...
    Input,     // level_input_on_key_press, on_before_item_use
    Monster,   // monster_on_*
    Physic,    // physic_object_on_*, heli_on_hit_callback
    Npc,       // npc_on_* (except npc_on_update)
    Lifecycle, // actor_on_init/reinit, on_game_load, actor_on_net_destroy, actor_on_update*, on_level_changing
    Count_
};

// Declares every engine-sourced event with its group on the event bus. Called once when the bus is created.
void RegisterEngineEvents();

// Console command gw_event_engine_sources. false for an unknown group name (nothing changes then).
bool SetEnabledGroups(pcstr text);
void GetStatus(pstr out, size_t size); // "all" / "none" / "actor,npc" + which are implemented

bool IsActive(EGroup group); // the engine emits the events of this group

// ---------------------------------------------------------------------------------------------
// Emitters. Each is called right after the engine calls the Lua slot of the same callback (the Lua binder does
// its work first, subscribers see the same world as before). They do nothing while the group is inactive.
// ---------------------------------------------------------------------------------------------

// The actor finished (or started) net_Spawn: actor events are sent only for a spawned actor, like the Lua
// binder did (it checked db.actor).
void SetActorSpawned(bool spawned);

// Group actor (B-1)
void ActorHit(const CGameObject* actor, float amount, const Fvector& local_dir, const IGameObject* who, s16 bone);

// actor_on_before_death(who_id) with a result. Returns false when a subscriber cancelled the death.
bool ActorBeforeDeath(u16 who_id);

enum class EItem
{
    Take,
    Drop,
    ToBelt,
    ToRuck,
    ToSlot
};
// Items of an inventory owner: actor_on_item_* / actor_item_to_* for the actor, npc_on_item_take/drop for stalkers.
void OwnerItem(EItem kind, const CGameObject* owner, const CGameObject* item);
void ActorItemUse(const CGameObject* item);
void ActorItemTakeFromBox(const CGameObject* box, const CGameObject* item);
void ActorTrade(const CGameObject* item, bool sell, u32 money);
void ActorInfo(const CGameObject* actor, pcstr info_id);

enum class EKey
{
    Press,
    Release,
    Hold
};
void ActorKey(EKey kind, int key);

enum class EWeapon
{
    Jammed,
    ZoomIn,
    ZoomOut,
    Reload, // try_reload: the weapon looks for ammo (actor_on_weapon_reload)
    NoAmmo  // the magazine became empty (actor_on_weapon_no_ammo)
};
// owner: the holder of the weapon (checked to be the actor); ammo_total: for Reload and NoAmmo.
void ActorWeapon(EWeapon kind, const IGameObject* owner, const CGameObject* weapon, int ammo_total = 0);
void ActorHudAnimationEnd(const CGameObject* item, pcstr hud_section, pcstr motion, u32 state, u32 slot);
void ActorTorch(const CGameObject* torch, bool on);

enum class EVehicle
{
    Attach,
    Detach,
    Use
};
void ActorVehicle(EVehicle kind, const IGameObject* vehicle);

// Group input (B-1)
// level_input_on_key_press(dik, action): true when a subscriber consumed the key (flags.ret = true in Lua).
bool LevelInputKeyPress(int key, int action);
// on_before_item_use(owner, item): false when a subscriber cancelled the use (flags.ret_value = false in Lua).
bool BeforeItemUse(const CGameObject* owner, const CGameObject* item);

// Groups npc (B-3), monster and physic (B-2). The event name follows the class of the object: npc_on_* for
// stalkers, monster_on_* for monsters, physic_object_on_* for physic objects; other classes send nothing.
void ObjectHit(const CGameObject* object, float amount, const Fvector& local_dir, const IGameObject* who, s16 bone);
void ObjectDeath(const CGameObject* victim, const IGameObject* who); // npc / monster (not poltergeist); who may be nil
void ObjectUse(const CGameObject* object, const IGameObject* user);  // alive NPC, dead monster, physic object
void ObjectNetSpawn(const CGameObject* object);                      // npc (alive) / monster, after the Lua binder
void ObjectNetDestroy(const CGameObject* object);                    // npc, before the Lua binder
// npc_on_hear_callback: for stalkers and monsters (like the Lua binders). sound_type: the engine bit mask.
void ObjectHear(const CGameObject* object, u16 who_id, int sound_type, const Fvector& position, float power);
bool WantsHear(); // the event is sent by the engine and has subscribers: queue sounds even without a Lua slot

// Group lifecycle (B-4): the actor binder. The first argument of these events is the Lua actor binder, which the
// engine does not know: plugins get GWP_T_LUA_REF, Lua subscribers get db.actor_binder through the Lua adapters of
// axr_main.script.
enum class EBinder
{
    Init,     // actor_on_init: the Lua binder object was created (CScriptBinder::reload)
    Reinit,   // actor_on_reinit
    NetSpawn, // on_game_load: the binder net_spawn succeeded
};
void ObjectBinder(EBinder kind, const CGameObject* object); // actor only
void ObjectBinderUpdate(const CGameObject* object, u32 dt_ms); // actor: first update, update_fast/slow, update
void LevelChanging(); // on_level_changing: CALifeUpdateManager::change_level, before the client save
void HeliHit(const CGameObject* heli, float damage, float impulse, u32 hit_type, const IGameObject* who);
} // namespace gw::addons::objevents
