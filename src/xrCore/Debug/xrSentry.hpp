#pragma once

#include "xrCore/xrCore.h"

// Optional crash reporting via sentry-native (USE_SENTRY / CMake WITH_SENTRY / MSVC UseSentry).

void xrSentry_Initialize(pcstr commandLine);
void xrSentry_Shutdown();

// Non-fatal issue (recovered error): sends a warning event to Sentry with native stack; optional Lua stack text in extra.
void XRCORE_API xrSentry_CaptureSoftError(pcstr logger, pcstr message, pcstr lua_stack = nullptr);

// CInifile::r_string before xrDebug::Fatal: error-level event, native stack, extra ini_file/section/key.
void XRCORE_API xrSentry_CaptureIniRStringError(pcstr ini_path, pcstr section, pcstr key);
