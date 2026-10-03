#include "StdAfx.h"

#include "addon_binders.h"
#include "addon_event_bus.h"
#include "addon_host.h"

#include "GameObject.h"
#include "Level.h"
#include "npc_cpp_profile.h"

#include <algorithm>
#include <cctype>

namespace gw::addons::binders
{
namespace
{
constexpr u32 kMaxBinders = 64;     // one bit of the per-object mask each
constexpr u32 kObjectSlots = 65536; // every GwpObjectId
constexpr size_t kMaxMaskLength = 64;
constexpr u16 kInvalidObjectId = u16(-1); // the demo spectator and the like

struct Binder
{
    GwpBinderId id = GWP_INVALID_BINDER_ID;
    const GwpPlugin* plugin = nullptr;
    CLASS_ID class_id = 0;
    bool any_class = true;
    xr_string mask; // empty = any section
    GwpBinderVTable vtable{};
    void* user = nullptr;
    u32 bound = 0;  // objects attached now
    u64 spawns = 0; // statistics for binder_list
    u64 updates = 0;
    u64 batches = 0;
    xr_vector<GwpBinderUpdate> batch; // records collected since the previous on_update
};

Binder* g_slots[kMaxBinders] = {};
u64 g_active = 0; // bit i: g_slots[i] holds a binder
GwpBinderId g_next_id = 1;

// Per object: the slots it is attached to, chosen at OnObjectInit, and its state (kSpawned: on_spawn was
// delivered, on_destroy pairs with it; a spawn that failed never gets either).
u64* g_object_slots = nullptr;
u8* g_object_spawned = nullptr;
constexpr u8 kSpawned = 1;    // net_Spawn succeeded (OnObjectSpawn)
constexpr u8 kDestroying = 2; // OnObjectDestroy is sending on_destroy: a binder registered now is not attached

// A binder unregistered from inside a callback (its own or another one's) is deleted after the outermost
// dispatch returns: the frame that called it may still hold the pointer.
u32 g_dispatch_depth = 0;
xr_vector<Binder*>* g_graveyard = nullptr;

// Plugins that crashed in a binder callback: binder_register refuses them until the library is unloaded
// (ForgetPluginCrash: the host loads it again later).
xr_vector<const GwpPlugin*>* g_crashed = nullptr;

constexpr u64 Bit(u32 slot) { return u64(1) << slot; }

bool CheckCall(pcstr function)
{
    if (IsMainThread())
        return true;
    Msg("! [binders] %s called outside the main thread, ignored", function);
    return false;
}

void EnsureTables()
{
    if (g_object_slots)
        return;
    g_object_slots = xr_alloc<u64>(kObjectSlots);
    g_object_spawned = xr_alloc<u8>(kObjectSlots);
    memset(g_object_slots, 0, sizeof(u64) * kObjectSlots);
    memset(g_object_spawned, 0, sizeof(u8) * kObjectSlots);
}

int Lower(char c) { return std::tolower(static_cast<unsigned char>(c)); }

// Section mask: '*' any run of characters, '?' one character; case does not matter (ltx sections are
// case-insensitive). Iterative with one backtrack point, the usual glob without character classes.
bool MatchMask(pcstr mask, pcstr text)
{
    pcstr m = mask;
    pcstr t = text;
    pcstr star = nullptr;
    pcstr star_text = nullptr;
    while (*t)
    {
        if (*m == '?' || (*m && *m != '*' && Lower(*m) == Lower(*t)))
        {
            ++m;
            ++t;
        }
        else if (*m == '*')
        {
            star = m++;
            star_text = t;
        }
        else if (star)
        {
            m = star + 1;
            t = ++star_text;
        }
        else
            return false;
    }
    while (*m == '*')
        ++m;
    return *m == 0;
}

bool Matches(const Binder& binder, const CGameObject& object)
{
    if (!binder.any_class && object.CLS_ID != binder.class_id)
        return false;
    return binder.mask.empty() || MatchMask(binder.mask.c_str(), object.cNameSect_str());
}

// CLASS_ID as the text it was registered with, trailing spaces removed ("AI_STL_S", "SM_DOG_S").
void ClassIdText(CLASS_ID class_id, char out[9])
{
    CLSID2TEXT(class_id, out);
    for (int i = 7; i >= 0 && out[i] == ' '; --i)
        out[i] = 0;
}

Binder* FindBinder(GwpBinderId id, u32* out_slot = nullptr)
{
    if (id == GWP_INVALID_BINDER_ID)
        return nullptr;
    for (u32 i = 0; i < kMaxBinders; ++i)
    {
        if ((g_active & Bit(i)) && g_slots[i]->id == id)
        {
            if (out_slot)
                *out_slot = i;
            return g_slots[i];
        }
    }
    return nullptr;
}

// The slot still holds the binder `id`. A slot is reused: a callback may unregister a binder and register another
// one, which takes the first free slot - often the same one. A loop that remembered a slot (or a bit of it) must
// not hand the new binder a call meant for the old one, so the id is compared, not only the bit.
bool SameBinder(u32 slot, GwpBinderId id) { return (g_active & Bit(slot)) && g_slots[slot]->id == id; }

bool HasCrashed(const GwpPlugin* plugin)
{
    return g_crashed && std::find(g_crashed->begin(), g_crashed->end(), plugin) != g_crashed->end();
}

// ---------------------------------------------------------------------------------------------
// Guarded calls into the plugin
// ---------------------------------------------------------------------------------------------

enum class ECallback
{
    Reinit,
    Spawn,
    Destroy,
    Update,
};
constexpr pcstr kCallbackNames[] = { "binder on_reinit", "binder on_spawn", "binder on_destroy", "binder on_update" };

struct Call
{
    const Binder* binder;
    ECallback kind;
    u16 id;
    pcstr section;
    u32 count;
    const GwpBinderUpdate* updates;
};

void Thunk(void* context)
{
    const Call& call = *static_cast<const Call*>(context);
    const GwpBinderVTable& vt = call.binder->vtable;
    void* const user = call.binder->user;
    switch (call.kind)
    {
    case ECallback::Reinit: vt.on_reinit(user, call.id); break;
    case ECallback::Spawn: vt.on_spawn(user, call.id, call.section); break;
    case ECallback::Destroy: vt.on_destroy(user, call.id); break;
    case ECallback::Update: vt.on_update(user, call.count, call.updates); break;
    }
}

const void* CallbackOf(const Binder& binder, ECallback kind)
{
    switch (kind)
    {
    case ECallback::Reinit: return reinterpret_cast<const void*>(binder.vtable.on_reinit);
    case ECallback::Spawn: return reinterpret_cast<const void*>(binder.vtable.on_spawn);
    case ECallback::Destroy: return reinterpret_cast<const void*>(binder.vtable.on_destroy);
    case ECallback::Update: return reinterpret_cast<const void*>(binder.vtable.on_update);
    }
    return nullptr;
}

void FlushGraveyard()
{
    if (g_dispatch_depth || !g_graveyard)
        return;
    for (Binder* binder : *g_graveyard)
        xr_delete(binder);
    g_graveyard->clear();
}

// Calls one callback of the binder. false: the plugin crashed in it; its binders are removed by then and `binder`
// must not be touched by the caller anymore.
bool Invoke(Binder& binder, ECallback kind, u16 id, pcstr section = nullptr, u32 count = 0,
    const GwpBinderUpdate* updates = nullptr)
{
    const void* callback = CallbackOf(binder, kind);
    if (!callback)
        return true;
    Call call{ &binder, kind, id, section, count, updates };
    const GwpPlugin* const plugin = binder.plugin;
    const pcstr name = kCallbackNames[static_cast<size_t>(kind)];
    ++g_dispatch_depth;
    const bool ok = events::CallPluginGuarded(callback, name, &Thunk, &call);
    --g_dispatch_depth;
    if (!ok)
    {
        if (!g_crashed)
            g_crashed = xr_new<xr_vector<const GwpPlugin*>>();
        g_crashed->push_back(plugin);
        OnPluginCrashed(plugin, name); // "binder on_spawn" and so on: the whole plugin stops, its binders with it
        RemovePluginBinders(plugin);   // and for sure: the caller must not touch `binder` any more
    }
    FlushGraveyard();
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Attaching and detaching
// ---------------------------------------------------------------------------------------------

// Ids of the online objects: a snapshot, because callbacks may spawn or destroy objects while we walk.
void OnlineObjectIds(xr_vector<u16>& out)
{
    out.clear();
    if (!g_pGameLevel)
        return;
    const u32 count = Level().Objects.o_count();
    out.reserve(count);
    for (u32 i = 0; i < count; ++i)
    {
        const IGameObject* object = Level().Objects.o_get_by_iterator(i);
        if (object && !object->getDestroy() && object->ID() != kInvalidObjectId)
            out.push_back(object->ID());
    }
}

CGameObject* OnlineObject(u16 id)
{
    if (!g_pGameLevel)
        return nullptr;
    IGameObject* object = Level().Objects.net_Find(id);
    return object && !object->getDestroy() ? smart_cast<CGameObject*>(object) : nullptr;
}

// Forgets the slot: the bit leaves every object mask, the binder is deleted (or parked until the dispatch that
// called us returns). No callbacks.
void RemoveSlot(u32 slot)
{
    Binder* binder = g_slots[slot];
    if (!binder)
        return;
    const u64 bit = Bit(slot);
    g_active &= ~bit;
    g_slots[slot] = nullptr;
    if (g_object_slots)
    {
        for (u32 id = 0; id < kObjectSlots; ++id)
            g_object_slots[id] &= ~bit;
    }
    binder->bound = 0;
    if (g_dispatch_depth)
    {
        if (!g_graveyard)
            g_graveyard = xr_new<xr_vector<Binder*>>();
        g_graveyard->push_back(binder);
    }
    else
        xr_delete(binder);
}

// Attaches a new binder to the objects that are online already: they get on_spawn now (no on_reinit, that
// point is in the past). An object in the middle of its net_Spawn (the binder is registered from npc_on_net_spawn,
// from on_reinit) is attached without a call: OnObjectSpawn that follows delivers its on_spawn. false when the
// binder is gone: the plugin crashed in one of the calls.
bool AttachOnline(u32 slot, GwpBinderId binder_id)
{
    xr_vector<u16> ids;
    OnlineObjectIds(ids);
    const u64 bit = Bit(slot);
    for (const u16 id : ids)
    {
        if (!SameBinder(slot, binder_id))
            return false; // removed by a crash in an earlier call
        Binder& binder = *g_slots[slot];
        const CGameObject* object = OnlineObject(id);
        if (!object || (g_object_spawned[id] & kDestroying) || (g_object_slots[id] & bit) || !Matches(binder, *object))
            continue;
        g_object_slots[id] |= bit;
        ++binder.bound;
        if (!(g_object_spawned[id] & kSpawned))
            continue; // net_Spawn is not over: on_spawn comes from OnObjectSpawn
        ++binder.spawns;
        if (!Invoke(binder, ECallback::Spawn, id, object->cNameSect_str()))
            return false;
    }
    return SameBinder(slot, binder_id); // a crash of the plugin in a nested call removes it as well
}

// binder_unregister: every bound object gets on_destroy first, so the plugin can drop its per-object state.
void DetachAll(u32 slot, GwpBinderId binder_id)
{
    xr_vector<u16> ids;
    OnlineObjectIds(ids);
    const u64 bit = Bit(slot);
    for (const u16 id : ids)
    {
        // Removed by a crash in an earlier call, or unregistered from inside it (the slot may hold another binder)
        if (!SameBinder(slot, binder_id))
            return;
        if (!(g_object_slots[id] & bit))
            continue;
        Binder& binder = *g_slots[slot];
        g_object_slots[id] &= ~bit;
        --binder.bound;
        if ((g_object_spawned[id] & kSpawned) && !Invoke(binder, ECallback::Destroy, id))
            return;
    }
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group binders)
// ---------------------------------------------------------------------------------------------

GwpBinderId GWP_CALL ApiBinderRegister(const GwpPlugin* self, const char* class_id, const char* section_mask,
    const GwpBinderVTable* vtable, void* user)
{
    if (!CheckCall("binder_register") || !self)
        return GWP_INVALID_BINDER_ID;
    const pcstr addon = PluginAddonId(self);
    const bool has_class = class_id && class_id[0];
    const bool has_mask = section_mask && section_mask[0];
    if (!vtable || vtable->size < 2 * sizeof(uint32_t))
    {
        Msg("! [plugin:%s] binder_register: vtable is missing or its size is not set", addon);
        return GWP_INVALID_BINDER_ID;
    }
    if (!has_class && !has_mask)
    {
        Msg("! [plugin:%s] binder_register: a class id or a section mask is required", addon);
        return GWP_INVALID_BINDER_ID;
    }
    if (has_class && xr_strlen(class_id) > 8)
    {
        Msg("! [plugin:%s] binder_register: class id '%s' is longer than 8 characters", addon, class_id);
        return GWP_INVALID_BINDER_ID;
    }
    if (has_mask && xr_strlen(section_mask) > kMaxMaskLength)
    {
        Msg("! [plugin:%s] binder_register: section mask is longer than %u characters", addon,
            static_cast<u32>(kMaxMaskLength));
        return GWP_INVALID_BINDER_ID;
    }
    if (HasCrashed(self))
    {
        Msg("! [plugin:%s] binder_register refused: the plugin crashed in a binder callback earlier", addon);
        return GWP_INVALID_BINDER_ID;
    }

    u32 slot = kMaxBinders;
    for (u32 i = 0; i < kMaxBinders; ++i)
    {
        if (!(g_active & Bit(i)))
        {
            slot = i;
            break;
        }
    }
    if (slot == kMaxBinders)
    {
        Msg("! [plugin:%s] binder_register: all %u binder slots are taken", addon, kMaxBinders);
        return GWP_INVALID_BINDER_ID;
    }

    EnsureTables();
    Binder* binder = xr_new<Binder>();
    binder->id = g_next_id++;
    binder->plugin = self;
    binder->any_class = !has_class;
    binder->class_id = has_class ? TEXT2CLSID(class_id) : 0;
    if (has_mask)
        binder->mask = section_mask;
    binder->user = user;
    // Only the part the plugin knows about: a plugin built against a longer vtable of a later version leaves
    // the rest zero, a plugin with a shorter one gets NULL for the callbacks it does not have.
    binder->vtable = {};
    memcpy(&binder->vtable, vtable, std::min<size_t>(vtable->size, sizeof(GwpBinderVTable)));
    binder->vtable.size = sizeof(GwpBinderVTable);
    g_slots[slot] = binder;
    g_active |= Bit(slot);

    const GwpBinderId id = binder->id;
    if (!AttachOnline(slot, id))
        return GWP_INVALID_BINDER_ID; // crashed in on_spawn of an object that was online: the binder is gone
    if (IsDebugLog())
    {
        Msg("* [plugin:%s] binder #%u registered: class '%s', mask '%s', %u object(s) online", addon, id,
            has_class ? class_id : "*", has_mask ? section_mask : "*", g_slots[slot]->bound);
    }
    return id;
}

GwpResult GWP_CALL ApiBinderUnregister(const GwpPlugin* self, GwpBinderId id)
{
    if (!CheckCall("binder_unregister"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    u32 slot = 0;
    const Binder* binder = FindBinder(id, &slot);
    if (!binder || binder->plugin != self)
    {
        if (IsDebugLog())
            Msg("~ [plugin:%s] binder_unregister: no binder #%u of this plugin", PluginAddonId(self), id);
        return GWP_ERROR_INVALID_ARGUMENT;
    }
    DetachAll(slot, id);
    // Not removed by a crash inside on_destroy, nor unregistered from it: then the slot may hold a newer binder
    if (SameBinder(slot, id))
        RemoveSlot(slot);
    FlushGraveyard();
    if (IsDebugLog())
        Msg("* [plugin:%s] binder #%u unregistered", PluginAddonId(self), id);
    return GWP_OK;
}

uint32_t GWP_CALL ApiBinderObjectCount(GwpBinderId id)
{
    if (!CheckCall("binder_object_count"))
        return 0;
    const Binder* binder = FindBinder(id);
    return binder ? binder->bound : 0;
}
} // namespace

void FillEngineApi(GwpEngineApi& api)
{
    api.binder_register = &ApiBinderRegister;
    api.binder_unregister = &ApiBinderUnregister;
    api.binder_object_count = &ApiBinderObjectCount;
    EnsureTables(); // the spawn flags are kept from the first object on, binders or not
}

// ---------------------------------------------------------------------------------------------
// Engine points
// ---------------------------------------------------------------------------------------------

void OnObjectInit(const CGameObject* object)
{
    if (!g_object_slots || !object || object->ID() == kInvalidObjectId)
        return;
    const u16 id = object->ID();
    // The previous life of this id ended in net_Destroy, which cleared the entry; if it did not, the counts
    // would drift, so the old attachments are dropped first (no callbacks: that object is long gone).
    if (g_object_slots[id])
    {
        for (u32 i = 0; i < kMaxBinders; ++i)
        {
            if ((g_object_slots[id] & g_active & Bit(i)) && g_slots[i]->bound)
                --g_slots[i]->bound;
        }
    }
    g_object_slots[id] = 0;
    g_object_spawned[id] = 0;
    if (!g_active)
        return;
    u64 slots = 0;
    for (u32 i = 0; i < kMaxBinders; ++i)
    {
        if ((g_active & Bit(i)) && Matches(*g_slots[i], *object))
        {
            slots |= Bit(i);
            ++g_slots[i]->bound;
        }
    }
    g_object_slots[id] = slots;
}

void OnObjectReinit(const CGameObject* object)
{
    if (!g_active || !object || object->ID() == kInvalidObjectId)
        return;
    const u16 id = object->ID();
    for (u32 i = 0; i < kMaxBinders; ++i)
    {
        // re-read every time: a crash in one call removes binders
        if (g_object_slots[id] & g_active & Bit(i))
            Invoke(*g_slots[i], ECallback::Reinit, id);
    }
}

void OnObjectSpawn(const CGameObject* object)
{
    if (!g_object_spawned || !object || object->ID() == kInvalidObjectId)
        return;
    const u16 id = object->ID();
    g_object_spawned[id] = kSpawned;
    if (!g_active)
        return;
    // The binders attached before the loop, by id: a binder registered from one of these on_spawn calls got its
    // own on_spawn in AttachOnline already (the object is spawned now), and a slot freed and taken again inside
    // the loop holds a binder that is not ours to call
    const u64 slots = g_object_slots[id] & g_active;
    GwpBinderId binder_ids[kMaxBinders];
    for (u32 i = 0; i < kMaxBinders; ++i)
        binder_ids[i] = (slots & Bit(i)) ? g_slots[i]->id : GWP_INVALID_BINDER_ID;
    const pcstr section = object->cNameSect_str();
    for (u32 i = 0; i < kMaxBinders; ++i)
    {
        // re-read every time: a crash in one call removes binders, an on_spawn may unregister one
        if (!(slots & Bit(i)) || !(g_object_slots[id] & Bit(i)) || !SameBinder(i, binder_ids[i]))
            continue;
        ++g_slots[i]->spawns;
        Invoke(*g_slots[i], ECallback::Spawn, id, section);
    }
}

void OnObjectDestroy(const CGameObject* object)
{
    if (!g_object_slots || !object || object->ID() == kInvalidObjectId)
        return;
    const u16 id = object->ID();
    const bool spawned = (g_object_spawned[id] & kSpawned) != 0;
    const u64 slots = g_object_slots[id] & g_active;
    if (!slots)
    {
        g_object_spawned[id] = 0;
        g_object_slots[id] = 0;
        return;
    }
    // The bits leave one by one, right before each call: a binder unregistered from one of these on_destroy calls
    // still sends this object its on_destroy (DetachAll), and only once. A binder registered meanwhile is not
    // attached to the object (kDestroying), and a slot taken again holds a binder that is not ours (the id).
    g_object_spawned[id] |= kDestroying;
    GwpBinderId binder_ids[kMaxBinders];
    for (u32 i = 0; i < kMaxBinders; ++i)
        binder_ids[i] = (slots & Bit(i)) ? g_slots[i]->id : GWP_INVALID_BINDER_ID;
    for (u32 i = 0; i < kMaxBinders; ++i)
    {
        const u64 bit = Bit(i);
        if (!(slots & bit) || !(g_object_slots[id] & bit) || !SameBinder(i, binder_ids[i]))
            continue;
        g_object_slots[id] &= ~bit;
        Binder& binder = *g_slots[i];
        if (binder.bound)
            --binder.bound;
        if (spawned)
            Invoke(binder, ECallback::Destroy, id);
    }
    g_object_spawned[id] = 0;
    g_object_slots[id] = 0;
}

void OnObjectUpdate(const CGameObject* object, u32 dt_ms)
{
    if (!g_active || !object)
        return;
    const u16 id = object->ID();
    const u64 slots = g_object_slots[id] & g_active;
    if (!slots)
        return;
    const GwpBinderUpdate record{ id, 0, dt_ms };
    for (u32 i = 0; i < kMaxBinders; ++i)
    {
        if (slots & Bit(i))
            g_slots[i]->batch.push_back(record);
    }
}

void FlushUpdates()
{
    if (!g_active)
        return;
    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::AddonBinderUpdate);
    for (u32 i = 0; i < kMaxBinders; ++i)
    {
        const u64 bit = Bit(i);
        if (!(g_active & bit) || g_slots[i]->batch.empty())
            continue;
        Binder& binder = *g_slots[i];
        xr_vector<GwpBinderUpdate>& batch = binder.batch;
        // Objects that went offline during the pass are dropped: their on_destroy was the last call for the id.
        batch.erase(std::remove_if(batch.begin(), batch.end(),
                        [bit](const GwpBinderUpdate& record) { return !(g_object_slots[record.id] & bit); }),
            batch.end());
        if (batch.empty() || !binder.vtable.on_update)
        {
            batch.clear();
            continue;
        }
        binder.updates += batch.size();
        ++binder.batches;
        // The records move out first: the callback may unregister its own binder (deleted when the dispatch
        // returns) or crash (the binder goes the same way), so nothing of `binder` is touched after the call
        xr_vector<GwpBinderUpdate> records;
        records.swap(batch);
        ZoneScopedN("binders/on_update"); // Tracy: one batch of a plugin binder, text = addon and record count
        ZoneTextF("%s: %u", PluginAddonId(binder.plugin), static_cast<u32>(records.size()));
        Invoke(binder, ECallback::Update, 0, nullptr, static_cast<u32>(records.size()), records.data());
    }
}

// ---------------------------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------------------------

void RemovePluginBinders(const GwpPlugin* plugin)
{
    for (u32 i = 0; i < kMaxBinders; ++i)
    {
        if ((g_active & Bit(i)) && g_slots[i]->plugin == plugin)
            RemoveSlot(i);
    }
    FlushGraveyard();
}

void ForgetPluginCrash(const GwpPlugin* plugin)
{
    if (g_crashed)
        g_crashed->erase(std::remove(g_crashed->begin(), g_crashed->end(), plugin), g_crashed->end());
}

void Shutdown()
{
    for (u32 i = 0; i < kMaxBinders; ++i)
        RemoveSlot(i);
    g_dispatch_depth = 0;
    FlushGraveyard();
    xr_delete(g_graveyard);
    xr_delete(g_crashed);
    xr_free(g_object_slots);
    xr_free(g_object_spawned);
    g_object_slots = nullptr;
    g_object_spawned = nullptr;
    g_active = 0;
}

void PrintList()
{
    u32 count = 0;
    for (u32 i = 0; i < kMaxBinders; ++i)
        count += (g_active & Bit(i)) ? 1 : 0;
    Msg("- [binders] %u binder(s), %u slot(s) free:", count, kMaxBinders - count);
    for (u32 i = 0; i < kMaxBinders; ++i)
    {
        if (!(g_active & Bit(i)))
            continue;
        const Binder& binder = *g_slots[i];
        char class_text[9] = "*";
        if (!binder.any_class)
            ClassIdText(binder.class_id, class_text);
        Msg("-   #%-4u %-24s class=%-8s mask=%-24s bound=%-5u spawns=%llu updates=%llu batches=%llu", binder.id,
            PluginAddonId(binder.plugin), class_text, binder.mask.empty() ? "*" : binder.mask.c_str(), binder.bound,
            static_cast<unsigned long long>(binder.spawns), static_cast<unsigned long long>(binder.updates),
            static_cast<unsigned long long>(binder.batches));
    }
}
} // namespace gw::addons::binders
