#include "StdAfx.h"

#include "addon_host.h"

#include "Level.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_time_manager.h"
#include "date_time.h"
#include "game_base_space.h"
#include "xrAICore/Navigation/level_graph.h"
#include "xrEngine/xr_object.h"
#include "raypick.h"
#include "xrMaterialSystem/GameMtlLib.h"
#include "GameObject.h"

#include <algorithm>
#include <cmath>

namespace gw::addons
{
namespace
{
bool CheckLevelCall(pcstr function)
{
    if (IsMainThread())
        return true;
    Msg("! [level] %s called outside the main thread, ignored", function);
    return false;
}

bool HasLevel() { return g_pGameLevel != nullptr; }

// The AI graph belongs to the level: without a level there is nothing to ask.
bool HasGraph() { return HasLevel() && ai().get_level_graph(); }

Fvector ToVector(const float xyz[3]) { return Fvector().set(xyz[0], xyz[1], xyz[2]); }

// ---------------------------------------------------------------------------------------------
// Plugin API (group level)
// ---------------------------------------------------------------------------------------------

int GWP_CALL ApiLevelPresent()
{
    return CheckLevelCall("level_present") && HasLevel() ? 1 : 0;
}

const char* GWP_CALL ApiLevelName()
{
    if (!CheckLevelCall("level_name") || !HasLevel())
        return nullptr;
    return Level().name().c_str();
}

int GWP_CALL ApiLevelTimeParts(uint32_t out_parts[7])
{
    if (out_parts)
    {
        for (int i = 0; i < 7; ++i)
            out_parts[i] = 0;
    }
    if (!CheckLevelCall("level_time_parts") || !out_parts)
        return 0;
    // The same source Lua uses for level.get_time_*: the time of the game while it runs, of ALife otherwise.
    u64 time = 0;
    if (HasLevel() && Level().game)
        time = Level().GetGameTime();
    else if (ai().get_alife() && ai().alife().initialized())
        time = ai().alife().time_manager().game_time();
    else
        return 0;

    u32 year = 0, month = 0, day = 0, hours = 0, minutes = 0, seconds = 0, milliseconds = 0;
    split_time(time, year, month, day, hours, minutes, seconds, milliseconds);
    out_parts[0] = year;
    out_parts[1] = month;
    out_parts[2] = day;
    out_parts[3] = hours;
    out_parts[4] = minutes;
    out_parts[5] = seconds;
    out_parts[6] = milliseconds;
    return 1;
}

uint32_t GWP_CALL ApiLevelObjectCount()
{
    if (!CheckLevelCall("level_object_count") || !HasLevel())
        return 0;
    return Level().Objects.o_count();
}

// Writes up to max ids, returns how many online objects there are in all (out may be NULL with max 0 to count)
uint32_t GWP_CALL ApiLevelObjects(GwpObjectId* out, uint32_t max)
{
    if (!CheckLevelCall("level_objects") || !HasLevel())
        return 0;
    // o_count/o_get_by_iterator walk the list of live objects; the Lua way (a scan of all 65536 ids with a cast
    // on every slot) is not needed here.
    const u32 count = Level().Objects.o_count();
    uint32_t total = 0;
    for (u32 i = 0; i < count; ++i)
    {
        const IGameObject* object = Level().Objects.o_get_by_iterator(i);
        if (!object)
            continue;
        if (out && total < max)
            out[total] = object->ID();
        ++total;
    }
    return total;
}

uint32_t GWP_CALL ApiLevelVertexAtPosition(const float xyz[3])
{
    if (!CheckLevelCall("level_vertex_at_position") || !xyz || !HasGraph())
        return GWP_INVALID_LEVEL_VERTEX;
    return ai().level_graph().vertex_id(ToVector(xyz));
}

int GWP_CALL ApiLevelVertexValid(uint32_t vertex_id)
{
    if (!CheckLevelCall("level_vertex_valid") || !HasGraph())
        return 0;
    return ai().level_graph().valid_vertex_id(vertex_id) ? 1 : 0;
}

int GWP_CALL ApiLevelVertexPosition(uint32_t vertex_id, float out_xyz[3])
{
    if (out_xyz)
        out_xyz[0] = out_xyz[1] = out_xyz[2] = 0.f;
    if (!CheckLevelCall("level_vertex_position") || !out_xyz || !HasGraph())
        return 0;
    if (!ai().level_graph().valid_vertex_id(vertex_id))
        return 0;
    const Fvector position = ai().level_graph().vertex_position(vertex_id);
    out_xyz[0] = position.x;
    out_xyz[1] = position.y;
    out_xyz[2] = position.z;
    return 1;
}

uint32_t GWP_CALL ApiLevelVertexInDirection(uint32_t vertex_id, const float dir[3], float distance)
{
    if (!CheckLevelCall("level_vertex_in_direction") || !dir || !HasGraph())
        return GWP_INVALID_LEVEL_VERTEX;
    if (!ai().level_graph().valid_vertex_id(vertex_id) || !(distance > 0.f))
        return vertex_id;

    Fvector direction = ToVector(dir);
    direction.normalize_safe();
    direction.mul(distance);
    const Fvector start = ai().level_graph().vertex_position(vertex_id);
    const Fvector finish = Fvector(start).add(direction);
    u32 result = u32(-1);
    ai().level_graph().farthest_vertex_in_direction(vertex_id, start, finish, result, nullptr);
    return ai().level_graph().valid_vertex_id(result) ? result : vertex_id;
}

int GWP_CALL ApiLevelRayPick(const float from_xyz[3], const float dir[3], float distance, uint32_t targets,
    GwpObjectId ignore, float* out_distance, GwpObjectId* out_object)
{
    if (out_distance)
        *out_distance = 0.f;
    if (out_object)
        *out_object = GWP_INVALID_OBJECT_ID;
    if (!CheckLevelCall("level_ray_pick") || !from_xyz || !dir || !HasLevel() || !(distance > 0.f))
        return 0;

    IGameObject* ignore_object = ignore != GWP_INVALID_OBJECT_ID ? Level().Objects.net_Find(ignore) : nullptr;
    const collide::rq_target target = static_cast<collide::rq_target>(targets ? targets : GWP_RAY_ANY);
    collide::rq_result result;
    if (!Level().ObjectSpace.RayPick(ToVector(from_xyz), ToVector(dir), distance, target, result, ignore_object))
        return 0;

    if (out_distance)
        *out_distance = result.range;
    if (out_object)
        *out_object = result.O ? result.O->ID() : GWP_INVALID_OBJECT_ID; // no object means static geometry
    return 1;
}
// level_ray_pick with the surface (plans/addon_base_building D2): ray_pick_surface of raypick.cpp, as rq_result of Lua
int GWP_CALL ApiLevelRayPickEx(const float from_xyz[3], const float dir[3], float distance, uint32_t targets,
    GwpObjectId ignore, float* out_distance, GwpObjectId* out_object, float out_normal[3], uint32_t* out_material)
{
    if (out_distance)
        *out_distance = 0.f;
    if (out_object)
        *out_object = GWP_INVALID_OBJECT_ID;
    if (out_normal)
        out_normal[0] = out_normal[1] = out_normal[2] = 0.f;
    if (out_material)
        *out_material = GWP_INVALID_MATERIAL;
    if (!CheckLevelCall("level_ray_pick_ex") || !from_xyz || !dir || !HasLevel() || !(distance > 0.f))
        return 0;

    IGameObject* ignore_object = ignore != GWP_INVALID_OBJECT_ID ? Level().Objects.net_Find(ignore) : nullptr;
    const collide::rq_target target = static_cast<collide::rq_target>(targets ? targets : GWP_RAY_ANY);
    const Fvector start = ToVector(from_xyz);
    const Fvector direction = ToVector(dir);
    collide::rq_result result;
    if (!Level().ObjectSpace.RayPick(start, direction, distance, target, result, ignore_object))
        return 0;

    if (out_distance)
        *out_distance = result.range;
    if (out_object)
        *out_object = result.O ? result.O->ID() : GWP_INVALID_OBJECT_ID; // no object means static geometry
    if (out_normal || out_material)
    {
        Fvector normal;
        u16 material = u16(-1);
        ray_pick_surface(result, start, direction, normal, material);
        if (out_normal)
        {
            out_normal[0] = normal.x;
            out_normal[1] = normal.y;
            out_normal[2] = normal.z;
        }
        if (out_material)
            *out_material = material == u16(-1) ? GWP_INVALID_MATERIAL : uint32_t(material);
    }
    return 1;
}

// The name of a game material by its index (level_ray_pick_ex, rq_result.material_id); the library lives all session
const char* GWP_CALL ApiLevelMaterialName(uint32_t material)
{
    if (!CheckLevelCall("level_material_name") || material >= GMLib.CountMaterial())
        return nullptr;
    return GMLib.GetMaterialByIdx(u16(material))->m_Name.c_str();
}

// time_global() of Lua (Device.dwTimeGlobal).
uint32_t GWP_CALL ApiLevelTimeMs()
{
    return CheckLevelCall("level_time_ms") && HasLevel() ? Device.dwTimeGlobal : 0;
}
} // namespace

void FillLevelApi(GwpEngineApi& api)
{
    api.level_present = &ApiLevelPresent;
    api.level_name = &ApiLevelName;
    api.level_time_parts = &ApiLevelTimeParts;
    api.level_object_count = &ApiLevelObjectCount;
    api.level_objects = &ApiLevelObjects;
    api.level_vertex_at_position = &ApiLevelVertexAtPosition;
    api.level_vertex_valid = &ApiLevelVertexValid;
    api.level_vertex_position = &ApiLevelVertexPosition;
    api.level_vertex_in_direction = &ApiLevelVertexInDirection;
    api.level_ray_pick = &ApiLevelRayPick;
    api.level_time_ms = &ApiLevelTimeMs;
    api.level_ray_pick_ex = &ApiLevelRayPickEx; // D2 (plans/addon_base_building)
    api.level_material_name = &ApiLevelMaterialName;
}

// engine gaps P47: the search behind level.objects_in_radius (level_script.cpp); see addon_host.h
void LevelObjectsInRadius(const Fvector& center, float radius, CLASS_ID class_id, int script_clsid,
    bool with_inventory, xr_vector<u16>& out)
{
    out.clear();
    if (!HasLevel() || !(radius > 0.f))
        return;
    // A walk over the live objects (o_count / o_get_by_iterator, as level_objects): a few thousand distance checks,
    // cheaper than the 65536 ids Lua scans; the spatial tree would miss items in inventories and objects without a
    // collision form
    const float radius_sqr = radius * radius;
    xr_vector<std::pair<float, u16>> found;
    const u32 count = Level().Objects.o_count();
    for (u32 i = 0; i < count; ++i)
    {
        const CGameObject* object = smart_cast<const CGameObject*>(Level().Objects.o_get_by_iterator(i));
        if (!object || object->getDestroy())
            continue;
        if (!with_inventory && object->H_Parent())
            continue;
        if (class_id && object->CLS_ID != class_id)
            continue;
        if (script_clsid >= 0 && object->clsid() != script_clsid)
            continue;
        const float distance_sqr = object->Position().distance_to_sqr(center);
        if (distance_sqr <= radius_sqr)
            found.emplace_back(distance_sqr, object->ID());
    }
    std::sort(found.begin(), found.end()); // nearest first; the same distance - by id, so the order is stable
    out.reserve(found.size());
    for (const auto& [distance_sqr, id] : found)
        out.push_back(id);
}
} // namespace gw::addons
