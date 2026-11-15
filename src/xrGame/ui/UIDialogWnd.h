#pragma once

#include "xrUICore/Windows/UIWindow.h"

#include "UIDialogHolder.h"

class CUIDialogWnd : public CUIWindow
{
private:
    typedef CUIWindow inherited;
    CDialogHolder* m_pParentHolder;

protected:
public:
    bool m_bWorkInPause;
    // The actor keeps walking while the dialog is the input receiver: StopAnyMove() is false, so the keys the dialog
    // does not take go on to the actor (as with the PDA). Set by the script method AllowMovement(bool) of
    // CUIDialogWnd (plans/addon_base_building/plan.md 17.13, the panel of the addon gwd_base_building: the cursor over
    // the game while a key is held). False by default - every dialog stops the actor as before
    bool m_bAllowMovement{};
    CUIDialogWnd(pcstr window_name);
    virtual ~CUIDialogWnd();

    virtual void Show(bool status);

    bool OnKeyboardAction(int dik, EUIMessages keyboard_action) override;
    bool OnControllerAction(int axis, const ControllerAxisState& state, EUIMessages controller_action) override;

    CDialogHolder* GetHolder() const { return m_pParentHolder; }
    void SetHolder(CDialogHolder* h) { m_pParentHolder = h; }

    virtual bool StopAnyMove() { return !m_bAllowMovement; }
    virtual bool NeedCursor() const { return true; }
    virtual bool NeedCenterCursor() const { return true; }
    virtual bool WorkInPause() const { return m_bWorkInPause; }
    virtual bool Dispatch(int cmd, int param) { return true; }
    virtual void ShowOrHideDialog(bool bDoHideIndicators);
    virtual void ShowDialog(bool bDoHideIndicators);
    virtual void HideDialog();

    virtual bool IR_process();

    pcstr GetDebugType() override { return "CUIDialogWnd"; }
    void FillDebugInfo() override;

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION(CUIWindow, CDialogHolder);
};
