#include "StdAfx.h"

#include "addon_api_world.h"
#include "addon_host.h"

#include "Actor.h"
#include "CustomMonster.h"
#include "Explosive.h"
#include "Level.h"
#include "PhysicsShellHolder.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_time_manager.h"
#include "alife_time_skip.h"
#include "game_sv_single.h"
#include "script_game_object.h"
#include "xrServer.h"
#include "xrAICore/Navigation/game_graph.h"
#include "xrAICore/Navigation/game_level_cross_table.h"
#include "xrAICore/Navigation/level_graph.h"
#include "xrEngine/CameraBase.h"
#include "xrEngine/Environment.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrPhysics/PhysicsShell.h"

#include <cmath>
#include <cstring>

namespace gw::addons
{
namespace
{
// game_time_advance: at most ten game years in one call (the game clock is a u64 of milliseconds; the scripts shift
// it by hours and days)
constexpr u64 kWorldMaxTimeAdvanceMs = 10ull * 365 * 24 * 60 * 60 * 1000;
// game_time_set_factor: the mod uses 1..~100; a million keeps the float math of the clock and the sky finite
constexpr double kWorldMaxTimeFactor = 1e6;

bool WorldCheckCall(pcstr function)
{
    if (IsMainThread())
        return true;
    Msg("! [world] %s called outside the main thread, ignored", function);
    return false;
}

uint32_t WorldCopy(pcstr text, char* out, uint32_t cap)
{
    const u32 length = text ? xr_strlen(text) : 0;
    if (out && cap)
    {
        const u32 count = length < cap - 1 ? length : cap - 1;
        if (count)
            std::memcpy(out, text, count);
        out[count] = '\0';
    }
    return length;
}

uint32_t WorldNone(char* out, uint32_t cap)
{
    if (out && cap)
        out[0] = '\0';
    return 0;
}

// The game graph (of every level) exists while a game runs; the vertex accessors of the engine do not check ids.
const CGameGraph::CGameVertex* WorldGameVertex(uint32_t game_vertex)
{
    if (!ai().get_game_graph() || !ai().game_graph().valid_vertex_id(game_vertex))
        return nullptr;
    return ai().game_graph().vertex(game_vertex);
}

CGameObject* WorldOnline(GwpObjectId id)
{
    if (!g_pGameLevel || id == GWP_INVALID_OBJECT_ID)
        return nullptr;
    CGameObject* object = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
    return object && !object->getDestroy() ? object : nullptr;
}

// The calls that change the world: the thread, the plugin and a loaded level.
GwpResult WorldWriteGate(const GwpPlugin* self, pcstr function)
{
    if (!WorldCheckCall(function))
        return GWP_ERROR_NOT_MAIN_THREAD;
    if (!self)
        return GWP_ERROR_INVALID_ARGUMENT;
    if (!g_pGameLevel || !g_pGamePersistent || Device.editor_mode())
        return GWP_ERROR_INVALID_STATE;
    return GWP_OK;
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group world): the game graph and the levels
// ---------------------------------------------------------------------------------------------

uint32_t GWP_CALL ApiGraphVertexCount()
{
    if (!WorldCheckCall("graph_vertex_count") || !ai().get_game_graph())
        return 0;
    return ai().game_graph().header().vertex_count();
}

int32_t GWP_CALL ApiGraphVertexLevelId(uint32_t game_vertex)
{
    const CGameGraph::CGameVertex* vertex = WorldCheckCall("graph_vertex_level_id") ? WorldGameVertex(game_vertex) : nullptr;
    return vertex ? static_cast<int32_t>(vertex->level_id()) : -1;
}

uint32_t GWP_CALL ApiGraphVertexLevelVertex(uint32_t game_vertex)
{
    const CGameGraph::CGameVertex* vertex =
        WorldCheckCall("graph_vertex_level_vertex") ? WorldGameVertex(game_vertex) : nullptr;
    return vertex ? vertex->level_vertex_id() : GWP_INVALID_LEVEL_VERTEX;
}

int GWP_CALL ApiGraphVertexPosition(uint32_t game_vertex, float out_xyz[3])
{
    if (out_xyz)
        out_xyz[0] = out_xyz[1] = out_xyz[2] = 0.f;
    const CGameGraph::CGameVertex* vertex = WorldCheckCall("graph_vertex_position") ? WorldGameVertex(game_vertex) : nullptr;
    if (!vertex || !out_xyz)
        return 0;
    // level_point: the place on its own level (game_point is the coordinate of the global map, for distances
    // between levels and the PDA)
    const Fvector& point = vertex->level_point();
    out_xyz[0] = point.x;
    out_xyz[1] = point.y;
    out_xyz[2] = point.z;
    return 1;
}

uint32_t GWP_CALL ApiGraphVertexByLevelVertex(uint32_t level_vertex)
{
    if (!WorldCheckCall("graph_vertex_by_level_vertex") || !g_pGameLevel || !ai().get_level_graph() ||
        !ai().get_cross_table() || !ai().level_graph().valid_vertex_id(level_vertex))
        return GWP_INVALID_GAME_VERTEX;
    // The cross table checks the vertex with VERIFY only: valid_vertex_id above keeps a release build in bounds
    return ai().cross_table().vertex(level_vertex).game_vertex_id();
}

uint32_t GWP_CALL ApiLevelNameById(int32_t level_id, char* out, uint32_t cap)
{
    if (!WorldCheckCall("level_name_by_id") || !ai().get_game_graph() || level_id < 0 || level_id > 255)
        return WorldNone(out, cap); // GameGraph::_LEVEL_ID is a u8
    const auto& header = ai().game_graph().header();
    const auto id = static_cast<GameGraph::_LEVEL_ID>(level_id);
    if (!header.level_exist(id))
        return WorldNone(out, cap);
    return WorldCopy(header.level(id).name().c_str(), out, cap);
}

int32_t GWP_CALL ApiLevelIdByName(const char* name)
{
    if (!WorldCheckCall("level_id_by_name") || !name || !*name || !ai().get_game_graph())
        return -1;
    const GameGraph::SLevel* level = ai().game_graph().header().level(name, true); // true: nullptr, no assert
    return level ? static_cast<int32_t>(level->id()) : -1;
}

int32_t GWP_CALL ApiLevelCurrentId()
{
    if (!WorldCheckCall("level_current_id") || !g_pGameLevel || !ai().get_level_graph())
        return -1;
    return static_cast<int32_t>(ai().level_graph().level_id());
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group world): moving online objects, the camera of the actor, explosions
// ---------------------------------------------------------------------------------------------

GwpResult GWP_CALL ApiObjectTeleport(const GwpPlugin* self, GwpObjectId id, const float position[3])
{
    const GwpResult gate = WorldWriteGate(self, "object_teleport");
    if (gate != GWP_OK)
        return gate;
    if (!position || !std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(position[2]))
        return GWP_ERROR_INVALID_ARGUMENT; // a NaN reaches the physics of ODE in a release build
    CGameObject* object = WorldOnline(id);
    if (!object)
        return GWP_ERROR_NOT_FOUND;
    if (object->H_Parent())
        return GWP_ERROR_INVALID_ARGUMENT;
    CScriptGameObject* script = object->lua_game_object();
    const Fvector place = Fvector().set(position[0], position[1], position[2]);
    auto* actor = smart_cast<CActor*>(object);
    auto* creature = smart_cast<CCustomMonster*>(object);
    // The Lua methods themselves: set_actor_position / set_npc_position / force_set_position
    if (actor)
    {
        if (actor->Holder())
            return GWP_ERROR_INVALID_STATE; // in a vehicle or at a mounted gun the holder owns the position
        script->SetActorPosition(place);
    }
    else if (creature && creature->g_Alive())
        script->SetNpcPosition(place);
    else
    {
        // A physics object, and a corpse too: the character controller of a dead creature ignores ForceTransform,
        // its ragdoll is a physics shell
        CPhysicsShellHolder* holder = object->cast_physics_shell_holder();
        if (!holder || !holder->PPhysicsShell())
            return GWP_ERROR_NOT_SUPPORTED; // force_set_position logs an error without a shell
        script->ForceSetPosition(place, true);
    }
    if (IsDebugLog())
        Msg("~ [plugin:%s] object_teleport: '%s' id=%u -> [%f,%f,%f]", PluginAddonId(self), object->cName().c_str(),
            u32(id), place.x, place.y, place.z);
    return GWP_OK;
}

GwpResult GWP_CALL ApiActorSetDirection(const GwpPlugin* self, const float dir[3])
{
    const GwpResult gate = WorldWriteGate(self, "actor_set_direction");
    if (gate != GWP_OK)
        return gate;
    if (!dir)
        return GWP_ERROR_INVALID_ARGUMENT;
    CActor* actor = g_actor; // the actor, also while it sits in a vehicle (CurrentEntity is the vehicle then)
    if (!actor || !actor->cam_Active() || actor->Holder())
        return GWP_ERROR_INVALID_STATE; // in a vehicle or at a mounted gun the camera belongs to the holder
    Fvector direction = Fvector().set(dir[0], dir[1], dir[2]);
    const float horizontal = std::sqrt(direction.x * direction.x + direction.z * direction.z);
    if (horizontal < 1e-4f || !std::isfinite(direction.magnitude()))
        return GWP_ERROR_INVALID_ARGUMENT; // a zero or a vertical vector has no yaw
    // The heading and the pitch of the vector (getHP gives the angles getHPB gives for an orthonormal matrix with this
    // k axis), handed to the camera the way CActor::ForceTransformAndDirection does: with the opposite signs (Lua
    // scripts call set_actor_direction(-dir:getH()))
    float heading = 0.f, pitch = 0.f;
    direction.getHP(heading, pitch);
    actor->cam_Active()->Set(-heading, -pitch, 0.f);
    return GWP_OK;
}

GwpResult GWP_CALL ApiObjectExplode(const GwpPlugin* self, GwpObjectId id)
{
    const GwpResult gate = WorldWriteGate(self, "object_explode");
    if (gate != GWP_OK)
        return gate;
    CGameObject* object = WorldOnline(id);
    if (!object)
        return GWP_ERROR_NOT_FOUND;
    CExplosive* explosive = smart_cast<CExplosive*>(object);
    if (!explosive || object->H_Parent())
        return GWP_ERROR_INVALID_ARGUMENT;
    if (explosive->IsExploding())
        return GWP_ERROR_INVALID_STATE; // the explosion runs already; a second event is ignored by the object
    // CScriptGameObject::explode: the explosion event of the object at its place, the normal of the ground under it
    Fvector normal;
    explosive->FindNormal(normal);
    explosive->SetInitiator(object->ID());
    explosive->GenExplodeEvent(object->Position(), normal);
    if (IsDebugLog())
        Msg("~ [plugin:%s] object_explode: '%s' id=%u", PluginAddonId(self), object->cName().c_str(), u32(id));
    return GWP_OK;
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group world): weather
// ---------------------------------------------------------------------------------------------

uint32_t GWP_CALL ApiWeatherGet(char* out, uint32_t cap)
{
    if (!WorldCheckCall("weather_get") || !g_pGameLevel || !g_pGamePersistent)
        return WorldNone(out, cap);
    return WorldCopy(g_pGamePersistent->Environment().GetWeather().c_str(), out, cap);
}

GwpResult GWP_CALL ApiWeatherSet(const GwpPlugin* self, const char* name, int forced)
{
    const GwpResult gate = WorldWriteGate(self, "weather_set");
    if (gate != GWP_OK)
        return gate;
    if (!name || !*name)
        return GWP_ERROR_INVALID_ARGUMENT; // SetWeather stops the game on an empty name
    CEnvironment& environment = g_pGamePersistent->Environment();
    if (environment.WeatherCycles.find(shared_str(name)) == environment.WeatherCycles.end())
        return GWP_ERROR_NOT_FOUND;
    // forced during an effect: SetWeather only remembers the cycle then, and its forced branch asserts on the effect
    environment.SetWeather(name, forced != 0 && !environment.IsWFXPlaying());
    return GWP_OK;
}

GwpResult GWP_CALL ApiWeatherFxStart(const GwpPlugin* self, const char* name, float time_left)
{
    const GwpResult gate = WorldWriteGate(self, "weather_fx_start");
    if (gate != GWP_OK)
        return gate;
    // time_left: the game seconds the effect has left (the engine keeps wfx_time as that), at most a game day
    if (!name || !*name || !std::isfinite(time_left) || time_left < 0.f || time_left > 86400.f)
        return GWP_ERROR_INVALID_ARGUMENT;
    CEnvironment& environment = g_pGamePersistent->Environment();
    // SetWeatherFX asserts on an unknown name: checked here
    if (environment.WeatherFXs.find(shared_str(name)) == environment.WeatherFXs.end())
        return GWP_ERROR_NOT_FOUND;
    // Busy with another effect, or the blend descriptors are not set yet (right after a load, before the first
    // frame of the weather): SetWeatherFX reads Current[0] / Current[1]
    if (environment.IsWFXPlaying() || !environment.Current[0] || !environment.Current[1])
        return GWP_ERROR_INVALID_STATE;
    const bool started = time_left > 0.f ? environment.StartWeatherFXFromTime(name, time_left) :
                                           environment.SetWeatherFX(name);
    return started ? GWP_OK : GWP_ERROR_INVALID_STATE; // the engine refused to start it (an effect started meanwhile)
}

GwpResult GWP_CALL ApiWeatherFxStop(const GwpPlugin* self)
{
    const GwpResult gate = WorldWriteGate(self, "weather_fx_stop");
    if (gate != GWP_OK)
        return gate;
    CEnvironment& environment = g_pGamePersistent->Environment();
    if (!environment.IsWFXPlaying())
        return GWP_ERROR_INVALID_STATE; // StopWFX does not check: it would replace the weather with a stale end
    environment.StopWFX();
    return GWP_OK;
}

int GWP_CALL ApiWeatherFxPlaying()
{
    if (!WorldCheckCall("weather_fx_playing") || !g_pGameLevel || !g_pGamePersistent)
        return 0;
    return g_pGamePersistent->Environment().IsWFXPlaying() ? 1 : 0;
}

float GWP_CALL ApiWeatherFxTime()
{
    if (!WorldCheckCall("weather_fx_time") || !g_pGameLevel || !g_pGamePersistent)
        return 0.f;
    CEnvironment& environment = g_pGamePersistent->Environment();
    return environment.IsWFXPlaying() ? environment.wfx_time : 0.f;
}

float GWP_CALL ApiWeatherRainFactor()
{
    if (!WorldCheckCall("weather_rain_factor") || !g_pGameLevel || !g_pGamePersistent)
        return 0.f;
    return g_pGamePersistent->Environment().CurrentEnv.rain_density;
}

// ---------------------------------------------------------------------------------------------
// engine gaps P11 (plans/addon_api_ideas/spec.md): the weather lock. level_weathers.script picks the weather at
// every change of the game hour and on a load; with a lock it applies the locked cycle instead of its own pick.
// One lock at a time, owned by a plugin ("plugin:<id>") or by Lua (an owner name). It lives until unlocked, the
// owning plugin stops, or a game starts that is not a level change (a new game, a loaded save). Not saved.
// ---------------------------------------------------------------------------------------------

struct WeatherLockState
{
    bool active = false;
    xr_string cycle;
    xr_string owner;
    const GwpPlugin* plugin = nullptr; // the owning plugin, nullptr for a Lua lock
};
WeatherLockState g_weather_lock;

GwpResult WeatherLockSet(pcstr owner, const GwpPlugin* plugin, pcstr name, bool forced)
{
    if (!name || !*name)
        return GWP_ERROR_INVALID_ARGUMENT; // SetWeather stops the game on an empty name
    if (!g_pGameLevel || !g_pGamePersistent || Device.editor_mode())
        return GWP_ERROR_INVALID_STATE;
    if (g_weather_lock.active && g_weather_lock.owner != owner)
        return GWP_ERROR_ACCESS_DENIED;
    CEnvironment& environment = g_pGamePersistent->Environment();
    if (environment.WeatherCycles.find(shared_str(name)) == environment.WeatherCycles.end())
        return GWP_ERROR_NOT_FOUND;
    g_weather_lock.active = true;
    g_weather_lock.cycle = name;
    g_weather_lock.owner = owner;
    g_weather_lock.plugin = plugin;
    // as weather_set: forced during an effect only remembers the cycle (the forced branch asserts on the effect)
    environment.SetWeather(name, forced && !environment.IsWFXPlaying());
    if (IsDebugLog())
        Msg("~ [weather] lock '%s' by %s%s", name, owner, forced ? " (forced)" : "");
    return GWP_OK;
}

GwpResult WeatherLockRelease(pcstr owner)
{
    if (!g_weather_lock.active)
        return GWP_ERROR_NOT_FOUND;
    if (g_weather_lock.owner != owner)
        return GWP_ERROR_ACCESS_DENIED;
    if (IsDebugLog())
        Msg("~ [weather] unlock '%s' by %s", g_weather_lock.cycle.c_str(), owner);
    g_weather_lock = {};
    return GWP_OK;
}

xr_string WeatherPluginOwner(const GwpPlugin* self)
{
    xr_string owner = "plugin:";
    owner += PluginAddonId(self);
    return owner;
}

GwpResult GWP_CALL ApiWeatherLock(const GwpPlugin* self, const char* name, int forced)
{
    if (!WorldCheckCall("weather_lock"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    if (!self)
        return GWP_ERROR_INVALID_ARGUMENT;
    const GwpResult result = WeatherLockSet(WeatherPluginOwner(self).c_str(), self, name, forced != 0);
    if (result == GWP_ERROR_ACCESS_DENIED && IsDebugLog())
        Msg("~ [plugin:%s] weather_lock: the weather is locked by %s", PluginAddonId(self), g_weather_lock.owner.c_str());
    return result;
}

GwpResult GWP_CALL ApiWeatherUnlock(const GwpPlugin* self)
{
    if (!WorldCheckCall("weather_unlock"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    if (!self)
        return GWP_ERROR_INVALID_ARGUMENT;
    return WeatherLockRelease(WeatherPluginOwner(self).c_str());
}

uint32_t GWP_CALL ApiWeatherLocked(char* out, uint32_t cap)
{
    if (!WorldCheckCall("weather_locked") || !g_weather_lock.active)
        return WorldNone(out, cap);
    return WorldCopy(g_weather_lock.cycle.c_str(), out, cap);
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group world): writing the game time
// ---------------------------------------------------------------------------------------------

GwpResult GWP_CALL ApiGameTimeAdvance(const GwpPlugin* self, uint64_t milliseconds)
{
    const GwpResult gate = WorldWriteGate(self, "game_time_advance");
    if (gate != GWP_OK)
        return gate;
    game_sv_Single* game = Level().Server ? smart_cast<game_sv_Single*>(Level().Server->GetGameState()) : nullptr;
    if (!game || !ai().get_alife() || !ai().alife().initialized())
        return GWP_ERROR_INVALID_STATE;
    if (milliseconds > kWorldMaxTimeAdvanceMs)
        return GWP_ERROR_INVALID_ARGUMENT;
    if (!milliseconds)
        return GWP_OK;
    if (IsDebugLog())
        Msg("~ [plugin:%s] game_time_advance: %llu ms", PluginAddonId(self), static_cast<unsigned long long>(milliseconds));
    // level.change_game_time in one step of any length: the clock of ALife is set to its time plus the shift (what
    // change_game_time adds, without its u32 limit), the sky turns by the time of day the shift adds (the sky keeps
    // the time of day only, NormalizeTime takes one day off at most)
    CALifeTimeManager& clock = game->alife().time_manager();
    clock.set_game_time_factor(clock.game_time() + milliseconds, clock.time_factor());
    constexpr u64 day_ms = 24ull * 60 * 60 * 1000;
    g_pGamePersistent->Environment().ChangeGameTime(static_cast<float>(milliseconds % day_ms) / 1000.f);
    // plans/lua_to_cpp/55-timeskip-squad-movement.md: the offline movement catches up step by step (as change_game_time)
    alife_time_skip::Begin(game->alife(), milliseconds);
    return GWP_OK;
}

GwpResult GWP_CALL ApiGameTimeSetFactor(const GwpPlugin* self, double factor)
{
    const GwpResult gate = WorldWriteGate(self, "game_time_set_factor");
    if (gate != GWP_OK)
        return gate;
    if (!std::isfinite(factor) || factor <= 0. || factor > kWorldMaxTimeFactor)
        return GWP_ERROR_INVALID_ARGUMENT; // a float inf breaks game_time() and the rewind of a weather effect
    if (!OnServer() || !Level().Server || !ai().get_alife())
        return GWP_ERROR_INVALID_STATE;
    // level.set_time_factor: the game state passes it on to the time manager of ALife
    Level().Server->GetGameState()->SetGameTimeFactor(static_cast<float>(factor));
    return GWP_OK;
}
} // namespace

// ---------------------------------------------------------------------------------------------
// Plugin API (group world): the level of the actor (plans/addon_api_ideas/spec.md P20)
// ---------------------------------------------------------------------------------------------

// The level change of the actor needs a running game with the actor alive (the server moves it at the next network
// update; the autosave and on_level_changing come as for a level changer)
GwpResult WorldActorLevelGate(const GwpPlugin* self, pcstr function)
{
    const GwpResult gate = WorldWriteGate(self, function);
    if (gate != GWP_OK)
        return gate;
    if (!ai().get_alife() || !ai().get_game_graph() || !g_actor || !g_actor->g_Alive())
    {
        if (IsDebugLog())
            Msg("~ [plugin:%s] %s: no game or no live actor", PluginAddonId(self), function);
        return GWP_ERROR_INVALID_STATE;
    }
    return GWP_OK;
}

GwpResult GWP_CALL ApiActorChangeLevel(const GwpPlugin* self, uint32_t game_vertex, uint32_t level_vertex,
    const float position[3], const float angles[3])
{
    const GwpResult gate = WorldActorLevelGate(self, "actor_change_level");
    if (gate != GWP_OK)
        return gate;
    const CGameGraph::CGameVertex* vertex = WorldGameVertex(game_vertex);
    if (!vertex)
        return GWP_ERROR_INVALID_ARGUMENT;
    for (const float* v : { position, angles })
    {
        if (v && (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])))
            return GWP_ERROR_INVALID_ARGUMENT;
    }
    // The packet of game.jump_to_level (level_script.cpp): the game vertex, the level vertex, the position, the angles
    GameGraph::_GRAPH_ID graph_id = static_cast<GameGraph::_GRAPH_ID>(game_vertex);
    u32 level_id = level_vertex == GWP_INVALID_LEVEL_VERTEX ? vertex->level_vertex_id() : level_vertex;
    const Fvector place = position ? Fvector().set(position[0], position[1], position[2]) : vertex->level_point();
    const Fvector turn = angles ? Fvector().set(angles[0], angles[1], angles[2]) : Fvector().set(0.f, 0.f, 0.f);
    NET_Packet packet;
    packet.w_begin(M_CHANGE_LEVEL);
    packet.w(&graph_id, sizeof(graph_id));
    packet.w(&level_id, sizeof(level_id));
    packet.w_vec3(place);
    packet.w_vec3(turn);
    Level().Send(packet, net_flags(TRUE));
    Msg("* [plugin:%s] actor_change_level: game vertex %u (level '%s'), level vertex %u", PluginAddonId(self),
        game_vertex, ai().game_graph().header().level(vertex->level_id()).name().c_str(), level_id);
    return GWP_OK;
}

GwpResult GWP_CALL ApiActorChangeLevelTo(const GwpPlugin* self, const char* level_name)
{
    const GwpResult gate = WorldActorLevelGate(self, "actor_change_level_to");
    if (gate != GWP_OK)
        return gate;
    if (!level_name || !*level_name)
        return GWP_ERROR_INVALID_ARGUMENT;
    if (!ai().game_graph().header().level_exist(level_name))
        return GWP_ERROR_NOT_FOUND;
    Msg("* [plugin:%s] actor_change_level_to: '%s'", PluginAddonId(self), level_name);
    ai().alife().jump_to_level(level_name); // game.jump_to_level(name): the nearest game vertex of the level
    return GWP_OK;
}

void FillWorldApi(GwpEngineApi& api)
{
    api.graph_vertex_count = &ApiGraphVertexCount;
    api.graph_vertex_level_id = &ApiGraphVertexLevelId;
    api.graph_vertex_level_vertex = &ApiGraphVertexLevelVertex;
    api.graph_vertex_position = &ApiGraphVertexPosition;
    api.graph_vertex_by_level_vertex = &ApiGraphVertexByLevelVertex;
    api.level_name_by_id = &ApiLevelNameById;
    api.level_id_by_name = &ApiLevelIdByName;
    api.level_current_id = &ApiLevelCurrentId;
    api.object_teleport = &ApiObjectTeleport;
    api.actor_set_direction = &ApiActorSetDirection;
    api.object_explode = &ApiObjectExplode;
    api.weather_get = &ApiWeatherGet;
    api.weather_set = &ApiWeatherSet;
    api.weather_fx_start = &ApiWeatherFxStart;
    api.weather_fx_stop = &ApiWeatherFxStop;
    api.weather_fx_playing = &ApiWeatherFxPlaying;
    api.weather_fx_time = &ApiWeatherFxTime;
    api.weather_rain_factor = &ApiWeatherRainFactor;
    api.game_time_advance = &ApiGameTimeAdvance;
    api.game_time_set_factor = &ApiGameTimeSetFactor;
    // engine gaps P11: the weather lock
    api.weather_lock = &ApiWeatherLock;
    api.weather_unlock = &ApiWeatherUnlock;
    api.weather_locked = &ApiWeatherLocked;
    api.actor_change_level = &ApiActorChangeLevel; // P20
    api.actor_change_level_to = &ApiActorChangeLevelTo;
}

// engine gaps P11: the weather lock for Lua (level.weather_lock / weather_unlock / weather_locked, level_script.cpp)
bool LuaWeatherLock(pcstr name, bool forced, pcstr owner)
{
    if (!WorldCheckCall("level.weather_lock"))
        return false;
    const pcstr who = owner && *owner ? owner : "lua";
    const GwpResult result = WeatherLockSet(who, nullptr, name, forced);
    if (result == GWP_OK)
        return true;
    if (result == GWP_ERROR_ACCESS_DENIED)
        Msg("! [weather] level.weather_lock('%s'): the weather is locked by %s", name, g_weather_lock.owner.c_str());
    else if (result == GWP_ERROR_NOT_FOUND)
        Msg("! [weather] level.weather_lock('%s'): no such weather cycle", name);
    else
        Msg("! [weather] level.weather_lock('%s'): no level or no name", name ? name : "");
    return false;
}

bool LuaWeatherUnlock(pcstr owner)
{
    if (!WorldCheckCall("level.weather_unlock"))
        return false;
    return WeatherLockRelease(owner && *owner ? owner : "lua") == GWP_OK;
}

pcstr WeatherLockedCycle() { return g_weather_lock.active ? g_weather_lock.cycle.c_str() : nullptr; }
pcstr WeatherLockOwner() { return g_weather_lock.active ? g_weather_lock.owner.c_str() : nullptr; }

void WeatherLockOnGameStart(bool level_change)
{
    // A level change keeps the lock: the next level applies it (level_weathers on its load)
    if (!level_change)
        g_weather_lock = {};
}

void ReleasePluginWeatherLock(const GwpPlugin* plugin)
{
    if (plugin && g_weather_lock.active && g_weather_lock.plugin == plugin)
    {
        Msg("* [weather] the lock '%s' of %s is released: the plugin stops", g_weather_lock.cycle.c_str(),
            g_weather_lock.owner.c_str());
        g_weather_lock = {};
    }
}
} // namespace gw::addons
