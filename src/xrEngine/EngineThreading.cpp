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

// K9: GEnv.Sound->render() of this frame is done by the Game thread's task (SoundRenderer in seqFrameMT skips it).
// The Game thread only (set and cleared there; seqFrameMT runs on it).
bool s_sound_render_taken = false;

// Game/render overlap: state of the dedicated "Game" thread. Same handshake as CPHWorld's physics thread
// (auto-reset Event pair, lazily started, parked between frames, torn down only by ShutdownGameLogicThread).
struct GameLogicThreadState
{
    std::thread thread;
    Event wake, done;
    std::atomic<bool> exit{ false };
    bool run_scheduler{}; // written by the main thread before wake.Set() (the Event orders it)
    bool in_flight{}; // main thread only: kicked, done not consumed yet (done is auto-reset - never wait twice)
    // 26-framemove-game-next.md, K3/K2b: what a wake asks for. Both may be pending at one wake (wake is an auto-reset
    // Event: two Set() before the thread waits are one wake), so the thread takes each request by its own flag:
    // phase 0 first, then the body. A wake with no request left (the second Set of a pair taken at once) is harmless.
    std::atomic<bool> early_requested{ false };
    std::atomic<bool> body_requested{ false };
    // Phase 0 request: written by main before early_requested (release), read by the thread after taking it (acquire)
    bool early_sound_dispatch{};
    void (*early_work)() {};
    // The Game thread only: phase 0 of this frame ran SoundEvent_Dispatch (the body then skips it)
    bool early_dispatched{};
    // perf_metrics: xray::perf::Ticks() of KickGameLogicThread (game/start_delay); written before body_requested
    u64 kick_ticks{};
    // 29-game-thread-next.md, A3: the K9 sound render task phase 0 started (the body waits for it). The Game thread only
    Task* early_sound_task{};
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

// K9 (26-framemove-game-next.md): GEnv.Sound->render() of the frame as a TaskManager task
void RunSoundRenderTask()
{
    ZoneScopedN("SoundRenderTask");
    PERF_SCOPE(game_seq_frame_mt_sound);
    GEnv.Sound->render();
}

// 29-game-thread-next.md, A3 (always on since 2026-10-18, see plan 41): the K9 task of the frame starts here, in phase
// 0 right after its dispatch - SoundEvent_Dispatch reads the emitters' params (feedback->get_params()) without
// m_emitters_lock, so the render must not start before it ends. The body then only waits for it (before seqFrameMT, as
// before).
void StartSoundRenderEarly(GameLogicThreadState& s)
{
    if (!TaskScheduler || !GEnv.Sound || s.early_sound_task)
        return;
    s_sound_render_taken = true;
    s.early_sound_task = &TaskScheduler->AddTask([] { RunSoundRenderTask(); });
    PERF_INC(game_sound_render_early);
}

// 26-framemove-game-next.md, K3/K2b: phase 0 (XRay::Engine::KickGameLogicEarly). Runs on the dedicated thread while
// main finishes the bones prepass (phases B/C) and kicks the physics; the body of the frame follows on this thread.
void GameThreadEarlyBody(GameLogicThreadState& s)
{
    ZoneScopedN("GameThreadEarly");
    PERF_SCOPE(game_early);
    s.early_dispatched = false;
    const bool dispatch = s.early_sound_dispatch && g_pGameLevel && g_pGameLevel->bReady;
    if (dispatch)
    {
        // IGame_Level::SoundEvent_Dispatch (no animations, visuals, ODE; its only Lua - the hit callback of the "fake"
        // hit of an enemy shot - runs here at once, the GC step comes after it). The sound events were registered by
        // GEnv.Sound->update in FrameMove of main, before the prepass.
        {
            ZoneScopedN("SoundEvent_Dispatch");
            PERF_SCOPE(game_sound_dispatch);
            PERF_SPIKE_PHASE("game/sound_dispatch");
            g_pGameLevel->SoundEvent_Dispatch();
        }
        s.early_dispatched = true;
        PERF_INC(game_early_dispatch);
        StartSoundRenderEarly(s); // A3: the dispatch is over
    }
    if (s.early_work)
    {
        ZoneScopedN("GameThreadEarly/work");
        s.early_work();
    }
}

// Body of one game/render overlap frame on the dedicated thread. No GameThreadScope here: s_game_thread_id /
// g_game_tid (and g_game_overlap_tid, which only this path sets: GW_OVERLAP_HAZARD) were set by KickGameLogicThread()
// before the wake and are cleared by the main thread's join - the scope's destructor would hand "logic thread" back
// to main while main is still rendering.
// The thread is a registered TaskManager worker (GameLogicThreadLoop): xr_parallel_for issued here
// (PostSchedulerVisionBatch...) is spread over the pool, and fire-and-forget tasks it pushes are stolen by the pool.
// The physics step was kicked by the main thread before the wake (CRenderDevice::ProcessFrame) and runs parallel to
// this whole body: physics mutations made here are deferred to the step join (xrPhysics/PHStepDefer.h).
void GameThreadOverlapBody(GameLogicThreadState& s)
{
    ZoneScopedN("GameThread");
    // From the kick to here: the wake latency, plus the part of phase 0 that did not fit before the kick (K3/K2b)
    if (s.kick_ticks)
        PERF_ADD(game_start_delay, xray::perf::Ticks() - s.kick_ticks);
    PERF_SCOPE(game_total);
    const bool run_scheduler = s.run_scheduler;

    // K3: phase 0 of this frame dispatched the sounds already
    const bool dispatched = s.early_dispatched;
    s.early_dispatched = false;
    if (!dispatched && g_pGameLevel && g_pGameLevel->bReady)
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

    // K9 (26-framemove-game-next.md, always on since 2026-10-18): the sound render of the frame (OpenAL sources, stream
    // prefill) as a task beside seqParallel instead of in seqFrameMT. It takes m_emitters_lock as before: what plays or
    // stops a sound meanwhile (object handler shots, sound players, the physics step) waits for its loop there.
    // 29-game-thread-next.md, A3: phase 0 may have started it already
    Task* sound_task = s.early_sound_task;
    s.early_sound_task = nullptr;
    if (!sound_task && TaskScheduler && GEnv.Sound)
    {
        s_sound_render_taken = true;
        sound_task = &TaskScheduler->AddTask([] { RunSoundRenderTask(); });
    }

    RunParallelSequence();
    if (sound_task)
    {
        // Normally done long ago (seqParallel takes ms); while waiting this thread runs other queued tasks
        ZoneScopedN("SoundRenderTask/wait");
        PERF_SCOPE(game_sound_render_wait);
        TaskScheduler->Wait(*sound_task);
    }
    RunSeqFrameMT();
    s_sound_render_taken = false;
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
        // Phase 0 (K3/K2b) and the body: each by its own request, phase 0 first (see GameLogicThreadState)
        if (s.early_requested.exchange(false, std::memory_order_acq_rel))
            GameThreadEarlyBody(s);
        if (s.body_requested.exchange(false, std::memory_order_acq_rel))
        {
            GameThreadOverlapBody(s);
            s.done.Set();
        }
    }
}
} // namespace

bool XRay::Engine::SoundRenderTakenByTask() { return s_sound_render_taken; }

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
    s.kick_ticks = xray::perf::g_perf_metrics_active ? xray::perf::Ticks() : 0;
    // The dedicated thread is the game logic thread from BEFORE the wake (no window where main and it both
    // count as such, see plan P1.1) until the main thread's join.
    const std::thread::id id = s.thread.get_id();
    s_game_thread_id.store(id, std::memory_order_release);
    xray::perf::g_game_tid.store(id, std::memory_order_release);
    // Only here, not in GameThreadScope: the render runs parallel to this thread until the join (GW_OVERLAP_HAZARD).
    xray::perf::g_game_overlap_tid.store(id, std::memory_order_release);
    s.in_flight = true;
    s.done.Reset(); // drop a stale release, auto-reset event
    s.body_requested.store(true, std::memory_order_release);
    s.wake.Set();
}

void XRay::Engine::KickGameLogicEarly(bool sound_dispatch, void (*work)())
{
    VERIFY(std::this_thread::get_id() == s_main_thread_id.load(std::memory_order_acquire));
    if (!sound_dispatch && !work)
        return;
    if (!s_logic_state)
        s_logic_state = xr_new<GameLogicThreadState>();
    GameLogicThreadState& s = *s_logic_state;
    R_ASSERT2(!s.in_flight, "Game thread phase 0 kicked after the frame's kick");
    if (!s.thread.joinable())
        s.thread = Threading::RunThread("Game logic", [&s] { GameLogicThreadLoop(s); });
    s.early_sound_dispatch = sound_dispatch;
    s.early_work = work;
    // The hazard detector covers phase 0 as well (main computes bones in phase B meanwhile); the logic thread
    // identity (s_game_thread_id / g_game_tid) stays with main until KickGameLogicThread
    xray::perf::g_game_overlap_tid.store(s.thread.get_id(), std::memory_order_release);
    s.early_requested.store(true, std::memory_order_release);
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
