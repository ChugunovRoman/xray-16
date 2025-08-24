#include "stdafx.h"
#include "dxUIShader.h"

namespace xray::render::RENDER_NAMESPACE
{
void dxUIShader::Copy(IUIShader& _in) { *this = *((dxUIShader*)&_in); }
void dxUIShader::create(LPCSTR sh, LPCSTR tex) { hShader.create(sh, tex); }
void dxUIShader::destroy() { hShader.destroy(); }

bool dxUIShader::GetBaseTextureResolution(Fvector2& res)
{
    if (!hShader)
        return false;

    const SPass& pass = *hShader->E[0]->passes[0];

    const STextureList& textures = *pass.T;

    if (textures.empty())
        return false;

    const R_constant* sbase = pass.constants->get(baseTexture)._get();

    CTexture& texture = *textures[sbase ? sbase->samp.index : 0].second;

    res = { float(texture.get_Width()), float(texture.get_Height()) };
    return true;
}
} // namespace xray::render::RENDER_NAMESPACE
