#include "StdAfx.h"

#include "addon_host.h"
#include "addon_api_console.h"
#include "addon_api_ini.h"
#include "addon_api_services.h"
#include "addon_api_threads.h"
#include "addon_api_alife_ext.h"
#include "addon_api_world.h"
#include "addon_api_feedback.h"
#include "addon_api_character.h"
#include "addon_api_inventory_ext.h"
#include "addon_binders.h"
#include "addon_callbacks.h"
#include "addon_data_bus.h"
#include "addon_goap.h"
#include "addon_goap_planner.h"
#include "addon_storage.h"
#include "addon_event_bus.h"
#include "addon_timers.h"

#include "xrAddonHost/include/gwp/gwp_api.h"
#include "xrCore/ModuleLookup.hpp"
#include "xrEngine/EngineThreading.h"

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
    Failed,     // addon cannot work (manifest error, required plugin failed, dependency missing or failed)
    Skipped,    // duplicate id
    Disabled,   // switched off in appdata/addons.ltx, or not listed there
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

// [addon] version and the versions of [addon] depends conditions: MAJOR.MINOR.PATCH, plain numbers.
// Not packed like GWP_MAKE_VERSION: an addon may number its versions by date (2026.10.3).
struct AddonVersion
{
    u32 major = 0;
    u32 minor = 0;
    u32 patch = 0;
};

int CompareAddonVersions(const AddonVersion& a, const AddonVersion& b)
{
    if (a.major != b.major)
        return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor)
        return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch)
        return a.patch < b.patch ? -1 : 1;
    return 0;
}

enum class EVersionOp
{
    Less,
    LessEqual,
    Equal,
    GreaterEqual,
    Greater,
};

struct VersionCondition
{
    EVersionOp op = EVersionOp::GreaterEqual;
    AddonVersion version;
    xr_string text; // as written: ">=1.2.0"
};

// One entry of [addon] depends: "some_lib", "some_lib>=1.2.0", "some_lib >=1.2 <2" (every condition must hold).
struct AddonDependency
{
    xr_string id;
    xr_vector<VersionCondition> conditions;
    xr_string text; // the entry as written, for messages
};

enum class EPluginState
{
    None,        // addon declares no plugin
    Loaded,      // plugin loaded and initialized
    Unavailable, // plugin declared but not loaded (see reason)
    Crashed,     // stopped by a crash: unloaded at the next frame, loaded again at the next game load (OnGameStart)
};

struct AddonRecord
{
    xr_string id;
    xr_string version;          // as written in the manifest: what addon_version() gives the plugin
    AddonVersion version_parsed; // valid only with version_valid
    bool version_valid = false;  // version is MAJOR.MINOR.PATCH
    xr_string type_name;
    EAddonType type = EAddonType::Unknown;
    xr_vector<AddonDependency> depends; // [addon] depends
    xr_string trust = "local";  // appdata/addons.ltx: first_party | index | local; only shown for now
    xr_string plugin_sha256;    // appdata/addons.ltx: hash of the plugin library, not checked yet
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
    const void* module_base = nullptr; // Windows: the HMODULE of the loaded copy (its symbols leave dbghelp with it)
    GwpPluginDesc desc{};
    GwpPlugin handle{};

    // Crashes and restarts (OnPluginCrashed, OnFrame, OnGameStart)
    bool loading = false;    // inside gwp_plugin_init: a crash there fails the load
    xr_string load_crash;    // what crashed during gwp_plugin_init
    u32 restarts = 0;        // automatic loads after a crash in this run of the game
    bool restart_refused_logged = false;
    bool reload_requested = false; // plugin_reload: at the next frame
};

// Heap-allocated on purpose: if Shutdown() is never called, a static vector would unload the plugin libraries
// from the xrGame DllMain(DLL_PROCESS_DETACH), which is not allowed. Leaking them instead is harmless at exit.
xr_vector<xr_unique_ptr<AddonRecord>>* g_addons = nullptr;
GwpEngineApi g_engine_api{};
bool g_initialized = false;
bool g_debug_log = false;
// Ids of every found addon, loaded or not (KnownAddonIds): filled before the first plugin loads
xr_vector<xr_string> g_known_ids;

// A crashed plugin is loaded again at the next game load, at most this many times per run of the game: a plugin
// that crashes on every load must not take every load with it
constexpr u32 kMaxAutoRestarts = 2;
bool g_plugin_work = false; // a crashed plugin waits for its unload, or plugin_reload asked for a reload

// Plugin save data by addon id. Independent of g_addons: data of addons that are not installed now is kept and
// written to new saves unchanged. xr_map: stable order in the save file.
struct SaveChunk
{
    u32 version = 0;
    xr_vector<u8> data;
};
xr_map<xr_string, SaveChunk> g_save_data;

// Chunk of the ALife save stream (the stream is a sequence of chunks, see alife_space.h: ALIFE_CHUNK_DATA...).
// Found by id, so its position in the stream does not matter; a save without it (older build) has no plugin data.
constexpr u32 kSaveChunkId = 0x0100;
constexpr u32 kSaveFormat = 1;
constexpr u32 kMaxSaveChunk = 64u * 1024u * 1024u;

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
    case EAddonState::Disabled: return "disabled";
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
    case EPluginState::Crashed: return "crashed";
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

// path::string() throws on Windows for a character the ANSI code page lacks (a folder from appdata/addons.ltx may
// have one): such a path goes to the log as UTF-8 instead of taking the game down.
xr_string ToNarrow(const fs::path& path)
{
    try
    {
        return xr_string(path.string().c_str());
    }
    catch (const std::system_error&)
    {
        return ToUtf8(path);
    }
}

// UTF-8 text (appdata/addons.ltx) -> path. On Windows through UTF-16; text that is not valid UTF-8 is taken in
// the narrow encoding, as the engine's own paths.
fs::path PathFromUtf8(const xr_string& text)
{
#if defined(XR_PLATFORM_WINDOWS)
    const int size = static_cast<int>(text.size());
    const int length = size ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), size, nullptr, 0) : 0;
    if (length <= 0)
        return fs::path(text.c_str());
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), size, &wide[0], length);
    return fs::path(wide);
#else
    return fs::path(text.c_str());
#endif
}

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

// Reads min_parts..3 dot-separated decimal numbers (up to 9 digits each) at text[pos]; pos moves past them.
// Missing parts are 0: "1.2" in a depends condition is 1.2.0.
bool ReadAddonVersion(const xr_string& text, size_t& pos, u32 min_parts, AddonVersion& out)
{
    u32 parts[3] = { 0, 0, 0 };
    u32 count = 0;
    while (count < 3)
    {
        u32 digits = 0;
        u32 value = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9')
        {
            if (++digits > 9)
                return false;
            value = value * 10 + static_cast<u32>(text[pos] - '0');
            ++pos;
        }
        if (digits == 0)
            return false;
        parts[count++] = value;
        if (count == 3 || pos >= text.size() || text[pos] != '.')
            break;
        ++pos; // the dot
    }
    if (count < min_parts)
        return false;
    out.major = parts[0];
    out.minor = parts[1];
    out.patch = parts[2];
    return true;
}

// [addon] version: exactly MAJOR.MINOR.PATCH.
bool ParseAddonVersion(const xr_string& text, AddonVersion& out)
{
    size_t pos = 0;
    return ReadAddonVersion(text, pos, 3, out) && pos == text.size();
}

xr_string AddonVersionString(const AddonVersion& version)
{
    string64 buf;
    xr_sprintf(buf, "%u.%u.%u", version.major, version.minor, version.patch);
    return buf;
}

bool VersionConditionHolds(const VersionCondition& condition, const AddonVersion& version)
{
    const int cmp = CompareAddonVersions(version, condition.version);
    switch (condition.op)
    {
    case EVersionOp::Less: return cmp < 0;
    case EVersionOp::LessEqual: return cmp <= 0;
    case EVersionOp::Equal: return cmp == 0;
    case EVersionOp::GreaterEqual: return cmp >= 0;
    case EVersionOp::Greater: return cmp > 0;
    }
    return false;
}

bool IsIdChar(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }

struct VersionOpName
{
    EVersionOp op;
    pcstr name;
};

constexpr VersionOpName kVersionOps[] = {
    { EVersionOp::GreaterEqual, ">=" },
    { EVersionOp::LessEqual, "<=" },
    { EVersionOp::Equal, "==" },
    { EVersionOp::Greater, ">" },
    { EVersionOp::Less, "<" },
    { EVersionOp::Equal, "=" },
};

// [addon] depends = a, b>=1.2.0, c >=1.0 <2.0
// Entries are separated by commas; an entry is an addon id and zero or more conditions "<op><version>" separated
// by spaces, every one must hold. op: >= > <= < = (== is the same as =). An empty entry (a trailing comma) is
// ignored. false with a message in `error` on anything else.
bool ParseDepends(const xr_string& value, xr_vector<AddonDependency>& out, xr_string& error)
{
    out.clear();
    size_t start = 0;
    while (start <= value.size())
    {
        size_t comma = value.find(',', start);
        if (comma == xr_string::npos)
            comma = value.size();
        const xr_string entry = Trim(value.substr(start, comma - start));
        start = comma + 1;
        if (entry.empty())
            continue;

        AddonDependency dependency;
        dependency.text = entry;
        size_t pos = 0;
        while (pos < entry.size() && IsIdChar(entry[pos]))
            ++pos;
        dependency.id = entry.substr(0, pos);
        if (!IsValidId(dependency.id))
        {
            error = "'" + entry + "': expected an addon id (a-z 0-9 _ , up to 64 chars) first";
            return false;
        }
        for (;;)
        {
            while (pos < entry.size() && (entry[pos] == ' ' || entry[pos] == '\t'))
                ++pos;
            if (pos >= entry.size())
                break;
            const size_t condition_start = pos;
            VersionCondition condition;
            // Two-character operators first: ">=" must not read as ">" followed by "=1.2.0"
            const VersionOpName* op = nullptr;
            for (const VersionOpName& candidate : kVersionOps)
            {
                if (entry.compare(pos, xr_strlen(candidate.name), candidate.name) == 0)
                {
                    op = &candidate;
                    break;
                }
            }
            if (!op)
            {
                error = "'" + entry + "': expected a version condition such as >=1.2.0 after the id";
                return false;
            }
            condition.op = op->op;
            pos += xr_strlen(op->name);
            while (pos < entry.size() && (entry[pos] == ' ' || entry[pos] == '\t'))
                ++pos;
            if (!ReadAddonVersion(entry, pos, 1, condition.version))
            {
                error = "'" + entry + "': expected a version (MAJOR[.MINOR[.PATCH]], numbers) after the operator";
                return false;
            }
            condition.text = entry.substr(condition_start, pos - condition_start);
            dependency.conditions.push_back(std::move(condition));
        }
        out.push_back(std::move(dependency));
    }
    return true;
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
// merge_repeated = false: a repeated section stays a separate entry of `sections` (find() and get() see the first
// one) - appdata/addons.ltx reports it and ignores it instead of mixing two entries of one addon.
bool ParseSimpleIni(const char* text, size_t size, SimpleIni& out, xr_string& error, bool merge_repeated = true)
{
    out.sections.clear();
    IniSection* current = nullptr;
    u32 line_no = 0;
    size_t pos = 0;
    // UTF-8 BOM (an editor may add it): not part of the first line
    if (size >= 3 && static_cast<u8>(text[0]) == 0xEF && static_cast<u8>(text[1]) == 0xBB &&
        static_cast<u8>(text[2]) == 0xBF)
        pos = 3;
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
            // A repeated section continues the first one (unless merge_repeated is off).
            current = nullptr;
            for (IniSection& section : out.sections)
            {
                if (merge_repeated && section.name == name)
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

GwpResult GWP_CALL ApiSaveWrite(const GwpPlugin* self, uint32_t data_version, const void* data, uint32_t size)
{
    const AddonRecord* addon = AddonOf(self);
    if (!addon || (size > 0 && !data) || size > kMaxSaveChunk)
        return GWP_ERROR_INVALID_ARGUMENT;
    if (!IsMainThread())
        return GWP_ERROR_NOT_MAIN_THREAD;
    if (size == 0)
    {
        g_save_data.erase(addon->id);
        return GWP_OK;
    }
    SaveChunk& chunk = g_save_data[addon->id];
    chunk.version = data_version;
    chunk.data.assign(static_cast<const u8*>(data), static_cast<const u8*>(data) + size);
    return GWP_OK;
}

GwpResult GWP_CALL ApiSaveRead(const GwpPlugin* self, uint32_t* data_version, const void** data, uint32_t* size)
{
    if (data_version)
        *data_version = 0;
    if (data)
        *data = nullptr;
    if (size)
        *size = 0;
    const AddonRecord* addon = AddonOf(self);
    if (!addon || !data || !size)
        return GWP_ERROR_INVALID_ARGUMENT;
    if (!IsMainThread())
        return GWP_ERROR_NOT_MAIN_THREAD;
    const auto it = g_save_data.find(addon->id);
    if (it == g_save_data.end() || it->second.data.empty())
        return GWP_ERROR_NOT_FOUND; // nothing failed: the save has no data of this addon
    if (data_version)
        *data_version = it->second.version;
    *data = it->second.data.data();
    *size = static_cast<uint32_t>(it->second.data.size());
    return GWP_OK;
}

// Thread that ran gw::addons::Initialize() and with it every gwp_plugin_init.
std::thread::id g_host_thread_id;

uint32_t GWP_CALL ApiEngineBuildId() { return Core.GetBuildId(); }
// "Main thread" of the Plugin API. Two threads qualify, and they never run plugin code at the same time:
//  - the thread running the game logic now. The engine runs Sheduler.Update (and with it the Lua binders, ALife and
//    every plugin callback) in a task that any worker of the task scheduler may execute while the main thread waits
//    for it, so the OS main thread alone is the wrong test: on such frames every event and API call would be rejected;
//  - the thread that loaded the plugins, as a safety net. IsGameLogicThread() compares against the thread marked by
//    XRay::Engine::MarkMainThread() in CApplication; before that mark existed it compared against the thread that
//    loaded xrEngine.dll, which is not the one CApplication runs on (xr_3da starts it as GameThreadEntry), and every
//    call of every plugin was rejected. With the mark in place this half is redundant, and it keeps the host working
//    if plugin loading ever moves off that thread. It counts only between frames: while the game logic task runs on
//    a worker, the OS main thread waits for it and may execute other tasks meanwhile (TaskManager::Wait), and code
//    of those must not reach the plugins at the same time as the game logic.
int GWP_CALL ApiIsMainThread()
{
    // A task_parallel_for piece is never "main", also when the logic thread runs it while waiting
    if (threads::InTaskPiece())
        return 0;
    if (XRay::Engine::IsGameLogicThread())
        return 1;
    return !XRay::Engine::IsGameThreadRunning() && std::this_thread::get_id() == g_host_thread_id ? 1 : 0;
}
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
    events::FillEngineApi(g_engine_api);
    FillObjectsApi(g_engine_api);
    data::FillEngineApi(g_engine_api);
    ini::FillEngineApi(g_engine_api);
    console::FillEngineApi(g_engine_api);
    FillLevelApi(g_engine_api);
    FillInfoApi(g_engine_api);
    timers::FillEngineApi(g_engine_api);
    binders::FillEngineApi(g_engine_api);
    storage::FillEngineApi(g_engine_api);
    callbacks::FillEngineApi(g_engine_api);
    goap::FillEngineApi(g_engine_api);
    FillNpcApi(g_engine_api);
    goap::FillPlannerApi(g_engine_api);
    FillExportsApi(g_engine_api);
    FillNpcControlApi(g_engine_api);
    threads::FillEngineApi(g_engine_api);
    services::FillEngineApi(g_engine_api);
    FillAlifeExtApi(g_engine_api);
    FillWorldApi(g_engine_api);
    feedback::FillEngineApi(g_engine_api);
    character::FillEngineApi(g_engine_api);
    items::FillEngineApi(g_engine_api);
    g_engine_api.save_write = &ApiSaveWrite;
    g_engine_api.save_read = &ApiSaveRead;
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

// Two search roots name one folder: the same path (trailing separators aside, case-insensitive on Windows) or the
// same directory on disk (a junction, another spelling).
bool SameRoot(const xr_string& a, const xr_string& b)
{
    const auto trimmed = [](xr_string path) {
        while (path.size() > 1 && (path.back() == '\\' || path.back() == '/'))
            path.pop_back();
        return path;
    };
    if (SamePath(trimmed(a), trimmed(b)))
        return true;
    std::error_code ec;
    return fs::equivalent(fs::path(a.c_str()), fs::path(b.c_str()), ec) && !ec;
}

void AddRoot(xr_vector<xr_string>& roots, const xr_string& root)
{
    const xr_string native = ToNativeSeparators(root); // -addon_dir may come with "\" too
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(fs::path(native.c_str()), ec);
    xr_string normalized = native;
    if (!ec)
    {
        // Roots stay in the narrow encoding (DiscoverInRoot reads them so). A junction or subst whose target has a
        // character the ANSI code page lacks makes string() throw: the root is kept as written, the OS follows it.
        try
        {
            normalized = canonical.string().c_str();
        }
        catch (const std::system_error&)
        {
            Logf("~ ", "addons", "search root %s: its real path is not representable in the ANSI code page, used "
                "as written", native.c_str());
        }
    }
    const auto same = [&](const xr_string& r) { return SameRoot(r, normalized); };
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

// warn = false: no warnings in the log (an addon that is not loaded anyway, only its id is wanted).
bool ParseManifest(AddonRecord& addon, bool warn = true)
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
    const xr_string tag = AddonTag(addon);
    if (!ini.get("addon", "version", addon.version))
        addon.version = "0.0.0";
    // A warning for now (plan 26, 4.2); depends conditions on this addon cannot hold then
    addon.version_valid = ParseAddonVersion(addon.version, addon.version_parsed);
    if (!addon.version_valid && warn)
    {
        Logf("~ ", tag.c_str(), "[addon] version '%s' is not MAJOR.MINOR.PATCH (numbers): version conditions of "
            "other addons on it never hold", addon.version.c_str());
    }

    if (!ini.get("addon", "type", addon.type_name))
    {
        addon.reason = "manifest has no [addon] type (allowed: " + AddonTypeList() + ")";
        return false;
    }
    addon.type = ParseAddonType(addon.type_name);
    if (addon.type == EAddonType::Unknown)
    {
        // A type of a newer engine: the addon works as plain content here (type_name keeps what is written)
        addon.type = EAddonType::Content;
        if (warn)
        {
            Logf("~ ", tag.c_str(), "unknown [addon] type '%s' (known: %s), treated as content",
                addon.type_name.c_str(), AddonTypeList().c_str());
        }
    }

    xr_string depends;
    if (ini.get("addon", "depends", depends))
    {
        xr_string error;
        if (!ParseDepends(depends, addon.depends, error))
        {
            addon.reason = "invalid [addon] depends: " + error;
            return false;
        }
        for (const AddonDependency& dependency : addon.depends)
        {
            if (dependency.id == addon.id)
            {
                addon.reason = "[addon] depends names the addon itself";
                return false;
            }
        }
    }
    // Reserved (plan 26, 4.3): read so that an addon written for a newer engine learns they are ignored here
    for (const pcstr reserved : { "conflicts", "load_after" })
    {
        xr_string value;
        if (warn && ini.get("addon", reserved, value))
            Logf("~ ", tag.c_str(), "[addon] %s is not supported by this engine yet, ignored", reserved);
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
        // ParseVersion also takes "1", "1.2", "1..2", "1.2x" (sscanf): kept for the plugins out there, only reported
        AddonVersion strict;
        if (warn && !api_min.empty() && !ParseAddonVersion(api_min, strict))
        {
            Logf("~ ", tag.c_str(), "[plugin] api_min '%s' is not MAJOR.MINOR.PATCH, read as %s", api_min.c_str(),
                VersionString(addon.plugin_api_min).c_str());
        }
        addon.plugin_required = ini.get_bool("plugin", "required", false);
    }
    return true;
}

xr_unique_ptr<AddonRecord> MakeAddonRecord(const fs::path& dir)
{
    auto addon = xr_make_unique<AddonRecord>();
    addon->dir_path = dir;
    addon->manifest_path = dir / kManifestName;
    addon->dir = WithTrailingSeparator(ToUtf8(dir));
    addon->dir_log = WithTrailingSeparator(ToNarrow(dir));
    addon->handle.addon = addon.get();
    return addon;
}

// The addon that answers to `id`: the first one found with it that has neither failed nor been skipped as a
// duplicate. Disabled ones count. nullptr when there is none - also when the copy that won the id failed later
// (its dependency, its required plugin): the skipped copies behind it never stand in for it.
AddonRecord* FindAddonById(const xr_string& id)
{
    for (auto& addon : *g_addons)
    {
        if (addon->state != EAddonState::Failed && addon->state != EAddonState::Skipped && addon->id == id)
            return addon.get();
    }
    return nullptr;
}

// Parses the manifest and adds the addon to g_addons: failed on a manifest error, skipped as a duplicate of an id
// found earlier. expected_id: the id appdata/addons.ltx gives the folder ("" when the folder came from a scan).
void AddFoundAddon(xr_unique_ptr<AddonRecord> addon, const xr_string& expected_id)
{
    if (!ParseManifest(*addon))
    {
        addon->id = expected_id.empty() ? ToNarrow(addon->dir_path.filename()) : expected_id;
        addon->state = EAddonState::Failed;
        Logf("! ", AddonTag(*addon).c_str(), "%s: %s", addon->reason.c_str(), ToNarrow(addon->manifest_path).c_str());
        g_addons->push_back(std::move(addon));
        return;
    }
    if (!expected_id.empty() && addon->id != expected_id)
    {
        // The folder holds another addon than the launcher thinks: the list is stale, the player has to look
        addon->state = EAddonState::Failed;
        addon->reason = "appdata/addons.ltx lists this folder as '" + expected_id + "', its addon.ltx has id '" +
            addon->id + "'";
        Logf("! ", AddonTag(*addon).c_str(), "%s: %s", addon->reason.c_str(), addon->dir_log.c_str());
        g_addons->push_back(std::move(addon));
        return;
    }

    if (const AddonRecord* duplicate = FindAddonById(addon->id))
    {
        addon->state = EAddonState::Skipped;
        addon->reason = "duplicate id, first found at " + duplicate->dir_log;
        Logf("~ ", AddonTag(*addon).c_str(), "%s (skipped %s)", addon->reason.c_str(), addon->dir_log.c_str());
    }
    g_addons->push_back(std::move(addon));
}

bool SameFolder(const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    return fs::equivalent(a, b, ec) && !ec;
}

// unlisted = true: appdata/addons.ltx is in charge; the folders it does not name are recorded as disabled, so that
// addon_list tells the player why they do not work (the launcher has not seen them yet).
void DiscoverInRoot(const xr_string& root, bool unlisted = false)
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
        if (!unlisted)
        {
            AddFoundAddon(MakeAddonRecord(dir), "");
            continue;
        }
        const auto same_folder = [&](const xr_unique_ptr<AddonRecord>& other) {
            return SameFolder(other->dir_path, dir);
        };
        if (std::any_of(g_addons->begin(), g_addons->end(), same_folder))
            continue; // listed, or found with -addon_dir
        auto addon = MakeAddonRecord(dir);
        if (!ParseManifest(*addon, false)) // quietly: only the id is wanted
            addon->id = ToNarrow(dir.filename());
        addon->state = EAddonState::Disabled;
        addon->reason = "not listed in appdata/addons.ltx";
        Logf("~ ", AddonTag(*addon).c_str(), "%s, not loaded: %s", addon->reason.c_str(), addon->dir_log.c_str());
        g_addons->push_back(std::move(addon));
    }
}

// ---------------------------------------------------------------------------------------------
// appdata/addons.ltx: the list of addons written by the launcher (docs: wiki/doc/plugins/loading.md)
//   [addons]              format = 1
//   [addon.<id>]          one section per addon, in load order:
//                         path = <folder>, enabled = true|false, trust = first_party|index|local,
//                         plugin_sha256 = <64 hex digits>
// Read with ParseSimpleIni: a broken file is reported and the addon folders are scanned as without it.
// ---------------------------------------------------------------------------------------------

constexpr pcstr kLauncherListName = "addons.ltx";
constexpr pcstr kLauncherListLog = "appdata/addons.ltx";
constexpr pcstr kLauncherEntryPrefix = "addon.";
constexpr u32 kLauncherListFormat = 1;

struct LauncherEntry
{
    xr_string id;
    xr_string path; // as written (UTF-8); empty: <game addons folder>/<id>
    bool enabled = true;
    xr_string trust = "local";
    xr_string plugin_sha256;
};

bool IsHexDigest(const xr_string& text, size_t length)
{
    return text.size() == length && std::all_of(text.begin(), text.end(), [](char c) {
        return std::isxdigit(static_cast<unsigned char>(c)) != 0;
    });
}

// false: no file, or a file that cannot be used (reported) - the addon folders are scanned as without it.
bool ReadLauncherList(xr_vector<LauncherEntry>& out)
{
    out.clear();
    string_path file_name;
    FS.update_path(file_name, "$app_data_root$", kLauncherListName);
    const fs::path file(ToNativeSeparators(file_name).c_str());
    std::error_code ec;
    if (!fs::is_regular_file(file, ec))
    {
        if (g_debug_log)
            Logf("  ", "addons", "no %s, the addon folders are scanned", kLauncherListLog);
        return false;
    }

    xr_vector<char> bytes;
    SimpleIni ini;
    xr_string error;
    if (!ReadFileBytes(file, bytes))
        error = "cannot read the file";
    else if (!ParseSimpleIni(bytes.data(), bytes.size(), ini, error, false)) // a repeated [addon.x]: reported below
        error = "syntax error: " + error;
    if (!error.empty())
    {
        Logf("! ", "addons", "%s: %s; the addon folders are scanned as without the file", kLauncherListLog,
            error.c_str());
        return false;
    }

    // A plain decimal number; anything else ("1abc", "1.0", "-1") is an unknown format, as a number other than ours
    xr_string format_text;
    u32 format = kLauncherListFormat; // no key: the first format
    if (ini.get("addons", "format", format_text))
    {
        const bool is_number = format_text.size() <= 9 && std::all_of(format_text.begin(), format_text.end(),
            [](char c) { return c >= '0' && c <= '9'; });
        format = is_number ? static_cast<u32>(strtoul(format_text.c_str(), nullptr, 10)) : 0;
    }
    if (format != kLauncherListFormat)
    {
        Logf("~ ", "addons", "%s: format '%s', this engine reads format %u: what it does not know is ignored",
            kLauncherListLog, format_text.c_str(), kLauncherListFormat);
    }

    const size_t prefix_length = xr_strlen(kLauncherEntryPrefix);
    for (const IniSection& section : ini.sections)
    {
        if (section.name == "addons")
            continue;
        if (section.name.compare(0, prefix_length, kLauncherEntryPrefix) != 0)
        {
            Logf("~ ", "addons", "%s: unknown section [%s], ignored", kLauncherListLog, section.name.c_str());
            continue;
        }
        LauncherEntry entry;
        entry.id = section.name.substr(prefix_length);
        if (!IsValidId(entry.id))
        {
            Logf("~ ", "addons", "%s: [%s]: invalid addon id (allowed: a-z 0-9 _ , up to 64 chars), ignored",
                kLauncherListLog, section.name.c_str());
            continue;
        }
        const auto same_id = [&](const LauncherEntry& other) { return other.id == entry.id; };
        if (std::any_of(out.begin(), out.end(), same_id))
        {
            Logf("~ ", "addons", "%s: [%s] is listed twice, the first one counts", kLauncherListLog,
                section.name.c_str());
            continue;
        }
        const pcstr name = section.name.c_str();
        ini.get(name, "path", entry.path);
        entry.enabled = ini.get_bool(name, "enabled", true);
        xr_string trust;
        if (ini.get(name, "trust", trust))
        {
            std::transform(trust.begin(), trust.end(), trust.begin(), [](char c) {
                return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            });
            if (trust == "first_party" || trust == "index" || trust == "local")
                entry.trust = trust;
            else
                Logf("~ ", "addons", "%s: [%s]: unknown trust '%s' (first_party, index, local), taken as local",
                    kLauncherListLog, name, trust.c_str());
        }
        if (ini.get(name, "plugin_sha256", entry.plugin_sha256) && !IsHexDigest(entry.plugin_sha256, 64))
        {
            Logf("~ ", "addons", "%s: [%s]: plugin_sha256 is not 64 hex digits, ignored", kLauncherListLog, name);
            entry.plugin_sha256.clear();
        }
        out.push_back(std::move(entry));
    }
    return true;
}

// The folder of an entry: empty path - <game addons folder>/<id>; "$alias$rest" - through fsgame.ltx;
// a relative path - from the game folder ($fs_root$); an absolute one as it is.
// false with the reason in `why` when the folder cannot be named.
bool LauncherEntryFolder(const LauncherEntry& entry, const xr_string& game_root, fs::path& out, xr_string& why)
{
    if (entry.path.empty())
    {
        if (game_root.empty())
        {
            why = "no path, and the game has no addons folder ($game_addons$ in fsgame.ltx, <game>/addons/) for the "
                  "default one";
            return false;
        }
        out = fs::path(game_root.c_str()) / entry.id.c_str();
        return true;
    }
    xr_string base_alias = "$fs_root$";
    xr_string rest = ToNativeSeparators(entry.path);
    if (rest[0] == '$')
    {
        const size_t close = rest.find('$', 1);
        if (close == xr_string::npos)
        {
            why = "path '" + entry.path + "' opens an fsgame.ltx alias with '$' and does not close it";
            return false;
        }
        base_alias = rest.substr(0, close + 1);
        rest = rest.substr(close + 1);
    }
    fs::path relative = PathFromUtf8(rest);
    if (relative.is_absolute())
    {
        out = relative;
        return true;
    }
    if (!FS.path_exist(base_alias.c_str()))
    {
        why = "path '" + entry.path + "': fsgame.ltx has no alias " + base_alias;
        return false;
    }
    string_path base;
    FS.update_path(base, base_alias.c_str(), "");
    // "\addons\x" after an alias is relative to it: a leading separator would make it the root of the drive
    xr_string relative_text = rest;
    while (!relative_text.empty() && (relative_text[0] == '\\' || relative_text[0] == '/'))
        relative_text.erase(0, 1);
    relative = PathFromUtf8(relative_text);
    out = fs::path(ToNativeSeparators(base).c_str()) / relative;
    return true;
}

void AddListedAddon(const LauncherEntry& entry, const xr_string& game_root)
{
    fs::path dir;
    xr_string why;
    const bool has_folder = LauncherEntryFolder(entry, game_root, dir, why);
    std::error_code ec;
    if (has_folder)
    {
        const fs::path canonical = fs::weakly_canonical(dir, ec);
        if (!ec)
            dir = canonical;
    }

    auto addon = MakeAddonRecord(dir);
    addon->trust = entry.trust;
    addon->plugin_sha256 = entry.plugin_sha256;
    if (!entry.enabled)
    {
        // Not parsed: a switched-off addon must not fill the log, its id is known from the list
        addon->id = entry.id;
        addon->state = EAddonState::Disabled;
        addon->reason = "disabled in appdata/addons.ltx";
        Logf("* ", AddonTag(*addon).c_str(), "disabled in %s", kLauncherListLog);
        g_addons->push_back(std::move(addon));
        return;
    }
    if (!has_folder || !fs::is_regular_file(addon->manifest_path, ec))
    {
        addon->id = entry.id;
        addon->state = EAddonState::Failed;
        if (has_folder)
        {
            addon->reason = "listed in appdata/addons.ltx, but the folder has no addon.ltx";
            Logf("! ", AddonTag(*addon).c_str(), "%s: %s", addon->reason.c_str(), addon->dir_log.c_str());
        }
        else
        {
            addon->reason = "listed in appdata/addons.ltx, but its folder cannot be resolved: " + why;
            Logf("! ", AddonTag(*addon).c_str(), "%s", addon->reason.c_str());
        }
        g_addons->push_back(std::move(addon));
        return;
    }
    AddFoundAddon(std::move(addon), entry.id);
}

// ---------------------------------------------------------------------------------------------
// [addon] depends: presence, versions, load order
// ---------------------------------------------------------------------------------------------

// Why the dependency does not hold right now, "" when it does.
xr_string DependencyProblem(const AddonDependency& dependency)
{
    const AddonRecord* found = FindAddonById(dependency.id);
    if (!found)
    {
        // The copy that took the id failed (a skipped duplicate of it is no replacement): name its reason
        for (const auto& addon : *g_addons)
        {
            if (addon->state == EAddonState::Failed && addon->id == dependency.id)
                return "dependency '" + dependency.id + "' failed (" + addon->reason + ")";
        }
        return "missing dependency '" + dependency.id + "'";
    }
    if (found->state == EAddonState::Disabled)
        return "dependency '" + dependency.id + "' is disabled";
    if (dependency.conditions.empty())
        return "";
    if (!found->version_valid)
    {
        return "dependency '" + dependency.id + "' has version '" + found->version +
            "' (not MAJOR.MINOR.PATCH), needed: " + dependency.text;
    }
    for (const VersionCondition& condition : dependency.conditions)
    {
        if (!VersionConditionHolds(condition, found->version_parsed))
        {
            return "dependency '" + dependency.id + "' is version " + AddonVersionString(found->version_parsed) +
                ", needed: " + dependency.text;
        }
    }
    return "";
}

// false (the addon is failed, logged) when a dependency does not hold.
bool CheckDependencies(AddonRecord& addon)
{
    for (const AddonDependency& dependency : addon.depends)
    {
        const xr_string problem = DependencyProblem(dependency);
        if (problem.empty())
            continue;
        addon.state = EAddonState::Failed;
        addon.reason = problem;
        Logf("! ", AddonTag(addon).c_str(), "failed: %s", problem.c_str());
        return false;
    }
    return true;
}

// Before any plugin loads. The addons that can load get the order: every dependency before the addons that need
// it, otherwise the order of discovery (appdata/addons.ltx, or the search roots and folder names). The others
// (failed, skipped, disabled) follow in the order found. Shutdown goes backwards, so dependents unload first.
void ResolveDependencies()
{
    // A failed addon fails the ones that need it: repeat until nothing changes
    for (bool changed = true; changed;)
    {
        changed = false;
        for (auto& addon : *g_addons)
        {
            if (addon->state == EAddonState::Discovered && !CheckDependencies(*addon))
                changed = true;
        }
    }

    xr_vector<xr_unique_ptr<AddonRecord>>& all = *g_addons;
    xr_vector<xr_unique_ptr<AddonRecord>> ordered;
    ordered.reserve(all.size());
    const auto is_placed = [&](const xr_string& id) {
        return std::any_of(ordered.begin(), ordered.end(), [&](const xr_unique_ptr<AddonRecord>& addon) {
            return addon->id == id;
        });
    };
    // The earliest addon whose dependencies are all placed goes next (a stable topological order)
    for (bool placed = true; placed;)
    {
        placed = false;
        for (auto& addon : all)
        {
            if (!addon || addon->state != EAddonState::Discovered)
                continue;
            const bool ready = std::all_of(addon->depends.begin(), addon->depends.end(),
                [&](const AddonDependency& dependency) { return is_placed(dependency.id); });
            if (!ready)
                continue;
            ordered.push_back(std::move(addon));
            placed = true;
            break;
        }
    }

    // What is left waits for itself: a dependency cycle (or an addon that needs one of the cycle)
    xr_string waiting;
    for (const auto& addon : all)
    {
        if (addon && addon->state == EAddonState::Discovered)
            waiting += (waiting.empty() ? "" : ", ") + addon->id;
    }
    for (auto& addon : all)
    {
        if (!addon)
            continue;
        if (addon->state == EAddonState::Discovered)
        {
            addon->state = EAddonState::Failed;
            addon->reason = "dependency cycle among: " + waiting;
            Logf("! ", AddonTag(*addon).c_str(), "failed: %s", addon->reason.c_str());
        }
        ordered.push_back(std::move(addon));
    }
    all.swap(ordered);
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
        const xr_string name = ToUtf8(it->path().filename()); // string() throws on a stray non-ANSI file name
        if (name.rfind(prefix, 0) == 0 && it->path() != shadow)
        {
            std::error_code rm_ec;
            fs::remove(it->path(), rm_ec);
        }
    }

    // The PDB next to the copy under the name the library refers to (<name>.pdb), and the folder in the dbghelp
    // search path: a stack trace of a crash names the functions of the plugin. Copied while no copy of the plugin is
    // loaded (dbghelp keeps the file open until the module leaves it); a failed copy only costs the names.
    const fs::path pdb = original.parent_path() / (addon.plugin_name + ".pdb").c_str();
    if (fs::is_regular_file(pdb, ec))
    {
        std::error_code pdb_ec;
        fs::copy_file(pdb, cache_dir / pdb.filename(), fs::copy_options::update_existing, pdb_ec);
        if (pdb_ec && g_debug_log)
            Logf("  ", AddonTag(addon).c_str(), "the PDB is not copied next to the library: %s", pdb_ec.message().c_str());
    }
    xrDebug::AddSymbolSearchPath(ToNarrow(cache_dir).c_str());

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

// gwp_plugin_init and on_unload run under the guards of the host, like every other plugin function: a crash in them
// costs the plugin, not the game
struct InitCall
{
    GwpPluginInitFn init = nullptr;
    const GwpPlugin* self = nullptr;
    GwpPluginDesc* desc = nullptr;
    GwpResult result = GWP_ERROR;
};

void InitThunk(void* context)
{
    InitCall& call = *static_cast<InitCall*>(context);
    call.result = call.init(&g_engine_api, call.self, call.desc);
}

struct UnloadCall
{
    void(GWP_CALL* on_unload)(void* user) = nullptr;
    void* user = nullptr;
};

void UnloadThunk(void* context)
{
    UnloadCall& call = *static_cast<UnloadCall*>(context);
    call.on_unload(call.user);
}

// Every registration of the plugin leaves the groups of the engine. Safe inside a dispatch (a crash): each group
// marks or defers what is running right now - the subscribers, binders, callbacks and exports; the proxies of its
// native evaluators and actions call Lua from now on; its planners stop calling. Nothing of the library is called
// afterwards.
void StopPlugin(const GwpPlugin* plugin)
{
    events::RemovePluginSubscriptions(plugin);
    binders::RemovePluginBinders(plugin);
    callbacks::RemovePluginCallbacks(plugin);
    goap::RemovePluginEvaluators(plugin);
    goap::StopPluginPlanners(plugin);
    RemovePluginExports(plugin);
    services::RemovePluginServices(plugin);
    feedback::RemovePluginResources(plugin); // its sounds, particle effects and effectors
}

// StopPlugin and the rest, right before the library goes: the planners are destroyed, the variables and ini files
// of the plugin are released (the values are kept for its next load), the crash marks of the groups are dropped for
// the next instance, the module leaves dbghelp and the process. No plugin code may be on the stack.
void ReleasePlugin(AddonRecord& addon)
{
    const GwpPlugin* plugin = &addon.handle;
    StopPlugin(plugin);
    goap::RemovePluginPlanners(plugin);
    console::RemovePluginCvars(plugin);
    ini::ClosePluginFiles(plugin);
    binders::ForgetPluginCrash(plugin);
    goap::ForgetPluginCrash(plugin);
    if (addon.module_base)
        xrDebug::ForgetModuleSymbols(addon.module_base);
    addon.module_base = nullptr;
    addon.module.reset();
    addon.desc = {};
}

// on_unload under the guards, then ReleasePlugin. A plugin that crashes in on_unload is unloaded all the same.
void UnloadPlugin(AddonRecord& addon)
{
    // The services go first: a consumer may call the interface from its service_on_unregister handler, and after
    // on_unload the state behind it is gone (gwp::Plugin deletes the plugin object there)
    services::RemovePluginServices(&addon.handle);
    if (addon.desc.on_unload)
    {
        UnloadCall call{ addon.desc.on_unload, addon.desc.user };
        if (!events::CallPluginGuarded(reinterpret_cast<const void*>(call.on_unload), "on_unload", &UnloadThunk, &call))
            Logf("! ", AddonTag(addon).c_str(), "plugin '%s' crashed in on_unload, unloaded anyway",
                addon.plugin_name.c_str());
    }
    ReleasePlugin(addon);
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
#if defined(XR_PLATFORM_WINDOWS)
    HMODULE module_base = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(init), &module_base);
    addon.module_base = module_base;
#endif

    // The plugin may be built against a newer minor version whose GwpPluginDesc is longer than ours.
    // It writes into a buffer of GWP_PLUGIN_DESC_MAX_SIZE bytes (the contract in gwp_api.h), never onto our struct.
    alignas(std::max_align_t) uint8_t desc_buffer[GWP_PLUGIN_DESC_MAX_SIZE] = {};
    GwpPluginDesc& desc = *reinterpret_cast<GwpPluginDesc*>(desc_buffer);
    InitCall call{ init, &addon.handle, &desc, GWP_ERROR };
    addon.loading = true;
    addon.load_crash.clear();
    const bool init_ran = events::CallPluginGuarded(reinterpret_cast<const void*>(init), GWP_PLUGIN_ENTRY_NAME,
        &InitThunk, &call);
    addon.loading = false;
    const GwpResult result = init_ran && addon.load_crash.empty() ? call.result : GWP_ERROR;
    if (result != GWP_OK)
    {
        ReleasePlugin(addon); // the plugin may have registered things before failing; on_unload is not called
        string64 code;
        xr_sprintf(code, "%d", static_cast<int>(result));
        addon.reason = !init_ran ? xr_string(GWP_PLUGIN_ENTRY_NAME) + " crashed"
            : !addon.load_crash.empty() ? "crashed in " + addon.load_crash + " during " + GWP_PLUGIN_ENTRY_NAME
                                        : xr_string(GWP_PLUGIN_ENTRY_NAME) + " returned " + code;
        MarkPluginUnavailable(addon);
        return;
    }

    // With a mismatching header the rest of the structure cannot be trusted (another layout), so on_unload is
    // deliberately not called before unloading. Documented in wiki/doc/plugins/api/entry_point.md.
    // api_built (the header version the plugin was built with) and the reserved field after it exist only in a
    // description long enough to hold them. api_min == 0 means "the version I was built with". A description
    // without api_built comes from a header older than the current layout of GwpEvent / GwpExportFn (before the
    // release, under the same version 0.1.0): such a binary would crash in its first handler, so it is refused.
    const bool has_built = desc.size >= offsetof(GwpPluginDesc, api_built) + sizeof(uint32_t);
    const bool has_reserved = desc.size >= offsetof(GwpPluginDesc, reserved) + sizeof(uint32_t);
    const uint32_t api_built = has_built ? desc.api_built : 0u;
    const uint32_t api_min = desc.api_min != 0 ? desc.api_min : api_built;
    const bool header_ok = desc.size >= 3 * sizeof(uint32_t) && desc.size <= GWP_PLUGIN_DESC_MAX_SIZE &&
        desc.abi_major == GWP_API_VERSION_MAJOR && has_built && api_built != 0 && api_min <= GWP_API_VERSION &&
        (!has_reserved || desc.reserved == 0);
    if (!header_ok)
    {
        ReleasePlugin(addon);
        addon.reason = "plugin header mismatch: abi " + VersionString(GWP_MAKE_VERSION(desc.abi_major, 0, 0)) +
            ", requires api " + VersionString(api_min) + ", engine api " + VersionString(GWP_API_VERSION) +
            (has_reserved && desc.reserved != 0 ? ", non-zero reserved field" : "") +
            (!has_built || api_built == 0 ? ", no api_built (built with a pre-release header: rebuild the plugin)" : "");
        MarkPluginUnavailable(addon);
        return;
    }

    // Copy only the part of the description the plugin actually filled (older plugins have a shorter struct).
    addon.desc = {};
    memcpy(&addon.desc, &desc, std::min<size_t>(desc.size, sizeof(GwpPluginDesc)));
    addon.plugin_state = EPluginState::Loaded;
    addon.state = EAddonState::Active;
    addon.reason.clear();
    Logf("* ", AddonTag(addon).c_str(), "plugin '%s' loaded (built with Plugin API %s, engine %s): %s",
        addon.plugin_name.c_str(), VersionString(api_built).c_str(),
        VersionString(GWP_API_VERSION).c_str(), addon.plugin_path.c_str());
    // Services published during init are announced only now: a failed init removes them silently
    services::AnnouncePluginServices(&addon.handle);
}

// Unloads what is loaded (a crashed plugin too) and loads the library again: the copy is made anew, so a library
// rebuilt meanwhile is the one loaded. No plugin code may be on the stack.
void ReloadPlugin(AddonRecord& addon)
{
    if (addon.module)
        UnloadPlugin(addon);
    addon.plugin_state = EPluginState::None;
    LoadPlugin(addon);
}
} // namespace detail

void Initialize()
{
    using namespace detail;

    if (g_initialized)
        return;
    g_initialized = true;
    g_host_thread_id = std::this_thread::get_id();
    g_debug_log = strstr(Core.Params, "-addon_debug") != nullptr;
    g_addons = xr_new<xr_vector<xr_unique_ptr<AddonRecord>>>();
    FillEngineApi();

    xr_vector<xr_string> roots;
    CollectCommandLineRoots(roots); // -addon_dir: development folders, always enabled, win on a duplicate id
    xr_vector<xr_string> game_roots;
    CollectGameAddonsRoot(game_roots); // <game>\addons\ ($game_addons$ in fsgame.ltx)
    const xr_string game_root = game_roots.empty() ? xr_string() : game_roots.front();
    if (!game_root.empty())
    {
        // -addon_dir naming the addons folder of the game itself stays the game folder: appdata/addons.ltx applies
        // to it, instead of every addon there working as a development one and the list giving only duplicates
        const auto is_game_root = [&](const xr_string& root) { return SameRoot(root, game_root); };
        const auto removed = std::remove_if(roots.begin(), roots.end(), is_game_root);
        if (removed != roots.end())
        {
            roots.erase(removed, roots.end());
            Logf("~ ", "addons", "-addon_dir %s is the addons folder of the game: read as the game folder (after the "
                "other -addon_dir folders, by appdata/addons.ltx when there is one)", game_root.c_str());
        }
    }
    const size_t dev_roots = roots.size();
    if (!game_root.empty())
        roots.push_back(game_root);

    // appdata/addons.ltx from the launcher: when it is there, it decides which addons of the game folder work, in
    // what order and with what trust; the -addon_dir roots stay as they are
    xr_vector<LauncherEntry> list;
    const bool has_list = ReadLauncherList(list);

    Logf("* ", "addons", "Plugin API %s, platform %s/%s, %u search root(s)%s",
        VersionString(GWP_API_VERSION).c_str(), kPluginOs, kPluginArch, static_cast<u32>(roots.size()),
        has_list ? ", list appdata/addons.ltx" : "");

    for (size_t i = 0; i < roots.size(); ++i)
    {
        if (has_list && i >= dev_roots)
            break; // the game folder: below, through the list
        if (g_debug_log)
            Logf("  ", "addons", "search root: %s", roots[i].c_str());
        DiscoverInRoot(roots[i]);
    }
    if (has_list)
    {
        Logf("* ", "addons", "%s: %u addon(s) listed", kLauncherListLog, static_cast<u32>(list.size()));
        for (const LauncherEntry& entry : list)
            AddListedAddon(entry, game_root);
        if (roots.size() > dev_roots)
            DiscoverInRoot(roots.back(), true); // the folders the list does not name: disabled, reported
    }

    // Dependencies and the load order, before any plugin: a dependency loads before the addons that need it
    ResolveDependencies();
    // Every id is known now, also of the addons that will not load: names "<id>_..." belong to them (console)
    g_known_ids.clear();
    for (const auto& addon : *g_addons)
    {
        if (IsValidId(addon->id) && std::find(g_known_ids.begin(), g_known_ids.end(), addon->id) == g_known_ids.end())
            g_known_ids.push_back(addon->id);
    }

    console::LoadCvarsFile(); // appdata/plugins.ltx: the saved values, before the plugins register variables
    for (auto& addon : *g_addons)
    {
        // The dependencies went first: one that failed meanwhile (its required plugin) fails the addons that need it
        if (addon->state == EAddonState::Discovered && CheckDependencies(*addon))
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
        if (!addon.module)
            continue; // no plugin, not loaded, or a crashed one unloaded already
        // on_unload, then everything that points into the library: handlers, binder vtables, callbacks, the
        // evaluator and action proxies (back to Lua), planners, exports, variables and ini files of the plugin
        UnloadPlugin(addon);
        addon.plugin_state = EPluginState::Unavailable;
    }
    xr_delete(g_addons);
    g_known_ids.clear();
    g_save_data.clear();
    data::Shutdown();
    ini::Shutdown();
    console::Shutdown();
    timers::Shutdown();
    binders::Shutdown();
    storage::Shutdown();
    callbacks::Shutdown();
    goap::Shutdown();
    goap::ShutdownPlanners();
    ShutdownNpcApi();
    ShutdownExports();
    services::Shutdown();
    feedback::Shutdown();
    events::Shutdown();
    g_initialized = false;
}

void ResetSaveData()
{
    detail::g_save_data.clear();
    data::ResetPersistent();
    timers::Reset();
    binders::ResetSave();
}

void WriteSaveData(IWriter& stream)
{
    using namespace detail;
    // First: it calls on_save of the bound objects, and a save_write made there must land in the chunk below
    binders::WriteSave(stream);
    stream.open_chunk(kSaveChunkId);
    stream.w_u32(kSaveFormat);
    stream.w_u32(static_cast<u32>(g_save_data.size()));
    for (const auto& [id, chunk] : g_save_data)
    {
        stream.w_stringZ(id.c_str());
        stream.w_u32(chunk.version);
        stream.w_u32(static_cast<u32>(chunk.data.size()));
        stream.w(chunk.data.data(), chunk.data.size());
    }
    stream.close_chunk();
    data::WriteSave(stream); // persistent values of the data bus: own chunk
    timers::WriteSave(stream); // persistent timers: own chunk
}

bool ReadStringZChecked(IReader& reader, xr_string& out)
{
    const auto* begin = static_cast<const char*>(reader.pointer());
    const intptr_t left = reader.elapsed();
    const void* end = left > 0 ? memchr(begin, 0, static_cast<size_t>(left)) : nullptr;
    if (!end)
        return false;
    out.assign(begin, static_cast<const char*>(end));
    reader.advance(out.size() + 1);
    return true;
}

void ReadSaveData(IReader& stream)
{
    using namespace detail;
    data::ReadSave(stream); // persistent values of the data bus: own chunk
    timers::ReadSave(stream); // persistent timers: own chunk
    binders::ReadSave(stream); // kept state of bound objects: own chunk
    g_save_data.clear();
    IReader* reader = stream.open_chunk(kSaveChunkId);
    if (!reader)
        return; // save made by a build without plugin data
    const auto has = [reader](size_t bytes) { return reader->elapsed() >= static_cast<intptr_t>(bytes); };

    const u32 format = has(sizeof(u32)) ? reader->r_u32() : 0;
    if (format != kSaveFormat)
    {
        Logf("! ", "addons", "plugin save data: format %u is not supported, ignored", format);
        reader->close();
        return;
    }
    const u32 count = has(sizeof(u32)) ? reader->r_u32() : 0;
    for (u32 i = 0; i < count; ++i)
    {
        xr_string id;
        if (!ReadStringZChecked(*reader, id) || !has(2 * sizeof(u32)))
        {
            Logf("! ", "addons", "plugin save data is truncated, the rest is ignored");
            break;
        }
        SaveChunk chunk;
        chunk.version = reader->r_u32();
        const u32 size = reader->r_u32();
        if (size > kMaxSaveChunk || !has(size))
        {
            Logf("! ", "addons", "plugin save data of '%s' is truncated, the rest is ignored", id.c_str());
            break;
        }
        chunk.data.resize(size);
        reader->r(chunk.data.data(), size);
        g_save_data[id] = std::move(chunk);
    }
    reader->close();
    if (g_debug_log)
        Logf("  ", "addons", "plugin save data: %u chunk(s) loaded", static_cast<u32>(g_save_data.size()));
}

void OnPluginCrashed(const GwpPlugin* plugin, pcstr what)
{
    using namespace detail;
    AddonRecord* addon = plugin ? plugin->addon : nullptr;
    if (!addon)
        return;
    const pcstr where = what && what[0] ? what : "?";
    if (addon->loading)
    {
        // A callback the plugin got while it registered (on_spawn of an online object): the load fails
        if (addon->load_crash.empty())
            addon->load_crash = where;
        StopPlugin(plugin);
        return;
    }
    if (addon->plugin_state == EPluginState::Crashed)
    {
        // Stopped already: a second crash before the unload. An outer frame of the plugin may have subscribed or
        // registered again after the first stop - that goes too (StopPlugin may run any number of times)
        StopPlugin(plugin);
        return;
    }
    if (addon->plugin_state != EPluginState::Loaded)
        return;
    addon->plugin_state = EPluginState::Crashed;
    addon->reason = xr_string("crashed in ") + where;
    StopPlugin(plugin);
    g_plugin_work = true;
    const u32 left = addon->restarts < kMaxAutoRestarts ? kMaxAutoRestarts - addon->restarts : 0;
    Logf("! ", AddonTag(*addon).c_str(),
        "plugin '%s' crashed in %s and is stopped, what it took over goes back to Lua. The library is unloaded at the "
        "next frame and loaded again at the next game load (%u automatic restart(s) left in this run), or now with "
        "the console command plugin_reload %s. NPCs it already ran may misbehave until a save is loaded",
        addon->plugin_name.c_str(), where, left, addon->id.c_str());
}

void OnFrame()
{
    using namespace detail;
    // Sounds and effects of plugins: the ended ones and those of a gone level are released, attached effects follow
    feedback::OnFrame();
    // Only at the top of a frame: a plugin function on the stack would return into an unloaded library
    if (!g_plugin_work || !g_addons || events::PluginCallDepth() != 0)
        return;
    g_plugin_work = false;
    for (auto& entry : *g_addons)
    {
        AddonRecord& addon = *entry;
        if (addon.reload_requested)
        {
            addon.reload_requested = false;
            Logf("* ", AddonTag(addon).c_str(), "plugin_reload: reloading plugin '%s'", addon.plugin_name.c_str());
            ReloadPlugin(addon);
            if (addon.plugin_state == EPluginState::Loaded && g_pGameLevel)
            {
                Logf("~ ", AddonTag(addon).c_str(), "plugin '%s' reloaded inside a game: it got no alife_on_start of "
                    "this game. The objects online keep what the old instance gave them, unless the plugin sets them "
                    "up again itself (gw_npc does); otherwise load a save", addon.plugin_name.c_str());
            }
            continue;
        }
        if (addon.plugin_state == EPluginState::Crashed && addon.module)
        {
            UnloadPlugin(addon);
            Logf("~ ", AddonTag(addon).c_str(), "plugin '%s' unloaded after its crash", addon.plugin_name.c_str());
        }
    }
}

void OnGameStart()
{
    using namespace detail;
    // Sounds, effects and effectors of plugins belong to the previous game
    feedback::OnGameStart();
    if (!g_addons)
        return;
    for (auto& entry : *g_addons)
    {
        AddonRecord& addon = *entry;
        if (addon.plugin_state != EPluginState::Crashed)
            continue;
        if (events::PluginCallDepth() != 0)
        {
            // The game started from inside a plugin function: its library cannot go now, the next load tries
            g_plugin_work = true;
            continue;
        }
        if (addon.restarts >= kMaxAutoRestarts)
        {
            if (!addon.restart_refused_logged)
            {
                addon.restart_refused_logged = true;
                Logf("! ", AddonTag(addon).c_str(), "plugin '%s' stays stopped: it crashed after %u automatic "
                    "restart(s) in this run (console: plugin_reload %s)", addon.plugin_name.c_str(), addon.restarts,
                    addon.id.c_str());
            }
            continue;
        }
        ++addon.restarts;
        Logf("* ", AddonTag(addon).c_str(), "plugin '%s' is loaded again (it %s; restart %u of %u)",
            addon.plugin_name.c_str(), addon.reason.c_str(), addon.restarts, kMaxAutoRestarts);
        ReloadPlugin(addon);
    }
}

bool RequestPluginReload(pcstr addon_id, xr_string& reason)
{
    using namespace detail;
    if (!g_addons || !addon_id || !addon_id[0])
    {
        reason = "no addon id";
        return false;
    }
    xr_string refusal; // the addon is there, but no copy of it is a target: why
    for (auto& entry : *g_addons)
    {
        AddonRecord& addon = *entry;
        if (!EqualsNoCase(addon.id, addon_id))
            continue;
        // A duplicate, a disabled addon or one with a broken manifest (its plugin name may be unchecked) is no
        // reload target; an addon failed by its required plugin is, as a plugin that did not load at the start.
        // A failed dependency leaves plugin_state None: no target either, its plugin was never tried.
        if (addon.state == EAddonState::Disabled && refusal.empty())
            refusal = "the addon is disabled: " + addon.reason;
        else if (addon.state == EAddonState::Failed && addon.plugin_state != EPluginState::Unavailable &&
            refusal.empty())
            refusal = "the addon failed: " + addon.reason;
        if (addon.state == EAddonState::Skipped || addon.state == EAddonState::Disabled ||
            addon.state == EAddonState::Discovered ||
            (addon.state == EAddonState::Failed && addon.plugin_state != EPluginState::Unavailable))
            continue;
        if (addon.plugin_name.empty())
        {
            reason = "the addon has no plugin";
            return false;
        }
        addon.reload_requested = true;
        g_plugin_work = true;
        return true;
    }
    reason = refusal.empty() ? xr_string("no such addon (addon_list shows them)") : refusal;
    return false;
}

pcstr PluginAddonId(const GwpPlugin* plugin)
{
    const detail::AddonRecord* addon = detail::AddonOf(plugin);
    return addon ? addon->id.c_str() : "?";
}

bool IsMainThread() { return detail::ApiIsMainThread() != 0; }
bool IsDebugLog() { return detail::g_debug_log; }

const xr_vector<xr_string>& KnownAddonIds() { return detail::g_known_ids; }

pcstr NameOwnerAddonId(pcstr name)
{
    if (!name)
        return nullptr;
    const xr_string* owner = nullptr;
    for (const xr_string& id : detail::g_known_ids)
    {
        if (strncmp(name, id.c_str(), id.size()) == 0 && name[id.size()] == '_' &&
            (!owner || id.size() > owner->size()))
            owner = &id;
    }
    return owner ? owner->c_str() : nullptr;
}

bool IsPluginLoading(const GwpPlugin* plugin)
{
    const detail::AddonRecord* addon = detail::AddonOf(plugin);
    return addon && addon->loading;
}

bool IsPluginRunning(const GwpPlugin* plugin)
{
    const detail::AddonRecord* addon = detail::AddonOf(plugin);
    return addon && (addon->loading || addon->plugin_state == detail::EPluginState::Loaded);
}

bool IsPluginLoaded(pcstr addon_id)
{
    if (!detail::g_addons || !addon_id)
        return false;
    for (const auto& addon : *detail::g_addons)
    {
        if (addon->plugin_state == detail::EPluginState::Loaded && addon->id == addon_id)
            return true;
    }
    return false;
}

u32 PluginApiBuilt(pcstr addon_id)
{
    if (!detail::g_addons || !addon_id)
        return 0;
    for (const auto& addon : *detail::g_addons)
    {
        // addon.desc holds only what the plugin filled (LoadPlugin): api_built stays 0 in a shorter description
        if (addon->plugin_state == detail::EPluginState::Loaded && addon->id == addon_id)
            return addon->desc.api_built;
    }
    return 0;
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
        Msg("-   %-24s %-10s %-10s type=%s plugin=%s (%s) trust=%s dir=%s%s%s", addon->id.c_str(),
            addon->version.empty() ? "-" : addon->version.c_str(), AddonStateName(addon->state),
            addon->type_name.empty() ? "-" : addon->type_name.c_str(),
            addon->plugin_name.empty() ? "-" : addon->plugin_name.c_str(),
            PluginStateName(addon->plugin_state), addon->trust.c_str(), addon->dir_log.c_str(),
            addon->reason.empty() ? "" : " reason=", addon->reason.c_str());
    }
    services::PrintList();
}
} // namespace gw::addons
