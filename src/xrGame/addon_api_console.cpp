#include "StdAfx.h"

#include "addon_api_console.h"
#include "addon_host.h"
#include "npc_cpp_profile.h"

#include "xrEngine/XR_IOConsole.h"
#include "xrEngine/xr_ioc_cmd.h"

#include <algorithm>

namespace gw::addons::console
{
namespace
{
// A console variable of a plugin. The value lives here, in the host: CCC_Integer keeps a pointer to it, and a
// plugin library can be unloaded while the console command is still registered.
struct PluginCvar
{
    xr_string name; // the command is registered with name.c_str(): the string must outlive the command
    const GwpPlugin* owner = nullptr;
    bool is_float = false;
    int int_value = 0;
    float float_value = 0.f;
    IConsole_Command* command = nullptr;
};

// Stage of the -npc_cpp_profile report owned by a plugin.
struct ProfileStage
{
    xr_string name;
    std::atomic_ullong ticks{ 0 };
    std::atomic_ullong calls{ 0 };
};

// Pointers stay valid while the entry lives: the console holds a pointer into PluginCvar, and profile_add
// works with an index. Both vectors hold pointers, so growing them moves nothing.
xr_vector<PluginCvar*>* g_cvars = nullptr;
xr_vector<ProfileStage*>* g_stages = nullptr;
xr_vector<xr_string>* g_deferred = nullptr;

constexpr size_t kMaxNameLength = 128;
constexpr size_t kMaxCommandLength = 512;

PluginCvar* FindCvar(const char* name)
{
    if (!g_cvars || !name)
        return nullptr;
    for (PluginCvar* cvar : *g_cvars)
    {
        if (cvar->name == name)
            return cvar;
    }
    return nullptr;
}

bool CheckCall(pcstr function)
{
    if (IsMainThread())
        return true;
    Msg("! [console] %s called outside the main thread, ignored", function);
    return false;
}

// A plugin owns the names that start with "<addon id>_": its own variables cannot collide with the engine
// or with another addon, and the host knows what to remove when the addon goes away.
GwpResult CheckOwnName(const GwpPlugin* self, const char* name, pcstr function)
{
    if (!CheckCall(function))
        return GWP_ERROR_NOT_MAIN_THREAD;
    if (!self || !name || !*name || xr_strlen(name) >= kMaxNameLength)
        return GWP_ERROR_INVALID_ARGUMENT;
    const pcstr id = PluginAddonId(self);
    const size_t id_length = xr_strlen(id);
    if (strncmp(name, id, id_length) != 0 || name[id_length] != '_')
    {
        Msg("! [plugin:%s] %s('%s'): the name of a console variable must start with '%s_'", id, function, name, id);
        return GWP_ERROR_ACCESS_DENIED;
    }
    return GWP_OK;
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group console)
// ---------------------------------------------------------------------------------------------

int GWP_CALL ApiCvarExists(const char* name)
{
    if (!CheckCall("cvar_exists") || !name || !Console)
        return 0;
    return Console->GetCommand(name) != nullptr ? 1 : 0;
}

int GWP_CALL ApiCvarGetInt(const char* name, int def)
{
    if (!CheckCall("cvar_get_int") || !name || !Console || !Console->GetCommand(name))
        return def;
    int min = 0, max = 0;
    return Console->GetInteger(name, min, max);
}

double GWP_CALL ApiCvarGetFloat(const char* name, double def)
{
    if (!CheckCall("cvar_get_float") || !name || !Console || !Console->GetCommand(name))
        return def;
    float min = 0.f, max = 0.f;
    return Console->GetFloat(name, min, max);
}

uint32_t GWP_CALL ApiCvarGetString(const char* name, char* out, uint32_t size)
{
    if (out && size)
        out[0] = 0;
    if (!CheckCall("cvar_get_string") || !name || !Console)
        return 0;
    IConsole_Command* command = Console->GetCommand(name);
    if (!command)
        return 0;
    // Through a local buffer on purpose: CConsole::GetString hands out a pointer to a function-local static.
    IConsole_Command::TStatus status;
    status[0] = 0;
    command->GetStatus(status);
    const uint32_t length = static_cast<uint32_t>(xr_strlen(status));
    if (out && size)
    {
        const uint32_t copied = std::min(length, size - 1);
        std::memcpy(out, status, copied);
        out[copied] = 0;
    }
    return length;
}

GwpResult RegisterCvar(const GwpPlugin* self, const char* name, pcstr function, bool is_float, double def,
    double min, double max)
{
    const GwpResult check = CheckOwnName(self, name, function);
    if (check != GWP_OK)
        return check;
    if (!(min <= max) || !(def >= min && def <= max)) // also false for NaN
        return GWP_ERROR_INVALID_ARGUMENT;
    if (FindCvar(name))
        return GWP_OK; // registered again after a reload of the plugin: the value of the game is kept
    if (!Console)
        return GWP_ERROR;

    if (!g_cvars)
        g_cvars = xr_new<xr_vector<PluginCvar*>>();
    PluginCvar* cvar = xr_new<PluginCvar>();
    cvar->name = name;
    cvar->owner = self;
    cvar->is_float = is_float;
    if (is_float)
    {
        cvar->float_value = static_cast<float>(def);
        cvar->command = xr_new<CCC_Float>(cvar->name.c_str(), &cvar->float_value, static_cast<float>(min),
            static_cast<float>(max));
    }
    else
    {
        cvar->int_value = static_cast<int>(def);
        cvar->command = xr_new<CCC_Integer>(cvar->name.c_str(), &cvar->int_value, static_cast<int>(min),
            static_cast<int>(max));
    }
    g_cvars->push_back(cvar);
    Console->AddCommand(cvar->command);
    return GWP_OK;
}

GwpResult GWP_CALL ApiCvarRegisterInt(const GwpPlugin* self, const char* name, int def, int min, int max)
{
    return RegisterCvar(self, name, "cvar_register_int", false, def, min, max);
}

GwpResult GWP_CALL ApiCvarRegisterFloat(const GwpPlugin* self, const char* name, double def, double min, double max)
{
    return RegisterCvar(self, name, "cvar_register_float", true, def, min, max);
}

GwpResult GWP_CALL ApiCvarSetInt(const GwpPlugin* self, const char* name, int value)
{
    const GwpResult check = CheckOwnName(self, name, "cvar_set_int");
    if (check != GWP_OK)
        return check;
    PluginCvar* cvar = FindCvar(name);
    if (!cvar || cvar->is_float)
        return GWP_ERROR;
    cvar->int_value = value;
    return GWP_OK;
}

GwpResult GWP_CALL ApiCvarSetFloat(const GwpPlugin* self, const char* name, double value)
{
    const GwpResult check = CheckOwnName(self, name, "cvar_set_float");
    if (check != GWP_OK)
        return check;
    PluginCvar* cvar = FindCvar(name);
    if (!cvar || !cvar->is_float)
        return GWP_ERROR;
    cvar->float_value = static_cast<float>(value);
    return GWP_OK;
}

GwpResult GWP_CALL ApiConsoleExecute(const GwpPlugin* self, const char* command)
{
    if (!CheckCall("console_execute"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    if (!self || !command || !*command || xr_strlen(command) >= kMaxCommandLength)
        return GWP_ERROR_INVALID_ARGUMENT;
    if (!Console)
        return GWP_ERROR;
    if (IsDebugLog())
        Msg("  [plugin:%s] console: %s", PluginAddonId(self), command);
    Console->Execute(command);
    return GWP_OK;
}

GwpResult GWP_CALL ApiConsoleExecuteDeferred(const GwpPlugin* self, const char* command)
{
    if (!CheckCall("console_execute_deferred"))
        return GWP_ERROR_NOT_MAIN_THREAD;
    if (!self || !command || !*command || xr_strlen(command) >= kMaxCommandLength)
        return GWP_ERROR_INVALID_ARGUMENT;
    if (!g_deferred)
        g_deferred = xr_new<xr_vector<xr_string>>();
    g_deferred->emplace_back(command);
    return GWP_OK;
}

uint32_t GWP_CALL ApiProfileStage(const GwpPlugin* self, const char* name)
{
    if (!CheckCall("profile_stage") || !self || !name || !*name || xr_strlen(name) >= kMaxNameLength)
        return 0;
    if (!npc_cpp_profile::enabled())
        return 0; // profiling is off: the plugin can skip its measurements

    if (!g_stages)
        g_stages = xr_new<xr_vector<ProfileStage*>>();
    string256 full;
    xr_sprintf(full, "%s.%s", PluginAddonId(self), name);
    for (size_t i = 0; i < g_stages->size(); ++i)
    {
        if ((*g_stages)[i]->name == full)
            return static_cast<uint32_t>(i + 1);
    }
    ProfileStage* stage = xr_new<ProfileStage>();
    stage->name = full;
    g_stages->push_back(stage);
    return static_cast<uint32_t>(g_stages->size());
}

void GWP_CALL ApiProfileAdd(uint32_t stage, uint64_t ticks)
{
    if (!g_stages || stage == 0 || stage > g_stages->size())
        return;
    ProfileStage& entry = *(*g_stages)[stage - 1];
    entry.ticks.fetch_add(ticks, std::memory_order_relaxed);
    entry.calls.fetch_add(1, std::memory_order_relaxed);
}

uint64_t GWP_CALL ApiProfileTicks() { return CPU::QPC(); }
} // namespace

void FillEngineApi(GwpEngineApi& api)
{
    api.cvar_exists = &ApiCvarExists;
    api.cvar_get_int = &ApiCvarGetInt;
    api.cvar_get_float = &ApiCvarGetFloat;
    api.cvar_get_string = &ApiCvarGetString;
    api.cvar_register_int = &ApiCvarRegisterInt;
    api.cvar_register_float = &ApiCvarRegisterFloat;
    api.cvar_set_int = &ApiCvarSetInt;
    api.cvar_set_float = &ApiCvarSetFloat;
    api.console_execute = &ApiConsoleExecute;
    api.console_execute_deferred = &ApiConsoleExecuteDeferred;
    api.profile_stage = &ApiProfileStage;
    api.profile_add = &ApiProfileAdd;
    api.profile_ticks = &ApiProfileTicks;
}

void OnFrame()
{
    if (!g_deferred || g_deferred->empty() || !Console)
        return;
    // A command may destroy the level and everything in it, and a handler of it may queue another command:
    // take the queue away first, then run it.
    xr_vector<xr_string> commands;
    commands.swap(*g_deferred);
    for (const xr_string& command : commands)
        Console->Execute(command.c_str());
}

void FlushProfile()
{
    if (!g_stages)
        return;
    for (ProfileStage* stage : *g_stages)
    {
        const u64 ticks = stage->ticks.exchange(0, std::memory_order_relaxed);
        const u64 calls = stage->calls.exchange(0, std::memory_order_relaxed);
        if (!calls || !ticks)
            continue;
        const double total_ms = 1000.0 * static_cast<double>(ticks) / CPU::qpc_freq;
        const double avg_us = 1000000.0 * static_cast<double>(ticks) / (CPU::qpc_freq * calls);
        Msg("*   plugin:%s total=%.2fms calls=%llu avg=%.2fus", stage->name.c_str(), total_ms,
            static_cast<unsigned long long>(calls), avg_us);
    }
}

void RemovePluginCvars(const GwpPlugin* plugin)
{
    if (!g_cvars)
        return;
    for (auto it = g_cvars->begin(); it != g_cvars->end();)
    {
        PluginCvar* cvar = *it;
        if (cvar->owner != plugin)
        {
            ++it;
            continue;
        }
        if (Console && cvar->command)
            Console->RemoveCommand(cvar->command);
        xr_delete(cvar->command);
        xr_delete(cvar);
        it = g_cvars->erase(it);
    }
}

void Shutdown()
{
    if (g_cvars)
    {
        for (PluginCvar* cvar : *g_cvars)
        {
            if (Console && cvar->command)
                Console->RemoveCommand(cvar->command);
            xr_delete(cvar->command);
            xr_delete(cvar);
        }
    }
    xr_delete(g_cvars);
    if (g_stages)
    {
        for (ProfileStage* stage : *g_stages)
            xr_delete(stage);
    }
    xr_delete(g_stages);
    xr_delete(g_deferred);
}
} // namespace gw::addons::console
