////////////////////////////////////////////////////////////////////////////
//	Module 		: patrol_point.h
//	Created 	: 15.06.2004
//  Modified 	: 15.06.2004
//	Author		: Dmitriy Iassenev
//	Description : Patrol point
////////////////////////////////////////////////////////////////////////////

#pragma once

#include <atomic>

class CPatrolPath;
class CLevelGraph;
class CGameLevelCrossTable;
class CGameGraph;

#include "Common/object_interfaces.h"
#include "xrAICore/Navigation/game_graph_space.h"

class XRAICORE_API CPatrolPoint : public ISerializable
{
protected:
    shared_str m_name;
    Fvector m_position;
    u32 m_flags;
    u32 m_level_vertex_id;
    GameGraph::_GRAPH_ID m_game_vertex_id;

private:
    // Lazy level_vertex_id remap cache (see plans/patrol_point_remap/plan.md).
    // level_vertex_id() may be called from scheduler worker threads, so the cache uses atomics:
    // a duplicate computation is harmless, a torn or half-published read is not. The level tag is
    // always stored last with release ordering and read with acquire, so a reader that sees the tag
    // also sees the remapped id that belongs to it.
    // m_remapped_level_vertex_id == u32(-1) means "not computed" or "remap failed" (cached failure).
    // m_remap_level_id == _LEVEL_ID(-1) means "cache is empty", otherwise the cache holds the
    // value computed for that level id, so a level change invalidates the cache automatically.
    mutable std::atomic<u32> m_remapped_level_vertex_id;
    mutable std::atomic<GameGraph::_LEVEL_ID> m_remap_level_id;

protected:
#ifdef DEBUG
    bool m_initialized;
    const CPatrolPath* m_path;
#endif

private:
    IC void reset_remap_cache() const;
    void correct_position(
        const CLevelGraph* level_graph, const CGameLevelCrossTable* cross, const CGameGraph* game_graph);
#ifdef DEBUG
    void verify_vertex_id(
        const CLevelGraph* level_graph, const CGameLevelCrossTable* cross, const CGameGraph* game_graph) const;
#endif

public:
    CPatrolPoint(const CLevelGraph* level_graph, const CGameLevelCrossTable* cross, const CGameGraph* game_graph,
        const CPatrolPath* path, const Fvector& position, u32 level_vertex_id, u32 flags, shared_str name);
    CPatrolPoint(const CPatrolPath* path = 0);
    // atomic cache members delete the implicit copy constructor and copy assignment, restore both
    // manually: points are copied by CGraphAbstract::add_vertex and assigned by CGraphVertex::data()
    CPatrolPoint(const CPatrolPoint& other);
    CPatrolPoint& operator=(const CPatrolPoint& other);
    inline bool operator==(const CPatrolPoint& rhs) const;
    virtual void load(IReader& stream);
    virtual void save(IWriter& stream);
    CPatrolPoint& load_raw(const CLevelGraph* level_graph, const CGameLevelCrossTable* cross,
        const CGameGraph* game_graph, IReader& stream);
    IC const Fvector& position() const;
    IC const u32& level_vertex_id(
        const CLevelGraph* level_graph, const CGameLevelCrossTable* cross, const CGameGraph* game_graph) const;
    IC const GameGraph::_GRAPH_ID& game_vertex_id(
        const CLevelGraph* level_graph, const CGameLevelCrossTable* cross, const CGameGraph* game_graph) const;
    IC const u32& flags() const;
    IC const shared_str& name() const;
    // for xrGame; returns by value: may lazily remap the stored vertex id for the loaded level.ai
    u32 level_vertex_id() const;
    // for xrGame
    const GameGraph::_GRAPH_ID& game_vertex_id() const;

#ifdef DEBUG
public:
    IC void path(const CPatrolPath* path);
#endif
};

#include "xrAICore/Navigation/PatrolPath/patrol_point_inline.h"
