#include "stdafx.h"
#include "Layers/xrRender/du_box.h"

namespace xray::render::RENDER_NAMESPACE
{
void CRender::render_lights(light_Package& LP)
{
    ZoneScoped;

    //////////////////////////////////////////////////////////////////////////
    // Refactor order based on ability to pack shadow-maps
    // 1. calculate area + sort in descending order
    // const	u16		smap_unassigned		= u16(-1);
    {
        xr_vector<light*>& source = LP.v_shadowed;
        xr_vector<light*> kept;
        kept.reserve(source.size());

        const float shadow_dist = ps_r2_light_shadow_dist;

        for (u32 it = 0; it < source.size(); it++)
        {
            light* L = source[it];
            L->vis_update();
            if (!L->vis.visible)
                continue; // drop invisible

            // Distance-based shadow skip: render shadow maps only for lights whose volume
            // edge is close enough to the camera. Skipped lights disappear entirely (no
            // unshadowed fallback — it lights through walls).
            // For OMNIPART use the parent position+range: all 6 parts of one shadowed
            // point light share it, so they are skipped/kept together automatically
            // (independent per-part decisions produce a half-lit sphere).
            // Hysteresis: a light rendered on the previous frame is kept until 20% past
            // the threshold — no popping when walking back and forth near the border.
            if (shadow_dist > 0.f)
            {
                float dist;
                if (L->flags.type == IRender_Light::OMNIPART)
                    dist = Device.vCameraPosition.distance_to(L->position) - L->range;
                else
                    dist = Device.vCameraPosition.distance_to(L->spatial.sphere.P) - L->spatial.sphere.R;
                if (dist < 0.f)
                    dist = 0.f;

                const bool was_rendered = (Device.dwFrame - L->shadow_render_frame) <= 1;
                const float limit = was_rendered ? shadow_dist * 1.2f : shadow_dist;
                if (dist > limit)
                    continue;
            }

            // Secondary LOD skip (usually off; distance culling above is the main tool)
            if (ps_r2_light_degrade_lod > 0.f && L->get_LOD() < ps_r2_light_degrade_lod)
                continue;

            kept.push_back(L);
            L->shadow_render_frame = Device.dwFrame;
            LR.compute_xf_spot(L);
        }

        // Budget cap (safety net for extreme scenes): keep the closest lights.
        if (ps_r2_light_shadow_budget > 0 && kept.size() > (size_t)ps_r2_light_shadow_budget)
        {
            std::sort(kept.begin(), kept.end(), [](light* l1, light* l2)
            {
                return l1->get_LOD() > l2->get_LOD();
            });
            kept.resize((size_t)ps_r2_light_shadow_budget);
        }

        LP.v_shadowed = std::move(kept);
    }

    // 2. refactor - infact we could go from the backside and sort in ascending order
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

    // Helper: record one light's shadow map into dsgraph.cmd_list (immediate or deferred).
    // Shared stats/L_spot_s updates are NOT done here — caller decides where they are safe.
    const auto render_light_smap = [this](R_dsgraph_structure& dsgraph, light* L)
    {
        const bool bNormal = !dsgraph.mapNormalPasses[0][0].empty() || !dsgraph.mapMatrixPasses[0][0].empty();
        const bool bSpecial = !dsgraph.mapNormalPasses[1][0].empty() || !dsgraph.mapMatrixPasses[1][0].empty() ||
            !dsgraph.mapSorted.empty();
        if (!bNormal && !bSpecial)
            return false;

        PIX_EVENT_CTX(dsgraph.cmd_list, SHADOWED_LIGHT);
        Target->phase_smap_spot(dsgraph.cmd_list, L);
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
            // Reuse the spot light's depth-only shader element (SE_R2_SHADOW) — in the SMAP
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
    };

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
                    static_cast<ID3D11CommandList*>(item.cmdList)->Release();
                    item.cmdList = nullptr;
                    any_submit = true;
                }
#endif
            }
            else
            {
                // Serial mode: recording happens here on the main thread (original behavior)
                if (render_light_smap(dsgraph, L))
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

    // Single shared spatial query for all light passes: one q_sphere around the camera
    // replaces a per-light q_frustum octree walk. Radius covers every light volume that
    // survived the distance cull (shadow_dist + max light range with margin).
    static xr_vector<ISpatial*> common_dynamic;
    if (ps_r2_light_common_dynamic > 0 && !LP.v_shadowed.empty())
    {
        ZoneScopedN("light_common_dynamic_query");
        float max_reach = ps_r2_light_shadow_dist;
        for (light* Ld : LP.v_shadowed)
            max_reach = _max(max_reach, Device.vCameraPosition.distance_to(Ld->spatial.sphere.P) + Ld->range);
        common_dynamic.clear();
        g_pGamePersistent->SpatialSpace.q_sphere(common_dynamic, 0, STYPE_RENDERABLE, Device.vCameraPosition, max_reach * 1.05f);
    }

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
            const auto& calc_lights = [item_ptr = &item, mt_light, &render_light_smap]
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

                    dsgraph.build_subspace();

                    // Parallel mode: record the shadow map draw calls into the deferred context
                    // right here in the task. Stats/L_spot_s are NOT touched (race); the main
                    // thread handles them in flush_lights after Wait.
                    if (mt_light)
                    {
                        if (render_light_smap(dsgraph, L))
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

        PIX_EVENT(UNSHADOWED_LIGHTS);

        //		switch-to-accumulator
        Target->phase_accumulator(cmd_list);

        PIX_EVENT(POINT_LIGHTS);

        //		if (has_point_unshadowed)	-> 	accum point unshadowed
        if (!LP.v_point.empty())
        {
            light* L2 = LP.v_point.back();
            LP.v_point.pop_back();
            L2->vis_update();
            if (L2->vis.visible)
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
            L2->vis_update();
            if (L2->vis.visible)
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
            p_light->vis_update();
            if (p_light->vis.visible)
            {
                render_indirect(p_light);
                Target->accum_point(cmd_list, p_light);
            }
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
            p_light->vis_update();
            if (p_light->vis.visible)
            {
                LR.compute_xf_spot(p_light);
                render_indirect(p_light);
                Target->accum_spot(cmd_list, p_light);
            }
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
} // namespace xray::render::RENDER_NAMESPACE
