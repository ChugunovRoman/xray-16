#pragma once

// Addons host: discovers addons by their addon.ltx manifests and loads their plugins.
//  - Addon:  game package (models, textures, configs, levels, shaders, Lua scripts, plugins).
//  - Plugin: native C/C++ library inside an addon, loaded as native code through the Plugin API.
// Docs:   wiki/doc/plugins
// Design: plans/lua_to_cpp/03-build-and-discovery.md (section 4),
//         plans/addons_api_support/08-unified-addons-api-lua-native.md (sections 4.2, 4.3, 4.8).
// Plugin API (C ABI): xrAddonHost/include/gwp/gwp_api.h

#include "xrAddonHost/include/gwp/gwp_api.h"

#include <filesystem>
#include <system_error>

class IReader;
class IWriter;

namespace gw::addons
{
// Discovers addons and loads their plugins. Call once per process on the main thread,
// after the file system is initialized and before any Lua code runs.
void Initialize();

// Calls on_unload of every loaded plugin (reverse load order) and unloads the libraries.
void Shutdown();

// Prints every discovered addon with its plugin state to the log (console command "addon_list"), then a legend of the
// states it shows.
void PrintList();

// A plugin crashed in one of its functions (a guard of the host caught it, the crash block with the stack is in the
// log). It stops at once: every group of the engine drops what it registered - the subscriptions, binders, engine
// callbacks and exports go, its native evaluators and actions go back to Lua, its planners stop. The library is
// unloaded at the start of the next frame and loaded again at the next game load, at most kMaxAutoRestarts times
// per run of the game (and by the console command plugin_reload). Safe inside a dispatch. Main thread.
void OnPluginCrashed(const GwpPlugin* plugin, pcstr what);

// Start of a frame (CGamePersistent::OnFrame), no plugin code on the stack: the unloads of crashed plugins and the
// reloads asked for by plugin_reload.
void OnFrame();

// A game session starts (CALifeSimulator, right before alife_on_start): crashed plugins are loaded again, so they
// get alife_on_start like at a normal start.
void OnGameStart();

// Console command plugin_reload <addon id>: the plugin of the addon is unloaded (when loaded) and loaded again from
// its library - a rebuilt one too - at the start of the next frame. false with a reason for an unknown addon.
bool RequestPluginReload(pcstr addon_id, xr_string& reason);
// Console command addon_reload <addon id> (08-unified-addons-api-lua-native.md 4.10, U6): at the start of the next
// frame the addon goes and comes back - its Lua part (on_unload, its save data in memory, its registrations and
// modules go: addon_lua_entry.h UnloadLuaEntryForReload), its plugin (as plugin_reload), its Lua part again (on_load,
// in a game on_game_start, load_state with the kept data, the level callbacks). A development command: allowed for
// an addon with trust = local, one not in appdata/addons.ltx (-addon_dir, no launcher list) or any with
// -addon_debug; refused while a level loads or a save is written. false with a reason when refused.
bool RequestAddonReload(pcstr addon_id, xr_string& reason);

// Plugin save data (save_write/save_read): a block at the end of the ALife save stream (alife_storage_manager.cpp).
void ResetSaveData();                // new game: forget the data of the previous one
void WriteSaveData(IWriter& stream); // writes its own chunk into the ALife save stream
void ReadSaveData(IReader& stream);  // finds that chunk; a save without it gives no data

// Internal: shared by the parts of the host (addon_event_bus.cpp, addon_data_bus.cpp, addon_api_objects.cpp).
pcstr PluginAddonId(const GwpPlugin* plugin); // "?" for an unknown handle
bool IsMainThread();
bool IsDebugLog(); // -addon_debug
bool IsPluginLoaded(pcstr addon_id); // the addon is known and its plugin is loaded (and not unloaded yet)
bool IsPluginLoading(const GwpPlugin* plugin); // inside its gwp_plugin_init
bool IsPluginRunning(const GwpPlugin* plugin); // loading or loaded: not stopped by a crash, not unloaded
// GwpPluginDesc::api_built (GWP_MAKE_VERSION) of the loaded plugin of the addon; 0 when the addon is unknown, its
// plugin is not loaded or its description is too short to have the field (Lua plugins.version).
u32 PluginApiBuilt(pcstr addon_id);
// Ids of every addon found at the start - loaded, failed, skipped or disabled - known before the first plugin loads.
// Empty before Initialize and after Shutdown.
const xr_vector<xr_string>& KnownAddonIds();
// A discovered addon as the other parts of the engine see it (level_pack.cpp, addon_slots.cpp): a copy
struct AddonInfo
{
    xr_string id;
    xr_string version; // as written in the manifest
    xr_string type;    // [addon] type as written; "content" when the manifest has none
    xr_string dir;     // the addon folder: UTF-8, the trailing separator
    bool active = false; // works: enabled, its dependencies hold, its required plugin loaded
    // The state as addon_list shows it (the report of the addons, addons_report.json): discovered, active, failed,
    // skipped, disabled; the plugin (empty - none) and its state; why the addon or its plugin does not work
    xr_string state;
    xr_string plugin;
    xr_string plugin_state;
    xr_string reason;
    // [addon] load_after of addon.ltx: the addons it loads after when they are there (the ids, the valid ones only).
    // The order of the addons keeps it (ResolveDependencies) - the plugins, the content stack, the patches
    xr_vector<xr_string> load_after;
    // [addon] depends of addon.ltx: the ids of the addons it depends on (the conditions on their versions are checked
    // by the host: active means they hold)
    xr_vector<xr_string> depends;
    // [addon] patches of addon.ltx (15-merge-implementation.md 6.1): a compatibility patch of these addons - it works
    // only while all of them do (else skipped) and loads after them; its fragments <dir>merge\gamedata\<file> take
    // part in the merge of the files they replace (addon_merge.h), merge\ is not mounted
    xr_vector<xr_string> patches;
    // [addon] game_version of addon.ltx (15-merge-implementation.md 2.2; 00-roadmap.md 5a, decision 47): the release
    // tag of the game its replacements of the files of the base are made for ("v1.3.0"), as written; empty - not set.
    // The merge warns when it is older than the tag of the game (configs\gw_release.ltx) and the addon has no usable
    // delta (none, or a stale one)
    xr_string game_version;
    xr_vector<std::pair<xr_string, xr_string>> location; // [location] of addon.ltx, as written
    xr_vector<xr_string> private_masks; // [private] of addon.ltx: one mask per line, as written (addon_content.h)
    // [lua] entry of addon.ltx: the Lua module of the addon (its gamedata\scripts\<entry>.script), empty - none or
    // not a module name (the log has the reason); when it runs - addon_lua_entry.h
    xr_string lua_entry;
    // plans/addons_api_support/13-extension-model.md, 4.3: [slot.<slot>] sections (the addon provides the slot; the
    // keys as written, addon_slots.cpp reads them) and [slots_declare] (the slots the addon declares itself)
    struct SlotSection
    {
        xr_string slot; // lower case, as the section name after "slot."
        xr_vector<std::pair<xr_string, xr_string>> keys;
    };
    xr_vector<SlotSection> slots;
    xr_vector<std::pair<xr_string, xr_string>> slots_declare;
};
// Every discovered addon - working or not - in the order of discovery (appdata/addons.ltx when it lists them).
// Empty before Initialize and after Shutdown.
xr_vector<AddonInfo> ListAddons();

// The plugins that load on demand (plans/addons_api_support/00-roadmap.md 5a, decision 1): the plugin of an addon of
// type location or of an addon that provides slots ([slot.<slot>]) is not loaded at the start; it is loaded while the
// location pack or one of the slot providers of its addon is active and unloaded when they are switched off (the
// content set, addon_slots.cpp). The plugins of the other addons load and stay as before. Main thread, no plugin
// code on the stack (the start of a frame).
//  - UnloadOnDemandPlugins: the loaded on-demand plugins of the addons not in `active` go (on_unload, then every
//    registration of theirs, as plugin_reload does);
//  - LoadOnDemandPlugins: the plugins of the addons in `active` that are not loaded yet load; `failed` gets the
//    addons whose required plugin did not load (the addon failed: its pack / provider cannot be active).
void UnloadOnDemandPlugins(const xr_vector<xr_string>& active);
void LoadOnDemandPlugins(const xr_vector<xr_string>& active, xr_vector<xr_string>& failed);

// appdata/addons.ltx, section [slots] (13-extension-model.md, 4.4): the provider chosen for each slot, as read at the
// start - "<slot> = <provider id>", in the order written. The launcher and the game write it.
const xr_vector<std::pair<xr_string, xr_string>>& SlotChoices();
// Writes "<slot> = <provider>" into [slots] of appdata/addons.ltx (provider empty or "core": its line goes); every
// other line of the file stays as it is. No file - it is created with [slots] only: a file without [addons] and
// [addon.<id>] sections is no list of addons (the addon folders are scanned as without the file). The choices of this
// run (SlotChoices) change only once the file is written. False - not written (the log says why).
bool WriteSlotChoice(pcstr slot, pcstr provider);

// appdata/addons.ltx, section [conflicts] (13-extension-model.md 4.2, 4.4; 00-roadmap.md 5a, decision 11): the rules
// of the player for the files more than one addon gives, over the order of the addons. The launcher and the game
// write it (the game: console addon_file_winner), as [slots]:
//   [conflicts]
//   pair:<winner id> = <loser id>[, <loser id> ...]  ; the winner lies over the losers in the content stack
//   file:<file> = <addon id | core>                  ; that addon serves the file (relative to gamedata\), core - the base
//   atom:<file>|<atom> = <addon id | core>           ; the merge of the file takes the atom from that addon (core - the
//                                                    ; base): 15-merge-implementation.md 4.4, addon_merge.h
// (the game: console addon_pair for the pair rules, addon_atom_winner for the atom rules)
// Applied at each build of the content stack (addon_content.cpp SyncContentLayers).
struct PairRule
{
    xr_string winner;
    xr_string loser;
};
struct FileRule
{
    // relative to gamedata\: '\' between the folders, lower case on Windows - the names of the file system index
    // (NormalizeRuleFile)
    xr_string file;
    xr_string winner; // the addon id or "core"
};
// atom:<file>|<atom> = <winner>: one atom of a merged file (the label of addon_merges: "ranks.parse_ranks",
// "[settings] always_help_distance", "string_table/string[id=ls_header]/text#text"). In the line of addons.ltx the
// atom keeps its case; '%', '=', ';' and the second '/' of "//" are written as %25, %3D, %3B, %2F (else the line would
// not read back: the value, a comment) - EncodeRuleAtom / DecodeRuleAtom
struct AtomRule
{
    xr_string file; // as FileRule::file
    xr_string atom; // the label as the merge names it (decoded)
    xr_string winner; // the addon id or "core"
};
const xr_vector<PairRule>& PairRules(); // in the order written: an earlier rule wins over a later one it contradicts
const xr_vector<FileRule>& FileRules();
const xr_vector<AtomRule>& AtomRules(); // the first rule of a file and an atom counts
xr_string EncodeRuleAtom(const xr_string& atom);
// False - not a label: empty, a line end, a bad %XX
bool DecodeRuleAtom(const xr_string& text, xr_string& atom);
// A file of a rule: '/' -> '\', no leading separator, lower case where the file system index is (Windows; on POSIX
// the case stays, as the index keeps the names of the disk); false - not a file name (empty, "..", ':', a mask)
bool NormalizeRuleFile(pcstr text, xr_string& out);
// Seconds since 1970 (UTC): the last moment the rules of this run (PairRules, FileRules, AtomRules) were what
// appdata/addons.ltx [conflicts] has - the read at the start, or a write of the game (WritePairRule, WriteFileRule,
// WriteAtomRule, WriteSlotChoice) over a file nobody else changed since. The file is not read again in the run: a
// change of the launcher after this time is not in the rules
u64 ConflictRulesReadAt();
// Writes "pair:<winner> = <loser>[, ...]" into [conflicts] of appdata/addons.ltx: `loser` is added to the losers of the
// winner (loser empty or nullptr: the line of the winner goes), the reverse rule pair:<loser> = <winner> goes; the
// lines are changed from what the file has at the moment of the write (a loser the launcher added while the game runs
// stays), the other lines of the file stay as they are. The rules
// of this run follow (PairRules) - they change only once the file is written. False - not an addon id (or loser ==
// winner), or the file is not written; `error` (optional) - why, for the console ("cannot write <file>: <reason>")
bool WritePairRule(pcstr winner, pcstr loser, xr_string* error = nullptr);
// Writes "file:<file> = <winner>" into [conflicts] of appdata/addons.ltx (winner empty: its line goes); the other lines
// of the file stay as they are. The rules of this run follow (FileRules) - they change only once the file is written.
// `error` (optional) - why it is false, as for WritePairRule
bool WriteFileRule(pcstr file, pcstr winner, xr_string* error = nullptr);
// Writes "atom:<file>|<atom> = <winner>" into [conflicts] of appdata/addons.ltx (winner empty: its line goes), as
// WriteFileRule; the rules of this run (AtomRules) change only once the file is written. "@gap X" (a changed gap of a
// delta) and "X" are one atom: the line is written as "X" and the line of the other spelling goes - one line per atom,
// as the launcher writes it
bool WriteAtomRule(pcstr file, pcstr atom, pcstr winner, xr_string* error = nullptr);
// The atom as WriteAtomRule writes it: "@gap X" -> "X", trimmed; empty - not an atom
xr_string NormalizeRuleAtom(pcstr atom);
// The addon that owns a name "<addon id>_...": the longest known id followed by '_' at the start of `name`, nullptr
// when none. With addons "gw" and "gw_npc" known, "gw_npc_x" belongs to gw_npc even before (or without) its plugin.
pcstr NameOwnerAddonId(pcstr name);
// The paths of the file system (00-roadmap.md 5a, decision 33): the engine keeps every path in UTF-8 (xrCore: the
// working folder through FromWide, the files through CreateFileW(Utf8::ToWide)), while std::filesystem::path made of
// a narrow string reads it in the ANSI code page on Windows - a folder with Cyrillic in its name would name another
// folder. Every std::filesystem::path of the addons (the host, the content, the slots, the packs) goes through these:
//  - PathFromUtf8: UTF-8 text (an FS path, appdata/addons.ltx, the command line) -> path, '\' as the separator of the
//    OS on POSIX. Not valid UTF-8 (a file saved in a legacy code page) - read in the ANSI code page on Windows;
//  - PathToUtf8: path -> UTF-8, never throws (path::string() does on Windows for a character the code page lacks).
std::filesystem::path PathFromUtf8(const xr_string& text);
xr_string PathToUtf8(const std::filesystem::path& path);
// The text of an error for the log, in UTF-8: std::error_code::message() is in the ANSI code page on Windows (the log
// would warn about text that is not UTF-8) - there the system text comes through FormatMessageW. No trailing period
xr_string ErrorText(const std::error_code& ec);
// Reads a zero-terminated string without leaving the reader (IReader::r_stringZ does not check bounds).
bool ReadStringZChecked(IReader& reader, xr_string& out);
void FillObjectsApi(GwpEngineApi& api);
void FillLevelApi(GwpEngineApi& api);
void FillInfoApi(GwpEngineApi& api);
void FillNpcApi(GwpEngineApi& api); // groups npc and script (addon_api_npc.cpp)
void ShutdownNpcApi();
void FillExportsApi(GwpEngineApi& api); // script_export (addon_script_exports.cpp)
void FillNpcControlApi(GwpEngineApi& api); // groups npc_body, npc_sight, npc_anim, inventory, smart_cover
void RemovePluginExports(const GwpPlugin* plugin);
void ShutdownExports();
// script_call_list: counters of script_call (plugin -> Lua) and of plugins.call (Lua -> plugin); `reset` zeroes them
void PrintScriptCallList(bool reset);
void PrintExportCallList(bool reset);
struct lua_State* CallingLuaThread(); // the Lua thread of the plugins.call in progress, nullptr outside one
} // namespace gw::addons
