#pragma once

#include <initializer_list>

struct lua_State;

// The Lua part of an addon (plans/addons_api_support/08-unified-addons-api-lua-native.md, 4.2 and 4.3):
//   [lua]
//   entry = my_addon_main   ; the module <addon>\gamedata\scripts\my_addon_main.script
//
// In every Lua state - the script engine starts or restarts: the start of the game, a switch of the content in the
// main menu (addon_slots.cpp, CMainMenu::RestartScripts), the start of a game session (CALifeSimulator) - the host
// calls <entry>.on_load(ctx) of each addon whose Lua part is active, in the order of the addons, once. That is right
// after the common scripts and EngineOnScriptStart of the plugins, before the scripts make any window (the main menu,
// the options, the PDA): what on_load registers (gw_registry, gw_options, hooks) is there when they are built.
// ctx = {id, version, path, type}. The plugin of the addon is loaded before its Lua part (Initialize,
// LoadOnDemandPlugins). The Lua side is gw_addons_entry.script.
//
// The Lua part of an addon is active when:
//  - the addon works (addon_list: active; its required on-demand plugin did not fail);
//  - its content is mounted: its scripts lie in its gamedata\ layer. Nothing is mounted before the first frame of
//    the main menu (the Lua state of the start runs no entry, the one after the mount does); a location pack is
//    mounted while it is the active pack;
//  - an addon that provides slots ([slot.<slot>]): while one of its providers is active - its plugin is loaded only
//    then (00-roadmap.md 5a, decision 1). The entry of the provider (<slot entry>.on_slot_active, gw_slots.script)
//    runs after on_load.
// An addon switched off is not called in the next Lua state.
//
// A failing entry does not stop the others: no module, no on_load, a Lua error in it - "[addon:<id>]" in the log,
// the addon is failed in this Lua state (gw_addons_entry.is_failed: its on_slot_active and the on_game_start of its
// modules are not called). What it registered before a Lua error in on_load goes (gw_addons_entry.on_load_failed). An
// addon whose dependency ([addon] depends, patches) is failed is failed too, its on_load is not called. The next Lua
// state tries it again.

namespace gw::addons
{
struct LuaEntryInfo
{
    xr_string id;
    xr_string version; // as written in the manifest
    xr_string type; // [addon] type as written
    xr_string path; // the addon folder: UTF-8, the trailing separator
    xr_string entry; // the Lua module
    // The ids of [addon] depends and [addon] patches, "a,b": a failed Lua part of one of them - on_load of this one is
    // not called (gw_addons_entry.load_addon)
    xr_string depends;
};

// The addons whose Lua part is active now (see above), in the order of the addons (Lua addon_lua.list)
xr_vector<LuaEntryInfo> ActiveLuaEntries();

// The addon whose module the script `module` is (Lua addon_lua.module_owner): the layer of that addon serves
// scripts\<module>.script (the name in any case) and the file covers no file of the base or of a lower layer - the
// rule of the modules of the addons (addon_reload, on_game_start). "" - no such file, the base or a layer of no addon
// serves it, it replaces a script of the base or of a lower addon, or `module` is not a module name. The set of a
// provider of a slot is a layer of its addon too. Lua checks with it that [slot.<slot>] panel / entry is a module of
// the provider itself (ui_mm_appearance.script, gw_slots.script)
xr_string AddonScriptModuleOwner(pcstr module);

// CAI_Space::RestartScriptEngine: on_load of the active Lua parts in the new Lua state. Main thread.
void RunLuaEntries();

// The end of a Lua state: <entry>.on_unload(ctx) of every Lua part RunLuaEntries ran in it whose on_load passed, in
// the reverse order of the addons, one protected call per addon (gw_addons_entry.unload_addon). Called by
// CAI_Space::RestartScriptEngine before the state is replaced (a new game, a load, a switch of the content in the main
// menu) and by ~CAI_Space before the script engine goes (the exit of the game). The exit of a game to the main menu
// does not end the Lua state (the menu keeps it until the next restart), so no on_unload there. Main thread.
void RunLuaUnloadEntries();

// Console addon_reload <id> (08-unified-addons-api-lua-native.md 4.10, U6; addon_host.cpp ReloadAddon): the Lua part
// of one addon goes and comes back in the running Lua state. Main thread, the top of a frame: no Lua code on the
// stack. Each step that runs code of the addon is a protected call of its own: an error is logged, the next steps go.
//  - UnloadLuaEntryForReload: the save data of its modules - its save_state handlers fill a new table, kept in memory
//    in the format of the saves (marshal) - when a game runs, then <entry>.on_unload(ctx) (when its on_load passed;
//    the order of a game: a save comes before the end of a Lua state), then everything its modules registered goes (the subscriptions of the event bus, the hooks, the options, the
//    registries of gw_registry - by owner or by the environment of the functions) and its modules leave the Lua state
//    (_G[<module>] = nil: the next access loads the file again). Its modules: the *.script at the root of
//    $game_scripts$ a layer of the addon serves and that cover no file of the base or of a lower layer.
//    false - there was nothing of it in this Lua state (its Lua part was not loaded, no module of it was loaded);
//  - LoadLuaEntryForReload: the hooks of other addons on functions of its modules go onto the new modules, then
//    <entry>.on_load(ctx) as in RunLuaEntries, the entries of its slot providers, and in a game: on_game_start of
//    its modules, its load_state handlers with the kept data, and when the level runs its on_game_load and
//    actor_on_first_update handlers. An addon without [lua] entry gets the same for its modules (no on_load).
//    false - its Lua part did not load (reason; the Lua side has it as failed);
//  - AbortLuaEntryReload: the reload stops after the unload (its required plugin did not load): the kept data goes,
//    its Lua part is failed in this Lua state with `reason`.
bool UnloadLuaEntryForReload(pcstr addon_id);
bool LoadLuaEntryForReload(pcstr addon_id, xr_string& reason);
void AbortLuaEntryReload(pcstr addon_id, pcstr reason);

// CAI_Space::RegisterScriptClasses, after the class registrators of script.ltx and before the table clsid is made
// (CObjectFactory::register_script): <entry>.on_register_classes(ctx) of the active Lua parts, one protected call per
// addon (gw_registry.run_addon_class; the object factory is the one class_registrator.register gave to
// gw_registry.run_addon_classes). A Lua error in one does not stop the game or the others: the classes it registered
// are not taken, its Lua part is failed in this Lua state (no on_load, as a failed on_load). Main thread.
void RunLuaClassEntries();
// The start of a game session (CALifeSimulator): on_game_start of the modules of the addons, one protected call per
// module. The base calls on_game_start of every module at the root of scripts\ (axr_main.on_game_start inside
// [alife] start_game_callback), unprotected: a Lua error there is a fatal error of the start. The modules of the
// addons (the *.script a layer of an addon serves that replace no script of the base or of a lower addon, see
// gw_addons_entry.set_game_start_modules) are left out of that loop:
//  - PrepareAddonGameStart, before start_game_callback: gives Lua the list of these modules;
//  - RunAddonGameStart, after start_game_callback: calls on_game_start of each of them, in the order of the list. A Lua
//    error in one is logged with the addon, the addon is marked (gw_addons_entry.list: failed, reason
//    "on_game_start ..."), the next modules and the game go on.
// Main thread.
void PrepareAddonGameStart();
void RunAddonGameStart();

// A call of the engine into Lua that runs the code of an addon (on_load, on_slot_active): under lua_pcall, not
// through luabind::functor - the CMake Release build defines LUABIND_NO_EXCEPTIONS, there a Lua error raised through
// a functor ends the process (no catch can take it). The error is logged with the Lua stack.
struct LuaCallArg
{
    // A value the caller pushes itself (a table): it pushes exactly one value onto the stack of the call
    using PushFunction = void (*)(::lua_State* L);

    LuaCallArg(pcstr value) : text(value) {}
    LuaCallArg(bool value) : is_bool(true), flag(value) {}
    LuaCallArg(PushFunction value) : push(value) {}

    pcstr text = nullptr; // nullptr - nil
    bool is_bool = false;
    bool flag = false;
    PushFunction push = nullptr;
};
enum class ELuaCall
{
    Ok,
    NotFound, // no such Lua function (no script engine, no module, not a function)
    Failed, // a Lua error in it (logged)
};
// `function` - "<module>.<name>", found as CScriptEngine::functor finds it. `result` - its first return value as a
// Lua boolean (false on an error); `error` - the message of the Lua error. Main thread.
ELuaCall CallLuaProtected(pcstr function, std::initializer_list<LuaCallArg> args, bool* result = nullptr,
    xr_string* error = nullptr);
} // namespace gw::addons
