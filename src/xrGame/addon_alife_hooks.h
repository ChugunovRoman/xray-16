#pragma once

// Plugin API, group alife: alife_update_hook_register / _set_mode / _unregister - the offline update of squads and
// smart terrains by a plugin (plans/lua_to_cpp/31-gw-eoc-e36-spawn-api.md, 4.4). The ALife scheduler
// (CALifeScheduleRegistry::CUpdatePredicate, alife_schedule_registry.h) asks Update only while g_active is not 0;
// without an active hook an update is the virtual update() as before: one load and one compare more.
// Docs: wiki/doc/plugins/api/alife.md

#include "xrAddonHost/include/gwp/gwp_api.h"

class CSE_ALifeSchedulable;

namespace gw::addons::alife_hooks
{
// Hooks whose mode is not GWP_UPDATE_LUA (the only ones Update acts on). Main thread.
extern u32 g_active;

void FillEngineApi(GwpEngineApi& api);

// The scheduled update of `object` through the active hook of its class: true when it was done here (the Lua update
// included where the mode wants it), false when the caller updates the object as usual (object.update()). Main thread.
bool Update(CSE_ALifeSchedulable& object);

// StopPlugin (a crash, an unload): the hooks of the plugin go, their objects get the Lua update again. Safe inside a
// hook call.
void RemovePluginHooks(const GwpPlugin* plugin);

void Shutdown();

// Part of the console command callback_list: every hook with its counters.
void PrintList();
} // namespace gw::addons::alife_hooks
