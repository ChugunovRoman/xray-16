#pragma once

#include "Engine.h"

class CPS_Instance;

/** Async particle batch for `PreRenderThread` / `Device.ParticleWorkerCallback` (ixray-style). */

ENGINE_API void ParticleWorker_Enqueue(CPS_Instance* inst);
void ParticleWorker_BeginFrameCollect();
void ParticleWorker_RunBatch();
/** After `secondary_tasks.wait()`: clear batch and drop pending flags so pointers are not stale. */
void ParticleWorker_ShutdownBeforeNullCallback();
