/*
    Copyright (c) 2014-2021 OpenXRay

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/
#include "stdafx.h"

#include "Task.hpp"
#include "TaskManager.hpp"

#include "ScopeLock.hpp"
#include "xrCore/Profiling/PerfMetrics.hpp"

#include <algorithm>
#include <thread>
#include <SDL_events.h>

#include "Math/fast_lc16.hpp"

#include <random>

#if defined(XR_ARCHITECTURE_X86) || defined(XR_ARCHITECTURE_X64) || defined(XR_ARCHITECTURE_E2K)
#include <immintrin.h>
#elif defined(XR_ARCHITECTURE_ARM) || defined(XR_ARCHITECTURE_ARM64)
#include "sse2neon/sse2neon.h"
#elif defined(XR_ARCHITECTURE_RISCV)
#include "sse2rvv/sse2rvv.h"
#elif defined(XR_ARCHITECTURE_PPC64)
#include <xmmintrin.h>
#else
#error Add your platform here
#endif

xr_unique_ptr<TaskManager> TaskScheduler;

static constexpr size_t OTHER_THREADS_COUNT = 1; // Primary and Secondary thread
// GW: worker slots for custom threads registered after SpawnThreads() - the game/render overlap "Game" thread
// (EngineThreading.cpp) and CPHWorld's "Physics" thread (PHWorld.cpp). Reserved up
// front so such a late RegisterThisThreadAsWorker() never reallocates `workers` while the pool threads read it
// without the lock (TryToSteal). The pool size is not affected.
static constexpr size_t CUSTOM_THREADS_RESERVE = 2;

class TaskStorageSize
{
    template <int E, typename Dummy = void> // XXX: remove Dummy workaround when gcc will finally match the standard
    struct PowerOfTwo
    {
        static constexpr size_t value = 2 * PowerOfTwo<E - 1>::value;
    };

    template <typename Dummy>
    struct PowerOfTwo<0, Dummy>
    {
        static constexpr size_t value = 1;
    };

public:
    template <size_t Power>
    static constexpr size_t Get()
    {
        return PowerOfTwo<Power>::value;
    }
};

static constexpr size_t TASK_STORAGE_SIZE = TaskStorageSize::Get<12>(); // 4096 tasks
static constexpr size_t TASK_STORAGE_MASK = TASK_STORAGE_SIZE - 1;

class TaskAllocator
{
    size_t m_allocated{};
    Task   m_storage[TASK_STORAGE_SIZE];

public:
    Task* allocate() noexcept
    {
        Task* task = &m_storage[m_allocated++ & TASK_STORAGE_MASK];
        VERIFY(task->IsFinished());
        return task;
    }
} static thread_local s_tl_allocator;

// Multi producer
// Multi consumer
class TaskQueue
{
    std::atomic_size_t  m_head_pos{};
    std::atomic_size_t  m_tail_pos{};
    Task*               m_storage[TASK_STORAGE_SIZE]{};

public:
    void push(Task* task) noexcept
    {
        const auto task_pos = m_tail_pos.fetch_add(1, std::memory_order_acq_rel);
        VERIFY2(task_pos - m_head_pos.load(std::memory_order_acquire) < TASK_STORAGE_SIZE, "Task queue overflow");
        m_storage[task_pos & TASK_STORAGE_MASK] = task;
    }

    Task* pop() noexcept
    {
        size_t head_pos = m_head_pos.load(std::memory_order_acquire);
        Task* task = m_storage[head_pos & TASK_STORAGE_MASK];
        if (!task)
            return nullptr;

        const bool success = m_head_pos.compare_exchange_weak(head_pos, head_pos + 1, std::memory_order_acq_rel);
        if (success)
        {
            m_storage[head_pos & TASK_STORAGE_MASK] = nullptr;
            return task;
        }
        return nullptr;
    }

    Task* steal() noexcept
    {
        return pop();
    }

    size_t size() const noexcept
    {
        return m_head_pos - m_tail_pos;
    }

    bool empty() const noexcept
    {
        return size() == 0;
    }
};

struct TaskWorkerStats
{
    size_t allocatedTasks{};
    size_t pushedTasks{};
    size_t finishedTasks{};
    size_t execOwn{};
    size_t execWorker0{};
    size_t execSteal{};
    size_t execMiss{};
    size_t stealAttempts{};
    size_t stealSuccess{};
    size_t sleepCount{};
    u64    sleepTime{};
    size_t waitSpin{};
    size_t wakeEvents{};
};

class TaskWorker : public TaskQueue, public TaskWorkerStats
{
public:
    fast_lc16        random{ this };
    size_t           id    { size_t(-1) };
} static thread_local s_tl_worker;

TaskManager::TaskManager()
{
    ZoneScoped;
    workers.reserve(std::thread::hardware_concurrency() + CUSTOM_THREADS_RESERVE);
    RegisterThisThreadAsWorker();
}

void TaskManager::SpawnThreads()
{
    ZoneScoped;

    const u32 threads = std::thread::hardware_concurrency() - OTHER_THREADS_COUNT;
    workerThreads.reserve(threads);

    for (u32 i = 0; i < threads; ++i)
    {
        workerThreads.emplace_back(Threading::RunThread("Task Worker", &TaskManager::TaskWorkerStart, this));
    }
}

TaskManager::~TaskManager()
{
    ZoneScoped;
    shouldStop.store(true, std::memory_order_release);

    // Event::Set() wakes up only one thread
    // Try to wake all of them
    const auto count = workerThreads.size();
    for (size_t i = 0; i < count; ++i)
        newWorkArrived.Set();

    // Finish all pending tasks
    while (Task* t = s_tl_worker.pop())
    {
        ExecuteTask(*t);
    }

    UnregisterThisThreadAsWorker();
    while (!workers.empty())
    {
        newWorkArrived.Set();
        Sleep(0);
    }
    for (auto& thread : workerThreads)
    {
        if (thread.joinable())
            thread.join();
    }
}

void TaskManager::RegisterThisThreadAsWorker()
{
    ZoneScoped;
    std::lock_guard guard{ workersLock };

    // Never past the reserved capacity: the pool threads index `workers` without the lock.
    R_ASSERT2(workers.size() < workers.capacity(),
        "You must change CUSTOM_THREADS_RESERVE if you want to register more custom threads.");

    // The lowest ID no registered worker holds, not workers.size(): UnregisterThisThreadAsWorker() erases from the
    // middle without renumbering the others. Once one custom thread has left (the physics thread with its level),
    // size() can equal the ID another one still holds, and the two would share their per-worker slots
    // (CObjectList::m_secondary_crows). The result stays below workers.capacity() (GetMaxWorkersCount()).
    size_t id = 0;
    while (std::any_of(workers.begin(), workers.end(), [id](const TaskWorker* w) { return w->id == id; }))
        ++id;
    s_tl_worker.id = id;
    workers.emplace_back(&s_tl_worker);
}

void TaskManager::UnregisterThisThreadAsWorker()
{
    ZoneScoped;
    std::lock_guard guard{ workersLock };

    shouldPause.store(true, std::memory_order_release);
    while (activeWorkersCount.load(std::memory_order_relaxed))
        Sleep(2);

    s_tl_worker.id = size_t(-1);

    const auto it = std::find(workers.begin(), workers.end(), &s_tl_worker);
    if (it != workers.end())
        workers.erase(it);

    shouldPause.store(false, std::memory_order_release);
}

void TaskManager::SetThreadStatus(bool active) noexcept
{
    if (active)
        activeWorkersCount.fetch_add(1, std::memory_order_relaxed);
    else
        activeWorkersCount.fetch_sub(1, std::memory_order_relaxed);
}

void TaskManager::TaskWorkerStart()
{
    RegisterThisThreadAsWorker();
    SetThreadStatus(true);

    while (!shouldStop.load(std::memory_order_consume))
    {
        if (!shouldPause.load(std::memory_order_consume))
        {
            if (ExecuteOneTask())
                continue;
        }

        SetThreadStatus(false);
        {
            do
            {
                ++s_tl_worker.sleepCount;
                PERF_INC(task_sleep_count);
                const u64 sleep_start = ::xray::perf::Ticks();
                newWorkArrived.Wait();
                const u64 sleep_dt = ::xray::perf::Ticks() - sleep_start;
                s_tl_worker.sleepTime += sleep_dt;
                PERF_ADD(task_sleep_time, sleep_dt);
            } while (shouldPause.load(std::memory_order_consume));
        }
        SetThreadStatus(true);
    }

    SetThreadStatus(false);

    // Finish all pending tasks
    while (Task* t = s_tl_worker.pop())
    {
        ExecuteTask(*t);
    }

    // Prepare to exit
    UnregisterThisThreadAsWorker();
}

Task* TaskManager::TryToSteal() const
{
    std::uniform_int_distribution<u16> dist{ 0, static_cast<u16>(workers.size() - 1) };

    int steal_attempts = 5;
    while (steal_attempts > 0)
    {
        const auto idx = dist(s_tl_worker.random);
        TaskWorker* other = workers[idx];
        if (other == &s_tl_worker)
            continue;

        ++s_tl_worker.stealAttempts;
        PERF_INC(task_steal_attempt);

        if (auto* task = other->steal())
        {
            ++s_tl_worker.stealSuccess;
            PERF_INC(task_steal_success);

            if (!other->empty())
                newWorkArrived.Set();
            return task;
        }
        --steal_attempts;
    }
    return nullptr;
}

Task* TaskManager::AllocateTask() noexcept
{
    ++s_tl_worker.allocatedTasks;
    return s_tl_allocator.allocate();
}

void TaskManager::PushTask(Task& task) noexcept
{
    s_tl_worker.push(&task);
    newWorkArrived.Set();
    ++s_tl_worker.wakeEvents;
    PERF_INC(task_wake_event);
    ++s_tl_worker.pushedTasks;
}

void TaskManager::ExecuteTask(Task& task)
{
    task();
    ++s_tl_worker.finishedTasks;
}

void TaskManager::RunTask(Task& task)
{
    ExecuteTask(task);
}

namespace
{
// Nesting guard for TaskManager::Wait: a Wait() can steal and run a task that itself calls Wait()
// on the same thread. Only the outermost call's duration means "this thread was blocked here" -
// an inner one is already counted as exec time inside the outer call's loop.
thread_local u32 s_tl_wait_depth = 0;
}

void TaskManager::Wait(const Task& task, bool updateSystemEvents /*= false*/) const
{
    ZoneScoped;

    ++s_tl_wait_depth;
    // Per METRICS_PLAN §8.2: time only the outermost Wait() and only on main (worker id 0, the
    // thread that constructed TaskManager) - everything else either double-counts a nested wait
    // or measures a worker thread that has no single "the frame is blocked here" meaning.
    const bool time_it = s_tl_wait_depth == 1 && s_tl_worker.id == 0 && xray::perf::g_perf_metrics_active;
    const u64 wait_t0 = time_it ? xray::perf::Ticks() : 0;
    u64 exec_ticks = 0;

    while (!task.IsFinished())
    {
        const u64 iter_t0 = time_it ? xray::perf::Ticks() : 0;
        if (ExecuteOneTask())
        {
            if (time_it)
                exec_ticks += xray::perf::Ticks() - iter_t0;
        }
        else
        {
            ++s_tl_worker.waitSpin;
            PERF_INC(task_wait_spin);
        }
        if (s_tl_worker.id == 0 && (xrDebug::ProcessingFailure() || updateSystemEvents))
            SDL_PumpEvents(); // Necessary to prevent dead locks
    }

    if (time_it)
    {
        xray::perf::AddTimeSlow(xray::perf::Metric::task_wait_total, xray::perf::Ticks() - wait_t0);
        xray::perf::AddTimeSlow(xray::perf::Metric::task_wait_exec, exec_ticks);
        xray::perf::Add(xray::perf::WaitSiteCountMetric(xray::perf::TlWaitSite()));
    }
    --s_tl_wait_depth;
}

bool TaskManager::ExecuteOneTask() const
{
    Task* task = s_tl_worker.pop();
    if (task)
    {
        ++s_tl_worker.execOwn;
        PERF_INC(task_exec_own);
    }
    else
    {
        task = workers[0]->steal();
        if (task)
        {
            ++s_tl_worker.execWorker0;
            PERF_INC(task_exec_worker0);
        }
        else
        {
            task = TryToSteal();
            if (task)
            {
                ++s_tl_worker.execSteal;
                PERF_INC(task_exec_steal);
            }
            else
            {
                ++s_tl_worker.execMiss;
                PERF_INC(task_exec_miss);
            }
        }
    }

    if (task)
    {
        ExecuteTask(*task);
        return true;
    }
    return false;
}

size_t TaskManager::GetWorkersCount() const noexcept
{
    return workers.size();
}

size_t TaskManager::GetMaxWorkersCount() const noexcept
{
    return workers.capacity();
}

void TaskManager::WakeAllWorkers() noexcept
{
    // Event::Set() releases a single waiting thread: signal once per worker so all of
    // them wake up and start stealing from the producer's queue.
    std::lock_guard guard{ workersLock };
    for (size_t i = 0; i < workers.size(); ++i)
        newWorkArrived.Set();
}

size_t TaskManager::GetCurrentWorkerID() noexcept
{
    return s_tl_worker.id;
}

void TaskManager::GetStats(size_t& allocated, size_t& pushed, size_t& finished)
{
    std::lock_guard guard{ workersLock };
    for (const TaskWorker* worker : workers)
    {
        allocated += worker->allocatedTasks;
        pushed += worker->pushedTasks;
        finished += worker->finishedTasks;
    }
}
