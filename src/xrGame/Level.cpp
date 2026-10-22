#include "pch_script.h"
#include "addon_binders.h"
#include "addon_event_bus.h"
#include "addon_goap.h"
#include "addon_timers.h"
#include "lua_spike_probe.h"
#include <tracy/Tracy.hpp>
#include "xrEngine/FDemoRecord.h"
#include "xrEngine/FDemoPlay.h"
#include "xrEngine/Environment.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/EngineThreading.h"
#include "ParticlesObject.h"
#include "Level.h"
#include "VisionBatch.h"
#include "Include/xrRender/Kinematics.h"
#include "inventory_item.h" // render prepare of held items: CLevel::BonesPrepass collect step
#include "HudItem.h"
#include "PhysicsShellHolder.h" // pipelined bones prepass: physics shells (EarlyPhysicsAllowed)
#include "CharacterPhysicsSupport.h"
#include "xrPhysics/PhysicsShell.h"
#include "xrCore/Threading/ParallelFor.hpp"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "xrCore/BackgroundIo.h"
#include "xrCore/Platform/HwProfile.h"
#include "HUDManager.h"
#include "xrServer.h"
#include "NET_Queue.h"
#include "game_cl_base.h"
#include "entity_alive.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "ai_debug.h"
#include "ShootingObject.h"
#include "GametaskManager.h"
#include "Level_Bullet_Manager.h"
#include "xrScriptEngine/script_process.hpp"
#include "xrScriptEngine/script_engine.hpp"
#include "team_base_zone.h"
#include "InfoPortion.h"
#include "xrAICore/Navigation/PatrolPath/patrol_path_storage.h"
#include "date_time.h"
#include "space_restriction_manager.h"
#include "seniority_hierarchy_holder.h"
#include "space_restrictor.h"
#include "client_spawn_manager.h"
#include "autosave_manager.h"
#include "ClimableObject.h"
#include "xrAICore/Navigation/level_graph.h"
#include "mt_config.h"
#include "script_entity.h"
#include "map_manager.h"
#include "xrEngine/CameraManager.h"
#include "xrEngine/Render.h"
#include "level_sounds.h"
#include "Car.h"
#include "trade_parameters.h"
#include "game_cl_base_weapon_usage_statistic.h"
#include "MainMenu.h"
#include "xrEngine/XR_IOConsole.h"
#include "Actor.h"
#include "Explosive.h"
#include "player_hud.h"
#include "ui/UIGameTutorial.h"
#include "file_transfer.h"
#include "Message_Filter.h"
#include "DemoPlay_Control.h"
#include "DemoInfo.h"
#include "CustomDetector.h"
#include "xrPhysics/IPHWorld.h"
#include "xrPhysics/PHCommander.h"
#include "xrPhysics/console_vars.h"
#include "xrNetServer/NET_Messages.h"
#include "xrEngine/GameFont.h"

#include "level_debug.h"
#include "ai/stalker/ai_stalker.h"
#include "debug_renderer.h"
#include "PhysicObject.h"
#include "PHDebug.h"
#include "debug_text_tree.h"
#ifdef DEBUG
#include "LevelGraphDebugRender.hpp"
#endif

extern CUISequencer* g_tutorial;
extern CUISequencer* g_tutorial2;

// P19 (plans/optimization_mt/14-spike-instrumentation.md): one phase of CLevel::OnFrame - Tracy zone, frame/level/*
// timer of perf_metrics and the spike logger (perf_spike_us, with the CPU time of the thread). One per block.
#define LEVEL_FRAME_PHASE(zone, metric) ZoneScopedN(zone); PERF_SCOPE(metric); PERF_SPIKE_PHASE(zone)

float g_cl_lvInterp = 0.1;
u32 lvInterpSteps = 0;

namespace
{
// Pipelined bones prepass (26-framemove-game-next.md): the UpdateCL batch hooks, defined with the prepass below
bool EarlyBegin(IGameObject* const* b, IGameObject* const* e, u32 batch_size);
void EarlyBatchDone(u32 batch);
void EarlyEnd();
} // namespace

CLevel::CLevel()
    : IPureClient(Device.GetTimerGlobal())
#ifdef CONFIG_PROFILE_LOCKS
      ,
      DemoCS(MUTEX_PROFILE_ID(DemoCS))
#endif
{
    ZoneScoped;

    g_bDebugEvents = strstr(Core.Params, "-debug_ge") != nullptr;
    game_events = xr_new<NET_Queue_Event>();
    eChangeRP = Engine.Event.Handler_Attach("LEVEL:ChangeRP", this);
    eDemoPlay = Engine.Event.Handler_Attach("LEVEL:PlayDEMO", this);
    eChangeTrack = Engine.Event.Handler_Attach("LEVEL:PlayMusic", this);
    eEnvironment = Engine.Event.Handler_Attach("LEVEL:Environment", this);
    eEntitySpawn = Engine.Event.Handler_Attach("LEVEL:spawn", this);
    m_pBulletManager = xr_new<CBulletManager>();
    if (!GEnv.isDedicatedServer)
    {
        m_map_manager = xr_new<CMapManager>();
        m_game_task_manager = xr_new<CGameTaskManager>();
    }
    m_dwDeltaUpdate = u32(fixed_step * 1000);
    m_seniority_hierarchy_holder = xr_new<CSeniorityHierarchyHolder>();
    if (!GEnv.isDedicatedServer)
    {
        m_level_sound_manager = xr_new<CLevelSoundManager>();
        m_space_restriction_manager = xr_new<CSpaceRestrictionManager>();
        m_client_spawn_manager = xr_new<CClientSpawnManager>();
        m_autosave_manager = xr_new<CAutosaveManager>();
        m_debug_renderer = xr_new<CDebugRenderer>();
#ifdef DEBUG
        levelGraphDebugRender = xr_new<LevelGraphDebugRender>();
#endif
        // m_level_debug = xr_new<CLevelDebug>();
    }
    m_ph_commander = xr_new<CPHCommander>();
    m_ph_commander_scripts = xr_new<CPHCommander>();
    // 32-frame-spikes.md: per-call Tracy zones, ph_commander/* metrics and the "level/add_call/call" spike site
    m_ph_commander_scripts->set_profiled(true);
    pObjects4CrPr.clear();
    pActors4CrPr.clear();
    pHUD = xr_new<CHUDManager>();
    g_player_hud[0] = xr_new<player_hud>();
    g_player_hud[0]->load_default();
    g_player_hud[1] = xr_new<player_hud>();
    g_player_hud[1]->load_default();

    ObjectSpace.SetValidateObjectCallback([](u16 id) -> IGameObject* {
        IGameObject* O = Level().Objects.net_Find(id);
        return (O && !O->getDestroy()) ? O : nullptr;
    });

    Device.PostSchedulerVisionBatch = &CLevel::VisionBatchPostScheduler;
    Device.BonesPrepassCallback = &CLevel::BonesPrepass;
    Device.BonesPrepassJoinCallback = &CLevel::BonesPrepassEarlyKickJoin;
    // Pipelined bones prepass (26-framemove-game-next.md): hooks of the UpdateCL batch loop
    Objects.update_batch_hooks.begin = &EarlyBegin;
    Objects.update_batch_hooks.batch_done = &EarlyBatchDone;
    Objects.update_batch_hooks.end = &EarlyEnd;
}

CLevel::~CLevel()
{
    ZoneScoped;

    Device.PostSchedulerVisionBatch = nullptr;
    // Early kick: nothing may still compute this level's models (normally joined in the frame of the kick already)
    BonesPrepassEarlyKickJoin();
    Device.BonesPrepassCallback = nullptr;
    Device.BonesPrepassJoinCallback = nullptr;
    Objects.update_batch_hooks = {};

    xr_delete(g_player_hud[0]);
    xr_delete(g_player_hud[1]);
    xr_delete(pHUD);
    delete_data(hud_zones_list);
    hud_zones_list = nullptr;
    Msg("- Destroying level");
    Engine.Event.Handler_Detach(eEntitySpawn, this);
    Engine.Event.Handler_Detach(eEnvironment, this);
    Engine.Event.Handler_Detach(eChangeTrack, this);
    Engine.Event.Handler_Detach(eDemoPlay, this);
    Engine.Event.Handler_Detach(eChangeRP, this);
    if (physics_world())
    {
        destroy_physics_world();
    }
    // destroy PSs
    for (auto& ps : m_StaticParticles)
        CParticlesObject::Destroy(ps);
    m_StaticParticles.clear();
    // Unload sounds
    // unload prefetched sounds
    sound_registry.clear();
    // unload static sounds
    for (auto& sound : static_Sounds)
    {
        sound->destroy();
        xr_delete(sound);
    }
    static_Sounds.clear();
    xr_delete(m_level_sound_manager);
    xr_delete(m_space_restriction_manager);
    xr_delete(m_seniority_hierarchy_holder);
    xr_delete(m_client_spawn_manager);
    xr_delete(m_autosave_manager);
    xr_delete(m_debug_renderer);
#ifdef DEBUG
    xr_delete(levelGraphDebugRender);
#endif
    if (!GEnv.isDedicatedServer)
        GEnv.ScriptEngine->remove_script_process(ScriptProcessor::Level);
    xr_delete(game);
    xr_delete(game_events);
    xr_delete(m_pBulletManager);
    xr_delete(pStatGraphR);
    xr_delete(pStatGraphS);
    xr_delete(m_ph_commander);
    xr_delete(m_ph_commander_scripts);
    pObjects4CrPr.clear();
    pActors4CrPr.clear();
    ai().unload();
#ifdef DEBUG
    xr_delete(m_level_debug);
#endif
    xr_delete(m_map_manager);
    delete_data(m_game_task_manager);
    // here we clean default trade params
    // because they should be new for each saved/loaded game
    // and I didn't find better place to put this code in
    // XXX nitrocaster: find better place for this clean()
    CTradeParameters::clean();
    if (g_tutorial && g_tutorial->m_pStoredInputReceiver == this)
        g_tutorial->m_pStoredInputReceiver = nullptr;
    if (g_tutorial2 && g_tutorial2->m_pStoredInputReceiver == this)
        g_tutorial2->m_pStoredInputReceiver = nullptr;
    if (IsDemoPlay())
    {
        StopPlayDemo();
        if (m_reader)
        {
            FS.r_close(m_reader);
            m_reader = nullptr;
        }
    }
    xr_delete(m_msg_filter);
    xr_delete(m_demoplay_control);
    xr_delete(m_demo_info);
    if (IsDemoSave())
    {
        StopSaveDemo();
    }
    deinit_compression();
}

shared_str CLevel::name() const { return map_data.m_name; }
void CLevel::GetLevelInfo(CServerInfo* si)
{
    if (Server && game)
    {
        Server->GetServerInfo(si);
    }
}

void CLevel::PrefetchSound(LPCSTR name)
{
    // preprocess sound name
    string_path tmp;
    xr_strcpy(tmp, name);
    xr_strlwr(tmp);
    if (strext(tmp))
        *strext(tmp) = 0;
    shared_str snd_name = tmp;
    // find in registry
    auto it = sound_registry.find(snd_name);
    // if find failed - preload sound
    if (it == sound_registry.end())
        sound_registry[snd_name].create(snd_name.c_str(), st_Effect, sg_SourceType);
}

// Game interface ////////////////////////////////////////////////////
int CLevel::get_RPID(LPCSTR /**name**/)
{
    /*
    // Gain access to string
    LPCSTR	params = pLevel->r_string("respawn_point",name);
    if (0==params)	return -1;

    // Read data
    Fvector4	pos;
    int			team;
    sscanf		(params,"%f,%f,%f,%d,%f",&pos.x,&pos.y,&pos.z,&team,&pos.w); pos.y += 0.1f;

    // Search respawn point
    svector<Fvector4,maxRP>	&rp = Level().get_team(team).RespawnPoints;
    for (int i=0; i<(int)(rp.size()); ++i)
    if (pos.similar(rp[i],EPS_L))	return i;
    */
    return -1;
}

bool g_bDebugEvents = false;

void CLevel::cl_Process_Event(u16 dest, u16 type, NET_Packet& P)
{
    ZoneNamedN(___tracy_cl_process_event, "CLevel::cl_Process_Event", true);
    ZoneTextVF(___tracy_cl_process_event, "type=%u dest=%u", (unsigned)type, (unsigned)dest);

    // Msg("--- event[%d] for [%d]",type,dest);
    IGameObject* O = nullptr;
    {
        ZoneNamedN(___tracy_cl_pe_find_dest, "CLevel::cl_Process_Event/net_Find_dest", true);
        O = Objects.net_Find(dest);
    }
    if (0 == O)
    {
        // Spawn stage 0 (plans/spawn_optimization/SPEC.md 3.3): an event to an object the client does not have is dropped.
        // Hits to just destroyed objects are normal; a lost GE_OWNERSHIP_TAKE leaves the item without its owner on the
        // client, so that one is logged in every build.
        PERF_INC(ge_unknown_dest);
        if (type == GE_OWNERSHIP_TAKE)
        {
            const u32 pos = P.r_tell();
            const u16 child = P.r_u16();
            P.r_seek(pos);
            IGameObject* child_object = Objects.net_Find(child);
            Msg("! WARNING: GE_OWNERSHIP_TAKE to [%u] that is not on the client: object [%u][%s] stays without its owner",
                (unsigned)dest, (unsigned)child, child_object ? child_object->cNameSect().c_str() : "not on the client");
        }
#ifdef DEBUG
        Msg("* WARNING: c_EVENT[%d] to [%d]: unknown dest", type, dest);
#endif
        return;
    }
    CGameObject* GO = smart_cast<CGameObject*>(O);
    if (!GO)
    {
#ifndef MASTER_GOLD
        Msg("! ERROR: c_EVENT[%d] : non-game-object", dest);
#endif
        return;
    }
    if (type != GE_DESTROY_REJECT)
    {
        if (type == GE_DESTROY)
        {
            ZoneNamedN(___tracy_cl_pe_ondestroy, "CLevel::cl_Process_Event/Game_OnDestroy", true);
            Game().OnDestroy(GO);
        }
        {
            ZoneNamedN(___tracy_cl_pe_onevent, "CLevel::cl_Process_Event/GO_OnEvent", true);
            GO->OnEvent(P, type);
        }
    }
    else
    {
        ZoneNamedN(___tracy_cl_pe_reject, "CLevel::cl_Process_Event/GE_DESTROY_REJECT", true);
        // handle GE_DESTROY_REJECT here
        u32 pos = P.r_tell();
        u16 id = P.r_u16();
        P.r_seek(pos);
        bool ok = true;
        IGameObject* D = nullptr;
        {
            ZoneNamedN(___tracy_cl_pe_find_id, "CLevel::cl_Process_Event/destroy_reject/net_Find_id", true);
            D = Objects.net_Find(id);
        }
        if (0 == D)
        {
#ifndef MASTER_GOLD
            Msg("! ERROR: c_EVENT[%d] : unknown dest", id);
#endif
            ok = false;
        }
        CGameObject* GD = smart_cast<CGameObject*>(D);
        if (!GD)
        {
#ifndef MASTER_GOLD
            Msg("! ERROR: c_EVENT[%d] : non-game-object", id);
#endif
            ok = false;
        }
        {
            ZoneNamedN(___tracy_cl_pe_rej_evt, "CLevel::cl_Process_Event/destroy_reject/OnOwnershipReject", true);
            GO->OnEvent(P, GE_OWNERSHIP_REJECT);
        }
        if (ok)
        {
            ZoneNamedN(___tracy_cl_pe_rej_des, "CLevel::cl_Process_Event/destroy_reject/OnDestroy_chain", true);
            Game().OnDestroy(GD);
            GD->OnEvent(P, GE_DESTROY);
        }
    }
}

// plans/spawn_optimization/SPEC.md 8.6: the head M_SPAWN waits while its ID is held by an object marked for destruction
// (it is destroyed by CObjectList::Update of this very frame, right after ProcessGameEvents). The whole queue behind it
// waits too, so the order of events is kept: its children come after it, events to it after it. If the old object is
// still there after this many frames (should never happen: the destroy queue is processed every frame), stop waiting -
// a stalled queue would freeze every spawn and event of the game; g_sv_Spawn then skips the spawn with an error.
constexpr u32 spawn_id_wait_frames_max = 30;

bool CLevel::SpawnMustWait(u16 id, pcstr section)
{
    if (GameID() != eGameIDSingle || id == u16(-1))
        return false;
    const IGameObject* holder = Objects.net_Find(id);
    // 30-game-events.md, B1 (ol_destroy_child_scan_batch 1): a child of an object destroyed earlier in this pass is
    // marked by the walk only - do it now, it may be this one (nothing queued: returns at once)
    if (holder && !holder->getDestroy())
        Objects.flush_destroy_child_scan();
    if (!holder || !holder->getDestroy())
        return false; // the ID is free, or held by a live object (a real duplicate: g_sv_Spawn skips it with an error)

    SpawnIdWait& wait = m_spawn_id_wait;
    if (!wait.waiting || wait.id != id)
    {
        wait.id = id;
        wait.section = section;
        wait.first_frame = Device.dwFrame;
        wait.frames = 0;
        wait.waiting = true;
        PERF_INC(spawn_id_reuse_deferred);
        Msg("* spawn: id[%u] [%s] is still held by [%s][%s] being destroyed; the spawn and the %u events behind it wait "
            "for the next frame", (unsigned)id, section, holder->cNameSect().c_str(), holder->cName().c_str(),
            (unsigned)(game_events->size() - 1));
        return true;
    }
    wait.frames = Device.dwFrame - wait.first_frame;
    if (wait.frames <= spawn_id_wait_frames_max)
        return true;

    Msg("! ERROR: spawn: id[%u] [%s] is still held by [%s][%s] after %u frames; stop waiting, the spawn will be skipped",
        (unsigned)id, section, holder->cNameSect().c_str(), holder->cName().c_str(), wait.frames);
    wait.waiting = false;
    return false;
}

void CLevel::SpawnWaitDone(u16 id)
{
    SpawnIdWait& wait = m_spawn_id_wait;
    if (!wait.waiting || wait.id != id)
        return;
    wait.waiting = false;
    const IGameObject* object = Objects.net_Find(id);
    if (object && !object->getDestroy())
        Msg("* spawn: id[%u] [%s] materialized after waiting %u frame(s)", (unsigned)id, object->cNameSect().c_str(),
            Device.dwFrame - wait.first_frame);
    else
        Msg("! ERROR: spawn: id[%u] [%s] waited %u frame(s) but did not materialize", (unsigned)id,
            wait.section.c_str(), Device.dwFrame - wait.first_frame);
}

// Spawn stage 2 (plans/spawn_optimization/SPEC.md 5.A, section 12): the budget of the client materialization of M_SPAWN
// per frame. Saved console variables (console_commands.cpp); spawn_budget_us 0 = as before: the whole queue in one
// frame.
int g_spawn_budget_us = 5000; // time of one pass of ProcessGameEvents (M_EVENT count too) after which no group starts
int g_spawn_min_per_frame = 2; // groups materialized in every frame whatever the budget: the queue always moves
int g_spawn_max_delay_ms = 500; // a head that waits longer goes on past the budget (player actions, quest items)...
// ...but only up to spawn_catchup_us per pass: a wave of 1000+ spawns (session 2026-10-12) otherwise went in one frame
// once its head was 500 ms old (forced/delay 1431 groups, 740 ms frame); the backlog now drains at this rate.
// 0 = no limit (the old behaviour).
int g_spawn_catchup_us = 20000;
// 36-spawn-model-load.md (plans/optimization_mt), spawn_cold_models - a tunable limit, not saved, 0 = off: once a
// budgeted pass has made this many cold model loads (CModelPool::Create loading an ogf, ~12 ms each for a monster), it
// stops in front of the next root M_SPAWN - also before spawn_min_per_frame groups and past spawn_max_delay_ms - so a
// burst of new models spreads over frames. One group a frame always goes; a group is never split (an NPC with cold
// weapons still loads them together).
int g_spawn_cold_models = 1; // the default since 2026-10-17

// 50i-stage3-slow-disk-io.md (plans/optimization_mt, plan 50 R6), a slow disk only: the hw policy's spawn_cold_ms is 0
// (an HDD / USB / network disk of gamedata) or the start-up warm-up measured the disk slow. A group with no parent (a
// creature or an object coming online - not an item given to someone) does not start while the "Model prefetch"
// thread still has to read the files of its models: they are queued ahead of the ALife jobs, and the group with
// everything behind it waits for the next frames, as with the budget. Instead of a 50-400 ms cold load of an ogf from an
// HDD in one frame the NPC appears a little later. At most until the head has spent spawn_warm_max_delay_ms in the
// client queue (then it goes on, cold); not on the loading screen, on pause or without the spawn budget. 1.5 s: three
// times spawn_max_delay_ms - an HDD reads a model (ogf, LOD, omf; no dds there) in ~50-150 ms, a group has 1-4 models,
// and the thread may be in the middle of a model of ALife. On an SSD / NVMe (spawn_cold_ms 6 / 8) the spawns do not
// wait: the limits there are spawn_cold_models and the time limit spawn_cold_ms (ProcessGameEvents), as before.
constexpr u32 spawn_warm_max_delay_ms = 1500;

namespace
{
// An M_SPAWN joins the open group when its parent is the root or a child already in it (the server sends the
// inventory right behind its owner: alife_trader_abstract.cpp, add_online_impl)
bool spawn_joins_group(const xr_vector<u16>& group, u16 parent)
{
    return parent != u16(-1) && std::find(group.begin(), group.end(), parent) != group.end();
}

// Groups and M_SPAWN in the queue, by the rule of ProcessGameEvents. `group` is a scratch vector.
void count_spawn_groups(const xr_deque<NET_Event>& queue, xr_vector<u16>& group, u32& groups, u32& spawns)
{
    group.clear();
    groups = 0;
    spawns = 0;
    for (const NET_Event& event : queue)
    {
        if (event.ID != M_SPAWN)
            continue;
        ++spawns;
        if (!spawn_joins_group(group, event.spawn_parent))
        {
            ++groups;
            group.clear();
        }
        if (event.spawn_id != u16(-1))
            group.push_back(event.spawn_id);
    }
    group.clear();
}

// How long the event has been in the client queue, ms (real time)
u32 queue_wait_ms(const NET_Event& event, u32 now_ms) { return now_ms - event.time_queued; }

// 50i (R6): the group the spawns wait for (main thread)
struct SpawnWarmWait
{
    u16 id = u16(-1); // the root M_SPAWN
    bool waiting = false;
    u64 t0 = 0; // xray::perf::Ticks() of the first stopped pass (metrics)
};
SpawnWarmWait s_spawn_warm_wait;
xr_vector<u16> s_spawn_warm_group; // scratch: the IDs of the group asked about

// 50i: the spawns wait for the warm-up of their models (a slow disk). The "* [hw] io" line once per process.
bool spawn_warm_wait_mode()
{
    const xray::hw::Policy& policy = xray::hw::GetPolicy();
    const bool by_policy = xray::hw::PolicyValid(policy) && policy.spawn_cold_ms == 0;
    const bool measured = xray::io::BackgroundIoMeasuredSlow();
    if (!by_policy && !measured)
        return false;
    static bool s_logged = false;
    if (!s_logged)
    {
        s_logged = true;
        Msg("* [hw] io: slow disk -> single background queue, spawn waits for warm-up (max %u ms)%s",
            spawn_warm_max_delay_ms, by_policy ? "" : " (measured at the start-up warm-up)");
    }
    return true;
}

// 50i (R6): the time limit of the cold model loads of a budgeted pass, us; 0 = none (no policy, or the slow disk's
// spawn_cold_ms 0: the spawns wait for the warm-up instead)
u64 spawn_cold_us_limit()
{
    const xray::hw::Policy& policy = xray::hw::GetPolicy();
    return xray::hw::PolicyValid(policy) ? u64(policy.spawn_cold_ms) * 1000 : 0;
}

// 50l-weak-cpu-spawn-models.md (plans/optimization_mt): a weak CPU (the hw policy's cpu tier low / potato), where a cold
// model load costs 100-350 ms of this thread. At most one per spawn_cold_frames frames (low 3, potato 4): a group with
// no parent does not start while the last cold model load of this thread is more recent than that - of a spawn
// (spawn_cold_models stops the pass right after one) or outside the pass (cNameVisual_set of an update...). The group
// and everything behind it wait, as with the budget: NPCs appear a few frames later. Not with spawn_cold_models 0 (the
// console switch of the cold limits). With the background base loading (D3D11 with DriverConcurrentCreates, the render)
// the groups wait as on a slow disk (spawn_warm_wait_stop, up to spawn_warm_max_delay_ms) - for the files and the base
// models of their creatures, which then come off the main thread: the spawn only makes a copy.
struct SpawnColdSpacing
{
    u32 loads = 0; // TlColdModelLoads of this thread when last looked at
    u32 frame = 0; // the frame a cold load was seen in last
    bool any = false;
};
thread_local SpawnColdSpacing s_spawn_cold_spacing; // the passes run on the thread of CLevel::OnFrame

u32 spawn_cold_frames()
{
    const xray::hw::Policy& policy = xray::hw::GetPolicy();
    return xray::hw::PolicyValid(policy) ? policy.spawn_cold_frames : 0;
}

bool spawn_bg_wait_mode()
{
    const xray::hw::Policy& policy = xray::hw::GetPolicy();
    return xray::hw::PolicyValid(policy) && policy.model_bg_load != 0;
}

// The "* [hw] spawn" line once per process, at the first budgeted pass in play on a weak CPU
void spawn_weak_cpu_log()
{
    static bool s_logged = false;
    if (s_logged)
        return;
    const u32 frames = spawn_cold_frames();
    const bool bg = spawn_bg_wait_mode();
    if (!frames && !bg)
        return;
    s_logged = true;
    Msg("* [hw] spawn: weak CPU -> 1 cold model per %u frames, background base loading %s", frames, bg ? "on" : "off");
}

// A cold model load of this thread since the last look: its frame
void spawn_cold_note()
{
    SpawnColdSpacing& spacing = s_spawn_cold_spacing;
    const u32 loads = xray::perf::TlColdModelLoads();
    if (loads == spacing.loads)
        return;
    spacing.loads = loads;
    spacing.frame = Device.dwFrame;
    spacing.any = true;
}

// true = the last cold model load of this thread was less than `frames` frames ago (this frame counts)
bool spawn_cold_spaced(u32 frames)
{
    spawn_cold_note();
    const SpawnColdSpacing& spacing = s_spawn_cold_spacing;
    return spacing.any && Device.dwFrame - spacing.frame < frames;
}

void spawn_warm_wait_end()
{
    SpawnWarmWait& wait = s_spawn_warm_wait;
    if (!wait.waiting)
        return;
    wait.waiting = false;
#if XR_PERF_METRICS
    if (wait.t0 && xray::perf::g_perf_metrics_active)
        xray::perf::AddTimeSlow(xray::perf::Metric::spawn_warm_time, xray::perf::Ticks() - wait.t0);
#endif
}

// 50i: are the model files of the group at the head of the queue still to be read? The root and its children right
// behind it (the rule of the budget groups); every one of them is asked, so all their jobs go ahead of the ALife ones at
// once.
bool spawn_group_files_pending(const xr_deque<NET_Event>& queue)
{
    xr_vector<u16>& group = s_spawn_warm_group;
    group.clear();
    bool pending = false;
    u32 spawns = 0;
    for (const NET_Event& event : queue)
    {
        if (event.ID != M_SPAWN)
            continue;
        if (spawns && !spawn_joins_group(group, event.spawn_parent))
            break;
        if (event.spawn_id != u16(-1))
        {
            group.push_back(event.spawn_id);
            pending = model_prefetch_spawn_pending(event.spawn_id) || pending;
        }
        if (++spawns >= 64) // an inventory larger than this: the rest is asked when the budget defers it
            break;
    }
    group.clear();
    return pending;
}

// 50i: in front of a root M_SPAWN with no parent (a slow disk, a budgeted pass that is not loading / paused): true =
// the group waits for the "Model prefetch" thread, stop the pass here
bool spawn_warm_wait_stop(const xr_deque<NET_Event>& queue, const NET_Event& head)
{
    SpawnWarmWait& wait = s_spawn_warm_wait;
    if (wait.waiting && wait.id != head.spawn_id)
        spawn_warm_wait_end(); // the group it waited for went on another way (a nested pass, the loading screen...)
    if (head.spawn_id == u16(-1) || !spawn_group_files_pending(queue))
    {
        spawn_warm_wait_end();
        return false;
    }
    if (queue_wait_ms(head, CPU::GetTicks()) >= spawn_warm_max_delay_ms)
    {
        PERF_INC(spawn_warm_timeout); // goes on cold, its jobs still queued are dropped by the cold load
        spawn_warm_wait_end();
        return false;
    }
    if (!wait.waiting)
    {
        wait.waiting = true;
        wait.id = head.spawn_id;
        wait.t0 = 0;
#if XR_PERF_METRICS
        if (xray::perf::g_perf_metrics_active)
            wait.t0 = xray::perf::Ticks();
#endif
        PERF_INC(spawn_warm_wait);
    }
    PERF_INC(spawn_warm_frames);
    return true;
}
} // namespace

bool CLevel::SpawnBudgetSpent(SpawnBudgetPass& pass, const NET_Event& head)
{
    if (!pass.budget_ns || pass.groups < pass.min_groups || pass.timer.GetElapsed_ns() < pass.budget_ns)
        return false;
    // The budget of this frame is spent. The head goes on regardless while loading, on pause or when it has waited too
    // long: then every head older than spawn_max_delay_ms goes (FIFO: the ones behind it are younger).
    SpawnBudgetForce force = pass.force;
    if (force == SpawnBudgetForce::None && queue_wait_ms(head, CPU::GetTicks()) >= pass.max_delay_ms)
        force = SpawnBudgetForce::Delay;
    switch (force)
    {
    case SpawnBudgetForce::None:
        pass.stopped = true;
        return true;
    case SpawnBudgetForce::Loading: PERF_INC(spawn_forced_loading); break;
    case SpawnBudgetForce::Pause: PERF_INC(spawn_forced_pause); break;
    case SpawnBudgetForce::Delay:
        if (pass.catchup_ns && pass.timer.GetElapsed_ns() >= pass.catchup_ns)
        {
            PERF_INC(spawn_catchup_stopped);
            pass.stopped = true;
            return true;
        }
        PERF_INC(spawn_forced_delay);
        break;
    default: break;
    }
    ++pass.forced[size_t(force)];
    return false;
}

void CLevel::SpawnQueueNoteSave(pcstr where) const
{
    if (!game_events || GameID() != eGameIDSingle)
        return;
    xr_vector<u16> group;
    u32 groups = 0;
    u32 spawns = 0;
    count_spawn_groups(game_events->queue, group, groups, spawns);
    if (spawns)
        Msg("* spawn: %s with %u M_SPAWN (%u groups) still in the client queue (spawn_budget_us %d): their objects are "
            "saved from the server state",
            where, spawns, groups, g_spawn_budget_us);
}

#if XR_PERF_METRICS
// 26-framemove-game-next.md, M6: the ge/time/* bucket of a message of the game event queue
static xray::perf::Metric GameEventTimeMetric(u16 msg, u16 type)
{
    using M = xray::perf::Metric;
    if (msg == M_SPAWN)
        return M::ge_time_spawn;
    if (msg != M_EVENT)
        return M::ge_time_other;
    switch (type)
    {
    case GE_HIT: return M::ge_time_hit;
    case GE_DIE:
    case GE_ASSIGN_KILLER: return M::ge_time_die;
    case GE_OWNERSHIP_TAKE:
    case GE_OWNERSHIP_TAKE_MP_FORCED:
    case GE_TRADE_BUY: return M::ge_time_take;
    case GE_OWNERSHIP_REJECT:
    case GE_TRADE_SELL: return M::ge_time_reject;
    case GE_DESTROY:
    case GE_DESTROY_REJECT: return M::ge_time_destroy;
    case GE_WPN_AMMO_ADD:
    case GE_WPN_STATE_CHANGE:
    case GE_ADDON_ATTACH:
    case GE_ADDON_DETACH:
    case GE_ADDON_CHANGE:
    case GE_INSTALL_UPGRADE:
    case GE_TRANSFER_AMMO: return M::ge_time_weapon;
    case GE_GRENADE_EXPLODE: return M::ge_time_explode; // 30-game-events.md: apart from weapon (cold model loads)
    default: return M::ge_time_event_other;
    }
}

namespace
{
// 30-game-events.md (plans/optimization_mt), section 4: ge/max_event_us, ge/max_event_type, ge/n_frame/* - the
// statistics of the frame over every ProcessGameEvents call of it (the pass may nest: an event's time includes the
// events handled inside it), and the longest event of the 5 s window for the "* ge: longest event" line (log bit).
// Main thread only.
struct GameEventFrameStats
{
    u32 frame = u32(-1);
    u64 max_ticks = 0;
    u32 max_code = 0; // GE_* of an M_EVENT, 1000 + the message ID otherwise
    u32 n_die = 0, n_destroy = 0, n_reject = 0, n_hit = 0;
};
GameEventFrameStats s_ge_frame;

struct GameEventWindow
{
    u32 start_ms = 0;
    u64 max_ticks = 0;
    u32 code = 0;
    u16 dest = u16(-1);
    u32 frame = 0;
    u32 events_in_frame = 0; // events of that frame up to it
    shared_str section; // of the destination object, when the client had it after the event
};
GameEventWindow s_ge_window;

// Times one message of the queue (perf_metrics only)
class GameEventTimer
{
public:
    GameEventTimer(u16 msg, u16 dest, u16 type, u32 index) noexcept
        : m_code(msg == M_EVENT ? u32(type) : 1000u + msg), m_dest(dest), m_index(index)
    {
        if (!xray::perf::g_perf_metrics_active)
            return;
        m_active = true;
        if (msg == M_EVENT)
        {
            switch (type)
            {
            case GE_DIE: ++s_ge_frame.n_die; break;
            case GE_HIT: ++s_ge_frame.n_hit; break;
            case GE_DESTROY: ++s_ge_frame.n_destroy; break;
            case GE_DESTROY_REJECT:
                ++s_ge_frame.n_destroy;
                ++s_ge_frame.n_reject;
                break;
            case GE_OWNERSHIP_REJECT:
            case GE_TRADE_SELL: ++s_ge_frame.n_reject; break;
            default: break;
            }
        }
        m_t0 = xray::perf::Ticks();
    }
    ~GameEventTimer() noexcept
    {
        if (!m_active)
            return;
        const u64 ticks = xray::perf::Ticks() - m_t0;
        if (ticks > s_ge_frame.max_ticks)
        {
            s_ge_frame.max_ticks = ticks;
            s_ge_frame.max_code = m_code;
        }
        if (ticks > s_ge_window.max_ticks)
        {
            s_ge_window.max_ticks = ticks;
            s_ge_window.code = m_code;
            s_ge_window.dest = m_dest;
            s_ge_window.frame = Device.dwFrame;
            s_ge_window.events_in_frame = m_index;
            const IGameObject* object = Level().Objects.net_Find(m_dest);
            s_ge_window.section = object ? object->cNameSect() : shared_str();
        }
    }
    GameEventTimer(const GameEventTimer&) = delete;
    GameEventTimer& operator=(const GameEventTimer&) = delete;

private:
    u64 m_t0 = 0;
    u32 m_code;
    u16 m_dest;
    u32 m_index;
    bool m_active = false;
};

void GameEventFrameBegin()
{
    if (s_ge_frame.frame == Device.dwFrame)
        return;
    s_ge_frame = GameEventFrameStats();
    s_ge_frame.frame = Device.dwFrame;
}

void GameEventFrameEnd()
{
    using xray::perf::Metric;
    xray::perf::SetGauge(Metric::ge_max_event_us,
        static_cast<s64>(xray::perf::TicksToMilliseconds(s_ge_frame.max_ticks) * 1000.0));
    xray::perf::SetGauge(Metric::ge_max_event_type, static_cast<s64>(s_ge_frame.max_code));
    xray::perf::SetGauge(Metric::ge_n_frame_die, static_cast<s64>(s_ge_frame.n_die));
    xray::perf::SetGauge(Metric::ge_n_frame_destroy, static_cast<s64>(s_ge_frame.n_destroy));
    xray::perf::SetGauge(Metric::ge_n_frame_reject, static_cast<s64>(s_ge_frame.n_reject));
    xray::perf::SetGauge(Metric::ge_n_frame_hit, static_cast<s64>(s_ge_frame.n_hit));

    const u32 now_ms = CPU::GetTicks();
    if (!s_ge_window.start_ms)
        s_ge_window.start_ms = now_ms;
    if (now_ms - s_ge_window.start_ms < 5000)
        return;
    if ((xray::perf::g_perf_metrics & 2) && s_ge_window.max_ticks)
    {
        Msg("* ge: longest event of %u ms: %.0f us, %s %u, dest [%u][%s], frame %u (event #%u of the frame)",
            now_ms - s_ge_window.start_ms, xray::perf::TicksToMilliseconds(s_ge_window.max_ticks) * 1000.0,
            s_ge_window.code >= 1000 ? "message" : "GE type",
            s_ge_window.code >= 1000 ? s_ge_window.code - 1000 : s_ge_window.code, (unsigned)s_ge_window.dest,
            s_ge_window.section.c_str() ? s_ge_window.section.c_str() : "-", s_ge_window.frame,
            s_ge_window.events_in_frame);
    }
    s_ge_window = GameEventWindow();
    s_ge_window.start_ms = now_ms;
}
} // namespace
#endif

void CLevel::ProcessGameEvents(bool frame_budget)
{
    ZoneScoped;
    PERF_GAME_EVENT_TAG(); // 30-game-events.md: ge/cold/* - the cold loads of the sound and the render inside the pass
#if XR_PERF_METRICS
    GameEventFrameBegin();
#endif

    // Game events
    {
        NET_Packet P;
        u32 svT = timeServer() - NET_Latency;
        u32 processed = 0;
        // Spawn stage 0: the queue length, M_SPAWN of this pass and the longest "server sent it -> materialized"
        const size_t queue_len = game_events->size();
        u32 spawns = 0;
        u32 spawn_delay_ms = 0;
        u32 spawn_wait_ms = 0; // stage 2: the longest "entered the client queue -> materialized" of this pass
        TracyPlot("ge_queue_len", static_cast<s64>(queue_len));

        // Stage 2: the budget of this pass (see SpawnBudgetPass in Level.h)
        SpawnBudgetPass budget;
        if (frame_budget && g_spawn_budget_us > 0 && !g_bDebugEvents && GameID() == eGameIDSingle)
        {
            budget.timer.Start();
            budget.budget_ns = u64(g_spawn_budget_us) * 1000;
            budget.min_groups = u32(std::max(g_spawn_min_per_frame, 1));
            budget.max_delay_ms = u32(std::max(g_spawn_max_delay_ms, 0));
            budget.catchup_ns = g_spawn_catchup_us > 0 ? u64(std::max(g_spawn_catchup_us, g_spawn_budget_us)) * 1000 : 0;
            if (load_screen_renderer.IsActive() || Device.dwPrecacheFrame > 0)
                budget.force = SpawnBudgetForce::Loading;
            else if (Device.Paused())
                budget.force = SpawnBudgetForce::Pause;
        }
        xr_vector<u16> group; // IDs of the open group of this pass, the root first (local: the pass may nest)
        // 36-spawn-model-load.md: cold model loads of this thread at the start of the pass, the limit (0 = none)
        const u32 cold_models_at_start = xray::perf::TlColdModelLoads();
        const u32 cold_models_limit = budget.budget_ns && budget.force != SpawnBudgetForce::Loading &&
                budget.force != SpawnBudgetForce::Pause ? u32(std::max(g_spawn_cold_models, 0)) : 0;
        // 50i-stage3-slow-disk-io.md (R6): a slow disk - a group with no parent waits for the warm-up of its models.
        // Otherwise the time limit of the cold model loads of this pass: the hw policy's spawn_cold_ms (NVMe 8, SATA 6;
        // it can only stop where spawn_cold_models stops too unless that is 0 or more than 1 on the console)
        // 50l-weak-cpu-spawn-models.md: and on a weak CPU with the background base loading (no "[hw] io" line then)
        const bool warm_wait = budget.budget_ns && budget.force == SpawnBudgetForce::None &&
            (spawn_warm_wait_mode() || spawn_bg_wait_mode());
        const u64 cold_us_at_start = xray::perf::TlColdModelUs();
        const u64 cold_us_limit =
            budget.budget_ns && budget.force == SpawnBudgetForce::None ? spawn_cold_us_limit() : 0;
        // 50l-weak-cpu-spawn-models.md: a weak CPU - one cold model load per spawn_cold_frames frames
        const u32 cold_frames =
            cold_models_limit && budget.force == SpawnBudgetForce::None ? spawn_cold_frames() : 0;
        if (budget.budget_ns && budget.force == SpawnBudgetForce::None)
            spawn_weak_cpu_log();
        if (cold_frames)
            spawn_cold_note(); // a cold load since the last pass (an update of an object...) counts from this frame

        while (game_events->available(svT))
        {
            u16 spawn_id = u16(-1);
            {
                const NET_Event& head = game_events->front();
                if (head.ID == M_SPAWN)
                {
                    const bool joins_group = spawn_joins_group(group, head.spawn_parent);
                    // 36-spawn-model-load.md: spawn_cold_models - after N cold model loads no new group in this frame
                    if (!joins_group && cold_models_limit && budget.groups > 0 &&
                        xray::perf::TlColdModelLoads() - cold_models_at_start >= cold_models_limit)
                    {
                        PERF_INC(spawn_cold_stopped);
                        budget.stopped = true;
                        break; // as the budget below: the group and everything behind it wait for the next frame
                    }
                    // 50i (R6): and after spawn_cold_ms of them
                    if (!joins_group && cold_us_limit && budget.groups > 0 &&
                        xray::perf::TlColdModelUs() - cold_us_at_start >= cold_us_limit)
                    {
                        PERF_INC(spawn_cold_time_stopped);
                        budget.stopped = true;
                        break;
                    }
                    // 50l: a weak CPU - a group with no parent waits until spawn_cold_frames frames have passed since
                    // the last cold model load (the first group of a frame too: that is the spacing); not a head older
                    // than spawn_warm_max_delay_ms (cold loads outside the passes every frame must not hold it)
                    if (!joins_group && cold_frames && head.spawn_parent == u16(-1) && spawn_cold_spaced(cold_frames) &&
                        queue_wait_ms(head, CPU::GetTicks()) < spawn_warm_max_delay_ms)
                    {
                        PERF_INC(spawn_cold_spaced);
                        budget.stopped = true;
                        break;
                    }
                    // Stage 2: the budget is checked in front of a root only, a group is never split by it
                    if (!joins_group && SpawnBudgetSpent(budget, head))
                        break; // this group and everything behind it go on in the next frame, in the same order
                    // 50i (R6): past the budget too (a head older than spawn_max_delay_ms), up to
                    // spawn_warm_max_delay_ms; as the budget, the deferred spawns behind it are asked ahead below
                    if (!joins_group && warm_wait && head.spawn_parent == u16(-1) &&
                        spawn_warm_wait_stop(game_events->queue, head))
                    {
                        budget.stopped = true;
                        break;
                    }
                    if (SpawnMustWait(head.spawn_id, head.spawn_section()))
                        break; // SPEC.md 8.6: this spawn and everything behind it go on in the next frame
                    if (!joins_group)
                    {
                        group.clear();
                        ++budget.groups;
                    }
                    if (head.spawn_id != u16(-1))
                        group.push_back(head.spawn_id);
                    budget.group_size_max = std::max(budget.group_size_max, static_cast<u32>(group.size()));
                    spawn_wait_ms = std::max(spawn_wait_ms, queue_wait_ms(head, CPU::GetTicks()));
                    spawn_id = head.spawn_id;
                    const u32 now = timeServer_Async();
                    if (head.time_receive && now >= head.time_receive)
                        spawn_delay_ms = std::max(spawn_delay_ms, now - head.time_receive);
                }
            }
            ++processed;
            u16 ID, dest, type;
            game_events->get(ID, dest, type, P);
#if XR_PERF_METRICS
            xray::perf::ScopedTime ge_time(GameEventTimeMetric(ID, type)); // the handling of this message
            GameEventTimer ge_timer(ID, dest, type, processed); // 30-game-events.md: ge/max_event_*, ge/n_frame/*
#endif
            switch (ID)
            {
            case M_SPAWN:
            {
                PERF_INC(ge_spawn);
                ++spawns;
                u16 dummy16;
                P.r_begin(dummy16);
                cl_Process_Spawn(P);
                SpawnWaitDone(spawn_id);
                break;
            }
            case M_EVENT:
            {
                PERF_INC(ge_event);
                cl_Process_Event(dest, type, P);
                break;
            }
            case M_MOVE_PLAYERS:
            {
                PERF_INC(ge_other);
                u8 Count = P.r_u8();
                for (u8 i = 0; i < Count; i++)
                {
                    u16 ID = P.r_u16();
                    Fvector NewPos, NewDir;
                    P.r_vec3(NewPos);
                    P.r_vec3(NewDir);
                    CActor* OActor = smart_cast<CActor*>(Objects.net_Find(ID));
                    if (0 == OActor)
                        break;
                    OActor->MoveActor(NewPos, NewDir);
                }
                NET_Packet PRespond;
                PRespond.w_begin(M_MOVE_PLAYERS_RESPOND);
                Send(PRespond, net_flags(TRUE, TRUE));
                break;
            }
            case M_STATISTIC_UPDATE:
            {
                PERF_INC(ge_other);
                if (GameID() != eGameIDSingle)
                    Game().m_WeaponUsageStatistic->OnUpdateRequest(&P);
                break;
            }
            case M_FILE_TRANSFER:
            {
                PERF_INC(ge_other);
                if (m_file_transfer) // in case of net_Stop
                    m_file_transfer->on_message(&P);
                break;
            }
            case M_GAMEMESSAGE:
            {
                PERF_INC(ge_other);
                Game().OnGameMessage(P);
                break;
            }
            default:
            {
                PERF_INC(ge_other);
                VERIFY(0);
                break;
            }
            }
        }

        // 50l: the frame of a cold model load of this pass, for the spacing of the next frames
        if (cold_frames)
            spawn_cold_note();

        // 30-game-events.md, B1 (ol_destroy_child_scan_batch 1): the live children of the objects destroyed by this pass
        // are destroyed now, before UpdateCL of the objects (as when every destruction walked for them at once)
        Objects.flush_destroy_child_scan();

        // Stage 2: the head left in the queue waits too (budget, SPEC.md 8.6); what the budget left for the next frame
        if (game_events->size())
            spawn_wait_ms = std::max(spawn_wait_ms, queue_wait_ms(game_events->front(), CPU::GetTicks()));
        u32 deferred_groups = 0;
        u32 deferred_spawns = 0;
        if (budget.stopped)
        {
            PERF_INC(spawn_budget_frames);
            count_spawn_groups(game_events->queue, group, deferred_groups, deferred_spawns);
            PERF_ADD(spawn_budget_deferred, deferred_groups);
            // 36-spawn-model-load.md (the model prefetch): read the model files of the deferred spawns ahead (the
            // first ones of the queue; each model once per session, the render keeps the set)
            u32 asked = 0;
            for (const NET_Event& event : game_events->queue)
            {
                if (event.ID != M_SPAWN)
                    continue;
                model_prefetch_spawn(event.spawn_id);
                if (++asked >= 32)
                    break;
            }
        }
        if (budget.budget_ns)
        {
            ZoneTextVF(___tracy_scoped_zone, "budget=%dus groups=%u (max %u spawns) deferred=%u groups/%u spawns",
                g_spawn_budget_us, budget.groups, budget.group_size_max, deferred_groups, deferred_spawns);
            TracyPlot("ge_spawn_deferred", static_cast<s64>(deferred_spawns));
            if (budget.groups || budget.stopped)
            {
                SpawnBudgetFrame frame;
                frame.budget_us = u32(g_spawn_budget_us);
                frame.min_groups = budget.min_groups;
                frame.max_delay_ms = budget.max_delay_ms;
                frame.groups = budget.groups;
                frame.group_size_max = budget.group_size_max;
                frame.deferred_groups = deferred_groups;
                frame.deferred_spawns = deferred_spawns;
                frame.queue_wait_ms = spawn_wait_ms;
                std::copy(std::begin(budget.forced), std::end(budget.forced), std::begin(frame.forced));
                frame.stopped = budget.stopped;
                SpawnBudgetReport(frame);
            }
        }

        if (xray::perf::g_perf_metrics_active)
        {
            using xray::perf::Metric;
            xray::perf::SetGauge(Metric::ge_total_frame, static_cast<s64>(processed));
            xray::perf::SetGauge(Metric::ge_queue_len, static_cast<s64>(queue_len));
            xray::perf::SetGauge(Metric::ge_spawn_delay_ms, static_cast<s64>(spawn_delay_ms));
            xray::perf::SetGauge(Metric::spawn_deferred_frames_max,
                m_spawn_id_wait.waiting ? static_cast<s64>(Device.dwFrame - m_spawn_id_wait.first_frame) + 1 : 0);
            xray::perf::SetGauge(Metric::spawn_queue_wait_ms, static_cast<s64>(spawn_wait_ms));
            xray::perf::SetGauge(Metric::spawn_groups_frame, static_cast<s64>(budget.groups));
            xray::perf::SetGauge(Metric::spawn_group_size_max, static_cast<s64>(budget.group_size_max));
            // 36-spawn-model-load.md: cold model loads of this pass (max per frame)
            xray::perf::UpdateMax(
                Metric::ge_cold_model_frame, static_cast<s64>(xray::perf::TlColdModelLoads() - cold_models_at_start));
#if XR_PERF_METRICS
            GameEventFrameEnd();
#endif
        }
        TracyPlot("ge_spawn_frame", static_cast<s64>(spawns));
    }
    if (OnServer() && GameID() != eGameIDSingle)
        Game().m_WeaponUsageStatistic->Send_Check_Respond();
}

void CLevel::MakeReconnect()
{
    if (!Engine.Event.Peek("KERNEL:disconnect"))
    {
        Engine.Event.Defer("KERNEL:disconnect");
        char const* server_options = nullptr;
        char const* client_options = nullptr;
        if (m_caServerOptions.c_str())
        {
            server_options = xr_strdup(m_caServerOptions.c_str());
        }
        else
        {
            server_options = xr_strdup("");
        }
        if (m_caClientOptions.c_str())
        {
            client_options = xr_strdup(m_caClientOptions.c_str());
        }
        else
        {
            client_options = xr_strdup("");
        }
        Engine.Event.Defer("KERNEL:start", size_t(server_options), size_t(client_options));
    }
}

void CLevel::VisionBatchPostScheduler()
{
    {
        ZoneScopedN("VisionBatchPostScheduler/vision_batch");
        ZoneTextF("%u NPC", g_vision_batch.GetEntryCount());
        xray::perf::SetGauge(xray::perf::Metric::vision_entries, g_vision_batch.GetEntryCount());
        xray::perf::SetGauge(xray::perf::Metric::vision_rays, g_vision_batch.GetRayCount());
        g_vision_batch.ExecuteParallel();
    }
    {
        ZoneScopedN("VisionBatchPostScheduler/visibility_process");
        for (u32 i = 0; i < g_vision_batch.GetEntryCount(); ++i)
        {
            CCustomMonster* npc = g_vision_batch.GetEntry(i).npc;
            if (npc && !npc->getDestroy())
                npc->Exec_Visibility_Process();
        }
    }
    g_vision_batch.Reset();
    // No planner runs here: the native evaluators of the plugins are put into (or taken out of) the planners.
    gw::addons::goap::ApplyPending();
    // Plugin binders: the {id, dt} records collected by shedule_Update of this pass go to the plugins now, on the
    // same thread, before anything else of the frame touches the objects.
    gw::addons::binders::FlushUpdates();
}

// --- 26-framemove-game-next.md, package 2-A: work moved off the critical path (always on since 2026-10-18, their
// switches mt_sound_dispatch_early, lua_gc_overlap, mt_prepass_main_slot removed; 41-cvar-cleanup.md) -----------------
// K3: SoundEvent_Dispatch of a game/render overlap frame runs on the Game thread in phase 0
// (XRay::Engine::KickGameLogicEarly), right after phase A of the bones prepass - parallel to phase B/C on main.
// K2b (with mt_script_gc on): the Lua GC step of the frame (CLevel::script_gc) runs in phase 0 of the Game thread too,
// after the dispatch - parallel to phase B/C on main, the physics kick and the start of the render, instead of in
// seqParallel after the scheduler. Frames without phase 0 run it in seqParallel as before.
// K4: CMapManager::Update runs on main inside the phase B window of the bones prepass (the workers compute the bones
// meanwhile) instead of in CLevel::OnFrame before the objects' UpdateCL.

namespace
{
// K2b: the GC step of this frame is still to be done (set by CLevel::OnFrame on main before the Game thread starts;
// taken by phase 0 or by the seqParallel delegate - both on the game logic side, after the frame's kick)
std::atomic<bool> s_script_gc_pending{ false };
// K4: the map update of this frame waits for the prepass window (main thread only)
bool s_map_update_pending = false;

// K2b: the GC step of the frame, if it is still pending. early: phase 0 of the Game thread (lua/gc_in_early), else
// the seqParallel delegate at the old place (lua/gc_in_seq; a no-op once phase 0 took it)
void RunPendingScriptGc(bool early)
{
    if (!s_script_gc_pending.exchange(false, std::memory_order_acq_rel))
        return;
    if (!g_pGameLevel || !GEnv.ScriptEngine)
        return;
    if (early)
        PERF_INC(lua_gc_in_early);
    else
        PERF_INC(lua_gc_in_seq);
    Level().script_gc();
}
void RunEarlyScriptGc() { RunPendingScriptGc(true); }
void RunSeqScriptGc() { RunPendingScriptGc(false); }

// K4: the deferred map update of this frame (main thread), if it is still pending
void RunPendingMapUpdate()
{
    if (!s_map_update_pending)
        return;
    s_map_update_pending = false;
    if (!g_pGameLevel)
        return;
    ZoneScopedN("BonesPrepass/map_update");
    PERF_SCOPE(prepass_map_update);
    Level().MapManager().Update();
}

// K4: whatever way CLevel::BonesPrepass leaves, the map update of the frame is done before it returns
struct PendingMapUpdateGuard
{
    PendingMapUpdateGuard() = default;
    PendingMapUpdateGuard(const PendingMapUpdateGuard&) = delete;
    PendingMapUpdateGuard& operator=(const PendingMapUpdateGuard&) = delete;
    ~PendingMapUpdateGuard() { RunPendingMapUpdate(); }
};
} // namespace

// --- Bones prepass (always, plans/optimization_mt/04-bones-prepass.md) -----------------------------------------------
// The render used to compute the bones of every skeleton it drew itself (CalculateBones(TRUE) in add_leafs_dynamic,
// main view + shadows, on the critical path of the render). The prepass computes, before the render, the bones of the
// models the render computed in the previous frame: phase A (UpdateTracks, blend callbacks - game code, Lua) for all
// of them sequentially on main, then phase B (bone math, bone callbacks, visbox, attachments) in parallel. The render
// then takes the UCalc_DoneTime fast-out; anything new or invalidated after the prepass it still computes itself.
namespace
{
// Object IDs the render noted during DoRender of frame N (CGameObject::renderable_Render, any render thread), consumed
// by the prepass of frame N+1. One buffer is enough: the render (all its threads, the SVP one too, are joined inside
// DoRender) and the prepass (main thread, after the game logic) never run at the same time. Every object is noted at
// most once per frame (CGameObject::m_bones_prepass_note_frame) and IDs are u16, so 64K slots can't overflow.
constexpr u32 bones_prepass_note_capacity = 0x10000;
u16 s_bones_prepass_noted[bones_prepass_note_capacity];
std::atomic<u32> s_bones_prepass_noted_count{0};
// Main thread only (CLevel::BonesPrepass): kept between frames to keep their capacity.
xr_vector<IKinematics*> s_bones_prepass_candidates;
xr_vector<u16> s_bones_prepass_candidate_ids; // the object of each candidate (a weapon for its addon models)
xr_vector<IKinematics*> s_bones_prepass_phase_b;
// Phase C owners (pipeline on): Device.dwFrame per object ID of the models the prepass took this frame, so an owner is
// handed to phase B once and never next to its own regular candidate.
u32 s_bones_prepass_taken_frame[0x10000];
// Render prepare: IDs of the held / dropped items (CHudItem) phase C prepares. IDs again: phase A runs game
// code, the items are resolved anew after it.
xr_vector<u16> s_render_prepare_items;

// Render prepare of held items (plans/optimization_mt/05-render-prepare-weapons.md), collect step on main, for an object
// the render drew last frame (already filtered: alive, not the view entity or held by it). A CHudItem (weapon,
// grenade, detector, artefact... - held: noted through the owner's CInventoryOwner / CAttachmentOwner::renderable_Render,
// which reach CGameObject::renderable_Render by the virtual call; or lying on the ground) goes to phase C: its
// renderable_Render wrote XFORM / the shot light in every pass. A weapon with the attachment system adds its addon
// models: the render drew them with add_Visual and computed their bones itself (they are no objects, never noted).
// Attached items (helmet, torch, belt) need nothing here: they are noted themselves and their XFORM is written by the
// owner's AttachmentCallback in the owner's phase B.
void RenderPrepareCollect(IGameObject* O, xr_vector<IKinematics*>& models)
{
    CInventoryItem* inventory_item = O->cast_inventory_item();
    CHudItem* hud_item = inventory_item ? inventory_item->cast_hud_item() : nullptr;
    if (!hud_item)
        return;
    s_render_prepare_items.push_back(O->ID());
    const size_t before = models.size();
    hud_item->render_prepare_models(models);
    PERF_ADD(prepare_addon_models, models.size() - before);
}

// Render prepare, phase C on main: after phase B (the owners' bones are final), before the render.
// Sequential: UpdateXForm of a weapon whose owner the prepass didn't cover computes the owner's bones (as the render
// did), and the shot light moves in the spatial DB. With the pipelined prepass the prepass covers those owners too
// (phase C owners in CLevel::BonesPrepass).
void RenderPreparePhaseC(CLevel& level, const IGameObject* view)
{
    if (s_render_prepare_items.empty())
        return;
    ZoneScopedN("BonesPrepass/prepare");
    ZoneTextF("%u items", static_cast<u32>(s_render_prepare_items.size()));
    PERF_SCOPE(prepare_time);
    u32 prepared = 0;
    for (const u16 id : s_render_prepare_items)
    {
        IGameObject* O = level.Objects.net_Find(id);
        // Re-check after phase A (game code): gone, or taken by the view entity (its HUD path owns it).
        if (!O || O->getDestroy() || (view && O->H_Root() == view))
            continue;
        CInventoryItem* inventory_item = O->cast_inventory_item();
        CHudItem* hud_item = inventory_item ? inventory_item->cast_hud_item() : nullptr;
        if (!hud_item)
            continue;
#if XR_PERF_METRICS
        // prepare/owner_bones_main: the item's UpdateXForm will compute the owner's bones here, on main (phase A, and
        // phase B unless they are younger than UCalc_Interval)
        bool owner_pending = false;
        if (xray::perf::g_perf_metrics_active)
        {
            const IGameObject* owner = O->H_Parent();
            const IKinematics* owner_kinematics = owner ? PKinematics(owner->Visual()) : nullptr;
            owner_pending = owner_kinematics && !owner_kinematics->BonesPrepass_Done();
        }
        PERF_SCOPE_IF(owner_pending, prepare_owner_bones_main);
#endif
        hud_item->render_prepare();
        ++prepared;
    }
    PERF_ADD(prepare_items, prepared);
}
} // namespace

// --- Pipelined bones prepass (plans/optimization_mt/26-framemove-game-next.md; always on since 2026-10-18) -------------
// The UpdateCL loop of the frame keeps the workers idle for ~11 ms, the prepass after it computes ~2700 models on them in
// a ~2-3 ms window on main. With the pipeline, right after an UpdateCL batch (CObjectList::update_batch_hooks) main runs
// phase A of the models whose objects the loop is done with (the object and everything it holds - the held items run
// their UpdateCL after the owner and write state the owner's bone callbacks read: shot effector, attached XFORM), and
// phase B of them goes to the workers while main goes on with the next batches. The last batch and everything left (or
// redone) stay with the prepass, which then finds DoneTime == now and skips them.
// The models are those the render drew last frame (its notes, as the prepass reads them): roots, held and attached
// items, weapon addon models - every one goes when the loop is done with its root and everything under it, in chunks of
// 64 objects (xr_object_list.cpp); a root outside the loop after the first chunk. The last chunk is left to the prepass
// (beside the map update there). prepass/skip/* say why a model was still computed by the prepass.
// Phase A stays on main: its blend callbacks are game code (Lua ScriptCallBack, AI planner properties, PlayCycle,
// visual swaps of the bloodsucker, CollideAll of imotion_position).
// What may change the pose input after the early compute: animation mutators on main (PlayCycle & co. take the model
// lock - they wait for a running phase B - and mark the early bones dirty), invalidation (UCalc reset - recomputed by
// the next reader as before), the owner's XFORM (checked against a snapshot at the prepass: AttachmentCallback and the
// IK used the old one). Dirty bones are recomputed by the prepass (prepass/dirty, prepass/redone).
// Concurrency of phase B beside main's UpdateCL: no physics step runs in FrameMove; physics-shell owners (ragdolls, physic
// objects, vehicles) are left to the prepass (an activating ragdoll moves ODE bodies in its bone callback) except corpses
// whose shell is fully active (EarlyPhysicsAllowed); the code that swaps or removes a shell or a visual inside the loop
// joins the early tasks first (CLevel::BonesPrepassEarlyJoin); model deletes
// are deferred (SetFrameOverlapActive) until the next ModelDeferredClear; attach/detach and the visual callback list take
// the owner's bone lock (attachment_owner.cpp, GameObject.cpp); nested CalculateBones of another model that would need
// its phase A is skipped on the task (SkeletonRigid.cpp, prepass/early_nested_skip). All tasks are joined at the end of
// the loop, before PostUpdateCL and the destroy queue.
// Early kick (26-framemove-game-next.md; always on since 2026-10-18): after the UpdateCL loop the
// main thread still runs the rest of FrameMove (SoundProcessor, the UI subscribers) and the RT scheduler before the
// prepass, ~4 ms with the workers idle. At the end of CLevel::OnFrame (all of the level's game code of the frame is done:
// PostUpdateCL, the destroy queue, the HUD pick, plugin timers, script_process, add_call) main runs phase A of what the
// pipeline left to the prepass - the last chunk and the phase C owners - and hands their phase B to the workers without
// waiting. The prepass joins them first (main helps), then checks moved owners and mutated blends as for the pipeline.
// What runs beside the tasks and how it is guarded:
// - animation mutators (PlayCycle & co.) take the model lock (wait for a running phase B) and mark early bones dirty -
//   the prepass recomputes them (prepass/redone_after_kick); a moved owner the same way (EarlyCheckMoved);
// - the view entity, the RT scheduler (the actor) and script-controlled NPCs (ActionCallback moves a sound emitter from
//   phase B): their models are not kicked (prepass/skip/kick_excluded); Lua bone_position takes the model lock;
// - shell / visual swaps (CreateShell, SetRemoved, in_ChangeVisual, activate/setup_physic_shell) join the tasks first
//   (CLevel::BonesPrepassEarlyJoin), model deletes are deferred (SetFrameOverlapActive stays on until the frame's end);
// - a frame that turns sequential (pause, main menu) joins before its Game thread task (Device.BonesPrepassJoinCallback).
// Not covered by GW_OVERLAP_HAZARD (the detector fires on the Game thread only) - like the pipeline's own window.
// The Game thread's phase 0 (KickGameLogicEarly) is NOT moved here: its sound events are produced by SoundProcessor
// after this point, and its Lua GC step can't run beside the UI / actor binder Lua. It starts earlier anyway: the
// prepass phase A in front of it is shorter by the kicked models (game/early_start_ms).

namespace
{
// Why a model the prepass computes itself was not pipelined (prepass/skip/*): set per object ID by the pipeline, read by
// the prepass for the models it still computes (a byte per object; the counters need perf metrics).
enum EarlyReason : u8
{
    early_reason_none = 0, // not examined by the pipeline
    early_reason_kicked, // handed to a task (recomputed by the prepass all the same: lost)
    early_reason_tail_own, // the object's own UpdateCL is in the last chunk of the loop
    early_reason_tail_holder, // the object is earlier, its root (or another object under the root) is in the last chunk
    early_reason_physics, // a physics shell the pipeline may not touch
    early_reason_cooldown, // recently redone
    early_reason_filtered, // gone / reparented / no model / the view entity's at its turn
    early_reason_phase_a, // nothing to do at its turn (computed or started this tick)
    early_reason_full, // no room left in the model list of the frame
    early_reason_kick, // handed to the early kick and still computed by the prepass: redone
    early_reason_kick_excluded, // the early kick left it: its root is in the RT scheduler or script-controlled
};

// Per object ID, valid for the pipeline of one frame (epoch): as a root - the last workload index of the root or of
// anything under it; the object's own workload index; the reason of its own model (and of its weapon addon models).
struct EarlySlot
{
    u32 epoch;
    u32 root_last;
    u32 own;
    u8 reason;
    u8 kick_lazy; // early kick: examined as a phase C owner already (an owner holds several items)
};
constexpr u32 early_id_slots = 0x10000;
constexpr u32 early_index_none = u32(-1);
EarlySlot s_early_slots[early_id_slots];
u32 s_early_epoch = 0;
u32 s_early_frame = u32(-1); // Device.dwFrame of the last pipeline (the slots are read by its prepass)

EarlySlot& EarlySlotOf(u16 id)
{
    EarlySlot& s = s_early_slots[id];
    if (s.epoch != s_early_epoch)
        s = {s_early_epoch, early_index_none, early_index_none, early_reason_none, 0};
    return s;
}

// An object the render drew last frame and the chunk after which the loop is done with its root.
struct EarlyItem
{
    u32 ready_chunk;
    u16 id;
    u16 root_id;
};
struct EarlyModel
{
    IKinematics* model;
    u16 id;
    // The root's own model: AttachmentCallback and the IK used its XFORM (EarlyCheckMoved). A held item's bones don't
    // depend on its XFORM (phase C writes that from the owner's bones; attached items get it from AttachmentCallback).
    bool check_xform;
    Fmatrix xform;
};
xr_vector<EarlyItem> s_early_items; // sorted by ready_chunk
size_t s_early_cursor = 0;
// Capacity fixed in EarlyBegin: push_back never reallocates while tasks read their ranges (EarlyModelRoom).
xr_vector<EarlyModel> s_early_models;
xr_vector<IKinematics*> s_early_addons; // scratch: the addon models of one weapon
u32 s_early_models_frame = u32(-1);
xr_vector<Task*> s_early_tasks;
std::atomic<u32> s_early_computed{0};
bool s_early_overlap_prev = false;
// Between EarlyBegin and EarlyEnd of a pipelined loop (main thread); read by CLevel::BonesPrepassEarlyJoin.
std::atomic<bool> s_early_in_loop{false};
std::thread::id s_early_tid;
// Early kick (main thread): the switch as EarlyBegin read it - the last chunk's items stay in s_early_items for the kick
bool s_kick_wanted = false;
// Device.dwFrame of the last kick (s_kick_lazy and the dirty check of its phase C owners in the prepass belong to it)
u32 s_kick_frame = u32(-1);
// Kicked and not joined yet: read by CLevel::BonesPrepassEarlyJoin (the guards) and by the joins
std::atomic<bool> s_kick_open{false};
// perf_metrics: Ticks() at the end of the kick (prepass/early_kick), main time of its phase A (game/early_start_ms)
u64 s_kick_end_ticks = 0;
u64 s_kick_main_ticks = 0;
u64 s_kick_wait_ticks = 0;
// The phase C owners the kick computes (pointers stay valid: model deletes are deferred until the next frame)
xr_vector<IKinematics*> s_kick_lazy;

// The frames the prepass will run in as a game/render overlap frame (CRenderDevice::CanOverlapGameAndRender decides
// after FrameMove - this is its condition): in a sequential frame the whole scheduler runs between the early bones and
// the prepass (correct with the dirty checks, but mostly redone).
bool EarlyPipelineFrame()
{
    if (!TaskScheduler || GEnv.isDedicatedServer || !GEnv.Render)
        return false;
    if (!g_pGameLevel || !g_pGameLevel->bReady)
        return false;
    if (!Device.b_is_Active || Device.dwPrecacheFrame || load_screen_renderer.IsActive() || Device.Paused())
        return false;
    if (GEnv.Render->GetBackendAPI() != IRender::BackendAPI::D3D11)
        return false;
    if (g_pGamePersistent && g_pGamePersistent->m_pMainMenu && g_pGamePersistent->m_pMainMenu->IsActive())
        return false;
    return true;
}

// Physics shells (xrPhysics: CPHElement::BonesCallBack). While a shell or any of its elements is activating, the bone
// callback moves ODE bodies and the physics spatial DB (ActivatingPos -> ToBonePos -> SetTransform); once everything is
// fully active it only reads the element's and the shell's matrices (written by the physics step, which doesn't run in
// FrameMove, and by the object's own UpdateCL, which is done) and writes the bone matrix. Taken: corpses only (a dead
// CEntityAlive without an interactive death motion - imotion swaps the callbacks and moves the shell from its phase A),
// with the shell and every element fully active, asleep or not. A corpse's shell is created, removed or swapped inside
// the loop only by KillHit/CreateShell, SetRemoved and in_ChangeVisual (CharacterPhysicsSupport.cpp) - they join the
// early tasks first (CLevel::BonesPrepassEarlyJoin) - and by net_Destroy after the loop. Physic objects, items on the
// ground and vehicles stay with the prepass (their shells are changed from more places).
bool EarlyPhysicsAllowed(IGameObject* O)
{
    CPhysicsShellHolder* holder = O->cast_physics_shell_holder();
    const CPhysicsShell* shell = holder ? holder->PPhysicsShell() : nullptr;
    if (!shell)
        return true;
    const CEntityAlive* entity = O->cast_entity_alive();
    if (!entity || entity->g_Alive())
        return false;
    CCharacterPhysicsSupport* support = holder->character_physics_support();
    if (!support || support->is_interactive_motion() || !shell->isFullActive())
        return false;
    const u16 elements = shell->get_ElementsNumber();
    for (u16 i = 0; i < elements; ++i)
    {
        const CPhysicsElement* element = shell->get_ElementByStoreOrder(i);
        if (!element || !element->isFullActive())
            return false;
    }
    PERF_INC(prepass_early_corpses);
    return true;
}

bool EarlyModelRoom() { return s_early_models.size() < s_early_models.capacity(); }

bool EarlyBegin(IGameObject* const* b, IGameObject* const* e, u32 chunk)
{
    s_early_items.clear();
    s_early_cursor = 0;
    s_early_models.clear();
    s_early_models_frame = u32(-1);
    s_early_tasks.clear();
    s_early_computed.store(0, std::memory_order_relaxed);
    s_kick_wanted = false;
    if (!EarlyPipelineFrame())
        return false;
    s_kick_wanted = true;
    ZoneScopedN("BonesPrepass/early_begin");
    PERF_SCOPE(prepass_early_main);
    if (++s_early_epoch == 0)
    {
        for (EarlySlot& s : s_early_slots)
            s.epoch = 0;
        s_early_epoch = 1;
    }
    s_early_frame = Device.dwFrame;
    // SingleUpdate updates the parent first, the held items come in any chunk (before or after the owner's own place in
    // the list): a root is ready after the last chunk that holds it or any object under it.
    const u32 n = static_cast<u32>(e - b);
    for (u32 i = 0; i < n; ++i)
    {
        IGameObject* O = b[i];
        EarlySlotOf(O->ID()).own = i;
        EarlySlotOf(O->H_Root()->ID()).root_last = i;
    }
    const u32 last_chunk = (n - 1) / chunk;
    // The models: what the render drew last frame - its notes, read again (and filtered the same way) by the prepass.
    // Every object, not only the roots: the held items, the attached ones and the weapon addon models go with their
    // root. A root outside the loop (no UpdateCL this frame: processing off, a settled corpse) goes after the first chunk.
    const u32 noted = std::min(s_bones_prepass_noted_count.load(std::memory_order_relaxed), bones_prepass_note_capacity);
    CObjectList& objects = g_pGameLevel->Objects;
    const IGameObject* view = Level().CurrentViewEntity();
    for (u32 k = 0; k < noted; ++k)
    {
        const u16 id = s_bones_prepass_noted[k];
        IGameObject* O = objects.net_Find(id);
        if (!O || O->getDestroy() || !O->Visual())
            continue;
        IGameObject* root = O->H_Root();
        if (view && (O == view || root == view))
            continue;
        const u32 root_last = EarlySlotOf(root->ID()).root_last;
        u32 ready = root_last == early_index_none ? 0 : root_last / chunk;
        EarlySlot& s = EarlySlotOf(id);
        if (ready >= last_chunk)
        {
            s.reason = (s.own != early_index_none && s.own / chunk >= last_chunk) ? early_reason_tail_own :
                                                                                    early_reason_tail_holder;
            if (!s_kick_wanted)
                continue;
            // Early kick: kept for EarlyKickTail at the end of CLevel::OnFrame (batch_done never reaches last_chunk)
            ready = last_chunk;
        }
        s_early_items.push_back({ready, id, root->ID()});
    }
    std::sort(s_early_items.begin(), s_early_items.end(),
        [](const EarlyItem& l, const EarlyItem& r) { return l.ready_chunk < r.ready_chunk; });
    // Room for the frame's models (a weapon with the attachment system adds its addon models): what doesn't fit is left
    // to the prepass (prepass/skip/full).
    s_early_models.reserve(s_early_items.size() * 2 + 64);
    s_early_models_frame = Device.dwFrame;
    // A model released meanwhile (a visual swap by game code) is only queued until ModelDeferredClear of the next frame,
    // so a running phase B never computes a freed model. Restored in EarlyEnd (ProcessFrame sets it after FrameMove).
    s_early_overlap_prev = Device.IsFrameOverlapActive();
    Device.SetFrameOverlapActive(true);
    s_early_tid = std::this_thread::get_id();
    s_early_in_loop.store(true, std::memory_order_relaxed);
    return true;
}

// Main, after the chunk that made the item's root ready: filters, phase A, the model(s) to the list of the next task.
void EarlyTake(const EarlyItem& item, const IGameObject* view)
{
    EarlySlot& s = EarlySlotOf(item.id);
    IGameObject* O = g_pGameLevel->Objects.net_Find(item.id);
    // Same filters as the prepass (the view entity and what it holds: the HUD path; frozen corpse pose: no bones); an
    // object another root took meanwhile is left to the prepass (the new root may not be through the loop yet).
    if (!O || O->getDestroy() || !O->Visual() || O->H_Root()->ID() != item.root_id || O == view ||
        (view && O->H_Root() == view) || O->renderable_CorpsePoseFrozen())
    {
        s.reason = early_reason_filtered;
        return;
    }
    if (!EarlyPhysicsAllowed(O))
    {
        s.reason = early_reason_physics;
        return;
    }
    s.reason = early_reason_phase_a;
    if (IKinematics* K = PKinematics(O->Visual()))
    {
        // EarlyWanted is BonesPrepass_Wanted plus the redo cooldown: for a model the prepass computes, a refusal here
        // is the cooldown.
        if (!K->BonesPrepass_EarlyWanted())
            s.reason = early_reason_cooldown;
        else if (!EarlyModelRoom())
            s.reason = early_reason_full;
        else if (K->BonesPrepass_PhaseA()) // here, on main: the blend callbacks (game code, Lua) stay on this thread
        {
            s.reason = early_reason_kicked;
            s_early_models.push_back({K, item.id, item.id == item.root_id, O->XFORM()});
        }
    }
    // A held weapon with the attachment system: its addon models (no objects of their own), as the prepass collects them.
    CInventoryItem* inventory_item = O->cast_inventory_item();
    if (CHudItem* hud_item = inventory_item ? inventory_item->cast_hud_item() : nullptr)
    {
        s_early_addons.clear();
        hud_item->render_prepare_models(s_early_addons);
        for (IKinematics* addon : s_early_addons)
        {
            if (!addon->BonesPrepass_EarlyWanted())
                continue;
            if (!EarlyModelRoom())
            {
                s.reason = early_reason_full;
                break;
            }
            if (addon->BonesPrepass_PhaseA())
                s_early_models.push_back({addon, item.id, false, Fidentity});
        }
    }
}

// Phase B of [first, end of the list) on the workers, main goes on with the loop.
void EarlyKick(size_t first)
{
    const u32 count = static_cast<u32>(s_early_models.size() - first);
    if (!count)
        return;
    // One task per chunk, split by xr_parallel_for into about two pieces per worker.
    const u32 workers = std::max<u32>(static_cast<u32>(TaskScheduler->GetWorkersCount()), 1u);
    const size_t grain = std::max<size_t>(1, count / (workers * 2));
    EarlyModel* const models = s_early_models.data();
    Task& task = xr_parallel_for(TaskRange<u32>(static_cast<u32>(first), static_cast<u32>(first) + count, grain), false,
        [models](const TaskRange<u32>& range)
        {
            ZoneScopedN("BonesPrepass/early_task");
            PERF_PASS(xray::perf::pass::Prepass);
            PERF_SCOPE(prepass_early_phase_b);
            PERF_SCOPE_BY_THREAD(prepass_early_phase_b_main, prepass_early_phase_b_game, prepass_early_phase_b_worker);
            u32 local = 0;
            for (u32 i = range.begin(); i != range.end(); ++i)
            {
                if (models[i].model->BonesPrepass_PhaseBEarly())
                    ++local;
            }
            s_early_computed.fetch_add(local, std::memory_order_relaxed);
        });
    s_early_tasks.push_back(&task);
}

void EarlyBatchDone(u32 chunk)
{
    ZoneScopedN("BonesPrepass/early_batch");
    PERF_SCOPE(prepass_early_main);
    // A Task of main's ring is reused 4096 allocations later: don't keep finished ones.
    s_early_tasks.erase(std::remove_if(s_early_tasks.begin(), s_early_tasks.end(),
                            [](const Task* t) { return t->IsFinished(); }),
        s_early_tasks.end());
    const size_t first = s_early_models.size();
    const IGameObject* view = Level().CurrentViewEntity();
    {
        PERF_SCOPE(prepass_early_main_take);
        while (s_early_cursor < s_early_items.size() && s_early_items[s_early_cursor].ready_chunk <= chunk)
            EarlyTake(s_early_items[s_early_cursor++], view);
    }
    ZoneValue(s_early_models.size() - first);
    PERF_SCOPE(prepass_early_main_kick);
    EarlyKick(first);
}

void EarlyJoinTasks()
{
    for (const Task* t : s_early_tasks)
        TaskScheduler->Wait(*t);
    s_early_tasks.clear();
}

void EarlyEnd()
{
    {
        ZoneScopedN("BonesPrepass/early_tail_wait");
        PERF_SCOPE(prepass_tail_wait);
        // Before PostUpdateCL and the destroy queue (net_Destroy of objects whose bones may be in flight). Main helps.
        // The last chunk stays with the prepass: there its phase B runs beside the map update (K4), here main would
        // only wait for it.
        EarlyJoinTasks();
    }
    s_early_in_loop.store(false, std::memory_order_relaxed);
    PERF_ADD(prepass_pipelined, s_early_computed.exchange(0, std::memory_order_relaxed));
    Device.SetFrameOverlapActive(s_early_overlap_prev);
}

// At the prepass: early bones whose owner moved after them (Lua teleport, set_position...) - AttachmentCallback and the
// IK used the old XFORM: recomputed by the prepass. By ID: the object may be gone; the model pointer is only compared.
void EarlyCheckMoved()
{
    if (s_early_models_frame != Device.dwFrame || !g_pGameLevel)
    {
        s_early_models.clear();
        return;
    }
    CObjectList& objects = g_pGameLevel->Objects;
    for (const EarlyModel& m : s_early_models)
    {
        if (!m.check_xform)
            continue;
        IGameObject* O = objects.net_Find(m.id);
        if (!O || O->getDestroy() || PKinematics(O->Visual()) != m.model)
            continue;
        if (std::memcmp(&O->XFORM(), &m.xform, sizeof(Fmatrix)) != 0)
            m.model->BonesPrepass_MarkEarlyDirty();
    }
    s_early_models.clear();
    s_early_models_frame = u32(-1);
}

// At the prepass, for a model it computes itself in a pipelined frame: why the pipeline didn't (prepass/skip/*).
void EarlyCountSkip(u16 id)
{
#if XR_PERF_METRICS
    if (s_early_frame != Device.dwFrame || !xray::perf::g_perf_metrics_active)
        return;
    const EarlySlot& s = s_early_slots[id];
    switch (s.epoch == s_early_epoch ? s.reason : u8(early_reason_none))
    {
    case early_reason_kicked: PERF_INC(prepass_skip_lost); break;
    case early_reason_tail_own: PERF_INC(prepass_skip_tail_own); break;
    case early_reason_tail_holder: PERF_INC(prepass_skip_tail_holder); break;
    case early_reason_physics: PERF_INC(prepass_skip_physics); break;
    case early_reason_cooldown: PERF_INC(prepass_skip_cooldown); break;
    case early_reason_filtered: PERF_INC(prepass_skip_filtered); break;
    case early_reason_phase_a: PERF_INC(prepass_skip_phase_a); break;
    case early_reason_full: PERF_INC(prepass_skip_full); break;
    case early_reason_kick: PERF_INC(prepass_redone_after_kick); break;
    case early_reason_kick_excluded: PERF_INC(prepass_skip_kick_excluded); break;
    default: PERF_INC(prepass_skip_unseen); break;
    }
#else
    (void)id;
#endif
}

// Early kick: roots whose models stay with the prepass. (1) In the RT scheduler (the actor - CActor::shedule_Update runs
// after FrameMove: movement, animations, attach/detach of HUD models, its binder's Lua). (2) Script-controlled
// (CScriptEntity::GetScriptControl): its visual callback (ActionCallback, script_entity.cpp) moves the current sound's
// emitter (_feedback()->set_position, no lock) and particles from phase B - beside SoundProcessor's update, which may
// delete that emitter.
bool EarlyKickExcluded(IGameObject* root)
{
    if (root->GetSchedulerData().b_RT)
        return true;
    const CScriptEntity* script_entity = root->cast_script_entity();
    return script_entity && script_entity->GetScriptControl();
}

// Early kick, main thread, at the end of CLevel::OnFrame of a pipelined frame (see the early kick above): phase A
// of what the pipeline left (the last chunk's models and the phase C owners), their phase B to the workers, no wait.
void EarlyKickTail()
{
    if (!s_kick_wanted || s_early_frame != Device.dwFrame || s_kick_open.load(std::memory_order_relaxed))
        return;
    s_kick_wanted = false;
    // Paused or the main menu opened by the level's own code meanwhile: nothing to overlap with, the prepass does it
    if (!EarlyPipelineFrame() || !g_pGameLevel)
        return;
    ZoneScopedN("BonesPrepass/early_kick");
    PERF_SCOPE(prepass_early_main);
    const u64 t0 = xray::perf::g_perf_metrics_active ? xray::perf::Ticks() : 0;
    const u32 frame = Device.dwFrame;
    s_kick_frame = frame;
    s_kick_lazy.clear();
    // The loop's tasks were joined by EarlyEnd; the guards join the kick's from now on (BonesPrepassEarlyJoin). A model a
    // blend callback or the code after this point releases is only queued: FrameMove's window is open until
    // ProcessFrame sets it for the frame (and closes it after the physics join) - no restore here.
    s_early_tasks.clear();
    Device.SetFrameOverlapActive(true);
    s_kick_open.store(true, std::memory_order_relaxed);
    CObjectList& objects = g_pGameLevel->Objects;
    const IGameObject* view = Level().CurrentViewEntity();
    const size_t first = s_early_models.size();
    // prepass/early_main/take: the phase A of the items below and of the phase C owners (up to the kick)
    const u64 take_t0 = t0 ? xray::perf::Ticks() : 0;
    // The last chunk's items (and any the batches left: none, every chunk but the last was handed over)
    while (s_early_cursor < s_early_items.size())
    {
        const EarlyItem& item = s_early_items[s_early_cursor++];
        IGameObject* root = objects.net_Find(item.root_id);
        if (root && EarlyKickExcluded(root))
        {
            EarlySlotOf(item.id).reason = early_reason_kick_excluded;
            continue;
        }
        EarlyTake(item, view);
        EarlySlot& s = EarlySlotOf(item.id);
        if (s.reason == early_reason_kicked)
            s.reason = early_reason_kick;
    }
    // Phase C owners (as the prepass takes them, the pipelined prepass): the owner of a held item the render drew
    // whose bones nobody computes before phase C (an NPC drawn as a shadow blob, a model in a cooldown). Same filters as
    // the prepass, plus the pipeline's physics rule and EarlyKickExcluded; not one the kick or the loop already took.
    const u32 noted = std::min(s_bones_prepass_noted_count.load(std::memory_order_relaxed), bones_prepass_note_capacity);
    for (u32 k = 0; k < noted; ++k)
    {
        IGameObject* O = objects.net_Find(s_bones_prepass_noted[k]);
        if (!O || O->getDestroy())
            continue;
        CInventoryItem* inventory_item = O->cast_inventory_item();
        if (!inventory_item || !inventory_item->cast_hud_item())
            continue;
        IGameObject* owner = O->H_Parent();
        if (!owner || owner->getDestroy() || !owner->Visual() || owner == view || owner->H_Root() == view ||
            !owner->cast_entity_alive() || owner->renderable_CorpsePoseFrozen() || EarlyKickExcluded(owner->H_Root()))
            continue;
        EarlySlot& s = EarlySlotOf(owner->ID());
        if (s.kick_lazy || s.reason == early_reason_kick || s.reason == early_reason_kicked)
            continue;
        s.kick_lazy = 1;
        IKinematics* K = PKinematics(owner->Visual());
        if (!K || K->BonesPrepass_Done() || !EarlyPhysicsAllowed(owner))
            continue;
        if (K->BonesPrepass_LazyPhaseA()) // on main: the blend callbacks
            s_kick_lazy.push_back(K);
    }
#if XR_PERF_METRICS
    if (take_t0)
        xray::perf::AddTimeSlow(xray::perf::Metric::prepass_early_main_take, xray::perf::Ticks() - take_t0);
#else
    (void)take_t0;
#endif
    PERF_ADD(prepare_owner_lazy, s_kick_lazy.size());
    const u32 count_models = static_cast<u32>(s_early_models.size() - first);
    const u32 count_lazy = static_cast<u32>(s_kick_lazy.size());
    ZoneValue(count_models + count_lazy);
    if (count_models + count_lazy && TaskScheduler)
    {
        PERF_SCOPE(prepass_early_main_kick);
        const u32 count = count_models + count_lazy;
        const u32 workers = std::max<u32>(static_cast<u32>(TaskScheduler->GetWorkersCount()), 1u);
        // A stalker costs many times a pistol: small chunks (about 4 per worker), as the prepass splits its phase B
        const size_t grain = std::max<size_t>(1, count / (workers * 4));
        EarlyModel* const models = s_early_models.data() + first;
        IKinematics* const* const lazy = s_kick_lazy.data();
        Task& task = xr_parallel_for(TaskRange<u32>(0, count, grain), false,
            [models, lazy, count_models](const TaskRange<u32>& range)
            {
                ZoneScopedN("BonesPrepass/early_kick_task");
                PERF_PASS(xray::perf::pass::Prepass);
                PERF_SCOPE(prepass_early_phase_b);
                PERF_SCOPE_BY_THREAD(prepass_early_phase_b_main, prepass_early_phase_b_game,
                    prepass_early_phase_b_worker);
                u32 local = 0;
                for (u32 i = range.begin(); i != range.end(); ++i)
                {
                    if (i >= count_models)
                        lazy[i - count_models]->BonesPrepass_LazyPhaseBEarly();
                    else if (models[i].model->BonesPrepass_PhaseBEarly())
                        ++local;
                }
                s_early_computed.fetch_add(local, std::memory_order_relaxed);
            });
        s_early_tasks.push_back(&task);
    }
    if (t0)
    {
        const u64 t1 = xray::perf::Ticks();
        s_kick_main_ticks = t1 - t0;
        s_kick_end_ticks = t1;
    }
    else
        s_kick_main_ticks = s_kick_end_ticks = 0;
}

// Early kick: wait for its tasks (main helps). at_prepass: the regular join at the prepass, with the metrics of the
// window; otherwise a frame that turned sequential (before its Game thread task), a device reset or the level's end.
void EarlyKickJoin(bool at_prepass)
{
    if (!s_kick_open.load(std::memory_order_relaxed) || std::this_thread::get_id() != s_early_tid)
        return;
    const u64 t0 = s_kick_end_ticks ? xray::perf::Ticks() : 0;
#if XR_PERF_METRICS
    // prepass/early_kick: the overlapped window - from the end of the kick to this join (sound, UI, RT scheduler)
    if (at_prepass && t0)
        xray::perf::AddTimeSlow(xray::perf::Metric::prepass_early_kick, t0 - s_kick_end_ticks);
#endif
    {
        ZoneScopedN("BonesPrepass/early_kick_wait");
        PERF_SCOPE_IF(at_prepass, prepass_early_kick_wait);
        EarlyJoinTasks();
    }
    s_kick_wait_ticks = t0 ? xray::perf::Ticks() - t0 : 0;
    s_kick_open.store(false, std::memory_order_relaxed);
    PERF_ADD(prepass_pipelined, s_early_computed.exchange(0, std::memory_order_relaxed));
}
} // namespace

// Main thread, inside the UpdateCL loop of a pipelined frame: game code that is about to swap or remove a physics shell
// or a visual (its bone callbacks and the elements they read) waits for the early tasks first - one of them may be
// computing that model. Deadlock-free: an early task blocks only on a model lock it doesn't hold, never on the global
// bones lock (SkeletonRigid.cpp); so main may hold the global lock here (a blend callback in phase A), but not a model
// lock - then it can't wait and goes on as before (prepass/early_guard_skip). Elsewhere a no-op.
void CLevel::BonesPrepassEarlyJoin()
{
    // The UpdateCL loop of a pipelined frame, or the early kick's window (end of CLevel::OnFrame .. the prepass)
    if (!(s_early_in_loop.load(std::memory_order_relaxed) || s_kick_open.load(std::memory_order_relaxed)) ||
        std::this_thread::get_id() != s_early_tid)
        return;
    if (s_early_tasks.empty())
        return;
    if (xray::perf::TlBonesLocked() != 0)
    {
        PERF_INC(prepass_early_guard_skip);
        return;
    }
    ZoneScopedN("BonesPrepass/early_guard_join");
    PERF_SCOPE(prepass_early_guard_join);
    EarlyJoinTasks();
}

// Device.BonesPrepassJoinCallback: the early kick's tasks must not outlive a frame that turned sequential (its Game
// thread task runs the scheduler), a frame without the prepass, a device reset or the level. Main thread; else a no-op.
void CLevel::BonesPrepassEarlyKickJoin() { EarlyKickJoin(false); }

bool CLevel::BonesPrepassEarlyKickOpen() { return s_kick_open.load(std::memory_order_relaxed); }

void CLevel::BonesPrepassNote(u16 id)
{
    const u32 slot = s_bones_prepass_noted_count.fetch_add(1, std::memory_order_relaxed);
    if (slot < bones_prepass_note_capacity)
        s_bones_prepass_noted[slot] = id;
}

void CLevel::BonesPrepass()
{
    // K4: the deferred map update runs in the phase B window below, or here on the way out (no prepass this frame)
    const PendingMapUpdateGuard map_update_guard;
    // Early kick: its phase B first - the dirty marks below need the early stamps of its
    // models, and phase A must not race them. Normally done by now (the sound update, the UI and the RT scheduler ran
    // beside it); main helps with what is left. Before the Game thread's phase 0 below, so the map update stays after it.
    EarlyKickJoin(true);
    // Pipelined prepass: early bones whose owner moved since are marked dirty - phase A below recomputes them.
    EarlyCheckMoved();
    // Take the previous render's notes and start an empty list for this frame's render - always, even when nothing
    // is computed below (a level being loaded, or one that has just replaced another: stale IDs must not pile up).
    const u32 noted = std::min(s_bones_prepass_noted_count.exchange(0, std::memory_order_relaxed),
        bones_prepass_note_capacity);
    if (!g_pGameLevel || !g_pGameLevel->bReady || noted == 0)
        return;
    PERF_ADD(prepass_noted, noted);

    CLevel& level = Level();
    const IGameObject* view = level.CurrentViewEntity();
    auto& candidates = s_bones_prepass_candidates;
    auto& candidate_ids = s_bones_prepass_candidate_ids;
    auto& phase_b = s_bones_prepass_phase_b;
    candidates.clear();
    candidate_ids.clear();
    phase_b.clear();
    const u32 frame = Device.dwFrame;
    // The regular candidates in phase_b (before the phase C owners)
    size_t phase_b_regular = 0;
    s_render_prepare_items.clear();
    // Render prepare of held items (always, plans/optimization_mt/05-render-prepare-weapons.md): held items and the
    // addon models of weapons are prepared here too (collect -> phases A/B -> phase C), and their renderable_Render
    // only reads.

    {
        ZoneScopedN("BonesPrepass/phase_a");
        PERF_SCOPE(prepass_phase_a);
        // IDs, not pointers, cross the frame: an object may be gone (or its ID reused) since the render noted it.
        // Collect everything first, then run phase A: its game code may move objects between the active and the
        // sleeping list or change visuals; the IKinematics pointers stay valid meanwhile, because we are inside the
        // overlap window and CModelPool::Delete only queues the model until ModelDeferredClear of the next frame.
        for (u32 i = 0; i < noted; ++i)
        {
            IGameObject* O = level.Objects.net_Find(s_bones_prepass_noted[i]);
            if (!O || O->getDestroy() || !O->Visual())
                continue;
            // The view entity (first-person actor) and what it holds or wears: the actor camera, player_hud and the
            // HUD items own that path - left to the render exactly as before.
            if (view && (O == view || O->H_Root() == view))
                continue;
            // Before the filters below: a weapon in the "cooldown" (and its addon models) is prepared all the same.
            RenderPrepareCollect(O, candidates);
            candidate_ids.resize(candidates.size(), O->ID());
            // The render doesn't compute the bones of a frozen corpse pose (renderable_CorpsePoseFrozen) - neither do we.
            if (O->renderable_CorpsePoseFrozen())
                continue;
            IKinematics* K = PKinematics(O->Visual());
            // Only the bones the render really computed last frame (not the LOD-blob / skipped shadow casters) and not
            // a model the prepass recently missed on.
            if (!K || !K->BonesPrepass_Wanted())
                continue;
            candidates.push_back(K);
            candidate_ids.push_back(O->ID());
            s_bones_prepass_taken_frame[O->ID()] = frame;
        }
        PERF_ADD(prepass_candidates, candidates.size());
        // Phase A on main for every candidate: the blend callbacks (animation ends, Lua ScriptCallBack) fire here, on
        // the game logic thread, while nothing else runs game code (sequential frame: GameThread joined; game/render
        // overlap frame: the Game thread is not woken yet) - instead of on render workers.
        for (size_t i = 0; i < candidates.size(); ++i)
        {
            if (candidates[i]->BonesPrepass_PhaseA())
            {
                phase_b.push_back(candidates[i]);
                EarlyCountSkip(candidate_ids[i]); // pipelined frame: why it wasn't computed early (perf metrics)
            }
        }
        phase_b_regular = phase_b.size();
        // Phase C owners (the pipelined prepass): a held item's UpdateXForm in phase C computes its owner's bones
        // (CalculateBones(FALSE)) when the render drew the owner without computing them (an NPC drawn only as a shadow
        // blob: noted, not BonesPrepass_Wanted) - one owner after another on main, a burst whenever their 100 ms
        // UCalc_Interval runs out together. The same computation here: phase A on main (the blend callbacks stay on
        // this thread), phase B on the workers with the candidates; phase C then fast-outs. Same filters as phase C and
        // UpdateXFormFromOwner (a living or dead CEntityAlive holder, not the view entity's path); a frozen corpse pose
        // is left as it was.
        for (const u16 id : s_render_prepare_items)
        {
            IGameObject* O = level.Objects.net_Find(id);
            IGameObject* owner = O && !O->getDestroy() ? O->H_Parent() : nullptr;
            if (!owner || owner->getDestroy() || !owner->Visual() || owner == view || !owner->cast_entity_alive() ||
                owner->renderable_CorpsePoseFrozen())
                continue;
            if (s_bones_prepass_taken_frame[owner->ID()] == frame)
                continue;
            s_bones_prepass_taken_frame[owner->ID()] = frame;
            IKinematics* K = PKinematics(owner->Visual());
            if (!K)
                continue;
            if (s_kick_frame == frame && K->BonesPrepass_Done())
            {
                // Early kick: computed by the kick (LazyPhaseBEarly) and a mutator changed its blends since - the
                // dirty path of phase A recomputes it (a no-op for bones that are still valid)
                if (K->BonesPrepass_PhaseA())
                {
                    phase_b.push_back(K);
                    PERF_INC(prepass_redone_after_kick);
                }
            }
            else if (K->BonesPrepass_LazyPhaseA())
                phase_b.push_back(K);
        }
        PERF_ADD(prepare_owner_lazy, phase_b.size() - phase_b_regular);
    }
    // K3 / K2b (26-framemove-game-next.md, package 2-A): the last Lua of main before the kick has run (phase A);
    // phase B, the map update (K4), phase C and the physics kick are C++ only. Wake the Game thread for phase 0 now:
    // the sound dispatch and/or the Lua GC step of the frame run beside them. Game/render overlap frames only - their
    // KickGameLogicThread follows in CRenderDevice::ProcessFrame; a sequential frame has run its game logic already.
    if (Device.IsGameRenderOverlapFrame())
    {
        const bool early_gc = s_script_gc_pending.load(std::memory_order_acquire);
#if XR_PERF_METRICS
        // game/early_start_ms: how much sooner phase 0 starts thanks to the early kick - the phase A work it took off
        // this point, less the wait for its tasks it added here (an estimate, the kick's own share of main)
        if (s_kick_frame == frame && s_kick_main_ticks > s_kick_wait_ticks)
            xray::perf::AddTimeSlow(xray::perf::Metric::game_early_start_ms, s_kick_main_ticks - s_kick_wait_ticks);
#endif
        XRay::Engine::KickGameLogicEarly(true, early_gc ? &RunEarlyScriptGc : nullptr);
    }
    if (!phase_b.empty())
    {
        ZoneScopedN("BonesPrepass/phase_b");
        ZoneTextF("%u of %u candidates", static_cast<u32>(phase_b.size()), static_cast<u32>(candidates.size()));
        // Phase B in parallel, like the render's own build does it on its workers: per-model lock, nested computations
        // of other models (IK rays -> CCF_Skeleton::BuildState -> CalculateBones) only TryEnter, so no lock cycle (the
        // MX PR #576 AB-BA can't happen: no thread here waits for a model lock while it holds another one).
        const u32 count = static_cast<u32>(phase_b.size());
        const u32 workers = TaskScheduler ? std::max<u32>(static_cast<u32>(TaskScheduler->GetWorkersCount()), 1u) : 1u;
        // A stalker costs many times a pistol: small chunks (about 4 per worker) keep the workers balanced.
        const size_t grain = std::max<size_t>(1, count / (workers * 4));
        std::atomic<u32> computed{0};
        const auto phase_b_range = [&](const TaskRange<u32>& range)
        {
            ZoneScopedN("BonesPrepass/phase_b_task");
            PERF_PASS(xray::perf::pass::Prepass);
            PERF_SCOPE(prepass_phase_b);
            u32 local = 0;
            for (u32 i = range.begin(); i != range.end(); ++i)
            {
                // The phase C owners after the regular candidates: no prepass stamps (see LazyPhaseB), not counted in
                // prepass/computed (prepare/owner_lazy)
                if (i >= phase_b_regular)
                    phase_b[i]->BonesPrepass_LazyPhaseB();
                else if (phase_b[i]->BonesPrepass_PhaseB())
                    ++local;
            }
            computed.fetch_add(local, std::memory_order_relaxed);
        };
        if (s_map_update_pending && TaskScheduler)
        {
            // K4: main does its own C++ work while the workers take the phase B chunks, then helps with the rest
            // (TaskManager::Wait runs queued chunks). The map update reads object positions (an attached item's XFORM
            // may be written by phase B right then - a torn position of a map spot for one frame at worst) and
            // creates/destroys map spots (CUIWindow, main thread only); no Lua, bones or ODE.
            Task& phase_b_task = xr_parallel_for(TaskRange<u32>(0, count, grain), false, phase_b_range);
            RunPendingMapUpdate();
            ZoneScopedN("BonesPrepass/phase_b_wait");
            PERF_SCOPE(prepass_main_slot_wait);
            TaskScheduler->Wait(phase_b_task);
        }
        else
            xr_parallel_for(TaskRange<u32>(0, count, grain), true, phase_b_range);
        PERF_ADD(prepass_computed, computed.load(std::memory_order_relaxed));
    }

    // Phase C (render prepare of held items): after phase B - a held item's XFORM is built from its owner's bones,
    // which are final now. Runs even if phase B had nothing to do (the owners computed by the game logic this frame).
    RenderPreparePhaseC(level, view);
}

void CLevel::OnFrame()
{
    ZoneScoped;
    PERF_SCOPE(lvl_frame_total);

    if (xray::perf::g_perf_metrics_active)
    {
        static u32 s_npc_r_hist_ms = 0;
        const u32 now = Device.dwTimeContinual;
        if (now - s_npc_r_hist_ms >= 5000)
        {
            ZoneScopedN("level/npc_radius_hist"); // P19b: perf_metrics only, once per 5 s, walks every object
            s_npc_r_hist_ms = now;
            // Both radius sources (§8.6): the visual sphere drives render culling, the spatial sphere
            // drives AI/physics range checks - they can differ (e.g. a crouching stalker's visual sphere
            // shrinks, the spatial one may not). Stalkers and monsters are counted into separate buckets.
            using M = xray::perf::Metric;
            struct RadiusBuckets
            {
                M b0_05, b05_1, b1_15, b15_2, b2_3, b3p, bgt1;
            };
            static const RadiusBuckets vis_stlk{M::npc_r_vis_stlk_0_05, M::npc_r_vis_stlk_05_1,
                M::npc_r_vis_stlk_1_15, M::npc_r_vis_stlk_15_2, M::npc_r_vis_stlk_2_3, M::npc_r_vis_stlk_3p,
                M::npc_r_vis_stlk_gt1};
            static const RadiusBuckets vis_mon{M::npc_r_vis_mon_0_05, M::npc_r_vis_mon_05_1,
                M::npc_r_vis_mon_1_15, M::npc_r_vis_mon_15_2, M::npc_r_vis_mon_2_3, M::npc_r_vis_mon_3p,
                M::npc_r_vis_mon_gt1};
            static const RadiusBuckets spt_stlk{M::npc_r_spt_stlk_0_05, M::npc_r_spt_stlk_05_1,
                M::npc_r_spt_stlk_1_15, M::npc_r_spt_stlk_15_2, M::npc_r_spt_stlk_2_3, M::npc_r_spt_stlk_3p,
                M::npc_r_spt_stlk_gt1};
            static const RadiusBuckets spt_mon{M::npc_r_spt_mon_0_05, M::npc_r_spt_mon_05_1,
                M::npc_r_spt_mon_1_15, M::npc_r_spt_mon_15_2, M::npc_r_spt_mon_2_3, M::npc_r_spt_mon_3p,
                M::npc_r_spt_mon_gt1};
            auto inc_bucket = [](float r, const RadiusBuckets& b) {
                // gt1 is not an 8th bucket; it shares the same >1m boundary as the start of b1_15, so the
                // "fraction above 1m" reading is always exactly the sum of b1_15+b15_2+b2_3+b3p.
                // PERF_INC can't be used here: it macro-expands to xray::perf::Metric::<arg>, but the
                // bucket to hit is a runtime field of `b`, not a compile-time Metric name.
                if (r > 1.f)
                    xray::perf::Add(b.bgt1);
                if (r < 0.5f)
                    xray::perf::Add(b.b0_05);
                else if (r <= 1.f)
                    xray::perf::Add(b.b05_1);
                else if (r < 1.5f)
                    xray::perf::Add(b.b1_15);
                else if (r < 2.f)
                    xray::perf::Add(b.b15_2);
                else if (r < 3.f)
                    xray::perf::Add(b.b2_3);
                else
                    xray::perf::Add(b.b3p);
            };
            const u32 n = Objects.o_count();
            for (u32 i = 0; i < n; ++i)
            {
                IGameObject* O = Objects.o_get_by_iterator(i);
                if (!O)
                    continue;
                const bool is_stalker = O->cast_stalker() != nullptr;
                if (!is_stalker && !O->cast_base_monster())
                    continue;
                CEntityAlive* ea = O->cast_entity_alive();
                if (!ea || !ea->g_Alive())
                    continue;
                inc_bucket(O->Radius(), is_stalker ? vis_stlk : vis_mon);
                inc_bucket(O->GetSpatialData().sphere.R, is_stalker ? spt_stlk : spt_mon);
            }
        }
    }

#ifdef DEBUG
    DBG_RenderUpdate();
#endif
    Fvector temp_vector;
    {
        // P19b (14-spike-instrumentation.md): the parts of CLevel::OnFrame P19 left without a phase
        LEVEL_FRAME_PHASE("level/feel_deny", lvl_frame_feel_deny);
        m_feel_deny.feel_touch_update(temp_vector, 0.f);
    }
    if (GameID() != eGameIDSingle)
        psDeviceFlags.set(rsDisableObjectsAsCrows, true);
    else
        psDeviceFlags.set(rsDisableObjectsAsCrows, false);
    // commit events from bullet manager from prev-frame
    {
        LEVEL_FRAME_PHASE("level/bullets_commit", lvl_frame_bullets);
        stats.BulletManagerCommit.Begin();
        BulletManager().CommitEvents();
        stats.BulletManagerCommit.End();
    }
    // Client receive
    if (net_isDisconnected())
    {
        if (OnClient() && GameID() != eGameIDSingle)
        {
#ifdef DEBUG
            Msg("--- I'm disconnected, so clear all objects...");
#endif
            ClearAllObjects();
        }
        Engine.Event.Defer("kernel:disconnect");
        return;
    }
    else
    {
        LEVEL_FRAME_PHASE("level/client_receive", lvl_frame_client_receive);
        stats.ClientRecv.Begin();
        ClientReceive();
        stats.ClientRecv.End();
    }
    {
        LEVEL_FRAME_PHASE("level/game_events", lvl_frame_game_events);
        GW_FRAME_PHASE(LvlEvents); // 50n: the frame spike report
        ProcessGameEvents(true); // the only call with the spawn budget (spawn_budget_us, SPEC.md 5.A)
    }
    if (m_bNeed_CrPr)
        make_NetCorrectionPrediction();
    if (!GEnv.isDedicatedServer)
    {
        if (IsGameTypeSingle() && ai().get_alife())
        {
            LEVEL_FRAME_PHASE("level/alife_positions", lvl_frame_alife_positions);
            ai().alife().update_positions_only();
        }
        // CMapManager::Update may destroy CMapSpot (CUIWindow) — must not run on seqParallel/worker
        // (~CUIWindow detaches from UI hierarchy; off-main-thread caused crashes / inconsistent UI state).
        {
            LEVEL_FRAME_PHASE("level/map_manager", lvl_frame_map_manager);
            // K4: the update waits for the phase B window of the bones prepass of this frame (CLevel::BonesPrepass; it
            // runs on main on every way out of it). Still pending here = the previous frame had no prepass (window
            // inactive, loading): then this frame updates here, never twice in one frame (CMapLocation::Update asserts
            // that).
            if (!s_map_update_pending)
                s_map_update_pending = true;
            else
            {
                if (s_map_update_pending)
                    PERF_INC(prepass_map_update_late);
                s_map_update_pending = false;
                MapManager().Update();
            }
        }
        if (IsGameTypeSingle() && Device.dwPrecacheFrame == 0)
        {
            // XXX nitrocaster: was enabled in x-ray 1.5; to be restored or removed
            // if (g_mt_config.test(mtMap))
            //{
            //    Device.seqParallel.push_back(fastdelegate::FastDelegate0<>(
            //    m_game_task_manager,&CGameTaskManager::UpdateTasks));
            //}
            // else
            LEVEL_FRAME_PHASE("level/update_tasks", lvl_frame_tasks);
            GW_FRAME_PHASE(LvlScript); // 50n: the frame spike report
            GameTaskManager().UpdateTasks();
        }
    }
    // Inherited update
    {
        // objects UpdateCL + destroy + HUD: no spike phase (tens of ms is normal), each object UpdateCL is a spike site
        PERF_SCOPE(lvl_frame_objects);
        GW_FRAME_PHASE(LvlObjects); // 50n: the frame spike report
        inherited::OnFrame();
    }
    if (!Device.Paused())
    {
        // 06-framemove-offload.md, B4: level_on_frame always goes to the game logic thread, OnGameLogicFrame of this
        // frame (after UpdateCL, before the scheduler; in a game/render overlap frame parallel to the render)
        m_level_on_frame_pending = true;
        // Plugin timers (timer_start) stay here, on main: a timer may send any event, to any subscriber
        LEVEL_FRAME_PHASE("level/plugin_timers", lvl_frame_plugin_timers);
        GW_FRAME_PHASE(LvlScript); // 50n: the frame spike report
        gw::addons::timers::Update(Device.dwTimeDelta);
    }
    // Draw client/server stats
    if (!GEnv.isDedicatedServer && psDeviceFlags.test(rsStatistic))
    {
        CGameFont* F = UI().Font().pFontDI;
        if (!psNET_direct_connect)
        {
            if (IsServer())
            {
                const IServerStatistic* S = Server->GetStatistic();
                F->SetHeightI(0.015f);
                F->OutSetI(0.0f, 0.5f);
                F->SetColor(color_xrgb(0, 255, 0));
                F->OutNext("IN:  %4d/%4d (%2.1f%%)", S->bytes_in_real, S->bytes_in,
                    100.f * float(S->bytes_in_real) / float(S->bytes_in));
                F->OutNext("OUT: %4d/%4d (%2.1f%%)", S->bytes_out_real, S->bytes_out,
                    100.f * float(S->bytes_out_real) / float(S->bytes_out));
                F->OutNext("client_2_sever ping: %d", net_Statistic.getPing());
                F->OutNext("SPS/Sended : %4d/%4d", S->dwBytesPerSec, S->dwBytesSended);
                F->OutNext("sv_urate/cl_urate : %4d/%4d", psNET_ServerUpdate, psNET_ClientUpdate);
                F->SetColor(color_xrgb(255, 255, 255));
                struct net_stats_functor
                {
                    xrServer* m_server;
                    CGameFont* F;
                    void operator()(IClient* C)
                    {
                        m_server->UpdateClientStatistic(C);
                        F->OutNext("0x%08x: P(%d), BPS(%2.1fK), MRR(%2d), MSR(%2d), Retried(%2d), Blocked(%2d)",
                            // Server->game->get_option_s(*C->Name,"name",*C->Name),
                            C->ID.value(), C->stats.getPing(),
                            float(C->stats.getBPS()), // /1024,
                            C->stats.getMPS_Receive(), C->stats.getMPS_Send(), C->stats.getRetriedCount(),
                            C->stats.dwTimesBlocked);
                    }
                };
                net_stats_functor tmp_functor;
                tmp_functor.m_server = Server;
                tmp_functor.F = F;
                Server->ForEachClientDo(tmp_functor);
            }
            if (IsClient())
            {
                IPureClient::UpdateStatistic();
                F->SetHeightI(0.015f);
                F->OutSetI(0.0f, 0.5f);
                F->SetColor(color_xrgb(0, 255, 0));
                F->OutNext("client_2_sever ping: %d", net_Statistic.getPing());
                F->OutNext("sv_urate/cl_urate : %4d/%4d", psNET_ServerUpdate, psNET_ClientUpdate);
                F->SetColor(color_xrgb(255, 255, 255));
                F->OutNext("BReceivedPs(%2d), BSendedPs(%2d), Retried(%2d), Blocked(%2d)",
                    net_Statistic.getReceivedPerSec(), net_Statistic.getSendedPerSec(), net_Statistic.getRetriedCount(),
                    net_Statistic.dwTimesBlocked);
#ifdef DEBUG
                if (!pStatGraphR)
                {
                    pStatGraphR = xr_new<CStatGraph>();
                    pStatGraphR->SetRect(50, 700, 300, 68, 0xff000000, 0xff000000);
                    // m_stat_graph->SetGrid(0, 0.0f, 10, 1.0f, 0xff808080, 0xffffffff);
                    pStatGraphR->SetMinMax(0.0f, 65536.0f, 1000);
                    pStatGraphR->SetStyle(CStatGraph::stBarLine);
                    pStatGraphR->AppendSubGraph(CStatGraph::stBarLine);
                }
                pStatGraphR->AppendItem(float(net_Statistic.getBPS()), 0xff00ff00, 0);
                F->OutSet(20.f, 700.f);
                F->OutNext("64 KBS");
#endif
            }
        }
    }
    else
    {
#ifdef DEBUG
        if (pStatGraphR)
            xr_delete(pStatGraphR);
#endif
    }
    {
        LEVEL_FRAME_PHASE("level/game_time", lvl_frame_game_time);
        g_pGamePersistent->Environment().SetGameTime(GetEnvironmentGameDayTimeSec(), game->GetEnvironmentGameTimeFactor());
    }
    if (!GEnv.isDedicatedServer)
    {
        LEVEL_FRAME_PHASE("level/script_process", lvl_frame_script_process);
        GW_FRAME_PHASE(LvlScript); // 50n: the frame spike report
        GEnv.ScriptEngine->script_process(ScriptProcessor::Level)->update();
    }
    {
        LEVEL_FRAME_PHASE("level/ph_commander", lvl_frame_ph_commander);
        m_ph_commander->update();
    }
    {
        LEVEL_FRAME_PHASE("level/add_call", lvl_frame_add_call);
        GW_FRAME_PHASE(LvlScript); // 50n: the frame spike report
        // 32-frame-spikes.md, npc_perf_lua_spike_us > 0: the phase over the threshold goes to the log as
        // "[lua-spike] add_call" with its slowest call (the Lua source of its condition/action), the heap and the GC
        const bool lua_spike = gw::lua_spike::Enabled();
        m_ph_commander_scripts->set_describe_slowest(lua_spike);
        gw::lua_spike::Probe spike(lua_spike && GEnv.ScriptEngine ? GEnv.ScriptEngine->lua() : nullptr,
            gw::lua_spike::ECategory::AddCall);
        m_ph_commander_scripts->update();
        if (spike.Finish())
            spike.Report(m_ph_commander_scripts->slowest_call(), gw::lua_spike::kNoId, gw::lua_spike::kNoId);
    }
    {
        // the second part of frame/level/bullets (its calls = 2 per frame)
        LEVEL_FRAME_PHASE("level/bullets_render_set", lvl_frame_bullets);
        stats.BulletManagerCommit.Begin();
        BulletManager().CommitRenderSet();
        stats.BulletManagerCommit.End();
    }
    // update static sounds
    if (!GEnv.isDedicatedServer)
    {
        if (g_mt_config.test(mtLevelSounds))
        {
            R_ASSERT(m_level_sound_manager);
            Device.seqParallel.push_back(
                fastdelegate::FastDelegate0<>(m_level_sound_manager, &CLevelSoundManager::Update));
        }
        else
        {
            LEVEL_FRAME_PHASE("level/level_sounds", lvl_frame_level_sounds);
            m_level_sound_manager->Update();
        }

        // defer LUA-GC-STEP
        if (g_mt_config.test(mtLUA_GC))
        {
            // K2b: phase 0 of the Game thread takes it (CLevel::BonesPrepass); if the frame has no phase 0, the
            // seqParallel delegate runs it at the old place
            s_script_gc_pending.store(true, std::memory_order_release);
            Device.seqParallel.push_back(fastdelegate::FastDelegate0<>(&RunSeqScriptGc));
        }
        else
            script_gc();
    }
    // Early kick: the level's game code of the frame is done - the rest of the prepass's
    // phase A here, its phase B beside SoundProcessor, the UI and the RT scheduler (a no-op unless the pipeline ran)
    EarlyKickTail();
    if (pStatGraphR)
    {
        static float fRPC_Mult = 10.0f;
        static float fRPS_Mult = 1.0f;
        pStatGraphR->AppendItem(float(m_dwRPC) * fRPC_Mult, 0xffff0000, 1);
        pStatGraphR->AppendItem(float(m_dwRPS) * fRPS_Mult, 0xff00ff00, 0);
    }
}

#undef LEVEL_FRAME_PHASE

void CLevel::EmitLevelOnFrame()
{
    if (!gw::addons::events::HasSubscribers(gw::addons::events::EBuiltin::LevelOnFrame))
        return;
    const GwpValue args[] = { gw::addons::events::Number(Device.fTimeDelta) };
    gw::addons::events::Emit(gw::addons::events::EBuiltin::LevelOnFrame, args, 1);
}

// plans/optimization_mt/06-framemove-offload.md (B4/B5). The game logic thread of the frame: XRay::Engine::GameThread
// (sequential frame, the main thread waits for it) or the game/render overlap Game thread (parallel to the 3D render).
// Order: SoundEvent_Dispatch (the sounds heard this frame) -> the sound callbacks of the NPCs (they arm the script
// danger of gw_npc) -> level_on_frame (the inputs gw_npc runs read that danger) -> the scheduler (the Think of the
// NPCs reads the inputs).
void CLevel::OnGameLogicFrame()
{
    ZoneScoped;
    PERF_SCOPE(game_logic_frame);
    // 29-game-thread-next.md, A2: UpdateWorkload of the bullets of a game/render overlap frame as a
    // task from here (the dispatch of the frame is over - nothing on the Game thread plays or reads the bullets' sounds
    // beside it any more) to its old place in seqParallel, where the Game thread waits for it (no-op unless
    // CommitRenderSet of this frame asked for it)
    if (Device.IsGameRenderOverlapFrame())
        BulletManager().StartWorkloadTask();
    {
        ZoneScopedN("hear_callbacks");
        PERF_SCOPE(game_hear_callbacks);
        CScriptEntity::ProcessQueuedSoundCallbacks();
    }
    if (m_level_on_frame_pending)
    {
        m_level_on_frame_pending = false;
        ZoneScopedN("level_on_frame");
        PERF_SCOPE(game_level_on_frame);
        EmitLevelOnFrame();
    }
    // 32-frame-spikes.md, A1: a part of the ALife update every frame (seqParallel of this frame)
    // instead of the whole of it on the turn of the scheduler; a no-op with the switch off
    if (CALifeSimulator* alife = const_cast<CALifeSimulator*>(ai().get_alife()))
        alife->spread_frame();
}

// plans/optimization_mt/22-game-thread.md, step 2: the sound delegates of a frame (SoundEvent_Register queues one per
// listener in range, ~500 per sound in a crowd) cost ~1.2 us each in feel_sound_new, and every NPC that heard
// something also runs its hear callbacks (npc_on_hear_callback emits) in the next OnGameLogicFrame.
// CLevel::SoundEvent_Filter below caps and dedupes them, always (2026-10-14: the developer accepts that cut far
// listeners lose npc_on_hear_callback of such a sound). The tunables are cvars (console_commands.cpp).
// A listener closer to the sound than this always gets it: gw_npc and xr_danger arm the script danger only within 30 m
// of the sound (npc_on_hear_callback, dist_sqr < 900), plus a margin for the listener moving until its callbacks run
float npc_perf_sound_dispatch_near_dist = 35.f;
// A listener closer to the actor than this always gets every combat sound: what the player can see reacts as before
// (G3 below: the non-combat sounds of other objects use npc_sound_noncombat_dist instead of both of these limits)
float npc_perf_sound_dispatch_actor_dist = 50.f;
// Beyond those, a sound goes to at most this many of the other listeners, the loudest ones (power with occlusion):
// combat sounds (shot, bullet hit, explosion, monster attack / injury / death) and the rest
int npc_perf_sound_dispatch_max_combat = 64;
int npc_perf_sound_dispatch_max = 32;

// plans/optimization_mt/24-critical-path.md, G3 (always on since 2026-10-16; saved tunables, console_commands.cpp).
// In a crowd fight nearly every listener is within npc_perf_sound_dispatch_actor_dist of the actor, so the cap above
// cuts almost nothing (~2000 deliveries a frame, ~1.3 us each). Combat sounds and the sounds of the actor are never
// touched by these limits.
// m: a non-combat sound (step, voice, reload, item, world) of another object goes to every listener within this
// distance of it, to the npc_perf_sound_dispatch_max loudest of the others and to the listeners with the Lua sound
// callback; the actor distance exemption does not apply to it
float npc_sound_noncombat_dist = 20.f;
// > 0: a listener gets at most this many non-combat sounds of other objects a frame, the loudest (power with distance
// and occlusion); a listener with the Lua sound callback keeps all of them. 0 = no cap
int npc_sound_noncombat_per_listener = 0;

namespace
{
// The sound type is a bit mask: a sound is of a type when it has all bits of that type
constexpr u32 SoundDispatchCombatTypes[] = { SOUND_TYPE_WEAPON_SHOOTING, SOUND_TYPE_WEAPON_BULLET_HIT,
    SOUND_TYPE_OBJECT_EXPLODING, SOUND_TYPE_MONSTER_ATTACKING, SOUND_TYPE_MONSTER_INJURING, SOUND_TYPE_MONSTER_DYING };

bool SoundDispatchIsCombat(int type)
{
    const u32 mask = static_cast<u32>(type);
    for (const u32 combat : SoundDispatchCombatTypes)
    {
        if ((mask & combat) == combat)
            return true;
    }
    return false;
}

// One sound event of the frame: the run [begin, end) of the delegates
struct SoundDispatchRun
{
    u32 begin;
    u32 end;
    const IGameObject* source;
    int type;
};

struct SoundDispatchCandidate
{
    float power;
    u32 index;
};

struct SoundDispatchCopy
{
    const Feel::Sound* dest;
    float power;
    u32 index;
};

// The class of a delegate's sound (24-critical-path.md, G3): only SoundDispatchOther is cut by the G3 limits
enum SoundDispatchClass : u8
{
    SoundDispatchOther,
    SoundDispatchCombat,
    SoundDispatchActor, // the actor's own sounds and those of what it holds, shot or threw
    SoundDispatchClassCount
};

// A delivery in the listener order (24-critical-path.md, G3)
struct SoundDispatchOrder
{
    const Feel::Sound* dest;
    u32 index;
};

// Scratch of CLevel::SoundEvent_Filter, kept between frames (no allocation per frame); game logic thread only
xr_vector<SoundDispatchRun> s_sound_dispatch_runs;
xr_vector<SoundDispatchCandidate> s_sound_dispatch_candidates;
xr_vector<SoundDispatchCopy> s_sound_dispatch_copies;
xr_vector<u8> s_sound_dispatch_classes;
xr_vector<SoundDispatchOrder> s_sound_dispatch_order;
xr_vector<IGame_Level::_esound_delegate> s_sound_dispatch_grouped;

// xr_hear.reset_hear_callback sets the Lua sound callback for a section with on_sound only: its condlists react to
// sounds at the distance and power they name, so such a listener keeps every sound
bool SoundDispatchKeepsAll(const IGame_Level::_esound_delegate& entry)
{
    CCustomMonster* monster = entry.dest_object->cast_custom_monster();
    return monster && monster->callback(GameObject::eSound);
}
} // namespace

// The game logic thread, from SoundEvent_Dispatch before any delegate goes out. A delegate is dropped by setting its
// dest to null; the rest are grouped by listener (see below).
// - Cap: a sound goes to every listener within npc_perf_sound_dispatch_near_dist of it, within
//   npc_perf_sound_dispatch_actor_dist of the actor, or with the Lua sound callback set (an xr_hear on_sound section),
//   and to at most npc_perf_sound_dispatch_max[_combat] of the other listeners, the loudest. The sounds of the actor
//   and of what it holds or shot (steps, shots, items, bullet hits and whines: their object is the actor) are not
//   capped at all - stealth and the reaction to the player stay as before. A non-combat sound of another object
//   (G3) uses npc_sound_noncombat_dist instead and no actor distance exemption.
// - Dedupe: the same source and sound type reaches a listener once a frame, the loudest delegate (of equal ones the
//   later). CSoundMemoryManager raises its threshold to each sound it hears and CScriptEntity::sound_callback merges
//   the same source and type into one saved sound, so the quieter copy changes nothing there.
// - G3, npc_sound_noncombat_per_listener (> 0): at most that many non-combat sounds of other objects per listener,
//   the loudest.
// - G3, grouped: the kept delegates are reordered so that the deliveries of one listener go out back to back (the
//   memory managers of an NPC stay in the cache over its ~3 sounds of the frame instead of being reloaded for each).
//   SoundEvent_Dispatch takes them from the back, so each listener still gets its sounds in the order it got them
//   before; only the order between listeners changes (and with it the order of the NPCs in the hear callback queue
//   of the next frame).
void CLevel::SoundEvent_Filter(xr_vector<_esound_delegate>& events)
{
    if (events.empty())
        return;
    ZoneScoped;
    PERF_SCOPE(npc_sound_filter);

    const IGameObject* actor = CurrentEntity();
    const float near_dist = npc_perf_sound_dispatch_near_dist;
    const float actor_dist_sqr = _sqr(npc_perf_sound_dispatch_actor_dist);
    // G3 tunables, read once: the console runs on main while this runs on the game logic thread
    const float noncombat_dist = npc_sound_noncombat_dist;
    const u32 per_listener = npc_sound_noncombat_per_listener > 0 ? static_cast<u32>(npc_sound_noncombat_per_listener) : 0;
    u32 capped = 0;
    u32 deduped = 0;
    u32 noncombat_cut = 0;
    u32 listener_capped = 0;

    // The runs of the sound events: SoundEvent_Register queues the listeners of one event back to back
    xr_vector<SoundDispatchRun>& runs = s_sound_dispatch_runs;
    runs.clear();
    const u32 count = static_cast<u32>(events.size());
    for (u32 begin = 0; begin < count;)
    {
        u32 end = begin + 1;
        while (end < count && events[end].event == events[begin].event)
            ++end;
        const ref_sound& sound = events[begin].source;
        // A stopped sound (no feedback) is skipped by the dispatch, and its object may be destroyed already (the
        // relcase clears g_object only for live emitters): no source, so H_Root below never touches it
        runs.push_back({ begin, end, sound->feedback ? sound->g_object : nullptr, sound->g_type });
        begin = end;
    }

    // Cap
    xr_vector<SoundDispatchCandidate>& candidates = s_sound_dispatch_candidates;
    xr_vector<u8>& classes = s_sound_dispatch_classes;
    classes.resize(count);
    for (const SoundDispatchRun& run : runs)
    {
        bool actor_sound = actor && run.source && (run.source == actor || run.source->H_Root() == actor);
        // A grenade or a charge of the actor: no parent any more after the throw, its initiator tells whose it is
        if (!actor_sound && actor && run.source)
        {
            // const_cast: smart_cast and CExplosive::Initiator() are non-const, the object is only read
            CExplosive* explosive = smart_cast<CExplosive*>(const_cast<IGameObject*>(run.source));
            actor_sound = explosive && explosive->Initiator() == actor->ID();
        }
        const bool combat = SoundDispatchIsCombat(run.type);
        const u8 sound_class = actor_sound ? SoundDispatchActor : (combat ? SoundDispatchCombat : SoundDispatchOther);
        std::fill(classes.begin() + run.begin, classes.begin() + run.end, sound_class);
        if (actor_sound)
            continue;
        // G3: a non-combat sound of another object - a smaller always-zone, no actor distance exemption
        const bool noncombat_cap = !combat;
        const u32 limit = static_cast<u32>(combat ? npc_perf_sound_dispatch_max_combat : npc_perf_sound_dispatch_max);
        if (run.end - run.begin <= limit)
            continue;
        candidates.clear();
        for (u32 i = run.begin; i < run.end; ++i)
        {
            const _esound_delegate& entry = events[i];
            if (noncombat_cap)
            {
                if (entry.dist <= noncombat_dist)
                    continue;
            }
            else
            {
                if (entry.dist <= near_dist)
                    continue;
                if (actor && entry.dest_object->Position().distance_to_sqr(actor->Position()) <= actor_dist_sqr)
                    continue;
            }
            candidates.push_back({ entry.power, i });
        }
        if (candidates.size() <= limit)
            continue;
        std::nth_element(candidates.begin(), candidates.begin() + limit, candidates.end(),
            [](const SoundDispatchCandidate& a, const SoundDispatchCandidate& b) { return a.power > b.power; });
        for (auto it = candidates.begin() + limit; it != candidates.end(); ++it)
        {
            _esound_delegate& entry = events[it->index];
            if (SoundDispatchKeepsAll(entry))
                continue;
            // The G3 share: dropped although the old rules (near_dist, actor distance) always keep it
            if (noncombat_cap && (entry.dist <= near_dist ||
                    (actor && entry.dest_object->Position().distance_to_sqr(actor->Position()) <= actor_dist_sqr)))
            {
                ++noncombat_cut;
            }
            entry.dest = nullptr;
            ++capped;
        }
    }

    // Dedupe: only a source heard in two or more events of the frame can reach a listener twice
    if (runs.size() > 1)
    {
        std::sort(runs.begin(), runs.end(), [](const SoundDispatchRun& a, const SoundDispatchRun& b)
        {
            if (a.source != b.source)
                return std::less<const IGameObject*>()(a.source, b.source);
            return a.type < b.type;
        });
        xr_vector<SoundDispatchCopy>& copies = s_sound_dispatch_copies;
        for (size_t first = 0; first < runs.size();)
        {
            size_t last = first + 1;
            while (last < runs.size() && runs[last].source == runs[first].source && runs[last].type == runs[first].type)
                ++last;
            // A sound without an object (the world) has no source to compare: never merged
            if (last - first > 1 && runs[first].source)
            {
                copies.clear();
                for (size_t r = first; r < last; ++r)
                {
                    for (u32 i = runs[r].begin; i < runs[r].end; ++i)
                    {
                        if (events[i].dest)
                            copies.push_back({ events[i].dest, events[i].power, i });
                    }
                }
                std::sort(copies.begin(), copies.end(), [](const SoundDispatchCopy& a, const SoundDispatchCopy& b)
                {
                    if (a.dest != b.dest)
                        return std::less<const Feel::Sound*>()(a.dest, b.dest);
                    if (a.power != b.power)
                        return a.power > b.power;
                    return a.index > b.index;
                });
                for (size_t k = 1; k < copies.size(); ++k)
                {
                    if (copies[k].dest != copies[k - 1].dest)
                        continue;
                    events[copies[k].index].dest = nullptr;
                    ++deduped;
                }
            }
            first = last;
        }
    }

    // G3: the kept delegates in the listener order (by listener, then by the original index)
    xr_vector<SoundDispatchOrder>& order = s_sound_dispatch_order;
    order.clear();
    for (u32 i = 0; i < count; ++i)
    {
        if (events[i].dest)
            order.push_back({ events[i].dest, i });
    }
    std::sort(order.begin(), order.end(), [](const SoundDispatchOrder& a, const SoundDispatchOrder& b)
    {
        if (a.dest != b.dest)
            return std::less<const Feel::Sound*>()(a.dest, b.dest);
        return a.index < b.index;
    });

    // G3: per listener at most per_listener non-combat sounds of other objects, the loudest
    if (per_listener)
    {
        for (size_t first = 0; first < order.size();)
        {
            size_t last = first + 1;
            while (last < order.size() && order[last].dest == order[first].dest)
                ++last;
            candidates.clear();
            for (size_t k = first; k < last; ++k)
            {
                const u32 index = order[k].index;
                if (classes[index] == SoundDispatchOther)
                    candidates.push_back({ events[index].power, index });
            }
            if (candidates.size() > per_listener && !SoundDispatchKeepsAll(events[order[first].index]))
            {
                std::nth_element(candidates.begin(), candidates.begin() + per_listener, candidates.end(),
                    [](const SoundDispatchCandidate& a, const SoundDispatchCandidate& b) { return a.power > b.power; });
                for (auto it = candidates.begin() + per_listener; it != candidates.end(); ++it)
                {
                    events[it->index].dest = nullptr;
                    ++listener_capped;
                }
            }
            first = last;
        }
    }

    // What goes out, by class (24-critical-path.md, G3: how much of the dispatch the non-combat cuts can reach)
    u32 kept[SoundDispatchClassCount] = {};
    xr_vector<_esound_delegate>& out = s_sound_dispatch_grouped;
    out.clear();
    out.reserve(order.size());
    for (const SoundDispatchOrder& entry : order)
    {
        _esound_delegate& delegate = events[entry.index];
        if (!delegate.dest)
            continue; // dropped by the per-listener cap
        ++kept[classes[entry.index]];
        out.push_back(std::move(delegate));
    }
    events.swap(out);
    out.clear(); // the old delegates, all moved from or dropped: no sound is held between frames
    PERF_ADD(npc_sound_capped, capped);
    PERF_ADD(npc_sound_deduped, deduped);
    PERF_ADD(npc_sound_noncombat_cut, noncombat_cut);
    PERF_ADD(npc_sound_listener_capped, listener_capped);
    PERF_ADD(npc_sound_kept_combat, kept[SoundDispatchCombat]);
    PERF_ADD(npc_sound_kept_other, kept[SoundDispatchOther]);
    PERF_ADD(npc_sound_kept_actor, kept[SoundDispatchActor]);
    // without XR_PERF_METRICS
    (void)capped;
    (void)deduped;
    (void)noncombat_cut;
    (void)listener_capped;
    (void)kept;
}

int psLUA_GCSTEP = 100; // 10
int psLUA_GCTIMEOUT = 1000;

u32 ps_lua_gc_method = 1;

#if XR_PERF_METRICS
namespace
{
// G1 (plans/optimization_mt/32-frame-spikes.md): the detail of a long paced GC frame in "[perf-spike] lua/script_gc"
void DescribeGcSpike(const void* ctx, char* buf, size_t size) noexcept
{
    const auto& gc = *static_cast<const gw::lua_spike::GcFrameStats*>(ctx);
    xr_sprintf(buf, size, "steps=%llu debt=%lld->%lld budget=%uus uncapped=%d%s atomic=%s max_step=%uus heap=%dKB "
        "cycle_start=%dKB", static_cast<unsigned long long>(gc.steps), static_cast<long long>(gc.due_before),
        static_cast<long long>(gc.due_after), gc.budget_us, gc.uncapped ? 1 : 0, gc.heap_guard ? "(heap)" : "",
        gc.atomic < 0 ? "?" : gc.atomic ? "1" : "0", gc.max_step_us, gc.heap_kb, gc.cycle_start_kb);
}
} // namespace
#endif

void CLevel::script_gc()
{
    ZoneScoped;
    AIStats.LuaGC.Begin();
    lua_State* L = GEnv.ScriptEngine ? GEnv.ScriptEngine->lua() : nullptr;
    if (L)
        xray::perf::SetGauge(xray::perf::Metric::lua_gc_kb, lua_gc(L, LUA_GCCOUNT, 0));
#if XR_PERF_METRICS
    {
        // The luabind instances without __gc (32-frame-spikes.md): luabind keeps a running total, the metric gets what was made since
        static u64 nogc_seen = 0;
        const u64 nogc_now = luabind::detail::nonowning_instance_nogc_created();
        PERF_ADD(lua_udata_nogc_created, nogc_now - nogc_seen);
        nogc_seen = nogc_now;
    }
#endif
    // G2 (32-frame-spikes.md): lua_gc_pause/lua_gc_stepmul, lua_gc_atomic_stats, lua_heap_census - outside lua/gc_step
    gw::lua_spike::ServiceGcDiagnostics(L);
    PERF_SCOPE(lua_gc_step);
    GW_FRAME_EVENT(LuaGc); // 50n: the frame spike report
    // P18 (plans/optimization_mt/13-lua-spikes.md), npc_perf_lua_spike_us > 0: a long GC step goes to the log, and an
    // atomic phase inside the step is counted apart (lua/gc_atomic_in_step of lua/gc_atomic)
    const bool probe_atomic = gw::lua_spike::Enabled() && L;
    const u32 atomic_before = probe_atomic ? gw::lua_spike::PollAtomic(L) : 0;
    gw::lua_spike::Probe spike(L, gw::lua_spike::ECategory::GcStep);

    switch (ps_lua_gc_method)
    {
    case 0:
        break;

    case 2:
        if (lua_gc(GEnv.ScriptEngine->lua(), LUA_GCTIMEOUT, psLUA_GCTIMEOUT) >= 0)
            break;
        // LUA_GCTIMEOUT is unsupported, fallback to LUA_GCSTEP
        [[fallthrough]];

    default:
        ps_lua_gc_method = 1;

    case 1:
    {
        // G1 (32-frame-spikes.md), perf_spike_us: a long paced GC frame -> "[perf-spike] lua/script_gc" with its steps,
        // debt, budget, whether it went uncapped, the atomic phase and the heap (gw::lua_spike::LastGcFrame)
        PERF_SPIKE_NAMED(gc_spike, "lua/script_gc", &gw::lua_spike::LastGcFrame(), &DescribeGcSpike, true);
        // The paced steps (lua_spike_probe.h): the GC of the frame works only here, psLUA_GCSTEP KB on top
        gw::lua_spike::GcStepFrame(GEnv.ScriptEngine->lua(), psLUA_GCSTEP);
        break;
    }
    }

    if (probe_atomic && gw::lua_spike::PollAtomic(L) != atomic_before)
        PERF_INC(lua_gc_atomic_in_step);
    if (spike.Finish())
        spike.Report(ps_lua_gc_method == 1 ? "paced" : "", gw::lua_spike::kNoId, gw::lua_spike::kNoId);

    AIStats.LuaGC.End();
}

#ifdef DEBUG_PRECISE_PATH
void test_precise_path();
#endif

#ifdef DEBUG
extern Flags32 dbg_net_Draw_Flags;
#endif

void CLevel::RenderBulletTracersForMainViewport()
{
    BulletManager().Render();
}

void CLevel::RenderBulletTracersForSecondViewport()
{
    BulletManager().Render();
}

void CLevel::OnRender()
{
    ZoneScoped;

    GEnv.Render->BeforeWorldRender();

#ifdef DEBUG
    CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(Level().CurrentEntity());
    if (stalker)
        stalker->ShouldProcessOnRender(true);
#endif

    inherited::OnRender();
    if (!game)
        return;
    {
        ZoneScopedN("lvl_post3d");
        {
            ZoneScopedN("lvl_game_on_render");
            PERF_SCOPE(rlvl_game_on_render);
            Game().OnRender();
        }

        // Dedicated second VP: tracers are drawn in IGame_Level (main RT, then scope RT). Otherwise once here.
        {
            IMainMenu* pMainMenu = g_pGamePersistent ? g_pGamePersistent->m_pMainMenu : nullptr;
            const bool bMenu = pMainMenu && pMainMenu->CanSkipSceneRendering();
            const bool tracers_done_in_level = Device.m_SecondViewport.IsSVPFrame() && !bMenu;
            if (!tracers_done_in_level)
            {
                ZoneScopedN("lvl_bullets");
                PERF_SCOPE(rlvl_bullets);
                BulletManager().Render();
            }
        }

        {
            ZoneScopedN("lvl_after_world");
            PERF_SCOPE(rlvl_after_world);
            GEnv.Render->AfterWorldRender(); //--#SM+#-- +SecondVP+
        }
        {
            ZoneScopedN("lvl_hud_overlay");
            PERF_SCOPE(rlvl_hud_overlay);
            // HUD overlay scope (g_3d_scopes 3): snapshot the live HUD into $user$hud_overlay before UI.
            GEnv.Render->RenderHudOverlayToTexture();
            // Blend the snapshot over the backbuffer (GL native path; explicit stencil/blend/depth state).
            // Drawn here — after the overlay RT is filled, before HUD().RenderUI() so indicators/reticle
            // render on top of the composited HUD.
            GEnv.Render->CompositeHudOverlay();
        }
        WorldRendered(true);
    }

    // GAME_RENDER_OVERLAP_PLAN P1 (game/render overlap frame): the join point. The 3D world (incl. scope pass,
    // tracers, HUD overlay) was rendered in parallel with the Game thread; the UI below is drawn after it -
    // custom statics, dialogs, news, hit marks and Lua-owned windows are changed by the game logic. No-op in a
    // sequential frame.
    Device.JoinGameLogicBeforeUI();

    {
        ZoneScopedN("lvl_RenderUI");
        PERF_SCOPE(rlvl_ui);
        GW_FRAME_PHASE(UiRender); // 50z5-extra-diagnostics.md: the "uir" phase of the frame report (out of "rs")
        if (!Device.IsAnselActive)
            HUD().RenderUI();
    }
    {
        ZoneScopedN("lvl_debug");
        debug_renderer().render();
#ifdef DEBUG
    // GAME_RENDER_OVERLAP_PLAN Phase 3, P3 "known/accepted risk" category (see physics_game.cpp's
    // TContactShotMark comment for the sibling case): physics_world()->OnRender() -> PH_DBG_Render()
    // reads physics debug-draw state (PHDebug.cpp) which the dedicated physics thread may be mid-Step()
    // writing. ph_dbg_draw_mask is opt-in/off by default, so skipping one frame of debug visualization
    // while overlap is active is an acceptable trade-off vs adding a lock to a DEBUG/MIXED-only path.
    // Note xray_build.props' "Mixed" config defines both MIXED and DEBUG, so this guard is live there
    // too, not just in pure Debug builds.
    if (!Device.IsFrameOverlapActive())
        physics_world()->OnRender();
#endif
#ifdef DEBUG
    if (ai().get_level_graph())
        levelGraphDebugRender->Render(ai().game_graph(), ai().level_graph());
#ifdef DEBUG_PRECISE_PATH
    test_precise_path();
#endif
    if (stalker)
    {
        stalker->OnRender();
        stalker->ShouldProcessOnRender(false);
    }
    if (bDebug)
    {
        for (u32 I = 0; I < Level().Objects.o_count(); I++)
        {
            IGameObject* _O = Level().Objects.o_get_by_iterator(I);
            CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(_O);
            if (stalker)
                stalker->OnRender();
            CCustomMonster* monster = smart_cast<CCustomMonster*>(_O);
            if (monster)
                monster->OnRender();
            CPhysicObject* physic_object = smart_cast<CPhysicObject*>(_O);
            if (physic_object)
                physic_object->OnRender();
            CSpaceRestrictor* space_restrictor = smart_cast<CSpaceRestrictor*>(_O);
            if (space_restrictor)
                space_restrictor->OnRender();
            CClimableObject* climable = smart_cast<CClimableObject*>(_O);
            if (climable)
                climable->OnRender();
            CTeamBaseZone* team_base_zone = smart_cast<CTeamBaseZone*>(_O);
            if (team_base_zone)
                team_base_zone->OnRender();
            if (GameID() != eGameIDSingle)
            {
                CInventoryItem* pIItem = smart_cast<CInventoryItem*>(_O);
                if (pIItem)
                    pIItem->OnRender();
            }
            if (dbg_net_Draw_Flags.test(dbg_draw_skeleton)) // draw skeleton
            {
                CGameObject* pGO = smart_cast<CGameObject*>(_O);
                if (pGO && pGO != Level().CurrentViewEntity() && !pGO->H_Parent())
                {
                    if (pGO->Position().distance_to_sqr(Device.vCameraPosition) < 400.0f)
                    {
                        pGO->dbg_DrawSkeleton();
                    }
                }
            }
        }
        //  [7/5/2005]
        if (Server && Server->GetGameState())
            Server->GetGameState()->OnRender();
        //  [7/5/2005]
        ObjectSpace.dbgRender();
        UI().Font().pFontStat->OutSet(170, 630);
        UI().Font().pFontStat->SetHeight(16.0f);
        UI().Font().pFontStat->SetColor(0xffff0000);
        if (Server)
            UI().Font().pFontStat->OutNext("Client Objects:      [%d]", Server->GetEntitiesNum());
        UI().Font().pFontStat->OutNext("Server Objects:      [%d]", Objects.o_count());
        UI().Font().pFontStat->OutNext("Interpolation Steps: [%d]", Level().GetInterpolationSteps());
        if (Server)
        {
            UI().Font().pFontStat->OutNext("Server updates size: [%d]", Server->GetLastUpdatesSize());
        }
        UI().Font().pFontStat->SetHeight(8.0f);
    }
#endif

#ifdef DEBUG
    if (bDebug)
    {
        DBG().draw_object_info();
        DBG().draw_text();
        DBG().draw_level_info();
    }
    debug_renderer().render();
    DBG().draw_debug_text();
    if (psAI_Flags.is(aiVision))
    {
        for (u32 I = 0; I < Level().Objects.o_count(); I++)
        {
            IGameObject* object = Objects.o_get_by_iterator(I);
            CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(object);
            if (!stalker)
                continue;
            stalker->dbg_draw_vision();
        }
    }

    if (psAI_Flags.test(aiDrawVisibilityRays))
    {
        for (u32 I = 0; I < Level().Objects.o_count(); I++)
        {
            IGameObject* object = Objects.o_get_by_iterator(I);
            CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(object);
            if (!stalker)
                continue;
            stalker->dbg_draw_visibility_rays();
        }
    }
#endif
    }
}

void CLevel::OnEvent(EVENT E, u64 P1, u64 /**P2**/)
{
    ZoneScoped;

    if (E == eEntitySpawn)
    {
        char Name[128];
        Name[0] = 0;
        sscanf(LPCSTR(P1), "%s", Name);
        Level().g_cl_Spawn(Name, 0xff, M_SPAWN_OBJECT_LOCAL, Fvector().set(0, 0, 0));
    }
    else if (E == eChangeRP && P1)
    {
    }
    else if (E == eDemoPlay && P1)
    {
        char* name = (char*)P1;
        string_path RealName;
        xr_strcpy(RealName, name);
        xr_strcat(RealName, ".xrdemo");
        Cameras().AddCamEffector(xr_new<CDemoPlay>(RealName, 1.3f, 0));
    }
    else if (E == eChangeTrack && P1)
    {
        // int id = atoi((char*)P1);
        // Environment->Music_Play(id);
    }
    else if (E == eEnvironment)
    {
        // int id=0; float s=1;
        // sscanf((char*)P1,"%d,%f",&id,&s);
        // Environment->set_EnvMode(id,s);
    }
}

void CLevel::DumpStatistics(IGameFont& font, IPerformanceAlert* alert)
{
    inherited::DumpStatistics(font, alert);
    stats.FrameEnd();
    font.OutNext("Client:");
    font.OutNext("- receive:    %2.2fms, %d", stats.ClientRecv.result, stats.ClientRecv.count);
    font.OutNext("- send:       %2.2fms, %d", stats.ClientSend.result, stats.ClientSend.count);
    font.OutNext("- compress:   %2.2fms", stats.ClientCompressor.result);
    font.OutNext("- int send:   %2.2fms, %d", stats.ClientSendInternal.result, stats.ClientSendInternal.count);
    font.OutNext("- bmcommit:   %2.2fms, %d", stats.BulletManagerCommit.result, stats.BulletManagerCommit.count);
    stats.FrameStart();
    if (Server)
        Server->DumpStatistics(font, alert);
    AIStats.FrameEnd();
    font.OutNext("AI think:     %2.2fms, %d", AIStats.Think.result, AIStats.Think.count);
    font.OutNext("- range:      %2.2fms, %d", AIStats.Range.result, AIStats.Range.count);
    font.OutNext("- path:       %2.2fms, %d", AIStats.Path.result, AIStats.Path.count);
    font.OutNext("- node:       %2.2fms, %d", AIStats.Node.result, AIStats.Node.count);
    font.OutNext("AI vision:    %2.2fms, %d", AIStats.Vis.result, AIStats.Vis.count);
    font.OutNext("- query:      %2.2fms", AIStats.VisQuery.result);
    font.OutNext("- rayCast:    %2.2fms", AIStats.VisRayTests.result);
    font.OutNext("LUA GC:       %d Kb, %2.2fms", lua_gc(GEnv.ScriptEngine->lua(), LUA_GCCOUNT, 0), AIStats.LuaGC.result);
    AIStats.FrameStart();
}

void CLevel::AddObject_To_Objects4CrPr(CGameObject* pObj)
{
    if (!pObj)
        return;
    for (CGameObject* obj : pObjects4CrPr)
    {
        if (obj == pObj)
            return;
    }
    pObjects4CrPr.push_back(pObj);
}
void CLevel::AddActor_To_Actors4CrPr(CGameObject* pActor)
{
    if (!pActor)
        return;
    if (!smart_cast<CActor*>(pActor))
        return;
    for (CGameObject* act : pActors4CrPr)
    {
        if (act == pActor)
            return;
    }
    pActors4CrPr.push_back(pActor);
}

void CLevel::RemoveObject_From_4CrPr(CGameObject* pObj)
{
    if (!pObj)
        return;
    auto objIt = std::find(pObjects4CrPr.begin(), pObjects4CrPr.end(), pObj);
    if (objIt != pObjects4CrPr.end())
    {
        pObjects4CrPr.erase(objIt);
    }
    auto aIt = std::find(pActors4CrPr.begin(), pActors4CrPr.end(), pObj);
    if (aIt != pActors4CrPr.end())
    {
        pActors4CrPr.erase(aIt);
    }
}

void CLevel::make_NetCorrectionPrediction()
{
    ZoneScoped;

    m_bNeed_CrPr = false;
    m_bIn_CrPr = true;
    u64 NumPhSteps = physics_world()->StepsNum();
    physics_world()->StepsNum() -= m_dwNumSteps;
    if (ph_console::g_bDebugDumpPhysicsStep && m_dwNumSteps > 10)
    {
        Msg("!!! TOO MANY PHYSICS STEPS FOR CORRECTION PREDICTION = %d !!!", m_dwNumSteps);
        m_dwNumSteps = 10;
    }
    physics_world()->Freeze();
    // setting UpdateData and determining number of PH steps from last received update
    for (CGameObject* obj : pObjects4CrPr)
    {
        if (!obj)
            continue;
        obj->PH_B_CrPr();
    }
    // first prediction from "delivered" to "real current" position
    // making enought PH steps to calculate current objects position based on their updated state
    for (u32 i = 0; i < m_dwNumSteps; i++)
    {
        physics_world()->Step();

        for (CGameObject* act : pActors4CrPr)
        {
            if (!act || act->CrPr_IsActivated())
                continue;
            act->PH_B_CrPr();
        }
    }
    for (CGameObject* obj : pObjects4CrPr)
    {
        if (!obj)
            continue;
        obj->PH_I_CrPr();
    }
    if (!InterpolationDisabled())
    {
        for (u32 i = 0; i < lvInterpSteps; i++) // second prediction "real current" to "future" position
        {
            physics_world()->Step();
        }
        for (CGameObject* obj : pObjects4CrPr)
        {
            if (!obj)
                continue;
            obj->PH_A_CrPr();
        }
    }
    physics_world()->UnFreeze();
    physics_world()->StepsNum() = NumPhSteps;
    m_dwNumSteps = 0;
    m_bIn_CrPr = false;
    pObjects4CrPr.clear();
    pActors4CrPr.clear();
}

u32 CLevel::GetInterpolationSteps() { return lvInterpSteps; }
void CLevel::UpdateDeltaUpd(u32 LastTime)
{
    u32 CurrentDelta = LastTime - m_dwLastNetUpdateTime;
    if (CurrentDelta < m_dwDeltaUpdate)
        CurrentDelta = iFloor(float(m_dwDeltaUpdate * 10 + CurrentDelta) / 11);
    m_dwLastNetUpdateTime = LastTime;
    m_dwDeltaUpdate = CurrentDelta;
    if (0 == g_cl_lvInterp)
        ReculcInterpolationSteps();
    else if (g_cl_lvInterp > 0)
    {
        lvInterpSteps = iCeil(g_cl_lvInterp / fixed_step);
    }
}

void CLevel::ReculcInterpolationSteps()
{
    lvInterpSteps = iFloor(float(m_dwDeltaUpdate) / (fixed_step * 1000));
    if (lvInterpSteps > 60)
        lvInterpSteps = 60;
    if (lvInterpSteps < 3)
        lvInterpSteps = 3;
}

bool CLevel::InterpolationDisabled() { return g_cl_lvInterp < 0; }
void CLevel::PhisStepsCallback(u32 Time0, u32 Time1)
{
    if (!Level().game)
        return;
    if (GameID() == eGameIDSingle)
        return;
    //#pragma todo("Oles to all: highly inefficient and slow!!!")
    // fixed (Andy)
    /*
    for (xr_vector<IGameObject*>::iterator O=Level().Objects.objects.begin(); O!=Level().Objects.objects.end(); ++O)
    {
    if( smart_cast<CActor*>((*O)){
    CActor* pActor = smart_cast<CActor*>(*O);
    if (!pActor || pActor->Remote()) continue;
    pActor->UpdatePosStack(Time0, Time1);
    }
    };
    */
}

void CLevel::SetNumCrSteps(u32 NumSteps)
{
    m_bNeed_CrPr = true;
    if (m_dwNumSteps > NumSteps)
        return;
    m_dwNumSteps = NumSteps;
    if (m_dwNumSteps > 1000000)
    {
        VERIFY(0);
    }
}

ALife::_TIME_ID CLevel::GetStartGameTime() { return (game->GetStartGameTime()); }

ALife::_TIME_ID CLevel::GetGameTime() { return (game->GetGameTime()); }

ALife::_TIME_ID CLevel::GetEnvironmentGameTime() const
{
    if (!game)
        return 0;
    return (game->GetEnvironmentGameTime());
}

u8 CLevel::GetDayTime()
{
    u32 dummy32, hours;
    GetGameDateTime(dummy32, dummy32, dummy32, hours, dummy32, dummy32, dummy32);
    VERIFY(hours < 256);
    return u8(hours);
}

float CLevel::GetGameDayTimeSec() { return (float(s64(GetGameTime() % (24 * 60 * 60 * 1000))) / 1000.f); }

u32 CLevel::GetGameDayTimeMS() { return (u32(s64(GetGameTime() % (24 * 60 * 60 * 1000)))); }

float CLevel::GetEnvironmentGameDayTimeSec() const
{
    return (float(s64(GetEnvironmentGameTime() % (24 * 60 * 60 * 1000))) / 1000.f);
}

void CLevel::GetGameDateTime(u32& year, u32& month, u32& day, u32& hours, u32& mins, u32& secs, u32& milisecs)
{
    split_time(GetGameTime(), year, month, day, hours, mins, secs, milisecs);
}

float CLevel::GetGameTimeFactor()
{
    return game->GetGameTimeFactor();
}

void CLevel::SetGameTimeFactor(const float fTimeFactor)
{
    game->SetGameTimeFactor(fTimeFactor);
}

void CLevel::SetGameTimeFactor(ALife::_TIME_ID GameTime, const float fTimeFactor)
{
    game->SetGameTimeFactor(GameTime, fTimeFactor);
}

float CLevel::GetEnvironmentTimeFactor() const
{
    if (!game)
        return 0.0f;
    return game->GetEnvironmentGameTimeFactor();
}

void CLevel::SetEnvironmentTimeFactor(const float fTimeFactor)
{
    if (!game)
        return;
    game->SetEnvironmentGameTimeFactor(fTimeFactor);
}

void CLevel::SetEnvironmentGameTimeFactor(u64 const& GameTime, float const& fTimeFactor)
{
    if (!game)
        return;
    game->SetEnvironmentGameTimeFactor(GameTime, fTimeFactor);
}

bool CLevel::IsServer()
{
    if (!Server || IsDemoPlayStarted())
        return false;
    return true;
}

bool CLevel::IsClient()
{
    if (IsDemoPlayStarted())
        return true;
    if (Server)
        return false;
    return true;
}

void CLevel::OnAlifeSimulatorUnLoaded()
{
    MapManager().ResetStorage();
    GameTaskManager().ResetStorage();
}

void CLevel::OnAlifeSimulatorLoaded()
{
    MapManager().ResetStorage();
    GameTaskManager().ResetStorage();
}

void CLevel::OnSessionTerminate(pcstr reason) { MainMenu()->OnSessionTerminate(reason); }
u32 GameID() { return Game().Type(); }
CZoneList* CLevel::create_hud_zones_list()
{
    hud_zones_list = xr_new<CZoneList>();
    hud_zones_list->clear();
    return hud_zones_list;
}

bool CZoneList::feel_touch_contact(IGameObject* O)
{
    TypesMapIt it = m_TypesMap.find(O->cNameSect());
    bool res = (it != m_TypesMap.end());
    CCustomZone* pZone = smart_cast<CCustomZone*>(O);
    if (pZone && !pZone->IsEnabled())
    {
        res = false;
    }
    return res;
}

CZoneList::CZoneList() {}
CZoneList::~CZoneList()
{
    clear();
    destroy();
}
