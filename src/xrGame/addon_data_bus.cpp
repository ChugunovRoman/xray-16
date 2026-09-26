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
    GwpValue value{};    // for GWP_T_STRING the text lives in `text`: use ValueOf()
    xr_string text;
    bool persistent = false;
};

// xr_map: stable order for data_list and the save; nodes never move, so pointers into `text` stay valid
// until the entry is changed or erased (the lifetime promised by data_get).
using Store = xr_map<xr_string, Entry>;
Store* g_store = nullptr;

constexpr size_t kMaxKeyLength = 256;
constexpr u32 kMaxStringLength = 1024u * 1024u;
// Chunk of the ALife save stream (plugin save data is 0x0100, see addon_host.cpp).
constexpr u32 kSaveChunkId = 0x0101;
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

bool IsStorableType(u32 type)
{
    switch (type)
    {
    case GWP_T_BOOL:
    case GWP_T_INT:
    case GWP_T_NUMBER:
    case GWP_T_STRING:
    case GWP_T_VEC3:
    case GWP_T_OBJECT:
    case GWP_T_SERVER_OBJECT: return true;
    default: return false;
    }
}

GwpValue ValueOf(const Entry& entry)
{
    GwpValue value = entry.value;
    if (value.type == GWP_T_STRING)
    {
        value.u.s.ptr = entry.text.c_str();
        value.u.s.len = static_cast<uint32_t>(entry.text.size());
    }
    return value;
}

bool Equal(const GwpValue& a, const GwpValue& b)
{
    if (a.type != b.type)
        return false;
    switch (a.type)
    {
    case GWP_T_BOOL: return (a.u.b != 0) == (b.u.b != 0);
    case GWP_T_INT: return a.u.i == b.u.i;
    case GWP_T_NUMBER: return a.u.n == b.u.n;
    case GWP_T_STRING:
        return a.u.s.len == b.u.s.len && (a.u.s.len == 0 || memcmp(a.u.s.ptr, b.u.s.ptr, a.u.s.len) == 0);
    case GWP_T_VEC3: return a.u.v[0] == b.u.v[0] && a.u.v[1] == b.u.v[1] && a.u.v[2] == b.u.v[2];
    case GWP_T_OBJECT:
    case GWP_T_SERVER_OBJECT: return a.u.id == b.u.id;
    default: return true;
    }
}

// data_on_changed(key, value). Key and string are copied first: a handler may change or erase the same key.
void NotifyChanged(const xr_string& key, const GwpValue* value)
{
    if (!events::HasSubscribers(events::EBuiltin::DataOnChanged))
        return;
    const xr_string key_copy = key;
    xr_string text_copy;
    GwpValue value_copy = value ? *value : events::Nil();
    if (value_copy.type == GWP_T_STRING)
    {
        text_copy.assign(value_copy.u.s.ptr, value_copy.u.s.len);
        value_copy.u.s.ptr = text_copy.c_str();
    }
    const GwpValue args[] = { events::String(key_copy.c_str()), value_copy };
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

// The caller has validated key and value. GWP_T_NIL erases.
void Set(const xr_string& key, const GwpValue& value, bool persistent)
{
    if (value.type == GWP_T_NIL)
    {
        Erase(key);
        return;
    }
    Store& store = GetStore();
    const auto [it, inserted] = store.try_emplace(key);
    Entry& entry = it->second;
    // A key stays persistent once made so (a writer without the flag, e.g. Lua data_bus.set(key, value) with two
    // arguments, must not drop it from the save); it becomes plain again only through erase.
    entry.persistent = entry.persistent || persistent;
    if (!inserted && Equal(ValueOf(entry), value))
        return;

    if (value.type == GWP_T_STRING)
    {
        // Through a temporary: `value` may point into entry.text itself (data_get + data_set of the same key).
        xr_string text(value.u.s.ptr ? value.u.s.ptr : "", value.u.s.len);
        entry.text.swap(text);
    }
    else
        entry.text.clear();
    entry.value = value;
    if (value.type == GWP_T_STRING)
        entry.value.u.s = {}; // the text is in entry.text, see ValueOf

    const GwpValue stored = ValueOf(entry);
    NotifyChanged(key, &stored);
}

bool CheckValue(const GwpValue& value, pcstr key, pcstr who)
{
    if (value.type == GWP_T_NIL || IsStorableType(value.type))
    {
        if (value.type == GWP_T_STRING && (value.u.s.len > kMaxStringLength || (!value.u.s.ptr && value.u.s.len)))
        {
            Msg("! [data] %s: string value of '%s' is too long or invalid", who, key);
            return false;
        }
        return true;
    }
    Msg("! [data] %s: value of '%s' has a type that cannot be stored (%u)", who, key, value.type);
    return false;
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
    if (!value)
        return GWP_ERROR_INVALID_ARGUMENT;
    string128 who;
    xr_sprintf(who, "plugin:%s", PluginAddonId(self));
    if (!CheckValue(*value, key, who))
        return GWP_ERROR_INVALID_ARGUMENT;
    Set(key, *value, (flags & GWP_DATA_PERSISTENT) != 0);
    return GWP_OK;
}

GwpResult GWP_CALL ApiDataErase(const GwpPlugin* self, const char* key)
{
    const GwpResult check = CheckPluginKey(self, key, "data_erase");
    if (check != GWP_OK)
        return check;
    Erase(key);
    return GWP_OK;
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
        return GWP_ERROR;
    const auto it = g_store->find(key);
    if (it == g_store->end())
        return GWP_ERROR;
    *out = ValueOf(it->second);
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
// Object ids come back as game objects (nil when the object is offline), like event arguments.
int LuaGet(lua_State* L)
{
    const pcstr key = LuaKey(L, 1, "get");
    if (key && g_store)
    {
        const auto it = g_store->find(key);
        if (it != g_store->end())
        {
            events::PushLuaValue(L, ValueOf(it->second));
            return 1;
        }
    }
    lua_settop(L, 2);
    return 1; // default or nil
}

// data_bus.set(key, value [, persistent]): true on success. value = nil erases the key.
// Tables and functions cannot be stored.
int LuaSet(lua_State* L)
{
    const pcstr key = LuaKey(L, 1, "set");
    if (!key)
    {
        lua_pushboolean(L, 0);
        return 1;
    }
    const GwpValue value = events::LuaToValue(L, 2);
    if (!CheckValue(value, key, "Lua"))
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

void WriteValue(IWriter& stream, const Entry& entry)
{
    const GwpValue value = ValueOf(entry);
    stream.w_u32(value.type);
    switch (value.type)
    {
    case GWP_T_BOOL: stream.w_u8(value.u.b ? 1 : 0); break;
    case GWP_T_INT: stream.w_s64(value.u.i); break;
    case GWP_T_NUMBER: stream.w(&value.u.n, sizeof(value.u.n)); break;
    case GWP_T_STRING:
        stream.w_u32(value.u.s.len);
        stream.w(value.u.s.ptr, value.u.s.len);
        break;
    case GWP_T_VEC3: stream.w(value.u.v, sizeof(value.u.v)); break;
    case GWP_T_OBJECT:
    case GWP_T_SERVER_OBJECT: stream.w_u16(value.u.id); break;
    default: break;
    }
}

// false when the data is truncated or the type is unknown.
bool ReadValue(IReader& reader, Entry& entry)
{
    const auto has = [&reader](size_t bytes) { return reader.elapsed() >= static_cast<intptr_t>(bytes); };
    if (!has(sizeof(u32)))
        return false;
    GwpValue value = events::Nil();
    value.type = reader.r_u32();
    switch (value.type)
    {
    case GWP_T_BOOL:
        if (!has(1))
            return false;
        value.u.b = reader.r_u8() ? 1 : 0;
        break;
    case GWP_T_INT:
        if (!has(sizeof(s64)))
            return false;
        value.u.i = reader.r_s64();
        break;
    case GWP_T_NUMBER:
        if (!has(sizeof(double)))
            return false;
        reader.r(&value.u.n, sizeof(value.u.n));
        break;
    case GWP_T_STRING:
    {
        if (!has(sizeof(u32)))
            return false;
        const u32 length = reader.r_u32();
        if (length > kMaxStringLength || !has(length))
            return false;
        entry.text.assign(static_cast<const char*>(reader.pointer()), length);
        reader.advance(length);
        break;
    }
    case GWP_T_VEC3:
        if (!has(sizeof(value.u.v)))
            return false;
        reader.r(value.u.v, sizeof(value.u.v));
        break;
    case GWP_T_OBJECT:
    case GWP_T_SERVER_OBJECT:
        if (!has(sizeof(u16)))
            return false;
        value.u.id = reader.r_u16();
        break;
    default: return false;
    }
    entry.value = value;
    return true;
}

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
            WriteValue(stream, entry);
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
        if (!ReadStringZChecked(*reader, key) || !IsValidKey(key.c_str()) || !ReadValue(*reader, entry))
        {
            Msg("! [data] save data is truncated or corrupted, the rest is ignored");
            break;
        }
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
            const GwpValue value = ValueOf(it->second);
            string512 text;
            switch (value.type)
            {
            case GWP_T_BOOL: xr_strcpy(text, value.u.b ? "true" : "false"); break;
            case GWP_T_INT: xr_sprintf(text, "%lld", static_cast<long long>(value.u.i)); break;
            case GWP_T_NUMBER: xr_sprintf(text, "%g", value.u.n); break;
            case GWP_T_STRING: xr_sprintf(text, "\"%.200s\"%s", it->second.text.c_str(), value.u.s.len > 200 ? "..." : ""); break;
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
