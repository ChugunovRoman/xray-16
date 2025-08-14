#include "StdAfx.h"

#include "UIPdaMsgListItem.h"
#include "xrUICore/XML/xrUIXmlParser.h"
#include "UIXmlInit.h"
#include "UIHelper.h"

#define ICON_SIZE 512.0f

void CUIPdaMsgListItem::SetFont(CGameFont* pFont)
{
    UITimeText.SetFont(pFont);
    UICaptionText.SetFont(pFont);
    UIMsgText.SetFont(pFont);
}

void CUIPdaMsgListItem::InitPdaMsgListItem(const Fvector2& size)
{
    inherited::SetWndSize(size);

    CUIXml uiXml;
    uiXml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, "maingame_pda_msg.xml");
    AttachChild(&UIIcon);
    AttachChild(&UIIcon2);
    CUIXmlInit::InitStatic(uiXml, "icon_static", 0, &UIIcon);
    CUIXmlInit::InitStatic(uiXml, "icon_static_2", 0, &UIIcon2);

    UIIcon2.SetStretchTexture(true);
    UIIcon2.SetTextureRect(Frect().set(0, 0, ICON_SIZE, ICON_SIZE));

    if (CUIXmlInit::InitStatic(uiXml, "time_static", 0, &UITimeText, false))
        AttachChild(&UITimeText);

    if (CUIXmlInit::InitStatic(uiXml, "caption_static", 0, &UICaptionText, false))
        AttachChild(&UICaptionText);

    if (CUIXmlInit::InitStatic(uiXml, "msg_static", 0, &UIMsgText, false) ||
        CUIXmlInit::InitStatic(uiXml, "text_static", 0, &UIMsgText, false))
    {
        AttachChild(&UIMsgText);
    }

    UIHelper::CreateStatic(uiXml, "name_static", this, false);
}
