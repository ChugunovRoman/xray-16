////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_graph_registry_inline.h
//	Created 	: 15.01.2003
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife graph registry inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

IC CALifeLevelRegistry& CALifeGraphRegistry::level() const
{
    VERIFY(m_level);
    return (*m_level);
}

IC void CALifeGraphRegistry::change(
    CSE_ALifeDynamicObject* object, GameGraph::_GRAPH_ID tGraphPointID, GameGraph::_GRAPH_ID tNextGraphPointID)
{
    VERIFY3(object->used_ai_locations() /** && (object->interactive() || object->m_bOnline)**/,
        object->s_name.c_str(), object->name_replace());
    // 63-wave3-fixes.md S3: add() dedups through the id -> vertex index in O(1) (fd6cecd15, #2058) - no
    // known_unique to pass along; a stale registration left by a desync is dropped by the add itself
    remove(object, tGraphPointID);
    add(object, tNextGraphPointID, true);
    object->m_tGraphID = tNextGraphPointID;
    object->o_Position = ai().game_graph().vertex(object->m_tGraphID)->level_point();
    object->m_tNodeID = ai().game_graph().vertex(object->m_tGraphID)->level_vertex_id();
}

IC void CALifeGraphRegistry::assign(CSE_ALifeMonsterAbstract* monster)
{
    monster->m_tNextGraphID = monster->m_tPrevGraphID = monster->m_tGraphID;
    monster->m_fDistanceToPoint = monster->m_fDistance;
    CGameGraph::const_iterator i, e;
    ai().game_graph().begin(monster->m_tNextGraphID, i, e);
    for (; i != e; ++i)
        if ((*i).distance() > monster->m_fDistance)
        {
            monster->m_fDistanceFromPoint = (*i).distance() - monster->m_fDistance;
            break;
        }
}

IC void CALifeGraphRegistry::set_process_time(const float& process_time)
{
    m_process_time = process_time;
    if (m_level)
        level().set_process_time(m_process_time);
}

// review2 G-m4: whether the object is registered in SOME vertex map right now, O(1) through the id -> vertex
// index - regardless of which vertex that turns out to be (a caller that only wants to know "is there anything
// to remove" must not re-derive this by checking one particular vertex's map, such as object->m_tGraphID: on a
// desync the registration can sit at a DIFFERENT vertex, see remove()'s own comment and CSE_ALifeOnlineOfflineGroup
// ::pull_member_to_squad_level).
IC bool CALifeGraphRegistry::is_registered(ALife::_OBJECT_ID id) const { return m_id_to_vertex[id] != kNotRegistered; }

IC CSE_ALifeCreatureActor* CALifeGraphRegistry::actor() const { return (m_actor); }
IC const CALifeGraphRegistry::GRAPH_REGISTRY& CALifeGraphRegistry::objects() const { return (m_objects); }
template <typename F>
IC void CALifeGraphRegistry::iterate_objects(GameGraph::_GRAPH_ID game_vertex_id, const F& f)
{
    iterate(((CGraphPointInfo&)(objects()[game_vertex_id])).objects(), f);
}

template <typename F, typename C>
IC void CALifeGraphRegistry::iterate(C& c, const F& f)
{
    c.update(f);
}
