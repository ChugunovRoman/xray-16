// Entity.cpp: implementation of the CEntity class.

//////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "addon_object_events.h"
#include <tracy/Tracy.hpp>
#include "Entity.h"
#include "Actor.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "Entity.h"
#include "Level.h"
#include "seniority_hierarchy_holder.h"
#include "team_hierarchy_holder.h"
#include "squad_hierarchy_holder.h"
#include "group_hierarchy_holder.h"
#include "Include/xrRender/Kinematics.h"
#include "monster_community.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_time_manager.h"
#include "alife_object_registry.h"
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "xrNetServer/NET_Messages.h"
#include "xrCore/Profiling/PerfMetrics.hpp"

#define BODY_REMOVE_TIME 600000

// 26-framemove-game-next.md, scheduler overrun (distribute_enemies of the group). Always on since 2026-10-18 (its switch
// npc_agent_group_by_squad removed, 41-cvar-cleanup.md): a stalker that belongs to an ALife squad is registered in the group of that squad (its agent
// manager and the memory its group shares) instead of the group the scripts give it (sim_board.script: every sim squad
// of a faction on a smart gets group 1 - one agent manager for all of them, tens of members). g_Group() and everything
// the scripts see stay the same. Applies when the stalker registers: on spawn (and so after a load) and on change_team
// (the sim board calls it when a squad takes a smart; the same team/squad/group also moves the stalker when the group
// it belongs to changed). Squads on one smart then do not share enemies, targets and memory.

namespace
{
// group keys above the 32 slots of a squad holder (CSquadHierarchyHolder::m_extra_groups)
constexpr u32 entity_hierarchy_squad_base = 0x10000; // + the ALife squad id (the group by squad)
constexpr u32 entity_hierarchy_overflow_shift = 20; // base | n << 20: the n-th overflow group of a full one
constexpr u32 entity_hierarchy_base_mask = (1u << entity_hierarchy_overflow_shift) - 1;
constexpr u32 entity_hierarchy_max_overflow = 255;
// an agent manager tells its members by a bit of MemorySpace::squad_mask_type (CAgentMemberManager::mask)
constexpr size_t entity_hierarchy_max_agent_members = sizeof(MemorySpace::squad_mask_type) * 8;

ALife::_OBJECT_ID entity_hierarchy_alife_squad(ALife::_OBJECT_ID id)
{
    if (!ai().get_alife() || id == ALife::_OBJECT_ID(-1))
        return ALife::_OBJECT_ID(-1);

    const CSE_ALifeMonsterAbstract* monster =
        smart_cast<const CSE_ALifeMonsterAbstract*>(ai().alife().objects().object(id, true));
    return monster ? monster->m_group_id : ALife::_OBJECT_ID(-1);
}
} // namespace

// The group a stalker should be in, before the 64-member guard: the group of its ALife squad, or g_Group() outside
// one. Other entities (monsters) always use g_Group()
u32 CEntity::desired_hierarchy_group(ALife::_OBJECT_ID self_id) const
{
    const u32 group = u32(id_Group);
    if (!const_cast<CEntity*>(this)->cast_stalker())
        return group;

    const ALife::_OBJECT_ID squad_id = entity_hierarchy_alife_squad(self_id);
    if (squad_id == ALife::_OBJECT_ID(-1))
        return group;

    return entity_hierarchy_squad_base + u32(squad_id);
}

// Safety guard (always on): an agent manager holds at most 64 stalkers - the 65th used to get the mask 1 << 64
// (undefined behaviour; on x86/ARM it aliased the first member: shared enemy marks, wrong member lookups). A stalker that
// would be the 65th goes to an overflow group of the same team/squad (base | n << 20) instead, with its own agent
// manager and shared memory
u32 CEntity::fit_hierarchy_group(u32 group) const
{
    if (!const_cast<CEntity*>(this)->cast_stalker())
        return group;

    const CSquadHierarchyHolder& squad = Level().seniority_holder().team(g_Team()).squad(g_Squad());
    const u32 base = group & entity_hierarchy_base_mask;
    for (u32 n = 0; n <= entity_hierarchy_max_overflow; ++n)
    {
        const u32 key = n ? (base | (n << entity_hierarchy_overflow_shift)) : base;
        const CAgentManager* agent_manager = squad.group(key).get_agent_manager();
        if (agent_manager && agent_manager->member().members().size() >= entity_hierarchy_max_agent_members)
            continue;

        if (n)
        {
            PERF_INC(agent_group_overflow);
            static u32 s_reported = 0;
            ++s_reported;
            if (s_reported <= 16 || !(s_reported % 512))
                Msg("! [GW] agent group [team:%d][squad:%d][group:%u] has %u stalkers, [%s] goes to its overflow group %u",
                    g_Team(), g_Squad(), base, u32(entity_hierarchy_max_agent_members),
                    cName().size() ? cName().c_str() : "<spawning>", n);
        }
        return key;
    }

    return base;
}

void CEntity::register_in_hierarchy(ALife::_OBJECT_ID self_id)
{
    m_hierarchy_group = fit_hierarchy_group(desired_hierarchy_group(self_id));
    m_hierarchy_group_set = true;
    Level().seniority_holder().team(g_Team()).squad(g_Squad()).group(m_hierarchy_group).register_member(this);
}

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CEntity::CEntity()
{
    m_registered_member = false;
    m_hierarchy_group_set = false;
    m_hierarchy_group = 0;
}
CEntity::~CEntity() { xr_delete(m_entity_condition); }
CEntityConditionSimple* CEntity::create_entity_condition(CEntityConditionSimple* ec)
{
    if (!ec)
        m_entity_condition = xr_new<CEntityConditionSimple>();
    else
        m_entity_condition = smart_cast<CEntityCondition*>(ec);

    return m_entity_condition;
}

void CEntity::OnEvent(NET_Packet& P, u16 type)
{
    inherited::OnEvent(P, type);

    switch (type)
    {
    case GE_DIE:
    {
        u16 id;
        u32 cl;
        P.r_u16(id);
        P.r_u32(cl);
        IGameObject* who = Level().Objects.net_Find(id);
        if (who && !IsGameTypeSingle())
        {
            if (this != who) /*if(bDebug) */
                Msg("%s killed by %s ...", cName().c_str(), who->cName().c_str());
            else /*if(bDebug) */
                Msg("%s dies himself ...", cName().c_str());
        }
        Die(who);
    }
    break;
    }
}

void CEntity::Die(IGameObject* who)
{
    if (!AlreadyDie())
        set_death_time();
    set_ready_to_save();
    SetfHealth(-1.f);

    // Only a member that actually got registered may be unregistered. net_Spawn registers under
    // g_Alive() (Entity.cpp, CEntity::net_Spawn), so an entity spawned dead - or one that is dying
    // a second time - never entered the group. Unregistering it anyway used to walk into
    // CGroupHierarchyHolder::unregister_in_group with an iterator equal to end(); the VERIFY there
    // is compiled out in release and the erase silently dropped a live member from the registry.
    // The group then looked empty and freed the visible/sound/hit vectors its live members still
    // point at - which is the dangling pointer that surfaces in object_id().
    if (m_registered_member)
    {
        m_registered_member = false;
        if (IsGameTypeSingle())
            Level().seniority_holder().team(g_Team()).squad(g_Squad()).group(hierarchy_group()).unregister_member(this);
    }
}

//обновление состояния
float CEntity::CalcCondition(float hit)
{
    // If Local() - perform some logic
    if (Local() && g_Alive())
    {
        SetfHealth(GetfHealth() - hit);
        SetfHealth((GetfHealth() < -1000) ? -1000 : GetfHealth());
    }
    return hit;
}

// void CEntity::Hit			(float perc, Fvector &dir, IGameObject* who, s16 element,Fvector position_in_object_space,
// float
// impulse, ALife::EHitType hit_type)
void CEntity::Hit(SHit* pHDS)
{
    //	if (bDebug)				Log("Process HIT: ", *cName());

    // *** process hit calculations
    // Calc impulse
    Fvector vLocalDir;
    VERIFY(pHDS->dir.magnitude() > EPS);

    // convert impulse into local coordinate system
    Fmatrix mInvXForm;
    mInvXForm.invert(XFORM());
    mInvXForm.transform_dir(vLocalDir, pHDS->dir);
    vLocalDir.invert();

    // hit impulse
    if (pHDS->impulse)
        HitImpulse(pHDS->impulse, pHDS->dir, vLocalDir); // @@@: WT

    // Calc amount (correct only on local player)
    float lost_health = CalcCondition(pHDS->damage());

    // Signal hit
    if (BI_NONE != pHDS->bone())
        HitSignal(lost_health, vLocalDir, pHDS->who, pHDS->boneID);

    // If Local() - perform some logic
    if (Local() && !g_Alive() && !AlreadyDie() && (m_killer_id == ALife::_OBJECT_ID(-1)))
    {
        PERF_SCOPE(hit_kill_entity); // 30-game-events.md: GE_DIE to the server (its Lua on_death) inside
        KillEntity(pHDS->whoID);
    }
    // must be last!!! @slipch
    inherited::Hit(pHDS);
}

void CEntity::Load(LPCSTR section)
{
    inherited::Load(section);

    setVisible(FALSE);

    // Team params
    id_Team = READ_IF_EXISTS(pSettings, r_s32, section, "team", -1);
    id_Squad = READ_IF_EXISTS(pSettings, r_s32, section, "squad", -1);
    id_Group = READ_IF_EXISTS(pSettings, r_s32, section, "group", -1);

#pragma todo("Jim to Dima: no specific figures or comments needed")
    m_fMorale = 66.f;

    //время убирания тела с уровня
    m_dwBodyRemoveTime = READ_IF_EXISTS(pSettings, r_u32, section, "body_remove_time", BODY_REMOVE_TIME);
    //////////////////////////////////////
}

bool CEntity::net_Spawn(CSE_Abstract* DC)
{
    m_level_death_time = 0;
    m_game_death_time = 0;
    m_killer_id = ALife::_OBJECT_ID(-1);

    CSE_Abstract* e = (CSE_Abstract*)(DC);
    CSE_ALifeCreatureAbstract* E = smart_cast<CSE_ALifeCreatureAbstract*>(e);

    // Initialize variables
    if (E)
    {
        SetfHealth(E->get_health());

        R_ASSERT2(!((E->get_killer_id() != ALife::_OBJECT_ID(-1)) && g_Alive()),
            make_string(
                "server entity [%s][%d] has an killer [%d] and not dead", E->name_replace(), E->ID, E->get_killer_id())
                .c_str());

        m_killer_id = E->get_killer_id();
        if (m_killer_id == ID())
            m_killer_id = ALife::_OBJECT_ID(-1);
    }
    else
        SetfHealth(1.0f);

    // load damage params
    if (!E)
    {
        // Car or trader only!!!!
        CSE_ALifeCar* C = smart_cast<CSE_ALifeCar*>(e);
        CSE_ALifeTrader* T = smart_cast<CSE_ALifeTrader*>(e);
        CSE_ALifeHelicopter* H = smart_cast<CSE_ALifeHelicopter*>(e);

        R_ASSERT2(C || T || H,
            "Invalid entity (no inheritance from CSE_CreatureAbstract, CSE_ALifeItemCar and CSE_ALifeTrader and "
            "CSE_ALifeHelicopter)!");
        id_Team = id_Squad = id_Group = 0;
    }
    else
    {
        id_Team = E->g_team();
        id_Squad = E->g_squad();
        id_Group = E->g_group();

        CSE_ALifeMonsterBase* monster = smart_cast<CSE_ALifeMonsterBase*>(E);
        if (monster)
        {
            MONSTER_COMMUNITY monster_community;
            monster_community.set(pSettings->r_string(cNameSect().c_str(), "species"));

            if (monster_community.team() != NO_COMMUNITY_INDEX)
                id_Team = monster_community.team();
        }
    }

    // the group key of a previous life of this object is not this life's one
    m_hierarchy_group_set = false;
    if (g_Alive() && IsGameTypeSingle())
    {
        m_registered_member = true;
        register_in_hierarchy(e->ID);
        ++Level().seniority_holder().team(g_Team()).squad(g_Squad()).group(hierarchy_group()).m_dwAliveCount;
    }

    if (!g_Alive())
    {
        m_level_death_time = Device.dwTimeGlobal;
        m_game_death_time = E->m_game_death_time;
        ;
    }

    if (!inherited::net_Spawn(DC))
        return (FALSE);

    //	SetfHealth			(E->fHealth);
    IKinematics* pKinematics = smart_cast<IKinematics*>(Visual());
    CInifile* ini = NULL;

    if (pKinematics)
        ini = pKinematics->LL_UserData();
    if (ini)
    {
        if (ini->section_exist("damage_section"))
            CDamageManager::reload(pSettings->r_string("damage_section", "damage"), ini);

        CParticlesPlayer::LoadParticles(pKinematics);
    }
    return TRUE;
}

void CEntity::net_Destroy()
{
    if (m_registered_member)
    {
        m_registered_member = false;
        if (IsGameTypeSingle())
            Level().seniority_holder().team(g_Team()).squad(g_Squad()).group(hierarchy_group()).unregister_member(this);
    }

    inherited::net_Destroy();

    set_ready_to_save();
}

void CEntity::KillEntity(u16 whoID, bool bypass_actor_check)
{
    if (IsGameTypeSingle() && this->ID() == Actor()->ID())
    {
        Actor()->use_HolderEx(nullptr, true);
#ifdef ACTOR_BEFORE_DEATH_CALLBACK
        //AVO: allow scripts to process actor condition and prevent actor's death or kill him if desired.
        //IMPORTANT: if you wish to kill actor you need to call db.actor:kill(level:object_by_id(whoID), true) in actor_before_death callback, to ensure all objects are properly destroyed
        // this will bypass below if block and go to normal KillEntity routine.
        if (bypass_actor_check == false)
        {
            const ALife::_OBJECT_ID killer_before = m_killer_id;
            const bool has_slot = !!Actor()->callback(GameObject::eActorBeforeDeath);
            Actor()->callback(GameObject::eActorBeforeDeath)(whoID);
            // Stage B (plans/lua_to_cpp/06, B4.7): with the actor group active the engine sends actor_on_before_death
            // itself and finishes the death here. An addon holding the slot may have killed the actor on its own
            // (kill(..., true) sets the killer): then there is nothing left to do.
            if (m_killer_id != killer_before)
                return;
            if (gw::addons::objevents::IsActive(gw::addons::objevents::EGroup::Actor))
            {
                if (!gw::addons::objevents::ActorBeforeDeath(whoID))
                    return; // a subscriber cancelled the death (flags.ret_value = false)
                if (m_killer_id != killer_before)
                    return; // a subscriber killed the actor itself (kill(..., true)): no second death
            }
            // Group disabled: the slot handler decides, as the Lua binder did before stage B. Without a slot (the
            // Lua binder sets none since B-5) the actor dies as it would without the define.
            else if (has_slot)
                return;
        }
        //-AVO
#endif
    }
    if (whoID != ID())
    {
#ifdef DEBUG
        if (m_killer_id != ALife::_OBJECT_ID(-1))
        {
            Msg("! Entity [%s][%s] already has killer with id %d, but new killer id arrived - %d", cNameSect().c_str(),
                cName().c_str(), m_killer_id, whoID);

            IGameObject* old_killer = Level().Objects.net_Find(m_killer_id);
            Msg("! Old killer is %s", old_killer ? old_killer->cName().c_str() : "unknown");

            IGameObject* new_killer = Level().Objects.net_Find(whoID);
            Msg("! New killer is %s", new_killer ? new_killer->cName().c_str() : "unknown");

            VERIFY(m_killer_id == ALife::_OBJECT_ID(-1));
        }
#endif
    }
    else
    {
        if (m_killer_id != ALife::_OBJECT_ID(-1))
            return;
    }

    m_killer_id = whoID;

    set_death_time();

    if (!getDestroy())
    {
        NET_Packet P;
        u_EventGen(P, GE_DIE, ID());
        P.w_u16(u16(whoID));
        P.w_u32(0);
        if (OnServer())
            u_EventSend(P, net_flags(TRUE, TRUE, FALSE, TRUE));
    }
};

// void CEntity::KillEntity(IGameObject* who)
//{
//	VERIFY			(who);
//	if (who) KillEntity(who->ID());
//}

void CEntity::reinit() { inherited::reinit(); }
void CEntity::reload(LPCSTR section)
{
    inherited::reload(section);
    CDamageManager::reload(section, "damage", pSettings);
}

void CEntity::set_death_time()
{
    m_level_death_time = Device.dwTimeGlobal;
    m_game_death_time = ai().get_alife() ? ai().alife().time_manager().game_time() : Level().GetGameTime();
}

bool CEntity::IsFocused() const { return (smart_cast<const CEntity*>(g_pGameLevel->CurrentEntity()) == this); }
bool CEntity::IsMyCamera() const { return (smart_cast<const CEntity*>(g_pGameLevel->CurrentViewEntity()) == this); }
void CEntity::set_ready_to_save() {}
IFactoryObject* CEntity::_construct()
{
    inherited::_construct();
    CDamageManager::_construct();
    m_entity_condition = create_entity_condition(NULL);
    return (this);
}

const u32 FORGET_KILLER_TIME = 180000;

void CEntity::shedule_Update(u32 dt)
{
    {
        ZoneScopedN("sh_CEntity_shedule/inherited");
        inherited::shedule_Update(dt);
    }
    if (!getDestroy() && !g_Alive() && (m_killer_id != u16(-1)))
    {
        ZoneScopedN("sh_CEntity_shedule/forget_killer_packet");
        if (Device.dwTimeGlobal > m_level_death_time + FORGET_KILLER_TIME)
        {
            m_killer_id = u16(-1);
            NET_Packet P;
            u_EventGen(P, GE_ASSIGN_KILLER, ID());
            P.w_u16(u16(-1));
            if (IsGameTypeSingle())
                u_EventSend(P);
        }
    }
}

void CEntity::on_before_change_team() {}
void CEntity::on_after_change_team() {}
void CEntity::ChangeTeam(int team, int squad, int group)
{
    if ((team == g_Team()) && (squad == g_Squad()) && (group == g_Group()))
    {
        // The group by squad: the same team/squad/group still moves a registered stalker whose group key is not the
        // one it should have now (it joined an ALife squad after it registered)
        if (!m_registered_member || !IsGameTypeSingle() ||
            desired_hierarchy_group(ID()) == (hierarchy_group() & entity_hierarchy_base_mask))
            return;
    }

    VERIFY2(g_Alive(), "Try to change team of a dead object");

    if (IsGameTypeSingle())
    {
        VERIFY(m_registered_member);
    }
    // remove from current team
    on_before_change_team();
    Level().seniority_holder().team(g_Team()).squad(g_Squad()).group(hierarchy_group()).unregister_member(this);

    id_Team = team;
    id_Squad = squad;
    id_Group = group;

    // add to new team
    register_in_hierarchy(ID());
    on_after_change_team();
}
