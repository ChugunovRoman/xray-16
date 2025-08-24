#include "StdAfx.h"
#include "xrAICore/pch.hpp"
#include "UICellItem.h"
#include "xrScriptEngine/ScriptExporter.hpp"

// clang-format off
SCRIPT_EXPORT(CUICellItem, (CUIStatic),
{
    using namespace luabind;
    using namespace luabind::policy;

    module(luaState)
    [
        class_<CUICellItem, CUIStatic>("CUICellItem")
            .def(constructor<>())
            .def_readwrite("prop", &CUICellItem::m_fSpawnProp)
            .def_readwrite("points", &CUICellItem::m_iPoints)
            .def_readwrite("allow", &CUICellItem::m_bAllow)
    ];
});
// clang-format on
