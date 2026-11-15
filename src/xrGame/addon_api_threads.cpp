#include "StdAfx.h"

// Plugin API, group threads: the worker threads of the engine for the heavy loops of a plugin
// (wiki/doc/plugins/api/threads.md). A plugin reads the game on the logic thread, splits its own computation over
// the workers with task_parallel_for and applies the results on the logic thread again: the snapshot and the
// deferred writes of plan 07 (C10) are the plugin's own data, no engine state is shared with the workers.

#include "addon_api_threads.h"
#include "addon_event_bus.h"
#include "addon_host.h"

#include "xrCore/Threading/ParallelFor.hpp"
#include "xrCore/Threading/ThreadUtil.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

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

// ---------------------------------------------------------------------------------------------
// P46 (plans/addon_api_ideas, group plugin): task_async - a work on a background thread of the host, its done on the
// main thread. GWP_ASYNC_THREADS threads of their own, started at the first task: a long or blocking work (a file, the
// network) never takes a worker of the task scheduler that the frame waits for.
// ---------------------------------------------------------------------------------------------

struct AsyncTask
{
    const GwpPlugin* plugin = nullptr;
    GwpAsyncFn work = nullptr;
    GwpAsyncFn done = nullptr;
    void* user = nullptr;
};

struct AsyncState
{
    std::mutex lock;
    std::condition_variable wake; // a task came, or the stop
    std::condition_variable idle; // a work ended (WaitPluginTasks)
    std::deque<AsyncTask> queue;  // not started
    xr_vector<AsyncTask> finished; // work ended, done waits for the main thread (OnFrame)
    xr_vector<const GwpPlugin*> running; // one entry per work that runs now
    xr_vector<const GwpPlugin*> crashed; // a work crashed: the plugin is stopped on the main thread (OnFrame)
    xr_vector<const GwpPlugin*> cancelled; // stopped plugins: a work of them that ends now gives no done
    bool stop = false;
    std::thread threads[GWP_ASYNC_THREADS];
    bool started = false;
};

AsyncState* g_async = nullptr;

void AsyncWorkThunk(void* context)
{
    const AsyncTask& task = *static_cast<const AsyncTask*>(context);
    task.work(task.user);
}

void AsyncDoneThunk(void* context)
{
    const AsyncTask& task = *static_cast<const AsyncTask*>(context);
    task.done(task.user);
}

u32 CountOf(const xr_vector<const GwpPlugin*>& list, const GwpPlugin* plugin)
{
    return static_cast<u32>(std::count(list.begin(), list.end(), plugin));
}

void EraseOne(xr_vector<const GwpPlugin*>& list, const GwpPlugin* plugin)
{
    const auto it = std::find(list.begin(), list.end(), plugin);
    if (it != list.end())
        list.erase(it);
}

void AsyncThreadLoop(AsyncState* state)
{
    std::unique_lock<std::mutex> guard(state->lock);
    for (;;)
    {
        state->wake.wait(guard, [state] { return state->stop || !state->queue.empty(); });
        if (state->stop)
            return;
        const AsyncTask task = state->queue.front();
        state->queue.pop_front();
        state->running.push_back(task.plugin);
        guard.unlock();
        // The crash guard of every plugin call, without the call depth of the main thread (the unload waits for this
        // work itself, WaitPluginTasks)
        const bool ok = events::CallPluginGuardedBackground(reinterpret_cast<const void*>(task.work), "task_async",
            &AsyncWorkThunk, const_cast<AsyncTask*>(&task));
        guard.lock();
        EraseOne(state->running, task.plugin);
        if (!ok)
            state->crashed.push_back(task.plugin);
        else if (task.done && CountOf(state->cancelled, task.plugin) == 0)
            state->finished.push_back(task);
        state->idle.notify_all();
    }
}

GwpResult GWP_CALL ApiTaskAsync(const GwpPlugin* self, GwpAsyncFn work, GwpAsyncFn done, void* user)
{
    if (!CheckThreadsCall("task_async"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    if (!self || !work)
        return GWP_ERROR_INVALID_ARGUMENT;
    if (!g_async)
        g_async = xr_new<AsyncState>();
    AsyncState& state = *g_async;
    std::lock_guard<std::mutex> guard(state.lock);
    u32 count = CountOf(state.running, self);
    for (const AsyncTask& task : state.queue)
        count += task.plugin == self ? 1 : 0;
    for (const AsyncTask& task : state.finished)
        count += task.plugin == self ? 1 : 0;
    if (count >= GWP_ASYNC_MAX_PER_PLUGIN)
    {
        if (IsDebugLog())
            Msg("~ [plugin:%s] task_async: %u tasks are queued or running, refused", PluginAddonId(self), count);
        return GWP_ERROR_INVALID_STATE;
    }
    // A new instance of a plugin (reloaded) may take the address of the handle again: its tasks are not cancelled
    state.cancelled.erase(std::remove(state.cancelled.begin(), state.cancelled.end(), self), state.cancelled.end());
    if (!state.started)
    {
        state.started = true;
        state.stop = false;
        for (std::thread& thread : state.threads)
            thread = Threading::RunThread("X-Ray Plugin async", &AsyncThreadLoop, &state);
    }
    state.queue.push_back(AsyncTask{ self, work, done, user });
    state.wake.notify_one();
    return GWP_OK;
}
} // namespace

bool InTaskPiece() { return t_piece_depth != 0; }

void FillEngineApi(GwpEngineApi& api)
{
    api.task_parallel_for = &ApiTaskParallelFor;
    api.task_worker_count = &ApiTaskWorkerCount;
    api.task_async = &ApiTaskAsync; // P46 (plans/addon_api_ideas, group plugin)
}

void OnFrame()
{
    if (!g_async)
        return;
    xr_vector<AsyncTask> finished;
    xr_vector<const GwpPlugin*> crashed;
    {
        std::lock_guard<std::mutex> guard(g_async->lock);
        finished.swap(g_async->finished);
        crashed.swap(g_async->crashed);
    }
    for (const GwpPlugin* plugin : crashed)
        OnPluginCrashed(plugin, "a work of task_async"); // StopPlugin -> CancelPluginTasks: its done do not come
    for (const AsyncTask& task : finished)
    {
        // A done of a plugin stopped meanwhile (a crash above, or in a done before this one) is dropped
        {
            std::lock_guard<std::mutex> guard(g_async->lock);
            if (CountOf(g_async->cancelled, task.plugin) != 0)
                continue;
        }
        if (!events::CallPluginGuarded(reinterpret_cast<const void*>(task.done), "task_async done", &AsyncDoneThunk,
                const_cast<AsyncTask*>(&task)))
            OnPluginCrashed(task.plugin, "a done of task_async");
    }
}

void CancelPluginTasks(const GwpPlugin* plugin)
{
    if (!g_async || !plugin)
        return;
    std::lock_guard<std::mutex> guard(g_async->lock);
    auto& queue = g_async->queue;
    queue.erase(std::remove_if(queue.begin(), queue.end(), [plugin](const AsyncTask& t) { return t.plugin == plugin; }),
        queue.end());
    auto& finished = g_async->finished;
    finished.erase(std::remove_if(finished.begin(), finished.end(),
        [plugin](const AsyncTask& t) { return t.plugin == plugin; }), finished.end());
    if (CountOf(g_async->cancelled, plugin) == 0)
        g_async->cancelled.push_back(plugin);
}

void WaitPluginTasks(const GwpPlugin* plugin)
{
    if (!g_async || !plugin)
        return;
    CancelPluginTasks(plugin);
    std::unique_lock<std::mutex> guard(g_async->lock);
    if (CountOf(g_async->running, plugin) != 0)
    {
        Msg("* [plugin:%s] waits for its task_async work to end before the unload", PluginAddonId(plugin));
        g_async->idle.wait(guard, [plugin] { return CountOf(g_async->running, plugin) == 0; });
    }
    // A crash of the last work is no news now: the plugin goes anyway. The next instance of the plugin (the same
    // handle after a reload) starts clean
    EraseOne(g_async->crashed, plugin);
    g_async->cancelled.erase(std::remove(g_async->cancelled.begin(), g_async->cancelled.end(), plugin),
        g_async->cancelled.end());
}

void Shutdown()
{
    if (!g_async)
        return;
    {
        std::lock_guard<std::mutex> guard(g_async->lock);
        g_async->stop = true;
        g_async->queue.clear();
        g_async->finished.clear();
    }
    g_async->wake.notify_all();
    for (std::thread& thread : g_async->threads)
    {
        if (thread.joinable())
            thread.join();
    }
    xr_delete(g_async);
}
} // namespace gw::addons::threads
