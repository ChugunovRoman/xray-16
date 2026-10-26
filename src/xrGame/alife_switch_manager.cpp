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
#include "npc_cpp_profile.h"
#include <cstring>

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

// plans/lua_to_cpp/33-gw-eoc-alife-switch.md (diagnostics only, -npc_cpp_profile): the switch pass by object. While
// switch_object is on an object in a timed part of the pass, the can_switch_* stages of its kind;
// ENpcCppProfileStage::Count = not timed (the profiler is off, a part without the timers, or a call outside the pass)
namespace
{
// thread_local: the pass runs on one thread at a time (main or a seqParallel worker); a call of the same functions on
// another thread meanwhile is not taken for a part of the pass
thread_local ENpcCppProfileStage g_alife_switch_profile_can_online = ENpcCppProfileStage::Count;
thread_local ENpcCppProfileStage g_alife_switch_profile_can_offline = ENpcCppProfileStage::Count;
// a sync_location / go_* stage is open (a squad switches its members through the same calls)
thread_local bool g_alife_switch_profile_nested = false;

// section 9 (2026-10-25): what the current part of the pass gathers (CAlifeSwitchPassProfile) - nothing (no profiler, or not
// in a part), the counters only, or the counters and the timers of switch_object (every 8th part). The former timers
// on every object (two QPC and an npc_cpp_profile::add of six atomic operations each, several a object) made the pass
// under the profiler ~0.2 ms longer (plan 50, run 1)
enum EAlifeSwitchProfileMode : u8
{
    alife_switch_mode_off = 0,
    alife_switch_mode_counters,
    alife_switch_mode_timers,
};
thread_local u8 g_alife_switch_profile_mode = alife_switch_mode_off;
constexpr u32 ALIFE_SWITCH_PROFILE_SAMPLE_EVERY = 8;
u32 g_alife_switch_profile_parts = 0; // the parts so far (the pass never runs on two threads at once)

// The switch_pass/* stages (AlifeSwitchSquad .. AlifeSwitchSyncExitNoAi, one block of the enum) of the part,
// gathered without atomics and flushed into npc_cpp_profile when the part ends
constexpr u32 ALIFE_SWITCH_PROFILE_FIRST = static_cast<u32>(ENpcCppProfileStage::AlifeSwitchSquad);
constexpr u32 ALIFE_SWITCH_PROFILE_STAGES =
    static_cast<u32>(ENpcCppProfileStage::AlifeSwitchSyncExitNoAi) - ALIFE_SWITCH_PROFILE_FIRST + 1;

struct CAlifeSwitchProfileBucket
{
    u64 total_qpc;
    u64 calls;
    u64 max_qpc;
};
thread_local CAlifeSwitchProfileBucket g_alife_switch_profile_buckets[ALIFE_SWITCH_PROFILE_STAGES];

CAlifeSwitchProfileBucket& alife_switch_profile_bucket(ENpcCppProfileStage stage)
{
    const u32 index = static_cast<u32>(stage) - ALIFE_SWITCH_PROFILE_FIRST;
    VERIFY(index < ALIFE_SWITCH_PROFILE_STAGES);
    return g_alife_switch_profile_buckets[index];
}

// a timed sample (the callers time only in a timed part)
void alife_switch_profile_time(ENpcCppProfileStage stage, u64 qpc_delta)
{
    CAlifeSwitchProfileBucket& bucket = alife_switch_profile_bucket(stage);
    bucket.total_qpc += qpc_delta;
    ++bucket.calls;
    if (qpc_delta > bucket.max_qpc)
        bucket.max_qpc = qpc_delta;
}

// a counter, in any part of the pass with the profiler
void alife_switch_profile_count(ENpcCppProfileStage stage)
{
    if (g_alife_switch_profile_mode != alife_switch_mode_off)
        ++alife_switch_profile_bucket(stage).calls;
}

bool alife_switch_profile_is_counter(ENpcCppProfileStage stage)
{
    switch (stage)
    {
    case ENpcCppProfileStage::AlifeSwitchKeepSavedEmpty:
    case ENpcCppProfileStage::AlifeSwitchCanSkipped:
    case ENpcCppProfileStage::AlifeSwitchSyncFast:
    case ENpcCppProfileStage::AlifeSwitchSyncMemo:
    case ENpcCppProfileStage::AlifeSwitchSyncFull:
    case ENpcCppProfileStage::AlifeSwitchSyncFullUnchanged:
    case ENpcCppProfileStage::AlifeSwitchN3NativeCanOnline:
    case ENpcCppProfileStage::AlifeSwitchN3NativeCanOffline:
    case ENpcCppProfileStage::AlifeSwitchN3NativeKeepSaved:
    case ENpcCppProfileStage::AlifeSwitchN3NativeUsedAi:
    case ENpcCppProfileStage::AlifeSwitchN3LuaCanOnline:
    case ENpcCppProfileStage::AlifeSwitchN3LuaCanOffline:
    case ENpcCppProfileStage::AlifeSwitchN3LuaKeepSaved:
    case ENpcCppProfileStage::AlifeSwitchN3LuaUsedAi:
    case ENpcCppProfileStage::AlifeSwitchN3Luabind:
    case ENpcCppProfileStage::AlifeSwitchSyncExitAttached:
    case ENpcCppProfileStage::AlifeSwitchSyncExitOfflineInvalid:
    case ENpcCppProfileStage::AlifeSwitchSyncExitNoAi: return true;
    default: return false;
    }
}

void alife_switch_profile_flush()
{
    for (u32 i = 0; i < ALIFE_SWITCH_PROFILE_STAGES; ++i)
    {
        CAlifeSwitchProfileBucket& bucket = g_alife_switch_profile_buckets[i];
        if (!bucket.calls)
            continue;
        const auto stage = static_cast<ENpcCppProfileStage>(ALIFE_SWITCH_PROFILE_FIRST + i);
        if (alife_switch_profile_is_counter(stage))
            npc_cpp_profile::count(stage, bucket.calls);
        else
            npc_cpp_profile::add_many(stage, bucket.total_qpc, bucket.calls, bucket.max_qpc);
        bucket = {};
    }
}

// switch_object of one object: alife/switch_pass/<kind>. The kind by the virtual casts of CSE_Abstract (no RTTI), only
// in a timed part
class CAlifeSwitchProfileScope
{
public:
    explicit CAlifeSwitchProfileScope(CSE_ALifeDynamicObject* object)
    {
        m_stage = ENpcCppProfileStage::Count;
        if (g_alife_switch_profile_mode != alife_switch_mode_timers)
            return;
        m_saved_can_online = g_alife_switch_profile_can_online;
        m_saved_can_offline = g_alife_switch_profile_can_offline;
        if (object->cast_online_offline_group())
            set(ENpcCppProfileStage::AlifeSwitchSquad, ENpcCppProfileStage::AlifeSwitchCanOnlineSquad,
                ENpcCppProfileStage::AlifeSwitchCanOfflineSquad);
        else if (object->cast_human_abstract())
            set(ENpcCppProfileStage::AlifeSwitchStalker, ENpcCppProfileStage::AlifeSwitchCanOnlineCreature,
                ENpcCppProfileStage::AlifeSwitchCanOfflineCreature);
        else if (object->cast_monster_abstract())
            set(ENpcCppProfileStage::AlifeSwitchMonster, ENpcCppProfileStage::AlifeSwitchCanOnlineCreature,
                ENpcCppProfileStage::AlifeSwitchCanOfflineCreature);
        else if (object->cast_inventory_item())
            set(ENpcCppProfileStage::AlifeSwitchItem, ENpcCppProfileStage::AlifeSwitchCanOnlineOther,
                ENpcCppProfileStage::AlifeSwitchCanOfflineOther);
        else if (object->cast_smart_zone())
            set(ENpcCppProfileStage::AlifeSwitchSmartTerrain, ENpcCppProfileStage::AlifeSwitchCanOnlineOther,
                ENpcCppProfileStage::AlifeSwitchCanOfflineOther);
        else if (object->cast_anomalous_zone())
            set(ENpcCppProfileStage::AlifeSwitchAnomaly, ENpcCppProfileStage::AlifeSwitchCanOnlineOther,
                ENpcCppProfileStage::AlifeSwitchCanOfflineOther);
        else
            set(ENpcCppProfileStage::AlifeSwitchOther, ENpcCppProfileStage::AlifeSwitchCanOnlineOther,
                ENpcCppProfileStage::AlifeSwitchCanOfflineOther);
        m_start = CPU::QPC();
    }

    ~CAlifeSwitchProfileScope()
    {
        if (m_stage == ENpcCppProfileStage::Count)
            return;
        alife_switch_profile_time(m_stage, CPU::QPC() - m_start);
        g_alife_switch_profile_can_online = m_saved_can_online;
        g_alife_switch_profile_can_offline = m_saved_can_offline;
    }

    CAlifeSwitchProfileScope(const CAlifeSwitchProfileScope&) = delete;
    CAlifeSwitchProfileScope& operator=(const CAlifeSwitchProfileScope&) = delete;

private:
    void set(ENpcCppProfileStage stage, ENpcCppProfileStage can_online, ENpcCppProfileStage can_offline)
    {
        m_stage = stage;
        g_alife_switch_profile_can_online = can_online;
        g_alife_switch_profile_can_offline = can_offline;
    }

    ENpcCppProfileStage m_stage;
    ENpcCppProfileStage m_saved_can_online{ENpcCppProfileStage::Count};
    ENpcCppProfileStage m_saved_can_offline{ENpcCppProfileStage::Count};
    u64 m_start{0};
};

// A part of switch_object (sync_location, go_online, go_offline): only inside a profiled switch_object, the outermost
// one (a squad switches its members through the same calls). every_part (go_online, go_offline: rare and long, the
// timer costs nothing next to them) - in every part of the pass, not only in the timed ones
class CAlifeSwitchProfileNested
{
public:
    explicit CAlifeSwitchProfileNested(ENpcCppProfileStage stage, bool every_part = false)
    {
        m_active = (every_part ? g_alife_switch_profile_mode != alife_switch_mode_off :
                                 g_alife_switch_profile_can_online != ENpcCppProfileStage::Count) &&
            !g_alife_switch_profile_nested;
        if (!m_active)
            return;
        g_alife_switch_profile_nested = true;
        m_stage = stage;
        m_start = CPU::QPC();
    }

    ~CAlifeSwitchProfileNested()
    {
        if (!m_active)
            return;
        alife_switch_profile_time(m_stage, CPU::QPC() - m_start);
        g_alife_switch_profile_nested = false;
    }

    CAlifeSwitchProfileNested(const CAlifeSwitchProfileNested&) = delete;
    CAlifeSwitchProfileNested& operator=(const CAlifeSwitchProfileNested&) = delete;

private:
    bool m_active;
    ENpcCppProfileStage m_stage{ENpcCppProfileStage::Count};
    u64 m_start{0};
};

// section 9 (2026-10-25): synchronize_location of the pass for an object whose CSE_ALifeDynamicObject::synchronize_location
// (alife_dynamic_object.cpp) is not overridden - not a group (CSE_ALifeGroupAbstract, CSE_ALifeOnlineOfflineGroup: the
// only overrides). That call reads the position, the level and game vertices, the distance and the online flag of the
// object, the level graph and the cross table (fixed for a level), and writes only the same fields (and the graph
// registry, when the game vertex changes - then m_tGraphID changes too); the logs and the error dialog only on an
// invalid level vertex. So the call is a pure function of these fields: with the same input it does the same.
// The input of a call that changed nothing - the memo of the object; the object is taken for unchanged while its
// fields are bitwise the same. Only a call on a valid level vertex is remembered (on an invalid one it logs)
struct CAlifeSyncMemo
{
    Fvector position;
    u32 level_vertex;
    float distance;
    GameGraph::_GRAPH_ID game_vertex;
    bool online;
};

CAlifeSyncMemo alife_sync_memo_of(const CSE_ALifeDynamicObject* object)
{
    return {object->o_Position, object->m_tNodeID, object->m_fDistance, object->m_tGraphID, object->m_bOnline};
}

bool alife_sync_memo_same(const CAlifeSyncMemo& left, const CAlifeSyncMemo& right)
{
    // bitwise: the call computes on the bits, 0.f and -0.f (equal as floats) are not taken for one input
    return !std::memcmp(&left.position, &right.position, sizeof(left.position)) &&
        left.level_vertex == right.level_vertex && !std::memcmp(&left.distance, &right.distance, sizeof(float)) &&
        left.game_vertex == right.game_vertex && left.online == right.online;
}

// By object ID; an ID taken again by another object is harmless (the memo is of the input, not of the object). Only
// the pass touches it (one thread at a time); dropped with another level graph and with the simulator
xr_unordered_map<ALife::_OBJECT_ID, CAlifeSyncMemo> g_alife_sync_memo;
GameGraph::_LEVEL_ID g_alife_sync_memo_level = GameGraph::_LEVEL_ID(-1);

// The early returns of CSE_ALifeDynamicObject::synchronize_location that come before it writes anything, the same
// expressions: no position on the level graph, or the position is in its level vertex (the xz cell; the most of the
// objects that lie still)
bool alife_sync_location_stays(const CSE_ALifeDynamicObject* object)
{
    const CLevelGraph& level_graph = ai().level_graph();
    if (!level_graph.valid_vertex_position(object->o_Position))
        return true;
    return level_graph.valid_vertex_id(object->m_tNodeID) &&
        level_graph.inside(level_graph.vertex(object->m_tNodeID), object->o_Position);
}
} // namespace

CAlifeSwitchPassProfile::CAlifeSwitchPassProfile()
{
    if (!npc_cpp_profile::enabled())
        return;
    m_active = true;
    m_saved_mode = g_alife_switch_profile_mode;
    m_sampled = (g_alife_switch_profile_parts++ % ALIFE_SWITCH_PROFILE_SAMPLE_EVERY) == 0;
    g_alife_switch_profile_mode = m_sampled ? alife_switch_mode_timers : alife_switch_mode_counters;
    m_start = CPU::QPC();
}

CAlifeSwitchPassProfile::~CAlifeSwitchPassProfile()
{
    if (!m_active)
        return;
    const u64 delta = CPU::QPC() - m_start;
    g_alife_switch_profile_mode = m_saved_mode;
    alife_switch_profile_time(
        m_sampled ? ENpcCppProfileStage::AlifeSwitchPartSampled : ENpcCppProfileStage::AlifeSwitchPartClean, delta);
    // after the part: the report a flush may print is not a part of it
    alife_switch_profile_flush();
}

bool alife_switch_profiled_can_online(const CSE_ALifeObject* object)
{
    const ENpcCppProfileStage stage = g_alife_switch_profile_can_online;
    if (stage == ENpcCppProfileStage::Count)
        return object->can_switch_online();
    const u64 start = CPU::QPC();
    const bool result = object->can_switch_online();
    alife_switch_profile_time(stage, CPU::QPC() - start);
    return result;
}

bool alife_switch_profiled_can_offline(const CSE_ALifeObject* object)
{
    const ENpcCppProfileStage stage = g_alife_switch_profile_can_offline;
    if (stage == ENpcCppProfileStage::Count)
        return object->can_switch_offline();
    const u64 start = CPU::QPC();
    const bool result = object->can_switch_offline();
    alife_switch_profile_time(stage, CPU::QPC() - start);
    return result;
}

bool alife_switch_profiled_keep_saved(const CSE_ALifeDynamicObject* object)
{
    if (g_alife_switch_profile_mode == alife_switch_mode_off)
        return object->keep_saved_data_anyway();
    if (object->client_data.empty())
        alife_switch_profile_count(ENpcCppProfileStage::AlifeSwitchKeepSavedEmpty);
    if (g_alife_switch_profile_can_online == ENpcCppProfileStage::Count) // a part without the timers
        return object->keep_saved_data_anyway();
    const u64 start = CPU::QPC();
    const bool result = object->keep_saved_data_anyway();
    alife_switch_profile_time(ENpcCppProfileStage::AlifeSwitchKeepSaved, CPU::QPC() - start);
    return result;
}

// 33-gw-eoc-alife-switch.md, N1 (always on since 2026-10-25, its switch gw_alife_switch_skip_lua removed)
bool alife_switch_keep_client_data(const CSE_ALifeDynamicObject* object)
{
    // Nothing to clear: whatever keep_saved_data_anyway answers (every override of the mod returns a constant), the
    // client data stays as it is - empty
    if (object->client_data.empty())
    {
        alife_switch_profile_count(ENpcCppProfileStage::AlifeSwitchKeepSavedEmpty);
        return true;
    }
    return alife_switch_profiled_keep_saved(object);
}

void alife_switch_count_skipped() { alife_switch_profile_count(ENpcCppProfileStage::AlifeSwitchCanSkipped); }

// section 10 (N3, xrServer_script_macroses.h): how a wrapper of a server class answered one of the four methods
void alife_switch_count_default_call(u32 method, xr_luabind::EDefaultCall how)
{
    if (g_alife_switch_profile_mode == alife_switch_mode_off)
        return;
    static constexpr ENpcCppProfileStage native[] = {ENpcCppProfileStage::AlifeSwitchN3NativeCanOnline,
        ENpcCppProfileStage::AlifeSwitchN3NativeCanOffline, ENpcCppProfileStage::AlifeSwitchN3NativeKeepSaved,
        ENpcCppProfileStage::AlifeSwitchN3NativeUsedAi};
    static constexpr ENpcCppProfileStage lua[] = {ENpcCppProfileStage::AlifeSwitchN3LuaCanOnline,
        ENpcCppProfileStage::AlifeSwitchN3LuaCanOffline, ENpcCppProfileStage::AlifeSwitchN3LuaKeepSaved,
        ENpcCppProfileStage::AlifeSwitchN3LuaUsedAi};
    if (method >= std::size(native))
        return;
    if (how == xr_luabind::EDefaultCall::native)
        alife_switch_profile_count(native[method]);
    else if (how == xr_luabind::EDefaultCall::lua)
        alife_switch_profile_count(lua[method]);
    else
        alife_switch_profile_count(ENpcCppProfileStage::AlifeSwitchN3Luabind);
}

CALifeSwitchManager::~CALifeSwitchManager()
{
    // section 9: the memo of synchronize_location belongs to this simulator
    g_alife_sync_memo.clear();
    g_alife_sync_memo_level = GameGraph::_LEVEL_ID(-1);
}
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
    const CAlifeSwitchProfileNested profile(ENpcCppProfileStage::AlifeSwitchGoOnline, true); // 33-gw-eoc-alife-switch.md
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
    const CAlifeSwitchProfileNested profile(ENpcCppProfileStage::AlifeSwitchGoOffline, true); // 33-gw-eoc-alife-switch.md
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

    // 33-gw-eoc-alife-switch.md section 9 (always on since 2026-10-25, its switch gw_alife_switch_skip_lua removed): every
    // way out of this function returns true and the checks below only read the object (used_ai_locations - a Lua call
    // for a script class, no override in the mod). When the synchronize_location of the object would return before
    // changing anything (alife_sync_location_stays, or the memo: the same input as a call that changed nothing), the
    // result is "true, nothing changed" whatever the checks answer - neither they nor the graph search are needed.
    // Groups and squads keep the old sequence (the else branch below)
    const bool plain = !I->cast_group_abstract() && !I->cast_online_offline_group();
    if (plain)
    {
        if (alife_sync_location_stays(I))
        {
            alife_switch_profile_count(ENpcCppProfileStage::AlifeSwitchSyncFast);
            return (true);
        }
        const GameGraph::_LEVEL_ID level_id = graph().level().level_id();
        if (g_alife_sync_memo_level != level_id)
        {
            g_alife_sync_memo.clear();
            g_alife_sync_memo_level = level_id;
        }
        const auto memo = g_alife_sync_memo.find(I->ID);
        if (memo != g_alife_sync_memo.end() && alife_sync_memo_same(memo->second, alife_sync_memo_of(I)))
        {
            alife_switch_profile_count(ENpcCppProfileStage::AlifeSwitchSyncMemo);
            return (true);
        }
        // section 10: the three checks below in another order - first the two that read only fields of the object,
        // then the virtual used_ai_locations (a wrapper of a script class; the C++ default since N3, but still a Lua
        // lookup). Each of them returns true and none writes, so the order does not change the result (the same
        // assumption as above: used_ai_locations has no side effects)
        if (0xffff != I->ID_Parent)
        {
            alife_switch_profile_count(ENpcCppProfileStage::AlifeSwitchSyncExitAttached);
            return (true);
        }
        if (!I->m_bOnline && !ai().level_graph().valid_vertex_id(I->m_tNodeID))
        {
            alife_switch_profile_count(ENpcCppProfileStage::AlifeSwitchSyncExitOfflineInvalid);
            return (true);
        }
        if (!I->used_ai_locations())
        {
            alife_switch_profile_count(ENpcCppProfileStage::AlifeSwitchSyncExitNoAi);
            return (true);
        }
    }
    else
    {
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
    }

    alife_switch_profile_count(ENpcCppProfileStage::AlifeSwitchSyncFull);
    const bool valid_vertex = ai().level_graph().valid_vertex_id(I->m_tNodeID);
    const CAlifeSyncMemo input = alife_sync_memo_of(I);
    const bool result = (*I).synchronize_location();
    if (valid_vertex && result && alife_sync_memo_same(input, alife_sync_memo_of(I)))
    {
        alife_switch_profile_count(ENpcCppProfileStage::AlifeSwitchSyncFullUnchanged);
        g_alife_sync_memo[I->ID] = input;
    }
    else
        g_alife_sync_memo.erase(I->ID);
    return (result);
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

    // alife_switch_keep_client_data: the virtual keep_saved_data_anyway, not asked when there is no client data
    // (33-gw-eoc-alife-switch.md, N1), timed with -npc_cpp_profile
    if (!I->m_bOnline && !alife_switch_keep_client_data(I))
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
    const CAlifeSwitchProfileScope profile(I); // 33-gw-eoc-alife-switch.md, -npc_cpp_profile only

    if (I->redundant())
    {
        release(I);
        return;
    }

    bool synchronized;
    {
        const CAlifeSwitchProfileNested profile_sync(ENpcCppProfileStage::AlifeSwitchSyncLocation);
        synchronized = synchronize_location(I);
    }
    if (!synchronized)
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
    // 50l-weak-cpu-spawn-models.md (plans/optimization_mt): source bit 2 - a creature: on a weak CPU (D3D11) the render
    // also loads its base model in the background once the files are read. Not items: a section with world_material_N
    // registers its texture replacements right before its first model_Create, a base loaded earlier would miss them.
    if (smart_cast<CSE_ALifeCreatureAbstract*>(object))
        source |= 2;
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

bool model_prefetch_spawn_pending(u16 id)
{
    if (id == u16(-1) || !ai().get_alife())
        return false;
    CSE_ALifeDynamicObject* object = ai().alife().objects().object(id, true);
    if (!object)
        return false;
    // 50l: a creature is asked as a spawn first with the creature bit (PrefetchPending asks without it), so one that
    // comes online without an ALife prefetch gets its background load too
    if (smart_cast<CSE_ALifeCreatureAbstract*>(object))
        model_prefetch_files_of(object, 1);
    // every visual is asked (each queues its job), not up to the first pending one
    bool pending = false;
    model_prefetch_for_each_visual(
        object, [&pending](pcstr name) { pending = GEnv.Render->model_PrefetchPending(name) || pending; });
    return pending;
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
