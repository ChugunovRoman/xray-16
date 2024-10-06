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
#include "Inventory.h"
#include "InventoryOwner.h"
#include "weapons_textures_addons_position.h"

CUITexturesAddonsPosition::CUITexturesAddonsPosition()
{
    ImGui::SetCurrentContext(Device.GetImGuiContext());
    paused = fsimilar(Device.time_factor(), EPS);
}

bool CUITexturesAddonsPosition::is_active() const
{
    return is_open() && Device.editor().IsActiveState();
}

void CUITexturesAddonsPosition::ResetToDefaultWpn1Values()
{
    if (g_actor != nullptr && &g_actor->inventory() != nullptr)
    {
        CInventory* inv = &Actor()->inventory();
        PIItem item_in_slot_2 = inv->ItemFromSlot(INV_SLOT_2);

        if (item_in_slot_2 && pSettings->section_exist(item_in_slot_2->m_alt_section_id))
        {
            int x, y = 0;
            x = READ_IF_EXISTS(pSettings, r_s32, item_in_slot_2->m_alt_section_id, "scope_x", 0);
            y = READ_IF_EXISTS(pSettings, r_s32, item_in_slot_2->m_alt_section_id, "scope_y", 0);
            pos[0][0] = Ivector2().set(x, y);

            x = READ_IF_EXISTS(pSettings, r_s32, item_in_slot_2->m_alt_section_id, "silencer_x", 0);
            y = READ_IF_EXISTS(pSettings, r_s32, item_in_slot_2->m_alt_section_id, "silencer_y", 0);
            pos[0][1] = Ivector2().set(x, y);

            x = READ_IF_EXISTS(pSettings, r_s32, item_in_slot_2->m_alt_section_id, "grenade_launcher_x", 0);
            y = READ_IF_EXISTS(pSettings, r_s32, item_in_slot_2->m_alt_section_id, "grenade_launcher_y", 0);
            pos[0][2] = Ivector2().set(x, y);
        }
    }
    else
    {
        // for weapon 1
        pos[0][0] = Ivector2().set(0, 0);
        pos[0][1] = Ivector2().set(0, 0);
        pos[0][2] = Ivector2().set(0, 0);
    }

    // for weapon 1
    new_pos[0][0] = pos[0][0];
    new_pos[0][1] = pos[0][1];
    new_pos[0][2] = pos[0][2];
}
void CUITexturesAddonsPosition::ResetToDefaultWpn2Values()
{
    if (g_actor != nullptr && &g_actor->inventory() != nullptr)
    {
        CInventory* inv = &Actor()->inventory();
        PIItem item_in_slot_3 = inv->ItemFromSlot(INV_SLOT_3);

        if (item_in_slot_3 && pSettings->section_exist(item_in_slot_3->m_alt_section_id))
        {
            int x, y = 0;
            x = READ_IF_EXISTS(pSettings, r_s32, item_in_slot_3->m_alt_section_id, "scope_x", 0);
            y = READ_IF_EXISTS(pSettings, r_s32, item_in_slot_3->m_alt_section_id, "scope_y", 0);
            pos[1][0] = Ivector2().set(x, y);

            x = READ_IF_EXISTS(pSettings, r_s32, item_in_slot_3->m_alt_section_id, "silencer_x", 0);
            y = READ_IF_EXISTS(pSettings, r_s32, item_in_slot_3->m_alt_section_id, "silencer_y", 0);
            pos[1][1] = Ivector2().set(x, y);

            x = READ_IF_EXISTS(pSettings, r_s32, item_in_slot_3->m_alt_section_id, "grenade_launcher_x", 0);
            y = READ_IF_EXISTS(pSettings, r_s32, item_in_slot_3->m_alt_section_id, "grenade_launcher_y", 0);
            pos[1][2] = Ivector2().set(x, y);
        }
    }
    else
    {
        // for weapon 2
        pos[1][0] = Ivector2().set(0, 0);
        pos[1][1] = Ivector2().set(0, 0);
        pos[1][2] = Ivector2().set(0, 0);
    }

    // for weapon 2
    new_pos[1][0] = pos[1][0];
    new_pos[1][1] = pos[1][1];
    new_pos[1][2] = pos[1][2];
}

void CUITexturesAddonsPosition::UpdateValues()
{
    if (g_actor != nullptr && &g_actor->inventory() != nullptr)
    {
        CInventory* inv = &Actor()->inventory();
        PIItem item_in_slot_2 = inv->ItemFromSlot(INV_SLOT_2);
        PIItem item_in_slot_3 = inv->ItemFromSlot(INV_SLOT_3);

        if (item_in_slot_2)
        {
            CWeapon* wpn = smart_cast<CWeapon*>(item_in_slot_2);

            if (wpn)
            {
                // Msg("UpdateValues, wpn1=[%s] scope=[%d, %d] sil=[%d, %d]", *wpn->m_alt_section_id, new_pos[1][0].x, new_pos[1][0].y, new_pos[1][1].x, new_pos[1][1].y);
                wpn->SetScopeOffset(new_pos[0][0]);
                wpn->SetSilencerOffset(new_pos[0][1]);
                wpn->SetGLOffset(new_pos[0][2]);
            }
        }
        if (item_in_slot_3)
        {
            CWeapon* wpn = smart_cast<CWeapon*>(item_in_slot_3);

            if (wpn)
            {
                // Msg("UpdateValues, wpn2=[%s] scope=[%d, %d] sil=[%d, %d]", *wpn->m_alt_section_id, new_pos[1][0].x, new_pos[1][0].y, new_pos[1][1].x, new_pos[1][1].y);
                wpn->SetScopeOffset(new_pos[1][0]);
                wpn->SetSilencerOffset(new_pos[1][1]);
                wpn->SetGLOffset(new_pos[1][2]);
            }
        }
    }
}

void CUITexturesAddonsPosition::OnFrame()
{
#ifndef MASTER_GOLD
    if (!get_open_state())
        return;

    if (!g_actor)
        return;

    CInventory* inv = &Actor()->inventory();

    if (!inv)
        return;

    PIItem item_in_slot_2 = inv->ItemFromSlot(INV_SLOT_2);
    PIItem item_in_slot_3 = inv->ItemFromSlot(INV_SLOT_3);

    pcstr item1Sect = "[No weapon in slot]";
    pcstr item2Sect = "[No weapon in slot]";

    if (item_in_slot_2 && curr_wpn_1 != *item_in_slot_2->m_alt_section_id)
        ResetToDefaultWpn1Values();
    if (item_in_slot_3 && curr_wpn_2 != *item_in_slot_3->m_alt_section_id)
        ResetToDefaultWpn2Values();

    if (item_in_slot_2)
    {
        item1Sect = *item_in_slot_2->m_alt_section_id;
        curr_wpn_1 = item1Sect;
    }
    if (item_in_slot_3)
    {
        item2Sect = *item_in_slot_3->m_alt_section_id;
        curr_wpn_2 = item2Sect;
    }

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

            ImGui::EndMenuBar();
        }

        ImGui::NewLine();

        ImGui::BeginChild("Weapon 1", ImVec2(ImGui::GetWindowWidth() / 2, ImGui::GetContentRegionAvail().y), true, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_AlwaysVerticalScrollbar);
        ImGui::Text(item1Sect);
        if (item_in_slot_2)
        {
            ImGui::NewLine();

            ImGui::SliderInt("Position step", &item_1_delta_pos, 0, 10, "%d");

            ImGui::NewLine();

            ImGui::DragInt2(addons_pos[SCOPE_POS], (int*)&new_pos[0][0], (float)item_1_delta_pos, min_v, max_v, "%d");
            ImGui::DragInt2(addons_pos[SIL_POS], (int*)&new_pos[0][1], (float)item_1_delta_pos, min_v, max_v, "%d");
            ImGui::DragInt2(addons_pos[GL_POS], (int*)&new_pos[0][2], (float)item_1_delta_pos, min_v, max_v, "%d");

            ImGui::NewLine();

            if (ImGui::Button("Copy values to clipboard"))
            {
                string128 selectable;

                ImGui::LogToClipboard();
                xr_sprintf(selectable, "[%s]\n", *item_in_slot_2->m_alt_section_id);
                ImGui::LogText(selectable);
                xr_sprintf(selectable, "scope_x = %d\n", new_pos[0][0].x);
                ImGui::LogText(selectable);
                xr_sprintf(selectable, "scope_y = %d\n", new_pos[0][0].y);
                ImGui::LogText(selectable);
                xr_sprintf(selectable, "silencer_x = %d\n", new_pos[0][1].x);
                ImGui::LogText(selectable);
                xr_sprintf(selectable, "silencer_y = %d\n", new_pos[0][1].y);
                ImGui::LogText(selectable);
                xr_sprintf(selectable, "grenade_launcher_x = %d\n", new_pos[0][2].x);
                ImGui::LogText(selectable);
                xr_sprintf(selectable, "grenade_launcher_y = %d\n", new_pos[0][2].y);
                ImGui::LogText(selectable);
                ImGui::LogFinish();
            }
            if (ImGui::Button("Reset to default values"))
                ResetToDefaultWpn1Values();

            CWeapon* wpn = smart_cast<CWeapon*>(item_in_slot_2);

            if (wpn)
                wpn->b_forceIconUpdate = true;
        }
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("Weapon 2", ImVec2(ImGui::GetWindowWidth() / 2 - 8, ImGui::GetContentRegionAvail().y), true, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text(item2Sect);
        if (item_in_slot_3)
        {
            ImGui::NewLine();

            ImGui::SliderInt("Position step", &item_2_delta_pos, 0, 10, "%d");

            ImGui::NewLine();
            
            ImGui::DragInt2(addons_pos[SCOPE_POS], (int*)&new_pos[1][0], (float)item_2_delta_pos, min_v, max_v, "%d");
            ImGui::DragInt2(addons_pos[SIL_POS], (int*)&new_pos[1][1], (float)item_2_delta_pos, min_v, max_v, "%d");
            ImGui::DragInt2(addons_pos[GL_POS], (int*)&new_pos[1][2], (float)item_2_delta_pos, min_v, max_v, "%d");

            ImGui::NewLine();

            if (ImGui::Button("Copy values to clipboard"))
            {
                string128 selectable;

                ImGui::LogToClipboard();
                xr_sprintf(selectable, "[%s]\n", *item_in_slot_3->m_alt_section_id);
                ImGui::LogText(selectable);
                xr_sprintf(selectable, "scope_x = %d\n", new_pos[1][0].x);
                ImGui::LogText(selectable);
                xr_sprintf(selectable, "scope_y = %d\n", new_pos[1][0].y);
                ImGui::LogText(selectable);
                xr_sprintf(selectable, "silencer_x = %d\n", new_pos[1][1].x);
                ImGui::LogText(selectable);
                xr_sprintf(selectable, "silencer_y = %d\n", new_pos[1][1].y);
                ImGui::LogText(selectable);
                xr_sprintf(selectable, "grenade_launcher_x = %d\n", new_pos[1][2].x);
                ImGui::LogText(selectable);
                xr_sprintf(selectable, "grenade_launcher_y = %d\n", new_pos[1][2].y);
                ImGui::LogText(selectable);
                ImGui::LogFinish();
            }
            if (ImGui::Button("Reset to default values"))
                ResetToDefaultWpn2Values();

            CWeapon* wpn = smart_cast<CWeapon*>(item_in_slot_3);

            if (wpn)
                wpn->b_forceIconUpdate = true;
        }
        ImGui::EndChild();

        UpdateValues();

    }
    ImGui::End();
#endif
}
