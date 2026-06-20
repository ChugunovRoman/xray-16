#include "stdafx.h"

#include "Layers/xrRender_R2/r2.h"
#include "Layers/xrRender/SH_RT.h"
#include "Layers/xrRender/Shader.h"
#include "xrEngine/device.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/IRenderable.h"
#include "xrCore/FS.h"
#include "Layers/xrRender/xrRender_console.h"
#include <algorithm>

#if defined(USE_DX11)
#include "Layers/xrRenderPC_R4/r4_rendertarget.h"
#include <DirectXTex.h>
#include "xrCore/_std_extensions.h"
#endif

namespace xray::render::RENDER_NAMESPACE
{
namespace wpn_icon
{
u32 s_icon_gbuf_w{};
u32 s_icon_gbuf_h{};
u32 s_icon_gbuf_cfg_pack{};

// Keep UI RT alive: local ref_rt in WeaponIcon_RenderToTexture used to be destroyed on return, clearing
// CTexture's pSurface — UI then sampled an empty $user$ texture (fully transparent). DDS dumps still looked fine.
static xr_map<shared_str, ref_rt> s_persist_icon_ui_rt;
void ReleasePersistedIconUiRts() { s_persist_icon_ui_rt.clear(); }

ref_rt s_rt_p{};
ref_rt s_rt_n{};
ref_rt s_rt_defer{}; // deferred albedo (same fmt as rt_Color); UI RT is separate A8R8G8B8
ref_rt s_rt_z{};

ref_shader s_wpn_icon_resolve_shader{};

u32 PackGbufCfg(const CRender& R)
{
    return (R.o.gbuffer_opt ? 1u : 0u) | (R.o.albedo_wo ? 2u : 0u);
}

void DestroyIconGBuffer()
{
    s_rt_p = nullptr;
    s_rt_n = nullptr;
    s_rt_defer = nullptr;
    s_rt_z = nullptr;
    s_icon_gbuf_w = s_icon_gbuf_h = s_icon_gbuf_cfg_pack = 0;
}

void EnsureIconGBuffer(u32 w, u32 h, CRenderTarget* Tgt, const CRender& R)
{
    const u32 pack = PackGbufCfg(R);
    if (s_icon_gbuf_w == w && s_icon_gbuf_h == h && s_icon_gbuf_cfg_pack == pack)
        return;

    DestroyIconGBuffer();
    s_icon_gbuf_w = w;
    s_icon_gbuf_h = h;
    s_icon_gbuf_cfg_pack = pack;

    VERIFY(Tgt && Tgt->rt_Position && Tgt->rt_Color);
    constexpr u32 sc = 1;
    s_rt_p.create("$user$__wpn_icon_P", w, h, Tgt->rt_Position->fmt, sc);
    if (!R.o.gbuffer_opt)
    {
        VERIFY(Tgt->rt_Normal);
        s_rt_n.create("$user$__wpn_icon_N", w, h, Tgt->rt_Normal->fmt, sc);
    }
    s_rt_defer.create("$user$__wpn_icon_defer_albedo", w, h, Tgt->rt_Color->fmt, sc);
    s_rt_z.create("$user$__wpn_icon_Z", w, h, HW.Caps.fDepth, sc, { CRT::CreateSurface });
    VERIFY(s_rt_p && s_rt_defer && s_rt_z);
    if (!R.o.gbuffer_opt)
        VERIFY(s_rt_n);
}

void EnsureWpnIconResolveShader()
{
    if (s_wpn_icon_resolve_shader)
        return;
    s_wpn_icon_resolve_shader.create("wpn_icon_resolve", "$user$__wpn_icon_defer_albedo");
}

#if defined(USE_DX11)
// Debug: dump RT to $screenshots$ (same approach as dx11r_screenshot.cpp).
void SaveWeaponIconRtToDds(pcstr slot_label, pcstr item_label, const ref_rt& rt)
{
    if (!slot_label || !slot_label[0] || !rt._get() || !rt->pRT)
        return;

    ID3DResource* pSrc{};
    rt->pRT->GetResource(&pSrc);
    if (!pSrc)
    {
        Msg("! [weapon_inv_icon] DDS [%s]: GetResource failed", slot_label);
        return;
    }

    DirectX::ScratchImage image;
    const HRESULT cap =
        CaptureTexture(HW.pDevice, HW.get_context(CHW::IMM_CTX_ID), pSrc, image);
    if (FAILED(cap))
    {
        Msg("! [weapon_inv_icon] DDS [%s]: CaptureTexture failed hr=0x%08x", slot_label, (unsigned)cap);
        _RELEASE(pSrc);
        return;
    }

    DirectX::Blob saved;
    const HRESULT hr =
        SaveToDDSMemory(*image.GetImage(0, 0, 0), DirectX::DDS_FLAGS_FORCE_DX9_LEGACY, saved);
    if (FAILED(hr))
    {
        Msg("! [weapon_inv_icon] DDS [%s]: SaveToDDSMemory failed hr=0x%08x", slot_label, (unsigned)hr);
        _RELEASE(pSrc);
        return;
    }

    static u32 s_dump_seq{};
    string64 t_stemp{};
    string256 safe_item{};
    const pcstr raw_item = (item_label && item_label[0]) ? item_label : "unknown_item";
    {
        u32 j = 0;
        for (u32 i = 0; raw_item[i] && j < (sizeof(safe_item) - 1); ++i)
        {
            const char c = raw_item[i];
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c == '_') ||
                (c == '-');
            safe_item[j++] = ok ? c : '_';
        }
        safe_item[j] = 0;
    }
    string_path fname{};
    xr_sprintf(fname, "wpn_icon_%s_%s_%s_%u.dds", slot_label, safe_item, timestamp(t_stemp), ++s_dump_seq);

    if (IWriter* fs = FS.w_open("$screenshots$", fname))
    {
        fs->w(saved.GetBufferPointer(), saved.GetBufferSize());
        FS.w_close(fs);
        Msg("~ [weapon_inv_icon] DDS -> $screenshots$\\%s", fname);
    }
    else
        Msg("! [weapon_inv_icon] DDS: cannot write $screenshots$\\%s", fname);

    _RELEASE(pSrc);
}

bool SavePersistedIconRtToDdsDxt5(pcstr user_texture_name, pcstr fs_root, pcstr fname)
{
    if (!user_texture_name || !user_texture_name[0] || !fs_root || !fs_root[0] || !fname || !fname[0])
        return false;

    const auto it = s_persist_icon_ui_rt.find(shared_str(user_texture_name));
    if (it == s_persist_icon_ui_rt.end() || !it->second._get() || !it->second->pRT)
    {
        Msg("! [weapon_inv_icon] SaveDDS: no persisted RT for [%s]", user_texture_name);
        return false;
    }

    ID3DResource* pSrc{};
    it->second->pRT->GetResource(&pSrc);
    if (!pSrc)
    {
        Msg("! [weapon_inv_icon] SaveDDS [%s]: GetResource failed", user_texture_name);
        return false;
    }

    DirectX::ScratchImage image;
    const HRESULT cap = CaptureTexture(HW.pDevice, HW.get_context(CHW::IMM_CTX_ID), pSrc, image);
    if (FAILED(cap))
    {
        Msg("! [weapon_inv_icon] SaveDDS [%s]: CaptureTexture hr=0x%08x", user_texture_name, (unsigned)cap);
        _RELEASE(pSrc);
        return false;
    }

    DirectX::ScratchImage compressed;
    HRESULT hr = Compress(*image.GetImage(0, 0, 0), DXGI_FORMAT_BC3_UNORM,
        DirectX::TEX_COMPRESS_DEFAULT | DirectX::TEX_COMPRESS_PARALLEL, 0.0f, compressed);
    if (FAILED(hr))
    {
        Msg("! [weapon_inv_icon] SaveDDS [%s]: Compress BC3 hr=0x%08x", user_texture_name, (unsigned)hr);
        _RELEASE(pSrc);
        return false;
    }

    DirectX::Blob saved;
    hr = SaveToDDSMemory(*compressed.GetImage(0, 0, 0), DirectX::DDS_FLAGS_FORCE_DX9_LEGACY, saved);
    if (FAILED(hr))
    {
        Msg("! [weapon_inv_icon] SaveDDS [%s]: SaveToDDSMemory hr=0x%08x", user_texture_name, (unsigned)hr);
        _RELEASE(pSrc);
        return false;
    }

    if (IWriter* fs = FS.w_open(fs_root, fname))
    {
        if (!fs->valid())
        {
            Msg("! [weapon_inv_icon] SaveDDS: file open failed (check path/permissions) $game_textures$\\%s", fname);
            FS.w_close(fs);
            _RELEASE(pSrc);
            return false;
        }
        fs->w(saved.GetBufferPointer(), saved.GetBufferSize());
        FS.w_close(fs);
        string_path abs_log{};
        xr_strcpy(abs_log, fname);
        if (FS.update_path(abs_log, fs_root, abs_log, false))
            Msg("~ [weapon_inv_icon] SaveDDS DXT5 -> %s", abs_log);
        else
            Msg("~ [weapon_inv_icon] SaveDDS DXT5 -> %s\\%s", fs_root, fname);
        _RELEASE(pSrc);
        return true;
    }

    Msg("! [weapon_inv_icon] SaveDDS: cannot write %s\\%s", fs_root, fname);
    _RELEASE(pSrc);
    return false;
}
#endif // USE_DX11

} // namespace wpn_icon

void WeaponIcon_ReleaseStaticResources()
{
    wpn_icon::ReleasePersistedIconUiRts();
    if (g_pGamePersistent)
        g_pGamePersistent->OnWeaponIconUserRtsReleased();
    wpn_icon::s_wpn_icon_resolve_shader = nullptr;
    wpn_icon::DestroyIconGBuffer();
}

void CRender::WeaponIcon_ReleaseUserIconRt(pcstr texture_name)
{
    if (!texture_name || !texture_name[0])
        return;
    const shared_str key(texture_name);
    const auto it = wpn_icon::s_persist_icon_ui_rt.find(key);
    if (it != wpn_icon::s_persist_icon_ui_rt.end())
        wpn_icon::s_persist_icon_ui_rt.erase(it);
}

void CRender::WeaponIcon_ReleaseAllUserIconRts()
{
    wpn_icon::s_persist_icon_ui_rt.clear();
}

bool CRender::WeaponIcon_SavePersistedUserRtToDdsDxt5(pcstr user_texture_name, pcstr fs_root, pcstr fname)
{
#if !defined(USE_DX11)
    (void)user_texture_name;
    (void)fs_root;
    (void)fname;
    return false;
#else
    return wpn_icon::SavePersistedIconRtToDdsDxt5(user_texture_name, fs_root, fname);
#endif
}

bool CRender::WeaponIcon_RenderToTexture(
    pcstr texture_name, u32 w, u32 h, const Fmatrix& view, const Fmatrix& proj, IRenderable* subject)
{
    using namespace wpn_icon;
    if (!texture_name || !texture_name[0] || !w || !h)
        return false;
    if (!Device.b_is_Ready || !Target)
        return false;
    if (!subject)
        return false;

    if (o.albedo_wo)
    {
        static bool s_warned{};
        if (!s_warned)
        {
            s_warned = true;
            Msg("! [weapon_inv_icon] renderer uses albedo_wo; weapon inventory icon pass is not supported yet (empty RT).");
        }
        // Must return false: UI treats success as "dynamic icon ready" and switches off static inv_icon.
        return false;
    }

    VERIFY(Target->rt_Color);
    ref_rt& rt_ui = wpn_icon::s_persist_icon_ui_rt[shared_str(texture_name)];
    rt_ui.create(texture_name, w, h, D3DFMT_A8R8G8B8, 1);
    if (!rt_ui)
        return false;

    EnsureIconGBuffer(w, h, Target, *this);
    EnsureWpnIconResolveShader();
    if (!s_wpn_icon_resolve_shader)
    {
        Msg("! [weapon_inv_icon] missing shader r3\\wpn_icon_resolve (.s/.ps) under $game_shaders$ — cannot build UI texture.");
        return false;
    }

    struct SDeviceCam
    {
        Fmatrix mView{}, mProject{}, mFullTransform{}, mInvView{}, mInvFullTransform{};
        Fvector vCameraPosition{}, vCameraDirection{}, vCameraTop{}, vCameraRight{};
        u32 dwWidth{};
        u32 dwHeight{};
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
    saved.dwWidth = Device.dwWidth;
    saved.dwHeight = Device.dwHeight;

    Device.mView = view;
    Device.mProject = proj;
    Device.mInvView.invert(Device.mView);
    Device.mFullTransform.mul(Device.mProject, Device.mView);
    Device.mInvFullTransform.invert_44(Device.mFullTransform);
    Device.vCameraPosition.set(Device.mInvView.c.x, Device.mInvView.c.y, Device.mInvView.c.z);
    Device.mInvView.transform_dir(Device.vCameraDirection, Fvector().set(0.f, 0.f, -1.f));
    Device.vCameraDirection.normalize();
    Device.mInvView.transform_dir(Device.vCameraTop, Fvector().set(0.f, 1.f, 0.f));
    Device.vCameraTop.normalize();
    Device.vCameraRight.crossproduct(Device.vCameraTop, Device.vCameraDirection);
    Device.vCameraRight.normalize();
    Device.dwWidth = w;
    Device.dwHeight = h;
    GEnv.Render->SetCacheXform(Device.mView, Device.mProject);
    RCache.set_xform_view(Device.mView);
    RCache.set_xform_project(Device.mProject);

    auto& dg = get_imm_context();
    const u32 phase_saved = dg.o.phase;
    const bool pm0 = dg.o.pmask[0];
    const bool pm1 = dg.o.pmask[1];
    const bool pmw = dg.o.pmask_wmark;

    dg.marker++;
    dg.o.phase = PHASE_NORMAL;
    dg.r_pmask(true, true, true);

    if (!o.gbuffer_opt)
        Target->u_setrt(RCache, s_rt_p, s_rt_n, s_rt_defer, s_rt_z);
    else
        Target->u_setrt(RCache, s_rt_p, s_rt_defer, s_rt_z);

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

    if (g_pGamePersistent)
        g_pGamePersistent->OnWeaponIconSnapshot(subject, true);
    subject->renderable_Render(dg.context_id, subject);
    if (g_pGamePersistent)
        g_pGamePersistent->OnWeaponIconSnapshot(subject, false);

    dg.render_graph(0);
    dg.render_graph(1);

#if RENDER != R_R1
    // render_graph only drains normal + matrix passes. insert_dynamic also queues strict-sorted, emissive,
    // wmark, distort with the same IRenderable*; if we return while those still hold a stack-local bake proxy,
    // the next full frame Render() will apply_object() on a dangling pointer.
    dg.mapEmissive.clear();
    dg.mapSorted.clear();
    dg.mapWmark.clear();
    dg.mapDistort.clear();
#endif

    // SaveWeaponIconRtToDds("defer", texture_name, s_rt_defer);

    dg.o.phase = phase_saved;
    dg.r_pmask(pm0, pm1, pmw);

    // Bind the same depth-stencil as the g-buffer pass so stencil test can mask the resolve quad (transparent bg).
#if defined(USE_DX11)
    Target->u_setrt(RCache, rt_ui, nullptr, nullptr, s_rt_z->pZRT[RCache.context_id]);
#elif defined(USE_OGL)
    Target->u_setrt(RCache, w, h, rt_ui->pRT, 0, 0, s_rt_z->pZRT);
#else
#   error No graphics API selected or enabled!
#endif
    RImplementation.rmNormal(RCache);
    const Fcolor clear_ui(0.f, 0.f, 0.f, 0.f);
    RCache.ClearRT(rt_ui, clear_ui);

    RCache.set_CullMode(CULL_NONE);
    RCache.set_Z(FALSE);
    // Only fill pixels where the g-buffer pass marked stencil==0x01 so the UI RT keeps clear color (transparent a).
    RCache.set_Stencil(TRUE, D3DCMP_EQUAL, 0x01, 0xff, 0x00, D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP,
        D3DSTENCILOP_KEEP);
    RCache.set_ColorWriteEnable();

    u32 Offset;
    const u32 C = color_rgba(255, 255, 255, 255);
    const float _wf = float(w);
    const float _hf = float(h);
    Fvector2 p0, p1;
    p0.set(.5f / _wf, .5f / _hf);
    p1.set((_wf + .5f) / _wf, (_hf + .5f) / _hf);
    const float d_Z = EPS_S;
    const float d_W = 1.f;

    FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, Target->g_menu->vb_stride, Offset);
    pv->set(EPS, float(_hf + EPS), d_Z, d_W, C, p0.x, p1.y);
    pv++;
    pv->set(EPS, EPS, d_Z, d_W, C, p0.x, p0.y);
    pv++;
    pv->set(float(_wf + EPS), float(_hf + EPS), d_Z, d_W, C, p1.x, p1.y);
    pv++;
    pv->set(float(_wf + EPS), EPS, d_Z, d_W, C, p1.x, p0.y);
    RImplementation.Vertex.Unlock(4, Target->g_menu->vb_stride);

    RCache.set_Shader(s_wpn_icon_resolve_shader);
    // $user RT keeps the same CTexture when CRT is recreated; set_Textures skips bind if the pointer
    // matches, leaving a stale/released SRV — force rebind for every texture slot in this pass.
    if (ShaderElement* se = s_wpn_icon_resolve_shader->E[0]._get())
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

    RCache.set_Stencil(FALSE);
#if defined(USE_DX11)
    Target->u_setrt(RCache, saved.dwWidth, saved.dwHeight, Target->get_base_rt(), nullptr, nullptr, Target->get_base_zb());
#elif defined(USE_OGL)
    Target->u_setrt(RCache, saved.dwWidth, saved.dwHeight, Target->get_base_rt(), 0, 0, Target->get_base_zb());
#else
#   error No graphics API selected or enabled!
#endif
    RImplementation.rmNormal(RCache);

    Device.mView = saved.mView;
    Device.mProject = saved.mProject;
    Device.mFullTransform = saved.mFullTransform;
    Device.mInvView = saved.mInvView;
    Device.mInvFullTransform = saved.mInvFullTransform;
    Device.vCameraPosition = saved.vCameraPosition;
    Device.vCameraDirection = saved.vCameraDirection;
    Device.vCameraTop = saved.vCameraTop;
    Device.vCameraRight = saved.vCameraRight;
    Device.dwWidth = saved.dwWidth;
    Device.dwHeight = saved.dwHeight;
    GEnv.Render->SetCacheXform(Device.mView, Device.mProject);
    RCache.set_xform_view(Device.mView);
    RCache.set_xform_project(Device.mProject);

    return true;
}
} // namespace xray::render::RENDER_NAMESPACE
