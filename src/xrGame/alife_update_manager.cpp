////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_update_manager.h
//	Created 	: 25.12.2002
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife Simulator update manager
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "addon_object_events.h"
#include "alife_update_manager.h"
#include "addon_event_bus.h"
#include "addon_api_alife_ext.h"
#include "alife_simulator_header.h"
#include "alife_time_manager.h"
#include "alife_graph_registry.h"
#include "alife_schedule_registry.h"
#include "alife_spawn_registry.h"
#include "alife_object_registry.h"
#include "ef_storage.h"
#include "xrServer.h"
#include "Level.h"
#include "xrAICore/Navigation/graph_engine.h"
#include "restriction_space.h"
#include "xrEngine/profiler.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "mt_config.h"
#include "npc_cpp_profile.h"
#include "xrNetServer/NET_Messages.h"
#include "xrServerEntities/xrServer_Objects_Alife_Smartcovers.h"
#include "Checker.h"
#include "ai_space.h"
#include "xrAICore/Navigation/PatrolPath/patrol_path_storage.h"
#include "xrAICore/Navigation/PatrolPath/patrol_path.h"
#include "xrAICore/Navigation/PatrolPath/patrol_point.h"

using namespace ALife;

extern string_path g_last_saved_game;
extern Checker g_checker;

class CSwitchPredicate
{
private:
    CALifeSwitchManager* m_switch_manager;

public:
    IC CSwitchPredicate(CALifeSwitchManager* switch_manager) { m_switch_manager = switch_manager; }
    IC bool operator()(CALifeLevelRegistry::_iterator& i, u64 cycle_count, bool) const
    {
        if ((*i).second->m_switch_counter == cycle_count)
            return (false);

        (*i).second->m_switch_counter = cycle_count;
        return (true);
    }

    IC void operator()(CALifeLevelRegistry::_iterator& i, u64 cycle_count) const
    {
        m_switch_manager->switch_object((*i).second);
    }
};

// plans/optimization_mt/32-frame-spikes.md, A1 (always on since 2026-10-18, its switch alife_update_spread removed,
// 41-cvar-cleanup.md; the tunables below are not saved, console_commands.cpp): the ALife update no longer runs as one piece when its turn in the scheduler comes. In GW ALife
// sits in the far heap of the scheduler (xrSheduler.cpp, the services are always far) and gets its turn once in ~5 s
// under load; that call (alife/update) took 18-35 ms of the Game thread: the full online/offline switch pass
// (alife/switches ~2600) and objects_per_update (20) scheduled objects, ~1 ms each (smart terrains, more when they
// spawn squads). Now a turn (shedule_Update -> spread_tick) only gives the work - the tokens of objects_per_update
// objects (a bucket, at most 4 ticks) and one switch pass over the level registry - and every unpaused frame
// (CLevel::OnGameLogicFrame -> spread_frame -> update_spread, seqParallel as before) does a part of it within
// alife_scheduled_budget_us: first the switch pass, then the scheduled objects; at least one object of each a frame
// while it has work, so both move on even when one object takes the whole budget. The registries are rings
// (CSafeMapIterator) and go on from where the last part stopped, so the average throughput is that of the turns, as
// before; the work is only spread over the frames between them. The first update after a load and the precache frames
// run update() as before.
// The time limit of the switch pass: set_process_time (process_time of alife.ltx, al_process_time, Lua) gives the
// level registry microseconds where CSafeMapIterator::time_over compares seconds (500 us -> ~500 s), so the legacy pass
// is never cut - kept as it is: cut at 450 us, a pass of the turn once in 5 s would visit only a part of the level a
// turn. The spread pass has its own limit (CSwitchSpreadPredicate, alife_scheduled_budget_us).
int alife_scheduled_budget_us = 1500;
// 32-frame-spikes.md: alife_switch_offline_per_frame_max N > 0 (temporary, not saved, 0 = no limit as before) - the
// spread switch pass (A1) takes no more objects offline in one frame than the client can destroy
// cheaply: it stops before an object whose going offline would bring the client objects destroyed by this frame's pass
// (the object with all its children - the NPC inventory; a squad with all its members: CSE_ALifeOnlineOfflineGroup goes
// offline as a whole and cannot be split) over N, and the next frame goes on from it (the registry is a ring, the
// object is not marked as visited). The first object that goes offline in a frame always goes, so a frame takes at least
// one squad. The cost is paid by main in the next frame (game events -> destroy_queue -> CObjectList relcase, net_Destroy)
// without any limit, ~0.2-0.3 ms an object (the wave of 2026-10-17: 120-138 objects, 70-77 ms frames). Only offline:
// going online is already spread by the client spawn queue (spawn budget). The full pass of update() (the first update
// after a load, the precache frames) is not limited.
// 37-destroy-relcase-smooth.md (2026-10-18): the estimate alone did not hold the wave of s_al.log (window 48: 30 frames
// x ~97 destroyed objects, alife/switch_offline_frame max 133, alife/switch_offline_deferred never counted). Why is not
// seen in the log; the estimate is 0 for an object it takes for staying online, and switch_offline_estimate does not
// ask can_switch_online (an object that may not stay online goes offline at any distance). So the pass also stops
// once what already went this call reached N, whatever the estimate of the next object; the first object of a call
// still always goes, a squad is never split (the stop is before an object, never inside its switch). alife/switch_offline_spread / alife/switch_offline_unestimated show how
// much of the wave came through this pass and how much of it the estimate missed.
// s_ao.log (2026-10-18): alife/switch_offline_spread stayed 0 through a wave of ~5000 objects - it was not this pass but
// the gw_release plugin (Plugin API alife_switch_offline, 32 corpses a frame with their inventories); limited there by
// client objects. alife/switch_offline_plugin / _teleport / _full tell the paths apart.
int alife_switch_offline_per_frame_max = 32; // a tunable, not saved (the default since 2026-10-17, 41-cvar-cleanup.md)

class CSwitchSpreadPredicate
{
private:
    CALifeSwitchManager* m_switch_manager;
    u32 m_count;
    u64 m_deadline;
    u32 m_offline_max;

public:
    mutable u32 m_done;
    mutable bool m_wrapped; // stopped on an object switched in this call: the call got around the whole registry
    mutable u32 m_offline_done; // client objects destroyed by the objects this call took offline
    mutable bool m_offline_stopped; // stopped by alife_switch_offline_per_frame_max
    mutable u32 m_estimate; // switch_offline_estimate of the object being switched (0 with the limit off)

    IC CSwitchSpreadPredicate(CALifeSwitchManager* switch_manager, u32 count, u64 deadline, u32 offline_max)
    {
        m_switch_manager = switch_manager;
        m_count = count;
        m_deadline = deadline;
        m_offline_max = offline_max;
        m_done = 0;
        m_wrapped = false;
        m_offline_done = 0;
        m_offline_stopped = false;
        m_estimate = 0;
    }

    IC bool operator()(CALifeLevelRegistry::_iterator& i, u64 cycle_count, bool) const
    {
        if ((*i).second->m_switch_counter == cycle_count)
        {
            m_wrapped = true;
            return (false);
        }

        if (m_done >= m_count)
            return (false);

        // one object always goes
        if (m_done > 0 && CPU::QPC() >= m_deadline)
            return (false);

        // the offline limit: only once something went offline in this call (the first one always goes) - reached
        // already, or the object would bring it over N by its estimate
        m_estimate = 0;
        if (m_offline_max)
        {
            if (m_offline_done >= m_offline_max)
            {
                m_offline_stopped = true;
                return (false);
            }
            m_estimate = m_switch_manager->switch_offline_estimate((*i).second);
            if (m_offline_done && m_estimate && m_offline_done + m_estimate > m_offline_max)
            {
                m_offline_stopped = true;
                return (false);
            }
        }

        ++m_done;
        (*i).second->m_switch_counter = cycle_count;
        return (true);
    }

    IC void operator()(CALifeLevelRegistry::_iterator& i, u64 cycle_count) const
    {
        const u32 before = m_switch_manager->m_offline_client_objects.load(std::memory_order_relaxed);
        m_switch_manager->switch_object((*i).second);
        const u32 went = m_switch_manager->m_offline_client_objects.load(std::memory_order_relaxed) - before;
        if (!went)
            return;
        m_offline_done += went;
        PERF_ADD(alife_switch_offline_spread, went);
        if (m_offline_max && !m_estimate)
            PERF_ADD(alife_switch_offline_unestimated, went);
    }
};

CALifeUpdateManager::CALifeUpdateManager(IPureServer* server, LPCSTR section)
    : CALifeSimulatorBase(server, section), CALifeSwitchManager(server, section),
      CALifeSurgeManager(server, section), CALifeStorageManager(server, section)
{
    shedule.t_min = pSettings->r_s32(section, "schedule_min");
    shedule.t_max = pSettings->r_s32(section, "schedule_max");
    shedule_register();

    m_max_process_time = pSettings->r_s32(section, "process_time");
    m_update_monster_factor = pSettings->r_float(section, "update_monster_factor");
    m_objects_per_update = pSettings->r_u32(section, "objects_per_update");
    m_position_update_interval_ms = pSettings->read_if_exists<u32>(section, "position_update_interval_ms", 100);
    m_last_position_update_time = 0;
    m_changing_level = false;
    m_first_time = true;
    m_spread_tokens = 0;
    m_spread_switch_left = 0;
}

CALifeUpdateManager::~CALifeUpdateManager()
{
    shedule_unregister();
    Device.remove_from_seq_parallel(fastdelegate::FastDelegate0<>(this, &CALifeUpdateManager::update));
    Device.remove_from_seq_parallel(fastdelegate::FastDelegate0<>(this, &CALifeUpdateManager::update_spread));
}

float CALifeUpdateManager::shedule_Scale() const
{
    return (.5f); // (schedule_min + schedule_max)*0.5f
}

void CALifeUpdateManager::update_switch()
{
    init_ef_storage();

    // Switching an object can release it (and whatever it drags along); keep the objects alive
    // until the pass is over, the code that asked for the release is still standing on them.
    const destruction_lock lock(*this);

    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::AlifeUpdateSwitch);
    START_PROFILE("ALife/switch");
    // Spawn stage 0 (plans/spawn_optimization/SPEC.md): how many M_SPAWN one pass sends (a group of a squad and all the
    // inventories is one pass): the gauge keeps the last pass, its window max is the biggest pass
    const u32 spawns_before = m_online_spawns.load(std::memory_order_relaxed);
#if XR_PERF_METRICS
    // 37-destroy-relcase-smooth.md: what the full pass took offline (alife/switch_offline_full)
    const u32 offline_before = m_offline_client_objects.load(std::memory_order_relaxed);
#endif
    graph().level().update(CSwitchPredicate(this), Device.dwPrecacheFrame > 0);
    PERF_ADD(alife_switch_offline_full, m_offline_client_objects.load(std::memory_order_relaxed) - offline_before);
    xray::perf::SetGauge(xray::perf::Metric::alife_switch_pass_spawns,
        static_cast<s64>(m_online_spawns.load(std::memory_order_relaxed) - spawns_before));
    STOP_PROFILE
}

void CALifeUpdateManager::update_scheduled(bool init_ef)
{
    if (init_ef)
        init_ef_storage();

    // An object's update - the scripted squads in particular - regularly ends up releasing itself
    // (sim_squad_scripted:remove_npc / remove_squad on the last member). Postpone the delete until
    // the whole pass is done, otherwise the rest of the update runs on freed memory.
    const destruction_lock lock(*this);

    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::AlifeUpdateScheduled);
    // The size of the registry once a pass: count/frame of the row is the average size (one pass a frame)
    if (npc_cpp_profile::enabled())
        npc_cpp_profile::count(ENpcCppProfileStage::AlifeScheduledRegistrySize, scheduled().objects().size());
    START_PROFILE("ALife/scheduled");
    scheduled().update();
    STOP_PROFILE
}

void CALifeUpdateManager::update()
{
    ZoneScopedN("alife_update");
    PERF_SCOPE(alife_update);
    // -npc_cpp_profile: the whole update and how many of them were long (no histograms in the profiler). The time may
    // include a report printed from a nested stage (flush_if_needed), so up to one long update per 5 s window can be
    // the report itself.
    const bool profile = npc_cpp_profile::enabled();
    const u64 profile_start = profile ? CPU::QPC() : 0;
    update_switch();
    update_scheduled(false);
    if (profile)
    {
        const u64 delta = CPU::QPC() - profile_start;
        npc_cpp_profile::add(ENpcCppProfileStage::AlifeUpdate, delta);
        if (delta * 1000 > 2 * CPU::qpc_freq)
            npc_cpp_profile::count(ENpcCppProfileStage::AlifeUpdateOver2ms);
        if (delta * 1000 > 5 * CPU::qpc_freq)
            npc_cpp_profile::count(ENpcCppProfileStage::AlifeUpdateOver5ms);
    }
}

void CALifeUpdateManager::update_positions_only() const
{
    if (!initialized())
        return;
    const u32 now = Device.dwTimeGlobal;
    if (m_position_update_interval_ms > 0 && m_last_position_update_time != 0 &&
        (now - m_last_position_update_time) < m_position_update_interval_ms)
        return;
    m_last_position_update_time = now;

    START_PROFILE("ALife/update_positions");
    for (const auto& it : scheduled().objects())
        it.second->update_position();
    STOP_PROFILE
}

void CALifeUpdateManager::shedule_Update(u32 dt)
{
    ScheduledBase::shedule_Update(dt);

    if (!initialized())
        return;

    // 32-frame-spikes.md, A1: the turn only gives the work to the frames (see alife_scheduled_budget_us)
    if (!m_first_time && Device.dwPrecacheFrame == 0)
    {
        spread_tick();
        return;
    }

    if (!m_first_time && g_mt_config.test(mtALife))
    {
        if (GEnv.Render->GetBackendAPI() == IRender::BackendAPI::OpenGL)
            CALifeUpdateManager::update();
        else
            Device.seqParallel.push_back(fastdelegate::FastDelegate0<>(this, &CALifeUpdateManager::update));

        return;
    }

    m_first_time = false;

    START_PROFILE("ALife/update")
    update();
    STOP_PROFILE
}

// 32-frame-spikes.md, A1: a turn of the scheduler (alife/spread_ticks). The Game thread
void CALifeUpdateManager::spread_tick()
{
    PERF_INC(alife_spread_ticks);
    // The scheduled objects: objects_per_update a turn, as the former update. The bucket keeps at most 4 turns: when
    // the frames cannot keep up (alife_scheduled_budget_us too small for the objects) the rest is dropped instead of
    // piling up (alife/spread_dropped). Not more than the registry holds: the former update took each object at most once
    // a turn, and the wrap check of update_limited works only inside one frame's part
    const u32 per_turn =
        std::min(scheduled().objects_per_update(), static_cast<u32>(scheduled().objects().size()));
    const u32 cap = 4 * std::max(per_turn, 1u);
    u32 tokens = m_spread_tokens + per_turn;
    if (tokens > cap)
    {
        PERF_ADD(alife_spread_dropped, tokens - cap);
        tokens = cap;
    }
    m_spread_tokens = tokens;
    // The switch pass: the whole level registry once a turn, as the former update. A pass the frames have not finished
    // is not added to: it starts over at the size of the registry (alife/spread_switch_unfinished)
    if (m_spread_switch_left)
        PERF_INC(alife_spread_switch_unfinished);
    m_spread_switch_left = static_cast<u32>(graph().level().objects().size());
}

void CALifeUpdateManager::spread_frame()
{
    if (!initialized() || m_first_time || Device.dwPrecacheFrame > 0)
        return;
    if (!m_spread_tokens && !m_spread_switch_left)
        return;

    // The same place as the update of a turn (shedule_Update): seqParallel of this frame, on this thread later on
    if (g_mt_config.test(mtALife) && GEnv.Render->GetBackendAPI() != IRender::BackendAPI::OpenGL)
        Device.seqParallel.push_back(fastdelegate::FastDelegate0<>(this, &CALifeUpdateManager::update_spread));
    else
        update_spread();
}

// 32-frame-spikes.md, A1: a part of the work of the turns within alife_scheduled_budget_us (alife/update: calls =
// frames with a part, max = the longest part; alife/update_switch, alife/update_scheduled - its two halves)
void CALifeUpdateManager::update_spread()
{
    ZoneScopedN("alife_update_spread");
    PERF_SCOPE(alife_update);
    if (!initialized())
        return;
    PERF_INC(alife_spread_frames);
    const bool profile = npc_cpp_profile::enabled();
    const u64 start = CPU::QPC();
    const u64 deadline =
        start + static_cast<u64>(std::max(alife_scheduled_budget_us, 0)) * CPU::qpc_freq / 1000000;

    init_ef_storage();

    if (m_spread_switch_left)
    {
        // As update_switch: the objects a switch releases are deleted when this part ends
        const destruction_lock lock(*this);
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::AlifeUpdateSwitch);
        ZoneScopedN("alife_update_spread/switch");
        PERF_SCOPE(alife_update_switch);
        const u32 spawns_before = m_online_spawns.load(std::memory_order_relaxed);
        const CSwitchSpreadPredicate predicate(this, m_spread_switch_left, deadline,
            static_cast<u32>(std::max(alife_switch_offline_per_frame_max, 0)));
        graph().level().update(predicate, false);
        xray::perf::SetGauge(xray::perf::Metric::alife_switch_pass_spawns,
            static_cast<s64>(m_online_spawns.load(std::memory_order_relaxed) - spawns_before));
        if (predicate.m_offline_stopped)
            PERF_INC(alife_switch_offline_deferred);
        m_spread_switch_left =
            predicate.m_wrapped ? 0 : m_spread_switch_left - std::min(predicate.m_done, m_spread_switch_left);
    }

    if (m_spread_tokens)
    {
        // As update_scheduled: an object's update may release it (or its squad); deleted when this part ends
        const destruction_lock lock(*this);
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::AlifeUpdateScheduled);
        ZoneScopedN("alife_update_spread/scheduled");
        PERF_SCOPE(alife_update_scheduled);
        bool wrapped = false;
        const u32 done = scheduled().update_limited(m_spread_tokens, deadline, wrapped);
        // around the whole registry: the former update too never took an object twice a turn
        m_spread_tokens = wrapped ? 0 : m_spread_tokens - std::min(done, m_spread_tokens);
    }
    xray::perf::SetGauge(xray::perf::Metric::alife_spread_tokens, static_cast<s64>(m_spread_tokens));

    if (profile)
    {
        const u64 delta = CPU::QPC() - start;
        npc_cpp_profile::add(ENpcCppProfileStage::AlifeUpdate, delta);
        if (delta * 1000 > 2 * CPU::qpc_freq)
            npc_cpp_profile::count(ENpcCppProfileStage::AlifeUpdateOver2ms);
        if (delta * 1000 > 5 * CPU::qpc_freq)
            npc_cpp_profile::count(ENpcCppProfileStage::AlifeUpdateOver5ms);
    }
}

void CALifeUpdateManager::set_process_time(int microseconds)
{
    graph().set_process_time(float(microseconds) - float(microseconds) * update_monster_factor() / 1000000.f);
}

void CALifeUpdateManager::objects_per_update(const u32& objects_per_update)
{
    scheduled().objects_per_update(objects_per_update);
}

void CALifeUpdateManager::init_ef_storage() const { ai().ef_storage().alife_evaluation(true); }
bool CALifeUpdateManager::change_level(NET_Packet& net_packet)
{
    if (m_changing_level)
        return (false);

    luabind::functor<void> funct;
    if (GEnv.ScriptEngine->functor("_G.CALifeUpdateManager__on_before_change_level", funct))
        funct(&net_packet);

    //	prepare_objects_for_save		();
    // we couldn't use prepare_objects_for_save since we need
    // get updates from client
    // then change actor server entity
    // then call client net_Save
    // then restore actor server entity
    Level().ClientSend();

    m_changing_level = true;

    GameGraph::_GRAPH_ID safe_graph_vertex_id = graph().actor()->m_tGraphID;
    u32 safe_level_vertex_id = graph().actor()->m_tNodeID;
    Fvector safe_position = graph().actor()->o_Position;
    Fvector safe_angles = graph().actor()->o_Angle;
    SRotation safe_torso = graph().actor()->o_torso;

    GameGraph::_GRAPH_ID holder_safe_graph_vertex_id = GameGraph::_GRAPH_ID(-1);
    u32 holder_safe_level_vertex_id = u32(-1);
    Fvector holder_safe_position = Fvector().set(flt_max, flt_max, flt_max);
    Fvector holder_safe_angles = Fvector().set(flt_max, flt_max, flt_max);
    CSE_ALifeObject* holder = 0;

    net_packet.r(&graph().actor()->m_tGraphID, sizeof(graph().actor()->m_tGraphID));
    net_packet.r(&graph().actor()->m_tNodeID, sizeof(graph().actor()->m_tNodeID));
    net_packet.r_vec3(graph().actor()->o_Position);
    net_packet.r_vec3(graph().actor()->o_Angle);

    // on_level_changing (stage B-4): the server actor already points at the new level (utils.level_changing() is
    // true, axr_companions teleports the companions to its vertex), the client save comes next. Before stage B the
    // Lua actor binder sent it from its save() inside ClientSave.
    gw::addons::objevents::LevelChanging();

    Level().SpawnQueueNoteSave("level change"); // spawn stage 2: see CALifeStorageManager::prepare_objects_for_save
    Level().ClientSave();

    graph().actor()->o_torso.yaw = graph().actor()->o_Angle.y;
    graph().actor()->o_torso.pitch = graph().actor()->o_Angle.x;
    graph().actor()->o_torso.roll = 0.f;

    if (graph().actor()->m_holderID != 0xffff)
    {
        holder = objects().object(graph().actor()->m_holderID);

        holder_safe_graph_vertex_id = holder->m_tGraphID;
        holder_safe_level_vertex_id = holder->m_tNodeID;
        holder_safe_position = holder->o_Position;
        holder_safe_angles = holder->o_Angle;

        holder->m_tGraphID = graph().actor()->m_tGraphID;
        holder->m_tNodeID = graph().actor()->m_tNodeID;
        holder->o_Position = graph().actor()->o_Position;
        holder->o_Angle = graph().actor()->o_Angle;
    }

    string256 autoave_name;
    strconcat(sizeof(autoave_name), autoave_name, Core.UserName, " - ", "autosave");
    LPCSTR temp0 = strchr(m_server_command_line->c_str(), '/');
    VERIFY(temp0);
    string256 temp;
    *m_server_command_line = strconcat(sizeof(temp), temp, autoave_name, temp0);

    save(autoave_name);
    gw::addons::events::MarkLevelChange(); // the next ALife start (loading this autosave) is a level change

    graph().actor()->m_tGraphID = safe_graph_vertex_id;
    graph().actor()->m_tNodeID = safe_level_vertex_id;
    graph().actor()->o_Position = safe_position;
    graph().actor()->o_Angle = safe_angles;
    graph().actor()->o_torso = safe_torso;

    if (graph().actor()->m_holderID != 0xffff)
    {
        VERIFY(holder);
        holder->m_tGraphID = holder_safe_graph_vertex_id;
        holder->m_tNodeID = holder_safe_level_vertex_id;
        holder->o_Position = holder_safe_position;
        holder->o_Angle = holder_safe_angles;
    }

    return (true);
}

#include "xrEngine/IGame_Persistent.h"
void CALifeUpdateManager::new_game(LPCSTR save_name)
{
    g_pGamePersistent->LoadTitle("st_creating_new_game");
    Msg("* Creating new game...");

    unload();
    reload(m_section);
    spawns().load(save_name);
    graph().on_load();
    server().PerformIDgen(0x0000);
    time_manager().init(m_section);
    VERIFY(can_register_objects());

    can_register_objects(false);
    spawn_new_objects();
    can_register_objects(true);

    CALifeObjectRegistry::OBJECT_REGISTRY::iterator I = objects().objects().begin();
    CALifeObjectRegistry::OBJECT_REGISTRY::iterator E = objects().objects().end();
    for (; I != E; ++I)
    {
        (*I).second->on_register();
        gw::addons::alife_ext::OnServerObjectRegistered((*I).second); // server_object_on_register
    }

#ifdef DEBUG
    save(save_name);
#endif // #ifdef DEBUG

    Msg("* New game is successfully created!");

    if (strstr(Core.Params, "-checks"))
        checks();
}

void CALifeUpdateManager::load(LPCSTR game_name, bool no_assert, bool new_only)
{
    g_pGamePersistent->LoadTitle("st_loading_alife_simulator");

#ifdef DEBUG
    Memory.mem_compact();
    u32 memory_usage = Memory.mem_usage();
#endif

    xr_strcpy(g_last_saved_game, game_name);

    if (new_only || !CALifeStorageManager::load(game_name))
    {
        R_ASSERT3(new_only || (no_assert && xr_strlen(game_name)), "Cannot find the specified saved game ", game_name);
        new_game(game_name);
    }

    if (g_pGameLevel)
        Level().OnAlifeSimulatorLoaded();

#ifdef DEBUG
    Msg("* Loading alife simulator is successfully completed (%7.3f Mb)",
        float(Memory.mem_usage() - memory_usage) / 1048576.0);
#endif
    g_pGamePersistent->LoadTitle("st_server_connecting", true, g_pGameLevel->name());
}

void CALifeUpdateManager::reload(LPCSTR section)
{
    CALifeSimulatorBase::reload(section);
    m_position_update_interval_ms = pSettings->read_if_exists<u32>(section, "position_update_interval_ms", 100);
    set_process_time((int)m_max_process_time);
    objects_per_update(m_objects_per_update);
}

bool CALifeUpdateManager::load_game(LPCSTR game_name, bool no_assert)
{
    {
        string_path temp, file_name;
        strconcat(sizeof(temp), temp, game_name, SAVE_EXTENSION);
        FS.update_path(file_name, "$game_saves$", temp);
        if (!FS.exist(file_name))
        {
            strconcat(sizeof(temp), temp, game_name, SAVE_EXTENSION_LEGACY);
            FS.update_path(file_name, "$game_saves$", temp);
            if (!FS.exist(file_name))
            {
                R_ASSERT3(no_assert, "There is no saved game ", game_name);
                return (false);
            }
        }
    }

    string512 S, S1;
    xr_strcpy(S, m_server_command_line->c_str());
    pstr temp = strchr(S, '/');
    R_ASSERT2(temp, "Invalid server options!");
    strconcat(sizeof(S1), S1, game_name, temp);
    *m_server_command_line = S1;
    return (true);
}

void CALifeUpdateManager::set_switch_online(ALife::_OBJECT_ID id, bool value)
{
    CSE_ALifeDynamicObject* object = objects().object(id);
    VERIFY(object);
    object->can_switch_online(value);
}

void CALifeUpdateManager::set_switch_offline(ALife::_OBJECT_ID id, bool value)
{
    CSE_ALifeDynamicObject* object = objects().object(id);
    VERIFY(object);
    object->can_switch_offline(value);
}

void CALifeUpdateManager::set_interactive(ALife::_OBJECT_ID id, bool value)
{
    CSE_ALifeDynamicObject* object = objects().object(id);
    VERIFY(object);
    object->interactive(value);
}

void CALifeUpdateManager::jump_to_level(LPCSTR level_name) const
{
    const CGameGraph::SLevel& level = ai().game_graph().header().level(level_name);
    GameGraph::_GRAPH_ID dest = GameGraph::_GRAPH_ID(-1);
    GraphEngineSpace::CGameLevelParams evaluator(level.id());
    bool failed = !ai().graph_engine().search(
        ai().game_graph(), graph().actor()->m_tGraphID, GameGraph::_GRAPH_ID(-1), 0, evaluator);
    if (failed)
    {
#ifndef MASTER_GOLD
        Msg("! Cannot build path via game graph from the current level to the level %s!", level_name);
#endif // #ifndef MASTER_GOLD
        float min_dist = flt_max;
        Fvector current = ai().game_graph().vertex(graph().actor()->m_tGraphID)->game_point();
        GameGraph::_GRAPH_ID n = ai().game_graph().header().vertex_count();
        for (GameGraph::_GRAPH_ID i = 0; i < n; ++i)
            if (ai().game_graph().vertex(i)->level_id() == level.id())
            {
                float distance = ai().game_graph().vertex(i)->game_point().distance_to_sqr(current);
                if (distance < min_dist)
                {
                    min_dist = distance;
                    dest = i;
                }
            }
        if (!ai().game_graph().vertex(dest))
        {
            Msg("! There is no game vertices on the level %s, cannot jump to the specified level", level_name);
            return;
        }
    }
    else
        dest = (GameGraph::_GRAPH_ID)evaluator.selected_vertex_id();
    NET_Packet net_packet;
    net_packet.w_begin(M_CHANGE_LEVEL);
    net_packet.w(&dest, sizeof(dest));

    u32 vertex_id = ai().game_graph().vertex(dest)->level_vertex_id();
    net_packet.w(&vertex_id, sizeof(vertex_id));

    Fvector level_point = ai().game_graph().vertex(dest)->level_point();
    net_packet.w(&level_point, sizeof(level_point));
    net_packet.w_vec3(Fvector().set(0.f, 0.f, 0.f));
    Level().Send(net_packet, net_flags(TRUE));
}

void CALifeUpdateManager::teleport_object(
    ALife::_OBJECT_ID id, GameGraph::_GRAPH_ID game_vertex_id, u32 level_vertex_id, const Fvector& position)
{
    CSE_ALifeDynamicObject* object = objects().object(id, true);
    if (!object)
    {
        Msg("! cannot teleport entity with id %d", id);
        return;
    }

#ifdef DEBUG
    if (psAI_Flags.test(aiALife))
    {
        Msg("[LSS] teleporting object [%s][%s][%d] from level [%s], position [%f][%f][%f] to level [%s], position "
            "[%f][%f][%f]",
            object->name_replace(), object->s_name.c_str(), object->ID,
            ai().game_graph().header().level(ai().game_graph().vertex(object->m_tGraphID)->level_id()).name().c_str(),
            VPUSH(ai().game_graph().vertex(object->m_tGraphID)->level_point()),
            ai().game_graph().header().level(ai().game_graph().vertex(game_vertex_id)->level_id()).name().c_str(),
            VPUSH(ai().game_graph().vertex(game_vertex_id)->level_point()));
    }
#endif

    if (object->m_bOnline)
    {
#if XR_PERF_METRICS
        // 37-destroy-relcase-smooth.md: an online object teleported goes offline first (alife/switch_offline_teleport)
        const u32 offline_before = m_offline_client_objects.load(std::memory_order_relaxed);
#endif
        switch_offline(object);
        PERF_ADD(alife_switch_offline_teleport, m_offline_client_objects.load(std::memory_order_relaxed) - offline_before);
    }
    graph().change(object, object->m_tGraphID, game_vertex_id);
    object->m_tNodeID = level_vertex_id;
    object->o_Position = position;
    CSE_ALifeMonsterAbstract* monster_abstract = smart_cast<CSE_ALifeMonsterAbstract*>(object);
    if (monster_abstract)
        monster_abstract->m_tNextGraphID = object->m_tGraphID;
}

void CALifeUpdateManager::add_restriction(
    ALife::_OBJECT_ID id, ALife::_OBJECT_ID restriction_id, const RestrictionSpace::ERestrictorTypes& restriction_type)
{
    CSE_ALifeDynamicObject* object = objects().object(id, true);
    if (!object)
    {
        Msg("! cannot add restriction with id %d to the entity with id %d, because there is no creature with the "
            "specified "
            "id",
            restriction_id, id);
        return;
    }

    CSE_ALifeDynamicObject* object_restrictor = objects().object(restriction_id, true);
    if (!object_restrictor)
    {
        Msg("! cannot add restriction with id %d to the entity with id %d, because there is no space restrictor with "
            "the "
            "specified id",
            restriction_id, id);
        return;
    }

    CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(object);
    if (!creature)
    {
        Msg("! cannot add restriction with id %d to the entity with id %d, because there is an object with the "
            "specified "
            "id, but it is not a creature",
            restriction_id, id);
        return;
    }

    CSE_ALifeSpaceRestrictor* restrictor = smart_cast<CSE_ALifeSpaceRestrictor*>(object_restrictor);
    if (!restrictor)
    {
        Msg("! cannot add restriction with id %d to the entity with id %d, because there is an object with the "
            "specified "
            "id, but it is not a space restrictor",
            restriction_id, id);
        return;
    }

    switch (restriction_type)
    {
    case RestrictionSpace::eRestrictorTypeOut: {
#ifdef DEBUG
        if (std::find(creature->m_dynamic_out_restrictions.begin(), creature->m_dynamic_out_restrictions.end(),
                restriction_id) != creature->m_dynamic_out_restrictions.end())
        {
            Msg("! cannot add out-restriction with id %d, name %s to the entity with id %d, name %s, because it is "
                "already "
                "added",
                restriction_id, restrictor->name_replace(), id, creature->name_replace());
            return;
        }
#endif

        creature->m_dynamic_out_restrictions.push_back(restriction_id);

        break;
    }
    case RestrictionSpace::eRestrictorTypeIn: {
#ifdef DEBUG
        if (std::find(creature->m_dynamic_in_restrictions.begin(), creature->m_dynamic_in_restrictions.end(),
                restriction_id) != creature->m_dynamic_in_restrictions.end())
        {
            xrDebug::LogStackTrace("cannot add in-restriction stack trace");
            Msg("! cannot add in-restriction with id %d, name %s to the entity with id %d, name %s, because it is "
                "already "
                "added",
                restriction_id, restrictor->name_replace(), id, creature->name_replace());
            Msg("! Please report this log file to Lain");
            return;
        }
#endif

        creature->m_dynamic_in_restrictions.push_back(restriction_id);

        break;
    }
    default:
    {
        Msg("! Invalid restriction type!");
        return;
    }
    }
}

void CALifeUpdateManager::remove_restriction(
    ALife::_OBJECT_ID id, ALife::_OBJECT_ID restriction_id, const RestrictionSpace::ERestrictorTypes& restriction_type)
{
    CSE_ALifeDynamicObject* object = objects().object(id, true);
    if (!object)
    {
        Msg("! cannot remove restriction with id %d to the entity with id %d, because there is no creature with the "
            "specified id",
            restriction_id, id);
        return;
    }

    CSE_ALifeDynamicObject* object_restrictor = objects().object(restriction_id, true);
    if (!object_restrictor)
    {
        Msg("! cannot remove restriction with id %d to the entity with id %d, because there is no space restrictor "
            "with "
            "the specified id",
            restriction_id, id);
        return;
    }

    CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(object);
    if (!creature)
    {
        Msg("! cannot remove restriction with id %d to the entity with id %d, because there is an object with the "
            "specified id, but it is not a creature",
            restriction_id, id);
        return;
    }

    CSE_ALifeSpaceRestrictor* restrictor = smart_cast<CSE_ALifeSpaceRestrictor*>(object_restrictor);
    if (!restrictor)
    {
        Msg("! cannot remove restriction with id %d to the entity with id %d, because there is an object with the "
            "specified id, but it is not a space restrictor",
            restriction_id, id);
        return;
    }

    switch (restriction_type)
    {
    case RestrictionSpace::eRestrictorTypeOut:
    {
        xr_vector<ALife::_OBJECT_ID>::iterator I = std::find(
            creature->m_dynamic_out_restrictions.begin(), creature->m_dynamic_out_restrictions.end(), restriction_id);
        if (I == creature->m_dynamic_out_restrictions.end())
        {
            Msg("~ cannot remove restriction with id [%d][%s] to the entity with id [%d][%s], because it is not added",
                restriction_id, object_restrictor->name_replace(), id, object->name_replace());
            return;
        }

        creature->m_dynamic_out_restrictions.erase(I);

        break;
    }
    case RestrictionSpace::eRestrictorTypeIn:
    {
        xr_vector<ALife::_OBJECT_ID>::iterator I = std::find(
            creature->m_dynamic_in_restrictions.begin(), creature->m_dynamic_in_restrictions.end(), restriction_id);
        if (I == creature->m_dynamic_in_restrictions.end())
        {
            Msg("~ cannot remove restriction with id [%d][%s] to the entity with id [%d][%s], because it is not added",
                restriction_id, object_restrictor->name_replace(), id, object->name_replace());
            return;
        }

        creature->m_dynamic_in_restrictions.erase(I);

        break;
    }
    default:
    {
        Msg("! Invalid restriction type!");
        return;
    }
    }
}

void CALifeUpdateManager::remove_all_restrictions(
    ALife::_OBJECT_ID id, const RestrictionSpace::ERestrictorTypes& restriction_type)
{
    CSE_ALifeDynamicObject* object = objects().object(id, true);
    if (!object)
    {
        Msg("! cannot remove restrictions to the entity with id %d, because there is no creature with the specified id",
            id);
        return;
    }

    CSE_ALifeCreatureAbstract* creature = smart_cast<CSE_ALifeCreatureAbstract*>(object);
    if (!creature)
    {
        Msg("! cannot remove restriction to the entity with id %d, because there is an object with the specified id, "
            "but "
            "it is not a creature",
            id);
        return;
    }

    switch (restriction_type)
    {
    case RestrictionSpace::eRestrictorTypeOut:
    {
        creature->m_dynamic_out_restrictions.clear();
        break;
    }
    case RestrictionSpace::eRestrictorTypeIn:
    {
        creature->m_dynamic_in_restrictions.clear();
        break;
    }
    default: NODEFAULT;
    }
}

void CALifeUpdateManager::checks()
{
    g_checker.logic_files["sr_teleport_sections.ltx"] = true;

    xr_map<shared_str, bool> exists_cover_names;
    CALifeObjectRegistry::OBJECT_REGISTRY::iterator I = objects().objects().begin();
    CALifeObjectRegistry::OBJECT_REGISTRY::iterator E = objects().objects().end();
    for (; I != E; ++I)
    {
        CSE_SmartCover* smart_cover = smart_cast<CSE_SmartCover*>((*I).second);
        CSE_ALifeSmartZone* smart_terrain = smart_cast<CSE_ALifeSmartZone*>((*I).second);
        if (smart_cover)
        {
            exists_cover_names[smart_cover->name_replace()] = true;
            g_checker.AddToDictLog(Dicts::SmartCovers, smart_cover->name_replace());
        }
        if (smart_terrain)
            g_checker.AddToDictLog(Dicts::SmartTerrains, smart_terrain->name_replace());

        if (xr_strcmp((*I).second->s_name.c_str(), "campfire") == 0)
            g_checker.AddToDictLog(Dicts::Campfires, (*I).second->name_replace());
        if (xr_strcmp((*I).second->s_name.c_str(), "inventory_box") == 0)
            g_checker.AddToDictLog(Dicts::InventoryBox, (*I).second->name_replace());
        if (xr_strcmp((*I).second->s_name.c_str(), "space_restrictor") == 0)
            g_checker.AddToDictLog(Dicts::SpaceRestrictor, (*I).second->name_replace());
        if (xr_strcmp((*I).second->s_name.c_str(), "anomal_zone") == 0)
            g_checker.AddToDictLog(Dicts::AnomalZone, (*I).second->name_replace());
    }

    for (const auto& I : g_checker.logic_files)
    {
        string_path fn;
        FS.update_path(fn, "$game_config$", I.first.c_str());
        CInifile* ini_file = xr_new<CInifile>(fn, true, true, false);
        for (const auto& section : ini_file->sections())
        {
            u32 count = ini_file->line_count(section->Name);
            for (u32 c = 0; c < count; c++)
            {
                LPCSTR key, value;
                ini_file->r_line(section->Name, c, &key, &value);
                if (value && key && strstr(key, "cover_name"))
                {
                    auto it = exists_cover_names.find(value);
                    if (it == exists_cover_names.end())
                        g_checker.AddToCheckLog(Checks::SmartCovers, make_string("! Сover by name: %s doesn't exist! in file: %s, section: %s", value, I.first.c_str(), section->Name.c_str()).c_str());
                }
            }

        }
    }

    exists_cover_names.clear();
}
