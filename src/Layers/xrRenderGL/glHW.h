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
