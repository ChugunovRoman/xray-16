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
#include <filesystem>
#include <vector>

#include "LocatorAPI.h"

#if defined(XR_PLATFORM_WINDOWS)
#include <Windows.h>
#include <dbghelp.h>
#endif

#if __has_include(".GitInfo.hpp")
#include ".GitInfo.hpp"
#endif

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

// Live process snapshot for xrDebug::Fail (not a crash). sentry_capture_minidump() reads the file into the envelope synchronously.
bool write_live_minidump_to_temp(char* pathOut, size_t pathOutSize)
{
    if (!pathOut || pathOutSize < MAX_PATH)
        return false;

    char tempDir[MAX_PATH]{};
    if (!GetTempPathA(static_cast<DWORD>(sizeof(tempDir)), tempDir))
        return false;
    if (!GetTempFileNameA(tempDir, "xrdf", 0, pathOut))
        return false;

    const HANDLE hFile = CreateFileA(pathOut, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        DeleteFileA(pathOut);
        pathOut[0] = '\0';
        return false;
    }

    HMODULE dbghelpMod = LoadLibraryA("dbghelp.dll");
    if (!dbghelpMod)
    {
        CloseHandle(hFile);
        DeleteFileA(pathOut);
        pathOut[0] = '\0';
        return false;
    }

    using MiniDumpWriteDump_t = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
        PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    auto const pfn = reinterpret_cast<MiniDumpWriteDump_t>(GetProcAddress(dbghelpMod, "MiniDumpWriteDump"));
    if (!pfn)
    {
        FreeLibrary(dbghelpMod);
        CloseHandle(hFile);
        DeleteFileA(pathOut);
        pathOut[0] = '\0';
        return false;
    }

    const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory |
        MiniDumpWithProcessThreadData | MiniDumpWithThreadInfo);

    const BOOL ok = pfn(GetCurrentProcess(), GetCurrentProcessId(), hFile, dumpType, nullptr, nullptr, nullptr);
    FreeLibrary(dbghelpMod);
    CloseHandle(hFile);
    if (!ok)
    {
        DeleteFileA(pathOut);
        pathOut[0] = '\0';
        return false;
    }
    return true;
}
#endif
} // namespace

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
        return;
    }
    s_xr_sentry_started = true;

    if (commandLine && std::strstr(commandLine, "-sentry_test_av_crash"))
    {
        volatile int* p = nullptr;
        *p = 42;
    }
}

void xrSentry_Shutdown()
{
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
        DeleteFileA(dumpPath);
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
