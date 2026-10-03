#include "StdAfx.h"

#include "addon_api_console.h"
#include "addon_host.h"
#include "npc_cpp_profile.h"

#include "xrEngine/XR_IOConsole.h"
#include "xrEngine/xr_ioc_cmd.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>

#ifdef TRACY_ENABLE
#include <tracy/TracyC.h>
#endif

namespace gw::addons::console
{
namespace
{
// Where the value of a variable came from when it was registered (plugin_cvar_list shows it).
enum class ECvarSource : u8
{
    Default, // the default of the plugin
    File,    // appdata/plugins.ltx
    UserLtx, // a line of user.ltx the console did not know at its start (the old place, moved to plugins.ltx)
};

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
    double min = 0.0, max = 0.0; // the range of the registration: cvar_set_* checks it as the console does
    ECvarSource source = ECvarSource::Default;
    int saved_int = 0; // the value in plugins.ltx after the last write: a difference means the file is stale
    float saved_float = 0.f;
};

// Stage of the -npc_cpp_profile report owned by a plugin.
struct ProfileStage
{
    xr_string name;
    std::atomic_ullong ticks{ 0 };
    std::atomic_ullong calls{ 0 };
};

// Pointers stay valid while the entry lives: the console holds a pointer into PluginCvar. The vector holds
// pointers, so growing it moves nothing.
xr_vector<PluginCvar*>* g_cvars = nullptr;
xr_vector<xr_string>* g_deferred = nullptr;

// Stages, as the zone sites below: profile_add runs on any thread and works with an index, so the entries never
// move (a fixed array, allocated once, never freed: a worker of the plugin may still add to a stage at the exit).
// The main thread fills a stage first and then raises the count (release); profile_add reads the count (acquire)
// and touches only the stages below it. A stage keeps its index for the run: a plugin loaded anew takes it again.
constexpr u32 kMaxProfileStages = 1024;
ProfileStage* g_stages = nullptr;
std::atomic<u32> g_stage_count{ 0 };

constexpr size_t kMaxNameLength = 128;
constexpr size_t kMaxCommandLength = 512;

// appdata/plugins.ltx: the values of the variables of every plugin, one "name value" line each. user.ltx cannot
// hold them: it runs before the plugins are loaded and is written after they are unloaded (plan 12).
constexpr pcstr kCvarsFile = "plugins.ltx";
// Lines of the file without a registered variable: the addon is gone, did not load, or registers the variable
// later. They are written back as they are; a registration takes its line out.
xr_map<xr_string, xr_string>* g_stored = nullptr;
bool g_cvars_loaded = false; // the file was read (or is missing): nothing is written before, not to lose it
bool g_cvars_dirty = false;  // a variable appeared, went away with a new value, or got a default for a bad one
bool g_write_failed = false; // the failure to write is reported once, until a write succeeds
u32 g_retry_at_ms = 0;       // after a failed write (not read-only): the next try, once a second at most
constexpr size_t kMaxLineLength = 512; // a line of plugins.ltx: "name value", both short

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
// appdata/plugins.ltx
// ---------------------------------------------------------------------------------------------

void CvarsFilePath(string_path& path) { FS.update_path(path, "$app_data_root$", kCvarsFile); }

bool IsBlank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// The rule of the console for the names it keeps from a config (is_valid_pending_name, XR_IOConsole.cpp).
bool ValidCvarName(const xr_string& name)
{
    if (name.empty() || name.size() >= kMaxNameLength)
        return false;
    const auto first = static_cast<unsigned char>(name[0]);
    if (!(std::isalpha(first) || first == '_'))
        return false;
    for (const char c : name)
    {
        const auto ch = static_cast<unsigned char>(c);
        if (!(std::isalnum(ch) || ch == '_' || ch == '.'))
            return false;
    }
    return true;
}

// "name value": one name and one value token. False for anything else; `blank` is set for an empty line.
bool ParseCvarLine(pcstr line, xr_string& name, xr_string& value, bool& blank)
{
    pcstr begin = line;
    if (static_cast<unsigned char>(begin[0]) == 0xEF && static_cast<unsigned char>(begin[1]) == 0xBB &&
        static_cast<unsigned char>(begin[2]) == 0xBF)
    {
        begin += 3; // a BOM left by an editor
    }
    while (*begin && IsBlank(*begin))
        ++begin;
    pcstr end = begin + xr_strlen(begin);
    while (end > begin && IsBlank(end[-1]))
        --end;
    blank = begin == end;
    if (blank)
        return false;
    pcstr name_end = begin;
    while (name_end < end && !IsBlank(*name_end))
        ++name_end;
    pcstr value_begin = name_end;
    while (value_begin < end && IsBlank(*value_begin))
        ++value_begin;
    for (pcstr p = value_begin; p < end; ++p)
    {
        if (IsBlank(*p))
            return false; // more than one token
    }
    name.assign(begin, name_end);
    value.assign(value_begin, end);
    return !value.empty() && ValidCvarName(name);
}

bool ParseInt(pcstr text, int& out)
{
    char* end = nullptr;
    errno = 0;
    const long long value = std::strtoll(text, &end, 10);
    if (end == text || *end || errno == ERANGE || value < INT_MIN || value > INT_MAX)
        return false;
    out = static_cast<int>(value);
    return true;
}

bool ParseFloat(pcstr text, float& out)
{
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end || !std::isfinite(value))
        return false;
    out = static_cast<float>(value);
    return true;
}

// The same range checks as CCC_Integer and CCC_Float, so a saved value is what the console would accept.
bool ApplyCvarText(PluginCvar& cvar, pcstr text, double min, double max)
{
    if (cvar.is_float)
    {
        float value = 0.f;
        if (!ParseFloat(text, value) || value < static_cast<float>(min) - EPS || value > static_cast<float>(max) + EPS)
            return false;
        cvar.float_value = value;
        return true;
    }
    int value = 0;
    if (!ParseInt(text, value) || value < min || value > max)
        return false;
    cvar.int_value = value;
    return true;
}

// The value a new variable starts with: its line of plugins.ltx, else the line of user.ltx the console kept
// (taken, so the next cfg_save drops it from user.ltx), else the default.
void ApplySavedValue(PluginCvar& cvar, double min, double max)
{
    xr_string text;
    ECvarSource source = ECvarSource::Default;
    if (g_stored)
    {
        const auto it = g_stored->find(cvar.name);
        if (it != g_stored->end())
        {
            text = it->second;
            g_stored->erase(it);
            source = ECvarSource::File;
        }
    }
    shared_str pending;
    // Taken out of user.ltx only when plugins.ltx is readable: it must land there at the next write
    if (source == ECvarSource::Default && Console && g_cvars_loaded &&
        Console->ConsumePendingConfigCommandValue(cvar.name.c_str(), pending) && pending.size())
    {
        text = pending.c_str();
        while (!text.empty() && IsBlank(text.back()))
            text.pop_back();
        source = ECvarSource::UserLtx;
    }
    if (source == ECvarSource::Default)
        return;
    if (!ApplyCvarText(cvar, text.c_str(), min, max))
    {
        Msg("! [console] %s: the saved value '%s' of '%s' is not %s in [%g, %g], the default is used",
            source == ECvarSource::File ? kCvarsFile : "user.ltx", text.c_str(), cvar.name.c_str(),
            cvar.is_float ? "a number" : "an integer", min, max);
        return;
    }
    cvar.source = source;
}

// The value as the console prints it (and reads it back): the text of the line in plugins.ltx.
void CvarStatus(const PluginCvar& cvar, IConsole_Command::TStatus& status)
{
    status[0] = 0;
    cvar.command->GetStatus(status);
}

bool CvarChanged(const PluginCvar& cvar)
{
    // Bitwise for the float: NaN != NaN would mean "changed" on every frame
    return cvar.is_float ? std::memcmp(&cvar.float_value, &cvar.saved_float, sizeof(float)) != 0 :
                           cvar.int_value != cvar.saved_int;
}

void SnapshotCvars()
{
    if (!g_cvars)
        return;
    for (PluginCvar* cvar : *g_cvars)
    {
        cvar->saved_int = cvar->int_value;
        cvar->saved_float = cvar->float_value;
    }
}

bool CvarsFileStale()
{
    if (g_write_failed && g_retry_at_ms && Device.dwTimeGlobal < g_retry_at_ms)
        return false; // a write failed a moment ago: not every frame
    if (g_cvars_dirty)
        return true;
    if (g_cvars)
    {
        for (const PluginCvar* cvar : *g_cvars)
        {
            if (CvarChanged(*cvar))
                return true;
        }
    }
    return false;
}

void SaveCvarsFile()
{
    // Without the read the lines of addons that are not loaded now would be lost
    if (!g_cvars_loaded)
        return;
    xr_map<xr_string, xr_string> lines; // sorted by name
    if (g_stored)
        lines = *g_stored;
    if (g_cvars)
    {
        for (const PluginCvar* cvar : *g_cvars)
        {
            IConsole_Command::TStatus status;
            CvarStatus(*cvar, status);
            if (status[0])
                lines[cvar->name] = status;
        }
    }

    string_path path;
    CvarsFilePath(path);
#if defined(XR_PLATFORM_WINDOWS)
    // A read-only file is the wish of the player to keep it: it is left alone (cfg_save would clear the flag)
    const DWORD attributes = GetFileAttributes(path);
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY))
    {
        if (!g_write_failed)
            Msg("~ [console] '%s' is read-only: the values of plugin variables are not saved", path);
        // Nothing is retried until the next change: the file stays as the player left it
        g_write_failed = true;
        g_retry_at_ms = 0;
        SnapshotCvars();
        g_cvars_dirty = false;
        return;
    }
#endif
    IWriter* writer = FS.w_open(path);
    if (!writer || !writer->valid())
    {
        if (writer)
            FS.w_close(writer);
        if (!g_write_failed)
            Msg("! [console] cannot write '%s': the values of plugin variables are not saved", path);
        // The file may be busy (a launcher, an antivirus): tried again a second later, and at the shutdown
        g_write_failed = true;
        g_retry_at_ms = Device.dwTimeGlobal + 1000;
        g_cvars_dirty = true;
        return;
    }
    for (const auto& [name, value] : lines)
        writer->w_printf("%s %s\r\n", name.c_str(), value.c_str());
    FS.w_close(writer);
    g_write_failed = false;
    g_retry_at_ms = 0;
    SnapshotCvars();
    g_cvars_dirty = false;
    if (IsDebugLog())
        Msg("  [console] '%s' saved: %u line(s)", path, static_cast<u32>(lines.size()));
}

// Before a variable goes away: its value stays in the file.
void KeepCvarValue(const PluginCvar& cvar)
{
    IConsole_Command::TStatus status;
    CvarStatus(cvar, status);
    if (!status[0])
        return;
    if (!g_stored)
        g_stored = xr_new<xr_map<xr_string, xr_string>>();
    (*g_stored)[cvar.name] = status;
    if (CvarChanged(cvar))
        g_cvars_dirty = true;
}

pcstr SourceName(ECvarSource source)
{
    switch (source)
    {
    case ECvarSource::File: return kCvarsFile;
    case ECvarSource::UserLtx: return "user.ltx";
    default: return "default";
    }
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

// A number of the console: CCC_Integer, CCC_Mask (0/1) or CCC_Float. CConsole::GetInteger/GetFloat return 0 for
// a command of another class, so the class is checked here: a number of the other kind is converted, anything
// else (a token, a string, an action) gives the default of the caller.
bool CvarNumber(IConsole_Command* command, double& out)
{
    if (const auto* integer = dynamic_cast<CCC_Integer*>(command))
    {
        out = integer->GetValue();
        return true;
    }
    if (const auto* mask = dynamic_cast<CCC_Mask*>(command))
    {
        out = mask->GetValue() ? 1.0 : 0.0;
        return true;
    }
    if (const auto* real = dynamic_cast<CCC_Float*>(command))
    {
        out = real->GetValue();
        return true;
    }
    return false;
}

int GWP_CALL ApiCvarGetInt(const char* name, int def)
{
    if (!CheckCall("cvar_get_int") || !name || !Console)
        return def;
    double value = 0.0;
    if (!CvarNumber(Console->GetCommand(name), value))
        return def;
    // A float is truncated toward zero, as a C cast; one that does not fit an int (or NaN) gives the default
    if (!(value > static_cast<double>(INT_MIN) - 1.0 && value < static_cast<double>(INT_MAX) + 1.0))
        return def;
    return static_cast<int>(value);
}

double GWP_CALL ApiCvarGetFloat(const char* name, double def)
{
    if (!CheckCall("cvar_get_float") || !name || !Console)
        return def;
    double value = 0.0;
    return CvarNumber(Console->GetCommand(name), value) ? value : def;
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
    if (const PluginCvar* existing = FindCvar(name))
    {
        // The prefix alone does not tell the owner: addon "gw" passes the check for "gw_npc_*" of addon "gw_npc"
        if (existing->owner != self)
        {
            Msg("! [plugin:%s] %s('%s'): the variable belongs to addon '%s'", PluginAddonId(self), function, name,
                PluginAddonId(existing->owner));
            return GWP_ERROR_ACCESS_DENIED;
        }
        if (existing->is_float != is_float)
        {
            Msg("! [plugin:%s] %s('%s'): the variable is registered as %s already", PluginAddonId(self), function,
                name, existing->is_float ? "a float" : "an integer");
            return GWP_ERROR_INVALID_ARGUMENT;
        }
        return GWP_OK; // registered again after a reload of the plugin: the value of the game is kept
    }
    if (!Console)
        return GWP_ERROR;
    if (Console->GetCommand(name))
    {
        // An addon id that is a prefix of the engine (hud, ai, r2, ...): the command of the engine must stay
        Msg("! [plugin:%s] %s('%s'): the console has this command already", PluginAddonId(self), function, name);
        return GWP_ERROR_INVALID_ARGUMENT;
    }

    if (!g_cvars)
        g_cvars = xr_new<xr_vector<PluginCvar*>>();
    PluginCvar* cvar = xr_new<PluginCvar>();
    cvar->name = name;
    cvar->owner = self;
    cvar->is_float = is_float;
    cvar->min = min;
    cvar->max = max;
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
    ApplySavedValue(*cvar, min, max);
    cvar->saved_int = cvar->int_value;
    cvar->saved_float = cvar->float_value;
    if (cvar->source != ECvarSource::File)
        g_cvars_dirty = true; // a new line for the file, or the default instead of a bad value
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

// cvar_set_*: a variable this plugin registered, or nullptr with the reason in `result`. Only variables of
// plugins can be set at all; the engine ones are not in g_cvars.
PluginCvar* OwnCvar(const GwpPlugin* self, const char* name, pcstr function, GwpResult& result)
{
    result = CheckOwnName(self, name, function);
    if (result != GWP_OK)
        return nullptr;
    PluginCvar* cvar = FindCvar(name);
    if (!cvar)
    {
        result = GWP_ERROR;
        return nullptr;
    }
    // The prefix alone does not tell the owner: addon "gw" passes the check for "gw_npc_*" of addon "gw_npc"
    if (cvar->owner != self)
    {
        Msg("! [plugin:%s] %s('%s'): the variable belongs to addon '%s'", PluginAddonId(self), function, name,
            PluginAddonId(cvar->owner));
        result = GWP_ERROR_ACCESS_DENIED;
        return nullptr;
    }
    return cvar;
}

GwpResult GWP_CALL ApiCvarSetInt(const GwpPlugin* self, const char* name, int value)
{
    GwpResult result = GWP_OK;
    PluginCvar* cvar = OwnCvar(self, name, "cvar_set_int", result);
    if (!cvar)
        return result;
    if (cvar->is_float)
        return GWP_ERROR;
    if (value < cvar->min || value > cvar->max)
        return GWP_ERROR_INVALID_ARGUMENT; // the console refuses it too; the file would drop it at the next start
    cvar->int_value = value;
    return GWP_OK;
}

GwpResult GWP_CALL ApiCvarSetFloat(const GwpPlugin* self, const char* name, double value)
{
    GwpResult result = GWP_OK;
    PluginCvar* cvar = OwnCvar(self, name, "cvar_set_float", result);
    if (!cvar)
        return result;
    if (!cvar->is_float)
        return GWP_ERROR;
    if (!std::isfinite(value) || value < cvar->min - EPS || value > cvar->max + EPS)
        return GWP_ERROR_INVALID_ARGUMENT; // as CCC_Float::Execute; NaN never
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

    string256 full;
    xr_sprintf(full, "%s.%s", PluginAddonId(self), name);
    const u32 count = g_stage_count.load(std::memory_order_relaxed); // only the main thread writes it
    for (u32 i = 0; i < count; ++i)
    {
        if (g_stages[i].name == full)
            return static_cast<uint32_t>(i + 1);
    }
    if (count >= kMaxProfileStages)
    {
        Msg("! [console] profile_stage: more than %u profiling stages, '%s' is not registered", kMaxProfileStages,
            full);
        return 0;
    }
    if (!g_stages)
        g_stages = new ProfileStage[kMaxProfileStages]; // never freed, see g_stages
    ProfileStage& stage = g_stages[count];
    stage.name = full;
    stage.ticks.store(0, std::memory_order_relaxed);
    stage.calls.store(0, std::memory_order_relaxed);
    g_stage_count.store(count + 1, std::memory_order_release);
    return static_cast<uint32_t>(count + 1); // index + 1
}

void GWP_CALL ApiProfileAdd(uint32_t stage, uint64_t ticks)
{
    // The stages only grow and are never freed; a stage handed out is writable from any thread.
    if (stage == 0 || stage > g_stage_count.load(std::memory_order_acquire))
        return;
    ProfileStage& entry = g_stages[stage - 1];
    entry.ticks.fetch_add(ticks, std::memory_order_relaxed);
    entry.calls.fetch_add(1, std::memory_order_relaxed);
}

uint64_t GWP_CALL ApiProfileTicks() { return CPU::QPC(); }

// ---------------------------------------------------------------------------------------------
// Tracy zones of the plugins
// ---------------------------------------------------------------------------------------------

#ifdef TRACY_ENABLE
// A registered place. Tracy keeps the pointer to `data` and reads the strings whenever the profiler asks, also
// after the plugin is gone: the sites live until the process ends (a fixed array: the addresses never move).
struct ZoneSite
{
    ___tracy_source_location_data data{};
    xr_string name;
    xr_string function;
    xr_string file;
};
constexpr u32 kMaxZoneSites = 4096; // a fixed set of places per plugin; more means one registration per call
ZoneSite* g_zone_sites = nullptr;    // kMaxZoneSites entries, allocated once, never freed (see above)
// Sites published so far. The main thread fills a site first and then raises the count (release); a worker
// reads the count (acquire) and only the sites below it, so it never sees a half-written one
std::atomic<u32> g_zone_site_count{ 0 };

GwpZoneSite GWP_CALL ApiProfileZoneSite(const GwpPlugin* self, const char* name, const char* file, uint32_t line,
    uint32_t color)
{
    if (!IsMainThread() || !self || !name || !name[0])
        return GWP_INVALID_ZONE_SITE;
    const u32 count = g_zone_site_count.load(std::memory_order_relaxed); // only the main thread writes it
    // The same place again (a plugin loaded anew: plugin_reload, a restart after a crash) takes its old site: the
    // sites are never freed, so every reload would use up new ones
    const pcstr addon_id = PluginAddonId(self);
    const pcstr file_name = file ? file : "";
    for (u32 i = 0; i < count; ++i)
    {
        const ZoneSite& known = g_zone_sites[i];
        if (known.data.line == line && known.data.color == color && known.name == name &&
            known.function == addon_id && known.file == file_name)
        {
            return static_cast<GwpZoneSite>(i + 1);
        }
    }
    if (count >= kMaxZoneSites)
    {
        Msg("! [console] profile_zone_site: more than %u zone sites, '%s' of addon '%s' is not registered",
            kMaxZoneSites, name, PluginAddonId(self));
        return GWP_INVALID_ZONE_SITE;
    }
    if (!g_zone_sites)
        g_zone_sites = new ZoneSite[kMaxZoneSites]; // never freed, see ZoneSite
    ZoneSite& site = g_zone_sites[count];
    site.name = name;
    site.function = PluginAddonId(self);
    site.file = file ? file : "";
    site.data.name = site.name.c_str();
    site.data.function = site.function.c_str();
    site.data.file = site.file.c_str();
    site.data.line = line;
    site.data.color = color;
    g_zone_site_count.store(count + 1, std::memory_order_release);
    return static_cast<GwpZoneSite>(count + 1); // index + 1
}

GwpZone GWP_CALL ApiProfileZoneBegin(GwpZoneSite site)
{
    // The sites only grow and are never freed; a site handed out is readable from any thread.
    if (site == GWP_INVALID_ZONE_SITE || site > g_zone_site_count.load(std::memory_order_acquire))
        return 0;
    const TracyCZoneCtx ctx = ___tracy_emit_zone_begin(&g_zone_sites[site - 1].data, 1);
    return ctx.active ? (static_cast<GwpZone>(ctx.id) << 1) | 1u : 0;
}

TracyCZoneCtx ContextOf(GwpZone zone)
{
    TracyCZoneCtx ctx;
    ctx.id = static_cast<uint32_t>(zone >> 1);
    ctx.active = static_cast<int>(zone & 1u);
    return ctx;
}

void GWP_CALL ApiProfileZoneText(GwpZone zone, const char* text, uint32_t length)
{
    // Tracy keeps the length in 16 bits: a longer text is cut, not wrapped
    if (zone && text)
        ___tracy_emit_zone_text(ContextOf(zone), text, length < 65535u ? length : 65534u);
}

void GWP_CALL ApiProfileZoneEnd(GwpZone zone)
{
    if (zone)
        ___tracy_emit_zone_end(ContextOf(zone));
}
#else
GwpZoneSite GWP_CALL ApiProfileZoneSite(const GwpPlugin*, const char*, const char*, uint32_t, uint32_t)
{
    return GWP_INVALID_ZONE_SITE;
}
GwpZone GWP_CALL ApiProfileZoneBegin(GwpZoneSite) { return 0; }
void GWP_CALL ApiProfileZoneText(GwpZone, const char*, uint32_t) {}
void GWP_CALL ApiProfileZoneEnd(GwpZone) {}
#endif
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
    api.profile_zone_site = &ApiProfileZoneSite;
    api.profile_zone_begin = &ApiProfileZoneBegin;
    api.profile_zone_text = &ApiProfileZoneText;
    api.profile_zone_end = &ApiProfileZoneEnd;
}

void OnFrame()
{
    if (g_deferred && !g_deferred->empty() && Console)
    {
        // A command may destroy the level and everything in it, and a handler of it may queue another command:
        // take the queue away first, then run it.
        xr_vector<xr_string> commands;
        commands.swap(*g_deferred);
        for (const xr_string& command : commands)
            Console->Execute(command.c_str());
    }
    // Once a frame at most, and only after a change: a value survives a crash of the game
    if (CvarsFileStale())
        SaveCvarsFile();
}

void LoadCvarsFile()
{
    if (g_cvars_loaded)
        return;
    g_cvars_loaded = true;
    if (!g_stored)
        g_stored = xr_new<xr_map<xr_string, xr_string>>();
    string_path path;
    CvarsFilePath(path);
    if (!FS.exist(path))
    {
        if (IsDebugLog())
            Msg("  [console] '%s' is not there yet: plugin variables start with their defaults", path);
        return;
    }
    IReader* reader = FS.r_open(path);
    if (!reader)
    {
        Msg("! [console] cannot open '%s': plugin variables start with their defaults", path);
        g_cvars_loaded = false; // not written over: the values of the player stay in the file
        return;
    }
    xr_string line;
    u32 number = 0;
    while (!reader->eof())
    {
        reader->r_string(line); // the fixed-buffer overload asserts on a long line
        ++number;
        if (line.size() > kMaxLineLength)
        {
            Msg("! [console] %s:%u: a line of %u bytes, dropped", kCvarsFile, number, static_cast<u32>(line.size()));
            continue;
        }
        xr_string name, value;
        bool blank = false;
        if (ParseCvarLine(line.c_str(), name, value, blank))
            (*g_stored)[name] = value;
        else if (!blank)
            Msg("! [console] %s:%u: not a 'name value' line, dropped: '%s'", kCvarsFile, number, line.c_str());
    }
    FS.r_close(reader);
    if (IsDebugLog())
        Msg("  [console] '%s' read: %u value(s)", path, static_cast<u32>(g_stored->size()));
}

void PrintCvarList()
{
    string_path path;
    CvarsFilePath(path);
    Msg("- plugin console variables, file '%s':", path);
    u32 count = 0;
    if (g_cvars)
    {
        for (const PluginCvar* cvar : *g_cvars)
        {
            IConsole_Command::TStatus status;
            CvarStatus(*cvar, status);
            Msg("  %s %s  [addon %s, from %s]", cvar->name.c_str(), status, PluginAddonId(cvar->owner),
                SourceName(cvar->source));
            ++count;
        }
    }
    const u32 stored = g_stored ? static_cast<u32>(g_stored->size()) : 0;
    if (stored)
    {
        Msg("- lines of the file without a variable (kept as they are):");
        for (const auto& [name, value] : *g_stored)
            Msg("  %s %s", name.c_str(), value.c_str());
    }
    Msg("- %u variable(s), %u line(s) without a variable%s", count, stored,
        g_write_failed ? ", the last write failed" : "");
}

void FlushProfile()
{
    const u32 count = g_stage_count.load(std::memory_order_relaxed); // the main thread, the only writer
    for (u32 i = 0; i < count; ++i)
    {
        ProfileStage& stage = g_stages[i];
        const u64 ticks = stage.ticks.exchange(0, std::memory_order_relaxed);
        const u64 calls = stage.calls.exchange(0, std::memory_order_relaxed);
        if (!calls || !ticks)
            continue;
        const double total_ms = 1000.0 * static_cast<double>(ticks) / CPU::qpc_freq;
        const double avg_us = 1000000.0 * static_cast<double>(ticks) / (CPU::qpc_freq * calls);
        Msg("*   plugin:%s total=%.2fms calls=%llu avg=%.2fus", stage.name.c_str(), total_ms,
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
        KeepCvarValue(*cvar);
        if (Console && cvar->command)
            Console->RemoveCommand(cvar->command);
        xr_delete(cvar->command);
        xr_delete(cvar);
        it = g_cvars->erase(it);
    }
}

void Shutdown()
{
    // Usually every plugin has already left (RemovePluginCvars keeps its values); the rest is kept here
    if (g_cvars)
    {
        for (PluginCvar* cvar : *g_cvars)
            KeepCvarValue(*cvar);
    }
    g_retry_at_ms = 0; // the last chance: no throttle
    if (CvarsFileStale())
        SaveCvarsFile();
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
    xr_delete(g_stored);
    g_cvars_loaded = false;
    g_cvars_dirty = false;
    g_write_failed = false;
    g_retry_at_ms = 0;
    // The profiling stages stay: profile_add may still run on a thread of the plugin (see g_stages)
    xr_delete(g_deferred);
}
} // namespace gw::addons::console
