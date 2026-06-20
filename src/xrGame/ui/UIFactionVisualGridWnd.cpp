#include "StdAfx.h"

#include "UIFactionVisualGridWnd.h"
#include "UIXmlInit.h"
#include "UIInventoryUtilities.h"
#include "xrEngine/Render.h"
#include "xrUICore/ScrollBar/UIScrollBar.h"
#include "xrUICore/Static/UIStaticItem.h"
#include "xrUICore/XML/UITextureMaster.h"
#include "xrUICore/FontManager/FontManager.h"
#include "xrCore/_std_extensions.h"
#include "xrEngine/XR_IOConsole.h"

namespace
{
static constexpr char kAllTabName[] = "all";
static constexpr char kDefaultTabName[] = "default";
static constexpr float kMinCellSize = 8.0f;
static constexpr char kHoverHighlighterTexture[] = "ui_inGame2_armor_highlighter";

static bool is_empty_path(pcstr value)
{
    return !value || !value[0];
}

static void schedule_preview_model(pcstr model_path, u32 priority = 1000)
{
    if (!model_path || !model_path[0] || !GEnv.Render)
        return;

    GEnv.Render->PreviewScene_ScheduleModel(model_path, priority);
}

static void draw_preview_texture(
    const shared_str& texture_name, const Frect& rect, const Fvector2& origin, const Fvector2& source_size, u32 color)
{
    if (texture_name.empty())
        return;

    CUIStaticItem item;
    item.SetShader(InventoryUtilities::GetInstanceRtIconShader(texture_name.c_str()));
    item.SetTextureColor(color);
    Fvector2 ts{};
    if (item.GetShader())
        item.GetShader()->GetBaseTextureResolution(ts);

    // Never derive texture UV size from grid frame settings.
    // Different grids may share the same cached RT with different desired frame sizes,
    // and using frame size as texture rect causes UV overflow/stretch artifacts.
    if (ts.x <= 0.0f || ts.y <= 0.0f)
        ts = source_size;

    if (ts.x > 0.0f && ts.y > 0.0f)
    {
        const float scale = std::min(rect.width() / ts.x, rect.height() / ts.y);
        const float draw_width = ts.x * scale;
        const float draw_height = ts.y * scale;
        const float draw_x = origin.x + rect.left + (rect.width() - draw_width) * 0.5f;
        const float draw_y = origin.y + rect.top + (rect.height() - draw_height) * 0.5f;

        item.SetPos(draw_x, draw_y);
        item.SetSize(Fvector2().set(draw_width, draw_height));
        item.SetTextureRect(Frect().set(0.0f, 0.0f, ts.x, ts.y));
    }
    else
    {
        item.SetPos(origin.x + rect.left, origin.y + rect.top);
        item.SetSize(Fvector2().set(rect.width(), rect.height()));
    }
    item.Render();
}

static void draw_hover_highlighter(const Frect& rect, const Fvector2& origin, u32 color)
{
    ui_shader shader;
    Frect texture_rect{};
    if (!CUITextureMaster::InitTexture(kHoverHighlighterTexture, "hud" DELIMITER "default", shader, texture_rect))
        return;

    CUIStaticItem item;
    item.SetShader(shader);
    item.SetTextureRect(texture_rect);
    item.SetTextureColor(color);
    item.SetPos(origin.x + rect.left, origin.y + rect.top);
    item.SetSize(Fvector2().set(rect.width(), rect.height()));
    item.Render();
}

static void draw_loading_placeholder(const Frect& rect, const Fvector2& origin)
{
    draw_hover_highlighter(rect, origin, color_rgba(70, 70, 70, 160));
}

} // namespace

CUIFactionVisualGridWnd::CUIFactionVisualGridWnd()
    : CUIWindow("CUIFactionVisualGridWnd"),
      m_activeTab(kAllTabName),
      m_selectedCellIndex(u32(-1)),
      m_gridColumns(4),
      m_gridRows(3),
      m_cellSize(),
      m_cellSpacing(),
      m_gridPadding(),
      m_tabBarHeight(28.0f),
      m_tabSpacing(4.0f),
      m_pageOffset(0),
      m_hoveredCellIndex(-1),
      m_hoveredTabIndex(-1)
{
    m_cellSize.set(256.0f, 256.0f);
    m_previewFrameSize.set(256.0f, 256.0f);
    m_cellSpacing.set(6.0f, 6.0f);
    m_gridPadding.set(8.0f, 8.0f);
    m_previewOffset.set(0.0f, 0.0f, 0.0f);

    m_vScrollBar = xr_new<CUIScrollBar>();
    m_vScrollBar->SetAutoDelete(true);
    m_vScrollBar->SetMessageTarget(this);
    m_vScrollBar->Show(false);
    AttachChild(m_vScrollBar);
}

CUIFactionVisualGridWnd::~CUIFactionVisualGridWnd()
{
    ReleaseNoCachePreviews();
}

bool CUIFactionVisualGridWnd::Init(pcstr xml_name, pcstr path)
{
    CUIXml uiXml;
    if (!uiXml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, xml_name, true))
    {
        return false;
    }

    InitFromXML(uiXml, path, 0);
    return true;
}

void CUIFactionVisualGridWnd::Update()
{
    inherited::Update();

    if (m_previewSettingsApplyRequested)
        SyncPreviewSettings();
}

void CUIFactionVisualGridWnd::InitFromXML(CUIXml& xml, pcstr path, int index)
{
    CUIXmlInit::InitWindow(xml, path, index, this);

    m_gridColumns = static_cast<u32>(std::max(1, xml.ReadAttribInt(path, index, "cols", static_cast<int>(m_gridColumns))));
    m_gridRows = static_cast<u32>(std::max(1, xml.ReadAttribInt(path, index, "rows", static_cast<int>(m_gridRows))));

    m_cellSize.x = std::max(kMinCellSize, xml.ReadAttribFlt(path, index, "cell_width", m_cellSize.x));
    m_cellSize.y = std::max(kMinCellSize, xml.ReadAttribFlt(path, index, "cell_height", m_cellSize.y));
    m_previewFrameSize.x = std::max(kMinCellSize, xml.ReadAttribFlt(path, index, "frame_width", m_previewFrameSize.x));
    m_previewFrameSize.y = std::max(kMinCellSize, xml.ReadAttribFlt(path, index, "frame_height", m_previewFrameSize.y));
    m_cellSpacing.x = std::max(0.0f, xml.ReadAttribFlt(path, index, "cell_spacing_x", m_cellSpacing.x));
    m_cellSpacing.y = std::max(0.0f, xml.ReadAttribFlt(path, index, "cell_spacing_y", m_cellSpacing.y));

    const float padding = xml.ReadAttribFlt(path, index, "padding", -1.0f);
    if (padding >= 0.0f)
    {
        m_cellSpacing.set(padding, padding);
    }
    else
    {
        m_cellSpacing.x = std::max(0.0f, xml.ReadAttribFlt(path, index, "padding_x", m_cellSpacing.x));
        m_cellSpacing.y = std::max(0.0f, xml.ReadAttribFlt(path, index, "padding_y", m_cellSpacing.y));
    }

    const float grid_padding = xml.ReadAttribFlt(path, index, "grid_padding", -1.0f);
    if (grid_padding >= 0.0f)
    {
        m_gridPadding.set(grid_padding, grid_padding);
    }
    else
    {
        m_gridPadding.x = std::max(0.0f, xml.ReadAttribFlt(path, index, "grid_padding_x", m_gridPadding.x));
        m_gridPadding.y = std::max(0.0f, xml.ReadAttribFlt(path, index, "grid_padding_y", m_gridPadding.y));
    }

    m_tabBarHeight = std::max(0.0f, xml.ReadAttribFlt(path, index, "tab_height", m_tabBarHeight));
    m_tabSpacing = std::max(0.0f, xml.ReadAttribFlt(path, index, "tab_spacing", m_tabSpacing));
    m_previewYawDeg = xml.ReadAttribFlt(path, index, "preview_yaw_deg", m_previewYawDeg);
    m_previewPitchDeg = xml.ReadAttribFlt(path, index, "preview_pitch_deg", m_previewPitchDeg);
    m_previewRollDeg = xml.ReadAttribFlt(path, index, "preview_roll_deg", m_previewRollDeg);
    m_previewOffset.x = xml.ReadAttribFlt(path, index, "preview_offset_x", m_previewOffset.x);
    m_previewOffset.y = xml.ReadAttribFlt(path, index, "preview_offset_y", m_previewOffset.y);
    m_previewOffset.z = xml.ReadAttribFlt(path, index, "preview_offset_z", m_previewOffset.z);
    m_previewCameraDistanceOffset =
        xml.ReadAttribFlt(path, index, "preview_camera_distance_offset", m_previewCameraDistanceOffset);
    m_previewPoseName = xml.ReadAttrib(path, index, "preview_pose", "");
    const int disable_preview_cache = xml.ReadAttribInt(
        path, index, "disable_cache", xml.ReadAttribInt(path, index, "disable_cashe", m_disablePreviewCache ? 1 : 0));
    m_disablePreviewCache = (disable_preview_cache != 0);
    m_previewSettingsDirty = true;
    m_previewSettingsApplyRequested = true;

    ClampPageOffset();
}

bool CUIFactionVisualGridWnd::IsSyntheticAllTab(pcstr tab_name)
{
    return !is_empty_path(tab_name) && xr_stricmp(tab_name, kAllTabName) == 0;
}

pcstr CUIFactionVisualGridWnd::NormalizeTabName(pcstr tab_name)
{
    return is_empty_path(tab_name) ? kDefaultTabName : tab_name;
}

CUIFactionVisualGridWnd::TTabIterator CUIFactionVisualGridWnd::FindTab(pcstr tab_name)
{
    const pcstr normalized = NormalizeTabName(tab_name);
    return std::find_if(m_tabs.begin(), m_tabs.end(),
        [normalized](const STabData& tab) { return !tab.name.empty() && xr_stricmp(tab.name.c_str(), normalized) == 0; });
}

CUIFactionVisualGridWnd::TConstTabIterator CUIFactionVisualGridWnd::FindTab(pcstr tab_name) const
{
    const pcstr normalized = NormalizeTabName(tab_name);
    return std::find_if(m_tabs.begin(), m_tabs.end(),
        [normalized](const STabData& tab) { return !tab.name.empty() && xr_stricmp(tab.name.c_str(), normalized) == 0; });
}

CUIFactionVisualGridWnd::TTabIterator CUIFactionVisualGridWnd::EnsureTab(pcstr tab_name)
{
    const pcstr normalized = NormalizeTabName(tab_name);
    auto it = FindTab(normalized);
    if (it != m_tabs.end())
        return it;

    STabData tab;
    tab.name = normalized;
    m_tabs.push_back(tab);
    m_tabNamesDirty = true;
    return m_tabs.end() - 1;
}

bool CUIFactionVisualGridWnd::ContainsModel(const TModelList& models, const shared_str& model_path)
{
    return std::find(models.begin(), models.end(), model_path) != models.end();
}

u32 CUIFactionVisualGridWnd::FindModelIndex(const TModelList& models, const shared_str& model_path)
{
    const auto it = std::find(models.begin(), models.end(), model_path);
    return it == models.end() ? u32(-1) : static_cast<u32>(std::distance(models.begin(), it));
}

void CUIFactionVisualGridWnd::AddUniqueToAll(const shared_str& model_path)
{
    if (model_path.empty())
        return;

    if (!ContainsModel(m_allModels, model_path))
    {
        m_allModels.push_back(model_path);
    }
}

void CUIFactionVisualGridWnd::AddUniqueToTab(STabData& tab, const shared_str& model_path)
{
    if (model_path.empty())
        return;

    if (!ContainsModel(tab.models, model_path))
    {
        tab.models.push_back(model_path);
    }
}

void CUIFactionVisualGridWnd::RebuildAllModels()
{
    m_allModels.clear();
    for (const STabData& tab : m_tabs)
    {
        for (const shared_str& model_path : tab.models)
            AddUniqueToAll(model_path);
    }
}

void CUIFactionVisualGridWnd::ResetSelection()
{
    m_selectedModel = "";
    m_selectedTab = "";
    m_selectedCellIndex = u32(-1);
}

void CUIFactionVisualGridWnd::ResetHover()
{
    m_hoveredCellIndex = -1;
    m_hoveredTabIndex = -1;
}

void CUIFactionVisualGridWnd::ClampPageOffset()
{
    const u32 total_rows = GetTotalRowCount();
    const u32 visible_rows = GetVisibleRowCount();

    if (m_gridColumns == 0 || total_rows == 0 || visible_rows == 0)
    {
        m_pageOffset = 0;
        return;
    }

    u32 row_offset = GetRowOffset();
    const u32 max_row_offset = (total_rows > visible_rows) ? (total_rows - visible_rows) : 0;
    if (row_offset > max_row_offset)
        row_offset = max_row_offset;

    m_pageOffset = row_offset * m_gridColumns;
}

u32 CUIFactionVisualGridWnd::GetTabCount() const
{
    return 1u + static_cast<u32>(m_tabs.size());
}

const CUIFactionVisualGridWnd::TModelList& CUIFactionVisualGridWnd::GetActiveModels() const
{
    if (IsSyntheticAllTab(m_activeTab.c_str()))
        return m_allModels;

    const auto it = FindTab(m_activeTab.c_str());
    if (it != m_tabs.end())
        return it->models;

    return m_allModels;
}

pcstr CUIFactionVisualGridWnd::GetModelDisplayName(pcstr model_path)
{
    static string256 display_name;
    display_name[0] = '\0';

    if (is_empty_path(model_path))
        return display_name;

    pcstr leaf = model_path;
    if (pcstr last_slash = strrchr(model_path, '/'))
        leaf = last_slash + 1;
    if (pcstr last_backslash = strrchr(leaf, '\\'))
        leaf = last_backslash + 1;

    xr_strcpy(display_name, leaf);
    return display_name;
}

Frect CUIFactionVisualGridWnd::GetGridRect() const
{
    Frect rect;
    rect.left = m_gridPadding.x;
    rect.top = m_gridPadding.y;
    rect.right = GetWidth() - m_gridPadding.x;
    rect.bottom = GetHeight() - m_gridPadding.y;
    if (m_vScrollBar && m_vScrollBar->IsShown())
        rect.right -= m_vScrollBar->GetWidth() + m_cellSpacing.x;
    return rect;
}

Frect CUIFactionVisualGridWnd::GetTabRect(u32 tab_index) const
{
    Frect rect;
    rect.set(0.0f, 0.0f, 0.0f, m_tabBarHeight);

    CGameFont* font = UI().Font().pFontStat;
    if (!font)
        return rect;

    float x = m_gridPadding.x;
    const float y = m_gridPadding.y;
    const TTabNameList tab_names = GetTabNames();
    if (tab_index >= tab_names.size())
        return rect;

    for (u32 i = 0; i < tab_index; ++i)
    {
        float prev_width = font->SizeOf_(tab_names[i].c_str());
        UI().ClientToScreenScaledWidth(prev_width);
        x += prev_width + m_tabSpacing;
    }

    float text_width = font->SizeOf_(tab_names[tab_index].c_str());
    UI().ClientToScreenScaledWidth(text_width);

    const float tab_width = text_width + 20.0f;
    rect.left = x;
    rect.top = y;
    rect.right = x + tab_width;
    rect.bottom = y + m_tabBarHeight - m_gridPadding.y;
    return rect;
}

Frect CUIFactionVisualGridWnd::GetCellRect(u32 visible_index) const
{
    Frect grid = GetGridRect();
    const u32 col = m_gridColumns ? (visible_index % m_gridColumns) : 0;
    const u32 row = m_gridColumns ? (visible_index / m_gridColumns) : 0;

    const float x = grid.left + float(col) * (m_cellSize.x + m_cellSpacing.x);
    const float y = grid.top + float(row) * (m_cellSize.y + m_cellSpacing.y);

    Frect rect;
    rect.set(x, y, x + m_cellSize.x, y + m_cellSize.y);
    return rect;
}

int CUIFactionVisualGridWnd::GetTabIndexAt(float x, float y) const
{
    Fvector2 point;
    point.set(x, y);
    const TTabNameList tab_names = GetTabNames();
    for (u32 i = 0; i < tab_names.size(); ++i)
    {
        Frect rect = GetTabRect(i);
        if (rect.in(point))
            return static_cast<int>(i);
    }
    return -1;
}

int CUIFactionVisualGridWnd::GetCellIndexAt(float x, float y) const
{
    Fvector2 point;
    point.set(x, y);
    const Frect grid = GetGridRect();
    if (!grid.in(point))
        return -1;

    if (m_gridColumns == 0 || m_gridRows == 0)
        return -1;

    const float local_x = x - grid.left;
    const float local_y = y - grid.top;

    const float step_x = m_cellSize.x + m_cellSpacing.x;
    const float step_y = m_cellSize.y + m_cellSpacing.y;

    const int col = step_x > 0.0f ? static_cast<int>(local_x / step_x) : -1;
    const int row = step_y > 0.0f ? static_cast<int>(local_y / step_y) : -1;

    if (col < 0 || row < 0 || col >= static_cast<int>(m_gridColumns) || row >= static_cast<int>(m_gridRows))
        return -1;

    const float cell_left = grid.left + float(col) * step_x;
    const float cell_top = grid.top + float(row) * step_y;
    Frect cell_rect;
    cell_rect.set(cell_left, cell_top, cell_left + m_cellSize.x, cell_top + m_cellSize.y);
    if (!cell_rect.in(point))
        return -1;

    return row * static_cast<int>(m_gridColumns) + col;
}

u32 CUIFactionVisualGridWnd::GetTotalRowCount() const
{
    const u32 model_count = static_cast<u32>(GetActiveModels().size());
    if (model_count == 0 || m_gridColumns == 0)
        return 0;

    return (model_count + m_gridColumns - 1) / m_gridColumns;
}

u32 CUIFactionVisualGridWnd::GetVisibleRowCount() const
{
    return std::max(1u, m_gridRows);
}

u32 CUIFactionVisualGridWnd::GetRowOffset() const
{
    return m_gridColumns ? (m_pageOffset / m_gridColumns) : 0;
}

float CUIFactionVisualGridWnd::GetContentHeight() const
{
    const u32 total_rows = GetTotalRowCount();
    if (total_rows == 0)
        return 0.0f;

    return float(total_rows) * m_cellSize.y + float(total_rows - 1) * m_cellSpacing.y;
}

float CUIFactionVisualGridWnd::GetViewportHeight() const
{
    return std::max(0.0f, GetGridRect().height());
}

int CUIFactionVisualGridWnd::GetScrollStepPixels() const
{
    return std::max(1, iFloor(m_cellSize.y + m_cellSpacing.y + 0.5f));
}

int CUIFactionVisualGridWnd::GetScrollPixelOffset() const
{
    return static_cast<int>(GetRowOffset()) * GetScrollStepPixels();
}

void CUIFactionVisualGridWnd::SetRowOffset(u32 row_offset)
{
    m_pageOffset = row_offset * std::max(1u, m_gridColumns);
    ClampPageOffset();
}

void CUIFactionVisualGridWnd::UpdateScrollBar()
{
    if (!m_vScrollBar)
        return;

    const float content_height = GetContentHeight();
    const float viewport_height = GetViewportHeight();
    const bool need_scroll = content_height > viewport_height + EPS_L;

    if (!need_scroll)
    {
        m_vScrollBar->SetScrollPos(0);
        m_vScrollBar->Show(false);
        return;
    }

    const float scroll_y = m_gridPadding.y;
    const float scroll_length = std::max(1.0f, GetHeight() - scroll_y - m_gridPadding.y);

    if (!m_vScrollBarInitialized)
    {
        m_vScrollBar->InitScrollBar(Fvector2().set(0.0f, scroll_y), scroll_length, false);
        m_vScrollBarInitialized = true;
    }

    const float scroll_x = GetWidth() - m_gridPadding.x - m_vScrollBar->GetWidth();
    m_vScrollBar->SetWndPos(Fvector2().set(scroll_x, scroll_y));
    m_vScrollBar->SetHeight(scroll_length);
    m_vScrollBar->SetStepSize(GetScrollStepPixels());
    m_vScrollBar->SetRange(0, std::max(1, iFloor(content_height + 0.5f)));
    m_vScrollBar->SetPageSize(std::max(1, iFloor(viewport_height + 0.5f)));
    m_vScrollBar->SetScrollPos(GetScrollPixelOffset());
    m_vScrollBar->Show(true);
    m_vScrollBar->Enable(true);
}

void CUIFactionVisualGridWnd::SetGridDimensions(u32 columns, u32 rows)
{
    m_gridColumns = std::max(1u, columns);
    m_gridRows = std::max(1u, rows);
    ClampPageOffset();
    UpdateScrollBar();
}

void CUIFactionVisualGridWnd::SetCellSize(const Fvector2& size)
{
    m_cellSize.x = std::max(kMinCellSize, size.x);
    m_cellSize.y = std::max(kMinCellSize, size.y);
    ClampPageOffset();
    UpdateScrollBar();
}

void CUIFactionVisualGridWnd::SetPreviewFrameSize(const Fvector2& size)
{
    m_previewFrameSize.x = std::max(kMinCellSize, size.x);
    m_previewFrameSize.y = std::max(kMinCellSize, size.y);
    m_previewSettingsDirty = true;
    m_previewSettingsApplyRequested = true;
}

void CUIFactionVisualGridWnd::SetCellSpacing(const Fvector2& spacing)
{
    m_cellSpacing.x = std::max(0.0f, spacing.x);
    m_cellSpacing.y = std::max(0.0f, spacing.y);
    ClampPageOffset();
    UpdateScrollBar();
}

void CUIFactionVisualGridWnd::SetGridPadding(const Fvector2& padding)
{
    m_gridPadding.x = std::max(0.0f, padding.x);
    m_gridPadding.y = std::max(0.0f, padding.y);
    ClampPageOffset();
    UpdateScrollBar();
}

void CUIFactionVisualGridWnd::SetTabBarHeight(float height)
{
    m_tabBarHeight = std::max(0.0f, height);
    ClampPageOffset();
    UpdateScrollBar();
}

void CUIFactionVisualGridWnd::SetTabSpacing(float spacing)
{
    m_tabSpacing = std::max(0.0f, spacing);
    UpdateScrollBar();
}

void CUIFactionVisualGridWnd::SetPageOffset(u32 first_index)
{
    m_pageOffset = first_index;
    ClampPageOffset();
    UpdateScrollBar();
}

void CUIFactionVisualGridWnd::SetPreviewAngles(float yaw_deg, float pitch_deg, float roll_deg)
{
    m_previewYawDeg = yaw_deg;
    m_previewPitchDeg = pitch_deg;
    m_previewRollDeg = roll_deg;
    m_previewSettingsDirty = true;
    m_previewSettingsApplyRequested = true;
}

void CUIFactionVisualGridWnd::SetPreviewOffset(const Fvector& offset)
{
    m_previewOffset = offset;
    m_previewSettingsDirty = true;
    m_previewSettingsApplyRequested = true;
}

void CUIFactionVisualGridWnd::SetPreviewCameraDistanceOffset(float offset)
{
    m_previewCameraDistanceOffset = offset;
    m_previewSettingsDirty = true;
    m_previewSettingsApplyRequested = true;
}

void CUIFactionVisualGridWnd::SelectTab(u32 tab_index)
{
    const TTabNameList tab_names = GetTabNames();
    if (tab_index >= tab_names.size())
        return;

    SetActiveTab(tab_names[tab_index].c_str());
    ResetHover();
    ClampPageOffset();
}

void CUIFactionVisualGridWnd::SelectCell(u32 visible_index, bool fire_callback)
{
    const TModelList& models = GetActiveModels();
    const u32 model_index = m_pageOffset + visible_index;
    if (model_index >= models.size())
        return;

    const shared_str& model_path = models[model_index];
    SetSelectedModel(model_path.c_str(), m_activeTab.c_str(), model_index);

    if (fire_callback && m_onSelection)
        m_onSelection(m_selectedModel, m_selectedTab, m_selectedCellIndex);
}

void CUIFactionVisualGridWnd::Clear()
{
    ReleaseNoCachePreviews();
    m_tabs.clear();
    m_allModels.clear();
    m_highlightedModels.clear();
    m_tabNamesDirty = true;
    m_activeTab = kAllTabName;
    m_pageOffset = 0;
    ResetHover();
    ResetSelection();
}

void CUIFactionVisualGridWnd::AddModel(pcstr model_path)
{
    AddModelToTab(kDefaultTabName, model_path);
}

void CUIFactionVisualGridWnd::AddModels(const TModelList& models)
{
    AddModelsToTab(kDefaultTabName, models);
}

void CUIFactionVisualGridWnd::AddModelToTab(pcstr tab_name, pcstr model_path)
{
    if (is_empty_path(model_path))
        return;
    if (IsSyntheticAllTab(tab_name))
        tab_name = kDefaultTabName;

    STabData& tab = *EnsureTab(tab_name);
    const shared_str path = model_path;
    AddUniqueToTab(tab, path);
    AddUniqueToAll(path);
    schedule_preview_model(path.c_str());
}

void CUIFactionVisualGridWnd::AddModelsToTab(pcstr tab_name, const TModelList& models)
{
    for (const shared_str& model_path : models)
        AddModelToTab(tab_name, model_path.c_str());
}

void CUIFactionVisualGridWnd::SetModels(const TModelList& models)
{
    Clear();
    AddModels(models);
    SetActiveTab(kAllTabName);
}

void CUIFactionVisualGridWnd::SetData(const TTabList& tabs)
{
    Clear();

    for (const STabData& tab : tabs)
    {
        if (IsSyntheticAllTab(tab.name.c_str()))
            continue;

        for (const shared_str& model_path : tab.models)
            AddModelToTab(tab.name.c_str(), model_path.c_str());
    }

    SetActiveTab(kAllTabName);
}

void CUIFactionVisualGridWnd::SetData(const TTabMap& tabs)
{
    Clear();

    for (const auto& [tab_name, models] : tabs)
    {
        if (IsSyntheticAllTab(tab_name.c_str()))
            continue;

        AddModelsToTab(tab_name.c_str(), models);
    }

    SetActiveTab(kAllTabName);
}

const CUIFactionVisualGridWnd::TTabNameList& CUIFactionVisualGridWnd::GetTabNames() const
{
    if (m_tabNamesDirty)
    {
        m_cachedTabNames.clear();
        m_cachedTabNames.push_back(kAllTabName);
        for (const STabData& tab : m_tabs)
            m_cachedTabNames.push_back(tab.name);
        m_tabNamesDirty = false;
    }
    return m_cachedTabNames;
}

const CUIFactionVisualGridWnd::TModelList& CUIFactionVisualGridWnd::GetModelsInTab(pcstr tab_name) const
{
    if (IsSyntheticAllTab(tab_name))
        return m_allModels;

    const auto it = FindTab(tab_name);
    if (it != m_tabs.end())
        return it->models;

    static const TModelList empty_models;
    return empty_models;
}

u32 CUIFactionVisualGridWnd::GetModelCount(pcstr tab_name) const
{
    return static_cast<u32>(GetModelsInTab(tab_name).size());
}

bool CUIFactionVisualGridWnd::HasTab(pcstr tab_name) const
{
    return IsSyntheticAllTab(tab_name) || FindTab(tab_name) != m_tabs.end();
}

bool CUIFactionVisualGridWnd::HasModel(pcstr model_path) const
{
    if (is_empty_path(model_path))
        return false;

    const shared_str path = model_path;
    return ContainsModel(m_allModels, path);
}

void CUIFactionVisualGridWnd::SetActiveTab(pcstr tab_name)
{
    if (IsSyntheticAllTab(tab_name) || HasTab(tab_name))
        m_activeTab = NormalizeTabName(tab_name);
    else
        m_activeTab = kAllTabName;
    ClampPageOffset();
    ResetHover();
}

bool CUIFactionVisualGridWnd::SetCurrentTab(pcstr tab_name)
{
    if (!(IsSyntheticAllTab(tab_name) || HasTab(tab_name)))
        return false;

    SetActiveTab(tab_name);
    return true;
}

void CUIFactionVisualGridWnd::SetSelectedModel(pcstr model_path, pcstr tab_name, u32 cell_index)
{
    if (is_empty_path(model_path))
    {
        ResetSelection();
        return;
    }

    const shared_str path = model_path;
    if (!ContainsModel(m_allModels, path))
    {
        ResetSelection();
        return;
    }

    m_selectedModel = path;
    if (!is_empty_path(tab_name))
    {
        m_selectedTab = NormalizeTabName(tab_name);
    }
    else
    {
        const auto tab_it = std::find_if(m_tabs.begin(), m_tabs.end(),
            [&path](const STabData& tab) { return ContainsModel(tab.models, path); });
        m_selectedTab = (tab_it != m_tabs.end()) ? tab_it->name : kAllTabName;
    }

    if (cell_index != u32(-1))
        m_selectedCellIndex = cell_index;
    else
        m_selectedCellIndex = FindModelIndex(GetModelsInTab(m_selectedTab.c_str()), path);
}

void CUIFactionVisualGridWnd::OnFocusLost()
{
    inherited::OnFocusLost();
    ResetHover();
}

shared_str CUIFactionVisualGridWnd::GetPreviewPoseSourceModel() const
{
    if (m_selectedModel.size())
        return m_selectedModel;

    const TModelList& models = GetActiveModels();
    if (models.empty())
        return shared_str();

    if (m_pageOffset < models.size())
        return models[m_pageOffset];

    return models.front();
}

void CUIFactionVisualGridWnd::RefreshPreviewPoseCycleNames()
{
    m_previewPoseCycleNames.clear();
    m_previewPoseCycleSourceModel = GetPreviewPoseSourceModel();

    if (!m_previewPoseCycleSourceModel.size() || !GEnv.Render)
        return;

    GEnv.Render->PreviewScene_CollectCycleNames(m_previewPoseCycleSourceModel.c_str(), m_previewPoseCycleNames);
}

void CUIFactionVisualGridWnd::FillDebugInfo()
{
#ifndef MASTER_GOLD
    CUIWindow::FillDebugInfo();

    if (!ImGui::CollapsingHeader(CUIFactionVisualGridWnd::GetDebugType()))
        return;

    bool changed = false;
    bool layout_changed = false;
    int grid_dims[2] = {static_cast<int>(m_gridColumns), static_cast<int>(m_gridRows)};
    if (ImGui::DragInt2("Grid size", grid_dims, 1.0f, 1, 64))
    {
        SetGridDimensions(static_cast<u32>(grid_dims[0]), static_cast<u32>(grid_dims[1]));
        layout_changed = true;
    }

    float cell_size[2] = {m_cellSize.x, m_cellSize.y};
    if (ImGui::DragFloat2("Cell size", cell_size, 1.0f, kMinCellSize, 4096.0f, "%.0f"))
    {
        SetCellSize(Fvector2().set(cell_size[0], cell_size[1]));
        layout_changed = true;
    }

    float cell_spacing[2] = {m_cellSpacing.x, m_cellSpacing.y};
    if (ImGui::DragFloat2("Cell spacing", cell_spacing, 1.0f, 0.0f, 1024.0f, "%.0f"))
    {
        SetCellSpacing(Fvector2().set(cell_spacing[0], cell_spacing[1]));
        layout_changed = true;
    }

    float uniform_padding = (fis_zero(m_cellSpacing.x - m_cellSpacing.y, EPS_S) ? m_cellSpacing.x : 0.0f);
    if (ImGui::DragFloat("Padding", &uniform_padding, 1.0f, 0.0f, 1024.0f, "%.0f"))
    {
        SetCellSpacing(Fvector2().set(uniform_padding, uniform_padding));
        layout_changed = true;
    }

    float tab_height = m_tabBarHeight;
    if (ImGui::DragFloat("Tab height", &tab_height, 1.0f, 0.0f, 1024.0f, "%.0f"))
    {
        SetTabBarHeight(tab_height);
        layout_changed = true;
    }

    float tab_spacing = m_tabSpacing;
    if (ImGui::DragFloat("Tab spacing", &tab_spacing, 1.0f, 0.0f, 1024.0f, "%.0f"))
    {
        SetTabSpacing(tab_spacing);
        layout_changed = true;
    }

    changed |= ImGui::DragFloat("Preview yaw", &m_previewYawDeg, 0.1f, -360.0f, 360.0f, "%.2f deg");
    changed |= ImGui::DragFloat("Preview pitch", &m_previewPitchDeg, 0.1f, -360.0f, 360.0f, "%.2f deg");
    changed |= ImGui::DragFloat("Preview roll", &m_previewRollDeg, 0.1f, -360.0f, 360.0f, "%.2f deg");
    changed |= ImGui::DragFloat3("Preview offset", &m_previewOffset.x, 0.005f, -10.0f, 10.0f, "%.3f");
    changed |= ImGui::DragFloat("Preview cam distance", &m_previewCameraDistanceOffset, 0.01f, -10.0f, 10.0f, "%.3f");
    int preview_frame[2] = {iFloor(m_previewFrameSize.x + 0.5f), iFloor(m_previewFrameSize.y + 0.5f)};
    if (ImGui::DragInt2("Preview frame", preview_frame, 1.0f, int(kMinCellSize), 4096))
    {
        m_previewFrameSize.set(
            float(std::max<int>(int(kMinCellSize), preview_frame[0])),
            float(std::max<int>(int(kMinCellSize), preview_frame[1])));
        changed = true;
        m_previewSettingsApplyRequested = true;
    }

    const shared_str source_model = GetPreviewPoseSourceModel();
    if (source_model != m_previewPoseCycleSourceModel)
        RefreshPreviewPoseCycleNames();

    const pcstr combo_label = m_previewPoseName.size() ? m_previewPoseName.c_str() : "Auto";
    if (ImGui::BeginCombo("Preview pose", combo_label))
    {
        const bool auto_selected = (m_previewPoseName.size() == 0);
        if (ImGui::Selectable("Auto", auto_selected))
        {
            m_previewPoseName = "";
            changed = true;
            m_previewSettingsApplyRequested = true;
        }

        if (auto_selected)
            ImGui::SetItemDefaultFocus();

        for (const shared_str& cycle_name : m_previewPoseCycleNames)
        {
            const bool selected = (m_previewPoseName == cycle_name);
            if (ImGui::Selectable(cycle_name.c_str(), selected))
            {
                m_previewPoseName = cycle_name;
                changed = true;
                m_previewSettingsApplyRequested = true;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }

        ImGui::EndCombo();
    }

    if (changed)
        m_previewSettingsDirty = true;

    if (layout_changed)
        ResetHover();

    if (m_previewSettingsApplyRequested)
        SyncPreviewSettings(true);

    if (m_previewSettingsDirty)
    {
        if (ImGui::Button("Update previews"))
        {
            m_previewSettingsApplyRequested = true;
            SyncPreviewSettings(true);
        }
    }

    if (source_model.size())
        ImGui::Text("Pose source: %s", source_model.c_str());

    if (source_model.size() && GEnv.Render)
    {
        const shared_str resolved_pose = GEnv.Render->PreviewScene_ResolvedPoseName(source_model.c_str());
        if (resolved_pose.size())
            ImGui::Text("Resolved pose: %s", resolved_pose.c_str());
    }
#endif
}

bool CUIFactionVisualGridWnd::OnMouseAction(float x, float y, EUIMessages mouse_action)
{
    const u32 prev_page_offset = m_pageOffset;
    const int prev_scroll_pos = m_vScrollBar ? m_vScrollBar->GetScrollPos() : 0;
    const bool handled = inherited::OnMouseAction(x, y, mouse_action);

    if (mouse_action == WINDOW_MOUSE_WHEEL_UP || mouse_action == WINDOW_MOUSE_WHEEL_DOWN ||
        mouse_action == WINDOW_MOUSE_WHEEL_LEFT || mouse_action == WINDOW_MOUSE_WHEEL_RIGHT)
    {
        const int new_scroll_pos = m_vScrollBar ? m_vScrollBar->GetScrollPos() : 0;
        return handled || prev_page_offset != m_pageOffset || prev_scroll_pos != new_scroll_pos;
    }

    return handled;
}

void CUIFactionVisualGridWnd::SendMessage(CUIWindow* pWnd, s16 msg, void* pData)
{
    if (pWnd == m_vScrollBar && msg == SCROLLBAR_VSCROLL)
    {
        const int scroll_pixels = std::max(0, m_vScrollBar->GetScrollPos());
        const int row_step_pixels = GetScrollStepPixels();
        SetRowOffset(static_cast<u32>(scroll_pixels / std::max(1, row_step_pixels)));
        UpdateScrollBar();
        ResetHover();
        return;
    }

    inherited::SendMessage(pWnd, msg, pData);
}

void CUIFactionVisualGridWnd::OnMouseMove()
{
    m_hoveredTabIndex = -1;
    m_hoveredCellIndex = GetCellIndexAt(cursor_pos.x, cursor_pos.y);
}

void CUIFactionVisualGridWnd::OnMouseScroll(float iDirection)
{
    const u32 visible_count = GetVisibleCellCount();
    if (visible_count == 0)
        return;

    const TModelList& models = GetActiveModels();
    if (models.empty())
        return;

    const u32 scroll_step = std::max(1u, m_gridColumns);

    const int wheel_dir = static_cast<int>(iDirection);
    if (wheel_dir == WINDOW_MOUSE_WHEEL_UP || wheel_dir == WINDOW_MOUSE_WHEEL_LEFT)
    {
        if (m_pageOffset >= scroll_step)
            m_pageOffset -= scroll_step;
        else
            m_pageOffset = 0;
    }
    else if (wheel_dir == WINDOW_MOUSE_WHEEL_DOWN || wheel_dir == WINDOW_MOUSE_WHEEL_RIGHT)
    {
        m_pageOffset += scroll_step;
    }

    ClampPageOffset();
    UpdateScrollBar();
    ResetHover();
}

bool CUIFactionVisualGridWnd::OnMouseDown(int mouse_btn)
{
    if (inherited::OnMouseDown(mouse_btn))
        return true;

    if (mouse_btn != MOUSE_1)
        return false;

    const int cell_index = GetCellIndexAt(cursor_pos.x, cursor_pos.y);
    if (cell_index >= 0)
    {
        m_hoveredCellIndex = cell_index;
        SelectCell(static_cast<u32>(cell_index), true);
        return true;
    }

    return false;
}

bool CUIFactionVisualGridWnd::OnDbClick()
{
    const int cell_index = GetCellIndexAt(cursor_pos.x, cursor_pos.y);
    if (cell_index >= 0)
    {
        m_hoveredCellIndex = cell_index;
        SelectCell(static_cast<u32>(cell_index), true);
        return true;
    }
    return false;
}

void CUIFactionVisualGridWnd::Draw()
{
    ClampPageOffset();
    UpdateScrollBar();

    Frect client_rect;
    GetAbsoluteRect(client_rect);
    UI().PushScissor(client_rect);

    CGameFont* font = UI().Font().pFontStat;
    if (!font)
    {
        UI().PopScissor();
        inherited::Draw();
        return;
    }

    const TModelList& models = GetActiveModels();
    const u32 visible_count = GetVisibleCellCount();
    const u32 model_count = static_cast<u32>(models.size());

    for (u32 visible_index = 0; visible_index < visible_count; ++visible_index)
    {
        const u32 model_index = m_pageOffset + visible_index;
        if (model_index >= model_count)
            break;

        const Frect cell_rect = GetCellRect(visible_index);
        const bool is_hovered = (m_hoveredCellIndex == static_cast<int>(visible_index));
        const pcstr model_path = models[model_index].c_str();

        // Visible cells get top priority so they render before off-screen background preloads
        schedule_preview_model(model_path, 1);

        const Frect preview_rect = cell_rect;

        if (IsModelHighlighted(model_path))
            draw_hover_highlighter(preview_rect, client_rect.lt, m_persistentHighlightColor);

        if (is_hovered)
            draw_hover_highlighter(preview_rect, client_rect.lt, color_rgba(255, 255, 255, 96));

        const shared_str texture_name = GetPreviewTextureForModel(model_path);
        if (texture_name.size())
        {
            draw_preview_texture(
                texture_name, preview_rect, client_rect.lt, m_previewFrameSize, color_rgba(255, 255, 255, 255));
        }
        else if (GEnv.Render && GEnv.Render->PreviewScene_IsDirty(model_path))
        {
            draw_loading_placeholder(preview_rect, client_rect.lt);
        }

    }

    if (m_pageOffset > 0 || model_count > visible_count)
    {
        const u32 max_offset = (model_count > visible_count) ? (model_count - visible_count) : 0;
        string256 page_info;
        xr_sprintf(page_info, "%u/%u", (m_pageOffset / std::max(1u, m_gridColumns)) + 1u,
            ((max_offset / std::max(1u, m_gridColumns)) + 1u));

        Fvector2 info_pos;
        info_pos.x = GetWidth() - m_gridPadding.x - 4.0f;
        info_pos.y = m_gridPadding.y + 2.0f;
        UI().ClientToScreenScaled(info_pos);
        font->SetAligment(CGameFont::alRight);
        font->SetColor(color_rgba(145, 145, 145, 255));
        font->Out(info_pos.x, info_pos.y, "%s", page_info);
    }

    font->OnRender();
    UI().PopScissor();
    inherited::Draw();
}

void CUIFactionVisualGridWnd::SyncPreviewSettings(bool force)
{
    if ((!force && !m_previewSettingsDirty) || !GEnv.Render)
        return;

    SPreviewSceneSettings settings;
    settings.yaw_deg = m_previewYawDeg;
    settings.pitch_deg = m_previewPitchDeg;
    settings.roll_deg = m_previewRollDeg;
    settings.offset = m_previewOffset;
    settings.camera_distance_offset = m_previewCameraDistanceOffset;
    settings.pose_name = m_previewPoseName;
    settings.frame_width = std::max<u32>(u32(kMinCellSize), iFloor(m_previewFrameSize.x + 0.5f));
    settings.frame_height = std::max<u32>(u32(kMinCellSize), iFloor(m_previewFrameSize.y + 0.5f));

    m_previewSettingsDirty = false;
    m_previewSettingsApplyRequested = false;
    GEnv.Render->PreviewScene_SetSettings(settings);
    WarmPreviewVisibleModels();
}

void CUIFactionVisualGridWnd::SetPreviewPose(pcstr pose_name)
{
    m_previewPoseName = pose_name ? pose_name : "";
    m_previewSettingsDirty = true;
    m_previewSettingsApplyRequested = true;
}

void CUIFactionVisualGridWnd::ClearHighlightedModels()
{
    m_highlightedModels.clear();
}

void CUIFactionVisualGridWnd::SetHighlightedModels(const TModelList& models)
{
    m_highlightedModels.clear();
    for (const shared_str& model_path : models)
    {
        if (model_path.empty())
            continue;
        m_highlightedModels.insert(model_path);
    }
}

void CUIFactionVisualGridWnd::SetPersistentHighlightColor(u32 color)
{
    m_persistentHighlightColor = color;
}

void CUIFactionVisualGridWnd::WarmPreviewVisibleModels()
{
    if (!GEnv.Render)
        return;

    const TModelList& models = GetActiveModels();
    const u32 visible_count = GetVisibleCellCount();
    if (models.empty() || visible_count == 0)
        return;

    for (u32 visible_index = 0; visible_index < visible_count; ++visible_index)
    {
        const u32 model_index = m_pageOffset + visible_index;
        if (model_index >= models.size())
            break;

        GEnv.Render->PreviewScene_ScheduleModel(models[model_index].c_str(), 1);
    }
}

void CUIFactionVisualGridWnd::ReleaseNoCachePreviews()
{
    if (!m_noCacheModelTextures.empty() && GEnv.Render)
    {
        for (const auto& [model_path, texture_name] : m_noCacheModelTextures)
        {
            (void)model_path;
            if (texture_name.size())
                GEnv.Render->PreviewScene_ReleaseEphemeralTexture(texture_name.c_str());
        }
    }

    m_noCacheModelTextures.clear();
}

void CUIFactionVisualGridWnd::RefreshNoCachePreview(pcstr model_path)
{
    if (!m_disablePreviewCache || !GEnv.Render || is_empty_path(model_path))
        return;

    // Apply this grid preview settings without calling SyncPreviewSettings(),
    // because SyncPreviewSettings() warms visible models and would recurse
    // back into this method for no-cache grids.
    SPreviewSceneSettings settings;
    settings.yaw_deg = m_previewYawDeg;
    settings.pitch_deg = m_previewPitchDeg;
    settings.roll_deg = m_previewRollDeg;
    settings.offset = m_previewOffset;
    settings.camera_distance_offset = m_previewCameraDistanceOffset;
    settings.pose_name = m_previewPoseName;
    settings.frame_width = std::max<u32>(u32(kMinCellSize), iFloor(m_previewFrameSize.x + 0.5f));
    settings.frame_height = std::max<u32>(u32(kMinCellSize), iFloor(m_previewFrameSize.y + 0.5f));
    GEnv.Render->PreviewScene_SetSettings(settings);

    shared_str texture_name;
    if (GEnv.Render->PreviewScene_RenderModelNoCache(model_path, texture_name))
        m_noCacheModelTextures[shared_str(model_path)] = texture_name;
}

bool CUIFactionVisualGridWnd::IsModelHighlighted(pcstr model_path) const
{
    if (is_empty_path(model_path) || m_highlightedModels.empty())
        return false;

    return m_highlightedModels.count(shared_str(model_path)) != 0;
}

shared_str CUIFactionVisualGridWnd::GetPreviewTextureForModel(pcstr model_path) const
{
    if (is_empty_path(model_path) || !GEnv.Render)
        return shared_str();

    if (m_disablePreviewCache)
    {
        auto it = m_noCacheModelTextures.find(shared_str(model_path));
        if (it != m_noCacheModelTextures.end())
            return it->second;
        return shared_str();
    }

    if (GEnv.Render->PreviewScene_IsCached(model_path))
        return GEnv.Render->PreviewScene_TextureName(model_path);

    return shared_str();
}
