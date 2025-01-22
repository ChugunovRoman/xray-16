#pragma once

class CUICamAnmTool final : public xray::editor::ide_tool
{
public:
    CUICamAnmTool();
    void on_tool_frame() override;
    bool is_active() const override;

private:
    pcstr tool_name() const override { return "Camera Animation list"; }

    xr_vector<shared_str> list_anm;
    xr_vector<shared_str> list_ppe;

    bool paused{};
    bool cyclic{};
};
