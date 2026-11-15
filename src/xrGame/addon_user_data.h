#pragma once

// Files of an addon in the profile of the user (plans/addon_achievements/plan.md 7.1, 00-roadmap.md 5a rows 66-67):
// one folder per addon, <root>/addons/<addon id>/, outside the game folder and the saves - shared by every save, every
// game folder of this user and every copy of the game running at once.
//  - The root lies next to the data of the launcher (Tauri AppConfig of the identifier com.ruut.stalker,
//    launcher-new/src-tauri/tauri.conf.json): Windows %APPDATA%\com.ruut.stalker\game\ (FOLDERID_RoamingAppData),
//    Linux $XDG_CONFIG_HOME (without it ~/.config)/com.ruut.stalker/game/, macOS
//    ~/Library/Application Support/com.ruut.stalker/game/. The command line key -user_data <folder> replaces it
//    (tests, a portable installation). The engine creates the folders.
//  - Paths are UTF-8 inside and wide text for the OS on Windows: any name of the profile works.
//  - A write replaces the file atomically (a temporary file of the same folder, flushed, then MoveFileExW
//    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH / rename + fsync of the folder); a lock "<name>.lock"
//    (Windows: no sharing, deleted on close; POSIX: flock) serializes read - merge - write between the copies of the
//    game.
//  - An addon reaches only its own folder: plugins by their handle (self), Lua by the module of the calling code
//    (addons.user_data, addons.script). The scripts of the base get the folder "core"; an addon with the id "core"
//    gets none (refused, logged once).
// Plugins: user_data_* of GwpEngineApi (gwp_api.h, group user_data). Lua: _gw_internal.user_data_open, internal to
// addons.script (the API is addons.user_data). Docs: wiki/doc/plugins/api/user_data.md, wiki/doc/addons/lua_api.md.

#include "xrAddonHost/include/gwp/gwp_api.h"

struct GwpPlugin;

namespace gw::addons::user_data
{
void FillEngineApi(GwpEngineApi& api);

// The plugin unloads (or crashed): the locks it still holds are released (logged).
void ReleasePluginLocks(const GwpPlugin* plugin);
} // namespace gw::addons::user_data
