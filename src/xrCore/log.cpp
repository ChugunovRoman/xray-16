#include "stdafx.h"
#pragma hdrstop

#include <time.h>
#include "resource.h"
#include "log.h"
#include "xrCore/Threading/Lock.hpp"

#if defined(XR_PLATFORM_WINDOWS)
#   include "xrCore/Text/Utf8Utils.hpp"
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace
{
// One log file keeps the last few runs, separated by these markers. A run without the "finished"
// marker ended in a crash or a kill, which is visible at a glance when reading a player's log.
constexpr pcstr kSessionStartMarker = "========== Game started: ";
constexpr pcstr kSessionEndMarker = "========== Game finished: ";
constexpr pcstr kSessionMarkerTail = " ==========";

constexpr u32 kDefaultLogHistory = 5;
constexpr u32 kMaxLogHistory = 50;

// History is bounded by the number of runs only. There is deliberately no byte limit: a size cap
// makes the feature unreliable exactly when it matters, because a long session or a chatty build
// produces the biggest and most interesting logs.

void FormatLogTimestamp(pstr buffer, size_t bufferSize)
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto time = system_clock::to_time_t(now);

    // localtime returns null for an out-of-range time_t or broken TZ settings; strftime would
    // then dereference it. The marker is still written, just without a usable timestamp.
    const std::tm* local = std::localtime(&time);
    if (!local || !std::strftime(buffer, bufferSize, "%Y-%m-%d %H:%M:%S", local))
        xr_strcpy(buffer, bufferSize, "unknown");
}

u32 ParseLogHistoryParam()
{
    constexpr pcstr key = "-log_history";
    cpcstr param = strstr(Core.Params, key);
    if (!param)
        return kDefaultLogHistory;

    pcstr value_text = param + xr_strlen(key);
    // Guard against another switch that merely starts the same way.
    if (*value_text && *value_text != ' ' && *value_text != '\t')
        return kDefaultLogHistory;

    while (*value_text == ' ' || *value_text == '\t')
        ++value_text;

    // No number after the key (typo, or the key ends the command line): the user asked for history,
    // so fall back to the default instead of silently disabling it.
    if (*value_text < '0' || *value_text > '9')
        return kDefaultLogHistory;

    const int value = std::atoi(value_text);
    if (value <= 0)
        return 1; // an explicit 0: keep just the current run, as it used to be
    return std::min<u32>(static_cast<u32>(value), kMaxLogHistory);
}

// Not w_printf: IWriter::VPrintf truncates everything past 1023 bytes, and stack trace frames
// or long resource paths go over that.
void WriteLogLine(IWriter* writer, pcstr line)
{
    writer->w(line, xr_strlen(line));
    writer->w("\r\n", 2);
}

// Previous sessions, kept as one raw block so the whole carry-over costs a single allocation.
// The caller writes `text` to the fresh log and releases `block`.
struct LogHistory
{
    void* block{};      // allocation to release, null when there is nothing to carry over
    pcstr text{};       // points inside block
    size_t size{};
    bool needsHeader{}; // previous log predates session markers, so it gets a synthetic one
};

// True when the marker starts a line, which is the only place a real session header can be.
// A script is free to log a line that merely contains the marker text.
bool IsSessionStartAt(pcstr data, size_t size, size_t pos, size_t markerLength)
{
    if (pos + markerLength > size)
        return false;
    if (pos != 0 && data[pos - 1] != '\n')
        return false;
    return std::memcmp(data + pos, kSessionStartMarker, markerLength) == 0;
}

// Reads back the last `keep` sessions of the previous log. CFileWriter can only truncate, so the
// history is carried over by writing it into the fresh file before the new session starts.
void ReadPreviousSessions(pcstr path, u32 keep, LogHistory& out)
{
    out = {};
    if (!keep)
        return;

    // FS paths carry backslashes on every platform, so the separators have to be converted like
    // CFileWriter does before opening. Without this the file is never found on POSIX and the
    // history silently resets on every run. On Windows the conversion is a no-op.
    string_path openPath;
    xr_strcpy(openPath, sizeof(openPath), path);
    convert_path_separators(openPath);

#if defined(XR_PLATFORM_WINDOWS)
    FILE* file = _wfopen(XRay::Utf8::ToWide(openPath).c_str(), L"rb");
#else
    FILE* file = fopen(openPath, "rb");
#endif
    if (!file)
    {
        // First run, a new logs folder, or a path we cannot open. Diagnosing "my history is gone"
        // without this line means guessing, so every branch that drops history says why.
        Msg("~ [log] history: no previous log to carry over [%s]", openPath);
        return;
    }

    // 64-bit offsets: with no size cap the file can outgrow what ftell reports.
#if defined(XR_PLATFORM_WINDOWS)
    _fseeki64(file, 0, SEEK_END);
    const s64 size = _ftelli64(file);
    _fseeki64(file, 0, SEEK_SET);
#else
    fseeko(file, 0, SEEK_END);
    const s64 size = static_cast<s64>(ftello(file));
    fseeko(file, 0, SEEK_SET);
#endif

    if (size <= 0)
    {
        std::fclose(file);
        if (size < 0)
            Msg("~ [log] history: cannot measure previous log [%s]", openPath);
        return; // an empty previous log is normal and needs no message
    }

    // Not a policy limit, just the addressable range: on a 32-bit build size_t cannot hold this.
    if (static_cast<u64>(size) > static_cast<u64>(type_max<size_t>))
    {
        std::fclose(file);
        Msg("~ [log] history: previous log is %lld bytes, too large to load on this build", size);
        return;
    }

    // Deliberately not xr_string: the engine allocator returns null instead of throwing, and
    // std::basic_string does not check that, so a failed allocation would be a null write inside
    // CreateLog. A raw nothrow block is checked here and costs exactly one allocation.
    const size_t byteCount = static_cast<size_t>(size);
    void* block = Memory.mem_alloc(byteCount, std::nothrow);
    if (!block)
    {
        std::fclose(file);
        Msg("~ [log] history: not enough memory for the previous log of %zu bytes", byteCount);
        return;
    }

    const size_t read = std::fread(block, 1, byteCount, file);
    std::fclose(file);

    if (!read)
    {
        Memory.mem_free(block);
        return;
    }

    pcstr data = static_cast<pcstr>(block);
    const size_t markerLength = xr_strlen(kSessionStartMarker);

    // Offset of the oldest session to keep. Scanning backwards stops as soon as enough are found,
    // so a long history is not walked in full.
    size_t offset = 0;
    bool found = false;
    u32 seen = 0;
    for (size_t i = read; i-- > 0;)
    {
        if (!IsSessionStartAt(data, read, i, markerLength))
            continue;

        offset = i;
        found = true;
        if (++seen == keep)
            break;
    }

    out.block = block;
    out.text = data + offset;
    out.size = read - offset;
    out.needsHeader = !found;

    if (found)
        Msg("~ [log] history: carrying over %u previous session(s), %zu bytes", seen, out.size);
    else
        Msg("~ [log] history: previous log has no session markers, carrying it over as one session, %zu bytes",
            out.size);
}

} // namespace

bool LogExecCB = true;
string_path log_file_name{};
bool no_log = true;
#ifdef CONFIG_PROFILE_LOCKS
Lock logCS(MUTEX_PROFILE_ID(log));
#else // CONFIG_PROFILE_LOCKS
Lock logCS;
#endif // CONFIG_PROFILE_LOCKS
xr_vector<xr_string> LogFile;
LogCallback LogCB = nullptr;

bool ForceFlushLog = false;
IWriter* LogWriter = nullptr;

void FlushLog()
{
    if (no_log)
        return;

    ScopeLock scope{ &logCS };
    if (LogWriter)
        LogWriter->flush();
}

void AddOne(pcstr split)
{
    ScopeLock scope{ &logCS };

    OutputDebugString(split);
    OutputDebugString("\n");

    LogFile.push_back(split);

    // exec CallBack
    if (LogExecCB && LogCB)
        LogCB(split);

    if (LogWriter)
    {
        WriteLogLine(LogWriter, split);

        if (ForceFlushLog)
            FlushLog();
    }
}

void Log(pcstr s)
{
    int i, j;

    const u32 length = xr_strlen(s);
    pstr split = static_cast<pstr>(xr_alloca((length + 1) * sizeof(char)));
    for (i = 0, j = 0; s[i] != 0; i++)
    {
        if (s[i] == '\n')
        {
            split[j] = 0; // end of line
            if (split[0] == 0)
            {
                split[0] = ' ';
                split[1] = 0;
            }
            AddOne(split);
            j = 0;
        }
        else
        {
            split[j++] = s[i];
        }
    }
    split[j] = 0;
    AddOne(split);
}

void __cdecl Msg(pcstr format, ...)
{
    va_list mark;
    string2048 buf;
    va_start(mark, format);
    const int sz = std::vsnprintf(buf, sizeof(buf) - 1, format, mark);
    buf[sizeof(buf) - 1] = 0;
    va_end(mark);
    if (sz)
        Log(buf);
}

void Log(pcstr msg, pcstr dop)
{
    if (!dop)
    {
        Log(msg);
        return;
    }

    const u32 buffer_size = (xr_strlen(msg) + 1 + xr_strlen(dop) + 1) * sizeof(char);
    pstr buf = static_cast<pstr>(xr_alloca(buffer_size));
    strconcat(buffer_size, buf, msg, " ", dop);
    Log(buf);
}

void Log(pcstr msg, int dop)
{
    const u32 buffer_size = (xr_strlen(msg) + 1 + 11 + 1) * sizeof(char);
    pstr buf = static_cast<pstr>(xr_alloca(buffer_size));

    xr_sprintf(buf, buffer_size, "%s %i", msg, dop);
    Log(buf);
}

void Log(pcstr msg, unsigned int dop)
{
    const u32 buffer_size = (xr_strlen(msg) + 1 + 10 + 1) * sizeof(char);
    pstr buf = static_cast<pstr>(xr_alloca(buffer_size));

    xr_sprintf(buf, buffer_size, "%s %u", msg, dop);
    Log(buf);
}

void Log(pcstr msg, long dop)
{
    const u32 buffer_size = (xr_strlen(msg) + 1 + 64 + 1) * sizeof(char);
    pstr buf = static_cast<pstr>(xr_alloca(buffer_size));

    xr_sprintf(buf, buffer_size, "%s %li", msg, dop);
    Log(buf);
}

void Log(pcstr msg, unsigned long dop)
{
    const u32 buffer_size = (xr_strlen(msg) + 1 + 64 + 1) * sizeof(char);
    pstr buf = static_cast<pstr>(xr_alloca(buffer_size));

    xr_sprintf(buf, buffer_size, "%s %lu", msg, dop);
    Log(buf);
}

void Log(pcstr msg, long long dop)
{
    const u32 buffer_size = (xr_strlen(msg) + 1 + 64 + 1) * sizeof(char);
    pstr buf = static_cast<pstr>(xr_alloca(buffer_size));

    xr_sprintf(buf, buffer_size, "%s %lli", msg, dop);
    Log(buf);
}

void Log(pcstr msg, unsigned long long dop)
{
    const u32 buffer_size = (xr_strlen(msg) + 1 + 64 + 1) * sizeof(char);
    pstr buf = static_cast<pstr>(xr_alloca(buffer_size));

    xr_sprintf(buf, buffer_size, "%s %llu", msg, dop);
    Log(buf);
}

void Log(pcstr msg, float dop)
{
    // actually, float string representation should be no more, than 40 characters,
    // but we will count with slight overhead
    const u32 buffer_size = (xr_strlen(msg) + 1 + 64 + 1) * sizeof(char);
    pstr buf = static_cast<pstr>(xr_alloca(buffer_size));

    xr_sprintf(buf, buffer_size, "%s %f", msg, dop);
    Log(buf);
}

void Log(pcstr msg, const Fvector& dop)
{
    const u32 buffer_size = (xr_strlen(msg) + 2 + 3 * (64 + 1) + 1) * sizeof(char);
    pstr buf = static_cast<pstr>(xr_alloca(buffer_size));

    xr_sprintf(buf, buffer_size, "%s (%f,%f,%f)", msg, VPUSH(dop));
    Log(buf);
}

void Log(pcstr msg, const Fmatrix& dop)
{
    const u32 buffer_size = (xr_strlen(msg) + 2 + 4 * (4 * (64 + 1) + 1) + 1) * sizeof(char);
    pstr buf = static_cast<pstr>(xr_alloca(buffer_size));

    xr_sprintf(buf, buffer_size, "%s:\n%f,%f,%f,%f\n%f,%f,%f,%f\n%f,%f,%f,%f\n%f,%f,%f,%f\n", msg, dop.i.x, dop.i.y,
        dop.i.z, dop._14_, dop.j.x, dop.j.y, dop.j.z, dop._24_, dop.k.x, dop.k.y, dop.k.z, dop._34_, dop.c.x, dop.c.y,
        dop.c.z, dop._44_);
    Log(buf);
}

void LogWinErr(pcstr msg, long err_code) { Msg("%s: %s", msg, xrDebug::ErrorToString(err_code)); }
LogCallback SetLogCB(const LogCallback& cb)
{
    ScopeLock scope{ &logCS };
    const LogCallback result = LogCB;
    LogCB = cb;
    return (result);
}

pcstr log_name() { return (log_file_name); }

void CreateLog(bool nl)
{
    ZoneScoped;
    LogFile.reserve(1000);

    no_log = nl;

    const bool unique_logs = strstr(Core.Params, "-unique_logs");

    if (unique_logs)
    {
        string32 TimeBuf;
        using namespace std::chrono;
        const auto now = system_clock::now();
        const auto time = system_clock::to_time_t(now);
        std::strftime(TimeBuf, sizeof(TimeBuf), "%d-%m-%y_%H-%M-%S", std::localtime(&time));
        strconcat(log_file_name, Core.ApplicationName, "_", Core.UserName, "_", TimeBuf, ".log");
    }
    else
    {
        strconcat(log_file_name, Core.ApplicationName, "_", Core.UserName, ".log");
    }

    if (FS.path_exist("$logs$"))
        FS.update_path(log_file_name, "$logs$", log_file_name);

    if (no_log)
        return;

    // With -unique_logs every run already has its own file, so there is nothing to carry over.
    LogHistory history;
    if (!unique_logs)
    {
        const u32 keep = ParseLogHistoryParam();
        if (keep > 1)
            ReadPreviousSessions(log_file_name, keep - 1, history);
    }

    if (const auto w = FS.w_open(log_file_name))
    {
        // Held across the whole handover: another thread logging while LogFile is being flushed
        // could reallocate the vector under the loop below, and anything appended after the loop
        // passed its index would be lost, because LogWriter is not published yet.
        ScopeLock scope{ &logCS };

        if (history.text && history.size)
        {
            if (history.needsHeader)
                w->w_printf("%s%s%s\r\n", kSessionStartMarker, "unknown", kSessionMarkerTail);

            w->w(history.text, history.size);

            if (history.text[history.size - 1] != '\n')
                w->w("\r\n", 2);
            w->w("\r\n", 2); // blank line between runs
        }

        string64 timestamp;
        FormatLogTimestamp(timestamp, sizeof(timestamp));
        w->w_printf("%s%s%s\r\n", kSessionStartMarker, timestamp, kSessionMarkerTail);

        for (u32 it = 0; it < LogFile.size(); it++)
        {
            cpcstr s = LogFile[it].c_str();
            WriteLogLine(w, s ? s : "");
        }
        w->flush();

        LogWriter = w;
    }

    if (history.block)
        Memory.mem_free(history.block);

    if (strstr(Core.Params, "-force_flushlog"))
        ForceFlushLog = true;
}

void CloseLog(void)
{
    ZoneScoped;
    FlushLog();

    ScopeLock scope{ &logCS };
    if (LogWriter)
    {
        // Absence of this marker in the next run's log is what tells a crash from a clean exit.
        string64 timestamp;
        FormatLogTimestamp(timestamp, sizeof(timestamp));
        LogWriter->w_printf("%s%s%s\r\n", kSessionEndMarker, timestamp, kSessionMarkerTail);
        FS.w_close(LogWriter);
    }

    LogFile.clear();
}
