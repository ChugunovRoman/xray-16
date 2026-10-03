#include "StdAfx.h"

#include "addon_data_bus.h"
#include "addon_event_bus.h"
#include "addon_host.h"

#include "xrScriptEngine/script_engine.hpp"

#include <cstring>

namespace gw::addons::data
{
namespace
{
struct Entry
{
    events::OwnedValue value; // a deep copy: strings, bytes and the items of arrays belong to the entry
    bool persistent = false;
};

// xr_map: stable order for data_list and the save; nodes never move, and an OwnedValue keeps its parts at their
// addresses until it is assigned again, so what data_get gives stays valid until the key is changed or erased.
using Store = xr_map<xr_string, Entry>;
Store* g_store = nullptr;

constexpr size_t kMaxKeyLength = 256;
constexpr u32 kMaxStringLength = 1024u * 1024u;
// Chunk of the ALife save stream (plugin save data is 0x0100, see addon_host.cpp).
constexpr u32 kSaveChunkId = 0x0101;
// Format 1: u32 count, then per value the key (stringZ) and the value (events::WriteValue). GWP_T_BYTES and
// GWP_T_ARRAY (plans/lua_to_cpp/26, 1.7) are new types of the same value format, so the number stays 1: older
// saves read as they are, and an older engine reads a newer save up to the first value of a type it does not know.
constexpr u32 kSaveFormat = 1;

Store& GetStore()
{
    if (!g_store)
        g_store = xr_new<Store>();
    return *g_store;
}

// "<owner>/<name>": at least one character on both sides of the first '/', no '/' at the end.
bool IsValidKey(pcstr key)
{
    if (!key)
        return false;
    const size_t length = xr_strlen(key);
    const char* slash = strchr(key, '/');
    return length > 0 && length <= kMaxKeyLength && slash && slash != key && key[length - 1] != '/';
}

// What the store refuses in a value that passed events::CheckValue: GWP_T_LUA_REF at any depth (a reference
// without content means nothing in a save or to another reader) and strings or bytes longer than kMaxStringLength.
// nullptr for a storable value, else the reason for the log.
pcstr DataUnstorableReason(const GwpValue& value, u32 depth)
{
    switch (value.type)
    {
    case GWP_T_LUA_REF: return "a Lua table or userdata without a native form (GWP_T_LUA_REF)";
    case GWP_T_STRING:
    case GWP_T_BYTES: return value.u.s.len > kMaxStringLength ? "a string longer than 1 MiB" : nullptr;
    case GWP_T_ARRAY:
        if (depth >= events::kMaxArrayDepth)
            return "arrays nested too deep";
        for (u32 i = 0; i < value.u.a.count; ++i)
        {
            if (const pcstr reason = DataUnstorableReason(value.u.a.items[i], depth + 1))
                return reason;
        }
        return nullptr;
    default: return nullptr;
    }
}

// data_on_changed(key, value). Key and value are copied first: a handler may change or erase the same key, and the
// entry's own copy would be gone under the next handlers.
void NotifyChanged(const xr_string& key, const GwpValue* value)
{
    if (!events::HasSubscribers(events::EBuiltin::DataOnChanged))
        return;
    const xr_string key_copy = key;
    const events::OwnedValue value_copy(value ? *value : events::Nil());
    const GwpValue args[] = { events::String(key_copy.c_str()), value_copy.view() };
    events::Emit(events::EBuiltin::DataOnChanged, args, 2);
}

bool Erase(const xr_string& key)
{
    Store& store = GetStore();
    const auto it = store.find(key);
    if (it == store.end())
        return false;
    store.erase(it);
    NotifyChanged(key, nullptr);
    return true;
}

// The caller has validated key and value (CheckValue). GWP_T_NIL erases.
void Set(const xr_string& key, const GwpValue& value, bool persistent)
{
    if (value.type == GWP_T_NIL)
    {
        Erase(key);
        return;
    }
    GwpValue stored_value = value;
    // A string or bytes with ptr == NULL (len 0, CheckValue allows it) is an empty string here, as it always was
    // for the data bus; inside the value machinery ptr == NULL would mean nil.
    if ((stored_value.type == GWP_T_STRING || stored_value.type == GWP_T_BYTES) && !stored_value.u.s.ptr)
    {
        stored_value.u.s.ptr = "";
        stored_value.u.s.len = 0;
    }
    Store& store = GetStore();
    const auto [it, inserted] = store.try_emplace(key);
    Entry& entry = it->second;
    // A key stays persistent once made so (a writer without the flag, e.g. Lua data_bus.set(key, value) with two
    // arguments, must not drop it from the save); it becomes plain again only through erase.
    entry.persistent = entry.persistent || persistent;
    if (!inserted && events::ValuesEqual(entry.value.view(), stored_value))
        return;
    // Assign copies deeply through a new arena: `value` may point into entry.value itself (data_get + data_set of
    // the same key).
    entry.value.Assign(stored_value);
    NotifyChanged(key, &entry.value.view());
}

// The value check of both sides (plugin and Lua): false (logged) when the value cannot be stored.
bool CheckStorable(const GwpValue& value, pcstr key, pcstr who)
{
    if (events::CheckValue(value) != GWP_OK)
    {
        Msg("! [data] %s: value of '%s' is invalid (type %u, reserved %u, a NULL pointer, a string over 1 MiB, an "
            "array over its limits, more than 65536 values or 16 MiB of strings in total)", who, key, value.type,
            value.reserved);
        return false;
    }
    if (const pcstr reason = DataUnstorableReason(value, 0))
    {
        Msg("! [data] %s: value of '%s' cannot be stored: %s", who, key, reason);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group data)
// ---------------------------------------------------------------------------------------------

// A plugin writes only keys of its own addon: "<addon id>/...".
GwpResult CheckPluginKey(const GwpPlugin* self, const char* key, pcstr function)
{
    if (!IsMainThread())
    {
        Msg("! [plugin:%s] %s called outside the main thread, ignored", PluginAddonId(self), function);
        return GWP_ERROR_NOT_MAIN_THREAD;
    }
    if (!self || !IsValidKey(key))
        return GWP_ERROR_INVALID_ARGUMENT;
    const pcstr id = PluginAddonId(self);
    const size_t id_length = xr_strlen(id);
    if (strncmp(key, id, id_length) != 0 || key[id_length] != '/')
        return GWP_ERROR_ACCESS_DENIED;
    return GWP_OK;
}

GwpResult GWP_CALL ApiDataSet(const GwpPlugin* self, const char* key, const GwpValue* value, uint32_t flags)
{
    const GwpResult check = CheckPluginKey(self, key, "data_set");
    if (check != GWP_OK)
        return check;
    if (flags & ~GWP_DATA_PERSISTENT)
    {
        Msg("! [plugin:%s] data_set '%s': unknown flags 0x%x (a flag of a later version?)", PluginAddonId(self), key,
            flags);
        return GWP_ERROR_INVALID_ARGUMENT;
    }
    if (!value)
        return GWP_ERROR_INVALID_ARGUMENT;
    string128 who;
    xr_sprintf(who, "plugin:%s", PluginAddonId(self));
    if (!CheckStorable(*value, key, who))
        return GWP_ERROR_INVALID_ARGUMENT;
    Set(key, *value, (flags & GWP_DATA_PERSISTENT) != 0);
    return GWP_OK;
}

GwpResult GWP_CALL ApiDataErase(const GwpPlugin* self, const char* key)
{
    const GwpResult check = CheckPluginKey(self, key, "data_erase");
    if (check != GWP_OK)
        return check;
    return Erase(key) ? GWP_OK : GWP_ERROR_NOT_FOUND;
}

GwpResult GWP_CALL ApiDataGet(const char* key, GwpValue* out)
{
    if (out)
        *out = events::Nil();
    if (!out || !key)
        return GWP_ERROR_INVALID_ARGUMENT;
    if (!IsMainThread())
        return GWP_ERROR_NOT_MAIN_THREAD;
    if (!g_store)
        return GWP_ERROR_NOT_FOUND;
    const auto it = g_store->find(key);
    if (it == g_store->end())
        return GWP_ERROR_NOT_FOUND;
    *out = it->second.value.view(); // the parts live in the entry until the key is changed or erased
    return GWP_OK;
}

// ---------------------------------------------------------------------------------------------
// Lua API: global table data_bus
// ---------------------------------------------------------------------------------------------

// Key argument; logs and returns nullptr when it is not a valid "<owner>/<name>" string.
pcstr LuaKey(lua_State* L, int index, pcstr function)
{
    const char* key = lua_type(L, index) == LUA_TSTRING ? lua_tostring(L, index) : nullptr;
    if (IsValidKey(key))
        return key;
    Msg("! [data] data_bus.%s: the key must be a string \"<owner>/<name>\", got %s", function,
        key ? key : lua_typename(L, lua_type(L, index)));
    return nullptr;
}

// data_bus.get(key [, default]): the value, or default (nil) when there is no such key.
// Object ids come back as game objects (nil when the object is offline), like event arguments; an array comes back
// as a new table {1..n} (a copy: changing it does not change the stored value), bytes as a Lua string.
int LuaGet(lua_State* L)
{
    const pcstr key = LuaKey(L, 1, "get");
    if (key && g_store)
    {
        const auto it = g_store->find(key);
        if (it != g_store->end())
        {
            events::PushLuaValue(L, it->second.value.view());
            return 1;
        }
    }
    lua_settop(L, 2);
    return 1; // default or nil
}

// data_bus.set(key, value [, persistent]): true on success. value = nil erases the key.
// A table with the keys exactly 1..n whose items have a native form (nested such tables too) is stored as an array
// (GWP_T_ARRAY, a deep copy); any other table, a function or a userdata other than a game object cannot be stored.
// The owner of the key is not checked: Lua may write any key, those of plugins included (a C function called from
// Lua cannot tell which script or addon called it; wiki/doc/plugins/api/data.md, "Lua").
int LuaSet(lua_State* L)
{
    const pcstr key = LuaKey(L, 1, "set");
    if (!key)
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    events::ValueArena arena; // the items of an array, until Set copies them into the store
    const GwpValue value = events::LuaToValue(L, 2, &arena);
    if (!CheckStorable(value, key, "Lua"))
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    Set(key, value, lua_toboolean(L, 3) != 0);
    lua_pushboolean(L, 1);
    return 1;
}

// data_bus.erase(key): true when the key existed.
int LuaErase(lua_State* L)
{
    const pcstr key = LuaKey(L, 1, "erase");
    lua_pushboolean(L, key && Erase(key) ? 1 : 0);
    return 1;
}

// data_bus.has(key)
int LuaHas(lua_State* L)
{
    const pcstr key = LuaKey(L, 1, "has");
    lua_pushboolean(L, key && g_store && g_store->count(key) ? 1 : 0);
    return 1;
}

// data_bus.keys([prefix]): array of keys that start with prefix (all keys without it), sorted.
int LuaKeys(lua_State* L)
{
    const char* prefix = lua_type(L, 1) == LUA_TSTRING ? lua_tostring(L, 1) : "";
    const size_t prefix_length = xr_strlen(prefix);
    lua_newtable(L);
    if (!g_store)
        return 1;
    int index = 0;
    for (auto it = g_store->lower_bound(prefix); it != g_store->end(); ++it)
    {
        if (it->first.compare(0, prefix_length, prefix) != 0)
            break;
        lua_pushlstring(L, it->first.c_str(), it->first.size());
        lua_rawseti(L, -2, ++index);
    }
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------------------------

pcstr TypeName(u32 type)
{
    switch (type)
    {
    case GWP_T_BOOL: return "bool";
    case GWP_T_INT: return "int";
    case GWP_T_NUMBER: return "number";
    case GWP_T_STRING: return "string";
    case GWP_T_VEC3: return "vec3";
    case GWP_T_OBJECT: return "object";
    case GWP_T_SERVER_OBJECT: return "server_object";
    case GWP_T_BYTES: return "bytes";
    case GWP_T_ARRAY: return "array";
    default: return "?";
    }
}
} // namespace

// Registered through the script export list: called for every new Lua state, before any script is loaded.
// Not in the anonymous namespace: the export node must have a normal, always-emitted definition.
struct CDataBusScript
{
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

void CDataBusScript::script_register(lua_State* L)
{
    const luaL_Reg functions[] = {
        { "get", &LuaGet },
        { "set", &LuaSet },
        { "erase", &LuaErase },
        { "has", &LuaHas },
        { "keys", &LuaKeys },
    };
    lua_newtable(L);
    for (const luaL_Reg& function : functions)
    {
        lua_pushcfunction(L, function.func);
        lua_setfield(L, -2, function.name);
    }
    lua_setglobal(L, "data_bus");
}

void FillEngineApi(GwpEngineApi& api)
{
    api.data_set = &ApiDataSet;
    api.data_erase = &ApiDataErase;
    api.data_get = &ApiDataGet;
}

void ResetPersistent()
{
    if (!g_store)
        return;
    for (auto it = g_store->begin(); it != g_store->end();)
        it = it->second.persistent ? g_store->erase(it) : std::next(it);
}

void WriteSave(IWriter& stream)
{
    u32 count = 0;
    if (g_store)
    {
        for (const auto& [key, entry] : *g_store)
            count += entry.persistent ? 1 : 0;
    }
    stream.open_chunk(kSaveChunkId);
    stream.w_u32(kSaveFormat);
    stream.w_u32(count);
    if (g_store)
    {
        for (const auto& [key, entry] : *g_store)
        {
            if (!entry.persistent)
                continue;
            stream.w_stringZ(key.c_str());
            entry.value.Write(stream); // events::WriteValue: the value format of the chunk
        }
    }
    stream.close_chunk();
}

void ReadSave(IReader& stream)
{
    // Values of the loaded game replace the persistent ones; no data_on_changed here: read them in alife_on_load.
    ResetPersistent();
    IReader* reader = stream.open_chunk(kSaveChunkId);
    if (!reader)
        return; // save made by a build without the data bus
    const u32 format = reader->elapsed() >= static_cast<intptr_t>(sizeof(u32)) ? reader->r_u32() : 0;
    if (format != kSaveFormat)
    {
        Msg("! [data] save data: format %u is not supported, ignored", format);
        reader->close();
        return;
    }
    const u32 count = reader->elapsed() >= static_cast<intptr_t>(sizeof(u32)) ? reader->r_u32() : 0;
    Store& store = GetStore();
    u32 loaded = 0;
    for (u32 i = 0; i < count; ++i)
    {
        xr_string key;
        Entry entry;
        entry.persistent = true;
        // events::ReadValue reads format 1 as it is (the old types) plus GWP_T_BYTES and GWP_T_ARRAY
        if (!ReadStringZChecked(*reader, key) || !IsValidKey(key.c_str()) || !entry.value.Read(*reader))
        {
            Msg("! [data] save data is truncated or corrupted, the rest is ignored");
            break;
        }
        if (entry.value.type() == GWP_T_NIL)
            continue; // nothing to keep (a value the writer could not write is written as NIL)
        store[key] = std::move(entry);
        ++loaded;
    }
    reader->close();
    if (IsDebugLog())
        Msg("  [data] save data: %u value(s) loaded", loaded);
}

void Shutdown() { xr_delete(g_store); }

void PrintList(pcstr prefix)
{
    const pcstr filter = prefix ? prefix : "";
    const size_t filter_length = xr_strlen(filter);
    u32 shown = 0;
    Msg("- [data] values%s%s (p = persistent):", filter_length ? " with prefix " : "", filter);
    if (g_store)
    {
        for (auto it = g_store->lower_bound(filter); it != g_store->end(); ++it)
        {
            if (it->first.compare(0, filter_length, filter) != 0)
                break;
            const GwpValue& value = it->second.value.view();
            string512 text;
            switch (value.type)
            {
            case GWP_T_BOOL: xr_strcpy(text, value.u.b ? "true" : "false"); break;
            case GWP_T_INT: xr_sprintf(text, "%lld", static_cast<long long>(value.u.i)); break;
            case GWP_T_NUMBER: xr_sprintf(text, "%g", value.u.n); break;
            case GWP_T_STRING: // a copy of the store: zero-terminated after its len bytes
                xr_sprintf(text, "\"%.200s\"%s", value.u.s.ptr ? value.u.s.ptr : "", value.u.s.len > 200 ? "..." : "");
                break;
            case GWP_T_BYTES: xr_sprintf(text, "%u byte(s)", value.u.s.len); break;
            case GWP_T_ARRAY: xr_sprintf(text, "%u item(s)", value.u.a.count); break;
            case GWP_T_VEC3: xr_sprintf(text, "(%g, %g, %g)", value.u.v[0], value.u.v[1], value.u.v[2]); break;
            case GWP_T_OBJECT:
            case GWP_T_SERVER_OBJECT: xr_sprintf(text, "id %u", static_cast<u32>(value.u.id)); break;
            default: xr_strcpy(text, "?"); break;
            }
            Msg("-   %s %-40s %-13s %s", it->second.persistent ? "p" : " ", it->first.c_str(), TypeName(value.type), text);
            ++shown;
        }
    }
    Msg("- [data] %u value(s) shown, %u total", shown, g_store ? static_cast<u32>(g_store->size()) : 0u);
}
} // namespace gw::addons::data
