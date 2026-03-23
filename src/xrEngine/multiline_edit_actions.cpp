////////////////////////////////////////////////////////////////////////////
// multiline_edit_actions.cpp
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "multiline_edit_actions.h"
#include "multiline_edit_control.h"
#include "xr_input.h"

namespace text_editor
{
ml_base::ml_base() : m_previous_action(NULL) {}
ml_base::~ml_base() { xr_delete(m_previous_action); }
void ml_base::on_assign(ml_base* const prev_action) { m_previous_action = prev_action; }
void ml_base::on_key_press(multiline_edit_control* const control)
{
    if (m_previous_action)
        m_previous_action->on_key_press(control);
}

ml_callback_base::ml_callback_base(Callback const& callback, key_state state)
    : m_run_state(state), m_callback(callback) {}

ml_callback_base::~ml_callback_base() {}
void ml_callback_base::on_key_press(multiline_edit_control* const control)
{
    if (control->get_key_state(m_run_state))
    {
        m_callback();
        return;
    }
    ml_base::on_key_press(control);
}

ml_key_state_base::ml_key_state_base(key_state state, ml_base* type_pair) : m_state(state), m_type_pair(type_pair) {}
ml_key_state_base::~ml_key_state_base() { xr_delete(m_type_pair); }
void ml_key_state_base::on_key_press(multiline_edit_control* const control)
{
    control->set_key_state(m_state, true);
    if (m_type_pair)
        m_type_pair->on_key_press(control);
}

} // namespace text_editor
