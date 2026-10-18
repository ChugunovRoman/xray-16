#pragma once

#include <atomic>

#include "ParticleEffectDef.h"
#ifdef _EDITOR
#include "Layers/xrRender/FBasicVisual.h"
#include "Layers/xrRender/dxParticleCustom.h"
#else // _EDITOR
#include "Layers/xrRender/FBasicVisual.h"
#include "Layers/xrRender/dxParticleCustom.h"
#endif // _EDITOR

namespace PAPI
{
struct ParticleEffect;
}

namespace xray::render::RENDER_NAMESPACE
{
namespace PS
{
class ECORE_API CParticleEffect : public dxParticleCustom
{
    //		friend void ParticleRenderStream( LPVOID lpvParams );
    friend class CPEDef;

protected:
    float m_fElapsedLimit;

    int m_HandleEffect;
    int m_HandleActionList;

    s32 m_MemDT;

    Fvector m_InitialPosition;

    // Particle count as of the last OnFrame step / Stop / Compile - the only places that change it (births and
    // deaths happen in ParticleManager()->Update and ExecuteCollision inside OnFrame, StopEffect kills all).
    // Read lock-free by the render (IsEmptyForRender) instead of GetParticlesCount, which takes the global pm_lock
    // that the PreRender particle worker hammers. Same staleness as reading p_count while a step is in flight.
    std::atomic<u32> m_RenderParticleCount{ 0 };

    // 27-render-main.md (R1): the particle manager's effect of m_HandleEffect, taken once in the
    // constructor. It lives until DestroyEffect in the destructor and its particle array moves only in Compile
    // (SetMaxParticles, main thread), so p_count / particles are read through it without GetParticles - pm_lock only
    // guards the manager's effect vector against a concurrent CreateEffect, not the effect itself.
    PAPI::ParticleEffect* m_PE{};

public:
    CPEDef* m_Def;
    Fmatrix m_XFORM;

protected:
    DestroyCallback m_DestroyCallback;
    CollisionCallback m_CollisionCallback;

public:
    enum
    {
        flRT_Playing = (1 << 0),
        flRT_DefferedStop = (1 << 1),
        flRT_XFORM = (1 << 2),
        flRT_HUDmode = (1 << 3),
    };
    Flags8 m_RT_Flags;

protected:
    BOOL SaveActionList(IWriter& F);
    BOOL LoadActionList(IReader& F);

    void RefreshShader();

public:
    CParticleEffect();
    virtual ~CParticleEffect();

    void OnFrame(u32 dt);

    u32 RenderTO();
    virtual void Render(CBackend& cmd_list, float LOD, bool use_fast_geo) override;

    // 27-render-main.md (R1): the sorted pass's particle batch (R_dsgraph_structure::render_sorted).
    // BatchPrepare - main thread: one snapshot of the particles (no pm_lock); false - this effect stays on Render()
    // (HUD mode - it switches the projection, not a sprite, no particles). BatchFill - any thread: the sprites of
    // particles [begin, end) of the snapshot into pv_start (the effect's first vertex), 4 vertices per particle.
    // BatchDraw - main thread: the tail of Render() for vertices already in the stream (count - of one or of several
    // adjacent effects with this one's ShaderElement and cull mode).
    struct BatchSnapshot
    {
        PAPI::Particle* particles{};
        u32 count{};
    };
    bool BatchPrepare(BatchSnapshot& snapshot) const;
    void BatchFill(FVF::LIT* pv_start, const BatchSnapshot& snapshot, u32 begin, u32 end) const;
    u32 BatchCullMode() const;
    void BatchDraw(CBackend& cmd_list, u32 vertex_offset, u32 vertex_count);

private:
    void ParticleRenderStream(FVF::LIT* pv_start, u32 count, PAPI::Particle* particles);
    // The sprites of particles [begin, end) into pv_start + begin * 4 (no metrics: also runs on task workers).
    void FillSprites(FVF::LIT* pv_start, u32 begin, u32 end, PAPI::Particle* particles) const;

public:
    virtual void Copy(dxRender_Visual* pFrom);

    virtual void OnDeviceCreate();
    virtual void OnDeviceDestroy();

    virtual void UpdateParent(const Fmatrix& m, const Fvector& velocity, BOOL bXFORM);

    BOOL Compile(CPEDef* def);

    CPEDef* GetDefinition() const { return m_Def; }
    int GetHandleEffect() const { return m_HandleEffect; }
    int GetHandleActionList() { return m_HandleActionList; }
    virtual void Play();
    virtual void Stop(BOOL bDefferedStop = TRUE);
    virtual BOOL IsPlaying() { return m_RT_Flags.is(flRT_Playing); }
    virtual void SetHudMode(BOOL b) { m_RT_Flags.set(flRT_HUDmode, b); }
    virtual BOOL GetHudMode() { return m_RT_Flags.is(flRT_HUDmode); }
    virtual float GetTimeLimit()
    {
        VERIFY(m_Def);
        return m_Def->m_Flags.is(CPEDef::dfTimeLimit) ? m_Def->m_fTimeLimit : -1.f;
    }

    virtual const shared_str Name()
    {
        VERIFY(m_Def);
        return m_Def->m_Name;
    }

    void SetDestroyCB(DestroyCallback destroy_cb) { m_DestroyCallback = destroy_cb; }
    void SetCollisionCB(CollisionCallback collision_cb) { m_CollisionCallback = collision_cb; }
    void SetBirthDeadCB(PAPI::OnBirthParticleCB bc, PAPI::OnDeadParticleCB dc, void* owner, u32 p) const;

    virtual u32 ParticlesCount();

    // Render() would draw nothing: no sprite definition or no particles alive (not started yet, finished, between
    // bursts). Lets the dsgraph keep such effects out of mapSorted/mapDistort/... (plans/optimization_mt/18-combine-pass.md, E3).
    bool IsEmptyForRender() const
    {
        return !m_Def || !m_Def->m_Flags.is(CPEDef::dfSprite) ||
            m_RenderParticleCount.load(std::memory_order_relaxed) == 0;
    }
};
void OnEffectParticleBirth(void* owner, u32 param, PAPI::Particle& m, u32 idx);
void OnEffectParticleDead(void* owner, u32 param, PAPI::Particle& m, u32 idx);

extern const u32 uDT_STEP;
extern const float fDT_STEP;
}
//---------------------------------------------------------------------------
} // namespace xray::render::RENDER_NAMESPACE
