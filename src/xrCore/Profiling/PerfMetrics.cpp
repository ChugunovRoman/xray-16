#include "stdafx.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "xrCore/Profiling/IniLoadStats.hpp"

#include <atomic>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <thread>
#include <ctime>
#include <SDL.h>

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
XRCORE_API int g_perf_spike_us = 0;
XRCORE_API int g_perf_spike_cpu = 0;
XRCORE_API int g_perf_spike_lines = 40;
XRCORE_API u64 g_perf_spike_ticks = 0;
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
thread_local u32 tl_bones_locked_v = 0;
thread_local u8 tl_ai_planner_v = 0;
thread_local u8 tl_ai_context_v = 0;
thread_local u8 tl_ff_caller_v = 0;

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

u64 CalibrateTicksFrequency() noexcept
{
#if defined(_MSC_VER) || defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    const u64 qpc_freq = SDL_GetPerformanceFrequency();
    if (!qpc_freq)
        return 3000000000ULL;

    const u64 wait_ticks = qpc_freq / 500; // 2 ms spin calibration
    const u64 qpc0 = SDL_GetPerformanceCounter();
    const u64 tsc0 = __rdtsc();
    while ((SDL_GetPerformanceCounter() - qpc0) < wait_ticks)
    {
    }
    const u64 qpc1 = SDL_GetPerformanceCounter();
    const u64 tsc1 = __rdtsc();
    const u64 delta_qpc = qpc1 - qpc0;
    const u64 delta_tsc = tsc1 - tsc0;
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
} // namespace

namespace xray::perf
{
XRCORE_API u8& TlPass() noexcept { return tl_pass_v; }
XRCORE_API u8& TlCrowSrc() noexcept { return tl_crow_src_v; }
XRCORE_API u8& TlBoneCaller() noexcept { return tl_bone_caller_v; }
XRCORE_API u8& TlWaitSite() noexcept { return tl_wait_site_v; }
XRCORE_API u8& TlPhWake() noexcept { return tl_ph_wake_v; }
XRCORE_API u8& TlSpawn() noexcept { return tl_spawn_v; }
XRCORE_API u32& TlBonesLocked() noexcept { return tl_bones_locked_v; }
XRCORE_API u8& TlAiPlanner() noexcept { return tl_ai_planner_v; }
XRCORE_API u8& TlAiContext() noexcept { return tl_ai_context_v; }
XRCORE_API u8& TlFfCaller() noexcept { return tl_ff_caller_v; }

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

// C1.0 split (PerfMetrics.hpp, ScopedSplit): the class the main thread's time goes to now and since when. Main thread
// only (ScopedSplit is inert elsewhere), so plain variables. Every DoRender scope ends inside the frame, so a segment
// never spans Flush().
namespace
{
u8 s_split_kind = split::None;
u64 s_split_t0 = 0;

void SplitSwitch(u8 kind) noexcept
{
    const u64 now = Ticks();
    if (s_split_kind == split::Build)
        AddSlow(Metric::rsplit_build, now - s_split_t0);
    else if (s_split_kind == split::Submit)
        AddSlow(Metric::rsplit_submit, now - s_split_t0);
    s_split_kind = kind;
    s_split_t0 = now;
}
} // namespace

XRCORE_API u8 SplitEnter(u8 kind) noexcept
{
    const u8 prev = s_split_kind;
    if (kind != prev)
        SplitSwitch(kind);
    return prev;
}

XRCORE_API void SplitLeave(u8 prev) noexcept
{
    if (prev != s_split_kind)
        SplitSwitch(prev);
}

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

XRCORE_API void Flush() noexcept
{
    // P19: before everything else - the spike logger works with perf_metrics 0 as well.
    SpikeFrame();

    // Latched on its own, ahead of the early return below: bit 2 is meant to work even with bits 0-1 both off
    // (hide the hot zones while reading Tracy live, without also turning on plots or the 5 s log).
    g_perf_hide_hot_zones = (g_perf_metrics & 4) != 0;

    const bool was_active = g_perf_metrics_active;
    g_perf_metrics_active = (g_perf_metrics & 1) != 0 || (g_perf_metrics & 2) != 0;
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
