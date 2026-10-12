#include "stdafx.h"

#include <cmath>
#include <cfloat>
#include <atomic>
#include <memory>
#include <thread>

#include "FHierrarhyVisual.h"
#include "SkeletonCustom.h"
#include "xrCore/Threading/ParallelFor.hpp"
#include "xrEngine/CustomHUD.h"
#include "xrEngine/IRenderable.h"
#include "xrEngine/xr_object.h"

#include "FLOD.h"

#include "LightTrack.h"
#include "ParticleGroup.h"
#include "FTreeVisual.h"
#include "xrCDB/ISpatial.h"
#include "xrCore/Profiling/PerfMetrics.hpp"

namespace xray::render::RENDER_NAMESPACE
{
using namespace R_dsgraph;

////////////////////////////////////////////////////////////////////////////////////////////////////
// Scene graph actual insertion and sorting ////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
float r_ssaDISCARD;
float r_ssaDONTSORT;
float r_ssaLOD_A, r_ssaLOD_B;
float r_ssaLOD_CHAR_A, r_ssaLOD_CHAR_B;
float r_ssaGLOD_start, r_ssaGLOD_end;
float r_ssaHZBvsTEX;

ICF float CalcSSA(float& distSQ, Fvector& C, dxRender_Visual* V)
{
    float R = V->vis.sphere.R + 0;
    distSQ = Device.vCameraPosition.distance_to_sqr(C) + EPS;
    return R / distSQ;
}
ICF float CalcSSA(float& distSQ, Fvector& C, float R)
{
    distSQ = Device.vCameraPosition.distance_to_sqr(C) + EPS;
    return R / distSQ;
}

void R_dsgraph_structure::insert_dynamic(IRenderable* root, dxRender_Visual* pVisual, Fmatrix& xform, Fvector& Center)
{
    // No ZoneScoped: called from build_subspace() on task workers (sun SMAP mt_calc); Tracy rpmalloc/queue can AV.
    CRender& RI = RImplementation;

    if (pVisual->vis.marker[context_id] == marker)
        return;
    pVisual->vis.marker[context_id] = marker;

#if RENDER == R_R1
    if (RI.o.vis_intersect && (pVisual->vis.accept_frame != Device.dwFrame))
        return;
    pVisual->vis.accept_frame = Device.dwFrame;
#endif

    float distSQ;
    float SSA = CalcSSA(distSQ, Center, pVisual);
    if (SSA <= r_ssaDISCARD)
        return;
    const bool cheapCorpse = root && root->renderable_CheapCorpsePath();
    if (cheapCorpse && o.phase == CRender::PHASE_SMAP)
    {
        ++RI.BasicStats.CorpseSmapSkipped;
        PERF_INC(render_corpse_smap_skip);
        return;
    }

    // Distortive geometry should be marked and R2 special-cases it
    // a) Allow to optimize RT order
    // b) Should be rendered to special distort buffer in another pass
    VERIFY(pVisual->shader._get());
    const Shader* vis_sh = pVisual->shader._get();
    ShaderElement* sh_d = vis_sh ? vis_sh->E[4]._get() : nullptr; // 4=L_special
    if (!cheapCorpse && sh_d)
    {
        if (RImplementation.o.distortion && sh_d && sh_d->flags.bDistort && o.pmask[sh_d->flags.iPriority / 2])
        {
            mapDistort.insert_anyway(distSQ, _MatrixItemS({ SSA, root, pVisual, xform, sh_d })); // sh_d -> L_special
        }
    }

    // Select shader
    ShaderElement* sh = RImplementation.rimp_select_sh_dynamic(pVisual, distSQ, o.phase);
    if (nullptr == sh)
        return;
    if (!o.pmask[sh->flags.iPriority / 2])
        return;

    // HUD rendering
    if (root && root->renderable_HUD())
    {
        if (sh->flags.bStrictB2F)
        {
            mapHUDSorted.insert_anyway(distSQ, _MatrixItemS({ SSA, root, pVisual, xform, sh }));
            return;
        }
        mapHUD.insert_anyway(distSQ, _MatrixItemS({ SSA, root, pVisual, xform, sh }));

#if RENDER != R_R1
        if (sh->flags.bEmissive && sh_d)
            mapHUDEmissive.insert_anyway(distSQ, _MatrixItemS({ SSA, root, pVisual, xform, sh_d })); // sh_d -> L_special
#endif
        return;
    }

// Shadows registering
#if RENDER == R_R1
    RI.L_Shadows->add_element(_MatrixItem{ SSA, root, pVisual, xform });
#endif
    if (root && root->renderable_Invisible())
        return;

    // strict-sorting selection
    if (sh->flags.bStrictB2F)
    {
        mapSorted.insert_anyway(distSQ, _MatrixItemS({ SSA, root, pVisual, xform, sh }));
        return;
    }

#if RENDER != R_R1
    // Emissive geometry should be marked and R2 special-cases it
    // a) Allow to skeep already lit pixels
    // b) Allow to make them 100% lit and really bright
    // c) Should not cast shadows
    // d) Should be rendered to accumulation buffer in the second pass
    if (sh->flags.bEmissive)
    {
        mapEmissive.insert_anyway(distSQ, _MatrixItemS({ SSA, root, pVisual, xform, sh_d })); // sh_d -> L_special
    }
    if (sh->flags.bWmark && o.pmask_wmark)
    {
        mapWmark.insert_anyway(distSQ, _MatrixItemS({ SSA, root, pVisual, xform, sh }));
        return;
    }
#endif

    for (u32 iPass = 0; iPass < sh->passes.size(); ++iPass)
    {
        SPass* pass = sh->passes[iPass]._get();
        if (!pass)
            continue;
        mapMatrix_T& map = mapMatrixPasses[sh->flags.iPriority / 2][iPass];
        mapMatrixItems& matrixItems = map[pass];

        // Create common node
        // NOTE: Invisible elements exist only in R1
        matrixItems.emplace_back(_MatrixItem{ SSA, root, pVisual, xform });

        // Need to sort for HZB efficient use
        if (SSA > matrixItems.ssa)
        {
            matrixItems.ssa = SSA;
        }
    }

#if RENDER != R_R1
    if (val_recorder)
    {
        Fbox3 temp;
        temp.xform(pVisual->vis.box, xform);
        val_recorder->push_back(temp);
    }
#endif
}

bool R_dsgraph_structure::shadow_hull_cull(const Fbox& caster_box) const
{
    CRender& RI = RImplementation;

    Fbox hull;
    hull.invalidate();
    hull.modify(o.shadow_light_pos);
    Fvector corner;
    for (int i = 0; i < 8; ++i)
    {
        caster_box.getpoint(i, corner);
        corner.sub(o.shadow_light_pos);
        const float len = corner.magnitude();
        if (len > EPS)
        {
            // Extend through the caster to the light range: the shadow falls BEHIND
            // the caster (away from the light), so the hull must reach light range.
            corner.mul(o.shadow_light_range / len);
            corner.add(o.shadow_light_pos);
            hull.modify(corner);
        }
    }
    // Project the caster CENTER to range as well: due to sphere curvature the center
    // ray reaches deeper than any corner ray. Without this the hull misses the deepest
    // shadow and culls walls that still block light.
    Fvector caster_center;
    caster_box.getcenter(caster_center);
    caster_center.sub(o.shadow_light_pos);
    const float center_len = caster_center.magnitude();
    if (center_len > EPS)
    {
        caster_center.mul(o.shadow_light_range / center_len);
        caster_center.add(o.shadow_light_pos);
        hull.modify(caster_center);
    }
    // Safety margin: expand by the caster bounding-sphere radius so any geometric
    // edge case stays inside the hull.
    const float caster_radius = caster_box.getradius();
    hull.grow(caster_radius);

    Fvector hull_center;
    hull.getcenter(hull_center);
    const float hull_radius = hull.getradius();
    u32 mask = FRUSTUM_SAFE;
    return fcvNone == RI.ViewBase.testSAABB(hull_center, hull_radius, hull.data(), mask);
}

void R_dsgraph_structure::insert_static(dxRender_Visual* pVisual)
{
    PERF_INC(render_static);
    if (pVisual->vis.marker[context_id] == marker)
        return;
    pVisual->vis.marker[context_id] = marker;

    // Shadow-caster culling: skip casters whose shadow hull (light apex extruded through the
    // caster AABB up to light range) cannot reach the main camera frustum at all.
    if (o.use_shadow_hull_cull)
    {
        Fbox hull;
        hull.invalidate();
        hull.modify(o.shadow_light_pos);
        Fvector corner;
        for (int i = 0; i < 8; ++i)
        {
            pVisual->vis.box.getpoint(i, corner);
            corner.sub(o.shadow_light_pos);
            const float len = corner.magnitude();
            if (len > EPS)
            {
                corner.mul(o.shadow_light_range / len);
                corner.add(o.shadow_light_pos);
                hull.modify(corner);
            }
        }
        Fvector caster_center;
        pVisual->vis.box.getcenter(caster_center);
        caster_center.sub(o.shadow_light_pos);
        const float center_len = caster_center.magnitude();
        if (center_len > EPS)
        {
            caster_center.mul(o.shadow_light_range / center_len);
            caster_center.add(o.shadow_light_pos);
            hull.modify(caster_center);
        }
        const float caster_radius = pVisual->vis.box.getradius();
        hull.grow(caster_radius);

        Fvector hull_center;
        hull.getcenter(hull_center);
        const float hull_radius = hull.getradius();
        u32 mask = FRUSTUM_SAFE;
        if (fcvNone == RImplementation.ViewBase.testSAABB(hull_center, hull_radius, hull.data(), mask))
            return;
    }

#if RENDER == R_R1
    if (RImplementation.o.vis_intersect && (pVisual->vis.accept_frame != Device.dwFrame))
        return;
    pVisual->vis.accept_frame = Device.dwFrame;
#endif

    float distSQ;
    float SSA = CalcSSA(distSQ, pVisual->vis.sphere.P, pVisual);
    if (SSA <= r_ssaDISCARD)
        return;

    // Distortive geometry should be marked and R2 special-cases it
    // a) Allow to optimize RT order
    // b) Should be rendered to special distort buffer in another pass
    VERIFY(pVisual->shader._get());
    const Shader* vis_sh = pVisual->shader._get();
    ShaderElement* sh_d = vis_sh ? vis_sh->E[4]._get() : nullptr; // 4=L_special
    if (sh_d)
    {
        if (RImplementation.o.distortion && sh_d && sh_d->flags.bDistort && o.pmask[sh_d->flags.iPriority / 2])
        {
            mapDistort.insert_anyway(distSQ, _MatrixItemS({ SSA, nullptr, pVisual, Fidentity, sh_d })); // sh_d -> L_special
        }
    }

    // Select shader
    ShaderElement* sh = RImplementation.rimp_select_sh_static(pVisual, distSQ, o.phase);
    if (nullptr == sh)
        return;
    if (!o.pmask[sh->flags.iPriority / 2])
        return;

    // strict-sorting selection
    if (sh->flags.bStrictB2F)
    {
        // TODO: Выяснить, почему в единственном месте параметр ssa не используется
        // Визуально различий не замечено
        mapSorted.insert_anyway(distSQ, _MatrixItemS({ /*0*/SSA, nullptr, pVisual, Fidentity, sh }));
        return;
    }

#if RENDER != R_R1
    // Emissive geometry should be marked and R2 special-cases it
    // a) Allow to skeep already lit pixels
    // b) Allow to make them 100% lit and really bright
    // c) Should not cast shadows
    // d) Should be rendered to accumulation buffer in the second pass
    if (sh->flags.bEmissive && sh_d)
    {
        mapEmissive.insert_anyway(distSQ, _MatrixItemS({ SSA, nullptr, pVisual, Fidentity, sh_d })); // sh_d -> L_special
    }
    if (sh->flags.bWmark && o.pmask_wmark)
    {
        mapWmark.insert_anyway(distSQ, _MatrixItemS({ SSA, nullptr, pVisual, Fidentity, sh }));
        return;
    }
#endif

    if (val_feedback && counter_S == val_feedback_breakp)
        val_feedback->rfeedback_static(pVisual);

    counter_S++;

    for (u32 iPass = 0; iPass < sh->passes.size(); ++iPass)
    {
        SPass* pass = sh->passes[iPass]._get();
        if (!pass)
            continue;
        mapNormal_T& map = mapNormalPasses[sh->flags.iPriority / 2][iPass];
        mapNormalItems& normalItems = map[pass];

        normalItems.emplace_back(_NormalItem{ SSA, pVisual });

        // Need to sort for HZB efficient use
        if (normalItems.ssa && SSA > normalItems.ssa)
        {
            normalItems.ssa = SSA;
        }
    }

#if RENDER != R_R1
    if (val_recorder)
    {
        val_recorder->push_back(pVisual->vis.box);
    }
#endif
}

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
void R_dsgraph_structure::add_leafs_dynamic(
    IRenderable* root, dxRender_Visual* pVisual, Fmatrix& xform, CKinematics* lod_bind_source)
{
    if (nullptr == pVisual)
        return;

    // Visual is 100% visible - simply add it
    switch (pVisual->Type)
    {
    case MT_PARTICLE_GROUP:
    {
        // Add all children, doesn't perform any tests
        PS::CParticleGroup* pG = (PS::CParticleGroup*)pVisual;
        for (auto& it : pG->items)
        {
            PS::CParticleGroup::SItem& I = it;
            if (I._effect)
                add_leafs_dynamic(root, I._effect, xform, nullptr);
            for (auto& pit : I._children_related)
                add_leafs_dynamic(root, pit, xform, nullptr);
            for (auto& pit : I._children_free)
                add_leafs_dynamic(root, pit, xform, nullptr);
        }
    }
        return;
    case MT_HIERRARHY:
    {
        // Add all children, doesn't perform any tests
        FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
        for (auto& i : pV->children)
        {
            //i->vis.obj_data = pV->getVisData().obj_data; // Наследники используют шейдерные данные от родительского визуала
                                                         // [use shader data from parent model, rather than it childrens]

            add_leafs_dynamic(root, i, xform, nullptr);
        }
    }
        return;
    case MT_SKELETON_ANIM:
    case MT_SKELETON_RIGID:
    {
        if (root && root->renderable_CheapCorpsePath() && o.phase == CRender::PHASE_SMAP)
        {
            ++RImplementation.BasicStats.CorpseSmapSkipped;
            PERF_INC(render_corpse_smap_skip);
            return; // Corpse cheap tier never participates in sun shadow-map traversal.
        }

        // Add all children, doesn't perform any tests
        CKinematics* pV = (CKinematics*)pVisual;
        const bool skinning_from_parent = lod_bind_source && lod_bind_source != pV &&
            pV->LL_BoneCount() == lod_bind_source->LL_BoneCount();
        const bool corpse_pose_frozen = root && root->renderable_CorpsePoseFrozen();

        if (skinning_from_parent)
        {
            PERF_INC(render_skin_from_parent);
            pV->CopyBoneTransformsFrom(*lod_bind_source);
        }

        // r2_smap_npc_blob 3: skip NPC entirely from shadow map (no CalculateBones,
        // no draw calls, no blob — NPC casts no shadow at all). Maximum CPU savings.
        if (o.phase == CRender::PHASE_SMAP && ps_r2_smap_npc_blob >= 3)
        {
            PERF_INC(render_blob_skip);
            return;
        }

        // Cheap blob shadow in SMAP passes: for distant NPCs render a simple box instead of
        // the full skeleton (no CalculateBones, 12 tris). Matrix scales the unit cube to the
        // NPC bounding sphere.
        if (o.phase == CRender::PHASE_SMAP && ps_r2_smap_npc_blob > 0)
        {
            const bool blob_forced = ps_r2_smap_npc_blob >= 2;
            const bool blob_fallback = (ps_r2_smap_npc_blob == 1) && !pV->m_lod;
            if (blob_forced || blob_fallback)
            {
                // Screen-size test from the CAMERA (same metric as the LOD logic below)
                Fvector Tpos;
                float D;
                xform.transform_tiny(Tpos, pV->vis.sphere.P);
                const float ssa = CalcSSA(D, Tpos, pV->vis.sphere.R / 2.f);
                const float ssaLodA =
                    (root && root->renderable_SsaLodCharacter()) ? r_ssaLOD_CHAR_A : r_ssaLOD_A;
                if (ssa < ssaLodA)
                {
                    // Build blob matrix: unit cube scaled to the bounding sphere, centered on NPC
                    Fmatrix mBlob;
                    mBlob.scale(pV->vis.sphere.R * 0.7f, pV->vis.sphere.R * 0.5f, pV->vis.sphere.R * 0.7f);
                    Fvector vCenter;
                    xform.transform_tiny(vCenter, pV->vis.sphere.P);
                    vCenter.y -= pV->vis.sphere.R * 0.5f; // drop to the ground: blob center at feet level
                    mBlob.c = vCenter;
                    npc_blobs.push_back(mBlob);
                    PERF_INC(render_blob_add);
                    return; // skip the full skeleton entirely (no CalculateBones!)
                }
            }
        }

        BOOL _use_lod = FALSE;
        if (pV->m_lod && (!root || root->renderable_AllowCharacterMeshLod()))
        {
            if (root && root->renderable_ForceLodCharacter())
                _use_lod = TRUE;
            else
            {
                Fvector Tpos;
                float D;
                xform.transform_tiny(Tpos, pV->vis.sphere.P);
                float ssa = CalcSSA(D, Tpos, pV->vis.sphere.R / 2.f); // assume dynamics never consume full sphere
                const float ssaLodA =
                    (root && root->renderable_SsaLodCharacter()) ? r_ssaLOD_CHAR_A : r_ssaLOD_A;
                if (ssa < ssaLodA)
                    _use_lod = TRUE;
            }
        }
        if (_use_lod)
        {
            if (!corpse_pose_frozen)
            {
                pV->BonesPrepass_MarkRendered(); // bones prepass: candidate of the next frame's prepass
                pV->CalculateBones(TRUE);
            }
            else
                PERF_INC(render_corpse_pose_skip);
            PERF_INC(render_skel_lod);
            PERF_ADD_IF(o.is_main_pass && !o.second_vp_pass, rmain_skel_lod, 1); // P15
            add_leafs_dynamic(root, pV->m_lod, xform, pV);
        }
        else
        {
            if (!skinning_from_parent && !corpse_pose_frozen)
            {
                pV->BonesPrepass_MarkRendered(); // bones prepass: candidate of the next frame's prepass
                pV->CalculateBones(TRUE);
            }
            else if (corpse_pose_frozen)
                PERF_INC(render_corpse_pose_skip);
            PERF_INC(render_skel_full);
            PERF_ADD_IF(o.is_main_pass && !o.second_vp_pass, rmain_skel_full, 1); // P15
            if (o.phase == CRender::PHASE_NORMAL && !skinning_from_parent && !corpse_pose_frozen)
            {
                // Stage C: wallmark capture mutates the shared wallmark manager - the concurrent
                // SVP build must not run it (the main build captures this frame's marks).
                if (!o.second_vp_pass)
                    pV->CalculateWallmarks(root ? root->renderable_HUD() : false); //. bug?
            }
            for (auto& i : pV->children)
            {
                //i->vis.obj_data = pV->getVisData().obj_data; // Наследники используют шейдерные данные от родительского визуала
                                                             // [use shader data from parent model, rather than it childrens]
                add_leafs_dynamic(root, i, xform, nullptr);
            }
        }
    }
        return;
    default:
    {
        // General type of visual
        // Calculate distance to it's center
        Fvector Tpos;
        xform.transform_tiny(Tpos, pVisual->vis.sphere.P);
        insert_dynamic(root, pVisual, xform, Tpos);
    }
        return;
    }
}

void R_dsgraph_structure::add_leafs_static(dxRender_Visual* pVisual)
{
    if (o.use_hom && !RImplementation.HOM.visible(pVisual->vis))
        return;

    // Visual is 100% visible - simply add it
    switch (pVisual->Type)
    {
    case MT_PARTICLE_GROUP:
    {
        // Xottab_DUTY: for dynamic objects we need matrixб
        // which is nullptr, when we use add_leafs_Static
        Log("Dynamic particles added via static procedure. Please, contact Xottab_DUTY and tell him about the issue.");
        NODEFAULT;

        // Add all children, doesn't perform any tests
        /*PS::CParticleGroup* pG = (PS::CParticleGroup*)pVisual;
        for (auto& it : pG->items)
        {
            PS::CParticleGroup::SItem& I = it;
            if (I._effect)
                add_leafs_Dynamic(I._effect);
            for (auto& pit : I._children_related)
                add_leafs_Dynamic(pit);
            for (auto& pit : I._children_free)
                add_leafs_Dynamic(pit);
        }*/
    }
    return;
    case MT_HIERRARHY:
    {
        // Add all children, doesn't perform any tests
        FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
        for (auto& i : pV->children)
        {
            //i->vis.obj_data = pV->getVisData().obj_data; // Наследники используют шейдерные данные от родительского визуала
                                                         // [use shader data from parent model, rather than it childrens]
            add_leafs_static(i);
        }
    }
    return;
    case MT_SKELETON_ANIM:
    case MT_SKELETON_RIGID:
    {
        // Add all children, doesn't perform any tests
        CKinematics* pV = (CKinematics*)pVisual;
        pV->CalculateBones(TRUE);
        for (auto& i : pV->children)
        {
            //i->vis.obj_data = pV->getVisData().obj_data; // Наследники используют шейдерные данные от родительского визуала
                                                         // [use shader data from parent model, rather than it childrens]
            add_leafs_static(i);
        }
    }
    return;
    case MT_LOD:
    {
        FLOD* pV = (FLOD*)pVisual;
        float D;
        float ssa = CalcSSA(D, pV->vis.sphere.P, pV);
        ssa *= pV->lod_factor;
        if (ssa < r_ssaLOD_A)
        {
            if (ssa < r_ssaDISCARD)
                return;
            mapLOD.insert_anyway(D, _LodItem({ ssa, pVisual }));
        }
#if RENDER != R_R1
        if (ssa > r_ssaLOD_B || o.phase == CRender::PHASE_SMAP)
#else
        if (ssa > r_ssaLOD_B)
#endif
        {
            // Add all children, doesn't perform any tests
            for (auto& i : pV->children)
            {
                //i->vis.obj_data = pV->getVisData().obj_data; // Наследники используют шейдерные данные от родительского визуала
                                                             // [use shader data from parent model, rather than it childrens]
                add_leafs_static(i);
            }
        }
    }
    return;
    case MT_TREE_PM:
    case MT_TREE_ST:
    {
        // General type of visual
        insert_static(pVisual);
    }
    return;
    default:
    {
        // General type of visual
        insert_static(pVisual);
    }
    return;
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

/* Xottab_DUTY: this function is only called from add_Static,
 * but we need a matrix, which is nullptr at this point
BOOL R_dsgraph_structure::add_Dynamic(dxRender_Visual* pVisual, u32 planes) // normal processing
{
    // Check frustum visibility and calculate distance to visual's center
    Fvector Tpos; // transformed position
    EFC_Visible VIS;

    val_pTransform->transform_tiny(Tpos, pVisual->vis.sphere.P);
    VIS = View->testSphere(Tpos, pVisual->vis.sphere.R, planes);
    if (fcvNone == VIS)
        return FALSE;

    // If we get here visual is visible or partially visible
    switch (pVisual->Type)
    {
    case MT_PARTICLE_GROUP:
    {
        // Add all children, doesn't perform any tests
        PS::CParticleGroup* pG = (PS::CParticleGroup*)pVisual;
        for (auto& it : pG->items)
        {
            PS::CParticleGroup::SItem& I = it;
            if (fcvPartial == VIS)
            {
                if (I._effect)
                    add_Dynamic(I._effect, planes);
                for (auto& pit : I._children_related)
                    add_Dynamic(pit, planes);
                for (auto& pit : I._children_free)
                    add_Dynamic(pit, planes);
            }
            else
            {
                if (I._effect)
                    add_leafs_Dynamic(I._effect);
                for (auto& pit : I._children_related)
                    add_leafs_Dynamic(pit);
                for (auto& pit : I._children_free)
                    add_leafs_Dynamic(pit);
            }
        }
    }
    break;
    case MT_HIERRARHY:
    {
        // Add all children
        FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
        if (fcvPartial == VIS)
        {
            for (auto& i : pV->children)
                add_Dynamic(i, planes);
        }
        else
        {
            for (auto& i : pV->children)
                add_leafs_Dynamic(i);
        }
    }
    break;
    case MT_SKELETON_ANIM:
    case MT_SKELETON_RIGID:
    {
        // Add all children, doesn't perform any tests
        CKinematics* pV = (CKinematics*)pVisual;
        BOOL _use_lod = FALSE;
        if (pV->m_lod)
        {
            Fvector Tpos2;
            float D;
            val_pTransform->transform_tiny(Tpos2, pV->vis.sphere.P);
            float ssa = CalcSSA(D, Tpos2, pV->vis.sphere.R / 2.f); // assume dynamics never consume full sphere
            if (ssa < r_ssaLOD_A)
                _use_lod = TRUE;
        }
        if (_use_lod)
        {
            add_leafs_Dynamic(pV->m_lod);
        }
        else
        {
            pV->CalculateBones(TRUE);
            pV->CalculateWallmarks(val_pObject ? val_pObject->renderable_HUD() : false); //. bug?
            for (auto& i : pV->children)
                add_leafs_Dynamic(i);
        }
    }
    break;
    default:
    {
        // General type of visual
        r_dsgraph_insert_dynamic(pVisual, Tpos);
    }
    break;
    }
    return TRUE;
}*/

void R_dsgraph_structure::add_static(dxRender_Visual* pVisual, const CFrustum& view, u32 planes)
{
    vis_data& vis = pVisual->vis;

    // Check frustum visibility and calculate distance to visual's center
    const EFC_Visible VIS = view.testSAABB(vis.sphere.P, vis.sphere.R, vis.box.data(), planes);
    if (fcvNone == VIS)
        return;

    if (o.use_hom && !RImplementation.HOM.visible(vis))
        return;

    PERF_INC(render_static);

    // If we get here visual is visible or partially visible
    switch (pVisual->Type)
    {
    case MT_PARTICLE_GROUP:
    {
        // Xottab_DUTY: for dynamic objects we need matrix,
        // which is nullptr, when we use add_Static
        Log("Dynamic particles added via static procedure. Please, contact Xottab_DUTY and tell him about the issue.");
        NODEFAULT;

        // Add all children, doesn't perform any tests
        /*PS::CParticleGroup* pG = (PS::CParticleGroup*)pVisual;
        for (auto& it : pG->items)
        {
            PS::CParticleGroup::SItem& I = it;
            if (fcvPartial == VIS)
            {
                if (I._effect)
                    add_Dynamic(I._effect, planes);
                for (auto& pit : I._children_related)
                    add_Dynamic(pit, planes);
                for (auto& pit : I._children_free)
                    add_Dynamic(pit, planes);
            }
            else
            {
                if (I._effect)
                    add_leafs_Dynamic(I._effect);
                for (auto& pit : I._children_related)
                    add_leafs_Dynamic(pit);
                for (auto& pit : I._children_free)
                    add_leafs_Dynamic(pit);
            }
        }*/
    }
    break;
    case MT_HIERRARHY:
    {
        // Add all children
        FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
        if (fcvPartial == VIS)
        {
            for (auto& i : pV->children)
                add_static(i, view, planes);
        }
        else
        {
            for (auto& i : pV->children)
                add_leafs_static(i);
        }
    }
    break;
    case MT_SKELETON_ANIM:
    case MT_SKELETON_RIGID:
    {
        // Add all children, doesn't perform any tests
        CKinematics* pV = (CKinematics*)pVisual;
        pV->CalculateBones(TRUE);
        if (fcvPartial == VIS)
        {
            for (auto& i : pV->children)
                add_static(i, view, planes);
        }
        else
        {
            for (auto& i : pV->children)
                add_leafs_static(i);
        }
    }
    break;
    case MT_LOD:
    {
        FLOD* pV = (FLOD*)pVisual;
        float D;
        float ssa = CalcSSA(D, pV->vis.sphere.P, pV);
        ssa *= pV->lod_factor;
        if (ssa < r_ssaLOD_A)
        {
            if (ssa < r_ssaDISCARD)
                return;
            mapLOD.insert_anyway(D, _LodItem({ ssa, pVisual }));
        }
#if RENDER != R_R1
        if (ssa > r_ssaLOD_B || o.phase == CRender::PHASE_SMAP)
#else
        if (ssa > r_ssaLOD_B)
#endif
        {
            // Add all children, perform tests
            for (auto& i : pV->children)
                add_leafs_static(i);
        }
    }
    break;
    case MT_TREE_ST:
    case MT_TREE_PM:
    {
        // General type of visual
        insert_static(pVisual);
    }
        return;
    default:
    {
        // General type of visual
        insert_static(pVisual);
    }
    break;
    }
}

void R_dsgraph_structure::load(const xr_vector<CSector::level_sector_data_t>& sectors_data,
    const xr_vector<CPortal::level_portal_data_t>& portals_data)
{
    ZoneScoped;

    const auto portals_count = portals_data.size();
    const auto sectors_count = sectors_data.size();

    Sectors.resize(sectors_count);
    Portals.resize(portals_count);

    for (u32 idx = 0; idx < portals_count; ++idx)
    {
        auto* portal = xr_new<CPortal>();
        Portals[idx] = portal;
    }

    for (u32 idx = 0; idx < sectors_count; ++idx)
    {
        auto* sector = xr_new<CSector>();

        sector->unique_id = static_cast<IRender_Sector::sector_id_t>(idx);
        sector->setup(sectors_data[idx], Portals);
        Sectors[idx] = sector;
    }

    for (u32 idx = 0; idx < portals_count; ++idx)
    {
        auto* portal = static_cast<CPortal*>(Portals[idx]);

        portal->setup(portals_data[idx], Sectors);
    }
}

void R_dsgraph_structure::unload()
{
    for (auto* sector : Sectors)
        xr_delete(sector);
    Sectors.clear();

    for (auto* portal : Portals)
        xr_delete(portal);
    Portals.clear();
}


////////////////////////////////////////////////////////////////////////////////////////////////////
// Parallel main build (plans/optimization_mt/08-parallel-main-build.md) ////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
// The dynamic part of the main-view build (sector refresh, HOM, renderable_Render -> add_Visual ->
// add_leafs_dynamic -> insert_dynamic for every visible object) used to run as one loop on one worker
// while the main thread waited in r_main.sync(). Here lstRenderables is cut into contiguous parts; task
// workers claim parts one by one, and each part writes into its own shard (add_Visual is redirected by
// insert_target() on the thread running the part). The shards are then appended to the main dsgraph in
// part order: every map receives exactly the insertion sequence of the sequential loop, so the result
// (sorted/transparent order included) is identical and deterministic.
namespace
{
// Which shard add_Visual() of which dsgraph lands in on this thread (nested parts save/restore it).
struct shard_binding
{
    const R_dsgraph_structure* owner{};
    R_dsgraph_structure* shard{};
};
thread_local shard_binding t_shard_binding;

constexpr u32 PARALLEL_MAIN_BUILD_MAX_PARTS = 64;

// Results of one part besides its shard's lists, kept in list order for the merge.
struct main_build_part
{
    xr_vector<light*> lights; // visible light sources to register (CLight_DB is not thread-safe)
    IRenderable* ltrack{}; // the object whose lighting (CROS) is tracked this frame, if it is in this part
    bool ran_here{}; // set by the thread that ran it (par_threads metric)
};

// Grown and used only by the main-view build (one at a time: the scope pass and the Z-prefill build never
// take this path), reused between frames - the shards keep their map memory.
xr_vector<std::unique_ptr<R_dsgraph_structure>> s_main_build_shards;
main_build_part s_main_build_parts[PARALLEL_MAIN_BUILD_MAX_PARTS];
} // namespace

R_dsgraph_structure& R_dsgraph_structure::insert_target()
{
    const shard_binding& binding = t_shard_binding;
    return binding.owner == this ? *binding.shard : *this;
}

void R_dsgraph_structure::clear_dynamic()
{
    for (auto& passes : mapMatrixPasses)
    {
        for (auto& map : passes)
        {
            // xr_fixed_map::clear() keeps the nodes: their item vectors are reused by the next insert.
            for (auto* node = map.begin(); node != map.end(); ++node)
            {
                node->second.clear();
                node->second.ssa = 0.f;
            }
            map.clear();
        }
    }
    mapSorted.clear();
    mapHUD.clear();
    mapDistort.clear();
    mapHUDSorted.clear();
#if RENDER != R_R1
    mapWmark.clear();
    mapEmissive.clear();
    mapHUDEmissive.clear();
#endif
    npc_blobs.clear();
}

void R_dsgraph_structure::merge_dynamic(R_dsgraph_structure& shard)
{
    // xr_fixed_map nodes are stored in insertion order: replaying them reproduces the shard's sequence.
    for (u32 priority = 0; priority < 2; ++priority)
    {
        for (u32 pass_id = 0; pass_id < SHADER_PASSES_MAX; ++pass_id)
        {
            mapMatrix_T& from = shard.mapMatrixPasses[priority][pass_id];
            mapMatrix_T& to = mapMatrixPasses[priority][pass_id];
            for (auto* node = from.begin(); node != from.end(); ++node)
            {
                mapMatrixItems& items = to[node->first];
                items.insert(items.end(), node->second.begin(), node->second.end());
                if (node->second.ssa > items.ssa)
                    items.ssa = node->second.ssa;
            }
        }
    }

    const auto merge_sorted = [](mapSorted_T& to, mapSorted_T& from)
    {
        for (auto* node = from.begin(); node != from.end(); ++node)
            to.insert_anyway(node->first, node->second);
    };
    merge_sorted(mapSorted, shard.mapSorted);
    merge_sorted(mapHUD, shard.mapHUD);
    merge_sorted(mapDistort, shard.mapDistort);
    merge_sorted(mapHUDSorted, shard.mapHUDSorted);
#if RENDER != R_R1
    merge_sorted(mapWmark, shard.mapWmark);
    merge_sorted(mapEmissive, shard.mapEmissive);
    merge_sorted(mapHUDEmissive, shard.mapHUDEmissive);
#endif
}

bool R_dsgraph_structure::build_dynamic_main_parallel(u32 ltrack_index, bool collect_lights)
{
    const u32 count = static_cast<u32>(lstRenderables.size());
    const u32 grain = static_cast<u32>(std::max(r__parallel_main_build_grain, 1));
    const u32 parts = std::min((count + grain - 1) / grain, PARALLEL_MAIN_BUILD_MAX_PARTS);
    const u32 workers = TaskScheduler ? static_cast<u32>(TaskScheduler->GetWorkersCount()) : 0;
    // val_recorder (old sun cascades) is a shared vector the parts would race on - sequential then.
    if (parts < 2 || workers < 2 || count < static_cast<u32>(std::max(r__parallel_main_build_min, 0)) || val_recorder)
    {
        PERF_INC(rmain_par_fallback);
        return false;
    }

    ZoneScopedN("dsg_dynamic_parallel");
    PERF_ADD(rmain_par_parts, parts);
    PERF_ADD(rmain_par_objects, count);

    while (s_main_build_shards.size() < parts)
        s_main_build_shards.emplace_back(std::make_unique<R_dsgraph_structure>());
    for (u32 p = 0; p < parts; ++p)
    {
        R_dsgraph_structure& shard = *s_main_build_shards[p];
        // Same options, context (vis.marker slot) and marker as this build: insert_dynamic dedups with them.
        shard.o = o;
        shard.context_id = context_id;
        shard.marker = marker;
        shard.val_recorder = nullptr;
        shard.val_feedback = nullptr;
        main_build_part& part = s_main_build_parts[p];
        part.lights.clear();
        part.ltrack = nullptr;
        part.ran_here = false;
    }

    // The loop body of the sequential main-pass branch of build_subspace(), with two differences:
    //  - a sector is detected only for objects whose sector was invalidated (spatial_updatesector() ignores the
    //    result otherwise), with the part's own collider;
    //  - lights and the tracked object are collected and registered after the parts (merge, list order).
    const auto process_item = [this, collect_lights, ltrack_index](
                                  u32 o_it, xrXRC& xrc, main_build_part& part)
    {
        ISpatial* spatial = lstRenderables[o_it];
        if (!spatial)
            return;
        if (spatial->GetSpatialData().type.load(std::memory_order_relaxed) & STYPEFLAG_INVALIDSECTOR)
        {
            PERF_SCOPE(rmain_build_sector);
            PERF_INC(rmain_sector_detect);
            spatial->spatial_updatesector(detect_sector(xrc, spatial->spatial_sector_point()));
        }
        const auto& data = spatial->GetSpatialData();
        const u32 type = data.type.load(std::memory_order_relaxed);
        const auto sector_id = data.sector_id.load(std::memory_order_relaxed);
        if (sector_id == IRender_Sector::INVALID_SECTOR_ID || sector_id >= Sectors.size())
        {
            PERF_INC(render_sector_cull);
            return; // disassociated from S/P structure
        }
        CSector* sector = Sectors[sector_id];

        if (collect_lights && (type & STYPE_LIGHTSOURCE))
        {
            light* L = (light*)spatial->dcast_Light();
            if (L && L->get_LOD() > EPS_L && RImplementation.HOM.visible(L->get_homdata()))
                part.lights.push_back(L);
            return;
        }

        if (PortalTraverser.i_marker != sector->r_marker)
        {
            PERF_INC(render_sector_cull);
            return; // inactive (untouched) sector
        }
        for (const CFrustum& view : sector->r_frustums)
        {
            if (!view.testSphere_dirty(data.sphere.P, data.sphere.R))
            {
                PERF_INC(render_sphere_miss);
                continue;
            }
            if (type & STYPE_RENDERABLE)
            {
                IRenderable* renderable = spatial->dcast_Renderable();
                VERIFY(renderable);

                // Occlusion (the visual and its vis_data belong to this object only)
                vis_data& v_orig = ((dxRender_Visual*)renderable->GetRenderData().visual)->vis;
                vis_data v_copy = v_orig;
                v_copy.box.xform(renderable->GetRenderData().xform);
                BOOL visible;
                {
                    PERF_SCOPE(rmain_build_hom);
                    visible = RImplementation.HOM.visible(v_copy);
                }
                memcpy(v_orig.marker, v_copy.marker, sizeof(v_copy.marker));
                v_orig.accept_frame = v_copy.accept_frame;
                v_orig.hom_frame = v_copy.hom_frame;
                v_orig.hom_tested = v_copy.hom_tested;
                if (!visible)
                {
                    PERF_INC(render_hom_cull);
                    return;
                }

                if (o_it == ltrack_index)
                    part.ltrack = renderable;

                PERF_INC(renderable_render);
                PERF_SCOPE(rmain_build_renderable);
                renderable->renderable_Render(context_id, renderable);
            }
            return; // exit loop on frustums
        }
    };

    struct shared_state
    {
        std::atomic<u32> next{ 0 };
        std::atomic<u32> done{ 0 };
    } state;
    const u8 perf_pass = xray::perf::TlPass();

    const auto run_parts = [&]()
    {
        PERF_PASS(perf_pass); // CGameObject::renderable_Render and the crow/bones metrics read the pass tag
        bool ran_any = false;
        for (;;)
        {
            const u32 p = state.next.fetch_add(1, std::memory_order_relaxed);
            if (p >= parts)
                break;
            {
                ZoneScopedN("dsg_dynamic_part");
                PERF_SCOPE(rmain_par_part);
                R_dsgraph_structure& shard = *s_main_build_shards[p];
                main_build_part& part = s_main_build_parts[p];
                const u32 begin = static_cast<u32>(u64(count) * p / parts);
                const u32 end = static_cast<u32>(u64(count) * (p + 1) / parts);
                const shard_binding prev = t_shard_binding;
                t_shard_binding = { this, &shard };
                for (u32 o_it = begin; o_it < end; ++o_it)
                    process_item(o_it, shard.Sectors_xrc, part);
                t_shard_binding = prev;
                if (!ran_any)
                    part.ran_here = true; // one flag per thread: counts the threads, not the parts
                ran_any = true;
            }
            state.done.fetch_add(1, std::memory_order_release);
        }
    };

    {
        PERF_SCOPE(rmain_par_wall);
        // Helpers on other workers; this thread claims parts too, so it never idles while parts are left.
        Task& root = TaskScheduler->CreateTask([] {});
        const u32 helpers = std::min(parts - 1, workers - 1);
        for (u32 h = 0; h < helpers; ++h)
            TaskScheduler->AddTask(root, [&run_parts] { run_parts(); });
        run_parts();
        {
            // Spin (no stealing) until the parts taken by others are done: TaskManager::Wait() could pick up an
            // unrelated long task (a sun cascade) here and delay the main thread waiting for this build.
            PERF_SCOPE(rmain_par_tail);
            while (state.done.load(std::memory_order_acquire) < parts)
                std::this_thread::yield();
        }
        // Helpers still queued find no parts and return at once; Wait() runs them first from this thread's queue.
        TaskManager::RunTask(root);
        TaskScheduler->Wait(root);
    }

    {
        PERF_SCOPE(rmain_par_merge);
        u32 threads = 0;
        for (u32 p = 0; p < parts; ++p)
        {
            R_dsgraph_structure& shard = *s_main_build_shards[p];
            main_build_part& part = s_main_build_parts[p];
            merge_dynamic(shard);
            shard.clear_dynamic(); // drop the pointers now, keep the memory
            for (light* L : part.lights)
                RImplementation.Lights.add_light(L);
            if (part.ltrack)
            {
                if (auto* ros = (CROS_impl*)part.ltrack->renderable_ROS())
                {
                    PERF_SCOPE(rmain_build_ros);
                    ros->update(part.ltrack);
                }
            }
            threads += part.ran_here ? 1 : 0;
        }
        PERF_ADD(rmain_par_threads, threads);
    }
    return true;
}

// sub-space rendering - main procedure
void R_dsgraph_structure::build_subspace()
{
    u8 pass = xray::perf::TlPass();
    if (!pass)
    {
        if (o.second_vp_pass)
            pass = xray::perf::pass::Svp;
        else if (o.is_main_pass)
            pass = xray::perf::pass::Main;
        else
            pass = xray::perf::pass::SmapOther;
    }
    PERF_PASS(pass);
    PERF_SCOPE(dsg_build_subspace);
    // render/main/* metrics: only the main view's build (not cascades, lights or the scope pass).
    [[maybe_unused]] const bool perf_main = o.is_main_pass && !o.second_vp_pass;
    // Tracy zone is thread-safe; this runs on task workers for the 3 parallel sun cascades plus the
    // main pass (4 calls/frame). Coarse zone -> negligible overhead, reveals culling/render-list cost.
    // Main pass shows on the primary thread; cascades on task workers -> split is visible per-thread.
    ZoneScopedN("dsg_build_subspace");
    marker++; // !!! critical here

    if (o.precise_portals && RImplementation.rmPortals)
    {
        // Check if camera is too near to some portal - if so force DualRender
        Fvector box_radius;
        box_radius.set(o.query_box_side, o.query_box_side, o.query_box_side);
        Sectors_xrc.box_query(CDB::OPT_FULL_TEST, RImplementation.rmPortals, o.view_pos, box_radius);
        for (size_t K = 0; K < Sectors_xrc.r_count(); K++)
        {
            CPortal* pPortal = Portals[RImplementation.rmPortals->get_tris()[Sectors_xrc.r_begin()[K].id].dummy];
            pPortal->bDualRender = TRUE;
        }
    }

    if (o.is_main_pass && !o.second_vp_pass && (o.sector_id == IRender_Sector::INVALID_SECTOR_ID))
    {
        if (g_pGameLevel)
            g_pGameLevel->pHUD->Render_Last(context_id);
        return;
    }

    // Traverse sector/portal structure (bounds-checked: OOB would be UB; keep traverser epoch if we skip)
    CSector* const start_sector = (o.sector_id < Sectors.size()) ? Sectors[o.sector_id] : nullptr;
    if (start_sector)
    {
        ZoneScopedN("dsg_portal_traverse");
        PortalTraverser.traverse(start_sector, o.view_frustum, o.view_pos, o.xform, o.portal_traverse_flags);
    }
    else
    {
        PortalTraverser.i_marker++;
        PortalTraverser.r_sectors.clear();
    }

    // Determine visibility for static geometry hierarchy
#if 0
    static xr_vector<Task*> static_geo_tasks;
    static_geo_tasks.resize(PortalTraverser.r_sectors.size());
#endif

    if (psDeviceFlags.test(rsDrawStatic))
    {
        ZoneScopedN("dsg_static");
        PERF_SCOPE_IF(perf_main, rmain_build_static);
        for (u32 s_it = 0; s_it < PortalTraverser.r_sectors.size(); s_it++)
        {
            CSector* sector = PortalTraverser.r_sectors[s_it];
            dxRender_Visual* root = sector->root();
            //VERIFY(root->getType() == MT_HIERRARHY);

            // Sector-level shadow hull reject: skip the entire sector (and its full
            // add_static recursion over every visual) when its bounding box cannot cast
            // a shadow into the camera frustum. This is the big win for build_subspace
            // cost: one box test instead of recursing hundreds of visuals.
            if (o.use_shadow_hull_cull && shadow_hull_cull(root->vis.box))
                continue;

            const auto &children = static_cast<FHierrarhyVisual*>(root)->children;

            for (u32 v_it = 0; v_it < sector->r_frustums.size(); v_it++)
            {
#if 0
                const auto traverse_children = [&, this](const TaskRange<size_t>& range)
                {
                    for (size_t id = range.cbegin(); id != range.cend(); ++id)
                    {
                        const auto& view = sector->r_frustums[v_it];
                        add_static(children[id], view, view.getMask());
                    }
                };

                if (o.mt_calculate) // NOTE: this code doesn't work until visuals maps are separated by worker ID.
                {
                    static_geo_tasks[s_it] = &xr_parallel_for(TaskRange<size_t>(0, children.size()), false, traverse_children);
                }
                else
                {
                    traverse_children(TaskRange<size_t>(0, children.size()));
                }
#else
                const auto& view = sector->r_frustums[v_it];
                add_static(root, view, view.getMask());
#endif
            }
        }
    }

    const bool collect_dynamic_any = (o.spatial_types != 0) && psDeviceFlags.test(rsDrawDynamic);

    if (collect_dynamic_any)
    {
        ZoneScopedN("dsg_dynamic");
        PERF_SCOPE_IF(perf_main, rmain_build_dynamic);
        // Traverse object database
        if (o.precomputed_dynamic)
        {
            // Shared pre-fetched list (one query for all light passes): copy pointers, the
            // sector/frustum filters below drop everything this pass does not need.
            lstRenderables.assign(o.precomputed_dynamic->begin(), o.precomputed_dynamic->end());
            PERF_INC(render_precomputed);
        }
        else
        {
            ZoneScopedN("dsg_q_frustum");
            PERF_SCOPE_IF(perf_main, rmain_build_query);
            SpatialCaller spatial_caller(SpatialQueryCaller::render);
            g_pGamePersistent->SpatialSpace.q_frustum(lstRenderables, o.spatial_traverse_flags, o.spatial_types, o.view_frustum);
        }
        PERF_ADD(render_in, lstRenderables.size());

        if (o.spatial_traverse_flags & ISpatial_DB::O_ORDERED) // this should be inside of query functions
        {
            ZoneScopedN("dsg_sort");
            PERF_SCOPE_IF(perf_main, rmain_build_sort);
            // Exact sorting order (front-to-back). Precalculate distances: repeating GetSpatialData in the
            // comparator breaks strict weak ordering with NaNs / equality; MSVC std::sort can crash (AV in sort internals).
            struct SpatialDist
            {
                ISpatial* s{};
                float d{};
            };
            xr_vector<SpatialDist> ordered;
            ordered.reserve(lstRenderables.size());
            for (ISpatial* spatial : lstRenderables)
            {
                if (!spatial)
                    continue;
                float dist = spatial->GetSpatialData().sphere.P.distance_to_sqr(o.view_pos);
                if (!std::isfinite(dist))
                    dist = FLT_MAX;
                ordered.push_back({ spatial, dist });
            }
            std::sort(ordered.begin(), ordered.end(),
                [](const SpatialDist& a, const SpatialDist& b)
                {
                    if (a.d != b.d)
                        return a.d < b.d;
                    return reinterpret_cast<uintptr_t>(a.s) < reinterpret_cast<uintptr_t>(b.s);
                });
            lstRenderables.resize(ordered.size());
            for (size_t i = 0; i < ordered.size(); ++i)
                lstRenderables[i] = ordered[i].s;
        }

        u32 uID_LTRACK = 0xffffffff;
        // Stage C: light-tracking mutates shared game-side state - the concurrent SVP build opts out.
        if (o.is_main_pass && !o.second_vp_pass) // temporary
        {
            if (o.phase == CRender::PHASE_NORMAL)
            {
                RImplementation.uLastLTRACK++;
                if (!lstRenderables.empty())
                    uID_LTRACK = RImplementation.uLastLTRACK % lstRenderables.size();

                // update light-vis for current entity / actor
                IGameObject* O = g_pGameLevel->CurrentViewEntity();
                if (O)
                {
                    CROS_impl* R = (CROS_impl*)O->ROS();
                    if (R)
                    {
                        PERF_SCOPE(rmain_build_ros); // main view only (this whole branch is)
                        R->update(O);
                    }
                }
            }
        }

        const bool collect_lights = o.spatial_types & STYPE_LIGHTSOURCE;

        // Parallel main build: the main view's loop below split over task workers (same result, see
        // build_dynamic_main_parallel); false - this frame runs the loop below (fallback: few objects, no workers...).
        const bool parallel_done = o.is_main_pass && !o.second_vp_pass &&
            o.phase == CRender::PHASE_NORMAL && !o.precomputed_dynamic &&
            build_dynamic_main_parallel(uID_LTRACK, collect_lights);

        // Determine visibility for dynamic part of scene
        for (u32 o_it = 0; !parallel_done && o_it < lstRenderables.size(); o_it++)
        {
            ISpatial* spatial = lstRenderables[o_it];
            if (!spatial)
                continue;
            // Stage C: sector refresh writes shared spatial nodes - the concurrent SVP build relies
            // on the sectors the main build has already updated this frame.
            if (o.is_main_pass && !o.second_vp_pass)
            {
                PERF_SCOPE(rmain_build_sector); // main view only (this whole branch is)
                PERF_INC(rmain_sector_detect);
                const auto& entity_pos = spatial->spatial_sector_point();
                const auto sector_id = detect_sector(entity_pos);
                spatial->spatial_updatesector(sector_id);
            }
            const auto& data = spatial->GetSpatialData();
            // data.type / data.sector_id are std::atomic - snapshot via .load(), not by copying the
            // atomic objects themselves into the tuple (std::atomic has no copy constructor).
            const auto& [type, sphere, sector_id] =
                std::tuple(data.type.load(std::memory_order_relaxed), data.sphere,
                    data.sector_id.load(std::memory_order_relaxed));
            if (sector_id == IRender_Sector::INVALID_SECTOR_ID)
            {
                PERF_INC(render_sector_cull);
                continue; // disassociated from S/P structure
            }
            if (sector_id >= Sectors.size())
            {
                PERF_INC(render_sector_cull);
                continue;
            }
            auto* sector = Sectors[sector_id];

            if (collect_lights && (type & STYPE_LIGHTSOURCE))
            {
                light* L = (light*)spatial->dcast_Light();
                if (!L)
                    continue;
                float lod = L->get_LOD();
                if (lod > EPS_L)
                {
                    // TODO: check for HOM flag
                    vis_data& vis = L->get_homdata();
                    if (RImplementation.HOM.visible(vis))
                        RImplementation.Lights.add_light(L);
                }
                continue;
            }

            if (PortalTraverser.i_marker != sector->r_marker)
            {
                PERF_INC(render_sector_cull);
                continue; // inactive (untouched) sector
            }
            for (u32 v_it = 0; v_it < sector->r_frustums.size(); v_it++)
            {
                const CFrustum& view = sector->r_frustums[v_it];
                if (!view.testSphere_dirty(spatial->GetSpatialData().sphere.P, spatial->GetSpatialData().sphere.R))
                {
                    PERF_INC(render_sphere_miss);
                    continue;
                }

            // Stage C: the main-pass branch mutates shared state (HOM vis writeback into shared
            // visuals, CROS updates) - the concurrent SVP build takes the plain insert path.
            if (o.is_main_pass && !o.second_vp_pass)
            {
                if (type & STYPE_RENDERABLE)
                {
                        // renderable
                        IRenderable* renderable = spatial->dcast_Renderable();
                        VERIFY(renderable);

                        // Occlusion
                        //	casting is faster then using getVis method
                        vis_data& v_orig = ((dxRender_Visual*)renderable->GetRenderData().visual)->vis;
                        vis_data v_copy = v_orig;
                        v_copy.box.xform(renderable->GetRenderData().xform);
                        BOOL bVisible;
                        {
                            PERF_SCOPE(rmain_build_hom); // main view only (this whole branch is)
                            bVisible = RImplementation.HOM.visible(v_copy);
                        }
                        memcpy(v_orig.marker, v_copy.marker, sizeof(v_copy.marker));
                        v_orig.accept_frame = v_copy.accept_frame;
                        v_orig.hom_frame = v_copy.hom_frame;
                        v_orig.hom_tested = v_copy.hom_tested;
                        if (!bVisible)
                        {
                            PERF_INC(render_hom_cull);
                            break; // exit loop on frustums
                        }

                        // update light-vis for selected entity
                        if (o_it == uID_LTRACK && renderable->renderable_ROS())
                        {
                            // track lighting environment
                            CROS_impl* T = (CROS_impl*)renderable->renderable_ROS();
                            PERF_SCOPE(rmain_build_ros);
                            T->update(renderable);
                        }

                        // Rendering
                        PERF_INC(renderable_render);
                        {
                            PERF_SCOPE(rmain_build_renderable); // bones, attachments, insert_dynamic
                            renderable->renderable_Render(context_id, renderable);
                        }
                    }
                    break; // exit loop on frustums
                }
                else
                {
                    // renderable
                    IRenderable* renderable = spatial->dcast_Renderable();
                    if (nullptr == renderable)
                        continue; // unknown, but renderable object (r1_glow???)

                    // Per-light owner exclusion: skip the light's owning renderable from its own
                    // shadow-map caster build to prevent self-shadowing (e.g. an NPC's torch
                    // shadow-mapping the NPC's own skeleton, making the owner dark in the lens).
                    if (o.phase == CRender::PHASE_SMAP && o.shadow_owner && renderable == o.shadow_owner)
                    {
                        PERF_INC(render_shadow_owner_skip);
                        continue;
                    }

                    // DARK-DYNAMICS ROOT-CAUSE FIX (bug introduced by commit 197d536a): this branch
                    // serves both the SMAP builds and the SVP scope build. The scope build passed
                    // root=nullptr (SMAP semantics), which dropped pObject from every matrix item -
                    // apply_object() then early-returned and never refreshed o_hemi/o_sun/
                    // o_hemi_cube on the recording backend, so ALL lens dynamics were recorded
                    // with a ZERO hemisphere (deffer_model*.vs derives hemi_val purely from the
                    // hemi_cube_* constants -> G-buffer hemi bits = 0 -> combine hmodel ambient =
                    // black; local spot/point lights still work - exactly the observed bug).
                    // Pass the root like the sequential path does: renderable_Render only READS it
                    // here (LOD flags, renderable_Invisible/HUD routing; MakeMeCrow is already
                    // guarded against the worker thread), while the shared-state mutations stay
                    // gated above (HOM writeback, CROS update, sector refresh, wallmarks, HUD).
                    PERF_INC(renderable_render);
                    renderable->renderable_Render(context_id,
                        o.second_vp_pass ? renderable : nullptr);
                }
            }
        }

        if (g_pGameLevel)
        {
#if RENDER != R_R1
            // Actor Shadow (Sun + Light)
            if (o.phase == CRender::PHASE_SMAP && ps_r__common_flags.test(RFLAG_ACTOR_SHADOW))
            {
                do
                {
                    IGameObject* viewEntity = g_pGameLevel->CurrentViewEntity();
                    if (viewEntity == nullptr)
                        break;
                    const auto& entity_pos = viewEntity->spatial_sector_point();
                    viewEntity->spatial_updatesector(detect_sector(entity_pos));
                    const auto sector_id = viewEntity->GetSpatialData().sector_id.load(std::memory_order_relaxed);
                    if (sector_id == IRender_Sector::INVALID_SECTOR_ID)
                        break; // disassociated from S/P structure
                    if (sector_id >= Sectors.size())
                        break;
                    CSector* sector = Sectors[sector_id];
                    if (PortalTraverser.i_marker != sector->r_marker)
                        break; // inactive (untouched) sector
                    for (const CFrustum& view : sector->r_frustums)
                    {
                        if (!view.testSphere_dirty(
                            viewEntity->GetSpatialData().sphere.P, viewEntity->GetSpatialData().sphere.R))
                            continue;

                        // renderable
                        g_pGameLevel->pHUD->Render_First(context_id);
                    }
                } while (0);
            }
#endif

        // Stage C: HUD queue is game-side shared state - only the main pass submits to it here.
        if (o.is_main_pass && !o.second_vp_pass)
        {
            PERF_SCOPE(rmain_build_hud);
            g_pGameLevel->pHUD->Render_Last(context_id);
        }
        }
    }

#if 0
    // wait for static geo collecting to be done.
    for (auto* task : static_geo_tasks)
    {
        if (task)
            TaskScheduler->Wait(*task);
    }
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
