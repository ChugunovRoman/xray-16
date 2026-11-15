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
// actor_on_trade_deal(partner, money got, money paid, sold ids, bought ids, sold prices, bought prices)
// (plans/addon_quests): one press of a trade button of the trade window (CUIActorMenu::OnBtnPerformTrade*). Begin
// before the transfer of the lists, ActorTrade collects every item of it (id and the money of its actor_on_trade), End
// sends the event when at least one item moved. The price arrays are parallel to the id arrays. A free item given to
// the partner (DonateCurrentItem) is not a deal: only actor_on_trade comes for it
void ActorTradeDealBegin();
void ActorTradeDealEnd(const CGameObject* partner);
// actor_on_money_change(new_money, delta) from CInventoryOwner::set_money of the actor (plans/addon_achievements 7.2):
// only a real change, only after the first update of the actor binder since its spawn - setting the money of the
// saved actor at the spawn (a new game, a load, a level change) sends nothing
void ActorMoneyChange(u32 old_money, u32 new_money);
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
// level_input_on_mouse_wheel(y, x): true when a subscriber consumed the wheel (flags.ret = true in Lua) - then neither
// the actor (weapon switch, HUD FOV) nor anything after it gets it. plans/addon_base_building/plan.md D1
bool LevelInputMouseWheel(float y, float x);
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
// Every game object goes through these (the plugin binders of addon_binders.cpp are dispatched here);
// the events they emit are for the actor only.
void ObjectBinder(EBinder kind, const CGameObject* object);
void ObjectBinderUpdate(const CGameObject* object, u32 dt_ms); // actor: first update, update_fast/slow, update
// actor_on_frame(dt_ms): every scheduler pass of the actor = every frame out of the pause (CGameObject::shedule_Update,
// not throttled like the binder and its actor_on_update*), live actor, after actor_on_first_update; only with
// subscribers. No Lua source.
void ActorFrame(const CGameObject* object, u32 dt_ms);
void LevelChanging(); // on_level_changing: CALifeUpdateManager::change_level, before the client save
// npc_on_update(npc, storage) for a live stalker, when the native gate of the binder lets its update through
// (W3-2; called from CGameObject::shedule_Update). Not a group of stage B: while the gate is off, xr_motivator
// sends the event from Lua as before.
void NpcUpdate(const CGameObject* npc);
void HeliHit(const CGameObject* heli, float damage, float impulse, u32 hit_type, const IGameObject* who);

// engine gaps P09 (plans/addon_api_ideas/spec.md): GUI_on_show(name, class) / GUI_on_hide(name, class) - a dialog of
// the game UI or of the main menu was shown / hidden (CDialogHolder::StartMenu / StopMenu: engine windows and Lua
// windows shown with ShowDialog alike). name: the window name (SetWindowName), the class when it has none; class: the
// engine class of the window (CUIActorMenu, CUIPdaWnd, CUITalkWnd, CUIDialogWndEx for Lua windows, ...). Not in
// kEngineEvents: Lua may send these events too. Only with subscribers.
void GuiDialog(pcstr name, pcstr window_class, bool shown);
// engine gaps P38: map_spot_on_removed(id, spot_type) - a map location (a spot of a type on an object) is removed:
// level.map_remove_object_spot, the plugin map_spot_remove, the object went away, the location expired. CMapManager
// collects them (only while WantsMapSpotRemoved) and sends them at the start of its next Update (the next frame).
bool WantsMapSpotRemoved();
void MapSpotRemoved(u16 id, pcstr spot_type);

// engine gaps P08/P10/P30 (plans/addon_api_ideas/spec.md): events with a result and an upgrade event. Not in
// kEngineEvents (no Lua source to replace): the schemas live in intercepts of axr_main.script, the result field of Lua
// in kLuaResultBindings (addon_event_bus.cpp). Each is emitted only with subscribers and a spawned actor.
// trade_on_item_price(item, partner, actor_buys, price) -> price: CTrade::GetItemPrice of a trade with the actor (the
// window shows it, the transfer pays it). Returns the price to use: a subscriber's INT / NUMBER result clamped to
// 1..1000000 like the engine price, anything else keeps `price`.
u32 TradeItemPrice(const CGameObject* item, const CGameObject* partner, bool actor_buys, u32 price);
// actor_on_before_trade_deal(partner, money_got, money_paid, sold ids, bought ids) -> bool: the trade button, after the
// money check, before the transfer. false: the deal is not made, the items stay in the trade lists.
bool ActorBeforeTradeDeal(const CGameObject* partner, u32 money_got, u32 money_paid, const xr_vector<u16>& sold,
    const xr_vector<u16>& bought);
// item_on_upgrade(item, upgrade): an upgrade was installed (Manager::upgrade_install outside the load, upgrade_add).
void ItemUpgrade(const CGameObject* item, pcstr upgrade);
// actor_on_before_item_take(item) -> bool: the actor picks an item up from the world (the use key, CActor pickup).
// actor_on_before_item_drop(item) -> bool: the player drops an item (the inventory window, the drop key). false
// cancels. Script drops and transfers (drop_item, transfer_item) do not ask.
bool ActorBeforeItemTake(const CGameObject* item);
bool ActorBeforeItemDrop(const CGameObject* item);
// news_on_before_show(caption, text, icon, show_ms, type) -> bool: CActor::AddGameNews (Lua give_game_news, plugin
// news_send, a deferred news when its time comes); type: GAME_NEWS_DATA::eNewsType (0 news, 1 a line of a talk).
// false: the news is not shown and not kept in the PDA log.
bool NewsBeforeShow(pcstr caption, pcstr text, pcstr icon, int show_ms, int type);

// engine gaps P11 (plans/addon_api_ideas/spec.md): weather_on_changed(weather, previous, fx, cycle) - the weather
// (level.get_weather(): the effect while one plays) or the effect state changed: a new cycle of level_weathers, a
// weather_set / weather_lock, an effect starting or ending. Polled once a frame (CGamePersistent::OnFrame) by the
// pointer of the name: the first weather of every level comes with previous = nil. Not in kEngineEvents: no Lua
// source. Only with subscribers (the state is tracked always).
void WeatherFrame();
} // namespace gw::addons::objevents
