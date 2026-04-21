#pragma once

#include "Common/Noncopyable.hpp"
#include "xrCore/Threading/Lock.hpp"
#include "xrCore/Threading/ScopeLock.hpp"
#include "xrEngine/Render.h"

namespace xray::render::RENDER_NAMESPACE
{
/** Single lock for cross-thread GL work on shared loader / primary contexts (textures, buffers, etc.). */
Lock& OglGpuUploadLock();

/**
 * If the current thread does not hold the primary GL context, bind the secondary shared "loader"
 * context so GL calls are valid. Restores previous binding on destruction.
 */
class OglUploadContext final : Noncopyable
{
    IRender::RenderContext prev{ IRender::NoContext };
    bool mustRestore{ false };

public:
    OglUploadContext();
    bool ok() const;
    ~OglUploadContext();
};

/** Lock + OglUploadContext (mutex released only after GL state is restored). */
class OglGpuScope final : Noncopyable
{
    ScopeLock m_mux;
    OglUploadContext ctx;

public:
    OglGpuScope();
    bool ok() const { return ctx.ok(); }
};
} // namespace xray::render::RENDER_NAMESPACE
