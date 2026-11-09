#include "pch_script.h"

#include "UIFactionEditorMapWnd.h"
#include "script_enum_dummy.h"

bool ui_map_click_has_modifier(u32 mask, u32 flag)
{
    return (mask & flag) != 0;
}

pcstr get_spawn_smart_level_name(pcstr smart_name)
{
    return CSpawnSmartTerrainMapDataSource::GetSharedSmartLevelName(smart_name);
}

// 0 - the data of the smart terrains of all.spawn is not loaded yet (get_spawn_smart_level_name gives ""); another
// value - loaded, it changes when the data is loaded again. A window shown before the data polls it and rebuilds
u32 get_spawn_smart_map_revision()
{
    return CSpawnSmartTerrainMapDataSource::GetSharedRevision();
}

void CUIFactionEditorMapWnd::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<enum_dummy<EUiMapModifier>>("EUiMapModifier")
            .enum_("EUiMapModifier")
            [
                value("None", int(eUiMapModNone)),
                value("LSHIFT", int(eUiMapModLShift)),
                value("RSHIFT", int(eUiMapModRShift)),
                value("LALT", int(eUiMapModLAlt)),
                value("RALT", int(eUiMapModRAlt)),
                value("LCTRL", int(eUiMapModLCtrl)),
                value("RCTRL", int(eUiMapModRCtrl))
            ],

        def("ui_map_click_has_modifier", &ui_map_click_has_modifier),
        def("get_spawn_smart_level_name", &get_spawn_smart_level_name),
        def("get_spawn_smart_map_revision", &get_spawn_smart_map_revision),

        class_<CUIFactionEditorMapWnd, CUIWindow>("CUIFactionEditorMapWnd")
            .def(constructor<>())
            .def("Init", &CUIFactionEditorMapWnd::Init)
            .def("SetSpawnName", &CUIFactionEditorMapWnd::SetSpawnName)
            .def("SetPointVisual", &CUIFactionEditorMapWnd::SetPointVisual)
            .def("Reload", &CUIFactionEditorMapWnd::Reload)
            .def("ReloadPreserveView", &CUIFactionEditorMapWnd::ReloadPreserveView)
            .def("AddSpotClickCallback", &CUIFactionEditorMapWnd::AddSpotClickCallback)
            .def("HasPendingClick", &CUIFactionEditorMapWnd::HasPendingClick)
            .def("GetLastClickedId", &CUIFactionEditorMapWnd::GetLastClickedId)
            .def("GetLastClickType", &CUIFactionEditorMapWnd::GetLastClickType)
            .def("GetLastClickModifiers", &CUIFactionEditorMapWnd::GetLastClickModifiers)
            .def("ClearPendingClick", &CUIFactionEditorMapWnd::ClearPendingClick)
            .def("GetPointLevelName", &CUIFactionEditorMapWnd::GetPointLevelName)
            .def("GetPointSmartName", &CUIFactionEditorMapWnd::GetPointSmartName)
            .def("GetPointSectionName", &CUIFactionEditorMapWnd::GetPointSectionName)
            .def("GetPointHintText", &CUIFactionEditorMapWnd::GetPointHintText)
            .def("GetPointDisplayName", &CUIFactionEditorMapWnd::GetPointDisplayName)
            .def("GetPointSmartType", &CUIFactionEditorMapWnd::GetPointSmartType)
            .def("GetPointOwnerFaction", &CUIFactionEditorMapWnd::GetPointOwnerFaction)
            .def("GetPointIconTexture", &CUIFactionEditorMapWnd::GetPointIconTexture)
            .def("GetLogicalIdBySmartName", &CUIFactionEditorMapWnd::GetLogicalIdBySmartName)
            .def("EnumeratePoints", &CUIFactionEditorMapWnd::EnumeratePoints)
            .def("SetSmartType", &CUIFactionEditorMapWnd::SetSmartType)
            .def("SetPointHintText", &CUIFactionEditorMapWnd::SetPointHintText)
    ];
}
