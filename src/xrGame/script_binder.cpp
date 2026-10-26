////////////////////////////////////////////////////////////////////////////
//	Module 		: script_binder.cpp
//	Created 	: 26.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script objects binder
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "ai_space.h"
#include "xrScriptEngine/script_engine.hpp"
#include "script_binder.h"
#include "xrServer_Objects_ALife.h"
#include "script_binder_object.h"
#include "script_game_object.h"
#include "GameObject.h"
#include "Level.h"
#include "smart_zone.h"
#include "xrEngine/profiler.h"
#include "npc_cpp_profile.h"
#include "lua_spike_probe.h"

namespace
{
// plans/lua_to_cpp/57-gw-eoc-next-steps.md, E2 (diagnostics only): the smart zones by class - the script smart
// terrain and the script space restrictor (class_registrator.script binds both to ce_smart_zone)
constexpr CLASS_ID kScriptBinderClsidSmartTerrain = xray::make_clsid("SMRTTRRN");
constexpr CLASS_ID kScriptBinderClsidRestrictor = xray::make_clsid("SPC_RS_S");

// The type row of a binder update (script_binder/update/<type>) and, for a smart zone, its class row
// (script_binder/update/smartzone/<class>) and its section (binder_section_top): one timer for all of them.
// start == 0: the profiler is off, nothing is added.
struct CScriptBinderTypeProfile
{
    ENpcCppProfileStage type = ENpcCppProfileStage::ScriptBinderUpdateOther;
    ENpcCppProfileStage zone = ENpcCppProfileStage::Count; // Count: not a smart zone
    const CGameObject* owner = nullptr;
    u64 start = 0;

    ~CScriptBinderTypeProfile()
    {
        if (!start)
            return;
        const u64 delta = CPU::QPC() - start;
        npc_cpp_profile::add(type, delta);
        if (zone != ENpcCppProfileStage::Count)
        {
            npc_cpp_profile::add(zone, delta);
            npc_cpp_profile::add_binder_section(owner->cNameSect().c_str(), delta);
        }
    }
};
} // namespace

// comment next string when commiting
//#define DBG_DISABLE_SCRIPTS

CScriptBinder::CScriptBinder(CGameObject* owner)
{
    this->owner = owner;
    init();
}

CScriptBinder::~CScriptBinder() { VERIFY(!m_object); }
void CScriptBinder::init() { m_object = 0; }
void CScriptBinder::clear()
{
    try
    {
        xr_delete(m_object);
    }
    catch (...)
    {
        m_object = 0;
    }
    init();
}

void CScriptBinder::reinit()
{
    if (m_object)
    {
        try
        {
            m_object->reinit();
        }
        catch (...)
        {
            clear();
        }
    }
}

void CScriptBinder::reload(LPCSTR section)
{
#ifndef DBG_DISABLE_SCRIPTS
    VERIFY(!m_object);
    if (!pSettings->line_exist(section, "script_binding"))
        return;

    luabind::functor<void> lua_function;
    if (!GEnv.ScriptEngine->functor(pSettings->r_string(section, "script_binding"), lua_function))
    {
        GEnv.ScriptEngine->script_log(
            LuaMessageType::Error, "function %s is not loaded!", pSettings->r_string(section, "script_binding"));
        return;
    }

    try
    {
        lua_function(owner->lua_game_object());
    }
    catch (...)
    {
        clear();
        return;
    }

    if (m_object)
    {
        try
        {
            m_object->reload(section);
        }
        catch (...)
        {
            clear();
        }
    }
#endif
}

BOOL CScriptBinder::net_Spawn(CSE_Abstract* DC)
{
    CSE_Abstract* abstract = (CSE_Abstract*)DC;
    CSE_ALifeObject* object = smart_cast<CSE_ALifeObject*>(abstract);
    if (object && m_object)
    {
        try
        {
            return ((BOOL)m_object->net_Spawn(object));
        }
        catch (...)
        {
            clear();
        }
    }

    return (TRUE);
}

void CScriptBinder::net_Destroy()
{
    if (m_object)
    {
#ifdef _DEBUG
        Msg("* Core object %s is UNbinded from the script object", owner->cName().c_str());
#endif // _DEBUG
        try
        {
            m_object->net_Destroy();
        }
        catch (...)
        {
            clear();
        }
    }
    xr_delete(m_object);
}

void CScriptBinder::set_object(CScriptBinderObject* object)
{
    if (IsGameTypeSingle())
    {
        VERIFY2(!m_object, "Cannot bind to the object twice!");
#ifdef _DEBUG
        Msg("* Core object %s is binded with the script object", owner->cName().c_str());
#endif // _DEBUG
        m_object = object;
    }
    else
    {
        xr_delete(object);
    }
}

void CScriptBinder::shedule_Update(u32 time_delta)
{
    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::ScriptBinderUpdate);
    CScriptBinderTypeProfile type_profile;
    // The classification below costs a dynamic_cast (smart_cast<CSmartZone*>); skip it entirely when the
    // profiler is off instead of paying that cast on every bound object's shedule_Update every tick.
    if (npc_cpp_profile::enabled())
    {
        if (owner) // without one: "other", as before
        {
            if (owner->cast_stalker())
                type_profile.type = ENpcCppProfileStage::ScriptBinderUpdateStalker;
            else if (owner->cast_base_monster())
                type_profile.type = ENpcCppProfileStage::ScriptBinderUpdateMonster;
            else if (smart_cast<CSmartZone*>(owner))
            {
                type_profile.type = ENpcCppProfileStage::ScriptBinderUpdateSmartZone;
                // 57-gw-eoc-next-steps.md, E2: smart terrains apart from the space restrictors
                if (owner->CLS_ID == kScriptBinderClsidSmartTerrain)
                    type_profile.zone = ENpcCppProfileStage::ScriptBinderUpdateSmartZoneSmartTerrain;
                else if (owner->CLS_ID == kScriptBinderClsidRestrictor)
                    type_profile.zone = ENpcCppProfileStage::ScriptBinderUpdateSmartZoneRestrictor;
                else
                    type_profile.zone = ENpcCppProfileStage::ScriptBinderUpdateSmartZoneOther;
                type_profile.owner = owner;
            }
            else if (owner->cast_inventory_item())
                type_profile.type = ENpcCppProfileStage::ScriptBinderUpdateItem;
        }
        type_profile.start = CPU::QPC(); // where the scope of the type row started before
    }
    if (m_object)
    {
        try
        {
            START_PROFILE("script_binder/update")
            // P18 (plans/optimization_mt/13-lua-spikes.md): an update over npc_perf_lua_spike_us goes to the log
            gw::lua_spike::Probe spike(
                gw::lua_spike::Enabled() && GEnv.ScriptEngine ? GEnv.ScriptEngine->lua() : nullptr,
                gw::lua_spike::ECategory::Binder);
            m_object->shedule_Update(time_delta);
            if (spike.Finish() && owner)
                spike.Report(owner->cNameSect().c_str(), owner->ID(), gw::lua_spike::kNoId);
            STOP_PROFILE
        }
        catch (...)
        {
            clear();
        }
    }
}

void CScriptBinder::save(NET_Packet& output_packet)
{
    if (m_object)
    {
        try
        {
            m_object->save(&output_packet);
        }
        catch (...)
        {
            clear();
        }
    }
}

void CScriptBinder::load(IReader& input_packet)
{
    if (m_object)
    {
        try
        {
            m_object->load(&input_packet);
        }
        catch (...)
        {
            clear();
        }
    }
}

BOOL CScriptBinder::net_SaveRelevant()
{
    if (m_object)
    {
        try
        {
            return (m_object->net_SaveRelevant());
        }
        catch (...)
        {
            clear();
        }
    }
    return (FALSE);
}

void CScriptBinder::net_Relcase(IGameObject* object)
{
    ZoneScopedN("CScriptBinder::net_Relcase (Lua)");
    CGameObject* game_object = smart_cast<CGameObject*>(object);
    if (m_object && game_object)
    {
        try
        {
            m_object->net_Relcase(game_object->lua_game_object());
        }
        catch (...)
        {
            clear();
        }
    }
}
