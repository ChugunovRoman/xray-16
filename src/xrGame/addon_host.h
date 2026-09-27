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

// Plugin save data (save_write/save_read): a block at the end of the ALife save stream (alife_storage_manager.cpp).
void ResetSaveData();                // new game: forget the data of the previous one
void WriteSaveData(IWriter& stream); // writes its own chunk into the ALife save stream
void ReadSaveData(IReader& stream);  // finds that chunk; a save without it gives no data

// Internal: shared by the parts of the host (addon_event_bus.cpp, addon_data_bus.cpp, addon_api_objects.cpp).
pcstr PluginAddonId(const GwpPlugin* plugin); // "?" for an unknown handle
bool IsMainThread();
bool IsDebugLog(); // -addon_debug
bool IsPluginLoaded(pcstr addon_id); // the addon is known and its plugin is loaded (and not unloaded yet)
// Reads a zero-terminated string without leaving the reader (IReader::r_stringZ does not check bounds).
bool ReadStringZChecked(IReader& reader, xr_string& out);
void FillObjectsApi(GwpEngineApi& api);
void FillLevelApi(GwpEngineApi& api);
void FillInfoApi(GwpEngineApi& api);
} // namespace gw::addons
