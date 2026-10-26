////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_graph_registry.h
//	Created 	: 15.01.2003
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife graph registry
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "xrServer_Objects_ALife_All.h"
#include "alife_level_registry.h"

class CSE_ALifeCreatureActor;
class CSE_ALifeInventoryItem;

class CALifeGraphRegistry
{
public:
    typedef CSafeMapIterator<ALife::_OBJECT_ID, CSE_ALifeDynamicObject, std::less<ALife::_OBJECT_ID>, false>
        OBJECT_REGISTRY;

public:
    class CGraphPointInfo
    {
    protected:
        OBJECT_REGISTRY m_objects;

    public:
        IC OBJECT_REGISTRY& objects() { return (m_objects); }
        IC const OBJECT_REGISTRY& objects() const { return (m_objects); }
    };

public:
    typedef xr_vector<CGraphPointInfo> GRAPH_REGISTRY;
    typedef xr_vector<GameGraph::_GRAPH_ID> TERRAIN_REGISTRY;

    // 63-wave3-fixes.md S3: the "not registered" mark of m_id_to_vertex (a valid game vertex id is < vertex_count,
    // far below u16(-1))
    static constexpr GameGraph::_GRAPH_ID kNotRegistered = (GameGraph::_GRAPH_ID)(-1);

protected:
    GRAPH_REGISTRY m_objects;
    TERRAIN_REGISTRY m_terrain[GameGraph::LOCATION_TYPE_COUNT][GameGraph::LOCATION_COUNT];
    CALifeLevelRegistry* m_level;
    CSE_ALifeCreatureActor* m_actor;
    float m_process_time;
    xr_vector<CSE_ALifeDynamicObject*> m_temp;
    // 63-wave3-fixes.md S3: the id -> vertex index of the registrations above (the engine invariant #2058, fd6cecd15:
    // at most one vertex map holds a given id). add() drops a stale registration through it and remove() finds the
    // vertex the object really sits at - both O(1), no all-vertices sweeps (the wave-3 dedup scan cost O(V) per
    // inventory item on the offline switch of a squad). Object ids are u16 (ALife::_OBJECT_ID), one slot per id.
    // Kept in add()/remove()/on_load() - the only writers of m_objects; u16(-1) - not registered
    xr_vector<GameGraph::_GRAPH_ID> m_id_to_vertex;

protected:
    void setup_current_level();
    template <typename F, typename C>
    IC void iterate(C& c, const F& f);

public:
    CALifeGraphRegistry();
    virtual ~CALifeGraphRegistry();
    void on_load();
    void update(CSE_ALifeDynamicObject* object);
    void attach(CSE_Abstract& object, CSE_ALifeInventoryItem* item, GameGraph::_GRAPH_ID game_vertex_id,
        bool alife_query = true, bool add_children = true);
    void detach(CSE_Abstract& object, CSE_ALifeInventoryItem* item, GameGraph::_GRAPH_ID game_vertex_id,
        bool alife_query = true, bool remove_children = true);
    IC void assign(CSE_ALifeMonsterAbstract* object);
    // 61-wave3-spec.md E6 / 63-wave3-fixes.md S3: every add checks the id -> vertex index and drops a stale
    // registration (fd6cecd15, #2058) in O(1) - the known_unique shortcut of the wave-3 code is gone with the
    // all-vertices sweep it used to skip
    void add(CSE_ALifeDynamicObject* object, GameGraph::_GRAPH_ID game_vertex_id, bool bUpdateSwitchObjects = true);
    // true - the object was found and removed from its vertex map (the index says where; on a graph/level desync -
    // the vertex it was actually registered at, not the claimed one); false - it was in no vertex map
    bool remove(CSE_ALifeDynamicObject* object, GameGraph::_GRAPH_ID game_vertex_id, bool bUpdateSwitchObjects = true);
    // true - the object has an entry in SOME vertex map right now (O(1), the id -> vertex index); does not say which
    IC bool is_registered(ALife::_OBJECT_ID id) const;
    IC void change(
        CSE_ALifeDynamicObject* object, GameGraph::_GRAPH_ID game_vertex_id, GameGraph::_GRAPH_ID next_game_vertex_id);
    IC CALifeLevelRegistry& level() const;
    IC void set_process_time(const float& process_time);
    IC CSE_ALifeCreatureActor* actor() const;
    IC const GRAPH_REGISTRY& objects() const;
    template <typename F>
    IC void iterate_objects(GameGraph::_GRAPH_ID game_vertex_id, const F& f);
};

#include "alife_graph_registry_inline.h"
