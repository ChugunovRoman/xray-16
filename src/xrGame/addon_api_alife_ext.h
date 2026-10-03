#pragma once

// Plugin API: ALife on write and the squads of the simulation (groups alife and squads, addon_api_alife_ext.cpp):
// spawn and release, listing the server objects, story objects, teleport, online/offline switching, kill, the
// squads; and the events server_object_on_register / server_object_on_unregister.
// Docs: wiki/doc/plugins/api/alife.md, wiki/doc/plugins/api/squads.md

#include "xrAddonHost/include/gwp/gwp_api.h"

class CSE_ALifeDynamicObject;

namespace gw::addons
{
void FillAlifeExtApi(GwpEngineApi& api);

namespace alife_ext
{
// The object is registered in ALife and its on_register (the Lua binder se_*) ran: server_object_on_register.
// Called from CALifeSimulatorBase::register_object and from the on_register loops of a load and a new game.
void OnServerObjectRegistered(CSE_ALifeDynamicObject* object);

// The object is about to leave ALife (CALifeSimulatorBase::unregister_object, before its on_unregister):
// server_object_on_unregister.
void OnServerObjectUnregistering(CSE_ALifeDynamicObject* object);

// A dispatch of server_object_on_register / _on_unregister is in progress: the engine may be walking the registry
// (the on_register loop of a load), so spawns and releases are refused - the Plugin API ones here, the Lua ones of
// alife_simulator_script.cpp through this check.
bool InRegistryEvent();
} // namespace alife_ext
} // namespace gw::addons
