#include "StdAfx.h"

#include "UIFactionEditorMapWnd.h"

#include "UIMapWnd.h"
#include "UIXmlInit.h"

CUIFactionEditorMapWnd::CUIFactionEditorMapWnd()
    : CUIWindow("CUIFactionEditorMapWnd"), m_mapWnd(nullptr), m_spawnSource("all"),
      m_lastClickedId(u32(-1)), m_lastClickType(EUiMapClick::Left), m_lastClickModifiers(eUiMapModNone),
      m_hasPendingClick(false)
{
}

bool CUIFactionEditorMapWnd::Init(pcstr xml_name, pcstr path)
{
    CUIXml uiXml;
    if (!uiXml.Load(CONFIG_PATH, UI_PATH, UI_PATH_DEFAULT, xml_name, true))
        return false;

    CUIXmlInit::InitWindow(uiXml, path, 0, this);

    string512 mapPath;
    strconcat(sizeof(mapPath), mapPath, path, ":map_wnd");

    m_mapWnd = xr_new<CUIMapWnd>(nullptr);
    m_mapWnd->SetAutoDelete(true);
    if (!m_mapWnd->Init(xml_name, mapPath, true))
        return false;

    AttachChild(m_mapWnd);
    Reload();
    m_mapWnd->Show(true);
    return true;
}

void CUIFactionEditorMapWnd::SetSpawnName(pcstr spawn_name)
{
    m_spawnSource.SetSpawnName(spawn_name);
}

bool CUIFactionEditorMapWnd::SetPointVisual(u32 logical_id, pcstr owner_faction, pcstr icon_texture)
{
    if (!m_mapWnd)
        return false;

    return m_mapWnd->SetExternalPointVisual(logical_id, owner_faction, icon_texture);
}

void CUIFactionEditorMapWnd::Reload()
{
    ReloadInternal(true);
}

void CUIFactionEditorMapWnd::ReloadPreserveView()
{
    ReloadInternal(false);
}

void CUIFactionEditorMapWnd::ReloadInternal(bool focus_default_target)
{
    if (!m_mapWnd)
        return;

    m_spawnSource.Reload();
    m_mapWnd->ClearExternalDataSource();
    m_mapWnd->SetExternalDataSource(&m_spawnSource);
    if (focus_default_target)
        FocusDefaultTarget();
}

void CUIFactionEditorMapWnd::AddSpotClickCallback(const luabind::object& lua_function, const luabind::object& context)
{
    m_onSpotClickLua.set(lua_function, context);
}

void CUIFactionEditorMapWnd::FocusDefaultTarget()
{
    if (!m_mapWnd)
        return;

    m_mapWnd->ActivateLayer(EPdaMapLayer::Surface, false);
    m_mapWnd->ViewGlobalMap();
}

void CUIFactionEditorMapWnd::Update()
{
    inherited::Update();

    if (!m_mapWnd)
        return;

    u32 logicalId = u32(-1);
    EUiMapClick clickType = EUiMapClick::Left;
    u32 clickModifiers = eUiMapModNone;
    if (m_mapWnd->ConsumeExternalMapClick(logicalId, clickType, clickModifiers))
    {
        m_lastClickedId = logicalId;
        m_lastClickType = clickType;
        m_lastClickModifiers = clickModifiers;
        m_hasPendingClick = true;

        if (m_onSpotClickLua)
            m_onSpotClickLua(logicalId, static_cast<u32>(clickType), clickModifiers);
    }
}

u32 CUIFactionEditorMapWnd::GetLogicalIdBySmartName(pcstr smart_name) const
{
    if (!smart_name || !smart_name[0])
        return u32(-1);

    xr_vector<SMapPointDesc> points;
    m_spawnSource.EnumeratePoints(points);
    for (const auto& point : points)
    {
        if (point.smart_name == smart_name)
            return point.logical_id;
    }

    return u32(-1);
}

void CUIFactionEditorMapWnd::Draw()
{
    inherited::Draw();

    if (m_mapWnd)
        m_mapWnd->DrawHint();
}

pcstr CUIFactionEditorMapWnd::GetPointString(u32 logical_id, shared_str SMapPointDesc::*field) const
{
    if (!m_mapWnd)
        return "";

    SMapPointDesc pointDesc;
    if (!m_mapWnd->GetExternalPointDesc(logical_id, pointDesc))
        return "";

    m_cachedPointString = pointDesc.*field;
    return m_cachedPointString.c_str();
}

pcstr CUIFactionEditorMapWnd::GetPointLevelName(u32 logical_id) const
{
    return GetPointString(logical_id, &SMapPointDesc::level_name);
}

pcstr CUIFactionEditorMapWnd::GetPointSmartName(u32 logical_id) const
{
    return GetPointString(logical_id, &SMapPointDesc::smart_name);
}

pcstr CUIFactionEditorMapWnd::GetPointSectionName(u32 logical_id) const
{
    return GetPointString(logical_id, &SMapPointDesc::section_name);
}

pcstr CUIFactionEditorMapWnd::GetPointHintText(u32 logical_id) const
{
    return GetPointString(logical_id, &SMapPointDesc::hint_text);
}

pcstr CUIFactionEditorMapWnd::GetPointDisplayName(u32 logical_id) const
{
    return GetPointString(logical_id, &SMapPointDesc::display_name);
}

pcstr CUIFactionEditorMapWnd::GetPointSmartType(u32 logical_id) const
{
    return GetPointString(logical_id, &SMapPointDesc::smart_type);
}

pcstr CUIFactionEditorMapWnd::GetPointOwnerFaction(u32 logical_id) const
{
    return GetPointString(logical_id, &SMapPointDesc::owner_faction);
}

pcstr CUIFactionEditorMapWnd::GetPointIconTexture(u32 logical_id) const
{
    return GetPointString(logical_id, &SMapPointDesc::icon_texture);
}

void CUIFactionEditorMapWnd::EnumeratePoints(const luabind::functor<void>& callback) const
{
    xr_vector<SMapPointDesc> points;
    m_spawnSource.EnumeratePoints(points);
    for (const auto& point : points)
        callback(point.logical_id, point.smart_name.c_str());
}
