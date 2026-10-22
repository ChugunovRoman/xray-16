#include "stdafx.h"
#include "Include/xrRender/DrawUtils.h"
#include "ParticleWorker.h"
#include "Render.h"
#include "XR_IOConsole.h"
#include "xrCDB/xrXRC.h"
#include "xrCore/Profiling/StartupMarks.hpp"

void CRenderDevice::SetupStates()
{
    // General Render States
    mView.identity();
    mProject.identity();
    mFullTransform.identity();
    vCameraPosition.set(0, 0, 0);
    vCameraDirection.set(0, 0, 1);
    vCameraTop.set(0, 1, 0);
    vCameraRight.set(1, 0, 0);
    GEnv.Render->SetupStates();
}

void CRenderDevice::Create()
{
    if (b_is_Ready)
        return; // prevent double call

    ZoneScoped;

    Statistic = xr_new<CStats>();
    Log("Starting RENDER device...");
#ifdef _EDITOR
    psDeviceMode.Width = dwWidth;
    psDeviceMode.Height = dwHeight;
#endif
    fFOV = 90.f;
    fASPECT = 1.f;

    if (GEnv.isDedicatedServer)
        psDeviceMode.WindowStyle = rsWindowed;

    UpdateWindowProps();
    GEnv.Render->Create(m_sdlWnd, dwWidth, dwHeight, fWidth_2, fHeight_2);
    xray::StartupMark("device_render_create"); // 50z1: the real D3D / GL device, swap chain

    // 50d-stage2-auto-engine.md (plans/optimization_mt): CHW::CreateDevice has given the GPU of the real device to the
    // hardware policy (stage C), the "auto" quality settings take their final values now - before OnDeviceCreate below
    // creates the render targets (r2_smap_size) and loads the first textures (texture_lod)
    if (Console)
        Console->ResolveAutoCommands(true);

    // Sentry (Crashpad) after GPU/device setup — avoids driver init races on some systems.
    xrDebug::InitializeSentry(Core.Params);
    xray::StartupMark("device_sentry"); // 50z1: crashpad_handler started from the game folder

    Memory.mem_compact();
    b_is_Ready = true;

    SetupStates();
    string_path fname;
    FS.update_path(fname, "$game_data$", "shaders.xr");
    GEnv.Render->OnDeviceCreate(fname);
    xray::StartupMark("device_resources"); // 50z1: shaders.xr, .thm descriptions, shader cache, render targets
    m_imgui_render = GEnv.RenderFactory->CreateImGuiRender();
    m_imgui_render->OnDeviceCreate(GetImGuiContext());
    Statistic->OnDeviceCreate();
    dwFrame = 0;

    if (!GEnv.isDedicatedServer)
    {
        // Single owner: engine assigns; `Device_destroy` / shutdown clears after `secondary_tasks.wait()`.
        ParticleWorkerCallback = [] { ParticleWorker_RunBatch(); };
    }
}
