/*************************************/ //--#SM+#--
/***** Хелпер для настройки худа *****/ // -#Debrovski
/*************************************/ // -#Romann

#include "StdAfx.h"
#include "player_hud.h"
#include "Level.h"
#include "debug_renderer.h"
#include "xrEngine/xr_input.h"
#include "HUDManager.h"
#include "HudItem.h"
#include "xrEngine/Effector.h"
#include "xrEngine/CameraManager.h"
#include "xrEngine/FDemoRecord.h"
#include "xrUICore/ui_base.h"
#include "debug_renderer.h"
#include "xrEngine/GameFont.h"

#include "xrCore/FS_internal.h"

u32 hud_adj_mode = 0;
u32 hud_adj_item_idx = 0;

u8 child_idx = u8(-1); //--#SM+#--
bool bNextChildItem = false; //--#SM+#--
shared_str cur_hud_name = nullptr; //--#SM+#--
// "press SHIFT+NUM 0-return 1-hud_pos 2-hud_rot 3-itm_pos 4-itm_rot 5-fire_point 6-fire_2_point 7-shell_point";

// float _delta_pos = 0.0001f; // MNP
// float _delta_rot = 0.003f;  // MNP

float _delta_pos = READ_IF_EXISTS(pSettingsOpenXRay, r_float, "hud_adj", "_delta_pos", 0.0001f);
float _delta_rot = READ_IF_EXISTS(pSettingsOpenXRay, r_float, "hud_adj", "_delta_rot", 0.003f);

bool is_attachable_item_tuning_mode()
{
    return pInput->iGetAsyncKeyState(SDL_SCANCODE_LSHIFT) || pInput->iGetAsyncKeyState(SDL_SCANCODE_Z) ||
        pInput->iGetAsyncKeyState(SDL_SCANCODE_X) || pInput->iGetAsyncKeyState(SDL_SCANCODE_C);
}

void tune_remap(const Ivector& in_values, Ivector& out_values)
{
    if (pInput->iGetAsyncKeyState(SDL_SCANCODE_LSHIFT))
    {
        out_values = in_values;
        //--#SM+#--
        out_values.x *= -1;
        out_values.y *= -1;
        out_values.z *= 1;
    }
    else if (pInput->iGetAsyncKeyState(SDL_SCANCODE_Z))
    { // strict by X
        out_values.x = -in_values.x; //--#SM+#--
        out_values.y = 0;
        out_values.z = 0;
    }
    else if (pInput->iGetAsyncKeyState(SDL_SCANCODE_X))
    { // strict by Y
        out_values.x = 0;
        out_values.y = -in_values.y; //--#SM+#--
        out_values.z = 0;
    }
    else if (pInput->iGetAsyncKeyState(SDL_SCANCODE_C))
    { // strict by Z
        out_values.x = 0;
        out_values.y = 0;
        out_values.z = -in_values.y; //--#SM+#--
    }
    else
    {
        out_values.set(0, 0, 0);
    }
}

void calc_cam_diff_pos(Fmatrix item_transform, Fvector diff, Fvector& res)
{
    Fmatrix cam_m;
    cam_m.i.set(Device.vCameraRight);
    cam_m.j.set(Device.vCameraTop);
    cam_m.k.set(Device.vCameraDirection);
    cam_m.c.set(Device.vCameraPosition);

    Fvector res1;
    cam_m.transform_dir(res1, diff);

    Fmatrix item_transform_i;
    item_transform_i.invert(item_transform);
    item_transform_i.transform_dir(res, res1);
}

void calc_cam_diff_rot(Fmatrix item_transform, Fvector diff, Fvector& res)
{
    Fmatrix cam_m;
    cam_m.i.set(Device.vCameraRight);
    cam_m.j.set(Device.vCameraTop);
    cam_m.k.set(Device.vCameraDirection);
    cam_m.c.set(Device.vCameraPosition);

    Fmatrix R;
    R.identity();
    if (!fis_zero(diff.x))
    {
        R.rotation(cam_m.i, diff.x);
    }
    else if (!fis_zero(diff.y))
    {
        R.rotation(cam_m.j, diff.y);
    }
    else if (!fis_zero(diff.z))
    {
        R.rotation(cam_m.k, diff.z);
    };

    Fmatrix item_transform_i;
    item_transform_i.invert(item_transform);
    R.mulB_43(item_transform);
    R.mulA_43(item_transform_i);

    R.getHPB(res);

    res.mul(180.0f / PI);
}

//MNP
//////////////////////////////////////////////////////////////////
struct data
{
    std::pair<bool, Fvector> hands_position = { false,{} };
    std::pair<bool, Fvector> hands_orientation = { false,{} };
    std::pair<bool, Fvector> item_position = { false,{} };
    std::pair<bool, Fvector> item_orientation = { false,{} };
};
static xr_map<const char*, data> mapOfConfiguredItems;

void logInfoAboutTunedItems()
{
    Log("#--------------------------------------------------------------------------------------------#");
    for (const auto& pair : mapOfConfiguredItems)
    {
        const char* name = pair.first;
        const data& data = pair.second;

        Msg("/n[%s]", name);

        UI().is_widescreen();

        if (data.hands_position.first)
        {
            Msg("hands_position%s = %f,%f,%f", (UI().is_widescreen()) ? "_16x9" : "",
                data.hands_position.second.x,
                data.hands_position.second.y,
                data.hands_position.second.z);
        }
        if (data.hands_orientation.first)
        {
            Msg("hands_orientation%s = %f,%f,%f", (UI().is_widescreen()) ? "_16x9" : "",
                data.hands_orientation.second.x,
                data.hands_orientation.second.y,
                data.hands_orientation.second.z);
        }
        if (data.item_position.first)
        {
            Msg("item_position = %f,%f,%f",
                data.item_position.second.x,
                data.item_position.second.y,
                data.item_position.second.z);
        }
        if (data.item_orientation.first)
        {
            Msg("item_orientation = %f,%f,%f",
                data.item_orientation.second.x,
                data.item_orientation.second.y,
                data.item_orientation.second.z);
        }
    }
    Log("!--------------------------------------------------------------------------------------------#");
}
//////////////////////////////////////////////////////////////////

void attachable_hud_item::tune(Ivector values)
{
    if (!is_attachable_item_tuning_mode())
        return;

    bool bZeroMode = false; //--#SM+#--

    // Сбросить в нули //--#SM+#--
    if (pInput->iGetAsyncKeyState(SDL_SCANCODE_BACKSPACE))
    {
        bZeroMode = true;
    }

    //Инвертируем мышку --#SM+#--
    Fvector diff;
    diff.set(0, 0, 0);

    if (hud_adj_mode == 3 || hud_adj_mode == 4)
    {
        if (hud_adj_mode == 3)
        {
            if (values.x)
                diff.x = (values.x > 0) ? -_delta_pos : _delta_pos; //--#SM+#--
            if (values.y)
                diff.y = (values.y > 0) ? _delta_pos : -_delta_pos;
            if (values.z)
                diff.z = (values.z > 0) ? _delta_pos : -_delta_pos;
            diff.z *= 10; //--#SM+#--

            Fvector d;
            Fmatrix ancor_m;
            m_parent->calc_transform(m_attach_place_idx, Fidentity, ancor_m);
            calc_cam_diff_pos(ancor_m, diff, d);

            if (bZeroMode) //--#SM+#--
                m_measures.m_item_attach[0].set(0, 0, 0);
            else
                m_measures.m_item_attach[0].add(d);
        }
        else if (hud_adj_mode == 4)
        {
            if (values.x)
                diff.x = (values.x > 0) ? _delta_rot : -_delta_rot;
            if (values.y)
                diff.y = (values.y > 0) ? _delta_rot : -_delta_rot;
            if (values.z)
                diff.z = (values.z > 0) ? _delta_rot : -_delta_rot;
            diff.z *= 10; //--#SM+#--

            Fvector d;
            Fmatrix ancor_m;
            m_parent->calc_transform(m_attach_place_idx, Fidentity, ancor_m);

            calc_cam_diff_pos(m_item_transform, diff, d);

            if (bZeroMode) //--#SM+#--
                m_measures.m_item_attach[1].set(0, 0, 0);
            else
                m_measures.m_item_attach[1].add(d);
        }

        if (pInput->iGetAsyncKeyState(SDL_SCANCODE_END))
        {
            data& curr = mapOfConfiguredItems[m_sect_name.c_str()];
            curr.item_position.first = true;
            curr.item_position.second = m_measures.m_item_attach[0];
            curr.item_orientation.first = true;
            curr.item_orientation.second = m_measures.m_item_attach[1];
            Log("--------------------------------------------");
            FlushLog();
        }
    }

    if (hud_adj_mode == 5 || hud_adj_mode == 6 || hud_adj_mode == 7)
    {
        if (values.x)
            diff.x = (values.x > 0) ? -_delta_pos : _delta_pos; //--#SM+#--
        if (values.y)
            diff.y = (values.y > 0) ? _delta_pos : -_delta_pos;
        if (values.z)
            diff.z = (values.z > 0) ? _delta_pos : -_delta_pos;
        diff.z *= 10; //--#SM+#--

        if (hud_adj_mode == 5)
        {
            if (bZeroMode) //--#SM+#--
                m_measures.m_fire_point_offset.set(0, 0, 0);
            else
                m_measures.m_fire_point_offset.add(diff);
        }
        if (hud_adj_mode == 6)
        {
            if (bZeroMode) //--#SM+#--
                m_measures.m_fire_point2_offset.set(0, 0, 0);
            else
                m_measures.m_fire_point2_offset.add(diff);
        }
        if (hud_adj_mode == 7)
        {
            if (bZeroMode) //--#SM+#--
                m_measures.m_shell_point_offset.set(0, 0, 0);
            else
                m_measures.m_shell_point_offset.add(diff);
        }
        if (pInput->iGetAsyncKeyState(SDL_SCANCODE_END)) // MNP
        {
            Msg("[%s]", m_sect_name.c_str());
            Msg("fire_point = %f,%f,%f", m_measures.m_fire_point_offset.x, m_measures.m_fire_point_offset.y, m_measures.m_fire_point_offset.z);
            Msg("fire_point2 = %f,%f,%f", m_measures.m_fire_point2_offset.x, m_measures.m_fire_point2_offset.y, m_measures.m_fire_point2_offset.z);
            Msg("shell_point = %f,%f,%f", m_measures.m_shell_point_offset.x, m_measures.m_shell_point_offset.y, m_measures.m_shell_point_offset.z);
            Log("--------------------------------------------");
            FlushLog();
        }
    }
}

// IRenderVisual* pDbgModelSphere = nullptr; //--#SM+#--

void attachable_hud_item::debug_draw_firedeps() //--#SM+#--
{
/*
    if (pDbgModelSphere == NULL)
        pDbgModelSphere = GEnv.Render->model_Create("dbg\\dbg_sphere.ogf");
*/
    bool bForce = (hud_adj_mode == 3 || hud_adj_mode == 4);
    if (hud_adj_mode == 5 || hud_adj_mode == 6 || hud_adj_mode == 7 || bForce)
    {
        CDebugRenderer& render = Level().debug_renderer();

        firedeps fd;
        setup_firedeps(fd);

        Fmatrix m;
        m.identity();

        if (hud_adj_mode == 5 || bForce)
            render.draw_aabb(fd.vLastFP, 0.005f, 0.005f, 0.005f, color_xrgb(255, 0, 0));
            // m.translate(fd.vLastFP);

        if (hud_adj_mode == 6)
            render.draw_aabb(fd.vLastFP2, 0.005f, 0.005f, 0.005f, color_xrgb(0, 0, 255));
            // m.translate(fd.vLastFP2);

        if (hud_adj_mode == 7)
            render.draw_aabb(fd.vLastSP, 0.005f, 0.005f, 0.005f, color_xrgb(0, 255, 0));
            // m.translate(fd.vLastSP);

        // GEnv.Render->add_Visual(nullptr, pDbgModelSphere, m);
    }
}

void player_hud::tune(Ivector _values) //--#SM+#--
{
    //-------------------//
    if (bNextChildItem)
    {
        child_idx++;
        bNextChildItem = false;
    }

    attachable_hud_item* hi = m_attached_items[hud_adj_item_idx];
    if (!hi)
        return;

    attachable_hud_item* parent_hi = hi;
/*
    if (child_idx < hi->m_child_items.size())
    {
        hi = hi->m_child_items[child_idx];
    }
    else
    {
        child_idx = u8(-1);
    }
*/
    cur_hud_name = hi->m_sect_name;
    //-------------------//

    Ivector values;
    tune_remap(_values, values);

    bool is_16x9 = UI().is_widescreen();
    bool bZeroMode = false;

    // Сбросить в нули //--#SM+#--
    if (pInput->iGetAsyncKeyState(SDL_SCANCODE_BACKSPACE))
    {
        bZeroMode = true;
    }

    if (hud_adj_mode == 1 || hud_adj_mode == 2)
    {
        Fvector diff;
        diff.set(0, 0, 0);

        float _curr_dr = _delta_rot;
        float _curr_dp = _delta_pos;

        attachable_hud_item* hi = m_attached_items[hud_adj_item_idx];
        if (!hi)
            return;

        u8 idx = hi->m_parent_hud_item->GetCurrentHudOffsetIdx();
        if (idx)
        {
            if (!pInput->iGetAsyncKeyState(SDL_SCANCODE_SPACE))
            {
                _curr_dr /= 200.0f;
                _curr_dp /= 20.0f;
            }
            else
            {
                _curr_dr /= 80.0f;
                _curr_dp /= 5.0f;
            }
        }
        else
        {
            if (pInput->iGetAsyncKeyState(SDL_SCANCODE_SPACE))
            {
                _curr_dr *= 40.0f;
                _curr_dp *= 5.0f;
            }
        }

        Fvector& pos_ = (idx != 0) ? hi->hands_offset_pos() : hi->hands_attach_pos();
        Fvector& rot_ = (idx != 0) ? hi->hands_offset_rot() : hi->hands_attach_rot();

        if (hud_adj_mode == 1)
        {
            if (values.x)
                diff.x = (values.x < 0) ? _curr_dp : -_curr_dp;
            if (values.y)
                diff.y = (values.y > 0) ? _curr_dp : -_curr_dp;
            if (values.z)
                diff.z = (values.z > 0) ? _curr_dp : -_curr_dp;
            diff.z *= 10;

            if (bZeroMode)
                pos_.set(0, 0, 0);
            else
                pos_.add(diff);
        }

        if (hud_adj_mode == 2)
        {
            if ((idx) == 0)
            {
                if (values.x)
                    diff.x = (values.x > 0) ? _curr_dr : -_curr_dr;
                if (values.y)
                    diff.y = (values.y > 0) ? _curr_dr : -_curr_dr;
                if (values.z)
                    diff.z = (values.z > 0) ? _curr_dr : -_curr_dr;
            }
            else
            {
                if (values.x)
                    diff.y = (values.x > 0) ? -_curr_dr : _curr_dr;
                if (values.y)
                    diff.x = (values.y > 0) ? -_curr_dr : _curr_dr;
                if (values.z)
                    diff.z = (values.z > 0) ? _curr_dr : -_curr_dr;
            }
            diff.z *= 10;

            if (bZeroMode)
                rot_.set(0, 0, 0);
            else
                rot_.add(diff);
        }
        if (pInput->iGetAsyncKeyState(SDL_SCANCODE_END)) // MNP
        {
            if ((idx) == 0)
            {
                data& curr = mapOfConfiguredItems[m_attached_items[hud_adj_item_idx]->m_sect_name.c_str()];
                curr.hands_position.first = true;
                curr.hands_position.second = pos_;
                curr.hands_orientation.first = true;
                curr.hands_orientation.second = rot_;
            }
            else if ((idx) == 1)
            {
                Msg("[%s]", hi->m_sect_name.c_str());
                Msg("aim_hud_offset_pos%s = %f,%f,%f", (is_16x9) ? "_16x9" : "", pos_.x, pos_.y, pos_.z);
                Msg("aim_hud_offset_rot%s = %f,%f,%f", (is_16x9) ? "_16x9" : "", rot_.x, rot_.y, rot_.z);
                Log("--------------------------------------------");
                FlushLog();
            }
            else if ((idx) == 2)
            {
                Msg("[%s]", hi->m_sect_name.c_str());
                Msg("gl_hud_offset_pos%s = %f,%f,%f", (is_16x9) ? "_16x9" : "", pos_.x, pos_.y, pos_.z);
                Msg("gl_hud_offset_rot%s = %f,%f,%f", (is_16x9) ? "_16x9" : "", rot_.x, rot_.y, rot_.z);
                Log("--------------------------------------------");
                FlushLog();
            }
        }
    }
    else if (hud_adj_mode == 8 || hud_adj_mode == 9)
    {
        if (hud_adj_mode == 8 && (values.z))
            _delta_pos += (values.z > 0) ? _delta_pos * 0.1f : _delta_pos * -0.1f;

        if (hud_adj_mode == 9 && (values.z))
            _delta_rot += (values.z > 0) ? _delta_rot * .1f : _delta_rot * -0.1f;
    }
    else
    {
        if (hud_adj_mode == 8 || hud_adj_mode == 9)
        {
            if (hud_adj_mode == 8 && (values.z))
            {
                _delta_pos += (values.z > 0) ? 0.00001f : -0.00001f;
                if (_delta_pos < 0)
                    _delta_pos = 0;
            }

            if (hud_adj_mode == 9 && (values.z))
            {
                _delta_rot += (values.z > 0) ? 0.0001f : -0.0001f;
                if (_delta_rot < 0)
                    _delta_rot = 0;
            }
        }
        else
        {
            hi->tune(values);
        }
    }

    // Сохранение в файл
    if (pInput->iGetAsyncKeyState(SDL_SCANCODE_LSHIFT) && pInput->iGetAsyncKeyState(SDL_SCANCODE_RETURN))
    {
        LPCSTR sect_name = hi->m_sect_name.c_str();
        string_path fname;
        FS.update_path(fname, "$game_data$", make_string("_hud_adjustment\\%s.ltx", sect_name).c_str());

        CInifile* pHudCfg = xr_new<CInifile>(fname, FALSE, FALSE, TRUE);
        //-----------------//
        pHudCfg->w_string(sect_name, make_string("gl_hud_offset_pos%s", (is_16x9) ? "_16x9" : "").c_str(),
            make_string("%f,%f,%f", hi->m_measures.m_hands_offset[0][2].x, hi->m_measures.m_hands_offset[0][2].y,
                hi->m_measures.m_hands_offset[0][2].z)
                .c_str());
        pHudCfg->w_string(sect_name, make_string("gl_hud_offset_rot%s", (is_16x9) ? "_16x9" : "").c_str(),
            make_string("%f,%f,%f", hi->m_measures.m_hands_offset[1][2].x, hi->m_measures.m_hands_offset[1][2].y,
                hi->m_measures.m_hands_offset[1][2].z)
                .c_str());

        pHudCfg->w_string(sect_name, make_string("aim_hud_offset_pos%s", (is_16x9) ? "_16x9" : "").c_str(),
            make_string("%f,%f,%f", hi->m_measures.m_hands_offset[0][1].x, hi->m_measures.m_hands_offset[0][1].y,
                hi->m_measures.m_hands_offset[0][1].z)
                .c_str());
        pHudCfg->w_string(sect_name, make_string("aim_hud_offset_rot%s", (is_16x9) ? "_16x9" : "").c_str(),
            make_string("%f,%f,%f", hi->m_measures.m_hands_offset[1][1].x, hi->m_measures.m_hands_offset[1][1].y,
                hi->m_measures.m_hands_offset[1][1].z)
                .c_str());

        pHudCfg->w_string(sect_name, //--#SM+#--
            make_string("aim_hud_offset_alt_pos%s", (is_16x9) ? "_16x9" : "").c_str(),
            make_string("%f,%f,%f", hi->m_measures.m_hands_offset[0][3].x, hi->m_measures.m_hands_offset[0][3].y,
                hi->m_measures.m_hands_offset[0][3].z)
                .c_str());
        pHudCfg->w_string(sect_name, //--#SM+#--
            make_string("aim_hud_offset_alt_rot%s", (is_16x9) ? "_16x9" : "").c_str(),
            make_string("%f,%f,%f", hi->m_measures.m_hands_offset[1][3].x, hi->m_measures.m_hands_offset[1][3].y,
                hi->m_measures.m_hands_offset[1][3].z)
                .c_str());

        pHudCfg->w_string(sect_name, make_string("hands_position%s", (is_16x9) ? "_16x9" : "").c_str(),
            make_string("%f,%f,%f", hi->m_measures.m_hands_attach[0].x, hi->m_measures.m_hands_attach[0].y,
                hi->m_measures.m_hands_attach[0].z)
                .c_str());
        pHudCfg->w_string(sect_name, make_string("hands_orientation%s", (is_16x9) ? "_16x9" : "").c_str(),
            make_string("%f,%f,%f", hi->m_measures.m_hands_attach[1].x, hi->m_measures.m_hands_attach[1].y,
                hi->m_measures.m_hands_attach[1].z)
                .c_str());

        pHudCfg->w_string(sect_name, "item_position",
            make_string("%f,%f,%f", hi->m_measures.m_item_attach[0].x, hi->m_measures.m_item_attach[0].y,
                hi->m_measures.m_item_attach[0].z)
                .c_str());
        pHudCfg->w_string(sect_name, "item_orientation",
            make_string("%f,%f,%f", hi->m_measures.m_item_attach[1].x, hi->m_measures.m_item_attach[1].y,
                hi->m_measures.m_item_attach[1].z)
                .c_str());

        pHudCfg->w_string(sect_name, "fire_point",
            make_string("%f,%f,%f", hi->m_measures.m_fire_point_offset.x, hi->m_measures.m_fire_point_offset.y,
                hi->m_measures.m_fire_point_offset.z)
                .c_str());
        pHudCfg->w_string(sect_name, "fire_point2",
            make_string("%f,%f,%f", hi->m_measures.m_fire_point2_offset.x, hi->m_measures.m_fire_point2_offset.y,
                hi->m_measures.m_fire_point2_offset.z)
                .c_str());
        pHudCfg->w_string(sect_name, "shell_point",
            make_string("%f,%f,%f", hi->m_measures.m_shell_point_offset.x, hi->m_measures.m_shell_point_offset.y,
                hi->m_measures.m_shell_point_offset.z)
                .c_str());

        //-----------------//
        xr_delete(pHudCfg);
        Msg("#---- HUD data saved to %s", fname);
        Sleep(250);
    }

    // Загружаем из файла
    if (pInput->iGetAsyncKeyState(SDL_SCANCODE_LSHIFT) && pInput->iGetAsyncKeyState(SDL_SCANCODE_DELETE))
    {
        LPCSTR sect_name = hi->m_sect_name.c_str();
        string_path fname;
        FS.update_path(fname, "$game_data$", make_string("_hud_adjustment\\%s.ltx", sect_name).c_str());

        CInifile* pHudCfg = xr_new<CInifile>(fname, TRUE, TRUE, FALSE);
        if (!pHudCfg)
            return;
        //-----------------//
        hi->m_measures.m_hands_attach[0] = READ_IF_EXISTS(pHudCfg, r_fvector3, sect_name,
            make_string("hands_position%s", (is_16x9) ? "_16x9" : "").c_str(), hi->m_measures.m_hands_attach[0]);
        hi->m_measures.m_hands_attach[1] = READ_IF_EXISTS(pHudCfg, r_fvector3, sect_name,
            make_string("hands_orientation%s", (is_16x9) ? "_16x9" : "").c_str(), hi->m_measures.m_hands_attach[1]);

        hi->m_measures.m_hands_offset[0][1] = READ_IF_EXISTS(pHudCfg, r_fvector3, sect_name,
            make_string("aim_hud_offset_pos%s", (is_16x9) ? "_16x9" : "").c_str(), hi->m_measures.m_hands_offset[0][1]);
        hi->m_measures.m_hands_offset[1][1] = READ_IF_EXISTS(pHudCfg, r_fvector3, sect_name,
            make_string("aim_hud_offset_rot%s", (is_16x9) ? "_16x9" : "").c_str(), hi->m_measures.m_hands_offset[1][1]);

        hi->m_measures.m_hands_offset[0][3] = READ_IF_EXISTS(pHudCfg, //--#SM+#--
            r_fvector3, sect_name, make_string("aim_hud_offset_alt_pos%s", (is_16x9) ? "_16x9" : "").c_str(),
            hi->m_measures.m_hands_offset[0][3]);
        hi->m_measures.m_hands_offset[1][3] = READ_IF_EXISTS(pHudCfg, //--#SM+#--
            r_fvector3, sect_name, make_string("aim_hud_offset_alt_rot%s", (is_16x9) ? "_16x9" : "").c_str(),
            hi->m_measures.m_hands_offset[1][3]);

        hi->m_measures.m_hands_offset[0][2] = READ_IF_EXISTS(pHudCfg, r_fvector3, sect_name,
            make_string("gl_hud_offset_pos%s", (is_16x9) ? "_16x9" : "").c_str(), hi->m_measures.m_hands_offset[0][2]);
        hi->m_measures.m_hands_offset[1][2] = READ_IF_EXISTS(pHudCfg, r_fvector3, sect_name,
            make_string("gl_hud_offset_rot%s", (is_16x9) ? "_16x9" : "").c_str(), hi->m_measures.m_hands_offset[1][2]);

        hi->m_measures.m_item_attach[0] =
            READ_IF_EXISTS(pHudCfg, r_fvector3, sect_name, "item_position", hi->m_measures.m_item_attach[0]);
        hi->m_measures.m_item_attach[1] =
            READ_IF_EXISTS(pHudCfg, r_fvector3, sect_name, "item_orientation", hi->m_measures.m_item_attach[1]);

        hi->m_measures.m_fire_point_offset =
            READ_IF_EXISTS(pHudCfg, r_fvector3, sect_name, "fire_point", hi->m_measures.m_fire_point_offset);
        hi->m_measures.m_fire_point2_offset =
            READ_IF_EXISTS(pHudCfg, r_fvector3, sect_name, "fire_point2", hi->m_measures.m_fire_point2_offset);
        hi->m_measures.m_shell_point_offset =
            READ_IF_EXISTS(pHudCfg, r_fvector3, sect_name, "shell_point", hi->m_measures.m_shell_point_offset);
        //-----------------//
        xr_delete(pHudCfg);
        Msg("~HUD data loaded from %s", fname);
        Sleep(100);
    }
/*
    if (child_idx != u8(-1))
        parent_hi->UpdateHudFromChildren(false);
*/
}

void hud_draw_adjust_mode()
{
    if (!hud_adj_mode)
        return;

    LPCSTR _text = nullptr;

    switch (hud_adj_mode)
    {
    case 1: _text = "adjusting HUD POSITION"; break;
    case 2: _text = "adjusting HUD ROTATION"; break;
    case 3: _text = "adjusting ITEM POSITION"; break;
    case 4: _text = "adjusting ITEM ROTATION"; break;
    case 5: _text = "adjusting FIRE POINT"; break;
    case 6: _text = "adjusting FIRE 2 POINT"; break;
    case 7: _text = "adjusting SHELL POINT"; break;
    case 8: _text = "adjusting pos STEP"; break;
    case 9: _text = "adjusting rot STEP"; break;
    };
    if (_text) //--#SM+#--
    {
        CGameFont* F = UI().Font().pFontDI;
        F->SetAligment(CGameFont::alCenter);
        F->OutSetI(0.f, -0.8f);
        F->SetColor(0xffffff00);
        F->OutNext(_text);

        if (cur_hud_name != nullptr)
        {
            if (child_idx != u8(-1))
                F->OutNext("for item [%s] idx = %d (%s)", cur_hud_name.c_str(), hud_adj_item_idx,
                    (hud_adj_item_idx) ? "Detector" : "Attachment");
            else
                F->OutNext("for item [%s] idx = %d (%s)", cur_hud_name.c_str(), hud_adj_item_idx,
                    (hud_adj_item_idx) ? "Detector" : "Weapon");
        }
        else
            F->OutNext("for item idx = [%d] (%s)", hud_adj_item_idx, (hud_adj_item_idx) ? "Detector" : "Weapon");

        F->OutNext("delta values dP=%f dR=%f", _delta_pos, _delta_rot);

        F->OutNext("[Z]-x axis [X]-y axis [C]-z axis [H]- Help");

        //--#SM+#--
        F->OutNext("[LSHIFT]- x\\y axis + [MWheel]-z axis");
        F->OutNext("");
        F->SetColor(0xff49e20e);

        if (hud_adj_mode == 1 || hud_adj_mode == 2)
        {
            F->OutNext("Push \"Space\" to move faster");
        }
        if (hud_adj_mode == 5 || hud_adj_mode == 6 || hud_adj_mode == 7)
        {
            F->OutNext("Type \"hud_fov 1\" in console <!> (0.45 as default)");
        }
        if (hud_adj_item_idx == 0 && pInput->iGetAsyncKeyState(SDL_SCANCODE_LCTRL))
        {
            F->OutNext("Use LCTRL+NUM 0-1 to switch between attachments");
        }
        if (hud_adj_mode == 8 || hud_adj_mode == 9)
        {
            if (hud_adj_mode == 8)
            {
                F->OutNext("Push \"C\" + move MOUSE — to customize _delta_pos");
            }
            if (hud_adj_mode == 9)
            {
                F->OutNext("Push \"C\" + move MOUSE — to customize _delta_rot");
            }
        }

        if (pInput->iGetAsyncKeyState(SDL_SCANCODE_LALT) && hud_adj_mode)
        {
            F->OutNext("press LALT+press NUM: 0-return|1-hud_pos|2-hud_rot|3-itm_pos|4-itm_rot|5-fire_point|6-fire_point_2|7-shell_point|8-pos_step 9-rot_step");
        }

        if (pInput->iGetAsyncKeyState(SDL_SCANCODE_LSHIFT) && hud_adj_mode)
        {
            F->OutNext("Hold Right button MOUSE+LSHIFT - release LSHIFT - release Right button MOUSE");
        }

        if (pInput->iGetAsyncKeyState(SDL_SCANCODE_H)) //--#SM+#--
        {
            F->OutSetI(-1.f, -0.4f);
            F->SetAligment(CGameFont::alLeft);
            F->SetColor(0xff00ff00);
            F->OutNext("");
            F->OutNext("   [LALT+NUM 0]  - Quit\\Return");
            F->OutNext("   [LALT+NUM 1]  - HUD Position");
            F->OutNext("   [LALT+NUM 2]  - HUD Rotation");
            F->OutNext("   [LALT+NUM 3]  - ITEM Position");
            F->OutNext("   [LALT+NUM 4]  - ITEM Rotation");
            F->OutNext("   [LALT+NUM 5]  - Fire Point");
            F->OutNext("   [LALT+NUM 6]  - Fire Point GL");
            F->OutNext("   [LALT+NUM 7]  - Shell Point");
            F->OutNext("   [LALT+NUM 8]  - STEP Position");
            F->OutNext("   [LALT+NUM 9]  - STEP Rotation");
            F->OutNext("");
            F->OutNext("   [LCTRL+NUM 0] - Weapon");
            F->OutNext("   [LCTRL+NUM 1] - Detector");
            F->OutNext("   [LCTRL+NUM 2] - Switch attached item");
            F->OutNext("");
            F->SetColor(0xffff0000);                                                                   // MNP
            F->OutNext("   [END]              - Output in Log/console");                               // MNP
            F->OutNext("   [LSHIFT+ENTER]     - SAVE to   gamedata\\_hud_adjustment\\<hud_sect>.ltx");
            F->OutNext("   [LSHIFT+DELETE]    - LOAD from gamedata\\_hud_adjustment\\<hud_sect>.ltx");
            F->OutNext("   [LSHIFT+BACKSPACE] - Set values to zero (0,0,0)");
            F->OutNext("");
            F->OutNext("   Use MOUSE and ARROWS to move weapon");
        }
    }
}

extern bool isCustomWeapon;

void hud_adjust_mode_keyb(int dik)
{
    if (pInput->iGetAsyncKeyState(SDL_SCANCODE_LALT) && isCustomWeapon)
    {
        if (dik == SDL_SCANCODE_KP_0 || dik == SDL_SCANCODE_0)
            hud_adj_mode = 0;
        if (dik == SDL_SCANCODE_KP_1 || dik == SDL_SCANCODE_1)
            hud_adj_mode = 1;
        if (dik == SDL_SCANCODE_KP_2 || dik == SDL_SCANCODE_2)
            hud_adj_mode = 2;
        if (dik == SDL_SCANCODE_KP_3 || dik == SDL_SCANCODE_3)
            hud_adj_mode = 3;
        if (dik == SDL_SCANCODE_KP_4 || dik == SDL_SCANCODE_4)
            hud_adj_mode = 4;
        if (dik == SDL_SCANCODE_KP_5 || dik == SDL_SCANCODE_5)
            hud_adj_mode = 5;
        if (dik == SDL_SCANCODE_KP_6 || dik == SDL_SCANCODE_6)
            hud_adj_mode = 6;
        if (dik == SDL_SCANCODE_KP_7 || dik == SDL_SCANCODE_7)
            hud_adj_mode = 7;
        if (dik == SDL_SCANCODE_KP_8 || dik == SDL_SCANCODE_8)
            hud_adj_mode = 8;
        if (dik == SDL_SCANCODE_KP_9 || dik == SDL_SCANCODE_9)
            hud_adj_mode = 9;
    }
    if (pInput->iGetAsyncKeyState(SDL_SCANCODE_LCTRL))
    {
        if (dik == SDL_SCANCODE_KP_0 || dik == SDL_SCANCODE_0)
            hud_adj_item_idx = 0;
        if (dik == SDL_SCANCODE_KP_1 || dik == SDL_SCANCODE_1)
            hud_adj_item_idx = 1;
        if (dik == SDL_SCANCODE_KP_2) //--#SM+#--
            bNextChildItem = true;
        g_player_hud->tune(Ivector().set(0, 0, 0)); //--#SM+#--
    }
}
