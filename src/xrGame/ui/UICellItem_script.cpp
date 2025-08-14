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
    ];
});
// clang-format on
