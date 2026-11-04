#pragma once

// Location packs: the world of the game - all.spawn, levels\, the level-dependent configs, scripts data, textures and
// strings. The built-in pack is the one of gamedata (configs\misc\level_pack\level_pack.ltx is its passport); an addon
// of type location is another one: its gamedata\ is mounted over the game data as a layer of the file system while
// the pack is active. One pack is active at a time; it is switched in the main menu (no level, no ALife).
// A save carries the id of the pack it was made on: the persistent data bus key "gw/game/level_pack".
// Plans: plans/addons_api_support/10-location-packs.md, plans/level_pack_id/plan.md

#include "xrAddonHost/include/gwp/gwp_api.h" // GwpValue (the save data)

namespace gw
{
struct LevelPackInfo
{
    xr_string id;
    xr_string version;
    u32 save_compat = 1; // a save of the pack loads only with the same value: raised when all.spawn or levels change
    xr_string name; // string table id
    xr_string description; // string table id, may be empty
    xr_string preview; // texture, may be empty
    xr_string dir; // the addon folder: UTF-8, the trailing separator; empty for the built-in pack

    bool builtin() const { return dir.empty(); }
};

// After the addon host found the addons (CAI_Space::init), before any pack is mounted: the built-in pack and the
// working addons of type location, in the order of the host.
void InitLevelPacks();

const xr_vector<LevelPackInfo>& LevelPacks(); // the built-in pack first
const LevelPackInfo* FindLevelPack(pcstr id);
const LevelPackInfo& ActiveLevelPack();
void PrintLevelPacks(); // console level_pack_list

// True when the location pack `id` is installed (one of LevelPacks)
bool IsLevelPackInstalled(pcstr id);

// Console variable g_level_pack (user.ltx): the pack chosen last; the switch writes it. -level_pack <id> overrides it.
extern string64 g_level_pack;

// A switch to the pack `id` (Lua level_pack.switch). It does not run here - it restarts Lua: at the start of a frame
// with the main menu shown and no level (OnFrameLevelPacks). Then the new Lua state gets
// ui_main_menu.on_level_pack_applied(id, action, ok); `action` is up to the menu ("new_game", "load:<save>", "").
// False: an unknown pack, or a game is on (CanLoadSaveLevelPack ends the game itself for a save of another pack).
bool RequestLevelPackSwitch(pcstr id, pcstr action);
bool LevelPackSwitchPending();
// The end of CGamePersistent::OnAppStart: the pack of -level_pack / g_level_pack is asked for (the game starts on the
// built-in one: one code path for the start and for a switch in the menu)
void RequestStartupLevelPack();
// CGamePersistent::OnFrame, its start (no Lua and no deferred event of the engine on the stack)
void OnFrameLevelPacks();

// The data a save keeps about its pack (gw_game_state: the persistent data bus keys gw/game/level_pack*)
constexpr pcstr kSaveLevelPackKey = "gw/game/level_pack";
constexpr pcstr kSaveLevelPackCompatKey = "gw/game/level_pack_save_compat";
constexpr pcstr kSaveLevelNameKey = "gw/game/level_name"; // the level the save was made on

// A string value of the save data (CSavedGameWrapper::data); false when there is none
bool SaveValueString(const GwpValue* value, xr_string& out);

// The check before a save is loaded (console commands load and load_last_save, Lua level_pack.check_save): true - it
// may be loaded now. A save without the pack key (or a missing / invalid one: the callers check those themselves)
// may be loaded. false - not now, with the reason in the log:
//  - its pack is not installed: Lua ui_load_dialog.on_level_pack_missing(save_name, pack_id);
//  - its pack is installed, save_compat differs: Lua ui_load_dialog.on_level_pack_incompatible(save_name, pack_id);
//  - another installed pack: the switch to it is asked for with the action "load:<save>" (a game on ends first),
//    ui_main_menu.on_level_pack_applied loads the save after it.
bool CanLoadSaveLevelPack(pcstr save_name);
} // namespace gw
