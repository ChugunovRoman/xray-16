#include "pch.hpp"
#include "UIMultiLineEdit.h"
#include "Windows/UIFrameLineWnd.h"
#include "ScrollBar/UIScrollBar.h"
#include "Lines/UILines.h"
#include "ui_base.h"
#include "xrEngine/IGameFont.hpp"
#include "xrEngine/multiline_edit_control.h"
#include "xrEngine/xr_input.h"
#include "xrCore/Text/LtxMultilineString.hpp"

#include <algorithm>

#ifdef SendMessage
#undef SendMessage
#endif

CUIMultiLineEdit::CUIMultiLineEdit() : CUIStatic("CUIMultiLineEdit"), m_frameLine(nullptr)
{
    m_editor_control = xr_new<text_editor::multiline_edit_control>(4096);

    TextItemControl()->SetVTextAlignment(valTop);
    TextItemControl()->SetTextAlignment(CGameFont::alLeft);
    TextItemControl()->SetTextComplexMode(true);
    TextItemControl()->SetUseNewLineMode(true);
    TextItemControl()->SetColoringMode(false);
    TextItemControl()->SetCutWordsMode(true);

    m_last_key_state_time = 0;
    m_bInputFocus = false;
    m_force_update = true;
    m_read_mode = false;
    m_next_focus_capturer = NULL;

    UI().Focus().RegisterFocusable(this);
}

CUIMultiLineEdit::~CUIMultiLineEdit()
{
    xr_delete(m_editor_control);
    UI().Focus().UnregisterFocusable(this);
}

text_editor::multiline_edit_control& CUIMultiLineEdit::ec()
{
    VERIFY(m_editor_control);
    return *m_editor_control;
}

text_editor::multiline_edit_control const& CUIMultiLineEdit::ec() const
{
    VERIFY(m_editor_control);
    return *m_editor_control;
}

void CUIMultiLineEdit::Init(u32 max_char_count, bool read_only)
{
    if (max_char_count == 0)
        max_char_count = 4096;

    if (read_only)
    {
        m_editor_control->init(max_char_count, text_editor::ml_im_read_only);
        m_editor_control->set_selected_mode(true);
        m_read_mode = true;
    }
    else
    {
        m_editor_control->init(max_char_count, text_editor::ml_im_standart);
        m_editor_control->set_selected_mode(false);
        m_read_mode = false;
    }

    Register_callbacks();
    ClearText();

    m_bInputFocus = false;
}

void CUIMultiLineEdit::InitCustomEdit(Fvector2 pos, Fvector2 size)
{
    if (m_frameLine)
    {
        m_frameLine->SetWndPos(Fvector2().set(0, 0));
        m_frameLine->SetWndSize(size);
    }
    inherited::SetWndPos(pos);
    inherited::SetWndSize(size);
}

bool CUIMultiLineEdit::InitTextureEx(pcstr texture, pcstr shader, bool fatal /*= true*/)
{
    if (!m_frameLine)
    {
        m_frameLine = xr_new<CUIFrameLineWnd>("Frameline");
        AttachChild(m_frameLine);
        m_frameLine->SetAutoDelete(true);
    }
    const bool result = m_frameLine->InitTextureEx(texture, shader, fatal);
    m_frameLine->SetWndPos(Fvector2().set(0, 0));
    m_frameLine->SetWndSize(GetWndSize());
    return result;
}

bool CUIMultiLineEdit::InitTexture(pcstr texture, bool fatal /*= true*/)
{
    return InitTextureEx(texture, "hud" DELIMITER "default", fatal);
}

void CUIMultiLineEdit::Register_callbacks()
{
    ec().assign_callback(SDL_SCANCODE_ESCAPE, text_editor::ks_free, Callback(this, &CUIMultiLineEdit::press_escape));
    ec().assign_callback(SDL_SCANCODE_TAB, text_editor::ks_free, Callback(this, &CUIMultiLineEdit::press_tab));
}

void CUIMultiLineEdit::SendMessage(CUIWindow* pWnd, s16 msg, void* pData)
{
    if (msg == WINDOW_KEYBOARD_CAPTURE_LOST && m_bInputFocus)
    {
        CaptureFocus(false);
        GetMessageTarget()->SendMessage(this, EDIT_TEXT_COMMIT, NULL);
    }
}

bool CUIMultiLineEdit::OnMouseAction(float x, float y, EUIMessages mouse_action)
{
    if (mouse_action == WINDOW_MOUSE_WHEEL_UP || mouse_action == WINDOW_MOUSE_WHEEL_DOWN)
    {
        EnsureVScrollBar();
        SyncScrollLayout(TextItemControl());
        if (m_vscroll && m_vscroll->IsShown())
        {
            if (mouse_action == WINDOW_MOUSE_WHEEL_UP)
                m_vscroll->TryScrollDec(true);
            else
                m_vscroll->TryScrollInc(true);
            return true;
        }
        return false;
    }

    if (mouse_action == WINDOW_LBUTTON_DB_CLICK && !m_bInputFocus)
        CaptureFocus(true);

    if (mouse_action == WINDOW_LBUTTON_DOWN && !m_bInputFocus)
        CaptureFocus(true);

    if (!m_read_mode && (mouse_action == WINDOW_LBUTTON_DOWN || mouse_action == WINDOW_LBUTTON_DB_CLICK))
    {
        CUILines* tc = TextItemControl();
        tc->SetText(ec().str_edit());
        SyncScrollLayout(tc);
        float vindent = TextVIndentForLayout(tc);
        size_t pos = 0;
        if (tc->CursorPosFromLocalPoint(x, y, pos, GetScrollPixels(), &vindent))
        {
            ec().set_cursor_pos(pos);
            m_force_update = true;
        }
    }

    return false;
}

bool CUIMultiLineEdit::OnKeyboardAction(int dik, EUIMessages keyboard_action)
{
    if (!m_bInputFocus)
        return false;

    if (keyboard_action == WINDOW_KEY_PRESSED)
    {
        if (dik == SDL_SCANCODE_RETURN || dik == SDL_SCANCODE_RETURN2 || dik == SDL_SCANCODE_KP_ENTER)
        {
            if (pInput->iGetAsyncKeyState(SDL_SCANCODE_LCTRL) || pInput->iGetAsyncKeyState(SDL_SCANCODE_RCTRL))
            {
                press_commit();
                return true;
            }
            ec().on_key_press(dik);
            return true;
        }
        if (dik == SDL_SCANCODE_UP || dik == SDL_SCANCODE_DOWN)
        {
            CUILines* tc = TextItemControl();
            tc->SetText(ec().str_edit());
            size_t cur = ec().cursor_pos();
            const int d = (dik == SDL_SCANCODE_UP) ? -1 : 1;
            if (tc->MoveCursorByVisualLine(cur, d))
            {
                ec().set_cursor_pos(cur);
                return true;
            }
        }
    }

    switch (keyboard_action)
    {
    case WINDOW_KEY_PRESSED:
        ec().on_key_press(dik);
        return true;

    case WINDOW_KEY_HOLD:
        ec().on_key_hold(dik);
        return true;

    case WINDOW_KEY_RELEASED:
        ec().on_key_release(dik);
        return true;
    }

    return false;
}

bool CUIMultiLineEdit::OnTextInput(pcstr text)
{
    if (!m_bInputFocus)
        return false;

    // Key path may be suppressed when IME/text-input is active; some platforms send CR only.
    if (text && text[0] == '\n' && text[1] == 0)
    {
        ec().on_key_press(SDL_SCANCODE_RETURN);
        return true;
    }
    if (text && text[0] == '\r' && (text[1] == 0 || (text[1] == '\n' && text[2] == 0)))
    {
        ec().on_key_press(SDL_SCANCODE_RETURN);
        return true;
    }

    ec().on_text_input(text);
    return true;
}

void CUIMultiLineEdit::Update()
{
    ec().on_frame();

    if (!ec().get_key_state(text_editor::ks_force))
        m_last_key_state_time = Device.dwTimeGlobal;

    ScrollToShowCursorIfNeeded();

    inherited::Update();
}

void CUIMultiLineEdit::DrawText()
{
    CUILines* tc = TextItemControl();
    if (!tc)
        return;

    SyncScrollLayout(tc);

    Fvector2 p;
    GetAbsolutePos(p);
    Frect clip;
    GetAbsoluteRect(clip);
    UI().PushScissor(clip);

    const float scroll = GetScrollPixels();
    tc->Draw(p.x, p.y - scroll);

    UI().PopScissor();
}

void CUIMultiLineEdit::Draw()
{
    Fvector2 pos, out;
    GetAbsolutePos(pos);
    CUILines* tc = TextItemControl();
    CGameFont* font = tc->GetFont();

    if (ec().need_update() || m_force_update)
    {
        tc->SetText(ec().str_edit());
        m_force_update = false;
    }

    inherited::Draw();

    if (m_vscroll && m_vscroll->IsShown())
        m_vscroll->Draw();

    if (m_bInputFocus && ec().cursor_view())
    {
        tc->SetText(ec().str_edit());
        SyncScrollLayout(tc);

        float x_off = 0.f;
        size_t line_index = 0;
        if (!tc->ComputeCursorPlacement(ec().cursor_pos(), x_off, line_index))
        {
            pcstr before = ec().str_before_cursor();
            pcstr last_nl = strrchr(before, '\n');
            pcstr cur_line_start = last_nl ? last_nl + 1 : before;

            x_off = font->SizeOf_(cur_line_start);

            line_index = 0;
            for (pcstr q = before; *q; ++q)
            {
                if (*q == '\n')
                    ++line_index;
            }
        }

        float line_h = font->CurrentHeight_();
        UI().ClientToScreenScaledHeight(line_h);

        const float scroll = GetScrollPixels();
        const float vindent = TextVIndentForLayout(tc);

        // Match CUICustomEdit::Draw: scale base position only, then add raw SizeOf_ (not scaled again).
        out.x = pos.x + tc->m_TextOffset.x + tc->GetIndentByAlign();
        out.y = pos.y + 2.0f + tc->m_TextOffset.y + vindent + line_index * line_h - scroll;
        UI().ClientToScreenScaled(out);

        out.x += x_off;

        Frect cursor_clip;
        GetAbsoluteRect(cursor_clip);
        UI().PushScissor(cursor_clip);

        font->SetColor(tc->GetTextColor());
        font->Out(out.x, out.y, "_");

        UI().PopScissor();
    }
    font->OnRender();
}

void CUIMultiLineEdit::EnsureVScrollBar()
{
    if (m_vscroll)
        return;

    m_vscroll = xr_new<CUIScrollBar>();
    m_vscroll->SetAutoDelete(true);
    m_vscroll->SetCustomDraw(true);
    AttachChild(m_vscroll);

    float h = GetWndSize().y;
    if (h < 1.f)
        h = 100.f;
    m_vscroll->InitScrollBar(Fvector2().set(GetWndSize().x, 0.f), h, false, "default");
    const Fvector2 sc_pos = { m_vscroll->GetWndPos().x - m_vscroll->GetWndSize().x, m_vscroll->GetWndPos().y };
    m_vscroll->SetWndPos(sc_pos);
    m_vscroll->SetStepSize(_max(1, iFloor(h / 10)));
    m_vscroll->SetPageSize(_max(1, iFloor(h)));
    m_vscroll->Show(false);
    m_vscroll->SetEnabled(false);
}

void CUIMultiLineEdit::LayoutVScrollBar()
{
    if (!m_vscroll)
        return;
    const float h = GetWndSize().y;
    const float w = GetWndSize().x;
    if (h < 1.f || w < 1.f)
        return;
    m_vscroll->SetHeight(h);
    m_vscroll->SetWndPos(Fvector2().set(w - m_vscroll->GetWidth(), 0.f));
}

void CUIMultiLineEdit::SyncScrollLayout(CUILines* tc)
{
    if (!tc)
        return;

    const float wnd_w = GetWndSize().x;
    const float wnd_h = GetWndSize().y;
    if (wnd_w <= 1.f || wnd_h <= 1.f)
    {
        tc->m_wndSize.set(wnd_w, wnd_h);
        return;
    }

    EnsureVScrollBar();
    LayoutVScrollBar();

    const float sw = m_vscroll->GetWidth();

    tc->m_wndSize.x = wnd_w;
    tc->m_wndSize.y = wnd_h;
    tc->ParseText(true);

    float content_h = tc->GetVisibleHeight();
    if (content_h <= wnd_h + 1.f)
    {
        tc->m_wndSize.x = wnd_w;
        tc->m_wndSize.y = wnd_h;
        tc->ParseText(true);
        m_vscroll->SetScrollPos(0);
        m_vscroll->Show(false);
        m_vscroll->SetEnabled(false);
        return;
    }

    tc->m_wndSize.x = wnd_w - sw;
    tc->m_wndSize.y = wnd_h;
    tc->ParseText(true);
    content_h = tc->GetVisibleHeight();

    const int max_pos = iCeil(content_h);
    const int page = std::max(1, iFloor(wnd_h));
    m_vscroll->SetRange(0, max_pos);
    m_vscroll->SetPageSize(page);
    m_vscroll->SetStepSize(std::max(1, page / 10));
    m_vscroll->SetEnabled(true);
    m_vscroll->Show(true);

    const int smax = std::max(0, max_pos - page + 1);
    if (m_vscroll->GetScrollPos() > smax)
        m_vscroll->SetScrollPos(smax);
}

float CUIMultiLineEdit::GetScrollPixels() const
{
    if (!m_vscroll || !m_vscroll->IsShown())
        return 0.f;
    return float(m_vscroll->GetScrollPos());
}

float CUIMultiLineEdit::TextVIndentForLayout(CUILines* tc) const
{
    if (!tc)
        return 0.f;
    if (m_vscroll && m_vscroll->IsShown())
        return 0.f;
    switch (tc->GetVTextAlignment())
    {
    case valTop: return 0.f;
    case valCenter: return (tc->m_wndSize.y - tc->GetVisibleHeight()) / 2;
    case valBottom: return tc->m_wndSize.y - tc->GetVisibleHeight();
    default: return 0.f;
    }
}

void CUIMultiLineEdit::ScrollToShowCursorIfNeeded()
{
    if (!m_bInputFocus)
        return;

    CUILines* tc = TextItemControl();
    tc->SetText(ec().str_edit());
    SyncScrollLayout(tc);
    if (!m_vscroll || !m_vscroll->IsShown())
        return;

    CGameFont* font = tc->GetFont();
    if (!font)
        return;

    float x_off = 0.f;
    size_t line_index = 0;
    if (!tc->ComputeCursorPlacement(ec().cursor_pos(), x_off, line_index))
    {
        pcstr before = ec().str_before_cursor();
        line_index = 0;
        for (pcstr q = before; *q; ++q)
        {
            if (*q == '\n')
                ++line_index;
        }
    }

    float line_h = font->CurrentHeight_();
    UI().ClientToScreenScaledHeight(line_h);

    const float cursor_top = float(line_index) * line_h;
    const float cursor_bot = cursor_top + line_h;
    const float view = GetWndSize().y;
    const int s = m_vscroll->GetScrollPos();

    if (cursor_top < s)
        m_vscroll->SetScrollPos(iFloor(cursor_top));
    else if (cursor_bot > s + view)
        m_vscroll->SetScrollPos(iFloor(cursor_bot - view));
}

void CUIMultiLineEdit::Show(bool status)
{
    m_force_update = true;
    inherited::Show(status);
}

void CUIMultiLineEdit::ClearText() { ec().set_edit(""); }
void CUIMultiLineEdit::SetText(pcstr str) { ec().set_edit(str); m_force_update = true; }
pcstr CUIMultiLineEdit::GetText() const { return ec().str_edit(); }

void CUIMultiLineEdit::SetTextFromLtxLine(pcstr ltx_line)
{
    xr_string t;
    ltx_multiline::UnescapeLtxLineToInternal(t, ltx_line);
    ec().set_edit(t.c_str());
    m_force_update = true;
}

pcstr CUIMultiLineEdit::GetTextAsLtxLine() const
{
    thread_local static xr_string s;
    ltx_multiline::EscapeInternalToLtxLine(s, ec().str_edit());
    return s.c_str();
}

void CUIMultiLineEdit::Enable(bool status)
{
    inherited::Enable(status);
    if (!status)
        GetMessageTarget()->SendMessage(this, WINDOW_KEYBOARD_CAPTURE_LOST);
}

void CUIMultiLineEdit::nothing() {}

void CUIMultiLineEdit::press_escape()
{
    if (xr_strlen(ec().str_edit()) != 0)
    {
        if (!m_read_mode)
            ec().set_edit("");
    }
    else
    {
        CaptureFocus(false);
        GetParent()->SetKeyboardCapture(this, false);
        GetMessageTarget()->SendMessage(this, EDIT_TEXT_CANCEL, NULL);
    }
}

void CUIMultiLineEdit::press_commit()
{
    CaptureFocus(false);
    GetParent()->SetKeyboardCapture(this, false);
    GetMessageTarget()->SendMessage(this, EDIT_TEXT_COMMIT, NULL);
}

void CUIMultiLineEdit::press_tab()
{
    if (!m_next_focus_capturer)
        return;

    CaptureFocus(false);
    GetParent()->SetKeyboardCapture(this, false);
    GetMessageTarget()->SendMessage(this, EDIT_TEXT_COMMIT, NULL);
    GetParent()->SetKeyboardCapture(m_next_focus_capturer, true);
    m_next_focus_capturer->CaptureFocus(true);
}

void CUIMultiLineEdit::CaptureFocus(bool bCapture)
{
    if (bCapture)
    {
        GetParent()->SetKeyboardCapture(this, true);
        ec().on_ir_capture();
    }
    else
    {
        ec().on_ir_release();
    }

    m_bInputFocus = bCapture;
}
