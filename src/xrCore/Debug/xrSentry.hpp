#pragma once

#include "xrCore/xrCore.h"
#include "xrCommon/xr_string.h"

// Optional crash reporting via sentry-native (USE_SENTRY / CMake WITH_SENTRY / MSVC UseSentry).

void xrSentry_Initialize(pcstr commandLine);
void xrSentry_Shutdown();
// Hardware profile tags (hw.class, hw.ram_gb, hw.commit_gb, hw.disk, hw.vram_mb, hw.cpu, hw.wine): set when Sentry
// starts and again when the render reports its GPU. No-op when Sentry is not compiled in or not started.
void xrSentry_UpdateHwTags();

using xrSentry_LuaStackFn = void (*)(xr_string& out);
// Registered when CScriptEngine is constructed; used from xrDebug::Fail and when CaptureError has no explicit stack.
void XRCORE_API xrSentry_SetLuaStackProvider(xrSentry_LuaStackFn fn);

// Saves the local crash report (minidump + log) for a crash that never reaches the unhandled
// exception filter, such as a stack overflow caught by its own SEH frame in the game thread.
// No-op when Sentry is not compiled in or the reports folder could not be prepared.
void XRCORE_API xrSentry_SaveLocalCrashReport(EXCEPTION_POINTERS* exPtrs);

// Non-fatal issue (recovered error): sends a warning event to Sentry with native stack; optional Lua stack text in extra.
// Deduplicated: one event per unique normalized message per session, plus session/rate caps (excess is dropped with a
// single "N events suppressed" log line). The event is built here and sent from a background thread - the caller does
// not wait for serialization or the transport.
void XRCORE_API xrSentry_CaptureSoftError(pcstr logger, pcstr message, pcstr lua_stack = nullptr);

// Script/engine error (caught Lua, etc.): ERROR level, native stack; lua_stack overrides provider when non-null.
// Same dedup/limits and background send as xrSentry_CaptureSoftError (run7-freezes-p5.md §6.2).
void XRCORE_API xrSentry_CaptureError(pcstr logger, pcstr message, pcstr lua_stack = nullptr);

// xrDebug::Fail: compact assertion text + Lua stack (provider or empty). Only queues the event, never waits for the
// upload: the error dialog comes right after it, and the player may continue the game.
void XRCORE_API xrSentry_CaptureDebugFail(pcstr expr, pcstr desc, pcstr arg1 = nullptr, pcstr arg2 = nullptr);

// Waits until the queued events are sent, at most timeoutMs (also hands the not-yet-sent events to the transport
// first). xrDebug::Fail calls it once the player chose to exit.
void xrSentry_Flush(u32 timeoutMs);

// CInifile::r_string before xrDebug::Fatal: error-level event, native stack, extra ini_file/section/key.
void XRCORE_API xrSentry_CaptureIniRStringError(pcstr ini_path, pcstr section, pcstr key);

// Review2 SEN-m1: true when CaptureError/CaptureSoftError(message) would actually be sent right now (new this
// session, session/window caps not hit) - lets a caller skip building an expensive payload (a formatted Lua stack)
// for a message the gate would drop anyway. Read-only: does not record the key or consume a slot.
bool XRCORE_API xrSentry_WouldCapture(pcstr message);
