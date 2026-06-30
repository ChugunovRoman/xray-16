#include "StdAfx.h"
#include "ActorBackpack.h"
#include "Actor.h"
#include "Inventory.h"

CBackpack::CBackpack()
{
    m_flags.set(FUsingCondition, false);
}

void CBackpack::Load(pcstr section)
{
    inherited::Load(section);

    m_additional_weight = pSettings->r_float(section, "additional_inventory_weight");
    m_additional_weight2 = pSettings->r_float(section, "additional_inventory_weight2");
    m_fPowerRestoreSpeed = pSettings->read_if_exists<float>(section, "power_restore_speed", 0.0f);
    m_fPowerLoss = pSettings->read_if_exists<float>(section, "power_loss", 1.0f);
    clamp(m_fPowerLoss, EPS, 1.0f);

    m_fJumpSpeed = pSettings->read_if_exists<float>(section, "jump_speed", 1.f);
    m_fWalkAccel = pSettings->read_if_exists<float>(section, "walk_accel", 1.f);
    m_fOverweightWalkK = pSettings->read_if_exists<float>(section, "overweight_walk_accel", 1.f);

    m_flags.set(FUsingCondition, pSettings->read_if_exists<bool>(section, "use_condition", true));
}

void CBackpack::Hit(float hit_power, ALife::EHitType hit_type)
{
    if (!IsUsingCondition())
        return;
    hit_power *= GetHitImmunity(hit_type);
    ChangeCondition(-hit_power);
}

bool CBackpack::install_upgrade_impl(pcstr section, bool test)
{
    bool result = inherited::install_upgrade_impl(section, test);

    result |= process_if_exists(section, "power_restore_speed", &CInifile::r_float, m_fPowerRestoreSpeed, test);
    result |= process_if_exists(section, "power_loss", &CInifile::r_float, m_fPowerLoss, test);
    clamp(m_fPowerLoss, 0.0f, 1.0f);

    result |= process_if_exists(section, "additional_inventory_weight", &CInifile::r_float, m_additional_weight, test);
    result |= process_if_exists(section, "additional_inventory_weight2", &CInifile::r_float, m_additional_weight2, test);

    return result;
}

void CBackpack::RebuildDescription()
{
    inherited::RebuildDescription();
    const auto section = object().cNameSect();
    if (!pSettings->line_exist(section, "description"))
        return;

    shared_str str_props_title = StringTable().translate("st_outfit_properties");
    shared_str str_outfit_list_symbol = StringTable().translate("st_outfit_list_symbol");
    shared_str str_bp_weight = StringTable().translate("st_outfit_property_inventory_weight");
    shared_str str_bp_weight_suffix = StringTable().translate("st_kg");

    float weight = pSettings->read_if_exists<float>(section, "additional_inventory_weight", 0.0f);

    m_Description = make_string("%s\\n%s\\n", m_Description.c_str(), str_props_title.c_str()).c_str();
    m_Description = make_string("%s%%c[255,238,153,26] %s %%c[0,140,140,140] %s %.2f %s\\n", m_Description.c_str(), str_outfit_list_symbol.c_str(), str_bp_weight.c_str(), weight, str_bp_weight_suffix.c_str()).c_str();
}
