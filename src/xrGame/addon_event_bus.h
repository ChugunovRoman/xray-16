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
    GameOnStart,             // CGamePersistent::OnGameStart
    GameOnEnd,               // CGamePersistent::OnGameEnd
    AlifeOnBeforeSave,       // (save_name: file name with extension) before the ALife save is written; save_write here
    AlifeOnAfterSave,        // (save_name) the save is on disk
    AlifeOnLoad,             // (save_name) plugin save data is read; objects registered, on_register not called yet
    AlifeOnAfterLoad,        // (save_name) on_register of every object is done (comes on every load)
    LevelOnStart,            // (level_name) the level is loaded and the client started
    LevelOnStop,             // the level is being stopped; object ids are still valid
    LevelOnFrame,            // (dt seconds) every frame of an active, unpaused level
    ActorOnSpawn,            // (actor object) CActor::net_Spawn succeeded
    ActorOnDestroy,          // (actor object) CActor::net_Destroy
    Count_
};

GwpEventId Intern(pcstr name); // id of the event, registering the name when needed; 0 for an empty name
void Declare(pcstr name, u32 flags = 0);

// Synchronous dispatch to Lua and native subscribers. result may be nullptr.
void Emit(GwpEventId id, const GwpValue* argv = nullptr, u32 argc = 0, GwpValue* result = nullptr);
inline void Emit(EBuiltin id, const GwpValue* argv = nullptr, u32 argc = 0, GwpValue* result = nullptr)
{
    Emit(static_cast<GwpEventId>(id), argv, argc, result);
}
bool HasSubscribers(GwpEventId id);
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
void RemovePluginSubscriptions(const GwpPlugin* plugin);
void Shutdown();
void PrintList(); // console command "event_list"
} // namespace gw::addons::events
