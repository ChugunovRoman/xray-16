#include "stdafx.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "xrCore/Profiling/IniLoadStats.hpp"
#include "xrCore/Profiling/SysDiag.hpp"

#include <atomic>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <thread>
#include <ctime>
#include <cstdarg>
#include <cstdio>
#include <SDL.h>

#if defined(XR_PLATFORM_WINDOWS)
#   include <psapi.h>
#else
#   include <sys/resource.h>
#   include <unistd.h>
#endif

#if defined(_MSC_VER)
#   include <intrin.h>
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#   include <x86intrin.h>
#endif

namespace xray::perf
{
XRCORE_API int  g_perf_metrics = 7;
XRCORE_API bool g_perf_metrics_active = false;
XRCORE_API bool g_perf_hide_hot_zones = false;
XRCORE_API std::atomic<std::thread::id> g_main_tid{};
XRCORE_API std::atomic<std::thread::id> g_game_tid{};
XRCORE_API std::atomic<std::thread::id> g_game_overlap_tid{};
XRCORE_API int g_overlap_hazard_probe = 0;
// 50n-frame-spike-report.md: on by default in the measurement builds (a call of a marked place longer than 50 ms goes to
// the log with its object), off in the others (the marks are not compiled there)
XRCORE_API int g_perf_spike_us = XR_PERF_METRICS ? 50000 : 0;
XRCORE_API int g_perf_spike_cpu = 0;
XRCORE_API int g_perf_spike_lines = 40;
XRCORE_API u64 g_perf_spike_ticks = 0;
XRCORE_API int g_perf_frame_peak_ms = 0;
} // namespace xray::perf

namespace
{
constexpr size_t kMaxSlots = 32;
constexpr size_t kNumMetrics = static_cast<size_t>(xray::perf::Metric::Count);

struct alignas(64) Slot
{
    std::atomic<u64> v[kNumMetrics];
};

Slot s_slots[kMaxSlots]{};
Slot s_fallback_slot{};
std::atomic<u64> s_gauges[kNumMetrics]{};
// Gauges written by UpdateMax(): read and reset to 0 by Flush() every frame (a per-frame maximum).
std::atomic<bool> s_gauge_is_max[kNumMetrics]{};
std::atomic<u32> s_next_slot{0};
thread_local size_t tl_slot = size_t(-1);
thread_local u8 tl_pass_v = 0;
thread_local u8 tl_crow_src_v = 0;
thread_local u8 tl_bone_caller_v = 0;
thread_local u8 tl_wait_site_v = 0;
thread_local u8 tl_ph_wake_v = 0;
thread_local u8 tl_spawn_v = 0;
thread_local u8 tl_game_event_v = 0;
thread_local u32 tl_bones_locked_v = 0;
thread_local u8 tl_ai_planner_v = 0;
thread_local u8 tl_ai_context_v = 0;
thread_local u8 tl_ff_caller_v = 0;
thread_local u32 tl_cold_model_loads_v = 0; // 36-spawn-model-load.md
thread_local u64 tl_cold_model_us_v = 0; // 50i-stage3-slow-disk-io.md

constexpr size_t kMaxBlendCb = 16;
struct BlendCbEntry
{
    const void* fn{};
    const char* name{};
};
BlendCbEntry s_blend_cb[kMaxBlendCb]{};
std::atomic<u32> s_blend_cb_n{0};

inline size_t GetThreadSlot() noexcept
{
    if (tl_slot == size_t(-1))
    {
        tl_slot = s_next_slot.fetch_add(1, std::memory_order_relaxed);
    }
    return tl_slot;
}

struct MetricDesc
{
    const char* name;
    const char* base_name;
    bool is_timer_ticks;
    bool is_timer_calls;
    bool is_gauge;
};

#define PERF_COUNTER(id, name) { name, name, false, false, false },
#define PERF_GAUGE(id, name)   { name, name, false, false, true },
#define PERF_TIMER(id, name)   { name "/ms", name, true, false, false }, { name "/calls", name, false, true, false },
const MetricDesc s_metric_descs[kNumMetrics] = {
#include "PerfMetricsList.inl"
};
#undef PERF_COUNTER
#undef PERF_GAUGE
#undef PERF_TIMER

// 50n: QueryThreadCycleTime cycles per Ticks() tick, measured by CalibrateTicksFrequency (0 = not measured)
double s_thread_cycles_per_tick = 0.0;

u64 CalibrateTicksFrequency() noexcept
{
#if defined(_MSC_VER) || defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    const u64 qpc_freq = SDL_GetPerformanceFrequency();
    if (!qpc_freq)
        return 3000000000ULL;

    const u64 wait_ticks = qpc_freq / 500; // 2 ms spin calibration
#if defined(XR_PLATFORM_WINDOWS)
    ULONG64 cycles0 = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles0);
#endif
    const u64 qpc0 = SDL_GetPerformanceCounter();
    const u64 tsc0 = __rdtsc();
    while ((SDL_GetPerformanceCounter() - qpc0) < wait_ticks)
    {
    }
    const u64 qpc1 = SDL_GetPerformanceCounter();
    const u64 tsc1 = __rdtsc();
    const u64 delta_qpc = qpc1 - qpc0;
    const u64 delta_tsc = tsc1 - tsc0;
#if defined(XR_PLATFORM_WINDOWS)
    // 50n: the thread cycle time per TSC tick, measured on the same spin (1.0 where the kernel counts it with the TSC,
    // the usual case since Vista; the frame spike report divides by it when it is clearly above, see ThreadCpuScale)
    ULONG64 cycles1 = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles1);
    if (delta_tsc)
        s_thread_cycles_per_tick = static_cast<double>(cycles1 - cycles0) / static_cast<double>(delta_tsc);
#endif
    return delta_qpc ? ((delta_tsc * qpc_freq) / delta_qpc) : 3000000000ULL;
#else
    return SDL_GetPerformanceFrequency();
#endif
}

u64 s_ticks_frequency = 0;

inline u64 GetTicksFrequency() noexcept
{
    if (!s_ticks_frequency)
        s_ticks_frequency = CalibrateTicksFrequency();
    return s_ticks_frequency;
}

inline double TicksToMs(u64 ticks) noexcept
{
    const u64 freq = GetTicksFrequency();
    if (!freq)
        return 0.0;
    return (static_cast<double>(ticks) * 1000.0) / static_cast<double>(freq);
}

u64 s_calib_last_tsc = 0;
u64 s_calib_last_qpc = 0;

void UpdateTicksCalibration() noexcept
{
#if defined(_MSC_VER) || defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    const u64 cur_tsc = xray::perf::Ticks();
    const u64 cur_qpc = SDL_GetPerformanceCounter();
    const u64 qpc_freq = SDL_GetPerformanceFrequency();
    if (s_calib_last_qpc != 0 && qpc_freq != 0)
    {
        const u64 dqpc = cur_qpc - s_calib_last_qpc;
        if (dqpc >= qpc_freq) // 1 second
        {
            const u64 dtsc = cur_tsc - s_calib_last_tsc;
            s_ticks_frequency = (dtsc * qpc_freq) / dqpc;
            s_calib_last_tsc = cur_tsc;
            s_calib_last_qpc = cur_qpc;
        }
    }
    else
    {
        s_calib_last_tsc = cur_tsc;
        s_calib_last_qpc = cur_qpc;
        if (!s_ticks_frequency)
            s_ticks_frequency = CalibrateTicksFrequency();
    }
#endif
}

// P19 spike logger (PerfMetrics.hpp, SpikeScope): the sites and their per-window statistics live here, in xrCore.
constexpr size_t kMaxSpikeSites = 192; // 128 -> 192: spawn stage 0 adds ~40 sites (PERF_STEP)
constexpr u32 kSpikeWindowMs = 5000;

struct SpikeSiteStat
{
    char name[48]{};
    std::atomic<u32> hits{0};       // spikes in the window
    std::atomic<u32> suppressed{0}; // of them not printed (perf_spike_lines reached)
    std::atomic<u64> max_ticks{0};
    std::atomic<u64> sum_ticks{0};
};

SpikeSiteStat s_spike_sites[kMaxSpikeSites];
std::atomic<u32> s_spike_site_count{0};
std::atomic<u32> s_spike_window_lines{0};
u32 s_spike_window_start_ms = 0;
thread_local xray::perf::SpikeScope* tl_spike_top = nullptr;
thread_local char tl_spike_thread_name[64] = {};
thread_local bool tl_spike_thread_name_read = false;

u64 s_prev_totals[kNumMetrics]{};
u64 s_window_deltas[kNumMetrics]{};
u64 s_window_max_frame[kNumMetrics]{};
u32 s_window_start_ms = 0;
u32 s_window_frames = 0;

// 32-frame-spikes.md, [frame-peak] (perf_frame_peak_ms): the wall time between two Flush() calls is the frame
u64 s_frame_peak_prev_ticks = 0;
constexpr u32 kFramePeakLinesPerWindow = 30; // per 5 s: a low threshold must not flood the log
u32 s_frame_peak_window_start_ms = 0;
u32 s_frame_peak_lines = 0;
u32 s_frame_peak_dropped = 0;
} // namespace

namespace xray::perf
{
XRCORE_API u8& TlPass() noexcept { return tl_pass_v; }
XRCORE_API u8& TlCrowSrc() noexcept { return tl_crow_src_v; }
XRCORE_API u8& TlBoneCaller() noexcept { return tl_bone_caller_v; }
XRCORE_API u8& TlWaitSite() noexcept { return tl_wait_site_v; }
XRCORE_API u8& TlPhWake() noexcept { return tl_ph_wake_v; }
XRCORE_API u8& TlSpawn() noexcept { return tl_spawn_v; }
XRCORE_API u8& TlGameEvent() noexcept { return tl_game_event_v; }
XRCORE_API u32& TlBonesLocked() noexcept { return tl_bones_locked_v; }
XRCORE_API u8& TlAiPlanner() noexcept { return tl_ai_planner_v; }
XRCORE_API u8& TlAiContext() noexcept { return tl_ai_context_v; }
XRCORE_API u8& TlFfCaller() noexcept { return tl_ff_caller_v; }
XRCORE_API u32& TlColdModelLoads() noexcept { return tl_cold_model_loads_v; }
XRCORE_API u64& TlColdModelUs() noexcept { return tl_cold_model_us_v; }

XRCORE_API void RegisterBlendCb(const void* fn, const char* name) noexcept
{
    if (!fn)
        return;
    const u32 n = s_blend_cb_n.load(std::memory_order_relaxed);
    for (u32 i = 0; i < n && i < kMaxBlendCb; ++i)
    {
        if (s_blend_cb[i].fn == fn)
            return;
    }
    const u32 slot = s_blend_cb_n.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kMaxBlendCb)
        return;
    s_blend_cb[slot].fn = fn;
    s_blend_cb[slot].name = name;
}

XRCORE_API const char* LookupBlendCb(const void* fn) noexcept
{
    const u32 n = s_blend_cb_n.load(std::memory_order_relaxed);
    for (u32 i = 0; i < n && i < kMaxBlendCb; ++i)
    {
        if (s_blend_cb[i].fn == fn)
            return s_blend_cb[i].name;
    }
    return nullptr;
}

XRCORE_API u64 Ticks() noexcept
{
#if defined(_MSC_VER) || defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    return __rdtsc();
#else
    return SDL_GetPerformanceCounter();
#endif
}

XRCORE_API double TicksToMilliseconds(u64 ticks) noexcept { return TicksToMs(ticks); }

XRCORE_API void AddSlow(Metric m, u64 v) noexcept
{
    const size_t idx = static_cast<size_t>(m);
    if (idx >= kNumMetrics)
        return;

    const size_t slot = GetThreadSlot();
    if (slot < kMaxSlots)
    {
        auto& atom = s_slots[slot].v[idx];
        atom.store(atom.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);

        if (s_metric_descs[idx].is_timer_ticks && idx + 1 < kNumMetrics)
        {
            auto& atom_calls = s_slots[slot].v[idx + 1];
            atom_calls.store(atom_calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        }
    }
    else
    {
        s_fallback_slot.v[idx].fetch_add(v, std::memory_order_relaxed);
        if (s_metric_descs[idx].is_timer_ticks && idx + 1 < kNumMetrics)
        {
            s_fallback_slot.v[idx + 1].fetch_add(1, std::memory_order_relaxed);
        }
    }
}

XRCORE_API void AddTimeSlow(Metric m, u64 ticks) noexcept
{
    AddSlow(m, ticks);
}

XRCORE_API void SetGauge(Metric m, s64 v) noexcept
{
    const size_t idx = static_cast<size_t>(m);
    if (idx >= kNumMetrics)
        return;
    s_gauges[idx].store(static_cast<u64>(v), std::memory_order_relaxed);
}

XRCORE_API void UpdateMax(Metric m, s64 v) noexcept
{
    const size_t idx = static_cast<size_t>(m);
    if (idx >= kNumMetrics)
        return;
    if (!s_gauge_is_max[idx].load(std::memory_order_relaxed))
        s_gauge_is_max[idx].store(true, std::memory_order_relaxed);
    std::atomic<u64>& slot = s_gauges[idx];
    u64 cur = slot.load(std::memory_order_relaxed);
    const u64 desired = static_cast<u64>(v);
    while (static_cast<s64>(cur) < v && !slot.compare_exchange_weak(cur, desired, std::memory_order_relaxed))
    {
    }
}

XRCORE_API void AddHist(Metric m, u32 bucket) noexcept
{
    const size_t idx = static_cast<size_t>(m) + bucket;
    if (idx < kNumMetrics)
        AddSlow(static_cast<Metric>(idx), 1);
}

// 50n-frame-spike-report.md: the frame phases (PerfMetrics.hpp, frame_phase). Per thread: the phase its time goes to
// now and since when; the sums are global (any thread may carry a scope), read and reset by Flush() on main. Every scope
// of main ends inside CRenderDevice::ProcessFrame and the game logic is joined there, so no segment spans Flush().
namespace
{
std::atomic<u64> s_phase_ticks[frame_phase::Count]{};
std::atomic<u64> s_game_on_main_ticks{0}; // game logic phases main ran itself (the sequential task inside a Wait)
thread_local u8 tl_phase = frame_phase::None;
thread_local u64 tl_phase_t0 = 0;

void PhaseAccount(u8 phase, u64 dt) noexcept
{
    if (phase == frame_phase::None || phase >= frame_phase::Count)
        return;
    s_phase_ticks[phase].fetch_add(dt, std::memory_order_relaxed);
    if (phase >= frame_phase::MainEnd)
    {
        if (std::this_thread::get_id() == g_main_tid.load(std::memory_order_relaxed))
            s_game_on_main_ticks.fetch_add(dt, std::memory_order_relaxed);
    }
    else if (g_perf_metrics_active)
    {
        // C1.0 render/split/* (17-c1-semi-pipeline.md), as before: build = PERF_SPLIT(Build), submit = PERF_SPLIT(Submit)
        // and the present inside it
        if (phase == frame_phase::RenderBuild)
            AddSlow(Metric::rsplit_build, dt);
        else if (phase == frame_phase::RenderSubmit || phase == frame_phase::UiRender || phase == frame_phase::Present)
            AddSlow(Metric::rsplit_submit, dt);
    }
}

u8 PhaseSwitch(u8 phase) noexcept
{
    const u8 prev = tl_phase;
    if (phase == prev)
        return prev;
    const u64 now = Ticks();
    PhaseAccount(prev, now - tl_phase_t0);
    tl_phase = phase;
    tl_phase_t0 = now;
    return prev;
}
} // namespace

XRCORE_API u8 FramePhaseEnter(u8 phase) noexcept { return PhaseSwitch(phase); }
XRCORE_API void FramePhaseLeave(u8 prev) noexcept { PhaseSwitch(prev); }

XRCORE_API u8 FramePhaseEnterLogic(u8 main_phase, u8 game_phase) noexcept
{
    // a game logic phase current (the Game thread, the GameThread task, also on main inside a Wait) or not main: the
    // game side; main outside them: the main side
    const bool main_side = tl_phase < frame_phase::MainEnd &&
        std::this_thread::get_id() == g_main_tid.load(std::memory_order_relaxed);
    return PhaseSwitch(main_side ? main_phase : game_phase);
}

// C1.0 split (PerfMetrics.hpp, ScopedSplit), since 50n a frame phase of main: Build / Submit, None = the wait for the
// Game thread (its only use, JoinGameLogic). Returns the phase to restore.
XRCORE_API u8 SplitEnter(u8 kind) noexcept
{
    const u8 phase = kind == split::Build ? frame_phase::RenderBuild :
        kind == split::Submit ? frame_phase::RenderSubmit : frame_phase::GameWait;
    return PhaseSwitch(phase);
}

XRCORE_API void SplitLeave(u8 prev) noexcept { PhaseSwitch(prev); }

// ---------------------------------------------------------------------------------------------------------------------
// P19 spike logger (plans/optimization_mt/14-spike-instrumentation.md)

XRCORE_API u16 RegisterSpikeSite(const char* name) noexcept
{
    static std::mutex s_register_mutex;
    std::lock_guard<std::mutex> lock(s_register_mutex);
    const u32 count = s_spike_site_count.load(std::memory_order_relaxed);
    // the same name in several places is one site: one line of the summary
    for (u32 i = 0; i < count; ++i)
    {
        if (0 == std::strncmp(s_spike_sites[i].name, name, sizeof(s_spike_sites[i].name) - 1))
            return static_cast<u16>(i);
    }
    if (count >= kMaxSpikeSites)
        return u16(-1);
    xr_strcpy(s_spike_sites[count].name, name);
    s_spike_site_count.store(count + 1, std::memory_order_release);
    return static_cast<u16>(count);
}

XRCORE_API u64 ThreadCpuTicks() noexcept
{
#if defined(XR_PLATFORM_WINDOWS)
    // Since Vista the thread cycle time is counted with the TSC at context switches: the same unit as Ticks().
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    return static_cast<u64>(cycles);
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    const u64 ns = static_cast<u64>(ts.tv_sec) * 1000000000ULL + static_cast<u64>(ts.tv_nsec);
    return static_cast<u64>(static_cast<double>(ns) * static_cast<double>(GetTicksFrequency()) / 1e9);
#else
    return 0;
#endif
}

XRCORE_API const char* SpikeClassName(const std::type_info& type) noexcept
{
    const char* name = type.name();
    if (0 == std::strncmp(name, "class ", 6))
        return name + 6;
    if (0 == std::strncmp(name, "struct ", 7))
        return name + 7;
    return name;
}

XRCORE_API void SpikeScope::Begin(bool want_cpu) noexcept
{
    cpu = want_cpu || g_perf_spike_cpu != 0;
    parent = tl_spike_top;
    tl_spike_top = this;
    if (cpu)
        cpu0 = ThreadCpuTicks();
    t0 = Ticks();
}

namespace
{
const char* SpikeThreadName() noexcept
{
    if (!tl_spike_thread_name_read)
    {
        tl_spike_thread_name_read = true;
#if defined(XR_PLATFORM_WINDOWS)
        // Threading::SetCurrentThreadName prefers SetThreadDescription, so the name is readable back the same way
        // (as xrDebug's crash report does).
        using GetThreadDescriptionProc = HRESULT(WINAPI*)(HANDLE, PWSTR*);
        const HMODULE kernel = GetModuleHandleA("kernel32.dll");
        const auto get_description =
            kernel ? reinterpret_cast<GetThreadDescriptionProc>(GetProcAddress(kernel, "GetThreadDescription")) : nullptr;
        PWSTR description = nullptr;
        if (get_description && SUCCEEDED(get_description(GetCurrentThread(), &description)) && description)
        {
            size_t converted = 0;
            wcstombs_s(&converted, tl_spike_thread_name, sizeof(tl_spike_thread_name), description,
                sizeof(tl_spike_thread_name) - 1);
            LocalFree(description);
        }
#endif
        for (char* c = tl_spike_thread_name; *c; ++c)
        {
            if (*c == ' ')
                *c = '_';
        }
    }
    if (tl_spike_thread_name[0])
        return tl_spike_thread_name;
    switch (CurrentThreadKind())
    {
    case ThreadKind::Main: return "main";
    case ThreadKind::Game: return "Game";
    default: return "worker";
    }
}

u32 SpikeThreadId() noexcept
{
#if defined(XR_PLATFORM_WINDOWS)
    return static_cast<u32>(GetCurrentThreadId());
#else
    return static_cast<u32>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

void SpikeReport(const SpikeScope& scope, u64 wall, u64 cpu_ticks) noexcept
{
    Add(Metric::perf_spike_hits);
    if (scope.site >= kMaxSpikeSites)
        return;
    SpikeSiteStat& stat = s_spike_sites[scope.site];
    stat.hits.fetch_add(1, std::memory_order_relaxed);
    stat.sum_ticks.fetch_add(wall, std::memory_order_relaxed);
    u64 cur = stat.max_ticks.load(std::memory_order_relaxed);
    while (cur < wall && !stat.max_ticks.compare_exchange_weak(cur, wall, std::memory_order_relaxed)) {}

    if (s_spike_window_lines.fetch_add(1, std::memory_order_relaxed) >= static_cast<u32>(std::max(g_perf_spike_lines, 0)))
    {
        stat.suppressed.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    string32 cpu_text;
    if (scope.cpu)
        xr_sprintf(cpu_text, "%lluus", static_cast<unsigned long long>(TicksToMs(cpu_ticks) * 1000.0));
    else
        xr_strcpy(cpu_text, "-");

    string512 detail{};
    if (scope.describe && scope.ctx)
        scope.describe(scope.ctx, detail, sizeof(detail));

    // outer marked scopes of this thread, innermost first: "in=ucl<level/objects" etc.
    string256 outer{};
    u32 depth = 0;
    for (const SpikeScope* p = scope.parent; p && depth < 6; p = p->parent, ++depth)
    {
        if (p->site >= kMaxSpikeSites)
            continue;
        if (outer[0])
            xr_strcat(outer, "<");
        xr_strcat(outer, s_spike_sites[p->site].name);
    }

    Msg("! [perf-spike] %s %lluus cpu=%s %s%sthread=%s#%u frame=%u in=%s", s_spike_sites[scope.site].name,
        static_cast<unsigned long long>(TicksToMs(wall) * 1000.0), cpu_text, detail, detail[0] ? " " : "",
        SpikeThreadName(), SpikeThreadId(), Core.dwFrame, outer[0] ? outer : "-");
}

// Once per frame from Flush() (main thread): threshold to Ticks(), and the summary of the 5 s window.
void SpikeFrame() noexcept
{
    const int us = g_perf_spike_us;
    g_perf_spike_ticks = us > 0 ? std::max<u64>(1, static_cast<u64>(static_cast<double>(us) *
        static_cast<double>(GetTicksFrequency()) / 1e6)) : 0;

    const u32 now_ms = CPU::GetTicks();
    if (s_spike_window_start_ms == 0)
        s_spike_window_start_ms = now_ms;
    const u32 elapsed_ms = now_ms - s_spike_window_start_ms;
    if (elapsed_ms < kSpikeWindowMs)
        return;
    s_spike_window_start_ms = now_ms;

    const u32 count = s_spike_site_count.load(std::memory_order_acquire);
    u32 total_hits = 0;
    u32 total_suppressed = 0;
    for (u32 i = 0; i < count; ++i)
    {
        total_hits += s_spike_sites[i].hits.load(std::memory_order_relaxed);
        total_suppressed += s_spike_sites[i].suppressed.load(std::memory_order_relaxed);
    }
    s_spike_window_lines.store(0, std::memory_order_relaxed);
    if (!total_hits)
        return;

    Msg("! [perf-spike] summary window=%ums threshold=%dus spikes=%u not_printed=%u (site hits max avg not_printed)",
        elapsed_ms, us, total_hits, total_suppressed);
    for (u32 i = 0; i < count; ++i)
    {
        SpikeSiteStat& stat = s_spike_sites[i];
        const u32 hits = stat.hits.exchange(0, std::memory_order_relaxed);
        const u32 suppressed = stat.suppressed.exchange(0, std::memory_order_relaxed);
        const u64 max_ticks = stat.max_ticks.exchange(0, std::memory_order_relaxed);
        const u64 sum_ticks = stat.sum_ticks.exchange(0, std::memory_order_relaxed);
        if (!hits)
            continue;
        Msg("!   [perf-spike] %s hits=%u max=%lluus avg=%lluus not_printed=%u", stat.name, hits,
            static_cast<unsigned long long>(TicksToMs(max_ticks) * 1000.0),
            static_cast<unsigned long long>(TicksToMs(sum_ticks) * 1000.0 / hits), suppressed);
    }
}
} // namespace

XRCORE_API void SpikeScope::End() noexcept
{
    const u64 wall = Ticks() - t0;
    tl_spike_top = parent;
    if (wall < threshold)
        return;
    const u64 cpu_ticks = cpu ? ThreadCpuTicks() - cpu0 : 0;
    SpikeReport(*this, wall, cpu_ticks);
}

// ---------------------------------------------------------------------------------------------------------------------
// 50n-frame-spike-report.md: GPU time per engine frame (PerfMetrics.hpp, GpuFrameTimeSubmit). One 64-bit slot per
// frame_id % 16: the frame id in the high half, the float bits in the low half - written and read with one atomic
// access, so a reader never sees the time of one frame with the id of another.
namespace
{
constexpr u32 kGpuFrameRing = 16;
std::atomic<u64> s_gpu_frame_ring[kGpuFrameRing]{};
std::atomic<bool> s_gpu_frame_any{false}; // a GPU timer exists (the 60 s summary prints "gpu ?" otherwise)
} // namespace

XRCORE_API void GpuFrameTimeSubmit(u32 frame_id, float gpu_ms) noexcept
{
    u32 bits;
    static_assert(sizeof(bits) == sizeof(gpu_ms));
    std::memcpy(&bits, &gpu_ms, sizeof(bits));
    s_gpu_frame_ring[frame_id % kGpuFrameRing].store((static_cast<u64>(frame_id) << 32) | bits, std::memory_order_release);
    if (!s_gpu_frame_any.load(std::memory_order_relaxed))
        s_gpu_frame_any.store(true, std::memory_order_relaxed);
}

XRCORE_API bool GpuFrameTimeGet(u32 frame_id, float& gpu_ms) noexcept
{
    const u64 v = s_gpu_frame_ring[frame_id % kGpuFrameRing].load(std::memory_order_acquire);
    if (v == 0 || static_cast<u32>(v >> 32) != frame_id)
        return false;
    const u32 bits = static_cast<u32>(v);
    std::memcpy(&gpu_ms, &bits, sizeof(gpu_ms));
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// 50n-frame-spike-report.md: the frame spike report and the 60 s frame window (PerfMetrics.hpp). Always compiled. Once
// per frame from Flush() on main: the wall time since the previous Flush(), the phases, the CPU time of main and of the
// game logic, the I/O and page faults of the process, the event counters; a game frame over the threshold is kept until
// its GPU time arrives (at most 3 frames) and then printed as two "! [frame-spike]" lines.
XRCORE_API int g_perf_frame_spike_ms = 50;
// LuaDispatchNote (PerfMetrics.hpp): latched once per frame by FrameReportTick, off until the first one
XRCORE_API std::atomic<u64> g_lua_dispatch_floor_ticks{u64(-1)};
XRCORE_API std::atomic<u64> g_lua_slow_ticks{0};

namespace
{
std::atomic<u64> s_game_wall_ticks{0};
std::atomic<u64> s_game_cpu_ticks{0};
std::atomic<u64> s_event_ticks[frame_event::Count]{};
std::atomic<u32> s_event_count[frame_event::Count]{};

// the frame context of FrameSetContext (main thread: set in ProcessFrame, taken by the next Flush)
bool s_ctx_set = false;
bool s_ctx_in_game = false;
u32 s_ctx_session_s = 0;
u32 s_ctx_day_s = 0;

constexpr const char* kPhaseNames[] = { "-", "bf", "fm", "ui", "ev", "ucl", "lua", "luaev", "rt", "pp", "gw", "r",
    "rb", "rs", "uir", "pres", "phw", "prw", "phf", "sleep", "snd", "logic", "sched", "alife", "vis", "par", "mt", "sndw",
    "luaev" };
static_assert(sizeof(kPhaseNames) / sizeof(kPhaseNames[0]) == frame_phase::Count, "kPhaseNames != frame_phase");
constexpr const char* kEventNames[frame_event::Count] = { "model", "snd", "sset", "tex", "spawn", "gc", "destroy",
    "save", "iow", "lockw", "texa", "shc", "shcc", "rtc" };

// Process I/O and page faults (cumulative): Windows - GetProcessIoCounters (all ReadFile calls, the file cache hits
// included; the mapped archives are read through page faults instead), PageFaultCount (soft + hard), and the
// system-wide hard faults and the free RAM from NtQuerySystemInformation(SystemPerformanceInformation); Linux / BSD -
// getrusage: blocks read from the disk, minor + major faults, major faults of the process.
struct IoSnap
{
    u64 read_ops{};
    u64 read_bytes{};
    u64 faults{};
    u64 hard{};     // Windows: pages read from the disk by the hard faults of the whole system; else: major faults
    u32 avail_mb{}; // Windows: free physical memory; else 0 (read on a spike only)
};

#if defined(XR_PLATFORM_WINDOWS)
using NtQuerySystemInformationProc = LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);
NtQuerySystemInformationProc s_nt_query_sysinfo = nullptr;
bool s_nt_query_sysinfo_init = false;
u32 s_page_size = 4096;
#endif

void IoQuery(IoSnap& s) noexcept
{
#if defined(XR_PLATFORM_WINDOWS)
    IO_COUNTERS io{};
    if (GetProcessIoCounters(GetCurrentProcess(), &io))
    {
        s.read_ops = io.ReadOperationCount;
        s.read_bytes = io.ReadTransferCount;
    }
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        s.faults = pmc.PageFaultCount;
    if (!s_nt_query_sysinfo_init)
    {
        s_nt_query_sysinfo_init = true;
        if (const HMODULE ntdll = GetModuleHandleA("ntdll.dll"))
            s_nt_query_sysinfo = reinterpret_cast<NtQuerySystemInformationProc>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        if (si.dwPageSize)
            s_page_size = si.dwPageSize;
    }
    if (s_nt_query_sysinfo)
    {
        // SYSTEM_PERFORMANCE_INFORMATION (class 2, 376 bytes on Windows 10/11 x64; the head is stable since NT 4):
        // AvailablePages at 44, PageReadCount (pages read by the hard faults) at 80. Checked against GlobalMemoryStatusEx
        // and the "Memory\Pages Input/sec" counter (50n journal).
        alignas(8) u8 buf[512];
        ULONG len = 0;
        if (s_nt_query_sysinfo(2, buf, sizeof(buf), &len) >= 0 && len >= 88)
        {
            u32 avail_pages, page_reads;
            std::memcpy(&avail_pages, buf + 44, sizeof(avail_pages));
            std::memcpy(&page_reads, buf + 80, sizeof(page_reads));
            s.hard = page_reads;
            s.avail_mb = static_cast<u32>(static_cast<u64>(avail_pages) * s_page_size / (1024 * 1024));
        }
    }
#else
    rusage ru{};
    if (0 == getrusage(RUSAGE_SELF, &ru))
    {
        s.read_ops = static_cast<u64>(ru.ru_inblock);
        s.read_bytes = static_cast<u64>(ru.ru_inblock) * 512;
        s.faults = static_cast<u64>(ru.ru_minflt) + static_cast<u64>(ru.ru_majflt);
        s.hard = static_cast<u64>(ru.ru_majflt);
    }
#endif
}

u32 AvailMbSlow() noexcept
{
#if defined(XR_PLATFORM_WINDOWS)
    return 0;
#elif defined(_SC_AVPHYS_PAGES)
    const long pages = sysconf(_SC_AVPHYS_PAGES);
    const long page = sysconf(_SC_PAGESIZE);
    return pages > 0 && page > 0 ? static_cast<u32>(static_cast<u64>(pages) * static_cast<u64>(page) / (1024 * 1024)) : 0;
#else
    return 0;
#endif
}

// The thread CPU clock per Ticks() tick: 1 unless the calibration measured clearly more (a kernel counting core cycles)
double ThreadCpuScale() noexcept
{
    return s_thread_cycles_per_tick > 1.25 ? s_thread_cycles_per_tick : 1.0;
}

// Frame time histogram (ms): 1 ms buckets below 100 ms, 10 ms buckets below 1000 ms, then one bucket
constexpr u32 kHistBuckets = 100 + 90 + 1;

u32 HistBucket(double ms) noexcept
{
    if (ms < 0.0)
        return 0;
    if (ms < 100.0)
        return static_cast<u32>(ms);
    if (ms < 1000.0)
        return 100 + static_cast<u32>((ms - 100.0) / 10.0);
    return kHistBuckets - 1;
}

double HistBucketTop(u32 bucket) noexcept
{
    if (bucket < 100)
        return bucket + 1.0;
    if (bucket < kHistBuckets - 1)
        return 100.0 + (bucket - 100 + 1) * 10.0;
    return 1000.0;
}

// The upper bound of the bucket holding the q-th fraction of the samples (capped by the exact maximum)
double HistPercentile(const u32* hist, u32 total, double q, double max_ms) noexcept
{
    if (!total)
        return 0.0;
    const u64 rank = static_cast<u64>(q * total + 0.999999);
    u64 acc = 0;
    for (u32 i = 0; i < kHistBuckets; ++i)
    {
        acc += hist[i];
        if (acc >= rank)
            return std::min(HistBucketTop(i), max_ms);
    }
    return max_ms;
}

struct FrameWindow
{
    u32 start_ms{};
    u32 frames{};
    double sum_ms{};
    double max_ms{};
    u32 hist[kHistBuckets]{};
    u32 spikes{};
    u32 warmup_spikes{}, exit_spikes{}; // counted only (FrameSessionMark): after a load / from an exit to the next load
    u32 main_busy{}; // frames with the main thread's CPU time >= 80% of its wall time (the fps sleep excluded)
    double main_cpu_ms{}, main_wall_ms{}, game_cpu_ms{}, game_wall_ms{};
    // the GPU (50o-gpu-frame-timing.md): the timestamp span of the frame; the CPU waits for the GPU of the frame (the
    // sync point + Present); gpu_bound = frames with these waits >= 30% of the frame; the GPU idle gap before it
    u32 gpu_frames{};
    double gpu_sum_ms{}, gpu_max_ms{};
    u32 gpu_hist[kHistBuckets]{};
    u32 wait_frames{}, gpu_bound{};
    double wait_sum_ms{}, wait_max_ms{};
    u32 wait_hist[kHistBuckets]{};
    u32 gap_frames{};
    double gap_sum_ms{};
    u64 read_ops{}, read_bytes{}, faults{}, hard{};
    u32 avail_min_mb{};
    u64 ev_count[frame_event::Count]{};
    u64 ev_ticks[frame_event::Count]{};
    double phase_ms[frame_phase::Count]{};
};
FrameWindow s_window;

// The last game frames (wall ms) for the running median
constexpr u32 kMedianRing = 128;
float s_median_ring[kMedianRing]{};
u32 s_median_n = 0;
u32 s_median_pos = 0;
float s_median_ms = 0.f;
u32 s_median_age = 0;
constexpr u32 kMedianMinFrames = 60; // fewer game frames in the ring: the absolute threshold only

// The last frames' wall time for the late GPU results (the window takes frame N - 3)
struct RecentFrame
{
    u32 frame{};
    float wall_ms{};
    bool game{};
};
constexpr u32 kRecentFrames = 8;
RecentFrame s_recent[kRecentFrames]{};

// A spike waiting for its GPU time
struct SpikeRecord
{
    bool used{};
    u32 frame{};
    u32 age{};
    float wall_ms{}, median_ms{}, threshold_ms{};
    u32 session_s{}, day_s{};
    float main_cpu_ms{}, game_wall_ms{}, game_cpu_ms{}, game_on_main_ms{};
    float phase_ms[frame_phase::Count]{};
    IoSnap io{};
    u32 ev_count[frame_event::Count]{};
    float ev_ms[frame_event::Count]{};
    char lua[192]{}; // the slowest named Lua subscriber call of the frame (LuaDispatchNote): "<event> -> <subscriber>"
    float lua_ms{};
    char top[256]{};
    char cpu[48]{}; // 50z5-extra-diagnostics.md: the CPU frequency at the spike (SysDiag.cpp, CpuFreqNow)
};
constexpr u32 kPendingSpikes = 4;
SpikeRecord s_pending[kPendingSpikes];
constexpr u32 kSpikeGpuWaitFrames = 3;

// Throttling: at most kSpikeReportsPerWindow reports (2 lines each) per 10 s, the rest is counted
constexpr u32 kSpikeThrottleMs = 10000;
constexpr u32 kSpikeReportsPerWindow = 10;
u32 s_throttle_start_ms = 0;
u32 s_throttle_reports = 0;
u32 s_throttle_suppressed = 0;
float s_throttle_suppressed_max = 0.f;

// FrameWindowReport: the request of a window line, printed by the next Flush() on main
std::mutex s_window_req_lock;
string32 s_window_req_where{};
std::atomic<bool> s_window_req{false};
void WindowReportPrint(const char* where) noexcept;

// 50z5-extra-diagnostics.md: the present-to-present interval of a frame (PresentWaitSubmit, the 50o block below)
bool PresentIntervalGet(u32 frame_id, float& ms) noexcept;
// 50z5: the phase of the main thread for the crash context (its thread-locals, taken by the first FrameReportTick)
const u8* s_main_phase_ptr = nullptr;
const u64* s_main_phase_t0_ptr = nullptr;
u32 s_last_wall_ms_x10 = 0; // the last frame's wall time, 0.1 ms

u64 s_report_prev_ticks = 0;
u64 s_report_prev_cpu = 0;
IoSnap s_report_prev_io{};
bool s_report_have_prev = false;
bool s_report_thread_logged = false;

// FrameSessionMark (50n, Lua events and the spike filter): the warmup after a load, the exit until the next load
enum class ESession : u8
{
    Play,
    Warmup, // after LoadEnd: until kWarmupFrames game frames and kWarmupMs since the first of them
    Exit,   // from Exit until the next LoadEnd (or kExitPlayMs of game frames: a mark without a load after it)
};
constexpr u32 kWarmupFrames = 120;
constexpr u32 kWarmupMs = 3000;
constexpr double kExitPlayMs = 10000.0;
std::atomic<u8> s_session_marks{0}; // frame_session::* bits since the last Flush()
ESession s_session = ESession::Play;
u32 s_warmup_frames = 0;
u32 s_warmup_start_ms = 0;
double s_exit_play_ms = 0.0;

// LuaDispatchNote: the slowest named call of the frame and the throttling of the [lua-slow] lines (any thread that runs
// the game logic; Flush() on main takes the frame's call)
constexpr double kLuaNoteFloorMs = 1.0;
constexpr double kLuaSlowShare = 0.4; // the [lua-slow] threshold: this share of perf_frame_spike_ms
constexpr u32 kLuaSlowLinesPerWindow = 10; // per kSpikeThrottleMs
std::mutex s_lua_lock;
u64 s_lua_frame_ticks = 0; // under s_lua_lock
char s_lua_frame_what[192]{};
u32 s_lua_throttle_start_ms = 0;
u32 s_lua_throttle_lines = 0;
u32 s_lua_throttle_suppressed = 0;
float s_lua_throttle_max_ms = 0.f;
u64 s_lua_floor_base = u64(-1); // the floor of a new frame
std::atomic<bool> s_lua_pending{false}; // the frame's call or suppressed lines are there: Flush() takes the lock
std::atomic<bool> s_lua_lines_on{false}; // the last frame was a game frame outside the warmup / exit

// Under s_lua_lock: the summary of the suppressed [lua-slow] lines when their 10 s window ended, a new window
void LuaThrottleRoll(u32 now_ms, char* summary, size_t size) noexcept
{
    if (s_lua_throttle_start_ms == 0)
        s_lua_throttle_start_ms = now_ms;
    if (now_ms - s_lua_throttle_start_ms < kSpikeThrottleMs)
        return;
    if (s_lua_throttle_suppressed)
    {
        xr_sprintf(summary, size, "! [lua-slow] ... %u more slow Lua dispatches suppressed in the last %u s (max %.1f ms)",
            s_lua_throttle_suppressed, (now_ms - s_lua_throttle_start_ms) / 1000, s_lua_throttle_max_ms);
    }
    s_lua_throttle_start_ms = now_ms;
    s_lua_throttle_lines = 0;
    s_lua_throttle_suppressed = 0;
    s_lua_throttle_max_ms = 0.f;
}

// Once per frame from FrameReportTick (main): latches the thresholds of the next frame, takes the slowest named call of
// this one ("" when none), prints the summary of the suppressed lines of an ended window
void LuaFrameTick(u32 now_ms, char* what, size_t size, float& ms) noexcept
{
    what[0] = 0;
    ms = 0.f;
    const int abs_ms = g_perf_frame_spike_ms;
    const double ticks_per_ms = static_cast<double>(GetTicksFrequency()) / 1000.0;
    const u64 slow = abs_ms > 0 ? std::max<u64>(1, static_cast<u64>(abs_ms * kLuaSlowShare * ticks_per_ms)) : 0;
    s_lua_floor_base = abs_ms > 0 ?
        std::min<u64>(slow, std::max<u64>(1, static_cast<u64>(kLuaNoteFloorMs * ticks_per_ms))) : u64(-1);
    g_lua_slow_ticks.store(slow, std::memory_order_relaxed);
    if (!s_lua_pending.load(std::memory_order_acquire))
    {
        g_lua_dispatch_floor_ticks.store(s_lua_floor_base, std::memory_order_relaxed);
        return;
    }
    string256 summary{};
    {
        std::lock_guard<std::mutex> lock(s_lua_lock);
        if (s_lua_frame_ticks)
        {
            xr_strcpy(what, size, s_lua_frame_what);
            ms = static_cast<float>(TicksToMs(s_lua_frame_ticks));
            s_lua_frame_ticks = 0;
        }
        g_lua_dispatch_floor_ticks.store(s_lua_floor_base, std::memory_order_relaxed);
        if (s_lua_throttle_suppressed)
            LuaThrottleRoll(now_ms, summary, sizeof(summary));
        s_lua_pending.store(s_lua_throttle_suppressed != 0, std::memory_order_relaxed);
    }
    if (summary[0])
        Msg("%s", summary);
}

void AppendF(char* buf, size_t size, const char* fmt, ...) noexcept
{
    const size_t len = std::strlen(buf);
    if (len + 1 >= size)
        return;
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf + len, size - len, fmt, args);
    va_end(args);
}

// "model 3/120.4 snd 2/280.1 ..." for the events with a count
void AppendEvents(char* buf, size_t size, const u32* counts, const float* ms) noexcept
{
    bool any = false;
    for (u32 e = 0; e < frame_event::Count; ++e)
    {
        if (!counts[e])
            continue;
        AppendF(buf, size, "%s%s %u/%.1f", any ? " " : "", kEventNames[e], counts[e], ms[e]);
        any = true;
    }
    if (!any)
        AppendF(buf, size, "-");
}

// " gpu 7.9 sync 3.1 pwait 0.2 gap 1.0" (50o-gpu-frame-timing.md), "?" for what the render did not submit
void AppendGpu(char* buf, size_t size, u32 frame) noexcept
{
    float v = 0.f;
    if (GpuFrameTimeGet(frame, v))
        AppendF(buf, size, " gpu %.1f", v);
    else
        AppendF(buf, size, " gpu ?");
    if (GpuSyncWaitGet(frame, v))
        AppendF(buf, size, " sync %.1f", v);
    else
        AppendF(buf, size, " sync ?");
    if (PresentWaitGet(frame, v))
        AppendF(buf, size, " pwait %.1f", v);
    else
        AppendF(buf, size, " pwait ?");
    if (GpuIdleGapGet(frame, v))
        AppendF(buf, size, " gap %.1f", v);
}

void PrintSpike(const SpikeRecord& r) noexcept
{
    string1024 line1{};
    const u32 day_h = r.day_s / 3600, day_m = (r.day_s / 60) % 60;
    AppendF(line1, sizeof(line1), "! [frame-spike] #%u %.1fms (med %.1f thr %.0f) t=%us %02u:%02u | main cpu %.1f | "
        "game %.1f cpu %.1f", r.frame, r.wall_ms, r.median_ms, r.threshold_ms, r.session_s, day_h, day_m,
        r.main_cpu_ms, r.game_wall_ms, r.game_cpu_ms);
    AppendF(line1, sizeof(line1), " |");
    AppendGpu(line1, sizeof(line1), r.frame);
    AppendF(line1, sizeof(line1), " | io %llu/%.1fMB pf %llu hf %llu", static_cast<unsigned long long>(r.io.read_ops),
        static_cast<double>(r.io.read_bytes) / (1024.0 * 1024.0), static_cast<unsigned long long>(r.io.faults),
        static_cast<unsigned long long>(r.io.hard));
    if (r.io.avail_mb)
        AppendF(line1, sizeof(line1), " ram %uMB", r.io.avail_mb);
    AppendF(line1, sizeof(line1), " | ");
    AppendEvents(line1, sizeof(line1), r.ev_count, r.ev_ms);
    if (r.lua[0])
        AppendF(line1, sizeof(line1), " | lua-slowest %s %.1fms", r.lua, r.lua_ms);
    if (r.cpu[0])
        AppendF(line1, sizeof(line1), " | %s", r.cpu);
    Msg("%s", line1);
    detail::CrashRingPush(line1);

    // main: its phases + other (they add up to the wall time); game: its phases + other (they add up to "game")
    string1024 line2{};
    AppendF(line2, sizeof(line2), "!   [frame-spike] #%u main:", r.frame);
    double main_sum = 0.0;
    for (u8 p = 1; p < frame_phase::MainEnd; ++p)
    {
        main_sum += r.phase_ms[p];
        if (r.phase_ms[p] >= 0.1f)
            AppendF(line2, sizeof(line2), " %s %.1f", kPhaseNames[p], r.phase_ms[p]);
    }
    const double main_other = std::max(0.0, r.wall_ms - main_sum - r.game_on_main_ms);
    AppendF(line2, sizeof(line2), " other %.1f | game:", main_other);
    double game_sum = 0.0;
    for (u8 p = frame_phase::MainEnd; p < frame_phase::Count; ++p)
    {
        game_sum += r.phase_ms[p];
        if (r.phase_ms[p] >= 0.1f)
            AppendF(line2, sizeof(line2), " %s %.1f", kPhaseNames[p], r.phase_ms[p]);
    }
    AppendF(line2, sizeof(line2), " other %.1f", std::max(0.0, r.game_wall_ms - game_sum));
    if (r.game_on_main_ms >= 0.1f)
        AppendF(line2, sizeof(line2), " (on main %.1f)", r.game_on_main_ms);
    if (r.top[0])
        AppendF(line2, sizeof(line2), " | top %s", r.top);
    Msg("%s", line2);
    detail::CrashRingPush(line2);
}

void ThrottleTick(u32 now_ms) noexcept
{
    if (s_throttle_start_ms == 0)
        s_throttle_start_ms = now_ms;
    if (now_ms - s_throttle_start_ms < kSpikeThrottleMs)
        return;
    if (s_throttle_suppressed)
    {
        Msg("! [frame-spike] ... %u more spikes suppressed in the last %u s (max %.1fms)", s_throttle_suppressed,
            (now_ms - s_throttle_start_ms) / 1000, s_throttle_suppressed_max);
    }
    s_throttle_start_ms = now_ms;
    s_throttle_reports = 0;
    s_throttle_suppressed = 0;
    s_throttle_suppressed_max = 0.f;
}

// Prints the pending spikes whose GPU time arrived or waited long enough (all of them with `all`)
void FlushPendingSpikes(bool all) noexcept
{
    for (SpikeRecord& r : s_pending)
    {
        if (!r.used)
            continue;
        float gpu_ms = 0.f;
        const bool known = GpuFrameTimeGet(r.frame, gpu_ms);
        if (!known && !all && r.age < kSpikeGpuWaitFrames)
        {
            ++r.age;
            continue;
        }
        PrintSpike(r);
        r.used = false;
    }
}

float UpdateMedian(float wall_ms) noexcept
{
    s_median_ring[s_median_pos] = wall_ms;
    s_median_pos = (s_median_pos + 1) % kMedianRing;
    if (s_median_n < kMedianRing)
        ++s_median_n;
    // nth_element over <= 128 floats every 8th frame: ~1 us
    if (++s_median_age >= 8 || s_median_n < kMedianMinFrames)
    {
        s_median_age = 0;
        float tmp[kMedianRing];
        std::memcpy(tmp, s_median_ring, s_median_n * sizeof(float));
        std::nth_element(tmp, tmp + s_median_n / 2, tmp + s_median_n);
        s_median_ms = tmp[s_median_n / 2];
    }
    return s_median_ms;
}

void WindowReset(u32 now_ms) noexcept
{
    s_window = FrameWindow{};
    s_window.start_ms = now_ms;
}

// Once per frame, at the start of Flush() (main thread)
void FrameReportTick() noexcept
{
    if (!s_main_phase_ptr)
    {
        s_main_phase_ptr = &tl_phase;
        s_main_phase_t0_ptr = &tl_phase_t0;
    }
    const u64 now = Ticks();
    const u64 cpu_now = ThreadCpuTicks();
    IoSnap io_now;
    IoQuery(io_now);

    // take the per-frame sums whatever the frame was
    float phase_ms[frame_phase::Count];
    for (u8 p = 0; p < frame_phase::Count; ++p)
        phase_ms[p] = static_cast<float>(TicksToMs(s_phase_ticks[p].exchange(0, std::memory_order_relaxed)));
    const float game_on_main_ms = static_cast<float>(TicksToMs(s_game_on_main_ticks.exchange(0, std::memory_order_relaxed)));
    const double cpu_scale = ThreadCpuScale();
    const float game_wall_ms = static_cast<float>(TicksToMs(s_game_wall_ticks.exchange(0, std::memory_order_relaxed)));
    const float game_cpu_ms =
        static_cast<float>(TicksToMs(s_game_cpu_ticks.exchange(0, std::memory_order_relaxed)) / cpu_scale);
    u32 ev_count[frame_event::Count];
    u64 ev_ticks[frame_event::Count];
    for (u8 e = 0; e < frame_event::Count; ++e)
    {
        ev_count[e] = s_event_count[e].exchange(0, std::memory_order_relaxed);
        ev_ticks[e] = s_event_ticks[e].exchange(0, std::memory_order_relaxed);
    }

    const bool have_prev = s_report_have_prev;
    const u64 wall_ticks = now - s_report_prev_ticks;
    const u64 cpu_ticks = cpu_now - s_report_prev_cpu;
    const IoSnap io_prev = s_report_prev_io;
    s_report_prev_ticks = now;
    s_report_prev_cpu = cpu_now;
    s_report_prev_io = io_now;
    s_report_have_prev = true;

    const bool in_game = s_ctx_set && s_ctx_in_game;
    s_ctx_set = false;
    const u32 frame = Core.dwFrame;
    const u32 now_ms = CPU::GetTicks();
    if (s_window.start_ms == 0)
        s_window.start_ms = now_ms;

    // 50n Lua events: the slowest named Lua subscriber call of the frame, the thresholds of the next frame
    char lua_what[192];
    float lua_ms = 0.f;
    LuaFrameTick(now_ms, lua_what, sizeof(lua_what), lua_ms);
    // FrameSessionMark: an exit, then the load end of the next level (both in one frame: the load end wins)
    const u8 marks = s_session_marks.exchange(0, std::memory_order_acquire);
    if (marks & frame_session::Exit)
    {
        s_session = ESession::Exit;
        s_exit_play_ms = 0.0;
    }
    if (marks & frame_session::LoadEnd)
    {
        // the level is in play: its warmup, and the running median of this level only
        s_session = ESession::Warmup;
        s_warmup_frames = 0;
        s_warmup_start_ms = 0;
        s_median_n = 0;
        s_median_pos = 0;
        s_median_age = 0;
        s_median_ms = 0.f;
    }

    const float wall_ms = static_cast<float>(TicksToMs(wall_ticks));
    s_last_wall_ms_x10 = static_cast<u32>(std::min(wall_ms * 10.f, 4.0e9f));
    // 50z5: the present interval counts when the previous frame was a game frame too (not the first after a load)
    const RecentFrame& before = s_recent[(frame - 1) % kRecentFrames];
    const bool before_game = before.game && before.frame == frame - 1;
    RecentFrame& recent = s_recent[frame % kRecentFrames];
    recent.frame = frame;
    recent.wall_ms = wall_ms;
    recent.game = in_game && have_prev;

    ThrottleTick(now_ms);
    FlushPendingSpikes(false);
    if (s_window_req.load(std::memory_order_acquire))
    {
        string32 where;
        {
            std::lock_guard<std::mutex> lock(s_window_req_lock);
            xr_strcpy(where, s_window_req_where);
            s_window_req.store(false, std::memory_order_relaxed);
        }
        WindowReportPrint(where);
    }

    if (!in_game || !have_prev)
    {
        s_lua_lines_on.store(false, std::memory_order_relaxed);
        return;
    }

    if (!s_report_thread_logged)
    {
        s_report_thread_logged = true;
        Msg("* [frame] spike report: threshold max(%d ms, 3 x median), median from %u frames, not reported %u s / %u "
            "frames after a load and from an exit to the next load, lua-slow %.0f ms, thread cycles per TSC tick %.3f "
            "(scale %.3f)", g_perf_frame_spike_ms, kMedianMinFrames, kWarmupMs / 1000, kWarmupFrames,
            g_perf_frame_spike_ms * kLuaSlowShare, s_thread_cycles_per_tick, cpu_scale);
    }

    // the warmup after a load and the exit (FrameSessionMark): the spikes of these frames are only counted
    ESession quiet = ESession::Play;
    if (s_session == ESession::Warmup)
    {
        if (!s_warmup_frames)
            s_warmup_start_ms = now_ms;
        ++s_warmup_frames;
        if (s_warmup_frames > kWarmupFrames && now_ms - s_warmup_start_ms >= kWarmupMs)
            s_session = ESession::Play;
        else
            quiet = ESession::Warmup;
    }
    else if (s_session == ESession::Exit)
    {
        // a mark without a load after it (a level change that did not happen): play again after kExitPlayMs
        s_exit_play_ms += wall_ms;
        if (s_exit_play_ms >= kExitPlayMs)
        {
            s_session = ESession::Play;
            Msg("* [frame] spike report: %.0f s of play after an exit mark without a load, spikes are reported again",
                kExitPlayMs / 1000.0);
        }
        else
            quiet = ESession::Exit;
    }
    s_lua_lines_on.store(s_session == ESession::Play, std::memory_order_relaxed);

    const float main_cpu_ms = static_cast<float>(TicksToMs(cpu_ticks) / cpu_scale);
    IoSnap io;
    io.read_ops = io_now.read_ops - io_prev.read_ops;
    io.read_bytes = io_now.read_bytes - io_prev.read_bytes;
    io.faults = io_now.faults - io_prev.faults;
    io.hard = io_now.hard >= io_prev.hard ? io_now.hard - io_prev.hard : 0; // a 32-bit system counter wraps
    io.avail_mb = io_now.avail_mb;

    // the window
    FrameWindow& w = s_window;
    ++w.frames;
    w.sum_ms += wall_ms;
    w.max_ms = std::max<double>(w.max_ms, wall_ms);
    ++w.hist[HistBucket(wall_ms)];
    const float busy_wall_ms = std::max(0.f, wall_ms - phase_ms[frame_phase::FpsSleep]);
    if (busy_wall_ms > 0.f && main_cpu_ms >= 0.8f * busy_wall_ms)
        ++w.main_busy;
    w.main_cpu_ms += main_cpu_ms;
    w.main_wall_ms += busy_wall_ms;
    w.game_cpu_ms += game_cpu_ms;
    w.game_wall_ms += game_wall_ms;
    w.read_ops += io.read_ops;
    w.read_bytes += io.read_bytes;
    w.faults += io.faults;
    w.hard += io.hard;
    if (io.avail_mb && (!w.avail_min_mb || io.avail_mb < w.avail_min_mb))
        w.avail_min_mb = io.avail_mb;
    for (u8 e = 0; e < frame_event::Count; ++e)
    {
        w.ev_count[e] += ev_count[e];
        w.ev_ticks[e] += ev_ticks[e];
    }
    for (u8 p = 0; p < frame_phase::Count; ++p)
        w.phase_ms[p] += phase_ms[p];
    {
        // 50z5-extra-diagnostics.md: the frame pacing ([frame] pace line), the end of a load timeline
        float interval_ms = -1.f;
        if (!before_game || !PresentIntervalGet(frame, interval_ms))
            interval_ms = -1.f;
        detail::SysFrameTick(wall_ms, phase_ms[frame_phase::FpsSleep], interval_ms);
    }

    // the GPU time of an earlier game frame (the results arrive late)
    const RecentFrame& late = s_recent[(frame - kSpikeGpuWaitFrames) % kRecentFrames];
    if (late.game && late.frame == frame - kSpikeGpuWaitFrames)
    {
        float gpu_ms = 0.f;
        if (GpuFrameTimeGet(late.frame, gpu_ms))
        {
            ++w.gpu_frames;
            w.gpu_sum_ms += gpu_ms;
            w.gpu_max_ms = std::max<double>(w.gpu_max_ms, gpu_ms);
            ++w.gpu_hist[HistBucket(gpu_ms)];
        }
        // 50o: on D3D11 the sync point keeps the CPU about one frame ahead, so a GPU-bound frame waits there (GL: in
        // the sync point too, vsync: in Present) - GPU-bound = these waits take >= 30% of the frame
        float sync_ms = 0.f, present_ms = 0.f;
        const bool have_sync = GpuSyncWaitGet(late.frame, sync_ms);
        const bool have_present = PresentWaitGet(late.frame, present_ms);
        if (have_sync || have_present)
        {
            const float wait_ms = sync_ms + present_ms;
            ++w.wait_frames;
            w.wait_sum_ms += wait_ms;
            w.wait_max_ms = std::max<double>(w.wait_max_ms, wait_ms);
            ++w.wait_hist[HistBucket(wait_ms)];
            if (late.wall_ms > 0.f && wait_ms >= 0.3f * late.wall_ms)
                ++w.gpu_bound;
        }
        float gap_ms = 0.f;
        if (GpuIdleGapGet(late.frame, gap_ms))
        {
            ++w.gap_frames;
            w.gap_sum_ms += gap_ms;
        }
    }

    // the spike (the exit frames stay out of the median: the next load restarts it anyway)
    const float median_ms = quiet == ESession::Exit ? s_median_ms : UpdateMedian(wall_ms);
    const int abs_ms = g_perf_frame_spike_ms;
    if (abs_ms <= 0)
        return;
    const float threshold_ms =
        std::max(static_cast<float>(abs_ms), s_median_n >= kMedianMinFrames ? 3.f * median_ms : 0.f);
    if (wall_ms < threshold_ms)
        return;
    if (quiet != ESession::Play)
    {
        ++(quiet == ESession::Warmup ? w.warmup_spikes : w.exit_spikes);
        return;
    }
    ++w.spikes;
    if (s_throttle_reports >= kSpikeReportsPerWindow)
    {
        ++s_throttle_suppressed;
        s_throttle_suppressed_max = std::max(s_throttle_suppressed_max, wall_ms);
        return;
    }
    ++s_throttle_reports;

    SpikeRecord* rec = nullptr;
    for (SpikeRecord& r : s_pending)
    {
        if (!r.used)
        {
            rec = &r;
            break;
        }
    }
    if (!rec)
    {
        FlushPendingSpikes(true); // more spikes in a row than slots: print the waiting ones without the GPU time
        rec = &s_pending[0];
    }
    *rec = SpikeRecord{};
    rec->used = true;
    rec->frame = frame;
    rec->wall_ms = wall_ms;
    rec->median_ms = median_ms;
    rec->threshold_ms = threshold_ms;
    rec->session_s = s_ctx_session_s;
    rec->day_s = s_ctx_day_s;
    rec->main_cpu_ms = main_cpu_ms;
    rec->game_wall_ms = game_wall_ms;
    rec->game_cpu_ms = game_cpu_ms;
    rec->game_on_main_ms = game_on_main_ms;
    std::memcpy(rec->phase_ms, phase_ms, sizeof(phase_ms));
    rec->io = io;
    if (!rec->io.avail_mb)
        rec->io.avail_mb = AvailMbSlow();
    xr_strcpy(rec->lua, lua_what);
    rec->lua_ms = lua_ms;
    detail::CpuFreqNow(rec->cpu, sizeof(rec->cpu)); // 50z5: ~0.4 us, printed spikes only
    for (u8 e = 0; e < frame_event::Count; ++e)
    {
        rec->ev_count[e] = ev_count[e];
        rec->ev_ms[e] = static_cast<float>(TicksToMs(ev_ticks[e]));
    }
}

#if XR_PERF_METRICS
// Under XR_PERF_METRICS: the 5 most expensive timers of the spike frame that was just recorded (not the frame/* phases,
// they are in the line already), "name ms" each
void FrameReportAttachTop(const u64* deltas) noexcept
{
    SpikeRecord* rec = nullptr;
    for (SpikeRecord& r : s_pending)
    {
        if (r.used && r.frame == Core.dwFrame && !r.top[0] && r.age == 0)
            rec = &r;
    }
    if (!rec)
        return;
    constexpr u32 top_n = 5;
    size_t best[top_n];
    u64 best_v[top_n]{};
    u32 n = 0;
    for (size_t i = 0; i < kNumMetrics; ++i)
    {
        const MetricDesc& d = s_metric_descs[i];
        if (!d.is_timer_ticks || !deltas[i])
            continue;
        if (0 == std::strncmp(d.base_name, "frame/", 6) || 0 == std::strncmp(d.base_name, "render/split/", 13) ||
            0 == std::strcmp(d.base_name, "game/total"))
            continue;
        const u64 v = deltas[i];
        if (n < top_n)
        {
            best[n] = i;
            best_v[n] = v;
            ++n;
        }
        else
        {
            u32 low = 0;
            for (u32 k = 1; k < top_n; ++k)
            {
                if (best_v[k] < best_v[low])
                    low = k;
            }
            if (v <= best_v[low])
                continue;
            best[low] = i;
            best_v[low] = v;
        }
    }
    for (u32 a = 0; a < n; ++a) // by time, descending
    {
        for (u32 b = a + 1; b < n; ++b)
        {
            if (best_v[b] > best_v[a])
            {
                std::swap(best_v[a], best_v[b]);
                std::swap(best[a], best[b]);
            }
        }
    }
    for (u32 a = 0; a < n; ++a)
    {
        const double ms = TicksToMs(best_v[a]);
        if (ms < 1.0)
            break;
        AppendF(rec->top, sizeof(rec->top), "%s%s %.1f", a ? ", " : "", s_metric_descs[best[a]].base_name, ms);
    }
}
#endif
} // namespace

GameSpanScope::GameSpanScope() noexcept : t0(Ticks()), cpu0(ThreadCpuTicks()) {}

GameSpanScope::~GameSpanScope() noexcept
{
    const u64 cpu = ThreadCpuTicks() - cpu0;
    s_game_wall_ticks.fetch_add(Ticks() - t0, std::memory_order_relaxed);
    s_game_cpu_ticks.fetch_add(cpu, std::memory_order_relaxed);
}

XRCORE_API void FrameEventAdd(u8 event, u64 ticks, u32 count) noexcept
{
    if (event >= frame_event::Count)
        return;
    s_event_count[event].fetch_add(count, std::memory_order_relaxed);
    s_event_ticks[event].fetch_add(ticks, std::memory_order_relaxed);
}

XRCORE_API void FrameSetContext(bool in_game, u32 session_s, u32 game_day_s) noexcept
{
    s_ctx_set = true;
    s_ctx_in_game = in_game;
    s_ctx_session_s = session_s;
    s_ctx_day_s = game_day_s;
}

XRCORE_API void FrameWindowReport(const char* where) noexcept
{
    // any thread (the [mem] lines of a save may come from the Lua of the Game thread): the next Flush() prints it
    std::lock_guard<std::mutex> lock(s_window_req_lock);
    // truncated: the label is the argument of the stat_memory console command (xr_strcpy: a fatal error on overflow)
    std::snprintf(s_window_req_where, sizeof(s_window_req_where), "%s", where ? where : "-");
    s_window_req.store(true, std::memory_order_release);
}

XRCORE_API void FrameSessionMark(u8 mark) noexcept
{
    s_session_marks.fetch_or(mark, std::memory_order_release);
    detail::LoadSessionMark(mark); // 50z5-extra-diagnostics.md: the [load] timeline
}

XRCORE_API void LuaDispatchNote(u64 ticks, const char* event, const char* subscriber, u32 subscribers) noexcept
{
    const u64 slow = g_lua_slow_ticks.load(std::memory_order_relaxed);
    if (!slow)
        return;
    if (!event)
        event = "?";
    if (!subscriber)
        subscriber = "?";
    const float ms = static_cast<float>(TicksToMs(ticks));
    // no line in the warmup / exit, also not in the frame that marks an exit (a level change: its save)
    const bool line_on = ticks >= slow && s_lua_lines_on.load(std::memory_order_relaxed) &&
        !(s_session_marks.load(std::memory_order_relaxed) & frame_session::Exit);
    string512 line{};
    string256 summary{};
    {
        std::lock_guard<std::mutex> lock(s_lua_lock);
        if (!subscribers && ticks > s_lua_frame_ticks)
        {
            // the slowest call of the frame so far: only a slower one is named again (a slow one for its line still is)
            s_lua_frame_ticks = ticks;
            xr_sprintf(s_lua_frame_what, "%.80s -> %.100s", event, subscriber);
            g_lua_dispatch_floor_ticks.store(std::min(slow, ticks + 1), std::memory_order_relaxed);
            s_lua_pending.store(true, std::memory_order_release);
        }
        if (line_on)
        {
            LuaThrottleRoll(CPU::GetTicks(), summary, sizeof(summary));
            if (s_lua_throttle_lines < kLuaSlowLinesPerWindow)
            {
                ++s_lua_throttle_lines;
                if (subscribers)
                {
                    xr_sprintf(line, "! [lua-slow] event %.100s -> all %u Lua subscribers %.1f ms (frame #%u)", event,
                        subscribers, ms, Core.dwFrame);
                }
                else
                {
                    xr_sprintf(line, "! [lua-slow] event %.100s -> %.200s %.1f ms (frame #%u)", event, subscriber, ms,
                        Core.dwFrame);
                }
            }
            else
            {
                ++s_lua_throttle_suppressed;
                s_lua_throttle_max_ms = std::max(s_lua_throttle_max_ms, ms);
                s_lua_pending.store(true, std::memory_order_release);
            }
        }
    }
    if (summary[0])
        Msg("%s", summary);
    if (line[0])
        Msg("%s", line);
}

namespace
{
void WindowReportPrint(const char* where) noexcept
{
    const u32 now_ms = CPU::GetTicks();
    FrameWindow& w = s_window;
    if (!w.frames)
    {
        WindowReset(now_ms);
        detail::SysWindowReport(where, false); // 50z5: the pace and [io] windows start anew too
        return;
    }
    const double secs = (now_ms - w.start_ms) / 1000.0;
    string1024 line1{};
    AppendF(line1, sizeof(line1), "* [frame] %s: %.1f s, %u frames, avg %.1f p50 %.0f p95 %.0f p99 %.0f max %.1f ms, "
        "spikes %u", where, secs, w.frames, w.sum_ms / w.frames, HistPercentile(w.hist, w.frames, 0.50, w.max_ms),
        HistPercentile(w.hist, w.frames, 0.95, w.max_ms), HistPercentile(w.hist, w.frames, 0.99, w.max_ms), w.max_ms,
        w.spikes);
    // the spikes of the warmup after a load and of the exit, counted only (FrameSessionMark)
    if (w.warmup_spikes || w.exit_spikes)
        AppendF(line1, sizeof(line1), ", warmup spikes %u, exit spikes %u", w.warmup_spikes, w.exit_spikes);
    AppendF(line1, sizeof(line1), " | main busy %.0f%% (cpu/wall %.0f%%), game cpu/wall %.0f%%",
        100.0 * w.main_busy / w.frames, w.main_wall_ms > 0.0 ? 100.0 * w.main_cpu_ms / w.main_wall_ms : 0.0,
        w.game_wall_ms > 0.0 ? 100.0 * w.game_cpu_ms / w.game_wall_ms : 0.0);
    if (w.gpu_frames)
        AppendF(line1, sizeof(line1), " | gpu avg %.1f p95 %.0f max %.1f ms (%u frames)", w.gpu_sum_ms / w.gpu_frames,
            HistPercentile(w.gpu_hist, w.gpu_frames, 0.95, w.gpu_max_ms), w.gpu_max_ms, w.gpu_frames);
    else
        AppendF(line1, sizeof(line1), " | gpu ?");
    if (w.wait_frames)
        AppendF(line1, sizeof(line1), ", wait sync+pwait avg %.1f p95 %.0f max %.1f ms, gpu-bound %.0f%%",
            w.wait_sum_ms / w.wait_frames, HistPercentile(w.wait_hist, w.wait_frames, 0.95, w.wait_max_ms),
            w.wait_max_ms, 100.0 * w.gpu_bound / w.wait_frames);
    if (w.gap_frames)
        AppendF(line1, sizeof(line1), ", gap avg %.1f ms", w.gap_sum_ms / w.gap_frames);
    AppendF(line1, sizeof(line1), " | io %llu/%.1fMB pf %llu hf %llu", static_cast<unsigned long long>(w.read_ops),
        static_cast<double>(w.read_bytes) / (1024.0 * 1024.0), static_cast<unsigned long long>(w.faults),
        static_cast<unsigned long long>(w.hard));
    if (w.avail_min_mb)
        AppendF(line1, sizeof(line1), " ram min %uMB", w.avail_min_mb);
    Msg("%s", line1);
    detail::CrashRingPush(line1);

    // the average phases per frame (ms) and the event totals of the window
    string1024 line2{};
    AppendF(line2, sizeof(line2), "* [frame] %s: avg main:", where);
    for (u8 p = 1; p < frame_phase::MainEnd; ++p)
    {
        const double avg = w.phase_ms[p] / w.frames;
        if (avg >= 0.05)
            AppendF(line2, sizeof(line2), " %s %.2f", kPhaseNames[p], avg);
    }
    AppendF(line2, sizeof(line2), " | game %.2f:", w.game_wall_ms / w.frames);
    for (u8 p = frame_phase::MainEnd; p < frame_phase::Count; ++p)
    {
        const double avg = w.phase_ms[p] / w.frames;
        if (avg >= 0.05)
            AppendF(line2, sizeof(line2), " %s %.2f", kPhaseNames[p], avg);
    }
    u32 counts[frame_event::Count];
    float ms[frame_event::Count];
    for (u8 e = 0; e < frame_event::Count; ++e)
    {
        counts[e] = static_cast<u32>(std::min<u64>(w.ev_count[e], u32(-1)));
        ms[e] = static_cast<float>(TicksToMs(w.ev_ticks[e]));
    }
    AppendF(line2, sizeof(line2), " | events ");
    AppendEvents(line2, sizeof(line2), counts, ms);
    Msg("%s", line2);
    detail::CrashRingPush(line2);
    WindowReset(now_ms);
    // 50z5-extra-diagnostics.md: the [frame] pace and [io] lines of the same window, the [sys] lines (Diag thread)
    detail::SysWindowReport(where, true);
}
} // namespace

// 32-frame-spikes.md: a frame longer than perf_frame_peak_ms -> one "! [frame-peak]" line with the deltas of this frame
// of what makes the peaks (Lua GC, relcase, ALife, game events, Lua binders, the bus, the waits of main). Timers in ms
// (calls in parentheses where they say something), counters as they are.
static void FramePeakReport(const u64* deltas, const u64* totals, u64 wall_ticks) noexcept
{
    const double wall_ms = TicksToMs(wall_ticks);
    if (wall_ms < static_cast<double>(g_perf_frame_peak_ms))
        return;
    const u32 now_ms = CPU::GetTicks();
    if (now_ms - s_frame_peak_window_start_ms >= 5000)
    {
        s_frame_peak_window_start_ms = now_ms;
        s_frame_peak_lines = 0;
    }
    if (++s_frame_peak_lines > kFramePeakLinesPerWindow)
    {
        ++s_frame_peak_dropped;
        return;
    }
    const auto ms = [deltas](Metric m) { return TicksToMs(deltas[static_cast<size_t>(m)]); };
    const auto calls = [deltas](Metric m) { return deltas[static_cast<size_t>(m) + 1]; }; // a timer's "/calls"
    const auto count = [deltas](Metric m) { return deltas[static_cast<size_t>(m)]; };
    const auto gauge = [totals](Metric m) { return static_cast<s64>(totals[static_cast<size_t>(m)]); };

    // The Lua binders by class: the sum and the most expensive class
    struct BinderClass
    {
        Metric metric;
        const char* name;
    };
    constexpr BinderClass binders[] = { { Metric::go_shed_binder_stalker, "stalker" },
        { Metric::go_shed_binder_monster, "monster" }, { Metric::go_shed_binder_other, "other" } };
    double binder_sum = 0.0, binder_top = 0.0;
    const char* binder_top_name = "-";
    u64 binder_calls = 0;
    for (const BinderClass& b : binders)
    {
        const double v = ms(b.metric);
        binder_sum += v;
        binder_calls += calls(b.metric);
        if (v > binder_top)
        {
            binder_top = v;
            binder_top_name = b.name;
        }
    }

    string32 dropped{};
    if (s_frame_peak_dropped)
    {
        xr_sprintf(dropped, " (+%u not logged)", s_frame_peak_dropped);
        s_frame_peak_dropped = 0;
    }
    Msg("! [frame-peak] frame=%u wall=%.1fms total=%.1fms | gc step=%.1fms atomic=%.1fms uncapped=%llu steps=%llu "
        "debt=%lld heap=%lldKB | destroy=%llu relcase act=%.1fms sleep=%.1fms cb=%.1fms npc_batch=%.1fms | alife=%.1fms "
        "game_events=%.1fms | binder=%.1fms(%llu) top=%s %.1fms | dispatch_lua=%.1fms(%llu) | game_wait=%.1fms "
        "frame_move=%.1fms render=%.1fms%s",
        Core.dwFrame, wall_ms, ms(Metric::frame_total), ms(Metric::lua_gc_step), ms(Metric::lua_gc_atomic_step),
        count(Metric::lua_gc_uncapped), count(Metric::lua_gc_paced_steps), gauge(Metric::lua_gc_debt),
        gauge(Metric::lua_gc_kb), count(Metric::ol_destroy), ms(Metric::ol_relcase_act), ms(Metric::ol_relcase_sleep),
        ms(Metric::ol_relcase_cb), ms(Metric::relcase_npc_batch), ms(Metric::alife_update),
        ms(Metric::lvl_frame_game_events), binder_sum, binder_calls, binder_top_name, binder_top,
        ms(Metric::events_dispatch_lua), calls(Metric::events_dispatch_lua), ms(Metric::frame_game_wait),
        ms(Metric::frame_move), ms(Metric::frame_render), dropped);
}

XRCORE_API void Flush() noexcept
{
    // 50n: the frame spike report and the 60 s window - always, whatever perf_metrics is
    FrameReportTick();

    // P19: before everything else - the spike logger works with perf_metrics 0 as well.
    SpikeFrame();

    // Latched on its own, ahead of the early return below: bit 2 is meant to work even with bits 0-1 both off
    // (hide the hot zones while reading Tracy live, without also turning on plots or the 5 s log).
    g_perf_hide_hot_zones = (g_perf_metrics & 4) != 0;

    // [frame-peak]: the wall time of the frame that ends here (0 for the first one)
    const u64 frame_now_ticks = Ticks();
    const u64 frame_wall_ticks = s_frame_peak_prev_ticks ? frame_now_ticks - s_frame_peak_prev_ticks : 0;
    s_frame_peak_prev_ticks = frame_now_ticks;

    const bool was_active = g_perf_metrics_active;
    // perf_frame_peak_ms > 0 needs the per-frame deltas: it turns the collection on (without the plots and the log)
    g_perf_metrics_active = (g_perf_metrics & 1) != 0 || (g_perf_metrics & 2) != 0 || g_perf_frame_peak_ms > 0;
    const bool tracy_plots_enabled = (g_perf_metrics & 1) != 0;
    const bool logging_enabled = (g_perf_metrics & 2) != 0;

    if (!g_perf_metrics_active && !was_active)
        return;

    UpdateTicksCalibration();

    // Sum all slots
    u64 current_totals[kNumMetrics]{};
    const u32 slots_allocated = std::min<u32>(s_next_slot.load(std::memory_order_relaxed), static_cast<u32>(kMaxSlots));

    for (size_t i = 0; i < kNumMetrics; ++i)
    {
        if (s_metric_descs[i].is_gauge)
        {
            current_totals[i] = s_gauge_is_max[i].load(std::memory_order_relaxed) ?
                s_gauges[i].exchange(0, std::memory_order_relaxed) :
                s_gauges[i].load(std::memory_order_relaxed);
            continue;
        }
        u64 sum = s_fallback_slot.v[i].load(std::memory_order_relaxed);
        for (u32 s = 0; s < slots_allocated; ++s)
        {
            sum += s_slots[s].v[i].load(std::memory_order_relaxed);
        }
        current_totals[i] = sum;
    }

    // Compute frame deltas
    u64 deltas[kNumMetrics]{};
    for (size_t i = 0; i < kNumMetrics; ++i)
    {
        if (was_active)
        {
            deltas[i] = (current_totals[i] >= s_prev_totals[i]) ? (current_totals[i] - s_prev_totals[i]) : 0;
        }
        else
        {
            deltas[i] = 0;
        }
        s_prev_totals[i] = current_totals[i];
    }

    if (was_active && g_perf_frame_peak_ms > 0)
        FramePeakReport(deltas, current_totals, frame_wall_ticks);
#if XR_PERF_METRICS
    // 50n: the top timers of a frame spike recorded by FrameReportTick() above
    if (was_active)
        FrameReportAttachTop(deltas);
#endif

    // Tracy plots
#if defined(TRACY_ENABLE)
    if (tracy_plots_enabled && TracyIsConnected)
    {
        static bool s_tracy_plots_configured = false;
        if (!s_tracy_plots_configured)
        {
            for (size_t i = 0; i < kNumMetrics; ++i)
            {
                TracyPlotConfig(s_metric_descs[i].name, tracy::PlotFormatType::Number, true, false, 0);
            }
            s_tracy_plots_configured = true;
        }

        for (size_t i = 0; i < kNumMetrics; ++i)
        {
            if (s_metric_descs[i].is_gauge)
                TracyPlot(s_metric_descs[i].name, static_cast<double>(current_totals[i]));
            else if (s_metric_descs[i].is_timer_ticks)
                TracyPlot(s_metric_descs[i].name, TicksToMs(deltas[i]));
            else
                TracyPlot(s_metric_descs[i].name, static_cast<double>(deltas[i]));
        }
    }
#endif

    // 5-second interval logging
    const u32 now_ms = CPU::GetTicks();
    if (s_window_start_ms == 0)
        s_window_start_ms = now_ms;

    ++s_window_frames;
    for (size_t i = 0; i < kNumMetrics; ++i)
    {
        const u64 frame_v = s_metric_descs[i].is_gauge ? current_totals[i] : deltas[i];
        s_window_deltas[i] += frame_v;
        if (frame_v > s_window_max_frame[i])
            s_window_max_frame[i] = frame_v;
    }

    const u32 elapsed_ms = now_ms - s_window_start_ms;
    if (elapsed_ms >= 5000)
    {
        if (logging_enabled)
        {
#if XR_PERF_METRICS
            // 26-framemove-game-next.md, M13: log/perf_dump - this dump itself (counted in the next frame)
            const u64 dump_t0 = Ticks();
#endif
            Msg("* perf_metrics interval=%ums frames=%u", elapsed_ms, s_window_frames);
            for (size_t i = 0; i < kNumMetrics; ++i)
            {
                if (s_metric_descs[i].is_timer_calls)
                    continue;

                if (s_metric_descs[i].is_timer_ticks)
                {
                    const u64 total_ticks = s_window_deltas[i];
                    const u64 total_calls = (i + 1 < kNumMetrics) ? s_window_deltas[i + 1] : 0;
                    if (total_calls > 0 || total_ticks > 0)
                    {
                        const double total_ms = TicksToMs(total_ticks);
                        const double max_frame_ms = TicksToMs(s_window_max_frame[i]);
                        const double avg_us = total_calls > 0 ? ((total_ms * 1000.0) / static_cast<double>(total_calls)) : 0.0;
                        Msg("*   %s total=%.2fms calls=%llu avg=%.1fus max_frame=%.2fms",
                            s_metric_descs[i].base_name, total_ms, total_calls, avg_us, max_frame_ms);
                    }
                }
                else if (s_metric_descs[i].is_gauge)
                {
                    if (s_window_max_frame[i] > 0)
                    {
                        const double avg_frame = s_window_frames > 0 ? (static_cast<double>(s_window_deltas[i]) / static_cast<double>(s_window_frames)) : 0.0;
                        Msg("*   %s avg=%.1f max=%llu", s_metric_descs[i].name, avg_frame, s_window_max_frame[i]);
                    }
                }
                else
                {
                    const u64 total_count = s_window_deltas[i];
                    if (total_count > 0)
                    {
                        const double avg_frame = s_window_frames > 0 ? (static_cast<double>(total_count) / static_cast<double>(s_window_frames)) : 0.0;
                        Msg("*   %s total=%llu avg=%.1f/frame max_frame=%llu",
                            s_metric_descs[i].name, total_count, avg_frame, s_window_max_frame[i]);
                    }
                }
            }
#if XR_PERF_METRICS
            AddTimeSlow(Metric::log_perf_dump, Ticks() - dump_t0);
#endif
        }

        // P20: the top sources of ltx parsing in this window (printed with the log bit), the table starts anew
        xray::ini_stats::WindowReport(logging_enabled);

        s_window_start_ms = now_ms;
        s_window_frames = 0;
        std::memset(s_window_deltas, 0, sizeof(s_window_deltas));
        std::memset(s_window_max_frame, 0, sizeof(s_window_max_frame));
    }
}

XRCORE_API bool IsNewHazardStack(OverlapHazardSite& site) noexcept
{
#if defined(XR_PLATFORM_WINDOWS)
    // Slots fill in order, so a taken last slot means the site is full: no capture any more (the hot path of a site
    // hit millions of times per session).
    if (site.stacks[kHazardStacksPerSite - 1].load(std::memory_order_relaxed) != 0)
        return false;
    constexpr DWORD hash_frames = 24;
    void* frames[hash_frames];
    const USHORT count = CaptureStackBackTrace(1, hash_frames, frames, nullptr); // skip this function
    u64 hash = 14695981039346656037ull; // FNV-1a over the return addresses
    for (USHORT i = 0; i < count; ++i)
    {
        hash ^= u64(reinterpret_cast<uintptr_t>(frames[i]));
        hash *= 1099511628211ull;
    }
    if (hash == 0)
        hash = 1; // 0 marks a free slot
    for (std::atomic<u64>& slot : site.stacks)
    {
        u64 logged = slot.load(std::memory_order_relaxed);
        // Claim the first free slot; a lost race leaves the winner's hash in `logged` (the same path from another
        // thread is then not logged twice).
        if (logged == 0 && slot.compare_exchange_strong(logged, hash, std::memory_order_relaxed))
            return true;
        if (logged == hash)
            return false;
    }
    return false;
#else
    (void)site;
    return false;
#endif
}

XRCORE_API void ReportOverlapHazard(OverlapHazardSite& site, const char* where) noexcept
{
    const u32 n = site.count.fetch_add(1, std::memory_order_relaxed) + 1;
    // Names the caller: the whole stack of the first few distinct call paths of this site (symbolized by the same
    // code as the crash log, at most kHazardStacksPerSite per site).
    if (IsNewHazardStack(site))
    {
        string512 header;
        xr_sprintf(header, "! [overlap-hazard] %s: hit #%u, call stack of a new caller:", where, n);
        xrDebug::LogStackTrace(header);
    }
    if (n <= 8)
    {
        Msg("! [overlap-hazard] %s called from GameThread (logic thread) while the render runs in parallel, hit #%u",
            where, n);
        return;
    }
    const u64 now_ms = CPU::GetTicks();
    u64 last = site.last_report_ms.load(std::memory_order_relaxed);
    if (now_ms - last < 5000)
        return;
    if (!site.last_report_ms.compare_exchange_strong(last, now_ms, std::memory_order_relaxed))
        return; // another thread just reported this site
    Msg("! [overlap-hazard] %s called from GameThread (logic thread), %u hits so far", where, n);
}
} // namespace xray::perf

// ---------------------------------------------------------------------------------------------------------------------
// 50o-gpu-frame-timing.md: Present / sync point waits and the GPU idle gap per engine frame (PerfMetrics.hpp,
// PresentWaitSubmit). The scheme of GpuFrameTimeSubmit: the frame id in the high half of a slot, the float bits in the
// low half, one atomic access each way.
namespace xray::perf
{
namespace
{
constexpr u32 kFrameValueRing = 16;
using FrameValueRing = std::atomic<u64>[kFrameValueRing];
FrameValueRing s_present_wait_ring{};
FrameValueRing s_gpu_sync_wait_ring{};
FrameValueRing s_gpu_idle_gap_ring{};
FrameValueRing s_present_interval_ring{}; // 50z5-extra-diagnostics.md
std::atomic<u64> s_present_prev_ticks{0};

void FrameValueSubmit(FrameValueRing& ring, u32 frame_id, float ms) noexcept
{
    u32 bits;
    static_assert(sizeof(bits) == sizeof(ms));
    std::memcpy(&bits, &ms, sizeof(bits));
    ring[frame_id % kFrameValueRing].store((static_cast<u64>(frame_id) << 32) | bits, std::memory_order_release);
}

bool FrameValueGet(const FrameValueRing& ring, u32 frame_id, float& ms) noexcept
{
    const u64 v = ring[frame_id % kFrameValueRing].load(std::memory_order_acquire);
    if (v == 0 || static_cast<u32>(v >> 32) != frame_id)
        return false;
    const u32 bits = static_cast<u32>(v);
    std::memcpy(&ms, &bits, sizeof(ms));
    return true;
}
} // namespace

XRCORE_API void PresentWaitSubmit(u32 frame_id, float ms) noexcept
{
    FrameValueSubmit(s_present_wait_ring, frame_id, ms);
    // 50z5-extra-diagnostics.md: the present-to-present interval (called right after the Present / swap of the frame)
    const u64 now = Ticks();
    const u64 prev = s_present_prev_ticks.exchange(now, std::memory_order_relaxed);
    if (prev && now > prev)
        FrameValueSubmit(s_present_interval_ring, frame_id, static_cast<float>(TicksToMs(now - prev)));
}
XRCORE_API bool PresentWaitGet(u32 frame_id, float& ms) noexcept { return FrameValueGet(s_present_wait_ring, frame_id, ms); }
XRCORE_API void GpuSyncWaitSubmit(u32 frame_id, float ms) noexcept { FrameValueSubmit(s_gpu_sync_wait_ring, frame_id, ms); }
XRCORE_API bool GpuSyncWaitGet(u32 frame_id, float& ms) noexcept { return FrameValueGet(s_gpu_sync_wait_ring, frame_id, ms); }
XRCORE_API void GpuIdleGapSubmit(u32 frame_id, float ms) noexcept { FrameValueSubmit(s_gpu_idle_gap_ring, frame_id, ms); }
XRCORE_API bool GpuIdleGapGet(u32 frame_id, float& ms) noexcept { return FrameValueGet(s_gpu_idle_gap_ring, frame_id, ms); }
} // namespace xray::perf

// ---------------------------------------------------------------------------------------------------------------------
// 50z5-extra-diagnostics.md (plans/optimization_mt): the helpers of SysDiag.cpp and the crash context (SysDiag.hpp)
namespace xray::perf
{
namespace
{
bool PresentIntervalGet(u32 frame_id, float& ms) noexcept { return FrameValueGet(s_present_interval_ring, frame_id, ms); }

// The last report lines ([frame], [frame-spike], [io], [sys]) for the crash report: written under a lock by main and
// the Diag thread, read without it by the crash handler (a torn line at worst, always terminated)
constexpr u32 kCrashRingLines = 16;
constexpr u32 kCrashRingLineSize = 1024;
char s_crash_ring[kCrashRingLines][kCrashRingLineSize]{};
std::atomic<u32> s_crash_ring_next{0}; // lines pushed so far
std::mutex s_crash_ring_lock;
} // namespace

namespace detail
{
void ProcessIoSnapshot(u64& read_ops, u64& read_bytes, u64& faults, u64& hard) noexcept
{
    IoSnap s;
    IoQuery(s);
    read_ops = s.read_ops;
    read_bytes = s.read_bytes;
    faults = s.faults;
    hard = s.hard;
}

double ThreadCyclesScale() noexcept { return ThreadCpuScale(); }

void CrashRingPush(const char* line) noexcept
{
    std::lock_guard<std::mutex> lock(s_crash_ring_lock);
    const u32 n = s_crash_ring_next.load(std::memory_order_relaxed);
    char* dst = s_crash_ring[n % kCrashRingLines];
    std::strncpy(dst, line, kCrashRingLineSize - 1);
    dst[kCrashRingLineSize - 1] = 0;
    s_crash_ring_next.store(n + 1, std::memory_order_release);
}
} // namespace detail

XRCORE_API size_t CrashContextFormat(char* buf, size_t size) noexcept
{
    if (!buf || !size)
        return 0;
    buf[0] = 0;
    const auto phase_name = [](u8 p) { return p < frame_phase::Count ? kPhaseNames[p] : "?"; };
    std::snprintf(buf, size, "\n--- frame context (50z5) ---\nframe #%u, last frame %.1f ms, session %u s, in game %s\n",
        Core.dwFrame, s_last_wall_ms_x10 / 10.0, s_ctx_session_s, s_ctx_in_game ? "yes" : "no");
    if (s_main_phase_ptr && s_main_phase_t0_ptr)
    {
        // the main thread's phase now and since when ("-" = outside any phase: the "other" of the report)
        const u8 phase = *s_main_phase_ptr;
        const u64 t0 = *s_main_phase_t0_ptr;
        const u64 now = Ticks();
        AppendF(buf, size, "main thread phase: %s for %.1f ms\n", phase_name(phase), t0 && now > t0 ? TicksToMs(now - t0) : 0.0);
    }
    const ThreadKind kind = CurrentThreadKind();
    AppendF(buf, size, "crashing thread: %s, its phase %s\n",
        kind == ThreadKind::Main ? "main" : kind == ThreadKind::Game ? "game logic" : "worker / other", phase_name(tl_phase));
    const char* stage = detail::LoadStageNow();
    if (stage && stage[0])
        AppendF(buf, size, "load timeline: last stage '%s'\n", stage);
    AppendF(buf, size, "last report lines (oldest first):\n");
    const u32 next = s_crash_ring_next.load(std::memory_order_acquire);
    const u32 count = std::min(next, kCrashRingLines);
    for (u32 i = 0; i < count; ++i)
    {
        char tmp[kCrashRingLineSize];
        std::memcpy(tmp, s_crash_ring[(next - count + i) % kCrashRingLines], sizeof(tmp));
        tmp[kCrashRingLineSize - 1] = 0;
        if (tmp[0])
            AppendF(buf, size, "%s\n", tmp);
    }
    return std::strlen(buf);
}
} // namespace xray::perf
