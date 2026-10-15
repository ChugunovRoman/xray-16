#include "stdafx.h"

#include "EngineThreading.h"
#include "device.h"
#include "Engine.h"
#include "IGame_Level.h"
#include "IGame_Persistent.h"
#include "xrSheduler.h"

#ifndef _EDITOR
#include "Environment.h"
#include "Rain.h"
#endif

#include "xrCore/Threading/ThreadUtil.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "xrCore/Profiling/PerfMetrics.hpp"

#include <atomic>
#include <thread>

namespace
{
// The game loop thread, set by MarkMainThread(). The initial value (the thread that loaded the module) only covers
// the calls before CApplication exists.
std::atomic<std::thread::id> s_main_thread_id{ std::this_thread::get_id() };
// The thread executing XRay::Engine::GameThread() right now, or the empty id between the frames.
std::atomic<std::thread::id> s_game_thread_id{};

struct GameThreadScope
{
    GameThreadScope()
    {
        const auto id = std::this_thread::get_id();
        s_game_thread_id.store(id, std::memory_order_release);
        xray::perf::g_game_tid.store(id, std::memory_order_release);
        if (id == xray::perf::g_main_tid.load(std::memory_order_acquire))
        {
            PERF_INC(task_wait_game_on_main);
            xray::perf::Add(xray::perf::GameOnMainSiteMetric(xray::perf::TlWaitSite()));
        }
    }
    ~GameThreadScope()
    {
        s_game_thread_id.store(std::thread::id{}, std::memory_order_release);
        xray::perf::g_game_tid.store(std::thread::id{}, std::memory_order_release);
    }
};

// Game/render overlap: state of the dedicated "Game" thread. Same handshake as CPHWorld's physics thread
// (auto-reset Event pair, lazily started, parked between frames, torn down only by ShutdownGameLogicThread).
struct GameLogicThreadState
{
    std::thread thread;
    Event wake, done;
    std::atomic<bool> exit{ false };
    bool run_scheduler{}; // written by the main thread before wake.Set() (the Event orders it)
    bool in_flight{}; // main thread only: kicked, done not consumed yet (done is auto-reset - never wait twice)
};
// Created on the first kick and never deleted on purpose: a joinable std::thread in a static destructor calls
// std::terminate() (e.g. exit after a fatal error without Device.Destroy). ShutdownGameLogicThread() joins it.
GameLogicThreadState* s_logic_state = nullptr;

void RunParallelSequence()
{
    ZoneScopedN("ProcessParallelSequence");
    PERF_SCOPE(game_parallel_seq);
    PERF_SPIKE_PHASE("game/parallel_seq"); // P19 spike logger (14-spike-instrumentation.md)
    xr_vector<fastdelegate::FastDelegate0<>> parallel_batch;
    parallel_batch.swap(Device.seqParallel);
    // 22-game-thread.md (diagnostics only): delegates a frame; their parts are the game/pseq/* timers
    xray::perf::SetGauge(xray::perf::Metric::game_pseq_items, static_cast<s64>(parallel_batch.size()));
    for (u32 pit = 0; pit < parallel_batch.size(); pit++)
        parallel_batch[pit]();
    parallel_batch.clear();
}

void RunSeqFrameMT()
{
    ZoneScopedN("seqFrameMT");
    PERF_SCOPE(game_seq_frame_mt);
    PERF_SPIKE_PHASE("game/seq_frame_mt");
    Device.seqFrameMT.Process();
}

// Body of one game/render overlap frame on the dedicated thread. No GameThreadScope here: s_game_thread_id /
// g_game_tid (and g_game_overlap_tid, which only this path sets: GW_OVERLAP_HAZARD) were set by KickGameLogicThread()
// before the wake and are cleared by the main thread's join - the scope's destructor would hand "logic thread" back
// to main while main is still rendering.
// The thread is a registered TaskManager worker (GameLogicThreadLoop): xr_parallel_for issued here
// (PostSchedulerVisionBatch...) is spread over the pool, and fire-and-forget tasks it pushes are stolen by the pool.
// The physics step was kicked by the main thread before the wake (CRenderDevice::ProcessFrame) and runs parallel to
// this whole body: physics mutations made here are deferred to the step join (xrPhysics/PHStepDefer.h).
void GameThreadOverlapBody(bool run_scheduler)
{
    ZoneScopedN("GameThread");
    PERF_SCOPE(game_total);

    if (g_pGameLevel && g_pGameLevel->bReady)
    {
        ZoneScopedN("SoundEvent_Dispatch");
        PERF_SCOPE(game_sound_dispatch);
        PERF_SPIKE_PHASE("game/sound_dispatch");
        g_pGameLevel->SoundEvent_Dispatch();
    }

    // 06-framemove-offload.md (B4/B5): the queued NPC sound callbacks and level_on_frame, before the non-RT
    // scheduler (the inputs gw_npc computes there are read by the Think of the same frame). run_scheduler = !Paused.
    if (run_scheduler && g_pGameLevel && g_pGameLevel->bReady)
    {
        ZoneScopedN("GameLogicFrame");
        g_pGameLevel->OnGameLogicFrame();
    }

    if (run_scheduler)
    {
        ZoneScopedN("Sheduler_UpdateNonRT");
        {
            PERF_SCOPE(game_sheduler);
            ::Engine.Sheduler.UpdateNonRT(); // the RT part already ran on the main thread (GameThreadRT)
        }
        if (Device.PostSchedulerVisionBatch)
        {
            PERF_SCOPE(game_vision_batch);
            PERF_SPIKE_PHASE("game/vision_batch");
            Device.PostSchedulerVisionBatch();
        }
    }

    RunParallelSequence();
    RunSeqFrameMT();
}

void GameLogicThreadLoop(GameLogicThreadState& s)
{
    // A TaskManager worker, not just a thread: PushTask() puts a task into the CURRENT thread's queue, and only
    // registered workers' queues are visible to the pool (TryToSteal). Unregistered, a fire-and-forget task pushed
    // here - CSoundRender_Emitter::dispatch_prefill() from GEnv.Sound->render() in seqFrameMT - stayed in this
    // parked thread's queue forever, and the next frame's CSoundRender_Core::update on main spun in wait_prefill()
    // (hang on the first game/render overlap frames, 2026-10-10). Slot: TaskManager CUSTOM_THREADS_RESERVE.
    if (TaskScheduler)
        TaskScheduler->RegisterThisThreadAsWorker();
    for (;;)
    {
        s.wake.Wait();
        if (s.exit.load(std::memory_order_acquire))
        {
            // Run what is still queued here (not stolen yet) - an unregistered queue is invisible to everyone.
            if (TaskScheduler)
            {
                while (TaskScheduler->ExecuteOneTask()) {}
                TaskScheduler->UnregisterThisThreadAsWorker();
            }
            s.done.Set(); // never leave a waiter hanging
            return;
        }
        GameThreadOverlapBody(s.run_scheduler);
        s.done.Set();
    }
}
} // namespace

bool XRay::Engine::IsGameLogicThread()
{
    const std::thread::id game = s_game_thread_id.load(std::memory_order_acquire);
    const std::thread::id self = std::this_thread::get_id();
    return game != std::thread::id{} ? self == game : self == s_main_thread_id.load(std::memory_order_acquire);
}

bool XRay::Engine::IsGameThreadRunning()
{
    return s_game_thread_id.load(std::memory_order_acquire) != std::thread::id{};
}

void XRay::Engine::MarkMainThread()
{
    const auto id = std::this_thread::get_id();
    s_main_thread_id.store(id, std::memory_order_release);
    xray::perf::g_main_tid.store(id, std::memory_order_release);
}

void XRay::Engine::PreRenderThread()
{
    // A TaskManager task, not a thread: it runs on a pool worker or on main (TaskManager::Wait executes queued tasks),
    // so it does not rename the thread - the zone marks it. P19b: SetCurrentThreadName("Pre-Render") here renamed whichever
    // thread ran it, main included, for the rest of the session (Tracy, SetThreadDescription, perf-spike lines).
    ZoneScopedN("PreRenderThread");

    {
        ZoneScopedN("PreRenderThread/seqParallelRender");
        xr_vector<fastdelegate::FastDelegate0<>> batch;
        batch.swap(Device.seqParallelRender);
        for (auto& d : batch)
            d();
        batch.clear();
    }

#ifndef _EDITOR
    if (!GEnv.isDedicatedServer && g_pGamePersistent && g_pGamePersistent->pEnvironment &&
        g_pGamePersistent->pEnvironment->eff_Rain)
    {
        ZoneScopedN("PreRenderThread/RainUpdateItems");
        g_pGamePersistent->pEnvironment->eff_Rain->UpdateItems();
    }
#endif

    if (Device.ParticleWorkerCallback)
    {
        ZoneScopedN("PreRenderThread/ParticleWorker");
        Device.ParticleWorkerCallback();
    }
}

void XRay::Engine::GameThread()
{
    // Sequential frame: a TaskManager task, not a thread - no thread rename, the zone marks it (P19b: the former
    // SetCurrentThreadName("Game") renamed the pool worker or main that ran it, and in the trace main and the overlap
    // Game thread both became "Game").
    ZoneScopedN("GameThread");
    PERF_SCOPE(game_total);
    const GameThreadScope logic_thread; // IsGameLogicThread() is true on this thread until the task ends

    if (g_pGameLevel && g_pGameLevel->bReady)
    {
        ZoneScopedN("SoundEvent_Dispatch");
        PERF_SCOPE(game_sound_dispatch);
        PERF_SPIKE_PHASE("game/sound_dispatch");
        g_pGameLevel->SoundEvent_Dispatch();
    }

    // 06-framemove-offload.md (B4/B5): the same place as in GameThreadOverlapBody - after SoundEvent_Dispatch, before
    // the scheduler - so both modes run the level part of the logic frame in the same order.
    if (!Device.Paused() && g_pGameLevel && g_pGameLevel->bReady)
    {
        ZoneScopedN("GameLogicFrame");
        g_pGameLevel->OnGameLogicFrame();
    }

    if (!Device.Paused())
    {
        ZoneScopedN("Sheduler_Update");
        {
            PERF_SCOPE(game_sheduler);
            ::Engine.Sheduler.Update();
        }

        // Execute parallel vision batch after all NPCs have submitted rays.
        // Must run after scheduler (NPCs submit rays during Update) and only when unpaused.
        if (Device.PostSchedulerVisionBatch)
        {
            PERF_SCOPE(game_vision_batch);
            PERF_SPIKE_PHASE("game/vision_batch");
            Device.PostSchedulerVisionBatch();
        }
    }

    RunParallelSequence();
    RunSeqFrameMT();
}

void XRay::Engine::GameThreadRT()
{
    ZoneScopedN("Sheduler_UpdateRT");
    ::Engine.Sheduler.UpdateRT();
}

void XRay::Engine::KickGameLogicThread(bool run_scheduler)
{
    VERIFY(std::this_thread::get_id() == s_main_thread_id.load(std::memory_order_acquire));
    if (!s_logic_state)
        s_logic_state = xr_new<GameLogicThreadState>();
    GameLogicThreadState& s = *s_logic_state;
    R_ASSERT2(!s.in_flight, "Game thread kicked twice without a join");
    if (!s.thread.joinable())
        s.thread = Threading::RunThread("Game logic", [&s] { GameLogicThreadLoop(s); });

    s.run_scheduler = run_scheduler;
    // The dedicated thread is the game logic thread from BEFORE the wake (no window where main and it both
    // count as such, see plan P1.1) until the main thread's join.
    const std::thread::id id = s.thread.get_id();
    s_game_thread_id.store(id, std::memory_order_release);
    xray::perf::g_game_tid.store(id, std::memory_order_release);
    // Only here, not in GameThreadScope: the render runs parallel to this thread until the join (GW_OVERLAP_HAZARD).
    xray::perf::g_game_overlap_tid.store(id, std::memory_order_release);
    s.in_flight = true;
    s.done.Reset(); // drop a stale release, auto-reset event
    s.wake.Set();
}

bool XRay::Engine::JoinGameLogicThread()
{
    if (!s_logic_state || !s_logic_state->in_flight)
        return false;
    VERIFY(std::this_thread::get_id() == s_main_thread_id.load(std::memory_order_acquire));
    GameLogicThreadState& s = *s_logic_state;
    s.done.Wait();
    s.in_flight = false;
    s_game_thread_id.store(std::thread::id{}, std::memory_order_release);
    xray::perf::g_game_tid.store(std::thread::id{}, std::memory_order_release);
    xray::perf::g_game_overlap_tid.store(std::thread::id{}, std::memory_order_release);
    return true;
}

bool XRay::Engine::IsGameLogicThreadInFlight()
{
    return s_logic_state && s_logic_state->in_flight;
}

void XRay::Engine::ShutdownGameLogicThread()
{
    if (!s_logic_state)
        return;
    JoinGameLogicThread();
    GameLogicThreadState& s = *s_logic_state;
    if (s.thread.joinable())
    {
        s.exit.store(true, std::memory_order_release);
        s.wake.Set();
        s.thread.join();
        s.exit.store(false, std::memory_order_release);
        s.done.Reset();
    }
}
