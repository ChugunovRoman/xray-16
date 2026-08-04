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
    auto& dsgraph = get_imm_context();

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
                L->vis_prepare(dsgraph.cmd_list);
                if (L->vis.pending)
                    LP_pending.v_point.push_back(L);
                else
                    LP_normal.v_point.push_back(L);
            }
            if (it < LP.v_spot.size())
            {
                light* L = LP.v_spot[it];
                L->vis_prepare(dsgraph.cmd_list);
                if (L->vis.pending)
                    LP_pending.v_spot.push_back(L);
                else
                    LP_normal.v_spot.push_back(L);
            }
            if (it < LP.v_shadowed.size())
            {
                light* L = LP.v_shadowed[it];
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
        if (!(svp_pass && ps_r__svp_skip_sun_csm))
        {
            if (!RImplementation.o.oldshadowcascades)
                r_sun.sync();
            else
                r_sun_old.sync();
        }
        Target->accum_direct_blend(dsgraph.cmd_list);
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
    }

    // Lighting, non dependant on OCCQ
    {
        ZoneScopedN("Render/LightsNoOccq");
        PIX_EVENT(DEFER_LIGHT_NO_OCCQ);
        render_lights(LP_normal);
    }

    // Lighting, dependant on OCCQ
    {
        ZoneScopedN("Render/LightsOccq");
        PIX_EVENT(DEFER_LIGHT_OCCQ);
        render_lights(LP_pending);
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
}

void CRender::RenderSecondViewport()
{
    // Only invoked when ps_r__dedicated_second_vp (see IGame_Level).
    {
        const float sc = clampr(ps_r__second_vp_render_scale, 0.05f, 1.f);
        const u32 sw = _max(1u, (u32)iFloor(float(Device.dwWidth) * sc + 0.5f));
        const u32 sh = _max(1u, (u32)iFloor(float(Device.dwHeight) * sc + 0.5f));
        Target->ResizeSecondVPRT(sw, sh);
    }

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
            // Y-flip the source region: the lens shader (model_scope_lense.ps) samples s_vp2 with
            // gl_FragCoord.xy/screen_res.xy — on GL gl_FragCoord has lower-left origin, while the DX
            // path it was written for (CopyResource from the D3D backbuffer) yields top-left-origin
            // texture content. Without flipping the blit the lens shows the world upside-down.
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
