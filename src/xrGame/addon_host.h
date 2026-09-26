#pragma once

// Addons host: discovers addons by their addon.ltx manifests and loads their plugins.
//  - Addon:  game package (models, textures, configs, levels, shaders, Lua scripts, plugins).
//  - Plugin: native C/C++ library inside an addon, loaded as native code through the Plugin API.
// Docs:   wiki/doc/plugins
// Design: plans/lua_to_cpp/03-build-and-discovery.md (section 4),
//         plans/addons_api_support/08-unified-addons-api-lua-native.md (sections 4.2, 4.3, 4.8).
// Plugin API (C ABI): xrAddonHost/include/gwp/gwp_api.h

namespace gw::addons
{
// Discovers addons and loads their plugins. Call once per process on the main thread,
// after the file system is initialized and before any Lua code runs.
void Initialize();

// Calls on_unload of every loaded plugin (reverse load order) and unloads the libraries.
void Shutdown();

// Prints every discovered addon with its plugin state to the log (console command "addon_list").
void PrintList();
} // namespace gw::addons
