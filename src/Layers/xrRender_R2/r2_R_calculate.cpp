#include "stdafx.h"

#include "xrEngine/CustomHUD.h"
#include "xrEngine/device.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Render.h"
#include "Layers/xrRender/xrRender_console.h"
#include "xrCore/Threading/TaskManager.hpp"
#include "xrCore/Profiling/PerfMetrics.hpp"

namespace xray::render::RENDER_NAMESPACE
{
float g_fSCREEN;

extern float r_dtex_range;
extern float r_ssaDISCARD;
extern float r_ssaDONTSORT;
extern float r_ssaLOD_A;
extern float r_ssaLOD_B;
extern float r_ssaLOD_CHAR_A;
extern float r_ssaLOD_CHAR_B;
extern float r_ssaHZBvsTEX;
extern float r_ssaGLOD_start, r_ssaGLOD_end;

extern int ps_r2_mt_calculate;
extern int ps_r2_mt_render;


//-----
void render_main::init()
{
    const int pm = ps_r__phase_mt < 0 ? 0 : (ps_r__phase_mt > 3 ? 3 : ps_r__phase_mt);
    o.mt_calc_enabled =
        RImplementation.o.mt_calculate && !RImplementation.o.oldshadowcascades && !ps_r2_ls_flags.test(R2FLAG_ZFILL) &&
        ((pm & 1) != 0);
    o.mt_draw_enabled = (pm & 2) != 0;
    o.active = true; // always active
}

void render_main::calculate()
{
    // Compatibility wrapper (synchronous callers): resolve the current target at call time.
    auto& ds = RImplementation.r_main_dsgraph_override ? *RImplementation.r_main_dsgraph_override
                                                      : RImplementation.get_imm_context();
    calculate_into(ds, RImplementation.r_main_calc_params);
}

void render_main::calculate_into(R_dsgraph_structure& dsgraph_main, const SRVPCalcParams* params)
{
    ZoneScoped;
    prepare_into(dsgraph_main, params);
    dsgraph_main.build_subspace();
}

void render_main::prepare_into(R_dsgraph_structure& dsgraph_main, const SRVPCalcParams* params)
{
    // Stage C: every camera input comes either from the argument snapshot (worker build) or from
    // the Device - never from mutable override members, which an async task must not re-read.

    dsgraph_main.o.phase = CRender::PHASE_NORMAL;
    dsgraph_main.r_pmask(true, true, true); // enable priority "0,1",+ capture wmarks
    if (RImplementation.r_sun.o.active && RImplementation.o.oldshadowcascades)
        dsgraph_main.set_Recorder(&RImplementation.main_coarse_structure); // this is a show-stopper. Can't be paralleled with sun
    else
        dsgraph_main.set_Recorder(nullptr);
    dsgraph_main.o.use_hom = true;
    dsgraph_main.o.is_main_pass = true;
    // Stage B/C: this build runs concurrently with the main render - opt out of blocks that mutate
    // shared game-side state (light-tracking, spatial sector updates).
    dsgraph_main.o.second_vp_pass = params != nullptr;
    dsgraph_main.o.sector_id = params ? params->sector_id : RImplementation.last_sector_id;
    dsgraph_main.o.portal_traverse_flags =
        CPortalTraverser::VQ_HOM | CPortalTraverser::VQ_SSA | CPortalTraverser::VQ_FADE;
    dsgraph_main.o.spatial_traverse_flags = ISpatial_DB::O_ORDERED;
    dsgraph_main.o.spatial_types = STYPE_RENDERABLE | STYPE_LIGHTSOURCE;
    dsgraph_main.o.view_pos = params ? params->view_pos : Device.vCameraPosition;
    dsgraph_main.o.xform = params ? params->full_transform : Device.mFullTransform;
    // 31-svp-scope.md: the sequential scope build culls against the scope's own frustum (svp_seq_frustum, set by
    // calculate_for(true)); every other build - the main view's ViewBase.
    dsgraph_main.o.view_frustum = params ? params->view_frustum
        : (RImplementation.svp_seq_frustum_valid ? RImplementation.svp_seq_frustum : RImplementation.ViewBase);
    dsgraph_main.o.query_box_side = VIEWPORT_NEAR + EPS_L;
    dsgraph_main.o.precise_portals = true;
    dsgraph_main.o.mt_calculate = o.mt_calc_enabled;
    // The scope build on the SVP worker culls with the scope's thresholds (the globals are the main view's until
    // EndSecondVPCalculateParallel), every other build keeps reading the globals (the LOD fix and H of 31-svp-scope.md,
    // always on since 2026-10-18, 41-cvar-cleanup.md).
    dsgraph_main.o.lod_own = params != nullptr;
    if (dsgraph_main.o.lod_own)
        dsgraph_main.o.lod = params->lod;
    // 31-svp-scope.md: the scope view's build (metrics only) and the main view's HOM test (the worker's build only).
    dsgraph_main.o.svp_view = params != nullptr || RImplementation.svp_seq_build;
    dsgraph_main.o.svp_hom = params != nullptr;
}

// Stage C: create+dispatch the visibility build with the TARGET DSGRAPH captured at task-creation
// time. Mirrors i_render_phase::run() minus the draw subtask chain (render_main::render is empty).
// The task handle lands in *sink so the matching render phase can join exactly this build.
void render_main::launch_build(R_dsgraph_structure& ds, const SRVPCalcParams* params, Task** sink)
{
    if (!o.active)
        return;

    Task* task = &TaskScheduler->CreateTask([this, &ds, params] { calculate_into(ds, params); });
    if (!sink)
        sink = &main_task;
    *sink = task;
    if (o.mt_calc_enabled)
        TaskScheduler->PushTask(*task);
    else
        TaskScheduler->RunTask(*task);
}

void render_main::launch_build_late(R_dsgraph_structure& ds)
{
    if (!o.active)
        return;

    main_task = &TaskScheduler->CreateTask([&ds]
    {
        ZoneScopedN("r_main_build_late");
        PERF_SCOPE(rmain_build_late);
        ds.build_subspace(R_dsgraph_structure::build_scope::late_dynamic);
    });
    if (o.mt_calc_enabled)
        TaskScheduler->PushTask(*main_task);
    else
        TaskScheduler->RunTask(*main_task);
}

void render_main::render()
{
    // TODO
}

//-----

void CRender::Calculate()
{
    ZoneScopedN("r2_calculate");
    calculate_for(Device.m_SecondViewport.IsSecondCalculatePass());
}

// Visibility calculation for one camera pass.
// TODO(parallel-svp, stage C): take an explicit camera snapshot (full_transform / view_pos / fov /
// frustum) instead of reading Device.*, and per-pass copies of the LOD globals below - both are
// required before this function may run concurrently with the main pass.
void CRender::calculate_for(bool second_pass)
{
    const bool main_pass = !second_pass;

    // Early main build: the static half of this build has been running since FrameMove.
    // Joined first - everything below may touch the immediate dsgraph (sector collider, icon pass) or the LOD
    // globals it reads.
    const bool main_early = main_pass && main_build_early_join();
    if (main_pass)
        main_build_early_used = main_early; // R2 (27-render-main.md) reads it in Render()

    if (main_pass)
    {
        // Per-frame lifecycle: pool contexts were reset by D3DXRenderBase::End()->cleanup_contexts()
        // after the previous frame, so a stale SVP handle must not survive into this frame.
        svp_context_id = R_dsgraph_structure::INVALID_CONTEXT_ID;
        svp_dsgraph = nullptr;
        r_main_dsgraph_override = nullptr;
        r_main_calc_params = nullptr;
        svp_cmd_deferred = false;
        svp_parallel = false;
        svp_frame_driver = false; // frame driver gate is re-resolved by Begin...Parallel each frame
        svp_shadow_stage = 0;     // scope shadow prebuild state is per-frame (stage machine)
        // The scope-pass light packages are refilled by this frame's main pass; whatever a previous frame left
        // there (its scope pass skipped/aborted) would be stale light pointers by now.
        LP_svp_normal.clear();
        LP_svp_pending.clear();
        svp_sun_slices_ready = false; // sun-reuse copies (if any) happen later, in main Render/Sun
        svp_geom_on_main = false;
        svp_seq_build = false; // 31-svp-scope.md: cleared after the scope pass already, here for safety
        svp_seq_frustum_valid = false;
    }

    // Transfer to global space to avoid deep pointer access.
    // NOTE: published by BOTH passes for now - safe only while the passes stay sequential.
    // Per-pass copies land together with the parallel orchestration (stage C).
    publish_lod_globals();
    if (main_early && g_fSCREEN != main_build_early_screen)
        PERF_INC(rmain_early_lod_diff); // the static half culled with other thresholds than the dynamic one

    // Configure (main pass only: the second one inherits the identical values already written
    // this frame; concurrent writes would race once the passes run in parallel).
    if (main_pass)
    {
        o.distortion    = o.distortion_enabled;
        o.mt_calculate  = ps_r2_mt_calculate > 0;
#ifdef USE_DX11
        o.mt_render     = ps_r2_mt_render > 0;
#else
        o.mt_render     = 0; // OpenGL does not support parallel draw calls
#endif
    }

    if (m_bFirstFrameAfterReset)
        return;

    auto& dsgraph_main = get_imm_context();

    // Detect camera-sector (main pass only): the scope camera shares the actor position, so the
    // result would be identical, and OnSectorChanged is a game-side callback.
    // Early main build: detected by KickMainBuildEarly already (the same camera).
    if (main_pass && !main_early)
        detect_camera_sector(dsgraph_main);

    // Collect lights (main pass only): same camera position -> identical light set, so the second
    // pass reuses the package gathered above instead of mutating CLight_DB a second time.
    if (main_pass)
    {
        Lights.Update();

        // Per-frame buffer; must not be static — nested or concurrent spatial queries could clear it mid-iteration.
        xr_vector<ISpatial*> spatial_lights;
        spatial_lights.reserve(32);
        g_pGamePersistent->SpatialSpace.q_sphere(spatial_lights, 0, STYPE_LIGHTSOURCE, Device.vCameraPosition, EPS_L);
        for (auto spatial : spatial_lights)
        {
            if (!spatial)
                continue;

            const auto& entity_pos = spatial->spatial_sector_point();
            spatial->spatial_updatesector(dsgraph_main.detect_sector(entity_pos));
            const auto sector_id = spatial->GetSpatialData().sector_id.load(std::memory_order_relaxed);
            if (sector_id == IRender_Sector::INVALID_SECTOR_ID)
                continue; // disassociated from S/P structure

            VERIFY(spatial->GetSpatialData().type & STYPE_LIGHTSOURCE);
            light* L = (light*)spatial->dcast_Light();
            if (!L)
                continue;
            Lights.add_light(L);
        }
    }

    // Dedicated SVP dsgraph (stage B): isolates visibility maps/markers from the main pass.
    // Immediate cmd_list on purpose (alloc_cmd_list=false) - GPU submission stays on the single
    // sequential stream; only CPU-side state is isolated here.
    if (second_pass && !svp_dsgraph)
    {
        svp_context_id = alloc_context(/*alloc_cmd_list=*/false);
        svp_cmd_deferred = false; // legacy sequential path: immediate cmd list
        svp_dsgraph = svp_context_id != R_dsgraph_structure::INVALID_CONTEXT_ID
            ? &get_context(svp_context_id)
            : nullptr;
        if (!svp_dsgraph)
            Msg("! SVP: no free dsgraph context, scope visibility falls back to the immediate one");
    }
    r_main_dsgraph_override = second_pass ? svp_dsgraph : nullptr;

    // 31-svp-scope.md: the sequential scope build (GL, RenderDoc, no free context). It used to cull against the main
    // view's wide ViewBase; now it takes the scope's own frustum - BeginSecondViewportRender has put the scope
    // projection into Device.mProject - cropped to the lens when aimed (L0, always on since 2026-10-18). Read by
    // prepare_into in the build task below; cleared after the scope pass's Render() (RenderSecondViewport), which
    // joins that task.
    if (second_pass)
    {
        svp_seq_build = true;
        Fmatrix scope_full;
        scope_full.mul(Device.mProject, Device.mView);
        float area_pct = 100.f;
        if (svp_lens_crop(scope_full, Device.dwWidth, Device.dwHeight, svp_seq_frustum, area_pct))
        {
            PERF_INC(rsvp_lens_frames);
            PERF_ADD(rsvp_lens_area_pct, u64(area_pct + 0.5f));
        }
        else
        {
            PERF_INC(rsvp_lens_fallback);
            svp_seq_frustum.CreateFromMatrix(scope_full, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);
        }
        svp_seq_frustum_valid = true;
    }

    {
        PERF_WAIT_SITE(xray::perf::wait_site::Hom);
        TaskScheduler->Wait(*ProcessHOMTask);
    }

    r_main.init();
    const bool skip_svp_sun_csm =
        Device.m_SecondViewport.IsSecondCalculatePass() && ps_r__svp_skip_sun_csm;
    if (!skip_svp_sun_csm)
    {
        if (o.oldshadowcascades)
            r_sun_old.init();
        else
            r_sun.init();
    }
#if RENDER != R_R2
    r_rain.init();
#endif

    // Main calc
    if (main_pass)
        BasicStats.Culling.Begin();
    {
        // Early main build: not this frame - the icon pass drains the immediate dsgraph (render_graph) the static half
        // has filled. KickMainBuildEarly ran only with no icons pending; icons queued since wait for the next frame.
        if (g_pGamePersistent && !main_early)
        {
            // C1.0: inventory icons draw live game objects on the immediate context (build side, §2 of 17-c1-semi-pipeline.md)
            ZoneScopedN("r2_calculate/icons");
            PERF_SCOPE_IF(main_pass, rframe_icons);
            g_pGamePersistent->OnWeaponIconRenderPass();
        }
        // Stage C: target captured at task creation - an async main build must not re-read the
        // override member that the scope launcher mutates right after this function returns.
        // For the scope pass (second_pass=true), r_main_dsgraph_override points to the dedicated
        // svp dsgraph; the inner Render() reads from the same override, so the build must write
        // into it - otherwise the inner Render drains an empty graph (only skybox/fences render).
        auto& build_ds = r_main_dsgraph_override ? *r_main_dsgraph_override : get_imm_context();
        if (main_early)
            r_main.launch_build_late(build_ds); // the static half is in build_ds already, options set by the kick
        else
            r_main.launch_build(build_ds, nullptr);
    }
    if (main_pass)
        BasicStats.Culling.End();

    // Rain calc
#if RENDER != R_R2
    r_rain.run();
#endif

    // Sun calc (second dedicated-VP Calculate skips init/run when r__svp_skip_sun_csm — must match Render() skip sync)
    if (!skip_svp_sun_csm)
    {
        if (o.oldshadowcascades)
            r_sun_old.run();
        else
            r_sun.run();
    }
}

void CRender::publish_lod_globals()
{
    const float fov_factor = _sqr(90.f / Device.fFOV);
    g_fSCREEN = float(Target->get_width(RCache) * Target->get_height(RCache)) * fov_factor * (EPS_S + ps_r__LOD);
    r_ssaDISCARD = _sqr(ps_r__ssaDISCARD) / g_fSCREEN;
    r_ssaDONTSORT = _sqr(ps_r__ssaDONTSORT / 3) / g_fSCREEN;
    r_ssaLOD_A = _sqr(ps_r2_ssaLOD_A / 3) / g_fSCREEN;
    r_ssaLOD_B = _sqr(ps_r2_ssaLOD_B / 3) / g_fSCREEN;
    r_ssaLOD_CHAR_A = _sqr(ps_r2_ssaLOD_CHAR_A / 3) / g_fSCREEN;
    r_ssaLOD_CHAR_B = _sqr(ps_r2_ssaLOD_CHAR_B / 3) / g_fSCREEN;
    r_ssaGLOD_start = _sqr(ps_r__GLOD_ssa_start / 3) / g_fSCREEN;
    r_ssaGLOD_end = _sqr(ps_r__GLOD_ssa_end / 3) / g_fSCREEN;
    r_ssaHZBvsTEX = _sqr(ps_r__ssaHZBvsTEX / 3) / g_fSCREEN;
    r_dtex_range = ps_r2_df_parallax_range * g_fSCREEN / (1024.f * 768.f);
}

void CRender::detect_camera_sector(R_dsgraph_structure& dsgraph_main)
{
    if (Device.vCameraDirectionSaved.similar(Device.vCameraPosition, EPS_L))
        return;
    const auto sector_id = dsgraph_main.detect_sector(Device.vCameraPosition);
    if (sector_id != IRender_Sector::INVALID_SECTOR_ID)
    {
        if (sector_id != last_sector_id)
            g_pGamePersistent->OnSectorChanged(sector_id);

        last_sector_id = sector_id;
    }
}

// ---------------------------------------------------------------------------
// Early main build (plans/optimization_mt/26-framemove-game-next.md, "early main build"; always on since 2026-10-18).
// What the static half reads and why it is final at the kick (right after FrameMove, before the RT scheduler part):
//  - camera: Device.vCameraPosition / mFullTransform (OnCameraUpdated in CActor::UpdateCL, frame-guarded, the call
//    after FrameMove is a no-op) and ViewBase (CRender::OnCameraUpdated) - snapshotted into o.* by prepare_into;
//    CalcSSA reads Device.vCameraPosition live, nothing writes it before DoRender (the icon / preview renders that
//    do are guarded by main_build_early_discard, the scope camera switches after the main Render);
//  - HOM: its task was kicked by the same OnCameraUpdated, waited for below;
//  - LOD thresholds (r_ssa*): published below with the main-pass formula, compared again in calculate_for;
//  - camera sector: detected below on main (OnSectorChanged is a game callback; Sectors_xrc of the immediate dsgraph
//    is the static half's collider afterwards);
//  - sectors, portals, static visuals: per-context copies / level data, nothing else touches them meanwhile.
// It writes only the immediate dsgraph (lists, traverser, marker, vis.marker of static visuals) and the HOM stats.
// ---------------------------------------------------------------------------
bool CRender::KickMainBuildEarly()
{
    // A static half nobody consumed (a frame without Calculate): never two in flight on the same dsgraph.
    main_build_early_discard();
#if !defined(USE_DX11)
    return false; // GL keeps the old path (its frames don't overlap game and render anyway)
#else
    if (m_bFirstFrameAfterReset || o.oldshadowcascades || !TaskScheduler || !ProcessHOMTask)
        return false;
    if (Device.m_SecondViewport.IsSecondCalculatePass())
        return false;
    // RenderDoc in-app capture: the old order, like the other parallel paths (SVP frame driver) do.
    if (D3DXRenderBase::RenderDocActive())
        return false;
    if (!g_pGamePersistent || g_pGamePersistent->MainMenuActiveOrLevelNotExist())
        return false;
    // Inventory icons render on the immediate dsgraph in Calculate() (render_graph drains it): old path this frame.
    if (g_pGamePersistent->WeaponIconRenderPassPending())
    {
        PERF_INC(rmain_early_skip_icons);
        return false;
    }
    // A task only when the main build is one anyway (mt_calculate, no Z-prefill - it builds on the immediate dsgraph
    // inside Render() -, no old sun cascades - their recorder).
    r_main.init();
    if (!r_main.o.mt_calc_enabled)
        return false;

    ZoneScopedN("r_main_build_kick");
    auto& ds = get_imm_context();
    publish_lod_globals();
    main_build_early_screen = g_fSCREEN;
    detect_camera_sector(ds);
    {
        PERF_WAIT_SITE(xray::perf::wait_site::Hom);
        TaskScheduler->Wait(*ProcessHOMTask);
    }
    r_main.prepare_into(ds, nullptr);

    Task& task = TaskScheduler->CreateTask([&ds]
    {
        ZoneScopedN("r_main_build_early");
        PERF_SCOPE(rmain_build_early);
        ds.build_subspace(R_dsgraph_structure::build_scope::early_static);
    });
    main_build_early_task.store(&task, std::memory_order_release);
    TaskScheduler->PushTask(task);
    PERF_INC(rmain_early_frames);
    return true;
#endif
}

bool CRender::main_build_early_join()
{
    Task* task = main_build_early_task.exchange(nullptr, std::memory_order_acq_rel);
    if (!task)
        return false;
    ZoneScopedN("r_main_build_early_wait");
    PERF_SCOPE(rmain_build_early_wait);
    PERF_WAIT_SITE(xray::perf::wait_site::RMain);
    TaskScheduler->Wait(*task);
    return true;
}

void CRender::main_build_early_discard()
{
    if (!main_build_early_join())
        return;
    PERF_INC(rmain_early_discard);
    // The frame's Calculate() (if any) then builds everything as before: marker++, traversal, static, dynamic.
    get_imm_context().clear_build_lists();
}

void CRender::JoinMainBuildEarly() { main_build_early_discard(); }

void CRender::End()
{
    main_build_early_discard(); // cleanup_contexts() below resets the dsgraph the static half writes into
    D3DXRenderBase::End();
}

// ---------------------------------------------------------------------------
// Stage C (SVP parallelization): the scope-viewport visibility build runs on a worker while the
// main pass renders. The worker body below is deliberately restricted to pure CPU culling:
//  - no Device.* reads (everything comes from the SRVPCalcParams snapshot);
//  - no shared-global writes: LOD thresholds go into per-pass storage, published into the globals
//    by ApplySecondVPLodGlobals() on the main thread right before this pass drains; the build itself
//    (and its geometry recording) reads the same values from SRVPCalcParams::lod -
//    the globals still hold the MAIN view's thresholds while the worker runs;
//  - no light collection / sector detection / stats / option writes (stage A gates);
//  - no sun/rain shadow phases (GPU work): they run sequentially in SecondVPPostCalculate().
// ---------------------------------------------------------------------------

// Per-pass copies of the LOD thresholds computed by the worker; republished into the file-scope
// globals once the main thread has joined the worker and before the scope pass drains.
static struct SVPLodGlobals
{
    bool valid{false};
    float g_fSCREEN{};
    float ssaDISCARD{};
    float ssaDONTSORT{};
    float ssaLOD_A{};
    float ssaLOD_B{};
    float ssaLOD_CHAR_A{};
    float ssaLOD_CHAR_B{};
    float GLOD_start{};
    float GLOD_end{};
    float HZBvsTEX{};
    float dtex_range{};
} s_svp_lod;

void CRender::ApplySecondVPLodGlobals()
{
    if (!s_svp_lod.valid)
        return;

    g_fSCREEN = s_svp_lod.g_fSCREEN;
    r_ssaDISCARD = s_svp_lod.ssaDISCARD;
    r_ssaDONTSORT = s_svp_lod.ssaDONTSORT;
    r_ssaLOD_A = s_svp_lod.ssaLOD_A;
    r_ssaLOD_B = s_svp_lod.ssaLOD_B;
    r_ssaLOD_CHAR_A = s_svp_lod.ssaLOD_CHAR_A;
    r_ssaLOD_CHAR_B = s_svp_lod.ssaLOD_CHAR_B;
    r_ssaGLOD_start = s_svp_lod.GLOD_start;
    r_ssaGLOD_end = s_svp_lod.GLOD_end;
    r_ssaHZBvsTEX = s_svp_lod.HZBvsTEX;
    r_dtex_range = s_svp_lod.dtex_range;

    s_svp_lod.valid = false;
}

namespace
{
// Publishes this thread's id for the lifetime of the SVP build thread's body; game render
// callbacks compare it with their own thread before touching scheduler-worker-keyed queues
// (o_crow etc.), so only the worker is silenced and the main pass keeps registering objects.
struct SVPWorkerRenderGuard
{
    SVPWorkerRenderGuard()
    {
        g_svp_worker_thread_id.store(std::this_thread::get_id(), std::memory_order_relaxed);
    }
    ~SVPWorkerRenderGuard()
    {
        g_svp_worker_thread_id.store(std::thread::id{}, std::memory_order_relaxed);
    }
};
} // namespace

// Persistent SVP worker body (see the svp_worker_* members in r2.h): parked on
// svp_worker_wake; each wake runs ONE frame's job, then svp_worker_done.Set() and back to
// park. svp_dsgraph/svp_calc_params are re-read at job start - the main thread arms them
// before wake and does not touch them until the matching done.
void CRender::svp_worker_loop()
{
    for (;;)
    {
        svp_worker_wake.Wait();
        if (svp_worker_exit.load(std::memory_order_relaxed))
            return;
        {
            // Per-job scope: the worker id must be published only while the worker actually
            // renders (parity with the old per-frame threads), cleared while parked.
            SVPWorkerRenderGuard worker_render_guard; // reset on every exit path, incl. abort
            R_dsgraph_structure* const ds = svp_dsgraph;
            auto* const params = &svp_calc_params;
            // Stage A2: visibility build - pure CPU culling into the dedicated dsgraph (no
            // Device reads, no shared-state mutation, see the Begin comment).
            {
                ZoneScopedN("SVP/worker_build");
                PERF_SCOPE(rsvp_worker_build);
                r_main.calculate_into(*ds, params);
            }
            if (!svp_build_abort.load(std::memory_order_relaxed))
            {
                // Stage B: record render_graph(0) into the deferred cmd list - submitted later
                // on the main thread (SubmitSVPDeferred), overlapping the main render's
                // lighting/combine tail.
                if (!svp_geom_on_main)
                {
                    ZoneScopedN("SVP/worker_geom");
                    PERF_SCOPE(rsvp_worker_geom);
                    record_second_vp_geometry_into(*ds);
                }
                // Scope shadows are not built here: the main pass copies its smap pages into the
                // dedicated SVP atlas itself (shadow-transfer, see render_lights).
            }
        }
        svp_worker_done.Set();
    }
}

// L0 (plans/optimization_mt/31-svp-scope.md, always on since 2026-10-18). model_scope_lense.ps (r3/r4/gl alike) reads
// rt_secondVP pixel for pixel with the screen around the screen center (svp_uv: 0.5 + offset from the center in
// screen heights; the pincushion and the chroma push the rim taps up to ~6% outward), so the lens shows only the part
// of the scope image under the lens on the screen. With the weapon fully raised the lens sits on the eye axis - in the
// screen center; the circle of r__svp_lens_margin_pct % of the screen height around it (a margin for the sway, the
// recoil and a large ocular is the user's choice) is all the build has to see. Its bounding square in NDC: the screen
// height is 2 units (a radius of k*H pixels = 2k), the width 2 units of W pixels (2k * H / W). The crop scales clip x
// and y about the image center - the columns 1 and 2 of the row-vector transform - so the frustum planes of the
// cropped matrix bound exactly that square; everything else (SSA, LODs, the recording) keeps the full transform.
// Not applied (false): not aimed through this scope (hud_params.x = CWeapon::bInZoomRightNow, false on the
// alternative sight, whose lens is off the eye axis) or not fully raised (hud_params_2.z = the ADS progress
// GetZRotatingFactor, exactly 1 at the end of the raise) - the lens is moving or off-center then.
bool CRender::svp_lens_crop(const Fmatrix& full_transform, u32 width, u32 height, CFrustum& out, float& area_pct) const
{
    if (!width || !height || !g_pGamePersistent || !g_pGamePersistent->m_pGShaderConstants)
        return false;
    const ShadersExternalData& shader_data = *g_pGamePersistent->m_pGShaderConstants;
    if (shader_data.hud_params.x < 0.5f || shader_data.hud_params_2.z < 0.999f)
        return false;

    const float radius = float(clampr(ps_r__svp_lens_margin_pct, 10, 100)) / 100.f; // of the screen height
    const float half_y = _min(2.f * radius, 1.f);
    const float half_x = _min(2.f * radius * float(height) / float(width), 1.f);
    if (half_x >= 1.f && half_y >= 1.f)
        return false; // the circle covers the whole image: nothing to crop

    Fmatrix cropped = full_transform;
    const float sx = 1.f / half_x;
    const float sy = 1.f / half_y;
    cropped._11 *= sx;
    cropped._21 *= sx;
    cropped._31 *= sx;
    cropped._41 *= sx;
    cropped._12 *= sy;
    cropped._22 *= sy;
    cropped._32 *= sy;
    cropped._42 *= sy;
    out.CreateFromMatrix(cropped, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);
    area_pct = half_x * half_y * 100.f;
    return true;
}

bool CRender::BeginSecondVPCalculateParallel(float second_vp_fov, const Fmatrix& scope_project)
{
    // RenderDoc in-app capture active: force the fully sequential scope path. The worker
    // records the scope deferred context on its thread while the main thread keeps issuing
    // Finish/ExecuteCommandList on its own contexts, and that two-writer deferred context
    // crashes inside RenderDoc's hooks (AV in renderdoc.dll at the scope SubmitSVPDeferred).
    // Everything stays on the immediate context - stable and fully captured.
    if (D3DXRenderBase::RenderDocActive())
    {
        static bool s_rdoc_gate_logged = false;
        if (!s_rdoc_gate_logged)
        {
            s_rdoc_gate_logged = true;
            Msg("* SVP: parallel frame driver disabled for this session (RenderDoc active)");
        }
        return false;
    }

    // Guards mirrored from what the sequential second pass tolerated.
    // GL: no deferred contexts exist - recording geometry on a worker thread would interleave
    // GL calls with the main thread's immediate rendering. Fall back to the sequential path.
    if (m_bFirstFrameAfterReset || o.oldshadowcascades || second_vp_fov <= EPS_L ||
        !svp_frame_driver_available())
        return false;

    // Frame driver gate: the worker will record scope lighting (stage C) and combine (stage D)
    // into deferred lists after the geometry stage. The dedicated shadow atlas must exist BEFORE
    // the worker reaches lighting, so it is created here on the main thread; a GL backend or
    // creation failure just keeps those stages inline later without affecting the already-shipped
    // calculate/geometry.
    svp_frame_driver = svp_frame_driver_available();
    // Shadow-transfer: the main pass copies each smap page into the dedicated SVP atlas itself
    // (see render_lights); the worker builds no scope shadows.
    if (svp_frame_driver && !Target->SVPSmapAtlasEnsure())
        svp_frame_driver = false;
    // Stage machine: 3 = shadow-transfer, the MAIN pass delivers copied atlas slices.
    svp_shadow_stage = svp_frame_driver ? 3 : 0;
    // Transfer page groups are filled by the MAIN pass page loop (render_lights) - start the
    // frame clean.
    svp_shadow_page_lights.clear();
    svp_shadow_unshadowed.clear();

    // P2.3: DEFERRED cmd list - the dedicated thread builds visibility AND records
    // render_graph(0) into this list while the main pass renders; the recorded commands are
    // executed on the immediate context at the join point (SubmitSVPDeferred in Render()).
    // Safe because render_graph(0) is query-free and free of RCache/imm interleaves; the
    // lighting phases (occq + RCache) stay on the main thread.
    svp_geom_on_main = false;
    svp_context_id = alloc_context(/*alloc_cmd_list=*/true);
    svp_cmd_deferred = true;
    svp_dsgraph = svp_context_id != R_dsgraph_structure::INVALID_CONTEXT_ID
        ? &get_context(svp_context_id)
        : nullptr;
    if (!svp_dsgraph)
    {
        svp_cmd_deferred = false;
        Msg("! SVP: no free dsgraph context, scope visibility falls back to the sequential path");
        return false;
    }

    // Snapshot. Unlike the legacy sequential pass (which culled against the stale WIDE main
    // frustum), compose the true NARROW scope frustum: at zoom, only a small solid-angle slice of
    // the scene is visible through the lens, so culling here cuts the recorded draw-call count
    // multiples - the inner Render() is CPU-recording-bound, this is where its time goes.
    Fmatrix full_transform;
    full_transform.mul(scope_project, Device.mView);

    auto& p = svp_calc_params;
    // full_transform stays the whole scope image: SSA/LOD, the portal projection and the recording read it.
    p.full_transform = full_transform;
    // L0 (31-svp-scope.md, always on since 2026-10-18, 41-cvar-cleanup.md): only the culling frustum is cropped to the
    // lens. The light filter of the scope pass (SV) tests against the same p.view_frustum.
    float lens_area_pct = 100.f;
    if (svp_lens_crop(full_transform, Device.dwWidth, Device.dwHeight, p.view_frustum, lens_area_pct))
    {
        PERF_INC(rsvp_lens_frames);
        PERF_ADD(rsvp_lens_area_pct, u64(lens_area_pct + 0.5f));
    }
    else
    {
        PERF_INC(rsvp_lens_fallback);
        p.view_frustum.CreateFromMatrix(full_transform, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);
    }
    p.view_pos = Device.vCameraPosition;
    p.fov = second_vp_fov;
    p.width = Device.dwWidth;
    p.height = Device.dwHeight;
    p.sector_id = last_sector_id;

    // P2.3 recording seed: the dedicated thread records render_graph(0) into the deferred list
    // right after the build, using the narrow scope transforms captured here on the main thread.
    svp_seed_view = Device.mView;
    svp_seed_project = scope_project;
    capture_svp_seed_targets();
    svp_cmd_deferred = true;

    r_main_dsgraph_override = svp_dsgraph;
    r_main_calc_params = &svp_calc_params;

    // Per-pass LOD thresholds (computed on the main thread; the shared globals are republished by
    // EndSecondVPCalculateParallel after the join, before this pass drains).
    const float fov_factor = _sqr(90.f / p.fov);
    const float screen =
        float(p.width) * float(p.height) * fov_factor * (EPS_S + ps_r__LOD);
    s_svp_lod.valid = true;
    s_svp_lod.g_fSCREEN = screen;
    s_svp_lod.ssaDISCARD = _sqr(ps_r__ssaDISCARD) / screen;
    s_svp_lod.ssaDONTSORT = _sqr(ps_r__ssaDONTSORT / 3) / screen;
    s_svp_lod.ssaLOD_A = _sqr(ps_r2_ssaLOD_A / 3) / screen;
    s_svp_lod.ssaLOD_B = _sqr(ps_r2_ssaLOD_B / 3) / screen;
    s_svp_lod.ssaLOD_CHAR_A = _sqr(ps_r2_ssaLOD_CHAR_A / 3) / screen;
    s_svp_lod.ssaLOD_CHAR_B = _sqr(ps_r2_ssaLOD_CHAR_B / 3) / screen;
    s_svp_lod.GLOD_start = _sqr(ps_r__GLOD_ssa_start / 3) / screen;
    s_svp_lod.GLOD_end = _sqr(ps_r__GLOD_ssa_end / 3) / screen;
    s_svp_lod.HZBvsTEX = _sqr(ps_r__ssaHZBvsTEX / 3) / screen;
    s_svp_lod.dtex_range = ps_r2_df_parallax_range * screen / (1024.f * 768.f);
    // The same values for the build itself (and its geometry recording) on the worker (prepare_into: lod_own).
    p.lod.ssaDISCARD = s_svp_lod.ssaDISCARD;
    p.lod.ssaLOD_A = s_svp_lod.ssaLOD_A;
    p.lod.ssaLOD_B = s_svp_lod.ssaLOD_B;
    p.lod.ssaLOD_CHAR_A = s_svp_lod.ssaLOD_CHAR_A;
    p.lod.GLOD_start = s_svp_lod.GLOD_start;
    p.lod.GLOD_end = s_svp_lod.GLOD_end;
    p.lod.dtex_range = s_svp_lod.dtex_range;

    r_main.init();

    // Dedicated PERSISTENT thread (see the svp_worker_* member comment in r2.h): the task
    // scheduler starves this build behind the main render's own subtasks, which serializes
    // the pass again. Created lazily on the first scope frame and parked on svp_worker_wake
    // between jobs - the job contents (stage A2 visibility build, stage B deferred geometry
    // recording) live in svp_worker_loop above.
    svp_build_abort.store(false);
    if (!svp_build_thread.joinable())
        svp_build_thread = std::thread([this] { svp_worker_loop(); });
    svp_worker_done.Reset(); // drop a stale release (a job aborted by teardown), auto-reset event
    svp_worker_wake.Set();
    svp_parallel = true;
    return true;
}

void CRender::EndSecondVPCalculateParallel()
{
    // The JOB wait lives HERE (right after the main Render) deliberately: past this point the
    // main thread resumes RCache/constant-table activity (camera switch, sun cascades, scope
    // pass), which must not run concurrently with the worker's own constant recording - a
    // null-CB crash was observed in dx11ConstantBuffer::set when stage C overlapped
    // SetCacheXform. The persistent worker parks after this wait; the THREAD itself is joined
    // only on reset/teardown (JoinSecondVPBuildThread), never per frame.
    if (!svp_parallel)
        return;

    svp_worker_done.Wait(); // blocking; the worker signals after every job, incl. aborts
    release_svp_seed_targets(); // the stage B recording that used them is over

    // Publish the worker's per-pass LOD thresholds before the scope pass drains (main render is
    // already finished at this point, so the globals are exclusive again).
    ApplySecondVPLodGlobals();
}

void CRender::JoinSecondVPBuildThread()
{
    // Full teardown (reset/level unload/dtor only): ask the persistent worker to exit, wake it
    // (the abort flag breaks an in-flight stage-C wait; the wake release also covers a worker
    // that is still busy - it consumes it at the next park), and join. Per-frame code NEVER
    // joins - EndSecondVPCalculateParallel only waits for the current job.
    if (svp_build_thread.joinable())
    {
        svp_build_abort.store(true, std::memory_order_relaxed);
        svp_worker_exit.store(true, std::memory_order_relaxed);
        svp_worker_wake.Set();
        svp_build_thread.join();
        svp_worker_exit.store(false, std::memory_order_relaxed);
        svp_build_abort.store(false, std::memory_order_relaxed);
    }
    release_svp_seed_targets(); // teardown: never outlive the render target that owns them
}

void CRender::AbortSecondVPCalculate()
{
    if (!svp_parallel)
        return;

    // Let the worker skip its remaining stages instead of finishing them pointlessly.
    svp_build_abort.store(true);
    // Stop THIS job but keep the persistent worker alive - it parks on svp_worker_wake; the
    // thread itself is joined only by reset/teardown. Wait for the park BEFORE freeing the
    // job state below (dsgraph/overrides the worker may still touch inside the job).
    svp_worker_done.Wait();
    release_svp_seed_targets(); // the aborted job is over (main thread)
    svp_shadow_stage = 0;
    if (svp_context_id != R_dsgraph_structure::INVALID_CONTEXT_ID)
    {
        release_context(svp_context_id);
        svp_context_id = R_dsgraph_structure::INVALID_CONTEXT_ID;
    }
    svp_dsgraph = nullptr;
    r_main_dsgraph_override = nullptr;
    r_main_calc_params = nullptr;
    svp_parallel = false;
}

bool CRender::svp_sun_reuse_active() const
{
    // Stage 2 (sun-reuse): reuse the MAIN pass sun cascades in the scope view. Requires this
    // frame's frame driver (shadow-transfer) - the sun slices are copied into the dedicated SVP
    // atlas right after the main pass Render/Sun (see r2_R_render.cpp). Otherwise the legacy
    // rebuild/skip flags (r__svp_skip_sun_csm) keep ruling.
    return svp_parallel && svp_frame_driver;
}

void CRender::SecondVPPostCalculate()
{
    // Sun/rain tail of the old sequential second Calculate(), executed after
    // BeginSecondViewportRender() switched the Device to the scope projection.
    // Sun-reuse mode skips the sun entirely: the scope pass accumulates the MAIN pass cascades
    // from the transferred atlas slices (see the reuse branch in Render/Sun) - no rebuild task
    // is started, so the later r_sun.sync() in the scope Render/Sun is a no-op.
    const bool skip_sun = ps_r__svp_skip_sun_csm != 0;
    const bool sun_reuse = svp_sun_reuse_active();
    if (!skip_sun && !sun_reuse)
    {
        if (o.oldshadowcascades)
            r_sun_old.init();
        else
            r_sun.init();
    }
#if RENDER != R_R2
    r_rain.init();
#endif
#if RENDER != R_R2
    r_rain.run();
#endif
    if (!skip_sun && !sun_reuse)
    {
        if (o.oldshadowcascades)
            r_sun_old.run();
        else
            r_sun.run();
    }
}
} // namespace xray::render::RENDER_NAMESPACE
