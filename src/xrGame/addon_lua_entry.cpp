#include "StdAfx.h"

#include "addon_lua_entry.h"
#include "addon_host.h"
#include "addon_content.h"
#include "addon_merge.h"
#include "addon_slots.h"

#include "xrScriptEngine/script_engine.hpp"

#include <algorithm>
#include <cctype>

namespace gw::addons
{
namespace
{
// gw_addons_entry.script: load_addon(id, version, path, type, entry, depends) -> bool calls <entry>.on_load(ctx)
// (depends: the addons it depends on, "a,b"; a failed Lua part of one of them - on_load is not called);
// on_load_failed(id, message, modules) - the engine caught a Lua error in it (modules: "a,b,c", the modules of the
// addon - what they registered goes)
constexpr pcstr kLuaEntryLoadFunction = "gw_addons_entry.load_addon";
constexpr pcstr kLuaEntryFailedFunction = "gw_addons_entry.on_load_failed";
// gw_addons_entry.script, on_game_start of the modules of the addons at the start of a game session
// (PrepareAddonGameStart, RunAddonGameStart):
//   set_game_start_modules(list)   the modules of the addons, "module=id,module=id" (axr_main.on_game_start leaves
//                                  them to the engine);
//   run_next_game_start() -> bool  runs on_game_start of one of them (false - none is left);
//   game_start_call_failed(message) the engine caught a Lua error in the call run_next_game_start ran
constexpr pcstr kLuaGameStartModules = "gw_addons_entry.set_game_start_modules";
constexpr pcstr kLuaGameStartRunNext = "gw_addons_entry.run_next_game_start";
constexpr pcstr kLuaGameStartFailed = "gw_addons_entry.game_start_call_failed";
// gw_registry.script: run_addon_class(id, version, path, type, entry) -> bool calls <entry>.on_register_classes(ctx);
// on_addon_classes_failed(id, message) - the engine caught a Lua error in it; end_addon_classes() - the classes are
// taken, register_class refuses from now on
constexpr pcstr kLuaClassesFunction = "gw_registry.run_addon_class";
constexpr pcstr kLuaClassesFailedFunction = "gw_registry.on_addon_classes_failed";
constexpr pcstr kLuaClassesEndFunction = "gw_registry.end_addon_classes";
// gw_addons_entry.script: unload_addon(id) -> bool calls <entry>.on_unload(ctx) at the end of a Lua state
constexpr pcstr kLuaEntryUnloadFunction = "gw_addons_entry.unload_addon";
// gw_addons_entry.script, console addon_reload (the steps - UnloadLuaEntryForReload, LoadLuaEntryForReload):
//   reload_begin(id, version, path, type, entry, modules) -> bool   the reload starts (modules: "a,b,c")
//   reload_phase(phase) -> bool   queues the calls of a phase: "unload", "save", "game_start", "load_state", "level"
//   reload_run_next() -> bool     runs one queued call (false - the queue is empty)
//   reload_call_failed(message)   the engine caught a Lua error in the call reload_run_next ran
//   reload_remove()               the registrations of the modules go, the modules leave the Lua state
//   reload_attach()               the hooks of other addons go onto the new modules
//   reload_end(ok, reason)        the reload is over (not ok: the Lua part is failed with the reason)
constexpr pcstr kLuaReloadBegin = "gw_addons_entry.reload_begin";
constexpr pcstr kLuaReloadPhase = "gw_addons_entry.reload_phase";
constexpr pcstr kLuaReloadRunNext = "gw_addons_entry.reload_run_next";
constexpr pcstr kLuaReloadCallFailed = "gw_addons_entry.reload_call_failed";
constexpr pcstr kLuaReloadRemove = "gw_addons_entry.reload_remove";
constexpr pcstr kLuaReloadAttach = "gw_addons_entry.reload_attach";
constexpr pcstr kLuaReloadEnd = "gw_addons_entry.reload_end";
// gw_slots.script: rearm_entries(id) - the entries of the slot providers of the addon run again (RunSlotProviderEntries)
constexpr pcstr kLuaSlotsRearm = "gw_slots.rearm_entries";

// The Lua parts RunLuaEntries ran in the current Lua state: on_unload goes to them when it ends (RunLuaUnloadEntries)
xr_vector<LuaEntryInfo> g_state_entries;

// Its scripts are in the file system: a layer of the content stack belongs to it
bool LuaEntryContentMounted(const xr_string& id)
{
    const xr_vector<ContentLayer>& layers = MountedContentLayers();
    return std::any_of(layers.begin(), layers.end(), [&id](const ContentLayer& layer) { return layer.owner == id; });
}

// The addon provides slots: one of its providers must be active (world: a location pack - its layer tells)
bool LuaEntryProviderActive(const xr_string& id)
{
    bool provides = false;
    for (const gw::slots::SlotInfo& slot : gw::slots::Slots())
    {
        if (slot.id == gw::slots::kWorldSlot)
            continue;
        for (const gw::slots::ProviderInfo& provider : slot.providers)
        {
            if (provider.builtin || provider.id != id)
                continue;
            provides = true;
            if (&provider == &slot.active_provider())
                return true;
        }
    }
    return !provides;
}

// The message handler of CallLuaProtected and _gw_internal.protected_call / protected_call_quiet: the message with the
// Lua stack of the error. A message that has a stack already is left as it is: an error that a quiet call caught and
// its caller raised again (gw_hooks: the error of next goes through the hook unchanged; error(text, 0)) - its stack is
// the stack of the place of the error, a second one would only show the way up through the hooks
int LuaEntryTraceback(lua_State* L)
{
    pcstr const message = lua_tostring(L, 1);
    if (message && strstr(message, "\nstack traceback:"))
    {
        lua_settop(L, 1);
        return 1;
    }
    luaL_traceback(L, L, message && message[0] ? message : "(a Lua error without a message)", 1);
    return 1;
}

// The owner of the layer that serves the file `name` at the root of $game_scripts$ ("<module>.script", lower case) and
// that layer: nullptr - no such file, the base serves it, or a layer of no addon serves it (the merged files of the
// addons, a writable layer). The rule of the modules of the addons: AddonOwnedScriptModules, AddonScriptModuleOwner
pcstr LuaEntryScriptOwner(pcstr name, u16& layer)
{
    layer = 0;
    string_path full;
    FS.update_path(full, "$game_scripts$", name);
    const CLocatorAPI::file* desc = FS.GetFileDesc(full);
    if (!desc || desc->layer == 0)
        return nullptr;
    layer = desc->layer;
    return FS.layer_owner(desc->layer);
}

// The file `name` of the layer `layer` (LuaEntryScriptOwner) covers a file of the base or of a lower layer
bool LuaEntryScriptReplaces(pcstr name, u16 layer)
{
    IReader* under = layer ? FS.r_open_under("$game_scripts$", name, layer - 1) : nullptr;
    if (!under)
        return false;
    FS.r_close(under);
    return true;
}

// The modules of the addons: the *.script at the root of $game_scripts$ a layer of an addon serves (not the base, not
// the merged files of the addons, not a writable layer) and that cover no file of the base or of a lower layer.
// A script that replaces a script of the base (or of a lower addon) is a module of the base: others hold it and
// register into it (xr_conditions, modules). { module (lower case), addon id }, in the order of the file list.
// addon_id: the modules of that addon only (nullptr - of every addon); log_replaced: a line for each script of it left
// out as a replacement (addon_reload)
xr_vector<std::pair<xr_string, xr_string>> AddonOwnedScriptModules(pcstr addon_id, bool log_replaced)
{
    xr_vector<std::pair<xr_string, xr_string>> result;
    xr_vector<xr_string> addon_ids;
    if (!addon_id)
    {
        for (const AddonInfo& addon : ListAddons())
            addon_ids.push_back(addon.id);
    }
    FS_FileSet files;
    FS.file_list(files, "$game_scripts$", FS_ListFiles | FS_RootOnly, "*.script");
    constexpr size_t extension = 7; // ".script"
    for (const FS_File& file : files)
    {
        if (file.name.size() <= extension)
            continue;
        u16 layer = 0;
        const pcstr owner = LuaEntryScriptOwner(file.name.c_str(), layer);
        if (!owner)
            continue; // the base serves it, the merged files of the addons, a writable layer
        const bool owned = addon_id ? xr_strcmp(owner, addon_id) == 0 :
            std::find(addon_ids.begin(), addon_ids.end(), owner) != addon_ids.end();
        if (!owned)
            continue; // another addon
        if (LuaEntryScriptReplaces(file.name.c_str(), layer))
        {
            if (log_replaced)
            {
                Msg("~ [addon:%s] addon_reload: scripts\\%s replaces a script of the base or of another addon - its "
                    "module is not reloaded", owner, file.name.c_str());
            }
            continue;
        }
        xr_string module = file.name.substr(0, file.name.size() - extension);
        std::transform(module.begin(), module.end(), module.begin(),
            [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        result.emplace_back(std::move(module), xr_string(owner));
    }
    return result;
}

// The modules of one addon (see AddonOwnedScriptModules): "a,b,c", lower case
xr_string AddonScriptModules(pcstr addon_id, bool log_replaced)
{
    xr_string result;
    for (const auto& [module, owner] : AddonOwnedScriptModules(addon_id, log_replaced))
    {
        if (!result.empty())
            result += ',';
        result += module;
    }
    return result;
}

void LuaEntryReportFailure(const LuaEntryInfo& info, const xr_string& what)
{
    Msg("! [addon:%s] [lua] %s.on_load failed (the stack is above): what it registered is removed", info.id.c_str(),
        info.entry.c_str());
    // Only the first line of the message goes to Lua (the stack is in the log)
    const xr_string message = what.substr(0, what.find('\n'));
    // The modules of the addon: what they registered in this on_load goes (gw_addons_entry.on_load_failed)
    const xr_string modules = AddonScriptModules(info.id.c_str(), false);
    if (CallLuaProtected(kLuaEntryFailedFunction, { info.id.c_str(), message.c_str(), modules.c_str() }) ==
        ELuaCall::NotFound)
        Msg("! [addons] Lua function %s not found", kLuaEntryFailedFunction);
}
} // namespace

xr_string AddonScriptModuleOwner(pcstr module)
{
    const size_t length = module ? xr_strlen(module) : 0;
    if (length == 0 || length > 64)
        return {};
    // a name of a module: no path, no wildcard (the same characters as [lua] entry / [slot.*] entry)
    const bool valid = std::all_of(module, module + length, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
    if (!valid)
        return {};
    xr_string name = module;
    name += ".script";
    xr_strlwr(name);
    u16 layer = 0;
    const pcstr owner = LuaEntryScriptOwner(name.c_str(), layer);
    if (!owner || LuaEntryScriptReplaces(name.c_str(), layer))
        return {};
    return owner;
}

xr_vector<LuaEntryInfo> ActiveLuaEntries()
{
    xr_vector<LuaEntryInfo> result;
    for (const AddonInfo& addon : ListAddons())
    {
        if (addon.lua_entry.empty() || !addon.active || gw::slots::AddonFailed(addon.id.c_str()))
            continue;
        if (!LuaEntryContentMounted(addon.id) || !LuaEntryProviderActive(addon.id))
            continue;
        LuaEntryInfo& info = result.emplace_back();
        info.id = addon.id;
        info.version = addon.version;
        info.type = addon.type;
        info.path = addon.dir;
        info.entry = addon.lua_entry;
        // [addon] depends and the addons of a compatibility patch ([addon] patches): it works only with them
        for (const xr_vector<xr_string>* ids : { &addon.depends, &addon.patches })
        {
            for (const xr_string& id : *ids)
            {
                if (!info.depends.empty())
                    info.depends += ',';
                info.depends += id;
            }
        }
    }
    return result;
}

namespace
{
// <entry>.on_slot_active of the active slot providers (gw_slots.run_next_entry runs one pending entry per call):
// one protected call per provider, so a Lua error in one provider does not stop the others. After the on_load of
// the [lua] entries: a provider entry may rely on its addon's on_load.
void RunSlotProviderEntries()
{
    constexpr u32 max_entries = 4096; // a guard: each call marks its entry done before running it
    for (u32 i = 0; i < max_entries; ++i)
    {
        bool more = false;
        xr_string error;
        const ELuaCall status = CallLuaProtected("gw_slots.run_next_entry", {}, &more, &error);
        if (status == ELuaCall::NotFound)
            return;
        if (status == ELuaCall::Failed)
        {
            Msg("! [slots] on_slot_active of a slot provider failed: %s", error.c_str());
            continue;
        }
        if (!more)
            return;
    }
}
} // namespace

void RunLuaEntries()
{
    g_state_entries.clear(); // a new Lua state: the entries of the old one got on_unload in RunLuaUnloadEntries
    if (!GEnv.ScriptEngine)
        return;
    const xr_vector<LuaEntryInfo> entries = ActiveLuaEntries();
    g_state_entries = entries;
    if (entries.empty())
    {
        RunSlotProviderEntries();
        return;
    }
    u32 loaded = 0;
    for (const LuaEntryInfo& info : entries)
    {
        // One protected call per addon: a Lua error in one entry is caught here, the next entries still run
        bool ok = false;
        xr_string error;
        const ELuaCall status = CallLuaProtected(kLuaEntryLoadFunction, { info.id.c_str(), info.version.c_str(),
            info.path.c_str(), info.type.c_str(), info.entry.c_str(), info.depends.c_str() }, &ok, &error);
        if (status == ELuaCall::NotFound)
        {
            Msg("! [addons] Lua function %s not found: the [lua] entries of %u addon(s) do not run",
                kLuaEntryLoadFunction, u32(entries.size()));
            return;
        }
        if (status == ELuaCall::Failed)
            LuaEntryReportFailure(info, error);
        else if (ok)
            ++loaded;
    }
    Msg("* [addons] Lua entries: %u of %u loaded", loaded, u32(entries.size()));
    RunSlotProviderEntries();
}

void RunLuaUnloadEntries()
{
    // Nothing ran in this state (the first state of a start, before the content is mounted, has no entries): no Lua
    // call at all - the script engine may have no Lua state yet (the first CAI_Space::RestartScriptEngine)
    if (g_state_entries.empty() || !GEnv.ScriptEngine || !GEnv.ScriptEngine->lua())
    {
        g_state_entries.clear();
        return;
    }
    const xr_vector<LuaEntryInfo> entries = std::move(g_state_entries);
    g_state_entries.clear();
    u32 unloaded = 0;
    u32 failed = 0;
    // The reverse order: an addon that uses another one goes before it
    for (auto it = entries.rbegin(); it != entries.rend(); ++it)
    {
        const LuaEntryInfo& info = *it;
        bool ran = false;
        xr_string error;
        const ELuaCall status = CallLuaProtected(kLuaEntryUnloadFunction, { info.id.c_str() }, &ran, &error);
        if (status == ELuaCall::NotFound)
        {
            Msg("! [addons] Lua function %s not found: on_unload of the [lua] entries does not run",
                kLuaEntryUnloadFunction);
            return;
        }
        if (status == ELuaCall::Failed)
        {
            ++failed;
            Msg("! [addon:%s] [lua] %s.on_unload failed (the stack is above)", info.id.c_str(), info.entry.c_str());
        }
        else if (ran)
            ++unloaded;
    }
    if (unloaded || failed)
        Msg("* [addons] Lua entries: on_unload of %u addon(s) done, %u failed", unloaded, failed);
}

namespace
{
// One step of the reload in Lua that runs no code of the addon (it is ours): false when it is not there or failed
bool ReloadCall(pcstr function, std::initializer_list<LuaCallArg> args, bool* result = nullptr)
{
    xr_string error;
    const ELuaCall status = CallLuaProtected(function, args, result, &error);
    if (status == ELuaCall::NotFound)
        Msg("! [addons] addon_reload: Lua function %s not found (the scripts are older than the engine)", function);
    return status == ELuaCall::Ok;
}

// A phase of the reload: Lua queues the calls (each one runs code of the addon), the engine runs them one by one under
// lua_pcall - an error in one is logged and the next ones run. Returns the number of the failed calls.
u32 RunReloadPhase(pcstr addon_id, pcstr phase)
{
    bool queued = false;
    if (!ReloadCall(kLuaReloadPhase, { phase }, &queued) || !queued)
        return 0;
    constexpr u32 max_calls = 16384; // a guard: each call takes its item off the queue before running it
    u32 failed = 0;
    for (u32 i = 0; i < max_calls; ++i)
    {
        bool more = false;
        xr_string error;
        const ELuaCall status = CallLuaProtected(kLuaReloadRunNext, {}, &more, &error);
        if (status == ELuaCall::NotFound)
            break;
        if (status == ELuaCall::Failed)
        {
            ++failed;
            Msg("! [addon:%s] addon_reload: a call of the phase '%s' failed (the stack is above)", addon_id, phase);
            const xr_string message = error.substr(0, error.find('\n'));
            CallLuaProtected(kLuaReloadCallFailed, { message.c_str() });
            continue;
        }
        if (!more)
            break;
    }
    return failed;
}
} // namespace

bool UnloadLuaEntryForReload(pcstr addon_id)
{
    if (!GEnv.ScriptEngine || !GEnv.ScriptEngine->lua())
        return false;
    // ctx of the entry: the record of RunLuaEntries, else the host (a part that was not active in this state)
    LuaEntryInfo info;
    info.id = addon_id;
    bool known = false;
    for (const LuaEntryInfo& entry : g_state_entries)
    {
        if (entry.id == addon_id)
        {
            info = entry;
            known = true;
            break;
        }
    }
    if (!known)
    {
        for (const AddonInfo& addon : ListAddons())
        {
            if (addon.id != addon_id)
                continue;
            info.version = addon.version;
            info.type = addon.type;
            info.path = addon.dir;
            info.entry = addon.lua_entry;
            break;
        }
    }
    const xr_string modules = AddonScriptModules(addon_id, true);
    Msg("* [addon:%s] addon_reload: its modules: %s", addon_id, modules.empty() ? "(none)" : modules.c_str());

    bool present = false;
    if (!ReloadCall(kLuaReloadBegin, { info.id.c_str(), info.version.c_str(), info.path.c_str(), info.type.c_str(),
            info.entry.c_str(), modules.c_str() }, &present))
        return false;
    // The save data first, then on_unload - the order of a game: a save comes before the end of the Lua state, and
    // on_unload may clear the tables of the modules
    const u32 save_failed = RunReloadPhase(addon_id, "save");
    const u32 unload_failed = RunReloadPhase(addon_id, "unload");
    if (!ReloadCall(kLuaReloadRemove, {}))
        Msg("! [addon:%s] addon_reload: its registrations were not removed (the error is above)", addon_id);
    Msg("* [addon:%s] addon_reload: unloaded (on_unload: %s, save data: %s)", addon_id,
        unload_failed ? "failed" : "ok", save_failed ? "incomplete, not carried over" : "ok");
    return present;
}

bool LoadLuaEntryForReload(pcstr addon_id, xr_string& reason)
{
    reason.clear();
    if (!GEnv.ScriptEngine || !GEnv.ScriptEngine->lua())
    {
        reason = "no script engine";
        return false;
    }
    bool has_entry = false;
    for (const AddonInfo& addon : ListAddons())
    {
        if (addon.id == addon_id)
        {
            has_entry = !addon.lua_entry.empty();
            break;
        }
    }

    if (!ReloadCall(kLuaReloadAttach, {}))
        Msg("! [addon:%s] addon_reload: the hooks on its modules were not moved onto the new modules", addon_id);

    // An addon without [lua] entry: only its modules (on_game_start and the rest below)
    if (has_entry)
    {
        const xr_vector<LuaEntryInfo> entries = ActiveLuaEntries();
        const auto it = std::find_if(entries.begin(), entries.end(),
            [addon_id](const LuaEntryInfo& info) { return info.id == addon_id; });
        if (it == entries.end())
        {
            reason = "its Lua part is not active now (addon_list: the addon does not work, its content is not mounted "
                     "or its slot provider is not chosen)";
            ReloadCall(kLuaReloadEnd, { false, reason.c_str() });
            return false;
        }
        const LuaEntryInfo info = *it;
        // The entry is one of the parts of this Lua state now: on_unload at its end
        const auto known = std::find_if(g_state_entries.begin(), g_state_entries.end(),
            [addon_id](const LuaEntryInfo& entry) { return entry.id == addon_id; });
        if (known == g_state_entries.end())
            g_state_entries.push_back(info);
        else
            *known = info;

        bool ok = false;
        xr_string error;
        const ELuaCall status = CallLuaProtected(kLuaEntryLoadFunction, { info.id.c_str(), info.version.c_str(),
            info.path.c_str(), info.type.c_str(), info.entry.c_str(), info.depends.c_str() }, &ok, &error);
        if (status != ELuaCall::Ok || !ok)
        {
            if (status == ELuaCall::Failed)
            {
                LuaEntryReportFailure(info, error);
                reason = error.substr(0, error.find('\n'));
            }
            else if (status == ELuaCall::NotFound)
                reason = xr_string("Lua function ") + kLuaEntryLoadFunction + " not found";
            else
                reason = "no module, no on_load or a failed dependency (the warning is above)";
            ReloadCall(kLuaReloadEnd, { false, reason.c_str() });
            return false;
        }
    }
    // Its slot providers: on_slot_active again (after on_load, as in RunLuaEntries). An addon without [lua] entry too:
    // the module of its slot entry was reloaded and what on_slot_active registered was removed with it
    if (CallLuaProtected(kLuaSlotsRearm, { addon_id }) == ELuaCall::Ok)
        RunSlotProviderEntries();
    const u32 failed = RunReloadPhase(addon_id, "game_start") + RunReloadPhase(addon_id, "load_state") +
        RunReloadPhase(addon_id, "level");
    ReloadCall(kLuaReloadEnd, { true, "" });
    if (failed)
        Msg("~ [addon:%s] addon_reload: on_load passed, %u call(s) after it failed (the stacks are above)", addon_id,
            failed);
    return true;
}

void AbortLuaEntryReload(pcstr addon_id, pcstr reason)
{
    Msg("! [addon:%s] addon_reload: its Lua part is not loaded again: %s", addon_id, reason ? reason : "");
    ReloadCall(kLuaReloadEnd, { false, reason ? reason : "addon_reload failed" });
}

void RunLuaClassEntries()
{
    if (!GEnv.ScriptEngine)
        return;
    const xr_vector<LuaEntryInfo> entries = ActiveLuaEntries();
    u32 registered = 0;
    u32 failed = 0;
    for (const LuaEntryInfo& info : entries)
    {
        // One protected call per addon, as for on_load: a Lua error in one is caught here, the next ones still run
        bool ok = false;
        xr_string error;
        const ELuaCall status = CallLuaProtected(kLuaClassesFunction, { info.id.c_str(), info.version.c_str(),
            info.path.c_str(), info.type.c_str(), info.entry.c_str() }, &ok, &error);
        if (status == ELuaCall::NotFound)
        {
            Msg("! [addons] Lua function %s not found: the classes of %u addon(s) are not registered",
                kLuaClassesFunction, u32(entries.size()));
            break;
        }
        if (status == ELuaCall::Failed)
        {
            ++failed;
            Msg("! [addon:%s] [lua] %s.on_register_classes failed (the stack is above): its classes are not registered, "
                "its Lua part does not load in this Lua state", info.id.c_str(), info.entry.c_str());
            // Only the first line of the message goes to Lua (the stack is in the log)
            const xr_string message = error.substr(0, error.find('\n'));
            if (CallLuaProtected(kLuaClassesFailedFunction, { info.id.c_str(), message.c_str() }) == ELuaCall::NotFound)
                Msg("! [addons] Lua function %s not found", kLuaClassesFailedFunction);
        }
        else if (ok)
            ++registered;
    }
    if (registered || failed)
        Msg("* [addons] classes of the addons: on_register_classes of %u addon(s) done, %u failed", registered, failed);
    CallLuaProtected(kLuaClassesEndFunction, {});
}

void PrepareAddonGameStart()
{
    if (!GEnv.ScriptEngine || !GEnv.ScriptEngine->lua())
        return;
    // Also with no module of an addon: the list of the previous game session goes
    xr_string list;
    for (const auto& [module, owner] : AddonOwnedScriptModules(nullptr, false))
    {
        if (!list.empty())
            list += ',';
        list += module;
        list += '=';
        list += owner;
    }
    if (CallLuaProtected(kLuaGameStartModules, { list.c_str() }) == ELuaCall::NotFound && !list.empty())
    {
        Msg("! [addons] Lua function %s not found (the scripts are older than the engine): on_game_start of the "
            "modules of the addons runs unprotected", kLuaGameStartModules);
    }
}

void RunAddonGameStart()
{
    if (!GEnv.ScriptEngine || !GEnv.ScriptEngine->lua())
        return;
    constexpr u32 max_calls = 16384; // a guard: each call takes its module off the queue before running it
    u32 failed = 0;
    for (u32 i = 0; i < max_calls; ++i)
    {
        bool more = false;
        xr_string error;
        const ELuaCall status = CallLuaProtected(kLuaGameStartRunNext, {}, &more, &error);
        if (status == ELuaCall::NotFound)
            break;
        if (status == ELuaCall::Failed)
        {
            ++failed;
            // Only the first line of the message goes to Lua (the stack is in the log); Lua names the module and
            // the addon
            const xr_string message = error.substr(0, error.find('\n'));
            CallLuaProtected(kLuaGameStartFailed, { message.c_str() });
            continue;
        }
        if (!more)
            break;
    }
    if (failed)
        Msg("! [addons] on_game_start of %u module(s) of the addons failed (the stacks are above), the game goes on",
            failed);
}

ELuaCall CallLuaProtected(pcstr function, std::initializer_list<LuaCallArg> args, bool* result, xr_string* error)
{
    if (result)
        *result = false;
    if (!GEnv.ScriptEngine || !function)
        return ELuaCall::NotFound;
    luabind::object target;
    // function_object loads the script of the namespace when needed, as CScriptEngine::functor does
    if (!GEnv.ScriptEngine->function_object(function, target, LUA_TFUNCTION))
        return ELuaCall::NotFound;
    lua_State* L = GEnv.ScriptEngine->lua();
    if (!lua_checkstack(L, static_cast<int>(args.size()) + 3))
    {
        Msg("! [addons] Lua call %s: no room on the Lua stack", function);
        return ELuaCall::Failed;
    }
    const int top = lua_gettop(L);
    lua_pushcfunction(L, &LuaEntryTraceback);
    target.push(L);
    if (!lua_isfunction(L, -1))
    {
        lua_settop(L, top);
        return ELuaCall::NotFound;
    }
    for (const LuaCallArg& arg : args)
    {
        if (arg.push)
            arg.push(L);
        else if (arg.is_bool)
            lua_pushboolean(L, arg.flag ? 1 : 0);
        else if (arg.text)
            lua_pushstring(L, arg.text);
        else
            lua_pushnil(L);
    }
    const int status = lua_pcall(L, static_cast<int>(args.size()), 1, top + 1);
    if (status != 0)
    {
        const pcstr message = lua_tostring(L, -1);
        const xr_string text = message ? message : "(a Lua error without a message)";
        Msg("! [addons] Lua error in %s: %s", function, text.c_str());
        DescribeMergedLuaError(text.c_str()); // a line of a merged script: the addon it came from
        if (error)
            *error = text;
        lua_settop(L, top);
        return ELuaCall::Failed;
    }
    if (result)
        *result = lua_toboolean(L, -1) != 0;
    lua_settop(L, top);
    return ELuaCall::Ok;
}

namespace
{
// _gw_internal.protected_call / protected_report: the failures of one callback (owner + what, or owner + the group of
// protected_report) in this Lua state. Allocated on the first failure, cleared by every new Lua state
// (CAddonProtectedCallScript::script_register)
xr_unordered_map<xr_string, u32>* g_addon_protected_failures = nullptr;
// A callback logs its error with the Lua stack this many times per Lua state, then one line at the 10th, 100th ...
// failure: a broken condition of an addon fails on every update of every object that checks it
constexpr u32 kAddonProtectedFullReports = 3;
// protected_call / protected_call_quiet: the Lua stack cannot grow by one slot above the results of fn (lua_checkstack;
// LuaJIT: about 65500 values) - the call ends as failed, the results are dropped
constexpr pcstr kAddonResultsDoNotFit =
    "the results of the callback do not fit on the Lua stack: it ran, its results are dropped";

bool AddonProtectedReportCount(u32 failures)
{
    for (u32 mark = 10; mark && mark <= failures; mark = mark <= type_max<u32> / 10 ? mark * 10 : 0)
    {
        if (mark == failures)
            return true;
    }
    return false;
}

// The log of a failed callback of an addon (protected_call, protected_report): the message and the Lua stack the first
// kAddonProtectedFullReports times per owner + what in this Lua state, then one line at the 10th, 100th ... failure.
// group (nullptr - what): what the failures are counted as, for a what that names one of many callbacks made at run
// time (_g.ProcessEventQueue: "time event [<event id>][<action id>] ..." - the ids are often made per object, a count
// per what would add a key for each of them and never reach its limit). The full lines name what, the count - group
void ReportAddonProtectedFailure(pcstr owner, pcstr what, const xr_string& text, pcstr group = nullptr)
{
    if (!g_addon_protected_failures)
        g_addon_protected_failures = xr_new<xr_unordered_map<xr_string, u32>>();
    pcstr const counted = group ? group : what;
    xr_string key = owner;
    key += '\n';
    key += counted;
    u32& failures = (*g_addon_protected_failures)[key];
    if (failures < type_max<u32>)
        ++failures;
    if (failures <= kAddonProtectedFullReports)
    {
        if (failures < kAddonProtectedFullReports)
            Msg("! [addon:%s] %s failed:", owner, what);
        else if (group)
            Msg("! [addon:%s] %s failed (the next failures of %s are counted, not logged in full):", owner, what, group);
        else
            Msg("! [addon:%s] %s failed (its next failures are counted, not logged in full):", owner, what);
        Log(text.c_str()); // the message and the Lua stack: longer than one Msg may be
        DescribeMergedLuaError(text.c_str());
    }
    else if (AddonProtectedReportCount(failures))
    {
        const xr_string first_line = text.substr(0, text.find('\n'));
        Msg("! [addon:%s] %s failed %u times (the first errors with the stack are above): %s", owner, counted, failures,
            first_line.c_str());
    }
}

// The owner and the what of protected_call / protected_report: strings (else "unknown", "a callback")
pcstr AddonProtectedOwner(lua_State* L, int index)
{
    return lua_type(L, index) == LUA_TSTRING ? lua_tostring(L, index) : "unknown";
}

pcstr AddonProtectedWhat(lua_State* L, int index)
{
    return lua_type(L, index) == LUA_TSTRING ? lua_tostring(L, index) : "a callback";
}

// _gw_internal.protected_call(owner, what, fn, ...) -> true, <the results of fn> | false, <the first line of the error>
//
// The scripts of the mod (gw_hooks, gw_registry, gw_options, utils.protect_addon_fn) call the code an addon gave them
// (a hook, a condition, an effect, a keybind, the button of a menu, a callback of an option) through this when the
// owner is not the base. CLAUDE.md of the project forbids pcall in the Lua scripts, so the protection is here, in C++:
// lua_pcall with the message handler of CallLuaProtected (the message and the Lua stack), the log of the engine and the
// addon of a line of a merged script (DescribeMergedLuaError). A Lua error in fn is logged as
//   ! [addon:<owner>] <what> failed:
//   <the message and the stack>
// and does not go further: the caller gets false and gives its safe default (what says which, e.g.
// "condition 'x' (false)"). Nothing is marked failed, the next call runs fn again. owner and what: strings (nil -
// "unknown", "a callback"); fn may be anything callable. The traceback handler is upvalue 1 (no closure is made per
// call); the results of fn stay on the stack as they are (LUA_MULTRET), the handler slot becomes the true in front of
// them. Internal to the scripts of the mod, not an API for addons. Main thread (the Lua state).
int LuaAddonProtectedCall(lua_State* L)
{
    const int args = lua_gettop(L);
    if (args < 3)
        return luaL_error(L, "_gw_internal.protected_call(owner, what, fn, ...): fn is missing");
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 3); // owner, what, handler, fn, args...
    if (lua_pcall(L, args - 3, LUA_MULTRET, 3) == 0)
    {
        // LUA_MULTRET leaves no free slot above the results: the true needs one
        if (!lua_checkstack(L, 1))
        {
            lua_settop(L, 2); // owner, what
            const xr_string text = kAddonResultsDoNotFit;
            ReportAddonProtectedFailure(AddonProtectedOwner(L, 1), AddonProtectedWhat(L, 2), text);
            lua_pushboolean(L, 0);
            lua_pushlstring(L, text.c_str(), text.size());
            return 2;
        }
        lua_pushboolean(L, 1);
        lua_replace(L, 3); // owner, what, true, results...
        return lua_gettop(L) - 2;
    }
    // owner, what, handler, message
    const pcstr message = lua_tostring(L, -1);
    const xr_string text = message && message[0] ? message : "(a Lua error without a message)";
    const xr_string first_line = text.substr(0, text.find('\n'));
    ReportAddonProtectedFailure(AddonProtectedOwner(L, 1), AddonProtectedWhat(L, 2), text);
    lua_pushboolean(L, 0);
    lua_pushlstring(L, first_line.c_str(), first_line.size());
    return 2;
}

// _gw_internal.protected_call_quiet(fn, ...) -> true, <the results of fn> | false, <the first line of the error>, <the
// message with the Lua stack>
//
// protected_call without the log: the caller decides what the error is and logs it once through protected_report (or
// raises it again with error(text, 0)). For callers that learn only after the call whose error it is: gw_hooks
// (an error inside next is not the error of the hook), _g.ProcessEventQueue and _g.AddUniqueCall (the addon of a time
// event is looked up only on an error). Nothing is counted. The handler (LuaEntryTraceback: a message that has a stack
// already keeps only it) is upvalue 1; the results of fn stay on the stack as they are, the handler slot becomes the
// true in front of them. Internal to the scripts of the mod. Main thread (the Lua state); a coroutine cannot yield
// through it (a C function with lua_pcall inside).
int LuaAddonProtectedCallQuiet(lua_State* L)
{
    const int args = lua_gettop(L);
    if (args < 1)
        return luaL_error(L, "_gw_internal.protected_call_quiet(fn, ...): fn is missing");
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1); // handler, fn, args...
    if (lua_pcall(L, args - 1, LUA_MULTRET, 1) == 0)
    {
        // LUA_MULTRET leaves no free slot above the results: the true needs one
        if (!lua_checkstack(L, 1))
        {
            lua_settop(L, 0);
            lua_pushboolean(L, 0);
            lua_pushstring(L, kAddonResultsDoNotFit); // the first line
            lua_pushstring(L, kAddonResultsDoNotFit); // the message (no stack: not an error of Lua)
            return 3;
        }
        lua_pushboolean(L, 1);
        lua_replace(L, 1); // true, results...
        return lua_gettop(L);
    }
    // handler, message
    const pcstr message = lua_tostring(L, -1);
    const xr_string text = message && message[0] ? message : "(a Lua error without a message)";
    const size_t line_end = text.find('\n');
    lua_pushboolean(L, 0);
    lua_pushlstring(L, text.c_str(), line_end == xr_string::npos ? text.size() : line_end);
    lua_pushlstring(L, text.c_str(), text.size());
    return 3;
}

// _gw_internal.protected_report(owner, what, text[, group]): logs a failure that protected_call_quiet caught, as
// protected_call does (the same lines and the same count per owner + what). text: the third result of
// protected_call_quiet. group (a string, optional): count the failure per owner + group instead of what - for a what
// that names one of many callbacks made at run time (_g.ProcessEventQueue: the ids of a time event)
int LuaAddonProtectedReport(lua_State* L)
{
    const pcstr message = lua_type(L, 3) == LUA_TSTRING ? lua_tostring(L, 3) : nullptr;
    const xr_string text = message && message[0] ? message : "(a Lua error without a message)";
    const pcstr group = lua_type(L, 4) == LUA_TSTRING ? lua_tostring(L, 4) : nullptr;
    ReportAddonProtectedFailure(AddonProtectedOwner(L, 1), AddonProtectedWhat(L, 2), text, group);
    return 0;
}

// _gw_internal[name] = the C closure of fn with the handler as upvalue 1 (nullptr - no upvalue); the table is on the top
void SetInternalFunction(lua_State* L, pcstr name, lua_CFunction fn, lua_CFunction handler)
{
    int upvalues = 0;
    if (handler)
    {
        lua_pushcfunction(L, handler);
        upvalues = 1;
    }
    lua_pushcclosure(L, fn, upvalues);
    lua_setfield(L, -2, name);
}
} // namespace

// Registered through the script export list: called for every new Lua state, before any script is loaded.
// Not in the anonymous namespace: the export node must have a normal, always-emitted definition.
struct CAddonProtectedCallScript
{
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

void CAddonProtectedCallScript::script_register(lua_State* L)
{
    // _gw_internal is shared with addon_event_bus.cpp, addon_storage.cpp, gw_condlist_native.cpp: the first one
    // creates it
    lua_getglobal(L, "_gw_internal");
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "_gw_internal");
    }
    SetInternalFunction(L, "protected_call", &LuaAddonProtectedCall, &LuaEntryTraceback);
    SetInternalFunction(L, "protected_call_quiet", &LuaAddonProtectedCallQuiet, &LuaEntryTraceback);
    SetInternalFunction(L, "protected_report", &LuaAddonProtectedReport, nullptr);
    lua_pop(L, 1);
    // A new Lua state: the callbacks of the old one are gone, their errors are logged in full again
    if (g_addon_protected_failures)
        g_addon_protected_failures->clear();
}
} // namespace gw::addons
