#pragma once

// Event bus: one dispatcher in the engine for Lua scripts and native plugins.
//  - Lua: global table event_bus (declare/subscribe/unsubscribe/emit); axr_main.callback_set/make_callback and
//    RegisterScriptCallback/SendScriptCallback are thin wrappers over it.
//  - Plugins: event_* functions of GwpEngineApi (xrAddonHost/include/gwp/gwp_api.h).
//  - Engine: built-in lifecycle events below, emitted from C++.
// Main thread only. Docs: wiki/doc/plugins/api/events.md; plan: plans/lua_to_cpp/04-plugin-api-events-and-data.md

#include "xrAddonHost/include/gwp/gwp_api.h"

struct lua_State;

namespace gw::addons::events
{
// Events emitted by the engine itself. Registered first, so their ids are fixed: id == value.
enum class EBuiltin : GwpEventId
{
    EngineOnScriptStart = 1, // Lua state (re)created and common scripts loaded
    AlifeOnStart,            // (reason: "new_game" | "load" | "level_change") ALife created, before alife_on_load
    AlifeOnEnd,              // ALife is being destroyed (exit to menu, another save, level change, quit)
    AlifeOnBeforeSave,       // (save_name: file name with extension) before the ALife save is written; save_write here
    AlifeOnAfterSave,        // (save_name) the save is on disk
    AlifeOnLoad,             // (save_name) plugin save data is read; objects registered, on_register not called yet
    AlifeOnAfterLoad,        // (save_name) on_register of every object is done (comes on every load)
    LevelOnStart,            // (level_name) the level is loaded and the client started
    LevelOnStop,             // the level is being stopped; object ids are still valid
    LevelOnFrame,            // (dt seconds) every frame of an active, unpaused level
    ActorOnSpawn,            // (actor object) CActor::net_Spawn succeeded
    ActorOnDestroy,          // (actor object) CActor::net_Destroy
    DataOnChanged,           // (key, value) a value of the data bus changed; value is nil after an erase
    Count_
};

GwpEventId Intern(pcstr name); // id of the event, registering the name when needed; 0 for an empty name

// schema: argument codes (b bool, I integer, N number, s string, v vector, o game object, O server object,
// t Lua table/userdata, * anything; '?' after a code = may be nil). nullptr = keep the current one.
void Declare(pcstr name, u32 flags = 0, pcstr schema = nullptr);

// Synchronous dispatch to Lua and native subscribers. result may be nullptr.
void Emit(GwpEventId id, const GwpValue* argv = nullptr, u32 argc = 0, GwpValue* result = nullptr);
inline void Emit(EBuiltin id, const GwpValue* argv = nullptr, u32 argc = 0, GwpValue* result = nullptr)
{
    Emit(static_cast<GwpEventId>(id), argv, argc, result);
}
bool HasSubscribers(GwpEventId id);

// Delivers the collected events to batch subscribers (GWP_SUBSCRIBE_BATCH). Called at the start of every frame
// (CGamePersistent::OnFrame) and right before level_on_stop. force: ignore throttle_ms (level stop: the ids are
// valid only now).
void FlushBatches(bool force = false);
inline bool HasSubscribers(EBuiltin id) { return HasSubscribers(static_cast<GwpEventId>(id)); }

GwpValue Nil();
GwpValue Bool(bool value);
GwpValue Int(s64 value);
GwpValue Number(double value);
GwpValue String(pcstr value); // the string must outlive the Emit call
GwpValue Vec3(const Fvector& value);
GwpValue Object(u16 id);
GwpValue ServerObject(u16 id);

// Host integration (addon_host.cpp).
void FillEngineApi(GwpEngineApi& api);

// Stage B (plans/lua_to_cpp/06): events whose source moves from Lua binders to the engine, grouped
// (addon_object_events.h). An event of an active group sent from Lua is dropped with a log line.
void DeclareEngineSource(pcstr name, u32 group);
void SetActiveEngineGroups(u32 mask);

// Argument schema check (console gw_event_schema_check): -1 = on with -addon_debug (default), 0 = off, 1 = on.
void SetSchemaCheck(int mode);
int GetSchemaCheck();

// Log of every emit of the events moved to the engine (console gw_event_engine_log, rate-limited per event):
// -1 = on with -addon_debug (default), 0 = off, 1 = on.
void SetEngineLog(int mode);
int GetEngineLog();

// Console command event_trace: logs every emit of the event with its arguments. false for an invalid name.
bool SetTrace(pcstr name, bool on);

// alife_on_start reason: a level change goes through an autosave and a new ALife loading it. change_level marks it,
// the next ALife start takes the mark.
void MarkLevelChange();
bool TakeLevelChangeMark();

void RemovePluginSubscriptions(const GwpPlugin* plugin);
// The object went offline (CGameObject::net_Destroy, after its Lua binder): its GWP_SUBSCRIBE_OBJECT subscriptions end.
void RemoveObjectSubscriptions(u16 object);
void Shutdown();
void PrintList(); // console command "event_list"

// Calls fn(context), a host thunk that calls one plugin function, with the guards of the event bus: a C++
// exception, and on Windows a crash inside the plugin library, stop here and give false (logged with `what`).
// plugin_code: an address inside the plugin library (the callback itself), so the crash filter knows the module.
// Shared with the binders (addon_binders.cpp).
bool CallPluginGuarded(const void* plugin_code, pcstr what, void (*fn)(void*), void* context);

// Plugin functions running right now on this thread (guarded calls nested in each other). 0: no plugin code is on
// the stack, a plugin library may be unloaded (addon_host.cpp: the unload and the reload of a crashed plugin).
u32 PluginCallDepth();

// Lua <-> GwpValue, the same conversion as for event arguments (addon_data_bus.cpp).
// LuaToValue: strings point into Lua data, valid while the value stays on the Lua stack.
void PushLuaValue(lua_State* L, const GwpValue& value);
// The Lua thread to call Lua on right now: the coroutine that runs a Lua emit, else the main state; nullptr when
// the bus has no Lua state (addon_api_npc.cpp: script_call from a handler of such an emit).
lua_State* ActiveLuaThread();
GwpValue LuaToValue(lua_State* L, int index);
} // namespace gw::addons::events
