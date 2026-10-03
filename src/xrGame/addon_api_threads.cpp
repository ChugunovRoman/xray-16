#include "StdAfx.h"

// Plugin API, group threads: the worker threads of the engine for the heavy loops of a plugin
// (wiki/doc/plugins/api/threads.md). A plugin reads the game on the logic thread, splits its own computation over
// the workers with task_parallel_for and applies the results on the logic thread again: the snapshot and the
// deferred writes of plan 07 (C10) are the plugin's own data, no engine state is shared with the workers.

#include "addon_api_threads.h"
#include "addon_event_bus.h"
#include "addon_host.h"

#include "xrCore/Threading/ParallelFor.hpp"

#include <algorithm>
#include <atomic>

namespace gw::addons::threads
{
namespace
{
// The task scheduler keeps its tasks in rings of 4096 per thread and checks them only in debug builds: a range split
// into thousands of pieces would overrun them (live parent tasks overwritten). The pieces of one call are capped at
// this many per worker; the grain of the plugin is a lower bound.
constexpr u32 kMaxPiecesPerWorker = 16;

// Depth of task_parallel_for pieces running on this thread. The logic thread runs pieces too while it waits, and
// there the thread id alone would let a piece pass the main-thread checks of the API: IsMainThread answers false
// while this is not zero (addon_host.cpp)
thread_local u32 t_piece_depth = 0;

struct PieceScope
{
    PieceScope() { ++t_piece_depth; }
    ~PieceScope() { --t_piece_depth; }
    PieceScope(const PieceScope&) = delete;
    PieceScope& operator=(const PieceScope&) = delete;
};

bool CheckThreadsCall(pcstr function)
{
    if (IsMainThread())
        return true;
    // Once per run: a loop calling it from worker threads would flood the log
    static std::atomic<bool> reported{ false };
    if (!reported.exchange(true))
        Msg("! [threads] %s called outside the main thread (or from a task_parallel_for piece), ignored "
            "(reported once)", function);
    return false;
}

// One task_parallel_for in progress. A crash of one piece stops the plugin: the pieces still waiting skip its code.
struct ParallelForRun
{
    GwpTaskRangeFn fn = nullptr;
    void* user = nullptr;
    std::atomic<bool> crashed{ false };
};

struct PieceCall
{
    const ParallelForRun* run;
    uint32_t begin;
    uint32_t end;
};

void PieceThunk(void* context)
{
    const PieceCall& call = *static_cast<const PieceCall*>(context);
    call.run->fn(call.run->user, call.begin, call.end);
}

GwpResult GWP_CALL ApiTaskParallelFor(
    const GwpPlugin* self, uint32_t count, uint32_t grain, GwpTaskRangeFn fn, void* user)
{
    if (!CheckThreadsCall("task_parallel_for"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    if (!self || !fn)
        return GWP_ERROR_INVALID_ARGUMENT;
    if (!count)
        return GWP_OK;
    if (!TaskScheduler)
        return GWP_ERROR_INVALID_STATE;

    ParallelForRun run;
    run.fn = fn;
    run.user = user;
    // Every piece runs under the same guard as any other plugin call: a crash or a C++ exception in it is caught and
    // logged there (with its stack, on the thread that ran it), the game goes on
    const auto body = [&run](const TaskRange<u32>& range) {
        if (run.crashed.load(std::memory_order_relaxed))
            return;
        PieceCall call{ &run, range.begin(), range.end() };
        const PieceScope piece;
        if (!events::CallPluginGuarded(reinterpret_cast<const void*>(run.fn), "task_parallel_for", &PieceThunk, &call))
            run.crashed.store(true, std::memory_order_relaxed);
    };
    // grain: the plugin's, else count / workers; never below what keeps the pieces under the cap
    const u32 workers = std::max<u32>(static_cast<u32>(TaskScheduler->GetWorkersCount()), 1u);
    const u64 max_pieces = u64(workers) * kMaxPiecesPerWorker;
    const u32 min_grain = static_cast<u32>((u64(count) + max_pieces - 1) / max_pieces);
    const u32 wanted = grain ? grain : static_cast<u32>((u64(count) + workers - 1) / workers);
    const u32 effective_grain = std::max({ wanted, min_grain, 1u });
    // The calling thread waits and runs pieces itself meanwhile (TaskManager::Wait)
    xr_parallel_for(TaskRange<u32>(0, count, effective_grain), body);
    // What the pieces wrote is visible here (the finish flags of the scheduler are not ordered on every CPU)
    std::atomic_thread_fence(std::memory_order_acquire);

    if (run.crashed.load())
    {
        // Back on the logic thread: the plugin is stopped here, as for a crash anywhere else
        OnPluginCrashed(self, "task_parallel_for");
        return GWP_ERROR_CRASHED;
    }
    return GWP_OK;
}

uint32_t GWP_CALL ApiTaskWorkerCount() { return TaskScheduler ? static_cast<uint32_t>(TaskScheduler->GetWorkersCount()) : 0; }
} // namespace

bool InTaskPiece() { return t_piece_depth != 0; }

void FillEngineApi(GwpEngineApi& api)
{
    api.task_parallel_for = &ApiTaskParallelFor;
    api.task_worker_count = &ApiTaskWorkerCount;
}
} // namespace gw::addons::threads
