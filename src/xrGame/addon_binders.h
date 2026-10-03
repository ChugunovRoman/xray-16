#pragma once

// Plugin API, group "binders": the plugin's own callbacks for the life of game objects, chosen by class id
// (CLASS_ID text, "AI_STL_S") or by a section mask ("wpn_*"). Wave W0 of the native NPC work.
//  - A binder lives next to the Lua binder of the object: script_binding in the ltx is not touched, the Lua binder
//    runs first at every point, then the registered binders in registration order.
//  - Nothing is stored inside CGameObject (reinit() wipes what is): the host keeps a per-object mask of the
//    binders attached to it, computed once at the spawn of the object (or when a binder is registered later).
//  - Updates are batched: the scheduler pass collects {id, dt} per binder, one on_update call per binder and frame
//    delivers them after the pass, on the same thread. The throttling of the Lua binder applies as it is.
//  - Error policy is softer than CScriptBinder's clear-on-exception: a crash in a callback removes the binders of
//    that plugin only; other plugins and the Lua binder of the object keep working.
//  - State per object for the save (on_save / on_load of the vtable): kept per (addon, class id, section mask) and
//    object id, in memory while the object is offline and in an own chunk of the ALife save stream. Dropped when the
//    server object is released (OnServerObjectRelease) or comes back with another section. While a binder holds an
//    object, the engine keeps a shadow copy of its last bytes (from on_load or the on_save of a save): a binder that
//    goes away without on_save (a crash, an unload without binder_unregister) leaves them as the kept state.
// Docs: wiki/doc/plugins/api/binders.md; plan: plans/lua_to_cpp/08-wave-w0-native-npc-foundation.md (W0.1)

#include "xrAddonHost/include/gwp/gwp_api.h"

class CGameObject;
class IReader;
class IWriter;
struct GwpPlugin;

namespace gw::addons::binders
{
void FillEngineApi(GwpEngineApi& api);

// Engine points, called from addon_object_events.cpp for every game object (not on a dedicated server).
void OnObjectInit(const CGameObject* object);    // net_Spawn, after reload(section): the binders are chosen here
void OnObjectReinit(const CGameObject* object);  // net_Spawn, after the Lua binder reinit
void OnObjectSpawn(const CGameObject* object);   // net_Spawn succeeded, after the Lua binder net_Spawn
void OnObjectDestroy(const CGameObject* object); // net_Destroy, before the Lua binder
void OnObjectUpdate(const CGameObject* object, u32 dt_ms); // shedule_Update, inside the run_binder block
void FlushUpdates(); // end of the scheduler pass (CLevel::VisionBatchPostScheduler): delivers on_update batches

// Server object `id` leaves ALife for good (CALifeSimulatorBase::unregister_object, called by release): its saved
// binder state is dropped, and an online object with the id saves nothing when it goes offline - the id may come
// back as another object. Main thread.
void OnServerObjectRelease(u16 id);

// Saved binder state (addon_host.cpp: ResetSaveData / WriteSaveData / ReadSaveData). WriteSave asks on_save of every
// bound online object first - plugin code runs inside it, so it belongs before the plugin chunk of WriteSaveData
// (a save_write from on_save then reaches the same save).
void ResetSave();                 // a new game: the kept state of the previous one is dropped
void WriteSave(IWriter& stream);  // own chunk of the ALife save stream
void ReadSave(IReader& stream);   // replaces the kept state; a save without the chunk gives none
// WriteSave is asking on_save: the ALife save stream is being built. binder_register / binder_unregister refuse, and
// so do the plugin API calls that change ALife (addon_api_alife_ext.cpp, AlifeExtWriteGate). Main thread.
bool IsSaving();

void RemovePluginBinders(const GwpPlugin* plugin); // the plugin is unloaded or crashed: no callbacks are sent
void ForgetPluginCrash(const GwpPlugin* plugin);   // the crashed plugin is unloaded: its next instance may register
void Shutdown();
void PrintList(); // console command "binder_list"
} // namespace gw::addons::binders
