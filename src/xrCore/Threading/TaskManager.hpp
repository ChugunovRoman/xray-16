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
#pragma once

#include "Task.hpp"
#include "Event.hpp"

#include <atomic>
#include <mutex>
#include <thread>

class TaskWorker;

class XRCORE_API TaskManager final
{
private:
    xr_vector<TaskWorker*> workers;
    xr_vector<std::thread> workerThreads;
    std::mutex workersLock;

    inline static Event newWorkArrived;
    std::atomic_size_t activeWorkersCount{};

    std::atomic_bool shouldPause{};
    std::atomic_bool shouldStop{};

private:
    ICN void TaskWorkerStart();

    [[nodiscard]] Task* TryToSteal() const;

    [[nodiscard]] static Task* AllocateTask() noexcept;

    static void ExecuteTask(Task& task);

    void SetThreadStatus(bool active) noexcept;

    // plans/optimization_mt/29-game-thread-next.md, A4: Wait() of the dedicated Game thread of a game/render overlap frame
    void WaitGameThread(const Task& task) const;

public:
    TaskManager();
    ~TaskManager();

    void SpawnThreads();

    void RegisterThisThreadAsWorker();
    void UnregisterThisThreadAsWorker();

public:
    // Create a task, but don't run it yet
    template <typename Invokable>
    [[nodiscard]] static Task& CreateTask(Invokable func)
    {
        return *new (AllocateTask()) Task(func);
    }

    // Create a task as child, but don't run it yet
    template <typename Invokable>
    [[nodiscard]] static Task& CreateTask(Task& parent, Invokable func)
    {
        return *new (AllocateTask()) Task(func, &parent);
    }

    // Run task in parallel
    static void PushTask(Task& task) noexcept;

    // Run task immediately in this thread
    static void RunTask(Task& task);

    // Shortcut: create a task and run it immediately
    template <typename Invokable>
    static Task& AddTask(Invokable func)
    {
        Task& task = CreateTask(func);
        PushTask(task);
        return task;
    }

    // Shortcut: create task and run it immediately
    template <typename Invokable>
    static Task& AddTask(Task& parent, Invokable func)
    {
        Task& task = CreateTask(parent, func);
        PushTask(task);
        return task;
    }

public:
    void Wait(const Task& task, bool updateSystemEvents = false) const;
    bool ExecuteOneTask() const;

    // plans/optimization_mt/50g-stage3-threads-power.md, R11: the waits use the bounded spin - the Short spin class
    // (a small pool, SMT on <= 4 cores, on battery or in battery saver: xray::hw::GetEffectiveSpinClass). Long
    // otherwise - every wait loop behaves exactly as before then.
    [[nodiscard]] static bool ShortSpin() noexcept;

    // R11: spins until done() is true, for a wait on work other threads already run (the tails of the render's
    // parallel parts). Long: std::this_thread::yield() between the checks, as these loops always did. Short: see
    // ShortSpinStep.
    template <typename Done>
    static void SpinUntil(const Done& done)
    {
        if (!ShortSpin())
        {
            while (!done())
                std::this_thread::yield();
            return;
        }
        for (u32 misses = 0; !done(); ++misses)
            ShortSpinStep(misses, done);
    }

    // R11: as SpinUntil, running queued tasks meanwhile (help-while-waiting, as Wait() does for a Task). Long: a yield
    // after each check that found no task, as the sound's loops always did.
    template <typename Done>
    void HelpUntil(const Done& done) const
    {
        if (!ShortSpin())
        {
            while (!done())
            {
                if (!ExecuteOneTask())
                    std::this_thread::yield();
            }
            return;
        }
        for (u32 misses = 0; !done();)
        {
            if (ExecuteOneTask())
                misses = 0;
            else
                ShortSpinStep(misses++, done);
        }
    }

    // R11, Short spin class: one step of a wait loop after `misses` checks in a row found nothing to do - a CPU pause
    // (_mm_pause) for the first ones, then a yield of the time slice, then blocking until any task finishes or is
    // pushed (at most ~1 ms a block, the guard against a missed wake-up). done() is checked again after this thread
    // announced the block, so a finish right before the block is never lost.
    template <typename Done>
    static void ShortSpinStep(u32 misses, const Done& done)
    {
        ShortSpinStepImpl(misses, [](const void* context) -> bool { return (*static_cast<const Done*>(context))(); }, &done);
    }

    using SpinDoneFn = bool(const void* context);
    static void ShortSpinStepImpl(u32 misses, SpinDoneFn* done, const void* context) noexcept;

    // Wake up ALL worker threads. PushTask wakes only one worker per task; after pushing
    // a batch of tasks from one thread they all land in that thread's queue and the rest
    // of the workers stay asleep until they happen to steal. Call this after scheduling
    // a batch so every worker starts stealing immediately.
    void WakeAllWorkers() noexcept;

    void Pause(bool pause) { shouldPause.store(pause, std::memory_order_release); }

public:
    // All registered threads: main, the pool, and the custom threads registered later (the overlap "Game" thread,
    // "Physics") - they run tasks only while they wait. Grains of splits.
    [[nodiscard]] size_t GetWorkersCount() const noexcept;
    // The pool threads only (SpawnThreads; 0 before it): how many helpers a split can count on. Thresholds and part
    // counts of parallel paths (plans/optimization_mt/50b-stage1-threads-render.md).
    [[nodiscard]] size_t GetPoolWorkersCount() const noexcept;
    // Upper bound of GetCurrentWorkerID() + 1 for any registered thread, including custom threads registered
    // later (GetWorkersCount() may still grow). For per-worker arrays sized once.
    [[nodiscard]] size_t GetMaxWorkersCount() const noexcept;
    [[nodiscard]] static size_t GetCurrentWorkerID() noexcept;
    void GetStats(size_t& allocated, size_t& pushed, size_t& finished);
};

extern XRCORE_API xr_unique_ptr<TaskManager> TaskScheduler;

// plans/optimization_mt/29-game-thread-next.md, A4 (always on since 2026-10-18, its switch mt_game_wait_own_only
// removed, see plan 41): a Wait() of the dedicated Game thread of a game/render overlap frame (phase 0 and the body,
// xray::perf::g_game_overlap_tid) runs only the tasks of its own queue and the pieces of the awaited task (the awaited
// task or its children at the head of another queue), not other threads' tasks (the render's, sound decoding); after
// 1 ms without such work it steals as before (deadlock guard: the awaited task in a queue nobody serves).
