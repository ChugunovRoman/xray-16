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
#ifndef LUABIND_NO_EXCEPTIONS
    script_action_try_void("initialize", [&]() { luabind::call_member<void>(this, "initialize"); });
#else
    luabind::call_member<void>(this, "initialize");
#endif
}

void CScriptActionWrapper::initialize_static(CScriptActionBase* action) { action->CScriptActionBase::initialize(); }

void CScriptActionWrapper::execute()
{
    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::ScriptActionUpdate);
#ifndef LUABIND_NO_EXCEPTIONS
    script_action_try_void("execute", [&]() { luabind::call_member<void>(this, "execute"); });
#else
    luabind::call_member<void>(this, "execute");
#endif
}

void CScriptActionWrapper::execute_static(CScriptActionBase* action) { action->CScriptActionBase::execute(); }
void CScriptActionWrapper::finalize()
{
#ifndef LUABIND_NO_EXCEPTIONS
    script_action_try_void("finalize", [&]() { luabind::call_member<void>(this, "finalize"); });
#else
    luabind::call_member<void>(this, "finalize");
#endif
}
void CScriptActionWrapper::finalize_static(CScriptActionBase* action) { action->CScriptActionBase::finalize(); }

CScriptActionWrapper::edge_value_type CScriptActionWrapper::weight(const CSConditionState& condition0, const CSConditionState& condition1) const
{
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
