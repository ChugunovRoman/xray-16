#include "pch_script.h"

#include "xrUICore/Static/UIStatic.h"

#include "UIGameCustom.h"
#include "Level.h"

// The Lua wrapper cache (UIGameCustom.h): the cached wrapper of this UI, made (as luabind does: a pointer_holder, the object not owned) and kept on the first
// pass. luabind's own check first, as the generic get_back_reference does: a UI made by a Lua class is a
// luabind::wrap_base and goes to Lua as its Lua object, never cached (UIGame_custom_script.cpp, the only such class,
// is out of the build now - kept for the day it comes back).
namespace luabind
{
template <>
bool get_back_reference<CUIGameCustom*>(lua_State* L, CUIGameCustom* const& x)
{
#ifndef LUABIND_NO_RTTI
    if (const wrap_base* w = detail::get_back_reference(x))
    {
        detail::wrap_access::ref(*w).get(L);
        return true;
    }
#endif
    using namespace xray::script_wrapper_cache;
    if (Push(L, x->m_lua_wrapper))
        return true;
    luabind::detail::make_pointer_instance(L, x);
    Store(L, x->m_lua_wrapper);
    return true;
}
} // namespace luabind

void CUIGameCustom::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<StaticDrawableWrapper>("StaticDrawableWrapper")
            .def_readwrite("m_endTime", &StaticDrawableWrapper::m_endTime)
            .def("wnd", &StaticDrawableWrapper::wnd),

        class_<CUIGameCustom, CDialogHolder>("CUIGameCustom")
            .def("AddDialogToRender", &CUIGameCustom::AddDialogToRender)
            .def("RemoveDialogToRender", &CUIGameCustom::RemoveDialogToRender)
            .def("AddCustomStatic", +[](CUIGameCustom* self, pcstr id, bool singleInstance)
            {
                return self->AddCustomStatic(id, singleInstance);
            })
            .def("AddCustomStatic", &CUIGameCustom::AddCustomStatic)
            .def("RemoveCustomStatic", &CUIGameCustom::RemoveCustomStatic)
            .def("HideActorMenu", &CUIGameCustom::HideActorMenu)
             //Alundaio
            .def("ShowActorMenu", &CUIGameCustom::ShowActorMenu)
            .def("UpdateActorMenu", &CUIGameCustom::UpdateActorMenu)
            .def("CurrentItemAtCell", &CUIGameCustom::CurrentItemAtCell)
            //-Alundaio
            .def("HidePdaMenu", &CUIGameCustom::HidePdaMenu)
            .def("show_messages", &CUIGameCustom::ShowMessagesWindow)
            .def("hide_messages", &CUIGameCustom::HideMessagesWindow)
            .def("GetCustomStatic", &CUIGameCustom::GetCustomStatic)
            .def("update_fake_indicators", &CUIGameCustom::update_fake_indicators)
            .def("enable_fake_indicators", &CUIGameCustom::enable_fake_indicators),

        def("get_hud", +[]() -> CUIGameCustom* { return CurrentGameUI(); })
    ];
}
