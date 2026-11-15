#pragma once

#include "xrCommon/xr_string.h"
#include "xrCommon/xr_map.h"

#include "xrCore/xrstring.h"

#include "xrEngine/xr_input.h" // Don't remove this include

#include "xrScriptEngine/ScriptExporter.hpp"

enum EGameActions : u32
{
    kLOOK_AROUND, // gamepad
    kLEFT,
    kRIGHT,
    kUP,
    kDOWN,

    kMOVE_AROUND, // gamepad
    kFWD,
    kBACK,
    kL_STRAFE,
    kR_STRAFE,

    kL_LOOKOUT,
    kR_LOOKOUT,

    kJUMP,
    kCROUCH,
    kCROUCH_TOGGLE,
    kACCEL,
    kSPRINT_TOGGLE,

    kENGINE,

    kCAM_1,
    kCAM_2,
    kCAM_3,
    kCAM_4,
    kCAM_ZOOM_IN,
    kCAM_ZOOM_OUT,
    kCAM_AUTOAIM,

    kTORCH,
    kNIGHT_VISION,
    kDETECTOR,
    kWPN_1,
    kWPN_2,
    kWPN_3,
    kWPN_4,
    kWPN_5,
    kWPN_6,
    kARTEFACT,
    kWPN_NEXT,
    kWPN_FIRE,
    kWPN_ZOOM,
    kWPN_ZOOM_SECOND,
    kWPN_ZOOM_INC,
    kWPN_ZOOM_DEC,
    kWPN_HUD_FOV_ADJUST,
    kWPN_RELOAD,
    kWPN_FUNC,
    kWPN_FIREMODE_PREV,
    kWPN_FIREMODE_NEXT,
    kWPN_QUICK_UNLOAD,
    kMOVE_ALL_STACK_ITEMS,

    kPAUSE,
    kDROP,
    kUSE,
    kSCORES,
    kCHAT,
    kCHAT_TEAM,
    kSCREENSHOT,
    kENTER,
    kQUIT,
    kCONSOLE,
    kINVENTORY,
    kBUY,
    kSKIN,
    kTEAM,
    kACTIVE_JOBS,
    kMAP,
    kCONTACTS,
    kEXT_1,

    kVOTE_BEGIN,
    kSHOW_ADMIN_MENU,
    kVOTE,
    kVOTEYES,
    kVOTENO,

    kNEXT_SLOT,
    kPREV_SLOT,

    kSPEECH_MENU_0,
    kSPEECH_MENU_1,
    kSPEECH_MENU_2,
    kSPEECH_MENU_3,
    kSPEECH_MENU_4,
    kSPEECH_MENU_5,
    kSPEECH_MENU_6,
    kSPEECH_MENU_7,
    kSPEECH_MENU_8,
    kSPEECH_MENU_9,

    kUSE_BANDAGE,
    kUSE_MEDKIT,

    kQUICK_USE_1,
    kQUICK_USE_2,
    kQUICK_USE_3,
    kQUICK_USE_4,

    kQUICK_SAVE,
    kQUICK_LOAD,
    kALIFE_CMD,
    kWPN_NV_CHANGE,

    kINSERT,
    kNUMPAD0,
    kNUMPAD1,
    kNUMPAD2,
    kNUMPAD3,
    kNUMPAD4,
    kNUMPAD5,
    kNUMPAD6,
    kNUMPAD7,
    kNUMPAD8,
    kNUMPAD9,
    kNUMPADENTER,

    kCUSTOM1,
    kCUSTOM2,
    kCUSTOM3,
    kCUSTOM4,
    kCUSTOM5,
    kCUSTOM6,
    kCUSTOM7,
    kCUSTOM8,
    kCUSTOM9,
    kCUSTOM10,
    kCUSTOM11,
    kCUSTOM12,
    kCUSTOM13,
    kCUSTOM14,
    kCUSTOM15,

    kPDA_TAB1,
    kPDA_TAB2,
    kPDA_TAB3,
    kPDA_TAB4,
    kPDA_TAB5,
    kPDA_TAB6,

    kKICK, // alpet: kick dynamic objects

    kEDITOR,

    // kPDA_TAB1,
    // kPDA_TAB2,
    // kPDA_TAB3,
    // kPDA_TAB4,
    // kPDA_TAB5,
    // kPDA_TAB6,

    // Contextual actions:
    // UI
    kUI_MOVE,
    kUI_MOVE_LEFT,
    kUI_MOVE_RIGHT,
    kUI_MOVE_UP,
    kUI_MOVE_DOWN,

    kUI_MOVE_SECONDARY,

    kUI_CLICK_1,
    kUI_CLICK_2,

    kUI_ACCEPT,
    kUI_BACK,
    kUI_ACTION_1,
    kUI_ACTION_2,

    kUI_TAB_PREV,
    kUI_TAB_NEXT,

    kUI_BUTTON_1,
    kUI_BUTTON_2,
    kUI_BUTTON_3,
    kUI_BUTTON_4,
    kUI_BUTTON_5,
    kUI_BUTTON_6,
    kUI_BUTTON_7,
    kUI_BUTTON_8,
    kUI_BUTTON_9,
    kUI_BUTTON_0,

    // PDA:
    kPDA_MAP_MOVE, // gamepad axis
    kPDA_MAP_MOVE_LEFT,
    kPDA_MAP_MOVE_RIGHT,
    kPDA_MAP_MOVE_UP,
    kPDA_MAP_MOVE_DOWN,

    kPDA_MAP_ZOOM_IN,
    kPDA_MAP_ZOOM_OUT,
    kPDA_MAP_ZOOM_RESET,

    kPDA_MAP_SHOW_ACTOR,
    kPDA_MAP_SHOW_LEGEND,

    kPDA_FILTER_TOGGLE,

    // Talk:
    kTALK_SWITCH_TO_TRADE, // _OR_UPGRADE
    kTALK_LOG_SCROLL,
    kTALK_LOG_SCROLL_UP,
    kTALK_LOG_SCROLL_DOWN,

    // GW. Appended after the contextual actions: the ids of the actions before it go to Lua and to the plugins
    // (key_bindings, the event level_input_on_key_press) and must stay the same.
    kBODYCAM_SMS, // bodycam HUD: show / hide the SMS feed (g_bodycam_sms), F11 by default

    kLASTACTION,
    kNOTBINDED
};

enum class EKeyContext
{
    Undefined = 0, // default behaviour
    UI, // UI overall
    PDA,
    Talk,
};

constexpr char GAME_ACTION_MARK = 27; // escape symbol

struct keyboard_key
{
    pcstr key_name;
    int dik;
    xr_string key_local_name;
};

enum EKeyGroup
{
    _both = (1 << 0),
    _sp = _both | (1 << 1),
    _mp = _both | (1 << 2),
};

struct game_action
{
    pcstr action_name;
    EGameActions id;
    EKeyGroup key_group;
    EKeyContext key_context{ EKeyContext::Undefined };
};

#define bindtypes_count 3
struct key_binding
{
    game_action* m_action;
    keyboard_key* m_keyboard[bindtypes_count];
};

extern ENGINE_API EKeyGroup g_current_keygroup;

extern ENGINE_API game_action actions[];
extern ENGINE_API keyboard_key keyboards[];

extern ENGINE_API key_binding g_key_bindings[];

ENGINE_API bool IsGroupNotConflicted(EKeyGroup g1, EKeyGroup g2);
ENGINE_API bool IsContextNotConflicted(EKeyContext c1, EKeyContext c2);

ENGINE_API pcstr IdToActionName(EGameActions id);
ENGINE_API EGameActions ActionNameToId(pcstr name, bool silent = false);
ENGINE_API game_action* ActionNameToPtr(pcstr name, bool silent = false);

ENGINE_API pcstr DikToKeyname(int dik);
ENGINE_API int KeynameToDik(pcstr name, bool silent = false);
ENGINE_API keyboard_key* KeynameToPtr(pcstr name, bool silent = false);
ENGINE_API keyboard_key* DikToPtr(int dik, bool silent);

ENGINE_API bool IsBinded(EGameActions action_id, int dik, EKeyContext context = EKeyContext::Undefined);
ENGINE_API int GetActionDik(EGameActions action_id, int idx = -1);
ENGINE_API EGameActions GetBindedAction(int dik, EKeyContext context = EKeyContext::Undefined);

ENGINE_API pcstr GetActionBinding(EGameActions action);

template <typename Invocable>
void ForAllActionKeys(EGameActions action_id, Invocable&& invocable)
{
    for (size_t i = 0; i < bindtypes_count; ++i)
    {
        const int key = GetActionDik(action_id, i);
        if (key == SDL_SCANCODE_UNKNOWN)
            continue;

        if constexpr (std::is_invocable_r_v<bool, Invocable, size_t, int>)
        {
            if (invocable(i, key))
                break;
        }
        else
        {
            static_assert(std::is_invocable_v<Invocable, size_t, int>);
            invocable(i, key);
        }
    }
}

// Addon key actions (plans/addons_api_support/19-addon-key-groups.md; Lua - addons.register_key_group, gw_keys.script).
// Rebindable keys of addons outside the engine actions (EGameActions): a group with a caption and its actions, each with
// a caption and up to two keys (keyboard / mouse; gamepads are not supported). Options -> Controls shows every group
// after the groups of ui_keybinding.xml (CUIKeyBinding::FillUpList); the keys a player changed are the console command
// bind_addon in user.ltx, default_controls returns the defaults. The engine dispatches nothing: the addon compares the
// key it got (on_key_press, level_input_on_key_press) with the key of its action - AddonKeyIs.
// Names: a group - 1..64 of [A-Za-z0-9_-]; an action - "<group>.<id>" (the dot keeps them apart from engine actions).
// A key the player set for an action that no addon registers now (its addon is off) is kept and saved back.
struct addon_key_action_view
{
    shared_str name; // "<group>.<id>"
    shared_str label; // string id of the caption
};

struct addon_key_group_view
{
    shared_str name;
    shared_str title; // string id of the caption
    // true - the keys work only in a mode of the addon (it takes them while the mode is on): in Options -> Controls they
    // do not clear the same key of other groups and of the game actions (and the other way round), only inside the group
    bool mode;
    xr_vector<addon_key_action_view> actions;
};

// A group, or new caption / mode of the group. false - a bad name (one log line)
ENGINE_API bool AddonKeyGroupRegister(pcstr group, pcstr title, bool mode);
// An action of a registered group (again - new caption / defaults / group, the keys the player set stay). key, key_sec
// - default keys (DIK_keys / SDL scancode, MOUSE_n), 0 or an unknown code - none. false - bad name or no such group
ENGINE_API bool AddonKeyRegister(pcstr group, pcstr action, pcstr label, int key, int key_sec);
// Drops every group (the actions keep only the keys the player set): a new Lua state registers them again
// (RunLuaEntries of xrGame\addon_lua_entry.cpp)
ENGINE_API void AddonKeyGroupsClear();
// The groups in the order of registration, the actions of a group too
ENGINE_API void AddonKeyGroups(xr_vector<addon_key_group_view>& groups);
// Whether `action` is a registered addon action; its group and the mode of the group
ENGINE_API bool AddonKeyFind(pcstr action, shared_str* group = nullptr, bool* mode = nullptr);
// The key of the slot idx (0 - primary, 1 - secondary) of a registered action, nullptr - none
ENGINE_API keyboard_key* AddonKeyGet(pcstr action, int idx);
// Sets the key of the slot (nullptr - none); the default key makes the slot "not changed" (not saved)
ENGINE_API void AddonKeySet(pcstr action, int idx, keyboard_key* key);
// Whether dik is one of the keys of a registered action
ENGINE_API bool AddonKeyIs(pcstr action, int dik);
// The keys of a registered action for a hint, localized: "Q", "Q, Delete"; "" - no key / no action
ENGINE_API xr_string AddonKeyText(pcstr action);
// The localized name of a key (as Options -> Controls shows it); "" - unknown code
ENGINE_API xr_string DikToLocalName(int dik);

extern ENGINE_API void CCC_RegisterInput();
extern ENGINE_API void CCC_DeregisterInput();

struct con_cmd
{
    shared_str cmd;
};

class ENGINE_API ConsoleBindCmds
{
public:
    xr_map<int, con_cmd> m_bindConsoleCmds;

    void bind(int dik, LPCSTR N);
    void unbind(int dik);
    bool execute(int dik);
    void clear();
    void save(IWriter* F);
};

extern ENGINE_API ConsoleBindCmds g_consoleBindCmds;

struct ENGINE_API key_binding_registrator
{
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};
