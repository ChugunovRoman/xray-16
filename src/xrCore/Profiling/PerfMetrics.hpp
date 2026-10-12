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
// Spawn stage 0 (plans/spawn_optimization/SPEC.md): non-zero while this thread materializes an M_SPAWN on the client
// (CLevel::g_sv_Spawn, PERF_SPAWN_TAG). Lets the render and the sound tell a cold load inside a spawn from any other.
XRCORE_API u8& TlSpawn() noexcept;

XRCORE_API void AddSlow(Metric m, u64 v) noexcept;
XRCORE_API void AddTimeSlow(Metric m, u64 ticks) noexcept;
XRCORE_API void SetGauge(Metric m, s64 v) noexcept;
// A sticky running maximum (e.g. "worst corpse velocity seen"), lock-free compare-exchange into the same
// gauge storage as SetGauge. Unlike SetGauge it never goes down and is not reset on Flush(): it is an
// all-time high-water-mark for the session, not a per-5s-window peak (there is no per-metric reset hook).
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

// GW_OVERLAP_HAZARD (METRICS_PLAN §8.7): flags a render/main-only resource being touched from the game
// logic thread while that thread is running - a data race waiting to happen under plan 1's overlap, not
// something perf_metrics should gate. Own cvar (overlap_hazard_probe, set from xrEngine), independent of
// g_perf_metrics, so it stays usable with metrics collection off.
XRCORE_API extern int g_overlap_hazard_probe; // cvar overlap_hazard_probe (xrEngine), 0/1, not saved to user.ltx

struct OverlapHazardSite
{
    std::atomic<u32> count{0};
    std::atomic<u64> last_report_ms{0};
};

// Logs the first 8 hits at `where` in full, then at most once per ~5s after that (with the running count).
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

inline bool SpikeActive() noexcept { return g_perf_spike_ticks != 0; }

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

// Always compiled (not gated by XR_PERF_METRICS): overlap_hazard_probe is its own opt-in cvar, meant to
// stay available for diagnosing plan-1 overlap races even in builds where perf_metrics is off.
#define GW_OVERLAP_HAZARD(where)                                                                         \
    do                                                                                                   \
    {                                                                                                     \
        if (::xray::perf::g_overlap_hazard_probe)                                                         \
        {                                                                                                  \
            const std::thread::id _goh_game = ::xray::perf::g_game_tid.load(std::memory_order_acquire);    \
            if (_goh_game != std::thread::id{} && std::this_thread::get_id() == _goh_game)                  \
            {                                                                                                \
                static ::xray::perf::OverlapHazardSite _goh_site;                                             \
                ::xray::perf::ReportOverlapHazard(_goh_site, where);                                          \
            }                                                                                                \
        }                                                                                                    \
    } while (0)
