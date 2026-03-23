////////////////////////////////////////////////////////////////////////////
// multiline_edit_control.h — multi-line text buffer for UI editing
////////////////////////////////////////////////////////////////////////////

#ifndef MULTILINE_EDIT_CONTROL_H_INCLUDED
#define MULTILINE_EDIT_CONTROL_H_INCLUDED

#include "xr_input.h"
#include "multiline_edit_actions.h"

namespace text_editor
{
enum ml_init_mode : u32
{
    ml_im_standart = 0,
    ml_im_read_only,

    ml_im_count
};

class ENGINE_API multiline_edit_control
{
    using ml_Base = ml_base;
    using Callback = fastdelegate::FastDelegate0<void>;

public:
    multiline_edit_control(size_t str_buffer_size);
    void init(size_t str_buffer_size, ml_init_mode mode = ml_im_standart);
    ~multiline_edit_control();

    void clear_states();

    void on_ir_capture();
    void on_ir_release();

    void on_key_press(int dik);
    void on_key_hold(int dik);
    void on_key_release(int dik);
    void on_text_input(const char* text);

    void on_frame();

    void assign_callback(int const dik, key_state state, Callback const& callback);
    void remove_callback(int dik);

    void insert_character(char c);

    bool get_key_state(key_state mask) const { return mask ? !!m_key_state.test(mask) : true; }
    void set_key_state(key_state mask, bool value) { m_key_state.set(mask, value); }
    bool cursor_view() const { return m_cursor_view; }
    bool need_update() const { return m_need_update; }
    pcstr str_edit() const { return m_edit_str; }
    pcstr str_before_cursor() const { return m_buf0; }
    pcstr str_before_mark() const { return m_buf1; }
    pcstr str_mark() const { return m_buf2; }
    pcstr str_after_mark() const { return m_buf3; }
    void set_edit(pcstr str);
    void set_cursor_pos(size_t pos);
    size_t cursor_pos() const { return m_cur_pos; }
    void set_selected_mode(bool status) { m_unselected_mode = !status; }
    bool get_selected_mode() const { return !m_unselected_mode; }

    bool char_is_allowed(char c);

private:
    void sync_preferred_col_from_cursor();

    multiline_edit_control(multiline_edit_control const&);
    multiline_edit_control const& operator=(multiline_edit_control const&);

    void update_key_states();
    void update_bufs();

    void undo_buf();
    void select_all_buf();
    void flip_insert_mode();

    void copy_to_clipboard();
    void paste_from_clipboard();
    void cut_to_clipboard();

    void move_pos_home();
    void move_pos_end();
    void move_pos_home_line();
    void move_pos_end_line();
    void on_home_key();
    void on_end_key();
    void move_pos_left();
    void move_pos_right();
    void move_pos_up();
    void move_pos_down();
    void move_pos_left_word();
    void move_pos_right_word();

    void delete_selected_back();
    void delete_selected_forward();
    void delete_word_back();
    void delete_word_forward();

    void insert_newline();
    void insert_tab_spaces();
    void SwitchKL();

    void create_key_state(int const dik, key_state state);

    void clear_inserted();
    bool empty_inserted() const;

    void add_inserted_text();

    void delete_selected(bool back);
    void compute_positions();
    void clamp_cur_pos();

private:
    ml_Base* m_actions[CInput::COUNT_KB_BUTTONS];

    char* m_edit_str;
    char* m_undo_buf;
    char* m_inserted;
    char* m_buf0;
    char* m_buf1;
    char* m_buf2;
    char* m_buf3;

    enum
    {
        MIN_BUF_SIZE = 8,
        MAX_BUF_SIZE = 4096
    };
    size_t m_buffer_size;

    size_t m_cur_pos;
    size_t m_inserted_pos;
    size_t m_select_start;
    size_t m_p1;
    size_t m_p2;

    size_t m_preferred_col;

    float m_accel;
    float m_cur_time;
    float m_rep_time;
    float m_last_key_time;
    u32 m_last_frame_time;
    u32 m_last_changed_frame;

    Flags32 m_key_state;
    ml_init_mode m_current_mode;

    bool m_hold_mode;
    bool m_insert_mode;
    bool m_repeat_mode;
    bool m_mark;
    bool m_cursor_view;
    bool m_need_update;
    bool m_unselected_mode;
};

} // namespace text_editor

#endif
