#include "pch.hpp"

#include "ui_debug.h"
#include "ui_base.h"

CUIDebuggable::~CUIDebuggable()
{
    if (GEnv.UI)
    {
        if (UI().Debugger().GetSelected() == this)
            UI().Debugger().SetSelected(nullptr);
    }
}

void CUIDebuggable::RegisterDebuggable()
{
    UI().Debugger().Register(this);
}

void CUIDebuggable::UnregisterDebuggable()
{
    UI().Debugger().Unregister(this);
}

void CUIDebugger::Register(CUIDebuggable* debuggable)
{
    if (UiDebuggerEnabled)
        m_root_windows.emplace_back(debuggable);
}

void CUIDebugger::Unregister(CUIDebuggable* debuggable)
{
    if (UiDebuggerEnabled)
    {
        const auto it = std::find(m_root_windows.begin(), m_root_windows.end(), debuggable);
        if (it != m_root_windows.end())
            m_root_windows.erase(it);
    }
}

void CUIDebugger::SetSelected(CUIDebuggable* debuggable)
{
    m_state.selected = debuggable;
    m_state.newSelected = debuggable;
}

CUIDebugger::CUIDebugger()
{
    ImGui::SetAllocatorFunctions(
        [](size_t size, void* /*user_data*/)
        {
        return xr_malloc(size);
        },
        [](void* ptr, void* /*user_data*/)
        {
        xr_free(ptr);
        }
    );
    ImGui::SetCurrentContext(Device.GetImGuiContext());
}

void CUIDebugger::OnFrame()
{
    if (!UiDebuggerEnabled)
        return;
    if (!get_open_state())
        return;

    if (ImGui::Begin(tool_name(), &get_open_state(), get_default_window_flags()))
    {
        if (ImGui::BeginMenuBar())
        {
            ImGui::Checkbox("Draw rects", &m_state.drawWndRects);

            ImGui::BeginDisabled(!m_state.drawWndRects);
            ImGui::Checkbox("Coloured rects", &m_state.coloredRects);
            ImGui::EndDisabled();

            ImGui::EndMenuBar();
        }

        ImGui::BeginChild("UI tree", ImVec2(ImGui::GetWindowWidth() - 400, ImGui::GetContentRegionAvail().y), true, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_AlwaysVerticalScrollbar);
        ImGui::Text("UI tree");
        for (const auto& window : m_root_windows)
        {
            window->FillDebugTree(m_state);
            if (m_state.selected != m_state.newSelected)
                m_state.selected = m_state.newSelected;
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("UI properties", ImVec2(0, ImGui::GetContentRegionAvail().y), true, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("UI properties");
        if (m_state.selected)
            m_state.selected->FillDebugInfo();
        ImGui::EndChild();
    }
    ImGui::End();
}
