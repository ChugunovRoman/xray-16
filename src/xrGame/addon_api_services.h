#pragma once

// Plugin API, group services (addon_api_services.cpp): interfaces published by plugins for other plugins.

#include "xrAddonHost/include/gwp/gwp_api.h"

namespace gw::addons::services
{
void FillEngineApi(GwpEngineApi& api);
// The services of a stopped plugin (unload, crash): removed, each announced with service_on_unregister
void RemovePluginServices(const GwpPlugin* plugin);
// The plugin loaded: its services published during the init are announced now (service_on_register)
void AnnouncePluginServices(const GwpPlugin* plugin);
void Shutdown();
void PrintList(); // addon_list
} // namespace gw::addons::services
