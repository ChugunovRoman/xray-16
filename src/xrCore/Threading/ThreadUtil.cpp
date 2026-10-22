#include "stdafx.h"
#include "ThreadUtil.h"
#include "xrCore/Profiling/SysDiag.hpp"

#include <atomic>

#if defined(XR_PLATFORM_POSIX)
#   include <pthread.h>

#   if defined(XR_PLATFORM_OPENBSD) || (defined(XR_PLATFORM_FREEBSD) && __FreeBSD_version < 1201519)
#   include <pthread_np.h>

static int pthread_setname_np(pthread_t threadId, const char* name)
{
    pthread_set_name_np(threadId, name);
    return 0;
}
#   elif defined(XR_PLATFORM_NETBSD)
static int pthread_setname_np(pthread_t threadId, const char* name)
{
    return pthread_setname_np(threadId, "%s", const_cast<char*>(name));
}
#   elif defined(XR_PLATFORM_APPLE)
static int pthread_setname_np(pthread_t /*threadId*/, const char* name)
{
    return pthread_setname_np(name);
}
#   elif defined(XR_PLATFORM_HAIKU)
#include <OS.h>

static int pthread_setname_np(pthread_t /*threadId*/, const char* name)
{
    thread_id this_thread = find_thread(NULL);
    return rename_thread(this_thread, name);
}
#   endif
#endif

namespace Threading
{
#if defined(XR_PLATFORM_WINDOWS)
namespace
{
// R15 (plans/optimization_mt 50g): THREAD_POWER_THROTTLING_STATE and the ThreadPowerThrottling class of the Windows
// 10 SDK spelled out - the function is looked up by name (it is not in kernel32 of Windows 7), so nothing here may
// depend on the SDK headers or _WIN32_WINNT.
struct ThreadUtilPowerThrottlingState
{
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
};
constexpr int THREAD_UTIL_POWER_THROTTLING_CLASS = 3; // THREAD_INFORMATION_CLASS::ThreadPowerThrottling
constexpr ULONG THREAD_UTIL_POWER_THROTTLING_VERSION = 1; // THREAD_POWER_THROTTLING_CURRENT_VERSION
constexpr ULONG THREAD_UTIL_POWER_THROTTLING_EXECUTION_SPEED = 0x1; // THREAD_POWER_THROTTLING_EXECUTION_SPEED
using ThreadUtilSetThreadInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);

// ControlMask = EXECUTION_SPEED: the thread decides itself (the system heuristics no longer apply to it);
// StateMask = EXECUTION_SPEED - throttle (EcoQoS), 0 - never throttle (high QoS)
bool SetCurrentThreadPowerThrottling(bool eco)
{
    static const auto set_thread_information = reinterpret_cast<ThreadUtilSetThreadInformationFn>(
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadInformation"));
    static std::atomic<bool> logged[2]{};

    const pcstr kind = eco ? "EcoQoS" : "high QoS (power throttling opted out)";
    DWORD error = 0;
    bool ok = false;
    if (set_thread_information)
    {
        ThreadUtilPowerThrottlingState state{ THREAD_UTIL_POWER_THROTTLING_VERSION,
            THREAD_UTIL_POWER_THROTTLING_EXECUTION_SPEED, eco ? THREAD_UTIL_POWER_THROTTLING_EXECUTION_SPEED : 0u };
        ok = !!set_thread_information(GetCurrentThread(), THREAD_UTIL_POWER_THROTTLING_CLASS, &state, sizeof(state));
        if (!ok)
            error = GetLastError(); // ERROR_INVALID_PARAMETER before Windows 10 1709
    }
    if (!logged[eco ? 1 : 0].exchange(true, std::memory_order_relaxed))
    {
        if (ok)
            Msg("* [hw] thread QoS: %s set", kind);
        else if (!set_thread_information)
            Msg("~ [hw] thread QoS: %s not set - no SetThreadInformation (Windows 7)", kind);
        else
            Msg("~ [hw] thread QoS: %s not set - error %u (needs Windows 10 1709+)", kind, error);
    }
    return ok;
}
} // namespace

void SetThreadNameImpl(pcstr name)
{
    string256 fullName;
    strconcat(fullName, "X-Ray ", name);

    using SetThreadDescriptionProc = decltype(&SetThreadDescription);
    static auto kernelHandle = GetModuleHandleA("kernel32.dll");
    static auto setThreadDescription = reinterpret_cast<SetThreadDescriptionProc>(GetProcAddress(kernelHandle, "SetThreadDescription"));

    if (setThreadDescription)
    {
        wchar_t buf[256];
        mbstowcs(buf, fullName, 256);

        setThreadDescription(GetCurrentThread(), buf);
    }
    else __try
    {
        constexpr DWORD MSVC_EXCEPTION = 0x406D1388;

        struct SThreadNameInfo
        {
            DWORD  dwType{ 0x1000 };
            LPCSTR szName{};
            DWORD  dwThreadID{ DWORD(-1) };
            DWORD  dwFlags{};
        };

        SThreadNameInfo info;
        info.szName = fullName;

        RaiseException(MSVC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
    }
    __except (EXCEPTION_CONTINUE_EXECUTION) {}
}

void SetCurrentThreadName(cpcstr name)
{
    SetThreadNameImpl(name);
    xray::perf::ThreadDiagRegister(name); // 50z5-extra-diagnostics.md: the CPU of the thread in the [sys] lines
#ifdef TRACY_ENABLE
    tracy::SetThreadName(name);
#endif
}

priority_level GetCurrentThreadPriorityLevel()
{
    switch (GetThreadPriority(GetCurrentThread()))
    {
    case THREAD_PRIORITY_IDLE:          return priority_level::idle;
    case THREAD_PRIORITY_LOWEST:        return priority_level::lowest;
    case THREAD_PRIORITY_BELOW_NORMAL:  return priority_level::below_normal;
    default: [[fallthrough]];
    case THREAD_PRIORITY_NORMAL:        return priority_level::normal;
    case THREAD_PRIORITY_ABOVE_NORMAL:  return priority_level::above_normal;
    case THREAD_PRIORITY_HIGHEST:       return priority_level::highest;
    case THREAD_PRIORITY_TIME_CRITICAL: return priority_level::time_critical;
    }
}

priority_class GetCurrentProcessPriorityClass()
{
   switch (GetPriorityClass(GetCurrentProcess()))
   {
   case IDLE_PRIORITY_CLASS:         return priority_class::idle;
   case BELOW_NORMAL_PRIORITY_CLASS: return priority_class::below_normal;
   default: [[fallthrough]];
   case NORMAL_PRIORITY_CLASS:       return priority_class::normal;
   case ABOVE_NORMAL_PRIORITY_CLASS: return priority_class::above_normal;
   case HIGH_PRIORITY_CLASS:         return priority_class::high;
   case REALTIME_PRIORITY_CLASS:     return priority_class::realtime;
   }
}

void SetCurrentThreadPriorityLevel(priority_level prio)
{
    int nPriority;
    switch (prio)
    {
    case priority_level::idle:          nPriority = THREAD_PRIORITY_IDLE; break;
    case priority_level::lowest:        nPriority = THREAD_PRIORITY_LOWEST; break;
    case priority_level::below_normal:  nPriority = THREAD_PRIORITY_BELOW_NORMAL; break;
    default: [[fallthrough]];
    case priority_level::normal:        nPriority = THREAD_PRIORITY_NORMAL; break;
    case priority_level::above_normal:  nPriority = THREAD_PRIORITY_ABOVE_NORMAL; break;
    case priority_level::highest:       nPriority = THREAD_PRIORITY_HIGHEST; break;
    case priority_level::time_critical: nPriority = THREAD_PRIORITY_TIME_CRITICAL; break;
    }
    // GW (plans/optimization_mt 49, W1): the requested level. Upstream passed THREAD_PRIORITY_TIME_CRITICAL here
    // whatever was asked, so the "below normal" I/O threads preempted main and the workers on a 4-thread CPU.
    SetThreadPriority(GetCurrentThread(), nPriority);
}

bool SetCurrentThreadBackgroundMode()
{
    // Works for the current thread only (GetCurrentThread). The system lowers the resource scheduling priorities of the
    // thread (I/O priority very low, memory priority low) until THREAD_MODE_BACKGROUND_END or the thread ends.
    const bool background = !!SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    // R15: and the most efficient clock / cores for it (the callers are the background readers only)
    SetCurrentThreadEcoQos();
    return background;
}

bool SetCurrentThreadHighQos() { return SetCurrentThreadPowerThrottling(false); }
bool SetCurrentThreadEcoQos() { return SetCurrentThreadPowerThrottling(true); }

void SetCurrentProcessPriorityClass(priority_class cls)
{
    DWORD dwPriorityClass;
    switch (cls)
    {
    case priority_class::idle:         dwPriorityClass = IDLE_PRIORITY_CLASS; break;
    case priority_class::below_normal: dwPriorityClass = BELOW_NORMAL_PRIORITY_CLASS; break;
    default: [[fallthrough]];
    case priority_class::normal:       dwPriorityClass = NORMAL_PRIORITY_CLASS; break;
    case priority_class::above_normal: dwPriorityClass = ABOVE_NORMAL_PRIORITY_CLASS; break;
    case priority_class::high:         dwPriorityClass = HIGH_PRIORITY_CLASS; break;
    case priority_class::realtime:     dwPriorityClass = REALTIME_PRIORITY_CLASS; break;
    }
    SetPriorityClass(GetCurrentProcess(), dwPriorityClass);
}
#elif defined(XR_PLATFORM_POSIX)
void SetCurrentThreadName(cpcstr name)
{
    xray::perf::ThreadDiagRegister(name); // 50z5-extra-diagnostics.md: the CPU of the thread in the [sys] lines
    if (auto error = pthread_setname_np(pthread_self(), name) != 0)
    {
        Msg("SetCurrentThreadName: failed to set thread name to '%s'. Errno: '%d'", name, error);
    }
#ifdef TRACY_ENABLE
    tracy::SetThreadName(name);
#endif
}

priority_level GetCurrentThreadPriorityLevel()
{
    return priority_level::normal;
}

priority_class GetCurrentProcessPriorityClass()
{
    return priority_class::normal;
}

void SetCurrentThreadPriorityLevel(priority_level prio)
{

}

bool SetCurrentThreadBackgroundMode() { return false; } // as the priority functions above: not done on POSIX yet
// R15: no per-thread QoS API on POSIX (Linux EAS / macOS QoS classes are not used yet)
bool SetCurrentThreadHighQos() { return false; }
bool SetCurrentThreadEcoQos() { return false; }

void SetCurrentProcessPriorityClass(priority_class cls)
{

}
#else
void SetCurrentThreadName(cpcstr name) {}

priority_level GetCurrentThreadPriorityLevel() { return priority_level::normal; }
priority_class GetCurrentProcessPriorityClass() { return priority_class::normal; }

void SetCurrentThreadPriorityLevel(priority_level prio) {}
bool SetCurrentThreadBackgroundMode() { return false; }
bool SetCurrentThreadHighQos() { return false; }
bool SetCurrentThreadEcoQos() { return false; }
void SetCurrentProcessPriorityClass(priority_class cls) {}
#endif
} // namespace Threading
