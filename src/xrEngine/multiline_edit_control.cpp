////////////////////////////////////////////////////////////////////////////
// multiline_edit_control.cpp
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "multiline_edit_control.h"

#include "xrCore/os_clipboard.h"
#include "xrCore/buffer_vector.h"
#include "xrCore/Text/StringConversion.hpp"
#include "Common/object_broker.h"
#include "xr_input.h"

#include <SDL.h>

#include <locale>

extern ENGINE_API float g_console_sensitive;

namespace text_editor
{
namespace
{
static bool terminate_char_ml(char c, bool check_space = false)
{
    switch (c)
    {
    case '\n':
    case '\r': return true;
    case ' ': return check_space;
    case '(':
    case ')':
    case '{':
    case '}':
    case '[':
    case ']':
    case '<':
    case '>':
    case '\'':
    case '\"':
    case '=':
    case '+':
    case '-':
    case '*':
    case '\\':
    case '/':
    case '&':
    case '|':
    case '!':
    case '@':
    case '#':
    case '~':
    case '`':
    case '$':
    case '%':
    case '^':
    case ':':
    case ';':
    case '?':
    case ',':
    case '.':
    case '_': return true;
    }
    return false;
}

static size_t line_start_pos(pcstr s, size_t pos)
{
    while (pos > 0 && s[pos - 1] != '\n')
        --pos;
    return pos;
}

static size_t line_end_pos(pcstr s, size_t len, size_t pos)
{
    while (pos < len && s[pos] != '\n')
        ++pos;
    return pos;
}
} // namespace

multiline_edit_control::multiline_edit_control(size_t str_buffer_size)
{
    m_edit_str = nullptr;
    m_inserted = nullptr;
    m_undo_buf = nullptr;
    m_buf0 = nullptr;
    m_buf1 = nullptr;
    m_buf2 = nullptr;
    m_buf3 = nullptr;

    for (auto& action : m_actions)
        action = nullptr;

    init(str_buffer_size);

    update_key_states();
}

multiline_edit_control::~multiline_edit_control()
{
    xr_free(m_edit_str);
    xr_free(m_inserted);
    xr_free(m_undo_buf);
    xr_free(m_buf0);
    xr_free(m_buf1);
    xr_free(m_buf2);
    xr_free(m_buf3);

    size_t const array_size = sizeof(m_actions) / sizeof(m_actions[0]);
    buffer_vector<ml_Base*> actions(m_actions, array_size, &m_actions[0], &m_actions[0] + array_size);
    std::sort(actions.begin(), actions.end());
    actions.erase(std::unique(actions.begin(), actions.end()), actions.end());
    delete_data(actions);
}

void multiline_edit_control::update_key_states()
{
    m_key_state.zero();

    set_key_state(ks_LShift, pInput->iGetAsyncKeyState(SDL_SCANCODE_LSHIFT));
    set_key_state(ks_RShift, pInput->iGetAsyncKeyState(SDL_SCANCODE_RSHIFT));
    set_key_state(ks_LCtrl, pInput->iGetAsyncKeyState(SDL_SCANCODE_LCTRL));
    set_key_state(ks_RCtrl, pInput->iGetAsyncKeyState(SDL_SCANCODE_RCTRL));
    set_key_state(ks_LAlt, pInput->iGetAsyncKeyState(SDL_SCANCODE_LALT));
    set_key_state(ks_RAlt, pInput->iGetAsyncKeyState(SDL_SCANCODE_RALT));
    set_key_state(ks_CapsLock, SDL_GetModState() & KMOD_CAPS);
}

void multiline_edit_control::clear_states()
{
    m_edit_str[0] = 0;
    clear_inserted();
    m_undo_buf[0] = 0;

    m_buf0[0] = 0;
    m_buf1[0] = 0;
    m_buf2[0] = 0;
    m_buf3[0] = 0;

    m_cur_pos = 0;
    m_inserted_pos = 0;
    m_select_start = 0;
    m_p1 = 0;
    m_p2 = 0;

    m_preferred_col = 0;

    m_accel = 1.0f;
    m_cur_time = 0.0f;
    m_rep_time = 0.0f;
    m_last_frame_time = 0;
    m_last_key_time = 0.0f;
    m_last_changed_frame = 0;

    m_hold_mode = false;
    m_insert_mode = false;
    m_repeat_mode = false;
    m_mark = false;
    m_cursor_view = false;
    m_need_update = false;
    m_unselected_mode = false;

    update_key_states();
}

void multiline_edit_control::on_ir_capture() { pInput->EnableTextInput(); }
void multiline_edit_control::on_ir_release() { pInput->DisableTextInput(); }

void multiline_edit_control::init(size_t str_buffer_size, ml_init_mode mode)
{
    m_buffer_size = str_buffer_size;
    clamp<size_t>(m_buffer_size, MIN_BUF_SIZE, MAX_BUF_SIZE);

    xr_free(m_edit_str);
    m_edit_str = (pstr)xr_malloc(m_buffer_size * sizeof(char));
    xr_free(m_inserted);
    m_inserted = (pstr)xr_malloc(m_buffer_size * sizeof(char));
    xr_free(m_undo_buf);
    m_undo_buf = (pstr)xr_malloc(m_buffer_size * sizeof(char));

    xr_free(m_buf0);
    m_buf0 = (pstr)xr_malloc(m_buffer_size * sizeof(char));
    xr_free(m_buf1);
    m_buf1 = (pstr)xr_malloc(m_buffer_size * sizeof(char));
    xr_free(m_buf2);
    m_buf2 = (pstr)xr_malloc(m_buffer_size * sizeof(char));
    xr_free(m_buf3);
    m_buf3 = (pstr)xr_malloc(m_buffer_size * sizeof(char));

    clear_states();

    for (auto& action : m_actions)
        xr_delete(action);

    m_current_mode = mode;
    if (mode == ml_im_read_only)
    {
        assign_callback(SDL_SCANCODE_A, ks_Ctrl, Callback(this, &multiline_edit_control::select_all_buf));
        assign_callback(SDL_SCANCODE_C, ks_Ctrl, Callback(this, &multiline_edit_control::copy_to_clipboard));
        assign_callback(SDL_SCANCODE_INSERT, ks_Ctrl, Callback(this, &multiline_edit_control::copy_to_clipboard));

        assign_callback(SDL_SCANCODE_HOME, ks_free, Callback(this, &multiline_edit_control::on_home_key));
        assign_callback(SDL_SCANCODE_END, ks_free, Callback(this, &multiline_edit_control::on_end_key));
        assign_callback(SDL_SCANCODE_LEFT, ks_free, Callback(this, &multiline_edit_control::move_pos_left));
        assign_callback(SDL_SCANCODE_RIGHT, ks_free, Callback(this, &multiline_edit_control::move_pos_right));
        assign_callback(SDL_SCANCODE_UP, ks_free, Callback(this, &multiline_edit_control::move_pos_up));
        assign_callback(SDL_SCANCODE_DOWN, ks_free, Callback(this, &multiline_edit_control::move_pos_down));
        assign_callback(SDL_SCANCODE_LEFT, ks_Ctrl, Callback(this, &multiline_edit_control::move_pos_left_word));
        assign_callback(SDL_SCANCODE_RIGHT, ks_Ctrl, Callback(this, &multiline_edit_control::move_pos_right_word));
    }
    else
    {
        assign_callback(SDL_SCANCODE_INSERT, ks_free, Callback(this, &multiline_edit_control::flip_insert_mode));
        assign_callback(SDL_SCANCODE_A, ks_Ctrl, Callback(this, &multiline_edit_control::select_all_buf));
        assign_callback(SDL_SCANCODE_Z, ks_Ctrl, Callback(this, &multiline_edit_control::undo_buf));

        assign_callback(SDL_SCANCODE_C, ks_Ctrl, Callback(this, &multiline_edit_control::copy_to_clipboard));
        assign_callback(SDL_SCANCODE_V, ks_Ctrl, Callback(this, &multiline_edit_control::paste_from_clipboard));
        assign_callback(SDL_SCANCODE_X, ks_Ctrl, Callback(this, &multiline_edit_control::cut_to_clipboard));

        assign_callback(SDL_SCANCODE_INSERT, ks_Ctrl, Callback(this, &multiline_edit_control::copy_to_clipboard));
        assign_callback(SDL_SCANCODE_INSERT, ks_Shift, Callback(this, &multiline_edit_control::paste_from_clipboard));
        assign_callback(SDL_SCANCODE_DELETE, ks_Shift, Callback(this, &multiline_edit_control::cut_to_clipboard));

        assign_callback(SDL_SCANCODE_HOME, ks_free, Callback(this, &multiline_edit_control::on_home_key));
        assign_callback(SDL_SCANCODE_END, ks_free, Callback(this, &multiline_edit_control::on_end_key));
        assign_callback(SDL_SCANCODE_LEFT, ks_free, Callback(this, &multiline_edit_control::move_pos_left));
        assign_callback(SDL_SCANCODE_RIGHT, ks_free, Callback(this, &multiline_edit_control::move_pos_right));
        assign_callback(SDL_SCANCODE_UP, ks_free, Callback(this, &multiline_edit_control::move_pos_up));
        assign_callback(SDL_SCANCODE_DOWN, ks_free, Callback(this, &multiline_edit_control::move_pos_down));
        assign_callback(SDL_SCANCODE_LEFT, ks_Ctrl, Callback(this, &multiline_edit_control::move_pos_left_word));
        assign_callback(SDL_SCANCODE_RIGHT, ks_Ctrl, Callback(this, &multiline_edit_control::move_pos_right_word));

        assign_callback(SDL_SCANCODE_BACKSPACE, ks_free, Callback(this, &multiline_edit_control::delete_selected_back));
        assign_callback(SDL_SCANCODE_DELETE, ks_free, Callback(this, &multiline_edit_control::delete_selected_forward));
        assign_callback(SDL_SCANCODE_BACKSPACE, ks_Ctrl, Callback(this, &multiline_edit_control::delete_word_back));
        assign_callback(SDL_SCANCODE_DELETE, ks_Ctrl, Callback(this, &multiline_edit_control::delete_word_forward));

        assign_callback(SDL_SCANCODE_RETURN, ks_free, Callback(this, &multiline_edit_control::insert_newline));
        assign_callback(SDL_SCANCODE_RETURN2, ks_free, Callback(this, &multiline_edit_control::insert_newline));
        assign_callback(SDL_SCANCODE_KP_ENTER, ks_free, Callback(this, &multiline_edit_control::insert_newline));
        assign_callback(SDL_SCANCODE_TAB, ks_free, Callback(this, &multiline_edit_control::insert_tab_spaces));

        assign_callback(SDL_SCANCODE_LSHIFT, ks_Ctrl, Callback(this, &multiline_edit_control::SwitchKL));
        assign_callback(SDL_SCANCODE_LSHIFT, ks_Alt, Callback(this, &multiline_edit_control::SwitchKL));
    }

    create_key_state(SDL_SCANCODE_LSHIFT, ks_LShift);
    create_key_state(SDL_SCANCODE_RSHIFT, ks_RShift);
    create_key_state(SDL_SCANCODE_LCTRL, ks_LCtrl);
    create_key_state(SDL_SCANCODE_RCTRL, ks_RCtrl);
    create_key_state(SDL_SCANCODE_LALT, ks_LAlt);
    create_key_state(SDL_SCANCODE_RALT, ks_RAlt);
}

void multiline_edit_control::create_key_state(int const dik, key_state state)
{
    ml_Base* prev = m_actions[dik];
    m_actions[dik] = xr_new<text_editor::ml_key_state_base>(state, prev);
}

void multiline_edit_control::assign_callback(int const dik, key_state state, Callback const& callback)
{
    VERIFY(dik < CInput::COUNT_KB_BUTTONS);
    ml_Base* prev_action = m_actions[dik];
    m_actions[dik] = xr_new<text_editor::ml_callback_base>(callback, state);
    m_actions[dik]->on_assign(prev_action);
}

void multiline_edit_control::remove_callback(int dik)
{
    VERIFY(dik < CInput::COUNT_KB_BUTTONS);
    if (dik < CInput::COUNT_KB_BUTTONS)
        xr_delete(m_actions[dik]);
}

void multiline_edit_control::insert_character(char c)
{
    VERIFY(m_inserted_pos < (m_buffer_size - 1 /*trailing zero*/));
    m_inserted[m_inserted_pos] = c;
    m_inserted[m_inserted_pos + 1] = 0;
    m_inserted_pos++;
}

void multiline_edit_control::clear_inserted() { m_inserted[0] = m_inserted[1] = 0; m_inserted_pos = 0; }
bool multiline_edit_control::empty_inserted() const { return m_inserted_pos == 0; }

void multiline_edit_control::set_edit(pcstr str)
{
    size_t str_size = xr_strlen(str);
    clamp<size_t>(str_size, 0, m_buffer_size - 1);
    strncpy_s(m_edit_str, m_buffer_size, str, str_size);
    m_edit_str[str_size] = 0;

    m_cur_pos = str_size;
    m_select_start = m_cur_pos;
    m_accel = 1.0f;
    sync_preferred_col_from_cursor();
    update_bufs();
}

void multiline_edit_control::set_cursor_pos(size_t pos)
{
    m_cur_pos = pos;
    clamp_cur_pos();
    m_select_start = m_cur_pos;
    sync_preferred_col_from_cursor();
    compute_positions();
    update_bufs();
}

void multiline_edit_control::sync_preferred_col_from_cursor()
{
    const size_t ls = line_start_pos(m_edit_str, m_cur_pos);
    m_preferred_col = m_cur_pos - ls;
}

bool multiline_edit_control::char_is_allowed(char c)
{
    switch (c)
    {
    case '\r': return true; // normalized to \n in on_text_input
    default: return true;
    }
}

void multiline_edit_control::on_key_press(int dik)
{
    if (CInput::COUNT_KB_BUTTONS <= dik)
        return;
    if (!m_hold_mode)
    {
        m_last_key_time = 0.0f;
        m_accel = 1.0f;
    }
    m_mark = true;

    clamp_cur_pos();
    clear_inserted();
    compute_positions();

    if (m_actions[dik])
        m_actions[dik]->on_key_press(this);

    if (dik == SDL_SCANCODE_LCTRL || dik == SDL_SCANCODE_RCTRL)
        m_mark = false;

    m_edit_str[m_buffer_size - 1] = 0;
    clamp_cur_pos();

    add_inserted_text();
    if (m_mark && (!get_key_state(ks_Shift) || !empty_inserted()))
        m_select_start = m_cur_pos;
    compute_positions();

    m_repeat_mode = false;
    m_rep_time = 0.0f;

    update_key_states();
    update_bufs();
}

void multiline_edit_control::on_text_input(const char* text)
{
    clamp_cur_pos();
    clear_inserted();
    compute_positions();

    static std::locale locale("");
    const auto str = StringFromUTF8(text, locale);

    for (char c : str)
    {
        if (c == '\r')
            c = '\n';
        if (char_is_allowed(c))
            insert_character(c);
    }
    add_inserted_text();

    m_edit_str[m_buffer_size - 1] = 0;
    m_select_start = m_cur_pos;

    update_bufs();
}

void multiline_edit_control::on_key_hold(int dik)
{
    update_key_states();
    update_bufs();
    switch (dik)
    {
    case SDL_SCANCODE_TAB:
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT:
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:
    case SDL_SCANCODE_LALT:
    case SDL_SCANCODE_RALT: return;
    }

    if (m_repeat_mode && m_last_key_time > 5.0f * g_console_sensitive)
    {
        float buf_time = m_rep_time;
        m_hold_mode = true;

        on_key_press(dik);

        m_hold_mode = false;
        m_rep_time = buf_time;
    }
}

void multiline_edit_control::on_key_release(int dik)
{
    m_accel = 1.0f;
    m_rep_time = 0.0f;
    m_last_key_time = 0.0f;

    update_key_states();
    update_bufs();
}

void multiline_edit_control::on_frame()
{
    update_key_states();

    const auto fr_time = Device.dwTimeContinual;
    float dt = (fr_time - m_last_frame_time) * 0.001f;
    if (dt > 0.06666f)
        dt = 0.06666f;
    m_last_frame_time = fr_time;
    m_cur_time += dt;

    m_cursor_view = true;
    if (m_cur_time > 0.3f)
        m_cursor_view = false;
    if (m_cur_time > 0.4f)
        m_cur_time = 0.0f;

    m_rep_time += dt * m_accel;
    if (m_rep_time > g_console_sensitive)
    {
        m_rep_time = 0.0f;
        m_repeat_mode = true;
        m_accel += 0.2f;
    }
    m_last_key_time += dt;

    if (m_last_changed_frame + 1 < Device.dwFrame)
        m_need_update = false;
}

void multiline_edit_control::update_bufs()
{
    m_buf0[0] = 0;
    m_buf1[0] = 0;
    m_buf2[0] = 0;
    m_buf3[0] = 0;

    const size_t edit_size = xr_strlen(m_edit_str);
    const u8 ds = (m_cursor_view && m_insert_mode && m_p2 < edit_size) ? 1 : 0;
    strncpy_s(m_buf0, m_buffer_size, m_edit_str, m_cur_pos);
    strncpy_s(m_buf1, m_buffer_size, m_edit_str, m_p1);
    strncpy_s(m_buf2, m_buffer_size, m_edit_str + m_p1, m_p2 - m_p1 + ds);
    strncpy_s(m_buf3, m_buffer_size, m_edit_str + m_p2 + ds, edit_size - m_p2 - ds);

    m_need_update = true;
    m_last_changed_frame = Device.dwFrame;
}

void multiline_edit_control::add_inserted_text()
{
    if (empty_inserted())
        return;

    const size_t old_edit_size = xr_strlen(m_edit_str);

    auto buf = (pstr)xr_alloca((m_buffer_size + 1) * sizeof(char));

    strncpy_s(buf, m_buffer_size, m_edit_str, m_p1);
    strncpy_s(m_undo_buf, m_buffer_size, m_edit_str + m_p1, m_p2 - m_p1);

    size_t new_size = xr_strlen(m_inserted);
    if (m_buffer_size - 1 < m_p1 + new_size)
    {
        m_inserted[m_buffer_size - 1 - m_p1] = 0;
        new_size = xr_strlen(m_inserted);
    }
    strncpy_s(buf + m_p1, m_buffer_size - m_p1, m_inserted, _min(new_size, m_buffer_size - m_p1));

    const u8 ds = (m_insert_mode && m_p2 < old_edit_size) ? 1 : 0;
    strncpy_s(buf + m_p1 + new_size, m_buffer_size - (m_p1 + new_size), m_edit_str + m_p2 + ds,
        _min(old_edit_size - m_p2 - ds, m_buffer_size - m_p1 - new_size));
    buf[m_buffer_size] = 0;

    const size_t szn = m_p1 + new_size + old_edit_size - m_p2 - ds;
    if (szn < m_buffer_size)
    {
        strncpy_s(m_edit_str, m_buffer_size, buf, szn);
        m_edit_str[m_buffer_size - 1] = 0;
        m_cur_pos = m_p1 + new_size;
    }
    clamp_cur_pos();
    sync_preferred_col_from_cursor();
}

void multiline_edit_control::copy_to_clipboard()
{
    if (m_p1 >= m_p2)
        return;
    const size_t edit_len = xr_strlen(m_edit_str);
    auto buf = (pstr)xr_alloca((edit_len + 1) * sizeof(char));
    strncpy_s(buf, edit_len + 1, m_edit_str + m_p1, m_p2 - m_p1);
    buf[edit_len] = 0;
    os_clipboard::copy_to_clipboard(buf);
    m_mark = false;
}

void multiline_edit_control::paste_from_clipboard()
{
    os_clipboard::paste_from_clipboard(m_inserted, m_buffer_size - 1);
    m_inserted_pos = xr_strlen(m_inserted);
}

void multiline_edit_control::cut_to_clipboard()
{
    copy_to_clipboard();
    delete_selected_forward();
}

void multiline_edit_control::undo_buf()
{
    xr_strcpy(m_inserted, m_buffer_size, m_undo_buf);
    m_undo_buf[0] = 0;
}

void multiline_edit_control::select_all_buf()
{
    m_select_start = 0;
    m_cur_pos = xr_strlen(m_edit_str);
    m_mark = false;
}

void multiline_edit_control::flip_insert_mode() { m_insert_mode = !m_insert_mode; }
void multiline_edit_control::delete_selected_back() { delete_selected(true); }
void multiline_edit_control::delete_selected_forward() { delete_selected(false); }

void multiline_edit_control::delete_selected(bool back)
{
    clamp_cur_pos();
    const size_t edit_len = xr_strlen(m_edit_str);
    if (edit_len > 0)
    {
        if (back)
        {
            u8 dp = ((m_p1 == m_p2) && m_p1 > 0) ? 1 : 0;
            strncpy_s(m_undo_buf, m_buffer_size, m_edit_str + m_p1 - dp, m_p2 - m_p1 + dp);
            strncpy_s(m_edit_str + m_p1 - dp, m_buffer_size - (m_p1 - dp), m_edit_str + m_p2, edit_len - m_p2);
            m_cur_pos = m_p1 - dp;
        }
        else
        {
            u8 dn = ((m_p1 == m_p2) && m_p2 < edit_len) ? 1 : 0;
            strncpy_s(m_undo_buf, m_buffer_size, m_edit_str + m_p1, m_p2 - m_p1 + dn);
            strncpy_s(m_edit_str + m_p1, m_buffer_size - m_p1, m_edit_str + m_p2 + dn, edit_len - m_p2 - dn);
            m_cur_pos = m_p1;
        }
        clamp_cur_pos();
    }
    m_select_start = m_cur_pos;
    sync_preferred_col_from_cursor();
}

void multiline_edit_control::delete_word_back()
{
    bool const left_shift = get_key_state(ks_LShift);
    bool const right_shift = get_key_state(ks_RShift);
    set_key_state(ks_Shift, true);

    move_pos_left_word();
    compute_positions();
    delete_selected(true);

    set_key_state(ks_LShift, left_shift);
    set_key_state(ks_RShift, right_shift);
}

void multiline_edit_control::delete_word_forward()
{
    set_key_state(ks_Shift, true);
    move_pos_right_word();
    compute_positions();
    delete_selected(false);
    set_key_state(ks_Shift, false);
}

void multiline_edit_control::move_pos_home() { m_cur_pos = 0; sync_preferred_col_from_cursor(); }

void multiline_edit_control::move_pos_end()
{
    m_cur_pos = xr_strlen(m_edit_str);
    sync_preferred_col_from_cursor();
}

void multiline_edit_control::move_pos_home_line()
{
    m_cur_pos = line_start_pos(m_edit_str, m_cur_pos);
    sync_preferred_col_from_cursor();
}

void multiline_edit_control::on_home_key()
{
    if (get_key_state(ks_Ctrl))
        move_pos_home();
    else
        move_pos_home_line();
}

void multiline_edit_control::on_end_key()
{
    if (get_key_state(ks_Ctrl))
        move_pos_end();
    else
        move_pos_end_line();
}

void multiline_edit_control::move_pos_end_line()
{
    const size_t len = xr_strlen(m_edit_str);
    const size_t ls = line_start_pos(m_edit_str, m_cur_pos);
    m_cur_pos = line_end_pos(m_edit_str, len, ls);
    sync_preferred_col_from_cursor();
}

void multiline_edit_control::move_pos_left()
{
    if (m_cur_pos > 0)
        --m_cur_pos;
    sync_preferred_col_from_cursor();
}

void multiline_edit_control::move_pos_right()
{
    ++m_cur_pos;
    clamp_cur_pos();
    sync_preferred_col_from_cursor();
}

void multiline_edit_control::move_pos_up()
{
    const size_t ls = line_start_pos(m_edit_str, m_cur_pos);
    if (ls == 0)
        return;

    const size_t prev_line_end = ls - 1;
    const size_t prev_ls = line_start_pos(m_edit_str, prev_line_end);
    const size_t prev_len = prev_line_end - prev_ls;
    size_t col = m_preferred_col;
    if (col > prev_len)
        col = prev_len;
    m_cur_pos = prev_ls + col;
    clamp_cur_pos();
}

void multiline_edit_control::move_pos_down()
{
    const size_t len = xr_strlen(m_edit_str);
    const size_t ls = line_start_pos(m_edit_str, m_cur_pos);
    const size_t le = line_end_pos(m_edit_str, len, ls);
    if (le >= len)
        return;

    const size_t next_ls = le + 1;
    const size_t next_le = line_end_pos(m_edit_str, len, next_ls);
    const size_t next_len = next_le - next_ls;
    size_t col = m_preferred_col;
    if (col > next_len)
        col = next_len;
    m_cur_pos = next_ls + col;
    clamp_cur_pos();
}

void multiline_edit_control::move_pos_left_word()
{
    size_t i = m_cur_pos > 0 ? m_cur_pos - 1 : 0;

    while (i > 0 && m_edit_str[i] == ' ')
        --i;

    if (i > 0 && !terminate_char_ml(m_edit_str[i]))
    {
        while (i > 0 && !terminate_char_ml(m_edit_str[i], true))
            --i;

        if (i > 0)
            ++i;
    }

    m_cur_pos = i;
    sync_preferred_col_from_cursor();
}

void multiline_edit_control::move_pos_right_word()
{
    const size_t edit_len = xr_strlen(m_edit_str);
    size_t i = m_cur_pos + 1;

    while (i < edit_len && !terminate_char_ml(m_edit_str[i], true))
        ++i;

    while (i < edit_len && m_edit_str[i] == ' ')
        ++i;

    m_cur_pos = i;
    sync_preferred_col_from_cursor();
}

void multiline_edit_control::insert_newline()
{
    // Ctrl+Enter (e.g. commit) is handled in CUIMultiLineEdit / CUICustomEdit before on_key_press.
    insert_character('\n');
}

void multiline_edit_control::insert_tab_spaces()
{
    for (int k = 0; k < 4; ++k)
        insert_character(' ');
}

void multiline_edit_control::compute_positions()
{
    m_p1 = m_cur_pos;
    m_p2 = m_cur_pos;

    if (m_unselected_mode)
        return;

    if (m_cur_pos > m_select_start)
        m_p1 = m_select_start;

    else if (m_cur_pos < m_select_start)
        m_p2 = m_select_start;
}

void multiline_edit_control::clamp_cur_pos() { clamp<size_t>(m_cur_pos, 0, xr_strlen(m_edit_str)); }

void multiline_edit_control::SwitchKL()
{
    cpcstr hint = SDL_GetHint(SDL_HINT_GRAB_KEYBOARD);
    if (!hint || 0 != xr_strcmp("1", hint))
        return;
#ifdef XR_PLATFORM_WINDOWS
    if (pInput->IsExclusiveMode())
        ActivateKeyboardLayout((HKL)HKL_NEXT, 0);
#endif
}

} // namespace text_editor
