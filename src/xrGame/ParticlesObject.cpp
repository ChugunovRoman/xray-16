#include "StdAfx.h"

#include "ParticlesObject.h"
#include "xrEngine/defines.h"
#include "xrEngine/ParticleWorker.h"
#include "Include/xrRender/RenderVisual.h"
#include "Include/xrRender/ParticleCustom.h"
#include "xrEngine/Render.h"
#include "xrEngine/IGame_Persistent.h"

const Fvector zero_vel = {0.f, 0.f, 0.f};

CParticlesObject::CParticlesObject(LPCSTR p_name, BOOL bAutoRemove, bool destroy_on_game_load)
    : inherited(destroy_on_game_load)
{
    Init(p_name, IRender_Sector::INVALID_SECTOR_ID, bAutoRemove);
}

void CParticlesObject::Init(LPCSTR p_name, IRender_Sector::sector_id_t sector_id, BOOL bAutoRemove)
{
    m_bLooped = false;
    m_bStopping = false;
    m_bAutoRemove = bAutoRemove;
    float time_limit = 0.0f;

    if (!GEnv.isDedicatedServer)
    {
        // create visual
        renderable.visual = GEnv.Render->model_CreateParticles(p_name);
        VERIFY(renderable.visual);
        IParticleCustom* V = smart_cast<IParticleCustom*>(renderable.visual);
        VERIFY(V);
        time_limit = V->GetTimeLimit();
    }
    else
    {
        time_limit = 1.0f;
    }

    if (time_limit > 0.f)
    {
        m_iLifeTime = iFloor(time_limit * 1000.f);
    }
    else
    {
        if (bAutoRemove)
        {
            R_ASSERT3(!m_bAutoRemove, "Can't set auto-remove flag for looped particle system.", p_name);
        }
        else
        {
            m_iLifeTime = 0;
            m_bLooped = true;
        }
    }

    // spatial
    spatial.type = 0;
    spatial.sector_id = sector_id;

    // sheduled
    shedule.t_min = 20;
    shedule.t_max = 50;
    shedule_register();

    dwLastTime = Device.dwTimeGlobal;
    m_asyncParticlePendingDt = 0;
}

//----------------------------------------------------
CParticlesObject::~CParticlesObject()
{
    VERIFY(0 == m_asyncParticlePendingDt);

    //	we do not need this since CPS_Instance does it
    //	shedule_unregister		();
}

void CParticlesObject::UpdateSpatial()
{
    if (GEnv.isDedicatedServer)
        return;

    // spatial	(+ workaround occasional bug inside particle-system)
    vis_data& vis = renderable.visual->getVisData();
    if (_valid(vis.sphere))
    {
        Fvector P;
        float R;
        renderable.xform.transform_tiny(P, vis.sphere.P);
        R = vis.sphere.R;
        if (0 == spatial.type)
        {
            // First 'valid' update - register
            spatial.type = STYPE_RENDERABLE;
            spatial.sphere.set(P, R);
            spatial_register();
        }
        else
        {
            BOOL bMove = FALSE;
            if (!P.similar(spatial.sphere.P, EPS_L * 10.f))
                bMove = TRUE;
            if (!fsimilar(R, spatial.sphere.R, 0.15f))
                bMove = TRUE;
            if (bMove)
            {
                spatial.sphere.set(P, R);
                spatial_move();
            }
        }
    }
}

const shared_str CParticlesObject::Name()
{
    if (GEnv.isDedicatedServer)
        return "";

    IParticleCustom* V = smart_cast<IParticleCustom*>(renderable.visual);
    VERIFY(V);
    return (V) ? V->Name() : "";
}

//----------------------------------------------------
void CParticlesObject::Play(bool bHudMode)
{
    if (GEnv.isDedicatedServer)
        return;

    IParticleCustom* V = smart_cast<IParticleCustom*>(renderable.visual);
    VERIFY(V);
    if (bHudMode)
        V->SetHudMode(bHudMode);

    V->Play();
    dwLastTime = Device.dwTimeGlobal - 33ul;
    m_asyncParticlePendingDt = 0;
    PerformAllTheWork(0);
    m_bStopping = false;
}

void CParticlesObject::play_at_pos(const Fvector& pos, BOOL xform)
{
    if (GEnv.isDedicatedServer)
        return;

    IParticleCustom* V = smart_cast<IParticleCustom*>(renderable.visual);
    VERIFY(V);
    Fmatrix m;
    m.translate(pos);
    V->UpdateParent(m, zero_vel, xform);
    V->Play();
    dwLastTime = Device.dwTimeGlobal - 33ul;
    m_asyncParticlePendingDt = 0;
    PerformAllTheWork(0);
    m_bStopping = false;
}

void CParticlesObject::Stop(BOOL bDefferedStop)
{
    if (GEnv.isDedicatedServer)
        return;

    m_asyncParticlePendingDt = 0;

    IParticleCustom* V = smart_cast<IParticleCustom*>(renderable.visual);
    VERIFY(V);
    V->Stop(bDefferedStop);
    m_bStopping = true;
}

void CParticlesObject::shedule_Update(u32 _dt)
{
    inherited::shedule_Update(_dt);

    if (GEnv.isDedicatedServer)
        return;

    // Update
    if (m_bDead)
        return;

    if (psDeviceFlags.test(mtParticles))
    {
        UpdateSpatial();
        return;
    }

    const u32 dt = Device.dwTimeGlobal - dwLastTime;
    if (dt)
    {
        IParticleCustom* V = smart_cast<IParticleCustom*>(renderable.visual);
        VERIFY(V);
        V->OnFrame(dt);
        dwLastTime = Device.dwTimeGlobal;
    }
    UpdateSpatial();
}

void CParticlesObject::PerformAllTheWork(u32 _dt)
{
    if (GEnv.isDedicatedServer)
        return;

    m_asyncParticlePendingDt = 0;

    // Update
    u32 dt = Device.dwTimeGlobal - dwLastTime;
    if (dt)
    {
        IParticleCustom* V = smart_cast<IParticleCustom*>(renderable.visual);
        VERIFY(V);
        V->OnFrame(dt);
        dwLastTime = Device.dwTimeGlobal;
    }
    UpdateSpatial();
}

void CParticlesObject::PSI_destroy()
{
    if (!m_bDead)
        m_deadFrame = Device.dwFrame;
    inherited::PSI_destroy();
}

void CParticlesObject::AsyncParticle_PreWorkerCollect()
{
    if (GEnv.isDedicatedServer)
        return;
    if (m_bDead)
    {
        // Guard (plans/optimization_mt/38-frozen-particles.md). A dead instance is not stepped any more (here and in
        // ParticleWorker_ApplyFrame) but stays in the spatial DB and is drawn by renderable_Render until
        // OnFrameBeforePreRender deletes it through ps_destroy - at the start of the next frame. Still here two frames
        // later, it is not in ps_destroy (the only other case, Locked(), is a ps_destroy entry waiting and logs
        // "--locked"): its last particles hang in the air for good. Runs on main at the start of the frame, after the
        // ps_destroy loop and with no other thread touching the lists, so the entry is put back safely.
        if (Device.dwFrame - m_deadFrame > 2)
        {
            auto& destroy = g_pGamePersistent->ps_destroy;
            if (std::find(destroy.begin(), destroy.end(), this) == destroy.end())
            {
                if (!m_deadOrphanReported)
                {
                    m_deadOrphanReported = true;
                    Msg("! [particles] '%s' dead since frame %u is not in ps_destroy: queued again", Name().c_str(),
                        m_deadFrame);
                }
                destroy.push_back(this);
            }
        }
        return;
    }
    if (!psDeviceFlags.test(mtParticles))
        return;
    // One snapshot of the time for dt and for dwLastTime (plans/optimization_mt/38-frozen-particles.md): this runs at
    // the start of the frame, before FrameMove moves Device.dwTimeGlobal on, while ParticleWorker_ApplyFrame runs on the
    // PreRender task beside FrameMove and used to read the time anew - usually the new one. The next collect then found
    // dt == 0 and the interval of the previous frame was never simulated: particles ran at about half speed against the
    // real-time lifetime of the object.
    const u32 now = Device.dwTimeGlobal;
    const u32 dt = now - dwLastTime;
    if (!dt)
        return;
    m_asyncParticleCollectTime = now;
    m_asyncParticlePendingDt = dt;
    ParticleWorker_Enqueue(this);
}

void CParticlesObject::ParticleWorker_ApplyFrame()
{
    if (GEnv.isDedicatedServer || m_bDead)
    {
        m_asyncParticlePendingDt = 0;
        return;
    }
    if (!psDeviceFlags.test(mtParticles))
    {
        m_asyncParticlePendingDt = 0;
        return;
    }
    if (!m_asyncParticlePendingDt)
        return;
    IParticleCustom* V = smart_cast<IParticleCustom*>(renderable.visual);
    if (!V)
    {
        m_asyncParticlePendingDt = 0;
        return;
    }
    V->OnFrame(m_asyncParticlePendingDt);
    dwLastTime = m_asyncParticleCollectTime; // the time dt was measured up to, not the current one (see the collect)
    m_asyncParticlePendingDt = 0;
}

void CParticlesObject::ParticleWorker_CancelPending()
{
    m_asyncParticlePendingDt = 0;
}

void CParticlesObject::SetXFORM(const Fmatrix& m)
{
    if (GEnv.isDedicatedServer)
        return;

    IParticleCustom* V = smart_cast<IParticleCustom*>(renderable.visual);
    VERIFY(V);
    V->UpdateParent(m, zero_vel, TRUE);
    renderable.xform.set(m);
    UpdateSpatial();
}

void CParticlesObject::UpdateParent(const Fmatrix& m, const Fvector& vel)
{
    if (GEnv.isDedicatedServer)
        return;

    IParticleCustom* V = smart_cast<IParticleCustom*>(renderable.visual);
    VERIFY(V);
    V->UpdateParent(m, vel, FALSE);
    UpdateSpatial();
}

Fvector& CParticlesObject::Position() const
{
    if (GEnv.isDedicatedServer)
    {
        static Fvector _pos = Fvector().set(0, 0, 0);
        return _pos;
    }
    vis_data& vis = renderable.visual->getVisData();
    return vis.sphere.P;
}

float CParticlesObject::shedule_Scale() const
{
    if (GEnv.isDedicatedServer)
        return 5.0f;

    return Device.vCameraPosition.distance_to(Position()) / 200.f;
}

void CParticlesObject::renderable_Render(u32 context_id, IRenderable* root)
{
    if (!psDeviceFlags.test(rsDrawParticles))
        return;

    VERIFY(renderable.visual);
    if (!psDeviceFlags.test(mtParticles))
    {
        const auto dt = Device.dwTimeGlobal - dwLastTime;
        if (dt)
        {
            IParticleCustom* V = smart_cast<IParticleCustom*>(renderable.visual);
            VERIFY(V);
            V->OnFrame(dt);
            dwLastTime = Device.dwTimeGlobal;
        }
    }

    GEnv.Render->add_Visual(context_id, root, renderable.visual, renderable.xform);
}

bool CParticlesObject::IsAutoRemove()
{
    if (m_bAutoRemove)
        return true;
    else
        return false;
}
void CParticlesObject::SetAutoRemove(bool auto_remove)
{
    VERIFY(m_bStopping || !IsLooped());
    m_bAutoRemove = auto_remove;
}

//играются ли партиклы, отличается от PSI_Alive, тем что после
//остановки Stop партиклы могут еще доигрывать анимацию IsPlaying = true
bool CParticlesObject::IsPlaying()
{
    if (GEnv.isDedicatedServer)
        return false;

    IParticleCustom* V = smart_cast<IParticleCustom*>(renderable.visual);
    VERIFY(V);
    return !!V->IsPlaying();
}
