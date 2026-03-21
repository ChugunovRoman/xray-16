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

#include <cstdlib>
#include <cstring>
#include <filesystem>

#if defined(XR_PLATFORM_WINDOWS)
#include <Windows.h>
#endif

#if __has_include(".GitInfo.hpp")
#include ".GitInfo.hpp"
#endif

namespace
{
bool s_xr_sentry_started = false;

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

#else // !USE_SENTRY

void xrSentry_Initialize(pcstr /*commandLine*/) {}
void xrSentry_Shutdown() {}

#endif // USE_SENTRY
