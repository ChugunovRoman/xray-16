#include "stdafx.h"

#include "xrCore/Threading/TaskManager.hpp"

#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/IGame_Level.h"
#include "xrEngine/CameraManager.h"
#include "xrEngine/CustomHUD.h"
#include "xrEngine/Render.h"
#include "xrEngine/device.h"
#include "xrEngine/xr_object.h"

#include "Layers/xrRender/FBasicVisual.h"
#include "Layers/xrRender/xrRender_console.h"

namespace xray::render::RENDER_NAMESPACE
{
void CRender::RenderMenu()
{
#if defined(USE_DX11)
    TracyD3D11Zone(HW.profiler_ctx, "render_menu");
#endif
    PIX_EVENT(render_menu);
    //	Globals
    RCache.set_CullMode(CULL_CCW);
    RCache.set_Stencil(FALSE);
    RCache.set_ColorWriteEnable();

    // Main Render
    {
        Target->u_setrt(RCache, Target->rt_Generic_0, nullptr, nullptr, Target->rt_Base_Depth); // LDR RT
        g_pGamePersistent->OnRenderPPUI_main(); // PP-UI
    }
    // Distort
    {
        Target->u_setrt(RCache, Target->rt_Generic_1, nullptr, nullptr, Target->rt_Base_Depth); // Now RT is a distortion mask
        RCache.ClearRT(Target->rt_Generic_1, color_rgba(127, 127, 0, 127));
        g_pGamePersistent->OnRenderPPUI_PP(); // PP-UI
    }

    // Actual Display
    Target->u_setrt(RCache, Device.dwWidth, Device.dwHeight, Target->get_base_rt(), 0, 0, Target->get_base_zb());
    RCache.set_Shader(Target->s_menu);
    RCache.set_Geometry(Target->g_menu);

    Fvector2 p0, p1;
    u32 Offset;
    u32 C = color_rgba(255, 255, 255, 255);
    float _w = float(Device.dwWidth);
    float _h = float(Device.dwHeight);
    float d_Z = EPS_S;
    float d_W = 1.f;
    p0.set(.5f / _w, .5f / _h);
    p1.set((_w + .5f) / _w, (_h + .5f) / _h);

    FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, Target->g_menu->vb_stride, Offset);
#if defined(USE_DX11)
    pv->set(EPS, float(_h + EPS), d_Z, d_W, C, p0.x, p1.y);
    pv++;
    pv->set(EPS, EPS, d_Z, d_W, C, p0.x, p0.y);
    pv++;
    pv->set(float(_w + EPS), float(_h + EPS), d_Z, d_W, C, p1.x, p1.y);
    pv++;
    pv->set(float(_w + EPS), EPS, d_Z, d_W, C, p1.x, p0.y);
    pv++;
#elif defined(USE_OGL)
    pv->set(EPS, EPS, d_Z, d_W, C, p0.x, p0.y);
    pv++;
    pv->set(EPS, float(_h + EPS), d_Z, d_W, C, p0.x, p1.y);
    pv++;
    pv->set(float(_w + EPS), EPS, d_Z, d_W, C, p1.x, p0.y);
    pv++;
    pv->set(float(_w + EPS), float(_h + EPS), d_Z, d_W, C, p1.x, p1.y);
    pv++;
#else
#   error No graphics API selected or enabled!
#endif
    RImplementation.Vertex.Unlock(4, Target->g_menu->vb_stride);
    RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);
}

extern u32 g_r;

// Second viewport pass (m_SecondViewportPass): this runs again after a second Calculate() with scope FOV.
// Pipeline notes:
// - q_sync_point Wait/End + r_main.sync(): always run each pass — GPU/CPU ordering vs. the current dsgraph;
//   not exposed as a skip (high risk of races or corruption).
// - MSAA mark_msaa_edges: per pass; G-buffer MSAA targets are repopulated each time.
// - r_rain.sync(): optional r__svp_skip_rain_sync (rain may disagree with main pass timing).
// - r_sun / r_sun_old .sync(): refresh sun cascades for current frustum; r__svp_skip_sun_csm skips the *second*
//   pass init/run/sync (see r2_R_calculate.cpp) and reuses main-pass cascades (often wrong through scope).
// - Details / Wallmarks: r__svp_skip_details, r__svp_skip_wallmarks.
// - Z-prefill (R2FLAG_ZFILL): r__svp_skip_zfill skips only for the SVP pass.
// - Reusing shadow maps between main and SVP without re-sync is a separate optimization (not implemented).

// P2.3 (worker part): seed the deferred cmd list with the FULL initial pipeline state (a deferred
// context inherits nothing) and record render_graph(0) into it. Runs on the dedicated thread right
// after the visibility build, overlapping the main render's lighting/combine tail. Deliberately
// excludes lods (shared DVB fill), Details (shared shader-constant flips) and the albedo copy
// (Vertex.Lock quad) - those stay on the main thread (see Render() Part0). No submit here: the
// main thread executes the recorded list via SubmitSVPDeferred after joining this thread.
void CRender::record_second_vp_geometry_into(R_dsgraph_structure& ds)
{
    // NOTE: never call CBackend::Invalidate() on this pooled deferred context - it re-labels
    // the backend as the immediate context (context_id = IMM_CTX_ID) and the following submit
    // then calls FinishCommandList on the IMMEDIATE context, killing the device. The pool
    // cache staleness is handled engine-side (CBackend::ResetDeferredCache at alloc/submit).

    // Twin refs with fallback to the rt_* members when the scaled twin set does not exist
    // (r__second_vp_render_scale == 1): identical bindings, sequential-only in that case.
    const ref_rt& rtP = Target->svp_Position ? Target->svp_Position : Target->rt_Position;
    const ref_rt& rtN = Target->svp_Normal ? Target->svp_Normal : Target->rt_Normal;
    const ref_rt& rtC = Target->svp_Color ? Target->svp_Color : Target->rt_Color;
    const ref_rt& rtA = Target->svp_Accumulator ? Target->svp_Accumulator : Target->rt_Accumulator;
    const ref_rt& rtZ = Target->svp_MSAADepth ? Target->svp_MSAADepth : Target->rt_MSAADepth;

    if (!o.gbuffer_opt)
    {
        if (o.albedo_wo)
            Target->u_setrt(ds.cmd_list, rtP, rtN, rtA, rtZ);
        else
            Target->u_setrt(ds.cmd_list, rtP, rtN, rtC, rtZ);
    }
    else
    {
        if (o.albedo_wo)
            Target->u_setrt(ds.cmd_list, rtP, rtA, rtZ);
        else
            Target->u_setrt(ds.cmd_list, rtP, rtC, rtZ);
    }

    // Stencil: write 0x1 at every geometry pixel (combine shades only marked pixels).
    ds.cmd_list.set_Stencil(TRUE, D3DCMP_ALWAYS, 0x01, 0xff, 0x7f, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP);
    ds.cmd_list.set_CullMode(CULL_CCW);
    ds.cmd_list.set_ColorWriteEnable();

    // A deferred context inherits nothing: viewport and camera transforms come from the launch
    // snapshot (narrow scope frustum transforms captured in BeginSecondVPCalculateParallel).
    ds.cmd_list.SetViewport({ 0.f, 0.f, float(rtP->dwWidth), float(rtP->dwHeight), 0.f, 1.f });
    ds.cmd_list.set_xform_world(Fidentity);
    ds.cmd_list.set_xform_view(svp_seed_view);
    ds.cmd_list.set_xform_project(svp_seed_project);

    ds.render_graph(0);
}

// P2.3: executes the recorded deferred commands on the immediate context. No-op on the legacy
// sequential path (immediate cmd list - commands were already executed inline).
void CRender::SubmitSVPDeferred(R_dsgraph_structure& ds)
{
    if (svp_cmd_deferred)
        ds.cmd_list.submit();
}

void CRender::ReleaseSVPReplayLists()
{
#if defined(USE_DX11)
    for (void* list : svp_smap_replay_lists)
        static_cast<ID3D11CommandList*>(list)->Release();
#endif
    svp_smap_replay_lists.clear();
}

void CRender::Render()
{
    ZoneScoped;
#if defined(USE_DX11)
    TracyD3D11Zone(HW.profiler_ctx, "Render");
#endif
    PIX_EVENT(CRender_Render);

    g_r = 1;

    const bool svp_pass = m_SecondViewportPass;

    rmNormal(RCache);

    IMainMenu* pMainMenu = g_pGamePersistent ? g_pGamePersistent->m_pMainMenu : 0;
    bool bMenu = pMainMenu ? pMainMenu->CanSkipSceneRendering() : false;

    // XXX: do we need to handle case when there is level, but HUD isn't loaded yet?
    // if (!(g_pGameLevel && g_hud) || bMenu)
    if (!g_pGameLevel || bMenu)
    {
        Target->u_setrt(RCache, Device.dwWidth, Device.dwHeight, Target->get_base_rt(), 0, 0, Target->get_base_zb());
        return;
    }

    if (m_bFirstFrameAfterReset)
    {
        m_bFirstFrameAfterReset = false;
        return;
    }

    //.	VERIFY					(g_pGameLevel && g_pGameLevel->pHUD);
    // Stage B (SVP): the dedicated scope pass drains its own dsgraph context (see calculate_for).
    auto& dsgraph = (svp_pass && r_main_dsgraph_override) ? *r_main_dsgraph_override : get_imm_context();
    // Two CBackend frontends share one immediate device context this frame: RCache (imm) and the
    // SVP dsgraph's own cmd_list. After alloc_context() the SVP instance is Invalidate()-dirty, so
    // its FIRST draw would ApplyRTandZB its empty cached bindings (null RT/DSV) - discarding the
    // whole scope G-buffer pass (symptom: only the skybox visible in the lens). Seed it with the
    // exact output-merger setup phase_scene_begin() uses, plus the camera transforms (RCache got
    // those during frame setup; the SVP instance's caches are cold).
    if (svp_pass)
    {
        auto& c = dsgraph.cmd_list;
        if (!RImplementation.o.gbuffer_opt)
        {
            if (RImplementation.o.albedo_wo)
                Target->u_setrt(c, Target->rt_Position, Target->rt_Normal, Target->rt_Accumulator, Target->rt_MSAADepth);
            else
                Target->u_setrt(c, Target->rt_Position, Target->rt_Normal, Target->rt_Color, Target->rt_MSAADepth);
        }
        else
        {
            if (RImplementation.o.albedo_wo)
                Target->u_setrt(c, Target->rt_Position, Target->rt_Accumulator, Target->rt_MSAADepth);
            else
                Target->u_setrt(c, Target->rt_Position, Target->rt_Color, Target->rt_MSAADepth);
        }
        c.set_Stencil(TRUE, D3DCMP_ALWAYS, 0x01, 0xff, 0x7f, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP);
        c.set_CullMode(CULL_CCW);
        c.set_ColorWriteEnable();
        c.set_xform_world(Fidentity);
        c.set_xform_view(Device.mView);
        c.set_xform_project(Device.mProject);
    }

    // HUD overlay scope (g_3d_scopes 3): capture the scene camera while it is still intact
    // (a protective snapshot - used by RenderHudOverlayToTexture to render the HUD with the SAME
    // view/proj/FOV the world pass used, regardless of later camera edits).
    // Only capture in the MAIN pass: a second viewport pass (RenderSecondViewport, ps_r__dedicated_second_vp)
    // re-enters Render() with the scope camera and would otherwise overwrite m_hudOvlCam with a wrong one.
    if (!svp_pass)
    {
        m_hudOvlCam.mView.set(Device.mView);
        m_hudOvlCam.mProject.set(Device.mProject);
        m_hudOvlCam.mFullTransform.set(Device.mFullTransform);
        m_hudOvlCam.mInvView.set(Device.mInvView);
        m_hudOvlCam.mInvFullTransform.set(Device.mInvFullTransform);
        m_hudOvlCam.vCameraPosition.set(Device.vCameraPosition);
        m_hudOvlCam.vCameraDirection.set(Device.vCameraDirection);
        m_hudOvlCam.vCameraTop.set(Device.vCameraTop);
        m_hudOvlCam.vCameraRight.set(Device.vCameraRight);
        m_hudOvlCam.fFOV = Device.fFOV;
        m_hudOvlCam.fASPECT = Device.fASPECT;
        m_hudOvlCam.valid = true;
    }

    //******* Z-prefill calc - DEFERRER RENDERER
    if (ps_r2_ls_flags.test(R2FLAG_ZFILL) && !(svp_pass && ps_r__svp_skip_zfill))
    {
        ZoneScopedN("Render/ZPrefill/Build");
        PIX_EVENT(DEFER_Z_FILL);
        BasicStats.Culling.Begin();
        float z_distance = ps_r2_zfill;
        Fmatrix m_zfill, m_project;
        m_project.build_projection(deg2rad(Device.fFOV /* *Device.fASPECT*/), Device.fASPECT, VIEWPORT_NEAR,
            z_distance * g_pGamePersistent->Environment().CurrentEnv.far_plane);
        m_zfill.mul(m_project, Device.mView);

        if (last_sector_id != IRender_Sector::INVALID_SECTOR_ID)
        {
            dsgraph.o.phase = PHASE_SMAP;
            dsgraph.r_pmask(true, false); // enable priority "0"
            dsgraph.set_Recorder(nullptr);
            dsgraph.o.use_hom = true;
            dsgraph.o.is_main_pass = true;
            dsgraph.o.sector_id = last_sector_id;
            dsgraph.o.portal_traverse_flags = CPortalTraverser::VQ_HOM | CPortalTraverser::VQ_SSA | CPortalTraverser::VQ_FADE;
            dsgraph.o.spatial_traverse_flags = ISpatial_DB::O_ORDERED;
            dsgraph.o.spatial_types = STYPE_RENDERABLE | STYPE_LIGHTSOURCE;
            dsgraph.o.view_pos = Device.vCameraPosition;
            dsgraph.o.xform = m_zfill;
            dsgraph.o.view_frustum = ViewBase;
            dsgraph.o.query_box_side = VIEWPORT_NEAR + EPS_L;
            dsgraph.o.precise_portals = true;

            dsgraph.build_subspace();
        }
        BasicStats.Culling.End();
    }

    //*******
    // Sync point
    {
        ZoneScopedN("Render/SyncPoint");
        BasicStats.WaitS.Begin();
        {
            q_sync_point.Wait(ps_r2_wait_sleep, ps_r2_wait_timeout);
        }
        BasicStats.WaitS.End();
        q_sync_point.End();
    }

    r_main.sync();

    if (ps_r2_ls_flags.test(R2FLAG_ZFILL) && !(svp_pass && ps_r__svp_skip_zfill))
    {
        ZoneScopedN("Render/ZPrefill/Flush");
        // flush
        Target->phase_scene_prepare();
        dsgraph.cmd_list.set_ColorWriteEnable(FALSE);
        dsgraph.render_graph(0);
        dsgraph.cmd_list.set_ColorWriteEnable();
    }
    else
    {
        Target->phase_scene_prepare();
    }

    BOOL split_the_scene_to_minimize_wait = FALSE;
    if (ps_r2_ls_flags.test(R2FLAG_EXP_SPLIT_SCENE))
        split_the_scene_to_minimize_wait = TRUE;
    // P2.1: the scope pass always uses the unified geometry path (the legacy split variant is a
    // memory-saving option orthogonal to the scope pipeline).
    if (svp_pass)
        split_the_scene_to_minimize_wait = FALSE;

    //******* Main render :: PART-0	-- first
#ifdef USE_OGL
    if (psDeviceFlags.test(rsWireframe))
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
#endif
    if (!split_the_scene_to_minimize_wait)
    {
        ZoneScopedN("Render/MainPart0/NoSplit");
        PIX_EVENT(DEFER_PART0_NO_SPLIT);
        // level, DO NOT SPLIT
        if (svp_pass)
        {
            // P2.3: parallel path — the worker already recorded render_graph(0) into the deferred
            // list (joined in EndSecondVPCalculateParallel); only lods/Details are added here,
            // then SubmitSVPDeferred executes everything. Legacy path — the cmd list is
            // immediate, so record_second_vp_geometry_into records AND executes inline.
            // Legacy: capture the scope transforms at record time (Begin is active here);
            // the parallel path captured them in BeginSecondVPCalculateParallel.
            if (!svp_cmd_deferred || svp_geom_on_main)
            {
                svp_seed_view = Device.mView;
                svp_seed_project = Device.mProject;
                record_second_vp_geometry_into(dsgraph);
            }
            dsgraph.render_lods(true, true);
            if (Details && !ps_r__svp_skip_details)
                Details->Render(dsgraph.cmd_list);
            SubmitSVPDeferred(dsgraph);
        }
        else
        {
            Target->phase_scene_begin();
            {
                // Skip 3D HUD only for legacy alternating SVP (no dedicated RT). Dedicated second pass draws HUD here + hud_ui below.
                // HUD overlay scope (g_3d_scopes 3): HUD goes to the offscreen overlay instead of the world pass.
                const bool skip_world_hud = m_SecondViewportPass || m_HudOverlayActive ||
                    (!ps_r__dedicated_second_vp && Device.m_SecondViewport.IsSVPFrame());
                if (!skip_world_hud)
                    dsgraph.render_hud();
            }

            dsgraph.render_graph(0);
            dsgraph.render_lods(true, true);
            if (Details && !(svp_pass && ps_r__svp_skip_details))
                Details->Render(dsgraph.cmd_list);
        }
        Target->phase_scene_end();
    }
    else
    {
        ZoneScopedN("Render/MainPart0/Split");
        PIX_EVENT(DEFER_PART0_SPLIT);
        // level, SPLIT
        Target->phase_scene_begin();
        dsgraph.render_graph(0);
        Target->disable_aniso();
    }
#ifdef USE_OGL
    if (psDeviceFlags.test(rsWireframe))
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
#endif

    {
        //******* Occlusion testing of volume-limited light-sources
        ZoneScopedN("Render/Occlusion/Prepare");
        Target->phase_occq();
        LP_normal.clear();
        LP_pending.clear();
        if (o.msaa)
        {
#if defined(USE_DX11)
            dsgraph.cmd_list.set_ZB(Target->rt_MSAADepth->pZRT[dsgraph.cmd_list.context_id]);
#elif defined(USE_OGL)
            dsgraph.cmd_list.set_ZB(Target->rt_MSAADepth->pZRT);
#endif
        }
    }
    {
        ZoneScopedN("Render/Occlusion/VisPrepare");
        PIX_EVENT(DEFER_TEST_LIGHT_VIS);
        light_Package& LP = Lights.package;

        // stats
        Stats.l_shadowed = LP.v_shadowed.size();
        Stats.l_unshadowed = LP.v_point.size() + LP.v_spot.size();
        Stats.l_total = Stats.l_shadowed + Stats.l_unshadowed;

        // perform tests
        size_t count = 0;
        count = _max(count, LP.v_point.size());
        count = _max(count, LP.v_spot.size());
        count = _max(count, LP.v_shadowed.size());
        for (size_t it = 0; it < count; it++)
        {
            if (it < LP.v_point.size())
            {
                light* L = LP.v_point[it];
                // Frame-driver stage 1b: the scope pass skips vis_prepare altogether - its
                // package is pre-filtered by the narrow lens frustum and stage C will record
                // lighting on the worker, away from occlusion queries. The MAIN pass keeps
                // running the tests for both passes (shared eye position -> same decisions).
                if (!svp_pass)
                    L->vis_prepare(dsgraph.cmd_list);
                if (L->vis.pending)
                    LP_pending.v_point.push_back(L);
                else
                    LP_normal.v_point.push_back(L);
            }
            if (it < LP.v_spot.size())
            {
                light* L = LP.v_spot[it];
                // Frame-driver stage 1b: skip vis_prepare on the scope pass (see the point branch).
                if (!svp_pass)
                    L->vis_prepare(dsgraph.cmd_list);
                if (L->vis.pending)
                    LP_pending.v_spot.push_back(L);
                else
                    LP_normal.v_spot.push_back(L);
            }
            if (it < LP.v_shadowed.size())
            {
                light* L = LP.v_shadowed[it];
                // Frame-driver stage 1b: skip vis_prepare on the scope pass (see the point branch).
                if (!svp_pass)
                    L->vis_prepare(dsgraph.cmd_list);
                if (L->vis.pending)
                    LP_pending.v_shadowed.push_back(L);
                else
                    LP_normal.v_shadowed.push_back(L);
            }
        }
    }
    LP_normal.sort();
    LP_pending.sort();

    //******* Main render :: PART-1 (second)
    if (split_the_scene_to_minimize_wait)
    {
        PIX_EVENT(DEFER_PART1_SPLIT);
        // skybox can be drawn here
        if (false)
        {
            Target->u_setrt(dsgraph.cmd_list, Target->rt_Generic_0_r, Target->rt_Generic_1_r, nullptr, Target->rt_MSAADepth);
            dsgraph.cmd_list.set_CullMode(CULL_NONE);
            dsgraph.cmd_list.set_Stencil(FALSE);

            // draw skybox
            dsgraph.cmd_list.set_ColorWriteEnable();
            dsgraph.cmd_list.set_Z(false);
            g_pGamePersistent->Environment().RenderSky();
            dsgraph.cmd_list.set_Z(true);
        }

        // level
        Target->phase_scene_begin();
        {
            // Skip 3D HUD only for legacy alternating SVP (no dedicated RT). Dedicated second pass draws HUD here + hud_ui below.
            // HUD overlay scope (g_3d_scopes 3): HUD goes to the offscreen overlay instead of the world pass.
            const bool skip_world_hud = m_SecondViewportPass || m_HudOverlayActive ||
                (!ps_r__dedicated_second_vp && Device.m_SecondViewport.IsSVPFrame());
            if (!skip_world_hud)
                dsgraph.render_hud();
        }
        dsgraph.render_lods(true, true);
        if (Details && !(svp_pass && ps_r__svp_skip_details))
            Details->Render(dsgraph.cmd_list);
        Target->phase_scene_end();
    }

    // Main pass: wallmarks + hud_ui; dedicated second pass: hud_ui only.
    if (g_pGameLevel->pHUD && g_pGameLevel->pHUD->RenderActiveItemUIQuery())
    {
        ZoneScopedN("Render/HUD_UI");
        if (!m_SecondViewportPass)
        {
            Target->phase_wallmarks();
            dsgraph.render_hud_ui();
        }
        else if (ps_r__dedicated_second_vp)
        {
            dsgraph.render_hud_ui();
            // P2.3: flush the deferred segment before lighting reads the accumulator/albedo.
            SubmitSVPDeferred(dsgraph);
        }
    }

    // Wall marks
    if (Wallmarks && !(svp_pass && ps_r__svp_skip_wallmarks))
    {
        ZoneScopedN("Render/Wallmarks");
        PIX_EVENT(DEFER_WALLMARKS);
        Target->phase_wallmarks();
        g_r = 0;
        Wallmarks->Render(); // wallmarks has priority as normal geometry
    }

    // Update incremental shadowmap-visibility solver
    {
        ZoneScopedN("Render/Occlusion/FlushLastFrame");
        PIX_EVENT(DEFER_FLUSH_OCCLUSION);
        u32 it = 0;
        for (it = 0; it < Lights_LastFrame.size(); it++)
        {
            if (0 == Lights_LastFrame[it])
                continue;
            try
            {
                for (int id = 0; id < 3; ++id)
                    Lights_LastFrame[it]->svis[id].flushoccq();
            }
            catch (...)
            {
                Msg("! Failed to flush-OCCq on light [%d] %X", it, *(u32*)(&Lights_LastFrame[it]));
            }
        }
        Lights_LastFrame.clear();
    }

    // full screen pass to mark msaa-edge pixels in highest stencil bit
    if (o.msaa)
    {
        ZoneScopedN("Render/MSAA/MarkEdges");
        PIX_EVENT(MARK_MSAA_EDGES);
        Target->mark_msaa_edges();
    }

    {
        ZoneScopedN("Render/RainSync");
        if (!(svp_pass && ps_r__svp_skip_rain_sync))
            r_rain.sync();
    }

    // Directional light - fucking sun
    {
        ZoneScopedN("Render/Sun");
        PIX_EVENT(DEFER_SUN);
        Stats.l_visible++;
        const bool svp_sun_off = ps_r__svp_skip_sun_csm != 0;
#if defined(USE_DX11)
        // Capture BEFORE sync(): it deactivates the phase; the slices are final right after.
        const bool sun_was_active = !RImplementation.o.oldshadowcascades && r_sun.o.active;
#endif
        // Stage 2 (sun-reuse): the scope pass reuses the MAIN pass sun cascades from the
        // transferred atlas slices; when unavailable (no parallel/transfer this frame, or the
        // slices were not copied yet) it falls back to the legacy per-camera rebuild below.
        const bool sun_reuse = svp_pass && svp_sun_reuse_active() && svp_sun_slices_ready;
        if (!(svp_pass && svp_sun_off) && !sun_reuse)
        {
            if (!RImplementation.o.oldshadowcascades)
                r_sun.sync();
            else
                r_sun_old.sync();
        }
#if defined(USE_DX11)
        // DX11-only: the reuse loop swaps per-slice SRVs via CTexture::set_slice, which the GL
        // backend does not implement (and the reuse path can never activate there anyway -
        // BeginSecondVPCalculateParallel refuses the parallel mode on GL).
        if (sun_reuse)
        {
            // The cascades were rendered by the MAIN pass this frame and copied into the
            // dedicated SVP atlas (see the copy below); accumulate them against the scope view
            // with the MAIN pass cascade transforms - the lens frustum is a subset of the main
            // one, so the shadows are correct, only the texel density in the lens is lower.
            // Mirrors render_sun::accumulate_cascade with the slice index offset into the
            // atlas tail; recorded into the scope deferred list, executed together with this
            // segment's submit (same pattern as accum_direct_blend).
            if (Target->svp_publish_smap_atlas(true))
            {
                const u32 sun_base = u32(ps_r__svp_smap_pages);
                for (u32 i = 0; i < R__NUM_SUN_CASCADES; ++i)
                {
                    Target->rt_smap_depth->pTexture->set_slice(int(sun_base + i));
                    if ((i == SE_SUN_NEAR) && Target->use_minmax_sm_this_frame())
                        Target->create_minmax_SM(dsgraph.cmd_list);
                    Fmatrix& xform = r_sun.m_sun_cascades[i].xform;
                    Fmatrix& xform_prev = r_sun.m_sun_cascades[i ? i - 1 : i].xform;
                    const u32 sub_phase = (i == 0) ? SE_SUN_NEAR
                        : ((i < R__NUM_SUN_CASCADES - 1) ? SE_SUN_MIDDLE : SE_SUN_FAR);
                    Target->accum_direct_cascade(dsgraph.cmd_list, sub_phase, xform, xform_prev,
                        r_sun.m_sun_cascades[i].bias);
                }
                Target->svp_publish_smap_atlas(false);
            }
        }
#endif // USE_DX11
        if (sun_reuse || !(svp_pass && svp_sun_off))
            Target->accum_direct_blend(dsgraph.cmd_list);
#if defined(USE_DX11)
        // Stage 2 sun-reuse, MAIN pass side: copy the finished sun cascade slices into the
        // dedicated SVP atlas RIGHT HERE - after r_sun.sync() (the slices are final) and BEFORE
        // render_lights, whose first spot page clear destroys slice 0 (the NEAR cascade;
        // MIDDLE/FAR would survive in slices 1/2, but all three are copied for one uniform window).
        if (!svp_pass && svp_sun_reuse_active() && sun_was_active &&
            Target->svp_rt_smap_depth && Target->svp_rt_smap_depth->valid() &&
            Target->svp_rt_smap_depth->pSurface && Target->rt_smap_depth &&
            Target->rt_smap_depth->pSurface)
        {
            const UINT sun_base = UINT(ps_r__svp_smap_pages);
            if (Target->svp_rt_smap_depth->n_slices >= sun_base + R__NUM_SUN_CASCADES)
            {
                ID3D11Texture2D* dst_tex = static_cast<ID3D11Texture2D*>(Target->svp_rt_smap_depth->pSurface);
                ID3D11Texture2D* src_tex = static_cast<ID3D11Texture2D*>(Target->rt_smap_depth->pSurface);
                for (UINT i = 0; i < R__NUM_SUN_CASCADES; ++i)
                    HW.get_context(CHW::IMM_CTX_ID)->CopySubresourceRegion(
                        dst_tex, D3D11CalcSubresource(0, sun_base + i, 1), 0, 0, 0,
                        src_tex, D3D11CalcSubresource(0, i, 1), nullptr);
                svp_sun_slices_ready = true;
            }
        }
#endif
    }

    {
        ZoneScopedN("Render/SelfIllum");
        PIX_EVENT(DEFER_SELF_ILLUM);
        Target->phase_accumulator(dsgraph.cmd_list);
        // Render emissive geometry, stencil - write 0x0 at pixel pos
        dsgraph.cmd_list.set_xform_project(Device.mProject);
        dsgraph.cmd_list.set_xform_view(Device.mView);
        // Stencil - write 0x1 at pixel pos -
        if (!o.msaa)
        {
            dsgraph.cmd_list.set_Stencil(TRUE, D3DCMP_ALWAYS, 0x01, 0xff, 0xff,
                D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP);
        }
        else
        {
            dsgraph.cmd_list.set_Stencil(TRUE, D3DCMP_ALWAYS, 0x01, 0xff, 0x7f,
                D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP);
        }
        dsgraph.cmd_list.set_CullMode(CULL_CCW);
        dsgraph.cmd_list.set_ColorWriteEnable();
        dsgraph.render_emissive();
        // P2.3: flush the deferred segment (sun blend + emissive) before the combine reads the
        // accumulator. Everything recorded into dsgraph.cmd_list so far is now executed.
        SubmitSVPDeferred(dsgraph);
    }

    // Вариант A: for the scope pass, filter the light package by the NARROW scope frustum -
    // lights whose volume sphere (position + range, covering the shadow extent) misses the lens
    // view skip shadow-map building and accumulation entirely. The main package is untouched.
    // Only when the parallel path built the frustum snapshot (svp_parallel).
    //
    // Frame driver (stage 1c): when the worker is going to pre-build shadows, the filtering
    // happened EARLIER - on the main pass, into the LP_svp_* members, so the builder consumed
    // exactly what the scope pass accumulates below (svp_use_prebuilt branch).
    light_Package svp_lp_normal, svp_lp_pending;
    const bool svp_filter_lights = svp_pass && svp_parallel;
    const bool svp_use_prebuilt = svp_pass && svp_shadow_stage != 0;

    // Frame driver: pre-filter into the LP_svp_* members BEFORE the main pass consumes its own
    // packages (render_lights drains them) - this is a snapshot of light pointers that the
    // worker (after the signal) and the scope pass both consume.
    if (!svp_pass && svp_parallel && svp_frame_driver)
    {
        filter_light_package_for_svp(LP_normal, LP_svp_normal);
        filter_light_package_for_svp(LP_pending, LP_svp_pending);
    }

    if (svp_use_prebuilt)
    {
        // LP_svp_normal/LP_svp_pending were filled above on the main pass - do not re-filter.
    }
    else if (svp_filter_lights)
    {
        filter_light_package_for_svp(LP_normal, svp_lp_normal);
        filter_light_package_for_svp(LP_pending, svp_lp_pending);
    }

    // Lighting, non dependant on OCCQ
    {
        ZoneScopedN("Render/LightsNoOccq");
        PIX_EVENT(DEFER_LIGHT_NO_OCCQ);
        // Stage 1b/1c: the scope package skips per-light occq visibility entirely - it was
        // prefiltered by the narrow lens frustum and (prebuilt mode) its shadows are already
        // built by the worker.
        const bool no_vis = svp_use_prebuilt || svp_filter_lights;
        render_lights(svp_use_prebuilt ? LP_svp_normal : (svp_filter_lights ? svp_lp_normal : LP_normal), no_vis);
    }

    // Lighting, dependant on OCCQ
    {
        ZoneScopedN("Render/LightsOccq");
        PIX_EVENT(DEFER_LIGHT_OCCQ);
        // Same as above: pending lights are treated as visible for the scope pass - they passed
        // the frustum test, and their occq results belong to the MAIN pass's decision loop.
        const bool no_vis = svp_use_prebuilt || svp_filter_lights;
        render_lights(svp_use_prebuilt ? LP_svp_pending : (svp_filter_lights ? svp_lp_pending : LP_pending), no_vis);
    }

    // Release the MAIN pass's sealed smap lists BEFORE unblocking the SVP worker: the worker
    // pushes its own page lists into the same vector, and releasing after the signal would
    // destroy the worker's fresh lists (use-after-free at the scope-pass replay).
    ReleaseSVPReplayLists();

    // Frame driver stage C kickoff: the main pass has drained its own lighting - from here the
    // context pool and the shared light objects belong to the worker (the pool has no internal
    // locking, which is exactly why the worker waits for this signal before allocating).
    // The LP_svp_* member packages were snapshotted BEFORE the main lighting drained them.
    if (!svp_pass && svp_parallel && svp_frame_driver)
    {
        svp_lights_go.Set();
    }

    // P2.3: execute all deferred-recorded lighting before the combine reads the
    // G-buffer/accumulator.
    SubmitSVPDeferred(dsgraph);

    // BUG-1/BUG-3: phase_combine/render_forward drain second-order geometry from
    // get_imm_context() only. The scope pass's water (mapDistort) and particles
    // (mapNormalPasses[1], mapSorted) are in ITS dsgraph. Copy them into the (now
    // empty) imm context so the combine's existing drain paths handle them through
    // RCache (correct render state). After phase_combine, render_distort/render_sorted
    // clean the imm lists; the tail VERIFY(dsgraph.mapDistort.empty()) passes because
    // we clear the scope's lists after copying.
    // NOTE: std::swap is NOT safe here — xr_fixed_map has an internal node pool tied
    // to its owning dsgraph; swapping moves the pool pointer, and release_context
    // frees the pool while the other dsgraph still references it (use-after-free).
    // A parameter approach (passing dsgraph to phase_combine) caused DEVICE_HUNG because
    // render_graph records into the dsgraph's deferred cmd_list while the render targets
    // are set on RCache (immediate context) — incompatible GPU state.
    if (svp_pass)
    {
        auto& imm = get_imm_context();
        // mapDistort (water)
        for (auto* cur = dsgraph.mapDistort.begin(); cur != dsgraph.mapDistort.end(); ++cur)
            imm.mapDistort.insert(cur->first, cur->second);
        dsgraph.mapDistort.clear();
        // mapSorted (sorted particles)
        for (auto* cur = dsgraph.mapSorted.begin(); cur != dsgraph.mapSorted.end(); ++cur)
            imm.mapSorted.insert(cur->first, cur->second);
        dsgraph.mapSorted.clear();
        // mapNormalPasses[1] and mapMatrixPasses[1] (priority-1 forward geometry)
        for (int j = 0; j < SHADER_PASSES_MAX; ++j)
        {
            for (auto* cur = dsgraph.mapNormalPasses[1][j].begin(); cur != dsgraph.mapNormalPasses[1][j].end(); ++cur)
                imm.mapNormalPasses[1][j].insert(cur->first, cur->second);
            dsgraph.mapNormalPasses[1][j].clear();
            for (auto* cur = dsgraph.mapMatrixPasses[1][j].begin(); cur != dsgraph.mapMatrixPasses[1][j].end(); ++cur)
                imm.mapMatrixPasses[1][j].insert(cur->first, cur->second);
            dsgraph.mapMatrixPasses[1][j].clear();
        }
    }

    // Postprocess
    {
        ZoneScopedN("Render/Combine");
        PIX_EVENT(DEFER_LIGHT_COMBINE);
        Target->phase_combine();
    }

    VERIFY(dsgraph.mapDistort.empty());
}

void CRender::BindBackbufferForUI()
{
    Target->u_setrt(RCache, Device.dwWidth, Device.dwHeight, Target->get_base_rt(), 0, 0, Target->get_base_zb());
    // Raw u_setrt never pushes a GPU viewport: after a scaled SVP pass it may still be sw×sh,
    // which would rasterize the UI into the top-left corner of the backbuffer.
    RCache.SetViewport({ 0.f, 0.f, float(Device.dwWidth), float(Device.dwHeight), 0.f, 1.f });
}

void CRender::RenderSecondViewport()
{
    // Only invoked when ps_r__dedicated_second_vp (see IGame_Level).
    const float sc = clampr(ps_r__second_vp_render_scale, 0.05f, 1.f);
    const u32 sw = _max(1u, (u32)iFloor(float(Device.dwWidth) * sc + 0.5f));
    const u32 sh = _max(1u, (u32)iFloor(float(Device.dwHeight) * sc + 0.5f));
    Target->ResizeSecondVPRT(sw, sh);

    // Render this pass into a parallel sw×sh RT set ($user$sv_*) at ANY scale - including 1.0:
    // without the twins the scope pass renders into the MAIN G-buffer surfaces, whose
    // depth/stencil/accumulator state was already consumed by the main pass, and the lens
    // output collapses to gray. At scale 1.0 the twins are simply full-size (extra VRAM, but
    // correct); silently falls back to the main-surface chain if the target set cannot be created.
    const bool scaled_pipeline = Target->SVPTargetsEnsure(sw, sh);
    if (scaled_pipeline)
        Target->SVPPipelineBegin();

    Fvector4 saved_svp_capture{};
    if (g_pGamePersistent && g_pGamePersistent->m_pGShaderConstants)
    {
        saved_svp_capture = g_pGamePersistent->m_pGShaderConstants->m_svp_rt_capture;
        g_pGamePersistent->m_pGShaderConstants->m_svp_rt_capture.set(1.f, 0.f, 0.f, 0.f);
    }

    // Render scope RT without gameplay PP (NV/psy/etc.) to avoid doubled PP inside scope lens.
    SPPInfo neutral_pp = pp_identity;
    neutral_pp.cm_influence = 0.f;
    neutral_pp.cm_interpolate = 1.f;
    neutral_pp.cm_tex1 = "";
    neutral_pp.cm_tex2 = "";
    SetPostProcessParams(neutral_pp);

    m_SecondViewportPass = true;
    m_SecondViewportOutputToRT = true;
    Render();
    m_SecondViewportOutputToRT = false;
    m_SecondViewportPass = false;

    // Stage B: release this frame's dedicated SVP dsgraph context - its visibility maps have been
    // drained by the Render() above. Next SVP frame allocates a fresh one.
    if (svp_context_id != R_dsgraph_structure::INVALID_CONTEXT_ID)
    {
        release_context(svp_context_id);
        svp_context_id = R_dsgraph_structure::INVALID_CONTEXT_ID;
    }
    svp_dsgraph = nullptr;
    r_main_dsgraph_override = nullptr;

    // Restore before returning: bullet tracers are drawn right after this call into rt_secondVP,
    // which stays bound on purpose (SVPPipelineEnd does not touch live GPU state).
    if (scaled_pipeline)
        Target->SVPPipelineEnd();

    if (g_pGamePersistent && g_pGamePersistent->m_pGShaderConstants)
        g_pGamePersistent->m_pGShaderConstants->m_svp_rt_capture = saved_svp_capture;

    // Restore main-view postprocess parameters after second viewport render.
    if (g_pGameLevel)
        g_pGameLevel->Cameras().ApplyDevice();
}

void CRender::render_forward()
{
    ZoneScoped;
    auto& dsgraph = get_imm_context();

    //******* Main render - second order geometry (the one, that doesn't support deffering)
    //.todo: should be done inside "combine" with estimation of of luminance, tone-mapping, etc.
    {
        //	Igor: we don't want to render old lods on next frame.
        dsgraph.mapLOD.clear();
        dsgraph.render_graph(1); // normal level, secondary priority
        dsgraph.PortalTraverser.fade_render(); // faded-portals
        dsgraph.render_sorted(); // strict-sorted geoms
        g_pGamePersistent->Environment().RenderLast(); // rain/thunder-bolts
    }
}

// Перед началом рендера мира --#SM+#--
void CRender::BeforeWorldRender() {}

// Копия бэкбуфера (текущего экрана) в рендер-таргет второго вьюпорта.
// Общий код для legacy SVP и HUD-overlay прицела (g_3d_scopes 3).
void CRender::CopyBackbufferToSecondVPRT()
{
    if (Target->rt_secondVP && (Target->rt_secondVP->dwWidth != Device.dwWidth || Target->rt_secondVP->dwHeight != Device.dwHeight))
        Target->ResizeSecondVPRT(Device.dwWidth, Device.dwHeight);
#ifdef USE_DX9
    IDirect3DSurface9* pBuffer = nullptr;
    HW.pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBuffer, nullptr);
    D3DXLoadSurfaceFromSurface(Target->rt_secondVP->pRT, nullptr, nullptr, pBuffer, nullptr, nullptr, D3DX_DEFAULT, 0);
    pBuffer->Release();
#endif
#ifdef USE_DX11
    // Back buffer lives on IDXGISwapChain; m_pSwapChain2 is optional (QueryInterface may fail).
    if (HW.m_pSwapChain && Target->rt_secondVP && Target->rt_secondVP->pSurface)
    {
        ID3DTexture2D* pBuffer = nullptr;
        if (SUCCEEDED(HW.m_pSwapChain->GetBuffer(0, __uuidof(ID3DTexture2D), (LPVOID*)&pBuffer)) && pBuffer)
        {
            auto pContext = HW.get_context(CHW::IMM_CTX_ID);
            pContext->CopyResource(Target->rt_secondVP->pSurface, pBuffer);
            pBuffer->Release();
        }
    }
#endif
#ifdef USE_OGL
    // HUD overlay scope (g_3d_scopes 2): copy the clean zoomed world (post-combine, no HUD) into
    // rt_secondVP so the scope lens (model_scope_lense.ps sampling s_vp2) shows a live frame.
    // Without this the lens never updates (stale first frame) — GL was missing this branch while
    // DX9/DX11 had it. GL 4.1 has no glCopyImageSubData (needs 4.3), so we blit via 2 FBOs — the
    // source is the engine's base color RT (Target->get_base_rt() = rt_Base[CurrentBackBuffer],
    // already holds the final world image after phase_combine), the dest is rt_secondVP.
    if (Target->rt_secondVP && Target->rt_secondVP->pRT)
    {
        const GLuint srcRT = Target->get_base_rt();
        const GLuint dstRT = Target->rt_secondVP->pRT;
        if (srcRT && dstRT)
        {
            static GLuint s_readFBO = 0, s_drawFBO = 0;
            if (!s_readFBO) CHK_GL(glGenFramebuffers(1, &s_readFBO));
            if (!s_drawFBO) CHK_GL(glGenFramebuffers(1, &s_drawFBO));

            GLint prevRead = 0, prevDraw = 0;
            CHK_GL(glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead));
            CHK_GL(glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw));

            CHK_GL(glBindFramebuffer(GL_READ_FRAMEBUFFER, s_readFBO));
            CHK_GL(glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, srcRT, 0));
            CHK_GL(glReadBuffer(GL_COLOR_ATTACHMENT0));

            CHK_GL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_drawFBO));
            CHK_GL(glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dstRT, 0));
            CHK_GL(glDrawBuffer(GL_COLOR_ATTACHMENT0));

            // rt_secondVP was resized above to match Device.dwWidth/Height, so blit is 1:1 (GL_NEAREST).
            // Y-flip the source region. rt_secondVP must end up in the SAME orientation as the PiP path
            // (g_3d_scopes 1) leaves it, because model_scope_lense.ps samples s_vp2 the same way for both
            // modes (no per-mode V-flip in the lens shader — it mirrors DX r3/r4 which never flip).
            //   - PiP path:   rt_secondVP is filled by phase_pp through the GL-reordered postprocess VB,
            //                 which stores the frame screen-aligned for gl_FragCoord sampling.
            //   - Overlay:    get_base_rt() is written by phase_combine through a DIFFERENT VB layout, so
            //                 its rows run the opposite way. An identity copy would hand the lens an
            //                 upside-down frame (observed); flipping the source Y here lands the world
            //                 upright in the lens, matching the PiP orientation.
            CHK_GL(glBlitFramebuffer(
                0, Device.dwHeight, Device.dwWidth, 0,
                0, 0, Target->rt_secondVP->dwWidth, Target->rt_secondVP->dwHeight,
                GL_COLOR_BUFFER_BIT, GL_NEAREST));

            // Restore the FBO bindings the rest of the frame expects (the engine keeps HW.pFB bound
            // as both read/draw outside of blit helpers — see R_Backend_Runtime.h set_FB).
            CHK_GL(glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead));
            CHK_GL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDraw));
        }
    }
#endif
}

// После рендера мира и пост-эффектов --#SM+#-- +SecondVP+
void CRender::AfterWorldRender()
{
    // HUD overlay scope (g_3d_scopes 3): clean zoomed frame (no HUD) for the scope lens.
    if (m_HudOverlayActive)
        CopyBackbufferToSecondVPRT();

    if (ps_r__dedicated_second_vp)
        return;
    if (Device.m_SecondViewport.IsSVPFrame())
        CopyBackbufferToSecondVPRT();
}
} // namespace xray::render::RENDER_NAMESPACE
