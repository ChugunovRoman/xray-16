#include "StdAfx.h"

#include "UIFactionEditorMapWnd.h"

#include "UIMapWnd.h"
#include "UIXmlInit.h"

CUIFactionEditorMapWnd::CUIFactionEditorMapWnd()
    : CUIWindow("CUIFactionEditorMapWnd"), m_mapWnd(nullptr), m_spawnSource("all"),
      m_lastClickedId(u32(-1)), m_lastClickType(EUiMapClick::Left), m_hasPendingClick(false)
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

void CUIFactionEditorMapWnd::Reload()
{
    if (!m_mapWnd)
        return;

    m_mapWnd->ClearExternalDataSource();
    m_mapWnd->SetExternalDataSource(&m_spawnSource);
    FocusDefaultTarget();
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
    if (m_mapWnd->ConsumeExternalMapClick(logicalId, clickType))
    {
        m_lastClickedId = logicalId;
        m_lastClickType = clickType;
        m_hasPendingClick = true;
    }
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
