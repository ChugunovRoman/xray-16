#include "pch_script.h"
#include "addon_object_events.h"
#include "addon_event_bus.h"
#include <tracy/Tracy.hpp>
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "xrCore/Profiling/IniLoadStats.hpp"
#include "GameObject.h"

#include "Include/xrRender/RenderVisual.h"
#include "xrPhysics/PhysicsShell.h"
#include "ai_space.h"
#include "CustomMonster.h"
#include "PhysicObject.h"
#include "HangingLamp.h"
#include "xrPhysics/PhysicsShell.h"
#include "game_sv_single.h"
#include "xrAICore/Navigation/level_graph.h"
#include "ph_shell_interface.h"
#include "script_game_object.h"
#include "xrServer_Objects_ALife.h"
#include "xrServer_Objects_ALife_Items.h"
#include "game_cl_base.h"
#include "object_factory.h"
#include "Include/xrRender/Kinematics.h"
#include "xrAICore/Navigation/ai_object_location.h"
#include "xrAICore/Navigation/ai_object_location_impl.h"
#include "xrAICore/Navigation/game_graph.h"
#include "ai_debug.h"
#include "xrEngine/IGame_Level.h"
#include "xrEngine/EngineThreading.h"
#include "Level.h"
#include "xrScriptEngine/script_callback_ex.h"
#include "xrEngine/profiler.h"
#include "xrPhysics/MathUtils.h"
#include "game_cl_base_weapon_usage_statistic.h"
#include "game_cl_mp.h"
#include "npc_cpp_profile.h"
#include "performance_cvars.h"
#include "xrAICore/Navigation/game_level_cross_table.h"
#include "ai_obstacle.h"
#include "magic_box3.h"
#include "animation_movement_controller.h"
#include "ai/stalker/ai_stalker.h"
#include "memory_manager.h"
#include "enemy_manager.h"
#include "Actor.h"
#include "xrEngine/xr_collide_form.h"
#include "script_game_object.h"
#include "xrScriptEngine/script_callback_ex.h"
#include "game_object_space.h"
#include "doors_door.h"
#include "doors.h"
#include "xrNetServer/NET_Messages.h"
#include "smart_zone.h"
#include "xrScriptEngine/script_engine.hpp"

extern MagicBox3 MagicMinBox(int iQuantity, const Fvector* akPoint);

#ifdef DEBUG
#include "debug_renderer.h"
#include "PHDebug.h"
#endif

static const float base_spu_epsP = 0.05f;
static const float base_spu_epsR = 0.05f;

CGameObject::CGameObject() : SpatialBase(g_pGamePersistent->SpatialSpace), scriptBinder(this)
{
    dwFrame_AsCrow = u32(-1);
    Props.storage = 0;
    Parent = nullptr;
    NameObject = nullptr;
    NameSection = nullptr;
    NameVisual = nullptr;
#ifdef DEBUG
    shedule.dbg_update_shedule = u32(-1) / 2;
    dbg_update_cl = u32(-1) / 2;
#endif
    // ~IGameObject ctor
    // CUsableScriptObject init
    m_bNonscriptUsable = true;
    set_tip_text_default();
    //
    m_ai_obstacle = 0;

    init();
    //-----------------------------------------
    m_bCrPr_Activated = false;
    m_dwCrPr_ActivationStep = 0;
    m_spawn_time = 0;
    m_ai_location = !GEnv.isDedicatedServer ? xr_new<CAI_ObjectLocation>() : 0;
    m_server_flags.one();

    m_callbacks = xr_new<CALLBACK_MAP>();
    m_anim_mov_ctrl = 0;
}

CGameObject::~CGameObject()
{
    VERIFY(!animation_movement());
    VERIFY(!m_ini_file);
    VERIFY(!m_lua_game_object);
    VERIFY(!m_spawned);
    xr_delete(m_ai_location);
    xr_delete(m_callbacks);
    xr_delete(m_ai_obstacle);
    cNameVisual_set(0);
    cName_set(0);
    cNameSect_set(0);
}

// A corpse may only be throttled once its ragdoll has actually come to rest. While the shell is still enabled the body
// is falling or sliding, and skipping MakeMeCrow / spatial_update there leaves it hanging in the air with a stale
// level_vertex_id. ODE's own auto-disable (xrPhysics/PHDisabling.cpp) decides when the body has settled.
static bool corpse_ragdoll_settled(CEntityAlive* entity_alive)
{
    if (!entity_alive)
        return false;

    CPhysicsShell* shell = entity_alive->PPhysicsShell();
    return !shell || !shell->isEnabled();
}

void CGameObject::MakeMeCrow()
{
    if (m_is_crow.load(std::memory_order_relaxed))
    {
        PERF_INC(crow_early_already);
        return;
    }
    if (!processing_enabled())
    {
        PERF_INC(crow_early_processing);
        return;
    }

    // Optimization: skip MakeMeCrow for corpses whose ragdoll has already settled.
    // This reduces post-battle CPU tail significantly.
    if (CEntityAlive* entity_alive = cast_entity_alive())
    {
        if (!entity_alive->g_Alive() && corpse_ragdoll_settled(entity_alive))
        {
            const u32 death_time = entity_alive->GetLevelDeathTime();
            if (death_time && (Device.dwTimeGlobal - death_time > 3000))
            {
                PERF_INC(crow_early_corpse);
                return;
            }
        }
    }

    u32 const device_frame_id = Device.dwFrame;
    u32 object_frame_id = dwFrame_AsCrow.load(std::memory_order_relaxed);
    if (object_frame_id == device_frame_id)
    {
        PERF_INC(crow_early_same_frame);
        return;
    }

    if (dwFrame_AsCrow.compare_exchange_strong(object_frame_id, device_frame_id, std::memory_order_acq_rel))
    {
        VERIFY(dwFrame_AsCrow.load(std::memory_order_acquire) == device_frame_id);
        const u8 src = xray::perf::TlCrowSrc() ? xray::perf::TlCrowSrc()
                                               : xray::perf::CrowSrcFromPass(xray::perf::TlPass());
        m_crow_packed.store(xray::perf::PackCrow(src, device_frame_id), std::memory_order_relaxed);
        xray::perf::IncCrowSrc(src);
        g_pGameLevel->Objects.o_crow(this);
        m_is_crow.store(true, std::memory_order_relaxed);
    }
}

void CGameObject::cName_set(shared_str N) { NameObject = N; }
void CGameObject::cNameSect_set(shared_str N) { NameSection = N; }
void CGameObject::cNameVisual_set(shared_str N)
{
    GW_OVERLAP_HAZARD("CGameObject::cNameVisual_set");
    // check if equal
    if (N.c_str() && NameVisual.c_str())
        if (N == NameVisual)
            return;
    // replace model
    if (N.c_str() && N[0])
    {
        IRenderVisual* old_v = renderable.visual;
        shared_str const prev_visual_name = NameVisual;
        NameVisual = N;
        shared_str model_suffix = pSettings->read_if_exists<pcstr>(NameSection.c_str(), "model_cache_suffix", "");
        // Sparse indices: only listed world_material_N lines are applied (no need for 1..N-1 placeholders).
        constexpr u16 kMaxWeaponSkinMaterialSlots = 64;
        auto load_world_materials = [&](const char* format) {
            shared_str line_name;
            shared_str material_value;
            shared_str material_key;

            for (u16 index = 1; index <= kMaxWeaponSkinMaterialSlots; ++index)
            {
                line_name = make_string(format, index).c_str();
                if (!pSettings->line_exist(NameSection.c_str(), line_name.c_str()))
                    continue;

                string256 dds_path = "", shader_name = "";
                material_value = pSettings->r_string(NameSection.c_str(), line_name.c_str());
                _GetItem(material_value.c_str(), 0, dds_path);
                _GetItem(material_value.c_str(), 1, shader_name);
                string256 low_name;
                xr_strcpy(low_name, NameVisual.c_str());
                if (strext(low_name))
                    *strext(low_name) = 0;
                material_key = make_string("%s:%d%s", low_name, index, model_suffix.c_str()).c_str();

                GEnv.Render->emplace_texture_replacements(material_key, dds_path);
            }
        };

        load_world_materials("world_material_%d");
        renderable.visual = GEnv.Render->model_Create(N.c_str(), model_suffix.c_str());
        if (!renderable.visual)
        {
            Msg("! [%s] model_Create failed for visual [%s]", cName().c_str(), N.c_str());
            NameVisual = prev_visual_name;
            renderable.visual = old_v;
            OnChangeVisual();
            return;
        }
        IKinematics* old_k = old_v ? old_v->dcast_PKinematics() : NULL;
        IKinematics* new_k = renderable.visual->dcast_PKinematics();
        /*
        if(old_k && new_k){
        new_k->Update_Callback = old_k->Update_Callback;
        new_k->Update_Callback_Param = old_k->Update_Callback_Param;
        }
        */
        if (old_k && new_k)
        {
            new_k->SetUpdateCallback(old_k->GetUpdateCallback());
            new_k->SetUpdateCallbackParam(old_k->GetUpdateCallbackParam());
        }
        GEnv.Render->model_Delete(old_v);
    }
    else
    {
        GEnv.Render->model_Delete(renderable.visual);
        NameVisual = nullptr;
    }
    OnChangeVisual();
}

void CGameObject::ReloadVisualFromDisk()
{
    if (GEnv.isDedicatedServer || !GEnv.Render)
        return;
    shared_str v = NameVisual;
    if (!v.size() || !v[0])
        return;
    // bDiscard: drop pool/base refs so mesh and skeleton changes on disk are picked up (same idea as HUD hot reload).
    IRenderVisual* rv = renderable.visual;
    if (rv)
    {
        GEnv.Render->model_Delete(rv, TRUE);
        renderable.visual = rv;
    }
    NameVisual = nullptr;
    OnChangeVisual();

    cNameVisual_set(v);
}

bool CGameObject::ShouldDeferToPostJoin()
{
    // IsGameThreadRunning: from KickGameLogicThread until the join before the UI, i.e. while DoRender may read the
    // objects; IsGameLogicThread then is true only on the dedicated Game thread (main runs the render).
    return Device.IsGameRenderOverlapFrame() && XRay::Engine::IsGameThreadRunning() &&
        XRay::Engine::IsGameLogicThread();
}

void CGameObject::PostJoinForObject(u16 id, std::function<void(CGameObject&)> fn)
{
    // By ID, not by pointer: the object is looked up again when the command runs
    Device.PostJoin([id, fn = std::move(fn)] {
        if (!g_pGameLevel)
            return;
        CGameObject* object = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
        if (!object || object->getDestroy())
            return;
        fn(*object);
    });
}

// flagging
void CGameObject::processing_activate()
{
    VERIFY3(255 != Props.bActiveCounter, "Invalid sequence of processing enable/disable calls: overflow", cName().c_str());
    Props.bActiveCounter++;
    if (!(Props.bActiveCounter - 1))
        g_pGameLevel->Objects.o_activate(this);
}

void CGameObject::processing_deactivate()
{
    VERIFY3(Props.bActiveCounter, "Invalid sequence of processing enable/disable calls: underflow", cName().c_str());
    Props.bActiveCounter--;
    if (!Props.bActiveCounter)
        g_pGameLevel->Objects.o_sleep(this);
}

void CGameObject::setEnabled(bool _enabled)
{
    if (_enabled)
    {
        Props.bEnabled = 1;
        if (CForm)
            spatial.type |= STYPE_COLLIDEABLE;
    }
    else
    {
        Props.bEnabled = 0;
        spatial.type &= ~STYPE_COLLIDEABLE;
    }
}

void CGameObject::setVisible(bool _visible)
{
    if (_visible)
    {
        // Parent should control object visibility itself (??????)
        Props.bVisible = 1;
        if (renderable.visual)
            spatial.type |= STYPE_RENDERABLE;
    }
    else
    {
        Props.bVisible = 0;
        spatial.type &= ~STYPE_RENDERABLE;
    }
}

void CGameObject::Center(Fvector& C) const
{
    VERIFY2(renderable.visual, cName().c_str());
    renderable.xform.transform_tiny(C, renderable.visual->getVisData().sphere.P);
}

float CGameObject::Radius() const
{
    VERIFY2(renderable.visual, cName().c_str());
    return renderable.visual->getVisData().sphere.R;
}

const Fbox& CGameObject::BoundingBox() const
{
    VERIFY2(renderable.visual, cName().c_str());
    return renderable.visual->getVisData().box;
}

void CGameObject::Load(LPCSTR section)
{
    // Name
    R_ASSERT(section);
    cName_set(section);
    cNameSect_set(section);
    // Visual and light-track
    if (pSettings->line_exist(section, "visual"))
    {
        string_path tmp;
        xr_strcpy(tmp, pSettings->r_string(section, "visual"));
        if (strext(tmp))
            *strext(tmp) = 0;
        xr_strlwr(tmp);
        cNameVisual_set(tmp);
    }
    setVisible(false);
    // ~
    ISpatial* self = smart_cast<ISpatial*>(this);
    if (self)
    {
        // #pragma todo("to Dima: All objects are visible for AI ???")
        // self->spatial.type	|=	STYPE_VISIBLEFORAI;
        self->GetSpatialData().type &= ~STYPE_REACTTOSOUND;
    }
}

void CGameObject::PostLoad(LPCSTR section) {}

void CGameObject::init()
{
    m_lua_game_object = 0;
    m_script_clsid = -1;
    m_ini_file = 0;
    m_spawned = false;
}

void CGameObject::reinit()
{
    m_visual_callback.clear();
    if (!GEnv.isDedicatedServer)
        ai_location().reinit();

    // clear callbacks
    for (auto it = m_callbacks->begin(); it != m_callbacks->end(); ++it)
        it->second.clear();
}

void CGameObject::reload(LPCSTR section)
{
    m_script_clsid = object_factory().script_clsid(CLS_ID);
}

namespace
{
// Stage D, W3-1: the lane and phase rules of motivator_binder:update (xr_motivator.script), in C++.
// Mask layout shared with the Lua side: bits 0..3 = medium, slow, switch, callback due; bits 4..5 = lane
// (0 near, 1 medium, 2 far); bit 30 = the mask is valid (the gate is on for this update).
constexpr u32 kGatePhaseMedium = 1u << 0;
constexpr u32 kGatePhaseSlow = 1u << 1;
constexpr u32 kGatePhaseSwitch = 1u << 2;
constexpr u32 kGatePhaseCallback = 1u << 3;
constexpr u32 kGateLaneShift = 4;
constexpr u32 kGateActive = 1u << 30;
constexpr float kGateNearDistSqr = 35.f * 35.f;
constexpr float kGateMediumDistSqr = 80.f * 80.f;
constexpr float kGateFrustumDeg = 35.f;
constexpr u32 kGateBuckets = 8;

enum EGateLane : u32
{
    GateLaneNear = 0,
    GateLaneMedium = 1,
    GateLaneFar = 2,
};

// get_update_lane: an enemy or a talk -> near; no live actor -> far; distance to the actor; in the camera cone
// (npc_in_actor_frustrum: the angle between the camera direction and the actor -> NPC vector) -> medium
EGateLane gate_lane(CAI_Stalker& stalker)
{
    if (stalker.memory().enemy().selected() || stalker.IsTalking())
        return GateLaneNear;
    if (!g_actor || !g_actor->g_Alive())
        return GateLaneFar;
    Fvector to_npc;
    to_npc.sub(stalker.Position(), g_actor->Position());
    const float dist_sqr = to_npc.square_magnitude();
    if (dist_sqr <= kGateNearDistSqr)
        return GateLaneNear;
    if (dist_sqr <= kGateMediumDistSqr)
        return GateLaneMedium;
    const Fvector& cam = Device.vCameraDirection;
    const float denom = _sqrt(cam.square_magnitude() * dist_sqr);
    if (denom > 0.f)
    {
        const float cos_a = clampr(cam.dotproduct(to_npc) / denom, -1.f, 1.f);
        if (rad2deg(acosf(cos_a)) < kGateFrustumDeg)
            return GateLaneMedium;
    }
    return GateLaneFar;
}

// is_bucket_due: the first due time is spread over the interval by id % 8; then every interval, catching up
bool gate_due(u32& interval_slot, u32& next_slot, u16 id, u32 interval, u32 now)
{
    if (interval_slot != interval)
    {
        interval_slot = interval;
        const u32 buckets = std::min(kGateBuckets, std::max(interval, 1u));
        next_slot = now + (interval * (id % buckets)) / buckets;
    }
    if (now < next_slot)
        return false;
    next_slot += interval;
    return true;
}
} // namespace

// Returns false when the Lua binder update may be skipped for this pass (the gate is on, the Lua binder reads the
// mask and no phase is due). The mask is kept for binder_phase_mask().
bool CGameObject::binder_gate_update(u32 now)
{
    m_binder_gate.mask = 0;
    if (!npc_perf_motivator_native_gate)
    {
        // Forget the phase timers while the gate is off: switched on again, they restart (as the Lua timers do
        // when it goes off) instead of catching up every missed interval with a due phase on each update.
        std::fill(std::begin(m_binder_gate.interval), std::end(m_binder_gate.interval), 0u);
        return true;
    }
    CAI_Stalker* stalker = cast_stalker();
    if (!stalker || !stalker->g_Alive())
        return true; // dead NPCs keep their own Lua path (loot tips, the dead switch timer)

    const EGateLane lane = gate_lane(*stalker);
    BinderGate& g = m_binder_gate;
    u32 mask = kGateActive | (static_cast<u32>(lane) << kGateLaneShift);

    // should_run_medium: every update near, else by interval
    if (lane == GateLaneNear ||
        gate_due(g.interval[0], g.next[0], ID(),
            lane == GateLaneMedium ? npc_perf_motivator_medium_interval : npc_perf_motivator_far_interval, now))
        mask |= kGatePhaseMedium;
    const u32 slow = lane == GateLaneNear ? npc_perf_motivator_slow_near_interval :
        lane == GateLaneMedium ? npc_perf_motivator_slow_medium_interval : npc_perf_motivator_slow_far_interval;
    if (gate_due(g.interval[1], g.next[1], ID(), slow, now))
        mask |= kGatePhaseSlow;
    const u32 sw = lane == GateLaneNear ? npc_perf_motivator_switch_near_interval :
        lane == GateLaneMedium ? npc_perf_motivator_switch_medium_interval : npc_perf_motivator_switch_far_interval;
    if (gate_due(g.interval[2], g.next[2], ID(), sw, now))
        mask |= kGatePhaseSwitch;
    const u32 cb = lane == GateLaneNear ? npc_perf_motivator_callback_near_interval :
        lane == GateLaneMedium ? npc_perf_motivator_callback_medium_interval :
                                 npc_perf_motivator_callback_far_interval;
    if (gate_due(g.interval[3], g.next[3], ID(), cb, now))
        mask |= kGatePhaseCallback;

    g.mask = mask;
    // Only a binder that reads the mask may be skipped: another Lua binder on a stalker keeps every update.
    return !g.consumer || (mask & (kGatePhaseMedium | kGatePhaseSlow | kGatePhaseSwitch | kGatePhaseCallback)) != 0;
}

void CGameObject::net_Destroy()
{
#ifdef DEBUG
    if (psAI_Flags.test(aiDestroy))
        Msg("Destroying client object [%d][%s][%x]", ID(), cName().c_str(), this);
#endif

    VERIFY(m_spawned);
    // npc_on_net_destroy / actor_on_net_destroy: before the Lua binder (db.storage of the NPC is still there) and
    // before Objects.net_Unregister below, or the subscribers would get nil for the object (net_Find).
    gw::addons::objevents::ObjectNetDestroy(this);
    if (m_anim_mov_ctrl)
        destroy_anim_mov_ctrl();

    xr_delete(m_ini_file);

    if (Visual() && smart_cast<IKinematics*>(Visual()))
        smart_cast<IKinematics*>(Visual())->Callback(0, 0);
    //
    VERIFY(getDestroy());
    xr_delete(CForm);
    if (register_schedule())
        shedule_unregister();
    spatial_unregister();
    // setDestroy (true); // commented in original src
    // remove visual
    cNameVisual_set(0);
    // ~
    setReady(FALSE);

    if (Level().IsDemoPlayStarted() && ID() == u16(-1))
    {
        Msg("Destroying demo_spectator object");
    }
    else
    {
        g_pGameLevel->Objects.net_Unregister(this);
    }

    if (this == Level().CurrentEntity())
    {
        if (!Level().IsDemoPlayStarted())
        {
            Level().SetControlEntity(0);
        }
        Level().SetEntity(0); // do not switch !!!
    }

    Level().RemoveObject_From_4CrPr(this);

    //.	Parent									= 0;

    scriptBinder.net_Destroy();
    // Plugin subscriptions to this object end with it (GWP_SUBSCRIBE_OBJECT): after the Lua binder, whose
    // net_destroy may still send events about the object.
    gw::addons::events::RemoveObjectSubscriptions(ID());
    m_script_clsid = -1;

    xr_delete(m_lua_game_object);
    m_spawned = false;
}

void CGameObject::OnEvent(NET_Packet& P, u16 type)
{
    switch (type)
    {
    case GE_HIT:
    case GE_HIT_STATISTIC:
    {
        /*
                    u16				id,weapon_id;
                    Fvector			dir;
                    float			power, impulse;
                    s16				element;
                    Fvector			position_in_bone_space;
                    u16				hit_type;
                    float			ap = 0.0f;

                    P.r_u16			(id);
                    P.r_u16			(weapon_id);
                    P.r_dir			(dir);
                    P.r_float		(power);
                    P.r_s16			(element);
                    P.r_vec3		(position_in_bone_space);
                    P.r_float		(impulse);
                    P.r_u16			(hit_type);	//hit type
                    if ((ALife::EHitType)hit_type == ALife::eHitTypeFireWound)
                    {
                        P.r_float	(ap);
                    }

                    IGameObject*	Hitter = Level().Objects.net_Find(id);
                    IGameObject*	Weapon = Level().Objects.net_Find(weapon_id);

                    SHit	HDS = SHit(power, dir, Hitter, element, position_in_bone_space, impulse,
           (ALife::EHitType)hit_type, ap);
        */
        SHit HDS;
        HDS.PACKET_TYPE = type;
        HDS.Read_Packet_Cont(P);
        //			Msg("Hit received: %d[%d,%d]", HDS.whoID, HDS.weaponID, HDS.BulletID);
        IGameObject* Hitter = Level().Objects.net_Find(HDS.whoID);
        IGameObject* Weapon = Level().Objects.net_Find(HDS.weaponID);
        HDS.who = Hitter;
        if (!HDS.who)
        {
            Msg("! ERROR: hitter object [%d] is NULL on client.", HDS.whoID);
        }
        //-------------------------------------------------------
        switch (HDS.PACKET_TYPE)
        {
        case GE_HIT_STATISTIC:
        {
            if (GameID() != eGameIDSingle)
                Game().m_WeaponUsageStatistic->OnBullet_Check_Request(&HDS);
        }
        break;
        }
        SetHitInfo(Hitter, Weapon, HDS.bone(), HDS.p_in_bone_space, HDS.dir);
        Hit(&HDS);
        //---------------------------------------------------------------------------
        if (GameID() != eGameIDSingle)
        {
            Game().m_WeaponUsageStatistic->OnBullet_Check_Result(false);
        }
        //---------------------------------------------------------------------------
    }
    break;
    case GE_DESTROY:
    {
        if (H_Parent())
        {
            Msg("! ERROR (GameObject): GE_DESTROY arrived to object[%d][%s], that has parent[%d][%s], frame[%d]", ID(),
                cNameSect().c_str(), H_Parent()->ID(), H_Parent()->cName().c_str(), Device.dwFrame);

            // This object will be destroy on call function <H_Parent::Destroy>
            // or it will be call <H_Parent::Reject>  ==>  H_Parent = NULL
            // !!! ___ it is necessary to be check!
            break;
        }
#ifdef MP_LOGGING
        Msg("--- Object: GE_DESTROY of [%d][%s]", ID(), cNameSect().c_str());
#endif // MP_LOGGING

        setDestroy(TRUE);
        //			MakeMeCrow		();
    }
    break;
    }
}

void VisualCallback(IKinematics* tpKinematics);

bool CGameObject::net_Spawn(CSE_Abstract* DC)
{
    VERIFY(!m_spawned);
    m_spawned = true;
    m_spawn_time = Device.dwFrame;
    m_ai_obstacle = xr_new<ai_obstacle>(this);

    CSE_Abstract* E = (CSE_Abstract*)DC;
    VERIFY(E);

    const CSE_Visual* visual = smart_cast<const CSE_Visual*>(E);
    if (visual)
    {
        {
            // Spawn stage 0 (plans/spawn_optimization/SPEC.md): CGameObject::net_Spawn by phase, go_spawn/* + spawn/go/*
            PERF_STEP("go_spawn/visual", spawn_go_visual);
            cNameVisual_set(visual_name(E));
        }
        if (visual->flags.test(CSE_Visual::flObstacle))
        {
            ISpatial* self = smart_cast<ISpatial*>(this);
            self->GetSpatialData().type |= STYPE_OBSTACLE;
        }
    }

    // Naming
    cName_set(E->s_name);
    cNameSect_set(E->s_name);
    if (E->name_replace()[0])
        cName_set(E->name_replace());

    bool demo_spectator = false;

    if (Level().IsDemoPlayStarted() && E->ID == u16(-1))
    {
        Msg("* Spawning demo spectator ...");
        demo_spectator = true;
    }
    else
    {
        //Alundaio:
        //R_ASSERT(Level().Objects.net_Find(E->ID) == nullptr);
        if (const auto obj = Level().Objects.net_Find(E->ID))
        {
            Msg("! ERROR: CGameObject:net_spawn() Object with ID[%u] already exists! self=%s other=%s", E->ID, cName().c_str(), obj->cName().c_str());
            return false;
        }
        //-Alundaio
    }

    setID(E->ID);
    //	if (GameID() != eGameIDSingle)
    //		Msg ("CGameObject::net_Spawn -- object %s[%x] setID [%d]", *(E->s_name), this, E->ID);

    // XForm
    XFORM().setXYZ(E->o_Angle);
    Position().set(E->o_Position);
#ifdef DEBUG
    if (ph_dbg_draw_mask1.test(ph_m1_DbgTrackObject) && xr_stricmp(PH_DBG_ObjectTrackName(), cName().c_str()) == 0)
    {
        Msg("CGameObject::net_Spawn obj %s Position set from CSE_Abstract %f,%f,%f", PH_DBG_ObjectTrackName(),
            Position().x, Position().y, Position().z);
    }
#endif
    VERIFY(_valid(renderable.xform));
    VERIFY(!fis_zero(DET(renderable.xform)));
    CSE_ALifeObject* O = smart_cast<CSE_ALifeObject*>(E);
    if (O && xr_strlen(O->m_ini_string))
    {
        ZoneScopedN("go_spawn/custom_data_ini"); // the timer is ini/load/obj_custom_data (P20)
#pragma warning(push)
#pragma warning(disable : 4238)
        IReader reader((void*)((O->m_ini_string).c_str()), O->m_ini_string.size());
        INI_SOURCE(ObjectCustomData, E->s_name.c_str()); // P20: parsed on every net_Spawn (going online)
        m_ini_file = xr_new<CInifile>(&reader, FS.get_path("$game_config$")->m_Path);
#pragma warning(pop)
    }

    m_story_id = ALife::_STORY_ID(-1);
    if (O)
        m_story_id = O->m_story_id;

    // Net params
    setLocal(E->s_flags.is(M_SPAWN_OBJECT_LOCAL));
    if (Level().IsDemoPlay()) //&& OnClient())
    {
        if (!demo_spectator)
        {
            setLocal(FALSE);
        }
    };

    setReady(TRUE);
    if (!demo_spectator)
        g_pGameLevel->Objects.net_Register(this);

    m_server_flags.one();
    if (O)
    {
        m_server_flags = O->m_flags;
        if (O->m_flags.is(CSE_ALifeObject::flVisibleForAI))
            spatial.type |= STYPE_VISIBLEFORAI;
        else
            spatial.type.fetch_and(~STYPE_VISIBLEFORAI, std::memory_order_relaxed);
    }

    {
        PERF_STEP("go_spawn/reload", spawn_go_reload);
        reload(cNameSect().c_str());
    }
    if (!GEnv.isDedicatedServer)
    {
        PERF_STEP("go_spawn/script_reload", spawn_go_script_reload);
        scriptBinder.reload(cNameSect().c_str());
        gw::addons::objevents::ObjectBinder(gw::addons::objevents::EBinder::Init, this);
    }

    {
        PERF_STEP("go_spawn/reinit", spawn_go_reinit);
        reinit();
    }
    if (!GEnv.isDedicatedServer)
    {
        PERF_STEP("go_spawn/script_reinit", spawn_go_script_reinit);
        scriptBinder.reinit();
        gw::addons::objevents::ObjectBinder(gw::addons::objevents::EBinder::Reinit, this);
    }
#ifdef DEBUG
    if (ph_dbg_draw_mask1.test(ph_m1_DbgTrackObject) && xr_stricmp(PH_DBG_ObjectTrackName(), cName().c_str()) == 0)
    {
        Msg("CGameObject::net_Spawn obj %s After Script Binder reinit %f,%f,%f", PH_DBG_ObjectTrackName(), Position().x,
            Position().y, Position().z);
    }
#endif
    // load custom user data from server
    if (!E->client_data.empty())
    {
        PERF_STEP("go_spawn/net_load", spawn_go_net_load);
        //		Msg				("client data is present for object [%d][%s], load is processed",ID(),*cName());
        IReader ireader = IReader(&*E->client_data.begin(), E->client_data.size());
        net_Load(ireader);
    }
    else
    {
        //		Msg				("no client data for object [%d][%s], load is skipped",ID(),*cName());
    }

    // if we have a parent
    if (ai().get_level_graph())
    {
        PERF_STEP("go_spawn/ai_location", spawn_go_ai_location);
        if (E->ID_Parent == 0xffff)
        {
            CSE_ALifeObject* l_tpALifeObject = smart_cast<CSE_ALifeObject*>(E);
            if (l_tpALifeObject && ai().level_graph().valid_vertex_id(l_tpALifeObject->m_tNodeID))
                ai_location().level_vertex(l_tpALifeObject->m_tNodeID);
            else
            {
                CSE_Temporary* l_tpTemporary = smart_cast<CSE_Temporary*>(E);
                if (l_tpTemporary && ai().level_graph().valid_vertex_id(l_tpTemporary->m_tNodeID))
                    ai_location().level_vertex(l_tpTemporary->m_tNodeID);
            }

            if (l_tpALifeObject && ai().game_graph().valid_vertex_id(l_tpALifeObject->m_tGraphID))
                ai_location().game_vertex(l_tpALifeObject->m_tGraphID);

            validate_ai_locations(false);

            // validating position
            if (UsedAI_Locations() && ai().level_graph().inside(ai_location().level_vertex_id(), Position()) &&
                can_validate_position_on_spawn())
                Position().y = EPS_L +
                    ai().level_graph().vertex_plane_y(*ai_location().level_vertex(), Position().x, Position().z);
        }
        else
        {
            CSE_ALifeObject* const alife_object = smart_cast<CSE_ALifeObject*>(E);
            if (alife_object && ai().level_graph().valid_vertex_id(alife_object->m_tNodeID))
            {
                ai_location().level_vertex(alife_object->m_tNodeID);
                ai_location().game_vertex(alife_object->m_tGraphID);
            }
        }
    }
    //
    PositionStack.clear();
    VERIFY(_valid(renderable.xform));
    if (0 == Visual() && pSettings->line_exist(cNameSect(), "visual"))
    {
        PERF_STEP("go_spawn/visual", spawn_go_visual);
        cNameVisual_set(pSettings->r_string(cNameSect(), "visual"));
    }
    {
        PERF_STEP("go_spawn/cform_spatial_shedule", spawn_go_cform_spatial);
        if (0 == CForm)
        {
            if (pSettings->line_exist(cNameSect(), "cform"))
            {
                VERIFY3(NameVisual.c_str(), "Model isn't assigned for object, but cform requisted", cName().c_str());
                CForm = xr_new<CCF_Skeleton>(this);
            }
        }
        R_ASSERT(spatial.space);
        spatial_register();
        if (register_schedule())
            shedule_register();
        // reinitialize flags
        processing_activate();
    }
    setDestroy(false);
    {
        PERF_STEP("go_spawn/crow", spawn_go_crow);
        PERF_CROW_SRC(xray::perf::crow::Spawn);
        MakeMeCrow();
    }
    // ~
    m_bObjectRemoved = false;
    {
        PERF_STEP("go_spawn/spawn_supplies", spawn_go_supplies);
        spawn_supplies();
    }
#ifdef DEBUG
    if (ph_dbg_draw_mask1.test(ph_m1_DbgTrackObject) && xr_stricmp(PH_DBG_ObjectTrackName(), cName().c_str()) == 0)
    {
        Msg("CGameObject::net_Spawn obj %s Before CScriptBinder::net_Spawn %f,%f,%f", PH_DBG_ObjectTrackName(),
            Position().x, Position().y, Position().z);
    }
    BOOL ret;
    {
        PERF_STEP("go_spawn/script_net_spawn", spawn_go_script_net_spawn);
        ret = scriptBinder.net_Spawn(DC);
    }
    if (ret)
    {
        PERF_STEP("go_spawn/addon_events", spawn_go_addon_events);
        gw::addons::objevents::ObjectNetSpawn(this); // after the Lua binder: its db.storage entry and logic exist (B3.6)
        gw::addons::objevents::ObjectBinder(gw::addons::objevents::EBinder::NetSpawn, this);
    }
#else
    BOOL ret;
    {
        PERF_STEP("go_spawn/script_net_spawn", spawn_go_script_net_spawn);
        ret = scriptBinder.net_Spawn(DC);
    }
    if (ret)
    {
        PERF_STEP("go_spawn/addon_events", spawn_go_addon_events);
        gw::addons::objevents::ObjectNetSpawn(this); // after the Lua binder: its db.storage entry and logic exist (B3.6)
        gw::addons::objevents::ObjectBinder(gw::addons::objevents::EBinder::NetSpawn, this); // on_game_load (actor)
    }
    return ret;
#endif

#ifdef DEBUG
    if (ph_dbg_draw_mask1.test(ph_m1_DbgTrackObject) && xr_stricmp(PH_DBG_ObjectTrackName(), cName().c_str()) == 0)
    {
        Msg("CGameObject::net_Spawn obj %s Before CScriptBinder::net_Spawn %f,%f,%f", PH_DBG_ObjectTrackName(),
            Position().x, Position().y, Position().z);
    }
    return ret;
#endif
}

void CGameObject::net_Save(NET_Packet& net_packet)
{
    u32 position;
    // Msg("CGameObject::net_Save 1 obj=[%s] count=[%d] r_pos=[%d]", *cName(), net_packet.B.count, net_packet.r_pos);
    net_packet.w_chunk_open16(position);
    // Msg("CGameObject::net_Save 2 obj=[%s] count=[%d] r_pos=[%d] position=[%d]", *cName(), net_packet.B.count, net_packet.r_pos, position);
    save(net_packet);

// Script Binder Save ---------------------------------------
#ifdef DEBUG
    if (psAI_Flags.test(aiSerialize))
    {
        Msg(">> **** Save script object [%s] *****", cName().c_str());
        Msg(">> Before save :: packet position = [%u]", net_packet.w_tell());
    }

#endif

    scriptBinder.save(net_packet);

#ifdef DEBUG

    if (psAI_Flags.test(aiSerialize))
    {
        Msg(">> After save :: packet position = [%u]", net_packet.w_tell());
    }
#endif

    // ----------------------------------------------------------

    net_packet.w_chunk_close16(position);
}

void CGameObject::net_Load(IReader& ireader)
{
    load(ireader);

// Script Binder Load ---------------------------------------
#ifdef DEBUG
    if (psAI_Flags.test(aiSerialize))
    {
        Msg(">> **** Load script object [%s] *****", cName().c_str());
        Msg(">> Before load :: reader position = [%i]", ireader.tell());
    }

#endif

    scriptBinder.load(ireader);

#ifdef DEBUG

    if (psAI_Flags.test(aiSerialize))
    {
        Msg(">> After load :: reader position = [%i]", ireader.tell());
    }
#endif
// ----------------------------------------------------------
#ifdef DEBUG
    if (ph_dbg_draw_mask1.test(ph_m1_DbgTrackObject) && xr_stricmp(PH_DBG_ObjectTrackName(), cName().c_str()) == 0)
    {
        Msg("CGameObject::net_Load obj %s (loaded) %f,%f,%f", PH_DBG_ObjectTrackName(), Position().x, Position().y,
            Position().z);
    }

#endif
}

void CGameObject::save(NET_Packet& output_packet) {}
void CGameObject::load(IReader& input_packet) {}
void CGameObject::spawn_supplies()
{
    if (!spawn_ini() || ai().get_alife())
        return;

    if (!spawn_ini()->section_exist("spawn"))
        return;

    LPCSTR N, V;
    float p;
    bool bScope = false;
    bool bSilencer = false;
    bool bLauncher = false;

    for (u32 k = 0, j; spawn_ini()->r_line("spawn", k, &N, &V); k++)
    {
        VERIFY(xr_strlen(N));
        if (!pSettings->section_exist(N)) //Alundaio: Validate section exists
            continue;

        j = 1;
        p = 1.f;

        float f_cond = 1.0f;
        if (V && xr_strlen(V))
        {
            int n = _GetItemCount(V);
            string16 temp;
            if (n > 0)
                j = atoi(_GetItem(V, 0, temp)); // count

            if (NULL != strstr(V, "prob="))
                p = (float)atof(strstr(V, "prob=") + 5);
            if (fis_zero(p))
                p = 1.f;
            if (!j)
                j = 1;
            if (NULL != strstr(V, "cond="))
                f_cond = (float)atof(strstr(V, "cond=") + 5);
            bScope = (NULL != strstr(V, "scope"));
            bSilencer = (NULL != strstr(V, "silencer"));
            bLauncher = (NULL != strstr(V, "launcher"));
        }
        for (u32 i = 0; i < j; ++i)
        {
            if (::Random.randF(1.f) < p)
            {
                CSE_Abstract* A = Level().spawn_item(N, Position(), ai_location().level_vertex_id(), ID(), true);

                CSE_ALifeInventoryItem* pSE_InventoryItem = smart_cast<CSE_ALifeInventoryItem*>(A);
                if (pSE_InventoryItem)
                    pSE_InventoryItem->m_fCondition = f_cond;

                CSE_ALifeItemWeapon* W = smart_cast<CSE_ALifeItemWeapon*>(A);
                if (W)
                {
                    if (W->m_scope_status == ALife::eAddonAttachable)
                        W->m_addon_flags.set(CSE_ALifeItemWeapon::eWeaponAddonScope, bScope);
                    if (W->m_silencer_status == ALife::eAddonAttachable)
                        W->m_addon_flags.set(CSE_ALifeItemWeapon::eWeaponAddonSilencer, bSilencer);
                    if (W->m_grenade_launcher_status == ALife::eAddonAttachable)
                        W->m_addon_flags.set(CSE_ALifeItemWeapon::eWeaponAddonGrenadeLauncher, bLauncher);
                }

                NET_Packet P;
                A->Spawn_Write(P, TRUE);
                Level().Send(P, net_flags(TRUE));
                F_entity_Destroy(A);
            }
        }
    }
}

void CGameObject::setup_parent_ai_locations(bool assign_position)
{
    //	CGameObject				*l_tpGameObject	= static_cast<CGameObject*>(H_Root());
    VERIFY(H_Parent());
    CGameObject* l_tpGameObject = smart_cast<CGameObject*>(H_Parent());
    VERIFY(l_tpGameObject);

    // get parent's position
    if (assign_position && use_parent_ai_locations())
        Position().set(l_tpGameObject->Position());

    // if ( assign_position &&
    //		( use_parent_ai_locations() &&
    //		!( cast_attachable_item() && cast_attachable_item()->enabled() )
    //		 )
    //	)
    //	Position().set		(l_tpGameObject->Position());

    // setup its ai locations
    if (!UsedAI_Locations())
        return;

    if (!ai().get_level_graph())
        return;

    if (l_tpGameObject->UsedAI_Locations() &&
        ai().level_graph().valid_vertex_id(l_tpGameObject->ai_location().level_vertex_id()))
        ai_location().level_vertex(l_tpGameObject->ai_location().level_vertex_id());
    else
        validate_ai_locations(false);
    //	VERIFY2						(l_tpGameObject->UsedAI_Locations(),*l_tpGameObject->cNameSect());
    //	VERIFY2
    //(ai().level_graph().valid_vertex_id(l_tpGameObject->ai_location().level_vertex_id()),*cNameSect());
    //	ai_location().level_vertex	(l_tpGameObject->ai_location().level_vertex_id());

    if (ai().game_graph().valid_vertex_id(l_tpGameObject->ai_location().game_vertex_id()))
        ai_location().game_vertex(l_tpGameObject->ai_location().game_vertex_id());
    else
        ai_location().game_vertex(ai().cross_table().vertex(ai_location().level_vertex_id()).game_vertex_id());
    //	VERIFY2
    //(ai().game_graph().valid_vertex_id(l_tpGameObject->ai_location().game_vertex_id()),*cNameSect());
    //	ai_location().game_vertex	(l_tpGameObject->ai_location().game_vertex_id());
}

#if XR_PERF_METRICS
#if defined(XR_PLATFORM_WINDOWS)
#include <psapi.h> // QueryWorkingSetEx (K32QueryWorkingSetEx of kernel32 with _WIN32_WINNT >= Win7)
#endif
namespace
{
// 33-level-path.md (plans/optimization_mt), journal 34: the [perf-spike] go/ai_locations line says where its time went.
// tl_ai_loc_cross_ticks = the cross table lookups of update_ai_locations on this thread (the cross table of the level
// lives in the memory-mapped all.spawn); spatial_move resets it before its block.
thread_local u64 tl_ai_loc_cross_ticks = 0;
// 35 (33-level-path.md, journal): cross_ms by part. get = ai().cross_table(), read = the read of the cell (the page
// fault of the mapped all.spawn is here), set = ai_location().game_vertex(); ws_miss = the lookups whose cell page was
// not in the working set before the read (perf_metrics on, Windows); src: 0 = no lookup, 1 = the mapped file,
// 2 = the copy in the memory (35)
thread_local u64 tl_ai_loc_cross_get_ticks = 0;
thread_local u64 tl_ai_loc_cross_read_ticks = 0;
thread_local u64 tl_ai_loc_cross_set_ticks = 0;
thread_local u32 tl_ai_loc_cross_ws_miss = 0;
thread_local u8 tl_ai_loc_cross_src = 0;

// 35: 1 = the bytes [address, address + size) are in the working set of the process (reading them is no page fault),
// 0 = a page of them is not (the read faults: a soft fault from the standby list, or a read from the disk),
// -1 = unknown (not Windows, or the query failed). A syscall, ~1 us: only with perf_metrics on.
int AiLocCrossPagesResident(const void* address, size_t size) noexcept
{
#if defined(XR_PLATFORM_WINDOWS)
    PSAPI_WORKING_SET_EX_INFORMATION info[2]{};
    const uintptr_t first = reinterpret_cast<uintptr_t>(address);
    const uintptr_t last = first + size - 1;
    info[0].VirtualAddress = reinterpret_cast<void*>(first);
    info[1].VirtualAddress = reinterpret_cast<void*>(last);
    const DWORD count = (first >> 12) == (last >> 12) ? 1 : 2; // a 6-byte cell may cross a 4 KB page
    if (!QueryWorkingSetEx(GetCurrentProcess(), info, count * sizeof(info[0])))
        return -1;
    for (DWORD i = 0; i < count; ++i)
        if (!info[i].VirtualAttributes.Valid)
            return 0;
    return 1;
#else
    (void)address;
    (void)size;
    return -1;
#endif
}

const char* AiLocCrossSourceName(u8 src) noexcept
{
    switch (src)
    {
    case 1: return "map";
    case 2: return "mem";
    default: return "-";
    }
}

// " get= read= set= ws_miss= src=" of the cross table lookups (35)
void DescribeAiLocationsCross(char* buf, size_t size) noexcept
{
    xr_sprintf(buf, size, " get=%.3f read=%.3f set=%.3f ws_miss=%u src=%s",
        xray::perf::TicksToMilliseconds(tl_ai_loc_cross_get_ticks),
        xray::perf::TicksToMilliseconds(tl_ai_loc_cross_read_ticks),
        xray::perf::TicksToMilliseconds(tl_ai_loc_cross_set_ticks), tl_ai_loc_cross_ws_miss,
        AiLocCrossSourceName(tl_ai_loc_cross_src));
}

struct AiLocationsSpike
{
    const CGameObject* object;
    u32 vertex_seq; // CLevelGraph::last_vertex_stats().seq before the block: another value = vertex() was called
};

// "<the object> lg=<path>[/fast][/fallback] lg_ms= cross_ms= get= read= set= ws_miss= src= dist= dy= cur= new=": the
// path of CLevelGraph::vertex (current/xz/full/full_fast/guess/nearest, "-" = not called: the parent's vertex), its
// time, the cross table time and its parts (35: DescribeAiLocationsCross), how far the position is from the contour
// of the vertex found (dist, m) and above its plane (dy, m)
void DescribeAiLocationsSpike(const void* ctx, char* buf, size_t size) noexcept
{
    const AiLocationsSpike& info = *static_cast<const AiLocationsSpike*>(ctx);
    string256 object{};
    xray::perf::DescribeSpikeObject<CGameObject>(info.object, object, sizeof(object));
    const double cross_ms = xray::perf::TicksToMilliseconds(tl_ai_loc_cross_ticks);
    string128 cross{};
    DescribeAiLocationsCross(cross, sizeof(cross));
    const CLevelGraph::VertexStats& lg = CLevelGraph::last_vertex_stats();
    if (lg.seq == info.vertex_seq || !ai().get_level_graph())
    {
        xr_sprintf(buf, size, "%s lg=- cross_ms=%.3f%s", object, cross_ms, cross);
        return;
    }
    const CLevelGraph& graph = ai().level_graph();
    float dist = -1.f, dy = 0.f;
    if (graph.valid_vertex_id(lg.result))
    {
        dist = _sqrt(graph.distance(lg.result, lg.position));
        dy = lg.position.y - graph.vertex_plane_y(lg.result, lg.position.x, lg.position.z);
    }
    xr_sprintf(buf, size, "%s lg=%s%s%s lg_ms=%.3f cross_ms=%.3f%s dist=%.2f dy=%.2f cur=%d new=%u", object,
        CLevelGraph::vertex_branch_name(lg.branch), lg.fast ? "/fast" : "", lg.fallback ? "/fallback" : "",
        xray::perf::TicksToMilliseconds(lg.ticks), cross_ms, cross, dist, dy,
        graph.valid_vertex_id(lg.current_id) ? int(lg.current_id) : -1, lg.result);
}
} // namespace
#endif

u32 CGameObject::new_level_vertex_id() const
{
    Fvector center;
    Center(center);
    center.x = Position().x;
    center.z = Position().z;
    return (ai().level_graph().vertex(ai_location().level_vertex_id(), center));
}

void CGameObject::update_ai_locations(bool decrement_reference)
{
    u32 l_dwNewLevelVertexID = new_level_vertex_id();
    VERIFY(ai().level_graph().valid_vertex_id(l_dwNewLevelVertexID));
    if (decrement_reference && (ai_location().level_vertex_id() == l_dwNewLevelVertexID))
        return;

    ai_location().level_vertex(l_dwNewLevelVertexID);

    if (!ai().get_game_graph() && ai().get_cross_table())
        return;

#if XR_PERF_METRICS
    {
        // 34: cross_ms of the [perf-spike] go/ai_locations line; 35: by part, the same lookup as below. The read of
        // the cell is between two calls of Ticks() (opaque calls into xrCore: the load cannot move past them).
        const u64 t0 = xray::perf::Ticks();
        const CGameLevelCrossTable& cross_table = ai().cross_table();
        const u64 t1 = xray::perf::Ticks();
        const CGameLevelCrossTable::CCell& cell = cross_table.vertex(ai_location().level_vertex_id());
        const bool active = xray::perf::g_perf_metrics_active;
        const int resident = active ? AiLocCrossPagesResident(&cell, sizeof(cell)) : -1;
        const u64 t2 = xray::perf::Ticks();
        const GameGraph::_GRAPH_ID game_vertex_id = cell.game_vertex_id();
        const u64 t3 = xray::perf::Ticks();
        ai_location().game_vertex(game_vertex_id);
        const u64 t4 = xray::perf::Ticks();
        const u64 get_ticks = t1 - t0, read_ticks = t3 - t2, set_ticks = t4 - t3;
        tl_ai_loc_cross_ticks += get_ticks + read_ticks + set_ticks;
        tl_ai_loc_cross_get_ticks += get_ticks;
        tl_ai_loc_cross_read_ticks += read_ticks;
        tl_ai_loc_cross_set_ticks += set_ticks;
        tl_ai_loc_cross_ws_miss += resident == 0 ? 1 : 0;
        tl_ai_loc_cross_src = cross_table.in_memory() ? 2 : 1;
        if (active)
        {
            xray::perf::AddTimeSlow(xray::perf::Metric::ai_cross_lookup, get_ticks + read_ticks + set_ticks);
            xray::perf::AddTimeSlow(xray::perf::Metric::ai_cross_read, read_ticks);
            if (xray::perf::TicksToMilliseconds(read_ticks) >= 0.5)
                xray::perf::AddTimeSlow(xray::perf::Metric::ai_cross_slow, read_ticks);
            PERF_ADD_IF(resident == 0, ai_cross_ws_miss, 1);
        }
    }
#else
    ai_location().game_vertex(ai().cross_table().vertex(ai_location().level_vertex_id()).game_vertex_id());
#endif
    VERIFY(ai().game_graph().valid_vertex_id(ai_location().game_vertex_id()));
}

void CGameObject::validate_ai_locations(bool decrement_reference)
{
    if (!ai().get_level_graph())
        return;

    if (!UsedAI_Locations())
        return;

    update_ai_locations(decrement_reference);
}

void CGameObject::spatial_register()
{
    SpatialBase::spatial_register();
}

void CGameObject::spatial_unregister() { SpatialBase::spatial_unregister(); }

void CGameObject::spatial_refresh_bounds()
{
    Center(spatial.sphere.P);
    spatial.sphere.R = Radius();
}

void CGameObject::spatial_move()
{
    {
        ZoneScopedN("go_spatial_move/ai_locations");
        // P19 (plans/optimization_mt/14-spike-instrumentation.md): validate_ai_locations -> CLevelGraph::vertex, whose
        // slow paths (full search, guess, nearest) are ai/level_graph/*; the actor apart from the other objects
        const bool is_actor = xray::perf::g_perf_metrics_active && cast_actor();
        PERF_SCOPE_IF(is_actor, go_ai_locations_actor);
        PERF_SCOPE_IF(!is_actor, go_ai_locations_other);
#if XR_PERF_METRICS
        // 34 (33-level-path.md, journal): the object, the path of CLevelGraph::vertex and the cross table time
        tl_ai_loc_cross_ticks = 0;
        tl_ai_loc_cross_get_ticks = tl_ai_loc_cross_read_ticks = tl_ai_loc_cross_set_ticks = 0; // 35
        tl_ai_loc_cross_ws_miss = 0;
        tl_ai_loc_cross_src = 0;
        const AiLocationsSpike spike_info{this, CLevelGraph::last_vertex_stats().seq};
        PERF_SPIKE_NAMED(ai_locations_spike, "go/ai_locations", &spike_info, &DescribeAiLocationsSpike, false);
#endif
        if (H_Parent())
            setup_parent_ai_locations();
        else if (Visual())
            validate_ai_locations();
    }
    SpatialBase::spatial_move();
}

void CGameObject::spatial_update(float eps_P, float eps_R)
{
    //
    BOOL bUpdate = FALSE;
    if (PositionStack.empty())
    {
        // Empty
        bUpdate = TRUE;
        PositionStack.push_back(GameObjectSavedPosition());
        PositionStack.back().dwTime = Device.dwTimeGlobal;
        PositionStack.back().vPosition = Position();
    }
    else
    {
        if (PositionStack.back().vPosition.similar(Position(), eps_P))
        {
            // Just update time
            PositionStack.back().dwTime = Device.dwTimeGlobal;
        }
        else
        {
            // Register _new_ record
            bUpdate = TRUE;
            if (PositionStack.size() < 4)
            {
                PositionStack.push_back(GameObjectSavedPosition());
            }
            else
            {
                PositionStack[0] = PositionStack[1];
                PositionStack[1] = PositionStack[2];
                PositionStack[2] = PositionStack[3];
            }
            PositionStack.back().dwTime = Device.dwTimeGlobal;
            PositionStack.back().vPosition = Position();
        }
    }

    if (bUpdate)
    {
        spatial_move();
    }
    else
    {
        if (spatial.node_ptr)
        {
            // Object registered!
            if (!fsimilar(Radius(), spatial.sphere.R, eps_R))
                spatial_move();
            else
            {
                Fvector C;
                Center(C);
                if (!C.similar(spatial.sphere.P, eps_P))
                    spatial_move();
            }
            // else nothing to do :_)
        }
    }
}

#ifdef DEBUG
void CGameObject::dbg_DrawSkeleton()
{
    CCF_Skeleton* Skeleton = smart_cast<CCF_Skeleton*>(CForm);
    if (!Skeleton)
        return;
    Skeleton->_dbg_refresh();

    const CCF_Skeleton::ElementVec& Elements = Skeleton->_GetElements();
    for (CCF_Skeleton::ElementVec::const_iterator I = Elements.begin(); I != Elements.end(); ++I)
    {
        if (!I->valid())
            continue;
        switch (I->type)
        {
        case SBoneShape::stBox:
        {
            Fmatrix M;
            M.invert(I->b_IM);
            Fvector h_size = I->b_hsize;
            Level().debug_renderer().draw_obb(M, h_size, color_rgba(0, 255, 0, 255));
        }
        break;
        case SBoneShape::stCylinder:
        {
            Fmatrix M;
            M.c.set(I->c_cylinder.m_center);
            M.k.set(I->c_cylinder.m_direction);
            Fvector h_size;
            h_size.set(I->c_cylinder.m_radius, I->c_cylinder.m_radius, I->c_cylinder.m_height * 0.5f);
            Fvector::generate_orthonormal_basis(M.k, M.j, M.i);
            Level().debug_renderer().draw_obb(M, h_size, color_rgba(0, 127, 255, 255));
        }
        break;
        case SBoneShape::stSphere:
        {
            Fmatrix l_ball;
            l_ball.scale(I->s_sphere.R, I->s_sphere.R, I->s_sphere.R);
            l_ball.translate_add(I->s_sphere.P);
            Level().debug_renderer().draw_ellipse(l_ball, color_rgba(0, 255, 0, 255));
        }
        break;
        };
    };
}
#endif

IGameObject* CGameObject::H_SetParent(IGameObject* new_parent, bool just_before_destroy)
{
    if (new_parent == Parent)
        return new_parent;
    IGameObject* old_parent = Parent;
    VERIFY2(!new_parent || !old_parent, "Before set parent - execute H_SetParent(0)");
    // if (Parent) Parent->H_ChildRemove (this);
    if (!old_parent)
        OnH_B_Chield(); // before attach
    else
        OnH_B_Independent(just_before_destroy); // before detach
    if (new_parent)
        spatial_unregister();
    else
        spatial_register();
    Parent = new_parent;
    if (!old_parent)
        OnH_A_Chield(); // after attach
    else
        OnH_A_Independent(); // after detach
    // if (Parent) Parent->H_ChildAdd (this);
    {
        PERF_CROW_SRC(xray::perf::crow::SetParent);
        MakeMeCrow();
    }
    return old_parent;
}

void CGameObject::OnH_A_Chield() {}
void CGameObject::OnH_B_Chield()
{
    //
    setVisible(false);
    // ~
    /// PHSetPushOut();????
}

void CGameObject::OnH_A_Independent() { setVisible(true); }
void CGameObject::OnH_B_Independent(bool just_before_destroy)
{
    setup_parent_ai_locations(false);
    CGameObject* parent = smart_cast<CGameObject*>(H_Parent());
    VERIFY(parent);
    if (ai().get_level_graph() && ai().level_graph().valid_vertex_id(parent->ai_location().level_vertex_id()))
        validate_ai_locations(false);
}

void CGameObject::setDestroy(bool _destroy)
{
    if (_destroy == (BOOL)Props.bDestroy)
        return;
    Props.bDestroy = _destroy ? 1 : 0;
    if (_destroy)
    {
        g_pGameLevel->Objects.register_object_to_destroy(this);
#ifdef DEBUG
        if (debug_destroy)
            Msg("cl setDestroy [%d][%d]", ID(), Device.dwFrame);
#endif
#ifdef MP_LOGGING
        Msg("cl setDestroy [%d][%d]", ID(), Device.dwFrame);
#endif //#ifdef MP_LOGGING
    }
    else
        VERIFY(!g_pGameLevel->Objects.registered_object_to_destroy(this));
}

Fvector CGameObject::get_new_local_point_on_mesh(u16& bone_id) const
{
    bone_id = u16(-1);
    return Fvector().random_dir().mul(.7f);
}

Fvector CGameObject::get_last_local_point_on_mesh(Fvector const& local_point, u16 const bone_id) const
{
    VERIFY(bone_id == u16(-1));
    Fvector result;
    // Fetch data
    Fmatrix mE;
    const Fmatrix& M = XFORM();
    const Fbox& B = CForm->getBBox();
    // Build OBB + Ellipse and X-form point
    Fvector c, r;
    Fmatrix T, mR, mS;
    B.getcenter(c);
    B.getradius(r);
    T.translate(c);
    mR.mul_43(M, T);
    mS.scale(r);
    mE.mul_43(mR, mS);
    mE.transform_tiny(result, local_point);
    return result;
}

bool CGameObject::rendered_in_main_pass(u32 frames) const
{
    const u32 frame = m_main_pass_frame.load(std::memory_order_relaxed);
    return frame && frame + frames >= Device.dwFrame;
}

bool CGameObject::rendered_in_any_pass(u32 frames) const
{
    const u32 frame = m_bones_prepass_note_frame.load(std::memory_order_relaxed);
    return frame && frame + frames >= Device.dwFrame;
}

void CGameObject::renderable_Render(u32 context_id, IRenderable* root)
{
    //
    // The dedicated SVP build thread renders objects outside the task scheduler; engine
    // registration queues (o_crow) are keyed by scheduler worker IDs and must not be touched
    // from there. The main pass renders the same objects and performs the registration.
    if (xray::perf::TlPass() == xray::perf::pass::Main)
        m_main_pass_frame.store(Device.dwFrame, std::memory_order_relaxed);
    // Bones prepass: the input of the next frame's prepass - every object the render draws, in any pass (main,
    // shadows, lights, SVP), once per frame. The prepass keeps only those whose bones the render really computed.
    {
        const u32 frame = Device.dwFrame;
        if (m_bones_prepass_note_frame.load(std::memory_order_relaxed) != frame &&
            m_bones_prepass_note_frame.exchange(frame, std::memory_order_relaxed) != frame)
            CLevel::BonesPrepassNote(ID());
    }
    if (!svp_worker_is_current_thread())
        MakeMeCrow();
    // ~
    GEnv.Render->add_Visual(context_id, root, Visual(), XFORM());
    Visual()->getVisData().hom_frame = Device.dwFrame;
}

/*
float CGameObject::renderable_Ambient	()
{
    return (ai().get_level_graph() && ai().level_graph().valid_vertex_id(level_vertex_id()) ?
float(level_vertex()->light()/15.f) : 1.f);
}
*/

GameObjectSavedPosition CGameObject::ps_Element(u32 ID) const
{
    VERIFY(ID < ps_Size());
    GameObjectSavedPosition SP = PositionStack[ID];
    SP.dwTime += Level().timeServer_Delta();
    return SP;
}

void CGameObject::u_EventGen(NET_Packet& P, u32 type, u32 dest)
{
    P.w_begin(M_EVENT);
    P.w_u32(Level().timeServer());
    P.w_u16(u16(type & 0xffff));
    P.w_u16(u16(dest & 0xffff));
}

void CGameObject::u_EventSend(NET_Packet& P, u32 dwFlags) { Level().Send(P, dwFlags); }
#include "Bolt.h"

bool CGameObject::UsedAI_Locations() { return (m_server_flags.test(CSE_ALifeObject::flUsedAI_Locations)); }
bool CGameObject::TestServerFlag(u32 Flag) const { return (m_server_flags.test(Flag)); }
void CGameObject::add_visual_callback(visual_callback callback)
{
    VERIFY(smart_cast<IKinematics*>(Visual()));
    [[maybe_unused]] auto I = std::find(visual_callbacks().begin(), visual_callbacks().end(), callback);
    VERIFY(I == visual_callbacks().end());

    // The list is walked by VisualCallback in phase B of the bones (a pipelined prepass task may run it beside main's
    // UpdateCL): change it under the bone lock (waits for a running phase B; a no-op if this thread may not block).
    const KinematicsBonesLock bones_lock(PKinematics(Visual()));
    if (m_visual_callback.empty())
        SetKinematicsCallback(true);
    //		smart_cast<IKinematics*>(Visual())->Callback(VisualCallback,this);
    m_visual_callback.push_back(callback);
}

void CGameObject::remove_visual_callback(visual_callback callback)
{
    const KinematicsBonesLock bones_lock(PKinematics(Visual())); // see add_visual_callback
    CALLBACK_VECTOR_IT I = std::find(m_visual_callback.begin(), m_visual_callback.end(), callback);
    VERIFY(I != m_visual_callback.end());
    m_visual_callback.erase(I);
    if (m_visual_callback.empty())
        SetKinematicsCallback(false);
    //		smart_cast<IKinematics*>(Visual())->Callback(0,0);
}

void CGameObject::SetKinematicsCallback(bool set)
{
    if (!Visual())
        return;
    if (set)
        smart_cast<IKinematics*>(Visual())->Callback(VisualCallback, this);
    else
        smart_cast<IKinematics*>(Visual())->Callback(0, 0);
};

void VisualCallback(IKinematics* tpKinematics)
{
    CGameObject* game_object =
        smart_cast<CGameObject*>(static_cast<IGameObject*>(tpKinematics->GetUpdateCallbackParam()));
    VERIFY(game_object);
    for (const auto& cb : game_object->visual_callbacks())
        cb(tpKinematics);
}

CScriptGameObject* CGameObject::lua_game_object() const
{
    if (!m_spawned)
    {
         GEnv.ScriptEngine->script_log(LuaMessageType::Error, 
            "! ERROR: you are trying to use a destroyed object [%s]", cName().c_str());
        xrDebug::Fatal(DEBUG_INFO, make_string("! ERROR: you are trying to use a destroyed object [%s]", cName().c_str()).c_str());
    }
    VERIFY(m_spawned);
    if (!m_lua_game_object)
        m_lua_game_object = xr_new<CScriptGameObject>(const_cast<CGameObject*>(this));
    return (m_lua_game_object);
}

bool CGameObject::NeedToDestroyObject() const { return false; }
void CGameObject::DestroyObject()
{
    if (m_bObjectRemoved)
        return;
    m_bObjectRemoved = true;
    if (getDestroy())
        return;

    if (Local())
    {
        // When this object is in "pre-destroy" state (typically initiated after
        // GE_OWNERSHIP_REJECT with just_before_destroy flag), the server will
        // destroy it anyway. Sending GE_DESTROY again causes server-side
        // "not found on server" spam due to event ordering/race.
        if (GetTmpPreDestroy())
            return;

        // P17: relcase report (perf_metrics) - destroyed by engine code (used up item, ammo, grenade...)
        g_pGameLevel->Objects.destroy_origin_note(ID(), CObjectList::destroy_origin_engine);
        NET_Packet P;
        u_EventGen(P, GE_DESTROY, ID());
        u_EventSend(P);
    }
}

void CGameObject::shedule_Update(u32 dt)
{
    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::GameObjectScheduleUpdate);
    START_PROFILE("game_object/schedule_update")
    //уничтожить
    if (NeedToDestroyObject())
    {
        ZoneScopedN("sh_CGameObject_shedule/DestroyObject");
#ifndef MASTER_GOLD
        Msg("--NeedToDestroyObject for [%d][%d]", ID(), Device.dwFrame);
#endif // #ifndef MASTER_GOLD
        DestroyObject();
    }
    // Msg("-SUB-:[%x][%s] CGameObject::shedule_Update",smart_cast<void*>(this),*cName());
    // IGameObject::shedule_Update(dt);
    // consistency check
    // Msg ("-SUB-:[%x][%s] IGameObject::shedule_Update",dynamic_cast<void*>(this),*cName());
    {
        ZoneScopedN("sh_CGameObject_shedule/ScheduledBase");
        START_PROFILE("game_object/schedule_update/base")
        ScheduledBase::shedule_Update(dt);
        STOP_PROFILE
    }

    // Optimization: check if this is a long-dead entity
    CEntityAlive* entity_alive = cast_entity_alive();
    const bool is_dead_entity = entity_alive && !entity_alive->g_Alive();
    const u32 death_time = entity_alive && entity_alive->GetLevelDeathTime() ? entity_alive->GetLevelDeathTime() : 0;
    const bool is_long_dead = is_dead_entity && death_time && (Device.dwTimeGlobal - death_time > 3000) &&
        corpse_ragdoll_settled(entity_alive);
    // P5-opt: after npc_perf_long_dead_skip_binder_ms skip script binder entirely for corpses (reduce schedule_update tail)
    const bool very_long_dead = is_dead_entity && death_time && (Device.dwTimeGlobal - death_time > npc_perf_long_dead_skip_binder_ms);

    // For long-dead entities, skip expensive spatial update
    if (!is_long_dead)
    {
        ZoneScopedN("sh_CGameObject_shedule/spatial_update");
        START_PROFILE("game_object/schedule_update/spatial")
        spatial_update(base_spu_epsP * 1, base_spu_epsR * 1);
        STOP_PROFILE
    }

    // Always make me crow on shedule-update
    // Makes sure that update-cl called at least with freq of shedule-update
    {
        ZoneScopedN("sh_CGameObject_shedule/MakeMeCrow");
        START_PROFILE("game_object/schedule_update/crow")
        PERF_CROW_SRC(xray::perf::crow::Shedule);
        MakeMeCrow();
        STOP_PROFILE
    }
    /*
    if (AlwaysTheCrow()) MakeMeCrow ();
    else if (Device.vCameraPosition.distance_to_sqr(Position()) < CROW_RADIUS*CROW_RADIUS) MakeMeCrow ();
    */
    // ~
    if (!GEnv.isDedicatedServer && !very_long_dead)
    {
        ZoneScopedN("sh_CGameObject_shedule/client_block");
        // Lightweight per-frame Lua callback for actor only (hitmark, HUD, etc.). No full binder.
        const bool is_actor = (cast_actor() != nullptr);
        if (is_actor)
        {
            ZoneScopedN("sh_CGameObject_shedule/actor_frame_update");
            luabind::functor<void> actor_frame_update;
            if (GEnv.ScriptEngine->functor("actor_frame_update", actor_frame_update))
                actor_frame_update();
        }

        // BUGFIX: мёртвые мутанты/НПС тоже должны получать shedule_Update через script binder,
        // иначе bind_monster:update() / bind_stalker:update() никогда не установят
        // callback.use_object (лут частей мутанта, обыск трупа).
        // Для живых объектов — без ограничений. Для мёртвых — throttle до ~5 Hz,
        // чтобы сохранить выигрыш от оптимизации, но не ломать USE-колбэк.
        // P5: после LONG_DEAD_SKIP_BINDER_MS (5s) script binder не вызывается.
        // Доп. LOD: для объектов далеко от камеры снижаем частоту вызова binder,
        // уменьшая пики в script_binder/luabind_update.
        const bool is_dead_non_actor = is_dead_entity && !cast_actor();
        const bool run_dead_binder = !is_dead_non_actor || (Device.dwFrame % 6u == u32(ID()) % 6u);

        // Усиленный anti-synchronization для scriptBinder:
        // для дальних объектов включаем binder не по кадру, а по "временным слотам",
        // что режет одновременные пики luabind_update у множества NPC.
        const float binder_far_dist_sqr = npc_perf_binder_far_dist * npc_perf_binder_far_dist;
        const bool is_far_to_camera =
            Device.vCameraPosition.distance_to_sqr(Position()) > binder_far_dist_sqr;

        // Длительность слота и количество фаз в цикле:
        // чем больше фаз, тем меньше шанс синхронизированных вызовов.
        const u32 far_slot = Device.dwTimeGlobal / npc_perf_binder_far_interval_ms;
        const u32 far_phase = far_slot % npc_perf_binder_far_phases;
        const u32 obj_phase = u32(ID()) % npc_perf_binder_far_phases;
        const bool run_far_binder = !is_far_to_camera || (far_phase == obj_phase);

        const u32 now_ms = Device.dwTimeGlobal;

        // Lazy init: stagger first binder run to avoid all objects running binder in the same frame.
        if (m_next_script_binder_update_time == 0)
            m_next_script_binder_update_time = now_ms + (u32(ID()) % 350u);

        // Per-object throttle: intervals from npc_perf_binder_near_interval_ms / npc_perf_binder_far_throttle_ms (user.ltx).
        const bool run_binder_timed = now_ms >= m_next_script_binder_update_time;

        // Smart zones (смарт-террейны): no throttle — call binder every shedule_Update.
        const bool is_smart_zone = (smart_cast<CSmartZone*>(this) != nullptr);
        const bool run_binder = (run_dead_binder && run_far_binder && run_binder_timed) || is_smart_zone;

        if (run_binder)
        {
            ZoneScopedN("sh_CGameObject_shedule/scriptBinder");
            START_PROFILE("game_object/schedule_update/script_binder")
            if (binder_gate_update(now_ms)) // stage D, W3-1: false = the Lua binder has nothing due this pass
            {
                // 22-game-thread.md (diagnostics only): the Lua binder update (xr_motivator, bind_monster...) by class
                xray::perf::ScopedTime binder_time(cast_stalker() ? xray::perf::Metric::go_shed_binder_stalker :
                        cast_base_monster() ? xray::perf::Metric::go_shed_binder_monster :
                                              xray::perf::Metric::go_shed_binder_other);
                scriptBinder.shedule_Update(dt);
            }
            else
                PERF_INC(binder_skip_gate);
            // W3-2: with the gate on, npc_on_update comes from the engine on the callback phase (Lua skips its emit)
            // Only for the binder that reads the mask: it is the one that skips its own emit then.
            if (m_binder_gate.consumer && (m_binder_gate.mask & kGatePhaseCallback))
            {
                PERF_SCOPE(go_shed_npc_update_emit);
                gw::addons::objevents::NpcUpdate(this);
            }
            gw::addons::objevents::ObjectBinderUpdate(this, dt); // actor_on_update* (actor only)
            STOP_PROFILE

            if (!is_smart_zone)
            {
                if (is_far_to_camera)
                {
                    const u32 jitter = (u32(ID()) % 200);
                    m_next_script_binder_update_time = now_ms + npc_perf_binder_far_throttle_ms + jitter;
                }
                else
                {
                    const u32 jitter = (u32(ID()) % 120);
                    m_next_script_binder_update_time = now_ms + npc_perf_binder_near_interval_ms + jitter;
                }
            }
        }
        else
            PERF_INC(binder_skip_throttle);
    }
    STOP_PROFILE
}

bool CGameObject::net_SaveRelevant() { return scriptBinder.net_SaveRelevant(); }
//игровое имя объекта
LPCSTR CGameObject::Name() const { return (cName().c_str()); }
u32 CGameObject::ef_creature_type() const
{
    string16 temp;
    CLSID2TEXT(CLS_ID, temp);
    R_ASSERT3(false, "Invalid creature type request, virtual function is not properly overridden!", temp);
    return (u32(-1));
}

u32 CGameObject::ef_equipment_type() const
{
    string16 temp;
    CLSID2TEXT(CLS_ID, temp);
    R_ASSERT3(false, "Invalid equipment type request, virtual function is not properly overridden!", temp);
    return (u32(-1));
    //	return		(6);
}

u32 CGameObject::ef_main_weapon_type() const
{
    string16 temp;
    CLSID2TEXT(CLS_ID, temp);
    R_ASSERT3(false, "Invalid main weapon type request, virtual function is not properly overridden!", temp);
    return (u32(-1));
    //	return		(5);
}

u32 CGameObject::ef_anomaly_type() const
{
    string16 temp;
    CLSID2TEXT(CLS_ID, temp);
    R_ASSERT3(false, "Invalid anomaly type request, virtual function is not properly overridden!", temp);
    return (u32(-1));
}

u32 CGameObject::ef_weapon_type() const
{
    string16 temp;
    CLSID2TEXT(CLS_ID, temp);
    R_ASSERT3(false, "Invalid weapon type request, virtual function is not properly overridden!", temp);
    return (u32(-1));
    //	return		(u32(0));
}

u32 CGameObject::ef_detector_type() const
{
    string16 temp;
    CLSID2TEXT(CLS_ID, temp);
    R_ASSERT3(false, "Invalid detector type request, virtual function is not properly overridden!", temp);
    return (u32(-1));
}

// The heavy overrides (CAI_Stalker, CCustomMonster, memory managers...) stay per object: what they share with other
// objects (group memory lists, agent manager) checks CObjectList::relcase_batch() and is cleaned of the whole batch once.
void CGameObject::net_RelcaseBatch(const RelcaseBatch& batch)
{
    // 32-frame-spikes.md, the no-op skip (always on since 2026-10-18): the class of this object keeps the empty net_Relcase below and this
    // function (game_object_relcase_noop of the type the factory created): the loop would only make empty calls
    if (m_relcase_noop)
    {
        ++ol_relcase_noop_skipped;
        return;
    }
    // 37-destroy-relcase-smooth.md (always on since 2026-10-18, see plan 41): the same for the script factory objects
    // (weapons, food, outfits... of class_registrator.script), whose exact class was found noop
    // (ObjectFactory::script_client_object_created)
    if (m_relcase_noop_script)
    {
        ++ol_relcase_noop_skipped;
        ++ol_relcase_noop_script_skipped;
        return;
    }
    for (u32 it = batch.count; it-- > 0;)
        net_Relcase(batch.objects[it]);
}

void CGameObject::net_Relcase(IGameObject* O)
{
    // objects x destroyed objects of them per frame with a destroy: hidden with perf_metrics bit 4 (P17)
    PERF_HOT_ZONE("CGameObject::net_Relcase");
    // The script binders are not told about destroyed objects (vanilla called scriptBinder.net_Relcase here: an
    // O(N*M) broadcast to every binder for every destroyed object), so the body is empty.
    (void)O;
}

CGameObject::CScriptCallbackExVoid& CGameObject::callback(GameObject::ECallbackType type) const
{
    return ((*m_callbacks)[type]);
}

void CGameObject::clear_callbacks() const
{
    for (auto& callback : *m_callbacks)
    {
        callback.second.clear();
    }
}

LPCSTR CGameObject::visual_name(CSE_Abstract* server_entity)
{
    const CSE_Visual* visual = smart_cast<const CSE_Visual*>(server_entity);
    VERIFY(visual);
    return (visual->get_visual());
}
bool CGameObject::animation_movement_controlled() const
{
    return !!animation_movement() && animation_movement()->IsActive();
}

void CGameObject::update_animation_movement_controller()
{
    if (!m_anim_mov_ctrl)
        return;

    if (m_anim_mov_ctrl->IsActive())
    {
        m_anim_mov_ctrl->OnFrame();
        return;
    }

    destroy_anim_mov_ctrl();
}

bool CGameObject::is_ai_obstacle() const { return (false); }
void CGameObject::OnChangeVisual()
{
    if (m_anim_mov_ctrl)
        destroy_anim_mov_ctrl();
}

bool CGameObject::shedule_Needed() { return (!getDestroy()); }
void CGameObject::create_anim_mov_ctrl(CBlend* b, Fmatrix* start_pose, bool local_animation)
{
    if (animation_movement_controlled())
    {
        m_anim_mov_ctrl->NewBlend(b, start_pose ? *start_pose : XFORM(), local_animation);
    }
    else
    {
        //		start_pose		= &renderable.xform;
        if (m_anim_mov_ctrl)
            destroy_anim_mov_ctrl();

        VERIFY2(start_pose, make_string("start pose hasn't been specified for animation [%s][%s]",
                                smart_cast<IKinematicsAnimated&>(*Visual()).LL_MotionDefName_dbg(b->motionID).first,
                                smart_cast<IKinematicsAnimated&>(*Visual()).LL_MotionDefName_dbg(b->motionID).second));

        VERIFY2(!animation_movement(),
            make_string("start pose hasn't been specified for animation [%s][%s]",
                smart_cast<IKinematicsAnimated&>(*Visual()).LL_MotionDefName_dbg(b->motionID).first,
                smart_cast<IKinematicsAnimated&>(*Visual()).LL_MotionDefName_dbg(b->motionID).second));

        VERIFY(Visual());
        IKinematics* K = Visual()->dcast_PKinematics();
        VERIFY(K);

        m_anim_mov_ctrl = xr_new<animation_movement_controller>(&XFORM(), *start_pose, K, b);
    }
}

void CGameObject::destroy_anim_mov_ctrl() { xr_delete(m_anim_mov_ctrl); }
IC bool similar(const Fmatrix& _0, const Fmatrix& _1, const float& epsilon = EPS)
{
    if (!_0.i.similar(_1.i, epsilon))
        return (false);

    if (!_0.j.similar(_1.j, epsilon))
        return (false);

    if (!_0.k.similar(_1.k, epsilon))
        return (false);

    if (!_0.c.similar(_1.c, epsilon))
        return (false);

    // note: we do not compare projection here
    return (true);
}

// The crow rules of UpdateCL: stay in the update list of the next frame or not. `position` - where the object is
// for the distance rules (its own position; the root's one for a child whose XFORM was not refreshed)
void CGameObject::UpdateCLCrow(const Fvector& position)
{
    if (Parent == g_pGameLevel->CurrentViewEntity() || AlwaysTheCrow())
    {
        PERF_CROW_SRC(xray::perf::crow::UclView);
        MakeMeCrow();
    }
    else
    {
        float dist = Device.vCameraPosition.distance_to_sqr(position);
        if (dist < CROW_RADIUS * CROW_RADIUS)
        {
            PERF_CROW_SRC(xray::perf::crow::UclR30);
            MakeMeCrow();
        }
        else if ((Visual() && Visual()->getVisData().hom_frame + 2 > Device.dwFrame) &&
            dist < CROW_RADIUS2 * CROW_RADIUS2)
        {
            PERF_CROW_SRC(xray::perf::crow::UclHom60);
            MakeMeCrow();
        }
    }
}

void CGameObject::UpdateCLSkippedChild()
{
    VERIFY(H_Parent());
#ifdef DEBUG
    // CObjectList::SingleUpdate checks that UpdateCL reached CGameObject::UpdateCL this frame
    if (Device.dwFrame == dbg_update_cl)
        xrDebug::Fatal(DEBUG_INFO, "'UpdateCL' called twice per frame for %s", cName().c_str());
    dbg_update_cl = Device.dwFrame;
#endif
    // spatial_update of CGameObject::UpdateCL: a child is not in the spatial DB, nothing to do
    UpdateCLCrow(H_Root()->Position());
}

void CGameObject::UpdateCLSkippedRoot()
{
    VERIFY(!H_Parent());
#ifdef DEBUG
    // CObjectList::SingleUpdate checks that UpdateCL reached CGameObject::UpdateCL this frame
    if (Device.dwFrame == dbg_update_cl)
        xrDebug::Fatal(DEBUG_INFO, "'UpdateCL' called twice per frame for %s", cName().c_str());
    dbg_update_cl = Device.dwFrame;
#endif
    m_ucl_lod_skip_frame = Device.dwFrame;
    // The tail of CGameObject::UpdateCL. The XFORM of a skipped object normally stays where the last full UpdateCL
    // left it, then both are no-ops; but scripts and teleports (ForceTransform) move it any frame - keep the spatial
    // DB (what the render, the vision rays and the sound queries see) and the obstacle in step with it then
    spatial_update(base_spu_epsP * 5, base_spu_epsR * 5);
    UpdateCLCrow(Position());
    if (similar(XFORM(), m_previous_matrix, EPS))
        return;
    on_matrix_change(m_previous_matrix);
    m_previous_matrix = XFORM();
}

bool CGameObject::ucl_lod_skipped_this_frame() const
{
    return m_ucl_lod_skip_frame && m_ucl_lod_skip_frame == Device.dwFrame;
}

bool CGameObject::ucl_lod_skipped_last_frame() const
{
    return m_ucl_lod_skip_frame && m_ucl_lod_skip_frame + 1 == Device.dwFrame;
}

void CGameObject::UpdateCL()
{
    ZoneScopedN("ucl_CGameObject");
    const u64 crow_packed = m_crow_packed.load(std::memory_order_relaxed);
    const u32 crow_frame = xray::perf::UnpackCrowFrame(crow_packed);
    const u8 crow_src = xray::perf::UnpackCrowSrc(crow_packed);
    if (crow_frame + 1 == Device.dwFrame &&
        (crow_src == xray::perf::crow::Sun || crow_src == xray::perf::crow::Light) &&
        m_main_pass_frame.load(std::memory_order_relaxed) != crow_frame)
    {
        if (cast_stalker())
            PERF_INC(crow_shadow_only_stalker);
        else if (cast_base_monster())
            PERF_INC(crow_shadow_only_monster);
        else
            PERF_INC(crow_shadow_only_other);
    }
// IGameObject::UpdateCL();
// consistency check
#ifdef DEBUG
    VERIFY2(_valid(renderable.xform), cName().c_str());
    if (Device.dwFrame == dbg_update_cl)
        xrDebug::Fatal(DEBUG_INFO, "'UpdateCL' called twice per frame for %s", cName().c_str());
    dbg_update_cl = Device.dwFrame;
    if (Parent && spatial.node_ptr)
        xrDebug::Fatal(DEBUG_INFO, "Object %s has parent but is still registered inside spatial DB", cName().c_str());
    if (!CForm && (spatial.type & STYPE_COLLIDEABLE))
        xrDebug::Fatal(DEBUG_INFO, "Object %s registered as 'collidable' but has no collidable model", cName().c_str());
#endif

    // Optimization: check if this is a long-dead entity
    CEntityAlive* entity_alive = cast_entity_alive();
    const bool is_dead_entity = entity_alive && !entity_alive->g_Alive();
    const u32 time_since_death = is_dead_entity ? (Device.dwTimeGlobal - entity_alive->GetLevelDeathTime()) : 0;
    
    // Grace period: don't skip updates for the first 3 seconds after death
    // This ensures death animation plays smoothly
    constexpr u32 DEATH_ANIMATION_GRACE_PERIOD_MS = 3000;
    const bool in_grace_period = is_dead_entity && (time_since_death < DEATH_ANIMATION_GRACE_PERIOD_MS);
    
    const bool is_long_dead =
        is_dead_entity && !in_grace_period && (time_since_death > 5000) && corpse_ragdoll_settled(entity_alive);

    // For long-dead entities, skip expensive spatial update in UpdateCL
    // P19 (plans/optimization_mt/14-spike-instrumentation.md): stages by actor / other objects (game_object/ucl/*) and
    // the spike logger (perf_spike_us), the actor always with the thread CPU time
    const bool perf_actor = (xray::perf::g_perf_metrics_active || xray::perf::SpikeActive()) && cast_actor();
    if (!is_long_dead)
    {
        ZoneScopedN("ucl_go/spatial_update");
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::GameObjectUpdateCLSpatial);
        PERF_SCOPE_IF(perf_actor, go_ucl_spatial_actor);
        PERF_SCOPE_IF(!perf_actor, go_ucl_spatial_other);
        PERF_SPIKE_OBJECT_CPU("ucl_go/spatial_update", this, perf_actor);
        spatial_update(base_spu_epsP * 5, base_spu_epsR * 5);
    }

    // crow
    {
        ZoneScopedN("ucl_go/crow");
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::GameObjectUpdateCLCrow);
        PERF_SCOPE_IF(perf_actor, go_ucl_crow_actor);
        PERF_SCOPE_IF(!perf_actor, go_ucl_crow_other);
        PERF_SPIKE_OBJECT_CPU("ucl_go/crow", this, perf_actor);
        UpdateCLCrow(Position());
    }
    // ~
    //	if (!is_ai_obstacle())
    //		return;

    if (H_Parent())
        return;

    // For long-dead entities, skip matrix change tracking
    if (is_long_dead)
        return;

    if (similar(XFORM(), m_previous_matrix, EPS))
        return;

    {
        ZoneScopedN("ucl_go/matrix_change");
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::GameObjectUpdateCLMatrixChange);
        PERF_SCOPE_IF(perf_actor, go_ucl_matrix_actor);
        PERF_SCOPE_IF(!perf_actor, go_ucl_matrix_other);
        PERF_SPIKE_OBJECT_CPU("ucl_go/matrix_change", this, perf_actor);
        on_matrix_change(m_previous_matrix);
    }
    m_previous_matrix = XFORM();
}

void CGameObject::PreUpdateCL() { m_client_updated = false; }
void CGameObject::PostUpdateCL(bool /*bUpdateCL_disabled*/) { m_client_updated = true;}

void CGameObject::on_matrix_change(const Fmatrix& previous) { obstacle().on_move(); }
#ifdef DEBUG

void render_box(
    IRenderVisual* visual, const Fmatrix& xform, const Fvector& additional, bool draw_child_boxes, const u32& color)
{
    CDebugRenderer& renderer = Level().debug_renderer();
    IKinematics* kinematics = smart_cast<IKinematics*>(visual);
    VERIFY(kinematics);
    u16 bone_count = kinematics->LL_BoneCount();
    VERIFY(bone_count);
    u16 visible_bone_count = kinematics->LL_VisibleBoneCount();
    if (!visible_bone_count)
        return;

    Fmatrix matrix;
    Fvector* points = (Fvector*)xr_alloca(visible_bone_count * 8 * sizeof(Fvector));
    Fvector* I = points;
    for (u16 i = 0; i < bone_count; ++i)
    {
        if (!kinematics->LL_GetBoneVisible(i))
            continue;

        const Fobb& obb = kinematics->LL_GetData(i).obb;
        if (fis_zero(obb.m_halfsize.square_magnitude()))
        {
            VERIFY(visible_bone_count > 1);
            --visible_bone_count;
            continue;
        }

        Fmatrix Mbox;
        obb.xform_get(Mbox);

        const Fmatrix& Mbone = kinematics->LL_GetBoneInstance(i).mTransform;
        Fmatrix X;
        matrix.mul_43(xform, X.mul_43(Mbone, Mbox));

        Fvector half_size = Fvector().add(obb.m_halfsize, additional);
        matrix.mulB_43(Fmatrix().scale(half_size));

        if (draw_child_boxes)
            renderer.draw_obb(matrix, color);

        static const Fvector local_points[8] = {Fvector().set(-1.f, -1.f, -1.f), Fvector().set(-1.f, -1.f, +1.f),
            Fvector().set(-1.f, +1.f, +1.f), Fvector().set(-1.f, +1.f, -1.f), Fvector().set(+1.f, +1.f, +1.f),
            Fvector().set(+1.f, +1.f, -1.f), Fvector().set(+1.f, -1.f, +1.f), Fvector().set(+1.f, -1.f, -1.f)};

        for (u32 i = 0; i < 8; ++i, ++I)
            matrix.transform_tiny(*I, local_points[i]);
    }

    VERIFY(visible_bone_count);
    if (visible_bone_count == 1)
    {
        renderer.draw_obb(matrix, color);
        return;
    }

    VERIFY((I - points) == (visible_bone_count * 8));
    MagicBox3 box = MagicMinBox(visible_bone_count * 8, points);
    box.ComputeVertices(points);

    Fmatrix result;
    result.identity();

    result.c = box.Center();

    result.i.sub(points[3], points[2]).normalize();
    result.j.sub(points[2], points[1]).normalize();
    result.k.sub(points[2], points[6]).normalize();

    Fvector scale;
    scale.x = points[3].distance_to(points[2]) * .5f;
    scale.y = points[2].distance_to(points[1]) * .5f;
    scale.z = points[2].distance_to(points[6]) * .5f;
    result.mulB_43(Fmatrix().scale(scale));

    renderer.draw_obb(result, color);
}

void CGameObject::OnRender()
{
    if (!ai().get_level_graph())
        return;

    CDebugRenderer& renderer = Level().debug_renderer();
    if (/**bDebug && **/ Visual())
    {
        float half_cell_size = 1.f * ai().level_graph().header().cell_size() * .5f;
        Fvector additional = Fvector().set(half_cell_size, half_cell_size, half_cell_size);

        render_box(Visual(), XFORM(), Fvector().set(0.f, 0.f, 0.f), true, color_rgba(0, 0, 255, 255));
        render_box(Visual(), XFORM(), additional, false, color_rgba(0, 255, 0, 255));
    }

    if (0)
    {
        Fvector bc, bd;
        Visual()->getVisData().box.get_CD(bc, bd);
        Fmatrix M = Fidentity;
        float half_cell_size = ai().level_graph().header().cell_size() * .5f;
        bd.add(Fvector().set(half_cell_size, half_cell_size, half_cell_size));
        M.scale(bd);
        Fmatrix T = XFORM();
        T.c.add(bc);
        renderer.draw_obb(T, bd, color_rgba(255, 255, 255, 255));
    }
}
#endif // DEBUG

bool CGameObject::use(IGameObject* obj)
{
    VERIFY(obj);
    CScriptGameObject* scriptObj = lua_game_object();
    if (scriptObj && scriptObj->m_door)
    {
        doors::door* door = scriptObj->m_door;
        if (door->is_blocked(doors::door_state_open) || door->is_blocked(doors::door_state_closed))
            return false;
    }
    callback(GameObject::eUseObject)(scriptObj, obj->lua_game_object());
    gw::addons::objevents::ObjectUse(this, obj);
    return true;
}

LPCSTR CGameObject::tip_text() { return m_sTipText.c_str(); }
void CGameObject::set_tip_text(LPCSTR new_text) { m_sTipText = new_text; }
void CGameObject::set_tip_text_default() { m_sTipText = nullptr; }
bool CGameObject::nonscript_usable() { return m_bNonscriptUsable; }
void CGameObject::set_nonscript_usable(bool usable) { m_bNonscriptUsable = usable; }
