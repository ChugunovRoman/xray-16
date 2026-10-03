#pragma once

// Plugin API, group "console": console variables, console commands and profiling stages of plugins.
//  - Reading works for any variable of the engine; a plugin registers its own under the "<addon id>_" prefix.
//    The value is stored by the engine, so a reload of the plugin library cannot leave a dangling reference
//    inside the console (CCC_Integer keeps a pointer to the variable).
//  - A command may be run at once or at the end of the frame: quit, disconnect, load and main_menu destroy the
//    level inside the call, which is not safe from an event handler.
//  - A profiling stage of a plugin is printed together with the stages of the engine (-npc_cpp_profile).
//  - The values of the variables of plugins live in appdata/plugins.ltx, not in user.ltx: user.ltx runs before
//    the plugins are loaded and is written after they are unloaded. The file is read before the plugins load,
//    a registration takes its value, a change is written at the end of the frame; lines of variables nobody
//    registered are kept as they are. Plan: plans/lua_to_cpp/12-plugin-cvars-file.md
// Docs: wiki/doc/plugins/api/console.md; plan: plans/lua_to_cpp/07-stage-c-service-api-groups.md

#include "xrAddonHost/include/gwp/gwp_api.h"

struct GwpPlugin;

namespace gw::addons::console
{
void FillEngineApi(GwpEngineApi& api);

void OnFrame();       // runs the commands of console_execute_deferred and saves plugins.ltx after a change
void LoadCvarsFile(); // reads appdata/plugins.ltx: before the first plugin registers a variable
void PrintCvarList(); // plugin_cvar_list
void FlushProfile();  // prints the stages of plugins (npc_cpp_profile.cpp, inside its own report)
void RemovePluginCvars(const GwpPlugin* plugin); // the plugin is unloaded: its commands leave the console
void Shutdown();
} // namespace gw::addons::console
