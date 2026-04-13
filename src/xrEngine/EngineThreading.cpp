#include "stdafx.h"

#include "EngineThreading.h"
#include "device.h"
#include "Engine.h"
#include "IGame_Level.h"
#include "IGame_Persistent.h"
#include "xrSheduler.h"

#include "xrCore/Threading/ThreadUtil.h"

void XRay::Engine::PreRenderThread()
{
    ZoneScopedN("PreRenderThread");
    Threading::SetCurrentThreadName("Pre-Render");

    {
        ZoneScopedN("seqParallelRender");
        xr_vector<fastdelegate::FastDelegate0<>> batch;
        batch.swap(Device.seqParallelRender);
        for (auto& d : batch)
            d();
        batch.clear();
    }

    if (Device.ParticleWorkerCallback)
    {
        ZoneScopedN("ParticleWorker");
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
