#include "stdafx.h"

// HUD overlay scope (g_3d_scopes 3): renders the live player HUD offscreen into $user$hud_overlay.
//
// Frame flow (while active):
//   1. World pass skips the HUD (skip_world_hud in r2_R_render.cpp) -> clean zoomed backbuffer.
//   2. AfterWorldRender copies the clean backbuffer into rt_secondVP (scope lens source).
//   3. This pass drains the HUD queued this frame (mapHUD -> mini G-buffer -> resolve with
//      transparent background, then mapHUDSorted/mapHUDEmissive forward passes for the scope lens).
//   4. UIGameCustom composites $user$hud_overlay on top in the UI layer.
//
// The scope lens inside the overlay samples rt_secondVP 1:1 (same zoom everywhere), so the lens
// shows the live clean frame without the HUD and without recursion.

#include "Layers/xrRender_R2/r2.h"
#include "Layers/xrRender/SH_RT.h"
#include "Layers/xrRender/Shader.h"
#include "Layers/xrRender/ResourceManager.h" // _CreateTexture (bind-diag)
#include "Layers/xrRender/xrRender_console.h" // ps_r2_sun_depth_near_scale/bias (NEAR cascade shadow xform)
#include "xrEngine/device.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Render.h"
#include "xrCore/FS.h"

extern ENGINE_API float g_fov; // unzoomed camera fov (console "fov")

#if defined(USE_DX11)
#include "Layers/xrRenderPC_R4/r4_rendertarget.h"
#include <DirectXTex.h>
#include "xrCore/_std_extensions.h"
#endif

namespace xray::render::RENDER_NAMESPACE
{
namespace hud_overlay
{
constexpr pcstr RT_NAME = "$user$hud_overlay";
constexpr pcstr WORK_NAME = "$user$__hud_ovl_work"; // private render target (GL Y-flip source)
constexpr pcstr GBUF_P_NAME = "$user$__hud_ovl_P";
constexpr pcstr GBUF_N_NAME = "$user$__hud_ovl_N";
constexpr pcstr GBUF_DEFER_NAME = "$user$__hud_ovl_defer_albedo";
constexpr pcstr GBUF_Z_NAME = "$user$__hud_ovl_Z";

// Persistent refs: $user$ textures are looked up by name; releasing the CRT leaves UI sampling an
// empty texture (same lifetime trick as s_persist_icon_ui_rt in r2_weapon_icon.cpp).
// Released from CRender::destroy() — static dtors would run after the resource manager is dead.
//
// s_rt_overlay — public RT carrying the $user$hud_overlay name.
//   GL: resolve + forward passes render DIRECTLY into s_rt_overlay (no work/flip pair). The native
//       composite (CompositeHudOverlay) samples this FBO texture in its native bottom-up orientation
//       — no Y-flip compensation, no inherited UI-stencil. This is the fix for the GL upside-down /
//       transparent-overlay bug.
//   DX11: resolve + forward passes write into s_rt_work; FlipOverlayV publishes it to s_rt_overlay
//       for the UI-layer composite (UIGameCustom), which still handles this backend.
// s_rt_work  — DX11-only private work RT (see above); unused on GL.
ref_rt s_rt_overlay;
ref_rt s_rt_work;
ref_rt s_rt_p;
ref_rt s_rt_n;
ref_rt s_rt_defer;
ref_rt s_rt_z;
ref_shader s_resolve_shader;
u32 s_gbuf_w = 0;
u32 s_gbuf_h = 0;

void ReleaseAll()
{
    s_rt_overlay = nullptr;
    s_rt_work = nullptr;
    s_rt_p = nullptr;
    s_rt_n = nullptr;
    s_rt_defer = nullptr;
    s_rt_z = nullptr;
    s_resolve_shader = nullptr;
    s_gbuf_w = 0;
    s_gbuf_h = 0;
}

void EnsureTargets(CRenderTarget* Tgt, const u32 w, const u32 h)
{
    // On GL the resolve + forward passes render DIRECTLY into the public s_rt_overlay (no work/flip
    // pair) — the native composite (CompositeHudOverlay) samples the overlay in FBO-native orientation,
    // so the intermediate work RT and the Y-flip blit are not needed. DX11 still uses the work/overlay
    // split because its UI-layer composite (UIGameCustom) samples $user$hud_overlay with DX conventions.
#if defined(USE_DX11)
    if (s_gbuf_w == w && s_gbuf_h == h && s_rt_p && s_rt_defer && s_rt_z && s_rt_overlay && s_rt_work)
        return;
#else
    if (s_gbuf_w == w && s_gbuf_h == h && s_rt_p && s_rt_defer && s_rt_z && s_rt_overlay)
        return;
#endif

    s_rt_p = nullptr;
    s_rt_n = nullptr;
    s_rt_defer = nullptr;
    s_rt_z = nullptr;
    s_rt_overlay = nullptr;
    s_rt_work = nullptr;
    s_gbuf_w = w;
    s_gbuf_h = h;

    VERIFY(Tgt && Tgt->rt_Position && Tgt->rt_Color);
    s_rt_p.create(GBUF_P_NAME, w, h, Tgt->rt_Position->fmt, 1);
    if (!RImplementation.o.gbuffer_opt)
    {
        VERIFY(Tgt->rt_Normal);
        s_rt_n.create(GBUF_N_NAME, w, h, Tgt->rt_Normal->fmt, 1);
    }
    s_rt_defer.create(GBUF_DEFER_NAME, w, h, Tgt->rt_Color->fmt, 1);
    s_rt_z.create(GBUF_Z_NAME, w, h, HW.Caps.fDepth, 1, { CRT::CreateSurface });
#if defined(USE_DX11)
    s_rt_work.create(WORK_NAME, w, h, D3DFMT_A8R8G8B8, 1);
#endif
    s_rt_overlay.create(RT_NAME, w, h, D3DFMT_A8R8G8B8, 1);

    VERIFY(s_rt_p && s_rt_defer && s_rt_z && s_rt_overlay);
#if defined(USE_DX11)
    VERIFY(s_rt_work);
#endif
    if (!RImplementation.o.gbuffer_opt)
        VERIFY(s_rt_n);
}

void EnsureResolveShader()
{
    if (!s_resolve_shader)
        s_resolve_shader.create("hud_overlay_resolve", GBUF_DEFER_NAME);
}

#if defined(USE_DX11)
// DX: render targets are top-left origin already; the "work -> overlay" blit is a 1:1 identity copy.
// GL does not use this — on GL the resolve + forward passes render directly into s_rt_overlay, and
// CompositeHudOverlay samples it in FBO-native orientation (no work/flip pair, see EnsureTargets).
void FlipOverlayV(const ref_rt& src, const ref_rt& dst)
{
    if (!src._get() || !dst._get() || !src->pSurface || !dst->pSurface)
        return;
    auto pContext = HW.get_context(CHW::IMM_CTX_ID);
    pContext->CopyResource(dst->pSurface, src->pSurface);
}
#endif
} // namespace hud_overlay

void CRender::ReleaseHudOverlayRT()
{
    hud_overlay::ReleaseAll();
}

// HUD overlay scope (g_3d_scopes 3): clears the overlay depth buffer to far between the two
// filtered mapHUDSorted passes. The scope lens (bScopeLens) is otherwise Z-rejected by the scope
// body, so it is drained in a second pass after this reset. Lives here next to s_rt_z so the dsgraph
// code (which calls render_hud_blends) does not need to know about the overlay depth RT.
void ClearHudOverlayDepth(CBackend& cmd_list)
{
    if (hud_overlay::s_rt_z)
        cmd_list.ClearZB(hud_overlay::s_rt_z, 1.0f, 0);
}

#if defined(USE_DX11)
// Debug (r__hud_overlay_debug 5): dump overlay RTs to $screenshots$ (same approach as r2_weapon_icon.cpp).
namespace hud_overlay
{
void DumpRT(pcstr label, const ref_rt& rt)
{
    if (!label || !rt._get() || !rt->pRT)
        return;

    ID3DResource* pSrc{};
    rt->pRT->GetResource(&pSrc);
    if (!pSrc)
    {
        Msg("! [hud_overlay] DDS [%s]: GetResource failed", label);
        return;
    }

    DirectX::ScratchImage image;
    const HRESULT cap = CaptureTexture(HW.pDevice, HW.get_context(CHW::IMM_CTX_ID), pSrc, image);
    if (FAILED(cap))
    {
        Msg("! [hud_overlay] DDS [%s]: CaptureTexture failed hr=0x%08x", label, (unsigned)cap);
        _RELEASE(pSrc);
        return;
    }

    DirectX::Blob saved;
    const HRESULT hr = SaveToDDSMemory(*image.GetImage(0, 0, 0), DirectX::DDS_FLAGS_FORCE_DX9_LEGACY, saved);
    _RELEASE(pSrc);
    if (FAILED(hr))
    {
        Msg("! [hud_overlay] DDS [%s]: SaveToDDSMemory failed hr=0x%08x", label, (unsigned)hr);
        return;
    }

    static u32 s_dump_seq{};
    string64 t_stemp{};
    string_path fname{};
    xr_sprintf(fname, "hud_ovl_%s_%s_%u.dds", label, timestamp(t_stemp), ++s_dump_seq);

    if (IWriter* fs = FS.w_open("$screenshots$", fname))
    {
        fs->w(saved.GetBufferPointer(), saved.GetBufferSize());
        FS.w_close(fs);
        Msg("~ [hud_overlay] DDS -> $screenshots$\\%s", fname);
    }
    else
        Msg("! [hud_overlay] DDS: cannot write $screenshots$\\%s", fname);
}
} // namespace hud_overlay
#endif

void CRender::RenderHudOverlayToTexture()
{
    static bool s_diag_pending = true; // one-shot diagnostics per activation (lens pipeline debug)
    static bool s_diag_steady = true; // one-shot diagnostics at steady aim (alpha >= 0.9)
    if (!m_HudOverlayActive)
    {
        s_diag_pending = true;
        s_diag_steady = true;
        return;
    }

#if defined(USE_DX11)
    // When the overlay is active, the world pass deliberately skips HUD draining (skip_world_hud in
    // r2_R_render.cpp + IsHudOverlayActive() guards on mapHUDSorted/mapHUDEmissive in r__dsgraph_render).
    // The drain is OUR job - done below. If we bail before the drain, the HUD maps would linger until
    // the next Calculate() rebuilds them. Normally harmless (rebuilt every frame anyway), but on a
    // persistent failure (e.g. missing shader) they'd accumulate and hold visuals/shaders alive, and
    // a one-frame bail can render a stale HUD. Flush them on any early exit to keep state consistent.
    auto& dg0 = get_imm_context();
    auto flush_hud_maps = [&dg0]()
    {
        dg0.mapHUD.clear();
        dg0.mapHUDSorted.clear();
        dg0.mapHUDEmissive.clear();
    };

    if (!Device.b_is_Ready || !Target)
    {
        flush_hud_maps();
        return;
    }
    if (!g_pGameLevel || !g_pGameLevel->pHUD)
    {
        flush_hud_maps();
        return;
    }

    using namespace hud_overlay;
    const u32 w = Device.dwWidth;
    const u32 h = Device.dwHeight;

    EnsureTargets(Target, w, h);
    if (!s_rt_p || !s_rt_defer || !s_rt_z || !s_rt_overlay || !s_rt_work)
    {
        flush_hud_maps();
        return;
    }
    EnsureResolveShader();
    if (!s_resolve_shader)
    {
        Msg("! [hud_overlay] missing shader hud_overlay_resolve (.s/.ps) under $game_shaders$");
        flush_hud_maps();
        return;
    }

    auto& dg = get_imm_context();
    const u32 phase_saved = dg.o.phase;

    if (!m_hudOvlCam.valid)
    {
        flush_hud_maps();
        return;
    }
    // Post-processing passes clobber the Device camera state — save it and override with the scene
    // camera captured at Render() start. The HUD drain must render exactly like the world HUD pass
    // would have (same view/proj/FOV), otherwise the HUD lands offscreen (empty overlay RTs).
    //
    // We override a LOT of global state here (Device camera, m_blender_mode.y/w, cascade slice on
    // rt_smap_depth, the SVP flag, render targets, Z/stencil/cull). All of it MUST be restored on exit,
    // including if an early VERIFY throws in debug or any call below raises - otherwise the rest of
    // the frame renders with corrupted state. Everything that needs restoring is captured BEFORE the
    // override into the variables below, and an RAII guard (state_guard) puts it all back in its
    // destructor. Declare the restore-state up front so the guard can capture them by reference.
    struct SDeviceCam
    {
        Fmatrix mView{}, mProject{}, mFullTransform{}, mInvView{}, mInvFullTransform{};
        Fvector vCameraPosition{}, vCameraDirection{}, vCameraTop{}, vCameraRight{};
        float fFOV{}, fASPECT{};
    } saved{};
    saved.mView = Device.mView;
    saved.mProject = Device.mProject;
    saved.mFullTransform = Device.mFullTransform;
    saved.mInvView = Device.mInvView;
    saved.mInvFullTransform = Device.mInvFullTransform;
    saved.vCameraPosition = Device.vCameraPosition;
    saved.vCameraDirection = Device.vCameraDirection;
    saved.vCameraTop = Device.vCameraTop;
    saved.vCameraRight = Device.vCameraRight;
    saved.fFOV = Device.fFOV;
    saved.fASPECT = Device.fASPECT;

    // Capture the remaining restore-state now (before the override touches anything) so the guard can
    // reference them. These mirror the save points that were previously scattered through the body.
    const float blender_y_saved = g_pGamePersistent ? g_pGamePersistent->m_pGShaderConstants->m_blender_mode.y : 0.f;
    const float blender_w_saved = g_pGamePersistent ? g_pGamePersistent->m_pGShaderConstants->m_blender_mode.w : 0.f;
    const bool wasSVPActive = Device.m_SecondViewport.IsSVPActive();
    bool shadow_slice_set = false; // true once we set_slice_read(SE_SUN_NEAR); the guard restores FAR

    // RAII guard: restores all overridden state on scope exit (normal return, early return, or throw).
    struct StateGuard
    {
        CRender& owner;
        CBackend& cmd_list;
        SDeviceCam& saved;
        float blender_y, blender_w;
        bool wasSVP;
        bool& shadow_slice_set; // by ref so the guard sees if the slice was switched
        u32 w, h;
        ~StateGuard()
        {
            // Restore SVP state (we forced it true for the lens pass).
            Device.m_SecondViewport.SetSVPActive(wasSVP);
            if (g_pGamePersistent && g_pGamePersistent->m_pGShaderConstants)
            {
                g_pGamePersistent->m_pGShaderConstants->m_blender_mode.y = blender_y;
                g_pGamePersistent->m_pGShaderConstants->m_blender_mode.w = blender_w;
            }
            // Restore cascade slice: the resolve used NEAR; leave FAR for the rest of the frame.
            if (shadow_slice_set && owner.Target->rt_smap_depth)
                owner.Target->rt_smap_depth->set_slice_read(SE_SUN_FAR);
            // Restore the main target and common states for the following UI rendering.
            cmd_list.set_Z(TRUE);
            cmd_list.set_CullMode(CULL_CCW);
            owner.Target->u_setrt(cmd_list, w, h, owner.Target->get_base_rt(), nullptr, nullptr, owner.Target->get_base_zb());
            owner.rmNormal(cmd_list);
            // Restore the (post-processing) Device camera state saved above.
            Device.mView = saved.mView;
            Device.mProject = saved.mProject;
            Device.mFullTransform = saved.mFullTransform;
            Device.mInvView = saved.mInvView;
            Device.mInvFullTransform = saved.mInvFullTransform;
            Device.vCameraPosition = saved.vCameraPosition;
            Device.vCameraDirection = saved.vCameraDirection;
            Device.vCameraTop = saved.vCameraTop;
            Device.vCameraRight = saved.vCameraRight;
            Device.fFOV = saved.fFOV;
            Device.fASPECT = saved.fASPECT;
            GEnv.Render->SetCacheXform(Device.mView, Device.mProject);
        }
    } state_guard{ *this, RCache, saved, blender_y_saved, blender_w_saved, wasSVPActive, shadow_slice_set, w, h };

    Device.mView = m_hudOvlCam.mView;
    Device.mProject = m_hudOvlCam.mProject;
    Device.mFullTransform = m_hudOvlCam.mFullTransform;
    Device.mInvView = m_hudOvlCam.mInvView;
    Device.mInvFullTransform = m_hudOvlCam.mInvFullTransform;
    Device.vCameraPosition = m_hudOvlCam.vCameraPosition;
    Device.vCameraDirection = m_hudOvlCam.vCameraDirection;
    Device.vCameraTop = m_hudOvlCam.vCameraTop;
    Device.vCameraRight = m_hudOvlCam.vCameraRight;
    // FOV lerps from zoomed (matches world HUD depth — no jump) to fixed (weapon stays stable
    // during zoom changes). The lerp factor = m_HudOverlayAlpha (0 at rotation 0.5, 1 at 0.9).
    // After transition (alpha=1): FOV = g_fov, weapon depth is fixed, no perspective growth on zoom.
    const float fovLerp = m_HudOverlayAlpha; // 0 = zoomed_fov, 1 = g_fov
    Device.fFOV = m_hudOvlCam.fFOV + (g_fov - m_hudOvlCam.fFOV) * fovLerp;
    GEnv.Render->SetCacheXform(Device.mView, Device.mProject);
    RCache.set_xform_view(Device.mView);
    RCache.set_xform_project(Device.mProject);

    // m_blender_mode.y = debug mode, m_blender_mode.w = brightness multiplier.
    if (g_pGamePersistent)
    {
        g_pGamePersistent->m_pGShaderConstants->m_blender_mode.y = float(ps_r__hud_overlay_debug);
        g_pGamePersistent->m_pGShaderConstants->m_blender_mode.w = ps_r__hud_overlay_brightness;
    }

    if (s_diag_pending || (s_diag_steady && m_HudOverlayAlpha >= 0.9f))
    {
        const bool steady = m_HudOverlayAlpha >= 0.9f;
        if (steady)
            s_diag_steady = false;
        s_diag_pending = false;
        const float blender_z = g_pGamePersistent ? g_pGamePersistent->m_pGShaderConstants->m_blender_mode.z : -1.f;
        Msg("* [hud_overlay] diag%s: mapHUD=%u mapHUDSorted=%u mapHUDEmissive=%u blender_mode.z=%.2f overlay_alpha=%.3f",
            steady ? "(steady)" : "",
            (u32)dg.mapHUD.size(), (u32)dg.mapHUDSorted.size(), (u32)dg.mapHUDEmissive.size(), blender_z, m_HudOverlayAlpha);
        Msg("* [hud_overlay] diag: rt_secondVP=%s %ux%u fmt=%u",
            Target->rt_secondVP ? "ok" : "null",
            Target->rt_secondVP ? Target->rt_secondVP->dwWidth : 0,
            Target->rt_secondVP ? Target->rt_secondVP->dwHeight : 0,
            Target->rt_secondVP ? (u32)Target->rt_secondVP->fmt : 0);
#ifdef USE_DX11
        if (HW.m_pSwapChain)
        {
            ID3DTexture2D* bb = nullptr;
            if (SUCCEEDED(HW.m_pSwapChain->GetBuffer(0, __uuidof(ID3DTexture2D), (LPVOID*)&bb)) && bb)
            {
                D3D11_TEXTURE2D_DESC desc;
                bb->GetDesc(&desc);
                Msg("* [hud_overlay] diag: backbuffer fmt=%u %ux%u", (u32)desc.Format, desc.Width, desc.Height);
                bb->Release();
            }
        }
#endif
    }

    // 1) HUD-only mini G-buffer. The live HUD visual was queued into mapHUD during this frame and
    //    the world pass deliberately skipped draining it.
    if (!o.gbuffer_opt)
        Target->u_setrt(RCache, s_rt_p, s_rt_n, s_rt_defer, s_rt_z);
    else
        Target->u_setrt(RCache, s_rt_p, s_rt_defer, s_rt_z);

    // Mark HUD pixels in stencil so the resolve can keep a transparent background.
    RCache.set_Stencil(
        TRUE, D3DCMP_ALWAYS, 0x01, 0xff, 0x7f, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP);
    RCache.set_CullMode(CULL_CCW);
    RCache.set_ColorWriteEnable();

    const Fcolor clear_z(0.f, 0.f, 0.f, 0.f);
    RCache.ClearRT(s_rt_p, clear_z);
    if (!o.gbuffer_opt)
        RCache.ClearRT(s_rt_n, clear_z);
    RCache.ClearRT(s_rt_defer, clear_z);
    RCache.ClearZB(s_rt_z, 1.0f, 0);
    RImplementation.rmNormal(RCache);

    dg.o.phase = PHASE_NORMAL;
    dg.render_hud(); // drains mapHUD with proper HUD projections (custom FOV handled inside)
    dg.o.phase = phase_saved;

    // 1.5) Sun shadow for resolve: sample s_smap (NEAR cascade) like accum_sun does for the world.
    //      r_sun already rendered shadows during the world pass; rt_smap_depth is still bound, but the
    //      cascade slice rests on the last rendered (FAR). Switch to NEAR (close HUD weapon range),
    //      build m_shadow = TexelAdjust * sun.combine[NEAR] * mInvView (mirror of accum_direct), and
    //      restore the slice to FAR after the resolve so the rest of the frame is unaffected.
    //      s_position stores VIEW-space positions (see gbuffer_stage.h: "float3 P; //View space").
    //      m_shadow therefore must use the inverse of the SAME view matrix that rendered the overlay
    //      G-buffer (step 1) — that is m_hudOvlCam.mInvView (= Device.mInvView right now, since we
    //      overwrote Device above). 'saved' holds the post-processing camera (captured AFTER world
    //      render + post-effects), which is unrelated to the HUD G-buffer and would mis-project.
    bool shadow_ok = false;
    Fmatrix m_shadow; // declared here so it survives the if-block; set_c runs AFTER set_Shader (below)
    if (Target->rt_smap_depth && RImplementation.r_sun.sun)
    {
        light* sun = RImplementation.r_sun.sun;
        // Build the world->shadow-texgen matrix exactly like CRenderTarget::accum_direct does for
        // the NEAR sub-phase (r4_rendertarget_accum_direct.cpp:174-203).
        const float fRange = ps_r2_sun_depth_near_scale;
        const float fBias = -ps_r2_sun_depth_near_bias; // NEAR bias is inverted (see accum_direct)
        Fmatrix m_TexelAdjust =
        {
            0.5f, 0.0f, 0.0f, 0.0f,
            0.0f, -0.5f, 0.0f, 0.0f,
            0.0f, 0.0f, fRange, 0.0f,
            0.5f, 0.5f, fBias, 1.0f
        };
        Fmatrix xf_project;
        xf_project.mul(m_TexelAdjust, sun->X.D[SE_SUN_NEAR].combine);
        // view->world for the camera that rendered the overlay G-buffer (= Device.mInvView right now).
        m_shadow.mul(xf_project, m_hudOvlCam.mInvView);
        Target->rt_smap_depth->set_slice_read(SE_SUN_NEAR);
        shadow_slice_set = true; // tell the RAII guard to restore FAR on exit
        shadow_ok = true;
    }

    // HUD overlay resolve must darken under roofs/trees like the world does. combine applies extra
    // multipliers in C++ (r4_rendertarget_phase_combine.cpp:120-132) that the auto-bound
    // L_ambient/L_hemi_color/L_sun_color binders do NOT (they pass raw Environment.CurrentEnv values).
    // The shader applies them itself via hud_ovl_lumscale:
    //   .x = 2 * ps_r2_sun_lumscale_amb   (ambient)
    //   .y = 2 * ps_r2_sun_lumscale_hemi  (hemi/env)
    //   .z = ps_r2_sun_lumscale           (sun diffuse, applied to the light source in Light_DB.cpp)
    //   .w = SSAO present flag (1 if rt_ssao_temp is alive and written this frame, 0 otherwise) —
    //        lets the shader fall back to occ=1 instead of reading a NULL SRV (which yields 0 and
    //        would turn the HUD black when ps_r_ssao == 0). rt_ssao_temp is created exactly when
    //        ssao_blur_on || ssao_hdao is true (r2_rendertarget.cpp:582-621), so checking the flags
    //        is equivalent to probing the private RT member.
    const bool ssao_present = RImplementation.o.ssao_blur_on || RImplementation.o.ssao_hdao;
    Fvector4 hud_ovl_lumscale;
    hud_ovl_lumscale.set(
        2.0f * ps_r2_sun_lumscale_amb,
        2.0f * ps_r2_sun_lumscale_hemi,
        ps_r2_sun_lumscale,
        ssao_present ? 1.0f : 0.0f);

    // 2) Resolve albedo into the overlay RT (transparent background via stencil mask).
    //    On GL we render DIRECTLY into s_rt_overlay (the public $user$hud_overlay) — no work/flip pair:
    //    CompositeHudOverlay samples this FBO texture in its native orientation, so there is no Y-flip
    //    step. On DX11 the resolve still targets s_rt_work; FlipOverlayV publishes it to s_rt_overlay
    //    at the end (the DX UI-layer composite samples with DX conventions).
    Target->u_setrt(RCache, s_rt_work, nullptr, nullptr, s_rt_z->pZRT[RCache.context_id]);
    RImplementation.rmNormal(RCache);
    const Fcolor clear_ui(0.f, 0.f, 0.f, 0.f);
    RCache.ClearRT(s_rt_work, clear_ui);

    RCache.set_CullMode(CULL_NONE);
    RCache.set_Z(FALSE);
    RCache.set_Stencil(FALSE);
    RCache.set_ColorWriteEnable(); // RGBA all channels

    u32 Offset;
    const u32 C = color_rgba(255, 255, 255, 255);
    const float _wf = float(w);
    const float _hf = float(h);
    Fvector2 p0, p1;
    p0.set(.5f / _wf, .5f / _hf);
    p1.set((_wf + .5f) / _wf, (_hf + .5f) / _hf);
    const float d_Z = EPS_S;
    const float d_W = 1.f;

    // Fullscreen quad sampling the HUD G-buffer (s_position/s_normal/s_base) into the overlay RT.
    // UV order matches the canonical X-Ray fullscreen quad (BL, TL, BR, TR with top-screen vertex
    // -> V~=0, bottom -> V~=1) — identical to RenderMenu (r2_R_render.cpp), PreviewSceneRenderer and
    // r2_weapon_icon. Both this resolve quad and the UI-layer composite quad (UIGameCustom CUIStatic)
    // run through stub_notransform_t.vs, which inverts NDC-Y (* -1); using the standard UV order on
    // both sides keeps the Y-flip symmetric and the overlay lands upright on screen. The previous
    // swapped-V variant (bottom -> V0, top -> V1) flipped the writer an extra time relative to the
    // reader, storing the HUD upside down and making the composite show it flipped on screen.
    FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, Target->g_menu->vb_stride, Offset);
    pv->set(EPS, float(_hf + EPS), d_Z, d_W, C, p0.x, p1.y);               // bottom-left  -> V~1
    pv++;
    pv->set(EPS, EPS, d_Z, d_W, C, p0.x, p0.y);                            // top-left     -> V~0
    pv++;
    pv->set(float(_wf + EPS), float(_hf + EPS), d_Z, d_W, C, p1.x, p1.y);  // bottom-right -> V~1
    pv++;
    pv->set(float(_wf + EPS), EPS, d_Z, d_W, C, p1.x, p0.y);               // top-right    -> V~0
    RImplementation.Vertex.Unlock(4, Target->g_menu->vb_stride);

    RCache.set_Shader(s_resolve_shader);
    // IMPORTANT: set_c("name") resolves the constant against the CURRENT shader's ctable (R_Backend.h
    // set_c(cpcstr)). set_Shader swaps the ctable to the resolve shader, so per-pass constants must be
    // set AFTER set_Shader — otherwise the lookup misses and the value stays at 0 (-> black HUD).
    // This mirrors accum_direct (r4_rendertarget_accum_direct.cpp:260-263) which calls set_c after
    // set_Element. m_shadow/lumscale are non-auto-bound; both must be set here.
    if (shadow_ok)
        RCache.set_c("m_shadow", m_shadow);
    RCache.set_c("hud_ovl_lumscale", hud_ovl_lumscale);
    // $user RT keeps the same CTexture when CRT is recreated; set_Textures skips bind if the pointer
    // matches, leaving a stale/released SRV — force rebind for every texture slot in this pass.
    if (ShaderElement* se = s_resolve_shader->E[0]._get())
    {
        if (SPass* pass = se->passes[0]._get())
        {
            if (STextureList* tls = pass->T._get())
            {
                for (const auto& st : *tls)
                {
                    if (CTexture* tex = st.second._get())
                        tex->apply_load(RCache, st.first);
                }
            }
        }
    }
    RCache.set_Geometry(Target->g_menu);
    RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);

    // The cascade slice (NEAR, set above for the resolve) is restored to FAR by the RAII guard on
    // scope exit - no manual restore needed here. The lens pass below does not touch the cascade.

    // 3) HUD forward passes (scope lens and other blended/emissive surfaces) on top of the resolve.
    //    The lens is rendered HERE (offscreen overlay pass), NOT in the world pass — this avoids
    //    recursion: the lens samples the clean backbuffer copy (rt_secondVP) which has no HUD.
    //    Use SetSVPActive(true) to set m_blender_mode.z=1 — the SAME path as mode 1, which the
    //    lens shader's constant binder definitely picks up.
    //    The overlay RT is rebound WITH the depth buffer filled in step 1 (the opaque HUD mesh).
    //    The lens shader has :zb(true,false) — the shader element overrides it, so a valid depth
    //    buffer is required or the lens geometry is Z-rejected and nothing draws.
    //
    //    mapHUDSorted holds TWO kinds of transparent HUD surface that need OPPOSITE depth handling,
    //    so render_hud_blends drains it in two filtered passes via the lens_depth_clear callback:
    //      Pass A — non-lens (collimator glass models\lfo_light_dot_weapons and similar): keep the
    //        HUD-mesh depth so the glass is depth-tested and does not shine through the mesh.
    //      Pass B — scope lens (bScopeLens): after ClearHudOverlayDepth() resets s_rt_z to far, so
    //        the lens (Z-rejected by the scope body otherwise) draws over the lens opening.
    Target->u_setrt(RCache, s_rt_work, nullptr, nullptr, s_rt_z->pZRT[RCache.context_id]);
    RCache.set_Stencil(FALSE);
    RCache.set_Z(TRUE); // Z test ON — pass A tests against the step-1 HUD-mesh depth
    // wasSVPActive was captured above (before the override); the RAII guard restores it on exit.
    Device.m_SecondViewport.SetSVPActive(true);
    // Force-rebind $user$viewport2 — the CTexture for rt_secondVP may hold a stale SRV after
    // ResizeSecondVPRT; surface_set re-creates the SRV so set_Textures sees the current one.
    if (Target->rt_secondVP && Target->rt_secondVP->pTexture._get())
    {
        CTexture* T = Target->rt_secondVP->pTexture._get();
        T->surface_set(Target->rt_secondVP->pSurface);
    }
    RImplementation.rmNormal(RCache);
    RCache.set_ColorWriteEnable();

    // Two-pass overlay drain: non-lens (depth-tested vs mesh) then lens (depth cleared to far).
    dg.render_hud_blends(&ClearHudOverlayDepth);

    // Publish the composited overlay.
    // DX11: FlipOverlayV blits s_rt_work -> s_rt_overlay (1:1 identity copy; the DX UI-layer composite
    //   samples the public name), then force-rebinds the $user$hud_overlay CTexture.
    // GL: nothing to do here — resolve + forward passes already wrote into s_rt_overlay directly
    //   (no work/flip pair). CompositeHudOverlay force-rebinds the overlay texture before it draws.
#if defined(USE_DX11)
    FlipOverlayV(s_rt_work, s_rt_overlay);
    // Force-rebind the $user$hud_overlay CTexture: it carries the SAME texture object across recreations,
    // so set_Textures would otherwise keep a stale/non-current SRV. surface_set refreshes the binding
    // so the UI composite (UIGameCustom) samples the just-blitted content.
    if (s_rt_overlay && s_rt_overlay->pTexture._get())
    {
        CTexture* T = s_rt_overlay->pTexture._get();
        T->surface_set(s_rt_overlay->pSurface);
    }
#endif

    // SVP state, m_blender_mode.y/w, cascade slice, render target, Z/stencil/cull and the Device
    // camera are all restored by the state_guard destructor on scope exit (no manual restore here).

    // r__hud_overlay_debug 5: one-shot RT dump per activation for visual inspection.
    // Dumps the overlay RT (what the UI samples on DX), plus the G-buffer and secondVP RTs, and on DX11
    // also dumps the work RT (pre-FlipOverlayV) to compare with the published overlay. Done BEFORE the
    // guard's destructor so we dump the overlay RT, not the restored base RT.
    //
    // Trigger on the FIRST frame where m_HudOverlayAlpha >= 0.9 (stable aim, fully raised weapon),
    // NOT on the first activation frame: the HUD crossfade lerps alpha 0->1 across the ADS entry
    // animation (rotFactor 0.5 -> 0.9), so a first-frame dump captures an almost-empty overlay and a
    // near-transparent composite, which looks like "the overlay is fully transparent" but is just the
    // fade-in. Dumping at alpha >= 0.9 captures the steady-state the player actually sees while aiming.
    static bool s_dumped = false;
    if (ps_r__hud_overlay_debug == 5 && !s_dumped && m_HudOverlayAlpha >= 0.9f)
    {
        s_dumped = true;
        DumpRT("work", s_rt_work);
        DumpRT("overlay", s_rt_overlay);
        DumpRT("defer", s_rt_defer);
        DumpRT("pos", s_rt_p);
        if (s_rt_n)
            DumpRT("norm", s_rt_n);
        if (Target->rt_secondVP)
            DumpRT("svp", Target->rt_secondVP);
    }
    if (ps_r__hud_overlay_debug != 5)
        s_dumped = false;
    // state_guard destructor runs here, restoring all overridden global state.
#else  // USE_DX11
    // GL stub: HUD overlay is DX11-only (see r2.h SetHudOverlayActive). m_HudOverlayActive is forced
    // false on GL, so the early-return above is what normally runs; this body exists only to keep the
    // function linkable on GL. Flush the HUD maps (mirrors the DX11 early-exit) for consistency.
    auto& dg = get_imm_context();
    dg.mapHUD.clear();
    dg.mapHUDSorted.clear();
    dg.mapHUDEmissive.clear();
#endif // USE_DX11
}

// Native overlay composite. Previously this drew a fullscreen $user$hud_overlay quad over the GL
// backbuffer to work around UI-layer (CUIStatic) stencil/Y-orientation issues (bug #22). The GL path
// was removed after it produced a magenta silhouette fringe; GL now keeps the overlay feature off
// entirely (see r2.h SetHudOverlayActive), so this method has nothing to do on any backend. The DX11
// UI-layer composite in UIGameCustom was never routed through here either. Kept as an empty stub to
// preserve the IRender vtable layout shared with xrGame.
void CRender::CompositeHudOverlay() {}
} // namespace xray::render::RENDER_NAMESPACE
