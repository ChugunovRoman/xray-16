////////////////////////////////////////////////////////////////////////////
//	Module 		: script_engine_script.cpp
//	Created 	: 25.12.2002
//  Modified 	: 13.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife Simulator script engine export
////////////////////////////////////////////////////////////////////////////

#include "pch.hpp"

#include "ScriptEngineScript.hpp"
#include "script_engine.hpp"
#include "script_profiler.hpp"
#include "script_debugger.hpp"

#include "xrCommon/xr_string.h"
#include "xrCore/Debug/xrSentry.hpp"

namespace
{
void LuaSentrySoftErrorImpl(pcstr logger, pcstr message)
{
    if (!message || !*message)
        return;
    pcstr lg = (logger && *logger) ? logger : "lua.script";
    xr_string stack;
    if (GEnv.ScriptEngine)
        GEnv.ScriptEngine->format_lua_stack(nullptr, stack);
    xrSentry_CaptureSoftError(lg, message, stack.empty() ? nullptr : stack.c_str());
}

void LuaSentrySoftError_MessageOnly(pcstr message) { LuaSentrySoftErrorImpl("lua.script", message); }
void LuaSentrySoftError_LoggerMessage(pcstr logger, pcstr message) { LuaSentrySoftErrorImpl(logger, message); }
} // namespace

void ReportLuabindServerWrapperSeh(cpcstr wrapper_method_name)
{
    cpcstr method = (wrapper_method_name && *wrapper_method_name) ? wrapper_method_name : "<unknown>";
    string256 msg{};
    xr_sprintf(msg, "SEH in luabind server wrapper %s (AV / invalid code)", method);
    Msg("! %s", msg);

    xr_string stack;
    if (GEnv.ScriptEngine)
    {
        GEnv.ScriptEngine->format_lua_stack(nullptr, stack);
        GEnv.ScriptEngine->script_log(LuaMessageType::Error, "%s", msg);
    }

    xrSentry_CaptureSoftError("xrGame.luabind_server_wrapper", msg, stack.empty() ? nullptr : stack.c_str());
}

void LuaLog(pcstr caMessage)
{
    GEnv.ScriptEngine->script_log(LuaMessageType::Message, "%s", caMessage);
#if defined(USE_DEBUGGER)
    if (GEnv.ScriptEngine->debugger())
        GEnv.ScriptEngine->debugger()->Write(caMessage);
#endif
}

void ErrorLog(pcstr caMessage)
{
    GEnv.ScriptEngine->error_log("%s", caMessage);
    GEnv.ScriptEngine->print_stack();
#if defined(USE_DEBUGGER)
    if (GEnv.ScriptEngine->debugger())
        GEnv.ScriptEngine->debugger()->Write(caMessage);
#endif
    R_ASSERT2(0, caMessage);
}

void LuaError(pcstr caMessage)
{
    GEnv.ScriptEngine->error_log("%s", caMessage);
    R_ASSERT2(0, caMessage);
    static bool ignoreAlways;
    const auto result = xrDebug::Fail(ignoreAlways, DEBUG_INFO, "LUA error", caMessage);
}

//AVO:
void PrintStack()
{
    GEnv.ScriptEngine->print_stack();
}
//-AVO

void FlushLogs()
{
#ifdef DEBUG
    FlushLog();
    GEnv.ScriptEngine->flush_log();
#endif
}

void CScriptEngine::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        def("log", &LuaLog),
        def("error_log", &ErrorLog),
        def("lua_error", &LuaError),
        def("flush", &FlushLogs),
        def("print_stack", +[]()
        {
            GEnv.ScriptEngine->print_stack();
        }),
        // Same Sentry soft-event as xrSentry_CaptureSoftError: warning + native stack + extra.lua_stack (current Lua stack).
        def("sentry_soft_error", static_cast<void (*)(pcstr)>(&LuaSentrySoftError_MessageOnly)),
        def("sentry_soft_error", static_cast<void (*)(pcstr, pcstr)>(&LuaSentrySoftError_LoggerMessage)),
        def("prefetch", +[](pcstr file_name)
        {
            GEnv.ScriptEngine->process_file(file_name);
        }),
        def("verify_if_thread_is_running", +[]()
        {
            THROW2(GEnv.ScriptEngine->current_thread(), "coroutine.yield() is called outside the LUA thread!");
        }),
        def("bit_and", +[](const int i, const int j)
        {
            return i & j;
        }),
        def("bit_or", +[](const int i, const int j)
        {
            return i | j;
        }),
        def("bit_xor", +[](const int i, const int j)
        {
            return i ^ j;
        }),
        def("bit_not", +[](const int i)
        {
            return ~i;
        }),
        def("editor", +[]()
        {
            return GEnv.ScriptEngine->is_editor();
        }),
        def("user_name", +[]()
        {
            return Core.UserName;
        })
    ];
}

struct profile_timer_script
{
    using Clock = std::chrono::high_resolution_clock;
    using Time = Clock::time_point;
    using Duration = Clock::duration;

    Time start_time;
    Duration accumulator;
    u64 count = 0;
    int recurse_mark = 0;

    profile_timer_script() : start_time(), accumulator(), count(0), recurse_mark(0) {}

    bool operator<(const profile_timer_script& profile_timer) const
    {
        return accumulator < profile_timer.accumulator;
    }

    void start()
    {
        if (recurse_mark)
        {
            ++recurse_mark;
            return;
        }

        ++recurse_mark;
        ++count;
        start_time = Clock::now();
    }

    void stop()
    {
        if (!recurse_mark)
            return;

        --recurse_mark;

        if (recurse_mark)
            return;

        const auto finish = Clock::now();
        if (finish > start_time)
            accumulator += finish - start_time;
    }

    float time() const
    {
        using namespace std::chrono;
        return float(duration_cast<microseconds>(accumulator).count());
    }
};

inline profile_timer_script operator+(const profile_timer_script& portion0, const profile_timer_script& portion1)
{
    profile_timer_script result;
    result.accumulator = portion0.accumulator + portion1.accumulator;
    result.count = portion0.count + portion1.count;
    return result;
}

std::ostream& operator<<(std::ostream& os, const profile_timer_script& pt)
{
    return os << pt.time();
}

void CScriptProfiler::script_register(lua_State* luaState)
{
    using namespace luabind;

    globals(luaState)["PROFILER_TYPE_NONE"]     = (u32)CScriptProfilerType::None;
    globals(luaState)["PROFILER_TYPE_HOOK"]     = (u32)CScriptProfilerType::Hook;
    globals(luaState)["PROFILER_TYPE_SAMPLING"] = (u32)CScriptProfilerType::Sampling;

    module(luaState)
    [
        class_<profile_timer_script>("profile_timer")
            .def(constructor<>())
            .def(constructor<profile_timer_script&>())
            .def(const_self + profile_timer_script())
            .def(const_self < profile_timer_script())
            .def(tostring(self))
            .def("start", &profile_timer_script::start)
            .def("stop", &profile_timer_script::stop)
            .def("time", &profile_timer_script::time)
    ];

    module(luaState, "profiler")
    [
        def("is_active", +[]() -> bool
        {
            return GEnv.ScriptEngine->m_profiler->IsActive();
        }),
        def("get_type", +[]()-> u32
        {
            return static_cast<u32>(GEnv.ScriptEngine->m_profiler->GetType());
        }),
        def("start", +[]()
        {
            GEnv.ScriptEngine->m_profiler->Start();
        }),
        def("start", +[](CScriptProfilerType profiler_type)
        {
            GEnv.ScriptEngine->m_profiler->Start(profiler_type);
        }),
        def("start_hook_mode", +[]()
        {
            GEnv.ScriptEngine->m_profiler->StartHookMode();
        }),
        def("start_sampling_mode", +[]()
        {
            GEnv.ScriptEngine->m_profiler->StartSamplingMode();
        }),
        def("start_sampling_mode", +[](u32 sampling_interval = CScriptProfiler::PROFILE_SAMPLING_INTERVAL_DEFAULT)
        {
            GEnv.ScriptEngine->m_profiler->StartSamplingMode(sampling_interval);
        }),
        def("stop", +[]()
        {
            GEnv.ScriptEngine->m_profiler->Stop();
        }),
        def("reset", +[]()
        {
            GEnv.ScriptEngine->m_profiler->Reset();
        }),
        def("log_report", +[]()
        {
            GEnv.ScriptEngine->m_profiler->LogReport();
        }),
        def("log_report", +[](u32 entries_limit)
        {
            GEnv.ScriptEngine->m_profiler->LogReport(entries_limit);
        }),
        def("save_report", +[]()
        {
            GEnv.ScriptEngine->m_profiler->SaveReport();
        })
    ];
}
