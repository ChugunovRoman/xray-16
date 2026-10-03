#include "StdAfx.h"

#include "addon_binders.h"
#include "addon_event_bus.h"
#include "addon_host.h"

#include "ai_space.h"
#include "alife_object_registry.h"
#include "alife_simulator.h"
#include "GameObject.h"
#include "Level.h"
#include "npc_cpp_profile.h"

#include <algorithm>
#include <cctype>
#include <cstddef>

namespace gw::addons::binders
{
namespace
{
constexpr u32 kMaxBinders = 64;     // one bit of the per-object mask each
constexpr u32 kObjectSlots = 65536; // every GwpObjectId
constexpr size_t kMaxMaskLength = 64;
constexpr u16 kInvalidObjectId = u16(-1); // the demo spectator and the like

// The saved state of one object: its section when on_save gave the bytes (a different section later = another
// object) and the bytes.
struct SavedObject
{
    xr_string section;
    xr_vector<u8> data;
};

struct Binder
{
    GwpBinderId id = GWP_INVALID_BINDER_ID;
    const GwpPlugin* plugin = nullptr;
    CLASS_ID class_id = 0;
    bool any_class = true;
    xr_string mask; // empty = any section
    xr_string save_key; // the vtable has on_save or on_load: the key of its saved state (SaveKeyOf), else empty
    GwpBinderVTable vtable{};
    void* user = nullptr;
    u32 bound = 0;  // objects attached now
    u64 spawns = 0; // statistics for binder_list
    u64 updates = 0;
    u64 batches = 0;
    xr_vector<GwpBinderUpdate> batch; // records collected since the previous on_update
    // Binders with on_save: the last bytes the engine knows for each bound object (handed to on_load, or given by
    // on_save of a save). Not asked again: RemoveSlot (a crash, an unload without binder_unregister) moves them to
    // g_saved, WriteSave writes them for objects it could not ask. Only bound objects of this game have an entry.
    xr_map<u16, SavedObject> shadow;
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
constexpr u8 kThisGame = 4;   // the object came online in the game the saved state belongs to (ResetSave, ReadSave)
constexpr u8 kReleased = 8;   // its server object is released (OnServerObjectRelease): nothing is saved for the id

// A binder unregistered from inside a callback (its own or another one's) is deleted after the outermost
// dispatch returns: the frame that called it may still hold the pointer.
u32 g_dispatch_depth = 0;

// The depth comes back down also when something unwinds through a dispatch: a depth stuck above zero would keep the
// graveyard from being emptied for the rest of the run
struct DispatchDepthScope
{
    DispatchDepthScope() { ++g_dispatch_depth; }
    ~DispatchDepthScope() { --g_dispatch_depth; }
    DispatchDepthScope(const DispatchDepthScope&) = delete;
    DispatchDepthScope& operator=(const DispatchDepthScope&) = delete;
};
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
// Saved state per object (on_save / on_load)
// ---------------------------------------------------------------------------------------------

// Kept per (addon, class id, section mask): the binder id is new in every session, the registration is the same.
struct SavedBinder
{
    xr_string addon;
    xr_string class_text; // "" = any class
    xr_string mask;       // "" = any section, lower case
    xr_map<u16, SavedObject> objects;
};

// key = SaveKeyOf(addon, class, mask). Kept for addons that are not loaded now too: written to new saves unchanged.
// Holds only the objects no binder holds now: offline ones, ones not handed to on_load yet, and the online objects
// of a binder that went away without on_save (Binder::shadow).
xr_map<xr_string, SavedBinder>* g_saved = nullptr;

// binders::WriteSave is asking on_save: the ALife save stream is being built, binder_register / binder_unregister
// (and the ALife writes of the plugin API, IsSaving) are refused
bool g_binders_writing_save = false;

constexpr u32 kSaveChunkId = 0x0103; // own chunk of the ALife save stream (0x0100..0x0102: addon_host, data, timers)
constexpr u32 kSaveFormat = 1;
constexpr u32 kFirstSaveBuffer = 256; // the first on_save call gets this much; a bigger state is asked for again

xr_string SaveKeyOf(pcstr addon, pcstr class_text, pcstr mask)
{
    xr_string key = addon;
    key += '\x1f'; // never inside an addon id, a class id or a section
    key += class_text;
    key += '\x1f';
    key += mask;
    return key;
}

// The parts of the key of a binder: its addon, its class id as text ("" = any), its mask in lower case ("" = any).
struct SaveKeyParts
{
    xr_string addon;
    xr_string class_text;
    xr_string mask;
};

SaveKeyParts SaveKeyPartsOf(const Binder& binder)
{
    SaveKeyParts parts;
    parts.addon = PluginAddonId(binder.plugin);
    if (!binder.any_class)
    {
        char class_text[9] = {};
        ClassIdText(binder.class_id, class_text);
        parts.class_text = class_text;
    }
    parts.mask = binder.mask;
    for (char& c : parts.mask)
        c = static_cast<char>(Lower(c)); // the mask matches case-insensitively: "WPN_*" and "wpn_*" are one key
    return parts;
}

SavedBinder& EnsureSaved(const xr_string& key, pcstr addon, pcstr class_text, pcstr mask)
{
    if (!g_saved)
        g_saved = xr_new<xr_map<xr_string, SavedBinder>>();
    SavedBinder& saved = (*g_saved)[key];
    if (saved.addon.empty())
    {
        saved.addon = addon;
        saved.class_text = class_text;
        saved.mask = mask;
    }
    return saved;
}

SavedBinder* FindSaved(const xr_string& key)
{
    if (!g_saved || key.empty())
        return nullptr;
    const auto it = g_saved->find(key);
    return it != g_saved->end() ? &it->second : nullptr;
}

// The server object `id` is still the one that was online with this section. Without ALife (the game is going away)
// nothing is saved: the state would have no game to come back to.
bool ServerObjectMatches(u16 id, pcstr section)
{
    if (!ai().get_alife() || !section)
        return false;
    const CSE_ALifeDynamicObject* object = ai().alife().objects().object(id, true);
    return object && object->s_name.c_str() && xr_strcmp(object->s_name.c_str(), section) == 0;
}

// The id is not the object the shadow copies were made for any more (a new life, a released server object)
void DropBinderShadows(u16 id)
{
    for (u32 i = 0; i < kMaxBinders; ++i)
    {
        if ((g_active & Bit(i)) && !g_slots[i]->shadow.empty())
            g_slots[i]->shadow.erase(id);
    }
}

// The binder goes away without on_save for its bound objects (a crash, an unload of a plugin that did not call
// binder_unregister): their last known bytes become the kept state, like an on_save right before on_destroy would
// have made them. Not for an object of an earlier game, a released one, or one whose server object is another now.
void MoveBinderShadowsToSaved(Binder& binder)
{
    if (binder.shadow.empty())
        return;
    if (!binder.save_key.empty() && g_object_spawned)
    {
        const SaveKeyParts parts = SaveKeyPartsOf(binder);
        u32 moved = 0;
        for (auto& [id, object] : binder.shadow)
        {
            const u8 flags = g_object_spawned[id];
            if (!(flags & kThisGame) || (flags & kReleased) || !ServerObjectMatches(id, object.section.c_str()))
                continue;
            SavedBinder& saved =
                EnsureSaved(binder.save_key, parts.addon.c_str(), parts.class_text.c_str(), parts.mask.c_str());
            saved.objects[id] = std::move(object);
            ++moved;
        }
        if (moved && IsDebugLog())
            Msg("~ [plugin:%s] binder #%u is gone without on_save: the last saved state of %u bound object(s) is kept",
                parts.addon.c_str(), binder.id, moved);
    }
    binder.shadow.clear();
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
    Save,
    Load,
};
constexpr pcstr kCallbackNames[] = { "binder on_reinit", "binder on_spawn", "binder on_destroy", "binder on_update",
    "binder on_save", "binder on_load" };

struct Call
{
    const Binder* binder;
    ECallback kind;
    u16 id;
    pcstr section;
    u32 count;                      // Update: records; Save: the capacity of `buffer`; Load: the bytes of `data`
    const GwpBinderUpdate* updates; // Update
    void* buffer;                   // Save
    const void* data;               // Load
    u32 result;                     // Save: the size on_save asked for
};

void Thunk(void* context)
{
    Call& call = *static_cast<Call*>(context);
    const GwpBinderVTable& vt = call.binder->vtable;
    void* const user = call.binder->user;
    switch (call.kind)
    {
    case ECallback::Reinit: vt.on_reinit(user, call.id); break;
    case ECallback::Spawn: vt.on_spawn(user, call.id, call.section); break;
    case ECallback::Destroy: vt.on_destroy(user, call.id); break;
    // The stride is the size of the engine's record: a plugin of a later version, whose record is longer, steps
    // with it and reads only the fields it covers
    case ECallback::Update:
        vt.on_update(user, call.count, call.updates, static_cast<uint32_t>(sizeof(GwpBinderUpdate)));
        break;
    case ECallback::Save: call.result = vt.on_save(user, call.id, call.buffer, call.count); break;
    case ECallback::Load: vt.on_load(user, call.id, call.data, call.count); break;
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
    case ECallback::Save: return reinterpret_cast<const void*>(binder.vtable.on_save);
    case ECallback::Load: return reinterpret_cast<const void*>(binder.vtable.on_load);
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
bool InvokeCall(Binder& binder, Call& call)
{
    const void* callback = CallbackOf(binder, call.kind);
    if (!callback)
        return true;
    const GwpPlugin* const plugin = binder.plugin;
    const pcstr name = kCallbackNames[static_cast<size_t>(call.kind)];
    bool ok;
    {
        const DispatchDepthScope depth;
        ok = events::CallPluginGuarded(callback, name, &Thunk, &call);
    }
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

bool Invoke(Binder& binder, ECallback kind, u16 id, pcstr section = nullptr, u32 count = 0,
    const GwpBinderUpdate* updates = nullptr)
{
    Call call{ &binder, kind, id, section, count, updates, nullptr, nullptr, 0 };
    return InvokeCall(binder, call);
}

// Asks on_save of the binder in `slot` for object `id`: first with a small buffer, once more with the size it asked
// for. true when the binder is still there after the calls, with `keep` (false: nothing to keep, or a state above
// the limit) and the bytes; false when the plugin crashed (its binders are gone) or the binder was unregistered
// from inside the call - `bytes` is not to be used then.
bool AskSave(u32 slot, GwpBinderId binder_id, u16 id, xr_vector<u8>& bytes, bool& keep)
{
    keep = false;
    bytes.clear();
    if (!SameBinder(slot, binder_id))
        return false;
    if (!g_slots[slot]->vtable.on_save)
        return true;
    bytes.resize(kFirstSaveBuffer);
    Call first{ g_slots[slot], ECallback::Save, id, nullptr, kFirstSaveBuffer, nullptr, bytes.data(), nullptr, 0 };
    if (!InvokeCall(*g_slots[slot], first) || !SameBinder(slot, binder_id))
        return false;
    u32 size = first.result;
    if (size > kFirstSaveBuffer && size <= GWP_BINDER_SAVE_MAX_SIZE)
    {
        bytes.resize(size);
        Call second{ g_slots[slot], ECallback::Save, id, nullptr, size, nullptr, bytes.data(), nullptr, 0 };
        if (!InvokeCall(*g_slots[slot], second) || !SameBinder(slot, binder_id))
            return false;
        if (second.result > size)
        {
            Msg("! [plugin:%s] binder #%u on_save of object %u asked for %u bytes, then for %u: not kept",
                PluginAddonId(g_slots[slot]->plugin), binder_id, id, size, second.result);
            bytes.clear();
            return true;
        }
        size = second.result;
    }
    else if (size > GWP_BINDER_SAVE_MAX_SIZE)
    {
        Msg("! [plugin:%s] binder #%u on_save of object %u: %u bytes, more than %u, not kept",
            PluginAddonId(g_slots[slot]->plugin), binder_id, id, size, GWP_BINDER_SAVE_MAX_SIZE);
        bytes.clear();
        return true;
    }
    bytes.resize(size);
    keep = size != 0;
    return true;
}

// The object leaves the binder (goes offline, or the binder is unregistered): what on_save gives is kept under the
// key of the binder, 0 drops the kept state. Nothing is asked when the id is not worth keeping anything for: an
// object of an earlier game (a load came meanwhile), a released server object, no ALife (the game is going away).
// false: the binder is gone (a crash, or unregistered from inside on_save).
// The shadow copy of the object goes with it: after a successful on_save, or when nothing is asked. A binder gone
// inside on_save took it to g_saved (RemoveSlot): the object keeps its last known bytes.
bool KeepObject(u32 slot, GwpBinderId binder_id, u16 id, pcstr section)
{
    Binder& binder = *g_slots[slot];
    if (binder.save_key.empty() || !binder.vtable.on_save)
        return true;
    const u8 flags = g_object_spawned[id];
    if (!(flags & kThisGame) || (flags & kReleased) || !ServerObjectMatches(id, section))
    {
        binder.shadow.erase(id);
        return true;
    }
    // copies: the binder may be gone after the call
    const xr_string key = binder.save_key;
    const SaveKeyParts parts = SaveKeyPartsOf(binder);
    const xr_string object_section = section;
    xr_vector<u8> bytes;
    bool keep = false;
    if (!AskSave(slot, binder_id, id, bytes, keep))
        return false;
    g_slots[slot]->shadow.erase(id); // AskSave true: the binder is still in the slot
    SavedBinder& saved = EnsureSaved(key, parts.addon.c_str(), parts.class_text.c_str(), parts.mask.c_str());
    if (keep)
    {
        SavedObject& object = saved.objects[id];
        object.section = object_section;
        object.data = std::move(bytes);
    }
    else
        saved.objects.erase(id);
    return true;
}

// Before on_spawn: the kept state of the object goes to on_load of the binder once and is forgotten (a binder with
// on_save only drops it: it will give a fresh one). A state kept for another section belongs to another object
// that had the id: dropped. false: the binder is gone (a crash, or unregistered from inside on_load).
bool DeliverLoad(u32 slot, GwpBinderId binder_id, u16 id, pcstr section)
{
    Binder& binder = *g_slots[slot];
    SavedBinder* saved = FindSaved(binder.save_key);
    if (!saved)
        return true;
    const auto it = saved->objects.find(id);
    if (it == saved->objects.end())
        return true;
    SavedObject object = std::move(it->second);
    saved->objects.erase(it);
    if (!section || xr_strcmp(object.section.c_str(), section) != 0)
    {
        if (IsDebugLog())
            Msg("~ [plugin:%s] binder #%u: the saved state of object %u was kept for section '%s', the object is '%s' "
                "now: dropped", PluginAddonId(binder.plugin), binder_id, id, object.section.c_str(),
                section ? section : "");
        return true;
    }
    if (!binder.vtable.on_load || object.data.empty())
        return true;
    Call call{ &binder, ECallback::Load, id, section, static_cast<u32>(object.data.size()), nullptr, nullptr,
        object.data.data(), 0 };
    if (!InvokeCall(binder, call) || !SameBinder(slot, binder_id))
        return false;
    // The plugin holds the state now; the engine keeps a copy in case the binder goes away without on_save. Only
    // while the object is still bound (on_load may have unregistered and registered binders) and saves its state.
    Binder& holder = *g_slots[slot];
    if (holder.vtable.on_save && (g_object_slots[id] & Bit(slot)))
        holder.shadow[id] = std::move(object);
    return true;
}

// A load or a new game: the objects online now belong to the game before it, nothing of theirs is kept
void ForgetThisGame()
{
    if (!g_object_spawned)
        return;
    for (u32 id = 0; id < kObjectSlots; ++id)
        g_object_spawned[id] &= static_cast<u8>(~kThisGame);
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
// called us returns). No callbacks. keep_shadows: the last known state of the objects still bound goes to g_saved
// (MoveBinderShadowsToSaved); false only at Shutdown.
void RemoveSlot(u32 slot, bool keep_shadows = true)
{
    Binder* binder = g_slots[slot];
    if (!binder)
        return;
    if (keep_shadows)
        MoveBinderShadowsToSaved(*binder);
    binder->shadow.clear();
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
            continue; // net_Spawn is not over: on_load and on_spawn come from OnObjectSpawn
        ++binder.spawns;
        const pcstr section = object->cNameSect_str();
        if (!DeliverLoad(slot, binder_id, id, section) || !Invoke(*g_slots[slot], ECallback::Spawn, id, section))
            return false;
    }
    return SameBinder(slot, binder_id); // a crash of the plugin in a nested call removes it as well
}

// Section of a bound object, also of one marked for destroy (getDestroy) that has not gone through net_Destroy yet:
// it is still bound and gets its on_save / on_destroy from DetachAll
pcstr BoundObjectSection(u16 id)
{
    if (!g_pGameLevel)
        return nullptr;
    const CGameObject* object = smart_cast<CGameObject*>(Level().Objects.net_Find(id));
    return object ? object->cNameSect_str() : nullptr;
}

// binder_unregister: every bound object gets on_destroy first, so the plugin can drop its per-object state.
// The binder's own bits are walked, not a list of the online objects: an object marked for destroy in this frame
// (left out of such a list) is still bound and must get its on_save and on_destroy here, its OnObjectDestroy will
// not find the bit any more. The bits are re-read at every id: callbacks may attach or detach objects meanwhile.
void DetachAll(u32 slot, GwpBinderId binder_id)
{
    const u64 bit = Bit(slot);
    for (u32 index = 0; index < kObjectSlots; ++index)
    {
        // Removed by a crash in an earlier call, or unregistered from inside it (the slot may hold another binder)
        if (!SameBinder(slot, binder_id))
            return;
        const u16 id = static_cast<u16>(index);
        if (!(g_object_slots[id] & bit))
            continue;
        g_object_slots[id] &= ~bit;
        if (g_slots[slot]->bound)
            --g_slots[slot]->bound;
        if (!(g_object_spawned[id] & kSpawned))
        {
            g_slots[slot]->shadow.erase(id);
            continue; // not spawned yet: no on_destroy, and its kept state stays for the next binder
        }
        // The bit is gone before the calls: an unregister from inside them does not reach this object again. An
        // object sending its on_destroy now (kDestroying, the unregister comes from one of those calls) gets ours
        // here: OnObjectDestroy skips this binder, its bit is gone.
        if (!KeepObject(slot, binder_id, id, BoundObjectSection(id)))
            return;
        if (!Invoke(*g_slots[slot], ECallback::Destroy, id))
            return;
    }
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group binders)
// ---------------------------------------------------------------------------------------------

GwpBinderId GWP_CALL ApiBinderRegister(const GwpPlugin* self, const char* class_id, const char* section_mask,
    const GwpBinderVTable* vtable, void* user)
{
    if (!CheckCall("binder_register"))
        return GWP_INVALID_BINDER_ID;
    if (!self)
    {
        Msg("! [binders] binder_register: self is NULL, refused");
        return GWP_INVALID_BINDER_ID;
    }
    const pcstr addon = PluginAddonId(self);
    const bool has_class = class_id && class_id[0];
    const bool has_mask = section_mask && section_mask[0];
    if (!vtable)
    {
        Msg("! [plugin:%s] binder_register: vtable is missing", addon);
        return GWP_INVALID_BINDER_ID;
    }
    // The table up to on_update is the least a binder has (version 0.1); a smaller size is a table not filled in
    if (vtable->size < GWP_BINDER_VTABLE_MIN_SIZE)
    {
        Msg("! [plugin:%s] binder_register: vtable size %u is below %u, set size = sizeof(GwpBinderVTable)", addon,
            vtable->size, GWP_BINDER_VTABLE_MIN_SIZE);
        return GWP_INVALID_BINDER_ID;
    }
    if (vtable->reserved != 0)
    {
        Msg("! [plugin:%s] binder_register: vtable reserved field is %u, must be 0", addon, vtable->reserved);
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
    // on_spawn / on_load of the objects online now would run in the middle of the ALife save stream
    if (g_binders_writing_save)
    {
        Msg("! [plugin:%s] binder_register refused: called from on_save while the game is being saved", addon);
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
    // the rest zero, a plugin with a shorter one gets NULL for the callbacks it does not have. A field the size
    // covers only in part (a size that is not the sizeof of any version) is not taken either: half a pointer.
    binder->vtable = {};
    const size_t known = std::min<size_t>(vtable->size, sizeof(GwpBinderVTable));
    memcpy(&binder->vtable, vtable, known);
    if (known < offsetof(GwpBinderVTable, on_save) + sizeof(binder->vtable.on_save))
        binder->vtable.on_save = nullptr;
    if (known < offsetof(GwpBinderVTable, on_load) + sizeof(binder->vtable.on_load))
        binder->vtable.on_load = nullptr;
    binder->vtable.size = sizeof(GwpBinderVTable);
    // The saved state is kept per addon, class and mask: two binders of a plugin under one key would mix it up
    if (binder->vtable.on_save || binder->vtable.on_load)
    {
        const SaveKeyParts parts = SaveKeyPartsOf(*binder);
        binder->save_key = SaveKeyOf(parts.addon.c_str(), parts.class_text.c_str(), parts.mask.c_str());
        for (u32 i = 0; i < kMaxBinders; ++i)
        {
            if ((g_active & Bit(i)) && g_slots[i]->plugin == self && g_slots[i]->save_key == binder->save_key)
            {
                Msg("! [plugin:%s] binder_register: binder #%u of this plugin has the same class id and mask and "
                    "saves its objects too (on_save / on_load): the saved state would be shared",
                    addon, g_slots[i]->id);
                xr_delete(binder);
                return GWP_INVALID_BINDER_ID;
            }
        }
    }
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
    if (!self || id == GWP_INVALID_BINDER_ID)
        return GWP_ERROR_INVALID_ARGUMENT;
    u32 slot = 0;
    const Binder* binder = FindBinder(id, &slot);
    if (!binder)
    {
        if (IsDebugLog())
            Msg("~ [plugin:%s] binder_unregister: no binder #%u", PluginAddonId(self), id);
        return GWP_ERROR_NOT_FOUND;
    }
    if (binder->plugin != self)
    {
        if (IsDebugLog())
            Msg("~ [plugin:%s] binder_unregister: binder #%u belongs to another plugin", PluginAddonId(self), id);
        return GWP_ERROR_ACCESS_DENIED;
    }
    // WriteSave is walking the binders and their objects: an unregister from on_save would change both under it
    if (g_binders_writing_save)
    {
        Msg("! [plugin:%s] binder_unregister refused: called from on_save while the game is being saved",
            PluginAddonId(self));
        return GWP_ERROR_INVALID_STATE;
    }
    DetachAll(slot, id);
    // Not removed by a crash inside on_save / on_destroy, nor unregistered from it: then the slot may hold a newer
    // binder
    if (SameBinder(slot, id))
        RemoveSlot(slot);
    FlushGraveyard();
    if (HasCrashed(self))
        return GWP_ERROR_CRASHED; // in on_save / on_destroy: every binder of the plugin is gone, this one too
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
        DropBinderShadows(id);
    }
    g_object_slots[id] = 0;
    g_object_spawned[id] = kThisGame; // a new life of the id: its state may be kept for the save of this game
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
    g_object_spawned[id] = static_cast<u8>((g_object_spawned[id] & (kThisGame | kReleased)) | kSpawned);
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
        // the kept state first (on_load), then on_spawn; on_load may remove the binder (a crash, an unregister)
        if (!DeliverLoad(i, binder_ids[i], id, section))
            continue;
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
    const pcstr section = object->cNameSect_str();
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
        if (!spawned)
        {
            binder.shadow.erase(id);
            continue;
        }
        // on_save right before on_destroy; a binder gone inside it (a crash, an unregister) gets no on_destroy
        if (!KeepObject(i, binder_ids[i], id, section))
            continue;
        Invoke(*g_slots[i], ECallback::Destroy, id);
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
// Saved state: ALife and the save stream
// ---------------------------------------------------------------------------------------------

void OnServerObjectRelease(u16 id)
{
    if (id == kInvalidObjectId)
        return;
    if (g_saved)
    {
        for (auto& [key, saved] : *g_saved)
            saved.objects.erase(id);
    }
    DropBinderShadows(id); // nor does a binder that goes away without on_save bring it back
    // An online object with the id goes offline after this (its client side learns of the release later): it keeps
    // nothing. OnObjectInit of the next life of the id clears the mark.
    if (g_object_spawned)
        g_object_spawned[id] |= kReleased;
}

void ResetSave()
{
    ForgetThisGame();
    if (g_saved)
        g_saved->clear();
    for (u32 i = 0; i < kMaxBinders; ++i)
    {
        if (g_active & Bit(i))
            g_slots[i]->shadow.clear(); // the objects online now belong to the game before
    }
}

bool IsSaving() { return g_binders_writing_save; }

void WriteSave(IWriter& stream)
{
    // What is written: the kept state of the objects no binder holds, and for the bound objects the state of their
    // binder. on_save is asked now for every bound online object; its answer replaces the shadow copy of the object
    // (Binder::shadow), 0 drops it. An object that is not asked (marked for destroy, left after a crash of its
    // binder) keeps its last known bytes: the shadow copy, or what a binder gone without on_save left in g_saved.
    // The answers stay in memory only as shadow copies: the binder holds those objects, they get on_save again when
    // they go offline. binder_register / binder_unregister and the ALife writes of plugins are refused meanwhile
    // (IsSaving): the ALife save stream is being built around this call.
    struct BindersWritingSaveScope
    {
        BindersWritingSaveScope() { g_binders_writing_save = true; }
        ~BindersWritingSaveScope() { g_binders_writing_save = false; }
        BindersWritingSaveScope(const BindersWritingSaveScope&) = delete;
        BindersWritingSaveScope& operator=(const BindersWritingSaveScope&) = delete;
    };
    const BindersWritingSaveScope writing;
    if (g_active && g_object_slots)
    {
        xr_vector<u16> ids;
        OnlineObjectIds(ids);
        for (u32 i = 0; i < kMaxBinders; ++i)
        {
            if (!(g_active & Bit(i)) || g_slots[i]->save_key.empty() || !g_slots[i]->vtable.on_save)
                continue;
            const GwpBinderId binder_id = g_slots[i]->id;
            for (const u16 id : ids)
            {
                if (!SameBinder(i, binder_id))
                    break; // crashed inside on_save: its shadow copies went to g_saved (RemoveSlot)
                const u8 flags = g_object_spawned[id];
                if (!(g_object_slots[id] & Bit(i)) || (flags & (kSpawned | kDestroying | kThisGame | kReleased)) !=
                    (kSpawned | kThisGame))
                {
                    continue;
                }
                const CGameObject* object = OnlineObject(id);
                if (!object || !ServerObjectMatches(id, object->cNameSect_str()))
                    continue;
                xr_string section = object->cNameSect_str();
                xr_vector<u8> bytes;
                bool keep = false;
                if (!AskSave(i, binder_id, id, bytes, keep))
                    break;
                Binder& binder = *g_slots[i]; // AskSave true: still in the slot
                if (keep)
                {
                    SavedObject& entry = binder.shadow[id];
                    entry.section = std::move(section);
                    entry.data = std::move(bytes);
                }
                else
                    binder.shadow.erase(id);
            }
        }
    }

    // After the calls: a crash in one of them moved the shadow copies of that binder to g_saved
    xr_map<xr_string, SavedBinder> snapshot;
    if (g_saved)
        snapshot = *g_saved;
    for (u32 i = 0; i < kMaxBinders; ++i)
    {
        if (!(g_active & Bit(i)) || g_slots[i]->shadow.empty() || g_slots[i]->save_key.empty())
            continue;
        const Binder& binder = *g_slots[i];
        const SaveKeyParts parts = SaveKeyPartsOf(binder);
        SavedBinder& saved = snapshot[binder.save_key];
        if (saved.addon.empty())
        {
            saved.addon = parts.addon;
            saved.class_text = parts.class_text;
            saved.mask = parts.mask;
        }
        for (const auto& [id, object] : binder.shadow)
        {
            const u8 flags = g_object_spawned[id];
            if ((g_object_slots[id] & Bit(i)) && (flags & kThisGame) && !(flags & kReleased))
                saved.objects[id] = object;
        }
    }

    u32 count = 0;
    for (const auto& [key, saved] : snapshot)
        count += saved.objects.empty() ? 0 : 1;
    stream.open_chunk(kSaveChunkId);
    stream.w_u32(kSaveFormat);
    stream.w_u32(count);
    for (const auto& [key, saved] : snapshot)
    {
        if (saved.objects.empty())
            continue;
        stream.w_stringZ(saved.addon.c_str());
        stream.w_stringZ(saved.class_text.c_str());
        stream.w_stringZ(saved.mask.c_str());
        stream.w_u32(static_cast<u32>(saved.objects.size()));
        for (const auto& [id, object] : saved.objects)
        {
            stream.w_u16(id);
            stream.w_stringZ(object.section.c_str());
            stream.w_u32(static_cast<u32>(object.data.size()));
            stream.w(object.data.data(), object.data.size());
        }
    }
    stream.close_chunk();
}

void ReadSave(IReader& stream)
{
    ResetSave(); // the loaded game replaces the kept state; the objects online now are of the game before
    IReader* reader = stream.open_chunk(kSaveChunkId);
    if (!reader)
        return; // a save made by a build without it: no binder kept anything
    const auto has = [reader](size_t bytes) { return reader->elapsed() >= static_cast<intptr_t>(bytes); };
    const u32 format = has(sizeof(u32)) ? reader->r_u32() : 0;
    if (format != kSaveFormat)
    {
        Msg("! [binders] save data: format %u is not supported, ignored", format);
        reader->close();
        return;
    }
    const u32 count = has(sizeof(u32)) ? reader->r_u32() : 0;
    u32 objects = 0;
    bool broken = false;
    for (u32 i = 0; i < count && !broken; ++i)
    {
        SaveKeyParts parts;
        if (!ReadStringZChecked(*reader, parts.addon) || !ReadStringZChecked(*reader, parts.class_text) ||
            !ReadStringZChecked(*reader, parts.mask) || !has(sizeof(u32)) || parts.addon.empty() ||
            parts.class_text.size() > 8 || parts.mask.size() > kMaxMaskLength)
        {
            broken = true;
            break;
        }
        const u32 object_count = reader->r_u32();
        SavedBinder& saved = EnsureSaved(SaveKeyOf(parts.addon.c_str(), parts.class_text.c_str(), parts.mask.c_str()),
            parts.addon.c_str(), parts.class_text.c_str(), parts.mask.c_str());
        for (u32 j = 0; j < object_count; ++j)
        {
            SavedObject object;
            if (!has(sizeof(u16)))
            {
                broken = true;
                break;
            }
            const u16 id = reader->r_u16();
            if (!ReadStringZChecked(*reader, object.section) || !has(sizeof(u32)))
            {
                broken = true;
                break;
            }
            const u32 size = reader->r_u32();
            if (id == kInvalidObjectId || size == 0 || size > GWP_BINDER_SAVE_MAX_SIZE || !has(size))
            {
                broken = true;
                break;
            }
            object.data.resize(size);
            reader->r(object.data.data(), size);
            saved.objects[id] = std::move(object);
            ++objects;
        }
    }
    if (broken)
        Msg("! [binders] save data is truncated or corrupted, the rest is ignored");
    reader->close();
    if (IsDebugLog())
        Msg("  [binders] save data: the state of %u object(s) loaded", objects);
}

// ---------------------------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------------------------

// No callbacks: the last known state of the bound objects is kept from the shadow copies (RemoveSlot)
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
        RemoveSlot(i, false); // the kept state goes away below anyway
    g_dispatch_depth = 0;
    FlushGraveyard();
    xr_delete(g_graveyard);
    xr_delete(g_crashed);
    xr_delete(g_saved);
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
    u32 saved_objects = 0;
    if (g_saved)
    {
        for (const auto& [key, saved] : *g_saved)
            saved_objects += static_cast<u32>(saved.objects.size());
    }
    Msg("- [binders] %u binder(s), %u slot(s) free, saved state kept for %u object(s):", count,
        kMaxBinders - count, saved_objects);
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
