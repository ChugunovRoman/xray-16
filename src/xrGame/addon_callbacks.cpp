#include "StdAfx.h"

#include "addon_callbacks.h"
#include "addon_event_bus.h"
#include "addon_host.h"

#include "CustomMonster.h"
#include "enemy_manager.h"
#include "entity_alive.h"
#include "Level.h"
#include "memory_manager.h"

#include <algorithm>

namespace gw::addons::callbacks
{
namespace
{
enum class EKind
{
    Enemy,
    Patrol,
};

struct Callback
{
    GwpCallbackId id = GWP_INVALID_CALLBACK_ID;
    EKind kind = EKind::Enemy;
    const GwpPlugin* plugin = nullptr;
    GwpEnemyFilterFn enemy = nullptr;
    GwpPatrolExtrapolateFn patrol = nullptr;
    void* user = nullptr;
    u64 calls = 0;
    u64 vetoes = 0; // answers "no"
};

// Few entries (one or two per plugin), walked on every question: a flat vector.
xr_vector<Callback>* g_callbacks = nullptr;
u32 g_enemy_count = 0;
u32 g_patrol_count = 0;
GwpCallbackId g_next_id = 1;
u32 g_dispatch_depth = 0;

// The depth comes back down also when something unwinds through a dispatch: a depth stuck above zero would keep the
// dead callbacks in the list for the rest of the run
struct DispatchDepthScope
{
    DispatchDepthScope() { ++g_dispatch_depth; }
    ~DispatchDepthScope() { --g_dispatch_depth; }
    DispatchDepthScope(const DispatchDepthScope&) = delete;
    DispatchDepthScope& operator=(const DispatchDepthScope&) = delete;
};
bool g_has_dead = false; // entries removed during a dispatch, erased after it

bool CheckCall(pcstr function)
{
    if (IsMainThread())
        return true;
    Msg("! [callbacks] %s called outside the main thread, ignored", function);
    return false;
}

xr_vector<Callback>& Callbacks()
{
    if (!g_callbacks)
        g_callbacks = xr_new<xr_vector<Callback>>();
    return *g_callbacks;
}

void Recount()
{
    g_enemy_count = 0;
    g_patrol_count = 0;
    if (!g_callbacks)
        return;
    for (const Callback& callback : *g_callbacks)
    {
        if (callback.id == GWP_INVALID_CALLBACK_ID)
            continue;
        (callback.kind == EKind::Enemy ? g_enemy_count : g_patrol_count) += 1;
    }
}

void EraseDead()
{
    if (g_dispatch_depth || !g_has_dead || !g_callbacks)
        return;
    g_callbacks->erase(std::remove_if(g_callbacks->begin(), g_callbacks->end(),
                           [](const Callback& callback) { return callback.id == GWP_INVALID_CALLBACK_ID; }),
        g_callbacks->end());
    g_has_dead = false;
}

// Marks the entries of the plugin dead; erased at once outside a dispatch, after it otherwise.
void RemoveOf(const GwpPlugin* plugin, GwpCallbackId only = GWP_INVALID_CALLBACK_ID)
{
    if (!g_callbacks)
        return;
    for (Callback& callback : *g_callbacks)
    {
        if (callback.id != GWP_INVALID_CALLBACK_ID && callback.plugin == plugin &&
            (only == GWP_INVALID_CALLBACK_ID || callback.id == only))
        {
            callback.id = GWP_INVALID_CALLBACK_ID;
            g_has_dead = true;
        }
    }
    Recount();
    EraseDead();
}

void ApiResetAll(); // below: drops the enemy cache of every online NPC

struct Call
{
    const Callback* callback;
    u16 npc;
    u16 enemy;
    u32 point;
    int result;
};

void Thunk(void* context)
{
    Call& call = *static_cast<Call*>(context);
    const Callback& callback = *call.callback;
    call.result = callback.kind == EKind::Enemy ? callback.enemy(callback.user, call.npc, call.enemy)
                                                : callback.patrol(callback.user, call.npc, call.point);
}

// The walk of Ask, inside the dispatch depth (dead entries are only marked meanwhile)
bool AskAll(EKind kind, u16 npc, u16 enemy, u32 point)
{
    const DispatchDepthScope depth;
    bool answer = true;
    // Index loop over the size at the start: a callback may register another one (push_back) meanwhile.
    const size_t count = g_callbacks->size();
    for (size_t i = 0; i < count && answer; ++i)
    {
        Callback& callback = (*g_callbacks)[i];
        if (callback.id == GWP_INVALID_CALLBACK_ID || callback.kind != kind)
            continue;
        const GwpPlugin* const plugin = callback.plugin; // `callback` may move: the plugin may register another one
        Call call{ &callback, npc, enemy, point, 1 };
        const void* code = kind == EKind::Enemy ? reinterpret_cast<const void*>(callback.enemy)
                                                : reinterpret_cast<const void*>(callback.patrol);
        const pcstr what = kind == EKind::Enemy ? "enemy filter" : "patrol extrapolate callback";
        ++callback.calls; // before the call: the reference is not used after it
        if (!events::CallPluginGuarded(code, what, &Thunk, &call))
        {
            RemoveOf(plugin); // marks only: the dispatch is running
            if (kind == EKind::Enemy)
                ApiResetAll(); // the cached answers of the removed filters must not outlive them
            OnPluginCrashed(plugin, what); // the whole plugin stops
            continue;
        }
        if (!call.result)
        {
            ++(*g_callbacks)[i].vetoes;
            answer = false;
        }
    }
    return answer;
}

// Asks every callback of the kind; false as soon as one says no. A crash counts as yes.
bool Ask(EKind kind, u16 npc, u16 enemy, u32 point)
{
    // Plugins run on the game logic thread only (their API refuses any other): a question from elsewhere is
    // answered as without them.
    if (!IsMainThread())
        return true;
    const bool answer = AskAll(kind, npc, enemy, point);
    EraseDead();
    return answer;
}

void ResetNpc(CCustomMonster* npc) { npc->memory().enemy().clear_useful_callback_cache(); }

void GWP_CALL ApiNpcEnemyFilterReset(GwpObjectId id);

// The engine caches the enemy decision per NPC and enemy (250 ms): a filter that appears or goes away must not be
// shadowed by answers given without it.
void ApiResetAll() { ApiNpcEnemyFilterReset(GWP_INVALID_OBJECT_ID); }

GwpCallbackId Register(const GwpPlugin* self, EKind kind, GwpEnemyFilterFn enemy, GwpPatrolExtrapolateFn patrol,
    void* user, pcstr function)
{
    if (!CheckCall(function))
        return GWP_INVALID_CALLBACK_ID;
    if (!self || (!enemy && !patrol))
    {
        Msg("! [plugin:%s] %s: no plugin handle or no function", PluginAddonId(self), function);
        return GWP_INVALID_CALLBACK_ID;
    }
    Callback callback;
    callback.id = g_next_id++;
    callback.kind = kind;
    callback.plugin = self;
    callback.enemy = enemy;
    callback.patrol = patrol;
    callback.user = user;
    Callbacks().push_back(callback);
    Recount();
    if (kind == EKind::Enemy)
        ApiResetAll(); // earlier answers were cached without this filter
    if (IsDebugLog())
        Msg("* [plugin:%s] %s registered as #%u", PluginAddonId(self), function, callback.id);
    return callback.id;
}

// Plugin API (group callbacks)

GwpCallbackId GWP_CALL ApiEnemyFilterRegister(const GwpPlugin* self, GwpEnemyFilterFn fn, void* user)
{
    return Register(self, EKind::Enemy, fn, nullptr, user, "enemy_filter_register");
}

GwpCallbackId GWP_CALL ApiPatrolExtrapolateRegister(const GwpPlugin* self, GwpPatrolExtrapolateFn fn, void* user)
{
    return Register(self, EKind::Patrol, nullptr, fn, user, "patrol_extrapolate_register");
}

GwpResult GWP_CALL ApiCallbackUnregister(const GwpPlugin* self, GwpCallbackId id)
{
    if (!CheckCall("callback_unregister"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    if (id == GWP_INVALID_CALLBACK_ID || !g_callbacks)
        return GWP_ERROR_INVALID_ARGUMENT;
    const auto it = std::find_if(g_callbacks->begin(), g_callbacks->end(),
        [id, self](const Callback& callback) { return callback.id == id && callback.plugin == self; });
    if (it == g_callbacks->end())
        return GWP_ERROR_INVALID_ARGUMENT;
    const bool enemy = it->kind == EKind::Enemy;
    RemoveOf(self, id);
    if (enemy)
        ApiResetAll(); // the cached "no" of this filter must not outlive it
    return GWP_OK;
}

void GWP_CALL ApiNpcEnemyFilterReset(GwpObjectId id)
{
    if (!CheckCall("npc_enemy_filter_reset") || !g_pGameLevel)
        return;
    if (id != GWP_INVALID_OBJECT_ID)
    {
        if (CCustomMonster* npc = smart_cast<CCustomMonster*>(Level().Objects.net_Find(id)))
            ResetNpc(npc);
        return;
    }
    const u32 count = Level().Objects.o_count();
    for (u32 i = 0; i < count; ++i)
    {
        if (CCustomMonster* npc = smart_cast<CCustomMonster*>(Level().Objects.o_get_by_iterator(i)))
            ResetNpc(npc);
    }
}
} // namespace

void FillEngineApi(GwpEngineApi& api)
{
    api.enemy_filter_register = &ApiEnemyFilterRegister;
    api.patrol_extrapolate_register = &ApiPatrolExtrapolateRegister;
    api.callback_unregister = &ApiCallbackUnregister;
    api.npc_enemy_filter_reset = &ApiNpcEnemyFilterReset;
}

bool HasEnemyFilters() { return g_enemy_count != 0; }

bool EnemyUseful(const CCustomMonster* npc, const CEntityAlive* enemy)
{
    if (!g_enemy_count || !npc || !enemy)
        return true;
    return Ask(EKind::Enemy, npc->ID(), enemy->ID(), 0);
}

bool HasPatrolExtrapolate() { return g_patrol_count != 0; }

bool PatrolExtrapolate(u16 npc, u32 point_index)
{
    if (!g_patrol_count)
        return true;
    return Ask(EKind::Patrol, npc, 0, point_index);
}

void RemovePluginCallbacks(const GwpPlugin* plugin)
{
    const bool had_enemy = g_enemy_count != 0;
    RemoveOf(plugin);
    if (had_enemy && g_pGameLevel)
        ApiResetAll();
}

void Shutdown()
{
    xr_delete(g_callbacks);
    g_enemy_count = 0;
    g_patrol_count = 0;
    g_dispatch_depth = 0;
    g_has_dead = false;
}

void PrintList()
{
    const u32 count = g_enemy_count + g_patrol_count;
    Msg("- [callbacks] %u engine callback(s): %u enemy filter(s), %u patrol extrapolate", count, g_enemy_count,
        g_patrol_count);
    if (!g_callbacks)
        return;
    for (const Callback& callback : *g_callbacks)
    {
        if (callback.id == GWP_INVALID_CALLBACK_ID)
            continue;
        Msg("-   #%-4u %-24s %-18s calls=%llu vetoes=%llu", callback.id, PluginAddonId(callback.plugin),
            callback.kind == EKind::Enemy ? "enemy_filter" : "patrol_extrapolate",
            static_cast<unsigned long long>(callback.calls), static_cast<unsigned long long>(callback.vetoes));
    }
}
} // namespace gw::addons::callbacks
