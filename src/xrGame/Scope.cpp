#include "pch_script.h"
#include "Scope.h"
#include "Silencer.h"
#include "GrenadeLauncher.h"
#include "xrScriptEngine/ScriptExporter.hpp"

CScope::CScope() {}
CScope::~CScope() {}

void CScope::Load(LPCSTR section)
{
    inherited::Load(section);

    if (pSettings->line_exist(section, "addon_type"))
        m_addon_type = pSettings->r_string(section, "addon_type");
}

SCRIPT_EXPORT(CScope, (CGameObject),
{
    using namespace luabind;

    module(luaState)
    [
        class_<CScope, CGameObject>("CScope")
            .def(constructor<>()),

        class_<CSilencer, CGameObject>("CSilencer")
            .def(constructor<>()),

        class_<CGrenadeLauncher, CGameObject>("CGrenadeLauncher")
            .def(constructor<>())
    ];
});
