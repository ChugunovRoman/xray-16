#include "stdafx.h"
#include "dxUIShader.h"
#include "Shader.h"

namespace xray::render::RENDER_NAMESPACE
{
void dxUIShader::Copy(IUIShader& _in) { *this = *((dxUIShader*)&_in); }
void dxUIShader::create(LPCSTR sh, LPCSTR tex) { hShader.create(sh, tex); }
void dxUIShader::destroy() { hShader.destroy(); }

bool dxUIShader::operator==(const IUIShader& other) const
{
    return hShader == static_cast<const dxUIShader&>(other).hShader;
}

CTexture* dxUIShader::GetBaseTexture() const
{
    if (!hShader)
        return nullptr;

    const ShaderElement* element = hShader->E[0]._get();
    if (!element || element->passes.empty())
        return nullptr;

    const SPass* pass = element->passes[0]._get();
    if (!pass || !pass->T)
        return nullptr;

    const STextureList& textures = *pass->T;
    if (textures.empty())
        return nullptr;

    const R_constant* sbase = nullptr;
    if (pass->constants)
        sbase = pass->constants->get(baseTexture)._get();

    return textures[sbase ? sbase->samp.index : 0].second._get();
}

xrImTextureData dxUIShader::GetImGuiTextureId()
{
    const auto texture = GetBaseTexture();
    if (!texture)
        return {};

    return
    {
        texture->GetImTextureID(),
        {
            (float)texture->get_Width(),
            (float)texture->get_Height()
        }
    };
}

bool dxUIShader::GetBaseTextureResolution(Fvector2& res)
{
    const auto texture = GetBaseTexture();
    if (!texture)
    {
        res = {};
        return false;
    }

    res = { float(texture->get_Width()), float(texture->get_Height()) };
    return true;
}

bool dxUIShader::BaseTexturePending()
{
    // 50r-slow-disk-async-textures.md: the UI asks after SetShader - a texture not loaded yet is queued by that bind
    const CTexture* texture = GetBaseTexture();
    return texture && texture->async_pending();
}
} // namespace xray::render::RENDER_NAMESPACE
