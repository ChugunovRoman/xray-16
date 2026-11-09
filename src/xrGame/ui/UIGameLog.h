//=============================================================================
//  Filename:   UIGameLog.h
//	Created by Vitaly 'Mad Max' Maximov, mad-max@gsc-game.kiev.ua
//	Copyright 2005. GSC Game World
//	---------------------------------------------------------------------------
//  Multiplayer game log window
//=============================================================================

#pragma once

#include "xrUICore/ScrollView/UIScrollView.h"
#include "bodycam_sms_scroll.h"

class CUIXml;
class CUIPdaKillMessage;
class CUIPdaMsgListItem;
class CUIStatic;
struct KillMessageStruct;

class CUIGameLog final : public CUIScrollView
{
public:
    CUIGameLog();
    CUIStatic* AddLogMessage(LPCSTR msg);
    CUIPdaKillMessage* AddLogMessage(KillMessageStruct& msg);
    CUIPdaMsgListItem* AddPdaMessage();
    void AddChatMessage(LPCSTR msg, LPCSTR author);
    virtual void Update();
    void Draw() override;
    void BodycamLatest();

    void SetTextAtrib(CGameFont* pFont, u32 color);
    u32 GetTextColor() { return txt_color; }

    pcstr GetDebugType() override { return "CUIGameLog"; }

private:
    void RecalcSize() override;
    bodycam_sms::Scroll m_bodycamScroll;
    u32 m_bodycamLastTime=0,m_bodycamLastFrame=0,m_bodycamLogTime=0;
    WINDOW_LIST toDelList;
    float kill_msg_height;
    u32 txt_color;
    CGameFont* m_pFont;
};
