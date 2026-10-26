////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_graph_registry.cpp
//	Created 	: 15.01.2003
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife graph registry
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "alife_graph_registry.h"
#include "xrServerEntities/xrMessages.h"
#include "npc_cpp_profile.h"

using namespace ALife;

CALifeGraphRegistry::CALifeGraphRegistry()
{
    m_level = 0;
    m_process_time = 0;
    m_actor = 0;
    // 63-wave3-fixes.md S3: one index slot per possible object id (u16, ALife::_OBJECT_ID)
    m_id_to_vertex.assign((size_t)u16(-1) + 1, kNotRegistered);
}

CALifeGraphRegistry::~CALifeGraphRegistry() { xr_delete(m_level); }
void CALifeGraphRegistry::on_load()
{
    for (int i = 0; i < GameGraph::LOCATION_TYPE_COUNT; ++i)
    {
        {
            for (int j = 0; j < GameGraph::LOCATION_COUNT; ++j)
                m_terrain[i][j].clear();
        }
        for (GameGraph::_GRAPH_ID j = 0; j < (GameGraph::_GRAPH_ID)ai().game_graph().header().vertex_count(); ++j)
            m_terrain[i][ai().game_graph().vertex(j)->vertex_type()[i]].push_back(j);
    }

    m_objects.resize(ai().game_graph().header().vertex_count());

    {
        GRAPH_REGISTRY::iterator I = m_objects.begin();
        GRAPH_REGISTRY::iterator E = m_objects.end();
        for (; I != E; ++I)
            (*I).objects().clear();
    }

    // 63-wave3-fixes.md S3: the registrations of the previous game died with m_objects above - the index too
    // (the load registers every object again through update() -> add())
    m_id_to_vertex.assign(m_id_to_vertex.size(), kNotRegistered);
}

void CALifeGraphRegistry::update(CSE_ALifeDynamicObject* object)
{
    if (!object->m_bDirectControl)
        return;

    if (object->s_flags.is(M_SPAWN_OBJECT_ASPLAYER))
    {
        m_actor = smart_cast<CSE_ALifeCreatureActor*>(object);
        R_ASSERT2(m_actor, "Invalid flag M_SPAWN_OBJECT_ASPLAYER for non-actor object!");

        if (g_start_game_vertex_id != (u16)-1 && g_start_level_vertex_id != (u32)-1)
        {
            m_actor->m_tNodeID = g_start_level_vertex_id;
            m_actor->m_tGraphID = g_start_game_vertex_id;
            m_actor->o_Position = g_start_position;
        }
    }

    if (m_actor && !m_level)
        setup_current_level();

    CSE_ALifeInventoryItem* item = smart_cast<CSE_ALifeInventoryItem*>(object);
    if (!item || !item->attached())
        add(object, object->m_tGraphID);
}

void CALifeGraphRegistry::setup_current_level()
{
    m_level = xr_new<CALifeLevelRegistry>(ai().game_graph().vertex(actor()->m_tGraphID)->level_id());
    level().set_process_time(m_process_time);
    for (int i = 0, n = ai().game_graph().header().vertex_count(); i < n; ++i)
        if (ai().game_graph().vertex(i)->level_id() == level().level_id())
        {
            D_OBJECT_P_MAP::const_iterator I = m_objects[i].objects().objects().begin();
            D_OBJECT_P_MAP::const_iterator E = m_objects[i].objects().objects().end();
            for (; I != E; ++I)
                level().add((*I).second);
        }

    {
        xr_vector<CSE_ALifeDynamicObject*>::const_iterator I = m_temp.begin();
        xr_vector<CSE_ALifeDynamicObject*>::const_iterator E = m_temp.end();
        for (; I != E; ++I)
            level().add(*I);

        m_temp.clear();
    }
    GameGraph::LEVEL_MAP::const_iterator I =
        ai().game_graph().header().levels().find(ai().game_graph().vertex(actor()->m_tGraphID)->level_id());
    R_ASSERT2(ai().game_graph().header().levels().end() != I, "Graph point level ID not found!");

    [[maybe_unused]] const int id = g_pGamePersistent->Level_ID(I->second.name().c_str(), "1.0", true);
    VERIFY3(id >= 0, "Level is corrupted or doesn't exist", I->second.name().c_str());
    ai().load(I->second.name().c_str());

    g_start_game_vertex_id = (u16)-1;
    g_start_level_vertex_id = (u32)-1;
}

void CALifeGraphRegistry::attach(CSE_Abstract& object, CSE_ALifeInventoryItem* item,
    GameGraph::_GRAPH_ID game_vertex_id, bool alife_query, bool add_children)
{
#ifdef DEBUG
    if (psAI_Flags.test(aiALife))
    {
        Msg("[LSS] Attaching item [%s][%d] to [%s][%d]", item->base()->name_replace(), item->base()->ID,
            object.name_replace(), object.ID);
    }
#endif
    if (alife_query)
        remove(smart_cast<CSE_ALifeDynamicObject*>(item), game_vertex_id);
    else
        level().remove(smart_cast<CSE_ALifeDynamicObject*>(item));

    CSE_ALifeDynamicObject* dynamic_object = smart_cast<CSE_ALifeDynamicObject*>(&object);
    R_ASSERT2(!alife_query || dynamic_object, "Cannot attach an item to a non-alife object object");

    dynamic_object->attach(item, alife_query, add_children);
}

void CALifeGraphRegistry::detach(CSE_Abstract& object, CSE_ALifeInventoryItem* item,
    GameGraph::_GRAPH_ID game_vertex_id, bool alife_query, bool remove_children)
{
#ifdef DEBUG
    if (psAI_Flags.test(aiALife))
    {
        Msg("[LSS] Detaching item [%s][%d] from [%s][%d]", item->base()->name_replace(), item->base()->ID,
            object.name_replace(), object.ID);
    }
#endif
    if (alife_query)
        add(smart_cast<CSE_ALifeDynamicObject*>(item), game_vertex_id);
    else
    {
        CSE_ALifeDynamicObject* object = smart_cast<CSE_ALifeDynamicObject*>(item);
        VERIFY(object);
        object->m_tGraphID = game_vertex_id;
        level().add(object);
    }

    CSE_ALifeDynamicObject* dynamic_object = smart_cast<CSE_ALifeDynamicObject*>(&object);
    R_ASSERT2(!alife_query || dynamic_object, "Cannot detach an item from non-alife object");

    VERIFY(alife_query || !smart_cast<CSE_ALifeDynamicObject*>(&object) ||
        (ai().game_graph().vertex(smart_cast<CSE_ALifeDynamicObject*>(&object)->m_tGraphID)->level_id() ==
            level().level_id()));

    if (dynamic_object)
        dynamic_object->detach(item, 0, alife_query, remove_children);
    else
    {
#ifdef DEBUG
        bool value =
            std::find(object.children.begin(), object.children.end(), item->base()->ID) != object.children.end();
        if (!value)
        {
            Msg("! ERROR: can't detach independant object. entity[%s:%d], parent[%s:%d], section[%s]",
                item->base()->name_replace(), item->base()->ID, object.name_replace(), object.ID,
                item->base()->s_name.c_str());
        }
#endif // DEBUG
        //		R_ASSERT2				(value,"Can't detach an item which is not on my own");
    }
}

void CALifeGraphRegistry::add(CSE_ALifeDynamicObject* object, GameGraph::_GRAPH_ID game_vertex_id, bool update)
{
#ifdef DEBUG
    if (psAI_Flags.test(aiALife))
    {
        Msg("[LSS] adding object [%s][%d] to graph point %d", object->name_replace(), object->ID, game_vertex_id);
    }
#endif
    if (!object)
        return;

    if (!object->m_bOnline && object->used_ai_locations() /**&& object->interactive()**/)
    {
        if (!ai().game_graph().valid_vertex_id(game_vertex_id) || (size_t)game_vertex_id >= m_objects.size())
        {
            Msg("! [ALife] CALifeGraphRegistry::add: invalid game vertex %u for [%s][%u] (registry_size=%zu) — skipping",
                (u32)game_vertex_id, object->name_replace(), object->ID, m_objects.size());
            return;
        }

        VERIFY(ai().game_graph().valid_vertex_id(game_vertex_id));
        OBJECT_REGISTRY& target = m_objects[game_vertex_id].objects();
        const auto& target_map = target.objects();
        const auto existing_at_target = target_map.find(object->ID);
        const bool already_registered_here =
            (existing_at_target != target_map.end() && existing_at_target->second == object);

        if (already_registered_here)
        {
            // idempotent re-add of the same pair: just make sure the index agrees (self-heal of a ghost entry).
            // review2 G-m2: under the invariant (fd6cecd15, #2058) a duplicate across two vertices never happens,
            // so registered_at is always already game_vertex_id here - but if that invariant were ever broken, this
            // blind overwrite would drop the OTHER vertex's entry from the index for good: a plain miss in remove()
            // no longer sweeps every vertex to find it (that sweep is exactly what the O(1) index replaced). Cheap
            // insurance: drop the stale entry the same way the branch below does.
            const GameGraph::_GRAPH_ID registered_at = m_id_to_vertex[object->ID];
            if (registered_at != kNotRegistered && registered_at != game_vertex_id &&
                (size_t)registered_at < m_objects.size())
            {
                m_objects[registered_at].objects().remove(object->ID, true);
                npc_cpp_profile::count(ENpcCppProfileStage::AlifeGraphAddStale);
#ifndef MASTER_GOLD
                Msg("! [ALife] graph registry: add [%s][%d] already at vertex %u, dropped a stale duplicate at %u",
                    object->name_replace(), object->ID, (u32)game_vertex_id, (u32)registered_at);
#endif
            }
            m_id_to_vertex[object->ID] = game_vertex_id;
        }
        else
        {
            // 63-wave3-fixes.md S3: the #2058 dedup (fd6cecd15) through the id -> vertex index - O(1) instead of
            // the wave-3 all-vertices sweep, so EVERY add pays it and the invariant "at most one registration per
            // id" holds for every caller (load, spawn, the offline switch of an inventory item, detach,
            // alife_combat_manager kill_entity) with no known_unique hints. The index is kept by add/remove/on_load
            // - the only writers of the vertex maps
            const GameGraph::_GRAPH_ID registered_at = m_id_to_vertex[object->ID];
            bool dropped_stale = false;
            if (registered_at != kNotRegistered && (size_t)registered_at < m_objects.size() &&
                registered_at != game_vertex_id)
            {
                // a stale registration at another vertex: add without a confirmed remove, or the graph/level
                // desync of a teleport
                m_objects[registered_at].objects().remove(object->ID, true);
                dropped_stale = true;
#ifndef MASTER_GOLD
                Msg("! [ALife] graph registry: add [%s][%d] dropped a stale registration at vertex %u (registering at "
                    "%u)",
                    object->name_replace(), object->ID, (u32)registered_at, (u32)game_vertex_id);
#endif
            }
            if (existing_at_target != target_map.end())
            {
                // the id sits at the target with another pointer (or the index lost it): CSafeMapIterator::add
                // silently keeps an existing id, so the slot is cleared before the insert
                target.remove(object->ID, true);
                dropped_stale = true;
            }
            target.add(object->ID, object);
            m_id_to_vertex[object->ID] = game_vertex_id;
            // 63-wave3-fixes.md S4: plain counters - no report flush here, these run inside the QPC interval of
            // alife/offline_path/vertex_change (a flush skewed its max by ~1 ms every 5 s window)
            npc_cpp_profile::count(ENpcCppProfileStage::AlifeGraphAdd);
            if (dropped_stale)
                npc_cpp_profile::count(ENpcCppProfileStage::AlifeGraphAddStale);
        }
        object->m_tGraphID = game_vertex_id;
    }
    else if (!m_level && update && ai().game_graph().valid_vertex_id(game_vertex_id) &&
             (size_t)game_vertex_id < m_objects.size())
    {
        m_temp.push_back(object);
        object->m_tGraphID = game_vertex_id;
    }

    if (update && m_level && ai().game_graph().valid_vertex_id(game_vertex_id))
        level().add(object);
}

bool CALifeGraphRegistry::remove(CSE_ALifeDynamicObject* object, GameGraph::_GRAPH_ID game_vertex_id, bool update)
{
    bool removed_from_graph = false;
    if (object->used_ai_locations() /**&& object->interactive()**/)
    {
#ifdef DEBUG
        if (psAI_Flags.test(aiALife))
        {
            Msg("[LSS] removing object [%s][%d] from graph point %d", object->name_replace(), object->ID,
                game_vertex_id);
        }
#endif
        // 63-wave3-fixes.md S3: the index says which vertex map holds the object (at most one, invariant #2058).
        // The lookup is O(1) and covers the desync of fd6cecd15 - the registration can live at another vertex than
        // the claimed m_tGraphID - without the wave-3 all-vertices sweep
        bool index_lost = false;
        const GameGraph::_GRAPH_ID registered_at = m_id_to_vertex[object->ID];
        if (registered_at != kNotRegistered)
        {
            if ((size_t)registered_at < m_objects.size())
            {
                OBJECT_REGISTRY& reg = m_objects[registered_at].objects();
                const auto& om = reg.objects();
                if (om.find(object->ID) != om.end())
                {
                    reg.remove(object->ID);
                    removed_from_graph = true;
                    if (registered_at != game_vertex_id)
                    {
                        npc_cpp_profile::count(ENpcCppProfileStage::AlifeGraphRemoveDesync);
#ifndef MASTER_GOLD
                        Msg("! [ALife] graph registry: object [%s][%d] was at vertex %u, not at m_tGraphID %u — removed "
                            "from actual vertex",
                            object->name_replace(), object->ID, (u32)registered_at, game_vertex_id);
#endif
                    }
                }
                else
                    index_lost = true;
            }
            else
                index_lost = true;
            if (index_lost)
            {
                // the index and the vertex maps disagree (should not happen: add/remove/on_load are their only
                // writers) - self-heal below so a confirmed remove still means "registered nowhere" (fd6cecd15)
                Msg("! [ALife] graph registry: remove [%s][%d] - the index says vertex %u but it holds no entry - "
                    "sweeping",
                    object->name_replace(), object->ID, (u32)registered_at);
                m_id_to_vertex[object->ID] = kNotRegistered;
            }
        }
        if (index_lost && !removed_from_graph)
        {
            // the self-heal path only (a lost registration); a clean miss above is authoritative and skips it
            const GameGraph::_GRAPH_ID vertex_count = (GameGraph::_GRAPH_ID)m_objects.size();
            for (GameGraph::_GRAPH_ID i = 0; i < vertex_count; ++i)
            {
                if (!ai().game_graph().valid_vertex_id(i))
                    continue;
                OBJECT_REGISTRY& reg = m_objects[i].objects();
                const auto& om = reg.objects();
                if (om.find(object->ID) == om.end())
                    continue;
                reg.remove(object->ID);
                removed_from_graph = true;
                npc_cpp_profile::count(ENpcCppProfileStage::AlifeGraphRemoveDesync);
#ifndef MASTER_GOLD
                Msg("! [ALife] graph registry: object [%s][%d] was at vertex %u, not at m_tGraphID %u — removed from "
                    "actual vertex",
                    object->name_replace(), object->ID, (u32)i, game_vertex_id);
#endif
            }
        }
        if (removed_from_graph)
            m_id_to_vertex[object->ID] = kNotRegistered;
#ifndef MASTER_GOLD
        if (!removed_from_graph)
            Msg("! [ALife] graph registry: remove [%s][%d] — not in any vertex map (expected %u), continuing",
                object->name_replace(), object->ID, game_vertex_id);
#endif
    }
    if (update && m_level)
    {
        // review2 G-m1: an invalid game_vertex_id (a parent with a corrupted/never-set AI location: add() already
        // refuses to register such an object, see its own "invalid game vertex" guard above) used to reach
        // vertex(game_vertex_id) unchecked here - a read past the end of the vertex array in a release build.
        // The short-circuit below skips that call entirely instead of only adding a second condition after it.
        bool level_no_assert = !ai().game_graph().valid_vertex_id(game_vertex_id) ||
            ai().game_graph().vertex(game_vertex_id)->level_id() != level().level_id();
        if (object->used_ai_locations() && !removed_from_graph)
            level_no_assert = true;
        level().remove(object, level_no_assert);
    }
    return (removed_from_graph);
}
