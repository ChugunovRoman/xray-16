#include "pch_script.h"

#include "Weapon.h"
#include "WeaponMagazined.h"
#include "WeaponMagazinedWGrenade.h"
#include "WeaponBinoculars.h"
#include "weaponBM16.h"
#include "F1.h"
#include "WeaponFN2000.h"
#include "WeaponFORT.h"
#include "WeaponAmmo.h"
#include "WeaponHPSA.h"
#include "WeaponKnife.h"
#include "WeaponLR300.h"
#include "WeaponPM.h"
#include "RGD5.h"
#include "WeaponRPG7.h"
#include "WeaponSVD.h"
#include "WeaponSVU.h"
#include "WeaponAK74.h"
#include "WeaponAutomaticShotgun.h"
#include "WeaponGroza.h"
#include "WeaponRG6.h"
#include "WeaponSSRS.h"
#include "WeaponShotgun.h"
#include "WeaponUSP45.h"
#include "WeaponVal.h"
#include "WeaponVintorez.h"
#include "WeaponWalther.h"
#include "Missile.h"
#include "Bolt.h"
#include "medkit.h"
#include "antirad.h"
#include "FoodItem.h"
#include "BottleItem.h"
#include "ExplosiveItem.h"
#include "InventoryBox.h"

void CWeapon::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CWeapon, CGameObject>("CWeapon")
            .def(constructor<>())
            .def("can_kill", (bool (CWeapon::*)() const)&CWeapon::can_kill)
    ];
}

void CWeaponMagazined::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CWeaponMagazined, CWeapon>("CWeaponMagazined")
            .def(constructor<>())
    ];
}

void CWeaponMagazinedWGrenade::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CWeaponMagazinedWGrenade, CWeaponMagazined>("CWeaponMagazinedWGrenade")
            .def(constructor<>())
    ];
}

void CWeaponBinoculars::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CWeaponBinoculars, CWeaponMagazined>("CWeaponBinoculars")
            .def(constructor<>())
    ];
}

void CBolt::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
     [
        class_<CBolt, CGameObject>("CBolt")
            .def(constructor<>())
    ];
}

void CWeaponShotgun::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CWeaponShotgun, CWeaponMagazined>("CWeaponShotgun")
            .def(constructor<>())
    ];
}

void CWeaponBM16::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CWeaponBM16, CWeaponShotgun>("CWeaponBM16")
            .def(constructor<>())
    ];
}

void CWeaponFN2000::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CWeaponFN2000, CWeaponMagazined>("CWeaponFN2000")
            .def(constructor<>())
    ];
}

void CWeaponFORT::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CWeaponFORT, CWeaponMagazined>("CWeaponFORT")
            .def(constructor<>())
    ];
}

void CF1::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CF1, bases<CGameObject, CExplosive>>("CF1")
            .def(constructor<>()),
        // new 14.10.08 peacemaker
        class_<CWeaponAmmo, CGameObject>("CWeaponAmmo")
            .def(constructor<>()),
        class_<CMedkit, CGameObject>("CMedkit")
            .def(constructor<>()),
        class_<CAntirad, CGameObject>("CAntirad")
            .def(constructor<>()),
        class_<CFoodItem, CGameObject>("CFoodItem")
            .def(constructor<>()),
        class_<CBottleItem, CGameObject>("CBottleItem")
            .def(constructor<>()),
        class_<CInventoryBox, CGameObject>("CInventoryBox")
            .def(constructor<>()),
        class_<CExplosiveItem, bases<CGameObject, CExplosive>>("CExplosiveItem")
            .def(constructor<>())
    ];
}

void CWeaponHPSA::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CWeaponHPSA, CWeaponMagazined>("CWeaponHPSA")
            .def(constructor<>())
    ];
}

void CWeaponKnife::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CWeaponKnife, CWeapon>("CWeaponKnife")
            .def(constructor<>())
    ];
}

void CWeaponLR300::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CWeaponLR300, CWeaponMagazined>("CWeaponLR300")
            .def(constructor<>())
    ];
}

void CWeaponPM::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CWeaponPM, CWeaponMagazined>("CWeaponPM")
            .def(constructor<>())
    ];
}

void CRGD5::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CRGD5, bases<CGameObject, CExplosive>>("CRGD5")
            .def(constructor<>())
    ];
}

void CWeaponRPG7::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CWeaponRPG7, CWeaponMagazined>("CWeaponRPG7")
            .def(constructor<>())
    ];
}

void CWeaponSVD::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CWeaponSVD, CWeaponMagazined>("CWeaponSVD")
            .def(constructor<>())
    ];
}

void CWeaponSVU::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CWeaponSVU, CWeaponMagazined>("CWeaponSVU")
            .def(constructor<>())
    ];
}

void CWeaponAK74::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CWeaponAK74, CWeaponMagazinedWGrenade>("CWeaponAK74")
            .def(constructor<>())
    ];
}

void CWeaponAutomaticShotgun::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CWeaponAutomaticShotgun, CWeaponMagazined>("CWeaponAutomaticShotgun")
            .def(constructor<>())
    ];
}

void CWeaponGroza::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CWeaponGroza, CWeaponMagazinedWGrenade>("CWeaponGroza")
            .def(constructor<>())
    ];
}

void CWeaponRG6::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CWeaponRG6, CWeaponShotgun>("CWeaponRG6")
            .def(constructor<>())
    ];
}

void CWeaponSSRS::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CWeaponSSRS, CWeaponMagazined>("CWeaponSSRS")
            .def(constructor<>())
    ];
}

void CWeaponUSP45::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CWeaponUSP45, CWeaponMagazined>("CWeaponUSP45")
            .def(constructor<>())
    ];
}

void CWeaponVal::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CWeaponVal, CWeaponMagazined>("CWeaponVal")
            .def(constructor<>())
    ];
}

void CWeaponVintorez::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CWeaponVintorez, CWeaponMagazined>("CWeaponVintorez")
            .def(constructor<>())
    ];
}

void CWeaponWalther::script_register(lua_State* luaState)
{
    using namespace luabind;

    module(luaState)
    [
        class_<CWeaponWalther, CWeaponMagazined>("CWeaponWalther")
            .def(constructor<>())
    ];
}
