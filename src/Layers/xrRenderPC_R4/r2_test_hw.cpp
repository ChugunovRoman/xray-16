#include "stdafx.h"

#include "Layers/xrRender/D3DXRenderBase.h"

namespace xray::render::RENDER_NAMESPACE
{
class DX11TestHelper
{
    SDL_Window* m_window = nullptr;
    CHW m_hw;

public:
    DX11TestHelper()
    {
        m_window = SDL_CreateWindow("TestDX11Window", 0, 0, 1, 1, SDL_WINDOW_HIDDEN);
        if (!m_window)
        {
            Log("~ Cannot create helper window for DirectX 11 test:", SDL_GetError());
            return;
        }
        m_hw.CreateDevice(m_window);
    }

    bool Successful() const
    {
        return m_window && m_hw.Valid;
    }

    const CHW& GetHW() const { return m_hw; }

    ~DX11TestHelper()
    {
        m_hw.DestroyDevice();
        SDL_DestroyWindow(m_window);
    }
};


BOOL xrRender_test_hw()
{
    ZoneScoped;

    // The first LoadLibrary("d3d11") of the process happens inside this probe (DX11TestHelper
    // -> CHW::CreateDevice). RenderDoc must be loaded and hooked BEFORE that module load,
    // otherwise the D3D11 exports are not intercepted and neither the probe device nor the
    // later main device ever gets wrapped - TriggerCapture() then has nothing to consume.
    D3DXRenderBase::InitializeRenderDoc();

    const DX11TestHelper helper;
    if (!helper.Successful())
        return FALSE;

    const auto level = helper.GetHW().FeatureLevel;
    if (level >= D3D_FEATURE_LEVEL_11_0)
        return TRUE + TRUE; // XXX: remove hack
    if (level >= D3D_FEATURE_LEVEL_10_0)
        return TRUE;
    return FALSE;
}
} // namespace xray::render::RENDER_NAMESPACE
