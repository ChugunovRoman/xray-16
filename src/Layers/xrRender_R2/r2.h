#pragma once

#include <atomic>
#include <thread>

#include "xrCore/Threading/Event.hpp"
#include "xrEngine/device.h"

#include "Layers/xrRender/D3DXRenderBase.h"
#include "Layers/xrRender/r__occlusion.h"
#include "Layers/xrRender/r__sync_point.h"

#include "Layers/xrRender/PSLibrary.h"

#include "r2_types.h"

#include "Layers/xrRender/HOM.h"
#include "Layers/xrRender/DetailManager.h"
#include "Layers/xrRender/ModelPool.h"
#include "Layers/xrRender/WallmarksEngine.h"
#include "Layers/xrRender/SH_RT.h"

#include "SMAP_Allocator.h"
#include "Layers/xrRender/Light_DB.h"
#include "Layers/xrRender/Light_Render_Direct.h"
#include "Layers/xrRender/LightTrack.h"
#include "Layers/xrRender/r_sun_cascades.h"
#include "xrCommon/xr_map.h"

#include "xrEngine/IRenderable.h"
#include "xrCore/Threading/TaskManager.hpp"
#include "xrCore/FMesh.hpp"

namespace xray::render::RENDER_NAMESPACE
{
class CRenderTarget;
class dxRender_Visual;
class CGlowManager;
class CPreviewSceneRenderer;

// Camera snapshot for the scope-viewport visibility pass. Captured on the main thread BEFORE the
// worker task is pushed, so calculate_for never reads Device.* concurrently with the main render.
// Values mirror what the sequential second pass used to see: BeginSecondViewportRender mutates
// only fFOV/mProject, leaving view_pos/full_transform/ViewBase untouched.
struct SRVPCalcParams
{
    Fmatrix full_transform;
    CFrustum view_frustum;
    Fvector view_pos;
    float fov{};
    u32 width{};
    u32 height{};
    u32 sector_id{};
};

// TODO: move it into separate file.
struct i_render_phase
{
    explicit i_render_phase(const xr_string& name_in)
        : name(name_in)
    {
        o.active = false;
        o.mt_calc_enabled = false;
        o.mt_draw_enabled = false;
    }

    virtual ~i_render_phase() = default;

    ICF void run()
    {
        if (!o.active)
            return;

        main_task = &TaskScheduler->CreateTask([this]
        {
            calculate();

            if (o.mt_draw_enabled)
            {
                draw_task = &TaskScheduler->AddTask(*main_task, [this]
                {
                    render();
                });
            }
        });

        if (o.mt_calc_enabled)
        {
            TaskScheduler->PushTask(*main_task);
        }
        else
        {
            TaskScheduler->RunTask(*main_task);
        }
    }

    ICF void sync()
    {
        if (main_task)
            TaskScheduler->Wait(*main_task);
        main_task = nullptr;

        if (o.mt_draw_enabled && draw_task)
        {
            // The draw subtask records into the phase's DEFERRED contexts; flush() below
            // submits those lists and records the accumulation on the same contexts. It MUST
            // be finished first - the old VERIFY was a no-op in release builds, leaving a race
            // (partially recorded depth lists + concurrent recording into the same context).
            TaskScheduler->Wait(*draw_task);
            draw_task = nullptr;
        }
        else
        {
            render();
        }

        flush();

        o.active = false;
    }

    virtual void init() = 0;
    virtual void calculate() = 0;
    virtual void render() = 0;
    virtual void flush() {}

    struct options_t
    {
        u32 active : 1;
        u32 mt_calc_enabled : 1;
        u32 mt_draw_enabled : 1;
    } o;
    Task* main_task{ nullptr };
    Task* draw_task{ nullptr };
    xr_string name{ "<UNKNOWN>" };
};

struct render_main : public i_render_phase
{
    render_main() : i_render_phase("main_render") {}

    void init() override;
    void calculate() override;
    // Stage C: explicit-target build - an ASYNC task must never re-read mutable "current target"
    // state (override pointers) at execution time, or two passes can land in the same dsgraph.
    void calculate_into(R_dsgraph_structure& ds, const SRVPCalcParams* params);
    // Creates+dispatches the build task; the task handle lands in *sink (defaults to the phase's
    // own main_task slot - the caller's sync() then joins exactly this build).
    void launch_build(R_dsgraph_structure& ds, const SRVPCalcParams* params, Task** sink = nullptr);
    void render() override;
};

struct render_rain : public i_render_phase
{
    render_rain() : i_render_phase("rain_render") {}

    void init() override;
    void calculate() override;
    void render() override;
    void flush() override;

    light RainLight;
    u32 context_id{ R_dsgraph_structure::INVALID_CONTEXT_ID };
    float rain_factor{ 0.0f };
};

struct render_sun : public i_render_phase
{
    render_sun() : i_render_phase("sun_render") {}

    void init() override;
    void calculate() override;
    void render() override;
    void flush() override;

    void accumulate_cascade(u32 cascade_ind);

    sun::cascade m_sun_cascades[R__NUM_SUN_CASCADES];
    light* sun{ nullptr };
    bool need_to_render_sunshafts{ false };
    bool last_cascade_chain_mode{ false };

    u32 contexts_ids[R__NUM_SUN_CASCADES];
};

struct render_sun_old : public i_render_phase
{
    render_sun_old() : i_render_phase("sun_render_old") {}

    void init() override;
    void calculate() override {}
    void render() override;
    void flush() override;

    void render_sun();
    void render_sun_near();
    void render_sun_filtered() const;

    xr_vector<sun::cascade> m_sun_cascades;
    xr_vector<Fbox> s_casters;
    light* sun{ nullptr };
    u32 context_id{ R_dsgraph_structure::INVALID_CONTEXT_ID };
};
//----

class CGlowManager
{
public:
    CGlowManager() = default;
    ~CGlowManager() = default;

    void Destroy();
    void Register(IRender_Glow* glow);
    void Unregister(IRender_Glow* glow);
    void Render();
    bool Empty() const { return m_active.empty(); }

private:
    void Initialize();

private:
    xr_vector<IRender_Glow*> m_active;
    ref_geom m_hGeom;
};

// definition
class CRender final : public D3DXRenderBase
{
public:
    friend class CPreviewSceneRenderer;

    enum
    {
        PHASE_NORMAL = 0, // E[0]
        PHASE_SMAP = 1, // E[1]
    };

    enum
    {
        MSAA_ATEST_NONE = 0x0, //	Hi bit - DX10.1 mode
        MSAA_ATEST_DX10_0_ATOC = 0x1, //	Lo bit - ATOC mode
        MSAA_ATEST_DX10_1_NATIVE = 0x2,
        MSAA_ATEST_DX10_1_ATOC = 0x3,
    };

    enum
    {
        MMSM_OFF = 0,
        MMSM_ON,
        MMSM_AUTO,
        MMSM_AUTODETECT
    };

public:
    struct _options
    {
        u32 bug : 1;

        u32 ssao_blur_on : 1;
        u32 ssao_opt_data : 1;
        u32 ssao_half_data : 1;
        u32 ssao_hbao : 1;
        u32 ssao_hdao : 1;
        u32 ssao_ultra : 1;
        u32 hbao_vectorized : 1;

        u32 rain_smapsize : 16;
        u32 smapsize : 16;
        u32 depth16 : 1;
        u32 mrt : 1;
        u32 mrtmixdepth : 1;
        u32 fp16_filter : 1;
        u32 fp16_blend : 1;
        u32 albedo_wo : 1; // work-around albedo on less capable HW
        u32 HW_smap : 1;
        u32 HW_smap_PCF : 1;
        u32 HW_smap_FETCH4 : 1;

        u32 HW_smap_FORMAT : 32;

        u32 nvstencil : 1;
        u32 nvdbt : 1;

        u32 nullrt : 1;
        u32 ffp : 1; // don't use shaders, only fixed-function pipeline or software processing

        u32 distortion : 1;
        u32 distortion_enabled : 1;
        u32 mblur : 1;

        u32 sunfilter : 1;
        u32 sunstatic : 1;
        u32 sjitter : 1;
        u32 noshadows : 1;
        u32 Tshadows : 1; // transluent shadows
        u32 oldshadowcascades : 1;
        u32 disasm : 1;
        u32 advancedpp : 1; //	advanced post process (DOF, SSAO, volumetrics, etc.)
        u32 volumetricfog : 1;

        u32 msaa : 1; // DX10.0 path
        u32 msaa_hybrid : 1; // DX10.0 main path with DX10.1 A-test msaa allowed
        u32 msaa_opt : 1; // DX10.1 path
        u32 gbuffer_opt : 1;
        u32 dx11_sm4_1 : 1; // DX10.1 path
        u32 msaa_alphatest : 2; //	A-test mode
        u32 msaa_samples : 4;

        u32 minmax_sm : 2;
        u32 minmax_sm_screenarea_threshold;

        u32 tessellation : 1;

        u32 forcegloss : 1;
        u32 forceskinw : 1;

        u32 mt_calculate : 1;
        u32 mt_render : 1;

        u32 support_rt_arrays : 1;

        float forcegloss_v;
    } o;

    struct RenderR2Statistics
    {
        u32 l_total;
        u32 l_visible;
        u32 l_shadowed;
        u32 l_unshadowed;
        s32 s_used;
        s32 s_merged;
        s32 s_finalclip;
        u32 ic_total;
        u32 ic_culled;

        RenderR2Statistics() { FrameStart(); }
        void FrameStart()
        {
            l_total = 0;
            l_visible = 0;
            l_shadowed = 0;
            l_unshadowed = 0;
            s_used = 0;
            s_merged = 0;
            s_finalclip = 0;
            ic_total = 0;
            ic_culled = 0;
        }

        void FrameEnd() {}
    };

public:
    RenderR2Statistics Stats;
    // Sector detection and visibility
    IRender_Sector::sector_id_t last_sector_id{IRender_Sector::INVALID_SECTOR_ID};
    u32 uLastLTRACK;
    xrXRC Sectors_xrc;
    CDB::MODEL* rmPortals;
    CHOM HOM;
    Task* ProcessHOMTask;
    R_occlusion HWOCC;

    // Global vertex-buffer container
    xr_vector<FSlideWindowItem> SWIs;
    xr_vector<ref_shader> Shaders;
    typedef svector<VertexElement, MAXD3DDECLLENGTH + 1> VertexDeclarator;
    xr_vector<VertexDeclarator> nDC, xDC;
    xr_vector<VertexStagingBuffer> nVB, xVB;
    xr_vector<IndexStagingBuffer> nIB, xIB;
    xr_vector<dxRender_Visual*> Visuals;
    CPSLibrary PSLibrary;

    CDetailManager* Details;
    CModelPool* Models;
    CWallmarksEngine* Wallmarks;
    CGlowManager* Glows{};

    CRenderTarget* Target; // Render-target

    CLight_DB Lights;
    CLight_Compute_XFORM_and_VIS LR;
    xr_vector<light*> Lights_LastFrame;
    SMAP_Allocator LP_smap_pool;
    // Frame driver: scope-pass shadow packer, isolated from LP_smap_pool - the worker records
    // lighting into the dedicated $user$sv_smap_depth atlas while the main pass owns its own.
    SMAP_Allocator svp_LP_smap_pool;
    // Frame driver stage 1c results, filled by the worker (record_second_vp_shadows) and
    // consumed once per frame by svp_accumulate_prebuilt: per-page light groups (page i pairs
    // with svp_smap_replay_lists[i]) and lights that passed the culls but not the page cap
    // (accumulated UNSHADOWED - light without shadow lookup, instead of going dark).
    // Per-page grouping is REQUIRED: every page list starts with a full atlas clear, so a
    // page's shadows survive only until the next page list replays - each page's lights must
    // be accumulated before the next page executes.
    xr_vector<xr_vector<light*>> svp_shadow_page_lights;
    xr_vector<light*> svp_shadow_unshadowed;
    light_Package LP_normal;
    light_Package LP_pending;
    // Frame driver stage 1c: PRE-FILTERED copies filled by the main pass right before it
    // unblocks the worker's shadow build (record_second_vp_shadows reads their union); the
    // scope pass then accumulates from and consumes these very vectors (one per render_lights
    // call). Kept as members because they bridge two threads and two Render() invocations.
    light_Package LP_svp_normal;
    light_Package LP_svp_pending;

    xr_vector<Fbox3> main_coarse_structure;

    R_sync_point q_sync_point;

    // Dedicated dsgraph context for the SVP (scope) visibility pass. Allocated per SVP frame with an
    // immediate cmd_list (alloc_context(false): GPU work stays on the single sequential stream),
    // isolating the visibility maps/markers so Calculate₂ can later run off-thread.
    // Lifecycle: svp_context_id/svp_dsgraph are invalidated at the start of every main-pass
    // calculate_for() and released after the SVP inner Render drains them.
    u32 svp_context_id{ R_dsgraph_structure::INVALID_CONTEXT_ID };
    R_dsgraph_structure* svp_dsgraph{};
    // When set, render_main::calculate() and CRender::Render() drain this dsgraph instead of the
    // immediate one. Only non-null between the second calculate_for() and the end of its Render.
    R_dsgraph_structure* r_main_dsgraph_override{};
    // Stage C: worker-built visibility for the scope viewport.
    SRVPCalcParams svp_calc_params;
    const SRVPCalcParams* r_main_calc_params{}; // consumed by render_main::calculate()
    bool svp_parallel{}; // this frame's Calculate₂ was pushed to a worker
    // P2.3: sealed smap command lists from the main pass flush_lights, re-executed by the scope
    // pass (shadow map content is camera-independent — same light, same frame). DX11 only;
    // released after the scope pass accumulation or at the end of a non-svp frame.
    xr_vector<void*> svp_smap_replay_lists;
    // P2.3: the dedicated thread builds visibility AND records render_graph(0) into the deferred
    // cmd list, overlapping the main render's tail. Seeded on the main thread at launch.
    Fmatrix svp_seed_view{};
    Fmatrix svp_seed_project{};
    bool svp_cmd_deferred{}; // svp dsgraph cmd_list is a deferred context this frame
    bool svp_geom_on_main{};
    // Frame driver: resolved per accepted SVP frame on the main thread; gates the worker stages
    // beyond geometry recording. Reset with the other per-frame svp fields at the top of calculate_for().
    bool svp_frame_driver{};
    // Cooperative abort checked by the worker thread between stages; set by AbortSecondVPCalculate.
    std::atomic<bool> svp_build_abort{ false };
    // Frame driver stage C sync: the MAIN pass sets it once its own lighting drained - from that
    // moment the context pool and the shared light objects belong to the worker (the pool has no
    // internal locking, so concurrent alloc_context calls are forbidden).
    Event svp_lights_go;
    // Roadmap A.2 (shadow-transfer): snapshot of r__svp_shadow_transfer, resolved per accepted
    // SVP frame in BeginSecondVPCalculateParallel. When set, the MAIN pass copies every smap
    // page into a dedicated SVP atlas slice right after the page's flush (see render_lights),
    // the worker skips its shadow build (stage C) entirely, and the scope pass accumulates
    // against those copies using the main pass's X.S placements (they stay valid all frame).
    bool svp_shadow_transfer{};
    // Scope shadow prebuild state: 0 = off/legacy path, 1 = worker lists await replay,
    // 2 = replay consumed (the second filtered package of the frame accumulates w/o rebuilding),
    // 3 = shadow-transfer: pages were copied into SVP atlas slices by the main pass, the scope
    // pass accumulates them per-slice without replaying any sealed command lists.
    u8 svp_shadow_stage{};
    // Stage 2 sun-reuse (r__svp_sun_mode 1): set by the MAIN pass right after it copied the sun
    // cascade slices into the SVP atlas tail (Render/Sun); consumed by the scope pass Render/Sun
    // to accumulate the reused cascades. False = no sun this frame (night) or copy failed.
    bool svp_sun_slices_ready{};
    // Deferred command lists exist only on DX11: GL renders the scope pass inline (single GL
    // context, nothing to record into), so every frame-driver stage is gated on this.
    static constexpr bool svp_frame_driver_available()
    {
#ifdef USE_DX11
        return true;
#else
        return false;
#endif
    }

    bool m_bFirstFrameAfterReset{}; // Determines weather the frame is the first after resetting device.

    bool m_fast_geom_loaded{};

private:
    // Loading / Unloading
    void LoadBuffers(CStreamReader* fs, bool alternative);
    void LoadVisuals(IReader* fs);
    void LoadLights(IReader* fs);
    void LoadSectors(IReader* fs);
    void LoadSWIs(CStreamReader* fs);
#if RENDER != R_R2
    void Load3DFluid();
#endif

public:
    // Visibility calculation for one camera pass. second_pass=true runs the dedicated
    // scope-viewport calculation: shared per-frame work (light collection, sector detection +
    // game callback, global option writes, culling stats) is skipped so this pass can later be
    // moved off the main thread.
    void calculate_for(bool second_pass);
    // Stage C orchestration: snapshot + push the scope build to a worker (returns false -> caller
    // falls back to the sequential path); wait for the worker and publish its LOD globals.
    // The build culls against the NARROW scope frustum derived from scope_project.
    bool BeginSecondVPCalculateParallel(float second_vp_fov, const Fmatrix& scope_project) override;
    void EndSecondVPCalculateParallel() override;
    void AbortSecondVPCalculate() override;
    // Sun/rain tail of the sequential second Calculate(); runs on the main thread after
    // BeginSecondViewportRender() switched the Device to the scope projection.
    void SecondVPPostCalculate() override;
    void ApplySecondVPLodGlobals();
    // Stage 2 (sun-reuse): true when this frame's scope pass should REUSE the main pass sun
    // cascades from the transferred atlas slices instead of rebuilding them.
    bool svp_sun_reuse_active() const;
    void JoinSecondVPBuildThread();
    // Persistent SVP worker body (see the svp_worker_* members below).
    void svp_worker_loop();
    // Stage C: the scope visibility build runs on a DEDICATED thread, not the task scheduler -
    // scheduler LIFO ordering starves it behind the main render's own subtasks (sun cascades,
    // light batches), which serializes the pass again. Joined before the scope pass drains.
    std::thread svp_build_thread;
    // Persistent SVP worker (was: a thread spawned AND joined EVERY scope frame). Created
    // lazily on the first scope frame, parked on svp_worker_wake between jobs, fully joined
    // only on reset/teardown (JoinSecondVPBuildThread). Rationale: the per-frame thread
    // create/destroy churned per-thread Tracy/rpmalloc state in every statically linked DLL
    // (orphaned thread heaps, TLS slot churn) until the profiler's allocator destabilized -
    // AVs inside rpmalloc during queue-block allocation on unrelated threads; it also saves
    // the per-frame spawn cost. Handshake (Event is auto-reset): the main thread arms
    // svp_dsgraph/svp_calc_params, calls svp_worker_done.Reset() (drops a stale release,
    // e.g. a job aborted by teardown) and svp_worker_wake.Set(); the worker runs ONE job
    // (svp_build_abort skips its remaining stages) and svp_worker_done.Set()s;
    // EndSecondVPCalculateParallel waits done; teardown sets svp_worker_exit + wake + joins.
    Event svp_worker_wake;
    Event svp_worker_done;
    std::atomic<bool> svp_worker_exit{ false };
    void render_forward();
    void render_indirect(light* L) const;
    // svp_no_vis (frame-driver stage 1b): scope-pass mode - the package was pre-filtered against
    // the NARROW lens frustum (filter_light_package_for_svp), so per-light occq visibility
    // (vis_update / vis.visible checks) is skipped entirely: it cannot cull anything that the
    // frustum test missed and keeps occlusion queries off the worker-recording path (stage C).
    void render_lights(light_Package& LP, bool svp_no_vis = false);

    render_main r_main;
#if RENDER != R_R2
    render_rain r_rain;
#endif

    render_sun r_sun;
    render_sun_old r_sun_old;

public:
    auto get_largest_sector() const { return largest_sector_id; }
    ShaderElement* rimp_select_sh_static(dxRender_Visual* pVisual, float cdist_sq, u32 phase);
    ShaderElement* rimp_select_sh_dynamic(dxRender_Visual* pVisual, float cdist_sq, u32 phase);
    VertexElement* getVB_Format(int id, bool alternative = false);
    VertexStagingBuffer* getVB(int id, bool alternative = false);
    IndexStagingBuffer* getIB(int id, bool alternative = false);
    FSlideWindowItem* getSWI(int id);
    IRenderVisual* model_CreatePE(LPCSTR name);

    // HW-occlusion culling
    u32 occq_begin(u32& ID, u32 context_id = R__NUM_PARALLEL_CONTEXTS) { return HWOCC.occq_begin(ID, context_id); }
    void occq_end(u32& ID, u32 context_id = R__NUM_PARALLEL_CONTEXTS) { HWOCC.occq_end(ID, context_id); }
    auto occq_get(u32& ID) { return HWOCC.occq_get(ID); }
    bool occq_try_get(u32& ID, R_occlusion::occq_result& fragments) { return HWOCC.occq_try_get(ID, fragments); }

    ICF void apply_object(CBackend& cmd_list, IRenderable* O)
    {
        // No object / no ROS: keep the previous per-backend ambient constants - parity with the
        // main pass, which also skips the CROS refresh for ROS-less dynamics. (SVP callers now
        // always pass a real renderable - see the second_vp_pass fix in r__dsgraph_build.)
        if (!O || !O->renderable_ROS())
            return;

        CROS_impl& LT = *(CROS_impl*)O->renderable_ROS();
        LT.update_smooth(O);
        cmd_list.o_hemi = 0.75f * LT.get_hemi();
        // o_hemi						= 0.5f*LT.get_hemi			()	;
        cmd_list.o_sun = 0.75f * LT.get_sun();
        CopyMemory(cmd_list.o_hemi_cube, LT.get_hemi_cube(), CROS_impl::NUM_FACES * sizeof(float));
    }

public:
    // feature level
    GenerationLevel GetGeneration() const override { return IRender::GENERATION_R2; }
    bool is_sun_static() override { return o.sunstatic; }

#if defined(USE_DX11)
    BackendAPI GetBackendAPI() const override { return IRender::BackendAPI::D3D11; }
    u32 get_dx_level() override { return HW.FeatureLevel >= D3D_FEATURE_LEVEL_10_1 ? 0x000A0001 : 0x000A0000; }
    pcstr getShaderPath() override { return "r3\\"; }
#elif defined(USE_OGL)
    BackendAPI GetBackendAPI() const override { return IRender::BackendAPI::OpenGL; }
    u32 get_dx_level() override { return /*HW.pDevice1?0x000A0001:*/0x000A0000; }
    pcstr getShaderPath() override { return "gl\\"; }
#else
#   error No graphics API selected or enabled!
#endif

    [[nodiscard]]
    bool IsFastGeomSupported() const
    {
        return m_fast_geom_loaded;
    }

    // Loading / Unloading
    void create() override;
    void destroy() override;
    void reset_begin() override;
    void reset_end() override;

    void level_Load(IReader*) override;
    void level_Unload() override;

#if defined(USE_DX11)
    ID3DBaseTexture* texture_load(pcstr fname, u32& msize);
#elif defined(USE_OGL)
    GLuint           texture_load(pcstr fname, u32& msize, GLenum& ret_desc);
#else
#   error No graphics API selected or enabled!
#endif

    HRESULT shader_compile(pcstr name, IReader* fs,
        pcstr pFunctionName, pcstr pTarget, u32 Flags, void*& result) override;

    // Information
    void DumpStatistics(class IGameFont& font, class IPerformanceAlert* alert) override;
    ref_shader getShader(int id);
    IRenderVisual* getVisual(int id) override;

    // Main
    void add_Visual(u32 context_id, IRenderable* root, IRenderVisual* V, Fmatrix& m) override; // add visual leaf	(no culling performed at all)
    // wallmarks
    void add_StaticWallmark(ref_shader& S, const Fvector& P, float s, CDB::TRI* T, Fvector* V);
    void add_StaticWallmark(IWallMarkArray* pArray, const Fvector& P, float s, CDB::TRI* T, Fvector* V) override;
    void add_StaticWallmark(const wm_shader& S, const Fvector& P, float s, CDB::TRI* T, Fvector* V) override;
    void clear_static_wallmarks() override;
    void add_SkeletonWallmark(intrusive_ptr<CSkeletonWallmark> wm);
    void add_SkeletonWallmark(const Fmatrix* xf, CKinematics* obj, ref_shader& sh, const Fvector& start,
                              const Fvector& dir, float size);
    void add_SkeletonWallmark(const Fmatrix* xf, IKinematics* obj, IWallMarkArray* pArray, const Fvector& start,
                              const Fvector& dir, float size) override;

    //
    IBlender* blender_create(CLASS_ID cls);
    void blender_destroy(IBlender*&);

    //
    IRender_ObjectSpecific* ros_create(IRenderable* parent) override;
    void ros_destroy(IRender_ObjectSpecific*&) override;

    // Lighting
    IRender_Light* light_create() override;
    IRender_Glow* glow_create() override;

    // Models
    IRenderVisual* model_CreateParticles(LPCSTR name) override;
    IRender_DetailModel* model_CreateDM(IReader* F);
    IRenderVisual* model_Create(LPCSTR name, IReader* data = nullptr) override;
    IRenderVisual* model_Create(LPCSTR name, LPCSTR suffix, IReader* data = nullptr) override;
    IRenderVisual* model_CreateChild(LPCSTR name, IReader* data) override;
    IRenderVisual* model_CreateChild(LPCSTR name, LPCSTR suffix, IReader* data) override;
    IRenderVisual* model_Duplicate(IRenderVisual* V) override;
    void model_Delete(IRenderVisual*& V, bool bDiscard) override;
    void model_Delete(IRender_DetailModel*& F);
    void model_Logging(bool bEnable) override { Models->Logging(bEnable); }
    void models_Prefetch() override;
    void models_Clear(bool b_complete) override;
    void emplace_texture_replacements(shared_str material_key, shared_str dds_path);

    // Occlusion culling
    bool occ_visible(vis_data& V) override;
    bool occ_visible(Fbox& B) override;
    bool occ_visible(sPoly& P) override;

    // Main
    void OnCameraUpdated() override;

    void Calculate() override;
    void Render() override;
    void RenderSecondViewport() override;
    // Scope geometry recording as a self-contained unit - binds the scope G-buffer twins on the
    // dsgraph's OWN cmd list (explicit binds, no rt_* member / RCache dependency) and drains its
    // visibility maps. with_details=false for the split-scene variant. Falls back to the rt_*
    // members when the twins do not exist (scale == 1) - sequential-only in that case.
    // P2.3: the worker part is record_second_vp_geometry_into() (seed + render_graph(0), deferred);
    // lods/Details/submit happen on the main thread in Render() after the join.
    void record_second_vp_geometry_into(R_dsgraph_structure& ds);
    // Frame driver stage C (worker): build the scope pass shadow maps into the dedicated atlas,
    // sealing one command list per page into svp_smap_replay_lists. DX11-only no-op elsewhere.
    void record_second_vp_shadows();
    // Shared by the inline serial flush and the worker shadow builder: records one light's smap
    // into dsgraph.cmd_list. smap_target = {} renders into the regular rt_smap_depth member.
    bool render_light_smap(R_dsgraph_structure& dsgraph, light* L, const ref_rt& smap_target);
    // Frame driver stage 1 consumer (scope pass): replay prebuilt worker lists + accumulate this
    // package's lights against them; also handles the already-consumed follow-up package.
    void svp_accumulate_prebuilt(light_Package& LP);
    // Executes the recorded deferred commands; no-op when the svp cmd list is immediate
    // (legacy sequential path).
    void SubmitSVPDeferred(R_dsgraph_structure& ds);
    // Releases all stored smap replay lists (DX11: COM Release; GL: vector is always empty).
    void ReleaseSVPReplayLists();
    // Вариант A (render_lights MT analysis): filters a light package by the NARROW scope frustum -
    // lights whose volume sphere (position + range) misses the lens view skip shadow-map building
    // and accumulation entirely. Conservative: sphere covers the shadow-casting extent.
    void filter_light_package_for_svp(const light_Package& src, light_Package& dst);
    void BindBackbufferForUI() override;
    void RenderMenu() override;

    bool IsSecondViewportRenderPass() const override { return m_SecondViewportPass; }
    bool IsSecondViewportOutputRT() const { return m_SecondViewportOutputToRT; }

    void Screenshot(ScreenshotMode mode = SM_NORMAL, pcstr name = nullptr) override;
    void OnFrame() override;

    void BeforeWorldRender() override; //--#SM+#-- +SecondVP+ Procedure is called before world render and post-effects
    void AfterWorldRender() override;  //--#SM+#-- +SecondVP+ Procedure is called after world render and before UI

    void SetPostProcessParams(const SPPInfo& ppi) override;

#ifdef USE_OGL
    RenderContext GetCurrentContext() const override;
    void MakeContextCurrent(RenderContext context) override;
#endif

    // Render mode
    void rmNear(CBackend& cmd_list);
    void rmFar(CBackend& cmd_list);
    void rmNormal(CBackend& cmd_list);

    bool WeaponIcon_RenderToTexture(pcstr texture_name, u32 w, u32 h, const Fmatrix& view, const Fmatrix& proj,
        IRenderable* subject = nullptr) override;
    void WeaponIcon_ReleaseUserIconRt(pcstr texture_name) override;
    void WeaponIcon_ReleaseAllUserIconRts() override;
    bool WeaponIcon_SavePersistedUserRtToDdsDxt5(pcstr user_texture_name, pcstr fs_root, pcstr fname) override;

    // Save an arbitrary user RT to a DXT5 DDS file under the given FS root.
    // Used by the preview texture disk cache; DX11-only (returns false on OGL).
    bool SaveRtToDdsDxt5(const ref_rt& rt, pcstr fs_root, pcstr fname);

    // HUD overlay scope (g_3d_scopes 2): the live HUD is rendered offscreen into $user$hud_overlay
    // and composited over the world by the UI layer (UIGameCustom). The composite stays in the UI
    // layer on every backend (no native CompositeHudOverlay pass); FlipOverlayV publishes the work RT
    // to the overlay RT as a 1:1 identity copy on every backend (no Y-flip — the resolve quad and the
    // UI composite quad share stub_notransform_t.vs + canonical UV order, so they are symmetric).
    void SetHudOverlayActive(bool v) override { m_HudOverlayActive = v; }
    bool IsHudOverlayActive() const override { return m_HudOverlayActive; }
    void RenderHudOverlayToTexture() override;
    void CompositeHudOverlay() override;
    // The composite is owned by the UI layer (UIGameCustom) on every backend, so no backend reports
    // a native composite and the UI quad is always the one drawing $user$hud_overlay.
    bool CompositeHudOverlayNative() const override { return false; }
    void SetHudOverlayAlpha(float v) override { m_HudOverlayAlpha = v; }
    float GetHudOverlayAlpha() const override { return m_HudOverlayAlpha; }

    // Constructor/destructor/loader
    CRender();
    ~CRender() override;

    void addShaderOption(pcstr name, pcstr value);
    void clearAllShaderOptions() { m_ShaderOptions.clear(); }

    // Lightweight menu/editor preview scene placeholder.
    void PreviewScene_Initialize() override;
    void PreviewScene_Shutdown() override;
    bool PreviewScene_IsReady() const override;
    void PreviewScene_ResetBegin() override;
    void PreviewScene_ResetEnd() override;
    bool PreviewScene_RenderRenderable(IRenderable* subject, const Fmatrix& view, const Fmatrix& proj) override;
    bool PreviewScene_RenderModel(pcstr model_path, const Fmatrix& view, const Fmatrix& proj) override;
    bool PreviewScene_RenderModelNoCache(pcstr model_path, shared_str& out_texture_name) override;
    void PreviewScene_ScheduleModel(pcstr model_path, u32 priority = 1000) override;
    void PreviewScene_ProcessQueue() override;
    [[nodiscard]] bool PreviewScene_IsCached(pcstr model_path) const override;
    [[nodiscard]] bool PreviewScene_IsDirty(pcstr model_path) const override;
    [[nodiscard]] shared_str PreviewScene_TextureName(pcstr model_path) const override;
    [[nodiscard]] shared_str PreviewScene_ResolvedPoseName(pcstr model_path) const override;
    void PreviewScene_CollectCycleNames(pcstr model_path, xr_vector<shared_str>& out_cycles) override;
    void PreviewScene_ReleaseEphemeralTexture(pcstr texture_name) override;
    void PreviewScene_SetSettings(const SPreviewSceneSettings& settings) override;
    [[nodiscard]] SPreviewSceneSettings PreviewScene_GetSettings() const override;
    void PreviewScene_ResetDiskCache() override;

    [[nodiscard]]
    const ref_rt& PreviewScene_ColorRT() const;

    [[nodiscard]]
    const ref_rt& PreviewScene_DepthRT() const;

private:
    bool m_SecondViewportPass{};
    bool m_SecondViewportOutputToRT{};
    bool m_HudOverlayActive{}; // g_3d_scopes 3: skip world HUD, mirror live HUD via $user$hud_overlay
    float m_HudOverlayAlpha{ 1.f }; // crossfade alpha (0 = transparent, 1 = opaque)
    CPreviewSceneRenderer* m_preview_scene{};

    void CopyBackbufferToSecondVPRT(); // shared by legacy SVP and HUD overlay scope (AfterWorldRender)
    void ReleaseHudOverlayRT(); // called from destroy(): static overlay RTs must die before the resource manager

    // HUD overlay scope (g_3d_scopes 3): scene camera captured at Render() start — post-processing
    // clobbers Device camera fields, so the post-frame HUD snapshot must use this saved state.
    struct SHudOverlayCam
    {
        Fmatrix mView{}, mProject{}, mFullTransform{}, mInvView{}, mInvFullTransform{};
        Fvector vCameraPosition{}, vCameraDirection{}, vCameraTop{}, vCameraRight{};
        float fFOV{}, fASPECT{};
        bool valid{};
    };
    SHudOverlayCam m_hudOvlCam;

#if defined(USE_DX11)
    xr_vector<D3D_SHADER_MACRO> m_ShaderOptions;
#elif defined(USE_OGL)
    xr_string m_ShaderOptions;
#else
#   error No graphics API selected or enabled!
#endif

    IRender_Sector::sector_id_t largest_sector_id{ IRender_Sector::INVALID_SECTOR_ID };
};

// r2_weapon_icon.cpp: drop static ref_shader/ref_rt before xr_delete(Resources) on shutdown.
void WeaponIcon_ReleaseStaticResources();

extern CRender RImplementation;
} // namespace xray::render::RENDER_NAMESPACE
