#include "stdafx.h"
#include "dx11ConstantBuffer.h"

#include "Layers/xrRender/BufferUtils.h"
#include "xrEngine/device.h"
#include "xrCore/Profiling/PerfMetrics.hpp"

namespace xray::render::RENDER_NAMESPACE
{
dx11ConstantBuffer::~dx11ConstantBuffer()
{
    RImplementation.Resources->_UnregisterConstantBufferFromAllContexts(this);
    _RELEASE(m_pBuffer);
    xr_free(m_pBufferData);
}

dx11ConstantBuffer::dx11ConstantBuffer(ID3DShaderReflectionConstantBuffer* pTable) : m_pBuffer(nullptr), m_bChanged(true)
{
    m_uiArrayOffset = 0;
    m_uiArrayEnd = 0;
    m_uiFlushEnd = 0;
    m_uiUploadEnd = 0;
    m_kind = KindOther;

    D3D_SHADER_BUFFER_DESC Desc;

    CHK_DX(pTable->GetDesc(&Desc));

    m_strBufferName._set(Desc.Name);
    m_eBufferType = Desc.Type;
    m_uiBufferSize = Desc.Size;

    //	Fill member list with variable descriptions
    m_MembersList.resize(Desc.Variables);
    m_MembersNames.resize(Desc.Variables);
    m_MembersTypeNames.resize(Desc.Variables);
    m_MembersOffsets.resize(Desc.Variables);
    // Partial palette upload: the largest float4 array member (sbones_array in the $Globals of skinned vertex shaders)
    // is the only part a Flush may leave out past the rows written for a draw.
    m_uiArrayOffset = Desc.Size;
    m_uiArrayEnd = Desc.Size;
    u32 array_bytes = 0;
    for (u32 i = 0; i < Desc.Variables; ++i)
    {
        ID3DShaderReflectionVariable* pVar;
        ID3DShaderReflectionType* pType;

        D3D_SHADER_VARIABLE_DESC var_desc;

        pVar = pTable->GetVariableByIndex(i);
        VERIFY(pVar);
        pType = pVar->GetType();
        VERIFY(pType);
        pType->GetDesc(&m_MembersList[i]);
        //	Buffers with the same layout can contain totally different members
        CHK_DX(pVar->GetDesc(&var_desc));
        m_MembersNames[i] = var_desc.Name;
        m_MembersTypeNames[i] = m_MembersList[i].Name;
        m_MembersOffsets[i] = var_desc.StartOffset;

        const D3D_SHADER_TYPE_DESC& type = m_MembersList[i];
        const u32 bytes = type.Elements * lineSize;
        if (type.Elements > 1 && type.Class == D3D_SVC_VECTOR && type.Columns == 4 && type.Rows == 1 &&
            bytes > array_bytes && var_desc.StartOffset + bytes <= Desc.Size)
        {
            array_bytes = bytes;
            m_uiArrayOffset = var_desc.StartOffset;
            m_uiArrayEnd = var_desc.StartOffset + bytes;
        }
    }
    m_uiFlushEnd = Desc.Size;

    if (0 == xr_strcmp(Desc.Name, "dynamic_transforms"))
        m_kind = KindTransforms;
    else if (0 == xr_strcmp(Desc.Name, "$Globals"))
        m_kind = KindGlobals;

    m_pBufferData = xr_malloc(Desc.Size);
    VERIFY(m_pBufferData);
}

void dx11ConstantBuffer::CreateGpuBuffer()
{
    VERIFY(!m_pBuffer);
    R_CHK(BufferUtils::CreateConstantBuffer(&m_pBuffer, m_uiBufferSize));
    VERIFY(m_pBuffer);

#ifdef DEBUG
    if (m_pBuffer)
    {
        m_pBuffer->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(m_strBufferName.size()), m_strBufferName.c_str());
    }
#endif
}

bool dx11ConstantBuffer::SameLayout(const dx11ConstantBuffer& _in) const
{
    if (m_strBufferName._get() != _in.m_strBufferName._get() || m_eBufferType != _in.m_eBufferType ||
        m_uiBufferSize != _in.m_uiBufferSize || m_MembersList.size() != _in.m_MembersList.size())
    {
        return false;
    }

    for (size_t i = 0; i < m_MembersList.size(); ++i)
    {
        const D3D_SHADER_TYPE_DESC& a = m_MembersList[i];
        const D3D_SHADER_TYPE_DESC& b = _in.m_MembersList[i];
        if (a.Class != b.Class || a.Type != b.Type || a.Rows != b.Rows || a.Columns != b.Columns ||
            a.Elements != b.Elements || a.Members != b.Members || a.Offset != b.Offset ||
            m_MembersOffsets[i] != _in.m_MembersOffsets[i] ||
            m_MembersNames[i]._get() != _in.m_MembersNames[i]._get() ||
            m_MembersTypeNames[i]._get() != _in.m_MembersTypeNames[i]._get())
        {
            return false;
        }
    }
    return true;
}

void dx11ConstantBuffer::Flush(u32 context_id)
{
    if (m_bChanged)
    {
        // Whole buffer, or everything but the palette rows past the ones written for
        // this draw (see WriteRows): [0, m_uiFlushEnd) + [m_uiArrayEnd, size). The draw reads nothing in the gap.
        const bool partial = m_uiFlushEnd < m_uiArrayEnd;
        VERIFY(m_uiFlushEnd <= m_uiBufferSize && (!partial || m_uiFlushEnd >= m_uiArrayOffset));
        const u32 bytes = partial ? m_uiFlushEnd + (m_uiBufferSize - m_uiArrayEnd) : m_uiBufferSize;
        PERF_SCOPE(cb_flush);
        PERF_ADD(cb_flush_bytes, bytes);
#if XR_PERF_METRICS
        ++g_cb_flush_count;
        g_cb_flush_bytes += bytes;
        if (bytes < m_uiBufferSize)
        {
            PERF_INC(cb_flush_partial);
            PERF_ADD(cb_flush_partial_saved, m_uiBufferSize - bytes);
        }
        if (g_perf_rmain_dyn)
        {
            switch (m_kind)
            {
            case KindTransforms: PERF_INC(rmain_dyn_cb_transforms); break;
            case KindGlobals:
                PERF_INC(rmain_dyn_cb_globals);
                PERF_ADD(rmain_dyn_cb_globals_bytes, bytes);
                break;
            default:
                PERF_INC(rmain_dyn_cb_other);
                PERF_ADD(rmain_dyn_cb_other_bytes, bytes);
                break;
            }
        }
#endif
        void* pData;
#ifdef USE_DX11
        D3D11_MAPPED_SUBRESOURCE pSubRes;
        CHK_DX(HW.get_context(context_id)->Map(m_pBuffer, 0, D3D_MAP_WRITE_DISCARD, 0, &pSubRes));
        pData = pSubRes.pData;
#else
        CHK_DX(m_pBuffer->Map(D3D_MAP_WRITE_DISCARD, 0, &pData));
#endif
        VERIFY(pData);
        VERIFY(m_pBufferData);
        if (partial)
        {
            CopyMemory(pData, m_pBufferData, m_uiFlushEnd);
            if (m_uiArrayEnd < m_uiBufferSize)
            {
                CopyMemory(static_cast<u8*>(pData) + m_uiArrayEnd,
                    static_cast<const u8*>(m_pBufferData) + m_uiArrayEnd, m_uiBufferSize - m_uiArrayEnd);
            }
        }
        else
            CopyMemory(pData, m_pBufferData, m_uiBufferSize);
#ifdef USE_DX11
        HW.get_context(context_id)->Unmap(m_pBuffer, 0);
#else
        m_pBuffer->Unmap();
#endif
        m_bChanged = false;
        m_uiUploadEnd = partial ? m_uiFlushEnd : m_uiBufferSize;
        m_uiFlushEnd = m_uiBufferSize;
    }
}
} // namespace xray::render::RENDER_NAMESPACE
