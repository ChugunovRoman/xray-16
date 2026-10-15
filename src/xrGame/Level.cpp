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
#include "ParticlesObject.h"
#include "Level.h"
#include "VisionBatch.h"
#include "Include/xrRender/Kinematics.h"
#include "inventory_item.h" // render prepare of held items: CLevel::BonesPrepass collect step
#include "HudItem.h"
#include "xrCore/Threading/ParallelFor.hpp"
#include "xrCore/Profiling/PerfMetrics.hpp"
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
}

CLevel::~CLevel()
{
    ZoneScoped;

    Device.PostSchedulerVisionBatch = nullptr;
    Device.BonesPrepassCallback = nullptr;

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
// per frame. Temporary console variables, not saved (console_commands.cpp); spawn_budget_us 0 = as before: the whole
// queue in one frame.
int g_spawn_budget_us = 5000; // time of one pass of ProcessGameEvents (M_EVENT count too) after which no group starts
int g_spawn_min_per_frame = 2; // groups materialized in every frame whatever the budget: the queue always moves
int g_spawn_max_delay_ms = 500; // a head that waits longer goes on past the budget (player actions, quest items)...
// ...but only up to spawn_catchup_us per pass: a wave of 1000+ spawns (session 2026-10-12) otherwise went in one frame
// once its head was 500 ms old (forced/delay 1431 groups, 740 ms frame); the backlog now drains at this rate.
// 0 = no limit (the old behaviour).
int g_spawn_catchup_us = 20000;

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

void CLevel::ProcessGameEvents(bool frame_budget)
{
    ZoneScoped;

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

        while (game_events->available(svT))
        {
            u16 spawn_id = u16(-1);
            {
                const NET_Event& head = game_events->front();
                if (head.ID == M_SPAWN)
                {
                    const bool joins_group = spawn_joins_group(group, head.spawn_parent);
                    // Stage 2: the budget is checked in front of a root only, a group is never split by it
                    if (!joins_group && SpawnBudgetSpent(budget, head))
                        break; // this group and everything behind it go on in the next frame, in the same order
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
xr_vector<IKinematics*> s_bones_prepass_phase_b;
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
// did), and the shot light moves in the spatial DB.
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
        hud_item->render_prepare();
        ++prepared;
    }
    PERF_ADD(prepare_items, prepared);
}
} // namespace

void CLevel::BonesPrepassNote(u16 id)
{
    const u32 slot = s_bones_prepass_noted_count.fetch_add(1, std::memory_order_relaxed);
    if (slot < bones_prepass_note_capacity)
        s_bones_prepass_noted[slot] = id;
}

void CLevel::BonesPrepass()
{
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
    auto& phase_b = s_bones_prepass_phase_b;
    candidates.clear();
    phase_b.clear();
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
            // The render doesn't compute the bones of a frozen corpse pose (renderable_CorpsePoseFrozen) - neither do we.
            if (O->renderable_CorpsePoseFrozen())
                continue;
            IKinematics* K = PKinematics(O->Visual());
            // Only the bones the render really computed last frame (not the LOD-blob / skipped shadow casters) and not
            // a model the prepass recently missed on.
            if (!K || !K->BonesPrepass_Wanted())
                continue;
            candidates.push_back(K);
        }
        PERF_ADD(prepass_candidates, candidates.size());
        // Phase A on main for every candidate: the blend callbacks (animation ends, Lua ScriptCallBack) fire here, on
        // the game logic thread, while nothing else runs game code (sequential frame: GameThread joined; game/render
        // overlap frame: the Game thread is not woken yet) - instead of on render workers.
        for (IKinematics* K : candidates)
        {
            if (K->BonesPrepass_PhaseA())
                phase_b.push_back(K);
        }
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
        xr_parallel_for(TaskRange<u32>(0, count, grain), true, [&](const TaskRange<u32>& range)
        {
            ZoneScopedN("BonesPrepass/phase_b_task");
            PERF_PASS(xray::perf::pass::Prepass);
            PERF_SCOPE(prepass_phase_b);
            u32 local = 0;
            for (u32 i = range.begin(); i != range.end(); ++i)
            {
                if (phase_b[i]->BonesPrepass_PhaseB())
                    ++local;
            }
            computed.fetch_add(local, std::memory_order_relaxed);
        });
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
            MapManager().Update();
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
            GameTaskManager().UpdateTasks();
        }
    }
    // Inherited update
    {
        // objects UpdateCL + destroy + HUD: no spike phase (tens of ms is normal), each object UpdateCL is a spike site
        PERF_SCOPE(lvl_frame_objects);
        inherited::OnFrame();
    }
    if (!Device.Paused())
    {
        // 06-framemove-offload.md, B4: level_on_frame always goes to the game logic thread, OnGameLogicFrame of this
        // frame (after UpdateCL, before the scheduler; in a game/render overlap frame parallel to the render)
        m_level_on_frame_pending = true;
        // Plugin timers (timer_start) stay here, on main: a timer may send any event, to any subscriber
        LEVEL_FRAME_PHASE("level/plugin_timers", lvl_frame_plugin_timers);
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
        GEnv.ScriptEngine->script_process(ScriptProcessor::Level)->update();
    }
    {
        LEVEL_FRAME_PHASE("level/ph_commander", lvl_frame_ph_commander);
        m_ph_commander->update();
    }
    {
        LEVEL_FRAME_PHASE("level/add_call", lvl_frame_add_call);
        m_ph_commander_scripts->update();
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
            Device.seqParallel.push_back(fastdelegate::FastDelegate0<>(this, &CLevel::script_gc));
        }
        else
            script_gc();
    }
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
}

// plans/optimization_mt/22-game-thread.md, step 2: the sound delegates of a frame (SoundEvent_Register queues one per
// listener in range, ~500 per sound in a crowd) cost ~1.2 us each in feel_sound_new, and every NPC that heard
// something also runs its hear callbacks (npc_on_hear_callback emits) in the next OnGameLogicFrame.
// CLevel::SoundEvent_Filter below caps and dedupes them, always (2026-10-14: the developer accepts that cut far
// listeners lose npc_on_hear_callback of such a sound). The tunables are cvars (console_commands.cpp).
// A listener closer to the sound than this always gets it: gw_npc and xr_danger arm the script danger only within 30 m
// of the sound (npc_on_hear_callback, dist_sqr < 900), plus a margin for the listener moving until its callbacks run
float npc_perf_sound_dispatch_near_dist = 35.f;
// A listener closer to the actor than this always gets every sound: what the player can see reacts as before
float npc_perf_sound_dispatch_actor_dist = 50.f;
// Beyond those, a sound goes to at most this many of the other listeners, the loudest ones (power with occlusion):
// combat sounds (shot, bullet hit, explosion, monster attack / injury / death) and the rest
int npc_perf_sound_dispatch_max_combat = 64;
int npc_perf_sound_dispatch_max = 32;

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

// Scratch of CLevel::SoundEvent_Filter, kept between frames (no allocation per frame); game logic thread only
xr_vector<SoundDispatchRun> s_sound_dispatch_runs;
xr_vector<SoundDispatchCandidate> s_sound_dispatch_candidates;
xr_vector<SoundDispatchCopy> s_sound_dispatch_copies;
} // namespace

// The game logic thread, from SoundEvent_Dispatch before any delegate goes out. A delegate is dropped by setting its
// dest to null; the rest keep their order.
// - Cap: a sound goes to every listener within npc_perf_sound_dispatch_near_dist of it, within
//   npc_perf_sound_dispatch_actor_dist of the actor, or with the Lua sound callback set (an xr_hear on_sound section),
//   and to at most npc_perf_sound_dispatch_max[_combat] of the other listeners, the loudest. The sounds of the actor
//   and of what it holds or shot (steps, shots, items, bullet hits and whines: their object is the actor) are not
//   capped at all - stealth and the reaction to the player stay as before.
// - Dedupe: the same source and sound type reaches a listener once a frame, the loudest delegate (of equal ones the
//   later). CSoundMemoryManager raises its threshold to each sound it hears and CScriptEntity::sound_callback merges
//   the same source and type into one saved sound, so the quieter copy changes nothing there.
void CLevel::SoundEvent_Filter(xr_vector<_esound_delegate>& events)
{
    if (events.empty())
        return;
    ZoneScoped;
    PERF_SCOPE(npc_sound_filter);

    const IGameObject* actor = CurrentEntity();
    const float near_dist = npc_perf_sound_dispatch_near_dist;
    const float actor_dist_sqr = _sqr(npc_perf_sound_dispatch_actor_dist);
    u32 capped = 0;
    u32 deduped = 0;

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
    for (const SoundDispatchRun& run : runs)
    {
        if (actor && run.source && (run.source == actor || run.source->H_Root() == actor))
            continue;
        // A grenade or a charge of the actor: no parent any more after the throw, its initiator tells whose it is
        if (actor && run.source)
        {
            // const_cast: smart_cast and CExplosive::Initiator() are non-const, the object is only read
            CExplosive* explosive = smart_cast<CExplosive*>(const_cast<IGameObject*>(run.source));
            if (explosive && explosive->Initiator() == actor->ID())
                continue;
        }
        const u32 limit = static_cast<u32>(
            SoundDispatchIsCombat(run.type) ? npc_perf_sound_dispatch_max_combat : npc_perf_sound_dispatch_max);
        if (run.end - run.begin <= limit)
            continue;
        candidates.clear();
        for (u32 i = run.begin; i < run.end; ++i)
        {
            const _esound_delegate& entry = events[i];
            if (entry.dist <= near_dist)
                continue;
            if (actor && entry.dest_object->Position().distance_to_sqr(actor->Position()) <= actor_dist_sqr)
                continue;
            candidates.push_back({ entry.power, i });
        }
        if (candidates.size() <= limit)
            continue;
        std::nth_element(candidates.begin(), candidates.begin() + limit, candidates.end(),
            [](const SoundDispatchCandidate& a, const SoundDispatchCandidate& b) { return a.power > b.power; });
        for (auto it = candidates.begin() + limit; it != candidates.end(); ++it)
        {
            _esound_delegate& entry = events[it->index];
            // xr_hear.reset_hear_callback sets the Lua sound callback for a section with on_sound only: its condlists
            // react to sounds at the distance and power they name, so such a listener keeps every sound
            CCustomMonster* monster = entry.dest_object->cast_custom_monster();
            if (monster && monster->callback(GameObject::eSound))
                continue;
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

    if (capped || deduped)
    {
        events.erase(std::remove_if(events.begin(), events.end(),
                         [](const _esound_delegate& entry) { return !entry.dest; }),
            events.end());
    }
    PERF_ADD(npc_sound_capped, capped);
    PERF_ADD(npc_sound_deduped, deduped);
}

int psLUA_GCSTEP = 100; // 10
int psLUA_GCTIMEOUT = 1000;

u32 ps_lua_gc_method = 1;

void CLevel::script_gc()
{
    ZoneScoped;
    AIStats.LuaGC.Begin();
    lua_State* L = GEnv.ScriptEngine ? GEnv.ScriptEngine->lua() : nullptr;
    if (L)
        xray::perf::SetGauge(xray::perf::Metric::lua_gc_kb, lua_gc(L, LUA_GCCOUNT, 0));
    PERF_SCOPE(lua_gc_step);
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
        // The paced steps (lua_spike_probe.h): the GC of the frame works only here, psLUA_GCSTEP KB on top
        gw::lua_spike::GcStepFrame(GEnv.ScriptEngine->lua(), psLUA_GCSTEP);
        break;
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
            const bool tracers_done_in_level =
                ps_r__dedicated_second_vp && Device.m_SecondViewport.IsSVPFrame() && !bMenu;
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
