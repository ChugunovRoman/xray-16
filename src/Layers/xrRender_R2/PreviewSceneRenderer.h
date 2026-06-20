#pragma once

#include "Layers/xrRender/SH_RT.h"
#include "xrEngine/Render.h"
#include "xrCommon/xr_map.h"
#include "xrCommon/xr_vector.h"

class IRenderable;

namespace xray::render::RENDER_NAMESPACE
{
class CRender;

class CPreviewSceneRenderer
{
public:
    friend class CRender;

    explicit CPreviewSceneRenderer(CRender& owner);
    ~CPreviewSceneRenderer() = default;

    void Initialize();
    void Shutdown();
    [[nodiscard]] bool IsReady() const;
    void ResetBegin();
    void ResetEnd();
    bool RenderRenderable(IRenderable* subject, const Fmatrix& view, const Fmatrix& proj);
    bool RenderModel(pcstr model_path, const Fmatrix& view, const Fmatrix& proj);
    bool RenderModelNoCache(pcstr model_path, shared_str& out_texture_name);
    void ScheduleModel(pcstr model_path, u32 priority = 1000);
    void ProcessQueue();
    [[nodiscard]] bool IsCached(pcstr model_path) const;
    [[nodiscard]] bool IsDirty(pcstr model_path) const;
    [[nodiscard]] shared_str TextureName(pcstr model_path) const;
    [[nodiscard]] shared_str ResolvedPoseName(pcstr model_path) const;
    void CollectCycleNames(pcstr model_path, xr_vector<shared_str>& out_cycles);
    void ReleaseEphemeralTexture(pcstr texture_name);
    void SetSettings(const SPreviewSceneSettings& settings);
    [[nodiscard]] SPreviewSceneSettings GetSettings() const { return m_settings; }

    [[nodiscard]]
    const ref_rt& ColorRT() const
    {
        return m_runtime.color_rt;
    }

    [[nodiscard]]
    const ref_rt& DepthRT() const
    {
        return m_runtime.depth_rt;
    }

private:
    struct SRuntime
    {
        bool ready{};
        u32 width{};
        u32 height{};
        ref_rt position_rt{};
        ref_rt normal_rt{};
        ref_rt albedo_rt{};
        ref_rt color_rt{};
        ref_rt depth_rt{};
        ref_shader resolve_shader{};

        [[nodiscard]] bool HasAnyResource() const
        {
            return !!position_rt || !!normal_rt || !!albedo_rt || !!color_rt || !!depth_rt;
        }

        void Destroy()
        {
            position_rt.destroy();
            normal_rt.destroy();
            albedo_rt.destroy();
            color_rt.destroy();
            depth_rt.destroy();
            resolve_shader.destroy();
            width = 0;
            height = 0;
            ready = false;
        }
    };

    struct SCacheEntry
    {
        shared_str model_path;
        shared_str texture_name;
        SPreviewSceneSettings settings{};
        ref_rt color_rt;
        IRenderVisual* cached_visual{nullptr};
        u32 priority{1000};
        u64 sequence{};
        bool queued{false};
        bool ready{false};
        bool dirty{false};
        bool failed{false};
        shared_str resolved_pose_name;
    };

    struct SQueueEntry
    {
        shared_str key;
        u32 priority{};
        u64 sequence{};
    };

    struct SEphemeralEntry
    {
        shared_str texture_name;
        ref_rt color_rt;
    };

    bool RenderRenderableImpl(IRenderable* subject, const Fmatrix& view, const Fmatrix& proj, bool prepare_kinematics);
    bool RenderRenderableImpl(
        IRenderable* subject, const Fmatrix& view, const Fmatrix& proj, bool prepare_kinematics, const ref_rt* color_rt_override);
    bool EnsureRuntimeReady();
    bool EnsureRuntimeReady(const SPreviewSceneSettings& settings);
    void EnsureResolveShader();
    bool ResolveOutput(ref_rt& output_rt, u32 width, u32 height);
    void DestroyRuntime(bool preserve_requests);
    void RequeueCachedEntries();
    void InvalidateCache();
    [[nodiscard]] shared_str MakeTextureName(pcstr model_path) const;
    [[nodiscard]] shared_str MakeTextureName(pcstr model_path, const SPreviewSceneSettings& settings) const;
    [[nodiscard]] shared_str MakeCacheKey(pcstr model_path, const SPreviewSceneSettings& settings) const;
    bool RenderCachedEntry(SCacheEntry& entry);
    shared_str MakeNoCacheTextureName(pcstr model_path);
    bool RenderModelIntoRT(
        pcstr model_path, ref_rt& output_rt, const SPreviewSceneSettings& settings, shared_str* resolved_pose_name = nullptr);
    bool RenderModelIntoRT(
        IRenderVisual* visual, ref_rt& output_rt, const SPreviewSceneSettings& settings, shared_str* resolved_pose_name = nullptr);

private:
    CRender& m_owner;
    SRuntime m_runtime;
    SPreviewSceneSettings m_settings{};
    xr_map<shared_str, SCacheEntry> m_cache;
    xr_vector<SQueueEntry> m_queue;
    xr_vector<SEphemeralEntry> m_ephemeral;
    u64 m_sequence{};
    u32 m_cached_budget{};
    bool m_queue_dirty{};
};
} // namespace xray::render::RENDER_NAMESPACE
