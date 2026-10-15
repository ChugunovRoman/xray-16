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
    // 24-critical-path.md G6b: scratch of feel_vision_merge_after_frustum - the objects that became visible
    // (seen - query); `diff` then holds the ones that left (query - seen). Emptied before the merge returns.
    xr_vector<IGameObject*> m_diff_new;
    collide::rq_results RQR;
    xr_vector<ISpatial*> r_spatial;
    IGameObject const* m_owner;
    u32 m_trace_cursor{0};
    // G6b: the merge swaps `seen` into `query` instead of copying it, so `seen` holds a stale list until the next
    // feel_vision_query. A merge without a query in between is a no-op then - as in the vanilla copy merge, where
    // seen == query after a merge (relcase drops an object from both).
    bool m_seen_merged{false};
    mutable std::recursive_mutex m_vision_mtx;

private:

    /** `trace_pass_cap` = hard cap on rays this pass (`0` = merge only). No cvar budget. */
    void o_trace(Fvector& P, float dt, float vis_threshold, u32 trace_pass_cap);

protected:
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
        // G6b: cp_LP / bone_id / cp_LAST not chosen yet - the merge adds an item without them and leaves them to the
        // first reader (feel_vision_init_mesh_point). Fits in the tail padding after bone_id.
        bool mesh_point_pending;
    };
    xr_vector<feel_visible_Item> feel_visible;

public:
    void feel_vision_clear();
    void feel_vision_query(Fmatrix& mFull, Fvector& P);
    void feel_vision_update(IGameObject* parent, Fvector& P, float dt, float vis_threshold);
    /** Same merge as `feel_vision_update`, then trace at most `trace_pass_cap` rays (`0` = merge only). */
    void feel_vision_update_staged(IGameObject* parent, Fvector& P, float dt, float vis_threshold, u32 trace_pass_cap);
    void feel_vision_relcase(IGameObject* object);
    // P17: feel_vision_relcase for all objects of the batch - one lock, one pass per list
    void feel_vision_relcase_batch(const RelcaseBatch& batch);
    // G6b: choose the mesh point of an item added by the merge (get_new_local_point_on_mesh -> cp_LP / bone_id,
    // and cp_LAST if `with_last`). Every reader of cp_LP / bone_id / cp_LAST calls it when mesh_point_pending is set:
    // o_trace and CCustomMonster::eye_pp_s2_prepare (they compute cp_LAST themselves right after), get_vispoint, the
    // debug draw. Same thread and lock (m_vision_mtx) as the merge that added the item.
    void feel_vision_init_mesh_point(feel_visible_Item& item, bool with_last);
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
                if (I->mesh_point_pending) // G6b: positive fuzzy means traced, so normally never here
                    feel_vision_init_mesh_point(*I, true);
                return I->cp_LAST;
            }
        VERIFY2(0, "There is no such object in the potentially visible list");
        return Fvector().set(flt_max, flt_max, flt_max);
    }
    virtual bool feel_vision_isRelevant(IGameObject* O) = 0;
    virtual float feel_vision_mtl_transp(IGameObject* O, u32 element) = 0;
};

};

// AI LOS / feel_vision ray tuning (see console_commands / user.ltx). Not vanilla: there is no separate ray pass over
// dynamic occluders (vehicles/props) after the main query - a deliberate perf trade-off, "worse detection around movers".
ENGINE_API extern float npc_perf_vision_cache_pos_slack_m; // >0 = reuse ray cache if eye moved less than this (meters)
ENGINE_API extern int npc_perf_vision_rays_per_npc;       // hard cap on rays per NPC per vision pass
