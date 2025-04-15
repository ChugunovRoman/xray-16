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
        curr_measures = current_hud_item->m_measures;
        CWeapon* wpn = smart_cast<CWeapon*>(current_hud_item->m_parent_hud_item);
        CEliteDetector* detector = smart_cast<CEliteDetector*>(current_hud_item->m_parent_hud_item);

        if (wpn)
        {
            wpn->LoadAltHudAim();
            m_hands_curr_offset[0][0] = wpn->m_hands_offset[0][1];
            m_hands_curr_offset[1][0] = wpn->m_hands_offset[1][1];
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

        if (detector)
        {
            detector->set_map_offset_pos(m_artefact_map_p);
            detector->set_map_offset_rot(m_artefact_map_r);
            detector->RecalcMapAttachOffset();
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
                Fvector offset{};
                Fvector cam_pos = Device.vCameraPosition;
                Fvector cam_dir{}, cam_dir_m{};
                cam_dir.set(Device.vCameraDirection);
                cam_dir_m.set(Device.vCameraDirection);
                cam_dir.mul(0.5);
                Fvector center{}, point_1{}, point_2{}, result_1{}, result_2{};
                center.set(cam_pos);
                center.add(cam_dir);

                point_1.set(center);
                point_1.sub(hud_item->hands_attach_pos());

                point_2.set(center);
                result_2.set(center);
                float offset_y = 0;

                offset.set(hud_item->hands_attach_pos());
                CWeapon* wpn = smart_cast<CWeapon*>(hud_item->m_parent_hud_item);
                if (wpn && wpn->bUseAttachmentSystem)
                {
                    for (auto item: wpn->m_addon_items)
                    {
                        u16 bone_id = item.second->addon_item_model->LL_BoneID("wpn_scope");
                        if (bone_id != BI_NONE && item.second->addon_item_visible && xr_strcmp(*item.second->addon_type, "base_scope") == 0)
                        {
                            auto bi = item.second->addon_item_model->LL_GetBoneInstance(bone_id);
                            offset.add(item.second->addon_item_pos);
                            offset.add(bi.mTransform.c);

                            point_2.sub(offset);

                            // float y = abs(hud_item->hands_attach_pos().y + bi.mTransform.c.y);
                            // float y = bi.mTransform.c.y - hud_item->m_measures.m_hands_offset[0][1].y;
                            float y1, y2, y3;
                            result_2.set(center);
                            result_2.sub(point_2);
                            result_2.x = -(result_2.x + (item.second->addon_item_pos.x + bi.mTransform.c.x));
                            // result_2.y = y;
                            result_2.z = hud_item->hands_attach_pos().z;

                            float y = abs(hud_item->hands_attach_pos().y + result_2.y);

                            offset_y = (center.y - (center.y * y)) * y;
                            y1 = center.y * y;
                            y2 = (center.y - (center.y * y)) * 0.1;
                            y3 = y2 * y;

                            ImGui::LabelText("y=", "[%.7f]", y);
                            ImGui::LabelText("y1=", "[%.7f]", y1);
                            ImGui::LabelText("y2=", "[%.7f]", y2);
                            ImGui::LabelText("y3=", "[%.7f]", y3);
                            ImGui::LabelText("center.y - point_2.y * 0.1=", "[%.7f]", (center.y - point_2.y) * 0.1);
                            ImGui::LabelText("center.y - point_2.y=", "[%.7f]", center.y - point_2.y);
                            ImGui::LabelText("center.y - point_1.y=", "[%.7f]", center.y - point_1.y);
                        }
                    }
                }

                result_1.set(center);
                result_1.sub(point_1);

                ImGui::LabelText("Device.vCameraPosition=", "[%.7f, %.7f, %.7f]", cam_pos.x, cam_pos.y, cam_pos.z);
                ImGui::LabelText("Device.vCameraPosition center=", "[%.7f, %.7f, %.7f]", center.x, center.y, center.z);

                ImGui::LabelText("m_item_transform.c=", "[%.7f, %.7f, %.7f]",hud_item->m_item_transform.c.x,hud_item->m_item_transform.c.y,hud_item->m_item_transform.c.z);
                ImGui::LabelText("point_1=", "[%.7f, %.7f, %.7f]",point_1.x,point_1.y,point_1.z);
                ImGui::LabelText("point_2=", "[%.7f, %.7f, %.7f]",point_2.x,point_2.y,point_2.z);
                ImGui::LabelText("result_1=", "[%.7f, %.7f, %.7f]",result_1.x,result_1.y,result_1.z);
                ImGui::LabelText("result_2=", "[%.7f, %.7f, %.7f]",result_2.x,result_2.y,result_2.z);
                ImGui::LabelText("offset_y=", "[%.7f]",offset_y);

                ImGui::LabelText("Device.vCameraDirection=", "[%.7f, %.7f, %.7f]",cam_dir.x,cam_dir.y,cam_dir.z);
                ImGui::LabelText("Device.vCameraDirection mul 0.5=", "[%.7f, %.7f, %.7f]",cam_dir.x,cam_dir.y,cam_dir.z);

                ImGui::LabelText("offset=", "[%.7f, %.7f, %.7f]",offset.x,offset.y,offset.z);
                ImGui::LabelText("hands_attach_pos=", "[%.7f, %.7f, %.7f]",hud_item->hands_attach_pos().x,hud_item->hands_attach_pos().y,hud_item->hands_attach_pos().z);
                ImGui::LabelText("hands_attach_rot=", "[%.7f, %.7f, %.7f]",hud_item->hands_attach_rot().x,hud_item->hands_attach_rot().y,hud_item->hands_attach_rot().z);
                ImGui::LabelText("hands_offset_pos=", "[%.7f, %.7f, %.7f]",hud_item->hands_offset_pos().x,hud_item->hands_offset_pos().y,hud_item->hands_offset_pos().z);
                ImGui::LabelText("hands_offset_rot=", "[%.7f, %.7f, %.7f]",hud_item->hands_offset_rot().x,hud_item->hands_offset_rot().y,hud_item->hands_offset_rot().z);
                ImGui::LabelText("m_attach_offset.c=", "[%.7f, %.7f, %.7f]",hud_item->m_attach_offset.c.x,hud_item->m_attach_offset.c.y,hud_item->m_attach_offset.c.z);
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

                    Fvector parallelPoint;
                    parallelPoint.set(point);
                    parallelPoint.mad(dir, RQ.range);
                    render.draw_aabb(parallelPoint, debug_point_size, debug_point_size, debug_point_size, color_xrgb(255, 0, 0));
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
            }

            if (ImGui::Button("Reset to default values"))
            {
                ResetToDefaultValues();
            }
        }

        ImGui::NewLine();

        if (ImGui::CollapsingHeader("Attachment Settings", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (current_hud_item)
            {
                CWeapon* wpn = smart_cast<CWeapon*>(current_hud_item->m_parent_hud_item);
                if (wpn && wpn->bUseAttachmentSystem && wpn->m_addon_items.size() > 0)
                {
                    for (auto item: wpn->m_addon_items)
                    {
                        if (!item.second->addon_item_visible)
                            continue;

                        ImGui::LabelText("Current item", "%s", item.first);

                        if (ImGui::RadioButton(make_string("Visible %s", item.first).c_str(), item.second->addon_item_visible))
                            item.second->addon_item_visible = !item.second->addon_item_visible;

                        ImGui::NewLine();

                        ImGui::DragFloat3(make_string("Addon %s Position", item.first).c_str(), (float*)&wpn->m_addon_items[item.first]->addon_item_pos, _delta_pos, 0.f, 0.f, "%.7f");
                        ImGui::DragFloat3(make_string("Addon %s Rotation", item.first).c_str(), (float*)&wpn->m_addon_items[item.first]->addon_item_hpb, _delta_rot, 0.f, 0.f, "%.7f");
                        ImGui::DragFloat3(make_string("Addon %s Scale", item.first).c_str(), (float*)&wpn->m_addon_items[item.first]->addon_item_scale, _delta_pos, 0.f, 0.f, "%.7f");
                        ImGui::DragFloat3(make_string("Addon %s Dot Pos", item.first).c_str(), (float*)&wpn->m_addon_items[item.first]->addon_item_dot_pos, _delta_pos, 0.f, 0.f, "%.7f");

                        ImGuiIO& io = ImGui::GetIO();
                        string512 selectable;

                        if (ImGui::Button(make_string("Copy %s values to clipboard", item.first).c_str()))
                        {
                            ImGui::LogToClipboard();

                            xr_sprintf(selectable, "%s_hud_hpb = %f, %f, %f\n", item.first, item.second->addon_item_hpb.x, item.second->addon_item_hpb.y, item.second->addon_item_hpb.z);
                            ImGui::LogText(selectable);
                            xr_sprintf(selectable, "%s_hud_pos = %f, %f, %f\n", item.first, item.second->addon_item_pos.x, item.second->addon_item_pos.y, item.second->addon_item_pos.z);
                            ImGui::LogText(selectable);
                            xr_sprintf(selectable, "%s_hud_scale = %f, %f, %f\n", item.first, item.second->addon_item_scale.x, item.second->addon_item_scale.y, item.second->addon_item_scale.z);
                            ImGui::LogText(selectable);
                            xr_sprintf(selectable, "%s_dot_pos = %f, %f, %f\n", item.first, item.second->addon_item_dot_pos.x, item.second->addon_item_dot_pos.y, item.second->addon_item_dot_pos.z);
                            ImGui::LogText(selectable);
                            ImGui::LogFinish();
                        }

                        ImGui::NewLine();
                    }
                }
            }
        }

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
