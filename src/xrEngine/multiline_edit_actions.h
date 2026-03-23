////////////////////////////////////////////////////////////////////////////
// multiline_edit_actions.h — key dispatch for multiline_edit_control
////////////////////////////////////////////////////////////////////////////

#ifndef MULTILINE_EDIT_ACTIONS_H_INCLUDED
#define MULTILINE_EDIT_ACTIONS_H_INCLUDED

#include "Common/Noncopyable.hpp"
#include "line_edit_control.h" // key_state

namespace text_editor
{
class multiline_edit_control;

class ml_base : private Noncopyable
{
public:
    ml_base();
    virtual ~ml_base();
    void on_assign(ml_base* const prev_action);
    virtual void on_key_press(multiline_edit_control* const control);

protected:
    ml_base* m_previous_action;
};

class ml_callback_base : public ml_base
{
private:
    typedef fastdelegate::FastDelegate0<void> Callback;

public:
    ml_callback_base(Callback const& callback, key_state state);
    virtual ~ml_callback_base();
    virtual void on_key_press(multiline_edit_control* const control);

protected:
    key_state m_run_state;
    Callback m_callback;
};

class ml_key_state_base : public ml_base
{
public:
    ml_key_state_base(key_state state, ml_base* type_pair);
    virtual ~ml_key_state_base();
    virtual void on_key_press(multiline_edit_control* const control);

private:
    key_state m_state;
    ml_base* m_type_pair;
};

} // namespace text_editor

#endif
