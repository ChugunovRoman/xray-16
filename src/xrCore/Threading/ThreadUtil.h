#pragma once

#include <thread>

namespace Threading
{
enum class priority_class
{
    idle,
    below_normal,
    normal,
    above_normal,
    high,
    realtime,
};

enum class priority_level
{
    idle,
    lowest,
    below_normal,
    normal,
    above_normal,
    highest,
    time_critical,
};

XRCORE_API priority_level GetCurrentThreadPriorityLevel();
XRCORE_API priority_class GetCurrentProcessPriorityClass();

XRCORE_API void SetCurrentThreadPriorityLevel(priority_level prio);
// GW (plans/optimization_mt 50, R0): background processing mode of the calling thread - low I/O and memory priority
// (Windows THREAD_MODE_BACKGROUND_BEGIN). For threads that only read files ahead. false = not done (other platforms
// or an error); the thread keeps the priority level set before. R15 (plan 50g): the thread also gets EcoQoS
// (SetCurrentThreadEcoQos below) whatever the result - the two callers are exactly the background readers.
XRCORE_API bool SetCurrentThreadBackgroundMode();
// GW (plans/optimization_mt 50g, R15): Windows power throttling (QoS) of the calling thread, Windows 10 1709+.
// SetThreadInformation is loaded at run time: on Windows 7/8 (and other platforms) these do nothing and return false.
// The first call of each kind logs its result once.
// High QoS - opted out of EcoQoS: the scheduler does not move the thread to the efficiency cores of a hybrid CPU or
// lower its clock when it decides the process is in the background or power saving is on. For the threads on the
// frame's critical path: main, Game logic, Physics.
XRCORE_API bool SetCurrentThreadHighQos();
// EcoQoS: the thread runs at the most efficient clock, on the efficiency cores of a hybrid CPU when it can. For the
// threads that only read files ahead.
XRCORE_API bool SetCurrentThreadEcoQos();
XRCORE_API void SetCurrentProcessPriorityClass(priority_class cls);

XRCORE_API void SetCurrentThreadName(cpcstr name);

template <typename Invocable, typename... Args>
[[nodiscard]] std::thread RunThread(cpcstr name, Invocable&& invocable, Args&&... args)
{
    return std::move(std::thread
    {
        [name](Invocable&& invocable2, Args&&... args2)
        {
            SetCurrentThreadName(name);
            _initialize_cpu_thread();
            std::invoke(std::move(invocable2), std::move(args2)...);
        },
        std::forward<Invocable>(invocable),
        std::forward<Args>(args)...
    });
}

template <typename... Args>
void SpawnThread(Args&&... args)
{
    RunThread(std::forward<Args>(args)...).detach();
}
} // namespace Threading
