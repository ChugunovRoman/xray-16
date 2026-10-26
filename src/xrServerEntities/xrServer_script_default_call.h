////////////////////////////////////////////////////////////////////////////
//  Module      : xrServer_script_default_call.h
//  Description : plans/lua_to_cpp/33-gw-eoc-alife-switch.md section 10 (N3): a const method of a luabind wrapper of a
//                server class called the way luabind::call_member calls it, except that the C++ default is called
//                directly when the Lua class of the object does not override the method
////////////////////////////////////////////////////////////////////////////

#pragma once

#include <type_traits>
#include <luabind/wrapper_base.hpp>
#include <luabind/function.hpp>
#include <luabind/detail/call_member.hpp>

namespace xr_luabind
{
// How call_member_or_default went: the C++ default directly (the method is not overridden), a Lua function (the
// override), or luabind::call_member (an unusual case: luabind decides as before)
enum class EDefaultCall : unsigned char
{
    native,
    lua,
    luabind,
};

// luabind::call_member<R>(self, name) of a method without arguments, the same steps (wrap_base::call and
// do_call_member_selection, luabind/src/wrapper_base.cpp):
// 1. the self reference of the object, lua_gettable(self, name) - the lookup luabind does (__index of the instance: its
//    own table, then the table of its Lua class, which holds the members of the base classes copied when the class
//    was made, the Lua overrides, and whatever Lua assigned to the class later);
// 2. luabind: when the result is a luabind function, the default implementation of the Lua class is called (its
//    default table, lua_pcall, the overload resolution, the conversions) - for a wrapper of the engine that is
//    W<T>::name_static, i.e. T::name() of the object (DEFINE_LUA_WRAPPER_CONST_METHOD_0 and the luabind_virtual_*
//    macros of xrServer_script_macroses.h register it for every class with that wrapper);
//    here: when the result is a member function a C++ class registered (luabind tags it as not default:
//    is_luabind_function(L, -1, false)) - a method nobody in Lua overrode - fallback(object) is called directly, the
//    same T::name() without Lua. Any other luabind function (tagged default: a C++ function Lua assigned to the class
//    or to the object) or nil (no method) - luabind::call_member from scratch, as before;
// 3. a Lua function (the override) - called on the same stack exactly as wrap_base::call does: no second lookup.
// Nothing is cached: the answer is looked up on every call, so a Lua class or object changed later, or a new script
// engine (new game, load: other class_rep at the same addresses), cannot leave a stale answer.
// The difference from luabind is only that the C++ default does not run inside lua_pcall: the engine defaults of the
// four methods it is used for (can_switch_online / can_switch_offline / keep_saved_data_anyway / used_ai_locations)
// read flags and fields and do not call Lua (a squad asks its members through the same wrappers).
template <typename R, typename T>
R call_member_or_default(
    const luabind::wrap_base* self, const T* object, const char* name, R (*fallback)(const T*), EDefaultCall& how)
{
    const luabind::wrapped_self_t& self_ref = luabind::detail::wrap_access::ref(*self);
    lua_State* L = self_ref.state();
    self_ref.get(L);
    if (lua_type(L, -1) != LUA_TUSERDATA) // no Lua object behind the wrapper: whatever luabind does with it
    {
        lua_pop(L, 1);
        how = EDefaultCall::luabind;
        return luabind::call_member<R>(self, name);
    }
    lua_pushstring(L, name);
    lua_gettable(L, -2);
    lua_replace(L, -2); // the method in place of the self reference, as do_call_member_selection
    // a luabind function is a C closure: a Lua function (the usual override) skips both tag checks
    const bool c_function = lua_iscfunction(L, -1) != 0;
    if (c_function && luabind::detail::is_luabind_function(L, -1, false))
    {
        lua_pop(L, 1);
        how = EDefaultCall::native;
        return fallback(object);
    }
    if (lua_isnil(L, -1) || (c_function && luabind::detail::is_luabind_function(L, -1, true)))
    {
        lua_pop(L, 1);
        how = EDefaultCall::luabind;
        return luabind::call_member<R>(self, name);
    }
    how = EDefaultCall::lua;
    self_ref.get(L); // the self reference as the first argument; pcall pops the function and self
    return luabind::detail::call_member_impl<R>(L, std::is_void<R>(), luabind::meta::index_range<1, 1>());
}
} // namespace xr_luabind
