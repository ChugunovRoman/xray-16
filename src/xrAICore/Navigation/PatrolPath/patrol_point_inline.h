////////////////////////////////////////////////////////////////////////////
//	Module 		: patrol_point_inline.h
//	Created 	: 15.06.2004
//  Modified 	: 15.06.2004
//	Author		: Dmitriy Iassenev
//	Description : Patrol point inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

inline bool CPatrolPoint::operator==(const CPatrolPoint& rhs) const
{
    R_ASSERT(!"not implemented");
    return false;
}

IC void CPatrolPoint::reset_remap_cache() const
{
    // same publication order as in level_vertex_id(): the level tag goes last, with release
    m_remapped_level_vertex_id.store(u32(-1), std::memory_order_relaxed);
    m_remap_level_id.store(GameGraph::_LEVEL_ID(-1), std::memory_order_release);
}

IC const Fvector& CPatrolPoint::position() const
{
    VERIFY(m_initialized);
    return (m_position);
}

IC const u32& CPatrolPoint::flags() const
{
    VERIFY(m_initialized);
    return (m_flags);
}

IC const shared_str& CPatrolPoint::name() const
{
    VERIFY(m_initialized);
    return (m_name);
}

IC const u32& CPatrolPoint::level_vertex_id(
    const CLevelGraph* level_graph, const CGameLevelCrossTable* cross, const CGameGraph* game_graph) const
{
    VERIFY(m_initialized);
#ifdef DEBUG
    verify_vertex_id(level_graph, cross, game_graph);
#endif
    return (m_level_vertex_id);
}

IC const GameGraph::_GRAPH_ID& CPatrolPoint::game_vertex_id(
    const CLevelGraph* level_graph, const CGameLevelCrossTable* cross, const CGameGraph* game_graph) const
{
    VERIFY(m_initialized);
#ifdef DEBUG
    verify_vertex_id(level_graph, cross, game_graph);
#endif
    return (m_game_vertex_id);
}

#ifdef DEBUG
IC void CPatrolPoint::path(const CPatrolPath* path)
{
    VERIFY(path);
    VERIFY(!m_path);
    m_path = path;
}
#endif
