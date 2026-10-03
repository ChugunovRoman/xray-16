////////////////////////////////////////////////////////////////////////////
//	Module 		: script_action_wrapper.h
//	Created 	: 19.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script action wrapper
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "script_action_wrapper.h"
#include "script_game_object.h"
#include "ai_space.h"
#include "xrScriptEngine/script_engine.hpp"
#include "npc_cpp_profile.h"
#include "performance_cvars.h"
#ifndef LUABIND_NO_EXCEPTIONS
#include "luabind/error.hpp"
#endif

namespace
{
#ifndef LUABIND_NO_EXCEPTIONS
void script_action_log_fail(pcstr method, pcstr what)
{
    if (!GEnv.ScriptEngine || !method)
        return;
    pcstr const detail = (what && *what) ? what : "(empty exception message)";
    GEnv.ScriptEngine->script_log(LuaMessageType::Error, "CScriptActionWrapper::%s: %s", method, detail);
    GEnv.ScriptEngine->print_stack();
}

template <typename Fn>
void script_action_try_void(pcstr method, Fn&& fn)
{
    try
    {
        fn();
    }
    catch (const luabind::error& e)
    {
        script_action_log_fail(method, e.what());
    }
    catch (const std::exception& e)
    {
        script_action_log_fail(method, e.what());
    }
    catch (...)
    {
        script_action_log_fail(method, "unknown C++ exception");
    }
}

template <typename R, typename Fn>
R script_action_try_r(pcstr method, R default_v, Fn&& fn)
{
    try
    {
        return fn();
    }
    catch (const luabind::error& e)
    {
        script_action_log_fail(method, e.what());
        return default_v;
    }
    catch (const std::exception& e)
    {
        script_action_log_fail(method, e.what());
        return default_v;
    }
    catch (...)
    {
        script_action_log_fail(method, "unknown C++ exception");
        return default_v;
    }
}
#endif
} // namespace

void CScriptActionWrapper::setup(CScriptGameObject* object, CPropertyStorage* storage)
{
#ifndef LUABIND_NO_EXCEPTIONS
    script_action_try_void("setup", [&]() { luabind::call_member<void>(this, "setup", object, storage); });
#else
    luabind::call_member<void>(this, "setup", object, storage);
#endif
}

void CScriptActionWrapper::setup_static(CScriptActionBase* action, CScriptGameObject* object, CPropertyStorage* storage)
{
    action->CScriptActionBase::setup(object, storage);
}

void CScriptActionWrapper::initialize()
{
    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::ScriptActionInitialize);
    const u64 start = npc_cpp_profile::enabled() ? CPU::QPC() : 0; // script_action_top by the name of the action
#ifndef LUABIND_NO_EXCEPTIONS
    script_action_try_void("initialize", [&]() { luabind::call_member<void>(this, "initialize"); });
#else
    luabind::call_member<void>(this, "initialize");
#endif
    if (start)
        npc_cpp_profile::add_script_action(m_action_name, "initialize", CPU::QPC() - start);
}

void CScriptActionWrapper::initialize_static(CScriptActionBase* action) { action->CScriptActionBase::initialize(); }

void CScriptActionWrapper::execute()
{
    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::ScriptActionUpdate);
    const u64 start = npc_cpp_profile::enabled() ? CPU::QPC() : 0;
#ifndef LUABIND_NO_EXCEPTIONS
    script_action_try_void("execute", [&]() { luabind::call_member<void>(this, "execute"); });
#else
    luabind::call_member<void>(this, "execute");
#endif
    if (start)
        npc_cpp_profile::add_script_action(m_action_name, "execute", CPU::QPC() - start);
}

void CScriptActionWrapper::execute_static(CScriptActionBase* action) { action->CScriptActionBase::execute(); }
void CScriptActionWrapper::finalize()
{
    const u64 start = npc_cpp_profile::enabled() ? CPU::QPC() : 0;
#ifndef LUABIND_NO_EXCEPTIONS
    script_action_try_void("finalize", [&]() { luabind::call_member<void>(this, "finalize"); });
#else
    luabind::call_member<void>(this, "finalize");
#endif
    if (start)
        npc_cpp_profile::add_script_action(m_action_name, "finalize", CPU::QPC() - start);
}
void CScriptActionWrapper::finalize_static(CScriptActionBase* action) { action->CScriptActionBase::finalize(); }

bool CScriptActionWrapper::weight_overridden() const
{
    if (m_weight_override < 0)
    {
        // self["weight"] through the metatable of the instance: a Lua function when a script class defines it,
        // the C function of the binding (the default of action_base) otherwise
        const auto& self = luabind::detail::wrap_access::ref(*this);
        lua_State* L = self.state();
        bool lua_function = true; // on any doubt the Lua call stays
        if (L)
        {
            const int top = lua_gettop(L);
            self.get(L);
            if (!lua_isnil(L, -1))
            {
                lua_pushstring(L, "weight");
                lua_gettable(L, -2);
                lua_function = lua_type(L, -1) == LUA_TFUNCTION && !lua_iscfunction(L, -1);
            }
            lua_settop(L, top);
        }
        m_weight_override = lua_function ? 1 : 0;
    }
    return m_weight_override == 1;
}

CScriptActionWrapper::edge_value_type CScriptActionWrapper::weight(const CSConditionState& condition0, const CSConditionState& condition1) const
{
    // No script action overrides weight() today: the default is answered here instead of a Lua round trip per edge
    if (npc_perf_script_action_weight_lookup && !weight_overridden())
        return CScriptActionBase::weight(condition0, condition1);
#ifndef LUABIND_NO_EXCEPTIONS
    auto _weight = script_action_try_r(
        "weight", min_weight(),
        [&]() {
            return luabind::call_member<edge_value_type>(
                const_cast<CScriptActionWrapper*>(this), "weight", condition0, condition1);
        });
#else
    auto _weight = luabind::call_member<edge_value_type>(
        const_cast<CScriptActionWrapper*>(this), "weight", condition0, condition1);
#endif
    if (_weight < min_weight())
    {
        GEnv.ScriptEngine->script_log(LuaMessageType::Error, "Weight is less than effect count! It is corrected from %d to %d", _weight, min_weight());
        _weight = min_weight();
    }
    return _weight;
}

CScriptActionWrapper::edge_value_type CScriptActionWrapper::weight_static(CScriptActionBase* action, const CSConditionState& condition0, const CSConditionState& condition1)
{
    return ((const CScriptActionWrapper*)action)->CScriptActionBase::weight(condition0, condition1);
}
