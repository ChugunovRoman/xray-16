#pragma once

// The content of an addon in the file system (plans/addons_api_support/14-addon-resource-scoping.md).
//
// <addon>\gamedata\ is mounted as a layer of the file system over the game data: its files take the names the base
// has (a file of the same name replaces the base one for everybody - public). Its textures\ and meshes\ are mounted
// once more, as the alias roots of the same layer, under the names textures\@<id>\ and meshes\@<id>\ - private files
// included. The models of the addon (their ogf lies in the layer) take their textures and motions through the alias
// (the render: VisualOwnerScope), so another addon or the base replacing the same name does not touch them, and
// a private file of the addon ([private] of addon.ltx) is seen by its own models only.
//
// [private] of addon.ltx - one mask per line, relative to gamedata\ ('*' takes the separators too):
//   [private]
//   textures\wpn\wpn_ak74.dds
//   textures\my_addon\*
//
// The location packs, the common content of the other addons and the sets of the slot providers are mounted this
// way, as one stack (SyncContentLayers; the order: plans/addons_api_support/13-extension-model.md, 4.5).

#include "xrCore/ContentMerge/Plan.h"

namespace gw::addons
{
struct AddonInfo;

// The layer `layer_id` (FS.mount_layer) with the alias roots of the addon, owner = the addon id. Logs the number of
// the base files the addon replaces ("~ [addon:<id>] replaces N base files") and, apart, of the files of the content
// mounted before it the layer covers ("~ [addon:<id>] <layer>: covers N file(s) of the content mounted before it").
// False - nothing is left mounted.
bool MountContentLayer(const AddonInfo& addon, pcstr layer_id);
// The same for another folder of the addon (the set of a slot provider: <addon>\slots\<slot>\gamedata\): `gamedata`
// is the folder (UTF-8, the trailing separator) mounted over the game data. `yielded` - the files the layer gives up
// to another addon by a rule of the player (ContentLayer::yielded): mounted as the private files are
bool MountContentFolder(const AddonInfo& addon, pcstr gamedata, pcstr layer_id,
    const xr_vector<xr_string>* yielded = nullptr);
// The layer goes with all its roots at once. The file system unmounts the top layer only: the layers mounted after
// it go first.
bool UnmountContentLayer(pcstr layer_id);

// One layer of the content stack (13-extension-model.md, 4.5): base -> the common content of the addons in their
// order -> the sets of the chosen slot providers -> the merged files (addon_merge.h) -> the location pack and the
// folder its writes go to. The struct of the plan of the library (xrCore ContentMerge/Plan.h PlanLayer: the plan of the
// merge and the rules pair: / file: are decided there, the same code as gw_merge plan):
//  - layer_id: the FS layer id - "addon:<id>", "slot:<slot>:<id>", "addons_merged", "level_pack:<id>",
//    "level_pack_user:<id>";
//  - owner: the addon id (the alias roots, the conflicts); empty - a writable layer of nobody;
//  - folder: mounted over the game data: UTF-8, the trailing separator;
//  - private_masks: [private] of the addon (an owned layer);
//  - writable: a plain writable layer (the writes of a pack, 10-location-packs.md 2.5), no owner;
//  - group: the layers of one group fail together (a pack and its writes): a failed one takes the mounted ones of its
//    group off and the rest of the group is not mounted;
//  - yielded: set by SyncContentLayers from file:<file> = <addon> of appdata/addons.ltx [conflicts]: the files
//    (relative to gamedata\, lower case, '\') this layer gives up so that a lower layer serves them. They stay
//    private: not at the game data, under the alias roots of the owner (its own models keep its textures);
//  - merged: the files several addons replace, merged (addon_merge.h, PlanContentMerge): no owner, no alias roots, not
//    writable; its folder is <appdata>\addons_merged\<set>\gamedata\ (one per set of the merged files).
using ContentLayer = xray::content_merge::PlanLayer;
// The layer gives `name` (relative to gamedata\, '\', the case of the index) at the game data: the file lies in its
// folder, no [private] mask of it and no file: rule of the player (yielded) keeps the file out. False for a layer of no
// addon (a writable one, the merged files)
bool ContentLayerServes(const ContentLayer& layer, const xr_string& name);
// Brings the mounted content to `desired`: the layers above the first one that differs go (top first), then the
// rest of `desired` is mounted in its order - a layer the stacks share at the bottom stays mounted. `failed_groups`
// gets the group (or the layer id, when it has no group) of each layer that did not mount. Then the conflicts of
// the files are collected again (addon_conflicts). Must not race with readers of the file system index.
// The rules of the conflicts (13-extension-model.md 4.2; 00-roadmap.md 5a, decision 11) apply to `desired` first:
//  - the order of the addons (the host: [addon] load_after of the authors included) - the default;
//  - pair:<winner> = <loser> of appdata/addons.ltx [conflicts]: the layers of the winner go right over the layers of
//    the loser - among the common content of the addons and among the sets of the providers. A set stays over the
//    common content of any addon (a provider brings its set whole, 13 4.1), the location pack over all;
//  - file:<file> = <addon | core>: every layer above the topmost layer of that addon that gives the file gives it up
//    (ContentLayer::yielded); core - every layer of an addon gives it up, the base serves it.
void SyncContentLayers(const xr_vector<ContentLayer>& desired, xr_vector<xr_string>& failed_groups);
const xr_vector<ContentLayer>& MountedContentLayers();

// The conflicts of the files (13-extension-model.md, 4.2): one name (relative to gamedata\) the public roots of
// layers of different addons give. The layer higher in the stack wins - the addon lower in the order of the addons.
struct ContentConflict
{
    xr_string name;
    xr_vector<xr_string> layers; // the layer ids that give the name, bottom up (the winner among them, when it is one)
    xr_vector<xr_string> owners; // the addons of those layers, the same order
    xr_string winner; // the layer that serves the name now ("base" - none of them)
    // What decided it: "order" (the order of the addons), "load_after" (of the winner, the author's pair rule), "pair"
    // and "player" (pair: / file: of appdata/addons.ltx [conflicts]), "slot" (the set of a chosen provider over the
    // common content), "pack" (the location pack over all). In this order: file: of the player, the part of the stack
    // of the winner against the layer right under it, then - within one part - pair, load_after, order. "merge" - the
    // merged file of the layer addons_merged serves the name (addon_merge.h), winner is that layer
    xr_string resolved_by;
    xr_string note; // a rule about the name that does not hold, and why; empty - none
};
const xr_vector<ContentConflict>& ContentConflicts();

// A rule of appdata/addons.ltx [conflicts] at the last build of the stack
// (xrCore ContentMerge/Plan.h RuleStatus): kind - "pair" | "file" ("atom": addon_merge.h); winner - the addon id
// ("core" for a file the base keeps); target - pair: the loser, file: the file (relative to gamedata\); applied - it
// took part in the build (a file rule: its winner gives the file); note - what it did, or why not
using ContentRuleStatus = xray::content_merge::RuleStatus;
const xr_vector<ContentRuleStatus>& ContentRuleStatuses();
// The summary in the log (after a sync): the number of the conflicts and the first ones
void LogContentConflicts();
// Console addon_conflicts: every conflict and what decided it, the rules; [addon id] - only those of the addon
void PrintContentConflicts(pcstr addon_id);

// appdata\logs\addons_report.json for the launcher (13-extension-model.md 4.2, 12-addon-breakage-without-api.md 4.4):
// the addons, the slots, the content stack, the rules and the conflicts of the files and of the patches (DLTX,
// XMLOverride, strings, ui textures - as many as the loaded configs recorded so far), the merged files (merges,
// merge_summary: addon_merge.h). UTF-8, written whole through a
// temporary file. After each build of the content stack, at the start of the slots, once a loaded level is in play
// (the configs of the level recorded their patches; CGamePersistent::OnFrame), at the exit of the game
// (CGamePersistent::OnAppEnd) and by the console (addons_report); `when` - what wrote it ("init", "startup",
// "content", "level", "exit", "console")
void WriteAddonsReport(pcstr when);

// Console addon_files <id> [all]: the public, private and replaced files of every mounted layer of the addon (its
// common content, the sets of its slots, its location pack), bottom up
void PrintAddonFiles(pcstr addon_id, bool all);
// Console addon_resource <name>: the layer that serves the name (relative to gamedata\, or a texture / a model / a
// motion without the folder and the extension: wpn\wpn_ak74, @<id>\wpn\wpn_ak74) and the file on the disk
void PrintAddonResource(pcstr name);
} // namespace gw::addons
