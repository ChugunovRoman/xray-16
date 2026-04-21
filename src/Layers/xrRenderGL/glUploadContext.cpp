#include "stdafx.h"

#include "Layers/xrRenderGL/glUploadContext.hpp"

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
Lock s_oglGpuUploadLock;
}

Lock& OglGpuUploadLock() { return s_oglGpuUploadLock; }

OglUploadContext::OglUploadContext()
{
    if (!GEnv.Render)
        return;
    if (GEnv.Render->GetCurrentContext() == IRender::PrimaryContext)
        return;
    if (GEnv.Render->GetCurrentContext() == IRender::HelperContext)
        return;
    if (!HW.HasLoaderContext())
    {
        Msg("! OpenGL: no shared loader context; cannot issue GL from this thread");
        return;
    }
    prev = GEnv.Render->GetCurrentContext();
    int err = HW.MakeContextCurrent(IRender::HelperContext);
    if (err != 0)
    {
        for (int i = 0; i < 64; ++i)
        {
            SDL_Delay(1);
            err = HW.MakeContextCurrent(IRender::HelperContext);
            if (err == 0)
                break;
        }
    }
    if (err != 0)
    {
        Msg("! OpenGL: could not bind shared loader context: %s", SDL_GetError());
        return;
    }
    mustRestore = true;
}

bool OglUploadContext::ok() const
{
    if (!GEnv.Render)
        return false;
    const IRender::RenderContext c = GEnv.Render->GetCurrentContext();
    return c == IRender::PrimaryContext || c == IRender::HelperContext;
}

OglUploadContext::~OglUploadContext()
{
    if (mustRestore)
        HW.MakeContextCurrent(prev);
}

OglGpuScope::OglGpuScope() : m_mux(&OglGpuUploadLock()) {}
} // namespace xray::render::RENDER_NAMESPACE
