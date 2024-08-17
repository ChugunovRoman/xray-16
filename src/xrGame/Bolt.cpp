#include "StdAfx.h"
#include "Bolt.h"
#include "ParticlesObject.h"
#include "xrPhysics/PhysicsShell.h"
#include "xrEngine/xr_level_controller.h"

CBolt::CBolt(void) { m_thrower_id = u16(-1); }
CBolt::~CBolt(void) {}
void CBolt::OnH_A_Chield()
{
    inherited::OnH_A_Chield();
    IGameObject* o = H_Parent()->H_Parent();
    if (o)
        SetInitiator(o->ID());
}

void CBolt::Load(LPCSTR section)
{
    inherited::Load(section);

    m_count = pSettings->r_u16(section, "count");

    if (pSettings->line_exist(section, "tip_text"))
        set_tip_text(pSettings->r_string(section, "tip_text"));
}

void CBolt::save(NET_Packet& output_packet)
{
    inherited::save(output_packet);
    CInventoryItemObject::save(output_packet);

    output_packet.w_u16(m_count);
}
void CBolt::load(IReader& input_packet)
{
    inherited::load(input_packet);
    CInventoryItemObject::load(input_packet);

    m_count = input_packet.r_u16();
}

bool CBolt::GetBriefInfo(II_BriefInfo& info)
{
    VERIFY(m_pInventory);
    info.clear();

    info.name._set(m_nameShort);
    info.icon._set(m_ammo_icon);

    string16 stmp;
    xr_sprintf(stmp, "%d", m_count);
    info.cur_ammo._set(stmp);
    return true;
}

void CBolt::Throw()
{
    CMissile* l_pBolt = smart_cast<CMissile*>(m_fake_missile);
    if (!l_pBolt)
        return;
    l_pBolt->set_destroy_time(u32(m_dwDestroyTimeMax / phTimefactor));
    inherited::Throw();
    spawn_fake_missile();
    AddCount(-1);

    NET_Packet P;
    P.w_begin(M_EVENT);
    P.w_u32(Device.dwTimeGlobal);
    P.w_u16(GE_WPN_AMMO_ADD);
    P.w_u16(ID());
    P.w_u16(m_count);
    Level().Send(P, net_flags(TRUE, TRUE));

    if (m_count == 0)
        DestroyObject();
}

void CBolt::spawn_fake_missile()
{
    if (OnClient())
        return;

    if (!getDestroy())
    {
        CSE_Abstract* object = Level().spawn_item(
            "bolt", Position(), (GEnv.isDedicatedServer) ? u32(-1) : ai_location().level_vertex_id(), ID(), true);

        CSE_ALifeObject* alife_object = smart_cast<CSE_ALifeObject*>(object);
        VERIFY(alife_object);
        alife_object->m_flags.set(CSE_ALifeObject::flCanSave, FALSE);

        NET_Packet P;
        object->Spawn_Write(P, TRUE);
        Level().Send(P, net_flags(TRUE));
        F_entity_Destroy(object);
    }
}

void CBolt::OnAnimationEnd(u32 state)
{
    switch (state)
    {
    case eHiding:
    {
        setVisible(FALSE);
        SwitchState(eHidden);
    }
    break;
    case eShowing:
    {
        setVisible(TRUE);
        SwitchState(eIdle);
    }
    break;
    case eThrowStart:
    {
        if (!m_fake_missile && !smart_cast<CMissile*>(H_Parent()))
            spawn_fake_missile();

        if (m_throw)
            SwitchState(eThrow);
        else
            SwitchState(eReady);
    }
    break;
    case eThrow: { SwitchState(eThrowEnd);
    }
    break;
    case eThrowEnd: { SwitchState(eShowing);
    }
    break;
    default: inherited::OnAnimationEnd(state);
    }
}

bool CBolt::Useful() const { return true; }
bool CBolt::Action(u16 cmd, u32 flags)
{
    if (inherited::Action(cmd, flags))
        return true;
    /*
        switch(cmd)
        {
        case kDROP:
            {
                if(flags&CMD_START)
                {
                    m_throw = false;
                    if(State() == MS_IDLE) State(MS_THREATEN);
                }
                else if(State() == MS_READY || State() == MS_THREATEN)
                {
                    m_throw = true;
                    if(State() == MS_READY) State(MS_THROW);
                }
            }
            return true;
        }
    */
    return false;
}

void CBolt::activate_physic_shell()
{
    inherited::activate_physic_shell();
    m_pPhysicsShell->SetAirResistance(.0001f);
}

void CBolt::SetInitiator(u16 id) { m_thrower_id = id; }
u16 CBolt::Initiator() { return m_thrower_id; }

