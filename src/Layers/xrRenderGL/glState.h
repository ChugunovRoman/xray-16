#pragma once

#include "../xrRender/SH_Texture.h"

namespace xray::render::RENDER_NAMESPACE
{
typedef struct
{
    BOOL DepthEnable;
    BOOL DepthWriteMask;
    D3DCMPFUNC DepthFunc;
    BOOL StencilEnable;
    u32 StencilMask;
    u32 StencilWriteMask;
    D3DSTENCILOP StencilFailOp;
    D3DSTENCILOP StencilDepthFailOp;
    D3DSTENCILOP StencilPassOp;
    D3DCMPFUNC StencilFunc;
    u32 StencilRef;
} D3D_DEPTH_STENCIL_STATE;

typedef struct
{
    BOOL BlendEnable;
    D3DBLEND SrcBlend;
    D3DBLEND DestBlend;
    D3DBLENDOP BlendOp;
    D3DBLEND SrcBlendAlpha;
    D3DBLEND DestBlendAlpha;
    D3DBLENDOP BlendOpAlpha;
    u32 ColorMask;
} D3D_BLEND_STATE;

class glState
{
private:
    D3DCULL rasterizerCullMode;
    D3D_DEPTH_STENCIL_STATE m_pDepthStencilState;
    D3D_BLEND_STATE m_pBlendState;
    float m_uiMipLODBias;

    GLuint m_samplerArray[CTexture::mtMaxCombinedShaderTextures];
    struct SamplerDesc
    {
        bool used{ false };
        bool dirty{ false };
        u32 addressU{ D3DTADDRESS_WRAP };
        u32 addressV{ D3DTADDRESS_WRAP };
        u32 addressW{ D3DTADDRESS_WRAP };
        u32 borderColor{ 0 };
        u32 magFilter{ D3DTEXF_LINEAR };
        u32 minFilter{ D3DTEXF_LINEAR };
        u32 mipFilter{ D3DTEXF_LINEAR };
        u32 mipLodBias{ 0 };
        u32 maxMipLevel{ 0 };
        u32 maxAnisotropy{ 1 };
        u32 comparisonFilter{ FALSE };
        u32 comparisonFunc{ D3D_COMPARISON_LESS_EQUAL };
    };
    SamplerDesc m_samplerDesc[CTexture::mtMaxCombinedShaderTextures];

public:
    glState();

    static glState* Create();

    void Apply();
    void Release();

    void UpdateRenderState(u32 name, u32 value);
    void UpdateSamplerState(u32 stage, u32 name, u32 value);
};
} // namespace xray::render::RENDER_NAMESPACE
