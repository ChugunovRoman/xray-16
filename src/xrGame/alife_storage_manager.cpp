////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_storage_manager.cpp
//	Created 	: 25.12.2002
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife Simulator storage manager
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "alife_storage_manager.h"
#include "alife_simulator_header.h"
#include "alife_time_manager.h"
#include "alife_time_skip.h"
#include "alife_update_manager.h"
#include "alife_spawn_registry.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "alife_group_registry.h"
#include "alife_registry_container.h"
#include "relation_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "xrServer.h"
#include "Level.h"
#include "saved_game_wrapper.h"
#include "xrEngine/IGame_Persistent.h"
#include "autosave_manager.h"
#include "addon_host.h"
#include "addon_api_alife_ext.h"
#include "addon_binders.h"
#include "addon_event_bus.h"
#include "addon_lua_entry.h"
#include "addon_timers.h"
#include "level_pack.h"
#include "map_manager.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include <algorithm>

using namespace ALife;

extern string_path g_last_saved_game;
// 53-create-squad-engine.md section 9 (xrServer_Objects_ALife_Monsters.cpp)
void gw_rebuild_specific_character_registry(CALifeObjectRegistry& objects, CSpecificCharacterRegistry& registry);

namespace
{
// 09-content-addons-vfs.md section 8 (C8): the event alife_objects_skipped (save name, ids, sections) - the saved
// objects load() could not create (a section of a switched off addon) and the objects inside them, by the index of
// the arrays. An array holds at most kMaxArrayItems values: a longer list comes in several events, in order
void EmitObjectsSkipped(pcstr save_name, const xr_vector<ALife::_OBJECT_ID>& ids, const xr_vector<shared_str>& sections)
{
    namespace events = gw::addons::events;
    if (ids.empty() || !events::HasSubscribers(events::EBuiltin::AlifeObjectsSkipped))
        return;
    VERIFY(ids.size() == sections.size());
    const size_t count = std::min(ids.size(), sections.size());
    xr_vector<GwpValue> id_items;
    xr_vector<GwpValue> section_items;
    for (size_t first = 0; first < count; first += events::kMaxArrayItems)
    {
        const size_t last = std::min(count, first + events::kMaxArrayItems);
        id_items.clear();
        section_items.clear();
        for (size_t i = first; i < last; ++i)
        {
            id_items.push_back(events::Int(ids[i]));
            section_items.push_back(events::String(sections[i].c_str())); // the shared_str outlives the emit
        }
        GwpValue id_array = events::Nil();
        id_array.type = GWP_T_ARRAY;
        id_array.u.a.items = id_items.data();
        id_array.u.a.count = static_cast<uint32_t>(id_items.size());
        GwpValue section_array = events::Nil();
        section_array.type = GWP_T_ARRAY;
        section_array.u.a.items = section_items.data();
        section_array.u.a.count = static_cast<uint32_t>(section_items.size());
        const GwpValue args[] = { events::String(save_name), id_array, section_array };
        events::Emit(events::EBuiltin::AlifeObjectsSkipped, args, static_cast<u32>(std::size(args)));
    }
}
} // namespace

CALifeStorageManager::~CALifeStorageManager() {}
void CALifeStorageManager::save(LPCSTR save_name_no_check, bool update_name)
{
    GW_FRAME_EVENT(Save); // 50n: the frame spike report
    // 50z5-extra-diagnostics.md: the "* [save]" line - the stages of the save and its size
    const u64 save_t0 = xray::perf::Ticks();
    pcstr gameSaveExtension = SAVE_EXTENSION;
    if (ShadowOfChernobylMode || ClearSkyMode)
        gameSaveExtension = SAVE_EXTENSION_LEGACY;

    LPCSTR game_saves_path = FS.get_path("$game_saves$")->m_Path;

    string_path save_name;
    strncpy_s(save_name, sizeof(save_name), save_name_no_check,
        sizeof(save_name) - 5 - xr_strlen(gameSaveExtension) - xr_strlen(game_saves_path));

    xr_strcpy(g_last_saved_game, save_name);

    string_path saveBackup;
    xr_strcpy(saveBackup, m_save_name);
    if (save_name[0])
    {
        strconcat(sizeof(m_save_name), m_save_name, save_name, gameSaveExtension);
    }
    else
    {
        if (!xr_strlen(m_save_name))
        {
            Log("There is no file name specified!");
            return;
        }
    }

    // plans/lua_to_cpp/55-timeskip-squad-movement.md: a time skip still catching up finishes first (the Lua save states
    // and the plugin blocks take its results)
    alife_time_skip::Finish(static_cast<CALifeUpdateManager&>(*this), "save");

	//Alundaio: To get the savegame fname to make our own custom save states
    luabind::functor<void> funct1;
    if (GEnv.ScriptEngine->functor("alife_storage_manager.CALifeStorageManager_before_save", funct1))
        funct1((pcstr)m_save_name);
	//-Alundaio

    // Addons: plugins store their data (save_write) before the stream is built.
    {
        const GwpValue args[] = { gw::addons::events::String(m_save_name) };
        gw::addons::events::Emit(gw::addons::events::EBuiltin::AlifeOnBeforeSave, args, 1);
    }
    // The keys of the content the save needs (gw/game/level_pack*, gw/slots/*): from the registry of the engine, over
    // what Lua and the plugins wrote (plan 10 section 4, P4)
    gw::WriteSaveContentKeys();

    u32 source_count;
    u32 dest_count;
    void* dest_data;
    const u64 save_t_lua = xray::perf::Ticks(); // 50z5
    u64 save_t_built;
    {
        CMemoryWriter stream;
        header().save(stream);
        time_manager().save(stream);
        spawns().save(stream);
        objects().save(stream);
        registry().save(stream);
        gw::addons::WriteSaveData(stream); // plugin data: own chunk of the stream, see ReadSaveData

        source_count = stream.tell();
        save_t_built = xray::perf::Ticks(); // 50z5
        void* source_data = stream.pointer();
        dest_count = rtc_csize(source_count);
        dest_data = xr_malloc(dest_count);
        dest_count = rtc_compress(dest_data, dest_count, source_data, source_count);
    }

    const u64 save_t_compressed = xray::perf::Ticks(); // 50z5
    string_path temp;
    FS.update_path(temp, "$game_saves$", m_save_name);
    IWriter* writer = FS.w_open(temp);
    writer->w_u32(u32(-1));
    writer->w_u32(ALIFE_VERSION);

    writer->w_u32(source_count);
    writer->w(dest_data, dest_count);
    xr_free(dest_data);
    FS.w_close(writer);
    const u64 save_t_written = xray::perf::Ticks(); // 50z5
#ifdef DEBUG
    Msg("* Game %s is successfully saved to file '%s' (%d bytes compressed to %d)", m_save_name, temp, source_count,
        dest_count + 4);
#else // DEBUG
    Msg("* Game %s is successfully saved to file '%s'", m_save_name, temp);
#endif // DEBUG

	//Alundaio: To get the savegame fname to make our own custom save states
    luabind::functor<void> funct2;
    if (GEnv.ScriptEngine->functor("alife_storage_manager.CALifeStorageManager_save", funct2))
        funct2((pcstr)m_save_name);
    //-Alundaio

    {
        const GwpValue args[] = { gw::addons::events::String(m_save_name) };
        gw::addons::events::Emit(gw::addons::events::EBuiltin::AlifeOnAfterSave, args, 1);
    }
    {
        // 50z5-extra-diagnostics.md: lua before = the before-save callbacks and plugins; build = the ALife stream;
        // write = the file (open, write, close); lua after = the after-save callbacks
        const auto ms = [](u64 a, u64 b) { return xray::perf::TicksToMilliseconds(b - a); };
        const u64 save_t_end = xray::perf::Ticks();
        Msg("* [save] %s %.1f ms: lua before %.1f, build %.1f, compress %.1f, write %.1f, lua after %.1f | %.1f MB -> "
            "%.1f MB", m_save_name, ms(save_t0, save_t_end), ms(save_t0, save_t_lua), ms(save_t_lua, save_t_built),
            ms(save_t_built, save_t_compressed), ms(save_t_compressed, save_t_written), ms(save_t_written, save_t_end),
            source_count / (1024.0 * 1024.0), (dest_count + 12) / (1024.0 * 1024.0));
    }

    if (!update_name)
        xr_strcpy(m_save_name, saveBackup);
}

void CALifeStorageManager::load(void* buffer, const u32& buffer_size, LPCSTR file_name)
{
    IReader source(buffer, buffer_size);

    // Addons: until time_manager().load the game time is the one of a fresh ALife, not of the save: a game-time
    // timer started now (e.g. from load_state) would get a wrong due time. Cleared after time_manager().load.
    gw::addons::timers::SetLoadingGameTime(true);

    // Addons: plugin data, data bus and timers (own chunks of the stream, found by id) are read first, so the
    // Lua load_state below already sees persistent data bus values. seek(0): open_chunk moves the position.
    gw::addons::ReadSaveData(source);
    source.seek(0);

	//Alundaio: So we can get the fname to make our own custom save states
    luabind::functor<void> funct;
    if (GEnv.ScriptEngine->functor("alife_storage_manager.CALifeStorageManager_load", funct))
        funct(file_name);
	//-Alundaio

    header().load(source);
    time_manager().load(source);
    gw::addons::timers::SetLoadingGameTime(false); // the game time of the save is known from here on
    spawns().load(source, file_name);
    graph().on_load();
    objects().load(source);
    // 09-content-addons-vfs.md section 8 (C8): objects of a switched off addon were skipped - their ids must not stay in
    // the saved references of the loaded objects: the member lists of the squads (on_after_game_load ->
    // register_member asserts on them) and the smart terrain of a monster (its brain asks the smart terrain registry)
    if (!objects().skipped_on_load().empty())
    {
        const auto& skipped = objects().skipped_on_load();
        u32 dropped_members = 0;
        u32 dropped_smarts = 0;
        for (auto& object : objects().objects())
        {
            if (CSE_ALifeOnlineOfflineGroup* squad = object.second->cast_online_offline_group())
                dropped_members += squad->drop_unloaded_members();

            CSE_ALifeMonsterAbstract* monster = smart_cast<CSE_ALifeMonsterAbstract*>(object.second);
            if (monster && monster->m_smart_terrain_id != ALife::_OBJECT_ID(-1) &&
                std::find(skipped.begin(), skipped.end(), monster->m_smart_terrain_id) != skipped.end())
            {
                Msg("~ [save] object %u '%s': its smart terrain %u is skipped, the smart terrain is cleared",
                    u32(monster->ID), monster->name_replace(), u32(monster->m_smart_terrain_id));
                monster->m_smart_terrain_id = ALife::_OBJECT_ID(-1);
                ++dropped_smarts;
            }
        }
        Msg("~ [save] references to the skipped objects cleared: %u squad members, %u smart terrains", dropped_members,
            dropped_smarts);
    }

    VERIFY(can_register_objects());
    can_register_objects(false);

    for (auto& object : objects().objects())
    {
        ALife::_OBJECT_ID id = object.second->ID;
        object.second->ID = server().PerformIDgen(id);
        VERIFY(id == object.second->ID);
        register_object(object.second, false);
    }

    registry().load(source);
    // C8: the saved state of a skipped object would pass to a new object that gets its id: its relations - its own
    // ones (as on_unregister of a monster clears them on a release) and the personal relations of the others to it -
    // and the kept binder state of the plugins (read by ReadSaveData above; as unregister_object drops it on a release)
    RELATION_REGISTRY().ClearRelationsOfRemoved(objects().skipped_on_load());
    for (const ALife::_OBJECT_ID id : objects().skipped_on_load())
        gw::addons::binders::OnServerObjectRelease(id);
    // C8: the same for the per-id tables of the scripts. Their load_state (CALifeStorageManager_load above) is done,
    // on_register of the objects (below) is not: the scripts drop the state of the skipped ids before a new object can
    // get one of them. The list: Lua alife_save.skipped_objects(). Under lua_pcall: the event reaches the addons
    if (!objects().skipped_on_load().empty())
    {
        // C8: the PDA map first - the saved map locations of the skipped ids would go only at the first update of the
        // map (no owner), till then a new object that gets such an id would find a location of the old one
        // (map_has_object_spot, the objectives of the tasks). Before the Lua callback and the event: both see the map
        // without them (events_meta.json, alife_objects_skipped)
        if (g_pGameLevel && !GEnv.isDedicatedServer) // a dedicated server has no map manager
        {
            const u32 locations = Level().MapManager().RemoveMapLocationsOfObjects(objects().skipped_on_load());
            if (locations)
                Msg("~ [save] %u map location(s) of the skipped objects removed", locations);
        }
        constexpr pcstr skipped_function = "alife_storage_manager.CALifeStorageManager_on_objects_skipped";
        if (gw::addons::CallLuaProtected(skipped_function, { file_name }) == gw::addons::ELuaCall::NotFound)
            Msg("! [save] Lua function %s not found: the script states of the skipped objects stay", skipped_function);
        // C8: the same for the plugins - the event alife_objects_skipped
        EmitObjectsSkipped(m_save_name, objects().skipped_on_load(), objects().skipped_sections_on_load());
    }
    // 53-create-squad-engine.md section 9: the saved registry of the specific characters holds the leaked ids of the
    // old engine; it is made again from the objects, before on_register below can select a character
    gw_rebuild_specific_character_registry(objects(), registry(specific_characters));

    // Addons: plugin data was read at the start of load(). Objects are registered, on_register is not called yet.
    {
        const GwpValue args[] = { gw::addons::events::String(m_save_name) };
        gw::addons::events::Emit(gw::addons::events::EBuiltin::AlifeOnLoad, args, 1);
    }

    can_register_objects(true);

    for (auto& object : objects().objects())
    {
        object.second->on_register();
        gw::addons::alife_ext::OnServerObjectRegistered(object.second); // server_object_on_register
    }

    // Addons: every object is registered. Before the early return: the event must come on every load.
    {
        const GwpValue args[] = { gw::addons::events::String(m_save_name) };
        gw::addons::events::Emit(gw::addons::events::EBuiltin::AlifeOnAfterLoad, args, 1);
    }

    if (!g_pGameLevel)
        return;

    Level().autosave_manager().on_game_loaded();

	//Neloreck: For consistency with before/after save callbacks.
    luabind::functor<void> funct2;
    if (GEnv.ScriptEngine->functor("alife_storage_manager.CALifeStorageManager_after_load", funct2))
        funct2(file_name);
	//-Neloreck
}

bool CALifeStorageManager::load(LPCSTR save_name_no_check)
{
    pcstr gameSaveExtension = SAVE_EXTENSION;
    if (ShadowOfChernobylMode || ClearSkyMode)
        gameSaveExtension = SAVE_EXTENSION_LEGACY;

    LPCSTR game_saves_path = FS.get_path("$game_saves$")->m_Path;

    string_path save_name;
    strncpy_s(save_name, sizeof(save_name), save_name_no_check,
        sizeof(save_name) - 5 - xr_strlen(gameSaveExtension) - xr_strlen(game_saves_path));

    CTimer timer;
    timer.Start();

    string_path saveBackup;
    xr_strcpy(saveBackup, m_save_name);
    if (!save_name[0])
    {
        if (!xr_strlen(m_save_name))
        {
            Log("There is no file name specified!");
            return false;
        }
    }
    else
    {
        strconcat(sizeof(m_save_name), m_save_name, save_name, gameSaveExtension);
    }
    string_path file_name;
    FS.update_path(file_name, "$game_saves$", m_save_name);

    xr_strcpy(g_last_saved_game, save_name);
    xrDebug::SetBugReportFile(file_name);

    IReader* stream = FS.r_open(file_name);
    if (!stream)
    {
        Msg("* Cannot open saved game %s", file_name);
        xr_strcpy(m_save_name, saveBackup);
        return false;
    }

    constexpr pcstr mismatch = "Saved game version mismatch or saved game is corrupted";
    const bool gameSaveIsValid = CSavedGameWrapper::valid_saved_game(*stream);
    VERIFY3(gameSaveIsValid, mismatch, file_name);

    if (!gameSaveIsValid)
    {
        Msg("! %s [%s]", mismatch, file_name);

        xr_strcpy(m_save_name, saveBackup);
        return false;
    }

    string512 temp;
    strconcat(temp, StringTable().translate("st_loading_saved_game").c_str(),
        " \"", save_name, gameSaveExtension, "\"");

    g_pGamePersistent->LoadTitle(temp);

    unload();
    reload(m_section);

    const u32 source_count = stream->r_u32();
    void* source_data = xr_malloc(source_count);
    rtc_decompress(source_data, source_count, stream->pointer(), stream->length() - 3 * sizeof(u32));
    FS.r_close(stream);
    load(source_data, source_count, file_name);
    xr_free(source_data);

    groups().on_after_game_load();

    VERIFY(graph().actor());

    Msg("* Game %s is successfully loaded from file '%s' (%.3fs)", save_name, file_name, timer.GetElapsed_sec());

    return (true);
}

void CALifeStorageManager::save(NET_Packet& net_packet)
{
    prepare_objects_for_save();

    shared_str game_name;
    net_packet.r_stringZ(game_name);
    save(game_name.c_str(), !!net_packet.r_u8());
}

void CALifeStorageManager::prepare_objects_for_save()
{
    // Spawn stage 2 (SPEC.md 5.A): M_SPAWN still waiting in the client queue (spawn budget) are logged only. The queue
    // is not drained here: a save may come from the Game thread, parallel to the render, where no object may be
    // created; and nothing is lost - such an object has no client state yet, the server saves it as it would spawn.
    Level().SpawnQueueNoteSave("save");
    Level().ClientSend();
    Level().ClientSave();
}
