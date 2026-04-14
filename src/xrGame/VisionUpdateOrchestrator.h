#pragma once

#include "xrCore/xr_types.h"

class CCustomMonster;

/** Per-frame global ray budget for AI `Feel::Vision` tracing (see docs/AI_VISION_STAGED_PIPELINE.md). */
class CVisionUpdateOrchestrator
{
public:
    /** Call once per frame from main thread before GameThread vision work (e.g. start of `CLevel::OnFrame`). */
    static void BeginFrame();

    /** Clamp `requested_cap` by remaining global pool; boost combat stalkers by `npc_perf_vision_combat_min_rays`. */
    static u32 TakeRayBudget(CCustomMonster& monster, u32 requested_cap);
};
