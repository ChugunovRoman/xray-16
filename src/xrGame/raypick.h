#pragma once

#include "script_game_object.h"
#include "script_game_object_impl.h"
#include "xrCDB/xr_collide_defs.h"

// D2 (plans/addon_base_building/plan.md 10.2): the surface the ray hit, for Lua (rq_result) and the Plugin API
// (level_ray_pick_ex). Static geometry: the normal of the hit triangle (turned towards the start of the ray) and its
// game material. An object with a skeleton: the normal of the hit bone shape at the hit point (a box - its face, a
// cylinder - its cap or side, a sphere - from its centre; CCF_Skeleton::_ElementNormal), from the bone centre when the
// shape gives none, and the material of the bone. Anything else: the normal is -dir and the material unknown (u16(-1)).
// Main thread (reads the static CDB of the level and the bones of the object).
void ray_pick_surface(const collide::rq_result& R, const Fvector& start, const Fvector& dir, Fvector& normal,
    u16& material_idx);

struct script_rq_result
{
    CScriptGameObject* O;
    float range;
    int element;
    // D2: the surface - see ray_pick_surface. material_id: the index of the game material (GMLib), -1 when unknown
    Fvector normal;
    int material_id;

    script_rq_result()
    {
        O = nullptr;
        range = 0;
        element = 0;
        normal.set(0.f, 0.f, 0.f);
        material_id = -1;
    };

    // The old form: the hit without the surface (normal 0, material -1)
    void set(collide::rq_result& R)
    {
        if (R.O)
        {
            IGameObject* go = smart_cast<IGameObject*>(R.O);
            if (go)
                O = go->lua_game_object();
        }
        range = R.range;
        element = R.element;
    };

    // D2: the hit and its surface (start, dir - the ray that hit). The object is reset first: the old form kept the
    // object of an earlier hit when an rq_result was reused for a hit of the static geometry
    void set(collide::rq_result& R, const Fvector& start, const Fvector& dir)
    {
        O = nullptr;
        set(R);
        u16 material = u16(-1);
        ray_pick_surface(R, start, dir, normal, material);
        material_id = material == u16(-1) ? -1 : int(material);
    }

    // D2: the name of the game material ("materials\earth", ...), "" when unknown
    pcstr get_material() const;
};

// class for performing ray pick
struct CRayPick
{
    Fvector start_position;
    Fvector direction;
    float range;
    collide::rq_target flags;
    script_rq_result result;
    IGameObject* ignore;

    CRayPick();
    CRayPick(const Fvector& P, const Fvector& D, const float R, const collide::rq_target F, CScriptGameObject* I);

    void set_position(Fvector& P) { start_position = P; };
    void set_direction(Fvector& D) { direction = D; };
    void set_range(float R) { range = R; };
    void set_flags(collide::rq_target F) { flags = F; };
    void set_ignore_object(CScriptGameObject* I)
    {
        if (I) ignore = smart_cast<IGameObject*>(&(I->object()));
    }

    bool query();

    script_rq_result get_result() { return result; };
    CScriptGameObject* get_object() { return result.O; };
    float get_distance() { return result.range; };
    int get_element() { return result.element; };
};
