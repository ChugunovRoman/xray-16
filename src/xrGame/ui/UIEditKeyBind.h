#pragma once

#include "xrUICore/Static/UIStatic.h"
#include "xrUICore/Options/UIOptionsItem.h"

struct game_action;
struct keyboard_key;
class CUIColorAnimatorWrapper;

class CUIEditKeyBind final : public CUIStatic, public CUIOptionsItem
{
    bool m_primary;
    bool m_isGamepadBinds;
    game_action* m_action;
    // An addon key action ("<group>.<id>", AddonKeyRegister in xr_level_controller.h) instead of an engine action:
    // m_action stays nullptr, the key goes to AddonKeySet, not to the console command bind
    shared_str m_addon_action;
    keyboard_key* m_keyboard;
    keyboard_key* m_opt_backup_value;
    // The player cleared the cell (Delete, ClearKey): "Accept" runs unbind / unbind_sec / unbind_gpad. A cell emptied
    // by OnMessage (its key was given to another row) keeps it false - the engine binding stays as it was
    bool m_cleared{};

public:
    CUIEditKeyBind(bool primary, bool isGamepadBinds = false);
    ~CUIEditKeyBind();

    // options item
    virtual void AssignProps(const shared_str& entry, const shared_str& group);

    virtual void SetCurrentOptValue(); // opt->current
    virtual void SaveBackUpOptValue(); // current->backup
    virtual void SaveOptValue(); // current->opt
    virtual void UndoOptValue(); // backup->current
    virtual bool IsChangedOptValue() const; // backup!=current

    void SetValue();
    virtual void OnMessage(LPCSTR message);

    // CUIWindow methods
    void InitKeyBind(Fvector2 pos, Fvector2 size);
    virtual void Update();
    virtual bool OnMouseDown(int mouse_btn);
    virtual void OnFocusLost();
    virtual bool OnKeyboardAction(int dik, EUIMessages keyboard_action);
    bool OnControllerAction(int axis, const ControllerAxisState& state, EUIMessages controller_action) override;

    virtual void SetText(LPCSTR text);
    void SetEditMode(bool b);

    pcstr GetDebugType() override { return "CUIEditKeyBind"; }

protected:
    void BindAction2Key();
    // The name in the messages "<action>=<key>" of the group key_binding: an engine action or an addon key action
    pcstr ActionName() const;
    // Delete: the cell without a key, out of the wait for a key. The key Delete itself is never bound from the menu
    void ClearKey();
    // The cursor is over this cell and over the visible part of its scroll list (a row scrolled out is not "hovered")
    bool IsCursorOverVisibleCell() const;

    bool m_isEditMode;
};
