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
#include "Weapon.h"
#include "CustomDetector.h"
#include "EliteDetector.h"
#include "Actor.h"
#include "ActorEffector.h"
#include "xrEngine/CameraBase.h"

extern ENGINE_API float psHUD_FOV;
extern int g_3d_scope_type;

class CUIArtefactDetectorElite;

CHudTuner::CHudTuner()
{
    ImGui::SetCurrentContext(Device.GetImGuiContext());
    paused = fsimilar(Device.time_factor(), EPS);
}

void CHudTuner::CollectAddonWeaponTunes(pcstr section)
{
    if (!section || !section[0] || !pSettings->section_exist(section))
        return;

    const u32 line_count = pSettings->line_count(section);
    for (u32 i = 0; i < line_count; ++i)
    {
        pcstr name = nullptr;
        pcstr val = nullptr;
        if (!pSettings->r_line(section, i, &name, &val) || !name || !val)
            continue;

        if (!strncmp(name, "addon_", 6))
            continue;

        const size_t name_len = xr_strlen(name);
        if (name_len > 6 && !xr_strcmp(name + name_len - 6, "_scale"))
        {
            shared_str addon_section = make_string("%.*s", (int)(name_len - 6), name).c_str();

            AddonWeaponTune& entry = m_addon_weapon_tunes[addon_section];
            entry.scale = (float)atof(val);
            entry.has_scale = true;
            continue;
        }

        if (name_len > 7 && !xr_strcmp(name + name_len - 7, "_offset"))
        {
            shared_str addon_section = make_string("%.*s", (int)(name_len - 7), name).c_str();

            AddonWeaponTune& entry = m_addon_weapon_tunes[addon_section];
            sscanf(val, "%f,%f,%f", &entry.offset.x, &entry.offset.y, &entry.offset.z);
            entry.has_offset = true;
        }
    }
}

void CHudTuner::ApplyAddonWeaponTunes(CWeapon* wpn)
{
    if (!wpn || !wpn->bUseAttachmentSystem)
        return;

    for (auto& [addon_sect, tune] : m_addon_weapon_tunes)
    {
        if (tune.has_offset)
            wpn->m_addon_section_offsets[addon_sect] = tune.offset;
    }

    for (auto& [addon_id, item] : wpn->m_addon_items)
    {
        auto tune_it = m_addon_weapon_tunes.find(item->addon_item_name);
        if (tune_it == m_addon_weapon_tunes.end())
            continue;

        if (tune_it->second.has_scale)
            item->scale = tune_it->second.scale;

        if (item->parent_id != 0)
            continue;

        addon_slot* slot = wpn->m_addon_slots[item->slot];
        if (!slot)
            continue;

        item->addon_item_pos = slot->transform;
        if (auto offset_it = wpn->m_addon_section_offsets.find(item->addon_item_name); offset_it != wpn->m_addon_section_offsets.end())
            item->addon_item_pos.translate_over(offset_it->second);

        item->addon_item_pos_world = item->addon_item_pos;
        item->addon_item_pos_world.mulB_43(wpn->bAttachmentSystemOffsetOnWorldModel);
    }
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
            wpn->LoadAddonSlosts(wpn->m_section_id.c_str());

            m_addon_weapon_tunes.clear();
            if (pSettings->line_exist(wpn->m_section_id.c_str(), "parent_section"))
            {
                shared_str parent = pSettings->r_string(wpn->m_section_id.c_str(), "parent_section");
                if (parent.size() && pSettings->section_exist(parent.c_str()))
                    CollectAddonWeaponTunes(parent.c_str());
            }
            CollectAddonWeaponTunes(wpn->m_section_id.c_str());

            for (auto& [slot_key, slot] : wpn->m_addon_slots)
            {
                if (slot == nullptr)
                    continue;
                SlotTransform t;
                t.pos = slot->transform.c;
                t.pos_2 = slot->transform_2.c;
                slot->transform.getHPB(t.rot.x, t.rot.y, t.rot.z);
                slot->transform_2.getHPB(t.rot_2.x, t.rot_2.y, t.rot_2.z);
                t.type = slot->slot_type;
                t.bone_name = slot->bone_name;
                t.bone_2_name = slot->bone_2_name;
                m_weapon_slots[slot_key] = t;
            }

            m_hands_curr_offset[0][0] = wpn->m_hands_offset[0][1];
            m_hands_curr_offset[1][0] = wpn->m_hands_offset[1][1];

            if (wpn->bUseAttachmentSystem)
            {
                auto addon = wpn->GetAddonMainScope();
                if (addon.second)
                {
                    curr_measures.m_hands_offset[0][1].set(addon.second->calc_aim_offset);
                    curr_measures.m_hands_offset[1][1].set(addon.second->calc_aim_rot);
                }
            }
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
        curr_measures.m_hands_offset[0][3] = zero;
        curr_measures.m_hands_offset[1][3] = zero;
        curr_measures.m_hands_offset[0][4] = zero;
        curr_measures.m_hands_offset[1][4] = zero;
        m_hands_curr_offset[0][0] = zero;
        m_hands_curr_offset[1][0] = zero;
        curr_measures.m_item_attach[0] = zero;
        curr_measures.m_item_attach[1] = zero;
        curr_measures.m_fire_point_offset = zero;
        curr_measures.m_fire_point2_offset = zero;
        curr_measures.m_shell_point_offset = zero;
        m_artefact_map_p = zero;
        m_artefact_map_r = zero;
        m_addon_weapon_tunes.clear();
    }

    collide::rq_result& RQ = HUD().GetCurrentRayQuery();

    if (RQ.O)
    {
        CWeapon* target_wpn = smart_cast<CWeapon*>(RQ.O);
        if (target_wpn)
        {
            world_addons_pos = target_wpn->bAttachmentSystemOffsetOnWorldModel.c;

            if (m_addon_weapon_tunes.empty())
            {
                if (pSettings->line_exist(target_wpn->m_section_id.c_str(), "parent_section"))
                {
                    shared_str parent = pSettings->r_string(target_wpn->m_section_id.c_str(), "parent_section");
                    if (parent.size() && pSettings->section_exist(parent.c_str()))
                        CollectAddonWeaponTunes(parent.c_str());
                }
                CollectAddonWeaponTunes(target_wpn->m_section_id.c_str());
            }

            for (auto& [slot_key, slot] : target_wpn->m_addon_slots)
            {
                if (slot == nullptr)
                    continue;
                SlotTransform t;
                t.pos = slot->transform.c;
                t.pos_2 = slot->transform_2.c;
                t.pos_w = slot->transform_world.c;
                slot->transform.getHPB(t.rot.x, t.rot.y, t.rot.z);
                slot->transform_2.getHPB(t.rot_2.x, t.rot_2.y, t.rot_2.z);
                slot->transform_world.getHPB(t.rot_w.x, t.rot_w.y, t.rot_w.z);
                t.type = slot->slot_type;
                t.bone_name = slot->bone_name;
                t.bone_2_name = slot->bone_2_name;
                m_weapon_slots[slot_key] = t;
            }
        }
    }

    new_measures = curr_measures;
}

void CHudTuner::UpdateValues()
{
    collide::rq_result& RQ = HUD().GetCurrentRayQuery();

    if (RQ.O)
    {
        CWeapon* target_wpn = smart_cast<CWeapon*>(RQ.O);
        if (target_wpn)
        {
            target_wpn->bAttachmentSystemOffsetOnWorldModel.c.set(world_addons_pos);
            for (auto& [slot_key, data] : m_weapon_slots)
                if (target_wpn->m_addon_slots[slot_key])
                {
                    Fmatrix transform_world;
                    transform_world.setHPB(data.rot_w.x, data.rot_w.y, data.rot_w.z);
                    transform_world.c.set(data.pos_w);
                    target_wpn->m_addon_slots[slot_key]->transform_world = transform_world;
                }
            for (auto& [addon_id, item] : target_wpn->m_addon_items)
                if (item->parent_id == 0)
                {
                    item->addon_item_pos = target_wpn->m_addon_slots[item->slot]->transform_world;
                    item->addon_item_pos_world = item->addon_item_pos;
                    item->addon_item_pos_world.mulB_43(target_wpn->bAttachmentSystemOffsetOnWorldModel);
                }
        }
    }

    if (current_hud_item)
    {
        current_hud_item->reload_measures();
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
                    if (wpn->m_addon_slots[slot_key])
                    {
                        Fmatrix transform;
                        transform.setHPB(data.rot.x, data.rot.y, data.rot.z);
                        transform.c.set(data.pos);
                        wpn->m_addon_slots[slot_key]->transform = transform;

                        Fmatrix transform_2;
                        transform_2.setHPB(data.rot_2.x, data.rot_2.y, data.rot_2.z);
                        transform_2.c.set(data.pos_2);
                        wpn->m_addon_slots[slot_key]->transform_2 = transform_2;
                    }
                }
            }   
            if (wpn->m_addon_items.size() > 0)
            {
                for (auto& [addon_id, item] : wpn->m_addon_items)
                    if (item->parent_id == 0)
                    {
                        item->addon_item_pos = wpn->m_addon_slots[item->slot]->transform;
                        item->addon_item_pos_world = item->addon_item_pos;
                        item->addon_item_pos_world.mulB_43(wpn->bAttachmentSystemOffsetOnWorldModel);
                    }
            }
            ApplyAddonWeaponTunes(wpn);
            wpn->calc_aim_addon_offset();
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
            if (ImGui::BeginCombo("3D Scopes", g_3d_scope_type == 0 ? "Off" : (g_3d_scope_type == 1 ? "PiP (lens zoom)" : "PiP (main FOV)")))
            {
                if (ImGui::Selectable("Off", g_3d_scope_type == 0))
                    g_3d_scope_type = 0;
                if (ImGui::Selectable("PiP (lens zoom)", g_3d_scope_type == 1))
                    g_3d_scope_type = 1;
                if (ImGui::Selectable("PiP (main FOV)", g_3d_scope_type == 2))
                    g_3d_scope_type = 2;
                ImGui::EndCombo();
            }

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
            ImGui::DragFloat3(hud_adj_modes[HUD_POS_COR_AIM], (float*)&new_measures.m_hands_offset[0][3], _delta_pos, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[HUD_ROT_COR_AIM], (float*)&new_measures.m_hands_offset[1][3], _delta_rot, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[HUD_ALT_POS_COR_AIM], (float*)&new_measures.m_hands_offset[0][4], _delta_pos, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[HUD_ALT_ROT_COR_AIM], (float*)&new_measures.m_hands_offset[1][4], _delta_rot, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[HUD_POS_GL], (float*)&new_measures.m_hands_offset[0][2], _delta_pos, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[HUD_ROT_GL], (float*)&new_measures.m_hands_offset[1][2], _delta_rot, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[ITEM_POS], (float*)&new_measures.m_item_attach[0], _delta_pos, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[ITEM_ROT], (float*)&new_measures.m_item_attach[1], _delta_rot, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[FIRE_POINT], (float*)&new_measures.m_fire_point_offset, _delta_pos, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[FIRE_POINT_2], (float*)&new_measures.m_fire_point2_offset, _delta_pos, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[SHELL_POINT], (float*)&new_measures.m_shell_point_offset, _delta_pos, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[ARTEFACT_POINT_POS], (float*)&m_artefact_map_p, _delta_pos, 0.f, 0.f, "%.7f");
            ImGui::DragFloat3(hud_adj_modes[ARTEFACT_POINT_ROT], (float*)&m_artefact_map_r, _delta_pos, 0.f, 0.f, "%.7f");

            collide::rq_result& RQ = HUD().GetCurrentRayQuery();

            if (RQ.O)
            {
                CWeapon* target_wpn = smart_cast<CWeapon*>(RQ.O);
                if (target_wpn)
                {
                    ImGui::DragFloat3(hud_adj_modes[ITEM_WRD_POS], (float*)&world_addons_pos, _delta_pos, 0.f, 0.f, "%.7f");
                    for (auto& [slot_id, data] : m_weapon_slots)
                    {
                        if (pSettings->line_exist(target_wpn->m_section_id.c_str(), make_string("addon_%s_offset_world", slot_id.c_str()).c_str()))
                        {
                            ImGui::DragFloat3(make_string("%s W Pos", slot_id.c_str()).c_str(), (float*)&data.pos_w, _delta_pos, 0.f, 0.f, "%.7f");
                            ImGui::DragFloat3(make_string("%s W Rot", slot_id.c_str()).c_str(), (float*)&data.rot_w, _delta_pos, 0.f, 0.f, "%.7f");
                        }
                    }
                }
            }

            if (wpn && wpn->bUseAttachmentSystem)
            {
                if (m_weapon_slots.size() > 0)
                {
                    for (auto& [slot_id, data] : m_weapon_slots)
                    {
                        ImGui::DragFloat3(make_string("%s Pos", slot_id.c_str()).c_str(), (float*)&data.pos, _delta_pos, 0.f, 0.f, "%.7f");
                        ImGui::DragFloat3(make_string("%s Rot", slot_id.c_str()).c_str(), (float*)&data.rot, _delta_pos, 0.f, 0.f, "%.7f");
                        
                        if (data.bone_2_name.c_str() != nullptr && xr_strcmp(data.bone_2_name.c_str(), "") != 0)
                        {
                            ImGui::DragFloat3(make_string("%s Pos 2", slot_id.c_str()).c_str(), (float*)&data.pos_2, _delta_pos, 0.f, 0.f, "%.7f");
                            ImGui::DragFloat3(make_string("%s Rot 2", slot_id.c_str()).c_str(), (float*)&data.rot_2, _delta_pos, 0.f, 0.f, "%.7f");
                        }
                    }
                }

                if (m_addon_weapon_tunes.size() > 0)
                {
                    if (ImGui::CollapsingHeader("Addon scale / offset", ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        for (auto& [addon_sect, tune] : m_addon_weapon_tunes)
                        {
                            if (tune.has_scale)
                            {
                                ImGui::DragFloat(
                                    make_string("%s_scale", addon_sect.c_str()).c_str(),
                                    &tune.scale,
                                    _delta_pos,
                                    0.01f,
                                    10.f,
                                    "%.4f"
                                );
                            }
                            if (tune.has_offset)
                            {
                                ImGui::DragFloat3(
                                    make_string("%s_offset", addon_sect.c_str()).c_str(),
                                    (float*)&tune.offset,
                                    _delta_pos,
                                    0.f,
                                    0.f,
                                    "%.7f"
                                );
                            }
                        }
                    }
                }
            }

            UpdateValues();

            string128 selectable;

            if (current_hud_item)
            {
                bool is_16x9 = UI().is_widescreen();
                shared_str m_sect_name = current_hud_item->m_sect_name;

                ImGuiIO& io = ImGui::GetIO();
                collide::rq_result& RQ = HUD().GetCurrentRayQuery();

                if (ImGui::Button("Copy formatted values to clipboard"))
                {
                    ImGui::LogToClipboard();
                    xr_sprintf(selectable, "[%s]\n", m_sect_name.c_str());
                    ImGui::LogText("%s", selectable);
                    xr_sprintf(selectable, "hands_position%s = %f,%f,%f\n", (is_16x9) ? "_16x9" : "", new_measures.m_hands_attach[0].x, new_measures.m_hands_attach[0].y, new_measures.m_hands_attach[0].z);
                    ImGui::LogText("%s", selectable);
                    xr_sprintf(selectable, "hands_orientation%s = %f,%f,%f\n", (is_16x9) ? "_16x9" : "", new_measures.m_hands_attach[1].x, new_measures.m_hands_attach[1].y, new_measures.m_hands_attach[1].z);
                    ImGui::LogText("%s", selectable);
                    xr_sprintf(selectable, "aim_hud_offset_pos%s = %f,%f,%f\n", (is_16x9) ? "_16x9" : "", new_measures.m_hands_offset[0][1].x, new_measures.m_hands_offset[0][1].y, new_measures.m_hands_offset[0][1].z);
                    ImGui::LogText("%s", selectable);
                    xr_sprintf(selectable, "aim_hud_offset_rot%s = %f,%f,%f\n", (is_16x9) ? "_16x9" : "", new_measures.m_hands_offset[1][1].x, new_measures.m_hands_offset[1][1].y, new_measures.m_hands_offset[1][1].z);
                    ImGui::LogText("%s", selectable);
                    xr_sprintf(selectable, "aim_hud_offset_alt_pos%s = %f,%f,%f\n", (is_16x9) ? "_16x9" : "", wpn->m_hands_offset[0][1].x, wpn->m_hands_offset[0][1].y, wpn->m_hands_offset[0][1].z);
                    ImGui::LogText("%s", selectable);
                    xr_sprintf(selectable, "aim_hud_offset_alt_rot%s = %f,%f,%f\n", (is_16x9) ? "_16x9" : "", wpn->m_hands_offset[1][1].x, wpn->m_hands_offset[1][1].y, wpn->m_hands_offset[1][1].z);
                    ImGui::LogText("%s", selectable);
                    xr_sprintf(selectable, "aim_hud_correct_offset_pos%s = %f,%f,%f\n", (is_16x9) ? "_16x9" : "", new_measures.m_hands_offset[0][3].x, new_measures.m_hands_offset[0][3].y, new_measures.m_hands_offset[0][3].z);
                    ImGui::LogText("%s", selectable);
                    xr_sprintf(selectable, "aim_hud_correct_offset_rot%s = %f,%f,%f\n", (is_16x9) ? "_16x9" : "", new_measures.m_hands_offset[1][3].x, new_measures.m_hands_offset[1][3].y, new_measures.m_hands_offset[1][3].z);
                    ImGui::LogText("%s", selectable);
                    xr_sprintf(selectable, "aim_hud_correct_alt_offset_pos%s = %f,%f,%f\n", (is_16x9) ? "_16x9" : "", new_measures.m_hands_offset[0][4].x, new_measures.m_hands_offset[0][4].y, new_measures.m_hands_offset[0][4].z);
                    ImGui::LogText("%s", selectable);
                    xr_sprintf(selectable, "aim_hud_correct_alt_offset_rot%s = %f,%f,%f\n", (is_16x9) ? "_16x9" : "", new_measures.m_hands_offset[1][4].x, new_measures.m_hands_offset[1][4].y, new_measures.m_hands_offset[1][4].z);
                    ImGui::LogText("%s", selectable);
                    xr_sprintf(selectable, "gl_hud_offset_pos%s = %f,%f,%f\n", (is_16x9) ? "_16x9" : "", new_measures.m_hands_offset[0][2].x, new_measures.m_hands_offset[0][2].y, new_measures.m_hands_offset[0][2].z);
                    ImGui::LogText("%s", selectable);
                    xr_sprintf(selectable, "gl_hud_offset_rot%s = %f,%f,%f\n", (is_16x9) ? "_16x9" : "", new_measures.m_hands_offset[1][2].x, new_measures.m_hands_offset[1][2].y, new_measures.m_hands_offset[1][2].z);
                    ImGui::LogText("%s", selectable);
                    xr_sprintf(selectable, "attachment_system_offset_on_world_model = %f,%f,%f\n", world_addons_pos.x, world_addons_pos.y, world_addons_pos.z);
                    ImGui::LogText("%s", selectable);
                    xr_sprintf(selectable, "item_position = %f,%f,%f\n", new_measures.m_item_attach[0].x, new_measures.m_item_attach[0].y, new_measures.m_item_attach[0].z);
                    ImGui::LogText("%s", selectable);
                    xr_sprintf(selectable, "item_orientation = %f,%f,%f\n", new_measures.m_item_attach[1].x, new_measures.m_item_attach[1].y, new_measures.m_item_attach[1].z);
                    ImGui::LogText("%s", selectable);
                    xr_sprintf(selectable, "fire_point = %f,%f,%f\n", new_measures.m_fire_point_offset.x, new_measures.m_fire_point_offset.y, new_measures.m_fire_point_offset.z);
                    ImGui::LogText("%s", selectable);
                    xr_sprintf(selectable, "fire_point = %f,%f,%f\n", new_measures.m_fire_point2_offset.x, new_measures.m_fire_point2_offset.y, new_measures.m_fire_point2_offset.z);
                    ImGui::LogText("%s", selectable);
                    xr_sprintf(selectable, "shell_point = %f,%f,%f\n", new_measures.m_shell_point_offset.x, new_measures.m_shell_point_offset.y, new_measures.m_shell_point_offset.z);
                    ImGui::LogText("%s", selectable);
                    xr_sprintf(selectable, "ui_p = %f,%f,%f\n", m_artefact_map_p.x, m_artefact_map_p.y, m_artefact_map_p.z);
                    ImGui::LogText("%s", selectable);
                    xr_sprintf(selectable, "ui_r = %f,%f,%f\n", m_artefact_map_r.x, m_artefact_map_r.y, m_artefact_map_r.z);
                    ImGui::LogText("%s", selectable);
                    if (wpn && wpn->bUseAttachmentSystem)
                    {
                        for (auto& [slot_id, data] : m_weapon_slots)
                        {
                            xr_sprintf(selectable, "addon_%s_offset = %d,%f,%f,%f,%f,%f,%f%s\n", slot_id.c_str(), data.type, data.pos.x, data.pos.y, data.pos.z, data.rot.x, data.rot.y, data.rot.z, data.bone_name.c_str() != nullptr && xr_strcmp(data.bone_name.c_str(), "") != 0 ? make_string(",%s", data.bone_name.c_str()).c_str() : "");
                            ImGui::LogText("%s", selectable);
                            if (data.bone_2_name.c_str() != nullptr && xr_strcmp(data.bone_2_name.c_str(), "") != 0)
                            {
                                xr_sprintf(selectable, "addon_%s_offset_2 = %f,%f,%f,%f,%f,%f\n", slot_id.c_str(), data.pos_2.x, data.pos_2.y, data.pos_2.z, data.rot_2.x, data.rot_2.y, data.rot_2.z);
                                ImGui::LogText("%s", selectable);
                            }
                            if (RQ.O)
                            {
                                CWeapon* target_wpn = smart_cast<CWeapon*>(RQ.O);
                                if (target_wpn && pSettings->line_exist(target_wpn->m_section_id.c_str(), make_string("addon_%s_offset_world", slot_id.c_str()).c_str()))
                                {
                                    xr_sprintf(selectable, "addon_%s_offset_world = %f,%f,%f,%f,%f,%f\n", slot_id.c_str(), data.pos_w.x, data.pos_w.y, data.pos_w.z, data.rot_w.x, data.rot_w.y, data.rot_w.z);
                                    ImGui::LogText("%s", selectable);
                                }
                            }
                        }
                        for (auto& [addon_sect, tune] : m_addon_weapon_tunes)
                        {
                            if (tune.has_scale)
                            {
                                xr_sprintf(selectable, "%s_scale = %f\n", addon_sect.c_str(), tune.scale);
                                ImGui::LogText("%s", selectable);
                            }
                            if (tune.has_offset)
                            {
                                xr_sprintf(
                                    selectable,
                                    "%s_offset = %f,%f,%f\n",
                                    addon_sect.c_str(),
                                    tune.offset.x,
                                    tune.offset.y,
                                    tune.offset.z
                                );
                                ImGui::LogText("%s", selectable);
                            }
                        }
                    }
                    ImGui::LogFinish();
                }

                ImGui::NewLine();

                firedeps fd;
                current_hud_item->setup_firedeps(fd);

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

                // 1. Отображение костей оружия
                if (draw_bones)
                {
                    CWeapon* wpn = smart_cast<CWeapon*>(current_hud_item->m_parent_hud_item);
                    if (wpn)
                    {
                        IKinematics* ik = current_hud_item->m_model;
                        for (const auto& [bone_name, bone_id] : *ik->LL_Bones())
                        {
                            Fmatrix m_res;
                            // Стандартный mul_43 вместо mulB
                            m_res.mul_43(current_hud_item->m_item_transform, ik->LL_GetTransform(bone_id));

                            Fvector pos = m_res.c;
                            current_hud_item->m_parent_hud_item->TransformPosFromWorldToHud(pos);

                            // Рисуем точку
                            render.draw_aabb(pos, debug_point_size, debug_point_size, debug_point_size, color_xrgb(255, 255, 0));

                            Fvector text_pos = pos;
                            text_pos.y += 0.01f;
                            render.draw_debug_string(bone_name.c_str(), text_pos, 0.002f, color_xrgb(255, 255, 0));
                        }
                    }
                }

                // 2. Отображение костей аттачментов
                if (draw_bones_addon)
                {
                    CWeapon* wpn = smart_cast<CWeapon*>(current_hud_item->m_parent_hud_item);
                    if (wpn && wpn->bUseAttachmentSystem)
                    {
                        for (auto [addon_id, item] : wpn->m_addon_items)
                        {
                            Fmatrix m_addon_visual_root;

                            // 1. Определяем базовую трансформацию аддона
                            if (item->bone_name.size() > 0)
                            {
                                u16 parent_id = current_hud_item->m_model->LL_BoneID(item->bone_name.c_str());
                                Fmatrix m_parent_bone = current_hud_item->m_model->LL_GetTransform(parent_id);

                                // ВАРИАНТ А: Наследуем ПОЛНУЮ трансформацию кости (с поворотами)
                                // Это покажет, как кость addon_1 крутит весь прицел
                                m_addon_visual_root.mul_43(current_hud_item->m_item_transform, m_parent_bone);
                            }
                            else
                            {
                                // ВАРИАНТ Б: Если кости нет, берем из конфига (как раньше)
                                m_addon_visual_root.mul_43(current_hud_item->m_item_transform, item->addon_item_pos);
                            }

                            // 2. Рендерим кости внутри этой системы координат
                            for (const auto& [bone_name, bone_id] : *item->addon_item_model->LL_Bones())
                            {
                                Fmatrix m_bone_local = item->addon_item_model->LL_GetTransform(bone_id);
                                
                                Fmatrix m_final;
                                // Умножаем базу на локальную матрицу кости аддона
                                m_final.mul_43(m_addon_visual_root, m_bone_local);

                                Fvector pos = m_final.c;
                                current_hud_item->m_parent_hud_item->TransformPosFromWorldToHud(pos);
                                
                                // Рендерим точку (Cyan для костей аддона)
                                render.draw_aabb(pos, 0.002f, 0.002f, 0.002f, color_xrgb(0, 255, 255));
                                
                                // Рендерим текст
                                Fvector text_p = pos;
                                text_p.y += 0.005f;
                                render.draw_debug_string(bone_name.c_str(), text_p, 0.002f, color_xrgb(0, 255, 255));
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
