#pragma once

#include <utility>

#include "r__sector.h"

namespace xray::render::RENDER_NAMESPACE
{
class CKinematics;
// feedback	for receiving visuals
class R_feedback
{
public:
    virtual void rfeedback_static(dxRender_Visual* V) = 0;
};

// LOD / culling thresholds of one camera pass - the values CRender::publish_lod_globals() writes into the globals
// r_ssa* / r_dtex_range (the formulas there). Kept per pass where the globals belong to another pass.
struct lod_thresholds_t
{
    float ssaDISCARD{};
    float ssaLOD_A{};
    float ssaLOD_B{};
    float ssaLOD_CHAR_A{};
    float GLOD_start{};
    float GLOD_end{};
    float dtex_range{};
};

// One object of the light passes' shared dynamic list (CRender::render_lights, options_t::precomputed_dynamic): the
// spatial and the SpatialData fields build_subspace's sector/frustum filters read, sampled once right after the shared
// query (46-engine-core-render-audit.md).
struct precomputed_spatial_t
{
    ISpatial* spatial;
    Fsphere sphere;
    IRender_Sector::sector_id_t sector_id;
};

struct R_dsgraph_structure
{
    static constexpr auto INVALID_CONTEXT_ID = static_cast<u32>(-1);
#if RENDER == R_R1
    static constexpr auto IMM_CTX_ID = 0; // TODO: to remove this ugly #ifdef we need to introduce per-render configuration
#else
    static constexpr auto IMM_CTX_ID = R__NUM_PARALLEL_CONTEXTS; // the next after pooled
#endif

    R_feedback* val_feedback{}; // feedback for geometry being rendered
    u32 val_feedback_breakp{}; // breakpoint
    xr_vector<Fbox3>* val_recorder; // coarse structure recorder
    u32 marker{};
    u32 context_id{ INVALID_CONTEXT_ID };

    struct options_t
    {
        u32 phase{};
        u32 portal_traverse_flags{};
        u32 spatial_traverse_flags{};
        u32 spatial_types{ STYPE_RENDERABLE };
        float query_box_side{ EPS_L * 20.0f };
        Fvector view_pos{};
        Fmatrix xform{};
        CFrustum view_frustum{};
        IRender_Sector::sector_id_t sector_id;
        bool pmask[2];
        bool pmask_wmark;
        bool use_hom{ false };
        bool precise_portals{ false };
        bool is_main_pass{ false };
        // Stage C (SVP): build runs on a worker concurrently with the main render; blocks that
        // mutate shared game-side state (uLastLTRACK, CROS updates, spatial_updatesector) must
        // skip this pass - the main build has already updated them this frame.
        bool second_vp_pass{ false };
        bool mt_calculate{ false };
        bool use_shadow_hull_cull{ false };
        Fvector shadow_light_pos{};
        float shadow_light_range{};
        // Shared pre-fetched dynamic list: when set, build_subspace skips q_frustum and
        // iterates this list instead (light passes filter it via sector/frustum tests, on the sampled fields)
        const xr_vector<precomputed_spatial_t>* precomputed_dynamic{};
        // Per-light owner exclusion from SMAP caster build: when set, the renderable matching
        // this pointer is skipped during PHASE_SMAP to prevent self-shadowing (e.g. NPC torch
        // shadow-mapping its own owner). Set from light->shadow_owner per smap build.
        ::IRenderable* shadow_owner{};
        // 31-svp-scope.md: the scope build runs on the SVP worker while the globals r_ssa* / r_dtex_range still hold
        // the MAIN view's thresholds (the scope ones are published only after the join, ApplySecondVPLodGlobals).
        // lod_own - this pass culls, picks LODs and shader elements (and its worker recording picks the geometry LOD
        // and the parallax range) with its own snapshot lod instead of the globals. false - the globals (every
        // other pass: they are published for it before its build).
        bool lod_own{ false };
        lod_thresholds_t lod{};
        // 31-svp-scope.md: this is the scope view's build (the SVP worker, or the sequential second Calculate) - only
        // the render/svp/* metrics read it (the sequential one keeps second_vp_pass false and its old behaviour).
        bool svp_view{ false };
        // H (31-svp-scope.md): the scope build on the SVP worker tests its dynamic objects against the main view's HOM, on a
        // copy of their vis_data (the main build owns the originals).
        bool svp_hom{ false };
    } o;

    // Dynamic scene graph
    // R_dsgraph::mapNormal_T										mapNormal	[2]		;	// 2==(priority/2)
    R_dsgraph::mapNormalPasses_T mapNormalPasses[2]; // 2==(priority/2)
    // R_dsgraph::mapMatrix_T										mapMatrix	[2]		;
    R_dsgraph::mapMatrixPasses_T mapMatrixPasses[2];
    R_dsgraph::mapSorted_T mapSorted;
    R_dsgraph::mapHUD_T mapHUD;
    R_dsgraph::mapLOD_T mapLOD;
    R_dsgraph::mapSorted_T mapDistort;
    R_dsgraph::mapHUD_T    mapHUDSorted;

#if RENDER != R_R1
    R_dsgraph::mapSorted_T mapWmark; // sorted
    R_dsgraph::mapSorted_T mapEmissive;
    R_dsgraph::mapSorted_T mapHUDEmissive;
#endif

    xr_vector<CSector*> Sectors;
    xr_vector<CPortal*> Portals;
    CPortalTraverser PortalTraverser;
    xrXRC Sectors_xrc;

    // Runtime structures
    xr_vector<R_dsgraph::mapNormal_T::value_type*> nrmPasses;
    xr_vector<R_dsgraph::mapMatrix_T::value_type*> matPasses;
    // R3 (27-render-main.md): the main view's priority-0 dynamic groups in render_graph(0)
    // order, sorted with their items at the end of the build (presort_main_dynamic); main_presorted_items - their item
    // count then. Taken by render_graph_main_parallel() or render_graph(0) (take_main_presorted).
    xr_vector<R_dsgraph::mapMatrix_T::value_type*> main_presorted[SHADER_PASSES_MAX];
    size_t main_presorted_items[SHADER_PASSES_MAX]{};
    bool main_presorted_valid{};
    xr_vector<R_dsgraph::_LodItem> lstLODs;
    xr_vector<int> lstLODgroups;
    xr_vector<ISpatial*> lstRenderables;
    xr_vector<ISpatial*> lstSpatial;
    xr_vector<dxRender_Visual*> lstVisuals;
    // NPC blob impostors collected during PHASE_SMAP (matrix = scale to bounding sphere)
    xr_vector<Fmatrix> npc_blobs;

    CBackend cmd_list{};

    u32 counter_S{};
    u32 counter_D{};

    void set_Feedback(R_feedback* V, u32 id)
    {
        val_feedback_breakp = id;
        val_feedback = V;
    }
    void set_Recorder(xr_vector<Fbox3>* dest)
    {
        val_recorder = dest;
        if (dest)
            dest->clear();
    }
    void get_Counters(u32& s, u32& d)
    {
        s = counter_S;
        d = counter_D;
    }
    void clear_Counters() { counter_S = counter_D = 0; }

    R_dsgraph_structure() : Sectors_xrc("dsgraph")
    {
        r_pmask(true, true);
    };

    // frame_reset - the per-frame recycling of the structure (D3DXRenderBase::End -> cleanup_contexts(true),
    // alloc_context): the maps are emptied but keep their nodes (reset_maps). false -
    // level load/unload, device reset: everything freed.
    void reset(bool frame_reset = false)
    {
        //marker = 0;
        context_id = INVALID_CONTEXT_ID;

        o.query_box_side = EPS_L * 20;
        o.use_hom = false;
        o.precise_portals = false;
        o.is_main_pass = false;
        o.second_vp_pass = false;
        o.spatial_traverse_flags = 0;
        o.portal_traverse_flags = 0;
        o.spatial_types = STYPE_RENDERABLE;
        o.use_shadow_hull_cull = false;
        o.shadow_light_pos.set(0, 0, 0);
        o.shadow_light_range = 0.f;
        o.precomputed_dynamic = nullptr;
        o.shadow_owner = nullptr;
        o.lod_own = false;
        o.svp_view = false;
        o.svp_hom = false;

        val_recorder = nullptr;
        val_feedback = nullptr;

        nrmPasses.clear();
        matPasses.clear();
        main_presorted_valid = false; // R3: the maps are destroyed (or emptied, R13) below
        for (auto& groups : main_presorted)
            groups.clear();

        lstLODs.clear();
        lstLODgroups.clear();
        lstRenderables.clear();
        lstSpatial.clear();
        lstVisuals.clear();
        npc_blobs.clear();

        reset_maps(frame_reset);
        cmd_list.Invalidate();
    }

    // The maps part of reset() (r__dsgraph_build.cpp): destroyed, or (R13, frame_reset)
    // emptied with their nodes kept - the node arrays and the item vectors of the nodes - and the slow decay.
    void reset_maps(bool frame_reset);
    // R13: nodes allocated by all the maps of this structure; max_map - raised to the largest single map.
    size_t maps_capacity(size_t& max_map) const;
    // R13: resets with the nodes kept since the last decay check of this structure.
    u32 reserve_resets{};

    void r_pmask(bool _1, bool _2, bool _wm = false)
    {
        o.pmask[0] = _1;
        o.pmask[1] = _2;
        o.pmask_wmark = _wm;
    }

    void load(const xr_vector<CSector::level_sector_data_t> &sectors, const xr_vector<CPortal::level_portal_data_t> &portals);
    void unload();

    ICF IRender_Portal* get_portal(size_t id) const
    {
        VERIFY(id < Portals.size());
        return Portals[id];
    }
    ICF IRender_Sector* get_sector(size_t id) const
    {
        VERIFY(id < Sectors.size());
        return Sectors[id];
    }
    IRender_Sector::sector_id_t detect_sector(const Fvector& P);
    IRender_Sector::sector_id_t detect_sector(const Fvector& P, Fvector& D);
    // Same with a caller-owned collider: Sectors_xrc keeps query results, so concurrent callers need their own.
    IRender_Sector::sector_id_t detect_sector(xrXRC& xrc, const Fvector& P);
    IRender_Sector::sector_id_t detect_sector(xrXRC& xrc, const Fvector& P, Fvector& D);

    void add_static(dxRender_Visual* pVisual, const CFrustum& view, u32 planes);
    // lod_bind_source: when following m_lod, pass hi CKinematics so LOD skinning uses hi bone matrices (LOD has no animation blends).
    void add_leafs_dynamic(
        IRenderable* root, dxRender_Visual* pVisual, Fmatrix& xform, CKinematics* lod_bind_source = nullptr);
    void add_leafs_static(dxRender_Visual* pVisual); // if detected node's full visibility

    void insert_dynamic(IRenderable* root, dxRender_Visual* pVisual, Fmatrix& xform, Fvector& Center);
    void insert_static(dxRender_Visual* pVisual);

    // Shadow-caster culling: true when the shadow hull (light apex extruded through the
    // AABB up to light range) cannot reach the main camera frustum at all. Used both for
    // single visuals (insert_static) and whole sectors (build_subspace fast reject).
    bool shadow_hull_cull(const Fbox& caster_box) const;

    // render primitives
    void render_graph(u32 _priority);
    // The static (mapNormalPasses) half of render_graph(_priority) alone - drawn and emptied the same way; a later
    // render_graph(_priority) then draws only the dynamic half (27-render-main.md, R2).
    void render_graph_static(u32 _priority);
    // P19 (d), plans/optimization_mt/19-part0-submit.md: render_graph(0) of the main view (immediate context, after
    // render_hud) recorded in parallel on pooled D3D11 deferred contexts and executed in graph order.
    // true - the graph is drawn and its maps are empty, the immediate context is re-seeded with the G-buffer state of
    // phase_scene_begin(); false - not this frame (not D3D11, RenderDoc, few draws, no free context), the
    // caller runs render_graph(0).
    bool render_graph_main_parallel();
    void render_hud();
    void render_hud_ui();
    // HUD overlay scope (g_3d_scopes 3): drain only HUD forward passes (scope lens, emissive).
    // lens_depth_clear, when non-null (overlay pass), drains mapHUDSorted in two filtered passes —
    // non-lens first, then the scope lens after the callback clears the overlay depth buffer.
    void render_hud_blends(void (*lens_depth_clear)(CBackend&) = nullptr);
    void render_lods(bool _setup_zb, bool _clear);
    // main_combine - the main view's combine (CRender::render_forward): the particle batch
    // (27-render-main.md, R1) and the render/combine/sorted_other, ps_* metrics; the shadow passes keep the plain loop.
    void render_sorted(bool main_combine = false);
    void render_emissive();
    void render_wmarks();
    void render_distort();
    void render_R1_box(IRender_Sector::sector_id_t sector_id, Fbox& _bb, int _element);

    // Early main build (plans/optimization_mt/26-framemove-game-next.md): the main view's build in
    // two halves on the same structure. early_static - marker, near-portal check, portal traversal, static geometry
    // (needs only the camera and HOM, may run beside the bones prepass); late_dynamic - the rest (q_frustum, objects,
    // HUD; after the prepass), with the options and traversal results the early half left. all - both in one call.
    enum class build_scope : u8
    {
        all,
        early_static,
        late_dynamic,
    };
    void build_subspace(build_scope scope = build_scope::all);

    // Parallel main build (plans/optimization_mt/08-parallel-main-build.md): the dynamic part of the main-view
    // build split into parts over task workers. Each part fills its own shard - a private R_dsgraph_structure - and the
    // shards are appended to this structure in part order, which reproduces the sequential insertion order exactly.
    //
    // The structure add_Visual() fills for this context on the calling thread: the shard of the part running on this
    // thread, otherwise this structure.
    R_dsgraph_structure& insert_target();
    // Appends the dynamic lists of a shard in its insertion order (mapMatrixPasses, the sorted/HUD maps).
    void merge_dynamic(R_dsgraph_structure& shard);
    // Empties the dynamic lists, keeping their memory for the next frame (unlike reset()).
    void clear_dynamic();
    // The same for everything a build fills (static lists and LODs too): drops an early build nobody consumed.
    void clear_build_lists();
    // Runs the main view's loop over lstRenderables split into parts; false - not this frame (the sequential loop runs).
    bool build_dynamic_main_parallel(u32 ltrack_index, bool collect_lights);
    // R3 (27-render-main.md): sorts mapMatrixPasses[0] - the groups and their items - as
    // render_graph(0) would, at the end of the main view's build (main_presorted, main_presorted_valid).
    void presort_main_dynamic();
    // R3: true - matPasses now holds the groups of mapMatrixPasses[0][iPass] in drawing order, their items sorted (the
    // caller skips both sorts); false - no presort, or the map has changed since (the caller sorts as before).
    bool take_main_presorted(u32 iPass);

private:
    // R1 (27-render-main.md): mapSorted drawn back to front with its particle effects prepared in one batch;
    // false - not this frame (cvar off, not D3D11, not the immediate context, RenderDoc, nothing to batch), nothing drawn.
    bool render_sorted_ps_batch();
    // The static half of render_graph (shared by render_graph and render_graph_static).
    void render_graph_static_passes(u32 _priority, bool use_fast_geo, bool perf_main);
};
} // namespace xray::render::RENDER_NAMESPACE
