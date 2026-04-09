#include "StdAfx.h"
#include "hud_item_object.h"
#include "ActorEffector.h"

CHudItemObject::CHudItemObject() {}
CHudItemObject::~CHudItemObject() {}
IFactoryObject* CHudItemObject::_construct()
{
    CInventoryItemObject::_construct();
    CHudItem::_construct();
    return (this);
}

void CHudItemObject::Load(LPCSTR section)
{
    CInventoryItemObject::Load(section);
    CHudItem::Load(section);

    LoadCamAnims(section);
}

void CHudItemObject::LoadCamAnims(LPCSTR section)
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
void CHudItemObject::PlayCamAnim(LPCSTR name)
{
    if (!psActorFlags.test(AF_USE_CAM_ANIMS))
        return;

    if (CActor* pActor = smart_cast<CActor*>(H_Parent()))
    {
        shared_str anms = cam_anims[name];
        if (anms.c_str())
        {
            const int count = _GetItemCount(anms.c_str());
            string512 str_item;
            _GetItem(anms.c_str(), Random.randI(0, count), str_item);

            if (!strstr(str_item, ".anm"))
                xr_strcat(str_item, ".anm");

            string_path fn;
            if (!FS.exist(fn, "$game_anims$", str_item))
                FATAL(make_string("! ERROR: Cam animation doesn't exist '%s' for prop '%s' in weapon '%s'", str_item, name, cName().c_str()).c_str());

            CAnimatorCamEffectorScriptCB* e = xr_new<CAnimatorCamEffectorScriptCB>("");
            e->SetType(ECamEffectorType::cefAnsel);
            e->SetCyclic(false);
            e->Start(str_item);
            pActor->Cameras().AddCamEffector(e);
        }
    }
}

bool CHudItemObject::Action(u16 cmd, u32 flags)
{
    if (CInventoryItemObject::Action(cmd, flags))
        return (true);
    return (CHudItem::Action(cmd, flags));
}

void CHudItemObject::SwitchState(u32 S) { CHudItem::SwitchState(S); }
void CHudItemObject::OnStateSwitch(u32 S, u32 oldState) { CHudItem::OnStateSwitch(S, oldState); }
void CHudItemObject::OnMoveToRuck(const SInvItemPlace& prev)
{
    CInventoryItemObject::OnMoveToRuck(prev);
    CHudItem::OnMoveToRuck(prev);
}

void CHudItemObject::OnEvent(NET_Packet& P, u16 type)
{
    CInventoryItemObject::OnEvent(P, type);
    CHudItem::OnEvent(P, type);
}

void CHudItemObject::OnH_A_Chield()
{
    CHudItem::OnH_A_Chield();
    CInventoryItemObject::OnH_A_Chield();
}

void CHudItemObject::OnH_B_Chield()
{
    CInventoryItemObject::OnH_B_Chield();
    CHudItem::OnH_B_Chield();
}

void CHudItemObject::OnH_B_Independent(bool just_before_destroy)
{
    CHudItem::OnH_B_Independent(just_before_destroy);
    CInventoryItemObject::OnH_B_Independent(just_before_destroy);
}

void CHudItemObject::OnH_A_Independent()
{
    CHudItem::OnH_A_Independent();
    CInventoryItemObject::OnH_A_Independent();
}

bool CHudItemObject::net_Spawn(CSE_Abstract* DC)
{
    return (CInventoryItemObject::net_Spawn(DC) && CHudItem::net_Spawn(DC));
}

void CHudItemObject::net_Destroy()
{
    CHudItem::net_Destroy();
    CInventoryItemObject::net_Destroy();
}

bool CHudItemObject::ActivateItem() { return (CHudItem::ActivateItem()); }
void CHudItemObject::DeactivateItem() { CHudItem::DeactivateItem(); }
void CHudItemObject::UpdateCL()
{
    ZoneScopedN("ucl_CHudItemObject");
    CInventoryItemObject::UpdateCL();
    CHudItem::UpdateCL();
}

void CHudItemObject::renderable_Render(u32 context_id, IRenderable* root) { CHudItem::renderable_Render(context_id, root); }
void CHudItemObject::on_renderable_Render(u32 context_id, IRenderable* root) { CInventoryItemObject::renderable_Render(context_id, root); }
