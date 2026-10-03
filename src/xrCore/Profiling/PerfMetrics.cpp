#include "stdafx.h"
#include "xrCore/Profiling/PerfMetrics.hpp"

#include <atomic>
#include <cstring>
#include <algorithm>
#include <SDL.h>

#if defined(_MSC_VER)
#   include <intrin.h>
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#   include <x86intrin.h>
#endif

namespace xray::perf
{
XRCORE_API int  g_perf_metrics = 0;
XRCORE_API bool g_perf_metrics_active = false;
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
std::atomic<u32> s_next_slot{0};
thread_local size_t tl_slot = size_t(-1);

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
};

#define PERF_COUNTER(id, name) { name, name, false, false },
#define PERF_TIMER(id, name)   { name "/ms", name, true, false }, { name "/calls", name, false, true },
const MetricDesc s_metric_descs[kNumMetrics] = {
#include "PerfMetricsList.inl"
};
#undef PERF_COUNTER
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

u64 s_prev_totals[kNumMetrics]{};
u64 s_window_deltas[kNumMetrics]{};
u64 s_window_max_frame[kNumMetrics]{};
u32 s_window_start_ms = 0;
u32 s_window_frames = 0;
} // namespace

namespace xray::perf
{
XRCORE_API u64 Ticks() noexcept
{
#if defined(_MSC_VER) || defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    return __rdtsc();
#else
    return SDL_GetPerformanceCounter();
#endif
}

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
    s_slots[0].v[idx].store(static_cast<u64>(v), std::memory_order_relaxed);
}

XRCORE_API void AddHist(Metric m, u32 bucket) noexcept
{
    const size_t idx = static_cast<size_t>(m) + bucket;
    if (idx < kNumMetrics)
        AddSlow(static_cast<Metric>(idx), 1);
}

XRCORE_API void Flush() noexcept
{
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
            if (s_metric_descs[i].is_timer_ticks)
            {
                const double ms = TicksToMs(deltas[i]);
                TracyPlot(s_metric_descs[i].name, ms);
            }
            else
            {
                TracyPlot(s_metric_descs[i].name, static_cast<double>(deltas[i]));
            }
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
        s_window_deltas[i] += deltas[i];
        if (deltas[i] > s_window_max_frame[i])
            s_window_max_frame[i] = deltas[i];
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

        s_window_start_ms = now_ms;
        s_window_frames = 0;
        std::memset(s_window_deltas, 0, sizeof(s_window_deltas));
        std::memset(s_window_max_frame, 0, sizeof(s_window_max_frame));
    }
}
} // namespace xray::perf
