#pragma once

#include "Layers/xrRender/HWCaps.h"
#include "xrCore/ModuleLookup.hpp"

namespace xray::render::RENDER_NAMESPACE
{
class CHW
    : public pureAppActivate,
      public pureAppDeactivate
{
public:
    CHW();
    ~CHW();

    void CreateDevice(SDL_Window* sdlWnd);
    void DestroyDevice();

    void Reset();

    void SetPrimaryAttributes(u32& windowFlags);

    IRender::RenderContext GetCurrentContext() const;
    int  MakeContextCurrent(IRender::RenderContext context) const;
    [[nodiscard]] bool HasLoaderContext() const noexcept { return m_loaderWindow != nullptr && m_loaderContext != nullptr; }

    static std::pair<u32, u32> GetSurfaceSize();
    DeviceState GetDeviceState() const;

    // 50o (plans/optimization_mt/50o-gpu-frame-timing.md): the GPU time of every engine frame, always on (Release and
    // Master Gold too). Two GL_TIMESTAMP queries (glQueryCounter) around the frame's work on the primary context
    // (BeginScene .. the blit of Present), a ring of GPU_TIMER_SETS sets polled with GL_QUERY_RESULT_AVAILABLE after
    // the swap, never waited for. Each result -> xray::perf::GpuFrameTimeSubmit with the Device.dwFrame the queries
    // were issued in; the CPU time of the swap -> PresentWaitSubmit, the render's sync point wait (AddGpuSyncWait) ->
    // GpuSyncWaitSubmit, the GPU idle time since the previous frame -> GpuIdleGapSubmit. With XR_PERF_METRICS also the
    // render/gpu/* metrics, the pass spans from GpuMarkPass among them. Without GL 3.3 / ARB_timer_query: nothing.
    enum class GpuMark : u8
    {
        MainBegin,  // CRender::Render, main pass: entry
        Lights,     // main pass: the sun starts (cascades + accumulate), then the shadowed lights
        Combine,    // main pass: phase_combine starts
        CombineEnd, // main pass: phase_combine done
        SvpBegin,   // CRender::RenderSecondViewport: entry
        SvpEnd,     // CRender::RenderSecondViewport: done
        Count
    };
    // A timestamp at a pass boundary of this frame. Issues anything only in builds with XR_PERF_METRICS while
    // perf_metrics is on.
    void GpuMarkPass(GpuMark mark);
    // CPU time the frame's thread waited for the GPU in the render's sync point (R_sync_point::Wait), summed over the
    // passes of the frame; published with the frame in Present.
    void AddGpuSyncWait(float ms) { m_gpuSyncWaitMs += ms; }

public:
    void BeginScene();
    void EndScene();
    void Present();

public:
    void OnAppActivate() override;
    void OnAppDeactivate() override;

private:
    void UpdateViews();
    bool ThisInstanceIsGlobal() const;

    // 50o: the GPU frame timer (see GpuMark)
    void GpuTimerCreate();
    void GpuTimerDestroy();
    void GpuTimerBegin();
    void GpuTimerEnd();
    void GpuTimerPoll();

    // The sets are taken and read back strictly in order: m_gpuSetOldest is the oldest one in flight, m_gpuSetsUsed
    // the sets in flight (the open one included).
    static constexpr u32 GPU_TIMER_SETS = 6;
    static constexpr u32 GPU_TIMER_TS = 2 + static_cast<u32>(GpuMark::Count); // frame begin, frame end, pass marks
    struct GpuTimerSet
    {
        GLuint ts[GPU_TIMER_TS]{};
        u32 frame{};     // Device.dwFrame the queries were issued in
        u32 marks{};     // bit i: ts[2 + i] (GpuMark i) was issued in this frame
        bool discard{};  // closed without its Present (a BeginScene came first): read back, not published
    };
    GpuTimerSet m_gpuSets[GPU_TIMER_SETS]{};
    u32 m_gpuSetOldest{};
    u32 m_gpuSetsUsed{};
    bool m_gpuTimerOk{};
    bool m_gpuSetOpen{};      // the newest set is between GpuTimerBegin and GpuTimerEnd
    u64 m_gpuPrevEnd{};       // end timestamp of the last frame read back (the idle gap of the next one)
    u32 m_gpuPrevFrame{u32(-1)};
    float m_gpuSyncWaitMs{};

public:
    void BeginPixEvent(pcstr name) const;
    void EndPixEvent() const;

public:
    static constexpr auto IMM_CTX_ID = 0;

    CHWCaps Caps;

    u32 BackBufferCount{};
    u32 CurrentBackBuffer{};

    GLuint pFB{};

    SDL_Window* m_window{};
    /** Separate hidden window so loader GL context is not competing with primary on the same HWND (WGL ERROR_BUSY). */
    SDL_Window* m_loaderWindow{};

    SDL_GLContext m_context{};
    SDL_GLContext m_loaderContext{};

    pcstr AdapterName;
    pcstr OpenGLVersionString;
    pcstr ShadingVersion;
    bool ComputeShadersSupported;
};

extern ECORE_API CHW HW;
} // namespace xray::render::RENDER_NAMESPACE
