#include "StdAfx.h"
#include "Level.h"
#include "xrEngine/xr_input.h"
#include "xrEngine/Effector.h"
#include "xrEngine/CameraManager.h"
#include "xrEngine/FDemoRecord.h"
#include "xrEngine/GameFont.h"
#include "ActorEffector.h"
#include "PostprocessAnimator.h"
#include "cam_anm_tool.h"

CUICamAnmTool::CUICamAnmTool()
{
    ImGui::SetCurrentContext(Device.GetImGuiContext());
    paused = fsimilar(Device.time_factor(), EPS);
}

bool CUICamAnmTool::is_active() const
{
    return is_open() && Device.editor().IsActiveState();
}

void CUICamAnmTool::OnFrame()
{
#ifndef MASTER_GOLD
    if (!get_open_state())
        return;

    if (!g_actor)
        return;

    auto calcColumnCount = [](float columnWidth) -> int
    {
        float windowWidth = ImGui::GetWindowWidth();
        int columnCount = _max(1, static_cast<int>(windowWidth / columnWidth));
        return columnCount;
    };

    if (list_anm.size() == 0)
    {
        FS_FileSet fset;
        FS.file_list(fset, "$game_anims$", FS_ListFiles, "*.anm*");

        if (fset.size())
            for (FS_FileSet::iterator it = fset.begin(); it != fset.end(); it++)
            list_anm.push_back((*it).name.c_str());
    }
    if (list_ppe.size() == 0)
    {
        FS_FileSet fset;
        FS.file_list(fset, "$game_anims$", FS_ListFiles, "*.ppe*");

        if (fset.size())
            for (FS_FileSet::iterator it = fset.begin(); it != fset.end(); it++)
            list_ppe.push_back((*it).name.c_str());
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
            if (ImGui::RadioButton("Cyclic", cyclic))
                cyclic = !cyclic;

            ImGui::EndMenuBar();
        }

        ImGui::NewLine();
        if (ImGui::CollapsingHeader("Animations", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::BeginTable("Animations", calcColumnCount(255.f)))
            {
                if (list_anm.size())
                {
                    for (auto it = list_anm.begin(); it != list_anm.end(); it++)
                    {
                        ImGui::TableNextColumn();
                        if (ImGui::Button((*it).c_str()))
                        {
                            CAnimatorCamEffectorScriptCB* e = xr_new<CAnimatorCamEffectorScriptCB>("");
                            e->SetType(ECamEffectorType::cefAnsel);
                            e->SetCyclic(cyclic);
                            e->Start((*it).c_str());
                            Actor()->Cameras().AddCamEffector(e);
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        {
                            ImGui::SetTooltip("%s", (*it).c_str());
                            if (ImGui::IsMouseDown(ImGuiMouseButton_Right))
                            {
                                string128 selectable;
                                string128 p_dir;
                                string128 p_name;
                                string128 name;
                                _splitpath((*it).c_str(), nullptr, p_dir, p_name, nullptr);
                                strconcat(sizeof(name), name, p_dir, p_name);
                                ImGui::LogToClipboard();
                                xr_sprintf(selectable, "%s", name);
                                ImGui::LogText(selectable);
                                ImGui::LogFinish();
                            }
                        }
                    }
                }

                ImGui::EndTable();
            }
        }

        ImGui::NewLine();
        if (ImGui::CollapsingHeader("PPE Effects", ImGuiTreeNodeFlags_DefaultOpen))
            if (ImGui::BeginTable("PPE Effects", calcColumnCount(255.f)))
            {
                if (list_ppe.size())
                {
                    for (auto it = list_ppe.begin(); it != list_ppe.end(); it++)
                    {
                        ImGui::TableNextColumn();
                        if (ImGui::Button((*it).c_str()))
                        {
                            CPostprocessAnimator* pp = xr_new<CPostprocessAnimator>(EEffectorPPType::ppeNext, cyclic);
                            pp->Load((*it).c_str());
                            Actor()->Cameras().AddPPEffector(pp);
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        {
                            ImGui::SetTooltip("%s", (*it).c_str());
                        }
                    }
                }

                ImGui::EndTable();
            }
    }
    ImGui::End();
#endif
}
