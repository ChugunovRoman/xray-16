// Engine.cpp: implementation of the CEngine class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "Engine.h"

#include "XR_IOConsole.h"
#include "xr_ioc_cmd.h"
#include "EngineThreading.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "xrCore/Profiling/StartupMarks.hpp"

struct SoundProcessor final : public pureFrame
{
    void OnFrame() override
    {
        GEnv.Sound->update(Device.vCameraPosition, Device.vCameraDirection, Device.vCameraTop, Device.vCameraRight);
    }
} g_sound_processor;

struct SoundRenderer final : public pureFrame
{
    void OnFrame() override
    {
        // K9 (26-framemove-game-next.md): the Game thread's task rendered this frame already
        if (XRay::Engine::SoundRenderTakenByTask())
            return;
        PERF_SCOPE(game_seq_frame_mt_sound); // 26-framemove-game-next.md: game/seq_frame_mt/*
        GEnv.Sound->render();
    }
} g_sound_renderer;

CEngine Engine;

CEngine::CEngine() {}
CEngine::~CEngine() {}

// The renderer forced by the command line (or the dedicated server), nullptr - none
pcstr CommandLineRenderer()
{
    if (GEnv.isDedicatedServer)
        return "renderer_r1";

    if (strstr(Core.Params, "-rgl"))
        return "renderer_rgl";
    if (strstr(Core.Params, "-r4"))
        return "renderer_r4";
    if (strstr(Core.Params, "-r3"))
        return "renderer_r3";
    if (strstr(Core.Params, "-r2.5"))
        return "renderer_r2.5";
    if (strstr(Core.Params, "-r2a"))
        return "renderer_r2a";
    if (strstr(Core.Params, "-r2"))
        return "renderer_r2";
    if (strstr(Core.Params, "-r1"))
        return "renderer_r1";
    return nullptr;
}

// 50z2 (plans/optimization_mt/50z2-startup-thm-shaders-probes-preview.md): the "renderer" line of user.ltx is needed
// before the renderer list exists (it decides which renderer modules are probed), but the console command checks its
// value against that list. So the config is read here only to keep the first such line; CheckAndSetupRenderer executes
// it once the list is built (the later "renderer" lines of the file did nothing: the first one locks the command).
class CCC_PeekRendererCfg final : public CCC_LoadCFG_custom
{
public:
    string1024 line{};

    CCC_PeekRendererCfg() : CCC_LoadCFG_custom("renderer ") {}

    bool allow(pcstr cmd) override
    {
        if (!line[0] && CCC_LoadCFG_custom::allow(cmd))
            xr_strcpy(line, cmd);
        return false;
    }
};

void CheckAndSetupRenderer(pcstr user_line)
{
    if (pcstr mode = CommandLineRenderer())
    {
        string64 buf;
        xr_sprintf(buf, "renderer %s", mode);
        Console->Execute(buf);
        return;
    }

    if (user_line[0])
        Console->Execute(user_line);
    renderer_allow_override = true;
}

extern void msCreate(pcstr name);

void CEngine::Initialize(GameModule* game, const std::array<RendererModule*, 2>& modules)
{
    ZoneScoped;
#ifdef DEBUG
    msCreate("game");
#endif

    eQuit = Event.Handler_Attach("KERNEL:quit", this);

    Device.seqFrame.Add(this, REG_PRIORITY_HIGH + 1000);

    Device.seqFrame.Add(&g_sound_processor, REG_PRIORITY_NORMAL - 1000); // Place it after Level update
    Device.seqFrameMT.Add(&g_sound_renderer);

    // 50z2: the renderer asked for (the command line, else the user.ltx line) - the list probes only what it needs
    string1024 user_line{};
    string64 requested{};
    if (pcstr mode = CommandLineRenderer())
        xr_strcpy(requested, mode);
    else
    {
        CCC_PeekRendererCfg peek;
        peek.Execute(Console->ConfigFile);
        xr_strcpy(user_line, peek.line);
        // "renderer <mode>" as the console splits it: the first word after the command
        pcstr value = user_line[0] ? user_line + xr_strlen("renderer") : user_line;
        while (*value == ' ' || *value == '\t')
            ++value;
        size_t n = 0;
        while (value[n] && value[n] != ' ' && value[n] != '\t' && value[n] != '\r' && value[n] != '\n' &&
            n + 1 < sizeof(requested))
        {
            requested[n] = value[n];
            ++n;
        }
        requested[n] = 0;
    }
    External.CreateRendererList(modules, requested);
    CheckAndSetupRenderer(user_line);
    xray::StartupMark("renderer_list"); // 50z1: renderer probes (D3D11 / GL test devices), user.ltx "renderer"

    External.Initialize(game);
    xray::StartupMark("game_module"); // 50z1: xrGame loaded and initialized (string table...)
    Sheduler.Initialize();
}

void CEngine::Destroy()
{
    ZoneScoped;

    Sheduler.Destroy();
    External.Destroy();
    Event._destroy();

    Event.Handler_Detach(eQuit, this);

    Device.seqFrameMT.Remove(&g_sound_processor);
    Device.seqFrame.Remove(&g_sound_processor);
    Device.seqFrame.Remove(this);
}

void CEngine::OnEvent(EVENT E, u64 P1, u64 P2)
{
    if (E == eQuit)
    {
        if (pInput != nullptr)
            pInput->GrabInput(false);

        SDL_Event quit = { SDL_QUIT };
        SDL_PushEvent(&quit);
    }
}

void CEngine::OnFrame()
{
    Event.OnFrame();
}
