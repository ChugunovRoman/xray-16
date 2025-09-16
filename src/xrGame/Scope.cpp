#include "pch_script.h"
#include "Scope.h"
#include "Silencer.h"
#include "GrenadeLauncher.h"

CScope::CScope() {}
CScope::~CScope() {}

void CScope::Load(LPCSTR section)
{
    inherited::Load(section);

    if (pSettings->line_exist(section, "addon_type"))
        m_addon_type = pSettings->r_string(section, "addon_type");
    if (pSettings->line_exist(section, "slot_type"))
        m_slot_type = (CWeapon::EWeaponAddonSlotType)pSettings->r_u8(section, "slot_type");
    else
        m_slot_type = CWeapon::EWeaponAddonSlotType::eNone;
    if (pSettings->line_exist(section, "provided_slot_type"))
        m_provided_slot_type = (CWeapon::EWeaponAddonSlotType)pSettings->r_u8(section, "provided_slot_type");
    else
        m_provided_slot_type = CWeapon::EWeaponAddonSlotType::eNone;
    if (pSettings->line_exist(section, "has_ort"))
        m_has_ort = pSettings->r_bool(section, "has_ort");
    if (pSettings->line_exist(section, "scope_texture"))
        m_scope_texture = pSettings->r_string(section, "scope_texture");
    if (pSettings->line_exist(section, "scope_dynamic_zoom"))
        m_scope_dynamic_zoom = pSettings->r_bool(section, "scope_dynamic_zoom");
    
    if (pSettings->line_exist(section, "mag_type"))
        m_mag_type = pSettings->r_string(section, "mag_type");
}

void CScope::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CScope, CGameObject>("CScope")
            .def(constructor<>()),

        class_<CSilencer, CGameObject>("CSilencer")
            .def(constructor<>()),

        class_<CGrenadeLauncher, CGameObject>("CGrenadeLauncher")
            .def(constructor<>())
    ];
}
