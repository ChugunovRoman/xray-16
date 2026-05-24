#include "pch_script.h"

#include "UIFactionEditorMapWnd.h"

void CUIFactionEditorMapWnd::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CUIFactionEditorMapWnd, CUIWindow>("CUIFactionEditorMapWnd")
            .def(constructor<>())
            .def("Init", &CUIFactionEditorMapWnd::Init)
            .def("SetSpawnName", &CUIFactionEditorMapWnd::SetSpawnName)
            .def("Reload", &CUIFactionEditorMapWnd::Reload)
            .def("HasPendingClick", &CUIFactionEditorMapWnd::HasPendingClick)
            .def("GetLastClickedId", &CUIFactionEditorMapWnd::GetLastClickedId)
            .def("GetLastClickType", &CUIFactionEditorMapWnd::GetLastClickType)
            .def("ClearPendingClick", &CUIFactionEditorMapWnd::ClearPendingClick)
            .def("GetPointLevelName", &CUIFactionEditorMapWnd::GetPointLevelName)
            .def("GetPointSmartName", &CUIFactionEditorMapWnd::GetPointSmartName)
            .def("GetPointSectionName", &CUIFactionEditorMapWnd::GetPointSectionName)
            .def("GetPointHintText", &CUIFactionEditorMapWnd::GetPointHintText)
            .def("GetPointDisplayName", &CUIFactionEditorMapWnd::GetPointDisplayName)
            .def("GetPointSmartType", &CUIFactionEditorMapWnd::GetPointSmartType)
            .def("GetPointOwnerFaction", &CUIFactionEditorMapWnd::GetPointOwnerFaction)
            .def("GetPointIconTexture", &CUIFactionEditorMapWnd::GetPointIconTexture)
    ];
}
