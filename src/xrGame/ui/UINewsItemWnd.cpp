#include "StdAfx.h"
#include "UINewsItemWnd.h"
#include "UIXmlInit.h"
#include "xrUICore/Static/UIStatic.h"
#include "game_news.h"
#include "date_time.h"
#include "UIInventoryUtilities.h"
#include "UIHelper.h"

#define ICON_SIZE 512.0f

CUINewsItemWnd::CUINewsItemWnd() : CUIWindow("CUINewsItemWnd") {}

void CUINewsItemWnd::Init(CUIXml& uiXml, LPCSTR start_from)
{
    CUIXmlInit::InitWindow(uiXml, start_from, 0, this);

    XML_NODE stored_root = uiXml.GetLocalRoot();
    XML_NODE node = uiXml.NavigateToNode(start_from, 0);
    uiXml.SetLocalRoot(node);

    m_UIImage = UIHelper::CreateStatic(uiXml, "image", this);
    m_UIImage2 = UIHelper::CreateStatic(uiXml, "image_2", this);
    m_UICaption = UIHelper::CreateStatic(uiXml, "caption_static", this, false); // no caption tag in SOC

    m_UIImage2->SetStretchTexture(true);
    m_UIImage2->SetTextureRect(Frect().set(0.0f, 0.0f, ICON_SIZE, ICON_SIZE));

    m_UIText = UIHelper::CreateStatic(uiXml, "text_static", this, false);
    m_UIDate = UIHelper::CreateStatic(uiXml, "date_static", this, false);

    // SOC
    if (!m_UIText)
        m_UIText = UIHelper::CreateStatic(uiXml, "text_cont", this, false);
    if (!m_UIDate)
        m_UIDate = UIHelper::CreateStatic(uiXml, "date_text_cont", this, false);

    uiXml.SetLocalRoot(stored_root);
}

void CUINewsItemWnd::Setup(GAME_NEWS_DATA& news_data)
{
    shared_str time_str = InventoryUtilities::GetTimeAndDateAsString(news_data.receive_time);
    u32 sz = (time_str.size() + 5) * sizeof(char);
    PSTR str = (PSTR)xr_alloca(sz);
    xr_strcpy(str, sz, time_str.c_str());
    if (m_UICaption)
        xr_strcat(str, sz, " -");
    m_UIDate->SetText(str);
    m_UIDate->AdjustWidthToText();

    if (m_UICaption)
    {
        m_UICaption->SetTextST(news_data.news_caption.c_str());
        Fvector2 pos = m_UICaption->GetWndPos();
        pos.x = m_UIDate->GetWndPos().x + m_UIDate->GetWndSize().x + 5.0f;
        m_UICaption->SetWndPos(pos);
        m_UICaption->SetWidth(_min(m_UIText->GetWidth() - m_UIDate->GetWidth() - 5.0f, m_UICaption->GetWidth()));
    }

    m_UIText->SetTextST(news_data.news_text.c_str());
    m_UIText->AdjustHeightToText();
    float h1 = m_UIText->GetWndPos().y + m_UIText->GetHeight() + 6.0f;

    if (news_data.faction_name != nullptr)
    {
        pcstr icon = pSettingsFE->read_if_exists<pcstr>(news_data.faction_name.c_str(), "icon", make_string("icons\\patches\\%s", news_data.faction_name.c_str()).c_str());
        pcstr sms_bg = pSettingsFE->read_if_exists<pcstr>(news_data.faction_name.c_str(), "sms_bg", "ui\\icons\\sms\\sms7");
        m_UIImage->InitTexture(sms_bg);
        m_UIImage2->InitTexture(icon);   
    }
    else
    {
        m_UIImage->InitTexture(news_data.texture_name.c_str());
        m_UIImage2->SetTextureRect(Frect().set(0.0f, 0.0f, 0.0f, 0.0f));
    }

    float h3 = m_UIImage->GetWndPos().y + m_UIImage->GetHeight();
    h1 = _max(h1, h3);
    SetHeight(h1);
}
