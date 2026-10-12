////////////////////////////////////////////////////////////////////////////
//	Module 		: inventory_item_object.cpp
//	Created 	: 24.03.2003
//  Modified 	: 27.12.2004
//	Author		: Victor Reutsky, Yuri Dobronravin
//	Description : Inventory item object implementation
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "inventory_item_object.h"
#include "xrCore/Profiling/PerfMetrics.hpp"

CInventoryItemObject::CInventoryItemObject() {}
CInventoryItemObject::~CInventoryItemObject() {}
IFactoryObject* CInventoryItemObject::_construct()
{
    CInventoryItem::_construct();
    CPhysicItem::_construct();
    return (this);
}

void CInventoryItemObject::Load(LPCSTR section)
{
    CPhysicItem::Load(section);
    CInventoryItem::Load(section);
}
/* remove
LPCSTR CInventoryItemObject::Name			()
{
    return						(CInventoryItem::Name());
}

LPCSTR CInventoryItemObject::NameShort		()
{
    return						(CInventoryItem::NameShort());
}
*/
/*
LPCSTR CInventoryItemObject::NameComplex	()
{
    return						(CInventoryItem::NameComplex());
}
*/

void CInventoryItemObject::Hit(SHit* pHDS)
{
    CPhysicItem::Hit(pHDS);
    CInventoryItem::Hit(pHDS);
}

void CInventoryItemObject::OnH_B_Independent(bool just_before_destroy)
{
    CInventoryItem::OnH_B_Independent(just_before_destroy);
    CPhysicItem::OnH_B_Independent(just_before_destroy);
}

void CInventoryItemObject::OnH_A_Independent()
{
    CInventoryItem::OnH_A_Independent();
    CPhysicItem::OnH_A_Independent();
}

void CInventoryItemObject::OnH_B_Chield()
{
    CPhysicItem::OnH_B_Chield();
    CInventoryItem::OnH_B_Chield();
}

void CInventoryItemObject::OnH_A_Chield()
{
    CPhysicItem::OnH_A_Chield();
    CInventoryItem::OnH_A_Chield();
}

void CInventoryItemObject::UpdateCL()
{
    ZoneScopedN("ucl_CInventoryItemObject");
    CPhysicItem::UpdateCL();
    CInventoryItem::UpdateCL();
}

void CInventoryItemObject::OnEvent(NET_Packet& P, u16 type)
{
    CPhysicItem::OnEvent(P, type);
    CInventoryItem::OnEvent(P, type);
}

bool CInventoryItemObject::net_Spawn(CSE_Abstract* DC)
{
    CInventoryItem::net_Spawn_install_upgrades(DC);
    BOOL res = CPhysicItem::net_Spawn(DC);
    CInventoryItem::net_Spawn(DC);
    return (res);
}

void CInventoryItemObject::net_Destroy()
{
    CInventoryItem::net_Destroy();
    CPhysicItem::net_Destroy();
}

void CInventoryItemObject::net_Import(NET_Packet& P) { CInventoryItem::net_Import(P); }
void CInventoryItemObject::net_Export(NET_Packet& P) { CInventoryItem::net_Export(P); }
void CInventoryItemObject::save(NET_Packet& packet)
{
    CPhysicItem::save(packet);
    CInventoryItem::save(packet);
}

void CInventoryItemObject::load(IReader& packet)
{
    CPhysicItem::load(packet);
    CInventoryItem::load(packet);
}

// Both bases draw the same thing: CPhysicItem -> CGameObject::renderable_Render (add_Visual(Visual(), XFORM()) plus
// the main pass frame, the bones prepass note, MakeMeCrow, hom_frame) and CInventoryItem -> CAttachableItem (only
// add_Visual(object().Visual(), object().XFORM()) - the same object, visual and matrix). The second one is a leftover
// of the original code: the render drops its leaves by the visual's marker (R_dsgraph_structure::insert_dynamic), but
// walks the model again (add_leafs_dynamic: SMAP blob of distant skeletons, CalculateBones / wallmarks fast-outs).
// plans/optimization_mt/07-item-single-render.md: always only the CGameObject one.
void CInventoryItemObject::renderable_Render(u32 context_id, IRenderable* root)
{
    PERF_INC(item_render_add_visual);
    CPhysicItem::renderable_Render(context_id, root);
}

void CInventoryItemObject::reload(LPCSTR section)
{
    CPhysicItem::reload(section);
    CInventoryItem::reload(section);
}

void CInventoryItemObject::reinit()
{
    CInventoryItem::reinit();
    CPhysicItem::reinit();
}

void CInventoryItemObject::activate_physic_shell() { CInventoryItem::activate_physic_shell(); }
void CInventoryItemObject::on_activate_physic_shell() { CPhysicItem::activate_physic_shell(); }
void CInventoryItemObject::make_Interpolation() { CInventoryItem::make_Interpolation(); }
void CInventoryItemObject::PH_B_CrPr() { CInventoryItem::PH_B_CrPr(); }
void CInventoryItemObject::PH_I_CrPr() { CInventoryItem::PH_I_CrPr(); }
#ifdef DEBUG
void CInventoryItemObject::PH_Ch_CrPr() { CInventoryItem::PH_Ch_CrPr(); }
#endif

void CInventoryItemObject::PH_A_CrPr() { CInventoryItem::PH_A_CrPr(); }
#ifdef DEBUG
void CInventoryItemObject::OnRender() { CInventoryItem::OnRender(); }
#endif

void CInventoryItemObject::modify_holder_params(float& range, float& fov) const
{
    CInventoryItem::modify_holder_params(range, fov);
}

u32 CInventoryItemObject::ef_weapon_type() const { return (0); }
bool CInventoryItemObject::NeedToDestroyObject() const { return CInventoryItem::NeedToDestroyObject(); }
bool CInventoryItemObject::Useful() const { return (CInventoryItem::Useful()); }
