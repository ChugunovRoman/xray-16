// File:		UIMessagesWindow.h
// Description:	Window with MP chat and Game Log ( with PDA messages in single and Kill Messages in MP)
// Created:		22.04.2005
// Author:		Serge Vynnychenko
// Mail:		narrator@gsc-game.kiev.ua
//
// Copyright 2005 GSC Game World

#pragma once

#include "xrUICore/Windows/UIWindow.h"
#include "xrUICore/Static/UIStatic.h"

class CUIGameLog;
class CUIChatWnd;
struct GAME_NEWS_DATA;
struct KillMessageStruct;

class CUIMessagesWindow final : public CUIWindow
{
public:
    CUIMessagesWindow();

    void AddIconedPdaMessage(const GAME_NEWS_DATA* news);

    void AddLogMessage(const shared_str& msg) const;
    void AddLogMessage(KillMessageStruct& msg) const;
    void AddChatMessage(const shared_str& msg, const shared_str& author) const;
    void PendingMode(bool const is_in_pending_mode);
    CUIChatWnd* GetChatWnd() const { return m_pChatWnd; }
    void Show(bool show) override;
    void Draw() override;
    void Update() override;
    void ApplyBodycamLayout();
    void RestoreBodycamNews();

    pcstr GetDebugType() override { return "CUIMessagesWindow"; }

protected:
    virtual void Init(float x, float y, float width, float height);

    CUIGameLog* m_pChatLog{};
    CUIChatWnd* m_pChatWnd{};
    CUIGameLog* m_pGameLog{};
    bool m_in_pending_mode{};
    bool m_bodycamStyle=false,m_bodycamReady=false;
    bool m_bodycamHistoryRestored=false;
    bool m_bodycamRestoreLatest=false;
    Frect m_originalLogRect{};
    CUIStatic m_bodycamFrame{"BODYCAM SMS frame"};
    CUIStatic m_bodycamDark{"BODYCAM SMS background"};

    Frect m_pending_chat_log_rect;
    Frect m_inprogress_chat_log_rect;
};
