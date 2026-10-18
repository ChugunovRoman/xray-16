#include "stdafx.h"

#include "Render.h"

#include "xrCore/FS_impl.h"
#include "xrCore/Threading/TaskManager.hpp"
#include "xrCore/Profiling/PerfMetrics.hpp"

#include "XR_IOConsole.h"
#include "xr_input.h"

#include "IGame_Level.h"
#include "IGame_Persistent.h"
#include "EngineThreading.h"
#include "Engine.h"

#include "xrScriptEngine/script_space.hpp"
#include "xrScriptEngine/script_wrapper_cache.hpp"

#include <SDL.h>

#include <optional>
#include <typeinfo>

ENGINE_API CRenderDevice Device;
ENGINE_API CLoadScreenRenderer load_screen_renderer;

ENGINE_API std::atomic<bool> g_bRendering = false;

// Id of the dedicated SVP build thread (a plain std::thread, NOT a task-scheduler worker) while
// it runs render stages that reach game code; std::thread::id{} while the worker is parked.
// Game-side render callbacks that mutate engine queues keyed by scheduler worker IDs
// (CObjectList::o_crow etc.) must no-op on that thread - the main pass renders the same objects
// and performs the registration.
ENGINE_API std::atomic<std::thread::id> g_svp_worker_thread_id{ std::thread::id{} };

ENGINE_API bool svp_worker_is_current_thread()
{
    return g_svp_worker_thread_id.load(std::memory_order_relaxed) == std::this_thread::get_id();
}

// 31-svp-scope.md (frame/game_wait_svp): Device.dwFrame of the last frame whose IGame_Level::OnRender ran the scope
// pass (written there on the main thread, read by JoinGameLogic later in the same frame).
u32 g_svp_scope_render_frame = u32(-1);

ENGINE_API bool g_bBenchmark = false;
string512 g_sBenchmarkName;

int ps_fps_limit = 501;
int ps_fps_limit_in_menu = 60;

bool g_bLoaded = false;
ref_light precache_light = 0;

using namespace xray;

bool CRenderDevice::RenderBegin()
{
    if (GEnv.isDedicatedServer)
        return true;

    ZoneScoped;

    switch (GEnv.Render->GetDeviceState())
    {
    case DeviceState::Normal: break;
    case DeviceState::Lost:
        // If the device was lost, do not render until we get it back
        Sleep(33);
        return false;

    case DeviceState::NeedReset:
        // Check if the device is ready to be reset
        WaitForFrameTasks();
        Reset();
        return false;

    default: R_ASSERT(0);
    }
    GEnv.Render->Begin();
    g_bRendering = true;

    return true;
}

void CRenderDevice::Clear() { GEnv.Render->Clear(); }

void CRenderDevice::RenderEnd(void)
{
    if (GEnv.isDedicatedServer)
        return;

    ZoneScoped;
    if (dwPrecacheFrame)
    {
        GEnv.Sound->set_master_volume(0.f);
        dwPrecacheFrame--;
        if (!dwPrecacheFrame)
        {
            GEnv.Render->updateGamma();
            if (precache_light)
            {
                precache_light->set_active(false);
                precache_light.destroy();
            }
            GEnv.Sound->set_master_volume(1.f);
            GEnv.Render->ResourcesDestroyNecessaryTextures();
            Memory.mem_compact();
            Msg("* MEMORY USAGE: %d K", Memory.mem_usage() / 1024);
            Msg("* End of synchronization A[%d] R[%d]", b_is_Active, b_is_Ready);
            FIND_CHUNK_COUNTER_FLUSH();
            if (g_pGamePersistent->GameType() == 1 && !psDeviceFlags.test(rsAlwaysActive)) // haCk
            {
                const Uint32 flags = SDL_GetWindowFlags(m_sdlWnd);
                if ((flags & SDL_WINDOW_INPUT_FOCUS) == 0)
                    Pause(true, true, true, "application start");
            }
        }
    }
    // end scene
    g_bRendering = false;
    GEnv.Render->End();
    GEnv.Render->PresentFrame();

    vCameraPositionSaved = vCameraPosition;
    vCameraDirectionSaved = vCameraDirection;
    vCameraTopSaved = vCameraTop;
    vCameraRightSaved = vCameraRight;

    mFullTransformSaved = mFullTransform;
    mViewSaved = mView;
    mProjectSaved = mProject;
}

void CRenderDevice::PreCache(u32 amount, bool wait_user_input)
{
    if (GEnv.isDedicatedServer)
        amount = 0;
    else if (GEnv.Render->GetForceGPU_REF())
        amount = 0;

    dwPrecacheFrame = dwPrecacheTotal = amount;
    if (amount && !precache_light && g_pGameLevel && g_loading_events.empty())
    {
        precache_light = GEnv.Render->light_create();
        precache_light->set_shadow(false);
        precache_light->set_position(vCameraPosition);
        precache_light->set_color(255, 255, 255);
        precache_light->set_range(5.0f);
        precache_light->set_active(true);
    }
    if (amount && !load_screen_renderer.IsActive())
    {
        load_screen_renderer.Start(wait_user_input);
    }
}

void CRenderDevice::CalcFrameStats()
{
    stats.RenderTotal.FrameEnd();
    do
    {
        // calc FPS & TPS
        if (fTimeDeltaReal <= EPS_S)
            break;
        const float fps = 1.f / fTimeDeltaReal;
        // if (Engine.External.tune_enabled) vtune.update (fps);
        constexpr float fOne = 0.3f;
        constexpr float fInv = 1.0f - fOne;
        stats.fFPS = fInv * stats.fFPS + fOne * fps;
        if (stats.RenderTotal.result > EPS_S)
        {
            const u32 renderedPolys = GEnv.Render->GetCacheStatPolys();
            stats.fTPS = fInv * stats.fTPS + fOne * float(renderedPolys) / (stats.RenderTotal.result * 1000.f);
            stats.fRFPS = fInv * stats.fRFPS + fOne * 1000.f / stats.RenderTotal.result;
        }
    } while (false);
    stats.RenderTotal.FrameStart();
}

int g_svDedicateServerUpdateReate = 100;

ENGINE_API xr_list<LOADING_EVENT> g_loading_events;

bool CRenderDevice::BeforeFrame()
{
    ZoneScoped;

    if (!b_is_Ready)
    {
        Sleep(100);
        return false;
    }

    if (psDeviceFlags.test(rsStatistic))
        g_bEnableStatGather = true; // XXX: why not use either rsStatistic or g_bEnableStatGather?
    else
        g_bEnableStatGather = false;

    if (!g_loading_events.empty())
    {
        if (g_loading_events.front()())
            g_loading_events.pop_front();
        g_pGamePersistent->LoadDraw();
        return false;
    }

    return true;
}

void CRenderDevice::OnCameraUpdated()
{
    static u32 frame{ u32(-1) };
    if (frame == dwFrame)
        return;

    ZoneScoped;

    // Precache
    if (dwPrecacheFrame)
    {
        const float factor = float(dwPrecacheFrame) / float(dwPrecacheTotal);
        const float angle = PI_MUL_2 * factor;
        vCameraDirection.set(_sin(angle), 0, _cos(angle));
        vCameraDirection.normalize();
        vCameraTop.set(0, 1, 0);
        vCameraRight.crossproduct(vCameraTop, vCameraDirection);
        mView.build_camera_dir(vCameraPosition, vCameraDirection, vCameraTop);
    }

    // Matrices
    mInvView.invert(mView);
    mFullTransform.mul(mProject, mView);
    mInvFullTransform.invert_44(mFullTransform);
    {
        // P19b (14-spike-instrumentation.md): CRender::OnCameraUpdated kicks the HOM task and the details task, the latter
        // after waiting for the previous one (CDetailManager::DispatchMTCalc). The first call of the frame is inside the
        // actor's UpdateCL (CActor::UpdateCL -> Device.OnCameraUpdated), the one after FrameMove is then a no-op.
        ZoneScopedN("OnCameraUpdated/render");
        PERF_SCOPE(frame_camera_render);
        GEnv.Render->OnCameraUpdated();
    }
    GEnv.Render->SetCacheXform(mView, mProject);

    frame = dwFrame;
}

static void UpdateViewports()
{
    // Update and Render additional Platform Windows
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}

namespace
{
// C1.0 (plans/optimization_mt/17-c1-semi-pipeline.md): one seqRender subscriber of DoRender - the level (frame/render/
// seq_level, CLevel::OnRender has its own zones) apart from all the others (frame/render/seq_other: cursor, console, drag
// item, debug renders, stats...), these with a Tracy zone named by the subscriber's class, as in SeqFrameCall.
void SeqRenderCall(pureRender* object)
{
#if XR_PERF_METRICS
    if (g_pGameLevel && object == static_cast<pureRender*>(g_pGameLevel))
    {
        PERF_SCOPE(frame_render_seq_level);
        object->OnPure(object);
        return;
    }
    PERF_SCOPE(frame_render_seq_other);
#endif
    ZoneTransientN(seq_render_zone, xray::perf::SpikeClassName(typeid(*object)), true);
    object->OnPure(object);
}
} // namespace

void CRenderDevice::DoRender()
{
    if (GEnv.isDedicatedServer)
        return;

    ZoneScoped;

    CStatTimer renderTotalReal;
    renderTotalReal.FrameStart();
    renderTotalReal.Begin();
    if (b_is_Active && RenderBegin())
    {
        // C1.0: everything below is submit (render/split/submit) unless a nested PERF_SPLIT says otherwise (the build
        // phases of the level render, the join with the Game thread).
        PERF_SPLIT(Submit);
        {
            ZoneScopedN("Render process");
            PERF_SCOPE(frame_render_seq);
            // C1.0: each subscriber through SeqRenderCall (frame/render/seq_level / seq_other); set here, on the thread
            // of Process(), like the seqFrame hook in FrameMove
            seqRender.SetCallHook(&SeqRenderCall);
            seqRender.Process(); // all rendering is done here
        }

        // Game/render overlap frame: CLevel::OnRender normally joined the Game thread before the UI already.
        // Safety net for frames where it did not (no level/game, scene skipped) - before stats/ImGui/Present.
        // No-op when nothing is in flight (always in a sequential frame).
        JoinGameLogic();

        {
            ZoneScopedN("stats");
            PERF_SCOPE(frame_render_stats);
            CalcFrameStats();
            Statistic->Show();
        }

        {
            ZoneScopedN("imgui");
            PERF_SCOPE(frame_render_imgui);
            ImGui::Render();
            m_imgui_render->Render(ImGui::GetDrawData());
        }

        // Present before ImGui platform viewports (same order as official ImGui DX11 examples). Rendering
        // child viewports first kept a very deep stack under IDXGISwapChain::Present with some GPU hooks.
        {
            ZoneScopedN("present");
            PERF_SCOPE(frame_render_present);
            RenderEnd(); // End + PresentFrame
        }

        {
            ZoneScopedN("UpdateViewports");
            PERF_SCOPE(frame_render_viewports);
            UpdateViewports();
        }
    }
    else
    {
        ZoneScopedN("UpdateViewports");
        UpdateViewports();
    }
    renderTotalReal.End();
    renderTotalReal.FrameEnd();
    stats.RenderTotal.accum = renderTotalReal.accum;
}

void CRenderDevice::WaitForFrameTasks()
{
    // Game/render overlap frame: the dedicated Game thread first - it runs beside the physics step and queues its
    // physics mutations for the join, so the physics wait below is only race-free on CPHWorld's main-thread state
    // after this join.
    // No-op when nothing is in flight (always in a sequential frame).
    JoinGameLogic();
    // Early kick of the bones prepass (a reset requested from FrameMove's UI / Lua after the kick): its bone tasks
    if (BonesPrepassJoinCallback)
        BonesPrepassJoinCallback();
    // Physics step (Kick..Join window) first: Reset() (mem_compact, render resources) and Destroy() must
    // not run beside it. Idempotent - the regular Join after DoRender() then only flushes.
    if (PhysicsWaitOverlapCallback)
        PhysicsWaitOverlapCallback();
    // Early main build: its static half writes into the render's immediate dsgraph, which Reset()/Destroy() reset.
    if (GEnv.Render)
        GEnv.Render->JoinMainBuildEarly();
    if (!TaskScheduler)
        return;
    if (m_game_task)
    {
        PERF_WAIT_SITE(xray::perf::wait_site::Game);
        TaskScheduler->Wait(*m_game_task);
        m_game_task = nullptr;
    }
    if (m_pre_render_task)
    {
        PERF_WAIT_SITE(xray::perf::wait_site::PreRender);
        TaskScheduler->Wait(*m_pre_render_task);
        m_pre_render_task = nullptr;
    }
    secondary_tasks.wait();
}

// GAME_RENDER_OVERLAP_PLAN P1, variant B: when a frame may run the game logic parallel to the render. Checked after
// FrameMove (which can pause, open the main menu or finish a level load) and before anything parallel starts.
bool CRenderDevice::CanOverlapGameAndRender()
{
    if (GEnv.isDedicatedServer || !GEnv.Render)
        return false;
    // Plan section 2: D3D11 (r4) only - creating GL resources from the game thread already crashed once.
    if (GEnv.Render->GetBackendAPI() != IRender::BackendAPI::D3D11)
        return false;
    if (!g_pGameLevel || !g_pGameLevel->bReady)
        return false;
    // Nothing to overlap with (DoRender skips the scene) or loading/precache frames: keep them sequential.
    if (!b_is_Active || dwPrecacheFrame || load_screen_renderer.IsActive())
        return false;
    if (Paused())
        return false;
    if (g_pGamePersistent && g_pGamePersistent->m_pMainMenu && g_pGamePersistent->m_pMainMenu->IsActive())
        return false;
    return true;
}

// Bones prepass (always): see device.h, BonesPrepassCallback. Called inside the overlap window
// (SetFrameOverlapActive(true)), so a model a blend callback releases is only queued (CModelPool::Delete) and the
// pointers the prepass collected stay valid until ModelDeferredClear of the next frame.
void CRenderDevice::RunBonesPrepass()
{
    if (!BonesPrepassCallback)
        return;
    // Same frames DoRender() would skip the scene for (nothing to prepare for) and loading/precache frames.
    if (GEnv.isDedicatedServer || !b_is_Active || dwPrecacheFrame || load_screen_renderer.IsActive())
    {
        // No prepass: the bone tasks of an early kick are joined here instead
        if (BonesPrepassJoinCallback)
            BonesPrepassJoinCallback();
        return;
    }
    ZoneScopedN("BonesPrepass");
    PERF_SCOPE(prepass_time);
    BonesPrepassCallback();
}

void CRenderDevice::JoinGameLogic()
{
    if (!XRay::Engine::IsGameLogicThreadInFlight())
        return;
    // Called on the Game logic thread itself (Lua on it ran a console command such as vid_reset -> Device.Reset ->
    // WaitForFrameTasks): waiting for our own `done` would hang forever. Skip the join; the main thread joins as usual.
    if (XRay::Engine::IsGameLogicThread())
    {
        static bool s_warned = false;
        if (!s_warned)
        {
            s_warned = true;
            Msg("! JoinGameLogic called on the Game logic thread itself (a device reset / quit from its Lua?); skipped");
        }
        return;
    }
    // Game/render overlap frame: frame/game_wait = how long the main thread waited for the Game thread here
    // (normally before the UI, inside frame/render).
    ZoneScopedN("GameLogicJoin");
    PERF_SCOPE(frame_game_wait);
    PERF_SCOPE_IF(g_svp_scope_render_frame == dwFrame, frame_game_wait_svp); // the same wait, frames with the scope pass
    PERF_SPLIT(None); // C1.0: neither build nor submit of the render (render/split/*)
    // 29-game-thread-next.md: the same wait, measured in every build (no XR_PERF_METRICS needed), feeds the adaptive
    // budget of the scheduler (see xrSheduler.cpp). One sample a frame: only the first join of
    // the frame finds the thread in flight
    CTimer wait_timer;
    wait_timer.Start();
    XRay::Engine::JoinGameLogicThread();
    ::Engine.Sheduler.NoteGameWait(wait_timer.GetElapsed_ns());
}

void CRenderDevice::JoinGameLogicBeforeUI() { JoinGameLogic(); }

void CRenderDevice::PostJoin(std::function<void()> fn)
{
    std::lock_guard<std::mutex> guard(m_post_join_lock);
    m_post_join_queue.emplace_back(std::move(fn));
}

void CRenderDevice::ApplyPostJoinQueue()
{
    xr_vector<std::function<void()>> local;
    {
        std::lock_guard<std::mutex> guard(m_post_join_lock);
        if (m_post_join_queue.empty())
            return;
        local.swap(m_post_join_queue);
    }
    ZoneScopedN("PostJoinQueue");
    for (auto& fn : local)
        fn();
}

void CRenderDevice::ProcessFrame()
{
    ZoneScoped;

    if (!BeforeFrame())
        return;

    const u64 frameStartTime = TimerGlobal.GetElapsed_ms();
#if XR_PERF_METRICS
    // frame/total: from here to the fps-limit sleep (reset() below), i.e. the whole frame/* breakdown.
    std::optional<xray::perf::ScopedTime> perf_frame_total(std::in_place, xray::perf::Metric::frame_total);
#endif

    {
        ZoneScopedN("seqParallelBeforRender");
        PERF_SCOPE(frame_before_prerender);
        xr_vector<fastdelegate::FastDelegate0<>> befor_batch;
        befor_batch.swap(seqParallelBeforRender);
        for (auto& d : befor_batch)
            d();
        befor_batch.clear();
    }

    if (g_pGamePersistent)
    {
        ZoneScopedN("OnFrameBeforePreRender");
        PERF_SCOPE(frame_before_prerender);
        g_pGamePersistent->OnFrameBeforePreRender();
    }

    // `CEnvironment::OnFrame` before PreRender: env is stable for `seqParallelRender` / `ParticleWorkerCallback` on
    // the worker. Uses `Device.vCameraPosition` before `OnCameraUpdated` this frame (same as prior early-seqFrame use).
    if (g_pGamePersistent)
    {
        ZoneScopedN("Environment_OnFrame");
        PERF_SCOPE(frame_environment);
        g_pGamePersistent->OnFrameEnvironment();
    }

    if (ModelDeferredClear)
    {
        ZoneScopedN("ModelDeferredClear");
        PERF_SCOPE(frame_model_clear);
        ModelDeferredClear();
    }

    // PreRenderThread overlaps with the main-thread FrameMove below (kept from the original design).
    if (TaskScheduler)
        m_pre_render_task = &TaskScheduler->AddTask([] { XRay::Engine::PreRenderThread(); });

    {
        PERF_SCOPE(frame_move);
        FrameMove();
    }

    {
        PERF_SCOPE(frame_camera);
        OnCameraUpdated();
    }

    // GAME_RENDER_OVERLAP_PLAN P1 (variant B): decided once per frame, here - after FrameMove and before anything
    // runs in parallel. Always on when the frame allows it (CanOverlapGameAndRender: D3D11, level ready, no loading,
    // pause or main menu); otherwise the sequential frame below. Read by CPHWorld (deferral predicate, camera
    // snapshot) for the whole window.
    const bool game_render_overlap = CanOverlapGameAndRender();
    m_game_render_overlap_frame.store(game_render_overlap, std::memory_order_release);

    // B-1: open overlap window before GameThread launch so scheduler-side physics intents defer until flush.
    if (PhysicsBeginOverlapFrameCallback)
        PhysicsBeginOverlapFrameCallback();
    SetFrameOverlapActive(true);

    if (game_render_overlap)
    {
        // Game/render overlap frame (MX f76479197 RT-split): the real-time scheduler part (the actor: movement,
        // g_Physics on its ODE body, camera-related state, actor binder) runs here on the main thread, inside the
        // same window state as in a sequential frame and strictly before the Game thread and the physics step - so
        // the a927f7153 race (actor scheduler || physics step) cannot come back.
        // Early main build (26-framemove-game-next.md, r__main_build_early, 0 = no-op): the camera and the HOM of the
        // frame are final now (CActor::UpdateCL -> OnCameraUpdated), so the static half of the main view's build starts
        // on a worker here, beside the RT part, the bones prepass and the physics kick; Calculate() in DoRender joins
        // it and launches the dynamic half (that one needs the prepass done).
        {
            ZoneScopedN("MainBuildEarlyKick");
            GEnv.Render->KickMainBuildEarly();
        }
        const bool run_scheduler = !Paused();
        if (run_scheduler)
        {
            PERF_SCOPE(frame_rt_sheduler);
            XRay::Engine::GameThreadRT();
        }
        // Bones prepass: after the RT part, before the Game thread starts changing animations (and the render
        // starts reading bones); the previous frame's physics step is joined and flushed. UpdateTracks (and its Lua
        // blend callbacks) of the prepassed models thus runs here on main, not on render workers beside the Game
        // thread. What the Game thread invalidates later is still recomputed by the render (prepass/miss_render).
        RunBonesPrepass();
        // Physics kick (plans/optimization_mt/09-physics-earlier-parallel.md, B6, always here): after the RT part and
        // the bones prepass, before the Game thread is woken, so the step runs parallel to the whole Game thread.
        // Mutations of simulated bodies made there meanwhile wait for the join (xrPhysics/PHStepDefer.h, flushed in
        // PhysicsJoinOverlapCallback below).
        if (PhysicsKickOverlapCallback)
        {
            ZoneScopedN("PhysicsOverlapKick");
            PhysicsKickOverlapCallback();
        }
        // Dedicated Game thread: SoundEvent_Dispatch -> level OnGameLogicFrame (B4/B5) -> non-RT scheduler ->
        // vision batch -> seqParallel -> seqFrameMT, parallel to DoRender() and the physics step. Joined in
        // CLevel::OnRender before the UI
        // (JoinGameLogicBeforeUI), with safety joins in DoRender() and right after it.
        // 26-framemove-game-next.md, K3/K2b: the bones prepass may have woken
        // the thread already for phase 0 (XRay::Engine::KickGameLogicEarly after its phase A); the body below then
        // starts when phase 0 is done and skips the dispatch phase 0 did.
        XRay::Engine::KickGameLogicThread(run_scheduler);
    }
    else
    {
    // Early kick of the bones prepass: the frame turned sequential after it (pause, main
    // menu) - its bone tasks must not run beside the scheduler of the GameThread task below.
    if (BonesPrepassJoinCallback)
        BonesPrepassJoinCallback();
    // GameThread (Sheduler.Update + seqFrameMT) runs in parallel with PreRenderThread / main-thread work.
    if (TaskScheduler)
        m_game_task = &TaskScheduler->AddTask([] { XRay::Engine::GameThread(); });

    // B-1 / FIX: join the GameThread (Sheduler.Update + seqFrameMT) BEFORE running the physics step.
    // Running FrameStep in parallel with Sheduler.Update raced on the player actor's physics body/geoms:
    // CActor::shedule_Update -> g_Physics -> movement()->Calculate()/GetPosition() mutates actor ODE
    // bodies (dGeomSetPosition, dSpaceAdd, ray casts) while FrameStep::Collide/IslandStep reads/writes
    // the same bodies/spaces on this thread -> nondeterministic actor XFORM -> first-person camera
    // jitter / image doubling. Joining here keeps physics serial w.r.t. scheduler mutations (same
    // ordering as overlap=0, where OnFrame runs inside GameThread after Sheduler.Update).
    if (TaskScheduler && m_game_task)
    {
        PERF_WAIT_SITE(xray::perf::wait_site::Game);
        PERF_SCOPE(frame_game_wait);
        TaskScheduler->Wait(*m_game_task);
        m_game_task = nullptr;
    }

    // Bones prepass: the game logic of the frame is done (UpdateCL, scheduler) and the physics step is not kicked
    // yet, so animations are final and the ODE bodies the ragdoll bone callbacks read are stable.
    RunBonesPrepass();

    // B-1 / Phase 3: kick the physics step here — after the GameThread scheduler joined (CPHWorld::OnFrame
    // in seqFrame always no-ops now). Wakes CPHWorld's dedicated physics thread and returns right away,
    // so the step runs parallel to DoRender() below.
    if (PhysicsKickOverlapCallback)
    {
        ZoneScopedN("PhysicsOverlapKick");
        PhysicsKickOverlapCallback();
    }
    } // sequential frame (no game/render overlap): the block above is the unchanged path (kept unindented on purpose)

    {
        PERF_SCOPE(frame_render);
        DoRender();
    }

    // Early main build: a static half DoRender did not take (scene skipped, failed RenderBegin) must not outlive the
    // frame - ModelDeferredClear / level unload of the next frame. No-op when Calculate() took it.
    if (game_render_overlap)
        GEnv.Render->JoinMainBuildEarly();

    // Game/render overlap frame: final safety join (DoRender skipped the scene: !b_is_Active, failed RenderBegin,
    // device reset). Must happen before the physics wait and flush below - the Game thread queues physics mutations
    // for the join - and in any case before the next OnFrameBeforePreRender / ModelDeferredClear / FrameMove.
    // No-op in a sequential frame.
    JoinGameLogic();

    // Wait for the physics thread to finish this frame's step (no flush yet).
    if (PhysicsWaitOverlapCallback)
    {
        ZoneScopedN("PhysicsOverlapJoin");
        PERF_SCOPE(ph_overlap_join);
        PhysicsWaitOverlapCallback();
    }

    // Join PreRenderThread (still overlapping with the physics step + DoRender above, as in the original
    // design) BEFORE the deferred-mutation flush below: the flush (KillHit/ragdoll activation, shell
    // destroy, bones) must not run beside PreRender's rain RayPick / particle worker.
    if (TaskScheduler && m_pre_render_task)
    {
        PERF_WAIT_SITE(xray::perf::wait_site::PreRender);
        PERF_SCOPE(frame_prerender_wait);
        TaskScheduler->Wait(*m_pre_render_task);
        m_pre_render_task = nullptr;
    }

    // Flush the mutations the scheduler/render path deferred during the window (the wait inside is a
    // no-op now - the step was already waited for above).
    if (PhysicsJoinOverlapCallback)
    {
        ZoneScopedN("PhysicsDeferredFlush");
        PERF_SCOPE(frame_ph_flush);
        PhysicsJoinOverlapCallback();
    }

    // Closed after the physics join above (not right after DoRender()): this flag models the actual
    // overlap window, which spans the whole Kick..Join interval (GameThread + physics-on-its-own-thread
    // running parallel to DoRender), not just DoRender() itself.
    SetFrameOverlapActive(false);

    // P1.3: post-join commands. After the physics flush on purpose: the deferred physics mutations hold raw
    // pointers captured during the window, and post-join commands (P2: visual changes, model/light deletion) may
    // invalidate them; and with the window closed (physics and IsFrameOverlapActive() both false) the commands
    // execute their effects immediately instead of deferring them again. Empty (no-op) in a sequential frame today.
    ApplyPostJoinQueue();
    m_game_render_overlap_frame.store(false, std::memory_order_release);

    const u64 frameEndTime = TimerGlobal.GetElapsed_ms();
    const u64 frameTime = frameEndTime - frameStartTime;

    u32 updateDelta = 1000 / ps_fps_limit;

    if (GEnv.isDedicatedServer)
        updateDelta = 1000 / g_svDedicateServerUpdateReate;

    else if (Paused() || g_pGameLevel == nullptr)
        updateDelta = 1000 / ps_fps_limit_in_menu;

#if XR_PERF_METRICS
    perf_frame_total.reset();
#endif
    if (frameTime < updateDelta)
    {
        PERF_SCOPE(frame_fps_sleep);
        Sleep(updateDelta - frameTime);
    }

    if (!b_is_Active)
        Sleep(1);
}

void CRenderDevice::ProcessEvent(const SDL_Event& event)
{
    ZoneScoped;

    switch (event.type)
    {
    case SDL_DISPLAYEVENT:
    {
        switch (event.display.type)
        {
        case SDL_DISPLAYEVENT_ORIENTATION:
        case SDL_DISPLAYEVENT_CONNECTED:
        case SDL_DISPLAYEVENT_DISCONNECTED:
            CleanupVideoModes();
            FillVideoModes();
            if (event.display.display == psDeviceMode.Monitor && event.display.type != SDL_DISPLAYEVENT_CONNECTED)
                Reset();
            else
                UpdateWindowProps();
            break;
        } // switch (event.display.type)
        break;
    }
    case SDL_WINDOWEVENT:
    {
        const auto window = SDL_GetWindowFromID(event.window.windowID);
        if (!window)
            break;
        ImGuiViewport* viewport = ImGui::FindViewportByPlatformHandle(window);
        if (!viewport)
            break;

        switch (event.window.event)
        {
        case SDL_WINDOWEVENT_MOVED:
        {
            if (window == m_sdlWnd)
            {
                UpdateWindowRects();
            }
            if (viewport)
                viewport->PlatformRequestMove = true;
            break;
        }

        case SDL_WINDOWEVENT_DISPLAY_CHANGED:
            psDeviceMode.Monitor = event.window.data1;
            break;

        case SDL_WINDOWEVENT_RESIZED:
            if (window == m_sdlWnd)
                UpdateWindowRects();
            break;

        case SDL_WINDOWEVENT_SIZE_CHANGED:
        {
            if (window == m_sdlWnd)
            {
                UpdateWindowRects();

                if (static_cast<int>(psDeviceMode.Width) == event.window.data1 &&
                    static_cast<int>(psDeviceMode.Height) == event.window.data2)
                    break; // we don't need to reset device if resolution wasn't really changed

                psDeviceMode.Width = event.window.data1;
                psDeviceMode.Height = event.window.data2;

                Reset();
            }
            if (viewport)
                viewport->PlatformRequestResize = true;

            break;
        }

        case SDL_WINDOWEVENT_CLOSE:
        {
            if (viewport)
                viewport->PlatformRequestClose = true;

            if (window == m_sdlWnd)
            {
                Engine.Event.Defer("KERNEL:disconnect");
                Engine.Event.Defer("KERNEL:quit");
            }
            break;
        }
        } // switch (event.window.event)
    }
    } // switch (event.type)

    editor().ProcessEvent(event);
}

void CRenderDevice::Run()
{
    ZoneScoped;

    g_bLoaded = false;
    Log("Starting engine...");

    // Startup timers and calculate timer delta
    dwTimeGlobal = 0;
    Timer_MM_Delta = 0;
    {
        const u32 time_mm = CPU::GetTicks();
        while (CPU::GetTicks() == time_mm)
            ; // wait for next tick
        const u32 time_system = CPU::GetTicks();
        const u32 time_local = TimerAsync();
        Timer_MM_Delta = time_system - time_local;
    }

    SDL_HideWindow(m_sdlWnd); // workaround for SDL bug
    UpdateWindowProps();
    SDL_ShowWindow(m_sdlWnd);
    SDL_RaiseWindow(m_sdlWnd);
}

void CRenderDevice::Shutdown()
{
    ZoneScoped;
    seqAppEnd.Process();
}

u32 app_inactive_time = 0;
u32 app_inactive_time_start = 0;

namespace
{
// P19b (plans/optimization_mt/14-spike-instrumentation.md, "FrameMove и актор"): one seqFrame subscriber of FrameMove -
// a Tracy zone named by the subscriber's class (transient: one row per class in Tracy statistics), a frame/move/* timer of
// perf_metrics and the spike site frame/move/subscriber. Not a spike site for the level: tens of ms there are normal, its
// phases and objects are sites of their own (P19).
#if XR_PERF_METRICS
xray::perf::Metric SeqFrameMetric(pureFrame* object, pcstr cls, bool is_level)
{
    using M = xray::perf::Metric;
    if (is_level)
        return M::fm_level;
    if (g_pGamePersistent && object == static_cast<pureFrame*>(g_pGamePersistent))
        return M::fm_persistent;
    if (object == static_cast<pureFrame*>(&Engine))
        return M::fm_engine;
    if (pInput && object == static_cast<pureFrame*>(pInput))
        return M::fm_input;
    if (Console && object == static_cast<pureFrame*>(Console))
        return M::fm_console;
    if (0 == xr_strcmp(cls, "SoundProcessor"))
        return M::fm_sound;
    if (0 == xr_strcmp(cls, "CRender"))
        return M::fm_render;
    if (0 == strncmp(cls, "CUI", 3) || 0 == xr_strcmp(cls, "CMainMenu"))
        return M::fm_ui;
    return M::fm_other;
}

void DescribeSeqFrameSubscriber(const void* ctx, char* buf, size_t size)
{
    xr_sprintf(buf, size, "cls=%s", static_cast<pcstr>(ctx));
}
#endif

void SeqFrameCall(pureFrame* object)
{
    [[maybe_unused]] const pcstr cls = xray::perf::SpikeClassName(typeid(*object));
    ZoneTransientN(seq_frame_zone, cls, true);
#if XR_PERF_METRICS
    const bool is_level = g_pGameLevel && object == static_cast<pureFrame*>(g_pGameLevel);
    xray::perf::ScopedTime time(SeqFrameMetric(object, cls, is_level));
    if (!is_level)
    {
        PERF_SPIKE_NAMED(spike, "frame/move/subscriber", cls, &DescribeSeqFrameSubscriber, true);
        object->OnPure(object);
        return;
    }
#endif
    object->OnPure(object);
}
} // namespace

void CRenderDevice::FrameMove()
{
    ZoneScoped;

    dwFrame++;
    Core.dwFrame = dwFrame;
    dwTimeContinual = TimerMM.GetElapsed_ms() - app_inactive_time;

    fTimeDeltaReal = Timer.GetElapsed_sec();
    if (!_valid(fTimeDeltaReal))
        fTimeDeltaReal = EPS_S + EPS_S;
    Timer.Start(); // previous frame

    if (psDeviceFlags.test(rsConstantFPS))
    {
        // 20ms = 50fps
        // fTimeDelta = 0.020f;
        // fTimeGlobal += 0.020f;
        // dwTimeDelta = 20;
        // dwTimeGlobal += 20;
        // 33ms = 30fps
        fTimeDelta = 0.033f;
        fTimeGlobal += 0.033f;
        dwTimeDelta = 33;
        dwTimeGlobal += 33;
    }
    else
    {
        if (Paused())
            fTimeDelta = 0.0f;
        else
        {
            fTimeDelta = 0.1f * fTimeDelta + 0.9f * fTimeDeltaReal; // smooth random system activity - worst case ~7% error
            clamp(fTimeDelta, EPS_S + EPS_S, .1f); // limit to 10fps minimum
        }
        fTimeGlobal = TimerGlobal.GetElapsed_sec();
        const u32 _old_global = dwTimeGlobal;
        dwTimeGlobal = TimerGlobal.GetElapsed_ms();
        dwTimeDelta = dwTimeGlobal - _old_global;
    }
    ImGui::GetIO().DeltaTime = fTimeDeltaReal;

    {
        ZoneScopedN("FrameMove/imgui_begin");
        PERF_SCOPE(fm_imgui);
        m_imgui_render->Frame();
        ImGui::NewFrame();
    }

    // Frame move
    stats.EngineTotal.FrameStart();
    stats.EngineTotal.Begin();
    // TODO: HACK to test loading screen.
    // if(!g_bLoaded)

    // P19b: each subscriber through SeqFrameCall (a zone by class, frame/move/*); set here, on the thread of Process()
    seqFrame.SetCallHook(&SeqFrameCall);
    seqFrame.Process();

    g_bLoaded = true;
    // else
    // seqFrame.Process(rp_Frame);
    stats.EngineTotal.End();
    stats.EngineTotal.FrameEnd();

    {
        ZoneScopedN("FrameMove/imgui_end");
        PERF_SCOPE(fm_imgui);
        ImGui::EndFrame();
    }
}

ENGINE_API bool bShowPauseString = true;

void CRenderDevice::Pause(bool bOn, bool bTimer, bool bSound, [[maybe_unused]] pcstr reason)
{
    static int snd_emitters_ = -1;
    if (g_bBenchmark || GEnv.isDedicatedServer)
        return;

    if (bOn)
    {
        if (!Paused())
        {
            if (editor_mode())
                bShowPauseString = false;
#ifdef DEBUG
            else if (xr_strcmp(reason, "li_pause_key_no_clip") == 0)
                bShowPauseString = false;
#endif
            else
                bShowPauseString = true;
        }
        if (bTimer && (!g_pGamePersistent || g_pGamePersistent->CanBePaused()))
        {
            g_pauseMngr().Pause(true);
#ifdef DEBUG
            if (xr_strcmp(reason, "li_pause_key_no_clip") == 0)
                TimerGlobal.Pause(false);
#endif
        }
        if (bSound && GEnv.Sound)
            snd_emitters_ = GEnv.Sound->pause_emitters(true);
    }
    else
    {
        if (bTimer && g_pauseMngr().Paused())
        {
            fTimeDelta = EPS_S + EPS_S;
            g_pauseMngr().Pause(false);
        }
        if (bSound)
        {
            if (snd_emitters_ > 0) // avoid crash
                snd_emitters_ = GEnv.Sound->pause_emitters(false);
            else
            {
#ifdef DEBUG
                Log("GEnv.Sound->pause_emitters underflow");
#endif
            }
        }
    }
}

bool CRenderDevice::Paused() { return g_pauseMngr().Paused(); }

void CRenderDevice::OnWindowActivate(SDL_Window* window, bool activated)
{
    ZoneScoped;

    if (editor().GetState() == editor::ide::visible_state::full)
    {
        if (window != m_sdlWnd)
        {
            if (activated)
                editor().OnAppActivate();
            else
                editor().OnAppDeactivate();
        }
        return;
    }

    if (!GEnv.isDedicatedServer && activated)
        pInput->GrabInput(true);
    else
        pInput->GrabInput(false);

    b_is_Active = activated || psDeviceFlags.test(rsAlwaysActive);

    if (activated != b_is_InFocus)
    {
        b_is_InFocus = activated;
        if (b_is_InFocus)
        {
            TaskScheduler->Pause(false);
            seqAppActivate.Process();
            app_inactive_time += TimerMM.GetElapsed_ms() - app_inactive_time_start;
        }
        else
        {
            app_inactive_time_start = TimerMM.GetElapsed_ms();
            seqAppDeactivate.Process();
            TaskScheduler->Pause(true);
        }
    }
}

void CRenderDevice::time_factor(const float time_factor)
{
    Timer.time_factor(time_factor);
    TimerGlobal.time_factor(time_factor);
    if (!strstr(Core.Params, "-sound_constant_speed"))
        psSoundTimeFactor = time_factor; //--#SM+#--
}

void CRenderDevice::AddSeqFrame(pureFrame* f, bool mt)
{
    if (mt)
        seqFrameMT.Add(f, REG_PRIORITY_HIGH);
    else
        seqFrame.Add(f, REG_PRIORITY_LOW);
}

void CRenderDevice::RemoveSeqFrame(pureFrame* f)
{
    seqFrameMT.Remove(f);
    seqFrame.Remove(f);
}

// The Lua wrapper cache (xrScriptEngine/script_wrapper_cache.hpp): device() gives one wrapper per Lua state
// (CRenderDevice is never a luabind::wrap_base).
// The device outlives the Lua states: the epoch of the slot forgets the wrapper of a closed state, nothing releases
// it. Only this file passes CRenderDevice* to Lua (device()), the specialization is declared and defined here.
static xray::script_wrapper_cache::Slot s_device_lua_wrapper;

namespace luabind
{
template <>
bool get_back_reference<CRenderDevice*>(lua_State* L, CRenderDevice* const& x)
{
    using namespace xray::script_wrapper_cache;
    if (x != &Device)
        return false;
    if (Push(L, s_device_lua_wrapper))
        return true;
    luabind::detail::make_pointer_instance(L, x);
    Store(L, s_device_lua_wrapper);
    return true;
}
} // namespace luabind

void CRenderDevice::script_register(lua_State* luaState)
{
    using namespace luabind;
    module(luaState)
    [
        class_<CRenderDevice>("render_device")
            .def_readonly("width", &CRenderDevice::dwWidth)
            .def_readonly("height", &CRenderDevice::dwHeight)
            .def_readonly("time_delta", &CRenderDevice::dwTimeDelta)
            .def_readonly("f_time_delta", &CRenderDevice::fTimeDelta)
            .def_readonly("cam_pos", &CRenderDevice::vCameraPosition)
            .def_readonly("cam_dir", &CRenderDevice::vCameraDirection)
            .def_readonly("cam_top", &CRenderDevice::vCameraTop)
            .def_readonly("cam_right", &CRenderDevice::vCameraRight)
            //			.def_readonly("view",					&CRenderDevice::mView)
            //			.def_readonly("projection",				&CRenderDevice::mProject)
            //			.def_readonly("full_transform",			&CRenderDevice::mFullTransform)
            .def_readonly("fov", &CRenderDevice::fFOV)
            .def_readonly("aspect_ratio", &CRenderDevice::fASPECT)
            .def_readonly("precache_frame", &CRenderDevice::dwPrecacheFrame)
            .def_readonly("frame", &CRenderDevice::dwFrame)
            .def("time_global", +[](const CRenderDevice* self)
            {
                return (self->dwTimeGlobal);
            })
            .def("is_paused", +[](CRenderDevice* device)
            {
                return device->Paused();
            })
            .def("pause", +[](CRenderDevice* device, bool b)
            {
                device->Pause(b, TRUE, FALSE, "set_device_paused_script");
            }),

        def("app_ready", +[]()
        {
            return g_pGamePersistent->IsLoaded();
        }),
        def("device", +[]()
        {
            return &Device;
        }),
        def("time_global", +[]()
        {
            return Device.dwTimeGlobal;
        }),
        def("time_global_async", +[]()
        {
            return Device.TimerAsync_MMT();
        })
    ];
};

void CLoadScreenRenderer::Start(bool b_user_input)
{
    Device.seqFrame.Add(this, 0);
    Device.seqRender.Add(this, 0);
    m_registered = true;
    m_need_user_input = b_user_input;

    g_pGamePersistent->ShowLoadingScreen(true);
    g_pGamePersistent->LoadBegin();
}

void CLoadScreenRenderer::Stop()
{
    if (!m_registered)
        return;
    Device.seqFrame.Remove(this);
    Device.seqRender.Remove(this);

    m_registered = false;
    m_need_user_input = false;

    g_pGamePersistent->ShowLoadingScreen(false);
    g_pGamePersistent->LoadEnd();
}

void CLoadScreenRenderer::OnFrame()
{
    g_pGamePersistent->LoadStage(false);
}

void CLoadScreenRenderer::OnRender()
{
    g_pGamePersistent->load_draw_internal();
}

void CRenderDevice::CSecondVPParams::SetSVPActive(bool bState) //--#SM+#-- +SecondVP+
{
    isActive = bState;
    if (g_pGamePersistent != nullptr)
        g_pGamePersistent->m_pGShaderConstants->m_blender_mode.z = (isActive ? 1.0f : 0.0f);
}

bool CRenderDevice::CSecondVPParams::IsSVPFrame() // --#SM+#-- +SecondVP+
{
    if (!IsSVPActive())
        return false;
    // 0 = "no skip" (every frame). Avoid % 0; delay 1 also runs every frame (n % 1 == 0).
    const u8 d = frameDelay ? frameDelay : 1;
    return (Device.dwFrame % d) == 0;
}
