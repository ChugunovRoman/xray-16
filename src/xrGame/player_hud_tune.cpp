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
#include "player_hud_tune.h"
#include "CustomDetector.h"
#include "EliteDetector.h"
#include "Actor.h"
#include "ActorEffector.h"
#include "xrEngine/CameraBase.h"

extern ENGINE_API float psHUD_FOV;

class CUIArtefactDetectorElite;

CHudTuner::CHudTuner()
{
    ImGui::SetCurrentContext(Device.GetImGuiContext());
    paused = fsimilar(Device.time_factor(), EPS);
}

bool CHudTuner::is_active() const
{
    return is_open() && Device.editor().IsActiveState();
}

void CHudTuner::ResetToDefaultValues()
{
    if (current_hud_item)
    {
        current_hud_item->reload_measures();
        current_hud_item->calc_addon_aim_offset();
        curr_measures = current_hud_item->m_measures;
        CWeapon* wpn = smart_cast<CWeapon*>(current_hud_item->m_parent_hud_item);
        CEliteDetector* detector = smart_cast<CEliteDetector*>(current_hud_item->m_parent_hud_item);

        if (wpn)
        {
            wpn->LoadAltHudAim();
            wpn->LoadAddonSlosts(*wpn->m_section_id, true);
            for (auto& [slot_key, transform] : wpn->m_addon_slots)
            {
                SlotTransform t;
                t.pos = transform.c;
                transform.getHPB(t.rot.x, t.rot.y, t.rot.z);
                m_weapon_slots[slot_key] = t;
            }

            m_hands_curr_offset[0][0] = wpn->m_hands_offset[0][1];
            m_hands_curr_offset[1][0] = wpn->m_hands_offset[1][1];

            if (wpn->bUseAttachmentSystem && wpn->m_addon_items.size() > 0)
                for (auto& [addon_id, item] : wpn->m_addon_items)
                    if (item->slot != nullptr)
                        item->addon_item_pos = wpn->m_addon_slots[item->slot];
        }
        if (detector)
        {
            m_artefact_map_p = detector->get_map_offset_pos();
            m_artefact_map_r = detector->get_map_offset_rot();
        }
    }
    else
    {
        Fvector zero = { 0, 0, 0 };
        curr_measures.m_hands_attach[0] = zero;
        curr_measures.m_hands_attach[1] = zero;
        curr_measures.m_hands_offset[0][0] = zero;
        curr_measures.m_hands_offset[1][0] = zero;
        curr_measures.m_hands_offset[0][1] = zero;
        curr_measures.m_hands_offset[1][1] = zero;
        curr_measures.m_hands_offset[0][2] = zero;
        curr_measures.m_hands_offset[1][2] = zero;
        m_hands_curr_offset[0][0] = zero;
        m_hands_curr_offset[1][0] = zero;
        curr_measures.m_item_attach[0] = zero;
        curr_measures.m_item_attach[1] = zero;
        curr_measures.m_fire_point_offset = zero;
        curr_measures.m_fire_point2_offset = zero;
        curr_measures.m_shell_point_offset = zero;
        m_artefact_map_p = zero;
        m_artefact_map_r = zero;
    }

    new_measures = curr_measures;
}

void CHudTuner::UpdateValues()
{
    if (current_hud_item)
    {
        current_hud_item->m_measures = new_measures;

        if (!current_hud_item->m_parent_hud_item)
            return;

        CEliteDetector* detector = smart_cast<CEliteDetector*>(current_hud_item->m_parent_hud_item);
        CWeapon* wpn = smart_cast<CWeapon*>(current_hud_item->m_parent_hud_item);

        if (detector)
        {
            detector->set_map_offset_pos(m_artefact_map_p);
            detector->set_map_offset_rot(m_artefact_map_r);
            detector->RecalcMapAttachOffset();
        }
        if (wpn && wpn->bUseAttachmentSystem)
        {
            if (wpn->m_addon_slots.size() > 0)
            {
                for (auto& [slot_key, data] : m_weapon_slots)
                {
                    Fmatrix transform;
                    transform.setHPB(data.rot.x, data.rot.y, data.rot.z);
                    transform.c.set(data.pos);
                    wpn->m_addon_slots[slot_key] = transform;
                }
            }
            if (wpn->m_addon_items.size() > 0)
            {
                for (auto& [addon_id, item] : wpn->m_addon_items)
                    if (item->parent == 0)
                        item->addon_item_pos = wpn->m_addon_slots[item->slot];
            }
        }

    }
}

void CHudTuner::on_tool_frame()
{
    if (!get_open_state())
        return;

    if (!g_player_hud[0] && !g_player_hud[1])
        return;

    auto calcColumnCount = [](float columnWidth) -> int
    {
        float windowWidth = ImGui::GetWindowWidth();
        int columnCount = _max(1, static_cast<int>(windowWidth / columnWidth));
        return columnCount;
    };

    auto hud_item = g_player_hud[current_hud_idx]->attached_item();
    if (current_hud_item != hud_item)
    {
        current_hud_item = hud_item;
        ResetToDefaultValues();
    }

    CWeapon* wpn = nullptr;
    if (current_hud_item && current_hud_item->m_parent_hud_item)
        wpn = smart_cast<CWeapon*>(current_hud_item->m_parent_hud_item);

    if (ImGui::Begin(tool_name(), &get_open_state(), get_default_window_flags()))
    {
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::RadioButton("Pause", paused))
            {
                paused = !paused;
                float time_factor = 1.f;
                if (paused)
                {
                    time_factor = EPS;
                }
                Device.time_factor(time_factor);
            }
            if (ImGui::RadioButton("PiP Scopes", psActorFlags.test(AF_3DSCOPE)))
                psActorFlags.invert(AF_3DSCOPE);

            ImGui::EndMenuBar();
        }

        if (ImGui::CollapsingHeader("Main Settings", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::BeginCombo("Hud Item Mode", hud_item_mode[current_hud_idx]))
            {
                for (const auto& [idx, value] : hud_item_mode)
                {
                    if (ImGui::Selectable(value, current_hud_idx == idx))
                    {
                        current_hud_idx = idx;
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::LabelText("Current item", "%s", hud_item ? hud_item->m_sect_name.c_str() : "none");

            ImGui::SliderFloat("HUD FOV", &psHUD_FOV, 0.1f, 1.0f);

            ImGui::NewLine();

            ImGui::SliderFloat("Position step", &_delta_pos, 0.0000001f, 0.001f, "%.7f");
            ImGui::SliderFloat("Rotation step", &_delta_rot, 0.000001f, 0.001f, "%.7f");

            ImGui::DragFloat3(hud_adj_modes[HUD_POS], (float*)&new_measures.m_hands_attach[0], _delta_pos, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[HUD_ROT], (float*)&new_measures.m_hands_attach[1], _delta_rot, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[HUD_POS_AIM], (float*)&new_measures.m_hands_offset[0][1], _delta_pos, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[HUD_ROT_AIM], (float*)&new_measures.m_hands_offset[1][1], _delta_rot, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[HUD_POS_ALT_AIM], wpn ? (float*)&wpn->m_hands_offset[0][1] : (float*)&m_hands_new_offset[0][0], _delta_pos, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[HUD_ROT_ALT_AIM], wpn ? (float*)&wpn->m_hands_offset[1][1] : (float*)&m_hands_new_offset[0][0], _delta_rot, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[HUD_POS_GL], (float*)&new_measures.m_hands_offset[0][2], _delta_pos, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[HUD_ROT_GL], (float*)&new_measures.m_hands_offset[1][2], _delta_rot, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[ITEM_POS], (float*)&new_measures.m_item_attach[0], _delta_pos, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[ITEM_ROT], (float*)&new_measures.m_item_attach[1], _delta_rot, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[FIRE_POINT], (float*)&new_measures.m_fire_point_offset, _delta_pos, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[FIRE_POINT_2], (float*)&new_measures.m_fire_point2_offset, _delta_pos, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[SHELL_POINT], (float*)&new_measures.m_shell_point_offset, _delta_pos, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[ARTEFACT_POINT_POS], (float*)&m_artefact_map_p, _delta_pos, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[ARTEFACT_POINT_ROT], (float*)&m_artefact_map_r, _delta_pos, 0.f, 0.f, "%.7f");

            if (hud_item)
            {
                ImGui::DragFloat3("Laser Dot Offset", (float*)&g_player_hud[0]->hud_laser_dot_offset, _delta_pos, 0.f, 0.f, "%.7f");
            }

            if (wpn && wpn->bUseAttachmentSystem)
            {
                if (m_weapon_slots.size() > 0)
                {
                    for (auto& [slot_id, data] : m_weapon_slots)
                    {
                        ImGui::DragFloat3(make_string("%s Pos", slot_id.c_str()).c_str(), (float*)&data.pos, _delta_pos, 0.f, 0.f, "%.7f");
                        ImGui::DragFloat3(make_string("%s Rot", slot_id.c_str()).c_str(), (float*)&data.rot, _delta_pos, 0.f, 0.f, "%.7f");
                    }
                }
                if (wpn->m_addon_items.size() > 0)
                    for (auto& [addon_id, addon] : wpn->m_addon_items)
                        ImGui::SliderFloat(make_string("%s aim z rot", *addon->addon_item_name).c_str(), (float*)&addon->addon_aim_z_rot, -5.f, 5.f, "%.7f");
            }

            UpdateValues();

            string128 selectable;

            if (current_hud_item)
            {
                bool is_16x9 = UI().is_widescreen();
                shared_str m_sect_name = current_hud_item->m_sect_name;

                ImGuiIO& io = ImGui::GetIO();

                if (ImGui::Button("Copy formatted values to clipboard"))
                {
                    ImGui::LogToClipboard();
                    xr_sprintf(selectable, "[%s]\n", m_sect_name.c_str());
                    ImGui::LogText(selectable);
                    xr_sprintf(selectable, "hands_position%s = %f,%f,%f\n", (is_16x9) ? "_16x9" : "", new_measures.m_hands_attach[0].x, new_measures.m_hands_attach[0].y, new_measures.m_hands_attach[0].z);
                    ImGui::LogText(selectable);
                    xr_sprintf(selectable, "hands_orientation%s = %f,%f,%f\n", (is_16x9) ? "_16x9" : "", new_measures.m_hands_attach[1].x, new_measures.m_hands_attach[1].y, new_measures.m_hands_attach[1].z);
                    ImGui::LogText(selectable);
                    xr_sprintf(selectable, "aim_hud_offset_pos%s = %f,%f,%f\n", (is_16x9) ? "_16x9" : "", new_measures.m_hands_offset[0][1].x, new_measures.m_hands_offset[0][1].y, new_measures.m_hands_offset[0][1].z);
                    ImGui::LogText(selectable);
                    xr_sprintf(selectable, "aim_hud_offset_rot%s = %f,%f,%f\n", (is_16x9) ? "_16x9" : "", new_measures.m_hands_offset[1][1].x, new_measures.m_hands_offset[1][1].y, new_measures.m_hands_offset[1][1].z);
                    ImGui::LogText(selectable);
                    xr_sprintf(selectable, "aim_hud_offset_alt_pos%s = %f,%f,%f\n", (is_16x9) ? "_16x9" : "", wpn->m_hands_offset[0][1].x, wpn->m_hands_offset[0][1].y, wpn->m_hands_offset[0][1].z);
                    ImGui::LogText(selectable);
                    xr_sprintf(selectable, "aim_hud_offset_alt_rot%s = %f,%f,%f\n", (is_16x9) ? "_16x9" : "", wpn->m_hands_offset[1][1].x, wpn->m_hands_offset[1][1].y, wpn->m_hands_offset[1][1].z);
                    ImGui::LogText(selectable);
                    xr_sprintf(selectable, "gl_hud_offset_pos%s = %f,%f,%f\n", (is_16x9) ? "_16x9" : "", new_measures.m_hands_offset[0][2].x, new_measures.m_hands_offset[0][2].y, new_measures.m_hands_offset[0][2].z);
                    ImGui::LogText(selectable);
                    xr_sprintf(selectable, "gl_hud_offset_rot%s = %f,%f,%f\n", (is_16x9) ? "_16x9" : "", new_measures.m_hands_offset[1][2].x, new_measures.m_hands_offset[1][2].y, new_measures.m_hands_offset[1][2].z);
                    ImGui::LogText(selectable);
                    xr_sprintf(selectable, "item_position = %f,%f,%f\n", new_measures.m_item_attach[0].x, new_measures.m_item_attach[0].y, new_measures.m_item_attach[0].z);
                    ImGui::LogText(selectable);
                    xr_sprintf(selectable, "item_orientation = %f,%f,%f\n", new_measures.m_item_attach[1].x, new_measures.m_item_attach[1].y, new_measures.m_item_attach[1].z);
                    ImGui::LogText(selectable);
                    xr_sprintf(selectable, "fire_point = %f,%f,%f\n", new_measures.m_fire_point_offset.x, new_measures.m_fire_point_offset.y, new_measures.m_fire_point_offset.z);
                    ImGui::LogText(selectable);
                    xr_sprintf(selectable, "fire_point = %f,%f,%f\n", new_measures.m_fire_point2_offset.x, new_measures.m_fire_point2_offset.y, new_measures.m_fire_point2_offset.z);
                    ImGui::LogText(selectable);
                    xr_sprintf(selectable, "shell_point = %f,%f,%f\n", new_measures.m_shell_point_offset.x, new_measures.m_shell_point_offset.y, new_measures.m_shell_point_offset.z);
                    ImGui::LogText(selectable);
                    xr_sprintf(selectable, "ui_p = %f,%f,%f\n", m_artefact_map_p.x, m_artefact_map_p.y, m_artefact_map_p.z);
                    ImGui::LogText(selectable);
                    xr_sprintf(selectable, "ui_r = %f,%f,%f\n", m_artefact_map_r.x, m_artefact_map_r.y, m_artefact_map_r.z);
                    ImGui::LogText(selectable);
                    if (wpn && wpn->bUseAttachmentSystem)
                    {
                        if (wpn->m_addon_items.size() > 0)
                        for (auto& [addon_id, addon] : wpn->m_addon_items)
                        {
                            xr_sprintf(selectable, "%s_aim_z_rotation = %f\n", *addon->addon_item_name, addon->addon_aim_z_rot);
                            ImGui::LogText(selectable);
                        }
                        for (auto& [slot_id, offset] : wpn->m_addon_slots)
                        {
                            float h, p, b;
                            offset.getHPB(h, p, b);
                            xr_sprintf(selectable, "addon_%s_offset = %f,%f,%f,%f\n", slot_id.c_str(), offset.c.x, offset.c.y, offset.c.z, b);
                            ImGui::LogText(selectable);
                        }
                    }
                    ImGui::LogFinish();
                }

                ImGui::NewLine();

                firedeps fd;
                current_hud_item->setup_firedeps(fd);
                collide::rq_result& RQ = HUD().GetCurrentRayQuery();

                CDebugRenderer& render = Level().debug_renderer();

                ImGui::SliderFloat("Debug Point Size", &debug_point_size, 0.00005f, 1.f, "%.5f");

                if (ImGui::BeginTable("Show Debug Widgets", calcColumnCount(210.f)))
                {
                    ImGui::TableNextColumn();
                    if (ImGui::RadioButton("Draw Fire Point", draw_fp)) { draw_fp = !draw_fp; };
                    ImGui::TableNextColumn();
                    if (ImGui::RadioButton("Draw Fire Point (GL)", draw_fp2)) { draw_fp2 = !draw_fp2; }
                    ImGui::TableNextColumn();
                    if (ImGui::RadioButton("Draw Fire Direction", draw_fd)) { draw_fd = !draw_fd; }
                    ImGui::TableNextColumn();
                    if (ImGui::RadioButton("Draw Fire Direction (GL)", draw_fd2)) { draw_fd2 = !draw_fd2; }
                    ImGui::TableNextColumn();
                    if (ImGui::RadioButton("Draw Shell Point", draw_sp)) { draw_sp = !draw_sp; }
                    ImGui::TableNextColumn();
                    if (ImGui::RadioButton("Draw Bones", draw_bones)) { draw_bones = !draw_bones; }
                    ImGui::TableNextColumn();
                    if (ImGui::RadioButton("Draw Bones Addon", draw_bones_addon)) { draw_bones_addon = !draw_bones_addon; }
                    ImGui::TableNextColumn();
                    if (ImGui::RadioButton("Draw Center", draw_center)) { draw_center = !draw_center; }
                    ImGui::EndTable();
                }

                if (draw_fp)
                {
                    Fvector point;
                    point.set(fd.vLastFP);
                    current_hud_item->m_parent_hud_item->TransformPosFromWorldToHud(point);
                    render.draw_aabb(point, debug_point_size, debug_point_size, debug_point_size, color_xrgb(255, 0, 0));
                }

                if (draw_fp2)
                {
                    Fvector point;
                    point.set(fd.vLastFP2);
                    current_hud_item->m_parent_hud_item->TransformPosFromWorldToHud(point);
                    render.draw_aabb(point, debug_point_size, debug_point_size, debug_point_size, color_xrgb(255, 0, 0));
                }

                if (draw_fd)
                {
                    Fvector point;
                    Fvector dir;
                    point.set(fd.vLastFP);
                    dir.set(fd.vLastFD);
                    current_hud_item->m_parent_hud_item->TransformPosFromWorldToHud(point);
                    current_hud_item->m_parent_hud_item->TransformDirFromWorldToHud(dir);

                    float range = RQ.range;
                    clamp(range, 2.f, 5.f);
                    Fvector parallelPoint;
                    parallelPoint.set(point);
                    parallelPoint.mad(dir, range);
                    Msg("draw_fd, point: [%f, %f, %f]", point.x, point.y, point.z);
                    Msg("draw_fd, dir: [%f, %f, %f]", dir.x, dir.y, dir.z);
                    Msg("draw_fd, parallelPoint: [%f, %f, %f]", parallelPoint.x, parallelPoint.y, parallelPoint.z);
                    render.draw_aabb(parallelPoint, 0.01f, 0.01f, 0.01f, color_xrgb(255, 0, 0));
                    render.draw_line(Fidentity, point, parallelPoint, color_xrgb(255, 0, 0));
                }

                if (draw_fd2)
                {
                    Fvector point;
                    Fvector dir;
                    point.set(fd.vLastFP2);
                    dir.set(fd.vLastFD);
                    current_hud_item->m_parent_hud_item->TransformPosFromWorldToHud(point);
                    current_hud_item->m_parent_hud_item->TransformDirFromWorldToHud(dir);

                    Fvector parallelPoint;
                    parallelPoint.set(point);
                    parallelPoint.mad(dir, RQ.range);
                    render.draw_aabb(parallelPoint, debug_point_size, debug_point_size, debug_point_size, color_xrgb(255, 0, 0));
                    render.draw_line(Fidentity, point, parallelPoint, color_xrgb(255, 0, 0));
                }

                if (draw_sp)
                {
                    Fvector point;
                    point.set(fd.vLastSP);
                    current_hud_item->m_parent_hud_item->TransformPosFromWorldToHud(point);
                    render.draw_aabb(point, debug_point_size, debug_point_size, debug_point_size, color_xrgb(255, 0, 0));
                }

                if (draw_center)
                {
                    Fvector center;
                    center.mad(Device.vCameraPosition, Device.vCameraDirection, 0.5f);

                    current_hud_item->m_parent_hud_item->TransformPosFromWorldToHud(center);
                    render.draw_aabb(center, debug_point_size, debug_point_size, debug_point_size, color_xrgb(0, 100, 255));
                    dbg_center.set(center);
                }

                if (draw_bones)
                {
                    CWeapon* wpn = smart_cast<CWeapon*>(current_hud_item->m_parent_hud_item);
                    if (wpn)
                    {
                        IKinematics* ik = current_hud_item->m_model;

                        for (const auto& [bone_name, bone_id] : *ik->LL_Bones())
                        {
                            auto data = ik->LL_GetTransform(bone_id);

                            Fmatrix hud_transform;
                            hud_transform.set(current_hud_item->m_item_transform);
                            hud_transform.mulB_43(data);

                            current_hud_item->m_parent_hud_item->TransformPosFromWorldToHud(hud_transform.c);
                            render.draw_aabb(hud_transform.c, debug_point_size, debug_point_size, debug_point_size, color_xrgb(255, 255, 0));
                        }
                    }
                }
                if (draw_bones_addon)
                {
                    CWeapon* wpn = smart_cast<CWeapon*>(current_hud_item->m_parent_hud_item);
                    if (wpn && wpn->bUseAttachmentSystem)
                    {
                        for (auto [addon_id, item]: wpn->m_addon_items)
                        {
                            for (const auto& [bone_name, bone_id] : *item->addon_item_model->LL_Bones())
                            {
                                auto data = item->addon_item_model->LL_GetTransform(bone_id);

                                Fmatrix hud_transform;
                                hud_transform.set(current_hud_item->m_item_transform);
                                
                                hud_transform.mulB_43(item->addon_item_pos);
                                hud_transform.mulB_43(data);

                                current_hud_item->m_parent_hud_item->TransformPosFromWorldToHud(hud_transform.c);
                                render.draw_aabb(hud_transform.c, 0.002, 0.002, 0.002, color_xrgb(0, 255, 255));
                            }
                        }
                    }
                }
            }

            if (ImGui::Button("Reset to default values"))
            {
                ResetToDefaultValues();
            }
        }

        ImGui::NewLine();

        // if (ImGui::CollapsingHeader("Attachment Settings", ImGuiTreeNodeFlags_DefaultOpen))
        // {
        //     if (current_hud_item)
        //     {
        //         CWeapon* wpn = smart_cast<CWeapon*>(current_hud_item->m_parent_hud_item);
        //         if (wpn && wpn->bUseAttachmentSystem && wpn->m_addon_items.size() > 0)
        //         {
        //             for (auto item: wpn->m_addon_items)
        //             {
        //                 if (!item->addon_item_visible)
        //                     continue;

        //                 ImGui::LabelText("Current item", "%s", item.first);

        //                 if (ImGui::RadioButton(make_string("Visible %s", item.first).c_str(), item->addon_item_visible))
        //                     item->addon_item_visible = !item->addon_item_visible;

        //                 ImGui::NewLine();

        //                 ImGui::DragFloat4(make_string("Addon %s Position", item.first).c_str(), (float*)&wpn->m_addon_items[item.first]->addon_item_pos, _delta_pos, 0.f, 0.f, "%.7f");
        //                 ImGui::DragFloat3(make_string("Addon Dot %s Position", item.first).c_str(), (float*)&wpn->m_addon_items[item.first]->addon_item_pos_dot, _delta_pos, 0.f, 0.f, "%.7f");

        //                 ImGuiIO& io = ImGui::GetIO();
        //                 string512 selectable;

        //                 if (ImGui::Button(make_string("Copy %s values to clipboard", item.first).c_str()))
        //                 {
        //                     ImGui::LogToClipboard();

        //                     xr_sprintf(selectable, "addon_%s_offset = %f, %f, %f, %f\n", item->slot, item->addon_item_pos.x, item->addon_item_pos.y, item->addon_item_pos.z, item->addon_item_pos.w);
        //                     ImGui::LogText(selectable);
        //                     ImGui::LogFinish();
        //                 }

        //                 ImGui::NewLine();
        //             }
        //         }
        //     }
        // }

        if (current_hud_item && ImGui::CollapsingHeader("Bone and Animation Debugging", ImGuiTreeNodeFlags_DefaultOpen))
        {
            IKinematics* ik = current_hud_item->m_model;
            ImGui::Text("Bone Count = %i", ik->LL_BoneCount());
            ImGui::Text("Root Bone = %s, ID: %i", ik->LL_BoneName_dbg(ik->LL_GetBoneRoot()), ik->LL_GetBoneRoot());

            if (ImGui::BeginTable("Bone Visibility", calcColumnCount(125.f)))
            {
                for (const auto& [bone_name, bone_id] : *ik->LL_Bones())
                {
                    if (bone_id == ik->LL_GetBoneRoot())
                        continue;

                    ImGui::TableNextColumn();
                    bool visible = ik->LL_GetBoneVisible(bone_id);
                    if (ImGui::RadioButton(bone_name.c_str(), visible)) { visible = !visible; };
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    {
                        ImGui::SetTooltip("Bone Name = %s, ID: %i", bone_name.c_str(), bone_id);
                    }
                    ik->LL_SetBoneVisible(bone_id, visible, FALSE);
                }
                ImGui::EndTable();
            }

            ImGui::NewLine();
            if (ImGui::BeginTable("Animations", calcColumnCount(125.f)))
            {
                for (const auto& [anim_name, motion] : current_hud_item->m_hand_motions.m_anims)
                {
                    if (strstr(anim_name.c_str(), "_16x9"))
                        continue;

                    ImGui::TableNextColumn();
                    if (ImGui::Button(anim_name.c_str()))
                    {
                        current_hud_item->m_parent_hud_item->PlayHUDMotion_noCB(anim_name, false);
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    {
                        ImGui::SetTooltip("%s = %s, %s", anim_name.c_str(), motion.m_base_name.c_str(), motion.m_additional_name.c_str());
                    }
                }

                ImGui::NewLine();

                for (const auto& [anim_name, motion] : current_hud_item->m_hand_motions.m_anims)
                {
                    if (!strstr(anim_name.c_str(), "_16x9"))
                        continue;

                    ImGui::TableNextColumn();
                    if (ImGui::Button(anim_name.c_str()))
                    {
                        current_hud_item->m_parent_hud_item->PlayHUDMotion_noCB(anim_name, false);
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    {
                        ImGui::SetTooltip("%s = %s, %s", anim_name.c_str(), motion.m_base_name.c_str(), motion.m_additional_name.c_str());
                    }
                }
                ImGui::EndTable();
            }
        }
    }
    ImGui::End();
}
