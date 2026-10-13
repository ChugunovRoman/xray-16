#include "stdafx.h"
#include "Render.h"
#include "device.h"
#include "EngineThreading.h"

namespace
{
// B2.1 light/glow graveyard (see resptrcode_light in Render.h). True only on the dedicated Game logic thread while
// it runs parallel to DoRender(): IsGameThreadRunning() is cleared by the main thread's join, so neither the main
// thread (render, post-join commands - IsGameLogicThread() falls back to main between the kicks) nor a sequential
// frame's GameThread task (no render beside it) ever defers.
bool render_ref_release_must_wait()
{
    return Device.IsGameRenderOverlapFrame() && XRay::Engine::IsGameThreadRunning() &&
        XRay::Engine::IsGameLogicThread();
}

// resptr_base::_dec() with the graveyard. A CAS loop instead of a plain decrement: the last reference must be
// recognized BEFORE it is dropped, so that it can be handed over instead of the object being deleted here.
template <class Ref, class T>
void render_ref_release(T*& p)
{
    if (!p)
        return;
    u32 count = p->ref_count.load(std::memory_order_acquire);
    for (;;)
    {
        if (count == 1 && render_ref_release_must_wait())
        {
            // Adopt our reference (no add_ref) and move it into the post-join command: the count stays 1, so
            // nothing is deleted on this thread; the command drops it on the main thread after the join.
            Ref keep(p, false);
            p = nullptr;
            Device.PostJoin([keep]() mutable { keep.destroy(); });
            return;
        }
        if (p->ref_count.compare_exchange_weak(count, count - 1, std::memory_order_acq_rel, std::memory_order_acquire))
            break;
    }
    if (count == 1)
        xr_delete(p);
}
} // namespace

void resptrcode_light::_dec() { render_ref_release<ref_light>(p_); }

void resptrcode_light::_set(IRender_Light* rhs)
{
    if (rhs)
        ++rhs->ref_count;
    _dec();
    p_ = rhs;
}

void resptrcode_glow::_dec() { render_ref_release<ref_glow>(p_); }

void resptrcode_glow::_set(IRender_Glow* rhs)
{
    if (rhs)
        ++rhs->ref_count;
    _dec();
    p_ = rhs;
}

// resources
IRender_Light::~IRender_Light()
{
    if (GEnv.Render)
        GEnv.Render->light_destroy(this);
}
IRender_Glow::~IRender_Glow()
{
    if (GEnv.Render)
        GEnv.Render->glow_destroy(this);
}

IRender::ScopedContext::ScopedContext(RenderContext context)
{
    previousContext = GEnv.Render->GetCurrentContext();
    GEnv.Render->MakeContextCurrent(context);
}

IRender::ScopedContext::~ScopedContext()
{
    GEnv.Render->MakeContextCurrent(previousContext);
}
