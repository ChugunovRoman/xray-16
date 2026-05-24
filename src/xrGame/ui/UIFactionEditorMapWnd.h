#pragma once

#include "../spawn_smart_terrain_map_data_source.h"
#include "UIMapDataSource.h"
#include "xrUICore/Windows/UIWindow.h"

class CUIMapWnd;

class CUIFactionEditorMapWnd final : public CUIWindow
{
    using inherited = CUIWindow;

public:
    CUIFactionEditorMapWnd();
    ~CUIFactionEditorMapWnd() override = default;

    bool Init(pcstr xml_name, pcstr path);
    void SetSpawnName(pcstr spawn_name);
    void Reload();

    bool HasPendingClick() const { return m_hasPendingClick; }
    u32 GetLastClickedId() const { return m_lastClickedId; }
    u32 GetLastClickType() const { return static_cast<u32>(m_lastClickType); }
    void ClearPendingClick() { m_hasPendingClick = false; }

    pcstr GetPointLevelName(u32 logical_id) const;
    pcstr GetPointSmartName(u32 logical_id) const;
    pcstr GetPointSectionName(u32 logical_id) const;
    pcstr GetPointHintText(u32 logical_id) const;
    pcstr GetPointDisplayName(u32 logical_id) const;
    pcstr GetPointSmartType(u32 logical_id) const;
    pcstr GetPointOwnerFaction(u32 logical_id) const;
    pcstr GetPointIconTexture(u32 logical_id) const;

    void Draw() override;
    void Update() override;
    pcstr GetDebugType() override { return "CUIFactionEditorMapWnd"; }

private:
    pcstr GetPointString(u32 logical_id, shared_str SMapPointDesc::*field) const;
    void FocusDefaultTarget();

private:
    CUIMapWnd* m_mapWnd;
    CSpawnSmartTerrainMapDataSource m_spawnSource;
    u32 m_lastClickedId;
    EUiMapClick m_lastClickType;
    bool m_hasPendingClick;
    mutable shared_str m_cachedPointString;

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION(CUIWindow);
};
