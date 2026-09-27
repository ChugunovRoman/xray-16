#pragma once

// Plugin API, group "ini": reading ltx configs by handles.
//  - A section is resolved once into a handle; reads by handle skip the lowercase copy and the two binary searches
//    that every read by name costs inside CInifile.
//  - Nothing here ever stops the game: a missing section, a missing line or a stale handle gives the default value
//    the caller passed (CInifile::r_* calls Fatal instead, so they are never reached without a check).
// Docs: wiki/doc/plugins/api/ini.md; plan: plans/lua_to_cpp/07-stage-c-service-api-groups.md

#include "xrAddonHost/include/gwp/gwp_api.h"

struct GwpPlugin;

namespace gw::addons::ini
{
void FillEngineApi(GwpEngineApi& api);

// Configs were reloaded (reload_system_ini and friends): every section handle and every string of a config
// becomes invalid. Called from xrServerEntities/script_ini_file_script.cpp.
void OnConfigsReloaded();

void ClosePluginFiles(const GwpPlugin* plugin); // the plugin is unloaded: close what ini_open gave it
void Shutdown();
void PrintList(); // console command "ini_list"
} // namespace gw::addons::ini
