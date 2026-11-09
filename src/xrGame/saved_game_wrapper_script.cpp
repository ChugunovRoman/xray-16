////////////////////////////////////////////////////////////////////////////
//	Module 		: saved_game_wrapper_script.cpp
//	Created 	: 21.02.2006
//  Modified 	: 21.02.2006
//	Author		: Dmitriy Iassenev
//	Description : saved game wrapper class script export
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"

#include "saved_game_wrapper.h"
#include "ai_space.h"
#include "xr_time.h"
#include "addon_event_bus.h"
#include "level_pack.h"
#include "addon_slots.h"
#include "addon_content.h"
#include "addon_merge.h"
#include "addon_lua_entry.h"
#include "addon_host.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"

void CSavedGameWrapper::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CSavedGameWrapper>("CSavedGameWrapper")
            .def(constructor<pcstr>())
            .def("game_time", +[](const CSavedGameWrapper* self)
            {
                return (xrTime(self->game_time()));
            })
            .def("level_id", &CSavedGameWrapper::level_id)
            .def("level_name", &CSavedGameWrapper::level_name)
            .def("actor_health", &CSavedGameWrapper::actor_health)
            // data(key): a persistent value of the data bus in the save (gw_game_state: "gw/game/<name>") -
            // string, number or boolean; nil when the save has no such key (or no data at all, or it is invalid)
            .def("data", +[](const CSavedGameWrapper* self, lua_State* L, pcstr key) -> luabind::object
            {
                // a valid object for nil too: luabind::object() (no state) cannot be pushed back to Lua
                if (const GwpValue* value = self->data(key))
                    gw::addons::events::PushLuaValue(L, *value);
                else
                    lua_pushnil(L);
                luabind::object result(luabind::from_stack(L, -1));
                lua_pop(L, 1);
                return result;
            }),

        // is_level_pack_installed(id): the load dialog marks and refuses saves of a pack that is not installed
        def("is_level_pack_installed", &gw::IsLevelPackInstalled),

        def("valid_saved_game", (bool (*)(pcstr))(&CSavedGameWrapper::valid_saved_game))
    ];

    // Location packs (plans/addons_api_support/10-location-packs.md)
    module(luaState, "level_pack")
    [
        // list(): { {id, version, save_compat, name, description, preview, builtin, active}, ... } - the built-in
        // pack first; name / description are string table ids
        def("list", +[](lua_State* L) -> luabind::object
        {
            luabind::object result = luabind::newtable(L);
            const gw::LevelPackInfo& active = gw::ActiveLevelPack();
            int index = 0;
            for (const gw::LevelPackInfo& pack : gw::LevelPacks())
            {
                luabind::object item = luabind::newtable(L);
                item["id"] = pack.id.c_str();
                item["version"] = pack.version.c_str();
                item["save_compat"] = pack.save_compat;
                item["name"] = pack.name.c_str();
                item["description"] = pack.description.c_str();
                item["preview"] = pack.preview.c_str();
                item["builtin"] = pack.builtin();
                item["active"] = &pack == &active;
                result[++index] = item;
            }
            return result;
        }),
        def("active", +[]() -> pcstr { return gw::ActiveLevelPack().id.c_str(); }),
        // switch(id, action): at the next frame of the main menu; then ui_main_menu.on_level_pack_applied(id, action,
        // ok) in the new Lua state. false - an unknown pack or a game is on
        def("switch", &gw::RequestLevelPackSwitch),
        def("switch_pending", &gw::LevelPackSwitchPending),
        // check_save(save): true - load it now; false - not now (another pack: the switch and the load follow; a pack
        // not installed or incompatible: ui_load_dialog.on_level_pack_missing / on_level_pack_incompatible)
        def("check_save", &gw::CanLoadSaveLevelPack)
    ];

    // The objects of the save the engine skipped at its load (09-content-addons-vfs.md section 8, C8: the section of a
    // switched off addon, and the objects inside them). The Lua side: alife_storage_manager.script, the event
    // on_save_objects_skipped
    module(luaState, "alife_save")
    [
        // skipped_objects(): { [id] = section } of the last loaded save; {} when nothing was skipped or there is no
        // ALife. Means something only during that load: after it the ids are free for new objects
        def("skipped_objects", +[](lua_State* L) -> luabind::object
        {
            luabind::object result = luabind::newtable(L);
            const CALifeSimulator* alife = ai().get_alife();
            if (!alife)
                return result;
            const xr_vector<ALife::_OBJECT_ID>& ids = alife->objects().skipped_on_load();
            const xr_vector<shared_str>& sections = alife->objects().skipped_sections_on_load();
            for (size_t i = 0; i < ids.size(); ++i)
            {
                const pcstr section = i < sections.size() ? sections[i].c_str() : nullptr;
                result[u32(ids[i])] = section ? section : "";
            }
            return result;
        })
    ];

    // Slots (plans/addons_api_support/13-extension-model.md, section 4). The Lua facade is gw_slots.script.
    module(luaState, "addon_slots")
    [
        // list(): { [slot] = {id, name, owner, save_bound, restart, active, chosen, providers = { {id, name,
        // description, preview, entry, panel, version, builtin, active}, ... }}, ... } - name / description are string
        // table ids; chosen - the provider of the next start (it differs from active for a slot taking effect after
        // a restart)
        def("list", +[](lua_State* L) -> luabind::object
        {
            luabind::object result = luabind::newtable(L);
            for (const gw::slots::SlotInfo& slot : gw::slots::Slots())
            {
                luabind::object item = luabind::newtable(L);
                item["id"] = slot.id.c_str();
                item["name"] = slot.name.c_str();
                item["owner"] = slot.owner.c_str();
                item["save_bound"] = slot.save_bound;
                item["restart"] = slot.restart;
                item["active"] = slot.active_provider().id.c_str();
                item["chosen"] = gw::slots::NextStartProvider(slot).c_str();
                luabind::object providers = luabind::newtable(L);
                int index = 0;
                for (const gw::slots::ProviderInfo& provider : slot.providers)
                {
                    luabind::object entry = luabind::newtable(L);
                    entry["id"] = provider.id.c_str();
                    entry["name"] = provider.name.c_str();
                    entry["description"] = provider.description.c_str();
                    entry["preview"] = provider.preview.c_str();
                    entry["entry"] = provider.entry.c_str();
                    entry["panel"] = provider.panel.c_str();
                    entry["version"] = provider.version.c_str();
                    entry["builtin"] = provider.builtin;
                    entry["active"] = &provider == &slot.active_provider();
                    providers[++index] = entry;
                }
                item["providers"] = providers;
                result[slot.id.c_str()] = item;
            }
            return result;
        }),
        // active(slot): the id of its active provider; "" for an unknown slot
        def("active", +[](pcstr slot) -> pcstr
        {
            const gw::slots::SlotInfo* info = gw::slots::FindSlot(slot);
            return info ? info->active_provider().id.c_str() : "";
        }),
        // select(slot, provider, action): at the next frame of the main menu (a slot taking effect after a restart:
        // only the choice); then gw_slots.on_slots_applied(action, ok) in the new Lua state. false - an unknown slot
        // or provider, a provider that cannot work, a game is on
        def("select", &gw::slots::RequestSelect),
        def("pending", &gw::slots::ApplyPending),
        // conflicts(): { {file, winner, winner_layer, winner_addon, resolved_by, note, addons = {...}, layers = {...}},
        // ... } - the files several addons give (console addon_conflicts, file_conflicts of addons_report.json):
        //  - winner, winner_layer - the layer that serves the file ("base" - none of them; "none" - no file now);
        //  - winner_addon - the addon of that layer ("base", "none" as they are; "merged" - the layer addons_merged of
        //    the merged files; "" - another layer of no addon);
        //  - resolved_by - what decided it: "order", "load_after", "pair", "player", "slot", "pack", "merge" (the
        //    merged file of the addons serves it, addon_merge.h) (ContentConflict);
        //  - note - a rule about the file that does not hold, and why; "" - none;
        //  - addons, layers - bottom up, in parallel: addons[i] gives the layer layers[i]
        def("conflicts", +[](lua_State* L) -> luabind::object
        {
            luabind::object result = luabind::newtable(L);
            int index = 0;
            for (const gw::addons::ContentConflict& conflict : gw::addons::ContentConflicts())
            {
                luabind::object item = luabind::newtable(L);
                item["file"] = conflict.name.c_str();
                item["winner"] = conflict.winner.c_str();
                item["winner_layer"] = conflict.winner.c_str();
                // The addon of the winner layer, as addons_report.json gives it
                const size_t at = size_t(std::find(conflict.layers.begin(), conflict.layers.end(), conflict.winner) -
                    conflict.layers.begin());
                pcstr winner_addon = "";
                if (at < conflict.layers.size() && at < conflict.owners.size())
                    winner_addon = conflict.owners[at].c_str();
                else if (conflict.winner == "base" || conflict.winner == "none")
                    winner_addon = conflict.winner.c_str();
                else if (conflict.winner == gw::addons::kMergedLayerId)
                    winner_addon = "merged";
                item["winner_addon"] = winner_addon;
                item["resolved_by"] = conflict.resolved_by.c_str();
                item["note"] = conflict.note.c_str();
                luabind::object owners = luabind::newtable(L);
                luabind::object layers = luabind::newtable(L);
                for (size_t i = 0; i < conflict.owners.size() && i < conflict.layers.size(); ++i)
                {
                    owners[int(i + 1)] = conflict.owners[i].c_str();
                    layers[int(i + 1)] = conflict.layers[i].c_str();
                }
                item["addons"] = owners;
                item["layers"] = layers;
                result[++index] = item;
            }
            return result;
        }),
        // merges(): { {file, kind, status, note, cached, served, atoms_merged, adjacent, unresolved, error, addons =
        // {...}, changed = {...}, rules_applied = {...}, conflicts = { {atom, winner, resolved_by, base_line, stale,
        // addons = {...}, lost = {...}}, ...}, patches = { {id, status, note, resolved, targets = {...},
        // stale_targets = {...}}, ...}}, ...} - the files several addons replace, merged at the last build of the
        // content (addon_merge.h, console addon_merges, merges of addons_report.json):
        //  - status: "merged", "merged_lines", "single", "unchanged", "fallback", "skipped" (note says why);
        //  - served: the merged file serves the name now (the layer addons_merged);
        //  - addons: the sides bottom up, changed: those that changed the base;
        //  - conflicts: one atom two addons changed differently - the winner's atom is taken as a whole; resolved_by
        //    "player" (atom: rule), "patch" (a compatibility patch; stale - a stale one), else the order of the stack;
        //  - patches: the compatibility patches of these addons: "applied", "stale" (patch_stale), "skipped";
        //  - rules_applied: the atoms the atom: rules of the player decided;
        //  - ancestors = {[id] = "base" | "delta:<tag>" | "current_base_guessed" | "stale_delta"} - the ancestor of each
        //    side (S9), rebase_conflicts = { {atom, addon, reason, note, winner, resolved_by}, ...} - the atoms of a
        //    delta the base changed too; resolved_by "rebase" (unresolved, the addon wins) | "player" (an atom: rule
        //    decided it)
        def("merges", +[](lua_State* L) -> luabind::object
        {
            const auto list = [L](const xr_vector<xr_string>& values) {
                luabind::object table = luabind::newtable(L);
                for (size_t i = 0; i < values.size(); ++i)
                    table[int(i + 1)] = values[i].c_str();
                return table;
            };
            luabind::object result = luabind::newtable(L);
            int index = 0;
            for (const gw::addons::ContentMergeFile& merge : gw::addons::ContentMerges())
            {
                luabind::object item = luabind::newtable(L);
                item["file"] = merge.file.c_str();
                item["kind"] = merge.kind.c_str();
                item["status"] = merge.status.c_str();
                item["note"] = merge.note.c_str();
                item["cached"] = merge.cached;
                item["served"] = merge.served;
                item["atoms_merged"] = merge.atoms_merged;
                item["adjacent"] = merge.adjacent;
                item["unresolved"] = merge.unresolved;
                item["error"] = merge.error.c_str();
                item["addons"] = list(merge.addons);
                item["changed"] = list(merge.changed);
                luabind::object conflicts = luabind::newtable(L);
                int conflict_index = 0;
                for (const gw::addons::ContentMergeFile::Conflict& c : merge.conflicts)
                {
                    luabind::object entry = luabind::newtable(L);
                    entry["atom"] = c.atom.c_str();
                    entry["winner"] = c.winner.c_str();
                    entry["resolved_by"] = c.resolved_by.c_str();
                    entry["base_line"] = c.base_line;
                    entry["stale"] = c.stale;
                    entry["addons"] = list(c.addons);
                    entry["lost"] = list(c.lost);
                    conflicts[++conflict_index] = entry;
                }
                item["conflicts"] = conflicts;
                luabind::object patches = luabind::newtable(L);
                int patch_index = 0;
                for (const gw::addons::ContentMergeFile::Patch& patch : merge.patches)
                {
                    luabind::object entry = luabind::newtable(L);
                    entry["id"] = patch.id.c_str();
                    entry["status"] = patch.status.c_str();
                    entry["note"] = patch.note.c_str();
                    entry["resolved"] = patch.resolved;
                    entry["targets"] = list(patch.targets);
                    entry["stale_targets"] = list(patch.stale_targets);
                    patches[++patch_index] = entry;
                }
                item["patches"] = patches;
                item["rules_applied"] = list(merge.rules_applied);
                luabind::object ancestors = luabind::newtable(L);
                for (size_t i = 0; i < merge.addons.size() && i < merge.ancestors.size(); ++i)
                    ancestors[merge.addons[i].c_str()] = merge.ancestors[i].c_str();
                item["ancestors"] = ancestors;
                luabind::object rebase = luabind::newtable(L);
                int rebase_index = 0;
                for (const gw::addons::ContentMergeFile::RebaseConflict& c : merge.rebase_conflicts)
                {
                    luabind::object entry = luabind::newtable(L);
                    entry["atom"] = c.atom.c_str();
                    entry["addon"] = c.addon.c_str();
                    entry["reason"] = c.reason.c_str();
                    entry["note"] = c.note.c_str();
                    entry["winner"] = c.winner.c_str();
                    entry["resolved_by"] = c.resolved_by.c_str();
                    rebase[++rebase_index] = entry;
                }
                item["rebase_conflicts"] = rebase;
                result[++index] = item;
            }
            return result;
        }),
        // merge_summary(): {files, merged, fallback, unresolved, patch_stale, rebased, stale_delta, guessed,
        // guessed_addons, algo, game, strict, blocked} - the summary of
        // merges() (merge_summary of addons_report.json): unresolved - the conflicts of atoms the order of the stack or
        // a stale compatibility patch decided and the fallbacks; strict - -addon_strict; blocked - strict and
        // unresolved: no level starts (gw::CanStartLevel)
        def("merge_summary", +[](lua_State* L) -> luabind::object
        {
            gw::addons::PushContentMergeSummary(L);
            luabind::object summary(luabind::from_stack(L, -1));
            lua_pop(L, 1);
            return summary;
        })
    ];

    // The Lua parts of the addons, [lua] entry (addon_lua_entry.h). The Lua facade is gw_addons_entry.script.
    module(luaState, "addon_lua")
    [
        // list(): { {id, version, type, path, entry}, ... } - the addons whose Lua part is active now, in the order
        // of the addons (the engine calls their on_load in this order at the start of each Lua state); path - the
        // addon folder (UTF-8, the trailing separator)
        def("list", +[](lua_State* L) -> luabind::object
        {
            luabind::object result = luabind::newtable(L);
            int index = 0;
            for (const gw::addons::LuaEntryInfo& info : gw::addons::ActiveLuaEntries())
            {
                luabind::object item = luabind::newtable(L);
                item["id"] = info.id.c_str();
                item["version"] = info.version.c_str();
                item["type"] = info.type.c_str();
                item["path"] = info.path.c_str();
                item["entry"] = info.entry.c_str();
                result[++index] = item;
            }
            return result;
        }),
        // entries(): { [<module, low case>] = <addon id> } - the [lua] entry of every addon that has one, active or
        // not (failed, a provider not chosen, no content). axr_main.on_game_start skips the module of an addon whose
        // Lua part did not load in this state: the file may lie in the common content of the addon all the same
        def("entries", +[](lua_State* L) -> luabind::object
        {
            luabind::object result = luabind::newtable(L);
            for (const gw::addons::AddonInfo& addon : gw::addons::ListAddons())
            {
                if (addon.lua_entry.empty())
                    continue;
                string256 module;
                xr_strcpy(module, addon.lua_entry.c_str());
                xr_strlwr(module);
                result[module] = addon.id.c_str();
            }
            return result;
        }),
        // module_owner(module): the addon whose module scripts\<module>.script is - its layer serves the file and the
        // file replaces no script of the base or of a lower addon (gw::addons::AddonScriptModuleOwner); "" - none.
        // ui_mm_appearance / gw_slots check [slot.<slot>] panel / entry with it
        def("module_owner", +[](pcstr module) -> pcstr
        {
            // luabind copies the string into Lua on return; the main thread only
            static xr_string owner;
            owner = gw::addons::AddonScriptModuleOwner(module);
            return owner.c_str();
        })
    ];
}
