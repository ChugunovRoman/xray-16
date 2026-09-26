#include "StdAfx.h"

#include "addon_host.h"

#include "xrAddonHost/include/gwp/gwp_api.h"
#include "xrCore/ModuleLookup.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>

namespace gw::addons::detail
{
struct AddonRecord;
}

// Plugin handle given to plugins (declared opaque in gwp_api.h). Global name on purpose: it completes the C type.
struct GwpPlugin
{
    gw::addons::detail::AddonRecord* addon = nullptr;
};

namespace gw::addons
{
namespace fs = std::filesystem;

namespace detail
{
enum class EAddonState
{
    Discovered, // manifest parsed, plugin not processed yet
    Active,     // addon works (with its plugin, or it has none / plugin unavailable and not required)
    Failed,     // addon cannot work (manifest error, required plugin failed)
    Skipped,    // duplicate id or disabled
};

bool EqualsNoCase(const xr_string& a, const xr_string& b);

// [addon] type, required. Behaviour specific to a type hooks on this value.
// List: plans/addons_api_support/00-roadmap.md, section 2.1; docs: wiki/doc/plugins/manifest.md
enum class EAddonType
{
    Unknown,
    Content,         // new items, weapons, NPCs, traders
    Graphics,        // textures, models, shaders
    Audio,           // sounds, music
    Script,          // gameplay script mods
    Ui,              // interface, HUD
    Localization,    // translations
    Library,         // library for other addons, not standalone
    Location,        // new levels (all.spawn, game.graph, global map)
    TotalConversion, // large conversion (reserved, v2+)
};

struct AddonTypeName
{
    EAddonType type;
    pcstr name;
};

constexpr AddonTypeName kAddonTypes[] = {
    { EAddonType::Content, "content" },
    { EAddonType::Graphics, "graphics" },
    { EAddonType::Audio, "audio" },
    { EAddonType::Script, "script" },
    { EAddonType::Ui, "ui" },
    { EAddonType::Localization, "localization" },
    { EAddonType::Library, "library" },
    { EAddonType::Location, "location" },
    { EAddonType::TotalConversion, "total_conversion" },
};

EAddonType ParseAddonType(const xr_string& text)
{
    for (const AddonTypeName& entry : kAddonTypes)
    {
        if (EqualsNoCase(text, entry.name))
            return entry.type;
    }
    return EAddonType::Unknown;
}

xr_string AddonTypeList()
{
    xr_string list;
    for (const AddonTypeName& entry : kAddonTypes)
    {
        if (!list.empty())
            list += ", ";
        list += entry.name;
    }
    return list;
}

enum class EPluginState
{
    None,        // addon declares no plugin
    Loaded,      // plugin loaded and initialized
    Unavailable, // plugin declared but not loaded (see reason)
};

struct AddonRecord
{
    xr_string id;
    xr_string version;
    xr_string type_name;
    EAddonType type = EAddonType::Unknown;
    fs::path dir_path;      // addon folder; use for every file system operation
    fs::path manifest_path; // dir_path / addon.ltx
    xr_string dir;          // dir_path as UTF-8 with trailing separator: what plugins get from addon_dir()
    xr_string dir_log;      // dir_path in the engine's narrow encoding: for log/console output only

    EAddonState state = EAddonState::Discovered;
    xr_string reason;

    // [plugin] section of addon.ltx
    xr_string plugin_name; // empty when the addon has no plugin
    u32 plugin_api_min = 0;
    bool plugin_required = false;

    EPluginState plugin_state = EPluginState::None;
    xr_string plugin_path; // original library path (narrow encoding, for messages)
    xr_string shadow_path; // loaded copy (narrow encoding, for messages)
    XRay::Module module;
    GwpPluginDesc desc{};
    GwpPlugin handle{};
};

// Heap-allocated on purpose: if Shutdown() is never called, a static vector would unload the plugin libraries
// from the xrGame DllMain(DLL_PROCESS_DETACH), which is not allowed. Leaking them instead is harmless at exit.
xr_vector<xr_unique_ptr<AddonRecord>>* g_addons = nullptr;
GwpEngineApi g_engine_api{};
std::thread::id g_main_thread_id;
bool g_initialized = false;
bool g_debug_log = false;

#if defined(XR_PLATFORM_WINDOWS)
constexpr pcstr kPluginOs = "windows";
constexpr pcstr kLibraryExt = ".dll";
#elif defined(XR_PLATFORM_APPLE)
constexpr pcstr kPluginOs = "macos";
constexpr pcstr kLibraryExt = ".dylib";
#elif defined(XR_PLATFORM_LINUX)
constexpr pcstr kPluginOs = "linux";
constexpr pcstr kLibraryExt = ".so";
#else
constexpr pcstr kPluginOs = "unknown";
constexpr pcstr kLibraryExt = ".so";
#endif

#if defined(XR_ARCHITECTURE_ARM64) || defined(_M_ARM64) || defined(__aarch64__)
constexpr pcstr kPluginArch = "arm64";
#elif defined(XR_ARCHITECTURE_X64) || defined(_M_X64) || defined(__x86_64__)
constexpr pcstr kPluginArch = "x64";
#else
constexpr pcstr kPluginArch = "x86";
#endif

constexpr pcstr kManifestName = "addon.ltx";

pcstr AddonStateName(EAddonState state)
{
    switch (state)
    {
    case EAddonState::Discovered: return "discovered";
    case EAddonState::Active: return "active";
    case EAddonState::Failed: return "failed";
    case EAddonState::Skipped: return "skipped";
    }
    return "?";
}

pcstr PluginStateName(EPluginState state)
{
    switch (state)
    {
    case EPluginState::None: return "none";
    case EPluginState::Loaded: return "loaded";
    case EPluginState::Unavailable: return "unavailable";
    }
    return "?";
}

// Encodings: std::filesystem::path::string() is the OS narrow encoding (ANSI on Windows) - what the engine's
// FS, CInifile and log use; u8string() is UTF-8 - what the Plugin API and SDL_LoadObject expect.
xr_string ToUtf8(const fs::path& path)
{
    // std::u8string in C++20 (MSVC build), std::string in C++17 (CMake build).
    const auto u8 = path.u8string();
    return xr_string(reinterpret_cast<const char*>(u8.c_str()), u8.size());
}

xr_string ToNarrow(const fs::path& path) { return xr_string(path.string().c_str()); }

bool EqualsNoCase(const xr_string& a, const xr_string& b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

xr_string WithTrailingSeparator(xr_string path)
{
    if (!path.empty() && path.back() != '\\' && path.back() != '/')
        path += static_cast<char>(fs::path::preferred_separator);
    return path;
}

// "MAJOR", "MAJOR.MINOR" or "MAJOR.MINOR.PATCH"; false for anything else.
bool ParseVersion(const xr_string& text, u32& out)
{
    u32 major = 0, minor = 0, patch = 0;
    char tail = 0;
    const int fields = sscanf(text.c_str(), "%u.%u.%u%c", &major, &minor, &patch, &tail);
    if (fields < 1 || fields > 3 || major > 0x3FFu || minor > 0x3FFu || patch > 0xFFFu)
        return false;
    out = GWP_MAKE_VERSION(major, minor, patch);
    return true;
}

xr_string VersionString(u32 version)
{
    string64 buf;
    xr_sprintf(buf, "%u.%u.%u", (version >> 22) & 0x3FFu, (version >> 12) & 0x3FFu, version & 0xFFFu);
    return buf;
}

bool IsValidId(const xr_string& id)
{
    if (id.empty() || id.size() > 64)
        return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

xr_string Trim(const xr_string& text)
{
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == xr_string::npos)
        return xr_string();
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// ---------------------------------------------------------------------------------------------
// Minimal ini reader for untrusted files (manifests, search roots).
// CInifile is not used on purpose: it calls xrDebug::Fatal / R_ASSERT on malformed input
// ("[addon" without "]", duplicate sections, "[a]:b" inheritance, "#include", lines over 4 KB),
// and a typo in a third-party addon.ltx must not crash the game.
// Supported: [section] (case-insensitive), key = value, lines without "=", ";" and "//" comments,
// quoted values. Section order and line order are preserved.
// ---------------------------------------------------------------------------------------------

struct IniLine
{
    xr_string key;
    xr_string value;
};

struct IniSection
{
    xr_string name; // lower case
    xr_vector<IniLine> lines;
};

struct SimpleIni
{
    xr_vector<IniSection> sections;

    const IniSection* find(pcstr name) const
    {
        for (const IniSection& section : sections)
        {
            if (EqualsNoCase(section.name, name))
                return &section;
        }
        return nullptr;
    }

    // False when the key is absent or the value is empty ("key =").
    bool get(pcstr section_name, pcstr key, xr_string& out) const
    {
        const IniSection* section = find(section_name);
        if (!section)
            return false;
        for (const IniLine& line : section->lines)
        {
            if (EqualsNoCase(line.key, key) && !line.value.empty())
            {
                out = line.value;
                return true;
            }
        }
        return false;
    }

    bool get_bool(pcstr section_name, pcstr key, bool default_value) const
    {
        xr_string value;
        if (!get(section_name, key, value))
            return default_value;
        return EqualsNoCase(value, "true") || EqualsNoCase(value, "on") || EqualsNoCase(value, "yes") || value == "1";
    }
};

// Returns false with a message in `error` on a syntax error; `text` may be any bytes, NUL not required.
bool ParseSimpleIni(const char* text, size_t size, SimpleIni& out, xr_string& error)
{
    out.sections.clear();
    IniSection* current = nullptr;
    u32 line_no = 0;
    size_t pos = 0;
    while (pos < size)
    {
        size_t end = pos;
        while (end < size && text[end] != '\n')
            ++end;
        xr_string line(text + pos, end - pos);
        pos = end + 1;
        ++line_no;

        // Strip comments (the engine's ini syntax: ";" or "//" to the end of line).
        size_t cut = line.find(';');
        const size_t slashes = line.find("//");
        if (slashes != xr_string::npos && (cut == xr_string::npos || slashes < cut))
            cut = slashes;
        if (cut != xr_string::npos)
            line.erase(cut);
        line = Trim(line);
        if (line.empty())
            continue;

        string64 where;
        xr_sprintf(where, " (line %u)", line_no);

        if (line[0] == '[')
        {
            const size_t close = line.find(']');
            if (close == xr_string::npos)
            {
                error = xr_string("section header without ']'") + where;
                return false;
            }
            if (!Trim(line.substr(close + 1)).empty())
            {
                error = xr_string("unexpected text after section header (inheritance is not supported)") + where;
                return false;
            }
            xr_string name = Trim(line.substr(1, close - 1));
            std::transform(name.begin(), name.end(), name.begin(), [](char c) {
                return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            });
            if (name.empty())
            {
                error = xr_string("empty section name") + where;
                return false;
            }
            // A repeated section continues the first one.
            current = nullptr;
            for (IniSection& section : out.sections)
            {
                if (section.name == name)
                    current = &section;
            }
            if (!current)
            {
                out.sections.push_back(IniSection{ name, {} });
                current = &out.sections.back();
            }
            continue;
        }

        if (line[0] == '#')
        {
            error = xr_string("directives such as #include are not supported") + where;
            return false;
        }
        if (!current)
        {
            error = xr_string("value outside of any section") + where;
            return false;
        }

        IniLine entry;
        const size_t eq = line.find('=');
        if (eq == xr_string::npos)
        {
            entry.key = line;
        }
        else
        {
            entry.key = Trim(line.substr(0, eq));
            entry.value = Trim(line.substr(eq + 1));
            if (entry.value.size() >= 2 && entry.value.front() == '"' && entry.value.back() == '"')
                entry.value = entry.value.substr(1, entry.value.size() - 2);
        }
        if (entry.key.empty())
        {
            error = xr_string("empty key") + where;
            return false;
        }
        current->lines.push_back(std::move(entry));
    }
    return true;
}

bool ReadFileBytes(const fs::path& path, xr_vector<char>& out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0)
        return false;
    file.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0)
        file.read(out.data(), size);
    return static_cast<bool>(file) || file.eof();
}

// ---------------------------------------------------------------------------------------------
// Logging. Tags: [addons] host, [addon:<id>] addon state, [plugin:<id>] messages written by a plugin.
// ---------------------------------------------------------------------------------------------

void Logf(pcstr prefix, pcstr tag, pcstr format, ...)
{
    string1024 buf;
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    Msg("%s[%s] %s", prefix, tag, buf);
}

xr_string AddonTag(const AddonRecord& addon) { return "addon:" + addon.id; }

// ---------------------------------------------------------------------------------------------
// GwpEngineApi implementation (v0.1)
// ---------------------------------------------------------------------------------------------

const AddonRecord* AddonOf(const GwpPlugin* self) { return self ? self->addon : nullptr; }

void GWP_CALL ApiLog(const GwpPlugin* self, GwpLogLevel level, const char* text)
{
    const AddonRecord* addon = AddonOf(self);
    const pcstr id = addon ? addon->id.c_str() : "?";
    const pcstr msg = text ? text : "";
    switch (level)
    {
    case GWP_LOG_DEBUG:
        if (g_debug_log)
            Msg("  [plugin:%s] %s", id, msg);
        break;
    case GWP_LOG_WARNING: Msg("~ [plugin:%s] %s", id, msg); break;
    case GWP_LOG_ERROR: Msg("! [plugin:%s] %s", id, msg); break;
    case GWP_LOG_INFO:
    default: Msg("* [plugin:%s] %s", id, msg); break;
    }
}

const char* GWP_CALL ApiAddonId(const GwpPlugin* self)
{
    const AddonRecord* addon = AddonOf(self);
    return addon ? addon->id.c_str() : "";
}

const char* GWP_CALL ApiAddonVersion(const GwpPlugin* self)
{
    const AddonRecord* addon = AddonOf(self);
    return addon ? addon->version.c_str() : "";
}

const char* GWP_CALL ApiAddonDir(const GwpPlugin* self)
{
    const AddonRecord* addon = AddonOf(self);
    return addon ? addon->dir.c_str() : "";
}

uint32_t GWP_CALL ApiEngineBuildId() { return Core.GetBuildId(); }
int GWP_CALL ApiIsMainThread() { return std::this_thread::get_id() == g_main_thread_id ? 1 : 0; }
int GWP_CALL ApiIsDebugLog() { return g_debug_log ? 1 : 0; } // set once in Initialize, read-only afterwards

void FillEngineApi()
{
    g_engine_api = {};
    g_engine_api.abi_major = GWP_API_VERSION_MAJOR;
    g_engine_api.size = sizeof(GwpEngineApi);
    g_engine_api.api_version = GWP_API_VERSION;
    g_engine_api.log = &ApiLog;
    g_engine_api.addon_id = &ApiAddonId;
    g_engine_api.addon_version = &ApiAddonVersion;
    g_engine_api.addon_dir = &ApiAddonDir;
    g_engine_api.engine_build_id = &ApiEngineBuildId;
    g_engine_api.is_main_thread = &ApiIsMainThread;
    g_engine_api.is_debug_log = &ApiIsDebugLog;
}

// ---------------------------------------------------------------------------------------------
// Search roots
// ---------------------------------------------------------------------------------------------

// The engine FS keeps "\" in paths (fsgame.ltx, update_path results) and converts them to "/" only when it
// calls the OS (convert_path_separators). std::filesystem needs native separators: on POSIX "\" is an ordinary
// file name character, so "/game/gamedata\scripts\" would name a non-existent directory.
xr_string ToNativeSeparators(xr_string path)
{
#if !defined(XR_PLATFORM_WINDOWS)
    std::replace(path.begin(), path.end(), '\\', '/');
#endif
    return path;
}

// Paths are case-insensitive on Windows only.
bool SamePath(const xr_string& a, const xr_string& b)
{
#if defined(XR_PLATFORM_WINDOWS)
    return EqualsNoCase(a, b);
#else
    return a == b;
#endif
}

// "$alias$rest" -> physical path via fsgame aliases; anything else is taken as a physical path.
// The result always uses native separators.
bool ResolveRoot(pcstr entry, xr_string& out)
{
    xr_string text = entry;
    if (text.empty())
        return false;
    if (text[0] == '$')
    {
        const size_t close = text.find('$', 1);
        if (close == xr_string::npos)
            return false;
        const xr_string alias = text.substr(0, close + 1);
        const xr_string rest = text.substr(close + 1);
        if (!FS.path_exist(alias.c_str()))
            return false;
        string_path buf;
        FS.update_path(buf, alias.c_str(), rest.c_str());
        out = ToNativeSeparators(buf);
        return true;
    }
    out = ToNativeSeparators(text);
    return true;
}

void AddRoot(xr_vector<xr_string>& roots, const xr_string& root)
{
    const xr_string native = ToNativeSeparators(root); // -addon_dir may come with "\" too
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(fs::path(native.c_str()), ec);
    const xr_string normalized = ec ? native : xr_string(canonical.string().c_str());
    const auto same = [&](const xr_string& r) { return SamePath(r, normalized); };
    if (std::none_of(roots.begin(), roots.end(), same))
        roots.push_back(normalized);
}

// -addon_dir <path> (repeatable, path may be quoted)
void CollectCommandLineRoots(xr_vector<xr_string>& roots)
{
    constexpr pcstr key = "-addon_dir ";
    for (pcstr p = strstr(Core.Params, key); p; p = strstr(p, key))
    {
        p += xr_strlen(key);
        while (*p == ' ')
            ++p;
        xr_string path;
        if (*p == '"')
        {
            const pcstr end = strchr(p + 1, '"');
            path.assign(p + 1, end ? end : p + xr_strlen(p));
            p = end ? end + 1 : p + xr_strlen(p);
        }
        else
        {
            const pcstr end = strchr(p, ' ');
            path.assign(p, end ? end : p + xr_strlen(p));
            p = end ? end : p + xr_strlen(p);
        }
        if (!path.empty())
            AddRoot(roots, path);
    }
}

// The common folder of all addons: fsgame.ltx alias
//   $game_addons$ = false| false| $fs_root$| addons\
// recurse = false keeps the engine FS from registering the addons' contents (bin/, sources, ...).
// Without the alias (an old fsgame.ltx) the same folder is used: <game>\addons\.
void CollectGameAddonsRoot(xr_vector<xr_string>& roots)
{
    xr_string resolved;
    if (ResolveRoot(FS.path_exist("$game_addons$") ? "$game_addons$" : "$fs_root$addons\\", resolved))
        AddRoot(roots, resolved);
}

// ---------------------------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------------------------

bool ParseManifest(AddonRecord& addon)
{
    xr_vector<char> bytes;
    if (!ReadFileBytes(addon.manifest_path, bytes))
    {
        addon.reason = "cannot read manifest";
        return false;
    }

    SimpleIni ini;
    xr_string error;
    if (!ParseSimpleIni(bytes.data(), bytes.size(), ini, error))
    {
        addon.reason = "manifest syntax error: " + error;
        return false;
    }

    if (!ini.get("addon", "id", addon.id))
    {
        addon.reason = "manifest has no [addon] id";
        return false;
    }
    if (!IsValidId(addon.id))
    {
        addon.reason = "invalid [addon] id '" + addon.id + "' (allowed: a-z 0-9 _ , up to 64 chars)";
        return false;
    }
    if (!ini.get("addon", "version", addon.version))
        addon.version = "0.0.0";

    if (!ini.get("addon", "type", addon.type_name))
    {
        addon.reason = "manifest has no [addon] type (allowed: " + AddonTypeList() + ")";
        return false;
    }
    addon.type = ParseAddonType(addon.type_name);
    if (addon.type == EAddonType::Unknown)
    {
        addon.reason = "unknown [addon] type '" + addon.type_name + "' (allowed: " + AddonTypeList() + ")";
        return false;
    }

    if (ini.find("plugin"))
    {
        if (!ini.get("plugin", "name", addon.plugin_name))
        {
            addon.reason = "[plugin] section has no name";
            return false;
        }
        // The name becomes a file name and a path component: same alphabet as the id keeps it inside bin/<os>/<arch>/.
        if (!IsValidId(addon.plugin_name))
        {
            addon.reason = "invalid [plugin] name '" + addon.plugin_name + "' (allowed: a-z 0-9 _ , up to 64 chars)";
            return false;
        }
        xr_string api_min;
        if (ini.get("plugin", "api_min", api_min) && !ParseVersion(api_min, addon.plugin_api_min))
        {
            addon.reason = "invalid [plugin] api_min '" + api_min + "' (expected MAJOR.MINOR.PATCH)";
            return false;
        }
        addon.plugin_required = ini.get_bool("plugin", "required", false);
    }
    return true;
}

void DiscoverInRoot(const xr_string& root)
{
    std::error_code ec;
    const fs::path root_path(root.c_str());
    if (!fs::is_directory(root_path, ec))
    {
        if (g_debug_log)
            Logf("  ", "addons", "search root does not exist: %s", root.c_str());
        return;
    }

    // One level deep: <root>/<dir>/addon.ltx; deterministic order by folder name.
    xr_vector<fs::path> candidates;
    for (fs::directory_iterator it(root_path, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec))
    {
        std::error_code entry_ec;
        if (it->is_directory(entry_ec) && fs::is_regular_file(it->path() / kManifestName, entry_ec))
            candidates.push_back(it->path());
    }
    std::sort(candidates.begin(), candidates.end());

    for (const fs::path& dir : candidates)
    {
        auto addon = xr_make_unique<AddonRecord>();
        addon->dir_path = dir;
        addon->manifest_path = dir / kManifestName;
        addon->dir = WithTrailingSeparator(ToUtf8(dir));
        addon->dir_log = WithTrailingSeparator(ToNarrow(dir));
        addon->handle.addon = addon.get();

        if (!ParseManifest(*addon))
        {
            addon->id = ToNarrow(dir.filename());
            addon->state = EAddonState::Failed;
            Logf("! ", AddonTag(*addon).c_str(), "%s: %s", addon->reason.c_str(),
                ToNarrow(addon->manifest_path).c_str());
            g_addons->push_back(std::move(addon));
            continue;
        }

        const auto same_id = [&](const xr_unique_ptr<AddonRecord>& other) {
            return other->state != EAddonState::Failed && other->id == addon->id;
        };
        const auto duplicate = std::find_if(g_addons->begin(), g_addons->end(), same_id);
        if (duplicate != g_addons->end())
        {
            addon->state = EAddonState::Skipped;
            addon->reason = "duplicate id, first found at " + (*duplicate)->dir_log;
            Logf("~ ", AddonTag(*addon).c_str(), "%s (skipped %s)", addon->reason.c_str(), addon->dir_log.c_str());
        }
        g_addons->push_back(std::move(addon));
    }
}

// ---------------------------------------------------------------------------------------------
// Plugin loading
// ---------------------------------------------------------------------------------------------

// Loads a copy from $app_data_root$\cache\plugins\<addon id>\ so the original can be rebuilt while the game runs.
// shadow_without_ext: UTF-8 path of the copy without extension - the form XRay::ModuleHandle::Open (SDL_LoadObject) wants.
bool MakeShadowCopy(AddonRecord& addon, const fs::path& original, xr_string& shadow_without_ext)
{
    std::error_code ec;
    const auto size = fs::file_size(original, ec);
    if (ec)
    {
        addon.reason = "cannot stat plugin library";
        return false;
    }
    const auto mtime = fs::last_write_time(original, ec).time_since_epoch().count();

    string_path cache_root;
    FS.update_path(cache_root, "$app_data_root$", "cache\\plugins\\");
    const fs::path cache_dir = fs::path(ToNativeSeparators(cache_root).c_str()) / addon.id.c_str();
    fs::create_directories(cache_dir, ec);
    if (ec)
    {
        addon.reason = "cannot create plugin cache directory";
        return false;
    }

    string256 stem;
    xr_sprintf(stem, "%s_%llu_%lld", addon.plugin_name.c_str(), static_cast<unsigned long long>(size),
        static_cast<long long>(mtime));
    const fs::path shadow = cache_dir / (xr_string(stem) + kLibraryExt).c_str();

    if (!fs::exists(shadow, ec))
    {
        fs::copy_file(original, shadow, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            addon.reason = "cannot copy plugin library to cache";
            return false;
        }
    }

    // Drop stale copies of this plugin; copies still loaded by another process stay (removal fails).
    const xr_string prefix = addon.plugin_name + "_";
    for (fs::directory_iterator it(cache_dir, ec), end; !ec && it != end; it.increment(ec))
    {
        const xr_string name(it->path().filename().string().c_str());
        if (name.rfind(prefix, 0) == 0 && it->path() != shadow)
        {
            std::error_code rm_ec;
            fs::remove(it->path(), rm_ec);
        }
    }

    addon.shadow_path = ToNarrow(shadow);
    fs::path without_ext = shadow;
    without_ext.replace_extension();
    shadow_without_ext = ToUtf8(without_ext);
    return true;
}

void MarkPluginUnavailable(AddonRecord& addon)
{
    addon.plugin_state = EPluginState::Unavailable;
    if (addon.plugin_required)
    {
        addon.state = EAddonState::Failed;
        Logf("! ", AddonTag(addon).c_str(), "failed, required plugin '%s' unavailable: %s", addon.plugin_name.c_str(),
            addon.reason.c_str());
    }
    else
    {
        addon.state = EAddonState::Active;
        Logf("~ ", AddonTag(addon).c_str(), "works without plugin '%s': %s", addon.plugin_name.c_str(),
            addon.reason.c_str());
    }
}

void LoadPlugin(AddonRecord& addon)
{
    if (addon.plugin_name.empty())
    {
        addon.plugin_state = EPluginState::None;
        addon.state = EAddonState::Active;
        return;
    }

    if (addon.plugin_api_min > GWP_API_VERSION)
    {
        addon.reason = "plugin requires Plugin API " + VersionString(addon.plugin_api_min) + ", engine has " +
            VersionString(GWP_API_VERSION);
        MarkPluginUnavailable(addon);
        return;
    }

    const fs::path library = addon.dir_path / "bin" / kPluginOs / kPluginArch / (addon.plugin_name + kLibraryExt).c_str();
    addon.plugin_path = ToNarrow(library);

    std::error_code ec;
    if (!fs::is_regular_file(library, ec))
    {
        addon.reason = xr_string("no build for ") + kPluginOs + "/" + kPluginArch + " (" + addon.plugin_path + ")";
        MarkPluginUnavailable(addon);
        return;
    }

    xr_string shadow_without_ext;
    if (!MakeShadowCopy(addon, library, shadow_without_ext))
    {
        MarkPluginUnavailable(addon);
        return;
    }

    addon.module = XRay::LoadModule(false);
    if (!addon.module->Open(shadow_without_ext.c_str()))
    {
        addon.module.reset();
        addon.reason = "cannot load library " + addon.shadow_path;
        MarkPluginUnavailable(addon);
        return;
    }

    const auto init = reinterpret_cast<GwpPluginInitFn>(addon.module->GetProcAddress(GWP_PLUGIN_ENTRY_NAME));
    if (!init)
    {
        addon.module.reset();
        addon.reason = xr_string("entry point '") + GWP_PLUGIN_ENTRY_NAME + "' not exported";
        MarkPluginUnavailable(addon);
        return;
    }

    // The plugin may be built against a newer minor version whose GwpPluginDesc is longer than ours.
    // It writes into a buffer of GWP_PLUGIN_DESC_MAX_SIZE bytes (the contract in gwp_api.h), never onto our struct.
    alignas(std::max_align_t) uint8_t desc_buffer[GWP_PLUGIN_DESC_MAX_SIZE] = {};
    GwpPluginDesc& desc = *reinterpret_cast<GwpPluginDesc*>(desc_buffer);
    const GwpResult result = init(&g_engine_api, &addon.handle, &desc);
    if (result != GWP_OK)
    {
        addon.module.reset();
        string64 code;
        xr_sprintf(code, "%d", static_cast<int>(result));
        addon.reason = xr_string(GWP_PLUGIN_ENTRY_NAME) + " returned " + code;
        MarkPluginUnavailable(addon);
        return;
    }

    // With a mismatching header the rest of the structure cannot be trusted (another layout), so on_unload is
    // deliberately not called before unloading. Documented in wiki/doc/plugins/api/entry_point.md.
    const bool header_ok = desc.size >= 3 * sizeof(uint32_t) && desc.size <= GWP_PLUGIN_DESC_MAX_SIZE &&
        desc.abi_major == GWP_API_VERSION_MAJOR && desc.api_min <= GWP_API_VERSION;
    if (!header_ok)
    {
        addon.module.reset();
        addon.reason = "plugin header mismatch: abi " + VersionString(GWP_MAKE_VERSION(desc.abi_major, 0, 0)) +
            ", requires api " + VersionString(desc.api_min) + ", engine api " + VersionString(GWP_API_VERSION);
        MarkPluginUnavailable(addon);
        return;
    }

    // Copy only the part of the description the plugin actually filled (older plugins have a shorter struct).
    addon.desc = {};
    memcpy(&addon.desc, &desc, std::min<size_t>(desc.size, sizeof(GwpPluginDesc)));
    addon.plugin_state = EPluginState::Loaded;
    addon.state = EAddonState::Active;
    addon.reason.clear();
    Logf("* ", AddonTag(addon).c_str(), "plugin '%s' loaded: %s", addon.plugin_name.c_str(), addon.plugin_path.c_str());
}
} // namespace detail

void Initialize()
{
    using namespace detail;

    if (g_initialized)
        return;
    g_initialized = true;
    // Called from CAI_Space::init(); the first ai() happens in the CMainMenu constructor on the main thread
    // (CGamePersistent::OnAppStart). If that ever moves to a worker, is_main_thread() becomes wrong.
    g_main_thread_id = std::this_thread::get_id();
    g_debug_log = strstr(Core.Params, "-addon_debug") != nullptr;
    g_addons = xr_new<xr_vector<xr_unique_ptr<AddonRecord>>>();
    FillEngineApi();

    // TODO(U1): appdata/addons.ltx written by the launcher goes first (explicit paths, order, enabled, trust).
    xr_vector<xr_string> roots;
    CollectCommandLineRoots(roots); // -addon_dir: development folders, win on a duplicate id
    CollectGameAddonsRoot(roots);   // <game>\addons\ ($game_addons$ in fsgame.ltx)

    Logf("* ", "addons", "Plugin API %s, platform %s/%s, %u search root(s)", VersionString(GWP_API_VERSION).c_str(),
        kPluginOs, kPluginArch, static_cast<u32>(roots.size()));

    for (const xr_string& root : roots)
    {
        if (g_debug_log)
            Logf("  ", "addons", "search root: %s", root.c_str());
        DiscoverInRoot(root);
    }

    for (auto& addon : *g_addons)
    {
        if (addon->state == EAddonState::Discovered)
            LoadPlugin(*addon);
    }

    u32 active = 0, plugins = 0;
    for (const auto& addon : *g_addons)
    {
        active += addon->state == EAddonState::Active ? 1 : 0;
        plugins += addon->plugin_state == EPluginState::Loaded ? 1 : 0;
    }
    Logf("* ", "addons", "%u addon(s) found, %u active, %u plugin(s) loaded", static_cast<u32>(g_addons->size()),
        active, plugins);
}

void Shutdown()
{
    using namespace detail;

    if (!g_addons)
        return;
    for (auto it = g_addons->rbegin(); it != g_addons->rend(); ++it)
    {
        AddonRecord& addon = **it;
        if (addon.plugin_state != EPluginState::Loaded)
            continue;
        if (addon.desc.on_unload)
            addon.desc.on_unload(addon.desc.user);
        addon.module.reset();
        addon.plugin_state = EPluginState::Unavailable;
    }
    xr_delete(g_addons);
    g_initialized = false;
}

void PrintList()
{
    using namespace detail;

    if (!g_addons)
    {
        Msg("- [addons] not initialized");
        return;
    }
    Msg("- [addons] Plugin API %s, platform %s/%s, %u addon(s):", VersionString(GWP_API_VERSION).c_str(), kPluginOs,
        kPluginArch, static_cast<u32>(g_addons->size()));
    for (const auto& addon : *g_addons)
    {
        Msg("-   %-24s %-10s %-10s type=%s plugin=%s (%s) trust=local dir=%s%s%s", addon->id.c_str(),
            addon->version.c_str(), AddonStateName(addon->state),
            addon->type_name.empty() ? "-" : addon->type_name.c_str(),
            addon->plugin_name.empty() ? "-" : addon->plugin_name.c_str(),
            PluginStateName(addon->plugin_state), addon->dir_log.c_str(), addon->reason.empty() ? "" : " reason=",
            addon->reason.c_str());
    }
}
} // namespace gw::addons
