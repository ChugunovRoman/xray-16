#include "StdAfx.h"

// Plugin API, group "objects": read-only queries about online game objects by id.
// Docs: wiki/doc/plugins/api/objects.md

#include "addon_api_common.h"
#include "addon_host.h"

#include "Actor.h"
#include "Entity.h"
#include "GameObject.h"
#include "Level.h"

namespace gw::addons
{
namespace
{
CGameObject* FindObject(GwpObjectId id)
{
    if (!g_pGameLevel || id == GWP_INVALID_OBJECT_ID || !IsMainThread())
        return nullptr;
    return smart_cast<CGameObject*>(Level().Objects.net_Find(id));
}

int GWP_CALL ApiObjectExists(GwpObjectId id) { return FindObject(id) ? 1 : 0; }

const char* GWP_CALL ApiObjectSection(GwpObjectId id)
{
    const CGameObject* object = FindObject(id);
    return object ? object->cNameSect().c_str() : nullptr;
}

const char* GWP_CALL ApiObjectName(GwpObjectId id)
{
    const CGameObject* object = FindObject(id);
    return object ? object->cName().c_str() : nullptr;
}

int GWP_CALL ApiObjectPosition(GwpObjectId id, float out_xyz[3])
{
    const CGameObject* object = FindObject(id);
    if (!object || !out_xyz)
        return 0;
    const Fvector& position = object->Position();
    out_xyz[0] = position.x;
    out_xyz[1] = position.y;
    out_xyz[2] = position.z;
    return 1;
}

// Where the object looks: a unit vector, the same one Lua gets from game_object:direction().
int GWP_CALL ApiObjectDirection(GwpObjectId id, float out_xyz[3])
{
    const CGameObject* object = FindObject(id);
    if (!object || !out_xyz)
        return 0;
    const Fvector& direction = object->Direction();
    out_xyz[0] = direction.x;
    out_xyz[1] = direction.y;
    out_xyz[2] = direction.z;
    return 1;
}

// CLASS_ID as the text the class was registered with ("AI_STL_S"): CLSID2TEXT pads it with spaces to 8
// characters, which are not part of the name. The length of the full text, out may be NULL to ask it.
uint32_t GWP_CALL ApiObjectClassId(GwpObjectId id, char* out, uint32_t cap)
{
    if (out && cap)
        out[0] = '\0';
    const CGameObject* object = FindObject(id);
    return object ? api::CopyClassIdText(object->CLS_ID, out, cap) : 0;
}

int GWP_CALL ApiObjectIsAlive(GwpObjectId id)
{
    const CEntity* entity = smart_cast<const CEntity*>(FindObject(id));
    return entity && entity->g_Alive() ? 1 : 0;
}

GwpObjectId GWP_CALL ApiActorId()
{
    if (!g_pGameLevel || !IsMainThread() || GameID() != eGameIDSingle) // Actor() asserts outside single player
        return GWP_INVALID_OBJECT_ID;
    const CActor* actor = Actor();
    return actor ? static_cast<GwpObjectId>(actor->ID()) : GWP_INVALID_OBJECT_ID;
}
} // namespace

void FillObjectsApi(GwpEngineApi& api)
{
    api.object_exists = &ApiObjectExists;
    api.object_section = &ApiObjectSection;
    api.object_name = &ApiObjectName;
    api.object_position = &ApiObjectPosition;
    api.object_is_alive = &ApiObjectIsAlive;
    api.actor_id = &ApiActorId;
    api.object_direction = &ApiObjectDirection;
    api.object_class_id = &ApiObjectClassId;
}
} // namespace gw::addons
