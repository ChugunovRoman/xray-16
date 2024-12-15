////////////////////////////////////////////////////////////////////////////
//	Module 		: eatable_item_object.cpp
//	Created 	: 24.03.2003
//  Modified 	: 29.01.2004
//	Author		: Yuri Dobronravin
//	Description : Eatable item object implementation
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "eatable_item_object.h"
#include "player_hud.h"
#include "CustomDetector.h"
#include "ActorEffector.h"

CEatableItemObject::CEatableItemObject() {}
CEatableItemObject::~CEatableItemObject() {}
IFactoryObject* CEatableItemObject::_construct()
{
    CEatableItem::_construct();
    CPhysicItem::_construct();
    CHudItem::_construct();
    return (this);
}

void CEatableItemObject::Load(LPCSTR section)
{
    CHudItem::Load(section);
    CPhysicItem::Load(section);
    CEatableItem::Load(section);

    LoadCamAnims(section);

    m_sounds.LoadSound(section, "use_anm_sound", "useAnmSound");
}

void CEatableItemObject::Hit(SHit* pHDS)
{
    CPhysicItem::Hit(pHDS);
    CEatableItem::Hit(pHDS);
}

void CEatableItemObject::OnH_A_Independent()
{
    CHudItem::OnH_A_Independent();
    CEatableItem::OnH_A_Independent();
    CPhysicItem::OnH_A_Independent();
    // If we are dropping used item before removing - don't show it
    if (!Useful())
    {
        setVisible(false);
        setEnabled(false);
    }
}

void CEatableItemObject::OnH_B_Independent(bool just_before_destroy)
{
    CHudItem::OnH_B_Independent(just_before_destroy);
    CEatableItem::OnH_B_Independent(just_before_destroy);
    CPhysicItem::OnH_B_Independent(just_before_destroy);
}

void CEatableItemObject::OnH_B_Chield()
{
    CPhysicItem::OnH_B_Chield();
    CEatableItem::OnH_B_Chield();
    CHudItem::OnH_B_Chield();
}

void CEatableItemObject::OnH_A_Chield()
{
    CPhysicItem::OnH_A_Chield();
    CEatableItem::OnH_A_Chield();
    CHudItem::OnH_A_Chield();
}

void CEatableItemObject::UpdateCL()
{
    CPhysicItem::UpdateCL();
    CEatableItem::UpdateCL();
    CHudItem::UpdateCL();

    if (GetState() == eHidden)
        return;
}

void CEatableItemObject::OnEvent(NET_Packet& P, u16 type)
{
    CPhysicItem::OnEvent(P, type);
    CEatableItem::OnEvent(P, type);
    CHudItem::OnEvent(P, type);
}

bool CEatableItemObject::net_Spawn(CSE_Abstract* DC)
{
    bool res = CPhysicItem::net_Spawn(DC);
    CEatableItem::net_Spawn(DC);
    CHudItem::net_Spawn(DC);

    return res;
}

void CEatableItemObject::net_Destroy()
{
    CHudItem::net_Destroy();
    CEatableItem::net_Destroy();
    CPhysicItem::net_Destroy();
}

void CEatableItemObject::net_Import(NET_Packet& P) { CEatableItem::net_Import(P); }
void CEatableItemObject::net_Export(NET_Packet& P) { CEatableItem::net_Export(P); }
void CEatableItemObject::save(NET_Packet& packet)
{
    CPhysicItem::save(packet);
    CEatableItem::save(packet);

    packet.w_u32(GetState());
}

void CEatableItemObject::load(IReader& packet)
{
    CPhysicItem::load(packet);
    CEatableItem::load(packet);

    const u32 state = packet.r_u32();
    SetState(state);
    SwitchState(state);
}

void CEatableItemObject::renderable_Render(u32 context_id, IRenderable* root)
{
    CHudItem::renderable_Render(context_id, root);
    CPhysicItem::renderable_Render(context_id, root);
    CEatableItem::renderable_Render(context_id, root);
}

void CEatableItemObject::reload(LPCSTR section)
{
    CPhysicItem::reload(section);
    CEatableItem::reload(section);
}

void CEatableItemObject::reinit()
{
    CEatableItem::reinit();
    CPhysicItem::reinit();
}

void CEatableItemObject::activate_physic_shell() { CEatableItem::activate_physic_shell(); }
void CEatableItemObject::on_activate_physic_shell() { CPhysicItem::activate_physic_shell(); }
void CEatableItemObject::make_Interpolation() { CEatableItem::make_Interpolation(); }
void CEatableItemObject::PH_B_CrPr() { CEatableItem::PH_B_CrPr(); }
void CEatableItemObject::PH_I_CrPr() { CEatableItem::PH_I_CrPr(); }
#ifdef DEBUG
void CEatableItemObject::PH_Ch_CrPr() { CEatableItem::PH_Ch_CrPr(); }
#endif

void CEatableItemObject::PH_A_CrPr() { CEatableItem::PH_A_CrPr(); }
#ifdef DEBUG
void CEatableItemObject::OnRender() { CEatableItem::OnRender(); }
#endif

bool CEatableItemObject::NeedToDestroyObject() const { return CInventoryItem::NeedToDestroyObject(); }
u32 CEatableItemObject::ef_weapon_type() const { return (0); }
bool CEatableItemObject::Useful() const { return (CEatableItem::Useful()); }

void CEatableItemObject::LoadCamAnims(LPCSTR section)
{
    const CInifile::Sect& _sect = pSettings->r_section(section);

    for (const auto& [name, anm] : _sect.Data)
    {
        if (0 == strncmp(name.c_str(), "cam_anm_",  sizeof("cam_anm_")  - 1))
        {
            const int count = _GetItemCount(anm.c_str());
            string512 str_item;
            _GetItem(anm.c_str(), Random.randI(0, count), str_item);
            cam_anims[name] = anm;
        }
    }
}
void CEatableItemObject::PlayCamAnim(LPCSTR name)
{
    if (!psActorFlags.test(AF_USE_CAM_ANIMS))
        return;

    if (CActor* pActor = smart_cast<CActor*>(H_Parent()))
    {
        shared_str anms = cam_anims[name];
        if (*anms)
        {
            const int count = _GetItemCount(anms.c_str());
            string512 str_item;
            _GetItem(anms.c_str(), Random.randI(0, count), str_item);

            if (!strstr(str_item, ".anm"))
                xr_strcat(str_item, ".anm");

            string_path fn;
            if (!FS.exist(fn, "$game_anims$", str_item))
                FATAL(make_string("! ERROR: Cam animation doesn't exist '%s' for prop '%s' in weapon '%s'", str_item, name, *cName()).c_str());

            CAnimatorCamEffectorScriptCB* e = xr_new<CAnimatorCamEffectorScriptCB>("");
            e->SetType(ECamEffectorType::cefAnsel);
            e->SetCyclic(false);
            e->Start(str_item);
            pActor->Cameras().AddCamEffector(e);
        }
    }
}

bool CEatableItemObject::UseBy()
{
    CInventoryOwner* m_pOwner = Parent->cast_inventory_owner();
    if (!m_pOwner)
        return false;

    CEntityAlive* entity_alive = smart_cast<CEntityAlive*>(m_pOwner);
    if (!entity_alive)
        return false;

    UseBy(entity_alive);

    return true;
}
bool CEatableItemObject::UseBy(CEntityAlive* entity_alive)
{
    CEatableItem::UseBy(entity_alive);

    return true;
}

bool CEatableItemObject::Action(u16 cmd, u32 flags)
{
    CInventoryItem::Action(cmd, flags);
    CHudItem::Action(cmd, flags);

    return IsPending();
}

void CEatableItemObject::SwitchState(u32 S)
{
    CHudItem::SwitchState(S);
}
void CEatableItemObject::OnStateSwitch(u32 S, u32 oldState)
{
    CHudItem::OnStateSwitch(S, oldState);

    switch (S)
    {
    case eShowing:
    {
        if (Parent)
        {
            CInventoryOwner* m_pOwner = Parent->cast_inventory_owner();
            if (m_pOwner && m_pOwner->object_id() == 0)
            {
                g_player_hud[0]->attach_item(this);
                m_sounds.PlaySound("useAnmSound", Fvector().set(0, 0, 0), this, true, false);
                PlayHUDMotion("anm_use", "anim_use", FALSE, this, GetState());
                PlayCamAnim("cam_anm_use");
                SetPending(true);
            }
            else
            {
                UseBy();
                SwitchState(eHiding);
            }
        }
        else
        {
            UseBy();
            SwitchState(eHiding);
        }
    }
    break;
    case eHiding:
    {
        if (prev_slot != NO_ACTIVE_SLOT)
            RestoreSlot();

        RemoveItemIfNecessaryOrMoveToRuck();
    }
    break;
    }
}

void CEatableItemObject::RemoveItemIfNecessaryOrMoveToRuck()
{
    if (Empty())
    {
        if (!CanDelete())
            return;
        SetDropManual(true);
    }

    CInventoryOwner* m_pOwner = Parent->cast_inventory_owner();
    if (!m_pOwner)
        return;

    m_pOwner->inventory().Ruck(this);
}
void CEatableItemObject::RestoreSlot()
{
    if (!Parent)
        return;

    CInventoryOwner* m_pOwner = Parent->cast_inventory_owner();
    if (!m_pOwner || m_pOwner->object_id() != 0)
        return;

    m_pOwner->inventory().SetActiveSlot(prev_slot);

    if (restor_detector)
    {
        if (CCustomDetector* detector = smart_cast<CCustomDetector*>(Actor()->inventory().ItemFromSlot(DETECTOR_SLOT)))
            detector->ShowDetector(CCustomDetector::eFast);
    }

    restor_detector = false;
}

void CEatableItemObject::OnAnimationEnd(u32 state)
{
    CHudItem::OnAnimationEnd(state);

    switch (state)
    {
    case eShowing:
    {
        UseBy();
        SetPending(false);
        SwitchState(eHiding);
    }
    break;
    }
}

void CEatableItemObject::OnActiveItem()
{
    CInventoryOwner* m_pOwner = Parent->cast_inventory_owner();
    if (!m_pOwner)
        return;

    const u16 prevSlot = m_pOwner->inventory().GetActiveSlot();
    if (prevSlot != NO_ACTIVE_SLOT && prevSlot != SLOTS_COUNT)
        prev_slot = prevSlot;

    if (g_player_hud[0]->attached_item())
        g_player_hud[0]->detach_all_items();
    if (g_player_hud[1]->attached_item())
        g_player_hud[1]->detach_all_items();

    SwitchState(eShowing);
    CHudItem::OnActiveItem();
    SetNextState(eHiding);
}

void CEatableItemObject::OnMoveToRuck(const SInvItemPlace& prev)
{
    CInventoryItem::OnMoveToRuck(prev);
    CHudItem::OnMoveToRuck(prev);
}

bool CEatableItemObject::ActivateItem()
{
    return CHudItem::ActivateItem();
}
void CEatableItemObject::DeactivateItem()
{
    CHudItem::DeactivateItem();
}

void CEatableItemObject::on_renderable_Render(u32 context_id, IRenderable* root)
{
    CInventoryItem::renderable_Render(context_id, root);
}

void CEatableItemObject::UpdateXForm()
{
    CInventoryItem::UpdateXForm();
}
