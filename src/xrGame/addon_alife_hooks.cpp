#include "StdAfx.h"

// Plugin API, group alife: the offline update of squads and smart terrains by a plugin (alife_update_hook_*).
// Design: plans/lua_to_cpp/31-gw-eoc-e36-spawn-api.md, 4.4. Docs: wiki/doc/plugins/api/alife.md
//
// The ALife scheduler updates the offline squads (sim_squad_scripted, "ON_OFF_S") and smart terrains
// (se_smart_terrain, "SMRTTRRN") through their virtual update(): the luabind wrapper calls the Lua update of the class,
// which calls the C++ update of the class first (cse_alife_online_offline_group.update(self) /
// cse_alife_smart_zone.update(self)). A hook of a class changes that per object:
//  - GWP_UPDATE_LUA: nothing (the hook is not active, Update is not even asked);
//  - GWP_UPDATE_NATIVE: fn(QUERY); GWP_UPDATE_HANDLE - the C++ update of the class (non-virtual), then fn(RUN), no Lua;
//    any other answer - the virtual update as before. A squad without members always gets the virtual update: the Lua
//    update releases an empty squad (remove_squad) or skips a released one (__gw_released, its members are gone).
//  - GWP_UPDATE_SHADOW: the virtual update, then fn(SHADOW).
// The calls are guarded as every plugin callback (events::CallPluginGuarded): a crash stops the plugin, its hooks go.

#include "addon_alife_hooks.h"
#include "addon_api_common.h"
#include "addon_event_bus.h"
#include "addon_host.h"

#include "xrCore/clsid.h"
#include "xrServer_Objects_ALife.h"
#include "xrServer_Objects_ALife_Monsters.h"

#include <cstring>

namespace gw::addons::alife_hooks
{
u32 g_active = 0;

namespace
{
// One hook per class: a slot per supported class
constexpr u32 kHookSquad = 0;
constexpr u32 kHookSmart = 1;
constexpr u32 kHookKinds = 2;
constexpr CLASS_ID kHookClassIds[kHookKinds] = { xray::make_clsid("ON_OFF_S"), xray::make_clsid("SMRTTRRN") };
constexpr pcstr kHookClassNames[kHookKinds] = { "ON_OFF_S", "SMRTTRRN" };

struct HookSlot
{
    GwpUpdateHookId id = GWP_INVALID_UPDATE_HOOK_ID; // the invalid id: a free slot
    const GwpPlugin* plugin = nullptr;
    GwpAlifeUpdateFn fn = nullptr;
    void* user = nullptr;
    u32 mode = GWP_UPDATE_LUA;
    u64 queries = 0;   // fn(QUERY) calls
    u64 handled = 0;   // answered GWP_UPDATE_HANDLE: the Lua update skipped
    u64 fallbacks = 0; // the virtual update in GWP_UPDATE_NATIVE (other answers, squads without members)
    u64 shadows = 0;   // fn(SHADOW) calls
    u64 ticks = 0;     // CPU::QPC ticks inside fn
};

HookSlot g_hook_slots[kHookKinds];
GwpUpdateHookId g_hook_next_id = 1;

pcstr HookModeName(u32 mode)
{
    switch (mode)
    {
    case GWP_UPDATE_LUA: return "lua";
    case GWP_UPDATE_NATIVE: return "native";
    case GWP_UPDATE_SHADOW: return "shadow";
    default: return "?";
    }
}

void HookRecount()
{
    u32 active = 0;
    for (const HookSlot& slot : g_hook_slots)
    {
        if (slot.id != GWP_INVALID_UPDATE_HOOK_ID && slot.mode != GWP_UPDATE_LUA)
            ++active;
    }
    g_active = active;
}

// The slot of a class id text ("ON_OFF_S"; trailing spaces do not matter, as in GwpAlifeFilter::class_id)
bool HookKindOfClass(pcstr class_id, u32& kind)
{
    if (!class_id)
        return false;
    size_t length = std::strlen(class_id);
    while (length && class_id[length - 1] == ' ')
        --length;
    for (u32 i = 0; i < kHookKinds; ++i)
    {
        if (length == std::strlen(kHookClassNames[i]) && std::strncmp(class_id, kHookClassNames[i], length) == 0)
        {
            kind = i;
            return true;
        }
    }
    return false;
}

// The slot of the hook `id`, or nullptr
HookSlot* HookFind(GwpUpdateHookId id)
{
    if (id == GWP_INVALID_UPDATE_HOOK_ID)
        return nullptr;
    for (HookSlot& slot : g_hook_slots)
    {
        if (slot.id == id)
            return &slot;
    }
    return nullptr;
}

bool HookModeValid(u32 mode) { return !(mode & GWP_MODE_FLAGS_MASK) && mode <= GWP_UPDATE_SHADOW; }

struct HookInvocation
{
    GwpAlifeUpdateFn fn;
    void* user;
    GwpObjectId object;
    u32 phase;
    int result;
};

void HookThunk(void* context)
{
    HookInvocation& call = *static_cast<HookInvocation*>(context);
    call.result = call.fn(call.user, call.object, call.phase);
}

// fn of the hook in slot `kind` with the id `id`, guarded. false: the hook is gone (unregistered from a call before)
// or crashed now (the plugin is stopped: its hooks are removed, OnPluginCrashed); `result` untouched then.
bool HookCall(u32 kind, GwpUpdateHookId id, GwpObjectId object, u32 phase, int& result)
{
    HookSlot& slot = g_hook_slots[kind];
    if (slot.id != id)
        return false;
    const GwpPlugin* plugin = slot.plugin;
    HookInvocation call{ slot.fn, slot.user, object, phase, GWP_UPDATE_FALLBACK };
    const u64 start = CPU::QPC();
    const bool done = events::CallPluginGuarded(
        reinterpret_cast<const void*>(call.fn), "alife update hook", &HookThunk, &call);
    const u64 spent = CPU::QPC() - start;
    if (!done)
    {
        RemovePluginHooks(plugin);
        OnPluginCrashed(plugin, "alife update hook"); // the whole plugin stops
        return false;
    }
    // The slot may have been emptied (or even taken again) by the call itself: count for the same hook only
    if (g_hook_slots[kind].id == id)
        g_hook_slots[kind].ticks += spent;
    result = call.result;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group alife)
// ---------------------------------------------------------------------------------------------

GwpUpdateHookId GWP_CALL ApiAlifeUpdateHookRegister(const GwpPlugin* self, const char* class_id, GwpAlifeUpdateFn fn,
    void* user, uint32_t mode)
{
    constexpr pcstr function = "alife_update_hook_register";
    if (!api::CheckPluginMutation(self, "alife", function))
        return GWP_INVALID_UPDATE_HOOK_ID;
    const pcstr addon = PluginAddonId(self);
    if (!fn)
    {
        Msg("! [plugin:%s] %s: no function", addon, function);
        return GWP_INVALID_UPDATE_HOOK_ID;
    }
    if (!HookModeValid(mode))
    {
        Msg("! [plugin:%s] %s: %s (mode 0x%08x)", addon, function,
            (mode & GWP_MODE_FLAGS_MASK) ? "a flag bit in mode" : "unknown mode", mode);
        return GWP_INVALID_UPDATE_HOOK_ID;
    }
    u32 kind = 0;
    if (!HookKindOfClass(class_id, kind))
    {
        Msg("! [plugin:%s] %s: class '%s' is not supported (\"ON_OFF_S\" or \"SMRTTRRN\")", addon, function,
            class_id ? class_id : "");
        return GWP_INVALID_UPDATE_HOOK_ID;
    }
    HookSlot& slot = g_hook_slots[kind];
    if (slot.id != GWP_INVALID_UPDATE_HOOK_ID)
    {
        Msg("! [plugin:%s] %s: class %s is taken by addon '%s'", addon, function, kHookClassNames[kind],
            PluginAddonId(slot.plugin));
        return GWP_INVALID_UPDATE_HOOK_ID;
    }
    slot = HookSlot{};
    slot.id = g_hook_next_id++;
    if (g_hook_next_id == GWP_INVALID_UPDATE_HOOK_ID) // the counter wrapped: 0 is the invalid id
        g_hook_next_id = 1;
    slot.plugin = self;
    slot.fn = fn;
    slot.user = user;
    slot.mode = mode;
    HookRecount();
    Msg("* [plugin:%s] alife update hook #%u: class %s, mode %s", addon, slot.id, kHookClassNames[kind],
        HookModeName(mode));
    return slot.id;
}

GwpResult GWP_CALL ApiAlifeUpdateHookSetMode(const GwpPlugin* self, GwpUpdateHookId hook, uint32_t mode)
{
    constexpr pcstr function = "alife_update_hook_set_mode";
    if (!api::CheckPluginMutation(self, "alife", function))
        return api::PluginCallRefused(self);
    HookSlot* slot = HookFind(hook);
    if (!slot)
        return api::PluginRefusal(self, function, GWP_ERROR_NOT_FOUND, "no such hook");
    if (slot->plugin != self)
        return api::PluginRefusal(self, function, GWP_ERROR_ACCESS_DENIED, "a hook of another plugin");
    if (!HookModeValid(mode)) // a flag bit (none defined yet) or a mode above GWP_UPDATE_SHADOW: of a later version
        return api::PluginRefusal(self, function, GWP_ERROR_NOT_SUPPORTED, "unknown mode or a flag bit");
    if (slot->mode != mode)
        Msg("* [plugin:%s] alife update hook #%u: mode %s -> %s", PluginAddonId(self), hook, HookModeName(slot->mode),
            HookModeName(mode));
    slot->mode = mode;
    HookRecount();
    return GWP_OK;
}

GwpResult GWP_CALL ApiAlifeUpdateHookUnregister(const GwpPlugin* self, GwpUpdateHookId hook)
{
    constexpr pcstr function = "alife_update_hook_unregister";
    if (!api::CheckPluginMutation(self, "alife", function))
        return api::PluginCallRefused(self);
    HookSlot* slot = HookFind(hook);
    if (!slot)
        return api::PluginRefusal(self, function, GWP_ERROR_NOT_FOUND, "no such hook");
    if (slot->plugin != self)
        return api::PluginRefusal(self, function, GWP_ERROR_ACCESS_DENIED, "a hook of another plugin");
    *slot = HookSlot{};
    HookRecount();
    if (IsDebugLog())
        Msg("~ [plugin:%s] alife update hook #%u unregistered", PluginAddonId(self), hook);
    return GWP_OK;
}
} // namespace

void FillEngineApi(GwpEngineApi& api)
{
    api.alife_update_hook_register = &ApiAlifeUpdateHookRegister;
    api.alife_update_hook_set_mode = &ApiAlifeUpdateHookSetMode;
    api.alife_update_hook_unregister = &ApiAlifeUpdateHookUnregister;
}

bool Update(CSE_ALifeSchedulable& object)
{
    CSE_Abstract* base = object.base();
    if (!base)
        return false;
    u32 kind = kHookKinds;
    for (u32 i = 0; i < kHookKinds; ++i)
    {
        if (base->m_tClassID == kHookClassIds[i])
        {
            kind = i;
            break;
        }
    }
    if (kind == kHookKinds)
        return false;
    HookSlot& slot = g_hook_slots[kind];
    if (slot.id == GWP_INVALID_UPDATE_HOOK_ID || slot.mode == GWP_UPDATE_LUA)
        return false;
    // Plugins run on the game logic thread only: an update from elsewhere goes as without them
    if (!IsMainThread())
        return false;
    // The mode of this update; a change made by fn applies from the next one
    const GwpUpdateHookId id = slot.id;
    const u32 mode = slot.mode;
    const GwpObjectId object_id = base->ID;
    int answer = GWP_UPDATE_FALLBACK;

    if (mode == GWP_UPDATE_SHADOW)
    {
        object.update(); // the Lua update, as without the hook
        // The update may have released the object: deleted only after the pass (destruction_lock), the id stays valid
        // for fn, the functions of the API answer "no such object" for it
        if (HookCall(kind, id, object_id, GWP_UPDATE_PHASE_SHADOW, answer) && g_hook_slots[kind].id == id)
            ++g_hook_slots[kind].shadows;
        return true;
    }

    CSE_ALifeOnlineOfflineGroup* group = nullptr;
    CSE_ALifeSmartZone* smart = nullptr;
    if (kind == kHookSquad)
    {
        group = smart_cast<CSE_ALifeOnlineOfflineGroup*>(base);
        // Empty: the Lua update releases it (remove_squad) or skips it (released already, __gw_released)
        if (!group || group->npc_count() == 0)
        {
            ++slot.fallbacks;
            return false;
        }
    }
    else
    {
        smart = smart_cast<CSE_ALifeSmartZone*>(base);
        if (!smart)
            return false;
    }
    if (!HookCall(kind, id, object_id, GWP_UPDATE_PHASE_QUERY, answer))
        return false; // crashed: the Lua update, so the object is not skipped
    HookSlot& after = g_hook_slots[kind];
    if (after.id != id)
        return false; // unregistered by fn itself: the Lua update
    ++after.queries;
    // Only GWP_UPDATE_HANDLE takes the update: GWP_UPDATE_FALLBACK, any other value and an answer never written
    // (answer starts as GWP_UPDATE_FALLBACK) give it to Lua
    if (answer != GWP_UPDATE_HANDLE)
    {
        ++after.fallbacks;
        return false;
    }
    ++after.handled;
    // The C++ update of the class, the part the Lua update calls first (cse_alife_*.update(self)): non-virtual, so
    // the luabind wrapper and the Lua update are skipped
    if (group)
        group->CSE_ALifeOnlineOfflineGroup::update();
    else
        smart->CSE_ALifeSmartZone::update();
    HookCall(kind, id, object_id, GWP_UPDATE_PHASE_RUN, answer); // a crash here: no more update this time
    return true;
}

void RemovePluginHooks(const GwpPlugin* plugin)
{
    bool removed = false;
    for (HookSlot& slot : g_hook_slots)
    {
        if (slot.id != GWP_INVALID_UPDATE_HOOK_ID && slot.plugin == plugin)
        {
            slot = HookSlot{};
            removed = true;
        }
    }
    if (removed)
    {
        HookRecount();
        if (IsDebugLog())
            Msg("~ [plugin:%s] alife update hooks removed", PluginAddonId(plugin));
    }
}

void Shutdown()
{
    for (HookSlot& slot : g_hook_slots)
        slot = HookSlot{};
    g_active = 0;
}

void PrintList()
{
    u32 count = 0;
    for (const HookSlot& slot : g_hook_slots)
        count += slot.id != GWP_INVALID_UPDATE_HOOK_ID ? 1 : 0;
    Msg("- [alife] %u alife update hook(s), %u active", count, g_active);
    const double ms_per_tick = 1000.0 / static_cast<double>(CPU::qpc_freq);
    for (u32 kind = 0; kind < kHookKinds; ++kind)
    {
        const HookSlot& slot = g_hook_slots[kind];
        if (slot.id == GWP_INVALID_UPDATE_HOOK_ID)
            continue;
        Msg("-   #%-4u %-24s %-8s %-6s queries=%llu handled=%llu fallbacks=%llu shadows=%llu fn_ms=%.2f", slot.id,
            PluginAddonId(slot.plugin), kHookClassNames[kind], HookModeName(slot.mode),
            static_cast<unsigned long long>(slot.queries), static_cast<unsigned long long>(slot.handled),
            static_cast<unsigned long long>(slot.fallbacks), static_cast<unsigned long long>(slot.shadows),
            slot.ticks * ms_per_tick);
    }
}
} // namespace gw::addons::alife_hooks
