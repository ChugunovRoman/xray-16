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
    // Delete the entire disk preview cache and reset in-memory state.
    void WipeDiskCache();
    // Pre-load disk-cached preview DDS textures into VRAM (called in menu).
    void WarmDiskCacheTextures();

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

    // Entry in the on-disk index (index.txt). Key = base texture name.
    struct SDiskCacheEntry
    {
        u32 ogf_size{};
        u32 ogf_mtime{};
        shared_str resolved_pose;
    };

    static constexpr u32 kPreviewDiskCacheVersion = 1;
    static constexpr pcstr kPreviewCacheAlias = "$preview_cache$";
    static constexpr pcstr kPreviewCacheIndexFile = "index.txt";

    // Resolved full path to the cache directory (filled once on first use).
    string_path m_diskCachePath{};

    // Safely resolve a filename inside the preview cache directory into a full
    // path.  Returns false if the $preview_cache$ alias is not registered.
    bool ResolveCacheFilePath(string_path& dest, pcstr filename) const;
    // Resolve the directory itself; caches the result in m_diskCachePath.
    bool EnsureDiskCacheDir();

    void EnsureDiskCacheLoaded();
    // Returns true if a valid disk-cached preview exists for the entry.
    bool TryApplyDiskCache(SCacheEntry& entry);
    // Persist a rendered preview RT to disk and update the index.
    void SaveToDiskCache(const SCacheEntry& entry, const ref_rt& rt);
    // Resolve the absolute FS path of the ogf for stat checks.
    bool ResolveOgfPath(pcstr model_path, string_path& out_fn);
    // Write the in-memory index back to disk.
    void FlushDiskIndex();

    bool RenderRenderableImpl(IRenderable* subject, const Fmatrix& view, const Fmatrix& proj, bool prepare_kinematics);
    bool RenderRenderableImpl(
        IRenderable* subject, const Fmatrix& view, const Fmatrix& proj, bool prepare_kinematics, const ref_rt* color_rt_override);
    bool EnsureRuntimeReady();
    bool EnsureRuntimeReady(const SPreviewSceneSettings& settings);
    void EnsureResolveShader();
    bool ResolveOutput(ref_rt& output_rt, u32 width, u32 height);
    void DestroyRuntime(bool preserve_requests);
    void RequeueCachedEntries();
    // Preview models are created per render and discarded right after, so only
    // the resulting preview textures stay cached. CModelPool keeps both the
    // instance and the base geometry resident on a regular (non-discard) delete,
    // which pinned gigabytes of actor models and their textures in memory.
    void DiscardPreviewVisual(IRenderVisual*& visual);
    void FlushDeferredVisualDiscard();
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
    xr_vector<IRenderVisual*> m_deferredVisualDiscard;
    // Disk preview cache: lazily loaded on first ScheduleModel.
    xr_map<shared_str, SDiskCacheEntry> m_diskIndex;
    bool m_diskCacheLoaded{false};
    bool m_diskCacheDirty{false};
    u64 m_sequence{};
    u32 m_cached_budget{};
    bool m_queue_dirty{};
    // One-shot background warm-up state (menu pre-loading).
    u32 m_warmCursor{};
    bool m_warmDone{};
};
} // namespace xray::render::RENDER_NAMESPACE
