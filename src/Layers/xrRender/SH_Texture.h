#pragma once

#include "xrCore/xr_resource.h"

class CAviPlayerCustom;
class ENGINE_API CTheoraSurface;

namespace xray::render::RENDER_NAMESPACE
{
class ECORE_API CTexture : public xr_resource_named
{
public:
    enum	MaxTextures
    {
        //	Actually these values are 128
        mtMaxPixelShaderTextures = 16,
        mtMaxVertexShaderTextures = 4,
        mtMaxGeometryShaderTextures = 16,
#ifdef USE_DX11
        mtMaxHullShaderTextures = 16,
        mtMaxDomainShaderTextures = 16,
        mtMaxComputeShaderTextures = 16,
#endif
        mtMaxCombinedShaderTextures =
        mtMaxPixelShaderTextures
        + mtMaxVertexShaderTextures
        + mtMaxGeometryShaderTextures
#ifdef USE_DX11
        + mtMaxHullShaderTextures
        + mtMaxDomainShaderTextures
        + mtMaxComputeShaderTextures
#endif
    };

#if defined(USE_DX11)
    //	Since DX11 allows up to 128 unique textures,
    //	distance between enum values should be at leas 128
    enum ResourceShaderType //	Don't change this since it's hardware-dependent
    {
        rstPixel = 0,
        // Default texture offset
        rstVertex = D3DVERTEXTEXTURESAMPLER0,
        rstGeometry = rstVertex + 256,
        rstHull = rstGeometry + 256,
        rstDomain = rstHull + 256,
        rstCompute = rstDomain + 256,
        rstInvalid = rstCompute + 256
    };
#elif defined(USE_OGL)
    //	Since OGL doesn't differentiate between stages,
    //	distance between enum values should be the max for that stage.
    enum ResourceShaderType
    {
        rstPixel = 0,	//	Default texture offset
        rstVertex = rstPixel + mtMaxPixelShaderTextures,
        rstGeometry = rstVertex + mtMaxVertexShaderTextures,
    };
#else
#   error No graphics API selected or enabled!
#endif

public:
    void apply_load(CBackend& cmd_list, u32 stage);
    void apply_theora(CBackend& cmd_list, u32 stage);
    void apply_avi(CBackend& cmd_list, u32 stage) const;
    void apply_seq(CBackend& cmd_list, u32 stage);
    void apply_normal(CBackend& cmd_list, u32 stage) const;

    void set_slice(int slice);

    void Preload();
    void Load();
    void PostLoad();
    void Unload();
    // void Apply(u32 dwStage);

#if defined(USE_DX11)
    void surface_set(ID3DBaseTexture* surf);
    [[nodiscard]] ID3DBaseTexture* surface_get() const;
#elif defined(USE_OGL)
    void surface_set(GLenum target, GLuint surf);
    [[nodiscard]] GLuint surface_get() const;
#else
#   error No graphics API selected or enabled!
#endif

    [[nodiscard]] BOOL isUser() const
    {
        return flags.bUser;
    }

    u32 get_Width()
    {
        async_ensure(); // 50r: the size of a texture still being read - finished now
        desc_enshure();
        return m_width;
    }

    u32 get_Height()
    {
        async_ensure();
        desc_enshure();
        return m_height;
    }

    // 50r-slow-disk-async-textures.md (plans/optimization_mt): the asynchronous loading of a texture on a slow disk
    // (CResourceManager::_AsyncTexture*, ResourceManager.cpp). A texture created in the game (or first bound in the
    // menu) is not read on the calling thread: the "Texture async" thread reads its file, a placeholder (no view) is
    // bound meanwhile, and the texture is put in place when created. Whoever needs it as a loaded one (Load, the size,
    // the surface) finishes it on the spot - a sync fallback. The state is the only thing read across threads.
    enum AsyncState : u8
    {
        asNone = 0, // not in the async loading: loaded or not, as before
        asQueued, // waits for the "Texture async" thread (a queue entry points at it)
        asReading, // the thread reads its file and, D3D11 with concurrent creates, creates it
        asWarm, // its file is in the OS cache: the main thread creates it (a budget per frame)
        asCreated, // created by the thread (m_async_*): the main thread puts it in place
        asInstalling, // being finished by one thread
        asReady, // in place: a loaded texture
    };
    std::atomic<u8> m_async_state{ asNone };
    std::atomic<u8> m_async_urgent{ 0 }; // bound with the placeholder: in the urgent queue too
    std::atomic<u8> m_async_entries{ 0 }; // queue entries pointing at it (changed under the queue lock)
    u32 m_async_t0{ 0 }; // queued at, ms (the loader's clock)
#if defined(USE_DX11)
    ID3DBaseTexture* m_async_surface{}; // asCreated: made by the thread
    ID3DShaderResourceView* m_async_srv{};
    u32 m_async_mem{};
#endif

    [[nodiscard]] bool async_pending() const
    {
        const u8 s = m_async_state.load(std::memory_order_acquire);
        return s != asNone && s != asReady;
    }
    // A texture of the async loading as a loaded one, now (any thread); cheap when it is not pending
    void async_ensure()
    {
        if (async_pending())
            async_finish_now();
    }
    // ResourceManager.cpp: finishes a pending texture on the calling thread (waits for the thread if it is reading this
    // one); true = in place now (or was), false = not in the async loading (any more): load it as before
    bool async_finish_now();
    // ResourceManager.cpp: the texture leaves the async loading (Unload, surface_set): waits for the thread or a
    // finisher working on it, drops its queue entries and what the thread created
    void async_detach();
    // the render API files: the thread's part (D3D11: texture_load + the view into m_async_*; GL: false - created on
    // main), putting m_async_* in place (the claimer of asInstalling), dropping them
    bool async_create();
    void async_install();
    void async_release_created();
    // the former body of Load: loads it now on the calling thread
    void load_now();
    // ResourceManager.cpp: a bind of a texture not loaded yet (apply_load). true = the async loading has it (queued
    // now or pending): the placeholder is bound; false = load it as before
    bool async_bind(CBackend& cmd_list, u32 stage);
    void async_bind_placeholder(CBackend& cmd_list, u32 stage); // the render API files

    // 50a-stage1-prefetch.md (plans/optimization_mt): Load reads no file of this texture now - it is created already
    // (pSurface: loaded earlier, by the prefetch stage or on the previous level; a surface set from outside; a video:
    // load_now returns at once) or put in place by the async loading (asReady: Load returns at once). DeferredUpload
    // asks it on the main thread before the loads start, to keep the "Texture warm" reader of a slow disk off such
    // textures. Only a hint: a texture taken for created that is not reads its file in its own Load, as without the reader.
    [[nodiscard]] bool load_reads_no_file() const
    {
        return pSurface || m_async_state.load(std::memory_order_acquire) == asReady;
    }

    void video_Sync(u32 _time) { m_play_time = _time; }
    void video_Play(BOOL looped, u32 _time = 0xFFFFFFFF);
    void video_Pause(BOOL state) const;
    void video_Stop() const;
    [[nodiscard]] BOOL video_IsPlaying() const;

    CTexture();
    virtual ~CTexture();

#if defined(USE_DX11)
    ID3DShaderResourceView* get_SRView() const { return m_pSRView; }
#endif

#if defined(USE_DX11)
    ImTextureID GetImTextureID()
    {
        if (!flags.bLoaded)
            Load();
        return reinterpret_cast<ImTextureID>(m_pSRView);
    }
#elif defined(USE_OGL)
    ImTextureID GetImTextureID()
    {
        if (!flags.bLoaded)
            Load();
        return static_cast<ImTextureID>(pSurface);
    }
#else
#   error No graphics API selected or enabled!
#endif

private:
    [[nodiscard]] BOOL desc_valid() const
    {
        return pSurface == desc_cache;
    }

    void desc_enshure()
    {
        if (!desc_valid())
            desc_update();
    }

    void desc_update();
#if defined(USE_DX11)
    void Apply(CBackend& cmd_list, u32 dwStage) const;
    D3D_USAGE GetUsage();
#endif

    //	Class data
public: //	Public class members (must be encapsulated further)
    struct
    {
        u32 bLoaded : 1;
        u32 bUser : 1;
        u32 seqCycles : 1;
        u32 MemoryUsage : 28;
    } flags;

    fastdelegate::FastDelegate2<CBackend&,u32> bind;

    CAviPlayerCustom* pAVI;
    CTheoraSurface* pTheora;
    float m_material;
    shared_str m_bumpmap;

    union
    {
        u32 m_play_time; // sync theora time
        u32 seqMSPF; // Sequence data milliseconds per frame
    };

    int curr_slice{ -1 };
    int last_slice{ -1 };

private:
#if defined(USE_DX11)
    ID3DBaseTexture* pSurface{};
    ID3DBaseTexture* pTempSurface{};
    // Sequence data
    xr_vector<ID3DBaseTexture*> seqDATA;

    // Description
    u32 m_width;
    u32 m_height;
    ID3DBaseTexture* desc_cache;
    D3D_TEXTURE2D_DESC desc;
#elif defined(USE_OGL)
    GLuint pSurface;
    GLuint pBuffer;
    // Sequence data
    xr_vector<GLuint> seqDATA;
    // Description
    GLint m_width;
    GLint m_height;
    GLuint desc_cache;
    GLenum desc;
#else
#   error No graphics API selected or enabled!
#endif

#if defined(USE_DX11)
    ID3DShaderResourceView* m_pSRView{ nullptr };
    ID3DShaderResourceView* srv_all{ nullptr };
    xr_vector<ID3DShaderResourceView*> srv_per_slice;
    // Sequence view data
    xr_vector<ID3DShaderResourceView*> m_seqSRView;
#endif
};

struct resptrcode_texture : public resptr_base<CTexture>
{
    void create(LPCSTR _name);
    void destroy() { _set(nullptr); }
    shared_str bump_get() { return _get()->m_bumpmap; }
    bool bump_exist() { return 0 != bump_get().size(); }
};

typedef resptr_core<CTexture, resptrcode_texture> ref_texture;
} // namespace xray::render::RENDER_NAMESPACE
