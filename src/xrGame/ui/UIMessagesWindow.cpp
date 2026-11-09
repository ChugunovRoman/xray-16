// File:		UIMessagesWindow.h
// Description:	Window with MP chat and Game Log ( with PDA messages in single and Kill Messages in MP)
// Created:		22.04.2005
// Author:		Serge Vynnychenko
// Mail:		narrator@gsc-game.kiev.ua
//
// Copyright 2005 GSC Game World

#include "StdAfx.h"
#include "UIMessagesWindow.h"
#include "../bodycam_motion.h"
#include "../Level.h"
#include "../UIGameCustom.h"
#include "UIGameLog.h"
#include "UIChatWnd.h"
#include "xrUICore/XML/xrUIXmlParser.h"
#include "UIXmlInit.h"
#include "UIInventoryUtilities.h"
#include "game_news.h"
#include "xrEngine/StringTable/StringTable.h"
#include "../alife_registry_wrappers.h"
#include "UIPdaMsgListItem.h"
#include "xrGame/game_type.h"

constexpr cpcstr CHAT_LOG_LIST_PENDING = "chat_log_list_pending";

CUIMessagesWindow::CUIMessagesWindow() : CUIWindow("CUIMessagesWindow")
{
    Init(0, 0, UI_BASE_WIDTH, UI_BASE_HEIGHT);
}

void CUIMessagesWindow::AddLogMessage(KillMessageStruct& msg) const { m_pGameLog->AddLogMessage(msg); }
void CUIMessagesWindow::AddLogMessage(const shared_str& msg) const { m_pGameLog->AddLogMessage(msg.c_str()); }

void CUIMessagesWindow::PendingMode(bool const is_pending_mode)
{
    if (is_pending_mode)
    {
        if (m_in_pending_mode)
            return;

        m_pChatWnd->PendingMode(is_pending_mode);
        m_pChatLog->SetWndRect(m_pending_chat_log_rect);
        m_in_pending_mode = true;
        return;
    }
    if (!m_in_pending_mode)
        return;

    m_pChatWnd->PendingMode(is_pending_mode);
    m_pChatLog->SetWndRect(m_inprogress_chat_log_rect);
    m_in_pending_mode = false;
}

void CUIMessagesWindow::Init(float x, float y, float width, float height)
{
    CUIXml xml;
    xml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, "messages_window.xml");
    m_pGameLog = xr_new<CUIGameLog>();
    m_pGameLog->SetAutoDelete(true);
    m_pGameLog->Show(true);
    AttachChild(m_pGameLog);
    if (IsGameTypeSingle())
    {
        CUIXmlInit::InitScrollView(xml, "sp_log_list", 0, m_pGameLog);
    }
    else
    {
        u32 color;
        CGameFont* pFont;

        m_pChatLog = xr_new<CUIGameLog>();
        m_pChatLog->SetAutoDelete(true);
        m_pChatLog->Show(true);
        AttachChild(m_pChatLog);
        m_pChatWnd = xr_new<CUIChatWnd>();
        m_pChatWnd->SetAutoDelete(true);
        AttachChild(m_pChatWnd);

        CUIXmlInit::InitScrollView(xml, "mp_log_list", 0, m_pGameLog);
        CUIXmlInit::InitFont(xml, "mp_log_list:font", 0, color, pFont);
        m_pGameLog->SetTextAtrib(pFont, color);

        CUIXmlInit::InitScrollView(xml, "chat_log_list", 0, m_pChatLog);
        m_inprogress_chat_log_rect = m_pChatLog->GetWndRect();

        m_in_pending_mode = false;

        XML_NODE pending_chat_list = xml.NavigateToNode(CHAT_LOG_LIST_PENDING);

        if (pending_chat_list)
        {
            m_pending_chat_log_rect.x1 = xml.ReadAttribFlt(CHAT_LOG_LIST_PENDING, 0, "x");
            m_pending_chat_log_rect.y1 = xml.ReadAttribFlt(CHAT_LOG_LIST_PENDING, 0, "y");
            m_pending_chat_log_rect.x2 = xml.ReadAttribFlt(CHAT_LOG_LIST_PENDING, 0, "width");
            m_pending_chat_log_rect.y2 = xml.ReadAttribFlt(CHAT_LOG_LIST_PENDING, 0, "height");
            m_pending_chat_log_rect.rb.add(m_pending_chat_log_rect.lt);
        }
        else
            m_pending_chat_log_rect = m_inprogress_chat_log_rect;

        CUIXmlInit::InitFont(xml, "chat_log_list:font", 0, color, pFont);
        m_pChatLog->SetTextAtrib(pFont, color);

        m_pChatWnd->Init(xml);
    }
}

static bool BodycamExcludedNews(const GAME_NEWS_DATA* news)
{
    if(news->news_text.c_str())
    {
        const auto text=CStringTable().translate(news->news_text);
        if(text.c_str() && (strstr(text.c_str(),"Fatal Error") || strstr(news->news_text.c_str(),"st_yuriy_semetskiy_death_")))return true;
    }
    return false;
}

void CUIMessagesWindow::RestoreBodycamNews()
{
    if(m_bodycamHistoryRestored || !gw_bodycam_motion::hud_active())return;
    auto* actor=smart_cast<CActor*>(Level().CurrentViewEntity());
    if(!actor || !actor->game_news_registry)return;
    m_bodycamHistoryRestored=true;
    // Already-delivered rows are never re-imported. Registry is the authoritative saved history.
    if(!m_pGameLog->Empty())return;
    auto& history=actor->game_news_registry->registry().objects();
    xr_vector<const GAME_NEWS_DATA*> latest;
    for(auto it=history.rbegin();it!=history.rend() && latest.size()<2;++it)
        if(!BodycamExcludedNews(&*it) && it->texture_name.size())latest.push_back(&*it);
    for(auto it=latest.rbegin();it!=latest.rend();++it)AddIconedPdaMessage(*it);
    m_bodycamRestoreLatest=!latest.empty();
    if(strstr(Core.Params,"-bodycam_sms_test"))Msg("* [bodycam_sms] restored=%u history=%u",u32(latest.size()),u32(history.size()));
}

void CUIMessagesWindow::AddIconedPdaMessage(const GAME_NEWS_DATA* news)
{
    if(gw_bodycam_motion::hud_active())
    {
        if(BodycamExcludedNews(news))return;
        RestoreBodycamNews();
        if(strstr(Core.Params,"-bodycam_sms_test"))Msg("* [bodycam_sms] add caption=%s bytes=%u",news->news_caption.c_str(),news->news_text.size());
    }
    CUIPdaMsgListItem* pItem = m_pGameLog->AddPdaMessage();

    const auto time_str = GetTimeAsString(news->receive_time, InventoryUtilities::etpTimeToMinutes);
    pItem->UITimeText.SetText(time_str.c_str());
    pItem->UITimeText.AdjustWidthToText();
    Fvector2 p = pItem->UICaptionText.GetWndPos();
    p.x = pItem->UITimeText.GetWndPos().x + pItem->UITimeText.GetWidth() + 3.0f;
    pItem->UICaptionText.SetWndPos(p);
    pItem->UICaptionText.SetTextST(news->news_caption.c_str());
    pItem->UIMsgText.SetTextST(news->news_text.c_str());
    pItem->UIMsgText.AdjustHeightToText();

    pItem->SetColorAnimation(
        "ui_main_msgs_short", LA_ONLYALPHA | LA_TEXTCOLOR | LA_TEXTURECOLOR, float(news->show_time));
    if (news->faction_name.c_str() != nullptr)
    {
        pcstr icon = pSettingsFE->read_if_exists<pcstr>(news->faction_name.c_str(), "icon", make_string("icons\\patches\\%s", news->faction_name.c_str()).c_str());
        pcstr sms_bg = pSettingsFE->read_if_exists<pcstr>(news->faction_name.c_str(), "sms_bg", "ui\\icons\\sms\\sms7");
        pItem->UIIcon.InitTexture(sms_bg);
        pItem->UIIcon2.InitTexture(icon);
    }
    else
        pItem->UIIcon.InitTexture(news->texture_name.c_str());

    const float h1 = _max(pItem->UIIcon.GetHeight(), pItem->UIMsgText.GetWndPos().y + pItem->UIMsgText.GetHeight());
    pItem->SetHeight(h1 + 3.0f);

    m_pGameLog->SendMessage(pItem, CHILD_CHANGED_SIZE);
}

void CUIMessagesWindow::AddChatMessage(const shared_str& msg, const shared_str& author) const
{
    m_pChatLog->AddChatMessage(msg.c_str(), author.c_str());
}

void CUIMessagesWindow::Show(bool show)
{
    if (m_pChatWnd)
        m_pChatWnd->Show(show);
    if (m_pGameLog)
        m_pGameLog->Show(show);
    if (m_pChatLog)
        m_pChatLog->Show(show);
}


void CUIMessagesWindow::ApplyBodycamLayout()
{
    if(!IsGameTypeSingle() || !m_pGameLog)return;
    const bool active=gw_bodycam_motion::hud_active();
    const float k=UI().get_current_kx();
    if(active && !m_bodycamStyle)
    {
        const auto p=m_pGameLog->GetWndPos(),s=m_pGameLog->GetWndSize();
        m_originalLogRect.set(p.x,p.y,p.x+s.x,p.y+s.y);
    }
    if(active)
    {
        RestoreBodycamNews();
        if(!m_bodycamReady)
        {
            m_bodycamFrame.InitTexture("ui\\gw_bodycam\\sms");
            m_bodycamFrame.SetTextureRect(Frect().set(27,112,1642,831));
            m_bodycamFrame.SetStretchTexture(true);
            m_bodycamDark.InitTextureEx("ui\\gw_bodycam\\gps","hud\\bodycam_back");
            m_bodycamDark.SetStretchTexture(true);m_bodycamReady=true;
        }
        m_bodycamFrame.SetWndRect(Frect().set(8*k,618,488*k,760));
        m_bodycamDark.SetWndRect(Frect().set(31*k,643,465*k,734));
        m_pGameLog->SetWndRect(Frect().set(37*k,644,459*k,734));
        xr_vector<CUIWindow*> rejected;
        for(auto* item:m_pGameLog->Items())
            if(auto* news=smart_cast<CUIPdaMsgListItem*>(item))
                if(strstr(news->UIMsgText.GetText(),"Fatal Error"))rejected.push_back(item);
        for(auto* item:rejected)m_pGameLog->RemoveWindow(item);
    }
    else if(m_bodycamStyle){m_pGameLog->SetWndRect(m_originalLogRect);m_bodycamHistoryRestored=false;}
    bool changed=active!=m_bodycamStyle;
    if(active || m_bodycamStyle)
    {
        for(auto* item:m_pGameLog->Items())
            if(auto* news=smart_cast<CUIPdaMsgListItem*>(item))
                changed=news->ApplyBodycamLayout(active,active?m_pGameLog->GetWidth()-m_pGameLog->GetHorizIndent():m_pGameLog->GetDesiredChildWidth(),k)||changed;
    }
    m_bodycamStyle=active;
    if(changed)m_pGameLog->ForceUpdate();
    if(active && m_bodycamRestoreLatest){m_pGameLog->BodycamLatest();m_bodycamRestoreLatest=false;}
}

void CUIMessagesWindow::Update()
{
    ApplyBodycamLayout();CUIWindow::Update();
}

void CUIMessagesWindow::Draw()
{
    if(!IsGameTypeSingle() || !gw_bodycam_motion::hud_active())
    {
        CUIWindow::Draw();return;
    }
    auto* actor=smart_cast<CActor*>(Level().CurrentViewEntity());
    // m_pGameLog->IsShown(): hud:hide_messages() (Show(false)) hides the log, as CUIWindow::Draw does natively
    if(!actor || !actor->g_Alive() || CurrentGameUI()->TopInputReceiver() || Device.Paused() || !gw_bodycam_motion::sms_visible() || !m_pGameLog->IsShown())return;
    m_bodycamDark.Draw();m_bodycamFrame.Draw();m_pGameLog->Draw();
}
