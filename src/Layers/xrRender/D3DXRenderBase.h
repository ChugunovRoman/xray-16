#pragma once

#include <mutex>

#include "xrEngine/Render.h"
#include "xrCDB/ISpatial.h"
#include "r__dsgraph_types.h"
#include "r__dsgraph_structure.h"
#include "r__sector.h"
#include "xr_effgamma.h"

namespace xray::render::RENDER_NAMESPACE
{
// Common part of interface implementation for all D3D renderers
class D3DXRenderBase : public IRender, public pureFrame
{
public:
    //friend class CSkeletonX; // Stats.Skinning
    //friend class CKinematics; // Stats.Animation
    RenderStatistics BasicStats;

    // Loads renderdoc.dll and installs its hooks. Idempotent. Must run BEFORE the first
    // LoadLibrary("d3d11") (the startup HW probe triggers one) - RenderDoc can only wrap
    // a device that is created through its hooks, so a late load silently captures nothing.
    static void InitializeRenderDoc();

    // True when the built-in RenderDoc integration has loaded and hooked the device.
    // Rendering paths that are known to crash RenderDoc's hooks (multi-threaded deferred
    // context recording) must fall back to the sequential immediate-context path.
    static bool RenderDocActive();

public:
    //	Gamma correction functions
    virtual void setGamma(float fGamma) override;
    virtual void setBrightness(float fGamma) override;
    virtual void setContrast(float fGamma) override;
    virtual void updateGamma() override;

    //	Destroy
    virtual void OnDeviceDestroy(bool bKeepTextures) override;
    virtual void Destroy() override;
    virtual void Reset(SDL_Window* hWnd, u32& dwWidth, u32& dwHeight, float& fWidth_2, float& fHeight_2) override;

    //	Init
    virtual void ObtainRequiredWindowFlags(u32& /*windowFlags*/) override;
    virtual void SetupStates() override;
    virtual void OnDeviceCreate(const char* shName) override;
    virtual void Create(SDL_Window* hWnd, u32& dwWidth, u32& dwHeight, float& fWidth_2, float& fHeight_2) override;

    //	Overdraw
    virtual void overdrawBegin() override;
    virtual void overdrawEnd() override;
    //	Resources control
    virtual void DeferredLoad(bool E) override;
    virtual void ResourcesDeferredUpload() override;
    virtual void ResourcesDeferredUnload() override;
    virtual void ResourcesGetMemoryUsage(u32& m_base, u32& c_base, u32& m_lmaps, u32& c_lmaps) override;
    virtual void ResourcesDestroyNecessaryTextures() override;
    virtual void ResourcesStoreNecessaryTextures() override;
    virtual void ResourcesDumpMemoryUsage() override;
    //	HWSupport
    virtual bool HWSupportsShaderYUV2RGB() override;
    //	Device state
    virtual DeviceState GetDeviceState() override;
    virtual bool GetForceGPU_REF() override;
    virtual u32 GetCacheStatPolys() override;
    virtual void Begin() override;
    virtual void Clear() override;
    virtual void End() override;
    virtual void PresentFrame() override;
    virtual void ClearTarget() override;
    virtual void SetCacheXform(Fmatrix& mView, Fmatrix& mProject) override;
    virtual void OnAssetsChanged() override;
    virtual void DumpStatistics(class IGameFont& font, class IPerformanceAlert* alert) override;

    xrImTextureData GetImGuiTextureId(pcstr texture_name) override;

    RenderContext GetCurrentContext() const override { return IRender::PrimaryContext; }
    void MakeContextCurrent(RenderContext /*context*/) override {}

    CBackend& get_imm_command_list()
    {
        return get_imm_context().cmd_list;
    }

#if RENDER != R_R1
    // Frame driver: the SVP worker allocates contexts concurrently with the main thread - the
    // pool bookkeeping (bitset + reset) needs a lock. Held only for the bookkeeping, never
    // while recording.
    std::mutex contexts_lock{};

    ICF u32 alloc_context(bool alloc_cmd_list = true)
    {
        std::lock_guard<std::mutex> lock{ contexts_lock };
        if (contexts_used.all())
            return R_dsgraph_structure::INVALID_CONTEXT_ID;
        const auto raw = ~contexts_used.to_ulong();
        int id = 0;
        for (; id < R__NUM_PARALLEL_CONTEXTS; ++id) // TODO: ffs intrinsic
        {
            if (raw & (1u << id))
                break;
        }
        contexts_used.set(id, true);
        contexts_pool[id].reset();
        contexts_pool[id].context_id = id;
        contexts_pool[id].cmd_list.context_id = alloc_cmd_list ? id : CHW::IMM_CTX_ID;
        // The REAL deferred-context state is at D3D11 defaults (any previous user finished
        // its command list) - drop the stale CPU state cache so the new owner's first
        // recording re-emits everything. Without this, pool reuse leaks the previous user's
        // cached state (diffs get skipped) - the root cause of the intermittent mis-rendered
        // deferred segments (scene-dependent, disappears under RenderDoc).
        contexts_pool[id].cmd_list.ResetDeferredCache();
        return id;
    }

    ICF R_dsgraph_structure& get_context(u32 id)
    {
        VERIFY(id < R__NUM_CONTEXTS);
        if (id == R_dsgraph_structure::IMM_CTX_ID)
        {
            return get_imm_context();
        }
        VERIFY(contexts_used.test(id));
        VERIFY(contexts_pool[id].context_id == id);
        return contexts_pool[id];
    }

    ICF void release_context(u32 id)
    {
        std::lock_guard<std::mutex> lock{ contexts_lock };
        VERIFY(id != R_dsgraph_structure::IMM_CTX_ID); // never release immediate context
        VERIFY(id < R__NUM_PARALLEL_CONTEXTS);
        VERIFY(contexts_used.test(id));
        VERIFY(contexts_pool[id].context_id != R_dsgraph_structure::INVALID_CONTEXT_ID);
        contexts_used.set(id, false);
    }

    ICF R_dsgraph_structure& get_imm_context()
    {
        auto& ctx = contexts_pool[R_dsgraph_structure::IMM_CTX_ID];
        ctx.context_id = R_dsgraph_structure::IMM_CTX_ID;
        contexts_used.set(ctx.context_id, true);
        return ctx;
    }

    ICF void cleanup_contexts()
    {
        for (int id = 0; id < R__NUM_CONTEXTS; ++id)
        {
            contexts_pool[id].reset();
        }
        contexts_used.reset();
    }
#else
    ICF R_dsgraph_structure& get_imm_context()
    {
        context_imm.context_id = R_dsgraph_structure::IMM_CTX_ID;
        return context_imm;
    }

    ICF R_dsgraph_structure& get_context(u32 id)
    {
        VERIFY(id == R_dsgraph_structure::IMM_CTX_ID); // be sure R1 doesn't go crazy
        return get_imm_context();
    }

    ICF void cleanup_contexts()
    {
        context_imm.reset();
    }
#endif

    void CreateQuadIB();

public:
    CResourceManager* Resources{};
    ref_shader m_WireShader;
    ref_shader m_SelectionShader;
    ref_shader m_PortalFadeShader;
    ref_geom   m_PortalFadeGeom;

    // Dynamic geometry streams
    _VertexStream Vertex;
    _IndexStream Index;

    IndexStagingBuffer QuadIB;
    IndexBufferHandle old_QuadIB;

protected:
#if RENDER == R_R1
    R_dsgraph_structure context_imm;
#else
    R_dsgraph_structure contexts_pool[R__NUM_CONTEXTS];
    std::bitset<R__NUM_CONTEXTS> contexts_used{};
#endif
private:
    CGammaControl m_Gamma;

protected:
    bool b_loaded{};
};
} // namespace xray::render::RENDER_NAMESPACE
