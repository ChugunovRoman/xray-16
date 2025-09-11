#include "pch_script.h"

#include "MainMenu.h"

#include "login_manager.h"
#include "account_manager.h"
#include "profile_store.h"

#include "xrUICore/ComboBox/UIComboBox.h"

// Including CScriptXmlInit to prevent it being optimized out by the linker
#include "ScriptXMLInit.h" // don't remove!

#include "ui/ServerList.h"

#include "ui/UIMapList.h"
#include "ui/UIScriptWnd.h"
#include "ui/UIMapInfo.h"
#include "ui/UIMessageBoxEx.h"
#include "ui/UIMMShniaga.h"
#include "ui/UISleepStatic.h"

void CDialogHolder::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CDialogHolder>("CDialogHolder")
            .def("TopInputReceiver", &CDialogHolder::TopInputReceiver)
            .def("MainInputReceiver", &CDialogHolder::TopInputReceiver)
            .def("SetTopInputReceiver", &CDialogHolder::SetMainInputReceiver)
            .def("SetMainInputReceiver", &CDialogHolder::SetMainInputReceiver)
            .def("start_stop_menu", &CDialogHolder::StartStopMenu)
            .def("AddDialogToRender", &CDialogHolder::AddDialogToRender)
            .def("RemoveDialogToRender", &CDialogHolder::RemoveDialogToRender)
    ];
}

void CUIDialogWnd::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CUIDialogWnd, CUIWindow>("CUIDialogWnd")
            .def("ShowDialog", &CUIDialogWnd::ShowDialog)
            .def("HideDialog", &CUIDialogWnd::HideDialog)
            .def("GetHolder", &CUIDialogWnd::GetHolder)
            .def("SetHolder", &CUIDialogWnd::SetHolder),

        class_<CUIMessageBoxEx, CUIDialogWnd>("CUIMessageBoxEx")
            .def(constructor<>())
            .def("Init", &CUIMessageBoxEx::InitMessageBox)
            .def("InitMessageBox", &CUIMessageBoxEx::InitMessageBox)
            .def("SetText", &CUIMessageBoxEx::SetText)
            .def("GetHost", &CUIMessageBoxEx::GetHost)
            .def("GetPassword", &CUIMessageBoxEx::GetPassword)
    ];
}

void CMainMenu::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CGameFont>("CGameFont")
            .enum_("EAligment")
            [
                value("alLeft", int(CGameFont::alLeft)),
                value("alRight", int(CGameFont::alRight)),
                value("alCenter", int(CGameFont::alCenter))
            ],

        class_<Patch_Dawnload_Progress>("Patch_Dawnload_Progress")
            .def("GetInProgress", &Patch_Dawnload_Progress::GetInProgress)
            .def("GetStatus", &Patch_Dawnload_Progress::GetStatus)
            .def("GetFlieName", &Patch_Dawnload_Progress::GetFlieName)
            .def("GetProgress", &Patch_Dawnload_Progress::GetProgress),

        class_<CMainMenu, CDialogHolder>("CMainMenu")
            .def("GetPatchProgress", &CMainMenu::GetPatchProgress)
            .def("CancelDownload", &CMainMenu::CancelDownload)
            .def("ValidateCDKey", &CMainMenu::ValidateCDKey)
            .def("GetGSVer", &CMainMenu::GetGSVer)
            .def("GetCDKey", &CMainMenu::GetCDKeyFromRegistry)
            .def("GetPlayerName", &CMainMenu::GetPlayerName)
            .def("GetDemoInfo", &CMainMenu::GetDemoInfo)
            .def("GetLoginMngr", &CMainMenu::GetLoginMngr)
            .def("GetAccountMngr", &CMainMenu::GetAccountMngr)
            .def("GetProfileStore", &CMainMenu::GetProfileStore),

        class_<CUIMMShniaga, CUIWindow>("CUIMMShniaga")
            .enum_("enum_page_id")
            [
                value("epi_main", CUIMMShniaga::epi_main),
                value("epi_new_game", CUIMMShniaga::epi_new_game),
                value("epi_new_network_game", CUIMMShniaga::epi_new_network_game)
            ]
            .def("SetVisibleMagnifier", &CUIMMShniaga::SetVisibleMagnifier)
            .def("SetPage", &CUIMMShniaga::SetPage)
            .def("ShowPage", &CUIMMShniaga::ShowPage),

        class_<SServerFilters>("SServerFilters")
            .def(constructor<>())
            .def_readwrite("empty", &SServerFilters::empty)
            .def_readwrite("full", &SServerFilters::full)
            .def_readwrite("with_pass", &SServerFilters::with_pass)
            .def_readwrite("without_pass", &SServerFilters::without_pass)
            .def_readwrite("without_ff", &SServerFilters::without_ff)
            .def_readwrite("with_battleye", &SServerFilters::with_battleye)
            .def_readwrite("listen_servers", &SServerFilters::listen_servers),

        class_<CServerList, CUIWindow>("CServerList")
            .def(constructor<>())
            .enum_("enum_connect_errcode")
            [
                value("ece_unique_nick_not_registred", int(ece_unique_nick_not_registred)),
                value("ece_unique_nick_expired", int(ece_unique_nick_expired))
            ]
            .def("SetConnectionErrCb", &CServerList::SetConnectionErrCb)
            .def("ConnectToSelected", &CServerList::ConnectToSelected)
            .def("SetFilters", &CServerList::SetFilters)
            .def("SetPlayerName", &CServerList::SetPlayerName)
            .def("RefreshList", &CServerList::RefreshGameSpyList)
            .def("RefreshQuick", &CServerList::RefreshQuick)
            .def("ShowServerInfo", &CServerList::ShowServerInfo)
            .def("NetRadioChanged", &CServerList::NetRadioChanged)
            .def("SetSortFunc", &CServerList::SetSortFunc),

        class_<CUIMapList, CUIWindow>("CUIMapList")
            .def(constructor<>())
            .def("SetWeatherSelector", &CUIMapList::SetWeatherSelector)
            .def("SetModeSelector", &CUIMapList::SetModeSelector)
            .def("OnModeChange", &CUIMapList::OnModeChange)
            .def("LoadMapList", &CUIMapList::LoadMapList)
            .def("SaveMapList", &CUIMapList::SaveMapList)
            .def("GetCommandLine", &CUIMapList::GetCommandLine)
            .def("SetServerParams", &CUIMapList::SetServerParams)
            .def("GetCurGameType", &CUIMapList::GetCurGameType)
            .def("StartDedicatedServer", &CUIMapList::StartDedicatedServer)
            .def("SetMapPic", &CUIMapList::SetMapPic)
            .def("SetMapInfo", &CUIMapList::SetMapInfo)
            .def("ClearList", &CUIMapList::ClearList)
            .def("IsEmpty", &CUIMapList::IsEmpty)
    ];

    module(luaState, "main_menu")
    [
        def("get_main_menu", &MainMenu)
    ];
}

void CUISleepStatic::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CUISleepStatic, CUIStatic>("CUISleepStatic")
            .def(constructor<>())
    ];
}

DEFINE_MIXED_DELEGATE_SCRIPT(connect_error_cb, "connect_error_cb");
