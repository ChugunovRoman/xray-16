// Shader.cpp: implementation of the CShader class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#pragma hdrstop

#include "Shader.h"
#include "ResourceManager.h"

namespace xray::render::RENDER_NAMESPACE
{
// XXX: render scripts should call these destructors before resource manager gets destroyed
STextureList::~STextureList() { RImplementation.Resources->_DeleteTextureList(this); }
SMatrixList::~SMatrixList() { RImplementation.Resources->_DeleteMatrixList(this); }
SConstantList::~SConstantList() { RImplementation.Resources->_DeleteConstantList(this); }
SPass::~SPass() { RImplementation.Resources->_DeletePass(this); }
ShaderElement::~ShaderElement() { RImplementation.Resources->_DeleteElement(this); }
SGeometry::~SGeometry() { RImplementation.Resources->DeleteGeom(this); }
Shader::~Shader() { RImplementation.Resources->Delete(this); }
//////////////////////////////////////////////////////////////////////////
void resptrcode_shader::create(LPCSTR s_shader, LPCSTR s_textures, LPCSTR s_constants, LPCSTR s_matrices)
{
    _set(RImplementation.Resources->Create(s_shader, s_textures, s_constants, s_matrices));
}
void resptrcode_shader::create(IBlender* B, LPCSTR s_shader, LPCSTR s_textures, LPCSTR s_constants, LPCSTR s_matrices)
{
    _set(RImplementation.Resources->Create(B, s_shader, s_textures, s_constants, s_matrices));
}

//////////////////////////////////////////////////////////////////////////
void resptrcode_geom::create(u32 FVF, VertexBufferHandle vb, IndexBufferHandle ib)
{
    _set(RImplementation.Resources->CreateGeom(FVF, vb, ib));
}

void resptrcode_geom::create(const VertexElement* decl, VertexBufferHandle vb, IndexBufferHandle ib)
{
    _set(RImplementation.Resources->CreateGeom(decl, vb, ib));
}


//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
bool SPass::equal(const SPass& other) const
{
    if (state != other.state)
        return false;
    if (ps != other.ps)
        return false;
    if (vs != other.vs)
        return false;
    if (gs != other.gs)
        return false;
#ifdef USE_DX11
    if (hs != other.hs)
        return false;
    if (ds != other.ds)
        return false;
    if (cs != other.cs)
        return false;
#elif defined(USE_OGL)
    if (pp != other.pp)
        return false;
#endif
    if (constants != other.constants)
        return false; // is this nessesary??? (ps+vs already combines)

    if (T != other.T)
        return false;
    if (C != other.C)
        return false;
#ifdef _EDITOR
    if (M != other.M)
        return false;
#endif
    return true;
}

//
ShaderElement::ShaderElement()
{
    flags.iPriority = 1;
    flags.bStrictB2F = FALSE;
    flags.bEmissive = FALSE;
    flags.bDistort = FALSE;
    flags.bWmark = FALSE;
    flags.bScopeLens = FALSE;
}

BOOL ShaderElement::equal(ShaderElement& S)
{
    if (flags.iPriority != S.flags.iPriority)
        return FALSE;
    if (flags.bStrictB2F != S.flags.bStrictB2F)
        return FALSE;
    if (flags.bEmissive != S.flags.bEmissive)
        return FALSE;
    if (flags.bWmark != S.flags.bWmark)
        return FALSE;
    if (flags.bDistort != S.flags.bDistort)
        return FALSE;
    if (flags.bScopeLens != S.flags.bScopeLens)
        return FALSE;
    if (passes.size() != S.passes.size())
        return FALSE;
    for (u32 p = 0; p < passes.size(); p++)
        if (passes[p] != S.passes[p])
            return FALSE;
    return TRUE;
}

BOOL Shader::equal(Shader* S, int index)
{
    if(nullptr == E[index] && nullptr == S->E[index])
        return TRUE;
    if(nullptr == E[index] || nullptr == S->E[index])
        return FALSE;

    return E[index]->equal(*S->E[index]);
}

BOOL Shader::equal(Shader* S)
{
    for (int i = 0; i < 5; i++)
    {
        if (!equal(S, i))
            return FALSE;
    }
    return TRUE;
}

// 40-parallel-prefetch.md (plans/optimization_mt): the hashes of the indices of CResourceManager. Each takes exactly
// what equal() compares, so equal entries hash equal.
size_t STextureList::hash() const
{
    size_t h = size();
    for (const auto& [stage, texture] : *this)
    {
        h = resource_hash_combine(h, stage);
        h = resource_hash_combine(h, resource_hash_value(texture._get()));
    }
    return h;
}

size_t SPass::hash() const
{
    size_t h = resource_hash_value(state._get());
    h = resource_hash_combine(h, resource_hash_value(ps._get()));
    h = resource_hash_combine(h, resource_hash_value(vs._get()));
    h = resource_hash_combine(h, resource_hash_value(gs._get()));
#ifdef USE_DX11
    h = resource_hash_combine(h, resource_hash_value(hs._get()));
    h = resource_hash_combine(h, resource_hash_value(ds._get()));
    h = resource_hash_combine(h, resource_hash_value(cs._get()));
#elif defined(USE_OGL)
    h = resource_hash_combine(h, resource_hash_value(pp._get()));
#endif
    h = resource_hash_combine(h, resource_hash_value(constants._get()));
    h = resource_hash_combine(h, resource_hash_value(T._get()));
    h = resource_hash_combine(h, resource_hash_value(C._get()));
#ifdef _EDITOR
    h = resource_hash_combine(h, resource_hash_value(M._get()));
#endif
    return h;
}

size_t ShaderElement::hash() const
{
    size_t h = flags.iPriority;
    h = resource_hash_combine(h, flags.bStrictB2F);
    h = resource_hash_combine(h, flags.bEmissive);
    h = resource_hash_combine(h, flags.bWmark);
    h = resource_hash_combine(h, flags.bDistort);
    h = resource_hash_combine(h, flags.bScopeLens);
    h = resource_hash_combine(h, passes.size());
    for (const ref_pass& pass : passes)
        h = resource_hash_combine(h, resource_hash_value(pass._get()));
    return h;
}

size_t Shader::hash() const
{
    // equal() compares E[0..4] by content (ShaderElement::equal), E[5] not at all
    size_t h = 0;
    for (int i = 0; i < 5; i++)
        h = resource_hash_combine(h, E[i] ? E[i]->hash() : 0);
    return h;
}

void STextureList::clear()
{
    iterator it = begin();
    iterator it_e = end();
    for (; it != it_e; ++it)
        (*it).second.destroy();

    erase(begin(), end());
}

u32 STextureList::find_texture_stage(const shared_str& TexName, bool warnIfMissing /*= true*/) const
{
    for (const auto& [stage, texture] : *this)
    {
        if (!texture)
            continue;
        if (texture->cName == TexName)
            return stage;
    }

    if (!warnIfMissing)
        return u32(-1);
    VERIFY3(false, "Couldn't find texture stage", TexName.c_str());
    return 0;
}

void STextureList::create_texture(u32 stage, pcstr textureName, bool overrideIfExists)
{
    for (auto& loader : *this)
    {
        if (loader.first == stage && (!loader.second || overrideIfExists))
        {
            //  Assign correct texture
            loader.second.create(textureName);
        }
    }
}
} // namespace xray::render::RENDER_NAMESPACE
