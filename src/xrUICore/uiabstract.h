#pragma once

#pragma warning(disable : 4511)
#pragma warning(disable : 4512)

#include "Common/Noncopyable.hpp"
#include "xrEngine/GameFont.h"

using ETextAlignment = CGameFont::EAligment;

enum EVTextAlignment : u8
{
    valTop = 0, valCenter, valBottom
};

class XR_NOVTABLE ITextureOwner
{
public:
    virtual ~ITextureOwner() = default;
    virtual bool InitTexture(pcstr texture, bool fatal = true) = 0;
    virtual bool InitTextureEx(pcstr texture, pcstr shader, bool fatal = true) = 0;
    virtual void SetTextureRect(const Frect& r) = 0;
    virtual const Frect& GetTextureRect() const = 0;
    virtual void SetTextureColor(u32 color) = 0;
    virtual u32 GetTextureColor() const = 0;
    virtual void SetStretchTexture(bool stretch) = 0;
    virtual bool GetStretchTexture() = 0;
};

// Window
enum EWindowAlignment : u8
{
    waNone   = 0,
    waLeft   = 1 << 0,
    waRight  = 1 << 1,
    waTop    = 1 << 2,
    waBottom = 1 << 3,
    waCenter = 1 << 4,
};

class CUISelectable
{
protected:
    bool m_bSelected;

public:
    CUISelectable() : m_bSelected(false) {}
    bool GetSelected() const { return m_bSelected; }
    virtual void SetSelected(bool b) { m_bSelected = b; }
};
