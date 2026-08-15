#include "stdafx.h"
#pragma hdrstop

#include "Debug/xrSentry.hpp"

#if defined(USE_SENTRY)
#if __has_include("xrSentry_embed_override.h")
#include "xrSentry_embed_override.h"
#else
#include "Debug/xrSentry_embed.h"
#endif
#include <sentry.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <vector>

#include "xrCore/Compression/miniz/miniz.h"
#include "LocatorAPI.h"

#if defined(XR_PLATFORM_WINDOWS)
#include <Windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include "xrCore/Text/Utf8Utils.hpp"
#endif

#if __has_include(".GitInfo.hpp")
#include ".GitInfo.hpp"
#endif

extern pcstr log_name(); // defined in log.cpp, intentionally not exposed in log.h

namespace
{
bool s_xr_sentry_started = false;
xrSentry_LuaStackFn s_lua_stack_provider{};

// Crashpad uploads these paths on crash; register likely logs at startup (after FS + CreateLog).
constexpr size_t kMaxLogAttachments = 16;

void sentry_add_logs_folder_attachments(sentry_options_t* options)
{
    if (!options)
        return;
    if (!FS.path_exist("$logs$"))
        return;

    string_path logs_dir{};
    if (!FS.update_path(logs_dir, "$logs$", "", false) || !logs_dir[0])
        return;

    std::error_code ec;
    const std::filesystem::path root(logs_dir);
    if (!std::filesystem::is_directory(root, ec))
        return;

    struct LogEntry
    {
        std::filesystem::path path;
        std::filesystem::file_time_type mtime{};
    };
    std::vector<LogEntry> logs;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec))
    {
        if (ec || !entry.is_regular_file(ec))
            continue;
        const auto& p = entry.path();
        if (!p.has_extension() || p.extension() != ".log")
            continue;
        LogEntry le{ p, {} };
        le.mtime = std::filesystem::last_write_time(p, ec);
        logs.push_back(std::move(le));
    }
    if (logs.empty())
        return;

    std::sort(logs.begin(), logs.end(),
        [](const LogEntry& a, const LogEntry& b) { return a.mtime > b.mtime; });

    const size_t n = std::min(logs.size(), kMaxLogAttachments);
    for (size_t i = 0; i < n; ++i)
    {
#if defined(XR_PLATFORM_WINDOWS)
        sentry_options_add_attachmentw(options, logs[i].path.c_str());
#else
        const std::string narrow = logs[i].path.string();
        sentry_options_add_attachment(options, narrow.c_str());
#endif
    }
#ifndef MASTER_GOLD
    Msg("[Sentry]: attached %zu log file(s) from [%s]", n, logs_dir);
#endif
}

sentry_value_t before_send_hook(sentry_value_t event, void* /*hint*/, void* /*closure*/)
{
    // Hook for future scrubbing (paths, user folders). Keep event as-is for now.
    return event;
}

#if defined(XR_PLATFORM_WINDOWS)
bool build_path_next_to_exe(const char* fileName, char* out, size_t outSize)
{
    if (!GetModuleFileNameA(nullptr, out, static_cast<DWORD>(outSize)))
        return false;
    char* slash = std::strrchr(out, '\\');
    if (!slash)
        return false;
    slash[1] = '\0';
    xr_strcat(out, outSize, fileName);
    return true;
}

// Writes a minidump of the current process to `utf8Path`.
// Pass exception pointers for real crashes so the dump carries the faulting context.
bool write_minidump_to_path(const char* utf8Path, EXCEPTION_POINTERS* exPtrs)
{
    const std::wstring wide = XRay::Utf8::ToWide(utf8Path);
    if (wide.empty())
        return false;

    const HANDLE hFile = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    HMODULE dbghelpMod = LoadLibraryW(L"dbghelp.dll");
    if (!dbghelpMod)
    {
        CloseHandle(hFile);
        return false;
    }

    using MiniDumpWriteDump_t = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
        PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    const auto pfn = reinterpret_cast<MiniDumpWriteDump_t>(GetProcAddress(dbghelpMod, "MiniDumpWriteDump"));
    if (!pfn)
    {
        FreeLibrary(dbghelpMod);
        CloseHandle(hFile);
        return false;
    }

    const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory |
        MiniDumpWithProcessThreadData | MiniDumpWithThreadInfo);

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    if (exPtrs)
    {
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = exPtrs;
        mei.ClientPointers = FALSE;
    }

    const BOOL ok = pfn(GetCurrentProcess(), GetCurrentProcessId(), hFile, dumpType,
        exPtrs ? &mei : nullptr, nullptr, nullptr);
    FreeLibrary(dbghelpMod);
    CloseHandle(hFile);
    return ok != FALSE;
}

// Live process snapshot for xrDebug::Fail (not a crash). sentry_capture_minidump() reads the file into the envelope synchronously.
bool write_live_minidump_to_temp(char* pathOut, size_t pathOutSize)
{
    if (!pathOut || pathOutSize < MAX_PATH)
        return false;

    wchar_t tempDir[MAX_PATH]{};
    if (!GetTempPathW(static_cast<DWORD>(sizeof(tempDir) / sizeof(wchar_t)), tempDir))
        return false;

    wchar_t tempFile[MAX_PATH]{};
    if (!GetTempFileNameW(tempDir, L"xrdf", 0, tempFile))
        return false;

    const xr_string utf8Path = XRay::Utf8::FromWide(tempFile);
    xr_strcpy(pathOut, pathOutSize, utf8Path.c_str());

    // GetTempFileNameW leaves an empty file behind; CREATE_ALWAYS truncates it.
    if (!write_minidump_to_path(utf8Path.c_str(), nullptr))
    {
        DeleteFileW(tempFile);
        pathOut[0] = '\0';
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Local crash reports: <app_data_root>/crashreports/crash_<date-time>.zip (minidump.dmp + game log)
// Saved from a top-level exception filter installed after sentry_init: our filter runs first,
// writes the local report, then delegates to the previous filter (Crashpad) so the Sentry flow
// is unchanged. Reports survive even when Sentry rejects uploads (quota exhausted).
constexpr size_t kCrashPathSize = MAX_PATH * 2;

char s_crash_reports_root[kCrashPathSize]{};
bool s_crash_reports_ready = false;
bool s_local_crash_filter_installed = false;
LPTOP_LEVEL_EXCEPTION_FILTER s_prev_uef = nullptr;
volatile LONG s_in_local_crash_report = 0;

void local_crash_reports_init()
{
    s_crash_reports_ready = false;
    if (!FS.path_exist("$app_data_root$"))
        return;
    string_path appDataRoot{};
    if (!FS.update_path(appDataRoot, "$app_data_root$", "", false) || !appDataRoot[0])
        return;

    xr_strcpy(s_crash_reports_root, sizeof(s_crash_reports_root), appDataRoot);
    const size_t len = std::strlen(s_crash_reports_root);
    if (len && s_crash_reports_root[len - 1] != '\\')
        xr_strcat(s_crash_reports_root, sizeof(s_crash_reports_root), "\\");
    xr_strcat(s_crash_reports_root, sizeof(s_crash_reports_root), "crashreports");

    // Create the root folder at startup; in the crash context we only create the zip file itself.
    const std::wstring rootWide = XRay::Utf8::ToWide(s_crash_reports_root);
    if (!CreateDirectoryW(rootWide.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return;

    xr_strcat(s_crash_reports_root, sizeof(s_crash_reports_root), "\\");
    s_crash_reports_ready = true;
}

// ---------------------------------------------------------------------------
// ZIP creation (miniz). All file I/O goes through WinAPI directly: UTF-8 paths
// must not be routed through CRT fopen, which mangles non-ASCII (Cyrillic) paths.

struct ZipReadCtx
{
    HANDLE hFile;
};

size_t zipfile_read_callback(void* pOpaque, mz_uint64 fileOfs, void* pBuf, size_t n)
{
    auto* ctx = static_cast<ZipReadCtx*>(pOpaque);
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(fileOfs);
    if (!SetFilePointerEx(ctx->hFile, pos, nullptr, FILE_BEGIN))
        return 0;
    DWORD read = 0;
    if (!ReadFile(ctx->hFile, pBuf, static_cast<DWORD>(n), &read, nullptr))
        return 0;
    return read;
}

bool zip_add_win_file(mz_zip_archive* zip, const char* utf8Path, const char* nameInZip)
{
    const HANDLE hFile = CreateFileW(XRay::Utf8::ToWide(utf8Path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size{};
    GetFileSizeEx(hFile, &size);

    ZipReadCtx ctx{ hFile };
    MZ_TIME_T fileTime = static_cast<MZ_TIME_T>(time(nullptr));
    const mz_bool ok = mz_zip_writer_add_read_buf_callback(zip, nameInZip, zipfile_read_callback, &ctx,
        static_cast<mz_uint64>(size.QuadPart), &fileTime, nullptr, 0, MZ_DEFAULT_LEVEL, nullptr, 0, nullptr, 0);
    CloseHandle(hFile);
    return ok != 0;
}

// Writes the zip with CREATE_NEW so a name clash never overwrites an existing report.
bool write_crash_zip_unique(const void* buf, size_t size, char* pathOut, size_t pathOutSize)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char base[64];
    xr_sprintf(base, sizeof(base), "%04u-%02u-%02u_%02u-%02u-%02u_%03u",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    for (unsigned attempt = 0; attempt < 64; ++attempt)
    {
        char name[80];
        if (attempt == 0)
            xr_strcpy(name, sizeof(name), base);
        else
            xr_sprintf(name, sizeof(name), "%s_%u", base, attempt);
        xr_sprintf(pathOut, pathOutSize, "%scrash_%s.zip", s_crash_reports_root, name);

        const HANDLE hFile = CreateFileW(XRay::Utf8::ToWide(pathOut).c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            if (GetLastError() == ERROR_FILE_EXISTS)
                continue;
            return false;
        }

        const BYTE* p = static_cast<const BYTE*>(buf);
        size_t left = size;
        while (left > 0)
        {
            constexpr size_t kChunk = 8 << 20;
            const DWORD chunk = static_cast<DWORD>(left < kChunk ? left : kChunk);
            DWORD written = 0;
            if (!WriteFile(hFile, p, chunk, &written, nullptr) || written != chunk)
            {
                CloseHandle(hFile);
                DeleteFileW(XRay::Utf8::ToWide(pathOut).c_str());
                return false;
            }
            p += written;
            left -= written;
        }
        CloseHandle(hFile);
        return true;
    }
    return false;
}

// Appends process memory statistics (goes into memory_stats.txt inside the crash zip).
// A size histogram of live heap blocks is the cheapest way to spot a leak: a leaked allocation
// type shows up as a huge count of same-sized blocks.
// IMPORTANT: while a heap is locked for HeapWalk there must be zero allocations on it
// (a single xr_string append would deadlock on the CRT heap), so per-heap text is built
// only after HeapUnlock.
void append_memory_stats(xr_string& out)
{
    char buf[640];

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
    {
        xr_sprintf(buf, sizeof(buf),
            "physical: total=%llu MB avail=%llu MB\r\n"
            "virtual: total=%llu MB avail=%llu MB\r\n",
            ms.ullTotalPhys >> 20, ms.ullAvailPhys >> 20,
            ms.ullTotalVirtual >> 20, ms.ullAvailVirtual >> 20);
        out += buf;
    }

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    u64 workingSet = 0, privateBytes = 0;
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
    {
        // GetTickCount64() is system uptime, not process uptime: use GetProcessTimes instead.
        u64 uptimeSec = 0;
        FILETIME creation{}, exitTime{}, kernelTime{}, userTime{};
        if (GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernelTime, &userTime))
        {
            FILETIME now{};
            GetSystemTimeAsFileTime(&now);
            const u64 now64 = (static_cast<u64>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
            const u64 created64 = (static_cast<u64>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
            if (now64 > created64)
                uptimeSec = (now64 - created64) / 10000000ULL; // FILETIME is in 100ns units
        }

        workingSet = pmc.WorkingSetSize;
        privateBytes = pmc.PrivateUsage;
        xr_sprintf(buf, sizeof(buf),
            "process: working_set=%llu MB peak_working_set=%llu MB private=%llu MB pagefile=%llu MB\r\n"
            "uptime=%llu sec\r\n",
            static_cast<u64>(pmc.WorkingSetSize) >> 20, static_cast<u64>(pmc.PeakWorkingSetSize) >> 20,
            static_cast<u64>(pmc.PrivateUsage) >> 20, static_cast<u64>(pmc.PagefileUsage) >> 20,
            uptimeSec);
        out += buf;
    }

    // Heap walk: count busy blocks, total bytes and a size histogram per heap.
    static constexpr DWORD kBucketBounds[] = { 64, 256, 1024, 4096, 16384, 65536, 262144, 1048576 };
    constexpr size_t kBucketCount = sizeof(kBucketBounds) / sizeof(kBucketBounds[0]); // + overflow bucket
    constexpr size_t kTopBlocks = 8;
    struct TopBlock
    {
        size_t size;
        const void* addr;
    };

    HANDLE heaps[64]{};
    const DWORD heapCount = std::min<DWORD>(GetProcessHeaps(64, heaps), 64);
    xr_sprintf(buf, sizeof(buf), "heaps: %u\r\n", heapCount);
    out += buf;

    u64 totalBusyBlocks = 0, totalBusyBytes = 0;
    for (DWORD hi = 0; hi < heapCount; ++hi)
    {
        u64 busyBlocks = 0, busyBytes = 0, freeBlocks = 0, freeBytes = 0;
        u64 buckets[kBucketCount + 1]{};
        TopBlock top[kTopBlocks]{};

        // No allocations from here until HeapUnlock (see note above).
        if (HeapLock(heaps[hi]))
        {
            PROCESS_HEAP_ENTRY entry{};
            while (HeapWalk(heaps[hi], &entry))
            {
                if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY)
                {
                    ++busyBlocks;
                    busyBytes += entry.cbData;
                    size_t b = 0;
                    while (b < kBucketCount && entry.cbData > kBucketBounds[b])
                        ++b;
                    ++buckets[b];
                    if (entry.cbData > top[kTopBlocks - 1].size)
                    {
                        size_t pos = kTopBlocks - 1;
                        while (pos > 0 && top[pos - 1].size < entry.cbData)
                        {
                            top[pos] = top[pos - 1];
                            --pos;
                        }
                        top[pos].size = entry.cbData;
                        top[pos].addr = entry.lpData;
                    }
                }
                else if (!(entry.wFlags & (PROCESS_HEAP_REGION | PROCESS_HEAP_UNCOMMITTED_RANGE)))
                {
                    ++freeBlocks;
                    freeBytes += entry.cbData;
                }
            }
            HeapUnlock(heaps[hi]);
        }

        totalBusyBlocks += busyBlocks;
        totalBusyBytes += busyBytes;

        xr_sprintf(buf, sizeof(buf),
            "heap[%u]: busy_blocks=%llu busy_bytes=%llu MB free_blocks=%llu free_bytes=%llu MB\r\n"
            "  busy by size: <=64B:%llu <=256B:%llu <=1KB:%llu <=4KB:%llu <=16KB:%llu <=64KB:%llu <=256KB:%llu <=1MB:%llu >1MB:%llu\r\n",
            hi, busyBlocks, busyBytes >> 20, freeBlocks, freeBytes >> 20,
            buckets[0], buckets[1], buckets[2], buckets[3], buckets[4],
            buckets[5], buckets[6], buckets[7], buckets[8]);
        out += buf;

        for (size_t t = 0; t < kTopBlocks && top[t].size != 0; ++t)
        {
            xr_sprintf(buf, sizeof(buf), "  largest: %zu bytes @ %p\r\n", top[t].size, top[t].addr);
            out += buf;
        }
    }

    Msg("! [Sentry]: memory at crash: WS=%llu MB, private=%llu MB, heap busy=%llu MB in %llu blocks",
        workingSet >> 20, privateBytes >> 20, totalBusyBytes >> 20, totalBusyBlocks);
}

void save_local_crash_report_impl(EXCEPTION_POINTERS* exPtrs)
{
    // dbghelp can only dump into a real file: write to %TEMP% first, zip afterwards.
    char dumpPath[MAX_PATH]{};
    bool haveDump = false;
    wchar_t tempDir[MAX_PATH]{};
    wchar_t tempFile[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tempDir) && GetTempFileNameW(tempDir, L"xrdf", 0, tempFile))
    {
        const xr_string utf8Path = XRay::Utf8::FromWide(tempFile);
        xr_strcpy(dumpPath, sizeof(dumpPath), utf8Path.c_str());
        haveDump = write_minidump_to_path(dumpPath, exPtrs);
    }

    const pcstr logPath = log_name();
    FlushLog();

    if (!haveDump && !(logPath && *logPath))
    {
        if (dumpPath[0])
            DeleteFileW(XRay::Utf8::ToWide(dumpPath).c_str());
        return;
    }

    mz_zip_archive zip{};
    void* zipBuf = nullptr;
    size_t zipSize = 0;
    char zipPath[kCrashPathSize]{};
    bool ok = mz_zip_writer_init_heap(&zip, 0, 0) != 0;
    if (ok)
    {
        if (haveDump)
            ok = zip_add_win_file(&zip, dumpPath, "minidump.dmp");
        if (ok && logPath && *logPath)
        {
            pcstr fileName = std::strrchr(logPath, '\\');
            const pcstr fileNameFwd = std::strrchr(logPath, '/');
            if (fileNameFwd && (!fileName || fileNameFwd > fileName))
                fileName = fileNameFwd;
            fileName = fileName ? fileName + 1 : logPath;
            ok = zip_add_win_file(&zip, logPath, fileName);
        }
        if (ok)
        {
            xr_string memStats;
            append_memory_stats(memStats);
            if (!memStats.empty())
                ok = mz_zip_writer_add_mem(&zip, "memory_stats.txt", memStats.data(), memStats.size(), MZ_DEFAULT_LEVEL) != 0;
        }
        ok = ok && mz_zip_writer_finalize_heap_archive(&zip, &zipBuf, &zipSize) != 0 && zipBuf != nullptr;
    }
    mz_zip_writer_end(&zip);

    if (ok)
        ok = write_crash_zip_unique(zipBuf, zipSize, zipPath, sizeof(zipPath));
    mz_free(zipBuf);

    if (dumpPath[0])
        DeleteFileW(XRay::Utf8::ToWide(dumpPath).c_str());

    if (ok && zipPath[0])
        Msg("! [Sentry]: local crash report saved to [%s] (%zu bytes)", zipPath, zipSize);
    else
        Msg("! [Sentry]: failed to save local crash report");
    FlushLog();
}

// Separate wrapper: __try cannot be used in a function with objects requiring unwinding.
void save_local_crash_report(EXCEPTION_POINTERS* exPtrs)
{
    if (InterlockedCompareExchange(&s_in_local_crash_report, 1, 0) != 0)
        return;
    __try
    {
        save_local_crash_report_impl(exPtrs);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    InterlockedExchange(&s_in_local_crash_report, 0);
}

LONG WINAPI local_crash_report_filter(EXCEPTION_POINTERS* exPtrs)
{
    if (s_crash_reports_ready)
        save_local_crash_report(exPtrs);
    // Chain into the previously installed filter (Crashpad / engine) to keep the existing flow.
    if (s_prev_uef)
        return s_prev_uef(exPtrs);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif
} // namespace

static void install_local_crash_reporter()
{
#if defined(XR_PLATFORM_WINDOWS)
    if (s_local_crash_filter_installed)
        return;
    local_crash_reports_init();
    // Must be installed after sentry_init: the filter installed last runs first,
    // so we save the local report and then delegate to Crashpad's filter.
    s_prev_uef = SetUnhandledExceptionFilter(local_crash_report_filter);
    s_local_crash_filter_installed = true;
#ifndef MASTER_GOLD
    if (s_crash_reports_ready)
        Msg("[Sentry]: local crash reports enabled -> [%s]", s_crash_reports_root);
    else
        Msg("! [Sentry]: local crash reports disabled ($app_data_root$ was not resolved)");
#endif
#endif
}

static void uninstall_local_crash_reporter()
{
#if defined(XR_PLATFORM_WINDOWS)
    if (!s_local_crash_filter_installed)
        return;
    SetUnhandledExceptionFilter(s_prev_uef);
    s_prev_uef = nullptr;
    s_local_crash_filter_installed = false;
#endif
}

void xrSentry_Initialize(pcstr commandLine)
{
    if (s_xr_sentry_started)
        return;

    sentry_options_t* options = sentry_options_new();

    const char* dsn = std::getenv("SENTRY_DSN");
    if (dsn && *dsn)
        sentry_options_set_dsn(options, dsn);
    else if (XRAY_SENTRY_DEFAULT_DSN_STR[0] != '\0')
        sentry_options_set_dsn(options, XRAY_SENTRY_DEFAULT_DSN_STR);
    else
        sentry_options_set_dsn(options, "");

#if defined(XR_PLATFORM_WINDOWS)
    char dbPath[MAX_PATH * 2]{};
    {
        char* localAppData = nullptr;
        size_t len = 0;
        if (_dupenv_s(&localAppData, &len, "LOCALAPPDATA") == 0 && localAppData && localAppData[0])
        {
            std::error_code ec;
            std::filesystem::create_directories(
                std::filesystem::path(localAppData) / "OpenXRay" / "sentry-native", ec);
            xr_sprintf(dbPath, sizeof(dbPath), "%s\\OpenXRay\\sentry-native", localAppData);
            free(localAppData);
        }
        else
        {
            std::error_code ec;
            std::filesystem::create_directories(".\\OpenXRay\\sentry-native", ec);
            xr_strcpy(dbPath, sizeof(dbPath), ".\\OpenXRay\\sentry-native");
        }
    }
    sentry_options_set_database_path(options, dbPath);

    char handlerPath[MAX_PATH]{};
    if (build_path_next_to_exe("crashpad_handler.exe", handlerPath, sizeof(handlerPath)))
        sentry_options_set_handler_path(options, handlerPath);
#else
    const char* home = std::getenv("HOME");
    if (home && home[0])
    {
        const std::filesystem::path db = std::filesystem::path(home) / ".cache" / "OpenXRay" / "sentry-native";
        std::error_code ec;
        std::filesystem::create_directories(db, ec);
        sentry_options_set_database_path(options, db.string().c_str());
    }
    else
    {
        std::error_code ec;
        std::filesystem::create_directories(".sentry-native", ec);
        sentry_options_set_database_path(options, ".sentry-native");
    }
#endif

    if (const char* env = std::getenv("SENTRY_ENVIRONMENT"))
        sentry_options_set_environment(options, env);
    else
    {
#if defined(_DEBUG)
        sentry_options_set_environment(options, "development");
#else
        sentry_options_set_environment(options, "production");
#endif
    }

    if (const char* rel = std::getenv("SENTRY_RELEASE"))
        sentry_options_set_release(options, rel);
    else
    {
#if defined(GIT_INFO_CURRENT_COMMIT)
        sentry_options_set_release(options, "OpenXRay@" GIT_INFO_CURRENT_COMMIT);
#else
        sentry_options_set_release(options, "OpenXRay@unknown");
#endif
    }

    sentry_options_set_before_send(options, before_send_hook, nullptr);

    sentry_add_logs_folder_attachments(options);

    // sentry_init() takes ownership of `options` on success (freed in sentry_close).
    // On failure it frees them internally. The caller must not sentry_options_free()
    // after sentry_init — that would leave g_options dangling and crash on shutdown.
    if (sentry_init(options) != 0)
    {
        Msg("! [Sentry]: sentry_init failed");
        // Local crash reports must not depend on Sentry availability.
        install_local_crash_reporter();
        return;
    }
    s_xr_sentry_started = true;

    install_local_crash_reporter();

    if (commandLine && std::strstr(commandLine, "-sentry_test_av_crash"))
    {
        volatile int* p = nullptr;
        *p = 42;
    }
}

void xrSentry_Shutdown()
{
    uninstall_local_crash_reporter();
    if (!s_xr_sentry_started)
        return;
    sentry_close();
    s_xr_sentry_started = false;
}

void XRCORE_API xrSentry_SetLuaStackProvider(xrSentry_LuaStackFn fn) { s_lua_stack_provider = fn; }

static void sentry_event_attach_lua_stack(sentry_value_t event, pcstr lua_stack)
{
    constexpr size_t kMaxLua = 12000;
    xr_string from_provider;
    if ((!lua_stack || !*lua_stack) && s_lua_stack_provider)
    {
        s_lua_stack_provider(from_provider);
        if (!from_provider.empty())
            lua_stack = from_provider.c_str();
    }
    if (!lua_stack || !*lua_stack)
        return;

    size_t len = std::strlen(lua_stack);
    if (len > kMaxLua)
        len = kMaxLua;

    sentry_value_t extra = sentry_value_new_object();
    sentry_value_set_by_key(extra, "lua_stack", sentry_value_new_string_n(lua_stack, len));
    sentry_value_set_by_key(event, "extra", extra);
}

void XRCORE_API xrSentry_CaptureSoftError(pcstr logger, pcstr message, pcstr lua_stack)
{
    if (!s_xr_sentry_started || !message || !*message)
        return;

    sentry_value_t event = sentry_value_new_message_event(SENTRY_LEVEL_WARNING, logger ? logger : "OpenXRay", message);

    // Tag for issue search / alert rules in Sentry: soft_error:true
    sentry_value_t tags = sentry_value_new_object();
    sentry_value_set_by_key(tags, "soft_error", sentry_value_new_string("true"));
    sentry_value_set_by_key(event, "tags", tags);

    sentry_event_attach_lua_stack(event, lua_stack);

    sentry_event_value_add_stacktrace(event, nullptr, 0);
    sentry_capture_event(event);
}

void XRCORE_API xrSentry_CaptureError(pcstr logger, pcstr message, pcstr lua_stack)
{
    if (!s_xr_sentry_started || !message || !*message)
        return;

    sentry_value_t event = sentry_value_new_message_event(SENTRY_LEVEL_ERROR, logger ? logger : "OpenXRay", message);

    sentry_value_t tags = sentry_value_new_object();
    sentry_value_set_by_key(tags, "engine_error", sentry_value_new_string("true"));
    sentry_value_set_by_key(event, "tags", tags);

    sentry_event_attach_lua_stack(event, lua_stack);

    sentry_event_value_add_stacktrace(event, nullptr, 0);
    sentry_capture_event(event);
}

void XRCORE_API xrSentry_CaptureDebugFail(pcstr expr, pcstr desc, pcstr arg1, pcstr arg2)
{
    if (!s_xr_sentry_started)
        return;

    xr_string m;
    if (expr && *expr)
        m = expr;
    if (desc && *desc)
    {
        if (!m.empty())
            m += " | ";
        m += desc;
    }
    if (arg1 && *arg1)
    {
        if (!m.empty())
            m += " | ";
        m += arg1;
    }
    if (arg2 && *arg2)
    {
        if (!m.empty())
            m += " | ";
        m += arg2;
    }
    if (m.empty())
        m = "assertion failed";
    constexpr size_t cap = 1800;
    if (m.size() > cap)
        m.resize(cap);

#if defined(XR_PLATFORM_WINDOWS)
    sentry_set_tag("xrDebug.Fail", "true");
    sentry_set_tag("logger", "xrCore.xrDebug.Fail");
    sentry_set_extra("assertion", sentry_value_new_string(m.c_str()));

    xr_string lua_for_extra;
    if (s_lua_stack_provider)
    {
        s_lua_stack_provider(lua_for_extra);
        constexpr size_t kMaxLua = 12000;
        if (lua_for_extra.size() > kMaxLua)
            lua_for_extra.resize(kMaxLua);
        if (!lua_for_extra.empty())
            sentry_set_extra("lua_stack", sentry_value_new_string(lua_for_extra.c_str()));
    }

    char dumpPath[MAX_PATH]{};
    const bool dumped = write_live_minidump_to_temp(dumpPath, sizeof(dumpPath));
    if (dumped)
    {
        sentry_capture_minidump(dumpPath);
        // WinHTTP transport queues uploads; xrDebug::Fail soon hits DEBUG_BREAK — flush so the envelope
        // (minidump + log attachments) is sent before the process may terminate or hang in the dialog.
        constexpr uint64_t kSentryFlushMs = 10000;
        if (sentry_flush(kSentryFlushMs) != 0)
        {
#ifndef MASTER_GOLD
            Msg("! [Sentry]: sentry_flush timed out after xrDebug::Fail minidump (report may be incomplete)");
#endif
        }
        DeleteFileW(XRay::Utf8::ToWide(dumpPath).c_str());
    }

    if (!lua_for_extra.empty())
        sentry_remove_extra("lua_stack");
    sentry_remove_extra("assertion");
    sentry_remove_tag("logger");
    sentry_remove_tag("xrDebug.Fail");

    if (!dumped)
#endif
    {
        xrSentry_CaptureError("xrCore.xrDebug.Fail", m.c_str(), nullptr);
    }
}

void XRCORE_API xrSentry_CaptureIniRStringError(pcstr ini_path, pcstr section, pcstr key)
{
    if (!s_xr_sentry_started)
        return;

    char message[768];
    xr_sprintf(message, sizeof(message), "Can't find variable %s in [%s]", key ? key : "?", section ? section : "?");

    sentry_value_t event = sentry_value_new_message_event(SENTRY_LEVEL_ERROR, "xrCore.Inifile", message);

    sentry_value_t tags = sentry_value_new_object();
    sentry_value_set_by_key(tags, "inifile_error", sentry_value_new_string("r_string"));
    sentry_value_set_by_key(event, "tags", tags);

    sentry_value_t extra = sentry_value_new_object();
    if (ini_path && *ini_path)
        sentry_value_set_by_key(extra, "ini_file", sentry_value_new_string(ini_path));
    if (section && *section)
        sentry_value_set_by_key(extra, "section", sentry_value_new_string(section));
    if (key && *key)
        sentry_value_set_by_key(extra, "key", sentry_value_new_string(key));
    sentry_value_set_by_key(event, "extra", extra);

    sentry_event_value_add_stacktrace(event, nullptr, 0);
    sentry_capture_event(event);
}

#else // !USE_SENTRY

void xrSentry_Initialize(pcstr /*commandLine*/) {}
void xrSentry_Shutdown() {}
void XRCORE_API xrSentry_SetLuaStackProvider(xrSentry_LuaStackFn /*fn*/) {}
void XRCORE_API xrSentry_CaptureSoftError(pcstr /*logger*/, pcstr /*message*/, pcstr /*lua_stack*/) {}
void XRCORE_API xrSentry_CaptureError(pcstr /*logger*/, pcstr /*message*/, pcstr /*lua_stack*/) {}
void XRCORE_API xrSentry_CaptureDebugFail(pcstr /*expr*/, pcstr /*desc*/, pcstr /*arg1*/, pcstr /*arg2*/) {}
void XRCORE_API xrSentry_CaptureIniRStringError(pcstr /*ini_path*/, pcstr /*section*/, pcstr /*key*/) {}

#endif // USE_SENTRY
