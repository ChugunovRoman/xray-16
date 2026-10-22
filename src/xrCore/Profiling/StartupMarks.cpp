#include "stdafx.h"
#include "xrCore/Profiling/StartupMarks.hpp"

#include <atomic>
#include <chrono>
#include <mutex>

namespace xray
{
namespace
{
using steady = std::chrono::steady_clock;

// Taken when xrCore is loaded (static init, before WinMain / main of the executable).
const steady::time_point s_load_time = steady::now();

std::atomic<u64> s_open_count{0};
std::atomic<u64> s_open_bytes{0};

// The previous mark; touched under s_mark_lock only
std::mutex s_mark_lock;
double s_prev_ms = 0.0;
u64 s_prev_count = 0;
u64 s_prev_bytes = 0;

// StartupMarkOnce: hashes of the stages already printed (a few dozen marks at most)
constexpr size_t kMaxOnceStages = 64;
u32 s_once_hashes[kMaxOnceStages];
size_t s_once_count = 0;

double SinceLoadMs()
{
    return std::chrono::duration<double, std::milli>(steady::now() - s_load_time).count();
}

// How long the process lived before xrCore was loaded (the executable and its DLLs mapped by the loader)
double LoaderOffsetMs()
{
#if defined(XR_PLATFORM_WINDOWS)
    FILETIME creation{}, exit_time{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user))
        return 0.0;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now); // a one-time offset, its 1-16 ms resolution is enough (Precise needs Windows 8)
    const ULARGE_INTEGER c{{creation.dwLowDateTime, creation.dwHighDateTime}};
    const ULARGE_INTEGER n{{now.dwLowDateTime, now.dwHighDateTime}};
    if (n.QuadPart <= c.QuadPart)
        return 0.0;
    const double process_age_ms = double(n.QuadPart - c.QuadPart) / 10000.0; // 100 ns units
    const double offset = process_age_ms - SinceLoadMs();
    return offset > 0.0 ? offset : 0.0;
#else
    return 0.0;
#endif
}

double ProcessAgeMs()
{
    // Once: both clocks are read at nearly the same moment, the rest goes by the steady clock
    static const double offset = LoaderOffsetMs();
    return SinceLoadMs() + offset;
}

u32 StageHash(pcstr stage)
{
    u32 h = 2166136261u; // FNV-1a
    for (pcstr c = stage; *c; ++c)
    {
        h ^= u8(*c);
        h *= 16777619u;
    }
    return h;
}
} // namespace

void startup_detail::NoteFileOpen(size_t bytes) noexcept
{
    s_open_count.fetch_add(1, std::memory_order_relaxed);
    s_open_bytes.fetch_add(u64(bytes), std::memory_order_relaxed);
}

double StartupElapsedMs()
{
    return ProcessAgeMs();
}

void StartupMark(pcstr stage)
{
    if (!stage)
        stage = "?";

    double now = 0.0, delta = 0.0;
    u64 files = 0, bytes = 0;
    {
        std::lock_guard<std::mutex> guard(s_mark_lock);
        now = ProcessAgeMs();
        const u64 count = s_open_count.load(std::memory_order_relaxed);
        const u64 total = s_open_bytes.load(std::memory_order_relaxed);
        delta = now - s_prev_ms;
        files = count - s_prev_count;
        bytes = total - s_prev_bytes;
        s_prev_ms = now;
        s_prev_count = count;
        s_prev_bytes = total;
    }
    Msg("* [startup] %s %.1f ms (since start %.1f ms), files %llu, MB %.1f", stage, delta, now,
        (unsigned long long)files, double(bytes) / (1024.0 * 1024.0));
}

void StartupMarkOnce(pcstr stage)
{
    if (!stage)
        return;
    const u32 hash = StageHash(stage);
    {
        std::lock_guard<std::mutex> guard(s_mark_lock);
        for (size_t i = 0; i < s_once_count; ++i)
        {
            if (s_once_hashes[i] == hash)
                return;
        }
        if (s_once_count < kMaxOnceStages)
            s_once_hashes[s_once_count++] = hash;
    }
    StartupMark(stage);
}
} // namespace xray
