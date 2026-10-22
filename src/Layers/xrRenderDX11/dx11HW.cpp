#include "stdafx.h"

#include "dx11HW.h"

#include "StateManager/dx11SamplerStateCache.h"
#include "dx11TextureUtils.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "xrCore/Profiling/SysDiag.hpp"
#include "xrCore/Platform/HwProfile.h"

#include <SDL_syswm.h>

#include <chrono>

#if defined(_WIN32)
#include <process.h>
#endif

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
// 50z5-extra-diagnostics.md: "shcc" of the frame report - every real HLSL compile (a shader cache miss) goes through
// CHW::D3DCompile; CreateDevice puts this wrapper there, it times the call to the function chosen before
CHW::D3DCompileFunc s_d3d_compile_real = nullptr;
HRESULT WINAPI TimedD3DCompile(LPCVOID src, SIZE_T src_size, LPCSTR source_name, const D3D_SHADER_MACRO* defines,
    ID3DInclude* include, LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2, ID3DBlob** code, ID3DBlob** errors)
{
    GW_FRAME_EVENT(ShaderCompile);
    return s_d3d_compile_real(src, src_size, source_name, defines, include, entry, target, flags1, flags2, code, errors);
}

#if defined(_WIN32)
struct SwapChainThreadData
{
    CHW* self{};
    HWND hwnd{};
    std::pair<bool, bool> out{};
};
#endif

// Hardware profile, stage C (plans/optimization_mt/50-smart-engine-hw-profile.md): what the device and DXGI say about
// the GPU, for the log, the tiers and the policy. Log only - the adapter selection and the device flags do not change.
void SubmitHwProfileGpu(CHW& hw, const DXGI_ADAPTER_DESC1& desc, bool probe)
{
    constexpr u32 kPciVendorMicrosoft = 0x1414; // Microsoft Basic Render Driver (WARP)
    xray::hw::Gpu gpu{};
    gpu.size = sizeof(gpu);
    gpu.api = xray::hw::GpuApi::D3D11;
    gpu.vendor = desc.VendorId;
    gpu.device = desc.DeviceId;
    if (!WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, gpu.name, int(sizeof(gpu.name)), nullptr, nullptr))
        gpu.name[0] = 0;
    gpu.software = ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || desc.VendorId == kPciVendorMicrosoft) ? 1 : 0;
    gpu.dedicated = desc.DedicatedVideoMemory;
    gpu.shared = desc.SharedSystemMemory;
    gpu.feature_level = u16(hw.FeatureLevel);

    // DriverCommandLists = 0: the runtime emulates the deferred contexts (R9); DriverConcurrentCreates = 0: the
    // resource creation is serialized (R3)
    D3D11_FEATURE_DATA_THREADING threading{};
    if (SUCCEEDED(hw.pDevice->CheckFeatureSupport(D3D11_FEATURE_THREADING, &threading, sizeof(threading))))
    {
        gpu.driver_cl = threading.DriverCommandLists ? 1 : 0;
        gpu.driver_cc = threading.DriverConcurrentCreates ? 1 : 0;
    }

    // Integrated GPU: VRAM is the same RAM. OPTIONS2 needs the Windows 10 runtime; fallback - dedicated <= 512 MB.
    bool uma_known = false;
#ifdef HAS_DX11_3
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 options2{};
    if (SUCCEEDED(hw.pDevice->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &options2, sizeof(options2))))
    {
        gpu.uma = options2.UnifiedMemoryArchitecture ? 1 : 0;
        uma_known = true;
    }
#endif
    if (!uma_known)
        gpu.uma = desc.DedicatedVideoMemory <= (u64(512) << 20) ? 1 : 0;

    u64 local_usage = 0, local_budget = 0, nonlocal_usage = 0, nonlocal_budget = 0;
    if (hw.QueryVideoMemory(local_usage, local_budget, nonlocal_usage, nonlocal_budget))
        gpu.budget_local_start = local_budget;

    // Laptops with two GPUs: is there a hardware adapter much stronger than the selected one (EnumAdapters1(0))?
    // Log only (R23); the selection is a separate step.
    if (hw.m_pFactory)
    {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; i < 16 && SUCCEEDED(hw.m_pFactory->EnumAdapters1(i, &adapter)) && adapter; ++i)
        {
            DXGI_ADAPTER_DESC1 other{};
            if (SUCCEEDED(adapter->GetDesc1(&other)) && !(other.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                other.VendorId != kPciVendorMicrosoft)
            {
                gpu.adapters = u8(std::min<u32>(gpu.adapters + 1u, 255u));
                const bool same = other.AdapterLuid.LowPart == desc.AdapterLuid.LowPart &&
                    other.AdapterLuid.HighPart == desc.AdapterLuid.HighPart;
                if (!same && other.DedicatedVideoMemory >= 2 * desc.DedicatedVideoMemory &&
                    other.DedicatedVideoMemory >= desc.DedicatedVideoMemory + (u64(1) << 30))
                {
                    gpu.better_adapter = 1;
                    if (!probe)
                    {
                        Msg("! [hw] adapter %u '%S' (%u MB) looks stronger than the selected '%S' (%u MB) - "
                            "log only, the selection is unchanged",
                            i, other.Description, u32(other.DedicatedVideoMemory >> 20), desc.Description,
                            u32(desc.DedicatedVideoMemory >> 20));
                    }
                }
            }
            adapter->Release();
            adapter = nullptr;
        }
    }

    gpu.width = psDeviceMode.Width;
    gpu.height = psDeviceMode.Height;
    gpu.refresh = psDeviceMode.RefreshRate;
    xray::hw::SubmitGpu(gpu, probe);
}
} // namespace

#if defined(_WIN32)
unsigned __stdcall CHW::SwapChainThreadProc(void* param)
{
    auto* d = static_cast<SwapChainThreadData*>(param);
    d->out = d->self->CreatePrimarySwapChains(d->hwnd);
    return 0;
}
#endif
CHW HW;

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
    if (m_pSwapChain && !m_ChainDesc.Windowed)
    {
        ShowWindow(m_ChainDesc.OutputWindow, SW_RESTORE);
        m_pSwapChain->SetFullscreenState(ThisInstanceIsGlobal() ? psDeviceMode.WindowStyle == rsFullscreen : false, NULL);
    }
}

void CHW::OnAppDeactivate()
{
    if (m_pSwapChain && !m_ChainDesc.Windowed)
    {
        m_pSwapChain->SetFullscreenState(FALSE, NULL);
        if (psDeviceMode.WindowStyle == rsFullscreen || psDeviceMode.WindowStyle == rsFullscreenBorderless)
            ShowWindow(m_ChainDesc.OutputWindow, SW_MINIMIZE);
    }
}

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
void CHW::CreateD3D()
{
    ZoneScoped;

    hDXGI = XRay::LoadModule("dxgi");
    hD3D = XRay::LoadModule("d3d11");
    if (!hD3D->IsLoaded() || !hDXGI->IsLoaded())
    {
        Valid = false;
        return;
    }

    // Минимально поддерживаемая версия Windows => Windows Vista SP2 или Windows 7.
    const auto createDXGIFactory = static_cast<decltype(&CreateDXGIFactory1)>(hDXGI->GetProcAddress("CreateDXGIFactory1"));
    if (createDXGIFactory)
        createDXGIFactory(__uuidof(IDXGIFactory1), (void**)(&m_pFactory));

    if (m_pFactory)
        m_pFactory->EnumAdapters1(0, &m_pAdapter);

    Valid = m_pAdapter;
}

void CHW::DestroyD3D()
{
    _SHOW_REF("refCount:m_pAdapter", m_pAdapter);
    _RELEASE(m_pAdapter);

    _SHOW_REF("refCount:m_pFactory", m_pFactory);
    _RELEASE(m_pFactory);

    // Close handles obtained via XRay::LoadModule.
    hD3D->Close();
    hDXGI->Close();

    // Do not force-unload dxgi/d3d11 with FreeLibrary on shutdown.
    // DXGI may still have internal watchdog/background threads
    // (FullscreenWatchdogThreadWorker), and explicit unload here can race
    // with their teardown causing access violations inside dxgi.dll.
}

void CHW::CreateDevice(SDL_Window* sdlWnd)
{
    ZoneScoped;

    CreateD3D();
    if (!Valid)
        return;

    m_DriverType = Caps.bForceGPU_REF ? D3D_DRIVER_TYPE_REFERENCE : D3D_DRIVER_TYPE_HARDWARE;

    // Display the name of video board
    DXGI_ADAPTER_DESC1 Desc{};
    if (FAILED(m_pAdapter->GetDesc1(&Desc)))
        Msg("! [%s] failed to retrieve adapter description", __FUNCTION__);
    //  Warning: Desc.Description is wide string
    Msg("* GPU [vendor:%X]-[device:%X]: %S", Desc.VendorId, Desc.DeviceId, Desc.Description);

    Caps.id_vendor = Desc.VendorId;
    Caps.id_device = Desc.DeviceId;

    u32 createDeviceFlags = 0;

#ifdef DEBUG
    if (xrDebug::DebuggerIsPresent())
        createDeviceFlags |= D3D_CREATE_DEVICE_DEBUG;
#endif

    // The runtime/driver internal threading is allowed by default: it takes the driver work of every draw and Map
    // off the main thread (2026-10-14 A/B in a heavy fight: dynamic draw 1.87 -> 1.26 us, static 0.72 -> 0.44 us,
    // -3..5 ms of submit per frame; plans/optimization_mt/19-part0-submit.md).
    // -d3d11_prevent_internal_threads brings back PREVENT_INTERNAL_THREADING_OPTIMIZATIONS, which was forced since
    // 2026-04 against 0xC00000FD on an NVIDIA driver worker thread (nvwgf2umx.dll). The real cause (minidump,
    // plans/optimization_mt/50z6-nvidia-driver-thread-stack-overflow.md): the driver creates that thread with a
    // 128 KB stack reserve, and sentry.dll's DllMain(DLL_THREAD_ATTACH) set a 64 KB SetThreadStackGuarantee on every
    // thread of the process, so the driver overflowed at ~56 KB. Fixed in sentry-native (src/sentry_os.c, patch
    // plans/optimization_mt/patches/sentry_thread_stack_guarantee.patch): the guarantee is set only on threads with a
    // stack reserve >= 1 MB. The key stays as an emergency switch.
    // R10 (plan 50): on <= 4 available logical CPUs (stage A, -hw_cpu / -hw_limit included) the hardware policy forbids
    // it - the driver thread competes with main there. The keys win over the policy: -d3d11_prevent_internal_threads
    // forbids it anywhere, -d3d11_allow_internal_threads allows it on a small CPU too (A/B).
    constexpr u32 kPciVendorNvidia = 0x10DE;
    const xray::hw::Policy& hw_policy = xray::hw::GetPolicy();
    if (strstr(Core.Params, "-d3d11_prevent_internal_threads"))
    {
        createDeviceFlags |= D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS;
        if (Caps.id_vendor == kPciVendorNvidia)
            Msg("* D3D11: PREVENT_INTERNAL_THREADING_OPTIMIZATIONS (NVIDIA, -d3d11_prevent_internal_threads)");
        else
            Msg("* D3D11: PREVENT_INTERNAL_THREADING_OPTIMIZATIONS (-d3d11_prevent_internal_threads)");
    }
    else if (strstr(Core.Params, "-d3d11_allow_internal_threads"))
        Msg("* D3D11: driver internal threading allowed (-d3d11_allow_internal_threads)");
    else if (xray::hw::PolicyValid(hw_policy) && hw_policy.prevent_internal_threads)
    {
        createDeviceFlags |= D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS;
        Msg("* D3D11: PREVENT_INTERNAL_THREADING_OPTIMIZATIONS (hw policy: %u CPUs available; "
            "-d3d11_allow_internal_threads to allow it)", xray::hw::GetProfile().cpu.logical_avail);
    }
    else
        Msg("* D3D11: driver internal threading allowed (-d3d11_prevent_internal_threads to forbid it)");

    HRESULT R;

    D3D_FEATURE_LEVEL featureLevels[] =
    {
#ifdef HAS_DX11_3
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
#endif
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    D3D_FEATURE_LEVEL featureLevels2[] =
    {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    D3D_FEATURE_LEVEL featureLevels3[] =
    {
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    auto& pContext = d3d_contexts_pool[CHW::IMM_CTX_ID];

    const auto createDevice = [&](const D3D_FEATURE_LEVEL* level, const u32 levels)
    {
        ZoneScopedN("CreateDevice");

        // Re-resolve on every device creation, never cache in a 'static': the startup HW
        // probe resolves this pointer BEFORE renderdoc.dll is loaded, and a cached raw
        // address would permanently bypass RenderDoc's export hook for the main device -
        // in-app captures would silently never trigger.
        const auto d3d11CreateDevice = static_cast<PFN_D3D11_CREATE_DEVICE>(hD3D->GetProcAddress("D3D11CreateDevice"));
        return d3d11CreateDevice(m_pAdapter, D3D_DRIVER_TYPE_UNKNOWN,
            nullptr, createDeviceFlags, level, levels,
            D3D11_SDK_VERSION, &pDevice, &FeatureLevel, &pContext);
    };

    if (DX10Only)
        R = createDevice(featureLevels3, std::size(featureLevels3));
    else
    {
        R = createDevice(featureLevels, std::size(featureLevels));
        if (FAILED(R))
            R = createDevice(featureLevels2, std::size(featureLevels2));
    }

    if (SUCCEEDED(R))
    {
        pContext->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&pContext1));
#ifdef HAS_DX11_3
        pDevice->QueryInterface(__uuidof(ID3D11Device3), reinterpret_cast<void**>(&pDevice3));
#endif
        if (FeatureLevel >= D3D_FEATURE_LEVEL_11_0)
        {
            D3DCompile = &::D3DCompile;
            ComputeShadersSupported = true;
        }
        else
        {
            if (ClearSkyMode)
            {
                hD3DCompiler = XRay::LoadModule("d3dcompiler_37");
                D3DCompile = static_cast<D3DCompileFunc>(hD3DCompiler->GetProcAddress("D3DCompileFromMemory"));
            }
            else
            {
                D3DCompile = &::D3DCompile;
            }

            D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS data;
            pDevice->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS,
                &data, sizeof(data));
            ComputeShadersSupported = data.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x;
        }
        // 50z5-extra-diagnostics.md: the real compiles in the frame report ("shcc"), see TimedD3DCompile
        if (D3DCompile && D3DCompile != &TimedD3DCompile)
        {
            s_d3d_compile_real = D3DCompile;
            D3DCompile = &TimedD3DCompile;
        }
        D3D11_FEATURE_DATA_D3D11_OPTIONS options;
        pDevice->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options));

        D3D11_FEATURE_DATA_DOUBLES doubles;
        pDevice->CheckFeatureSupport(D3D11_FEATURE_DOUBLES, &doubles, sizeof(doubles));

        DoublePrecisionFloatShaderOps = doubles.DoublePrecisionFloatShaderOps;
        SAD4ShaderInstructions = options.SAD4ShaderInstructions;
        ExtendedDoublesShaderInstructions = options.ExtendedDoublesShaderInstructions;
    }

    if (FAILED(R))
    {
        Valid = false;
        if (!ThisInstanceIsGlobal())
        {
            // Startup HW probe instance: log why the probe failed. A silent failure here
            // unregisters renderer_r4 entirely and the engine falls back to GL (e.g. when
            // launched under the RenderDoc injector).
            Msg("! D3D11CreateDevice failed in the HW probe: 0x%08x", static_cast<unsigned>(R));
            return;
        }
        // Fatal error! Cannot create rendering device AT STARTUP !!!
        Msg("Failed to initialize graphics hardware.\n"
            "Please try to restart the game.\n"
            "CreateDevice returned 0x%08x", R);
        xrDebug::DoExit("Failed to initialize graphics hardware.\nPlease try to restart the game.");
    }

    _SHOW_REF("* CREATE: DeviceREF:", pDevice);

    // Register on the device thread: worker threads often use a small stack; Tracy D3D11 setup touches
    // the device and can contribute to driver/DXGI re-entrancy issues alongside EscapeCB overflows.
    if (ThisInstanceIsGlobal())
    {
        ZoneScopedN("TracyD3D11Context");
        profiler_ctx = TracyD3D11Context(pDevice, get_context(CHW::IMM_CTX_ID));
    }

    // 50o: the GPU frame timer (only the global HW: the startup probe renders no frames)
    if (ThisInstanceIsGlobal())
        GpuTimerCreate();

    // Create deferred contexts
    if (ThisInstanceIsGlobal())
    {
        ZoneScopedN("Create deferred contexts");
        for (int id = 0; id < R__NUM_PARALLEL_CONTEXTS; ++id)
        {
            R = pDevice->CreateDeferredContext(0, &d3d_contexts_pool[id]);
            VERIFY(SUCCEEDED(R));
        }
    }

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);

    if (!SDL_GetWindowWMInfo(sdlWnd, &info))
    {
        Msg("! Failed to retrieve SDL window handle: %s", SDL_GetError());
        Valid = false;
        return;
    }

    const HWND hwnd = info.info.win.window;

    bool swap_chain_ok = false;
    bool used_factory_create_swap_chain = false;

    // EscapeCB runs with driver + DXGI frames on top of *our* calling thread's stack. Main exe may use /STACK:32M,
    // but CreateDevice is still deep (SDL, caps, Tracy, deferred contexts). Run swap chain creation on a fresh
    // thread with a large reserved stack so the entry frame is shallow (mitigates 0xC00000FD in nvwgf2umx).
    //
    // Exclusive DXGI fullscreen (rsFullscreen, Windowed=FALSE swap chain) must not be initialized from a worker
    // while the main thread blocks in WaitForSingleObject: drivers often post/callback to the HWND thread and
    // deadlock. Borderless uses a windowed swap chain and stays on the auxiliary path. Override:
    // -dxgi_swapchain_big_stack_fullscreen (not recommended unless debugging stack overflow in fullscreen).
    constexpr u32 kPciVendorNvidiaSwap = 0x10DE;
    const bool nvidia_or_big_stack_flag =
        (Caps.id_vendor == kPciVendorNvidiaSwap) || strstr(Core.Params, "-dxgi_swapchain_big_stack");
    const bool exclusive_dxgi_fullscreen = ThisInstanceIsGlobal() && psDeviceMode.WindowStyle == rsFullscreen;
    const bool force_aux_thread_in_fullscreen = strstr(Core.Params, "-dxgi_swapchain_big_stack_fullscreen");
    const bool use_swapchain_big_stack_thread =
#if defined(_WIN32)
        nvidia_or_big_stack_flag && (!exclusive_dxgi_fullscreen || force_aux_thread_in_fullscreen);
#else
        false;
#endif

#if defined(_WIN32)
    if (use_swapchain_big_stack_thread)
    {
        SwapChainThreadData data{};
        data.self = this;
        data.hwnd = hwnd;
        constexpr unsigned kSwapChainThreadStack = 16u * 1024u * 1024u;
        const uintptr_t th = _beginthreadex(nullptr, kSwapChainThreadStack, &CHW::SwapChainThreadProc, &data, 0, nullptr);
        if (th)
        {
            WaitForSingleObject(reinterpret_cast<HANDLE>(th), INFINITE);
            CloseHandle(reinterpret_cast<HANDLE>(th));
            swap_chain_ok = data.out.first;
            used_factory_create_swap_chain = data.out.second;
            Msg("* DXGI: swap chain init on auxiliary thread (%u MiB stack; NVIDIA or -dxgi_swapchain_big_stack)",
                kSwapChainThreadStack / (1024u * 1024u));
        }
        else
        {
            Msg("! DXGI: _beginthreadex failed for swap chain init, using inline path");
            const auto p = CreatePrimarySwapChains(hwnd);
            swap_chain_ok = p.first;
            used_factory_create_swap_chain = p.second;
        }
    }
    else
#endif
    {
#if defined(_WIN32)
        if (exclusive_dxgi_fullscreen && !force_aux_thread_in_fullscreen && nvidia_or_big_stack_flag)
            Msg("* DXGI: swap chain on main thread (exclusive fullscreen avoids off-thread deadlock)");
#endif
        const auto p = CreatePrimarySwapChains(hwnd);
        swap_chain_ok = p.first;
        used_factory_create_swap_chain = p.second;
    }

    if (!swap_chain_ok)
        Valid = false;
    else if (used_factory_create_swap_chain)
        Msg("* DXGI: IDXGIFactory::CreateSwapChain (-dxgi_modern_swapchain opts in to CreateSwapChainForHwnd first)");

    // Select depth-stencil format
    constexpr DXGI_FORMAT formats[] =
    {
        //DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
        DXGI_FORMAT_D24_UNORM_S8_UINT,
    };
    const DXGI_FORMAT selectedFormat = SelectFormat(D3D_FORMAT_SUPPORT_DEPTH_STENCIL, formats);
    if (selectedFormat == DXGI_FORMAT_UNKNOWN)
    {
        Valid = false;
        if (!ThisInstanceIsGlobal())
            return;
        Log("Failed to initialize graphics hardware: "
            "failed to select depth-stencil format.\n"
            "Please try to restart the game.");
        xrDebug::DoExit("Failed to initialize graphics hardware.\nPlease try to restart the game.");
    }
    Caps.fDepth = dx11TextureUtils::ConvertTextureFormat(selectedFormat);

    const auto memory = Desc.DedicatedVideoMemory;
    Msg("*   Texture memory: %d M", memory / (1024 * 1024));

    SubmitHwProfileGpu(*this, Desc, !ThisInstanceIsGlobal());

    // R9: the policy is final now (DriverCommandLists of the real device); the render reads it every frame
    // (render_graph_main_parallel, BeginSecondVPCalculateParallel). r__main_parallel_parts comes from user.ltx already.
    if (ThisInstanceIsGlobal())
    {
        const int user_parts = std::clamp(ps_r__main_parallel_parts, 2, 12);
        const u32 parts = xray::hw::EffectiveMainParallelParts(u32(user_parts));
        const xray::hw::Policy& policy = xray::hw::GetPolicy();
        Msg("* [hw] render: main parts %u%s (user %d, policy %u), svp worker %s", parts,
            parts < 2 ? " - sequential" : "", user_parts, policy.main_parallel_parts,
            xray::hw::SvpWorkerAllowed() ? "on" : "off");
    }
}

std::pair<bool, bool> CHW::CreatePrimarySwapChains(HWND hwnd)
{
    // CreateSwapChainForHwnd (DXGI 1.2+) drives D3DKMT_CheckMultiplaneOverlaySupport3 → EscapeCB → UMD; with a
    // deep application stack that overflows (0xC00000FD). Prefer legacy IDXGIFactory::CreateSwapChain first
    // unless the user opts into DXGI 1.2 ordering. NVIDIA: skip ForHwnd as fallback (same EscapeCB path);
    // use -dxgi_modern_swapchain if your system only works with ForHwnd.
    constexpr u32 kPciVendorNvidia = 0x10DE;
    const bool prefer_modern_dxgi = strstr(Core.Params, "-dxgi_modern_swapchain");
    const bool block_dxgi_1_2 = strstr(Core.Params, "-no_dx11_2") || strstr(Core.Params, "-dxgi_legacy_swapchain");
    const bool allow_dxgi_1_2_fallback =
        !block_dxgi_1_2 && (prefer_modern_dxgi || Caps.id_vendor != kPciVendorNvidia);

    bool swap_chain_ok = false;
    bool used_factory_create_swap_chain = false;
    if (prefer_modern_dxgi && !block_dxgi_1_2)
    {
        if (CreateSwapChain2(hwnd))
            swap_chain_ok = true;
        else if (CreateSwapChain(hwnd))
        {
            swap_chain_ok = true;
            used_factory_create_swap_chain = true;
        }
    }
    else
    {
        if (CreateSwapChain(hwnd))
        {
            swap_chain_ok = true;
            used_factory_create_swap_chain = true;
        }
        else if (allow_dxgi_1_2_fallback && CreateSwapChain2(hwnd))
            swap_chain_ok = true;
    }
    return {swap_chain_ok, used_factory_create_swap_chain};
}

bool CHW::CreateSwapChain(HWND hwnd)
{
    ZoneScoped;

    // Set up the presentation parameters
    DXGI_SWAP_CHAIN_DESC& sd = m_ChainDesc;
    ZeroMemory(&sd, sizeof(sd));

    // Back buffer
    sd.BufferDesc.Width = Device.dwWidth;
    sd.BufferDesc.Height = Device.dwHeight;

    //  TODO: DX11: implement dynamic format selection
    constexpr DXGI_FORMAT formats[] =
    {
        //DXGI_FORMAT_R16G16B16A16_FLOAT, // Do we even need this?
        //DXGI_FORMAT_R10G10B10A2_UNORM, // D3DX11SaveTextureToMemory fails on this format
        DXGI_FORMAT_R8G8B8A8_UNORM,
    };

    // Select back-buffer format
    sd.BufferDesc.Format = SelectFormat(D3D_FORMAT_SUPPORT_DISPLAY, formats);
    Caps.fTarget = dx11TextureUtils::ConvertTextureFormat(sd.BufferDesc.Format);

    // Buffering
    BackBufferCount = 1;
    sd.BufferCount = BackBufferCount;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

    // Multisample
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;

    // Windoze
    /* XXX:
       Probably the reason of weird tearing
       glitches reported by Shoker in windowed
       mode with VSync enabled.
       XXX: Fix this windoze stuff!!!
    */
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    sd.OutputWindow = hwnd;

    sd.Windowed = ThisInstanceIsGlobal() ? psDeviceMode.WindowStyle != rsFullscreen : true;

    //  Additional set up
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    const auto hr = m_pFactory->CreateSwapChain(pDevice, &sd, &m_pSwapChain);
    return SUCCEEDED(hr);
}

bool CHW::CreateSwapChain2(HWND hwnd)
{
    if (strstr(Core.Params, "-no_dx11_2"))
        return false;

    ZoneScoped;

#ifdef HAS_DX11_2
    IDXGIFactory2* pFactory2{};
    m_pAdapter->GetParent(__uuidof(IDXGIFactory2), (void**)&pFactory2);
    if (!pFactory2)
        return false;

    // Set up the presentation parameters
    DXGI_SWAP_CHAIN_DESC1 desc{};

    // Back buffer
    desc.Width = Device.dwWidth;
    desc.Height = Device.dwHeight;

    constexpr DXGI_FORMAT formats[] =
    {
        //DXGI_FORMAT_R16G16B16A16_FLOAT,
        //DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM,
    };

    // Select back-buffer format
    desc.Format = SelectFormat(D3D11_FORMAT_SUPPORT_DISPLAY, formats);
    Caps.fTarget = dx11TextureUtils::ConvertTextureFormat(desc.Format);

    // Buffering
    BackBufferCount = 1; // For DXGI_SWAP_EFFECT_FLIP_DISCARD we need at least two
    desc.BufferCount = BackBufferCount;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

    // Multisample
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;

    // Windoze
    //desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; // XXX: tearing glitches with flip presentation model
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fulldesc{};
    fulldesc.Windowed = ThisInstanceIsGlobal() ? psDeviceMode.WindowStyle != rsFullscreen : true;

    // Additional setup
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    IDXGISwapChain1* swapchain{};
    const HRESULT result = pFactory2->CreateSwapChainForHwnd(pDevice, hwnd, &desc,
        fulldesc.Windowed ? nullptr : &fulldesc, nullptr, &swapchain);
    _RELEASE(pFactory2);

    if (FAILED(result))
        return false;

    if (FAILED(swapchain->GetDesc(&m_ChainDesc)))
    {
        _RELEASE(swapchain);
        return false;
    }
    m_pSwapChain = swapchain;

    m_pSwapChain->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void**>(&m_pSwapChain2));

    if (m_pSwapChain2 && ThisInstanceIsGlobal())
        Device.PresentationFinished = m_pSwapChain2->GetFrameLatencyWaitableObject();

    return true;
#else // #ifdef HAS_DX11_2
    UNUSED(hwnd);
#endif

    return false;
}

bool CHW::ThisInstanceIsGlobal() const
{
    return this == &HW;
}

void CHW::DestroyDevice()
{
    const bool is_quit_requested = SDL_QuitRequested();

    if (ThisInstanceIsGlobal()) // only if we are global HW
    {
        RSManager.ClearStateArray();
        DSSManager.ClearStateArray();
        BSManager.ClearStateArray();
        SSManager.ClearStateArray();
    }

    // Some drivers keep DXGI watchdog/background activity alive during app quit.
    // Releasing swapchain/device at this stage can race with that teardown and
    // crash in dxgi (FullscreenWatchdogThreadWorker). On process exit it's safe
    // to skip deep COM cleanup and let OS reclaim resources.
    if (is_quit_requested)
    {
        Msg("* DX11: skipping deep DXGI teardown on quit");
        return;
    }

    //  Must switch to windowed mode to release swap chain
    if (!m_ChainDesc.Windowed && m_pSwapChain)
        m_pSwapChain->SetFullscreenState(FALSE, NULL);
#ifdef HAS_DX11_2
    _RELEASE(m_pSwapChain2);
#endif
    _SHOW_REF("refCount:m_pSwapChain", m_pSwapChain);
    _RELEASE(m_pSwapChain);

    if (profiler_ctx)
        TracyD3D11Destroy(profiler_ctx);

    GpuTimerDestroy();

    _RELEASE(pContext1);
    for (int id = 0; id < R__NUM_CONTEXTS; ++id)
    {
        _SHOW_REF("refCount:pContext", d3d_contexts_pool[id]);
        _RELEASE(d3d_contexts_pool[id]);
    }

#ifdef HAS_DX11_3
    _RELEASE(pDevice3);
#endif
    _SHOW_REF("refCount:pDevice:", pDevice);
    _RELEASE(pDevice);
    DestroyD3D();
}

//////////////////////////////////////////////////////////////////////
// Resetting device
//////////////////////////////////////////////////////////////////////
void CHW::Reset()
{
    ZoneScoped;
    DXGI_SWAP_CHAIN_DESC& cd = m_ChainDesc;
    const bool bWindowed = ThisInstanceIsGlobal() ? psDeviceMode.WindowStyle != rsFullscreen : true;
    cd.Windowed = bWindowed;
    m_pSwapChain->SetFullscreenState(!bWindowed, NULL);
    DXGI_MODE_DESC& desc = m_ChainDesc.BufferDesc;
    desc.Width = Device.dwWidth;
    desc.Height = Device.dwHeight;

    CHK_DX(m_pSwapChain->ResizeTarget(&desc));
    CHK_DX(m_pSwapChain->ResizeBuffers(
        cd.BufferCount, desc.Width, desc.Height, desc.Format, cd.Flags));
}

void CHW::SetPrimaryAttributes(u32& /*windowFlags*/)
{

}

bool CHW::CheckFormatSupport(const DXGI_FORMAT format, const u32 feature) const
{
    u32 supports;

    if (SUCCEEDED(pDevice->CheckFormatSupport(format, &supports)))
    {
        if (supports & feature)
            return true;
    }

    return false;
}

DXGI_FORMAT CHW::SelectFormat(D3D_FORMAT_SUPPORT feature, const DXGI_FORMAT formats[], size_t count) const
{
    for (size_t i = 0; i < count; ++i)
        if (CheckFormatSupport(formats[i], feature))
            return formats[i];

    return DXGI_FORMAT_UNKNOWN;
}

bool CHW::UsingFlipPresentationModel() const
{
    return m_ChainDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
#ifdef HAS_DXGI1_4
        || m_ChainDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD
#endif
    ;
}

std::pair<u32, u32> CHW::GetSurfaceSize() const
{
    return
    {
        m_ChainDesc.BufferDesc.Width,
        m_ChainDesc.BufferDesc.Height
    };
}

void CHW::BeginScene()
{
    // The thread that runs the frame owns the immediate context for this frame.
    m_immOwnerThread = u32(GetCurrentThreadId());
    GpuTimerBegin();
}
void CHW::EndScene() { }

pcstr CHW::DeviceRemovedReasonName(HRESULT reason)
{
    switch (reason)
    {
    case DXGI_ERROR_DEVICE_HUNG: return "DEVICE_HUNG (GPU fault/invalid command - corrupted command stream or bad resource)";
    case DXGI_ERROR_DEVICE_RESET: return "DEVICE_RESET (TDR timeout - a command ran too long)";
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DRIVER_INTERNAL_ERROR";
    case DXGI_ERROR_INVALID_CALL: return "INVALID_CALL (API misuse)";
    case DXGI_ERROR_DEVICE_REMOVED: return "DEVICE_REMOVED (generic)";
    case S_OK: return "S_OK (device not removed)";
    default: return "UNKNOWN";
    }
}

bool CHW::QueryVideoMemory(u64& local_usage, u64& local_budget, u64& nonlocal_usage, u64& nonlocal_budget)
{
    local_usage = local_budget = nonlocal_usage = nonlocal_budget = 0;
#ifdef HAS_DXGI1_4
    if (!m_pAdapter)
        return false;
    IDXGIAdapter3* adapter3 = nullptr;
    if (FAILED(m_pAdapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3))) || !adapter3)
        return false;
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
    const HRESULT hr_local = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
    const HRESULT hr_nonlocal = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal);
    adapter3->Release();
    if (FAILED(hr_local))
        return false;
    local_usage = local.CurrentUsage;
    local_budget = local.Budget;
    if (SUCCEEDED(hr_nonlocal))
    {
        nonlocal_usage = nonlocal.CurrentUsage;
        nonlocal_budget = nonlocal.Budget;
    }
    return true;
#else
    return false;
#endif
}

void CHW::CheckImmThread(const char* where)
{
    const u32 owner = m_immOwnerThread;
    if (!owner)
        return;
    const u32 tid = u32(GetCurrentThreadId());
    if (tid == owner)
        return;
    const u32 n = m_immForeignCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    xray::perf::SetGauge(xray::perf::Metric::hw_imm_foreign, static_cast<s64>(n));
    // First 32 occurrences verbatim, then every 1000th: enough to see the pattern, no flood.
    if (n <= 32 || (n % 1000) == 0)
    {
        Msg("! [imm-race] %s on the IMMEDIATE context from thread %u (owner %u, svp_worker=%d) #%u",
            where, tid, owner, svp_worker_is_current_thread() ? 1 : 0, n);
    }
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
} // namespace

void CHW::GpuTimerCreate()
{
    GpuTimerDestroy();

    // The pass marks only where something reads them (XR_PERF_METRICS); 2 timestamps per frame otherwise
    constexpr u32 ts_count = XR_PERF_METRICS ? GPU_TIMER_TS : 2;
    const D3D11_QUERY_DESC disjoint_desc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
    const D3D11_QUERY_DESC ts_desc{ D3D11_QUERY_TIMESTAMP, 0 };
    HRESULT hr = S_OK;
    for (GpuTimerSet& set : m_gpuSets)
    {
        hr = pDevice->CreateQuery(&disjoint_desc, &set.disjoint);
        for (u32 i = 0; SUCCEEDED(hr) && i < ts_count; ++i)
            hr = pDevice->CreateQuery(&ts_desc, &set.ts[i]);
        if (FAILED(hr))
            break;
    }
    if (FAILED(hr))
    {
        GpuTimerDestroy();
        Msg("* [gpu] frame timing: unavailable (D3D11 CreateQuery failed: 0x%08x)", static_cast<u32>(hr));
        return;
    }
    m_gpuTimerOk = true;

    // The tick frequency for the log line: one disjoint query, waited for right here, once at device creation (no
    // frame ever waits for a query); an idle GPU answers at once, 100 ms at most.
    ID3D11DeviceContext* ctx = get_context(IMM_CTX_ID);
    ID3D11Query* probe = m_gpuSets[0].disjoint;
    ctx->Begin(probe);
    ctx->End(probe);
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    const auto t0 = std::chrono::steady_clock::now();
    while ((hr = ctx->GetData(probe, &dj, sizeof(dj), 0)) == S_FALSE &&
        std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(100))
    {
        SwitchToThread();
    }
    if (hr == S_OK && dj.Frequency != 0)
        Msg("* [gpu] frame timing: D3D11 timestamp queries (freq %.2f MHz)", static_cast<double>(dj.Frequency) / 1e6);
    else
        Msg("* [gpu] frame timing: D3D11 timestamp queries (freq not known yet: 0x%08x)", static_cast<u32>(hr));
}

void CHW::GpuTimerDestroy()
{
    for (GpuTimerSet& set : m_gpuSets)
    {
        _RELEASE(set.disjoint);
        for (ID3D11Query*& q : set.ts)
            _RELEASE(q);
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
    if (!m_gpuTimerOk)
        return;
    ID3D11DeviceContext* ctx = get_context(IMM_CTX_ID);
    if (m_gpuSetOpen)
    {
        // A BeginScene without the Present of the previous one: close that set, its result is not a frame
        GpuTimerSet& open = m_gpuSets[(m_gpuSetOldest + m_gpuSetsUsed - 1) % GPU_TIMER_SETS];
        ctx->End(open.ts[1]);
        ctx->End(open.disjoint);
        open.discard = true;
        m_gpuSetOpen = false;
    }
    if (m_gpuSetsUsed == GPU_TIMER_SETS)
    {
        // Every set still waits for its result (the GPU is more than GPU_TIMER_SETS frames behind, or minimised):
        // this frame goes unmeasured rather than waiting
        PERF_INC(rgpu_skipped);
        return;
    }
    GpuTimerSet& set = m_gpuSets[(m_gpuSetOldest + m_gpuSetsUsed) % GPU_TIMER_SETS];
    ++m_gpuSetsUsed;
    m_gpuSetOpen = true;
    set.frame = Device.dwFrame;
    set.marks = 0;
    set.discard = false;
    ctx->Begin(set.disjoint);
    ctx->End(set.ts[0]);
}

void CHW::GpuTimerEnd()
{
    if (!m_gpuSetOpen)
        return;
    GpuTimerSet& set = m_gpuSets[(m_gpuSetOldest + m_gpuSetsUsed - 1) % GPU_TIMER_SETS];
    ID3D11DeviceContext* ctx = get_context(IMM_CTX_ID);
    ctx->End(set.ts[1]);
    ctx->End(set.disjoint);
    m_gpuSetOpen = false;
}

void CHW::GpuMarkPass(GpuMark mark)
{
#if XR_PERF_METRICS
    if (!m_gpuSetOpen || !xray::perf::g_perf_metrics_active || mark >= GpuMark::Count)
        return;
    if (u32(GetCurrentThreadId()) != m_immOwnerThread)
        return;
    GpuTimerSet& set = m_gpuSets[(m_gpuSetOldest + m_gpuSetsUsed - 1) % GPU_TIMER_SETS];
    get_context(IMM_CTX_ID)->End(set.ts[2 + static_cast<u32>(mark)]);
    set.marks |= 1u << static_cast<u32>(mark);
#else
    UNUSED(mark);
#endif
}

void CHW::GpuTimerPoll()
{
    ID3D11DeviceContext* ctx = get_context(IMM_CTX_ID);
    // Oldest first; the first set that is not ready ends the poll (the newer ones are not either). DONOTFLUSH: never
    // a flush or a wait here - Present has just flushed.
    while (m_gpuSetsUsed > (m_gpuSetOpen ? 1u : 0u))
    {
        GpuTimerSet& set = m_gpuSets[m_gpuSetOldest];
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        HRESULT hr = ctx->GetData(set.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE)
            return;
        bool ok = SUCCEEDED(hr) && !set.discard && !dj.Disjoint && dj.Frequency != 0;
        u64 t[GPU_TIMER_TS]{};
        if (ok)
        {
            const u32 need = 3u | (set.marks << 2);
            for (u32 i = 0; i < GPU_TIMER_TS; ++i)
            {
                if (!(need & (1u << i)))
                    continue;
                hr = ctx->GetData(set.ts[i], &t[i], sizeof(t[i]), D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (hr == S_FALSE)
                    return; // the disjoint query is ready before its timestamps (rare): the next frame then
                if (FAILED(hr))
                {
                    ok = false;
                    break;
                }
            }
        }
        const double to_ms = ok ? 1000.0 / static_cast<double>(dj.Frequency) : 0.0;
        const double frame_ms = ok && t[1] >= t[0] ? static_cast<double>(t[1] - t[0]) * to_ms : -1.0;
        if (frame_ms >= 0.0 && frame_ms < 60000.0)
        {
            xray::perf::GpuFrameTimeSubmit(set.frame, static_cast<float>(frame_ms));
            // The GPU had nothing to do between the previous frame's end and this one's begin
            const bool gap_ok = m_gpuPrevFrame != u32(-1) && m_gpuPrevFrame + 1 == set.frame &&
                m_gpuPrevFreq == dj.Frequency && t[0] >= m_gpuPrevEnd;
            const double gap_ms = gap_ok ? static_cast<double>(t[0] - m_gpuPrevEnd) * to_ms : 0.0;
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
                        AddGpuMs(m, static_cast<double>(t[2 + b] - t[2 + a]) * to_ms);
                };
                span(GpuMark::MainBegin, GpuMark::Lights, xray::perf::Metric::rgpu_gbuffer);
                span(GpuMark::Lights, GpuMark::Combine, xray::perf::Metric::rgpu_lights);
                span(GpuMark::Combine, GpuMark::CombineEnd, xray::perf::Metric::rgpu_combine);
                span(GpuMark::SvpBegin, GpuMark::SvpEnd, xray::perf::Metric::rgpu_svp);
                xray::perf::UpdateMax(xray::perf::Metric::rgpu_latency, static_cast<s64>(Device.dwFrame - set.frame));
            }
#endif
            m_gpuPrevEnd = t[1];
            m_gpuPrevFreq = dj.Frequency;
            m_gpuPrevFrame = set.frame;
        }
        else
        {
            // Disjoint (clock change, power event), failed or discarded: no value, and no gap for the next frame
            if (!set.discard)
                PERF_INC(rgpu_disjoint);
            m_gpuPrevFrame = u32(-1);
        }
        m_gpuSetOldest = (m_gpuSetOldest + 1) % GPU_TIMER_SETS;
        --m_gpuSetsUsed;
    }
}

void CHW::Present()
{
    // Убрал специально этоусловие чтобы мы не "воровали" кадры для второго вьюпорта, что сильно снижает ФПС!
    // if (!Device.m_SecondViewport.IsSVPFrame() && !Device.m_SecondViewport.isCamReady) //--#SM+#-- +SecondVP+ Не выводим кадр из второго рендера на экран
    // {
        const bool bUseVSync = psDeviceMode.WindowStyle == rsFullscreen &&
            psDeviceFlags.test(rsVSync); // xxx: weird tearing glitches when VSync turned on for windowed mode in DX11

        // 50o: the frame's GPU work ends here (Present's own copy is not in it); then the CPU time Present blocks -
        // the driver's frame queue is full (GPU-bound) or vsync
        GpuTimerEnd();
        const auto present_t0 = std::chrono::steady_clock::now();
        HRESULT present_hr;
        {
            PERF_SCOPE(rgpu_present_wait);
            present_hr = m_pSwapChain->Present(bUseVSync ? 1 : 0, 0);
        }
        const float present_ms =
            std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - present_t0).count();
        switch (present_hr)
        {
        case DXGI_STATUS_OCCLUDED:
        case DXGI_ERROR_DEVICE_REMOVED:
            doPresentTest = true;
            break;
        }
    // }

    xray::perf::PresentWaitSubmit(Device.dwFrame, present_ms);
    xray::perf::GpuSyncWaitSubmit(Device.dwFrame, m_gpuSyncWaitMs);
    m_gpuSyncWaitMs = 0.f;
    GpuTimerPoll();

    // 50z5-extra-diagnostics.md: the VRAM of the [frame] pace line, every 32nd frame (DXGI, ~0.6 us)
    if ((Device.dwFrame & 31) == 0)
    {
        u64 local_usage, local_budget, nonlocal_usage, nonlocal_budget;
        if (QueryVideoMemory(local_usage, local_budget, nonlocal_usage, nonlocal_budget))
            xray::perf::VramSubmit(local_usage, local_budget, nonlocal_usage, 0);
    }

    CurrentBackBuffer = (CurrentBackBuffer + 1) % BackBufferCount;

    TracyD3D11Collect(profiler_ctx);
}

DeviceState CHW::GetDeviceState()
{
    if (doPresentTest)
    {
        switch (m_pSwapChain->Present(0, DXGI_PRESENT_TEST))
        {
        case S_OK:
            doPresentTest = false;
            break;

        case DXGI_STATUS_OCCLUDED:
            // Do not render until we become visible again
            return DeviceState::Lost;

        case DXGI_ERROR_DEVICE_RESET:
            return DeviceState::NeedReset;

        case DXGI_ERROR_DEVICE_REMOVED:
        {
            // A removed device has a REASON - log it before dying. HUNG/RESET mean the GPU
            // executed something that timed out (TDR) or faulted: almost always a corrupted
            // command stream (e.g. two recorders interleaving on one deferred context) or a
            // runaway shader. DRIVER_INTERNAL_ERROR points at the driver. INVALID_CALL means
            // an API misuse the driver rejected. Without this line all four look identical
            // to the user and to crash reports.
            const HRESULT reason = pDevice->GetDeviceRemovedReason();
            Msg("! D3D11 device removed, reason: 0x%08x %s (frame %u)", u32(reason), DeviceRemovedReasonName(reason),
                Device.dwFrame);
            FATAL("Graphics driver was updated or GPU was physically removed from computer.\n"
                  "Please, restart the game.");
            break;
        }
        }
    }

    return DeviceState::Normal;
}
} // namespace xray::render::RENDER_NAMESPACE
