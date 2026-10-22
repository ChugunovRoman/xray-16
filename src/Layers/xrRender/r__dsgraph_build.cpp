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
#include "ParticleEffect.h"
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
extern float r_dtex_range;

// R_dsgraph_structure::options_t::lod_own (31-svp-scope.md): the thresholds of the pass being built - its own snapshot
// (the scope pass built on the SVP worker while the globals still hold the main view's ones), otherwise the globals.
ICF float pass_ssaDISCARD(const R_dsgraph_structure::options_t& o) { return o.lod_own ? o.lod.ssaDISCARD : r_ssaDISCARD; }
ICF float pass_ssaLOD_A(const R_dsgraph_structure::options_t& o) { return o.lod_own ? o.lod.ssaLOD_A : r_ssaLOD_A; }
ICF float pass_ssaLOD_B(const R_dsgraph_structure::options_t& o) { return o.lod_own ? o.lod.ssaLOD_B : r_ssaLOD_B; }
ICF float pass_ssaLOD_CHAR_A(const R_dsgraph_structure::options_t& o)
{
    return o.lod_own ? o.lod.ssaLOD_CHAR_A : r_ssaLOD_CHAR_A;
}
ICF float pass_dtex_range(const R_dsgraph_structure::options_t& o) { return o.lod_own ? o.lod.dtex_range : r_dtex_range; }

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

#if XR_PERF_METRICS
// 27-render-main.md (M): render/main/ps_small_{4,8,16}px - non-empty particle effects of the main view whose bounding
// sphere is smaller on screen than that many pixels (diameter; cumulative: a 3 px effect counts in all three).
static void perf_ps_screen_size(const dxRender_Visual* V, const Fvector& center)
{
    const float dist = _sqrt(Device.vCameraPosition.distance_to_sqr(center)) + EPS_L;
    const float tan_half = std::tan(deg2rad(Device.fFOV) * 0.5f);
    const float px = V->vis.sphere.R * float(Device.dwHeight) / (dist * tan_half);
    if (px < 16.f)
    {
        PERF_INC(rmain_ps_small_16);
        if (px < 8.f)
        {
            PERF_INC(rmain_ps_small_8);
            if (px < 4.f)
                PERF_INC(rmain_ps_small_4);
        }
    }
}

// 27-render-main.md (M): render/main/skel_dist/* - skeletal models of the main view by camera distance, and
// render/main/skel_parts/* - their meshes (draws per pass): the children drawn with the full mesh, 1 for the LOD mesh.
static void perf_skel_distance(const dxRender_Visual* V, const Fmatrix& xform, u32 parts)
{
    Fvector center;
    xform.transform_tiny(center, V->vis.sphere.P);
    const float dist = _sqrt(Device.vCameraPosition.distance_to_sqr(center));
    if (dist < 10.f)
    {
        PERF_INC(rmain_skel_dist_10);
        PERF_ADD(rmain_skel_parts_10, parts);
    }
    else if (dist < 25.f)
    {
        PERF_INC(rmain_skel_dist_25);
        PERF_ADD(rmain_skel_parts_25, parts);
    }
    else if (dist < 50.f)
    {
        PERF_INC(rmain_skel_dist_50);
        PERF_ADD(rmain_skel_parts_50, parts);
    }
    else if (dist < 100.f)
    {
        PERF_INC(rmain_skel_dist_100);
        PERF_ADD(rmain_skel_parts_100, parts);
    }
    else
    {
        PERF_INC(rmain_skel_dist_far);
        PERF_ADD(rmain_skel_parts_far, parts);
    }
}
#endif

// 50v-rain-context-race.md: a build whose dsgraph was reset under it (cleanup_contexts at End while its task still ran)
// reads context_id == INVALID_CONTEXT_ID - vis.marker[context_id] is then 16 GB past the visual (the AV in
// insert_static). The root cause is fixed (every phase task is joined before the pool reset, CRender::End); this keeps
// a regression a log line: the insert is skipped, the lists of such a build are dropped anyway.
static void report_invalid_context(const R_dsgraph_structure& ds, pcstr where)
{
    static std::atomic<bool> s_reported{ false };
    if (s_reported.load(std::memory_order_relaxed) || s_reported.exchange(true, std::memory_order_relaxed))
        return;
    Msg("! [render] %s: dsgraph context_id 0x%x is invalid (phase %u, frame %u) - its build was reset under it, inserts "
        "skipped", where, ds.context_id, ds.o.phase, Device.dwFrame);
}

void R_dsgraph_structure::insert_dynamic(IRenderable* root, dxRender_Visual* pVisual, Fmatrix& xform, Fvector& Center)
{
    // No ZoneScoped: called from build_subspace() on task workers (sun SMAP mt_calc); Tracy rpmalloc/queue can AV.
    CRender& RI = RImplementation;

    const u32 ctx = context_id; // read once: the guard and the marker index must agree
    if (ctx >= R__NUM_CONTEXTS)
    {
        report_invalid_context(*this, "insert_dynamic");
        return;
    }
    if (pVisual->vis.marker[ctx] == marker)
        return;
    pVisual->vis.marker[ctx] = marker;

#if RENDER == R_R1
    if (RI.o.vis_intersect && (pVisual->vis.accept_frame != Device.dwFrame))
        return;
    pVisual->vis.accept_frame = Device.dwFrame;
#endif

    float distSQ;
    float SSA = CalcSSA(distSQ, Center, pVisual);
    if (SSA <= pass_ssaDISCARD(o))
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
    ShaderElement* sh = RImplementation.rimp_select_sh_dynamic(pVisual, distSQ, o.phase, pass_dtex_range(o));
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
    const u32 ctx = context_id; // read once, see report_invalid_context
    if (ctx >= R__NUM_CONTEXTS)
    {
        report_invalid_context(*this, "insert_static");
        return;
    }
    if (pVisual->vis.marker[ctx] == marker)
        return;
    pVisual->vis.marker[ctx] = marker;

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
    if (SSA <= pass_ssaDISCARD(o))
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
    ShaderElement* sh = RImplementation.rimp_select_sh_static(pVisual, distSQ, o.phase, pass_dtex_range(o));
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
        // OnFrame (PreRender particle worker) and Play/Stop (game logic thread) reallocate/erase the
        // child vectors under render_lock while this view build runs - walk them under the same lock.
        // Children are CParticleEffect leaves (no nested group lock) and insert_dynamic takes no locks,
        // so the hold is a few pointer pushes; render threads only contend with each other that briefly.
        ScopeLock lock{ &pG->GetRenderLock() };
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
    case MT_PARTICLE_EFFECT:
    {
        // plans/optimization_mt/18-combine-pass.md (E3): an effect with nothing to draw (group children not started
        // yet or finished, emitters between bursts - up to half of mapSorted in a heavy fight) used to cost a node
        // in mapSorted/mapDistort/... and, in render_item, set_Element + xform + ROS + lmaterial for a Render()
        // that draws nothing. Lock-free read: the PreRender particle worker may be stepping this effect right now,
        // exactly as when Render() reads the count later; an effect whose first particles are born after this
        // build shows up from the next frame (as an effect created after it already does).
        if (static_cast<PS::CParticleEffect*>(pVisual)->IsEmptyForRender())
        {
            PERF_ADD_IF(o.is_main_pass && !o.second_vp_pass, rmain_ps_empty_skip, 1);
            return;
        }
        Fvector Tpos;
        xform.transform_tiny(Tpos, pVisual->vis.sphere.P);
#if XR_PERF_METRICS
        if (o.is_main_pass && !o.second_vp_pass && o.phase == CRender::PHASE_NORMAL && xray::perf::g_perf_metrics_active)
            perf_ps_screen_size(pVisual, Tpos);
#endif
        insert_dynamic(root, pVisual, xform, Tpos);
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
                    (root && root->renderable_SsaLodCharacter()) ? pass_ssaLOD_CHAR_A(o) : pass_ssaLOD_A(o);
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
                    (root && root->renderable_SsaLodCharacter()) ? pass_ssaLOD_CHAR_A(o) : pass_ssaLOD_A(o);
                if (ssa < ssaLodA)
                    _use_lod = TRUE;
            }
        }
#if XR_PERF_METRICS
        // A model of its own (not the LOD mesh followed below) of the main view, not the HUD.
        if (!lod_bind_source && o.is_main_pass && !o.second_vp_pass && o.phase == CRender::PHASE_NORMAL &&
            xray::perf::g_perf_metrics_active && !(root && root->renderable_HUD()))
        {
            perf_skel_distance(pV, xform, _use_lod ? 1 : static_cast<u32>(pV->children.size()));
        }
#endif
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
            PERF_ADD_IF(o.second_vp_pass, rsvp_skel_lod, 1); // the scope build on the SVP worker
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
        if (ssa < pass_ssaLOD_A(o))
        {
            if (ssa < pass_ssaDISCARD(o))
                return;
            mapLOD.insert_anyway(D, _LodItem({ ssa, pVisual }));
        }
#if RENDER != R_R1
        if (ssa > pass_ssaLOD_B(o) || o.phase == CRender::PHASE_SMAP)
#else
        if (ssa > pass_ssaLOD_B(o))
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
        if (ssa < pass_ssaLOD_A(o))
        {
            if (ssa < pass_ssaDISCARD(o))
                return;
            mapLOD.insert_anyway(D, _LodItem({ ssa, pVisual }));
        }
#if RENDER != R_R1
        if (ssa > pass_ssaLOD_B(o) || o.phase == CRender::PHASE_SMAP)
#else
        if (ssa > pass_ssaLOD_B(o))
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

// Runs job(i) for every i in [0, count) on this thread and up to `helpers` task workers, which claim the jobs in index
// order. own_work runs on this thread first, while the helpers start on the jobs. Then this thread spins (no stealing,
// as run_parts below) until the jobs taken by others are done.
template <typename Job, typename OwnWork>
void run_jobs_parallel(u32 count, u32 helpers, const Job& job, const OwnWork& own_work)
{
    struct shared_state
    {
        std::atomic<u32> next{ 0 };
        std::atomic<u32> done{ 0 };
    } state;
    const u8 perf_pass = xray::perf::TlPass();
    const auto run = [&]()
    {
        PERF_PASS(perf_pass);
        for (;;)
        {
            const u32 i = state.next.fetch_add(1, std::memory_order_relaxed);
            if (i >= count)
                break;
            job(i);
            state.done.fetch_add(1, std::memory_order_release);
        }
    };

    Task& root = TaskScheduler->CreateTask([] {});
    for (u32 h = 0; h < helpers; ++h)
        TaskScheduler->AddTask(root, [&run] { run(); });
    own_work();
    run();
    {
        PERF_SCOPE(rmain_par_jobs_tail);
        while (state.done.load(std::memory_order_acquire) < count)
            std::this_thread::yield();
    }
    // Helpers still queued find no jobs and return at once; Wait() runs them first from this thread's queue.
    TaskManager::RunTask(root);
    TaskScheduler->Wait(root);
}

// R10 (27-render-main.md): the shards are appended to the main dsgraph by jobs, one per
// destination list - the four matrix maps and the seven sorted/HUD maps are independent of each other. Every job replays
// the shards in part order exactly as merge_dynamic() does for its list, so each list ends up the same as with the
// sequential merge: the node array and the tree of every map (sorted maps: the same insert_anyway sequence, so equal
// keys keep their order), the item vectors and their ssa.
constexpr mapSorted_T R_dsgraph_structure::*MERGE_SORTED_MAPS[] = {
    &R_dsgraph_structure::mapSorted,
    &R_dsgraph_structure::mapHUD,
    &R_dsgraph_structure::mapDistort,
    &R_dsgraph_structure::mapHUDSorted,
#if RENDER != R_R1
    &R_dsgraph_structure::mapWmark,
    &R_dsgraph_structure::mapEmissive,
    &R_dsgraph_structure::mapHUDEmissive,
#endif
};
constexpr u32 MERGE_MATRIX_JOBS = 2 * SHADER_PASSES_MAX;
constexpr u32 MERGE_SORTED_JOBS = static_cast<u32>(std::size(MERGE_SORTED_MAPS));

// Scratch of one destination matrix map, reused between frames (the main-view build only, one at a time).
struct matrix_merge_scratch
{
    static constexpr u32 NO_NODE = u32(-1);
    xr_vector<std::pair<SPass*, u32>> table; // open addressing: key -> destination node index (nullptr - a free slot)
    xr_vector<mapMatrix_T::value_type*> from; // the shards' nodes in merge order (part order, then node order)
    xr_vector<u32> from_next; // the next shard node with the same destination node, NO_NODE - the last one
    xr_vector<u32> dest_first, dest_last; // per destination node: its first / last shard node, NO_NODE - none
};
matrix_merge_scratch s_merge_matrix[2][SHADER_PASSES_MAX];

// One matrix map of merge_dynamic() over all shards. The sequential merge calls to[key] for every shard node in part
// order - the node of a key is created at its first appearance - and appends that node's items: here the same to.insert()
// calls in the same order create the same nodes (the lookup of the keys already met goes through a hash table instead of
// the tree), then every destination node takes the items of its shard nodes in part order and the largest ssa. Empties
// the shards' map as clear_dynamic() does.
void merge_matrix_map(mapMatrix_T& to, u32 priority, u32 pass_id, u32 parts, matrix_merge_scratch& s)
{
    PERF_SCOPE(rmain_par_merge_matrix);
    constexpr u32 NO_NODE = matrix_merge_scratch::NO_NODE;
    s.from.clear();
    for (u32 p = 0; p < parts; ++p)
    {
        mapMatrix_T& from = s_main_build_shards[p]->mapMatrixPasses[priority][pass_id];
        for (auto* node = from.begin(); node != from.end(); ++node)
            s.from.push_back(node);
    }
    if (s.from.empty())
        return;

    const size_t keys_max = to.size() + s.from.size();
    size_t capacity = 64;
    while (capacity < keys_max * 2)
        capacity <<= 1;
    s.table.assign(capacity, { nullptr, 0 });
    const size_t mask = capacity - 1;
    u32 null_key_dest = NO_NODE; // a nullptr key (insert_dynamic never adds one) cannot live in the table
    // The destination index of key; a new key gets a slot holding NO_NODE.
    const auto lookup = [&](SPass* key) -> u32&
    {
        if (!key)
            return null_key_dest;
        size_t i = static_cast<size_t>((u64(reinterpret_cast<uintptr_t>(key)) * 0x9E3779B97F4A7C15ull) >> 32) & mask;
        for (;;)
        {
            auto& slot = s.table[i];
            if (slot.first == key)
                return slot.second;
            if (!slot.first)
            {
                slot.first = key;
                slot.second = NO_NODE;
                return slot.second;
            }
            i = (i + 1) & mask;
        }
    };
    for (size_t d = 0; d < to.size(); ++d)
        lookup(to.at_index(d).first) = static_cast<u32>(d);

    const u32 from_count = static_cast<u32>(s.from.size());
    s.dest_first.assign(keys_max, NO_NODE);
    s.dest_last.assign(keys_max, NO_NODE);
    s.from_next.assign(from_count, NO_NODE);
    for (u32 i = 0; i < from_count; ++i)
    {
        u32& dest = lookup(s.from[i]->first);
        if (dest == NO_NODE)
        {
            // A key not met yet: to[key] of the sequential merge creates its node here, at the same point of the order.
            const auto* node = to.insert(s.from[i]->first);
            dest = static_cast<u32>(node - to.begin());
        }
        const u32 d = dest;
        if (s.dest_last[d] == NO_NODE)
            s.dest_first[d] = i;
        else
            s.from_next[s.dest_last[d]] = i;
        s.dest_last[d] = i;
    }

    for (size_t d = 0; d < to.size(); ++d)
    {
        u32 i = s.dest_first[d];
        if (i == NO_NODE)
            continue;
        mapMatrixItems& items = to.at_index(d).second;
        size_t added = 0;
        for (u32 j = i; j != NO_NODE; j = s.from_next[j])
            added += s.from[j]->second.size();
        items.reserve(items.size() + added);
        for (; i != NO_NODE; i = s.from_next[i])
        {
            const mapMatrixItems& src = s.from[i]->second;
            items.insert(items.end(), src.begin(), src.end());
            if (src.ssa > items.ssa)
                items.ssa = src.ssa;
        }
    }

    for (u32 p = 0; p < parts; ++p)
    {
        mapMatrix_T& from = s_main_build_shards[p]->mapMatrixPasses[priority][pass_id];
        for (auto* node = from.begin(); node != from.end(); ++node)
        {
            node->second.clear();
            node->second.ssa = 0.f;
        }
        from.clear();
    }
}

// One sorted/HUD map of merge_dynamic() over all shards: the same insert_anyway() calls in the same order.
void merge_sorted_map(R_dsgraph_structure& to, mapSorted_T R_dsgraph_structure::*map, u32 parts)
{
    PERF_SCOPE(rmain_par_merge_sorted);
    mapSorted_T& dest = to.*map;
    for (u32 p = 0; p < parts; ++p)
    {
        mapSorted_T& from = (*s_main_build_shards[p]).*map;
        for (auto* node = from.begin(); node != from.end(); ++node)
            dest.insert_anyway(node->first, node->second);
    }
}

bool main_build_par_merge_on([[maybe_unused]] u32 workers)
{
#if defined(USE_DX11)
    return workers >= 2 && !D3DXRenderBase::RenderDocActive(); // R10: always on since 2026-10-18 (41-cvar-cleanup.md)
#else
    return false; // GL: the sequential merge (as every parallel path of 27-render-main.md)
#endif
}

// R3 (27-render-main.md; always on since 2026-10-18, 41-cvar-cleanup.md): the main view's G-buffer build of this frame refreshes the CROS
// of its dynamic objects in the parts and sorts the priority-0 dynamic groups at its end. The same conditions as the
// parallel submit that would otherwise do it (D3D11, not RenderDoc) plus no Z-prefill (it draws and empties this graph
// before part0).
bool main_par_prepare_in_build_on([[maybe_unused]] const R_dsgraph_structure::options_t& o)
{
#if defined(USE_DX11)
    return o.is_main_pass && !o.second_vp_pass &&
        o.phase == CRender::PHASE_NORMAL && !ps_r2_ls_flags.test(R2FLAG_ZFILL) && !D3DXRenderBase::RenderDocActive();
#else
    return false;
#endif
}

// R3: the once-per-frame CROS refresh (update_smooth) that part0's prepare (render_graph_main_parallel) does for every
// object of the priority-0 dynamic groups, done for the objects of one part on the thread that built it. The object of
// an item is the root of the visual: the renderable of an lstRenderables entry this part processed (attachments and
// the active item pass their owner's root), and every entry is in one part only - no two parts refresh one CROS. The
// tracked object (ltrack) is left alone: its CROS_impl::update() comes in the merge, and the refresh must not come
// before it (the update ends with the refresh of the frame). part0's prepare still calls update_smooth for every item:
// a no-op for the objects done here, the refresh for the rest (ltrack, objects added after the parts).
void refresh_part_ros(R_dsgraph_structure& shard, const IRenderable* ltrack)
{
    PERF_SCOPE(rmain_par_prep_smooth);
    for (u32 pass_id = 0; pass_id < SHADER_PASSES_MAX; ++pass_id)
    {
        mapMatrix_T& map = shard.mapMatrixPasses[0][pass_id];
        for (auto* node = map.begin(); node != map.end(); ++node)
        {
            for (const _MatrixItem& item : node->second)
            {
                if (!item.pObject || item.pObject == ltrack)
                    continue;
                if (auto* ros = static_cast<CROS_impl*>(item.pObject->renderable_ROS()))
                    ros->update_smooth(item.pObject);
            }
        }
    }
}

// R3: the groups of presort_main_dynamic(), all passes in a row, and their cut into jobs (first group of every job).
xr_vector<mapMatrix_T::value_type*> s_presort_groups;
xr_vector<u32> s_presort_jobs;
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

void R_dsgraph_structure::clear_build_lists()
{
    for (auto& passes : mapNormalPasses)
    {
        for (auto& map : passes)
        {
            for (auto* node = map.begin(); node != map.end(); ++node)
            {
                node->second.clear();
                node->second.ssa = 0.f;
            }
            map.clear();
        }
    }
    mapLOD.clear();
    clear_dynamic();
}

namespace
{
// R13 (27-render-main.md): the decay window of a kept map, in resets of its structure (the
// immediate dsgraph: one per frame; a pooled one: alloc_context and the end of the frame), and how many times bigger than
// the high-water mark of the window a map may stay. 64 - the first array resize() allocates (SG_REALLOC_ADVANCE).
constexpr u32 DSG_RESERVE_DECAY_RESETS = 1024;
constexpr size_t DSG_RESERVE_DECAY_FACTOR = 4;
constexpr size_t DSG_RESERVE_MIN_NODES = 64;

// Cuts a map that has held far more nodes than it used in the window down to that high-water mark (rounded up to the
// sizes resize() makes: 64 * 2^k; nothing if unused) - the reserve for the next frames, allocated at once.
template <typename Map>
void dsg_reserve_decay(Map& map)
{
    const size_t used = map.take_peak();
    size_t want = 0;
    if (used)
    {
        want = DSG_RESERVE_MIN_NODES;
        while (want < used)
            want <<= 1;
    }
    if (map.allocated() > std::max(want, DSG_RESERVE_MIN_NODES) * DSG_RESERVE_DECAY_FACTOR)
    {
        map.shrink(want);
        PERF_INC(rdsg_shrink);
    }
}

// A matrix/normal map (item vectors as values) emptied into the state the old reset left - a freshly constructed node
// for every insert: EVERY allocated node, not only the ones in use (render_graph and the scope pass's copy into the
// immediate dsgraph, r2_R_render.cpp, clear() some maps without emptying their nodes), gets an empty item vector (its
// capacity kept) and ssa 0.
template <typename Map>
void dsg_reset_items_map(Map& map, bool decay)
{
    for (auto* node = map.first(); node != map.last(); ++node)
    {
        node->second.clear();
        node->second.ssa = 0.f;
    }
    map.clear();
    if (decay)
        dsg_reserve_decay(map);
}

// A sorted/HUD/LOD map: every insert into them (insert/insert_anyway with a value) assigns the whole value, the stale
// one of a reused node is never read.
template <typename Map>
void dsg_reset_plain_map(Map& map, bool decay)
{
    map.clear();
    if (decay)
        dsg_reserve_decay(map);
}
} // namespace

void R_dsgraph_structure::reset_maps(bool frame_reset)
{
    // R13 (27-render-main.md, always on since 2026-10-18, 41-cvar-cleanup.md): only the per-frame reset keeps the nodes
    if (!frame_reset)
    {
        for (int i = 0; i < SHADER_PASSES_MAX; ++i)
        {
            mapNormalPasses[0][i].destroy();
            mapNormalPasses[1][i].destroy();
            mapMatrixPasses[0][i].destroy();
            mapMatrixPasses[1][i].destroy();
        }
        mapSorted.destroy();
        mapHUD.destroy();
        mapLOD.destroy();
        mapDistort.destroy();
        mapHUDSorted.destroy();

#if RENDER != R_R1
        mapWmark.destroy();
        mapEmissive.destroy();
        mapHUDEmissive.destroy();
#endif
        reserve_resets = 0;
        return;
    }

    // R13: the next frame (or the next user of this pooled context) inserts into the same nodes - the same insertion
    // sequence gives the same node array and tree as with fresh maps, the lists and their order are the same.
    const bool decay = ++reserve_resets >= DSG_RESERVE_DECAY_RESETS;
    if (decay)
        reserve_resets = 0;
    for (int i = 0; i < SHADER_PASSES_MAX; ++i)
    {
        dsg_reset_items_map(mapNormalPasses[0][i], decay);
        dsg_reset_items_map(mapNormalPasses[1][i], decay);
        dsg_reset_items_map(mapMatrixPasses[0][i], decay);
        dsg_reset_items_map(mapMatrixPasses[1][i], decay);
    }
    dsg_reset_plain_map(mapSorted, decay);
    dsg_reset_plain_map(mapHUD, decay);
    dsg_reset_plain_map(mapLOD, decay);
    dsg_reset_plain_map(mapDistort, decay);
    dsg_reset_plain_map(mapHUDSorted, decay);
#if RENDER != R_R1
    dsg_reset_plain_map(mapWmark, decay);
    dsg_reset_plain_map(mapEmissive, decay);
    dsg_reset_plain_map(mapHUDEmissive, decay);
#endif
}

size_t R_dsgraph_structure::maps_capacity(size_t& max_map) const
{
    size_t total = 0;
    const auto add = [&](size_t nodes)
    {
        total += nodes;
        if (nodes > max_map)
            max_map = nodes;
    };
    for (int i = 0; i < SHADER_PASSES_MAX; ++i)
    {
        add(mapNormalPasses[0][i].allocated());
        add(mapNormalPasses[1][i].allocated());
        add(mapMatrixPasses[0][i].allocated());
        add(mapMatrixPasses[1][i].allocated());
    }
    add(mapSorted.allocated());
    add(mapHUD.allocated());
    add(mapLOD.allocated());
    add(mapDistort.allocated());
    add(mapHUDSorted.allocated());
#if RENDER != R_R1
    add(mapWmark.allocated());
    add(mapEmissive.allocated());
    add(mapHUDEmissive.allocated());
#endif
    return total;
}

void R_dsgraph_structure::merge_dynamic(R_dsgraph_structure& shard)
{
    // xr_fixed_map nodes are stored in insertion order: replaying them reproduces the shard's sequence.
    // render/main/par_merge_matrix, par_merge_sorted (27-render-main.md, M): the two halves of render/main/par_merge.
    {
        PERF_SCOPE(rmain_par_merge_matrix);
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
    }

    PERF_SCOPE(rmain_par_merge_sorted);
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
                // 46-engine-core-render-audit.md: CHOM::visible(vis_data&) reads hom_frame and the box and writes only
                // hom_frame/hom_tested; its first check (next test not due: Device.dwFrame < hom_frame, ~94% of calls)
                // answers "visible" writing nothing - done here before the vis_data copy and the box xform. marker and
                // accept_frame are not copied back: HOM never changes them (the copy back only wrote the same values,
                // or undid a marker a concurrent cascade build had just set).
                vis_data& v_orig = ((dxRender_Visual*)renderable->GetRenderData().visual)->vis;
                BOOL visible = TRUE;
                {
                    PERF_SCOPE(rmain_build_hom);
                    if (Device.dwFrame >= v_orig.hom_frame)
                    {
                        vis_data v_copy = v_orig;
                        v_copy.box.xform(renderable->GetRenderData().xform);
                        visible = RImplementation.HOM.visible(v_copy);
                        v_orig.hom_frame = v_copy.hom_frame;
                        v_orig.hom_tested = v_copy.hom_tested;
                    }
                }
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
    const bool prep_smooth = main_par_prepare_in_build_on(o); // R3 (27-render-main.md)

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
                if (prep_smooth)
                    refresh_part_ros(shard, part.ltrack);
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

    if (main_build_par_merge_on(workers))
    {
        // R10 (27-render-main.md): the lists merged by jobs (see MERGE_SORTED_MAPS); this
        // thread registers the lights and updates the tracked object meanwhile, in part order as the loop below.
        PERF_SCOPE(rmain_par_merge);
        PERF_INC(rmain_par_merge_par);
        u32 threads = 0;
        const auto job = [this, parts](u32 i)
        {
            // 0 - matrix [0][0], 1 - mapSorted (the two big ones first), then the other matrix maps, the other sorted.
            u32 matrix = u32(-1);
            u32 sorted = u32(-1);
            if (i == 0)
                matrix = 0;
            else if (i == 1)
                sorted = 0;
            else if (i <= MERGE_MATRIX_JOBS)
                matrix = i - 1;
            else
                sorted = i - MERGE_MATRIX_JOBS;
            if (matrix != u32(-1))
            {
                const u32 priority = matrix / SHADER_PASSES_MAX;
                const u32 pass_id = matrix % SHADER_PASSES_MAX;
                merge_matrix_map(mapMatrixPasses[priority][pass_id], priority, pass_id, parts,
                    s_merge_matrix[priority][pass_id]);
            }
            else
                merge_sorted_map(*this, MERGE_SORTED_MAPS[sorted], parts);
        };
        const auto register_parts = [parts, &threads]
        {
            for (u32 p = 0; p < parts; ++p)
            {
                main_build_part& part = s_main_build_parts[p];
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
        };
        constexpr u32 jobs = MERGE_MATRIX_JOBS + MERGE_SORTED_JOBS;
        run_jobs_parallel(jobs, std::min(jobs - 1, workers - 1), job, register_parts);
        // The matrix maps are empty already; the rest of the shards' lists (pointers only, memory kept).
        for (u32 p = 0; p < parts; ++p)
            s_main_build_shards[p]->clear_dynamic();
        PERF_ADD(rmain_par_threads, threads);
    }
    else
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

// R3 (27-render-main.md): what render_graph(0) / render_graph_main_parallel() do first with
// mapMatrixPasses[0] - get_any_p, the groups by ssa (cmp_pass), the items of every group by ssa (cmp_ssa) - with the same
// comparisons and the same std::sort, so the same order; the items of the groups sorted in jobs on the build's helpers.
// The groups' ssa is left for the consumer to clear (take_main_presorted).
void R_dsgraph_structure::presort_main_dynamic()
{
    ZoneScopedN("dsg_presort_main");
    PERF_SCOPE(rmain_par_prep_sort);
    s_presort_groups.clear();
    size_t total_items = 0;
    for (u32 iPass = 0; iPass < SHADER_PASSES_MAX; ++iPass)
    {
        auto& groups = main_presorted[iPass];
        groups.clear();
        mapMatrixPasses[0][iPass].get_any_p(groups);
        std::sort(groups.begin(), groups.end(), [](const mapMatrix_T::value_type* left, const mapMatrix_T::value_type* right)
        {
            return left->second.ssa > right->second.ssa;
        });
        size_t items = 0;
        for (auto* group : groups)
        {
            items += group->second.size();
            s_presort_groups.push_back(group);
        }
        main_presorted_items[iPass] = items;
        total_items += items;
    }

    const auto sort_group = [](mapMatrix_T::value_type* group)
    {
        mapMatrixItems& items = group->second;
        std::sort(items.begin(), items.end(), [](const _MatrixItem& lhs, const _MatrixItem& rhs) { return lhs.ssa > rhs.ssa; });
    };
    constexpr size_t PRESORT_PARALLEL_MIN_ITEMS = 1024;
    constexpr u32 PRESORT_MAX_JOBS = 8;
    const u32 workers = TaskScheduler ? static_cast<u32>(TaskScheduler->GetWorkersCount()) : 0;
    const u32 group_count = static_cast<u32>(s_presort_groups.size());
    const u32 jobs = std::min({ group_count, workers, PRESORT_MAX_JOBS });
    if (jobs < 2 || total_items < PRESORT_PARALLEL_MIN_ITEMS)
    {
        for (auto* group : s_presort_groups)
            sort_group(group);
    }
    else
    {
        // Runs of groups with about the same item count, in group order.
        s_presort_jobs.clear();
        const size_t target = (total_items + jobs - 1) / jobs;
        size_t run_items = target; // opens the first job at group 0
        for (u32 g = 0; g < group_count; ++g)
        {
            if (run_items >= target && s_presort_jobs.size() < jobs)
            {
                s_presort_jobs.push_back(g);
                run_items = 0;
            }
            run_items += s_presort_groups[g]->second.size();
        }
        const u32 job_count = static_cast<u32>(s_presort_jobs.size());
        s_presort_jobs.push_back(group_count);
        run_jobs_parallel(job_count, job_count - 1,
            [&sort_group](u32 j)
            {
                for (u32 g = s_presort_jobs[j]; g < s_presort_jobs[j + 1]; ++g)
                    sort_group(s_presort_groups[g]);
            },
            [] {});
    }
    s_presort_groups.clear();
    main_presorted_valid = true;
}

// sub-space rendering - main procedure
// scope (26-framemove-game-next.md, early main build): early_static stops before the dynamic part, late_dynamic
// starts there - on the same structure, the options (o) and the traversal results of the early half untouched between.
void R_dsgraph_structure::build_subspace(build_scope scope)
{
    const bool run_static = scope != build_scope::late_dynamic;
    const bool run_dynamic = scope != build_scope::early_static;
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
    // render/svp/build_* (31-svp-scope.md): the scope view's build, the SVP worker's or the sequential one.
    [[maybe_unused]] const bool perf_svp = o.svp_view;
    // Tracy zone is thread-safe; this runs on task workers for the 3 parallel sun cascades plus the
    // main pass (4 calls/frame). Coarse zone -> negligible overhead, reveals culling/render-list cost.
    // Main pass shows on the primary thread; cascades on task workers -> split is visible per-thread.
    ZoneScopedN("dsg_build_subspace");
    main_presorted_valid = false; // R3: any build of this structure drops a previous presort
    if (run_static)
        marker++; // !!! critical here (once per build: the late half keeps the early half's marker)

    if (run_static && o.precise_portals && RImplementation.rmPortals)
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
        if (run_dynamic && g_pGameLevel)
            g_pGameLevel->pHUD->Render_Last(context_id);
        return;
    }

    // Traverse sector/portal structure (bounds-checked: OOB would be UB; keep traverser epoch if we skip)
    CSector* const start_sector = (o.sector_id < Sectors.size()) ? Sectors[o.sector_id] : nullptr;
    if (!run_static)
    {
        // late_dynamic: the early half traversed already (sector r_marker/r_frustums are read below)
    }
    else if (start_sector)
    {
        ZoneScopedN("dsg_portal_traverse");
        PERF_SCOPE_IF(perf_svp, rsvp_build_traverse);
        PortalTraverser.i_ssaDISCARD = pass_ssaDISCARD(o);
        PortalTraverser.i_ssaLOD_A = pass_ssaLOD_A(o);
        PortalTraverser.i_ssaLOD_B = pass_ssaLOD_B(o);
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

    if (run_static && psDeviceFlags.test(rsDrawStatic))
    {
        ZoneScopedN("dsg_static");
        PERF_SCOPE_IF(perf_main, rmain_build_static);
        PERF_SCOPE_IF(perf_svp, rsvp_build_static);
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

    const bool collect_dynamic_any = run_dynamic && (o.spatial_types != 0) && psDeviceFlags.test(rsDrawDynamic);

    if (collect_dynamic_any)
    {
        ZoneScopedN("dsg_dynamic");
        PERF_SCOPE_IF(perf_main, rmain_build_dynamic);
        PERF_SCOPE_IF(perf_svp, rsvp_build_dynamic);
        // Traverse object database
        // Shared pre-fetched list (one query for all light passes), 46-engine-core-render-audit.md: a light pass walks it
        // in place (precomputed_loop below) on the sphere/sector sampled after the shared query. Any other pass given
        // one (none today) keeps the old way - the pointers into lstRenderables and the generic loop.
        const bool precomputed_loop = o.precomputed_dynamic && !o.is_main_pass && !o.second_vp_pass && !o.svp_hom &&
            !(o.spatial_types & STYPE_LIGHTSOURCE) && !(o.spatial_traverse_flags & ISpatial_DB::O_ORDERED);
        if (o.precomputed_dynamic)
        {
            // the sector/frustum filters below drop everything this pass does not need
            lstRenderables.clear();
            if (!precomputed_loop)
            {
                lstRenderables.reserve(o.precomputed_dynamic->size());
                for (const precomputed_spatial_t& entry : *o.precomputed_dynamic)
                    lstRenderables.push_back(entry.spatial);
            }
            PERF_INC(render_precomputed);
            PERF_ADD(render_in, o.precomputed_dynamic->size());
        }
        else
        {
            ZoneScopedN("dsg_q_frustum");
            PERF_SCOPE_IF(perf_main, rmain_build_query);
            PERF_SCOPE_IF(perf_svp, rsvp_build_query);
            SpatialCaller spatial_caller(SpatialQueryCaller::render);
            g_pGamePersistent->SpatialSpace.q_frustum(lstRenderables, o.spatial_traverse_flags, o.spatial_types, o.view_frustum);
            PERF_ADD(render_in, lstRenderables.size());
        }

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

        // Light passes (precomputed_loop): the non-main branch of the loop below - the same tests in the same order, on
        // the sphere/sector sampled after the shared query (the loop read the same fields from the object a little later:
        // the main build's sector refresh is done by then, render_lights runs after r_main.sync; a concurrent
        // spatial_move is the staleness already accepted, see SpatialBase::spatial_move). lstRenderables is empty then.
        if (precomputed_loop)
        {
            for (const precomputed_spatial_t& entry : *o.precomputed_dynamic)
            {
                if (entry.sector_id == IRender_Sector::INVALID_SECTOR_ID || entry.sector_id >= Sectors.size())
                {
                    PERF_INC(render_sector_cull);
                    continue; // disassociated from S/P structure
                }
                const CSector* sector = Sectors[entry.sector_id];
                if (PortalTraverser.i_marker != sector->r_marker)
                {
                    PERF_INC(render_sector_cull);
                    continue; // inactive (untouched) sector
                }
                for (const CFrustum& view : sector->r_frustums)
                {
                    if (!view.testSphere_dirty(entry.sphere.P, entry.sphere.R))
                    {
                        PERF_INC(render_sphere_miss);
                        continue;
                    }
                    IRenderable* renderable = entry.spatial->dcast_Renderable();
                    if (nullptr == renderable)
                        continue; // unknown, but renderable object (r1_glow???)
                    // Per-light owner exclusion (see the loop below)
                    if (o.phase == CRender::PHASE_SMAP && o.shadow_owner && renderable == o.shadow_owner)
                    {
                        PERF_INC(render_shadow_owner_skip);
                        continue;
                    }
                    PERF_ADD_IF(perf_svp, rsvp_objects, 1);
                    PERF_INC(renderable_render);
                    renderable->renderable_Render(context_id, nullptr); // not the scope pass: no root (as below)
                }
            }
        }

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
                // 46-engine-core-render-audit.md: the sphere read with type/sector above (two virtual calls less per frustum)
                if (!view.testSphere_dirty(sphere.P, sphere.R))
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
                        // 46-engine-core-render-audit.md: the HOM test only when due, see process_item of
                        // build_dynamic_main_parallel
                        vis_data& v_orig = ((dxRender_Visual*)renderable->GetRenderData().visual)->vis;
                        BOOL bVisible = TRUE;
                        {
                            PERF_SCOPE(rmain_build_hom); // main view only (this whole branch is)
                            if (Device.dwFrame >= v_orig.hom_frame)
                            {
                                vis_data v_copy = v_orig;
                                v_copy.box.xform(renderable->GetRenderData().xform);
                                bVisible = RImplementation.HOM.visible(v_copy);
                                v_orig.hom_frame = v_copy.hom_frame;
                                v_orig.hom_tested = v_copy.hom_tested;
                            }
                        }
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
                        PERF_ADD_IF(perf_svp, rsvp_objects, 1); // the sequential scope build takes this branch
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
                    //
                    // H (31-svp-scope.md): the scope build tests the object against the MAIN view's HOM -
                    // the same eye, and the scope frustum lies inside the main one, so what the main view's
                    // occluders hide is hidden through the lens too. The occlusion buffer is final here: Calculate()
                    // waited for this frame's HOM task before the kick, the next one starts with the next frame's
                    // camera (OnCameraUpdated), after the join. CHOM::visible only reads it (as the parallel main
                    // build does from its workers). On a copy with hom_frame = 0: tested every frame, and nothing
                    // is written back - the main build owns the object's vis_data (marker, hom_frame...).
                    if (o.svp_hom && (type & STYPE_RENDERABLE))
                    {
                        const auto& rdata = renderable->GetRenderData();
                        if (const auto* visual = (const dxRender_Visual*)rdata.visual)
                        {
                            vis_data v_copy;
                            v_copy.clear();
                            v_copy.box = visual->vis.box;
                            v_copy.box.xform(rdata.xform);
                            if (!RImplementation.HOM.visible(v_copy))
                            {
                                PERF_INC(rsvp_hom_cull);
                                break; // exit loop on frustums (the HOM test does not depend on them)
                            }
                        }
                    }
                    PERF_ADD_IF(perf_svp, rsvp_objects, 1);
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

        // R3 (27-render-main.md): the last insert of the main view's build is behind -
        // sort its priority-0 dynamic groups for part0 here, on the build's thread and helpers.
        if (main_par_prepare_in_build_on(o))
            presort_main_dynamic();
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
