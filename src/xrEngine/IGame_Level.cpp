#include "stdafx.h"
#include "IGame_Level.h"
#include "IGame_Persistent.h"

#include "CustomHUD.h"
#include "device.h"
#include "Render.h"
#include "GameFont.h"
#include "Common/LevelStructure.hpp"
#include "CameraManager.h"
#include "xr_object.h"
#include "Feel_Sound.h"
#include "xrServerEntities/ai_sounds.h"

#include <algorithm>

ENGINE_API IGame_Level* g_pGameLevel = NULL;
extern bool g_bLoaded;

IGame_Level::IGame_Level()
    : ObjectSpace(&g_pGamePersistent->SpatialSpace)
{
    ZoneScoped;

    m_pCameras = xr_new<CCameraManager>(true);
    g_pGameLevel = this;
    pLevel = NULL;
    bReady = false;
    pCurrentEntity = NULL;
    pCurrentViewEntity = NULL;
    Sound = GEnv.Sound->create_scene();
    DefaultSoundScene = Sound;
#ifndef MASTER_GOLD
    GEnv.Render->ResourcesDumpMemoryUsage();
#endif
}

IGame_Level::~IGame_Level()
{
    ZoneScoped;

    if (strstr(Core.Params, "-nes_texture_storing"))
        GEnv.Render->ResourcesStoreNecessaryTextures();
    xr_delete(pLevel);

    // Render-level unload
    GEnv.Render->level_Unload();
    xr_delete(m_pCameras);
    // Unregister
    Device.seqRender.Remove(this);
    Device.seqFrame.Remove(this);
    CCameraManager::ResetPP();
    ///////////////////////////////////////////
    DefaultSoundScene = g_pGamePersistent->m_pSound;
    GEnv.Sound->destroy_scene(Sound);
#ifndef MASTER_GOLD
    GEnv.Render->ResourcesDumpMemoryUsage();
#endif

    u32 m_base = 0, c_base = 0, m_lmaps = 0, c_lmaps = 0;
    if (GEnv.Render)
        GEnv.Render->ResourcesGetMemoryUsage(m_base, c_base, m_lmaps, c_lmaps);

    Msg("* [ D3D ]: textures[%d K]", (m_base + m_lmaps) / 1024);
}

void IGame_Level::net_Stop()
{
    ZoneScoped;

    // XXX: why update 6 times?
    for (int i = 0; i < 6; i++)
        Objects.Update(false);
    // Destroy all objects
    Objects.Unload();
    IR_Release();

    bReady = false;
}

//-------------------------------------------------------------------------------------------
// extern CStatTimer tscreate;

namespace
{
void build_callback(Fvector* V, u32 Vcnt, CDB::TRI* T, u32 Tcnt, void* params)
{
    g_pGameLevel->Load_GameSpecific_CFORM(T, Tcnt);
}

void serialize_callback(IWriter& writer)
{
    g_pGameLevel->Load_GameSpecific_CFORM_Serialize(writer);
}

bool deserialize_callback(IReader& reader)
{
    return g_pGameLevel->Load_GameSpecific_CFORM_Deserialize(reader);
}

void remapping_materials_callback(CDB::TRI* T, u32 Tcnt, xr_map<u16, shared_str>& gameMtls)
{
    g_pGameLevel->Load_GameSpecific_CFORM_SetMaterials(T, Tcnt, gameMtls);
}
} // namespace

bool IGame_Level::Load(u32 dwNum)
{
    ZoneScoped;

    // Initialize level data
    g_pGamePersistent->Level_Set(dwNum);
    string_path temp;
    if (!FS.exist(temp, "$level$", "level.ltx"))
        xrDebug::Fatal(DEBUG_INFO, "Can't find level configuration file '%s'.", temp);
    pLevel = xr_new<CInifile>(temp);

    // Open
    g_pGamePersistent->LoadTitle("st_opening_stream");
    IReader* LL_Stream = FS.r_open("$level$", "level");
    IReader& fs = *LL_Stream;

    // Header
    hdrLEVEL H;
    fs.r_chunk_safe(fsL_HEADER, &H, sizeof(H));
    R_ASSERT2(XRCL_PRODUCTION_VERSION == H.XRLC_version, "Incompatible level version.");

    // CForms
    g_pGamePersistent->LoadTitle("st_loading_cform");

    ObjectSpace.Load(build_callback, serialize_callback, deserialize_callback, remapping_materials_callback);
    g_pGamePersistent->SpatialSpace.initialize(ObjectSpace.GetBoundingVolume());
    g_pGamePersistent->SpatialSpacePhysic.initialize(ObjectSpace.GetBoundingVolume());

    Sound->set_geometry_occ(ObjectSpace.GetStaticModel(), ObjectSpace.GetBoundingVolume());
    Sound->set_handler([](const ref_sound& S, float range)
    {
        if (g_pGameLevel && S && S->feedback)
            g_pGameLevel->SoundEvent_Register(S, range);
    });

    // Render-level Load
    GEnv.Render->level_Load(LL_Stream);
    // tscreate.FrameEnd ();
    // Msg ("* S-CREATE: %f ms, %d times",tscreate.result,tscreate.count);

    // Objects
    g_pGamePersistent->Environment().mods_load();
    R_ASSERT(Load_GameSpecific_Before());
    Objects.Load();
    //. ANDY R_ASSERT (Load_GameSpecific_After ());

    // Done
    FS.r_close(LL_Stream);
    bReady = true;

    if (!GEnv.isDedicatedServer)
    {
        IR_Capture();
        Device.seqRender.Add(this);
    }

    Device.seqFrame.Add(this);
    return true;
}

int psNET_DedicatedSleep = 5;
void IGame_Level::OnRender()
{
    ZoneScoped;

    if (GEnv.isDedicatedServer)
    {
        Sleep(psNET_DedicatedSleep);
        return;
    }

    Device.m_SecondViewport.SetSVPFrameDelay((u8)clampr(ps_r__svp_frame_delay, 0, 255));

    // if (_abs(Device.fTimeDelta)<EPS_S) return;

    // Level render, only when no client output required
    GEnv.Render->Calculate();
    GEnv.Render->Render();

    // IsSVPFrame(): SVP active + dwFrame % delay == 0; delay 0 from cvar → every frame (see CSecondVPParams::IsSVPFrame).
    if (ps_r__dedicated_second_vp && g_pGameLevel && Device.m_SecondViewport.IsSVPFrame())
    {
        IMainMenu* pMainMenu = g_pGamePersistent ? g_pGamePersistent->m_pMainMenu : nullptr;
        const bool bMenu = pMainMenu && pMainMenu->CanSkipSceneRendering();
        if (!bMenu)
        {
            // Tracers on the main (full) framebuffer while Device still matches the first pass.
            g_pGameLevel->RenderBulletTracersForMainViewport();

            if (g_pGameLevel->Cameras().BeginSecondViewportRender())
            {
                Device.m_SecondViewport.SetSecondCalculatePass(true);
                GEnv.Render->Calculate();
                Device.m_SecondViewport.SetSecondCalculatePass(false);
                GEnv.Render->RenderSecondViewport();
                // Tracers into rt_secondVP (current RT after phase_combine) with scope camera matrices.
                g_pGameLevel->RenderBulletTracersForSecondViewport();
                g_pGameLevel->Cameras().EndSecondViewportRender();
                // Second pass ends on rt_secondVP + small viewport; restore swapchain before HUD().RenderUI().
                GEnv.Render->BindBackbufferForUI();
            }
        }
    }

    // Font
    // pApp->pFontSystem->SetSizeI(0.023f);
    // pApp->pFontSystem->OnRender();
}

void IGame_Level::OnFrame()
{
    ZoneScopedN("lvl_OnFrame");

    // `SoundEvent_Dispatch` runs on the dedicated `GameThread` worker (see `EngineThreading.cpp`), after
    // `seqFrame` and before `Engine.Sheduler.Update`, matching ixray ordering. Lua-bound `feel_sound_new`
    // stays off unrelated `TaskScheduler` parallel paths (e.g. object list / ray batch).

    VERIFY(bReady);
    {
        ZoneScopedN("lvl_objs");
        Objects.Update(false);
    }
    {
        ZoneScopedN("lvl_hud");
        pHUD->OnFrame();
    }

    if (Sounds_Random.size() && (Device.dwTimeGlobal > Sounds_Random_dwNextTime))
    {
        ZoneScopedN("lvl_amb_random");
        Sounds_Random_dwNextTime = Device.dwTimeGlobal + ::Random.randI(10000, 20000);
        Fvector pos;
        pos.random_dir().normalize().mul(::Random.randF(30, 100)).add(Device.vCameraPosition);
        int id = ::Random.randI(Sounds_Random.size());
        if (Sounds_Random_Enabled)
        {
            Sounds_Random[id].play_at_pos(0, pos, 0);
            Sounds_Random[id].set_volume(1.f);
            Sounds_Random[id].set_range(10, 200);
        }
    }
}

void IGame_Level::DumpStatistics(IGameFont& font, IPerformanceAlert* alert) { Objects.DumpStatistics(font, alert); }
// ==================================================================================================

void CServerInfo::AddItem(pcstr name_, pcstr value_, u32 color_)
{
    shared_str s_name(name_);
    AddItem(s_name, value_, color_);
}

void CServerInfo::AddItem(shared_str& name_, pcstr value_, u32 color_)
{
    SItem_ServerInfo it;
    // shared_str s_name = CStringTable().translate( name_ );

    // xr_strcpy( it.name, s_name.c_str() );
    xr_strcpy(it.name, name_.c_str());
    xr_strcat(it.name, " = ");
    xr_strcat(it.name, value_);
    it.color = color_;

    if (data.size() < max_item)
    {
        data.push_back(it);
    }
}

void IGame_Level::SetEntity(IGameObject* O)
{
    if (pCurrentEntity)
        pCurrentEntity->On_LostEntity();

    if (O)
        O->On_SetEntity();

    pCurrentEntity = pCurrentViewEntity = O;
}

void IGame_Level::SetViewEntity(IGameObject* O)
{
    if (pCurrentViewEntity)
        pCurrentViewEntity->On_LostEntity();

    if (O)
        O->On_SetEntity();

    pCurrentViewEntity = O;
}

// B-1: AI sound-reaction coalescing. Rapid sounds from the same source (full-auto gunfire) re-alert the
// same listeners every ~100ms; skip the expensive listener q_box + per-listener occlusion if this source
// already propagated within the window. 0 = off (kill-switch).
int g_snd_ai_coalesce_ms = 180;

// Sound AI optimization: per-frame budget, burst coalescence, occlusion skip
int g_snd_ai_budget_per_frame = 100; // max fully-processed sound events per frame
int g_snd_ai_budget_enable = 1; // kill-switch for budget
int g_snd_ai_priority_only_beyond_budget = 1; // after budget exhausted, only allow combat sounds
float g_snd_ai_occlusion_skip_threshold = 0.15f; // skip occlusion below this power
float g_snd_ai_occlusion_skip_default = 0.3f; // conservative occlusion replacement
int g_snd_ai_burst_coalesce_time_ms = 50; // burst coalescence window (ms)
float g_snd_ai_burst_coalesce_dist = 5.0f; // burst coalescence radius (meters)
int g_snd_ai_burst_coalesce_enable = 1; // kill-switch for burst coalescence

// Per-frame budget state
static u32 s_snd_ai_budget_counter = 0;
static u32 s_snd_ai_budget_last_frame = u32(-1);

// Burst coalescence ring buffer: recent events by (pos, type, time)
struct SndBurstEntry
{
    Fvector pos;
    u32 type;
    u32 time_ms;
};
static SndBurstEntry s_snd_burst_ring[16] = {};
static int s_snd_burst_ring_idx = 0;

// Priority sound type mask: combat-critical sounds that bypass budget gate
static const u32 SND_PRIORITY_TYPE_MASK =
    SOUND_TYPE_WEAPON_SHOOTING | SOUND_TYPE_WEAPON_BULLET_HIT |
    SOUND_TYPE_MONSTER_ATTACKING | SOUND_TYPE_MONSTER_DYING |
    SOUND_TYPE_OBJECT_EXPLODING;

void IGame_Level::SoundEvent_Register(const ref_sound& S, float range)
{
    if (!g_bLoaded)
        return;
    if (!S)
        return;
    if (S->g_object && S->g_object->getDestroy())
    {
        S->g_object = 0;
        return;
    }
    if (0 == S->feedback)
        return;

    // B-1: coalesce rapid same-source AI sound propagation (see g_snd_ai_coalesce_ms). Lock-free: events
    // run on one thread; an aligned u32 store is atomic, and a missed/extra coalesce is harmless.
    if (g_snd_ai_coalesce_ms > 0 && S->g_object)
    {
        static u32 s_last_ai_snd_ms[0x10000] = {}; // [source object id] -> last AI-propagation time (ms)
        const u16 sid = S->g_object->ID();
        R_ASSERT(sid < std::size(s_last_ai_snd_ms));
        const u32 now = Device.dwTimeGlobal;
        const u32 last = s_last_ai_snd_ms[sid];
        if (last && now - last < u32(g_snd_ai_coalesce_ms))
            return; // this source already alerted nearby listeners just now -> skip redundant query
        s_last_ai_snd_ms[sid] = now;
    }

    // Burst coalescence: merge nearby same-type sounds within a short window (e.g. full-auto burst).
    // Independent from per-source coalescence above — handles different source objects at close positions.
    if (g_snd_ai_burst_coalesce_enable && S->g_type && S->feedback)
    {
        const u32 now = Device.dwTimeGlobal;
        Fvector eff_pos = S->feedback->get_params()->position;
        if (S->feedback->is_2D())
            eff_pos.add(GEnv.Sound->listener_position());

        for (int i = 0; i < 16; i++)
        {
            if (s_snd_burst_ring[i].time_ms == 0)
                continue;
            if (now - s_snd_burst_ring[i].time_ms > u32(g_snd_ai_burst_coalesce_time_ms))
                continue;
            if (s_snd_burst_ring[i].type != u32(S->g_type))
                continue;
            if (eff_pos.distance_to(s_snd_burst_ring[i].pos) > g_snd_ai_burst_coalesce_dist)
                continue;
            return; // burst-coalesced: a similar sound was already propagated nearby
        }
    }

    // Per-frame budget: cap the number of fully-processed (q_box + occlusion) sound events.
    // Beyond budget, only combat-critical sounds (weapon fire, explosions, monster attacks) pass through.
    if (g_snd_ai_budget_enable)
    {
        if (Device.dwFrame != s_snd_ai_budget_last_frame)
        {
            s_snd_ai_budget_counter = 0;
            s_snd_ai_budget_last_frame = Device.dwFrame;
            s_snd_burst_ring_idx = 0;
            ZeroMemory(s_snd_burst_ring, sizeof(s_snd_burst_ring));
        }

        const bool is_priority = (u32(S->g_type) & SND_PRIORITY_TYPE_MASK) != 0;
        if (s_snd_ai_budget_counter >= u32(g_snd_ai_budget_per_frame))
        {
            if (g_snd_ai_priority_only_beyond_budget && !is_priority)
                return; // budget exhausted, skip non-priority sounds
        }
        s_snd_ai_budget_counter++;
    }

    // Record burst-coalescence entry after all gates pass
    if (g_snd_ai_burst_coalesce_enable && S->g_type && S->feedback)
    {
        const u32 now = Device.dwTimeGlobal;
        Fvector eff_pos = S->feedback->get_params()->position;
        if (S->feedback->is_2D())
            eff_pos.add(GEnv.Sound->listener_position());

        s_snd_burst_ring[s_snd_burst_ring_idx].pos = eff_pos;
        s_snd_burst_ring[s_snd_burst_ring_idx].type = u32(S->g_type);
        s_snd_burst_ring[s_snd_burst_ring_idx].time_ms = now;
        s_snd_burst_ring_idx = (s_snd_burst_ring_idx + 1) & 15;
    }

    clamp(range, 0.1f, 500.f);

    const CSound_params* p = S->feedback->get_params();
    Fvector snd_position = p->position;
    if (S->feedback->is_2D())
    {
        snd_position.add(GEnv.Sound->listener_position());
    }

    VERIFY(p && _valid(range));
    range = std::min(range, p->max_ai_distance);
    VERIFY(_valid(snd_position));
    VERIFY(_valid(p->max_ai_distance));
    VERIFY(_valid(p->volume));

    // Query objects
    Fvector bb_size = {range, range, range};
    g_pGamePersistent->SpatialSpace.q_box(snd_ER, 0, STYPE_REACTTOSOUND, snd_position, bb_size);

    // Iterate
    for (auto& it : snd_ER)
    {
        Feel::Sound* L = it->dcast_FeelSound();
        if (0 == L)
            continue;
        IGameObject* CO = it->dcast_GameObject();
        VERIFY(CO);
        if (CO->getDestroy())
            continue;

        // Energy and signal
        VERIFY(_valid(it->GetSpatialData().sphere.P));
        const float dist = snd_position.distance_to(it->GetSpatialData().sphere.P);
        if (dist > p->max_ai_distance)
            continue;
        VERIFY(_valid(dist));
        VERIFY2(!fis_zero(p->max_ai_distance), S->handle->file_name());
        float Power = (1.f - dist / p->max_ai_distance) * p->volume;
        VERIFY(_valid(Power));
        if (Power > EPS_S)
        {
            // Occlusion skip: for sounds already below hearing threshold, skip the expensive
            // per-listener ray-trace and use a conservative occlusion default instead.
            float occ;
            if (g_snd_ai_occlusion_skip_threshold > 0.f && Power < g_snd_ai_occlusion_skip_threshold)
            {
                occ = g_snd_ai_occlusion_skip_default;
            }
            else
            {
                occ = Sound->get_occlusion_to(it->GetSpatialData().sphere.P, snd_position);
            }
            VERIFY(_valid(occ));
            Power *= occ;
            if (Power > EPS_S)
            {
                _esound_delegate D = {L, S, Power};
                snd_Events.push_back(D);
            }
        }
    }
    snd_ER.clear();
}

void IGame_Level::SoundEvent_Dispatch()
{
    ZoneScopedN("IGame_Level::SoundEvent_Dispatch");
    const size_t dispatch_count = snd_Events.size();
    ZoneTextF("%zu events", dispatch_count);

    if (dispatch_count == 0)
        return;

    const auto dispatch_one = [](const _esound_delegate& D)
    {
        VERIFY(D.dest && D.source);
        if (!D.source->feedback)
            return;
        ZoneScopedN("feel_sound_new");
        D.dest->feel_sound_new(D.source->g_object, D.source->g_type, D.source->g_userdata,
            D.source->feedback->is_2D() ? Device.vCameraPosition : D.source->feedback->get_params()->position,
            D.power);
    };

    // Single-threaded only: feel_sound_new -> CScriptEntity::sound_callback mutates m_saved_sounds and reads
    // luabind::object (Lua is not thread-safe). Parallel dispatch via TaskScheduler caused heap corruption.
    while (!snd_Events.empty())
    {
        const _esound_delegate D = std::move(snd_Events.back());
        snd_Events.pop_back();
        dispatch_one(D);
    }
}

// Lain: added
void IGame_Level::SoundEvent_OnDestDestroy(Feel::Sound* obj)
{
    struct rem_pred
    {
        rem_pred(Feel::Sound* obj) : m_obj(obj) {}
        bool operator()(const _esound_delegate& d) { return d.dest == m_obj; }
    private:
        Feel::Sound* m_obj;
    };

    snd_Events.erase(std::remove_if(snd_Events.begin(), snd_Events.end(), rem_pred(obj)), snd_Events.end());
}
