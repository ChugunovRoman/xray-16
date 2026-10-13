////////////////////////////////////////////////////////////////////////////
//	Module 		: attachment_owner.h
//	Created 	: 12.02.2004
//  Modified 	: 12.02.2004
//	Author		: Dmitriy Iassenev
//	Description : Attachment owner
////////////////////////////////////////////////////////////////////////////

#pragma once

class CGameObject;
class CAttachableItem;
class CInventoryItem;

class CAttachmentOwner
{
protected:
    xr_vector<shared_str> m_attach_item_sections;
    xr_vector<CAttachableItem*> m_attached_objects;

public:
    virtual CGameObject* cast_game_object() = 0;
    virtual CAttachmentOwner* cast_attachment_owner() { return this; }
public:
    CAttachmentOwner() {}
    virtual ~CAttachmentOwner();
    virtual void reinit();
    virtual void reload(LPCSTR section);
    virtual void net_Destroy();
    virtual void renderable_Render(u32 context_id, IRenderable* root);
    virtual void attach(CInventoryItem* inventory_item);
    virtual void detach(CInventoryItem* inventory_item);
    virtual bool can_attach(const CInventoryItem* inventory_item) const;
    bool attached(const CInventoryItem* inventory_item) const;
    bool attached(shared_str sect_name) const;
    virtual void reattach_items();
    // Game/render overlap (B2.2): on the Game logic thread in an overlap frame (CGameObject::ShouldDeferToPostJoin)
    // the render traverses m_attached_objects (renderable_Render, AttachmentCallback) and reads the visual callback,
    // so attach/detach go to a queue by IDs applied on the main thread at the end of the frame. Returns true when the
    // call is handled that way (queued, or dropped as a no-op the immediate call would be too); false: do it now.
    // set_visible (CAttachableItem::enable): the change plus setVisible of the item, always queued.
    bool queue_attachment_change(const CInventoryItem* inventory_item, bool attach, bool set_visible);
    const xr_vector<CAttachableItem*>& attached_objects() const { return (m_attached_objects); }

    CAttachableItem* attachedItem(CLASS_ID clsid) const;
    CAttachableItem* attachedItem(u16 id) const;
    CAttachableItem* attachedItem(shared_str& section) const;
};
