#include "stdafx.h"
#pragma hdrstop

#include "xrCore/BackgroundIo.h"
#include "xrCore/Platform/HwProfile.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "xrCore/Profiling/SysDiag.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

// 50i-stage3-slow-disk-io.md (plans/optimization_mt, plan 50 R7): the slot of the single background I/O queue, see
// BackgroundIo.h. Two readers at most (the model and the sound prefetch threads), so a mutex and a condition variable
// are enough: no fairness beyond "the model reader first".

namespace xray::io
{
namespace
{
std::mutex s_bgio_mutex; // s_bgio_busy; the waits on s_bgio_cv
std::condition_variable s_bgio_cv;
bool s_bgio_busy = false; // a reader holds the slot
std::atomic<u32> s_bgio_model_jobs{0};
std::atomic<u32> s_bgio_texture_jobs{0}; // 50r-slow-disk-async-textures.md
std::atomic_bool s_bgio_measured_slow{false};

// 50z3-startup-slow-disk-io.md
std::atomic_bool s_bgio_menu_phase{true}; // until the first level load
std::atomic_bool s_bgio_menu_settled{false};
std::atomic_bool s_bgio_menu_warm{false};
std::atomic<u64> s_bgio_fg_read_ms{0}; // steady ms (+1) of the last foreground read; 0 = none
thread_local bool tl_bgio_held = false; // this thread holds the slot: its reads are background ones
thread_local u64 tl_bgio_t0 = 0; // 50z5-extra-diagnostics.md: Ticks() when the slot was taken (the [io] "bg slot")

bool bgio_policy_single()
{
    const hw::Policy& policy = hw::GetPolicy();
    return hw::PolicyValid(policy) && policy.io_single_queue != 0;
}

u64 bgio_now_ms()
{
    using namespace std::chrono;
    return static_cast<u64>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count()) + 1;
}

u32 bgio_fg_idle_ms()
{
    const u64 last = s_bgio_fg_read_ms.load(std::memory_order_relaxed);
    if (!last)
        return u32(-1);
    const u64 now = bgio_now_ms();
    return now > last ? static_cast<u32>(std::min<u64>(now - last, u32(-2))) : 0;
}

bool bgio_fg_quiet() { return bgio_fg_idle_ms() >= kForegroundQuietMs; }
} // namespace

bool BackgroundIoSingle() { return s_bgio_measured_slow.load(std::memory_order_relaxed) || bgio_policy_single(); }

void BackgroundIoNoteMeasuredSlow() { s_bgio_measured_slow.store(true, std::memory_order_relaxed); }

bool BackgroundIoMeasuredSlow() { return s_bgio_measured_slow.load(std::memory_order_relaxed); }

void BackgroundIoSetModelJobs(u32 jobs)
{
    const u32 before = s_bgio_model_jobs.exchange(jobs, std::memory_order_relaxed);
    if (before == 0 || jobs != 0)
        return;
    // The sound reader may wait for exactly this: the lock makes sure it either sees 0 or is already waiting
    {
        std::lock_guard<std::mutex> lk(s_bgio_mutex);
    }
    s_bgio_cv.notify_all();
}

void BackgroundIoSetTextureJobs(u32 jobs)
{
    // 50r-slow-disk-async-textures.md: as BackgroundIoSetModelJobs - the sound reader and the menu warm-up wait for 0
    const u32 before = s_bgio_texture_jobs.exchange(jobs, std::memory_order_relaxed);
    if (before == 0 || jobs != 0)
        return;
    {
        std::lock_guard<std::mutex> lk(s_bgio_mutex);
    }
    s_bgio_cv.notify_all();
}

bool BackgroundIoAcquire(BackgroundIoClient who, u32 wait_ms)
{
    std::unique_lock<std::mutex> lk(s_bgio_mutex);
    const auto ready = [who] {
        if (s_bgio_busy)
            return false;
        const bool no_models = s_bgio_model_jobs.load(std::memory_order_relaxed) == 0;
        const bool no_textures = s_bgio_texture_jobs.load(std::memory_order_relaxed) == 0;
        switch (who)
        {
        case BackgroundIoClient::Model: return true;
        case BackgroundIoClient::Texture: return no_models; // 50r: after the spawns and the ALife models
        // 50r: the menu warm-up after the textures on the screen; 50z3: and after the reads of the start-up and the menu
        case BackgroundIoClient::Warm: return no_textures && bgio_fg_quiet();
        default: // Sound; 50z3: in the menu phase (the deferred sounds of the start-up) after the foreground reads too
            return no_models && no_textures && (!s_bgio_menu_phase.load(std::memory_order_relaxed) || bgio_fg_quiet());
        }
    };
    // The foreground rule is a time, not a notification: the predicate is asked again at the timeout of the wait
    if (!s_bgio_cv.wait_for(lk, std::chrono::milliseconds(wait_ms), ready))
        return false;
    s_bgio_busy = true;
    tl_bgio_held = true;
    tl_bgio_t0 = perf::Ticks();
    return true;
}

void BackgroundIoRelease()
{
    if (tl_bgio_held)
        perf::IoSlotHoldNote(perf::Ticks() - tl_bgio_t0);
    tl_bgio_held = false;
    {
        std::lock_guard<std::mutex> lk(s_bgio_mutex);
        s_bgio_busy = false;
    }
    s_bgio_cv.notify_all();
}

// ---------------------------------------------------------------------------------------------------------------------
// 50z3-startup-slow-disk-io.md

bool BackgroundIoMenuPhase() { return s_bgio_menu_phase.load(std::memory_order_relaxed); }

void BackgroundIoEndMenuPhase()
{
    if (!s_bgio_menu_phase.exchange(false, std::memory_order_relaxed))
        return;
    // a Sound reader may wait for the foreground reads to stop: from now on it does not
    {
        std::lock_guard<std::mutex> lk(s_bgio_mutex);
    }
    s_bgio_cv.notify_all();
}

void BackgroundIoNoteForegroundRead()
{
    if (tl_bgio_held || !s_bgio_menu_phase.load(std::memory_order_relaxed) || !BackgroundIoSingle())
        return;
    s_bgio_fg_read_ms.store(bgio_now_ms(), std::memory_order_relaxed);
}

u32 BackgroundIoForegroundIdleMs() { return bgio_fg_idle_ms(); }

void BackgroundIoNoteMenuSettled() { s_bgio_menu_settled.store(true, std::memory_order_relaxed); }

bool BackgroundIoMenuSettled() { return s_bgio_menu_settled.load(std::memory_order_relaxed); }

void BackgroundIoSetMenuWarmPending(bool pending) { s_bgio_menu_warm.store(pending, std::memory_order_relaxed); }

bool BackgroundIoMenuWarmPending() { return s_bgio_menu_warm.load(std::memory_order_relaxed); }
} // namespace xray::io
