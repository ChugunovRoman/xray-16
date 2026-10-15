#include "stdafx.h"
#include "dx11SamplerStateCache.h"
#include "Layers/xrRenderDX11/dx11StateUtils.h"

namespace xray::render::RENDER_NAMESPACE
{
using dx11StateUtils::operator==;

dx11SamplerStateCache SSManager;

dx11SamplerStateCache::dx11SamplerStateCache() : m_uiMaxAnisotropy(1), m_uiMipLODBias(0.0f)
{
    static const int iMaxRSStates = 10;
    m_StateArray.reserve(iMaxRSStates);
}

dx11SamplerStateCache::~dx11SamplerStateCache() { ClearStateArray(); }
dx11SamplerStateCache::SHandle dx11SamplerStateCache::GetState(D3D_SAMPLER_DESC& desc)
{
    SHandle hResult;

    //	MaxAnisitropy is reset by ValidateState if not aplicable
    //	to the filter mode used.
    desc.MaxAnisotropy = m_uiMaxAnisotropy;
    // RZ
    desc.MipLODBias = m_uiMipLODBias;

    dx11StateUtils::ValidateState(desc);

    u32 crc = dx11StateUtils::GetHash(desc);

    hResult = FindState(desc, crc);

    if (hResult == hInvalidHandle)
    {
        StateRecord rec;
        rec.m_crc = crc;
        CreateState(desc, &rec.m_pState);
        hResult = m_StateArray.size();
        m_StateArray.push_back(rec);
    }

    return hResult;
}

void dx11SamplerStateCache::CreateState(StateDecs desc, IDeviceState** ppIState)
{
    CHK_DX(HW.pDevice->CreateSamplerState(&desc, ppIState));
}

dx11SamplerStateCache::SHandle dx11SamplerStateCache::FindState(const StateDecs& desc, u32 StateCRC)
{
    u32 res = 0xffffffff;
    u32 i = 0;
    while (i < m_StateArray.size())
    {
        if (m_StateArray[i].m_crc == StateCRC)
        {
            StateDecs descCandidate;
            m_StateArray[i].m_pState->GetDesc(&descCandidate);
            if (descCandidate == desc)
            // return i;
            //	TEST
            {
                // return i;
                res = i;
                break;
            }
            // else
            //{
            //	VERIFY(0);
            //}
        }
        i++;
    }

    return res != 0xffffffff ? i : (u32)hInvalidHandle;
}

void dx11SamplerStateCache::ClearStateArray()
{
    for (u32 i = 0; i < m_StateArray.size(); ++i)
    {
        _RELEASE(m_StateArray[i].m_pState);
    }

    m_StateArray.clear();
    // The device (and its contexts) goes away with the states: start every context record from scratch.
    ResetBoundAll();
}

void dx11SamplerStateCache::PrepareSamplerStates(const HArray& samplers,
    ID3DSamplerState* pSS[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT]) const
{
    VERIFY(samplers.size() <= D3D_COMMONSHADER_SAMPLER_SLOT_COUNT);
    for (u32 i = 0; i < samplers.size(); ++i)
    {
        if (samplers[i] != hInvalidHandle)
        {
            VERIFY(samplers[i] < m_StateArray.size());
            pSS[i] = m_StateArray[samplers[i]].m_pState;
        }
    }
}

bool dx11SamplerStateCache::ApplySamplers(u32 context_id, u32 stage, const HArray& samplers)
{
    constexpr u32 slots = D3D_COMMONSHADER_SAMPLER_SLOT_COUNT;
    ID3DSamplerState* pSS[slots] = {};
    PrepareSamplerStates(samplers, pSS);

    VERIFY(context_id < R__NUM_CONTEXTS);
    BoundSamplers& bound = m_bound[context_id];
    ID3DSamplerState** cached = bound.states[stage];
    const u32 stage_bit = 1u << stage;

    // Only the [first, last] changed slots go out, nothing when the stage already holds these samplers (consecutive
    // passes mostly share the sampler set); all 16 slots while the record of the stage is not valid (P19 step 2).
    u32 first = 0;
    u32 last = slots - 1;
    if (bound.valid & stage_bit)
    {
        while (first < slots && cached[first] == pSS[first])
            ++first;
        if (first == slots)
            return false;
        while (cached[last] == pSS[last])
            --last;
    }

    for (u32 i = first; i <= last; ++i)
        cached[i] = pSS[i];
    bound.valid |= stage_bit;

    auto* context = HW.get_context(context_id);
    const u32 count = last - first + 1;
    switch (stage)
    {
    case StageVS: context->VSSetSamplers(first, count, &pSS[first]); break;
    case StagePS: context->PSSetSamplers(first, count, &pSS[first]); break;
    case StageGS: context->GSSetSamplers(first, count, &pSS[first]); break;
    case StageHS: context->HSSetSamplers(first, count, &pSS[first]); break;
    case StageDS: context->DSSetSamplers(first, count, &pSS[first]); break;
    case StageCS: context->CSSetSamplers(first, count, &pSS[first]); break;
    default: NODEFAULT;
    }
    return true;
}

bool dx11SamplerStateCache::VSApplySamplers(u32 context_id, HArray& samplers)
{
    return ApplySamplers(context_id, StageVS, samplers);
}

bool dx11SamplerStateCache::PSApplySamplers(u32 context_id, HArray& samplers)
{
    return ApplySamplers(context_id, StagePS, samplers);
}

bool dx11SamplerStateCache::GSApplySamplers(u32 context_id, HArray& samplers)
{
    return ApplySamplers(context_id, StageGS, samplers);
}

bool dx11SamplerStateCache::HSApplySamplers(u32 context_id, HArray& samplers)
{
    return ApplySamplers(context_id, StageHS, samplers);
}

bool dx11SamplerStateCache::DSApplySamplers(u32 context_id, HArray& samplers)
{
    return ApplySamplers(context_id, StageDS, samplers);
}

bool dx11SamplerStateCache::CSApplySamplers(u32 context_id, HArray& samplers)
{
    return ApplySamplers(context_id, StageCS, samplers);
}

void dx11SamplerStateCache::SetMaxAnisotropy(u32 uiMaxAniso)
{
    clamp(uiMaxAniso, (u32)1, (u32)16);

    if (m_uiMaxAnisotropy == uiMaxAniso)
        return;

    m_uiMaxAnisotropy = uiMaxAniso;
    // Every state object is re-created below: re-send all stages on the next Apply.
    ResetBoundAll();

    for (u32 i = 0; i < m_StateArray.size(); ++i)
    {
        StateRecord& rec = m_StateArray[i];
        StateDecs desc;

        rec.m_pState->GetDesc(&desc);

        //	MaxAnisitropy is reset by ValidateState if not aplicable
        //	to the filter mode used.
        //	Reason: all checks for aniso applicability are done
        //	in ValidateState.
        desc.MaxAnisotropy = m_uiMaxAnisotropy;
        dx11StateUtils::ValidateState(desc);

        //	This can cause fragmentation if called too often
        rec.m_pState->Release();
        CreateState(desc, &rec.m_pState);
    }
}

void dx11SamplerStateCache::SetMipLODBias(float uiMipLODBias)
{
    if (m_uiMipLODBias == uiMipLODBias)
        return;

    m_uiMipLODBias = uiMipLODBias;
    // Every state object is re-created below: re-send all stages on the next Apply.
    ResetBoundAll();

    for (u32 i = 0; i < m_StateArray.size(); ++i)
    {
        StateRecord& rec = m_StateArray[i];
        StateDecs desc;

        rec.m_pState->GetDesc(&desc);

        desc.MipLODBias = m_uiMipLODBias;
        dx11StateUtils::ValidateState(desc);

        // This can cause fragmentation if called too often
        rec.m_pState->Release();
        CreateState(desc, &rec.m_pState);
    }
}
} // namespace xray::render::RENDER_NAMESPACE
