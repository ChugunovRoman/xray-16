#include "StdAfx.h"

#include "addon_slots.h"
#include "addon_host.h"
#include "addon_content.h"
#include "addon_event_bus.h"
#include "addon_lua_entry.h"
#include "addon_merge.h"
#include "level_pack.h"
#include "MainMenu.h"
#include "GamePersistent.h"
#include "spawn_smart_terrain_map_data_source.h"

#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/IGame_Level.h"
#include "xrEngine/Render.h"
#include "xrSound/Sound.h"
#include "xrEngine/XR_IOConsole.h"
#include "xrEngine/StringTable/StringTable.h"
#include "xrScriptEngine/script_engine.hpp"
#include "xrUICore/ui_base.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

class CScriptIniFile;
CScriptIniFile* reload_system_ini(); // script_ini_file_script.cpp
CScriptIniFile* reload_faction_editor_config(); // script_ini_file_script.cpp

typedef xr_vector<std::pair<shared_str, int>> STORY_PAIRS; // alife_simulator_script.cpp
extern STORY_PAIRS story_ids;
extern STORY_PAIRS spawn_story_ids;

bool reload_game_globals(); // xrgame_dll_detach.cpp

// The start position of a new game the faction select sets (Actor_Network.cpp; alife_simulator_script.cpp
// set_start_position*): a smart terrain name, the vertices of the game graph of the pack it was chosen on
extern Fvector g_start_position;
extern u16 g_start_game_vertex_id;
extern u32 g_start_level_vertex_id;
extern shared_str g_start_position_smart;

namespace gw::slots
{
const ProviderInfo* SlotInfo::find(pcstr provider) const
{
    if (!provider)
        return nullptr;
    for (const ProviderInfo& candidate : providers)
    {
        if (0 == xr_stricmp(candidate.id.c_str(), provider))
            return &candidate;
    }
    return nullptr;
}

namespace
{
constexpr pcstr kSlotCatalog = "misc\\slots\\slots.ltx"; // the slots of the base, $game_config$
constexpr pcstr kSlotCoreName = "st_slot_provider_core";
constexpr pcstr kSlotWorldName = "st_slot_world";
constexpr pcstr kSlotDefaultFiles = "slots"; // [slot.<slot>] files: <addon>\slots\<slot>\ by default
constexpr pcstr kSlotWorldGroup = "world"; // the group of the layers of the pack (LevelPackLayers)
constexpr pcstr kSlotLayerPrefix = "slot:";
constexpr pcstr kSlotAddonLayerPrefix = "addon:";

xr_vector<SlotInfo> g_slot_list;
bool g_slot_initialized = false;
u32 g_content_generation = 0; // ContentGeneration
xr_vector<addons::ContentLayer> g_slot_base_content; // the common content of the addons, in their order
xr_vector<xr_string> g_slot_failed_addons; // their required on-demand plugin failed: they cannot be active
// Addons with [slot.*] sections none of which became a provider (an unknown slot, a typo), addons of type location that
// are not a pack (no all.spawn): their plugin is not tied to any provider or pack - it is loaded at Init and stays, as
// the plugin of any other addon
xr_vector<xr_string> g_slot_plain_plugins;

struct SlotRequest
{
    bool pending = false;
    bool startup = false; // the first mount: the common content of the addons too
    bool world_set = false;
    xr_string world;
    xr_vector<std::pair<xr_string, xr_string>> providers; // slot -> provider
    xr_string action;
    bool world_callback = false; // ui_main_menu.on_level_pack_applied after it: level_pack.switch, a save to load
    bool rules = false; // a rule of appdata\addons.ltx [conflicts] changed: the stack is built again (RequestRebuild)
};
SlotRequest g_slot_request;

xr_string SlotLower(xr_string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return text;
}

xr_string SlotTrim(const xr_string& text)
{
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == xr_string::npos)
        return xr_string();
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// a-z 0-9 _ and '.' between the parts, up to 64 chars (the same rule as appdata\addons.ltx [slots] in addon_host)
bool SlotIdValid(const xr_string& id)
{
    if (id.empty() || id.size() > 64 || id.front() == '.' || id.back() == '.' || id.find("..") != xr_string::npos)
        return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
    });
}

// An addon folder (UTF-8: addon_host) for std::filesystem: the common helper of the addons (addon_host.h)
std::filesystem::path SlotUtf8Path(const xr_string& text) { return addons::PathFromUtf8(text); }

// A Lua module name: a global of the script engine (the file name without .script)
bool SlotEntryValid(const xr_string& entry)
{
    return !entry.empty() && entry.size() <= 64 && std::all_of(entry.begin(), entry.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}

// The default string id of a slot: st_slot_<slot with '_' for '.'>
xr_string SlotDefaultName(const xr_string& id)
{
    xr_string name = "st_slot_" + id;
    std::replace(name.begin(), name.end(), '.', '_');
    return name;
}

SlotInfo* FindSlotMutable(const xr_string& id)
{
    for (SlotInfo& slot : g_slot_list)
    {
        if (slot.id == id)
            return &slot;
    }
    return nullptr;
}

size_t SlotIndex(const xr_string& id)
{
    for (size_t i = 0; i < g_slot_list.size(); ++i)
    {
        if (g_slot_list[i].id == id)
            return i;
    }
    return size_t(-1);
}

bool SlotAddonFailed(const xr_string& id)
{
    return std::find(g_slot_failed_addons.begin(), g_slot_failed_addons.end(), id) != g_slot_failed_addons.end();
}

SlotInfo MakeSlot(const xr_string& id, const xr_string& owner)
{
    SlotInfo slot;
    slot.id = id;
    slot.owner = owner;
    slot.name = SlotDefaultName(id);
    ProviderInfo& core = slot.providers.emplace_back();
    core.id = kCoreProvider;
    core.name = kSlotCoreName;
    core.builtin = true;
    return slot;
}

// configs\misc\slots\slots.ltx: [<slot>] name = <string id>, apply = menu | restart, save_bound = true | false.
// Read before anything is mounted: the file of the base, not of an addon.
void ReadBaseSlots()
{
    string_path file_name;
    FS.update_path(file_name, "$game_config$", kSlotCatalog);
    if (!FS.exist(file_name))
    {
        Msg("! [slots] '%s' not found: the base declares the slot %s only", file_name, kWorldSlot);
        return;
    }
    const CInifile ini(file_name, true, true, false);
    for (const CInifile::Sect* section : ini.sections())
    {
        const xr_string id = SlotLower(section->Name.c_str());
        if (!SlotIdValid(id) || FindSlotMutable(id))
        {
            Msg("! [slots] %s: [%s] is not a slot id (a-z 0-9 _ .) or is there twice, ignored", kSlotCatalog,
                section->Name.c_str());
            continue;
        }
        SlotInfo slot = MakeSlot(id, kCoreProvider);
        const pcstr sect = section->Name.c_str();
        if (ini.line_exist(sect, "name"))
            slot.name = ini.r_string(sect, "name");
        if (ini.line_exist(sect, "save_bound"))
            slot.save_bound = ini.r_bool(sect, "save_bound");
        if (ini.line_exist(sect, "apply"))
        {
            const xr_string apply = SlotLower(ini.r_string(sect, "apply"));
            if (apply == "restart")
                slot.restart = true;
            else if (apply != "menu")
                Msg("~ [slots] %s: [%s] apply = %s (menu, restart), taken as menu", kSlotCatalog, sect, apply.c_str());
        }
        if (slot.save_bound && slot.restart)
        {
            Msg("~ [slots] %s: [%s] save_bound with apply = restart is not supported, taken as menu", kSlotCatalog,
                sect);
            slot.restart = false;
        }
        g_slot_list.push_back(std::move(slot));
    }
}

// [slots_declare] <addon id>.<name> = <string id>[, save_bound][, restart] (13-extension-model.md 4.3, 7)
void DeclareAddonSlot(const addons::AddonInfo& addon, const xr_string& key, const xr_string& value)
{
    const xr_string id = SlotLower(SlotTrim(key));
    const xr_string prefix = addon.id + ".";
    if (!SlotIdValid(id) || id.size() <= prefix.size() || id.compare(0, prefix.size(), prefix) != 0)
    {
        Msg("~ [addon:%s] [slots_declare] '%s': a slot of an addon is named %s<name> (a-z 0-9 _ .), ignored",
            addon.id.c_str(), key.c_str(), prefix.c_str());
        return;
    }
    if (const SlotInfo* existing = FindSlotMutable(id))
    {
        Msg("~ [addon:%s] [slots_declare] %s is declared already (by %s), ignored", addon.id.c_str(), id.c_str(),
            existing->owner.c_str());
        return;
    }
    SlotInfo slot = MakeSlot(id, addon.id);
    size_t start = 0;
    for (u32 part = 0; start <= value.size(); ++part)
    {
        size_t comma = value.find(',', start);
        if (comma == xr_string::npos)
            comma = value.size();
        const xr_string token = SlotTrim(value.substr(start, comma - start));
        start = comma + 1;
        if (part == 0)
        {
            if (!token.empty())
                slot.name = token;
            continue;
        }
        const xr_string flag = SlotLower(token);
        if (flag == "save_bound")
            slot.save_bound = true;
        else if (flag == "restart")
            slot.restart = true;
        else if (!flag.empty())
            Msg("~ [addon:%s] [slots_declare] %s: unknown flag '%s' (save_bound, restart), ignored", addon.id.c_str(),
                id.c_str(), token.c_str());
    }
    if (slot.save_bound && slot.restart)
    {
        // A save would ask for a provider that cannot be switched to before the load
        Msg("~ [addon:%s] [slots_declare] %s: save_bound and restart together are not supported, restart is ignored",
            addon.id.c_str(), id.c_str());
        slot.restart = false;
    }
    Msg("* [slots] %s declared by addon '%s'%s%s", id.c_str(), addon.id.c_str(), slot.save_bound ? ", save-bound" : "",
        slot.restart ? ", after a restart" : "");
    g_slot_list.push_back(std::move(slot));
}

// configs\misc\slots\ names of the catalog of the slots: slots.ltx and its DLTX patches mod_slots_*.ltx (ASCII, any
// case); `out` - the name in lower case
template <typename Char>
bool SlotCatalogFileName(const std::basic_string<Char>& name, xr_string& out)
{
    out.clear();
    for (const Char c : name)
    {
        const auto code = static_cast<std::make_unsigned_t<Char>>(c);
        if (code > 127)
            return false;
        out += static_cast<char>(std::tolower(static_cast<int>(code)));
    }
    constexpr pcstr prefix = "mod_slots_";
    constexpr size_t prefix_length = 10;
    if (out == "slots.ltx")
        return true;
    return out.size() > prefix_length + 4 && out.compare(0, prefix_length, prefix) == 0 &&
        out.compare(out.size() - 4, 4, ".ltx") == 0;
}

// A file or folder `name` (ASCII, lower case) in `folder`, in any case: the file system of the engine (the layers of the
// content) takes the names in any case, a case-sensitive one (POSIX) would not find "Configs" by "configs". Empty - none
std::filesystem::path SlotChildAnyCase(const std::filesystem::path& folder, pcstr name, bool directory)
{
    using Char = std::filesystem::path::value_type;
    const size_t length = xr_strlen(name);
    std::error_code error;
    std::filesystem::directory_iterator it(folder, error);
    for (const std::filesystem::directory_iterator end{}; !error && it != end; it.increment(error))
    {
        const std::filesystem::path::string_type native = it->path().filename().native();
        if (native.size() != length)
            continue;
        bool same = true;
        for (size_t i = 0; i < length && same; ++i)
        {
            const auto code = static_cast<std::make_unsigned_t<Char>>(native[i]);
            same = code < 128 && std::tolower(static_cast<int>(code)) == static_cast<unsigned char>(name[i]);
        }
        std::error_code type_error;
        if (same && (directory ? it->is_directory(type_error) : it->is_regular_file(type_error)))
            return it->path();
    }
    return {};
}

// 00-roadmap.md 5a, decision 36: the files of the base the engine reads before the content of the addons is mounted
// (CGamePersistent::OnAppStart: GMLib.Load; ReadBaseSlots) - a layer that brings them changes nothing. `folder`: a
// gamedata\ of the addon (UTF-8, the trailing separator). The names in any case (SlotChildAnyCase)
void WarnUnsupportedFiles(const xr_string& owner, const xr_string& folder)
{
    const std::filesystem::path root = SlotUtf8Path(folder);
    std::error_code error;
    if (!SlotChildAnyCase(root, "gamemtl.xr", false).empty())
    {
        Msg("~ [addon:%s] %sgamemtl.xr: the game materials are read before the content of the addons is mounted, an "
            "addon cannot change them (not supported) - the file has no effect", owner.c_str(), folder.c_str());
    }
    std::filesystem::path catalog = SlotChildAnyCase(root, "configs", true);
    if (!catalog.empty())
        catalog = SlotChildAnyCase(catalog, "misc", true);
    if (!catalog.empty())
        catalog = SlotChildAnyCase(catalog, "slots", true);
    if (catalog.empty())
        return;
    std::filesystem::directory_iterator it(catalog, error);
    for (const std::filesystem::directory_iterator end{}; !error && it != end; it.increment(error))
    {
        xr_string name;
        if (!SlotCatalogFileName(it->path().filename().native(), name) || !it->is_regular_file(error))
            continue;
        Msg("~ [addon:%s] %sconfigs\\misc\\slots\\%s: the catalog of the slots is read before the content of the "
            "addons is mounted, an addon cannot change it (not supported) - the file has no effect; an addon declares "
            "its own slots with [slots_declare] of addon.ltx", owner.c_str(), folder.c_str(), name.c_str());
    }
}

pcstr SlotSectionValue(const addons::AddonInfo::SlotSection& section, pcstr key)
{
    for (const auto& [name, value] : section.keys)
    {
        if (0 == xr_stricmp(name.c_str(), key))
            return value.c_str();
    }
    return nullptr;
}

// files = <folder of the addon>: relative, inside the addon (no drive, no root, no "..")
bool SlotFilesValid(const xr_string& files)
{
    if (files.empty() || files.find(':') != xr_string::npos || files[0] == '\\' || files[0] == '/')
        return false;
    size_t start = 0;
    while (start < files.size())
    {
        size_t end = files.find_first_of("\\/", start);
        if (end == xr_string::npos)
            end = files.size();
        // ".." leaves the addon; "." and an empty component (".\", "a\\b") name a folder already named - "." the
        // root of the addon, whose gamedata\ is its common content: mounted the second time as the set
        if (files.compare(start, end - start, "..") == 0 || files.compare(start, end - start, ".") == 0)
            return false;
        if (end == start && end != files.size() - 1)
            return false; // an empty component that is not the trailing separator
        start = end + 1;
    }
    return true;
}

// [slot.<slot>] of a working addon; false - it is not a provider (the reason in the log)
bool AddProvider(const addons::AddonInfo& addon, u32 order, const addons::AddonInfo::SlotSection& section)
{
    if (section.slot == kWorldSlot)
    {
        Msg("~ [addon:%s] [slot.%s]: the location packs (type = location, [location]) provide it, ignored",
            addon.id.c_str(), kWorldSlot);
        return false;
    }
    SlotInfo* slot = FindSlotMutable(section.slot);
    if (!slot)
    {
        Msg("~ [addon:%s] [slot.%s]: no such slot (console slot_list), ignored", addon.id.c_str(),
            section.slot.c_str());
        return false;
    }

    ProviderInfo provider;
    provider.id = addon.id;
    provider.version = addon.version;
    provider.order = order;
    provider.private_masks = addon.private_masks;
    const pcstr name = SlotSectionValue(section, "name");
    provider.name = name && name[0] ? xr_string(name) : addon.id;
    if (const pcstr description = SlotSectionValue(section, "description"))
        provider.description = description;
    if (const pcstr preview = SlotSectionValue(section, "preview"))
        provider.preview = preview;
    if (const pcstr entry = SlotSectionValue(section, "entry"); entry && entry[0])
    {
        if (SlotEntryValid(entry))
            provider.entry = entry;
        else
            Msg("~ [addon:%s] [slot.%s] entry = %s: not a Lua module name (A-Z a-z 0-9 _), ignored",
                addon.id.c_str(), slot->id.c_str(), entry);
    }
    // The panel of the appearance dialog: a module of the common content (gamedata\scripts\ of the addon), Lua loads it
    // when the dialog shows the provider (ui_mm_appearance.script)
    if (const pcstr panel = SlotSectionValue(section, "panel"); panel && panel[0])
    {
        if (SlotEntryValid(panel))
            provider.panel = panel;
        else
            Msg("~ [addon:%s] [slot.%s] panel = %s: not a Lua module name (A-Z a-z 0-9 _), ignored",
                addon.id.c_str(), slot->id.c_str(), panel);
    }

    const pcstr files_value = SlotSectionValue(section, "files");
    xr_string files = files_value && files_value[0] ? xr_string(files_value) :
        xr_string(kSlotDefaultFiles) + DELIMITER + slot->id + DELIMITER;
    if (!SlotFilesValid(files))
    {
        Msg("! [addon:%s] [slot.%s] files = %s: a folder inside the addon is expected, the provider is ignored",
            addon.id.c_str(), slot->id.c_str(), files.c_str());
        return false;
    }
    for (char& c : files)
    {
        if (c == '\\' || c == '/')
            c = static_cast<char>(std::filesystem::path::preferred_separator);
    }
    if (files.back() != '\\' && files.back() != '/')
        files += static_cast<char>(std::filesystem::path::preferred_separator);
    const char separator = static_cast<char>(std::filesystem::path::preferred_separator);
    const xr_string folder = addon.dir + files + "gamedata" + separator;
    std::error_code error;
    if (std::filesystem::is_directory(SlotUtf8Path(folder), error))
    {
        provider.folder = folder;
        WarnUnsupportedFiles(addon.id, folder);
    }
    else
        Msg("~ [addon:%s] [slot.%s]: no folder %sgamedata, the provider brings no files", addon.id.c_str(),
            slot->id.c_str(), files.c_str());

    slot->providers.push_back(std::move(provider));
    return true;
}

bool AddonHasContent(const addons::AddonInfo& addon)
{
    const std::filesystem::path folder = SlotUtf8Path(addon.dir) / "gamedata";
    std::error_code error;
    return std::filesystem::is_directory(folder, error);
}

// The provider asked for a slot at the start: -slot, then appdata\addons.ltx [slots], else core
const ProviderInfo* ChosenProvider(const SlotInfo& slot)
{
    xr_string chosen;
    pcstr from = "-slot";
    if (!CommandLineChoice(slot.id.c_str(), chosen))
    {
        from = "appdata/addons.ltx [slots]";
        for (const auto& [id, provider] : addons::SlotChoices())
        {
            if (id == slot.id)
                chosen = provider;
        }
    }
    if (chosen.empty())
        return &slot.providers.front();
    const ProviderInfo* provider = slot.find(chosen.c_str());
    if (!provider || SlotAddonFailed(provider->id))
    {
        Msg("~ [slots] %s: %s chose '%s', it is not installed or does not work - %s", slot.id.c_str(), from,
            chosen.c_str(), kCoreProvider);
        return &slot.providers.front();
    }
    return provider;
}

void QueueProvider(const xr_string& slot, const xr_string& provider)
{
    for (auto& [id, value] : g_slot_request.providers)
    {
        if (id == slot)
        {
            value = provider;
            return;
        }
    }
    g_slot_request.providers.emplace_back(slot, provider);
}

void QueueAction(pcstr action)
{
    if (action && action[0])
        g_slot_request.action = action;
}

// The addons whose on-demand plugins are to be loaded with this content: the pack, the chosen providers. An addon
// whose required plugin failed in this run is not asked for again (LoadOnDemandPlugins would report it once more)
xr_vector<xr_string> ActiveAddons(const LevelPackInfo& world, const xr_vector<size_t>& target)
{
    xr_vector<xr_string> ids;
    for (const xr_string& id : g_slot_plain_plugins)
    {
        if (!SlotAddonFailed(id))
            ids.push_back(id);
    }
    if (!world.builtin() && !SlotAddonFailed(world.id))
        ids.push_back(world.id);
    for (size_t i = 0; i < g_slot_list.size(); ++i)
    {
        const ProviderInfo& provider = g_slot_list[i].providers[target[i]];
        if (g_slot_list[i].id == kWorldSlot || provider.builtin || SlotAddonFailed(provider.id))
            continue;
        if (std::find(ids.begin(), ids.end(), provider.id) == ids.end())
            ids.push_back(provider.id);
    }
    return ids;
}

xr_string SlotLayerId(const SlotInfo& slot, const ProviderInfo& provider)
{
    return kSlotLayerPrefix + slot.id + ":" + provider.id;
}

// 13-extension-model.md 4.5: the base -> the common content of the addons -> the sets of the providers -> the pack
void BuildContentLayers(const LevelPackInfo& world, const xr_vector<size_t>& target,
    xr_vector<addons::ContentLayer>& out)
{
    // The common content of an addon whose required plugin did not load goes too: the addon does not work
    out.clear();
    for (const addons::ContentLayer& layer : g_slot_base_content)
    {
        if (!SlotAddonFailed(layer.owner))
            out.push_back(layer);
    }
    xr_vector<size_t> sets;
    for (size_t i = 0; i < g_slot_list.size(); ++i)
    {
        const ProviderInfo& provider = g_slot_list[i].providers[target[i]];
        if (g_slot_list[i].id != kWorldSlot && !provider.builtin && !provider.folder.empty())
            sets.push_back(i);
    }
    // In the order of their addons: the set of the addon lower in the order wins, as the common content does
    std::stable_sort(sets.begin(), sets.end(), [&target](size_t a, size_t b) {
        return g_slot_list[a].providers[target[a]].order < g_slot_list[b].providers[target[b]].order;
    });
    for (const size_t i : sets)
    {
        const SlotInfo& slot = g_slot_list[i];
        const ProviderInfo& provider = slot.providers[target[i]];
        addons::ContentLayer& layer = out.emplace_back();
        layer.layer_id = SlotLayerId(slot, provider);
        layer.owner = provider.id;
        layer.folder = provider.folder;
        layer.private_masks = provider.private_masks;
    }
    LevelPackLayers(world, out);
}

// Step 3 of 10-location-packs.md 3: Lua runs no code until its restart (it may keep handles of the old configs). With
// the configs - the data they list (init_game_globals)
void ReloadContentConfigs()
{
    reload_system_ini(); // plugins: gw::addons::ini::OnConfigsReloaded
    string_path file_name;
    FS.update_path(file_name, "$game_config$", "game.ltx");
    xr_delete(pGameIni);
    pGameIni = xr_new<CInifile>(file_name);
    // The data of the faction editor (a pack gives its own, its writable layer keeps the edits): before
    // reload_game_globals - the specific characters read their sounds and visuals from it
    reload_faction_editor_config();
    story_ids.clear(); // generated again from game.ltx when the new Lua state registers them
    spawn_story_ids.clear();
    // The data of init_game_globals (00-roadmap.md 5a, decision 35): the info portions, the encyclopedia, the dialogs,
    // the profiles, the communities with creatures\relations, the ranks, the reputations of the new layers. After
    // system.ltx (the lists of their files, the communities), before the on-demand plugins and the Lua of the menu that
    // read them. No game object is there to hold their data: the prefetched ones go at the end of a game
    // (IGame_Persistent::OnGameEnd), none are made before a level; reload_game_globals checks it
    reload_game_globals();
}

void LogProviderChanges(const LevelPackInfo& world, const xr_vector<size_t>& target, bool startup)
{
    Msg("* [slots] applying the content%s: world '%s' -> '%s'", startup ? " of the start" : "",
        ActiveLevelPack().id.c_str(), world.id.c_str());
    for (size_t i = 0; i < g_slot_list.size(); ++i)
    {
        const SlotInfo& slot = g_slot_list[i];
        if (slot.id != kWorldSlot && target[i] != slot.active)
            Msg("* [slots]   %s: '%s' -> '%s'", slot.id.c_str(), slot.active_provider().id.c_str(),
                slot.providers[target[i]].id.c_str());
    }
}

// The procedure (10-location-packs.md section 3, generalized: 13-extension-model.md 4.5). The main menu is shown, no
// level and no ALife, the start of a frame: no Lua, no plugin and no deferred event of the engine on the stack. Or the
// end of CGamePersistent::OnAppStart before a level load of the command line (ApplyStartupNow): the same state, the
// menu is not shown yet (RestartScripts does not rebuild it), no reader of the file system runs (init_game_globals is
// waited for, the map of smart terrains is not started while the content of the start waits).
void ApplyContentSet(const LevelPackInfo* world, xr_vector<size_t> target, bool startup)
{
    CTimer timer;
    timer.Start();
    LogProviderChanges(*world, target, startup);

    // 1. Nothing reads the index of the file system on other threads while the layers change (a reader keeps a
    // pointer to an entry). The map of smart terrains loads all.spawn of the old pack - it goes.
    CSpawnSmartTerrainMapDataSource::ForgetSharedLoading();
    GEnv.Render->AssetsSwitchBegin();
    if (GEnv.Sound)
    {
        GEnv.Sound->stop_emitters();
        GEnv.Sound->prefetch_stop();
    }

    const LevelPackInfo& builtin = LevelPacks().front();
    const LevelPackInfo* const requested_world = world; // a fallback below is for this run: g_level_pack keeps it
    for (;;)
    {
        // What cannot work in this run (its required plugin failed - earlier or in the round before) is not mounted
        if (!world->builtin() && SlotAddonFailed(world->id))
        {
            Msg("! [level_pack] '%s': its required plugin did not load in this run, the built-in pack '%s' is active",
                world->id.c_str(), builtin.id.c_str());
            world = &builtin;
        }
        for (size_t i = 0; i < g_slot_list.size(); ++i)
        {
            const xr_string& provider = g_slot_list[i].providers[target[i]].id;
            if (g_slot_list[i].id == kWorldSlot || target[i] == 0 || !SlotAddonFailed(provider))
                continue;
            Msg("! [slots] %s: the required plugin of '%s' did not load in this run, %s is active",
                g_slot_list[i].id.c_str(), provider.c_str(), kCoreProvider);
            // The fallback is for this run: the cause may pass (a DLL locked, plugin_sha256 during an update), the
            // choice of the player in appdata/addons.ltx [slots] stays - the next start tries it again
            Msg("~ [slots] %s: the choice '%s' is kept for the next start", g_slot_list[i].id.c_str(),
                provider.c_str());
            target[i] = 0;
        }

        // 2. The on-demand plugins of what goes off leave before its files do (00-roadmap.md 5a, decision 1)
        const xr_vector<xr_string> plugins = ActiveAddons(*world, target);
        addons::UnloadOnDemandPlugins(plugins);

        // 3. The layers
        xr_vector<addons::ContentLayer> desired;
        BuildContentLayers(*world, target, desired);
        xr_vector<xr_string> failed_groups;
        addons::SyncContentLayers(desired, failed_groups);
        bool fallback = false;
        for (const xr_string& group : failed_groups)
        {
            if (group == kSlotWorldGroup && !world->builtin())
            {
                Msg("! [level_pack] '%s' cannot be mounted, the built-in pack '%s' is active", world->id.c_str(),
                    builtin.id.c_str());
                world = &builtin;
                fallback = true;
                continue;
            }
            for (size_t i = 0; i < g_slot_list.size(); ++i)
            {
                const SlotInfo& slot = g_slot_list[i];
                if (slot.id == kWorldSlot || target[i] == 0 || group != SlotLayerId(slot, slot.providers[target[i]]))
                    continue;
                Msg("! [slots] %s: the set of '%s' cannot be mounted, %s is active", slot.id.c_str(),
                    slot.providers[target[i]].id.c_str(), kCoreProvider);
                // For this run only (as above): the choice in appdata/addons.ltx [slots] stays
                Msg("~ [slots] %s: the choice '%s' is kept for the next start", slot.id.c_str(),
                    slot.providers[target[i]].id.c_str());
                target[i] = 0;
                fallback = true;
            }
            // addon:<id> - the common content of one addon is missing, the rest goes on (the log has the reason)
        }
        if (fallback)
            continue; // the stack without them

        // 4. The configs, then the plugins that read them
        ReloadContentConfigs();
        xr_vector<xr_string> failed;
        addons::LoadOnDemandPlugins(plugins, failed);
        bool failed_now = false;
        for (const xr_string& id : failed)
        {
            if (SlotAddonFailed(id))
                continue;
            g_slot_failed_addons.push_back(id);
            failed_now = true;
        }
        if (!failed_now)
            break;
        // The next round: the pack and the providers of the failed addons go back to the base (the top of the loop),
        // their layers go, the configs are read again. The list of the failed addons only grows: the loop ends
    }

    // 5. The strings (ReloadLanguage keeps them while the language is the same)
    StringTable().Destroy();
    StringTable().Init();

    // 6. What keeps files by name: the prefetched objects (script classes among them - before the Lua restart),
    // models, motions, texture descriptions, sound sources
    g_pGamePersistent->ObjectPool.clear();
    GEnv.Render->AssetsSwitchEnd();
    if (GEnv.Sound)
        GEnv.Sound->refresh_sources();
    // The weathers: the names of the cycles and effects come from the layers (a cycle of an addon is known only now),
    // the cycles read by a level before go - a provider of world.weather may have changed them (no level here)
    if (g_pGamePersistent->pEnvironment)
        g_pGamePersistent->Environment().OnContentChanged();
    // The ids of the ui textures (ui\textures_descr\ of the layers: the spots and the maps of a pack, the icons of an
    // addon) and the color defs: read with the UI at the start, before any layer, as ui_restart reads them. A window
    // copies what it takes from there; the menu is built again below
    if (GEnv.UI)
        UI().OnUIReset();
    // The loading screen: its layout (ui_mm_loading_screen.xml) and textures from the new layers, after the ui
    // textures. Never shown here: no level, no precache (OnFrame, ApplyStartupNow)
    GamePersistent().RecreateLoadingScreen();

    // 7. The levels of the pack. The start position chosen in the faction select of the previous content belongs to
    // its game graph: it goes (the faction select of the new menu sets it again)
    g_pGamePersistent->Level_Scan();
    g_start_position_smart = nullptr;
    g_start_game_vertex_id = u16(-1);
    g_start_level_vertex_id = u32(-1);
    g_start_position.set(0.f, 0.f, 0.f);
    ++g_content_generation; // the caches of the engine that compare it read again (the new menu among them)

    // 8. The state, then the scripts and the menu with the new content
    SetActiveLevelPack(*world);
    if (world != requested_world)
    {
        // The pack did not mount or its plugin failed in this run: the cause may pass (a DLL locked, appdata not
        // writable for its layer). The choice stays the one of the player (cfg_save below writes g_level_pack)
        xr_strcpy(g_level_pack, requested_world->id.c_str());
        Msg("~ [level_pack] the choice '%s' is kept for the next start (g_level_pack), '%s' is active in this run",
            requested_world->id.c_str(), world->id.c_str());
    }
    WarnMissingPackFiles(*world);
    for (size_t i = 0; i < g_slot_list.size(); ++i)
    {
        SlotInfo& slot = g_slot_list[i];
        if (slot.id == kWorldSlot)
        {
            const ProviderInfo* pack = slot.find(ActiveLevelPack().id.c_str());
            slot.active = pack ? size_t(pack - slot.providers.data()) : 0;
        }
        else
            slot.active = target[i];
    }
    Console->Execute("cfg_save"); // g_level_pack
    addons::LogContentConflicts();
    // The visuals of the old menu go back into the pool when it is destroyed (after AssetsSwitchEnd): the new menu
    // must not take a model of the same name from there
    MainMenu()->RestartScripts([] { GEnv.Render->models_Clear(true); });

    CSpawnSmartTerrainMapDataSource::StartSharedLoadingWhenIdle("all");
    Msg("* [slots] the content is applied, %.2f sec", timer.GetElapsed_sec());
    // For the launcher: the menu has read its configs and ui by now (the patches of them are recorded)
    addons::WriteAddonsReport(startup ? "startup" : "content");
}

// The event addons_content_applied (when, ok, world, providers): the content set is applied - the configs, the strings,
// the levels and the Lua state are new. A plugin loaded before (at the start, or an on-demand one that stays) reads its
// data of the content again here (dialogs, communities ...); an on-demand plugin loaded by this procedure gets it too.
// when: "startup" (the first mount) | "content" (a switch in the main menu); ok: the request is done as asked (false:
// a fallback - a pack or a provider did not mount or its required plugin failed; the log has the reason); world: the
// active location pack; providers: "<slot>=<provider>" of every slot but world, in the order of the slots
void EmitContentApplied(bool startup, bool ok)
{
    namespace events = addons::events;
    if (!events::HasSubscribers(events::EBuiltin::AddonsContentApplied))
        return;
    xr_vector<xr_string> texts;
    texts.reserve(g_slot_list.size());
    for (const SlotInfo& slot : g_slot_list)
    {
        if (slot.id != kWorldSlot)
            texts.push_back(slot.id + "=" + slot.active_provider().id);
    }
    xr_vector<GwpValue> items;
    items.reserve(texts.size());
    for (const xr_string& text : texts)
        items.push_back(events::String(text.c_str())); // `texts` outlives the emit
    GwpValue providers = events::Nil();
    providers.type = GWP_T_ARRAY;
    providers.u.a.items = items.empty() ? nullptr : items.data();
    providers.u.a.count = static_cast<uint32_t>(items.size());
    const xr_string world = ActiveLevelPack().id;
    const GwpValue args[] = { events::String(startup ? "startup" : "content"), events::Bool(ok),
        events::String(world.c_str()), providers };
    events::Emit(events::EBuiltin::AddonsContentApplied, args, static_cast<u32>(std::size(args)));
}

void ApplyRequest(const SlotRequest& request)
{
    const LevelPackInfo* world = request.world_set ? FindLevelPack(request.world.c_str()) : &ActiveLevelPack();
    if (!world)
        world = &ActiveLevelPack();

    xr_vector<size_t> target(g_slot_list.size());
    for (size_t i = 0; i < g_slot_list.size(); ++i)
        target[i] = g_slot_list[i].active;
    for (const auto& [slot_id, provider_id] : request.providers)
    {
        const size_t index = SlotIndex(slot_id);
        const ProviderInfo* provider = index != size_t(-1) ? g_slot_list[index].find(provider_id.c_str()) : nullptr;
        if (!provider || g_slot_list[index].id == kWorldSlot)
            continue;
        if (g_slot_list[index].restart && !request.startup)
            continue; // the choice is kept: the next start
        target[index] = size_t(provider - g_slot_list[index].providers.data());
    }

    bool changes = request.startup || request.rules || world != &ActiveLevelPack();
    for (size_t i = 0; i < g_slot_list.size() && !changes; ++i)
        changes = target[i] != g_slot_list[i].active;
    if (changes)
        ApplyContentSet(world, std::move(target), request.startup);

    // The callbacks in the Lua state of the new content
    const bool ok_world = !request.world_set || ActiveLevelPack().id == request.world;
    bool ok_slots = true;
    for (const auto& [slot_id, provider_id] : request.providers)
    {
        const SlotInfo* slot = FindSlot(slot_id.c_str());
        ok_slots &= slot && (slot->restart || slot->active_provider().id == provider_id);
    }
    // Under lua_pcall: it runs the entries of the providers (<entry>.on_slot_active - the code of the addons)
    const pcstr slots_action = request.world_callback ? "" : request.action.c_str();
    if (addons::CallLuaProtected("gw_slots.on_slots_applied", { slots_action, ok_slots }) == addons::ELuaCall::NotFound)
        Msg("! [slots] Lua function gw_slots.on_slots_applied not found: the entries of the providers do not run");
    // The merge of the files several addons replace (addon_merge.h): its summary for the menu (the line of a warning
    // about the unresolved conflicts), after each build of the content stack
    if (changes &&
        addons::CallLuaProtected("gw_slots.on_merge_report", { &addons::PushContentMergeSummary }) ==
            addons::ELuaCall::NotFound)
        Msg("~ [slots] Lua function gw_slots.on_merge_report not found: the menu does not warn about the merge");
    // The plugins: after the entries of the providers, before the world callback (it may start a level load)
    if (changes)
        EmitContentApplied(request.startup, ok_world && ok_slots);
    if (!request.world_callback)
        return;
    const xr_string pack_id = request.world_set ? request.world : ActiveLevelPack().id;
    luabind::functor<void> on_applied;
    if (GEnv.ScriptEngine && GEnv.ScriptEngine->functor("ui_main_menu.on_level_pack_applied", on_applied))
        on_applied(pack_id.c_str(), request.action.c_str(), ok_world && ok_slots);
    else if (!request.action.empty())
        Msg("! [level_pack] Lua function ui_main_menu.on_level_pack_applied not found, '%s' is not done",
            request.action.c_str());
}
} // namespace

void Init()
{
    if (g_slot_initialized)
        return;
    g_slot_initialized = true;
    g_slot_list.clear();
    g_slot_base_content.clear();
    g_slot_plain_plugins.clear();

    ReadBaseSlots();
    if (!FindSlotMutable(kWorldSlot))
    {
        SlotInfo world = MakeSlot(kWorldSlot, kCoreProvider);
        world.name = kSlotWorldName;
        g_slot_list.insert(g_slot_list.begin(), std::move(world));
    }
    // world: the location packs are its providers, the built-in one is the base
    {
        SlotInfo& world = *FindSlotMutable(kWorldSlot);
        world.save_bound = true;
        world.restart = false;
        world.providers.clear();
        for (const LevelPackInfo& pack : LevelPacks())
        {
            if (!pack.builtin())
                WarnUnsupportedFiles(pack.id, pack.dir + "gamedata" + DELIMITER);
            ProviderInfo& provider = world.providers.emplace_back();
            provider.id = pack.id;
            provider.name = pack.name;
            provider.description = pack.description;
            provider.preview = pack.preview;
            provider.version = pack.version;
            provider.builtin = pack.builtin();
        }
        const ProviderInfo* active = world.find(ActiveLevelPack().id.c_str());
        world.active = active ? size_t(active - world.providers.data()) : 0;
    }

    const xr_vector<addons::AddonInfo> found = addons::ListAddons();
    for (const addons::AddonInfo& addon : found)
    {
        if (!addon.active)
            continue;
        for (const auto& [key, value] : addon.slots_declare)
            DeclareAddonSlot(addon, key, value);
    }
    u32 providers = 0;
    for (u32 order = 0; order < found.size(); ++order)
    {
        const addons::AddonInfo& addon = found[order];
        if (!addon.active)
        {
            if (!addon.slots.empty())
                Msg("~ [slots] addon '%s' does not work (addon_list), its slot providers are left out",
                    addon.id.c_str());
            continue;
        }
        u32 accepted = 0;
        for (const addons::AddonInfo::SlotSection& section : addon.slots)
            accepted += AddProvider(addon, order, section) ? 1 : 0;
        providers += accepted;
        // 13-extension-model.md 11 (00-roadmap.md 5a, decision 37): an addon of type location that did not become a
        // pack (InitLevelPacks: no gamedata\spawns\all.spawn) is mounted as the common content of an addon, its
        // plugin loads as that of an addon without slots (the host keeps it on demand for the pack it is not)
        const bool location = 0 == xr_stricmp(addon.type.c_str(), "location");
        const LevelPackInfo* pack = location ? FindLevelPack(addon.id.c_str()) : nullptr;
        const bool is_pack = pack && !pack->builtin() && pack->dir == addon.dir;
        // The id of the built-in pack: InitLevelPacks left the addon out - its files (all.spawn among them) must not
        // lie over the base as common content
        const bool left_out = pack && !is_pack;
        if ((!addon.slots.empty() || location) && !accepted && !is_pack && !left_out)
            g_slot_plain_plugins.push_back(addon.id);
        // The common content: every working addon but a location pack (that one is the world, the top of the stack)
        const bool has_content = AddonHasContent(addon);
        // A provider set of its own (slots\<slot>\gamedata\) is a mounted layer of the addon too: its Lua part
        // runs while that provider is active (ActiveLuaEntries)
        bool has_set = false;
        for (const SlotInfo& slot : g_slot_list)
        {
            for (const ProviderInfo& provider : slot.providers)
                has_set = has_set || (provider.id == addon.id && !provider.folder.empty());
        }
        if (!has_content && !has_set && !addon.lua_entry.empty())
            Msg("! [addon:%s] [lua] entry = %s: the addon has no gamedata folder, its Lua part never runs",
                addon.id.c_str(), addon.lua_entry.c_str());
        if (is_pack || left_out || !has_content)
            continue;
        addons::ContentLayer& layer = g_slot_base_content.emplace_back();
        layer.layer_id = kSlotAddonLayerPrefix + addon.id;
        layer.owner = addon.id;
        layer.folder = addon.dir + "gamedata" + static_cast<char>(std::filesystem::path::preferred_separator);
        layer.private_masks = addon.private_masks;
        WarnUnsupportedFiles(addon.id, layer.folder);
    }

    // A choice of world in [slots]: the pack is chosen by g_level_pack / -level_pack (10-location-packs.md 2.3)
    for (const auto& [slot, provider] : addons::SlotChoices())
    {
        if (slot == kWorldSlot)
            Msg("~ [slots] appdata/addons.ltx [slots] world = %s: the location pack is chosen by g_level_pack, "
                "-level_pack or -slot world=<id>, ignored", provider.c_str());
        else if (!FindSlotMutable(slot))
            Msg("~ [slots] appdata/addons.ltx [slots] %s: no such slot, ignored", slot.c_str());
    }
    Msg("* [slots] %u slot(s), %u provider(s) from addons, %u addon(s) with content (mounted at the first frame of "
        "the main menu or before a level load of the command line)", u32(g_slot_list.size()), providers,
        u32(g_slot_base_content.size()));
    // The addons and their states before any content is mounted: the report stands when there is nothing to mount
    addons::WriteAddonsReport("init");

    // The plugins of the addons that turned out to provide nothing (no slot, no pack): loaded now and kept, as any
    // other plugin
    if (!g_slot_plain_plugins.empty())
    {
        for (const xr_string& id : g_slot_plain_plugins)
        {
            const addons::AddonInfo* addon = nullptr;
            for (const addons::AddonInfo& candidate : found)
            {
                if (candidate.id == id)
                    addon = &candidate;
            }
            if (addon && !addon->plugin.empty())
                Msg("~ [addon:%s] it provides no slot and no location pack: its plugin loads now, as of an addon "
                    "without slots", id.c_str());
        }
        xr_vector<xr_string> failed;
        addons::LoadOnDemandPlugins(g_slot_plain_plugins, failed);
        for (const xr_string& id : failed)
        {
            if (!SlotAddonFailed(id))
                g_slot_failed_addons.push_back(id);
        }
    }
}

bool AddonFailed(pcstr addon_id) { return addon_id && SlotAddonFailed(addon_id); }

const xr_vector<SlotInfo>& Slots() { return g_slot_list; }

const SlotInfo* FindSlot(pcstr slot)
{
    if (!slot)
        return nullptr;
    const xr_string id = SlotLower(slot);
    for (const SlotInfo& candidate : g_slot_list)
    {
        if (candidate.id == id)
            return &candidate;
    }
    return nullptr;
}

bool CommandLineChoice(pcstr slot, xr_string& provider)
{
    if (!slot || !slot[0])
        return false;
    const xr_string wanted = SlotLower(slot);
    bool found = false;
    constexpr pcstr key = "-slot ";
    for (pcstr p = strstr(Core.Params, key); p; p = strstr(p, key))
    {
        p += xr_strlen(key);
        string256 token{};
        sscanf(p, "%255s", token);
        const xr_string text = SlotLower(token);
        const size_t eq = text.find('=');
        if (eq == xr_string::npos || text.substr(0, eq) != wanted)
            continue;
        provider = text.substr(eq + 1);
        found = !provider.empty();
    }
    return found;
}

bool RequestSelect(pcstr slot_id, pcstr provider_id, pcstr action)
{
    const SlotInfo* slot = FindSlot(slot_id);
    if (!slot)
    {
        Msg("! [slots] select: no slot '%s' (console slot_list)", slot_id ? slot_id : "");
        return false;
    }
    if (slot->id == kWorldSlot)
    {
        // As level_pack.switch: core is the built-in pack
        const bool core = provider_id && 0 == xr_stricmp(provider_id, kCoreProvider);
        return RequestLevelPackSwitch(core ? LevelPacks().front().id.c_str() : provider_id, action);
    }
    const ProviderInfo* provider = slot->find(provider_id);
    if (!provider)
    {
        Msg("! [slots] select: slot %s has no provider '%s' (console slot_list)", slot->id.c_str(),
            provider_id ? provider_id : "");
        return false;
    }
    if (SlotAddonFailed(provider->id))
    {
        Msg("! [slots] select %s = %s: its required plugin did not load in this run", slot->id.c_str(),
            provider->id.c_str());
        return false;
    }
    if (g_pGameLevel)
    {
        Msg("! [slots] select %s = %s: only in the main menu, without a game", slot->id.c_str(), provider->id.c_str());
        return false;
    }
    // The choice of the next start (the default of a new game for a slot bound to the save); the launcher sees it
    const bool saved = addons::WriteSlotChoice(slot->id.c_str(), provider->id.c_str());
    if (!saved)
        Msg("! [slots] select %s = %s: appdata/addons.ltx is not written (see above): the next start keeps its choice",
            slot->id.c_str(), provider->id.c_str());
    if (slot->restart)
    {
        if (!saved)
            return false;
        Msg("- [slots] %s = %s: takes effect at the next start of the game", slot->id.c_str(), provider->id.c_str());
        return true;
    }
    QueueProvider(slot->id, provider->id);
    QueueAction(action);
    g_slot_request.pending = true;
    return true;
}

bool RequestRebuild(pcstr action)
{
    if (!g_slot_initialized)
        return false;
    if (g_pGameLevel)
    {
        Msg("- [addons] the rules of the conflicts take effect at the next build of the content (a switch in the main "
            "menu, the next start of the game)");
        return false;
    }
    g_slot_request.rules = true;
    QueueAction(action);
    g_slot_request.pending = true;
    return true;
}

bool RequestWorld(pcstr pack_id, pcstr action)
{
    if (!g_slot_initialized || !pack_id)
        return false;
    if (SlotAddonFailed(pack_id))
    {
        Msg("! [level_pack] switch to '%s': its required plugin did not load in this run", pack_id);
        return false;
    }
    g_slot_request.world_set = true;
    g_slot_request.world = pack_id;
    g_slot_request.world_callback = true;
    QueueAction(action);
    g_slot_request.pending = true;
    return true;
}

void RequestForSave(pcstr pack_id, const xr_vector<std::pair<xr_string, xr_string>>& providers, pcstr action)
{
    if (!g_slot_initialized)
        return;
    if (pack_id)
    {
        g_slot_request.world_set = true;
        g_slot_request.world = pack_id;
    }
    for (const auto& [slot, provider] : providers)
        QueueProvider(slot, provider);
    g_slot_request.world_callback = true; // ui_main_menu.on_level_pack_applied loads the save
    QueueAction(action);
    g_slot_request.pending = true;
}

void RequestStartup(pcstr pack_id)
{
    if (!g_slot_initialized)
        return;
    g_slot_request.startup = true;
    if (pack_id && pack_id[0])
    {
        g_slot_request.world_set = true;
        g_slot_request.world = pack_id;
        g_slot_request.world_callback = true;
    }
    for (const SlotInfo& slot : g_slot_list)
    {
        if (slot.id == kWorldSlot)
            continue;
        const ProviderInfo* provider = ChosenProvider(slot);
        if (!provider->builtin)
            QueueProvider(slot.id, provider->id);
    }
    if (g_slot_request.world_set || !g_slot_request.providers.empty() || !g_slot_base_content.empty())
        g_slot_request.pending = true;
    else
        g_slot_request = {}; // nothing to mount: the game goes on on the base, as without addons
}

void RequestStartupForSave(pcstr pack_id, const xr_vector<std::pair<xr_string, xr_string>>& providers)
{
    if (!g_slot_initialized || !pack_id)
        return;
    if (!g_slot_request.pending)
    {
        // Nothing of the start to mount: the save asks for something only when it is not the base
        bool changes = FindLevelPack(pack_id) != &ActiveLevelPack();
        for (const auto& [slot, provider] : providers)
            changes |= 0 != xr_stricmp(provider.c_str(), kCoreProvider);
        if (!changes)
            return;
    }
    g_slot_request.startup = true;
    g_slot_request.world_set = true; // the built-in pack too: not the one of -level_pack / g_level_pack
    g_slot_request.world = pack_id;
    for (const auto& [slot, provider] : providers)
        QueueProvider(slot, provider);
    g_slot_request.pending = true;
}

void ApplyStartupNow()
{
    if (!g_slot_request.pending || !g_slot_request.startup)
        return;
    SlotRequest request = g_slot_request;
    g_slot_request = {};
    request.world_callback = false; // no menu to go on from: the level load is next
    ApplyRequest(request);
}

bool ApplyPending() { return g_slot_request.pending; }
u32 ContentGeneration() { return g_content_generation; }

void OnFrame()
{
    if (!g_slot_request.pending)
        return;
    if (g_pGameLevel || Device.dwPrecacheFrame || !MainMenu() || !MainMenu()->IsActive())
        return; // later: a level is loading or unloading, the menu is not shown yet
    const SlotRequest request = g_slot_request;
    g_slot_request = {};
    ApplyRequest(request);
}

xr_string NextStartProvider(const SlotInfo& slot)
{
    if (slot.id == kWorldSlot)
        return g_level_pack[0] ? xr_string(g_level_pack) : LevelPacks().front().id;
    xr_string chosen = kCoreProvider;
    for (const auto& [id, provider] : addons::SlotChoices())
    {
        if (id == slot.id)
            chosen = provider;
    }
    return chosen;
}

void PrintSlots()
{
    if (!g_slot_initialized)
    {
        Msg("- [slots] not initialized (no addon host)");
        return;
    }
    Msg("- [slots] %u slot(s); * - active, > - chosen for the next start%s:", u32(g_slot_list.size()),
        g_slot_request.pending ? ", a switch is waiting for the next frame of the menu" : "");
    for (const SlotInfo& slot : g_slot_list)
    {
        // The choice of the next start (a restart slot shows it apart from the active provider)
        const xr_string chosen = NextStartProvider(slot);
        xr_string line;
        for (size_t i = 0; i < slot.providers.size(); ++i)
        {
            const ProviderInfo& provider = slot.providers[i];
            const bool is_chosen = 0 == xr_stricmp(chosen.c_str(), provider.id.c_str());
            line += (line.empty() ? "" : ", ") + xr_string(i == slot.active ? "*" : "") +
                (is_chosen && i != slot.active ? ">" : "") + provider.id;
        }
        Msg("-   %-24s %s%s%s: %s", slot.id.c_str(), slot.owner == kCoreProvider ? "" : "(addon ",
            slot.owner == kCoreProvider ? "" : (slot.owner + ") ").c_str(),
            slot.save_bound ? "save-bound" : slot.restart ? "after a restart" : "in the menu", line.c_str());
    }
}
} // namespace gw::slots
