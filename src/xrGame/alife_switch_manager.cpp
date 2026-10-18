////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_switch_manager.cpp
//	Created 	: 25.12.2002
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife Simulator switch manager
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "alife_switch_manager.h"
#include "xrServer_Objects_ALife.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "alife_graph_registry.h"
#include "alife_object_registry.h"
#include "alife_schedule_registry.h"
#include "xrAICore/Navigation/game_level_cross_table.h"
#include "xrServer.h"
#include "ai_space.h"
#include "xrAICore/Navigation/level_graph.h"
#include "xrNetServer/NET_Messages.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "alife_simulator.h"
#include "xrEngine/Render.h"

#ifdef DEBUG
#include "Level.h"
#endif // DEBUG

using namespace ALife;

struct remove_non_savable_predicate
{
    IPureServer* m_server;

    IC remove_non_savable_predicate(IPureServer* server)
    {
        VERIFY(server);
        m_server = server;
    }

    IC bool operator()(const ALife::_OBJECT_ID& id) const
    {
        CSE_Abstract* object = m_server->GetGameState()->get_entity_from_eid(id);
        VERIFY(object);
        CSE_ALifeObject* alife_object = smart_cast<CSE_ALifeObject*>(object);
        VERIFY(alife_object);
        return (!alife_object->can_save());
    }
};

CALifeSwitchManager::~CALifeSwitchManager() {}
void CALifeSwitchManager::add_online(CSE_ALifeDynamicObject* object, bool update_registries)
{
    START_PROFILE("ALife/switch/add_online")
    VERIFY((ai().game_graph().vertex(object->m_tGraphID)->level_id() == graph().level().level_id()));

    object->m_bOnline = true;

    NET_Packet tNetPacket;
    CSE_Abstract* l_tpAbstract = smart_cast<CSE_Abstract*>(object);
    server().entity_Destroy(l_tpAbstract);
    object->s_flags._or(M_SPAWN_UPDATE);
    ClientID clientID;
    clientID.set(server().GetServerClient() ? server().GetServerClient()->ID.value() : 0);
    server().Process_spawn(tNetPacket, clientID, FALSE, l_tpAbstract);
    object->s_flags._and (u16(-1) ^ M_SPAWN_UPDATE);
    PERF_INC(alife_online_objects); // spawn stage 0: the source of a spawn wave
    m_online_spawns.fetch_add(1, std::memory_order_relaxed);

	//Alundaio: Workaround for crash with corpses that end up outside AI map
    //R_ASSERT3(!object->used_ai_locations() || ai().level_graph().valid_vertex_id(object->m_tNodeID),
    //          "Invalid vertex for object ", object->name_replace());

#ifndef MASTER_GOLD
    if (psAI_Flags.test(aiALife))
        Msg("[LSS] Spawning object [%s][%s][%d]", object->name_replace(), object->s_name.c_str(), object->ID);
#endif

    object->add_online(update_registries);
    STOP_PROFILE
}

void CALifeSwitchManager::remove_online(CSE_ALifeDynamicObject* object, bool update_registries)
{
    START_PROFILE("ALife/switch/remove_online")
    object->m_bOnline = false;

    m_saved_chidren = object->children;
    CSE_ALifeTraderAbstract* inventory_owner = smart_cast<CSE_ALifeTraderAbstract*>(object);
    if (inventory_owner)
    {
        m_saved_chidren.erase(
            std::remove_if(m_saved_chidren.begin(), m_saved_chidren.end(), remove_non_savable_predicate(&server())),
            m_saved_chidren.end());
    }

    // P17: relcase report (perf_metrics) - the client object is destroyed because ALife switched it offline
    if (g_pGameLevel)
        g_pGameLevel->Objects.destroy_origin_note(object->ID, CObjectList::destroy_origin_switch);
    // 32-frame-spikes.md: what the client is going to destroy (alife/switch_offline_frame, the limit of the spread pass)
    const u32 client_count = client_objects(object);
    m_offline_client_objects.fetch_add(client_count, std::memory_order_relaxed);
    PERF_ADD(alife_switch_offline_frame, client_count);
    server().Perform_destroy(object, net_flags(TRUE, TRUE));
    VERIFY(object->children.empty());

    _OBJECT_ID object_id = object->ID;
    object->ID = server().PerformIDgen(object_id);

#ifdef DEBUG
    if (psAI_Flags.test(aiALife))
        Msg("[LSS] Destroying object [%s][%s][%d]", object->name_replace(), object->s_name.c_str(), object->ID);
#endif

    object->add_offline(m_saved_chidren, update_registries);
    STOP_PROFILE
}

u32 CALifeSwitchManager::client_objects(const CSE_ALifeDynamicObject* object) const
{
    u32 count = 1;
    for (const ALife::_OBJECT_ID child_id : object->children)
    {
        if (const CSE_ALifeDynamicObject* child = objects().object(child_id, true))
            count += client_objects(child);
        else
            ++count; // not an ALife object (should not happen): still a client object
    }
    return count;
}

u32 CALifeSwitchManager::switch_offline_estimate(const CSE_ALifeDynamicObject* object) const
{
    if (!object->m_bOnline || object->ID_Parent != 0xffff)
        return 0;
    const CSE_ALifeCreatureActor* actor = graph().actor();
    const float offline = offline_distance();
    // A squad goes offline with all its members (CSE_ALifeOnlineOfflineGroup::try_switch_offline): none may be within
    // offline_distance(). The members are taken by ID from the registry, not by the pointers of the squad (they can be
    // stale until sanitize_members of the switch itself)
    if (const auto* group = smart_cast<const CSE_ALifeOnlineOfflineGroup*>(object))
    {
        u32 count = 0;
        for (const auto& it : group->squad_members())
        {
            const CSE_ALifeDynamicObject* member = objects().object(it.first, true);
            if (!member || !member->m_bOnline)
                continue;
            if (actor && actor->o_Position.distance_to(member->o_Position) <= offline)
                return 0;
            count += client_objects(member);
        }
        return count;
    }
    if (actor && actor->o_Position.distance_to(object->o_Position) <= offline)
        return 0;
    return client_objects(object);
}

void CALifeSwitchManager::switch_online(CSE_ALifeDynamicObject* object)
{
    START_PROFILE("ALife/switch/switch_online")
#ifdef DEBUG
    //	if (psAI_Flags.test(aiALife))
    Msg("[LSS][%d] Going online [%d][%s][%d] ([%f][%f][%f] : [%f][%f][%f]), on '%s'", Device.dwFrame,
        Device.dwTimeGlobal, object->name_replace(), object->ID, VPUSH(graph().actor()->o_Position),
        VPUSH(object->o_Position), "*SERVER*");
#endif
    object->switch_online();
    STOP_PROFILE
}

void CALifeSwitchManager::switch_offline(CSE_ALifeDynamicObject* object)
{
    START_PROFILE("ALife/switch/switch_offline")
#ifdef DEBUG
    //	if (psAI_Flags.test(aiALife))
    Msg("[LSS][%d] Going offline [%d][%s][%d] ([%f][%f][%f] : [%f][%f][%f]), on '%s'", Device.dwFrame,
        Device.dwTimeGlobal, object->name_replace(), object->ID, VPUSH(graph().actor()->o_Position),
        VPUSH(object->o_Position), "*SERVER*");
#endif
    object->switch_offline();
    STOP_PROFILE
}

bool CALifeSwitchManager::synchronize_location(CSE_ALifeDynamicObject* I)
{
    START_PROFILE("ALife/switch/synchronize_location")
#ifdef DEBUG
    // VERIFY3(ai().level_graph().level_id() == ai().game_graph().vertex(I->m_tGraphID)->level_id(), I->s_name.c_str(),
    //     I->name_replace());
    // if (!I->children.empty())
    // {
    //     u32 size = I->children.size();
    //     ALife::_OBJECT_ID* test = (ALife::_OBJECT_ID*)xr_alloca(size * sizeof(ALife::_OBJECT_ID));
    //     memcpy(test, &*I->children.begin(), size * sizeof(ALife::_OBJECT_ID));
    //     std::sort(test, test + size);
    //     for (u32 i = 1; i < size; ++i)
    //     {
    //         VERIFY3(test[i - 1] != test[i], "Child is registered twice in the child list", (*I).name_replace());
    //     }
    // }
#endif // DEBUG

    // check if we do not use ai locations
    if (!I->used_ai_locations())
        return (true);

    // check if we are not attached
    if (0xffff != I->ID_Parent)
        return (true);

    // check if we are not online and have an invalid level vertex id
    if (!I->m_bOnline && !ai().level_graph().valid_vertex_id(I->m_tNodeID))
        return (true);

    return ((*I).synchronize_location());
    STOP_PROFILE
}

void CALifeSwitchManager::try_switch_online(CSE_ALifeDynamicObject* I)
{
    START_PROFILE("ALife/switch/try_switch_online")
    // so, the object is offline
    // checking if the object is not attached
    if (0xffff != I->ID_Parent)
    {
// so, object is attached
// checking if parent is offline too
#ifdef DEBUG
        if (psAI_Flags.test(aiALife))
        {
            CSE_ALifeCreatureAbstract* l_tpALifeCreatureAbstract =
                smart_cast<CSE_ALifeCreatureAbstract*>(objects().object(I->ID_Parent));
            if (l_tpALifeCreatureAbstract && (l_tpALifeCreatureAbstract->get_health() < EPS_L))
                Msg("! uncontrolled situation [%d][%d][%s][%f]", I->ID, I->ID_Parent,
                    l_tpALifeCreatureAbstract->name_replace(), l_tpALifeCreatureAbstract->get_health());
            VERIFY2(!l_tpALifeCreatureAbstract || (l_tpALifeCreatureAbstract->get_health() >= EPS_L),
                "Parent online, item offline...");
            if (objects().object(I->ID_Parent)->m_bOnline)
                Msg("! uncontrolled situation [%d][%d][%s][%f]", I->ID, I->ID_Parent,
                    l_tpALifeCreatureAbstract->name_replace(), l_tpALifeCreatureAbstract->get_health());
        }
        VERIFY2(!objects().object(I->ID_Parent)->m_bOnline, "Parent online, item offline...");
#endif
        return;
    }

    VERIFY2((ai().game_graph().vertex(I->m_tGraphID)->level_id() != ai().level_graph().level_id()) ||
            !Level().Objects.net_Find(I->ID) || Level().Objects.dump_all_objects(),
        make_string("frame [%d] time [%d] object [%s] with id [%d] is offline, but is on the level", Device.dwFrame,
            Device.dwTimeGlobal, I->name_replace(), I->ID));

    I->try_switch_online();

    if (!I->m_bOnline && !I->keep_saved_data_anyway())
        I->clear_client_data();

    STOP_PROFILE
}

void CALifeSwitchManager::try_switch_offline(CSE_ALifeDynamicObject* I)
{
    START_PROFILE("ALife/switch/try_switch_offline")
    // checking if the object is not attached
    if (0xffff != I->ID_Parent)
    {
#ifdef DEBUG
        // checking if parent is online too
        CSE_ALifeCreatureAbstract* l_tpALifeCreatureAbstract =
            smart_cast<CSE_ALifeCreatureAbstract*>(objects().object(I->ID_Parent));
        if (l_tpALifeCreatureAbstract && (l_tpALifeCreatureAbstract->get_health() < EPS_L))
            Msg("! uncontrolled situation [%d][%d][%s][%f]", I->ID, I->ID_Parent,
                l_tpALifeCreatureAbstract->name_replace(), l_tpALifeCreatureAbstract->get_health());

        VERIFY2(!smart_cast<CSE_ALifeCreatureAbstract*>(objects().object(I->ID_Parent)) ||
                (smart_cast<CSE_ALifeCreatureAbstract*>(objects().object(I->ID_Parent))->get_health() >= EPS_L),
            "Parent offline, item online...");

        if (!objects().object(I->ID_Parent)->m_bOnline)
            Msg("! uncontrolled situation [%d][%d][%s][%f]", I->ID, I->ID_Parent,
                l_tpALifeCreatureAbstract->name_replace(), l_tpALifeCreatureAbstract->get_health());

        VERIFY2(objects().object(I->ID_Parent)->m_bOnline, "Parent offline, item online...");
#endif
        return;
    }

    I->try_switch_offline();
    STOP_PROFILE
}

void CALifeSwitchManager::switch_object(CSE_ALifeDynamicObject* I)
{
    if (I->redundant())
    {
        release(I);
        return;
    }

    if (!synchronize_location(I))
        return;

    if (I->m_bOnline)
        try_switch_offline(I);
    else
        try_switch_online(I);

    if (I->redundant())
        release(I);
}

// ---------------------------------------------------------------------------------------------------------------------
// 36-spawn-model-load.md (plans/optimization_mt): model prefetch, always on since 2026-10-18 (model_prefetch removed,
// see plan 41: all three parts, its former bits 1, 2 and 4). model_prefetch_margin: a tunable, not saved
// (console_commands.cpp)
int g_model_prefetch_margin = 25;

namespace
{
// The models an object brings with it when it goes online: the visual of the server entity (CGameObject::net_Spawn
// sets it), the visual of its section (CGameObject::Load creates that one first, object_pool/load) and a bloodsucker's
// Predator_Visual (set in the game). The same name may come twice: the render keeps each once.
template <typename F>
void model_prefetch_for_each_visual(CSE_Abstract* object, F&& f)
{
    if (const CSE_Visual* visual = smart_cast<CSE_Visual*>(object))
    {
        const pcstr name = visual->get_visual();
        if (name && name[0])
            f(name);
    }
    const pcstr section = object->s_name.c_str();
    if (!section || !pSettings->section_exist(section))
        return;
    for (const pcstr line : {"visual", "Predator_Visual"})
    {
        if (!pSettings->line_exist(section, line))
            continue;
        const pcstr name = pSettings->r_string(section, line);
        if (name && name[0])
            f(name);
    }
}

void model_prefetch_files_of(CSE_Abstract* object, u32 source)
{
    model_prefetch_for_each_visual(object, [source](pcstr name) { GEnv.Render->model_PrefetchFiles(name, source); });
}
} // namespace

bool model_prefetch_in_margin(float distance, float online_distance)
{
    return distance <= online_distance * (1.f + float(g_model_prefetch_margin) / 100.f);
}

void model_prefetch_object(CSE_ALifeDynamicObject* object)
{
    if (!object)
        return;
    model_prefetch_files_of(object, 0);
    // its inventory comes online with it (alife_trader_abstract.cpp, add_online_impl)
    for (const u16 child_id : object->children)
    {
        if (CSE_ALifeDynamicObject* child = ai().alife().objects().object(child_id, true))
            model_prefetch_files_of(child, 0);
    }
}

void model_prefetch_spawn(u16 id)
{
    if (id == u16(-1) || !ai().get_alife())
        return;
    if (CSE_ALifeDynamicObject* object = ai().alife().objects().object(id, true))
        model_prefetch_files_of(object, 1);
}

void model_preload_level()
{
    if (!ai().get_alife() || !GEnv.Render)
        return;
    ZoneScoped;
    PERF_SCOPE(mpreload_time);
    CTimer timer;
    timer.Start();
    u32 loaded = 0;
    u32 already = 0;
    xr_set<shared_str> seen; // a name once (shared_str: the same string, one pointer)
    const auto preload = [&](pcstr name) {
        if (!seen.insert(shared_str(name)).second)
            return;
        if (GEnv.Render->model_Preload(name))
        {
            ++loaded;
            PERF_INC(mpreload_models);
        }
        else
        {
            ++already;
            PERF_INC(mpreload_already);
        }
    };
    {
        // The creatures of this level, offline ones included: they come online here (fight1: mutant waves)
        const GameGraph::_LEVEL_ID level_id = ai().alife().graph().level().level_id();
        for (const auto& [id, object] : ai().alife().objects().objects())
        {
            if (!smart_cast<CSE_ALifeCreatureAbstract*>(object))
                continue;
            if (ai().game_graph().vertex(object->m_tGraphID)->level_id() != level_id)
                continue;
            model_prefetch_for_each_visual(object, preload);
        }
    }
    {
        // Every section whose visual is a monster (meshes\monsters): none of them is in prefetch_visuals_single
        for (CInifile::Sect* sect : pSettings->sections())
        {
            pcstr visual = nullptr;
            if (!sect->line_exist("visual", &visual) || !visual || !visual[0])
                continue;
            string_path low;
            xr_strcpy(low, visual);
            xr_strlwr(low);
            if (0 != strncmp(low, "monsters\\", 9))
                continue;
            preload(visual);
            pcstr predator = nullptr;
            if (sect->line_exist("Predator_Visual", &predator) && predator && predator[0])
                preload(predator);
        }
    }
    Msg("* [model_prefetch] preload: %u models loaded, %u already loaded or missing, %u ms", loaded, already,
        timer.GetElapsed_ms());
}
