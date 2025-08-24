#pragma once
#include "inventory_item_object.h"
#include "anticheat_dumpable_object.h"

#pragma pack(push, 1)
struct SCartridgeParam
{
    float kDist     = 1.0f;
    float kDisp     = 1.0f;
    float kHit      = 1.0f;
    float kImpulse  = 1.0f;
    float kPierce   = 1.0f;
    float kAP{};
    float kAirRes{};
    int   buckShot  = 1;
    float impair    = 1.0f;
    float fWallmarkSize{};
    u8    u8ColorID{};
};
#pragma pack(pop)

class CCartridge : public IAnticheatDumpable
{
public:
    void Load(LPCSTR section, u8 LocalAmmoType);

    [[nodiscard]] float Weight() const;

    shared_str m_ammoSect;
    enum : u8
    {
        cfTracer         = 1 << 0,
        cfRicochet       = 1 << 1,
        cfCanBeUnlimited = 1 << 2,
        cfExplosive      = 1 << 3,
        cfMagneticBeam   = 1 << 4,
        cf4to1Tracer     = 1 << 5,
    };
    SCartridgeParam param_s;

    u8 m_LocalAmmoType;

    u16 bullet_material_idx{ u16(-1) };
    Flags8 m_flags{ cfTracer | cfRicochet };

    void DumpActiveParams(shared_str const& section_name, CInifile& dst_ini) const override;
    [[nodiscard]] shared_str const GetAnticheatSectionName() const override { return m_ammoSect; }
};

class CWeaponAmmo : public CInventoryItemObject
{
protected:
    using inherited = CInventoryItemObject;

public:
    void Load(LPCSTR section) override;
    bool net_Spawn(CSE_Abstract* DC) override;
    void net_Destroy() override;
    void net_Export(NET_Packet& P) override;
    void net_Import(NET_Packet& P) override;
    void OnH_B_Chield() override;
    void OnH_B_Independent(bool just_before_destroy) override;
    void UpdateCL() override;

    void renderable_Render(u32 context_id, IRenderable* root) override;

    CWeaponAmmo* cast_weapon_ammo() override { return this; }

    [[nodiscard]] bool  Useful() const override;
    [[nodiscard]] float Weight() const override;
    [[nodiscard]] u32   Cost()   const override;

    bool Get(CCartridge& cartridge);

    SCartridgeParam cartridge_param;

    bool m_tracer;
    bool m_4to1_tracer{};
    u16 m_boxSize;
    u16 m_boxCurr;

public:
    CInventoryItem* can_make_killing(const CInventory* inventory) const override;
};
