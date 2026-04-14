#include "pch_script.h"
#include "VisionUpdateOrchestrator.h"

#include "CustomMonster.h"
#include "ai/stalker/ai_stalker.h"
#include "performance_cvars.h"

#include "xrEngine/profiler.h"

#include <atomic>

namespace
{
constexpr u32 kUnlimitedRays = ~0u;
std::atomic<u32> s_remaining_rays{kUnlimitedRays};
} // namespace

void CVisionUpdateOrchestrator::BeginFrame()
{
    ZoneScopedN("vision/BeginFrame");
    if (npc_perf_vision_global_ray_budget > 0)
        s_remaining_rays.store(npc_perf_vision_global_ray_budget, std::memory_order_relaxed);
    else
        s_remaining_rays.store(kUnlimitedRays, std::memory_order_relaxed);
}

u32 CVisionUpdateOrchestrator::TakeRayBudget(CCustomMonster& monster, u32 requested_cap)
{
    const u32 want_base = _max(1u, requested_cap);
    u32 want = want_base;

    if (CAI_Stalker* stalker = smart_cast<CAI_Stalker*>(&monster))
    {
        if (stalker->g_Alive() && stalker->memory().enemy().selected() && npc_perf_vision_combat_min_rays > 0)
            want = _max(want, npc_perf_vision_combat_min_rays);
    }

    u32 rem = s_remaining_rays.load(std::memory_order_relaxed);
    if (rem == kUnlimitedRays)
        return want;

    const u32 take = _min(want, rem);
    if (take == 0)
        return 0;

    s_remaining_rays.fetch_sub(take, std::memory_order_relaxed);
    return take;
}
