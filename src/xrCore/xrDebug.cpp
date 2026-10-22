#include "stdafx.h"
#pragma hdrstop

#include "xrDebug.h"
#include "Debug/xrSentry.hpp"

namespace
{
pcstr s_sentry_command_line = nullptr;
}
#include "Debug/StackTrace.h"
#include "os_clipboard.h"
#include "log.h"
#include "Threading/ScopeLock.hpp"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#if defined(XR_PLATFORM_WINDOWS)
#   include "Text/Utf8Utils.hpp"
#   include <dbghelp.h>
#   include <direct.h>
#   include <new.h> // for _set_new_mode
#   include <errorrep.h> // ReportFault

#   if !defined(USE_SENTRY)
#       define USE_BUG_TRAP
#       include "BugTrap.h"
#   endif

#   include "Debug/dxerr.h"
#endif

#if defined(XR_PLATFORM_LINUX) || defined(XR_PLATFORM_APPLE) || defined(XR_PLATFORM_BSD)
#   if __has_include(<sys/ptrace.h>)
#       include <sys/ptrace.h>
#       define PTRACE_AVAILABLE

#       if defined(XR_PLATFORM_APPLE) || defined(XR_PLATFORM_BSD)
#           define PTRACE_TRACEME PT_TRACE_ME
#           define PTRACE_DETACH PT_DETACH
#       endif
#   endif
#endif

#if defined(XR_PLATFORM_LINUX)
#   include <pthread.h> // pthread_getname_np
#endif

#if defined(XR_PLATFORM_WINDOWS)
// PlatformWindows.inl hides DrawText (NODRAWTEXT) and the system colours (NOCOLOR) from the whole engine;
// the error dialog of xrDebug::Fail needs DrawTextW to measure its header and the dialog background colour.
#   ifndef DT_WORDBREAK
#       define DT_WORDBREAK 0x00000010
#   endif
#   ifndef DT_CALCRECT
#       define DT_CALCRECT 0x00000400
#   endif
#   ifndef DT_NOPREFIX
#       define DT_NOPREFIX 0x00000800
#   endif
#   ifndef COLOR_BTNFACE
#       define COLOR_BTNFACE 15
#   endif
#   ifdef NODRAWTEXT
extern "C" __declspec(dllimport) int WINAPI DrawTextW(HDC, LPCWSTR, int, LPRECT, UINT);
#   endif
#endif

extern pcstr log_name();

namespace
{
// The thread that created the application window: xrDebug::SetWindowHandler is called right after SDL_CreateWindow.
// The window may only be changed from that thread. SDL_SetWindowFullscreen, ShowWindow, EnableWindow and a message box
// owned by the window all send messages to it and wait for the answer, and that thread may itself be waiting for the
// failing one (the main thread joins the Game logic thread every frame): the dialog then never appeared and the
// window turned "not responding".
std::atomic<SDL_threadID> s_window_thread{ 0 };

bool on_window_thread()
{
    const SDL_threadID owner = s_window_thread.load(std::memory_order_acquire);
    return owner != 0 && owner == SDL_ThreadID();
}

// Before a dialog: the window handler (leave fullscreen, release the input grab) only on the window's own thread.
// Returns true when it was called, and then it must be called again after the dialog.
bool begin_error_dialog(IWindowHandler* handler)
{
#if defined(XR_PLATFORM_WINDOWS)
    // SDL relative mouse mode clips the cursor to the game window. ClipCursor is global and does not involve the
    // window's thread, so it is safe from any thread; SDL clips again once the game window gets the focus back.
    ClipCursor(nullptr);
#endif
    if (!handler || !on_window_thread())
        return false;
    handler->OnErrorDialog(true);
    return true;
}

// Before the process goes down: hide the window (BugTrap shows its own dialog after the break).
void hide_window_before_exit(IWindowHandler* handler)
{
    if (!handler)
        return;
    if (on_window_thread())
    {
        handler->OnFatalError();
        return;
    }
#if defined(XR_PLATFORM_WINDOWS)
    // ShowWindowAsync only posts the request; ShowWindow would wait for the window's thread.
    if (const auto wnd = static_cast<HWND>(handler->GetApplicationWindowHandle()))
        ShowWindowAsync(wnd, SW_HIDE);
#endif
}

// Name of the calling thread as set by Threading::SetCurrentThreadName, empty if unknown.
void get_current_thread_name(pstr out, size_t size)
{
    if (!out || !size)
        return;
    out[0] = 0;
#if defined(XR_PLATFORM_WINDOWS)
    // SetCurrentThreadName prefers SetThreadDescription, so the name is readable back the same way.
    using GetThreadDescriptionProc = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    const HMODULE kernelHandle = GetModuleHandleA("kernel32.dll");
    const auto getThreadDescription = kernelHandle
        ? reinterpret_cast<GetThreadDescriptionProc>(GetProcAddress(kernelHandle, "GetThreadDescription"))
        : nullptr;

    if (getThreadDescription)
    {
        PWSTR description = nullptr;
        if (SUCCEEDED(getThreadDescription(GetCurrentThread(), &description)) && description)
        {
            size_t converted = 0;
            wcstombs_s(&converted, out, size, description, size - 1);
            LocalFree(description);
        }
    }
#elif defined(XR_PLATFORM_LINUX)
    if (pthread_getname_np(pthread_self(), out, size) != 0)
        out[0] = 0;
#endif
}

u64 get_current_thread_id()
{
#if defined(XR_PLATFORM_WINDOWS)
    return static_cast<u64>(GetCurrentThreadId());
#else
    return static_cast<u64>(SDL_ThreadID());
#endif
}
} // namespace

constexpr SDL_MessageBoxButtonData buttons[] =
{
    /* .flags, .buttonid, .text */
    { 0, (int)AssertionResult::ignore, "Continue"  },
    { 0, (int)AssertionResult::tryAgain, "Try again" },

    { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT |
      SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,
         (int)AssertionResult::abort, "Cancel" }
};

AssertionResult xrDebug::ShowMessage(pcstr title, pcstr message, bool simpleMode)
{
    // Owned by the game window only on the window's own thread: an owned message box disables its owner and shares
    // its input queue, which hangs both when the owner's thread waits for this one (see s_window_thread).
    const bool ownedByWindow = windowHandler && on_window_thread();
#ifdef XR_PLATFORM_WINDOWS // because Windows default Message box is fancy
    HWND hwnd = nullptr;
    if (ownedByWindow)
        hwnd = static_cast<HWND>(windowHandler->GetApplicationWindowHandle());

    if (simpleMode)
    {
        MessageBox(hwnd, message, title, MB_OK | MB_ICONERROR | MB_SYSTEMMODAL | MB_SETFOREGROUND);
        return AssertionResult::ok;
    }

    const int result = MessageBox(hwnd, message, title,
        MB_CANCELTRYCONTINUE | MB_ICONERROR | MB_SYSTEMMODAL | MB_SETFOREGROUND);

    switch (result)
    {
    case IDCANCEL: return AssertionResult::abort;
    case IDTRYAGAIN: return AssertionResult::tryAgain;
    case IDCONTINUE: return AssertionResult::ignore;
    default: return AssertionResult::undefined;
    }
#else
    if (simpleMode)
    {
        SDL_Window* parent = ownedByWindow ? windowHandler->GetApplicationWindow() : nullptr;
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, parent);
        return AssertionResult::ok;
    }

    SDL_MessageBoxData data =
    {
        SDL_MESSAGEBOX_ERROR,
        ownedByWindow ? windowHandler->GetApplicationWindow() : nullptr,
        title, message, SDL_arraysize(buttons), buttons, nullptr
    };

    int button = -1;
    SDL_ShowMessageBox(&data, &button);
    return (AssertionResult)button;
#endif
}

SDL_AssertState SDLAssertionHandler(const SDL_AssertData* data,
    void* /*userdata*/)
{
    if (data->always_ignore)
        return SDL_ASSERTION_ALWAYS_IGNORE;

    static constexpr pcstr desc = "SDL2 assertion triggered";
    bool alwaysIgnore = false;

    const auto result = xrDebug::Fail(alwaysIgnore,
        { data->filename, data->linenum, data->function },
        data->condition, desc);

    switch (result)
    {
    case AssertionResult::ignore:
        return SDL_ASSERTION_ALWAYS_IGNORE;

    case AssertionResult::tryAgain:
        // Continue in the error dialog: go on once. SDL_ASSERTION_RETRY would check the condition again
        // and bring the same dialog right back.
        return SDL_ASSERTION_IGNORE;

    case AssertionResult::abort:
        return SDL_ASSERTION_ABORT;

    case AssertionResult::undefined:
    case AssertionResult::ok:
    default:
        return SDL_ASSERTION_IGNORE;
    }
}

IWindowHandler* xrDebug::windowHandler = nullptr;
IUserConfigHandler* xrDebug::userConfigHandler = nullptr;
xrDebug::UnhandledExceptionFilter xrDebug::PrevFilter = nullptr;
xrDebug::OutOfMemoryCallbackFunc xrDebug::OutOfMemoryCallback = nullptr;
xrDebug::LuaStackProviderFunc xrDebug::LuaStackProvider = nullptr;
string_path xrDebug::BugReportFile;
bool xrDebug::ErrorAfterDialog = false;
bool xrDebug::ShowErrorMessage = true;

#ifdef PROFILE_CRITICAL_SECTIONS
Lock xrDebug::failLock(MUTEX_PROFILE_ID(xrDebug::Backend));
#else
Lock xrDebug::failLock;
#endif

void xrDebug::SetBugReportFile(const char* fileName) { xr_strcpy(BugReportFile, fileName); }

void xrDebug::SetWindowHandler(IWindowHandler* handler)
{
    windowHandler = handler;
    s_window_thread.store(handler ? SDL_ThreadID() : 0, std::memory_order_release);
}

void xrDebug::AddSymbolSearchPath(pcstr dir)
{
#if defined(XR_PLATFORM_WINDOWS)
    AddSymbolSearchDirectory(dir);
#else
    (void)dir;
#endif
}

void xrDebug::ForgetModuleSymbols(const void* moduleBase)
{
#if defined(XR_PLATFORM_WINDOWS)
    UnloadModuleSymbols(moduleBase);
#else
    (void)moduleBase;
#endif
}

xr_vector<xr_string> xrDebug::SymbolizeAddresses(const xr_vector<const void*>& addresses)
{
    return SymbolizeAddressList(addresses);
}

void xrDebug::LogStackTrace(const char* header)
{
    xr_vector<xr_string> stackTrace = BuildStackTrace();
    Msg("%s", header);
    for (const auto& frame : stackTrace)
    {
        Msg("%s", frame.c_str());
    }
}

#if defined(XR_PLATFORM_WINDOWS)
namespace
{
constexpr pcstr kCrashPrefix = "! [crash] ";
constexpr u16 kCrashStackFrames = 256;

// Defined by winnt.h, but keep building if an older SDK misses it.
#ifndef STATUS_HEAP_CORRUPTION
#   define STATUS_HEAP_CORRUPTION ((DWORD)0xC0000374L)
#endif

// Guards against a second crash block: the filter chain (local report -> Crashpad) and
// xrDebug::UnhandledFilter can both run for the same exception.
volatile LONG s_crash_logged = 0;

// Used instead of the one above by handlers that can fire on an exception the process survives,
// such as the heap corruption VEH. It only keeps such a handler out of its own way, so a genuine
// crash on another thread still finds the real budget free and gets its block written.
volatile LONG s_crash_logging = 0;

pcstr exception_code_to_string(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:         return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:               return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT:    return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DENORMAL_OPERAND:     return "EXCEPTION_FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INEXACT_RESULT:       return "EXCEPTION_FLT_INEXACT_RESULT";
    case EXCEPTION_FLT_INVALID_OPERATION:    return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW:             return "EXCEPTION_FLT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK:          return "EXCEPTION_FLT_STACK_CHECK";
    case EXCEPTION_FLT_UNDERFLOW:            return "EXCEPTION_FLT_UNDERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:      return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:            return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:             return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_INVALID_DISPOSITION:      return "EXCEPTION_INVALID_DISPOSITION";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_PRIV_INSTRUCTION:         return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_SINGLE_STEP:              return "EXCEPTION_SINGLE_STEP";
    case EXCEPTION_STACK_OVERFLOW:           return "EXCEPTION_STACK_OVERFLOW";
    case STATUS_HEAP_CORRUPTION:             return "STATUS_HEAP_CORRUPTION";
    case 0xE06D7363:                         return "C++ exception";
    case 0x406D1388:                         return "thread name notification";
    default:                                 return "<unknown exception>";
    }
}

// dbghelp and the Lua state are both suspect while handling a crash, so every call into them is
// wrapped in SEH. A function containing __try must not hold objects requiring unwinding, hence
// the work always lives in a separate _impl function.
void build_stack_trace_impl(PCONTEXT threadCtx, xr_vector<xr_string>* out)
{
    *out = threadCtx ? BuildStackTrace(threadCtx, kCrashStackFrames) : BuildStackTrace(kCrashStackFrames);
}

bool safe_build_stack_trace(PCONTEXT threadCtx, xr_vector<xr_string>* out)
{
    __try
    {
        build_stack_trace_impl(threadCtx, out);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void collect_modules_impl(xr_vector<xr_string>* out, const void* extraAddress)
{
    CollectGameModules(*out, extraAddress);
}

bool safe_collect_modules(xr_vector<xr_string>* out, const void* extraAddress)
{
    __try
    {
        collect_modules_impl(out, extraAddress);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void lua_stack_impl(xr_string* out)
{
    const auto provider = xrDebug::GetLuaStackProvider();
    if (provider)
        provider(*out);
}

bool safe_lua_stack(xr_string* out)
{
    __try
    {
        lua_stack_impl(out);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void log_multiline_with_prefix(pcstr text)
{
    pcstr cursor = text;
    while (cursor && *cursor)
    {
        cpcstr eol = std::strchr(cursor, '\n');
        size_t length = eol ? static_cast<size_t>(eol - cursor) : std::strlen(cursor);
        if (length && cursor[length - 1] == '\r')
            --length;

        string1024 line;
        const size_t copied = std::min(length, sizeof(line) - 1);
        std::memcpy(line, cursor, copied);
        line[copied] = 0;

        Msg("%s  %s", kCrashPrefix, line);
        cursor = eol ? eol + 1 : nullptr;
    }
}

void log_current_thread_name()
{
    string256 threadName{};
    get_current_thread_name(threadName, sizeof(threadName));

    Msg("%sthread: 0x%X \"%s\"", kCrashPrefix, static_cast<u32>(GetCurrentThreadId()), threadName);
}
} // namespace
#endif // XR_PLATFORM_WINDOWS

void xrDebug::LogCrashInfo(EXCEPTION_POINTERS* exPtrs, pcstr reason, bool oneShot)
{
#if defined(XR_PLATFORM_WINDOWS)
    volatile LONG* const guard = oneShot ? &s_crash_logged : &s_crash_logging;
    if (InterlockedCompareExchange(guard, 1, 0) != 0)
        return;

    // Before touching dbghelp: should symbol handling fault or hang, everything logged so far is on disk.
    FlushLog();

    Msg("%s===================== UNHANDLED EXCEPTION =====================", kCrashPrefix);

    if (reason && *reason)
        Msg("%sreason: %s", kCrashPrefix, reason);

    const EXCEPTION_RECORD* record = exPtrs ? exPtrs->ExceptionRecord : nullptr;
    if (record)
    {
        string256 details{};
        if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
                record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
            record->NumberParameters >= 2)
        {
            pcstr operation = "read of";
            switch (record->ExceptionInformation[0])
            {
            case 1:  operation = "write to"; break;
            case 8:  operation = "execute of"; break;
            default: break;
            }
            xr_sprintf(details, sizeof(details), " (%s address 0x%016llX)", operation,
                static_cast<u64>(record->ExceptionInformation[1]));
        }

        Msg("%scode: 0x%08X %s%s", kCrashPrefix, static_cast<u32>(record->ExceptionCode),
            exception_code_to_string(record->ExceptionCode), details);

        xr_string address;
        FormatModuleRelativeAddress(record->ExceptionAddress, address);
        Msg("%saddress: 0x%016llX (%s)", kCrashPrefix,
            static_cast<u64>(reinterpret_cast<uintptr_t>(record->ExceptionAddress)), address.c_str());
    }

    log_current_thread_name();

    Msg("%sbuild: %u, commit %s, branch %s", kCrashPrefix, Core.GetBuildId(), xrCore::GetBuildCommit(),
        xrCore::GetBuildBranch());

    xr_vector<xr_string> modules;
    if (safe_collect_modules(&modules, record ? record->ExceptionAddress : nullptr) && !modules.empty())
    {
        Msg("%smodules:", kCrashPrefix);
        for (const auto& moduleInfo : modules)
            Msg("%s  %s", kCrashPrefix, moduleInfo.c_str());
    }

    // StackWalk consumes the context, so the crashing thread's own record must not be passed directly.
    // Static storage keeps ~1.2 KiB off the stack of a thread that may have just overflowed it.
    static CONTEXT crashContext;
    PCONTEXT contextForWalk = nullptr;
    if (exPtrs && exPtrs->ContextRecord)
    {
        crashContext = *exPtrs->ContextRecord;
        contextForWalk = &crashContext;
    }

    xr_vector<xr_string> stackTrace;
    if (!safe_build_stack_trace(contextForWalk, &stackTrace))
        Msg("%sstack trace unavailable (dbghelp failed)", kCrashPrefix);
    else if (stackTrace.empty())
        Msg("%sstack trace unavailable (no frames)", kCrashPrefix);
    else
    {
        Msg("%sstack trace:", kCrashPrefix);
        for (size_t i = 0; i < stackTrace.size(); ++i)
            Msg("%s  #%02zu %s", kCrashPrefix, i, stackTrace[i].c_str());
    }

    xr_string luaStack;
    if (safe_lua_stack(&luaStack) && !luaStack.empty())
    {
        Msg("%slua stack:", kCrashPrefix);
        log_multiline_with_prefix(luaStack.c_str());
    }

    Msg("%s===============================================================", kCrashPrefix);
    FlushLog();

    if (!oneShot)
        InterlockedExchange(&s_crash_logging, 0);
#else
    (void)exPtrs;
    (void)reason;
    (void)oneShot;
#endif
}

#if defined(XR_PLATFORM_WINDOWS)
namespace
{
// Separate function: one containing __try must hold no objects requiring unwinding.
void log_crash_info_call(EXCEPTION_POINTERS* exPtrs, pcstr reason, bool oneShot)
{
    xrDebug::LogCrashInfo(exPtrs, reason, oneShot);
}

void release_crash_logging()
{
    InterlockedExchange(&s_crash_logging, 0);
}
} // namespace
#endif

void xrDebug::LogCrashInfoGuarded(EXCEPTION_POINTERS* exPtrs, pcstr reason, bool oneShot)
{
#if defined(XR_PLATFORM_WINDOWS)
    __try
    {
        log_crash_info_call(exPtrs, reason, oneShot);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Nothing usable left to report with; the caller still gets to save its dump.
    }

    // Outside the SEH frame on purpose: LogCrashInfo releases the guard at its very end, which a
    // fault during logging would skip, and it would stay taken for the rest of the process.
    if (!oneShot)
        release_crash_logging();
#else
    LogCrashInfo(exPtrs, reason, oneShot);
#endif
}


namespace
{
// Never let the assertion text overflow its buffer. Outside MASTER_GOLD xr_sprintf maps to
// vsprintf_s, which reports a buffer that is too small through the invalid parameter handler.
// That handler calls xrDebug::Fail, which calls GatherInfo again: an assertion with a long stack
// would recurse until the stack ran out. vsnprintf truncates instead, which is what we want here.
void append_clamped(char*& cursor, cpcstr bufferEnd, pcstr format, ...)
{
    if (!cursor || cursor >= bufferEnd)
        return;

    const size_t space = static_cast<size_t>(bufferEnd - cursor);

    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(cursor, space, format, args);
    va_end(args);

    if (written <= 0)
        return;

    // vsnprintf returns what it would have written, so clamp to what actually fits.
    cursor += std::min(static_cast<size_t>(written), space - 1);
}

// Appends to a NUL terminated buffer, truncating instead of failing. Same reason as append_clamped:
// xr_strcat is strcat_s outside MASTER_GOLD, and a full buffer there goes through the invalid
// parameter handler, which calls Fail and recurses. Callers append to the text GatherInfo produced.
void append_tail_clamped(pstr buffer, size_t bufferSize, pcstr text)
{
    if (!buffer || !text || bufferSize == 0)
        return;

    const size_t used = std::strlen(buffer);
    if (used + 1 >= bufferSize)
        return;

    std::snprintf(buffer + used, bufferSize - used, "%s", text);
}

// The dialog and the log only need enough frames to identify the caller, and every frame costs
// buffer space. BuildStackTrace's default of 512 is far more than a fixed buffer can hold.
constexpr u16 kAssertStackFrames = 128;

// Kept free at the end of the assertion text for what Fail and xr_terminate append afterwards
// (the button hints, an SDL error). Without it the stack would fill the buffer to the last byte.
constexpr size_t kAssertTailReserve = 512;
} // namespace

void xrDebug::GatherInfo(char* assertionInfo, size_t bufferSize, const ErrorLocation& loc, const char* expr,
                         const char* desc, const char* arg1, const char* arg2)
{
    char* buffer = assertionInfo;
    if (!expr)
        expr = "<no expression>";
    bool extendedDesc = desc && strchr(desc, '\n');
    pcstr prefix = "[error] ";
    const char* oneAboveBuffer = assertionInfo + bufferSize;
    append_clamped(buffer, oneAboveBuffer, "\nFATAL ERROR\n\n");
    append_clamped(buffer, oneAboveBuffer, "%sExpression    : %s\n", prefix, expr);
    append_clamped(buffer, oneAboveBuffer, "%sFunction      : %s\n", prefix, loc.Function);
    append_clamped(buffer, oneAboveBuffer, "%sFile          : %s\n", prefix, loc.File);
    append_clamped(buffer, oneAboveBuffer, "%sLine          : %d\n", prefix, loc.Line);
    if (extendedDesc)
    {
        append_clamped(buffer, oneAboveBuffer, "\n%s\n", desc);
        if (arg1)
        {
            append_clamped(buffer, oneAboveBuffer, "%s\n", arg1);
            if (arg2)
                append_clamped(buffer, oneAboveBuffer, "%s\n", arg2);
        }
    }
    else
    {
        append_clamped(buffer, oneAboveBuffer, "%sDescription   : %s\n", prefix, desc);
        if (arg1)
        {
            if (arg2)
            {
                append_clamped(buffer, oneAboveBuffer, "%sArgument 0    : %s\n", prefix, arg1);
                append_clamped(buffer, oneAboveBuffer, "%sArgument 1    : %s\n", prefix, arg2);
            }
            else
                append_clamped(buffer, oneAboveBuffer, "%sArguments     : %s\n", prefix, arg1);
        }
    }
    append_clamped(buffer, oneAboveBuffer, "\n");

    Log(assertionInfo);
    FlushLog();

    // buffer keeps pointing past the assertion text: the trace is appended, never written over it.
#if defined(XR_PLATFORM_WINDOWS)
    // The stack is what makes a player's report actionable, so it is written by default.
    // The condition used to be inverted, which logged it only when -no_call_stack_assert was passed.
    if (strstr(GetCommandLine(), "-no_call_stack_assert"))
        return;
#endif

    Log("stack trace:\n");

    // The trace stops short of the end of the buffer so that the caller can still append its own
    // lines without hitting the invalid parameter handler.
    cpcstr stackEnd = bufferSize > kAssertTailReserve ? oneAboveBuffer - kAssertTailReserve : assertionInfo;
    append_clamped(buffer, stackEnd, "stack trace:\n\n");

    // The log gets every frame; the buffer takes what is left, and silently stops when full.
    xr_vector<xr_string> stackTrace = BuildStackTrace(kAssertStackFrames);
    for (size_t i = 2; i < stackTrace.size(); i++)
    {
        Log(stackTrace[i].c_str());
        append_clamped(buffer, stackEnd, "%s\n", stackTrace[i].c_str());
    }

    FlushLog();
#ifdef DEBUG
    // Clipboard only in developer builds: an assertion in a shipped build would silently replace
    // whatever the player had copied, and the full text is in the log anyway.
    os_clipboard::copy_to_clipboard(assertionInfo);
#endif
}

namespace
{
// What the player chose in the error dialog of xrDebug::Fail
enum class FailChoice : int
{
    Continue,     // return from Fail once: the code after the assertion runs
    IgnoreAlways, // the same, and this file:line is not reported again in this session
    Exit,         // crash report and exit, what every error did before the dialog had these buttons
};

// Depth of Fail on this thread. A failure inside the handling of another one (the stack walk, Sentry, the dialog
// itself) must not open a second dialog on top of the first or recurse until the stack runs out.
thread_local int s_tl_fail_depth = 0;

// Set around a Fail whose caller cannot go on after it returns: a failed allocation (the new handler retries it and
// would fail again), a fatal signal, a pure virtual call. The dialog then offers no Continue.
thread_local bool s_tl_fail_no_continue = false;

struct FailNoContinueScope
{
    const bool previous = s_tl_fail_no_continue;
    FailNoContinueScope() { s_tl_fail_no_continue = true; }
    ~FailNoContinueScope() { s_tl_fail_no_continue = previous; }
};

struct FailDepthScope
{
    FailDepthScope() { ++s_tl_fail_depth; }
    ~FailDepthScope() { --s_tl_fail_depth; }
};

// Errors the player chose to ignore always, by file:line. R_ASSERT* and VERIFY* keep a static flag of their own, which
// Fail sets; this list covers FATAL and the callers that pass a local flag. The file name is copied: the invalid
// parameter handler passes a buffer on its stack. Only touched under failLock.
struct IgnoredFailSite
{
    char file[256];
    int line;
};
constexpr size_t kMaxIgnoredFailSites = 64;
IgnoredFailSite s_ignored_fail_sites[kMaxIgnoredFailSites];
size_t s_ignored_fail_site_count = 0;

bool is_fail_site_ignored(const ErrorLocation& loc)
{
    for (size_t i = 0; i < s_ignored_fail_site_count; ++i)
    {
        const IgnoredFailSite& site = s_ignored_fail_sites[i];
        if (site.line == loc.Line && 0 == std::strcmp(site.file, loc.File ? loc.File : ""))
            return true;
    }
    return false;
}

void ignore_fail_site(const ErrorLocation& loc)
{
    if (s_ignored_fail_site_count >= kMaxIgnoredFailSites || is_fail_site_ignored(loc))
        return;
    IgnoredFailSite& site = s_ignored_fail_sites[s_ignored_fail_site_count++];
    std::snprintf(site.file, sizeof(site.file), "%s", loc.File ? loc.File : "");
    site.line = loc.Line;
}

// The dialog text is fixed English on purpose: the string table lives in the game modules and may be the very thing
// that failed, and it may not be loaded yet (or any more) when an error happens.
constexpr pcstr kFailHeaderContinue =
    "The game has encountered an error. The details below are also written to the log.\n"
    "Continue: ignore the error this time and keep playing. Ignore always: also skip it for the rest of the session. "
    "Continuing may cause further errors or a crash; if the game goes on, save it to a new slot.\n"
    "Exit: close the game and save the crash report.";
constexpr pcstr kFailHeaderNoContinue =
    "The game has encountered an error it cannot recover from. The details below are also written to the log.\n"
    "Exit: close the game and save the crash report.";

#if defined(XR_PLATFORM_WINDOWS)
// Own Win32 window instead of MessageBox: MessageBox has no custom buttons, and a few kilobytes of stack trace need a
// scrollable, selectable text field. The window has no owner and runs its own message loop on the failing thread, so
// it does not depend on the game window's thread (which may be waiting for this one).
constexpr int kFailIdHeader = 100;
constexpr int kFailIdDetails = 101;
constexpr int kFailIdContinue = 102;
constexpr int kFailIdIgnore = 103;
constexpr int kFailIdCopy = 104;
constexpr int kFailIdExit = 105;
constexpr UINT_PTR kFailEnableTimer = 1;
// The player may be clicking or holding a key in the game when the dialog appears: its answer buttons are enabled
// only after a moment, so that input meant for the game does not pick an answer.
constexpr UINT kFailEnableDelayMs = 800;
constexpr wchar_t kFailDialogClass[] = L"XRayFailDialog";

struct FailDialogData
{
    std::wstring header;
    std::wstring details;
    bool allowContinue{};
    int choice{ -1 }; // FailChoice once answered
    bool answersEnabled{};
    float scale{ 1.0f };
    HFONT uiFont{};
    HFONT monoFont{};
    HWND headerWnd{};
    HWND detailsWnd{};
    HWND continueWnd{};
    HWND ignoreWnd{};
    HWND copyWnd{};
    HWND exitWnd{};
};

int fail_px(const FailDialogData& d, int value) { return static_cast<int>(value * d.scale + 0.5f); }

// Win32 edit controls break lines only at CRLF
std::wstring fail_text_to_wide(pcstr text)
{
    xr_string crlf;
    crlf.reserve(std::strlen(text) + 256);
    for (pcstr p = text; *p; ++p)
    {
        if (*p == '\n' && (p == text || p[-1] != '\r'))
            crlf += '\r';
        crlf += *p;
    }
    return XRay::Utf8::ToWide(crlf.c_str());
}

HWND fail_create_child(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, DWORD exStyle, int id,
    HFONT font)
{
    const HWND wnd = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (wnd && font)
        SendMessageW(wnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    return wnd;
}

void fail_layout(FailDialogData& d, HWND wnd)
{
    RECT client{};
    GetClientRect(wnd, &client);
    const int margin = fail_px(d, 12);
    const int gap = fail_px(d, 8);
    const int buttonW = fail_px(d, 130);
    const int buttonH = fail_px(d, 28);
    const int width = std::max(static_cast<int>(client.right) - 2 * margin, 1);

    int headerH = fail_px(d, 64);
    if (const HDC dc = GetDC(wnd))
    {
        const HGDIOBJ previous = d.uiFont ? SelectObject(dc, d.uiFont) : nullptr;
        RECT text{ 0, 0, width, 0 };
        DrawTextW(dc, d.header.c_str(), -1, &text, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        headerH = text.bottom - text.top;
        if (previous)
            SelectObject(dc, previous);
        ReleaseDC(wnd, dc);
    }

    const int buttonsY = client.bottom - margin - buttonH;
    const int detailsY = margin + headerH + gap;
    MoveWindow(d.headerWnd, margin, margin, width, headerH, TRUE);
    MoveWindow(d.detailsWnd, margin, detailsY, width, std::max(buttonsY - gap - detailsY, 0), TRUE);

    // Copy on the left, the answers on the right with Exit last
    MoveWindow(d.copyWnd, margin, buttonsY, buttonW, buttonH, TRUE);
    int x = client.right - margin - buttonW;
    for (const HWND button : { d.exitWnd, d.ignoreWnd, d.continueWnd })
    {
        if (!button)
            continue;
        MoveWindow(button, x, buttonsY, buttonW, buttonH, TRUE);
        x -= buttonW + gap;
    }
}

void fail_answer(FailDialogData& d, HWND wnd, FailChoice choice)
{
    d.choice = static_cast<int>(choice);
    DestroyWindow(wnd);
}

LRESULT CALLBACK fail_dialog_proc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* d = reinterpret_cast<FailDialogData*>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
    switch (msg)
    {
    case WM_NCCREATE:
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        break;
    }
    case WM_CREATE:
    {
        if (!d)
            return -1;
        d->headerWnd = fail_create_child(wnd, L"STATIC", d->header.c_str(), SS_LEFT | SS_NOPREFIX, 0,
            kFailIdHeader, d->uiFont);
        d->detailsWnd = fail_create_child(wnd, L"EDIT", nullptr,
            WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
            WS_EX_CLIENTEDGE, kFailIdDetails, d->monoFont ? d->monoFont : d->uiFont);
        if (d->detailsWnd)
        {
            SendMessageW(d->detailsWnd, EM_SETLIMITTEXT, 0, 0); // no 32K default limit
            SetWindowTextW(d->detailsWnd, d->details.c_str());
        }
        constexpr DWORD answerStyle = WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED;
        if (d->allowContinue)
        {
            d->continueWnd = fail_create_child(wnd, L"BUTTON", L"Continue", answerStyle, 0, kFailIdContinue, d->uiFont);
            d->ignoreWnd = fail_create_child(wnd, L"BUTTON", L"Ignore always", answerStyle, 0, kFailIdIgnore, d->uiFont);
        }
        d->copyWnd = fail_create_child(wnd, L"BUTTON", L"Copy to clipboard", WS_TABSTOP | BS_PUSHBUTTON, 0,
            kFailIdCopy, d->uiFont);
        d->exitWnd = fail_create_child(wnd, L"BUTTON", L"Exit game", answerStyle, 0, kFailIdExit, d->uiFont);
        SetTimer(wnd, kFailEnableTimer, kFailEnableDelayMs, nullptr);
        return 0;
    }
    case WM_TIMER:
        if (d && wParam == kFailEnableTimer)
        {
            KillTimer(wnd, kFailEnableTimer);
            d->answersEnabled = true;
            for (const HWND button : { d->continueWnd, d->ignoreWnd, d->exitWnd })
            {
                if (button)
                    EnableWindow(button, TRUE);
            }
            return 0;
        }
        break;
    case WM_SIZE:
        if (d)
            fail_layout(*d, wnd);
        return 0;
    case WM_GETMINMAXINFO:
        // The first message a window gets, before WM_NCCREATE: d is null then
        if (d)
        {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = fail_px(*d, 600);
            info->ptMinTrackSize.y = fail_px(*d, 320);
            return 0;
        }
        break;
    case WM_COMMAND:
        if (!d)
            break;
        // IDOK and IDCANCEL (Enter and Escape through IsDialogMessage) are not answers: a key the player was
        // pressing in the game must not close the dialog
        switch (LOWORD(wParam))
        {
        case kFailIdContinue:
            if (d->answersEnabled && d->allowContinue)
                fail_answer(*d, wnd, FailChoice::Continue);
            return 0;
        case kFailIdIgnore:
            if (d->answersEnabled && d->allowContinue)
                fail_answer(*d, wnd, FailChoice::IgnoreAlways);
            return 0;
        case kFailIdExit:
            if (d->answersEnabled)
                fail_answer(*d, wnd, FailChoice::Exit);
            return 0;
        case kFailIdCopy:
            // The edit control copies as Unicode text with CRLF, no SDL involved (its clipboard calls belong to
            // the window's thread)
            if (d->detailsWnd)
            {
                SendMessageW(d->detailsWnd, EM_SETSEL, 0, -1);
                SendMessageW(d->detailsWnd, WM_COPY, 0, 0);
                SetFocus(d->detailsWnd);
            }
            return 0;
        default: break;
        }
        break;
    case WM_CLOSE:
        // The close box and Alt+F4 answer Exit, as Escape did in the old dialog
        if (d && d->answersEnabled)
            fail_answer(*d, wnd, FailChoice::Exit);
        return 0;
    default: break;
    }
    return DefWindowProcW(wnd, msg, wParam, lParam);
}

// The old three-button message box, for the case the window above cannot be created
FailChoice show_fail_message_box(const std::wstring& title, const FailDialogData& d)
{
    std::wstring text = d.header;
    text += L"\r\n\r\n";
    if (d.allowContinue)
        text += L"Retry = Continue, Ignore = Ignore always, Abort = Exit.\r\n\r\n";
    text += d.details;
    const UINT type = (d.allowContinue ? MB_ABORTRETRYIGNORE : MB_OK) | MB_ICONERROR | MB_SYSTEMMODAL |
        MB_SETFOREGROUND | MB_TOPMOST;
    switch (MessageBoxW(nullptr, text.c_str(), title.c_str(), type))
    {
    case IDRETRY: return FailChoice::Continue;
    case IDIGNORE: return FailChoice::IgnoreAlways;
    default: return FailChoice::Exit;
    }
}

FailChoice show_fail_dialog(pcstr header, pcstr details, bool allowContinue, bool windowThread, HWND gameWnd)
{
    FailDialogData d;
    d.header = fail_text_to_wide(header);
    d.details = fail_text_to_wide(details);
    d.allowContinue = allowContinue;

    std::wstring title = XRay::Utf8::ToWide(Core.ApplicationTitle[0] ? Core.ApplicationTitle : "OpenXRay");
    title += L" - error";

    // Registered once, under failLock like the rest of the dialog code
    static bool classRegistered = false;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!classRegistered)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = fail_dialog_proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
        wc.lpszClassName = kFailDialogClass;
        classRegistered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    if (!classRegistered)
        return show_fail_message_box(title, d);

    if (const HDC screen = GetDC(nullptr))
    {
        d.scale = std::max(GetDeviceCaps(screen, LOGPIXELSY) / 96.0f, 1.0f);
        ReleaseDC(nullptr, screen);
    }

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
    {
        d.uiFont = CreateFontIndirectW(&metrics.lfMessageFont);
        LOGFONTW mono = metrics.lfMessageFont;
        wcsncpy_s(mono.lfFaceName, L"Consolas", _TRUNCATE);
        mono.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
        d.monoFont = CreateFontIndirectW(&mono);
    }

    // Centred on the monitor of the game window. MonitorFromWindow only reads the window rectangle.
    RECT work{ 0, 0, 1280, 720 };
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromWindow(gameWnd, MONITOR_DEFAULTTOPRIMARY), &monitor))
        work = monitor.rcWork;
    const int workW = work.right - work.left;
    const int workH = work.bottom - work.top;
    const int width = std::min(fail_px(d, 820), workW * 9 / 10);
    const int height = std::min(fail_px(d, 580), workH * 9 / 10);

    // Topmost and without an owner; WS_EX_APPWINDOW gives it a taskbar button of its own
    const HWND wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_APPWINDOW, kFailDialogClass, title.c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        work.left + (workW - width) / 2, work.top + (workH - height) / 2, width, height,
        nullptr, nullptr, instance, &d);

    FailChoice choice = FailChoice::Exit;
    if (!wnd)
        choice = show_fail_message_box(title, d);
    else
    {
        // Modal for the game window only on its own thread: EnableWindow from another thread waits for it
        const bool disableGame = windowThread && gameWnd && IsWindowEnabled(gameWnd);
        if (disableGame)
            EnableWindow(gameWnd, FALSE);

        ShowWindow(wnd, SW_SHOWNORMAL);
        SetForegroundWindow(wnd);
        if (d.detailsWnd)
            SetFocus(d.detailsWnd); // not a button: a key still held in the game must not press one

        // On the window's thread this also dispatches the game window's messages, as MessageBox did
        MSG msg;
        while (d.choice < 0)
        {
            const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
            if (got == 0)
            {
                PostQuitMessage(static_cast<int>(msg.wParam)); // leave WM_QUIT to the game's own loop
                break;
            }
            if (got < 0)
                break;
            if (!IsDialogMessageW(wnd, &msg))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        if (disableGame)
            EnableWindow(gameWnd, TRUE); // before the dialog goes, so that the activation returns to the game
        if (IsWindow(wnd))
            DestroyWindow(wnd);
        if (d.choice >= 0)
            choice = static_cast<FailChoice>(d.choice);
    }

    if (d.uiFont)
        DeleteObject(d.uiFont);
    if (d.monoFont)
        DeleteObject(d.monoFont);
    return choice;
}
#else
FailChoice show_fail_dialog(pcstr header, pcstr details, bool allowContinue, SDL_Window* parent)
{
    xr_string text = header;
    text += "\n\n";
    text += details;

    SDL_MessageBoxButtonData failButtons[3]{};
    int count = 0;
    if (allowContinue)
    {
        failButtons[count++] = { 0, static_cast<int>(FailChoice::Continue), "Continue" };
        failButtons[count++] = { 0, static_cast<int>(FailChoice::IgnoreAlways), "Ignore always" };
    }
    failButtons[count++] = { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, static_cast<int>(FailChoice::Exit), "Exit game" };

    const SDL_MessageBoxData data =
    {
        SDL_MESSAGEBOX_ERROR, parent, "Error", text.c_str(), count, failButtons, nullptr
    };

    int button = -1;
    if (SDL_ShowMessageBox(&data, &button) < 0)
    {
        Msg("! [debug] the error dialog failed: %s", SDL_GetError());
        return FailChoice::Exit;
    }
    switch (button)
    {
    case static_cast<int>(FailChoice::Continue): return FailChoice::Continue;
    case static_cast<int>(FailChoice::IgnoreAlways): return FailChoice::IgnoreAlways;
    default: return FailChoice::Exit; // closed without an answer
    }
}
#endif // XR_PLATFORM_WINDOWS
} // namespace

void xrDebug::Fatal(const ErrorLocation& loc, const char* format, ...)
{
    string1024 desc;
    va_list args;
    va_start(args, format);
    vsnprintf(desc, sizeof(desc), format, args);
    va_end(args);
    bool ignoreAlways = true;
    Fail(ignoreAlways, loc, nullptr, "fatal error", desc);
}

AssertionResult xrDebug::Fail(bool& ignoreAlways, const ErrorLocation& loc, const char* expr, long hresult, const char* arg1,
                   const char* arg2)
{
    return Fail(ignoreAlways, loc, expr, xrDebug::ErrorToString(hresult), arg1, arg2);
}

AssertionResult xrDebug::Fail(bool& ignoreAlways, const ErrorLocation& loc, const char* expr, const char* desc, const char* arg1,
                   const char* arg2)
{
    // A second thread failing meanwhile waits here until the first dialog is answered: one dialog at a time.
    ScopeLock lock(&failLock);

    const bool allowContinue = !s_tl_fail_no_continue;
    pcstr const file = loc.File ? loc.File : "<unknown file>";

    // Failed again while this thread handles a failure (the lock is recursive): log it and go on with the outer one.
    // A failure that does not allow going on is taken straight to the exit below, without a dialog.
    const bool nested = s_tl_fail_depth > 0;
    if (nested)
    {
        Msg("! [debug] error while handling another error, no dialog: %s | %s at %s:%d", expr ? expr : "",
            desc ? desc : "", file, loc.Line);
        FlushLog();
        if (allowContinue)
            return AssertionResult::tryAgain;
    }
    else if (allowContinue && is_fail_site_ignored(loc))
    {
        ignoreAlways = true;
        return AssertionResult::ignore;
    }

    const FailDepthScope depth;

    // Only after locking, so that two threads never change the window at once
    const bool dialogPrepared = !nested && begin_error_dialog(windowHandler);

    ErrorAfterDialog = true;
    string4096 assertionInfo;
    if (nested)
        std::snprintf(assertionInfo, sizeof(assertionInfo), "%s", desc ? desc : "error while handling another error");
    else
    {
        GatherInfo(assertionInfo, sizeof(assertionInfo), loc, expr, desc, arg1, arg2);
        // Captured before the dialog: the report is queued even if the process never gets past it. No flush here.
        xrSentry_CaptureDebugFail(expr, desc, arg1, arg2);
    }

    string256 threadName{};
    get_current_thread_name(threadName, sizeof(threadName));
    Msg("! [debug] thread: 0x%llX \"%s\"", static_cast<unsigned long long>(get_current_thread_id()), threadName);
    FlushLog();

    FailChoice choice = FailChoice::Exit;
    bool dialogShown = false;
    AssertionResult result = AssertionResult::abort;
    if (Core.PluginMode)
        /*result =*/ ShowMessage("X-Ray error", assertionInfo); // Do not assign 'result'
    else
    {
        // No dialog (Master Gold without -show_error_window): exit, as before
        if (ShowErrorMessage && !nested)
        {
            pcstr const logPath = log_name();
            string_path logLine{};
            std::snprintf(logLine, sizeof(logLine), "%s", logPath && *logPath ? logPath : "<not created yet>");

            xr_string details;
            details.reserve(sizeof(assertionInfo) + 512);
            details += "Thread: ";
            string64 threadId;
            std::snprintf(threadId, sizeof(threadId), "0x%llX ", static_cast<unsigned long long>(get_current_thread_id()));
            details += threadId;
            details += '"';
            details += threadName;
            details += "\"\nLog: ";
            details += logLine;
            details += '\n';
            details += assertionInfo; // starts with a line break

            pcstr const header = allowContinue ? kFailHeaderContinue : kFailHeaderNoContinue;
            const bool windowThread = on_window_thread();
#if defined(XR_PLATFORM_WINDOWS)
            const auto gameWnd = windowHandler ? static_cast<HWND>(windowHandler->GetApplicationWindowHandle()) : nullptr;
            choice = show_fail_dialog(header, details.c_str(), allowContinue, windowThread, gameWnd);
#else
            SDL_Window* parent = windowHandler && windowThread ? windowHandler->GetApplicationWindow() : nullptr;
            choice = show_fail_dialog(header, details.c_str(), allowContinue, parent);
#endif
            dialogShown = true;
            if (!allowContinue)
                choice = FailChoice::Exit;
        }

        switch (choice)
        {
        case FailChoice::Continue:
            Msg("! [debug] fatal error ignored by the user (Continue) at %s:%d", file, loc.Line);
            result = AssertionResult::tryAgain;
            break;

        case FailChoice::IgnoreAlways:
            Msg("! [debug] fatal error ignored by the user (Ignore always) at %s:%d", file, loc.Line);
            ignore_fail_site(loc);
            ignoreAlways = true;
            result = AssertionResult::ignore;
            break;

        case FailChoice::Exit:
        default:
            if (dialogShown)
                Msg("! [debug] fatal error: the user chose Exit at %s:%d", file, loc.Line);
            FlushLog();
            // The capture above only queued the report; send it before the process goes down
            constexpr u32 kSentryExitFlushMs = 10000;
            xrSentry_Flush(kSentryExitFlushMs);
#ifdef USE_BUG_TRAP
            BT_SetUserMessage(assertionInfo);
#endif
            // calling DEBUG_BREAK with no debugger will trigger BugTrap
            // we must hide the window
            if (!DebuggerIsPresent())
                hide_window_before_exit(windowHandler);
            DEBUG_BREAK;
            break;
        } // switch (choice)

        if (result != AssertionResult::abort)
        {
            ErrorAfterDialog = false;
            FlushLog();
#if defined(XR_PLATFORM_WINDOWS)
            // Give the focus back to the game. Unlike ShowWindow, SetForegroundWindow does not wait for the window's
            // thread; on that thread the dialog's EnableWindow already returned the activation.
            if (dialogShown && windowHandler && !on_window_thread())
            {
                if (const auto gameWnd = static_cast<HWND>(windowHandler->GetApplicationWindowHandle()))
                    SetForegroundWindow(gameWnd);
            }
#endif
        }
    }

    if (dialogPrepared && result != AssertionResult::abort)
        windowHandler->OnErrorDialog(false); // Call it only before unlocking so that multiple threads won't call this function simultaneously.

    return result;
}

AssertionResult xrDebug::Fail(bool& ignoreAlways, const ErrorLocation& loc, const char* expr, const std::string& desc,
                   const char* arg1, const char* arg2)
{
    return Fail(ignoreAlways, loc, expr, desc.c_str(), arg1, arg2);
}

[[noreturn]]
void xrDebug::DoExit(const std::string& message)
{
    ScopeLock lock(&failLock);

    // The window is changed only from its own thread (see s_window_thread)
    const bool dialogPrepared = begin_error_dialog(windowHandler);

    FlushLog();

    if (ShowErrorMessage)
    {
        const auto result = ShowMessage(Core.ApplicationName, message.c_str(), false);
        if (result != AssertionResult::abort && DebuggerIsPresent())
            DEBUG_BREAK;
    }
    else
        ShowMessage(Core.ApplicationName, message.c_str());

    hide_window_before_exit(windowHandler);

#if defined(XR_PLATFORM_WINDOWS)
    TerminateProcess(GetCurrentProcess(), 1);
#else
    exit(1);
#endif
    // if you're under debugger, you can jump here manually
    if (dialogPrepared)
        windowHandler->OnErrorDialog(false);
}

pcstr xrDebug::ErrorToString(long code)
{
    const char* result = nullptr;
#if defined(XR_PLATFORM_WINDOWS)
    static string1024 descStorage;
    DXGetErrorDescription(code, descStorage, sizeof(descStorage));
    if (!result)
    {
        FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, 0, code, 0, descStorage, sizeof(descStorage) - 1, 0);
        result = descStorage;
    }
#endif
    return result;
}

int out_of_memory_handler(size_t size)
{
    xrDebug::OutOfMemoryCallbackFunc cb = xrDebug::GetOutOfMemoryCallback();
    if (cb)
        cb();
    else
    {
        Memory.mem_compact();
        const size_t processHeap = Memory.mem_usage();
        const auto [ecoStringsBytes, ecoStringsCount] = g_pStringContainer->stat_economy();
        const size_t ecoSmem = g_pSharedMemoryContainer->stat_economy();
        Msg("* [x-ray]: process heap[%zu K]", processHeap / 1024);
        Msg("* [x-ray]: shared strings: memory[%ld K], count[%lu]", ecoStringsBytes / 1024, ecoStringsCount);
        Msg("* [x-ray]: shared memory[%ld K]", ecoSmem);
    }
    // The new handler retries the allocation when it returns: going on would only bring the dialog back
    const FailNoContinueScope noContinue;
    xrDebug::Fatal(DEBUG_INFO, "Out of memory. Memory request: %zu K", size / 1024);
    return 1;
}

void WINAPI xrDebug::PreErrorHandler(INT_PTR)
{
#if defined(USE_BUG_TRAP) && defined(XR_PLATFORM_WINDOWS)
    if (xr_FS && FS.m_Flags.test(CLocatorAPI::flReady))
    {
        string_path cfg_full_name;
        __try
        {
            // Code below copied from CCC_LoadCFG::Execute (xr_ioc_cmd.cpp)
            // XXX: Refactor Console to accept user config filename on initialization or even construction!
            // XXX: Maybe refactor CCC_LoadCFG, move code for loading user.ltx into a generic function
            const auto cfg_name = userConfigHandler ? userConfigHandler->GetUserConfigFileName() : "user.ltx";
            FS.update_path(cfg_full_name, "$app_data_root$", cfg_name);

            if (!FS.exist(cfg_full_name))
                FS.update_path(cfg_full_name, "$fs_root$", cfg_name);

            if (!FS.exist(cfg_full_name))
                xr_strcpy(cfg_full_name, cfg_name);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            xr_strcpy(cfg_full_name, "user.ltx");
        }
        BT_AddLogFile(cfg_full_name);
    }

    if (*BugReportFile)
        BT_AddLogFile(BugReportFile);

    BT_SaveSnapshot(nullptr);
#endif
}

void xrDebug::SetupExceptionHandler()
{
#if defined(XR_PLATFORM_WINDOWS)
    // disable 'appname has stopped working' popup dialog (BugTrap or Sentry)
    const auto prevMode = SetErrorMode(SEM_NOGPFAULTERRORBOX);
    SetErrorMode(prevMode | SEM_NOGPFAULTERRORBOX);
#endif
#if defined(USE_BUG_TRAP) && defined(XR_PLATFORM_WINDOWS)
    const auto commandLine = GetCommandLine();

    BT_InstallSehFilter();

    if (!GEnv.isDedicatedServer && !strstr(commandLine, "-silent_error_mode"))
        BT_SetActivityType(BTA_SHOWUI);
    else
        BT_SetActivityType(BTA_SAVEREPORT);
    BT_SetDialogMessage(BTDM_INTRO2,
                        "This is OpenXRay crash reporting client. "
                        "To help the development process, "
                        "please Submit Bug or save report and email it manually (button More...)."
                        "\r\n"
                        "Many thanks in advance and sorry for the inconvenience.");
    BT_SetPreErrHandler(PreErrorHandler, 0);
    BT_SetAppName("OpenXRay");
    BT_SetReportFormat(BTRF_TEXT);
    BT_SetFlags(BTF_DETAILEDMODE | BTF_ATTACHREPORT);

    auto minidumpFlags = MiniDumpWithDataSegs|
        MiniDumpWithIndirectlyReferencedMemory |
        MiniDumpScanMemory |
        MiniDumpWithProcessThreadData |
        MiniDumpWithThreadInfo;

    if (strstr(commandLine, "-full_memory_dump"))
        minidumpFlags |= MiniDumpWithFullMemory | MiniDumpIgnoreInaccessibleMemory;
    else if (strstr(commandLine, "-detailed_minidump"))
        minidumpFlags |= MiniDumpWithIndirectlyReferencedMemory;

    BT_SetDumpType(minidumpFlags);
    //BT_SetSupportEMail("cop-crash-report@stalker-game.com");
    BT_SetSupportEMail("openxray@yahoo.com");
    BT_SetSupportURL("https://github.com/OpenXRay/xray-16/issues");
#endif
}

void xrDebug::OnFilesystemInitialized()
{
#ifdef USE_BUG_TRAP
    string_path path{}, path2{};
    FS.update_path(path, "$logs$", "", false);
    FS.update_path(path2, "$checks$", "", false);
    FS.update_path(path2, "$dictionaries$", "", false);
    if (!path[0] || path[0] != _DELIMITER && path[1] != ':') // relative path
    {
        string_path currentDir;
        _getcwd(currentDir, sizeof(currentDir));
        string_path relDir;
        xr_strcpy(relDir, path);
        strconcat(path, currentDir, DELIMITER, relDir);
    }
    xr_strcat(path, log_name());
    BT_AddLogFile(path);

    if (FS.update_path(path, "$app_data_root$", "reports", false))
    {
        BT_SetReportFilePath(path);
    }
#endif
}

bool xrDebug::DebuggerIsPresent()
{
#ifdef XR_PLATFORM_WINDOWS
    return IsDebuggerPresent();
#elif defined(PTRACE_AVAILABLE)
    if (ptrace(PTRACE_TRACEME, 0, 0, 0) == -1)
        return true;
    ptrace(PTRACE_DETACH, 0, 0, 0);
    return false;
#else
    return false;
#endif
}

void xrDebug::FormatLastError(char* buffer, const size_t& bufferSize)
{
#if defined(XR_PLATFORM_WINDOWS)
    const int lastErr = GetLastError();
    if (lastErr == ERROR_SUCCESS)
    {
        *buffer = 0;
        return;
    }
    void* msg = nullptr;
    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, nullptr, lastErr,
                  MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (pstr)&msg, 0, nullptr);
    // snprintf, not xr_sprintf: the system message has no length bound, and vsprintf_s reports a
    // buffer that is too small through the invalid parameter handler, which calls Fail and recurses.
    // This runs on the crash path, before the crash block is written.
    std::snprintf(buffer, bufferSize, "[error][%8d]: %s", lastErr,
        msg ? static_cast<const char*>(msg) : "<no description>");
    if (msg)
        LocalFree(msg);
#endif
}

LONG WINAPI xrDebug::UnhandledFilter(EXCEPTION_POINTERS* exPtrs)
{
#if defined(XR_PLATFORM_WINDOWS)
    ScopeLock lock(&failLock);

    // Read before anything else: every WinAPI call below overwrites the thread's last error.
    string256 errMsg{};
    FormatLastError(errMsg, sizeof(errMsg));

    // Reached only when Sentry is disabled or failed to start: with Crashpad running, its filter
    // is installed on top of this one and terminates the process before returning here.
    // LogCrashInfo is shared with the Sentry local-report filter and logs at most once per process.
    if (!ErrorAfterDialog)
        LogCrashInfoGuarded(exPtrs);

    if (*errMsg)
        Msg("! [crash] last error: %s", errMsg);

    FlushLog();

    // The crash may be on any thread: the window is changed only from its own one (see s_window_thread)
    const bool dialogPrepared = begin_error_dialog(windowHandler);

    static constexpr pcstr fatalError = "Fatal error";

    AssertionResult msgRes = AssertionResult::abort;

    if (!ErrorAfterDialog && ShowErrorMessage)
    {
        static constexpr pcstr msg = "Fatal error occurred\n\n"
            "Press OK to abort program execution";
        msgRes = ShowMessage(fatalError, msg);
    }

#ifdef USE_BUG_TRAP
    BT_SetUserMessage(fatalError);
    BT_SaveSnapshotEx(exPtrs, nullptr);
#endif

#if !defined(USE_SENTRY)
    const auto reportRes = ReportFault(exPtrs, 0);
#else
    const auto reportRes = frrvOk;
#endif
    if (msgRes != AssertionResult::abort ||
        reportRes == frrvLaunchDebugger)
    {
        constexpr cpcstr debugger = "Please, attach the debugger to the process"
            " if you want to debug this fatal error.";
        ShowMessage("xrDebug", debugger);
        if (DebuggerIsPresent())
            DEBUG_BREAK;
    }

    // Typically, PrevFilter is BugTrap filter
    if (PrevFilter)
    {
        hide_window_before_exit(windowHandler);
        PrevFilter(exPtrs);
    }

    if (dialogPrepared)
        windowHandler->OnErrorDialog(false);

    return EXCEPTION_CONTINUE_SEARCH;
#else
    return 0;
#endif
}

#ifndef USE_BUG_TRAP
[[noreturn]]
void xr_terminate()
{
#if defined(XR_PLATFORM_WINDOWS)
    if (strstr(GetCommandLine(), "-silent_error_mode"))
    {
        // Without this the tail of the log stays in the CRT buffer and the report loses its last lines.
        Log("! [crash] unexpected application termination (silent error mode)");
        FlushLog();
        exit(-1);
    }
#endif
    //ScopeLock lock(&failLock);

    string4096 assertionInfo;
    xrDebug::GatherInfo(assertionInfo,sizeof(assertionInfo), DEBUG_INFO, nullptr, "Unexpected application termination");
    append_tail_clamped(assertionInfo, sizeof(assertionInfo), "Press OK to abort execution\r\n");
    xrDebug::ShowMessage("Fatal Error", assertionInfo);
    exit(-1);
}
#endif // USE_BUG_TRAP

static void handler_base(const char* reason)
{
    // Signals and the pure virtual call handler: the CRT aborts or the faulting instruction runs again on return
    const FailNoContinueScope noContinue;
    bool ignoreAlways = false;
    xrDebug::Fail(ignoreAlways, DEBUG_INFO, nullptr, reason, nullptr, nullptr);
}

#if defined(XR_PLATFORM_WINDOWS)
static void invalid_parameter_handler(const wchar_t* expression, const wchar_t* function, const wchar_t* file,
                                      unsigned int line, uintptr_t reserved)
{
    bool ignoreAlways = false;
    string4096 mbExpression;
    string4096 mbFunction;
    string4096 mbFile;
    size_t convertedChars = 0;
    if (expression)
        wcstombs_s(&convertedChars, mbExpression, sizeof(mbExpression), expression, (wcslen(expression) + 1) * 2);
    else
        xr_strcpy(mbExpression, "");
    if (function)
        wcstombs_s(&convertedChars, mbFunction, sizeof(mbFunction), function, (wcslen(function) + 1) * 2);
    else
        xr_strcpy(mbFunction, __FUNCTION__);
    if (file)
        wcstombs_s(&convertedChars, mbFile, sizeof(mbFile), file, (wcslen(file) + 1) * 2);
    else
    {
        line = __LINE__;
        xr_strcpy(mbFile, __FILE__);
    }
    xrDebug::Fail(ignoreAlways, {mbFile, int(line), mbFunction}, mbExpression, "invalid parameter");
}
#endif

void xrDebug::OnThreadSpawn()
{
#ifndef __SANITIZE_ADDRESS__
    std::signal(SIGINT,  nullptr);
    std::signal(SIGILL,  +[](int signal) { handler_base("illegal instruction"); });
    std::signal(SIGFPE,  +[](int signal) { handler_base("floating point error"); });
#   ifdef DEBUG
    std::signal(SIGSEGV, +[](int signal) { handler_base("segmentation fault"); });
#   endif
    std::signal(SIGABRT, +[](int signal) { handler_base("application is aborting"); });
    std::signal(SIGTERM, +[](int signal) { handler_base("termination with exit code 3"); });

#   if defined(XR_PLATFORM_WINDOWS)
    std::signal(SIGABRT_COMPAT, +[](int signal) { handler_base("application is aborting"); });
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler(&invalid_parameter_handler);
    _set_new_mode(1);
    _set_new_handler(&out_of_memory_handler);
    _set_purecall_handler(+[] { handler_base("pure virtual function call"); });
#   endif

#   ifdef USE_BUG_TRAP
    BT_SetTerminate();
#   else
    std::set_terminate(xr_terminate);
#   endif
#endif
}

void xrDebug::OnThreadExit()
{
#ifndef __SANITIZE_ADDRESS__
    std::signal(SIGINT,  nullptr);
    std::signal(SIGILL,  nullptr);
    std::signal(SIGFPE,  nullptr);
    std::signal(SIGSEGV, nullptr);
    std::signal(SIGABRT, nullptr);
    std::signal(SIGTERM, nullptr);
    std::set_terminate(nullptr);

#   if defined(XR_PLATFORM_WINDOWS)
    std::signal(SIGABRT_COMPAT, nullptr);
    _set_abort_behavior(0, 0);
    _set_invalid_parameter_handler(nullptr);
    _set_new_mode(1);
    _set_new_handler(nullptr);
    _set_purecall_handler(nullptr);
#   endif
#endif
}

void xrDebug::Initialize(pcstr commandLine)
{
    ZoneScoped;
    s_sentry_command_line = commandLine;
    *BugReportFile = 0;
    OnThreadSpawn();
    SetupExceptionHandler();
    SDL_SetAssertionHandler(SDLAssertionHandler, nullptr);
    // exception handler to all "unhandled" exceptions
#if defined(XR_PLATFORM_WINDOWS)
    PreloadStackTraceLibrary(); // keep LoadLibrary off the crash path, it takes the loader lock
    PrevFilter = SetUnhandledExceptionFilter(UnhandledFilter);
#endif
#ifdef MASTER_GOLD
    ShowErrorMessage = commandLine ? !!strstr(commandLine, "-show_error_window") : false;
#endif
}

void xrDebug::InitializeSentry(pcstr commandLine)
{
#if defined(USE_SENTRY)
    const pcstr cmd = commandLine ? commandLine : s_sentry_command_line;
    xrSentry_Initialize(cmd);
#else
    (void)commandLine;
#endif
}

void xrDebug::Finalize()
{
    OnThreadExit();
    SDL_SetAssertionHandler(nullptr, nullptr);
#if defined(USE_SENTRY)
    // Shut down Sentry (Crashpad) before removing the process exception filter.
    xrSentry_Shutdown();
#endif
#if defined(XR_PLATFORM_WINDOWS)
    SetUnhandledExceptionFilter(nullptr);
#endif
#ifdef MASTER_GOLD
    ShowErrorMessage = false;
#endif
}
