#include "StdAfx.h"

// Console commands of the Lua parts of addons, on_console_execute and on_localization_change
// (plans/addon_api_ideas/spec.md, gaps P03 and P09). See addon_lua_console.h.

#include "addon_lua_console.h"
#include "addon_event_bus.h"
#include "addon_host.h"
#include "addon_imgui_tools.h"

#include "xrEngine/XR_IOConsole.h"
#include "xrEngine/xr_ioc_cmd.h"
#include "xrScriptEngine/script_engine.hpp"

namespace gw::addons::lua_console
{
namespace
{
constexpr size_t kMaxNameLength = 64;   // a console name: short, the console prints it in a column
constexpr size_t kMaxHelpLength = 240;  // IConsole_Command::TInfo holds 256 bytes
constexpr u32 kMaxCommands = 1024;      // all the addons together

class LuaCommand;

struct Entry
{
    xr_string name; // the command is registered with name.c_str(): the string lives as long as the entry
    xr_string owner;
    xr_string help; // the id of a string or the text itself (translated when shown)
    int ref = LUA_NOREF; // the function, in the registry of `state`
    lua_State* state = nullptr; // the main Lua state the function belongs to
    LuaCommand* command = nullptr;
};

xr_vector<Entry*>* g_entries = nullptr;
// Removed commands: the console may still hold one of them right now (ExecuteCommand calls add_to_LRU after Execute,
// a command may remove itself or end the Lua state). Freed by OnFrame.
xr_vector<LuaCommand*>* g_removed = nullptr;
// Commands typed outside the game logic thread (Lua runs there only): run by OnFrame
xr_vector<std::pair<xr_string, xr_string>>* g_deferred = nullptr;

Entry* Find(pcstr name)
{
    if (!g_entries || !name)
        return nullptr;
    for (Entry* entry : *g_entries)
    {
        if (entry->name == name)
            return entry;
    }
    return nullptr;
}

void Run(pcstr name, pcstr args);

class LuaCommand final : public IConsole_Command
{
public:
    explicit LuaCommand(Entry& entry) : IConsole_Command(entry.name.c_str()), m_entry(&entry)
    {
        bEmptyArgsHandled = true; // the function runs without arguments too: fn("")
        bLowerCaseArgs = false;   // the arguments as typed
    }
    ~LuaCommand() override
    {
        // ~IConsole_Command removes the command of this name from the console: by then the name may belong to the
        // command an addon registered again (addon_reload). This one is out of the console already (Remove).
        cName = "";
    }

    void Execute(pcstr args) override
    {
        if (!m_entry)
            return; // removed by an earlier command of this line
        const pcstr text = args ? args : "";
        if (!IsMainThread())
        {
            if (!g_deferred)
                g_deferred = xr_new<xr_vector<std::pair<xr_string, xr_string>>>();
            g_deferred->emplace_back(m_entry->name, text);
            return;
        }
        Run(m_entry->name.c_str(), text);
    }

    void Info(TInfo& info) override
    {
        if (!m_entry || m_entry->help.empty())
        {
            xr_strcpy(info, "(a command of an addon)");
            return;
        }
        const shared_str text = StringTable().translate(m_entry->help.c_str());
        xr_strcpy(info, text.c_str() ? text.c_str() : m_entry->help.c_str());
    }

    void Detach() { m_entry = nullptr; }

private:
    Entry* m_entry;
};

// The command leaves the console and the list; its object waits for OnFrame. unref: the Lua state is alive (else
// the reference died with it).
void Remove(Entry* entry, bool unref)
{
    if (Console && entry->command)
        Console->RemoveCommand(entry->command);
    if (entry->command)
    {
        entry->command->Detach();
        if (!g_removed)
            g_removed = xr_new<xr_vector<LuaCommand*>>();
        g_removed->push_back(entry->command);
        entry->command = nullptr;
    }
    if (unref && entry->state && entry->ref != LUA_NOREF)
        luaL_unref(entry->state, LUA_REGISTRYINDEX, entry->ref);
    xr_delete(entry);
}

void RemoveAll(bool unref)
{
    if (!g_entries)
        return;
    for (Entry* entry : *g_entries)
        Remove(entry, unref);
    g_entries->clear();
    if (g_deferred)
        g_deferred->clear(); // the functions of the old state are gone
}

int Traceback(lua_State* L)
{
    const pcstr message = lua_tostring(L, 1);
    luaL_traceback(L, L, message && message[0] ? message : "(a Lua error without a message)", 1);
    return 1;
}

void Run(pcstr name, pcstr args)
{
    Entry* entry = Find(name);
    lua_State* main = GEnv.ScriptEngine ? GEnv.ScriptEngine->lua() : nullptr;
    if (!entry || !main || entry->state != main || entry->ref == LUA_NOREF)
        return; // removed, or a command of an old Lua state (RemoveAll drops them; nothing to call)
    // The coroutine that runs a Lua emit right now (a handler typing a command), else the main state: one registry
    lua_State* L = events::ActiveLuaThread();
    if (!L)
        L = main;
    const xr_string owner = entry->owner; // the function may remove its own command (the entry goes)
    const xr_string command = entry->name;
    const int top = lua_gettop(L);
    lua_pushcfunction(L, &Traceback);
    lua_rawgeti(L, LUA_REGISTRYINDEX, entry->ref);
    lua_pushstring(L, args);
    if (lua_pcall(L, 1, 0, top + 1) != 0)
    {
        const pcstr message = lua_tostring(L, -1);
        Msg("! [addon:%s] console command '%s' failed:", owner.c_str(), command.c_str());
        Log(message ? message : "(a Lua error without a message)");
    }
    lua_settop(L, top);
}

// on_console_execute(name, args): after a known, enabled command ran (CConsole::ExecuteHook)
void OnConsoleExecute(pcstr name, pcstr args)
{
    // Commands run from the console thread, from user.ltx at the start, from plugins: the bus is of the logic thread
    if (!IsMainThread())
        return;
    static const GwpEventId id = events::Intern("on_console_execute");
    if (!events::HasSubscribers(id))
        return;
    const GwpValue argv[] = { events::String(name ? name : ""), events::String(args ? args : "") };
    events::Emit(id, argv, 2);
}

// ---------------------------------------------------------------------------------------------
// Lua: _gw_internal.console_command_*
// ---------------------------------------------------------------------------------------------

bool ValidName(pcstr name)
{
    const size_t length = xr_strlen(name);
    if (length < 2 || length >= kMaxNameLength)
        return false;
    if (!((name[0] >= 'a' && name[0] <= 'z') || name[0] == '_'))
        return false;
    for (pcstr c = name; *c; ++c)
    {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_'))
            return false;
    }
    return true;
}

int Refuse(lua_State* L, pcstr owner, pcstr name, pcstr reason, pcstr text)
{
    Msg("! [addon:%s] addons.register_command('%s'): %s", owner, name ? name : "?", text);
    lua_pushboolean(L, false);
    lua_pushstring(L, reason);
    return 2;
}

// console_command_register(name, fn, help, owner) -> true | false, reason
//   reason: "bad name", "bad function", "bad help", "exists" (a command of the engine or of a plugin), "owner" (a
//   command of another addon), "limit", "no console"
int LuaRegister(lua_State* L)
{
    const pcstr name = lua_type(L, 1) == LUA_TSTRING ? lua_tostring(L, 1) : nullptr;
    const pcstr owner = lua_type(L, 4) == LUA_TSTRING ? lua_tostring(L, 4) : "unknown";
    if (!name || !ValidName(name))
    {
        return Refuse(L, owner, name, "bad name",
            "the name must be 2..63 characters a-z 0-9 _ starting with a letter or '_'");
    }
    if (lua_type(L, 2) != LUA_TFUNCTION)
        return Refuse(L, owner, name, "bad function", "the second argument must be a function");
    if (!lua_isnoneornil(L, 3) && lua_type(L, 3) != LUA_TSTRING)
        return Refuse(L, owner, name, "bad help", "help must be a string (the id of a string or the text)");
    const pcstr help = lua_type(L, 3) == LUA_TSTRING ? lua_tostring(L, 3) : "";
    if (xr_strlen(help) > kMaxHelpLength)
        return Refuse(L, owner, name, "bad help", "help is longer than 240 bytes");
    if (!Console)
        return Refuse(L, owner, name, "no console", "the console is not created yet");
    lua_State* main = GEnv.ScriptEngine ? GEnv.ScriptEngine->lua() : L;

    Entry* entry = Find(name);
    if (entry && entry->owner != owner)
    {
        Msg("! [addon:%s] addons.register_command('%s'): the command belongs to addon '%s'", owner, name,
            entry->owner.c_str());
        lua_pushboolean(L, false);
        lua_pushstring(L, "owner");
        return 2;
    }
    if (!entry && Console->GetCommand(name))
        return Refuse(L, owner, name, "exists", "the console has this command already (the engine or a plugin)");
    if (!entry && g_entries && g_entries->size() >= kMaxCommands)
        return Refuse(L, owner, name, "limit", "too many console commands of the addons (1024)");

    lua_pushvalue(L, 2);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (entry)
    {
        // The same addon again (on_load of a new pass, a second call): the function and the help are replaced
        if (entry->state == main && entry->ref != LUA_NOREF)
            luaL_unref(L, LUA_REGISTRYINDEX, entry->ref);
        entry->ref = ref;
        entry->state = main;
        entry->help = help;
        lua_pushboolean(L, true);
        return 1;
    }
    const size_t id_length = xr_strlen(owner);
    if (xr_strcmp(owner, "unknown") != 0 && (strncmp(name, owner, id_length) != 0 || name[id_length] != '_'))
    {
        Msg("~ [addon:%s] addons.register_command('%s'): better start the name with '%s_' - the names of the engine, "
            "the plugins and the other addons are taken first", owner, name, owner);
    }
    if (!g_entries)
        g_entries = xr_new<xr_vector<Entry*>>();
    entry = xr_new<Entry>();
    entry->name = name;
    entry->owner = owner;
    entry->help = help;
    entry->ref = ref;
    entry->state = main;
    entry->command = xr_new<LuaCommand>(*entry);
    g_entries->push_back(entry);
    Console->AddCommand(entry->command);
    lua_pushboolean(L, true);
    return 1;
}

// console_command_unregister(name, owner) -> true | false, reason ("not found", "owner")
int LuaUnregister(lua_State* L)
{
    const pcstr name = lua_type(L, 1) == LUA_TSTRING ? lua_tostring(L, 1) : nullptr;
    const pcstr owner = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : "unknown";
    Entry* entry = Find(name);
    if (!entry)
    {
        lua_pushboolean(L, false);
        lua_pushstring(L, "not found");
        return 2;
    }
    if (entry->owner != owner)
    {
        Msg("! [addon:%s] addons.unregister_command('%s'): the command belongs to addon '%s'", owner, name,
            entry->owner.c_str());
        lua_pushboolean(L, false);
        lua_pushstring(L, "owner");
        return 2;
    }
    g_entries->erase(std::find(g_entries->begin(), g_entries->end(), entry));
    Remove(entry, entry->state == (GEnv.ScriptEngine ? GEnv.ScriptEngine->lua() : nullptr));
    lua_pushboolean(L, true);
    return 1;
}

// console_command_list() -> { {name = , owner = , help = }, ... } in the order of the registration
int LuaList(lua_State* L)
{
    lua_newtable(L);
    int index = 0;
    if (g_entries)
    {
        for (const Entry* entry : *g_entries)
        {
            lua_newtable(L);
            lua_pushstring(L, entry->name.c_str());
            lua_setfield(L, -2, "name");
            lua_pushstring(L, entry->owner.c_str());
            lua_setfield(L, -2, "owner");
            lua_pushstring(L, entry->help.c_str());
            lua_setfield(L, -2, "help");
            lua_rawseti(L, -2, ++index);
        }
    }
    return 1;
}
} // namespace

void LocalizationChanged(pcstr language)
{
    if (!IsMainThread())
        return;
    // The titles, menus and tooltips of the ImGui tools of the addons (20-imgui-addon-tools.md)
    imgui_tools::LocalizationChanged();
    static const GwpEventId id = events::Intern("on_localization_change");
    if (!events::HasSubscribers(id))
        return;
    const GwpValue argv[] = { events::String(language ? language : "") };
    events::Emit(id, argv, 1);
}

void RemoveOwner(pcstr owner)
{
    if (!g_entries || !owner)
        return;
    u32 removed = 0;
    lua_State* main = GEnv.ScriptEngine ? GEnv.ScriptEngine->lua() : nullptr;
    for (auto it = g_entries->begin(); it != g_entries->end();)
    {
        Entry* entry = *it;
        if (entry->owner != owner)
        {
            ++it;
            continue;
        }
        it = g_entries->erase(it);
        Remove(entry, entry->state == main);
        ++removed;
    }
    if (removed)
        Msg("* [addon:%s] addon_reload: %u console command(s) removed", owner, removed);
}

void OnFrame()
{
    if (g_removed && !g_removed->empty())
    {
        for (LuaCommand* command : *g_removed)
            xr_delete(command);
        g_removed->clear();
    }
    if (g_deferred && !g_deferred->empty() && IsMainThread())
    {
        // A function may type another command: take the queue away first
        xr_vector<std::pair<xr_string, xr_string>> commands;
        commands.swap(*g_deferred);
        for (const auto& [name, args] : commands)
            Run(name.c_str(), args.c_str());
    }
}

void Shutdown()
{
    RemoveAll(false); // the Lua state may be gone already: the references go with it
    xr_delete(g_entries);
    xr_delete(g_deferred);
    if (g_removed)
    {
        for (LuaCommand* command : *g_removed)
            xr_delete(command);
    }
    xr_delete(g_removed);
    CConsole::SetExecuteHook(nullptr);
}
} // namespace gw::addons::lua_console

// Registered through the script export list: called for every new Lua state, before any script is loaded.
// Not in the anonymous namespace: the export node must have a normal, always-emitted definition.
struct CLuaConsoleScript
{
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

void CLuaConsoleScript::script_register(lua_State* L)
{
    using namespace gw::addons::lua_console;
    // A new Lua state: the functions of the old one are gone with it, the scripts register their commands again
    RemoveAll(false);
    CConsole::SetExecuteHook(&OnConsoleExecute);

    // _gw_internal is shared with addon_event_bus.cpp, addon_lua_entry.cpp, ...: the first one creates it
    lua_getglobal(L, "_gw_internal");
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "_gw_internal");
    }
    lua_pushcfunction(L, &LuaRegister);
    lua_setfield(L, -2, "console_command_register");
    lua_pushcfunction(L, &LuaUnregister);
    lua_setfield(L, -2, "console_command_unregister");
    lua_pushcfunction(L, &LuaList);
    lua_setfield(L, -2, "console_command_list");
    lua_pop(L, 1);
}
