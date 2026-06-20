#include "pch_script.h"

#include "UIFactionVisualGridWnd.h"
#include "xrScriptEngine/ScriptExporter.hpp"

void CUIFactionVisualGridWnd::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CUIFactionVisualGridWnd, CUIWindow>("CUIFactionVisualGridWnd")
            .def(constructor<>())
            .def("Init", &CUIFactionVisualGridWnd::Init)
            .def("Clear", &CUIFactionVisualGridWnd::Clear)
            .def("SetGridDimensions", &CUIFactionVisualGridWnd::SetGridDimensions)
            .def("SetCellSize", &CUIFactionVisualGridWnd::SetCellSize)
            .def("SetCellSpacing", &CUIFactionVisualGridWnd::SetCellSpacing)
            .def("SetGridPadding", &CUIFactionVisualGridWnd::SetGridPadding)
            .def("SetTabBarHeight", &CUIFactionVisualGridWnd::SetTabBarHeight)
            .def("SetTabSpacing", &CUIFactionVisualGridWnd::SetTabSpacing)
            .def("SetPageOffset", &CUIFactionVisualGridWnd::SetPageOffset)
            .def("SetPreviewAngles", &CUIFactionVisualGridWnd::SetPreviewAngles)
            .def("SetPreviewOffset", &CUIFactionVisualGridWnd::SetPreviewOffset)
            .def("SetPreviewCameraDistanceOffset", &CUIFactionVisualGridWnd::SetPreviewCameraDistanceOffset)
            .def("ClearHighlightedModels", &CUIFactionVisualGridWnd::ClearHighlightedModels)
            .def("SetPersistentHighlightColor", &CUIFactionVisualGridWnd::SetPersistentHighlightColor)
            .def("SetHighlightedModels", +[](CUIFactionVisualGridWnd* self, luabind::object item_tbl)
            {
                CUIFactionVisualGridWnd::TModelList models;

                for (luabind::iterator I(item_tbl), E; I != E; ++I)
                {
                    luabind::object value = *I;
                    if (luabind::type(value) != LUA_TSTRING)
                        continue;

                    models.push_back(shared_str(luabind::object_cast<pcstr>(value)));
                }

                self->SetHighlightedModels(models);
            })
            .def("AddModel", &CUIFactionVisualGridWnd::AddModel)
            .def("AddModels", &CUIFactionVisualGridWnd::AddModels)
            .def("AddModelToTab", &CUIFactionVisualGridWnd::AddModelToTab)
            .def("AddModelsToTab", &CUIFactionVisualGridWnd::AddModelsToTab)
            .def("SetModels", &CUIFactionVisualGridWnd::SetModels)
            .def("SetActiveTab", &CUIFactionVisualGridWnd::SetActiveTab)
            .def("SetCurrentTab", &CUIFactionVisualGridWnd::SetCurrentTab)
            .def("SetSelectedModel", &CUIFactionVisualGridWnd::SetSelectedModel)
            .def("GetModelCount", static_cast<u32 (CUIFactionVisualGridWnd::*)() const>(&CUIFactionVisualGridWnd::GetModelCount))
            .def("GetActiveTab", &CUIFactionVisualGridWnd::GetActiveTab)
            .def("GetCurrentTab", &CUIFactionVisualGridWnd::GetCurrentTab)
            .def("GetAllTabs", +[](CUIFactionVisualGridWnd* self, lua_State* L)
            {
                luabind::object result = luabind::newtable(L);
                const auto tabs = self->GetAllTabNames();
                for (u32 i = 0; i < tabs.size(); ++i)
                    result[i + 1] = tabs[i].c_str();
                return result;
            })
            .def("GetSelectedModel", &CUIFactionVisualGridWnd::GetSelectedModel)
            .def("GetSelectedTab", &CUIFactionVisualGridWnd::GetSelectedTab)
            .def("GetSelectedCellIndex", &CUIFactionVisualGridWnd::GetSelectedCellIndex)
            .def("HasTab", &CUIFactionVisualGridWnd::HasTab)
            .def("HasModel", &CUIFactionVisualGridWnd::HasModel)
    ];
}
