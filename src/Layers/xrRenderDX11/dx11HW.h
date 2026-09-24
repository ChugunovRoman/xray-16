#pragma once

#include <atomic>
#include <utility>

#include "xrCore/ModuleLookup.hpp"

#include "Layers/xrRender/HWCaps.h"
#include "Layers/xrRender/stats_manager.h"

#include <SDL.h>

namespace xray::render::RENDER_NAMESPACE
{
class CHW
    : public pureAppActivate,
      public pureAppDeactivate
{
public:
    CHW();
    ~CHW();

    void CreateD3D();
    void DestroyD3D();

    void CreateDevice(SDL_Window* sdlWnd);
    void DestroyDevice();

    void Reset();

    void SetPrimaryAttributes(u32& windowFlags);

    std::pair<u32, u32> GetSurfaceSize() const;

    bool CheckFormatSupport(DXGI_FORMAT format, u32 feature) const;
    DXGI_FORMAT SelectFormat(D3D_FORMAT_SUPPORT feature, const DXGI_FORMAT formats[], size_t count) const;
    template <size_t count>
    inline DXGI_FORMAT SelectFormat(D3D_FORMAT_SUPPORT feature, const DXGI_FORMAT (&formats)[count]) const
    {
        return SelectFormat(feature, formats, count);
    }
    bool UsingFlipPresentationModel() const;
    DeviceState GetDeviceState();

    // ID3D11DeviceContext is NOT free-threaded: only the thread that runs the frame (stamped in
    // BeginScene) may touch the immediate context. CheckImmThread reports, rate-limited, any call
    // from another thread - undefined behaviour and a prime suspect for corrupted GPU state.
    u32 ImmOwnerThread() const { return m_immOwnerThread; }
    u32 ImmForeignCalls() const { return m_immForeignCalls.load(std::memory_order_relaxed); }
    void CheckImmThread(const char* where);
    // Human-readable name of a GetDeviceRemovedReason() code (HUNG/RESET/INTERNAL/INVALID_CALL).
    static pcstr DeviceRemovedReasonName(HRESULT reason);
    // Diagnostics: what the OS video memory manager says this process holds and may hold. The
    // black-screen investigation found ~11k live textures, most of them per-instance weapon icon
    // render targets; a process running out of its video memory budget is the one explanation
    // that fits every symptom (evicted textures sample black, new icon targets fail to allocate,
    // FinishCommandList dies with a generic DEVICE_REMOVED, vid_restart keeps the leak alive).
    // Returns false when DXGI 1.4 is unavailable. Values in bytes.
    bool QueryVideoMemory(u64& local_usage, u64& local_budget, u64& nonlocal_usage, u64& nonlocal_budget);

public:
    void BeginScene();
    void EndScene();
    void Present();

public:
    void OnAppActivate() override;
    void OnAppDeactivate() override;

private:
    bool CreateSwapChain(HWND hwnd);
    bool CreateSwapChain2(HWND hwnd);
    // { swap_chain_ok, used_IDXGIFactory_CreateSwapChain }
    std::pair<bool, bool> CreatePrimarySwapChains(HWND hwnd);
#if defined(_WIN32)
    static unsigned __stdcall SwapChainThreadProc(void* param);
#endif

    bool ThisInstanceIsGlobal() const;

public:
    ICF ID3DDeviceContext* get_context(u32 context_id)
    {
        VERIFY(context_id < R__NUM_CONTEXTS);
        return d3d_contexts_pool[context_id];
    }

public:
    static constexpr auto IMM_CTX_ID = R__NUM_PARALLEL_CONTEXTS;

    CHWCaps Caps;

    u32 BackBufferCount{};
    u32 CurrentBackBuffer{};

    ID3DDevice* pDevice = nullptr; // render device

    D3D_DRIVER_TYPE m_DriverType;

    IDXGIFactory1* m_pFactory = nullptr;
    IDXGIAdapter1* m_pAdapter = nullptr; // pD3D equivalent
    IDXGISwapChain* m_pSwapChain = nullptr;
    D3D_FEATURE_LEVEL FeatureLevel;
    bool Valid = true;
    bool ComputeShadersSupported;
    bool DoublePrecisionFloatShaderOps;
    bool SAD4ShaderInstructions;
    bool ExtendedDoublesShaderInstructions;

    ID3DDeviceContext* d3d_contexts_pool[R__NUM_CONTEXTS]{};

    bool DX10Only = false;
#ifdef HAS_DX11_2
    IDXGISwapChain2* m_pSwapChain2 = nullptr;
#endif
#ifdef HAS_DX11_3
    ID3D11Device3* pDevice3 = nullptr;
#endif
    ID3D11DeviceContext1* pContext1 = nullptr;

    using D3DCompileFunc = decltype(&D3DCompile);
    D3DCompileFunc D3DCompile = nullptr;

#if !defined(_MAYA_EXPORT)
    stats_manager stats_manager;
#endif
    TracyD3D11Ctx profiler_ctx{}; // TODO: this should be one per d3d11 context
private:
    DXGI_SWAP_CHAIN_DESC m_ChainDesc; // DevPP equivalent
    bool doPresentTest{};
    u32 m_immOwnerThread{};               // thread that owns the immediate context (see CheckImmThread)
    std::atomic<u32> m_immForeignCalls{}; // immediate-context calls observed from other threads
    XRay::Module hD3DCompiler;
    XRay::Module hDXGI;
    XRay::Module hD3D;
};

extern ECORE_API CHW HW;
} // namespace xray::render::RENDER_NAMESPACE
