#pragma once

// Location packs: the world of the game - all.spawn, levels\, the level-dependent configs, scripts data, textures and
// strings. The built-in pack is the one of gamedata (configs\misc\level_pack\level_pack.ltx is its passport); an addon
// of type location is another one: its gamedata\ is mounted over the game data as a layer of the file system while
// the pack is active. One pack is active at a time; it is switched in the main menu (no level, no ALife).
// A save carries the id of the pack it was made on: the persistent data bus key "gw/game/level_pack".
// The location pack is the provider of the slot "world" (plans/addons_api_support/13-extension-model.md): the switch
// is the procedure of the content set (addon_slots.h), the pack is the top of its stack.
// Plans: plans/addons_api_support/10-location-packs.md, plans/level_pack_id/plan.md

#include "xrAddonHost/include/gwp/gwp_api.h" // GwpValue (the save data)

#include "xrCore/ContentMerge/Plan.h" // ContentLayer

namespace gw
{
namespace addons
{
using ContentLayer = xray::content_merge::PlanLayer; // addon_content.h
}
struct LevelPackInfo
{
    xr_string id;
    xr_string version;
    u32 save_compat = 1; // a save of the pack loads only with the same value: raised when all.spawn or levels change
    xr_string name; // string table id
    xr_string description; // string table id, may be empty
    xr_string preview; // texture, may be empty
    xr_string dir; // the addon folder: UTF-8, the trailing separator; empty for the built-in pack
    xr_vector<xr_string> private_masks; // [private] of addon.ltx (addon_content.h)

    bool builtin() const { return dir.empty(); }
};

// After the addon host found the addons (CAI_Space::init), before any pack is mounted: the built-in pack and the
// working addons of type location, in the order of the host.
void InitLevelPacks();

const xr_vector<LevelPackInfo>& LevelPacks(); // the built-in pack first
const LevelPackInfo* FindLevelPack(pcstr id);
const LevelPackInfo& ActiveLevelPack();
void PrintLevelPacks(); // console level_pack_list

// The content set (addon_slots.cpp): the layers of the pack - its gamedata\ with the alias roots and the folder its
// writes go to, one group (nothing for the built-in pack); then the pack became the active one (g_level_pack follows)
void LevelPackLayers(const LevelPackInfo& pack, xr_vector<addons::ContentLayer>& out);
void SetActiveLevelPack(const LevelPackInfo& pack);
// The content set, the pack mounted: one warning per file the game writes (the data of the faction editor, *_custom.ltx)
// that the base has and the pack does not ship - the pack then reads the base one and a write goes into the base
void WarnMissingPackFiles(const LevelPackInfo& pack);

// True when the location pack `id` is installed (one of LevelPacks)
bool IsLevelPackInstalled(pcstr id);

// Console variable g_level_pack (user.ltx): the pack chosen last; the switch writes it. -level_pack <id> overrides it.
extern string64 g_level_pack;

// A switch to the pack `id` (Lua level_pack.switch). It does not run here - it restarts Lua: at the start of a frame
// with the main menu shown and no level (OnFrameLevelPacks). Then the new Lua state gets
// ui_main_menu.on_level_pack_applied(id, action, ok); `action` is up to the menu ("new_game", "load:<save>", "").
// False: an unknown pack, or a game is on (CanLoadSaveLevelPack ends the game itself for a save of another pack).
bool RequestLevelPackSwitch(pcstr id, pcstr action);
// A switch of the content is waiting (a pack, a slot provider, the first mount of the content of the addons)
bool LevelPackSwitchPending();
// The end of CGamePersistent::OnAppStart: the content set of the start is asked for - the common content of the
// addons, the chosen slot providers and the pack of -level_pack / -slot world=<id> / g_level_pack (the game starts on
// the base: one code path for the start and for a switch in the menu)
void RequestStartupLevelPack();
// CGamePersistent::OnFrame, its start (no Lua and no deferred event of the engine on the stack): the content set
void OnFrameLevelPacks();
// The very end of CGamePersistent::OnAppStart (init_game_globals done, no frame yet), a level load waits from the
// command line (-start: KERNEL:start, its server options `op_server`): the menu does not show, so the content set of
// the start is applied now, before the level. A save to load brings its pack and the providers of its save-bound slots
// instead of the choice of the start, as a load from the menu does. When the save cannot load in this run (its pack or
// provider is not installed or does not work, save_compat differs, its content did not mount) the start is refused
// (CanStartLevel): the main menu shows, the content set waits for it as usual, then the menu shows the reason (the
// windows of CanLoadSaveLevelPack). Nothing for a dedicated server (no addon host).
void ApplyStartupContentForLevel(pcstr op_server);
// CGamePersistent::CanStartLevel (KERNEL:start): false once - for the start of the command line refused above; false
// with -addon_strict while the merge of the addons has unresolved conflicts (addon_merge.h StrictMergeBlocks): the
// menu stays (it is turned on again when a new game turned it off), then it shows the window
// ui_main_menu.on_addon_strict_blocked
bool CanStartLevel(pcstr op_server);

// The data a save keeps about its pack (gw_game_state: the persistent data bus keys gw/game/level_pack*)
constexpr pcstr kSaveLevelPackKey = "gw/game/level_pack";
constexpr pcstr kSaveLevelPackCompatKey = "gw/game/level_pack_save_compat";
constexpr pcstr kSaveLevelNameKey = "gw/game/level_name"; // the level the save was made on
constexpr pcstr kSaveLevelPackVersionKey = "gw/game/level_pack_version";

// Before an ALife save is written (alife_storage_manager.cpp, after the Lua save states and the plugins'
// alife_on_before_save): the keys of the content the save needs - gw/game/level_pack, _version, _save_compat,
// gw/game/level_name and gw/slots/<slot>[/version] of the save-bound slots - from the registry of the engine (plan 10
// section 4, P4). What Lua or a plugin wrote into them is replaced: the check before a load (ReadSaveContent) relies
// on them, an overridden gw_game_state.script must not drop them
void WriteSaveContentKeys();

// A string value of the save data (CSavedGameWrapper::data); false when there is none
bool SaveValueString(const GwpValue* value, xr_string& out);

// The check before a save is loaded (console commands load and load_last_save, Lua level_pack.check_save; the start of
// the command line - ApplyStartupContentForLevel, the same rules without the switch and the windows): true - it
// may be loaded now (a missing / invalid save too: the callers check those themselves). A save without the pack key
// (made before it existed) loads on the content of now if its spawn is the one of the active pack (the GUID of
// <name>.spawn; the built-in pack does not compare it - the rule of the base). false - not now, with the reason in the
// log:
//  - its pack is not installed: Lua ui_load_dialog.on_level_pack_missing(save_name, pack_id);
//  - its pack is installed, save_compat differs: Lua ui_load_dialog.on_level_pack_incompatible(save_name, pack_id);
//    the same for a save of the active pack made on another all.spawn (the GUID of the spawn) without save_compat in
//    it, and for a save without the pack key made on another all.spawn than the active (not built-in) pack has
//    (pack_id - the active pack); with the same save_compat the save loads, a warning goes into the log (once per
//    save per load: 00-roadmap.md 5a, decision 64.1);
//  - another installed pack: the switch to it is asked for with the action "load:<save>" (a game on ends first),
//    ui_main_menu.on_level_pack_applied loads the save after it.
// The slots bound to the save (13-extension-model.md, 7: world.weather, the slots an addon declares save_bound) go
// the same way: the key gw/slots/<slot> of the save names its provider - another installed one is switched to with
// the pack; one not installed: Lua gw_slots.on_save_provider_missing(save_name, slot, provider), false.
// -addon_strict with unresolved conflicts of the merge of the addons: Lua ui_main_menu.on_addon_strict_blocked(),
// false - only for a save that loads on the content of now; a save that needs a switch (another pack or provider) is
// checked after the switch, against the merge of its content set (CanStartLevel at the start of its load).
bool CanLoadSaveLevelPack(pcstr save_name);
} // namespace gw
