#include "StdAfx.h"

#include "spawn_smart_terrain_map_data_source.h"

#include <atomic>
#include <memory>
#include <thread>

#include "Common/object_loader.h"
#include "xrAICore/Navigation/game_graph.h"
#include "xrCore/Threading/ThreadUtil.h"
#include "xrEngine/Engine.h"
#include "xrServerEntities/xrMessages.h"

namespace
{
template <typename T>
struct XrDelete
{
    void operator()(T* value) const { xr_delete(value); }
};

constexpr pcstr SMART_TERRAIN_MAP_READY_EVENT = "SMART_TERRAIN_MAP:ready";
constexpr pcstr SMART_PROPS_FILE = "misc\\simulation_objects_props.ltx";
constexpr pcstr SMART_DEFAULT_SPAWN_FILE = "misc\\simulations\\default.ltx";
constexpr pcstr SMART_DEFAULT_CUSTOM_SPAWN_FILE = "misc\\simulations\\default_custom.ltx";
constexpr pcstr SMART_OPTIONS_FILE = "axr_options.ltx";

class CScopedGameGraph final
{
public:
    ~CScopedGameGraph()
    {
        xr_delete(m_graph);

        if (!m_reader)
            return;

        if (m_reader_from_spawn)
            m_reader->close();
        else
            FS.r_close(m_reader);
    }

    const CGameGraph* Load(IReader& spawn_file)
    {
        IReader* embedded_chunk = spawn_file.open_chunk(4);
        if (embedded_chunk)
        {
            m_reader = embedded_chunk;
            m_reader_from_spawn = true;
            m_graph = xr_new<CGameGraph>(*embedded_chunk);
            return m_graph;
        }

        string_path graph_file_name;
        FS.update_path(graph_file_name, "$game_data$", GRAPH_NAME);
        if (!FS.exist(graph_file_name))
            return nullptr;

        m_reader = FS.r_open(graph_file_name);
        if (!m_reader)
            return nullptr;

        m_reader_from_spawn = false;
        m_graph = xr_new<CGameGraph>(*m_reader);
        return m_graph;
    }

private:
    CGameGraph* m_graph{};
    IReader* m_reader{};
    bool m_reader_from_spawn = false;
};

bool resolve_level_name(GameGraph::_GRAPH_ID graph_id, shared_str& out_level_name, const CGameGraph* graph)
{
    if (!graph)
        return false;

    if (graph_id == GameGraph::_GRAPH_ID(-1))
        return false;

    if (!graph->valid_vertex_id(graph_id))
        return false;

    const CGameGraph::CGameVertex* game_vertex = graph->vertex(graph_id);
    if (!game_vertex)
        return false;

    const GameGraph::_LEVEL_ID level_id = game_vertex->level_id();
    if (!graph->header().level_exist(level_id))
        return false;

    out_level_name = graph->header().level(level_id).name().c_str();
    return true;
}

bool is_smart_terrain_section(pcstr section_name)
{
    if (!section_name || !section_name[0] || !pSettings->section_exist(section_name))
        return false;

    if (!pSettings->line_exist(section_name, "class"))
        return false;

    return xr_stricmp(pSettings->r_string(section_name, "class"), "SMRTTRRN") == 0;
}

xr_string trim_copy(pcstr value)
{
    if (!value)
        return {};

    xr_string result = value;
    const auto begin = result.find_first_not_of(" \t\r\n");
    if (begin == xr_string::npos)
        return {};

    const auto end = result.find_last_not_of(" \t\r\n");
    return result.substr(begin, end - begin + 1);
}

shared_str translate_smart_name(pcstr smart_name)
{
    if (!smart_name || !xr_strlen(smart_name))
        return nullptr;

    return StringTable().translate(smart_name).c_str();
}

pcstr detect_smart_type(const CInifile& ini, pcstr section_name)
{
    if (!section_name || !ini.section_exist(section_name))
        return nullptr;

    static constexpr pcstr smartTypes[] = {"resource", "base", "camp", "point", "lair", "territory"};
    for (pcstr smartType : smartTypes)
    {
        if (ini.line_exist(section_name, smartType) && ini.r_bool(section_name, smartType))
            return smartType;
    }

    return nullptr;
}

shared_str extract_default_owner_faction(const CInifile& ini, pcstr section_name)
{
    if (!section_name || !ini.section_exist(section_name))
        return nullptr;

    const u32 lineCount = ini.line_count(section_name);
    for (u32 idx = 0; idx < lineCount; ++idx)
    {
        pcstr lineName = nullptr;
        pcstr lineValue = nullptr;
        if (!ini.r_line(section_name, idx, &lineName, &lineValue) || !lineValue)
            continue;

        pcstr comma = strchr(lineValue, ',');
        if (!comma)
            continue;

        const xr_string faction = trim_copy(comma + 1);
        if (!faction.empty())
            return faction.c_str();
    }

    return nullptr;
}

shared_str resolve_icon_texture(const CInifile* options_ini, pcstr owner_faction, pcstr smart_type)
{
    if (owner_faction && xr_strlen(owner_faction))
    {
        xr_string icon = "ui\\icons\\patches\\";
        icon += owner_faction;
        return icon.c_str();
    }

    const pcstr typeName = smart_type && xr_strlen(smart_type) ? smart_type : "territory";
    const int style = options_ini ? options_ini->read_if_exists<int>("mm_options", "pda_smart_icons_color", 0) : 0;

    xr_string icon = "ui\\icons\\empty_smarts\\";
    icon += xr_string().append(std::to_string(style).c_str());
    icon += "\\";
    icon += typeName;
    return icon.c_str();
}

u32 resolve_icon_color(const CInifile* options_ini, pcstr owner_faction, pcstr smart_type)
{
    if (owner_faction && xr_strlen(owner_faction))
        return color_rgba(255, 255, 255, 255);

    if (!options_ini)
        return color_rgba(255, 255, 255, 255);

    const pcstr typeName = smart_type && xr_strlen(smart_type) ? smart_type : "territory";
    xr_string enableKey = "enable_pda_smart_";
    enableKey += typeName;
    enableKey += "_enabled_color";
    if (!options_ini->read_if_exists<bool>("mm_options", enableKey.c_str(), false))
        return color_rgba(255, 255, 255, 255);

    xr_string colorKey = "pda_smart_";
    colorKey += typeName;

    xr_string keyR = colorKey + "_color_r";
    xr_string keyG = colorKey + "_color_g";
    xr_string keyB = colorKey + "_color_b";

    const int r = options_ini->read_if_exists<int>("mm_options", keyR.c_str(), 255);
    const int g = options_ini->read_if_exists<int>("mm_options", keyG.c_str(), 255);
    const int b = options_ini->read_if_exists<int>("mm_options", keyB.c_str(), 255);
    return color_rgba(255, r, g, b);
}

shared_str build_hint_text(const SMapPointDesc& point)
{
    xr_string hint;

    const pcstr displayName = point.display_name.c_str();
    const pcstr smartName = point.smart_name.c_str();
    const pcstr levelName = point.level_name.c_str();

    if (displayName && xr_strlen(displayName))
        hint = displayName;
    else if (smartName && xr_strlen(smartName))
        hint = smartName;
    else
        hint = "smart_terrain";

    if (smartName && xr_strlen(smartName) && (!displayName || xr_strcmp(displayName, smartName) != 0))
    {
        hint += "\n";
        hint += smartName;
    }

    if (levelName && xr_strlen(levelName))
    {
        hint += "\n";
        hint += levelName;
    }

    return hint.c_str();
}

bool line_key_is_leader_squad(pcstr key)
{
    if (!key || !key[0])
        return false;

    static constexpr pcstr suffix = "_sim_squad_leader";
    const size_t key_len = xr_strlen(key);
    const size_t suffix_len = xr_strlen(suffix);
    if (key_len < suffix_len)
        return false;

    return xr_strcmp(key + key_len - suffix_len, suffix) == 0;
}

bool section_has_leader_squad(const CInifile& ini, pcstr section_name)
{
    if (!section_name || !ini.section_exist(section_name))
        return false;

    const u32 lineCount = ini.line_count(section_name);
    for (u32 idx = 0; idx < lineCount; ++idx)
    {
        pcstr lineName = nullptr;
        pcstr lineValue = nullptr;
        if (!ini.r_line(section_name, idx, &lineName, &lineValue))
            continue;

        if (line_key_is_leader_squad(lineName))
            return true;
    }

    return false;
}

void apply_leader_squad_flag(SMapPointDesc& point, const CInifile* defaultCustomSpawnIni, const CInifile* defaultSpawnIni)
{
    const bool hasCustomSection =
        defaultCustomSpawnIni && defaultCustomSpawnIni->section_exist(point.smart_name.c_str());

    bool hasLeader = false;
    if (hasCustomSection && defaultCustomSpawnIni)
        hasLeader = section_has_leader_squad(*defaultCustomSpawnIni, point.smart_name.c_str());
    else if (defaultSpawnIni && defaultSpawnIni->section_exist(point.smart_name.c_str()))
        hasLeader = section_has_leader_squad(*defaultSpawnIni, point.smart_name.c_str());

    if (hasLeader)
        point.flags |= eMapPointHasLeader;
    else
        point.flags &= ~eMapPointHasLeader;
}

void apply_simulation_owner_overrides(
    SMapPointDesc& point, const CInifile* defaultCustomSpawnIni, const CInifile* defaultSpawnIni, const CInifile* optionsIni)
{
    const bool hasCustomSection =
        defaultCustomSpawnIni && defaultCustomSpawnIni->section_exist(point.smart_name.c_str());
    if (hasCustomSection)
        point.owner_faction = extract_default_owner_faction(*defaultCustomSpawnIni, point.smart_name.c_str());
    // If smart exists in default_custom.ltx but has no lines, this is an explicit clear override.
    // In that case we must not fallback to default.ltx owner.
    if ((!point.owner_faction.c_str() || !point.owner_faction.c_str()[0]) && !hasCustomSection && defaultSpawnIni)
        point.owner_faction = extract_default_owner_faction(*defaultSpawnIni, point.smart_name.c_str());
    point.icon_texture = resolve_icon_texture(optionsIni, point.owner_faction.c_str(), point.smart_type.c_str());
    point.icon_color = resolve_icon_color(optionsIni, point.owner_faction.c_str(), point.smart_type.c_str());
    point.hint_text = build_hint_text(point);
    apply_leader_squad_flag(point, defaultCustomSpawnIni, defaultSpawnIni);
}

void load_simulation_override_inis(
    std::unique_ptr<CInifile, XrDelete<CInifile>>& defaultSpawnIni,
    std::unique_ptr<CInifile, XrDelete<CInifile>>& defaultCustomSpawnIni,
    std::unique_ptr<CInifile, XrDelete<CInifile>>& optionsIni)
{
    string_path default_spawn_file_name;
    string_path default_custom_spawn_file_name;
    string_path smart_options_file_name;
    FS.update_path(default_spawn_file_name, "$game_config$", SMART_DEFAULT_SPAWN_FILE);
    FS.update_path(default_custom_spawn_file_name, "$game_config$", SMART_DEFAULT_CUSTOM_SPAWN_FILE);
    FS.update_path(smart_options_file_name, "$game_config$", SMART_OPTIONS_FILE);

    defaultSpawnIni.reset();
    defaultCustomSpawnIni.reset();
    optionsIni.reset();

    if (FS.exist(default_spawn_file_name))
        defaultSpawnIni.reset(xr_new<CInifile>(default_spawn_file_name, true, true, false));
    if (FS.exist(default_custom_spawn_file_name))
        defaultCustomSpawnIni.reset(xr_new<CInifile>(default_custom_spawn_file_name, true, true, false));
    if (FS.exist(smart_options_file_name))
        optionsIni.reset(xr_new<CInifile>(smart_options_file_name, true, true, false));
}

bool read_spawn_packet(IReader& packet_chunk, NET_Packet& out_packet)
{
    const u16 packet_size = packet_chunk.r_u16();
    if (!packet_size || packet_size >= NET_PacketSizeLimit)
        return false;

    out_packet.B.count = packet_size;
    packet_chunk.r(out_packet.B.data, out_packet.B.count);
    out_packet.r_pos = 0;
    return true;
}

bool parse_spawn_packet_metadata(
    NET_Packet& packet, shared_str& out_section_name, shared_str& out_smart_name, Fvector& out_position,
    GameGraph::_GRAPH_ID& out_graph_id)
{
    u16 packet_id = u16(-1);
    packet.r_begin(packet_id);
    if (packet_id != M_SPAWN)
        return false;

    xr_string section_name;
    packet.r_stringZ(section_name);
    out_section_name = section_name.c_str();

    xr_string smart_name;
    packet.r_stringZ(smart_name);
    out_smart_name = smart_name.c_str();

    u8 temp_u8 = 0;
    packet.r_u8(temp_u8); // temp_gt
    packet.r_u8(temp_u8); // s_RP
    packet.r_vec3(out_position);

    Fvector temp_vec3;
    packet.r_vec3(temp_vec3); // angle

    u16 temp_u16 = 0;
    packet.r_u16(temp_u16); // RespawnTime
    packet.r_u16(temp_u16); // ID
    packet.r_u16(temp_u16); // ID_Parent
    packet.r_u16(temp_u16); // ID_Phantom

    u16 spawn_flags = 0;
    packet.r_u16(spawn_flags);

    u16 version = 0;
    if (spawn_flags & M_SPAWN_VERSION)
        packet.r_u16(version);

    if (!version)
        return false;

    if (version > 120)
        packet.r_u16(temp_u16); // game type

    if (version > 69)
        packet.r_u16(temp_u16); // script version

    if (version > 70)
    {
        const u16 client_data_size = version > 93 ? packet.r_u16() : packet.r_u8();
        packet.r_advance(client_data_size);
    }

    if (version > 79)
        packet.r_u16(temp_u16); // spawn id

    if (version < 112)
    {
        if (version > 82)
            packet.r_float(); // spawn probability

        if (version > 83)
        {
            u32 temp_u32 = 0;
            packet.r_u32(temp_u32); // spawn flags
            xr_string temp_string;
            packet.r_stringZ(temp_string); // spawn control
            packet.r_u32(temp_u32); // max spawn count
            packet.r_u32(temp_u32); // spawn count
        }

        if (version > 84)
        {
            u64 temp_u64 = 0;
            packet.r_u64(temp_u64); // min spawn interval
            packet.r_u64(temp_u64); // max spawn interval
        }
    }

    packet.r_u16(temp_u16); // state block size

    if (version >= 1)
    {
        if (version > 24)
        {
            if (version < 83)
                packet.r_float();
        }
        else
            packet.r_u8(temp_u8);

        if (version < 83)
        {
            u32 temp_u32 = 0;
            packet.r_u32(temp_u32);
        }

        if (version < 4)
            packet.r_u16(temp_u16);

        packet.r_u16(out_graph_id);
        packet.r_float(); // distance
        return true;
    }

    return false;
}

struct SSharedSmartTerrainSnapshot
{
    shared_str spawn_name;
    shared_str focus_level;
    xr_vector<SMapPointDesc> points;
};

bool load_spawn_smart_terrain_points(pcstr spawn_name, xr_vector<SMapPointDesc>& out_points, shared_str& out_focus_level)
{
    out_points.clear();
    out_focus_level = nullptr;

    if (!spawn_name || !xr_strlen(spawn_name))
        return false;

    string_path spawn_file_name;
    if (!FS.exist(spawn_file_name, "$game_spawn$", spawn_name, ".spawn"))
        return false;

    IReader* spawn_file = FS.r_open(spawn_file_name);
    if (!spawn_file)
        return false;

    string_path smart_props_file_name;
    string_path default_spawn_file_name;
    string_path default_custom_spawn_file_name;
    string_path smart_options_file_name;
    FS.update_path(smart_props_file_name, "$game_config$", SMART_PROPS_FILE);
    FS.update_path(default_spawn_file_name, "$game_config$", SMART_DEFAULT_SPAWN_FILE);
    FS.update_path(default_custom_spawn_file_name, "$game_config$", SMART_DEFAULT_CUSTOM_SPAWN_FILE);
    FS.update_path(smart_options_file_name, "$game_config$", SMART_OPTIONS_FILE);

    std::unique_ptr<CInifile, XrDelete<CInifile>> smartPropsIni;
    std::unique_ptr<CInifile, XrDelete<CInifile>> defaultSpawnIni;
    std::unique_ptr<CInifile, XrDelete<CInifile>> defaultCustomSpawnIni;
    std::unique_ptr<CInifile, XrDelete<CInifile>> optionsIni;
    if (FS.exist(smart_props_file_name))
        smartPropsIni.reset(xr_new<CInifile>(smart_props_file_name, true, true, false));
    if (FS.exist(default_spawn_file_name))
        defaultSpawnIni.reset(xr_new<CInifile>(default_spawn_file_name, true, true, false));
    if (FS.exist(default_custom_spawn_file_name))
        defaultCustomSpawnIni.reset(xr_new<CInifile>(default_custom_spawn_file_name, true, true, false));
    if (FS.exist(smart_options_file_name))
        optionsIni.reset(xr_new<CInifile>(smart_options_file_name, true, true, false));

    bool parsed = false;
    {
        CScopedGameGraph graph_holder;
        const CGameGraph* graph = graph_holder.Load(*spawn_file);
        if (graph)
        {
            IReader* spawn_graph_chunk = spawn_file->open_chunk(1);
            if (spawn_graph_chunk)
            {
                IReader* vertices_chunk = spawn_graph_chunk->open_chunk(1);
                if (!vertices_chunk)
                {
                    spawn_graph_chunk->close();
                    FS.r_close(spawn_file);
                    return false;
                }

                u32 chunk_id = 0;
                for (IReader* vertex_chunk = vertices_chunk->open_chunk_iterator(chunk_id); vertex_chunk;
                     vertex_chunk = vertices_chunk->open_chunk_iterator(chunk_id, vertex_chunk))
                {
                    u16 vertex_id = u16(-1);
                    if (IReader* vertex_id_chunk = vertex_chunk->open_chunk(0))
                    {
                        load_data(vertex_id, *vertex_id_chunk);
                        vertex_id_chunk->close();
                    }

                    IReader* wrapper_chunk = vertex_chunk->open_chunk(1);
                    if (!wrapper_chunk)
                        continue;

                    IReader* spawn_packet_chunk = wrapper_chunk->open_chunk(0);
                    if (!spawn_packet_chunk)
                    {
                        wrapper_chunk->close();
                        continue;
                    }

                    NET_Packet packet;
                    shared_str section_name;
                    shared_str smart_name;
                    Fvector position;
                    GameGraph::_GRAPH_ID graph_id = GameGraph::_GRAPH_ID(-1);

                    const bool packet_ok = read_spawn_packet(*spawn_packet_chunk, packet) &&
                        parse_spawn_packet_metadata(packet, section_name, smart_name, position, graph_id) &&
                        is_smart_terrain_section(section_name.c_str());

                    spawn_packet_chunk->close();
                    wrapper_chunk->close();

                    if (!packet_ok)
                        continue;

                    SMapPointDesc point;
                    if (!resolve_level_name(graph_id, point.level_name, graph))
                        continue;

                    point.position = position;
                    point.spot_type = "smart_terrain";
                    point.logical_id = static_cast<u32>(vertex_id);
                    point.flags = 0;
                    point.section_name = section_name;
                    point.smart_name = smart_name;
                    point.display_name = translate_smart_name(point.smart_name.c_str());
                    if (smartPropsIni)
                        point.smart_type = detect_smart_type(*smartPropsIni, point.smart_name.c_str());
                    apply_simulation_owner_overrides(
                        point, defaultCustomSpawnIni.get(), defaultSpawnIni.get(), optionsIni.get());

                    out_points.push_back(point);
                }

                vertices_chunk->close();
                spawn_graph_chunk->close();
                parsed = true;
            }
        }
    }

    FS.r_close(spawn_file);

    if (!out_points.empty())
        out_focus_level = out_points.front().level_name;

    return parsed;
}

class CSharedSmartTerrainMapCache final
{
public:
    static CSharedSmartTerrainMapCache& Instance()
    {
        static CSharedSmartTerrainMapCache instance;
        return instance;
    }

    void StartLoading(pcstr spawn_name)
    {
        if (!spawn_name || !xr_strlen(spawn_name))
            return;

        if (m_worker.joinable() && !m_loading.load(std::memory_order_acquire))
            m_worker.join();

        if (m_loading.load(std::memory_order_acquire))
        {
            if (xr_strcmp(m_requested_spawn_name.c_str(), spawn_name) == 0)
                return;

            return;
        }

        if (m_published && xr_strcmp(m_published->spawn_name.c_str(), spawn_name) == 0)
            return;

        if (SSharedSmartTerrainSnapshot* pending = m_pending_snapshot.load(std::memory_order_acquire))
        {
            if (xr_strcmp(pending->spawn_name.c_str(), spawn_name) == 0)
                return;
        }

        m_requested_spawn_name = spawn_name;
        delete m_pending_snapshot.exchange(nullptr, std::memory_order_acq_rel);

        xr_string requested_spawn_name = spawn_name;
        m_loading.store(true, std::memory_order_release);

        m_worker = Threading::RunThread("smart_terrain_map", [this, requested_spawn_name]()
        {
            auto* snapshot = xr_new<SSharedSmartTerrainSnapshot>();
            snapshot->spawn_name = requested_spawn_name.c_str();

            load_spawn_smart_terrain_points(requested_spawn_name.c_str(), snapshot->points, snapshot->focus_level);

            delete m_pending_snapshot.exchange(snapshot, std::memory_order_acq_rel);
            m_loading.store(false, std::memory_order_release);
            Engine.Event.Defer(SMART_TERRAIN_MAP_READY_EVENT);
        });
    }

    void PublishPending()
    {
        std::unique_ptr<SSharedSmartTerrainSnapshot> pending(
            m_pending_snapshot.exchange(nullptr, std::memory_order_acq_rel));
        if (!pending)
            return;

        m_published = std::move(pending);
        ++m_revision;
    }

    void Shutdown()
    {
        if (m_worker.joinable())
            m_worker.join();

        delete m_pending_snapshot.exchange(nullptr, std::memory_order_acq_rel);
        m_loading.store(false, std::memory_order_release);
    }

    void Enumerate(pcstr spawn_name, xr_vector<SMapPointDesc>& out) const
    {
        out.clear();
        if (!m_published || xr_strcmp(m_published->spawn_name.c_str(), spawn_name) != 0)
            return;

        out = m_published->points;
    }

    bool GetFocusLevel(pcstr spawn_name, shared_str& out_level) const
    {
        if (!m_published || xr_strcmp(m_published->spawn_name.c_str(), spawn_name) != 0 ||
            !m_published->focus_level.size())
        {
            return false;
        }

        out_level = m_published->focus_level;
        return true;
    }

    u32 GetRevision(pcstr spawn_name) const
    {
        if (!m_published || xr_strcmp(m_published->spawn_name.c_str(), spawn_name) != 0)
            return 0;

        return m_revision;
    }

    bool UpdatePublishedPoint(pcstr spawn_name, u32 logical_id, pcstr owner_faction, pcstr icon_texture)
    {
        if (!m_published || xr_strcmp(m_published->spawn_name.c_str(), spawn_name) != 0)
            return false;

        std::unique_ptr<CInifile, XrDelete<CInifile>> defaultSpawnIni;
        std::unique_ptr<CInifile, XrDelete<CInifile>> defaultCustomSpawnIni;
        std::unique_ptr<CInifile, XrDelete<CInifile>> optionsIni;
        load_simulation_override_inis(defaultSpawnIni, defaultCustomSpawnIni, optionsIni);

        for (auto& point : m_published->points)
        {
            if (point.logical_id != logical_id)
                continue;

            point.owner_faction = owner_faction ? owner_faction : "";
            if (icon_texture && icon_texture[0])
                point.icon_texture = icon_texture;
            else
                point.icon_texture =
                    resolve_icon_texture(optionsIni.get(), point.owner_faction.c_str(), point.smart_type.c_str());
            point.icon_color =
                resolve_icon_color(optionsIni.get(), point.owner_faction.c_str(), point.smart_type.c_str());
            point.hint_text = build_hint_text(point);
            apply_leader_squad_flag(point, defaultCustomSpawnIni.get(), defaultSpawnIni.get());
            return true;
        }

        return false;
    }

    void RefreshPublishedFromSimulationLtx(pcstr spawn_name)
    {
        if (!m_published || xr_strcmp(m_published->spawn_name.c_str(), spawn_name) != 0)
            return;

        std::unique_ptr<CInifile, XrDelete<CInifile>> defaultSpawnIni;
        std::unique_ptr<CInifile, XrDelete<CInifile>> defaultCustomSpawnIni;
        std::unique_ptr<CInifile, XrDelete<CInifile>> optionsIni;
        load_simulation_override_inis(defaultSpawnIni, defaultCustomSpawnIni, optionsIni);

        for (auto& point : m_published->points)
        {
            apply_simulation_owner_overrides(
                point, defaultCustomSpawnIni.get(), defaultSpawnIni.get(), optionsIni.get());
        }
    }

private:
    CSharedSmartTerrainMapCache() = default;

private:
    shared_str m_requested_spawn_name;
    std::thread m_worker;
    std::atomic<SSharedSmartTerrainSnapshot*> m_pending_snapshot{};
    std::unique_ptr<SSharedSmartTerrainSnapshot> m_published;
    std::atomic<bool> m_loading{ false };
    u32 m_revision = 0;
};
} // namespace

CSpawnSmartTerrainMapDataSource::CSpawnSmartTerrainMapDataSource(pcstr spawn_name)
    : m_spawn_name(spawn_name)
{
}

void CSpawnSmartTerrainMapDataSource::StartSharedLoading(pcstr spawn_name)
{
    CSharedSmartTerrainMapCache::Instance().StartLoading(spawn_name);
}

void CSpawnSmartTerrainMapDataSource::PublishSharedDataIfReady()
{
    CSharedSmartTerrainMapCache::Instance().PublishPending();
}

void CSpawnSmartTerrainMapDataSource::ShutdownSharedLoading()
{
    CSharedSmartTerrainMapCache::Instance().Shutdown();
}

void CSpawnSmartTerrainMapDataSource::SetSpawnName(pcstr spawn_name)
{
    m_spawn_name = spawn_name;
}

void CSpawnSmartTerrainMapDataSource::Reload()
{
    CSharedSmartTerrainMapCache::Instance().RefreshPublishedFromSimulationLtx(m_spawn_name.c_str());
}

bool CSpawnSmartTerrainMapDataSource::UpdatePointVisual(u32 logical_id, pcstr owner_faction, pcstr icon_texture)
{
    return CSharedSmartTerrainMapCache::Instance().UpdatePublishedPoint(
        m_spawn_name.c_str(), logical_id, owner_faction, icon_texture);
}

void CSpawnSmartTerrainMapDataSource::EnumeratePoints(xr_vector<SMapPointDesc>& out) const
{
    CSharedSmartTerrainMapCache::Instance().Enumerate(m_spawn_name.c_str(), out);
}

u32 CSpawnSmartTerrainMapDataSource::GetDataRevision() const
{
    return CSharedSmartTerrainMapCache::Instance().GetRevision(m_spawn_name.c_str());
}

bool CSpawnSmartTerrainMapDataSource::GetFocusLevel(shared_str& outLevel) const
{
    return CSharedSmartTerrainMapCache::Instance().GetFocusLevel(m_spawn_name.c_str(), outLevel);
}
