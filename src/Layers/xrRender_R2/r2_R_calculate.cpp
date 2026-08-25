#include "stdafx.h"

#include "xrEngine/CustomHUD.h"
#include "xrEngine/device.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Render.h"
#include "Layers/xrRender/xrRender_console.h"
#include "xrCore/Threading/TaskManager.hpp"

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
    dsgraph_main.o.view_frustum = params ? params->view_frustum : RImplementation.ViewBase;
    dsgraph_main.o.query_box_side = VIEWPORT_NEAR + EPS_L;
    dsgraph_main.o.precise_portals = true;
    dsgraph_main.o.mt_calculate = o.mt_calc_enabled;

    dsgraph_main.build_subspace();
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
    }

    // Transfer to global space to avoid deep pointer access.
    // NOTE: published by BOTH passes for now - safe only while the passes stay sequential.
    // Per-pass copies land together with the parallel orchestration (stage C).
    float fov_factor = _sqr(90.f / Device.fFOV);
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
    if (main_pass && !Device.vCameraDirectionSaved.similar(Device.vCameraPosition, EPS_L))
    {
        const auto sector_id = dsgraph_main.detect_sector(Device.vCameraPosition);
        if (sector_id != IRender_Sector::INVALID_SECTOR_ID)
        {
            if (sector_id != last_sector_id)
                g_pGamePersistent->OnSectorChanged(sector_id);

            last_sector_id = sector_id;
        }
    }

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
            const auto sector_id = spatial->GetSpatialData().sector_id;
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

    TaskScheduler->Wait(*ProcessHOMTask);

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
        if (g_pGamePersistent)
            g_pGamePersistent->OnWeaponIconRenderPass();
        // Stage C: target captured at task creation - an async main build must not re-read the
        // override member that the scope launcher mutates right after this function returns.
        r_main.launch_build(get_imm_context(), nullptr);
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

// ---------------------------------------------------------------------------
// Stage C (SVP parallelization): the scope-viewport visibility build runs on a worker while the
// main pass renders. The worker body below is deliberately restricted to pure CPU culling:
//  - no Device.* reads (everything comes from the SRVPCalcParams snapshot);
//  - no shared-global writes: LOD thresholds go into per-pass storage, published into the globals
//    by ApplySecondVPLodGlobals() on the main thread right before this pass drains;
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

bool CRender::BeginSecondVPCalculateParallel(float second_vp_fov, const Fmatrix& scope_project)
{
    // Guards mirrored from what the sequential second pass tolerated.
    if (m_bFirstFrameAfterReset || o.oldshadowcascades || second_vp_fov <= EPS_L)
        return false;

    // P2.3: DEFERRED cmd list - the dedicated thread builds visibility AND records
    // render_graph(0) into this list while the main pass renders; the recorded commands are
    // executed on the immediate context at the join point (SubmitSVPDeferred in Render()).
    // Safe because render_graph(0) is query-free and free of RCache/imm interleaves; the
    // lighting phases (occq + RCache) stay on the main thread.
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
    p.full_transform = full_transform;
    p.view_frustum.CreateFromMatrix(full_transform, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);
    p.view_pos = Device.vCameraPosition;
    p.fov = second_vp_fov;
    p.width = Device.dwWidth;
    p.height = Device.dwHeight;
    p.sector_id = last_sector_id;

    // P2.3 recording seed: the dedicated thread records render_graph(0) into the deferred list
    // right after the build, using the narrow scope transforms captured here on the main thread.
    svp_seed_view = Device.mView;
    svp_seed_project = scope_project;
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

    r_main.init();

    // Dedicated thread (see member comment): the task scheduler starves this build behind the
    // main render's own subtasks, which serializes the pass again. calculate_into is pure CPU
    // culling into the dedicated dsgraph - no Device reads, no shared-state mutation (gated).
    // P2.3: right after the build, the same thread records render_graph(0) into the deferred
    // cmd list - overlapping the main render's lighting/combine tail. No submit here: the main
    // thread executes the recorded list in Render() (SubmitSVPDeferred).
    svp_build_thread = std::thread([this, ds = svp_dsgraph, params = &svp_calc_params] {
        r_main.calculate_into(*ds, params);
        record_second_vp_geometry_into(*ds);
    });
    svp_parallel = true;
    return true;
}

void CRender::EndSecondVPCalculateParallel()
{
    if (!svp_parallel)
        return;

    JoinSecondVPBuildThread();

    // Publish the worker's per-pass LOD thresholds before the scope pass drains (main render is
    // already finished at this point, so the globals are exclusive again).
    ApplySecondVPLodGlobals();
}

void CRender::JoinSecondVPBuildThread()
{
    if (svp_build_thread.joinable())
        svp_build_thread.join();
}

void CRender::AbortSecondVPCalculate()
{
    if (!svp_parallel)
        return;

    JoinSecondVPBuildThread();
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

void CRender::SecondVPPostCalculate()
{
    // Sun/rain tail of the old sequential second Calculate(), executed after
    // BeginSecondViewportRender() switched the Device to the scope projection.
    const bool skip_sun = ps_r__svp_skip_sun_csm != 0;
    if (!skip_sun)
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
    if (!skip_sun)
    {
        if (o.oldshadowcascades)
            r_sun_old.run();
        else
            r_sun.run();
    }
}
} // namespace xray::render::RENDER_NAMESPACE
