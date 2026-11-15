#pragma once

// Console commands of the Lua parts of addons and the console / localization events
// (plans/addon_api_ideas/spec.md, gaps P03 and P09):
//  - addons.register_command(name, fn, help) (addons.script) registers a console command whose Execute calls the Lua
//    function fn(args) of the addon. The commands live in this Lua state: a new Lua state drops them all (the scripts
//    register them again in on_load), addon_reload <id> drops the ones of that addon (RemoveOwner). A command of the
//    engine or of a plugin is never replaced.
//  - on_console_execute(name, args): every console command that ran (CConsole::ExecuteHook of xrEngine).
//  - on_localization_change(language): g_language switched the language (console_commands.cpp).
// The Lua side: the functions console_command_* of the global table _gw_internal (not an API for addons: the facade
// addons.register_command / unregister_command / list_commands is). Main thread only.
// Docs: wiki/doc/addons/lua_api.md, wiki/doc/addons/howto/console.md

namespace gw::addons::lua_console
{
// on_localization_change(language): the strings of the new language are loaded, the names of the items reloaded.
void LocalizationChanged(pcstr language);

// addon_reload <owner>: the console commands of the addon go (addon_lua_entry.cpp, after its registrations).
void RemoveOwner(pcstr owner);

// Every frame (CGamePersistent::OnFrame): the commands typed outside the game logic thread run here, the objects of the
// removed commands are freed (a command may remove itself while the console still holds it).
void OnFrame();

// The host shuts down: every command leaves the console.
void Shutdown();
} // namespace gw::addons::lua_console
