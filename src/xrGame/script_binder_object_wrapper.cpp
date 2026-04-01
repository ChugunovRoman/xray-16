////////////////////////////////////////////////////////////////////////////
//	Module 		: script_binder_object_wrapper.cpp
//	Created 	: 29.03.2004
//  Modified 	: 29.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script object binder wrapper
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "script_binder_object_wrapper.h"
#include "script_game_object.h"
#include "xrServer_Objects_ALife.h"
#include "xrScriptEngine/script_engine.hpp"
#include "xrEngine/profiler.h"
#include "npc_cpp_profile.h"

namespace
{
#if defined(XR_PLATFORM_WINDOWS)
void call_script_binder_update_seh_guarded(CScriptBinderObjectWrapper* self, u32 time_delta)
{
    __try
    {
        luabind::call_member<void>(self, "update", time_delta);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (strstr(Core.Params, "-dbg") != nullptr)
        {
            pcstr objName = "<null>";
            pcstr objSection = "<null>";
            u16 objId = u16(-1);
            if (self->m_object)
            {
                objName = self->m_object->Name();
                objSection = self->m_object->Section();
                objId = self->m_object->ID();
            }

            Msg("! [script_binder] SEH in update(): object='%s' section='%s' id=%u dt=%u",
                objName, objSection, objId, time_delta);

            if (GEnv.ScriptEngine)
            {
                Msg("! [script_binder] Lua stack dump follows");
                GEnv.ScriptEngine->print_stack();
            }
        }
    }
}
#endif
} // namespace

CScriptBinderObjectWrapper::CScriptBinderObjectWrapper(CScriptGameObject* object) : CScriptBinderObject(object) {}
CScriptBinderObjectWrapper::~CScriptBinderObjectWrapper() {}
void CScriptBinderObjectWrapper::reinit() { luabind::call_member<void>(this, "reinit"); }
void CScriptBinderObjectWrapper::reinit_static(CScriptBinderObject* script_binder_object)
{
    script_binder_object->CScriptBinderObject::reinit();
}

void CScriptBinderObjectWrapper::reload(LPCSTR section) { luabind::call_member<void>(this, "reload", section); }
void CScriptBinderObjectWrapper::reload_static(CScriptBinderObject* script_binder_object, LPCSTR section)
{
    script_binder_object->CScriptBinderObject::reload(section);
}

bool CScriptBinderObjectWrapper::net_Spawn(SpawnType DC) { return (luabind::call_member<bool>(this, "net_spawn", DC)); }
bool CScriptBinderObjectWrapper::net_Spawn_static(CScriptBinderObject* script_binder_object, SpawnType DC)
{
    return (script_binder_object->CScriptBinderObject::net_Spawn(DC));
}

void CScriptBinderObjectWrapper::net_Destroy() { luabind::call_member<void>(this, "net_destroy"); }
void CScriptBinderObjectWrapper::net_Destroy_static(CScriptBinderObject* script_binder_object)
{
    script_binder_object->CScriptBinderObject::net_Destroy();
}

void CScriptBinderObjectWrapper::net_Import(NET_Packet* net_packet)
{
    luabind::call_member<void>(this, "net_import", net_packet);
}

void CScriptBinderObjectWrapper::net_Import_static(CScriptBinderObject* script_binder_object, NET_Packet* net_packet)
{
    script_binder_object->CScriptBinderObject::net_Import(net_packet);
}

void CScriptBinderObjectWrapper::net_Export(NET_Packet* net_packet)
{
    luabind::call_member<void>(this, "net_export", net_packet);
}

void CScriptBinderObjectWrapper::net_Export_static(CScriptBinderObject* script_binder_object, NET_Packet* net_packet)
{
    script_binder_object->CScriptBinderObject::net_Export(net_packet);
}

void CScriptBinderObjectWrapper::shedule_Update(u32 time_delta)
{
    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::ScriptBinderLuabindUpdate);
    START_PROFILE("script_binder/luabind_update")
#if defined(XR_PLATFORM_WINDOWS)
    call_script_binder_update_seh_guarded(this, time_delta);
#else
    luabind::call_member<void>(this, "update", time_delta);
#endif
    STOP_PROFILE
}

void CScriptBinderObjectWrapper::shedule_Update_static(CScriptBinderObject* script_binder_object, u32 time_delta)
{
    script_binder_object->CScriptBinderObject::shedule_Update(time_delta);
}

void CScriptBinderObjectWrapper::save(NET_Packet* output_packet)
{
    luabind::call_member<void>(this, "save", output_packet);
}

void CScriptBinderObjectWrapper::save_static(CScriptBinderObject* script_binder_object, NET_Packet* output_packet)
{
    script_binder_object->CScriptBinderObject::save(output_packet);
}

void CScriptBinderObjectWrapper::load(IReader* input_packet) { luabind::call_member<void>(this, "load", input_packet); }
void CScriptBinderObjectWrapper::load_static(CScriptBinderObject* script_binder_object, IReader* input_packet)
{
    script_binder_object->CScriptBinderObject::load(input_packet);
}

bool CScriptBinderObjectWrapper::net_SaveRelevant() { return (luabind::call_member<bool>(this, "net_save_relevant")); }
bool CScriptBinderObjectWrapper::net_SaveRelevant_static(CScriptBinderObject* script_binder_object)
{
    return (script_binder_object->CScriptBinderObject::net_SaveRelevant());
}

void CScriptBinderObjectWrapper::net_Relcase(CScriptGameObject* object)
{
    luabind::call_member<void>(this, "net_Relcase", object);
}

void CScriptBinderObjectWrapper::net_Relcase_static(
    CScriptBinderObject* script_binder_object, CScriptGameObject* object)
{
    script_binder_object->CScriptBinderObject::net_Relcase(object);
}
