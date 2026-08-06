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
// s_rt_overlay — public RT carrying the $user$hud_overlay name. The UI layer (UIGameCustom) samples
//   it through stub_notransform_t.vs.
//   DX: FlipOverlayV publishes s_rt_work -> s_rt_overlay as a 1:1 identity CopyResource.
//   GL: FlipOverlayV publishes s_rt_work -> s_rt_overlay as a 1:1 identity glBlitFramebuffer (NO Y-flip).
//       The resolve quad (writer) and the UI composite quad (reader) both use the SAME
//       stub_notransform_t.vs with the SAME canonical UV order, so they are symmetric — a straight
//       1:1 copy lands the HUD upright (same as the working wpn_icon path). A Y-flip here inverts it
//       once and was the "two HUDs / upside-down" bug (hud-overlay-scope-plan.md #14-#22).
// s_rt_work — private work RT. resolve + forward passes write here on every backend; FlipOverlayV
//   publishes it to s_rt_overlay for the UI-layer composite.
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
    // resolve + forward passes always write into the private s_rt_work; FlipOverlayV then publishes
    // it to the public s_rt_overlay (identity copy on DX, Y-flip blit on GL) for the UI-layer
    // composite. Both RTs are created on every backend.
    if (s_gbuf_w == w && s_gbuf_h == h && s_rt_p && s_rt_defer && s_rt_z && s_rt_overlay && s_rt_work)
        return;

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
    s_rt_work.create(WORK_NAME, w, h, D3DFMT_A8R8G8B8, 1);
    s_rt_overlay.create(RT_NAME, w, h, D3DFMT_A8R8G8B8, 1);

    VERIFY(s_rt_p && s_rt_defer && s_rt_z && s_rt_overlay && s_rt_work);
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
void FlipOverlayV(const ref_rt& src, const ref_rt& dst)
{
    if (!src._get() || !dst._get() || !src->pSurface || !dst->pSurface)
        return;
    auto pContext = HW.get_context(CHW::IMM_CTX_ID);
    pContext->CopyResource(dst->pSurface, src->pSurface);
}
#elif defined(USE_OGL)
// GL: the work -> overlay copy is a 1:1 IDENTITY blit, the SAME as DX's CopyResource.
//
// Why NO Y-flip (this was the "two HUDs / upside-down" bug, hud-overlay-scope-plan.md #14-#22):
// both the resolve quad (writer) and the UI composite quad (reader) run through the SAME VS
// (stub_notransform_t.vs), with the SAME canonical UV order (top-vertex -> V~0, bottom -> V~1) and
// the SAME top-down pixel input convention. The resolve quad writes a screen pixel with top-down
// coordinate y into work-RT at the NDC position stub_notransform_t.vs maps it to; the UI quad reads
// overlay-RT back through the identical mapping. The two sides are SYMMETRIC, so a straight 1:1
// copy lands the HUD upright — exactly like the working wpn_icon path (r2_weapon_icon.cpp), which
// resolves straight into the public UI RT with no flip at all.
//
// The Y-flip in CopyBackbufferToSecondVPRT (r2_R_render.cpp) is NOT applicable here: that blit feeds
// the scope LENS, whose reader (model_scope_lense.ps) samples s_vp2 via gl_FragCoord (lower-left
// origin) and does its OWN V-flip in the pixel shader — an ASYMMETRIC path. Copying that flip onto the
// symmetric overlay path inverts the HUD once, producing an upside-down transparent overlay over the
// (also-composited) world copy — i.e. the visible "two HUDs".
//
// CRT::pRT is a bare GLuint texture on GL (the engine keeps ONE shared FBO and re-attaches textures
// on the fly — no per-CRT framebuffer), so this uses two scratch FBOs + glFramebufferTexture2D +
// glBlitFramebuffer, saving/restoring the engine's FBO bindings (same skeleton as
// CopyBackbufferToSecondVPRT, only the source Y range differs).
void FlipOverlayV(const ref_rt& src, const ref_rt& dst)
{
    if (!src._get() || !dst._get() || !src->pRT || !dst->pRT)
        return;
    const GLuint srcTex = src->pRT;
    const GLuint dstTex = dst->pRT;
    const GLuint w = dst->dwWidth;
    const GLuint h = dst->dwHeight;

    static GLuint s_readFBO = 0, s_drawFBO = 0;
    if (!s_readFBO)
        CHK_GL(glGenFramebuffers(1, &s_readFBO));
    if (!s_drawFBO)
        CHK_GL(glGenFramebuffers(1, &s_drawFBO));

    GLint prevRead = 0, prevDraw = 0;
    CHK_GL(glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead));
    CHK_GL(glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw));

    CHK_GL(glBindFramebuffer(GL_READ_FRAMEBUFFER, s_readFBO));
    CHK_GL(glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, srcTex, 0));
    CHK_GL(glReadBuffer(GL_COLOR_ATTACHMENT0));

    CHK_GL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_drawFBO));
    CHK_GL(glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dstTex, 0));
    CHK_GL(glDrawBuffer(GL_COLOR_ATTACHMENT0));

    // 1:1 identity copy (no Y-flip): src 0..h maps to dst 0..h, top to top. A Y-flip here was the
    // historical "two HUDs / upside-down" bug (both the resolve quad and the UI composite quad share
    // stub_notransform_t.vs with the canonical UV order, so they are symmetric — a flip inverts once).
    CHK_GL(glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST));

    CHK_GL(glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead));
    CHK_GL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDraw));
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
#elif defined(USE_OGL)
// Debug (r__hud_overlay_debug 5): dump overlay RTs to $screenshots$ on GL.
// GL has no DirectXTex; we read the texture back via glReadPixels (into a scratch read-FBO, the same
// FBO-pair skeleton FlipOverlayV uses) and write a minimal uncompressed RGBA8 DDS with the DX9 legacy
// header layout, matching the DX11 DumpRT output byte-for-byte so the same viewers open both.
namespace hud_overlay
{
void DumpRT(pcstr label, const ref_rt& rt)
{
    if (!label || !rt._get() || !rt->pRT)
        return;

    const GLuint tex = rt->pRT;
    const u32 w = rt->dwWidth;
    const u32 h = rt->dwHeight;
    if (!w || !h)
        return;

    // glReadPixels reads the current READ framebuffer. Attach the source texture to a scratch read-FBO
    // (same pattern as FlipOverlayV) and save/restore GL_READ_FRAMEBUFFER_BINDING so we don't disturb
    // the engine's shared FBO state.
    static GLuint s_readFBO = 0;
    if (!s_readFBO)
        CHK_GL(glGenFramebuffers(1, &s_readFBO));

    GLint prevRead = 0;
    CHK_GL(glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead));
    CHK_GL(glBindFramebuffer(GL_READ_FRAMEBUFFER, s_readFBO));
    CHK_GL(glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0));
    CHK_GL(glReadBuffer(GL_COLOR_ATTACHMENT0));

    const GLenum status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        Msg("! [hud_overlay] DDS [%s]: read FBO incomplete (0x%x)", label, (unsigned)status);
        CHK_GL(glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead));
        return;
    }

    // Read as RGBA8 (engine overlay RTs are RGBA8). glReadPixels is bottom-row-first (GL convention);
    // we keep that order and flip in the DDS header via pitch/height sign is not standard, so instead
    // we write rows top-down into the DDS payload for parity with the DX11 dumps.
    xr_vector<u8> pixels;
    pixels.resize(size_t(w) * h * 4);
    CHK_GL(glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data()));
    CHK_GL(glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead));

    // Flip rows bottom->top so the file matches the DX11 (top-down) dumps for direct comparison.
    xr_vector<u8> flipped;
    flipped.resize(pixels.size());
    const u32 rowBytes = w * 4;
    for (u32 y = 0; y < h; ++y)
        memcpy(&flipped[size_t(y) * rowBytes], &pixels[size_t(h - 1 - y) * rowBytes], rowBytes);

    static u32 s_dump_seq{};
    string64 t_stemp{};
    string_path fname{};
    xr_sprintf(fname, "hud_ovl_%s_%s_%u.dds", label, timestamp(t_stemp), ++s_dump_seq);

    if (IWriter* fs = FS.w_open("$screenshots$", fname))
    {
        // Minimal DDS header (DX9 legacy, matches DX11 DumpRT's DirectX::DDS_FLAGS_FORCE_DX9_LEGACY).
        // magic + DDSURFACEDESC2 (124 bytes). Uncompressed RGBA8, no mipmaps.
        #pragma pack(push, 1)
        struct DDSPixelFormat
        {
            u32 size;   u32 flags; u32 fourCC; u32 rgbBitCount;
            u32 rMask;  u32 gMask; u32 bMask;  u32 aMask;
        };
        struct DDSHeader
        {
            u32 size; u32 flags; u32 height; u32 width; u32 pitchOrLinearSize;
            u32 depth; u32 mipMapCount; u32 reserved1[11];
            DDSPixelFormat ddspf;
            u32 caps; u32 caps2; u32 caps3; u32 caps4; u32 reserved2;
        };
        #pragma pack(pop)
        static_assert(sizeof(DDSHeader) == 124, "DDS header must be 124 bytes");

        DDSHeader hdr{};
        hdr.size = 124;
        hdr.flags = 0x1007; // DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT
        hdr.flags |= 0x1000; // DDSD_PITCH
        hdr.height = h;
        hdr.width = w;
        hdr.pitchOrLinearSize = rowBytes;
        hdr.mipMapCount = 0;
        hdr.ddspf.size = 32;
        hdr.ddspf.flags = 0x41; // DDPF_RGB | DDPF_ALPHAPIXELS
        hdr.ddspf.rgbBitCount = 32;
        hdr.ddspf.rMask = 0x00FF0000;
        hdr.ddspf.gMask = 0x0000FF00;
        hdr.ddspf.bMask = 0x000000FF;
        hdr.ddspf.aMask = 0xFF000000;
        hdr.caps = 0x1000; // DDSCAPS_TEXTURE

        fs->w("DDS ", 4);                       // magic
        fs->w(&hdr, sizeof(hdr));               // header
        fs->w(flipped.data(), flipped.size());  // pixels (top-down)
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
    // Save m_svp_rt_capture.z — we use this spare bit (x is consumed by the lens PS discard branch,
    // y/w are reserved; z is unused upstream) to flag the overlay pass to the lens shader so it can
    // skip the s_vp2 V-flip that is only correct in the world pass.
    const float svp_capture_z_saved = g_pGamePersistent ? g_pGamePersistent->m_pGShaderConstants->m_svp_rt_capture.z : 0.f;
    const bool wasSVPActive = Device.m_SecondViewport.IsSVPActive();
    bool shadow_slice_set = false; // true once we set_slice_read(SE_SUN_NEAR); the guard restores FAR

    // RAII guard: restores all overridden state on scope exit (normal return, early return, or throw).
    struct StateGuard
    {
        CRender& owner;
        CBackend& cmd_list;
        SDeviceCam& saved;
        float blender_y, blender_w, svp_capture_z;
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
                g_pGamePersistent->m_pGShaderConstants->m_svp_rt_capture.z = svp_capture_z;
            }
            // Restore cascade slice: the resolve used NEAR; leave FAR for the rest of the frame.
            if (shadow_slice_set && owner.Target->rt_smap_depth)
                owner.Target->rt_smap_depth->set_slice_read(SE_SUN_FAR);
            // Restore the main target and common states for the following UI rendering.
            // set_Stencil(FALSE) is essential on GL: the mini-G-buffer pass sets REPLACE 0x01 and the
            // UI-layer composite quad (UIGameCustom) does not set stencil itself, so it would inherit
            // an EQUAL-0x01 gate and only draw the overlay inside the HUD mask (erasing it where the
            // mask is empty). Clearing stencil here lets the UI quad composite over the whole screen.
            cmd_list.set_Stencil(FALSE);
            cmd_list.set_Z(TRUE);
            cmd_list.set_CullMode(CULL_CCW);
            // (0, 0) for the unused RT slots — matches the engine's canonical base-RT bind
            // (r2_R_render.cpp:42). On GL GLuint is an integer type, so 0 is the null texture handle.
            owner.Target->u_setrt(cmd_list, w, h, owner.Target->get_base_rt(), 0, 0, owner.Target->get_base_zb());
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
    } state_guard{ *this, RCache, saved, blender_y_saved, blender_w_saved, svp_capture_z_saved, wasSVPActive, shadow_slice_set, w, h };

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
    //      DX11 only: GL first iteration resolves WITHOUT sun shadow (shadow sampling in a standalone
    //      shader was never completed — m_shadow binding did not work; see hud-overlay-scope-plan.md
    //      bug #23 / "D. Shadow sampling — ОТЛОЖЕН"). The GL resolve shader applies ambient+hemi+sun
    //      N·L only (sun_sh = 1), so no m_shadow is set there.
    bool shadow_ok = false;
    Fmatrix m_shadow; // declared here so it survives the if-block; set_c runs AFTER set_Shader (below)
#if defined(USE_DX11)
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
#endif // USE_DX11

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

    // 2) Resolve albedo into the work RT (transparent background via stencil mask). On every backend
    //    resolve writes into s_rt_work; FlipOverlayV publishes it to s_rt_overlay afterwards (1:1
    //    identity copy on BOTH DX and GL — no Y-flip) so the UI-layer composite (UIGameCustom) sees
    //    the HUD upright (resolve quad and UI quad share stub_notransform_t.vs + canonical UV order).
#if defined(USE_DX11)
    Target->u_setrt(RCache, s_rt_work, nullptr, nullptr, s_rt_z->pZRT[RCache.context_id]);
#elif defined(USE_OGL)
    Target->u_setrt(RCache, w, h, s_rt_work->pRT, 0, 0, s_rt_z->pZRT);
#else
#   error No graphics API selected or enabled!
#endif
    RImplementation.rmNormal(RCache);
    const Fcolor clear_ui(0.f, 0.f, 0.f, 0.f);
    RCache.ClearRT(s_rt_work, clear_ui);

    RCache.set_CullMode(CULL_NONE);
    RCache.set_Z(FALSE);
    RCache.set_Stencil(FALSE);
    RCache.set_ColorWriteEnable(); // RGBA all channels

    // NOTE: the manual set_* calls above run BEFORE set_Shader below. On GL, set_Shader invokes
    // glState::Apply(), which RE-APPLIES the resolve pass's stencil/depth/cull/blend state from the
    // pass's glState descriptor. The resolve shader's .s block does not explicitly disable stencil/Z,
    // and the glState default ctor enables StencilEnable=TRUE and DepthEnable=TRUE — so Apply()
    // re-enables GL_STENCIL_TEST and GL_DEPTH_TEST, leaving the resolve quad gated by the HUD-shaped
    // 0x01 stencil mask written in step 1 (and the filled Z buffer). On GL this cuts a HUD-shaped hole
    // in the resolve coverage (the "second upside-down transparent HUD" symptom): the quad is fixed-
    // function clipped in the HUD silhouette instead of overwriting the whole RT. The proper fix is to
    // issue the protective state setters AFTER set_Shader so they win, mirroring the post-pass
    // set_Stencil(FALSE) pattern already used in the StateGuard destructor (r2_hud_overlay.cpp lines
    // 574-580). The block is repeated after set_Shader (below) for that reason; the pre-set_Shader
    // block is kept as a no-op safety in case a future path skips the shader setup.
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
    // GL: glState::Apply() runs inside set_Shader and re-enables StencilEnable=TRUE / DepthEnable=TRUE
    // from the resolve pass's glState descriptor (the .s block does not disable them). The mini-G-buffer
    // step wrote a HUD-shaped 0x01 stencil mask and filled the Z buffer, so those re-enabled tests clip
    // the fullscreen resolve quad to the HUD silhouette, cutting a HUD-shaped hole in the silly work RT
    // — the "second upside-down transparent HUD". Re-apply the protective state here, AFTER set_Shader,
    // so it wins (mirrors the StateGuard post-pass pattern). On DX11 these are a no-op against the
    // pass-desc state (the DSS object is rebuilt lazily from the cached desc), so this is portable.
    RCache.set_CullMode(CULL_NONE);
    RCache.set_Z(FALSE);
    RCache.set_Stencil(FALSE);
    RCache.set_ColorWriteEnable();
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
#if defined(USE_DX11)
    Target->u_setrt(RCache, s_rt_work, nullptr, nullptr, s_rt_z->pZRT[RCache.context_id]);
#elif defined(USE_OGL)
    Target->u_setrt(RCache, w, h, s_rt_work->pRT, 0, 0, s_rt_z->pZRT);
#else
#   error No graphics API selected or enabled!
#endif
    RCache.set_Stencil(FALSE);
    RCache.set_Z(TRUE); // Z test ON — pass A tests against the step-1 HUD-mesh depth
    // wasSVPActive was captured above (before the override); the RAII guard restores it on exit.
    Device.m_SecondViewport.SetSVPActive(true);
    // Flag the overlay pass to the scope lens shader (model_scope_lense.ps) via the spare
    // m_svp_rt_capture.z bit (x is consumed by the discard branch, y/w are reserved elsewhere;
    // z is unused upstream). The lens shader's V-flip for s_vp2 (svp_tc.y = 1 - gl_FragCoord.y/H)
    // is correct only in the world pass where rt_secondVP is stored the standard GL way; the overlay
    // pipeline stores the published overlay RT via stub_notransform_t.vs whose NDC-Y inversion makes
    // that flip double-invert the lens image here, so the lens must SKIP the V-flip in the overlay pass.
    if (g_pGamePersistent && g_pGamePersistent->m_pGShaderConstants)
        g_pGamePersistent->m_pGShaderConstants->m_svp_rt_capture.z = 1.0f;
    // Force-rebind $user$viewport2 so the lens shader samples the current rt_secondVP content.
    // DX11: the CTexture may hold a stale SRV after ResizeSecondVPRT; surface_set re-creates it.
    // GL: surface_set only stores the GLuint (no SRV to invalidate), but calling it is harmless and
    // keeps the path uniform — the blit in CopyBackbufferToSecondVPRT writes the same texture name.
#if defined(USE_DX11)
    if (Target->rt_secondVP && Target->rt_secondVP->pTexture._get())
    {
        CTexture* T = Target->rt_secondVP->pTexture._get();
        T->surface_set(Target->rt_secondVP->pSurface);
    }
#endif
    RImplementation.rmNormal(RCache);
    RCache.set_ColorWriteEnable();

    // Two-pass overlay drain: non-lens (depth-tested vs mesh) then lens (depth cleared to far).
    dg.render_hud_blends(&ClearHudOverlayDepth);

    // Publish the composited overlay from the work RT to the public $user$hud_overlay RT.
    // Both DX and GL do a 1:1 identity copy (DX: CopyResource; GL: identity glBlitFramebuffer, NO
    // Y-flip — resolve quad and UI quad share stub_notransform_t.vs + canonical UV order, so they are
    // symmetric and a flip would invert the HUD; see FlipOverlayV for the full rationale).
    // DX then force-rebinds the $user$hud_overlay CTexture (stale-SRV guard after CopyResource); on GL
    // surface_set only stores the GLuint the blit already wrote into, so no rebind is needed there.
    FlipOverlayV(s_rt_work, s_rt_overlay);
#if defined(USE_DX11)
    if (s_rt_overlay && s_rt_overlay->pTexture._get())
    {
        CTexture* T = s_rt_overlay->pTexture._get();
        T->surface_set(s_rt_overlay->pSurface);
    }
#endif

    // SVP state, m_blender_mode.y/w, cascade slice, render target, Z/stencil/cull and the Device
    // camera are all restored by the state_guard destructor on scope exit (no manual restore here).

    // r__hud_overlay_debug 5: one-shot RT dump per activation for visual inspection. Dumps the
    // published overlay RT plus the G-buffer and secondVP RTs and the work RT (pre-FlipOverlayV) to
    // compare with the published overlay. Done BEFORE the guard's destructor so we dump the overlay
    // RT, not the restored base RT.
    //
    // Trigger on the FIRST frame where m_HudOverlayAlpha >= 0.9 (stable aim, fully raised weapon),
    // NOT on the first activation frame: the HUD crossfade lerps alpha 0->1 across the ADS entry
    // animation (rotFactor 0.5 -> 0.9), so a first-frame dump captures an almost-empty overlay and a
    // near-transparent composite, which looks like "the overlay is fully transparent" but is just the
    // fade-in. Dumping at alpha >= 0.9 captures the steady-state the player actually sees while aiming.
    static bool s_dumped = false;
    // Backend-agnostic: DumpRT has both DX11 (DirectXTex) and GL (glReadPixels) implementations.
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
}

// Native overlay composite is unused on every backend: the overlay is composited by the UI layer
// (UIGameCustom CUIStatic sampling $user$hud_overlay). Kept as an empty stub to preserve the IRender
// vtable layout shared with xrGame.
void CRender::CompositeHudOverlay() {}
} // namespace xray::render::RENDER_NAMESPACE
