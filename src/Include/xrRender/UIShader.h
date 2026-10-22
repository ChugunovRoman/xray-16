#ifndef UIShader_included
#define UIShader_included
#pragma once

class IUIShader
{
public:
    virtual ~IUIShader() { ; }
    virtual void Copy(IUIShader& _in) = 0;
    virtual void create(LPCSTR sh, LPCSTR tex = nullptr) = 0;
    virtual bool inited() = 0;
    virtual void destroy() = 0;

    [[nodiscard]]
    virtual bool operator==(const IUIShader& other) const = 0;

    virtual bool GetBaseTextureResolution(Fvector2& res) = 0;
    virtual xrImTextureData GetImGuiTextureId() = 0;
    // 50r-slow-disk-async-textures.md (plans/optimization_mt): the base texture is still being read (a slow disk, the
    // async loading of the render): its size is not known yet without reading it on the spot, and it draws as nothing.
    // The UI skips drawing it this frame instead of asking the size (GetBaseTextureResolution would read it now).
    virtual bool BaseTexturePending() { return false; }
};

#endif //	UIShader_included
