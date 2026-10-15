#include "stdafx.h"
#include "Feel_Vision.h"
#include "Render.h"
#include "xr_object.h"
#include "xr_collide_form.h"
#include "IGame_Level.h"
#include "IGame_Persistent.h"
#include "xrCDB/Intersect.hpp"
#include "xrCore/Profiling/PerfMetrics.hpp"

ENGINE_API float npc_perf_vision_cache_pos_slack_m = 0.55f;
ENGINE_API int npc_perf_vision_rays_per_npc = 24;

namespace Feel
{

Vision::Vision(IGameObject const* owner)
    : pure_relcase(&Vision::feel_vision_relcase, &Vision::feel_vision_relcase_batch), m_owner(owner) {}
Vision::~Vision() {}
struct SFeelParam
{
    Vision* parent;
    Vision::feel_visible_Item* item;
    float vis;
    float vis_threshold;
    SFeelParam(Vision* _parent, Vision::feel_visible_Item* _item, float _vis_threshold)
        : parent(_parent), item(_item), vis(1.f), vis_threshold(_vis_threshold)
    {
    }
};
IC bool feel_vision_callback(collide::rq_result& result, LPVOID params)
{
    SFeelParam* fp = (SFeelParam*)params;
    float vis = fp->parent->feel_vision_mtl_transp(result.O, result.element);
    fp->vis *= vis;
    if (NULL == result.O && fis_zero(vis))
    {
        CDB::TRI* T = g_pGameLevel->ObjectSpace.GetStaticTris() + result.element;
        Fvector* V = g_pGameLevel->ObjectSpace.GetStaticVerts();
        fp->item->Cache.verts[0].set(V[T->verts[0]]);
        fp->item->Cache.verts[1].set(V[T->verts[1]]);
        fp->item->Cache.verts[2].set(V[T->verts[2]]);
    }
    return (fp->vis > fp->vis_threshold);
}
// 24-critical-path.md G6b. What this defers (per item, from the call graph, not timed): CEntityAlive::get_new_local_point_on_mesh =
// smart_cast to IKinematics + two passes over the hit-bone surface areas (LL_GetBoneVisible per bone) + Random;
// CEntityAlive::get_last_local_point_on_mesh = IKinematics::Bone_GetAnimPos: the model lock + BoneChain_Calculate from
// the root to the bone (CLBone = blend of all animations of each bone, 5-10 bones deep) - microseconds per living
// object; CGameObject (physics items) - a random_dir and a bbox transform. The vanilla o_new paid both for every object
// entering the frustum, though o_trace recomputes cp_LAST before each use, and with ~2000 items and a 24-ray budget
// most new items leave the frustum before their first ray (ai/vision/mesh_point_skipped).
void Vision::feel_vision_init_mesh_point(feel_visible_Item& item, bool with_last)
{
    item.mesh_point_pending = false;
    item.cp_LP = item.O->get_new_local_point_on_mesh(item.bone_id);
    if (with_last)
        item.cp_LAST = item.O->get_last_local_point_on_mesh(item.cp_LP, item.bone_id);
    PERF_INC(vision_mesh_point_lazy);
}

void Vision::feel_vision_clear()
{
    std::lock_guard<std::recursive_mutex> lock(m_vision_mtx);
    seen.clear();
    query.clear();
    diff.clear();
    m_diff_new.clear();
    m_seen_merged = false;
    feel_visible.clear();
}

void Vision::feel_vision_relcase(IGameObject* object)
{
    std::lock_guard<std::recursive_mutex> lock(m_vision_mtx);
    xr_vector<IGameObject*>::iterator Io;
    Io = std::find(seen.begin(), seen.end(), object);
    if (Io != seen.end())
        seen.erase(Io);
    Io = std::find(query.begin(), query.end(), object);
    if (Io != query.end())
        query.erase(Io);
    Io = std::find(diff.begin(), diff.end(), object);
    if (Io != diff.end())
        diff.erase(Io);
    xr_vector<feel_visible_Item>::iterator Ii = feel_visible.begin(), IiE = feel_visible.end();
    for (; Ii != IiE; ++Ii)
        if (Ii->O == object)
        {
            feel_visible.erase(Ii);
            break;
        }
}

// Same result as feel_vision_relcase for every object of the batch (it removes the first match from each list, there is
// at most one): the lists only lose pointers, nothing is called back.
void Vision::feel_vision_relcase_batch(const RelcaseBatch& batch)
{
    PERF_SCOPE(relcase_cb_vision);
    std::lock_guard<std::recursive_mutex> lock(m_vision_mtx);
    const auto in_batch = [&batch](const IGameObject* object) { return batch.contains(object); };
    const size_t query_size = query.size();
    seen.erase(std::remove_if(seen.begin(), seen.end(), in_batch), seen.end());
    query.erase(std::remove_if(query.begin(), query.end(), in_batch), query.end());
    diff.erase(std::remove_if(diff.begin(), diff.end(), in_batch), diff.end());
    // P17b (always): feel_visible holds exactly the objects of `query` - only feel_vision_merge_after_frustum adds to it
    // (seen - query) and removes from it (query - seen, then `seen` becomes `query`),
    // relcase drops an object from both, feel_vision_clear empties both, all under m_vision_mtx. So if no batch object
    // left `query`, none is in feel_visible, and its walk (an item of ~180 bytes per object in sight, thousands of cache
    // lines per frame over all NPCs) is skipped. The sizes must agree, otherwise it is walked as before.
    if (query.size() == query_size && feel_visible.size() == query_size)
    {
        PERF_INC(relcase_vision_visible_skips);
        return;
    }
    feel_visible.erase(std::remove_if(feel_visible.begin(), feel_visible.end(),
                           [&batch](const feel_visible_Item& item) { return batch.contains(item.O); }),
        feel_visible.end());
}

void Vision::feel_vision_query(Fmatrix& mFull, Fvector& P)
{
    std::lock_guard<std::recursive_mutex> lock(m_vision_mtx);
    CFrustum Frustum;
    Frustum.CreateFromMatrix(mFull, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);

    // Traverse object database
    {
        PERF_SCOPE(stk_vision_s1_query); // 24-critical-path.md M10
        r_spatial.clear();
        SpatialCaller spatial_caller(SpatialQueryCaller::vision);
        g_pGamePersistent->SpatialSpace.q_frustum(r_spatial, 0, STYPE_VISIBLEFORAI, Frustum);
    }
    PERF_ADD(vision_query_results, r_spatial.size());

    // Determine visibility for dynamic part of scene
    PERF_SCOPE(stk_vision_s1_filter);
    seen.clear();
    m_seen_merged = false; // G6b: a fresh `seen` for the next merge
    for (u32 o_it = 0; o_it < r_spatial.size(); o_it++)
    {
        ISpatial* spatial = r_spatial[o_it];
        IGameObject* object = spatial->dcast_GameObject();
        if (object && feel_vision_isRelevant(object))
            seen.push_back(object);
    }
    if (seen.size() > 1)
    {
        std::sort(seen.begin(), seen.end());
        xr_vector<IGameObject*>::iterator end = std::unique(seen.begin(), seen.end());
        if (end != seen.end())
            seen.erase(end, seen.end());
    }
    PERF_ADD(vision_query_relevant, seen.size());
}

// 24-critical-path.md G6b: the merge of the new frustum result (`seen`) into the tracked list (`query` / feel_visible).
// Same visible set and the same items in feel_visible in the same order as the vanilla merge (the kept ones in their
// order, then the new ones in ascending pointer order), `query` = the new `seen` without `parent`. Only the mesh point
// of a new item is chosen on its first use (feel_vision_init_mesh_point) instead of on adding it, so the random point
// (and the Random stream) differs from vanilla. Measured s2_merge 1.25 -> 0.09 ms/frame. Against vanilla:
//  - `parent`: a binary search in the sorted `seen` instead of std::remove over all of it;
//  - one merge pass over seen / query gives both differences (two set_difference before);
//  - removal: one stable remove_if over feel_visible with a binary search in the sorted `diff`; vanilla o_delete walked
//    feel_visible (~180-byte items) for each removed object and erased it, shifting the tail each time (O(n*k)).
//    Done before adding, which only appends, so the order equals vanilla o_new + o_delete. m_trace_cursor is an index
//    and is kept as is, exactly as with the vanilla erase calls (o_trace / eye_pp_s2_prepare wrap it);
//  - adding: no get_new_local_point_on_mesh / get_last_local_point_on_mesh (see feel_vision_init_mesh_point);
//  - query.swap(seen) instead of copying ~2000 pointers; m_seen_merged marks `seen` as consumed, so a merge without a
//    feel_vision_query in between is a no-op (vanilla found seen == query then).
// The caller holds m_vision_mtx (feel_vision_update*, CCustomMonster::eye_pp_s2_prepare).
void Vision::feel_vision_merge_after_frustum(IGameObject* parent)
{
    PERF_SCOPE(stk_vision_s2_merge); // 24-critical-path.md M10
    if (m_seen_merged)
        return;

    // feel_vision_query leaves `seen` sorted and unique, relcase only erases; `query` is the previous `seen`.
    const auto self = std::lower_bound(seen.begin(), seen.end(), parent);
    if (self != seen.end() && *self == parent)
        seen.erase(self);

    diff.clear(); // A-B = objects, that are invisible
    m_diff_new.clear(); // B-A = objects, that become visible
    {
        auto s = seen.cbegin();
        const auto s_end = seen.cend();
        auto q = query.cbegin();
        const auto q_end = query.cend();
        while (s != s_end && q != q_end)
        {
            if (*s < *q)
                m_diff_new.push_back(*s++);
            else if (*q < *s)
                diff.push_back(*q++);
            else
            {
                ++s;
                ++q;
            }
        }
        m_diff_new.insert(m_diff_new.end(), s, s_end);
        diff.insert(diff.end(), q, q_end);
    }

    if (!diff.empty())
    {
        PERF_SCOPE(stk_vision_s2_o_delete);
        u32 never_traced = 0;
        const auto gone = [this, &never_traced](const feel_visible_Item& item) {
            if (!std::binary_search(diff.cbegin(), diff.cend(), item.O))
                return false;
            if (item.mesh_point_pending)
                ++never_traced;
            return true;
        };
        feel_visible.erase(std::remove_if(feel_visible.begin(), feel_visible.end(), gone), feel_visible.end());
        PERF_ADD(vision_o_delete, diff.size());
        PERF_ADD(vision_mesh_point_skipped, never_traced);
    }

    if (!m_diff_new.empty())
    {
        PERF_SCOPE(stk_vision_s2_o_new);
        feel_visible.reserve(feel_visible.size() + m_diff_new.size());
        for (IGameObject* O : m_diff_new)
        {
            // A new item without the mesh point (cp_LP / bone_id / cp_LAST stay zero until feel_vision_init_mesh_point);
            // Cache.verts are zeroed by ray_cache(), as vanilla o_new did.
            feel_visible.push_back(feel_visible_Item());
            feel_visible_Item& I = feel_visible.back();
            I.O = O;
            I.Cache_vis = 1.f;
            I.fuzzy = -EPS_S;
            I.mesh_point_pending = true;
        }
        PERF_ADD(vision_o_new, m_diff_new.size());
        m_diff_new.clear();
    }

    query.swap(seen);
    m_seen_merged = true;
}

void Vision::feel_vision_update(IGameObject* parent, Fvector& P, float dt, float vis_threshold)
{
    std::lock_guard<std::recursive_mutex> lock(m_vision_mtx);
    feel_vision_merge_after_frustum(parent);
    PERF_SCOPE(stk_vision_s2_rays); // 24-critical-path.md M10
    o_trace(P, dt, vis_threshold, static_cast<u32>(npc_perf_vision_rays_per_npc));
}

void Vision::feel_vision_update_staged(
    IGameObject* parent, Fvector& P, float dt, float vis_threshold, u32 trace_pass_cap)
{
    std::lock_guard<std::recursive_mutex> lock(m_vision_mtx);
    feel_vision_merge_after_frustum(parent);
    PERF_SCOPE(stk_vision_s2_rays); // 24-critical-path.md M10
    o_trace(P, dt, vis_threshold, trace_pass_cap);
}

void Vision::o_trace(Fvector& P, float dt, float vis_threshold, u32 trace_pass_cap)
{
    RQR.r_clear();

    const u32 total = static_cast<u32>(feel_visible.size());
    if (!total)
        return;

    const u32 budget = _min(total, trace_pass_cap);
    if (budget == 0)
        return;

    u32 start_index = 0;
    if (total > budget)
    {
        if (m_trace_cursor >= total)
            m_trace_cursor = 0;

        start_index = m_trace_cursor;
        m_trace_cursor = (m_trace_cursor + budget) % total;
    }

    for (u32 trace_idx = 0; trace_idx < budget; ++trace_idx)
    {
        feel_visible_Item& item = feel_visible[(start_index + trace_idx) % total];
        feel_visible_Item* I = &item;

        if (0 == I->O->GetCForm())
        {
            I->fuzzy = -1;
            continue;
        }

        // verify relation
        // if (positive(I->fuzzy) && I->O->Position().similar(I->cp_LR_dst,lr_granularity) &&
        // P.similar(I->cp_LR_src,lr_granularity))
        // continue;

        I->cp_LR_dst = I->O->Position();
        I->cp_LR_src = P;
        if (I->mesh_point_pending) // G6b: added by the merge, the first ray at it
            feel_vision_init_mesh_point(*I, false);
        I->cp_LAST = I->O->get_last_local_point_on_mesh(I->cp_LP, I->bone_id);

        //
        Fvector D, OP = I->cp_LAST;
        D.sub(OP, P);
        if (fis_zero(D.magnitude()))
        {
            I->fuzzy = 1.f;
            continue;
        }

        float f = D.magnitude() + .2f;
        if (f > fuzzy_guaranteed)
        {
            D.div(f);
            // setup ray defs & feel params
            collide::ray_defs RD(P, D, f, CDB::OPT_CULL,
                collide::rq_target(collide::rqtStatic | collide::rqtObject | collide::rqtObstacle));
            SFeelParam feel_params(this, &*I, vis_threshold);
            // check cache
            if (feel_vision_ray_cache_reuse(I->Cache, P, D, f, npc_perf_vision_cache_pos_slack_m))
            {
                // similar with previous query
                feel_params.vis = I->Cache_vis;
                // Log("cache 0");
            }
            else
            {
                float _u, _v, _range;
                // Only use cached static triangle after a successful prior query filled verts; never
                // TestRayTri on the zeros of a new item / degenerate tris (undefined + can upset collide state under load).
                bool cached_static_tri = false;
                if (I->Cache.result)
                {
                    const Fvector& va = I->Cache.verts[0];
                    const Fvector& vb = I->Cache.verts[1];
                    const Fvector& vc = I->Cache.verts[2];
                    constexpr float min_edge2 = 1e-10f;
                    cached_static_tri =
                        va.distance_to_sqr(vb) > min_edge2 && vb.distance_to_sqr(vc) > min_edge2 && vc.distance_to_sqr(va) > min_edge2;
                }
                if (cached_static_tri && CDB::TestRayTri(P, D, I->Cache.verts, _u, _v, _range, false) &&
                    (_range > 0 && _range < f))
                {
                    feel_params.vis = 0.f;
                    // Log("cache 1");
                }
                else
                {
                    // cache outdated. real query.
                    VERIFY(!fis_zero(RD.dir.magnitude()));

                    if (g_pGameLevel->ObjectSpace.RayQuery(RQR, RD, feel_vision_callback, &feel_params, NULL, NULL))
                    {
                        I->Cache_vis = feel_params.vis;
                        I->Cache.set(P, D, f, true);
                    }
                    else
                    {
                        // No static hits: fully visible. Cache the miss so unobstructed rays are reused.
                        I->Cache_vis = feel_params.vis;
                        I->Cache.set(P, D, f, false);
                    }
                    // Log("query");
                }
            }
            // No separate pass over dynamic occluders (vehicles, props) after this query: a deliberate perf
            // trade-off, NPCs see through movers a little more often.
            if (feel_params.vis < feel_params.vis_threshold)
            {
                // INVISIBLE, choose next point
                I->fuzzy -= fuzzy_update_novis * dt;
                clamp(I->fuzzy, -.5f, 1.f);
                I->cp_LP = I->O->get_new_local_point_on_mesh(I->bone_id);
            }
            else
            {
                // VISIBLE
                I->fuzzy += fuzzy_update_vis * dt;
                clamp(I->fuzzy, -.5f, 1.f);
            }
        }
        else
        {
            // VISIBLE, 'cause near
            I->fuzzy += fuzzy_update_vis * dt;
            clamp(I->fuzzy, -.5f, 1.f);
        }
    }
}
};
