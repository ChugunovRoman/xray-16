#include "stdafx.h"

#include "XR_IOConsole.h"
#include "xr_ioc_cmd.h"

#include "xrScriptEngine/script_space.hpp"
#include "xrScriptEngine/script_wrapper_cache.hpp"

// The Lua wrapper cache (xrScriptEngine/script_wrapper_cache.hpp): get_console() gives one wrapper per Lua state. Only the console itself (Console) is
// cached, the slot remembers it (created before the Lua states, deleted at exit). CConsole has no luabind::wrap_base
// class (no Lua-derived consoles), so the generic get_back_reference never finds anything for it. Only this file
// passes CConsole* to Lua (get_console()): the specialization is declared and defined here, before the registration.
static xray::script_wrapper_cache::ObjectSlot s_console_lua_wrapper;

namespace luabind
{
template <>
bool get_back_reference<CConsole*>(lua_State* L, CConsole* const& x)
{
    using namespace xray::script_wrapper_cache;
    if (x != Console)
        return false;
    if (Push(L, s_console_lua_wrapper, x))
        return true;
    luabind::detail::make_pointer_instance(L, x);
    Store(L, s_console_lua_wrapper);
    return true;
}
} // namespace luabind

void CConsole::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        def("get_console", +[]()
        {
            return Console;
        }),

        class_<CConsole>("CConsole")
            .def("execute", &CConsole::Execute)
            .def("execute_script", &CConsole::ExecuteScript)
            .def("show", &CConsole::Show)
            .def("hide", &CConsole::Hide)

            .def("get_string", &CConsole::GetString)
            .def("get_integer", +[](const CConsole* self, pcstr cmd)
            {
                int min = 0, max = 0;
                const int val = self->GetInteger(cmd, min, max);
                return val;
            })
            .def("get_bool", +[](const CConsole* self, pcstr cmd)
            {
                return self->GetBool(cmd);
            })
            .def("get_float", +[](const CConsole* self, pcstr cmd)
            {
                float min = 0.0f, max = 0.0f;
                const float val = self->GetFloat(cmd, min, max);
                return val;
            })
            .def("get_token", &CConsole::GetToken)
            .def("execute_deferred", +[](CConsole*, pcstr string_to_execute)
            {
                Engine.Event.Defer("KERNEL:console", size_t(xr_strdup(string_to_execute)));
            }),

        def("renderer_allow_override", +[]()
        {
            return renderer_allow_override;
        })
    ];
}
