#include "stdafx.h"

namespace xray::render::RENDER_NAMESPACE
{
void CRenderTarget::phase_occq()
{
    // Dimensions follow the bound depth (may be a downsized svp twin during the scope pass);
    // GL forbids mixing attachment sizes inside one FBO, so this must stay consistent.
    const u32 w = rt_MSAADepth->dwWidth;
    const u32 h = rt_MSAADepth->dwHeight;
    if (!RImplementation.o.msaa)
        u_setrt(RCache, w, h, get_base_rt(), 0, 0, rt_MSAADepth);
    else
        u_setrt(RCache, w, h, 0, 0, 0, rt_MSAADepth);
    RCache.set_Shader(s_occq);
    RCache.set_CullMode(CULL_CCW);
    RCache.set_Stencil(TRUE, D3DCMP_LESSEQUAL, 0x01, 0xff, 0x00);
    RCache.set_ColorWriteEnable(FALSE);
}
} // namespace xray::render::RENDER_NAMESPACE
