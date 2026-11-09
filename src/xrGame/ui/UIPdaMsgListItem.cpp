#include "StdAfx.h"

#include "UIPdaMsgListItem.h"
#include "xrUICore/XML/xrUIXmlParser.h"
#include "UIXmlInit.h"
#include "UIHelper.h"
#include "Include/xrRender/UIShader.h"

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

bool CUIPdaMsgListItem::ApplyBodycamLayout(bool active,float width,float k)
{
    if(active==m_bodycamLayout && (!active || (fsimilar(width,m_bodycamWidth) && m_bodycamPreviewReady)))return false;
    CUIStatic* widgets[]={&UIIcon,&UIIcon2,&UITimeText,&UICaptionText,&UIMsgText};
    if(active && !m_bodycamLayout)
    {
        for(int i=0;i<5;++i){auto p=widgets[i]->GetWndPos(),s=widgets[i]->GetWndSize();m_savedRects[i].set(p.x,p.y,p.x+s.x,p.y+s.y);}
        m_savedFonts[0]=UITimeText.GetFont();m_savedFonts[1]=UICaptionText.GetFont();m_savedFonts[2]=UIMsgText.GetFont();
        m_savedColors[0]=UITimeText.GetTextColor();m_savedColors[1]=UICaptionText.GetTextColor();m_savedColors[2]=UIMsgText.GetTextColor();
        m_savedMessage=UIMsgText.GetText();
        // BODYCAM keeps complete text; expiry is controlled by the reading queue.
        SetColorAnimation(nullptr,0);
        ColorAnimationSetTextureColor(255,true);ColorAnimationSetTextColor(255,true);
    }
    m_bodycamLayout=active;m_bodycamWidth=width;
    if(!active)
    {
        for(int i=0;i<5;++i)widgets[i]->SetWndRect(m_savedRects[i]);
        UITimeText.SetFont(m_savedFonts[0]);UICaptionText.SetFont(m_savedFonts[1]);UIMsgText.SetFont(m_savedFonts[2]);
        UITimeText.SetTextColor(m_savedColors[0]);UICaptionText.SetTextColor(m_savedColors[1]);UIMsgText.SetTextColor(m_savedColors[2]);
        UICaptionText.SetEllipsis(false);
        UICaptionText.SetTextComplexMode(false);
        UIMsgText.SetText(m_savedMessage.c_str());UIMsgText.AdjustHeightToText();
        SetColorAnimation("ui_main_msgs_short",LA_ONLYALPHA|LA_TEXTCOLOR|LA_TEXTURECOLOR,5000.f);
        ResetColorAnimation();
        SetWidth(width);SetHeight(_max(UIIcon.GetHeight(),UIMsgText.GetWndPos().y+UIMsgText.GetHeight())+3.f);
        return true;
    }
    SetWidth(width);
    // Fit the complete texture/atlas rectangle, preserving its actual aspect ratio.
    auto& shader=UIIcon.GetShader();
    m_bodycamPreviewReady=shader && shader->inited() && !shader->BaseTexturePending();
    if(m_bodycamPreviewReady && !UIIcon.GetStaticItem()->uFlags.test(CUIStaticItem::flValidTextureRect))
    {
        Fvector2 resolution;shader->GetBaseTextureResolution(resolution);
        if(resolution.x>0 && resolution.y>0)UIIcon.SetTextureRect(Frect().set(0,0,resolution.x,resolution.y));
        else m_bodycamPreviewReady=false;
    }
    const Frect source=m_bodycamPreviewReady?UIIcon.GetTextureRect():Frect().set(0,0,512,250);
    const float scale=_min(74.f/_max(source.width(),1.f),36.f/_max(source.height(),1.f));
    const float pw=source.width()*scale,ph=source.height()*scale;
    UIIcon.SetWndRect(Frect().set((74.f-pw)*.5f*k,1.f+(36.f-ph)*.5f,(74.f+pw)*.5f*k,1.f+(36.f+ph)*.5f));
    UIIcon.SetStretchTexture(true);
    UIIcon2.SetWndRect(Frect().set(24*k,6,50*k,32));
    UITimeText.SetFont(UI().Font().pFontLetterica18Russian);
    UICaptionText.SetFont(UI().Font().pFontLetterica18Russian);
    UIMsgText.SetFont(UI().Font().pFontLetterica18Russian);
    UITimeText.SetTextColor(color_rgba(215,225,220,255));
    UICaptionText.SetTextColor(color_rgba(225,235,230,255));
    UIMsgText.SetTextColor(color_rgba(235,240,235,255));
    UITimeText.SetWndRect(Frect().set(82*k,0,121*k,14));
    UICaptionText.SetWndRect(Frect().set(124*k,0,width,14));
    UICaptionText.SetEllipsis(false);UICaptionText.SetTextComplexMode(true);
    UICaptionText.AdjustHeightToText();
    const float header=_max(14.f,UICaptionText.GetHeight());
    UIMsgText.SetWndRect(Frect().set(82*k,header,width,header+24));
    UIMsgText.SetTextComplexMode(true);UIMsgText.SetText(m_savedMessage.c_str());UIMsgText.AdjustHeightToText();
    SetHeight(_max(43.5f,header+UIMsgText.GetHeight()+1.f));
    return true;
}
