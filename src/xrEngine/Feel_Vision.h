#pragma once

#include <mutex>

#include "xrCDB/xr_collide_defs.h"
#include "xrCDB/ISpatial.h"
#include "Render.h"
#include "pure_relcase.h"

class IRender_Sector;
class IGameObject;
class ISpatial;

namespace Feel
{
const float fuzzy_update_vis = 1000.f; // speed of fuzzy-logic desisions
const float fuzzy_update_novis = 1000.f; // speed of fuzzy-logic desisions
const float fuzzy_guaranteed = 0.001f; // distance which is supposed 100% visible
const float lr_granularity = 0.1f; // assume similar positions

IC bool feel_vision_ray_cache_reuse(const collide::ray_cache& C, const Fvector& P, const Fvector& D, float range,
    float slack_m)
{
    if (slack_m <= 0.f)
        return C.similar(P, D, range);
    const float slack2 = slack_m * slack_m;
    if (P.distance_to_sqr(C.start) > slack2)
        return false;
    if (!fsimilar(1.f, D.dotproduct(C.dir)))
        return false;
    if (!fsimilar(range, C.range))
        return false;
    return true;
}

class ENGINE_API Vision : private pure_relcase
{
    friend class pure_relcase;

protected:
    xr_vector<IGameObject*> seen;
    xr_vector<IGameObject*> query;
    xr_vector<IGameObject*> diff;
    collide::rq_results RQR;
    xr_vector<ISpatial*> r_spatial;
    IGameObject const* m_owner;
    u32 m_trace_cursor{0};
    mutable std::recursive_mutex m_vision_mtx;

private:

    void o_new(IGameObject* E);
    void o_delete(IGameObject* E);
    /** `trace_pass_cap` = hard cap on rays this pass (`0` = merge only). No cvar budget. */
    void o_trace(Fvector& P, float dt, float vis_threshold, u32 trace_pass_cap);

protected:
    void feel_vision_merge_after_frustum(IGameObject* parent);

public:
    Vision(IGameObject const* owner);
    virtual ~Vision();
    struct dynamic_blocker_cache
    {
        bool valid = false;
        bool blocked = false;
        u32 scene_version = 0;
        Fvector start;
        Fvector dir;
        float range = 0.f;
    };

    struct feel_visible_Item
    {
        collide::ray_cache Cache;
        dynamic_blocker_cache dynamic_cache;
        Fvector cp_LP;
        Fvector cp_LR_src;
        Fvector cp_LR_dst;
        Fvector cp_LAST; // last point found to be visible
        IGameObject* O;
        float fuzzy; // note range: (-1[no]..1[yes])
        float Cache_vis;
        u16 bone_id;
    };
    xr_vector<feel_visible_Item> feel_visible;

public:
    void feel_vision_clear();
    void feel_vision_query(Fmatrix& mFull, Fvector& P);
    void feel_vision_update(IGameObject* parent, Fvector& P, float dt, float vis_threshold);
    /** Same merge as `feel_vision_update`, then trace at most `trace_pass_cap` rays (`0` = merge only). */
    void feel_vision_update_staged(IGameObject* parent, Fvector& P, float dt, float vis_threshold, u32 trace_pass_cap);
    void feel_vision_relcase(IGameObject* object);
    void feel_vision_get(xr_vector<IGameObject*>& R)
    {
        std::lock_guard<std::recursive_mutex> lock(m_vision_mtx);
        R.clear();
        xr_vector<feel_visible_Item>::iterator I = feel_visible.begin(), E = feel_visible.end();
        for (; I != E; ++I)
            if (positive(I->fuzzy))
                R.push_back(I->O);
    }
    Fvector feel_vision_get_vispoint(IGameObject* _O)
    {
        std::lock_guard<std::recursive_mutex> lock(m_vision_mtx);
        xr_vector<feel_visible_Item>::iterator I = feel_visible.begin(), E = feel_visible.end();
        for (; I != E; ++I)
            if (_O == I->O)
            {
                VERIFY(positive(I->fuzzy));
                return I->cp_LAST;
            }
        VERIFY2(0, "There is no such object in the potentially visible list");
        return Fvector().set(flt_max, flt_max, flt_max);
    }
    virtual bool feel_vision_isRelevant(IGameObject* O) = 0;
    virtual float feel_vision_mtl_transp(IGameObject* O, u32 element) = 0;
};

// B-1: dynamic-blocker cache helpers (used by Feel_Vision.cpp and CustomMonster.cpp)
IC bool feel_vision_dynamic_cache_reuse(const Vision::dynamic_blocker_cache& cache, const Fvector& P,
    const Fvector& D, float range, float slack_m)
{
    if (!cache.valid)
        return false;
    if (cache.scene_version != g_spatial_visible_for_ai_version)
        return false;
    if (slack_m <= 0.f)
    {
        if (!cache.start.similar(P) || !cache.dir.similar(D) || !fsimilar(cache.range, range))
            return false;
    }
    else
    {
        const float slack2 = slack_m * slack_m;
        if (P.distance_to_sqr(cache.start) > slack2)
            return false;
        if (!fsimilar(1.f, D.dotproduct(cache.dir)))
            return false;
        if (!fsimilar(range, cache.range))
            return false;
    }
    return true;
}

IC void feel_vision_dynamic_cache_store(Vision::dynamic_blocker_cache& cache, const Fvector& P, const Fvector& D,
    float range, bool blocked)
{
    cache.valid = true;
    cache.blocked = blocked;
    cache.scene_version = g_spatial_visible_for_ai_version;
    cache.start = P;
    cache.dir = D;
    cache.range = range;
}

};

// AI LOS / feel_vision ray tuning (defaults = vanilla behavior; see console_commands / user.ltx)
ENGINE_API extern int npc_perf_vision_skip_dynamic_ray;   // 1 = skip dynamic cform ray pass after static query
ENGINE_API extern int npc_perf_vision_static_only;        // 1 = static geometry only in main RayQuery (cheaper, less accurate)
ENGINE_API extern float npc_perf_vision_cache_pos_slack_m; // >0 = reuse ray cache if eye moved less than this (meters)
ENGINE_API extern int npc_perf_vision_rays_per_npc;       // hard cap on rays per NPC per vision pass
ENGINE_API extern int npc_perf_vision_dynamic_cache;      // 1 = cache dynamic blocker result
