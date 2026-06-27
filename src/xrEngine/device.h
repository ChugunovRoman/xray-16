#pragma once
#ifndef xr_device
#define xr_device

// Note:
// ZNear - always 0.0f
// ZFar  - always 1.0f

#include "pure.h"

#include "xrCore/FTimer.h"
#include "Stats.h"
#include "xrCommon/xr_list.h"
#include "xrCore/Threading/Event.hpp"
#include "xrCore/fastdelegate.h"
#include "xrCore/ModuleLookup.hpp"

#define DEVICE_RESET_PRECACHE_FRAME_COUNT 10

#include "editor_base.h"
#include "Include/xrRender/FactoryPtr.h"
#include "Render.h"

#include "xrScriptEngine/ScriptExporter.hpp"

#include <SDL.h>
#include <functional>

#include "FrameTaskGroup.hpp"

constexpr float VIEWPORT_NEAR = 0.2f;
constexpr float VIEWPORT_NEAR_3D = 0.01f;
constexpr float HUD_VIEWPORT_NEAR = 0.05f;

class ENGINE_API CRenderDevice : public IWindowHandler
{
public:
    class ENGINE_API CSecondVPParams //--#SM+#-- +SecondVP+
    {
        bool isActive; // Флаг активации рендера во второй вьюпорт
        u8 frameDelay{2}; // synced from r__svp_frame_delay; 0 = every frame (see SetSVPFrameDelay / IsSVPFrame)
        bool bSecondCalculatePass{}; // true only during the extra Calculate() for dedicated second VP (see IGame_Level)

    public:
        bool isCamReady; // Флаг готовности камеры (FOV, позиция, и т.п) к рендеру второго вьюпорта

        bool isR1;

        bool IsSVPActive() { return isActive; }
        void SetSVPActive(bool bState);
        bool IsSVPFrame();
        void SetSecondCalculatePass(bool v) { bSecondCalculatePass = v; }
        bool IsSecondCalculatePass() const { return bSecondCalculatePass; }

        u8 GetSVPFrameDelay() { return frameDelay; }
        void SetSVPFrameDelay(u8 iDelay)
        {
            frameDelay = iDelay;
            clamp<u8>(frameDelay, 0, u8(-1));
        }
    };

    // Main objects used for creating and rendering the 3D scene
    // Real application window resolution
    SDL_Rect m_rcWindowBounds{};

    // Real game window resolution
    SDL_Rect m_rcWindowClient{};

private:
    u32 Timer_MM_Delta{};
    CTimer_paused Timer;
    CTimer_paused TimerGlobal;
    CTimer TimerMM;

    void SetupStates();

public:
    // Main window
    SDL_Window* m_sdlWnd{};

    // Engine flow-control
    u32 dwFrame{};
    u32 dwPrecacheFrame{};
    u32 dwPrecacheTotal{};

    // Rendering resolution
    u32 dwWidth{};
    u32 dwHeight{};

    float fWidth_2{};
    float fHeight_2{};

    bool b_is_Ready{};
    bool b_is_Active{};
    bool b_is_InFocus{};

    bool m_bNearer{};

public:
    void SetNearer(bool enabled)
    {
        if (enabled && !m_bNearer)
        {
            m_bNearer = true;
            mProject._43 -= EPS_L;
        }
        else if (!enabled && m_bNearer)
        {
            m_bNearer = false;
            mProject._43 += EPS_L;
        }
        GEnv.Render->SetCacheXform(mView, mProject);
        // R_ASSERT(0);
        // TODO: re-implement set projection
        // RCache.set_xform_project (mProject);
    }

public:
    // Registrators
    MessageRegistry<pureRender> seqRender;
    MessageRegistry<pureAppActivate> seqAppActivate;
    MessageRegistry<pureAppDeactivate> seqAppDeactivate;
    MessageRegistry<pureAppEnd> seqAppEnd;
    MessageRegistry<pureFrame> seqFrame;
    MessageRegistry<pureFrame> seqFrameMT;
    MessageRegistry<pureDeviceReset> seqDeviceReset;
    MessageRegistry<pureUIReset> seqUIReset;
    /** Main-thread / GameThread parallel hooks (see EngineThreading, PARALLEL_WORKER_BOUNDARY). */
    xr_vector<fastdelegate::FastDelegate0<>> seqParallel;
    /** Runs on main before `PreRenderThread` (ixray `seqParallelBeforRender`). */
    xr_vector<fastdelegate::FastDelegate0<>> seqParallelBeforRender;
    /** Runs on `PreRenderThread` before optional particle worker (ixray `seqParallelRender`). */
    xr_vector<fastdelegate::FastDelegate0<>> seqParallelRender;
    std::function<void()> ParticleWorkerCallback;
    /** B-1: physics step, run after the GameThread scheduler launch (overlaps it) when
     *  ps_mt_scheduler_physics_overlap != 0. Set by CPHWorld; CPHWorld::OnFrame (seqFrame) then no-ops. */
    std::function<void()> PhysicsFrameOverlapCallback;
    /** B-1: open the scheduler-overlap frame window before GameThread launch. Set by CPHWorld. */
    std::function<void()> PhysicsBeginOverlapFrameCallback;
    /** B-1: apply owner-level physics intents deferred during the overlap window. Called
     *  after secondary_tasks.wait(). Set by CPHWorld. */
    std::function<void()> PhysicsDeferredFlushCallback;
    /** P2: execute parallel vision batch and process results. Called in GameThread after Sheduler.Update(). */
    std::function<void()> PostSchedulerVisionBatch;
    /** Optional: flush model pool delete queue on main before `PreRenderThread` (ixray `ModelDefferClear`). Set from R2 `CRender::create`. */
    std::function<void()> ModelDeferredClear;
    CSecondVPParams m_SecondViewport; // --#SM+#-- +SecondVP+

private:
    struct RenderDeviceStatistics
    {
        CStatTimer RenderTotal; // pureRender
        CStatTimer EngineTotal; // pureFrame
        float fFPS, fRFPS, fTPS; // FPS, RenderFPS, TPS

        RenderDeviceStatistics()
        {
            fFPS = 30.f;
            fRFPS = 30.f;
            fTPS = 0;
        }
    };

    RenderDeviceStatistics stats;
    CStats* Statistic{};

public:
    // Engine flow-control
    float fTimeDelta{};
    float fTimeDeltaReal{};
    float fTimeGlobal{};
    u32 dwTimeDelta{};
    u32 dwTimeGlobal{};
    u32 dwTimeContinual{};

    // Cameras & projection
    Fvector vCameraPosition{};
    Fvector vCameraDirection{};
    Fvector vCameraTop{};
    Fvector vCameraRight{};

    Fmatrix mView{};
    Fmatrix mInvView{};
    Fmatrix mProject{};
    Fmatrix mFullTransform{};
    Fmatrix mInvFullTransform{};

    // Copies of corresponding members. Used for synchronization.
    Fvector vCameraPositionSaved{};
    Fvector vCameraDirectionSaved{};
    Fvector vCameraTopSaved{};
    Fvector vCameraRightSaved{};

    Fmatrix mViewSaved{};
    Fmatrix mProjectSaved{};
    Fmatrix mFullTransformSaved{};

    float fFOV{};
    float fASPECT{};

    bool m_allowWindowDrag{}; // For windowed mode
    bool IsAnselActive{};

    CRenderDevice()
    {
        Timer.Start();

        m_SecondViewport.SetSVPActive(false);
        m_SecondViewport.SetSecondCalculatePass(false);
        m_SecondViewport.isCamReady = false;
        m_SecondViewport.isR1 = false;
    }

    void Pause(bool bOn, bool bTimer, bool bSound, pcstr reason);
    bool Paused();

public:
    // Scene control
    void ProcessFrame();

    void PreCache(u32 amount, bool wait_user_input);

    bool BeforeFrame();
    void FrameMove();

    void OnCameraUpdated();
    void DoRender();
    bool RenderBegin();
    void Clear();
    void RenderEnd();

    void overdrawBegin();
    void overdrawEnd();

    // Mode control
    IC CTimer_paused* GetTimerGlobal() { return &TimerGlobal; }
    u32 TimerAsync() { return TimerGlobal.GetElapsed_ms(); }
    u32 TimerAsync_MMT() { return TimerMM.GetElapsed_ms() + Timer_MM_Delta; }

public:
    // Creation & Destroying
    void Create();
    void Destroy();

    void Reset(bool precache = true);

    void Run();
    void Shutdown();

    void ProcessEvent(const SDL_Event& event);
    void OnWindowActivate(SDL_Window* window, bool activated);

    void UpdateWindowProps();
    void UpdateWindowRects();
    void SelectResolution(bool windowed);

    void Initialize();

    void InitializeImGui();
    void DestroyImGui();

    void FillVideoModes();
    void CleanupVideoModes();

    const RenderDeviceStatistics& GetStats() const { return stats; }
    void DumpStatistics(class IGameFont& font, class IPerformanceAlert* alert);

    void SetWindowDraggable(bool draggable);
    bool IsWindowDraggable() const { return m_allowWindowDrag; }

    void* GetApplicationWindowHandle() const override;
    SDL_Window* GetApplicationWindow() override;
    void OnErrorDialog(bool beforeDialog) override;
    void OnFatalError() override;

    void time_factor(const float time_factor);

    IC float time_factor() const
    {
        VERIFY(Timer.time_factor() == TimerGlobal.time_factor());
        return (Timer.time_factor());
    }

public:
    // Multi-threading (ixray `secondary_tasks`: PreRenderThread + GameThread per frame)
    xr_frame_task_group secondary_tasks;
    Event PresentationFinished = nullptr;

    static constexpr u32 MaximalWaitTime = 16; // ms

    // Usable only when called from thread, that initialized SDL
    // Calls SDL_PumpEvents() at least twice.
    static void WaitEvent(Event& event)
    {
        // Once at the beginning:
        SDL_PumpEvents();

        while (!event.Wait(MaximalWaitTime))
            SDL_PumpEvents();

        // And once in the end:
        SDL_PumpEvents();
    }

    void AddSeqFrame(pureFrame* f, bool mt);
    void RemoveSeqFrame(pureFrame* f);

    ICF void remove_from_seq_parallel(const fastdelegate::FastDelegate0<>& delegate)
    {
        xr_vector<fastdelegate::FastDelegate0<>>::iterator I =
            std::find(seqParallel.begin(), seqParallel.end(), delegate);
        if (I != seqParallel.end())
            seqParallel.erase(I);
    }

    ICF void remove_from_seq_parallel_beforerender(const fastdelegate::FastDelegate0<>& delegate)
    {
        auto I = std::find(seqParallelBeforRender.begin(), seqParallelBeforRender.end(), delegate);
        if (I != seqParallelBeforRender.end())
            seqParallelBeforRender.erase(I);
    }

    ICF void remove_from_seq_parallel_render(const fastdelegate::FastDelegate0<>& delegate)
    {
        auto I = std::find(seqParallelRender.begin(), seqParallelRender.end(), delegate);
        if (I != seqParallelRender.end())
            seqParallelRender.erase(I);
    }

private:
    void CalcFrameStats();

public:
    [[nodiscard]]
    auto& editor() { return m_editor; }

    [[nodiscard]]
    auto editor_mode() const { return m_editor.is_shown(); }

    [[nodiscard]]
    auto GetImGuiContext() const { return m_imgui_context; }

public:
    struct ImGuiViewportData
    {
        SDL_Window* Window;
        bool        WindowOwned;

        ImGuiViewportData(SDL_Window* window) : Window(window), WindowOwned(false) {}

        ImGuiViewportData(ImVec2 pos, ImVec2 size, Uint32 flags)
        {
            Window = SDL_CreateWindow("ImGui Viewport (no title yet)",
                (int)pos.x, (int)pos.y, (int)size.x, (int)size.y, flags);
            WindowOwned = true;
        }

        ~ImGuiViewportData()
        {
            if (Window && WindowOwned)
            {
                SDL_DestroyWindow(Window);
            }
        }
    };

private:
    xray::editor::ide m_editor;

    ImGuiContext* m_imgui_context{};
    IImGuiRender* m_imgui_render{};

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

extern ENGINE_API CRenderDevice Device;

extern ENGINE_API bool g_bBenchmark;

typedef fastdelegate::FastDelegate0<bool> LOADING_EVENT;
extern ENGINE_API xr_list<LOADING_EVENT> g_loading_events;

class ENGINE_API CLoadScreenRenderer : public pureFrame, public pureRender
{
public:
    void OnFrame() override;
    void OnRender() override;

    void Start(bool b_user_input);
    void Stop();

    bool IsActive() const { return m_registered; }
    bool NeedsUserInput() const { return m_need_user_input; }

private:
    bool m_registered{};
    bool m_need_user_input{};
};
extern ENGINE_API CLoadScreenRenderer load_screen_renderer;

class CDeviceResetNotifier : public pureDeviceReset
{    
public:
    CDeviceResetNotifier(const int prio = REG_PRIORITY_NORMAL)
    {
        Device.seqDeviceReset.Add(this, prio);
    }
    virtual ~CDeviceResetNotifier() { Device.seqDeviceReset.Remove(this); }
};

class CUIResetNotifier : public pureUIReset
{
public:
    CUIResetNotifier(const int uiResetPrio = REG_PRIORITY_NORMAL)
    {
        Device.seqUIReset.Add(this, uiResetPrio);
    }

    virtual ~CUIResetNotifier()
    {
        Device.seqUIReset.Remove(this);
    }
};

#endif
