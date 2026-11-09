#include "StdAfx.h"

#include "level_pack.h"
#include "addon_host.h"
#include "addon_content.h"
#include "addon_slots.h"
#include "addon_merge.h"
#include "addon_lua_entry.h"
#include "saved_game_wrapper.h"
#include "MainMenu.h"

#include "xrEngine/Engine.h"
#include "xrEngine/IGame_Level.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrScriptEngine/script_engine.hpp"

#include <filesystem>

string64 gw::g_level_pack = "";

namespace gw
{
namespace
{
constexpr pcstr kLevelPackConfig = "misc\\level_pack\\level_pack.ltx";
constexpr pcstr kLevelPackDefaultId = "gw_base";
constexpr pcstr kLevelPackType = "location"; // [addon] type of a location pack
constexpr pcstr kNamePrefix = "st_level_pack_"; // the default name of a pack: st_level_pack_<id>

constexpr pcstr kUserLayerFolder = "level_packs"; // $app_data_root$level_packs\<id>\gamedata\: the writes of a pack

xr_vector<LevelPackInfo> g_packs;
bool g_packs_initialized = false;
size_t g_active = 0; // index in g_packs: at the start of the game the built-in pack (nothing is mounted)

bool ParseSaveCompat(pcstr text, u32& out)
{
    if (!text || text[0] < '0' || text[0] > '9') // strtoull takes "-1" too
        return false;
    char* end = nullptr;
    const unsigned long long value = strtoull(text, &end, 10);
    if (*end || value == 0 || value > type_max<u32>)
        return false;
    out = static_cast<u32>(value);
    return true;
}

// The passport of the built-in pack. The file is not a part of system.ltx; it is read before any pack is mounted
// (a pack brings its own misc\level_pack\, that one is not the passport of gamedata).
LevelPackInfo ReadBuiltinPack()
{
    LevelPackInfo pack;
    pack.id = kLevelPackDefaultId;
    pack.version = "0.0.0";

    string_path file_name;
    FS.update_path(file_name, "$game_config$", kLevelPackConfig);
    if (!FS.exist(file_name))
        Msg("! [level_pack] '%s' not found, the built-in location pack is taken as '%s'", file_name, pack.id.c_str());
    else
    {
        const CInifile ini(file_name, true, true, false);
        const auto read = [&ini](pcstr key) -> pcstr {
            return ini.line_exist("level_pack", key) ? ini.r_string("level_pack", key) : nullptr;
        };
        if (const pcstr id = read("id"); id && id[0])
            pack.id = id;
        else
            Msg("! [level_pack] '%s' has no [level_pack] id, the built-in location pack is taken as '%s'", file_name,
                pack.id.c_str());
        if (const pcstr version = read("version"); version && version[0])
            pack.version = version;
        if (const pcstr compat = read("save_compat"); compat && !ParseSaveCompat(compat, pack.save_compat))
            Msg("! [level_pack] '%s': save_compat '%s' is not a positive number, 1 is taken", file_name, compat);
        if (const pcstr name = read("name"); name && name[0])
            pack.name = name;
        if (const pcstr description = read("description"); description && description[0])
            pack.description = description;
        if (const pcstr preview = read("preview"); preview && preview[0])
            pack.preview = preview;
    }
    if (pack.name.empty())
        pack.name = xr_string(kNamePrefix) + pack.id;
    return pack;
}

pcstr LocationValue(const addons::AddonInfo& addon, pcstr key)
{
    for (const auto& [name, value] : addon.location)
    {
        if (0 == xr_stricmp(name.c_str(), key))
            return value.c_str();
    }
    return nullptr;
}

// A working addon of type location: false (with the reason in the log) when it cannot be a pack
bool PackFromAddon(const addons::AddonInfo& addon, LevelPackInfo& pack)
{
    pack.id = addon.id;
    pack.version = addon.version;
    pack.dir = addon.dir;
    pack.private_masks = addon.private_masks;

    // The disk directly (UTF-8 path, the separators of the OS): the pack is not mounted yet
    const std::filesystem::path dir = addons::PathFromUtf8(addon.dir);
    const std::filesystem::path spawn = dir / "gamedata" / "spawns" / "all.spawn";
    std::error_code error;
    if (!std::filesystem::is_regular_file(spawn, error))
    {
        // 00-roadmap.md 5a, decision 37: not a world - its files still work, as those of any addon (slots::Init)
        Msg("~ [level_pack] addon '%s' (type = %s) has no gamedata\\spawns\\all.spawn, it is not a location pack: its "
            "gamedata\\ is mounted as the common content of an addon", addon.id.c_str(), kLevelPackType);
        return false;
    }

    const pcstr compat = LocationValue(addon, "save_compat");
    if (!compat)
        Msg("~ [level_pack] addon '%s': no [location] save_compat, 1 is taken", addon.id.c_str());
    else if (!ParseSaveCompat(compat, pack.save_compat))
        Msg("! [level_pack] addon '%s': [location] save_compat '%s' is not a positive number, 1 is taken",
            addon.id.c_str(), compat);
    const pcstr name = LocationValue(addon, "name");
    pack.name = name && name[0] ? xr_string(name) : xr_string(kNamePrefix) + addon.id;
    if (const pcstr description = LocationValue(addon, "description"))
        pack.description = description;
    if (const pcstr preview = LocationValue(addon, "preview"))
        pack.preview = preview;
    return true;
}

xr_string PackLayerId(const LevelPackInfo& pack) { return "level_pack:" + pack.id; }
xr_string UserLayerId(const LevelPackInfo& pack) { return "level_pack_user:" + pack.id; }
constexpr pcstr kPackLayerGroup = "world"; // the pack and its writes fail together (addons::ContentLayer::group)

// The files of the base the game writes (the faction editor, $game_config$): a pack ships its own, empty ones at least
// (wiki/doc/addons/location_packs.md "Files the game writes"), else the data of the base levels are read and a write
// goes into the base gamedata
constexpr pcstr kPackWrittenFiles[] = {
    "misc\\simulations\\default_custom.ltx", // sim_brain, the faction editor (bases), the faction select
    "misc\\simulation_objects_props_custom.ltx", // simulation_objects, the faction editor (point types)
    "creatures\\game_relations_custom.ltx", // game_relations, the faction editor (relations)
};
} // namespace

void WarnMissingPackFiles(const LevelPackInfo& pack)
{
    if (pack.builtin())
        return;
    const xr_string content_id = PackLayerId(pack), writes_id = UserLayerId(pack);
    const addons::ContentLayer* content = nullptr;
    const addons::ContentLayer* writes = nullptr;
    for (const addons::ContentLayer& layer : addons::MountedContentLayers())
    {
        if (layer.layer_id == content_id)
            content = &layer;
        else if (layer.layer_id == writes_id)
            writes = &layer;
    }
    if (!content)
        return; // not mounted (the fallback to the built-in pack is logged)

    // $game_config$ relative to $game_data$ - the root of the folders of the layers (configs\ in fsgame.ltx)
    string_path data_root, config_root;
    FS.update_path(data_root, "$game_data$", "");
    FS.update_path(config_root, "$game_config$", "");
    const size_t data_length = xr_strlen(data_root);
    if (0 != strnicmp(config_root, data_root, data_length))
        return; // $game_config$ outside the game data: the layers do not cover it
    const xr_string config_relative = config_root + data_length;

    for (const pcstr file : kPackWrittenFiles)
    {
        // The pack ships it: in its folder, or written before into the folder of its writes (a [private] mask or a
        // file: rule of the player that keeps the file of the pack out is a choice, not a missing file)
        const xr_string name = config_relative + file;
        std::error_code error;
        if (std::filesystem::is_regular_file(addons::PathFromUtf8(content->folder + name), error) ||
            (writes && std::filesystem::is_regular_file(addons::PathFromUtf8(writes->folder + name), error)))
            continue;
        IReader* base = FS.r_open_base("$game_config$", file);
        if (!base)
            continue; // the base has none either
        FS.r_close(base);

        string_path full, index_name;
        FS.update_path(full, "$game_config$", file);
        xr_strcpy(index_name, full);
        xr_fs_strlwr(index_name);
        const CLocatorAPI::file* serving = FS.GetFileDesc(index_name);
        const pcstr layer = serving && serving->layer ? FS.layer_id(serving->layer) : nullptr;
        if (layer)
            Msg("~ [level_pack] '%s' does not ship %s: the file of the layer '%s' is used (the base has one too)",
                pack.id.c_str(), name.c_str(), layer);
        else
            Msg("~ [level_pack] '%s' does not ship %s: the base file is used: %s", pack.id.c_str(), name.c_str(), full);
    }
}

// The layers of a pack: its gamedata\ (with the alias roots textures\@<id>\, meshes\@<id>\ - one layer,
// plans/addons_api_support/14-addon-resource-scoping.md), over it the folder its writes go to. The built-in pack has
// none (gamedata is the base itself, it writes where its files lie). Without the folder for the writes the pack is not
// mounted (the group): a write of a file of the pack would go into the base gamedata and the pack would go on
// serving its own old file.
void LevelPackLayers(const LevelPackInfo& pack, xr_vector<addons::ContentLayer>& out)
{
    if (pack.builtin())
        return;
    addons::ContentLayer& content = out.emplace_back();
    content.layer_id = PackLayerId(pack);
    content.owner = pack.id;
    content.folder = pack.dir + "gamedata" + DELIMITER;
    content.private_masks = pack.private_masks;
    content.group = kPackLayerGroup;

    string_path user_folder, user;
    strconcat(sizeof user_folder, user_folder, kUserLayerFolder, DELIMITER, pack.id.c_str(), DELIMITER, "gamedata",
        DELIMITER);
    FS.update_path(user, "$app_data_root$", user_folder);
    addons::ContentLayer& writes = out.emplace_back();
    writes.layer_id = UserLayerId(pack);
    writes.folder = user;
    writes.writable = true;
    writes.group = kPackLayerGroup;
}

void SetActiveLevelPack(const LevelPackInfo& pack)
{
    for (size_t i = 0; i < g_packs.size(); ++i)
    {
        if (&g_packs[i] != &pack)
            continue;
        g_active = i;
        xr_strcpy(g_level_pack, pack.id.c_str());
        return;
    }
}

void InitLevelPacks()
{
    if (g_packs_initialized)
        return;
    g_packs_initialized = true;
    g_packs.clear();
    g_packs.emplace_back(ReadBuiltinPack());
    g_active = 0;

    for (const addons::AddonInfo& addon : addons::ListAddons())
    {
        if (0 != xr_stricmp(addon.type.c_str(), kLevelPackType))
            continue;
        if (!addon.active)
        {
            Msg("~ [level_pack] addon '%s' does not work (addon_list), its location pack is left out", addon.id.c_str());
            continue;
        }
        if (FindLevelPack(addon.id.c_str()))
        {
            Msg("! [level_pack] addon '%s': a location pack with this id is there already, left out", addon.id.c_str());
            continue;
        }
        LevelPackInfo pack;
        if (PackFromAddon(addon, pack))
            g_packs.emplace_back(std::move(pack));
    }

    PrintLevelPacks();
    // The slots (13-extension-model.md): "world" takes its providers from the packs found above
    slots::Init();
}

void PrintLevelPacks()
{
    for (const LevelPackInfo& pack : g_packs)
    {
        Msg("* [level_pack] %s'%s' %s, save_compat %u%s%s", &pack == &g_packs[g_active] ? "(active) " : "",
            pack.id.c_str(), pack.version.c_str(), pack.save_compat, pack.builtin() ? ", built-in" : ", ",
            pack.builtin() ? "" : pack.dir.c_str());
    }
}

const xr_vector<LevelPackInfo>& LevelPacks()
{
    VERIFY2(g_packs_initialized, "gw::InitLevelPacks has not run yet");
    return g_packs;
}

const LevelPackInfo* FindLevelPack(pcstr id)
{
    if (!id)
        return nullptr;
    for (const LevelPackInfo& pack : g_packs)
    {
        if (pack.id == id)
            return &pack;
    }
    return nullptr;
}

const LevelPackInfo& ActiveLevelPack()
{
    if (g_packs.empty()) // before InitLevelPacks: only the built-in pack can be there (nothing is mounted)
    {
        static const LevelPackInfo builtin = [] {
            LevelPackInfo pack;
            pack.id = kLevelPackDefaultId;
            return pack;
        }();
        return builtin;
    }
    return g_packs[g_active];
}

bool IsLevelPackInstalled(pcstr id) { return FindLevelPack(id) != nullptr; }

bool RequestLevelPackSwitch(pcstr id, pcstr action)
{
    if (!FindLevelPack(id))
    {
        Msg("! [level_pack] switch: no location pack '%s'", id ? id : "");
        return false;
    }
    if (g_pGameLevel)
    {
        Msg("! [level_pack] switch to '%s': only in the main menu, without a game", id);
        return false;
    }
    return slots::RequestWorld(id, action);
}

bool LevelPackSwitchPending() { return slots::ApplyPending(); }

void RequestStartupLevelPack()
{
    if (!g_packs_initialized)
        return; // a dedicated server: no addon host, no packs, no content of the addons
    string64 id{};
    xr_string from_slot;
    if (pcstr arg = strstr(Core.Params, "-level_pack "))
        sscanf(arg + xr_strlen("-level_pack "), "%63s", id);
    else if (slots::CommandLineChoice("world", from_slot)) // -slot world=<id>: the same as -level_pack <id>
        xr_strcpy(id, from_slot.c_str());
    else
        xr_strcpy(id, g_level_pack);
    if (id[0] && !FindLevelPack(id))
    {
        Msg("! [level_pack] the location pack '%s' is not installed, the built-in '%s' stays", id,
            ActiveLevelPack().id.c_str());
        id[0] = 0;
    }
    if (id[0] && FindLevelPack(id) == &ActiveLevelPack())
        id[0] = 0; // the built-in pack (g_level_pack keeps it after a switch back): nothing to switch to
    // The content of the addons and the chosen slot providers are mounted at the first frame of the menu too, with
    // the pack (one apply)
    slots::RequestStartup(id[0] ? id : nullptr);
}

namespace
{
// The save of the start of the command line that cannot load in this run (ApplyStartupContentForLevel): the menu shows
// the reason once the content of the start is mounted there
xr_string g_refused_save;
bool g_refuse_start = false; // its KERNEL:start has not come yet (CanStartLevel)
// -addon_strict refused the start of a level (CanStartLevel): the window of the menu at its next frame; the menu was
// asked to show (a new game from the menu turns it off right after "start server")
bool g_strict_window = false;
bool g_strict_menu_asked = false;
// The save whose warning "made on another spawn with the same save_compat" is in the log already (ReadSaveContent):
// one load checks the save 2-3 times (level_pack.check_save of the menu, then the console load; -start before and after
// the mount). Cleared at the start of a level (CanStartLevel): the next load of that save warns again
xr_string g_spawn_warned_save;

// -addon_strict (15-merge-implementation.md 5.4): the window of the main menu with the unresolved conflicts of the
// merge of the addons (ui_main_menu.on_addon_strict_blocked, it reads them from addon_slots.merges)
void ShowStrictMergeWindow()
{
    if (addons::CallLuaProtected("ui_main_menu.on_addon_strict_blocked", {}) == addons::ELuaCall::NotFound)
        Msg("! [merge] Lua function ui_main_menu.on_addon_strict_blocked not found");
}

enum class ESaveContent
{
    Ok, // may load (a missing or invalid save too: the callers report that themselves)
    PackMissing, // its pack is not installed or does not work in this run
    PackIncompatible, // save_compat differs
    ProviderMissing, // the provider of its save-bound slot is not installed or does not work
};

// What a save needs of the content of this run: its pack (gw/game/level_pack*) and the providers of the slots bound to
// it (gw/slots/<slot>, 13-extension-model.md 7). No Lua, no switch: the reason of a refusal goes into the log
struct SaveContent
{
    const LevelPackInfo* pack = nullptr; // nullptr: a save without the pack key (made before it existed)
    xr_string pack_id;
    xr_vector<std::pair<xr_string, xr_string>> providers; // the save-bound slots the save names: slot -> provider
    xr_string missing_slot, missing_provider; // ESaveContent::ProviderMissing
};

// The GUID of the header of <spawn_name>.spawn under $game_spawn$ of the layers of now (CALifeSpawnHeader::load: the
// version, then the GUID at the start of chunk 0). False: no such file or no header
bool ReadSpawnGuid(pcstr spawn_name, xrGUID& guid)
{
    string_path file_name;
    if (!spawn_name || !spawn_name[0] || !FS.exist(file_name, "$game_spawn$", spawn_name, ".spawn"))
        return false;
    IReader* file = FS.r_open(file_name);
    if (!file)
        return false;
    bool ok = false;
    if (IReader* chunk = file->open_chunk(0))
    {
        if (chunk->elapsed() >= static_cast<intptr_t>(sizeof(u32) + sizeof(guid)))
        {
            chunk->r_u32(); // the version
            chunk->r(&guid, sizeof(guid));
            ok = true;
        }
        chunk->close();
    }
    FS.r_close(file);
    return ok;
}

// The save was made on another spawn than the one of the active content (the GUID of <name>.spawn differs). The
// built-in pack keeps the rule of the base (CALifeSpawnRegistry::load: a log line, the load goes on - the spawn of GW is
// rebuilt between its versions and the saves keep loading). Called when the pack of the save is the active one: a save
// of another pack is checked after the switch to it (the console load of ui_main_menu)
bool SaveSpawnDiffers(const CSavedGameWrapper::SavedSpawn& spawn)
{
    if (!spawn.valid || ActiveLevelPack().builtin())
        return false;
    xrGUID current{};
    if (!ReadSpawnGuid(spawn.name.c_str(), current))
        return false; // no such spawn: the load reports it as without the packs ("Can't find spawn file")
    return current != spawn.guid;
}

ESaveContent ReadSaveContent(pcstr save_name, SaveContent& out)
{
    // Only the data bus chunk and the name and GUID of the spawn: no actor, no graph as in the full CSavedGameWrapper
    gw::addons::data::SaveValues values;
    CSavedGameWrapper::SavedSpawn spawn;
    if (!CSavedGameWrapper::saved_game_data(save_name, values, &spawn))
        return ESaveContent::Ok; // missing or invalid: the callers report that themselves
    const auto find = [&values](pcstr key) -> const GwpValue* {
        const auto it = values.find(key);
        return it != values.end() ? &it->second.view() : nullptr;
    };
    if (!SaveValueString(find(kSaveLevelPackKey), out.pack_id))
    {
        // A save without the pack (made before the key existed - on the base): loads on the content of now, if its
        // spawn is that one. No save_compat to vouch for another spawn: on a pack it is the content of another one
        if (SaveSpawnDiffers(spawn))
        {
            Msg("! Cannot load saved game %s: it was made on another %s.spawn than the one of the location pack '%s' "
                "(the GUID of the spawn differs; the save has no location pack key - it is of other content)",
                save_name, spawn.name.c_str(), ActiveLevelPack().id.c_str());
            out.pack_id = ActiveLevelPack().id; // the name for the window
            return ESaveContent::PackIncompatible;
        }
        return ESaveContent::Ok;
    }

    const LevelPackInfo* pack = FindLevelPack(out.pack_id.c_str());
    // A pack whose required plugin failed in this run cannot become active (slots::RequestWorld refuses it): without
    // this the switch would fall back to the built-in pack and the load would silently not happen
    if (pack && !pack->builtin() && slots::AddonFailed(pack->id.c_str()))
    {
        Msg("! Cannot load saved game %s: its location pack '%s' does not work in this run (its required plugin did "
            "not load)", save_name, out.pack_id.c_str());
        return ESaveContent::PackMissing;
    }
    if (!pack)
    {
        Msg("! Cannot load saved game %s: its location pack '%s' is not installed", save_name, out.pack_id.c_str());
        return ESaveContent::PackMissing;
    }

    // A save made before save_compat was written keeps loading (its pack is the right one)
    const GwpValue* compat = find(kSaveLevelPackCompatKey);
    if (compat)
    {
        // Only a whole number >= 1 is a save_compat (as the pack has it); a string, a bool, a fraction, a negative
        // number or NaN in the key is a broken one - incompatible (u32(double) of them is undefined)
        bool valid = false;
        u32 value = 0;
        if (compat->type == GWP_T_INT && compat->u.i >= 1 && compat->u.i <= s64(type_max<u32>))
        {
            valid = true;
            value = u32(compat->u.i);
        }
        else if (compat->type == GWP_T_NUMBER && compat->u.n >= 1.0 && compat->u.n <= double(type_max<u32>) &&
            compat->u.n == std::floor(compat->u.n))
        {
            valid = true;
            value = u32(compat->u.n);
        }
        if (!valid)
        {
            Msg("! Cannot load saved game %s: its save_compat of the location pack '%s' is not a whole number >= 1",
                save_name, out.pack_id.c_str());
            return ESaveContent::PackIncompatible;
        }
        if (value != pack->save_compat)
        {
            Msg("! Cannot load saved game %s: it was made on save_compat %u of the location pack '%s', the installed "
                "one has %u", save_name, value, out.pack_id.c_str(), pack->save_compat);
            return ESaveContent::PackIncompatible;
        }
    }
    // The pack of the save is the active one: its all.spawn should be the one of the save (a pack of another one is
    // checked after the switch, by the load that follows it). 00-roadmap.md 5a, decision 64.1: the same save_compat is
    // the word of the author that the saves stay compatible after a rebuild of all.spawn - the load goes on with a
    // warning (as CALifeSpawnRegistry::load of the base). A save without save_compat (made before it was written) has
    // no such word - refused
    if (pack == &ActiveLevelPack() && SaveSpawnDiffers(spawn))
    {
        if (!compat)
        {
            Msg("! Cannot load saved game %s: it was made on another %s.spawn than the one of the location pack '%s' "
                "(the GUID of the spawn differs and the save has no save_compat of the pack to compare)",
                save_name, spawn.name.c_str(), out.pack_id.c_str());
            return ESaveContent::PackIncompatible;
        }
        if (g_spawn_warned_save != save_name) // once per load (g_spawn_warned_save)
        {
            g_spawn_warned_save = save_name;
            Msg("~ [level_pack] saved game %s was made on another %s.spawn of the location pack '%s' (the GUID of the "
                "spawn differs) with the same save_compat %u: loading it. The pack was rebuilt without a raised "
                "save_compat - start a new game if A-Life or spawns look wrong",
                save_name, spawn.name.c_str(), out.pack_id.c_str(), pack->save_compat);
        }
    }
    out.pack = pack;

    // The slots bound to the save (13-extension-model.md, 7): their providers come with the save as the pack does
    for (const slots::SlotInfo& slot : slots::Slots())
    {
        if (!slot.save_bound || slot.id == slots::kWorldSlot)
            continue; // world: the pack above (gw/game/level_pack, 13-extension-model.md 9, decision 14)
        xr_string provider;
        if (!SaveValueString(find((xr_string(slots::kSaveSlotKeyPrefix) + slot.id).c_str()), provider))
            continue; // a save made before the slot or the key existed: as it is
        const slots::ProviderInfo* found = slot.find(provider.c_str());
        if (!found || slots::AddonFailed(found->id.c_str()))
        {
            Msg("! Cannot load saved game %s: the provider '%s' of its slot %s is not installed or does not work",
                save_name, provider.c_str(), slot.id.c_str());
            out.missing_slot = slot.id;
            out.missing_provider = provider;
            return ESaveContent::ProviderMissing;
        }
        out.providers.emplace_back(slot.id, found->id);
    }
    // A save-bound slot the save names that this run does not know (its [slots_declare] addon is off or gone): its
    // provider is gone with it - the same refusal as a known slot without its provider. "core" (the base) needs nothing
    const size_t prefix_length = xr_strlen(slots::kSaveSlotKeyPrefix);
    for (const auto& [key, value] : values)
    {
        if (key.compare(0, prefix_length, slots::kSaveSlotKeyPrefix) != 0)
            continue;
        const xr_string slot_id = key.substr(prefix_length);
        if (slot_id.empty() || slot_id.find('/') != xr_string::npos || slots::FindSlot(slot_id.c_str()))
            continue; // gw/slots/<slot>/version, or a slot of this run (above)
        xr_string provider;
        if (!SaveValueString(&value.view(), provider))
            continue;
        if (provider == slots::kCoreProvider)
        {
            Msg("~ [slots] saved game %s names the slot %s that is not declared in this run (its provider was %s)",
                save_name, slot_id.c_str(), provider.c_str());
            continue;
        }
        Msg("! Cannot load saved game %s: its slot %s (provider '%s') is not declared in this run - the addon that "
            "declares it is not installed or does not work", save_name, slot_id.c_str(), provider.c_str());
        out.missing_slot = slot_id;
        out.missing_provider = provider;
        return ESaveContent::ProviderMissing;
    }
    return ESaveContent::Ok;
}

// The save a start loads (CALifeSimulator: "<save>/single/alife/load"; without "new" a save that exists is loaded):
// empty - a new game, a start of another kind
xr_string StartSaveName(pcstr op_server)
{
    IGame_Persistent::params params;
    params.parse_cmd_line(op_server ? op_server : "");
    if (!params.m_game_or_spawn[0] || xr_strcmp(params.m_game_type, "single") || xr_strcmp(params.m_alife, "alife") ||
        !xr_strcmp(params.m_new_or_load, "new"))
    {
        return xr_string();
    }
    return params.m_game_or_spawn;
}
} // namespace

void OnFrameLevelPacks()
{
    // The start of the command line was refused for its save: the reason in a window of the menu (the same check as a
    // load from the menu), a frame after the content of the start is mounted and the menu is built
    if (!g_refused_save.empty() && !slots::ApplyPending() && !g_pGameLevel && !Device.dwPrecacheFrame && MainMenu() &&
        MainMenu()->IsActive())
    {
        const xr_string save = g_refused_save;
        g_refused_save.clear();
        if (CanLoadSaveLevelPack(save.c_str()))
            Msg("- [level_pack] saved game %s may be loaded now (from the menu)", save.c_str());
    }
    // -addon_strict refused a start: the window, once the menu shows (it is asked for once)
    if (g_strict_window && !slots::ApplyPending() && !g_pGameLevel && !Device.dwPrecacheFrame)
    {
        if (MainMenu() && MainMenu()->IsActive())
        {
            g_strict_window = false;
            g_strict_menu_asked = false;
            ShowStrictMergeWindow();
        }
        else if (!g_strict_menu_asked)
        {
            g_strict_menu_asked = true;
            Engine.Event.Defer("KERNEL:console", size_t(xr_strdup("main_menu on")));
        }
    }
    slots::OnFrame();
}

void ApplyStartupContentForLevel(pcstr op_server)
{
    if (!g_packs_initialized)
        return; // a dedicated server: no addon host, no packs, no content of the addons
    const xr_string save = StartSaveName(op_server);
    SaveContent content;
    if (!save.empty())
    {
        if (ReadSaveContent(save.c_str(), content) != ESaveContent::Ok)
        {
            // The content set of the start stays for the first frame of the menu, as without -start
            Msg("! [level_pack] the start of the command line (%s) is refused: the main menu shows the reason",
                op_server);
            g_refused_save = save;
            g_refuse_start = true;
            return;
        }
        if (content.pack)
            slots::RequestStartupForSave(content.pack->id.c_str(), content.providers);
    }
    if (!slots::ApplyPending())
        return; // nothing to mount: the level loads on the base, as without addons
    Msg("- [level_pack] a level load waits from the command line: the content of the start is applied before it");
    slots::ApplyStartupNow();

    // The spawn of the save against all.spawn of the pack mounted now (before the mount no pack was the active one)
    if (!save.empty())
    {
        SaveContent mounted;
        if (ReadSaveContent(save.c_str(), mounted) == ESaveContent::PackIncompatible)
        {
            Msg("! [level_pack] the start of the command line (%s) is refused: the main menu shows the reason",
                op_server);
            g_refused_save = save;
            g_refuse_start = true;
            return;
        }
    }

    if (!content.pack)
        return;
    // A fallback of the procedure (the pack or a set cannot be mounted, a required plugin failed): not that save
    bool ok = &ActiveLevelPack() == content.pack;
    for (const auto& [slot_id, provider_id] : content.providers)
    {
        const slots::SlotInfo* slot = slots::FindSlot(slot_id.c_str());
        ok &= slot && slot->active_provider().id == provider_id;
    }
    if (ok)
        return;
    Msg("! Cannot load saved game %s: the content it needs did not mount (see above), the start of the command line "
        "is refused", save.c_str());
    g_refused_save = save;
    g_refuse_start = true;
}

bool CanStartLevel(pcstr op_server)
{
    g_spawn_warned_save.clear(); // the load attempt is decided: a new one warns again
    if (g_refuse_start)
    {
        g_refuse_start = false; // only the start of the command line it was decided for
        Msg("! [level_pack] the level load (%s) does not start: saved game %s cannot load in this run",
            op_server ? op_server : "", g_refused_save.c_str());
        return false;
    }
    // -addon_strict (15-merge-implementation.md 5.4; 00-roadmap.md 5a, decision 47.9): unresolved conflicts of the
    // merge of the addons - no level starts (a new game, a load from the menu or the command line); the menu stays,
    // the window lists them (OnFrameLevelPacks)
    if (addons::StrictMergeBlocks())
    {
        const xr_string what = xr_string("the level load (") + (op_server ? op_server : "") + ")";
        addons::LogStrictMergeBlock(what.c_str());
        g_strict_window = true;
        return false;
    }
    return true;
}

void WriteSaveContentKeys()
{
    if (!g_packs_initialized)
        return; // a dedicated server: no packs, no slots
    namespace events = addons::events;
    using addons::data::SetPersistentFromEngine;
    const LevelPackInfo& pack = ActiveLevelPack();
    SetPersistentFromEngine(kSaveLevelPackKey, events::String(pack.id.c_str()));
    SetPersistentFromEngine(kSaveLevelPackVersionKey, events::String(pack.version.c_str()));
    SetPersistentFromEngine(kSaveLevelPackCompatKey, events::Int(s64(pack.save_compat)));
    // The level for the load dialog: CSavedGameWrapper does not read it from a spawn of another pack
    const shared_str level_name = g_pGameLevel ? g_pGameLevel->name() : shared_str();
    SetPersistentFromEngine(kSaveLevelNameKey, level_name.size() ? events::String(level_name.c_str()) : events::Nil());
    for (const slots::SlotInfo& slot : slots::Slots())
    {
        if (!slot.save_bound || slot.id == slots::kWorldSlot)
            continue; // world: the pack above
        const slots::ProviderInfo& provider = slot.active_provider();
        const xr_string key = xr_string(slots::kSaveSlotKeyPrefix) + slot.id;
        SetPersistentFromEngine(key.c_str(), events::String(provider.id.c_str()));
        SetPersistentFromEngine((key + "/version").c_str(), events::String(provider.version.c_str()));
    }
}

bool SaveValueString(const GwpValue* value, xr_string& out)
{
    if (!value || value->type != GWP_T_STRING)
        return false;
    out.assign(value->u.s.ptr ? value->u.s.ptr : "", value->u.s.len);
    return true;
}

bool CanLoadSaveLevelPack(pcstr save_name)
{
    // -addon_strict: the load would be refused at its start anyway (CanStartLevel) - here, before the menu leaves. Only
    // for a save the content set of now loads: a save of another location pack (or provider) is switched to first, the
    // merge of that content set decides at its start (CanStartLevel), not the merge of the set of now
    const auto strict_refuses = [save_name]() {
        if (!addons::StrictMergeBlocks())
            return false;
        const xr_string what = xr_string("the load of saved game ") + (save_name ? save_name : "");
        addons::LogStrictMergeBlock(what.c_str());
        ShowStrictMergeWindow();
        return true;
    };
    if (!g_packs_initialized)
    {
        // A dedicated server: no addon host (CAI_Space::init), nothing to check against
        Msg("~ [level_pack] saved game %s: the location packs are not known yet, loaded unchecked", save_name);
        return !strict_refuses();
    }
    SaveContent content;
    const ESaveContent result = ReadSaveContent(save_name, content);
    const auto call_lua = [save_name, &content](pcstr function) {
        luabind::functor<void> callback;
        if (GEnv.ScriptEngine && GEnv.ScriptEngine->functor(function, callback))
            callback(save_name, content.pack_id.c_str());
        else
            Msg("! [level_pack] Lua function %s not found", function);
    };
    switch (result)
    {
    case ESaveContent::Ok: break;
    case ESaveContent::PackMissing: call_lua("ui_load_dialog.on_level_pack_missing"); return false;
    case ESaveContent::PackIncompatible: call_lua("ui_load_dialog.on_level_pack_incompatible"); return false;
    case ESaveContent::ProviderMissing:
    {
        luabind::functor<void> callback;
        if (GEnv.ScriptEngine && GEnv.ScriptEngine->functor("gw_slots.on_save_provider_missing", callback))
            callback(save_name, content.missing_slot.c_str(), content.missing_provider.c_str());
        else
            Msg("! [slots] Lua function gw_slots.on_save_provider_missing not found");
        return false;
    }
    }
    if (!content.pack)
        return !strict_refuses(); // a save without the pack key, a missing or invalid one

    // The providers the save names that are not the active ones
    xr_vector<std::pair<xr_string, xr_string>> slot_switches;
    for (const auto& [slot_id, provider_id] : content.providers)
    {
        const slots::SlotInfo* slot = slots::FindSlot(slot_id.c_str());
        if (slot && slot->active_provider().id != provider_id)
            slot_switches.emplace_back(slot_id, provider_id);
    }

    const bool other_pack = content.pack != &ActiveLevelPack();
    if (!other_pack && slot_switches.empty())
        return !strict_refuses();

    // Another installed pack or provider: the switch, then the load (ui_main_menu.on_level_pack_applied)
    if (other_pack)
        Msg("- [level_pack] saved game %s was made on the location pack '%s': switching to it", save_name,
            content.pack_id.c_str());
    for (const auto& [slot, provider] : slot_switches)
        Msg("- [slots] saved game %s was made with the provider '%s' of the slot %s: switching to it", save_name,
            provider.c_str(), slot.c_str());
    const xr_string action = xr_string("load:") + save_name;
    slots::RequestForSave(other_pack ? content.pack_id.c_str() : nullptr, slot_switches, action.c_str());
    if (g_pGameLevel) // the switch waits for the main menu: the game ends first
        Engine.Event.Defer("KERNEL:console", size_t(xr_strdup("disconnect")));
    return false;
}
} // namespace gw
