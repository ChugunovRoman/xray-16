#pragma once

#include "ui/UIMapWnd.h"

class CUILevelMap;

class CPdaMapEditor final : public xray::editor::ide_tool
{
public:
    CPdaMapEditor();
    void on_tool_frame() override;
    bool is_active() const override;

private:
    pcstr tool_name() const override { return "PDA Map Editor"; }

    void SyncSelection(CUIMapWnd* mapWnd);
    CUILevelMap* ResolveSelectedLevelMap(CUIMapWnd* mapWnd) const;
    void CaptureOriginalState(CUILevelMap* levelMap);
    void ApplyPreviewState(CUIMapWnd* mapWnd, CUILevelMap* levelMap);
    void RestoreOriginalState(CUIMapWnd* mapWnd, CUILevelMap* levelMap);
    bool DrawLevelSelectionUi(CUIMapWnd* mapWnd);
    void HighlightSelection(CUIMapWnd* mapWnd, CUILevelMap* selectedMap);
    bool SelectedTextureExists(CUILevelMap* levelMap) const;
    void ApplyGlobalMapTextureSwap(CUIMapWnd* mapWnd);

private:
    bool paused{};
    bool m_swapGlobalMapTextures{};
    u32 m_lastGlobalMapSwapFrame{};
    EPdaMapLayer m_selectedLayer{ EPdaMapLayer::Surface };
    shared_str m_selectedLevelName;
    shared_str m_lastSelectionKey;
    Frect m_originalRect{};
    Frect m_previewRect{};
    float m_originalHeadingDegrees{};
    float m_previewHeadingDegrees{};
    Fvector2 m_originalTextureOffset{};
    Fvector2 m_previewTextureOffset{};
    Fvector2 m_originalTextureScale{};
    Fvector2 m_previewTextureScale{};
    bool m_hasCapturedState{};
    float m_moveStep{ 1.0f };
    float m_sizeStep{ 1.0f };
    float m_headingStep{ 1.0f };
};
