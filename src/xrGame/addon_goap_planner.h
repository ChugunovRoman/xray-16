#pragma once

// Plugin API, group "goap" (second half): planners owned by a plugin. The same GOAP planner of the engine the Lua
// scripts create with action_planner() (the state manager has one per NPC), with native evaluators and actions:
// a plugin that moves such a Lua planner to C++ builds the identical graph and gets the identical decisions.
//  - A planner belongs to one online object and to the plugin that created it; it is destroyed with the object
//    (net_Destroy), with the plugin (unload or a crash in a callback) or by planner_destroy.
//  - planner_update is the Lua planner:update(): solve, then initialize/execute/finalize of the actions. It must
//    not be called from a callback of the same planner.
// Docs: wiki/doc/plugins/api/goap.md; plan: plans/lua_to_cpp/09-stage-d-native-npc.md §5.3, §7.2 (W1)

#include "xrAddonHost/include/gwp/gwp_api.h"

class CGameObject;
struct GwpPlugin;

namespace gw::addons::goap
{
void FillPlannerApi(GwpEngineApi& api);
void OnObjectDestroyPlanners(const CGameObject* object); // net_Destroy: the planners of the object go
void RemovePluginPlanners(const GwpPlugin* plugin);
// The plugin crashed: its planners call nothing any more and planner_create refuses it, until ForgetPluginCrash
// (its library is unloaded, a new instance may create planners again). Safe inside a planner update.
void StopPluginPlanners(const GwpPlugin* plugin);
void ForgetPluginCrash(const GwpPlugin* plugin);
void ShutdownPlanners();
} // namespace gw::addons::goap
