#pragma once
#ifndef IGame_PersistentH
#define IGame_PersistentH

#include "xrServerEntities/gametype_chooser.h"

#include "xrCommon/xr_set.h"
#include "xrCommon/xr_vector.h"

#include "xrCore/xr_trims.h"

#include "xrCDB/ISpatial.h"

#include "pure.h"
#ifndef _EDITOR
#include "Environment.h"
#include "EngineAPI.h"
#include "IGame_ObjectPool.h"
#endif
#include "ShadersExternalData.h" //--#SM+#--

class IGame_Level;
class IRenderable;
class IRenderVisual;
class ILoadingScreen;
class IMainMenu;
class ENGINE_API CPS_Instance;
//-----------------------------------------------------------------------------------------------------------
class ENGINE_API IGame_Persistent :
    public pureFrame,
    public pureAppActivate,
    public pureAppDeactivate,
    public IEventReceiver
{
public:
    struct ParticleStatistics
    {
        u32 Starting;
        u32 Active;
        u32 Destroying;

        ParticleStatistics() { FrameStart(); }
        void FrameStart()
        {
            Starting = 0;
            Active = 0;
            Destroying = 0;
        }

        void FrameEnd() {}
    };
    union params
    {
        struct
        {
            string256 m_game_or_spawn;
            string256 m_game_type;
            string256 m_alife;
            string256 m_new_or_load;
            EGameIDs m_e_game_type;
        };
        string256 m_params[4];
        params() { reset(); }
        void reset()
        {
            for (int i = 0; i < 4; ++i)
                xr_strcpy(m_params[i], "");
        }
        void parse_cmd_line(pcstr cmd_line)
        {
            reset();
            int n = _min(4, _GetItemCount(cmd_line, '/'));
            for (int i = 0; i < n; ++i)
            {
                _GetItem(cmd_line, i, m_params[i], '/');
                xr_strlwr(m_params[i]);
            }
        }
    };
    params m_game_params;

public:
    xr_set<CPS_Instance*> ps_active;
    xr_vector<CPS_Instance*> ps_destroy;
    xr_vector<CPS_Instance*> ps_needtoplay;

public:
    void destroy_particles(const bool& all_particles);

private:
    EVENT eStart;
    EVENT eStartLoad;
    EVENT eDisconnect;
    EVENT eStartMPDemo;

    u32 ll_dwReference{};
    int load_stage{};
    int max_load_stage{};
    CTimer phase_timer;

    bool loaded{};

    // Levels
    struct sLevelInfo
    {
        char* folder;
        char* name;
    };

    xr_vector<sLevelInfo> Levels;
    u32 Level_Current{ u32(-1) };

    void Level_Append(pcstr lname);

public:
    bool IsLoaded() const { return loaded; }
    void Level_Scan();
    int Level_ID(pcstr name, pcstr ver, bool bSet);
    void Level_Set(u32 id);
    static CInifile* GetArchiveHeader(pcstr name, pcstr ver);

public:
    virtual IGame_Level* CreateLevel() { return nullptr; }
    virtual void         DestroyLevel(IGame_Level*& lvl) { VERIFY(lvl == nullptr); }

    virtual void PreStart(pcstr op);
    virtual void Start(pcstr op);
    virtual void Disconnect();

    ISpatial_DB SpatialSpace      { "Spatial obj"  };
    ISpatial_DB SpatialSpacePhysic{ "Spatial phys" };

#ifndef _EDITOR
    IGame_ObjectPool ObjectPool;
    CEnvironment* pEnvironment;
    CEnvironment& Environment() { return *pEnvironment; };
    void Prefetch();
#endif
    ILoadingScreen* m_pLoadingScreen{};
    ISoundScene* m_pSound{};
    IMainMenu* m_pMainMenu{};

    bool IsMainMenuActive() const;
    bool MainMenuActiveOrLevelNotExist() const;

    ParticleStatistics stats;

    ShadersExternalData* m_pGShaderConstants; //--#SM+#--

    const ParticleStatistics& GetStats() { return stats; }
    virtual bool OnRenderPPUI_query() { return false; }; // should return true if we want to have second function called
    virtual void OnRenderPPUI_main(){};
    virtual void OnRenderPPUI_PP(){};

    void OnEvent(EVENT E, u64 P1, u64 P2) override;

    virtual void OnAppStart();
    virtual void OnAppEnd();
    virtual void OnAppActivate();
    virtual void OnAppDeactivate();
    virtual void OnFrame();
    /** Main thread only: spatial DB + persistent particle queues before `PreRenderThread` (ixray order vs `UpdateParticles`). */
    virtual void OnFrameBeforePreRender();
    /** Main thread only: `CEnvironment::OnFrame` before `PreRenderThread` (single call per frame; not in `seqFrame`). */
    virtual void OnFrameEnvironment();

    // вызывается только когда изменяется тип игры
    virtual void OnGameStart();
    virtual void OnGameEnd();

    virtual void UpdateGameType(){};
    virtual void GetCurrentDof(Fvector3& dof) { dof.set(-1.4f, 0.0f, 250.f); };
    virtual void SetBaseDof(const Fvector3& /*dof*/) {};
    virtual void OnSectorChanged(IRender_Sector::sector_id_t /*sector*/) {};
    virtual void OnAssetsChanged();

    // Called at the start of the main scene render pass (after level exists); used for dynamic weapon inventory icons.
    virtual void OnWeaponIconRenderPass() {}
    // True when OnWeaponIconRenderPass() of this frame would render icons (they use the render's immediate dsgraph):
    // the early main build (r__main_build_early) keeps the old order in such frames. Main thread.
    virtual bool WeaponIconRenderPassPending() const { return false; }

    // begin=true: prepare subject (e.g. identity XFORM) before renderable_Render; begin=false: restore.
    virtual void OnWeaponIconSnapshot(IRenderable* subject, bool begin) { (void)subject; (void)begin; }

    // Called after Lua reload_system_ini() rebuilt pSettings (system.ltx).
    virtual void OnSystemIniReloaded() {}

    // Persisted $user$ weapon/item icon RTs were dropped (device destroy / render reset).
    virtual void OnWeaponIconUserRtsReleased() {}

    // One persisted icon RT was dropped because the store went over its budget. The game must stop
    // reporting that texture as ready, otherwise the UI hides the static icon and draws nothing.
    virtual void OnWeaponIconRtEvicted(pcstr texture_name) { (void)texture_name; }

    IGame_Persistent();
    virtual ~IGame_Persistent();

    // Loading
    void LoadBegin();
    void LoadEnd();
    void LoadTitle(pcstr ls_title = nullptr, bool change_tip = false, shared_str map_name = nullptr);
    void LoadStage(bool draw = true);
    void LoadDraw() const;

    void load_draw_internal() const;
    void ShowLoadingScreen(bool show) const;

    ICF u32 GameType() { return m_game_params.m_e_game_type; };
    virtual void DumpStatistics(class IGameFont& font, class IPerformanceAlert* alert);
    virtual bool CanBePaused() { return true; }
};

class IMainMenu
{
public:
    virtual ~IMainMenu(){};
    virtual void Activate(bool bActive) = 0;
    virtual bool IsActive() const = 0;
    virtual bool CanSkipSceneRendering() = 0;
    virtual void DestroyInternal(bool bForce) = 0;
};

extern ENGINE_API IGame_Persistent* g_pGamePersistent;

#ifndef _EDITOR
// 50a-stage1-prefetch.md (plans/optimization_mt; plan 50, rules R1 variant B and R4): the lists of
// prefetch_single.ltx in their priority order and one byte budget for the objects and the models stage. A list line is
// "<name> = <KB>, <order>": its place in one priority order over both lists (CInifile keeps the keys of a section
// sorted by name) and the size of the files it adds to the lines with a smaller order, of either list (ogf, LOD, omf,
// dds). A line without a number gets the default size, a line without an order goes after the ordered ones, in name
// order. Main thread, inside IGame_Persistent::Prefetch (outside it the lists are taken whole).
struct PrefetchListItem
{
    shared_str name; // the key as written
    u64 bytes{}; // the estimated size of its files
};

// The lines of the list section (it must exist, as r_section) in the priority order
ENGINE_API void PrefetchListRead(pcstr section, xr_vector<PrefetchListItem>& items);
// Cuts the list (read by PrefetchListRead) to what the prefetch budget allows. PrefetchMode::Full: all of it.
// Core / Level: IGame_Persistent::Prefetch merges both lists by <order> beforehand and takes the merged lines while the
// estimated memory of what is taken fits prefetch_budget, the first line that does not fit ends the walk; each stage
// gets the lines of its list from that prefix. stage: "objects" or "models".
ENGINE_API void PrefetchListTake(pcstr stage, xr_vector<PrefetchListItem>& items);

// R4: how many bytes (by the list estimates) the start-up warm threads of the model prefetch may read ahead of the
// loads - from the RAM available at the start of the prefetch; 0 = no warm-up. with_textures: false = no dds.
struct PrefetchWarmLimits
{
    u64 lead_bytes;
    bool with_textures;
};
ENGINE_API PrefetchWarmLimits PrefetchWarmLimitsNow();
#endif
#endif // IGame_PersistentH
