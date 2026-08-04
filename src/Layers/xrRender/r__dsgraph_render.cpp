#include "stdafx.h"

#include "xrEngine/IRenderable.h"
#include "xrEngine/CustomHUD.h"

#include "FBasicVisual.h"
#include "SkeletonCustom.h"
#include "FLOD.h"

extern ENGINE_API float psHUD_FOV;

namespace xray::render::RENDER_NAMESPACE
{
using namespace R_dsgraph;

extern float r_ssaHZBvsTEX;
extern float r_ssaGLOD_start, r_ssaGLOD_end;

ICF float calcLOD(float ssa /*fDistSq*/, float /*R*/)
{
    return _sqrt(clampr((ssa - r_ssaGLOD_end) / (r_ssaGLOD_start - r_ssaGLOD_end), 0.f, 1.f));
}

template <class T>
bool cmp_ssa(const T &lhs, const T &rhs)
{
    return lhs.ssa > rhs.ssa;
}

// Sorting by SSA and changes minimizations
template <typename T>
bool cmp_pass(const T& left, const T& right)
{
    return left->second.ssa > right->second.ssa;
}

void R_dsgraph_structure::render_graph(u32 _priority)
{
    PIX_EVENT_CTX(cmd_list, dsgraph_render_graph);
    RImplementation.BasicStats.Primitives.Begin(); // XXX: Refactor a bit later

    cmd_list.set_xform_world(Fidentity);

    // **************************************************** NORMAL
    // Perform sorting based on ScreenSpaceArea
    // Sorting by SSA and changes minimizations
    // Render several passes
    {
        ZoneScopedN("dsgraph_render_static");
        PIX_EVENT_CTX(cmd_list, dsgraph_render_static);

        for (u32 iPass = 0; iPass < SHADER_PASSES_MAX; ++iPass)
        {
            auto& map = mapNormalPasses[_priority][iPass];

            map.get_any_p(nrmPasses);
            std::sort(nrmPasses.begin(), nrmPasses.end(), cmp_pass<mapNormal_T::value_type*>);
            for (const auto& it : nrmPasses)
            {
                cmd_list.set_Pass(it->first);
                cmd_list.apply_lmaterial();

                mapNormalItems& items = it->second;
                items.ssa = 0;

                std::sort(items.begin(), items.end(), cmp_ssa<_NormalItem>);
                for (const auto& item : items)
                {
                    const float LOD = calcLOD(item.ssa, item.pVisual->vis.sphere.R);
#ifdef USE_DX11
                    cmd_list.LOD.set_LOD(LOD);
#endif
                    // --#SM+#-- Обновляем шейдерные данные модели [update shader values for this model]
                    // RCache.hemi.c_update(item.pVisual);

                    item.pVisual->Render(cmd_list, LOD, o.phase == CRender::PHASE_SMAP);
                }
                items.clear();

            }
            nrmPasses.clear();
            map.clear();
        }
    }

    // **************************************************** MATRIX
    // Perform sorting based on ScreenSpaceArea
    // Sorting by SSA and changes minimizations
    // Render several passes
    {
        ZoneScopedN("dsgraph_render_dynamic");
        PIX_EVENT_CTX(cmd_list, dsgraph_render_dynamic);

        auto render_matrix_passes = [&](R_dsgraph::mapMatrixPasses_T& passSet, u32& statsCounter)
        {
            for (u32 iPass = 0; iPass < SHADER_PASSES_MAX; ++iPass)
            {
                auto& map = passSet[iPass];

                map.get_any_p(matPasses);
                std::sort(matPasses.begin(), matPasses.end(), cmp_pass<mapMatrix_T::value_type*>);
                for (const auto& it : matPasses)
                {
                    cmd_list.set_Pass(it->first);

                    mapMatrixItems& items = it->second;
                    items.ssa = 0;

                    std::sort(items.begin(), items.end(), cmp_ssa<_MatrixItem>);
                    for (auto& item : items)
                    {
                        cmd_list.set_xform_world(item.Matrix);
                        RImplementation.apply_object(cmd_list, item.pObject);
                        cmd_list.apply_lmaterial();

                        const float LOD = calcLOD(item.ssa, item.pVisual->vis.sphere.R);
#ifdef USE_DX11
                        cmd_list.LOD.set_LOD(LOD);
#endif
                        // --#SM+#-- Обновляем шейдерные данные модели [update shader values for this model]
                        // RCache.hemi.c_update(item.pVisual);

                        item.pVisual->Render(cmd_list, LOD, o.phase == CRender::PHASE_SMAP);
                        ++statsCounter;
                        if (item.pObject && item.pObject->renderable_ForceLodCharacter())
                            ++RImplementation.BasicStats.CorpseRegularSubmissions;
                    }
                    items.clear();
                }
                matPasses.clear();
                map.clear();
            }
        };

        render_matrix_passes(mapMatrixPasses[_priority], RImplementation.BasicStats.DynamicRegularSubmissions);
    }

    RImplementation.BasicStats.Primitives.End(); // XXX: Refactor a bit later
}

//////////////////////////////////////////////////////////////////////////
// Helper classes and functions

/*
Предназначен для установки режима отрисовки HUD и возврата оригинального после отрисовки.
*/
class hud_transform_helper
{
    Fmatrix Pold;
    static u32 cullMode;
    static bool isActive;

    CBackend& cmd_list;

public:
    explicit hud_transform_helper(CBackend& cmd_list_in)
        : cmd_list(cmd_list_in)
    {
        // Change projection
        Pold  = Device.mProject;

        // XXX: Xottab_DUTY: custom FOV. Implement it someday
        // It should be something like this:
        // float customFOV;
        // if (isCustomFOV)
        //     customFOV = V->getVisData().obj_data->m_hud_custom_fov;
        // else
        //     customFOV = psHUD_FOV * Device.fFOV;
        //
        // Device.mProject.build_projection(deg2rad(customFOV), Device.fASPECT,
        //    VIEWPORT_NEAR, g_pGamePersistent->Environment().CurrentEnv.far_plane);
        //
        // Look at the function:
        // void __fastcall sorted_L1_HUD(mapSorted_Node* N)
        // In the commit:
        // https://github.com/ShokerStlk/xray-16-SWM/commit/869de0b6e74ac05990f541e006894b6fe78bd2a5#diff-4199ef700b18ce4da0e2b45dee1924d0R83

        Fmatrix prj_new;
        prj_new.build_projection(deg2rad(psHUD_FOV * Device.fFOV /* *Device.fASPECT*/), Device.fASPECT,
            VIEWPORT_NEAR_3D, g_pGamePersistent->Environment().CurrentEnv.far_plane);
        cmd_list.set_xform_project(prj_new);

        RImplementation.rmNear(cmd_list);

        // preserve culling mode
        cullMode = cmd_list.get_CullMode();
        isActive = true;
    }

    ~hud_transform_helper()
    {
        RImplementation.rmNormal(cmd_list);

        // Restore projection
        cmd_list.set_xform_project(Pold);
        // restore culling mode
        cmd_list.set_CullMode(cullMode);
        isActive = false;
    }

    static void apply_custom_state(CBackend& cmd_list)
    {
        if (!isActive || !psHUD_Flags.test(HUD_LEFT_HANDED))
            return;

        // Change culling mode if HUD meshes were flipped
        if (cullMode != CULL_NONE)
        {
            cmd_list.set_CullMode(cullMode == CULL_CW ? CULL_CCW : CULL_CW);
        }
    }
};

u32 hud_transform_helper::cullMode = CULL_NONE;
bool hud_transform_helper::isActive = false;

template<class T>
void __fastcall render_item(u32 context_id, const T& item)
{
    auto& dsgraph = RImplementation.get_context(context_id);

    dxRender_Visual* V = item.second.pVisual;
    VERIFY(V && V->shader._get());
    dsgraph.cmd_list.set_Element(item.second.se);
    dsgraph.cmd_list.set_xform_world(item.second.Matrix);
    RImplementation.apply_object(dsgraph.cmd_list, item.second.pObject);
    dsgraph.cmd_list.apply_lmaterial();
    hud_transform_helper::apply_custom_state(dsgraph.cmd_list);
    //--#SM+#-- Обновляем шейдерные данные модели [update shader values for this model]
    //RCache.hemi.c_update(V);
    V->Render(dsgraph.cmd_list, calcLOD(item.first, V->vis.sphere.R), dsgraph.o.phase == CRender::PHASE_SMAP);
}

template<class T>
ICF void sort_front_to_back_render_and_clean(u32 context_id, T& vec)
{
    vec.traverse_left_right(context_id, render_item);
    vec.clear();
}

template<class T>
ICF void sort_back_to_front_render_and_clean(u32 context_id, T& vec)
{
    vec.traverse_right_left(context_id, render_item);
    vec.clear();
}

// HUD overlay scope (g_3d_scopes 3): split mapHUDSorted by the bScopeLens flag.
// The scope lens (samples $user$viewport2) needs a cleared depth buffer (it is otherwise Z-rejected
// by the scope body), while other transparent HUD surfaces (collimator glass etc.) must keep the
// HUD-mesh depth so they don't shine through the mesh. mapHUDSorted holds both, so the overlay pass
// drains it in two filtered traversals (non-lens first, then lens) with the caller changing the
// Z state in between. The map is cleared once at the end by the caller-friendly helper below.
// Ignored outside the overlay pipeline (render_sorted keeps the single unfiltered pass).
static thread_local bool g_render_hudblends_want_scope_lens = false;

template<class T>
void __fastcall render_item_scope_lens_filter(u32 context_id, const T& item)
{
    const bool is_lens = item.second.se->flags.bScopeLens != 0;
    if (is_lens != g_render_hudblends_want_scope_lens)
        return; // opposite polarity: skip, this pass belongs to the other group
    render_item(context_id, item);
}

// One filtered traversal (back-to-front). Does NOT clear the map — render_hud_blends() drains both
// groups (non-lens, then lens) and clears once at the end.
template<class T>
ICF void render_hud_sorted_filtered(u32 context_id, T& vec, bool want_scope_lens)
{
    if (vec.empty())
        return;
    g_render_hudblends_want_scope_lens = want_scope_lens;
    // Pass the template unqualified (no <T>): like render_item above, the compiler deduces the node
    // type from traverse_right_left's callback signature (value_type = xr_fixed_map_node<K,_MatrixItemS>).
    vec.traverse_right_left(context_id, render_item_scope_lens_filter);
}

//////////////////////////////////////////////////////////////////////////
// HUD render
void R_dsgraph_structure::render_hud()
{
    ZoneScoped;
    PIX_EVENT_CTX(cmd_list, dsgraph_render_hud);

    if (!mapHUD.empty())
    {
        hud_transform_helper helper{ cmd_list };
        sort_front_to_back_render_and_clean(context_id, mapHUD);
    }

#if RENDER == R_R1
    if (g_pGameLevel->pHUD && g_pGameLevel->pHUD->RenderActiveItemUIQuery())
        render_hud_ui(); // hud ui
#endif
}

void R_dsgraph_structure::render_hud_ui()
{
    ZoneScoped;
    CCustomHUD* levelHud = g_pGameLevel->pHUD;
    VERIFY(levelHud && levelHud->RenderActiveItemUIQuery());

    PIX_EVENT_CTX(cmd_list, dsgraph_render_hud_ui);

    hud_transform_helper helper{ cmd_list };

#if RENDER != R_R1
    // Targets, use accumulator for temporary storage
    const ref_rt rt_null;
    cmd_list.set_RT(0, 1);
    cmd_list.set_RT(0, 2);
    auto zb = RImplementation.Target->rt_Base_Depth;

#if (RENDER == R_R3) || (RENDER == R_R4) || (RENDER==R_GL)
    if (RImplementation.o.msaa)
        zb = RImplementation.Target->rt_MSAADepth;
#endif

    RImplementation.Target->u_setrt(cmd_list,
        RImplementation.o.albedo_wo ? RImplementation.Target->rt_Accumulator : RImplementation.Target->rt_Color,
        rt_null, rt_null, zb);
#endif // RENDER!=R_R1

    levelHud->RenderActiveItemUI();
}

//////////////////////////////////////////////////////////////////////////
// HUD overlay scope (g_3d_scopes 3): drain only the HUD forward passes (scope lens, blended and
// emissive HUD surfaces). Used by the offscreen HUD overlay pass; the world pass skips these maps.
// When lens_depth_clear is provided (overlay pass), mapHUDSorted is drained in two filtered
// traversals: non-lens surfaces first (depth-tested against the HUD mesh), then the scope lens
// after the caller clears the overlay depth buffer (the lens is otherwise Z-rejected by the scope
// body). Without the callback the whole map is drained in a single back-to-front pass.
void R_dsgraph_structure::render_hud_blends(void (*lens_depth_clear)(CBackend&))
{
    ZoneScoped;
    PIX_EVENT_CTX(cmd_list, dsgraph_render_hud_blends);

    if (!mapHUDSorted.empty())
    {
        hud_transform_helper helper{ cmd_list };
        if (lens_depth_clear)
        {
            // Pass A: non-lens HUD blends (collimator glass etc.) keep the HUD-mesh depth so they
            // do not shine through the weapon/scope mesh.
            render_hud_sorted_filtered(context_id, mapHUDSorted, false);
            // Let the caller reset the overlay depth to far so the scope lens passes its Z test.
            lens_depth_clear(cmd_list);
            // Pass B: scope lens only (draws over the lens opening, samples the second viewport).
            render_hud_sorted_filtered(context_id, mapHUDSorted, true);
            mapHUDSorted.clear();
        }
        else
        {
            sort_back_to_front_render_and_clean(context_id, mapHUDSorted);
        }
    }

#if RENDER != R_R1
    if (!mapHUDEmissive.empty())
    {
        hud_transform_helper helper{ cmd_list };
        sort_front_to_back_render_and_clean(context_id, mapHUDEmissive);
    }
#endif
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void R_dsgraph_structure::render_sorted()
{
    ZoneScoped;
    PIX_EVENT_CTX(cmd_list, dsgraph_render_sorted);

    sort_back_to_front_render_and_clean(context_id, mapSorted);

    // HUD overlay scope (g_3d_scopes 3): HUD blends (scope lens etc.) are rendered in the offscreen
    // overlay pass, NOT in the world pass. Prevents lens recursion — the lens samples the clean
    // backbuffer copy (rt_secondVP) which has no HUD, so no self-reference.
    if (!mapHUDSorted.empty() && !RImplementation.IsHudOverlayActive())
    {
        hud_transform_helper helper{ cmd_list };
        sort_back_to_front_render_and_clean(context_id, mapHUDSorted);
    }
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void R_dsgraph_structure::render_emissive()
{
#if RENDER != R_R1
    ZoneScoped;
    PIX_EVENT_CTX(cmd_list, dsgraph_render_emissive);

    sort_front_to_back_render_and_clean(context_id, mapEmissive);

    // HUD overlay scope (g_3d_scopes 3): HUD emissive surfaces go to the offscreen overlay, not the world.
    if (!mapHUDEmissive.empty() && !RImplementation.IsHudOverlayActive())
    {
        hud_transform_helper helper{ cmd_list };
        sort_front_to_back_render_and_clean(context_id, mapHUDEmissive);
    }
#endif
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void R_dsgraph_structure::render_wmarks()
{
#if RENDER != R_R1
    ZoneScoped;
    PIX_EVENT(dsgraph_render_wmarks);

    sort_front_to_back_render_and_clean(context_id, mapWmark);
#endif
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void R_dsgraph_structure::render_distort()
{
    ZoneScoped;
    PIX_EVENT(dsgraph_render_distort);

    sort_back_to_front_render_and_clean(context_id, mapDistort);
}

void R_dsgraph_structure::render_R1_box(IRender_Sector::sector_id_t sector_id, Fbox& BB, int sh)
{
    VERIFY(sector_id != IRender_Sector::INVALID_SECTOR_ID);
    auto* S = Sectors[sector_id];

    PIX_EVENT(dsgraph_render_R1_box);

    lstVisuals.clear();
    lstVisuals.push_back(((CSector*)S)->root());

    for (size_t test = 0; test < lstVisuals.size(); ++test)
    {
        dxRender_Visual* V = lstVisuals[test];

        // Visual is 100% visible - simply add it
        switch (V->Type)
        {
        case MT_HIERRARHY:
        {
            // Add all children
            FHierrarhyVisual* pV = (FHierrarhyVisual*)V;
            for (auto& i : pV->children)
            {
                dxRender_Visual* T = i;
                if (BB.intersect(T->vis.box))
                    lstVisuals.push_back(T);
            }
        }
        break;
        case MT_SKELETON_ANIM:
        case MT_SKELETON_RIGID:
        {
            // Add all children	(s)
            CKinematics* pV = (CKinematics*)V;
            pV->CalculateBones(TRUE);
            for (auto& i : pV->children)
            {
                dxRender_Visual* T = i;
                if (BB.intersect(T->vis.box))
                    lstVisuals.push_back(T);
            }
        }
        break;
        case MT_LOD:
        {
            FLOD* pV = (FLOD*)V;
            for (auto& i : pV->children)
            {
                dxRender_Visual* T = i;
                if (BB.intersect(T->vis.box))
                    lstVisuals.push_back(T);
            }
        }
        break;
        default:
        {
            // Renderable visual
            ShaderElement* E2 = V->shader->E[sh]._get();
            if (E2 && !(E2->flags.bDistort))
            {
                for (u32 pass = 0; pass < E2->passes.size(); pass++)
                {
                    cmd_list.set_Element(E2, pass);
                    V->Render(cmd_list, -1.f, o.phase == CRender::PHASE_SMAP);
                }
            }
        }
        break;
        }
    }
}
} // namespace xray::render::RENDER_NAMESPACE
