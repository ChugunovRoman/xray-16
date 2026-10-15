#pragma once

namespace xray::render::RENDER_NAMESPACE
{
class dx11SamplerStateCache
{
public:
    enum
    {
        hInvalidHandle = 0xFFFFFFFF
    };

    //	State handle
    typedef u32 SHandle;
    typedef xr_vector<SHandle> HArray;

public:
    dx11SamplerStateCache();
    ~dx11SamplerStateCache();

    void ClearStateArray();

    SHandle GetState(D3D_SAMPLER_DESC& desc);

    // Return true when a *SetSamplers call was issued, false when the stage already held exactly these samplers.
    bool VSApplySamplers(u32 context_id, HArray& samplers);
    bool PSApplySamplers(u32 context_id, HArray& samplers);
    bool GSApplySamplers(u32 context_id, HArray& samplers);
    bool HSApplySamplers(u32 context_id, HArray& samplers);
    bool DSApplySamplers(u32 context_id, HArray& samplers);
    bool CSApplySamplers(u32 context_id, HArray& samplers);

    // Forget what the device context (or all of them) has bound: the next Apply of every stage issues the full call.
    // Needed wherever the context's real state changes behind this cache: ClearState, ExecuteCommandList(.., FALSE)
    // on the immediate context, FinishCommandList on a deferred one, device re-creation.
    void ResetBound(u32 context_id)
    {
        VERIFY(context_id < R__NUM_CONTEXTS);
        m_bound[context_id].valid = 0;
    }
    void ResetBoundAll()
    {
        for (BoundSamplers& bound : m_bound)
            bound.valid = 0;
    }

    void SetMaxAnisotropy(u32 uiMaxAniso);
    void SetMipLODBias(float uiMipLODBias);

private:
    enum
    {
        StageVS,
        StagePS,
        StageGS,
        StageHS,
        StageDS,
        StageCS,
        StageCount
    };

    // The samplers last sent to each stage of one device context (P19 step 2). Kept per DEVICE context, not per
    // CBackend: several CBackend frontends may share the immediate context (SVP and serial light batches with
    // alloc_context(false)), and all of them set samplers only through this class, so the record stays the device's
    // truth. Only the thread that owns the context touches its record. Apply sends only the changed range of slots
    // (all 16 while the record of the stage is not valid).
    struct BoundSamplers
    {
        ID3DSamplerState* states[StageCount][D3D_COMMONSHADER_SAMPLER_SLOT_COUNT];
        u32 valid; // bit per stage: states[stage] mirrors the device
    };

    bool ApplySamplers(u32 context_id, u32 stage, const HArray& samplers);

    typedef ID3DSamplerState IDeviceState;
    typedef D3D_SAMPLER_DESC StateDecs;

    struct StateRecord
    {
        u32 m_crc;
        IDeviceState* m_pState;
    };

private:
    void CreateState(StateDecs desc, IDeviceState** ppIState);
    SHandle FindState(const StateDecs& desc, u32 StateCRC);

    void PrepareSamplerStates(const HArray& samplers, ID3DSamplerState* pSS[D3D_COMMONSHADER_SAMPLER_SLOT_COUNT]) const;

    //	Private data
private:
    //	This must be cleared on device destroy
    xr_vector<StateRecord> m_StateArray;
    // GetState: FindState + push_back of a new record (the readers rely on the reserve, see the constructor)
    std::mutex m_create_lock;

    BoundSamplers m_bound[R__NUM_CONTEXTS]{};

    u32 m_uiMaxAnisotropy;
    float m_uiMipLODBias;
};

extern dx11SamplerStateCache SSManager;
} // namespace xray::render::RENDER_NAMESPACE
