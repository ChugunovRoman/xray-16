#pragma once

class CUICamAnmTool final : public xray::editor::ide_tool
{
public:
    CUICamAnmTool();
    void OnFrame() override;
    bool is_active() const override;

private:
    pcstr tool_name() override { return "Camera Animation list"; }

    xr_vector<shared_str> list_anm;
    xr_vector<shared_str> list_ppe;

    bool paused{};
    bool cyclic{};
};
