#include "stdafx.h"

#include "EngineThreading.h"
#include "device.h"
#include "Engine.h"
#include "IGame_Level.h"
#include "IGame_Persistent.h"
#include "xrSheduler.h"

#ifndef _EDITOR
#include "Environment.h"
#include "Rain.h"
#endif

#include "xrCore/Threading/ThreadUtil.h"

void XRay::Engine::PreRenderThread()
{
    ZoneScopedN("PreRenderThread");
    Threading::SetCurrentThreadName("Pre-Render");

    {
        ZoneScopedN("PreRenderThread/seqParallelRender");
        xr_vector<fastdelegate::FastDelegate0<>> batch;
        batch.swap(Device.seqParallelRender);
        for (auto& d : batch)
            d();
        batch.clear();
    }

#ifndef _EDITOR
    if (!GEnv.isDedicatedServer && g_pGamePersistent && g_pGamePersistent->pEnvironment &&
        g_pGamePersistent->pEnvironment->eff_Rain)
    {
        ZoneScopedN("PreRenderThread/RainUpdateItems");
        g_pGamePersistent->pEnvironment->eff_Rain->UpdateItems();
    }
#endif

    if (Device.ParticleWorkerCallback)
    {
        ZoneScopedN("PreRenderThread/ParticleWorker");
        Device.ParticleWorkerCallback();
    }
}

void XRay::Engine::GameThread()
{
    ZoneScopedN("GameThread");
    Threading::SetCurrentThreadName("Game");

    if (g_pGameLevel && g_pGameLevel->bReady)
    {
        ZoneScopedN("SoundEvent_Dispatch");
        g_pGameLevel->SoundEvent_Dispatch();
    }

    if (!Device.Paused())
    {
        ZoneScopedN("Sheduler_Update");
        ::Engine.Sheduler.Update();

        // Execute parallel vision batch after all NPCs have submitted rays.
        // Must run after scheduler (NPCs submit rays during Update) and only when unpaused.
        if (Device.PostSchedulerVisionBatch)
            Device.PostSchedulerVisionBatch();
    }

    {
        ZoneScopedN("ProcessParallelSequence");
        xr_vector<fastdelegate::FastDelegate0<>> parallel_batch;
        parallel_batch.swap(Device.seqParallel);
        for (u32 pit = 0; pit < parallel_batch.size(); pit++)
            parallel_batch[pit]();
        parallel_batch.clear();
    }

    {
        ZoneScopedN("seqFrameMT");
        Device.seqFrameMT.Process();
    }
}
