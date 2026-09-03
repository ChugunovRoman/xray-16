#include "stdafx.h"
#include "Layers/xrRender/du_box.h"

namespace xray::render::RENDER_NAMESPACE
{
// Distance from the camera to the closest point of the light volume (0 = camera inside).
// For OMNIPART uses parent position+range: all 6 parts of one point light share the
// same value, so they pass/fail the distance cull together.
static float light_volume_dist(const light* L)
{
    if (L->flags.type == IRender_Light::OMNIPART)
        return _max(Device.vCameraPosition.distance_to(L->position) - L->range, 0.f);
    return _max(Device.vCameraPosition.distance_to(L->spatial.sphere.P) - L->spatial.sphere.R, 0.f);
}

// Budget cap: keep the closest lights, WHOLE sources at a time. A shadowed POINT light is
// 6 OMNIPARTs = 6 slots; cutting parts independently produces a half-shadowed sphere, so
// candidates are grouped by omnipart_owner and taken or dropped as a unit. SPOT weighs 1
// slot. Ranking key: volume-edge distance (LOD saturates at 1.0 for most real lights), with
// a hysteresis bonus for lights rendered on the previous frame (anti-popping). A 6-slot
// group that does not fit is skipped WHOLE; smaller farther groups may fill the remainder
// (slots must not idle). Budget < 6 means point lights get no shadows at all — expected.
// Player-attached lights (the torch beam, attached glows): the light ORIGIN sits within a
// small radius of the camera — the viewer IS effectively at the light source. Such a light
// can never be occluded from the player, always lights the immediate surroundings, and must
// never compete for shadow slots: it skips the occq-visibility drop, the distance cull and
// is taken into the budget for FREE (the budget effectively expands by its slots, e.g.
// budget 12 becomes 13 while the torch is on). For OMNIPART the member position equals the
// parent position (light::Export), so a shadowed attached point light whitelists atomically.
static bool is_player_attached_light(const light* L)
{
    // The torch bone is ~0.3-1.5 m from the camera center; 2 m covers every attach style.
    return Device.vCameraPosition.distance_to(L->position) <= 2.f;
}

static void apply_light_shadow_budget(xr_vector<light*>& kept, size_t budget)
{
    struct budget_group
    {
        light* key;     // omnipart_owner for parts, the light itself otherwise
        float eff_dist; // min effective distance among members
        size_t count;   // members that survived the filters = weight in slots
        bool attached;  // player-attached (torch): taken for free, does not consume slots
        bool taken;
    };
    xr_vector<budget_group> groups;
    groups.reserve(kept.size());
    for (light* L : kept)
    {
        light* key = L->omnipart_owner ? L->omnipart_owner : L;
        const bool was_rendered = (Device.dwFrame - L->shadow_render_frame) <= 1;
        const float d = light_volume_dist(L) * (was_rendered ? ps_r2_light_budget_hysteresis : 1.f);
        const bool attached = is_player_attached_light(L);
        bool found = false;
        for (budget_group& G : groups)
        {
            if (G.key == key)
            {
                G.eff_dist = _min(G.eff_dist, d);
                G.attached |= attached;
                ++G.count;
                found = true;
                break;
            }
        }
        if (!found)
        {
            budget_group G;
            G.key = key;
            G.eff_dist = d;
            G.count = 1;
            G.attached = attached;
            G.taken = false;
            groups.push_back(G);
        }
    }
    std::sort(groups.begin(), groups.end(), [](const budget_group& g1, const budget_group& g2)
    {
        return g1.eff_dist < g2.eff_dist;
    });
    // Greedy take by ascending distance: a 6-slot group that does not fit is skipped WHOLE
    // (smaller farther groups may fill the remainder — slots must not idle). Player-attached
    // groups are taken unconditionally and do not consume slots (budget expands for them).
    size_t used = 0;
    for (budget_group& G : groups)
    {
        if (G.attached)
        {
            G.taken = true;
            continue;
        }
        if (used + G.count <= budget)
        {
            G.taken = true;
            used += G.count;
        }
    }
    xr_vector<light*> selected;
    selected.reserve(kept.size());
    for (light* L : kept)
    {
        light* key = L->omnipart_owner ? L->omnipart_owner : L;
        for (const budget_group& G : groups)
            if (G.key == key && G.taken)
            {
                selected.push_back(L);
                break;
            }
    }
    kept = std::move(selected);
}

// Вариант A (render_lights MT analysis): filter a light package by the NARROW scope frustum.
// The scope pass reuses the main pass's light set (wide-frustum superset); lights whose volume
// sphere (position + range - the sphere also bounds the shadow-casting extent) misses the lens
// view cannot light any visible pixel and are dropped before smap building and accumulation.
// The source package is left untouched (the main pass owns it).
//
// DARK-DYNAMICS ROOT CAUSE (bisected to commit 197d536a, confirmed by r__svp_dbg2 A/B): this
// filter dropped lights that DO light lens-visible pixels - most importantly the SUN (a 600m
// sphere test against a narrow zoom frustum is unreliable) - so the scope frame lost all global
// The scope pass renders the full main light package. A narrow-frustum filter was tried here
// (r__svp_light_filter) but it dropped the directional sun from the lens - dark day-time
// dynamics. Any future filter must whitelist directional lights and lights near frustum planes.
void CRender::filter_light_package_for_svp(const light_Package& src, light_Package& dst)
{
    dst.v_shadowed = src.v_shadowed;
    dst.v_point = src.v_point;
    dst.v_spot = src.v_spot;
}

void CRender::render_lights(light_Package& LP, bool svp_no_vis)
{
    ZoneScoped;

    // P2.3 optimization: DISABLED — the multi-page smap design (each page re-packs from rect 0,0
    // and requires a full atlas clear before rendering) is architecturally incompatible with
    // atlas persistence for the scope pass. The per-page clear + Variant A frustum filter already
    // provide significant savings. Re-enable only with a single-page guarantee (budget cap that
    // fits all lights on one page) or a multi-atlas refactor.
    const bool reuse_shadowmaps = false;
    (void)reuse_shadowmaps;

    // Frame driver stage 1c: the SVP worker already built this pass's shadow maps into the
    // dedicated atlas (sealed lists await in svp_smap_replay_lists). Replay + accumulate only -
    // svp_accumulate_prebuilt also advances the per-frame stage machine for the second package.
    if (svp_no_vis && svp_shadow_stage != 0)
    {
        svp_accumulate_prebuilt(LP);
        return;
    }

    //////////////////////////////////////////////////////////////////////////
    // Refactor order based on ability to pack shadow-maps
    // 1. calculate area + sort in descending order
    // const	u16		smap_unassigned		= u16(-1);
    if (!reuse_shadowmaps)
    {
        xr_vector<light*>& source = LP.v_shadowed;
        xr_vector<light*> kept;
        kept.reserve(source.size());

        const float shadow_dist = ps_r2_light_shadow_dist;

        for (u32 it = 0; it < source.size(); it++)
        {
            light* L = source[it];
            // Player-attached lights (torch beam) never drop out of the shadow path: the
            // occq volume test is meaningless when the camera sits at the light origin, and
            // the distance cull must not touch them either. They are also budget-free (see
            // apply_light_shadow_budget) — the flashlight must not flicker under budget
            // pressure (a dropped shadowed light disappears entirely, no unshadowed fallback).
            const bool player_attached = is_player_attached_light(L);
            if (!svp_no_vis)
            {
                L->vis_update();
                if (!L->vis.visible && !player_attached)
                    continue; // drop invisible
            }

            // Distance-based shadow skip: render shadow maps only for lights whose volume
            // edge is close enough to the camera. Skipped lights disappear entirely (no
            // unshadowed fallback — it lights through walls).
            // For OMNIPART use the parent position+range: all 6 parts of one shadowed
            // point light share it, so they are skipped/kept together automatically
            // (independent per-part decisions produce a half-lit sphere).
            // Hysteresis: a light rendered on the previous frame is kept until 20% past
            // the threshold — no popping when walking back and forth near the border.
            if (shadow_dist > 0.f && !player_attached)
            {
                const float dist = light_volume_dist(L);

                const bool was_rendered = (Device.dwFrame - L->shadow_render_frame) <= 1;
                const float limit = was_rendered ? shadow_dist * 1.2f : shadow_dist;
                if (dist > limit)
                    continue;
            }

            // Secondary LOD skip (usually off; distance culling above is the main tool).
            // OMNIPARTs are excluded: their per-part spheres differ, so the per-part LOD check
            // can drop individual faces of one point light and produce a half-shadowed sphere.
            // The distance cull already handles them at parent level.
            if (ps_r2_light_degrade_lod > 0.f && L->flags.type != IRender_Light::OMNIPART &&
                L->get_LOD() < ps_r2_light_degrade_lod)
                continue;

            kept.push_back(L);
            LR.compute_xf_spot(L);
        }

        // Budget cap (safety net for extreme scenes): whole-source selection, see
        // apply_light_shadow_budget.
        if (ps_r2_light_shadow_budget > 0 && kept.size() > (size_t)ps_r2_light_shadow_budget)
            apply_light_shadow_budget(kept, (size_t)ps_r2_light_shadow_budget);

        // Mark only lights that actually survived the budget: shadow_render_frame drives both
        // the distance hysteresis (x1.2) and the budget hysteresis, so it must not lie.
        for (light* L : kept)
            L->shadow_render_frame = Device.dwFrame;

        // Budget lights hold a scarce slot: re-test their occq visibility sooner than the
        // default 10-20 frame interval, so a light hidden behind a wall releases its slot
        // faster. _min only shortens the wait — never extends it.
        if (ps_r2_light_vis_refresh > 0)
            for (light* L : kept)
                L->vis.frame2test = _min(L->vis.frame2test, Device.dwFrame + (u32)ps_r2_light_vis_refresh);

        LP.v_shadowed = std::move(kept);
    }

    // 2. refactor - infact we could go from the backside and sort in ascending order
    if (!reuse_shadowmaps)
    {
        xr_vector<light*>& source = LP.v_shadowed;
        xr_vector<light*> refactored;
        refactored.reserve(source.size());

        // Sort once by smap size descending — packing order per page stays identical
        // (each page greedily takes the largest fitting light; only removals change between
        // pages, not the relative order). Replaces the old per-page re-sort + erase-from-
        // middle which was O(pages * n * log n).
        std::sort(source.begin(), source.end(), [](light* l1, light* l2)
        {
            return l1->X.S.size > l2->X.S.size;
        });

        for (u16 smap_ID = 0; !source.empty(); ++smap_ID)
        {
            LP_smap_pool.initialize(RImplementation.o.smapsize);
            size_t kept = 0;
            for (size_t test = 0; test < source.size(); ++test)
            {
                light* L = source[test];
                SMAP_Rect R{};
                if (LP_smap_pool.push(R, L->X.S.size))
                {
                    // OK
                    L->X.S.posX = R.min.x;
                    L->X.S.posY = R.min.y;
                    L->vis.smap_ID = smap_ID;
                    refactored.push_back(L);
                }
                else
                    source[kept++] = L; // compact instead of erase
            }
            source.resize(kept);
        }

        // save (lights are popped from back)
        std::reverse(refactored.begin(), refactored.end());
        LP.v_shadowed = std::move(refactored);
    }

    auto& cmd_list = get_imm_context().cmd_list;
    Target->rt_smap_depth->set_slice_read(0);

    PIX_EVENT(SHADOWED_LIGHTS);

    //////////////////////////////////////////////////////////////////////////
    // sort lights by importance???
    // while (has_any_lights_that_cast_shadows) {
    //		if (has_point_shadowed)		->	generate point shadowmap
    //		if (has_spot_shadowed)		->	generate spot shadowmap
    //		switch-to-accumulator
    //		if (has_point_unshadowed)	-> 	accum point unshadowed
    //		if (has_spot_unshadowed)	-> 	accum spot unshadowed
    //		if (was_point_shadowed)		->	accum point shadowed
    //		if (was_spot_shadowed)		->	accum spot shadowed
    //	}
    //	if (left_some_lights_that_doesn't cast shadows)
    //		accumulate them
    static xr_vector<light*> L_spot_s;

    struct task_data_t
    {
        light* L{};
        Task* task{};
        u32 batch_id{};
        bool rendered{}; // filled by the task in parallel mode
        void* cmdList{}; // ID3D11CommandList* after FinishCommandList (DX11 parallel mode only)
    };
    // Per-context slot array: tasks capture pointers into this, so it must not reallocate.
    static task_data_t per_batch[R__NUM_CONTEXTS];
    // Ordered list of batch_ids for flush (lights within a smap page).
    xr_vector<u32> lights_queue;
    lights_queue.reserve(R__NUM_CONTEXTS);

    const bool mt_light = ps_r2_mt_light_render != 0;

    const auto& flush_lights = [&]()
    {
        ZoneScopedN("flush lights");
        bool any_submit = false;
        for (const auto batch_id : lights_queue)
        {
            task_data_t& item = per_batch[batch_id];
            if (item.task)
                TaskScheduler->Wait(*item.task);

            auto& dsgraph = get_context(batch_id);
            light* L = item.L;

            if (mt_light)
            {
                // Parallel mode: recording + svis.end + FinishCommandList were done in the task.
                // Main thread only executes the sealed command list on IMM (sequential).
                if (item.rendered)
                {
                    Stats.s_merged++;
                    L_spot_s.push_back(L);
                }
                else
                    Stats.s_finalclip++;

#if defined(USE_DX11)
                if (item.cmdList)
                {
                    HW.get_context(CHW::IMM_CTX_ID)->ExecuteCommandList(
                        static_cast<ID3D11CommandList*>(item.cmdList), false);
                    // P2.3: keep the sealed list alive - the scope pass (reuse mode) re-executes
                    // it to restore the shadow map content (camera-independent, same frame).
                    // Ownership transferred to svp_smap_replay_lists; released after use.
                    RImplementation.svp_smap_replay_lists.push_back(item.cmdList);
                    item.cmdList = nullptr;
                    any_submit = true;
                }
#endif
            }
            else
            {
                // Serial mode: recording happens here on the main thread (original behavior)
                if (render_light_smap(dsgraph, L, {}))
                {
                    Stats.s_merged++;
                    L_spot_s.push_back(L);
                }
                else
                    Stats.s_finalclip++;

                L->svis[batch_id].end();
            }

            RImplementation.release_context(batch_id);
        }

        lights_queue.clear();

        // ExecuteCommandList breaks the IMM context CPU-side state cache — reset it
        // (mirrors render_sun::flush)
        if (any_submit)
            get_imm_context().cmd_list.Invalidate();
    };

    // P2.3 optimization (reuse): the shadow maps were rendered FROM THE LIGHT's position by the
    // main pass THIS frame - their content is camera-independent, and the scope pass shares the
    // same eye position. Skip smap clear/build/tasks/flush entirely; instead re-execute the main
    // pass's sealed command lists (restores ALL pages' shadow depth) and run only the shadowed
    // accumulation. Unshadowed point/spot accumulation runs after (shared tail below).
#if defined(USE_DX11)
    if (reuse_shadowmaps)
    {
        // Restore ALL pages' shadow depth by re-executing the main pass's sealed lists.
        Target->phase_smap_spot_clear(cmd_list);
        auto replay_ctx = HW.get_context(CHW::IMM_CTX_ID);
        for (void* list : RImplementation.svp_smap_replay_lists)
            replay_ctx->ExecuteCommandList(static_cast<ID3D11CommandList*>(list), false);
        get_imm_context().cmd_list.Invalidate();

        L_spot_s.clear();
        for (light* p_light : LP.v_shadowed)
        {
            // Only lights whose shadow map the MAIN pass actually rendered this frame. Lights it
            // culled (distance/occq/budget) carry stale X.S page placements from arbitrary earlier
            // frames - sampling them maps the volume onto OTHER lights' atlas pages: hard-edged
            // rectangular shadow artifacts that flicker as the layout shifts.
            if ((Device.dwFrame - p_light->shadow_render_frame) <= 1)
                L_spot_s.push_back(p_light);
        }

        Target->phase_accumulator(cmd_list);

        PIX_EVENT(ACCUM_SPOT);
        for (light* p_light : L_spot_s)
        {
            Target->accum_spot(cmd_list, p_light);
            render_indirect(p_light);
        }

        PIX_EVENT(ACCUM_VOLUMETRIC);
        if (RImplementation.o.advancedpp && ps_r2_ls_flags.is(R2FLAG_VOLUMETRIC_LIGHTS))
            for (light* p_light : L_spot_s)
                Target->accum_volumetric(cmd_list, p_light);

        L_spot_s.clear();

        // Release the replayed lists (GPU work enqueued, CPU-side objects no longer needed).
        for (void* list : RImplementation.svp_smap_replay_lists)
            static_cast<ID3D11CommandList*>(list)->Release();
        RImplementation.svp_smap_replay_lists.clear();
        return;
    }
#else
    if (reuse_shadowmaps)
    {
        // GL: no sealed command lists to replay; accumulate against the main pass's last-page
        // atlas (partial but correct for lights on the last page).
        L_spot_s.assign(LP.v_shadowed.begin(), LP.v_shadowed.end());
        Target->phase_accumulator(cmd_list);
        for (light* p_light : L_spot_s)
        {
            Target->accum_spot(cmd_list, p_light);
            render_indirect(p_light);
        }
        L_spot_s.clear();
        return;
    }
#endif

    // Single shared spatial query for all light passes: one q_sphere around the camera
    // replaces a per-light q_frustum octree walk. Radius covers every light volume that
    // survived the distance cull (shadow_dist + max light range with margin).
    static xr_vector<ISpatial*> common_dynamic;
    if (!reuse_shadowmaps && ps_r2_light_common_dynamic > 0 && !LP.v_shadowed.empty())
    {
        ZoneScopedN("light_common_dynamic_query");
        float max_reach = ps_r2_light_shadow_dist;
        for (light* Ld : LP.v_shadowed)
            max_reach = _max(max_reach, Device.vCameraPosition.distance_to(Ld->spatial.sphere.P) + Ld->range);
        common_dynamic.clear();
        g_pGamePersistent->SpatialSpace.q_sphere(common_dynamic, 0, STYPE_RENDERABLE, Device.vCameraPosition, max_reach * 1.05f);
    }

    // P2.3: skipped entirely in the reuse mode (see above).
    if (!reuse_shadowmaps)
    while (!LP.v_shadowed.empty())
    {
        // if (has_spot_shadowed)
        Stats.s_used++;

        // generate spot shadowmap
        Target->phase_smap_spot_clear(cmd_list);
        xr_vector<light*>& source = LP.v_shadowed;
        light* L = source.back();
        const u16 sid = L->vis.smap_ID;
        while (true)
        {
            if (source.empty())
                break;
            L = source.back();
            if (L->vis.smap_ID != sid)
                break;

            const auto batch_id = alloc_context(mt_light); // true => deferred cmd_list for parallel recording
            if (batch_id == R_dsgraph_structure::INVALID_CONTEXT_ID)
            {
                VERIFY(!lights_queue.empty());
                flush_lights();
                continue;
            }

            source.pop_back();
            Lights_LastFrame.push_back(L);

            task_data_t& item = per_batch[batch_id];
            item = {}; // reset
            item.batch_id = batch_id;
            item.L = L;

            // Capture mt_light by value (cvar may change between task scheduling and execution)
            // and a pointer to the per_batch slot (static array, valid for the frame).
            const auto& calc_lights = [this, item_ptr = &item, mt_light]
            {
                ZoneScopedN("calc lights");
                auto& dsgraph = RImplementation.get_context(item_ptr->batch_id);
                {
                    auto* L = item_ptr->L;

                    L->svis[item_ptr->batch_id].begin();

                    dsgraph.o.phase = PHASE_SMAP;
                    dsgraph.r_pmask(true, RImplementation.o.Tshadows);
                    dsgraph.o.sector_id = L->spatial.sector_id;
                    dsgraph.o.view_pos = L->position;
                    dsgraph.o.xform = L->X.S.combine;
                    dsgraph.o.view_frustum.CreateFromMatrix(L->X.S.combine, FRUSTUM_P_ALL & (~FRUSTUM_P_NEAR));
                    dsgraph.o.use_shadow_hull_cull = ps_r2_smap_hull_cull != 0;
                    dsgraph.o.shadow_light_pos = L->position;
                    dsgraph.o.shadow_light_range = L->range;
                    dsgraph.o.precomputed_dynamic = (ps_r2_light_common_dynamic > 0) ? &common_dynamic : nullptr;
                    dsgraph.o.shadow_owner = L->shadow_owner;

                    dsgraph.build_subspace();

                    // Parallel mode: record the shadow map draw calls into the deferred context
                    // right here in the task. Stats/L_spot_s are NOT touched (race); the main
                    // thread handles them in flush_lights after Wait.
                    if (mt_light)
                    {
                        if (render_light_smap(dsgraph, L, {}))
                            item_ptr->rendered = true;

                        // Finalize the deferred command list in the task: svis.end() records
                        // the caster occq test, FinishCommandList seals the list. The main
                        // thread only needs to ExecuteCommandList (sequential on IMM).
                        L->svis[item_ptr->batch_id].end();
#if defined(USE_DX11)
                        ID3D11CommandList* pCmdList = nullptr;
                        CHK_DX(HW.get_context(dsgraph.context_id)->FinishCommandList(false, &pCmdList));
                        item_ptr->cmdList = pCmdList;
#endif
                    }
                }
            };

            // calculate
            if (o.mt_calculate)
            {
                item.task = &TaskScheduler->AddTask(calc_lights);
            }
            else
            {
                calc_lights();
            }
            lights_queue.push_back(batch_id);
        }
        flush_lights(); // in case if something left

        cmd_list.Invalidate();

#if defined(USE_DX11)
        // Shadow-transfer (roadmap A.2): preserve this page's atlas content in the dedicated SVP
        // atlas BEFORE the next iteration's clear destroys it. Issued on IMM right after the
        // page's sealed lists executed, so the GPU sees the page's final content. The scope pass
        // later accumulates against these copies using the SAME X.S placements the main pass
        // packed (valid for the whole frame) - the worker stage C has nothing to build.
        // Slice index = number of already-transferred pages: pages of BOTH light packages
        // (LP_normal + LP_pending) append continuously across the two render_lights calls.
        if (svp_shadow_transfer && svp_frame_driver && svp_shadow_stage == 3 && !L_spot_s.empty() &&
            Target->svp_rt_smap_depth && Target->svp_rt_smap_depth->valid())
        {
            const UINT dst_slice = UINT(svp_shadow_page_lights.size());
            // Cap at the spot-page BUDGET, not the atlas slice count: with sun-reuse
            // (r__svp_sun_mode 1) the atlas tail (slices >= ps_r__svp_smap_pages) holds the
            // copied sun cascades and must never be overwritten by spot pages.
            if (dst_slice < UINT(ps_r__svp_smap_pages))
            {
                ID3D11Texture2D* dst_tex = static_cast<ID3D11Texture2D*>(Target->svp_rt_smap_depth->pSurface);
                ID3D11Texture2D* src_tex = static_cast<ID3D11Texture2D*>(Target->rt_smap_depth->pSurface);
                const UINT dst_subres = D3D11CalcSubresource(0, dst_slice, 1);
                HW.get_context(CHW::IMM_CTX_ID)->CopySubresourceRegion(
                    dst_tex, dst_subres, 0, 0, 0, src_tex, 0, nullptr);
                svp_shadow_page_lights.push_back(L_spot_s);
            }
            else
            {
                // Atlas slice budget exhausted: degrade to unshadowed instead of sampling a
                // stale slice (mirrors the worker builder's page-cap fallback).
                svp_shadow_unshadowed.insert(svp_shadow_unshadowed.end(), L_spot_s.begin(), L_spot_s.end());
            }
        }
#endif

        PIX_EVENT(UNSHADOWED_LIGHTS);

        //		switch-to-accumulator
        Target->phase_accumulator(cmd_list);

        PIX_EVENT(POINT_LIGHTS);

        //		if (has_point_unshadowed)	-> 	accum point unshadowed
        if (!LP.v_point.empty())
        {
            light* L2 = LP.v_point.back();
            LP.v_point.pop_back();
            bool visible = true;
            if (!svp_no_vis)
            {
                L2->vis_update();
                visible = L2->vis.visible;
            }
            if (visible)
            {
                Target->accum_point(cmd_list, L2);
                render_indirect(L2);
            }
        }

        PIX_EVENT(SPOT_LIGHTS);

        //		if (has_spot_unshadowed)	-> 	accum spot unshadowed
        if (!LP.v_spot.empty())
        {
            light* L2 = LP.v_spot.back();
            LP.v_spot.pop_back();
            bool visible = true;
            if (!svp_no_vis)
            {
                L2->vis_update();
                visible = L2->vis.visible;
            }
            if (visible)
            {
                LR.compute_xf_spot(L2);
                Target->accum_spot(cmd_list, L2);
                render_indirect(L2);
            }
        }

        PIX_EVENT(SPOT_LIGHTS_ACCUM_VOLUMETRIC);

        //		if (was_spot_shadowed)		->	accum spot shadowed
        if (!L_spot_s.empty())
        {
            PIX_EVENT(ACCUM_SPOT);
            for (light* p_light : L_spot_s)
            {
                Target->accum_spot(cmd_list, p_light);
                render_indirect(p_light);
            }

            PIX_EVENT(ACCUM_VOLUMETRIC);
            if (RImplementation.o.advancedpp && ps_r2_ls_flags.is(R2FLAG_VOLUMETRIC_LIGHTS))
                for (light* p_light : L_spot_s)
                    Target->accum_volumetric(cmd_list, p_light);

            L_spot_s.clear();
        }
    }

    PIX_EVENT(POINT_LIGHTS_ACCUM);
    // Point lighting (unshadowed, if left)
    if (!LP.v_point.empty())
    {
        xr_vector<light*>& Lvec = LP.v_point;
        for (light* p_light : Lvec)
        {
            if (!svp_no_vis)
            {
                p_light->vis_update();
                if (!p_light->vis.visible)
                    continue;
            }
            render_indirect(p_light);
            Target->accum_point(cmd_list, p_light);
        }
        Lvec.clear();
    }

    PIX_EVENT(SPOT_LIGHTS_ACCUM);
    // Spot lighting (unshadowed, if left)
    if (!LP.v_spot.empty())
    {
        xr_vector<light*>& Lvec = LP.v_spot;
        for (light* p_light : Lvec)
        {
            if (!svp_no_vis)
            {
                p_light->vis_update();
                if (!p_light->vis.visible)
                    continue;
            }
            LR.compute_xf_spot(p_light);
            render_indirect(p_light);
            Target->accum_spot(cmd_list, p_light);
        }
        Lvec.clear();
    }
}

void CRender::render_indirect(light* L) const
{
    if (!ps_r2_ls_flags.test(R2FLAG_GI))
        return;

    auto& cmd_list = RImplementation.get_imm_context().cmd_list;

    light LIGEN;
    LIGEN.set_type(IRender_Light::REFLECTED);
    LIGEN.set_shadow(false);
    LIGEN.set_cone(PI_DIV_2 * 2.f);

    const xr_vector<light_indirect>& Lvec = L->indirect;
    if (Lvec.empty())
        return;
    const float LE = L->color.intensity();
    for (auto& LI : Lvec)
    {
        // energy and color
        const float LIE = LE * LI.E;
        if (LIE < ps_r2_GI_clip)
            continue;

        Fvector T{ L->color.r, L->color.g, L->color.b };
        T.mul(LI.E);
        LIGEN.set_color(T.x, T.y, T.z);

        // geometric
        Fvector L_up{ 0, 1, 0 }, L_right;
        if (_abs(L_up.dotproduct(LI.D)) > .99f)
            L_up = { 0, 0, 1 };

        L_right.crossproduct(L_up, LI.D).normalize();
        LIGEN.spatial.sector_id = LI.S;
        LIGEN.set_position(LI.P);
        LIGEN.set_rotation(LI.D, L_right);

        // range
        // dist^2 / range^2 = A - has infinity number of solutions
        // approximate energy by linear fallof Emax / (1 + x) = Emin
        const float Emax = LIE;
        const float Emin = 1.f / 255.f;
        const float x = (Emax - Emin) / Emin;
        if (x < 0.1f)
            continue;
        LIGEN.set_range(x);

        Target->accum_reflected(cmd_list, &LIGEN);
    }
}

// ---------------------------------------------------------------------------
// Frame driver stage 1c helpers
// ---------------------------------------------------------------------------

// Records one light's shadow map into dsgraph.cmd_list (immediate or deferred context).
// Shared stats/L_spot_s updates are NOT done here - the caller decides where they are safe.
// smap_target = {} keeps the historical rt_smap_depth member; the SVP shadow builder passes
// the dedicated atlas instead.
bool CRender::render_light_smap(R_dsgraph_structure& dsgraph, light* L, const ref_rt& smap_target)
{
    const bool bNormal = !dsgraph.mapNormalPasses[0][0].empty() || !dsgraph.mapMatrixPasses[0][0].empty();
    const bool bSpecial = !dsgraph.mapNormalPasses[1][0].empty() || !dsgraph.mapMatrixPasses[1][0].empty() ||
        !dsgraph.mapSorted.empty();
    if (!bNormal && !bSpecial)
        return false;

    PIX_EVENT_CTX(dsgraph.cmd_list, SHADOWED_LIGHT);
    Target->phase_smap_spot(dsgraph.cmd_list, L, smap_target);
    dsgraph.cmd_list.set_xform_world(Fidentity);
    dsgraph.cmd_list.set_xform_view(L->X.S.view);
    dsgraph.cmd_list.set_xform_project(L->X.S.project);
    dsgraph.render_graph(0);
    if (ps_r2_ls_flags.test(R2FLAG_SUN_DETAILS))
        Details->Render(dsgraph.cmd_list);

    // Render NPC blob shadows (cheap impostors collected during build_subspace)
    if (!dsgraph.npc_blobs.empty() && ps_r2_smap_npc_blob > 0)
    {
        PIX_EVENT_CTX(dsgraph.cmd_list, NPC_BLOB_SHADOWS);
        // Reuse the spot light's depth-only shader element (SE_R2_SHADOW) - in the SMAP
        // pass color-write is off, so any depth-writing shader works for the blob caster.
        dsgraph.cmd_list.set_Element(Target->get_accum_spot_shader()->E[SE_R2_SHADOW]);
        dsgraph.cmd_list.set_Geometry(Target->g_npc_blob);
        for (const Fmatrix& mBlob : dsgraph.npc_blobs)
        {
            dsgraph.cmd_list.set_xform_world(mBlob);
            dsgraph.cmd_list.Render(D3DPT_TRIANGLELIST, 0, 0, DU_BOX_NUMVERTEX, 0, DU_BOX_NUMFACES);
        }
        // Restore world matrix for subsequent passes
        dsgraph.cmd_list.set_xform_world(Fidentity);
    }

    L->X.S.transluent = FALSE;
    if (bSpecial)
    {
        L->X.S.transluent = TRUE;
        Target->phase_smap_spot_tsh(dsgraph.cmd_list, L);
        PIX_EVENT_CTX(dsgraph.cmd_list, SHADOWED_LIGHTS_RENDER_GRAPH);
        dsgraph.render_graph(1); // normal level, secondary priority
        PIX_EVENT_CTX(dsgraph.cmd_list, SHADOWED_LIGHTS_RENDER_SORTED);
        dsgraph.render_sorted(); // strict-sorted geoms
    }
    return true;
}

// Frame driver stage C (worker thread): builds the SCOPE pass's shadow maps into the dedicated
// atlas. Runs after the main pass signaled svp_lights_go, so the context pool and the shared
// light objects belong to this thread. Produces one sealed command list per packed page
// ([atlas clear + page lights]); the scope pass replays them in svp_accumulate_prebuilt.
void CRender::record_second_vp_shadows()
{
    ZoneScopedN("SVP record shadows");
#if defined(USE_DX11)
    if (!Target || !Target->svp_rt_smap_depth || !Target->svp_rt_smap_depth->valid())
        return;

    svp_shadow_page_lights.clear();
    svp_shadow_unshadowed.clear();

    const ref_rt& smap_target = Target->svp_rt_smap_depth;

    // Union of both pre-filtered packages' shadowed queues (disjoint by construction; the
    // members were filled by the main pass right before svp_lights_go was signaled).
    xr_vector<light*> source;
    source.reserve(LP_svp_normal.v_shadowed.size() + LP_svp_pending.v_shadowed.size());
    for (light* L : LP_svp_normal.v_shadowed)
        if (L)
            source.push_back(L);
    for (light* L : LP_svp_pending.v_shadowed)
        if (L)
            source.push_back(L);

    // Same culls as the main pass block-1 minus occlusion visibility (stage 1b dropped it on
    // this path): distance hysteresis + LOD + budget. Player-attached lights (torch) skip the
    // distance cull, same as the main pass — the lens view always sees the torch beam.
    const float shadow_dist = ps_r2_light_shadow_dist;
    xr_vector<light*> kept;
    kept.reserve(source.size());
    for (light* L : source)
    {
        if (shadow_dist > 0.f && !is_player_attached_light(L))
        {
            const float dist = light_volume_dist(L);
            const bool was_rendered = (Device.dwFrame - L->shadow_render_frame) <= 1;
            const float limit = was_rendered ? shadow_dist * 1.2f : shadow_dist;
            if (dist > limit)
                continue;
        }
        // OMNIPARTs are excluded from the LOD filter: same reasoning as the main pass —
        // independent per-part decisions produce a half-shadowed sphere.
        if (ps_r2_light_degrade_lod > 0.f && L->flags.type != IRender_Light::OMNIPART &&
            L->get_LOD() < ps_r2_light_degrade_lod)
            continue;
        LR.compute_xf_spot(L);
        kept.push_back(L);
    }

    if (kept.empty())
    {
        return;
    }

    // Same whole-source budget as the main pass (see apply_light_shadow_budget).
    if (ps_r2_light_shadow_budget > 0 && kept.size() > (size_t)ps_r2_light_shadow_budget)
        apply_light_shadow_budget(kept, (size_t)ps_r2_light_shadow_budget);

    // Shared spatial query (same optimization as the main pass r2_light_common_dynamic): ONE
    // q_sphere around the camera replaces a per-light octree walk in every shadow build. Without
    // it the worker paid a full spatial traversal per light (~2.5ms for 30 lights). Spatial DB
    // queries are lock-protected; the main pass is done with its own queries by the signal.
    xr_vector<ISpatial*> common_dynamic;
    const bool use_common = ps_r2_light_common_dynamic > 0;
    if (use_common)
    {
        float max_reach = ps_r2_light_shadow_dist;
        for (light* Ld : kept)
            max_reach = _max(max_reach, Device.vCameraPosition.distance_to(Ld->spatial.sphere.P) + Ld->range);
        g_pGamePersistent->SpatialSpace.q_sphere(common_dynamic, 0, STYPE_RENDERABLE, Device.vCameraPosition,
            max_reach * 1.05f);
    }

    // Pack pages with the DEDICATED allocator (main-pass placements stay untouched), then
    // record each page serially on this thread.
    std::sort(kept.begin(), kept.end(), [](light* l1, light* l2) { return l1->X.S.size > l2->X.S.size; });

    const u16 max_pages = u16(ps_r__svp_smap_pages);
    for (u16 smap_ID = 0; !kept.empty() && smap_ID < max_pages; ++smap_ID)
    {
        svp_LP_smap_pool.initialize(Target->svp_smap_page_size);
        xr_vector<light*> page;
        size_t remain = 0;
        for (size_t t = 0; t < kept.size(); ++t)
        {
            light* L = kept[t];
            SMAP_Rect R{};
            if (svp_LP_smap_pool.push(R, L->X.S.size))
            {
                L->X.S.posX = R.min.x;
                L->X.S.posY = R.min.y;
                L->vis.smap_ID = smap_ID;
                page.push_back(L);
            }
            else
                kept[remain++] = L; // compact instead of erase
        }
        kept.resize(remain);
        if (page.empty())
            break;

        const u32 batch_id = alloc_context(/*alloc_cmd_list=*/true);
        if (batch_id == R_dsgraph_structure::INVALID_CONTEXT_ID)
            break; // pool exhausted: unbuilt lights degrade to the unshadowed fallback

        auto& dsgraph = get_context(batch_id);
        Target->phase_smap_spot_clear(dsgraph.cmd_list, smap_target);
        for (light* L : page)
        {
            // svis is intentionally skipped: it would interleave caster occq queries into the
            // two passes' shared per-light query slots for a caster cache the scope pass never
            // consumes - pure slot churn.
            dsgraph.o.phase = PHASE_SMAP;
            dsgraph.r_pmask(true, RImplementation.o.Tshadows);
            dsgraph.o.sector_id = L->spatial.sector_id;
            dsgraph.o.view_pos = L->position;
            dsgraph.o.xform = L->X.S.combine;
            dsgraph.o.view_frustum.CreateFromMatrix(L->X.S.combine, FRUSTUM_P_ALL & (~FRUSTUM_P_NEAR));
            dsgraph.o.use_shadow_hull_cull = ps_r2_smap_hull_cull != 0;
            dsgraph.o.shadow_light_pos = L->position;
            dsgraph.o.shadow_light_range = L->range;
            dsgraph.o.precomputed_dynamic = use_common ? &common_dynamic : nullptr; // shared query, mirrors the main pass
            dsgraph.o.shadow_owner = L->shadow_owner;
            // NOTE: no second_vp_pass override here - field parity with the proven calc_lights
            // task path (is_main_pass is false for smap builds, so the shared-state branches
            // in build_subspace are off regardless).

            dsgraph.build_subspace();
            render_light_smap(dsgraph, L, smap_target);
        }

        ID3D11CommandList* sealed = nullptr;
        CHK_DX(HW.get_context(batch_id)->FinishCommandList(false, &sealed));
        release_context(batch_id);
        if (sealed)
        {
            svp_smap_replay_lists.push_back(sealed);
            // Freshness stamp (hysteresis consistency with the main pass block-1 stamp) and the
            // per-page light group: page i pairs with svp_smap_replay_lists[i] and must be
            // accumulated before page i+1's list clears the atlas.
            for (light* L : page)
                L->shadow_render_frame = Device.dwFrame;
            svp_shadow_page_lights.push_back(std::move(page));
        }
    }

    // Lights that passed the culls but not the page cap: they will be accumulated UNSHADOWED
    // (light without shadow lookup) instead of disappearing from the scope view entirely.
    if (!kept.empty())
    {
        svp_shadow_unshadowed.insert(svp_shadow_unshadowed.end(), kept.begin(), kept.end());
        Msg("! SVP shadow builder: %u shadowed light(s) left unpacked after %u page(s), they will light the scope view without shadows",
            u32(kept.size()), u32(max_pages));
    }

#endif
}

// Frame driver stage-1 consumer (scope pass, main thread): replays the worker's sealed page
// lists ONE BY ONE, accumulating each page's lights before the next page's list clears the
// atlas, then drains the unshadowed fallback and the package tails.
void CRender::svp_accumulate_prebuilt(light_Package& LP)
{
    PIX_EVENT(SVP_PREBUILT_SHADOWS);

    auto& cmd_list = get_imm_context().cmd_list;

    const bool do_replay = true;
    const bool do_publish = true;

    // Point the by-name s_smap samplers ($user$smap_depth) at the atlas for the accumulation
    // window; self-restored right after (same registry mechanism as svp_publish_surfaces).
    const bool published = do_publish ? Target->svp_publish_smap_atlas(true) : false;

    // Mark-guarded: the first call per frame clears, later calls just rebind and keep summing.
    Target->phase_accumulator(cmd_list);

    // The stage-1 call consumes the worker's EXACT results, page by page: every page list
    // starts with a full atlas clear, so page i's lights MUST be accumulated before page i+1's
    // list executes (their shadow rects overlap across pages). Kept-but-unpacked lights
    // accumulate UNSHADOWED after all pages. The stage-2 call (second package) only drains the
    // unshadowed tails - the shadowed set was fully handled by the first call.
    if (svp_shadow_stage == 1)
    {
        const size_t page_count = _min(svp_smap_replay_lists.size(), svp_shadow_page_lights.size());


#if defined(USE_DX11)
        auto replay_ctx = HW.get_context(CHW::IMM_CTX_ID);
        for (size_t i = 0; i < page_count; ++i)
        {
            if (do_replay)
            {
                replay_ctx->ExecuteCommandList(static_cast<ID3D11CommandList*>(svp_smap_replay_lists[i]), false);
                get_imm_context().cmd_list.Invalidate(); // ExecuteCommandList breaks the CPU state cache
                // The executed list leaves the tiny smap-rect viewport of its last light - the
                // accumulation quads below would rasterize into a corner of the accumulator.
                // (The non-interleaved original got this restore from phase_accumulator's
                // rmNormal, which ran AFTER all replays; here it must happen per page.)
                RImplementation.rmNormal(cmd_list);
            }

            const xr_vector<light*>& page = svp_shadow_page_lights[i];
            if (!page.empty())
            {
                PIX_EVENT(ACCUM_SPOT);
                for (light* p_light : page)
                {
                    Target->accum_spot(cmd_list, p_light);
                    render_indirect(p_light);
                }

                PIX_EVENT(ACCUM_VOLUMETRIC);
                if (RImplementation.o.advancedpp && ps_r2_ls_flags.is(R2FLAG_VOLUMETRIC_LIGHTS))
                    for (light* p_light : page)
                        Target->accum_volumetric(cmd_list, p_light);
            }
        }
#endif

        // Unshadowed fallback: temporarily clear bShadow so accum_spot picks SE_L_UNSHADOWED.
        // Safe - the main pass lighting is drained for this frame and the flag is restored
        // right after the draw.
        for (light* p_light : svp_shadow_unshadowed)
        {
            const bool wasShadow = p_light->flags.bShadow != 0;
            p_light->flags.bShadow = false;
            Target->accum_spot(cmd_list, p_light);
            render_indirect(p_light);
            p_light->flags.bShadow = wasShadow;
        }

        svp_shadow_page_lights.clear();
        svp_shadow_unshadowed.clear();
        ReleaseSVPReplayLists();
        svp_shadow_stage = 2;
    }
    else if (svp_shadow_stage == 3)
    {
#if defined(USE_DX11)
        // Shadow-transfer mode (roadmap A.2): pages were COPIED into the SVP atlas slices by
        // the main pass (see render_lights) - there are no sealed lists to replay and no
        // worker build. Every slice holds one page's content permanently, so the per-page
        // interleave requirement is gone; rebind the sampler to the page's slice before its
        // quads (the per-slice SRV swap render_sun::accumulate_cascade uses per cascade) and
        // accumulate with the MAIN pass's X.S placements, valid for the whole frame.
        ZoneScopedN("SVP accum transfer");
        for (size_t i = 0; i < svp_shadow_page_lights.size(); ++i)
        {
            Target->rt_smap_depth->pTexture->set_slice(int(i));
            const xr_vector<light*>& page = svp_shadow_page_lights[i];
            if (!page.empty())
            {
                PIX_EVENT(ACCUM_SPOT);
                for (light* p_light : page)
                {
                    Target->accum_spot(cmd_list, p_light);
                    render_indirect(p_light);
                }

                PIX_EVENT(ACCUM_VOLUMETRIC);
                if (RImplementation.o.advancedpp && ps_r2_ls_flags.is(R2FLAG_VOLUMETRIC_LIGHTS))
                    for (light* p_light : page)
                        Target->accum_volumetric(cmd_list, p_light);
            }
        }

        // Unshadowed fallback (page-cap overflow), same semantics as stage 1.
        for (light* p_light : svp_shadow_unshadowed)
        {
            const bool wasShadow = p_light->flags.bShadow != 0;
            p_light->flags.bShadow = false;
            Target->accum_spot(cmd_list, p_light);
            render_indirect(p_light);
            p_light->flags.bShadow = wasShadow;
        }

        svp_shadow_page_lights.clear();
        svp_shadow_unshadowed.clear();
        svp_shadow_stage = 2;
#endif // USE_DX11
    }

    if (published)
        Target->svp_publish_smap_atlas(false); // smap samplers done - restore before the tails

    LP.v_shadowed.clear();

    // Unshadowed tails: the package was prefiltered against the narrow lens frustum, so there
    // is no visibility gating here (frame-driver stage 1b semantics).
    PIX_EVENT(POINT_LIGHTS_ACCUM);
    for (light* p_light : LP.v_point)
    {
        render_indirect(p_light);
        Target->accum_point(cmd_list, p_light);
    }
    LP.v_point.clear();

    PIX_EVENT(SPOT_LIGHTS_ACCUM);
    for (light* p_light : LP.v_spot)
    {
        LR.compute_xf_spot(p_light);
        render_indirect(p_light);
        Target->accum_spot(cmd_list, p_light);
    }
    LP.v_spot.clear();
}
} // namespace xray::render::RENDER_NAMESPACE
