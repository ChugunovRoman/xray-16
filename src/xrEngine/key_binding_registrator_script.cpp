#include "stdafx.h"

#include "xr_level_controller.h"

#include "xrScriptEngine/ScriptExporter.hpp"

// clang-format off
SCRIPT_EXPORT(KeyBindings, (),
{
    class EnumGameActionsContexts {};
    class EnumGameActions {};
    class KeyBindingRegistrator {};

    using namespace luabind;
    module(luaState)
    [
        def("dik_to_bind", +[](int dik) -> int { return GetBindedAction(dik); }),
        def("dik_to_bind", +[](int dik, int ctx) -> int { return GetBindedAction(dik, (EKeyContext)ctx); }),

        class_<EnumGameActionsContexts>("key_bindings_context")
            .enum_("context")
            [
                value("undefined",                  int(EKeyContext::Undefined)),
                value("ui",                         int(EKeyContext::UI)),
                value("pda",                        int(EKeyContext::PDA)),
                value("talk",                       int(EKeyContext::Talk))
            ],

        class_<EnumGameActions>("key_bindings")
            .enum_("commands")
            [
                value("kLOOK_AROUND",               int(kLOOK_AROUND)),
                value("kLEFT",                      int(kLEFT)),
                value("kRIGHT",                     int(kRIGHT)),
                value("kUP",                        int(kUP)),
                value("kDOWN",                      int(kDOWN)),

                value("kMOVE_AROUND",               int(kMOVE_AROUND)),
                value("kFWD",                       int(kFWD)),
                value("kBACK",                      int(kBACK)),
                value("kL_STRAFE",                  int(kL_STRAFE)),
                value("kR_STRAFE",                  int(kR_STRAFE)),

                value("kL_LOOKOUT",                 int(kL_LOOKOUT)),
                value("kR_LOOKOUT",                 int(kR_LOOKOUT)),

                value("kJUMP",                      int(kJUMP)),
                value("kCROUCH",                    int(kCROUCH)),
                value("kCROUCH_TOGGLE",             int(kCROUCH_TOGGLE)),
                value("kACCEL",                     int(kACCEL)),
                value("kSPRINT_TOGGLE",             int(kSPRINT_TOGGLE)),

                value("kENGINE",                    int(kENGINE)),

                value("kCAM_1",                     int(kCAM_1)),
                value("kCAM_2",                     int(kCAM_2)),
                value("kCAM_3",                     int(kCAM_3)),
                value("kCAM_4",                     int(kCAM_4)),
                value("kCAM_ZOOM_IN",               int(kCAM_ZOOM_IN)),
                value("kCAM_ZOOM_OUT",              int(kCAM_ZOOM_OUT)),
                value("kCAM_AUTOAIM",               int(kCAM_AUTOAIM)),

                value("kTORCH",                     int(kTORCH)),
                value("kNIGHT_VISION",              int(kNIGHT_VISION)),
                value("kDETECTOR",                  int(kDETECTOR)),
                value("kWPN_1",                     int(kWPN_1)),
                value("kWPN_2",                     int(kWPN_2)),
                value("kWPN_3",                     int(kWPN_3)),
                value("kWPN_4",                     int(kWPN_4)),
                value("kWPN_5",                     int(kWPN_5)),
                value("kWPN_6",                     int(kWPN_6)),
                value("kARTEFACT",                  int(kARTEFACT)),
                value("kWPN_NEXT",                  int(kWPN_NEXT)),
                value("kWPN_FIRE",                  int(kWPN_FIRE)),
                value("kWPN_ZOOM",                  int(kWPN_ZOOM)),
                value("kWPN_ZOOM_SECOND",           int(kWPN_ZOOM_SECOND)),
                value("kWPN_ZOOM_INC",              int(kWPN_ZOOM_INC)),
                value("kWPN_ZOOM_DEC",              int(kWPN_ZOOM_DEC)),
                value("kWPN_RELOAD",                int(kWPN_RELOAD)),
                value("kWPN_FUNC",                  int(kWPN_FUNC)),
                value("kWPN_FIREMODE_PREV",         int(kWPN_FIREMODE_PREV)),
                value("kWPN_FIREMODE_NEXT",         int(kWPN_FIREMODE_NEXT)),
                value("kWPN_QUICK_UNLOAD",          int(kWPN_QUICK_UNLOAD)),

                value("kDROP",                      int(kDROP)),
                value("kUSE",                       int(kUSE)),
                value("kSCORES",                    int(kSCORES)),
                value("kCHAT",                      int(kCHAT)),
                value("kCHAT_TEAM",                 int(kCHAT_TEAM)),
                value("kSCREENSHOT",                int(kSCREENSHOT)),
                value("kENTER",                     int(kENTER)),
                value("kQUIT",                      int(kQUIT)),
                value("kCONSOLE",                   int(kCONSOLE)),
                value("kINVENTORY",                 int(kINVENTORY)),
                value("kBUY",                       int(kBUY)),
                value("kSKIN",                      int(kSKIN)),
                value("kTEAM",                      int(kTEAM)),
                value("kACTIVE_JOBS",               int(kACTIVE_JOBS)),
                value("kMAP",                       int(kMAP)),
                value("kCONTACTS",                  int(kCONTACTS)),
                value("kEXT_1",                     int(kEXT_1)),

                value("kVOTE_BEGIN",                int(kVOTE_BEGIN)),
                value("kSHOW_ADMIN_MENU",           int(kSHOW_ADMIN_MENU)),
                value("kVOTE",                      int(kVOTE)),
                value("kVOTEYES",                   int(kVOTEYES)),
                value("kVOTENO",                    int(kVOTENO)),

                value("kNEXT_SLOT",                 int(kNEXT_SLOT)),
                value("kPREV_SLOT",                 int(kPREV_SLOT)),

                value("kSPEECH_MENU_0",             int(kSPEECH_MENU_0)),
                value("kSPEECH_MENU_1",             int(kSPEECH_MENU_1)),
                value("kSPEECH_MENU_2",             int(kSPEECH_MENU_2)),
                value("kSPEECH_MENU_3",             int(kSPEECH_MENU_3)),
                value("kSPEECH_MENU_4",             int(kSPEECH_MENU_4)),
                value("kSPEECH_MENU_5",             int(kSPEECH_MENU_5)),
                value("kSPEECH_MENU_6",             int(kSPEECH_MENU_6)),
                value("kSPEECH_MENU_7",             int(kSPEECH_MENU_7)),
                value("kSPEECH_MENU_8",             int(kSPEECH_MENU_8)),
                value("kSPEECH_MENU_9",             int(kSPEECH_MENU_9)),

                value("kUSE_BANDAGE",               int(kUSE_BANDAGE)),
                value("kUSE_MEDKIT",                int(kUSE_MEDKIT)),

                value("kQUICK_USE_1",               int(kQUICK_USE_1)),
                value("kQUICK_USE_2",               int(kQUICK_USE_2)),
                value("kQUICK_USE_3",               int(kQUICK_USE_3)),
                value("kQUICK_USE_4",               int(kQUICK_USE_4)),

                value("kQUICK_SAVE",                int(kQUICK_SAVE)),
                value("kQUICK_LOAD",                int(kQUICK_LOAD)),
                value("kALIFE_CMD",                 int(kALIFE_CMD)),

                value("kNUMPAD0",                   int(kNUMPAD0)),
                value("kNUMPAD1",                   int(kNUMPAD1)),
                value("kNUMPAD2",                   int(kNUMPAD2)),
                value("kNUMPAD3",                   int(kNUMPAD3)),
                value("kNUMPAD4",                   int(kNUMPAD4)),
                value("kNUMPAD5",                   int(kNUMPAD5)),
                value("kNUMPAD6",                   int(kNUMPAD6)),
                value("kNUMPAD7",                   int(kNUMPAD7)),
                value("kNUMPAD8",                   int(kNUMPAD8)),
                value("kNUMPAD9",                   int(kNUMPAD9)),
                value("kNUMPADENTER",               int(kNUMPADENTER)),
                value("kINSERT",                    int(kINSERT)),

                value("kCUSTOM1",                   int(kCUSTOM1)),
                value("kCUSTOM2",                   int(kCUSTOM2)),
                value("kCUSTOM3",                   int(kCUSTOM3)),
                value("kCUSTOM4",                   int(kCUSTOM4)),
                value("kCUSTOM5",                   int(kCUSTOM5)),
                value("kCUSTOM6",                   int(kCUSTOM6)),
                value("kCUSTOM7",                   int(kCUSTOM7)),
                value("kCUSTOM8",                   int(kCUSTOM8)),
                value("kCUSTOM9",                   int(kCUSTOM9)),
                value("kCUSTOM10",                  int(kCUSTOM10)),
                value("kCUSTOM11",                  int(kCUSTOM11)),
                value("kCUSTOM12",                  int(kCUSTOM12)),
                value("kCUSTOM13",                  int(kCUSTOM13)),
                value("kCUSTOM14",                  int(kCUSTOM14)),
                value("kCUSTOM15",                  int(kCUSTOM15)),
                // value("kPDA_TAB1",                  int(kPDA_TAB1)),
                // value("kPDA_TAB2",                  int(kPDA_TAB2)),
                // value("kPDA_TAB3",                  int(kPDA_TAB3)),
                // value("kPDA_TAB4",                  int(kPDA_TAB4)),
                // value("kPDA_TAB5",                  int(kPDA_TAB5)),
                // value("kPDA_TAB6",                  int(kPDA_TAB6)),

                value("kPDA_TAB1",                  int(kPDA_TAB1)),
                value("kPDA_TAB2",                  int(kPDA_TAB2)),
                value("kPDA_TAB3",                  int(kPDA_TAB3)),
                value("kPDA_TAB4",                  int(kPDA_TAB4)),
                value("kPDA_TAB5",                  int(kPDA_TAB5)),
                value("kPDA_TAB6",                  int(kPDA_TAB6)),

                value("kKICK",                      int(kKICK)),

                value("kEDITOR",                    int(kEDITOR)),

                // Contextual actions:
                // UI
                value("kUI_MOVE",                   int(kUI_MOVE)),
                value("kUI_MOVE_LEFT",              int(kUI_MOVE_LEFT)),
                value("kUI_MOVE_RIGHT",             int(kUI_MOVE_RIGHT)),
                value("kUI_MOVE_UP",                int(kUI_MOVE_UP)),
                value("kUI_MOVE_DOWN",              int(kUI_MOVE_DOWN)),

                value("kUI_MOVE_SECONDARY",         int(kUI_MOVE_SECONDARY)),

                value("kUI_ACCEPT",                 int(kUI_ACCEPT)),
                value("kUI_BACK",                   int(kUI_BACK)),
                value("kUI_ACTION_1",               int(kUI_ACTION_1)),
                value("kUI_ACTION_2",               int(kUI_ACTION_2)),

                value("kUI_TAB_PREV",               int(kUI_TAB_PREV)),
                value("kUI_TAB_NEXT",               int(kUI_TAB_NEXT)),

                value("kUI_BUTTON_1",               int(kUI_BUTTON_1)),
                value("kUI_BUTTON_2",               int(kUI_BUTTON_2)),
                value("kUI_BUTTON_3",               int(kUI_BUTTON_3)),
                value("kUI_BUTTON_4",               int(kUI_BUTTON_4)),
                value("kUI_BUTTON_5",               int(kUI_BUTTON_5)),
                value("kUI_BUTTON_6",               int(kUI_BUTTON_6)),
                value("kUI_BUTTON_7",               int(kUI_BUTTON_7)),
                value("kUI_BUTTON_8",               int(kUI_BUTTON_8)),
                value("kUI_BUTTON_9",               int(kUI_BUTTON_9)),
                value("kUI_BUTTON_0",               int(kUI_BUTTON_0)),

                // PDA:
                value("kPDA_MAP_MOVE",              int(kPDA_MAP_MOVE)),
                value("kPDA_MAP_MOVE_LEFT",         int(kPDA_MAP_MOVE_LEFT)),
                value("kPDA_MAP_MOVE_RIGHT",        int(kPDA_MAP_MOVE_RIGHT)),
                value("kPDA_MAP_MOVE_UP",           int(kPDA_MAP_MOVE_UP)),
                value("kPDA_MAP_MOVE_DOWN",         int(kPDA_MAP_MOVE_DOWN)),

                value("kPDA_MAP_ZOOM_IN",           int(kPDA_MAP_ZOOM_IN)),
                value("kPDA_MAP_ZOOM_OUT",          int(kPDA_MAP_ZOOM_OUT)),
                value("kPDA_MAP_ZOOM_RESET",        int(kPDA_MAP_ZOOM_RESET)),

                value("kPDA_MAP_SHOW_ACTOR",        int(kPDA_MAP_SHOW_ACTOR)),
                value("kPDA_MAP_SHOW_LEGEND",       int(kPDA_MAP_SHOW_LEGEND)),

                value("kPDA_FILTER_TOGGLE",         int(kPDA_FILTER_TOGGLE)),
                value("kPDA_TASKS_TOGGLE",          int(kPDA_TASKS_TOGGLE)),

                // Talk:
                value("kTALK_SWITCH_TO_TRADE",      int(kTALK_SWITCH_TO_TRADE)),
                value("kTALK_LOG_SCROLL",           int(kTALK_LOG_SCROLL)),
                value("kTALK_LOG_SCROLL_UP",        int(kTALK_LOG_SCROLL_UP)),
                value("kTALK_LOG_SCROLL_DOWN",      int(kTALK_LOG_SCROLL_DOWN))
            ],

        class_<KeyBindingRegistrator>("DIK_keys")
            .enum_("dik_keys")
            [
                value("DIK_A",                      int(SDL_SCANCODE_A)),
                value("DIK_B",                      int(SDL_SCANCODE_B)),
                value("DIK_C",                      int(SDL_SCANCODE_C)),
                value("DIK_D",                      int(SDL_SCANCODE_D)),
                value("DIK_E",                      int(SDL_SCANCODE_E)),
                value("DIK_F",                      int(SDL_SCANCODE_F)),
                value("DIK_G",                      int(SDL_SCANCODE_G)),
                value("DIK_H",                      int(SDL_SCANCODE_H)),
                value("DIK_I",                      int(SDL_SCANCODE_I)),
                value("DIK_J",                      int(SDL_SCANCODE_J)),
                value("DIK_K",                      int(SDL_SCANCODE_K)),
                value("DIK_L",                      int(SDL_SCANCODE_L)),
                value("DIK_M",                      int(SDL_SCANCODE_M)),
                value("DIK_N",                      int(SDL_SCANCODE_N)),
                value("DIK_O",                      int(SDL_SCANCODE_O)),
                value("DIK_P",                      int(SDL_SCANCODE_P)),
                value("DIK_Q",                      int(SDL_SCANCODE_Q)),
                value("DIK_R",                      int(SDL_SCANCODE_R)),
                value("DIK_S",                      int(SDL_SCANCODE_S)),
                value("DIK_T",                      int(SDL_SCANCODE_T)),
                value("DIK_U",                      int(SDL_SCANCODE_U)),
                value("DIK_V",                      int(SDL_SCANCODE_V)),
                value("DIK_W",                      int(SDL_SCANCODE_W)),
                value("DIK_X",                      int(SDL_SCANCODE_X)),
                value("DIK_Y",                      int(SDL_SCANCODE_Y)),
                value("DIK_Z",                      int(SDL_SCANCODE_Z)),

                value("DIK_1",                      int(SDL_SCANCODE_1)),
                value("DIK_2",                      int(SDL_SCANCODE_2)),
                value("DIK_3",                      int(SDL_SCANCODE_3)),
                value("DIK_4",                      int(SDL_SCANCODE_4)),
                value("DIK_5",                      int(SDL_SCANCODE_5)),
                value("DIK_6",                      int(SDL_SCANCODE_6)),
                value("DIK_7",                      int(SDL_SCANCODE_7)),
                value("DIK_8",                      int(SDL_SCANCODE_8)),
                value("DIK_9",                      int(SDL_SCANCODE_9)),
                value("DIK_0",                      int(SDL_SCANCODE_0)),

                value("DIK_RETURN",                 int(SDL_SCANCODE_RETURN)),
                value("DIK_ESCAPE",                 int(SDL_SCANCODE_ESCAPE)),
                value("DIK_BACK",                   int(SDL_SCANCODE_BACKSPACE)),
                value("DIK_TAB",                    int(SDL_SCANCODE_TAB)),
                value("DIK_SPACE",                  int(SDL_SCANCODE_SPACE)),

                value("DIK_MINUS",                  int(SDL_SCANCODE_MINUS)),
                value("DIK_EQUALS",                 int(SDL_SCANCODE_EQUALS)),
                value("DIK_LBRACKET",               int(SDL_SCANCODE_LEFTBRACKET)),
                value("DIK_RBRACKET",               int(SDL_SCANCODE_RIGHTBRACKET)),
                value("DIK_BACKSLASH",              int(SDL_SCANCODE_BACKSLASH)),
                value("DIK_NONUSHASH",              int(SDL_SCANCODE_NONUSHASH)),

                value("DIK_SEMICOLON",              int(SDL_SCANCODE_SEMICOLON)),
                value("DIK_APOSTROPHE",             int(SDL_SCANCODE_APOSTROPHE)),
                value("DIK_GRAVE",                  int(SDL_SCANCODE_GRAVE)),
                value("DIK_COMMA",                  int(SDL_SCANCODE_COMMA)),
                value("DIK_PERIOD",                 int(SDL_SCANCODE_PERIOD)),
                value("DIK_SLASH",                  int(SDL_SCANCODE_SLASH)),

                value("DIK_CAPITAL",                int(SDL_SCANCODE_CAPSLOCK)),

                value("DIK_F1",                     int(SDL_SCANCODE_F1)),
                value("DIK_F2",                     int(SDL_SCANCODE_F2)),
                value("DIK_F3",                     int(SDL_SCANCODE_F3)),
                value("DIK_F4",                     int(SDL_SCANCODE_F4)),
                value("DIK_F5",                     int(SDL_SCANCODE_F5)),
                value("DIK_F6",                     int(SDL_SCANCODE_F6)),
                value("DIK_F7",                     int(SDL_SCANCODE_F7)),
                value("DIK_F8",                     int(SDL_SCANCODE_F8)),
                value("DIK_F9",                     int(SDL_SCANCODE_F9)),
                value("DIK_F10",                    int(SDL_SCANCODE_F10)),
                value("DIK_F11",                    int(SDL_SCANCODE_F11)),
                value("DIK_F12",                    int(SDL_SCANCODE_F12)),

                value("DIK_PRINTSCREEN",            int(SDL_SCANCODE_PRINTSCREEN)),
                value("DIK_SCROLL",                 int(SDL_SCANCODE_SCROLLLOCK)),
                value("DIK_PAUSE",                  int(SDL_SCANCODE_PAUSE)),
                value("DIK_INSERT",                 int(SDL_SCANCODE_INSERT)),

                value("DIK_HOME",                   int(SDL_SCANCODE_HOME)),
                value("DIK_PGDN",                   int(SDL_SCANCODE_PAGEUP)),
                value("DIK_DELETE",                 int(SDL_SCANCODE_DELETE)),
                value("DIK_END",                    int(SDL_SCANCODE_END)),
                value("DIK_PGUP",                   int(SDL_SCANCODE_PAGEDOWN)),

                value("DIK_RIGHT",                  int(SDL_SCANCODE_RIGHT)),
                value("DIK_LEFT",                   int(SDL_SCANCODE_LEFT)),
                value("DIK_DOWN",                   int(SDL_SCANCODE_DOWN)),
                value("DIK_UP",                     int(SDL_SCANCODE_UP)),

                value("DIK_NUMLOCK",                int(SDL_SCANCODE_NUMLOCKCLEAR)),

                value("DIK_DIVIDE",                 int(SDL_SCANCODE_KP_DIVIDE)),
                value("DIK_MULTIPLY",               int(SDL_SCANCODE_KP_MULTIPLY)),
                value("DIK_SUBTRACT",               int(SDL_SCANCODE_KP_MINUS)),
                value("DIK_ADD",                    int(SDL_SCANCODE_KP_PLUS)),
                value("DIK_NUMPADENTER",            int(SDL_SCANCODE_KP_ENTER)),

                value("DIK_NUMPAD1",                int(SDL_SCANCODE_KP_1)),
                value("DIK_NUMPAD2",                int(SDL_SCANCODE_KP_2)),
                value("DIK_NUMPAD3",                int(SDL_SCANCODE_KP_3)),
                value("DIK_NUMPAD4",                int(SDL_SCANCODE_KP_4)),
                value("DIK_NUMPAD5",                int(SDL_SCANCODE_KP_5)),
                value("DIK_NUMPAD6",                int(SDL_SCANCODE_KP_6)),
                value("DIK_NUMPAD7",                int(SDL_SCANCODE_KP_7)),
                value("DIK_NUMPAD8",                int(SDL_SCANCODE_KP_8)),
                value("DIK_NUMPAD9",                int(SDL_SCANCODE_KP_9)),
                value("DIK_NUMPAD0",                int(SDL_SCANCODE_KP_0)),

                value("DIK_NUMPADPERIOD",           int(SDL_SCANCODE_KP_PERIOD)),
                value("DIK_NONUSBACKSLASH",         int(SDL_SCANCODE_NONUSBACKSLASH)),
                value("DIK_APPLICATION",            int(SDL_SCANCODE_APPLICATION)),
                value("DIK_POWER",                  int(SDL_SCANCODE_POWER)),
                value("DIK_NUMPADEQUALS",           int(SDL_SCANCODE_KP_EQUALS)),

                value("DIK_F13",                    int(SDL_SCANCODE_F13)),
                value("DIK_F14",                    int(SDL_SCANCODE_F14)),
                value("DIK_F15",                    int(SDL_SCANCODE_F15)),
                value("DIK_F16",                    int(SDL_SCANCODE_F16)),
                value("DIK_F17",                    int(SDL_SCANCODE_F17)),
                value("DIK_F18",                    int(SDL_SCANCODE_F18)),
                value("DIK_F19",                    int(SDL_SCANCODE_F19)),
                value("DIK_F20",                    int(SDL_SCANCODE_F20)),
                value("DIK_F21",                    int(SDL_SCANCODE_F21)),
                value("DIK_F22",                    int(SDL_SCANCODE_F22)),
                value("DIK_F23",                    int(SDL_SCANCODE_F23)),
                value("DIK_F24",                    int(SDL_SCANCODE_F24)),

                value("DIK_EXECUTE",                int(SDL_SCANCODE_EXECUTE)),
                value("DIK_HELP",                   int(SDL_SCANCODE_HELP)),
                value("DIK_MENU",                   int(SDL_SCANCODE_MENU)),

                value("DIK_SELECT",                 int(SDL_SCANCODE_SELECT)),
                value("DIK_STOP",                   int(SDL_SCANCODE_STOP)),

                value("DIK_REDO",                   int(SDL_SCANCODE_AGAIN)),
                value("DIK_UNDO",                   int(SDL_SCANCODE_UNDO)),

                value("DIK_CUT",                    int(SDL_SCANCODE_CUT)),
                value("DIK_COPY",                   int(SDL_SCANCODE_COPY)),
                value("DIK_PASTE",                  int(SDL_SCANCODE_PASTE)),

                value("DIK_FIND",                   int(SDL_SCANCODE_FIND)),

                value("DIK_MUTE",                   int(SDL_SCANCODE_MUTE)),
                value("DIK_VOLUMEUP",               int(SDL_SCANCODE_VOLUMEUP)),
                value("DIK_VOLUMEDOWN",             int(SDL_SCANCODE_VOLUMEDOWN)),

                value("DIK_NUMPADCOMMA",            int(SDL_SCANCODE_KP_COMMA)),
                value("DIK_NUMPADEQUALSAS400",      int(SDL_SCANCODE_KP_EQUALSAS400)),

                value("DIK_INTERNATIONAL1",         int(SDL_SCANCODE_INTERNATIONAL1)), // Give a better name?
                value("DIK_INTERNATIONAL2",         int(SDL_SCANCODE_INTERNATIONAL2)), // Give a better name?
                value("DIK_YEN",                    int(SDL_SCANCODE_INTERNATIONAL3)),
                value("DIK_INTERNATIONAL4",         int(SDL_SCANCODE_INTERNATIONAL4)), // Give a better name?
                value("DIK_INTERNATIONAL5",         int(SDL_SCANCODE_INTERNATIONAL5)), // Give a better name?
                value("DIK_INTERNATIONAL6",         int(SDL_SCANCODE_INTERNATIONAL6)), // Give a better name?
                value("DIK_INTERNATIONAL7",         int(SDL_SCANCODE_INTERNATIONAL7)), // Give a better name?
                value("DIK_INTERNATIONAL8",         int(SDL_SCANCODE_INTERNATIONAL8)), // Give a better name?
                value("DIK_INTERNATIONAL9",         int(SDL_SCANCODE_INTERNATIONAL9)), // Give a better name?

                value("DIK_HANGUL",                 int(SDL_SCANCODE_LANG1)),
                value("DIK_HANJA",                  int(SDL_SCANCODE_LANG2)),
                value("DIK_KATAKANA",               int(SDL_SCANCODE_LANG3)),
                value("DIK_HIRAGANA",               int(SDL_SCANCODE_LANG4)),
                value("DIK_ZENHANKAKU",             int(SDL_SCANCODE_LANG5)),
                value("DIK_LANG6",                  int(SDL_SCANCODE_LANG6)), // Give a better name?
                value("DIK_LANG7",                  int(SDL_SCANCODE_LANG7)), // Give a better name?
                value("DIK_LANG8",                  int(SDL_SCANCODE_LANG8)), // Give a better name?
                value("DIK_LANG9",                  int(SDL_SCANCODE_LANG9)), // Give a better name?

                value("DIK_ALTERASE",               int(SDL_SCANCODE_ALTERASE)),
                value("DIK_CANCEL",                 int(SDL_SCANCODE_CANCEL)),
                value("DIK_CLEAR",                  int(SDL_SCANCODE_CLEAR)),
                value("DIK_PRIOR",                  int(SDL_SCANCODE_PRIOR)),
                value("DIK_RETURN2",                int(SDL_SCANCODE_RETURN2)),
                value("DIK_SEPARATOR",              int(SDL_SCANCODE_SEPARATOR)),
                value("DIK_OUT",                    int(SDL_SCANCODE_OUT)),
                value("DIK_OPER",                   int(SDL_SCANCODE_OPER)),
                value("DIK_CLEARAGAIN",             int(SDL_SCANCODE_CLEARAGAIN)),
                value("DIK_CRSEL",                  int(SDL_SCANCODE_CRSEL)),
                value("DIK_EXSEL",                  int(SDL_SCANCODE_EXSEL)),

                value("DIK_NUMPAD_00",              int(SDL_SCANCODE_KP_00)),
                value("DIK_NUMPAD_000",             int(SDL_SCANCODE_KP_000)),
                value("DIK_THOUSANDSSEPARATOR",     int(SDL_SCANCODE_THOUSANDSSEPARATOR)),
                value("DIK_DECIMALSEPARATOR",       int(SDL_SCANCODE_DECIMALSEPARATOR)),
                value("DIK_CURRENCYUNIT",           int(SDL_SCANCODE_CURRENCYUNIT)),
                value("DIK_CURRENCYSUBUNIT",        int(SDL_SCANCODE_CURRENCYSUBUNIT)),

                value("DIK_NUMPAD_LEFTPAREN",       int(SDL_SCANCODE_KP_LEFTPAREN)),
                value("DIK_NUMPAD_RIGHTPAREN",      int(SDL_SCANCODE_KP_RIGHTPAREN)),
                value("DIK_NUMPAD_LEFTBRACE",       int(SDL_SCANCODE_KP_LEFTBRACE)),
                value("DIK_NUMPAD_RIGHTBRACE",      int(SDL_SCANCODE_KP_RIGHTBRACE)),
                value("DIK_NUMPAD_TAB",             int(SDL_SCANCODE_KP_TAB)),
                value("DIK_NUMPAD_BACKSPACE",       int(SDL_SCANCODE_KP_BACKSPACE)),

                value("DIK_NUMPAD_A",               int(SDL_SCANCODE_KP_A)),
                value("DIK_NUMPAD_B",               int(SDL_SCANCODE_KP_B)),
                value("DIK_NUMPAD_C",               int(SDL_SCANCODE_KP_C)),
                value("DIK_NUMPAD_D",               int(SDL_SCANCODE_KP_D)),
                value("DIK_NUMPAD_E",               int(SDL_SCANCODE_KP_E)),
                value("DIK_NUMPAD_F",               int(SDL_SCANCODE_KP_F)),

                value("DIK_NUMPAD_XOR",             int(SDL_SCANCODE_KP_XOR)),

                value("DIK_NUMPAD_POWER",           int(SDL_SCANCODE_KP_POWER)),
                value("DIK_NUMPAD_PERCENT",         int(SDL_SCANCODE_KP_PERCENT)),

                value("DIK_NUMPAD_LESS",            int(SDL_SCANCODE_KP_LESS)),
                value("DIK_NUMPAD_GREATER",         int(SDL_SCANCODE_KP_GREATER)),

                value("DIK_NUMPAD_AMPERSAND",       int(SDL_SCANCODE_KP_AMPERSAND)),
                value("DIK_NUMPAD_DBLAMPERSAND",    int(SDL_SCANCODE_KP_DBLAMPERSAND)),

                value("DIK_NUMPAD_VERTICALBAR",     int(SDL_SCANCODE_KP_VERTICALBAR)),
                value("DIK_NUMPAD_DBLVERTICALBAR",  int(SDL_SCANCODE_KP_DBLVERTICALBAR)),

                value("DIK_NUMPAD_COLON",           int(SDL_SCANCODE_KP_COLON)),
                value("DIK_NUMPAD_HASH",            int(SDL_SCANCODE_KP_HASH)),
                value("DIK_NUMPAD_SPACE",           int(SDL_SCANCODE_KP_SPACE)),
                value("DIK_NUMPAD_AT",              int(SDL_SCANCODE_KP_AT)),
                value("DIK_NUMPAD_EXCLAM",          int(SDL_SCANCODE_KP_EXCLAM)),

                value("DIK_NUMPAD_MEMSTORE",        int(SDL_SCANCODE_KP_MEMSTORE)),
                value("DIK_NUMPAD_MEMRECALL",       int(SDL_SCANCODE_KP_MEMRECALL)),
                value("DIK_NUMPAD_MEMCLEAR",        int(SDL_SCANCODE_KP_MEMCLEAR)),
                value("DIK_NUMPAD_MEMADD",          int(SDL_SCANCODE_KP_MEMADD)),
                value("DIK_NUMPAD_MEMSUBTRACT",     int(SDL_SCANCODE_KP_MEMSUBTRACT)),
                value("DIK_NUMPAD_MEMMULTIPLY",     int(SDL_SCANCODE_KP_MEMMULTIPLY)),
                value("DIK_NUMPAD_MEMDIVIDE",       int(SDL_SCANCODE_KP_MEMDIVIDE)),

                value("DIK_NUMPAD_PLUSMINUS",       int(SDL_SCANCODE_KP_PLUSMINUS)),
                value("DIK_NUMPAD_CLEAR",           int(SDL_SCANCODE_KP_CLEAR)),
                value("DIK_NUMPAD_CLEARENTRY",      int(SDL_SCANCODE_KP_CLEARENTRY)),
                value("DIK_NUMPAD_BINARY",          int(SDL_SCANCODE_KP_BINARY)),
                value("DIK_NUMPAD_OCTAL",           int(SDL_SCANCODE_KP_OCTAL)),
                value("DIK_NUMPAD_DECIMAL",         int(SDL_SCANCODE_KP_DECIMAL)),
                value("DIK_NUMPAD_HEXADECIMAL",     int(SDL_SCANCODE_KP_HEXADECIMAL)),

                value("DIK_LCONTROL",               int(SDL_SCANCODE_LCTRL)),
                value("DIK_LSHIFT",                 int(SDL_SCANCODE_LSHIFT)),
                value("DIK_LMENU",                  int(SDL_SCANCODE_LALT)),
                value("DIK_LWIN",                   int(SDL_SCANCODE_LGUI)),
                value("DIK_RCONTROL",               int(SDL_SCANCODE_RCTRL)),
                value("DIK_RSHIFT",                 int(SDL_SCANCODE_RSHIFT)),
                value("DIK_RMENU",                  int(SDL_SCANCODE_RALT)),
                value("DIK_RWIN",                   int(SDL_SCANCODE_RGUI)),

                value("DIK_MODE",                   int(SDL_SCANCODE_MODE)),

                value("DIK_AUDIONEXT",              int(SDL_SCANCODE_AUDIONEXT)),
                value("DIK_AUDIOPREV",              int(SDL_SCANCODE_AUDIOPREV)),
                value("DIK_AUDIOSTOP",              int(SDL_SCANCODE_AUDIOSTOP)),
                value("DIK_AUDIOPLAY",              int(SDL_SCANCODE_AUDIOPLAY)),
                value("DIK_AUDIOMUTE",              int(SDL_SCANCODE_AUDIOMUTE)),

                value("DIK_NUMPAD_MEDIASELECT",     int(SDL_SCANCODE_MEDIASELECT)),
                value("DIK_NUMPAD_WWW",             int(SDL_SCANCODE_WWW)),
                value("DIK_NUMPAD_MAIL",            int(SDL_SCANCODE_MAIL)),
                value("DIK_NUMPAD_CALCULATOR",      int(SDL_SCANCODE_CALCULATOR)),
                value("DIK_NUMPAD_COMPUTER",        int(SDL_SCANCODE_COMPUTER)),

                value("DIK_NUMPAD_AC_SEARCH",       int(SDL_SCANCODE_AC_SEARCH)),
                value("DIK_NUMPAD_AC_HOME",         int(SDL_SCANCODE_AC_HOME)),
                value("DIK_NUMPAD_AC_BACK",         int(SDL_SCANCODE_AC_BACK)),
                value("DIK_NUMPAD_AC_FORWARD",      int(SDL_SCANCODE_AC_FORWARD)),
                value("DIK_NUMPAD_AC_STOP",         int(SDL_SCANCODE_AC_STOP)),
                value("DIK_NUMPAD_AC_REFRESH",      int(SDL_SCANCODE_AC_REFRESH)),
                value("DIK_NUMPAD_AC_BOOKMARKS",    int(SDL_SCANCODE_AC_BOOKMARKS)),

                value("DIK_BRIGHTNESSDOWN",         int(SDL_SCANCODE_BRIGHTNESSDOWN)),
                value("DIK_BRIGHTNESSUP",           int(SDL_SCANCODE_BRIGHTNESSUP)),
                value("DIK_DISPLAYSWITCH",          int(SDL_SCANCODE_DISPLAYSWITCH)),

                value("DIK_KBDILLUMTOGGLE)",        int(SDL_SCANCODE_KBDILLUMTOGGLE)),
                value("DIK_KBDILLUMDOWN",           int(SDL_SCANCODE_KBDILLUMDOWN)),
                value("DIK_KBDILLUMUP",             int(SDL_SCANCODE_KBDILLUMUP)),

                value("DIK_EJECT",                  int(SDL_SCANCODE_EJECT)),
                value("DIK_SLEEP",                  int(SDL_SCANCODE_SLEEP)),

                value("DIK_APP1",                   int(SDL_SCANCODE_APP1)),
                value("DIK_APP2",                   int(SDL_SCANCODE_APP2)),

                value("MOUSE_1",                    int(MOUSE_1)),
                value("MOUSE_2",                    int(MOUSE_2)),
                value("MOUSE_3",                    int(MOUSE_3)),
                value("MOUSE_4",                    int(MOUSE_4)),
                value("MOUSE_5",                    int(MOUSE_5)),

                value("GAMEPAD_A",                  int(XR_CONTROLLER_BUTTON_A)),
                value("GAMEPAD_B",                  int(XR_CONTROLLER_BUTTON_B)),
                value("GAMEPAD_X",                  int(XR_CONTROLLER_BUTTON_X)),
                value("GAMEPAD_Y",                  int(XR_CONTROLLER_BUTTON_Y)),
                value("GAMEPAD_BACK",               int(XR_CONTROLLER_BUTTON_BACK)),
                value("GAMEPAD_GUIDE",              int(XR_CONTROLLER_BUTTON_GUIDE)),
                value("GAMEPAD_START",              int(XR_CONTROLLER_BUTTON_START)),
                value("GAMEPAD_LEFTSTICK",          int(XR_CONTROLLER_BUTTON_LEFTSTICK)),
                value("GAMEPAD_RIGHTSTICK",         int(XR_CONTROLLER_BUTTON_RIGHTSTICK)),
                value("GAMEPAD_LEFTSHOULDER",       int(XR_CONTROLLER_BUTTON_LEFTSHOULDER)),
                value("GAMEPAD_RIGHTSHOULDER",      int(XR_CONTROLLER_BUTTON_RIGHTSHOULDER)),
                value("GAMEPAD_DPAD_UP",            int(XR_CONTROLLER_BUTTON_DPAD_UP)),
                value("GAMEPAD_DPAD_DOWN",          int(XR_CONTROLLER_BUTTON_DPAD_DOWN)),
                value("GAMEPAD_DPAD_LEFT",          int(XR_CONTROLLER_BUTTON_DPAD_LEFT)),
                value("GAMEPAD_DPAD_RIGHT",         int(XR_CONTROLLER_BUTTON_DPAD_RIGHT)),
                value("GAMEPAD_DPAD_MISC1",         int(XR_CONTROLLER_BUTTON_MISC1)),
                value("GAMEPAD_DPAD_PADDLE1",       int(XR_CONTROLLER_BUTTON_PADDLE1)),
                value("GAMEPAD_DPAD_PADDLE2",       int(XR_CONTROLLER_BUTTON_PADDLE2)),
                value("GAMEPAD_DPAD_PADDLE3",       int(XR_CONTROLLER_BUTTON_PADDLE3)),
                value("GAMEPAD_DPAD_PADDLE4",       int(XR_CONTROLLER_BUTTON_PADDLE4)),
                value("GAMEPAD_DPAD_TOUCHPAD",      int(XR_CONTROLLER_BUTTON_TOUCHPAD))
        ]
    ];
});
// clang-format on
