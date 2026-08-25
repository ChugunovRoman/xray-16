#pragma once

#include "xrEngine/Engine.h"
#include "xrCDB/Frustum.h"
#include "xrCDB/ISpatial.h"
#include "vis_common.h"
#include "Include/xrRender/FactoryPtr.h"
#include "xrCore/xr_resource.h"

class IUIShader;
typedef FactoryPtr<IUIShader> wm_shader;
// refs
class ENGINE_API IRenderable;
struct ENGINE_API FSlideWindowItem;

struct ENGINE_API SPreviewSceneSettings
{
    float yaw_deg{};
    float pitch_deg{};
    float roll_deg{};
    Fvector offset{};
    float camera_distance_offset{};
    shared_str pose_name{};
    u32 frame_width{256};
    u32 frame_height{256};

    [[nodiscard]] bool operator==(const SPreviewSceneSettings& rhs) const noexcept
    {
        return yaw_deg == rhs.yaw_deg && pitch_deg == rhs.pitch_deg && roll_deg == rhs.roll_deg &&
            offset.x == rhs.offset.x && offset.y == rhs.offset.y && offset.z == rhs.offset.z &&
            camera_distance_offset == rhs.camera_distance_offset && pose_name == rhs.pose_name &&
            frame_width == rhs.frame_width && frame_height == rhs.frame_height;
    }

    [[nodiscard]] bool operator!=(const SPreviewSceneSettings& rhs) const noexcept { return !(*this == rhs); }
};

// fwd. decl.
struct SDL_Window;
struct SPPInfo;
class IRenderVisual;
class IKinematics;
class IGameFont;
class IPerformanceAlert;
struct Fbox2;
struct Fcolor;
class IReader;
class CMemoryWriter;

#ifndef _EDITOR
extern const float fLightSmoothFactor;
#else
const float fLightSmoothFactor = 4.f;
#endif
//////////////////////////////////////////////////////////////////////////
// definition (Dynamic Light)
class ENGINE_API IRender_Light : public xr_resource
{
public:
    enum LT
    {
        DIRECT = 0,
        POINT = 1,
        SPOT = 2,
        OMNIPART = 3,
        REFLECTED = 4,
    };

public:
    virtual void set_type(LT type) = 0;
    virtual void set_active(bool) = 0;
    virtual bool get_active() = 0;
    virtual void set_shadow(bool) = 0;
    virtual void set_volumetric(bool) = 0;
    virtual void set_volumetric_quality(float) = 0;
    virtual void set_volumetric_intensity(float) = 0;
    virtual void set_volumetric_distance(float) = 0;
    virtual void set_indirect(bool){};
    virtual void set_position(const Fvector& P) = 0;
    virtual void set_rotation(const Fvector& D, const Fvector& R) = 0;
    virtual void set_cone(float angle) = 0;
    virtual void set_range(float R) = 0;
    virtual void set_virtual_size(float R) = 0;
    virtual void set_texture(pcstr name) = 0;
    virtual void set_color(const Fcolor& C) = 0;
    virtual void set_color(float r, float g, float b) = 0;
    virtual void set_hud_mode(bool b) = 0;
    virtual bool get_hud_mode() = 0;
    virtual ~IRender_Light();
};
struct ENGINE_API resptrcode_light : public resptr_base<IRender_Light>
{
    void destroy() { _set(NULL); }
};
typedef resptr_core<IRender_Light, resptrcode_light> ref_light;

//////////////////////////////////////////////////////////////////////////
// definition (Dynamic Glow)
class ENGINE_API IRender_Glow : public xr_resource
{
public:
    virtual void set_active(bool) = 0;
    virtual bool get_active() = 0;
    virtual void set_ignore_occlusion(bool) = 0;
    virtual bool get_ignore_occlusion() const = 0;
    virtual void set_render_in_second_viewport(bool) = 0;
    virtual bool get_render_in_second_viewport() const = 0;
    virtual void set_world_glow(bool) = 0;
    virtual bool get_world_glow() const = 0;
    virtual void set_position(const Fvector& P) = 0;
    virtual void set_direction(const Fvector& P) = 0;
    virtual void set_radius(float R) = 0;
    virtual void set_texture(pcstr name) = 0;
    virtual void set_color(const Fcolor& C) = 0;
    virtual void set_color(float r, float g, float b) = 0;
    virtual ~IRender_Glow();
};
struct ENGINE_API resptrcode_glow : public resptr_base<IRender_Glow>
{
    void destroy() { _set(NULL); }
};
typedef resptr_core<IRender_Glow, resptrcode_glow> ref_glow;

//////////////////////////////////////////////////////////////////////////
// definition (Per-object render-specific data)
class ENGINE_API IRender_ObjectSpecific
{
public:
    enum mode
    {
        TRACE_LIGHTS = (1 << 0),
        TRACE_SUN = (1 << 1),
        TRACE_HEMI = (1 << 2),
        TRACE_ALL = (TRACE_LIGHTS | TRACE_SUN | TRACE_HEMI),
    };

public:
    virtual void force_mode(u32 mode) = 0;
    virtual float get_luminocity() = 0;
    virtual float get_luminocity_hemi() = 0;
    virtual float* get_luminocity_hemi_cube() = 0;

    virtual ~IRender_ObjectSpecific(){};
};

struct xrImTextureData
{
    ImTextureID texture{};
    Fvector2 size{};
};

enum class DeviceState
{
    Normal = 0,
    Lost,
    NeedReset
};

// 1: full second render to rt_secondVP each frame while 3D scope is active; main view keeps HUD. 0: legacy alternating frame + backbuffer copy.
extern ENGINE_API int ps_r__dedicated_second_vp;

// Tuning for the extra scope render pass (only when r__dedicated_second_vp 1). See comment on CRender::Render() in r2_R_render.cpp.
extern ENGINE_API int ps_r__svp_skip_details;     // 1 = skip DetailManager (grass etc.)
extern ENGINE_API int ps_r__svp_skip_wallmarks;  // 1 = skip Wallmarks
extern ENGINE_API int ps_r__svp_skip_rain_sync;  // 1 = skip r_rain.sync()
extern ENGINE_API int ps_r__svp_skip_sun_csm;    // 1 = skip sun init/run in 2nd Calculate + skip r_sun.sync in 2nd Render (reuse main cascades)
extern ENGINE_API int ps_r__svp_skip_zfill;      // 1 = skip Z-prefill pass when r2_zfill is enabled
extern ENGINE_API int ps_r__svp_frame_delay;     // Second VP: IsSVPFrame uses dwFrame % delay (0 = every frame). Console: r__svp_frame_delay

// HUD overlay scope (g_3d_scopes 2) debug output of the resolve pass. Console: r__hud_overlay_debug
// 0 = normal (lit albedo), 1 = solid magenta (drain/stencil check), 2 = normals, 3 = light factor,
// 4 = raw albedo, 5 = one-shot DDS dump of the overlay RTs into $screenshots$,
// 6 = sun N·L dot (white = sun-facing side of the weapon).
extern ENGINE_API int ps_r__hud_overlay_debug;
// 1 = crossfade alpha on ADS entry/exit (smooth transition), 0 = instant on/off (default)
extern ENGINE_API int ps_r__hud_overlay_crossfade;
// Light multiplier for overlay HUD (1.0 = engine default, 1.5-2.0 to approximate world HUD brightness)
extern ENGINE_API float ps_r__hud_overlay_brightness;
// FOV scale for overlay HUD after ADS lerp (1.0 = g_fov size, <1 larger HUD). Console: r__hud_overlay_fov_scale
extern ENGINE_API float ps_r__hud_overlay_fov_scale;

class ENGINE_API IRender
{
public:
    enum GenerationLevel : u32
    {
        GENERATION_R1 = 1,
        GENERATION_R2 = 2,
    };

    enum class BackendAPI : u32
    {
        D3D9,
        D3D10,
        D3D11,
        OpenGL
    };

    enum ScreenshotMode : u32
    {
        SM_NORMAL = 0, // jpeg, name ignored
        SM_FOR_CUBEMAP = 1, // tga, name used as postfix
        SM_FOR_GAMESAVE = 2, // dds/dxt1,name used as full-path
        SM_FOR_LEVELMAP = 3, // tga, name used as postfix (level_name)
    };

    enum RenderContext
    {
        NoContext = -1,
        PrimaryContext,
        HelperContext
    };

    class ENGINE_API ScopedContext
    {
        RenderContext previousContext;

    public:
        ScopedContext(RenderContext context);
        ~ScopedContext();
    };

    struct RenderStatistics
    {
        CStatTimer Culling; // portal traversal, frustum culling, entities "renderable_Render"
        CStatTimer Animation; // skeleton calculation
        CStatTimer Primitives; // actual primitive rendering
        CStatTimer Wait; // ...waiting something back (queries results, etc.)
        CStatTimer WaitS; // ...frame-limit sync
        CStatTimer RenderTargets; // ...render-targets
        CStatTimer Skinning; // ...skinning
        CStatTimer DetailVisibility; // ...details visibility detection
        CStatTimer DetailRender; // ...details rendering
        CStatTimer DetailCache; // ...details slot cache access
        u32 DetailCount; // ...number of DT-elements
        CStatTimer Wallmarks; // ...wallmark sorting, rendering
        u32 StaticWMCount; // ...number of static wallmark
        u32 DynamicWMCount; // ...number of dynamic wallmark
        u32 WMTriCount; // ...number of wallmark tri
        CStatTimer HUD; // ...hud rendering
        CStatTimer Glows; // ...glows vis-testing,sorting,render
        CStatTimer Lights; // ...d-lights building/rendering
        CStatTimer Projectors; // ...projectors building
        CStatTimer ShadowsCalc; // ...shadows building
        CStatTimer ShadowsRender; // ...shadows render
        u32 OcclusionQueries;
        u32 OcclusionCulled;
        u32 DynamicRegularSubmissions;
        u32 CorpseRegularSubmissions;
        u32 CorpseSmapSkipped;

        void FrameStart()
        {
            Culling.FrameStart();
            Animation.FrameStart();
            Primitives.FrameStart();
            Wait.FrameStart();
            WaitS.FrameStart();
            RenderTargets.FrameStart();
            Skinning.FrameStart();
            DetailVisibility.FrameStart();
            DetailRender.FrameStart();
            DetailCache.FrameStart();
            DetailCount = 0;
            Wallmarks.FrameStart();
            StaticWMCount = 0;
            DynamicWMCount = 0;
            WMTriCount = 0;
            HUD.FrameStart();
            Glows.FrameStart();
            Lights.FrameStart();
            Projectors.FrameStart();
            ShadowsCalc.FrameStart();
            ShadowsRender.FrameStart();
            OcclusionQueries = 0;
            OcclusionCulled = 0;
            DynamicRegularSubmissions = 0;
            CorpseRegularSubmissions = 0;
            CorpseSmapSkipped = 0;
        }

        void FrameEnd()
        {
            Culling.FrameEnd();
            Animation.FrameEnd();
            Primitives.FrameEnd();
            Wait.FrameEnd();
            WaitS.FrameEnd();
            RenderTargets.FrameEnd();
            Skinning.FrameEnd();
            DetailVisibility.FrameEnd();
            DetailRender.FrameEnd();
            DetailCache.FrameEnd();
            Wallmarks.FrameEnd();
            HUD.FrameEnd();
            Glows.FrameEnd();
            Lights.FrameEnd();
            Projectors.FrameEnd();
            ShadowsCalc.FrameEnd();
            ShadowsRender.FrameEnd();
        }
    };

public:
    // options
    bool m_hq_skinning;
    s32 m_skinning;
    s32 m_MSAASample;
    u32 m_SMAPSize;

    // data
    CFrustum ViewBase;

public:
    // feature level
    virtual GenerationLevel GetGeneration() const = 0;
    bool GenerationIsR1() const { return GetGeneration() == GENERATION_R1; }
    bool GenerationIsR2() const { return GetGeneration() == GENERATION_R2; }
    bool GenerationIsR2OrHigher() const { return GetGeneration() >= GENERATION_R2; }

    virtual BackendAPI GetBackendAPI() const = 0;

    virtual bool is_sun_static() = 0;
    virtual u32 get_dx_level() = 0;

    // Loading / Unloading
    virtual void create() = 0;
    virtual void destroy() = 0;
    virtual void reset_begin() = 0;
    virtual void reset_end() = 0;

    virtual void level_Load(IReader* fs) = 0;
    virtual void level_Unload() = 0;

    void shader_option_skinning(s32 mode) { m_skinning = mode; }
    virtual HRESULT shader_compile(pcstr name, IReader* fs, pcstr pFunctionName, pcstr pTarget, u32 Flags,
        void*& result) = 0;

    // Information
    virtual void DumpStatistics(IGameFont& font, IPerformanceAlert* alert) = 0;

    virtual pcstr getShaderPath() = 0;
    // virtual ref_shader getShader (int id) = 0;
    virtual IRenderVisual* getVisual(int id) = 0;

    virtual xrImTextureData GetImGuiTextureId(pcstr texture_name) = 0;

    // Main
    virtual void add_Visual(u32 context_id, IRenderable* root, IRenderVisual* V, Fmatrix& m) = 0; // add visual leaf (no culling performed at all)
    // virtual void add_StaticWallmark (ref_shader& S, const Fvector& P, float s, CDB::TRI* T, Fvector* V)=0;
    virtual void add_StaticWallmark(const wm_shader& S, const Fvector& P, float s, CDB::TRI* T, Fvector* V) = 0;
    // Prefer this function when possible
    virtual void add_StaticWallmark(IWallMarkArray* pArray, const Fvector& P, float s, CDB::TRI* T, Fvector* V) = 0;
    virtual void clear_static_wallmarks() = 0;
    // virtual void add_SkeletonWallmark (intrusive_ptr<CSkeletonWallmark> wm) = 0;
    // virtual void add_SkeletonWallmark (const Fmatrix* xf, CKinematics* obj, ref_shader& sh, const Fvector& start,
    // const Fvector& dir, float size)=0;
    // Prefer this function when possible
    virtual void add_SkeletonWallmark(const Fmatrix* xf, IKinematics* obj, IWallMarkArray* pArray, const Fvector& start,
        const Fvector& dir, float size) = 0;

    // virtual IBlender* blender_create (CLASS_ID cls) = 0;
    // virtual void blender_destroy (IBlender* &) = 0;

    virtual IRender_ObjectSpecific* ros_create(IRenderable* parent) = 0;
    virtual void ros_destroy(IRender_ObjectSpecific*&) = 0;

    // Lighting/glowing
    virtual IRender_Light* light_create() = 0;
    virtual void light_destroy(IRender_Light* p_){};
    virtual IRender_Glow* glow_create() = 0;
    virtual void glow_destroy(IRender_Glow* p_){};

    // Models
    virtual IRenderVisual* model_CreateParticles(pcstr name) = 0;
    // virtual IRender_DetailModel* model_CreateDM (IReader* F) = 0;
    // virtual IRenderDetailModel* model_CreateDM (IReader* F) = 0;
    // virtual IRenderVisual* model_Create (pcstr name, IReader* data=0) = 0;
    virtual IRenderVisual* model_Create(pcstr name, IReader* data = 0) = 0;
    virtual IRenderVisual* model_Create(pcstr name, LPCSTR suffix, IReader* data = 0) = 0;
    virtual IRenderVisual* model_CreateChild(pcstr name, IReader* data) = 0;
    virtual IRenderVisual* model_CreateChild(pcstr name, LPCSTR suffix, IReader* data) = 0;
    virtual IRenderVisual* model_Duplicate(IRenderVisual* V) = 0;
    // virtual void model_Delete (IRenderVisual* & V, bool bDiscard=false) = 0;
    virtual void model_Delete(IRenderVisual*& V, bool bDiscard = false) = 0;
    // virtual void model_Delete (IRender_DetailModel* & F) = 0;
    virtual void model_Logging(bool bEnable) = 0;
    virtual void models_Prefetch() = 0;
    virtual void models_Clear(bool b_complete) = 0;
    virtual void emplace_texture_replacements(shared_str material_key, shared_str dds_path) = 0;

    xr_map<shared_str, shared_str> texture_replacements;

    // Occlusion culling
    virtual bool occ_visible(vis_data& V) = 0;
    virtual bool occ_visible(Fbox& B) = 0;
    virtual bool occ_visible(sPoly& P) = 0;

    // Main
    virtual void Calculate() = 0;
    virtual void Render() = 0;
    // Extra world render (deferred pipeline) into rt_secondVP for 3D scope; default no-op (R1 / unsupported).
    virtual void RenderSecondViewport() {}
    /// After dedicated second pass, re-bind main swapchain + full viewport for game UI (HUD). Default no-op.
    virtual void BindBackbufferForUI() {}
    /// True only inside the extra CRender::Render() pass that targets rt_secondVP (dedicated 3D scope).
    virtual bool IsSecondViewportRenderPass() const { return false; }
    virtual void RenderMenu() = 0;

    virtual void BeforeWorldRender() = 0; //--#SM+#-- Перед рендерингом мира
    virtual void AfterWorldRender() = 0; //--#SM+#-- После рендеринга мира (до UI)

    virtual void Screenshot(ScreenshotMode mode = SM_NORMAL, pcstr name = nullptr) = 0;
    virtual void SetPostProcessParams(const SPPInfo& ppi) = 0;

    // Constructor/destructor
    virtual ~IRender() {}

public:
    //	Gamma correction functions
    virtual void setGamma(float fGamma) = 0;
    virtual void setBrightness(float fGamma) = 0;
    virtual void setContrast(float fGamma) = 0;
    virtual void updateGamma() = 0;

    //	Destroy
    virtual void OnDeviceDestroy(bool bKeepTextures) = 0;
    virtual void Destroy() = 0;
    virtual void Reset(SDL_Window* hWnd, u32& dwWidth, u32& dwHeight, float& fWidth_2, float& fHeight_2) = 0;

    //	Init
    virtual void ObtainRequiredWindowFlags(u32& windowFlags) = 0;
    virtual void SetupStates() = 0;
    virtual void OnDeviceCreate(pcstr shName) = 0;
    virtual void Create(SDL_Window* hWnd, u32& dwWidth, u32& dwHeight, float& fWidth_2, float& fHeight_2) = 0;

    //	Overdraw
    virtual void overdrawBegin() = 0;
    virtual void overdrawEnd() = 0;

    //	Resources control
    virtual void DeferredLoad(bool E) = 0;
    virtual void ResourcesDeferredUpload() = 0;
    virtual void ResourcesDeferredUnload() = 0;
    virtual void ResourcesGetMemoryUsage(u32& m_base, u32& c_base, u32& m_lmaps, u32& c_lmaps) = 0;
    virtual void ResourcesDestroyNecessaryTextures() = 0;
    virtual void ResourcesStoreNecessaryTextures() = 0;
    virtual void ResourcesDumpMemoryUsage() = 0;

    //	HWSupport
    virtual bool HWSupportsShaderYUV2RGB() = 0;

    //	Device state
    virtual DeviceState GetDeviceState() = 0;
    virtual bool GetForceGPU_REF() = 0;
    virtual u32 GetCacheStatPolys() = 0;
    virtual void OnCameraUpdated() = 0;
    virtual void Begin() = 0;
    virtual void Clear() = 0;
    virtual void End() = 0;
    // Called after End() returns so IDXGISwapChain::Present runs with a shallower stack (driver + hooks
    // are sensitive to remaining stack space; see 0xC00000FD in nvwgf2umx.dll).
    virtual void PresentFrame() = 0;
    virtual void ClearTarget() = 0;
    virtual void SetCacheXform(Fmatrix& mView, Fmatrix& mProject) = 0;
    virtual void OnAssetsChanged() = 0;

    virtual void PreviewScene_Initialize() {}
    virtual void PreviewScene_Shutdown() {}
    [[nodiscard]] virtual bool PreviewScene_IsReady() const { return false; }
    virtual void PreviewScene_ResetBegin() {}
    virtual void PreviewScene_ResetEnd() {}
    virtual bool PreviewScene_RenderRenderable(IRenderable* subject, const Fmatrix& view, const Fmatrix& proj)
    {
        (void)subject;
        (void)view;
        (void)proj;
        return false;
    }
    virtual bool PreviewScene_RenderModel(pcstr model_path, const Fmatrix& view, const Fmatrix& proj)
    {
        (void)model_path;
        (void)view;
        (void)proj;
        return false;
    }
    virtual bool PreviewScene_RenderModelNoCache(pcstr model_path, shared_str& out_texture_name)
    {
        (void)model_path;
        out_texture_name = shared_str();
        return false;
    }
    virtual void PreviewScene_ScheduleModel(pcstr model_path, u32 priority = 1000)
    {
        (void)model_path;
        (void)priority;
    }
    virtual void PreviewScene_ProcessQueue() {}
    [[nodiscard]] virtual bool PreviewScene_IsCached(pcstr model_path) const
    {
        (void)model_path;
        return false;
    }
    [[nodiscard]] virtual bool PreviewScene_IsDirty(pcstr model_path) const
    {
        (void)model_path;
        return false;
    }
    [[nodiscard]] virtual shared_str PreviewScene_TextureName(pcstr model_path) const
    {
        (void)model_path;
        return shared_str();
    }
    [[nodiscard]] virtual shared_str PreviewScene_ResolvedPoseName(pcstr model_path) const
    {
        (void)model_path;
        return shared_str();
    }
    virtual void PreviewScene_CollectCycleNames(pcstr model_path, xr_vector<shared_str>& out_cycles)
    {
        (void)model_path;
        out_cycles.clear();
    }
    virtual void PreviewScene_ReleaseEphemeralTexture(pcstr texture_name) { (void)texture_name; }
    virtual void PreviewScene_SetSettings(const SPreviewSceneSettings& settings) { (void)settings; }
    [[nodiscard]] virtual SPreviewSceneSettings PreviewScene_GetSettings() const { return {}; }
    virtual void PreviewScene_ResetDiskCache() {}

    virtual RenderContext GetCurrentContext() const = 0;
    virtual void MakeContextCurrent(RenderContext context) = 0;

    // Inventory weapon icon: render once to a named $user$ texture (see weapon_inv_icon). Default: unsupported.
    virtual bool WeaponIcon_RenderToTexture(
        pcstr texture_name, u32 w, u32 h, const Fmatrix& view, const Fmatrix& proj, IRenderable* subject = nullptr)
    {
        (void)texture_name;
        (void)w;
        (void)h;
        (void)view;
        (void)proj;
        (void)subject;
        return false;
    }
    // Drop persisted $user$ RT used by WeaponIcon_RenderToTexture (see r2_weapon_icon.cpp).
    virtual void WeaponIcon_ReleaseUserIconRt(pcstr texture_name) { (void)texture_name; }
    virtual void WeaponIcon_ReleaseAllUserIconRts() {}
    // Save a persisted weapon icon RT (same key as WeaponIcon_RenderToTexture) to DDS DXT5/BC3. DX11 only.
    virtual bool WeaponIcon_SavePersistedUserRtToDdsDxt5(pcstr user_texture_name, pcstr fs_root, pcstr fname)
    {
        (void)user_texture_name;
        (void)fs_root;
        (void)fname;
        return false;
    }

    // HUD overlay scope (g_3d_scopes 3): world pass skips the HUD, live HUD is rendered offscreen into
    // $user$hud_overlay, then blended over the backbuffer by CompositeHudOverlay before the UI layer.
    // Default: unsupported (flag never set by game).
    virtual void SetHudOverlayActive(bool) {}
    virtual bool IsHudOverlayActive() const { return false; }
    virtual void RenderHudOverlayToTexture() {}
    // Blend $user$hud_overlay over the backbuffer (GL native path: explicit stencil/blend/depth state,
    // avoids the UI quad's inherited stencil-gate and D3D-vs-FBO Y-orientation mismatch).
    virtual void CompositeHudOverlay() {}
    // True when this backend blends $user$hud_overlay over the backbuffer itself (CompositeHudOverlay),
    // so the UI layer must NOT draw the overlay quad again (double-draw bug: the UI path samples the
    // overlay FBO through stub_notransform_t.vs which inverts NDC-Y for DDS uploads, flipping the FBO
    // content an extra time -> a second, upside-down HUD over the native composite). GL returns true
    // (native composite implemented), DX11/base return false (UI quad still owns the composite there).
    virtual bool CompositeHudOverlayNative() const { return false; }
    // Crossfade alpha for the overlay (0 = fully transparent, 1 = fully opaque).
    virtual void SetHudOverlayAlpha(float) {}
    virtual float GetHudOverlayAlpha() const { return 0.f; }

    // Stage C of the SVP parallelization (plans/optimization_svp/svp_parallel_calculate_plan.md):
    // snapshot the scope camera and push the scope visibility build onto a worker while the caller
    // renders the main pass; join it afterwards and publish its per-pass LOD thresholds.
    // scope_project must be built by CameraManager::BuildSecondVPProjection - the visibility build
    // culls against the NARROW scope frustum derived from it (huge CPU saving on zoomed scopes).
    // Default: not supported (caller falls back to the sequential second Calculate).
    virtual bool BeginSecondVPCalculateParallel(float /*second_vp_fov*/, const Fmatrix& /*scope_project*/) { return false; }
    virtual void EndSecondVPCalculateParallel() {}
    virtual void AbortSecondVPCalculate() {}
    // Sun/rain tail of the sequential second Calculate(); must be called after
    // BeginSecondViewportRender() has switched the Device projection to the scope camera.
    virtual void SecondVPPostCalculate() {}
};
