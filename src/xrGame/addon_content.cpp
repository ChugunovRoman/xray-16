#include "StdAfx.h"

#include "addon_content.h"
#include "addon_host.h"
#include "addon_merge.h"
#include "addon_slots.h"

#include "xrCore/ContentMerge/ContentMerge.h"
#include "xrCore/ContentMerge/Plan.h"
#include "xrCore/ContentPatches.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#if !defined(XR_PLATFORM_WINDOWS)
#include <unistd.h> // getpid
#endif

namespace gw::addons
{
namespace
{
// The folders of gamedata\ the models take by name: mounted once more under "<path alias>@<id>\"
struct ContentAliasRoot
{
    pcstr folder; // in <addon>\gamedata\ (the disk)
    pcstr path; // the path alias of fsgame.ltx its names lie under
};
constexpr ContentAliasRoot kContentAliasRoots[] = {
    { "textures", "$game_textures$" },
    { "meshes", "$game_meshes$" },
};

bool ContentAddonIdValid(const xr_string& id)
{
    // The rule of addon_host (IsValidId) and of FS.mount_layer (layer_owner_valid): a-z 0-9 _ only - "@<id>\" stays
    // one folder name, and the file system takes the id as the owner of the layer
    return !id.empty() && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}
} // namespace

bool MountContentLayer(const AddonInfo& addon, pcstr layer_id)
{
    string_path gamedata;
    strconcat(sizeof gamedata, gamedata, addon.dir.c_str(), "gamedata", DELIMITER);
    return MountContentFolder(addon, gamedata, layer_id);
}

bool MountContentFolder(const AddonInfo& addon, pcstr gamedata, pcstr layer_id, const xr_vector<xr_string>* yielded)
{
    if (!ContentAddonIdValid(addon.id))
    {
        Msg("! [addon:%s] the id is not a-z 0-9 _ (the owner of a layer, an alias root), the content is not mounted",
            addon.id.c_str());
        return false;
    }

    // The files given up by the rules of the player are private files of the layer: one mask per file
    const u32 given_up = yielded ? u32(yielded->size()) : 0;
    xr_vector<xr_string> masks;
    if (given_up)
    {
        masks = addon.private_masks;
        masks.insert(masks.end(), yielded->begin(), yielded->end());
    }
    CLocatorAPI::layer_options options;
    options.owner = addon.id.c_str();
    options.private_masks = given_up ? &masks : &addon.private_masks;
    const u16 index = FS.mount_layer(layer_id, gamedata, nullptr, false, &options);
    if (!index)
        return false;

    for (const ContentAliasRoot& root : kContentAliasRoots)
    {
        if (!FS.path_exist(root.path))
        {
            Msg("! [addon:%s] no %s in fsgame.ltx: no alias root for %s\\", addon.id.c_str(), root.path, root.folder);
            continue;
        }
        string_path physical, alias, virtual_root;
        strconcat(sizeof physical, physical, gamedata, root.folder, DELIMITER);
        strconcat(sizeof alias, alias, "@", addon.id.c_str(), DELIMITER);
        FS.update_path(virtual_root, root.path, alias);
        FS.mount_layer_alias(layer_id, physical, virtual_root);
    }

    CLocatorAPI::layer_stats stats;
    if (FS.get_layer_stats(index, stats))
    {
        // The files it covers: of the base, of the layers mounted before it (other addons - the conflicts, a set of
        // a provider over its own common content). The public ones only, as layer_stats::covered_files
        xr_vector<CLocatorAPI::layer_covered_entry> covered;
        FS.get_layer_covered(index, covered);
        const u32 base_files = u32(std::count_if(covered.begin(), covered.end(),
            [](const CLocatorAPI::layer_covered_entry& entry) { return entry.under == 0; }));
        // A patch of one name in another addon below is applied too (CollectContentConflicts): it is not covered
        const u32 layer_files = u32(std::count_if(covered.begin(), covered.end(),
            [&addon](const CLocatorAPI::layer_covered_entry& entry) {
                if (entry.under == 0)
                    return false;
                const pcstr under_owner = FS.layer_owner(entry.under);
                return !under_owner || addon.id == under_owner ||
                    !xray::content_patches::IsPatchFile(entry.name.c_str());
            }));
        if (base_files)
            Msg("~ [addon:%s] replaces %u base files (console: addon_files %s)", addon.id.c_str(), base_files,
                addon.id.c_str());
        if (layer_files)
            Msg("~ [addon:%s] %s: covers %u file(s) of the content mounted before it (console: addon_conflicts %s)",
                addon.id.c_str(), layer_id, layer_files, addon.id.c_str());
        // SyncContentLayers gives up only the files the folder has: each of them is one of the private files
        if (given_up)
            Msg("* [addon:%s] %s: gives %u file(s) up to other addons by appdata/addons.ltx [conflicts] (console "
                "addon_conflicts %s)", addon.id.c_str(), layer_id, given_up, addon.id.c_str());
        const u32 private_files = stats.private_files > given_up ? stats.private_files - given_up : 0;
        if (!addon.private_masks.empty())
        {
            if (private_files)
                Msg("* [addon:%s] %u private files (only under @%s\\)", addon.id.c_str(), private_files,
                    addon.id.c_str());
            else
                Msg("~ [addon:%s] [private]: no file matches the masks", addon.id.c_str());
        }
    }
    return true;
}

bool UnmountContentLayer(pcstr layer_id) { return FS.unmount_layer(layer_id); }

namespace
{
xr_vector<ContentLayer> g_content_mounted; // bottom up, as FS has them
xr_vector<ContentConflict> g_content_conflicts;
xr_vector<ContentRuleStatus> g_content_rules; // the rules of [conflicts] at the last build of the stack
// addons_report.json content_built: the rules of that build as of this time (ConflictRulesReadAt); 0 - no build yet
u64 g_content_built = 0;
constexpr u32 kContentConflictLogLines = 20; // the summary in the log; addon_conflicts prints all
constexpr pcstr kContentReportName = "addons_report.json";
constexpr u32 kContentReportFormat = 1;

bool SameContentLayer(const ContentLayer& a, const ContentLayer& b)
{
    return a.layer_id == b.layer_id && a.owner == b.owner && a.folder == b.folder && a.writable == b.writable &&
        a.private_masks == b.private_masks && a.group == b.group && a.yielded == b.yielded && a.merged == b.merged;
}

xr_string ContentGroupOf(const ContentLayer& layer) { return layer.group.empty() ? layer.layer_id : layer.group; }

bool MountContentLayerOf(const ContentLayer& layer)
{
    if (layer.writable)
        return FS.mount_layer(layer.layer_id.c_str(), layer.folder.c_str(), nullptr, true) != 0;
    // The merged files (addon_merge.h): a plain layer - no owner (its files belong to several addons), no alias roots
    if (layer.merged)
        return FS.mount_layer(layer.layer_id.c_str(), layer.folder.c_str(), nullptr, false, nullptr) != 0;
    AddonInfo addon;
    addon.id = layer.owner;
    addon.private_masks = layer.private_masks;
    return MountContentFolder(addon, layer.folder.c_str(), layer.layer_id.c_str(), &layer.yielded);
}

// The index names are the full paths in lower case: the part after the game data folder, for the messages
xr_string ContentRelativeName(const xr_string& name)
{
    string_path root;
    FS.update_path(root, "$game_data$", "");
    const size_t length = xr_strlen(root);
    const bool under = name.size() > length && std::equal(name.begin(), name.begin() + length, root,
        [](char a, char b) { return std::tolower(u8(a)) == std::tolower(u8(b)); });
    return under ? name.substr(length) : name;
}

namespace cm = xray::content_merge;

// The walk of the file system for the rules of the library (xrCore ContentMerge/Plan.h): CLocatorAPI::layer_walk_skips
// (hidden on Windows, ".*" on POSIX, ignore_name), the names of the index in lower case on Windows, the layers over
// $game_data$ (a name too long there is not mounted: cm::WalkNameTooLong)
bool ContentWalkSkips(void* /*context*/, const char* physical) { return CLocatorAPI::layer_walk_skips(physical); }

cm::WalkRules ContentWalk()
{
#if defined(XR_PLATFORM_WINDOWS)
    cm::WalkRules walk{ &ContentWalkSkips, nullptr, true };
#else
    cm::WalkRules walk{ &ContentWalkSkips, nullptr, false };
#endif
    string_path game_data;
    FS.update_path(game_data, "$game_data$", "");
    walk.index_root = xr_strlen(game_data);
    if (walk.index_root && game_data[walk.index_root - 1] != _DELIMITER)
        ++walk.index_root; // the trailing delimiter of the root of a layer
    return walk;
}

// The rules of the player (pair:, file:) in the form of the library
cm::Vector<cm::PairRule> ContentPairRules()
{
    cm::Vector<cm::PairRule> out;
    for (const PairRule& rule : PairRules())
        out.push_back({ rule.winner, rule.loser });
    return out;
}

cm::Vector<cm::FileRule> ContentFileRules()
{
    cm::Vector<cm::FileRule> out;
    for (const FileRule& rule : FileRules())
        out.push_back({ rule.file, rule.winner });
    return out;
}

// pair:<winner> = <loser>: within each part of the stack (cm::LayerRun) the layers of the winner go right over the
// layers of the loser; the rest keeps the order of the addons (a stable topological order). A rule that contradicts
// the rules before it (a cycle) is not taken. The rule of the library (cm::ApplyPairRules), the one of gw_merge plan
void ApplyPairRules(xr_vector<ContentLayer>& stack) { cm::ApplyPairRules(stack, ContentPairRules(), g_content_rules); }

// file:<file> = <addon | core>: the layers over the topmost layer of the addon that gives the file give it up
// (ContentLayer::yielded); core - every layer that gives it does (cm::ApplyFileRules)
void ApplyFileRules(xr_vector<ContentLayer>& stack)
{
    cm::ApplyFileRules(stack, ContentFileRules(), ContentWalk(), g_content_rules);
}

// 13-extension-model.md, 4.2: a public name of a layer of an addon that covers the entry of a layer of another addon,
// and a file a rule took from a layer (it is the private file of that layer now: the file system does not see it as
// covering anything)
void CollectContentConflicts()
{
    g_content_conflicts.clear();
    struct Found
    {
        xr_vector<u16> layers;
        u16 serving = 0;
        bool missing = false; // no layer and no base file serves the name (file:<file> = core, the base has none)
    };
    xr_map<xr_string, Found> found; // by the name relative to gamedata\ (lower case); stable order: by name
    xr_vector<CLocatorAPI::layer_covered_entry> covered;
    const u16 count = FS.layers_count();
    for (u16 index = 1; index <= count; ++index)
    {
        const pcstr owner = FS.layer_owner(index);
        if (!owner)
            continue; // the writes of a pack: not an addon of its own
        FS.get_layer_covered(index, covered);
        for (const CLocatorAPI::layer_covered_entry& entry : covered)
        {
            const pcstr under_owner = entry.under ? FS.layer_owner(entry.under) : nullptr;
            // The base, the writes of a pack, the same addon (its common content under the set of its provider)
            if (!under_owner || 0 == xr_strcmp(under_owner, owner))
                continue;
            // A patch (mod_<stem>_*.ltx / .xml next to its target) of one name in several addons: each one is applied
            // (xrCore ContentPatches.h), no file wins - the conflicts of what they change are the atoms
            // (patch_conflicts of the report, dltx_conflicts)
            if (xray::content_patches::IsPatchFile(entry.name.c_str()))
                continue;
            Found& item = found[ContentRelativeName(entry.name)];
            // The layer that serves the name too: it may cover only a layer of its own addon (the set of a provider
            // over its common content) - without it the winner of the conflict would be none of its layers
            for (const u16 layer : { entry.under, index, entry.serving })
            {
                if (layer && FS.layer_owner(layer) &&
                    std::find(item.layers.begin(), item.layers.end(), layer) == item.layers.end())
                    item.layers.push_back(layer);
            }
            item.serving = entry.serving;
        }
    }
    for (const ContentLayer& layer : g_content_mounted)
    {
        const u16 index = layer.yielded.empty() ? 0 : FS.find_layer(layer.layer_id.c_str());
        if (!index)
            continue;
        for (const xr_string& name : layer.yielded)
        {
            Found& item = found[name];
            if (item.layers.empty())
            {
                // The layer that serves the name now (another addon, or the base: 0)
                string_path full_name;
                const CLocatorAPI::file* desc =
                    FS.exist(full_name, "$game_data$", name.c_str()) ? FS.GetFileDesc(full_name) : nullptr;
                item.serving = desc ? desc->layer : 0;
                item.missing = !desc;
                if (item.serving && FS.layer_owner(item.serving))
                    item.layers.push_back(item.serving);
            }
            if (std::find(item.layers.begin(), item.layers.end(), index) == item.layers.end())
                item.layers.push_back(index);
        }
    }

    // load_after of the winners, the rules of the player: in the form of the library once
    cm::Vector<cm::PlanAddon> plan_addons;
    for (const AddonInfo& info : ListAddons())
        plan_addons.push_back({ info.id, info.dir, info.active, info.load_after, info.patches, info.game_version });
    const cm::Vector<cm::PairRule> pair_rules = ContentPairRules();
    const cm::Vector<cm::FileRule> file_rules = ContentFileRules();
    for (auto& [name, item] : found)
    {
        std::sort(item.layers.begin(), item.layers.end());
        ContentConflict& conflict = g_content_conflicts.emplace_back();
        conflict.name = name;
        for (const u16 layer : item.layers)
        {
            conflict.layers.emplace_back(FS.layer_id(layer));
            conflict.owners.emplace_back(FS.layer_owner(layer));
        }
        const pcstr winner = item.serving ? FS.layer_id(item.serving) : nullptr;
        conflict.winner = item.missing ? "none" : winner ? winner : "base";
        const ContentMergeFile* merge = FindContentMerge(name);
        if (winner && 0 == xr_strcmp(winner, kMergedLayerId))
        {
            // The merged file of the addons serves it (addon_merge.h): no layer of them wins
            conflict.resolved_by = "merge";
            if (merge && merge->unresolved)
            {
                conflict.note = std::to_string(merge->unresolved).c_str() +
                    xr_string(" conflict(s) of atoms decided by the order (console addon_merge_show ") + name + ")";
            }
            continue;
        }
        // What decided it (cm::ResolveFileConflict, the rule of gw_merge plan): the rules of the player first, then
        // the parts of the stack (the set of a provider over the common content, the pack over all), then - within
        // one part - the pair rules, load_after of the winner and the order of the addons
        cm::ResolveFileConflict(conflict.name, conflict.layers, conflict.owners, winner ? winner : "", file_rules,
            pair_rules, g_content_rules, plan_addons, conflict.resolved_by, conflict.note);
        if (merge && merge->status == "fallback")
        {
            conflict.note += (conflict.note.empty() ? "" : "; ") + xr_string("not merged: ") +
                (merge->error.empty() ? merge->note : merge->error);
        }
        if (item.missing)
            conflict.note += (conflict.note.empty() ? "" : "; ") + xr_string("the base has no such file: it is not in "
                "the game now");
    }
}

void PrintContentConflict(pcstr prefix, const ContentConflict& conflict)
{
    xr_string owners; // each addon once: its common content and its set may both give the name
    for (size_t i = 0; i < conflict.owners.size(); ++i)
    {
        const auto first = std::find(conflict.owners.begin(), conflict.owners.end(), conflict.owners[i]);
        if (size_t(first - conflict.owners.begin()) == i)
            owners += (owners.empty() ? "" : ", ") + conflict.owners[i];
    }
    Msg("%sconflict file %s addons: %s winner: %s (%s)%s%s", prefix, conflict.name.c_str(), owners.c_str(),
        conflict.winner.c_str(), conflict.resolved_by.c_str(), conflict.note.empty() ? "" : "; ",
        conflict.note.c_str());
}

void PrintRuleStatus(pcstr prefix, const ContentRuleStatus& status)
{
    Msg("%srule %s:%s = %s: %s%s%s", prefix, status.kind.c_str(),
        status.kind == "pair" ? status.winner.c_str() : status.target.c_str(),
        status.kind == "pair" ? status.target.c_str() : status.winner.c_str(), status.applied ? "applied" : "not applied",
        status.note.empty() ? "" : ", ", status.note.c_str());
}
// The merged files of the plan are served only when their layer is mounted: else the upper layers serve them. Then the
// atom: rules of the player join the statuses of the rules (whether the merged file that took the atom is served)
void FinishContentMerge(const xr_vector<ContentLayer>& desired)
{
    const auto planned = std::find_if(desired.begin(), desired.end(), [](const ContentLayer& l) { return l.merged; });
    if (planned != desired.end() && std::none_of(g_content_mounted.begin(), g_content_mounted.end(),
            [&planned](const ContentLayer& l) { return l.merged && l.folder == planned->folder; }))
        OnMergedLayerFailed();
    const xr_vector<ContentRuleStatus>& atoms = ContentMergeRuleStatuses();
    g_content_rules.insert(g_content_rules.end(), atoms.begin(), atoms.end());
}
} // namespace

bool ContentLayerServes(const ContentLayer& layer, const xr_string& name)
{
    return cm::LayerServes(layer, name, ContentWalk());
}

namespace
{
// appdata\logs\addons_base_files.txt (decision 63): the files of the base of the game for the plan of the launcher
// (gw_merge plan --base-list <file>) - a player's base lies in database\*.db, the tool cannot read the archives.
// The format (UTF-8, LF, no BOM):
//   # gw_base_files 1                  the first line: the format
//   # game_tag <tag>                   [release] tag of configs\gw_release.ltx (no line - unknown)
//   <vpath>                            one file per line, sorted: relative to gamedata\, '\', ASCII lower case on
//                                      Windows (the names of the index: cm::IndexName, as gw_merge plan spells them)
// A line that starts with '#' is a comment (a vpath that starts with '#' is not listed). The files of layer 0 only -
// the loose files of the folders of fsgame.ltx and the files of the archives, as r_open_base finds them - the ones
// the layers of the addons cover now included; no folders. Written when the stack is built, only when the text changed,
// through a temporary file (the launcher never reads half a list)
constexpr pcstr kContentBaseFilesName = "addons_base_files.txt";
// The text written last in this run - its size and SHA-256, not the text (the list of the base is megabytes): no read
// of the file when nothing changed. sha256 empty: nothing written yet
struct ContentBaseFilesStamp
{
    size_t size = 0;
    xr_string sha256;
};
ContentBaseFilesStamp g_content_base_files;

void WriteContentBaseFiles()
{
    xr_vector<xr_string> names;
    FS.get_base_files("$game_data$", names);
    if (names.empty())
        return; // no $game_data$: nothing to tell (and an empty list would be a base without files)
#if defined(XR_PLATFORM_WINDOWS)
    constexpr bool lower_case = true; // the index spells the names in lower case (xr_fs_strlwr)
#else
    constexpr bool lower_case = false;
#endif
    xr_string text = "# gw_base_files 1\n";
    const xr_string& tag = GameReleaseTag();
    if (!tag.empty() && tag.find_first_of("\r\n") == xr_string::npos)
        text += "# game_tag " + tag + "\n";
    for (const xr_string& name : names)
    {
        const xr_string vpath = cm::IndexName(name, lower_case);
        if (vpath.empty() || vpath[0] == '#' || vpath.find_first_of("\r\n\t") != xr_string::npos)
            continue; // not a line of the list
        text += vpath;
        text += '\n';
    }
    const xr_string sha256 = cm::Sha256Hex(text);
    if (!g_content_base_files.sha256.empty() && text.size() == g_content_base_files.size &&
        sha256 == g_content_base_files.sha256)
    {
        return;
    }
    const auto remember = [&text, &sha256]() {
        g_content_base_files.size = text.size();
        g_content_base_files.sha256 = sha256;
    };

    string_path file_name;
    FS.update_path(file_name, FS.path_exist("$logs$") ? "$logs$" : "$app_data_root$", kContentBaseFilesName);
    const std::filesystem::path file = PathFromUtf8(file_name);
    {
        std::ifstream in(file, std::ios::binary);
        if (in)
        {
            xr_string old((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (old == text)
            {
                remember();
                return; // the same base as the run before
            }
        }
    }
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    // "<list>.<pid>.tmp": two instances of the game sharing the appdata never write one temporary file
#if defined(XR_PLATFORM_WINDOWS)
    const unsigned long pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    std::filesystem::path temporary = file;
    temporary += ("." + std::to_string(pid) + ".tmp").c_str();
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close(); // a full disk shows only here: a cut list is not renamed
        if (out.fail())
        {
            Msg("! [addons] cannot write [%s]: the list of the files of the base is not updated", file_name);
            std::filesystem::remove(temporary, error);
            return;
        }
    }
    std::filesystem::rename(temporary, file, error);
    if (error)
    {
        Msg("! [addons] cannot replace [%s] (%s): the list of the files of the base is not updated", file_name,
            ErrorText(error).c_str());
        std::filesystem::remove(temporary, error);
        return;
    }
    remember();
    Msg("* [addons] the files of the base: %u [%s]", u32(names.size()), file_name);
}
} // namespace

void SyncContentLayers(const xr_vector<ContentLayer>& desired_order, xr_vector<xr_string>& failed_groups)
{
    // The rules of the player over the order of the addons (addon_content.h): the stack as it is to be mounted
    g_content_rules.clear();
    g_content_built = ConflictRulesReadAt();
    xr_vector<ContentLayer> desired;
    desired.reserve(desired_order.size() + 1);
    for (const ContentLayer& layer : desired_order)
    {
        if (layer.merged)
            continue; // planned again below
        desired.push_back(layer);
        desired.back().yielded.clear();
    }
    ApplyPairRules(desired);
    ApplyFileRules(desired);
    // The files several addons replace (addon_merge.h): merged into the layer addons_merged under the location pack,
    // after the rules (the order of the sides, the files they give)
    PlanContentMerge(desired);
    // The base for the plan of the launcher (gw_merge plan --base-list): its tag is read by the plan above
    WriteContentBaseFiles();

    size_t keep = 0;
    while (keep < g_content_mounted.size() && keep < desired.size() && SameContentLayer(g_content_mounted[keep], desired[keep]))
        ++keep;

    // The layers above the common bottom go, the top one first (the file system unmounts the top layer only)
    while (g_content_mounted.size() > keep)
    {
        const ContentLayer& top = g_content_mounted.back();
        if (FS.find_layer(top.layer_id.c_str()) && !UnmountContentLayer(top.layer_id.c_str()))
        {
            // Someone else mounted a layer over it: the stack cannot be rebuilt, nothing new goes on it
            Msg("! [addons] the content layer '%s' cannot be unmounted, the content stays as it is",
                top.layer_id.c_str());
            for (size_t i = keep; i < desired.size(); ++i)
                failed_groups.push_back(ContentGroupOf(desired[i]));
            FinishContentMerge(desired);
            CollectContentConflicts();
            return;
        }
        g_content_mounted.pop_back();
    }

    for (size_t i = keep; i < desired.size(); ++i)
    {
        const ContentLayer& layer = desired[i];
        const xr_string group = ContentGroupOf(layer);
        if (std::find(failed_groups.begin(), failed_groups.end(), group) != failed_groups.end())
            continue; // a layer of its group did not mount
        if (MountContentLayerOf(layer))
        {
            g_content_mounted.push_back(layer);
            continue;
        }
        // The mounted layers of its group are the top ones: they go too
        while (!layer.group.empty() && !g_content_mounted.empty() && g_content_mounted.back().group == layer.group)
        {
            UnmountContentLayer(g_content_mounted.back().layer_id.c_str());
            g_content_mounted.pop_back();
        }
        failed_groups.push_back(group);
        Msg("! [addons] the content layer '%s' [%s] is not mounted%s%s", layer.layer_id.c_str(), layer.folder.c_str(),
            layer.group.empty() ? "" : ", nor the rest of ", layer.group.c_str());
    }
    FinishContentMerge(desired);
    CollectContentConflicts();
    CleanContentMergeFolders(); // the folders of the other sets are not mounted now
}

const xr_vector<ContentLayer>& MountedContentLayers() { return g_content_mounted; }

const xr_vector<ContentConflict>& ContentConflicts() { return g_content_conflicts; }

const xr_vector<ContentRuleStatus>& ContentRuleStatuses() { return g_content_rules; }

void LogContentConflicts()
{
    xr_string stack;
    for (const ContentLayer& layer : g_content_mounted)
        stack += (stack.empty() ? "" : ", ") + layer.layer_id;
    Msg("* [addons] content: %u layer(s)%s%s", u32(g_content_mounted.size()), stack.empty() ? "" : ": ", stack.c_str());
    for (const ContentRuleStatus& status : g_content_rules)
        PrintRuleStatus(status.applied ? "* [addons] " : "~ [addons] ", status);
    LogContentMerges();
    if (g_content_conflicts.empty())
        return;
    Msg("~ [addons] %u file(s) come from more than one addon: the one lower in the order of the addons wins, the set "
        "of a chosen slot provider wins over the common content; the rules of appdata/addons.ltx [conflicts] go first "
        "(console addon_conflicts)", u32(g_content_conflicts.size()));
    for (size_t i = 0; i < g_content_conflicts.size() && i < kContentConflictLogLines; ++i)
        PrintContentConflict("~   ", g_content_conflicts[i]);
    if (g_content_conflicts.size() > kContentConflictLogLines)
        Msg("~   ... %u more (console addon_conflicts)", u32(g_content_conflicts.size() - kContentConflictLogLines));
}

void PrintContentConflicts(pcstr addon_id)
{
    const bool all = !addon_id || !addon_id[0];
    Msg("- [addons] content stack, bottom up (%u layer(s)):", u32(g_content_mounted.size()));
    for (const ContentLayer& layer : g_content_mounted)
    {
        Msg("-   %s [%s]%s", layer.layer_id.c_str(), layer.folder.c_str(),
            layer.yielded.empty() ? "" : " (gives files up by [conflicts])");
    }
    u32 rules = 0;
    for (const ContentRuleStatus& status : g_content_rules)
    {
        if (!all && status.winner != addon_id && !(status.kind == "pair" && status.target == addon_id))
            continue;
        PrintRuleStatus("-   ", status);
        ++rules;
    }
    u32 shown = 0;
    for (const ContentConflict& conflict : g_content_conflicts)
    {
        if (!all && std::find(conflict.owners.begin(), conflict.owners.end(), addon_id) == conflict.owners.end())
            continue;
        PrintContentConflict("-   ", conflict);
        ++shown;
    }
    Msg("- [addons] %u file conflict(s), %u rule(s) of appdata/addons.ltx [conflicts]%s%s (what decided a conflict: "
        "order, load_after, pair, player, slot, pack, merge; the choice of the player: addon_pair <winner id> <loser "
        "id>, addon_file_winner <file> <addon id | core>; the merged files: addon_merges)", shown, rules,
        all ? "" : " of the addon ", all ? "" : addon_id);
}

namespace
{
// The JSON of the report: UTF-8, two spaces of indentation
struct ContentReportJson
{
    xr_string out;
    xr_vector<bool> empty; // per open container: nothing in it yet

    void quote(const xr_string& text)
    {
        out += '"';
        for (size_t i = 0; i < text.size(); ++i)
        {
            const u8 c = u8(text[i]);
            // A sequence of UTF-8 as is; a byte outside one (a narrow path of the log) - '?'
            // Strict (RFC 3629): no overlong form, no surrogate, nothing over U+10FFFF - a reader of the launcher
            // (a UTF-8 string type) rejects the whole file for one such sequence
            u32 length = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c & 0xF0) == 0xE0 ? 3 :
                (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
            for (u32 k = 1; k < length; ++k)
            {
                if (i + k >= text.size() || (u8(text[i + k]) & 0xC0) != 0x80)
                    length = 0;
            }
            if (length > 2)
            {
                const u8 next = u8(text[i + 1]);
                if ((c == 0xE0 && next < 0xA0) || (c == 0xED && next > 0x9F) || (c == 0xF0 && next < 0x90) ||
                    (c == 0xF4 && next > 0x8F))
                    length = 0;
            }
            if (length > 1)
            {
                out.append(text, i, length);
                i += length - 1;
                continue;
            }
            if (!length)
                out += '?';
            else if (c == '"' || c == '\\')
            {
                out += '\\';
                out += char(c);
            }
            else if (c < 0x20)
            {
                string16 escaped;
                xr_sprintf(escaped, "\\u%04x", u32(c));
                out += escaped;
            }
            else
                out += char(c);
        }
        out += '"';
    }
    void item(pcstr key)
    {
        if (!empty.empty())
        {
            out += empty.back() ? "" : ",";
            empty.back() = false;
            out += '\n';
            out.append(empty.size() * 2, ' ');
        }
        if (key)
        {
            quote(key);
            out += ": ";
        }
    }
    void open(pcstr key, char bracket)
    {
        item(key);
        out += bracket;
        empty.push_back(true);
    }
    void close(char bracket)
    {
        const bool was_empty = empty.back();
        empty.pop_back();
        if (!was_empty)
        {
            out += '\n';
            out.append(empty.size() * 2, ' ');
        }
        out += bracket;
    }
    void text(pcstr key, const xr_string& value)
    {
        item(key);
        quote(value);
    }
    void flag(pcstr key, bool value)
    {
        item(key);
        out += value ? "true" : "false";
    }
    void number(pcstr key, u64 value)
    {
        item(key);
        string32 buffer;
        xr_sprintf(buffer, "%llu", static_cast<unsigned long long>(value));
        out += buffer;
    }
    void texts(pcstr key, const xr_vector<xr_string>& values)
    {
        open(key, '[');
        for (const xr_string& value : values)
            text(nullptr, value);
        close(']');
    }
};
} // namespace

void WriteAddonsReport(pcstr when)
{
    ContentReportJson json;
    json.open(nullptr, '{');
    json.number("format", kContentReportFormat);
    json.number("written", static_cast<u64>(std::time(nullptr))); // seconds since 1970 (UTC)
    json.text("when", when ? when : "");
    // The rules of [conflicts] the report shows (rules, file_conflicts, merges) are those of appdata/addons.ltx as of
    // this time (seconds since 1970, UTC): the file is read at the start only, the report is written again later
    // (level, exit, console) - the launcher compares the time of the file with it. 0 - no stack built in this run
    json.number("content_built", g_content_built);
    {
        string32 api;
        xr_sprintf(api, "%u.%u.%u", GWP_API_VERSION_MAJOR, GWP_API_VERSION_MINOR, GWP_API_VERSION_PATCH);
        json.text("plugin_api", api);
    }

    // The addons in their order (the host: appdata/addons.ltx, depends, load_after)
    json.open("addons", '[');
    for (const AddonInfo& addon : ListAddons())
    {
        json.open(nullptr, '{');
        json.text("id", addon.id);
        json.text("version", addon.version);
        json.text("type", addon.type);
        json.text("state", addon.state);
        json.text("plugin", addon.plugin);
        json.text("plugin_state", addon.plugin_state);
        json.text("reason", addon.reason);
        json.text("dir", addon.dir);
        json.texts("load_after", addon.load_after);
        json.texts("patches", addon.patches); // [addon] patches: a compatibility patch of these (addon_merge.h)
        json.text("game_version", addon.game_version); // [addon] game_version: the base of its replacements
        xr_vector<xr_string> layers;
        for (const ContentLayer& layer : g_content_mounted)
        {
            if (layer.owner == addon.id)
                layers.push_back(layer.layer_id);
        }
        json.texts("layers", layers); // the mounted ones; empty - no content in the game now
        json.close('}');
    }
    json.close(']');

    json.open("slots", '[');
    for (const slots::SlotInfo& slot : slots::Slots())
    {
        json.open(nullptr, '{');
        json.text("id", slot.id);
        json.text("owner", slot.owner);
        json.text("active", slot.active_provider().id);
        json.text("next", slots::NextStartProvider(slot)); // the choice of the next start
        json.flag("save_bound", slot.save_bound);
        json.flag("restart", slot.restart);
        xr_vector<xr_string> providers;
        for (const slots::ProviderInfo& provider : slot.providers)
            providers.push_back(provider.id);
        json.texts("providers", providers);
        json.close('}');
    }
    json.close(']');

    json.open("stack", '['); // bottom up
    for (const ContentLayer& layer : g_content_mounted)
    {
        json.open(nullptr, '{');
        json.text("layer", layer.layer_id);
        json.text("owner", layer.owner);
        json.texts("yielded", layer.yielded);
        json.close('}');
    }
    json.close(']');

    json.open("rules", '[');
    for (const ContentRuleStatus& status : g_content_rules)
    {
        json.open(nullptr, '{');
        json.text("kind", status.kind);
        json.text("winner", status.winner);
        if (status.kind == "atom") // target "<file>|<atom>" (addon_merge.h, ContentMergeRuleStatuses)
        {
            const size_t bar = status.target.find('|');
            json.text("file", status.target.substr(0, bar));
            json.text("atom", bar == xr_string::npos ? xr_string() : status.target.substr(bar + 1));
        }
        else
            json.text(status.kind == "pair" ? "loser" : "file", status.target);
        json.flag("applied", status.applied);
        json.text("note", status.note);
        json.close('}');
    }
    json.close(']');

    json.open("file_conflicts", '[');
    for (const ContentConflict& conflict : g_content_conflicts)
    {
        json.open(nullptr, '{');
        json.text("file", conflict.name);
        // The addon of the winner layer; "base", "none" (no file now) - as they are; "merged" - the layer of the merged
        // files (addons_merged); "" - another layer of no addon
        const auto winner = std::find(conflict.layers.begin(), conflict.layers.end(), conflict.winner);
        const bool plain = conflict.winner == "base" || conflict.winner == "none";
        json.text("winner", winner != conflict.layers.end() ? conflict.owners[winner - conflict.layers.begin()] :
                plain ? conflict.winner : conflict.winner == kMergedLayerId ? xr_string("merged") : xr_string());
        json.text("winner_layer", conflict.winner);
        json.texts("addons", conflict.owners);
        json.texts("layers", conflict.layers);
        json.text("resolved_by", conflict.resolved_by);
        json.text("note", conflict.note);
        json.close('}');
    }
    json.close(']');

    // The patches recorded so far: the configs read since the last change of the layers (13 4.2, dltx_conflicts)
    xr_vector<xray::content_patches::Conflict> patches;
    xray::content_patches::CollectConflicts(patches);
    json.open("patch_conflicts", '[');
    for (const xray::content_patches::Conflict& conflict : patches)
    {
        json.open(nullptr, '{');
        json.text("kind", xray::content_patches::AtomKindName(conflict.kind));
        json.text("file", conflict.file);
        json.text("atom", conflict.atom);
        json.texts("addons", conflict.owners); // in the order of application
        json.text("winner", conflict.owners.empty() ? xr_string() : conflict.owners.back());
        json.close('}');
    }
    json.close(']');

    // The files several addons replace, merged at the last build of the stack (addon_merge.h; 15-merge-implementation.md
    // 5.3). ancestors: the ancestor of each side - "base" (the current base), "delta:<tag>" (S9: carried over by the
    // delta of the catalog), "current_base_guessed" (no delta, game_version older than the game), "stale_delta"
    json.open("merges", '[');
    for (const ContentMergeFile& merge : ContentMerges())
    {
        json.open(nullptr, '{');
        json.text("file", merge.file);
        json.text("kind", merge.kind);
        json.text("status", merge.status);
        json.text("note", merge.note);
        json.texts("addons", merge.addons);
        json.texts("layers", merge.layers);
        json.texts("changed", merge.changed);
        json.flag("cached", merge.cached);
        json.flag("served", merge.served);
        json.open("ancestors", '{');
        for (size_t i = 0; i < merge.addons.size(); ++i)
            json.text(merge.addons[i].c_str(), i < merge.ancestors.size() ? merge.ancestors[i] : xr_string("base"));
        json.close('}');
        json.number("atoms_merged", merge.atoms_merged);
        json.number("adjacent", merge.adjacent);
        json.texts("adjacent_atoms", merge.adjacent_atoms);
        json.texts("reordered", merge.reordered);
        json.open("conflicts", '[');
        for (const ContentMergeFile::Conflict& c : merge.conflicts)
        {
            json.open(nullptr, '{');
            json.text("atom", c.atom);
            json.texts("addons", c.addons);
            json.text("winner", c.winner);
            json.text("resolved_by", c.resolved_by);
            json.texts("lost", c.lost);
            json.number("base_line", c.base_line);
            json.flag("patch_stale", c.stale);
            json.close('}');
        }
        json.close(']');
        // The compatibility patches whose targets replace the file (15 section 6.1): applied | stale (patch_stale) |
        // skipped
        json.open("patches", '[');
        for (const ContentMergeFile::Patch& patch : merge.patches)
        {
            json.open(nullptr, '{');
            json.text("id", patch.id);
            json.texts("for", patch.targets);
            json.text("status", patch.status);
            json.text("note", patch.note);
            json.texts("stale_targets", patch.stale_targets);
            json.number("resolved", patch.resolved);
            json.close('}');
        }
        json.close(']');
        json.texts("rules_applied", merge.rules_applied); // the atoms the atom: rules of the player decided
        // S9 (15 section 2.4): the atoms of a delta the base changed too, the addon wins them; reason: both_changed |
        // base_deleted | both_added | rebase_failed (note - why); resolved_by: "rebase" (unresolved) | "player" (an
        // atom: rule decided it, winner - core or an addon)
        json.open("rebase_conflicts", '[');
        for (const ContentMergeFile::RebaseConflict& c : merge.rebase_conflicts)
        {
            json.open(nullptr, '{');
            json.text("atom", c.atom);
            json.text("addon", c.addon);
            json.text("reason", c.reason);
            json.text("winner", c.winner);
            json.text("resolved_by", c.resolved_by);
            if (!c.note.empty())
                json.text("note", c.note);
            json.close('}');
        }
        json.close(']');
        json.number("unresolved", merge.unresolved);
        json.text("error", merge.error);
        json.close('}');
    }
    json.close(']');
    const ContentMergeSummary merge_summary = ContentMergesSummary();
    json.open("merge_summary", '{');
    json.number("files", merge_summary.files);
    json.number("merged", merge_summary.merged);
    json.number("fallback", merge_summary.fallback);
    json.number("unresolved", merge_summary.unresolved);
    json.number("patch_stale", merge_summary.patch_stale);
    json.number("rebased", merge_summary.rebased); // S9: the sides carried over by their deltas
    json.number("stale_delta", merge_summary.stale_delta);
    json.number("guessed", merge_summary.guessed); // no delta, game_version older than the game
    json.number("guessed_addons", merge_summary.guessed_addons); // such addons (one warning each)
    // The version of the parse into atoms (the launcher takes the delta of this version, 15 section 7.2) and the
    // release tag of the game (configs\gw_release.ltx; empty - unknown)
    json.text("algo", xray::content_merge::kAlgorithmVersion);
    json.text("game", GameReleaseTag());
    json.flag("strict", merge_summary.strict); // -addon_strict: unresolved ones forbid the start of a level (15 5.4)
    json.close('}');
    json.close('}');
    json.out += '\n';

    string_path file_name;
    FS.update_path(file_name, FS.path_exist("$logs$") ? "$logs$" : "$app_data_root$", kContentReportName);
    const std::filesystem::path file = PathFromUtf8(file_name); // UTF-8, as every FS path
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    // "<report>.<pid>.tmp": two instances of the game sharing the appdata (and $logs$) never write one temporary file
#if defined(XR_PLATFORM_WINDOWS)
    const unsigned long pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    std::filesystem::path temporary = file;
    temporary += ("." + std::to_string(pid) + ".tmp").c_str();
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(json.out.data(), static_cast<std::streamsize>(json.out.size()));
        out.close(); // the last bytes are written here: a full disk shows only after it, a cut report is not renamed
        if (out.fail())
        {
            Msg("! [addons] cannot write [%s]: the report of the addons is not updated", file_name);
            std::filesystem::remove(temporary, error);
            return;
        }
    }
    std::filesystem::rename(temporary, file, error);
    if (error)
    {
        Msg("! [addons] cannot replace [%s] (%s): the report of the addons is not updated", file_name,
            ErrorText(error).c_str());
        std::filesystem::remove(temporary, error);
        return;
    }
    Msg("* [addons] report (%s): %u addon(s), %u file conflict(s), %u patch conflict(s) [%s]", when ? when : "",
        u32(ListAddons().size()), u32(g_content_conflicts.size()), u32(patches.size()), file_name);
}

void PrintAddonFiles(pcstr addon_id, bool all)
{
    // Every layer of the addon, bottom up: its common content, the sets of its slots, its location pack
    u32 shown = 0;
    const u16 count = FS.layers_count();
    for (u16 index = 1; addon_id && addon_id[0] && index <= count; ++index)
    {
        const pcstr owner = FS.layer_owner(index);
        if (!owner || 0 != xr_stricmp(owner, addon_id)) // the ids are lower case, the console line may be not
            continue;
        FS.print_layer(index, true, all);
        ++shown;
    }
    if (!shown)
    {
        Msg("- [addon:%s] no content mounted (the content of the addons is mounted at the first frame of the main "
            "menu; a location pack and the set of a slot provider - while they are active only)", addon_id);
    }
}

void PrintAddonResource(pcstr name)
{
    string_path relative;
    // The console line has no limit: the name must leave room for a game folder before it (string_path)
    if (!name || xr_strlen(name) >= sizeof(relative) / 2)
    {
        Msg("- addon_resource: the name is too long");
        return;
    }
    xr_strcpy(relative, name);
    for (char* c = relative; *c; ++c)
    {
        if (*c == '/')
            *c = '\\';
    }
    const bool has_ext = strext(relative) != nullptr;

    struct Candidate
    {
        pcstr path;
        pcstr ext; // added when the name has none
    };
    // As the engine looks them up: the level first, then the game folders (a texture, a model, a motion)
    constexpr Candidate candidates[] = {
        { "$game_data$", "" },
        { "$level$", ".dds" },
        { "$game_textures$", ".dds" },
        { "$level$", ".ogf" },
        { "$game_meshes$", ".ogf" },
        { "$game_meshes$", ".omf" },
    };
    u32 found = 0;
    xr_vector<xr_string> shown; // a name with an extension is tried as is under each path once
    for (const Candidate& candidate : candidates)
    {
        if (!FS.path_exist(candidate.path))
            continue;
        string_path fn;
        if (!FS.exist(fn, candidate.path, relative, has_ext ? "" : candidate.ext))
            continue;
        if (std::find(shown.begin(), shown.end(), fn) != shown.end())
            continue;
        shown.emplace_back(fn);
        const CLocatorAPI::file* desc = FS.GetFileDesc(fn);
        if (!desc)
            continue;
        ++found;
        string_path physical;
        if (desc->layer && FS.physical_name(*desc, physical))
        {
            pcstr owner = FS.layer_owner(desc->layer);
            Msg("- %s: layer %u '%s'%s%s%s, file [%s]", fn, desc->layer, FS.layer_id(desc->layer),
                owner ? " (addon '" : "", owner ? owner : "", owner ? "')" : "", physical);
        }
        else
            Msg("- %s: base%s", fn, desc->vfs != size_t(-1) ? " (archive)" : ""); // -1: a loose file (LocatorAPI.cpp)
    }
    if (!found)
        Msg("- [%s]: not found in $game_data$, $level$, $game_textures$ (.dds), $game_meshes$ (.ogf, .omf)", name);
}
} // namespace gw::addons
