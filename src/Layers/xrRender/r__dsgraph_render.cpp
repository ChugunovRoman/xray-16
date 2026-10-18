#include "stdafx.h"

#include "xrEngine/IRenderable.h"
#include "xrEngine/CustomHUD.h"

#include "FBasicVisual.h"
#include "SkeletonCustom.h"
#include "FLOD.h"
#include "ParticleEffect.h"

#include "xrCore/Profiling/PerfMetrics.hpp"
#ifdef USE_DX11
#include <atomic>
#include <thread>

#include "FSkinned.h"
#include "LightTrack.h"
#include "xrCore/Threading/TaskManager.hpp"
#include "Layers/xrRenderDX11/dx11ConstantBuffer.h"
#endif

// DX11-only constant-buffer flush deltas for render/main/*_cb_flush (g_cb_flush_* are per-thread totals).
#if defined(USE_DX11) && XR_PERF_METRICS
#define PERF_CB_SNAPSHOT(name) [[maybe_unused]] const u32 name##_n = g_cb_flush_count; [[maybe_unused]] const u64 name##_b = g_cb_flush_bytes
#define PERF_CB_DELTA(cond, m_count, name) PERF_ADD_IF(cond, m_count, g_cb_flush_count - name##_n)
#define PERF_CB_BYTES(cond, m_bytes, name) PERF_ADD_IF(cond, m_bytes, g_cb_flush_bytes - name##_b)
#else
#define PERF_CB_SNAPSHOT(name) ((void)0)
#define PERF_CB_DELTA(cond, m_count, name) ((void)0)
#define PERF_CB_BYTES(cond, m_bytes, name) ((void)0)
#endif

extern ENGINE_API float psHUD_FOV;

namespace xray::render::RENDER_NAMESPACE
{
using namespace R_dsgraph;

extern float r_ssaHZBvsTEX;
extern float r_ssaGLOD_start, r_ssaGLOD_end;

// Geometry LOD range of the pass being drawn: its own snapshot when the scope geometry is recorded on the SVP worker
// while the globals still hold the main view's range (R_dsgraph_structure::options_t::lod_own),
// otherwise the globals.
struct glod_range_t
{
    float start;
    float end;
};

ICF glod_range_t pass_glod(const R_dsgraph_structure::options_t& o)
{
    if (o.lod_own)
        return { o.lod.GLOD_start, o.lod.GLOD_end };
    return { r_ssaGLOD_start, r_ssaGLOD_end };
}

ICF float calcLOD(float ssa /*fDistSq*/, float /*R*/, const glod_range_t& glod)
{
    return _sqrt(clampr((ssa - glod.end) / (glod.start - glod.end), 0.f, 1.f));
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

// The group bodies of render_graph, shared with the parallel main-view submit (render_graph_main_parallel below): a run of
// SSA-sorted items of one SPass group drawn on cmd_list. Static (mapNormal) items keep the world transform the caller
// set (identity); dynamic (mapMatrix) items set their own.
static void render_normal_items(CBackend& cmd_list, SPass* pass, const _NormalItem* items, size_t count,
    const glod_range_t& glod, bool use_fast_geo, [[maybe_unused]] bool perf_main)
{
    cmd_list.set_Pass(pass);
    cmd_list.apply_lmaterial();
    PERF_ADD_IF(perf_main, rmain_static_passes, 1);
    PERF_ADD_IF(perf_main, rmain_static_items, count);

    for (size_t i = 0; i < count; ++i)
    {
        const _NormalItem& item = items[i];
        const float LOD = calcLOD(item.ssa, item.pVisual->vis.sphere.R, glod);
#ifdef USE_DX11
        cmd_list.LOD.set_LOD(LOD);
#endif
        // --#SM+#-- Обновляем шейдерные данные модели [update shader values for this model]
        // RCache.hemi.c_update(item.pVisual);

        item.pVisual->Render(cmd_list, LOD, use_fast_geo);
    }
}

// Returns the number of items whose object is forced to the character LOD (BasicStats.CorpseRegularSubmissions).
static u32 render_matrix_items(CBackend& cmd_list, SPass* pass, const _MatrixItem* items, size_t count,
    const glod_range_t& glod, bool use_fast_geo, [[maybe_unused]] bool perf_main)
{
    cmd_list.set_Pass(pass);
    PERF_ADD_IF(perf_main, rmain_dyn_passes, 1);

    u32 corpses = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const _MatrixItem& item = items[i];
        cmd_list.set_xform_world(item.Matrix);
        {
            PERF_SCOPE_IF(perf_main, rmain_dyn_apply_object);
            RImplementation.apply_object(cmd_list, item.pObject);
        }
        cmd_list.apply_lmaterial();

        const float LOD = calcLOD(item.ssa, item.pVisual->vis.sphere.R, glod);
#ifdef USE_DX11
        cmd_list.LOD.set_LOD(LOD);
#endif
        // --#SM+#-- Обновляем шейдерные данные модели [update shader values for this model]
        // RCache.hemi.c_update(item.pVisual);

        {
            PERF_SCOPE_IF(perf_main, rmain_dyn_visual_render);
            item.pVisual->Render(cmd_list, LOD, use_fast_geo);
        }
        if (item.pObject && item.pObject->renderable_ForceLodCharacter())
            ++corpses;
    }
    return corpses;
}

void R_dsgraph_structure::render_graph_static_passes(u32 _priority, bool use_fast_geo, [[maybe_unused]] bool perf_main)
{
    // **************************************************** NORMAL
    // Perform sorting based on ScreenSpaceArea
    // Sorting by SSA and changes minimizations
    // Render several passes
    ZoneScopedN("dsgraph_render_static");
    PIX_EVENT_CTX(cmd_list, dsgraph_render_static);
    PERF_SCOPE_IF(perf_main, rmain_submit_static);
    // render/svp/geom_* (31-svp-scope.md): the scope view's geometry recording (the SVP worker or the sequential pass).
    [[maybe_unused]] const bool perf_svp = o.svp_view && _priority == 0;
    PERF_SCOPE_IF(perf_svp, rsvp_geom_static);
    [[maybe_unused]] const u32 perf_draws0 = cmd_list.stat.render.calls;
    PERF_CB_SNAPSHOT(perf_cb_static);
    const glod_range_t glod = pass_glod(o);

    for (u32 iPass = 0; iPass < SHADER_PASSES_MAX; ++iPass)
    {
        auto& map = mapNormalPasses[_priority][iPass];

        map.get_any_p(nrmPasses);
        std::sort(nrmPasses.begin(), nrmPasses.end(), cmp_pass<mapNormal_T::value_type*>);
        for (const auto& it : nrmPasses)
        {
            mapNormalItems& items = it->second;
            items.ssa = 0;

            std::sort(items.begin(), items.end(), cmp_ssa<_NormalItem>);
            render_normal_items(cmd_list, it->first, items.data(), items.size(), glod, use_fast_geo, perf_main);
            items.clear();
        }
        nrmPasses.clear();
        map.clear();
    }
    PERF_ADD_IF(perf_main, rmain_static_draws, cmd_list.stat.render.calls - perf_draws0);
    PERF_ADD_IF(perf_svp, rsvp_geom_static_draws, cmd_list.stat.render.calls - perf_draws0);
    PERF_CB_DELTA(perf_main, rmain_static_cb_flush, perf_cb_static);
}

// 27-render-main.md (R2): the main view's static geometry before r_main.sync(), while the dynamic
// half of the build is still running on a worker - it fills only the dynamic lists (mapMatrixPasses, mapSorted, the HUD
// maps...), never mapNormalPasses, which the early static half has completed by then.
void R_dsgraph_structure::render_graph_static(u32 _priority)
{
    PIX_EVENT_CTX(cmd_list, dsgraph_render_graph);
    RImplementation.BasicStats.Primitives.Begin(); // XXX: Refactor a bit later
    cmd_list.set_xform_world(Fidentity);
    const bool perf_main = o.is_main_pass && !o.second_vp_pass;
    render_graph_static_passes(_priority, o.phase == CRender::PHASE_SMAP, perf_main);
    RImplementation.BasicStats.Primitives.End(); // XXX: Refactor a bit later
}

void R_dsgraph_structure::render_graph(u32 _priority)
{
    PIX_EVENT_CTX(cmd_list, dsgraph_render_graph);
    RImplementation.BasicStats.Primitives.Begin(); // XXX: Refactor a bit later

    cmd_list.set_xform_world(Fidentity);

    // render/main/* metrics: only the main view's graph (not cascades, lights or the scope pass).
    [[maybe_unused]] const bool perf_main = o.is_main_pass && !o.second_vp_pass;
    [[maybe_unused]] const bool perf_svp = o.svp_view && _priority == 0; // render/svp/geom_* (31-svp-scope.md)
    const bool use_fast_geo = o.phase == CRender::PHASE_SMAP;
#if defined(USE_DX11) && XR_PERF_METRICS
    // P19 step 2: backend redundancy counters of this graph (static + dynamic), see CBackend::_stats.
    const u32 perf_layout_skip0 = cmd_list.stat.layout_skip;
    const u32 perf_layout_set0 = cmd_list.stat.layout_set;
    const u32 perf_sampler_skip0 = cmd_list.stat.sampler_skip;
    const u32 perf_sampler_set0 = cmd_list.stat.sampler_set;
    const u32 perf_cb_flush_slots0 = cmd_list.stat.cb_flush_slots;
#endif

    // NORMAL (static)
    render_graph_static_passes(_priority, use_fast_geo, perf_main);

    // **************************************************** MATRIX
    // Perform sorting based on ScreenSpaceArea
    // Sorting by SSA and changes minimizations
    // Render several passes
    {
        ZoneScopedN("dsgraph_render_dynamic");
        PIX_EVENT_CTX(cmd_list, dsgraph_render_dynamic);
        PERF_SCOPE_IF(perf_main, rmain_submit_dynamic);
        PERF_SCOPE_IF(perf_svp, rsvp_geom_dynamic);
        [[maybe_unused]] const u32 perf_draws0 = cmd_list.stat.render.calls;
        [[maybe_unused]] const u32 perf_states0 = cmd_list.stat.states;
        [[maybe_unused]] const u32 perf_textures0 = cmd_list.stat.textures;
        PERF_CB_SNAPSHOT(perf_cb_dyn);
        const glod_range_t glod = pass_glod(o);
#if XR_PERF_METRICS
        // P15: lets CSkeletonX::_Render / dx11ConstantBuffer::Flush count render/main/skin_* and dyn_cb_* (restored on exit).
        xray::perf::ScopedTag perf_dyn_tag(g_perf_rmain_dyn, (perf_main && xray::perf::g_perf_metrics_active) ? 1 : 0);
        // 31-svp-scope.md: the same for render/svp/geom_skin_* of the scope view (its own thread-local tag).
        xray::perf::ScopedTag perf_svp_tag(g_perf_svp_geom, (perf_svp && xray::perf::g_perf_metrics_active) ? 1 : 0);
#endif

        auto render_matrix_passes = [&](R_dsgraph::mapMatrixPasses_T& passSet, u32& statsCounter)
        {
            for (u32 iPass = 0; iPass < SHADER_PASSES_MAX; ++iPass)
            {
                auto& map = passSet[iPass];

                // R3 (27-render-main.md): the main view's groups may come sorted from the build already.
                const bool presorted = _priority == 0 && take_main_presorted(iPass);
                if (!presorted)
                {
                    map.get_any_p(matPasses);
                    std::sort(matPasses.begin(), matPasses.end(), cmp_pass<mapMatrix_T::value_type*>);
                }
                for (const auto& it : matPasses)
                {
                    mapMatrixItems& items = it->second;
                    items.ssa = 0;

                    if (!presorted)
                        std::sort(items.begin(), items.end(), cmp_ssa<_MatrixItem>);
                    RImplementation.BasicStats.CorpseRegularSubmissions +=
                        render_matrix_items(cmd_list, it->first, items.data(), items.size(), glod, use_fast_geo, perf_main);
                    statsCounter += static_cast<u32>(items.size());
                    items.clear();
                }
                matPasses.clear();
                map.clear();
            }
        };

        render_matrix_passes(mapMatrixPasses[_priority], RImplementation.BasicStats.DynamicRegularSubmissions);
        if (_priority == 0)
            main_presorted_valid = false; // R3: drawn (or not taken)
        PERF_ADD_IF(perf_main, rmain_dyn_draws, cmd_list.stat.render.calls - perf_draws0);
        PERF_ADD_IF(perf_svp, rsvp_geom_dyn_draws, cmd_list.stat.render.calls - perf_draws0);
        PERF_ADD_IF(perf_main, rmain_dyn_states, cmd_list.stat.states - perf_states0);
        PERF_ADD_IF(perf_main, rmain_dyn_textures, cmd_list.stat.textures - perf_textures0);
        PERF_CB_DELTA(perf_main, rmain_dyn_cb_flush, perf_cb_dyn);
        PERF_CB_BYTES(perf_main, rmain_dyn_cb_bytes, perf_cb_dyn);
    }

#if defined(USE_DX11) && XR_PERF_METRICS
    PERF_ADD_IF(perf_main, rmain_layout_skip, cmd_list.stat.layout_skip - perf_layout_skip0);
    PERF_ADD_IF(perf_main, rmain_layout_set, cmd_list.stat.layout_set - perf_layout_set0);
    PERF_ADD_IF(perf_main, rmain_sampler_skip, cmd_list.stat.sampler_skip - perf_sampler_skip0);
    PERF_ADD_IF(perf_main, rmain_sampler_set, cmd_list.stat.sampler_set - perf_sampler_set0);
    PERF_ADD_IF(perf_main, rmain_cb_flush_slots, cmd_list.stat.cb_flush_slots - perf_cb_flush_slots0);
#endif

    RImplementation.BasicStats.Primitives.End(); // XXX: Refactor a bit later
}

bool R_dsgraph_structure::take_main_presorted(u32 iPass)
{
    if (!main_presorted_valid)
        return false;
    // R3 (27-render-main.md): nothing may have touched the map since the end of the build (nothing inserts into it
    // before part0) - the same nodes, the same item count; otherwise the caller sorts as before.
    auto& groups = main_presorted[iPass];
    mapMatrix_T& map = mapMatrixPasses[0][iPass];
    const uintptr_t first = reinterpret_cast<uintptr_t>(map.begin());
    const uintptr_t last = reinterpret_cast<uintptr_t>(map.end());
    bool same = groups.size() == map.size();
    size_t items = 0;
    for (size_t i = 0; same && i < groups.size(); ++i)
    {
        const uintptr_t node = reinterpret_cast<uintptr_t>(groups[i]);
        same = node >= first && node < last;
        if (same)
            items += groups[i]->second.size();
    }
    if (!same || items != main_presorted_items[iPass])
    {
        PERF_INC(rmain_par_prep_stale);
        groups.clear();
        return false;
    }
    PERF_INC(rmain_par_prep_used);
    matPasses.clear();
    matPasses.swap(groups);
    return true;
}

//////////////////////////////////////////////////////////////////////////
// P19 (d), plans/optimization_mt/19-part0-submit.md: the main view's render_graph(0) recorded in parallel.
//
// The SSA-sorted SPass groups of render_graph(0) (static, then dynamic - the same order) are cut into contiguous units
// of similar recording cost. Unit 0 is recorded by the main thread straight on the immediate context; the following
// units are recorded by task workers (and by the main thread when it gets to one nobody has taken) into pooled D3D11
// deferred contexts, then executed on the immediate context in unit order. The command stream the GPU sees is the
// serial one cut into pieces, so the draw order - and with it every depth/stencil tie - is unchanged.
//
// Recording off the main thread is limited to what only writes into its own backend:
//  - per context: CBackend state cache, constant buffers (per-context pools), transforms, LOD/hemi/tree constants;
//  - CROS (apply_object -> update_smooth writes the object's smoothed lighting once per frame): every object of the
//    graph is refreshed on the main thread before the units start, apply_object in the units then only reads it;
//  - groups whose visuals touch shared immediate-context resources stay on the main thread, inline at their place in
//    the order: particles, LODs, fluid volumes and CPU-skinned meshes fill the shared dynamic VB (Vertex.Lock maps the
//    immediate context), video textures upload through the immediate context, a texture not bound yet loads itself
//    or switches its bind delegate on the first bind.
#if defined(USE_DX11)
namespace
{
constexpr u32 MAIN_PAR_INLINE = u32(-1);
constexpr u32 MAIN_PAR_MAX_SLOTS = R__NUM_PARALLEL_CONTEXTS;
// Recording cost weights for cutting the graph (P19 journal: ~0.4 us per static draw, ~0.9 us per dynamic draw).
constexpr u32 MAIN_PAR_WEIGHT_STATIC = 1;
constexpr u32 MAIN_PAR_WEIGHT_DYNAMIC = 2;

// A run of SSA-sorted items of one SPass group (a whole group before the cut).
struct main_par_piece
{
    SPass* pass;
    const _NormalItem* normal; // static items, or nullptr
    const _MatrixItem* matrix; // dynamic items, or nullptr
    u32 count;
    bool main_only; // drawn by the main thread on the immediate context
};

// Pieces [piece_begin, piece_end), recorded into a deferred slot or inline on the immediate context.
struct main_par_unit
{
    u32 piece_begin;
    u32 piece_end;
    u32 slot; // index into s_main_par_slots, MAIN_PAR_INLINE - the main thread records it on the immediate context
};

// A deferred unit: its pooled context and the sealed command list.
struct main_par_slot
{
    u32 unit{};
    u32 context_id{ R_dsgraph_structure::INVALID_CONTEXT_ID };
    CBackend* cmd{};
    ID3D11CommandList* list{};
    u32 corpses{};
    std::atomic<u32> done{ 0 }; // 1 - recorded and sealed (release; the main thread acquires before executing)
};

// The state render_graph(0) inherits on the immediate context from phase_scene_begin() and the frame setup (G-buffer
// targets, stencil 0x01 marking, cull, color writes, viewport, camera, the last apply_object ambient used by the static
// groups' apply_lmaterial). A deferred context inherits nothing, and the immediate one is at D3D11 defaults after
// ExecuteCommandList(.., FALSE) - both get this seed. Captured on the main thread, read-only for the units.
struct main_par_seed
{
    ref_rt rt0, rt1, rt2, zb; // rt2 empty: two G-buffer targets (o.gbuffer_opt)
    D3D_VIEWPORT viewport{};
    Fmatrix view{}, project{};
    float o_hemi{}, o_sun{};
    float o_hemi_cube[6]{};

    void capture(CBackend& imm)
    {
        // Same targets as CRenderTarget::phase_scene_begin().
        const auto& T = *RImplementation.Target;
        rt0 = T.rt_Position;
        if (!RImplementation.o.gbuffer_opt)
        {
            rt1 = T.rt_Normal;
            rt2 = RImplementation.o.albedo_wo ? T.rt_Accumulator : T.rt_Color;
        }
        else
            rt1 = RImplementation.o.albedo_wo ? T.rt_Accumulator : T.rt_Color;
        zb = T.rt_MSAADepth;

        // The viewport the graph would draw with (rmNormal at the frame start, scaled with r__render_scale).
        UINT count = 1;
        HW.get_context(CHW::IMM_CTX_ID)->RSGetViewports(&count, &viewport);
        if (count == 0)
            viewport = { 0.f, 0.f, float(rt0->dwWidth), float(rt0->dwHeight), 0.f, 1.f };

        view = imm.get_xform_view();
        project = imm.get_xform_project();
        o_hemi = imm.o_hemi;
        o_sun = imm.o_sun;
        static_assert(sizeof(o_hemi_cube) == sizeof(imm.o_hemi_cube));
        CopyMemory(o_hemi_cube, imm.o_hemi_cube, sizeof(o_hemi_cube));
    }

    void apply(CBackend& cmd) const
    {
        auto& T = *RImplementation.Target;
        if (rt2)
            T.u_setrt(cmd, rt0, rt1, rt2, zb);
        else
            T.u_setrt(cmd, rt0, rt1, zb);
        cmd.set_Stencil(TRUE, D3DCMP_ALWAYS, 0x01, 0xff, 0x7f, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP);
        cmd.set_CullMode(CULL_CCW);
        cmd.set_ColorWriteEnable();
        cmd.SetViewport(viewport);
        cmd.set_xform_world(Fidentity);
        cmd.set_xform_view(view);
        cmd.set_xform_project(project);
        cmd.o_hemi = o_hemi;
        cmd.o_sun = o_sun;
        CopyMemory(cmd.o_hemi_cube, o_hemi_cube, sizeof(o_hemi_cube));
    }
};

// Main-thread scratch reused every frame (the pieces point into this frame's dsgraph maps).
xr_vector<main_par_piece> s_main_par_groups;
xr_vector<main_par_piece> s_main_par_pieces;
xr_vector<main_par_unit> s_main_par_units;
main_par_slot s_main_par_slots[MAIN_PAR_MAX_SLOTS];

// A visual whose Render() only records into the backend it is given.
bool main_par_visual_ok(dxRender_Visual* V)
{
    switch (V->Type)
    {
    case MT_NORMAL:
    case MT_PROGRESSIVE:
    case MT_TREE_ST:
    case MT_TREE_PM:
        return true;
    case MT_SKELETON_GEOMDEF_PM:
        return !static_cast<CSkeletonX_PM*>(V)->IsSoftwareSkinned();
    case MT_SKELETON_GEOMDEF_ST:
        return !static_cast<CSkeletonX_ST*>(V)->IsSoftwareSkinned();
    default:
        return false; // particles, LODs (Vertex.Lock), fluid volumes, anything new
    }
}

// The pass's textures can be bound off the main thread: no video upload through the immediate context, and the
// texture is past its first bind (apply_load loads it or switches the bind delegate - not for concurrent callers).
bool main_par_textures_ok(SPass* pass)
{
    const STextureList* list = pass->T._get();
    if (!list)
        return true;
    for (const auto& stage_texture : *list)
    {
        CTexture* texture = stage_texture.second._get();
        if (!texture)
            continue;
        if (texture->pAVI || texture->pTheora)
            return false;
        if (texture->bind == fastdelegate::FastDelegate2<CBackend&, u32>(texture, &CTexture::apply_load))
            return false;
    }
    return true;
}

// Records units' pieces into cmd with the render/main/* counters of render_graph (sums over the units).
void main_par_record(CBackend& cmd, const main_par_unit& unit, u32& corpses)
{
    PIX_EVENT_CTX(cmd, dsgraph_render_graph_part);
#if XR_PERF_METRICS
    const u32 perf_layout_skip0 = cmd.stat.layout_skip;
    const u32 perf_layout_set0 = cmd.stat.layout_set;
    const u32 perf_sampler_skip0 = cmd.stat.sampler_skip;
    const u32 perf_sampler_set0 = cmd.stat.sampler_set;
    const u32 perf_cb_flush_slots0 = cmd.stat.cb_flush_slots;
#endif
    constexpr bool use_fast_geo = false; // PHASE_NORMAL only
    const glod_range_t glod = { r_ssaGLOD_start, r_ssaGLOD_end }; // the main view: always the globals
    // As at the top of render_graph: static items draw with the identity world (the immediate context's first unit
    // follows render_hud, which leaves the last HUD item's transform).
    cmd.set_xform_world(Fidentity);
    for (u32 i = unit.piece_begin; i < unit.piece_end; ++i)
    {
        const main_par_piece& piece = s_main_par_pieces[i];
        if (piece.normal)
        {
            PERF_SCOPE(rmain_submit_static);
            [[maybe_unused]] const u32 perf_draws0 = cmd.stat.render.calls;
            PERF_CB_SNAPSHOT(perf_cb_static);
            render_normal_items(cmd, piece.pass, piece.normal, piece.count, glod, use_fast_geo, true);
            PERF_ADD(rmain_static_draws, cmd.stat.render.calls - perf_draws0);
            PERF_CB_DELTA(true, rmain_static_cb_flush, perf_cb_static);
        }
        else
        {
            PERF_SCOPE(rmain_submit_dynamic);
            [[maybe_unused]] const u32 perf_draws0 = cmd.stat.render.calls;
            [[maybe_unused]] const u32 perf_states0 = cmd.stat.states;
            [[maybe_unused]] const u32 perf_textures0 = cmd.stat.textures;
            PERF_CB_SNAPSHOT(perf_cb_dyn);
#if XR_PERF_METRICS
            xray::perf::ScopedTag perf_dyn_tag(g_perf_rmain_dyn, xray::perf::g_perf_metrics_active ? 1 : 0);
#endif
            corpses += render_matrix_items(cmd, piece.pass, piece.matrix, piece.count, glod, use_fast_geo, true);
            PERF_ADD(rmain_dyn_draws, cmd.stat.render.calls - perf_draws0);
            PERF_ADD(rmain_dyn_states, cmd.stat.states - perf_states0);
            PERF_ADD(rmain_dyn_textures, cmd.stat.textures - perf_textures0);
            PERF_CB_DELTA(true, rmain_dyn_cb_flush, perf_cb_dyn);
            PERF_CB_BYTES(true, rmain_dyn_cb_bytes, perf_cb_dyn);
        }
    }
#if XR_PERF_METRICS
    PERF_ADD(rmain_layout_skip, cmd.stat.layout_skip - perf_layout_skip0);
    PERF_ADD(rmain_layout_set, cmd.stat.layout_set - perf_layout_set0);
    PERF_ADD(rmain_sampler_skip, cmd.stat.sampler_skip - perf_sampler_skip0);
    PERF_ADD(rmain_sampler_set, cmd.stat.sampler_set - perf_sampler_set0);
    PERF_ADD(rmain_cb_flush_slots, cmd.stat.cb_flush_slots - perf_cb_flush_slots0);
#endif
}
} // namespace

bool R_dsgraph_structure::render_graph_main_parallel()
{
    // Only the main view's G-buffer graph on the immediate context (the scope pass, shadows, Z-prefill and the split
    // scene keep render_graph). RenderDoc: its hooks are not safe with deferred recording on several threads (see
    // BeginSecondVPCalculateParallel) - serial, like the scope pass.
    const u32 workers = TaskScheduler ? static_cast<u32>(TaskScheduler->GetWorkersCount()) : 0;
    if (!o.is_main_pass || o.second_vp_pass || o.phase != CRender::PHASE_NORMAL ||
        cmd_list.context_id != CHW::IMM_CTX_ID || workers < 2 || D3DXRenderBase::RenderDocActive())
    {
        PERF_INC(rmain_par_submit_fallback);
        return false;
    }

    // Small graphs: the serial loop is cheaper than the setup.
    u32 total_items = 0;
    for (u32 iPass = 0; iPass < SHADER_PASSES_MAX; ++iPass)
    {
        for (auto* node = mapNormalPasses[0][iPass].begin(); node != mapNormalPasses[0][iPass].end(); ++node)
            total_items += static_cast<u32>(node->second.size());
        for (auto* node = mapMatrixPasses[0][iPass].begin(); node != mapMatrixPasses[0][iPass].end(); ++node)
            total_items += static_cast<u32>(node->second.size());
    }
    if (total_items < static_cast<u32>(std::max(ps_r__main_parallel_submit_min, 0)))
    {
        PERF_INC(rmain_par_submit_fallback);
        return false;
    }

    // One pooled deferred context per deferred unit. At this point the sun cascades (3), rain and the scope worker
    // may hold some of the pool; a short pool just means fewer units.
    const u32 slots_wanted = std::min(static_cast<u32>(std::clamp(ps_r__main_parallel_parts, 2, 12)) - 1, MAIN_PAR_MAX_SLOTS);
    u32 slots = 0;
    for (; slots < slots_wanted; ++slots)
    {
        const u32 id = RImplementation.alloc_context(/*alloc_cmd_list=*/true);
        if (id == INVALID_CONTEXT_ID)
            break;
        main_par_slot& slot = s_main_par_slots[slots];
        slot.context_id = id;
        slot.cmd = &RImplementation.get_context(id).cmd_list;
        slot.list = nullptr;
        slot.corpses = 0;
        slot.done.store(0, std::memory_order_relaxed);
    }
    if (slots < slots_wanted)
        PERF_INC(rmain_par_submit_ctx_short);
    if (slots == 0)
    {
        PERF_INC(rmain_par_submit_fallback);
        return false;
    }

    ZoneScopedN("dsgraph_render_graph_parallel");
    PIX_EVENT_CTX(cmd_list, dsgraph_render_graph);
    PERF_SCOPE(rmain_par_submit_wall);
    RImplementation.BasicStats.Primitives.Begin(); // XXX: Refactor a bit later

    // 1. The groups in render_graph(0) order, sorted the same way; which of them must stay on the main thread; the
    //    once-per-frame CROS refresh of every dynamic object (apply_object in the units only reads it then).
    u32 dynamic_items = 0;
    u32 main_items = 0;
    u64 weight_total = 0;
    s_main_par_groups.clear();
    {
        PERF_SCOPE(rmain_par_submit_prepare);
        for (u32 iPass = 0; iPass < SHADER_PASSES_MAX; ++iPass)
        {
            auto& map = mapNormalPasses[0][iPass];
            map.get_any_p(nrmPasses);
            std::sort(nrmPasses.begin(), nrmPasses.end(), cmp_pass<mapNormal_T::value_type*>);
            for (const auto& it : nrmPasses)
            {
                mapNormalItems& items = it->second;
                items.ssa = 0;
                std::sort(items.begin(), items.end(), cmp_ssa<_NormalItem>);
                bool main_only = !main_par_textures_ok(it->first);
                for (size_t i = 0; i < items.size() && !main_only; ++i)
                    main_only = !main_par_visual_ok(items[i].pVisual);
                s_main_par_groups.push_back({ it->first, items.data(), nullptr, static_cast<u32>(items.size()), main_only });
            }
            nrmPasses.clear();
        }
        for (u32 iPass = 0; iPass < SHADER_PASSES_MAX; ++iPass)
        {
            auto& map = mapMatrixPasses[0][iPass];
            // R3 (27-render-main.md): the groups and their items may come sorted from the
            // build, the CROS of most objects refreshed in its parts (the update_smooth below is then a no-op for them).
            const bool presorted = take_main_presorted(iPass);
            if (!presorted)
            {
                map.get_any_p(matPasses);
                std::sort(matPasses.begin(), matPasses.end(), cmp_pass<mapMatrix_T::value_type*>);
            }
            for (const auto& it : matPasses)
            {
                mapMatrixItems& items = it->second;
                items.ssa = 0;
                if (!presorted)
                    std::sort(items.begin(), items.end(), cmp_ssa<_MatrixItem>);
                bool main_only = !main_par_textures_ok(it->first);
                for (const _MatrixItem& item : items)
                {
                    if (!main_only)
                        main_only = !main_par_visual_ok(item.pVisual);
                    // The same refresh apply_object would do on the first draw of the object this frame.
                    if (item.pObject)
                    {
                        if (auto* ros = static_cast<CROS_impl*>(item.pObject->renderable_ROS()))
                        {
                            PERF_ADD_IF(ros->dwFrameSmooth != Device.dwFrame, rmain_par_submit_smooth_late, 1);
                            ros->update_smooth(item.pObject);
                        }
                    }
                }
                s_main_par_groups.push_back({ it->first, nullptr, items.data(), static_cast<u32>(items.size()), main_only });
                dynamic_items += static_cast<u32>(items.size());
            }
            matPasses.clear();
        }
        main_presorted_valid = false; // R3: taken (or dropped)

        for (const main_par_piece& group : s_main_par_groups)
        {
            if (group.main_only)
                main_items += group.count;
            else
                weight_total += u64(group.count) * (group.matrix ? MAIN_PAR_WEIGHT_DYNAMIC : MAIN_PAR_WEIGHT_STATIC);
        }

        // 2. Cut into units of ~equal weight: the first one is the main thread's own (inline), then the deferred
        //    slots; once they run out, the last deferred unit takes the rest. A main-only group closes the current
        //    unit and becomes an inline unit at its place.
        s_main_par_pieces.clear();
        s_main_par_units.clear();
        const u64 target = std::max<u64>(1, (weight_total + slots) / (slots + 1));
        u32 slots_used = 0;
        bool main_unit_given = false;
        u32 current = MAIN_PAR_INLINE; // index of the open unit
        u64 current_weight = 0;
        for (const main_par_piece& group : s_main_par_groups)
        {
            if (group.main_only)
            {
                current = MAIN_PAR_INLINE;
                const u32 piece = static_cast<u32>(s_main_par_pieces.size());
                s_main_par_pieces.push_back(group);
                s_main_par_units.push_back({ piece, piece + 1, MAIN_PAR_INLINE });
                continue;
            }
            const u64 weight = group.matrix ? MAIN_PAR_WEIGHT_DYNAMIC : MAIN_PAR_WEIGHT_STATIC;
            u32 taken = 0;
            while (taken < group.count)
            {
                if (current == MAIN_PAR_INLINE)
                {
                    u32 slot = MAIN_PAR_INLINE;
                    if (main_unit_given && slots_used < slots)
                        slot = slots_used++;
                    main_unit_given = true;
                    current = static_cast<u32>(s_main_par_units.size());
                    current_weight = 0;
                    const u32 piece = static_cast<u32>(s_main_par_pieces.size());
                    s_main_par_units.push_back({ piece, piece, slot });
                }
                const u64 room = target > current_weight ? target - current_weight : 0;
                const u32 take = static_cast<u32>(std::min<u64>(group.count - taken, std::max<u64>(1, room / weight)));
                main_par_piece piece = group;
                piece.normal = group.normal ? group.normal + taken : nullptr;
                piece.matrix = group.matrix ? group.matrix + taken : nullptr;
                piece.count = take;
                s_main_par_pieces.push_back(piece);
                s_main_par_units[current].piece_end = static_cast<u32>(s_main_par_pieces.size());
                current_weight += u64(take) * weight;
                taken += take;
                // The last deferred unit (no slot left) is never closed: it takes the remainder.
                const bool last_slot_unit = s_main_par_units[current].slot != MAIN_PAR_INLINE && slots_used == slots;
                if (current_weight >= target && !last_slot_unit)
                    current = MAIN_PAR_INLINE;
            }
        }

        // Unused contexts go back to the pool; the used ones learn their unit.
        for (u32 s = slots_used; s < slots; ++s)
            RImplementation.release_context(s_main_par_slots[s].context_id);
        slots = slots_used;
        for (u32 u = 0; u < s_main_par_units.size(); ++u)
        {
            if (s_main_par_units[u].slot != MAIN_PAR_INLINE)
                s_main_par_slots[s_main_par_units[u].slot].unit = u;
        }
    }

    main_par_seed seed;
    seed.capture(cmd_list);

    // 3. Deferred units: claimed in order through `next` by the helper tasks and by the main thread.
    std::atomic<u32> next{ 0 };
    const auto record_slot = [&seed](u32 s)
    {
        main_par_slot& slot = s_main_par_slots[s];
        {
            ZoneScopedN("dsgraph_render_graph_part");
            PERF_SCOPE(rmain_par_submit_record_cpu);
            seed.apply(*slot.cmd);
            main_par_record(*slot.cmd, s_main_par_units[slot.unit], slot.corpses);
            // Sealed here, by the recording thread; the main thread only executes. The next owner of this pooled
            // context resets the backend's state cache (alloc_context -> ResetDeferredCache).
            ID3D11CommandList* list = nullptr;
            const HRESULT hr = HW.get_context(slot.context_id)->FinishCommandList(FALSE, &list);
            if (FAILED(hr) || !list)
            {
                FATAL_F("FinishCommandList failed on context %u (hr=0x%08x) - parallel main submit aborted",
                    slot.context_id, u32(hr));
            }
            slot.list = list;
        }
        slot.done.store(1, std::memory_order_release);
    };
    const auto run_slots = [&next, &record_slot, slots]
    {
        for (;;)
        {
            const u32 s = next.fetch_add(1, std::memory_order_relaxed);
            if (s >= slots)
                break;
            record_slot(s);
        }
    };

    Task& root = TaskScheduler->CreateTask([] {});
    const u32 helpers = std::min(slots, workers - 1);
    for (u32 h = 0; h < helpers; ++h)
        TaskScheduler->AddTask(root, [&run_slots] { run_slots(); });

    // 4. On the main thread, in unit order: record the inline units on the immediate context, execute the sealed
    //    lists. ExecuteCommandList(.., FALSE) leaves the immediate context at D3D11 defaults: before the next inline
    //    unit and before returning (lods and details follow) its backend cache is dropped and the seed re-applied
    //    (lesson f756f8939: never reuse a cache across an executed list).
    u32 corpses = 0;
    bool imm_dirty = false;
    const auto reseed_immediate = [&]
    {
        PERF_SCOPE(rmain_par_submit_execute);
        cmd_list.Invalidate();
        seed.apply(cmd_list);
        imm_dirty = false;
    };
    for (const main_par_unit& unit : s_main_par_units)
    {
        if (unit.slot == MAIN_PAR_INLINE)
        {
            if (imm_dirty)
                reseed_immediate();
            PERF_SCOPE(rmain_par_submit_record_cpu);
            PERF_SCOPE(rmain_par_submit_main_unit); // 27-render-main.md (M): the main thread's own units only
            main_par_record(cmd_list, unit, corpses);
            continue;
        }

        main_par_slot& slot = s_main_par_slots[unit.slot];
        while (!slot.done.load(std::memory_order_acquire))
        {
            // Take the units nobody has started yet (lowest first, so this one comes first); then wait for the ones
            // running on workers. Spin, no stealing: TaskManager::Wait() could pick up an unrelated long task.
            const u32 s = next.fetch_add(1, std::memory_order_relaxed);
            if (s < slots)
            {
                record_slot(s);
                continue;
            }
            PERF_SCOPE(rmain_par_submit_tail);
            while (!slot.done.load(std::memory_order_acquire))
                std::this_thread::yield();
        }
        {
            PERF_SCOPE(rmain_par_submit_execute);
            HW.get_context(CHW::IMM_CTX_ID)->ExecuteCommandList(slot.list, FALSE);
            _RELEASE(slot.list);
        }
        imm_dirty = true;
        corpses += slot.corpses;
        RImplementation.release_context(slot.context_id);
    }
    if (imm_dirty)
        reseed_immediate();

    // Helpers still queued find no units and return at once; Wait() runs them first from this thread's queue.
    TaskManager::RunTask(root);
    TaskScheduler->Wait(root);

    // 5. Empty the maps as render_graph does (keeping their memory).
    for (u32 iPass = 0; iPass < SHADER_PASSES_MAX; ++iPass)
    {
        auto& normal = mapNormalPasses[0][iPass];
        for (auto* node = normal.begin(); node != normal.end(); ++node)
            node->second.clear();
        normal.clear();
        auto& matrix = mapMatrixPasses[0][iPass];
        for (auto* node = matrix.begin(); node != matrix.end(); ++node)
            node->second.clear();
        matrix.clear();
    }
    RImplementation.BasicStats.DynamicRegularSubmissions += dynamic_items;
    RImplementation.BasicStats.CorpseRegularSubmissions += corpses;
    PERF_ADD(rmain_par_submit_parts, s_main_par_units.size());
    PERF_ADD(rmain_par_submit_lists, slots);
    PERF_ADD(rmain_par_submit_main_items, main_items);
    s_main_par_groups.clear();
    s_main_par_pieces.clear();
    s_main_par_units.clear();

    RImplementation.BasicStats.Primitives.End(); // XXX: Refactor a bit later
    return true;
}
#else
bool R_dsgraph_structure::render_graph_main_parallel() { return false; }
#endif

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
    // 18-combine-pass.md (E3): the build already drops empty effects (add_leafs_dynamic); this catches the ones whose
    // last particles died on the PreRender particle worker since. Render() would draw nothing - skip its state setup too.
    if (V->Type == MT_PARTICLE_EFFECT && static_cast<PS::CParticleEffect*>(V)->IsEmptyForRender())
    {
        PERF_ADD_IF(g_perf_combine_main != 0, rcomb_ps_empty, 1);
        return;
    }
    dsgraph.cmd_list.set_Element(item.second.se);
    dsgraph.cmd_list.set_xform_world(item.second.Matrix);
    RImplementation.apply_object(dsgraph.cmd_list, item.second.pObject);
    dsgraph.cmd_list.apply_lmaterial();
    hud_transform_helper::apply_custom_state(dsgraph.cmd_list);
    //--#SM+#-- Обновляем шейдерные данные модели [update shader values for this model]
    //RCache.hemi.c_update(V);
    V->Render(dsgraph.cmd_list, calcLOD(item.first, V->vis.sphere.R, pass_glod(dsgraph.o)),
        dsgraph.o.phase == CRender::PHASE_SMAP);
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

    {
        // C1.0: a call into the game from the submit (plans/optimization_mt/17-c1-semi-pipeline.md, §2)
        ZoneScopedN("render_hud_ui/RenderActiveItemUI");
        PERF_SCOPE(rhud_active_item_ui);
        levelHud->RenderActiveItemUI();
    }
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
// 27-render-main.md (M, R1): mapSorted of the main view's combine.
namespace
{
using sorted_node = mapSorted_T::value_type;

// mapSorted in the order traverse_right_left visits it (back to front). Main thread only.
xr_vector<sorted_node*> s_sorted_order;

// render_item with render/combine/sorted_other around the non-particle items (particles time themselves:
// render/combine/ps_effect).
template <class T>
void __fastcall render_item_sorted_main(u32 context_id, const T& item)
{
    if (item.second.pVisual->Type == MT_PARTICLE_EFFECT)
    {
        render_item(context_id, item);
        return;
    }
    PERF_SCOPE_IF(g_perf_combine_main != 0, rcomb_sorted_other);
    render_item(context_id, item);
}

#if XR_PERF_METRICS
// render/combine/ps_adjacent_same - neighbours in the drawing order that one draw could take (sprite effects with the
// same ShaderElement and cull mode, nothing drawn between; effects render_item skips as empty do not separate them),
// render/combine/ps_hud - HUD-mode effects (they switch the projection, always drawn by Render()).
void sorted_ps_stats(const xr_vector<sorted_node*>& order)
{
    const _MatrixItemS* prev = nullptr;
    u32 prev_cull = 0;
    u32 adjacent = 0;
    u32 hud = 0;
    for (sorted_node* node : order)
    {
        const _MatrixItemS& item = node->second;
        if (item.pVisual->Type != MT_PARTICLE_EFFECT)
        {
            prev = nullptr;
            continue;
        }
        auto* effect = static_cast<PS::CParticleEffect*>(item.pVisual);
        if (effect->IsEmptyForRender())
            continue;
        if (effect->GetHudMode())
        {
            ++hud;
            prev = nullptr;
            continue;
        }
        const u32 cull = effect->BatchCullMode();
        if (prev && prev->se == item.se && prev_cull == cull)
            ++adjacent;
        prev = &item;
        prev_cull = cull;
    }
    PERF_ADD(rcomb_ps_adjacent_same, adjacent);
    PERF_ADD(rcomb_ps_hud, hud);
}
#endif

#if defined(USE_DX11)
// One draw of QuadIB takes at most this many sprites (D3DXRenderBase::CreateQuadIB).
constexpr u32 PS_BATCH_QUADS_MAX = 4 * 1024;
// Fewer particles in the batch - filled by the main thread alone.
constexpr u32 PS_BATCH_PARALLEL_MIN = 2048;
constexpr u32 PS_BATCH_CHUNKS_MAX = 8;

// A mapSorted node in drawing order; effect - its prepared particle effect (vertices at first_vertex of the batch),
// nullptr - render_item draws the node.
struct ps_batch_item
{
    sorted_node* node{};
    PS::CParticleEffect* effect{};
    PS::CParticleEffect::BatchSnapshot snapshot{};
    u32 first_vertex{};
};
xr_vector<ps_batch_item> s_ps_items;
xr_vector<u32> s_ps_prepared; // indices into s_ps_items of the prepared effects, in order

bool ps_no_ros(IRenderable* object) { return !object || !object->renderable_ROS(); }

// The sprites of all prepared effects into pv (the batch's first vertex): particle ranges of ~equal size claimed by
// the main thread and task workers; no stealing while waiting (see render_graph_main_parallel).
void ps_batch_fill(FVF::LIT* pv, u32 total_particles, [[maybe_unused]] bool perf)
{
    const auto fill_range = [pv](u32 range_begin, u32 range_end)
    {
        // The prepared effect holding range_begin, then on while they overlap the range.
        auto it = std::upper_bound(s_ps_prepared.begin(), s_ps_prepared.end(), range_begin,
            [](u32 particle, u32 index) { return particle < s_ps_items[index].first_vertex / 4; });
        VERIFY(it != s_ps_prepared.begin());
        --it;
        for (; it != s_ps_prepared.end(); ++it)
        {
            const ps_batch_item& entry = s_ps_items[*it];
            const u32 first = entry.first_vertex / 4;
            if (first >= range_end)
                break;
            const u32 begin = std::max(range_begin, first) - first;
            const u32 end = std::min(range_end, first + entry.snapshot.count) - first;
            if (begin < end)
                entry.effect->BatchFill(pv + entry.first_vertex, entry.snapshot, begin, end);
        }
    };

    const u32 workers = static_cast<u32>(TaskScheduler->GetWorkersCount());
    const u32 chunks = std::min({ total_particles / PS_BATCH_PARALLEL_MIN, workers, PS_BATCH_CHUNKS_MAX });
    if (chunks < 2)
    {
        PERF_SCOPE_IF(perf, rcomb_ps_prefill_cpu);
        fill_range(0, total_particles);
        return;
    }

    std::atomic<u32> next{ 0 };
    std::atomic<u32> done{ 0 };
    const auto run_chunks = [&]
    {
        for (;;)
        {
            const u32 c = next.fetch_add(1, std::memory_order_relaxed);
            if (c >= chunks)
                break;
            {
                PERF_SCOPE_IF(perf, rcomb_ps_prefill_cpu);
                fill_range(static_cast<u32>(u64(total_particles) * c / chunks),
                    static_cast<u32>(u64(total_particles) * (c + 1) / chunks));
            }
            done.fetch_add(1, std::memory_order_release);
        }
    };
    Task& root = TaskScheduler->CreateTask([] {});
    const u32 helpers = std::min(chunks - 1, workers - 1);
    for (u32 h = 0; h < helpers; ++h)
        TaskScheduler->AddTask(root, [&run_chunks] { run_chunks(); });
    run_chunks();
    while (done.load(std::memory_order_acquire) < chunks)
        std::this_thread::yield();
    // Helpers still queued find no chunks and return at once; Wait() runs them first from this thread's queue.
    TaskManager::RunTask(root);
    TaskScheduler->Wait(root);
}
#endif
} // namespace

// R1 (27-render-main.md; mode 2 always on since 2026-10-18, 41-cvar-cleanup.md). mapSorted is drawn in the order of the plain loop (back to front);
// what changes is where the sprite vertices of the particle effects come from:
//  1. one pass over the order snapshots every drawable sprite effect (one read of its particle count, no pm_lock);
//     HUD-mode effects (they switch the projection), empty ones and the rest stay with render_item;
//  2. one Lock of the dynamic stream for all of them, the sprites filled in parallel into consecutive ranges, Unlock -
//     instead of a Lock/fill/Unlock per effect in between the draws;
//  3. the draws in order: per effect the state render_item sets (element, world, object ambient, lmaterial), then
//     the tail of CParticleEffect::Render for its range. A run of such effects adjacent in the
//     order, with the same ShaderElement and cull mode and owners without ROS (so apply_object leaves the ambient
//     alone and apply_lmaterial sets the same values - they depend on the element's texture only) becomes one
//     DrawIndexed: the sprite shaders read only m_WVP / m_WV (the world matrix is identity for every sprite),
//     L_material.w and the frame constants; the primitives keep their order inside the draw.
// A Lock that wraps the stream (Map DISCARD) after the fill - by an item drawn in between - drops the prepared
// vertices: the effects after it go back to Render(), which fills them again (render/combine/ps_fallback).
// The particles are read while the PreRender worker may still be stepping them (render/combine/ps_sim_late), as
// Render() reads them now: earlier in the pass, still inside the max_particles array.
bool R_dsgraph_structure::render_sorted_ps_batch()
{
#if defined(USE_DX11)
    if (mapSorted.empty() || cmd_list.context_id != CHW::IMM_CTX_ID || !TaskScheduler ||
        D3DXRenderBase::RenderDocActive())
    {
        return false;
    }
    ZoneScopedN("dsgraph_render_sorted_ps_batch");
    [[maybe_unused]] const bool perf = g_perf_combine_main != 0;

    s_sorted_order.clear();
    mapSorted.get_right_left_p(s_sorted_order);
#if XR_PERF_METRICS
    if (perf)
        sorted_ps_stats(s_sorted_order);
#endif

    // 1. The order and the snapshots. Half of the stream at most, so the batch never fills it alone.
    constexpr u32 stride = sizeof(FVF::LIT);
    const u32 vertex_cap = RImplementation.Vertex.GetSize() / stride / 2;
    u32 total_vertices = 0;
    u32 fallback = 0;
    s_ps_items.clear();
    s_ps_prepared.clear();
    for (sorted_node* node : s_sorted_order)
    {
        ps_batch_item entry;
        entry.node = node;
        dxRender_Visual* V = node->second.pVisual;
        if (V->Type == MT_PARTICLE_EFFECT)
        {
            auto* effect = static_cast<PS::CParticleEffect*>(V);
            if (!effect->IsEmptyForRender() && effect->BatchPrepare(entry.snapshot))
            {
                const u32 vertices = entry.snapshot.count * 4;
                if (total_vertices + vertices <= vertex_cap)
                {
                    VERIFY(effect->geom->vb_stride == stride);
                    entry.effect = effect;
                    entry.first_vertex = total_vertices;
                    total_vertices += vertices;
                    s_ps_prepared.push_back(static_cast<u32>(s_ps_items.size()));
                }
                else
                    ++fallback; // does not fit: Render() locks its own range
            }
        }
        s_ps_items.push_back(entry);
    }

    // 2. One lock, the fill, one unlock.
    u32 base_vertex = 0;
    u32 discard_id = 0;
    if (total_vertices)
    {
        PERF_SCOPE_IF(perf, rcomb_ps_prefill);
        PERF_ADD_IF(perf && Device.m_pre_render_task && !Device.m_pre_render_task->IsFinished(), rcomb_ps_sim_late, 1);
        auto* pv = static_cast<FVF::LIT*>(RImplementation.Vertex.Lock(total_vertices, stride, base_vertex));
        discard_id = RImplementation.Vertex.DiscardID();
        ps_batch_fill(pv, total_vertices / 4, perf);
        RImplementation.Vertex.Unlock(total_vertices, stride);
        PERF_ADD_IF(perf, rcomb_ps_prepared, s_ps_prepared.size());
        PERF_ADD_IF(perf, rcomb_ps_sprites, total_vertices / 4);
    }

    // 3. The draws, back to front.
    u32 runs = 0;
    u32 run_max = 0;
    const size_t count = s_ps_items.size();
    for (size_t i = 0; i < count;)
    {
        const ps_batch_item& entry = s_ps_items[i];
        if (!entry.effect)
        {
            render_item_sorted_main(context_id, *entry.node);
            ++i;
            continue;
        }
        if (RImplementation.Vertex.DiscardID() != discard_id)
        {
            ++fallback; // the stream wrapped since the fill: the prepared vertices are gone
            render_item(context_id, *entry.node);
            ++i;
            continue;
        }

        // render_item's state for this effect
        const _MatrixItemS& item = entry.node->second;
        cmd_list.set_Element(item.se);
        cmd_list.set_xform_world(item.Matrix);
        RImplementation.apply_object(cmd_list, item.pObject);
        cmd_list.apply_lmaterial();
        hud_transform_helper::apply_custom_state(cmd_list);

        u32 vertex_count = entry.snapshot.count * 4;
        u32 effects = 1;
        size_t j = i + 1;
        if (ps_no_ros(item.pObject))
        {
            const u32 cull = entry.effect->BatchCullMode();
            for (; j < count; ++j)
            {
                const ps_batch_item& other = s_ps_items[j];
                if (!other.effect)
                {
                    // An effect emptied since the build: render_item would return before touching any state.
                    dxRender_Visual* V = other.node->second.pVisual;
                    if (V->Type == MT_PARTICLE_EFFECT && static_cast<PS::CParticleEffect*>(V)->IsEmptyForRender())
                    {
                        PERF_ADD_IF(perf, rcomb_ps_empty, 1);
                        continue;
                    }
                    break;
                }
                const u32 other_vertices = other.snapshot.count * 4;
                if (other.node->second.se != item.se || other.effect->BatchCullMode() != cull ||
                    other.first_vertex != entry.first_vertex + vertex_count ||
                    (vertex_count + other_vertices) / 4 > PS_BATCH_QUADS_MAX || !ps_no_ros(other.node->second.pObject))
                {
                    break;
                }
                vertex_count += other_vertices;
                ++effects;
            }
        }
        entry.effect->BatchDraw(cmd_list, base_vertex + entry.first_vertex, vertex_count);
        ++runs;
        run_max = std::max(run_max, effects);
        i = j;
    }
    PERF_ADD_IF(perf, rcomb_ps_runs, runs);
    PERF_ADD_IF(perf, rcomb_ps_run_max, run_max);
    PERF_ADD_IF(perf, rcomb_ps_fallback, fallback);

    mapSorted.clear();
    s_ps_items.clear();
    s_ps_prepared.clear();
    s_sorted_order.clear();
    return true;
#else
    return false; // GL: the plain loop
#endif
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void R_dsgraph_structure::render_sorted(bool main_combine)
{
    ZoneScoped;
    PIX_EVENT_CTX(cmd_list, dsgraph_render_sorted);

    if (!main_combine)
        sort_back_to_front_render_and_clean(context_id, mapSorted);
    else if (!render_sorted_ps_batch())
    {
#if XR_PERF_METRICS
        if (g_perf_combine_main && !mapSorted.empty())
        {
            s_sorted_order.clear();
            mapSorted.get_right_left_p(s_sorted_order);
            sorted_ps_stats(s_sorted_order);
            s_sorted_order.clear();
        }
#endif
        mapSorted.traverse_right_left(context_id, render_item_sorted_main);
        mapSorted.clear();
    }

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

// Local to this file (unity builds glue several .cpp into one TU).
#undef PERF_CB_SNAPSHOT
#undef PERF_CB_DELTA
#undef PERF_CB_BYTES
