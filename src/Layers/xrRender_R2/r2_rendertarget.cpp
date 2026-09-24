#include "stdafx.h"
#include "Layers/xrRender/ResourceManager.h"
#include "Layers/xrRender/blenders/blender_light_occq.h"
#include "Layers/xrRender/blenders/blender_light_mask.h"
#include "Layers/xrRender/blenders/blender_light_direct.h"
#include "Layers/xrRender/blenders/blender_light_point.h"
#include "Layers/xrRender/blenders/blender_light_spot.h"
#include "Layers/xrRender/blenders/blender_light_reflected.h"
#include "Layers/xrRender/blenders/blender_combine.h"
#include "Layers/xrRender/blenders/blender_bloom_build.h"
#include "Layers/xrRender/blenders/blender_luminance.h"
#include "Layers/xrRender/blenders/blender_ssao.h"

#include "Layers/xrRender/blenders/dx11MSAABlender.h"
#include "Layers/xrRender/blenders/dx11RainBlender.h"

#include "Layers/xrRender/blenders/dx11MinMaxSMBlender.h"
#if defined(USE_DX11)
#    include "Layers/xrRender/blenders/dx11HDAOCSBlender.h"
#endif

namespace xray::render::RENDER_NAMESPACE
{
void CRenderTarget::u_stencil_optimize(CBackend& cmd_list, eStencilOptimizeMode eSOM)
{
    PIX_EVENT(stencil_optimize);

#if defined(USE_DX11)
    // TODO: DX11: remove half pixel offset?
    VERIFY(RImplementation.o.nvstencil);
    u32 Offset;
    float _w = float(Device.dwWidth);
    float _h = float(Device.dwHeight);
    u32 C = color_rgba(255, 255, 255, 255);
    FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, g_combine->vb_stride, Offset);
    float eps = 0;
    float _dw = 0.5f;
    float _dh = 0.5f;
    pv->set(-_dw, _h - _dh, eps, 1.f, C, 0, 0);
    pv++;
    pv->set(-_dw, -_dh, eps, 1.f, C, 0, 0);
    pv++;
    pv->set(_w - _dw, _h - _dh, eps, 1.f, C, 0, 0);
    pv++;
    pv->set(_w - _dw, -_dh, eps, 1.f, C, 0, 0);
    pv++;
    RImplementation.Vertex.Unlock(4, g_combine->vb_stride);

    cmd_list.set_Element(s_occq->E[1]);

    switch (eSOM)
    {
    case SO_Light: cmd_list.StateManager.SetStencilRef(dwLightMarkerID); break;
    case SO_Combine: cmd_list.StateManager.SetStencilRef(0x01); break;
    default: VERIFY(!"CRenderTarget::u_stencil_optimize. switch no default!");
    }

    cmd_list.set_Geometry(g_combine);
    cmd_list.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);
#elif defined(USE_OGL)
    //	TODO: OGL: should we implement stencil optimization?
    VERIFY(RImplementation.o.nvstencil);
    VERIFY(!"CRenderTarget::u_stencil_optimize no implemented");
    UNUSED(eSOM);
#else
#   error No graphics API selected or enabled!
#endif // USE_DX11
}

// 2D texgen (texture adjustment matrix)
void CRenderTarget::u_compute_texgen_screen(CBackend& cmd_list, Fmatrix& m_Texgen)
{
#if defined(USE_DX11)
    Fmatrix m_TexelAdjust =
    {
        0.5f, 0.0f, 0.0f, 0.0f,
        0.0f, -0.5f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.5f, 0.5f, 0.0f, 1.0f
};
#elif defined(USE_OGL)
    Fmatrix m_TexelAdjust =
    {
        0.5f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.5f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.5f, 0.5f, 0.0f, 1.0f
    };
#else
#   error No graphics API selected or enabled!
#endif

    m_Texgen.mul(m_TexelAdjust, cmd_list.xforms.m_wvp);
}

// 2D texgen for jitter (texture adjustment matrix)
void CRenderTarget::u_compute_texgen_jitter(CBackend& cmd_list, Fmatrix& m_Texgen_J)
{
    // place into 0..1 space
    Fmatrix m_TexelAdjust =
    {
        0.5f, 0.0f, 0.0f, 0.0f,
#if defined(USE_DX11)
        0.0f, -0.5f, 0.0f, 0.0f,
#elif defined(USE_OGL)
        0.0f, 0.5f, 0.0f, 0.0f,
#else
#   error No graphics API selected or enabled!
#endif
        0.0f, 0.0f, 1.0f, 0.0f,
        0.5f, 0.5f, 0.0f, 1.0f
    };
    m_Texgen_J.mul(m_TexelAdjust, cmd_list.xforms.m_wvp);

    // rescale - tile it (scene pixels: downsized under r__render_scale < 1, else == Device)
    float scale_X = float(scene_width()) / float(TEX_jitter);
    float scale_Y = float(scene_height()) / float(TEX_jitter);
    m_TexelAdjust.scale(scale_X, scale_Y, 1.f);
    m_Texgen_J.mulA_44(m_TexelAdjust);
}

u8 fpack(float v)
{
    s32 _v = iFloor(((v + 1) * .5f) * 255.f + .5f);
    clamp(_v, 0, 255);
    return u8(_v);
}

u8 fpackZ(float v)
{
    s32 _v = iFloor(_abs(v) * 255.f + .5f);
    clamp(_v, 0, 255);
    return u8(_v);
}

Fvector vunpack(s32 x, s32 y, s32 z)
{
    Fvector pck;
    pck.x = (float(x) / 255.f - .5f) * 2.f;
    pck.y = (float(y) / 255.f - .5f) * 2.f;
    pck.z = -float(z) / 255.f;
    return pck;
}

Fvector vunpack(const Ivector& src)
{
    return vunpack(src.x, src.y, src.z);
}

Ivector vpack(const Fvector& src)
{
    Fvector _v;
    int bx = fpack(src.x);
    int by = fpack(src.y);
    int bz = fpackZ(src.z);
    // dumb test
    float e_best = flt_max;
    int r = bx, g = by, b = bz;
#ifdef DEBUG
    int d = 0;
#else
    int d = 3;
#endif
    for (int x = _max(bx - d, 0); x <= _min(bx + d, 255); x++)
        for (int y = _max(by - d, 0); y <= _min(by + d, 255); y++)
            for (int z = _max(bz - d, 0); z <= _min(bz + d, 255); z++)
            {
                _v = vunpack(x, y, z);
                float m = _v.magnitude();
                float me = _abs(m - 1.f);
                if (me > 0.03f)
                    continue;
                _v.div(m);
                float e = _abs(src.dotproduct(_v) - 1.f);
                if (e < e_best)
                {
                    e_best = e;
                    r = x, g = y, b = z;
                }
            }
    Ivector ipck;
    ipck.set(r, g, b);
    return ipck;
}

void manually_assign_texture(ref_shader& shader, pcstr textureName, pcstr rendertargetTextureName)
{
    SPass& pass = *shader->E[0]->passes[0];
    if (!pass.constants)
        return;

    const ref_constant constant = pass.constants->get(textureName);
    if (!constant)
        return;

    const auto index = constant->samp.index;
    pass.T->create_texture(index, rendertargetTextureName, false);
}

CRenderTarget::CRenderTarget()
{
    ZoneScoped;

    static constexpr pcstr SAMPLE_DEFS[] = { "0", "1", "2", "3", "4", "5", "6", "7" };

    if (!strstr(Core.Params, "-smap"))
        RImplementation.o.smapsize = ps_r2_smapsize;

    RImplementation.m_SMAPSize = RImplementation.o.smapsize;
    RImplementation.o.rain_smapsize = ps_r3_dyn_wet_surf_sm_res;

    const auto& options = RImplementation.o;

    const u32 SampleCount  = options.msaa ? options.msaa_samples : 1u;
    const u32 BoundSamples = options.msaa_opt ? 1u : options.msaa_samples;

#ifdef DEBUG
    Msg("MSAA samples = %d", SampleCount);
    if (options.msaa_opt)
        Msg("MSAA_opt = on");
    if (options.gbuffer_opt)
        Msg("gbuffer_opt = on");
#endif

    param_blur = 0.f;
    param_gray = 0.f;
    param_noise = 0.f;
    param_duality_h = 0.f;
    param_duality_v = 0.f;
    param_noise_fps = 25.f;
    param_noise_scale = 1.f;

    im_noise_time = 1.0f / 100.0f;
    im_noise_shift_w = 0;
    im_noise_shift_h = 0;

    param_color_base = color_rgba(127, 127, 127, 0);
    param_color_gray = color_rgba(85, 85, 85, 0);
    param_color_add.set(0.0f, 0.0f, 0.0f);

    dwAccumulatorClearMark = 0;
    RImplementation.Resources->Evict();

    // Blenders
    b_accum_spot = xr_new<CBlender_accum_spot>();

    if (options.msaa)
    {
        for (u32 i = 0; i < BoundSamples; ++i)
        {
            b_accum_spot_msaa[i] = xr_new<CBlender_accum_spot_msaa>("ISAMPLE", SAMPLE_DEFS[i]);
            b_accum_volumetric_msaa[i] = xr_new<CBlender_accum_volumetric_msaa>("ISAMPLE", SAMPLE_DEFS[i]);
        }
    }

    // NORMAL
    {
        u32 w = Device.dwWidth, h = Device.dwHeight;
        rt_Base.resize(HW.BackBufferCount);
        for (u32 i = 0; i < HW.BackBufferCount; i++)
        {
            string32 temp;
            xr_sprintf(temp, "%s%u", r2_RT_base, i);
            rt_Base[i].create(temp, w, h, HW.Caps.fTarget, 1, { CRT::CreateBase });
        }
        rt_Base_Depth.create(r2_RT_base_depth, w, h, HW.Caps.fDepth, 1, { CRT::CreateBase });

        if (!options.msaa)
            rt_MSAADepth = rt_Base_Depth;
        else
            rt_MSAADepth.create(r2_RT_MSAAdepth, w, h, D3DFMT_D24S8, SampleCount);

        rt_Position.create(r2_RT_P, w, h, D3DFMT_A16B16G16R16F, SampleCount);
        if (!options.gbuffer_opt)
            rt_Normal.create(r2_RT_N, w, h, D3DFMT_A16B16G16R16F, SampleCount);

        // select albedo & accum
        if (options.mrtmixdepth)
        {
            // NV50
            rt_Color.create(r2_RT_albedo, w, h, D3DFMT_A8R8G8B8, SampleCount);
            rt_Accumulator.create(r2_RT_accum, w, h, D3DFMT_A16B16G16R16F, SampleCount);
        }
        else
        {
            // can't - mix-depth
            if (options.fp16_blend)
            {
                // NV40
                if (!options.gbuffer_opt)
                {
                    rt_Color.create(r2_RT_albedo, w, h, D3DFMT_A16B16G16R16F, SampleCount); // expand to full
                    rt_Accumulator.create(r2_RT_accum, w, h, D3DFMT_A16B16G16R16F, SampleCount);
                }
                else
                {
                    rt_Color.create(r2_RT_albedo, w, h, D3DFMT_A8R8G8B8, SampleCount); // expand to full
                    rt_Accumulator.create(r2_RT_accum, w, h, D3DFMT_A16B16G16R16F, SampleCount);
                }
            }
            else
            {
                // R4xx, no-fp-blend,-> albedo_wo
                VERIFY(options.albedo_wo);
                rt_Color.create(r2_RT_albedo, w, h, D3DFMT_A8R8G8B8, SampleCount); // normal
                rt_Accumulator.create(r2_RT_accum, w, h, D3DFMT_A16B16G16R16F, SampleCount);
                rt_Accumulator_temp.create(r2_RT_accum_temp, w, h, D3DFMT_A16B16G16R16F, SampleCount);
            }
        }

        // generic(LDR) RTs
        rt_Generic_0.create(r2_RT_generic0, w, h, D3DFMT_A8R8G8B8, 1);
        rt_Generic_1.create(r2_RT_generic1, w, h, D3DFMT_A8R8G8B8, 1);
        rt_secondVP.create(r2_RT_secondVP, w, h, D3DFMT_A8R8G8B8, 1); // --#SM+#-- +SecondVP+
        rt_Generic.create(r2_RT_generic, w, h, D3DFMT_A8R8G8B8, 1);

        if (!options.msaa)
        {
            rt_Generic_0_r = rt_Generic_0;
            rt_Generic_1_r = rt_Generic_1;
        }
        else
        {
            rt_Generic_0_r.create(r2_RT_generic0_r, w, h, D3DFMT_A8R8G8B8, SampleCount);
            rt_Generic_1_r.create(r2_RT_generic1_r, w, h, D3DFMT_A8R8G8B8, SampleCount);
        }
        //	Igor: for volumetric lights
        // rt_Generic_2.create			(r2_RT_generic2,w,h,D3DFMT_A8R8G8B8		);
        //	temp: for higher quality blends
        if (options.advancedpp)
            rt_Generic_2.create(r2_RT_generic2, w, h, D3DFMT_A16B16G16R16F, SampleCount);
    }

    // OCCLUSION
    {
        CBlender_light_occq b_occq;
        s_occq.create(&b_occq, "r2" DELIMITER "occq");
    }

    // DIRECT (spot)
    pcstr smapTarget = r2_RT_smap_depth;
    {
        const u32 smapsize = options.smapsize;

        D3DFORMAT depth_format = D3DFMT_D24X8;
        D3DFORMAT surf_format = D3DFMT_R32F;

        Flags32 flags{};
        if (!options.HW_smap)
        {
            flags.flags = CRT::CreateSurface;
            smapTarget = r2_RT_smap_surf;
        }
        else
        {
            depth_format = (D3DFORMAT)options.HW_smap_FORMAT;
            if (options.nullrt) // use nullrt if possible
                surf_format = (D3DFORMAT)MAKEFOURCC('N', 'U', 'L', 'L');
            else
                surf_format = D3DFMT_R5G6B5;
        }

        // We only need to create rt_smap_surf on DX9, on DX10+ it's always a NULL render target
        // TODO: OGL: Don't create a color buffer for the shadow map.
#if defined(USE_OGL)
        rt_smap_surf.create(r2_RT_smap_surf, smapsize, smapsize, surf_format);
#endif

        // Create D3DFMT_D24X8 depth-stencil surface if HW smap is not supported,
        // otherwise - create texture with specified HW_smap_FORMAT
        const auto num_slices = RImplementation.o.support_rt_arrays ? R__NUM_SUN_CASCADES : 1;
        rt_smap_depth.create(r2_RT_smap_depth, smapsize, smapsize, depth_format, 1, num_slices, flags);
        rt_smap_rain.create(r2_RT_smap_rain, options.rain_smapsize, options.rain_smapsize, depth_format);
        if (options.minmax_sm)
        {
            rt_smap_depth_minmax.create(r2_RT_smap_depth_minmax, smapsize / 4, smapsize / 4, D3DFMT_R32F);
            CBlender_createminmax b_create_minmax;
            s_create_minmax_sm.create(&b_create_minmax, "null");
        }

        // Accum mask
        {
            CBlender_accum_direct_mask b_accum_mask;
            s_accum_mask.create(&b_accum_mask, "r2" DELIMITER "accum_mask");
        }

        // Accum direct
        {
#if RENDER == R_R2
            if (options.oldshadowcascades)
            {
                CBlender_accum_direct b_accum_direct;
                s_accum_direct.create(&b_accum_direct, "r2" DELIMITER "accum_direct");
            }
            else
            {
                CBlender_accum_direct_cascade b_accum_direct;
                s_accum_direct.create(&b_accum_direct, "r2" DELIMITER "accum_direct_cascade");
            }
#else
            CBlender_accum_direct b_accum_direct;
            s_accum_direct.create(&b_accum_direct, "r2" DELIMITER "accum_direct");
#endif // RENDER == R_R2
        }

        // Accum direct/mask MSAA
        if (options.msaa)
        {
            for (u32 i = 0; i < BoundSamples; ++i)
            {
                CBlender_accum_direct_msaa b_accum_direct_msaa{ "ISAMPLE", SAMPLE_DEFS[i] };
                s_accum_direct_msaa[i].create(&b_accum_direct_msaa, "r2" DELIMITER "accum_direct");
                CBlender_accum_direct_mask_msaa b_accum_mask_msaa{ "ISAMPLE", SAMPLE_DEFS[i] };
                s_accum_mask_msaa[i].create(&b_accum_mask_msaa, "r2" DELIMITER "accum_direct");
            }
        }

        // Accum volumetric
        if (options.advancedpp)
        {
            s_accum_direct_volumetric.create("accum_volumetric_sun_nomsaa");
            manually_assign_texture(s_accum_direct_volumetric, "s_smap", smapTarget);

            if (options.minmax_sm)
            {
                s_accum_direct_volumetric_minmax.create("accum_volumetric_sun_nomsaa_minmax");
                manually_assign_texture(s_accum_direct_volumetric_minmax, "s_smap", smapTarget);
            }

            if (options.msaa)
            {
                static constexpr pcstr snames[] =
                {
                    "accum_volumetric_sun_msaa0", "accum_volumetric_sun_msaa1",
                    "accum_volumetric_sun_msaa2", "accum_volumetric_sun_msaa3",
                    "accum_volumetric_sun_msaa4", "accum_volumetric_sun_msaa5",
                    "accum_volumetric_sun_msaa6", "accum_volumetric_sun_msaa7"
                };

                for (u32 i = 0; i < BoundSamples; ++i)
                {
                    // CBlender_accum_direct_volumetric_sun_msaa b_accum_direct_volumetric_sun_msaa{ "ISAMPLE", SAMPLE_DEFS[i] };
                    // s_accum_direct_volumetric_msaa[i].create(&b_accum_direct_volumetric_sun_msaa, "r2" DELIMITER "accum_direct");
                    s_accum_direct_volumetric_msaa[i].create(snames[i]);
                    manually_assign_texture(s_accum_direct_volumetric_msaa[i], "s_smap", smapTarget);
                }
            }
        }
    }

    // RAIN
    // TODO: DX11: Create resources only when DX11 rain is enabled.
    // Or make DX11 rain switch dynamic?
    {
        CBlender_rain b_rain;
        s_rain.create(&b_rain, "null");

        if (options.msaa)
        {
            for (u32 i = 0; i < BoundSamples; ++i)
            {
                CBlender_combine_msaa b_combine_msaa{ "ISAMPLE", SAMPLE_DEFS[i] };
                s_combine_msaa[i].create(&b_combine_msaa, "r2" DELIMITER "combine");

                CBlender_rain_msaa b_rain_msaa{ "ISAMPLE", SAMPLE_DEFS[i] };
                s_rain_msaa[i].create(&b_rain_msaa, "null");

                CBlender_accum_point_msaa b_accum_point_msaa{ "ISAMPLE", SAMPLE_DEFS[i] };
                s_accum_point_msaa[i].create(&b_accum_point_msaa, "r2" DELIMITER "accum_point_s");

                s_accum_spot_msaa[i].create(b_accum_spot_msaa[i], "r2" DELIMITER "accum_spot_s", "lights" DELIMITER "lights_spot01");

                // CBlender_accum_direct_volumetric_msaa b_accum_direct_volumetric_msaa{ "ISAMPLE", SAMPLE_DEFS[i] };
                // s_accum_volume_msaa[i].create(&b_accum_direct_volumetric_msaa, "lights" DELIMITER "lights_spot01");
                s_accum_volume_msaa[i].create(b_accum_volumetric_msaa[i], "lights" DELIMITER "lights_spot01");
            }
        }
    }

    if (options.msaa)
    {
        CBlender_msaa b_msaa;
        s_mark_msaa_edges.create(&b_msaa, "null");
    }

    // POINT
    {
        CBlender_accum_point b_accum_point;
        s_accum_point.create(&b_accum_point, "r2" DELIMITER "accum_point_s");
        accum_point_geom_create();
        g_accum_point.create(D3DFVF_XYZ, g_accum_point_vb, g_accum_point_ib);
        accum_omnip_geom_create();
        g_accum_omnipart.create(D3DFVF_XYZ, g_accum_omnip_vb, g_accum_omnip_ib);
    }

    // SPOT
    {
        s_accum_spot.create(b_accum_spot, "r2" DELIMITER "accum_spot_s", "lights" DELIMITER "lights_spot01");
        accum_spot_geom_create();
        g_accum_spot.create(D3DFVF_XYZ, g_accum_spot_vb, g_accum_spot_ib);

        // NPC blob shadow impostor: unit cube geometry for cheap SMAP casters
        npc_blob_geom_create();
        g_npc_blob.create(D3DFVF_XYZ, g_npc_blob_vb, g_npc_blob_ib);
    }

    // SPOT VOLUMETRIC
    if (options.advancedpp)
    {
        s_accum_volume.create("accum_volumetric", "lights" DELIMITER "lights_spot01");
        manually_assign_texture(s_accum_volume, "s_smap", smapTarget);
        accum_volumetric_geom_create();
        g_accum_volumetric.create(D3DFVF_XYZ, g_accum_volumetric_vb, g_accum_volumetric_ib);
    }

    // REFLECTED
    {
        CBlender_accum_reflected b_accum_reflected;
        s_accum_reflected.create(&b_accum_reflected, "r2" DELIMITER "accum_refl");
        if (options.msaa)
        {
            for (u32 i = 0; i < BoundSamples; ++i)
            {
                CBlender_accum_reflected_msaa b_accum_reflected_msaa{ "ISAMPLE", SAMPLE_DEFS[i] };
                s_accum_reflected_msaa[i].create(&b_accum_reflected_msaa, "null");
            }
        }
    }

    // BLOOM
    {
        D3DFORMAT fmt = D3DFMT_A8R8G8B8; // D3DFMT_X8R8G8B8;
        u32 w = BLOOM_size_X, h = BLOOM_size_Y;
        constexpr u32 fvf_build = D3DFVF_XYZRHW | D3DFVF_TEX4 | D3DFVF_TEXCOORDSIZE2(0) | D3DFVF_TEXCOORDSIZE2(1) |
            D3DFVF_TEXCOORDSIZE2(2) | D3DFVF_TEXCOORDSIZE2(3);
        constexpr u32 fvf_filter = (u32)D3DFVF_XYZRHW | D3DFVF_TEX8 | D3DFVF_TEXCOORDSIZE4(0) | D3DFVF_TEXCOORDSIZE4(1) |
            D3DFVF_TEXCOORDSIZE4(2) | D3DFVF_TEXCOORDSIZE4(3) | D3DFVF_TEXCOORDSIZE4(4) | D3DFVF_TEXCOORDSIZE4(5) |
            D3DFVF_TEXCOORDSIZE4(6) | D3DFVF_TEXCOORDSIZE4(7);
        rt_Bloom_1.create(r2_RT_bloom1, w, h, fmt);
        rt_Bloom_2.create(r2_RT_bloom2, w, h, fmt);
        g_bloom_build.create(fvf_build, RImplementation.Vertex.Buffer(), RImplementation.QuadIB);
        g_bloom_filter.create(fvf_filter, RImplementation.Vertex.Buffer(), RImplementation.QuadIB);
        s_bloom_dbg_1.create("effects" DELIMITER "screen_set", r2_RT_bloom1);
        s_bloom_dbg_2.create("effects" DELIMITER "screen_set", r2_RT_bloom2);

        CBlender_bloom_build b_bloom;
        s_bloom.create(&b_bloom, "r2" DELIMITER "bloom");
        if (!options.msaa)
            s_bloom_msaa = s_bloom;
        else
        {
            CBlender_bloom_build_msaa b_bloom_msaa;
            s_bloom_msaa.create(&b_bloom_msaa, "r2" DELIMITER "bloom");
        }
        f_bloom_factor = 0.5f;
    }

    // Check if SSAO Ultra is allowed
    if (ps_r_ssao_mode != ssao_mode_hdao || !options.ssao_ultra)
        ps_r_ssao = _min(ps_r_ssao, 3);

    // HBAO
    if (options.ssao_opt_data)
    {
        u32 w = 0;
        u32 h = 0;
        if (options.ssao_half_data)
        {
            w = Device.dwWidth / 2;
            h = Device.dwHeight / 2;
        }
        else
        {
            w = Device.dwWidth;
            h = Device.dwHeight;
        }

        D3DFORMAT fmt = HW.Caps.id_vendor == 0x10DE ? D3DFMT_R32F : D3DFMT_R16F;
        rt_half_depth.create(r2_RT_half_depth, w, h, fmt);

        CBlender_SSAO_noMSAA b_ssao;
        s_ssao.create(&b_ssao, "r2" DELIMITER "ssao");
    }

    // HDAO/SSAO
    const bool ssao_blur_on = options.ssao_blur_on;
    const bool ssao_hdao_ultra = options.ssao_hdao && options.ssao_ultra && ps_r_ssao > 3;

    if (ssao_blur_on || ssao_hdao_ultra)
    {
        const u32 w = Device.dwWidth, h = Device.dwHeight;

        if (ssao_hdao_ultra)
        {
#if defined(USE_DX11) // XXX: support compute shaders for OpenGL
            if (options.msaa)
            {
                CBlender_CS_HDAO_MSAA b_hdao_msaa_cs;
                s_hdao_cs.create(&b_hdao_msaa_cs, "r2" DELIMITER "ssao");
            }
            else
            {
                CBlender_CS_HDAO b_hdao_cs;
                s_hdao_cs.create(&b_hdao_cs, "r2" DELIMITER "ssao");
            }
            rt_ssao_temp.create(r2_RT_ssao_temp, w, h, D3DFMT_R16F, 1, { CRT::CreateUAV });
#endif
        }
        else if (ssao_blur_on)
        {
            CBlender_SSAO_noMSAA b_ssao;
            s_ssao.create(&b_ssao, "r2" DELIMITER "ssao");

            // Should be used in r*_rendertarget_phase_ssao.cpp but it's commented there.
            /*if (options.msaa)
            {
                for (u32 i = 0; i < BoundSamples; ++i)
                {
                    CBlender_SSAO_MSAA b_ssao_msaa{ "ISAMPLE", SAMPLE_DEFS[i] };
                    s_ssao_msaa[i].create(&b_ssao_msaa, "null");
                }
            }*/
            rt_ssao_temp.create(r2_RT_ssao_temp, w, h, D3DFMT_G16R16F, SampleCount);
        }
    }

    // TONEMAP
    {
        rt_LUM_64.create(r2_RT_luminance_t64, 64, 64, D3DFMT_A16B16G16R16F);
        rt_LUM_8.create(r2_RT_luminance_t8, 8, 8, D3DFMT_A16B16G16R16F);

        CBlender_luminance b_luminance;
        s_luminance.create(&b_luminance, "r2" DELIMITER "luminance");
        f_luminance_adapt = 0.5f;

        t_LUM_src.create(r2_RT_luminance_src);
        t_LUM_dest.create(r2_RT_luminance_cur);

        // create pool
        for (u32 it = 0; it < HW.Caps.iGPUNum * 2; it++)
        {
            string256 name;
            xr_sprintf(name, "%s_%d", r2_RT_luminance_pool, it);
            rt_LUM_pool[it].create(name, 1, 1, D3DFMT_R32F);
            RCache.ClearRT(rt_LUM_pool[it], 0x7f7f7f7f);
        }
        u_setrt(RCache, Device.dwWidth, Device.dwHeight, get_base_rt(), 0, 0, get_base_zb());
    }

    // COMBINE
    {
        static D3DVERTEXELEMENT9 dwDecl[] =
        {
            { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 }, // pos+uv
            D3DDECL_END()
        };

        CBlender_combine b_combine;
        s_combine.create(&b_combine, "r2" DELIMITER "combine");
        s_combine_volumetric.create("combine_volumetric");
        s_combine_dbg_0.create("effects" DELIMITER "screen_set", r2_RT_smap_surf);
        s_combine_dbg_1.create("effects" DELIMITER "screen_set", r2_RT_luminance_t8);
        s_combine_dbg_Accumulator.create("effects" DELIMITER "screen_set", r2_RT_accum);
        g_combine_VP.create(dwDecl, RImplementation.Vertex.Buffer(), RImplementation.QuadIB);
        g_combine.create(FVF::F_TL, RImplementation.Vertex.Buffer(), RImplementation.QuadIB);
        g_combine_2UV.create(FVF::F_TL2uv, RImplementation.Vertex.Buffer(), RImplementation.QuadIB);
        g_combine_cuboid.create(dwDecl, RImplementation.Vertex.Buffer(), RImplementation.Index.Buffer());

        constexpr u32 fvf_aa_blur = D3DFVF_XYZRHW | D3DFVF_TEX4 | D3DFVF_TEXCOORDSIZE2(0) | D3DFVF_TEXCOORDSIZE2(1) |
            D3DFVF_TEXCOORDSIZE2(2) | D3DFVF_TEXCOORDSIZE2(3);
        g_aa_blur.create(fvf_aa_blur, RImplementation.Vertex.Buffer(), RImplementation.QuadIB);

        constexpr u32 fvf_aa_AA = D3DFVF_XYZRHW | D3DFVF_TEX7 | D3DFVF_TEXCOORDSIZE2(0) | D3DFVF_TEXCOORDSIZE2(1) |
            D3DFVF_TEXCOORDSIZE2(2) | D3DFVF_TEXCOORDSIZE2(3) | D3DFVF_TEXCOORDSIZE2(4) | D3DFVF_TEXCOORDSIZE4(5) |
            D3DFVF_TEXCOORDSIZE4(6);
        g_aa_AA.create(fvf_aa_AA, RImplementation.Vertex.Buffer(), RImplementation.QuadIB);
    }

    // Build textures
    build_textures();

    // PP
    s_postprocess.create("postprocess");
    // Main render scale: copies the downsized scene depth ($user$mr_base_depth, created lazily by
    // MainScaleBegin) into the full-size backbuffer depth, see phase_pp.
    s_depth_upscale.create("depth_upscale");
    g_postprocess.create(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX3,
        RImplementation.Vertex.Buffer(), RImplementation.QuadIB);
    if (!options.msaa)
        s_postprocess_msaa = s_postprocess;
    else
    {
        CBlender_postprocess_msaa b_postprocess_msaa;
        s_postprocess_msaa.create(&b_postprocess_msaa, "r2" DELIMITER "post");
    }

    // Menu
    s_menu.create("distort");
    g_menu.create(FVF::F_TL, RImplementation.Vertex.Buffer(), RImplementation.QuadIB);

#if 0 // OpenGL: kept for historical reasons
    // Flip
    t_base = RImplementation.Resources->_CreateTexture(r2_base);
    t_base->surface_set(GL_TEXTURE_2D, get_base_rt());
    s_flip.create("effects" DELIMITER "screen_set", r2_base);
    g_flip.create(FVF::F_TL, RImplementation.Vertex.Buffer(), RImplementation.QuadIB);
#endif

    //
    for (int id = 0; id < R__NUM_CONTEXTS; ++id)
    {
        dwWidth[id] = Device.dwWidth;
        dwHeight[id] = Device.dwHeight;
    }
}

CRenderTarget::~CRenderTarget()
{
#if defined(USE_DX11)
    _RELEASE(t_ss_async);
    _RELEASE(dbg_lum_staging);
    for (ID3DTexture2D*& staging : dbg_probe_staging)
        _RELEASE(staging);
#elif defined(USE_OGL)
    // Textures
    t_material->surface_set(GL_TEXTURE_3D, 0);
    glDeleteTextures(1, &t_material_surf);
    t_material.destroy();

    t_LUM_src->surface_set(GL_TEXTURE_2D, 0);
    t_LUM_dest->surface_set(GL_TEXTURE_2D, 0);
    t_LUM_src.destroy();
    t_LUM_dest.destroy();

    // Jitter
    for (u32 it = 0; it < TEX_jitter_count; it++)
    {
        t_noise[it]->surface_set(GL_TEXTURE_2D, 0);
    }
    glDeleteTextures(TEX_jitter_count, t_noise_surf);

    t_noise_mipped->surface_set(GL_TEXTURE_2D, 0);
    glDeleteTextures(1, &t_noise_surf_mipped);
#else
#   error No graphics API selected or enabled!
#endif
    //
    accum_spot_geom_destroy();
    accum_omnip_geom_destroy();
    accum_point_geom_destroy();
    accum_volumetric_geom_destroy();

    // Blenders
    xr_delete(b_accum_spot);
    if (RImplementation.o.msaa)
    {
        const u32 bound = RImplementation.o.msaa_opt ? 1 : RImplementation.o.msaa_samples;

        for (u32 i = 0; i < bound; ++i)
        {
            xr_delete(b_accum_spot_msaa[i]);
            xr_delete(b_accum_volumetric_msaa[i]);
        }
    }
}

void CRenderTarget::reset_light_marker(CBackend& cmd_list, bool bResetStencil)
{
    dwLightMarkerID = 5;
    if (bResetStencil)
    {
        u32 Offset;
        float _w = float(Device.dwWidth);
        float _h = float(Device.dwHeight);
        u32 C = color_rgba(255, 255, 255, 255);
        float eps = 0;
        float _dw = 0.5f;
        float _dh = 0.5f;
        FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, g_combine->vb_stride, Offset);
        pv->set(-_dw, _h - _dh, eps, 1.f, C, 0, 0);
        pv++;
        pv->set(-_dw, -_dh, eps, 1.f, C, 0, 0);
        pv++;
        pv->set(_w - _dw, _h - _dh, eps, 1.f, C, 0, 0);
        pv++;
        pv->set(_w - _dw, -_dh, eps, 1.f, C, 0, 0);
        pv++;
        RImplementation.Vertex.Unlock(4, g_combine->vb_stride);
        cmd_list.set_Element(s_occq->E[2]);
        cmd_list.set_Geometry(g_combine);
        cmd_list.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);
    }
}

void CRenderTarget::increment_light_marker(CBackend& cmd_list)
{
    dwLightMarkerID += 2;

    const u32 iMaxMarkerValue = RImplementation.o.msaa ? 127 : 255;

    if (dwLightMarkerID > iMaxMarkerValue)
    {
        // Mid-frame stencil wipe: the marker ran out of stencil values. The second viewport pass
        // shares this counter with the main pass (phase_accumulator only resets it on the FIRST
        // call of a frame), so a scope frame consumes it twice as fast. Counted for diagnostics.
        ++dbg_light_marker_overflows;
        reset_light_marker(cmd_list, true);
    }
}

bool CRenderTarget::need_to_render_sunshafts()
{
    if (!(RImplementation.o.advancedpp && ps_r_sun_shafts))
        return false;

    {
        const auto& env = g_pGamePersistent->Environment().CurrentEnv;
        const float fValue = env.m_fSunShaftsIntensity;
        // TODO: add multiplication by sun color here
        if (fValue < 0.0001)
            return false;
    }

    return true;
}

bool CRenderTarget::use_minmax_sm_this_frame()
{
    switch (RImplementation.o.minmax_sm)
    {
    case CRender::MMSM_ON: return true;
    case CRender::MMSM_AUTO: return need_to_render_sunshafts();
    case CRender::MMSM_AUTODETECT:
    {
        const auto& [width, height] = HW.GetSurfaceSize();
        u32 dwScreenArea = width * height;

        if (dwScreenArea >= RImplementation.o.minmax_sm_screenarea_threshold)
            return need_to_render_sunshafts();
        return false;
    }

    default: return false;
    }
}

void CRenderTarget::ResizeSecondVPRT(u32 w, u32 h)
{
    VERIFY(w && h);
    if (rt_secondVP && rt_secondVP->dwWidth == w && rt_secondVP->dwHeight == h)
        return;
    rt_secondVP.destroy();
    rt_secondVP.create(r2_RT_secondVP, w, h, D3DFMT_A8R8G8B8, 1); // --#SM+#-- +SecondVP+
}

// ---------------------------------------------------------------------------
// Scaled second-viewport pipeline (r__svp_scaled_pipeline, active while r__second_vp_render_scale < 1).
//
// The dedicated SVP pass normally renders the whole deferred chain at full screen resolution and
// only the final postprocess quad lands in the smaller rt_secondVP, so the scale cvar degraded
// image quality without improving FPS. The helpers below give that pass a parallel downsized RT set:
//   1. SVPTargetsEnsure() lazily creates the $user$sv_* twins, copying format/sample count from the
//      live targets so every creation path picked by the constructor stays supported.
//   2. SVPPipelineBegin() swaps the twins into the regular rt_* members - every u_setrt then binds
//      downsized surfaces and dwWidth/dwHeight (hence viewports) follow automatically - and
//      republishes the named "$user$..." textures under twin surfaces: deferred shaders sample
//      those names through the texture registry (same per-frame mechanism as t_LUM_src/dest).
//   3. SVPPipelineEnd() restores members and published surfaces. Live GPU state is intentionally
//      left untouched so draws issued right after the pass (bullet tracers) still land in the
//      currently bound rt_secondVP.
// Fullscreen quads inside the pass keep Device-sized vertex extents; with the smaller viewport they
// clip to sw×sh, and their normalized TCs keep sampling the downsized sources correctly.
// ---------------------------------------------------------------------------

// SVP wrappers: behavior identical to the historical svp_* implementation.
bool CRenderTarget::SVPTargetsEnsure(u32 w, u32 h) { return ScaledSetEnsure(svp_set, w, h); }
void CRenderTarget::SVPTargetsRelease() { ScaledSetRelease(svp_set); }
void CRenderTarget::SVPPipelineBegin() { ScaledSetBegin(svp_set); }
void CRenderTarget::SVPPipelineEnd() { ScaledSetEnd(svp_set); }
void CRenderTarget::svp_publish_surfaces(bool use_twins) { scaled_publish_surfaces(svp_set, use_twins); }

bool CRenderTarget::ScaledSetEnsure(ScaledTargetSet& set, u32 w, u32 h)
{
    if (set.Position && set.Position->dwWidth == w && set.Position->dwHeight == h)
        return true;

    VERIFY(!set.swapped);

    // Latch creation failures per size: without this a broken set would be re-attempted (and
    // log-spammed) on every frame.
    if (!set.Position && set.failed_w == w && set.failed_h == h)
        return false;

    ScaledSetRelease(set);

    const auto& options = RImplementation.o;
    const u32 SampleCount = options.msaa ? options.msaa_samples : 1u;

    string64 name;
    const auto twin_name = [&](pcstr suffix) -> pcstr
    {
        xr_sprintf(name, "%s%s", set.prefix, suffix);
        return name;
    };

    // Base color + depth are plain textures on purpose (no CRT::CreateBase): the real rt_Base wraps
    // backbuffer views, while these must be standalone w×h surfaces bound during the scaled pass.
    set.Base.resize(HW.BackBufferCount);
    for (u32 i = 0; i < HW.BackBufferCount; i++)
    {
        xr_sprintf(name, "%sbase_%u", set.prefix, i);
        set.Base[i].create(name, w, h, HW.Caps.fTarget, 1);
    }
    set.Base_Depth.create(twin_name("base_depth"), w, h, HW.Caps.fDepth, 1);
    if (!options.msaa)
        set.MSAADepth = set.Base_Depth;
    else
        set.MSAADepth.create(twin_name("msaadepth"), w, h, D3DFMT_D24S8, SampleCount);

    // Format + sample count copied from each live original so the mrtmixdepth / fp16_blend /
    // gbuffer_opt / albedo_wo / advancedpp variations are mirrored without duplicating ctor logic.
    auto create_twin = [&](ref_rt& twin, const ref_rt& orig, pcstr suffix) {
        if (orig)
            twin.create(twin_name(suffix), w, h, orig->fmt, orig->sampleCount);
    };
    create_twin(set.Position, rt_Position, "position");
    create_twin(set.Normal, rt_Normal, "normal");
    create_twin(set.Color, rt_Color, "albedo");
    create_twin(set.Accumulator, rt_Accumulator, "accum");
    create_twin(set.Accumulator_temp, rt_Accumulator_temp, "accum_temp");
    create_twin(set.Generic_0, rt_Generic_0, "generic0");
    create_twin(set.Generic_1, rt_Generic_1, "generic1");
    if (!options.msaa)
    {
        set.Generic_0_r = set.Generic_0;
        set.Generic_1_r = set.Generic_1;
    }
    else
    {
        create_twin(set.Generic_0_r, rt_Generic_0_r, "generic0_r");
        create_twin(set.Generic_1_r, rt_Generic_1_r, "generic1_r");
    }
    create_twin(set.Generic, rt_Generic, "generic");
    create_twin(set.Generic_2, rt_Generic_2, "generic2");

    // AO targets (main pass only: the scope pass skips SSAO and reads the main AO). Same size
    // ratio to the scene as the originals: rt_half_depth is scene/2 under ssao_half_data, and
    // rt_ssao_temp keeps its UAV when HDAO (compute) writes it.
    bool ao_ok = true;
    if (set.with_ao)
    {
        if (rt_half_depth)
        {
            const u32 hw = options.ssao_half_data ? _max(1u, w / 2) : w;
            const u32 hh = options.ssao_half_data ? _max(1u, h / 2) : h;
            set.half_depth.create(twin_name("half_depth"), hw, hh, rt_half_depth->fmt, rt_half_depth->sampleCount);
            ao_ok = ao_ok && !!set.half_depth;
        }
        if (rt_ssao_temp)
        {
            Flags32 flags{};
#if defined(USE_DX11)
            if (rt_ssao_temp->pUAView)
                flags.flags = CRT::CreateUAV;
#endif
            set.ssao_temp.create(twin_name("ssao_temp"), w, h, rt_ssao_temp->fmt, rt_ssao_temp->sampleCount, flags);
            ao_ok = ao_ok && !!set.ssao_temp;
#if defined(USE_DX11)
            ao_ok = ao_ok && (!flags.test(CRT::CreateUAV) || set.ssao_temp->pUAView);
#endif
        }
    }

    set.w = w;
    set.h = h;

    // Only the critical chain is mandatory; optional extras (Normal/Accumulator_temp/Generic*/
    // Generic_2) may legitimately stay absent exactly like on the main path.
    if (!set.Base[0] || !set.Base_Depth || !set.MSAADepth || !set.Position || !set.Color ||
        !set.Accumulator || !set.Generic_0 || !set.Generic_1 || !ao_ok)
    {
        Msg("! %s: failed to create %ux%u target set, falling back to full-res render", set.tag, w, h);
        ScaledSetRelease(set);
        set.failed_w = w;
        set.failed_h = h;
        return false;
    }

    set.failed_w = 0;
    set.failed_h = 0;
    Msg("%s: created %ux%u target set", set.tag, w, h);
    return true;
}

void CRenderTarget::ScaledSetRelease(ScaledTargetSet& set)
{
    VERIFY(!set.swapped);
    // VERIFY is compiled out in release. Releasing the twins while the swap is active leaves the
    // named textures pointing at destroyed surfaces: ScaledSetEnd skips every pair whose twin is
    // gone, so the restore never happens and the scene stays black. Say so loudly.
    if (set.swapped)
        Msg("! [%s] ScaledSetRelease() called while the target swap is ACTIVE - named surfaces will not be restored", set.tag);
    if (set.ao_published && !set.swapped)
    {
        scaled_publish_surfaces(set, false, true); // AO names back onto the originals' surfaces
        set.ao_published = false;
    }
    set.Base.clear();
    set.Base_Depth.destroy();
    set.MSAADepth.destroy();
    set.Position.destroy();
    set.Normal.destroy();
    set.Color.destroy();
    set.Accumulator.destroy();
    set.Accumulator_temp.destroy();
    set.Generic_0.destroy();
    set.Generic_1.destroy();
    set.Generic_0_r.destroy();
    set.Generic_1_r.destroy();
    set.Generic.destroy();
    set.Generic_2.destroy();
    set.half_depth.destroy();
    set.ssao_temp.destroy();
    set.w = 0;
    set.h = 0;
}

// ---------------------------------------------------------------------------
// Main render scale (r__render_scale < 1): the whole main deferred chain renders into mrs_set
// ($user$mr_*) and phase_pp stretches the result into the real backbuffer. Same swap mechanics
// as the scope pass; the two swaps never overlap (the scope pass runs after the main Render()).
// ---------------------------------------------------------------------------
bool CRenderTarget::MainScaleBegin(u32 w, u32 h)
{
    if (svp_set.swapped || mrs_set.swapped)
    {
        Msg("! [%s] Begin refused: a target swap is already active (svp=%d main=%d)", mrs_set.tag,
            svp_set.swapped ? 1 : 0, mrs_set.swapped ? 1 : 0);
        if (mrs_set.ao_published && !mrs_set.swapped)
        {
            scaled_publish_surfaces(mrs_set, false, true); // this frame's SSAO writes the originals
            mrs_set.ao_published = false;
        }
        return false;
    }
    if (!ScaledSetEnsure(mrs_set, w, h))
        return false;
    ScaledSetBegin(mrs_set);
    return true;
}

void CRenderTarget::MainScaleEnd()
{
    if (!mrs_set.swapped)
        return;
    ScaledSetEnd(mrs_set);
    // The scope pass skips SSAO and samples the MAIN pass's AO by name ($user$ssao_temp /
    // $user$half_depth, normalized TCs). This frame that AO lives in the twins - the originals
    // were not written - so keep those two names on the twins until the next Begin (which
    // republishes everything) or the release of the set (which restores them).
    scaled_publish_surfaces(mrs_set, true, true);
    mrs_set.ao_published = true;
}

void CRenderTarget::MainScaleRelease()
{
    // Back at full resolution: drop the downsized set (about a second G-buffer worth of VRAM).
    if (!mrs_set.swapped && (mrs_set.Position || mrs_set.failed_w))
    {
        const bool had_set = !!mrs_set.Position;
        ScaledSetRelease(mrs_set);
        mrs_set.failed_w = 0;
        mrs_set.failed_h = 0;
        if (had_set)
            Msg("%s: target set released", mrs_set.tag);
    }
}

// Main render scale only: the scope pass (svp_set) deliberately keeps its historical Device-based
// sizes here, so the tested SVP pipeline behaves exactly as before this feature.
u32 CRenderTarget::scene_width() const
{
    return mrs_set.swapped ? mrs_set.w : Device.dwWidth;
}

u32 CRenderTarget::scene_height() const
{
    return mrs_set.swapped ? mrs_set.h : Device.dwHeight;
}

void CRenderTarget::set_scene_viewport(CBackend& cmd_list)
{
    cmd_list.SetViewport({ 0.f, 0.f, float(scene_width()), float(scene_height()), 0.f, 1.f });
}

// ---------------------------------------------------------------------------
// Dedicated SVP shadow-map atlas (frame driver stage 1a). Mirrors the constructor's DIRECT(spot)
// block but sizes the page from r__svp_smap_size, so worker-recorded scope lighting packs its own
// SMAP_Allocator without touching LP_smap_pool / rt_smap_depth shared with the main pass. Like
// the rest of the svp_* set it is created lazily on the MAIN thread (the worker only consumes
// it); the same latch transparently re-creates it when the size cvar changes or a device reset
// invalidated the surfaces.
// ---------------------------------------------------------------------------
bool CRenderTarget::SVPSmapAtlasEnsure()
{
    // Shadow-transfer: the main pass copies WHOLE pages into atlas SLICES via
    // CopySubresourceRegion - that requires identical dimensions, and the shadow UV math
    // (accum_spot normalizes by o.smapsize over the MAIN pass's placements) demands the
    // main page size.
    const u32 page = RImplementation.o.smapsize;
    // Sun-reuse tail: three extra slices hold the copied sun cascades (base =
    // ps_r__svp_smap_pages), mirrored from the main atlas right after its Render/Sun - slice 0
    // of the main atlas is destroyed by the first spot page clear.
    const u32 num_slices = static_cast<u32>(ps_r__svp_smap_pages) + R__NUM_SUN_CASCADES;
    if (svp_rt_smap_depth && svp_rt_smap_depth->valid() && svp_smap_page_size == page &&
        svp_rt_smap_depth->dwWidth == page && svp_rt_smap_depth->n_slices == num_slices)
        return true;

    // Latch creation failures per size (same anti retry-spam pattern as SVPTargetsEnsure).
    if (!svp_rt_smap_depth && svp_smap_failed && svp_smap_page_size == page)
        return false;

#ifdef USE_DX11
    SVPSmapAtlasRelease();

    const auto& options = RImplementation.o;
    D3DFORMAT depth_format = D3DFMT_D24X8;
    Flags32 flags{};
    if (!options.HW_smap)
        flags.flags = CRT::CreateSurface;
    else
        depth_format = (D3DFORMAT)options.HW_smap_FORMAT;

    // Worker mode (default): single slice ON PURPOSE - the scope pass suppresses sun cascades
    // (ps_r__svp_skip_sun_csm), spots need no array, and a full-texture SRV keeps the by-name
    // republish in svp_publish_smap_atlas() free of per-slice SRV bookkeeping.
    // Transfer mode: one slice per page; the scope accumulation swaps the per-slice SRV before
    // each page's quads (the sun uses the same pattern in render_sun::accumulate_cascade).
    svp_rt_smap_depth.create("$user$sv_smap_depth", page, page, depth_format, 1, num_slices, flags);
    if (!svp_rt_smap_depth || !svp_rt_smap_depth->valid())
    {
        svp_smap_page_size = page; // key the failure latch to this size
        svp_smap_failed = true;
        Msg("! SVP frame driver: failed to create %ux%u shadow atlas, scope lighting stays inline", page, page);
        SVPSmapAtlasRelease();
        return false;
    }
    svp_smap_page_size = page;
    svp_smap_failed = false;
    Msg("SVP frame driver: created %ux%u shadow atlas (%u slice(s))", page, page, u32(num_slices));
    return true;
#else
    (void)page;
    return false; // GL: a single context has nothing to record into, the frame driver is DX11-only
#endif
}

void CRenderTarget::SVPSmapAtlasRelease()
{
    VERIFY(!svp_set.swapped);
    svp_rt_smap_depth.destroy();
    svp_smap_page_size = 0;
    svp_smap_failed = false;
}

bool CRenderTarget::svp_publish_smap_atlas(bool use_atlas)
{
#if defined(USE_DX11)
    if (!svp_rt_smap_depth || !svp_rt_smap_depth->valid() || !rt_smap_depth || !rt_smap_depth->pTexture ||
        !svp_rt_smap_depth->pSurface)
        return false;

    // Same mechanism as svp_publish_surfaces: deferred/accumulation shaders resolve s_smap by
    // the "$user$smap_depth" name through the texture registry; repoint that named texture at
    // the worker-built atlas for the duration of the scope accumulation, then self-restore.
    rt_smap_depth->pTexture->surface_set(use_atlas ? svp_rt_smap_depth->pSurface : rt_smap_depth->pSurface);
    // surface_set() finishes with set_slice(-1) => srv_all. For the 1-slice atlas that view is a
    // plain Texture2D SRV, which does NOT match the shader's Texture2DArray s_smap declaration -
    // samples read as fully occluded and the whole scope image goes dark. Force the per-slice
    // view instead: it is always created as a Texture2DArray SRV (ArraySize=1, FirstArraySlice=0),
    // which is exactly the atlas's only slice - and the slice the restored main texture is
    // expected to expose as well.
    rt_smap_depth->pTexture->set_slice(0);
    return true;
#else
    (void)use_atlas;
    return false; // frame driver is DX11-only
#endif
}

void CRenderTarget::scaled_publish_surfaces(const ScaledTargetSet& set, bool use_twins, bool ao_only)
{
#if defined(USE_DX11)
    auto publish = [](const ref_rt& named, const ref_rt& src) {
        // Rebinding invalidates cached views; the SRV is recreated lazily on next bind
        // (same fix as the rt_secondVP rebind in r2_hud_overlay.cpp).
        named->pTexture->surface_set(src->pSurface);
    };
#elif defined(USE_OGL)
    auto publish = [](const ref_rt& named, const ref_rt& src) { named->pTexture->surface_set(src->target, src->pRT); };
#else
#   error No graphics API selected or enabled!
#endif

    // Publishing a member against itself restores its own surface (use_twins == false branch).
    struct SVPNamedPair
    {
        const ref_rt* named;
        const ref_rt* twin;
    };
    const SVPNamedPair pairs[] = {
        { &rt_Position, &set.Position },
        { &rt_Normal, &set.Normal },
        { &rt_Color, &set.Color },
        { &rt_Accumulator, &set.Accumulator },
        { &rt_Accumulator_temp, &set.Accumulator_temp },
        { &rt_Generic_0, &set.Generic_0 },
        { &rt_Generic_1, &set.Generic_1 },
        { &rt_Generic_0_r, &set.Generic_0_r },
        { &rt_Generic_1_r, &set.Generic_1_r },
        { &rt_Generic, &set.Generic },
        { &rt_Generic_2, &set.Generic_2 },
        { &rt_half_depth, &set.half_depth }, // with_ao sets only (null twin -> skipped)
        { &rt_ssao_temp, &set.ssao_temp },
    };
    constexpr size_t ao_first = std::size(pairs) - 2; // the two AO pairs are last
    for (size_t i = ao_only ? ao_first : 0; i < std::size(pairs); ++i)
    {
        const auto& pair = pairs[i];
        const ref_rt& twin = *pair.twin;
        if (!twin || !twin->pTexture || !*pair.named || !(*pair.named)->pTexture)
            continue;
        publish(*pair.named, use_twins ? twin : *pair.named);
    }
}

void CRenderTarget::dbg_dump_state()
{
    Msg("~ [rt-state] frame %u, swap active=%d, accum_clear_mark=%u, light_marker=%u, marker_overflows=%u",
        Device.dwFrame, svp_set.swapped ? 1 : 0, dwAccumulatorClearMark, dwLightMarkerID, dbg_light_marker_overflows);

    // These feed rmNormal(): a zero here means every pass that relies on it rasterizes nothing.
    for (int id = 0; id < R__NUM_CONTEXTS; ++id)
    {
        Msg("~   dims[ctx %d] = %ux%u%s", id, dwWidth[id], dwHeight[id],
            (dwWidth[id] == 0 || dwHeight[id] == 0) ? "   <== ZERO" : "");
    }

    const auto row = [](const char* label, const ref_rt& rt)
    {
        if (!rt)
        {
            Msg("~   %-16s <null ref>", label);
            return;
        }
#if defined(USE_DX11)
        Msg("~   %-16s %ux%u valid=%d surface=%p rtv=%p slices=%u", label, rt->dwWidth, rt->dwHeight,
            rt->valid() ? 1 : 0, (void*)rt->pSurface, (void*)rt->pRT, rt->n_slices);
#else
        Msg("~   %-16s %ux%u valid=%d", label, rt->dwWidth, rt->dwHeight, rt->valid() ? 1 : 0);
#endif
    };

    row("rt_Base_Depth", rt_Base_Depth);
    row("rt_MSAADepth", rt_MSAADepth);
    row("rt_Position", rt_Position);
    row("rt_Normal", rt_Normal);
    row("rt_Color", rt_Color);
    row("rt_Accumulator", rt_Accumulator);
    row("rt_Accum_temp", rt_Accumulator_temp);
    row("rt_Generic_0", rt_Generic_0);
    row("rt_Generic_1", rt_Generic_1);
    row("rt_Generic_0_r", rt_Generic_0_r);
    row("rt_Generic_1_r", rt_Generic_1_r);
    row("rt_Generic", rt_Generic);
    row("rt_Generic_2", rt_Generic_2);
    row("rt_Bloom_1", rt_Bloom_1);
    row("rt_LUM_64", rt_LUM_64);
    row("rt_LUM_8", rt_LUM_8);
    row("rt_secondVP", rt_secondVP);
    row("rt_smap_depth", rt_smap_depth);
    for (u32 i = 0; i < rt_Base.size(); ++i)
    {
        string32 name;
        xr_sprintf(name, "rt_Base[%u]", i);
        row(name, rt_Base[i]);
    }

    Msg("~   svp twins: %ux%u", svp_set.w, svp_set.h);
    Msg("~   main render scale twins: %ux%u, swapped=%d", mrs_set.w, mrs_set.h, mrs_set.swapped ? 1 : 0);
    row("svp_set.Position", svp_set.Position);
    row("svp_set.Generic_0", svp_set.Generic_0);
    row("svp_set.Base_Depth", svp_set.Base_Depth);
    row("svp_smap_depth", svp_rt_smap_depth);
}

bool CRenderTarget::dbg_read_lum(u32 idx, float& value)
{
    value = 0.f;
#if defined(USE_DX11)
    if (idx >= HW.Caps.iGPUNum * 2 || !rt_LUM_pool[idx] || !rt_LUM_pool[idx]->pSurface)
        return false;
    ID3DTexture2D* src = rt_LUM_pool[idx]->pSurface;
    if (!dbg_lum_staging)
    {
        // Same format as the pool texture (CopyResource demands it); we only ever read 4 bytes.
        D3D11_TEXTURE2D_DESC desc{};
        src->GetDesc(&desc);
        desc.Width = 1;
        desc.Height = 1;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        if (FAILED(HW.pDevice->CreateTexture2D(&desc, nullptr, &dbg_lum_staging)) || !dbg_lum_staging)
        {
            dbg_lum_staging = nullptr;
            return false;
        }
    }
    auto ctx = HW.get_context(CHW::IMM_CTX_ID);
    ctx->CopyResource(dbg_lum_staging, src);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(dbg_lum_staging, 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData)
        return false;
    value = *static_cast<const float*>(mapped.pData);
    ctx->Unmap(dbg_lum_staging, 0);
    return true;
#else
    (void)idx;
    return false;
#endif
}

void CRenderTarget::dbg_probe_rt(const ref_rt& rt, u32& nonzero, u32& sampled)
{
    nonzero = 0;
    sampled = 0;
#if defined(USE_DX11)
    if (!rt || !rt->pSurface)
        return;

    D3D11_TEXTURE2D_DESC src_desc{};
    rt->pSurface->GetDesc(&src_desc);
    if (src_desc.SampleDesc.Count != 1 || src_desc.Width < 4 || src_desc.Height < 4)
        return; // multisampled surfaces need a Resolve first - not worth it for a probe

    // One 1x1 staging texture per source format (CopySubresourceRegion requires a match).
    ID3DTexture2D* staging = nullptr;
    int slot = -1;
    for (int i = 0; i < dbg_probe_staging_count; ++i)
    {
        if (dbg_probe_staging[i] && dbg_probe_fmt[i] == src_desc.Format)
        {
            staging = dbg_probe_staging[i];
            break;
        }
        if (!dbg_probe_staging[i] && slot < 0)
            slot = i;
    }
    if (!staging)
    {
        if (slot < 0)
            return;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 1;
        desc.Height = 1;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = src_desc.Format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(HW.pDevice->CreateTexture2D(&desc, nullptr, &staging)) || !staging)
            return;
        dbg_probe_staging[slot] = staging;
        dbg_probe_fmt[slot] = src_desc.Format;
    }

    // Spread the samples: a single centre pixel can be legitimately black in any scene.
    const u32 xs[5] = {src_desc.Width / 2, src_desc.Width / 4, (src_desc.Width * 3) / 4,
        src_desc.Width / 4, (src_desc.Width * 3) / 4};
    const u32 ys[5] = {src_desc.Height / 2, src_desc.Height / 4, src_desc.Height / 4,
        (src_desc.Height * 3) / 4, (src_desc.Height * 3) / 4};

    auto ctx = HW.get_context(CHW::IMM_CTX_ID);
    for (int i = 0; i < 5; ++i)
    {
        D3D11_BOX box{};
        box.left = xs[i];
        box.right = xs[i] + 1;
        box.top = ys[i];
        box.bottom = ys[i] + 1;
        box.front = 0;
        box.back = 1;
        ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, rt->pSurface, 0, &box);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData)
            continue;
        // Format agnostic: any non-zero byte in the pixel counts as "something was written".
        // 16 bytes covers every format used here (up to RGBA32F).
        const u8* bytes = static_cast<const u8*>(mapped.pData);
        bool any = false;
        for (int b = 0; b < 16; ++b)
            any |= (bytes[b] != 0);
        ctx->Unmap(staging, 0);

        ++sampled;
        if (any)
            ++nonzero;
    }
#else
    (void)rt;
#endif
}

void CRenderTarget::dbg_probe_dump(const char* label, const ref_rt& rt)
{
#if defined(USE_DX11)
    if (!rt || !rt->pSurface)
    {
        Msg("~   %-14s <no surface>", label);
        return;
    }

    D3D11_TEXTURE2D_DESC src_desc{};
    rt->pSurface->GetDesc(&src_desc);
    if (src_desc.SampleDesc.Count != 1 || src_desc.Width < 4 || src_desc.Height < 4)
    {
        Msg("~   %-14s %ux%u fmt=%u <not sampleable: %u samples>", label, src_desc.Width, src_desc.Height,
            u32(src_desc.Format), src_desc.SampleDesc.Count);
        return;
    }

    // Reuse the per-format staging texture created by dbg_probe_rt.
    ID3DTexture2D* staging = nullptr;
    int slot = -1;
    for (int i = 0; i < dbg_probe_staging_count; ++i)
    {
        if (dbg_probe_staging[i] && dbg_probe_fmt[i] == src_desc.Format)
        {
            staging = dbg_probe_staging[i];
            break;
        }
        if (!dbg_probe_staging[i] && slot < 0)
            slot = i;
    }
    if (!staging)
    {
        if (slot < 0)
        {
            Msg("~   %-14s <no staging slot left>", label);
            return;
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 1;
        desc.Height = 1;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = src_desc.Format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(HW.pDevice->CreateTexture2D(&desc, nullptr, &staging)) || !staging)
        {
            Msg("~   %-14s <staging creation failed for fmt=%u>", label, u32(src_desc.Format));
            return;
        }
        dbg_probe_staging[slot] = staging;
        dbg_probe_fmt[slot] = src_desc.Format;
    }

    const u32 xs[3] = {src_desc.Width / 2, src_desc.Width / 4, (src_desc.Width * 3) / 4};
    const u32 ys[3] = {src_desc.Height / 2, src_desc.Height / 3, (src_desc.Height * 2) / 3};

    auto ctx = HW.get_context(CHW::IMM_CTX_ID);
    string512 line;
    xr_sprintf(line, "~   %-14s %ux%u fmt=%u", label, src_desc.Width, src_desc.Height, u32(src_desc.Format));

    for (int i = 0; i < 3; ++i)
    {
        D3D11_BOX box{};
        box.left = xs[i];
        box.right = xs[i] + 1;
        box.top = ys[i];
        box.bottom = ys[i] + 1;
        box.back = 1;
        ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, rt->pSurface, 0, &box);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData)
        {
            xr_strcat(line, " | <map failed>");
            continue;
        }
        const u8* b = static_cast<const u8*>(mapped.pData);
        string64 hex;
        xr_sprintf(hex, " | %u,%u=%02x%02x%02x%02x%02x%02x%02x%02x", xs[i], ys[i],
            b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
        ctx->Unmap(staging, 0);
        xr_strcat(line, hex);
    }
    Msg("%s", line);
#else
    Msg("~   %-14s <probe unavailable on this renderer>", label);
    (void)rt;
#endif
}

int CRenderTarget::dbg_final_peak()
{
#if defined(USE_DX11)
    const u32 index = (HW.CurrentBackBuffer < rt_Base.size()) ? HW.CurrentBackBuffer : 0;
    if (index >= rt_Base.size() || !rt_Base[index] || !rt_Base[index]->pSurface)
        return -1;

    ID3DTexture2D* src = rt_Base[index]->pSurface;
    D3D11_TEXTURE2D_DESC src_desc{};
    src->GetDesc(&src_desc);
    if (src_desc.SampleDesc.Count != 1 || src_desc.Width < 8 || src_desc.Height < 8)
        return -1;

    ID3DTexture2D* staging = nullptr;
    int slot = -1;
    for (int i = 0; i < dbg_probe_staging_count; ++i)
    {
        if (dbg_probe_staging[i] && dbg_probe_fmt[i] == src_desc.Format)
        {
            staging = dbg_probe_staging[i];
            break;
        }
        if (!dbg_probe_staging[i] && slot < 0)
            slot = i;
    }
    if (!staging)
    {
        if (slot < 0)
            return -1;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 1;
        desc.Height = 1;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = src_desc.Format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(HW.pDevice->CreateTexture2D(&desc, nullptr, &staging)) || !staging)
            return -1;
        dbg_probe_staging[slot] = staging;
        dbg_probe_fmt[slot] = src_desc.Format;
    }

    // Nine points spread over the frame, skipping the screen edges where the HUD lives: the 2D UI
    // keeps drawing during the bug and would mask the very darkness we are looking for.
    static const float fx[9] = {0.5f, 0.3f, 0.7f, 0.5f, 0.35f, 0.65f, 0.5f, 0.4f, 0.6f};
    static const float fy[9] = {0.5f, 0.35f, 0.35f, 0.3f, 0.5f, 0.5f, 0.6f, 0.65f, 0.65f};

    auto ctx = HW.get_context(CHW::IMM_CTX_ID);
    int peak = 0;
    for (int i = 0; i < 9; ++i)
    {
        D3D11_BOX box{};
        box.left = u32(src_desc.Width * fx[i]);
        box.right = box.left + 1;
        box.top = u32(src_desc.Height * fy[i]);
        box.bottom = box.top + 1;
        box.back = 1;
        ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, src, 0, &box);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData)
            continue;
        const u8* b = static_cast<const u8*>(mapped.pData);
        // The swapchain buffer is an 8- or 10-bit UNORM format, so the raw bytes track brightness
        // closely enough to tell "there is a picture here" from "there is not".
        for (int k = 0; k < 4; ++k)
        {
            if (int(b[k]) > peak)
                peak = int(b[k]);
        }
        ctx->Unmap(staging, 0);
    }
    return peak;
#else
    return -1;
#endif
}

void CRenderTarget::dbg_probe_targets(const char* where)
{
#if defined(USE_DX11)
    // Raw pixel values along the whole chain. Read them in order: the first target whose pixels
    // stop carrying a picture is the stage that produced the black screen.
    //   position/color  - the G-buffer, written by the geometry pass;
    //   accumulator     - deferred lighting;
    //   generic/generic_0 - the LDR image phase_combine composes before postprocess;
    //   base            - the swapchain buffer that is actually presented.
    Msg("~ [probe] %s frame %u, backbuffer index %u of %u", where, Device.dwFrame, HW.CurrentBackBuffer,
        u32(rt_Base.size()));
    dbg_probe_dump("position", rt_Position);
    dbg_probe_dump("color", rt_Color);
    dbg_probe_dump("accumulator", rt_Accumulator);
    dbg_probe_dump("generic", rt_Generic);
    dbg_probe_dump("generic_0", rt_Generic_0);
    for (u32 i = 0; i < rt_Base.size(); ++i)
    {
        string32 name;
        xr_sprintf(name, "base[%u]%s", i, (i == HW.CurrentBackBuffer) ? "*" : "");
        dbg_probe_dump(name, rt_Base[i]);
    }
#else
    (void)where;
#endif
}

void CRenderTarget::dbg_reset_lum()
{
    // Mirrors the startup clear in CRenderTarget::create (0x7f -> ~0.498 for R32F).
    for (u32 it = 0; it < HW.Caps.iGPUNum * 2; it++)
        if (rt_LUM_pool[it])
            RCache.ClearRT(rt_LUM_pool[it], 0x7f7f7f7f);
}

u32 CRenderTarget::svp_dbg_check_named(bool dump)
{
#if defined(USE_DX11)
    struct Entry
    {
        const ref_rt* named;
        const ref_rt* twin;
        const char* label;
    };
    const Entry entries[] = {
        { &rt_Position, &svp_set.Position, "position" },
        { &rt_Normal, &svp_set.Normal, "normal" },
        { &rt_Color, &svp_set.Color, "color" },
        { &rt_Accumulator, &svp_set.Accumulator, "accum" },
        { &rt_Accumulator_temp, &svp_set.Accumulator_temp, "accum_temp" },
        { &rt_Generic_0, &svp_set.Generic_0, "generic_0" },
        { &rt_Generic_1, &svp_set.Generic_1, "generic_1" },
        { &rt_Generic_0_r, &svp_set.Generic_0_r, "generic_0_r" },
        { &rt_Generic_1_r, &svp_set.Generic_1_r, "generic_1_r" },
        { &rt_Generic, &svp_set.Generic, "generic" },
        { &rt_Generic_2, &svp_set.Generic_2, "generic_2" },
    };

    u32 bad = 0;
    for (const auto& e : entries)
    {
        const ref_rt& named = *e.named;
        if (!named || !named->pTexture)
            continue;
        ID3DBaseTexture* cur = named->pTexture->surface_get(); // AddRef'd
        const auto own = static_cast<ID3DBaseTexture*>(named->pSurface);
        const ref_rt& twin = *e.twin;
        const auto twin_surf = (twin && twin->pSurface) ? static_cast<ID3DBaseTexture*>(twin->pSurface) : nullptr;
        const bool mismatch = (cur != own);
        if (mismatch)
            ++bad;
        if (dump || mismatch)
        {
            Msg("%s [svp-surf] %-12s named=%p own=%p twin=%p%s", mismatch ? "!" : "~", e.label,
                (void*)cur, (void*)own, (void*)twin_surf,
                !mismatch ? "" : (cur == twin_surf ? "  <== STILL ON THE TWIN" : "  <== FOREIGN SURFACE"));
        }
        _RELEASE(cur);
    }
    return bad;
#else
    (void)dump;
    return 0;
#endif
}

void CRenderTarget::ScaledSetBegin(ScaledTargetSet& set)
{
    VERIFY(!svp_set.swapped && !mrs_set.swapped);
    VERIFY(set.Position && set.w && set.h);

    // Publish FIRST, while *pairs.named still holds the originals: deferred shaders resolve
    // G-buffer inputs by texture name ($user$position etc.), and those named textures belong to
    // the original CRTs - they must be repointed at the twin surfaces before any draw happens.
    scaled_publish_surfaces(set, true);

    set.orig_Base = rt_Base;
    set.orig_Base_Depth = rt_Base_Depth;

    set.saved.clear();
    auto swap_in = [&set](ref_rt& member, const ref_rt& twin) {
        if (!twin)
            return;
        set.saved.emplace_back(&member, member);
        member = twin;
    };

    swap_in(rt_Position, set.Position);
    swap_in(rt_Normal, set.Normal);
    swap_in(rt_Color, set.Color);
    swap_in(rt_Accumulator, set.Accumulator);
    swap_in(rt_Accumulator_temp, set.Accumulator_temp);
    swap_in(rt_Generic_0, set.Generic_0);
    swap_in(rt_Generic_1, set.Generic_1);
    swap_in(rt_Generic_0_r, set.Generic_0_r);
    swap_in(rt_Generic_1_r, set.Generic_1_r);
    swap_in(rt_Generic, set.Generic);
    swap_in(rt_Generic_2, set.Generic_2);
    swap_in(rt_half_depth, set.half_depth);
    swap_in(rt_ssao_temp, set.ssao_temp);
    swap_in(rt_Base_Depth, set.Base_Depth);
    swap_in(rt_MSAADepth, set.MSAADepth);
    for (u32 i = 0; i < set.Base.size(); ++i)
        if (i < rt_Base.size())
            swap_in(rt_Base[i], set.Base[i]);

    // SVP only: accumulation targets are cleared by the engine only ONCE per frame
    // (dwAccumulatorClearMark / m_bHasActiveVolumetric are already consumed by the main pass),
    // while this second pass binds the twins holding LAST scope frame's contents. Zero them here
    // or scope lighting/volumetrics add onto the previous frame - the lens image progressively
    // washes out to white. The main pass owns those once-per-frame clears itself.
    if (set.clear_on_begin)
    {
        RCache.ClearRT(rt_Accumulator, {});
        if (rt_Accumulator_temp)
            RCache.ClearRT(rt_Accumulator_temp, {});
        if (rt_Generic_2)
            RCache.ClearRT(rt_Generic_2, {}); // volumetric lights sum within one pass, starting from black
    }

    set.saved_w = dwWidth[RCache.context_id];
    set.saved_h = dwHeight[RCache.context_id];
    dwWidth[RCache.context_id] = set.w;
    dwHeight[RCache.context_id] = set.h;

    set.swapped = true;
}

void CRenderTarget::ScaledSetEnd(ScaledTargetSet& set)
{
    VERIFY(set.swapped);

    for (const auto& saved : set.saved)
        *saved.first = saved.second;
    set.saved.clear();

    // Point the named textures back at the restored originals' own surfaces.
    scaled_publish_surfaces(set, false);

    dwWidth[RCache.context_id] = set.saved_w;
    dwHeight[RCache.context_id] = set.saved_h;

    set.orig_Base.clear();
    set.orig_Base_Depth.destroy(); // drops the extra reference only, the original stays in rt_Base_Depth

    set.swapped = false;
}
} // namespace xray::render::RENDER_NAMESPACE
