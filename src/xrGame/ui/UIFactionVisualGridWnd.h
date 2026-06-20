#pragma once

#include <functional>

#include "xrUICore/Windows/UIWindow.h"
#include "xrCommon/xr_set.h"

class CUIXml;
class CUIScrollBar;

class CUIFactionVisualGridWnd final : public CUIWindow
{
public:
    using inherited = CUIWindow;

    struct STabData
    {
        shared_str name;
        xr_vector<shared_str> models;
    };

    using TModelList = xr_vector<shared_str>;
    using TTabList = xr_vector<STabData>;
    using TTabMap = xr_map<shared_str, TModelList>;
    using TTabNameList = xr_vector<shared_str>;
    using TSelectionCallback = std::function<void(const shared_str& model_path, const shared_str& tab_name, u32 cell_index)>;

public:
    CUIFactionVisualGridWnd();
    ~CUIFactionVisualGridWnd() override;

    bool Init(pcstr xml_name, pcstr path);
    void InitFromXML(CUIXml& xml, pcstr path, int index);
    void Update() override;
    void Draw() override;
    void SendMessage(CUIWindow* pWnd, s16 msg, void* pData = nullptr) override;
    bool OnMouseAction(float x, float y, EUIMessages mouse_action) override;
    void OnMouseMove() override;
    void OnMouseScroll(float iDirection) override;
    bool OnMouseDown(int mouse_btn) override;
    bool OnDbClick() override;
    void OnFocusLost() override;
    void FillDebugInfo() override;

    void Clear();

    void SetGridDimensions(u32 columns, u32 rows);
    void SetCellSize(const Fvector2& size);
    void SetCellSpacing(const Fvector2& spacing);
    void SetGridPadding(const Fvector2& padding);
    void SetTabBarHeight(float height);
    void SetTabSpacing(float spacing);
    void SetPageOffset(u32 first_index);
    void SetPreviewAngles(float yaw_deg, float pitch_deg, float roll_deg);
    void SetPreviewOffset(const Fvector& offset);
    void SetPreviewCameraDistanceOffset(float offset);
    void SetPreviewFrameSize(const Fvector2& size);
    void SetPreviewPose(pcstr pose_name);
    void ClearHighlightedModels();
    void SetHighlightedModels(const TModelList& models);
    void SetPersistentHighlightColor(u32 color);

    [[nodiscard]] u32 GetGridColumns() const { return m_gridColumns; }
    [[nodiscard]] u32 GetGridRows() const { return m_gridRows; }
    [[nodiscard]] u32 GetVisibleCellCount() const { return m_gridColumns * m_gridRows; }
    [[nodiscard]] const Fvector2& GetCellSize() const { return m_cellSize; }
    [[nodiscard]] const Fvector2& GetCellSpacing() const { return m_cellSpacing; }
    [[nodiscard]] const Fvector2& GetGridPadding() const { return m_gridPadding; }
    [[nodiscard]] float GetTabBarHeight() const { return m_tabBarHeight; }
    [[nodiscard]] float GetTabSpacing() const { return m_tabSpacing; }
    [[nodiscard]] u32 GetPageOffset() const { return m_pageOffset; }
    [[nodiscard]] int GetHoveredCellIndex() const { return m_hoveredCellIndex; }
    [[nodiscard]] float GetPreviewYawDeg() const { return m_previewYawDeg; }
    [[nodiscard]] float GetPreviewPitchDeg() const { return m_previewPitchDeg; }
    [[nodiscard]] float GetPreviewRollDeg() const { return m_previewRollDeg; }
    [[nodiscard]] const Fvector& GetPreviewOffset() const { return m_previewOffset; }
    [[nodiscard]] float GetPreviewCameraDistanceOffset() const { return m_previewCameraDistanceOffset; }
    [[nodiscard]] const Fvector2& GetPreviewFrameSize() const { return m_previewFrameSize; }
    [[nodiscard]] pcstr GetPreviewPose() const { return m_previewPoseName.c_str(); }
    [[nodiscard]] bool IsPreviewCacheDisabled() const { return m_disablePreviewCache; }

    void AddModel(pcstr model_path);
    void AddModels(const TModelList& models);
    void AddModelToTab(pcstr tab_name, pcstr model_path);
    void AddModelsToTab(pcstr tab_name, const TModelList& models);

    void SetModels(const TModelList& models);
    void SetData(const TTabList& tabs);
    void SetData(const TTabMap& tabs);
    void SetTabs(const TTabMap& tabs) { SetData(tabs); }

    [[nodiscard]] const TModelList& GetAllModels() const { return m_allModels; }
    [[nodiscard]] const TTabList& GetTabGroups() const { return m_tabs; }
    [[nodiscard]] const TTabNameList& GetTabNames() const;
    [[nodiscard]] TTabNameList GetAllTabNames() const { return GetTabNames(); }
    [[nodiscard]] const TModelList& GetModelsInTab(pcstr tab_name) const;
    [[nodiscard]] u32 GetModelCount() const { return static_cast<u32>(m_allModels.size()); }
    [[nodiscard]] u32 GetModelCount(pcstr tab_name) const;
    [[nodiscard]] bool HasTab(pcstr tab_name) const;
    [[nodiscard]] bool HasModel(pcstr model_path) const;

    void SetActiveTab(pcstr tab_name);
    [[nodiscard]] pcstr GetActiveTab() const { return m_activeTab.c_str(); }
    [[nodiscard]] pcstr GetCurrentTab() const { return GetActiveTab(); }
    bool SetCurrentTab(pcstr tab_name);

    void SetSelectedModel(pcstr model_path, pcstr tab_name = nullptr, u32 cell_index = u32(-1));
    [[nodiscard]] pcstr GetSelectedModel() const { return m_selectedModel.c_str(); }
    [[nodiscard]] pcstr GetSelectedTab() const { return m_selectedTab.c_str(); }
    [[nodiscard]] u32 GetSelectedCellIndex() const { return m_selectedCellIndex; }

    void SetSelectionCallback(TSelectionCallback callback) { m_onSelection = std::move(callback); }
    void ClearSelectionCallback() { m_onSelection = nullptr; }

    pcstr GetDebugType() override { return "CUIFactionVisualGridWnd"; }

private:
    using TTabIterator = TTabList::iterator;
    using TConstTabIterator = TTabList::const_iterator;

    [[nodiscard]] static bool IsSyntheticAllTab(pcstr tab_name);
    [[nodiscard]] static pcstr NormalizeTabName(pcstr tab_name);

    [[nodiscard]] TTabIterator FindTab(pcstr tab_name);
    [[nodiscard]] TConstTabIterator FindTab(pcstr tab_name) const;
    [[nodiscard]] TTabIterator EnsureTab(pcstr tab_name);
    [[nodiscard]] static bool ContainsModel(const TModelList& models, const shared_str& model_path);
    [[nodiscard]] static u32 FindModelIndex(const TModelList& models, const shared_str& model_path);

    void AddUniqueToAll(const shared_str& model_path);
    void AddUniqueToTab(STabData& tab, const shared_str& model_path);
    void RebuildAllModels();
    void ResetSelection();
    void ResetHover();
    void ClampPageOffset();
    [[nodiscard]] u32 GetTabCount() const;
    [[nodiscard]] const TModelList& GetActiveModels() const;
    [[nodiscard]] pcstr GetModelDisplayName(pcstr model_path);
    [[nodiscard]] Frect GetTabRect(u32 tab_index) const;
    [[nodiscard]] Frect GetGridRect() const;
    [[nodiscard]] Frect GetCellRect(u32 visible_index) const;
    [[nodiscard]] int GetCellIndexAt(float x, float y) const;
    [[nodiscard]] int GetTabIndexAt(float x, float y) const;
    void SelectCell(u32 visible_index, bool fire_callback);
    void SelectTab(u32 tab_index);
    void SyncPreviewSettings(bool force = false);
    void WarmPreviewVisibleModels();
    void ReleaseNoCachePreviews();
    void RefreshNoCachePreview(pcstr model_path);
    [[nodiscard]] shared_str GetPreviewTextureForModel(pcstr model_path) const;
    [[nodiscard]] shared_str GetPreviewPoseSourceModel() const;
    void RefreshPreviewPoseCycleNames();
    [[nodiscard]] bool IsModelHighlighted(pcstr model_path) const;
    [[nodiscard]] u32 GetTotalRowCount() const;
    [[nodiscard]] u32 GetVisibleRowCount() const;
    [[nodiscard]] u32 GetRowOffset() const;
    [[nodiscard]] float GetContentHeight() const;
    [[nodiscard]] float GetViewportHeight() const;
    [[nodiscard]] int GetScrollStepPixels() const;
    [[nodiscard]] int GetScrollPixelOffset() const;
    void SetRowOffset(u32 row_offset);
    void UpdateScrollBar();

private:
    TTabList m_tabs;
    TModelList m_allModels;
    shared_str m_activeTab;
    shared_str m_selectedModel;
    shared_str m_selectedTab;
    u32 m_selectedCellIndex;
    TSelectionCallback m_onSelection;
    u32 m_gridColumns;
    u32 m_gridRows;
    Fvector2 m_cellSize;
    Fvector2 m_cellSpacing;
    Fvector2 m_gridPadding;
    float m_tabBarHeight;
    float m_tabSpacing;
    u32 m_pageOffset;
    int m_hoveredCellIndex;
    int m_hoveredTabIndex;
    float m_previewYawDeg{};
    float m_previewPitchDeg{};
    float m_previewRollDeg{};
    Fvector m_previewOffset{};
    float m_previewCameraDistanceOffset{};
    Fvector2 m_previewFrameSize{};
    shared_str m_previewPoseName;
    shared_str m_previewPoseCycleSourceModel;
    xr_vector<shared_str> m_previewPoseCycleNames;
    xr_set<shared_str> m_highlightedModels;
    mutable TTabNameList m_cachedTabNames;
    mutable bool m_tabNamesDirty{true};
    xr_map<shared_str, shared_str> m_noCacheModelTextures;
    u32 m_persistentHighlightColor{color_rgba(255, 220, 32, 140)};
    bool m_disablePreviewCache{};
    bool m_previewSettingsDirty{true};
    bool m_previewSettingsApplyRequested{true};
    CUIScrollBar* m_vScrollBar{};
    bool m_vScrollBarInitialized{false};

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION(CUIWindow);
};
