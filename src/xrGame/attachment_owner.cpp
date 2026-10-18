////////////////////////////////////////////////////////////////////////////
//	Module 		: attachment_owner.cpp
//	Created 	: 12.02.2004
//  Modified 	: 12.02.2004
//	Author		: Dmitriy Iassenev
//	Description : Attachment owner
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "attachment_owner.h"
#include "attachable_item.h"
#include "Include/xrRender/Kinematics.h"
#include "inventory_item.h"
#include "PhysicsShellHolder.h"
#include "xrEngine/IGame_Level.h"

static void AttachmentCallback(IKinematics* tpKinematics);

namespace
{
// B2.2 (plans/optimization_mt/16-b2-overlap-races.md): attach/detach asked on the Game logic thread in a game/render
// overlap frame. The render meanwhile walks m_attached_objects (push_back may reallocate it under the walk) and calls
// AttachmentCallback through VisualCallback (the last detach resets the kinematics callback with two plain stores:
// Param may be read as null), so the changes wait for the end of the frame. Written only on the Game thread inside
// the window, applied and cleared only on the main thread after the join: no lock.
struct AttachmentOwnerQueuedChange
{
    u16 owner_id;
    u16 item_id;
    bool attach;
    bool set_visible; // CAttachableItem::enable: setVisible of the item together with the change
};
xr_vector<AttachmentOwnerQueuedChange> s_attachment_owner_queue;
u32 s_attachment_owner_queue_frame = u32(-1); // the frame whose PostJoin applies the queue

void AttachmentOwnerApplyQueue()
{
    xr_vector<AttachmentOwnerQueuedChange> changes;
    changes.swap(s_attachment_owner_queue);
    if (!g_pGameLevel)
        return;
    for (const AttachmentOwnerQueuedChange& change : changes)
    {
        // By IDs: both objects are looked up again
        CGameObject* owner_object = smart_cast<CGameObject*>(g_pGameLevel->Objects.net_Find(change.owner_id));
        CGameObject* item_object = smart_cast<CGameObject*>(g_pGameLevel->Objects.net_Find(change.item_id));
        CAttachmentOwner* owner = owner_object ? smart_cast<CAttachmentOwner*>(owner_object) : nullptr;
        CInventoryItem* item = item_object ? smart_cast<CInventoryItem*>(item_object) : nullptr;
        if (!owner || !item)
            continue;
        const bool owned = item_object->H_Parent() == owner_object;
        if (change.attach)
        {
            // What OnItemTake/OnItemSlot/enable relied on when they asked: both alive, the item is still the owner's
            if (!owned || owner_object->getDestroy() || item_object->getDestroy() || !owner_object->Visual())
                continue;
            // Virtual (CObjectHandler switches the torch on after it) and immediate now: the window is closed.
            // It checks "already attached" and can_attach itself.
            owner->attach(item);
            if (change.set_visible)
                item_object->setVisible(true);
        }
        else
        {
            owner->detach(item); // a no-op when it is not attached (any more)
            if (change.set_visible && owned)
                item_object->setVisible(false);
        }
    }
}
} // namespace

bool CAttachmentOwner::queue_attachment_change(const CInventoryItem* inventory_item, bool attach, bool set_visible)
{
    if (!CGameObject::ShouldDeferToPostJoin())
        return false;

    const u16 owner_id = cast_game_object()->ID();
    const u16 item_id = inventory_item->object().ID();
    if (!set_visible)
    {
        // Nothing of this owner is queued: m_attached_objects is current, so the immediate call's own early-outs are
        // decided here and most calls (OnItemRuck/OnItemDrop of never attached items, attach of not attachable ones)
        // queue nothing. Otherwise queue: the apply replays the calls in order with the same checks.
        const bool owner_queued = std::any_of(s_attachment_owner_queue.begin(), s_attachment_owner_queue.end(),
            [owner_id](const AttachmentOwnerQueuedChange& change) { return change.owner_id == owner_id; });
        if (!owner_queued)
        {
            const bool is_attached = attachedItem(item_id) != nullptr;
            if (attach ? (is_attached || !can_attach(inventory_item)) : !is_attached)
                return true;
        }
    }

    // One PostJoin per frame; by the frame number, not by "the queue was empty", so a queue never applied (the frame
    // loop stopped) cannot block the next ones.
    if (s_attachment_owner_queue_frame != Device.dwFrame)
    {
        s_attachment_owner_queue_frame = Device.dwFrame;
        Device.PostJoin(AttachmentOwnerApplyQueue);
    }
    s_attachment_owner_queue.push_back({ owner_id, item_id, attach, set_visible });
    return true;
}

CAttachmentOwner::~CAttachmentOwner() {}
void CAttachmentOwner::reload(LPCSTR section)
{
    if (!pSettings->line_exist(section, "attachable_items"))
    {
        m_attach_item_sections.clear();
        return;
    }

    LPCSTR attached_sections = pSettings->r_string(section, "attachable_items");
    u32 item_count = _GetItemCount(attached_sections);
    string256 current_item_section;
    m_attach_item_sections.resize(item_count);
    for (u32 i = 0; i < item_count; ++i)
        m_attach_item_sections[i] = _GetItem(attached_sections, i, current_item_section);
}

void CAttachmentOwner::reinit() { VERIFY(m_attached_objects.empty()); }
void CAttachmentOwner::net_Destroy()
{
    if (!m_attached_objects.empty())
    {
        CGameObject* game_object = smart_cast<CGameObject*>(this);
        if (game_object && game_object->Visual())
            game_object->remove_visual_callback(AttachmentCallback);
        m_attached_objects.clear();
    }
    R_ASSERT(attached_objects().empty());
}

void CAttachmentOwner::renderable_Render(u32 context_id, IRenderable* root)
{
    xr_vector<CAttachableItem*>::iterator I = m_attached_objects.begin();
    xr_vector<CAttachableItem*>::iterator E = m_attached_objects.end();
    for (; I != E; ++I)
        (*I)->renderable_Render(context_id, root);
}

static void AttachmentCallback(IKinematics* tpKinematics)
{
    CGameObject* game_object =
        smart_cast<CGameObject*>(static_cast<IGameObject*>(tpKinematics->GetUpdateCallbackParam()));
    VERIFY(game_object);

    CAttachmentOwner* attachment_owner = smart_cast<CAttachmentOwner*>(game_object);
    VERIFY(attachment_owner);

    IKinematics* kinematics = smart_cast<IKinematics*>(game_object->Visual());

    xr_vector<CAttachableItem*>::const_iterator I = attachment_owner->attached_objects().begin();
    xr_vector<CAttachableItem*>::const_iterator E = attachment_owner->attached_objects().end();
    for (; I != E; ++I)
    {
        (*I)->item().object().XFORM().mul_43(
            kinematics->LL_GetBoneInstance((*I)->bone_id()).mTransform, (*I)->offset());
        (*I)->item().object().XFORM().mulA_43(game_object->XFORM());
    }
}

void CAttachmentOwner::attach(CInventoryItem* inventory_item)
{
    if (queue_attachment_change(inventory_item, true, false))
        return;
    GW_OVERLAP_HAZARD("CAttachmentOwner::attach");
    xr_vector<CAttachableItem*>::const_iterator I = m_attached_objects.begin();
    xr_vector<CAttachableItem*>::const_iterator E = m_attached_objects.end();
    for (; I != E; ++I)
    {
        if ((*I)->item().object().ID() == inventory_item->object().ID())
            return; // already attached, fake, I'll repair It
        //		VERIFY								((*I)->ID() != inventory_item->object().ID());
    }

    if (can_attach(inventory_item))
    {
        CAttachableItem* attachable_item = smart_cast<CAttachableItem*>(inventory_item);
        VERIFY(attachable_item);
        CGameObject* game_object = smart_cast<CGameObject*>(this);
        VERIFY(game_object && game_object->Visual());
        {
            // AttachmentCallback walks the list in phase B of the owner's bones (a pipelined prepass task may run it
            // beside main's UpdateCL): change it under the owner's bone lock (waits for a running phase B).
            const KinematicsBonesLock bones_lock(PKinematics(game_object->Visual()));
            if (m_attached_objects.empty())
                game_object->add_visual_callback(AttachmentCallback);
            attachable_item->set_bone_id(
                smart_cast<IKinematics*>(game_object->Visual())->LL_BoneID(attachable_item->bone_name()));
            m_attached_objects.push_back(smart_cast<CAttachableItem*>(inventory_item));
        }

        inventory_item->object().setVisible(true);
        attachable_item->afterAttach();
    }
}

void CAttachmentOwner::detach(CInventoryItem* inventory_item)
{
    if (queue_attachment_change(inventory_item, false, false))
        return;
    GW_OVERLAP_HAZARD("CAttachmentOwner::detach");
    xr_vector<CAttachableItem*>::iterator I = m_attached_objects.begin();
    xr_vector<CAttachableItem*>::iterator E = m_attached_objects.end();
    for (; I != E; ++I)
    {
        if ((*I)->item().object().ID() == inventory_item->object().ID())
        {
            CAttachableItem* ai = *I;
            CGameObject* game_object = smart_cast<CGameObject*>(this);
            bool last = false;
            {
                // See attach: the list and the visual callback change under the owner's bone lock; the item's own
                // reactions below run without it (they may compute other models' bones).
                const KinematicsBonesLock bones_lock(game_object ? PKinematics(game_object->Visual()) : nullptr);
                m_attached_objects.erase(I);
                last = m_attached_objects.empty();
                if (last)
                {
                    VERIFY(game_object && game_object->Visual());
                    game_object->remove_visual_callback(AttachmentCallback);
                }
            }
            ai->afterDetach();
            if (last)
                inventory_item->object().setVisible(false);
            break;
        }
    }
}

bool CAttachmentOwner::attached(const CInventoryItem* inventory_item) const
{
    return (attachedItem(inventory_item->object().ID()) != NULL);
}

bool CAttachmentOwner::attached(shared_str sect_name) const { return (attachedItem(sect_name) != NULL); }
bool CAttachmentOwner::can_attach(const CInventoryItem* inventory_item) const
{
    const CAttachableItem* item = smart_cast<const CAttachableItem*>(inventory_item);
    if (!item || !item->enabled() || !item->can_be_attached())
        return (false);

    //можно ли присоединять объекты такого типа
    if (m_attach_item_sections.end() ==
        std::find(m_attach_item_sections.begin(), m_attach_item_sections.end(), inventory_item->object().cNameSect()))
        return false;

    //если уже есть присоединненый объет такого типа
    if (attached(inventory_item->object().cNameSect()))
        return false;

    return true;
}

void CAttachmentOwner::reattach_items()
{
    CGameObject* game_object = smart_cast<CGameObject*>(this);
    VERIFY(game_object && game_object->Visual());

    xr_vector<CAttachableItem*>::const_iterator I = m_attached_objects.begin();
    xr_vector<CAttachableItem*>::const_iterator E = m_attached_objects.end();
    for (; I != E; ++I)
    {
        CAttachableItem* attachable_item = *I;
        VERIFY(attachable_item);
        attachable_item->set_bone_id(
            smart_cast<IKinematics*>(game_object->Visual())->LL_BoneID(attachable_item->bone_name()));
    }
}

CAttachableItem* CAttachmentOwner::attachedItem(CLASS_ID clsid) const
{
    xr_vector<CAttachableItem*>::const_iterator I = m_attached_objects.begin();
    xr_vector<CAttachableItem*>::const_iterator E = m_attached_objects.end();
    for (; I != E; ++I)
        if ((*I)->item().object().CLS_ID == clsid)
            return (*I);

    return NULL;
}

CAttachableItem* CAttachmentOwner::attachedItem(u16 id) const
{
    xr_vector<CAttachableItem*>::const_iterator I = m_attached_objects.begin();
    xr_vector<CAttachableItem*>::const_iterator E = m_attached_objects.end();
    for (; I != E; ++I)
        if ((*I)->item().object().ID() == id)
            return (*I);

    return NULL;
}

CAttachableItem* CAttachmentOwner::attachedItem(shared_str& section) const
{
    xr_vector<CAttachableItem*>::const_iterator I = m_attached_objects.begin();
    xr_vector<CAttachableItem*>::const_iterator E = m_attached_objects.end();
    for (; I != E; ++I)
        if (!xr_strcmp((*I)->item().object().cNameSect(), section) && !(*I)->item().IsInvalid())
            return (*I);

    return NULL;
}
