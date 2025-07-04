#pragma once

#include "player_hud.h"

class CHudTuner final : public xray::editor::ide_tool
{
public:
    CHudTuner();
    void on_tool_frame() override;
    bool is_active() const override;

private:
    pcstr tool_name() const override { return "Hud Tuner"; }

    void ResetToDefaultValues();
    void UpdateValues();

    enum hud_adj_mode_keys
    {
        HUD_POS = 0,
        HUD_ROT,
        HUD_POS_AIM,
        HUD_ROT_AIM,
        HUD_POS_ALT_AIM,
        HUD_ROT_ALT_AIM,
        HUD_POS_COR_AIM,
        HUD_ROT_COR_AIM,
        HUD_POS_GL,
        HUD_ROT_GL,
        ITEM_WRD_POS,
        ITEM_POS,
        ITEM_ROT,
        FIRE_POINT,
        FIRE_POINT_2,
        SHELL_POINT,
        ARTEFACT_POINT_POS,
        ARTEFACT_POINT_ROT,
    };
    enum hud_item_idx
    {
        MAIN_ITEM = 0,
        OFFHAND_ITEM,
    };
    xr_map<hud_item_idx, pcstr> hud_item_mode
    {
        { MAIN_ITEM, "Main hand item" },
        { OFFHAND_ITEM, "Off hand item" },
    };
    xr_map<hud_adj_mode_keys, pcstr> hud_adj_modes =
    {
        { HUD_POS, "Hud Position (Default)" },
        { HUD_ROT, "Hud Rotation (Default)" },
        { HUD_POS_AIM, "Hud Position (Aiming)" },
        { HUD_ROT_AIM, "Hud Rotation (Aiming)" },
        { HUD_POS_ALT_AIM, "Hud Position (Second Aiming)" },
        { HUD_ROT_ALT_AIM, "Hud Rotation (Second Aiming)" },
        { HUD_POS_COR_AIM, "Hud Position (Correction Aim)" },
        { HUD_ROT_COR_AIM, "Hud Rotation (Correction Aim)" },
        { HUD_POS_GL, "Hud Position (GL)" },
        { HUD_ROT_GL, "Hud Rotation (GL)" },
        { ITEM_WRD_POS, "Addons World Position" },
        { ITEM_POS, "Item Position" },
        { ITEM_ROT, "Item Rotation" },
        { FIRE_POINT, "Fire Point" },
        { FIRE_POINT_2, "Fire Point 2" },
        { SHELL_POINT, "Shell Point" },
        { ARTEFACT_POINT_POS, "Artefact Dot Position" },
        { ARTEFACT_POINT_ROT, "Artefact Dot Rotation" },
    };

    bool paused{};
    bool draw_fp{};
    bool draw_fp2{};
    bool draw_fd{};
    bool draw_fd2{};
    bool draw_sp{};
    bool draw_bones{};
    bool draw_bones_addon{};
    bool draw_center{};

    float debug_point_size{ 0.005f };
    float _delta_pos{ 0.0005f };
    float _delta_rot{ 0.0005f };

    shared_str current_section{};
    attachable_hud_item* current_hud_item{};
    hud_item_idx current_hud_idx{ MAIN_ITEM };

    hud_item_measures curr_measures{};
    hud_item_measures new_measures{};

    Fvector m_hands_curr_offset[2][1]; // pos,rot/ alt_aim
    Fvector m_hands_new_offset[2][1]; // pos,rot/ alt_aim

    Fvector pos{};
    Fvector world_addons_pos{};

    Fvector m_artefact_map_p{0.0f,0.0f,0.0f};
    Fvector m_artefact_map_r{0.0f,0.0f,0.0f};

    Fvector dbg_center{0.0f,0.0f,0.0f};
    Fvector dbg_wpn_scope_pos{0.0f,0.0f,0.0f};

    struct SlotTransform
    {
        u16 type;
        Fvector pos;
        Fvector rot;
    };
    
    xr_map<shared_str, SlotTransform> m_weapon_slots;
};
