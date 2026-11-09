//////////////////////////////////////////////////////////////////////
// UIPdaMsgListItem.h: элемент окна списка в основном
// экране для сообщений PDA
//////////////////////////////////////////////////////////////////////
#pragma once

#include "xrUICore/Static/UIStatic.h"

class CUIPdaMsgListItem final : public CUIColorAnimConrollerContainer
{
    using inherited = CUIColorAnimConrollerContainer;

public:
    CUIPdaMsgListItem() : CUIColorAnimConrollerContainer("CUIPdaMsgListItem") {}

    void InitPdaMsgListItem(const Fvector2& size);
    void SetFont(CGameFont* pFont) override;
    bool ApplyBodycamLayout(bool active,float width,float k);
private:
    bool m_bodycamLayout=false;
    bool m_bodycamPreviewReady=false;
    float m_bodycamWidth=0;
    Frect m_savedRects[5]{};
    CGameFont* m_savedFonts[3]{};
    u32 m_savedColors[3]{};
    xr_string m_savedMessage;
public:

    pcstr GetDebugType() override { return "CUIPdaMsgListItem"; }

    CUIStatic UIIcon       { "Icon" };
    CUIStatic UIIcon2      { "Icon2" };
    CUIStatic UITimeText   { "Time text" };
    CUIStatic UICaptionText{ "Caption text" };
    CUIStatic UIMsgText    { "Message text" };
};
