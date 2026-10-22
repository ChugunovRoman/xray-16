// glHW.cpp: implementation of the OpenGL specialisation of CHW.
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#pragma hdrstop

#include "glHW.h"
#include "xrEngine/XR_IOConsole.h"
#include "xrCore/Platform/HwProfile.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "xrCore/Profiling/SysDiag.hpp"

#include <chrono>

namespace xray::render::RENDER_NAMESPACE
{
CHW HW;

void CALLBACK OnDebugCallback(GLenum /*source*/, GLenum /*type*/, GLuint id, GLenum severity, GLsizei /*length*/,
    const GLchar* message, const void* /*userParam*/)
{
    if (severity != GL_DEBUG_SEVERITY_NOTIFICATION)
        Log(message, id);
}

static_assert(std::is_same_v<decltype(&OnDebugCallback), GLDEBUGPROC>);

// Hardware profile, stage C (plans/optimization_mt/50-smart-engine-hw-profile.md): what GL says about the GPU, for
// the log, the tiers and the policy. GL has one context: driver_cl / driver_cc stay 0.
static void SubmitHwProfileGpuGL(bool probe)
{
    const auto vendor = reinterpret_cast<pcstr>(glGetString(GL_VENDOR));
    const auto renderer = reinterpret_cast<pcstr>(glGetString(GL_RENDERER));
    const auto contains = [](pcstr text, pcstr what) { return text && strstr(text, what) != nullptr; };

    xray::hw::Gpu gpu{};
    gpu.size = sizeof(gpu);
    gpu.api = xray::hw::GpuApi::OpenGL;
    if (renderer)
        xr_strcpy(gpu.name, renderer);
    // PCI vendor ids by the vendor string (GL has no ids)
    if (contains(vendor, "NVIDIA"))
        gpu.vendor = 0x10DE;
    else if (contains(vendor, "AMD") || contains(vendor, "ATI"))
        gpu.vendor = 0x1002;
    else if (contains(vendor, "Intel"))
        gpu.vendor = 0x8086;
    else if (contains(vendor, "Apple"))
        gpu.vendor = 0x106B;
    gpu.software = (contains(renderer, "llvmpipe") || contains(renderer, "softpipe") ||
        contains(renderer, "SwiftShader") || contains(renderer, "GDI Generic")) ? 1 : 0;
    // UMA by the name (heuristic): Apple GPUs and Intel ones except Arc
    gpu.uma = (gpu.vendor == 0x106B || (gpu.vendor == 0x8086 && !contains(renderer, "Arc"))) ? 1 : 0;

    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    gpu.feature_level = u16(((major & 0xF) << 12) | ((minor & 0xF) << 8));

    // Video memory, KB: NVIDIA - dedicated and currently available; AMD - free texture memory
    if (GLAD_GL_NVX_gpu_memory_info)
    {
        GLint dedicated_kb = 0, available_kb = 0;
        glGetIntegerv(GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &dedicated_kb);
        glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &available_kb);
        gpu.dedicated = u64(std::max(dedicated_kb, 0)) * 1024;
        gpu.budget_local_start = u64(std::max(available_kb, 0)) * 1024;
    }
    else if (GLAD_GL_ATI_meminfo)
    {
        GLint free_kb[4]{};
        glGetIntegerv(GL_TEXTURE_FREE_MEMORY_ATI, free_kb);
        gpu.budget_local_start = u64(std::max(free_kb[0], 0)) * 1024;
        gpu.dedicated = gpu.budget_local_start;
    }
    // an unsupported query must not reach the CHK_GL of the next call
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {}

    gpu.width = psDeviceMode.Width;
    gpu.height = psDeviceMode.Height;
    gpu.refresh = psDeviceMode.RefreshRate;
    xray::hw::SubmitGpu(gpu, probe);
}

void UpdateVSync()
{
    if (psDeviceFlags.test(rsVSync))
    {
        // Try adaptive vsync first
        if (SDL_GL_SetSwapInterval(-1) == -1)
            SDL_GL_SetSwapInterval(1);
    }
    else
    {
        SDL_GL_SetSwapInterval(0);
    }
}

CHW::CHW()
{
    if (!ThisInstanceIsGlobal())
        return;

    Device.seqAppActivate.Add(this);
    Device.seqAppDeactivate.Add(this);
}

CHW::~CHW()
{
    if (!ThisInstanceIsGlobal())
        return;

    Device.seqAppActivate.Remove(this);
    Device.seqAppDeactivate.Remove(this);
}

void CHW::OnAppActivate()
{
    if (m_window)
    {
        SDL_RestoreWindow(m_window);
    }
}

void CHW::OnAppDeactivate()
{
    if (m_window)
    {
        if (psDeviceMode.WindowStyle == rsFullscreen || psDeviceMode.WindowStyle == rsFullscreenBorderless)
            SDL_MinimizeWindow(m_window);
    }
}

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
void CHW::CreateDevice(SDL_Window* hWnd)
{
    ZoneScoped;

    m_window = hWnd;

    R_ASSERT(m_window);

    // Choose the closest pixel format
    SDL_DisplayMode mode;
    SDL_GetWindowDisplayMode(m_window, &mode);
    mode.format = SDL_PIXELFORMAT_RGBA8888;
    // Apply the pixel format to the device context
    SDL_SetWindowDisplayMode(m_window, &mode);

    Caps.fTarget = D3DFMT_A8R8G8B8;
    Caps.fDepth = D3DFMT_D24S8;

    // Create the context
    m_context = SDL_GL_CreateContext(m_window);
    if (m_context == nullptr)
    {
        Log("! OpenGL: could not create drawing context:", SDL_GetError());
        return;
    }

    if (MakeContextCurrent(IRender::PrimaryContext) != 0)
    {
        Log("! OpenGL: could not make context current:", SDL_GetError());
        return;
    }

    int version;
    {
        ZoneScopedN("gladLoadGL");
        version = gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress));
    }
    if (version == 0)
    {
        Log("! OpenGL: could not initialize GLAD.");
        if (const auto err = SDL_GetError())
            Log("SDL Error:", err);
        return;
    }

    if (ThisInstanceIsGlobal())
    {
        UpdateVSync();

#ifdef DEBUG
        if (glDebugMessageCallback)
        {
            CHK_GL(glEnable(GL_DEBUG_OUTPUT));
            CHK_GL(glDebugMessageCallback((GLDEBUGPROC)OnDebugCallback, nullptr));
        }
#endif // DEBUG
    }

    int iMaxVTFUnits, iMaxCTIUnits;
    glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &iMaxVTFUnits);
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &iMaxCTIUnits);

    AdapterName = reinterpret_cast<pcstr>(glGetString(GL_RENDERER));
    OpenGLVersionString = reinterpret_cast<pcstr>(glGetString(GL_VERSION));
    ShadingVersion = reinterpret_cast<pcstr>(glGetString(GL_SHADING_LANGUAGE_VERSION));

    Msg("* GPU vendor: [%s] device: [%s]", glGetString(GL_VENDOR), AdapterName);
    Msg("* GPU OpenGL version: %s", OpenGLVersionString);
    Msg("* GPU OpenGL shading language version: %s", ShadingVersion);
    Msg("* GPU OpenGL VTF units: [%d] CTI units: [%d]", iMaxVTFUnits, iMaxCTIUnits);

    ComputeShadersSupported = false; // XXX: Implement compute shaders support

    SubmitHwProfileGpuGL(!ThisInstanceIsGlobal());

    if (glGenFramebuffers && glBindFramebuffer)
        UpdateViews();

    // Secondary context shares object namespace with primary. Must use a *separate* SDL window: two GL
    // contexts on the same HWND cannot reliably be current on different threads (WGL: resource in use).
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    m_loaderWindow =
        SDL_CreateWindow("XR_GL_SharedLoader", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1, 1,
            SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL);
    if (!m_loaderWindow)
    {
        Log("! OpenGL: could not create hidden loader window:", SDL_GetError());
    }
    else
    {
        m_loaderContext = SDL_GL_CreateContext(m_loaderWindow);
        if (!m_loaderContext)
            Log("! OpenGL: could not create secondary shared (loader) context:", SDL_GetError());
    }

    // SDL makes the newly created GL context current; we must bind primary again or the frame is
    // rendered into the loader's 1x1 window while SwapWindow targets the main surface (black screen).
    if (MakeContextCurrent(IRender::PrimaryContext) != 0)
        Log("! OpenGL: could not restore primary context after loader setup:", SDL_GetError());

    // 50o: the GPU frame timer, on the primary context (query objects are not shared between contexts); only the
    // global HW: the startup probe renders no frames
    if (ThisInstanceIsGlobal())
        GpuTimerCreate();
}

void CHW::DestroyDevice()
{
    GpuTimerDestroy();

    CHK_GL(glDeleteFramebuffers(1, &pFB));
    pFB = 0;

    const auto context = SDL_GL_GetCurrentContext();
    if (context == m_context || context == m_loaderContext)
        SDL_GL_MakeCurrent(nullptr, nullptr);

    if (m_loaderContext)
    {
        SDL_GL_DeleteContext(m_loaderContext);
        m_loaderContext = nullptr;
    }

    if (m_loaderWindow)
    {
        SDL_DestroyWindow(m_loaderWindow);
        m_loaderWindow = nullptr;
    }

    SDL_GL_DeleteContext(m_context);
    m_context = nullptr;
}

//////////////////////////////////////////////////////////////////////
// Resetting device
//////////////////////////////////////////////////////////////////////
void CHW::Reset()
{
    ZoneScoped;

    CHK_GL(glDeleteFramebuffers(1, &pFB));
    pFB = 0;
    UpdateViews();

    UpdateVSync();
}

void CHW::SetPrimaryAttributes(u32& windowFlags)
{
    windowFlags |= SDL_WINDOW_OPENGL;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    if (!strstr(Core.Params, "-no_gl_context"))
    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    }
}

IRender::RenderContext CHW::GetCurrentContext() const
{
    const auto context = SDL_GL_GetCurrentContext();
    if (context == m_context)
        return IRender::PrimaryContext;
    if (context == m_loaderContext)
        return IRender::HelperContext;
    return IRender::NoContext;
}

int CHW::MakeContextCurrent(IRender::RenderContext context) const
{
    switch (context)
    {
    case IRender::NoContext:
        return SDL_GL_MakeCurrent(nullptr, nullptr);

    case IRender::PrimaryContext:
        return SDL_GL_MakeCurrent(m_window, m_context);

    case IRender::HelperContext:
        if (!m_loaderWindow || !m_loaderContext)
            return -1;
        return SDL_GL_MakeCurrent(m_loaderWindow, m_loaderContext);

    default:
        NODEFAULT;
    }
    return -1;
}

void CHW::UpdateViews()
{
    // Create the default framebuffer
    glGenFramebuffers(1, &pFB);
    CHK_GL(glBindFramebuffer(GL_FRAMEBUFFER, pFB));

    BackBufferCount = 1;
}

// ---------------------------------------------------------------------------------------------------------------------
// 50o (plans/optimization_mt/50o-gpu-frame-timing.md): the GPU frame timer, see CHW::GpuMark.
namespace
{
#if XR_PERF_METRICS
// render/gpu/* are perf_metrics timers (Ticks()): GPU milliseconds in the calibration perf_metrics uses for its own
void AddGpuMs(xray::perf::Metric m, double ms)
{
    constexpr u64 probe_ticks = u64(1) << 24;
    const double ms_per_tick = xray::perf::TicksToMilliseconds(probe_ticks) / static_cast<double>(probe_ticks);
    if (ms_per_tick > 0.0 && ms >= 0.0)
        xray::perf::AddTimeSlow(m, static_cast<u64>(ms / ms_per_tick));
}
#endif

// GL_TIMESTAMP values are nanoseconds
constexpr double kGlTimestampToMs = 1e-6;
} // namespace

void CHW::GpuTimerCreate()
{
    GpuTimerDestroy();

    // GL_TIMESTAMP + glQueryCounter: core in 3.3, ARB_timer_query before. A pair of timestamps rather than
    // GL_TIME_ELAPSED: those cannot nest or overlap each other, the pass marks and the idle gap need the timestamps.
    if (!(GLAD_GL_VERSION_3_3 || GLAD_GL_ARB_timer_query) || !glQueryCounter || !glGetQueryObjectui64v ||
        !glGetQueryiv || !glGenQueries || !glDeleteQueries)
    {
        Msg("* [gpu] frame timing: unavailable (no GL 3.3 / ARB_timer_query)");
        return;
    }
    GLint bits = 0;
    glGetQueryiv(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &bits);
    // an unsupported query must not reach the CHK_GL of the next call
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {}
    if (bits <= 0)
    {
        Msg("* [gpu] frame timing: unavailable (GL_TIMESTAMP has no counter bits)");
        return;
    }
    // The pass marks only where something reads them (XR_PERF_METRICS); 2 timestamps per frame otherwise
    constexpr GLsizei ts_count = XR_PERF_METRICS ? GPU_TIMER_TS : 2;
    for (GpuTimerSet& set : m_gpuSets)
        glGenQueries(ts_count, set.ts);
    m_gpuTimerOk = true;
    Msg("* [gpu] frame timing: GL_TIMESTAMP queries (%d-bit counter)", bits);
}

void CHW::GpuTimerDestroy()
{
    // Deleting needs the context that owns them; without it they go with the context
    const bool own_context = m_context && SDL_GL_GetCurrentContext() == m_context;
    for (GpuTimerSet& set : m_gpuSets)
    {
        if (own_context && set.ts[0])
            glDeleteQueries(GPU_TIMER_TS, set.ts); // zeros (the pass marks without XR_PERF_METRICS) are ignored
        set = {};
    }
    m_gpuSetOldest = 0;
    m_gpuSetsUsed = 0;
    m_gpuSetOpen = false;
    m_gpuTimerOk = false;
    m_gpuPrevFrame = u32(-1);
}

void CHW::GpuTimerBegin()
{
    if (!m_gpuTimerOk || SDL_GL_GetCurrentContext() != m_context)
        return; // the queries belong to the primary context
    if (m_gpuSetOpen)
    {
        // A BeginScene without the Present of the previous one: close that set, its result is not a frame
        GpuTimerSet& open = m_gpuSets[(m_gpuSetOldest + m_gpuSetsUsed - 1) % GPU_TIMER_SETS];
        glQueryCounter(open.ts[1], GL_TIMESTAMP);
        open.discard = true;
        m_gpuSetOpen = false;
    }
    if (m_gpuSetsUsed == GPU_TIMER_SETS)
    {
        // Every set still waits for its result (the GPU is more than GPU_TIMER_SETS frames behind): this frame goes
        // unmeasured rather than waiting
        PERF_INC(rgpu_skipped);
        return;
    }
    GpuTimerSet& set = m_gpuSets[(m_gpuSetOldest + m_gpuSetsUsed) % GPU_TIMER_SETS];
    ++m_gpuSetsUsed;
    m_gpuSetOpen = true;
    set.frame = Device.dwFrame;
    set.marks = 0;
    set.discard = false;
    glQueryCounter(set.ts[0], GL_TIMESTAMP);
}

void CHW::GpuTimerEnd()
{
    if (!m_gpuSetOpen)
        return;
    GpuTimerSet& set = m_gpuSets[(m_gpuSetOldest + m_gpuSetsUsed - 1) % GPU_TIMER_SETS];
    glQueryCounter(set.ts[1], GL_TIMESTAMP);
    m_gpuSetOpen = false;
}

void CHW::GpuMarkPass(GpuMark mark)
{
#if XR_PERF_METRICS
    if (!m_gpuSetOpen || !xray::perf::g_perf_metrics_active || mark >= GpuMark::Count)
        return;
    if (SDL_GL_GetCurrentContext() != m_context)
        return; // the queries belong to the primary context
    GpuTimerSet& set = m_gpuSets[(m_gpuSetOldest + m_gpuSetsUsed - 1) % GPU_TIMER_SETS];
    glQueryCounter(set.ts[2 + static_cast<u32>(mark)], GL_TIMESTAMP);
    set.marks |= 1u << static_cast<u32>(mark);
#else
    UNUSED(mark);
#endif
}

void CHW::GpuTimerPoll()
{
    // Oldest first; the first set that is not ready ends the poll (the newer ones are not either). Only
    // GL_QUERY_RESULT_AVAILABLE before GL_QUERY_RESULT: never a wait here.
    if (!m_gpuSetsUsed || SDL_GL_GetCurrentContext() != m_context)
        return;
    while (m_gpuSetsUsed > (m_gpuSetOpen ? 1u : 0u))
    {
        GpuTimerSet& set = m_gpuSets[m_gpuSetOldest];
        const u32 need = 3u | (set.marks << 2);
        u64 t[GPU_TIMER_TS]{};
        // the end timestamp first: issued last (but the marks), it is the one that decides
        for (u32 n = 0; n < GPU_TIMER_TS; ++n)
        {
            const u32 i = n == 0 ? 1 : n == 1 ? 0 : n;
            if (!(need & (1u << i)))
                continue;
            GLint available = 0;
            glGetQueryObjectiv(set.ts[i], GL_QUERY_RESULT_AVAILABLE, &available);
            if (!available)
                return;
            GLuint64 value = 0;
            glGetQueryObjectui64v(set.ts[i], GL_QUERY_RESULT, &value);
            t[i] = value;
        }
        const double frame_ms = t[1] >= t[0] ? static_cast<double>(t[1] - t[0]) * kGlTimestampToMs : -1.0;
        if (!set.discard && frame_ms >= 0.0 && frame_ms < 60000.0)
        {
            xray::perf::GpuFrameTimeSubmit(set.frame, static_cast<float>(frame_ms));
            // The GPU had nothing to do between the previous frame's end and this one's begin
            const bool gap_ok = m_gpuPrevFrame != u32(-1) && m_gpuPrevFrame + 1 == set.frame && t[0] >= m_gpuPrevEnd;
            const double gap_ms = gap_ok ? static_cast<double>(t[0] - m_gpuPrevEnd) * kGlTimestampToMs : 0.0;
            if (gap_ok)
                xray::perf::GpuIdleGapSubmit(set.frame, static_cast<float>(gap_ms));
#if XR_PERF_METRICS
            if (xray::perf::g_perf_metrics_active)
            {
                AddGpuMs(xray::perf::Metric::rgpu_frame, frame_ms);
                if (gap_ok)
                    AddGpuMs(xray::perf::Metric::rgpu_gap, gap_ms);
                const auto span = [&](GpuMark from, GpuMark to, xray::perf::Metric m)
                {
                    const u32 a = static_cast<u32>(from), b = static_cast<u32>(to);
                    if ((set.marks >> a & 1u) && (set.marks >> b & 1u) && t[2 + b] >= t[2 + a])
                        AddGpuMs(m, static_cast<double>(t[2 + b] - t[2 + a]) * kGlTimestampToMs);
                };
                span(GpuMark::MainBegin, GpuMark::Lights, xray::perf::Metric::rgpu_gbuffer);
                span(GpuMark::Lights, GpuMark::Combine, xray::perf::Metric::rgpu_lights);
                span(GpuMark::Combine, GpuMark::CombineEnd, xray::perf::Metric::rgpu_combine);
                span(GpuMark::SvpBegin, GpuMark::SvpEnd, xray::perf::Metric::rgpu_svp);
                xray::perf::UpdateMax(xray::perf::Metric::rgpu_latency, static_cast<s64>(Device.dwFrame - set.frame));
            }
#endif
            m_gpuPrevEnd = t[1];
            m_gpuPrevFrame = set.frame;
        }
        else
        {
            if (!set.discard)
                PERF_INC(rgpu_disjoint);
            m_gpuPrevFrame = u32(-1);
        }
        m_gpuSetOldest = (m_gpuSetOldest + 1) % GPU_TIMER_SETS;
        --m_gpuSetsUsed;
    }
}

void CHW::BeginScene()
{
    GpuTimerBegin();
}
void CHW::EndScene() { }

void CHW::Present()
{
#if 0 // kept for historical reasons
    RImplementation.Target->phase_flip();
#else
    glBindFramebuffer(GL_READ_FRAMEBUFFER, pFB);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(
        0, 0, Device.dwWidth, Device.dwHeight,
        0, 0, Device.dwWidth, Device.dwHeight,
        GL_COLOR_BUFFER_BIT, GL_NEAREST);
#endif
    // 50o: the frame's GPU work (the blit above included) ends here; then the CPU time of the swap - blocks when the
    // driver's frame queue is full (GPU-bound) or on vsync (some drivers block in the first GL call after it instead)
    GpuTimerEnd();
    const auto present_t0 = std::chrono::steady_clock::now();
    // Убрал специально этоусловие чтобы мы не "воровали" кадры для второго вьюпорта, что сильно снижает ФПС!
    //  --#SM+#-- +SecondVP+ Не выводим кадр из второго рендера на экран
    // if (!Device.m_SecondViewport.IsSVPFrame() && !Device.m_SecondViewport.isCamReady)
    {
        PERF_SCOPE(rgpu_present_wait);
        SDL_GL_SwapWindow(m_window);
    }
    const float present_ms =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - present_t0).count();
    xray::perf::PresentWaitSubmit(Device.dwFrame, present_ms);
    xray::perf::GpuSyncWaitSubmit(Device.dwFrame, m_gpuSyncWaitMs);
    m_gpuSyncWaitMs = 0.f;
    GpuTimerPoll();
    // 50z5-extra-diagnostics.md: the VRAM of the [frame] pace line, every 32nd frame - NVIDIA only (GL_NVX_gpu_memory_info:
    // the dedicated memory, what is available now, the evictions so far; the other drivers have no live query)
    if ((Device.dwFrame & 31) == 0 && GLAD_GL_NVX_gpu_memory_info)
    {
        GLint dedicated_kb = 0, available_kb = 0, evictions = 0;
        glGetIntegerv(GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &dedicated_kb);
        glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &available_kb);
        glGetIntegerv(GL_GPU_MEMORY_INFO_EVICTION_COUNT_NVX, &evictions);
        if (dedicated_kb > 0)
        {
            const u64 dedicated = u64(dedicated_kb) * 1024;
            const u64 available = u64(std::max(available_kb, 0)) * 1024;
            xray::perf::VramSubmit(dedicated > available ? dedicated - available : 0, dedicated, 0, u32(std::max(evictions, 0)));
        }
    }
    CurrentBackBuffer = (CurrentBackBuffer + 1) % BackBufferCount;
}

DeviceState CHW::GetDeviceState() const
{
    //  TODO: OGL: Implement GetDeviceState
    return DeviceState::Normal;
}

std::pair<u32, u32> CHW::GetSurfaceSize()
{
    return
    {
        psDeviceMode.Width,
        psDeviceMode.Height
    };
}

bool CHW::ThisInstanceIsGlobal() const
{
    return this == &HW;
}

void CHW::BeginPixEvent(pcstr name) const
{
    if (glPushDebugGroup)
        glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, name);
}

void CHW::EndPixEvent() const
{
    if (glPushDebugGroup)
        glPopDebugGroup();
}
} // namespace xray::render::RENDER_NAMESPACE
