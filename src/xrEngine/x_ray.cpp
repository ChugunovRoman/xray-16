//-----------------------------------------------------------------------------
// File: x_ray.cpp
//
// Programmers:
// Oles - Oles Shishkovtsov
// AlexMX - Alexander Maksimchuk
//-----------------------------------------------------------------------------
#include "stdafx.h"

#include "x_ray.h"

#include "embedded_resources_management.h"
#include "EngineThreading.h"

#include "xrCore/Profiling/PerfMetrics.hpp"
#include "xrCore/Profiling/StartupMarks.hpp"
#include "xrCore/Threading/TaskManager.hpp"
#include "xrNetServer/NET_AuthCheck.h"

#include "IGame_Persistent.h"
#include "LightAnimLibrary.h"
#include "XR_IOConsole.h"

#if defined(XR_PLATFORM_WINDOWS)
#include "AccessibilityShortcuts.hpp"
#include "Text_Console.h"
#else
#define CTextConsole CConsole
#pragma todo("Implement text console or it's alternative")
#endif

#ifdef XR_PLATFORM_WINDOWS
#include "DiscordGameSDK/discord.h"
#define USE_DISCORD_INTEGRATION

#include "xrCore/Text/Utf8Utils.hpp"
#endif

// global variables
constexpr size_t MAX_WINDOW_EVENTS = 32;

#ifdef USE_DISCORD_INTEGRATION
constexpr discord::ClientId DISCORD_APP_ID = 421286728695939072;
#endif

ENGINE_API CInifile* pGameIni = nullptr;
ENGINE_API bool CallOfPripyatMode = false;
ENGINE_API bool ClearSkyMode = false;
ENGINE_API bool ShadowOfChernobylMode = false;

ENGINE_API bool UiDebuggerEnabled = false;

ENGINE_API string512 g_sLaunchOnExit_params;
ENGINE_API string512 g_sLaunchOnExit_app;
ENGINE_API string_path g_sLaunchWorkingFolder;

namespace
{
struct PathIncludePred
{
private:
    const xr_auth_strings_t* ignored;

public:
    explicit PathIncludePred(const xr_auth_strings_t* ignoredPaths) : ignored(ignoredPaths) {}
    bool IsIncluded(pcstr path)
    {
        if (!ignored)
            return true;

        return allow_to_include_path(*ignored, path);
    }
};
}

template <typename T>
void InitConfig(T& config, pcstr name, bool fatal = true,
    bool readOnly = true, bool loadAtStart = true, bool saveAtEnd = true,
    u32 sectCount = 0, const CInifile::allow_include_func_t& allowIncludeFunc = nullptr)
{
    string_path fname;
    FS.update_path(fname, "$game_config$", name);
    config = xr_new<CInifile>(fname, readOnly, loadAtStart, saveAtEnd, sectCount, allowIncludeFunc);

    CHECK_OR_EXIT(config->section_count() || !fatal,
        make_string("Cannot find file %s.\nReinstalling application may fix this problem.", fname));
}

// XXX: make it more fancy
// некрасиво слишком
void set_shoc_mode()
{
    CallOfPripyatMode = false;
    ShadowOfChernobylMode = true;
    ClearSkyMode = false;
}

void set_cs_mode()
{
    CallOfPripyatMode = false;
    ShadowOfChernobylMode = false;
    ClearSkyMode = true;
}

void set_cop_mode()
{
    CallOfPripyatMode = true;
    ShadowOfChernobylMode = false;
    ClearSkyMode = false;
}

void set_free_mode()
{
    CallOfPripyatMode = false;
    ShadowOfChernobylMode = false;
    ClearSkyMode = false;
}

// system.ltx without the files fill_auth_check_params leaves out of the multiplayer auth check (text, ui, fonts...)
static void LoadSettingsAuth()
{
    ZoneScoped;

    xr_auth_strings_t ignoredPaths, checkedPaths;
    fill_auth_check_params(ignoredPaths, checkedPaths); //TODO port xrNetServer to Linux
    PathIncludePred includePred(&ignoredPaths);
    CInifile::allow_include_func_t includeFilter;
    includeFilter.bind(&includePred, &PathIncludePred::IsIncluded);

    InitConfig(pSettingsAuth, "system.ltx", true, true, true, false, 0, includeFilter);
}

void InitSettings()
{
    ZoneScoped;

    InitConfig(pSettings, "system.ltx");
    // 50z1 (plans/optimization_mt/50z1-startup-markers-fs-index.md): pSettingsAuth is used only by the multiplayer
    // auth check (CLocatorAPI::auth_runtime), which parses it on its first use. Before, every start parsed the whole
    // system.ltx tree a second time (~660 files) for it. Its content differs from pSettings (fonts.ltx,
    // localization.ltx... are filtered out), so it is not a copy of pSettings.
    pSettingsAuthLoader = &LoadSettingsAuth;
    InitConfig(pSettingsOpenXRay, "openxray.ltx", false, true, true, false);
    InitConfig(pSettingsFE, "faction_editor_config.ltx", true, true, true, false);
    InitConfig(pGameIni, "game.ltx");

    if (strstr(Core.Params, "-shoc") || strstr(Core.Params, "-soc"))
        set_shoc_mode();
    else if (strstr(Core.Params, "-cs"))
        set_cs_mode();
    else if (strstr(Core.Params, "-cop"))
        set_cop_mode();
    else if (strstr(Core.Params, "-unlock_game_mode"))
        set_free_mode();
    else
    {
        pcstr gameMode = READ_IF_EXISTS(pSettingsOpenXRay, r_string, "compatibility", "game_mode", "cop");
        if (xr_strcmpi("cop", gameMode) == 0)
            set_cop_mode();
        else if (xr_strcmpi("cs", gameMode) == 0)
            set_cs_mode();
        else if (xr_strcmpi("shoc", gameMode) == 0 || xr_strcmpi("soc", gameMode) == 0)
            set_shoc_mode();
        else if (xr_strcmpi("unlock", gameMode) == 0)
            set_free_mode();
    }

    if (strstr(Core.Params, "-uidbg"))
        UiDebuggerEnabled = true;

    Dbg.InitSectionLists();
}

void InitConsole()
{
    ZoneScoped;

    if (GEnv.isDedicatedServer)
        Console = xr_new<CTextConsole>();
    else
        Console = xr_new<CConsole>();

    Console->Initialize();
    xr_strcpy(Console->ConfigFile, "user.ltx");
    if (strstr(Core.Params, "-ltx "))
    {
        string64 c_name;
        sscanf(strstr(Core.Params, "-ltx ") + strlen("-ltx "), "%[^ ] ", c_name);
        xr_strcpy(Console->ConfigFile, c_name);
    }
}

void destroySettings()
{
    ZoneScoped;
    auto s = const_cast<CInifile**>(&pSettings);
    xr_delete(*s);

    auto sa = const_cast<CInifile**>(&pSettingsAuth);
    xr_delete(*sa);
    pSettingsAuthLoader = nullptr;

    auto so = const_cast<CInifile**>(&pSettingsOpenXRay);
    xr_delete(*so);

    xr_delete(pGameIni);
}

void destroyConsole()
{
    ZoneScoped;
    Console->Execute("cfg_save");
    Console->Destroy();
    xr_delete(Console);
}

// What the user config says about the quality preset (plans/optimization_mt/50d-stage2-auto-engine.md, R22)
enum class EUserCfgPreset
{
    NoFile, // no user config at all: the first start, a deleted user.ltx
    NoLine, // the file exists, but has no "_preset" line: the engine never saved it (the launcher creates user.ltx
            // with its own lines only - vid_mode, renderer, fov...); cfg_save writes "_preset" into every user.ltx
    HasLine,
};

// Looks the config up as cfg_load does (CCC_LoadCFG::Execute): appdata, then fs root, then the name as it is. Quiet:
// cfg_load itself logs the missing file right after.
EUserCfgPreset XRayUserConfigPresetState(pcstr config)
{
    string_path cfg_name;
    xr_strcpy(cfg_name, config);
    if (strext(cfg_name))
        *strext(cfg_name) = 0;
    xr_strcat(cfg_name, ".ltx");

    string_path cfg_full_name;
    FS.update_path(cfg_full_name, "$app_data_root$", cfg_name);
    if (!FS.exist(cfg_full_name))
        FS.update_path(cfg_full_name, "$fs_root$", cfg_name);
    if (!FS.exist(cfg_full_name))
        xr_strcpy(cfg_full_name, cfg_name);
    if (!FS.exist(cfg_full_name))
        return EUserCfgPreset::NoFile;

    IReader* F = FS.r_open(cfg_full_name);
    if (!F)
        return EUserCfgPreset::NoFile;
    constexpr pcstr preset_cmd = "_preset";
    const size_t preset_len = xr_strlen(preset_cmd);
    bool found = false;
    string1024 str;
    while (!found && !F->eof())
    {
        F->r_string(str, sizeof(str));
        pcstr s = str;
        while (*s == ' ' || *s == '\t')
            ++s;
        found = !strncmp(s, preset_cmd, preset_len) && (s[preset_len] == ' ' || s[preset_len] == '\t');
    }
    FS.r_close(F);
    return found ? EUserCfgPreset::HasLine : EUserCfgPreset::NoLine;
}

void execUserScript()
{
    ZoneScoped;
    Console->Execute("default_controls");
    // R22 (50d-stage2-auto-engine.md): a config without "_preset" never got a quality preset - no rspec_*.ltx, and an
    // integrated GPU kept the full-size main pass (no r__render_scale 0.75). "_preset Auto" runs first, as the first
    // line of a saved user.ltx does ("_preset Auto" sorts before the other commands), so every line of the config
    // still wins over the preset. Here the hardware policy is valid (stages A/B in xrCore, the GPU of the startup
    // probe of the renderer list), the renderer and its console commands are set up, the device is not created yet.
    // The normal cfg_save at exit writes "_preset Auto" with the values: the next start keeps the player's values.
    if (!GEnv.isDedicatedServer && Console->GetCommand("_preset"))
    {
        const EUserCfgPreset state = XRayUserConfigPresetState(Console->ConfigFile);
        if (state != EUserCfgPreset::HasLine)
        {
            Msg("* [hw] first run (%s %s): _preset Auto", Console->ConfigFile,
                state == EUserCfgPreset::NoFile ? "is missing" : "has no _preset line");
            Console->Execute("_preset Auto");
        }
    }
    Console->ExecuteScript(Console->ConfigFile);
}

constexpr pcstr FRAME_MARK_APPLICATION_STARTUP = "Application startup";
constexpr pcstr FRAME_MARK_APPLICATION_SHUTDOWN = "Application shutdown";
constexpr pcstr FRAME_MARK_APPLICATION_RUN = "Application run";

CApplication::CApplication(pcstr commandLine, GameModule* game, const std::array<RendererModule*, 2>& modules)
{
    TracySetProgramName("OpenXRay");
    Threading::SetCurrentThreadName("Primary thread");
    XRay::Engine::MarkMainThread(); // the game logic thread of IsGameLogicThread() (again every frame, see Run)
    FrameMarkStart(FRAME_MARK_APPLICATION_STARTUP);

    if (strstr(commandLine, "-dedicated"))
        GEnv.isDedicatedServer = true;

    xrDebug::Initialize(commandLine);
    {
        ZoneScopedN("SDL_Init");
        u32 flags = SDL_INIT_VIDEO;
        if (!strstr(commandLine, "-no_gamepad"))
            flags |= SDL_INIT_GAMECONTROLLER;
        R_ASSERT3(SDL_Init(flags) == 0, "Unable to initialize SDL", SDL_GetError());
    }

#ifdef XR_PLATFORM_WINDOWS
    AccessibilityShortcuts shortcuts;
    if (!GEnv.isDedicatedServer)
        shortcuts.Disable();
#endif

    if (!strstr(commandLine, "-nosplash"))
    {
        const bool topmost = !strstr(commandLine, "-splashnotop");
        ShowSplash(topmost);
    }

    SDL_StopTextInput(); // It's enabled by default for some reason, we don't want it
    const auto& inputTask = TaskManager::AddTask([]
    {
        const bool captureInput = !strstr(Core.Params, "-i");
        pInput = xr_new<CInput>(captureInput);
    });

    const auto& createSoundDevicesList = TaskManager::AddTask([]
    {
        Engine.Sound.CreateDevicesList();
    });

    pcstr fsltx = "-fsltx ";
    string_path fsgame = "";
    if (strstr(commandLine, fsltx))
    {
        const size_t sz = xr_strlen(fsltx);
        sscanf(strstr(commandLine, fsltx) + sz, "%[^ ] ", fsgame);
    }

    Core.Initialize("OpenXRay", commandLine, true, *fsgame ? fsgame : nullptr);
    xray::StartupMark("core"); // 50z1: the rest of xrCore::Initialize after the log (hardware profile, disk probe)
    // plans/optimization_mt 50g, R15: main never on the efficiency cores / at a throttled clock (the log is up now)
    Threading::SetCurrentThreadHighQos();

    InitSettings();
    xray::StartupMark("settings"); // 50z1: system.ltx with its includes, openxray.ltx, faction_editor_config, game.ltx
    // Adjust player & computer name for Asian
    if (pSettings->line_exist("string_table", "no_native_input"))
    {
        xr_strcpy(Core.UserName, sizeof(Core.UserName), "Player");
        xr_strcpy(Core.CompName, sizeof(Core.CompName), "Computer");
    }

    Device.InitializeImGui();
    Device.FillVideoModes();
    TaskScheduler->Wait(inputTask);
    InitConsole();
    xray::StartupMark("console"); // 50z1: ImGui, video modes, input, console

    Engine.Initialize(game, modules);
    Device.Initialize();

    Console->OnDeviceInitialize();

    execUserScript();
    InitializeDiscord();
    xray::StartupMark("device_init_user_ltx"); // 50z1: Device.Initialize (window), user.ltx, Discord

    TaskScheduler->Wait(createSoundDevicesList);
    Engine.Sound.Create();
    xray::StartupMark("sound_create"); // 50z1

    // ...command line for auto start
    pcstr startArgs = strstr(Core.Params, "-start ");
    if (startArgs)
        Console->Execute(startArgs + 1);
    pcstr loadArgs = strstr(Core.Params, "-load ");
    if (loadArgs)
        Console->Execute(loadArgs + 1);

    // Initialize APP
    const auto& createLightAnim = TaskScheduler->AddTask([]
    {
        LALib.OnCreate();
    });

    Device.Create();
    TaskScheduler->Wait(createLightAnim);
    xray::StartupMark("device_create_done"); // 50z1: the rest of Device.Create (marks device_* inside)

    if (game)
    {
        m_game_module = game;
        g_pGamePersistent = game->create_persistent();
        R_ASSERT(g_pGamePersistent);
    }
    if (g_pGamePersistent)
        g_pGamePersistent->OnAppStart();
    else
        Console->Show();
    xray::StartupMark("app_start_done"); // 50z1: the rest of OnAppStart (marks app_* inside)

    FrameMarkEnd(FRAME_MARK_APPLICATION_STARTUP);
}

CApplication::~CApplication()
{
    FrameMarkStart(FRAME_MARK_APPLICATION_SHUTDOWN);

    if (g_pGamePersistent)
        g_pGamePersistent->OnAppEnd();

    if (m_game_module)
        m_game_module->destroy_persistent(g_pGamePersistent);

    // Discord SDK must shut down before graphics/engine teardown; late destroy can hit 0xC0000008 (invalid handle)
    // inside discord_game_sdk when pipes/handles are already torn down with the device/window stack.
    ShutdownDiscord();

    Engine.Event.Dump();

    xr_delete(pInput);
    destroySettings();

    LALib.OnDestroy();

    destroyConsole();

    Device.CleanupVideoModes();
    Device.DestroyImGui();
    Engine.Sound.Destroy();

    Device.Destroy();
    Engine.Destroy();

    // check for need to execute something external
    if (/*xr_strlen(g_sLaunchOnExit_params) && */ xr_strlen(g_sLaunchOnExit_app))
    {
#if defined(XR_PLATFORM_WINDOWS)
        // CreateProcess need to return results to next two structures
        STARTUPINFO si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        // We use CreateProcess to setup working folder
        pcstr tempDir = xr_strlen(g_sLaunchWorkingFolder) ? g_sLaunchWorkingFolder : nullptr;
        CreateProcess(g_sLaunchOnExit_app, g_sLaunchOnExit_params, nullptr, nullptr, FALSE, 0, nullptr, tempDir, &si, &pi);
#endif
    }

    // xrDebug::Finalize() must run while SDL is still initialized (it clears
    // SDL_SetAssertionHandler) and before Core._destroy() so Sentry (started after
    // render device create) can shut down cleanly before xrMemory and other
    // subsystems are torn down.
    xrDebug::Finalize();
    Core._destroy();
    {
        ZoneScopedN("SDL_Quit");
        SDL_Quit();
    }

    FrameMarkEnd(FRAME_MARK_APPLICATION_SHUTDOWN);
}

int CApplication::Run()
{
    HideSplash();
    xray::StartupMark("splash_hidden"); // 50z1
    Device.Run();

    while (!SDL_QuitRequested()) // SDL_PumpEvents is here
    {
        FrameMarkStart(FRAME_MARK_APPLICATION_RUN);
        XRay::Engine::MarkMainThread();
        bool canCallActivate = false;
        bool shouldActivate = false;

        SDL_Event events[MAX_WINDOW_EVENTS];
        const int count = SDL_PeepEvents(events, MAX_WINDOW_EVENTS,
            SDL_GETEVENT, SDL_WINDOWEVENT, SDL_WINDOWEVENT);

        for (int i = 0; i < count; ++i)
        {
            const SDL_Event event = events[i];

            switch (event.type)
            {
            case SDL_WINDOWEVENT:
            {
                const auto window = SDL_GetWindowFromID(event.window.windowID);

                switch (event.window.event)
                {
                case SDL_WINDOWEVENT_SHOWN:
                case SDL_WINDOWEVENT_FOCUS_GAINED:
                case SDL_WINDOWEVENT_RESTORED:
                case SDL_WINDOWEVENT_MAXIMIZED:
                    if (window != Device.m_sdlWnd)
                        Device.OnWindowActivate(window, true);
                    else
                    {
                        canCallActivate = true;
                        shouldActivate = true;
                    }
                    continue;

                case SDL_WINDOWEVENT_HIDDEN:
                case SDL_WINDOWEVENT_FOCUS_LOST:
                case SDL_WINDOWEVENT_MINIMIZED:
                    if (window != Device.m_sdlWnd)
                        Device.OnWindowActivate(window, false);
                    else
                    {
                        canCallActivate = true;
                        shouldActivate = false;
                    }
                    continue;
                } // switch (event.window.event)
            }
            } // switch (event.type)

            // Only process event in Device
            // if it wasn't processed in the switch above
            Device.ProcessEvent(event);
        } // for (int i = 0; i < count; ++i)

        // Workaround for screen blinking when there's too much timeouts
        if (canCallActivate)
        {
            Device.OnWindowActivate(Device.m_sdlWnd, shouldActivate);
        }

        Device.ProcessFrame();

        ::xray::perf::Flush();

        UpdateDiscordStatus();
        FrameMarkEnd(FRAME_MARK_APPLICATION_RUN);
    } // while (!SDL_QuitRequested())

    Device.Shutdown();

    return 0;
}

void CApplication::ShowSplash(bool topmost)
{
    if (m_window)
        return;

    ZoneScoped;

    m_surface = std::move(ExtractSplashScreen());
    if (!m_surface)
    {
        Log("~ Couldn't create surface from image:", SDL_GetError());
        return;
    }

    Uint32 flags = SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIDDEN;

    if (topmost)
        flags |= SDL_WINDOW_ALWAYS_ON_TOP;

    m_window = SDL_CreateWindow("OpenXRay", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, m_surface->w, m_surface->h, flags);
    SDL_ShowWindow(m_window);

    m_splash_thread = Threading::RunThread("Splash Thread", &CApplication::SplashProc, this);
    SDL_PumpEvents();
}

constexpr u32 SPLASH_FRAMERATE = 30;

void CApplication::SplashProc()
{
    {
        ZoneScopedN("Update splash image");
        const auto current = SDL_GetWindowSurface(m_window);
        SDL_BlitSurface(m_surface, nullptr, current, nullptr);
        SDL_UpdateWindowSurface(m_window);
    }
    while (!m_should_exit.load(std::memory_order_acquire))
    {
        UpdateDiscordStatus();
        Sleep(SPLASH_FRAMERATE);
    }
}

void CApplication::HideSplash()
{
    if (!m_window)
        return;

    ZoneScoped;

    m_should_exit.store(true, std::memory_order_release);
    m_splash_thread.join();

    SDL_DestroyWindow(m_window);
    m_window = nullptr;

    SDL_FreeSurface(m_surface);
}

void CApplication::InitializeDiscord()
{
#ifdef USE_DISCORD_INTEGRATION
    ZoneScoped;
    discord::Core* core;
    discord::Core::Create(DISCORD_APP_ID, discord::CreateFlags::NoRequireDiscord, &core);

#   ifndef MASTER_GOLD
    if (core)
    {
        const auto level = xrDebug::DebuggerIsPresent() ? discord::LogLevel::Debug : discord::LogLevel::Info;
        core->SetLogHook(level, [](discord::LogLevel level, pcstr message)
        {
            switch (level)
            {
            case discord::LogLevel::Error: Log("!", message); break;
            case discord::LogLevel::Warn:  Log("~", message); break;
            case discord::LogLevel::Info:  Log("*", message); break;
            case discord::LogLevel::Debug: Log("#", message); break;
            }
        });
    }
#   endif

    if (core)
    {
        discord::Activity activity{};
        activity.SetType(discord::ActivityType::Playing);
        activity.SetApplicationId(DISCORD_APP_ID);
        activity.SetState(XRay::Utf8::FromACP(Core.ApplicationTitle).c_str());
        activity.GetAssets().SetLargeImage("logo");
        core->ActivityManager().UpdateActivity(activity, nullptr);

        std::lock_guard guard{ m_discord_lock };
        m_discord_core = core;
    }
#endif
}

void CApplication::UpdateDiscordStatus()
{
#ifdef USE_DISCORD_INTEGRATION
    if (!m_discord_core)
        return;

    static u32 s_discord_frame = 0;
    ++s_discord_frame;
    // Throttle main-loop callbacks (Discord SDK is not proven thread-safe for RunCallbacks off-main).
    // Splash window uses a shorter interval while m_window is set.
    const u32 interval = m_window ? 2u : 32u;
    if ((s_discord_frame % interval) != 0u)
        return;

    ZoneScoped;
    std::lock_guard guard{ m_discord_lock };
    m_discord_core->RunCallbacks();
#endif
}

void CApplication::ShutdownDiscord()
{
#ifdef USE_DISCORD_INTEGRATION
    discord::Core* core{};
    {
        std::lock_guard guard{ m_discord_lock };
        core = m_discord_core;
        m_discord_core = nullptr;
    }
    if (!core)
        return;

    core->ActivityManager().ClearActivity(nullptr);
    for (int i = 0; i < 32; ++i)
        (void)core->RunCallbacks();

    discord::Core::Destroy(&core);
#endif
}
