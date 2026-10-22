#include "pch_script.h"
#include "xrEngine/XR_IOConsole.h"
#include "xrEngine/xr_ioc_cmd.h"
#include "xrEngine/CustomHUD.h"
#include "xrEngine/FDemoRecord.h"
#include "xrEngine/FDemoPlay.h"
#include "xrCore/Profiling/PerfMetrics.hpp" // 50n: xray::perf::FrameWindowReport
#include "xrCore/Profiling/SysDiag.hpp" // 50z5: the [load] timeline
#include "xrMessages.h"
#include "xrServer.h"
#include "Level.h"
#include "lua_spike_probe.h"
#include "lua_udata_profile.h"
#include "xrScriptEngine/script_debugger.hpp"
#include "ai_debug.h"
#include "alife_simulator.h"
#include "game_cl_base.h"
#include "game_cl_single.h"
#include "game_sv_single.h"
#include "Hit.h"
#include "PHDestroyable.h"
#include "Actor.h"
#include "Actor_Flags.h"
#include "CustomZone.h"
#include "xrScriptEngine/script_engine.hpp"
#include "xrScriptEngine/script_profiler.hpp"
#include "xrScriptEngine/script_process.hpp"
#include "xrServer_Objects.h"
#include "ui/UIMainIngameWnd.h"
#include "xrPhysics/IPHWorld.h"
#include "autosave_manager.h"
#include "ai_space.h"
#include "addon_host.h"
#include "addon_event_bus.h"
#include "addon_object_events.h"
#include "addon_data_bus.h"
#include "addon_timers.h"
#include "addon_api_ini.h"
#include "addon_api_console.h"
#include "addon_binders.h"
#include "addon_callbacks.h"
#include "addon_goap.h"
#include "addon_storage.h"
#include "ai/monsters/basemonster/base_monster.h"
#include "date_time.h"
#include "mt_config.h"
#include "ui/UIOptConCom.h"
#include "UIGameSP.h"
#include "ui/UIActorMenu.h"
#include "xrUICore/Static/UIStatic.h"
#include "xrUICore/ui_styles.h"
#include "zone_effector.h"
#include "GameTask.h"
#include "MainMenu.h"
#include "saved_game_wrapper.h"
#include "xrAICore/Components/ai_planner_search_limits.h"
#include "xrAICore/Navigation/ai_graph_engine_cvars.h"
#include "xrAICore/Navigation/level_graph.h"
#include "xrNetServer/NET_Messages.h"
#include "character_community.h"
#include "CustomOutfit.h"

#include "CameraLook.h"
#include "xrCommon/xr_map.h"
#include "character_hit_animations_params.h"
#include "inventory_upgrade_manager.h"

#include "xrGameSpy/GameSpy_Full.h"

#include "ai_debug_variables.h"
#include "xrPhysics/console_vars.h"
#include "GametaskManager.h"
#include "performance_cvars.h"
#include "xrEngine/Feel_Vision.h"
#include "map_spot_cvars.h"
#include "weapon_inv_icon.h"
#include "alife_object_registry.h"
#include "xrCore/Platform/HwProfile.h"

#ifdef DEBUG
#include "PHDebug.h"
#include "ui/UIDebugFonts.h"
#include "xrAICore/Navigation/game_graph.h"
#include "LevelGraphDebugRender.hpp"
#include "CharacterPhysicsSupport.h"
#endif // DEBUG

string_path g_last_saved_game;

#ifdef DEBUG
extern float air_resistance_epsilon;
#endif // #ifdef DEBUG

extern void show_smart_cast_stats();
extern void clear_smart_cast_stats();
extern void release_smart_cast_stats();

extern u64 g_qwStartGameTime;
extern u64 g_qwEStartGameTime;

ENGINE_API
extern float psHUD_FOV_def; // --#SM+#-- [it's now used in hud_fov]
extern float psSqueezeVelocity;
extern int psLUA_GCSTEP;
extern int psLUA_GCTIMEOUT;
extern u32 ps_lua_gc_method;
extern int g_auto_ammo_unload;

extern int x_m_x;
extern int x_m_z;
extern BOOL net_cl_inputguaranteed;
extern BOOL net_sv_control_hit;
extern int g_dwInputUpdateDelta;
#ifdef DEBUG
extern BOOL g_ShowAnimationInfo;
#endif // DEBUG
extern BOOL g_bShowHitSectors;
// extern	BOOL	g_bDebugDumpPhysicsStep	;
extern ESingleGameDifficulty g_SingleGameDifficulty;
//-----------------------------------------------------------
extern float g_fTimeFactor;
extern BOOL b_toggle_weapon_aim;

extern float g_smart_cover_factor;
extern int g_upgrades_log;
extern float g_smart_cover_animation_speed_factor;

// Ф1 (PHYSICS_ACTIVE_SET_PLAN): defined in movement_manager_physic.cpp
extern float ph_npc_kinematic_far_dist;
extern float ph_npc_kinematic_far_hyst;
extern float ph_npc_kinematic_unseen_dist;
// Live NPCs left high in the air (plans/optimization_mt/21-physics-step.md, 2026-10-15): movement_manager_physic.cpp
extern float ai_npc_ground_snap_height;
extern float ai_npc_ground_snap_speed;
// Spawn stage 2 (plans/spawn_optimization/SPEC.md 5.A): the budget of the client materialization of M_SPAWN, Level.cpp
extern int g_spawn_budget_us;
extern int g_spawn_min_per_frame;
extern int g_spawn_max_delay_ms;
extern int g_spawn_catchup_us;
// 36-spawn-model-load.md (plans/optimization_mt): Level.cpp, alife_switch_manager.cpp
extern int g_spawn_cold_models;
extern int g_model_prefetch_margin;
// plans/optimization_mt/22-game-thread.md, step 2: the cap and dedupe of the sound delegates, Level.cpp
extern float npc_perf_sound_dispatch_near_dist;
extern float npc_perf_sound_dispatch_actor_dist;
extern int npc_perf_sound_dispatch_max_combat;
extern int npc_perf_sound_dispatch_max;
extern float npc_sound_noncombat_dist; // 24-critical-path.md, G3 (Level.cpp)
extern int npc_sound_noncombat_per_listener;

extern BOOL g_ai_use_old_vision;
float g_aim_predict_time = 0.40f;
int g_keypress_on_start = 1;
extern int ps_force_character_lod_render;
extern int ps_r_disable_character_lod_render;
extern u32 ps_r_corpse_pose_freeze_ms;

float g_aim_z_offset_coff = -0.04f;
float g_second_aim_z_offset_coff = -0.09f;
extern BOOL debug_show_second_wpn_model;
extern BOOL debug_show_thrid_wpn_model;
extern BOOL debug_show_attachments_slots;
extern BOOL debug_ui_item_cell;

ENGINE_API extern float g_console_sensitive;

//Alundaio
extern BOOL g_ai_die_in_anomaly;
int g_inv_highlight_equipped = 0;
int g_inv_inv_cell_size = 3;
int g_3d_scope_type = 1; // 0 = off, 1 = PiP (lens zoom), 2 = HUD overlay (no 2nd world render)
xr_map<shared_str, float> g_scope_hud_fov_presets;
xr_map<shared_str, float> g_weapon_hud_fov_presets;
//-Alundaio

int g_first_person_death = 0;
int g_normalize_mouse_sens = 0;
int g_normalize_upgrade_mouse_sens = 0;

int g_quick_unload_upgraded = 1;
int g_quick_unload_new = 0;
int g_quick_unload_scopes = 0;
int g_quick_unload_gl = 0;
int g_quick_unload_silencers = 0;
int g_outfit_faction = 1;
int g_enhancend_anims = 1;

void register_mp_console_commands();
//-----------------------------------------------------------

BOOL g_bCheckTime = FALSE;
int net_cl_inputupdaterate = 50;
Flags32 g_mt_config = {mtLevelPath | mtDetailPath | mtObjectHandler | mtSoundPlayer | mtBullets | mtLUA_GC |
    mtLevelSounds | mtALife | mtMap};

Flags32 g_map_spot_config = {MAP_SPOT_SMOOTH};
float ps_map_spot_smooth_speed = 0.6f;

#ifdef DEBUG
Flags32 dbg_net_Draw_Flags{};
#endif

#ifdef DEBUG
BOOL g_bDebugNode = FALSE;
u32 g_dwDebugNodeSource = 0;
u32 g_dwDebugNodeDest = 0;
extern BOOL g_bDrawBulletHit;
extern BOOL g_bDrawFirstBulletCrosshair;

float debug_on_frame_gather_stats_frequency = 0.f;
#endif
#ifdef DEBUG
extern pstr dbg_stalker_death_anim;
extern BOOL b_death_anim_velocity;
extern BOOL death_anim_debug;
extern BOOL dbg_imotion_draw_skeleton;
extern BOOL dbg_imotion_draw_velocity;
extern BOOL dbg_imotion_collide_debug;
extern float dbg_imotion_draw_velocity_scale;
#endif
int g_AI_inactive_time = 0;

// Performance / NPC throttling defaults (overridable via user.ltx)
float npc_perf_planner_near_dist = 35.f;
float npc_perf_planner_medium_dist = 80.f;
u32 npc_perf_planner_solve_interval_near_idle_ms = 110;
u32 npc_perf_planner_solve_interval_medium_ms = 300;
u32 npc_perf_planner_solve_interval_far_ms = 750;
u32 npc_perf_planner_solve_interval_combat_near_ms = 280;
u32 npc_perf_planner_solve_interval_danger_only_ms = 0;
u32 npc_perf_planner_actuality_interval_combat_ms = 90;
u32 npc_perf_planner_actuality_interval_danger_ms = 120;
u32 npc_perf_planner_actuality_interval_near_idle_ms = 220;
u32 npc_perf_planner_graph_search_max_nodes = 1200;
u32 npc_perf_planner_graph_search_max_nodes_combat = 600;
u32 npc_perf_planner_graph_search_max_nodes_danger = 700;
int npc_perf_cover_best_max_evaluate = 32;
int npc_perf_cover_nearest_max_points = 128;
int npc_perf_cover_best_max_accessible = 48;
int npc_perf_cover_find_eval_budget_total = 64;
u32 g_npc_perf_cover_find_eval_budget_remaining = u32(-1);
u32 npc_perf_long_dead_skip_binder_ms = 5000;
float npc_perf_binder_far_dist = 300.f;
u32 npc_perf_binder_far_interval_ms = 250;
u32 npc_perf_binder_far_phases = 6;
u32 npc_perf_binder_near_interval_ms = 180;
u32 npc_perf_binder_far_throttle_ms = 450;
int npc_perf_stalker_vis_interval_near_ms = 0;
int npc_perf_stalker_vis_interval_medium_ms = 350;
int npc_perf_stalker_vis_interval_far_ms = 900;
int npc_perf_vision_parallel_batch_min_rays = 32;
float npc_perf_vision_small_physics_radius = 0.25f;
float npc_perf_monster_vis_near_dist = 35.f;
float npc_perf_monster_vis_medium_dist = 80.f;
u32 npc_perf_monster_vis_interval_medium_ms = 220;
u32 npc_perf_monster_vis_interval_far_ms = 320;
u32 npc_perf_ik_interval_near_idle_ms = 50;
u32 npc_perf_ik_interval_medium_ms = 200;
u32 npc_perf_ik_interval_far_ms = 450;
u32 npc_perf_ik_interval_disabled_ms = 30000;
// K10 of plans/optimization_mt/26-framemove-game-next.md: the tested values are the defaults (were 0 = IK of a fighting
// stalker every frame at any distance, on screen or not)
u32 npc_perf_ik_interval_enemy_selected_ms = 100;
int npc_perf_ik_foot_raypick_batch_min_rays = 128; // 12 -> 128 (2026-10-16): the batch wait on main stole the prepass phase B tasks (ik/update 13 -> 7.6 us)
int ai_evaluator_ttl_ms = 500;
u32 npc_perf_state_mgr_animstate_ttl_ms = 120;
u32 npc_perf_script_combat_ttl_ms = 100;
u32 npc_perf_evaluator_combat_enemy_cache_ttl_ms = 120;
u32 npc_perf_danger_eval_ttl_ms = 150;
u32 npc_perf_script_danger_eval_ttl_ms = 150;
u32 npc_perf_state_mgr_idle_combat_ttl_ms = 80;
u32 npc_perf_state_mgr_end_ttl_ms = 120;
u32 npc_perf_planner_update_min_interval_ms = 35;
u32 npc_perf_planner_update_min_interval_far_ms = 50;
u32 npc_perf_evaluator_abuse_ttl_ms = 120;
u32 npc_perf_gather_items_eval_ttl_ms = 80;
u32 npc_perf_state_mgr_movement_ttl_ms = 80;
u32 npc_perf_motivator_medium_interval = 800;
u32 npc_perf_motivator_far_interval = 2500;
u32 npc_perf_motivator_slow_near_interval = 1000;
u32 npc_perf_motivator_slow_medium_interval = 3500;
u32 npc_perf_motivator_slow_far_interval = 6000;
u32 npc_perf_motivator_switch_near_interval = 800;
u32 npc_perf_motivator_switch_medium_interval = 1500;
u32 npc_perf_motivator_switch_far_interval = 3500;
u32 npc_perf_motivator_callback_near_interval = 800;
u32 npc_perf_motivator_callback_medium_interval = 1500;
u32 npc_perf_motivator_callback_far_interval = 3500;
u32 npc_perf_motivator_dead_interval = 3000;
u32 npc_perf_motivator_native_gate = 0;
u32 npc_perf_sim_brain_actor_update_interval = 10000;
int npc_perf_agent_enemy_fill_squads_trace_members = 0;
int npc_perf_visible_value_native = 1;        // visual_memory_manager.cpp: get_visible_value in C++
// Stage D, W4-2: xr_logic.pick_section_from_condlist in the engine (gw_condlist_native.cpp);
// 0 = Lua, 1 = native, 2 = shadow (dry native pass first, Lua decides, mismatches counted)
int npc_perf_condlist_native = 0;
int npc_perf_ik_enemy_near_dist = 20;        // CharacterPhysicsSupport.cpp: the fight IK interval only this near (K10)
int npc_perf_ik_offscreen_interval_ms = 450; // CharacterPhysicsSupport.cpp: the IK interval of NPCs off screen (K10)
// Stage 3 (ai_stalker.cpp, CAI_Stalker::TrySkipUpdateCLLod, always on): UpdateCL every 2nd / 3rd frame by the
// camera distance
int npc_perf_ucl_lod_dist = 30;            // m: beyond it every 2nd frame
int npc_perf_ucl_lod_dist2 = 45;           // m: beyond it every 3rd frame (0 = no such tier)
int npc_perf_ucl_lod_max_delta_ms = 250;   // ms: the most time between two full UpdateCL of a skipped stalker
// 24-critical-path.md, F2 (ai_stalker.cpp, always on): the combat tier of the UpdateCL LOD - a full UpdateCL every N-th
// frame (0 / 1 = every frame)
int npc_ucl_lod_combat = 2;
// 22-game-thread.md, step 1 (ai_stalker.cpp): the object handler LOD of idle stalkers and the friendly-fire pick LOD of
// unseen firing ones (always on in single player); the tunables
int npc_perf_object_handler_lod_dist = 20;        // m: nearer to the camera - every UpdateCL
int npc_perf_object_handler_lod_every_seen = 3;   // on screen: every N-th UpdateCL
int npc_perf_object_handler_lod_every_unseen = 12; // off screen: every N-th UpdateCL
int npc_perf_object_handler_lod_max_ms = 200;     // ms: the most time between two runs of a skipped handler
int npc_perf_object_handler_lod_pick_ms = 100;    // ms: the age of a kept friendly-fire pick (0 = every frame)
// 24-critical-path.md, G2 (ai_stalker_fire.cpp, always on): the tunables of the friendly-fire pick cache of any firing
// stalker and of the cut of the rays behind the enemy
extern int npc_ff_pick_cache_ms;
extern int npc_ff_pick_cache_frames;
extern int npc_ff_pick_cache_actor_dist;
extern int npc_ff_pick_cut_behind;
// 22-game-thread.md, step 1 (ai_stalker.cpp): the scheduler interval in ms of settled corpses (always on)
int npc_perf_dead_shed_interval_ms = 5000;
// Stage 4 (movement_manager_physic.cpp): frames without TryPosition after a successful one; both K = 0 -> off
int npc_perf_move_try_skip_frames = 0;     // K of an NPC on screen and near the camera
int npc_perf_move_try_skip_frames_far = 0; // K of an NPC off screen or beyond npc_perf_move_try_skip_far_dist
int npc_perf_move_try_skip_far_dist = 30;  // m from the camera; 0 = only the visibility picks the far K
int npc_perf_move_try_skip_recover = 3;    // successes in a row after a failure before skipping again
int npc_perf_move_try_skip_actor_dist = 5; // m from the current entity: no skipping this near
// Stage D, W5-1: switches of the Lua scheme actions, read by the scripts (utils.perf_cvar_int); 0 = as before
int npc_perf_camper_lean = 0;             // xr_camper: move_mgr:continue() once per stretch of walking
int npc_perf_walker_camp_once = 0;        // xr_walker: register_npc once per stay in a camp
int npc_perf_scheme_condlist_ttl_ms = 0;  // xr_animpoint reach_movement, xr_cover anm: condlist answer kept (ms)
int npc_perf_move_mgr_fixes = 0;          // move_mgr: a look point without ret drops the ret of the previous one
// P17b (plans/optimization_mt/12-relcase.md), read by death_manager.script: the items a dead NPC must not keep are
// released together once per this many ms (one frame with a destroy instead of one per death); 0 = at once.
int npc_perf_death_release_batch_ms = 1000;
// R0 (plans/optimization_mt/32-frame-spikes.md, a tunable, not saved), read by death_manager.script: > 0 - at most this
// many queued releases per tick of the queue, the rest goes with the next ticks; 0 = 256 as before
int npc_perf_death_release_batch_max = 64; // the default since 2026-10-17
// 37-destroy-relcase-smooth.md: what a flush of the death_manager.script queue leaves (fewer than
// npc_perf_death_release_batch_max) waits npc_perf_death_release_batch_ms again instead of going with the next tick -
// always on since 2026-10-18 (npc_perf_death_release_coalesce removed, see plan 41).
// 2026-10-18 (plans/optimization_mt/41-cvar-cleanup.md): the Lua switches npc_perf_death_lua_lite (B3 of
// 30-game-events.md), npc_perf_smart_first_update_jitter (S1) and npc_perf_squad_pack_children (Q1 of 32-frame-spikes.md)
// are removed, the scripts always take the new path.
// S1 (plans/optimization_mt/32-frame-spikes.md, a tunable, not saved), read by sim_smart.script, 0 = as before: > 0 - at
// most one squad created by spawn_defense_squad / spawn_attack_squad of all the smarts per this many ms; a smart that
// finds the token taken leaves the spawn to its next update (its timers are not touched).
int npc_perf_smart_spawn_gap_ms = 500; // the default since 2026-10-17

// Animation LOD (always on): the animation update interval of far, non-combat NPCs (read in ai_stalker.cpp).
int npc_anim_lod_far_interval_ms = 120;

u32 npc_preview_scene_budget_per_frame = 1;
u32 npc_preview_disk_cache_warm_per_frame = 16;

Flags32 g_uCommonFlags;
enum E_COMMON_FLAGS
{
    flAiUseTorchDynamicLights = 1
};

const xr_token lua_gc_method_token[] =
{
    { "gc_disable", 0 },
    { "gc_step", 1 },
    { "gc_timeout", 2 },
    { "gc_full", 3 },
    { nullptr, -1 }
};

CUIOptConCom g_OptConCom;

// 50c-commit-growth-analysis.md (plans/optimization_mt): the [mem] lines - what holds the process commit; the fields
// show which part grows. At the end of stat_memory (the end of a load, before a save, before loading a save, the main
// menu) and after a level is destroyed (CGamePersistent::DestroyLevel). Not per frame: the model pool is counted under
// its lock.
// mi_collect(true) only after a level is destroyed (collect): there it gives 0.5-1.7 GB back to the OS in 50-150 ms of
// the unload; at the other points it returned ~0 for 2-36 ms (36 ms at opening the menu), so it is not called there.
// The commit before and after it is printed.
// The same lines once a minute in play, label "tick" (stat_memory_tick): the locks of the model pool, the motions and
// the textures only tried (a busy one: "busy" instead of its fields).
static std::atomic<u32> s_mem_line_time{0}; // Device.dwTimeContinual of the last [mem] lines (the ticks count from it)

static void stat_memory_lines(pcstr where, bool tick, bool collect_heap)
{
    constexpr u64 mb = 1024 * 1024;
    const u64 commit_before = Memory.mem_usage();
    string128 collect;
    if (!collect_heap)
        collect[0] = 0;
    else
    {
        CTimer collect_timer;
        collect_timer.Start();
        Memory.mem_collect(true);
        const float collect_ms = collect_timer.GetElapsed_sec() * 1000.f;
        const u64 commit_after = Memory.mem_usage();
        const s64 collect_delta_mb =
            (static_cast<s64>(commit_after) - static_cast<s64>(commit_before)) / static_cast<s64>(mb);
        xr_sprintf(collect, " (mi_collect %+lld MB, %.1f ms)", collect_delta_mb, collect_ms);
    }

    CTimer stats_timer;
    stats_timer.Start();
    u64 avail_phys = 0, commit_avail = 0;
    xray::hw::QueryMemoryNow(avail_phys, commit_avail);

    RenderMemoryStats rs;
    if (GEnv.Render)
        GEnv.Render->GetMemoryStats(rs, tick);
    size_t snd_sources = 0, snd_pcm_bytes = 0;
    if (GEnv.Sound)
        GEnv.Sound->memory_stats(snd_sources, snd_pcm_bytes);
    int lua_kb = 0;
    if (GEnv.ScriptEngine)
    {
        if (lua_State* L = GEnv.ScriptEngine->lua())
            lua_kb = lua_gc(L, LUA_GCCOUNT, 0);
    }
    const CALifeSimulator* alife = g_ai_space ? g_ai_space->get_alife() : nullptr;
    const u32 alife_objects = alife ? static_cast<u32>(alife->objects().objects().size()) : 0;
    const u32 client_objects = g_pGameLevel ? Level().Objects.o_count() : 0;
    const float stats_ms = stats_timer.GetElapsed_sec() * 1000.f;

    string128 vram;
    if (rs.vram_valid)
        xr_sprintf(vram, "vram local %llu/%llu MB, nonlocal %llu MB", rs.vram_local_usage / mb,
            rs.vram_local_budget / mb, rs.vram_nonlocal_usage / mb);
    else
        xr_strcpy(vram, "vram n/a");
    // pool / inst: the estimated bytes of the free / all instances (CModelPool::memory_counts); refs only when the
    // instance counts of the bases (the bytes are counted by them) differ from Registry
    string256 models;
    if (rs.models_valid)
    {
        string32 refs;
        if (rs.model_instance_refs != rs.model_instances)
            xr_sprintf(refs, ", refs %u", rs.model_instance_refs);
        else
            refs[0] = 0;
        xr_sprintf(models, "models %u bases (unref %u), pool %u (%llu MB), inst %u (%llu MB%s)", rs.model_bases,
            rs.model_bases_unref, rs.model_pool, rs.model_pool_bytes / mb, rs.model_instances,
            rs.model_instance_bytes / mb, refs);
    }
    else
        xr_strcpy(models, "models busy");
    string64 motions;
    if (rs.motions_valid)
        xr_sprintf(motions, "motions %u (unref %u)", rs.motions, rs.motions_unref);
    else
        xr_strcpy(motions, "motions busy");
    string64 tex;
    if (rs.tex_valid)
        xr_sprintf(tex, "tex %u (%llu MB)", rs.tex_count, rs.tex_bytes / mb);
    else
        xr_strcpy(tex, "tex busy");

    Msg("* [mem] %s: commit %llu MB%s, sys commit avail %llu MB, ram avail %llu MB, %s", where, commit_before / mb,
        collect, commit_avail / mb, avail_phys / mb, vram);
    Msg("* [mem] %s: %s, %s, host vb/ib %llu/%llu MB, %s, snd src %llu, pcm %llu MB, lua %d MB, objects alife %u "
        "client %u, time %u s (%.1f ms, models %.2f ms)",
        where, models, motions, rs.host_vb_bytes / mb, rs.host_ib_bytes / mb, tex, static_cast<u64>(snd_sources),
        static_cast<u64>(snd_pcm_bytes) / mb, lua_kb / 1024, alife_objects, client_objects,
        Device.dwTimeGlobal / 1000, stats_ms, rs.models_ms);
    // 50n-frame-spike-report.md: the "* [frame]" window lines (the frames since the previous [mem] lines), printed by
    // the next frame's xray::perf::Flush()
    xray::perf::FrameWindowReport(where);
    s_mem_line_time.store(Device.dwTimeContinual, std::memory_order_relaxed);
}

void stat_memory_line(pcstr where) { stat_memory_lines(where, false, false); }

// CGamePersistent::DestroyLevel: the lines after the unload, with mi_collect(true) - the memory the allocator kept after
// the level went back to the OS
void stat_memory_level_destroyed()
{
    stat_memory_lines("level destroyed", false, true);
    xray::perf::LoadMark("level_destroyed"); // 50z5-extra-diagnostics.md: the level deleted, its memory returned
}

// CGamePersistent::OnFrame (main, a level loaded and ready, not paused - the main menu has its own line): the [mem]
// lines once per mem_tick_interval_ms of Device.dwTimeContinual (real time without the time the app was inactive)
// since the last ones of any label
void stat_memory_tick()
{
    constexpr u32 mem_tick_interval_ms = 60 * 1000;
    // signed: a line of another thread (a save from Lua) may store a time a little ahead of this one
    const s32 since = static_cast<s32>(Device.dwTimeContinual - s_mem_line_time.load(std::memory_order_relaxed));
    if (since < static_cast<s32>(mem_tick_interval_ms))
        return;
    stat_memory_lines("tick", true, false);
}

// mem_line_where: the label of the [mem] lines, null = without them (the out of memory handler: they take locks)
static void full_memory_stats(pcstr mem_line_where)
{
    Memory.mem_compact();
    u64 m_base = 0, m_lmaps = 0;
    u32 c_base = 0, c_lmaps = 0;
    GEnv.Render->ResourcesGetMemoryUsage(m_base, c_base, m_lmaps, c_lmaps);
    log_vminfo();
    size_t _process_heap = ::Memory.mem_usage();
    const auto [_eco_strings_bytes, _eco_strings_count] = g_pStringContainer->stat_economy();
    int _eco_smem = (int)g_pSharedMemoryContainer->stat_economy();
    Msg("* [ render ]: textures[%llu K]", (m_base + m_lmaps) / 1024);
    Msg("* [ x-ray  ]: process heap[%u K]", _process_heap / 1024);
    Msg("* [ x-ray  ]: shared strings: memory[%ld K], count[%lu]", _eco_strings_bytes / 1024, _eco_strings_count);
    Msg("* [ x-ray  ]: shared memory[%ld K]", _eco_smem);
#ifdef FS_DEBUG
    Msg("* [ x-ray  ]: file mapping: memory[%d K], count[%d]", g_file_mapped_memory / 1024, g_file_mapped_count);
    dump_file_mappings();
#endif
    if (mem_line_where)
        stat_memory_line(mem_line_where);
}

static void full_memory_stats_oom() { full_memory_stats(nullptr); }

class CCC_MemStats : public IConsole_Command
{
public:
    CCC_MemStats(LPCSTR N) : IConsole_Command(N)
    {
        bEmptyArgsHandled = TRUE;
        xrDebug::SetOutOfMemoryCallback(full_memory_stats_oom);
    };
    // args: the label of the [mem] lines (the callers in xrGame pass where they are). No args: the end of a load
    // (IGame_Persistent::LoadEnd runs it before the level counts as loaded) or the console
    virtual void Execute(LPCSTR args)
    {
        pcstr where = args && args[0] ? args :
            (g_pGameLevel && g_pGamePersistent && !g_pGamePersistent->IsLoaded() ? "load end" : "console");
        full_memory_stats(where);
    }
};

class CCC_GameDifficulty : public CCC_Token
{
public:
    CCC_GameDifficulty(LPCSTR N) : CCC_Token(N, (u32*)&g_SingleGameDifficulty, difficulty_type_token){};
    virtual void Execute(LPCSTR args)
    {
        CCC_Token::Execute(args);
        if (g_pGameLevel && Level().game)
        {
            //#ifndef	DEBUG
            if (GameID() != eGameIDSingle)
            {
                Msg("For this game type difficulty level is disabled.");
                return;
            };
            //#endif

            game_cl_Single* game = smart_cast<game_cl_Single*>(Level().game);
            VERIFY(game);
            game->OnDifficultyChanged();
        }
    }
    virtual void Info(TInfo& I) { xr_strcpy(I, "game difficulty"); }
};

class CCC_GameLanguage : public CCC_Token
{
public:
    CCC_GameLanguage(pcstr N) : CCC_Token(N, (u32*)&CStringTable::LanguageID, nullptr) {}

    void Execute(pcstr args) override
    {
        CCC_Token::Execute(args);
        StringTable().ReloadLanguage();

        Device.seqUIReset.Process();

        if (!g_pGameLevel)
            return;

        for (u16 id = 0; id < 0xffff; id++)
        {
            IGameObject* gameObj = Level().Objects.net_Find(id);
            if (gameObj)
            {
                if (CInventoryItem* invItem = gameObj->cast_inventory_item())
                    invItem->ReloadNames();
            }
        }
    }

    const xr_token* GetToken() noexcept override
    {
        tokens = StringTable().GetLanguagesToken();
        if(!tokens) // Prevent failure without usage Nifty counters
        {
            Msg("GetToken: token missing");
            StringTable().Destroy();
            StringTable().Init();

            tokens = StringTable().GetLanguagesToken();
        }
        return CCC_Token::GetToken();
    }
};

#ifdef DEBUG
class CCC_ALifePath : public IConsole_Command
{
public:
    CCC_ALifePath(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        if (!ai().get_level_graph())
            Msg("! there is no graph!");
        else
        {
            int id1 = -1, id2 = -1;
            sscanf(args, "%d %d", &id1, &id2);
            if ((-1 != id1) && (-1 != id2))
                if (_max(id1, id2) > (int)ai().game_graph().header().vertex_count() - 1)
                    Msg("! there are only %d vertexes!", ai().game_graph().header().vertex_count());
                else if (_min(id1, id2) < 0)
                    Msg("! invalid vertex number (%d)!", _min(id1, id2));
                else
                {
                    //						Sleep				(1);
                    //						CTimer				timer;
                    //						timer.Start			();
                    //						float				fValue = ai().m_tpAStar->ffFindMinimalPath(id1,id2);
                    //						Msg					("* %7.2f[%d] : %11I64u cycles (%.3f
                    // microseconds)",fValue,ai().m_tpAStar->m_tpaNodes.size(),timer.GetElapsed_ticks(),timer.GetElapsed_ms()*1000.f);
                }
            else
                Msg("! not enough parameters!");
        }
    }
};
#endif // DEBUG

class CCC_U32 : public IConsole_Command
{
protected:
    unsigned* value;
    unsigned min, max;

public:
    const unsigned GetValue() const { return *value; };
    void GetBounds(u32& imin, u32& imax) const
    {
        imin = min;
        imax = max;
    }
    CCC_U32(const char* N, unsigned* V, unsigned _min = 0, unsigned _max = 999)
        : IConsole_Command(N), value(V), min(_min), max(_max) {};
    virtual void Execute(const char* args)
    {
        int v = atoi(args);
        if (v < min || v > max)
            InvalidSyntax();
        else
            *value = v;
    }
    virtual void Status(TStatus& S) { itoa(*value, S, 10); }
    virtual void Info(TInfo& I) { xr_sprintf(I, sizeof(I), "integer value in range [%d,%d]", min, max); }
    virtual void fill_tips(vecTips& tips, u32 mode)
    {
        TStatus str;
        xr_sprintf(str, sizeof(str), "%d  (current)  [%d,%d]", *value, min, max);
        tips.push_back(str);
        IConsole_Command::fill_tips(tips, mode);
    }
};

class CCC_ALifeTimeFactor : public IConsole_Command
{
public:
    CCC_ALifeTimeFactor(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        float id1 = 0.0f;
        sscanf(args, "%f", &id1);
        if (id1 < EPS_L)
            Msg("Invalid time factor! (%.4f)", id1);
        else
        {
            if (!OnServer())
                return;

            Level().SetGameTimeFactor(id1);
        }
    }

    virtual void Save(IWriter* F){};
    void GetStatus(TStatus& S) override
    {
        if (!g_pGameLevel)
            return;

        float v = Level().GetGameTimeFactor();
        xr_sprintf(S, sizeof(S), "%3.5f", v);
        while (xr_strlen(S) && ('0' == S[xr_strlen(S) - 1]))
            S[xr_strlen(S) - 1] = 0;
    }
    virtual void Info(TInfo& I)
    {
        if (!OnServer())
            return;
        float v = Level().GetGameTimeFactor();
        xr_sprintf(I, sizeof(I), " value = %3.5f", v);
    }
    virtual void fill_tips(vecTips& tips, u32 mode)
    {
        if (!OnServer())
            return;
        float v = Level().GetGameTimeFactor();

        TStatus str;
        xr_sprintf(str, sizeof(str), "%3.5f  (current)  [0.0,1000.0]", v);
        tips.push_back(str);
        IConsole_Command::fill_tips(tips, mode);
    }
};

class CCC_ALifeSwitchDistance : public IConsole_Command
{
public:
    CCC_ALifeSwitchDistance(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        if ((GameID() == eGameIDSingle) && ai().get_alife())
        {
            float id1 = 0.0f;
            sscanf(args, "%f", &id1);
            if (id1 < 2.0f)
                Msg("Invalid online distance! (%.4f)", id1);
            else
            {
                NET_Packet P;
                P.w_begin(M_SWITCH_DISTANCE);
                P.w_float(id1);
                Level().Send(P, net_flags(TRUE, TRUE));
            }
        }
        else
            Log("!Not a single player game!");
    }
};

class CCC_ALifeProcessTime : public IConsole_Command
{
public:
    CCC_ALifeProcessTime(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        if ((GameID() == eGameIDSingle) && ai().get_alife())
        {
            game_sv_Single* tpGame = smart_cast<game_sv_Single*>(Level().Server->GetGameState());
            VERIFY(tpGame);
            int id1 = 0;
            sscanf(args, "%d", &id1);
            if (id1 < 1)
                Msg("Invalid process time! (%d)", id1);
            else
                tpGame->alife().set_process_time(id1);
        }
        else
            Log("!Not a single player game!");
    }
};

class CCC_ALifeObjectsPerUpdate : public IConsole_Command
{
public:
    CCC_ALifeObjectsPerUpdate(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        if ((GameID() == eGameIDSingle) && ai().get_alife())
        {
            game_sv_Single* tpGame = smart_cast<game_sv_Single*>(Level().Server->GetGameState());
            VERIFY(tpGame);
            int id1 = 0;
            sscanf(args, "%d", &id1);
            tpGame->alife().objects_per_update(id1);
        }
        else
            Log("!Not a single player game!");
    }
};

class CCC_ALifeSwitchFactor : public IConsole_Command
{
public:
    CCC_ALifeSwitchFactor(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        if ((GameID() == eGameIDSingle) && ai().get_alife())
        {
            game_sv_Single* tpGame = smart_cast<game_sv_Single*>(Level().Server->GetGameState());
            VERIFY(tpGame);
            float id1 = 0;
            sscanf(args, "%f", &id1);
            clamp(id1, .1f, 1.f);
            tpGame->alife().set_switch_factor(id1);
        }
        else
            Log("!Not a single player game!");
    }
};

//-----------------------------------------------------------------------
class CCC_DemoRecord : public IConsole_Command
{
public:
    CCC_DemoRecord(LPCSTR N) : IConsole_Command(N) {}
    virtual void Execute(LPCSTR args)
    {
        if (!g_pGameLevel) // level not loaded
        {
            Log("Demo Record is disabled when level is not loaded.");
            return;
        }

        Console->Hide();

        // close main menu if it is open
        if (MainMenu()->IsActive())
            MainMenu()->Activate(false);

        pstr fn_;
        STRCONCAT(fn_, args, ".xrdemo");
        string_path fn;
        FS.update_path(fn, "$game_saves$", fn_);

        g_pGameLevel->Cameras().AddCamEffector(xr_new<CDemoRecord>(fn));
    }
};

class CCC_DemoRecordSetPos : public CCC_Vector3
{
    static Fvector p;

public:
    CCC_DemoRecordSetPos(LPCSTR N)
        : CCC_Vector3(N, &p, Fvector().set(-FLT_MAX, -FLT_MAX, -FLT_MAX), Fvector().set(FLT_MAX, FLT_MAX, FLT_MAX)){};
    virtual void Execute(LPCSTR args)
    {
#ifndef DEBUG
// if (GameID() != eGameIDSingle)
//{
//	Msg("For this game type Demo Record is disabled.");
//	return;
//};
#endif
        CDemoRecord::GetGlobalPosition(p);
        CCC_Vector3::Execute(args);
        CDemoRecord::SetGlobalPosition(p);
    }
    virtual void Save(IWriter* F) { ; }
};
Fvector CCC_DemoRecordSetPos::p = {0, 0, 0};

class CCC_DemoPlay : public IConsole_Command
{
public:
    CCC_DemoPlay(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args)
    {
#ifndef DEBUG
// if (GameID() != eGameIDSingle)
//{
//	Msg("For this game type Demo Play is disabled.");
//	return;
//};
#endif
        if (0 == g_pGameLevel)
        {
            Msg("! There are no level(s) started");
        }
        else
        {
            Console->Hide();
            string_path fn;
            u32 loops = 0;
            pstr comma = strchr(const_cast<pstr>(args), ',');
            if (comma)
            {
                loops = atoi(comma + 1);
                *comma = 0; //. :)
            }
            strconcat(sizeof(fn), fn, args, ".xrdemo");
            FS.update_path(fn, "$game_saves$", fn);
            g_pGameLevel->Cameras().AddCamEffector(xr_new<CDemoPlay>(fn, 1.0f, loops));
        }
    }
};

class CCC_PrintNpcProfiles : public IConsole_Command
{
public:
    CCC_PrintNpcProfiles(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args)
    {
        for (auto it : Dbg.profiles)
        {
            xr_string icons = "";

            for (shared_str ico : it.second.icons)
            {
                icons.append(ico.c_str());
                icons.append(",");
            }
            Msg("NPC profile data, visual: %s icons: %s", it.first.c_str(), icons.c_str());
        }
    }
};

class CCC_Spawn : public IConsole_Command
{
public:
    CCC_Spawn(pcstr name) : IConsole_Command(name) {}

    void Execute(pcstr args) override
    {
        if (!g_pGameLevel)
            return;

        if (!IsGameTypeSingle())
        {
            Log("Spawn command is available only in singleplayer mode.");
            return;
        }

        if (!pSettings->section_exist(args))
        {
            Msg("! Section [%s] doesn't exist...", args);
            return;
        }

        Fvector pos = Actor()->Position();
        Level().g_cl_Spawn(args, 0xff, M_SPAWN_OBJECT_LOCAL, pos);
    }

    void Info(TInfo& I) override
    {
        xr_strcpy(I, "valid name of an entity or item that can be spawned");
    }
};

class CCC_SpawnToInventory : public IConsole_Command
{
public:
    CCC_SpawnToInventory(pcstr name) : IConsole_Command(name) {}

    void Execute(pcstr args) override
    {
        if (!g_pGameLevel)
            return;

        if (!IsGameTypeSingle())
        {
            Log("Spawn command is available only in singleplayer mode.");
            return;
        }

        if (!pSettings->section_exist(args))
        {
            Msg("! Section [%s] doesn't exist...", args);
            return;
        }

        Level().spawn_item(args, Actor()->Position(), false, Actor()->ID());
    }

    void Info(TInfo& I) override
    {
        xr_strcpy(I, "valid name of an item that can be spawned");
    }
};

class CCC_InvIconBakeDds final : public IConsole_Command
{
public:
    CCC_InvIconBakeDds(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr args) override
    {
        pcstr a = args;
        while (a && *a == ' ')
            ++a;
        Msg("~ [weapon_inv_icon] inv_icon_bake_dds: command invoked, target=[%s]", (a && *a) ? a : "* (all sections)");
        if (a && *a)
            weapon_inv_icon::BakeDynamicInvIconsToDds(a);
        else
            weapon_inv_icon::BakeDynamicInvIconsToDds(nullptr);
    }

    void Info(TInfo& I) override
    {
        xr_strcpy(I, "[section] optional: bake inv+tech DDS DXT5 for use_dynamic_inv_icon weapons");
    }
};

class CCC_InvIconBakeCurrentDds final : public IConsole_Command
{
public:
    CCC_InvIconBakeCurrentDds(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr args) override
    {
        if (args && *args)
            Msg("~ [weapon_inv_icon] inv_icon_bake_current_dds: args are ignored");
        weapon_inv_icon::BakeCurrentActorWeaponInvIconToDds();
    }

    void Info(TInfo& I) override
    {
        xr_strcpy(I, "bake active actor weapon to DDS (inventory+technician presets)");
    }
};
class CCC_InvIconStats final : public IConsole_Command
{
public:
    CCC_InvIconStats(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr args) override
    {
        pcstr a = args;
        while (a && *a == ' ')
            ++a;
        const bool detailed = a && *a && (0 == xr_stricmp(a, "full"));
        weapon_inv_icon::LogInvIconStats(detailed);
    }

    void Info(TInfo& I) override
    {
        xr_strcpy(I, "dynamic inventory icons: target count and video memory; [full] lists every target");
    }
};

class CCC_NpcPreviewCacheReset final : public IConsole_Command
{
public:
    CCC_NpcPreviewCacheReset(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr args) override
    {
        if (!GEnv.Render)
        {
            Msg("! [preview_scene] npc_preview_cache_reset: renderer not available");
            return;
        }
        if (args && *args)
            Msg("~ [preview_scene] npc_preview_cache_reset: args are ignored");
        GEnv.Render->PreviewScene_ResetDiskCache();
    }

    void Info(TInfo& I) override
    {
        xr_strcpy(I, "wipe npc_preview disk cache and force re-render");
    }
};
// helper functions --------------------------------------------

bool valid_saved_game_name(LPCSTR file_name)
{
    LPCSTR I = file_name;
    LPCSTR E = file_name + xr_strlen(file_name);
    for (; I != E; ++I)
    {
        if (!strchr("/" DELIMITER ":*?\"<>|^()[]%", *I))
            continue;

        return (false);
    };

    return (true);
}

void get_files_list(xr_vector<shared_str>& files, LPCSTR dir, LPCSTR file_ext)
{
    VERIFY(dir && file_ext);
    files.clear();

    FS_Path* P = FS.get_path(dir);
    P->m_Flags.set(FS_Path::flNeedRescan, TRUE);
    FS.m_Flags.set(CLocatorAPI::flNeedCheck, TRUE);
    FS.rescan_pathes();

    LPCSTR fext;
    STRCONCAT(fext, "*", file_ext);

    FS_FileSet files_set;
    FS.file_list(files_set, dir, FS_ListFiles, fext);
    u32 len_str_ext = xr_strlen(file_ext);

    auto itb = files_set.begin();
    auto ite = files_set.end();

    for (; itb != ite; ++itb)
    {
        LPCSTR fn_ext = (*itb).name.c_str();
        VERIFY(xr_strlen(fn_ext) > len_str_ext);
        string_path fn;
        strncpy_s(fn, sizeof(fn), fn_ext, xr_strlen(fn_ext) - len_str_ext);
        files.push_back(fn);
    }
    FS.m_Flags.set(CLocatorAPI::flNeedCheck, FALSE);
}

#include "UIGameCustom.h"

class CCC_ALifeSave : public IConsole_Command
{
public:
    CCC_ALifeSave(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args)
    {
#if 0
        if (!Level().autosave_manager().ready_for_autosave()) {
            Msg		("! Cannot save the game right now!");
            return;
        }
#endif
        if (!IsGameTypeSingle())
        {
            Msg("for single-mode only");
            return;
        }
        if (!g_actor || !Actor()->g_Alive())
        {
            Msg("cannot make saved game because actor is dead :(");
            return;
        }

        Console->Execute("stat_memory save");

        string_path S, S1;
        S[0] = 0;
        strncpy_s(S, sizeof(S), args, MAX_PATH - 1);

#ifdef DEBUG
        CTimer timer;
        timer.Start();
#endif
        if (!xr_strlen(S))
        {
            strconcat(sizeof(S), S, Core.UserName, " - ", "quicksave");
            NET_Packet net_packet;
            net_packet.w_begin(M_SAVE_GAME);
            net_packet.w_stringZ(S);
            net_packet.w_u8(0);
            Level().Send(net_packet, net_flags(TRUE));
        }
        else
        {
            if (!valid_saved_game_name(S))
            {
                Msg("! Save failed: invalid file name - %s", S);
                return;
            }

            NET_Packet net_packet;
            net_packet.w_begin(M_SAVE_GAME);
            net_packet.w_stringZ(S);
            net_packet.w_u8(1);
            Level().Send(net_packet, net_flags(TRUE));
        }
#ifdef DEBUG
        Msg("Game save overhead  : %f milliseconds", timer.GetElapsed_sec() * 1000.f);
#endif
        const bool compat = ClearSkyMode || ShadowOfChernobylMode;
        StaticDrawableWrapper* _s = CurrentGameUI()->AddCustomStatic("game_saved", true, compat ? 3.0f : -1.0f);

        pstr save_name;
        STRCONCAT(save_name, StringTable().translate("st_game_saved").c_str(), ": ", S);
        _s->wnd()->TextItemControl()->SetText(save_name);

        xr_strcat(S, ".dds");
        FS.update_path(S1, "$game_saves$", S);

#ifdef DEBUG
        timer.Start();
#endif
        MainMenu()->Screenshot(IRender::SM_FOR_GAMESAVE, S1);

#ifdef DEBUG
        Msg("Screenshot overhead : %f milliseconds", timer.GetElapsed_sec() * 1000.f);
#endif
    } // virtual void Execute

    virtual void fill_tips(vecTips& tips, u32 mode)
    {
        if (ShadowOfChernobylMode || ClearSkyMode)
            get_files_list(tips, "$game_saves$", SAVE_EXTENSION_LEGACY);
        else
            get_files_list(tips, "$game_saves$", SAVE_EXTENSION);
    }
}; // CCC_ALifeSave

class CCC_ALifeLoadFrom : public IConsole_Command
{
public:
    CCC_ALifeLoadFrom(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args)
    {
        string_path saved_game;
        strncpy_s(saved_game, sizeof(saved_game), args, MAX_PATH - 1);

        if (!ai().get_alife())
        {
            Log("! ALife simulator has not been started yet");
            return;
        }

        if (!xr_strlen(saved_game))
        {
            Log("! Specify file name!");
            return;
        }

        if (!CSavedGameWrapper::saved_game_exist(saved_game))
        {
            Msg("! Cannot find saved game %s", saved_game);
            return;
        }

        if (!CSavedGameWrapper::valid_saved_game(saved_game))
        {
            Msg("! Cannot load saved game %s, version mismatch or saved game is corrupted", saved_game);
            return;
        }

        if (!valid_saved_game_name(saved_game))
        {
            Msg("! Cannot load saved game %s, invalid file name", saved_game);
            return;
        }

        /*     moved to level_network_messages.cpp
                CSavedGameWrapper			wrapper(args);
                if (wrapper.level_id() == ai().level_graph().level_id()) {
                    if (Device.Paused())
                        Device.Pause		(FALSE, TRUE, TRUE, "CCC_ALifeLoadFrom");

                    Level().remove_objects	();

                    game_sv_Single			*game = smart_cast<game_sv_Single*>(Level().Server->game);
                    R_ASSERT				(game);
                    game->restart_simulator	(saved_game);

                    return;
                }
        */

        if (MainMenu()->IsActive())
            MainMenu()->Activate(false);

        Console->Execute("stat_memory load save");
        xray::perf::LoadMarkBegin("load_save"); // 50z5-extra-diagnostics.md: the [load] timeline

        if (Device.Paused())
            Device.Pause(FALSE, TRUE, TRUE, "CCC_ALifeLoadFrom");

        NET_Packet net_packet;
        net_packet.w_begin(M_LOAD_GAME);
        net_packet.w_stringZ(saved_game);
        Level().Send(net_packet, net_flags(TRUE));
    }

    virtual void fill_tips(vecTips& tips, u32 mode)
    {
        if (ShadowOfChernobylMode || ClearSkyMode)
            get_files_list(tips, "$game_saves$", SAVE_EXTENSION_LEGACY);
        else
            get_files_list(tips, "$game_saves$", SAVE_EXTENSION);
    }
}; // CCC_ALifeLoadFrom

class CCC_LoadLastSave : public IConsole_Command
{
public:
    CCC_LoadLastSave(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
    virtual void Execute(LPCSTR args)
    {
        string_path saved_game = "";
        if (args)
        {
            strncpy_s(saved_game, sizeof(saved_game), args, MAX_PATH - 1);
        }

        if (*saved_game)
        {
            xr_strcpy(g_last_saved_game, saved_game);
            return;
        }

        if (!*g_last_saved_game)
        {
            Msg("! cannot load last saved game since it hasn't been specified");
            return;
        }

        if (!CSavedGameWrapper::saved_game_exist(g_last_saved_game))
        {
            Msg("! Cannot find saved game %s", g_last_saved_game);
            return;
        }

        if (!CSavedGameWrapper::valid_saved_game(g_last_saved_game))
        {
            Msg("! Cannot load saved game %s, version mismatch or saved game is corrupted", g_last_saved_game);
            return;
        }

        if (!valid_saved_game_name(g_last_saved_game))
        {
            Msg("! Cannot load saved game %s, invalid file name", g_last_saved_game);
            return;
        }

        pstr command;
        if (ai().get_alife())
        {
            STRCONCAT(command, "load ", g_last_saved_game);
            Console->Execute(command);
            return;
        }

        STRCONCAT(command, "start server(", g_last_saved_game, "/single/alife/load)");
        Console->Execute(command);
    }

    virtual void Save(IWriter* F)
    {
        if (!*g_last_saved_game)
            return;

        F->w_printf("%s %s\r\n", cName, g_last_saved_game);
    }
};

class CCC_FlushLog : public IConsole_Command
{
public:
    CCC_FlushLog(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR /**args**/)
    {
        FlushLog();
        Msg("* Log file has been saved successfully!");
    }
};

class CCC_ClearLog : public IConsole_Command
{
public:
    CCC_ClearLog(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR)
    {
        LogFile.clear();
        FlushLog();
        Msg("* Log file has been cleaned successfully!");
    }
};

class CCC_FloatBlock : public CCC_Float
{
public:
    CCC_FloatBlock(LPCSTR N, float* V, float _min = 0, float _max = 1) : CCC_Float(N, V, _min, _max){};

    virtual void Execute(LPCSTR args)
    {
#ifdef _DEBUG
        CCC_Float::Execute(args);
#else
        if (!g_pGameLevel || GameID() == eGameIDSingle)
            CCC_Float::Execute(args);
        else
        {
            Msg("! Command disabled for this type of game");
        }
#endif
    }
};

class CCC_Net_CL_InputUpdateRate : public CCC_Integer
{
protected:
    int* value_blin;

public:
    CCC_Net_CL_InputUpdateRate(LPCSTR N, int* V, int _min = 0, int _max = 999)
        : CCC_Integer(N, V, _min, _max), value_blin(V){};

    virtual void Execute(LPCSTR args)
    {
        CCC_Integer::Execute(args);
        if ((*value_blin > 0) && g_pGameLevel)
        {
            g_dwInputUpdateDelta = 1000 / (*value_blin);
        };
    }
};

class CCC_Integer_Outfit_Faction : public CCC_Integer
{
protected:
    int* value_blin;

public:
    CCC_Integer_Outfit_Faction(LPCSTR N, int* V, int _min = 0, int _max = 999)
        : CCC_Integer(N, V, _min, _max), value_blin(V){};

    virtual void Execute(LPCSTR args)
    {
        CCC_Integer::Execute(args);
        int v = atoi(args);

        if (v == 0)
            return;

        if (!g_actor)
            return;

        PIItem outfit_in_slot = g_actor->inventory().ItemFromSlot(OUTFIT_SLOT);
        if (!outfit_in_slot)
            return;

        CCustomOutfit* outfit = smart_cast<CCustomOutfit*>(outfit_in_slot);
        if (!outfit)
            return;

        CHARACTER_COMMUNITY community;
        string512 actor_faction;
        xr_sprintf(actor_faction, "actor_%s", outfit->m_faction.c_str());
        community.set(actor_faction);
        if (community.index() == NO_COMMUNITY_INDEX)
        {
            GEnv.ScriptEngine->script_log(
                LuaMessageType::Error, "SetCharacterCommunity can't set %s for %s", actor_faction, Name());
            return;
        }

        g_actor->SetCommunity(community.index());
    }
};

class CCC_DumpFactionRelations final : public IConsole_Command
{
public:
    CCC_DumpFactionRelations(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr args) override
    {
        (void)args;

        if (!pSettings)
        {
            Msg("! dump_faction_relations: settings not loaded");
            return;
        }

        if (!CHARACTER_COMMUNITY::GetByIndex(0, true))
        {
            Msg("! dump_faction_relations: communities not initialized (game_relations?)");
            return;
        }

        const CHARACTER_COMMUNITY_INDEX maxIdx = CHARACTER_COMMUNITY::GetMaxIndex();
        if (maxIdx < 0)
        {
            Msg("! dump_faction_relations: empty community list");
            return;
        }

        Msg("[dump_faction_relations] community -> community : goodwill (default_goodwill); negative=hostile, 0=neutral, positive=friendly");

        for (CHARACTER_COMMUNITY_INDEX i = 0; i <= maxIdx; ++i)
        {
            const CHARACTER_COMMUNITY_ID idFrom = CHARACTER_COMMUNITY::IndexToId(i, NO_COMMUNITY_ID, true);
            if (!idFrom.size())
                continue;

            for (CHARACTER_COMMUNITY_INDEX j = 0; j <= maxIdx; ++j)
            {
                const CHARACTER_COMMUNITY_ID idTo = CHARACTER_COMMUNITY::IndexToId(j, NO_COMMUNITY_ID, true);
                if (!idTo.size())
                    continue;

                const CHARACTER_GOODWILL gw = CHARACTER_COMMUNITY::relation(i, j);
                const CHARACTER_GOODWILL defGw = CHARACTER_COMMUNITY::default_relation(i, j);
                Msg("  %s -> %s : %d (%d)", idFrom.c_str(), idTo.c_str(), gw, defGw);
            }
        }
    }

    void Info(TInfo& I) override { xr_strcpy(I, "dump all faction pairs goodwill (game_relations)"); }
};

#ifdef DEBUG

class CCC_DrawGameGraphAll : public IConsole_Command
{
public:
    CCC_DrawGameGraphAll(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
    virtual void Execute(LPCSTR args)
    {
        if (!ai().get_level_graph())
            return;

        Level().GetLevelGraphDebugRender()->SetupCurrentLevel(-1);
    }
};

class CCC_DrawGameGraphCurrent : public IConsole_Command
{
public:
    CCC_DrawGameGraphCurrent(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
    virtual void Execute(LPCSTR args)
    {
        if (!ai().get_level_graph())
            return;

        Level().GetLevelGraphDebugRender()->SetupCurrentLevel(ai().level_graph().level_id());
    }
};

class CCC_DrawGameGraphLevel : public IConsole_Command
{
public:
    CCC_DrawGameGraphLevel(LPCSTR N) : IConsole_Command(N) {}
    virtual void Execute(LPCSTR args)
    {
        if (!ai().get_level_graph())
            return;

        if (!*args)
        {
            Level().GetLevelGraphDebugRender()->SetupCurrentLevel(-1);
            return;
        }

        const GameGraph::SLevel* level = ai().game_graph().header().level(args, true);
        if (!level)
        {
            Msg("! There is no level %s in the game graph", args);
            return;
        }

        Level().GetLevelGraphDebugRender()->SetupCurrentLevel(level->id());
    }
};

#if defined(USE_DEBUGGER)
class CCC_ScriptDbg : public IConsole_Command
{
public:
    CCC_ScriptDbg(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args)
    {
        if (strstr(cName, "script_debug_break") == cName)
        {
            CScriptDebugger* d = GEnv.ScriptEngine->debugger();
            if (d)
            {
                if (d->Active())
                    d->initiateDebugBreak();
                else
                    Msg("Script debugger not active.");
            }
            else
                Msg("Script debugger not present.");
        }
        else if (strstr(cName, "script_debug_stop") == cName)
        {
            GEnv.ScriptEngine->stopDebugger();
        }
        else if (strstr(cName, "script_debug_restart") == cName)
        {
            GEnv.ScriptEngine->restartDebugger();
        };
    };

    virtual void Info(TInfo& I)
    {
        if (strstr(cName, "script_debug_break") == cName)
            xr_strcpy(I, "initiate script debugger [DebugBreak] command");

        else if (strstr(cName, "script_debug_stop") == cName)
            xr_strcpy(I, "stop script debugger activity");

        else if (strstr(cName, "script_debug_restart") == cName)
            xr_strcpy(I, "restarts script debugger or start if no script debugger presents");
    }
};
#endif // #if defined(USE_DEBUGGER)

class CCC_DumpInfos : public IConsole_Command
{
public:
    CCC_DumpInfos(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args)
    {
        CActor* A = smart_cast<CActor*>(Level().CurrentEntity());
        if (A)
            A->DumpInfo();
    }
    virtual void Info(TInfo& I) { xr_strcpy(I, "dumps all infoportions that actor have"); }
};
class CCC_DumpTasks : public IConsole_Command
{
public:
    CCC_DumpTasks(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args)
    {
        CActor* A = smart_cast<CActor*>(Level().CurrentEntity());
        if (A)
            A->DumpTasks();
    }
    virtual void Info(TInfo& I) { xr_strcpy(I, "dumps all tasks that actor have"); }
};
#include "map_manager.h"
class CCC_DumpMap : public IConsole_Command
{
public:
    CCC_DumpMap(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args) { Level().MapManager().Dump(); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "dumps all currentmap locations"); }
};

#include "alife_graph_registry.h"
class CCC_DumpCreatures : public IConsole_Command
{
public:
    CCC_DumpCreatures(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args)
    {
        typedef CSafeMapIterator<ALife::_OBJECT_ID, CSE_ALifeDynamicObject>::_REGISTRY::const_iterator const_iterator;

        const_iterator I = ai().alife().graph().level().objects().begin();
        const_iterator E = ai().alife().graph().level().objects().end();
        for (; I != E; ++I)
        {
            CSE_ALifeCreatureAbstract* obj = smart_cast<CSE_ALifeCreatureAbstract*>(I->second);
            if (obj)
            {
                Msg("\"%s\",", obj->name_replace());
            }
        }
    }
    virtual void Info(TInfo& I) { xr_strcpy(I, "dumps all creature names"); }
};

class CCC_DebugFonts : public IConsole_Command
{
public:
    CCC_DebugFonts(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }

    ~CCC_DebugFonts()
    {
        xr_free(m_ui);
    }

    virtual void Execute(LPCSTR args)
    {
        if (!m_ui)
            m_ui = xr_new<CUIDebugFonts>();

        m_ui->ShowDialog(true);
    }

private:
    CUIDebugFonts* m_ui;
};

class CCC_DebugNode : public IConsole_Command
{
public:
    CCC_DebugNode(LPCSTR N) : IConsole_Command(N){};

    virtual void Execute(LPCSTR args)
    {
        string128 param1, param2;
        VERIFY(xr_strlen(args) < sizeof(string128));

        _GetItem(args, 0, param1, ' ');
        _GetItem(args, 1, param2, ' ');

        u32 value1;
        u32 value2;

        sscanf(param1, "%u", &value1);
        sscanf(param2, "%u", &value2);

        if ((value1 > 0) && (value2 > 0))
        {
            g_bDebugNode = TRUE;
            g_dwDebugNodeSource = value1;
            g_dwDebugNodeDest = value2;
        }
        else
        {
            g_bDebugNode = FALSE;
        }
    }
};

class CCC_ShowMonsterInfo : public IConsole_Command
{
public:
    CCC_ShowMonsterInfo(LPCSTR N) : IConsole_Command(N){};

    virtual void Execute(LPCSTR args)
    {
        string128 param1, param2;
        VERIFY(xr_strlen(args) < sizeof(string128));

        _GetItem(args, 0, param1, ' ');
        _GetItem(args, 1, param2, ' ');

        IGameObject* obj = Level().Objects.FindObjectByName(param1);
        CBaseMonster* monster = smart_cast<CBaseMonster*>(obj);
        if (!monster)
            return;

        u32 value2;

        sscanf(param2, "%u", &value2);
        monster->set_show_debug_info(u8(value2));
    }
};

void PH_DBG_SetTrackObject();
extern string64 s_dbg_trace_obj_name;
class CCC_DbgPhTrackObj : public CCC_String
{
public:
    CCC_DbgPhTrackObj(LPCSTR N) : CCC_String(N, s_dbg_trace_obj_name, sizeof(s_dbg_trace_obj_name)){};
    virtual void Execute(LPCSTR args)
    {
        CCC_String::Execute(args);
        if (!xr_strcmp(args, "none"))
        {
            ph_dbg_draw_mask1.set(ph_m1_DbgTrackObject, FALSE);
            return;
        }
        ph_dbg_draw_mask1.set(ph_m1_DbgTrackObject, TRUE);
        PH_DBG_SetTrackObject();
        // IGameObject* O= Level().Objects.FindObjectByName(args);
        // if(O)
        //{
        //	PH_DBG_SetTrackObject(*(O->cName()));
        //	ph_dbg_draw_mask1.set(ph_m1_DbgTrackObject,TRUE);
        //}
    }

    // virtual void	Info	(TInfo& I)
    //{
    //	xr_strcpy(I,"restart game fast");
    //}
};
#endif

class CCC_PHIterations : public CCC_Integer
{
public:
    CCC_PHIterations(LPCSTR N) : CCC_Integer(N, &phIterations, 10, 50){};
    virtual void Execute(LPCSTR args)
    {
        CCC_Integer::Execute(args);
        // dWorldSetQuickStepNumIterations(NULL,phIterations);
        if (physics_world())
            physics_world()->StepNumIterations(phIterations);
    }
};

#ifdef DEBUG
class CCC_PHGravity : public IConsole_Command
{
public:
    CCC_PHGravity(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        if (!physics_world())
            return;
#ifndef DEBUG
        if (g_pGameLevel && Level().game && GameID() != eGameIDSingle)
        {
            Msg("Command is not available in Multiplayer");
            return;
        }
#endif
        physics_world()->SetGravity(float(atof(args)));
    }
    void GetStatus(TStatus& S) override
    {
        if (physics_world())
            xr_sprintf(S, "%3.5f", physics_world()->Gravity());
        else
            xr_sprintf(S, "%3.5f", default_world_gravity);
        while (xr_strlen(S) && ('0' == S[xr_strlen(S) - 1]))
            S[xr_strlen(S) - 1] = 0;
    }
};
#endif // DEBUG

class CCC_PHFps : public CCC_Float
{
#ifndef DEBUG
    static constexpr float MIN_FPS = 50;
    static constexpr float MAX_FPS = 200;
#else
    static constexpr float MIN_FPS = 1;
    static constexpr float MAX_FPS = 1000;
#endif

    float m_dummy = 1.f / ph_console::ph_step_time;

public:
    CCC_PHFps(pcstr name) : CCC_Float(name, &m_dummy, MIN_FPS, MAX_FPS) { }

    void Execute(pcstr args) override
    {
        CCC_Float::Execute(args);

        ph_console::ph_step_time = 1.f / m_dummy;
        if (physics_world())
            physics_world()->SetStep(ph_console::ph_step_time);
    }
};

#ifdef DEBUG

struct CCC_ShowSmartCastStats : public IConsole_Command
{
    CCC_ShowSmartCastStats(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args) { show_smart_cast_stats(); }
};

struct CCC_ClearSmartCastStats : public IConsole_Command
{
    CCC_ClearSmartCastStats(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args) { clear_smart_cast_stats(); }
};

#endif

#ifndef MASTER_GOLD
/*
struct CCC_NoClip : public CCC_Mask
{
public:
    CCC_NoClip(LPCSTR N, Flags32* V, u32 M):CCC_Mask(N,V,M){};
    virtual	void Execute(LPCSTR args)
    {
        CCC_Mask::Execute(args);
        if (EQ(args,"on") || EQ(args,"1"))
        {
            if(g_pGameLevel && Level().CurrentViewEntity())
            {
                CActor* actor = smart_cast<CActor*>(Level().CurrentViewEntity());
                actor->character_physics_support()->SetRemoved();
            }
        }
    };
};
*/

struct CCC_ToggleNoClip : public IConsole_Command
{
    CCC_ToggleNoClip(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; };

    void Execute(pcstr /*args*/) override
    {
        psActorFlags.invert(AF_NO_CLIP);

        if (!g_pGameLevel)
            return;

        CActor* actor = smart_cast<CActor*>(Level().CurrentViewEntity());
        if (!actor)
            return;

        // Workaround for actor has no physics at all until first move
        Fvector accel{};
        Actor()->g_Physics(accel, 0.0f, 0.0f);
    }
};

#include "xrAICore/Navigation/game_graph.h"
struct CCC_JumpToLevel : public IConsole_Command
{
    CCC_JumpToLevel(LPCSTR N) : IConsole_Command(N){};

    virtual void Execute(LPCSTR level)
    {
        if (!ai().get_alife())
        {
            Msg("! ALife simulator is needed to perform specified command!");
            return;
        }

        GameGraph::LEVEL_MAP::const_iterator I = ai().game_graph().header().levels().begin();
        GameGraph::LEVEL_MAP::const_iterator E = ai().game_graph().header().levels().end();
        for (; I != E; ++I)
            if (!xr_strcmp((*I).second.name(), level))
            {
                ai().alife().jump_to_level(level);
                return;
            }
        Msg("! There is no level \"%s\" in the game graph!", level);
    }

    virtual void Save(IWriter* F){};
    virtual void fill_tips(vecTips& tips, u32 mode)
    {
        if (!ai().get_alife())
        {
            Msg("! ALife simulator is needed to perform specified command!");
            return;
        }

        GameGraph::LEVEL_MAP::const_iterator itb = ai().game_graph().header().levels().begin();
        GameGraph::LEVEL_MAP::const_iterator ite = ai().game_graph().header().levels().end();
        for (; itb != ite; ++itb)
        {
            tips.push_back((*itb).second.name());
        }
    }
};

//#ifndef MASTER_GOLD
class CCC_Script : public IConsole_Command
{
public:
    CCC_Script(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = false; };
    virtual void Execute(LPCSTR args)
    {
        if (!xr_strlen(args))
        {
            Log("* Specify script name!");
        }
        else
        {
            // rescan pathes
            FS_Path* P = FS.get_path("$game_scripts$");
            P->m_Flags.set(FS_Path::flNeedRescan, TRUE);
            FS.rescan_pathes();
            // run script
            if (GEnv.ScriptEngine->script_process(ScriptProcessor::Level))
                GEnv.ScriptEngine->script_process(ScriptProcessor::Level)->add_script(args, false, true);
        }
    }

    void GetStatus(TStatus& S) override { xr_strcpy(S, "<script_name> (Specify script name!)"); }
    virtual void Save(IWriter* F) {}
    virtual void fill_tips(vecTips& tips, u32 mode) { get_files_list(tips, "$game_scripts$", ".script"); }
};

class CCC_ScriptCommand : public IConsole_Command
{
public:
    CCC_ScriptCommand(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = false; bLowerCaseArgs = false; }
    virtual void Execute(LPCSTR args)
    {
        if (!xr_strlen(args))
            Log("* Specify string to run!");
        else
        {
            if (GEnv.ScriptEngine->script_process(ScriptProcessor::Level))
            {
                GEnv.ScriptEngine->script_process(ScriptProcessor::Level)->add_script(args, true, true);
                return;
            }

            string4096 S;
            shared_str m_script_name = "console command";
            xr_sprintf(S, "%s\n", args);
            int l_iErrorCode = luaL_loadbuffer(GEnv.ScriptEngine->lua(), S, xr_strlen(S), "@console_command");
            if (!l_iErrorCode)
            {
                l_iErrorCode = lua_pcall(GEnv.ScriptEngine->lua(), 0, 0, 0);
                if (l_iErrorCode)
                {
                    GEnv.ScriptEngine->print_output(GEnv.ScriptEngine->lua(), m_script_name.c_str(), l_iErrorCode);
                    GEnv.ScriptEngine->on_error(GEnv.ScriptEngine->lua());
                    return;
                }
            }

            GEnv.ScriptEngine->print_output(GEnv.ScriptEngine->lua(), m_script_name.c_str(), l_iErrorCode);
        }
    } // void	Execute

    void GetStatus(TStatus& S) override { xr_strcpy(S, "<script_name.function()> (Specify script and function name!)"); }
    virtual void Save(IWriter* F) {}
    virtual void fill_tips(vecTips& tips, u32 mode)
    {
        if (mode == 1)
        {
            get_files_list(tips, "$game_scripts$", ".script");
            return;
        }

        IConsole_Command::fill_tips(tips, mode);
    }
};

class CCC_TimeFactor : public IConsole_Command
{
public:
    CCC_TimeFactor(LPCSTR N) : IConsole_Command(N) {}
    virtual void Execute(LPCSTR args)
    {
        float time_factor = (float)atof(args);
        clamp(time_factor, EPS, 1000.f);
        Device.time_factor(time_factor);
    }
    void GetStatus(TStatus& S) override { xr_sprintf(S, sizeof(S), "%f", Device.time_factor()); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "[0.001 - 1000.0]"); }
    virtual void fill_tips(vecTips& tips, u32 mode)
    {
        TStatus str;
        xr_sprintf(str, sizeof(str), "%3.3f  (current)  [0.001 - 1000.0]", Device.time_factor());
        tips.push_back(str);
        IConsole_Command::fill_tips(tips, mode);
    }
};

#endif // MASTER_GOLD

class CCC_LuaGCMethod : public CCC_Token
{
public:
    CCC_LuaGCMethod(pcstr name) : CCC_Token(name, &ps_lua_gc_method, lua_gc_method_token) {}

    void Execute(pcstr args) override
    {
        const auto prev = *value;
        CCC_Token::Execute(args);

        switch (*value)
        {
        case 0:
            lua_gc(GEnv.ScriptEngine->lua(), LUA_GCSTOP, 0);
            break;
        case 1:
        case 2:
            if (prev == 0)
                lua_gc(GEnv.ScriptEngine->lua(), LUA_GCRESTART, 0);
            break;
        case 3:
            // Perform a full garbage collection cycle and return to previous strategy.
            lua_gc(GEnv.ScriptEngine->lua(), LUA_GCCOLLECT, 0);
            *value = prev;
            break;
        }
    }
};

// G2 (plans/optimization_mt/32-frame-spikes.md): lua_heap_census [full] - the census of the Lua heap at the next GC
// step of a level (CLevel::script_gc, the thread that runs Lua); full = a full GC first
class CCC_LuaHeapCensus : public IConsole_Command
{
public:
    CCC_LuaHeapCensus(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr args) override { gw::lua_spike::RequestHeapCensus(args && strstr(args, "full") != nullptr); }
};

// g_w_scope_hud_fov <weapon_section>;<scope_section> <abs_hud_fov>
class CCC_ScopeHudFov : public IConsole_Command
{
public:
    CCC_ScopeHudFov(LPCSTR n) : IConsole_Command(n) { bEmptyArgsHandled = true; }

    void Execute(LPCSTR args) override
    {
        if (!args || !*args)
        {
            for (const auto& it : g_scope_hud_fov_presets)
                Msg("%s %s %f", cName, it.first.c_str(), it.second);
            return;
        }

        string512 key;
        float value = 0.0f;
        *key = 0;

        int parsed = sscanf(args, "%s %f", key, &value);
        if (parsed <= 0)
            return;

        if (parsed == 1)
        {
            auto it = g_scope_hud_fov_presets.find(shared_str(key));
            if (it != g_scope_hud_fov_presets.end())
                g_scope_hud_fov_presets.erase(it);
            return;
        }

        clamp(value, 0.05f, 2.0f);
        g_scope_hud_fov_presets[shared_str(key)] = value;
    }

    void Save(IWriter* f) override
    {
        if (g_scope_hud_fov_presets.empty())
            return;

        for (const auto& it : g_scope_hud_fov_presets)
            f->w_printf("%s %s %f\r\n", cName, it.first.c_str(), it.second);
    }
};

// weapon_hud <weapon_section> <abs_hud_fov>
class CCC_WeaponHudFov : public IConsole_Command
{
public:
    CCC_WeaponHudFov(LPCSTR n) : IConsole_Command(n) { bEmptyArgsHandled = true; }

    void Execute(LPCSTR args) override
    {
        if (!args || !*args)
        {
            for (const auto& it : g_weapon_hud_fov_presets)
                Msg("%s %s %f", cName, it.first.c_str(), it.second);
            return;
        }

        string512 key;
        float value = 0.0f;
        *key = 0;

        int parsed = sscanf(args, "%s %f", key, &value);
        if (parsed <= 0)
            return;

        if (parsed == 1)
        {
            auto it = g_weapon_hud_fov_presets.find(shared_str(key));
            if (it != g_weapon_hud_fov_presets.end())
                g_weapon_hud_fov_presets.erase(it);
            return;
        }

        clamp(value, 0.1f, 1.0f);
        g_weapon_hud_fov_presets[shared_str(key)] = value;
    }

    void Save(IWriter* f) override
    {
        if (g_weapon_hud_fov_presets.empty())
            return;

        for (const auto& it : g_weapon_hud_fov_presets)
            f->w_printf("%s %s %f\r\n", cName, it.first.c_str(), it.second);
    }
};

#include "GamePersistent.h"

struct CCC_LuaHelp : public IConsole_Command
{
	CCC_LuaHelp(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };

	virtual void Execute(LPCSTR args)
	{
        GEnv.ScriptEngine->dumpScriptBindings();
	}
};

// Lists discovered addons, their plugins, state and the reason of failure (wiki/doc/plugins).
class CCC_AddonList : public IConsole_Command
{
public:
    CCC_AddonList(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR /*args*/) { gw::addons::PrintList(); }
};

// plugin_reload <addon id>: the plugin of the addon is unloaded and loaded again from its library (a rebuilt one
// too) at the start of the next frame; it also brings back a plugin stopped by a crash
class CCC_PluginReload : public IConsole_Command
{
public:
    CCC_PluginReload(LPCSTR N) : IConsole_Command(N) {}
    virtual void Execute(LPCSTR args)
    {
        xr_string reason;
        if (gw::addons::RequestPluginReload(args, reason))
            Msg("- [addons] plugin of '%s' is reloaded at the next frame", args);
        else
            Msg("! [addons] plugin_reload %s: %s", args, reason.c_str());
    }
    virtual void Info(TInfo& I) { xr_strcpy(I, "addon id: unload its plugin and load it again (also after a crash)"); }
};

class CCC_EventList : public IConsole_Command
{
public:
    CCC_EventList(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR /*args*/) { gw::addons::events::PrintList(); }
};

// Stage B diagnostics (plans/lua_to_cpp/06): which engine groups emit object events, schema check, emit trace.
class CCC_EventEngineSources : public IConsole_Command
{
public:
    CCC_EventEngineSources(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = false; bLowerCaseArgs = true; };
    virtual void Execute(LPCSTR args) { gw::addons::objevents::SetEnabledGroups(args); }
    virtual void GetStatus(TStatus& S) { gw::addons::objevents::GetStatus(S, sizeof(S)); }
    virtual void Info(TInfo& I) { xr_strcpy(I, "all | none | actor,input,monster,physic,npc,lifecycle"); }
    virtual void Save(IWriter*) {} // diagnostic: not kept in user.ltx
};

class CCC_EventSchemaCheck : public IConsole_Command
{
public:
    CCC_EventSchemaCheck(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = false; bLowerCaseArgs = true; };
    virtual void Execute(LPCSTR args)
    {
        if (!xr_strcmp(args, "auto"))
            gw::addons::events::SetSchemaCheck(-1);
        else
            gw::addons::events::SetSchemaCheck(atoi(args) != 0 || !xr_strcmp(args, "on") ? 1 : 0);
    }
    virtual void GetStatus(TStatus& S)
    {
        const int mode = gw::addons::events::GetSchemaCheck();
        xr_strcpy(S, mode < 0 ? "auto (on with -addon_debug)" : mode ? "1" : "0");
    }
    virtual void Info(TInfo& I) { xr_strcpy(I, "0 | 1 | auto"); }
    virtual void Save(IWriter*) {}
};

class CCC_EventEngineLog : public IConsole_Command
{
public:
    CCC_EventEngineLog(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = false; bLowerCaseArgs = true; };
    virtual void Execute(LPCSTR args)
    {
        if (!xr_strcmp(args, "auto"))
            gw::addons::events::SetEngineLog(-1);
        else
            gw::addons::events::SetEngineLog(atoi(args) != 0 || !xr_strcmp(args, "on") ? 1 : 0);
    }
    virtual void GetStatus(TStatus& S)
    {
        const int mode = gw::addons::events::GetEngineLog();
        xr_strcpy(S, mode < 0 ? "auto (on with -addon_debug)" : mode ? "1" : "0");
    }
    virtual void Info(TInfo& I) { xr_strcpy(I, "0 | 1 | auto: log emits of the events moved to the engine"); }
    virtual void Save(IWriter*) {}
};

class CCC_EventTrace : public IConsole_Command
{
public:
    CCC_EventTrace(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = false; bLowerCaseArgs = false; };
    virtual void Execute(LPCSTR args)
    {
        string256 name, state;
        name[0] = state[0] = 0;
        sscanf(args, "%255s %255s", name, state);
        const bool on = !state[0] || !xr_strcmp(state, "1") || !xr_stricmp(state, "on");
        if (gw::addons::events::SetTrace(name, on))
            Msg("- [events] trace of '%s': %s", name, on ? "on" : "off");
        else
            Msg("! [events] event_trace: invalid event name '%s'", name);
    }
    virtual void Info(TInfo& I) { xr_strcpy(I, "<event name> [on|off]"); }
    virtual void Save(IWriter*) {}
};

class CCC_DataList : public IConsole_Command
{
public:
    CCC_DataList(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args) { gw::addons::data::PrintList(args); } // args: optional key prefix
};

// Evaluator cache tuning: the table lives in configs/npc_perf_evaluator_cache.ltx and is reloaded in place,
// so a policy can be tried without restarting the game (see docs, "кэш эвалюаторов").
extern void ai_evaluator_cache_reload();
extern void ai_evaluator_cache_print();

class CCC_EvaluatorCacheReload : public IConsole_Command
{
public:
    CCC_EvaluatorCacheReload(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR /*args*/) { ai_evaluator_cache_reload(); ai_evaluator_cache_print(); }
};

class CCC_EvaluatorCacheList : public IConsole_Command
{
public:
    CCC_EvaluatorCacheList(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR /*args*/) { ai_evaluator_cache_print(); }
};

class CCC_IniList : public IConsole_Command
{
public:
    CCC_IniList(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR /*args*/) { gw::addons::ini::PrintList(); }
};

class CCC_TimerList : public IConsole_Command
{
public:
    CCC_TimerList(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR /*args*/) { gw::addons::timers::PrintList(); }
};

class CCC_BinderList : public IConsole_Command
{
public:
    CCC_BinderList(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR /*args*/) { gw::addons::binders::PrintList(); }
};

// Calls between plugins and Lua: script_call (plugin -> Lua) and plugins.call (Lua -> plugin) by function, with the
// number of calls, calls per second, errors and time. "script_call_list reset" also zeroes the counters.
class CCC_ScriptCallList : public IConsole_Command
{
public:
    CCC_ScriptCallList(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args)
    {
        const bool reset = args && xr_strcmp(args, "reset") == 0;
        gw::addons::PrintScriptCallList(reset);
        gw::addons::PrintExportCallList(reset);
        if (reset)
            Msg("- [script] the counters are reset");
    }
    virtual void Info(TInfo& I) { xr_strcpy(I, "[reset] - calls between plugins and Lua by function"); }
};

// Variables of plugins with their values and where the values came from; lines of appdata/plugins.ltx
// without a variable (plans/lua_to_cpp/12-plugin-cvars-file.md).
class CCC_PluginCvarList : public IConsole_Command
{
public:
    CCC_PluginCvarList(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR /*args*/) { gw::addons::console::PrintCvarList(); }
};

class CCC_CallbackList : public IConsole_Command
{
public:
    CCC_CallbackList(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR /*args*/) { gw::addons::callbacks::PrintList(); }
};

class CCC_EvaluatorList : public IConsole_Command
{
public:
    CCC_EvaluatorList(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR /*args*/) { gw::addons::goap::PrintList(); }
};

class CCC_StorageList : public IConsole_Command
{
public:
    CCC_StorageList(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args) { gw::addons::storage::PrintList(args); }
};

class CCC_MainMenu : public IConsole_Command
{
public:
    CCC_MainMenu(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR args)
    {
        bool bWhatToDo = TRUE;
        if (0 == xr_strlen(args))
        {
            bWhatToDo = !MainMenu()->IsActive();
        };

        if (EQ(args, "on") || EQ(args, "1"))
            bWhatToDo = TRUE;

        if (EQ(args, "off") || EQ(args, "0"))
            bWhatToDo = FALSE;

        MainMenu()->Activate(bWhatToDo);
    }
};

class CCC_UIStyle : public CCC_Token
{
    u32 m_id = 0;

public:
    CCC_UIStyle(pcstr name) : CCC_Token(name, &m_id, nullptr) { }

    void Execute(pcstr args) override
    {
        CCC_Token::Execute(args);
        UIStyles->SetupStyle(m_id);
    }

    const xr_token* GetToken() noexcept override // may throw exceptions!
    {
        return UIStyles->GetToken().data();
    }
};

class CCC_UIRestart : public IConsole_Command
{
public:
    CCC_UIRestart(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr /*args*/) override
    {
        UIStyles->Reset();
    }
};

struct CCC_StartTimeSingle : public IConsole_Command
{
    CCC_StartTimeSingle(LPCSTR N) : IConsole_Command(N){};
    virtual void Execute(LPCSTR args)
    {
        u32 year = 1, month = 1, day = 1, hours = 0, mins = 0, secs = 0, milisecs = 0;
        sscanf(args, "%d.%d.%d %d:%d:%d.%d", &year, &month, &day, &hours, &mins, &secs, &milisecs);
        year = _max(year, 1);
        month = _max(month, 1);
        day = _max(day, 1);
        g_qwStartGameTime = generate_time(year, month, day, hours, mins, secs, milisecs);

        if (!g_pGameLevel)
            return;

        if (!Level().Server)
            return;

        if (!Level().Server->GetGameState())
            return;

        Level().SetGameTimeFactor(g_qwStartGameTime, g_fTimeFactor);
    }

    void GetStatus(TStatus& S) override
    {
        u32 year = 1, month = 1, day = 1, hours = 0, mins = 0, secs = 0, milisecs = 0;
        split_time(g_qwStartGameTime, year, month, day, hours, mins, secs, milisecs);
        xr_sprintf(S, "%d.%d.%d %d:%d:%d.%d", year, month, day, hours, mins, secs, milisecs);
    }
};

struct CCC_TimeFactorSingle : public CCC_Float
{
    CCC_TimeFactorSingle(LPCSTR N, float* V, float _min = 0.f, float _max = 1.f) : CCC_Float(N, V, _min, _max){};

    virtual void Execute(LPCSTR args)
    {
        CCC_Float::Execute(args);

        if (!g_pGameLevel)
            return;

        if (!Level().Server)
            return;

        if (!Level().Server->GetGameState())
            return;

        Level().SetGameTimeFactor(g_fTimeFactor);
    }
};

#include "attachable_item.h"
#include "attachment_owner.h"
#include "InventoryOwner.h"
#include "Inventory.h"
class CCC_TuneAttachableItem : public IConsole_Command
{
public:
    CCC_TuneAttachableItem(LPCSTR N) : IConsole_Command(N) {};
    virtual void Execute(LPCSTR args)
    {
        if (CAttachableItem::m_dbgItem)
        {
            CAttachableItem::m_dbgItem = NULL;
            Msg("CCC_TuneAttachableItem switched to off");
            return;
        };

        IGameObject* obj = Level().CurrentViewEntity();
        VERIFY(obj);
        shared_str ssss = args;

        CAttachmentOwner* owner = smart_cast<CAttachmentOwner*>(obj);
        CAttachableItem* itm = owner->attachedItem(ssss);
        if (itm)
        {
            CAttachableItem::m_dbgItem = itm;
        }
        else
        {
            CInventoryOwner* iowner = smart_cast<CInventoryOwner*>(obj);
            PIItem active_item = iowner->m_inventory->ActiveItem();
            if (active_item && active_item->object().cNameSect() == ssss)
                CAttachableItem::m_dbgItem = active_item->cast_attachable_item();
        }

        if (CAttachableItem::m_dbgItem)
            Msg("CCC_TuneAttachableItem switched to ON for [%s]", args);
        else
            Msg("CCC_TuneAttachableItem cannot find attached item [%s]", args);
    }

    virtual void Info(TInfo& I)
    {
        xr_sprintf(I, "allows to change bind rotation and position offsets for attached item, <section_name> given as arguments");
    }
};

#ifdef DEBUG
class CCC_RadioGroupMask2;
class CCC_RadioMask : public CCC_Mask
{
    CCC_RadioGroupMask2* group;

public:
    CCC_RadioMask(LPCSTR N, Flags32* V, u32 M) : CCC_Mask(N, V, M) { group = NULL; }
    void SetGroup(CCC_RadioGroupMask2* G) { group = G; }
    virtual void Execute(LPCSTR args);

    IC void Set(BOOL V) { value->set(mask, V); }
};

class CCC_RadioGroupMask2
{
    CCC_RadioMask* mask0;
    CCC_RadioMask* mask1;

public:
    CCC_RadioGroupMask2(CCC_RadioMask* m0, CCC_RadioMask* m1)
    {
        mask0 = m0;
        mask1 = m1;
        mask0->SetGroup(this);
        mask1->SetGroup(this);
    }
    void Execute(CCC_RadioMask& m, LPCSTR args)
    {
        BOOL value = m.GetValue();
        if (value)
        {
            mask0->Set(!value);
            mask1->Set(!value);
        }
        m.Set(value);
    }
};

void CCC_RadioMask::Execute(LPCSTR args)
{
    CCC_Mask::Execute(args);
    VERIFY2(group, "CCC_RadioMask: group not set");
    group->Execute(*this, args);
}

#define CMD_RADIOGROUPMASK2(p1, p2, p3, p4, p5, p6)                                        \
    \
{                                                                                   \
        \
static CCC_RadioMask x##CCC_RadioMask1(p1, p2, p3);                                        \
        Console->AddCommand(&x##CCC_RadioMask1);                                           \
        \
static CCC_RadioMask x##CCC_RadioMask2(p4, p5, p6);                                        \
        Console->AddCommand(&x##CCC_RadioMask2);                                           \
        \
static CCC_RadioGroupMask2 x##CCC_RadioGroupMask2(&x##CCC_RadioMask1, &x##CCC_RadioMask2); \
    \
}

struct CCC_DbgBullets : public CCC_Integer
{
    CCC_DbgBullets(LPCSTR N, int* V, int _min = 0, int _max = 999) : CCC_Integer(N, V, _min, _max){};

    virtual void Execute(LPCSTR args)
    {
        extern xr_vector<Fvector> g_hit[];
        g_hit[0].clear();
        g_hit[1].clear();
        g_hit[2].clear();
        CCC_Integer::Execute(args);
    }
};

class CCC_Crash : public IConsole_Command
{
public:
    CCC_Crash(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR /**args**/)
    {
        VERIFY3(false, "This is a test crash", "Do not post it as a bug");
        int* pointer = 0;
        *pointer = 0; //-V522
    }
};

class CCC_DumpModelBones : public IConsole_Command
{
public:
    CCC_DumpModelBones(LPCSTR N) : IConsole_Command(N) {}
    virtual void Execute(LPCSTR arguments)
    {
        if (!arguments || !*arguments)
        {
            Msg("! no arguments passed");
            return;
        }

        LPCSTR name;

        if (0 == strext(arguments))
            STRCONCAT(name, arguments, ".ogf");
        else
            STRCONCAT(name, arguments);

        string_path fn;

        if (!FS.exist(arguments) && !FS.exist(fn, "$level$", name) && !FS.exist(fn, "$game_meshes$", name))
        {
            Msg("! Cannot find visual \"%s\"", arguments);
            return;
        }

        IRenderVisual* visual = GEnv.Render->model_Create(arguments);
        IKinematics* kinematics = smart_cast<IKinematics*>(visual);
        if (!kinematics)
        {
            GEnv.Render->model_Delete(visual);
            Msg("! Invalid visual type \"%s\" (not a IKinematics)", arguments);
            return;
        }

        Msg("bones for model \"%s\"", arguments);
        for (u16 i = 0, n = kinematics->LL_BoneCount(); i < n; ++i)
            Msg("%s", kinematics->LL_GetData(i).name.c_str());

        GEnv.Render->model_Delete(visual);
    }
};

extern void show_animation_stats();

class CCC_ShowAnimationStats : public IConsole_Command
{
public:
    CCC_ShowAnimationStats(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR) { show_animation_stats(); }
};

class CCC_InvUpgradesHierarchy : public IConsole_Command
{
public:
    CCC_InvUpgradesHierarchy(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args)
    {
        if (ai().get_alife())
        {
            ai().alife().inventory_upgrade_manager().log_hierarchy();
        }
    }
};

class CCC_InvUpgradesCurItem : public IConsole_Command
{
public:
    CCC_InvUpgradesCurItem(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args)
    {
        if (!g_pGameLevel)
        {
            return;
        }
        CUIGameSP* ui_game_sp = smart_cast<CUIGameSP*>(CurrentGameUI());
        if (!ui_game_sp)
        {
            return;
        }
        PIItem item = ui_game_sp->GetActorMenu().get_upgrade_item();
        if (item)
        {
            item->log_upgrades();
        }
        else
        {
            Msg("- Current item in ActorMenu is unknown!");
        }
    }
};

class CCC_InvDropAllItems : public IConsole_Command
{
public:
    CCC_InvDropAllItems(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; };
    virtual void Execute(LPCSTR args)
    {
        if (!g_pGameLevel)
        {
            return;
        }
        CUIGameSP* ui_game_sp = smart_cast<CUIGameSP*>(CurrentGameUI());
        if (!ui_game_sp)
        {
            return;
        }
        int d = 0;
        sscanf(args, "%d", &d);
        if (ui_game_sp->GetActorMenu().DropAllItemsFromRuck(d == 1))
        {
            Msg("- All items from ruck of Actor is dropping now.");
        }
        else
        {
            Msg("! ActorMenu is not in state `Inventory`");
        }
    }
}; // CCC_InvDropAllItems

#endif // DEBUG

class CCC_DumpObjects : public IConsole_Command
{
public:
    CCC_DumpObjects(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; };
    virtual void Execute(LPCSTR) { Level().Objects.dump_all_objects(); }
};

class CCC_GSCheckForUpdates : public IConsole_Command
{
    bool m_informNoPatch = true;

    void SetupCallParams(pcstr args)
    {
        m_informNoPatch = true;
        if (args && *args)
        {
            int bInfo = 1;
            sscanf(args, "%d", &bInfo);
            m_informNoPatch = (bInfo != 0);
        }
    }

public:
    CCC_GSCheckForUpdates(LPCSTR N) : IConsole_Command(N)
    {
        bEmptyArgsHandled = true;
    };

    virtual void Execute(LPCSTR arguments)
    {
        auto mm = MainMenu();
        if (mm == nullptr)
            return;

        SetupCallParams(arguments);

        if (m_informNoPatch)
        {
            mm->OnPatchCheck(false);
        }
    }
};

class CCC_Net_SV_GuaranteedPacketMode : public CCC_Integer
{
protected:
    int* value_blin;

public:
    CCC_Net_SV_GuaranteedPacketMode(LPCSTR N, int* V, int _min = 0, int _max = 2)
        : CCC_Integer(N, V, _min, _max), value_blin(V){};

    virtual void Execute(LPCSTR args) { CCC_Integer::Execute(args); }
};
#ifdef DEBUG
void DBG_CashedClear();
class CCC_DBGDrawCashedClear : public IConsole_Command
{
public:
    CCC_DBGDrawCashedClear(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = true; }
private:
    virtual void Execute(LPCSTR args) { DBG_CashedClear(); }
};

#endif

class CCC_DbgVar : public IConsole_Command
{
public:
    CCC_DbgVar(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = false; };
    virtual void Execute(LPCSTR arguments)
    {
        if (!arguments || !*arguments)
        {
            return;
        }

        if (_GetItemCount(arguments, ' ') == 1)
        {
            ai_dbg::show_var(arguments);
        }
        else
        {
            char name[1024];
            float f;
            sscanf(arguments, "%s %f", name, &f);
            ai_dbg::set_var(name, f);
        }
    }
};

class CCC_CleanupTasks : public IConsole_Command
{
public:
    CCC_CleanupTasks(pcstr name) : IConsole_Command(name) {}
    void Execute(pcstr /*args*/) override
    {
        Level().GameTaskManager().CleanupTasks();
    }
};

class CCC_UI_Time_Dilation_Mode : public IConsole_Command
{
    UITimeDilator::UIMode mode;
    bool isEnable;

public:
    CCC_UI_Time_Dilation_Mode(pcstr name, UITimeDilator::UIMode mode) : IConsole_Command(name), mode(mode) {};

    void Execute(pcstr args) override
    {
        if (EQ(args, "on") || EQ(args, "1"))
        {
            TimeDilator()->SetModeEnability(mode, true);
            isEnable = true;
        }
        else if (EQ(args, "off") || EQ(args, "0"))
        {
            TimeDilator()->SetModeEnability(mode, false);
            isEnable = false;
        }
        else
            InvalidSyntax();
    }

    void GetStatus(TStatus& status) override
    {
        xr_strcpy(status, isEnable ? "on" : "off");
    }

    void Info(TInfo& info) override
    {
        xr_strcpy(info, "'on/off' or '1/0'");
    }

    void fill_tips(vecTips& tips, u32 /*mode*/) override
    {
        TStatus str;
        xr_sprintf(str, sizeof(str), "%s (current) [on/off]", isEnable ? "on" : "off");
        tips.push_back(str);
    }
};

class CCC_UI_Time_Factor : public IConsole_Command
{
    float uiTimeFactor = 1.0;

public:
    CCC_UI_Time_Factor(pcstr name) : IConsole_Command(name){};

    void Execute(pcstr args) override
    {
        float time_factor = (float)atof(args);
        clamp(time_factor, EPS, 1.f);
        TimeDilator()->SetUiTimeFactor(time_factor);
        uiTimeFactor = time_factor;
    }

    void Info(TInfo& info) override
    {
        xr_strcpy(info, "[0.001 - 1.0]");
    }

    void fill_tips(vecTips& tips, u32 mode) override
    {
        TStatus str;
        xr_sprintf(str, sizeof(str), "%3.3f (current) [0.001 - 1.0]", uiTimeFactor);
        tips.push_back(str);
    }

    void GetStatus(TStatus& status) override
    {
        xr_sprintf(status, sizeof(status), "%f", uiTimeFactor);
    }
};

class CCC_InvCellSize : public CCC_Integer
{
protected:
    int* value;
    int min, max;

public:
    int GetValue() const { return *value; }
    void GetBounds(int& imin, int& imax) const
    {
        imin = min;
        imax = max;
    }

    CCC_InvCellSize(pcstr N, int* V, int _min = 0, int _max = 999) : CCC_Integer(N, V, _min, _max), value(V), min(_min){
        bEmptyArgsHandled = true;
        int i = 1;

        string32 section;
        xr_sprintf(section, "inventory_size_%d", i);

        bool section_exist = pSettings->section_exist(section);
        do {
            xr_sprintf(section, "inventory_size_%d", i);

            i++;
            section_exist = pSettings->section_exist(section);
        } while(section_exist);

        max = i - 2;

        Msg("CCC_InvCellSize, i=[%d]", i);
    }

    virtual void Execute(pcstr args)
    {
        int num = atoi(args);
        string32 section;
        xr_sprintf(section, "inventory_size_%d", num);

        bool section_exist = pSettings->section_exist(section);

        if (!section_exist)
        {
            Msg("Section: '%s' not found in system.ltx", section);
            InvalidSyntax();
            return;
        }
        else
            *value = num;

    }
    virtual void GetStatus(TStatus& S) { xr_itoa(*value, S, 10); }
    virtual void Info(TInfo& I) { xr_sprintf(I, sizeof(I), "integer value in range [%d,%d]", min, max); }
    virtual void fill_tips(vecTips& tips, u32 mode)
    {
        TStatus str;
        xr_sprintf(str, sizeof(str), "%d (current) [%d,%d]", *value, min, max);
        tips.push_back(str);
        IConsole_Command::fill_tips(tips, mode);
    }
};

class CCC_LuaProfiler : public IConsole_Command
{
public:
    constexpr static cpcstr COMMAND_LUA_PROFILER_STATUS = "lua_profiler_status";
    constexpr static cpcstr COMMAND_LUA_PROFILER_START = "lua_profiler_start";
    constexpr static cpcstr COMMAND_LUA_PROFILER_START_HOOK_MODE = "lua_profiler_start_hook_mode";
    constexpr static cpcstr COMMAND_LUA_PROFILER_START_SAMPLING_MODE = "lua_profiler_start_sampling_mode";
    constexpr static cpcstr COMMAND_LUA_PROFILER_STOP = "lua_profiler_stop";
    constexpr static cpcstr COMMAND_LUA_PROFILER_RESET = "lua_profiler_reset";
    constexpr static cpcstr COMMAND_LUA_PROFILER_LOG = "lua_profiler_log";
    constexpr static cpcstr COMMAND_LUA_PROFILER_SAVE = "lua_profiler_save";

    CCC_LuaProfiler(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }

    void Execute(pcstr args) override
    {
        CScriptProfiler* profiler = GEnv.ScriptEngine->m_profiler;

        if (strstr(cName, COMMAND_LUA_PROFILER_STATUS) == cName)
        {
            Msg("[P] Profiler status: %s, type - %s", profiler->IsActive() ? "on" : "off",
                profiler->GetTypeString().c_str());
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_START_HOOK_MODE) == cName)
        {
            profiler->StartHookMode();
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_START_SAMPLING_MODE) == cName)
        {
            u32 interval = atoi(args);

            profiler->StartSamplingMode(interval ? interval : CScriptProfiler::PROFILE_SAMPLING_INTERVAL_DEFAULT);
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_START) == cName)
        {
            u32 profiler_type = atoi(args);

            profiler->Start(profiler_type ? (CScriptProfilerType)profiler_type : CScriptProfiler::PROFILE_TYPE_DEFAULT);
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_STOP) == cName)
        {
            profiler->Stop();
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_RESET) == cName)
        {
            profiler->Reset();
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_LOG) == cName)
        {
            u32 limit = atoi(args);

            profiler->LogReport(limit ? limit : CScriptProfiler::PROFILE_ENTRIES_LOG_LIMIT_DEFAULT);
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_SAVE) == cName)
        {
            profiler->SaveReport();
        }
    }

    void fill_tips(vecTips& tips, u32 /*mode*/) override
    {
        TStatus status_buffer;

        if (strstr(cName, COMMAND_LUA_PROFILER_STATUS) == cName)
        {
            // No arguments.
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_START_HOOK_MODE) == cName)
        {
            // No arguments.
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_START_SAMPLING_MODE) == cName)
        {
            xr_sprintf(status_buffer, "%d (default) [1-%d] - sampling interval",
                CScriptProfiler::PROFILE_SAMPLING_INTERVAL_DEFAULT, CScriptProfiler::PROFILE_SAMPLING_INTERVAL_MAX);
            tips.emplace_back(status_buffer);
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_START) == cName)
        {
            xr_sprintf(status_buffer, "%d - hooks based profiler", CScriptProfilerType::Hook);
            tips.emplace_back(status_buffer);

            xr_sprintf(status_buffer, "%d - sampling based profiler", CScriptProfilerType::Sampling);
            tips.emplace_back(status_buffer);
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_STOP) == cName)
        {
            // No arguments.
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_RESET) == cName)
        {
            // No arguments.
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_LOG) == cName)
        {
            xr_sprintf(status_buffer, "%d (default) - count of profiling entries to print",
                CScriptProfiler::PROFILE_ENTRIES_LOG_LIMIT_DEFAULT, CScriptProfiler::PROFILE_SAMPLING_INTERVAL_MAX);
            tips.emplace_back(status_buffer);
        }
        else if (strstr(cName, COMMAND_LUA_PROFILER_SAVE) == cName)
        {
            // No arguments.
        }
    }

    void Info(TInfo& info) override
    {
        if (strstr(cName, COMMAND_LUA_PROFILER_STATUS) == cName)
            xr_strcpy(info, "no arguments : print lua profiler status");
        else if (strstr(cName, COMMAND_LUA_PROFILER_START_HOOK_MODE) == cName)
            xr_strcpy(info, "no arguments : start lua script profiling in hook mode");
        else if (strstr(cName, COMMAND_LUA_PROFILER_START_SAMPLING_MODE) == cName)
            xr_strcpy(info,
                "integer value in range [1,1000] : start lua script profiling in sampling mode with provided sampling "
                "interval");
        else if (strstr(cName, COMMAND_LUA_PROFILER_START) == cName)
            xr_strcpy(info, "integer value in range [0,2] : start lua script profiling in provided mode");
        else if (strstr(cName, COMMAND_LUA_PROFILER_STOP) == cName)
            xr_strcpy(info, "no arguments : stop lua script profiling");
        else if (strstr(cName, COMMAND_LUA_PROFILER_RESET) == cName)
            xr_strcpy(info, "no arguments : reset lua script profiling stats");
        else if (strstr(cName, COMMAND_LUA_PROFILER_LOG) == cName)
            xr_strcpy(info, "integer value : log lua script profiling stats, limit entries with argument");
        else if (strstr(cName, COMMAND_LUA_PROFILER_SAVE) == cName)
            xr_strcpy(info, "no arguments : save lua script profiling stats in a file");
    }
};

// Experimental render switches of xrGame: like CCC_Integer, but not kept in user.ltx (always start at the default)
class CCC_IntegerNotSaved : public CCC_Integer
{
public:
    using CCC_Integer::CCC_Integer;
    void Save(IWriter*) override {}
};

// U1 (plans/optimization_mt/32-frame-spikes.md): lua_udata_profile 0/1 - installs/removes the luabind hook that counts
// the class instances made and samples their Lua call sites (lua_udata_profile.h); 1 starts a new window. U2: the same
// with the LuaJIT hook luaJIT_gw_udata_hook for every userdata (by size, C function, native caller, Lua site)
class CCC_LuaUdataProfile : public CCC_IntegerNotSaved
{
public:
    using CCC_IntegerNotSaved::CCC_IntegerNotSaved;
    void Execute(pcstr args) override
    {
        CCC_IntegerNotSaved::Execute(args);
        gw::lua_udata::Apply(*value != 0);
    }
};

// lua_udata_report [reset] [all | <class>] [top=N] - the "[lua-udata]" and "[lua-udata-all]" report of the window in the
// log (all = only the section of all userdata)
class CCC_LuaUdataReport : public IConsole_Command
{
public:
    CCC_LuaUdataReport(pcstr name) : IConsole_Command(name) { bEmptyArgsHandled = true; }
    void Execute(pcstr args) override { gw::lua_udata::Report(args); }
};

// 47-luabind-value-nogc.md: lua_value_nogc - the emergency switch (not saved, default 1) of the luabind instances that
// hold a trivially destructible value (vector, CTime, matrix... returned by value, vector() / CTime() from Lua) made
// without __gc (luabind set_value_instance_nogc). 0 = with __gc as before, for the instances made after it.
static int g_lua_value_nogc = 1;
class CCC_LuaValueNoGc : public CCC_IntegerNotSaved
{
public:
    using CCC_IntegerNotSaved::CCC_IntegerNotSaved;
    void Execute(pcstr args) override
    {
        CCC_IntegerNotSaved::Execute(args);
        luabind::detail::set_value_instance_nogc(*value != 0);
    }
};

// The same for a float
class CCC_FloatNotSaved : public CCC_Float
{
public:
    using CCC_Float::CCC_Float;
    void Save(IWriter*) override {}
};

// The ranges of the not saved tunables that start from the hardware policy (below)
constexpr int kLuaGcPacedMulMin = 10, kLuaGcPacedMulMax = 1000;
constexpr int kModelPrefetchMarginMin = 0, kModelPrefetchMarginMax = 300;

// plans/optimization_mt/50-smart-engine-hw-profile.md, 5.2 (50h-stage3-game-budgets.md): the not saved tunables start
// at the value of the hardware policy - R17 lua_gc_paced_mul (<= 4 physical cores 100, else 200), R5
// model_prefetch_margin (slow disk 50, else 25); the console changes them for the session as before. The CPU and disk
// fields of the policy are final here (stages A and B run in xrCore::Initialize, before the game module registers its
// commands). No policy (stage A never ran): the defaults of the code stay
static void InitTunablesFromPolicy()
{
    const xray::hw::Policy& policy = xray::hw::GetPolicy();
    if (!xray::hw::PolicyValid(policy))
        return;
    if (policy.lua_paced_mul)
        gw::lua_spike::g_paced_mul = std::clamp(int(policy.lua_paced_mul), kLuaGcPacedMulMin, kLuaGcPacedMulMax);
    g_model_prefetch_margin =
        std::clamp(int(policy.model_prefetch_margin), kModelPrefetchMarginMin, kModelPrefetchMarginMax);
}

// One "* [hw] tunables" line after the registration: the start values of the tunables above, the base of the scheduler
// budget (R16, its cap; the scheduler takes min(shed_budget_ms, it) and logs the result at its first pass) and the time
// limit of cold model loads of the spawn per frame (R6, read from the policy by the spawn queue; 0 = wait for the
// background warm-up of the models)
static void LogTunables()
{
    const xray::hw::Policy& policy = xray::hw::GetPolicy();
    if (!xray::hw::PolicyValid(policy))
    {
        Msg("* [hw] tunables: lua_gc_paced_mul %d, model_prefetch_margin %d (defaults, no policy)",
            gw::lua_spike::g_paced_mul, g_model_prefetch_margin);
        return;
    }
    string32 spawn_cold;
    if (policy.spawn_cold_ms)
        xr_sprintf(spawn_cold, "%u ms", u32(policy.spawn_cold_ms));
    else
        xr_strcpy(spawn_cold, "wait");
    Msg("* [hw] tunables: lua_gc_paced_mul %d, shed budget %u ms, model_prefetch_margin %d, spawn cold %s (policy)",
        gw::lua_spike::g_paced_mul, u32(policy.shed_budget_ms), g_model_prefetch_margin, spawn_cold);
}

void CCC_RegisterCommands()
{
    ZoneScoped;

    // 50-smart-engine-hw-profile.md, 5.2: before the commands of these variables are registered
    InitTunablesFromPolicy();

    // options
    g_OptConCom.Init();

    CMD1(CCC_MemStats, "stat_memory");

    // game
    CMD3(CCC_Mask, "g_crouch_toggle", &psActorFlags, AF_CROUCH_TOGGLE);
    CMD1(CCC_GameDifficulty, "g_game_difficulty");
    CMD1(CCC_GameLanguage, "g_language");
    CMD3(CCC_String, "g_language_ltx", CStringTable::LanguageIDInLTX, std::size(CStringTable::LanguageIDInLTX));

    CMD3(CCC_Mask, "g_backrun", &psActorFlags, AF_RUN_BACKWARD);

    CMD3(CCC_Mask, "g_multi_item_pickup", &psActorFlags, AF_MULTI_ITEM_PICKUP);

    // alife
#ifdef DEBUG
    CMD1(CCC_ALifePath, "al_path"); // build path

#endif // DEBUG

    CMD1(CCC_ALifeSave, "save"); // save game
    CMD1(CCC_ALifeLoadFrom, "load"); // load game from ...
    CMD1(CCC_LoadLastSave, "load_last_save"); // load last saved game from ...

    CMD1(CCC_FlushLog, "flush"); // flush log
    CMD1(CCC_ClearLog, "clear_log");

#ifndef MASTER_GOLD
    CMD1(CCC_ALifeTimeFactor, "al_time_factor"); // set time factor
    CMD1(CCC_ALifeSwitchDistance, "al_switch_distance"); // set switch distance
    CMD1(CCC_ALifeProcessTime, "al_process_time"); // set process time
    CMD1(CCC_ALifeObjectsPerUpdate, "al_objects_per_update"); // set process time
    CMD1(CCC_ALifeSwitchFactor, "al_switch_factor"); // set switch factor
#endif // #ifndef MASTER_GOLD

    CMD3(CCC_Mask, "hud_weapon", &psHUD_Flags, HUD_WEAPON);
    CMD3(CCC_Mask, "hud_info", &psHUD_Flags, HUD_INFO);
    CMD3(CCC_Mask, "hud_draw", &psHUD_Flags, HUD_DRAW);

    // hud
    psHUD_Flags.set(HUD_CROSSHAIR, true);
    psHUD_Flags.set(HUD_WEAPON, true);
    psHUD_Flags.set(HUD_DRAW, true);
    psHUD_Flags.set(HUD_INFO, true);

    CMD3(CCC_Mask, "hud_crosshair", &psHUD_Flags, HUD_CROSSHAIR);
    CMD3(CCC_Mask, "hud_crosshair_dist", &psHUD_Flags, HUD_CROSSHAIR_DIST);
    CMD3(CCC_Mask, "hud_left_handed", &psHUD_Flags, HUD_LEFT_HANDED);

    CMD4(CCC_Float, "hud_fov", &psHUD_FOV_def, 0.1f, 1.0f);
    CMD4(CCC_Float, "fov", &g_fov, 5.0f, 180.0f);
    CMD1(CCC_ScopeHudFov, "g_w_scope_hud_fov");
    CMD1(CCC_WeaponHudFov, "weapon_hud");

    // Demo
    CMD1(CCC_DemoPlay, "demo_play");
    CMD1(CCC_DemoRecord, "demo_record");
    CMD1(CCC_DemoRecordSetPos, "demo_set_cam_position");

#ifndef MASTER_GOLD
    // ai
    // Bit kept for console compatibility, no effect: NPC vision always goes through the shared batch
    // (g_vision_batch, CLevel::VisionBatchPostScheduler), the vanilla seqParallel Exec_Visibility is not used.
    CMD3(CCC_Mask, "mt_ai_vision", &g_mt_config, mtAiVision);
    CMD3(CCC_Mask, "mt_level_path", &g_mt_config, mtLevelPath);
    CMD3(CCC_Mask, "mt_detail_path", &g_mt_config, mtDetailPath);
    CMD3(CCC_Mask, "mt_object_handler", &g_mt_config, mtObjectHandler);
    CMD3(CCC_Mask, "mt_sound_player", &g_mt_config, mtSoundPlayer);
    CMD3(CCC_Mask, "mt_bullets", &g_mt_config, mtBullets);
    CMD3(CCC_Mask, "mt_script_gc", &g_mt_config, mtLUA_GC);
    CMD3(CCC_Mask, "mt_level_sounds", &g_mt_config, mtLevelSounds);
    CMD3(CCC_Mask, "mt_alife", &g_mt_config, mtALife);
    CMD3(CCC_Mask, "mt_map", &g_mt_config, mtMap);
    CMD3(CCC_Mask, "map_spot_smooth", &g_map_spot_config, MAP_SPOT_SMOOTH);
    CMD4(CCC_Float, "map_spot_smooth_speed", &ps_map_spot_smooth_speed, 0.01f, 100.f);
#endif // MASTER_GOLD

#ifndef MASTER_GOLD
    CMD3(CCC_Mask, "ai_obstacles_avoiding", &psAI_Flags, aiObstaclesAvoiding);
    CMD3(CCC_Mask, "ai_obstacles_avoiding_static", &psAI_Flags, aiObstaclesAvoidingStatic);
    CMD3(CCC_Mask, "ai_use_smart_covers", &psAI_Flags, aiUseSmartCovers);
    CMD3(CCC_Mask, "ai_use_smart_covers_animation_slots", &psAI_Flags, (u32)aiUseSmartCoversAnimationSlot);
    CMD4(CCC_Float, "ai_smart_factor", &g_smart_cover_factor, 0.f, 1000000.f);
#endif // MASTER_GOLD

    CMD3(CCC_Mask, "lua_debug", &g_LuaDebug, 1);
    CMD4(CCC_Integer, "lua_dump_depth", &g_LuaDumpDepth, 0, 16);

    CMD1(CCC_PrintNpcProfiles, "debug_print_npc_profiles");

    CMD1(CCC_LuaProfiler, CCC_LuaProfiler::COMMAND_LUA_PROFILER_STATUS);
    CMD1(CCC_LuaProfiler, CCC_LuaProfiler::COMMAND_LUA_PROFILER_START);
    CMD1(CCC_LuaProfiler, CCC_LuaProfiler::COMMAND_LUA_PROFILER_START_SAMPLING_MODE);
    CMD1(CCC_LuaProfiler, CCC_LuaProfiler::COMMAND_LUA_PROFILER_START_HOOK_MODE);
    CMD1(CCC_LuaProfiler, CCC_LuaProfiler::COMMAND_LUA_PROFILER_STOP);
    CMD1(CCC_LuaProfiler, CCC_LuaProfiler::COMMAND_LUA_PROFILER_RESET);
    CMD1(CCC_LuaProfiler, CCC_LuaProfiler::COMMAND_LUA_PROFILER_LOG);
    CMD1(CCC_LuaProfiler, CCC_LuaProfiler::COMMAND_LUA_PROFILER_SAVE);

    CMD1(CCC_LuaGCMethod, "lua_gc_method");
    CMD4(CCC_Integer, "lua_gcstep", &psLUA_GCSTEP, 1, 1000);
    CMD4(CCC_Integer, "lua_gc_timeout", &psLUA_GCTIMEOUT, 1000, 16000);
    // P18 (plans/optimization_mt/13-lua-spikes.md). Temporary, not saved. A Lua call of the engine (enemy callback,
    // event bus, script_call, binder update, GC step) longer than this many us goes to the log as "[lua-spike]" with the
    // heap before/after and whether the atomic phase of the GC ran inside it; 0 = off
    CMD4(CCC_IntegerNotSaved, "npc_perf_lua_spike_us", &gw::lua_spike::g_threshold_us, 0, 1000000);
    // With lua_gc_method gc_step the GC always works only in script_gc, paced by the allocations of the frame (the
    // implicit steps inside random Lua calls are held off): % of the LuaJIT pace (one step per KB); starts at the
    // hardware policy (R17, InitTunablesFromPolicy)
    CMD4(CCC_IntegerNotSaved, "lua_gc_paced_mul", &gw::lua_spike::g_paced_mul, kLuaGcPacedMulMin, kLuaGcPacedMulMax);
    // 26-framemove-game-next.md, package 2-A. Temporary A/B switches, not saved, 0 = as before.
    // K2a: > 0 - the paced GC steps of a frame stop after this many us, the rest of the debt goes to the next frames
    // (a debt over ~16 MB of allocations is paid off uncapped); one step (the atomic phase) is never cut
    CMD4(CCC_IntegerNotSaved, "lua_gc_step_budget_us", &gw::lua_spike::g_step_budget_us, 0, 100000);
    // G1 (32-frame-spikes.md), with lua_gc_step_budget_us > 0: > 0 - the cap of a frame grows with the debt,
    // clamp(base * (1 + debt / lua_gc_debt_soft), base, lua_gc_budget_max_us), instead of the K2a cliff; uncapped only
    // past 64K steps of debt or when the heap passed lua_gc_heap_guard_pct % of its size at the cycle start (0 = no
    // heap guard). lua_gc_budget_max_us 0 = K2a as before
    CMD4(CCC_IntegerNotSaved, "lua_gc_budget_max_us", &gw::lua_spike::g_budget_max_us, 0, 100000);
    CMD4(CCC_IntegerNotSaved, "lua_gc_debt_soft", &gw::lua_spike::g_debt_soft, 100, 1000000);
    CMD4(CCC_IntegerNotSaved, "lua_gc_heap_guard_pct", &gw::lua_spike::g_heap_guard_pct, 0, 1000);
    // G2 (32-frame-spikes.md; temporary, not saved). Needs LuaJIT with the GlobalWar patch (luajit.h LUAJIT_GW_GCSTATS).
    // lua_gc_atomic_stats 1: a "[lua-gc-atomic]" line after every atomic phase of the GC, each part timed with what it
    // walked (lua_spike_probe.h); lua_heap_census [full]: the census of the Lua heap ("[lua-census]" lines)
    CMD4(CCC_IntegerNotSaved, "lua_gc_atomic_stats", &gw::lua_spike::g_atomic_stats, 0, 1);
    CMD1(CCC_LuaHeapCensus, "lua_heap_census");
    // A/B, 0 = untouched: lua_gc_pause - LUA_GCSETPAUSE, and the paced GC waits between the cycles until the heap passes
    // this % of the heap at the end of the last one (100 = no wait); lua_gc_stepmul - LUA_GCSETSTEPMUL (LuaJIT: 200)
    CMD4(CCC_IntegerNotSaved, "lua_gc_pause", &gw::lua_spike::g_gc_pause, 0, 1000);
    CMD4(CCC_IntegerNotSaved, "lua_gc_stepmul", &gw::lua_spike::g_gc_stepmul, 0, 10000);
    // 42-lua-gc-prewalk.md: the work moved out of the atomic phase of the GC into the steps (LuaJIT luaJIT_gw_prewalk)
    // is always on since 2026-10-18 (its emergency switch lua_gc_prewalk removed, see plan 41)
    // U1 (32-frame-spikes.md; diagnostics, not saved): who makes the temporary luabind userdata. lua_udata_profile 1 -
    // count the class instances made (exact, by class) and sample the Lua call site of one in lua_udata_profile_sample
    // (source:line, its caller, the C function, the Lua callback about to be called); lua_udata_report [reset]
    // [<class>] [top=N] - the top sites by class and over all classes in the log. 0 = off (luabind: one load, a branch)
    // U2: lua_udata_profile 1 also hooks every userdata in LuaJIT (lj_udata_new); lua_udata_report adds the section
    // "[lua-udata-all]" (all userdata vs the luabind instances; by size, C function, native caller, Lua site), "all" = only it
    CMD4(CCC_LuaUdataProfile, "lua_udata_profile", &gw::lua_udata::g_enabled, 0, 1);
    CMD4(CCC_IntegerNotSaved, "lua_udata_profile_sample", &gw::lua_udata::g_sample, 1, 65536);
    CMD1(CCC_LuaUdataReport, "lua_udata_report");
    // 32-frame-spikes.md: the Lua wrapper cache (xrScriptEngine/script_wrapper_cache.hpp) and the luabind instances
    // without __gc over a raw pointer (game_object, alife(), device(), a reference returned by a method: they own
    // nothing; the GC frees them in the cycle they die in instead of queueing them for an empty finalizer) are always
    // on since 2026-10-18 (their switches lua_wrapper_cache, lua_wrapper_nogc removed, 41-cvar-cleanup.md)
    luabind::detail::set_nonowning_instance_nogc(true);
    // 47-luabind-value-nogc.md: the same for the value instances of trivially destructible types (emergency switch)
    CMD4(CCC_LuaValueNoGc, "lua_value_nogc", &g_lua_value_nogc, 0, 1);
    luabind::detail::set_value_instance_nogc(g_lua_value_nogc != 0);
    // 26-framemove-game-next.md: K2b, K3, K4 (Level.cpp), the pipelined bones prepass and its early kick (Level.cpp) and
    // K9 (xrEngine/EngineThreading.cpp) are always on since 2026-10-18 (their switches lua_gc_overlap,
    // mt_sound_dispatch_early, mt_prepass_main_slot, r__bones_prepass_pipeline, r__bones_prepass_early_kick,
    // mt_sound_render_task removed)
    // 29-game-thread-next.md (the Game thread on the critical path). A1 (mt_phase0_split, the phase 0 dispatch as a
    // task beside the Lua GC step) removed 2026-10-18: no gain (41-cvar-cleanup.md); phase 0 dispatches on the Game
    // thread.
    // Always on since 2026-10-18 (41-cvar-cleanup.md, step 2; their switches removed): A3 mt_sound_render_early - the
    // K9 sound render task starts in phase 0 right after its dispatch (xrEngine/EngineThreading.cpp); A2
    // mt_bullets_task - with mt_bullets the bullets' UpdateWorkload runs as a task from the start of the Game thread's
    // logic frame to its old place in seqParallel (Level_Bullet_Manager.cpp); A4 mt_game_wait_own_only - a Wait() of
    // the Game thread runs only its own tasks and the pieces of the awaited one, after 1 ms without own work it steals
    // (TaskManager.cpp).
    // 26-framemove-game-next.md, the Game thread after package 2: temporary A/B switches, not saved, 0 = as before.
    // net_sp_export_slices N > 1: the once-a-second single player object export to the server (CLevel::net_Update,
    // seqFrameMT, a 4.5-5 ms spike) spread over N frames (Level_network.cpp)
    extern int net_sp_export_slices;
    CMD4(CCC_IntegerNotSaved, "net_sp_export_slices", &net_sp_export_slices, 0, 32);
    // 26-framemove-game-next.md: the corpse reaction fix (agent_corpse_manager.cpp), the cheaper enemy distribution,
    // the enemy masks batch (agent_enemy_manager.cpp), the agent group by the ALife squad (Entity.cpp) and the cover scan
    // cache (space_restriction.cpp) are always on since 2026-10-18 (their switches npc_corpse_reaction_fix,
    // npc_agent_distribute_fast, npc_agent_masks_batch, npc_agent_group_by_squad, npc_cover_scan_cache 2 removed)
    // npc_cover_scan_frame_budget_us > 0: the cover searches of stalkers without a cover stop for the frame once they
    // took this many us; the rest get "no cover" for this update and search on the next one (ai_stalker_cover.cpp)
    extern int npc_cover_scan_frame_budget_us;
    CMD4(CCC_IntegerNotSaved, "npc_cover_scan_frame_budget_us", &npc_cover_scan_frame_budget_us, 0, 100000);
    // npc_agent_distribute_min_ms N > 0: the distribution a member without an enemy asks for on its memory update runs
    // at most once in N ms unless an enemy died/went offline or a new one was selected (agent_enemy_manager.cpp)
    extern int npc_agent_distribute_min_ms;
    CMD4(CCC_IntegerNotSaved, "npc_agent_distribute_min_ms", &npc_agent_distribute_min_ms, 0, 5000);
    // 32-frame-spikes.md, A1 (always on since 2026-10-18, its switch alife_update_spread removed): the ALife update of a
    // turn of the scheduler (18-35 ms once in ~5 s) is done in parts every frame, alife_scheduled_budget_us a frame (at
    // least one object); alife_update_manager.cpp
    extern int alife_scheduled_budget_us;
    CMD4(CCC_IntegerNotSaved, "alife_scheduled_budget_us", &alife_scheduled_budget_us, 0, 100000);
    // alife_switch_offline_per_frame_max N > 0: the spread switch pass takes at most ~N client objects (with the NPC
    // inventories) offline a frame, at least one object/squad; 0 = no limit (alife_update_manager.cpp)
    extern int alife_switch_offline_per_frame_max;
    CMD4(CCC_IntegerNotSaved, "alife_switch_offline_per_frame_max", &alife_switch_offline_per_frame_max, 0, 100000);
    // S1: the Lua tunable of the smarts (see npc_perf_smart_spawn_gap_ms above)
    CMD4(CCC_IntegerNotSaved, "npc_perf_smart_spawn_gap_ms", &npc_perf_smart_spawn_gap_ms, 0, 60000);
    // 33-level-path.md (plans/optimization_mt): the level paths of the NPCs (the fast level A*, the vertex lookup on the
    // row table, the cross table and the game graph in the memory - always on since 2026-10-18, their switches
    // ai_level_search_fast, ai_level_vertex_fast, ai_cross_table_copy, ai_game_graph_copy removed, xrAICore
    // ai_graph_engine_cvars.h). ai_level_path_spike_us N - a [path-spike] line for a level path delegate over N us;
    // ai_level_path_budget_us N - the level path delegates over N us a frame wait for the next frame (the first of a
    // frame always runs; movement_manager.cpp); ai_obstacle_path_visited_cap N - the stalkers' searches with obstacles
    // open at most N vertices until the one without obstacles found a path; ai_obstacle_path_iters_max N - at most N
    // passes of the obstacle loop, then the path without obstacles (stalker_movement_manager_obstacles_path.cpp)
    extern int ai_level_path_spike_us;
    extern int ai_level_path_budget_us;
    extern int ai_obstacle_path_visited_cap;
    extern int ai_obstacle_path_iters_max;
    CMD4(CCC_IntegerNotSaved, "ai_level_path_spike_us", &ai_level_path_spike_us, 0, 1000000);
    CMD4(CCC_IntegerNotSaved, "ai_level_path_budget_us", &ai_level_path_budget_us, 0, 100000);
    CMD4(CCC_IntegerNotSaved, "ai_obstacle_path_visited_cap", &ai_obstacle_path_visited_cap, 0, 65500);
    CMD4(CCC_IntegerNotSaved, "ai_obstacle_path_iters_max", &ai_obstacle_path_iters_max, 0, 64);

#ifdef DEBUG
    CMD3(CCC_Mask, "ai_debug", &psAI_Flags, aiDebug);
    CMD3(CCC_Mask, "ai_dbg_brain", &psAI_Flags, aiBrain);
    CMD3(CCC_Mask, "ai_dbg_motion", &psAI_Flags, aiMotion);
    CMD3(CCC_Mask, "ai_dbg_frustum", &psAI_Flags, aiFrustum);
    CMD3(CCC_Mask, "ai_dbg_funcs", &psAI_Flags, aiFuncs);
    CMD3(CCC_Mask, "ai_dbg_alife", &psAI_Flags, aiALife);
    CMD3(CCC_Mask, "ai_dbg_goap", &psAI_Flags, aiGOAP);
    CMD3(CCC_Mask, "ai_dbg_goap_script", &psAI_Flags, aiGOAPScript);
    CMD3(CCC_Mask, "ai_dbg_goap_object", &psAI_Flags, aiGOAPObject);
    CMD3(CCC_Mask, "ai_dbg_cover", &psAI_Flags, aiCover);
    CMD3(CCC_Mask, "ai_dbg_anim", &psAI_Flags, aiAnimation);
    CMD3(CCC_Mask, "ai_dbg_vision", &psAI_Flags, aiVision);
    CMD3(CCC_Mask, "ai_dbg_monster", &psAI_Flags, aiMonsterDebug);
    CMD3(CCC_Mask, "ai_dbg_stalker", &psAI_Flags, aiStalker);
    CMD3(CCC_Mask, "ai_stats", &psAI_Flags, aiStats);
    CMD3(CCC_Mask, "ai_dbg_destroy", &psAI_Flags, aiDestroy);
    CMD3(CCC_Mask, "ai_dbg_serialize", &psAI_Flags, aiSerialize);
    CMD3(CCC_Mask, "ai_dbg_dialogs", &psAI_Flags, aiDialogs);
    CMD3(CCC_Mask, "ai_dbg_infoportion", &psAI_Flags, aiInfoPortion);

    CMD3(CCC_Mask, "ai_draw_game_graph", &psAI_Flags, aiDrawGameGraph);
    CMD3(CCC_Mask, "ai_draw_game_graph_stalkers", &psAI_Flags, aiDrawGameGraphStalkers);
    CMD3(CCC_Mask, "ai_draw_game_graph_objects", &psAI_Flags, aiDrawGameGraphObjects);
    CMD3(CCC_Mask, "ai_draw_game_graph_real_pos", &psAI_Flags, aiDrawGameGraphRealPos);

    // XXX: register from script engine
    // CMD3(CCC_Mask,				"lua_nil_object_access",	&psAI_Flags,	aiNilObjectAccess);

    CMD3(CCC_Mask, "ai_draw_visibility_rays", &psAI_Flags, aiDrawVisibilityRays);
    CMD3(CCC_Mask, "ai_animation_stats", &psAI_Flags, aiAnimationStats);

    /////////////////////////////////////////////HIT ANIMATION////////////////////////////////////////////////////
    // float						power_factor				= 2.f;
    // float						rotational_power_factor		= 3.f;
    // float						side_sensitivity_threshold	= 0.2f;
    // float						anim_channel_factor			= 3.f;

    CMD4(CCC_Float, "hit_anims_power", &ghit_anims_params.power_factor, 0.0f, 100.0f);
    CMD4(CCC_Float, "hit_anims_rotational_power", &ghit_anims_params.rotational_power_factor, 0.0f, 100.0f);
    CMD4(CCC_Float, "hit_anims_side_sensitivity_threshold", &ghit_anims_params.side_sensitivity_threshold, 0.0f, 10.0f);
    CMD4(CCC_Float, "hit_anims_channel_factor", &ghit_anims_params.anim_channel_factor, 0.0f, 100.0f);
    // float	block_blend					= 0.1f;
    // float	reduce_blend				= 0.5f;
    // float	reduce_power_factor			= 0.5f;
    CMD4(CCC_Float, "hit_anims_block_blend", &ghit_anims_params.block_blend, 0.f, 1.f);
    CMD4(CCC_Float, "hit_anims_reduce_blend", &ghit_anims_params.reduce_blend, 0.f, 1.f);
    CMD4(CCC_Float, "hit_anims_reduce_blend_factor", &ghit_anims_params.reduce_power_factor, 0.0f, 1.0f);
    CMD4(CCC_Integer, "hit_anims_tune", &tune_hit_anims, 0, 1);
/////////////////////////////////////////////HIT ANIMATION END////////////////////////////////////////////////////

    CMD1(CCC_DumpModelBones, "debug_dump_model_bones");

    CMD1(CCC_DrawGameGraphAll, "ai_draw_game_graph_all");
    CMD1(CCC_DrawGameGraphCurrent, "ai_draw_game_graph_current_level");
    CMD1(CCC_DrawGameGraphLevel, "ai_draw_game_graph_level");

    CMD4(CCC_Integer, "ai_dbg_inactive_time", &g_AI_inactive_time, 0, 1000000);

    CMD1(CCC_DebugNode, "ai_dbg_node");
#if defined(USE_DEBUGGER)
    CMD1(CCC_ScriptDbg, "script_debug_break");
    CMD1(CCC_ScriptDbg, "script_debug_stop");
    CMD1(CCC_ScriptDbg, "script_debug_restart");
#endif // #if defined(USE_DEBUGGER)

    CMD1(CCC_ShowMonsterInfo, "ai_monster_info");
    CMD1(CCC_DebugFonts, "debug_fonts");

    CMD1(CCC_ShowAnimationStats, "ai_show_animation_stats");
#endif // DEBUG

    // Performance / NPC throttling and cache (user.ltx)
    CMD4(CCC_Integer, "ai_evaluator_ttl_ms", &ai_evaluator_ttl_ms, 0, 1000);
    CMD4(CCC_Integer, "npc_anim_lod_far_interval_ms", &npc_anim_lod_far_interval_ms, 1, 5000);
    CMD4(CCC_Float, "npc_perf_planner_near_dist", &npc_perf_planner_near_dist, 1.f, 2000.f);
    CMD4(CCC_Float, "npc_perf_planner_medium_dist", &npc_perf_planner_medium_dist, 1.f, 2000.f);
    CMD4(CCC_Integer, "npc_perf_planner_solve_interval_near_idle_ms", (int*)&npc_perf_planner_solve_interval_near_idle_ms, 10, 60000);
    CMD4(CCC_Integer, "npc_perf_planner_solve_interval_medium_ms", (int*)&npc_perf_planner_solve_interval_medium_ms, 10, 60000);
    CMD4(CCC_Integer, "npc_perf_planner_solve_interval_far_ms", (int*)&npc_perf_planner_solve_interval_far_ms, 10, 60000);
    CMD4(CCC_Integer, "npc_perf_planner_solve_interval_combat_near_ms", (int*)&npc_perf_planner_solve_interval_combat_near_ms, 10, 60000);
    CMD4(CCC_Integer, "npc_perf_planner_solve_interval_danger_only_ms", (int*)&npc_perf_planner_solve_interval_danger_only_ms, 0, 60000);
    CMD4(CCC_Integer, "npc_perf_planner_actuality_interval_combat_ms", (int*)&npc_perf_planner_actuality_interval_combat_ms, 10, 60000);
    CMD4(CCC_Integer, "npc_perf_planner_actuality_interval_danger_ms", (int*)&npc_perf_planner_actuality_interval_danger_ms, 10, 60000);
    CMD4(CCC_Integer, "npc_perf_planner_actuality_interval_near_idle_ms", (int*)&npc_perf_planner_actuality_interval_near_idle_ms, 10, 60000);
    CMD4(CCC_Integer, "npc_perf_planner_graph_search_max_nodes", (int*)&npc_perf_planner_graph_search_max_nodes, 100, 100000);
    CMD4(CCC_Integer, "npc_perf_planner_graph_search_max_nodes_combat", (int*)&npc_perf_planner_graph_search_max_nodes_combat, 0, 100000);
    CMD4(CCC_Integer, "npc_perf_planner_graph_search_max_nodes_danger", (int*)&npc_perf_planner_graph_search_max_nodes_danger, 0, 100000);
    CMD4(CCC_Integer, "npc_perf_planner_nested_graph_search_max_nodes", (int*)&g_ai_nested_planner_graph_search_max_nodes, 100,
        32000);
    CMD4(CCC_Integer, "ai_graph_engine_serialize", &ps_ai_graph_engine_serialize, 0, 1);
    CMD4(CCC_Integer, "ai_graph_engine_detect_concurrent", &ps_ai_graph_engine_detect_concurrent, 0, 1);
    CMD4(CCC_Integer, "ai_patrol_remap_log", &ps_ai_patrol_remap_log, 0, 1);
    CMD4(CCC_Integer, "npc_perf_cover_best_max_evaluate", &npc_perf_cover_best_max_evaluate, 0, 4096);
    CMD4(CCC_Integer, "npc_perf_cover_nearest_max_points", &npc_perf_cover_nearest_max_points, 0, 4096);
    CMD4(CCC_Integer, "npc_perf_cover_best_max_accessible", &npc_perf_cover_best_max_accessible, 0, 4096);
    CMD4(CCC_Integer, "npc_perf_cover_find_eval_budget_total", &npc_perf_cover_find_eval_budget_total, 0, 4096);
    CMD4(CCC_Integer, "npc_perf_long_dead_skip_binder_ms", (int*)&npc_perf_long_dead_skip_binder_ms, 0, 120000);
    CMD4(CCC_Float, "npc_perf_binder_far_dist", &npc_perf_binder_far_dist, 1.f, 2000.f);
    CMD4(CCC_Integer, "npc_perf_binder_far_interval_ms", (int*)&npc_perf_binder_far_interval_ms, 10, 10000);
    CMD4(CCC_Integer, "npc_perf_binder_far_phases", (int*)&npc_perf_binder_far_phases, 1, 64);
    CMD4(CCC_Integer, "npc_perf_binder_near_interval_ms", (int*)&npc_perf_binder_near_interval_ms, 10, 5000);
    CMD4(CCC_Integer, "npc_perf_binder_far_throttle_ms", (int*)&npc_perf_binder_far_throttle_ms, 50, 10000);
    CMD4(CCC_Integer, "npc_perf_stalker_vis_interval_near_ms", &npc_perf_stalker_vis_interval_near_ms, 0, 30000);
    CMD4(CCC_Integer, "npc_perf_stalker_vis_interval_medium_ms", &npc_perf_stalker_vis_interval_medium_ms, 10, 30000);
    CMD4(CCC_Integer, "npc_perf_stalker_vis_interval_far_ms", &npc_perf_stalker_vis_interval_far_ms, 10, 30000);
    // When 1, never use character m_lod in renderer (hi mesh only). Checked before r_force_character_lod and corpse LOD.
    CMD4(CCC_Integer, "r_disable_character_lod", &ps_r_disable_character_lod_render, 0, 1);
    // Force render m_lod for all CEntityAlive (NPC/mutant/actor) when available.
    CMD4(CCC_Integer, "r_force_character_lod", &ps_force_character_lod_render, 0, 1);
    // Ms after death before a corpse stops recalculating its bones. 0 = never freeze (a corpse frozen while the
    // ragdoll is still moving stays stuck mid-air).
    CMD4(CCC_Integer, "r_corpse_pose_freeze_ms", (int*)&ps_r_corpse_pose_freeze_ms, 0, 120000);
    CMD4(CCC_Integer, "npc_perf_vision_parallel_batch_min_rays", &npc_perf_vision_parallel_batch_min_rays, 0, 10000);
    CMD4(CCC_Float, "npc_perf_vision_small_physics_radius", &npc_perf_vision_small_physics_radius, 0.f, 10.f);
    CMD4(CCC_Float, "npc_perf_vision_cache_pos_slack_m", &npc_perf_vision_cache_pos_slack_m, 0.f, 5.f);
    CMD4(CCC_Integer, "npc_perf_vision_rays_per_npc", &npc_perf_vision_rays_per_npc, 0, 256);
    CMD4(CCC_Float, "npc_perf_monster_vis_near_dist", &npc_perf_monster_vis_near_dist, 1.f, 500.f);
    CMD4(CCC_Float, "npc_perf_monster_vis_medium_dist", &npc_perf_monster_vis_medium_dist, 1.f, 500.f);
    CMD4(CCC_Integer, "npc_perf_monster_vis_interval_medium_ms", (int*)&npc_perf_monster_vis_interval_medium_ms, 10, 30000);
    CMD4(CCC_Integer, "npc_perf_monster_vis_interval_far_ms", (int*)&npc_perf_monster_vis_interval_far_ms, 10, 30000);
    CMD4(CCC_Integer, "npc_perf_ik_interval_near_idle_ms", (int*)&npc_perf_ik_interval_near_idle_ms, 10, 60000);
    CMD4(CCC_Integer, "npc_perf_ik_interval_medium_ms", (int*)&npc_perf_ik_interval_medium_ms, 10, 60000);
    CMD4(CCC_Integer, "npc_perf_ik_interval_far_ms", (int*)&npc_perf_ik_interval_far_ms, 10, 60000);
    CMD4(CCC_Integer, "npc_perf_ik_interval_disabled_ms", (int*)&npc_perf_ik_interval_disabled_ms, 1000, 120000);
    CMD4(CCC_Integer, "npc_perf_ik_interval_enemy_selected_ms", (int*)&npc_perf_ik_interval_enemy_selected_ms, 0, 60000);
    CMD4(CCC_Integer, "npc_perf_ik_foot_raypick_batch_min_rays", &npc_perf_ik_foot_raypick_batch_min_rays, 0, 128);
    CMD4(CCC_Integer, "npc_perf_state_mgr_animstate_ttl_ms", (int*)&npc_perf_state_mgr_animstate_ttl_ms, 10, 30000);
    CMD4(CCC_Integer, "npc_perf_script_combat_ttl_ms", (int*)&npc_perf_script_combat_ttl_ms, 10, 30000);
    CMD4(CCC_Integer, "npc_perf_evaluator_combat_enemy_cache_ttl_ms", (int*)&npc_perf_evaluator_combat_enemy_cache_ttl_ms, 10, 30000);
    CMD4(CCC_Integer, "npc_perf_danger_eval_ttl_ms", (int*)&npc_perf_danger_eval_ttl_ms, 10, 30000);
    CMD4(CCC_Integer, "npc_perf_script_danger_eval_ttl_ms", (int*)&npc_perf_script_danger_eval_ttl_ms, 10, 30000);
    CMD4(CCC_Integer, "npc_perf_state_mgr_idle_combat_ttl_ms", (int*)&npc_perf_state_mgr_idle_combat_ttl_ms, 10, 30000);
    CMD4(CCC_Integer, "npc_perf_state_mgr_end_ttl_ms", (int*)&npc_perf_state_mgr_end_ttl_ms, 10, 30000);
    CMD4(CCC_Integer, "npc_perf_planner_update_min_interval_ms", (int*)&npc_perf_planner_update_min_interval_ms, 10, 10000);
    CMD4(CCC_Integer, "npc_perf_planner_update_min_interval_far_ms", (int*)&npc_perf_planner_update_min_interval_far_ms, 10, 10000);
    CMD4(CCC_Integer, "npc_perf_evaluator_abuse_ttl_ms", (int*)&npc_perf_evaluator_abuse_ttl_ms, 10, 30000);
    CMD4(CCC_Integer, "npc_perf_gather_items_eval_ttl_ms", (int*)&npc_perf_gather_items_eval_ttl_ms, 10, 30000);
    CMD4(CCC_Integer, "npc_perf_state_mgr_movement_ttl_ms", (int*)&npc_perf_state_mgr_movement_ttl_ms, 10, 30000);
    CMD4(CCC_Integer, "npc_perf_motivator_medium_interval", (int*)&npc_perf_motivator_medium_interval, 100, 120000);
    CMD4(CCC_Integer, "npc_perf_motivator_far_interval", (int*)&npc_perf_motivator_far_interval, 100, 120000);
    CMD4(CCC_Integer, "npc_perf_motivator_slow_near_interval", (int*)&npc_perf_motivator_slow_near_interval, 100, 120000);
    CMD4(CCC_Integer, "npc_perf_motivator_slow_medium_interval", (int*)&npc_perf_motivator_slow_medium_interval, 100, 120000);
    CMD4(CCC_Integer, "npc_perf_motivator_slow_far_interval", (int*)&npc_perf_motivator_slow_far_interval, 100, 120000);
    CMD4(CCC_Integer, "npc_perf_motivator_switch_near_interval", (int*)&npc_perf_motivator_switch_near_interval, 100, 120000);
    CMD4(CCC_Integer, "npc_perf_motivator_switch_medium_interval", (int*)&npc_perf_motivator_switch_medium_interval, 100, 120000);
    CMD4(CCC_Integer, "npc_perf_motivator_switch_far_interval", (int*)&npc_perf_motivator_switch_far_interval, 100, 120000);
    CMD4(CCC_Integer, "npc_perf_motivator_callback_near_interval", (int*)&npc_perf_motivator_callback_near_interval, 100, 120000);
    CMD4(CCC_Integer, "npc_perf_motivator_callback_medium_interval", (int*)&npc_perf_motivator_callback_medium_interval, 100, 120000);
    CMD4(CCC_Integer, "npc_perf_motivator_callback_far_interval", (int*)&npc_perf_motivator_callback_far_interval, 100, 120000);
    CMD4(CCC_Integer, "npc_perf_motivator_dead_interval", (int*)&npc_perf_motivator_dead_interval, 100, 120000);
    CMD4(CCC_Integer, "npc_perf_motivator_native_gate", (int*)&npc_perf_motivator_native_gate, 0, 1);
    CMD4(CCC_Integer, "npc_perf_sim_brain_actor_update_interval", (int*)&npc_perf_sim_brain_actor_update_interval, 1000, 120000);
    CMD4(CCC_Integer, "npc_perf_agent_enemy_fill_squads_trace_members", &npc_perf_agent_enemy_fill_squads_trace_members, 0, 1);
    CMD4(CCC_Integer, "npc_perf_visible_value_native", &npc_perf_visible_value_native, 0, 1);
    CMD4(CCC_Integer, "npc_perf_condlist_native", &npc_perf_condlist_native, 0, 2);
    CMD4(CCC_Integer, "npc_perf_ik_enemy_near_dist", &npc_perf_ik_enemy_near_dist, 0, 200);
    CMD4(CCC_Integer, "npc_perf_ik_offscreen_interval_ms", &npc_perf_ik_offscreen_interval_ms, 0, 30000);
    CMD4(CCC_Integer, "npc_perf_ucl_lod_dist", &npc_perf_ucl_lod_dist, 5, 500);
    CMD4(CCC_Integer, "npc_perf_ucl_lod_dist2", &npc_perf_ucl_lod_dist2, 0, 500);
    CMD4(CCC_Integer, "npc_perf_ucl_lod_max_delta_ms", &npc_perf_ucl_lod_max_delta_ms, 100, 500);
    // 24-critical-path.md, F2: the combat tier of the UpdateCL LOD (always on), a full UpdateCL every N-th frame
    // (0 / 1 = every frame)
    CMD4(CCC_Integer, "npc_ucl_lod_combat", &npc_ucl_lod_combat, 0, 4);
    // 22-game-thread.md, step 1: the tunables of the object handler LOD and of the corpse scheduler interval
    CMD4(CCC_Integer, "npc_perf_object_handler_lod_dist", &npc_perf_object_handler_lod_dist, 5, 500);
    CMD4(CCC_Integer, "npc_perf_object_handler_lod_every_seen", &npc_perf_object_handler_lod_every_seen, 1, 16);
    CMD4(CCC_Integer, "npc_perf_object_handler_lod_every_unseen", &npc_perf_object_handler_lod_every_unseen, 1, 32);
    CMD4(CCC_Integer, "npc_perf_object_handler_lod_max_ms", &npc_perf_object_handler_lod_max_ms, 50, 1000);
    CMD4(CCC_Integer, "npc_perf_object_handler_lod_pick_ms", &npc_perf_object_handler_lod_pick_ms, 0, 500);
    // 24-critical-path.md, G2: the tunables of the friendly-fire pick cache (G2a) and cut (G2b), both always on
    CMD4(CCC_Integer, "npc_ff_pick_cache_ms", &npc_ff_pick_cache_ms, 20, 500);
    CMD4(CCC_Integer, "npc_ff_pick_cache_frames", &npc_ff_pick_cache_frames, 1, 8);
    CMD4(CCC_Integer, "npc_ff_pick_cache_actor_dist", &npc_ff_pick_cache_actor_dist, 0, 50);
    CMD4(CCC_Integer, "npc_ff_pick_cut_behind", &npc_ff_pick_cut_behind, 1, 50);
    CMD4(CCC_Integer, "npc_perf_dead_shed_interval_ms", &npc_perf_dead_shed_interval_ms, 1000, 8000);
    CMD4(CCC_Integer, "npc_perf_move_try_skip_frames", &npc_perf_move_try_skip_frames, 0, 8);
    CMD4(CCC_Integer, "npc_perf_move_try_skip_frames_far", &npc_perf_move_try_skip_frames_far, 0, 16);
    CMD4(CCC_Integer, "npc_perf_move_try_skip_far_dist", &npc_perf_move_try_skip_far_dist, 0, 500);
    CMD4(CCC_Integer, "npc_perf_move_try_skip_recover", &npc_perf_move_try_skip_recover, 1, 16);
    CMD4(CCC_Integer, "npc_perf_move_try_skip_actor_dist", &npc_perf_move_try_skip_actor_dist, 0, 100);
    CMD4(CCC_Integer, "npc_perf_camper_lean", &npc_perf_camper_lean, 0, 1);
    CMD4(CCC_Integer, "npc_perf_walker_camp_once", &npc_perf_walker_camp_once, 0, 1);
    CMD4(CCC_Integer, "npc_perf_scheme_condlist_ttl_ms", &npc_perf_scheme_condlist_ttl_ms, 0, 5000);
    CMD4(CCC_Integer, "npc_perf_move_mgr_fixes", &npc_perf_move_mgr_fixes, 0, 1);
    CMD4(CCC_Integer, "npc_perf_death_release_batch_ms", &npc_perf_death_release_batch_ms, 0, 5000);
    CMD4(CCC_IntegerNotSaved, "npc_perf_death_release_batch_max", &npc_perf_death_release_batch_max, 0, 4096);
    // 30-game-events.md: B3 (Lua) and B2 (ai_stalker_events.cpp: a reject right before the destroy of the item skips
    // the bones of the owner) are always on since 2026-10-18 (npc_perf_death_lua_lite, npc_reject_destroy_skip_bones)
    // 22-game-thread.md, step 2: the sound dispatch filter (Level.cpp, CLevel::SoundEvent_Filter, always on) - a sound
    // goes to the listeners near it or near the actor, those with an xr_hear on_sound section, and at most N of the
    // others (the loudest); the actor's own sounds go to everyone. Its tunables
    CMD4(CCC_Float, "npc_perf_sound_dispatch_near_dist", &npc_perf_sound_dispatch_near_dist, 0.f, 500.f);
    CMD4(CCC_Float, "npc_perf_sound_dispatch_actor_dist", &npc_perf_sound_dispatch_actor_dist, 0.f, 500.f);
    CMD4(CCC_Integer, "npc_perf_sound_dispatch_max_combat", &npc_perf_sound_dispatch_max_combat, 0, 4096);
    CMD4(CCC_Integer, "npc_perf_sound_dispatch_max", &npc_perf_sound_dispatch_max, 0, 4096);
    // 24-critical-path.md, G3 (always on, the deliveries go out grouped by listener): the always-zone of the non-combat
    // sounds of other objects (no actor distance exemption) and at most N such sounds per listener a frame (0 = no
    // cap). Combat and actor sounds are never cut by them
    CMD4(CCC_Float, "npc_sound_noncombat_dist", &npc_sound_noncombat_dist, 0.f, 500.f);
    CMD4(CCC_Integer, "npc_sound_noncombat_per_listener", &npc_sound_noncombat_per_listener, 0, 64);
    CMD4(CCC_Integer, "npc_preview_scene_budget_per_frame", (int*)&npc_preview_scene_budget_per_frame, 1, 500);
    CMD4(CCC_Integer, "npc_preview_disk_cache_warm_per_frame", (int*)&npc_preview_disk_cache_warm_per_frame, 0, 500);

#ifndef MASTER_GOLD
    CMD3(CCC_Mask, "ai_ignore_actor", &psAI_Flags, aiIgnoreActor);
#endif // MASTER_GOLD

    // Physics
    CMD1(CCC_PHFps, "ph_frequency");
    CMD1(CCC_PHIterations, "ph_iterations");

    // P1: parallel island solve (permanent): the minimum count of active islands to solve them in parallel
    CMD4(CCC_Integer, "ph_mt_island_min", &ph_console::ph_mt_island_min, 1, 4096);
    // Cap physics substeps per frame to break the spiral-of-death under heavy load (0 = uncapped).
    CMD4(CCC_Integer, "ph_max_substeps", &ph_console::ph_max_substeps, 0, 30);

    // Ф2 (PHYSICS_ACTIVE_SET_PLAN): force-sleep far corpses, distance-based. ph_corpse_sleep_ms=0 disables.
    CMD4(CCC_Integer, "ph_corpse_sleep_ms", &ph_console::ph_corpse_sleep_ms, 0, 60000);
    CMD4(CCC_FloatBlock, "ph_corpse_sleep_far_dist", &ph_console::ph_corpse_sleep_far_dist, 0.f, 500.f);
    // ...but never while the ragdoll still moves fast or its joints are pulled apart (0 = no check).
    CMD4(CCC_FloatBlock, "ph_corpse_sleep_max_vel", &ph_console::ph_corpse_sleep_max_vel, 0.f, 100.f);
    CMD4(CCC_FloatBlock, "ph_corpse_sleep_max_stretch", &ph_console::ph_corpse_sleep_max_stretch, 0.f, 10.f);

    // Ф1 (PHYSICS_ACTIVE_SET_PLAN): far live NPCs -> pure kinematic. ph_npc_kinematic_far_dist=0 disables.
    CMD4(CCC_FloatBlock, "ph_npc_kinematic_far_dist", &ph_npc_kinematic_far_dist, 0.f, 500.f);
    CMD4(CCC_FloatBlock, "ph_npc_kinematic_far_hyst", &ph_npc_kinematic_far_hyst, 0.f, 200.f);
    // Ф3 (plans/optimization_mt/21-physics-step.md): also nearer NPCs that are not drawn in the main pass (always on).
    // The distance is saved and never below 10 m (an NPC next to the player keeps physics).
    CMD4(CCC_FloatBlock, "ph_npc_kinematic_unseen_dist", &ph_npc_kinematic_unseen_dist, 10.f, 500.f);

    // P1: NPC spawn / level-load optimizations (see plans/optimization_spawn/plan.md) are permanent; only the log is left.
    CMD4(CCC_Integer, "ph_dbg_spawn_stats", &ph_console::ph_dbg_spawn_stats, 0, 1);
    // P16 (plans/optimization_mt/11-ragdoll-spikes.md): death -> ragdoll spikes. Saved. The box of a starting free
    // ragdoll (it always ignores live characters) makes at most N world steps (vanilla up to 150), 0 = no cap
    CMD4(CCC_Integer, "ph_death_correct_max_steps", &ph_console::ph_death_correct_max_steps, 0, 150);
    // Launched NPCs and corpses, live NPCs thrown up in a dense crowd (plans/optimization_mt/21-physics-step.md,
    // 2026-10-15/16): the fixes are permanent, these are their saved limits (0 = no limit / the check off).
    // ph_contact_max_correct_vel: the contact push-out speed cap in the frame step, m/s; ph_ai_max_rise_vel: the rise
    // speed cap of live NPC controllers, m/s (the rise of the substep over it is taken back); ph_ai_try_max_dy, m: a
    // TryPosition result further up / down than that is refused; ph_ai_step_max_rise, m: a rise over that in one frame
    // substep is undone.
    CMD4(CCC_Float, "ph_contact_max_correct_vel", &ph_console::ph_contact_max_correct_vel, 0.f, 100.f);
    CMD4(CCC_Float, "ph_ai_max_rise_vel", &ph_console::ph_ai_max_rise_vel, 0.f, 100.f);
    CMD4(CCC_Float, "ph_ai_try_max_dy", &ph_console::ph_ai_try_max_dy, 0.f, 50.f);
    CMD4(CCC_Float, "ph_ai_step_max_rise", &ph_console::ph_ai_step_max_rise, 0.f, 50.f);
    // Corpse pairs dropped in the broadphase (plans/optimization_mt/21-physics-step.md, 2026-10-16): permanent, these are
    // the saved limits. Corpses never collide with each other beyond ph_corpse_vs_corpse_dist m of the camera (0 =
    // everywhere); corpse x live NPC pairs with both beyond ph_corpse_vs_npc_dist m of the camera are not collided (0 =
    // off; the actor always collides with corpses).
    CMD4(CCC_Float, "ph_corpse_vs_corpse_dist", &ph_console::ph_corpse_vs_corpse_dist, 0.f, 500.f);
    CMD4(CCC_Float, "ph_corpse_vs_npc_dist", &ph_console::ph_corpse_vs_npc_dist, 0.f, 500.f);
    // plans/npc-corpse-collision.md (always on, the emergency switch, not saved): live NPCs (stalkers, monsters) walk
    // through corpses at any distance; the actor still collides with them. 1 = the old collision (ph_corpse_vs_npc_dist).
    CMD4(CCC_IntegerNotSaved, "ph_npc_corpse_collide", &ph_console::ph_npc_corpse_collide, 0, 1);
    // Live NPCs left high in the air (plans/optimization_mt/21-physics-step.md, 2026-10-15; always on): a destination the
    // NPC is moved to without gravity (the kinematic branches of move_along_path, a successful TryPosition, a standing NPC
    // with a disabled character) that hangs more than ai_npc_ground_snap_height m above the AI floor is lowered to it at
    // ai_npc_ground_snap_speed m/s (0 = at once). Both are saved.
    CMD4(CCC_Float, "ai_npc_ground_snap_height", &ai_npc_ground_snap_height, 0.3f, 10.f);
    CMD4(CCC_Float, "ai_npc_ground_snap_speed", &ai_npc_ground_snap_speed, 0.f, 100.f);
    // Spawn stage 2 (plans/spawn_optimization/SPEC.md 5.A, section 12): CLevel::ProcessGameEvents materializes M_SPAWN
    // within spawn_budget_us per frame, by whole groups (a root + its inventory right behind it), from the head of the
    // queue only: what does not fit and every event behind it wait for the next frame, in the same order. At least
    // spawn_min_per_frame groups per frame; a head that waits spawn_max_delay_ms goes on without the budget, the same
    // while loading and on pause. spawn_budget_us 0 = as before (the whole queue in one frame).
    CMD4(CCC_Integer, "spawn_budget_us", &g_spawn_budget_us, 0, 100000);
    CMD4(CCC_Integer, "spawn_min_per_frame", &g_spawn_min_per_frame, 1, 1000);
    CMD4(CCC_Integer, "spawn_max_delay_ms", &g_spawn_max_delay_ms, 1, 60000);
    // A pass that goes past the budget for a head older than spawn_max_delay_ms stops at spawn_catchup_us (0 = no limit)
    CMD4(CCC_Integer, "spawn_catchup_us", &g_spawn_catchup_us, 0, 1000000);
    // 36-spawn-model-load.md (tunables, not saved). spawn_cold_models (0 = off): a budgeted pass stops in front of the
    // next spawn group after this many cold model loads. model_prefetch_margin: the model files of objects near the
    // online distance (+ this %) are read ahead on a thread. The model prefetch itself (the level's creatures and all
    // monster visuals into the pool on the loading screen, the files read ahead) is always on since 2026-10-18
    // (model_prefetch removed, see plan 41). model_prefetch_margin starts at the hardware policy (R5,
    // InitTunablesFromPolicy).
    CMD4(CCC_IntegerNotSaved, "spawn_cold_models", &g_spawn_cold_models, 0, 64);
    CMD4(CCC_IntegerNotSaved, "model_prefetch_margin", &g_model_prefetch_margin, kModelPrefetchMarginMin,
        kModelPrefetchMarginMax);

#ifdef DEBUG
    CMD1(CCC_PHGravity, "ph_gravity");
    CMD4(CCC_FloatBlock, "ph_timefactor", &phTimefactor, 0.000001f, 1000.f);
    CMD4(CCC_FloatBlock, "ph_break_common_factor", &ph_console::phBreakCommonFactor, 0.f, 1000000000.f);
    CMD4(CCC_FloatBlock, "ph_rigid_break_weapon_factor", &ph_console::phRigidBreakWeaponFactor, 0.f, 1000000000.f);
    CMD4(CCC_Integer, "ph_tri_clear_disable_count", &ph_console::ph_tri_clear_disable_count, 0, 255);
    CMD4(CCC_FloatBlock, "ph_tri_query_ex_aabb_rate", &ph_console::ph_tri_query_ex_aabb_rate, 1.01f, 3.f);
#endif // DEBUG

    CMD3(CCC_Mask, "g_god", &psActorFlags, AF_GODMODE);
    CMD3(CCC_Mask, "g_unlimitedammo", &psActorFlags, AF_UNLIMITEDAMMO);
    CMD1(CCC_TuneAttachableItem, "dbg_adjust_attachable_item");
    CMD4(CCC_Integer, "g_3d_scopes", &g_3d_scope_type, 0, 2);
    CMD3(CCC_Mask, "g_pnv_in_scope", &psActorFlags, AF_PNV_W_SCOPE_DIS);
    CMD3(CCC_Mask, "g_weapons_cam_anims", &psActorFlags, AF_USE_CAM_ANIMS);
#ifndef MASTER_GOLD
    CMD1(CCC_JumpToLevel, "jump_to_level");
    CMD1(CCC_ToggleNoClip, "g_no_clip");
    CMD1(CCC_Spawn, "g_spawn");
    CMD1(CCC_SpawnToInventory, "g_spawn_to_inventory");
    CMD1(CCC_Script, "run_script");
    CMD1(CCC_ScriptCommand, "run_string");
    CMD1(CCC_TimeFactor, "time_factor");
#endif // MASTER_GOLD

    // Available in MASTER_GOLD Release: bake DDS for dynamic inv icons (modding / content pipeline).
    CMD1(CCC_InvIconBakeDds, "inv_icon_bake_dds");
    CMD1(CCC_InvIconBakeCurrentDds, "inv_icon_bake_current_dds");
    CMD1(CCC_InvIconStats, "inv_icon_stats");
    CMD1(CCC_NpcPreviewCacheReset, "npc_preview_cache_reset");

    CMD1(CCC_LuaHelp, "dump_lua");
    CMD1(CCC_AddonList, "addon_list");
    CMD1(CCC_PluginReload, "plugin_reload");
    CMD1(CCC_EventList, "event_list");
    CMD1(CCC_EventEngineSources, "gw_event_engine_sources");
    CMD1(CCC_EventSchemaCheck, "gw_event_schema_check");
    CMD1(CCC_EventEngineLog, "gw_event_engine_log");
    CMD1(CCC_EventTrace, "event_trace");
    CMD1(CCC_DataList, "data_list");
    CMD1(CCC_TimerList, "timer_list");
    CMD1(CCC_IniList, "ini_list");
    CMD1(CCC_BinderList, "binder_list");
    CMD1(CCC_PluginCvarList, "plugin_cvar_list");
    CMD1(CCC_ScriptCallList, "script_call_list");
    CMD1(CCC_StorageList, "storage_list");
    CMD1(CCC_CallbackList, "callback_list");
    CMD1(CCC_EvaluatorList, "evaluator_list");
    CMD1(CCC_EvaluatorCacheReload, "ai_evaluator_cache_reload");
    CMD1(CCC_EvaluatorCacheList, "ai_evaluator_cache_list");
    CMD4(CCC_InvCellSize, "g_inv_cell_size", &g_inv_inv_cell_size, 1, 4);

    CMD3(CCC_Mask, "g_autopickup", &psActorFlags, AF_AUTOPICKUP);
    CMD3(CCC_Mask, "g_dynamic_music", &psActorFlags, AF_DYNAMIC_MUSIC);
    CMD3(CCC_Mask, "g_important_save", &psActorFlags, AF_IMPORTANT_SAVE);
    CMD3(CCC_Mask, "g_loading_stages", &psActorFlags, AF_LOADING_STAGES);
    CMD3(CCC_Mask, "g_always_use_attitude_sensors", &psActorFlags, AF_ALWAYS_USE_ATTITUDE_SENSORS);
    CMD3(CCC_Mask, "g_use_tracers", &psActorFlags, AF_USE_TRACERS);

    CMD4(CCC_Integer, "g_inv_highlight_equipped", &g_inv_highlight_equipped, 0, 1);
    CMD4(CCC_Integer, "g_first_person_death", &g_first_person_death, 0, 1);
    CMD4(CCC_Integer, "g_unload_ammo_after_pick_up", &g_auto_ammo_unload, 0, 1);
    CMD4(CCC_Integer, "g_normalize_mouse_sens", &g_normalize_mouse_sens, 0, 1);
    CMD4(CCC_Integer, "g_normalize_upgrade_mouse_sens", &g_normalize_upgrade_mouse_sens, 0, 1);

    CMD4(CCC_Float, "g_look_intensity_min", &psLookIntensityMin, 10.f, 100.f);
    CMD4(CCC_Float, "g_look_intensity_max", &psLookIntensityMax, 10.f, 100.f);
    CMD4(CCC_Float, "g_look_intensity_step", &psLookIntensityStep, 0.f, 10.f);

    CMD4(CCC_Float, "g_cursor_intensity_min", &psCursorIntensityMin, 1.f, 100.f);
    CMD4(CCC_Float, "g_cursor_intensity_max", &psCursorIntensityMax, 1.f, 100.f);
    CMD4(CCC_Float, "g_cursor_intensity_step", &psCursorIntensityStep, 0.f, 10.f);

    CMD1(CCC_CleanupTasks, "dbg_cleanup_tasks");

#ifdef DEBUG
    CMD1(CCC_ShowSmartCastStats, "show_smart_cast_stats");
    CMD1(CCC_ClearSmartCastStats, "clear_smart_cast_stats");

    CMD3(CCC_Mask, "dbg_draw_actor_alive", &dbg_net_Draw_Flags, dbg_draw_actor_alive);
    CMD3(CCC_Mask, "dbg_draw_actor_dead", &dbg_net_Draw_Flags, dbg_draw_actor_dead);
    CMD3(CCC_Mask, "dbg_draw_customzone", &dbg_net_Draw_Flags, dbg_draw_customzone);
    CMD3(CCC_Mask, "dbg_draw_teamzone", &dbg_net_Draw_Flags, dbg_draw_teamzone);
    CMD3(CCC_Mask, "dbg_draw_invitem", &dbg_net_Draw_Flags, dbg_draw_invitem);
    CMD3(CCC_Mask, "dbg_draw_actor_phys", &dbg_net_Draw_Flags, dbg_draw_actor_phys);
    CMD3(CCC_Mask, "dbg_draw_customdetector", &dbg_net_Draw_Flags, dbg_draw_customdetector);
    CMD3(CCC_Mask, "dbg_destroy", &dbg_net_Draw_Flags, dbg_destroy);
    CMD3(CCC_Mask, "dbg_draw_autopickupbox", &dbg_net_Draw_Flags, dbg_draw_autopickupbox);
    CMD3(CCC_Mask, "dbg_draw_rp", &dbg_net_Draw_Flags, dbg_draw_rp);
    CMD3(CCC_Mask, "dbg_draw_climbable", &dbg_net_Draw_Flags, dbg_draw_climbable);
    CMD3(CCC_Mask, "dbg_draw_skeleton", &dbg_net_Draw_Flags, dbg_draw_skeleton);

    CMD3(CCC_Mask, "dbg_draw_ph_contacts", &ph_dbg_draw_mask, phDbgDrawContacts);
    CMD3(CCC_Mask, "dbg_draw_ph_enabled_aabbs", &ph_dbg_draw_mask, phDbgDrawEnabledAABBS);
    CMD3(CCC_Mask, "dbg_draw_ph_intersected_tries", &ph_dbg_draw_mask, phDBgDrawIntersectedTries);
    CMD3(CCC_Mask, "dbg_draw_ph_saved_tries", &ph_dbg_draw_mask, phDbgDrawSavedTries);
    CMD3(CCC_Mask, "dbg_draw_ph_tri_trace", &ph_dbg_draw_mask, phDbgDrawTriTrace);
    CMD3(CCC_Mask, "dbg_draw_ph_positive_tries", &ph_dbg_draw_mask, phDBgDrawPositiveTries);
    CMD3(CCC_Mask, "dbg_draw_ph_negative_tries", &ph_dbg_draw_mask, phDBgDrawNegativeTries);
    CMD3(CCC_Mask, "dbg_draw_ph_tri_test_aabb", &ph_dbg_draw_mask, phDbgDrawTriTestAABB);
    CMD3(CCC_Mask, "dbg_draw_ph_tries_changes_sign", &ph_dbg_draw_mask, phDBgDrawTriesChangesSign);
    CMD3(CCC_Mask, "dbg_draw_ph_tri_point", &ph_dbg_draw_mask, phDbgDrawTriPoint);
    CMD3(CCC_Mask, "dbg_draw_ph_explosion_position", &ph_dbg_draw_mask, phDbgDrawExplosionPos);
    CMD3(CCC_Mask, "dbg_draw_ph_statistics", &ph_dbg_draw_mask, phDbgDrawObjectStatistics);
    CMD3(CCC_Mask, "dbg_draw_ph_mass_centres", &ph_dbg_draw_mask, phDbgDrawMassCenters);
    CMD3(CCC_Mask, "dbg_draw_ph_death_boxes", &ph_dbg_draw_mask, phDbgDrawDeathActivationBox);
    CMD3(CCC_Mask, "dbg_draw_ph_hit_app_pos", &ph_dbg_draw_mask, phHitApplicationPoints);
    CMD3(CCC_Mask, "dbg_draw_ph_cashed_tries_stats", &ph_dbg_draw_mask, phDbgDrawCashedTriesStat);
    CMD3(CCC_Mask, "dbg_draw_ph_car_dynamics", &ph_dbg_draw_mask, phDbgDrawCarDynamics);
    CMD3(CCC_Mask, "dbg_draw_ph_car_plots", &ph_dbg_draw_mask, phDbgDrawCarPlots);
    CMD3(CCC_Mask, "dbg_ph_ladder", &ph_dbg_draw_mask, phDbgLadder);
    CMD3(CCC_Mask, "dbg_draw_ph_explosions", &ph_dbg_draw_mask, phDbgDrawExplosions);
    CMD3(CCC_Mask, "dbg_draw_car_plots_all_trans", &ph_dbg_draw_mask, phDbgDrawCarAllTrnsm);
    CMD3(CCC_Mask, "dbg_draw_ph_zbuffer_disable", &ph_dbg_draw_mask, phDbgDrawZDisable);
    CMD3(CCC_Mask, "dbg_ph_obj_collision_damage", &ph_dbg_draw_mask, phDbgDispObjCollisionDammage);
    CMD_RADIOGROUPMASK2("dbg_ph_ai_always_phmove", &ph_dbg_draw_mask, phDbgAlwaysUseAiPhMove, "dbg_ph_ai_never_phmove",
        &ph_dbg_draw_mask, phDbgNeverUseAiPhMove);
    CMD3(CCC_Mask, "dbg_ph_ik", &ph_dbg_draw_mask, phDbgIK);
    CMD3(CCC_Mask, "dbg_ph_ik_off", &ph_dbg_draw_mask1, phDbgIKOff);
    CMD3(CCC_Mask, "dbg_draw_ph_ik_goal", &ph_dbg_draw_mask, phDbgDrawIKGoal);
    CMD3(CCC_Mask, "dbg_ph_ik_limits", &ph_dbg_draw_mask, phDbgIKLimits);
    CMD3(CCC_Mask, "dbg_ph_character_control", &ph_dbg_draw_mask, phDbgCharacterControl);
    CMD3(CCC_Mask, "dbg_draw_ph_ray_motions", &ph_dbg_draw_mask, phDbgDrawRayMotions);
    CMD4(CCC_Float, "dbg_ph_vel_collid_damage_to_display", &dbg_vel_collid_damage_to_display, 0.f, 1000.f);
    CMD4(CCC_DbgBullets, "dbg_draw_bullet_hit", &g_bDrawBulletHit, 0, 1);
    CMD4(CCC_Integer, "dbg_draw_fb_crosshair", &g_bDrawFirstBulletCrosshair, 0, 1);
    CMD1(CCC_DbgPhTrackObj, "dbg_track_obj");
    CMD3(CCC_Mask, "dbg_ph_actor_restriction", &ph_dbg_draw_mask1, ph_m1_DbgActorRestriction);
    CMD3(CCC_Mask, "dbg_draw_ph_hit_anims", &ph_dbg_draw_mask1, phDbgHitAnims);
    CMD3(CCC_Mask, "dbg_draw_ph_ik_limits", &ph_dbg_draw_mask1, phDbgDrawIKLimits);
    CMD3(CCC_Mask, "dbg_draw_ph_ik_predict", &ph_dbg_draw_mask1, phDbgDrawIKPredict);
    CMD3(CCC_Mask, "dbg_draw_ph_ik_collision", &ph_dbg_draw_mask1, phDbgDrawIKCollision);
    CMD3(CCC_Mask, "dbg_draw_ph_ik_shift_object", &ph_dbg_draw_mask1, phDbgDrawIKSHiftObject);
    CMD3(CCC_Mask, "dbg_draw_ph_ik_blending", &ph_dbg_draw_mask1, phDbgDrawIKBlending);
    CMD1(CCC_DBGDrawCashedClear, "dbg_ph_cashed_clear");
    extern BOOL dbg_draw_character_bones;
    extern BOOL dbg_draw_character_physics;
    extern BOOL dbg_draw_character_binds;
    extern BOOL dbg_draw_character_physics_pones;
    extern BOOL ik_cam_shift;
    CMD4(CCC_Integer, "dbg_draw_character_bones", &dbg_draw_character_bones, FALSE, TRUE);
    CMD4(CCC_Integer, "dbg_draw_character_physics", &dbg_draw_character_physics, FALSE, TRUE);
    CMD4(CCC_Integer, "dbg_draw_character_binds", &dbg_draw_character_binds, FALSE, TRUE);
    CMD4(CCC_Integer, "dbg_draw_character_physics_pones", &dbg_draw_character_physics_pones, FALSE, TRUE);

    CMD4(CCC_Integer, "ik_cam_shift", &ik_cam_shift, FALSE, TRUE);

    extern float ik_cam_shift_tolerance;
    CMD4(CCC_Float, "ik_cam_shift_tolerance", &ik_cam_shift_tolerance, 0.f, 2.f);
    extern float ik_cam_shift_speed;
    CMD4(CCC_Float, "ik_cam_shift_speed", &ik_cam_shift_speed, 0.f, 1.f);
    extern float ik_cam_shift_interpolation;
    CMD4(CCC_Float, "ik_cam_shift_interpolation", &ik_cam_shift_interpolation, 1.f, 10.f);
    extern BOOL dbg_draw_doors;
    CMD4(CCC_Integer, "dbg_draw_doors", &dbg_draw_doors, FALSE, TRUE);

    /*
    extern int ik_allign_free_foot;
    extern int ik_local_blending;
    extern int ik_blend_free_foot;
    extern int ik_collide_blend;
        CMD4(CCC_Integer,	"ik_allign_free_foot"			,&ik_allign_free_foot,	0,	1);
        CMD4(CCC_Integer,	"ik_local_blending"				,&ik_local_blending,	0,	1);
        CMD4(CCC_Integer,	"ik_blend_free_foot"			,&ik_blend_free_foot,	0,	1);
        CMD4(CCC_Integer,	"ik_collide_blend"				,&ik_collide_blend,	0,	1);
    */
    extern BOOL dbg_draw_ragdoll_spawn;
    CMD4(CCC_Integer, "dbg_draw_ragdoll_spawn", &dbg_draw_ragdoll_spawn, FALSE, TRUE);
    extern BOOL debug_step_info;
    extern BOOL debug_step_info_load;
    CMD4(CCC_Integer, "debug_step_info", &debug_step_info, FALSE, TRUE);
    CMD4(CCC_Integer, "debug_step_info_load", &debug_step_info_load, FALSE, TRUE);
    extern BOOL debug_character_material_load;
    CMD4(CCC_Integer, "debug_character_material_load", &debug_character_material_load, FALSE, TRUE);
    extern XRPHYSICS_API BOOL dbg_draw_camera_collision;
    CMD4(CCC_Integer, "dbg_draw_camera_collision", &dbg_draw_camera_collision, FALSE, TRUE);
    extern XRPHYSICS_API float camera_collision_character_skin_depth;
    extern XRPHYSICS_API float camera_collision_character_shift_z;
    CMD4(CCC_FloatBlock, "camera_collision_character_shift_z", &camera_collision_character_shift_z, 0.f, 1.f);
    CMD4(CCC_FloatBlock, "camera_collision_character_skin_depth", &camera_collision_character_skin_depth, 0.f, 1.f);
    extern BOOL dbg_draw_animation_movement_controller;
    CMD4(CCC_Integer, "dbg_draw_animation_movement_controller", &dbg_draw_animation_movement_controller, FALSE, TRUE);

    /*
    enum
    {
        dbg_track_obj_blends_bp_0			= 1<< 0,
        dbg_track_obj_blends_bp_1			= 1<< 1,
        dbg_track_obj_blends_bp_2			= 1<< 2,
        dbg_track_obj_blends_bp_3			= 1<< 3,
        dbg_track_obj_blends_motion_name	= 1<< 4,
        dbg_track_obj_blends_time			= 1<< 5,
        dbg_track_obj_blends_ammount		= 1<< 6,
        dbg_track_obj_blends_mix_params		= 1<< 7,
        dbg_track_obj_blends_flags			= 1<< 8,
        dbg_track_obj_blends_state			= 1<< 9,
        dbg_track_obj_blends_dump			= 1<< 10
    };
    */
    extern Flags32 dbg_track_obj_flags;
    CMD3(CCC_Mask, "dbg_track_obj_blends_bp_0", &dbg_track_obj_flags, dbg_track_obj_blends_bp_0);
    CMD3(CCC_Mask, "dbg_track_obj_blends_bp_1", &dbg_track_obj_flags, dbg_track_obj_blends_bp_1);
    CMD3(CCC_Mask, "dbg_track_obj_blends_bp_2", &dbg_track_obj_flags, dbg_track_obj_blends_bp_2);
    CMD3(CCC_Mask, "dbg_track_obj_blends_bp_3", &dbg_track_obj_flags, dbg_track_obj_blends_bp_3);
    CMD3(CCC_Mask, "dbg_track_obj_blends_motion_name", &dbg_track_obj_flags, dbg_track_obj_blends_motion_name);
    CMD3(CCC_Mask, "dbg_track_obj_blends_time", &dbg_track_obj_flags, dbg_track_obj_blends_time);

    CMD3(CCC_Mask, "dbg_track_obj_blends_ammount", &dbg_track_obj_flags, dbg_track_obj_blends_ammount);
    CMD3(CCC_Mask, "dbg_track_obj_blends_mix_params", &dbg_track_obj_flags, dbg_track_obj_blends_mix_params);
    CMD3(CCC_Mask, "dbg_track_obj_blends_flags", &dbg_track_obj_flags, dbg_track_obj_blends_flags);
    CMD3(CCC_Mask, "dbg_track_obj_blends_state", &dbg_track_obj_flags, dbg_track_obj_blends_state);
    CMD3(CCC_Mask, "dbg_track_obj_blends_dump", &dbg_track_obj_flags, dbg_track_obj_blends_dump);

    CMD1(CCC_DbgVar, "dbg_var");

    extern float dbg_text_height_scale;
    CMD4(CCC_FloatBlock, "dbg_text_height_scale", &dbg_text_height_scale, 0.2f, 5.f);
#endif

#ifdef DEBUG
    CMD1(CCC_DumpInfos, "dump_infos");
    CMD1(CCC_DumpTasks, "dump_tasks");
    CMD1(CCC_DumpMap, "dump_map");
    CMD1(CCC_DumpCreatures, "dump_creatures");

#endif

    CMD3(CCC_Mask, "cl_dynamiccrosshair", &psHUD_Flags, HUD_CROSSHAIR_DYNAMIC);
    CMD1(CCC_MainMenu, "main_menu");

#ifndef MASTER_GOLD
    CMD1(CCC_StartTimeSingle, "start_time_single");
    CMD4(CCC_TimeFactorSingle, "time_factor_single", &g_fTimeFactor, 0.f, 1000.0f);
#endif // MASTER_GOLD

    g_uCommonFlags.zero();
    g_uCommonFlags.set(flAiUseTorchDynamicLights, TRUE);

    CMD3(CCC_Mask, "ai_use_torch_dynamic_lights", &g_uCommonFlags, flAiUseTorchDynamicLights);

#ifndef MASTER_GOLD
    CMD4(CCC_Vector3, "psp_cam_offset", &CCameraLook2::m_cam_offset, Fvector().set(-1000, -1000, -1000),
        Fvector().set(1000, 1000, 1000));
#endif // MASTER_GOLD

    CMD1(CCC_GSCheckForUpdates, "check_for_updates");
#ifdef DEBUG
    CMD1(CCC_Crash, "crash");
    CMD1(CCC_DumpObjects, "dump_all_objects");
    CMD3(CCC_String, "stalker_death_anim", dbg_stalker_death_anim, 32);
    CMD4(CCC_Integer, "death_anim_debug", &death_anim_debug, FALSE, TRUE);
    CMD4(CCC_Integer, "death_anim_velocity", &b_death_anim_velocity, FALSE, TRUE);
    CMD4(CCC_Integer, "dbg_imotion_draw_velocity", &dbg_imotion_draw_velocity, FALSE, TRUE);
    CMD4(CCC_Integer, "dbg_imotion_collide_debug", &dbg_imotion_collide_debug, FALSE, TRUE);

    CMD4(CCC_Integer, "dbg_imotion_draw_skeleton", &dbg_imotion_draw_skeleton, FALSE, TRUE);
    CMD4(CCC_Float, "dbg_imotion_draw_velocity_scale", &dbg_imotion_draw_velocity_scale, 0.0001f, 100.0f);

    CMD4(CCC_Integer, "dbg_show_ani_info", &g_ShowAnimationInfo, 0, 1);
    CMD4(CCC_Integer, "dbg_dump_physics_step", &ph_console::g_bDebugDumpPhysicsStep, 0, 1);
    CMD1(CCC_InvUpgradesHierarchy, "inv_upgrades_hierarchy");
    CMD1(CCC_InvUpgradesCurItem, "inv_upgrades_cur_item");
    CMD4(CCC_Integer, "inv_upgrades_log", &g_upgrades_log, 0, 1);
    CMD1(CCC_InvDropAllItems, "inv_drop_all_items");

    extern BOOL dbg_moving_bones_snd_player;
    CMD4(CCC_Integer, "dbg_bones_snd_player", &dbg_moving_bones_snd_player, FALSE, TRUE);
#endif
    CMD4(CCC_Float, "con_sensitive", &g_console_sensitive, 0.01f, 1.0f);
    CMD4(CCC_Integer, "wpn_aim_toggle", &b_toggle_weapon_aim, 0, 1);
    CMD4(CCC_Integer, "wpn_quick_unload_upgraded", &g_quick_unload_upgraded, 0, 1);
    CMD4(CCC_Integer, "wpn_quick_unload_new", &g_quick_unload_new, 0, 1);
    CMD4(CCC_Integer, "wpn_quick_unload_scopes", &g_quick_unload_scopes, 0, 1);
    CMD4(CCC_Integer, "wpn_quick_unload_gl", &g_quick_unload_gl, 0, 1);
    CMD4(CCC_Integer, "wpn_quick_unload_silencers", &g_quick_unload_silencers, 0, 1);

    CMD4(CCC_Integer_Outfit_Faction, "g_outfit_faction", &g_outfit_faction, 0, 1);
    CMD1(CCC_DumpFactionRelations, "dump_faction_relations");

    CMD4(CCC_Integer, "g_enhancend_anims", &g_enhancend_anims, 0, 1);

    CMD1(CCC_UIStyle, "ui_style");
    CMD1(CCC_UIRestart, "ui_restart");

#ifdef DEBUG
    CMD4(CCC_Float, "ai_smart_cover_animation_speed_factor", &g_smart_cover_animation_speed_factor, .1f, 10.f);
    CMD4(CCC_Float, "air_resistance_epsilon", &air_resistance_epsilon, .0f, 1.f);
#endif // #ifdef DEBUG
    CMD4(CCC_Integer, "debug_show_second_wpn_model", &debug_show_second_wpn_model, 0, 1);
    CMD4(CCC_Integer, "debug_show_thrid_wpn_model", &debug_show_thrid_wpn_model, 0, 1);
    CMD4(CCC_Integer, "debug_show_attachments_slots", &debug_show_attachments_slots, 0, 1);
    CMD4(CCC_Integer, "debug_ui_item_cell", &debug_ui_item_cell, 0, 1);

    CMD4(CCC_Integer, "g_sleep_time", &psActorSleepTime, 1, 24);

    CMD4(CCC_Integer, "ai_use_old_vision", &g_ai_use_old_vision, 0, 1);

    CMD4(CCC_Integer, "ai_die_in_anomaly", &g_ai_die_in_anomaly, 0, 1); //Alundaio

    CMD4(CCC_Float, "ai_aim_predict_time", &g_aim_predict_time, 0.f, 10.f);
    CMD4(CCC_Float, "g_aim_z_offset_coff", &g_aim_z_offset_coff, -1.f, 1.f);
    CMD4(CCC_Float, "g_second_aim_z_offset_coff", &g_second_aim_z_offset_coff, -1.f, 1.f);

#ifdef DEBUG
    // extern BOOL g_use_new_ballistics;
    // CMD4(CCC_Integer,	"use_new_ballistics",	&g_use_new_ballistics, 0, 1);
    extern float g_bullet_time_factor;
    CMD4(CCC_Float, "g_bullet_time_factor", &g_bullet_time_factor, 0.f, 10.f);
#endif

#ifdef DEBUG
    extern BOOL g_ai_dbg_sight;
    CMD4(CCC_Integer, "ai_dbg_sight", &g_ai_dbg_sight, 0, 1);
#endif // #ifdef DEBUG

    //Alundaio: Scoped outside DEBUG
    extern BOOL g_ai_aim_use_smooth_aim;
    CMD4(CCC_Integer, "ai_aim_use_smooth_aim", &g_ai_aim_use_smooth_aim, 0, 1);

    extern float g_ai_aim_min_speed;
    CMD4(CCC_Float, "ai_aim_min_speed", &g_ai_aim_min_speed, 0.f, 10.f * PI);

    extern float g_ai_aim_min_angle;
    CMD4(CCC_Float, "ai_aim_min_angle", &g_ai_aim_min_angle, 0.f, 10.f * PI);

    extern float g_ai_aim_max_angle;
    CMD4(CCC_Float, "ai_aim_max_angle", &g_ai_aim_max_angle, 0.f, 10.f * PI);

#ifdef DEBUG
    extern BOOL g_debug_doors;
    CMD4(CCC_Integer, "ai_debug_doors", &g_debug_doors, 0, 1);
#endif // #ifdef DEBUG

    *g_last_saved_game = 0;

    CMD3(CCC_String, "slot_0", g_quick_use_slots[0], 32);
    CMD3(CCC_String, "slot_1", g_quick_use_slots[1], 32);
    CMD3(CCC_String, "slot_2", g_quick_use_slots[2], 32);
    CMD3(CCC_String, "slot_3", g_quick_use_slots[3], 32);

    CMD4(CCC_Integer, "keypress_on_start", &g_keypress_on_start, 0, 1);
    CMD1(CCC_UI_Time_Factor, "ui_time_factor");
    CMD2(CCC_UI_Time_Dilation_Mode, "time_dilation_inventory", UITimeDilator::Inventory);
    CMD2(CCC_UI_Time_Dilation_Mode, "time_dilation_pda", UITimeDilator::Pda);

    // K5 of plans/optimization_mt/26-framemove-game-next.md (1-B): a cheaper UpdateCL of weapons out of the actor's
    // hands (Weapon.h) - always on since 2026-10-18 (wpn_npc_ucl_lite)

    // K7 (plans/optimization_mt/26-framemove-game-next.md, 1-C): how long a cover search of a stalker that found no
    // cover is not repeated while the stalker and the threat stay in place (ai/stalker/ai_stalker_cover.cpp; 0 = no hold)
    extern int npc_cover_none_hold_ms;
    CMD4(CCC_Integer, "npc_cover_none_hold_ms", &npc_cover_none_hold_ms, 0, 10000);

    // K8 (plans/optimization_mt/26-framemove-game-next.md, 2-B). A tunable, not saved, 0 = off (ai/stalker/ai_stalker.cpp):
    // the combat tier of the UpdateCL LOD for fighters on screen at least N m from the camera (the zoomed distance;
    // every 2nd frame). K8c, the object handler of far fighters every 2nd pass / frame, is always on since 2026-10-18
    // (npc_oh_lod_combat 2)
    extern int npc_ucl_lod_combat_seen_dist;
    CMD4(CCC_IntegerNotSaved, "npc_ucl_lod_combat_seen_dist", &npc_ucl_lod_combat_seen_dist, 0, 500);

    // plans/optimization_mt/26-framemove-game-next.md (FrameMove after the pipelined prepass). Tunables, not saved,
    // 0 = as before: the combat tier for unseen fighters of the actor N m from the camera (ai/stalker/ai_stalker.cpp),
    // the map locations not near the camera updated every N-th frame (map_manager.cpp). The minimap cull (ui/UIMap.cpp,
    // ui_minimap_cull) is always on since 2026-10-18
    extern int npc_ucl_lod_combat_actor_dist;
    extern int map_update_lod;
    CMD4(CCC_IntegerNotSaved, "npc_ucl_lod_combat_actor_dist", &npc_ucl_lod_combat_actor_dist, 0, 500);
    CMD4(CCC_IntegerNotSaved, "map_update_lod", &map_update_lod, 0, 16);

    // plans/optimization_mt/28-updatecl-main.md: U1 - the feet IK of stalkers in phase B of their bones instead of their
    // UpdateCL (IKLimbsController.h), W1 - no UpdateCL of an idle weapon in the hands of a dead NPC (Weapon.h) - always on
    // since 2026-10-18 (npc_ik_in_bones, wpn_npc_dead_owner_skip)

    register_mp_console_commands();

    LogTunables(); // 50-smart-engine-hw-profile.md, 5.2
}
