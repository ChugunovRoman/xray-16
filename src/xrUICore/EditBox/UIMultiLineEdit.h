#pragma once

// WinUser.h defines SendMessage -> SendMessageA; must be undefined before UIWindow.h is parsed
// so CUIWindow keeps the method name SendMessage (see CUIWindow::SendMessage).
#ifdef SendMessage
#undef SendMessage
#endif

#include "xrUICore/Static/UIStatic.h"

namespace text_editor
{
class multiline_edit_control;
}

class CUIFrameLineWnd;
class CUIScrollBar;
class CUILines;

class XRUICORE_API CUIMultiLineEdit : public CUIStatic
{
protected:
    using inherited = CUIStatic;
    typedef fastdelegate::FastDelegate0<void> Callback;

public:
    CUIMultiLineEdit();
    ~CUIMultiLineEdit() override;

    void Init(u32 max_char_count = 4096, bool read_only = false);

    void InitCustomEdit(Fvector2 pos, Fvector2 size);

    bool InitTexture(pcstr texture, bool fatal = true) override;
    bool InitTextureEx(pcstr texture, pcstr shader, bool fatal = true) override;

    void SendMessage(CUIWindow* pWnd, s16 msg, void* pData = NULL) override;

    bool OnMouseAction(float x, float y, EUIMessages mouse_action) override;
    bool OnKeyboardAction(int dik, EUIMessages keyboard_action) override;
    bool OnTextInput(pcstr text) override;

    void Update() override;
    void Draw() override;
    void DrawText() override;
    void Show(bool status) override;
    void Enable(bool status) override;

    void CaptureFocus(bool bCapture);
    void SetNextFocusCapturer(CUIMultiLineEdit* next_capturer) { m_next_focus_capturer = next_capturer; }

    void ClearText();
    void SetText(pcstr str) override;
    pcstr GetText() const override;

    void SetTextFromLtxLine(pcstr ltx_line);
    pcstr GetTextAsLtxLine() const;

    pcstr GetDebugType() override { return "CUIMultiLineEdit"; }

private:
    text_editor::multiline_edit_control& ec();
    text_editor::multiline_edit_control const& ec() const;

    void Register_callbacks();

    void EnsureVScrollBar();
    void LayoutVScrollBar();
    void SyncScrollLayout(CUILines* tc);
    float GetScrollPixels() const;
    void ScrollToShowCursorIfNeeded();
    float TextVIndentForLayout(CUILines* tc) const;

    void press_escape();
    void press_commit();
    void press_tab();
    void nothing();

    text_editor::multiline_edit_control* m_editor_control;
    u32 m_last_key_state_time;
    bool m_bInputFocus;
    bool m_force_update;
    bool m_read_mode;
    CUIMultiLineEdit* m_next_focus_capturer;
    CUIFrameLineWnd* m_frameLine;
    CUIScrollBar* m_vscroll{};

    DECLARE_SCRIPT_REGISTER_FUNCTION(CUIStatic);
};
