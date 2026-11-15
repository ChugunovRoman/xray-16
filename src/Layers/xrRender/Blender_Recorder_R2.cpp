#include "stdafx.h"
#pragma hdrstop

#include "ResourceManager.h"
#include "Blender_Recorder.h"
#include "Blender.h"

namespace xray::render::RENDER_NAMESPACE
{
void fix_texture_name(pstr fn);

namespace
{
// GPU instancing (plans/render_instancing/plan.md): the vertex shaders with an INSTANCED variant (gw_instancing.h of
// the r3 and gl shaders) - the rigid model shaders of uber_deffer(C, .., "model", ..) and the shadow passes of the
// model blenders. Not the tessellated ones (r_TessPass, another r_Pass) nor the forward/transparent model_def_lq.
bool instancing_vs_supported(pcstr vs)
{
    static constexpr pcstr names[] = { "deffer_model_flat", "deffer_model_flat_d", "deffer_model_bump",
        "deffer_model_bump_d", "deffer_model_bump-hq", "deffer_model_bump_d-hq", "shadow_direct_model",
        "shadow_direct_model_aref" };
    for (pcstr name : names)
    {
        if (0 == xr_stricmp(vs, name))
            return true;
    }
    return false;
}
} // namespace

// The twin of the pass r_Pass records: the same pixel shader and (in r_End) the same state and textures, the INSTANCED
// variant of the vertex shader. Only for rigid meshes: no skinning variant (Fvisual, FProgressive) or SKIN_0 (the
// one-bone RM_SINGLE children of skeletons) - the palette-skinned variants SKIN_1..4 get no twin.
void CBlender_Compile::r_PassInstancedTwin(LPCSTR _vs, LPCSTR _ps, u32 vs_flags)
{
    bInstTwin = false;
    // r__instancing 0 (the emergency switch, read when the shader is created): no twin at all - no INSTANCED vertex
    // shader to compile (its failure would be CHECK_OR_EXIT), the passes are exactly the ones without instancing.
    // Switched on again in the game, it applies to the shaders created after that (all of them after a restart).
    if (!ps_r__instancing || !instancing_vs_supported(_vs) || RImplementation.shader_skinning() > 0)
        return;
#if defined(USE_OGL)
    // Without ARB_separate_shader_objects the pass is one linked program, and the stage of each sampler is its index
    // among the active uniforms of that program (R_constant_table::parse, RC_dest_all). The twin program has other
    // vertex uniforms (gw_inst instead of m_WVP...), so its sampler stages differ from those of dest.T and the sampler
    // states of r_End, which the twin shares: the textures would land on the wrong samplers. No twin then.
    if (!GLAD_GL_ARB_separate_shader_objects)
        return;
#endif

    ctable_inst.clear();
    dest_inst.ps = nullptr;
    dest_inst.vs = nullptr;
    dest_inst.gs = nullptr;
#if defined(USE_DX11)
    dest_inst.hs = nullptr;
    dest_inst.ds = nullptr;
    dest_inst.cs = nullptr;
#elif defined(USE_OGL)
    dest_inst.pp = nullptr;
#endif

    RImplementation.shader_option_instanced(true); // the "_i" names, the INSTANCED define
#if defined(USE_OGL)
    dest_inst.pp = RImplementation.Resources->_CreatePP(_vs, _ps, "null", "null", "null");
    if (GLAD_GL_ARB_separate_shader_objects || !dest_inst.pp->pp)
#endif
    {
        dest_inst.ps = RImplementation.Resources->_CreatePS(_ps);
        ctable_inst.merge(&dest_inst.ps->constants);
        dest_inst.vs = RImplementation.Resources->_CreateVS(_vs, vs_flags);
        ctable_inst.merge(&dest_inst.vs->constants);
        dest_inst.gs = RImplementation.Resources->_CreateGS("null");
#ifdef USE_DX11
        dest_inst.hs = RImplementation.Resources->_CreateHS("null");
        dest_inst.ds = RImplementation.Resources->_CreateDS("null");
        dest_inst.cs = RImplementation.Resources->_CreateCS("null");
#endif
    }
#if defined(USE_OGL)
    RImplementation.Resources->_LinkPP(dest_inst);
    ctable_inst.merge(&dest_inst.pp->constants);
#endif
    RImplementation.shader_option_instanced(false);
    bInstTwin = true;
}

void CBlender_Compile::r_Pass(LPCSTR _vs, LPCSTR _ps, bool bFog, BOOL bZtest, BOOL bZwrite,
    BOOL bABlend, D3DBLEND abSRC, D3DBLEND abDST, BOOL aTest, u32 aRef)
{
    RS.Invalidate();
    ctable.clear();
    passTextures.clear();
    passMatrices.clear();
    passConstants.clear();
    dwStage = 0;
    u32 vs_flags = 0;

    // Setup FF-units (Z-buffer, blender)
    PassSET_ZB(bZtest, bZwrite);
    PassSET_Blend(bABlend, abSRC, abDST, aTest, aRef);
    PassSET_LightFog(FALSE, bFog);

    // Create shaders
#if defined(USE_OGL)
    dest.pp = RImplementation.Resources->_CreatePP(_vs, _ps, "null", "null", "null");
    if (GLAD_GL_ARB_separate_shader_objects || !dest.pp->pp)
#endif
    {
        dest.ps = RImplementation.Resources->_CreatePS(_ps);
        ctable.merge(&dest.ps->constants);
        u32 flags = 0;
#if defined(USE_DX11)
        if (dest.ps->constants.dx9compatibility)
            flags |= D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY;
#endif
        vs_flags = flags;
        dest.vs = RImplementation.Resources->_CreateVS(_vs, flags);
        ctable.merge(&dest.vs->constants);
        dest.gs = RImplementation.Resources->_CreateGS("null");
#ifdef USE_DX11
        dest.hs = RImplementation.Resources->_CreateHS("null");
        dest.ds = RImplementation.Resources->_CreateDS("null");
        dest.cs = RImplementation.Resources->_CreateCS("null");
#endif
    }
#if defined(USE_OGL)
    RImplementation.Resources->_LinkPP(dest);
    ctable.merge(&dest.pp->constants);
#endif
    r_PassInstancedTwin(_vs, _ps, vs_flags);

    // Last Stage - disable
    if (0 == xr_stricmp(_ps, "null"))
    {
        RS.SetTSS(0, D3DTSS_COLOROP, D3DTOP_DISABLE);
        RS.SetTSS(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }
}

void CBlender_Compile::r_Constant(LPCSTR name, R_constant_setup* s)
{
    R_ASSERT(s);
    ref_constant C = ctable.get(name);
    if (C)
        C->handler = s;
}

void CBlender_Compile::r_ColorWriteEnable(bool cR, bool cG, bool cB, bool cA)
{
    u8 Mask = 0;
    Mask |= cR ? D3DCOLORWRITEENABLE_RED : 0;
    Mask |= cG ? D3DCOLORWRITEENABLE_GREEN : 0;
    Mask |= cB ? D3DCOLORWRITEENABLE_BLUE : 0;
    Mask |= cA ? D3DCOLORWRITEENABLE_ALPHA : 0;

    RS.SetRS(D3DRS_COLORWRITEENABLE, Mask);
    RS.SetRS(D3DRS_COLORWRITEENABLE1, Mask);
    RS.SetRS(D3DRS_COLORWRITEENABLE2, Mask);
    RS.SetRS(D3DRS_COLORWRITEENABLE3, Mask);
}

u32 CBlender_Compile::i_Sampler(LPCSTR _name) const
{
    string256 name;
    xr_strcpy(name, _name);
    fix_texture_name(name);

    // Find index
    ref_constant C = ctable.get(name, ctable.dx9compatibility ? RC_sampler : u16(-1));
    if (!C)
        return u32(-1);

    R_ASSERT(C->type == RC_sampler);
    u32 stage = C->samp.index;

    // Create texture
    // while (stage>=passTextures.size())	passTextures.push_back		(NULL);
    return stage;
}

void CBlender_Compile::i_Texture(u32 s, LPCSTR name)
{
    if (name)
        passTextures.emplace_back(s, ref_texture(RImplementation.Resources->_CreateTexture(name)));
}

void CBlender_Compile::i_Projective(u32 s, bool b)
{
    if (b)
        RS.SetTSS(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE | D3DTTFF_PROJECTED);
    else
        RS.SetTSS(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
}

void CBlender_Compile::i_Address(u32 s, u32 address)
{
    RS.SetSAMP(s, D3DSAMP_ADDRESSU, address);
    RS.SetSAMP(s, D3DSAMP_ADDRESSV, address);
    RS.SetSAMP(s, D3DSAMP_ADDRESSW, address);
}
void CBlender_Compile::i_BorderColor(u32 s, u32 color) { RS.SetSAMP(s, D3DSAMP_BORDERCOLOR, color); }
void CBlender_Compile::i_Filter_Min(u32 s, u32 f) { RS.SetSAMP(s, D3DSAMP_MINFILTER, f); }
void CBlender_Compile::i_Filter_Mip(u32 s, u32 f) { RS.SetSAMP(s, D3DSAMP_MIPFILTER, f); }
void CBlender_Compile::i_Filter_Mag(u32 s, u32 f) { RS.SetSAMP(s, D3DSAMP_MAGFILTER, f); }
void CBlender_Compile::i_Filter_Aniso(u32 s, u32 f) { RS.SetSAMP(s, D3DSAMP_MAXANISOTROPY, f); }
void CBlender_Compile::i_Filter(u32 s, u32 _min, u32 _mip, u32 _mag)
{
    i_Filter_Min(s, _min);
    i_Filter_Mip(s, _mip);
    i_Filter_Mag(s, _mag);
#if defined(USE_OGL)
    if (_min == D3DTEXF_ANISOTROPIC && _mag == D3DTEXF_ANISOTROPIC)
        i_Filter_Aniso(s, ps_r__tf_Anisotropic);
#endif
}

u32 CBlender_Compile::r_Sampler(
    LPCSTR _name, LPCSTR texture, bool b_ps1x_ProjectiveDivide, u32 address, u32 fmin, u32 fmip, u32 fmag)
{
    dwStage = i_Sampler(_name);
    if (u32(-1) != dwStage)
    {
#if defined(USE_DX11)
        r_dx11Texture(_name, texture, true);
#elif defined(USE_OGL)
        i_Texture(dwStage, texture);
#else
#   error No graphics API selected or enabled!
#endif

        // force ANISO-TF for "s_base"
        if ((0 == xr_strcmp(_name, "s_base")) && (fmin == D3DTEXF_LINEAR))
        {
            fmin = D3DTEXF_ANISOTROPIC;
            fmag = D3DTEXF_ANISOTROPIC;
        }

        if (0 == xr_strcmp(_name, "s_base_hud"))
        {
            fmin = D3DTEXF_GAUSSIANQUAD; // D3DTEXF_PYRAMIDALQUAD; //D3DTEXF_ANISOTROPIC; //D3DTEXF_LINEAR;
            // //D3DTEXF_POINT; //D3DTEXF_NONE
            fmag = D3DTEXF_GAUSSIANQUAD; // D3DTEXF_PYRAMIDALQUAD; //D3DTEXF_ANISOTROPIC; //D3DTEXF_LINEAR;
            // //D3DTEXF_POINT; //D3DTEXF_NONE;
        }

        if ((0 == xr_strcmp(_name, "s_detail")) && (fmin == D3DTEXF_LINEAR))
        {
            fmin = D3DTEXF_ANISOTROPIC;
            fmag = D3DTEXF_ANISOTROPIC;
        }

#if defined(USE_OGL)
        if (0 == xr_strcmp(_name, "s_position"))
        {
            address = D3DTADDRESS_CLAMP;
            fmin = D3DTEXF_POINT;
            fmip = D3DTEXF_NONE;
            fmag = D3DTEXF_POINT;
        }

        if (0 == xr_strcmp(_name, "s_smap"))
        {
            address = D3DTADDRESS_CLAMP;
            fmip = D3DTEXF_NONE;
        }
#endif

        // Sampler states
        i_Address(dwStage, address);
        i_Filter(dwStage, fmin, fmip, fmag);
        //.i_Filter				(dwStage,D3DTEXF_POINT,D3DTEXF_POINT,D3DTEXF_POINT); // show pixels
        if (dwStage < 4)
            i_Projective(dwStage, b_ps1x_ProjectiveDivide);
    }
    return dwStage;
}

void CBlender_Compile::r_Sampler_rtf(LPCSTR name, LPCSTR texture, bool b_ps1x_ProjectiveDivide)
{
    r_Sampler(name, texture, b_ps1x_ProjectiveDivide, D3DTADDRESS_CLAMP, D3DTEXF_POINT, D3DTEXF_NONE, D3DTEXF_POINT);
}
void CBlender_Compile::r_Sampler_clf(LPCSTR name, LPCSTR texture, bool b_ps1x_ProjectiveDivide)
{
    r_Sampler(name, texture, b_ps1x_ProjectiveDivide, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, D3DTEXF_NONE, D3DTEXF_LINEAR);
}
void CBlender_Compile::r_Sampler_clw(LPCSTR name, LPCSTR texture, bool b_ps1x_ProjectiveDivide)
{
    u32 s = r_Sampler(
        name, texture, b_ps1x_ProjectiveDivide, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, D3DTEXF_NONE, D3DTEXF_LINEAR);
    if (u32(-1) != s)
        RS.SetSAMP(s, D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);
}

void CBlender_Compile::r_End()
{
    SetMapping();
    dest.constants = RImplementation.Resources->_CreateConstantTable(ctable);
    dest.state = RImplementation.Resources->_CreateState(RS.GetContainer());
    dest.T = RImplementation.Resources->_CreateTextureList(passTextures);
    dest.C = nullptr;
    dest.M = nullptr;
    dest.inst = nullptr;
    if (bInstTwin)
    {
        // GPU instancing: the twin gets the state, the textures and the constant handlers of the pass (SetMapping
        // above and the blender's own r_Constant calls set them on ctable); its gw_inst array has no handler -
        // render_matrix_items writes it
        bInstTwin = false;
        for (const ref_constant& C : ctable_inst.table)
        {
            const ref_constant src = ctable.get(C->name);
            if (src && src->handler)
                C->handler = src->handler;
        }
        dest_inst.constants = RImplementation.Resources->_CreateConstantTable(ctable_inst);
        dest_inst.state = dest.state;
        dest_inst.T = dest.T;
        dest_inst.C = nullptr;
        dest_inst.M = nullptr;
        dest_inst.inst = nullptr;
        dest.inst = RImplementation.Resources->_CreatePass(dest_inst);
    }
    SH->passes.push_back(RImplementation.Resources->_CreatePass(dest));
    dest.inst = nullptr; // PassEnd and the other r_Pass variants record no twin
}
} // namespace xray::render::RENDER_NAMESPACE
