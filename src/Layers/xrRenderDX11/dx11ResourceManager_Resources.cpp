#include "stdafx.h"
#pragma hdrstop

#include "xrCore/Threading/ScopeLock.hpp"

#include "Layers/xrRender/ResourceManager.h"
#include "Layers/xrRender/tss.h"
#include "Layers/xrRender/blender.h"
#include "Layers/xrRender/blender_recorder.h"
#include "Layers/xrRender/BufferUtils.h"
#include "Layers/xrRenderDX11/dx11ConstantBuffer.h"
#include "Layers/xrRender/ShaderResourceTraits.h"

namespace xray::render::RENDER_NAMESPACE
{
//--------------------------------------------------------------------------------------------------------------
SPass* CResourceManager::_CreatePass(const SPass& proto)
{
    // 40-parallel-prefetch.md (plans/optimization_mt): through the hash index, it was a walk over v_passes
    ScopeLock scope(&m_create_lock);
    const size_t hash = proto.hash();
    if (SPass* pass = m_passes_index.find(hash, [&](const SPass& registered) { return registered.equal(proto); }))
        return pass;

    SPass* P = v_passes.emplace_back(xr_new<SPass>());
    m_passes_index.insert(hash, P); // the fields below are those of proto: the same hash
    P->dwFlags |= xr_resource_flagged::RF_REGISTERED;
    P->state = proto.state;
    P->ps = proto.ps;
    P->vs = proto.vs;
    P->gs = proto.gs;
#ifdef USE_DX11
    P->hs = proto.hs;
    P->ds = proto.ds;
    P->cs = proto.cs;
#endif
    P->constants = proto.constants;
    P->T = proto.T;
#ifdef _EDITOR
    P->M = proto.M;
#endif
    P->C = proto.C;

    return P;
}

//--------------------------------------------------------------------------------------------------------------
SVS* CResourceManager::_CreateVS(cpcstr shader, u32 flags /*= 0*/)
{
    string_path name;
    xr_strcpy(name, shader);
    switch (RImplementation.shader_skinning()) // 40-parallel-prefetch.md: per thread
    {
    case 0:
        xr_strcat(name, "_0");
        break;
    case 1:
        xr_strcat(name, "_1");
        break;
    case 2:
        xr_strcat(name, "_2");
        break;
    case 3:
        xr_strcat(name, "_3");
        break;
    case 4:
        xr_strcat(name, "_4");
        break;
    }

    return CreateShader<SVS>(name, shader, flags);
}

void CResourceManager::_DeleteVS(const SVS* vs)
{
    if (!vs)
        return;
    // "null" placeholder VS (and any already-torn-down signature) has no input signature to
    // unregister from vertex layouts.
    if (!vs->signature._get())
        return;
    // 50l-weak-cpu-spawn-models.md (plans/optimization_mt): the shader maps (m_vs, m_ps...), the input signatures and
    // the declarations are deleted under the locks they are created under - in play the background base loading
    // creates shaders (m_create_lock) and geometry (v_geoms_lock) while the main thread may release the last
    // reference of one. Order: m_create_lock -> v_geoms_lock -> the layout lock.
    ScopeLock scope(&m_create_lock);
    if (DestroyShader(vs))
    {
        ScopeLock geoms_scope(&v_geoms_lock); // v_declarations
        // Layouts of this signature are released below: drop the cached pairs of CBackend::ApplyVertexLayout.
        g_dx11_input_layout_epoch.fetch_add(1, std::memory_order_relaxed);
        std::unique_lock<std::shared_mutex> layout_lock{ g_dx11_input_layout_lock };
        for (const auto& iDecl : v_declarations)
        {
            const auto iLayout = iDecl->vs_to_layout.find(vs->signature->signature);
            if (iLayout != iDecl->vs_to_layout.end())
            {
                //	Release vertex layout
                _RELEASE(iLayout->second);
                iDecl->vs_to_layout.erase(iLayout);
            }
        }
    }
}

//--------------------------------------------------------------------------------------------------------------
SPS* CResourceManager::_CreatePS(LPCSTR _name)
{
    string_path name;
    xr_strcpy(name, _name);
    switch (RImplementation.m_MSAASample)
    {
    case 0:
        xr_strcat(name, "_0");
        break;
    case 1:
        xr_strcat(name, "_1");
        break;
    case 2:
        xr_strcat(name, "_2");
        break;
    case 3:
        xr_strcat(name, "_3");
        break;
    case 4:
        xr_strcat(name, "_4");
        break;
    case 5:
        xr_strcat(name, "_5");
        break;
    case 6:
        xr_strcat(name, "_6");
        break;
    case 7:
        xr_strcat(name, "_7");
        break;
    }

    return CreateShader<SPS>(name, _name);
}

void CResourceManager::_DeletePS(const SPS* ps)
{
    ScopeLock scope(&m_create_lock); // 50l-weak-cpu-spawn-models.md: see _DeleteVS
    DestroyShader(ps);
}

//--------------------------------------------------------------------------------------------------------------
SGS* CResourceManager::_CreateGS(LPCSTR Name) { return CreateShader<SGS>(Name); }
void CResourceManager::_DeleteGS(const SGS* GS)
{
    ScopeLock scope(&m_create_lock); // 50l
    DestroyShader(GS);
}

#ifdef USE_DX11
SHS* CResourceManager::_CreateHS(LPCSTR Name) { return CreateShader<SHS>(Name); }
void CResourceManager::_DeleteHS(const SHS* HS)
{
    ScopeLock scope(&m_create_lock); // 50l
    DestroyShader(HS);
}

SDS* CResourceManager::_CreateDS(LPCSTR Name) { return CreateShader<SDS>(Name); }
void CResourceManager::_DeleteDS(const SDS* DS)
{
    ScopeLock scope(&m_create_lock); // 50l
    DestroyShader(DS);
}

SCS* CResourceManager::_CreateCS(LPCSTR Name) { return CreateShader<SCS>(Name); }
void CResourceManager::_DeleteCS(const SCS* CS)
{
    ScopeLock scope(&m_create_lock); // 50l
    DestroyShader(CS);
}
#endif

//--------------------------------------------------------------------------------------------------------------

SDeclaration* CResourceManager::_CreateDecl(const D3DVERTEXELEMENT9* dcl)
{
    // Search equal code
    for (SDeclaration* D : v_declarations)
    {
        if (dcl_equal(dcl, &D->dcl_code.front()))
            return D;
    }

    // Create _new
    SDeclaration* D = v_declarations.emplace_back(xr_new<SDeclaration>());
    u32 dcl_size = GetDeclLength(dcl) + 1;
    //	Don't need it for DirectX 10 here
    // CHK_DX					(HW.pDevice->CreateVertexDeclaration(dcl,&D->dcl));
    D->dcl_code.assign(dcl, dcl + dcl_size);
    ConvertVertexDeclaration(D->dcl_code, D->dx11_dcl_code);
    D->dwFlags |= xr_resource_flagged::RF_REGISTERED;

    return D;
}

//--------------------------------------------------------------------------------------------------------------
dx11ConstantBuffer* CResourceManager::_CreateConstantBuffer(u32 context_id, ID3DShaderReflectionConstantBuffer* pTable)
{
    VERIFY(pTable);
    dx11ConstantBuffer* pTempBuffer = xr_new<dx11ConstantBuffer>(pTable);

    // One buffer per cbuffer layout in the context, shared by every shader with it (the vertex and pixel shader of a
    // pass write and map one cbuffer dynamic_transforms per draw).
    dx11ConstantBuffer* found = nullptr;
    {
        std::unique_lock lock(v_constant_buffer_lock);
        auto& pool = v_constant_buffer[context_id];
        for (dx11ConstantBuffer* buf : pool)
        {
            if (pTempBuffer->SameLayout(*buf))
            {
                found = buf;
                break;
            }
        }

        if (!found)
        {
            pTempBuffer->CreateGpuBuffer();
            pTempBuffer->dwFlags |= xr_resource_flagged::RF_REGISTERED;
            pool.emplace_back(pTempBuffer);
            return pTempBuffer;
        }
    }
    // Not registered: its destructor does not touch the registries (outside the lock anyway).
    xr_delete(pTempBuffer);
    return found;
}

void CResourceManager::_DeleteConstantBuffer(u32 context_id, const dx11ConstantBuffer* pBuffer)
{
    if (0 == (pBuffer->dwFlags & xr_resource_flagged::RF_REGISTERED))
        return;
    std::unique_lock lock(v_constant_buffer_lock);
    reclaim(v_constant_buffer[context_id], pBuffer);
    // Do not Msg here: each buffer is registered in only one context; other contexts correctly "don't find" it.
}

void CResourceManager::MarkConstantBuffersChanged(u32 context_id)
{
    VERIFY(context_id < R__NUM_CONTEXTS);
    // Render threads, possibly while the Game thread creates or destroys shaders (their constant buffers).
    std::shared_lock lock(v_constant_buffer_lock);
    for (dx11ConstantBuffer* buf : v_constant_buffer[context_id])
        buf->MarkChanged();
}

bool CResourceManager::_UnregisterConstantBufferFromAllContexts(const dx11ConstantBuffer* pBuffer)
{
    if (0 == (pBuffer->dwFlags & xr_resource_flagged::RF_REGISTERED))
        return true;
    std::unique_lock lock(v_constant_buffer_lock);
    for (u32 id = 0; id < R__NUM_CONTEXTS; ++id)
    {
        if (reclaim(v_constant_buffer[id], pBuffer))
            return true;
    }
#ifndef MASTER_GOLD
    Msg("! ERROR: Failed to find compiled constant buffer in any context (double free or never registered?)");
#endif
    return false;
}

//--------------------------------------------------------------------------------------------------------------
SInputSignature* CResourceManager::_CreateInputSignature(ID3DBlob* pBlob)
{
    VERIFY(pBlob);

    for (SInputSignature* sign : v_input_signature)
    {
        if ((pBlob->GetBufferSize() == sign->signature->GetBufferSize()) &&
            (!(memcmp(pBlob->GetBufferPointer(), sign->signature->GetBufferPointer(), pBlob->GetBufferSize()))))
        {
            return sign;
        }
    }

    SInputSignature* pSign = v_input_signature.emplace_back(xr_new<SInputSignature>(pBlob));
    pSign->dwFlags |= xr_resource_flagged::RF_REGISTERED;

    return pSign;
}

void CResourceManager::_DeleteInputSignature(const SInputSignature* pSignature)
{
    if (0 == (pSignature->dwFlags & xr_resource_flagged::RF_REGISTERED))
        return;
    ScopeLock scope(&m_create_lock); // 50l: see _DeleteVS
    if (reclaim(v_input_signature, pSignature))
        return;
#ifndef MASTER_GOLD
    Msg("! ERROR: Failed to find input signature");
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
