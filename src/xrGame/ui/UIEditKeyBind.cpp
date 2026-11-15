#include "StdAfx.h"

#include "UIEditKeyBind.h"
#include "xrEngine/xr_level_controller.h"
#include "Common/object_broker.h"
#include "xrEngine/XR_IOConsole.h"
#include "xrUICore/XML/UITextureMaster.h"
#include "xrUICore/ScrollView/UIScrollView.h"
#include "xrUICore/Cursor/UICursor.h"

CUIEditKeyBind::CUIEditKeyBind(bool primary, bool isGamepadBinds /*= false*/)
    : CUIStatic("CUIEditKeyBind")
{
    m_primary = primary;
    m_isGamepadBinds = isGamepadBinds;
    m_isEditMode = false;
    TextItemControl()->SetTextComplexMode(false);
    m_keyboard = NULL;
    m_opt_backup_value = NULL;
    m_action = NULL;
    UI().Focus().RegisterFocusable(this);
}

CUIEditKeyBind::~CUIEditKeyBind()
{
    UI().Focus().UnregisterFocusable(this);
}

u32 CutStringByLength(CGameFont* font, LPCSTR src, pstr dst, u32 dst_size, float length)
{
    if (font->IsMultibyte())
    {
        u16 nPos = font->GetCutLengthPos(length, src);
        VERIFY(nPos < dst_size);
        strncpy_s(dst, dst_size, src, nPos);
        dst[nPos] = '\0';
        return nPos;
    }
    else
    {
        float text_len = font->SizeOf_(src);
        UI().ClientToScreenScaledWidth(text_len);
        VERIFY(xr_strlen(src) <= dst_size);
        xr_strcpy(dst, dst_size, src);

        while (text_len > length)
        {
            dst[xr_strlen(dst) - 1] = 0;
            VERIFY(xr_strlen(dst));
            text_len = font->SizeOf_(dst);
            UI().ClientToScreenScaledWidth(text_len);
        }

        return xr_strlen(dst);
    }
}

void CUIEditKeyBind::SetText(const char* text)
{
    if (!text || 0 == xr_strlen(text))
        TextItemControl()->SetText("---");
    else
    {
        string256 buff;

        CutStringByLength(TextItemControl()->GetFont(), text, buff, sizeof(buff), GetWidth());

        TextItemControl()->SetText(buff);
    }
}

void CUIEditKeyBind::InitKeyBind(Fvector2 pos, Fvector2 size)
{
    CUIStatic::SetWndPos(pos);
    CUIStatic::SetWndSize(size);

    if (CUITextureMaster::ItemExist("ui_listline2")) // cop
        InitTexture("ui_listline2");
    else if (CUITextureMaster::ItemExist("ui_options_string")) // soc
        InitTexture("ui_options_string");
    else if (CUITextureMaster::ItemExist("ui_options_string_back")) // cs (exists in soc also, that's why it's last)
        InitTexture("ui_options_string_back");

    TextItemControl()->SetFont(UI().Font().pFontLetterica16Russian);
    SetStretchTexture(true);
    SetEditMode(false);
}

void CUIEditKeyBind::OnFocusLost()
{
    CUIStatic::OnFocusLost();
    SetEditMode(false);
    TextItemControl()->SetTextColor((subst_alpha(TextItemControl()->GetTextColor(), color_get_A(0xffffffff))));
}

bool CUIEditKeyBind::OnMouseDown(int mouse_btn)
{
    if (m_isEditMode)
    {
        // "<action>=<key>": an addon key action ("<group>.<id>", AddonKeyRegister) is up to 129 chars - not string64
        string256 message;

        m_keyboard = DikToPtr(mouse_btn, true);
        if (!m_keyboard)
            return true;

        m_cleared = false;
        SetValue();
        OnFocusLost();

        xr_strcpy(message, ActionName());
        xr_strcat(message, "=");
        xr_strcat(message, m_keyboard->key_name);
        SendMessage2Group("key_binding", message);

        return true;
    }

    if (mouse_btn == MOUSE_1)
        SetEditMode(m_bCursorOverWindow);

    return CUIStatic::OnMouseDown(mouse_btn);
}

bool CUIEditKeyBind::OnKeyboardAction(int dik, EUIMessages keyboard_action)
{
    if (dik == MOUSE_1 || dik == MOUSE_2 || dik == MOUSE_3)
        return false;

    if (CUIStatic::OnKeyboardAction(dik, keyboard_action))
        return true;

    string256 message; // see OnMouseDown
    if (m_isEditMode)
    {
        // The cell waits for a key: Delete is bound as any other key (the clearing is Delete over a hovered cell that
        // does not wait - below), so no key is lost for the bindings
        const bool is_gamepad_key = dik > XR_CONTROLLER_BUTTON_INVALID && dik < XR_CONTROLLER_BUTTON_MAX;

        // strictly separate keyboard/mouse from gamepad bindings
        if (m_isGamepadBinds != is_gamepad_key)
            return true;

        m_keyboard = DikToPtr(dik, true);
        if (!m_keyboard)
            return true;

        m_cleared = false;
        SetValue();

        xr_strcpy(message, ActionName());
        xr_strcat(message, "=");
        xr_strcat(message, m_keyboard->key_name);
        OnFocusLost();
        SendMessage2Group("key_binding", message);
        return true;
    }

    // Delete over a cell without the wait for a key clears it too. The keys go to every cell (no window captures the
    // keyboard), only the hovered one takes Delete; the menu navigation has no Delete (kUI_* actions)
    if (dik == SDL_SCANCODE_DELETE && keyboard_action == WINDOW_KEY_PRESSED && IsCursorOverVisibleCell())
    {
        ClearKey();
        return true;
    }
    return false;
}

bool CUIEditKeyBind::OnControllerAction(int axis, const ControllerAxisState& state, EUIMessages controller_action)
{
    if (CUIStatic::OnControllerAction(axis, state, controller_action))
        return true;

    if (m_isEditMode)
    {
        // strictly separate keyboard/mouse from gamepad bindings
        if (!m_isGamepadBinds)
            return true;

        m_keyboard = DikToPtr(axis, true);
        if (!m_keyboard)
            return true;

        m_cleared = false;
        SetValue();

        string256 message; // see OnMouseDown
        xr_strcpy(message, ActionName());
        xr_strcat(message, "=");
        xr_strcat(message, m_keyboard->key_name);
        OnFocusLost();
        SendMessage2Group("key_binding", message);
        return true;
    }

    return false;
}

void CUIEditKeyBind::Update()
{
    CUIStatic::Update();
}

void CUIEditKeyBind::SetEditMode(bool b)
{
    m_isEditMode = b;

    if (b)
    {
        SetColorAnimation("ui_map_area_anim", LA_CYCLIC | LA_ONLYALPHA | LA_TEXTCOLOR);
        TextureOn();
        if (GetParent())
            GetParent()->SetKeyboardCapture(this, true);
    }
    else
    {
        SetColorAnimation(NULL, 0);
        TextureOff();
        if (GetParent())
            GetParent()->SetKeyboardCapture(this, false);
    }
}

void CUIEditKeyBind::AssignProps(const shared_str& entry, const shared_str& group)
{
    CUIOptionsItem::AssignProps(entry, group);
    // An engine action never has a dot in its name, an addon key action always has one ("<group>.<id>")
    if (entry.size() && strchr(entry.c_str(), '.'))
    {
        m_addon_action = entry;
        m_action = nullptr;
        return;
    }
    m_action = ActionNameToPtr(entry.c_str());
}

pcstr CUIEditKeyBind::ActionName() const
{
    if (m_action)
        return m_action->action_name;
    return m_addon_action.size() ? m_addon_action.c_str() : "";
}

void CUIEditKeyBind::ClearKey()
{
    m_keyboard = nullptr;
    m_cleared = true;
    SetValue();
    // No message to the group "key_binding": no key, nothing to take from the other rows
    if (m_isEditMode)
        OnFocusLost(); // as after a key: out of the wait, the text alpha back
}

bool CUIEditKeyBind::IsCursorOverVisibleCell() const
{
    if (!CursorOverWindow())
        return false;
    // CUIWindow::OnKeyboardAction goes to the enabled children, the hidden ones too (a tab not shown); Update skips a
    // hidden window and leaves its m_bCursorOverWindow as it was
    for (const CUIWindow* it = this; it; it = it->GetParent())
    {
        if (!it->IsShown())
            return false;
    }
    // A row scrolled out of the list is still updated (CUIScrollView::Update) and may lie under the cursor outside
    // the list: the cursor must be inside the scroll view too
    for (const CUIWindow* it = GetParent(); it; it = it->GetParent())
    {
        if (dynamic_cast<const CUIScrollView*>(it))
        {
            Frect r;
            it->GetAbsoluteRect(r);
            Fvector2 cursor = UI().GetUICursor().GetCursorPosition();
            return !!r.in(cursor);
        }
    }
    return true;
}

void CUIEditKeyBind::SetValue()
{
    if (m_keyboard)
        SetText(m_keyboard->key_local_name.c_str());
    else
        SetText(NULL);
}

void CUIEditKeyBind::SetCurrentOptValue()
{
    m_cleared = false;
    if (m_addon_action.size())
    {
        m_keyboard = AddonKeyGet(m_addon_action.c_str(), m_primary ? 0 : 1);
        SetValue();
        return;
    }
    VERIFY(m_action);
    if (!m_action)
        return;
    key_binding* binding = &g_key_bindings[m_action->id];

    int idx = (!m_isGamepadBinds) ? ((m_primary) ? 0 : 1) : 2;
    m_keyboard = binding->m_keyboard[idx];

    SetValue();
}

void CUIEditKeyBind::SaveOptValue()
{
    CUIOptionsItem::SaveOptValue();
    BindAction2Key();
}

void CUIEditKeyBind::SaveBackUpOptValue()
{
    CUIOptionsItem::SaveBackUpOptValue();
    m_opt_backup_value = m_keyboard;
    m_cleared = false;
}

void CUIEditKeyBind::UndoOptValue()
{
    m_keyboard = m_opt_backup_value;
    m_cleared = false;
    CUIOptionsItem::UndoOptValue();
}

bool CUIEditKeyBind::IsChangedOptValue() const
{
    return m_keyboard != m_opt_backup_value;
}

void CUIEditKeyBind::BindAction2Key()
{
    const bool cleared = m_cleared;
    m_cleared = false;
    if (m_addon_action.size())
    {
        // nullptr too: a key taken by another row of the group (OnMessage) or cleared by Delete leaves the action
        // without it (bind_addon ... - in user.ltx)
        AddonKeySet(m_addon_action.c_str(), m_primary ? 0 : 1, m_keyboard);
        return;
    }
    if (!m_action)
        return;
    if (m_keyboard)
    {
        xr_string comm_bind = (!m_isGamepadBinds) ? ((m_primary) ? "bind " : "bind_sec ") : "bind_gpad ";
        comm_bind += m_action->action_name;
        comm_bind += " ";
        comm_bind += m_keyboard->key_name;
        Console->Execute(comm_bind.c_str());
    }
    else if (cleared)
    {
        // Only a cell the player cleared: a cell emptied by OnMessage keeps the key in the engine (one key may serve
        // several actions). CCC_Bind::Save writes the unbind to user.ltx if default_controls gives the slot a key
        xr_string comm_unbind = (!m_isGamepadBinds) ? ((m_primary) ? "unbind " : "unbind_sec ") : "unbind_gpad ";
        comm_unbind += m_action->action_name;
        Console->Execute(comm_unbind.c_str());
    }
}

void CUIEditKeyBind::OnMessage(LPCSTR message)
{
    // message = "command=key"
    int eq = (int)strcspn(message, "=");

    if (!m_keyboard)
        return;

    if (0 != xr_strcmp(m_keyboard->key_name, message + eq + 1))
        return;

    string256 command; // see OnMouseDown
    xr_strcpy(command, message);
    command[eq] = 0;

    if (0 == xr_strcmp(ActionName(), command))
        return; // fuck

    // Addon key actions: a key of a group with mode = true (the addon takes it only while its mode is on) may repeat
    // the keys of other groups and of the game actions - only a row of the same group gives the key up
    shared_str own_group, other_group;
    bool own_mode = false, other_mode = false;
    const bool own_addon = m_addon_action.size() && AddonKeyFind(m_addon_action.c_str(), &own_group, &own_mode);
    const bool other_addon = strchr(command, '.') && AddonKeyFind(command, &other_group, &other_mode);
    if (own_addon || other_addon)
    {
        const bool same_group = own_addon && other_addon && own_group == other_group;
        if (!same_group && (own_mode || other_mode))
            return;
    }

    // game_action* other_action = ActionNameToPtr(command);

    // bool no_conflict  = IsGroupNotConflicted(m_action->key_group, other_action->key_group);
    //      no_conflict &= IsContextNotConflicted(m_action->key_context, other_action->key_context);
    // if (no_conflict)
    //     return;

    // SetText("---");
    m_keyboard = NULL;
}
