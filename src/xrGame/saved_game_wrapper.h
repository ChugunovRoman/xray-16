////////////////////////////////////////////////////////////////////////////
//	Module 		: saved_game_wrapper.h
//	Created 	: 21.02.2006
//  Modified 	: 21.02.2006
//	Author		: Dmitriy Iassenev
//	Description : saved game wrapper class
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "alife_space.h"
#include "xrAICore/Navigation/game_graph_space.h"
#include "addon_data_bus.h"

class CSavedGameWrapper
{
public:
    typedef ALife::_TIME_ID _TIME_ID;
    typedef GameGraph::_LEVEL_ID _LEVEL_ID;

private:
    _TIME_ID m_game_time;
    _LEVEL_ID m_level_id;
    shared_str m_level_name;
    float m_actor_health;
    // Persistent values of the data bus in the save (gw::addons::data::ReadSaveValues), only bool, int, number and
    // string ones; empty for an invalid save
    gw::addons::data::SaveValues m_data;

public:
    CSavedGameWrapper(LPCSTR saved_game_name);
    static pcstr saved_game_full_name(pcstr saved_game_name, string_path& result, pcstr extension);
    static bool saved_game_exist(LPCSTR saved_game_name);
    static bool valid_saved_game(IReader& stream);
    static bool valid_saved_game(LPCSTR saved_game_name);
    // The spawn the save was made on (SPAWN_CHUNK_DATA, CALifeSpawnRegistry::save): its name and the GUID of its
    // header. valid - both were read
    struct SavedSpawn
    {
        xr_string name;
        xrGUID guid{};
        bool valid = false;
    };
    // Only the persistent values of the data bus of a save (what data() gives), without the actor and the level:
    // for the check before a load (gw::CanLoadSaveLevelPack). false for a missing or invalid save (`out` empty).
    // spawn: also the spawn of the save (gw::ReadSaveContent compares its GUID with all.spawn of the active pack)
    static bool saved_game_data(pcstr saved_game_name, gw::addons::data::SaveValues& out, SavedSpawn* spawn = nullptr);
    inline const _TIME_ID& game_time() const;
    inline const _LEVEL_ID& level_id() const;
    inline LPCSTR level_name() const;
    inline const float& actor_health() const;
    // A persistent value of the data bus in the save (Lua: data(key)); nullptr when there is no such key
    const GwpValue* data(pcstr key) const;

private:
    // The data bus chunk of the uncompressed stream into `out`, only bool, int, number and string values
    static void read_data(IReader& reader, gw::addons::data::SaveValues& out);

    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

#include "saved_game_wrapper_inline.h"
