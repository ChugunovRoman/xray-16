#include "StdAfx.h"
#include "xrAICore/pch.hpp"
#include "UICellItem.h"
#include "xrScriptEngine/ScriptExporter.hpp"

// clang-format off
void CUICellItem::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CUICellItem, CUIStatic>("CUICellItem")
            .def(constructor<>())
            .def_readonly("name", &CUICellItem::m_section_id)
            .def_readwrite("prop", &CUICellItem::m_fSpawnProp)
    ];
}
// clang-format on
