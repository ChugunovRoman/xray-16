#pragma once

#include "xrCore.h"

#ifndef MASTER_GOLD
#   define XR_PERF_METRICS 1
#else
#   define XR_PERF_METRICS 0
#endif

namespace xray::perf
{
enum class Metric : u16
{
#define PERF_COUNTER(id, name) id,
#define PERF_TIMER(id, name)   id, id##_Calls,
#include "PerfMetricsList.inl"
#undef PERF_COUNTER
#undef PERF_TIMER
    Count
};

XRCORE_API extern int  g_perf_metrics;
XRCORE_API extern bool g_perf_metrics_active;

XRCORE_API void AddSlow(Metric m, u64 v) noexcept;
XRCORE_API void AddTimeSlow(Metric m, u64 ticks) noexcept;
XRCORE_API void SetGauge(Metric m, s64 v) noexcept;
XRCORE_API void AddHist(Metric m, u32 bucket) noexcept;
XRCORE_API u64  Ticks() noexcept;
XRCORE_API void Flush() noexcept;

inline void Add(Metric m, u64 v = 1) noexcept
{
    if (g_perf_metrics_active)
        AddSlow(m, v);
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

    ~ScopedTime() noexcept
    {
        if (active)
            AddTimeSlow(metric, Ticks() - t0);
    }

    ScopedTime(const ScopedTime&) = delete;
    ScopedTime& operator=(const ScopedTime&) = delete;
};
} // namespace xray::perf

#if XR_PERF_METRICS
#   define PERF_ADD(m, v) ::xray::perf::Add(::xray::perf::Metric::m, v)
#   define PERF_INC(m)    ::xray::perf::Add(::xray::perf::Metric::m, 1)
#   define PERF_SCOPE(m)  ::xray::perf::ScopedTime CONCATENIZE(_perf_, __LINE__)(::xray::perf::Metric::m)
#else
#   define PERF_ADD(m, v) ((void)0)
#   define PERF_INC(m)    ((void)0)
#   define PERF_SCOPE(m)  ((void)0)
#endif
