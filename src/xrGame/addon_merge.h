#pragma once

// The merge of the replacements of one text file by several addons (plans/addons_api_support/15-merge-implementation.md,
// S8; 13-extension-model.md 4.6-4.7; 00-roadmap.md 5a, decision 47).
//
// Two addons or more replace one file of the base (.ltx, .xml, .script) as a whole: without the merge the upper one
// in the content stack serves it and the changes of the others are lost silently. PlanContentMerge (a step of
// SyncContentLayers, after the rules pair: and file: of the player) merges such files by atoms - a key of an ltx
// section, a node of an xml, a top level statement of a Lua script - with the base file as the ancestor (xrCore
// ContentMerge, the same library as the tool gw_merge of the catalog CI). The merged files go into one more layer of
// the content stack, addons_merged (<appdata>\addons_merged\<set>\gamedata\): over the sets of the slot providers,
// under the location pack. The readers of the files (scripts, configs, xml) take them from the file system as usual.
//
// Not merged (the upper layer serves the file, as without the merge): a file one addon replaces (unless it has a delta
// or an atom: rule names the file: below), a file the base does not have (two addons bring the same new file), a
// file:<file> rule of the player in appdata\addons.ltx [conflicts], a file the location pack gives, a patch file
// mod_<stem>_*.ltx / .xml (its layer orders it already).
//
// One atom two addons changed differently - a conflict, decided in this order (15 section 3.3):
//  1. the rule of the player atom:<file>|<atom> = <id | core> of appdata\addons.ltx [conflicts] (console
//     addon_atom_winner) - resolved_by "player";
//  2. a compatibility patch ([addon] patches = A, B of its addon.ltx, 15 section 6.1) made for all the addons of the
//     conflict whose fragment <patch>\merge\gamedata\<file> has the atom - resolved_by "patch". The fragment is a side
//     of the merge over the addons: an atom it does not have is not touched. <patch>\merge\targets.ltx keeps the
//     sha256 of the files of A and B the fragment is made for - another version installed: patch_stale;
//  3. the order of the stack (pair:, load_after, the order of the addons): the atom of the upper side wins as a whole,
//     the rest is merged - an unresolved conflict.
// A merged text that does not compile (Lua, luaL_loadbuffer in a separate lua_State) or parse (ltx, xml) is not
// served: the upper layer serves the file (fallback). The results are cached by the hash of the inputs
// (<appdata>\addons_merged\cache\); a set of merged files is reused while its list is the same (several instances of
// the game may share the appdata: a set is renamed in whole and never rewritten, addon_merge.cpp).
// S9 (15 sections 2.2-2.4, 7.3): an addon of the catalog has its delta <addon>\.gw_merge\delta.json (the launcher
// writes it, the recomputed one for the version of the parse of the engine when the catalog has it; the only file the
// engine reads, its "algo" is checked): for each
// file it replaced, the atoms it changed with their texts in the base of its version. Its side is carried over onto
// the current base first (ApplyDelta: the atoms the delta does not have come from the base - a change of the base is
// not undone), also when the file is replaced by this addon only (the layer addons_merged serves the result). The
// file of the addon edited after the publication (sha256), a delta of another version of the parse, a broken delta or
// an entry that does not agree with itself or with the file (a delta written by hand) - stale_delta: the delta is not
// used (one line in the log). A conflict of the rebase (the base changed an atom of the delta too) is won by the addon
// unless an atom: rule names the atom: core - the atom of the base, an addon id - the atom of that addon
// (resolved_by "player"). An atom: rule decides an atom of the version of one addon too (core - the atom of the base).
// No delta and [addon] game_version older than the tag of the game (configs\gw_release.ltx) - one warning per addon:
// the merge (or the file of the addon itself) may revert changes of the base.
// Unresolved (15 section 5.4): the conflicts of the atoms decided by the order (no atom: rule, no patch), the ones a
// stale patch decided, the rebase conflicts no atom: rule decided, the fallbacks. The game warns (the line of the main
// menu, gw_slots.on_merge_report); with -addon_strict a level does not start while there are any (gw::CanStartLevel,
// the loads of the saves).
// Console: addon_merges [id], addon_merge_show <file>, addon_merge_rebuild, addon_atom_winner; addons_report.json:
// merges, merge_summary, rules; Lua: addon_slots.merges(), addon_slots.merge_summary().

#include "xrCore/ContentMerge/Plan.h"

struct lua_State;

namespace gw::addons
{
// The layers of the stack and the statuses of the rules are the ones of the plan of the library (xrCore
// ContentMerge/Plan.h: the engine and gw_merge plan share it), as addon_content.h names them
using ContentLayer = xray::content_merge::PlanLayer;
using ContentRuleStatus = xray::content_merge::RuleStatus;

// The id of the layer of the merged files in the content stack (FS.layer_id): it has no owner (FS.layer_owner is
// nullptr), so the owner of its files for the patches and the string table is this id (xrCore ContentPatches)
constexpr pcstr kMergedLayerId = "addons_merged";

// One file several addons replace, at the last build of the content stack: the record of the plan of the library
// (xrCore ContentMerge/Plan.h PlanFile tells the fields - the plan and the merge of a file are decided there, the same
// code as gw_merge plan); the engine adds the cache (key, cached, text_hash) and the layer (served)
using ContentMergeFile = xray::content_merge::PlanFile;

struct ContentMergeSummary
{
    u32 files = 0; // the records (all statuses)
    u32 merged = 0; // served by the layer addons_merged
    u32 fallback = 0;
    u32 unresolved = 0; // the conflicts of the atoms decided by the order or a stale patch + the fallbacks
    u32 patch_stale = 0; // the compatibility patches made for other versions of the files (merge\targets.ltx)
    u32 rebased = 0; // S9: the sides carried over by their deltas
    u32 stale_delta = 0; // S9: the sides whose deltas are not used (stale_delta)
    u32 guessed = 0; // the sides without a delta whose game_version is older than the tag of the game
    // The addons without a delta whose game_version is older than the tag of the game (one warning each; their files
    // that no other addon replaces are not read for it)
    u32 guessed_addons = 0;
    bool strict = false; // -addon_strict
};

// The release tag of the game (configs\gw_release.ltx of the base, [release] tag, written by the pipeline of the
// release - the tag of the snapshot of the base), read once; empty - unknown (no file): the game_version of the
// addons is not compared
const xr_string& GameReleaseTag();
// game_version is older than tag: both "[v]N(.N)*[-<suffix>]" (addon_merge.cpp MergeParseTag) - the numbers compared
// (the missing ones are 0), then for the same numbers a pre-release ("-Beta") < the release < "-patch1" < "-patch2" ...
// (two pre-releases: their suffixes in lower case as strings): 0.5.5-Beta < 0.5.5 < 0.5.5-patch4 < 0.5.6-Beta. A tag
// that is not such a version - any textual difference counts as older (15 section 2.2). Empty - false
bool ReleaseTagOlder(const xr_string& game_version, const xr_string& tag);

// A step of SyncContentLayers (addon_content.cpp) after the rules of the player: finds the files several addon layers
// of `desired` replace, merges them (or takes the cache), writes the folder of the set and inserts the layer
// addons_merged (ContentLayer::merged) before the location pack (or on the top). Nothing to merge - no layer. Main
// thread, the state of the content procedure (the old stack is still mounted: the base is read under it).
void PlanContentMerge(xr_vector<ContentLayer>& desired);
// The atom: rules of the player at the last plan (kind "atom", target "<file>|<atom>"): applied - the rule decided the
// atom of the merged file; else the note says why not. SyncContentLayers adds them to ContentRuleStatuses
const xr_vector<ContentRuleStatus>& ContentMergeRuleStatuses();
// The layer addons_merged of the last plan did not mount: its files are served by the upper layers (fallback)
void OnMergedLayerFailed();
// After the stack is mounted: the cleanup of the folders of the other sets and of the old cache entries
void CleanContentMergeFolders();

const xr_vector<ContentMergeFile>& ContentMerges();
const ContentMergeFile* FindContentMerge(const xr_string& file);
ContentMergeSummary ContentMergesSummary();

// The summary line in the log (LogContentConflicts)
void LogContentMerges();
// Console addon_merges [id]: the merged files, their statuses and conflicts; of one addon only with the id
void PrintContentMerges(pcstr addon_id);
// Console addon_merge_show <file>: the sides, the atoms, the conflicts, the map of the lines, the cache
void PrintContentMerge(pcstr file);
// Console addon_merge_rebuild: the cache goes, the content is built again at the next frame of the main menu
void RebuildContentMerges();

// -addon_strict (the command line, the launcher passes it): unresolved conflicts of the merge forbid the start of a
// level (15 section 5.4; 00-roadmap.md 5a, decision 47.9) - not the start of the game: in the menu the player switches
// an addon off or chooses the winner (addon_atom_winner). Without the key - the warning only
bool AddonStrictMode();
// -addon_strict and ContentMergesSummary().unresolved > 0. The callers ask it for the content set the level loads
// with: a save of another location pack - after the switch to its pack (gw::CanStartLevel), not before it
bool StrictMergeBlocks();
// The log of a refused start (`what` - what was refused): the unresolved conflicts, up to 20 lines
void LogStrictMergeBlock(pcstr what);
// Pushes the table of ContentMergesSummary onto the Lua stack: {files, merged, fallback, unresolved, patch_stale,
// rebased, stale_delta, guessed, guessed_addons, algo, game, strict, blocked} (gw_slots.on_merge_report,
// addon_slots.merge_summary); algo - the version of the parse of the engine (kAlgorithmVersion), game - the tag of
// configs\gw_release.ltx
void PushContentMergeSummary(::lua_State* L);

// The message of a Lua error (xrScriptEngine print_output through CScriptEngine::SetErrorMessageHook,
// CallLuaProtected): for each "<...>\<file>:<line>" of a merged script the layer addons_merged serves - one line
// "~ [merge] <file>:<line> comes from <addon id>" by the map of the lines of the merge (15 section 5.3). Nothing for
// the other files
void DescribeMergedLuaError(pcstr message);
} // namespace gw::addons
