////////////////////////////////////////////////////////////////////////////
//	Module 		: script_export_macroses.h
//	Created 	: 24.06.2004
//  Modified 	: 24.06.2004
//	Author		: Andy Kolomiets, Dmitriy Iassenev
//	Description : XRay Script export macroses
////////////////////////////////////////////////////////////////////////////

#pragma once

#define MAKE_WRAPPER_NAME(cls) cls##_wrapper

#define DEFINE_LUA_WRAPPER_HEADER_0(cls)                                  \
    struct MAKE_WRAPPER_NAME(cls) : public cls, public luabind::wrap_base \
    {                                                                     \
        typedef cls inherited;                                            \
        typedef MAKE_WRAPPER_NAME(cls) self_type;                         \
        MAKE_WRAPPER_NAME(cls)() : inherited() {}
#define DEFINE_LUA_WRAPPER_FOOTER(cls) \
    }                                  \
    ;

#define DEFINE_LUABIND_CLASS_WRAPPER_0(type, wrapper, name) \
    luabind::class_<type, luabind::no_bases, luabind::default_holder, wrapper>(name)

#define DEFINE_LUABIND_CLASS_WRAPPER_1(type, wrapper, name, base1) \
    luabind::class_<type, base1, luabind::default_holder, wrapper>(name)

#define DEFINE_LUABIND_CLASS_WRAPPER_2(type, wrapper, name, base1, base2) \
    luabind::class_<type, bases<base1, base2>, luabind::default_holder, wrapper>(name)

#define DEFINE_LUABIND_CLASS_WRAPPER_3(type, wrapper, name, base1, base2, base3) \
    luabind::class_<type, bases<base1, base2, base3>, luabind::default_holder, wrapper>(name)

#define DEFINE_LUABIND_CLASS_WRAPPER_4(type, wrapper, name, base1, base2, base3, base4) \
    luabind::class_<type, bases<base1, base2, base3, base4>, luabind::default_holder, wrapper>(name)

#define DEFINE_LUABIND_VIRTUAL_FUNCTION(a, b, c) .def(#c, &a::c, &b::c##_static)

#define DEFINE_LUABIND_VIRTUAL_FUNCTION_EXPLICIT_CONST_0(a, b, c, d) \
    .def(#c, (d(a::*)() const)(&a::c), (d(*)(const a*))(&b::c##_static))

#define DEFINE_LUABIND_VIRTUAL_FUNCTION_EXPLICIT_1(a, b, c, d, e, f) \
    .def(#c, (d(a::*)(e))(&a::c), (d(*)(b*, f))(&b::c##_static))

#ifdef DEBUG
#ifdef LUABIND_NO_EXCEPTIONS
#define CAST_FAILED(v_func_name, ret_type)
#else
#define CAST_FAILED(v_func_name, ret_type)                                                                \
    catch (const luabind::cast_failed& exception)                                                                \
    {                                                                                                     \
        GEnv.ScriptEngine->script_log(LuaMessageType::Error,                                         \
            "SCRIPT RUNTIME ERROR : luabind::cast_failed in function %s (%s)!", #v_func_name, #ret_type); \
        return ((ret_type)(0));                                                                           \
    }
#endif
#else
#define CAST_FAILED(v_func_name, ret_type)
#endif

// C++ try/catch does not catch access violations from LuaJIT/luabind; SEH does (MSVC Windows).
// SEH and C++ try/catch cannot appear in the same function (C2712/C2713); use a separate helper for __try/__except.
#include <utility>
#include "script_engine.hpp"
#include "script_space.hpp"

#if defined(_WIN32) && defined(_MSC_VER)
#include <excpt.h>
template <typename... Args>
inline void XrLuabindCallMemberVoidSeh(luabind::wrap_base* self, cpcstr method_literal, Args&&... args)
{
    __try
    {
        luabind::call_member<void>(self, method_literal, std::forward<Args>(args)...);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ReportLuabindServerWrapperSeh(method_literal);
    }
}
#else
template <typename... Args>
inline void XrLuabindCallMemberVoidSeh(luabind::wrap_base* self, cpcstr method_literal, Args&&... args)
{
    try
    {
        luabind::call_member<void>(self, method_literal, std::forward<Args>(args)...);
    }
    catch (...)
    {
    }
}
#endif

#define DEFINE_LUA_WRAPPER_CONST_METHOD_0(v_func_name, ret_type) \
    virtual ret_type v_func_name() const noexcept\
    {                                                            \
        try                                                      \
        {                                                        \
            return luabind::call_member<ret_type>(this, #v_func_name);    \
        }                                                        \
        CAST_FAILED(v_func_name, ret_type)                       \
        catch (...) { return ((ret_type)(0)); }                  \
    }                                                            \
    static ret_type v_func_name##_static(const inherited* ptr) { return ptr->self_type::inherited::v_func_name(); }
#ifdef DEBUG
#define DEFINE_LUA_WRAPPER_CONST_METHOD_1(v_func_name, ret_type, t1)  \
    virtual ret_type v_func_name(t1 p1) const                         \
    {                                                                 \
        try                                                           \
        {                                                             \
            return luabind::call_member<ret_type>(this, #v_func_name, p1);     \
        }                                                             \
        CAST_FAILED(v_func_name, ret_type)                            \
        catch (...) { return ((ret_type)(0)); }                       \
    }                                                                 \
    static ret_type v_func_name##_static(const inherited* ptr, t1 p1) \
    {                                                                 \
        return ptr->self_type::inherited::v_func_name(p1);            \
    }
#else // DEBUG
#define DEFINE_LUA_WRAPPER_CONST_METHOD_1(v_func_name, ret_type, t1)                                    \
    virtual ret_type v_func_name(t1 p1) const { return luabind::call_member<ret_type>(this, #v_func_name, p1); } \
    static ret_type v_func_name##_static(const inherited* ptr, t1 p1)                                   \
    {                                                                                                   \
        return ptr->self_type::inherited::v_func_name(p1);                                              \
    }
#endif // DEBUG

#define DEFINE_LUA_WRAPPER_METHOD_V0(v_func_name)                                                              \
    virtual void v_func_name()                                                                                 \
    {                                                                                                          \
        try                                                                                                    \
        {                                                                                                      \
            XrLuabindCallMemberVoidSeh(static_cast<luabind::wrap_base*>(this), #v_func_name);                   \
        }                                                                                                      \
        catch (...)                                                                                            \
        {                                                                                                      \
        }                                                                                                      \
    }                                                                                                          \
    static void v_func_name##_static(inherited* ptr) { ptr->self_type::inherited::v_func_name(); }
#define DEFINE_LUA_WRAPPER_METHOD_V1(v_func_name, t1)                                                          \
    virtual void v_func_name(t1 p1)                                                                            \
    {                                                                                                          \
        try                                                                                                    \
        {                                                                                                      \
            XrLuabindCallMemberVoidSeh(static_cast<luabind::wrap_base*>(this), #v_func_name, p1);               \
        }                                                                                                      \
        catch (...)                                                                                            \
        {                                                                                                      \
        }                                                                                                      \
    }                                                                                                          \
    static void v_func_name##_static(inherited* ptr, t1 p1) { ptr->self_type::inherited::v_func_name(p1); }
#define DEFINE_LUA_WRAPPER_METHOD_V2(v_func_name, t1, t2)                                                      \
    virtual void v_func_name(t1 p1, t2 p2)                                                                     \
    {                                                                                                          \
        try                                                                                                    \
        {                                                                                                      \
            XrLuabindCallMemberVoidSeh(static_cast<luabind::wrap_base*>(this), #v_func_name, p1, p2);          \
        }                                                                                                      \
        catch (...)                                                                                            \
        {                                                                                                      \
        }                                                                                                      \
    }                                                                                                          \
    static void v_func_name##_static(inherited* ptr, t1 p1, t2 p2) { ptr->self_type::inherited::v_func_name(p1, p2); }
#define DEFINE_LUA_WRAPPER_METHOD_V3(v_func_name, t1, t2, t3)                                                  \
    virtual void v_func_name(t1 p1, t2 p2, t3 p3)                                                              \
    {                                                                                                          \
        try                                                                                                    \
        {                                                                                                      \
            XrLuabindCallMemberVoidSeh(static_cast<luabind::wrap_base*>(this), #v_func_name, p1, p2, p3);       \
        }                                                                                                      \
        catch (...)                                                                                            \
        {                                                                                                      \
        }                                                                                                      \
    }                                                                                                          \
    static void v_func_name##_static(inherited* ptr, t1 p1, t2 p2, t3 p3)                                      \
    {                                                                                                          \
        ptr->self_type::inherited::v_func_name(p1, p2, p3);                                                    \
    }

#define DEFINE_LUA_WRAPPER_METHOD_V4(v_func_name, t1, t2, t3, t4)                                              \
    virtual void v_func_name(t1 p1, t2 p2, t3 p3, t4 p4)                                                       \
    {                                                                                                          \
        try                                                                                                    \
        {                                                                                                      \
            XrLuabindCallMemberVoidSeh(static_cast<luabind::wrap_base*>(this), #v_func_name, p1, p2, p3, p4);   \
        }                                                                                                      \
        catch (...)                                                                                            \
        {                                                                                                      \
        }                                                                                                      \
    }                                                                                                          \
    static void v_func_name##_static(inherited* ptr, t1 p1, t2 p2, t3 p3, t4 p4)                               \
    {                                                                                                          \
        ptr->self_type::inherited::v_func_name(p1, p2, p3, p4);                                                \
    }

#define DEFINE_LUA_WRAPPER_METHOD_0(v_func_name, ret_type)    \
    virtual ret_type v_func_name()                            \
    {                                                         \
        try                                                   \
        {                                                     \
            return luabind::call_member<ret_type>(this, #v_func_name); \
        }                                                     \
        CAST_FAILED(v_func_name, ret_type)                    \
        catch (...) { return ((ret_type)(0)); }               \
    }                                                         \
    static ret_type v_func_name##_static(inherited* ptr) { return ptr->self_type::inherited::v_func_name(); }
#define DEFINE_LUA_WRAPPER_METHOD_1(v_func_name, ret_type, t1)    \
    virtual ret_type v_func_name(t1 p1)                           \
    {                                                             \
        try                                                       \
        {                                                         \
            return luabind::call_member<ret_type>(this, #v_func_name, p1); \
        }                                                         \
        CAST_FAILED(v_func_name, ret_type)                        \
        catch (...) { return ((ret_type)(0)); }                   \
    }                                                             \
    static ret_type v_func_name##_static(inherited* ptr, t1 p1) { return ptr->self_type::inherited::v_func_name(p1); }
#define DEFINE_LUA_WRAPPER_METHOD_2(v_func_name, ret_type, t1, t2)     \
    virtual ret_type v_func_name(t1 p1, t2 p2)                         \
    {                                                                  \
        try                                                            \
        {                                                              \
            return luabind::call_member<ret_type>(this, #v_func_name, p1, p2);  \
        }                                                              \
        CAST_FAILED(v_func_name, ret_type)                             \
        catch (...) { return ((ret_type)(0)); }                        \
    }                                                                  \
    static ret_type v_func_name##_static(inherited* ptr, t1 p1, t2 p2) \
    {                                                                  \
        return ptr->self_type::inherited::v_func_name(p1, p2);         \
    }

#define DEFINE_LUA_WRAPPER_METHOD_3(v_func_name, ret_type, t1, t2, t3)        \
    virtual ret_type v_func_name(t1 p1, t2 p2, t3 p3)                         \
    {                                                                         \
        try                                                                   \
        {                                                                     \
            return luabind::call_member<ret_type>(this, #v_func_name, p1, p2, p3);     \
        }                                                                     \
        CAST_FAILED(v_func_name, ret_type)                                    \
        catch (...) { return ((ret_type)(0)); }                               \
    }                                                                         \
    static ret_type v_func_name##_static(inherited* ptr, t1 p1, t2 p2, t3 p3) \
    {                                                                         \
        return ptr->self_type::inherited::v_func_name(p1, p2, p3);            \
    }

#define DEFINE_LUA_WRAPPER_METHOD_4(v_func_name, ret_type, t1, t2, t3, t4)           \
    virtual ret_type v_func_name(t1 p1, t2 p2, t3 p3, t4 p4)                         \
    {                                                                                \
        try                                                                          \
        {                                                                            \
            return luabind::call_member<ret_type>(this, #v_func_name, p1, p2, p3, p4);        \
        }                                                                            \
        CAST_FAILED(v_func_name, ret_type)                                           \
        catch (...) { return ((ret_type)(0)); }                                      \
    }                                                                                \
    static ret_type v_func_name##_static(inherited* ptr, t1 p1, t2 p2, t3 p3, t4 p4) \
    {                                                                                \
        return ptr->self_type::inherited::v_func_name(p1, p2, p3, p4);               \
    }

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#define DEFINE_LUA_WRAPPER_METHOD_R2P1_V1(v_func_name, t1) \
    virtual void v_func_name(t1& p1)                       \
    {                                                      \
        try                                                \
        {                                                  \
            call<void>(#v_func_name, &p1);                 \
        }                                                  \
        catch (...)                                        \
        {                                                  \
        }                                                  \
    }                                                      \
    static void v_func_name##_static(inherited* ptr, t1* p1) { ptr->self_type::inherited::v_func_name(*p1); }
#define DEFINE_LUA_WRAPPER_METHOD_R2P1_V2(v_func_name, t1, t2) \
    virtual void v_func_name(t1& p1, t2 p2)                    \
    {                                                          \
        try                                                    \
        {                                                      \
            call<void>(#v_func_name, &p1, p2);                 \
        }                                                      \
        catch (...)                                            \
        {                                                      \
        }                                                      \
    }                                                          \
    static void v_func_name##_static(inherited* ptr, t1* p1, t2 p2) { ptr->self_type::inherited::v_func_name(*p1, p2); }
#define DEFINE_LUA_WRAPPER_METHOD_R2P2_V2(v_func_name, t1, t2) \
    virtual void v_func_name(t1 p1, t2& p2)                    \
    {                                                          \
        try                                                    \
        {                                                      \
            call<void>(#v_func_name, p1, &p2);                 \
        }                                                      \
        catch (...)                                            \
        {                                                      \
        }                                                      \
    }                                                          \
    static void v_func_name##_static(inherited* ptr, t1 p1, t2* p2) { ptr->self_type::inherited::v_func_name(p1, *p2); }
#define DEFINE_LUA_WRAPPER_METHOD_R2P1_V4(v_func_name, t1, t2, t3, t4)            \
    virtual void v_func_name(t1& p1, t2 p2, t3 p3, t4 p4)                         \
    {                                                                             \
        try                                                                       \
        {                                                                         \
            call<void>(#v_func_name, &p1, p2, p3, p4);                            \
        }                                                                         \
        catch (...)                                                               \
        {                                                                         \
        }                                                                         \
    }                                                                             \
    static void v_func_name##_static(inherited* ptr, t1* p1, t2 p2, t3 p3, t4 p4) \
    {                                                                             \
        ptr->self_type::inherited::v_func_name(*p1, p2, p3, p4);                  \
    }

#define DEFINE_LUA_WRAPPER_METHOD_R2P3_V3(v_func_name, t1, t2, t3)         \
    virtual void v_func_name(t1 p1, t2 p2, t3& p3)                         \
    {                                                                      \
        try                                                                \
        {                                                                  \
            call<void>(#v_func_name, p1, p2, &p3);                         \
        }                                                                  \
        catch (...)                                                        \
        {                                                                  \
        }                                                                  \
    }                                                                      \
    static void v_func_name##_static(inherited* ptr, t1 p1, t2 p2, t3* p3) \
    {                                                                      \
        ptr->self_type::inherited::v_func_name(p1, p2, *p3);               \
    }
