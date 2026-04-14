#include "stdafx.h"
#include "ParticleWorker.h"

#include "defines.h"
#include "device.h"
#include "IGame_Persistent.h"
#include "PS_instance.h"

namespace
{
xr_vector<CPS_Instance*> s_particle_worker_batch;
} // namespace

ENGINE_API void ParticleWorker_Enqueue(CPS_Instance* inst)
{
    if (inst)
        s_particle_worker_batch.push_back(inst);
}

void ParticleWorker_BeginFrameCollect()
{
    ZoneScopedN("ParticleWorker_BeginFrameCollect");
    s_particle_worker_batch.clear();

    if (!g_pGamePersistent)
        return;
#ifndef _EDITOR
    if (GEnv.isDedicatedServer)
        return;
    if (!psDeviceFlags.test(mtParticles))
        return;

    for (CPS_Instance* inst : g_pGamePersistent->ps_active)
        inst->AsyncParticle_PreWorkerCollect();
#endif
}

void ParticleWorker_RunBatch()
{
    ZoneScopedN("ParticleWorker_RunBatch");

    if (GEnv.isDedicatedServer)
        return;

    for (CPS_Instance* inst : s_particle_worker_batch)
    {
        if (inst)
            inst->ParticleWorker_ApplyFrame();
    }
}

void ParticleWorker_ShutdownBeforeNullCallback()
{
    s_particle_worker_batch.clear();
    if (!g_pGamePersistent)
        return;
#ifndef _EDITOR
    for (CPS_Instance* inst : g_pGamePersistent->ps_active)
        inst->ParticleWorker_CancelPending();
#endif
}
