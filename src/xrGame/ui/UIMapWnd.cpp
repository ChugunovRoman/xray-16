#include "StdAfx.h"
#include "UIMapWnd.h"
#include "UIMap.h"
#include "UIXmlInit.h"
#include "Actor.h"
#include "map_manager.h"
#include "UIInventoryUtilities.h"
#include "map_spot.h"
#include "map_location.h"
#include "xrUICore/ScrollBar/UIFixedScrollBar.h"
#include "xrUICore/Windows/UIFrameWindow.h"
#include "xrUICore/Windows/UIFrameLineWnd.h"
#include "xrUICore/TabControl/UITabControl.h"
#include "xrUICore/Buttons/UI3tButton.h"
#include "UIMapWndActions.h"
#include "UIMapWndActionsSpace.h"
#include "xrUICore/Hint/UIHint.h"
#include "map_hint.h"
#include "xrUICore/Cursor/UICursor.h"
#include "xrUICore/PropertiesBox/UIPropertiesBox.h"
#include "xrUICore/ListBox/UIListBoxItem.h"
#include "xrEngine/xr_input.h" //remove me !!!
#include "UIHelper.h"

CUIMapWnd* g_map_wnd = NULL; // quick temporary solution -(
CUIMapWnd* GetMapWnd() { return g_map_wnd; }

namespace
{
EPdaMapLayer g_lastPdaMapLayer = EPdaMapLayer::Surface;

constexpr size_t layer_index(EPdaMapLayer layer) { return static_cast<size_t>(layer); }

pcstr get_global_map_section(EPdaMapLayer layer)
{
    if (layer == EPdaMapLayer::Underground && pGameIni->section_exist("global_map_underground"))
        return "global_map_underground";

    if (layer == EPdaMapLayer::Surface && pGameIni->section_exist("global_map_surface"))
        return "global_map_surface";

    return "global_map";
}

EPdaMapLayer get_level_map_layer(pcstr map_name)
{
    if (!pGameIni->line_exist(map_name, "pda_map_layer"))
        return EPdaMapLayer::Surface;

    return xr_stricmp(pGameIni->r_string(map_name, "pda_map_layer"), "underground") == 0 ? EPdaMapLayer::Underground :
                                                                                             EPdaMapLayer::Surface;
}

xr_string normalize_map_name(const shared_str& map_name)
{
    xr_string normalized = map_name.c_str();
    if (!normalized.empty())
        xr_strlwr(&normalized[0]);
    return normalized;
}

bool try_init_external_spot_preset(CUIStatic& icon, pcstr preset_id)
{
    if (!preset_id || !preset_id[0] || !strstr(preset_id, "circle_"))
        return false;

    static CUIXml xml;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = xml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, "map_spots.xml", true);
        if (!loaded)
            return false;
    }

    if (!xml.NavigateToNode(preset_id, 0))
        return false;

    CUIXmlInit::InitStatic(xml, preset_id, 0, &icon);
    if (!icon.Heading())
    {
        icon.SetWidth(icon.GetWidth() * UI().get_current_kx());
        icon.SetStretchTexture(true);
    }

    return true;
}

} // namespace

CUIMapWnd::CUIMapWnd(UIHint* hint)
    : CUIWindow("CUIMapWnd"), m_ActionPlanner(nullptr)
{
    m_tgtMap = NULL;
    m_GlobalMap = NULL;
    m_activeLayer = g_lastPdaMapLayer;
    m_view_actor = false;
    m_prev_actor_pos.set(FLT_MAX, FLT_MAX);
    m_force_viewport_reset = false;
    m_level_changed_since_last_show = false;
    m_currentZoom = 1.0f;
    m_map_location_hint = NULL;
    m_map_move_step = 10.0f;
    /*
    #ifdef DEBUG
    //	m_dbg_text_hint			= NULL;
    //	m_dbg_info				= NULL;
    #endif // DEBUG
    */

    m_UIMainMapHeader = nullptr;
    m_scroll_mode = false;
    m_btn_nav_parent = nullptr;
    m_btn_layer_surface = nullptr;
    m_btn_layer_underground = nullptr;
    m_nav_timing = Device.dwTimeGlobal;
    hint_wnd = hint;
    g_map_wnd = this;
    m_cur_location = nullptr;
    m_externalDataSource = nullptr;
    m_externalDataRevision = 0;
    m_lastExternalClickedId = u32(-1);
    m_lastExternalClick = EUiMapClick::Left;
    m_hasPendingExternalClick = false;
}

CUIMapWnd::~CUIMapWnd()
{
    ClearExternalSpots();
    delete_data(m_ActionPlanner);
    for (auto& globalMap : m_GlobalMaps)
    {
        if (globalMap && m_UILevelFrame && m_UILevelFrame->IsChild(globalMap))
            m_UILevelFrame->DetachChild(globalMap);
        xr_delete(globalMap);
    }
    for (auto& maps : m_GameMaps)
        delete_data(maps);
    delete_data(m_map_location_hint);
    /*
    #ifdef DEBUG
        delete_data( m_dbg_text_hint );
        delete_data( m_dbg_info );
    #endif // DEBUG
    */
    g_map_wnd = NULL;
}

EPdaMapLayer CUIMapWnd::ResolveLayerForGlobalMap(const CUIGlobalMap* global_map) const
{
    for (size_t idx = 0; idx < PDA_MAP_LAYER_COUNT; ++idx)
    {
        if (m_GlobalMaps[idx] == global_map)
            return static_cast<EPdaMapLayer>(idx);
    }

    return EPdaMapLayer::Surface;
}

CUICustomMap* CUIMapWnd::FindLevelMap(const shared_str& map_name, EPdaMapLayer* layer) const
{
    const xr_string normalized = normalize_map_name(map_name);
    const shared_str normalizedName = normalized.c_str();

    for (size_t idx = 0; idx < PDA_MAP_LAYER_COUNT; ++idx)
    {
        const auto& maps = m_GameMaps[idx];
        const auto it = maps.find(normalizedName);
        if (it != maps.end())
        {
            if (layer)
                *layer = static_cast<EPdaMapLayer>(idx);
            return it->second;
        }
    }

    if (layer)
        *layer = EPdaMapLayer::Surface;

    return nullptr;
}

pcstr CUIMapWnd::ResolveExternalSpotTexture(const SMapPointDesc& point) const
{
    const pcstr explicitTexture = point.icon_texture.c_str();
    if (explicitTexture && explicitTexture[0])
        return explicitTexture;

    const pcstr spotType = point.spot_type.c_str();
    if (!spotType || !spotType[0])
        return "ui_pda2_base";

    if (xr_stricmp(spotType, "smart_terrain") == 0)
        return "ui_pda2_base";

    return spotType;
}

void CUIMapWnd::ClearExternalSpots()
{
    HideCurHint();

    for (auto& spot : m_externalSpots)
    {
        if (spot.level_map && spot.icon && spot.level_map->IsChild(spot.icon))
            spot.level_map->DetachChild(spot.icon);
        xr_delete(spot.icon);
        spot.level_map = nullptr;
    }

    m_externalSpots.clear();
    m_lastExternalClickedId = u32(-1);
    m_hasPendingExternalClick = false;
}

void CUIMapWnd::RebuildExternalSpots()
{
    ClearExternalSpots();

    if (!m_externalDataSource)
        return;

    xr_vector<SMapPointDesc> points;
    m_externalDataSource->EnumeratePoints(points);

    for (const auto& point : points)
    {
        EPdaMapLayer layer = EPdaMapLayer::Surface;
        CUICustomMap* customMap = FindLevelMap(point.level_name, &layer);
        CUILevelMap* levelMap = smart_cast<CUILevelMap*>(customMap);
        if (!levelMap)
            continue;

        auto* icon = xr_new<CUIStatic>("external_map_spot");
        const pcstr textureId = ResolveExternalSpotTexture(point);
        const bool presetInit = try_init_external_spot_preset(*icon, textureId);
        if (!presetInit)
        {
            icon->InitTextureEx(textureId, "hud" DELIMITER "default");
            icon->SetWndSize(Fvector2().set(24.0f, 24.0f));
            icon->SetStretchTexture(true);
        }
        icon->SetTextureColor(point.icon_color);
        icon->SetWndPos(Fvector2().set(0.0f, 0.0f));
        icon->Show(layer == m_activeLayer && IsShown());
        levelMap->AttachChild(icon);

        SExternalMapSpot spot;
        spot.desc = point;
        spot.layer = layer;
        spot.level_map = levelMap;
        spot.icon = icon;
        spot.visible = false;
        m_externalSpots.push_back(spot);
    }

    UpdateExternalSpots();
}

void CUIMapWnd::RefreshExternalDataSource()
{
    if (!m_externalDataSource)
        return;

    m_externalDataRevision = m_externalDataSource->GetDataRevision();
    RebuildExternalSpots();
}

void CUIMapWnd::UpdateExternalSpots()
{
    if (!UsingExternalDataSource())
        return;

    for (auto& spot : m_externalSpots)
    {
        if (!spot.icon || !spot.level_map)
            continue;

        const bool shouldShow = IsShown() && spot.layer == m_activeLayer && spot.level_map->IsShown();
        spot.visible = shouldShow;
        spot.icon->Show(shouldShow);

        if (!shouldShow)
            continue;

        Fvector2 realPos;
        realPos.set(spot.desc.position.x, spot.desc.position.z);
        Fvector2 localPos = spot.level_map->ConvertRealToLocal(realPos, false);
        const Fvector2 iconSize = spot.icon->GetWndSize();
        localPos.x -= iconSize.x * 0.5f;
        localPos.y -= iconSize.y * 0.5f;
        spot.icon->SetWndPos(localPos);
    }
}

bool CUIMapWnd::HandleExternalSpotMouse(float x, float y, EUIMessages mouse_action)
{
    if (!UsingExternalDataSource())
        return false;

    const Fvector2 cursorPos = GetUICursor().GetCursorPosition();
    SExternalMapSpot* hoveredSpot = nullptr;

    for (auto& spot : m_externalSpots)
    {
        if (!spot.visible || !spot.icon)
            continue;

        Frect rect;
        spot.icon->GetAbsoluteRect(rect);
        if (rect.in(cursorPos.x, cursorPos.y))
        {
            hoveredSpot = &spot;
            break;
        }
    }

    if (mouse_action == WINDOW_MOUSE_MOVE)
    {
        if (hoveredSpot)
        {
            if (m_map_location_hint->GetOwner() != hoveredSpot->icon)
            {
                HideCurHint();
                const pcstr hintText = hoveredSpot->desc.hint_text.c_str();
                const pcstr smartName = hoveredSpot->desc.smart_name.c_str();
                ShowHintStr(hoveredSpot->icon, hintText && hintText[0] ? hintText : smartName);
            }
            return true;
        }

        if (m_map_location_hint->GetOwner())
            HideCurHint();
        return false;
    }

    if (!hoveredSpot)
        return false;

    if (mouse_action == WINDOW_LBUTTON_UP || mouse_action == WINDOW_RBUTTON_UP)
    {
        m_lastExternalClickedId = hoveredSpot->desc.logical_id;
        m_lastExternalClick = mouse_action == WINDOW_LBUTTON_UP ? EUiMapClick::Left : EUiMapClick::Right;
        m_hasPendingExternalClick = true;
        return true;
    }

    return false;
}

void CUIMapWnd::SetExternalDataSource(IMapDataSource* source)
{
    if (m_externalDataSource == source)
        return;

    ClearExternalSpots();
    for (auto& maps : m_GameMaps)
    {
        for (auto& mapEntry : maps)
            mapEntry.second->DetachAll();
    }
    m_externalDataSource = source;
    if (m_externalDataSource)
    {
        RefreshExternalDataSource();
        m_view_actor = true;
    }
    else
    {
        m_externalDataRevision = 0;
    }
}

void CUIMapWnd::ClearExternalDataSource()
{
    if (!m_externalDataSource && m_externalSpots.empty())
        return;

    ClearExternalSpots();
    m_externalDataSource = nullptr;
    m_externalDataRevision = 0;
}

bool CUIMapWnd::ConsumeExternalMapClick(u32& logical_id, EUiMapClick& click_type)
{
    if (!m_hasPendingExternalClick)
        return false;

    logical_id = m_lastExternalClickedId;
    click_type = m_lastExternalClick;
    m_hasPendingExternalClick = false;
    return true;
}

bool CUIMapWnd::GetExternalPointDescInternal(u32 logical_id, SMapPointDesc& out) const
{
    for (const auto& spot : m_externalSpots)
    {
        if (spot.desc.logical_id != logical_id)
            continue;

        out = spot.desc;
        return true;
    }

    return false;
}

void CUIMapWnd::UpdateActiveMapLayout()
{
    if (!GlobalMap())
        return;

    const Frect activeRect = ActiveMapRect();
    GlobalMap()->WorkingArea().set(activeRect);

    for (auto it = GameMaps().begin(), itEnd = GameMaps().end(); it != itEnd; ++it)
    {
        it->second->WorkingArea().set(activeRect);
    }
}

void CUIMapWnd::RefreshLevelMapRects()
{
    UpdateActiveMapLayout();

    if (GlobalMap())
        GlobalMap()->Update();

    for (auto it = GameMaps().begin(), itEnd = GameMaps().end(); it != itEnd; ++it)
        it->second->Update();
}

void CUIMapWnd::ResetMapStateForLevelChange()
{
    m_tgtMap = nullptr;
    m_tgtCenter.set(0.0f, 0.0f);
    m_prev_actor_pos.set(FLT_MAX, FLT_MAX);
    m_view_actor = true;
    m_force_viewport_reset = true;
    m_level_changed_since_last_show = true;
    HideCurHint();
    if (m_ActionPlanner)
        ResetActionPlanner();
}

bool CUIMapWnd::CheckForActorLevelChange()
{
    if (UsingExternalDataSource())
        return false;

    const shared_str currentLevel = Level().name();
    cpcstr previousLevel = m_last_actor_level_name.c_str();
    cpcstr currentLevelStr = currentLevel.c_str();
    const bool changed = !previousLevel || !previousLevel[0] || xr_stricmp(previousLevel, currentLevelStr) != 0;

    if (!changed)
        return false;

    m_last_actor_level_name = currentLevel;
    ResetMapStateForLevelChange();
    return true;
}

void CUIMapWnd::SyncActiveLayerVisibility(bool status)
{
    for (size_t idx = 0; idx < PDA_MAP_LAYER_COUNT; ++idx)
    {
        CUIGlobalMap* globalMap = m_GlobalMaps[idx];
        if (!globalMap)
            continue;

        const bool isActive = status && idx == layer_index(m_activeLayer);
        if (isActive)
        {
            if (!m_UILevelFrame->IsChild(globalMap))
                m_UILevelFrame->AttachChild(globalMap);
            globalMap->Show(true);
        }
        else
        {
            if (m_UILevelFrame->IsChild(globalMap))
                m_UILevelFrame->DetachChild(globalMap);
            globalMap->Show(false);
        }

        auto mapIt = m_GameMaps[idx].begin();
        auto mapItEnd = m_GameMaps[idx].end();
        for (; mapIt != mapItEnd; ++mapIt)
        {
            mapIt->second->Show(isActive);
        }
    }
}

bool CUIMapWnd::SetActiveLayer(EPdaMapLayer layer, bool preserveViewport)
{
    CUIGlobalMap* target = GetGlobalMap(layer);
    if (!target)
        return false;

    if (m_activeLayer == layer && m_GlobalMap == target && preserveViewport && !m_force_viewport_reset)
    {
        UpdateLayerSwitcherState();
        return true;
    }

    Frect previousRect;
    const bool hadPrevious = m_GlobalMap != nullptr;
    if (hadPrevious && preserveViewport && !m_force_viewport_reset)
    {
        previousRect = m_GlobalMap->GetWndRect();
    }

    m_activeLayer = layer;
    m_GlobalMap = target;
    g_lastPdaMapLayer = layer;

    if (hadPrevious && preserveViewport && !m_force_viewport_reset)
    {
        m_GlobalMap->SetWndRect(previousRect);
        m_GlobalMap->ClipByVisRect();
    }
    else
    {
        m_GlobalMap->OptimalFit(m_UILevelFrame->GetWndRect());
        m_GlobalMap->SetMinZoom(m_GlobalMap->GetCurrentZoom().x);
        m_GlobalMap->ClipByVisRect();
    }

    m_currentZoom = m_GlobalMap->GetCurrentZoom().x;
    clamp(m_currentZoom, m_GlobalMap->GetMinZoom(), m_GlobalMap->GetMaxZoom());

    UpdateActiveMapLayout();
    SyncActiveLayerVisibility(IsShown());
    UpdateScroll();
    UpdateLayerSwitcherState();
    HideCurHint();
    if (m_ActionPlanner)
        ResetActionPlanner();

    return true;
}

bool CUIMapWnd::Init(cpcstr xml_name, cpcstr start_from, bool critical /*= true*/)
{
    CUIXml uiXml;
    if (!uiXml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, xml_name, critical))
        return false;

    string512 pth;
    strconcat(sizeof(pth), pth, start_from, ":main_wnd");
    CUIXmlInit::InitWindow(uiXml, pth, 0, this);

    m_map_move_step = uiXml.ReadAttribFlt(start_from, 0, "map_move_step", 10.0f);

    strconcat(sizeof(pth), pth, start_from, ":main_map_frame");
    m_UIMainFrame = UIHelper::CreateFrameWindow(uiXml, pth, this, false);
    if (!m_UIMainFrame)
    {
        strconcat(sizeof(pth), pth, start_from, ":main_wnd:main_map_frame");
        m_UIMainFrame = UIHelper::CreateFrameWindow(uiXml, pth, this, false);
    }

    strconcat(sizeof(pth), pth, start_from, ":level_frame");
    m_UILevelFrame = UIHelper::CreateNormalWindow(uiXml, pth, this, false);
    if (!m_UILevelFrame)
    {
        strconcat(sizeof(pth), pth, start_from, ":main_wnd:main_map_frame:level_frame");
        m_UILevelFrame = UIHelper::CreateNormalWindow(uiXml, pth, m_UIMainFrame);
    }

    strconcat(sizeof(pth), pth, start_from, "main_map_header");
    m_UIMainMapHeader = UIHelper::CreateFrameLine(uiXml, pth, this, false);
    if (!m_UIMainMapHeader)
    {
        strconcat(sizeof(pth), pth, start_from, ":main_wnd:map_header_frame_line");
        m_UIMainMapHeader = UIHelper::CreateFrameLine(uiXml, pth, m_UIMainFrame, false);
    }

    m_scroll_mode = uiXml.ReadAttribInt(start_from, 0, "scroll_enable", 0) == 1;
    if (m_scroll_mode || ShadowOfChernobylMode)
    {
        float dx, dy, sx, sy;
        strconcat(sizeof(pth), pth, start_from, ":main_map_frame");
        dx = uiXml.ReadAttribFlt(pth, 0, "dx", 0.0f);
        dy = uiXml.ReadAttribFlt(pth, 0, "dy", 0.0f);
        sx = uiXml.ReadAttribFlt(pth, 0, "sx", 5.0f);
        sy = uiXml.ReadAttribFlt(pth, 0, "sy", 5.0f);

        CUIWindow* rect_parent = m_UIMainFrame; // m_UILevelFrame;
        Frect r = rect_parent->GetWndRect();

        auto tempScroll = xr_new<CUIFixedScrollBar>();
        if (tempScroll->InitScrollBar(Fvector2().set(r.left + dx, r.bottom - sy), true))
            m_UIMainScrollH = tempScroll;
        else
        {
            Msg("! Failed to init m_UIMainScrollH as FixedScrollBar, trying to initialize it as ScrollBar");
            xr_delete(tempScroll);
            m_UIMainScrollH = xr_new<CUIScrollBar>();
            m_UIMainScrollH->InitScrollBar(Fvector2().set(r.left + dx, r.bottom - sy), r.right - r.left - dx * 2 - sx, true, "pda");
        }

        m_UIMainScrollH->SetStepSize(_max(1, (int)(m_UILevelFrame->GetWidth() * 0.1f)));
        m_UIMainScrollH->SetPageSize((int)m_UILevelFrame->GetWidth()); // iFloor
        m_UIMainScrollH->SetAutoDelete(true);
        AttachChild(m_UIMainScrollH);
        Register(m_UIMainScrollH);
        AddCallback(m_UIMainScrollH, SCROLLBAR_HSCROLL, CUIWndCallback::void_function(this, &CUIMapWnd::OnScrollH));

        tempScroll = xr_new<CUIFixedScrollBar>();
        if (tempScroll->InitScrollBar(Fvector2().set(r.right - sx, r.top + dy), false))
            m_UIMainScrollV = tempScroll;
        else
        {
            Msg("! Failed to init m_UIMainScrollV as FixedScrollBar, trying to initialize it as ScrollBar");
            xr_delete(tempScroll);
            m_UIMainScrollV = xr_new<CUIScrollBar>();
            m_UIMainScrollV->InitScrollBar(Fvector2().set(r.right - sx, r.top + dy), r.bottom - r.top - dy * 2, false, "pda");
        }

        m_UIMainScrollV->SetStepSize(_max(1, (int)(m_UILevelFrame->GetHeight() * 0.1f)));
        m_UIMainScrollV->SetPageSize((int)m_UILevelFrame->GetHeight());
        m_UIMainScrollV->SetAutoDelete(true);
        AttachChild(m_UIMainScrollV);
        Register(m_UIMainScrollV);
        AddCallback(m_UIMainScrollV, SCROLLBAR_VSCROLL, CUIWndCallback::void_function(this, &CUIMapWnd::OnScrollV));
    }

    init_xml_nav(uiXml, start_from, critical);
    init_xml_layer_switcher(uiXml, start_from, critical);

    m_map_location_hint = xr_new<CUIMapLocationHint>();
    m_map_location_hint->SetAutoDelete(false);
    m_map_location_hint->SetCustomDraw(true);
    strconcat(sizeof(pth), pth, start_from, ":map_hint_item");
    m_map_location_hint->Init(uiXml, pth);

    // Load maps

    for (size_t idx = 0; idx < PDA_MAP_LAYER_COUNT; ++idx)
    {
        const auto layer = static_cast<EPdaMapLayer>(idx);
        CUIGlobalMap*& globalMap = m_GlobalMaps[idx];
        globalMap = xr_new<CUIGlobalMap>(this);
        const pcstr globalSection = get_global_map_section(layer);
        globalMap->Initialize(globalSection);
        globalMap->OptimalFit(m_UILevelFrame->GetWndRect());
        globalMap->SetMinZoom(globalMap->GetCurrentZoom().x);
        Register(globalMap);

    }

    m_GlobalMap = GetGlobalMap(m_activeLayer);
    if (!m_GlobalMap)
    {
        m_activeLayer = EPdaMapLayer::Surface;
        m_GlobalMap = GetGlobalMap(m_activeLayer);
    }

    m_currentZoom = m_GlobalMap ? m_GlobalMap->GetCurrentZoom().x : 1.0f;

    // initialize local maps
    xr_string sect_name;
    if (!g_pGameLevel && pGameIni->section_exist("level_maps_single"))
        sect_name = "level_maps_single";
    else if (IsGameTypeSingle())
        sect_name = "level_maps_single";
    else
        sect_name = "level_maps_mp";

    if (pGameIni->section_exist(sect_name.c_str()))
    {
        CInifile::Sect& S = pGameIni->r_section(sect_name.c_str());
        auto it = S.Data.cbegin(), end = S.Data.cend();
        for (; it != end; ++it)
        {
            shared_str map_name = it->first;
            xr_strlwr(map_name);
            const EPdaMapLayer layer = get_level_map_layer(map_name.c_str());
            GAME_MAPS& maps = GetGameMaps(layer);
            R_ASSERT2(maps.end() == maps.find(map_name), "Duplicate level name not allowed");

            CUICustomMap*& l = maps[map_name];

            l = xr_new<CUILevelMap>(this);
            R_ASSERT2(pGameIni->section_exist(map_name), map_name.c_str());
            l->Initialize(map_name, "hud" DELIMITER "default");

            l->OptimalFit(m_UILevelFrame->GetWndRect());
            l->Show(layer == m_activeLayer);
            GetGlobalMap(layer)->AttachChild(l);

        }
    }

#ifdef DEBUG
    for (size_t idx = 0; idx < PDA_MAP_LAYER_COUNT; ++idx)
    {
        CUIGlobalMap* globalMap = m_GlobalMaps[idx];
        auto& maps = m_GameMaps[idx];
        auto it = maps.begin();
        auto itEnd = maps.end();
        for (; it != itEnd; ++it)
        {
            CUILevelMap* l = smart_cast<CUILevelMap*>(it->second);
            VERIFY(l);
            auto it2 = it;
            for (; it2 != maps.end(); ++it2)
            {
                if (it == it2)
                    continue;

                CUILevelMap* l2 = smart_cast<CUILevelMap*>(it2->second);
                VERIFY(l2);
                if (l->GlobalRect().intersected(l2->GlobalRect()))
                {
                    Msg(" --error-incorrect map definition global rect of map [%s] intersects with [%s]", l->MapName().c_str(),
                        l2->MapName().c_str());
                }
            }

            if (globalMap && FALSE == l->GlobalRect().intersected(globalMap->BoundRect()))
            {
                Msg(" --error-incorrect map definition map [%s] places outside global map layer [%d]", l->MapName().c_str(),
                    idx);
            }
        }
    }
#endif
    m_ActionPlanner = xr_new<CMapActionPlanner>();
    m_ActionPlanner->setup(this);
    m_view_actor = true;

    m_UIPropertiesBox = xr_new<CUIPropertiesBox>();
    m_UIPropertiesBox->SetAutoDelete(true);
    m_UIPropertiesBox->InitPropertiesBox(Fvector2().set(0, 0), Fvector2().set(300, 300));
    AttachChild(m_UIPropertiesBox);
    m_UIPropertiesBox->Hide();
    m_UIPropertiesBox->SetWindowName("property_box");

    return true;
}

void CUIMapWnd::Show(bool status)
{
    const bool canCheckActorLevel = status && !UsingExternalDataSource() && g_pGameLevel != nullptr;
    const bool actorLevelChanged = canCheckActorLevel ? CheckForActorLevelChange() : false;

    inherited::Show(status);
    SyncActiveLayerVisibility(status);
    UpdateActiveMapLayout();
    UpdateLayerSwitcherState();

    if (status)
    {
        if (!UsingExternalDataSource() && !actorLevelChanged && !m_level_changed_since_last_show)
            Activated();
        UpdateScroll();

        if (m_view_actor || actorLevelChanged || m_level_changed_since_last_show)
        {
            m_view_actor = true;
        }
        if (!UsingExternalDataSource())
            InventoryUtilities::SendInfoToActor("ui_pda_map_local");
    }
    HideCurHint();
}

void CUIMapWnd::Activated()
{
    if (!IsShown())
        return;

    if (UsingExternalDataSource())
        return;

    if (CheckForActorLevelChange() || m_level_changed_since_last_show)
        return;

    Fvector v = Level().CurrentEntity()->Position();
    Fvector2 v2;
    v2.set(v.x, v.z);
    if (v2.distance_to(m_prev_actor_pos) > 3.0f)
    {
        ViewActor();
    }
}

void CUIMapWnd::AddMapToRender(CUICustomMap* m)
{
    Register(m);
    m_UILevelFrame->AttachChild(m);
    m->Show(true);
    m->WorkingArea().set(ActiveMapRect());
}

void CUIMapWnd::RemoveMapToRender(CUICustomMap* m)
{
    if (m != GlobalMap())
        m_UILevelFrame->DetachChild(smart_cast<CUIWindow*>(m));
}

void CUIMapWnd::SetTargetMap(const shared_str& name, const Fvector2& pos, bool bZoomIn)
{
    EPdaMapLayer layer{};
    if (CUICustomMap* levelMap = FindLevelMap(name, &layer))
    {
        SetActiveLayer(layer, !m_force_viewport_reset);
        SetTargetMap(levelMap, pos, bZoomIn);
    }
}

void CUIMapWnd::SetTargetMap(const shared_str& name, bool bZoomIn)
{
    EPdaMapLayer layer{};
    if (CUICustomMap* levelMap = FindLevelMap(name, &layer))
    {
        SetActiveLayer(layer, !m_force_viewport_reset);
        SetTargetMap(levelMap, bZoomIn);
    }
}

void CUIMapWnd::SetTargetMap(CUICustomMap* m, bool bZoomIn)
{
    if (!m)
        return;

    m_tgtMap = m;
    Fvector2 pos;
    Frect r = m->BoundRect();
    r.getcenter(pos);
    SetTargetMap(m, pos, bZoomIn);
}

void CUIMapWnd::SetTargetMap(CUICustomMap* m, const Fvector2& pos, bool bZoomIn)
{
    if (!m)
        return;

    if (auto* levelMap = smart_cast<CUILevelMap*>(m))
    {
        if (CUIGlobalMap* ownerGlobal = levelMap->GlobalMap())
            SetActiveLayer(ResolveLayerForGlobalMap(ownerGlobal), !m_force_viewport_reset);
    }
    else if (auto* globalMap = smart_cast<CUIGlobalMap*>(m))
    {
        SetActiveLayer(ResolveLayerForGlobalMap(globalMap), !m_force_viewport_reset);
    }

    m_tgtMap = m;

    if (m == GlobalMap())
    {
        CUIGlobalMap* gm = GlobalMap();
        SetZoom(gm->GetMinZoom());
        Frect vis_rect = ActiveMapRect();
        vis_rect.getcenter(m_tgtCenter);
        Fvector2 _p;
        gm->GetAbsolutePos(_p);
        m_tgtCenter.sub(_p);
        m_tgtCenter.div(gm->GetCurrentZoom());
    }
    else
    {
        if (bZoomIn /* && fsimilar(GlobalMap()->GetCurrentZoom(), GlobalMap()->GetMinZoom(),EPS_L )*/)
            SetZoom(GlobalMap()->GetMaxZoom());

        Fvector2 targetPos = pos;
        const Frect& levelBoundRect = m->BoundRect();
        if (!levelBoundRect.in(targetPos))
        {
            clamp(targetPos.x, levelBoundRect.x1, levelBoundRect.x2);
            clamp(targetPos.y, levelBoundRect.y1, levelBoundRect.y2);

        }

        if (auto* levelMap = smart_cast<CUILevelMap*>(m))
        {
            levelMap->Update();
            const Frect levelRect = levelMap->GetWndRect();
            const Fvector2 rawLogicLocal = levelMap->ConvertRealToLocal(targetPos, false);
            m_tgtCenter = rawLogicLocal;
            m_tgtCenter.add(levelRect.lt).div(GlobalMap()->GetCurrentZoom());
        }
        else
        {
            m_tgtCenter = m->ConvertRealToLocal(targetPos, true);
            m_tgtCenter.add(m->GetWndPos()).div(GlobalMap()->GetCurrentZoom());
        }
    }

    ResetActionPlanner();
}

void CUIMapWnd::MoveMap(Fvector2 const& pos_delta)
{
    if (!GlobalMap())
        return;

    GlobalMap()->MoveWndDelta(pos_delta);
    UpdateScroll();
    HideCurHint();
}

void CUIMapWnd::Draw()
{
    inherited::Draw();
    /*
    #ifdef DEBUG
        m_dbg_text_hint->Draw	();
        m_dbg_info->Draw		();
    #endif // DEBUG */

    if (m_btn_nav_parent)
        m_btn_nav_parent->Draw();
}

void CUIMapWnd::MapLocationRelcase(CMapLocation* ml)
{
    CUIWindow* owner = m_map_location_hint->GetOwner();
    if (owner)
    {
        CMapSpot* ms = smart_cast<CMapSpot*>(owner);
        if (ms && ms->MapLocation() == ml) // CUITaskItem also can be a HintOwner
            m_map_location_hint->SetOwner(NULL);
    }
}

void CUIMapWnd::DrawHint()
{
    CUIWindow* owner = m_map_location_hint->GetOwner();
    if (owner)
    {
        CMapSpot* ms = smart_cast<CMapSpot*>(owner);
        if (ms)
        {
            if (ms->MapLocation() && ms->MapLocation()->HintEnabled())
            {
                m_map_location_hint->Draw();
            }
        }
        else
        {
            m_map_location_hint->Draw();
        }
    }
}

bool CUIMapWnd::OnKeyboardAction(int dik, EUIMessages keyboard_action)
{
    switch (keyboard_action)
    {
    case WINDOW_KEY_PRESSED:
    {
        switch (GetBindedAction(dik, EKeyContext::PDA))
        {
        case kPDA_MAP_ZOOM_RESET:
            ViewGlobalMap();
            return true;

        case kPDA_MAP_SHOW_ACTOR:
            ViewActor();
            return true;

        case kPDA_MAP_SHOW_LEGEND:
            OnBtnLegend_Push(this, nullptr);
            return true;
        } // switch (dik)
        break;
    }

    case WINDOW_KEY_HOLD:
    {
        Fvector2 pos_delta{};

        switch (GetBindedAction(dik, EKeyContext::PDA))
        {
        case kPDA_MAP_ZOOM_OUT:
            // SetZoom(GetZoom()/1.5f);
            UpdateZoom(false);
            // ResetActionPlanner();
            return true;

        case kPDA_MAP_ZOOM_IN:
            // SetZoom(GetZoom()*1.5f);
            UpdateZoom(true);
            // ResetActionPlanner();
            return true;

        case kPDA_MAP_MOVE_UP:
            pos_delta.y += m_map_move_step;
            break;
        case kPDA_MAP_MOVE_DOWN:
            pos_delta.y -= m_map_move_step;
            break;
        case kPDA_MAP_MOVE_LEFT:
            pos_delta.x += m_map_move_step;
            break;
        case kPDA_MAP_MOVE_RIGHT:
            pos_delta.x -= m_map_move_step;
            break;
        }

        if (pos_delta.x || pos_delta.y)
        {
            MoveMap(pos_delta);
            return true;
        }
        break;
    }
    } // switch (keyboard_action)

    return inherited::OnKeyboardAction(dik, keyboard_action);
}

bool CUIMapWnd::OnControllerAction(int axis, const ControllerAxisState& state, EUIMessages controller_action)
{
    switch (GetBindedAction(axis, EKeyContext::PDA))
    {
    default:
        return OnKeyboardAction(axis, controller_action);

    case kPDA_MAP_MOVE:
    {
        if (controller_action != WINDOW_KEY_HOLD)
            break;
        const auto pos_delta = Fvector2{ m_map_move_step, m_map_move_step }.mul(Fvector2{ -state.x, -state.y }.normalize());
        MoveMap(pos_delta);
        return true;
    }
    } // switch (GetBindedAction(axis, EKeyContext::PDA))

    return inherited::OnControllerAction(axis, state, controller_action);
}

bool CUIMapWnd::OnMouseAction(float x, float y, EUIMessages mouse_action)
{
    const bool inheritedHandled = inherited::OnMouseAction(x, y, mouse_action);

    Fvector2 cursor_pos1 = GetUICursor().GetCursorPosition();

    if (UsingExternalDataSource() && ActiveMapRect().in(cursor_pos1))
    {
        if (HandleExternalSpotMouse(x, y, mouse_action))
            return true;
    }
    else if (UsingExternalDataSource() && mouse_action == WINDOW_MOUSE_MOVE && m_map_location_hint->GetOwner())
    {
        HideCurHint();
    }

    if (inheritedHandled /*|| m_btn_nav_parent->OnMouseAction(x,y,mouse_action)*/)
        return true;

    if (GlobalMap() && !GlobalMap()->Locked() && ActiveMapRect().in(cursor_pos1))
    {
        switch (mouse_action)
        {
        case WINDOW_RBUTTON_UP:
            ActivatePropertiesBox(NULL);
            break;
        case WINDOW_MOUSE_MOVE:
            if (pInput->iGetAsyncKeyState(MOUSE_1))
            {
                GlobalMap()->MoveWndDelta(GetUICursor().GetCursorPositionDelta());
                UpdateScroll();
                HideCurHint();
                return true;
            }
            break;

        case WINDOW_MOUSE_WHEEL_DOWN:
            UpdateZoom(true);
            return true;

        case WINDOW_MOUSE_WHEEL_UP:
            UpdateZoom(false);
            return true;
        } // switch (mouse_action)
    }

    return false;
}

bool CUIMapWnd::UpdateZoom(bool b_zoom_in)
{
    if (!GlobalMap())
        return true;

    float prev_zoom = GetZoom();
    float z = 0.0f;
    if (b_zoom_in)
    {
        z = GetZoom() * 1.2f;
        SetZoom(z);
    }
    else
    {
        z = GetZoom() / 1.2f;
        SetZoom(z);
    }

    if (!fsimilar(prev_zoom, GetZoom()))
    {
        //		m_tgtCenter.set( 0, 0 );// = cursor_pos;
        Frect vis_rect = ActiveMapRect();
        vis_rect.getcenter(m_tgtCenter);

        Fvector2 pos;
        CUIGlobalMap* gm = GlobalMap();
        gm->GetAbsolutePos(pos);
        m_tgtCenter.sub(pos);
        m_tgtCenter.div(gm->GetCurrentZoom());

        ResetActionPlanner();
        HideCurHint();
        return false;
    }
    return true;
}

void CUIMapWnd::SendMessage(CUIWindow* pWnd, s16 msg, void* pData)
{
    //	inherited::SendMessage( pWnd, msg, pData);
    CUIWndCallback::OnEvent(pWnd, msg, pData);

    if (pWnd == m_UIPropertiesBox && msg == PROPERTY_CLICKED && m_UIPropertiesBox->GetClickedItem())
    {
        luabind::functor<void> funct;
        if (GEnv.ScriptEngine->functor("pda.property_box_clicked", funct))
            funct(m_UIPropertiesBox, m_cur_location);
    }
}

void CUIMapWnd::ActivatePropertiesBox(CUIWindow* w)
{
    m_UIPropertiesBox->RemoveAll();

    CMapSpot* sp = smart_cast<CMapSpot*>(w);
    if (!sp)
        return;

    m_cur_location = sp->MapLocation();
    if (!m_cur_location)
        return;

    luabind::functor<void> funct;
    if (GEnv.ScriptEngine->functor("pda.property_box_add_properties", funct))
    {
        funct(m_UIPropertiesBox, m_cur_location->ObjectID(), (LPCSTR)m_cur_location->GetLevelName().c_str(), m_cur_location->GetHint());
    }

    // Только для меток игрока
    if (m_cur_location->IsUserDefined())
    {
        m_UIPropertiesBox->AddItem("st_pda_change_spot_hint", NULL, MAP_CHANGE_SPOT_HINT_ACT); // Изменяем название метки
        m_UIPropertiesBox->AddItem("st_pda_delete_spot", NULL, MAP_REMOVE_SPOT_ACT); // Удаляем метку
    }

    if (m_UIPropertiesBox->GetItemsCount() > 0)
    {
        m_UIPropertiesBox->AutoUpdateSize();

        Fvector2 cursor_pos;
        Frect vis_rect;

        GetAbsoluteRect(vis_rect);
        cursor_pos = GetUICursor().GetCursorPosition();
        cursor_pos.sub(vis_rect.lt);
        m_UIPropertiesBox->Show(vis_rect, cursor_pos);
    }
}

void CUIMapWnd::UpdateScroll()
{
    if (m_scroll_mode && GlobalMap())
    {
        Fvector2 w_pos = GlobalMap()->GetWndPos();
        m_UIMainScrollV->SetRange(m_UIMainScrollV->GetMinRange(), iFloor(GlobalMap()->GetHeight()));
        m_UIMainScrollH->SetRange(m_UIMainScrollV->GetMinRange(), iFloor(GlobalMap()->GetWidth()));

        m_UIMainScrollV->SetScrollPos(iFloor(-w_pos.y));
        m_UIMainScrollH->SetScrollPos(iFloor(-w_pos.x));
    }
}

void CUIMapWnd::OnScrollV(CUIWindow*, void*)
{
    if (m_scroll_mode && GlobalMap())
    {
        MoveScrollV(-1.0f * float(m_UIMainScrollV->GetScrollPos()));
    }
}

void CUIMapWnd::OnScrollH(CUIWindow*, void*)
{
    if (m_scroll_mode && GlobalMap())
    {
        MoveScrollH(-1.0f * float(m_UIMainScrollH->GetScrollPos()));
    }
}

void CUIMapWnd::MoveScrollV(float dy)
{
    if (!GlobalMap())
        return;

    Fvector2 w_pos = GlobalMap()->GetWndPos();
    GlobalMap()->SetWndPos(Fvector2().set(w_pos.x, dy));
}

void CUIMapWnd::MoveScrollH(float dx)
{
    if (!GlobalMap())
        return;

    Fvector2 w_pos = GlobalMap()->GetWndPos();
    GlobalMap()->SetWndPos(Fvector2().set(dx, w_pos.y));
}

void CUIMapWnd::Update()
{
    if (IsShown() && !UsingExternalDataSource() && g_pGameLevel != nullptr)
        CheckForActorLevelChange();

    UpdateActiveMapLayout();
    inherited::Update();

    if (UsingExternalDataSource() && m_externalDataSource)
    {
        const u32 revision = m_externalDataSource->GetDataRevision();
        if (revision != m_externalDataRevision)
        {
            RefreshExternalDataSource();
            m_view_actor = true;
        }
    }

    if (IsShown() && m_view_actor)
    {
        RefreshLevelMapRects();
        ViewActor();
        m_view_actor = false;
        m_force_viewport_reset = false;
        m_level_changed_since_last_show = false;
    }

    if (UsingExternalDataSource())
        UpdateExternalSpots();

    m_ActionPlanner->update();
    UpdateNav();
    UpdateLayerSwitcherState();
}

void CUIMapWnd::SetZoom(float value)
{
    if (!GlobalMap())
        return;

    m_currentZoom = value;
    clamp(m_currentZoom, GlobalMap()->GetMinZoom(), GlobalMap()->GetMaxZoom());
}

void CUIMapWnd::ViewGlobalMap()
{
    if (!GlobalMap() || GlobalMap()->Locked())
        return;
    SetTargetMap(GlobalMap());
}

void CUIMapWnd::ResetActionPlanner()
{
    m_ActionPlanner->m_storage.set_property(1, false);
    m_ActionPlanner->m_storage.set_property(2, false);
    m_ActionPlanner->m_storage.set_property(3, false);
}

void CUIMapWnd::ViewZoomIn()
{
    if (!GlobalMap() || GlobalMap()->Locked())
        return;
    UpdateZoom(true);
}

void CUIMapWnd::ViewZoomOut()
{
    if (!GlobalMap() || GlobalMap()->Locked())
        return;
    UpdateZoom(false);
}

void CUIMapWnd::ViewActor()
{
    if (!GlobalMap() || GlobalMap()->Locked())
        return;

    if (UsingExternalDataSource())
    {
        shared_str focusLevel;
        if (m_externalDataSource && m_externalDataSource->GetFocusLevel(focusLevel))
            SetTargetMap(focusLevel, true);
        else
            ViewGlobalMap();
        return;
    }

    Fvector v = Level().CurrentEntity()->Position();
    m_prev_actor_pos.set(v.x, v.z);

    CUICustomMap* lm = NULL;
    EPdaMapLayer layer{};
    if (CUICustomMap* levelMap = FindLevelMap(Level().name(), &layer))
    {
        SetActiveLayer(layer, !m_force_viewport_reset);
        lm = levelMap;
    }
    else
    {
        lm = GlobalMap();
    }

    SetTargetMap(lm, m_prev_actor_pos, true);
}

void CUIMapWnd::ShowHintStr(CUIWindow* parent, LPCSTR text) // map name
{
    if (m_map_location_hint->GetOwner())
        return;

    m_map_location_hint->SetInfoStr(text);
    m_map_location_hint->SetOwner(parent);
    ShowHint();
}

void CUIMapWnd::ShowHintSpot(CMapSpot* spot)
{
    CUIWindow* owner = m_map_location_hint->GetOwner();
    if (!owner)
    {
        m_map_location_hint->SetInfoMSpot(spot);
        m_map_location_hint->SetOwner(spot);
        ShowHint();
        return;
    }

    CMapSpot* prev_spot = smart_cast<CMapSpot*>(owner);
    if (prev_spot && (prev_spot->get_location_level() < spot->get_location_level()))
    {
        m_map_location_hint->SetInfoMSpot(spot);
        m_map_location_hint->SetOwner(spot);
        ShowHint();
        return;
    }
}

void CUIMapWnd::ShowHintTask(CGameTask* task, CUIWindow* owner)
{
    if (task)
    {
        m_map_location_hint->SetInfoTask(task);
        m_map_location_hint->SetOwner(owner);
        ShowHint(true);
        return;
    }
    HideCurHint();
}

void CUIMapWnd::ShowHint(bool extra)
{
    Frect vis_rect;
    if (extra)
    {
        vis_rect.set(Frect().set(0.0f, 0.0f, UI_BASE_WIDTH, UI_BASE_HEIGHT));
    }
    else
    {
        vis_rect = ActiveMapRect();
    }

    bool is_visible = fit_in_rect(m_map_location_hint, vis_rect);
    if (!is_visible)
    {
        HideCurHint();
    }
}

void CUIMapWnd::HideHint(CUIWindow* parent)
{
    if (m_map_location_hint->GetOwner() == parent)
    {
        HideCurHint();
    }
}

void CUIMapWnd::HideCurHint() { m_map_location_hint->SetOwner(NULL); }
void CUIMapWnd::Hint(const shared_str& text)
{
    /*
#ifdef DEBUG
    m_dbg_text_hint->SetTextST( *text );
#endif // DEBUG */
}

void CUIMapWnd::Reset()
{
    inherited::Reset();
    ResetActionPlanner();
}

#include "GametaskManager.h"
#include "Actor.h"
#include "map_spot.h"
#include "GameTask.h"

void CUIMapWnd::SpotSelected(CUIWindow* w)
{
    CMapSpot* sp = smart_cast<CMapSpot*>(w);
    if (!sp)
    {
        return;
    }

    CGameTask* t = Level().GameTaskManager().HasGameTask(sp->MapLocation(), true);
    if (t)
    {
        Level().GameTaskManager().SetActiveTask(t);
    }
}
