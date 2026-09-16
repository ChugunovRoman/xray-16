#include "stdafx.h"
#include "ParticleWorker.h"

#include "defines.h"
#include "device.h"
#include "IGame_Persistent.h"
#include "PS_instance.h"
#include "xrCore/Threading/Lock.hpp"

#include <algorithm>

namespace
{
xr_vector<CPS_Instance*> s_particle_worker_batch;

// The batch is filled on the main thread in OnFrameBeforePreRender and walked on the PreRender
// worker, while the main thread keeps running FrameMove in parallel. FrameMove can destroy
// particles (net_Stop / Disconnect -> destroy_particles), so the batch must not be touched
// without this lock: it held raw CPS_Instance pointers and a deleted one turned into a call
// through a freed vtable - the AV at "executing location 0x0" inside ParticleWorker_RunBatch.
// The lock is held for the whole batch, so a destruction on the main thread waits for it.
Lock s_particle_worker_lock;
} // namespace

ENGINE_API void ParticleWorker_Enqueue(CPS_Instance* inst)
{
    if (!inst)
        return;

    s_particle_worker_lock.Enter();
    s_particle_worker_batch.push_back(inst);
    s_particle_worker_lock.Leave();
}

void ParticleWorker_Remove(CPS_Instance* inst)
{
    if (!inst)
        return;

    // Blocks while the worker is inside ParticleWorker_RunBatch, which is exactly the point:
    // the instance cannot be freed until the batch is done with it.
    s_particle_worker_lock.Enter();
    const auto I = std::find(s_particle_worker_batch.begin(), s_particle_worker_batch.end(), inst);
    if (I != s_particle_worker_batch.end())
        s_particle_worker_batch.erase(I);
    s_particle_worker_lock.Leave();
}

void ParticleWorker_BeginFrameCollect()
{
    ZoneScopedN("ParticleWorker_BeginFrameCollect");

    s_particle_worker_lock.Enter();
    s_particle_worker_batch.clear();
    s_particle_worker_lock.Leave();

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

    s_particle_worker_lock.Enter();
    for (CPS_Instance* inst : s_particle_worker_batch)
    {
        if (inst)
            inst->ParticleWorker_ApplyFrame();
    }
    s_particle_worker_lock.Leave();
}

void ParticleWorker_ShutdownBeforeNullCallback()
{
    s_particle_worker_lock.Enter();
    s_particle_worker_batch.clear();
    s_particle_worker_lock.Leave();

    if (!g_pGamePersistent)
        return;
#ifndef _EDITOR
    for (CPS_Instance* inst : g_pGamePersistent->ps_active)
        inst->ParticleWorker_CancelPending();
#endif
}
