#include "stdafx.h"
#pragma hdrstop

#include <atomic>

namespace xray::render::RENDER_NAMESPACE
{
void D3DXRenderBase::CreateQuadIB()
{
    ZoneScoped;

    constexpr auto triCount = 4 * 1024;
    constexpr auto idxCount = triCount * 2 * 3;
    constexpr auto idxSize = idxCount * sizeof(u16);

    QuadIB.Create(idxSize, false, false);

    u16* Indices = static_cast<u16*>(QuadIB.Map());
    {
        int Cnt = 0;
        int ICnt = 0;
        for (int i = 0; i < triCount; i++)
        {
            Indices[ICnt++] = u16(Cnt + 0);
            Indices[ICnt++] = u16(Cnt + 1);
            Indices[ICnt++] = u16(Cnt + 2);

            Indices[ICnt++] = u16(Cnt + 3);
            Indices[ICnt++] = u16(Cnt + 2);
            Indices[ICnt++] = u16(Cnt + 1);

            Cnt += 4;
        }
    }
    QuadIB.Unmap(true); // upload index data
}

void CBackend::discard_pending_report(pcstr where)
{
    static std::atomic<u32> s_reports{ 0 };
    const u32 n = s_reports.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 32 || (n % 600) == 0)
        Msg("! [ctx-dirty] frame %u: context %u had an unsubmitted command list at %s - discarded #%u", Device.dwFrame,
            context_id, where, n);
    if (context_id == CHW::IMM_CTX_ID)
    {
        deferred_pending = false; // relabeled already - nothing safe to finish here
        return;
    }
    discard();
}
} // namespace xray::render::RENDER_NAMESPACE
