#pragma once

// Slots (plans/addons_api_support/13-extension-model.md, section 4): an extension point with exactly one active
// candidate - the main menu, the PDA, the location pack ... The base declares its slots in
// configs\misc\slots\slots.ltx; an addon may declare its own ([slots_declare] of addon.ltx, "<addon id>.<name>").
// An addon provides a slot with [slot.<slot>]:
//   [slot.ui.main_menu]
//   name        = st_my_menu_name        ; string table id (the default: the addon id)
//   description = st_my_menu_desc        ; string table id, may be absent
//   preview     = ui\my_menu\preview     ; texture for a window of choice (the common gamedata\textures\ of the
//                                        ; addon; not ui\@<id>\ - a layer serves no '@' folder), may be absent
//   files       = slots\main_menu\       ; its set: <addon>\<files>\gamedata\ (the default: slots\<slot>\)
//   entry       = my_menu_main           ; Lua: <entry>.on_slot_active(slot, provider) in each Lua state it is active
//   [slots_declare]
//   my_menu.theme = st_my_menu_theme_slot[, save_bound][, restart]
// The base is the provider "core" of every slot: nothing is mounted for it. The location packs are the providers of
// the slot "world" (level_pack.h): the built-in pack is its base.
//
// The content set (13-extension-model.md, 4.5): the layers of the file system over the game data, bottom up - the
// common content of the addons (<addon>\gamedata\ of every working addon but the location packs, in the order of the
// addons), the sets of the chosen providers (in the order of their addons), the location pack. One procedure applies
// it: at the first frame of the main menu after the start (or before the level when the command line starts one right
// away: ApplyStartupNow) and at each switch in the menu (no level, no ALife) - the
// readers of the file system stop, the on-demand plugins of the providers and the pack that go off are unloaded, the
// layers change, the configs are read again with the data of init_game_globals (the info portions, the encyclopedia,
// the dialogs, the profiles, the communities and creatures\relations, the ranks, the reputations), the plugins of the
// new ones load, the strings and the caches are reset, the Lua of the menu restarts. Not from an addon: gamemtl.xr and
// the catalog of the slots (configs\misc\slots\slots.ltx, mod_slots_*.ltx) - they are read before anything is mounted,
// the log warns of them; an addon declares its slots with [slots_declare]. An addon of type location without
// gamedata\spawns\all.spawn is no pack: its gamedata\ is the common content of an addon. Then Lua gw_slots.on_slots_applied(action, ok) and, for a switch of the pack or a
// save to load, ui_main_menu.on_level_pack_applied(pack, action, ok).
//
// The choice: appdata\addons.ltx [slots] (the launcher and the game write it), -slot <slot>=<provider> for one run;
// world - g_level_pack / -level_pack, as before. A slot bound to the save (world.weather, an addon slot declared
// save_bound) also comes with the save: the key gw/slots/<slot> (Lua gw_slots writes it, level_pack.cpp checks it).

namespace gw::slots
{
constexpr pcstr kWorldSlot = "world";
constexpr pcstr kCoreProvider = "core";
constexpr pcstr kSaveSlotKeyPrefix = "gw/slots/"; // + <slot>: the provider id; + <slot> + "/version": its version

struct ProviderInfo
{
    xr_string id; // the addon id; "core" - the base (world: the id of the location pack)
    xr_string name; // string table id
    xr_string description; // string table id, may be empty
    xr_string preview; // texture, may be empty
    xr_string entry; // Lua module, may be empty
    // Lua module of its panel in the appearance dialog (ui_mm_appearance.script), in the common content of the
    // addon (so it is there while the provider is not chosen); may be empty
    xr_string panel;
    xr_string version;
    xr_string folder; // the set: UTF-8, the trailing separator; empty - no files (core, world: level_pack.cpp)
    xr_vector<xr_string> private_masks; // [private] of the addon
    u32 order = 0; // the place of its addon in the order of the addons: the order of the sets in the stack
    bool builtin = false; // core; world: the built-in pack
};

struct SlotInfo
{
    xr_string id; // "ui.main_menu", "<addon id>.<name>"
    xr_string name; // string table id
    xr_string owner; // "core" - the base declared it, else the addon
    bool save_bound = false; // its provider comes with the save (world, world.weather, ...)
    bool restart = false; // a choice takes effect at the next start of the game only (fonts, cursor, intro)
    xr_vector<ProviderInfo> providers; // the base first
    size_t active = 0; // in providers

    const ProviderInfo* find(pcstr provider) const;
    const ProviderInfo& active_provider() const { return providers[active]; }
};

// InitLevelPacks, after the registry of the packs: the slots, their providers, the common content of the addons
void Init();
const xr_vector<SlotInfo>& Slots(); // empty before Init (a dedicated server)
const SlotInfo* FindSlot(pcstr slot);
// The required on-demand plugin of the addon did not load in this run: its pack / providers cannot be active
bool AddonFailed(pcstr addon_id);
// -slot <slot>=<provider> of the command line (repeatable, the last one of a slot counts)
bool CommandLineChoice(pcstr slot, xr_string& provider);

// A switch of the provider of a slot in the main menu (Lua addon_slots.select, console slot_select): the choice goes
// into appdata\addons.ltx [slots]; the switch runs at the next frame of the menu, then the Lua callbacks get `action`.
// A slot that takes effect after a restart only keeps the choice. world: as level_pack.switch (core - the built-in
// pack). False: an unknown slot or provider, a provider that cannot work, a game is on.
bool RequestSelect(pcstr slot, pcstr provider, pcstr action);
// A rule of appdata\addons.ltx [conflicts] changed (console addon_file_winner): the content stack is built again at the
// next frame of the main menu, as a switch of a slot (the same procedure; the Lua callbacks get `action`). False - a
// game is on (the rule takes effect at the next build: a switch in the menu, the next start) or no slots (a server)
bool RequestRebuild(pcstr action);
// level_pack.cpp: a switch of the pack (level_pack.switch), the providers a save needs with its pack (nullptr - the
// active pack), the content of the start (nullptr - the built-in pack; nothing to mount - nothing asked for)
bool RequestWorld(pcstr pack_id, pcstr action);
void RequestForSave(pcstr pack_id, const xr_vector<std::pair<xr_string, xr_string>>& providers, pcstr action);
void RequestStartup(pcstr pack_id);
// The start of the command line loads a save (level_pack.cpp ApplyStartupContentForLevel): its pack and the providers
// of its save-bound slots replace the choice of the start (-level_pack, g_level_pack, -slot, [slots]) in the content
// set of the start. Nothing is asked for when there is nothing to mount
void RequestStartupForSave(pcstr pack_id, const xr_vector<std::pair<xr_string, xr_string>>& providers);
// The content set of the start right away, the main menu is not shown: the end of CGamePersistent::OnAppStart when a
// level load waits from the command line (no level, no ALife, no Lua, no plugin and no deferred event on the stack).
// The Lua of the menu restarts without a rebuild of the menu; gw_slots.on_slots_applied runs, the callback of the menu
// (ui_main_menu.on_level_pack_applied) does not: the level load goes on itself
void ApplyStartupNow();
bool ApplyPending();
// CGamePersistent::OnFrame through OnFrameLevelPacks: the switch asked for, in the main menu without a level
void OnFrame();
// The number of the content set: grows by one with each ApplyContentSet (the first mount of the start, a switch of
// the pack or of a provider, a rebuild of the stack), 0 before the first one. An engine cache of the data of the
// files (not of Lua: Lua restarts with each content set) keeps the number it was filled at and reads again when it
// differs - cheaper than a list of the caches to reset
u32 ContentGeneration();

// The provider the next start takes: world - g_level_pack, the rest - appdata\addons.ltx [slots] (core when none)
xr_string NextStartProvider(const SlotInfo& slot);
void PrintSlots(); // console slot_list
} // namespace gw::slots
