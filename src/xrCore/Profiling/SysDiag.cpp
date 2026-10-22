#include "stdafx.h"
#include "xrCore/Profiling/SysDiag.hpp"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "xrCore/Platform/HwProfile.h"
#include "xrCore/Threading/ThreadUtil.h"
#include "xrCore/Threading/TaskManager.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <SDL.h>

#if defined(XR_PLATFORM_WINDOWS)
#   include <pdh.h>
#else
#   include <sys/resource.h>
#   include <unistd.h>
#   include <ctime>
#   if defined(XR_PLATFORM_LINUX)
#       include <pthread.h>
#   endif
#endif

// 50z5-extra-diagnostics.md (plans/optimization_mt): see SysDiag.hpp. The per-frame part is a few stores and atomic adds;
// everything that costs (PDH: ~120 ms to open, ~1 ms per sample; the process list: ~2.5 ms; the power plan: ~0.7 ms)
// runs on the "Diag" thread, which wakes only when a window line of 50n is printed (once a minute in play).

namespace xray::perf
{
namespace
{
using steady = std::chrono::steady_clock;

double SteadyMs() noexcept
{
    return std::chrono::duration<double, std::milli>(steady::now().time_since_epoch()).count();
}

void SAppendF(char* buf, size_t size, const char* fmt, ...) noexcept
{
    const size_t len = std::strlen(buf);
    if (len + 1 >= size)
        return;
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf + len, size - len, fmt, args);
    va_end(args);
}

void AtomicMax(std::atomic<u64>& slot, u64 v) noexcept
{
    u64 cur = slot.load(std::memory_order_relaxed);
    while (v > cur && !slot.compare_exchange_weak(cur, v, std::memory_order_relaxed))
    {
    }
}

double TicksMs(u64 ticks) noexcept { return TicksToMilliseconds(ticks); }

// ---------------------------------------------------------------------------------------------------------------------
// File reads: four classes per window (reset by the [io] line), cumulative totals for the [load] marks
constexpr u32 kIoBuckets = 16;
constexpr double kIoEdgesMs[kIoBuckets] = { 0.1, 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 1e30 };

enum IoClassId : u32
{
    IoMainRead,  // a real read (small loose file, compressed archive entry) on main / the game logic: the frame waits
    IoOtherRead, // the same on any other thread (task workers, the model / texture loaders)
    IoMap,       // a mapped open (the data comes later through page faults)
    IoSlot,      // a background reader held the slot of a slow disk (BackgroundIo)
    IoClassCount
};

struct IoClass
{
    std::atomic<u64> count{0};
    std::atomic<u64> ticks{0};
    std::atomic<u64> bytes{0};
    std::atomic<u64> max_ticks{0};
    std::atomic<u32> hist[kIoBuckets]{};
};
IoClass s_io[IoClassCount];
std::atomic<u64> s_io_loose_reads{0};
std::atomic<u64> s_io_total_count{0};
std::atomic<u64> s_io_total_bytes{0};
std::atomic<u64> s_io_total_ticks{0};

u32 IoBucket(double ms) noexcept
{
    u32 b = 0;
    while (b + 1 < kIoBuckets && ms >= kIoEdgesMs[b])
        ++b;
    return b;
}

void IoAdd(IoClass& c, u64 ticks, u64 bytes) noexcept
{
    c.count.fetch_add(1, std::memory_order_relaxed);
    c.ticks.fetch_add(ticks, std::memory_order_relaxed);
    c.bytes.fetch_add(bytes, std::memory_order_relaxed);
    AtomicMax(c.max_ticks, ticks);
    c.hist[IoBucket(TicksMs(ticks))].fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------------------------------
// Threads: registered by name when they name themselves; the Diag thread samples their CPU time
constexpr u32 kMaxThreads = 128;
struct ThreadEntry
{
    char name[32]{};
    bool used{};
    std::thread::id id{}; // a thread named twice keeps one entry
#if defined(XR_PLATFORM_WINDOWS)
    HANDLE handle{};
#elif defined(XR_PLATFORM_LINUX)
    clockid_t clock{};
#endif
    u64 prev{}; // Windows: thread cycles; Linux: ns of the thread CPU clock
};
std::mutex s_threads_lock; // s_threads (registration on the thread itself, sampling on Diag)
ThreadEntry s_threads[kMaxThreads];

u64 ThreadCpuRaw(ThreadEntry& t, bool& alive) noexcept
{
    alive = true;
#if defined(XR_PLATFORM_WINDOWS)
    ULONG64 cycles = 0;
    if (!QueryThreadCycleTime(t.handle, &cycles))
    {
        alive = false;
        return t.prev;
    }
    alive = WaitForSingleObject(t.handle, 0) != WAIT_OBJECT_0;
    return static_cast<u64>(cycles);
#elif defined(XR_PLATFORM_LINUX)
    timespec ts{};
    if (clock_gettime(t.clock, &ts) != 0)
    {
        alive = false;
        return t.prev;
    }
    return static_cast<u64>(ts.tv_sec) * 1000000000ULL + static_cast<u64>(ts.tv_nsec);
#else
    alive = false;
    return t.prev;
#endif
}

double ThreadCpuRawToMs(u64 raw) noexcept
{
#if defined(XR_PLATFORM_WINDOWS)
    return TicksMs(static_cast<u64>(static_cast<double>(raw) / detail::ThreadCyclesScale()));
#else
    return static_cast<double>(raw) / 1e6;
#endif
}

void ThreadRelease(ThreadEntry& t) noexcept
{
#if defined(XR_PLATFORM_WINDOWS)
    if (t.handle)
        CloseHandle(t.handle);
    t.handle = nullptr;
#endif
    t.used = false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Task pool: per worker thread batches -> globals
std::atomic<u64> s_pool_ticks{0};
std::atomic<u64> s_pool_tasks{0};
std::atomic<u64> s_pool_steals{0};
std::atomic<u64> s_pool_flush_ticks{0}; // ~1 ms in Ticks()
thread_local u64 tl_pool_ticks = 0;
thread_local u32 tl_pool_tasks = 0;
thread_local u32 tl_pool_steals = 0;

// ---------------------------------------------------------------------------------------------------------------------
// The pace window (main thread: FramePacingContext in ProcessFrame, VramSubmit in Present, SysFrameTick in Flush)
constexpr u32 kPaceFine = 256;  // 0.25 ms buckets below 64 ms
constexpr u32 kPaceCoarse = 234; // 4 ms buckets below 1000 ms
constexpr u32 kPaceBuckets = kPaceFine + kPaceCoarse + 1;

u32 PaceBucket(double ms) noexcept
{
    if (ms < 0.0)
        return 0;
    if (ms < 64.0)
        return static_cast<u32>(ms * 4.0);
    if (ms < 1000.0)
        return kPaceFine + static_cast<u32>((ms - 64.0) / 4.0);
    return kPaceBuckets - 1;
}

double PaceBucketTop(u32 b) noexcept
{
    if (b < kPaceFine)
        return (b + 1) * 0.25;
    if (b < kPaceBuckets - 1)
        return 64.0 + (b - kPaceFine + 1) * 4.0;
    return 1000.0;
}

struct PaceWindow
{
    u32 frames{};
    u32 n{}; // frames with a known present interval
    double sum{}, sumsq{}, max{};
    u32 hist[kPaceBuckets]{};
    u32 sleep_frames{};
    double sleep_req{}, sleep_act{}, oversleep_max{};
    // VRAM samples (every 32nd frame, play and loading)
    u32 vram_n{}, vram_over{}, vram_high{};
    u64 vram_max{}, vram_budget_at_max{}, vram_budget_min{}, nonlocal_max{};
    u32 evict_start{}, evict_last{}; // the driver's eviction counter (GL NVX) at the start of the window / now
    bool evict_any{};
};
PaceWindow s_pace;
u32 s_evict_prev = 0; // the last eviction counter seen (the start of the next window)
bool s_evict_seen = false;
bool s_ctx_vsync = false;
u8 s_ctx_window = 0;
u32 s_ctx_monitor = 0;
u32 s_ctx_budget_ms = 0;
u32 s_ctx_sleep_ms = 0;

double PacePercentile(const PaceWindow& w, double q) noexcept
{
    if (!w.n)
        return 0.0;
    const u64 rank = static_cast<u64>(q * w.n + 0.999999);
    u64 acc = 0;
    for (u32 i = 0; i < kPaceBuckets; ++i)
    {
        acc += w.hist[i];
        if (acc >= rank)
            return std::min(PaceBucketTop(i), w.max);
    }
    return w.max;
}

// ---------------------------------------------------------------------------------------------------------------------
// The load timeline
std::mutex s_load_lock;
bool s_load_active = false;
bool s_load_end_seen = false;
std::atomic<bool> s_load_wait_frame{false};
char s_load_kind[32]{};
char s_load_stage[64]{}; // the last mark, read by the crash context without a lock (POD, best effort)
double s_load_t0 = 0.0, s_load_prev = 0.0;
u64 s_load_io_count = 0, s_load_io_bytes = 0, s_load_io_ticks = 0;
u64 s_load_cpu_us = 0, s_load_pf = 0, s_load_hf = 0;
u32 s_load_marks = 0;
constexpr double kLoadStaleMs = 60000.0;

u64 ProcessCpuUs() noexcept
{
#if defined(XR_PLATFORM_WINDOWS)
    FILETIME c{}, e{}, k{}, u{};
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
        return 0;
    const u64 kt = (static_cast<u64>(k.dwHighDateTime) << 32) | k.dwLowDateTime;
    const u64 ut = (static_cast<u64>(u.dwHighDateTime) << 32) | u.dwLowDateTime;
    return (kt + ut) / 10;
#else
    rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) != 0)
        return 0;
    return static_cast<u64>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000ULL +
        static_cast<u64>(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec);
#endif
}

// Under s_load_lock: the snapshot the next mark subtracts from
void LoadSnapshot(double now) noexcept
{
    s_load_prev = now;
    s_load_io_count = s_io_total_count.load(std::memory_order_relaxed);
    s_load_io_bytes = s_io_total_bytes.load(std::memory_order_relaxed);
    s_load_io_ticks = s_io_total_ticks.load(std::memory_order_relaxed);
    s_load_cpu_us = ProcessCpuUs();
    u64 ops, bytes;
    detail::ProcessIoSnapshot(ops, bytes, s_load_pf, s_load_hf);
}

// Under s_load_lock: the mark line into `line`
void LoadMarkLocked(const char* stage, char* line, size_t size) noexcept
{
    const double now = SteadyMs();
    const u64 count = s_io_total_count.load(std::memory_order_relaxed);
    const u64 bytes = s_io_total_bytes.load(std::memory_order_relaxed);
    const u64 ticks = s_io_total_ticks.load(std::memory_order_relaxed);
    const u64 cpu_us = ProcessCpuUs();
    u64 ops, rb, pf, hf;
    detail::ProcessIoSnapshot(ops, rb, pf, hf);
    std::snprintf(line, size, "* [load] %s %.1f ms (since begin %.1f ms) | reads %llu, %.1f MB, %.1f ms | cpu %.1f ms | "
        "pf %llu hf %llu", stage, now - s_load_prev, now - s_load_t0, static_cast<unsigned long long>(count - s_load_io_count),
        static_cast<double>(bytes - s_load_io_bytes) / (1024.0 * 1024.0), TicksMs(ticks - s_load_io_ticks),
        static_cast<double>(cpu_us - s_load_cpu_us) / 1000.0, static_cast<unsigned long long>(pf - s_load_pf),
        static_cast<unsigned long long>(hf >= s_load_hf ? hf - s_load_hf : 0));
    std::snprintf(s_load_stage, sizeof(s_load_stage), "%s", stage); // truncated (xr_strcpy: a fatal error on overflow)
    ++s_load_marks;
    LoadSnapshot(now);
}

// ---------------------------------------------------------------------------------------------------------------------
// The Diag thread
struct DiagShared
{
    std::mutex lock;
    std::condition_variable cv;
    bool request = false;
    char where[32]{};
};
DiagShared* s_diag = nullptr; // created once on main, never freed (the thread may outlive the statics at the exit)

#if defined(XR_PLATFORM_WINDOWS)
using PdhOpenQueryWProc = PDH_STATUS(WINAPI*)(LPCWSTR, DWORD_PTR, PDH_HQUERY*);
using PdhAddEnglishCounterWProc = PDH_STATUS(WINAPI*)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER*);
using PdhCollectQueryDataProc = PDH_STATUS(WINAPI*)(PDH_HQUERY);
using PdhGetFormattedCounterValueProc = PDH_STATUS(WINAPI*)(PDH_HCOUNTER, DWORD, LPDWORD, PPDH_FMT_COUNTERVALUE);
using PdhGetFormattedCounterArrayWProc = PDH_STATUS(WINAPI*)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PPDH_FMT_COUNTERVALUE_ITEM_W);
using NtQuerySystemInformationProc = LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);
using NtQueryTimerResolutionProc = LONG(WINAPI*)(PULONG, PULONG, PULONG);
using CallNtPowerInformationProc = LONG(WINAPI*)(int, PVOID, ULONG, PVOID, ULONG);
using PowerGetActiveSchemeProc = DWORD(WINAPI*)(HKEY, GUID**);
using PowerGetEffectiveOverlaySchemeProc = DWORD(WINAPI*)(GUID*);

constexpr DWORD kPdhFmtDouble = 0x00000200; // PDH_FMT_DOUBLE
constexpr DWORD kPdhFmtNoCap100 = 0x00008000; // PDH_FMT_NOCAP100
constexpr PDH_STATUS kPdhMoreData = static_cast<PDH_STATUS>(0x800007D2L); // PDH_MORE_DATA

// PROCESSOR_POWER_INFORMATION (powrprof, class ProcessorInformation = 11)
struct ProcessorPowerInfo
{
    ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState;
};
constexpr u32 kMaxPowerCpus = 256;

CallNtPowerInformationProc s_cnpi = nullptr;
std::once_flag s_cnpi_once;

CallNtPowerInformationProc PowerInfoProc() noexcept
{
    std::call_once(s_cnpi_once, [] {
        if (const HMODULE m = LoadLibraryA("powrprof.dll"))
            s_cnpi = reinterpret_cast<CallNtPowerInformationProc>(GetProcAddress(m, "CallNtPowerInformation"));
    });
    return s_cnpi;
}

// Average current and maximum MHz, the lowest limit of the logical CPUs of this group
bool PowerCpuMhz(u32& cur, u32& max, u32& limit) noexcept
{
    const CallNtPowerInformationProc proc = PowerInfoProc();
    if (!proc)
        return false;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const u32 n = std::min<u32>(si.dwNumberOfProcessors, kMaxPowerCpus);
    ProcessorPowerInfo info[kMaxPowerCpus];
    if (!n || proc(11, nullptr, 0, info, static_cast<ULONG>(n * sizeof(ProcessorPowerInfo))) != 0)
        return false;
    u64 sum = 0;
    max = 0;
    limit = u32(-1);
    for (u32 i = 0; i < n; ++i)
    {
        sum += info[i].CurrentMhz;
        max = std::max<u32>(max, info[i].MaxMhz);
        limit = std::min<u32>(limit, info[i].MhzLimit);
    }
    cur = static_cast<u32>(sum / n);
    return max != 0;
}
#endif

#if !defined(XR_PLATFORM_WINDOWS)
// A small text file (sysfs, procfs) into buf; false when it cannot be read
bool ReadSmallFile(const char* path, char* buf, size_t size) noexcept
{
    FILE* f = std::fopen(path, "rb");
    if (!f)
        return false;
    const size_t n = std::fread(buf, 1, size - 1, f);
    std::fclose(f);
    buf[n] = 0;
    return n > 0;
}

u64 ReadSmallNumber(const char* path) noexcept
{
    char buf[64];
    if (!ReadSmallFile(path, buf, sizeof(buf)))
        return 0;
    return std::strtoull(buf, nullptr, 10);
}
#endif

class DiagSampler
{
public:
    void Init() noexcept;
    void Sample(const char* where) noexcept;

private:
    double m_prev_ms = 0.0;
    bool m_have_prev = false;
    u32 m_cpus = 1;
    u64 m_proc_cpu_us = 0;
    u64 m_pool_ticks = 0, m_pool_tasks = 0, m_pool_steals = 0;
#if defined(XR_PLATFORM_WINDOWS)
    // PDH
    PdhCollectQueryDataProc m_pdh_collect = nullptr;
    PdhGetFormattedCounterValueProc m_pdh_value = nullptr;
    PdhGetFormattedCounterArrayWProc m_pdh_array = nullptr;
    PDH_HQUERY m_pdh_query = nullptr;
    enum
    {
        CPerf,
        CBaseMhz,
        CLimit,
        CDiskRead,
        CDiskIdle,
        CDiskBytes,
        CDiskQueue,
        CThermal,
        CCount
    };
    PDH_HCOUNTER m_counters[CCount]{};
    xr_vector<u8> m_pdh_array_buf;
    // system / process CPU
    u64 m_sys_idle = 0, m_sys_total = 0;
    // the process list
    NtQuerySystemInformationProc m_ntqsi = nullptr;
    NtQueryTimerResolutionProc m_ntqtr = nullptr;
    PowerGetActiveSchemeProc m_power_scheme = nullptr;
    PowerGetEffectiveOverlaySchemeProc m_power_overlay = nullptr;
    xr_vector<u8> m_spi;
    struct ProcPrev
    {
        u64 pid;
        s64 create;
        u64 cpu; // 100 ns
    };
    xr_vector<ProcPrev> m_procs, m_procs_new;
    u32 m_self_hard = 0;
    bool m_have_self_hard = false;

    double PdhValue(u32 c, bool nocap) noexcept;
    void ProcessList(char* line, size_t size, double window_ms) noexcept;
#else
    u64 m_stat_busy = 0, m_stat_total = 0;
#endif
    void Threads(char* line, size_t size, double window_ms) noexcept;
};

void DiagSampler::Init() noexcept
{
#if defined(XR_PLATFORM_WINDOWS)
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    m_cpus = std::max<u32>(1, si.dwNumberOfProcessors);
    if (const HMODULE pdh = LoadLibraryA("pdh.dll"))
    {
        const auto open = reinterpret_cast<PdhOpenQueryWProc>(GetProcAddress(pdh, "PdhOpenQueryW"));
        const auto add = reinterpret_cast<PdhAddEnglishCounterWProc>(GetProcAddress(pdh, "PdhAddEnglishCounterW"));
        m_pdh_collect = reinterpret_cast<PdhCollectQueryDataProc>(GetProcAddress(pdh, "PdhCollectQueryData"));
        m_pdh_value = reinterpret_cast<PdhGetFormattedCounterValueProc>(GetProcAddress(pdh, "PdhGetFormattedCounterValue"));
        m_pdh_array = reinterpret_cast<PdhGetFormattedCounterArrayWProc>(GetProcAddress(pdh, "PdhGetFormattedCounterArrayW"));
        if (open && add && m_pdh_collect && m_pdh_value && open(nullptr, 0, &m_pdh_query) == 0)
        {
            // English names: the same on every system language. A missing counter (old Windows, a VM) stays null.
            const wchar_t* paths[CCount] = { L"\\Processor Information(_Total)\\% Processor Performance",
                L"\\Processor Information(_Total)\\Processor Frequency",
                L"\\Processor Information(_Total)\\% Performance Limit", L"\\PhysicalDisk(_Total)\\Avg. Disk sec/Read",
                L"\\PhysicalDisk(_Total)\\% Idle Time", L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec",
                L"\\PhysicalDisk(_Total)\\Avg. Disk Read Queue Length", L"\\Thermal Zone Information(*)\\Temperature" };
            for (u32 i = 0; i < CCount; ++i)
            {
                if (add(m_pdh_query, paths[i], 0, &m_counters[i]) != 0)
                    m_counters[i] = nullptr;
            }
            m_pdh_collect(m_pdh_query);
        }
        else
            m_pdh_query = nullptr;
    }
    if (const HMODULE ntdll = GetModuleHandleA("ntdll.dll"))
    {
        m_ntqsi = reinterpret_cast<NtQuerySystemInformationProc>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
        m_ntqtr = reinterpret_cast<NtQueryTimerResolutionProc>(GetProcAddress(ntdll, "NtQueryTimerResolution"));
    }
    if (const HMODULE pp = LoadLibraryA("powrprof.dll"))
    {
        m_power_scheme = reinterpret_cast<PowerGetActiveSchemeProc>(GetProcAddress(pp, "PowerGetActiveScheme"));
        m_power_overlay =
            reinterpret_cast<PowerGetEffectiveOverlaySchemeProc>(GetProcAddress(pp, "PowerGetEffectiveOverlayScheme"));
    }
    m_spi.resize(1024 * 1024);
#else
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    m_cpus = n > 0 ? static_cast<u32>(n) : 1;
#endif
    // the baseline of the first window
    Sample(nullptr);
}

#if defined(XR_PLATFORM_WINDOWS)
double DiagSampler::PdhValue(u32 c, bool nocap) noexcept
{
    if (!m_counters[c])
        return -1.0;
    PDH_FMT_COUNTERVALUE v{};
    if (m_pdh_value(m_counters[c], kPdhFmtDouble | (nocap ? kPdhFmtNoCap100 : 0), nullptr, &v) != 0)
        return -1.0;
    return v.doubleValue;
}

// " | others A.exe 6.1% B.exe 1.2%" (the CPU of other processes, % of all logical CPUs) and " | proc hf N ws M MB"
void DiagSampler::ProcessList(char* line, size_t size, double window_ms) noexcept
{
    // the offsets below are of the 64-bit layout (x86: ImageName.Buffer at 60, UniqueProcessId at 68 - a bad pointer)
    if (!m_ntqsi || sizeof(void*) != 8)
        return;
    ULONG len = 0;
    LONG st = m_ntqsi(5, m_spi.data(), static_cast<ULONG>(m_spi.size()), &len); // SystemProcessInformation
    if (st == static_cast<LONG>(0xC0000004L)) // STATUS_INFO_LENGTH_MISMATCH
    {
        m_spi.resize(len + 256 * 1024);
        st = m_ntqsi(5, m_spi.data(), static_cast<ULONG>(m_spi.size()), &len);
    }
    if (st < 0)
        return;
    // SYSTEM_PROCESS_INFORMATION (x64 offsets, checked on Windows 11 against GetProcessTimes / GetProcessMemoryInfo, see
    // the journal): NextEntryOffset 0, HardFaultCount 16, CreateTime 32, UserTime 40, KernelTime 48, ImageName 56
    // (Length, MaximumLength, Buffer at 64), UniqueProcessId 80, WorkingSetSize 144
    constexpr u32 kTop = 3;
    struct Top
    {
        const u8* entry;
        u64 delta;
    } top[kTop]{};
    const u64 self = GetCurrentProcessId();
    u32 self_hard = 0;
    u64 self_ws = 0;
    bool self_found = false;
    m_procs_new.clear();
    const u8* p = m_spi.data();
    const u8* const end = m_spi.data() + std::min<size_t>(len, m_spi.size());
    for (;;)
    {
        if (p + 152 > end)
            break;
        u32 next;
        u64 pid;
        s64 create, user, kernel;
        std::memcpy(&next, p, sizeof(next));
        std::memcpy(&create, p + 32, sizeof(create));
        std::memcpy(&user, p + 40, sizeof(user));
        std::memcpy(&kernel, p + 48, sizeof(kernel));
        std::memcpy(&pid, p + 80, sizeof(pid));
        const u64 cpu = static_cast<u64>(user + kernel);
        m_procs_new.push_back({ pid, create, cpu });
        if (pid == self)
        {
            std::memcpy(&self_hard, p + 16, sizeof(self_hard));
            size_t ws;
            std::memcpy(&ws, p + 144, sizeof(ws));
            self_ws = ws;
            self_found = true;
        }
        else if (pid != 0 && m_have_prev)
        {
            const auto it = std::lower_bound(m_procs.begin(), m_procs.end(), pid,
                [](const ProcPrev& a, u64 b) { return a.pid < b; });
            const u64 prev = it != m_procs.end() && it->pid == pid && it->create == create ? it->cpu : 0;
            const u64 delta = cpu > prev ? cpu - prev : 0;
            u32 low = 0;
            for (u32 i = 1; i < kTop; ++i)
            {
                if (top[i].delta < top[low].delta)
                    low = i;
            }
            if (delta > top[low].delta)
                top[low] = { p, delta };
        }
        if (!next)
            break;
        p += next;
    }
    std::sort(m_procs_new.begin(), m_procs_new.end(), [](const ProcPrev& a, const ProcPrev& b) { return a.pid < b.pid; });
    m_procs.swap(m_procs_new);
    if (m_have_prev && window_ms > 0.0)
    {
        std::sort(top, top + kTop, [](const Top& a, const Top& b) { return a.delta > b.delta; });
        const double capacity_100ns = window_ms * 10000.0 * m_cpus;
        bool any = false;
        for (const Top& t : top)
        {
            const double share = 100.0 * static_cast<double>(t.delta) / capacity_100ns;
            if (!t.entry || share < 0.5)
                continue;
            u16 name_len;
            const wchar_t* name_ptr;
            std::memcpy(&name_len, t.entry + 56, sizeof(name_len));
            std::memcpy(&name_ptr, t.entry + 64, sizeof(name_ptr));
            char name[64] = "?";
            if (name_ptr && name_len)
            {
                const int n = WideCharToMultiByte(CP_UTF8, 0, name_ptr, name_len / 2, name, sizeof(name) - 1, nullptr, nullptr);
                name[n > 0 ? n : 0] = 0;
            }
            SAppendF(line, size, "%s %s %.1f%%", any ? "," : " | others", name, share);
            any = true;
        }
        if (!any)
            SAppendF(line, size, " | others < 0.5%%");
    }
    if (self_found)
    {
        if (m_have_self_hard)
            SAppendF(line, size, " | proc hf %u ws %llu MB", self_hard - m_self_hard,
                static_cast<unsigned long long>(self_ws / (1024 * 1024)));
        else
            SAppendF(line, size, " | proc ws %llu MB", static_cast<unsigned long long>(self_ws / (1024 * 1024)));
        m_self_hard = self_hard;
        m_have_self_hard = true;
    }
}
#endif

// "* [sys] <where>: threads ..." - the CPU of our named threads (% of one core) and the task pool
void DiagSampler::Threads(char* line, size_t size, double window_ms) noexcept
{
    struct Agg
    {
        const char* name;
        double ms;
        u32 count;
    };
    Agg agg[kMaxThreads];
    u32 n = 0;
    {
        std::lock_guard<std::mutex> lock(s_threads_lock);
        for (ThreadEntry& t : s_threads)
        {
            if (!t.used)
                continue;
            bool alive = true;
            const u64 raw = ThreadCpuRaw(t, alive);
            const double ms = raw > t.prev ? ThreadCpuRawToMs(raw - t.prev) : 0.0;
            t.prev = raw;
            u32 i = 0;
            while (i < n && std::strcmp(agg[i].name, t.name) != 0)
                ++i;
            if (i == n)
                agg[n++] = { t.name, 0.0, 0 };
            agg[i].ms += ms;
            ++agg[i].count;
            if (!alive)
            {
                // the thread ended: its last CPU is in this window, its slot is free again; the name stays valid for
                // the copy below (the slot is not reused before the lock is released)
                --agg[i].count;
                ThreadRelease(t);
            }
        }
        std::sort(agg, agg + n, [](const Agg& a, const Agg& b) { return a.ms > b.ms; });
        bool any = false;
        for (u32 i = 0; i < n && window_ms > 0.0; ++i)
        {
            const double pct = 100.0 * agg[i].ms / window_ms;
            if (pct < 0.5)
                break;
            if (agg[i].count > 1)
                SAppendF(line, size, "%s %s x%u %.0f", any ? "," : "", agg[i].name, agg[i].count, pct);
            else
                SAppendF(line, size, "%s %s %.0f", any ? "," : "", agg[i].name, pct);
            any = true;
        }
        if (!any)
            SAppendF(line, size, " -");
    }
    // the task pool: the time the pool workers ran tasks / (workers x window)
    const u64 ticks = s_pool_ticks.load(std::memory_order_relaxed);
    const u64 tasks = s_pool_tasks.load(std::memory_order_relaxed);
    const u64 steals = s_pool_steals.load(std::memory_order_relaxed);
    const size_t workers = TaskScheduler ? TaskScheduler->GetPoolWorkersCount() : 0;
    if (workers && window_ms > 0.0)
    {
        const u64 dt = tasks - m_pool_tasks;
        SAppendF(line, size, " | pool %zu workers busy %.0f%%, tasks %llu (%.0f/s), stolen %.0f%%", workers,
            100.0 * TicksMs(ticks - m_pool_ticks) / (static_cast<double>(workers) * window_ms),
            static_cast<unsigned long long>(dt), dt * 1000.0 / window_ms,
            dt ? 100.0 * static_cast<double>(steals - m_pool_steals) / static_cast<double>(dt) : 0.0);
    }
    m_pool_ticks = ticks;
    m_pool_tasks = tasks;
    m_pool_steals = steals;
}

void DiagSampler::Sample(const char* where) noexcept
{
    const double now = SteadyMs();
    const double window_ms = m_have_prev ? now - m_prev_ms : 0.0;
    const bool print = where && m_have_prev;
    string1024 line1{};
    string1024 line2{};
    if (print)
    {
        SAppendF(line1, sizeof(line1), "* [sys] %s: %.1f s |", where, window_ms / 1000.0);
        SAppendF(line2, sizeof(line2), "* [sys] %s: threads %% of a core:", where);
    }

    // the CPU frequency and its limit
#if defined(XR_PLATFORM_WINDOWS)
    if (m_pdh_query)
        m_pdh_collect(m_pdh_query);
    u32 cur = 0, max = 0, limit = 0;
    const bool have_pi = PowerCpuMhz(cur, max, limit);
    if (print)
    {
        const double perf = PdhValue(CPerf, true);
        const double base = PdhValue(CBaseMhz, false);
        const double perf_limit = PdhValue(CLimit, false);
        if (perf >= 0.0 && base > 0.0)
            SAppendF(line1, sizeof(line1), " cpu %.0f MHz (base %.0f, perf %.0f%%", perf * base / 100.0, base, perf);
        else
            SAppendF(line1, sizeof(line1), " cpu ? MHz (");
        if (perf_limit >= 0.0)
            SAppendF(line1, sizeof(line1), ", limit %.0f%%", perf_limit);
        if (have_pi)
            SAppendF(line1, sizeof(line1), ", now %u/%u lim %u", cur, max, limit);
        SAppendF(line1, sizeof(line1), ")");
    }
#else
    if (print)
    {
        u64 sum = 0;
        u32 n = 0;
        for (u32 i = 0; i < m_cpus && i < 256; ++i)
        {
            string128 path;
            xr_sprintf(path, "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_cur_freq", i);
            const u64 khz = ReadSmallNumber(path);
            if (khz)
            {
                sum += khz;
                ++n;
            }
        }
        const u64 max_khz = ReadSmallNumber("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
        if (n)
            SAppendF(line1, sizeof(line1), " cpu %.0f MHz (max %.0f)", sum / 1000.0 / n, max_khz / 1000.0);
        else
            SAppendF(line1, sizeof(line1), " cpu ? MHz");
    }
#endif

    // the system busy share vs our process
    const u64 proc_cpu_us = ProcessCpuUs();
#if defined(XR_PLATFORM_WINDOWS)
    FILETIME idle_ft{}, kernel_ft{}, user_ft{};
    if (GetSystemTimes(&idle_ft, &kernel_ft, &user_ft))
    {
        const u64 idle = (static_cast<u64>(idle_ft.dwHighDateTime) << 32) | idle_ft.dwLowDateTime;
        const u64 total = ((static_cast<u64>(kernel_ft.dwHighDateTime) << 32) | kernel_ft.dwLowDateTime) +
            ((static_cast<u64>(user_ft.dwHighDateTime) << 32) | user_ft.dwLowDateTime); // kernel includes idle
        if (print && total > m_sys_total)
        {
            const double dt = static_cast<double>(total - m_sys_total);
            SAppendF(line1, sizeof(line1), " | load sys %.0f%%", 100.0 * (1.0 - static_cast<double>(idle - m_sys_idle) / dt));
        }
        m_sys_idle = idle;
        m_sys_total = total;
    }
#elif defined(XR_PLATFORM_LINUX)
    {
        char stat[512];
        if (ReadSmallFile("/proc/stat", stat, sizeof(stat)))
        {
            unsigned long long v[8]{};
            if (std::sscanf(stat, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
                    &v[6], &v[7]) >= 4)
            {
                const u64 total = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6] + v[7];
                const u64 busy = total - v[3] - v[4];
                if (print && total > m_stat_total)
                    SAppendF(line1, sizeof(line1), " | load sys %.0f%%",
                        100.0 * static_cast<double>(busy - m_stat_busy) / static_cast<double>(total - m_stat_total));
                m_stat_busy = busy;
                m_stat_total = total;
            }
        }
    }
#endif
    if (print && window_ms > 0.0)
        SAppendF(line1, sizeof(line1), "%s proc %.0f%% of %u cpus", std::strstr(line1, "load sys") ? "" : " | load",
            100.0 * static_cast<double>(proc_cpu_us - m_proc_cpu_us) / (window_ms * 1000.0 * m_cpus), m_cpus);
    m_proc_cpu_us = proc_cpu_us;

#if defined(XR_PLATFORM_WINDOWS)
    // other processes, our hard faults and working set (the process list: ~2.5 ms, here only)
    if (print)
        ProcessList(line1, sizeof(line1), window_ms);
    else
    {
        string512 sink{};
        ProcessList(sink, sizeof(sink), window_ms);
    }
    if (print)
    {
        // the disks of the system (all physical disks together)
        const double rd = PdhValue(CDiskRead, false);
        if (rd >= 0.0)
        {
            SAppendF(line1, sizeof(line1), " | disks read %.1f ms, idle %.0f%%, %.1f MB/s, queue %.2f", rd * 1000.0,
                std::max(0.0, PdhValue(CDiskIdle, false)), std::max(0.0, PdhValue(CDiskBytes, false)) / (1024.0 * 1024.0),
                std::max(0.0, PdhValue(CDiskQueue, false)));
        }
        // the hottest ACPI thermal zone, when the machine has one (laptops mostly)
        if (m_counters[CThermal] && m_pdh_array)
        {
            DWORD bytes = 0, items = 0;
            if (m_pdh_array(m_counters[CThermal], kPdhFmtDouble, &bytes, &items, nullptr) == kPdhMoreData && bytes)
            {
                m_pdh_array_buf.resize(bytes);
                const auto it = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(m_pdh_array_buf.data());
                if (m_pdh_array(m_counters[CThermal], kPdhFmtDouble, &bytes, &items, it) == 0 && items)
                {
                    double kelvin = 0.0;
                    for (DWORD i = 0; i < items; ++i)
                        kelvin = std::max(kelvin, it[i].FmtValue.doubleValue);
                    if (kelvin > 0.0)
                        SAppendF(line1, sizeof(line1), " | thermal zone %.0f C", kelvin - 273.15);
                }
            }
        }
    }
#endif

    if (print)
    {
        // power: the source, the battery, the saver, the plan
        bool on_battery = false, saver = false;
        const bool power = hw::QueryPowerNow(on_battery, saver);
        SAppendF(line1, sizeof(line1), " | power %s", power ? (on_battery ? "battery" : "ac") : "?");
#if defined(XR_PLATFORM_WINDOWS)
        SYSTEM_POWER_STATUS ps{};
        if (GetSystemPowerStatus(&ps) && ps.BatteryLifePercent <= 100 && !(ps.BatteryFlag & 128))
            SAppendF(line1, sizeof(line1), " %u%%", ps.BatteryLifePercent);
#else
        u64 capacity = ReadSmallNumber("/sys/class/power_supply/BAT0/capacity");
        if (!capacity)
            capacity = ReadSmallNumber("/sys/class/power_supply/BAT1/capacity");
        if (capacity)
            SAppendF(line1, sizeof(line1), " %llu%%", static_cast<unsigned long long>(capacity));
#endif
        SAppendF(line1, sizeof(line1), "%s", saver ? " saver" : "");
#if defined(XR_PLATFORM_WINDOWS)
        if (m_power_scheme)
        {
            GUID* scheme = nullptr;
            if (m_power_scheme(nullptr, &scheme) == 0 && scheme)
            {
                const u32 id = scheme->Data1;
                const char* name = id == 0x381b4222 ? "balanced" : id == 0x8c5e7fda ? "high performance" :
                    id == 0xa1841308 ? "power saver" : id == 0xe9a42b02 ? "ultimate" : nullptr;
                if (name)
                    SAppendF(line1, sizeof(line1), ", plan %s", name);
                else
                    SAppendF(line1, sizeof(line1), ", plan %08x", id);
                LocalFree(scheme);
            }
        }
        if (m_power_overlay)
        {
            // the power mode slider of Windows 10/11 (0 = the plan as it is)
            GUID overlay{};
            if (m_power_overlay(&overlay) == 0 && overlay.Data1)
            {
                const u32 id = overlay.Data1;
                const char* name = id == 0x961cc777 ? "better battery" : id == 0x3af9b8d9 ? "better performance" :
                    id == 0xded574b5 ? "best performance" : "?";
                SAppendF(line1, sizeof(line1), ", mode %s (%08x)", name, id);
            }
        }
        // the timer resolution of the system now (timeBeginPeriod of any process; the fps limiter sleeps with it)
        ULONG tmin = 0, tmax = 0, tcur = 0;
        if (m_ntqtr && m_ntqtr(&tmin, &tmax, &tcur) >= 0)
            SAppendF(line1, sizeof(line1), " | timer %.2f ms", tcur / 10000.0);
        const DWORD prio = GetPriorityClass(GetCurrentProcess());
        const char* prio_name = prio == NORMAL_PRIORITY_CLASS ? "normal" : prio == ABOVE_NORMAL_PRIORITY_CLASS ?
            "above normal" : prio == HIGH_PRIORITY_CLASS ? "high" : prio == BELOW_NORMAL_PRIORITY_CLASS ? "below normal" :
            prio == IDLE_PRIORITY_CLASS ? "idle" : prio == REALTIME_PRIORITY_CLASS ? "realtime" : "?";
        SAppendF(line1, sizeof(line1), ", priority %s", prio_name);
#endif
    }

    if (print)
        Threads(line2, sizeof(line2), window_ms);
    else
    {
        // the baseline of the threads and the pool
        string256 sink{};
        Threads(sink, sizeof(sink), 0.0);
    }

    m_prev_ms = now;
    m_have_prev = true;
    if (print)
    {
        Msg("%s", line1);
        Msg("%s", line2);
        detail::CrashRingPush(line1);
        detail::CrashRingPush(line2);
    }
}

void DiagThread() noexcept
{
    DiagSampler sampler;
    sampler.Init();
    DiagShared& d = *s_diag;
    for (;;)
    {
        char where[32];
        {
            std::unique_lock<std::mutex> lock(d.lock);
            d.cv.wait(lock, [&d] { return d.request; });
            d.request = false;
            xr_strcpy(where, d.where);
        }
        sampler.Sample(where);
    }
}

void DiagStart() noexcept
{
    if (s_diag)
        return;
    s_diag = new DiagShared();
    Threading::SpawnThread("Diag", &DiagThread);
}

// "* [frame] <where>: pace ..." (main): the frame pacing and the VRAM of the window
void PacePrint(const char* where) noexcept
{
    PaceWindow& w = s_pace;
    string1024 line{};
    SAppendF(line, sizeof(line), "* [frame] %s: pace", where);
    if (w.n)
    {
        const double avg = w.sum / w.n;
        const double sd = std::sqrt(std::max(0.0, w.sumsq / w.n - avg * avg));
        SAppendF(line, sizeof(line), " present avg %.2f sd %.2f p95 %.2f p99 %.2f max %.1f ms (%u)", avg, sd,
            PacePercentile(w, 0.95), PacePercentile(w, 0.99), w.max, w.n);
    }
    else
        SAppendF(line, sizeof(line), " present ?");
    int refresh = 0;
    SDL_DisplayMode mode{};
    if (SDL_GetCurrentDisplayMode(static_cast<int>(s_ctx_monitor), &mode) == 0)
        refresh = mode.refresh_rate;
    constexpr const char* kWindowNames[] = { "windowed", "borderless", "fullscreen borderless", "fullscreen" };
    SAppendF(line, sizeof(line), ", vsync %s, %s, %d Hz, limit %u ms/frame", s_ctx_vsync ? "on" : "off",
        s_ctx_window < 4 ? kWindowNames[s_ctx_window] : "?", refresh, s_ctx_budget_ms);
    if (w.sleep_frames)
        SAppendF(line, sizeof(line), " (sleep in %.0f%% frames, asked %.2f got %.2f ms, oversleep max %.1f ms)",
            w.frames ? 100.0 * w.sleep_frames / w.frames : 0.0, w.sleep_req / w.sleep_frames,
            w.sleep_act / w.sleep_frames, w.oversleep_max);
    else
        SAppendF(line, sizeof(line), " (no sleep)");
    if (w.vram_n)
    {
        constexpr u64 mb = 1024 * 1024;
        SAppendF(line, sizeof(line), " | vram max %llu/%llu MB (%.0f%%), budget min %llu MB, >= 95%% %u, over %u of %u samples",
            static_cast<unsigned long long>(w.vram_max / mb), static_cast<unsigned long long>(w.vram_budget_at_max / mb),
            w.vram_budget_at_max ? 100.0 * static_cast<double>(w.vram_max) / static_cast<double>(w.vram_budget_at_max) : 0.0,
            static_cast<unsigned long long>(w.vram_budget_min / mb), w.vram_high, w.vram_over, w.vram_n);
        if (w.nonlocal_max)
            SAppendF(line, sizeof(line), ", nonlocal max %llu MB", static_cast<unsigned long long>(w.nonlocal_max / mb));
        if (w.evict_any)
            SAppendF(line, sizeof(line), ", evictions %u", w.evict_last - w.evict_start);
    }
    Msg("%s", line);
    detail::CrashRingPush(line);
    w = PaceWindow{};
}

// "* [io] <where>: ..." (main): the file reads of the window
void IoPrint(const char* where) noexcept
{
    struct Snap
    {
        u64 count, ticks, bytes, max;
        u32 hist[kIoBuckets];
    } s[IoClassCount];
    for (u32 c = 0; c < IoClassCount; ++c)
    {
        IoClass& io = s_io[c];
        s[c].count = io.count.exchange(0, std::memory_order_relaxed);
        s[c].ticks = io.ticks.exchange(0, std::memory_order_relaxed);
        s[c].bytes = io.bytes.exchange(0, std::memory_order_relaxed);
        s[c].max = io.max_ticks.exchange(0, std::memory_order_relaxed);
        for (u32 b = 0; b < kIoBuckets; ++b)
            s[c].hist[b] = io.hist[b].exchange(0, std::memory_order_relaxed);
    }
    const u64 loose = s_io_loose_reads.exchange(0, std::memory_order_relaxed);
    if (!where)
        return; // only the reset
    const auto p95 = [](const Snap& v) {
        const u64 rank = static_cast<u64>(0.95 * v.count + 0.999999);
        u64 acc = 0;
        for (u32 b = 0; b < kIoBuckets; ++b)
        {
            acc += v.hist[b];
            if (acc >= rank)
                return b + 1 < kIoBuckets ? kIoEdgesMs[b] : -1.0;
        }
        return -1.0;
    };
    string1024 line{};
    SAppendF(line, sizeof(line), "* [io] %s:", where);
    constexpr const char* kNames[IoClassCount] = { "reads main/game", "reads other", "maps", "bg slot" };
    for (u32 c = 0; c < IoClassCount; ++c)
    {
        const Snap& v = s[c];
        SAppendF(line, sizeof(line), "%s %s %llu", c ? " |" : "", kNames[c], static_cast<unsigned long long>(v.count));
        if (!v.count)
            continue;
        const double total_ms = TicksMs(v.ticks);
        const double q = p95(v);
        SAppendF(line, sizeof(line), " avg %.2f p95 %s%.2f max %.1f ms", total_ms / v.count, q < 0.0 ? ">" : "",
            q < 0.0 ? kIoEdgesMs[kIoBuckets - 2] : q, TicksMs(v.max));
        if (c == IoMainRead || c == IoOtherRead)
        {
            const double mb = static_cast<double>(v.bytes) / (1024.0 * 1024.0);
            SAppendF(line, sizeof(line), ", %.1f MB at %.1f MB/s", mb, total_ms > 0.0 ? mb * 1000.0 / total_ms : 0.0);
        }
        else if (c == IoSlot)
            SAppendF(line, sizeof(line), ", %.1f s held", total_ms / 1000.0);
    }
    SAppendF(line, sizeof(line), " | loose files %llu", static_cast<unsigned long long>(loose));
    Msg("%s", line);
    detail::CrashRingPush(line);
}
} // namespace

// ---------------------------------------------------------------------------------------------------------------------
XRCORE_API void IoOpenNote(u64 ticks, u64 bytes, bool archive, bool compressed, bool stream) noexcept
{
    // a real read: a compressed archive entry (decompressed from the mapped archive: its pages are read here) or a small
    // loose file (read whole); the rest is mapped and read later through page faults
    const bool read = !stream && (archive ? compressed : bytes < 16 * 1024);
    if (read)
    {
        const ThreadKind kind = CurrentThreadKind();
        IoAdd(s_io[kind == ThreadKind::Worker ? IoOtherRead : IoMainRead], ticks, bytes);
        if (!archive)
            s_io_loose_reads.fetch_add(1, std::memory_order_relaxed);
    }
    else
        IoAdd(s_io[IoMap], ticks, 0);
    s_io_total_count.fetch_add(1, std::memory_order_relaxed);
    s_io_total_bytes.fetch_add(bytes, std::memory_order_relaxed);
    s_io_total_ticks.fetch_add(ticks, std::memory_order_relaxed);
}

XRCORE_API void IoSlotHoldNote(u64 ticks) noexcept { IoAdd(s_io[IoSlot], ticks, 0); }

XRCORE_API void ThreadDiagRegister(const char* name) noexcept
{
    if (!name || !name[0])
        return;
    std::lock_guard<std::mutex> lock(s_threads_lock);
    const std::thread::id self = std::this_thread::get_id();
    for (ThreadEntry& t : s_threads)
    {
        if (t.used && t.id == self)
        {
#if defined(XR_PLATFORM_WINDOWS)
            if (WaitForSingleObject(t.handle, 0) == WAIT_OBJECT_0)
            {
                ThreadRelease(t); // an ended thread whose id the system gave to this one
                break;
            }
#endif
            std::snprintf(t.name, sizeof(t.name), "%s", name); // renamed (the outer thread becomes "Primary thread")
            return;
        }
    }
#if defined(XR_PLATFORM_WINDOWS)
    // all slots taken (many short start-up threads before the Diag thread sampled them): free the ended ones (their
    // last CPU time is lost)
    if (std::all_of(std::begin(s_threads), std::end(s_threads), [](const ThreadEntry& t) { return t.used; }))
    {
        for (ThreadEntry& t : s_threads)
        {
            if (t.used && WaitForSingleObject(t.handle, 0) == WAIT_OBJECT_0)
                ThreadRelease(t);
        }
    }
#endif
    for (ThreadEntry& t : s_threads)
    {
        if (t.used)
            continue;
#if defined(XR_PLATFORM_WINDOWS)
        HANDLE h = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE,
                DUPLICATE_SAME_ACCESS))
            return;
        t.handle = h;
        ULONG64 cycles = 0;
        QueryThreadCycleTime(h, &cycles);
        t.prev = static_cast<u64>(cycles);
#elif defined(XR_PLATFORM_LINUX)
        if (pthread_getcpuclockid(pthread_self(), &t.clock) != 0)
            return;
        timespec ts{};
        clock_gettime(t.clock, &ts);
        t.prev = static_cast<u64>(ts.tv_sec) * 1000000000ULL + static_cast<u64>(ts.tv_nsec);
#else
        return;
#endif
        std::snprintf(t.name, sizeof(t.name), "%s", name); // truncated: any name SetCurrentThreadName takes
        t.id = self;
        t.used = true;
        return;
    }
}

XRCORE_API void TaskPoolExecNote(u64 ticks, bool stolen) noexcept
{
    tl_pool_ticks += ticks;
    ++tl_pool_tasks;
    tl_pool_steals += stolen ? 1 : 0;
    u64 flush = s_pool_flush_ticks.load(std::memory_order_relaxed);
    if (!flush)
    {
        const double ms = TicksMs(1000000);
        flush = ms > 0.0 ? static_cast<u64>(1000000.0 / ms) : 1000000;
        s_pool_flush_ticks.store(flush, std::memory_order_relaxed);
    }
    // published every 32 tasks or 1 ms of work: a worker going to sleep leaves at most that much behind
    if (tl_pool_tasks < 32 && tl_pool_ticks < flush)
        return;
    s_pool_ticks.fetch_add(tl_pool_ticks, std::memory_order_relaxed);
    s_pool_tasks.fetch_add(tl_pool_tasks, std::memory_order_relaxed);
    s_pool_steals.fetch_add(tl_pool_steals, std::memory_order_relaxed);
    tl_pool_ticks = 0;
    tl_pool_tasks = 0;
    tl_pool_steals = 0;
}

XRCORE_API void VramSubmit(u64 local_usage, u64 local_budget, u64 nonlocal_usage, u32 evictions) noexcept
{
    PaceWindow& w = s_pace;
    ++w.vram_n;
    if (local_usage >= w.vram_max)
    {
        w.vram_max = local_usage;
        w.vram_budget_at_max = local_budget;
    }
    if (local_budget && (!w.vram_budget_min || local_budget < w.vram_budget_min))
        w.vram_budget_min = local_budget;
    if (local_budget && local_usage > local_budget)
        ++w.vram_over;
    if (local_budget && local_usage * 100 >= local_budget * 95)
        ++w.vram_high;
    w.nonlocal_max = std::max(w.nonlocal_max, nonlocal_usage);
    if (evictions)
    {
        if (!w.evict_any)
            w.evict_start = s_evict_seen ? s_evict_prev : evictions;
        w.evict_any = true;
        w.evict_last = evictions;
        s_evict_prev = evictions;
        s_evict_seen = true;
    }
}

XRCORE_API void FramePacingContext(bool vsync, u8 window_mode, u32 monitor, u32 frame_budget_ms, u32 sleep_ms) noexcept
{
    s_ctx_vsync = vsync;
    s_ctx_window = window_mode;
    s_ctx_monitor = monitor;
    s_ctx_budget_ms = frame_budget_ms;
    s_ctx_sleep_ms = sleep_ms;
}

XRCORE_API void LoadMarkBegin(pcstr kind) noexcept
{
    if (!kind)
        kind = "?";
    string256 closed{};
    string512 line{};
    {
        std::lock_guard<std::mutex> lock(s_load_lock);
        const double now = SteadyMs();
        if (s_load_active && now - s_load_prev >= kLoadStaleMs)
        {
            xr_sprintf(closed, "* [load] the timeline of '%s' closed without a load (last stage '%s', %u marks)",
                s_load_kind, s_load_stage, s_load_marks);
            s_load_active = false;
        }
        if (s_load_active)
        {
            string64 stage;
            std::snprintf(stage, sizeof(stage), "begin %s", kind);
            LoadMarkLocked(stage, line, sizeof(line));
        }
        else
        {
            s_load_active = true;
            s_load_end_seen = false;
            s_load_wait_frame.store(false, std::memory_order_relaxed);
            s_load_marks = 0;
            s_load_t0 = now;
            std::snprintf(s_load_kind, sizeof(s_load_kind), "%s", kind);
            std::snprintf(s_load_stage, sizeof(s_load_stage), "begin %s", kind);
            LoadSnapshot(now);
            xr_sprintf(line, "* [load] begin %s", kind);
        }
    }
    if (closed[0])
        Msg("%s", closed);
    Msg("%s", line);
}

XRCORE_API void LoadMark(pcstr stage) noexcept
{
    if (!stage)
        stage = "?";
    string512 line{};
    {
        std::lock_guard<std::mutex> lock(s_load_lock);
        if (!s_load_active)
            return;
        LoadMarkLocked(stage, line, sizeof(line));
    }
    Msg("%s", line);
}

namespace detail
{
void SysWindowReport(const char* where, bool print) noexcept
{
    if (!print)
    {
        s_pace = PaceWindow{};
        IoPrint(nullptr);
        return;
    }
    PacePrint(where);
    IoPrint(where);
    if (!s_diag)
        return;
    {
        std::lock_guard<std::mutex> lock(s_diag->lock);
        xr_strcpy(s_diag->where, where);
        s_diag->request = true;
    }
    s_diag->cv.notify_one();
}

void SysFrameTick(float wall_ms, float sleep_ms, float present_interval_ms) noexcept
{
    (void)wall_ms;
    DiagStart(); // the first game frame: the Diag thread takes its baseline
    PaceWindow& w = s_pace;
    ++w.frames;
    if (present_interval_ms >= 0.f)
    {
        ++w.n;
        w.sum += present_interval_ms;
        w.sumsq += static_cast<double>(present_interval_ms) * present_interval_ms;
        w.max = std::max<double>(w.max, present_interval_ms);
        ++w.hist[PaceBucket(present_interval_ms)];
    }
    if (s_ctx_sleep_ms)
    {
        ++w.sleep_frames;
        w.sleep_req += s_ctx_sleep_ms;
        w.sleep_act += sleep_ms;
        w.oversleep_max = std::max<double>(w.oversleep_max, sleep_ms - static_cast<double>(s_ctx_sleep_ms));
    }
    if (s_load_wait_frame.load(std::memory_order_relaxed))
        LoadFirstGameFrame();
}

void LoadSessionMark(u8 mark) noexcept
{
    if (mark & frame_session::Exit)
    {
        bool active;
        {
            std::lock_guard<std::mutex> lock(s_load_lock);
            active = s_load_active;
        }
        if (!active)
            LoadMarkBegin("exit");
    }
    if (mark & frame_session::LoadEnd)
    {
        LoadMark("load_end");
        std::lock_guard<std::mutex> lock(s_load_lock);
        if (s_load_active)
        {
            s_load_end_seen = true;
            s_load_wait_frame.store(true, std::memory_order_relaxed);
        }
    }
}

void LoadFirstGameFrame() noexcept
{
    string512 line{};
    string128 done{};
    {
        std::lock_guard<std::mutex> lock(s_load_lock);
        s_load_wait_frame.store(false, std::memory_order_relaxed);
        if (!s_load_active || !s_load_end_seen)
            return;
        LoadMarkLocked("first_game_frame", line, sizeof(line));
        xr_sprintf(done, "* [load] done: %s, %.1f s, %u marks", s_load_kind, (s_load_prev - s_load_t0) / 1000.0, s_load_marks);
        s_load_active = false;
        s_load_stage[0] = 0;
    }
    Msg("%s", line);
    Msg("%s", done);
}

void CpuFreqNow(char* buf, size_t size) noexcept
{
    buf[0] = 0;
#if defined(XR_PLATFORM_WINDOWS)
    u32 cur = 0, max = 0, limit = 0;
    if (!PowerCpuMhz(cur, max, limit))
        return;
    std::snprintf(buf, size, "cpu %u/%uMHz", cur, max);
    if (limit < max)
        SAppendF(buf, size, " lim %u", limit);
#elif defined(XR_PLATFORM_LINUX)
    const u64 cur = ReadSmallNumber("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
    const u64 max = ReadSmallNumber("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
    if (cur)
        std::snprintf(buf, size, "cpu0 %llu/%lluMHz", static_cast<unsigned long long>(cur / 1000),
            static_cast<unsigned long long>(max / 1000));
#else
    (void)size;
#endif
}

const char* LoadStageNow() noexcept { return s_load_stage; }
} // namespace detail
} // namespace xray::perf
