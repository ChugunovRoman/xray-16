#pragma once

class CUITexturesAddonsPosition final : public xray::editor::ide_tool
{
public:
    CUITexturesAddonsPosition();
    void OnFrame() override;
    bool is_active() const override;

private:
    pcstr tool_name() override { return "Texture position addons"; }

    void ResetToDefaultWpn1Values();
    void ResetToDefaultWpn2Values();
    void UpdateValues();

    enum hud_adj_mode_keys
    {
        SCOPE_POS = 0,
        SIL_POS,
        GL_POS,
    };

    xr_map<hud_adj_mode_keys, pcstr> addons_pos =
    {
        { SCOPE_POS, "Scope Position (X, Y)" },
        { SIL_POS, "Silencer Position (X, Y)" },
        { GL_POS, "Grenade Launcher Position (X, Y)" },
    };


    bool paused{};

    int min_v{ -500 };
    int max_v{ 500 };

    int item_1_delta_pos{ 1 };
    int item_2_delta_pos{ 1 };

    Ivector2 pos[2][3]; // weapon1,weapon2 | scope,silencer,gl
    Ivector2 new_pos[2][3]; // weapon1,weapon2 | scope,silencer,gl

    pcstr curr_wpn_1;
    pcstr curr_wpn_2;
};
