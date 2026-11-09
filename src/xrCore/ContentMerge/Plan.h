#pragma once

// The plan of the merge of the addons (00-roadmap.md 5a, decision 63; reviews/2026-11-06-review.md 4.2 #2): which
// layers give each text file, which of them are the sides of the merge, what the rules of the player and the
// compatibility patches decide, which delta carries each side over, which files are merged and which are served by the
// upper layer as they are. One implementation for the engine (xrGame addon_merge.cpp PlanContentMerge, addon_content.cpp
// - the rules pair: / file:, the conflicts of the files) and for the tool gw_merge (command plan: the launcher, the
// catalog CI, the validator read its JSON instead of porting the rules).
//
// The library reads the folders of the layers itself (std::filesystem). What depends on the engine comes from the
// caller: the rule of the walk of the file system (CLocatorAPI::layer_walk_skips: hidden on Windows, ".*" on POSIX,
// ignore_name - the leaf rules are here, WalkSkipsEntry; the length of a name - WalkNameTooLong), the base file
// (FS.r_open_base - the archives too; gw_merge: a folder of the base), the patch files (xrCore ContentPatches
// IsPatchFile over the index; gw_merge: cm::IsPatchFile over the folders).

#include "ContentMerge.h"

#include <climits> // PATH_MAX (POSIX)

namespace xray::content_merge
{
// ---- the rules of the file system (xrCore LocatorAPI.cpp calls these) -------------------------------------------------

// The bytes of a path of the index of the file system: xrCore string_path (xr_types.h: 2 * max_path, where max_path is
// MAX_PATH on Windows and PATH_MAX on POSIX). LocatorAPI.cpp checks that they are the same
#if defined(_WIN32)
constexpr std::size_t kIndexPathBytes = 2 * 260;
#else
constexpr std::size_t kIndexPathBytes = 2 * PATH_MAX;
#endif
// CLocatorAPI::layer_walk: an entry whose name does not fit a path of the index ("! FS: layer '<id>': name too long
// (<kIndexPathBytes> bytes and more), skipped") - the entry alone (a folder with its subtree) is not mounted. The
// folder that holds it (`physical_dir` - its path on the disk, `virtual_dir` - its name in the index, both with the
// trailing separator; 0 - not known) and the leaf, in bytes of UTF-8; a folder adds its trailing separator
constexpr bool WalkNameTooLong(std::size_t physical_dir, std::size_t virtual_dir, std::size_t leaf, bool folder)
{
    const std::size_t name = leaf + (folder ? 1 : 0);
    return physical_dir + name >= kIndexPathBytes || virtual_dir + name >= kIndexPathBytes;
}

// ignore_name of the file system: "Thumbs.db", ".svn", ".vs" and the names ending with Thumbs.db, .VC.db, .VC.opendb,
// .sln, .pdb, .ipdb, .iobj (longer than the suffix by two chars at least)
CONTENT_MERGE_API bool IgnoredName(std::string_view name);
// CLocatorAPI::layer_walk_skips of one entry: its leaf is empty, ".", "..", IgnoredName, or the entry is hidden -
// FILE_ATTRIBUTE_HIDDEN on Windows, HiddenByName on POSIX
CONTENT_MERGE_API bool WalkSkipsEntry(std::string_view leaf, bool hidden);
// The hidden entry of POSIX: a leaf that starts with '.'
constexpr bool HiddenByName(std::string_view leaf) { return !leaf.empty() && leaf[0] == '.'; }
// PatternMatch of xrCore (LocatorAPI_defs.cpp): '*' - any run of chars ('\' too), '?' - one char; case-sensitive
CONTENT_MERGE_API bool PatternMatch(std::string_view name, std::string_view mask);
// A [private] mask as the file system keeps it (addon_content.cpp ContentMaskOf): '/' -> '\', no leading separator,
// ASCII lower case when `lower_case` (the index of Windows)
CONTENT_MERGE_API String PrivateMaskOf(std::string_view mask, bool lower_case);
// The name of the index (addon_merge.cpp MergeIndexName): '/' -> '\', ASCII lower case when `lower_case`
CONTENT_MERGE_API String IndexName(std::string_view name, bool lower_case);
CONTENT_MERGE_API String AsciiLower(std::string_view text);

// How the caller's file system walks a layer
struct WalkRules
{
    // CLocatorAPI::layer_walk_skips of an entry on the disk (`physical` - its UTF-8 path): the engine passes the one of
    // xrCore, gw_merge - WalkSkipsEntry with the attribute of the OS (Windows) or HiddenByName (POSIX)
    bool (*skips)(void* context, const char* physical){};
    void* context{};
    bool lower_case{}; // the index spells the names in ASCII lower case (Windows)
    // The bytes of the folder the layers are mounted over in the index, with its trailing separator (the engine:
    // $game_data$; gw_merge: the folder of --base): WalkNameTooLong of the names there. 0 - not known: the folder of
    // the layer alone counts
    std::size_t index_root{};
};

// The files of a layer folder (UTF-8, the trailing separator) the file system mounts (CLocatorAPI::layer_walk): not
// an entry `skips` takes with its subtree, not an entry WalkNameTooLong (its physical path goes to `too_long`), not
// under a folder "@..." (the alias roots of the addons); the folders linked in are walked, a folder that cannot be
// listed is left out (as layer_walk of a subfolder). Index names (IndexName), sorted, each once
CONTENT_MERGE_API Vector<String> LayerFiles(std::string_view folder, const WalkRules& walk,
    Vector<String>* too_long = nullptr);
// The text files (KindOfName) of LayerFiles
CONTENT_MERGE_API Vector<String> LayerTexts(std::string_view folder, const WalkRules& walk,
    Vector<String>* too_long = nullptr);
// addon_content.cpp ContentLayerGives without the owner check: `name` (an index name) lies in the folder, no [private]
// mask keeps it out, no part of it is an entry `skips` takes or WalkNameTooLong, no folder of it is "@..."
CONTENT_MERGE_API bool LayerGives(std::string_view folder, const Vector<String>& private_masks, std::string_view name,
    const WalkRules& walk);

// ---- the layers, the addons, the rules of the player (addon_content.h, addon_host.h) ---------------------------------

// A layer of the content stack, bottom up (addon_content.h; the engine names it ContentLayer)
struct PlanLayer
{
    // "addon:<id>", "slot:<slot>:<id>", "addons_merged", "level_pack:<id>", "level_pack_user:<id>"
    String layer_id;
    String owner; // the addon id; empty - a writable layer of nobody
    String folder; // UTF-8, the trailing separator
    Vector<String> private_masks; // [private] of the addon
    bool writable{}; // a plain writable layer (the writes of a pack), no owner
    String group; // the layers of one group fail together (a pack and its writes)
    Vector<String> yielded; // the files a file: rule took from it (FileRuleYields)
    bool merged{}; // the layer of the merged files (addons_merged)
};
// The layer gives `name` at the game data (addon_content.cpp ContentLayerServes): an owned layer, no file: rule took
// the name from it, LayerGives
CONTENT_MERGE_API bool LayerServes(const PlanLayer& layer, std::string_view name, const WalkRules& walk);

// An addon as the plan needs it (addon_host.h AddonInfo), in the order of the addons
struct PlanAddon
{
    String id;
    String dir; // UTF-8, the trailing separator: merge\gamedata\<file> (the fragments), merge\targets.ltx
    bool active{};
    Vector<String> load_after;
    Vector<String> patches; // [addon] patches
    String game_version; // [addon] game_version, as written
};

// A rule of appdata\addons.ltx [conflicts] at the last build of the stack (the engine names it ContentRuleStatus)
struct RuleStatus
{
    String kind; // "pair" | "file" | "atom"
    String winner; // the addon id ("core" - the base)
    String target; // pair: the loser; file: the file; atom: "<file>|<atom>"
    bool applied{};
    String note; // what it did, or why not
};

struct PairRule
{
    String winner;
    String loser;
};
struct FileRule
{
    String file; // an index name
    String winner; // an addon id or "core"
};
// atom:<file>|<atom> = <winner>
struct PlayerAtomRule
{
    String file;
    String atom; // decoded
    String winner;
};

// addon_content.cpp ContentRunOf: the part of the stack the pair rules reorder a layer in ("addon", "slot"); empty -
// the layer stays where it is (the location pack, a writable layer)
CONTENT_MERGE_API String LayerRun(const PlanLayer& layer);
// pair:<winner> = <loser> (addon_content.cpp ApplyPairRules): within each part of the stack the layers of the winner go
// right over the layers of the loser, the rest keeps its order (a stable topological order); a rule that contradicts
// the rules before it is not taken. Reorders `stack`, appends a status per rule
CONTENT_MERGE_API void ApplyPairRules(Vector<PlanLayer>& stack, const Vector<PairRule>& rules,
    Vector<RuleStatus>& statuses);
// file:<file> = <addon | core> (ApplyFileRules): the layers over the topmost layer of the addon that gives the file give
// it up (yielded); core - every layer that gives it does. Appends a status per rule
CONTENT_MERGE_API void ApplyFileRules(Vector<PlanLayer>& stack, const Vector<FileRule>& rules, const WalkRules& walk,
    Vector<RuleStatus>& statuses);

// What decided the conflict of a file the layers of several addons give (addon_content.cpp ResolveContentConflict):
// `layers` / `owners` - the layers that give it, bottom up; `winner` - the layer that serves it (one of `layers`), or
// empty - the base serves it. resolved_by: "player" (file:), "pack", "slot", "pair", "load_after", "order"; note: a rule
// about the name that does not hold
CONTENT_MERGE_API void ResolveFileConflict(std::string_view name, const Vector<String>& layers,
    const Vector<String>& owners, std::string_view winner, const Vector<FileRule>& file_rules,
    const Vector<PairRule>& pair_rules, const Vector<RuleStatus>& statuses, const Vector<PlanAddon>& addons,
    String& resolved_by, String& note);

// ---- the release tag of the game (addon_merge.h ReleaseTagOlder) ----------------------------------------------------

// game_version is older than tag: both "[v]N(.N)*[-<suffix>]" - the numbers (the missing ones are 0), then a
// pre-release < the release < -patch1 < -patch2 ...; a tag that is not such a version - any textual difference counts
// as older. Empty - false
CONTENT_MERGE_API bool ReleaseTagOlder(std::string_view game_version, std::string_view tag);
// [release] tag of configs\gw_release.ltx (its bytes): empty - none
CONTENT_MERGE_API String ReleaseTagOf(std::string_view ltx);

// ---- S9: the delta of an addon (addon_merge.cpp MergeReadDelta, MergeDeltaEntry) -------------------------------------

struct AddonDelta
{
    String file; // the name of the file read (".gw_merge\delta.json"), empty - no delta
    // Not used at all (stale_delta for every merged file of the addon): not read, or another version of the parse
    bool stale{true};
    // The JSON was read (DeltaFromJson): stale with read - another version of the parse, stale without - broken
    bool read{};
    Delta delta;
    // The entries that do not agree with the format (DeltaFileProblem): vpath (lower case, '\') and why
    Vector<String> inconsistent;
    Vector<String> inconsistent_why;
    String error; // stale: why (the log line of the caller)
};
// The bytes of the delta file `file` read: DeltaFromJson, the version of the parse, DeltaFileProblem of the entries
CONTENT_MERGE_API void ReadAddonDelta(std::string_view file, std::string_view bytes, AddonDelta& out);
// The entry of a file for the merge (a usable delta, the entry agrees with itself, not not_in_ancestor); nullptr - none
CONTENT_MERGE_API const DeltaFile* DeltaEntryOf(const AddonDelta* delta, std::string_view name);
// The delta was meant for the file but is not used: the whole delta is stale, or its entry of the file is inconsistent
CONTENT_MERGE_API bool DeltaStaleFor(const AddonDelta* delta, std::string_view name);
// The names of the delta files of an addon folder in the order they are looked for (<addon>\.gw_merge\<name>)
CONTENT_MERGE_API Vector<String> DeltaFileNames();

// ---- the plan -------------------------------------------------------------------------------------------------------

// One file several addons replace (or one addon with a delta or an atom: rule), at the last build of the content
// stack (the engine names it ContentMergeFile, addon_merge.h)
struct PlanFile
{
    String file; // relative to gamedata\, '\', the case of the index (lower case on Windows)
    String kind; // "ltx" | "xml" | "lua"
    // "merged" (by atoms), "merged_lines" (a parser did not understand the base: by lines), "single" (one addon
    // changed the base, the copies of the others are the base: its version is served), "unchanged" (no addon changed
    // the base), "fallback" (the merged text failed the check, or the layer did not mount: the upper layer serves the
    // file), "skipped" (not merged: note)
    String status;
    // skipped: "file_rule" (file: of the player), "pack" (the location pack gives it), "no_base" (the base has no such
    // file), "no_base_text" (the base has the file, its text is not available - gw_merge plan --base-list / a snapshot
    // without texts: the sides, the ancestors, the patches and the rules are planned, the merge is not; the engine
    // would merge it), "patch_file" (mod_<stem>_*), "read_error" (a file cannot be read), "one_addon" (one addon
    // replaces the file without a usable delta and without an atom: rule: its file is served as it is - without
    // PlanInput::all_files only a file whose delta does not hold gets such a record), "pack" (all_files: a file only
    // the location pack gives - its side is the layer of the pack); fallback: what failed; "check_failed" (merged,
    // single): the merged text does not pass the check, the file of the upper layer neither - the merged text is served
    // (error says why)
    String note;
    Vector<String> addons; // the sides, bottom up (the top one is the winner of the order)
    Vector<String> layers; // the layer of each side (its topmost layer that gives the file), in parallel
    // The ancestor of each side, in parallel (15 sections 2.2-2.4, 5.3): "base" (the current base: no delta,
    // [addon] game_version not older than the tag of the game or unknown), "delta:<tag>" (S9: the delta of the
    // catalog <addon>\.gw_merge\delta.json carried the side over onto the current base), "current_base_guessed" (no
    // delta, game_version older than the tag of the game: the merge may revert changes of the base),
    // "stale_delta" (the delta is not used: the file of the addon is not the published one, the delta is made by
    // another version of the parse, it is broken or does not agree with itself or with the file)
    Vector<String> ancestors;
    // Why the ancestor, in parallel (the plan, not the cache): the delta file read (".gw_merge\delta.json") and, for
    // stale_delta, why it is not used; for current_base_guessed - "game_version <v> is older than <tag>"; empty - nothing
    Vector<String> ancestor_notes;
    // S9: an atom of the delta the base changed too (15 section 2.4): the atom of the addon is served, unless an
    // atom: rule of the player names the atom (core - the atom of the base, an addon id - the atom of that side)
    struct RebaseConflict
    {
        String atom;
        String addon;
        String reason; // "both_changed" | "base_deleted" | "both_added" | "rebase_failed"
        String note; // rebase_failed: why
        String winner; // the addon, "core" (the base) or the winner of the atom: rule
        // "rebase" - the addon won by the rule of the rebase (unresolved, 15 section 5.4); "player" - an atom: rule
        // decided the atom (resolved)
        String resolved_by = "rebase";
    };
    Vector<RebaseConflict> rebase_conflicts;
    Vector<String> changed; // the sides whose text differs from the base
    bool cached{}; // taken from the cache of an earlier build
    bool served{}; // the file of the layer addons_merged serves the name
    std::uint32_t atoms_merged = 0;
    std::uint32_t adjacent = 0;
    Vector<String> adjacent_atoms;
    Vector<String> reordered;
    struct Conflict
    {
        String atom; // "ranks.parse_ranks", "[settings] key", "string_table/string[id=x]/text#text"
        Vector<String> addons; // the sides that changed it differently, bottom up
        String winner;
        // "order", "load_after", "pair", "slot" - the order of the stack, as the conflicts of the files; "player" -
        // the atom: rule of the player; "patch" - a compatibility patch (winner - its id)
        String resolved_by;
        Vector<String> lost; // the sides whose change of the atom is not in the result
        std::uint32_t base_line = 0; // the line of the atom in the base, 0 - a new atom
        bool stale{}; // decided by a stale compatibility patch (patch_stale): counts as unresolved
    };
    Vector<Conflict> conflicts;
    // The compatibility patches whose targets ([addon] patches) replace the file (15 section 6.1)
    struct Patch
    {
        String id;
        Vector<String> targets; // its [addon] patches
        // "applied" - its fragment merge\gamedata\<file> is a side of the merge; "stale" - applied, but
        // merge\targets.ltx names other versions of the files of `stale_targets` (patch_stale); "skipped" - note says
        // why ("not_a_side: <id> ...", "read_error: ..."); a patch with no fragment of the file has no record
        String status;
        String note;
        Vector<String> stale_targets;
        std::uint32_t resolved = 0; // the conflicts it decided
    };
    Vector<Patch> patches;
    Vector<String> rules_applied; // the atoms the atom: rules of the player decided
    // An atom: rule = core took the atom of the base into the version of the only addon that changes the file (no
    // merge by atoms there): the text is not the file of the addon, the layer addons_merged serves it
    bool rules_changed{};
    // The conflicts decided by the order of the stack only, those a stale patch decided, the rebase conflicts no
    // atom: rule decided (15 section 5.4)
    std::uint32_t unresolved = 0;
    String error; // fallback: why
    struct Lines
    {
        std::uint32_t from = 0, to = 0; // 1-based, inclusive, the lines of the merged text
        String owner; // a side or "core" (the base)
    };
    Vector<Lines> lines; // the map of the lines of the merged text (merged, merged_lines, single)
    String key; // the cache key, sha1 hex
    // The merged text the layer serves: "<size> <sha1 hex>" (the meta of the cache keeps it; a hit of the cache and a
    // ready set check the file by it). Empty - the layer does not serve the file
    String text_hash;
};

// The fragment of a compatibility patch: a side of the merge over the addons (Role::Patch, 15 section 6.1)
struct PlanPatchSide
{
    String id;
    Vector<String> targets;
    String text; // <patch>\merge\gamedata\<file>
};

// The inputs of the merge of one file of the plan
struct PlanJob
{
    String base; // the bytes of the base file
    Vector<String> texts; // the bytes of the file of each side
    Vector<const DeltaFile*> deltas; // the entry each side is carried over by (nullptr - none), in the AddonDelta given
    Vector<PlanPatchSide> patches;
    Vector<AtomRule> rules; // the atom: rules whose winner takes part (a side, an applied patch, core)
    Vector<std::size_t> side_layers; // the index of the layer of each side in PlanInput::layers
};

struct PlanEntry
{
    PlanFile item;
    bool merge{}; // the job is to be merged (MergePlanned); else item.status is "skipped"
    bool alone{}; // one addon replaces the file: it takes part for its delta or an atom: rule
    // Listed for PlanInput::all_files only (the plan without it has no record of the file): status "skipped", the
    // note of the file as a record of the merge would have it (file_rule, pack, patch_file, no_base, read_error), else
    // one_addon; one side, no ancestors, patches or rules
    bool extra{};
    PlanJob job;
    Vector<String> log; // the lines of the log about this file before its merge ("~ [merge] ...")
};

// What the plan asks the caller
struct PlanEnv
{
    void* context{};
    // The base file (the engine: FS.r_open_base("$game_data$", name)): false - the base has none (or, with
    // base_exists, has no text of it)
    bool (*base)(void* context, std::string_view name, String& out){};
    // Optional: the base has the file although `base` gives no text of it (gw_merge plan --base-list: the archives of a
    // player; a snapshot without its texts) - note no_base_text instead of no_base. nullptr - `base` decides alone
    bool (*base_exists)(void* context, std::string_view name){};
    // A patch file (mod_<stem>_*): the engine - IsPatchFile of ContentPatches over the index, gw_merge - cm::IsPatchFile
    bool (*is_patch_file)(void* context, std::string_view name){};
    WalkRules walk;
};

struct PlanInput
{
    Vector<PlanLayer> layers; // the stack bottom up after the rules (ApplyPairRules, ApplyFileRules), no merged layer
    Vector<PlanAddon> addons; // in the order of the addons
    Vector<const AddonDelta*> deltas; // in parallel with addons: the delta read for the addon (nullptr - none)
    Vector<FileRule> file_rules;
    Vector<PlayerAtomRule> atom_rules; // the first rule of a file and an atom counts
    String game_tag; // configs\gw_release.ltx, empty - unknown
    // gw_merge plan --all-files: a record of every text file a layer of an addon serves (PlanEntry::extra for the
    // files the plan has no record of). The engine leaves it off
    bool all_files{};
};

struct PlanResult
{
    // The addons whose layers take part, in the order of the stack
    Vector<String> owners;
    // The addons without a usable delta whose game_version is older than the tag of the game (one warning each)
    Vector<String> guessed_addons;
    Vector<PlanEntry> entries; // by the file name
};

// The owners of the layers that take part in the plan (the public content of the addons and the sets of the providers:
// owned, not writable, not merged, not of a group - the pack), each once, in the order of the stack. The caller reads
// their deltas (PlanInput::deltas)
CONTENT_MERGE_API Vector<String> PlanOwners(const Vector<PlanLayer>& layers);

// The plan (PlanContentMergeFiles of the engine without the cache and the layer): the files the layers of the addons
// give - the public content and the sets of the providers, not the pack and not a writable layer - that two addons give,
// or one addon whose delta has the file or an atom: rule names it. For each: the sides (the topmost layer of each addon
// that serves it), the reason it is not merged, the ancestors (deltas), the compatibility patches, the atom: rules.
// PlanInput::all_files: every other text file a layer of an addon (or the location pack) serves too, PlanEntry::extra
CONTENT_MERGE_API void PlanMerge(const PlanInput& input, const PlanEnv& env, PlanResult& out);

// The merge of one planned file (the engine: MergeOne without the cache): the sides carried over by their deltas under
// the atom: rules, the fragments of the patches over them, the rules; the merged text checked (Validate with `lua`): a
// text that fails while the file of the upper side passes - fallback ("compile" / "parse"), else check_failed. `text` -
// the merged text (the layer serves it when Serves). `log` - the lines of the log ("~ [merge] ...")
CONTENT_MERGE_API void MergePlanned(PlanFile& item, const PlanJob& job, const LuaChecker* lua, String& text,
    Vector<String>& log);
// The layer of the merged files serves the result (addon_merge.cpp MergeServes)
CONTENT_MERGE_API bool Serves(const PlanFile& item);
// The side of the addon was carried over by its delta
CONTENT_MERGE_API bool Rebased(const PlanFile& item, std::string_view addon);
// "order" of the library told the way the conflicts of the files are: "slot", "pair" (an applied pair: rule of
// `statuses`), "load_after", "order"
CONTENT_MERGE_API void RefineResolved(PlanFile& item, const Vector<PlanAddon>& addons, const Vector<RuleStatus>& statuses);
// What each patch decided; the conflicts a stale patch decided are unresolved
CONTENT_MERGE_API void CountPatches(PlanFile& item);
// "@gap X" and "X" are one atom for the rules
CONTENT_MERGE_API bool SameAtom(std::string_view a, std::string_view b);
// The statuses of the atom: rules (kind "atom") against the records of the plan
CONTENT_MERGE_API Vector<RuleStatus> AtomRuleStatuses(const Vector<PlayerAtomRule>& rules, const Vector<PlanFile>& records);
} // namespace xray::content_merge
