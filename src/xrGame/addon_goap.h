#pragma once

// Plugin API, group "goap": native evaluators in the planners of the stalkers. Wave W0 (W0.3 of the plan).
//  - A plugin registers a function for a world property once; the host wraps the Lua evaluator of that property in
//    every stalker with a proxy (CNativeEvaluatorProxy) that asks the plugin, the Lua evaluator or both, by the mode
//    of the registration. The Lua evaluator is kept inside the proxy, so switching the mode is instant and the
//    property goes back to Lua when the plugin leaves.
//  - The planner owns the proxy like any evaluator (it deletes it in clear/remove_evaluator); the proxy owns the Lua
//    evaluator it wraps. The host keeps no owning pointers, only the list of live proxies for the statistics.
//  - The planners are changed only between frames of the scheduler (ApplyPending): never during a solve, where
//    VERIFY2(!m_solving) does not protect a release build.
//  - Nothing is written to the save: save/load of the proxy go to the wrapped Lua evaluator, byte for byte.
// Docs: wiki/doc/plugins/api/goap.md; plan: plans/lua_to_cpp/08-wave-w0-native-npc-foundation.md (W0.3)

#include "xrAddonHost/include/gwp/gwp_api.h"

class CGameObject;
struct GwpPlugin;

namespace gw::addons::goap
{
void FillEngineApi(GwpEngineApi& api);

// A stalker is online (its Lua binder net_spawn, where the schemes add their evaluators, succeeded): the
// registrations are applied to it at the next ApplyPending.
void OnObjectSpawn(const CGameObject* object);
// After the scheduler pass (CLevel::VisionBatchPostScheduler): installs and removes proxies.
void ApplyPending();
// The active_section of the object changed (db.storage mirror): a scheme may add its evaluators now, the stalker is
// tried again for a while. Cheap for anything else: the queue drops non-stalkers.
void OnLogicChanged(u16 id);
// A new Lua state (new game, load, level change): the queue of stalkers to try is dropped with the old objects
void OnScriptRestart();

void RemovePluginEvaluators(const GwpPlugin* plugin); // unload: the properties go back to Lua
void Shutdown();
void PrintList(); // console command "evaluator_list"
} // namespace gw::addons::goap
