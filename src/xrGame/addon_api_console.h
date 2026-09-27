#pragma once

// Plugin API, group "console": console variables, console commands and profiling stages of plugins.
//  - Reading works for any variable of the engine; a plugin registers its own under the "<addon id>_" prefix.
//    The value is stored by the engine, so a reload of the plugin library cannot leave a dangling reference
//    inside the console (CCC_Integer keeps a pointer to the variable).
//  - A command may be run at once or at the end of the frame: quit, disconnect, load and main_menu destroy the
//    level inside the call, which is not safe from an event handler.
//  - A profiling stage of a plugin is printed together with the stages of the engine (-npc_cpp_profile).
// Docs: wiki/doc/plugins/api/console.md; plan: plans/lua_to_cpp/07-stage-c-service-api-groups.md

#include "xrAddonHost/include/gwp/gwp_api.h"

struct GwpPlugin;

namespace gw::addons::console
{
void FillEngineApi(GwpEngineApi& api);

void OnFrame();       // runs the commands of console_execute_deferred (CGamePersistent::OnFrame)
void FlushProfile();  // prints the stages of plugins (npc_cpp_profile.cpp, inside its own report)
void RemovePluginCvars(const GwpPlugin* plugin); // the plugin is unloaded: its commands leave the console
void Shutdown();
} // namespace gw::addons::console
