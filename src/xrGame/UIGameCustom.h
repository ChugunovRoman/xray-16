#pragma once

#include "Common/object_interfaces.h"
#include "inventory_space.h"
#include "gametype_chooser.h"
#include "UIDialogHolder.h"
#include "xrEngine/CustomHUD.h"
#include "script_game_object.h"
#include "xrCommon/xr_string.h"
#include "xrScriptEngine/script_wrapper_cache.hpp"

#include <luabind/back_reference_fwd.hpp>

// fwd. decl.
class CTeamBaseZone;
class game_cl_GameState;
class CUIDialogWnd;
class CUICaption;
class CUIStatic;
class CUIWindow;
class CUIXml;
class CUIActorMenu;
class CUIPdaWnd;
struct KillMessageStruct;
class CUIMainIngameWnd;
class CUIMessagesWindow;

class StaticDrawableWrapper : public IPureDestroyableObject
{
public:
    CUIStatic* m_static;
    float m_endTime;
    shared_str m_name;

    StaticDrawableWrapper();
    virtual void destroy();
    void Draw();
    void Update();
    CUIStatic* wnd() { return m_static; }
    bool IsActual() const;
    void SetText(const char* text);
};

struct MPLevelDesc
{
    shared_str map_name;
    shared_str map_ver;
    bool operator==(const MPLevelDesc& rhs) { return map_name == rhs.map_name && map_ver == rhs.map_ver; }
};

struct SGameTypeMaps
{
    shared_str m_game_type_name;
    EGameIDs m_game_type_id;
    xr_vector<MPLevelDesc> m_map_names;
};

struct MPWeatherDesc
{
    shared_str Name;
    shared_str StartTime;
};

class CMapListHelper
{
private:
    xr_vector<SGameTypeMaps> m_storage;
    xr_vector<MPWeatherDesc> m_weathers;

public:
    const SGameTypeMaps& GetMapListFor(const EGameIDs gameId);
    const xr_vector<MPWeatherDesc>& GetGameWeathers();

private:
    void Load();
    void LoadMapInfo(const char* cfgName, const xr_string& levelName, const char* levelVer = "1.0");
    SGameTypeMaps* GetMapListInt(const shared_str& gameType);
};

extern CMapListHelper gMapListHelper;

class CUIGameCustom : public FactoryObjectBase, public CDialogHolder, public CUIResetNotifier
{
protected:
    CUIWindow* Window;
    CUIXml* MsgConfig;
    xr_vector<StaticDrawableWrapper*> CustomStatics;
    CUIActorMenu* ActorMenu;
    CUIPdaWnd* PdaMenu;
    bool showGameIndicators;
    CUIStatic* m_pHudOverlay = nullptr; // g_3d_scopes 3: fullscreen composite of $user$hud_overlay

public:
    // XXX nitrocaster: make not public
    CUIMainIngameWnd* UIMainIngameWnd;
    CUIMessagesWindow* m_pMessagesWnd;

    CUIGameCustom();
    virtual ~CUIGameCustom();
    virtual void SetClGame(game_cl_GameState* gameState);
    virtual void OnInventoryAction(PIItem item, u16 actionType);
    virtual void Init(int stage) {}
    virtual void Render();
    virtual void OnFrame() override;
    virtual void OnUIReset() override;
    IC CUIActorMenu& GetActorMenu() const { return *ActorMenu; }
    IC CUIPdaWnd& GetPdaMenu() const { return *PdaMenu; }
    bool ShowActorMenu();
    void HideActorMenu();
    void UpdateActorMenu(); //Alundaio
    CScriptGameObject* CurrentItemAtCell(); //Alundaio
    bool ShowPdaMenu();
    void HidePdaMenu();
    void ShowMessagesWindow();
    void HideMessagesWindow();
    void ShowGameIndicators(bool show) { showGameIndicators = show; }
    bool GameIndicatorsShown() { return showGameIndicators; }
    void ShowCrosshair(bool show) { psHUD_Flags.set(HUD_CROSSHAIR_RT, show); }
    bool CrosshairShown() { return !!psHUD_Flags.test(HUD_CROSSHAIR_RT); }
    virtual void HideShownDialogs() {}
    virtual void ReinitDialogs() {}
    StaticDrawableWrapper* AddCustomStatic(const char* id, bool singleInstance, float ttlDefault = -1.0f);
    StaticDrawableWrapper* GetCustomStatic(const char* id);
    void RemoveCustomStatic(const char* id);
    void CommonMessageOut(const char* text);
    virtual void ChangeTotalMoneyIndicator(const char* newMoneyString) {}
    virtual void DisplayMoneyChange(const char* deltaMoney) {}
    virtual void DisplayMoneyBonus(KillMessageStruct* bonus) {}
    virtual void UnLoad();
    void Load();
    void OnConnected();
    void UpdatePda();
    void update_fake_indicators(u8 type, float power);
    void enable_fake_indicators(bool enable);

    pcstr GetDebugType() override { return "CUIGameCustom"; }
    bool FillDebugTree(const CUIDebugState& debugState) override;
    void FillDebugInfo() override;

    // The Lua wrapper cache: the one Lua wrapper of this game UI (get_hud(); luabind::get_back_reference below), released
    // in the destructor. Lives in the object: the UI of the next level / game - even at the same address and of
    // another type - never gets the wrapper of the old one.
    xray::script_wrapper_cache::Slot m_lua_wrapper;

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION(CDialogHolder);
};

// The Lua wrapper cache (xrScriptEngine/script_wrapper_cache.hpp): get_hud() and any other pass of the game UI to Lua
// by pointer give the cached wrapper. A Lua-derived UI (a
// luabind::wrap_base; UIGame_custom_script, not built now) would keep luabind's own path: its Lua object. Defined in
// UIGameCustom_script.cpp; declared with the class, so no translation unit instantiates the generic template.
namespace luabind
{
template <>
bool get_back_reference<CUIGameCustom*>(lua_State* L, CUIGameCustom* const& x);
} // namespace luabind

extern CUIGameCustom* CurrentGameUI();
