#pragma once

#include "xrCore/xrCore.h"

#include <atomic>
#include <thread>
#include <type_traits>
#include <typeinfo>

// By analogy with TRACY_ENABLE: an explicit opt-in via xray_build.props's PreprocessorDefinitions
// (Mixed and Release-non-Master-Gold today), not auto-derived from MASTER_GOLD. A bare `/D
// XR_PERF_METRICS` (no `=value`) makes the compiler define it as 1, so if the build already set it,
// leave it as-is; otherwise it's off.
#ifndef XR_PERF_METRICS
#   define XR_PERF_METRICS 0
#endif

namespace xray::perf
{
namespace pass
{
constexpr u8 None = 0;
constexpr u8 Main = 1;
constexpr u8 Light = 2;
constexpr u8 SvpShadow = 3;
constexpr u8 Rain = 4;
constexpr u8 SunOld = 5;
constexpr u8 Svp = 6;
constexpr u8 SmapOther = 7;
constexpr u8 Sun0 = 8; // sun cascades are Sun0 + cascade index
// Bones prepass (CLevel::BonesPrepass): not a render pass. Far above Sun0 + cascades on purpose.
constexpr u8 Prepass = 200;
}

namespace crow
{
constexpr u8 None = 0;
constexpr u8 Main = 1;
constexpr u8 Sun = 2;
constexpr u8 Light = 3;
constexpr u8 Svp = 4;
constexpr u8 Shedule = 5;
constexpr u8 UclView = 6;
constexpr u8 UclR30 = 7;
constexpr u8 UclHom60 = 8;
constexpr u8 Spawn = 9;
constexpr u8 SetParent = 10;
constexpr u8 OlAct = 11;
constexpr u8 OlSleep = 12;
constexpr u8 Other = 13;
}

namespace bone
{
constexpr u8 None = 0;
constexpr u8 EyePp = 1;
constexpr u8 Ccf = 2;
constexpr u8 Weapon = 3;
constexpr u8 Torch = 4;
constexpr u8 Other = 5;
}

namespace wait_site
{
constexpr u8 None = 0;
constexpr u8 Game = 1;
constexpr u8 PreRender = 2;
constexpr u8 Hom = 3;
constexpr u8 RMain = 4;
constexpr u8 RRain = 5;
constexpr u8 RSun = 6;
constexpr u8 Light = 7;
constexpr u8 Detail = 8;
// 28-updatecl-main.md: the foot ray batch of the IK (CIKLimbsController::ExecuteIKRayBatch -> RayPickBatch) in UpdateCL
constexpr u8 Ik = 9;
}

// Who wakes a force-slept corpse (plans/optimization_mt/21-physics-step.md, physics/corpse_wake/path/*): the reason
// tag of the thread that reaches CPHShell::EnableObject. PERF_PH_WAKE sets it (the outer, specific caller: a zone, a
// grab, a Lua call); PERF_PH_WAKE_DEFAULT only when nothing is set yet (the generic shell API: a hit, an impulse).
namespace ph_wake
{
constexpr u8 None = 0;
constexpr u8 Contact = 1;      // NearCallback inside the frame step on the physics thread (= physics/corpse_wake/by/*)
constexpr u8 ContactLocal = 2; // NearCallback outside it: TryPosition, activation shape, FlyTo, StepTouch on main
constexpr u8 Hit = 3;          // CPHShell::applyHit (bullets, melee, hit events)
constexpr u8 Explosion = 4;    // CPHShell::ExplosionHit
constexpr u8 Impulse = 5;      // applyImpulse* / applyForce / applyGravityAccel / setForce / setTorque
constexpr u8 Script = 6;       // a Lua export of the physics API (ph_step_defer::ScriptCallScope)
constexpr u8 Grab = 7;         // CPHCapture (actor grab), CTelekinesis (monsters, gravi zones' pull)
constexpr u8 Zone = 8;         // an anomaly zone update (CCustomZone)
constexpr u8 Activation = 9;   // CPHShell::Enable / CPHElement::Enable, net state
}

// 24-critical-path.md, M8/M9 (plans/optimization_mt): the stalker planner a thread is in (TlAiPlanner, TlAiContext).
namespace ai_planner
{
constexpr u8 None = 0;
constexpr u8 Combat = 1; // CStalkerCombatPlanner::update
constexpr u8 Danger = 2; // CStalkerDangerPlanner::update
}

// 24-critical-path.md, M11: who asked for the friendly-fire pick of CAI_Stalker::update_can_kill_info (TlFfCaller).
namespace ff_caller
{
constexpr u8 None = 0;
constexpr u8 ObjectHandler = 1; // CAI_Stalker::update_object_handler (UpdateCL, seqParallel)
constexpr u8 Think = 2;         // CAI_Stalker::Think (shedule_Update: brain + movement, fire_make_sense included)
}

enum class Metric : u16
{
#define PERF_COUNTER(id, name) id,
#define PERF_GAUGE(id, name)   id,
#define PERF_TIMER(id, name)   id, id##_Calls,
#include "PerfMetricsList.inl"
#undef PERF_COUNTER
#undef PERF_GAUGE
#undef PERF_TIMER
    Count
};

// perf_metrics bits: 1 = Tracy plots, 2 = 5 s log dump, 4 = hide the engine's own per-call Tracy zones
// (xrSheduler ZoneTransientN, q_box/spatial_move/CollideStatic/box_query, decompress, get_occlusion_to,
// feel_sound_new, ol_SingleUpdate/ol_ObjUpdateCL...) while measuring, so they stop skewing the capture.
XRCORE_API extern int  g_perf_metrics;
XRCORE_API extern bool g_perf_metrics_active;
XRCORE_API extern bool g_perf_hide_hot_zones;

XRCORE_API extern std::atomic<std::thread::id> g_main_tid;
XRCORE_API extern std::atomic<std::thread::id> g_game_tid;
// The dedicated "Game logic" thread while it runs parallel to the render: set by xrEngine's KickGameLogicThread()
// before the wake, cleared by JoinGameLogicThread(); the empty id otherwise. Unlike g_game_tid it stays empty for the
// sequential GameThread task (loading, precache, pause, menu, overlap off), where nothing renders beside the game
// logic, so GW_OVERLAP_HAZARD compares against it and counts only true game/render overlap.
XRCORE_API extern std::atomic<std::thread::id> g_game_overlap_tid;

enum class ThreadKind : u8 { Main, Game, Worker };

// Classifies the calling thread as main / GameThread / a TaskManager worker, using only g_main_tid/g_game_tid
// (set by xrEngine's EngineThreading.cpp). Lives in xrCore so modules that cannot link xrEngine - e.g. xrSound,
// which would otherwise have a link cycle - can still tell GameThread apart from main and from pool workers.
inline ThreadKind CurrentThreadKind() noexcept
{
    const std::thread::id id = std::this_thread::get_id();
    const std::thread::id game = g_game_tid.load(std::memory_order_acquire);
    if (game != std::thread::id{} && id == game)
        return ThreadKind::Game;
    if (id == g_main_tid.load(std::memory_order_acquire))
        return ThreadKind::Main;
    return ThreadKind::Worker;
}

XRCORE_API u8& TlPass() noexcept;
XRCORE_API u8& TlCrowSrc() noexcept;
XRCORE_API u8& TlBoneCaller() noexcept;
XRCORE_API u8& TlWaitSite() noexcept;
// ph_wake::* of this thread (PERF_PH_WAKE, PERF_PH_WAKE_DEFAULT), read by CPHShell::EnableObject.
XRCORE_API u8& TlPhWake() noexcept;
// Spawn stage 0 (plans/spawn_optimization/SPEC.md): non-zero while this thread materializes an M_SPAWN on the client
// (CLevel::g_sv_Spawn, PERF_SPAWN_TAG). Lets the render and the sound tell a cold load inside a spawn from any other.
XRCORE_API u8& TlSpawn() noexcept;
// 30-game-events.md (plans/optimization_mt): non-zero while this thread runs CLevel::ProcessGameEvents
// (PERF_GAME_EVENT_TAG). The cold loads of the sound and the render inside it go to ge/cold/* too.
XRCORE_API u8& TlGameEvent() noexcept;
// B2.4: depth of the per-model bone locks (CKinematics::UCalc_ModelLock: phase B of CalculateBones, KinematicsBonesLock
// in Include/xrRender/Kinematics.h, LL_SetBoneVisible) held by this thread, recursive re-entries included. Bone
// callbacks written inside one are serialized with the render's bone calculation of that model, so the overlap
// detector of CBoneInstance::set_callback skips them and counts only the writes still racing the render.
XRCORE_API u32& TlBonesLocked() noexcept;
// 24-critical-path.md, M8/M9/M11 (xrGame, set only with XR_PERF_METRICS through PERF_AI_PLANNER / PERF_AI_CONTEXT /
// PERF_FF_CALLER). TlAiPlanner: ai_planner::* of the next CActionPlanner::update, which takes it (resets it for its
// own duration), so the planners nested in the tagged one see None. TlAiContext: ai_planner::* for the whole update of
// that planner, nested planners and Lua included. TlFfCaller: ff_caller::*.
XRCORE_API u8& TlAiPlanner() noexcept;
XRCORE_API u8& TlAiContext() noexcept;
XRCORE_API u8& TlFfCaller() noexcept;
// 36-spawn-model-load.md (plans/optimization_mt): cold model loads (CModelPool::Create had to load the ogf) made by this
// thread, a running count. CLevel::ProcessGameEvents compares it before and after a spawn group (spawn_cold_models).
// Counted with or without XR_PERF_METRICS.
XRCORE_API u32& TlColdModelLoads() noexcept;
// 50i-stage3-slow-disk-io.md (plans/optimization_mt, plan 50 R6): their time on this thread (the outermost loads, us), a
// running sum; the time limit of the spawn per frame (the hw policy's spawn_cold_ms). With or without XR_PERF_METRICS.
XRCORE_API u64& TlColdModelUs() noexcept;

XRCORE_API void AddSlow(Metric m, u64 v) noexcept;
XRCORE_API void AddTimeSlow(Metric m, u64 ticks) noexcept;
XRCORE_API void SetGauge(Metric m, s64 v) noexcept;
// A running maximum within one frame (e.g. "worst corpse velocity of the frame"), lock-free compare-exchange into the
// same gauge storage as SetGauge. Flush() takes it and resets it to 0 every frame, so the 5 s window prints the average
// of the per-frame maxima and its peak (before 2026-10-15 it was never reset: a session high-water mark).
XRCORE_API void UpdateMax(Metric m, s64 v) noexcept;
XRCORE_API void AddHist(Metric m, u32 bucket) noexcept;
XRCORE_API u64  Ticks() noexcept;
// Ticks() difference in milliseconds, with the calibration perf_metrics uses for its timers (for own reports).
XRCORE_API double TicksToMilliseconds(u64 ticks) noexcept;
XRCORE_API void Flush() noexcept;
XRCORE_API void RegisterBlendCb(const void* fn, const char* name) noexcept;
XRCORE_API const char* LookupBlendCb(const void* fn) noexcept;

struct ScopedTag
{
    u8& slot;
    u8  prev;

    ScopedTag(u8& s, u8 v) noexcept : slot(s), prev(s) { s = v; }
    ~ScopedTag() noexcept { slot = prev; }
    ScopedTag(const ScopedTag&) = delete;
    ScopedTag& operator=(const ScopedTag&) = delete;
};

// ScopedTag that leaves an already set tag alone: a generic entry point under a more specific caller.
struct ScopedTagIfUnset
{
    u8& slot;
    u8  prev;

    ScopedTagIfUnset(u8& s, u8 v) noexcept : slot(s), prev(s)
    {
        if (!s)
            s = v;
    }
    ~ScopedTagIfUnset() noexcept { slot = prev; }
    ScopedTagIfUnset(const ScopedTagIfUnset&) = delete;
    ScopedTagIfUnset& operator=(const ScopedTagIfUnset&) = delete;
};

// physics/corpse_wake/path/* bucket for a ph_wake tag.
inline Metric PhWakePathMetric(u8 tag) noexcept
{
    switch (tag)
    {
    case ph_wake::Contact:      return Metric::ph_corpse_wake_path_contact;
    case ph_wake::ContactLocal: return Metric::ph_corpse_wake_path_contact_local;
    case ph_wake::Hit:          return Metric::ph_corpse_wake_path_hit;
    case ph_wake::Explosion:    return Metric::ph_corpse_wake_path_explosion;
    case ph_wake::Impulse:      return Metric::ph_corpse_wake_path_impulse;
    case ph_wake::Script:       return Metric::ph_corpse_wake_path_script;
    case ph_wake::Grab:         return Metric::ph_corpse_wake_path_grab;
    case ph_wake::Zone:         return Metric::ph_corpse_wake_path_zone;
    case ph_wake::Activation:   return Metric::ph_corpse_wake_path_activation;
    default:                    return Metric::ph_corpse_wake_path_other;
    }
}

inline u8 CrowSrcFromPass(u8 p) noexcept
{
    if (p == pass::Main)
        return crow::Main;
    if (p == pass::Light)
        return crow::Light;
    // SvpShadow (record_second_vp_shadows) is its own cost category in the SVP docs (plans\optimization_svp,
    // plans\optimize_lights\plan_lamp_shadow_cache.md §6.3) - never merged into the main-view Light bucket.
    // It is bucketed with Svp rather than Light: same reasoning as the general SVP visibility build.
    if (p == pass::Svp || p == pass::SvpShadow)
        return crow::Svp;
    if (p == pass::Prepass)
        return crow::Other;
    if (p == pass::SunOld || p >= pass::Sun0)
        return crow::Sun;
    return crow::Other;
}

// m_crow_src/m_crow_frame on CGameObject are written together by whichever MakeMeCrow() call wins the
// dwFrame_AsCrow CAS - possibly on a SUN/LIGHT shadow-worker thread - and read together on the main
// thread next frame. Packing them into one atomic<u64> makes that pair observed-as-written: a single
// store and a single load, instead of two independent atomics that could be read torn across writes.
inline u64 PackCrow(u8 src, u32 frame) noexcept
{
    return (static_cast<u64>(frame) << 8) | src;
}

inline u8 UnpackCrowSrc(u64 packed) noexcept
{
    return static_cast<u8>(packed & 0xFFu);
}

inline u32 UnpackCrowFrame(u64 packed) noexcept
{
    return static_cast<u32>(packed >> 8);
}

inline void Add(Metric m, u64 v = 1) noexcept
{
    if (g_perf_metrics_active)
        AddSlow(m, v);
}

inline void IncCrowSrc(u8 src) noexcept
{
    switch (src)
    {
    case crow::Main:     Add(Metric::crow_src_main); break;
    case crow::Sun:      Add(Metric::crow_src_sun); break;
    case crow::Light:    Add(Metric::crow_src_light); break;
    case crow::Svp:      Add(Metric::crow_src_svp); break;
    case crow::Shedule:  Add(Metric::crow_src_shedule); break;
    case crow::UclView:  Add(Metric::crow_src_ucl_view); break;
    case crow::UclR30:   Add(Metric::crow_src_ucl_r30); break;
    case crow::UclHom60: Add(Metric::crow_src_ucl_hom60); break;
    case crow::Spawn:    Add(Metric::crow_src_spawn); break;
    case crow::SetParent:Add(Metric::crow_src_setparent); break;
    case crow::OlAct:    Add(Metric::crow_src_ol_act); break;
    case crow::OlSleep:  Add(Metric::crow_src_ol_sleep); break;
    default:             Add(Metric::crow_src_other); break;
    }
}

inline Metric WaitSiteCountMetric(u8 site) noexcept
{
    switch (site)
    {
    case wait_site::Game:      return Metric::task_wait_site_game;
    case wait_site::PreRender: return Metric::task_wait_site_prerender;
    case wait_site::Hom:       return Metric::task_wait_site_hom;
    case wait_site::RMain:     return Metric::task_wait_site_rmain;
    case wait_site::RRain:     return Metric::task_wait_site_rrain;
    case wait_site::RSun:      return Metric::task_wait_site_rsun;
    case wait_site::Light:     return Metric::task_wait_site_light;
    case wait_site::Detail:    return Metric::task_wait_site_detail;
    case wait_site::Ik:        return Metric::task_wait_site_ik;
    default:                   return Metric::task_wait_site_other;
    }
}

// task_wait_game_on_main broken down by whichever Wait() call ended up running the GameThread task
// on the main thread itself (see GameThreadScope in EngineThreading.cpp).
inline Metric GameOnMainSiteMetric(u8 site) noexcept
{
    switch (site)
    {
    case wait_site::Game:      return Metric::task_wait_game_on_main_game;
    case wait_site::PreRender: return Metric::task_wait_game_on_main_prerender;
    case wait_site::Hom:       return Metric::task_wait_game_on_main_hom;
    case wait_site::RMain:     return Metric::task_wait_game_on_main_rmain;
    case wait_site::RRain:     return Metric::task_wait_game_on_main_rrain;
    case wait_site::RSun:      return Metric::task_wait_game_on_main_rsun;
    case wait_site::Light:     return Metric::task_wait_game_on_main_light;
    case wait_site::Detail:    return Metric::task_wait_game_on_main_detail;
    case wait_site::Ik:        return Metric::task_wait_game_on_main_ik;
    default:                   return Metric::task_wait_game_on_main_other;
    }
}

// TlBoneCaller() is a write-only tag (PERF_BONE_CALLER) set at the few call sites that force an
// out-of-cadence CalculateBones (eye_pp_s0, CCF::BuildState, Weapon::UpdateXForm, Torch::UpdateCL);
// this is the one place it is read, to bucket the exact-computation cost by whichever of those forced it.
inline Metric BoneCallerMetric(u8 caller) noexcept
{
    switch (caller)
    {
    case bone::EyePp:  return Metric::bones_caller_eye_pp;
    case bone::Ccf:    return Metric::bones_caller_ccf;
    case bone::Weapon: return Metric::bones_caller_weapon;
    case bone::Torch:  return Metric::bones_caller_torch;
    default:           return Metric::bones_caller_other;
    }
}

// bones/recompute/* bucket for the render pass tag (PERF_PASS) of the thread that recomputes bones.
inline Metric BonesRecomputePassMetric(u8 p) noexcept
{
    if (p == pass::Main)
        return Metric::bones_recompute_main;
    if (p == pass::Svp)
        return Metric::bones_recompute_svp;
    if (p == pass::None)
        return Metric::bones_recompute_other;
    if (p == pass::Prepass)
        return Metric::bones_recompute_prepass;
    return Metric::bones_recompute_smap;
}

inline Metric BonesLockWaitMetric(ThreadKind k) noexcept
{
    switch (k)
    {
    case ThreadKind::Main: return Metric::bones_lock_wait_main;
    case ThreadKind::Game: return Metric::bones_lock_wait_game;
    default:               return Metric::bones_lock_wait_worker;
    }
}

inline Metric BonesLockHoldMetric(ThreadKind k) noexcept
{
    switch (k)
    {
    case ThreadKind::Main: return Metric::bones_lock_hold_main;
    case ThreadKind::Game: return Metric::bones_lock_hold_game;
    default:               return Metric::bones_lock_hold_worker;
    }
}

// Whether existing hot-path Tracy zones should pass is_active=false to ZoneNamed/ZoneNamedN this frame
// (see PERF_HOT_ZONE below). Latched once per frame in Flush(), same as g_perf_metrics_active.
inline bool HideHotZones() noexcept
{
    return g_perf_hide_hot_zones;
}

struct ScopedTime
{
    Metric metric;
    u64    t0{0};
    bool   active{false};

    explicit ScopedTime(Metric m) noexcept
        : metric(m), active(g_perf_metrics_active)
    {
        if (active)
            t0 = Ticks();
    }

    // Times only when `enable` is true (e.g. "this dsgraph is the main view"), see PERF_SCOPE_IF.
    ScopedTime(Metric m, bool enable) noexcept
        : metric(m), active(enable && g_perf_metrics_active)
    {
        if (active)
            t0 = Ticks();
    }

    ~ScopedTime() noexcept
    {
        if (active)
            AddTimeSlow(metric, Ticks() - t0);
    }

    ScopedTime(const ScopedTime&) = delete;
    ScopedTime& operator=(const ScopedTime&) = delete;
};

// Times the scope into one of three timers by the calling thread: main / GameThread / TaskManager worker or any other
// (CurrentThreadKind, classified when the scope starts). See PERF_SCOPE_BY_THREAD.
struct ScopedTimeByThread
{
    Metric metric;
    u64    t0{0};
    bool   active{false};

    ScopedTimeByThread(Metric main, Metric game, Metric worker) noexcept
        : metric(main), active(g_perf_metrics_active)
    {
        if (!active)
            return;
        switch (CurrentThreadKind())
        {
        case ThreadKind::Main: metric = main; break;
        case ThreadKind::Game: metric = game; break;
        default: metric = worker; break;
        }
        t0 = Ticks();
    }

    ~ScopedTimeByThread() noexcept
    {
        if (active)
            AddTimeSlow(metric, Ticks() - t0);
    }

    ScopedTimeByThread(const ScopedTimeByThread&) = delete;
    ScopedTimeByThread& operator=(const ScopedTimeByThread&) = delete;
};

// ---------------------------------------------------------------------------------------------------------------------
// 50n-frame-spike-report.md (plans/optimization_mt): the frame spike report and the 60 s frame window - always compiled
// and on (Release and Master Gold too), for the PCs of players and testers without Tracy and without the metrics build.
//
// Frame phases: an exclusive breakdown of a frame's wall time. A scope (GW_FRAME_PHASE) switches the phase the calling
// thread's time goes to until it ends, then the enclosing one resumes: nested scopes give each moment to the innermost
// one, so the main thread's phases plus "other" add up to the frame. Any thread may carry a scope (the sequential
// GameThread task runs on a pool worker or on main itself); the time outside any scope is not counted. Cost: one
// Ticks() and one relaxed atomic add per switch. Read and reset once per frame by Flush() (main thread).
namespace frame_phase
{
constexpr u8 None = 0; // outside any scope: not counted (main: "other" of the report)
// main thread (the order is the order of the report)
constexpr u8 Before = 1;        // seqParallelBeforRender, OnFrameBeforePreRender, environment, ModelDeferredClear
constexpr u8 FrameMove = 2;     // CRenderDevice::FrameMove (seqFrame) outside the level phases below, OnCameraUpdated
// 50z5-extra-diagnostics.md: the game UI update out of FrameMove (CUIGameCustom::OnFrame: CDialogHolder::OnFrame, the
// Lua windows, the HUD windows, the messages)
constexpr u8 Ui = 3;
constexpr u8 LvlEvents = 4;     // CLevel::ProcessGameEvents: spawns, destroys, hits (frame/level/game_events)
constexpr u8 LvlObjects = 5;    // IGame_Level::OnFrame: UpdateCL of the objects, destroy and relcase, HUD
constexpr u8 LvlScript = 6;     // Lua on main in CLevel::OnFrame: script_process, add_call, update_tasks, plugin timers
constexpr u8 LuaBus = 7;        // the Lua subscribers of the event bus on main (addon_event_bus.cpp), wherever emitted
constexpr u8 RtSched = 8;       // the real-time scheduler on main (overlap frame: the actor)
constexpr u8 Prepass = 9;       // the bones prepass
constexpr u8 GameWait = 10;     // main waits for the Game thread (sequential wait, JoinGameLogic)
constexpr u8 Render = 11;       // DoRender outside the build / submit / present parts (RenderBegin, ...)
constexpr u8 RenderBuild = 12;  // PERF_SPLIT(Build): visibility, dsgraph build (render/split/build)
constexpr u8 RenderSubmit = 13; // PERF_SPLIT(Submit): recording and submitting the draw calls, stats, ImGui
// 50z5: the game UI drawn on main (CLevel::OnRender -> HUD().RenderUI: the HUD, the windows, the fonts), out of
// RenderSubmit (render/split/submit with XR_PERF_METRICS still counts it, as before)
constexpr u8 UiRender = 14;
constexpr u8 Present = 15;      // RenderEnd: End + PresentFrame (the wait for the GPU / vsync is here)
constexpr u8 PhysWait = 16;     // the wait for the physics step of the frame
constexpr u8 PreRenderWait = 17; // the wait for the PreRender task (rain, particles)
constexpr u8 PhysFlush = 18;    // the deferred physics mutations, the post-join queue
constexpr u8 FpsSleep = 19;     // the sleep of the fps limit
constexpr u8 MainEnd = 20;
// the game logic (the dedicated Game thread of an overlap frame or the sequential GameThread task)
constexpr u8 GSound = 20;     // SoundEvent_Dispatch (sound perception callbacks)
constexpr u8 GLogic = 21;     // OnGameLogicFrame: queued NPC sound callbacks, level_on_frame (Lua)
constexpr u8 GSched = 22;     // the scheduler (NPC Think, Lua binders, ...), outside GAlife
constexpr u8 GAlife = 23;     // the ALife update inside the scheduler
constexpr u8 GVision = 24;    // the vision batch after the scheduler
constexpr u8 GParallel = 25;  // seqParallel
constexpr u8 GSeqMt = 26;     // seqFrameMT
constexpr u8 GSoundWait = 27; // the wait for the sound render task
constexpr u8 GLuaBus = 28;    // the Lua subscribers of the event bus on the game logic side (LuaBus of main there)
constexpr u8 Count = 29;
}

XRCORE_API u8 FramePhaseEnter(u8 phase) noexcept; // returns the phase to restore
XRCORE_API void FramePhaseLeave(u8 prev) noexcept;
// For the code that runs where the game logic runs (the Lua of the event bus): main_phase on main outside the game
// logic phases, game_phase elsewhere (the Game thread, the GameThread task on a worker, or on main inside a Wait - a
// game logic phase is current there). Returns the phase to restore (FramePhaseLeave).
XRCORE_API u8 FramePhaseEnterLogic(u8 main_phase, u8 game_phase) noexcept;

struct FramePhaseScope
{
    u8 prev;
    explicit FramePhaseScope(u8 phase) noexcept : prev(FramePhaseEnter(phase)) {}
    FramePhaseScope(u8 main_phase, u8 game_phase) noexcept : prev(FramePhaseEnterLogic(main_phase, game_phase)) {}
    ~FramePhaseScope() noexcept { FramePhaseLeave(prev); }
    FramePhaseScope(const FramePhaseScope&) = delete;
    FramePhaseScope& operator=(const FramePhaseScope&) = delete;
};

// One span of the game logic (a body of the Game thread / the GameThread task): its wall time and the CPU time of the
// thread that ran it, summed per frame (two QueryThreadCycleTime calls per span).
struct GameSpanScope
{
    u64 t0;
    u64 cpu0;
    XRCORE_API GameSpanScope() noexcept;
    XRCORE_API ~GameSpanScope() noexcept;
    GameSpanScope(const GameSpanScope&) = delete;
    GameSpanScope& operator=(const GameSpanScope&) = delete;
};

// Always-on event counters of the frame (count + time), incremented where the event happens, on any thread: two
// relaxed atomic adds. Nested events are counted by each of them (a cold model inside a spawn is in both).
namespace frame_event
{
constexpr u8 ModelCold = 0; // a cold model load on main / Game (CModelPool::Create had to load the ogf); outermost only
constexpr u8 SoundCold = 1; // a cold sound source load on main / Game (the ogg opened and read on the calling thread)
constexpr u8 SoundSet = 2;  // a sound set of an object built (CSoundPlayer::add: its cold sources are in SoundCold too)
constexpr u8 TexSync = 3;   // a texture created synchronously (CResourceManager::_CreateTexture -> CTexture::Load)
constexpr u8 Spawn = 4;     // a client spawn (CLevel::g_sv_Spawn)
constexpr u8 LuaGc = 5;     // a Lua GC step of the level (CLevel::script_gc)
constexpr u8 Destroy = 6;   // the destroy queue: count = objects destroyed, time = the whole batch with the relcase
constexpr u8 Save = 7;      // a game save (CALifeStorageManager::save)
constexpr u8 IoWait = 8;    // main / Game wait for a background reader (a sound being read by the "Sound prefetch" thread)
constexpr u8 LockWait = 9;  // main / Game wait for the model pool lock held by another thread
// 50r-slow-disk-async-textures.md: textures of the async loading (a slow disk) put in place on main: count + the time
// of it (created on main from the OS cache, or only swapped in). Their sync fallbacks are TexSync, the waits IoWait.
constexpr u8 TexAsync = 10;
// 50z5-extra-diagnostics.md: a shader object created by the resource manager (CResourceManager::CreateShader: the
// source opened, then the shader cache hit or a compile), any thread
constexpr u8 ShaderCreate = 11;
// 50z5: a real HLSL compile (D3D11: CHW::D3DCompile, a shader cache miss; nested in ShaderCreate; nothing on GL)
constexpr u8 ShaderCompile = 12;
// 50z5: a render target surface created (CRT::create: the texture and its views; its $user$ texture is a "tex" too)
constexpr u8 RtCreate = 13;
constexpr u8 Count = 14;
}

XRCORE_API void FrameEventAdd(u8 event, u64 ticks, u32 count = 1) noexcept;

// Times its scope as `count` events (Ticks()); `enable` false = inert.
struct FrameEventScope
{
    u64 t0;
    u32 count;
    u8 event;
    bool active;
    explicit FrameEventScope(u8 e, bool enable = true, u32 n = 1) noexcept
        : t0(enable ? Ticks() : 0), count(n), event(e), active(enable) {}
    ~FrameEventScope() noexcept
    {
        if (active)
            FrameEventAdd(event, Ticks() - t0, count);
    }
    FrameEventScope(const FrameEventScope&) = delete;
    FrameEventScope& operator=(const FrameEventScope&) = delete;
};

// The state of the frame that ends with the next Flush(), set by xrEngine at the end of CRenderDevice::ProcessFrame:
// in_game - a level frame in play (level ready, window active, no loading / precache / pause); only these frames are
// judged and counted in the window. session_s - Device.dwTimeGlobal / 1000; game_day_s - the in-game time of day, s.
XRCORE_API void FrameSetContext(bool in_game, u32 session_s, u32 game_day_s) noexcept;
// perf_frame_spike_ms (xrEngine cvar, not saved): the absolute threshold of the frame spike report, ms; the frame is a
// spike when its wall time is >= max(this, 3 x the running median of the last 128 game frames; the median only from 60
// frames on); 0 = report off (the 60 s window is still collected; the Lua dispatch timing below is off too).
XRCORE_API extern int g_perf_frame_spike_ms;
// The 60 s window: one "* [frame] <where>: ..." line with the frames since the previous one (frame time percentiles,
// spikes, CPU/GPU-bound shares, I/O, the event totals), then the window starts anew. Called next to the [mem] lines
// (xrGame, stat_memory_lines). Nothing without game frames in the window.
XRCORE_API void FrameWindowReport(const char* where) noexcept;

// 50n, "Lua events and the spike filter" (50n-frame-spike-report.md, 2026-10-19). Always compiled.
// The load / exit marks of the frame spike report, any thread, taken by the next Flush(): from Exit (a level change,
// a disconnect, the start of a load) until LoadEnd (IGame_Persistent::LoadEnd, the level is in play) and then for the
// first 3 s and 120 game frames of the level (both must pass) a spike is not reported, only counted in the window line
// ("warmup spikes N, exit spikes M"). LoadEnd also restarts the running median.
namespace frame_session
{
constexpr u8 Exit = 1;
constexpr u8 LoadEnd = 2;
}
XRCORE_API void FrameSessionMark(u8 mark) noexcept;

// The Lua subscribers of the event bus (xrGame addon_event_bus.cpp): with perf_frame_spike_ms > 0 every call of one is
// timed (two Ticks()); a call of at least LuaDispatchFloorTicks() is named (source:line) and passed to LuaDispatchNote.
// The floor is the [lua-slow] threshold (LuaSlowTicks(): 0.4 x perf_frame_spike_ms, 20 ms by default) or, lower, 1 ms
// and then the slowest call of the frame so far: the slowest call goes to the frame's spike line ("lua-slowest"), one
// of at least the threshold to a "! [lua-slow]" line (10 per 10 s, the rest counted; not during the warmup / exit
// above). subscribers > 0: the whole dispatch of one event to that many Lua subscribers took at least the threshold
// while none of its calls did (only the line, subscriber unused). u64(-1) / 0 = off (perf_frame_spike_ms 0).
XRCORE_API extern std::atomic<u64> g_lua_dispatch_floor_ticks;
XRCORE_API extern std::atomic<u64> g_lua_slow_ticks;
inline u64 LuaDispatchFloorTicks() noexcept { return g_lua_dispatch_floor_ticks.load(std::memory_order_relaxed); }
inline u64 LuaSlowTicks() noexcept { return g_lua_slow_ticks.load(std::memory_order_relaxed); }
inline bool LuaDispatchTimed() noexcept { return LuaDispatchFloorTicks() != u64(-1); }
XRCORE_API void LuaDispatchNote(u64 ticks, const char* event, const char* subscriber, u32 subscribers = 0) noexcept;

// C1.0 (plans/optimization_mt/17-c1-semi-pipeline.md): render/split/build and render/split/submit - the main thread's
// time inside DoRender split along the future hand-off point between building a frame and submitting it. A scope
// switches the class the main thread's time goes to until it ends, then the enclosing class resumes: nested scopes
// give each moment to the innermost one, nothing is counted twice and no re-indented blocks are needed. Main thread
// only - elsewhere (task workers, the Game and SVP threads) it is inert, so a task body may carry one for the case main
// runs the task itself inside a Wait. See PERF_SPLIT.
// 50n: always compiled, a frame phase of the main thread (frame_phase::RenderBuild / RenderSubmit; None = GameWait, its
// only use is the join with the Game thread); render/split/* are counted from these phases with XR_PERF_METRICS.
namespace split
{
constexpr u8 None = 0; // not counted (the join with the Game thread)
constexpr u8 Build = 1;
constexpr u8 Submit = 2;
}

XRCORE_API u8 SplitEnter(u8 kind) noexcept; // returns the class to restore
XRCORE_API void SplitLeave(u8 prev) noexcept;

struct ScopedSplit
{
    u8   prev{split::None};
    bool active;

    explicit ScopedSplit(u8 kind) noexcept
        : active(std::this_thread::get_id() == g_main_tid.load(std::memory_order_relaxed))
    {
        if (active)
            prev = SplitEnter(kind);
    }

    ~ScopedSplit() noexcept
    {
        if (active)
            SplitLeave(prev);
    }

    ScopedSplit(const ScopedSplit&) = delete;
    ScopedSplit& operator=(const ScopedSplit&) = delete;
};

// GW_OVERLAP_HAZARD (METRICS_PLAN §8.7): flags a render/main-only resource being touched from the game
// logic thread while that thread is running - a data race waiting to happen under plan 1's overlap, not
// something perf_metrics should gate. Own cvar (overlap_hazard_probe, set from xrEngine), independent of
// g_perf_metrics, so it stays usable with metrics collection off.
XRCORE_API extern int g_overlap_hazard_probe; // cvar overlap_hazard_probe (xrEngine), 0/1, not saved to user.ltx

// Distinct call stacks logged per hazard site (GW_OVERLAP_HAZARD and xrPhysics' PH_STEP_HAZARD).
constexpr size_t kHazardStacksPerSite = 6;

struct OverlapHazardSite
{
    std::atomic<u32> count{0};
    std::atomic<u64> last_report_ms{0};
    // Hashes of the call stacks already logged for this site, filled in order (0 = free slot), see IsNewHazardStack.
    std::atomic<u64> stacks[kHazardStacksPerSite]{};
};

// True the first time a call path (hash of the return addresses) reaches `site`, for up to kHazardStacksPerSite
// distinct paths per site; the caller then logs the whole stack (xrDebug::LogStackTrace, symbolized - slow, but only
// once per new path). Lock-free and allocation-free; once all slots of the site are taken it returns false without
// capturing anything. Always false off Windows (no stacks there).
XRCORE_API bool IsNewHazardStack(OverlapHazardSite& site) noexcept;

// Logs the call stack of each new distinct caller (IsNewHazardStack), the first 8 hits at `where` in full, then at
// most once per ~5s after that (with the running count).
XRCORE_API void ReportOverlapHazard(OverlapHazardSite& site, const char* where) noexcept;

// ---------------------------------------------------------------------------------------------------------------------
// P19 (plans/optimization_mt/14-spike-instrumentation.md): spike logger. One call of a marked place (PERF_SPIKE*) that
// takes longer than perf_spike_us goes to the log as one line:
//   ! [perf-spike] <site> <wall>us cpu=<thread CPU>us|- <detail> thread=<name>#<os id> frame=<n> in=<outer sites>
// cpu = CPU time of the thread inside the call (QueryThreadCycleTime): wall >> cpu means the thread was preempted by the
// OS or blocked (lock, wait), not computing. At most perf_spike_lines lines per 5 s window, then "! [perf-spike]
// summary" with every site that spiked (hits, max, avg, lines not printed). Own cvars (xrEngine, not saved),
// independent of perf_metrics: off (perf_spike_us 0) a marked call costs one load and a branch.
XRCORE_API extern int g_perf_spike_us;    // perf_spike_us: threshold in microseconds, 0 = off
XRCORE_API extern int g_perf_spike_cpu;   // perf_spike_cpu: 1 = per-object sites read the thread CPU time too
XRCORE_API extern int g_perf_spike_lines; // perf_spike_lines: lines per 5 s window before only the summary counts
XRCORE_API extern u64 g_perf_spike_ticks; // perf_spike_us in Ticks(), latched once per frame in Flush(); 0 = off
// perf_frame_peak_ms (32-frame-spikes.md; xrEngine cvar, not saved): a frame (wall time between two Flush() calls)
// longer than this many ms -> one "! [frame-peak]" line with its deltas of GC / relcase / ALife / game events / Lua
// binders / the bus / the waits of main; 0 = off. > 0 turns the metric collection on (not the plots or the 5 s log).
XRCORE_API extern int g_perf_frame_peak_ms;

inline bool SpikeActive() noexcept { return g_perf_spike_ticks != 0; }

// ---------------------------------------------------------------------------------------------------------------------
// 50n-frame-spike-report.md (plans/optimization_mt): GPU time of an engine frame. Always compiled (Release and Master Gold
// too), POD only, callable from any thread.
//   GpuFrameTimeSubmit(frame_id, gpu_ms) - the render (or main) thread stores the GPU time measured for the engine frame
//       frame_id (= Device.dwFrame of the frame whose GPU work was measured), when its timestamp query result arrives
//       (usually 1-3 frames later). A later submit for the same frame_id overwrites the earlier one.
//   GpuFrameTimeGet(frame_id, gpu_ms) - true and the time if that frame's result is still in the ring buffer (the last
//       16 submitted frames: one lock-free 64-bit slot per frame_id % 16), false if it has not arrived (yet) or was
//       overwritten. The frame spike report waits up to 3 frames for it, the 60 s summary takes avg / p95 from it.
XRCORE_API void GpuFrameTimeSubmit(u32 frame_id, float gpu_ms) noexcept;
XRCORE_API bool GpuFrameTimeGet(u32 frame_id, float& gpu_ms) noexcept;

// A site is registered once (a function-local static, see PERF_SPIKE_SITE_ID); its name is copied into xrCore, so a
// site of a module that is unloaded later leaves nothing dangling. Returns the site id (u16(-1) when the table is full).
XRCORE_API u16 RegisterSpikeSite(const char* name) noexcept;
// CPU time of the calling thread in Ticks() units (Windows: thread cycle time, the same TSC as Ticks()).
XRCORE_API u64 ThreadCpuTicks() noexcept;
// type_info::name() without the "class " / "struct " prefix of MSVC.
XRCORE_API const char* SpikeClassName(const std::type_info& type) noexcept;

// Writes the "detail" part of a spike line for `ctx` (called only for a spike, after the call returned).
using SpikeDescribeFn = void (*)(const void* ctx, char* buf, size_t size);

struct SpikeScope
{
    u64 threshold; // latched at the start: 0 = this scope is inactive
    u64 t0{0};
    u64 cpu0{0};
    const void* ctx;
    SpikeDescribeFn describe;
    SpikeScope* parent{nullptr};
    u16 site;
    bool cpu{false};

    SpikeScope(u16 site_, const void* ctx_ = nullptr, SpikeDescribeFn describe_ = nullptr, bool want_cpu = false) noexcept
        : threshold(g_perf_spike_ticks), ctx(ctx_), describe(describe_), site(site_)
    {
        if (threshold)
            Begin(want_cpu);
    }

    ~SpikeScope() noexcept
    {
        if (threshold)
            End();
    }

    XRCORE_API void Begin(bool want_cpu) noexcept;
    XRCORE_API void End() noexcept;

    SpikeScope(const SpikeScope&) = delete;
    SpikeScope& operator=(const SpikeScope&) = delete;
};

// Detail of a game object for a spike line: name, section, ID and the most derived class. T must be the exact type of
// the pointer passed as ctx (PERF_SPIKE_OBJECT takes it from the expression), so the cast back is exact.
template <class T>
void DescribeSpikeObject(const void* ctx, char* buf, size_t size) noexcept
{
    const T* object = static_cast<const T*>(ctx);
    // cName()/cNameSect() return a shared_str copy, whose reference count is not atomic: off the main and Game threads
    // (physics thread, pool workers) only the ID and the class, which are read without copies
    if (CurrentThreadKind() == ThreadKind::Worker)
    {
        xr_sprintf(buf, size, "id=%u cls=%s", static_cast<u32>(object->ID()), SpikeClassName(typeid(*object)));
        return;
    }
    const shared_str name = object->cName();
    const shared_str section = object->cNameSect();
    xr_sprintf(buf, size, "obj=%s sect=%s id=%u cls=%s", name.c_str() ? name.c_str() : "", section.c_str() ? section.c_str() : "",
        static_cast<u32>(object->ID()), SpikeClassName(typeid(*object)));
}
} // namespace xray::perf

#if XR_PERF_METRICS
#   define PERF_ADD(m, v) ::xray::perf::Add(::xray::perf::Metric::m, v)
#   define PERF_INC(m)    ::xray::perf::Add(::xray::perf::Metric::m, 1)
#   define PERF_SCOPE(m)  ::xray::perf::ScopedTime CONCATENIZE(_perf_, __LINE__)(::xray::perf::Metric::m)
#   define PERF_SCOPE_IF(cond, m) ::xray::perf::ScopedTime CONCATENIZE(_perf_, __LINE__)(::xray::perf::Metric::m, (cond))
#   define PERF_ADD_IF(cond, m, v) do { if (cond) ::xray::perf::Add(::xray::perf::Metric::m, v); } while (0)
#   define PERF_SCOPE_BY_THREAD(m_main, m_game, m_worker)                                                            \
        ::xray::perf::ScopedTimeByThread CONCATENIZE(_perf_thr_, __LINE__)(::xray::perf::Metric::m_main,               \
            ::xray::perf::Metric::m_game, ::xray::perf::Metric::m_worker)
// Drop-in replacement for ZoneScopedN(name) on an existing hot-path zone: same zone, but perf_metrics bit 2
// (xray::perf::HideHotZones()) can turn it off for the session without recompiling, because Tracy's
// ScopedZone/ZoneNamed decide whether to send ZoneBegin/ZoneEnd from the is_active argument at construction.
#   define PERF_HOT_ZONE(name) ZoneNamedN(CONCATENIZE(_perf_hot_zone_, __LINE__), name, !::xray::perf::HideHotZones())
#else
#   define PERF_ADD(m, v) ((void)0)
#   define PERF_INC(m)    ((void)0)
#   define PERF_SCOPE(m)  ((void)0)
#   define PERF_SCOPE_IF(cond, m) ((void)0)
#   define PERF_ADD_IF(cond, m, v) ((void)0)
#   define PERF_SCOPE_BY_THREAD(m_main, m_game, m_worker) ((void)0)
#   define PERF_HOT_ZONE(name) ZoneScopedN(name)
#endif

// C1.0: the main thread's time until the end of the scope goes to render/split/<kind> (Build / Submit / None).
// 50n: always compiled - also the render phases of the frame spike report (frame_phase::RenderBuild / RenderSubmit).
#define PERF_SPLIT(kind) ::xray::perf::ScopedSplit CONCATENIZE(_perf_split_, __LINE__)(::xray::perf::split::kind)
// 50n (always compiled): the frame phase (frame_phase::*) of the calling thread until the end of the scope.
#define GW_FRAME_PHASE(phase) ::xray::perf::FramePhaseScope CONCATENIZE(_gw_frame_phase_, __LINE__)(::xray::perf::frame_phase::phase)
// 50n (always compiled): main_phase on main, game_phase on the game logic side (FramePhaseEnterLogic).
#define GW_FRAME_PHASE_LOGIC(main_phase, game_phase) ::xray::perf::FramePhaseScope CONCATENIZE(_gw_frame_phase_, __LINE__)( \
    ::xray::perf::frame_phase::main_phase, ::xray::perf::frame_phase::game_phase)
// 50n (always compiled): the scope is one frame_event::* (count 1 + its time); GW_FRAME_EVENT_IF only when cond is true.
#define GW_FRAME_EVENT(event) ::xray::perf::FrameEventScope CONCATENIZE(_gw_frame_event_, __LINE__)(::xray::perf::frame_event::event)
#define GW_FRAME_EVENT_IF(cond, event) ::xray::perf::FrameEventScope CONCATENIZE(_gw_frame_event_, __LINE__)(::xray::perf::frame_event::event, (cond))
// 50n (always compiled): a span of the game logic (wall + thread CPU time of the Game thread in the report).
#define GW_GAME_SPAN() ::xray::perf::GameSpanScope CONCATENIZE(_gw_game_span_, __LINE__)

// P19 spike logger (see SpikeScope). Each expansion owns its site (a lambda's static), the name is a string literal.
//   PERF_SPIKE(name)                 - plain site
//   PERF_SPIKE_PHASE(name)           - a once-per-frame phase: always reads the thread CPU time
//   PERF_SPIKE_OBJECT(name, obj)     - per-object site, detail = obj (name/section/ID/class); CPU with perf_spike_cpu 1
//   PERF_SPIKE_OBJECT_CPU(name, obj, cpu) - the same, `cpu` forces the CPU time (e.g. for the actor only)
//   PERF_SPIKE_NAMED(var, name, ctx, fn, cpu) - named scope with own detail, var.ctx can be changed before it ends
#if XR_PERF_METRICS
#   define PERF_SPIKE_SITE_ID(name) ([]() noexcept { static const u16 _spike_site = ::xray::perf::RegisterSpikeSite(name); return _spike_site; }())
#   define PERF_SPIKE_NAMED(var, name, ctx, fn, cpu) ::xray::perf::SpikeScope var(PERF_SPIKE_SITE_ID(name), (ctx), (fn), (cpu))
#   define PERF_SPIKE(name) PERF_SPIKE_NAMED(CONCATENIZE(_perf_spike_, __LINE__), name, nullptr, nullptr, false)
#   define PERF_SPIKE_PHASE(name) PERF_SPIKE_NAMED(CONCATENIZE(_perf_spike_, __LINE__), name, nullptr, nullptr, true)
#   define PERF_SPIKE_OBJECT_CPU(name, obj, cpu)                                                                     \
        PERF_SPIKE_NAMED(CONCATENIZE(_perf_spike_, __LINE__), name, static_cast<const void*>(obj),                    \
            &::xray::perf::DescribeSpikeObject<std::remove_cv_t<std::remove_pointer_t<decltype(obj)>>>, (cpu))
#   define PERF_SPIKE_OBJECT(name, obj) PERF_SPIKE_OBJECT_CPU(name, obj, false)
#else
#   define PERF_SPIKE_NAMED(var, name, ctx, fn, cpu) ((void)0)
#   define PERF_SPIKE(name) ((void)0)
#   define PERF_SPIKE_PHASE(name) ((void)0)
#   define PERF_SPIKE_OBJECT_CPU(name, obj, cpu) ((void)0)
#   define PERF_SPIKE_OBJECT(name, obj) ((void)0)
#endif

// P19b (14-spike-instrumentation.md, "FrameMove и актор"): one stage of a once-per-frame path (the actor, the HUD, the
// persistent) - a Tracy zone, a perf_metrics timer and a spike site with the thread CPU time, all under one name. One per
// block: the Tracy zone has a fixed variable name.
#define PERF_STAGE(zone, metric) ZoneScopedN(zone); PERF_SCOPE(metric); PERF_SPIKE_PHASE(zone)
// Spawn stage 0 (plans/spawn_optimization/SPEC.md): one step of a per-object path (an object being spawned) - the same
// trio, but the spike site reads the thread CPU time only with perf_spike_cpu 1 (PERF_SPIKE, not PERF_SPIKE_PHASE).
#define PERF_STEP(zone, metric) ZoneScopedN(zone); PERF_SCOPE(metric); PERF_SPIKE(zone)

#define PERF_PASS(v) ::xray::perf::ScopedTag CONCATENIZE(_perf_pass_, __LINE__)(::xray::perf::TlPass(), static_cast<u8>(v))
#define PERF_CROW_SRC(v) ::xray::perf::ScopedTag CONCATENIZE(_perf_crow_, __LINE__)(::xray::perf::TlCrowSrc(), static_cast<u8>(v))
#define PERF_BONE_CALLER(v) ::xray::perf::ScopedTag CONCATENIZE(_perf_bone_, __LINE__)(::xray::perf::TlBoneCaller(), static_cast<u8>(v))
#define PERF_WAIT_SITE(v) ::xray::perf::ScopedTag CONCATENIZE(_perf_wait_, __LINE__)(::xray::perf::TlWaitSite(), static_cast<u8>(v))
#define PERF_SPAWN_TAG() ::xray::perf::ScopedTag CONCATENIZE(_perf_spawn_, __LINE__)(::xray::perf::TlSpawn(), 1)
#define PERF_GAME_EVENT_TAG() ::xray::perf::ScopedTag CONCATENIZE(_perf_ge_, __LINE__)(::xray::perf::TlGameEvent(), 1)
// physics/corpse_wake/path/* (ph_wake::*): nothing at all in builds without XR_PERF_METRICS, a thread-local byte otherwise.
#if XR_PERF_METRICS
#   define PERF_PH_WAKE(v) ::xray::perf::ScopedTag CONCATENIZE(_perf_ph_wake_, __LINE__)(::xray::perf::TlPhWake(), static_cast<u8>(v))
#   define PERF_PH_WAKE_DEFAULT(v) ::xray::perf::ScopedTagIfUnset CONCATENIZE(_perf_ph_wake_, __LINE__)(::xray::perf::TlPhWake(), static_cast<u8>(v))
#else
#   define PERF_PH_WAKE(v) ((void)0)
#   define PERF_PH_WAKE_DEFAULT(v) ((void)0)
#endif
// 24-critical-path.md, M8/M9/M11: the AI tags (TlAiPlanner, TlAiContext, TlFfCaller); nothing without XR_PERF_METRICS.
#if XR_PERF_METRICS
#   define PERF_AI_PLANNER(v) ::xray::perf::ScopedTag CONCATENIZE(_perf_ai_planner_, __LINE__)(::xray::perf::TlAiPlanner(), static_cast<u8>(v))
#   define PERF_AI_CONTEXT(v) ::xray::perf::ScopedTag CONCATENIZE(_perf_ai_context_, __LINE__)(::xray::perf::TlAiContext(), static_cast<u8>(v))
#   define PERF_FF_CALLER(v) ::xray::perf::ScopedTag CONCATENIZE(_perf_ff_caller_, __LINE__)(::xray::perf::TlFfCaller(), static_cast<u8>(v))
#else
#   define PERF_AI_PLANNER(v) ((void)0)
#   define PERF_AI_CONTEXT(v) ((void)0)
#   define PERF_FF_CALLER(v) ((void)0)
#endif

// Always compiled (not gated by XR_PERF_METRICS): overlap_hazard_probe is its own opt-in cvar, meant to
// stay available for diagnosing plan-1 overlap races even in builds where perf_metrics is off.
// Fires only on the dedicated "Game logic" thread of an overlap frame (g_game_overlap_tid), i.e. while the render
// really runs in parallel; the sequential GameThread task (loading, precache, pause, menu) is not a hazard. The
// cvar stays the first check, so the probe costs one load when it is off.
#define GW_OVERLAP_HAZARD(where)                                                                         \
    do                                                                                                   \
    {                                                                                                     \
        if (::xray::perf::g_overlap_hazard_probe)                                                         \
        {                                                                                                  \
            const std::thread::id _goh_game =                                                              \
                ::xray::perf::g_game_overlap_tid.load(std::memory_order_acquire);                           \
            if (_goh_game != std::thread::id{} && std::this_thread::get_id() == _goh_game)                  \
            {                                                                                                \
                static ::xray::perf::OverlapHazardSite _goh_site;                                             \
                ::xray::perf::ReportOverlapHazard(_goh_site, where);                                          \
            }                                                                                                \
        }                                                                                                    \
    } while (0)

namespace xray::perf
{
// ---------------------------------------------------------------------------------------------------------------------
// 50o-gpu-frame-timing.md (plans/optimization_mt): where the main thread waited for the GPU in an engine frame, and
// whether the GPU was starved before it. Always compiled (Release and Master Gold too), POD only, callable from any
// thread; the same scheme as GpuFrameTimeSubmit/Get (16 frames, one lock-free 64-bit slot per frame_id % 16).
// Submitted by the render's Present (D3D11 and GL), frame_id = Device.dwFrame:
//   PresentWaitSubmit(frame_id, ms) - CPU time of IDXGISwapChain::Present / SDL_GL_SwapWindow of that frame: high =
//       the driver's frame queue was full (GPU-bound) or vsync. Submitted in the same frame (no delay).
//   GpuSyncWaitSubmit(frame_id, ms) - CPU time of the render's GPU sync point (R_sync_point::Wait, main + scope pass)
//       of that frame. D3D11 waits there for the GPU to reach the previous frame's sync point (the CPU is at most about
//       one frame ahead), GL for everything submitted so far: in a GPU-bound frame main waits here, not in Present.
//       Submitted in the same frame; 0 in frames without a level render (menu, loading screen).
//   GpuIdleGapSubmit(frame_id, ms) - GPU timeline from the previous frame's end timestamp to this frame's begin one
//       (both frames measured, no disjoint between): the GPU had nothing of ours to do - CPU-bound. Arrives with the
//       GPU time of the frame (1-3 frames later); absent when the previous frame was not measured.
// Reading a frame's GpuFrameTimeGet: the span between two timestamps on the GPU timeline, so in a CPU-bound frame it
// includes the GPU's waits for the commands (an upper bound of the GPU work, close to the CPU submit time); GPU-bound
// = the waits above are large and gpu_ms is close to the frame time.
XRCORE_API void PresentWaitSubmit(u32 frame_id, float ms) noexcept;
XRCORE_API bool PresentWaitGet(u32 frame_id, float& ms) noexcept;
XRCORE_API void GpuSyncWaitSubmit(u32 frame_id, float ms) noexcept;
XRCORE_API bool GpuSyncWaitGet(u32 frame_id, float& ms) noexcept;
XRCORE_API void GpuIdleGapSubmit(u32 frame_id, float ms) noexcept;
XRCORE_API bool GpuIdleGapGet(u32 frame_id, float& ms) noexcept;
} // namespace xray::perf
