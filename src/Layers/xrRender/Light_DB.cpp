#include "stdafx.h"
#include "Common/_d3d_extensions.h"
#include "Common/LevelStructure.hpp"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/device.h"
#include "xrEngine/Environment.h"
#include "utils/xrLC_Light/R_light.h"
#include "Light_DB.h"

namespace xray::render::RENDER_NAMESPACE
{
CLight_DB::CLight_DB() : sun(nullptr) {}
CLight_DB::~CLight_DB() {}
void CLight_DB::Load(IReader* fs)
{
    IReader* F = fs->open_chunk(fsL_LIGHT_DYNAMIC);
    {
        // Light itself
        sun = nullptr;

		const size_t size = F->length();
        const size_t element = sizeof(Flight) + 4;
        const size_t count = (size / element);
        VERIFY((count * element) == size);
        v_static.reserve(count);
        for (size_t i = 0; i < count; ++i)
        {
            light* L = Create();
            L->flags.bStatic = true;

#if RENDER == R_R1
            Flight& Ldata = L->ldata;
#else
            Flight Ldata;
#endif
            F->advance(sizeof(u32)); // u32 controller = F->r_u32();
            F->r(&Ldata, sizeof(Flight));

            Ldata.specular.set(Ldata.diffuse);
            Ldata.specular.mul_rgb(0.2f);

            if (Ldata.type == Flight::Type::Directional)
            {
                Fvector tmp_R;
                tmp_R.set(1, 0, 0);

                L->set_type(IRender_Light::DIRECT);
                L->set_shadow(true);
                L->set_rotation(Ldata.direction, tmp_R);

                sun = L;
            }
            else
            {
                Fvector tmp_D, tmp_R;
                tmp_D.set(0, 0, -1); // forward
                tmp_R.set(1, 0, 0); // right

                // point
                L->set_type(IRender_Light::POINT);
                L->set_position(Ldata.position);
                L->set_rotation(tmp_D, tmp_R);
                L->set_range(Ldata.range);
                L->set_color(Ldata.diffuse);
#if RENDER != R_R1
                // Honor spot lights authored in the level editor (previously ignored:
                // everything non-directional was forced to POINT). Direction and outer cone
                // angle come from level data. Read once at level load.
                if (ps_r2_light_editor_spot && Ldata.type == Flight::Type::Spot)
                {
                    Fvector spot_dir = Ldata.direction;
                    if (spot_dir.square_magnitude() < EPS_S)
                        spot_dir.set(0.f, -1.f, 0.f);
                    else
                        spot_dir.normalize();
                    L->set_type(IRender_Light::SPOT);
                    L->set_rotation(spot_dir, tmp_R);
                    L->set_cone(clampr(Ldata.phi, deg2rad(10.f), deg2rad(120.f)));
                }
#endif
#if RENDER == R_R1
                L->set_shadow(false);
#else
                // Engine-side optimization for legacy levels: one shadowed POINT light costs
                // 6 shadow-map slots. Optionally convert the big ones to a down-facing SPOT
                // (1 slot) or strip their shadow. Controlled by cvars, default off = original
                // behavior. Read once at level load.
                const bool in_scope = (ps_r2_static_light_range <= 0.f) || (Ldata.range > ps_r2_static_light_range);
                if (ps_r2_static_light_mode == 1 && in_scope && L->flags.type != IRender_Light::SPOT)
                {
                    Fvector down;
                    down.set(0.f, -1.f, 0.f); // shine down
                    L->set_type(IRender_Light::SPOT);
                    L->set_rotation(down, tmp_R);
                    L->set_cone(deg2rad(ps_r2_static_light_cone));
                    L->set_shadow(true);
                }
                else if (ps_r2_static_light_mode == 2 && in_scope)
                {
                    L->set_shadow(false);
                }
                else
                    L->set_shadow(true);
#endif
                L->set_active(true);

                v_static.push_back(L);
            }
        }
    }
    F->close();

    R_ASSERT2(sun, "Where is sun?");
}

#if RENDER != R_R1
void CLight_DB::LoadHemi()
{
    string_path fn_game;
    if (FS.exist(fn_game, "$level$", "build.lights"))
    {
        IReader* F = FS.r_open(fn_game);
        {
            // Hemispheric light chunk
            IReader* chunk = F->open_chunk(fsL_HEADER);

            if (chunk)
            {
                const size_t size = chunk->length();
                const size_t element = sizeof(R_Light);
                const size_t count = size / element;
                VERIFY(count * element == size);
                v_hemi.reserve(count);
                for (size_t i = 0; i < count; i++)
                {
                    R_Light Ldata;
                    chunk->r(&Ldata, sizeof(R_Light));

                    if (Ldata.type == D3DLIGHT_POINT)
                    {
                        Fvector tmp_D, tmp_R;
                        tmp_D.set(0, 0, -1); // forward
                        tmp_R.set(1, 0, 0); // right

                        light* L = Create();
                        L->flags.bStatic = true;
                        L->set_type(IRender_Light::POINT);
                        L->set_position(Ldata.position);
                        L->set_rotation(tmp_D, tmp_R);
                        L->set_range(Ldata.range);
                        L->set_color(Ldata.diffuse.x, Ldata.diffuse.y, Ldata.diffuse.z);
                        L->set_active(true);
                        L->spatial.type = STYPE_LIGHTSOURCEHEMI;
                        L->set_attenuation_params(
                            Ldata.attenuation0, Ldata.attenuation1, Ldata.attenuation2, Ldata.falloff);

                        v_hemi.push_back(L);
                    }
                }
                chunk->close();
            }
        }
        FS.r_close(F);
    }
}
#endif

void CLight_DB::Unload()
{
    v_static.clear();
    v_hemi.clear();
    sun.destroy();
}

light* CLight_DB::Create()
{
    light* L = xr_new<light>();
    L->flags.bStatic = false;
    L->flags.bActive = false;
    L->flags.bShadow = true;
    return L;
}

void CLight_DB::add_light(light* L)
{
    const u32 sub = Device.m_SecondViewport.IsSecondCalculatePass() ? 1u : 0u;
    const u32 seq = (Device.dwFrame << 1) | sub;
    if (L->add_light_pkg_seq == seq)
        return;
    L->add_light_pkg_seq = seq;
    L->frame_render = Device.dwFrame;
#if RENDER == R_R1
    if (L->flags.bStatic)
    {
        if (!RImplementation.o.ffp || ps_r1_flags.test(R1FLAG_FFP_LIGHTMAPS))
            return; // skip static lighting, 'cause they are in lmaps
    }
    if (ps_r1_flags.test(R1FLAG_DLIGHTS))
        RImplementation.L_Dynamic->add(L);
#else
    if (RImplementation.o.noshadows)
        L->flags.bShadow = FALSE;
    if (L->flags.bStatic && !ps_r2_ls_flags.test(R2FLAG_R1LIGHTS))
        return;
    L->Export(package);
#endif
}

void CLight_DB::Update()
{
    // set sun params
    if (sun)
    {
        light* _sun = (light*)sun._get();
        const auto& E = g_pGamePersistent->Environment().CurrentEnv;
        VERIFY(_valid(E.sun_dir));
#ifdef DEBUG
        if (E.sun_dir.y >= 0)
        {
            //			Log("sect_name", E.sect_name.c_str());
            Log("E.sun_dir", E.sun_dir);
            Log("E.wind_direction", E.wind_direction);
            Log("E.wind_velocity", E.wind_velocity);
            Log("E.sun_color", E.sun_color);
            Log("E.rain_color", E.rain_color);
            Log("E.rain_density", E.rain_density);
            Log("E.fog_distance", E.fog_distance);
            Log("E.fog_density", E.fog_density);
            Log("E.fog_color", E.fog_color);
            Log("E.far_plane", E.far_plane);
            Log("E.sky_rotation", E.sky_rotation);
            Log("E.sky_color", E.sky_color);
        }
#endif

        VERIFY2(E.sun_dir.y < 0, "Invalid sun direction settings in evironment-config");
        Fvector dir, pos;

        if (!RImplementation.is_sun_static() && !ShadowOfChernobylMode)
        {
            // true sunlight direction
            dir.set(E.sun_dir).normalize();
        }
        else
        {
            // for some reason E.sun_dir can point-up
            dir.set(0.0f, -0.75f, 0.0f).add(E.sun_dir);
            u32 counter = 0;
            while (dir.magnitude() < 0.001f && counter < 10)
            {
                dir.add(E.sun_dir);
                ++counter;
            }
            dir.normalize();
        }
        pos.mad(Device.vCameraPosition, dir, -500.f);

        sun->set_rotation(dir, _sun->right);
        sun->set_position(pos);
        sun->set_color(E.sun_color.x * ps_r2_sun_lumscale, E.sun_color.y * ps_r2_sun_lumscale, E.sun_color.z * ps_r2_sun_lumscale);
        sun->set_range(600.f);
    }
    // Clear selection
    package.clear();
}
} // namespace xray::render::RENDER_NAMESPACE
