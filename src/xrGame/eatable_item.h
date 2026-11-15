#pragma once

#include "inventory_item.h"

class CPhysicItem;
class CEntityAlive;

class CEatableItem : public CInventoryItem
{
private:
    typedef CInventoryItem inherited;

protected:
    CPhysicItem* m_physic_item;

    u8 m_iMaxUses;
    u8 m_iRemainingUses;
    bool m_bRemoveAfterUse;
    float m_fWeightFull;
    float m_fWeightEmpty;

public:
    CEatableItem();
    virtual ~CEatableItem();
    virtual IFactoryObject* _construct();
    virtual CEatableItem* cast_eatable_item() { return this; }
    virtual void Load(LPCSTR section);
    void load(IReader& packet) override;
    void save(NET_Packet& packet) override;
    virtual bool Useful() const;

    virtual bool net_Spawn(CSE_Abstract* DC);

    virtual void OnH_B_Independent(bool just_before_destroy);
    virtual void OnH_A_Independent();
    virtual bool UseBy(CEntityAlive* npc);
    virtual bool Empty() const { return m_iRemainingUses == 0; }
    bool CanDelete() const { return m_bRemoveAfterUse == true; }
    virtual u8 GetMaxUses() const { return m_iMaxUses; }
    virtual u8 GetRemainingUses() const { return m_iRemainingUses; }
    // Also refreshes the condition (use_condition sections): before, set_remaining_uses from Lua left it stale until
    // the next UseBy / net_Spawn
    void SetRemainingUses(u8 value)
    {
        if (value <= m_iMaxUses)
        {
            m_iRemainingUses = value;
            UpdateConditionFromUses();
        }
    }
    float Weight() const override;

    virtual bool GetBriefInfo(II_BriefInfo& info);

protected:
    // use_condition sections: condition = the share of the uses left (remaining / max, clamped to 0..1)
    void UpdateConditionFromUses();
};
