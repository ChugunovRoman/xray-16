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
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include "xrCore/Compression/miniz/miniz.h"
#include "LocatorAPI.h"
#include "xrCore/Platform/HwProfile.h"
#include "xrCore/Profiling/SysDiag.hpp"
#include "xrCore/Threading/ThreadUtil.h"

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
std::atomic<bool> s_xr_sentry_started{ false };

// Event gate + async send queue (wave 3 review fix, plans/lua_to_cpp/61-wave3-reports/run7-freezes-p5.md §6.2).
// Before this, every Lua error sent a Sentry event with the whole game log attached, synchronously in the game
// logic thread (22-70 ms and 75-80 MB per error; ~1950 repeats of one message froze the run). Now: one event per
// unique message per session (normalized - ids/addresses stripped), a session cap and a rate limit; the event is
// built on the erroring thread (the native stack must be that thread's and the Lua stack provider walks a live
// Lua state) and sent by a worker thread. Suppressed events produce at most one summary log line per minute.
constexpr unsigned kGateMaxEventsPerSession = 20;
// SEN-m4: CInifile::r_string errors right before xrDebug::Fatal, and the xrDebug::Fail fallback that could not take
// a live minidump, used to share kGateMaxEventsPerSession with ordinary Lua/engine errors - after 20 of those (not
// unusual with one broken script) every later critical event was silently dropped for the rest of the session.
// A budget of its own; deduplication (s_gate_keys) and the rate window below still apply to both kinds.
constexpr unsigned kGateMaxCriticalEventsPerSession = 20;
constexpr unsigned kGateMaxEventsPerWindow = 5; // per kGateRateWindowMs, shared by both budgets
constexpr int64_t kGateRateWindowMs = 60 * 1000;
constexpr size_t kGateMaxKeys = 256;
constexpr size_t kGateNormalizedMax = 512;
constexpr size_t kQueueMaxPending = 64;
constexpr int64_t kSuppressSummaryIntervalMs = 60 * 1000;
// The crash-only log attachment is a tail of the game log, never the whole file (75-81 MB, 5 sessions).
constexpr size_t kLogTailMaxBytes = 1024 * 1024;
constexpr size_t kLogTailPathSize = 520;

std::mutex s_gate_mutex; // gate counters + pending queue (low contention: repeats are rejected before any work)
std::condition_variable s_gate_cv;
std::vector<u64> s_gate_keys;
unsigned s_gate_sent_session = 0;
unsigned s_gate_sent_critical_session = 0; // SEN-m4: ini/assert-fallback events, counted apart from s_gate_sent_session
unsigned s_gate_sent_window = 0;
int64_t s_gate_window_start = 0;
unsigned s_gate_suppressed = 0;
unsigned s_gate_suppressed_logged = 0;
int64_t s_gate_summary_last = 0;
bool s_gate_summary_ever = false;
std::vector<sentry_value_t> s_queue;
bool s_queue_stop = false;
// SEN-1: a std::thread object with static/global storage duration has a non-trivial destructor that calls
// std::terminate() if the thread is still joinable - and that is exactly what happens on every exit() path that
// never reaches xrDebug::Finalize (os.exit() from Lua, xr_terminate, xrTheora_Stream.cpp, ppmd_compressor.cpp):
// ExitProcess tears down the thread, then DLL_PROCESS_DETACH runs this object's destructor, which sees it still
// joinable and terminates the process a second time, under the loader lock (undefined behaviour: a hang, or an
// "unexpected application termination" dialog on top of whatever caused the exit). A pointer is never destroyed by
// anything but xrSentry_Shutdown's own join-then-delete below; every other exit path just leaves it (the OS
// reclaims the thread handle, nothing of ours runs).
std::thread* s_queue_thread = nullptr;
// SEN-5/SEN-m3: guards sentry_capture_event/sentry_capture_minidump themselves (never the queue/gate bookkeeping,
// which stays under s_gate_mutex): the send worker holds it for its whole batch, xrSentry_CaptureDebugFail for its
// set-tag -> capture_minidump -> remove-tag sequence, and xrSentry_CaptureIniRStringError and xrSentry_Flush for
// their own direct sentry_capture_event/sentry_flush calls. Without it a Lua error (queued) and a R_ASSERT/VERIFY
// that follows it (xrDebug::Fail mutates the GLOBAL sentry scope) could interleave: the worker serializes the Lua
// event while the assert's tags/extra are still set, and the Lua error ships mislabeled as an assertion failure.
std::mutex s_capture_mutex;

// The tail file is the single registered attachment: it is written by the crash handlers and deleted at startup.
// While it is absent, sentry__prepare_event attaches nothing to ordinary events (a missing path is skipped).
char s_log_tail_path[kLogTailPathSize]{};
#if defined(XR_PLATFORM_WINDOWS)
// SEN-2/SEN-6: computed once in xrSentry_Initialize and used as-is on every crash path - never recomputed there,
// where XRay::Utf8::ToWide's heap allocation must not run (the crash may be an out-of-memory or a corrupted heap).
// s_log_path_w is also the SEN-6 fix: it comes from log_name(), which the engine already keeps as real UTF-8, so
// converting it to wide here is correct - unlike s_log_tail_path_w's old narrow form, which used to be built from
// an ANSI (code-page) LOCALAPPDATA and then re-decoded as UTF-8 (see xrSentry_Initialize).
wchar_t s_log_tail_path_w[kLogTailPathSize]{};
wchar_t s_log_path_w[kLogTailPathSize]{};
// SEN-2: guards sentry_write_log_tail_file's static scratch buffer and the tail file itself against two threads
// crashing at (almost) the same moment - the heap-corruption VEH and the top-level filter can both reach it.
volatile LONG s_in_log_tail_write = 0;
#endif

int64_t sentry_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool sentry_is_hex(char h)
{
    return (h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F');
}

// SEN-3: a digit run glued to a letter or underscore is part of an identifier (a section/object name: "zat_b14"
// vs "zat_b22" must stay distinct), not a value to collapse away.
bool sentry_is_ident_lead(char h)
{
    return (h >= 'a' && h <= 'z') || (h >= 'A' && h <= 'Z') || h == '_';
}

// Word-boundary check for a bare hex run (see below): alnum or underscore on either side means the run is part of
// a longer token, not a standalone address, so it is left untouched rather than risking a truncated identifier.
bool sentry_is_ident_glue(char h)
{
    return sentry_is_ident_lead(h) || (h >= '0' && h <= '9');
}

// True when msg[i] starts the line number right after "<name>.script:" / ".lua:" / ".cpp:" / ".h:" - two different
// bugs in the same file with the same wording ("attempt to index local 'npc'") but different lines must stay two
// distinct dedup keys, so that line number is kept verbatim instead of collapsing to '#'.
bool sentry_is_script_line_number(pcstr msg, size_t i)
{
    if (i == 0 || msg[i - 1] != ':')
        return false;
    static constexpr pcstr kExts[] = { ".script", ".lua", ".cpp", ".h" };
    for (pcstr ext : kExts)
    {
        const size_t len = xr_strlen(ext);
        if (i >= len + 1 && std::strncmp(msg + i - 1 - len, ext, len) == 0)
            return true;
    }
    return false;
}

// Normalizes the message and returns the FNV-1a 64 key of the normalized text. Collapsed to '#':
//  - a 0x-prefixed hex number (an address, a pointer-sized id), as before;
//  - a bare hex run of 8+ characters not glued to an identifier on either side: MSVC prints %p without "0x"
//    ("000001F2A3B4C5D6"), and the old rule only collapsed the digit sub-runs inside such a run, leaving the
//    letters A-F in place ("#F#A#B#C#D#") - two different addresses of the very same repeated error then produced
//    two different keys, so only the first ever reached Sentry;
//  - any other free-standing digit run (an object id, a count, a coordinate), as before.
// Kept verbatim, so it keeps two otherwise-identical messages apart (see the two helpers above):
//  - the line number right after ".script:" / ".lua:" / ".cpp:" / ".h:";
//  - a digit run glued to a letter or underscore ("zat_b14", "wpn_ak74") - a section/object id, not a value.
u64 sentry_message_key(pcstr msg, char* norm, size_t normSize)
{
    size_t o = 0;
    size_t i = 0;
    while (msg[i] && o + 1 < normSize)
    {
        const char c = msg[i];
        if (c == '0' && (msg[i + 1] == 'x' || msg[i + 1] == 'X') && sentry_is_hex(msg[i + 2]))
        {
            size_t len = 0;
            while (sentry_is_hex(msg[i + 2 + len]))
                ++len;
            if (o + 3 < normSize)
            {
                norm[o++] = '0';
                norm[o++] = 'x';
                norm[o++] = '#';
            }
            i += 2 + len;
            continue;
        }
        if (sentry_is_hex(c))
        {
            size_t len = 0;
            while (sentry_is_hex(msg[i + len]))
                ++len;
            const bool leftOk = i == 0 || !sentry_is_ident_glue(msg[i - 1]);
            const bool rightOk = !sentry_is_ident_glue(msg[i + len]);
            if (len >= 8 && leftOk && rightOk)
            {
                norm[o++] = '#';
                i += len;
                continue;
            }
            // too short to be a bare address, or glued to a token: fall through (a decimal digit goes through the
            // digit-run rule below, a bare hex letter A-F is copied as an ordinary character at the end)
        }
        if (c >= '0' && c <= '9')
        {
            size_t len = 0;
            while (msg[i + len] >= '0' && msg[i + len] <= '9')
                ++len;
            const bool attached = i > 0 && sentry_is_ident_lead(msg[i - 1]);
            const bool lineNumber = sentry_is_script_line_number(msg, i);
            if (attached || lineNumber)
            {
                for (size_t k = 0; k < len && o + 1 < normSize; ++k)
                    norm[o++] = msg[i + k];
            }
            else
                norm[o++] = '#';
            i += len;
            continue;
        }
        norm[o++] = c;
        ++i;
    }
    norm[o] = 0;

    u64 h = 14695981039346656037ULL;
    for (size_t k = 0; k < o; ++k)
    {
        h ^= static_cast<u8>(norm[k]);
        h *= 1099511628211ULL;
    }
    return h;
}

// Caller holds s_gate_mutex. One event per unique message per session, then the session cap and the rate window
// bound the total. A rate-limited message is NOT recorded as seen - its next occurrence may still go out (once).
// critical (SEN-m4): counts against kGateMaxCriticalEventsPerSession instead of kGateMaxEventsPerSession, so a
// flood of ordinary Lua/engine errors cannot starve the ini/assert-fallback events for the rest of the session;
// deduplication and the rate window are shared by both kinds.
bool sentry_gate_admit(u64 key, bool critical)
{
    const int64_t now = sentry_now_ms();
    if (now - s_gate_window_start >= kGateRateWindowMs)
    {
        s_gate_window_start = now;
        s_gate_sent_window = 0;
    }
    for (u64 seen : s_gate_keys)
    {
        if (seen == key)
            return false;
    }
    if (critical ? s_gate_sent_critical_session >= kGateMaxCriticalEventsPerSession
                 : s_gate_sent_session >= kGateMaxEventsPerSession)
        return false;
    if (s_gate_sent_window >= kGateMaxEventsPerWindow)
        return false;
    if (s_gate_keys.size() < kGateMaxKeys)
        s_gate_keys.push_back(key);
    if (critical)
        ++s_gate_sent_critical_session;
    else
        ++s_gate_sent_session;
    ++s_gate_sent_window;
    return true;
}

// Read-only peek for xrSentry_WouldCapture (SEN-m1): predicts sentry_gate_admit's answer for a NEW (not yet seen)
// non-critical message without recording anything - a caller that is about to do real work to build the payload
// (formatting a Lua stack trace) can skip it when the event would be dropped anyway. Mirrors sentry_gate_admit's
// window-reset prediction but never writes back s_gate_window_start/s_gate_sent_window: the real call still owns
// that transition, so the window is free to look "not yet due" if the real capture never follows this peek.
bool sentry_gate_would_admit_locked(u64 key)
{
    for (u64 seen : s_gate_keys)
    {
        if (seen == key)
            return false;
    }
    if (s_gate_sent_session >= kGateMaxEventsPerSession)
        return false;
    const int64_t now = sentry_now_ms();
    const unsigned windowCount =
        (now - s_gate_window_start >= kGateRateWindowMs) ? 0 : s_gate_sent_window;
    return windowCount < kGateMaxEventsPerWindow;
}

// Caller holds s_gate_mutex. One summary line instead of one line per suppressed event, at most once per minute.
void sentry_gate_note_suppressed()
{
    ++s_gate_suppressed;
    const int64_t now = sentry_now_ms();
    if (!s_gate_summary_ever || now - s_gate_summary_last >= kSuppressSummaryIntervalMs)
    {
        Msg("! [Sentry]: %u events suppressed", s_gate_suppressed);
        s_gate_suppressed_logged = s_gate_suppressed;
        s_gate_summary_last = now;
        s_gate_summary_ever = true;
    }
}

// Caller holds s_gate_mutex. Final count on flush/shutdown when it grew past the last printed line.
void sentry_gate_flush_summary()
{
    if (s_gate_suppressed > s_gate_suppressed_logged)
    {
        Msg("! [Sentry]: %u events suppressed", s_gate_suppressed);
        s_gate_suppressed_logged = s_gate_suppressed;
    }
}

// Caller holds s_gate_mutex. The worker runs sentry_capture_event off the game logic thread.
void sentry_queue_push(sentry_value_t event)
{
    if (s_queue.size() >= kQueueMaxPending)
    {
        sentry_value_decref(event);
        sentry_gate_note_suppressed();
        return;
    }
    s_queue.push_back(event);
    s_gate_cv.notify_one();
}

// SEN-1/SEN-m6: waits on the condition variable with a predicate - no more periodic 250 ms wake-ups with nothing to
// do (the thread's name and per-thread init now come from Threading::RunThread, see xrSentry_Initialize).
// SEN-5/SEN-m3: sentry_capture_event itself runs under s_capture_mutex, for the whole batch, so
// xrSentry_CaptureDebugFail's tag/extra scope mutation and xrSentry_Flush's own capture calls cannot interleave
// with it (see s_capture_mutex's own comment above).
void sentry_queue_worker()
{
    for (;;)
    {
        std::vector<sentry_value_t> batch;
        {
            std::unique_lock<std::mutex> lock(s_gate_mutex);
            s_gate_cv.wait(lock, [] { return !s_queue.empty() || s_queue_stop; });
            batch.swap(s_queue);
            if (batch.empty() && s_queue_stop)
                return;
        }
        std::lock_guard<std::mutex> captureLock(s_capture_mutex);
        for (sentry_value_t event : batch)
            sentry_capture_event(event);
    }
}

// Copies the last <= 1 MB of the game log into the attachment file. Crash-safe on Windows: WinAPI, the precomputed
// wide paths (s_log_path_w / s_log_tail_path_w - SEN-2/SEN-6, set once in xrSentry_Initialize) and a static buffer
// only; the crash paths must not allocate from a possibly damaged heap, and must not re-derive a UTF-8 path from
// an ANSI environment variable either (SEN-6). stdio elsewhere (no wide/narrow mismatch there: HOME is read as-is).
bool sentry_write_log_tail_file_impl()
{
#if defined(XR_PLATFORM_WINDOWS)
    if (!s_log_tail_path_w[0] || !s_log_path_w[0])
        return false;
#else
    if (!s_log_tail_path[0])
        return false;
    const pcstr logPath = log_name();
    if (!logPath || !*logPath)
        return false;
#endif

    static unsigned char s_log_tail_buf[kLogTailMaxBytes];
    u64 got = 0;
    bool truncated = false; // the window does not start at the real beginning of the log (SEN-m7)
#if defined(XR_PLATFORM_WINDOWS)
    const HANDLE hIn = CreateFileW(s_log_path_w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hIn == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(hIn, &size);
    const u64 total = static_cast<u64>(size.QuadPart);
    const u64 tail = total < kLogTailMaxBytes ? total : kLogTailMaxBytes;
    truncated = total > tail;
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(total - tail);
    SetFilePointerEx(hIn, pos, nullptr, FILE_BEGIN);
    DWORD read = 0;
    ReadFile(hIn, s_log_tail_buf, static_cast<DWORD>(tail), &read, nullptr);
    CloseHandle(hIn);
    got = read;
#else
    FILE* in = std::fopen(logPath, "rb");
    if (!in)
        return false;
    std::fseek(in, 0, SEEK_END);
    const long total = std::ftell(in);
    if (total < 0)
    {
        std::fclose(in);
        return false;
    }
    const long tail = static_cast<size_t>(total) < kLogTailMaxBytes ? total : static_cast<long>(kLogTailMaxBytes);
    truncated = static_cast<long>(total) > tail;
    std::fseek(in, total - tail, SEEK_SET);
    got = std::fread(s_log_tail_buf, 1, tail, in);
    std::fclose(in);
#endif
    if (!got)
        return false;

    // SEN-m7: when the 1 MB window does not start at the real beginning of the log, start the tail at the next
    // '\n' instead - otherwise the first line is a fragment cut off mid-text (possibly mid-UTF-8-character). The
    // buffer is already fully read, so this costs a scan, no extra I/O.
    unsigned char* writeFrom = s_log_tail_buf;
    u64 writeLen = got;
    if (truncated)
    {
        u64 skip = 0;
        while (skip < writeLen && writeFrom[skip] != '\n')
            ++skip;
        if (skip < writeLen)
        {
            ++skip;
            writeFrom += skip;
            writeLen -= skip;
        }
    }
    if (!writeLen)
        return false;

#if defined(XR_PLATFORM_WINDOWS)
    const HANDLE hOut = CreateFileW(
        s_log_tail_path_w, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    WriteFile(hOut, writeFrom, static_cast<DWORD>(writeLen), &written, nullptr);
    CloseHandle(hOut);
    return written == writeLen;
#else
    FILE* out = std::fopen(s_log_tail_path, "wb");
    if (!out)
        return false;
    const size_t written = std::fwrite(writeFrom, 1, writeLen, out);
    std::fclose(out);
    return written == writeLen;
#endif
}

// SEN-2: FlushLog (takes logCS) plus the read/write above are now reached from inside a crash handler (the on_crash
// hook of Crashpad fires on STATUS_HEAP_CORRUPTION too) with no protection at all - a damaged heap or an
// out-of-memory condition (the usual causes of a crash in the first place) could fault again right here, and an
// unguarded second exception inside the UEF/VEH never reaches Crashpad: no report, no minidump, the process just
// dies. __try/__except around the whole thing (as save_local_crash_report below already does), plus a one-at-a-
// time guard so a second thread crashing at (nearly) the same moment cannot write into the same static buffer and
// the same file concurrently. A plain wrapper, not the function itself: __try cannot be used in a function holding
// objects that require unwinding (none here, but see save_local_crash_report for why this split exists).
bool sentry_write_log_tail_file()
{
#if defined(XR_PLATFORM_WINDOWS)
    if (InterlockedCompareExchange(&s_in_log_tail_write, 1, 0) != 0)
        return false;
    bool ok = false;
    __try
    {
        FlushLog();
        ok = sentry_write_log_tail_file_impl();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ok = false;
    }
    InterlockedExchange(&s_in_log_tail_write, 0);
    return ok;
#else
    FlushLog();
    return sentry_write_log_tail_file_impl();
#endif
}

// Registers the crash-only log tail as the single attachment and removes a stale tail left by a previous crash
// (an existing file would be attached to every event until overwritten).
void sentry_add_log_tail_attachment(sentry_options_t* options)
{
    if (!options || !s_log_tail_path[0])
        return;
    // Remove a stale tail from a previous crash: an existing file would be attached to every event.
#if defined(XR_PLATFORM_WINDOWS)
    DeleteFileW(s_log_tail_path_w);
    sentry_options_add_attachmentw(options, s_log_tail_path_w);
#else
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(s_log_tail_path), ec);
    sentry_options_add_attachment(options, s_log_tail_path);
#endif
#ifndef MASTER_GOLD
    Msg("[Sentry]: crash log tail attachment [%s]", s_log_tail_path);
#endif
}

#if defined(XR_PLATFORM_WINDOWS)
// SEN-4: before sentry_init, which otherwise reads every "<dbPath>\<uuid>.run\*.envelope" synchronously into
// memory and queues it for upload (sentry_database.c: sentry__process_old_runs). A build before this fix also
// attached the whole game log to every Sentry event (sentry_add_logs_folder_attachments), so a player who had not
// started the game in a while could accumulate hundreds of envelopes at tens to hundreds of MB each: sentry_init
// would read and hold all of that in memory on the first start of a fixed build, and try to upload envelopes far
// past Sentry's own size limit (rejected there anyway). An ordinary event's envelope is on the order of 10-20 KB;
// anything far larger is leftover from that old format.
void prune_old_envelopes(const wchar_t* dbPathW)
{
    constexpr uintmax_t kMaxEnvelopeBytes = 2u * 1024 * 1024;
    size_t pruned = 0;
    uintmax_t prunedBytes = 0;
    std::error_code ec;
    std::filesystem::directory_iterator runDirs(dbPathW, ec);
    if (ec)
        return;
    for (const auto& runEntry : runDirs)
    {
        std::error_code dirEc;
        if (!runEntry.is_directory(dirEc) || dirEc || runEntry.path().extension() != L".run")
            continue;
        std::error_code innerEc;
        std::filesystem::directory_iterator files(runEntry.path(), innerEc);
        if (innerEc)
            continue;
        for (const auto& fileEntry : files)
        {
            std::error_code fileEc;
            if (!fileEntry.is_regular_file(fileEc) || fileEc || fileEntry.path().extension() != L".envelope")
                continue;
            const uintmax_t size = fileEntry.file_size(fileEc);
            if (fileEc || size <= kMaxEnvelopeBytes)
                continue;
            std::error_code removeEc;
            if (std::filesystem::remove(fileEntry.path(), removeEc))
            {
                ++pruned;
                prunedBytes += size;
            }
        }
    }
    if (pruned)
        Msg("[Sentry]: pruned %zu old envelopes (%llu MB)", pruned, static_cast<unsigned long long>(prunedBytes >> 20));
}
#endif

// Crashpad invokes this in its first-chance handler, before the handler process reads the attachment files.
// Crashpad ignores the returned event contents (the minidump carries the crash state) - the side effect is the point.
sentry_value_t sentry_on_crash_hook(const sentry_ucontext_t* /*uctx*/, sentry_value_t event, void* /*closure*/)
{
    sentry_write_log_tail_file(); // flushes the log and writes the tail itself (SEN-2), guarded
    return event;
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

using MiniDumpWriteDump_t = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

MiniDumpWriteDump_t s_minidump_write_dump = nullptr;

// Resolved once at startup. LoadLibrary takes the loader lock, and a crash that happens while some
// other thread holds it (inside DllMain, during LoadLibrary) would hang the handler before the dump
// is written. On the crash path only the stored pointer is used. The module is never released.
void init_minidump_writer()
{
    if (s_minidump_write_dump)
        return;

    const HMODULE dbghelpMod = LoadLibraryW(L"dbghelp.dll");
    if (!dbghelpMod)
    {
        Msg("! [Sentry]: dbghelp.dll not available, local minidumps are disabled");
        return;
    }

    s_minidump_write_dump =
        reinterpret_cast<MiniDumpWriteDump_t>(GetProcAddress(dbghelpMod, "MiniDumpWriteDump"));
    if (!s_minidump_write_dump)
        Msg("! [Sentry]: MiniDumpWriteDump not found, local minidumps are disabled");
}

// Writes a minidump of the current process to `utf8Path`.
// Pass exception pointers for real crashes so the dump carries the faulting context.
bool write_minidump_to_path(const char* utf8Path, EXCEPTION_POINTERS* exPtrs)
{
    if (!s_minidump_write_dump)
    {
        // Before init_minidump_writer ran (an early assertion). GetModuleHandle, never LoadLibrary:
        // this may already be the crash path. The module is normally in by then, because
        // PreloadStackTraceLibrary loads it from xrDebug::Initialize.
        if (const HMODULE dbghelpMod = GetModuleHandleW(L"dbghelp.dll"))
        {
            s_minidump_write_dump =
                reinterpret_cast<MiniDumpWriteDump_t>(GetProcAddress(dbghelpMod, "MiniDumpWriteDump"));
        }

        if (!s_minidump_write_dump)
            return false;
    }

    const std::wstring wide = XRay::Utf8::ToWide(utf8Path);
    if (wide.empty())
        return false;

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    if (exPtrs)
    {
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = exPtrs;
        mei.ClientPointers = FALSE;
    }

    // The full type first; if dbghelp fails on it (a huge or damaged address space, an out-of-memory state), once
    // more with MiniDumpNormal - threads, stacks and modules are enough to read the crash. Each failure goes to the
    // log with its error code (2026-10-20: a crash report was lost with a bare "failed to save").
    const MINIDUMP_TYPE dumpTypes[] = {
        static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory |
            MiniDumpWithProcessThreadData | MiniDumpWithThreadInfo),
        MiniDumpNormal,
    };
    const pcstr dumpTypeNames[] = { "full", "normal" };
    for (size_t attempt = 0; attempt < std::size(dumpTypes); ++attempt)
    {
        const HANDLE hFile = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            Msg("! [Sentry]: minidump: cannot create [%s], win32 error %lu", utf8Path, GetLastError());
            return false;
        }

        const BOOL ok = s_minidump_write_dump(GetCurrentProcess(), GetCurrentProcessId(), hFile, dumpTypes[attempt],
            exPtrs ? &mei : nullptr, nullptr, nullptr);
        // MiniDumpWriteDump reports its error as an HRESULT through GetLastError
        const DWORD error = ok ? 0 : GetLastError();
        CloseHandle(hFile);
        if (ok)
        {
            if (attempt != 0)
                Msg("! [Sentry]: minidump written with the %s type", dumpTypeNames[attempt]);
            return true;
        }
        Msg("! [Sentry]: minidump (%s type) failed, error 0x%08lX", dumpTypeNames[attempt], error);
    }
    return false;
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
PVOID s_heap_corruption_veh = nullptr;
volatile LONG s_in_local_crash_report = 0;
volatile LONG s_heap_corruption_logged = 0;

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
        std::snprintf(pathOut, pathOutSize, "%scrash_%s.zip", s_crash_reports_root, name);

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

// Heap walk: count busy blocks, total bytes and a size histogram per heap.
constexpr DWORD kBucketBounds[] = { 64, 256, 1024, 4096, 16384, 65536, 262144, 1048576 };
constexpr size_t kBucketCount = sizeof(kBucketBounds) / sizeof(kBucketBounds[0]); // + overflow bucket
constexpr size_t kTopBlocks = 8;

struct TopBlock
{
    size_t size;
    const void* addr;
};

// One heap, lock held for the whole walk. HeapWalk faults on damaged metadata, and __finally is what
// guarantees the unlock: the project builds without /EHa, so an SEH exception caught by a caller
// would not run destructors, and a heap left locked hangs every later allocation in the process.
// A function using __finally must hold no objects requiring unwinding, hence the plain parameters.
void walk_one_heap(HANDLE heap, u64& busyBlocks, u64& busyBytes, u64& freeBlocks, u64& freeBytes,
    u64* buckets, TopBlock* top)
{
    if (!HeapLock(heap))
        return;

    __try
    {
        PROCESS_HEAP_ENTRY entry{};
        while (HeapWalk(heap, &entry))
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
    }
    __finally
    {
        HeapUnlock(heap);
    }
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

        // No allocations from here until HeapUnlock (see note above). HeapWalk can fault on damaged
        // metadata, and the unlock must happen anyway: a heap left locked hangs every later
        // allocation in the process, including Crashpad's.
        walk_one_heap(heaps[hi], busyBlocks, busyBytes, freeBlocks, freeBytes, buckets, top);

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

    // 50z5-extra-diagnostics.md: the frame number, the phase of the main thread and of this one, the load stage and the
    // last [frame] / [frame-spike] / [io] / [sys] lines (formatted into a static buffer: no allocation, no lock)
    static char s_frame_context[20 * 1024];
    if (xray::perf::CrashContextFormat(s_frame_context, sizeof(s_frame_context)))
        out += s_frame_context;

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

    // Every step that can fail records its name and the error codes: a bare "failed to save" told nothing
    // (2026-10-20). win32 = GetLastError() right after the step; zip = the miniz error of the archive.
    pcstr failedStep = nullptr;
    DWORD failedWin32 = 0;
    mz_zip_error failedZip = MZ_ZIP_NO_ERROR;

    mz_zip_archive zip{};
    void* zipBuf = nullptr;
    size_t zipSize = 0;
    char zipPath[kCrashPathSize]{};
    const auto fail = [&](pcstr step, bool zipStep) {
        failedStep = step;
        failedWin32 = GetLastError();
        if (zipStep)
            failedZip = mz_zip_get_last_error(&zip);
    };
    bool ok = mz_zip_writer_init_heap(&zip, 0, 0) != 0;
    if (!ok)
        fail("zip init", true);
    if (ok)
    {
        if (haveDump)
        {
            ok = zip_add_win_file(&zip, dumpPath, "minidump.dmp");
            if (!ok)
                fail("zip: add minidump.dmp", true);
        }
        if (ok && logPath && *logPath)
        {
            pcstr fileName = std::strrchr(logPath, '\\');
            const pcstr fileNameFwd = std::strrchr(logPath, '/');
            if (fileNameFwd && (!fileName || fileNameFwd > fileName))
                fileName = fileNameFwd;
            fileName = fileName ? fileName + 1 : logPath;
            ok = zip_add_win_file(&zip, logPath, fileName);
            if (!ok)
                fail("zip: add the log", true);
        }
        if (ok)
        {
            xr_string memStats;
            append_memory_stats(memStats);
            if (!memStats.empty())
            {
                ok = mz_zip_writer_add_mem(&zip, "memory_stats.txt", memStats.data(), memStats.size(), MZ_DEFAULT_LEVEL) != 0;
                if (!ok)
                    fail("zip: add memory_stats.txt", true);
            }
        }
        if (ok)
        {
            ok = mz_zip_writer_finalize_heap_archive(&zip, &zipBuf, &zipSize) != 0 && zipBuf != nullptr;
            if (!ok)
                fail("zip finalize", true);
        }
    }
    mz_zip_writer_end(&zip);

    if (ok)
    {
        ok = write_crash_zip_unique(zipBuf, zipSize, zipPath, sizeof(zipPath));
        if (!ok)
            fail("write the zip to the reports folder", false);
    }
    mz_free(zipBuf);

    if (ok && zipPath[0])
    {
        if (dumpPath[0])
            DeleteFileW(XRay::Utf8::ToWide(dumpPath).c_str());
        Msg("! [Sentry]: local crash report saved to [%s] (%zu bytes)", zipPath, zipSize);
        FlushLog();
        return;
    }

    Msg("! [Sentry]: failed to save local crash report: %s (win32 error %lu, zip error %d %s)",
        failedStep ? failedStep : "?", failedWin32, int(failedZip),
        failedZip != MZ_ZIP_NO_ERROR ? mz_zip_get_error_string(failedZip) : "-");

    // The zip failed but the minidump may be fine: keep it - move it from %TEMP% into the reports folder as a bare
    // .dmp (the same place and naming as save_minidump_only) instead of deleting it.
    bool dumpKept = false;
    if (haveDump && dumpPath[0] && s_crash_reports_ready)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char keptPath[kCrashPathSize];
        std::snprintf(keptPath, sizeof(keptPath), "%scrash_%04u-%02u-%02u_%02u-%02u-%02u_%03u.dmp",
            s_crash_reports_root, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds);
        dumpKept = MoveFileExW(XRay::Utf8::ToWide(dumpPath).c_str(), XRay::Utf8::ToWide(keptPath).c_str(),
                       MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING) != FALSE;
        if (dumpKept)
            Msg("! [Sentry]: the minidump is kept as [%s] (without the log)", keptPath);
        else
            Msg("! [Sentry]: cannot move the minidump [%s] to [%s], win32 error %lu - left in place", dumpPath,
                keptPath, GetLastError());
        dumpKept = true; // moved, or left in %TEMP% for a manual pick-up: never deleted on this path
    }
    if (!dumpKept && dumpPath[0])
        DeleteFileW(XRay::Utf8::ToWide(dumpPath).c_str()); // an empty or failed temp file
    FlushLog();
}

// Minidump only, written straight into the reports folder. Used when the heap is the thing that
// broke: the zip writer and the log reader of the full report both allocate, and an allocation on a
// corrupted heap can hang on its lock or fault again. This path only formats a path and calls
// dbghelp, which still allocates internally, but nothing of ours does.
void save_minidump_only_impl(EXCEPTION_POINTERS* exPtrs)
{
    if (!s_crash_reports_ready || !s_minidump_write_dump)
        return;

    SYSTEMTIME st;
    GetLocalTime(&st);

    char path[kCrashPathSize];
    std::snprintf(path, sizeof(path), "%scrash_%04u-%02u-%02u_%02u-%02u-%02u_%03u.dmp",
        s_crash_reports_root, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        st.wMilliseconds);

    write_minidump_to_path(path, exPtrs);
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

// Shares the guard with save_local_crash_report on purpose: dbghelp is not thread safe, and since
// the heap handler got its own dump path two threads could otherwise be inside MiniDumpWriteDump at
// once. The SEH frame matters here too, the dump is taken from a heap that is known to be broken.
void save_minidump_only(EXCEPTION_POINTERS* exPtrs)
{
    if (InterlockedCompareExchange(&s_in_local_crash_report, 1, 0) != 0)
        return;
    __try
    {
        save_minidump_only_impl(exPtrs);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    InterlockedExchange(&s_in_local_crash_report, 0);
}

LONG WINAPI local_crash_report_filter(EXCEPTION_POINTERS* exPtrs)
{
    // Dump first. Logging allocates and calls back into the engine, so on an exhausted stack or a
    // broken allocator it can fault again before writing anything, and the minidump would be lost
    // with it. The log on disk still gets the crash block below, only the copy inside the zip
    // predates it. Note that a stack overflow on the game thread is handled by its own SEH frame in
    // entry_point.cpp and never reaches this filter.
    if (s_crash_reports_ready)
        save_local_crash_report(exPtrs);

    // This is the only place a native call stack reaches the log: Crashpad's filter never returns,
    // and xrDebug::UnhandledFilter sits below it, so it is not reached once Sentry is up.
    xrDebug::LogCrashInfoGuarded(exPtrs, nullptr);
    // The crash-only log tail for the crashpad upload: written after the crash block above so the tail
    // includes it. The file is absent during normal play (deleted at startup), so ordinary events attach
    // nothing. No queue drain here: the pending events are deduped repeats and a crash handler must not
    // block on sentry locks. sentry_write_log_tail_file flushes the log and writes the tail itself (SEN-2),
    // under its own InterlockedCompareExchange guard and __try/__except.
    sentry_write_log_tail_file();
    // Chain into the previously installed filter (Crashpad / engine) to keep the existing flow.
    if (s_prev_uef)
        return s_prev_uef(exPtrs);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Heap corruption never reaches a top-level filter: Crashpad catches it in a vectored handler and
// terminates the process. Ours is registered first (and after Crashpad's, so it is at the head of
// the list) and only reacts to that one code — a VEH also sees exceptions that are handled later,
// such as C++ throws and the __try blocks scattered around the game.
LONG WINAPI heap_corruption_veh(EXCEPTION_POINTERS* exPtrs)
{
    if (exPtrs && exPtrs->ExceptionRecord &&
        exPtrs->ExceptionRecord->ExceptionCode == STATUS_HEAP_CORRUPTION &&
        InterlockedCompareExchange(&s_heap_corruption_logged, 1, 0) == 0)
    {
        // A vectored handler sees the exception first-chance, so the process may well survive it.
        // Report it once, then hand the crash-block budget back for a later, fatal crash.
        // Only a bare minidump here: zipping and reading the log would allocate from the very heap
        // that is reported as corrupted.
        save_minidump_only(exPtrs);
        xrDebug::LogCrashInfoGuarded(exPtrs, "heap corruption", false);
    }
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
    init_minidump_writer(); // resolve dbghelp now, never from inside a crash handler
    // Must be installed after sentry_init: the filter installed last runs first,
    // so we save the local report and then delegate to Crashpad's filter.
    s_prev_uef = SetUnhandledExceptionFilter(local_crash_report_filter);
    s_heap_corruption_veh = AddVectoredExceptionHandler(1 /* first */, heap_corruption_veh);
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
    if (s_heap_corruption_veh)
    {
        RemoveVectoredExceptionHandler(s_heap_corruption_veh);
        s_heap_corruption_veh = nullptr;
    }
    s_local_crash_filter_installed = false;
#endif
}

void XRCORE_API xrSentry_SaveLocalCrashReport(EXCEPTION_POINTERS* exPtrs)
{
#if defined(XR_PLATFORM_WINDOWS)
    if (s_crash_reports_ready)
        save_local_crash_report(exPtrs);
#else
    (void)exPtrs;
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
    // SEN-6: LOCALAPPDATA read wide. _dupenv_s returns it in the ANSI code page (cp1251 on a Cyrillic Windows
    // install), while sentry_options_set_database_path and every narrow path built from it downstream are treated
    // as UTF-8 (sentry_string.c, XRay::Utf8::ToWide): a profile name with Cyrillic letters produced a path nothing
    // could open - CreateFileW/sentry pointed at bytes that do not spell that path in either encoding. The tail
    // never got written (and never deleted either), and the database itself ended up somewhere unreachable.
    wchar_t dbPathW[MAX_PATH * 2]{};
    {
        wchar_t* localAppDataW = nullptr;
        size_t lenW = 0;
        if (_wdupenv_s(&localAppDataW, &lenW, L"LOCALAPPDATA") == 0 && localAppDataW && localAppDataW[0])
        {
            std::error_code ec;
            const std::filesystem::path dbDir =
                std::filesystem::path(localAppDataW) / L"OpenXRay" / L"sentry-native";
            std::filesystem::create_directories(dbDir, ec);
            wcsncpy_s(dbPathW, dbDir.c_str(), _TRUNCATE);
            free(localAppDataW);
        }
        else
        {
            std::error_code ec;
            std::filesystem::create_directories(L".\\OpenXRay\\sentry-native", ec);
            wcsncpy_s(dbPathW, L".\\OpenXRay\\sentry-native", _TRUNCATE);
        }
    }
    sentry_options_set_database_pathw(options, dbPathW);
    // SEN-4: before sentry_init reads the whole database back in.
    prune_old_envelopes(dbPathW);

    // Narrow UTF-8 form, for the log line only (xrSentry_UpdateHwTags/install_local_crash_reporter print it);
    // never round-tripped back into a WinAPI/sentry call.
    char dbPath[MAX_PATH * 2]{};
    xr_strcpy(dbPath, sizeof(dbPath), XRay::Utf8::FromWide(dbPathW).c_str());
    xr_strcpy(s_log_tail_path, sizeof(s_log_tail_path), dbPath);
    xr_strcat(s_log_tail_path, sizeof(s_log_tail_path), "\\log_tail.txt");

    // SEN-2: the wide tail path and the wide log path, computed once here - never on the crash path, where
    // XRay::Utf8::ToWide's heap allocation must not run (the crash may be an out-of-memory or a corrupted heap).
    // The log path is fixed for the whole run once CreateLog ran, which is well before Sentry starts.
    wcsncpy_s(s_log_tail_path_w, dbPathW, _TRUNCATE);
    wcsncat_s(s_log_tail_path_w, L"\\log_tail.txt", _TRUNCATE);
    if (const pcstr logPath = log_name(); logPath && *logPath)
    {
        const std::wstring logPathW = XRay::Utf8::ToWide(logPath);
        wcsncpy_s(s_log_path_w, logPathW.c_str(), _TRUNCATE);
    }

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
        xr_strcpy(s_log_tail_path, sizeof(s_log_tail_path), db.string().c_str());
    }
    else
    {
        std::error_code ec;
        std::filesystem::create_directories(".sentry-native", ec);
        sentry_options_set_database_path(options, ".sentry-native");
        xr_strcpy(s_log_tail_path, sizeof(s_log_tail_path), ".sentry-native");
    }
    xr_strcat(s_log_tail_path, sizeof(s_log_tail_path), "/log_tail.txt");
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

    // Crash-only attachments (run7-freezes-p5.md §6.2): the tail file, not the whole log - and only while it
    // exists, which is just around a crash. Ordinary events attach nothing.
    sentry_add_log_tail_attachment(options);
    sentry_options_set_on_crash(options, sentry_on_crash_hook, nullptr);

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

    // Handlers first, flag second: the flag is what lets xrDebug::Fail take a live minidump, and
    // between the two there would otherwise be a window where the dump writer is not resolved yet.
    install_local_crash_reporter();
    s_xr_sentry_started = true;
    // The send worker: sentry_capture_event never runs on the game logic thread again. See s_queue_thread's own
    // comment (SEN-1) for why it is a pointer, never joined except from xrSentry_Shutdown below.
    s_queue_stop = false;
    s_queue_thread = new std::thread(Threading::RunThread("Sentry Send", &sentry_queue_worker));
    // Sentry starts after the render device: the profile has all three stages here
    xrSentry_UpdateHwTags();

    if (commandLine && std::strstr(commandLine, "-sentry_test_av_crash"))
    {
        volatile int* p = nullptr;
        *p = 42;
    }
}

// Crash reports grouped by the machine (plans/optimization_mt/50-smart-engine-hw-profile.md, section 4.2)
void xrSentry_UpdateHwTags()
{
    if (!s_xr_sentry_started)
        return;
    const xray::hw::Profile& p = xray::hw::GetProfile();
    constexpr double gib = double(u64(1) << 30);
    constexpr double mib = double(u64(1) << 20);
    string64 buf;
    sentry_set_tag("hw.class", xray::hw::TierName(p.tiers.overall));
    xr_sprintf(buf, "%.0f", p.mem.ram_total / gib);
    sentry_set_tag("hw.ram_gb", buf);
    xr_sprintf(buf, "%.0f", p.mem.commit_limit / gib);
    sentry_set_tag("hw.commit_gb", buf);
    sentry_set_tag("hw.disk", xray::hw::DiskKindName(p.data.game.kind));
    if (p.stages & (xray::hw::STAGE_C | xray::hw::STAGE_C_PROBE))
        xr_sprintf(buf, "%.0f", p.gpu.dedicated / mib);
    else
        xr_strcpy(buf, "?");
    sentry_set_tag("hw.vram_mb", buf);
    xr_sprintf(buf, "%uc/%ut", p.cpu.physical_avail, p.cpu.logical_avail);
    sentry_set_tag("hw.cpu", buf);
    sentry_set_tag("hw.wine", p.env.wine ? (p.env.wine_version[0] ? p.env.wine_version : "1") : "0");
}

void xrSentry_Shutdown()
{
    uninstall_local_crash_reporter();
    if (s_queue_thread)
    {
        {
            std::lock_guard<std::mutex> lock(s_gate_mutex);
            s_queue_stop = true;
        }
        s_gate_cv.notify_all();
        s_queue_thread->join(); // the worker drains the queue before it exits
        delete s_queue_thread;
        s_queue_thread = nullptr;
    }
    if (!s_xr_sentry_started)
        return;
    {
        std::lock_guard<std::mutex> lock(s_gate_mutex);
        sentry_gate_flush_summary();
    }
    sentry_close();
    s_xr_sentry_started = false;
}

// The provider is kept by xrDebug: crash logging needs it whether or not Sentry is compiled in.
void XRCORE_API xrSentry_SetLuaStackProvider(xrSentry_LuaStackFn fn) { xrDebug::SetLuaStackProvider(fn); }

static void sentry_event_attach_lua_stack(sentry_value_t event, pcstr lua_stack)
{
    constexpr size_t kMaxLua = 12000;
    xr_string from_provider;
    const auto lua_stack_provider = xrDebug::GetLuaStackProvider();
    if ((!lua_stack || !*lua_stack) && lua_stack_provider)
    {
        lua_stack_provider(from_provider);
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

// Common path of CaptureError/CaptureSoftError: the gate rejects repeats and floods before any work; the event
// is built on the calling thread (the native stack must be the erroring one, the Lua stack provider walks a live
// Lua state) and only sentry_capture_event runs on the send worker.
static void sentry_capture_gated(
    sentry_level_t level, pcstr logger, pcstr message, pcstr lua_stack, pcstr tag_key, bool critical = false)
{
    char norm[kGateNormalizedMax];
    {
        std::lock_guard<std::mutex> lock(s_gate_mutex);
        const u64 key = sentry_message_key(message, norm, sizeof(norm));
        if (!sentry_gate_admit(key, critical))
        {
            sentry_gate_note_suppressed();
            return;
        }
    }

    sentry_value_t event = sentry_value_new_message_event(level, logger, message);

    sentry_value_t tags = sentry_value_new_object();
    sentry_value_set_by_key(tags, tag_key, sentry_value_new_string("true"));
    sentry_value_set_by_key(event, "tags", tags);

    sentry_event_attach_lua_stack(event, lua_stack);

    sentry_event_value_add_stacktrace(event, nullptr, 0);

    std::lock_guard<std::mutex> lock(s_gate_mutex);
    sentry_queue_push(event);
}

void XRCORE_API xrSentry_CaptureSoftError(pcstr logger, pcstr message, pcstr lua_stack)
{
    if (!s_xr_sentry_started || !message || !*message)
        return;

    // Tag for issue search / alert rules in Sentry: soft_error:true
    sentry_capture_gated(SENTRY_LEVEL_WARNING, logger ? logger : "OpenXRay", message, lua_stack, "soft_error");
}

void XRCORE_API xrSentry_CaptureError(pcstr logger, pcstr message, pcstr lua_stack)
{
    if (!s_xr_sentry_started || !message || !*message)
        return;

    sentry_capture_gated(SENTRY_LEVEL_ERROR, logger ? logger : "OpenXRay", message, lua_stack, "engine_error");
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
    xr_string lua_for_extra;
    const auto lua_stack_provider = xrDebug::GetLuaStackProvider();
    if (lua_stack_provider)
    {
        lua_stack_provider(lua_for_extra);
        constexpr size_t kMaxLua = 12000;
        if (lua_for_extra.size() > kMaxLua)
            lua_for_extra.resize(kMaxLua);
    }

    char dumpPath[MAX_PATH]{};
    bool dumped = false;
    {
        // SEN-5: the scope mutation below (tags/extra, then sentry_capture_minidump reading the event) is global
        // state - sentry__prepare_event applies it to whatever event the send worker is serializing at that exact
        // moment, which is now a different thread. Without this lock a queued Lua error could ship tagged
        // "xrDebug.Fail=true" with this assertion's text in extra.assertion, mislabeled as an assertion failure.
        std::lock_guard<std::mutex> captureLock(s_capture_mutex);
        sentry_set_tag("xrDebug.Fail", "true");
        sentry_set_tag("logger", "xrCore.xrDebug.Fail");
        sentry_set_extra("assertion", sentry_value_new_string(m.c_str()));
        if (!lua_for_extra.empty())
            sentry_set_extra("lua_stack", sentry_value_new_string(lua_for_extra.c_str()));

        dumped = write_live_minidump_to_temp(dumpPath, sizeof(dumpPath));
        if (dumped)
        {
            // The minidump is read into the envelope right here, so the file can go at once. No flush: the WinHTTP
            // transport uploads in the background, and waiting for it here (up to 10 s, it timed out on players'
            // machines) held up the error dialog. xrDebug::Fail flushes with xrSentry_Flush only when the player
            // chose to exit.
            sentry_capture_minidump(dumpPath);
            DeleteFileW(XRay::Utf8::ToWide(dumpPath).c_str());
        }

        if (!lua_for_extra.empty())
            sentry_remove_extra("lua_stack");
        sentry_remove_extra("assertion");
        sentry_remove_tag("logger");
        sentry_remove_tag("xrDebug.Fail");
    }

    if (!dumped)
#endif
    {
        // SEN-m4: a budget of its own (not xrSentry_CaptureError's shared one) - an assert that could not take a
        // live minidump must not be starved by a flood of ordinary Lua errors that already filled that budget.
        sentry_capture_gated(SENTRY_LEVEL_ERROR, "xrCore.xrDebug.Fail", m.c_str(), nullptr, "engine_error",
            /*critical=*/true);
    }
}

void xrSentry_Flush(u32 timeoutMs)
{
    if (!s_xr_sentry_started)
        return;
    // Hand everything we hold to the transport first (the worker may still be capturing; both paths are
    // thread-safe), then wait for the transport itself.
    std::vector<sentry_value_t> batch;
    {
        std::lock_guard<std::mutex> lock(s_gate_mutex);
        batch.swap(s_queue);
        sentry_gate_flush_summary();
    }
    {
        std::lock_guard<std::mutex> captureLock(s_capture_mutex);
        for (sentry_value_t event : batch)
            sentry_capture_event(event);
        if (sentry_flush(timeoutMs) != 0)
        {
#ifndef MASTER_GOLD
            Msg("! [Sentry]: sentry_flush timed out after %u ms (report may be incomplete)", timeoutMs);
#endif
        }
    }
}

void XRCORE_API xrSentry_CaptureIniRStringError(pcstr ini_path, pcstr section, pcstr key)
{
    if (!s_xr_sentry_started)
        return;

    char message[768];
    xr_sprintf(message, sizeof(message), "Can't find variable %s in [%s]", key ? key : "?", section ? section : "?");

    char norm[kGateNormalizedMax];
    {
        std::lock_guard<std::mutex> lock(s_gate_mutex);
        const u64 msgKey = sentry_message_key(message, norm, sizeof(norm));
        if (!sentry_gate_admit(msgKey, /*critical=*/true))
        {
            sentry_gate_note_suppressed();
            return;
        }
    }

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
    // Captured synchronously on purpose: the caller is about to xrDebug::Fatal, and a queued event would be
    // lost if the process goes down before the send worker runs. The envelope is tiny (no log attachment).
    // SEN-5: under s_capture_mutex, same as every other sentry_capture_event/sentry_capture_minidump call.
    {
        std::lock_guard<std::mutex> captureLock(s_capture_mutex);
        sentry_capture_event(event);
    }
}

// SEN-m1: lets a caller skip building an expensive payload (formatting a Lua stack trace, for one) when the event
// would be dropped anyway. Read-only: does not record the key or consume a slot - the real xrSentry_CaptureError /
// xrSentry_CaptureSoftError call still does that admission, so this is a cheap, synchronized prediction, not a
// second gate. Not wired into any caller yet (script_engine.cpp's print_output/on_error/format_lua_stack run on
// every error today, before this gate); this is the API for whoever does that next.
bool XRCORE_API xrSentry_WouldCapture(pcstr message)
{
    if (!s_xr_sentry_started || !message || !*message)
        return false;
    char norm[kGateNormalizedMax];
    std::lock_guard<std::mutex> lock(s_gate_mutex);
    const u64 key = sentry_message_key(message, norm, sizeof(norm));
    return sentry_gate_would_admit_locked(key);
}

#else // !USE_SENTRY

void xrSentry_Initialize(pcstr /*commandLine*/) {}
void xrSentry_Shutdown() {}
void XRCORE_API xrSentry_SaveLocalCrashReport(EXCEPTION_POINTERS* /*exPtrs*/) {}
void XRCORE_API xrSentry_SetLuaStackProvider(xrSentry_LuaStackFn fn) { xrDebug::SetLuaStackProvider(fn); }
void XRCORE_API xrSentry_CaptureSoftError(pcstr /*logger*/, pcstr /*message*/, pcstr /*lua_stack*/) {}
void XRCORE_API xrSentry_CaptureError(pcstr /*logger*/, pcstr /*message*/, pcstr /*lua_stack*/) {}
void XRCORE_API xrSentry_CaptureDebugFail(pcstr /*expr*/, pcstr /*desc*/, pcstr /*arg1*/, pcstr /*arg2*/) {}
void xrSentry_Flush(u32 /*timeoutMs*/) {}
void XRCORE_API xrSentry_CaptureIniRStringError(pcstr /*ini_path*/, pcstr /*section*/, pcstr /*key*/) {}
bool XRCORE_API xrSentry_WouldCapture(pcstr /*message*/) { return false; }
void xrSentry_UpdateHwTags() {}

#endif // USE_SENTRY
