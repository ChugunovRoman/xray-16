#include "pch_script.h"
#include "addon_object_events.h"
#include "InventoryBox.h"
#include "Level.h"
#include "Actor.h"
#include "game_object_space.h"

#include "xrScriptEngine/script_callback_ex.h"
#include "script_game_object.h"
#include "ui/UIActorMenu.h"
#include "UIGameCustom.h"
#include "inventory_item.h"
#include "xrPhysics/IPHStaticGeomShell.h"
#include "Include/xrRender/Kinematics.h"
#ifdef DEBUG
#include "xrEngine/ObjectDump.h"
#endif

CInventoryBox::CInventoryBox()
{
    m_in_use = false;
    m_can_take = true;
    m_closed = false;
}

CInventoryBox::~CInventoryBox() {}
void CInventoryBox::OnEvent(NET_Packet& P, u16 type)
{
    inherited::OnEvent(P, type);

    switch (type)
    {
    case GE_TRADE_BUY:
    case GE_OWNERSHIP_TAKE:
    {
        u16 id;
        P.r_u16(id);
        IGameObject* itm = Level().Objects.net_Find(id);
        VERIFY(itm);
        m_items.push_back(id);
        itm->H_SetParent(this);
        itm->setVisible(FALSE);
        itm->setEnabled(FALSE);

        CInventoryItem* pIItem = smart_cast<CInventoryItem*>(itm);
        VERIFY(pIItem);
        if (CurrentGameUI())
        {
            if (CurrentGameUI()->GetActorMenu().GetMenuMode() == mmDeadBodySearch)
            {
                if (this == CurrentGameUI()->GetActorMenu().GetInvBox())
                    CurrentGameUI()->OnInventoryAction(pIItem, GE_OWNERSHIP_TAKE);
            }
        };
    }
    break;

    case GE_TRADE_SELL:
    case GE_OWNERSHIP_REJECT:
    {
        u16 id;
        P.r_u16(id);
        IGameObject* itm = Level().Objects.net_Find(id);
        VERIFY(itm);
        xr_vector<u16>::iterator it;
        it = std::find(m_items.begin(), m_items.end(), id);
        VERIFY(it != m_items.end());
        m_items.erase(it);

        bool just_before_destroy = !P.r_eof() && P.r_u8();
        bool dont_create_shell = (type == GE_TRADE_SELL) || just_before_destroy;

        itm->H_SetParent(NULL, dont_create_shell);

        if (m_in_use)
        {
            CGameObject* GO = smart_cast<CGameObject*>(itm);
            Actor()->callback(GameObject::eInvBoxItemTake)(this->lua_game_object(), GO->lua_game_object());
            gw::addons::objevents::ActorItemTakeFromBox(this, GO);
        }
    }
    break;
    };
}

void CInventoryBox::UpdateCL()
{
    ZoneScopedN("ucl_CInventoryBox");
    inherited::UpdateCL();
}
void CInventoryBox::net_Destroy()
{
    DestroyStaticGeomShell(m_static_shell);
    inherited::net_Destroy();
}

void CInventoryBox::Load(LPCSTR section)
{
    inherited::Load(section);
    m_static_collision = !!READ_IF_EXISTS(pSettings, r_bool, section, "static_collision", false);
    m_npc_collision = !!READ_IF_EXISTS(pSettings, r_bool, section, "npc_collision", true);
    m_is_ai_obstacle =
        !!READ_IF_EXISTS(pSettings, r_bool, section, "is_ai_obstacle", m_static_collision && m_npc_collision);
}

// Read in Load (the getter runs for the objects near a walking NPC on the main thread and in the path search on the
// Game thread - no ini lookup here, as CPhysicObject F5). A box without the keys stays no obstacle, as before
bool CInventoryBox::is_ai_obstacle() const { return m_is_ai_obstacle; }

// static_collision (plans/addon_base_building/plan.md 17.37): a placed stash of the base building addon stands as the
// level - the actor walks on it and does not pass through it, the dropped items and the grenades lie on it. The shapes
// of all the bones of the model in the bind pose at XFORM after inherited::net_Spawn (CGameObject::net_Spawn has put
// the box on the AI mesh already); the box never moves while online (a teleport takes it offline: net_Destroy, then
// a new net_Spawn builds the shell again). Ray picks (F, the crosshair) and bullets use the collision form
// (CCF_Skeleton), not the shell
void CInventoryBox::CreateStaticCollision()
{
    if (m_static_shell)
        return;
    m_static_shell = P_BuildStaticGeomShellFromBones(this);
    if (!m_static_shell)
    {
        Msg("! [static_collision] [%s] visual [%s]: no bone with a physics shape - the inventory box has no collision",
            cNameSect().c_str(), cNameVisual().c_str());
        return;
    }
    if (!m_npc_collision)
        P_StaticGeomShellAddContactCallback(m_static_shell, P_IgnoreNpcCharacterContact);
}

Fmatrix& CInventoryBox::ObjectXFORM() { return XFORM(); }
Fvector& CInventoryBox::ObjectPosition() { return Position(); }
// NameObject directly, as CPhysicsShellHolder::ObjectName (no shared_str copy: called from physics worker tasks too)
LPCSTR CInventoryBox::ObjectName() const { return NameObject.c_str(); }
LPCSTR CInventoryBox::ObjectNameVisual() const { return cNameVisual().c_str(); }
LPCSTR CInventoryBox::ObjectNameSect() const { return cNameSect().c_str(); }
bool CInventoryBox::ObjectGetDestroy() const { return getDestroy(); }
u16 CInventoryBox::ObjectID() const { return ID(); }
ICollisionForm* CInventoryBox::ObjectCollisionModel() { return CForm; }
IKinematics* CInventoryBox::ObjectKinematics() { return Visual() ? Visual()->dcast_PKinematics() : nullptr; }
bool CInventoryBox::has_parent_object() { return !!H_Parent(); }
#ifdef DEBUG
std::string CInventoryBox::dump(EDumpType /*type*/) const { return dbg_object_base_dump_string(this); }
#endif
#include "xrServerEntities/xrServer_Objects_ALife.h"
bool CInventoryBox::net_Spawn(CSE_Abstract* DC)
{
    inherited::net_Spawn(DC);
    if (m_static_collision)
        CreateStaticCollision();
    setVisible(TRUE);
    setEnabled(TRUE);
    set_tip_text("inventory_box_use");

    CSE_ALifeInventoryBox* pSE_box = smart_cast<CSE_ALifeInventoryBox*>(DC);
    if (/*IsGameTypeSingle() &&*/ pSE_box)
    {
        m_can_take = pSE_box->m_can_take;
        m_closed = pSE_box->m_closed;
        set_tip_text(pSE_box->m_tip_text.c_str());
    }

    return TRUE;
}

void CInventoryBox::net_Relcase(IGameObject* O) { inherited::net_Relcase(O); }
#include "inventory_item.h"
void CInventoryBox::AddAvailableItems(TIItemContainer& items_container) const
{
    xr_vector<u16>::const_iterator it = m_items.begin();
    xr_vector<u16>::const_iterator it_e = m_items.end();

    for (; it != it_e; ++it)
    {
        PIItem itm = smart_cast<PIItem>(Level().Objects.net_Find(*it));
        VERIFY(itm);
        items_container.push_back(itm);
    }
}

void CInventoryBox::set_can_take(bool status)
{
    m_can_take = status;
    SE_update_status();
}

void CInventoryBox::set_closed(bool status, LPCSTR reason)
{
    m_closed = status;

    if (reason && xr_strlen(reason))
    {
        set_tip_text(reason);
    }
    else
    {
        set_tip_text("inventory_box_use");
    }
    SE_update_status();
}

void CInventoryBox::SE_update_status()
{
    NET_Packet P;
    CGameObject::u_EventGen(P, GE_INV_BOX_STATUS, ID());
    P.w_u8((m_can_take) ? 1 : 0);
    P.w_u8((m_closed) ? 1 : 0);
    P.w_stringZ(tip_text());
    CGameObject::u_EventSend(P);
}
