#include "stdafx.h"
#include "r__occlusion.h"

#include "QueryHelper.h"

#include <new>

namespace xray::render::RENDER_NAMESPACE
{
R_occlusion::~R_occlusion(void) { occq_destroy(); }
void R_occlusion::occq_create(u32 limit)
{
    ZoneScoped;
    enabled = strstr(Core.Params, "-no_occq") ? false : true;
    pool.reserve(limit);
    used.reserve(limit);
    fids.reserve(limit);
    for (u32 it = 0; it < limit; it++)
    {
        Query q;
        q.order = it;
        if (FAILED(CreateQuery(&q.Q, D3D_QUERY_OCCLUSION)))
            break;
        pool.emplace_back(std::move(q));
    }
    std::reverse(pool.begin(), pool.end());
}
void R_occlusion::occq_destroy()
{
    for (const auto& q : used)
        ReleaseQuery(q.Q);
    for (const auto& q : pool)
        ReleaseQuery(q.Q);
    used.clear();
    pool.clear();
    fids.clear();
}

u32 R_occlusion::occq_begin(u32& ID, u32 context_id)
{
    if (!enabled)
        return 0;

    ScopeLock lock{ &render_lock };

    if (pool.empty())
    {
        const size_t sz = used.size();
        // Cap growth: pathological light counts (or desync) can push sz until xr_alloc returns
        // nullptr; vector then placement-new's at 0 (AV write in xalloc::construct).
        static constexpr size_t slot_hard_cap = size_t(occq_size) * 32u;
        if (sz >= slot_hard_cap)
        {
            if ((Device.dwFrame % 40) == 0)
                Msg("! R_occlusion: occlusion slot cap reached (%zu); use -no_occq or reduce lights.", slot_hard_cap);
            ID = iInvalidHandle;
            return 0;
        }

        Query q;
        q.order = static_cast<u32>(sz);
        if (FAILED(CreateQuery(&q.Q, D3D_QUERY_OCCLUSION)))
        {
            if ((Device.dwFrame % 40) == 0)
                Msg(" RENDER [Warning]: Too many occlusion queries were issued (>%zu)!!!", sz);
            ID = iInvalidHandle;
            return 0;
        }

        try
        {
            if (sz == used.capacity())
            {
                const size_t target_cap = sz + size_t(occq_size_base);
                if (target_cap < sz)
                    throw std::bad_alloc{};
                used.reserve(target_cap);
                pool.reserve(target_cap);
            }
            pool.emplace(pool.begin(), std::move(q));
        }
        catch (...)
        {
            ReleaseQuery(q.Q);
            if ((Device.dwFrame % 40) == 0)
                Msg("! R_occlusion: failed to grow occlusion query storage (out of memory?).");
            ID = iInvalidHandle;
            return 0;
        }
    }

    RImplementation.BasicStats.OcclusionQueries++;
    if (!fids.empty())
    {
        ID = fids.back();
        fids.pop_back();
        used[ID] = std::move(pool.back());
    }
    else
    {
        ID = static_cast<u32>(used.size());
        used.emplace_back(std::move(pool.back()));
    }
    pool.pop_back();
#if defined(USE_DX11)
    CHK_DX(BeginQueryCtx(HW.get_context(context_id), used[ID].Q));
#else
    CHK_DX(BeginQuery(used[ID].Q));
#endif

    return used[ID].order;
}
void R_occlusion::occq_end(u32& ID, u32 context_id)
{
    if (!enabled || ID == iInvalidHandle)
        return;

    ScopeLock lock{ &render_lock };

    // Stale ID after occq_destroy (vid_restart / reset) or double occq_end: slot empty or query already released.
    if (ID >= used.size())
    {
        ID = 0;
        return;
    }
#if defined(USE_DX11)
    if (!used[ID].Q)
#else
    if (used[ID].Q == 0)
#endif
    {
        ID = 0;
        return;
    }

#if defined(USE_DX11)
    CHK_DX(EndQueryCtx(HW.get_context(context_id), used[ID].Q));
#else
    CHK_DX(EndQuery(used[ID].Q));
#endif
}
R_occlusion::occq_result R_occlusion::occq_get(u32& ID)
{
    if (!enabled || ID == iInvalidHandle)
        return 0xffffffff;

    ScopeLock lock{ &render_lock };

    // Return 0 (culled): do not use 0xffffffff here — light::vis_update treats that as "visible"
    // and can draw lights with stale state after occq_destroy / bad slot (ref_shader copy AV).
    if (ID >= used.size())
    {
        ID = 0;
        return 0;
    }
#if defined(USE_DX11)
    if (!used[ID].Q)
#else
    if (used[ID].Q == 0)
#endif
    {
        ID = 0;
        return 0;
    }

    occq_result fragments = 0;
    HRESULT hr;
    CTimer T;
    T.Start();
    RImplementation.BasicStats.Wait.Begin();
    while ((hr = GetData(used[ID].Q, &fragments, sizeof(fragments))) == S_FALSE)
    {
        if (!SwitchToThread())
            Sleep(ps_r2_wait_sleep);

        if (T.GetElapsed_ms() > 500)
        {
            fragments = (occq_result)-1; // 0xffffffff;
            break;
        }
    }
    RImplementation.BasicStats.Wait.End();

    if (0 == fragments)
        RImplementation.BasicStats.OcclusionCulled++;

    // insert into pool (sorting in decreasing order)
    Query& Q = used[ID];
    if (pool.empty())
        pool.emplace_back(Q);
    else
    {
        int it = int(pool.size()) - 1;
        while ((it >= 0) && (pool[it].order < Q.order))
            it--;
        pool.emplace(pool.begin() + it + 1, std::move(Q));
    }

    // remove from used and shrink as nesessary
    used[ID].Q = 0;
    fids.emplace_back(ID);
    ID = 0;
    return fragments;
}

bool R_occlusion::occq_try_get(u32& ID, occq_result& fragments)
{
    if (!enabled || ID == iInvalidHandle)
    {
        fragments = 0xffffffff;
        return true;
    }

    ScopeLock lock{ &render_lock };

    if (ID >= used.size())
    {
        ID = 0;
        fragments = 0;
        return true;
    }
#if defined(USE_DX11)
    if (!used[ID].Q)
#else
    if (used[ID].Q == 0)
#endif
    {
        ID = 0;
        fragments = 0;
        return true;
    }

    fragments = 0;
    if (GetData(used[ID].Q, &fragments, sizeof(fragments)) == S_FALSE)
        return false; // not ready: do NOT touch the query, retry later

    if (0 == fragments)
        RImplementation.BasicStats.OcclusionCulled++;

    // insert into pool (sorting in decreasing order)
    Query& Q = used[ID];
    if (pool.empty())
        pool.emplace_back(Q);
    else
    {
        int it = int(pool.size()) - 1;
        while ((it >= 0) && (pool[it].order < Q.order))
            it--;
        pool.emplace(pool.begin() + it + 1, std::move(Q));
    }

    // remove from used and shrink as nesessary
    used[ID].Q = 0;
    fids.emplace_back(ID);
    ID = 0;
    return true;
}
} // namespace xray::render::RENDER_NAMESPACE
