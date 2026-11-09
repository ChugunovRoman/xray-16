//=============================================================================
//  Filename:   UIGameLog.h
//	Created by Vitaly 'Mad Max' Maximov, mad-max@gsc-game.kiev.ua
//	Copyright 2005. GSC Game World
//	---------------------------------------------------------------------------
//  Multiplayer game log window
//=============================================================================
#include "StdAfx.h"
#include "UIGameLog.h"
#include "../bodycam_motion.h"
#include "../Level.h"
#include "../Actor.h"
#include "../UIGameCustom.h"
#include "UIXmlInit.h"
#include "UIPdaMsgListItem.h"
#include "UIPdaKillMessage.h"
#include "xrUICore/Lines/UILines.h"

CUIGameLog::CUIGameLog()
{
    kill_msg_height = 20;
    txt_color = 0xff000000;
    m_pFont = NULL;
}

CUIStatic* CUIGameLog::AddLogMessage(LPCSTR msg)
{
    CUIStatic* pItem{};
    ADD_TEXT_TO_VIEW3(msg, pItem, this);
    pItem->SetFont(m_pFont);
    pItem->SetTextColor(txt_color);
    pItem->SetColorAnimation("ui_main_msgs_short", LA_ONLYALPHA | LA_TEXTCOLOR, 5000.0f);
    ForceUpdate();
    return pItem;
}

CUIPdaMsgListItem* CUIGameLog::AddPdaMessage()
{
    CUIPdaMsgListItem* pItem = xr_new<CUIPdaMsgListItem>();
    pItem->InitPdaMsgListItem(Fvector2().set(GetDesiredChildWidth(), 10.0f));
    pItem->SetColorAnimation("ui_main_msgs_short", LA_ONLYALPHA | LA_TEXTCOLOR | LA_TEXTURECOLOR);
    AddWindow(pItem, true);

    return pItem;
}

CUIPdaKillMessage* CUIGameLog::AddLogMessage(KillMessageStruct& msg)
{
    CUIPdaKillMessage* pItem = xr_new<CUIPdaKillMessage>();
    pItem->SetWidth(GetDesiredChildWidth());
    pItem->SetHeight(kill_msg_height);
    pItem->Init(msg, m_pFont);
    AddWindow(pItem, true);
    return pItem;
}

void CUIGameLog::AddChatMessage(LPCSTR msg, LPCSTR author)
{
    pstr fullLine;
    STRCONCAT(fullLine, author, " ", msg);

    _TrimRight(fullLine);

    auto* pItem = xr_new<CUIStatic>("Chat message");
    pItem->SetTextComplexMode(true);
    pItem->SetText(fullLine);
    pItem->SetCutWordsMode(true);
    pItem->SetFont(m_pFont);
    pItem->SetTextColor(txt_color);
    pItem->SetColorAnimation("ui_main_msgs_short", LA_ONLYALPHA | LA_TEXTCOLOR, 5000.0f);
    pItem->SetWidth(this->GetDesiredChildWidth());
    pItem->AdjustHeightToText();
    AddWindow(pItem, true);
}

void CUIGameLog::SetTextAtrib(CGameFont* pFont, u32 color)
{
    m_pFont = pFont;
    txt_color = color;
}

void CUIGameLog::Draw()
{
    const bool bodycam=IsGameTypeSingle() && gw_bodycam_motion::hud_active();
    if(!bodycam){CUIScrollView::Draw();return;}
    Frect rect;GetAbsoluteRect(rect);UI().PushScissor(rect);
    for(auto* item:Items())
    {
        Frect child;item->GetAbsoluteRect(child);
        if(item->GetVisible() && child.y2>rect.y1 && child.y1<rect.y2)item->Draw();
    }
    UI().PopScissor();
}

void CUIGameLog::RecalcSize()
{
    if(!IsGameTypeSingle() || !gw_bodycam_motion::hud_active()){CUIScrollView::RecalcSize();return;}
    float y=0,width=0;
    for(auto* item:Items())
    {
        item->SetWndPos(Fvector2().set(m_leftIndent,y));
        y+=item->GetHeight()+m_vertInterval;width=_max(width,item->GetWidth());
    }
    m_pad->SetWndSize(Fvector2().set(width,Empty()?0:y-m_vertInterval));
    m_flags.set(eNeedRecalc,false);m_visible_rgn.set(-1,-1);
}

void CUIGameLog::BodycamLatest()
{
    if(m_flags.test(eNeedRecalc))RecalcSize();
    std::vector<bodycam_sms::Row> rows;
    for(auto* item:Items())rows.push_back({item->GetWndPos().y,item->GetWndPos().y+item->GetHeight()});
    m_bodycamScroll.Latest(rows,GetHeight());
}

void CUIGameLog::Update()
{
    if (gw_bodycam_motion::hud_active() && IsGameTypeSingle())
    {
        // No mouse interaction/scrollbar and no expiry of unread messages.
        CUIWindow::Update();
        if (m_flags.test(eNeedRecalc))RecalcSize();
        const u32 now=Device.dwTimeContinual;
        const double dt=m_bodycamLastTime && Device.dwFrame-m_bodycamLastFrame==1?double(now-m_bodycamLastTime)/1000.:0.;
        m_bodycamLastTime=now;m_bodycamLastFrame=Device.dwFrame;
        std::vector<bodycam_sms::Row> rows;
        for(auto* item:Items())rows.push_back({item->GetWndPos().y,item->GetWndPos().y+item->GetHeight()});
        auto* actor=smart_cast<CActor*>(Level().CurrentViewEntity());
        const bool animate=actor && actor->g_Alive() && !Device.Paused() && !CurrentGameUI()->TopInputReceiver();
        m_bodycamScroll.Update(dt,rows,GetHeight(),gw_bodycam_motion::sms_visible()!=0,animate);
        // Unread rows are capped: with the SMS hidden (F11) the cursor stands still, and a news flow faster than
        // the reading pace grows the queue without end. Past the cap skip to the latest rows, as showing the SMS
        // again does, so the trim below releases the backlog (the PDA keeps every message).
        if(Items().size()>256)m_bodycamScroll.Latest(rows,GetHeight());
        // Release only already-read, off-screen UI rows. The actor's saved news registry is untouched.
        if(Items().size()>128 && m_bodycamScroll.cursor>2)
        {
            size_t count=std::min(m_bodycamScroll.cursor-2,Items().size()-2);
            const float shift=GetItem(u32(count))->GetWndPos().y;
            for(size_t i=0;i<count;++i)RemoveWindow(GetItem(0));
            m_bodycamScroll.RemovePrefix(count,shift);RecalcSize();
        }
        m_pad->SetWndPos(Fvector2().set(0.f,-m_bodycamScroll.offset));
        if(strstr(Core.Params,"-bodycam_sms_test") && now-m_bodycamLogTime>=500)
        {
            m_bodycamLogTime=now;
            Msg("* [bodycam_sms] rows=%u cursor=%u phase=%d offset=%.3f max=%.3f",GetSize(),u32(m_bodycamScroll.cursor),int(m_bodycamScroll.phase),m_bodycamScroll.offset,_max(0.f,m_pad->GetHeight()-GetHeight()));
        }
        return;
    }
    m_bodycamLastTime=0;m_bodycamScroll=bodycam_sms::Scroll{};
    CUIScrollView::Update();
    toDelList.clear();

    auto it = m_pad->GetChildWndList().begin();
    auto it_e = m_pad->GetChildWndList().end();

    for (; it != it_e; ++it)
    {
        CUIWindow* w = *it;
        if (!w || w == reinterpret_cast<CUIWindow*>(static_cast<ptrdiff_t>(-1)))
            continue;

        CUILightAnimColorConroller* pItem = smart_cast<CUILightAnimColorConroller*>(w);
        if (!pItem)
            continue;

        if (!pItem->IsColorAnimationPresent())
            toDelList.push_back(w);
    }

    // Delete elements
    it_e = toDelList.end();

    for (it = toDelList.begin(); it != it_e; ++it)
        RemoveWindow(*it);

    // REMOVE INVISIBLE AND PART VISIBLE ITEMS
    if (m_flags.test(eNeedRecalc))
        RecalcSize();

    toDelList.clear();
    Frect visible_rect;
    GetAbsoluteRect(visible_rect);
    it_e = m_pad->GetChildWndList().end();
    for (it = m_pad->GetChildWndList().begin(); it != it_e; ++it)
    {
        Frect r;
        (*it)->GetAbsoluteRect(r);
        r.shrink(3.0f, 3.0f);

        if (!(visible_rect.in(r.x1, r.y1) && visible_rect.in(r.x2, r.y1) && visible_rect.in(r.x1, r.y2) &&
                visible_rect.in(r.x2, r.y2)))
            toDelList.push_back(*it);
    }

    // Delete elements
    it_e = toDelList.end();
    for (it = toDelList.begin(); it != it_e; ++it)
        RemoveWindow(*it);

    if (m_flags.test(eNeedRecalc))
        RecalcSize();
}
