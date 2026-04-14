#pragma once

#include <mutex>

#include "xrCDB/xr_collide_defs.h"
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

class ENGINE_API Vision : private pure_relcase
{
    friend class pure_relcase;

private:
    xr_vector<IGameObject*> seen;
    xr_vector<IGameObject*> query;
    xr_vector<IGameObject*> diff;
    collide::rq_results RQR;
    xr_vector<ISpatial*> r_spatial;
    IGameObject const* m_owner;
    u32 m_trace_cursor{0};
    mutable std::recursive_mutex m_vision_mtx;

    void o_new(IGameObject* E);
    void o_delete(IGameObject* E);
    /** `trace_pass_cap == ~0u`: use `npc_perf_vision_trace_budget` (legacy). Else: `min(total, trace_pass_cap)` (0 = no rays). */
    void o_trace(Fvector& P, float dt, float vis_threshold, u32 trace_pass_cap);
    void feel_vision_merge_after_frustum(IGameObject* parent);

public:
    Vision(IGameObject const* owner);
    virtual ~Vision();
    struct feel_visible_Item
    {
        collide::ray_cache Cache;
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
};

// AI LOS / feel_vision ray tuning (defaults = vanilla behavior; see console_commands / user.ltx)
ENGINE_API extern int npc_perf_vision_trace_budget;       // max rays per feel_vision_update pass (was hardcoded 12)
ENGINE_API extern int npc_perf_vision_skip_dynamic_ray;   // 1 = skip dynamic cform ray pass after static query
ENGINE_API extern int npc_perf_vision_static_only;        // 1 = static geometry only in main RayQuery (cheaper, less accurate)
ENGINE_API extern float npc_perf_vision_cache_pos_slack_m; // >0 = reuse ray cache if eye moved less than this (meters)
