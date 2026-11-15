#pragma once

// Plugin API, the functions of plans/addon_api_ideas (group plugin) that have no file of their own group
// (addon_api_extra.cpp): the files of the game data (fs_*), game_save and the event game_on_before_save, item_params,
// the debug shapes (draw_*), input (input_*), level_objects_in_radius, character_start_dialog, object_luminosity,
// npc_visibility_of. Docs: wiki/doc/plugins/api/ini.md, save.md, items.md, level.md, character.md, npc.md.

#include "xrAddonHost/include/gwp/gwp_api.h"

struct GwpPlugin;

namespace gw::addons::extra
{
void FillEngineApi(GwpEngineApi& api);

// game_on_before_save: called by CCC_ALifeSave::Execute (kind "quicksave" / "save") and CAutosaveManager
// ("autosave") right before the save is sent; name - the name of the save without the extension. false - a subscriber
// cancelled it (logged): the caller does not save. Without subscribers: true at once.
bool BeforeGameSave(pcstr name, pcstr kind);

// The plugin stops: its debug shapes go (addon_host.cpp ReleasePlugin)
void RemovePluginResources(const GwpPlugin* plugin);
} // namespace gw::addons::extra
