#pragma once

#include <array>

#include "UIMapDataSource.h"
#include "xrUICore/Windows/UIWindow.h"
#include "xrUICore/Callbacks/UIWndCallback.h"

class CUICustomMap;
class CUIGlobalMap;
class CUIFrameWindow;
class CUIScrollBar;
class CUIFrameLineWnd;
class CMapActionPlanner;
class CUITabControl;
class CUIStatic;
class CUI3tButton;
class CUILevelMap;
class CUIMapLocationHint;
class CMapLocation;
class CMapSpot;
class CGameTask;
class CUIXml;
class UIHint;
class CUIPropertiesBox;

enum class EPdaMapLayer : u8
{
    Surface = 0,
    Underground,
    Count
};

constexpr size_t PDA_MAP_LAYER_COUNT = static_cast<size_t>(EPdaMapLayer::Count);

using GAME_MAPS = xr_map<shared_str, CUICustomMap*>;

class CUIMapWnd final : public CUIWindow, public CUIWndCallback
{
    typedef CUIWindow inherited;

private:
    struct SExternalMapSpot
    {
        SMapPointDesc desc;
        EPdaMapLayer layer = EPdaMapLayer::Surface;
        CUILevelMap* level_map = nullptr;
        CUIStatic* icon = nullptr;
        CUIStatic* leader_icon = nullptr;
        bool visible = false;
    };

    bool m_view_actor;
    Fvector2 m_prev_actor_pos;
    shared_str m_last_actor_level_name;
    bool m_force_viewport_reset;
    bool m_level_changed_since_last_show;

private:
    float m_map_move_step;

    float m_currentZoom;
    std::array<CUIGlobalMap*, PDA_MAP_LAYER_COUNT> m_GlobalMaps{};
    CUIGlobalMap* m_GlobalMap;
    std::array<GAME_MAPS, PDA_MAP_LAYER_COUNT> m_GameMaps;
    EPdaMapLayer m_activeLayer;

    CUIFrameWindow* m_UIMainFrame;
    bool m_scroll_mode;
    CUIScrollBar* m_UIMainScrollV;
    CUIScrollBar* m_UIMainScrollH;
    CUIWindow* m_UILevelFrame;
    CMapActionPlanner* m_ActionPlanner;
    CUIFrameLineWnd* m_UIMainMapHeader;
    CUIMapLocationHint* m_map_location_hint;
    CMapLocation* m_cur_location;
    IMapDataSource* m_externalDataSource;
    u32 m_externalDataRevision;
    xr_vector<SExternalMapSpot> m_externalSpots;
    u32 m_lastExternalClickedId;
    EUiMapClick m_lastExternalClick;
    u32 m_lastExternalClickModifiers;
    bool m_hasPendingExternalClick;
    bool m_pendingFocusActor{false};

#ifdef DEBUG
//	CUIStatic*					m_dbg_text_hint;
//	CUIStatic*					m_dbg_info;
#endif // DEBUG

    enum EBtnPos
    {
        btn_legend = 0,
        btn_up = 1,
        btn_zoom_more = 2,
        btn_left = 3,
        btn_actor = 4,
        btn_right = 5,
        btn_zoom_less = 6,
        btn_down = 7,
        btn_zoom_reset = 8,
        max_btn_nav = 9
    };
    CUI3tButton* m_btn_nav[max_btn_nav]{};
    CUIStatic* m_btn_nav_parent;
    CUI3tButton* m_btn_layer_surface;
    CUI3tButton* m_btn_layer_underground;
    u32 m_nav_timing;

    void UpdateNav();
    void UpdateLayerSwitcherState();

    void OnBtnLegend_Push(CUIWindow*, void*);
    void OnBtnUp_Push(CUIWindow*, void*);
    void OnBtnZoomMore_Push(CUIWindow*, void*);

    void OnBtnLeft_Push(CUIWindow*, void*);
    void OnBtnActor_Push(CUIWindow*, void*);
    void OnBtnRight_Push(CUIWindow*, void*);

    void OnBtnZoomLess_Push(CUIWindow*, void*);
    void OnBtnDown_Push(CUIWindow*, void*);
    void OnBtnZoomReset_Push(CUIWindow*, void*);
    void OnBtnLayerSurface_Push(CUIWindow*, void*);
    void OnBtnLayerUnderground_Push(CUIWindow*, void*);

private:
    void OnScrollV(CUIWindow*, void*);
    void OnScrollH(CUIWindow*, void*);

    void init_xml_layer_switcher(CUIXml& xml, pcstr start_from, bool critical);
    GAME_MAPS& GetGameMaps(EPdaMapLayer layer) { return m_GameMaps[static_cast<size_t>(layer)]; }
    const GAME_MAPS& GetGameMaps(EPdaMapLayer layer) const { return m_GameMaps[static_cast<size_t>(layer)]; }
    CUIGlobalMap* GetGlobalMap(EPdaMapLayer layer) const { return m_GlobalMaps[static_cast<size_t>(layer)]; }
    EPdaMapLayer ResolveLayerForGlobalMap(const CUIGlobalMap* global_map) const;
    CUICustomMap* FindLevelMap(const shared_str& map_name, EPdaMapLayer* layer = nullptr) const;
    bool SetActiveLayer(EPdaMapLayer layer, bool preserveViewport = true);
    void SyncActiveLayerVisibility(bool status);
    void UpdateActiveMapLayout();
    void RefreshLevelMapRects();
    bool CheckForActorLevelChange();
    void ResetMapStateForLevelChange();
    void ClearExternalSpots();
    void RefreshExternalDataSource();
    void RebuildExternalSpots();
    void UpdateExternalSpots();
    bool HandleExternalSpotMouse(float x, float y, EUIMessages mouse_action);
    pcstr ResolveExternalSpotTexture(const SMapPointDesc& point) const;
    bool GetExternalPointDescInternal(u32 logical_id, SMapPointDesc& out) const;
    void SyncLeaderOverlay(SExternalMapSpot& spot);
    void RefreshExternalSpotFlagsFromDataSource(u32 logical_id, SExternalMapSpot& spot);
    void DestroyLeaderIcon(SExternalMapSpot& spot);
    void AttachLeaderIcon(SExternalMapSpot& spot);
    bool ExternalSpotHitTest(const SExternalMapSpot& spot, const Fvector2& cursorPos) const;

    void ResetActionPlanner();

public:
    void ViewGlobalMap();
    void ViewActor();
    void ViewZoomIn();
    void ViewZoomOut();

    void MoveScrollV(float dy);
    void MoveScrollH(float dx);

    void ActivatePropertiesBox(CUIWindow* w);

public:
    CUICustomMap* m_tgtMap;
    Fvector2 m_tgtCenter;
    UIHint* hint_wnd;

protected:
    CUIPropertiesBox* m_UIPropertiesBox;

protected:
    void init_xml_nav(CUIXml& xml, pcstr start_from, bool critical);
    void ShowHint(bool extra = false);
    void Activated();

public:
    CUIMapWnd(UIHint* hint);
    virtual ~CUIMapWnd();

    virtual bool Init(cpcstr xml_name, cpcstr start_from, bool critical = true);
    virtual void Show(bool status);
    virtual void Draw();
    virtual void Reset();
    virtual void Update();
    void DrawHint();

    void MoveMap(Fvector2 const& pos_delta);
    float GetZoom() { return m_currentZoom; }
    void SetZoom(float value);
    bool UpdateZoom(bool b_zoom_in);

    void ShowHintStr(CUIWindow* parent, LPCSTR text);
    void ShowHintSpot(CMapSpot* spot);
    void ShowHintTask(CGameTask* task, CUIWindow* owner);

    void SpotSelected(CUIWindow* spot);

    void HideHint(CUIWindow* parent);
    void HideCurHint();
    void Hint(const shared_str& text);
    virtual bool OnMouseAction(float x, float y, EUIMessages mouse_action);
    virtual bool OnKeyboardAction(int dik, EUIMessages keyboard_action);
    bool OnControllerAction(int axis, const ControllerAxisState& state, EUIMessages controller_action) override;

    virtual void SendMessage(CUIWindow* pWnd, s16 msg, void* pData = NULL);

    void SetTargetMap(CUICustomMap* m, bool bZoomIn = false);
    void SetTargetMap(CUICustomMap* m, const Fvector2& pos, bool bZoomIn = false);
    void SetTargetMap(const shared_str& name, const Fvector2& pos, bool bZoomIn = false);
    void SetTargetMap(const shared_str& name, bool bZoomIn = false);

    void MapLocationRelcase(CMapLocation* ml);

    Frect ActiveMapRect()
    {
        Frect r;
        m_UILevelFrame->GetAbsoluteRect(r);
        return r;
    };
    void AddMapToRender(CUICustomMap*);
    void RemoveMapToRender(CUICustomMap*);
    CUIGlobalMap* GlobalMap() { return m_GlobalMap; }
    const GAME_MAPS& GameMaps() { return GetGameMaps(m_activeLayer); }
    const GAME_MAPS& GetMapsForLayer(EPdaMapLayer layer) const { return GetGameMaps(layer); }
    CUIGlobalMap* GetGlobalMapForLayer(EPdaMapLayer layer) const { return GetGlobalMap(layer); }
    CUICustomMap* GetLevelMap(const shared_str& map_name, EPdaMapLayer* layer = nullptr) const { return FindLevelMap(map_name, layer); }
    EPdaMapLayer ActiveLayer() const { return m_activeLayer; }
    bool ActivateLayer(EPdaMapLayer layer, bool preserveViewport = true) { return SetActiveLayer(layer, preserveViewport); }
    void SetExternalDataSource(IMapDataSource* source);
    void ClearExternalDataSource();
    bool UsingExternalDataSource() const { return m_externalDataSource != nullptr; }
    bool ConsumeExternalMapClick(u32& logical_id, EUiMapClick& click_type, u32& modifiers);
    bool GetExternalPointDesc(u32 logical_id, SMapPointDesc& out) const { return GetExternalPointDescInternal(logical_id, out); }
    bool SetExternalPointVisual(u32 logical_id, pcstr owner_faction, pcstr icon_texture);
    bool UpdateExternalSpotHint(u32 logical_id, pcstr hint_text);
    void SetPendingFocusActor(bool value) { m_pendingFocusActor = value; }
    void UpdateScroll();
    shared_str cName() const { return "ui_map_wnd"; }

    pcstr GetDebugType() override { return "CUIMapWnd"; }
};
