#include "stdafx.h"

#include "PreviewSceneRenderer.h"

#include "Layers/xrRender/FBasicVisual.h"
#include "Layers/xrRender/SkeletonCustom.h"
#include "Include/xrRender/KinematicsAnimated.h"
#include "Layers/xrRender_R2/r2.h"
#include "xrCore/FS.h"
#include "xrEngine/XR_IOConsole.h"
#include "xrEngine/device.h"
#include "Layers/xrRender/ResourceManager.h"
#include "Layers/xrRender/SH_Texture.h"

#include <algorithm>
#include <iterator>

extern bool ENGINE_API g_bRendering;

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
constexpr u32 kPreviewSceneDefaultRTSize = 256;
constexpr pcstr kPreviewScenePositionRtName = "$user$preview_scene_position";
constexpr pcstr kPreviewSceneNormalRtName = "$user$preview_scene_normal";
constexpr pcstr kPreviewSceneAlbedoRtName = "$user$preview_scene_albedo";
constexpr pcstr kPreviewSceneColorRtName = "$user$preview_scene_color";
constexpr pcstr kPreviewSceneDepthRtName = "$user$preview_scene_depth";

struct PreviewSceneBlendCounter final : IterateBlendsCallback
{
    u32 count{};

    void operator()(CBlend&) override
    {
        ++count;
    }
};

static bool PreviewSceneHasActiveAnimation(IKinematicsAnimated* anim)
{
    if (!anim)
        return false;

    PreviewSceneBlendCounter counter;
    anim->LL_IterateBlends(counter);
    return counter.count != 0;
}

static bool PreviewSceneIsWholeBodyCycleName(pcstr name)
{
    if (!name || !name[0])
        return false;

    return !strstr(name, "torso") && !strstr(name, "head") && !strstr(name, "legs") && !strstr(name, "steering");
}

static MotionID PreviewSceneFindCycleByNames(IKinematicsAnimated* anim, pcstr const* names, size_t count)
{
    if (!anim)
        return {};

    for (size_t i = 0; i < count; ++i)
    {
        const MotionID id = anim->ID_Cycle_Safe(names[i]);
        if (id.valid())
            return id;
    }

    return {};
}

static MotionID PreviewSceneFindCycleByToken(IKinematicsAnimated* anim, pcstr token, bool whole_body_only)
{
    if (!anim || !token || !token[0])
        return {};

    const u16 slot_count = anim->LL_MotionsSlotCount();
    for (u16 slot = 0; slot < slot_count; ++slot)
    {
        accel_map* cycles = anim->LL_MotionsSlot(slot).cycle();
        if (!cycles)
            continue;

        for (const auto& it : *cycles)
        {
            const shared_str& name = it.first;

            if (!name.size() || !strstr(name.c_str(), token))
                continue;

            if (whole_body_only && !PreviewSceneIsWholeBodyCycleName(name.c_str()))
                continue;

            const MotionID id = anim->ID_Cycle_Safe(name);
            if (id.valid())
                return id;
        }
    }

    return {};
}

static MotionID PreviewSceneFindPreferredCycle(IKinematicsAnimated* anim, pcstr pose_name)
{
    if (!anim || !pose_name || !pose_name[0])
        return {};

    return anim->ID_Cycle_Safe(pose_name);
}

static void PreviewSceneCollectCycleNames(IKinematicsAnimated* anim, xr_vector<shared_str>& out_cycles)
{
    out_cycles.clear();
    if (!anim)
        return;

    const u16 slot_count = anim->LL_MotionsSlotCount();
    for (u16 slot = 0; slot < slot_count; ++slot)
    {
        accel_map* cycles = anim->LL_MotionsSlot(slot).cycle();
        if (!cycles)
            continue;

        for (const auto& it : *cycles)
        {
            const shared_str& name = it.first;
            if (!name.size())
                continue;

            if (std::find(out_cycles.begin(), out_cycles.end(), name) == out_cycles.end())
                out_cycles.push_back(name);
        }
    }

    std::sort(out_cycles.begin(), out_cycles.end(),
        [](const shared_str& lhs, const shared_str& rhs) { return xr_strcmp(lhs.c_str(), rhs.c_str()) < 0; });
}

static MotionID PreviewSceneFindFallbackCycle(IKinematicsAnimated* anim)
{
    if (!anim)
        return {};

    static constexpr pcstr exact_names[] = {
        "idle",
        "idle_0",
        "norm_idle_0",
        "norm_idle_1",
        "stand_idle_0",
        "stand_idle_1",
        "wait_idle_0",
        "wait_0",
        "stand_0",
        "new_idle_0",
        "norm_torso_9_idle_0",
    };

    if (MotionID id = PreviewSceneFindCycleByNames(anim, exact_names, std::size(exact_names)); id.valid())
        return id;

    static constexpr pcstr preferred_tokens[] = { "idle", "stand", "wait", "sit" };
    for (pcstr token : preferred_tokens)
        if (MotionID id = PreviewSceneFindCycleByToken(anim, token, true); id.valid())
            return id;

    for (pcstr token : preferred_tokens)
        if (MotionID id = PreviewSceneFindCycleByToken(anim, token, false); id.valid())
            return id;

    const u16 slot_count = anim->LL_MotionsSlotCount();
    for (u16 slot = 0; slot < slot_count; ++slot)
    {
        accel_map* cycles = anim->LL_MotionsSlot(slot).cycle();
        if (!cycles || cycles->empty())
            continue;

        for (const auto& it : *cycles)
        {
            const shared_str& name = it.first;

            const MotionID id = anim->ID_Cycle_Safe(name);
            if (id.valid())
                return id;
        }
    }

    return {};
}

static MotionID PreviewSceneResolveAnimationPose(IKinematicsAnimated* anim, const SPreviewSceneSettings& settings, shared_str* resolved_pose_name)
{
    if (!anim)
        return {};

    if (resolved_pose_name)
        *resolved_pose_name = "";

    const bool custom_pose_requested = settings.pose_name.size() != 0;
    MotionID cycle{};

    if (custom_pose_requested)
        cycle = PreviewSceneFindPreferredCycle(anim, settings.pose_name.c_str());

    if (!cycle.valid())
    {
        if (custom_pose_requested)
        {
            if (resolved_pose_name)
                *resolved_pose_name = "<fallback:auto>";
        }
        else if (PreviewSceneHasActiveAnimation(anim))
            return {};

        cycle = PreviewSceneFindFallbackCycle(anim);
    }

    if (resolved_pose_name && cycle.valid())
    {
        if (custom_pose_requested && PreviewSceneFindPreferredCycle(anim, settings.pose_name.c_str()).valid())
        {
            *resolved_pose_name = settings.pose_name;
        }
        else if (resolved_pose_name->size() == 0)
        {
            *resolved_pose_name = custom_pose_requested ? "<fallback:auto>" : "<auto>";
        }
    }

    return cycle;
}

static void PreviewSceneEnsureAnimationPose(IKinematicsAnimated* anim, const SPreviewSceneSettings& settings, shared_str* resolved_pose_name = nullptr)
{
    if (!anim)
        return;

    const MotionID cycle = PreviewSceneResolveAnimationPose(anim, settings, resolved_pose_name);
    if (!cycle.valid())
        return;

    if (settings.pose_name.size() != 0)
    {
        const CPartition& partition = anim->partitions();
        for (u16 i = 0; i < partition.count(); ++i)
            anim->LL_CloseCycle(i);
    }

    anim->PlayCycle(cycle, FALSE);
}

static void PreviewSceneUpdateKinematics(IRenderVisual* visual, const SPreviewSceneSettings& settings, shared_str* resolved_pose_name = nullptr)
{
    if (!visual)
        return;

    if (IKinematics* kin = visual->dcast_PKinematics())
    {
        if (IKinematicsAnimated* anim = kin->dcast_PKinematicsAnimated())
        {
            PreviewSceneEnsureAnimationPose(anim, settings, resolved_pose_name);
            anim->UpdateTracks();
        }

        kin->CalculateBones_Invalidate();
        kin->CalculateBones(TRUE);
    }
}

static Fvector PreviewSceneModelPivot(IRenderVisual* visual, const SPreviewSceneSettings& settings, shared_str* resolved_pose_name = nullptr)
{
    Fvector pivot{};
    if (!visual)
        return pivot;

    if (IKinematics* kin = visual->dcast_PKinematics())
    {
        PreviewSceneUpdateKinematics(visual, settings, resolved_pose_name);

        const u16 root = kin->LL_GetBoneRoot();
        pivot = kin->LL_GetTransform(root).c;
        return pivot;
    }

    const Fbox& bb = visual->getVisData().box;
    if (bb.is_valid())
    {
        Fvector c, ext;
        bb.get_CD(c, ext);
        pivot = c;
    }

    return pivot;
}

static void PreviewSceneBuildCamera(const Fvector& pivot, const Fbox& bounds, Fmatrix& view, Fmatrix& proj)
{
    constexpr float preview_fov_deg = 30.f;
    constexpr float preview_aspect = 1.f;
    constexpr float preview_near = 0.01f;
    constexpr float preview_far = 50.f;

    float radius = bounds.is_valid() ? bounds.getradius() : 0.5f;
    clamp(radius, 0.25f, 50.f);

    Fvector focus = pivot;
    if (bounds.is_valid())
        bounds.getcenter(focus);

    const float cam_dist = _max(1.1f, radius * 1.85f);
    Fvector eye;
    eye.set(focus.x, focus.y + cam_dist * 0.08f, focus.z + cam_dist);
    Fvector look_at;
    look_at.set(focus.x, focus.y, focus.z);

    view.build_camera(eye, look_at, Fvector().set(0.f, 1.f, 0.f));
    proj.build_projection(deg2rad(preview_fov_deg), preview_aspect, preview_near, preview_far);
}

static void PreviewSceneBuildCamera(
    const Fvector& pivot, const Fbox& bounds, const SPreviewSceneSettings& settings, Fmatrix& view, Fmatrix& proj)
{
    constexpr float preview_fov_deg = 30.f;
    constexpr float preview_near = 0.01f;
    constexpr float preview_far = 50.f;
    const float preview_aspect =
        settings.frame_height ? (float(settings.frame_width) / float(settings.frame_height)) : 1.f;
    const float safe_aspect = std::max(preview_aspect, 0.01f);
    const float base_fov = deg2rad(preview_fov_deg);
    // Treat preview_fov_deg as the square/reference framing. Taller portrait frames
    // get a larger vertical FOV so they reveal more of the model instead of merely
    // stretching the same captured area in the UI.
    const float vertical_fov = 2.0f * atanf(tanf(base_fov * 0.5f) / safe_aspect);

    float radius = bounds.is_valid() ? bounds.getradius() : 0.5f;
    clamp(radius, 0.25f, 50.f);

    Fvector focus = pivot;
    if (bounds.is_valid())
        bounds.getcenter(focus);

    const float base_dist = _max(1.1f, radius * 1.85f);
    const float cam_dist = _max(0.25f, base_dist + settings.camera_distance_offset);

    Fvector eye;
    eye.set(0.f, cam_dist * 0.08f, cam_dist);
    Fvector look_at;
    look_at.set(0.f, 0.f, 0.f);

    Fmatrix rotation;
    rotation.identity();
    rotation.setHPB(deg2rad(settings.yaw_deg), deg2rad(settings.pitch_deg), deg2rad(settings.roll_deg));
    rotation.transform_tiny(eye);
    rotation.transform_tiny(look_at);

    eye.add(focus);
    look_at.add(focus);

    if (!fis_zero(settings.offset.x) || !fis_zero(settings.offset.y) || !fis_zero(settings.offset.z))
    {
        Fvector ax, ay, az, delta;
        rotation.transform_dir(ax, Fvector().set(1.f, 0.f, 0.f));
        rotation.transform_dir(ay, Fvector().set(0.f, 1.f, 0.f));
        rotation.transform_dir(az, Fvector().set(0.f, 0.f, 1.f));

        delta.set(0.f, 0.f, 0.f);
        Fvector t;
        t.mul(ax, settings.offset.x);
        delta.add(t);
        t.mul(ay, settings.offset.y);
        delta.add(t);
        t.mul(az, settings.offset.z);
        delta.add(t);

        eye.add(delta);
        look_at.add(delta);
    }

    view.build_camera(eye, look_at, Fvector().set(0.f, 1.f, 0.f));
    proj.build_projection(vertical_fov, preview_aspect, preview_near, preview_far);
}

static u32 PreviewSceneResolveFrameWidth(const SPreviewSceneSettings& settings)
{
    return std::max<u32>(kPreviewSceneDefaultRTSize / 32, settings.frame_width ? settings.frame_width : kPreviewSceneDefaultRTSize);
}

static u32 PreviewSceneResolveFrameHeight(const SPreviewSceneSettings& settings)
{
    return std::max<u32>(kPreviewSceneDefaultRTSize / 32, settings.frame_height ? settings.frame_height : kPreviewSceneDefaultRTSize);
}

static u32 PreviewSceneSettingsHash(const SPreviewSceneSettings& settings)
{
    u32 hash = 0;
    hash = crc32(&settings.yaw_deg, sizeof(settings.yaw_deg), hash);
    hash = crc32(&settings.pitch_deg, sizeof(settings.pitch_deg), hash);
    hash = crc32(&settings.roll_deg, sizeof(settings.roll_deg), hash);
    hash = crc32(&settings.offset.x, sizeof(settings.offset.x), hash);
    hash = crc32(&settings.offset.y, sizeof(settings.offset.y), hash);
    hash = crc32(&settings.offset.z, sizeof(settings.offset.z), hash);
    hash = crc32(&settings.camera_distance_offset, sizeof(settings.camera_distance_offset), hash);
    hash = crc32(settings.pose_name.c_str(), settings.pose_name.size(), hash);
    hash = crc32(&settings.frame_width, sizeof(settings.frame_width), hash);
    hash = crc32(&settings.frame_height, sizeof(settings.frame_height), hash);
    return hash;
}

class CPreviewSceneRenderable final : public RenderableBase
{
public:
    CPreviewSceneRenderable()
    {
        renderable.xform.identity();
        renderable.visual = nullptr;
        renderable.pROS = nullptr;
        renderable.pROS_Allowed = true;
        renderable.invisible = false;
        renderable.hud = false;
    }

    ~CPreviewSceneRenderable() override = default;

    void renderable_Render(u32 context_id, IRenderable* root) override
    {
        if (!renderable.visual || !GEnv.Render)
            return;

        GEnv.Render->add_Visual(context_id, root, renderable.visual, renderable.xform);
        renderable.visual->getVisData().hom_frame = Device.dwFrame;
    }

    bool renderable_ShadowGenerate() override { return false; }
    bool renderable_ShadowReceive() override { return false; }
    bool renderable_Invisible() override { return false; }
    void renderable_Invisible(bool) override {}
    bool renderable_HUD() override { return false; }
    void renderable_HUD(bool) override {}
    bool renderable_SsaLodCharacter() const override { return true; }
    bool renderable_AllowCharacterMeshLod() const override { return true; }
};

static void PreviewSceneApplySubjectTransform(
    CPreviewSceneRenderable& subject, IRenderVisual* visual, const SPreviewSceneSettings& settings)
{
    subject.renderable.xform.identity();
    (void)visual;
    (void)settings;
}

} // namespace

CPreviewSceneRenderer::CPreviewSceneRenderer(CRender& owner)
    : m_owner(owner)
{
}

bool CPreviewSceneRenderer::EnsureRuntimeReady()
{
    return EnsureRuntimeReady(m_settings);
}

bool CPreviewSceneRenderer::EnsureRuntimeReady(const SPreviewSceneSettings& settings)
{
    const u32 runtime_width = PreviewSceneResolveFrameWidth(settings);
    const u32 runtime_height = PreviewSceneResolveFrameHeight(settings);

    if (m_runtime.ready && m_runtime.width == runtime_width && m_runtime.height == runtime_height)
        return true;

    if (m_runtime.HasAnyResource() && (m_runtime.width != runtime_width || m_runtime.height != runtime_height))
        m_runtime.Destroy();

    if (!m_owner.Resources)
    {
        return false;
    }

    if (!m_owner.Target)
    {
        return false;
    }
    if (!m_runtime.position_rt)
        m_runtime.position_rt = m_owner.Resources->_CreateRT(kPreviewScenePositionRtName, runtime_width, runtime_height, D3DFMT_A16B16G16R16F, 1);
    if (!m_runtime.normal_rt)
        m_runtime.normal_rt = m_owner.Resources->_CreateRT(kPreviewSceneNormalRtName, runtime_width, runtime_height, D3DFMT_A16B16G16R16F, 1);
    if (!m_runtime.albedo_rt)
    {
        VERIFY(m_owner.Target->rt_Color);
        const auto preview_albedo_fmt = m_owner.Target->rt_Color ? m_owner.Target->rt_Color->fmt : HW.Caps.fTarget;
        m_runtime.albedo_rt = m_owner.Resources->_CreateRT(
            kPreviewSceneAlbedoRtName, runtime_width, runtime_height, preview_albedo_fmt, 1);
    }
    if (!m_runtime.color_rt)
    {
        const auto preview_color_fmt = m_owner.Target->rt_Color ? m_owner.Target->rt_Color->fmt : HW.Caps.fTarget;
        m_runtime.color_rt = m_owner.Resources->_CreateRT(kPreviewSceneColorRtName, runtime_width, runtime_height, preview_color_fmt, 1);
    }
    if (!m_runtime.depth_rt)
        m_runtime.depth_rt = m_owner.Resources->_CreateRT(kPreviewSceneDepthRtName, runtime_width, runtime_height, HW.Caps.fDepth, 1, 1, { CRT::CreateSurface });

    EnsureResolveShader();

    m_runtime.ready = !!m_runtime.position_rt && !!m_runtime.normal_rt && !!m_runtime.albedo_rt && !!m_runtime.color_rt &&
        !!m_runtime.depth_rt && !!m_runtime.resolve_shader;
    if (m_runtime.ready)
    {
        m_runtime.width = runtime_width;
        m_runtime.height = runtime_height;
    }
    return m_runtime.ready;
}

void CPreviewSceneRenderer::EnsureResolveShader()
{
    if (m_runtime.resolve_shader)
        return;

    m_runtime.resolve_shader.create("preview_scene_resolve", kPreviewSceneAlbedoRtName);
    if (!m_runtime.resolve_shader)
        Msg("! [preview_scene] missing resolve shader: preview_scene_resolve");
}

bool CPreviewSceneRenderer::ResolveOutput(ref_rt& output_rt, u32 width, u32 height)
{
    if (!output_rt || !m_runtime.depth_rt || !m_runtime.resolve_shader)
        return false;

#if defined(USE_DX11)
    m_owner.Target->u_setrt(
        RCache, width, height, output_rt->pRT, nullptr, nullptr, m_runtime.depth_rt->pZRT[RCache.context_id]);
#elif defined(USE_OGL)
    m_owner.Target->u_setrt(RCache, width, height, output_rt->pRT, 0, 0, m_runtime.depth_rt->pZRT);
#else
#   error No graphics API selected or enabled!
#endif
    m_owner.rmNormal(RCache);
    const Fcolor clear_out(0.f, 0.f, 0.f, 0.f);
    RCache.ClearRT(output_rt, clear_out);

    RCache.set_CullMode(CULL_NONE);
    RCache.set_Z(FALSE);
    RCache.set_Stencil(TRUE, D3DCMP_EQUAL, 0x01, 0xff, 0x00, D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP, D3DSTENCILOP_KEEP);
    RCache.set_ColorWriteEnable();

    if (!m_owner.Target->g_menu)
        return false;

    u32 offset = 0;
    const u32 color = color_rgba(255, 255, 255, 255);
    const float width_f = float(width);
    const float height_f = float(height);
    Fvector2 p0, p1;
    p0.set(.5f / width_f, .5f / height_f);
    p1.set((width_f + .5f) / width_f, (height_f + .5f) / height_f);
    const float depth = EPS_S;
    const float rhw = 1.f;

    FVF::TL* pv = (FVF::TL*)RImplementation.Vertex.Lock(4, m_owner.Target->g_menu->vb_stride, offset);
    pv->set(EPS, float(height_f + EPS), depth, rhw, color, p0.x, p1.y);
    pv++;
    pv->set(EPS, EPS, depth, rhw, color, p0.x, p0.y);
    pv++;
    pv->set(float(width_f + EPS), float(height_f + EPS), depth, rhw, color, p1.x, p1.y);
    pv++;
    pv->set(float(width_f + EPS), EPS, depth, rhw, color, p1.x, p0.y);
    RImplementation.Vertex.Unlock(4, m_owner.Target->g_menu->vb_stride);

    RCache.set_Shader(m_runtime.resolve_shader);
    if (ShaderElement* se = m_runtime.resolve_shader->E[0]._get())
    {
        if (SPass* pass = se->passes[0]._get())
        {
            if (STextureList* textures = pass->T._get())
            {
                for (const auto& st : *textures)
                {
                    if (CTexture* tex = st.second._get())
                        tex->apply_load(RCache, st.first);
                }
            }
        }
    }
    RCache.set_Geometry(m_owner.Target->g_menu);
    RCache.Render(D3DPT_TRIANGLELIST, offset, 0, 4, 0, 2);

    RCache.set_Z(TRUE);
    RCache.set_Stencil(FALSE);
    return true;
}

void CPreviewSceneRenderer::RequeueCachedEntries()
{
    m_queue.clear();

    for (auto& [key, entry] : m_cache)
    {
        entry.color_rt.destroy();
        entry.ready = false;
        entry.failed = false;
        if (!entry.model_path.size())
            continue;

        entry.queued = true;
        entry.sequence = ++m_sequence;
        m_queue.push_back({key, entry.priority, entry.sequence});
    }
    m_queue_dirty = !m_queue.empty();
}

void CPreviewSceneRenderer::DestroyRuntime(bool preserve_requests)
{
    m_ephemeral.clear();
    FlushDeferredVisualDiscard();

    if (preserve_requests)
    {
        RequeueCachedEntries();
    }
    else
    {
        if (!m_runtime.ready && !m_runtime.HasAnyResource() && m_queue.empty() && m_cache.empty())
            return;

        FlushDiskIndex();
        m_queue.clear();
        m_cache.clear();
        m_sequence = 0;
        m_cached_budget = 0;
    }

    m_runtime.Destroy();
}

void CPreviewSceneRenderer::Initialize()
{
    EnsureRuntimeReady();
}

void CPreviewSceneRenderer::Shutdown()
{
    DestroyRuntime(false);
}

bool CPreviewSceneRenderer::IsReady() const
{
    return m_runtime.ready;
}

void CPreviewSceneRenderer::ResetBegin()
{
    DestroyRuntime(true);
}

void CPreviewSceneRenderer::ResetEnd()
{
    EnsureRuntimeReady();
}

bool CPreviewSceneRenderer::RenderRenderable(IRenderable* subject, const Fmatrix& view, const Fmatrix& proj)
{
    return RenderRenderableImpl(subject, view, proj, true, nullptr);
}

bool CPreviewSceneRenderer::RenderRenderableImpl(
    IRenderable* subject, const Fmatrix& view, const Fmatrix& proj, bool prepare_kinematics)
{
    return RenderRenderableImpl(subject, view, proj, prepare_kinematics, nullptr);
}

bool CPreviewSceneRenderer::RenderRenderableImpl(
    IRenderable* subject, const Fmatrix& view, const Fmatrix& proj, bool prepare_kinematics, const ref_rt* color_rt_override)
{
    if (!EnsureRuntimeReady())
        return false;

    if (!m_runtime.ready || !subject || !subject->GetRenderData().visual)
        return false;
    if (!Device.b_is_Ready || !m_owner.Target || !m_owner.Resources)
        return false;

    auto& dg = m_owner.get_imm_context();
    dg.reset();
    dg.context_id = R_dsgraph_structure::IMM_CTX_ID;
    dg.o.phase = CRender::PHASE_NORMAL;
    dg.o.spatial_types = 0;
    dg.o.spatial_traverse_flags = 0;
    dg.o.portal_traverse_flags = 0;
    dg.o.is_main_pass = false;
    dg.o.precise_portals = false;
    dg.o.use_hom = false;
    dg.o.sector_id = IRender_Sector::INVALID_SECTOR_ID;
    dg.r_pmask(true, true, true);
    dg.o.pmask_wmark = false;
    dg.o.view_pos = Fvector().set(view.c.x, view.c.y, view.c.z);
    dg.o.xform.identity();

    if (prepare_kinematics)
        PreviewSceneUpdateKinematics(subject->GetRenderData().visual, m_settings);

    struct SDeviceState
    {
        Fmatrix mView{};
        Fmatrix mProject{};
        Fmatrix mFullTransform{};
        Fmatrix mInvView{};
        Fmatrix mInvFullTransform{};
        Fvector vCameraPosition{};
        Fvector vCameraDirection{};
        Fvector vCameraTop{};
        Fvector vCameraRight{};
        u32 dwWidth{};
        u32 dwHeight{};
    } saved{};

    saved.mView = Device.mView;
    saved.mProject = Device.mProject;
    saved.mFullTransform = Device.mFullTransform;
    saved.mInvView = Device.mInvView;
    saved.mInvFullTransform = Device.mInvFullTransform;
    saved.vCameraPosition = Device.vCameraPosition;
    saved.vCameraDirection = Device.vCameraDirection;
    saved.vCameraTop = Device.vCameraTop;
    saved.vCameraRight = Device.vCameraRight;
    saved.dwWidth = Device.dwWidth;
    saved.dwHeight = Device.dwHeight;

    ref_rt& output_rt = (color_rt_override && *color_rt_override) ? const_cast<ref_rt&>(*color_rt_override) : m_runtime.color_rt;
    if (!output_rt)
        return false;

    Device.mView = view;
    Device.mProject = proj;
    Device.mInvView.invert(Device.mView);
    Device.mFullTransform.mul(Device.mProject, Device.mView);
    Device.mInvFullTransform.invert_44(Device.mFullTransform);
    Device.vCameraPosition.set(Device.mInvView.c.x, Device.mInvView.c.y, Device.mInvView.c.z);
    Device.mInvView.transform_dir(Device.vCameraDirection, Fvector().set(0.f, 0.f, -1.f));
    Device.vCameraDirection.normalize();
    Device.mInvView.transform_dir(Device.vCameraTop, Fvector().set(0.f, 1.f, 0.f));
    Device.vCameraTop.normalize();
    Device.vCameraRight.crossproduct(Device.vCameraTop, Device.vCameraDirection);
    Device.vCameraRight.normalize();
    Device.dwWidth = output_rt->dwWidth;
    Device.dwHeight = output_rt->dwHeight;
    GEnv.Render->SetCacheXform(Device.mView, Device.mProject);
    dg.cmd_list.set_xform_view(Device.mView);
    dg.cmd_list.set_xform_project(Device.mProject);
    dg.cmd_list.o_hemi = 1.0f;
    dg.cmd_list.o_sun = 1.0f;
    for (float& hemi_face : dg.cmd_list.o_hemi_cube)
        hemi_face = 1.0f;

    const u32 phase_saved = dg.o.phase;
    const bool pm0 = dg.o.pmask[0];
    const bool pm1 = dg.o.pmask[1];
    const bool pmw = dg.o.pmask_wmark;

    dg.marker++;

    const Fcolor clear_color(0.0f, 0.0f, 0.0f, 0.0f);
    const bool use_gbuffer_opt = RImplementation.o.gbuffer_opt;
#if defined(USE_DX11) || defined(USE_OGL)
    if (use_gbuffer_opt)
        m_owner.Target->u_setrt(dg.cmd_list, m_runtime.position_rt, m_runtime.albedo_rt, m_runtime.depth_rt);
    else
        m_owner.Target->u_setrt(dg.cmd_list, m_runtime.position_rt, m_runtime.normal_rt, m_runtime.albedo_rt, m_runtime.depth_rt);
    dg.cmd_list.set_Stencil(
        TRUE, D3DCMP_ALWAYS, 0x01, 0xff, 0x7f, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP);
    dg.cmd_list.set_CullMode(CULL_CCW);
    dg.cmd_list.set_ColorWriteEnable();
    dg.cmd_list.ClearRT(m_runtime.position_rt->pRT, clear_color);
    if (!use_gbuffer_opt)
        dg.cmd_list.ClearRT(m_runtime.normal_rt->pRT, clear_color);
    dg.cmd_list.ClearRT(m_runtime.albedo_rt->pRT, clear_color);
    dg.cmd_list.ClearZB(m_runtime.depth_rt, 1.0f, 0);
    m_owner.rmNormal(dg.cmd_list);
#else
#   error No graphics API selected or enabled!
#endif

    subject->renderable_Render(dg.context_id, subject);
    dg.render_graph(0);
    // Preview thumbnails don't need shadow geometry — clear PHASE_SMAP before render
    for (int i = 0; i < SHADER_PASSES_MAX; ++i)
        dg.mapNormalPasses[1][i].destroy();
    dg.render_graph(1);

#if RENDER != R_R1
    dg.mapEmissive.clear();
    dg.mapSorted.clear();
    dg.mapWmark.clear();
    dg.mapDistort.clear();
#endif

    dg.o.phase = phase_saved;
    dg.r_pmask(pm0, pm1, pmw);

#if defined(USE_DX11)
    m_owner.Target->u_setrt(dg.cmd_list, saved.dwWidth, saved.dwHeight, m_owner.Target->get_base_rt(), nullptr, nullptr, m_owner.Target->get_base_zb());
#elif defined(USE_OGL)
    m_owner.Target->u_setrt(dg.cmd_list, saved.dwWidth, saved.dwHeight, m_owner.Target->get_base_rt(), 0, 0, m_owner.Target->get_base_zb());
#else
#   error No graphics API selected or enabled!
#endif
    m_owner.rmNormal(dg.cmd_list);
    const bool resolved = ResolveOutput(output_rt, output_rt->dwWidth, output_rt->dwHeight);

    Device.mView = saved.mView;
    Device.mProject = saved.mProject;
    Device.mFullTransform = saved.mFullTransform;
    Device.mInvView = saved.mInvView;
    Device.mInvFullTransform = saved.mInvFullTransform;
    Device.vCameraPosition = saved.vCameraPosition;
    Device.vCameraDirection = saved.vCameraDirection;
    Device.vCameraTop = saved.vCameraTop;
    Device.vCameraRight = saved.vCameraRight;
    Device.dwWidth = saved.dwWidth;
    Device.dwHeight = saved.dwHeight;
    GEnv.Render->SetCacheXform(Device.mView, Device.mProject);
    RCache.set_xform_view(Device.mView);
    RCache.set_xform_project(Device.mProject);

#if defined(USE_DX11)
    m_owner.Target->u_setrt(RCache, saved.dwWidth, saved.dwHeight, m_owner.Target->get_base_rt(), nullptr, nullptr, m_owner.Target->get_base_zb());
#elif defined(USE_OGL)
    m_owner.Target->u_setrt(RCache, saved.dwWidth, saved.dwHeight, m_owner.Target->get_base_rt(), 0, 0, m_owner.Target->get_base_zb());
#else
#   error No graphics API selected or enabled!
#endif
    m_owner.rmNormal(RCache);

    return resolved;
}

bool CPreviewSceneRenderer::RenderModel(pcstr model_path, const Fmatrix& view, const Fmatrix& proj)
{
    if (!EnsureRuntimeReady())
        return false;

    if (!m_runtime.ready || !model_path || !model_path[0] || !Device.b_is_Ready || !m_owner.Target || !m_owner.Resources)
        return false;

    IRenderVisual* visual = m_owner.model_Create(model_path);
    if (!visual)
    {
        Msg("! [preview_scene] model create failed: %s", model_path);
        return false;
    }

    CPreviewSceneRenderable subject{};
    subject.renderable.visual = visual;
    PreviewSceneApplySubjectTransform(subject, visual, m_settings);

    const bool rendered = RenderRenderableImpl(&subject, view, proj, false, nullptr);
    DiscardPreviewVisual(visual);
    return rendered;
}

shared_str CPreviewSceneRenderer::MakeTextureName(pcstr model_path) const
{
    return MakeTextureName(model_path, m_settings);
}

shared_str CPreviewSceneRenderer::MakeTextureName(pcstr model_path, const SPreviewSceneSettings& settings) const
{
    if (!model_path || !model_path[0])
        return shared_str();

    const u32 model_crc = path_crc32(model_path, xr_strlen(model_path));
    const u32 settings_crc = PreviewSceneSettingsHash(settings);
    string128 name{};
    xr_sprintf(name, sizeof(name), "$user$npc_preview_%08x_%08x", model_crc, settings_crc);
    return shared_str(name);
}

shared_str CPreviewSceneRenderer::MakeCacheKey(pcstr model_path, const SPreviewSceneSettings& settings) const
{
    return MakeTextureName(model_path, settings);
}

shared_str CPreviewSceneRenderer::MakeNoCacheTextureName(pcstr model_path)
{
    if (!model_path || !model_path[0])
        return shared_str();

    const u32 crc = path_crc32(model_path, xr_strlen(model_path));
    string64 name{};
    xr_sprintf(name, sizeof(name), "$user$npc_preview_nc_%08x_%08x", crc, u32(++m_sequence));
    return shared_str(name);
}

bool CPreviewSceneRenderer::RenderModelIntoRT(
    IRenderVisual* visual, ref_rt& output_rt, const SPreviewSceneSettings& settings, shared_str* resolved_pose_name)
{
    if (!output_rt || !visual)
        return false;

    const Fbox& bounds = visual->getVisData().box;
    shared_str local_resolved_pose_name;
    const Fvector pivot = PreviewSceneModelPivot(visual, settings, &local_resolved_pose_name);
    Fmatrix view, proj;
    PreviewSceneBuildCamera(pivot, bounds, settings, view, proj);

    CPreviewSceneRenderable subject{};
    subject.renderable.visual = visual;
    PreviewSceneApplySubjectTransform(subject, visual, settings);

    const bool rendered = RenderRenderableImpl(&subject, view, proj, false, &output_rt);
    subject.renderable.visual = nullptr;
    if (rendered && resolved_pose_name)
        *resolved_pose_name = local_resolved_pose_name;
    return rendered;
}

bool CPreviewSceneRenderer::RenderModelIntoRT(
    pcstr model_path, ref_rt& output_rt, const SPreviewSceneSettings& settings, shared_str* resolved_pose_name)
{
    if (!output_rt || !model_path || !model_path[0])
        return false;

    IRenderVisual* visual = m_owner.model_Create(model_path);
    if (!visual)
    {
        Msg("! [preview_scene] model create failed: %s", model_path);
        return false;
    }

    const bool rendered = RenderModelIntoRT(visual, output_rt, settings, resolved_pose_name);
    DiscardPreviewVisual(visual);
    return rendered;
}

void CPreviewSceneRenderer::ScheduleModel(pcstr model_path, u32 priority)
{
    if (!model_path || !model_path[0])
        return;

    const SPreviewSceneSettings settings = m_settings;
    const shared_str key = MakeCacheKey(model_path, settings);
    auto [it, inserted] = m_cache.emplace(key, SCacheEntry{});
    auto& entry = it->second;
    if (inserted || !entry.texture_name.size())
    {
        entry.model_path = shared_str(model_path);
        entry.settings = settings;
        entry.texture_name = MakeTextureName(model_path, settings);
        entry.priority = priority;
        entry.sequence = ++m_sequence;
        entry.ready = false;
        entry.dirty = false;
        entry.failed = false;
        entry.queued = true;
        m_queue.push_back({key, priority, entry.sequence});
        m_queue_dirty = true;
        return;
    }

    if (entry.ready && !entry.dirty)
        return;

    entry.settings = settings;
    if (!entry.queued)
    {
        entry.priority = priority;
        entry.queued = true;
        entry.sequence = ++m_sequence;
        m_queue.push_back({key, priority, entry.sequence});
        m_queue_dirty = true;
    }
    else if (priority < entry.priority)
    {
        // Priority promotion: visible models jump ahead of background preloads.
        // The superseded queue entry is skipped when processed (entry.ready == true by then).
        entry.priority = priority;
        m_queue.push_back({key, priority, ++m_sequence});
        m_queue_dirty = true;
    }
    entry.failed = false;
}

bool CPreviewSceneRenderer::IsCached(pcstr model_path) const
{
    if (!model_path || !model_path[0])
        return false;

    const auto it = m_cache.find(MakeCacheKey(model_path, m_settings));
    return it != m_cache.end() && it->second.ready && !it->second.dirty;
}

bool CPreviewSceneRenderer::IsDirty(pcstr model_path) const
{
    if (!model_path || !model_path[0])
        return false;

    const auto it = m_cache.find(MakeCacheKey(model_path, m_settings));
    if (it == m_cache.end())
        return false; // Not in cache yet — brand new model, no stale entry
    const auto& e = it->second;
    return !e.ready || e.dirty || e.queued;
}

shared_str CPreviewSceneRenderer::TextureName(pcstr model_path) const
{
    if (!model_path || !model_path[0])
        return shared_str();

    const auto it = m_cache.find(MakeCacheKey(model_path, m_settings));
    if (it != m_cache.end())
        return it->second.texture_name;
    return MakeTextureName(model_path, m_settings);
}

shared_str CPreviewSceneRenderer::ResolvedPoseName(pcstr model_path) const
{
    if (!model_path || !model_path[0])
        return shared_str();

    const auto it = m_cache.find(MakeCacheKey(model_path, m_settings));
    if (it != m_cache.end())
        return it->second.resolved_pose_name;
    return shared_str();
}

void CPreviewSceneRenderer::CollectCycleNames(pcstr model_path, xr_vector<shared_str>& out_cycles)
{
    out_cycles.clear();
    if (!model_path || !model_path[0])
        return;

    IRenderVisual* visual = m_owner.model_Create(model_path);
    if (!visual)
        return;

    if (IKinematics* kin = visual->dcast_PKinematics())
    {
        if (IKinematicsAnimated* anim = kin->dcast_PKinematicsAnimated())
            PreviewSceneCollectCycleNames(anim, out_cycles);
    }

    DiscardPreviewVisual(visual);
}

void CPreviewSceneRenderer::ReleaseEphemeralTexture(pcstr texture_name)
{
    if (!texture_name || !texture_name[0] || m_ephemeral.empty())
        return;

    m_ephemeral.erase(std::remove_if(m_ephemeral.begin(), m_ephemeral.end(),
            [texture_name](SEphemeralEntry& entry)
            {
                if (entry.texture_name != texture_name)
                    return false;

                entry.color_rt.destroy();
                return true;
            }),
        m_ephemeral.end());
}

void CPreviewSceneRenderer::SetSettings(const SPreviewSceneSettings& settings)
{
    if (m_settings == settings)
        return;

    // The preview renderer is a single shared instance queried by several grids
    // that each have their own settings (frame size, camera offset, pose, ...).
    // Cache entries are keyed by (model_path, settings), and IsCached/IsDirty/
    // TextureName lookups use the *current* m_settings. When the active grid
    // changes its settings, any visible model that has no ready entry under the
    // new settings must be re-rendered, otherwise it shows up as an empty cell
    // (the lookup misses and there is no entry to mark as dirty -> no placeholder
    // either). Re-queue entries matching the new settings so they render fresh.
    m_settings = settings;
    for (auto& [key, entry] : m_cache)
    {
        if (entry.settings != settings)
            continue;
        if (entry.ready && !entry.dirty)
            continue;
        if (!entry.model_path.size())
            continue;
        if (!entry.queued)
        {
            entry.queued = true;
            entry.sequence = ++m_sequence;
            m_queue.push_back({key, entry.priority, entry.sequence});
            m_queue_dirty = true;
        }
        entry.failed = false;
    }
}

bool CPreviewSceneRenderer::RenderCachedEntry(SCacheEntry& entry)
{
    if (!entry.model_path.size())
        return false;

    if (!EnsureRuntimeReady(entry.settings))
        return false;

    const u32 entry_width = PreviewSceneResolveFrameWidth(entry.settings);
    const u32 entry_height = PreviewSceneResolveFrameHeight(entry.settings);
    if (!!entry.color_rt && (entry.color_rt->dwWidth != entry_width || entry.color_rt->dwHeight != entry_height))
        entry.color_rt.destroy();

    if (!entry.color_rt)
    {
        VERIFY(m_owner.Resources);
        VERIFY(m_owner.Target);
        if (!m_owner.Resources || !m_owner.Target)
            return false;

        const auto preview_color_fmt = m_owner.Target->rt_Color->fmt;
        entry.color_rt = m_owner.Resources->_CreateRT(
            entry.texture_name.c_str(), entry_width, entry_height, preview_color_fmt, 1);
        if (!entry.color_rt)
        {
            Msg("! [preview_scene] cache RT create failed: model=[%s] tex=[%s]",
                entry.model_path.c_str(), entry.texture_name.c_str());
            return false;
        }
    }

    // Create the model per render and discard it right after: the cache keeps
    // only the rendered preview texture. Models are far heavier than their
    // preview RTs, and CModelPool never releases them on its own.
    IRenderVisual* visual = m_owner.model_Create(entry.model_path.c_str());
    if (!visual)
    {
        Msg("! [preview_scene] model create failed: %s", entry.model_path.c_str());
        entry.failed = true;
        entry.dirty = false;
        return false;
    }

    shared_str resolved_pose_name;
    const bool had_ready_texture = entry.ready;
    // RenderModelIntoRT -> RenderRenderableImpl calls EnsureRuntimeReady() (which
    // uses m_settings) and PreviewSceneUpdateKinematics(visual, m_settings). Since
    // the renderer is shared between grids with different settings, m_settings may
    // currently belong to another grid and have a different frame size / pose,
    // which would make EnsureRuntimeReady() destroy & recreate the shared runtime
    // (mismatched RT sizes) and apply the wrong pose. Temporarily align m_settings
    // with this entry's settings for the duration of the render so the cached
    // preview is rendered with its own configuration.
    const SPreviewSceneSettings saved_settings = m_settings;
    m_settings = entry.settings;
    const bool rendered = RenderModelIntoRT(visual, entry.color_rt, entry.settings, &resolved_pose_name);
    m_settings = saved_settings;
    DiscardPreviewVisual(visual);
    entry.ready = rendered || had_ready_texture;
    entry.failed = !rendered;
    entry.dirty = !rendered;
    if (rendered)
    {
        entry.resolved_pose_name = resolved_pose_name;
        // Derive the correct RT name from the entry's own settings (not the
        // active grid's m_settings which was restored above).
        entry.texture_name = MakeTextureName(entry.model_path.c_str(), entry.settings);
        SaveToDiskCache(entry, entry.color_rt);
    }
    return rendered;
}

bool CPreviewSceneRenderer::RenderModelNoCache(pcstr model_path, shared_str& out_texture_name)
{
    out_texture_name = shared_str();
    if (!EnsureRuntimeReady())
        return false;

    if (!m_runtime.ready || !model_path || !model_path[0] || !Device.b_is_Ready || !m_owner.Target || !m_owner.Resources)
        return false;

    const u32 entry_width = PreviewSceneResolveFrameWidth(m_settings);
    const u32 entry_height = PreviewSceneResolveFrameHeight(m_settings);
    const shared_str texture_name = MakeNoCacheTextureName(model_path);
    if (!texture_name.size())
        return false;

    const auto preview_color_fmt = m_owner.Target->rt_Color->fmt;
    ref_rt color_rt =
        m_owner.Resources->_CreateRT(texture_name.c_str(), entry_width, entry_height, preview_color_fmt, 1);
    if (!color_rt)
        return false;

    if (!RenderModelIntoRT(model_path, color_rt, m_settings, nullptr))
    {
        color_rt.destroy();
        return false;
    }

    SEphemeralEntry entry;
    entry.texture_name = texture_name;
    entry.color_rt = color_rt;
    m_ephemeral.push_back(entry);

    out_texture_name = texture_name;
    return true;
}

void CPreviewSceneRenderer::InvalidateCache()
{
    m_queue.clear();

    for (auto& it : m_cache)
    {
        SCacheEntry& entry = it.second;
        entry.failed = false;
        entry.dirty = true;
        entry.queued = true;
        entry.sequence = ++m_sequence;
        m_queue.push_back({it.first, entry.priority, entry.sequence});
    }
    m_queue_dirty = !m_queue.empty();
}

void CPreviewSceneRenderer::DiscardPreviewVisual(IRenderVisual*& visual)
{
    if (!visual)
        return;

    if (g_bRendering)
    {
        // A discarding delete inside an active frame hits VERIFY in
        // CModelPool::Delete and would silently fall back to pooling in
        // release builds. Defer to ProcessQueue, which runs in
        // CRender::OnFrame, outside of Begin/End rendering scope.
        m_deferredVisualDiscard.push_back(visual);
        visual = nullptr;
        return;
    }

    // bDiscard = true frees both the instance and the base model (once the
    // last reference is gone) instead of parking them in the model pool.
    m_owner.model_Delete(visual, true);
}

void CPreviewSceneRenderer::FlushDeferredVisualDiscard()
{
    for (IRenderVisual* visual : m_deferredVisualDiscard)
    {
        IRenderVisual* ptr = visual;
        m_owner.model_Delete(ptr, true);
    }
    m_deferredVisualDiscard.clear();
}

// ---------------------------------------------------------------------------
// Disk preview cache
// ---------------------------------------------------------------------------

bool CPreviewSceneRenderer::EnsureDiskCacheDir()
{
    if (m_diskCachePath[0])
        return true;

    if (!FS.path_exist(kPreviewCacheAlias))
        return false;

    // Resolve the alias to an absolute path (cache it for all future calls).
    if (!FS.update_path(m_diskCachePath, kPreviewCacheAlias, "", false))
        return false;

    return m_diskCachePath[0] != '\0';
}

bool CPreviewSceneRenderer::ResolveCacheFilePath(string_path& dest, pcstr filename) const
{
    // m_diskCachePath must be filled by EnsureDiskCacheDir first.
    if (!m_diskCachePath[0] || !filename || !filename[0])
        return false;

    // Build full path: <cache_dir>\<filename>
    strconcat(sizeof(dest), dest, m_diskCachePath, filename);
    return true;
}

bool CPreviewSceneRenderer::ResolveOgfPath(pcstr model_path, string_path& out_fn)
{
    if (!model_path || !model_path[0])
        return false;

    string_path name;
    xr_strcpy(name, model_path);
    if (!strext(name))
        xr_strcat(name, ".ogf");
    else if (!strext(name)[0])
        xr_strcat(name, ".ogf");

    out_fn[0] = '\0';

    // Same lookup order as CModelPool::Instance_Load. All lookups go through
    // a safe manual resolve: FS.exist(out, alias, name) variants call
    // update_path(crashOnNotFound=true) and fatal on missing aliases.
    const pcstr aliases[] = { "$level$", "$game_meshes$" };
    for (pcstr alias : aliases)
    {
        if (!FS.path_exist(alias))
            continue;
        string_path resolved;
        if (FS.update_path(resolved, alias, name, false) && FS.exist(resolved, FSType::Any))
        {
            xr_strcpy(out_fn, resolved);
            return true;
        }
    }

    return false;
}

void CPreviewSceneRenderer::EnsureDiskCacheLoaded()
{
    if (m_diskCacheLoaded)
        return;

    m_diskCacheLoaded = true;
    m_diskIndex.clear();

    if (!EnsureDiskCacheDir())
        return;

    // Read version header and entries from index.txt using a resolved full path.
    string_path index_path;
    if (!ResolveCacheFilePath(index_path, kPreviewCacheIndexFile))
        return;

    IReader* reader = FS.r_open(index_path);
    if (!reader)
        return;

    xr_string line;
    bool header_ok = false;
    while (!reader->eof())
    {
        const char* data = (const char*)reader->pointer();
        const u32 avail = reader->elapsed();
        if (avail == 0)
            break;

        // Extract a line (handle \r\n and \n).
        u32 len = 0;
        while (len < avail && data[len] != '\n')
            ++len;
        line.assign(data, len);
        reader->advance(len);
        if (reader->elapsed() > 0 && *(const char*)reader->pointer() == '\n')
            reader->advance(1);

        // Trim trailing \r.
        while (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (line.empty())
            continue;

        if (!header_ok)
        {
            // Expected: "version <N>"
            u32 ver = 0;
            if (sscanf(line.c_str(), "version %u", &ver) == 1)
            {
                if (ver != kPreviewDiskCacheVersion)
                {
                    FS.r_close(reader);
                    WipeDiskCache();
                    return;
                }
                header_ok = true;
            }
            else
            {
                // Old-format or corrupt file — wipe and start fresh.
                FS.r_close(reader);
                WipeDiskCache();
                return;
            }
            continue;
        }

        // Parse: "base|ogf_size|ogf_mtime|resolved_pose"
        // Split on '|' — pose may contain '|' theoretically (unlikely), so split from left.
        const char* s0 = line.c_str();
        const char* p1 = strchr(s0, '|');
        if (!p1) continue;
        const char* p2 = strchr(p1 + 1, '|');
        if (!p2) continue;
        const char* p3 = strchr(p2 + 1, '|');
        // p3 optional — pose can be empty.

        string64 base_buf;
        u32 base_len = (u32)(p1 - s0);
        clamp(base_len, 0u, (u32)(sizeof(base_buf) - 1));
        strncpy(base_buf, s0, base_len);
        base_buf[base_len] = '\0';

        shared_str base(base_buf);
        SDiskCacheEntry e;
        e.ogf_size = (u32)strtoul(p1 + 1, nullptr, 10);
        e.ogf_mtime = (u32)strtoul(p2 + 1, nullptr, 10);
        if (p3)
        {
            string256 pose_buf;
            u32 pose_len = (u32)(p3 - (p2 + 1));
            clamp(pose_len, 0u, (u32)(sizeof(pose_buf) - 1));
            strncpy(pose_buf, p2 + 1, pose_len);
            pose_buf[pose_len] = '\0';
            e.resolved_pose = shared_str(pose_buf);
        }
        else
        {
            e.resolved_pose = shared_str(p2 + 1);
        }

        if (base.size())
            m_diskIndex[base] = std::move(e);
    }

    FS.r_close(reader);
}

void CPreviewSceneRenderer::FlushDiskIndex()
{
    if (!m_diskCacheDirty || !m_diskCacheLoaded)
        return;

    if (!EnsureDiskCacheDir())
        return;

    string_path index_path;
    if (!ResolveCacheFilePath(index_path, kPreviewCacheIndexFile))
        return;

    if (IWriter* w = FS.w_open(index_path))
    {
        // Header
        string64 hdr;
        xr_sprintf(hdr, "version %u\r\n", kPreviewDiskCacheVersion);
        w->w(hdr, xr_strlen(hdr));

        // Entries
        for (const auto& [base, e] : m_diskIndex)
        {
            string512 line;
            xr_sprintf(line, "%s|%u|%u|%s\r\n", base.c_str(), e.ogf_size, e.ogf_mtime,
                e.resolved_pose.size() ? e.resolved_pose.c_str() : "");
            w->w(line, xr_strlen(line));
        }
        FS.w_close(w);
    }
    m_diskCacheDirty = false;
}

bool CPreviewSceneRenderer::TryApplyDiskCache(SCacheEntry& entry)
{
    EnsureDiskCacheLoaded();

    // Use the entry's own settings, not the active grid's m_settings.
    const shared_str rt_name = MakeTextureName(entry.model_path.c_str(), entry.settings);
    pcstr rt_str = rt_name.c_str();
    if (!rt_str || !rt_str[0])
        return false;

    pcstr base_start = rt_str;
    if (strncmp(rt_str, "$user$", 6) == 0)
        base_start = rt_str + 6;

    const shared_str base(base_start);
    const auto it = m_diskIndex.find(base);
    if (it == m_diskIndex.end())
        return false;

    // Validate ogf size+mtime.
    string_path ogf_fn;
    u32 cur_size = 0, cur_mtime = 0;
    if (ResolveOgfPath(entry.model_path.c_str(), ogf_fn))
    {
        cur_size = (u32)FS.file_length(ogf_fn);
        cur_mtime = FS.get_file_age(ogf_fn);
    }
    else
    {
        m_diskIndex.erase(it);
        m_diskCacheDirty = true;
        return false;
    }

    if (cur_size != it->second.ogf_size || cur_mtime != it->second.ogf_mtime)
    {
        m_diskIndex.erase(it);
        m_diskCacheDirty = true;
        return false;
    }

    // Check that the DDS file exists on disk. FSType::Any also checks the
    // physical filesystem: freshly written files may be missing from the
    // locator registry in the same session.
    string512 dds_filename;
    xr_sprintf(dds_filename, "%s.dds", base.c_str());
    string_path dds_path;
    if (!ResolveCacheFilePath(dds_path, dds_filename))
    {
        m_diskIndex.erase(it);
        m_diskCacheDirty = true;
        return false;
    }
    if (!FS.exist(dds_path, FSType::Any))
    {
        m_diskIndex.erase(it);
        m_diskCacheDirty = true;
        return false;
    }

    // Valid disk cache hit. Mark ready with the file-texture name (no RT created).
    entry.texture_name = base;
    entry.resolved_pose_name = it->second.resolved_pose;
    entry.ready = true;
    entry.failed = false;
    entry.dirty = false;
    return true;
}

void CPreviewSceneRenderer::SaveToDiskCache(const SCacheEntry& entry, const ref_rt& rt)
{
#if !defined(USE_DX11)
    (void)entry; (void)rt;
    return;
#else
    if (!rt._get())
        return;

    EnsureDiskCacheLoaded();

    // Use the entry's own settings, not the active grid's m_settings.
    const shared_str rt_name = MakeTextureName(entry.model_path.c_str(), entry.settings);
    pcstr rt_str = rt_name.c_str();
    if (!rt_str || !rt_str[0])
        return;
    pcstr base_start = rt_str;
    if (strncmp(rt_str, "$user$", 6) == 0)
        base_start = rt_str + 6;
    const shared_str base(base_start);

    if (!EnsureDiskCacheDir())
        return;

    string512 dds_filename;
    xr_sprintf(dds_filename, "%s.dds", base.c_str());
    string_path dds_full_path;
    if (!ResolveCacheFilePath(dds_full_path, dds_filename))
        return;

    // Pass resolved full path (empty fs_root tells the saver to use it directly).
    if (!m_owner.SaveRtToDdsDxt5(rt, "", dds_full_path))
        return;

    // Resolve ogf stats for the index.
    string_path ogf_fn;
    SDiskCacheEntry disk_entry;
    if (ResolveOgfPath(entry.model_path.c_str(), ogf_fn))
    {
        disk_entry.ogf_size = (u32)FS.file_length(ogf_fn);
        disk_entry.ogf_mtime = FS.get_file_age(ogf_fn);
    }
    disk_entry.resolved_pose = entry.resolved_pose_name;

    m_diskIndex[base] = disk_entry;
    m_diskCacheDirty = true;
    FlushDiskIndex();
#endif
}

void CPreviewSceneRenderer::WipeDiskCache()
{
    m_diskIndex.clear();
    m_diskCacheDirty = false;
    m_diskCacheLoaded = true;
    m_diskCachePath[0] = '\0';
    m_warmCursor = 0;
    m_warmDone = false;

    if (FS.path_exist("$app_data_root$"))
        FS.dir_delete("$app_data_root$", "cached_dyn_textures", true);

    // Force all cached entries to re-render so they show fresh content.
    InvalidateCache();
    Msg("[preview_scene] disk cache wiped");
}

// ---------------------------------------------------------------------------
// Background warm-up: pre-load disk-cached preview DDS into VRAM (menu)
// ---------------------------------------------------------------------------

void CPreviewSceneRenderer::WarmDiskCacheTextures()
{
    if (m_warmDone)
        return;

    EnsureDiskCacheLoaded();
    if (m_diskIndex.empty())
        return;

    int warm_min = 0, warm_max = 500;
    const int v = Console ? Console->GetInteger("npc_preview_disk_cache_warm_per_frame", warm_min, warm_max) : 0;
    if (v <= 0)
        return;
    const u32 budget = static_cast<u32>(v);

    // Advance cursor into the ordered map. O(cursor) but total is typically
    // < 200 entries; the dominant cost is DDS I/O per texture.
    const u32 total = static_cast<u32>(m_diskIndex.size());
    auto it = m_diskIndex.begin();
    for (u32 s = _min(m_warmCursor, total); s > 0 && it != m_diskIndex.end(); ++it, --s)
        ; // advance

    u32 loaded = 0;
    while (it != m_diskIndex.end() && loaded < budget)
    {
        const shared_str& base = it->first;
        // _CreateTexture returns the existing CTexture if already registered,
        // or creates a new one. In the menu bDeferredLoad=TRUE, so the DDS
        // is not read yet — Load() forces synchronous I/O + GPU upload.
        CTexture* tex = m_owner.Resources->_CreateTexture(base.c_str());
        if (tex && !tex->flags.bLoaded)
            tex->Load();
        ++loaded;
        ++it;
        ++m_warmCursor;
    }

    if (it == m_diskIndex.end())
        m_warmDone = true;
}

void CPreviewSceneRenderer::ProcessQueue()
{
    FlushDeferredVisualDiscard();

    if (m_queue.empty())
        return;

    if (!EnsureRuntimeReady())
        return;

    if (!Device.b_is_Ready || !m_owner.Target || !m_owner.Resources)
        return;

    // Fix #7: read console budget once and cache it; avoids string hash lookup every frame
    if (!m_cached_budget)
    {
        int min_budget = 1;
        int max_budget = 500;
        const int v = Console ? Console->GetInteger("npc_preview_scene_budget_per_frame", min_budget, max_budget) : 0;
        m_cached_budget = (v > 0) ? static_cast<u32>(v) : 8u;
        clamp(m_cached_budget, 1u, 500u);
    }
    const u32 frame_budget = _min<u32>(m_cached_budget, (u32)m_queue.size());
    if (!frame_budget)
        return;

    // Fix #1: sort only when entries were added; no per-comparison map lookups
    if (m_queue_dirty)
    {
        std::stable_sort(m_queue.begin(), m_queue.end(),
            [](const SQueueEntry& lhs, const SQueueEntry& rhs)
            {
                if (lhs.priority != rhs.priority)
                    return lhs.priority < rhs.priority;
                return lhs.sequence < rhs.sequence;
            });
        m_queue_dirty = false;
    }

    // Process entries: disk-cache hits and already-ready entries are cheap
    // (stat + map lookup) and consume no render budget.  Only actual model
    // renders (RenderCachedEntry) count against the per-frame budget to
    // avoid FPS drops.  A safety scan limit prevents stalls on huge queues.
    constexpr u32 kScanLimit = 1024;
    const u32 count = _min<u32>(kScanLimit, static_cast<u32>(m_queue.size()));
    u32 rendered = 0;
    u32 consumed = 0;

    for (u32 i = 0; i < count; ++i)
    {
        const shared_str& key = m_queue[i].key;

        auto it = m_cache.find(key);
        if (it == m_cache.end())
        {
            ++consumed;
            continue;
        }

        auto& entry = it->second;
        entry.queued = false;
        // Attempt disk cache before (re-)rendering the model.
        if (!entry.ready || entry.dirty)
        {
            if (TryApplyDiskCache(entry))
            {
                ++consumed;
                continue;
            }
        }
        if (entry.ready && !entry.dirty)
        {
            ++consumed;
            continue;
        }
        // Needs a full model render — respect the budget.
        if (rendered >= frame_budget)
            break;
        RenderCachedEntry(entry);
        ++rendered;
        ++consumed;
    }
    m_queue.erase(m_queue.begin(), m_queue.begin() + consumed);

    // TryApplyDiskCache may erase stale index entries without re-saving them;
    // persist those removals so the next session doesn't see them either.
    if (m_diskCacheDirty && m_queue.empty())
        FlushDiskIndex();
}
} // namespace xray::render::RENDER_NAMESPACE
