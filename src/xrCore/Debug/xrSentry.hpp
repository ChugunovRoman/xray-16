#pragma once

#include "xrCore/xrCore.h"
#include "xrCommon/xr_string.h"

// Optional crash reporting via sentry-native (USE_SENTRY / CMake WITH_SENTRY / MSVC UseSentry).

void xrSentry_Initialize(pcstr commandLine);
void xrSentry_Shutdown();

using xrSentry_LuaStackFn = void (*)(xr_string& out);
// Registered when CScriptEngine is constructed; used from xrDebug::Fail and when CaptureError has no explicit stack.
void XRCORE_API xrSentry_SetLuaStackProvider(xrSentry_LuaStackFn fn);

// Non-fatal issue (recovered error): sends a warning event to Sentry with native stack; optional Lua stack text in extra.
void XRCORE_API xrSentry_CaptureSoftError(pcstr logger, pcstr message, pcstr lua_stack = nullptr);

// Script/engine error (caught Lua, etc.): ERROR level, native stack; lua_stack overrides provider when non-null.
void XRCORE_API xrSentry_CaptureError(pcstr logger, pcstr message, pcstr lua_stack = nullptr);

// xrDebug::Fail: compact assertion text + Lua stack (provider or empty).
void XRCORE_API xrSentry_CaptureDebugFail(pcstr expr, pcstr desc, pcstr arg1 = nullptr, pcstr arg2 = nullptr);

// CInifile::r_string before xrDebug::Fatal: error-level event, native stack, extra ini_file/section/key.
void XRCORE_API xrSentry_CaptureIniRStringError(pcstr ini_path, pcstr section, pcstr key);
