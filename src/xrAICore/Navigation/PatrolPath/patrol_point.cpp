////////////////////////////////////////////////////////////////////////////
//	Module 		: patrol_point.cpp
//	Created 	: 15.06.2004
//  Modified 	: 15.06.2004
//	Author		: Dmitriy Iassenev
//	Description : Patrol point
////////////////////////////////////////////////////////////////////////////

#include "pch.hpp"

#include "patrol_point.h"
#include "Navigation/level_graph.h"
#include "Navigation/level_graph.h"
#include "Navigation/game_level_cross_table.h"
#include "Navigation/game_graph.h"
#ifdef DEBUG
#include "Navigation/PatrolPath/patrol_path.h"
#endif

#include "AISpaceBase.hpp"
#include "Common/object_broker.h"
#include "Navigation/ai_graph_engine_cvars.h"

CPatrolPoint::CPatrolPoint(const CPatrolPath* path)
    : m_flags(0), m_level_vertex_id(u32(-1)), m_game_vertex_id(GameGraph::_GRAPH_ID(-1))
#ifdef DEBUG
    , m_initialized(false), m_path(path)
#endif
{
    reset_remap_cache();
}

CPatrolPoint::CPatrolPoint(const CPatrolPoint& other)
    : ISerializable(other), m_name(other.m_name), m_position(other.m_position), m_flags(other.m_flags),
      m_level_vertex_id(other.m_level_vertex_id), m_game_vertex_id(other.m_game_vertex_id),
      m_remapped_level_vertex_id(other.m_remapped_level_vertex_id.load(std::memory_order_relaxed)),
      m_remap_level_id(other.m_remap_level_id.load(std::memory_order_relaxed))
#ifdef DEBUG
      , m_initialized(other.m_initialized), m_path(other.m_path)
#endif
{
}

CPatrolPoint& CPatrolPoint::operator=(const CPatrolPoint& other)
{
    if (this == &other)
        return *this;

    m_name = other.m_name;
    m_position = other.m_position;
    m_flags = other.m_flags;
    m_level_vertex_id = other.m_level_vertex_id;
    m_game_vertex_id = other.m_game_vertex_id;
    m_remapped_level_vertex_id.store(
        other.m_remapped_level_vertex_id.load(std::memory_order_relaxed), std::memory_order_relaxed);
    m_remap_level_id.store(other.m_remap_level_id.load(std::memory_order_relaxed), std::memory_order_release);
#ifdef DEBUG
    m_initialized = other.m_initialized;
    m_path = other.m_path;
#endif
    return *this;
}

#ifdef DEBUG
void CPatrolPoint::verify_vertex_id(
    const CLevelGraph* level_graph, const CGameLevelCrossTable* cross, const CGameGraph* game_graph) const
{
    if (!level_graph)
        return;

    if (level_graph->valid_vertex_id(m_level_vertex_id))
    {
        return;
    }

    VERIFY(m_path);
    string1024 temp;
    xr_sprintf(temp, "\n! Patrol point %s in path %s is not on the level graph vertex!", m_name.c_str(), m_path->m_name.c_str());
    THROW2(level_graph->valid_vertex_id(m_level_vertex_id), temp);
}
#endif

void CPatrolPoint::correct_position(
    const CLevelGraph* level_graph, const CGameLevelCrossTable* cross, const CGameGraph* game_graph)
{
    if (!level_graph || !level_graph->valid_vertex_position(position()) ||
        !level_graph->valid_vertex_id(m_level_vertex_id))
        return;

    if (!level_graph->inside(level_vertex_id(level_graph, cross, game_graph), position()))
        m_position = level_graph->vertex_position(level_vertex_id(level_graph, cross, game_graph));

    m_game_vertex_id = cross->vertex(level_vertex_id(level_graph, cross, game_graph)).game_vertex_id();
}

CPatrolPoint::CPatrolPoint(const CLevelGraph* level_graph, const CGameLevelCrossTable* cross,
    const CGameGraph* game_graph, const CPatrolPath* path, const Fvector& position, u32 level_vertex_id, u32 flags,
    shared_str name)
    : m_name(name)
{
    reset_remap_cache();
#ifdef DEBUG
    VERIFY(path);
    m_path = path;
#endif
    m_position = position;
    m_level_vertex_id = level_vertex_id;
    m_flags = flags;
#ifdef DEBUG
    m_initialized = true;
#endif
    correct_position(level_graph, cross, game_graph);
}

CPatrolPoint& CPatrolPoint::load_raw(
    const CLevelGraph* level_graph, const CGameLevelCrossTable* cross, const CGameGraph* game_graph, IReader& stream)
{
    stream.r_fvector3(m_position);
    m_flags = stream.r_u32();
    stream.r_stringZ(m_name);
    if (level_graph && level_graph->valid_vertex_position(m_position))
    {
        Fvector position = m_position;
        position.y += .15f;
        m_level_vertex_id = level_graph->vertex_id(position);
    }
    else
        m_level_vertex_id = u32(-1);
#ifdef DEBUG
    m_initialized = true;
#endif
    correct_position(level_graph, cross, game_graph);
    return (*this);
}

void CPatrolPoint::load(IReader& stream)
{
    load_data(m_name, stream);
    load_data(m_position, stream);
    load_data(m_flags, stream);
    load_data(m_level_vertex_id, stream);
    load_data(m_game_vertex_id, stream);

    // all.spawn data is the main source of stale vertex ids, drop any previously cached remap
    reset_remap_cache();

#ifdef DEBUG
    m_initialized = true;
#endif
}

void CPatrolPoint::save(IWriter& stream)
{
    save_data(m_name, stream);
    save_data(m_position, stream);
    save_data(m_flags, stream);
    save_data(m_level_vertex_id, stream);
    save_data(m_game_vertex_id, stream);
}

u32 CPatrolPoint::level_vertex_id() const
{
    const CLevelGraph& levelGraph = GEnv.AISpace->level_graph();

    // fast path: the stored id is valid for the loaded level.ai and covers the point position
    // (covers 99.9% of calls; a stale id that merely fits the range but points elsewhere is rejected too)
    if (levelGraph.valid_vertex_id(m_level_vertex_id) && levelGraph.inside(m_level_vertex_id, m_position))
        return m_level_vertex_id;

    // kill-switch: restore the old behaviour, callers assert on the invalid id themselves
    if (!ps_ai_patrol_remap_invalid_vertex)
        return m_level_vertex_id;

    // A point of another level keeps an id which is correct for ITS level.ai, and the offline
    // ALife (CALifeSmartTerrainTask / CALifeMonsterPatrolPathManager) feeds exactly such ids to
    // CALifeMonsterDetailPathManager::target for squads on levels that are not loaded. Remapping
    // them against the loaded level graph would always fail and return u32(-1), and then
    // CALifeMonsterDetailPathManager::completed() would never match m_tNodeID again.
    const CGameGraph& gameGraph = GEnv.AISpace->game_graph();
    if (gameGraph.valid_vertex_id(m_game_vertex_id) &&
        gameGraph.vertex(m_game_vertex_id)->level_id() != levelGraph.level_id())
        return m_level_vertex_id;

    const GameGraph::_LEVEL_ID levelId = levelGraph.level_id();

    // cache hit for the loaded level; a failed lookup is cached as u32(-1) as well,
    // so a hopeless point is not retried on every call.
    // acquire pairs with the release store of the level tag below: the tag is published last,
    // so seeing it guarantees the remapped id next to it is already visible
    if (m_remap_level_id.load(std::memory_order_acquire) == levelId)
        return m_remapped_level_vertex_id.load(std::memory_order_relaxed);

    u32 newVertexId = u32(-1);
    if (levelGraph.valid_vertex_position(m_position))
    {
        // O(log N) lookup only; never fall back to vertex(u32(-1), position) here:
        // it degrades into a full linear scan over every vertex of the level
        newVertexId = levelGraph.vertex_id(m_position);
        if (!levelGraph.valid_vertex_id(newVertexId))
        {
            // retry slightly above the position (e.g. the point sits a bit under the floor), same as load_raw
            Fvector position = m_position;
            position.y += .15f;
            if (levelGraph.valid_vertex_position(position))
                newVertexId = levelGraph.vertex_id(position);
        }
    }

    // the level tag is published last, with release ordering, so that a concurrent reader which
    // sees the tag is guaranteed to see the remapped id written above and not a stale one
    m_remapped_level_vertex_id.store(newVertexId, std::memory_order_relaxed);
    m_remap_level_id.store(levelId, std::memory_order_release);

    // the first remap of a session is logged unconditionally: it is the main diagnostic
    // for a level.ai / all.spawn mismatch after a mod update
    static std::atomic<bool> s_firstRemapLogged{false};
    if (ps_ai_patrol_remap_log || !s_firstRemapLogged.exchange(true))
        Msg("~ [GW] patrol point remap: point[%s] position[%f][%f][%f] level_vertex_id[%u] -> [%u]", m_name.c_str(),
            VPUSH(m_position), m_level_vertex_id, newVertexId);

    return newVertexId;
}

const GameGraph::_GRAPH_ID& CPatrolPoint::game_vertex_id() const
{
    const CGameGraph& gameGraph = GEnv.AISpace->game_graph();
    const CLevelGraph& levelGraph = GEnv.AISpace->level_graph();
    // CGameGraph::vertex() does not bounds-check on its own; a stale all.spawn may store
    // an id beyond the current game graph, which would read garbage memory on dereference
    if (!gameGraph.valid_vertex_id(m_game_vertex_id))
    {
        static std::atomic<u32> s_invalidGameVertexIdLogs{0};
        if (s_invalidGameVertexIdLogs.fetch_add(1) < 16)
            Msg("! [GW] patrol point[%s] position[%f][%f][%f] has invalid game_vertex_id[%u] for the loaded game graph",
                m_name.c_str(), VPUSH(m_position), u32(u16(m_game_vertex_id)));
        return m_game_vertex_id;
    }
    const CGameGraph::CGameVertex* vertex = gameGraph.vertex(m_game_vertex_id);
    if (vertex->level_id() == levelGraph.level_id())
        return game_vertex_id(&levelGraph, &gameGraph.cross_table(), &gameGraph);
    return m_game_vertex_id;
}
