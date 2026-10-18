#include "StdAfx.h"

#include "PHScriptCall.h"
#include "PHCommander.h"

#include "xrEngine/xr_object.h"

namespace
{
// Called under lua_pcall with (object, name): object[name]. A missing method or a failing __index of a luabind class
// instance stays inside the protected call.
int GetFieldProtected(lua_State* L)
{
    lua_settop(L, 2);
    lua_gettable(L, 1);
    return 1;
}

// The file name of a Lua chunk name without its folders: the full path of the scripts folder took the whole Tracy text
// and cut the "(up ...)" part with the real callback off
pcstr ScriptFileName(pcstr src)
{
    pcstr name = src;
    for (pcstr p = src; *p; ++p)
    {
        if (*p == '\\' || *p == '/')
            name = p + 1;
    }
    return name;
}

// "file:line" of the function on the top of the stack of L (popped). A Lua function with a function upvalue
// (a wrapper: AddUniqueCall of _g.script calls its functor_a) adds the first one, the real callback in that case.
void DescribeFunctionOnTop(lua_State* L, string256& out)
{
    xr_strcpy(out, "?");
    if (!lua_isfunction(L, -1))
    {
        lua_pop(L, 1);
        return;
    }
    lua_Debug ar{};
    lua_pushvalue(L, -1);
    if (lua_getinfo(L, ">S", &ar)) // pops the copy
        xr_sprintf(out, "%.100s:%d", ScriptFileName(ar.short_src), ar.linedefined);
    if (!lua_iscfunction(L, -1))
    {
        for (int n = 1; n <= 16; ++n)
        {
            if (!lua_getupvalue(L, -1, n))
                break;
            if (lua_isfunction(L, -1))
            {
                lua_Debug inner{};
                if (lua_getinfo(L, ">S", &inner)) // pops the upvalue
                {
                    string128 part;
                    xr_sprintf(part, "(up %.100s:%d)", ScriptFileName(inner.short_src), inner.linedefined);
                    xr_strcat(out, part);
                }
                break;
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

lua_State* DescribeState()
{
    lua_State* L = GEnv.ScriptEngine ? GEnv.ScriptEngine->lua() : nullptr;
    return L && lua_checkstack(L, 6) ? L : nullptr;
}
} // namespace

pcstr PHScriptDescribeFunction(shared_str& cache, const luabind::object& function)
{
    if (cache.size())
        return cache.c_str();
    string256 text;
    xr_strcpy(text, "?");
    lua_State* L = function.is_valid() ? DescribeState() : nullptr;
    if (L)
    {
        function.push(L);
        DescribeFunctionOnTop(L, text);
    }
    cache = text;
    return cache.c_str();
}

pcstr PHScriptDescribeMethod(shared_str& cache, const luabind::object& object, pcstr method)
{
    if (cache.size())
        return cache.c_str();
    string256 text;
    xr_strcpy(text, "?");
    lua_State* L = object.is_valid() && method ? DescribeState() : nullptr;
    if (L)
    {
        const int top = lua_gettop(L);
        lua_pushcfunction(L, GetFieldProtected);
        object.push(L);
        lua_pushstring(L, method);
        if (lua_pcall(L, 2, 1, 0) == 0)
            DescribeFunctionOnTop(L, text);
        lua_settop(L, top);
    }
    string256 full;
    xr_sprintf(full, "%.40s@%.200s", method ? method : "?", text);
    cache = full;
    return cache.c_str();
}

/*
IC bool compare_safe(const luabind::object &o1 , const luabind::object &o2)
{
    return (o1.type()==LUA_TNIL && o2.type()==LUA_TNIL) || o1==o2;
}
**/

CPHScriptCondition::CPHScriptCondition(const luabind::functor<bool>& func)
{
    m_lua_function = xr_new<luabind::functor<bool>>(func);
}

CPHScriptCondition::CPHScriptCondition(const CPHScriptCondition& func)
{
    m_lua_function = xr_new<luabind::functor<bool>>(*func.m_lua_function);
}

CPHScriptCondition::~CPHScriptCondition() { xr_delete(m_lua_function); }
bool CPHScriptCondition::is_true() { return (*m_lua_function)(); }
bool CPHScriptCondition::obsolete() const { return false; }
//
CPHScriptAction::CPHScriptAction(const luabind::functor<void>& func)
{
    b_obsolete = false;
    m_lua_function = xr_new<luabind::functor<void>>(func);
}

CPHScriptAction::CPHScriptAction(const CPHScriptAction& action)
{
    b_obsolete = action.b_obsolete;
    m_lua_function = xr_new<luabind::functor<void>>(*action.m_lua_function);
}

CPHScriptAction::~CPHScriptAction() { xr_delete(m_lua_function); }
void CPHScriptAction::run()
{
    (*m_lua_function)();
    b_obsolete = true;
}

bool CPHScriptAction::obsolete() const { return b_obsolete; }
/////////////////////////////////////////////////////////////////////////////////////////////
CPHScriptObjectAction::CPHScriptObjectAction(const luabind::object& lua_object, LPCSTR method)
    : b_obsolete(false), m_lua_object(xr_new<luabind::object>(lua_object)), m_method_name(method)
{
}

CPHScriptObjectAction::CPHScriptObjectAction(const CPHScriptObjectAction& object)
{
    b_obsolete = object.b_obsolete;
    m_lua_object = xr_new<luabind::object>(*object.m_lua_object);
    m_method_name = object.m_method_name;
}

CPHScriptObjectAction::~CPHScriptObjectAction() { xr_delete(m_lua_object); }
bool CPHScriptObjectAction::compare(const CPHScriptObjectAction* v) const
{
    return m_method_name == v->m_method_name && compare_safe(*m_lua_object, *(v->m_lua_object));
}
void CPHScriptObjectAction::run()
{
    luabind::call_member<void>(*m_lua_object, m_method_name.c_str());
    b_obsolete = true;
}

bool CPHScriptObjectAction::obsolete() const { return b_obsolete; }
//
CPHScriptObjectCondition::CPHScriptObjectCondition(const luabind::object& lua_object, LPCSTR method)
{
    m_lua_object = xr_new<luabind::object>(lua_object);
    m_method_name = method;
}

CPHScriptObjectCondition::CPHScriptObjectCondition(const CPHScriptObjectCondition& object)
{
    m_lua_object = xr_new<luabind::object>(*object.m_lua_object);
    m_method_name = object.m_method_name;
}

CPHScriptObjectCondition::~CPHScriptObjectCondition() { xr_delete(m_lua_object); }
bool CPHScriptObjectCondition::compare(const CPHScriptObjectCondition* v) const
{
    return m_method_name == v->m_method_name && compare_safe(*m_lua_object, *(v->m_lua_object));
}

bool CPHScriptObjectCondition::is_true() { return luabind::call_member<bool>(*m_lua_object, m_method_name.c_str()); }
bool CPHScriptObjectCondition::obsolete() const { return false; }
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
CPHScriptObjectActionN::CPHScriptObjectActionN(const luabind::object& object, const luabind::functor<void>& functor)
    : b_obsolete(false)
{
    m_callback.set(functor, object);
}

CPHScriptObjectActionN::~CPHScriptObjectActionN() { m_callback.clear(); }
void CPHScriptObjectActionN::run()
{
    m_callback();
    b_obsolete = true;
}

bool CPHScriptObjectActionN::obsolete() const { return b_obsolete; }
CPHScriptObjectConditionN::CPHScriptObjectConditionN(
    const luabind::object& object, const luabind::functor<bool>& functor)
{
    m_callback.set(functor, object);
}

CPHScriptObjectConditionN::~CPHScriptObjectConditionN() { m_callback.clear(); }
bool CPHScriptObjectConditionN::is_true() { return m_callback(); }
bool CPHScriptObjectConditionN::obsolete() const { return false; }
