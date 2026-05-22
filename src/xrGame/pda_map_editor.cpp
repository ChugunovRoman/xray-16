#include "StdAfx.h"

#include "pda_map_editor.h"

#include "ui/UIMap.h"
#include "xrEngine/editor_base.h"
#include "xrUICore/ui_base.h"

CUIMapWnd* GetMapWnd();

namespace
{
bool is_empty_cstr(cpcstr value) { return !value || !value[0]; }

enum class EEditorRectRotation
{
    None,
    Deg90,
    DegMinus90,
    Deg180,
    Other
};

EEditorRectRotation get_editor_rect_rotation(float degrees)
{
    if (fsimilar(degrees, 0.0f, 0.1f))
        return EEditorRectRotation::None;
    if (fsimilar(degrees, 90.0f, 0.1f))
        return EEditorRectRotation::Deg90;
    if (fsimilar(degrees, -90.0f, 0.1f))
        return EEditorRectRotation::DegMinus90;
    if (fsimilar(_abs(degrees), 180.0f, 0.1f))
        return EEditorRectRotation::Deg180;
    return EEditorRectRotation::Other;
}

Frect make_editor_rect(const Frect& runtimeRect, float headingDegrees)
{
    const auto rotation = get_editor_rect_rotation(headingDegrees);
    if (rotation == EEditorRectRotation::Deg90 || rotation == EEditorRectRotation::DegMinus90)
    {
        Frect rect;
        rect.set(runtimeRect.x1, runtimeRect.y1, runtimeRect.x1 + runtimeRect.height(), runtimeRect.y1 + runtimeRect.width());
        return rect;
    }

    return runtimeRect;
}

Frect make_runtime_rect(const Frect& editorRect, float headingDegrees)
{
    const auto rotation = get_editor_rect_rotation(headingDegrees);
    if (rotation == EEditorRectRotation::Deg90 || rotation == EEditorRectRotation::DegMinus90)
    {
        Frect rect;
        rect.set(editorRect.x1, editorRect.y1, editorRect.x1 + editorRect.height(), editorRect.y1 + editorRect.width());
        return rect;
    }

    return editorRect;
}

pcstr get_editor_layer_name(EPdaMapLayer layer)
{
    switch (layer)
    {
    case EPdaMapLayer::Surface: return "surface";
    case EPdaMapLayer::Underground: return "underground";
    default: return "unknown";
    }
}

const char* get_editor_layer_caption(EPdaMapLayer layer)
{
    return layer == EPdaMapLayer::Underground ? "Underground" : "Surface";
}
} // namespace

CPdaMapEditor::CPdaMapEditor()
{
    ImGui::SetCurrentContext(Device.GetImGuiContext());
    paused = fsimilar(Device.time_factor(), EPS);
}

bool CPdaMapEditor::is_active() const
{
    return is_open() && Device.editor().IsActiveState();
}

void CPdaMapEditor::SyncSelection(CUIMapWnd* mapWnd)
{
    if (!mapWnd)
        return;

    const GAME_MAPS& maps = mapWnd->GetMapsForLayer(m_selectedLayer);
    if (maps.empty())
    {
        m_selectedLevelName = "";
        m_lastSelectionKey = "";
        m_hasCapturedState = false;
        return;
    }

    if (is_empty_cstr(m_selectedLevelName.c_str()) || maps.find(m_selectedLevelName) == maps.end())
        m_selectedLevelName = maps.begin()->first;

    string256 selectionKey;
    xr_sprintf(selectionKey, "%s:%s", get_editor_layer_name(m_selectedLayer), m_selectedLevelName.c_str());
    if (is_empty_cstr(m_lastSelectionKey.c_str()) || xr_stricmp(m_lastSelectionKey.c_str(), selectionKey) != 0)
    {
        m_lastSelectionKey = selectionKey;
        if (CUILevelMap* levelMap = ResolveSelectedLevelMap(mapWnd))
            CaptureOriginalState(levelMap);
    }
}

CUILevelMap* CPdaMapEditor::ResolveSelectedLevelMap(CUIMapWnd* mapWnd) const
{
    if (!mapWnd || is_empty_cstr(m_selectedLevelName.c_str()))
        return nullptr;

    EPdaMapLayer levelLayer{};
    return smart_cast<CUILevelMap*>(mapWnd->GetLevelMap(m_selectedLevelName, &levelLayer));
}

void CPdaMapEditor::CaptureOriginalState(CUILevelMap* levelMap)
{
    if (!levelMap)
    {
        m_hasCapturedState = false;
        return;
    }

    m_originalRect = levelMap->GlobalRect();
    m_previewRect = m_originalRect;
    m_originalHeadingDegrees = levelMap->GetPdaMapHeadingDegrees();
    m_previewHeadingDegrees = m_originalHeadingDegrees;
    m_originalTextureOffset = levelMap->GetPdaMapTextureOffset();
    m_previewTextureOffset = m_originalTextureOffset;
    m_originalTextureScale = levelMap->GetPdaMapTextureScale();
    m_previewTextureScale = m_originalTextureScale;
    m_hasCapturedState = true;
}

void CPdaMapEditor::ApplyPreviewState(CUIMapWnd* mapWnd, CUILevelMap* levelMap)
{
    if (!mapWnd || !levelMap)
        return;

    if (mapWnd->IsShown())
        mapWnd->ActivateLayer(m_selectedLayer);
    levelMap->SetGlobalRect(m_previewRect);
    levelMap->SetPdaMapHeadingDegrees(m_previewHeadingDegrees);
    levelMap->SetPdaMapTextureOffset(m_previewTextureOffset);
    levelMap->SetPdaMapTextureScale(m_previewTextureScale);

    if (mapWnd->IsShown())
    {
        if (CUIGlobalMap* globalMap = mapWnd->GetGlobalMapForLayer(m_selectedLayer))
            globalMap->Update();
        levelMap->Update();
    }
}

void CPdaMapEditor::RestoreOriginalState(CUIMapWnd* mapWnd, CUILevelMap* levelMap)
{
    if (!mapWnd || !levelMap || !m_hasCapturedState)
        return;

    m_previewRect = m_originalRect;
    m_previewHeadingDegrees = m_originalHeadingDegrees;
    m_previewTextureOffset = m_originalTextureOffset;
    m_previewTextureScale = m_originalTextureScale;
    ApplyPreviewState(mapWnd, levelMap);
}

bool CPdaMapEditor::DrawLevelSelectionUi(CUIMapWnd* mapWnd)
{
    bool selectionChanged = false;

    if (ImGui::BeginCombo("Layer", get_editor_layer_caption(m_selectedLayer)))
    {
        const bool isSurface = m_selectedLayer == EPdaMapLayer::Surface;
        const bool isUnderground = m_selectedLayer == EPdaMapLayer::Underground;

        if (ImGui::Selectable("Surface", isSurface))
        {
            m_selectedLayer = EPdaMapLayer::Surface;
            SyncSelection(mapWnd);
            selectionChanged = true;
        }
        if (ImGui::Selectable("Underground", isUnderground))
        {
            m_selectedLayer = EPdaMapLayer::Underground;
            SyncSelection(mapWnd);
            selectionChanged = true;
        }

        ImGui::EndCombo();
    }

    const GAME_MAPS& maps = mapWnd->GetMapsForLayer(m_selectedLayer);
    const char* previewName = is_empty_cstr(m_selectedLevelName.c_str()) ? "<none>" : m_selectedLevelName.c_str();
    if (ImGui::BeginCombo("Level", previewName))
    {
        for (auto it = maps.begin(), itEnd = maps.end(); it != itEnd; ++it)
        {
            const bool selected = xr_stricmp(m_selectedLevelName.c_str(), it->first.c_str()) == 0;
            if (ImGui::Selectable(it->first.c_str(), selected))
            {
                m_selectedLevelName = it->first;
                SyncSelection(mapWnd);
                selectionChanged = true;
            }
        }
        ImGui::EndCombo();
    }

    return selectionChanged;
}

void CPdaMapEditor::HighlightSelection(CUIMapWnd* mapWnd, CUILevelMap* selectedMap)
{
    if (!mapWnd)
        return;

    for (size_t idx = 0; idx < PDA_MAP_LAYER_COUNT; ++idx)
    {
        const auto layer = static_cast<EPdaMapLayer>(idx);
        const GAME_MAPS& maps = mapWnd->GetMapsForLayer(layer);
        for (auto it = maps.begin(), itEnd = maps.end(); it != itEnd; ++it)
        {
            CUILevelMap* levelMap = smart_cast<CUILevelMap*>(it->second);
            if (!levelMap)
                continue;

            levelMap->SetTextureColor(levelMap == selectedMap ? color_rgba(255, 220, 120, 255) : color_rgba(255, 255, 255, 255));
        }
    }
}

bool CPdaMapEditor::SelectedTextureExists(CUILevelMap* levelMap) const
{
    if (!levelMap || is_empty_cstr(levelMap->m_texture.c_str()))
        return false;

    string_path texturePath;
    return !!FS.exist(texturePath, "$game_textures$", levelMap->m_texture.c_str(), ".dds");
}

void CPdaMapEditor::ApplyGlobalMapTextureSwap(CUIMapWnd* mapWnd)
{
    if (!mapWnd)
        return;

    auto* surfaceGlobal = mapWnd->GetGlobalMapForLayer(EPdaMapLayer::Surface);
    auto* undergroundGlobal = mapWnd->GetGlobalMapForLayer(EPdaMapLayer::Underground);
    if (!surfaceGlobal || !undergroundGlobal)
        return;

    const pcstr surfaceTexture = m_swapGlobalMapTextures ? undergroundGlobal->m_texture.c_str() : surfaceGlobal->m_texture.c_str();
    const pcstr undergroundTexture = m_swapGlobalMapTextures ? surfaceGlobal->m_texture.c_str() : undergroundGlobal->m_texture.c_str();

    surfaceGlobal->InitTextureEx(surfaceTexture, surfaceGlobal->m_shader_name.c_str());
    undergroundGlobal->InitTextureEx(undergroundTexture, undergroundGlobal->m_shader_name.c_str());

    surfaceGlobal->Update();
    undergroundGlobal->Update();
    m_lastGlobalMapSwapFrame = Device.dwFrame;
}

void CPdaMapEditor::on_tool_frame()
{
    if (!get_open_state())
    {
        if (CUIMapWnd* mapWnd = GetMapWnd())
        {
            if (m_swapGlobalMapTextures)
            {
                m_swapGlobalMapTextures = false;
                ApplyGlobalMapTextureSwap(mapWnd);
            }
            HighlightSelection(mapWnd, nullptr);
        }
        return;
    }

    CUIMapWnd* mapWnd = GetMapWnd();
    if (ImGui::Begin(tool_name(), &get_open_state(), get_default_window_flags()))
    {
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::RadioButton("Pause", paused))
            {
                paused = !paused;
                Device.time_factor(paused ? EPS : 1.0f);
            }
            ImGui::EndMenuBar();
        }

        if (!mapWnd)
        {
            ImGui::TextUnformatted("Open the PDA map first.");
            ImGui::End();
            return;
        }

        if (m_swapGlobalMapTextures && Device.dwFrame - m_lastGlobalMapSwapFrame >= 10)
            ApplyGlobalMapTextureSwap(mapWnd);

        SyncSelection(mapWnd);

        if (is_empty_cstr(m_selectedLevelName.c_str()))
        {
            ImGui::TextUnformatted("No level maps found for the selected layer.");
            ImGui::End();
            return;
        }

        const bool selectionChanged = DrawLevelSelectionUi(mapWnd);

        CUILevelMap* levelMap = ResolveSelectedLevelMap(mapWnd);
        HighlightSelection(mapWnd, levelMap);

        if (!levelMap)
        {
            ImGui::TextUnformatted("Selected map is unavailable in the current runtime state.");
            ImGui::End();
            return;
        }

        ApplyPreviewState(mapWnd, levelMap);

        if (selectionChanged && mapWnd->IsShown())
            mapWnd->SetTargetMap(levelMap, false);

        ImGui::Separator();
        ImGui::Text("Active layer: %s", get_editor_layer_caption(mapWnd->ActiveLayer()));
        ImGui::Text("Global textures swapped: %s", m_swapGlobalMapTextures ? "yes" : "no");
        ImGui::Text("Texture: %s", levelMap->m_texture.c_str());
        ImGui::Text("Texture status: %s", SelectedTextureExists(levelMap) ? "ok" : "missing or placeholder");
        ImGui::Text("Bound rect: %.2f, %.2f, %.2f, %.2f", levelMap->BoundRect().x1, levelMap->BoundRect().y1, levelMap->BoundRect().x2,
            levelMap->BoundRect().y2);
        ImGui::Text("Runtime global rect: %.2f, %.2f, %.2f, %.2f", m_previewRect.x1, m_previewRect.y1, m_previewRect.x2, m_previewRect.y2);

        const float configKx = UI().get_current_kx();
        ImGui::Text("Config global rect: %.2f, %.2f, %.2f, %.2f", m_previewRect.x1 / configKx, m_previewRect.y1, m_previewRect.x2 / configKx,
            m_previewRect.y2);
        ImGui::Text("Runtime texture offset: %.2f, %.2f", m_previewTextureOffset.x, m_previewTextureOffset.y);
        ImGui::Text("Config texture offset: %.2f, %.2f", m_previewTextureOffset.x / configKx, m_previewTextureOffset.y);
        ImGui::Text("Texture scale: %.3f, %.3f", m_previewTextureScale.x, m_previewTextureScale.y);

        ImGui::Separator();
        ImGui::DragFloat("Move step", &m_moveStep, 0.1f, 0.1f, 100.0f, "%.2f");
        ImGui::DragFloat("Size step", &m_sizeStep, 0.1f, 0.1f, 100.0f, "%.2f");
        ImGui::DragFloat("Heading step", &m_headingStep, 0.1f, 0.1f, 90.0f, "%.2f");

        Frect editorRect = make_editor_rect(m_previewRect, m_previewHeadingDegrees);
        if (ImGui::DragFloat4("global_rect", &editorRect.x1, 0.1f, 0.0f, 0.0f, "%.2f"))
            m_previewRect = make_runtime_rect(editorRect, m_previewHeadingDegrees);
        ImGui::DragFloat("pda_map_heading", &m_previewHeadingDegrees, m_headingStep, -360.0f, 360.0f, "%.2f");
        ImGui::DragFloat2("pda_map_texture_offset", (float*)&m_previewTextureOffset, 0.1f, 0.0f, 0.0f, "%.2f");
        ImGui::DragFloat2("pda_map_texture_scale", (float*)&m_previewTextureScale, 0.005f, 0.01f, 10.0f, "%.3f");
        m_previewTextureScale.x = _max(m_previewTextureScale.x, 0.01f);
        m_previewTextureScale.y = _max(m_previewTextureScale.y, 0.01f);

        if (ImGui::Button("Move Left"))
        {
            editorRect.add(-m_moveStep, 0.0f);
            m_previewRect = make_runtime_rect(editorRect, m_previewHeadingDegrees);
        }
        ImGui::SameLine();
        if (ImGui::Button("Move Right"))
        {
            editorRect.add(m_moveStep, 0.0f);
            m_previewRect = make_runtime_rect(editorRect, m_previewHeadingDegrees);
        }
        ImGui::SameLine();
        if (ImGui::Button("Move Up"))
        {
            editorRect.add(0.0f, -m_moveStep);
            m_previewRect = make_runtime_rect(editorRect, m_previewHeadingDegrees);
        }
        ImGui::SameLine();
        if (ImGui::Button("Move Down"))
        {
            editorRect.add(0.0f, m_moveStep);
            m_previewRect = make_runtime_rect(editorRect, m_previewHeadingDegrees);
        }

        if (ImGui::Button("Wider"))
        {
            editorRect.rb.x += m_sizeStep;
            m_previewRect = make_runtime_rect(editorRect, m_previewHeadingDegrees);
        }
        ImGui::SameLine();
        if (ImGui::Button("Narrower"))
        {
            editorRect.rb.x = _max(editorRect.x1 + 1.0f, editorRect.rb.x - m_sizeStep);
            m_previewRect = make_runtime_rect(editorRect, m_previewHeadingDegrees);
        }
        ImGui::SameLine();
        if (ImGui::Button("Taller"))
        {
            editorRect.rb.y += m_sizeStep;
            m_previewRect = make_runtime_rect(editorRect, m_previewHeadingDegrees);
        }
        ImGui::SameLine();
        if (ImGui::Button("Shorter"))
        {
            editorRect.rb.y = _max(editorRect.y1 + 1.0f, editorRect.rb.y - m_sizeStep);
            m_previewRect = make_runtime_rect(editorRect, m_previewHeadingDegrees);
        }

        if (ImGui::Button("0 deg"))
            m_previewHeadingDegrees = 0.0f;
        ImGui::SameLine();
        if (ImGui::Button("90 deg"))
            m_previewHeadingDegrees = 90.0f;
        ImGui::SameLine();
        if (ImGui::Button("-90 deg"))
            m_previewHeadingDegrees = -90.0f;
        ImGui::SameLine();
        if (ImGui::Button("180 deg"))
            m_previewHeadingDegrees = 180.0f;

        if (ImGui::Button("Tex Left"))
            m_previewTextureOffset.x -= m_moveStep;
        ImGui::SameLine();
        if (ImGui::Button("Tex Right"))
            m_previewTextureOffset.x += m_moveStep;
        ImGui::SameLine();
        if (ImGui::Button("Tex Up"))
            m_previewTextureOffset.y -= m_moveStep;
        ImGui::SameLine();
        if (ImGui::Button("Tex Down"))
            m_previewTextureOffset.y += m_moveStep;

        if (ImGui::Button("Tex Wider"))
            m_previewTextureScale.x += m_sizeStep * 0.01f;
        ImGui::SameLine();
        if (ImGui::Button("Tex Narrower"))
            m_previewTextureScale.x = _max(m_previewTextureScale.x - m_sizeStep * 0.01f, 0.01f);
        ImGui::SameLine();
        if (ImGui::Button("Tex Taller"))
            m_previewTextureScale.y += m_sizeStep * 0.01f;
        ImGui::SameLine();
        if (ImGui::Button("Tex Shorter"))
            m_previewTextureScale.y = _max(m_previewTextureScale.y - m_sizeStep * 0.01f, 0.01f);

        if (ImGui::Button("Switch global map textures"))
        {
            m_swapGlobalMapTextures = !m_swapGlobalMapTextures;
            ApplyGlobalMapTextureSwap(mapWnd);
        }

        if (ImGui::Button("Reset to original"))
            RestoreOriginalState(mapWnd, levelMap);

        ImGui::SameLine();
        if (ImGui::Button("Focus selection") && mapWnd->IsShown())
            mapWnd->SetTargetMap(levelMap, false);

        ImGui::SameLine();
        if (ImGui::Button("Copy block to clipboard"))
        {
            string1024 buffer;
            xr_sprintf(buffer,
                "[%s]\n"
                "global_rect = %.2f, %.2f, %.2f, %.2f\n"
                "pda_map_heading = %.2f\n"
                "pda_map_texture_offset = %.2f, %.2f\n"
                "pda_map_texture_scale = %.3f, %.3f\n",
                levelMap->MapName().c_str(), m_previewRect.x1 / configKx, m_previewRect.y1, m_previewRect.x2 / configKx, m_previewRect.y2,
                m_previewHeadingDegrees, m_previewTextureOffset.x / configKx, m_previewTextureOffset.y, m_previewTextureScale.x,
                m_previewTextureScale.y);
            ImGui::SetClipboardText(buffer);
        }
    }
    ImGui::End();
}
