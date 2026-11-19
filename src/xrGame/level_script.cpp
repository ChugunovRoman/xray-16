////////////////////////////////////////////////////////////////////////////
//	Module 		: level_script.cpp
//	Created 	: 28.06.2004
//  Modified 	: 28.06.2004
//	Author		: Dmitriy Iassenev
//	Description : Level script export
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "Level.h"
#include "Actor.h"
#include "script_game_object.h"
#include "script_game_object_impl.h"
#include "xrAICore/Navigation/PatrolPath/patrol_path_storage.h"
#include "xrServer.h"
#include "client_spawn_manager.h"
#include "xrEngine/IGame_Persistent.h"
#include "game_cl_base.h"
#include "UIGameCustom.h"
#include "ui/UIDialogWnd.h"
#include "date_time.h"
#include "ai_space.h"
#include "xrAICore/Navigation/level_graph.h"
#include "xrAICore/Navigation/game_level_cross_table.h"
#include "xrPhysics/PHCommander.h"
#include "xrPhysics/PHScriptCall.h"
#include "xrScriptEngine/script_engine.hpp"
#include "game_cl_single.h"
#include "game_sv_single.h"
#include "map_manager.h"
#include "map_spot.h"
#include "map_location.h"
#include "alife_simulator.h"
#include "alife_time_manager.h"
#include "alife_time_skip.h"
#include "ui/UIGameTutorial.h"
#include "ui/UIInventoryUtilities.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "HUDManager.h"
#include "raypick.h"
#include "level_preview_model.h"
#include "level_world_draw.h"
#include "addon_api_world.h"
#include "addon_host.h"
#include "ui/UIMainIngameWnd.h"
#include "UIZoneMap.h"
#include "xrCDB/xr_collide_defs.h"
#include "xrNetServer/NET_Messages.h"

LPCSTR command_line() { return Core.Params; }
bool IsDynamicMusic() { return !!psActorFlags.test(AF_DYNAMIC_MUSIC); }
bool IsImportantSave() { return !!psActorFlags.test(AF_IMPORTANT_SAVE); }
#ifdef DEBUG
void check_object(CScriptGameObject* object)
{
    try
    {
        Msg("check_object %s", object->Name());
    }
    catch (...)
    {
        object = object;
    }
}

CScriptGameObject* tpfGetActor()
{
    static bool first_time = true;
    if (first_time)
        GEnv.ScriptEngine->script_log(LuaMessageType::Error, "Do not use level.actor function!");
    first_time = false;

    CActor* l_tpActor = smart_cast<CActor*>(Level().CurrentEntity());
    if (l_tpActor)
        return (smart_cast<CGameObject*>(l_tpActor)->lua_game_object());
    else
        return (0);
}

CScriptGameObject* get_object_by_name(LPCSTR caObjectName)
{
    static bool first_time = true;
    if (first_time)
        GEnv.ScriptEngine->script_log(LuaMessageType::Error, "Do not use level.object function!");
    first_time = false;

    CGameObject* l_tpGameObject = smart_cast<CGameObject*>(Level().Objects.FindObjectByName(caObjectName));
    if (l_tpGameObject)
        return (l_tpGameObject->lua_game_object());
    else
        return (0);
}
#endif

CScriptGameObject* get_object_by_id(u16 id)
{
    CGameObject* pGameObject = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
    if (!pGameObject)
        return NULL;

    return pGameObject->lua_game_object();
}

LPCSTR get_weather() { return g_pGamePersistent->Environment().GetWeather().c_str(); }
void set_weather(pcstr const weather_name, const bool forced)
{
    if (!Device.editor_mode())
        g_pGamePersistent->Environment().SetWeather(weather_name, forced);
}

bool set_weather_fx(pcstr const weather_name)
{
    if (!Device.editor_mode())
        return g_pGamePersistent->Environment().SetWeatherFX(weather_name);

    return false;
}

bool start_weather_fx_from_time(pcstr const weather_name, const float time)
{
    if (!Device.editor_mode())
        return g_pGamePersistent->Environment().StartWeatherFXFromTime(weather_name, time);

    return false;
}

bool is_wfx_playing() { return (g_pGamePersistent->Environment().IsWFXPlaying()); }
float get_wfx_time() { return (g_pGamePersistent->Environment().wfx_time); }
void stop_weather_fx() { g_pGamePersistent->Environment().StopWFX(); }
void set_time_factor(const float time_factor)
{
    if (!OnServer() || Device.editor_mode())
        return;

    Level().Server->GetGameState()->SetGameTimeFactor(time_factor);
}

float get_time_factor() { return (Level().GetGameTimeFactor()); }
void set_game_difficulty(ESingleGameDifficulty dif)
{
    g_SingleGameDifficulty = dif;
    game_cl_Single* game = smart_cast<game_cl_Single*>(Level().game);
    VERIFY(game);
    game->OnDifficultyChanged();
}
ESingleGameDifficulty get_game_difficulty() { return g_SingleGameDifficulty; }
u32 get_time_days()
{
    u32 year = 0, month = 0, day = 0, hours = 0, mins = 0, secs = 0, milisecs = 0;
    split_time((g_pGameLevel && Level().game) ? Level().GetGameTime() : ai().alife().time_manager().game_time(), year,
        month, day, hours, mins, secs, milisecs);
    return day;
}

u32 get_time_hours()
{
    u32 year = 0, month = 0, day = 0, hours = 0, mins = 0, secs = 0, milisecs = 0;
    split_time((g_pGameLevel && Level().game) ? Level().GetGameTime() : ai().alife().time_manager().game_time(), year,
        month, day, hours, mins, secs, milisecs);
    return hours;
}

u32 get_time_minutes()
{
    u32 year = 0, month = 0, day = 0, hours = 0, mins = 0, secs = 0, milisecs = 0;
    split_time((g_pGameLevel && Level().game) ? Level().GetGameTime() : ai().alife().time_manager().game_time(), year,
        month, day, hours, mins, secs, milisecs);
    return mins;
}

void change_game_time(u32 days, u32 hours, u32 mins)
{
    game_sv_Single* tpGame = smart_cast<game_sv_Single*>(Level().Server->GetGameState());
    if (tpGame && ai().get_alife())
    {
        u32 value = days * 86400 + hours * 3600 + mins * 60;
        float fValue = static_cast<float>(value);
        value *= 1000; // msec
        g_pGamePersistent->Environment().ChangeGameTime(fValue);
        tpGame->alife().time_manager().change_game_time(value);
        // plans/lua_to_cpp/55-timeskip-squad-movement.md: the offline movement catches up step by step
        alife_time_skip::Begin(tpGame->alife(), value);
    }
}

float high_cover_in_direction(u32 level_vertex_id, const Fvector& direction)
{
    float y, p;
    direction.getHP(y, p);
    return (ai().level_graph().high_cover_in_direction(y, level_vertex_id));
}

float low_cover_in_direction(u32 level_vertex_id, const Fvector& direction)
{
    float y, p;
    direction.getHP(y, p);
    return (ai().level_graph().low_cover_in_direction(y, level_vertex_id));
}

float rain_factor() { return (g_pGamePersistent->Environment().CurrentEnv.rain_density); }
u32 vertex_in_direction(u32 level_vertex_id, Fvector direction, float max_distance)
{
    if (!ai().level_graph().valid_vertex_id(level_vertex_id))
    {
#ifndef MASTER_GOLD
        Msg("! vertex_in_direction: invalid level_vertex_id=%u", level_vertex_id);
#endif
        return level_vertex_id;
    }
    direction.normalize_safe();
    direction.mul(max_distance);
    Fvector start_position = ai().level_graph().vertex_position(level_vertex_id);
    Fvector finish_position = Fvector(start_position).add(direction);
    u32 result = u32(-1);
    ai().level_graph().farthest_vertex_in_direction(level_vertex_id, start_position, finish_position, result, 0);
    return (ai().level_graph().valid_vertex_id(result) ? result : level_vertex_id);
}

Fvector vertex_position(u32 level_vertex_id)
{
    if (!ai().level_graph().valid_vertex_id(level_vertex_id))
    {
#ifndef MASTER_GOLD
        Msg("! level.vertex_position: invalid level_vertex_id=%u", level_vertex_id);
#endif
        return Fvector().set(0.f, 0.f, 0.f);
    }
    return ai().level_graph().vertex_position(level_vertex_id);
}

// RS4 (plans/addon_runtime_smarts): the game vertex of a level vertex of the current level by the cross table (the
// nearest vertex of the game graph), as a server object placed there gets it. GameGraph::_GRAPH_ID(-1) (65535,
// MAX_U16_TYPE_SIZE of Lua) without a level graph / cross table or for an invalid level vertex
GameGraph::_GRAPH_ID game_vertex_id(u32 level_vertex_id)
{
    if (!ai().get_level_graph() || !ai().get_cross_table())
    {
#ifndef MASTER_GOLD
        Msg("! level.game_vertex_id: no level graph or cross table (level_vertex_id=%u)", level_vertex_id);
#endif
        return GameGraph::_GRAPH_ID(-1);
    }
    if (!ai().level_graph().valid_vertex_id(level_vertex_id))
    {
#ifndef MASTER_GOLD
        Msg("! level.game_vertex_id: invalid level_vertex_id=%u", level_vertex_id);
#endif
        return GameGraph::_GRAPH_ID(-1);
    }
    return ai().cross_table().vertex(level_vertex_id).game_vertex_id();
}

ENGINE_API extern float psHUD_FOV;

// RS10 (plans/addon_runtime_smarts, the labels of the smart mode): a point of the world on the screen, in the UI space
// 1024 x 768 (the UI is stretched to the whole screen at any aspect, 16:9 too - as the cursor, CUICursor). hud - the
// projection of the HUD (FOV of the weapon model: hud_fov * fov) instead of the one of the camera; allow_offscreen -
// a point in front of the camera but out of the screen gets its coordinates too (out of 0..1024 / 0..768).
// x = y = -9999 - the point is behind the camera (or out of the screen without allow_offscreen). The math is on the CPU
// by the matrices of the last frame (Device.mFullTransform), the same for DX11 and GL. As world2ui of Anomaly and
// IX-Ray (level_script.cpp, World2Ui) - the same call and the same -9999
Fvector2 world_to_ui(const Fvector& pos, bool hud, bool allow_offscreen)
{
    constexpr float NO_POINT = -9999.f;
    Fvector2 res;
    res.set(NO_POINT, NO_POINT);
    Fmatrix xform;
    if (hud)
    {
        Fmatrix project;
        project.build_projection(deg2rad(psHUD_FOV * Device.fFOV), Device.fASPECT, HUD_VIEWPORT_NEAR,
            g_pGamePersistent->Environment().CurrentEnv.far_plane);
        xform.mul(project, Device.mView);
    }
    else
        xform.set(Device.mFullTransform);

    // w - the depth of the point in the view space (the projection of build_projection: _34 = 1, _44 = 0)
    const float w = pos.x * xform._14 + pos.y * xform._24 + pos.z * xform._34 + xform._44;
    if (w <= EPS_S)
        return res;
    const float x = (pos.x * xform._11 + pos.y * xform._21 + pos.z * xform._31 + xform._41) / w;
    const float y = (pos.x * xform._12 + pos.y * xform._22 + pos.z * xform._32 + xform._42) / w;
    if (!allow_offscreen && (_abs(x) > 1.f || _abs(y) > 1.f))
        return res;
    res.set((1.f + x) * 0.5f * UI_BASE_WIDTH, (1.f - y) * 0.5f * UI_BASE_HEIGHT);
    return res;
}

// RS11 (plans/addon_runtime_smarts): level.draw_polyline - the points of a Lua array {vector, ...} (1..n, in order)
bool draw_polyline(pcstr id, const luabind::object& points, bool closed, u32 color)
{
    if (luabind::type(points) != LUA_TTABLE)
    {
        Msg("! [world_draw] draw_polyline '%s': the points are not a table", id ? id : "");
        return false;
    }
    lua_State* L = points.interpreter();
    points.push(L);
    const int table = lua_gettop(L);
    const int count = int(lua_objlen(L, table));
    xr_vector<Fvector> list;
    list.reserve(_min(count, int(gw::world_draw::kMaxPoints) + 1));
    int bad = 0;
    // more than kMaxPoints + 1 are not read: Polyline refuses them anyway
    for (int i = 1; i <= count && i <= int(gw::world_draw::kMaxPoints) + 1 && !bad; ++i)
    {
        lua_rawgeti(L, table, i);
        const luabind::object value(luabind::from_stack(L, -1));
        if (const auto* p = luabind::object_cast_nothrow<const Fvector*>(value, static_cast<const Fvector*>(nullptr)))
            list.push_back(*p);
        else
            bad = i;
        lua_pop(L, 1);
    }
    lua_settop(L, table - 1);
    if (bad)
    {
        Msg("! [world_draw] draw_polyline '%s': the point %d is not a vector", id ? id : "", bad);
        return false;
    }
    return gw::world_draw::Polyline(id, list, closed, color);
}

void map_add_object_spot(u16 id, LPCSTR spot_type, LPCSTR text)
{
    CMapLocation* ml = Level().MapManager().AddMapLocation(spot_type, id);
    if (xr_strlen(text))
        ml->SetHint(text);
}
void map_set_object_spot(u16 id, LPCSTR spot_type, LPCSTR text)
{
    CMapLocation* ml = Level().MapManager().SetMapLocation(spot_type, id);
    if (xr_strlen(text))
        ml->SetHint(text);
}
void map_change_spot_size(u16 id, LPCSTR spot_type, float width, float height)
{
    CMapLocation* ml = Level().MapManager().GetMapLocation(spot_type, id);
    if (!ml)
        return;
    ml->SetSize(width, height);
}
void map_change_spot_texture(u16 id, LPCSTR spot_type, LPCSTR path)
{
    CMapLocation* ml = Level().MapManager().GetMapLocation(spot_type, id);
    if (!ml)
        return;
    ml->SetTexture(path);
}
void map_change_spot_texture_rect(u16 id, LPCSTR spot_type, Frect rect)
{
    CMapLocation* ml = Level().MapManager().GetMapLocation(spot_type, id);
    if (!ml)
        return;
    ml->SetTextureRect(rect);
}
void map_change_spot_texture_offset(u16 id, LPCSTR spot_type, float x, float y)
{
    CMapLocation* ml = Level().MapManager().GetMapLocation(spot_type, id);
    if (!ml)
        return;
    ml->SetTextureOffset(x, y);
}
void map_change_spot_texture_offset_x(u16 id, LPCSTR spot_type, float x)
{
    CMapLocation* ml = Level().MapManager().GetMapLocation(spot_type, id);
    if (!ml)
        return;
    ml->SetTextureOffsetX(x);
}
void map_change_spot_texture_offset_y(u16 id, LPCSTR spot_type, float y)
{
    CMapLocation* ml = Level().MapManager().GetMapLocation(spot_type, id);
    if (!ml)
        return;
    ml->SetTextureOffsetY(y);
}
void map_change_spot_color(u16 id, LPCSTR spot_type, u32 color)
{
    CMapLocation* ml = Level().MapManager().GetMapLocation(spot_type, id);
    if (!ml)
        return;
    ml->SetColor(color);
    CMiniMapSpot* mini_spot = ml->MiniMapSpot();
    if (mini_spot)
        mini_spot->SetTextureColor(color);
}

void map_add_object_spot_ser(u16 id, LPCSTR spot_type, LPCSTR text)
{
    CMapLocation* ml = Level().MapManager().AddMapLocation(spot_type, id);
    if (xr_strlen(text))
        ml->SetHint(text);

    ml->SetSerializable(true);
}

void map_change_spot_hint(u16 id, LPCSTR spot_type, LPCSTR text)
{
    CMapLocation* ml = Level().MapManager().GetMapLocation(spot_type, id);
    if (!ml)
        return;
    ml->SetHint(text);
}

// H2 (plans/lua_to_cpp/36-gw-eoc-e8-spots.md): the hint of the spot is built by the script function `function`
// ("module.function", called with the object id, answers a string) when the hint is shown, not when the spot changes.
// An empty name removes the provider. false - the object has no such spot. Not saved with the spot.
bool map_set_spot_hint_provider(u16 id, LPCSTR spot_type, LPCSTR function)
{
    CMapLocation* ml = Level().MapManager().GetMapLocation(spot_type, id);
    if (!ml)
        return false;
    ml->SetHintProvider(function);
    return true;
}

void map_remove_object_spot(u16 id, LPCSTR spot_type) { Level().MapManager().RemoveMapLocation(spot_type, id); }
u16 map_has_object_spot(u16 id, LPCSTR spot_type) { return Level().MapManager().HasMapLocation(spot_type, id); }
bool patrol_path_exists(LPCSTR patrol_path) { return (!!ai().patrol_paths().path(patrol_path, true)); }
void prefetch_sound(LPCSTR name) { Level().PrefetchSound(name); }
CClientSpawnManager& get_client_spawn_manager() { return (Level().client_spawn_manager()); }

void start_stop_menu(CUIDialogWnd* pDialog, bool bDoHideIndicators)
{
    if (pDialog->IsShown())
        pDialog->HideDialog();
    else
        pDialog->ShowDialog(bDoHideIndicators);
}

void add_dialog_to_render(CUIDialogWnd* pDialog) { CurrentGameUI()->AddDialogToRender(pDialog); }
void remove_dialog_to_render(CUIDialogWnd* pDialog) { CurrentGameUI()->RemoveDialogToRender(pDialog); }

CUIDialogWnd* main_input_receiver()
{
    return HUD().GetGameUI()->TopInputReceiver();
}

void hide_indicators()
{
    if (CurrentGameUI())
    {
        CurrentGameUI()->HideShownDialogs();
        CurrentGameUI()->ShowGameIndicators(false);
        CurrentGameUI()->ShowCrosshair(false);
    }
    psActorFlags.set(AF_GODMODE_RT, TRUE);
}

void hide_indicators_safe()
{
    if (CurrentGameUI())
    {
        CurrentGameUI()->ShowGameIndicators(false);
        CurrentGameUI()->ShowCrosshair(false);

        CurrentGameUI()->OnExternalHideIndicators();
    }
    psActorFlags.set(AF_GODMODE_RT, TRUE);
}

void show_indicators()
{
    if (CurrentGameUI())
    {
        CurrentGameUI()->ShowGameIndicators(true);
        CurrentGameUI()->ShowCrosshair(true);
    }
    psActorFlags.set(AF_GODMODE_RT, FALSE);
}

void show_weapon(bool b)
{
    if (psActorFlags.test(AF_GODMODE) && Actor())
        return;

    psHUD_Flags.set(HUD_WEAPON_RT2, b);
}
bool is_level_present() { return (!!g_pGameLevel); }
void add_call(const luabind::functor<bool>& condition, const luabind::functor<void>& action)
{
    luabind::functor<bool> _condition = condition;
    luabind::functor<void> _action = action;
    CPHScriptCondition* c = xr_new<CPHScriptCondition>(_condition);
    CPHScriptAction* a = xr_new<CPHScriptAction>(_action);
    Level().ph_commander_scripts().add_call(c, a);
}

void remove_call(const luabind::functor<bool>& condition, const luabind::functor<void>& action)
{
    CPHScriptCondition c(condition);
    CPHScriptAction a(action);
    Level().ph_commander_scripts().remove_call(&c, &a);
}

void add_call(const luabind::object& lua_object, LPCSTR condition, LPCSTR action)
{
    luabind::functor<bool> _condition = luabind::object_cast<luabind::functor<bool>>(lua_object[condition]);
    luabind::functor<void> _action = luabind::object_cast<luabind::functor<void>>(lua_object[action]);
    CPHScriptObjectConditionN* c = xr_new<CPHScriptObjectConditionN>(lua_object, _condition);
    CPHScriptObjectActionN* a = xr_new<CPHScriptObjectActionN>(lua_object, _action);
    Level().ph_commander_scripts().add_call_unique(c, c, a, a);
}

void remove_call(const luabind::object& lua_object, LPCSTR condition, LPCSTR action)
{
    CPHScriptObjectCondition c(lua_object, condition);
    CPHScriptObjectAction a(lua_object, action);
    Level().ph_commander_scripts().remove_call(&c, &a);
}

void add_call(
    const luabind::object& lua_object, const luabind::functor<bool>& condition, const luabind::functor<void>& action)
{
    CPHScriptObjectConditionN* c = xr_new<CPHScriptObjectConditionN>(lua_object, condition);
    CPHScriptObjectActionN* a = xr_new<CPHScriptObjectActionN>(lua_object, action);
    Level().ph_commander_scripts().add_call(c, a);
}

void remove_call(
    const luabind::object& lua_object, const luabind::functor<bool>& condition, const luabind::functor<void>& action)
{
    CPHScriptObjectConditionN c(lua_object, condition);
    CPHScriptObjectActionN a(lua_object, action);
    Level().ph_commander_scripts().remove_call(&c, &a);
}

void remove_calls_for_object(const luabind::object& lua_object)
{
    CPHSriptReqObjComparer c(lua_object);
    Level().ph_commander_scripts().remove_calls(&c);
}

CEnvironment* environment() { return (g_pGamePersistent->pEnvironment); }
CEnvDescriptor* current_environment(CEnvironment* self) { return &self->CurrentEnv; }
extern bool g_bDisableAllInput;
void disable_input()
{
    if (psActorFlags.test(AF_GODMODE) && Actor())
        return;

    g_bDisableAllInput = true;
#ifdef DEBUG
    Msg("input disabled");
#endif // #ifdef DEBUG
}
void enable_input()
{
    g_bDisableAllInput = false;
#ifdef DEBUG
    Msg("input enabled");
#endif // #ifdef DEBUG
}

void spawn_phantom(const Fvector& position) { Level().spawn_item("m_phantom", position, u32(-1), u16(-1), false); }
Fbox get_bounding_volume() { return Level().ObjectSpace.GetBoundingVolume(); }
void iterate_sounds(LPCSTR prefix, u32 max_count, const CScriptCallbackEx<void>& callback)
{
    for (int j = 0, N = _GetItemCount(prefix); j < N; ++j)
    {
        string_path fn, s;
        pstr S = (pstr)&s;
        _GetItem(prefix, j, s);
        if (FS.exist(fn, "$game_sounds$", S, ".ogg"))
            callback(prefix);

        for (u32 i = 0; i < max_count; ++i)
        {
            string_path name;
            xr_sprintf(name, "%s%d", S, i);
            if (FS.exist(fn, "$game_sounds$", name, ".ogg"))
                callback(name);
        }
    }
}

void iterate_sounds1(LPCSTR prefix, u32 max_count, luabind::functor<void> functor)
{
    CScriptCallbackEx<void> temp;
    temp.set(functor);
    iterate_sounds(prefix, max_count, temp);
}

void iterate_sounds2(LPCSTR prefix, u32 max_count, luabind::object object, luabind::functor<void> functor)
{
    CScriptCallbackEx<void> temp;
    temp.set(functor, object);
    iterate_sounds(prefix, max_count, temp);
}

#include "ActorEffector.h"
float add_cam_effector(LPCSTR fn, int id, bool cyclic, LPCSTR cb_func)
{
    CAnimatorCamEffectorScriptCB* e = xr_new<CAnimatorCamEffectorScriptCB>(cb_func);
    e->SetType((ECamEffectorType)id);
    e->SetCyclic(cyclic);
    e->Start(fn);
    Actor()->Cameras().AddCamEffector(e);
    return e->GetAnimatorLength();
}

float add_cam_effector2(LPCSTR fn, int id, bool cyclic, LPCSTR cb_func, float cam_fov)
{
    CAnimatorCamEffectorScriptCB* e = xr_new<CAnimatorCamEffectorScriptCB>(cb_func);
    e->m_bAbsolutePositioning = true;
    e->m_fov = cam_fov;
    e->SetType((ECamEffectorType)id);
    e->SetCyclic(cyclic);
    e->Start(fn);
    Actor()->Cameras().AddCamEffector(e);
    return e->GetAnimatorLength();
}

void remove_cam_effector(int id) { Actor()->Cameras().RemoveCamEffector((ECamEffectorType)id); }
float get_snd_volume() { return psSoundVFactor; }
void set_snd_volume(float v)
{
    psSoundVFactor = v;
    clamp(psSoundVFactor, 0.0f, 1.0f);
}

#include "level_sounds.h"
// Attenuation factor (0..1) of the level ambient music (game.ltx music_tracks). It is applied on top of the
// snd_volume_music option, so scripts never have to rewrite the player's setting.
float get_music_volume()
{
    if (!g_pGameLevel)
        return 1.0f;
    const CLevelSoundManager* mgr = Level().level_sound_manager();
    return mgr ? mgr->GetMusicFactor() : 1.0f;
}

void set_music_volume(float v)
{
    if (!g_pGameLevel)
        return;
    if (CLevelSoundManager* mgr = Level().level_sound_manager())
        mgr->SetMusicFactor(v);
}
#include "actor_statistic_mgr.h"
void add_actor_points(LPCSTR sect, LPCSTR detail_key, int cnt, int pts)
{
    return Actor()->StatisticMgr().AddPoints(sect, detail_key, cnt, pts);
}

void add_actor_points_str(LPCSTR sect, LPCSTR detail_key, LPCSTR str_value)
{
    return Actor()->StatisticMgr().AddPoints(sect, detail_key, str_value);
}

int get_actor_points(LPCSTR sect) { return Actor()->StatisticMgr().GetSectionPoints(sect); }
#include "ActorEffector.h"
void add_complex_effector(LPCSTR section, int id) { AddEffector(Actor(), id, section); }
void remove_complex_effector(int id) { RemoveEffector(Actor(), id); }
#include "PostprocessAnimator.h"
void add_pp_effector(LPCSTR fn, int id, bool cyclic)
{
    CPostprocessAnimator* pp = xr_new<CPostprocessAnimator>(id, cyclic);
    pp->Load(fn);
    Actor()->Cameras().AddPPEffector(pp);
}

void remove_pp_effector(int id)
{
    CPostprocessAnimator* pp = smart_cast<CPostprocessAnimator*>(Actor()->Cameras().GetPPEffector((EEffectorPPType)id));

    if (pp)
        pp->Stop(1.0f);
}

void set_pp_effector_factor(int id, float f, float f_sp)
{
    CPostprocessAnimator* pp = smart_cast<CPostprocessAnimator*>(Actor()->Cameras().GetPPEffector((EEffectorPPType)id));

    if (pp)
        pp->SetDesiredFactor(f, f_sp);
}

void set_pp_effector_factor2(int id, float f)
{
    CPostprocessAnimator* pp = smart_cast<CPostprocessAnimator*>(Actor()->Cameras().GetPPEffector((EEffectorPPType)id));

    if (pp)
        pp->SetCurrentFactor(f);
}

#include "relation_registry.h"

int g_community_goodwill(LPCSTR _community, int _entity_id)
{
    CHARACTER_COMMUNITY c;
    c.set(_community);

    return RELATION_REGISTRY().GetCommunityGoodwill(c.index(), u16(_entity_id));
}

void g_set_community_goodwill(LPCSTR _community, int _entity_id, int val)
{
    CHARACTER_COMMUNITY c;
    c.set(_community);
    RELATION_REGISTRY().SetCommunityGoodwill(c.index(), u16(_entity_id), val);
}

void g_change_community_goodwill(LPCSTR _community, int _entity_id, int val)
{
    CHARACTER_COMMUNITY c;
    c.set(_community);
    RELATION_REGISTRY().ChangeCommunityGoodwill(c.index(), u16(_entity_id), val);
}

int g_get_community_relation(LPCSTR comm_from, LPCSTR comm_to)
{
    CHARACTER_COMMUNITY community_from;
    community_from.set(comm_from);
    CHARACTER_COMMUNITY community_to;
    community_to.set(comm_to);

    return RELATION_REGISTRY().GetCommunityRelation(community_from.index(), community_to.index());
}

int g_get_default_community_relation(LPCSTR comm_from, LPCSTR comm_to)
{
    CHARACTER_COMMUNITY community_from;
    community_from.set(comm_from);
    CHARACTER_COMMUNITY community_to;
    community_to.set(comm_to);

    return RELATION_REGISTRY().GetDefaultCommunityRelation(community_from.index(), community_to.index());
}
void g_reset_relations_to_default()
{
    return RELATION_REGISTRY().ResetRelationsToDefault();
}

void g_set_community_relation(LPCSTR comm_from, LPCSTR comm_to, int value)
{
    CHARACTER_COMMUNITY community_from;
    community_from.set(comm_from);
    CHARACTER_COMMUNITY community_to;
    community_to.set(comm_to);

    RELATION_REGISTRY().SetCommunityRelation(community_from.index(), community_to.index(), value);
}

int g_get_general_goodwill_between(u16 from, u16 to)
{
    CHARACTER_GOODWILL presonal_goodwill = RELATION_REGISTRY().GetGoodwill(from, to);
    VERIFY(presonal_goodwill != NO_GOODWILL);

    CSE_ALifeTraderAbstract* from_obj = smart_cast<CSE_ALifeTraderAbstract*>(ai().alife().objects().object(from, true));
    CSE_ALifeTraderAbstract* to_obj = smart_cast<CSE_ALifeTraderAbstract*>(ai().alife().objects().object(to, true));

    if (!from_obj || !to_obj)
    {
        GEnv.ScriptEngine->script_log(LuaMessageType::Error,
            "RELATION_REGISTRY::get_general_goodwill_between  : cannot convert obj to CSE_ALifeTraderAbstract!");
        return (0);
    }
    CHARACTER_GOODWILL community_to_obj_goodwill = RELATION_REGISTRY().GetCommunityGoodwill(from_obj->Community(), to);
    CHARACTER_GOODWILL community_to_community_goodwill =
        RELATION_REGISTRY().GetCommunityRelation(from_obj->Community(), to_obj->Community());

    return presonal_goodwill + community_to_obj_goodwill + community_to_community_goodwill;
}

luabind::object get_sections(lua_State* L, ESectionTypeName type) {
    R_ASSERT2(type < ESectionTypeName::latest, make_string("Invalid type of getting section, passed type=[%d], latest type in enum=[%d]", type, ESectionTypeName::latest));

    luabind::object result = luabind::newtable(L);
    std::size_t index = 1;

    for (const auto& name : Dbg.GetSections(type))
        result[index++] = name;

    return result;
}

u32 render_get_dx_level() { return GEnv.Render->get_dx_level(); }

// engine gaps P47 (plans/addon_api_ideas/spec.md): level.objects_in_radius(pos, radius[, clsid[, with_inventory]]) -
// a table {1..n} of the online game objects within radius of pos, nearest first (by the position of the object).
// clsid: a value of the clsid table (obj:clsid()), nil or -1 - any; with_inventory: true - also the items inside an
// owner (their position is the owner's). The search: gw::addons::LevelObjectsInRadius (addon_api_level.cpp)
static luabind::object objects_in_radius(lua_State* L, const Fvector& position, float radius, int script_clsid,
    bool with_inventory)
{
    luabind::object result = luabind::newtable(L);
    if (!g_pGameLevel)
        return result;
    xr_vector<u16> ids;
    gw::addons::LevelObjectsInRadius(position, radius, 0, script_clsid, with_inventory, ids);
    int index = 1;
    for (const u16 id : ids)
    {
        CGameObject* object = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
        if (object)
            result[index++] = object->lua_game_object();
    }
    return result;
}

// The framing of the next UI previews of models (level.ui_preview_thumb / level.ui_preview_live_set): the pan of the
// camera along its axes and its distance to the model, added to the framing of the engine (the camera at the center of
// the model box, distance by its radius). Part of the cache key of the preview
static Fvector g_ui_preview_pan{};
static float g_ui_preview_distance_offset = 0.f;
// The aspect (width / height) of the frame of the next previews; 0 or less - a square frame
static float g_ui_preview_frame_aspect = 0.f;
// The fit framing of the next previews (level.ui_preview_fit): 0 - the engine framing by the box radius, > 0 - the top
// part of the model box of this fraction of its height is fitted in the frame (SPreviewSceneSettings::frame_fit)
static float g_ui_preview_fit = 0.f;

// The settings of a UI preview of a model (level.ui_preview_*): the camera turned by yaw / pitch around the model
// (framed by its box), a square texture of size texels
static SPreviewSceneSettings ui_preview_settings(float yaw, float pitch, u32 size)
{
    SPreviewSceneSettings settings;
    settings.yaw_deg = yaw;
    settings.pitch_deg = pitch;
    clamp<u32>(size, 32, 512);
    settings.frame_width = size;
    settings.frame_height = size;
    // a non-square frame: width / height = frame_aspect of ui_preview_framing (0 or less - a square one)
    if (g_ui_preview_frame_aspect > 0.f)
    {
        // clamp() of xrCommon/math_funcs_inline.h takes its first argument by non-const reference and returns void:
        // the value is clamped in place, a temporary cannot be passed to it
        float frame_height = float(size) / g_ui_preview_frame_aspect;
        clamp(frame_height, 32.f, 512.f);
        settings.frame_height = u32(frame_height + 0.5f); // rounded: the addons compute the same height
    }
    settings.offset = g_ui_preview_pan;
    settings.camera_distance_offset = g_ui_preview_distance_offset;
    settings.frame_fit = g_ui_preview_fit;
    return settings;
}
CUISequencer* g_tutorial = NULL;
CUISequencer* g_tutorial2 = NULL;

void start_tutorial(LPCSTR name)
{
    // Skip any game tutorial on load screen
    if (load_screen_renderer.IsActive())
        return;

    if (g_tutorial)
    {
        VERIFY(!g_tutorial2);
        g_tutorial2 = g_tutorial;
    };

    g_tutorial = xr_new<CUISequencer>();
    g_tutorial->Start(name);
    if (g_tutorial2)
        g_tutorial->m_pStoredInputReceiver = g_tutorial2->m_pStoredInputReceiver;
}

void stop_tutorial()
{
    if (g_tutorial)
        g_tutorial->Stop();
}

LPCSTR translate_string(LPCSTR str) { return StringTable().translate(str).c_str(); }
void replace_translate_string(LPCSTR str_id, LPCSTR new_str) { StringTable().replace(str_id, new_str); }
void register_string_token(LPCSTR key, LPCSTR value) { StringTable().register_token(key, value); }
void unregister_string_token(LPCSTR key) { StringTable().unregister_token(key); }
LPCSTR apply_string_tokens(LPCSTR str) { return StringTable().apply_tokens(str).c_str(); }
bool has_active_tutotial() { return (g_tutorial != NULL); }

// Alundaio: namespace level exports extension
// ability to update level netpacket
void g_send(NET_Packet& P, bool bReliable = false, bool bSequential = true, bool bHighPriority = false, bool bSendImmediately = false)
{
    Level().Send(P, net_flags(bReliable, bSequential, bHighPriority, bSendImmediately));
}

//can spawn entities like bolts, phantoms, ammo, etc. which normally crash when using alife():create()
void spawn_section(pcstr sSection, Fvector3 vPosition, u32 LevelVertexID, u16 ParentID, bool bReturnItem = false)
{
    Level().spawn_item(sSection, vPosition, LevelVertexID, ParentID, bReturnItem);
}

//ability to get the target game_object at crosshair
CScriptGameObject* g_get_target_obj()
{
    collide::rq_result& RQ = HUD().GetCurrentRayQuery();
    if (RQ.O)
    {
        CGameObject* game_object = smart_cast<CGameObject*>(RQ.O);
        if (game_object)
            return game_object->lua_game_object();
    }
    return nullptr;
}

float g_get_target_dist()
{
    collide::rq_result& RQ = HUD().GetCurrentRayQuery();
    return RQ.range;
}

u32 g_get_target_element()
{
	collide::rq_result& RQ = HUD().GetCurrentRayQuery();
	if (RQ.element)
		return RQ.element;

	return 0;
}

u8 get_active_cam()
{
    CActor* actor = smart_cast<CActor*>(Level().CurrentViewEntity());
    if (actor)
        return (u8)actor->active_cam();

    return 255;
}

void set_active_cam(u8 mode)
{
    CActor* actor = smart_cast<CActor*>(Level().CurrentViewEntity());
    if (actor && mode <= eacMaxCam)
        actor->cam_Set((EActorCameras)mode);
}
//-Alundaio

void iterate_online_objects(luabind::functor<bool> functor)
{
    for (u16 i = 0; i < 0xffff; ++i)
    {
        CGameObject* pGameObject = smart_cast<CGameObject*>(Level().Objects.net_Find(i));
        if (pGameObject && functor(pGameObject->lua_game_object()))
            return;
    }
}

// KD: raypick
bool ray_pick(const Fvector& start, const Fvector& dir, float range,
              collide::rq_target tgt, script_rq_result& script_R,
              CScriptGameObject* ignore_object)
{
    collide::rq_result R;
    IGameObject* ignore = nullptr;
    if (ignore_object)
        ignore = smart_cast<IGameObject*>(&(ignore_object->object()));
    if (Level().ObjectSpace.RayPick(start, dir, range, tgt, R, ignore))
    {
        script_R.set(R, start, dir); // D2 (plans/addon_base_building): with the normal and the material
        return true;
    }
    return false;
}

// Graff46
void jump_to_level(const Fvector& m_position, u32 m_level_vertex_id, GameGraph::_GRAPH_ID m_game_vertex_id, const Fvector& m_angles)
{
    NET_Packet p;
    p.w_begin(M_CHANGE_LEVEL);
    p.w(&m_game_vertex_id, sizeof(m_game_vertex_id));
    p.w(&m_level_vertex_id, sizeof(m_level_vertex_id));
    p.w_vec3(m_position);
    p.w_vec3(m_angles);
    Level().Send(p, net_flags(TRUE));
}

// XXX nitrocaster: one can export enum like class, without defining dummy type
template<typename T>
struct EnumCallbackType {};

void CLevel::script_register(lua_State* luaState)
{
    using namespace luabind;
    using namespace luabind::policy;

    class_<CEnvDescriptor>("CEnvDescriptor")
        .def_readonly("fog_density", &CEnvDescriptor::fog_density)
        .def_readonly("far_plane", &CEnvDescriptor::far_plane),

    class_<CEnvironment>("CEnvironment")
        .def("current", current_environment);

    module(luaState)
    [
        class_<CRayPick>("ray_pick")
            .def(constructor<>())
            .def(constructor<Fvector&, Fvector&, float, collide::rq_target, CScriptGameObject*>())
            .def("set_position", &CRayPick::set_position)
            .def("set_direction", &CRayPick::set_direction)
            .def("set_range", &CRayPick::set_range)
            .def("set_flags", &CRayPick::set_flags)
            .def("set_ignore_object", &CRayPick::set_ignore_object)
            .def("query", &CRayPick::query)
            .def("get_result", &CRayPick::get_result)
            .def("get_object", &CRayPick::get_object)
            .def("get_distance", &CRayPick::get_distance)
            .def("get_element", &CRayPick::get_element),
        class_<script_rq_result>("rq_result")
            .def_readonly("object", &script_rq_result::O)
            .def_readonly("range", &script_rq_result::range)
            .def_readonly("element", &script_rq_result::element)
            // D2 (plans/addon_base_building/plan.md 10.2): the surface of the hit - the normal (static geometry: of
            // the triangle, facing the ray; a bone: the face of its shape; else -dir), the game material (index, name)
            .def_readonly("normal", &script_rq_result::normal)
            .def_readonly("material_id", &script_rq_result::material_id)
            .property("material", &script_rq_result::get_material)
            .def(constructor<>()),
        class_<EnumCallbackType<collide::rq_target>>("rq_target")
            .enum_("targets")
            [
                value("rqtNone", int(collide::rqtNone)),
                value("rqtObject", int(collide::rqtObject)),
                value("rqtStatic", int(collide::rqtStatic)),
                value("rqtShape", int(collide::rqtShape)),
                value("rqtObstacle", int(collide::rqtObstacle)),
                value("rqtBoth", int(collide::rqtBoth)),
                value("rqtDyn", int(collide::rqtDyn))
            ],
        class_<ESingleGameDifficulty>("game_difficulty")
            .enum_("game_difficulty")
            [
                value("novice",  int(egdNovice)),
                value("stalker", int(egdStalker)),
                value("veteran", int(egdVeteran)),
                value("master",  int(egdMaster))
            ]
    ];

    module(luaState, "level")
    [
        // X-Ray Extensions:
        def("get_target_obj", &g_get_target_obj),
        def("get_target_dist", &g_get_target_dist),
        def("get_target_element", &g_get_target_element), // Can get bone cursor is targeting
        def("get_actor_body_state", +[]() -> u32
        {
            if (!g_actor)
                return 0;
            return g_actor->GetBodyState();
        }),
        def("get_actor_body_state_wishful", +[]() -> u32
        {
            if (!g_actor)
                return 0;
            return g_actor->GetWishfulBodyState();
        }),
        def("get_fov", +[]
        {
            return g_fov;
        }),
        def("set_fov", +[](float fov)
        {
            g_fov = fov;
        }),
        def("show_minimap", +[]
        {
            CurrentGameUI()->UIMainIngameWnd->ShowZoneMap(true);
        }),
        def("hide_minimap", +[]
        {
            CurrentGameUI()->UIMainIngameWnd->ShowZoneMap(false);
        }),
        def("minimap_shown", +[]
        {
            return CurrentGameUI()->UIMainIngameWnd->IsZoneMapShown();
        }),

        //Alundaio: Extend level namespace exports
        def("send", &g_send) , //allow the ability to send netpacket to level
        def("spawn_item", &spawn_section),
        def("get_active_cam", &get_active_cam),
        def("set_active_cam", &set_active_cam),
        def("get_start_time", +[]() -> xrTime { return xrTime(Level().GetStartGameTime()); }),
        def("valid_vertex", +[](u32 level_vertex_id)
        {
            return ai().level_graph().valid_vertex_id(level_vertex_id);
        }),
        //Alundaio: END

        def("iterate_online_objects", &iterate_online_objects),
        // obsolete\deprecated
        def("object_by_id", get_object_by_id),
#ifdef DEBUG
        def("debug_object", get_object_by_name),
        def("debug_actor", tpfGetActor),
        def("check_object", check_object),
#endif

        def("get_weather", get_weather),
        def("set_weather", set_weather),
        def("set_weather_fx", set_weather_fx),
        def("start_weather_fx_from_time", start_weather_fx_from_time),
        def("is_wfx_playing", is_wfx_playing),
        def("get_wfx_time", get_wfx_time),
        def("stop_weather_fx", stop_weather_fx),
        // engine gaps P11 (plans/addon_api_ideas/spec.md): the weather lock - level_weathers.script applies the locked
        // cycle instead of its own pick (addon_api_world.cpp). weather_lock(name, forced[, owner]) -> bool: another
        // owner's lock refuses (false, logged); owner "lua" by default. weather_unlock([owner]) -> bool.
        // weather_locked() -> the locked cycle or nil; weather_lock_owner() -> its owner or nil
        def("weather_lock", +[](pcstr name, bool forced) { return gw::addons::LuaWeatherLock(name, forced, nullptr); }),
        def("weather_lock", +[](pcstr name, bool forced, pcstr owner)
            { return gw::addons::LuaWeatherLock(name, forced, owner); }),
        def("weather_unlock", +[]() { return gw::addons::LuaWeatherUnlock(nullptr); }),
        def("weather_unlock", +[](pcstr owner) { return gw::addons::LuaWeatherUnlock(owner); }),
        def("weather_locked", +[]() { return gw::addons::WeatherLockedCycle(); }),
        def("weather_lock_owner", +[]() { return gw::addons::WeatherLockOwner(); }),
        // engine gaps P47: online objects in a radius, nearest first (objects_in_radius above)
        def("objects_in_radius", +[](lua_State* L, const Fvector& position, float radius)
            { return objects_in_radius(L, position, radius, -1, false); }),
        def("objects_in_radius", +[](lua_State* L, const Fvector& position, float radius, int script_clsid)
            { return objects_in_radius(L, position, radius, script_clsid, false); }),
        def("objects_in_radius",
            +[](lua_State* L, const Fvector& position, float radius, int script_clsid, bool with_inventory)
            { return objects_in_radius(L, position, radius, script_clsid, with_inventory); }),

        def("environment", environment),

        def("set_time_factor", set_time_factor),
        def("get_time_factor", get_time_factor),

        def("set_game_difficulty", set_game_difficulty),
        def("get_game_difficulty", get_game_difficulty),

        def("get_time_days", get_time_days),
        def("get_time_hours", get_time_hours),
        def("get_time_minutes", get_time_minutes),
        def("change_game_time", change_game_time),

        def("high_cover_in_direction", high_cover_in_direction),
        def("low_cover_in_direction", low_cover_in_direction),
        def("vertex_in_direction", vertex_in_direction),
        def("rain_factor", rain_factor),
        def("patrol_path_exists", patrol_path_exists),
        def("vertex_position", vertex_position),
        def("game_vertex_id", game_vertex_id), // RS4 (plans/addon_runtime_smarts): cross table, 65535 on an error
        def("name", +[]() { return Level().name().c_str(); }),
        def("prefetch_sound", prefetch_sound),

        def("client_spawn_manager", get_client_spawn_manager),

        def("map_add_object_spot_ser", map_add_object_spot_ser),
        def("map_add_object_spot", map_add_object_spot),
        def("map_set_object_spot", map_set_object_spot),
        def("map_change_spot_size", map_change_spot_size),
        def("map_change_spot_texture", map_change_spot_texture),
        def("map_change_spot_texture_rect", map_change_spot_texture_rect),
        def("map_change_spot_texture_offset", map_change_spot_texture_offset),
        def("map_change_spot_texture_offset_x", map_change_spot_texture_offset_x),
        def("map_change_spot_texture_offset_y", map_change_spot_texture_offset_y),
        def("map_change_spot_color", map_change_spot_color),
        // def("map_add_object_spot_complex", map_add_object_spot_complex),
        def("map_remove_object_spot", map_remove_object_spot),
        def("map_has_object_spot", map_has_object_spot),
        def("map_change_spot_hint", map_change_spot_hint),
        def("map_set_spot_hint_provider", map_set_spot_hint_provider),

        def("start_stop_menu", start_stop_menu),
        def("add_dialog_to_render", add_dialog_to_render),
        def("remove_dialog_to_render", remove_dialog_to_render),
        def("main_input_receiver", main_input_receiver),
        def("hide_indicators", hide_indicators),
        def("hide_indicators_safe", hide_indicators_safe),

        def("show_indicators", show_indicators),
        def("show_weapon", show_weapon),
        def("add_call", ((void (*)(const luabind::functor<bool>&, const luabind::functor<void>&)) & add_call)),
        def("add_call",
            ((void (*)(const luabind::object&, const luabind::functor<bool>&, const luabind::functor<void>&)) &
                add_call)),
        def("add_call", ((void (*)(const luabind::object&, LPCSTR, LPCSTR)) & add_call)),
        def("remove_call", ((void (*)(const luabind::functor<bool>&, const luabind::functor<void>&)) & remove_call)),
        def("remove_call",
            ((void (*)(const luabind::object&, const luabind::functor<bool>&, const luabind::functor<void>&)) &
                remove_call)),
        def("remove_call", ((void (*)(const luabind::object&, LPCSTR, LPCSTR)) & remove_call)),
        def("remove_calls_for_object", remove_calls_for_object),
        def("present", is_level_present),
        def("disable_input", disable_input),
        def("enable_input", enable_input),
        def("spawn_phantom", spawn_phantom),

        def("get_bounding_volume", get_bounding_volume),

        def("iterate_sounds", &iterate_sounds1),
        def("iterate_sounds", &iterate_sounds2),
        def("get_snd_volume", &get_snd_volume),
        def("set_snd_volume", &set_snd_volume),
        def("get_music_volume", &get_music_volume),
        def("set_music_volume", &set_music_volume),

        def("add_cam_effector", &add_cam_effector),
        def("add_cam_effector2", &add_cam_effector2),
        def("add_cam_effector2", +[](pcstr fn, int id, bool cyclic, pcstr cb_func)
        {
            add_cam_effector2(fn, id, cyclic, cb_func, 0.0f);
        }),

        def("remove_cam_effector", &remove_cam_effector),
        def("add_pp_effector", &add_pp_effector),
        def("set_pp_effector_factor", &set_pp_effector_factor),
        def("set_pp_effector_factor", &set_pp_effector_factor2),
        def("remove_pp_effector", &remove_pp_effector),

        def("add_complex_effector", &add_complex_effector),
        def("remove_complex_effector", &remove_complex_effector),

        def("vertex_id", +[](Fvector position) -> u64
        {
            // Original luabind converts 4294967295 (which is u32(-1)) to 4294967296
            const u32 id = ai().level_graph().vertex_id(position);
            // reproduce original behaviour: 2^32 for a position off the level graph. The sum in u64 (plan 57, B1):
            // `id + 1` in u32 wrapped to 0 - the valid vertex 0 for Lua. u64 goes to Lua as a number (luabind
            // native_converter: lua_pushnumber), 4294967296 exactly
            return id == u32(-1) ? u64(id) + 1 : u64(id);
        }),
        def("game_id", &GameID),
        def("ray_pick", &ray_pick),

        // D3 (plans/addon_base_building/plan.md 10.3): "ghost" models without a game object (level_preview_model.h).
        // hpb - radians (heading, pitch, bank): as an object spawned with se_obj.angle = (p, h, b); id - a slot
        // 0..7, 0 when omitted. The change shows from the end of the frame; show answers false (and logs) for a
        // missing model file
        def("preview_model_show", +[](pcstr visual, const Fvector& position, const Fvector& hpb) -> bool
            { return gw::preview_model::Show(0, visual, position, hpb); }),
        def("preview_model_show", +[](pcstr visual, const Fvector& position, const Fvector& hpb, u32 id) -> bool
            { return gw::preview_model::Show(id, visual, position, hpb); }),
        def("preview_model_set_transform", +[](const Fvector& position, const Fvector& hpb) -> bool
            { return gw::preview_model::SetTransform(0, position, hpb); }),
        def("preview_model_set_transform", +[](const Fvector& position, const Fvector& hpb, u32 id) -> bool
            { return gw::preview_model::SetTransform(id, position, hpb); }),
        def("preview_model_hide", +[]() -> bool { return gw::preview_model::Hide(0); }),
        def("preview_model_hide", +[](u32 id) -> bool { return gw::preview_model::Hide(id); }),
        def("preview_model_hide_all", +[]() { gw::preview_model::HideAll(); }),
        // The tint of the translucent ghost: r, g, b, a in 0..1 (a - opacity); kept by the slot until the next call
        def("preview_model_set_color", +[](float r, float g, float b, float a) -> bool
            { return gw::preview_model::SetColor(0, Fvector4().set(r, g, b, a)); }),
        def("preview_model_set_color", +[](float r, float g, float b, float a, u32 id) -> bool
            { return gw::preview_model::SetColor(id, Fvector4().set(r, g, b, a)); }),
        def("preview_model_shown", +[]() -> bool { return gw::preview_model::IsShown(0); }),
        def("preview_model_shown", +[](u32 id) -> bool { return gw::preview_model::IsShown(id); }),
        // The bounding box of a model file in object space (the OGF header), into `box` (Fbox() of Lua): the overlap
        // check of a placement (base building). false - no such model / no box; the box is left as it was
        def("model_bounds", +[](pcstr visual, Fbox& box) -> bool { return gw::preview_model::ModelBounds(visual, box); }),

        // A picture of a model for the UI (plans/addon_base_building/plan.md 17.16) - the preview renderer of the
        // faction editor (PreviewSceneRenderer: R2 / R4 / GL, not R1). yaw / pitch - the camera around the model,
        // degrees; size - the texture, texels (32..512; one size for all the calls - the renderer keeps one set of its
        // buffers of that size). ui_preview_supported - the renderer has it. ui_preview_thumb - a cached picture: ""
        // while it waits for its render (one model a frame, console npc_preview_scene_budget_per_frame), then the
        // texture name for CUIStatic:InitTexture; kept for the session (DX11 - on disk too). ui_preview_live_* - a
        // live slot 0..3: the model stays created, the texture of the slot is rendered again only when yaw / pitch /
        // the model change (a turning model); live_texture - its name, live_ready - it holds the model
        // The framing of the next calls: the pan of the camera along its axes (positive y - the view up, the model lower
        // in the picture), its distance to the model (positive - farther, more of the model in the picture) and the
        // aspect of the frame (width / height, 0 or less - a square)
        def("ui_preview_framing", +[](float pan_x, float pan_y, float pan_z, float distance_offset, float frame_aspect)
        {
            g_ui_preview_pan.set(pan_x, pan_y, pan_z);
            g_ui_preview_distance_offset = distance_offset;
            g_ui_preview_frame_aspect = frame_aspect;
        }),
        // The fit framing of the next calls: fit > 0 - the top part of the model box of this fraction of its height (the
        // head down; 1 - the whole model) and the box width are fitted in the frame, with the real frame proportions
        // (no stretch for any frame_aspect); the framing pan / distance are adjustments on top. 0 - the engine framing
        // (camera at 1.85 x the box radius). Its presence (level.ui_preview_fit ~= nil) marks the engine with it
        def("ui_preview_fit", +[](float fit)
        {
            clamp(fit, 0.f, 1.f);
            g_ui_preview_fit = fit;
        }),
        def("ui_preview_supported", +[]() -> bool { return GEnv.Render->PreviewScene_LiveTextureName(0).size() != 0; }),
        def("ui_preview_thumb", +[](pcstr visual, float yaw, float pitch, u32 size) -> pcstr
        {
            static xr_string name;
            name.clear();
            if (!visual || !visual[0])
                return name.c_str();
            GEnv.Render->PreviewScene_SetSettings(ui_preview_settings(yaw, pitch, size));
            if (GEnv.Render->PreviewScene_IsCached(visual))
                name = GEnv.Render->PreviewScene_TextureName(visual).c_str();
            // the model pool stops the game on a missing model file (the render of the queue, a frame later)
            else if (gw::preview_model::ModelFileExists(visual, "ui_preview_thumb"))
                GEnv.Render->PreviewScene_ScheduleModel(visual, 10);
            return name.c_str();
        }),
        def("ui_preview_live_set", +[](u32 slot, pcstr visual, float yaw, float pitch, u32 size)
        {
            // a missing model file would stop the game in the model pool: the slot is emptied instead (live_ready false)
            if (visual && visual[0] && !gw::preview_model::ModelFileExists(visual, "ui_preview_live_set"))
            {
                GEnv.Render->PreviewScene_LiveClear(slot);
                return;
            }
            GEnv.Render->PreviewScene_LiveSet(slot, visual, ui_preview_settings(yaw, pitch, size));
        }),
        def("ui_preview_live_ready", +[](u32 slot) -> bool { return GEnv.Render->PreviewScene_LiveReady(slot); }),
        def("ui_preview_live_texture", +[](u32 slot) -> pcstr
        {
            static xr_string name;
            name = GEnv.Render->PreviewScene_LiveTextureName(slot).c_str();
            return name.c_str();
        }),
        def("ui_preview_live_clear", +[](u32 slot) { GEnv.Render->PreviewScene_LiveClear(slot); }),

        // RS11 (plans/addon_runtime_smarts, level_world_draw.h): shapes drawn in the world, kept by a string id until
        // set again / removed / the level unload. color - 0xAARRGGBB (GetARGB; the alpha is ignored); depth tested,
        // 1 pixel lines. false (and a log line): no level, a bad id or value, the limit of shapes
        def("draw_line", +[](pcstr id, const Fvector& a, const Fvector& b, u32 color) -> bool
            { return gw::world_draw::Line(id, a, b, color); }),
        def("draw_circle", +[](pcstr id, const Fvector& center, float radius, u32 color) -> bool
            { return gw::world_draw::Circle(id, center, radius, color, 0); }),
        def("draw_circle", +[](pcstr id, const Fvector& center, float radius, u32 color, u32 segments) -> bool
            { return gw::world_draw::Circle(id, center, radius, color, segments); }),
        def("draw_polyline", +[](pcstr id, const luabind::object& points, u32 color) -> bool
            { return draw_polyline(id, points, false, color); }),
        def("draw_polyline", &draw_polyline),
        def("draw_box", +[](pcstr id, const Fvector& center, const Fvector& half, const Fvector& hpb, u32 color) -> bool
            { return gw::world_draw::Box(id, center, half, hpb, color); }),
        def("draw_sphere", +[](pcstr id, const Fvector& center, float radius, u32 color) -> bool
            { return gw::world_draw::Sphere(id, center, radius, color); }),
        def("draw_arrow", +[](pcstr id, const Fvector& from, const Fvector& to, u32 color) -> bool
            { return gw::world_draw::Arrow(id, from, to, color, 0.f); }),
        def("draw_arrow", +[](pcstr id, const Fvector& from, const Fvector& to, u32 color, float head) -> bool
            { return gw::world_draw::Arrow(id, from, to, color, head); }),
        def("draw_remove", +[](pcstr id) -> bool { return gw::world_draw::Remove(id); }),
        def("draw_remove_prefix", +[](pcstr prefix) -> u32 { return gw::world_draw::RemovePrefix(prefix); }),
        def("draw_clear", +[]() { gw::world_draw::RemoveAll(); }),
        def("draw_count", +[]() -> u32 { return gw::world_draw::Count(); }),

        // RS12 (plans/addon_runtime_smarts, level_world_draw.h): a UI window (a child of a dialog of the HUD) put every
        // frame before the UI at the point of the world on the screen + (dx, dy) of the UI; hidden behind the camera
        def("ui_anchor", +[](CUIWindow* wnd, const Fvector& pos) -> bool
            { return gw::world_draw::Anchor(wnd, pos, 0.f, 0.f); }),
        def("ui_anchor", +[](CUIWindow* wnd, const Fvector& pos, float dx, float dy) -> bool
            { return gw::world_draw::Anchor(wnd, pos, dx, dy); }),
        def("ui_anchor_remove", +[](CUIWindow* wnd) -> bool { return gw::world_draw::AnchorRemove(wnd); }),
        def("ui_anchor_clear", +[]() { gw::world_draw::AnchorClear(); })
    ];

    module(luaState, "actor_stats")
    [
        def("add_points", &add_actor_points),
        def("add_points_str", &add_actor_points_str),
        def("get_points", &get_actor_points)
    ];

    module(luaState)
    [
        def("command_line", &command_line),
        def("IsGameTypeSingle", (bool (*)())&IsGameTypeSingle),
        def("IsDynamicMusic", &IsDynamicMusic),
        def("render_get_dx_level", &render_get_dx_level),
        def("IsImportantSave", &IsImportantSave)
    ];

    module(luaState, "relation_registry")
    [
        def("community_goodwill", &g_community_goodwill),
        def("set_community_goodwill", &g_set_community_goodwill),
        def("change_community_goodwill", &g_change_community_goodwill),

        def("community_default_relation", &g_get_default_community_relation),
        def("reset_relations_to_default", &g_reset_relations_to_default),

        def("community_relation", &g_get_community_relation),
        def("set_community_relation", &g_set_community_relation),
        def("get_general_goodwill_between", &g_get_general_goodwill_between)
    ];

    module(luaState, "sections_registry")
    [
        class_<EnumCallbackType<ESectionTypeName>>("section_type")
        .enum_("section_types")
        [
            value("sctypAmmo", int(ESectionTypeName::ammo)),
            value("sctypScopes", int(ESectionTypeName::scopes)),
            value("sctypSilencers", int(ESectionTypeName::silencers)),
            value("sctypLaunchers", int(ESectionTypeName::launchers)),
            value("sctypKnife", int(ESectionTypeName::knife)),
            value("sctypPistol", int(ESectionTypeName::pistol)),
            value("sctypAutoPistol", int(ESectionTypeName::auto_pistol)),
            value("sctypShotgun", int(ESectionTypeName::shotgun)),
            value("sctypRifle", int(ESectionTypeName::rifle)),
            value("sctypSniperRifle", int(ESectionTypeName::sniper_rifle)),
            value("sctypHeavyRifle", int(ESectionTypeName::heavy_rifle)),
            value("sctypExplosive", int(ESectionTypeName::explosive)),
            value("sctypOutfit", int(ESectionTypeName::outfit)),
            value("sctypArtefact", int(ESectionTypeName::artefact)),
            value("sctypItemQuest", int(ESectionTypeName::item_quest)),
            value("sctypItemMisc", int(ESectionTypeName::item_misc)),
            value("sctypItemConsumable", int(ESectionTypeName::item_consumable)),
            value("sctypItemMedical", int(ESectionTypeName::item_medical)),
            value("sctypItemFood", int(ESectionTypeName::item_food)),
            value("sctypItemMisc", int(ESectionTypeName::item_misc)),
            value("sctypItemQuest", int(ESectionTypeName::item_quest)),
            value("sctypNpc", int(ESectionTypeName::npc)),
            value("sctypMutant", int(ESectionTypeName::mutant)),
            value("sctypSquadNpc", int(ESectionTypeName::squad_npc)),
            value("sctypSquadMutant", int(ESectionTypeName::squad_mutant)),
            value("sctypVehicle", int(ESectionTypeName::vehicle)),
            value("sctypPhysic", int(ESectionTypeName::physic)),
            value("sctypModels", int(ESectionTypeName::models)),
            value("sctypAnomaly", int(ESectionTypeName::anomaly)),
            value("sctypPhantom", int(ESectionTypeName::phantom)),
            value("sctypBackpack", int(ESectionTypeName::backpack))
        ],
        def("get_sections", &get_sections)
    ];

    module(luaState, "game")
    [
        class_<xrTime>("CTime")
            .enum_("date_format")
            [
                value("DateToDay", int(InventoryUtilities::edpDateToDay)),
                value("DateToMonth", int(InventoryUtilities::edpDateToMonth)),
                value("DateToYear", int(InventoryUtilities::edpDateToYear))
            ]
            .enum_("time_format")
            [
                value("TimeToHours", int(InventoryUtilities::etpTimeToHours)),
                value("TimeToMinutes", int(InventoryUtilities::etpTimeToMinutes)),
                value("TimeToSeconds", int(InventoryUtilities::etpTimeToSeconds)),
                value("TimeToMilisecs", int(InventoryUtilities::etpTimeToMilisecs))
            ]
            .def(constructor<>())
            .def(constructor<const xrTime&>())
            .def(const_self < xrTime())
            .def(const_self <= xrTime())
            .def(const_self > xrTime())
            .def(const_self >= xrTime())
            .def(const_self == xrTime())
            .def(self + xrTime())
            .def(self - xrTime())

            .def("diffSec", &xrTime::diffSec_script)
            .def("add", &xrTime::add_script)
            .def("sub", &xrTime::sub_script)

            .def("setHMS", &xrTime::setHMS)
            .def("setHMSms", &xrTime::setHMSms)
            .def("set", &xrTime::set)
            .def("get", &xrTime::get, policy_list<out_value<2>, out_value<3>, out_value<4>,
                                          out_value<5>, out_value<6>, out_value<7>, out_value<8>>())
            .def("dateToString", &xrTime::dateToString)
            .def("timeToString", &xrTime::timeToString),

        // declarations
        def("time", get_time),
        def("get_game_time", get_time_struct),
        // def("get_surge_time",	Game::get_surge_time),
        // def("get_object_by_name",Game::get_object_by_name),

        def("start_tutorial", &start_tutorial),
        def("stop_tutorial", &stop_tutorial),
        def("has_active_tutorial", &has_active_tutotial),
        def("active_tutorial_name", +[](){ return g_tutorial->GetTutorName(); }),
        // RS10 (plans/addon_runtime_smarts): game.world2ui(pos[, hud[, allow_offscreen]]) -> vector2 of the UI
        // (1024 x 768); x = -9999 - behind the camera / out of the screen. See world_to_ui
        def("world2ui", +[](const Fvector& pos) { return world_to_ui(pos, false, false); }),
        def("world2ui", +[](const Fvector& pos, bool hud) { return world_to_ui(pos, hud, false); }),
        def("world2ui", &world_to_ui),
        def("translate_string", &translate_string),
        def("replace_translate_string", &replace_translate_string),
        def("register_string_token", &register_string_token),
        def("unregister_string_token", &unregister_string_token),
        def("apply_string_tokens", &apply_string_tokens),
        def("reload_language", +[]() { StringTable().ReloadLanguage(); }),
        def("log_stack_trace", &xrDebug::LogStackTrace),
        def("jump_to_level", +[](pcstr level_name)
        {
            if (!ai().game_graph().header().level_exist(level_name))
            {
                GEnv.ScriptEngine->script_log(LuaMessageType::Error,
                    "game.jump_to_level: cannot jump to level '%s' – it doesn't exist", level_name);
                return;
            }
            ai().alife().jump_to_level(level_name);
        }),
        def("jump_to_level", &jump_to_level),
        def("jump_to_level", +[](const Fvector& m_position, u32 m_level_vertex_id, GameGraph::_GRAPH_ID m_game_vertex_id)
        {
            jump_to_level(m_position, m_level_vertex_id, m_game_vertex_id, {});
        })
    ];
}
