#include "StdAfx.h"

#include "level_pack.h"
#include "addon_host.h"
#include "saved_game_wrapper.h"
#include "MainMenu.h"
#include "spawn_smart_terrain_map_data_source.h"

#include "xrEngine/Engine.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/IGame_Level.h"
#include "xrEngine/Render.h"
#include "xrSound/Sound.h"
#include "xrEngine/XR_IOConsole.h"
#include "xrEngine/StringTable/StringTable.h"
#include "xrScriptEngine/script_engine.hpp"

#include <filesystem>

class CScriptIniFile;
CScriptIniFile* reload_system_ini(); // script_ini_file_script.cpp

typedef xr_vector<std::pair<shared_str, int>> STORY_PAIRS; // alife_simulator_script.cpp
extern STORY_PAIRS story_ids;
extern STORY_PAIRS spawn_story_ids;

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

struct PendingSwitch
{
    bool pending = false;
    xr_string id;
    xr_string action;
};
PendingSwitch g_switch;

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

    // The disk directly (UTF-8 path, the separators of the OS): the pack is not mounted yet
    const std::filesystem::path spawn = std::filesystem::path(reinterpret_cast<const char8_t*>(addon.dir.c_str())) /
        "gamedata" / "spawns" / "all.spawn";
    std::error_code error;
    if (!std::filesystem::is_regular_file(spawn, error))
    {
        Msg("! [level_pack] addon '%s' (type = %s) has no gamedata\\spawns\\all.spawn, it is not a location pack",
            addon.id.c_str(), kLevelPackType);
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

// The layers of a pack: its gamedata\, over it the folder its writes go to. The built-in pack has none (gamedata is
// the base itself, it writes where its files lie).
bool MountPackLayers(const LevelPackInfo& pack)
{
    if (pack.builtin())
        return true;
    string_path gamedata;
    strconcat(sizeof gamedata, gamedata, pack.dir.c_str(), "gamedata", DELIMITER);
    if (!FS.mount_layer(PackLayerId(pack).c_str(), gamedata))
        return false;

    string_path user_folder, user;
    strconcat(sizeof user_folder, user_folder, kUserLayerFolder, DELIMITER, pack.id.c_str(), DELIMITER, "gamedata",
        DELIMITER);
    FS.update_path(user, "$app_data_root$", user_folder);
    if (!FS.mount_layer(UserLayerId(pack).c_str(), user, nullptr, true))
    {
        // Without it a write of a file of the pack would go into the base gamedata (the built-in pack) and the pack
        // would go on serving its own old file
        Msg("! [level_pack] '%s': no folder for its writes [%s], the pack is not mounted", pack.id.c_str(), user);
        FS.unmount_layer(PackLayerId(pack).c_str());
        return false;
    }
    return true;
}

void UnmountPackLayers(const LevelPackInfo& pack)
{
    if (pack.builtin())
        return;
    if (FS.find_layer(UserLayerId(pack).c_str()))
        FS.unmount_layer(UserLayerId(pack).c_str());
    FS.unmount_layer(PackLayerId(pack).c_str());
}

// The switch itself (plans/addons_api_support/10-location-packs.md, section 3). The main menu is shown, no level and
// no ALife, the start of a frame: no Lua and no deferred event of the engine on the stack.
void ApplyLevelPack(size_t target)
{
    CTimer timer;
    timer.Start();
    const LevelPackInfo& from = g_packs[g_active];
    Msg("* [level_pack] switch '%s' -> '%s'", from.id.c_str(), g_packs[target].id.c_str());

    // 1. Nothing reads the index of the file system on other threads while the layers change (a reader keeps a
    // pointer to an entry). The map of smart terrains loads all.spawn of the old pack - it goes.
    CSpawnSmartTerrainMapDataSource::ForgetSharedLoading();
    GEnv.Render->AssetsSwitchBegin();
    if (GEnv.Sound)
    {
        GEnv.Sound->stop_emitters();
        GEnv.Sound->prefetch_stop();
    }

    // 2. The layers
    UnmountPackLayers(from);
    if (!MountPackLayers(g_packs[target]))
    {
        Msg("! [level_pack] '%s' cannot be mounted, the built-in pack '%s' is active", g_packs[target].id.c_str(),
            g_packs[0].id.c_str());
        target = 0;
    }
    g_active = target;

    // 3. The configs. Lua runs no code until its restart below (it may keep handles of the old ones).
    reload_system_ini(); // plugins: gw::addons::ini::OnConfigsReloaded
    {
        string_path file_name;
        FS.update_path(file_name, "$game_config$", "game.ltx");
        xr_delete(pGameIni);
        pGameIni = xr_new<CInifile>(file_name);
    }
    story_ids.clear(); // generated again from game.ltx when the new Lua state registers them
    spawn_story_ids.clear();

    // 4. The strings (ReloadLanguage keeps them while the language is the same)
    StringTable().Destroy();
    StringTable().Init();

    // 5. What keeps files by name: the prefetched objects (script classes among them - before the Lua restart), models,
    // motions, texture descriptions, sound sources
    g_pGamePersistent->ObjectPool.clear();
    GEnv.Render->AssetsSwitchEnd();
    if (GEnv.Sound)
        GEnv.Sound->refresh_sources();

    // 6. The levels of the new pack
    g_pGamePersistent->Level_Scan();

    // 7. The scripts and the menu in the new world
    xr_strcpy(g_level_pack, g_packs[g_active].id.c_str());
    Console->Execute("cfg_save");
    // The visuals of the old menu go back into the pool when it is destroyed (after AssetsSwitchEnd): the new menu must
    // not take a model of the same name from there
    MainMenu()->RestartScripts([] { GEnv.Render->models_Clear(true); });

    CSpawnSmartTerrainMapDataSource::StartSharedLoadingWhenIdle("all");
    Msg("* [level_pack] '%s' is active, %.2f sec", g_packs[g_active].id.c_str(), timer.GetElapsed_sec());
}
} // namespace

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

namespace
{
void QueueSwitch(pcstr id, pcstr action)
{
    g_switch.pending = true;
    g_switch.id = id;
    g_switch.action = action ? action : "";
}
} // namespace

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
    QueueSwitch(id, action);
    return true;
}

bool LevelPackSwitchPending() { return g_switch.pending; }

void RequestStartupLevelPack()
{
    if (!g_packs_initialized)
        return; // a dedicated server: no addon host, no packs
    string64 id{};
    if (pcstr arg = strstr(Core.Params, "-level_pack "))
        sscanf(arg + xr_strlen("-level_pack "), "%63s", id);
    else
        xr_strcpy(id, g_level_pack);
    if (!id[0] || ActiveLevelPack().id == id)
        return;
    if (!FindLevelPack(id))
    {
        Msg("! [level_pack] the location pack '%s' is not installed, the built-in '%s' stays", id,
            ActiveLevelPack().id.c_str());
        return;
    }
    RequestLevelPackSwitch(id, "");
}

void OnFrameLevelPacks()
{
    if (!g_switch.pending)
        return;
    if (g_pGameLevel || Device.dwPrecacheFrame || !MainMenu() || !MainMenu()->IsActive())
        return; // later: a level is loading or unloading, the menu is not shown yet

    const PendingSwitch request = g_switch;
    g_switch = {};
    const LevelPackInfo* target = FindLevelPack(request.id.c_str());
    if (target && target != &g_packs[g_active])
        ApplyLevelPack(size_t(target - g_packs.data()));
    const bool ok = ActiveLevelPack().id == request.id;

    luabind::functor<void> on_applied;
    if (GEnv.ScriptEngine && GEnv.ScriptEngine->functor("ui_main_menu.on_level_pack_applied", on_applied))
        on_applied(request.id.c_str(), request.action.c_str(), ok);
    else if (!request.action.empty())
        Msg("! [level_pack] Lua function ui_main_menu.on_level_pack_applied not found, '%s' is not done",
            request.action.c_str());
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
    if (!g_packs_initialized)
    {
        // -load <save> of the command line runs before the addon host (CAI_Space::init): nothing to check against
        Msg("~ [level_pack] saved game %s: the location packs are not known yet, loaded unchecked", save_name);
        return true;
    }
    // Only the data bus chunk of the save: no actor, no spawn and graph as in the full CSavedGameWrapper
    gw::addons::data::SaveValues values;
    if (!CSavedGameWrapper::saved_game_data(save_name, values))
        return true; // missing or invalid: the callers report that themselves
    const auto find = [&values](pcstr key) -> const GwpValue* {
        const auto it = values.find(key);
        return it != values.end() ? &it->second.view() : nullptr;
    };
    xr_string pack_id;
    if (!SaveValueString(find(kSaveLevelPackKey), pack_id))
        return true; // a save without the pack (made before the key existed): load as usual

    const auto call_lua = [save_name, &pack_id](pcstr function) {
        luabind::functor<void> callback;
        if (GEnv.ScriptEngine && GEnv.ScriptEngine->functor(function, callback))
            callback(save_name, pack_id.c_str());
        else
            Msg("! [level_pack] Lua function %s not found", function);
    };

    const LevelPackInfo* pack = FindLevelPack(pack_id.c_str());
    if (!pack)
    {
        Msg("! Cannot load saved game %s: its location pack '%s' is not installed", save_name, pack_id.c_str());
        call_lua("ui_load_dialog.on_level_pack_missing");
        return false;
    }

    // A save made before save_compat was written keeps loading (its pack is the right one)
    if (const GwpValue* compat = find(kSaveLevelPackCompatKey))
    {
        const double value = compat->type == GWP_T_INT ? double(compat->u.i) :
            compat->type == GWP_T_NUMBER ? compat->u.n : double(pack->save_compat);
        if (u32(value) != pack->save_compat)
        {
            Msg("! Cannot load saved game %s: it was made on save_compat %u of the location pack '%s', the installed "
                "one has %u", save_name, u32(value), pack_id.c_str(), pack->save_compat);
            call_lua("ui_load_dialog.on_level_pack_incompatible");
            return false;
        }
    }

    if (pack == &ActiveLevelPack())
        return true;

    // Another installed pack: the switch, then the load (ui_main_menu.on_level_pack_applied)
    Msg("- [level_pack] saved game %s was made on the location pack '%s': switching to it", save_name,
        pack_id.c_str());
    const xr_string action = xr_string("load:") + save_name;
    QueueSwitch(pack_id.c_str(), action.c_str());
    if (g_pGameLevel) // the switch waits for the main menu: the game ends first
        Engine.Event.Defer("KERNEL:console", size_t(xr_strdup("disconnect")));
    return false;
}
} // namespace gw
