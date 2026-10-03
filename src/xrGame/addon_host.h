#pragma once

// Addons host: discovers addons by their addon.ltx manifests and loads their plugins.
//  - Addon:  game package (models, textures, configs, levels, shaders, Lua scripts, plugins).
//  - Plugin: native C/C++ library inside an addon, loaded as native code through the Plugin API.
// Docs:   wiki/doc/plugins
// Design: plans/lua_to_cpp/03-build-and-discovery.md (section 4),
//         plans/addons_api_support/08-unified-addons-api-lua-native.md (sections 4.2, 4.3, 4.8).
// Plugin API (C ABI): xrAddonHost/include/gwp/gwp_api.h

#include "xrAddonHost/include/gwp/gwp_api.h"

class IReader;
class IWriter;

namespace gw::addons
{
// Discovers addons and loads their plugins. Call once per process on the main thread,
// after the file system is initialized and before any Lua code runs.
void Initialize();

// Calls on_unload of every loaded plugin (reverse load order) and unloads the libraries.
void Shutdown();

// Prints every discovered addon with its plugin state to the log (console command "addon_list").
void PrintList();

// A plugin crashed in one of its functions (a guard of the host caught it, the crash block with the stack is in the
// log). It stops at once: every group of the engine drops what it registered - the subscriptions, binders, engine
// callbacks and exports go, its native evaluators and actions go back to Lua, its planners stop. The library is
// unloaded at the start of the next frame and loaded again at the next game load, at most kMaxAutoRestarts times
// per run of the game (and by the console command plugin_reload). Safe inside a dispatch. Main thread.
void OnPluginCrashed(const GwpPlugin* plugin, pcstr what);

// Start of a frame (CGamePersistent::OnFrame), no plugin code on the stack: the unloads of crashed plugins and the
// reloads asked for by plugin_reload.
void OnFrame();

// A game session starts (CALifeSimulator, right before alife_on_start): crashed plugins are loaded again, so they
// get alife_on_start like at a normal start.
void OnGameStart();

// Console command plugin_reload <addon id>: the plugin of the addon is unloaded (when loaded) and loaded again from
// its library - a rebuilt one too - at the start of the next frame. false with a reason for an unknown addon.
bool RequestPluginReload(pcstr addon_id, xr_string& reason);

// Plugin save data (save_write/save_read): a block at the end of the ALife save stream (alife_storage_manager.cpp).
void ResetSaveData();                // new game: forget the data of the previous one
void WriteSaveData(IWriter& stream); // writes its own chunk into the ALife save stream
void ReadSaveData(IReader& stream);  // finds that chunk; a save without it gives no data

// Internal: shared by the parts of the host (addon_event_bus.cpp, addon_data_bus.cpp, addon_api_objects.cpp).
pcstr PluginAddonId(const GwpPlugin* plugin); // "?" for an unknown handle
bool IsMainThread();
bool IsDebugLog(); // -addon_debug
bool IsPluginLoaded(pcstr addon_id); // the addon is known and its plugin is loaded (and not unloaded yet)
bool IsPluginLoading(const GwpPlugin* plugin); // inside its gwp_plugin_init
bool IsPluginRunning(const GwpPlugin* plugin); // loading or loaded: not stopped by a crash, not unloaded
// GwpPluginDesc::api_built (GWP_MAKE_VERSION) of the loaded plugin of the addon; 0 when the addon is unknown, its
// plugin is not loaded or its description is too short to have the field (Lua plugins.version).
u32 PluginApiBuilt(pcstr addon_id);
// Ids of every addon found at the start - loaded, failed, skipped or disabled - known before the first plugin loads.
// Empty before Initialize and after Shutdown.
const xr_vector<xr_string>& KnownAddonIds();
// The addon that owns a name "<addon id>_...": the longest known id followed by '_' at the start of `name`, nullptr
// when none. With addons "gw" and "gw_npc" known, "gw_npc_x" belongs to gw_npc even before (or without) its plugin.
pcstr NameOwnerAddonId(pcstr name);
// Reads a zero-terminated string without leaving the reader (IReader::r_stringZ does not check bounds).
bool ReadStringZChecked(IReader& reader, xr_string& out);
void FillObjectsApi(GwpEngineApi& api);
void FillLevelApi(GwpEngineApi& api);
void FillInfoApi(GwpEngineApi& api);
void FillNpcApi(GwpEngineApi& api); // groups npc and script (addon_api_npc.cpp)
void ShutdownNpcApi();
void FillExportsApi(GwpEngineApi& api); // script_export (addon_script_exports.cpp)
void FillNpcControlApi(GwpEngineApi& api); // groups npc_body, npc_sight, npc_anim, inventory, smart_cover
void RemovePluginExports(const GwpPlugin* plugin);
void ShutdownExports();
// script_call_list: counters of script_call (plugin -> Lua) and of plugins.call (Lua -> plugin); `reset` zeroes them
void PrintScriptCallList(bool reset);
void PrintExportCallList(bool reset);
struct lua_State* CallingLuaThread(); // the Lua thread of the plugins.call in progress, nullptr outside one
} // namespace gw::addons
