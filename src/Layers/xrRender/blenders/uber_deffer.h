#pragma once

namespace xray::render::RENDER_NAMESPACE
{
void uber_deffer(CBlender_Compile& C, bool hq, LPCSTR _vspec, LPCSTR _pspec, BOOL _aref, LPCSTR _detail_replace = 0,
    bool DO_NOT_FINISH = false);
#if defined(USE_OGL) || defined(USE_DX11)
void uber_deffer_hud(CBlender_Compile& C, bool hq, LPCSTR _vspec, LPCSTR _pspec, BOOL _aref);
#endif
void uber_shadow(CBlender_Compile& C, LPCSTR _vspec);
} // namespace xray::render::RENDER_NAMESPACE
