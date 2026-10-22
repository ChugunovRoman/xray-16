
// EngineAPI.cpp: implementation of the CEngineAPI class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "EngineAPI.h"
#include "XR_IOConsole.h"

#include "xrCore/xr_token.h"

#include "xrScriptEngine/ScriptExporter.hpp"
#include "xrScriptEngine/script_space.hpp"

extern xr_vector<xr_token> VidQualityToken;

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CEngineAPI::CEngineAPI()
{
    pCreate = [](CLASS_ID) -> IFactoryObject*
    {
        return nullptr;
    };

    pDestroy = [](IFactoryObject* p)
    {
        R_ASSERT2(p == nullptr, "Attempting to release an object that shouldn't be allocated");
    };
}

CEngineAPI::~CEngineAPI()
{
    VidQualityToken.clear();
}

bool is_enough_address_space_available()
{
#if defined(XR_PLATFORM_WINDOWS)
    SYSTEM_INFO system_info;
    GetSystemInfo(&system_info);
    return (*(u32*)&system_info.lpMaximumApplicationAddress) > 0x90000000;
#else
    return true; // In Linux allocated memory limited only by pointer size
#endif
}

void CEngineAPI::SelectRenderer()
{
    ZoneScoped;

    // User has some renderer selected, find it
    pcstr selected_mode = Console->GetString("renderer");
    const auto it = std::find_if(renderModes.begin(), renderModes.end(), [selected_mode](const auto& pair)
    {
        return xr_strcmp(selected_mode, pair.first) == 0;
    });

    // Check if we can use it
    if (it != renderModes.end())
    {
        if (it->second->CheckGameRequirements())
            selectedRenderer = it->second;
    }

    // Renderer is either fully unsupported (hardware)
    // or we don't comply with it's requirements (e.g. shaders missing)
    if (!selectedRenderer)
    {
        // Select the best suitable one. renderModes is ordered by the addresses of the mode strings, not by quality:
        // "any first" picked renderer_r2 (D3D11 at feature level 10.x) on a first start without user.ltx (and for the
        // shipped renderer_r2a, a mode that is not offered). GlobalWar targets r4 (the launcher writes RendererR4), so
        // the modes are tried from the best down; any other suitable one after them.
        static constexpr pcstr preferred_modes[] =
        {
            "renderer_r4", "renderer_r3", "renderer_r2.5", "renderer_r2", "renderer_rgl"
        };
        string64 wanted; // the value of the renderer command before the choice (GetString's buffer changes with it)
        xr_strcpy(wanted, selected_mode ? selected_mode : "");
        const auto choose = [&](pcstr mode, RendererModule* renderer)
        {
            selectedRenderer = renderer;
            selected_mode = mode;
            string64 buf;
            xr_sprintf(buf, "renderer %s", selected_mode);
            Console->Execute(buf);
            Msg("* renderer: '%s' is not available, selected %s", wanted, selected_mode);
        };
        for (pcstr preferred : preferred_modes)
        {
            const auto found = std::find_if(renderModes.begin(), renderModes.end(),
                [preferred](const auto& pair) { return xr_strcmp(preferred, pair.first) == 0; });
            if (found != renderModes.end() && found->second->CheckGameRequirements())
            {
                choose(found->first, found->second);
                break;
            }
        }
        if (!selectedRenderer)
        {
            for (const auto& [mode, renderer] : renderModes)
            {
                if (renderer->CheckGameRequirements())
                {
                    choose(mode, renderer);
                    break;
                }
            }
        }
    }

    R_ASSERT2(selectedRenderer, "Can't setup renderer");

    // Ask current renderer to setup GEnv
    selectedRenderer->SetupEnv(selected_mode);

    Log("Selected renderer:", selected_mode);
}

void CEngineAPI::Initialize(GameModule* game)
{
    ZoneScoped;

    SelectRenderer();

    if (game)
    {
        gameModule = game;
        gameModule->initialize(pCreate, pDestroy);
        R_ASSERT(pCreate);
        R_ASSERT(pDestroy);
    }
}

void CEngineAPI::Destroy()
{
    ZoneScoped;

    if (gameModule)
        gameModule->finalize();

    selectedRenderer = nullptr;

    pCreate = nullptr;
    pDestroy = nullptr;

    XRC.r_clear_compact();
}

void CEngineAPI::CreateRendererList(const std::array<RendererModule*, 2>& modules, pcstr requestedMode)
{
    if (!VidQualityToken.empty())
        return;

    ZoneScoped;

    const auto loadRenderer = [this](RendererModule* module) -> bool
    {
        if (!module)
            return false;

        const auto& modes = module->ObtainSupportedModes(); // Performs HW tests, may take time
        if (modes.empty())
            return false;

        for (const auto [mode, modeIndex] : modes)
        {
            const auto it = renderModes.find(mode);
            if (it != renderModes.end())
            {
                VERIFY3(false, "Renderer mode duplicate. Skipping.", mode);
                continue;
            }
            renderModes[mode] = module;
            VidQualityToken.emplace_back(mode, modeIndex);
        }

        return true;
    };

    if (GEnv.isDedicatedServer)
    {
        R_ASSERT2(loadRenderer(modules[0]), "Dedicated server needs xrRender to work");
    }
    else
    {
        // 50z2 (plans/optimization_mt/50z2-startup-thm-shaders-probes-preview.md): every module used to be probed - a
        // D3D11 device and swap chain, then two GL windows and contexts (the ICD load) - on every start, with r4
        // selected too. A later module is probed only when it is needed: no earlier module gave a mode the game can
        // use, or the renderer asked for is one of its modes. Otherwise its modes are listed untested, so the options
        // still offer them: choosing one takes effect after a restart, where that start probes the module (and if
        // the test fails there, SelectRenderer falls back to the best available mode, as for any unavailable one).
        // SelectRenderer never picks an untested mode: it is not in renderModes.
        bool usable = false; // an earlier probed module has modes and the game can use it
        for (const auto& module : modules)
        {
            if (!module)
                continue;
            if (usable)
            {
                const auto untested = module->GetUntestedModes();
                const bool requested = requestedMode && requestedMode[0] &&
                    std::any_of(untested.begin(), untested.end(), [requestedMode](const auto& m)
                    {
                        return xr_stricmp(m.first, requestedMode) == 0;
                    });
                if (!untested.empty() && !requested)
                {
                    for (const auto& [mode, modeIndex] : untested)
                    {
                        const bool listed = std::any_of(VidQualityToken.begin(), VidQualityToken.end(),
                            [mode = mode](const xr_token& t) { return t.name && xr_strcmp(t.name, mode) == 0; });
                        if (!listed)
                            VidQualityToken.emplace_back(mode, modeIndex);
                    }
                    Msg("* renderer: '%s' asked for, a module with %u more mode(s) is not probed (listed untested)",
                        requestedMode && requestedMode[0] ? requestedMode : "(none)", u32(untested.size()));
                    continue;
                }
            }
            if (loadRenderer(module) && module->CheckGameRequirements())
                usable = true;
        }
    }

    auto& modes = VidQualityToken;
    Msg("Available render modes[%d]:", modes.size());
    for (const auto& mode : modes)
    {
        if (mode.name)
            Log(mode.name);
    }
    modes.emplace_back(nullptr, -1);
}

struct CEngineAPIExport
{
    // Not in CEngineAPI directly due to linking problems with xrCDB
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

void CEngineAPIExport::script_register(lua_State* luaState)
{
    using namespace luabind;
    module(luaState)
    [
        def("is_enough_address_space_available", &is_enough_address_space_available),
        def("xrRender_test_r2_hw", +[](){ return true; })
    ];
}
