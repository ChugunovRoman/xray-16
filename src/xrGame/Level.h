#pragma once

#include "xrEngine/IGame_Level.h"
#include "xrEngine/IGame_Persistent.h"
#if defined(XR_PLATFORM_WINDOWS)
#include "xrNetServer/NET_Client.h"
#else // XXX: multiplayer on Linux
#include "xrNetServer/empty/NET_Client.h"
#endif
#include "xrEngine/StatGraph.h"
#include "xrMessages.h"
#include "alife_space.h"
#include "xrCore/xrDebug.h"
#include "xrServer.h"
#include "GlobalFeelTouch.hpp"
#include "Level_network_map_sync.h"
#include "secure_messaging.h"
#include "traffic_optimization.h"

class CHUDManager;
class CParticlesObject;
class xrServer;
class game_cl_GameState;
class NET_Queue_Event;
class NET_Event;
class CSE_Abstract;
class CSpaceRestrictionManager;
class CSeniorityHierarchyHolder;
class CClientSpawnManager;
class CGameObject;
class CAutosaveManager;
class CPHCommander;
class CLevelDebug;
class LevelGraphDebugRender;
class CLevelSoundManager;
class CGameTaskManager;
class CZoneList;
class message_filter;
class demoplay_control;
class demo_info;
class CStreamReader;

class CDebugRenderer;

extern ENGINE_API float g_fov;

const int maxRP = 64;
const int maxTeams = 32;

class CFogOfWarMngr;
class CBulletManager;
class CMapManager;

namespace file_transfer
{
class client_site;
}

class CLevel : public IGame_Level, public IPureClient
{
public:
    struct AIStatistics
    {
        CStatTimer Think; // thinking
        CStatTimer Range; // query: range
        CStatTimer Path; // query: path
        CStatTimer Node; // query: node
        CStatTimer Vis; // visibility detection - total
        CStatTimer VisQuery; // visibility detection - portal traversal and frustum culling
        CStatTimer VisRayTests; // visibility detection - ray casting
        CStatTimer LuaGC; // LuaJIT Garbage collector

        AIStatistics() { FrameStart(); }
        void FrameStart()
        {
            Think.FrameStart();
            Range.FrameStart();
            Path.FrameStart();
            Node.FrameStart();
            Vis.FrameStart();
            VisQuery.FrameStart();
            VisRayTests.FrameStart();
            LuaGC.FrameStart();
        }

        void FrameEnd()
        {
            Think.FrameEnd();
            Range.FrameEnd();
            Path.FrameEnd();
            Node.FrameEnd();
            Vis.FrameEnd();
            VisQuery.FrameEnd();
            VisRayTests.FrameEnd();
            LuaGC.FrameEnd();
        }
    };
    AIStatistics AIStats;
#include "Level_network_Demo.h"
    void ClearAllObjects();

protected:
    typedef IGame_Level inherited;
    CLevelSoundManager* m_level_sound_manager = nullptr;
    CSpaceRestrictionManager* m_space_restriction_manager = nullptr;
    CSeniorityHierarchyHolder* m_seniority_hierarchy_holder = nullptr;
    CClientSpawnManager* m_client_spawn_manager = nullptr;
    CAutosaveManager* m_autosave_manager = nullptr;
    CDebugRenderer* m_debug_renderer = nullptr;
#ifdef DEBUG
    LevelGraphDebugRender* levelGraphDebugRender = nullptr;
#endif
    CPHCommander* m_ph_commander = nullptr;
    CPHCommander* m_ph_commander_scripts = nullptr;

    // Local events
    EVENT eChangeRP;
    EVENT eDemoPlay;
    EVENT eChangeTrack;
    EVENT eEnvironment;
    EVENT eEntitySpawn;

    // Statistics
    CStatGraph* pStatGraphS = nullptr;
    u32 m_dwSPC; // SendedPacketsCount
    u32 m_dwSPS; // SendedPacketsSize
    CStatGraph* pStatGraphR = nullptr;
    u32 m_dwRPC; // ReceivedPacketsCount
    u32 m_dwRPS; // ReceivedPacketsSize

private:
    struct ClientStatistics
    {
        CStatTimer ClientSend;
        CStatTimer ClientRecv;
        CStatTimer ClientCompressor;
        // dbg stats
        CStatTimer ClientSendInternal;
        CStatTimer BulletManagerCommit;

        ClientStatistics() { FrameStart(); }
        void FrameStart()
        {
            ClientSend.FrameStart();
            ClientRecv.FrameStart();
            ClientCompressor.FrameStart();
            ClientSendInternal.FrameStart();
            BulletManagerCommit.FrameStart();
        }

        void FrameEnd()
        {
            ClientSend.FrameEnd();
            ClientRecv.FrameEnd();
            ClientCompressor.FrameEnd();
            ClientSendInternal.FrameEnd();
            BulletManagerCommit.FrameEnd();
        }
    };
    ClientStatistics stats;

public:
#ifdef DEBUG
    CLevelDebug* m_level_debug = nullptr;
#endif
    // Network
    u32 GetInterpolationSteps();
    void SetInterpolationSteps(u32 InterpSteps);
    bool InterpolationDisabled();
    void ReculcInterpolationSteps();
    u32 GetNumCrSteps() const { return m_dwNumSteps; }
    void SetNumCrSteps(u32 NumSteps);
    static void PhisStepsCallback(u32 Time0, u32 Time1);
    bool In_NetCorrectionPrediction() { return m_bIn_CrPr; }

    void OnMessage(void* data, u32 size) override;
    void OnInvalidHost() override;
    void OnInvalidPassword() override;
    void OnSessionFull() override;
    void OnConnectRejected() override;

private:
    void OnSecureMessage(NET_Packet& P);
    void OnSecureKeySync(NET_Packet& P);
    void SecureSend(NET_Packet& P, u32 dwFlags = 0x0008 /*DPNSEND_GUARANTEED*/, u32 dwTimeout = 0);
    secure_messaging::key_t m_secret_key;
    bool m_bNeed_CrPr = false;
    u32 m_dwNumSteps = 0;
    bool m_bIn_CrPr = false;
    xr_vector<CGameObject*> pObjects4CrPr;
    xr_vector<CGameObject*> pActors4CrPr;
    IGameObject* pCurrentControlEntity = nullptr;
    xrServer::EConnect m_connect_server_err = xrServer::ErrNoError;

public:
    void AddObject_To_Objects4CrPr(CGameObject* pObj);
    void AddActor_To_Actors4CrPr(CGameObject* pActor);
    void RemoveObject_From_4CrPr(CGameObject* pObj);
    IGameObject* CurrentControlEntity() const { return pCurrentControlEntity; }
    void SetControlEntity(IGameObject* O) { pCurrentControlEntity = O; }

private:
    void make_NetCorrectionPrediction();
    u32 m_dwDeltaUpdate = 0;
    u32 m_dwLastNetUpdateTime = 0;
    void UpdateDeltaUpd(u32 LastTime);
    void BlockCheatLoad();
    bool Connect2Server(const char* options);
    void SendClientDigestToServer();
    shared_str m_client_digest; // for screenshots

public:
    shared_str const get_cdkey_digest() const { return m_client_digest; }

private:
    bool m_bConnectResultReceived;
    bool m_bConnectResult;
    xr_string m_sConnectResult;

public:
    void OnGameSpyChallenge(NET_Packet* P);
    void OnBuildVersionChallenge();
    void OnConnectResult(NET_Packet* P);
    // Static particles
    using POVec = xr_vector<CParticlesObject*>;
    POVec m_StaticParticles;
    game_cl_GameState* game = nullptr;
    bool m_bGameConfigStarted = false;
    bool game_configured = false;
    NET_Queue_Event* game_events = nullptr;
    xr_deque<CSE_Abstract*> game_spawn_queue;
    xrServer* Server = nullptr;
    GlobalFeelTouch m_feel_deny;
    CZoneList* hud_zones_list = nullptr;
    CZoneList* create_hud_zones_list();

private:
    // preload sounds registry
    using SoundRegistryMap = xr_map<shared_str, ref_sound>;
    SoundRegistryMap sound_registry;

public:
    void PrefetchSound(const char* name);

protected:
    bool net_start_result_total;
    bool connected_to_server;
    bool deny_m_spawn; // only for debug...
    bool sended_request_connection_data;
    void MakeReconnect();
    LevelMapSyncData map_data;
    bool synchronize_map_data();
    bool synchronize_client();
    bool net_start1();
    bool net_start2();
    bool net_start3();
    bool net_start4();
    bool net_start5();
    bool net_start6();
    bool net_start_client1();
    bool net_start_client2();
    bool net_start_client3();
    bool net_start_client4();
    bool net_start_client5();
    bool net_start_client6();
    void net_OnChangeSelfName(NET_Packet* P);
    void CalculateLevelCrc32();

public:
    bool IsChecksumsEqual(u32 check_sum) const;

    // sounds
    xr_vector<ref_sound*> static_Sounds;

    // startup options
    shared_str m_caServerOptions;
    shared_str m_caClientOptions;

    // Starting/Loading
    bool net_Start(const char* op_server, const char* op_client) override;
    void net_Load(const char* name) override;
    void net_Save(const char* name) override;
    void net_Stop() override;
    virtual bool net_Start_client(const char* name);
    void net_Update() override;
    bool Load_GameSpecific_Before() override;
    bool Load_GameSpecific_After() override;
    void Load_GameSpecific_CFORM(CDB::TRI* T, u32 count) override;
    void Load_GameSpecific_CFORM_Serialize(IWriter& writer) override;
    bool Load_GameSpecific_CFORM_Deserialize(IReader& reader) override;
    void Load_GameSpecific_CFORM_SetMaterials(CDB::TRI* tris, u32 count, xr_map<u16, shared_str>& gameMtls) override;

    // Events
    void OnEvent(EVENT E, u64 P1, u64 P2) override;
    void OnFrame(void) override;
    // The game logic thread, after SoundEvent_Dispatch and before the scheduler (06-framemove-offload.md, B4/B5):
    // the queued NPC sound callbacks and level_on_frame when OnFrame of this frame left it there.
    void OnGameLogicFrame() override;
    // plans/optimization_mt/22-game-thread.md, step 2: cap and dedupe of the sound delegates
    void SoundEvent_Filter(xr_vector<_esound_delegate>& events) override;
    void OnRender() override;
    void RenderBulletTracersForMainViewport() override;
    void RenderBulletTracersForSecondViewport() override;
    void DumpStatistics(class IGameFont& font, class IPerformanceAlert* alert) override;
    shared_str OpenDemoFile(const char* demo_file_name) override;
    void net_StartPlayDemo() override;
    void cl_Process_Event(u16 dest, u16 type, NET_Packet& P);
    void cl_Process_Spawn(NET_Packet& P);
    // frame_budget: the call of CLevel::OnFrame, the only one the spawn budget (spawn_budget_us) applies to; every other
    // call (level stop, ClearAllObjects, g_bDebugEvents) drains the whole queue
    void ProcessGameEvents(bool frame_budget = false);
    void ProcessGameSpawns();
    // Spawn stage 2: a save or a level change that finds M_SPAWN still in the client queue logs it. Their objects are
    // saved from the server state, as when they come online (the server is the source of truth), so it is no error.
    void SpawnQueueNoteSave(pcstr where) const;

private:
    // plans/spawn_optimization/SPEC.md 8.6: the server frees an ID on entity_Destroy and may hand it to a new object in
    // the same frame, while the client still holds the old object (GE_DESTROY only marks it, CObjectList::Update destroys
    // it after ProcessGameEvents). An M_SPAWN that finds its ID held by an object marked for destruction is not dropped:
    // ProcessGameEvents stops in front of it and the rest of the queue (its children, its events) waits for the next frame.
    struct SpawnIdWait
    {
        shared_str section; // of the waiting spawn
        u32 first_frame{};
        u32 frames{}; // frames it has waited so far
        u16 id{u16(-1)};
        bool waiting{};
    };
    SpawnIdWait m_spawn_id_wait;
    // true: the head M_SPAWN must wait for the next frame (see SpawnIdWait); false: process it now
    bool SpawnMustWait(u16 id, pcstr section);
    // after an M_SPAWN left the queue: if it is the one that waited, log whether it materialized
    void SpawnWaitDone(u16 id);

    // Spawn stage 2 (plans/spawn_optimization/SPEC.md 5.A, section 12): a budget of the client materialization of
    // M_SPAWN per frame. The queue is taken from its head only, nothing overtakes anything. An "atomic group" - a root
    // M_SPAWN and the M_SPAWN of its children right behind it (parent = the root or a child already in the group;
    // M_EVENT between them do not break it) - is materialized whole in one frame: the budget is checked only in front
    // of a root. When it is spent, the root and everything behind it (its children, events to it and to anyone else)
    // wait for the next frame, in the same order.
public: // the reasons are named by the spawn report too (Level_network_spawn.cpp)
    enum class SpawnBudgetForce : u8
    {
        None,
        Loading, // loading screen / precache frames: the initial spawn of the level
        Pause, // the game is paused: nothing to protect, and the queue is empty before a save from the menu
        Delay, // the head waits longer than spawn_max_delay_ms
        Count
    };

private:
    struct SpawnBudgetPass
    {
        CTimerBase timer; // from the start of the pass: M_EVENT count too
        u64 budget_ns{}; // 0 = no budget (spawn_budget_us 0, not the call of OnFrame, g_bDebugEvents, not single)
        u32 min_groups{};
        u32 max_delay_ms{};
        u64 catchup_ns{}; // the limit of a pass that goes past the budget for an old head (spawn_catchup_us), 0 = none
        SpawnBudgetForce force{}; // Loading / Pause: the budget only counts what it would have deferred
        u32 groups{}; // groups materialized in this pass (one started counts)
        u32 group_size_max{}; // M_SPAWN of the largest group of this pass
        u32 forced[size_t(SpawnBudgetForce::Count)]{}; // groups materialized past the spent budget, by reason
        bool stopped{}; // the budget left the head group for the next frame
    };
    // In front of a root M_SPAWN: true = the budget of this frame is spent, stop here
    bool SpawnBudgetSpent(SpawnBudgetPass& pass, const NET_Event& head);
    // "*   [spawn] budget ..." line of the spawn report (Level_network_spawn.cpp), once per pass with the budget on
    struct SpawnBudgetFrame
    {
        u32 budget_us{};
        u32 min_groups{};
        u32 max_delay_ms{};
        u32 groups{};
        u32 group_size_max{};
        u32 deferred_groups{}; // groups left in the queue when the budget stopped (0 = it did not)
        u32 deferred_spawns{};
        u32 queue_wait_ms{};
        u32 forced[size_t(SpawnBudgetForce::Count)]{};
        bool stopped{};
    };
    void SpawnBudgetReport(const SpawnBudgetFrame& frame);

public:
    void ProcessCompressedUpdate(NET_Packet& P, const Flags8& compression_type);

    // Input
    void IR_OnKeyboardPress(int key) override;
    void IR_OnKeyboardRelease(int key) override;
    void IR_OnKeyboardHold(int key) override;

    void IR_OnTextInput(pcstr text) override;

    void IR_OnMousePress(int btn) override;
    void IR_OnMouseRelease(int btn) override;
    void IR_OnMouseHold(int btn) override;
    void IR_OnMouseMove(int, int) override;
    void IR_OnMouseWheel(float x, float y) override;

    void IR_OnControllerPress(int key, const ControllerAxisState& state) override;
    void IR_OnControllerRelease(int key, const ControllerAxisState& state) override;
    void IR_OnControllerHold(int key, const ControllerAxisState& state) override;

    void IR_OnControllerAttitudeChange(Fvector change) override;

    void IR_OnActivate() override;
    void IR_OnDeactivate() override;

    // Returns respawn point ID
    int get_RPID(LPCSTR name); // Xottab_DUTY: Seems to be deprecated

    // Game
    void InitializeClientGame(NET_Packet& P);
    void ClientReceive();
    void ClientSend();
    void ClientSendProfileData();
    void ClientSave();
    u32 Objects_net_Save(NET_Packet* _Packet, u32 start, u32 count);
    void Send(NET_Packet& P, u32 dwFlags = 0x0008 /*DPNSEND_GUARANTEED*/, u32 dwTimeout = 0) override;
    void g_cl_Spawn(LPCSTR name, u8 rp, u16 flags, Fvector pos); // only ask server
    void g_sv_Spawn(CSE_Abstract* E); // server reply/command spawning

    // Save/Load/State
    void SLS_Load(LPCSTR name); // Game Load
    void SLS_Default(); // Default/Editor Load
    IC CSpaceRestrictionManager& space_restriction_manager();
    IC CSeniorityHierarchyHolder& seniority_holder();
    IC CClientSpawnManager& client_spawn_manager();
    IC CAutosaveManager& autosave_manager();
    IC CDebugRenderer& debug_renderer();
    void script_gc(); // GC-cycle
    IC CPHCommander& ph_commander();
    IC CPHCommander& ph_commander_scripts();

    CLevel();
    virtual ~CLevel();

private:
    // Device.PostSchedulerVisionBatch: right after Sheduler.Update on the game thread (EngineThreading.cpp).
    static void VisionBatchPostScheduler();
    // level_on_frame of the event bus (only when it has subscribers).
    void EmitLevelOnFrame();
    // Set by OnFrame (main, unpaused frame), consumed by OnGameLogicFrame of the same frame.
    // The kick of the logic thread (task / Event) orders the write before the read.
    bool m_level_on_frame_pending = false;
    // Device.BonesPrepassCallback (always, plans/optimization_mt/04-bones-prepass.md): main thread,
    // between the game logic and the render (device.cpp, RunBonesPrepass).
    static void BonesPrepass();

public:
    // Bones prepass: CGameObject::renderable_Render notes the object (once per frame, any render thread); the next
    // frame's BonesPrepass resolves the IDs (net_Find) and computes the bones the render computed last time.
    static void BonesPrepassNote(u16 id);

    // название текущего уровня
    shared_str name() const override;
    // this method can be used ONLY from CCC_ChangeGameType
    // XXX nitrocaster: why c_str?
    shared_str version() const { return map_data.m_map_version.c_str(); }
    void GetLevelInfo(CServerInfo* si) override;

    // gets the time from the game simulation
    ALife::_TIME_ID GetStartGameTime();
    ALife::_TIME_ID GetGameTime();

    // возвращает время для энвайронмента в милисекундах относительно начала игры
    ALife::_TIME_ID GetEnvironmentGameTime() const override;
    static_assert(std::is_same_v<ALife::_TIME_ID, u64>,
        "Please, change return type of GetEnvironmentGameTime in IGame_Level and CGameLevel accordingly");

    // игровое время в отформатированном виде
    void GetGameDateTime(u32& year, u32& month, u32& day, u32& hours, u32& mins, u32& secs, u32& milisecs);

    float GetGameTimeFactor();
    void SetGameTimeFactor(const float fTimeFactor);
    void SetGameTimeFactor(ALife::_TIME_ID GameTime, const float fTimeFactor);

    float GetEnvironmentTimeFactor() const override;
    void SetEnvironmentTimeFactor(const float fTimeFactor) override;
    void SetEnvironmentGameTimeFactor(u64 const& GameTime, float const& fTimeFactor) override;

    // gets current daytime [0..23]
    u8 GetDayTime();
    u32 GetGameDayTimeMS();
    float GetGameDayTimeSec();
    float GetEnvironmentGameDayTimeSec() const override;

protected:
    // CFogOfWarMngr* m_pFogOfWarMngr;
    CMapManager* m_map_manager = nullptr;
    CGameTaskManager* m_game_task_manager = nullptr;

public:
    CMapManager& MapManager() const { return *m_map_manager; }
    CGameTaskManager& GameTaskManager() const { return *m_game_task_manager; }
    void OnAlifeSimulatorLoaded();
    void OnAlifeSimulatorUnLoaded();

protected:
    CBulletManager* m_pBulletManager;

public:
    IC CBulletManager& BulletManager() { return *m_pBulletManager; }
    bool IsServer();
    bool IsClient();
    CSE_Abstract* spawn_item(
        LPCSTR section, const Fvector& position, u32 level_vertex_id, u16 parent_id, bool return_item = false);

protected:
    u32 m_dwCL_PingDeltaSend = 1000;
    u32 m_dwCL_PingLastSendTime = 0;
    u32 m_dwRealPing = 0;

public:
    virtual u32 GetRealPing() { return m_dwRealPing; }
public:
    void remove_objects();
    void OnSessionTerminate(pcstr reason) override;
    file_transfer::client_site* m_file_transfer = nullptr;
    compression::ppmd_trained_stream* m_trained_stream = nullptr;
    compression::lzo_dictionary_buffer m_lzo_dictionary;
    // aligned to 16 bytes m_lzo_working_buffer
    u8* m_lzo_working_memory = nullptr;
    u8* m_lzo_working_buffer = nullptr;
    void init_compression();
    void deinit_compression();

#ifdef DEBUG
    LevelGraphDebugRender* GetLevelGraphDebugRender() const { return levelGraphDebugRender; }
#endif

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

// XXX nitrocaster: should not cast to inherited
IC CLevel& Level() { return *(CLevel*)g_pGameLevel; }
IC game_cl_GameState& Game() { return *Level().game; }
u32 GameID();

#ifdef DEBUG
IC CLevelDebug& DBG() { return *(CLevelDebug*)Level().m_level_debug; }
#endif

IC CSpaceRestrictionManager& CLevel::space_restriction_manager()
{
    VERIFY(m_space_restriction_manager);
    return *m_space_restriction_manager;
}

IC CSeniorityHierarchyHolder& CLevel::seniority_holder()
{
    VERIFY(m_seniority_hierarchy_holder);
    return *m_seniority_hierarchy_holder;
}

IC CClientSpawnManager& CLevel::client_spawn_manager()
{
    VERIFY(m_client_spawn_manager);
    return *m_client_spawn_manager;
}

IC CAutosaveManager& CLevel::autosave_manager()
{
    VERIFY(m_autosave_manager);
    return *m_autosave_manager;
}

IC CDebugRenderer& CLevel::debug_renderer()
{
    VERIFY(m_debug_renderer);
    return *m_debug_renderer;
}

IC CPHCommander& CLevel::ph_commander()
{
    VERIFY(m_ph_commander);
    return *m_ph_commander;
}

IC CPHCommander& CLevel::ph_commander_scripts()
{
    VERIFY(m_ph_commander_scripts);
    return *m_ph_commander_scripts;
}

extern bool g_bDebugEvents;
