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
#include "addon_alife_hooks.h"
#include "addon_binders.h"
#include "addon_callbacks.h"
#include "addon_data_bus.h"
#include "addon_goap.h"
#include "addon_goap_planner.h"
#include "addon_storage.h"
#include "addon_event_bus.h"
#include "addon_timers.h"
#include "addon_lua_entry.h"
#include "addon_slots.h"
#include "MainMenu.h"

#include "xrAddonHost/include/gwp/gwp_api.h"
#include "xrCore/ContentMerge/ContentMerge.h" // Sha256Hex: plugin_sha256, plugin_consent (CheckPluginLibrary)
#include "xrCore/ModuleLookup.hpp"
#include "xrEngine/EngineThreading.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <system_error>
#include <thread>

#if !defined(XR_PLATFORM_WINDOWS)
#include <dlfcn.h> // dlsym: FindLibrarySymbol
#include <fcntl.h> // open: PluginFileLock
#include <sys/stat.h> // fstat: PluginFileLock
#include <unistd.h> // getpid, read, close
#endif

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

fs::path PathFromUtf8(const xr_string& text)
{
#if defined(XR_PLATFORM_WINDOWS)
    const int size = static_cast<int>(text.size());
    const int length = size ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), size, nullptr, 0) : 0;
    if (length <= 0)
        return fs::path(text.c_str()); // empty, or not UTF-8: the ANSI code page
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), size, &wide[0], length);
    return fs::path(wide);
#else
    // The engine FS keeps '\' in its paths and converts them only when it calls the OS (convert_path_separators):
    // on POSIX '\' is an ordinary character of a file name
    xr_string native = text;
    std::replace(native.begin(), native.end(), '\\', '/');
    return fs::path(native.c_str());
#endif
}

xr_string PathToUtf8(const fs::path& path)
{
    // std::u8string in C++20 (MSVC build), std::string in C++17 (CMake build).
    const auto u8 = path.u8string();
    return xr_string(reinterpret_cast<const char*>(u8.c_str()), u8.size());
}

xr_string ErrorText(const std::error_code& ec)
{
    xr_string text;
#if defined(XR_PLATFORM_WINDOWS)
    // The system errors (std::filesystem gives them) through FormatMessageW: ec.message() is in the ANSI code page
    if (ec.category() == std::system_category())
    {
        wchar_t* buffer = nullptr;
        const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(ec.value()), 0,
            reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
        if (length && buffer)
        {
            const int size = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), nullptr, 0, nullptr,
                nullptr);
            if (size > 0)
            {
                text.resize(static_cast<size_t>(size));
                WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), &text[0], size, nullptr, nullptr);
            }
        }
        if (buffer)
            LocalFree(buffer);
    }
    if (text.empty())
    {
        // Another category, or no text of the system: the message of the library, from the ANSI code page
        const std::string ansi = ec.message();
        const int size = static_cast<int>(ansi.size());
        const int wide_length = size ? MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), size, nullptr, 0) : 0;
        if (wide_length > 0)
        {
            std::wstring wide(static_cast<size_t>(wide_length), L'\0');
            MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), size, &wide[0], wide_length);
            const int length = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wide_length, nullptr, 0, nullptr,
                nullptr);
            if (length > 0)
            {
                text.resize(static_cast<size_t>(length));
                WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wide_length, &text[0], length, nullptr, nullptr);
            }
        }
    }
#else
    text = ec.message().c_str();
#endif
    // FormatMessage ends the text with a period and a line end
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '.'))
        text.pop_back();
    if (text.empty())
    {
        string32 code;
        xr_sprintf(code, "error %d", ec.value());
        text = code;
    }
    return text;
}

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
    Inactive,    // on demand (UnloadOnDemandPlugins): waits for its location pack / slot provider to become active
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
    // [addon] load_after: the ids it loads after when they are there (ResolveDependencies), the valid ones only
    xr_vector<xr_string> load_after;
    // [addon] patches: the addons this compatibility patch is made for (15-merge-implementation.md 6.1): it works only
    // while all of them do (CheckPatchTargets) and loads after them, as after its dependencies
    xr_vector<xr_string> patches;
    // [addon] game_version: the release tag of the game the replacements are made for (15-merge-implementation.md
    // 2.2), as written; empty - not set. Read by addon_merge.cpp
    xr_string game_version;
    // [addon] api_min (GWP_MAKE_VERSION): the Addons API the whole addon needs (11-catalog-distribution.md 6); 0 - not
    // set (or 0.0.0): no requirement. Checked by CheckAddonApi
    u32 addon_api_min = 0;
    // [location] of a location pack (plans/addons_api_support/10-location-packs.md): read by level_pack.cpp
    xr_vector<std::pair<xr_string, xr_string>> location;
    // [private] of addon.ltx (plans/addons_api_support/14-addon-resource-scoping.md): masks of the content files
    // relative to gamedata\ mounted only under the alias roots of the addon (addon_content.cpp)
    xr_vector<xr_string> private_masks;
    // [lua] entry (plans/addons_api_support/08-unified-addons-api-lua-native.md, 4.2): read by addon_lua_entry.cpp
    xr_string lua_entry;
    // [slot.<slot>] and [slots_declare] (plans/addons_api_support/13-extension-model.md, 4.3): read by addon_slots.cpp
    xr_vector<AddonInfo::SlotSection> slots;
    xr_vector<std::pair<xr_string, xr_string>> slots_declare;
    // The plugin loads on demand (00-roadmap.md 5a, decision 1): type location or a slot provider
    bool plugin_on_demand = false;
    // appdata/addons.ltx (11-catalog-distribution.md 8.6): first_party | index | local. The plugin of index is not
    // loaded (v1, PluginTrustAllows), the plugin of a listed local one needs plugin_consent (CheckPluginLibrary)
    xr_string trust = "local";
    // appdata/addons.ltx, 64 lower case hex digits, empty - not given: the SHA-256 the plugin library must have
    // (plugin_sha256) and the one the player agreed to load for a local addon (plugin_consent). CheckPluginLibrary
    xr_string plugin_sha256;
    xr_string plugin_consent;
    // The key is there, but its value is not 64 hex digits: a hash that cannot match - the plugin is not loaded
    // (CheckPluginLibrary; plugin_consent only for a listed local addon, the others do not use it)
    bool plugin_sha256_malformed = false;
    bool plugin_consent_malformed = false;
    bool listed = false; // the record comes from appdata/addons.ltx (not from -addon_dir or a scan without the list)
    fs::path dir_path;      // addon folder; use for every file system operation
    fs::path manifest_path; // dir_path / addon.ltx
    xr_string dir;          // dir_path as UTF-8 with trailing separator: addon_dir() of the plugins, the log, the console

    EAddonState state = EAddonState::Discovered;
    xr_string reason;

    // [plugin] section of addon.ltx
    xr_string plugin_name; // empty when the addon has no plugin
    u32 plugin_api_min = 0;
    bool plugin_required = false;

    EPluginState plugin_state = EPluginState::None;
    xr_string plugin_path; // original library path (UTF-8, for messages)
    xr_string shadow_path; // loaded copy (UTF-8, for messages)
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
    bool addon_reload_requested = false; // addon_reload: at the next frame (ReloadAddon)
};

// Heap-allocated on purpose: if Shutdown() is never called, a static vector would unload the plugin libraries
// from the xrGame DllMain(DLL_PROCESS_DETACH), which is not allowed. Leaking them instead is harmless at exit.
xr_vector<xr_unique_ptr<AddonRecord>>* g_addons = nullptr;
GwpEngineApi g_engine_api{};
bool g_initialized = false;
bool g_debug_log = false;
// appdata/addons.ltx is there, but cannot be read or parsed (ReadLauncherList): the addon folders are scanned as
// without the list (the content and the Lua of the addons work), but no plugin is loaded - the list is what gives
// the hashes and the consent of the player, a broken one must not turn into "loaded unchecked" (LoadPlugin)
bool g_launcher_list_unreadable = false;
constexpr pcstr kLauncherListUnreadableReason = "appdata/addons.ltx cannot be read: plugins are not loaded";
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
    case EPluginState::Inactive: return "inactive";
    }
    return "?";
}

// Encodings: every path of the host is UTF-8, as the engine's FS, CInifile, the log, the Plugin API and
// SDL_LoadObject have them (PathFromUtf8 / PathToUtf8, addon_host.h). The one exception: the ANSI functions of
// dbghelp (xrDebug::AddSymbolSearchPath) take the path in the ANSI code page - path::string(), which throws on Windows
// for a character the code page lacks: such a path is given as UTF-8 then (dbghelp does not find the folder, the
// stack traces lose the names of the plugin, the game goes on).
xr_string ToAnsiForDbgHelp(const fs::path& path)
{
    try
    {
        return xr_string(path.string().c_str());
    }
    catch (const std::system_error&)
    {
        return PathToUtf8(path);
    }
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

xr_string VersionString(u32 version)
{
    string64 buf;
    xr_sprintf(buf, "%u.%u.%u", (version >> 22) & 0x3FFu, (version >> 12) & 0x3FFu, version & 0xFFFu);
    return buf;
}

// The "major" of an API version (GWP_MAKE_VERSION) by semver (11-catalog-distribution.md 6): the first number for
// 1.x.y and above, the pair 0.x for 0.x.y (before 1.0 the second number breaks). true - the one of the engine
// (GWP_API_VERSION, one version for the Lua API, the manifest and the plugins)
bool SameApiMajor(u32 version)
{
    const u32 major = (version >> 22) & 0x3FFu;
    const u32 minor = (version >> 12) & 0x3FFu;
    return major == GWP_API_VERSION_MAJOR && (GWP_API_VERSION_MAJOR != 0 || minor == GWP_API_VERSION_MINOR);
}

// The major of a version for the messages: "1.x", "0.2.x"
xr_string ApiMajorString(u32 version)
{
    const u32 major = (version >> 22) & 0x3FFu;
    string64 family;
    if (major == 0)
        xr_sprintf(family, "0.%u.x", (version >> 12) & 0x3FFu);
    else
        xr_sprintf(family, "%u.x", major);
    return family;
}

// An api_min of the manifest ([addon] or [plugin]) against the engine: compatible when it has the major of the engine
// (SameApiMajor) and is not newer than the engine; 0 (0.0.0) - not set, no requirement
enum class EApiFit
{
    Ok,
    Newer,      // the same major, newer than the engine: the game needs an update
    OtherMajor, // made for another major: the addon needs an update (or the game, when it is newer)
};

EApiFit ApiMinFit(u32 api_min)
{
    if (api_min == 0)
        return EApiFit::Ok;
    if (!SameApiMajor(api_min))
        return EApiFit::OtherMajor;
    return api_min <= GWP_API_VERSION ? EApiFit::Ok : EApiFit::Newer;
}

// What the player does about an api_min that does not fit (ApiMinFit): newer than the engine - a newer game; an older
// major - the version of the addon made for this game
pcstr ApiMinAdvice(u32 api_min)
{
    return api_min > GWP_API_VERSION ? "update the game, or install the version of the addon made for this one" :
                                       "install the version of the addon made for this game";
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

// [addon] api_min, [plugin] api_min: "MAJOR", "MAJOR.MINOR" or "MAJOR.MINOR.PATCH" - plain decimal numbers of up to 9
// digits, nothing before or after them (the ini reader trims the blanks), the missing parts are 0; within
// GWP_MAKE_VERSION (MAJOR, MINOR <= 1023, PATCH <= 4095). The grammar of parse_api of the launcher
// (launcher-new/src-tauri/src/service/addon_catalog.rs); the short forms are reported by ParseManifest. false for
// anything else ("1.2x", "1..2", "+1", "0x2", "1 2", an overflow): an error of the manifest
bool ParseVersion(const xr_string& text, u32& out)
{
    size_t pos = 0;
    AddonVersion version;
    if (!ReadAddonVersion(text, pos, 1, version) || pos != text.size() || version.major > 0x3FFu ||
        version.minor > 0x3FFu || version.patch > 0xFFFu)
        return false;
    out = GWP_MAKE_VERSION(version.major, version.minor, version.patch);
    return true;
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

    // The key is there, with a value or without one ("key =", a line without "=")
    bool has_key(pcstr section_name, pcstr key) const
    {
        const IniSection* section = find(section_name);
        if (!section)
            return false;
        return std::any_of(section->lines.begin(), section->lines.end(),
            [&](const IniLine& line) { return EqualsNoCase(line.key, key); });
    }

    bool get_bool(pcstr section_name, pcstr key, bool default_value) const
    {
        xr_string value;
        if (!get(section_name, key, value))
            return default_value;
        return IsTrueText(value);
    }

    static bool IsTrueText(const xr_string& value)
    {
        return EqualsNoCase(value, "true") || EqualsNoCase(value, "on") || EqualsNoCase(value, "yes") || value == "1";
    }

    static bool IsFalseText(const xr_string& value)
    {
        return EqualsNoCase(value, "false") || EqualsNoCase(value, "off") || EqualsNoCase(value, "no") || value == "0";
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
    alife_hooks::FillEngineApi(g_engine_api);
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
    return fs::equivalent(PathFromUtf8(a), PathFromUtf8(b), ec) && !ec;
}

void AddRoot(xr_vector<xr_string>& roots, const xr_string& root)
{
    const xr_string native = ToNativeSeparators(root); // -addon_dir may come with "\" too
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(PathFromUtf8(native), ec);
    // Roots stay in UTF-8, as the FS paths they come from (DiscoverInRoot reads them so)
    const xr_string normalized = ec ? native : PathToUtf8(canonical);
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

    // No type (or "type ="): plain content with a warning (00-roadmap.md 5a, decision 63), as before only the catalog
    // requires the field. type_name becomes "content": what addon_list, AddonInfo::type and the Lua ctx.type show
    if (!ini.get("addon", "type", addon.type_name))
    {
        addon.type_name = "content";
        if (warn)
        {
            Logf("~ ", tag.c_str(), "manifest has no [addon] type, treated as content (set type = <one of: %s>; "
                "the catalog requires it)", AddonTypeList().c_str());
        }
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
    // Reserved (plan 26, 4.3): read so that an addon written for a newer engine learns it is ignored here
    {
        xr_string value;
        if (warn && ini.get("addon", "conflicts", value))
            Logf("~ ", tag.c_str(), "[addon] conflicts is not supported by this engine yet, ignored");
    }
    // [addon] load_after = a, b: a soft order (wiki/doc/plugins/manifest.md, reserved fields) - the addon loads after
    // those of them that are there, a missing one is no error. The pair rule of the author (13-extension-model.md 4.2,
    // 00-roadmap.md 5a, decision 11): its files and patches win over theirs, as the one lower in the order
    addon.load_after.clear();
    xr_string load_after;
    if (ini.get("addon", "load_after", load_after))
    {
        for (size_t start = 0; start <= load_after.size();)
        {
            size_t comma = load_after.find(',', start);
            if (comma == xr_string::npos)
                comma = load_after.size();
            const xr_string id = Trim(load_after.substr(start, comma - start));
            start = comma + 1;
            if (id.empty())
                continue;
            if (!IsValidId(id) || id == addon.id)
            {
                if (warn)
                    Logf("~ ", tag.c_str(), "[addon] load_after '%s': %s, ignored", id.c_str(),
                        id == addon.id ? "the addon itself" : "not an addon id (a-z 0-9 _ , up to 64 chars)");
                continue;
            }
            if (std::find(addon.load_after.begin(), addon.load_after.end(), id) == addon.load_after.end())
                addon.load_after.push_back(id);
        }
    }
    // [addon] patches = a, b (15-merge-implementation.md 6.1; 00-roadmap.md 5a, decision 47): a compatibility patch of
    // these addons. Its fragments lie in <addon>\merge\gamedata\ (not mounted: the merge reads them, addon_merge.cpp);
    // it works only while all of them work and loads after them (ResolveDependencies). A new key: an addon without it
    // loads as before
    addon.patches.clear();
    xr_string patches;
    if (ini.get("addon", "patches", patches))
    {
        for (size_t start = 0; start <= patches.size();)
        {
            size_t comma = patches.find(',', start);
            if (comma == xr_string::npos)
                comma = patches.size();
            const xr_string id = Trim(patches.substr(start, comma - start));
            start = comma + 1;
            if (id.empty())
                continue;
            if (!IsValidId(id) || id == addon.id)
            {
                if (warn)
                    Logf("~ ", tag.c_str(), "[addon] patches '%s': %s, ignored", id.c_str(),
                        id == addon.id ? "the addon itself" : "not an addon id (a-z 0-9 _ , up to 64 chars)");
                continue;
            }
            if (std::find(addon.patches.begin(), addon.patches.end(), id) == addon.patches.end())
                addon.patches.push_back(id);
        }
    }
    // [addon] game_version = v1.3.0 (15-merge-implementation.md 2.2; 00-roadmap.md 5a, decision 47): the release tag
    // of the game the replacements of the files of the base are made for. A new key, nothing depends on it at the
    // load: the merge of the files compares it with the tag of the game (addon_merge.cpp)
    addon.game_version.clear();
    // "game_version =" is no value for get(): has_key tells it from no line at all (as the validator does)
    if (ini.get("addon", "game_version", addon.game_version))
        addon.game_version = Trim(addon.game_version);
    if (addon.game_version.empty() && warn && ini.has_key("addon", "game_version"))
        Logf("~ ", tag.c_str(), "[addon] game_version is empty, ignored (set the release tag of the game, v1.4.0, or "
            "remove the line)");
    // [addon] api_min = MAJOR.MINOR.PATCH (11-catalog-distribution.md 6): read as [plugin] api_min below - a value that
    // is no version is an error of the manifest, a short one ("1.2") is read with a warning. The compatibility with the
    // engine is checked before the dependencies (CheckAddonApi)
    addon.addon_api_min = 0;
    {
        xr_string api_min;
        if (ini.get("addon", "api_min", api_min) && !ParseVersion(api_min, addon.addon_api_min))
        {
            addon.reason = "invalid [addon] api_min '" + api_min + "' (expected MAJOR.MINOR.PATCH)";
            return false;
        }
        AddonVersion strict;
        if (warn && !api_min.empty() && !ParseAddonVersion(api_min, strict))
        {
            Logf("~ ", tag.c_str(), "[addon] api_min '%s' is not MAJOR.MINOR.PATCH, read as %s", api_min.c_str(),
                VersionString(addon.addon_api_min).c_str());
        }
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
        // No [plugin] api_min (or 0.0.0): the one of [addon] api_min (00-roadmap.md 5a, decision 63) - one version
        // for the whole addon. LoadPlugin checks it as if written in [plugin], and PluginBuiltVersionFits takes it as
        // the version of a library without gwp_plugin_api_built
        addon.plugin_api_min = 0;
        xr_string api_min;
        if (ini.get("plugin", "api_min", api_min) && !ParseVersion(api_min, addon.plugin_api_min))
        {
            addon.reason = "invalid [plugin] api_min '" + api_min + "' (expected MAJOR.MINOR.PATCH)";
            return false;
        }
        // ParseVersion also takes the short forms "1", "1.2" (as the launcher): kept for the plugins out there, only
        // reported
        AddonVersion strict;
        if (warn && !api_min.empty() && !ParseAddonVersion(api_min, strict))
        {
            Logf("~ ", tag.c_str(), "[plugin] api_min '%s' is not MAJOR.MINOR.PATCH, read as %s", api_min.c_str(),
                VersionString(addon.plugin_api_min).c_str());
        }
        if (addon.plugin_api_min == 0)
            addon.plugin_api_min = addon.addon_api_min;
        xr_string required;
        addon.plugin_required = ini.get("plugin", "required", required) && SimpleIni::IsTrueText(required);
        if (warn && !required.empty() && !addon.plugin_required && !SimpleIni::IsFalseText(required))
        {
            Logf("~ ", tag.c_str(), "[plugin] required = '%s' is not a boolean (true or false), taken as false",
                required.c_str());
        }
        // A key the engine does not read: a typo ("requried", "apimin") would silently be no requirement
        if (warn)
        {
            constexpr pcstr known_keys[] = { "name", "api_min", "required" };
            for (const IniLine& line : ini.find("plugin")->lines)
            {
                const bool known = std::any_of(std::begin(known_keys), std::end(known_keys),
                    [&](pcstr key) { return EqualsNoCase(line.key, key); });
                if (!known)
                    Logf("~ ", tag.c_str(), "[plugin] '%s' is not a key the engine reads (name, api_min, required), "
                        "ignored", line.key.c_str());
            }
        }
    }

    // [lua] entry = <module>: the Lua part of the addon (addon_lua_entry.h). A bad value leaves the addon without its
    // Lua part, the rest of it works
    addon.lua_entry.clear();
    if (ini.find("lua"))
    {
        xr_string entry;
        ini.get("lua", "entry", entry);
        const bool entry_valid = !entry.empty() && entry.size() <= 64 && !(entry[0] >= '0' && entry[0] <= '9') &&
            std::all_of(entry.begin(), entry.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
            });
        if (entry_valid)
        {
            addon.lua_entry = entry;
            // The convention of the names (09-content-addons-vfs.md 10.2, decision 3): a warning, no ban
            const xr_string prefix = addon.id + "_";
            if (warn && entry != addon.id && entry.compare(0, prefix.size(), prefix) != 0)
            {
                Logf("~ ", tag.c_str(), "[lua] entry '%s' does not start with '%s': a module of another addon or of "
                    "the game may have that name", entry.c_str(), prefix.c_str());
            }
        }
        else if (warn)
        {
            Logf("! ", tag.c_str(), "[lua] entry '%s' is not a Lua module name (A-Z a-z 0-9 _, not a digit first, up "
                "to 64 chars): the addon has no Lua part", entry.c_str());
        }
    }

    // The engine does not check the keys here: level_pack.cpp reads them for type = location
    addon.location.clear();
    if (const IniSection* location = ini.find("location"))
    {
        for (const IniLine& line : location->lines)
            addon.location.emplace_back(line.key, line.value);
    }
    // One mask per line, as a key ("textures\wpn\my_gun.dds"); a value ("mask = ...") is not taken
    addon.private_masks.clear();
    if (const IniSection* masks = ini.find("private"))
    {
        for (const IniLine& line : masks->lines)
        {
            if (warn && !line.value.empty())
                Logf("~ ", tag.c_str(), "[private] '%s = %s': a mask is a line of its own, the value is ignored",
                    line.key.c_str(), line.value.c_str());
            addon.private_masks.push_back(line.key);
        }
    }
    // [slot.<slot>]: the keys are checked by addon_slots.cpp (the slots are known there)
    addon.slots.clear();
    addon.slots_declare.clear();
    constexpr pcstr slot_prefix = "slot.";
    const size_t slot_prefix_length = xr_strlen(slot_prefix);
    for (const IniSection& section : ini.sections)
    {
        if (section.name.size() <= slot_prefix_length || section.name.compare(0, slot_prefix_length, slot_prefix) != 0)
            continue;
        AddonInfo::SlotSection& slot = addon.slots.emplace_back();
        slot.slot = section.name.substr(slot_prefix_length);
        for (const IniLine& line : section.lines)
            slot.keys.emplace_back(line.key, line.value);
    }
    if (const IniSection* declare = ini.find("slots_declare"))
    {
        for (const IniLine& line : declare->lines)
            addon.slots_declare.emplace_back(line.key, line.value);
    }
    addon.plugin_on_demand = addon.type == EAddonType::Location || !addon.slots.empty();
    return true;
}

xr_unique_ptr<AddonRecord> MakeAddonRecord(const fs::path& dir)
{
    auto addon = xr_make_unique<AddonRecord>();
    addon->dir_path = dir;
    addon->manifest_path = dir / kManifestName;
    addon->dir = WithTrailingSeparator(PathToUtf8(dir));
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

// The folders of gamedata\ an author may put at the root of the addon by mistake: only <addon>\gamedata\ is mounted
// (addon_content.cpp), <addon>\textures\ does nothing. Only these names, so that the other folders of an author
// (src\, docs\, the files of the slots, merge\, bin\) stay quiet
constexpr pcstr kGamedataFolders[] = { "ai", "anims", "configs", "levels", "meshes", "scripts", "shaders", "sounds",
    "spawns", "textures" };

// A folder of kGamedataFolders at the root of the addon: one warning per folder. A folder that the files of one of its
// [slot.*] name (files = textures\x: <addon>\textures\x\gamedata\) is no mistake
void WarnRootContentFolders(const AddonRecord& addon)
{
    std::error_code ec;
    for (fs::directory_iterator it(addon.dir_path, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec))
    {
        std::error_code entry_ec;
        if (!it->is_directory(entry_ec))
            continue;
        const xr_string name = PathToUtf8(it->path().filename());
        const bool content = std::any_of(std::begin(kGamedataFolders), std::end(kGamedataFolders),
            [&](pcstr folder) { return EqualsNoCase(name, folder); });
        if (!content)
            continue;
        const bool slot_files = std::any_of(addon.slots.begin(), addon.slots.end(),
            [&](const AddonInfo::SlotSection& slot) {
                return std::any_of(slot.keys.begin(), slot.keys.end(), [&](const std::pair<xr_string, xr_string>& key) {
                    if (!EqualsNoCase(key.first, "files"))
                        return false;
                    const size_t end_of_first = key.second.find_first_of("\\/");
                    return EqualsNoCase(Trim(key.second.substr(0, end_of_first)), name);
                });
            });
        if (slot_files)
            continue;
        Logf("~ ", AddonTag(addon).c_str(), "folder '%s' at the root of the addon is not mounted: the content of an "
            "addon lies in gamedata\\ (move it to %sgamedata\\%s\\)", name.c_str(), addon.dir.c_str(), name.c_str());
    }
}

// Parses the manifest and adds the addon to g_addons: failed on a manifest error, skipped as a duplicate of an id
// found earlier. expected_id: the id appdata/addons.ltx gives the folder ("" when the folder came from a scan).
void AddFoundAddon(xr_unique_ptr<AddonRecord> addon, const xr_string& expected_id)
{
    if (!ParseManifest(*addon))
    {
        addon->id = expected_id.empty() ? PathToUtf8(addon->dir_path.filename()) : expected_id;
        addon->state = EAddonState::Failed;
        Logf("! ", AddonTag(*addon).c_str(), "%s: %s", addon->reason.c_str(), PathToUtf8(addon->manifest_path).c_str());
        g_addons->push_back(std::move(addon));
        return;
    }
    if (!expected_id.empty() && addon->id != expected_id)
    {
        // The folder holds another addon than the launcher thinks: the list is stale, the player has to look
        addon->state = EAddonState::Failed;
        addon->reason = "appdata/addons.ltx lists this folder as '" + expected_id + "', its addon.ltx has id '" +
            addon->id + "'";
        Logf("! ", AddonTag(*addon).c_str(), "%s: %s", addon->reason.c_str(), addon->dir.c_str());
        g_addons->push_back(std::move(addon));
        return;
    }

    if (const AddonRecord* duplicate = FindAddonById(addon->id))
    {
        addon->state = EAddonState::Skipped;
        addon->reason = "duplicate id, first found at " + duplicate->dir;
        Logf("~ ", AddonTag(*addon).c_str(), "%s (skipped %s)", addon->reason.c_str(), addon->dir.c_str());
    }
    else
        WarnRootContentFolders(*addon);
    g_addons->push_back(std::move(addon));
}

// The subfolders of `dir` that hold an addon.ltx, by name; a folder whose name starts with '.' is no addon (as in
// DiscoverInRoot)
xr_vector<fs::path> NestedAddonFolders(const fs::path& dir)
{
    xr_vector<fs::path> nested;
    std::error_code ec;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec))
    {
        std::error_code entry_ec;
        const fs::path::string_type name = it->path().filename().native();
        if (!name.empty() && name.front() == '.')
            continue;
        if (it->is_directory(entry_ec) && fs::is_regular_file(it->path() / kManifestName, entry_ec))
            nested.push_back(it->path());
    }
    std::sort(nested.begin(), nested.end());
    return nested;
}

// A folder of a search root without addon.ltx (00-roadmap.md 5a, decision 63; review 2026-11-06 4.1 #2): an archive
// unpacked into a folder of its own name gives <root>\x\x\addon.ltx, and the engine reads addon.ltx only one level
// below the root. One addon inside - that mistake, a warning; several - a folder of addons (examples\, tests\ of the
// sources), told with -addon_debug only
void HintNestedAddonFolder(const fs::path& dir)
{
    const xr_vector<fs::path> nested = NestedAddonFolders(dir);
    if (nested.empty())
        return;
    const xr_string folder = WithTrailingSeparator(PathToUtf8(dir));
    if (nested.size() == 1)
    {
        const xr_string inner = WithTrailingSeparator(PathToUtf8(nested.front()));
        Logf("~ ", "addons", "folder %s has no addon.ltx, but %s has: addon.ltx is read only directly in the folder "
            "of an addon, one level below a search root - move the contents of %s up into %s", folder.c_str(),
            inner.c_str(), inner.c_str(), folder.c_str());
    }
    else if (g_debug_log)
    {
        Logf("  ", "addons", "folder %s has no addon.ltx: the %u addons inside it are not loaded from here (to load "
            "them, run the game with -addon_dir \"%s\")", folder.c_str(), static_cast<u32>(nested.size()),
            PathToUtf8(dir).c_str());
    }
}

// A folder given with -addon_dir is a folder of addons, not an addon (decision 63): the two mistakes of an author -
// no such folder, or the folder of the addon itself - are told, the root is searched as usual anyway
void HintCommandLineRoot(const xr_string& root)
{
    std::error_code ec;
    fs::path root_path = PathFromUtf8(root);
    if (!fs::is_directory(root_path, ec))
    {
        Logf("~ ", "addons", "-addon_dir %s does not exist or is not a folder: no addon is loaded from it",
            root.c_str());
        return;
    }
    if (!fs::is_regular_file(root_path / kManifestName, ec))
        return;
    if (!root_path.has_filename()) // a trailing separator
        root_path = root_path.parent_path();
    Logf("~ ", "addons", "-addon_dir %s is the folder of an addon itself (it has addon.ltx), and addons are looked for "
        "one level below -addon_dir, so this one is not loaded: point -addon_dir at its parent folder, \"%s\"",
        root.c_str(), PathToUtf8(root_path.parent_path()).c_str());
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
    const fs::path root_path = PathFromUtf8(root);
    if (!fs::is_directory(root_path, ec))
    {
        if (g_debug_log)
            Logf("  ", "addons", "search root does not exist: %s", root.c_str());
        return;
    }

    // One level deep: <root>/<dir>/addon.ltx; deterministic order by folder name. A folder whose name starts with '.'
    // is no addon (11-catalog-distribution.md 11, E3): the launcher keeps its work there (.staging - an addon being
    // installed, .trash - one being removed), also .git and the like; an addon listed in appdata/addons.ltx by such a
    // path is still read (AddListedAddon)
    xr_vector<fs::path> candidates;
    for (fs::directory_iterator it(root_path, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec))
    {
        std::error_code entry_ec;
        const fs::path::string_type name = it->path().filename().native();
        if (!name.empty() && name.front() == '.')
            continue;
        if (!it->is_directory(entry_ec))
            continue;
        if (fs::is_regular_file(it->path() / kManifestName, entry_ec))
            candidates.push_back(it->path());
        else
            HintNestedAddonFolder(it->path());
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
            addon->id = PathToUtf8(dir.filename());
        addon->state = EAddonState::Disabled;
        addon->reason = "not listed in appdata/addons.ltx (enable it in the launcher; while developing, keep the "
                        "addon in a folder of your own and run the game with -addon_dir <that folder>)";
        Logf("~ ", AddonTag(*addon).c_str(), "%s, not loaded: %s", addon->reason.c_str(), addon->dir.c_str());
        g_addons->push_back(std::move(addon));
    }
}

// ---------------------------------------------------------------------------------------------
// appdata/addons.ltx: the list of addons written by the launcher (docs: wiki/doc/plugins/loading.md)
//   [addons]              format = 1
//   [addon.<id>]          one section per addon, in load order:
//                         path = <folder>, enabled = true|false, trust = first_party|index|local,
//                         plugin_sha256 = <64 hex digits>, plugin_consent = <64 hex digits> (CheckPluginLibrary)
//   [slots]               <slot> = <provider id>: the providers chosen for the slots (13-extension-model.md, 4.4),
//                         written by the launcher and by the game (WriteSlotChoice)
//   [conflicts]           pair:<winner> = <loser>[, ...], file:<file> = <addon id | core>,
//                         atom:<file>|<atom> = <addon id | core>: the rules of the player for the conflicts of the
//                         files and of the atoms of the merged files (addon_host.h PairRule / FileRule / AtomRule),
//                         written by the launcher and by the game (WritePairRule, WriteFileRule, WriteAtomRule)
// Read with ParseSimpleIni: a file that is there but cannot be read or parsed, or has no section at all (0 bytes, only
// comments: a write cut short), is reported and the addon folders are scanned as without it, but no plugin is loaded
// (g_launcher_list_unreadable). A file with neither [addons] nor [addon.<id>] (only [slots] / [conflicts]: the game
// wrote it) is no list: the addon folders are scanned.
// ---------------------------------------------------------------------------------------------

constexpr pcstr kLauncherListName = "addons.ltx";
constexpr pcstr kLauncherListLog = "appdata/addons.ltx";
constexpr pcstr kLauncherEntryPrefix = "addon.";
constexpr pcstr kLauncherSlotsSection = "slots";
constexpr pcstr kLauncherConflictsSection = "conflicts";
constexpr pcstr kLauncherPairPrefix = "pair:";
constexpr pcstr kLauncherFilePrefix = "file:";
constexpr pcstr kLauncherAtomPrefix = "atom:";
constexpr char kLauncherAtomSeparator = '|'; // atom:<file>|<atom>: no file name has it (NormalizeRuleFileText)
constexpr pcstr kCoreWinner = "core"; // file:<file> = core - the base serves the file
constexpr u32 kLauncherListFormat = 1;

// [slots] of appdata/addons.ltx as read at the start (SlotChoices); WriteSlotChoice keeps it current
xr_vector<std::pair<xr_string, xr_string>> g_slot_choices;
// [conflicts] of appdata/addons.ltx as read at the start (PairRules, FileRules); WritePairRule and WriteFileRule
// keep it current
xr_vector<PairRule> g_pair_rules;
xr_vector<FileRule> g_file_rules;
xr_vector<AtomRule> g_atom_rules;
// The rules of this run against the file (ConflictRulesReadAt, addons_report.json content_built): the time (seconds
// since 1970, UTC) they last were what appdata/addons.ltx has - read at the start, or a write of the game over a file
// nobody changed since - and the time of the last change of the file then (g_rules_file_known false: no file). A file
// the launcher changed meanwhile is not read again in this run: the time stays, so the launcher sees the rules of
// the file as newer than the ones the report shows
u64 g_rules_read_at = 0;
fs::file_time_type g_rules_file_time{};
bool g_rules_file_known = false;

// The time of the last change of appdata/addons.ltx; false - no file (or no time)
bool LauncherListTime(const fs::path& file, fs::file_time_type& out)
{
    std::error_code ec;
    out = fs::last_write_time(file, ec);
    return !ec;
}

// A slot id: a-z 0-9 _ and '.' between the parts ("ui.main_menu", "<addon id>.theme"), up to 64 chars
bool IsValidSlotId(const xr_string& id)
{
    if (id.empty() || id.size() > 64 || id.front() == '.' || id.back() == '.' || id.find("..") != xr_string::npos)
        return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return IsIdChar(c) || c == '.'; });
}

xr_string ToLower(xr_string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return text;
}

void ReadSlotChoices(const SimpleIni& ini)
{
    g_slot_choices.clear();
    const IniSection* section = ini.find(kLauncherSlotsSection);
    if (!section)
        return;
    for (const IniLine& line : section->lines)
    {
        const xr_string slot = ToLower(line.key);
        const xr_string provider = ToLower(line.value);
        if (!IsValidSlotId(slot) || (!IsValidId(provider) && provider != "core"))
        {
            Logf("~ ", "addons", "%s: [%s] '%s = %s': expected <slot> = <addon id> (a-z 0-9 _ ., or core), ignored",
                kLauncherListLog, kLauncherSlotsSection, line.key.c_str(), line.value.c_str());
            continue;
        }
        const auto same_slot = [&](const std::pair<xr_string, xr_string>& other) { return other.first == slot; };
        if (std::any_of(g_slot_choices.begin(), g_slot_choices.end(), same_slot))
        {
            Logf("~ ", "addons", "%s: [%s] %s is there twice, the first one counts", kLauncherListLog,
                kLauncherSlotsSection, slot.c_str());
            continue;
        }
        g_slot_choices.emplace_back(slot, provider);
    }
}

// A character of a file of a rule as the names of the file system index have it: '/' -> '\', lower case on Windows
// only - the index of POSIX keeps the names as the disk spells them (xr_fs_strlwr, CLocatorAPI::layer_walk)
char RuleFileChar(char c)
{
    if (c == '/')
        return '\\';
#if defined(XR_PLATFORM_WINDOWS)
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#else
    return c;
#endif
}

bool NormalizeRuleFileText(const xr_string& text, xr_string& out)
{
    out.clear();
    for (const char c : text)
    {
        const char normalized = RuleFileChar(c);
        if (normalized == '\\' && (out.empty() || out.back() == '\\'))
            continue; // no leading separator, no empty folder
        out += normalized;
    }
    if (!out.empty() && out.back() == '\\')
        return false; // a folder
    // A path relative to gamedata\ of a layer: no drive, no way out of it, no mask (the rules name one file). No ';'
    // and '=' either: the line "file:<file> = <winner>" of addons.ltx would not read back (a comment, the value)
    const xr_string wrapped = "\\" + out + "\\";
    return !out.empty() && out.size() < 256 && out.find_first_of(":*?\"<>|;=") == xr_string::npos &&
        wrapped.find("\\..\\") == xr_string::npos && wrapped.find("\\.\\") == xr_string::npos;
}

// The atom of atom:<file>|<atom> in a line of addons.ltx (AtomRule): the case and the blanks inside stay; '%', '=',
// ';' and the second '/' of "//" as %XX - the ini reader would take '=' for the value and ';', "//" for a comment
xr_string EncodeRuleAtomText(const xr_string& atom)
{
    xr_string out;
    for (const char c : atom)
    {
        if (c == '%')
            out += "%25";
        else if (c == '=')
            out += "%3D";
        else if (c == ';')
            out += "%3B";
        else if (c == '/' && !out.empty() && out.back() == '/')
            out += "%2F";
        else
            out += c;
    }
    return out;
}

int RuleAtomHexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool DecodeRuleAtomText(const xr_string& text, xr_string& atom)
{
    atom.clear();
    for (size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '\n' || c == '\r' || c == '\0')
            return false;
        if (c != '%')
        {
            atom += c;
            continue;
        }
        if (i + 2 >= text.size() || RuleAtomHexDigit(text[i + 1]) < 0 || RuleAtomHexDigit(text[i + 2]) < 0)
            return false;
        atom += static_cast<char>(RuleAtomHexDigit(text[i + 1]) * 16 + RuleAtomHexDigit(text[i + 2]));
        i += 2;
    }
    atom = Trim(atom);
    return !atom.empty();
}

// A changed gap of a delta: "@gap <the label the merge gives it>" (addon_merge.cpp kMergeGapPrefix)
constexpr pcstr kRuleGapPrefix = "@gap";

// The atom of a rule as the merge names it: "@gap X" of a delta is the atom "X" - the merge takes a rule of either
// spelling for the same atom (addon_merge.cpp MergeRuleOf), so a rule is written once per atom, as "X" (the launcher
// the same: addons_ltx.rs rule_atom_label). "@gap" with nothing after it: empty - no atom
xr_string RuleAtomLabel(const xr_string& atom)
{
    const xr_string text = Trim(atom);
    const size_t prefix = xr_strlen(kRuleGapPrefix);
    if (text.compare(0, prefix, kRuleGapPrefix) == 0 && (text.size() == prefix || text[prefix] == ' '))
        return Trim(text.substr(prefix));
    return text;
}

// "<file>|<atom>" of atom:<file>|<atom> (the key without the prefix): the file as the rules keep it, the atom decoded
bool ParseAtomRuleKey(const xr_string& key, xr_string& file, xr_string& atom)
{
    const size_t bar = key.find(kLauncherAtomSeparator);
    return bar != xr_string::npos && NormalizeRuleFileText(Trim(key.substr(0, bar)), file) &&
        DecodeRuleAtomText(Trim(key.substr(bar + 1)), atom);
}

// [conflicts]: pair:<winner> = <loser>[, <loser> ...], file:<file> = <addon id | core>,
// atom:<file>|<atom> = <addon id | core>; the first rule of a file (of a file and an atom) counts
void ReadConflictRules(const SimpleIni& ini)
{
    g_pair_rules.clear();
    g_file_rules.clear();
    g_atom_rules.clear();
    const IniSection* section = ini.find(kLauncherConflictsSection);
    if (!section)
        return;
    const size_t pair_length = xr_strlen(kLauncherPairPrefix);
    const size_t file_length = xr_strlen(kLauncherFilePrefix);
    const size_t atom_length = xr_strlen(kLauncherAtomPrefix);
    for (const IniLine& line : section->lines)
    {
        const xr_string key = ToLower(line.key);
        if (key.compare(0, pair_length, kLauncherPairPrefix) == 0)
        {
            const xr_string winner = Trim(key.substr(pair_length));
            const xr_string value = ToLower(line.value);
            bool valid = IsValidId(winner) && !value.empty();
            for (size_t start = 0; valid && start <= value.size();)
            {
                size_t comma = value.find(',', start);
                if (comma == xr_string::npos)
                    comma = value.size();
                const xr_string loser = Trim(value.substr(start, comma - start));
                start = comma + 1;
                if (loser.empty())
                    continue;
                if (!IsValidId(loser) || loser == winner)
                {
                    Logf("~ ", "addons", "%s: [%s] %s: '%s' is not another addon id, ignored", kLauncherListLog,
                        kLauncherConflictsSection, line.key.c_str(), loser.c_str());
                    continue;
                }
                const auto same = [&](const PairRule& rule) { return rule.winner == winner && rule.loser == loser; };
                if (std::none_of(g_pair_rules.begin(), g_pair_rules.end(), same))
                    g_pair_rules.push_back(PairRule{ winner, loser });
            }
            if (!valid)
                Logf("~ ", "addons", "%s: [%s] '%s = %s': expected pair:<addon id> = <addon id>[, ...], ignored",
                    kLauncherListLog, kLauncherConflictsSection, line.key.c_str(), line.value.c_str());
            continue;
        }
        if (key.compare(0, file_length, kLauncherFilePrefix) == 0)
        {
            xr_string file;
            const xr_string winner = ToLower(line.value);
            if (!NormalizeRuleFileText(Trim(line.key.substr(file_length)), file) ||
                (!IsValidId(winner) && winner != kCoreWinner))
            {
                Logf("~ ", "addons", "%s: [%s] '%s = %s': expected file:<file in gamedata\\> = <addon id | core>, "
                    "ignored", kLauncherListLog, kLauncherConflictsSection, line.key.c_str(), line.value.c_str());
                continue;
            }
            const auto same = [&](const FileRule& rule) { return rule.file == file; };
            if (std::any_of(g_file_rules.begin(), g_file_rules.end(), same))
            {
                Logf("~ ", "addons", "%s: [%s] file:%s is there twice, the first one counts", kLauncherListLog,
                    kLauncherConflictsSection, file.c_str());
                continue;
            }
            g_file_rules.push_back(FileRule{ file, winner });
            continue;
        }
        if (key.compare(0, atom_length, kLauncherAtomPrefix) == 0)
        {
            // The atom keeps its case (Lua names, the keys of ltx, xml ids): from the line as written
            xr_string file, atom;
            const xr_string winner = ToLower(line.value);
            if (!ParseAtomRuleKey(line.key.substr(atom_length), file, atom) ||
                (!IsValidId(winner) && winner != kCoreWinner))
            {
                Logf("~ ", "addons", "%s: [%s] '%s = %s': expected atom:<file in gamedata\\>|<atom> = <addon id | "
                    "core>, ignored", kLauncherListLog, kLauncherConflictsSection, line.key.c_str(),
                    line.value.c_str());
                continue;
            }
            // "@gap X" and "X" are one atom (RuleAtomLabel): one rule per atom, the first line of either spelling -
            // the one the merge would take (addon_merge.cpp MergeRuleOf)
            const xr_string label = RuleAtomLabel(atom);
            const auto same = [&](const AtomRule& rule) {
                return rule.file == file && RuleAtomLabel(rule.atom) == label;
            };
            const auto first = std::find_if(g_atom_rules.begin(), g_atom_rules.end(), same);
            if (first != g_atom_rules.end())
            {
                if (first->atom == atom)
                    Logf("~ ", "addons", "%s: [%s] atom:%s|%s is there twice, the first one counts", kLauncherListLog,
                        kLauncherConflictsSection, file.c_str(), atom.c_str());
                else
                    Logf("~ ", "addons", "%s: [%s] atom:%s|%s and atom:%s|%s are one atom, the first one counts",
                        kLauncherListLog, kLauncherConflictsSection, file.c_str(), first->atom.c_str(), file.c_str(),
                        atom.c_str());
                continue;
            }
            g_atom_rules.push_back(AtomRule{ file, atom, winner });
            continue;
        }
        Logf("~ ", "addons", "%s: [%s] '%s': expected pair:<addon id>, file:<file> or atom:<file>|<atom>, ignored",
            kLauncherListLog, kLauncherConflictsSection, line.key.c_str());
    }
}

struct LauncherEntry
{
    xr_string id;
    xr_string path; // as written (UTF-8); empty: <game addons folder>/<id>
    bool enabled = true;
    xr_string trust = "local";
    xr_string plugin_sha256;  // lower case
    xr_string plugin_consent; // lower case
    bool plugin_sha256_malformed = false;  // given, but not 64 hex digits (AddonRecord)
    bool plugin_consent_malformed = false;
};

// Well-formed UTF-8 (RFC 3629): no stray continuation bytes, no overlong forms, no surrogates, nothing above U+10FFFF
bool IsValidUtf8(const xr_string& text)
{
    const auto* s = reinterpret_cast<const unsigned char*>(text.data());
    const size_t size = text.size();
    for (size_t i = 0; i < size;)
    {
        const unsigned char c = s[i];
        if (c < 0x80)
        {
            ++i;
            continue;
        }
        size_t length;
        unsigned char low = 0x80, high = 0xBF; // the range of the second byte
        if (c >= 0xC2 && c <= 0xDF)
            length = 2;
        else if (c >= 0xE0 && c <= 0xEF)
        {
            length = 3;
            low = c == 0xE0 ? 0xA0 : 0x80;
            high = c == 0xED ? 0x9F : 0xBF;
        }
        else if (c >= 0xF0 && c <= 0xF4)
        {
            length = 4;
            low = c == 0xF0 ? 0x90 : 0x80;
            high = c == 0xF4 ? 0x8F : 0xBF;
        }
        else
            return false;
        if (size - i < length || s[i + 1] < low || s[i + 1] > high)
            return false;
        for (size_t k = 2; k < length; ++k)
        {
            if (s[i + k] < 0x80 || s[i + k] > 0xBF)
                return false;
        }
        i += length;
    }
    return true;
}

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
    const fs::path file = PathFromUtf8(file_name);
    // Before the read: a write of the launcher during it makes the rules look older, never newer, than the file
    g_rules_read_at = static_cast<u64>(std::time(nullptr));
    g_rules_file_known = LauncherListTime(file, g_rules_file_time);
    std::error_code ec;
    const fs::file_status status = fs::status(file, ec);
    if (status.type() == fs::file_type::not_found)
    {
        if (g_debug_log)
            Logf("  ", "addons", "no %s, the addon folders are scanned", kLauncherListLog);
        return false;
    }

    xr_vector<char> bytes;
    SimpleIni ini;
    xr_string error;
    if (ec)
        error = "cannot get the status of the file: " + ErrorText(ec);
    else if (!fs::is_regular_file(status))
        error = "not a file";
    else if (!ReadFileBytes(file, bytes))
        error = "cannot read the file";
    else if (!ParseSimpleIni(bytes.data(), bytes.size(), ini, error, false)) // a repeated [addon.x]: reported below
        error = "syntax error: " + error;
    else if (ini.sections.empty())
    {
        // Every writer (the launcher, WriteLauncherLineBy) leaves at least one section: a file of 0 bytes or of only
        // comments and blanks is a write cut short (a power loss right after it), not "no list" - read as without
        // it, it would switch on every addon the player switched off and load every plugin unchecked
        error = "the file has no section (empty, or only comments): it is damaged, start the launcher to write it "
                "again";
    }
    if (!error.empty())
    {
        g_launcher_list_unreadable = true;
        Logf("! ", "addons", "%s: %s; the addon folders are scanned as without the file, plugins are not loaded",
            kLauncherListLog, error.c_str());
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

    ReadSlotChoices(ini);
    ReadConflictRules(ini);
    const size_t prefix_length = xr_strlen(kLauncherEntryPrefix);
    bool is_list = ini.find("addons") != nullptr;
    for (const IniSection& section : ini.sections)
    {
        if (section.name == "addons" || section.name == kLauncherSlotsSection ||
            section.name == kLauncherConflictsSection)
            continue;
        if (section.name.compare(0, prefix_length, kLauncherEntryPrefix) != 0)
        {
            Logf("~ ", "addons", "%s: unknown section [%s], ignored", kLauncherListLog, section.name.c_str());
            continue;
        }
        is_list = true;
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
        // A wrong hash is a hash that does not match: plugin_sha256 of any addon and plugin_consent of a local one
        // keep the plugin from loading (CheckPluginLibrary), never let it load unchecked. An empty value ("key =")
        // is no value (SimpleIni::get): not given, as without the line
        const auto read_digest = [&](pcstr key, xr_string& value, bool& malformed, bool used) {
            if (!ini.get(name, key, value))
                return;
            if (!IsHexDigest(value, 64))
            {
                Logf("~ ", "addons", "%s: [%s]: %s is not 64 hex digits%s", kLauncherListLog, name, key,
                    used ? ": the plugin of the addon is not loaded" : ", ignored (the trust of the addon does not "
                    "use it)");
                value.clear();
                malformed = used;
                return;
            }
            std::transform(value.begin(), value.end(), value.begin(), [](char c) {
                return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            });
        };
        read_digest("plugin_sha256", entry.plugin_sha256, entry.plugin_sha256_malformed, true);
        read_digest("plugin_consent", entry.plugin_consent, entry.plugin_consent_malformed, entry.trust == "local");
        out.push_back(std::move(entry));
    }
    if (!is_list && g_debug_log)
        Logf("  ", "addons", "%s has no [addons] and no [addon.<id>]: no list, the addon folders are scanned",
            kLauncherListLog);
    return is_list;
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
        out = PathFromUtf8(game_root) / PathFromUtf8(entry.id);
        return true;
    }
    if (!IsValidUtf8(entry.path))
    {
        // PathFromUtf8 would open it in the ANSI code page while the launcher reads it as UTF-8: the two would name two
        // folders. A path written by hand and saved in another encoding (Notepad "ANSI")
        why = "path is not UTF-8 (appdata/addons.ltx saved in another encoding?): save the file as UTF-8, or remove "
              "the addon in the launcher and add it again";
        return false;
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
    out = PathFromUtf8(base) / relative;
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
    addon->plugin_consent = entry.plugin_consent;
    addon->plugin_sha256_malformed = entry.plugin_sha256_malformed;
    addon->plugin_consent_malformed = entry.plugin_consent_malformed;
    addon->listed = true;
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
            // An archive unpacked one level too deep (HintNestedAddonFolder): the one addon.ltx inside is told
            const xr_vector<fs::path> nested = NestedAddonFolders(addon->dir_path);
            if (nested.size() == 1)
                addon->reason += " (it lies one level deeper, in " + WithTrailingSeparator(PathToUtf8(nested.front())) +
                    ": move the contents of that folder up)";
            Logf("! ", AddonTag(*addon).c_str(), "%s: %s", addon->reason.c_str(), addon->dir.c_str());
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

// [addon] api_min against the Addons API of the engine (GWP_API_VERSION, one version for the Lua API, the manifest and
// the plugins; 11-catalog-distribution.md 6): compatible when it has the "major" of the engine - by semver, the first
// number for 1.x.y and above, the pair 0.x for 0.x.y (before 1.0 the second number breaks) - and is not newer than the
// engine. Not set (0.0.0) - no requirement. false (the addon is failed, logged) when it is not compatible
bool CheckAddonApi(AddonRecord& addon)
{
    const u32 api_min = addon.addon_api_min;
    const EApiFit fit = ApiMinFit(api_min);
    if (fit == EApiFit::Ok)
        return true;
    addon.state = EAddonState::Failed;
    addon.reason = (fit == EApiFit::Newer ?
        "needs Addons API " + VersionString(api_min) + ", engine has " + VersionString(GWP_API_VERSION) :
        "built for Addons API " + ApiMajorString(api_min) + ", engine has " + VersionString(GWP_API_VERSION)) +
        ": " + ApiMinAdvice(api_min);
    Logf("! ", AddonTag(addon).c_str(), "failed: %s", addon.reason.c_str());
    return false;
}

// [addon] patches (15-merge-implementation.md 6.1): a compatibility patch works only while every addon it is made for
// works - else it is skipped (no error: the player switched one of them off, or it failed - its own error is logged).
// A target still to load (Discovered) or loaded (Active) works. false - skipped, logged
bool CheckPatchTargets(AddonRecord& addon)
{
    for (const xr_string& id : addon.patches)
    {
        const AddonRecord* target = FindAddonById(id);
        if (target && (target->state == EAddonState::Discovered || target->state == EAddonState::Active))
            continue;
        addon.state = EAddonState::Skipped;
        addon.reason = "patches: " + id + " is not active";
        Logf("* ", AddonTag(addon).c_str(), "skipped: %s (a compatibility patch works with all the addons it is made "
            "for)", addon.reason.c_str());
        return false;
    }
    return true;
}

// Before any plugin loads. The addons that can load get the order: every dependency before the addons that need
// it, then every addon of [addon] load_after that is there (a soft order: it gives way to depends and to a cycle),
// otherwise the order of discovery (appdata/addons.ltx, or the search roots and folder names). The others
// (failed, skipped, disabled) follow in the order found. Shutdown goes backwards, so dependents unload first.
void ResolveDependencies()
{
    // An addon made for another Addons API fails first: the ones that need it fail as for any failed dependency
    for (auto& addon : *g_addons)
    {
        if (addon->state == EAddonState::Discovered)
            CheckAddonApi(*addon);
    }
    // A failed addon fails the ones that need it: repeat until nothing changes
    for (bool changed = true; changed;)
    {
        changed = false;
        for (auto& addon : *g_addons)
        {
            if (addon->state == EAddonState::Discovered && (!CheckDependencies(*addon) || !CheckPatchTargets(*addon)))
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
    // An addon of load_after that is still to be placed (it loads at all: not failed, disabled or skipped)
    const auto is_waiting = [&](const xr_string& id) {
        return std::any_of(all.begin(), all.end(), [&](const xr_unique_ptr<AddonRecord>& addon) {
            return addon && addon->state == EAddonState::Discovered && addon->id == id;
        });
    };
    // The earliest addon whose dependencies and load_after are all placed goes next (a stable topological order).
    // None is ready while some have their dependencies placed: their load_after waits for itself (a cycle of
    // load_after, or with depends) - the earliest of them goes, its load_after does not hold
    for (bool placed = true; placed;)
    {
        placed = false;
        xr_unique_ptr<AddonRecord>* soft = nullptr;
        for (auto& addon : all)
        {
            if (!addon || addon->state != EAddonState::Discovered)
                continue;
            // The addons of [addon] patches go first, as the dependencies: the patch lies over them
            const bool ready = std::all_of(addon->depends.begin(), addon->depends.end(),
                [&](const AddonDependency& dependency) { return is_placed(dependency.id); }) &&
                std::all_of(addon->patches.begin(), addon->patches.end(), is_placed);
            if (!ready)
                continue;
            if (std::any_of(addon->load_after.begin(), addon->load_after.end(), is_waiting))
            {
                if (!soft)
                    soft = &addon;
                continue;
            }
            ordered.push_back(std::move(addon));
            placed = true;
            break;
        }
        if (placed || !soft)
            continue;
        xr_string waiting;
        for (const xr_string& id : (*soft)->load_after)
        {
            if (is_waiting(id))
                waiting += (waiting.empty() ? "" : ", ") + id;
        }
        Logf("~ ", AddonTag(**soft).c_str(), "[addon] load_after = %s does not hold: they wait for this addon "
            "themselves (a cycle of load_after or depends), it loads first", waiting.c_str());
        ordered.push_back(std::move(*soft));
        placed = true;
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
    const fs::path cache_dir = PathFromUtf8(cache_root) / PathFromUtf8(addon.id);
    fs::create_directories(cache_dir, ec);
    if (ec)
    {
        addon.reason = "cannot create plugin cache directory";
        return false;
    }

    string256 stem;
    xr_sprintf(stem, "%s_%llu_%lld", addon.plugin_name.c_str(), static_cast<unsigned long long>(size),
        static_cast<long long>(mtime));
    const fs::path shadow = cache_dir / PathFromUtf8(xr_string(stem) + kLibraryExt);

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
        const xr_string name = PathToUtf8(it->path().filename()); // string() throws on a stray non-ANSI file name
        if (name.rfind(prefix, 0) == 0 && it->path() != shadow)
        {
            std::error_code rm_ec;
            fs::remove(it->path(), rm_ec);
        }
    }

    // The PDB next to the copy under the name the library refers to (<name>.pdb), and the folder in the dbghelp
    // search path: a stack trace of a crash names the functions of the plugin. Copied while no copy of the plugin is
    // loaded (dbghelp keeps the file open until the module leaves it); a failed copy only costs the names.
    const fs::path pdb = original.parent_path() / PathFromUtf8(addon.plugin_name + ".pdb");
    if (fs::is_regular_file(pdb, ec))
    {
        std::error_code pdb_ec;
        fs::copy_file(pdb, cache_dir / pdb.filename(), fs::copy_options::update_existing, pdb_ec);
        if (pdb_ec && g_debug_log)
            Logf("  ", AddonTag(addon).c_str(), "the PDB is not copied next to the library: %s",
                ErrorText(pdb_ec).c_str());
    }
    xrDebug::AddSymbolSearchPath(ToAnsiForDbgHelp(cache_dir).c_str());

    addon.shadow_path = PathToUtf8(shadow);
    fs::path without_ext = shadow;
    without_ext.replace_extension();
    shadow_without_ext = PathToUtf8(without_ext);
    return true;
}

// E4 (11-catalog-distribution.md 8.6, 11): the plugin of an addon from the catalog (trust = index) is not loaded in v1
// - the CI of the catalog does not build plugins yet (v2, 11-catalog-distribution.md 10); the addon works without it,
// or fails when the plugin is required. first_party and local go on to CheckPluginLibrary. false - reason set
bool TrustLoadsPlugins(const AddonRecord& addon) { return addon.trust != "index"; }

bool PluginTrustAllows(AddonRecord& addon)
{
    if (TrustLoadsPlugins(addon))
        return true;
    addon.reason = "plugins of catalog addons are not loaded yet (trust = index)";
    return false;
}

// SHA-256 of a file (xrCore ContentMerge, as sha256sum), 64 lower case hex digits; empty when it cannot be read
xr_string FileSha256(const fs::path& file)
{
    xr_vector<char> bytes;
    if (!ReadFileBytes(file, bytes))
        return {};
    return xray::content_merge::Sha256Hex(std::string_view(bytes.data(), bytes.size()));
}

// E5 against TOCTOU: the copy in the cache is opened before its hash and stays open until the library is loaded from
// it (LoadPlugin), so the bytes that were hashed are the bytes that are loaded.
//  - Windows: shared for reading only (no FILE_SHARE_WRITE, no FILE_SHARE_DELETE): while the handle is open nobody can
//    write, rename, delete or replace the file (the open of a writer fails with a sharing violation), and a writer that
//    had it open already makes this open fail. LoadLibraryW of the same file by its full path works (it asks for read
//    access only).
//  - POSIX: there are no mandatory share modes. The file is opened O_RDONLY and hashed through that descriptor, but
//    dlopen opens the path again: an attacker able to write to $app_data_root$/cache/plugins/<id>/ (the same user)
//    can still rename another file over the copy between the hash and dlopen. The residual risk is accepted: such an
//    attacker can already change everything the game runs with the rights of that user.
class PluginFileLock
{
public:
    PluginFileLock() = default;
    PluginFileLock(const PluginFileLock&) = delete;
    PluginFileLock& operator=(const PluginFileLock&) = delete;
    ~PluginFileLock() { Release(); }

    // false: no such file, or a sharing violation (Windows: somebody writes to it or is deleting it)
    bool Open(const fs::path& file)
    {
        Release();
#if defined(XR_PLATFORM_WINDOWS)
        m_handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        return m_handle != INVALID_HANDLE_VALUE;
#else
        m_fd = open(file.c_str(), O_RDONLY | O_CLOEXEC);
        return m_fd >= 0;
#endif
    }

    // SHA-256 of the whole file read through the open handle (as FileSha256); empty when it cannot be read
    xr_string Sha256() const
    {
        xr_vector<char> bytes;
#if defined(XR_PLATFORM_WINDOWS)
        LARGE_INTEGER size{}, start{};
        if (m_handle == INVALID_HANDLE_VALUE || !GetFileSizeEx(m_handle, &size) || size.QuadPart < 0 ||
            !SetFilePointerEx(m_handle, start, nullptr, FILE_BEGIN))
            return {};
        bytes.resize(static_cast<size_t>(size.QuadPart));
        size_t done = 0;
        while (done < bytes.size())
        {
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - done, size_t(1) << 24));
            DWORD got = 0;
            if (!ReadFile(m_handle, bytes.data() + done, chunk, &got, nullptr) || got == 0)
                return {};
            done += got;
        }
#else
        struct stat info{};
        if (m_fd < 0 || fstat(m_fd, &info) != 0 || info.st_size < 0 || lseek(m_fd, 0, SEEK_SET) != 0)
            return {};
        bytes.resize(static_cast<size_t>(info.st_size));
        size_t done = 0;
        while (done < bytes.size())
        {
            const ssize_t got = read(m_fd, bytes.data() + done, bytes.size() - done);
            if (got < 0 && errno == EINTR)
                continue;
            if (got <= 0)
                return {};
            done += static_cast<size_t>(got);
        }
#endif
        return xray::content_merge::Sha256Hex(std::string_view(bytes.data(), bytes.size()));
    }

    bool IsOpen() const
    {
#if defined(XR_PLATFORM_WINDOWS)
        return m_handle != INVALID_HANDLE_VALUE;
#else
        return m_fd >= 0;
#endif
    }

    void Release()
    {
#if defined(XR_PLATFORM_WINDOWS)
        if (m_handle != INVALID_HANDLE_VALUE)
            CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
#else
        if (m_fd >= 0)
            close(m_fd);
        m_fd = -1;
#endif
    }

private:
#if defined(XR_PLATFORM_WINDOWS)
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
    int m_fd = -1;
#endif
};

// A digest for the messages: the first 12 hex digits
xr_string ShortDigest(const xr_string& digest)
{
    return digest.size() > 12 ? digest.substr(0, 12) + "..." : digest;
}

// E5 (11-catalog-distribution.md 8.6, lua_to_cpp/03-build-and-discovery.md 4a): the library about to be loaded
// against appdata/addons.ltx, after MakeShadowCopy:
//  - plugin_sha256 given (any trust): the library must have it;
//  - trust = local of an addon listed in appdata/addons.ltx: plugin_consent must be given and be the hash of the
//    library - the launcher asks the player when the addon is switched on and every time the hash changes;
//  - an addon not in the list (no appdata/addons.ltx, -addon_dir: the development) - not checked, a warning; a listed
//    first_party without plugin_sha256 - the same.
//  - a plugin_sha256 (any trust) or a plugin_consent (a listed local addon) that is not 64 hex digits is a mismatch.
// The copy in the cache is what is loaded, so the copy is hashed - and compared with the library itself: a stale or
// swapped copy under the same name (<name>_<size>_<time>) is made again once. The copy is hashed through `lock`, which
// stays open until the library is loaded (PluginFileLock: the copy cannot change in between). Every load is checked,
// plugin_reload too: a library rebuilt after the launcher wrote the hash is refused. The copy is compared with the
// library also with nothing to check against (unchecked): a stale copy of the same size and time is made again.
// false - reason set (MarkPluginUnavailable), `lock` released; true - `lock` holds the copy
bool CheckPluginLibrary(
    AddonRecord& addon, const fs::path& library, xr_string& shadow_without_ext, PluginFileLock& lock)
{
    const xr_string tag = AddonTag(addon);
    const bool needs_consent = addon.listed && addon.trust == "local";
    if (addon.plugin_sha256_malformed)
    {
        addon.reason = "plugin hash mismatch: plugin_sha256 in appdata/addons.ltx is not 64 hex digits";
        return false;
    }
    if (needs_consent && addon.plugin_consent_malformed)
    {
        addon.reason = "plugin_consent in appdata/addons.ltx is not 64 hex digits: taken as a consent to another "
                       "library (the launcher asks again)";
        return false;
    }
    const bool unchecked = addon.plugin_sha256.empty() && !needs_consent;
    if (unchecked)
    {
        if (!addon.listed)
            Logf("~ ", tag.c_str(),
                "plugin '%s' is loaded unchecked: the addon is not in appdata/addons.ltx (-addon_dir or no launcher "
                "list), so neither plugin_sha256 nor plugin_consent applies",
                addon.plugin_name.c_str());
        else
            Logf("~ ", tag.c_str(), "plugin '%s' is loaded unchecked: appdata/addons.ltx gives no plugin_sha256",
                addon.plugin_name.c_str());
    }

    // Unchecked too: the copy is compared with the library. The name of the copy (<name>_<size>_<time>) does not tell
    // two builds of one size apart when the time of the file stays (a copy keeps it): the old copy would load
    const xr_string digest = FileSha256(library);
    const auto hash_copy = [&]() -> xr_string {
        return lock.Open(PathFromUtf8(addon.shadow_path)) ? lock.Sha256() : xr_string();
    };
    xr_string loaded = digest.empty() ? xr_string() : hash_copy();
    if (!digest.empty() && loaded != digest)
    {
        lock.Release();
        std::error_code ec;
        fs::remove(PathFromUtf8(addon.shadow_path), ec);
        if (!MakeShadowCopy(addon, library, shadow_without_ext))
            return false;
        loaded = hash_copy();
    }
    if (digest.empty() || loaded != digest)
    {
        addon.reason = digest.empty() ? "cannot read the plugin library to check its SHA-256" :
            !lock.IsOpen() ? "cannot open the copy of the plugin library in the cache for the check (" +
                    addon.shadow_path + "): another program has it open for writing or deleting" :
                             "the copy of the plugin library in the cache differs from the library (" +
                    addon.shadow_path + "), its SHA-256 is not checked";
        lock.Release();
        return false;
    }
    if (unchecked)
        return true; // the copy is the library, nothing to compare it with

    if (!addon.plugin_sha256.empty() && digest != addon.plugin_sha256)
    {
        // The library changed after the launcher wrote the list (a rebuild, a manual copy), or the list belongs to
        // another copy of the game (two games sharing one appdata folder are not supported)
        addon.reason = "plugin hash mismatch: appdata/addons.ltx has plugin_sha256 " +
            ShortDigest(addon.plugin_sha256) + ", the library has " + ShortDigest(digest) +
            " (reinstall or update the addon in the launcher; two game folders sharing one appdata folder are not "
            "supported)";
        lock.Release();
        return false;
    }
    if (needs_consent && addon.plugin_consent.empty())
    {
        addon.reason = "the player did not agree to load the plugin of a local addon (no plugin_consent in "
                       "appdata/addons.ltx): start the game from the launcher and allow the plugin when it asks; "
                       "while developing, keep the addon in a folder of your own and run the game with -addon_dir "
                       "<that folder>";
        lock.Release();
        return false;
    }
    if (needs_consent && digest != addon.plugin_consent)
    {
        addon.reason = "the plugin library changed after the player agreed: plugin_consent " +
            ShortDigest(addon.plugin_consent) + ", the library has " + ShortDigest(digest) +
            " (the launcher asks again)";
        lock.Release();
        return false;
    }
    if (g_debug_log)
        Logf("  ", tag.c_str(), "plugin '%s': SHA-256 %s matches appdata/addons.ltx (%s)", addon.plugin_name.c_str(),
            digest.c_str(), !needs_consent ? "plugin_sha256" :
                addon.plugin_sha256.empty() ? "plugin_consent" : "plugin_sha256, plugin_consent");
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
    alife_hooks::RemovePluginHooks(plugin); // its objects get the Lua update again
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

// A symbol of a loaded library, nullptr when it has none - quietly: ModuleHandle::GetProcAddress logs a missing one as
// an error, and a library without gwp_plugin_api_built is no error by itself
const void* FindLibrarySymbol(const XRay::ModuleHandle& module, pcstr name)
{
    void* const handle = module();
    if (!handle)
        return nullptr;
#if defined(XR_PLATFORM_WINDOWS)
    // SDL_LoadObject gives the HMODULE of LoadLibrary
    return reinterpret_cast<const void*>(::GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    // dlopen (ModuleHandle, or SDL_LoadObject on the other POSIX platforms) gives the handle of dlsym
    return dlsym(handle, name);
#endif
}

// gwp_plugin_api_built of the library (GWP_PLUGIN_API_BUILT_NAME) against the engine, before any code of the plugin
// runs: the major of the engine (SameApiMajor). No symbol: fine when [plugin] api_min is given, or [addon] api_min it
// defaults to (checked by LoadPlugin before the load, the plugin of an older header with it loads as before), refused
// without both. false - `why` set
bool PluginBuiltVersionFits(const AddonRecord& addon, const XRay::ModuleHandle& module, xr_string& why)
{
    const auto* built = static_cast<const uint32_t*>(FindLibrarySymbol(module, GWP_PLUGIN_API_BUILT_NAME));
    if (built)
    {
        const uint32_t version = *built;
        if (version != 0 && SameApiMajor(version))
            return true;
        why = xr_string("version mismatch: the plugin is built with Plugin API ") + VersionString(version) + " (" +
            ApiMajorString(version) + "), the engine has " + VersionString(GWP_API_VERSION) + " (" +
            ApiMajorString(GWP_API_VERSION) + "): rebuild the plugin with the headers of this engine, or install the "
            "version of the addon made for it";
        return false;
    }
    if (addon.plugin_api_min != 0)
    {
        if (g_debug_log)
            Logf("  ", AddonTag(addon).c_str(), "plugin '%s' exports no %s (a header older than it, or "
                "gwp_plugin_init without GWP_PLUGIN_INIT): its version is taken from api_min %s of addon.ltx",
                addon.plugin_name.c_str(), GWP_PLUGIN_API_BUILT_NAME, VersionString(addon.plugin_api_min).c_str());
        return true;
    }
    why = xr_string("the Plugin API the plugin is built for is unknown: the library exports no ") +
        GWP_PLUGIN_API_BUILT_NAME + " and addon.ltx has no api_min ([plugin] or [addon]), so it is not run "
        "(rebuild the plugin with GWP_PLUGIN_INIT / GWP_DEFINE_PLUGIN of the current gwp_api.h, or set [plugin] "
        "api_min = <the Plugin API it is built with>)";
    return false;
}

void LoadPlugin(AddonRecord& addon)
{
    if (addon.plugin_name.empty())
    {
        addon.plugin_state = EPluginState::None;
        addon.state = EAddonState::Active;
        return;
    }

    if (g_launcher_list_unreadable)
    {
        addon.reason = kLauncherListUnreadableReason;
        MarkPluginUnavailable(addon);
        return;
    }

    if (!PluginTrustAllows(addon))
    {
        MarkPluginUnavailable(addon);
        return;
    }

    // [plugin] api_min (by default [addon] api_min, ParseManifest): the rule of [addon] api_min (CheckAddonApi) - the
    // major of the engine and not newer
    const EApiFit plugin_fit = ApiMinFit(addon.plugin_api_min);
    if (plugin_fit != EApiFit::Ok)
    {
        addon.reason = (plugin_fit == EApiFit::Newer ?
            "plugin requires Plugin API " + VersionString(addon.plugin_api_min) + ", engine has " +
                VersionString(GWP_API_VERSION) :
            "plugin built for Plugin API " + ApiMajorString(addon.plugin_api_min) + ", engine has " +
                VersionString(GWP_API_VERSION)) + ": " + ApiMinAdvice(addon.plugin_api_min);
        MarkPluginUnavailable(addon);
        return;
    }

    const fs::path library =
        addon.dir_path / "bin" / kPluginOs / kPluginArch / PathFromUtf8(addon.plugin_name + kLibraryExt);
    addon.plugin_path = PathToUtf8(library);

    std::error_code ec;
    if (!fs::is_regular_file(library, ec))
    {
        addon.reason = xr_string("no build for ") + kPluginOs + "/" + kPluginArch + " (" + addon.plugin_path + ")";
        MarkPluginUnavailable(addon);
        return;
    }

    xr_string shadow_without_ext;
    PluginFileLock shadow_lock; // E5: from the hash of the copy until it is loaded (CheckPluginLibrary)
    if (!MakeShadowCopy(addon, library, shadow_without_ext) ||
        !CheckPluginLibrary(addon, library, shadow_without_ext, shadow_lock))
    {
        MarkPluginUnavailable(addon);
        return;
    }

    addon.module = XRay::LoadModule(false);
    const bool opened = addon.module->Open(shadow_without_ext.c_str()) != nullptr;
    shadow_lock.Release(); // mapped by the loader now (or not loaded): the copy may change and go again
    if (!opened)
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

    // The version of the header the plugin is built with, before any call into it (GWP_PLUGIN_API_BUILT_NAME): a
    // plugin of another major would call a table of another layout from gwp_plugin_init - abi_major alone does not
    // tell 0.1 from 0.2, and a plugin may not check even that. Without the symbol only [plugin] api_min (or the
    // [addon] api_min it defaults to) tells the version (checked above); without both nothing does: refused
    xr_string why_not;
    if (!PluginBuiltVersionFits(addon, *addon.module, why_not))
    {
        addon.module.reset();
        addon.reason = why_not;
        MarkPluginUnavailable(addon);
        return;
    }
#if defined(XR_PLATFORM_WINDOWS)
    HMODULE module_base = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(init), &module_base);
    addon.module_base = module_base;
#endif

    // The plugin may be built against a newer version of the same major whose GwpPluginDesc is longer than ours.
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
            : result == GWP_ERROR_VERSION_MISMATCH ?
                xr_string(GWP_PLUGIN_ENTRY_NAME) + " returned " + code + " (GWP_ERROR_VERSION_MISMATCH): the plugin "
                    "refuses Plugin API " + VersionString(GWP_API_VERSION) + " of the engine (its line above may say "
                    "why) - rebuild it with the headers of this engine, or install the version of the addon made for it"
                                        : xr_string(GWP_PLUGIN_ENTRY_NAME) + " returned " + code + " (the plugin refused "
                    "to load: its lines above in the log may say why)";
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
    // abi_major carries only the first number; the major of api_built adds the pair 0.x before 1.0 (SameApiMajor,
    // 11-catalog-distribution.md 6): a plugin built with the 0.1 header has the layouts of 0.1, not of 0.2. Same
    // fields as before, so a plugin of the major of the engine passes as it did
    const uint32_t api_min = desc.api_min != 0 ? desc.api_min : api_built;
    const bool built_same_major = api_built != 0 && SameApiMajor(api_built);
    const bool header_ok = desc.size >= 3 * sizeof(uint32_t) && desc.size <= GWP_PLUGIN_DESC_MAX_SIZE &&
        desc.abi_major == GWP_API_VERSION_MAJOR && has_built && built_same_major && api_min <= GWP_API_VERSION &&
        (!has_reserved || desc.reserved == 0);
    if (!header_ok)
    {
        ReleasePlugin(addon);
        addon.reason = "plugin header mismatch: abi " + VersionString(GWP_MAKE_VERSION(desc.abi_major, 0, 0)) +
            ", requires api " + VersionString(api_min) + ", engine api " + VersionString(GWP_API_VERSION) +
            (has_reserved && desc.reserved != 0 ? ", non-zero reserved field" : "") +
            (!has_built || api_built == 0 ? ", no api_built (built with a pre-release header: rebuild the plugin)" :
                !built_same_major ? ", built with Plugin API " + VersionString(api_built) + " (" +
                        ApiMajorString(api_built) + "), engine " + ApiMajorString(GWP_API_VERSION) +
                        ": rebuild the plugin" :
                                    xr_string());
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

// addon_reload (RequestAddonReload): may it run now. A level that loads runs its loading events frame by frame - the
// Lua of the level (binders, on_game_load) is half done; a save is written inside one frame, but a console command
// run from Lua (a save_state handler) may come in the middle of it
bool AddonReloadMomentOk(xr_string& why)
{
    if (!GEnv.ScriptEngine || !GEnv.ScriptEngine->lua())
        why = "no script engine";
    else if (!g_loading_events.empty() || (g_pGameLevel && !g_pGameLevel->bReady))
        why = "a level is loading, try when it is loaded";
    else if (binders::IsSaving())
        why = "a save is being written";
    else
        return true;
    return false;
}

// The plugin of the addon is a target of the reload (the rule of RequestPluginReload): it has one, and it is not an
// on-demand plugin of a pack / provider that is off now
bool AddonReloadPluginTarget(const AddonRecord& addon, xr_string& why)
{
    if (addon.plugin_name.empty())
    {
        why = "the addon has no plugin";
        return false;
    }
    if (addon.plugin_state == EPluginState::Inactive ||
        (addon.plugin_on_demand && !addon.module && addon.plugin_state != EPluginState::Crashed))
    {
        why = "it loads only while its location pack / slot provider is active";
        return false;
    }
    return true;
}

// addon_reload: the steps of 08-unified-addons-api-lua-native.md 4.10 - on_unload and the save data of the Lua part,
// its registrations and modules go, the plugin is reloaded (on_unload, a new copy of the library, gwp_plugin_init:
// the E5 checks apply), the Lua part loads again and gets its save data back. Top of a frame: no plugin or Lua code
// on the stack. A failed step is logged and never ends the game: the Lua part stays failed with the reason
void ReloadAddon(AddonRecord& addon)
{
    const xr_string tag = AddonTag(addon);
    xr_string why;
    if (!AddonReloadMomentOk(why))
    {
        Logf("! ", tag.c_str(), "addon_reload: refused: %s", why.c_str());
        return;
    }
    // Copies: ReloadPlugin may change the record (the state, the reason), never the id
    const xr_string id = addon.id;
    Logf("* ", tag.c_str(), "addon_reload: start ([lua] entry: %s, plugin: %s)",
        addon.lua_entry.empty() ? "none" : addon.lua_entry.c_str(),
        addon.plugin_name.empty() ? "none" : addon.plugin_name.c_str());

    // 1. The Lua side lets go of the plugin before it goes: on_unload of the entry may still call it
    UnloadLuaEntryForReload(id.c_str());

    // 2. The plugin
    bool lua_may_load = true;
    if (AddonReloadPluginTarget(addon, why))
    {
        Logf("* ", tag.c_str(), "addon_reload: reloading plugin '%s'", addon.plugin_name.c_str());
        ReloadPlugin(addon);
        if (addon.plugin_state == EPluginState::Loaded)
        {
            if (g_pGameLevel)
                Logf("~ ", tag.c_str(), "plugin '%s' reloaded inside a game: it got no alife_on_start of this game. "
                    "The objects online keep what the old instance gave them, unless the plugin sets them up again "
                    "itself; otherwise load a save", addon.plugin_name.c_str());
        }
        else if (addon.state == EAddonState::Failed)
            lua_may_load = false; // a required plugin: the addon does not work (addon_list has the reason)
    }
    else if (!addon.plugin_name.empty())
        Logf("~ ", tag.c_str(), "addon_reload: its plugin '%s' is not reloaded: %s", addon.plugin_name.c_str(),
            why.c_str());

    // 3. The Lua part again
    bool lua_ok = true;
    if (!lua_may_load)
    {
        lua_ok = false;
        AbortLuaEntryReload(id.c_str(), ("its required plugin did not load: " + addon.reason).c_str());
    }
    else
    {
        xr_string reason;
        lua_ok = LoadLuaEntryForReload(id.c_str(), reason);
        if (!lua_ok)
            Logf("! ", tag.c_str(), "addon_reload: its Lua part did not load: %s", reason.c_str());
    }

    // The main menu shown now was built with the old registrations (the buttons of the addons): build it again
    if (MainMenu() && MainMenu()->IsActive())
        MainMenu()->RebuildUI();

    Logf(lua_ok ? "* " : "! ", tag.c_str(), "addon_reload: done%s", lua_ok ? "" :
        " with errors: the Lua part is failed in this Lua state (addons.lua_entries() has the reason), the next Lua "
        "state (a load, a new game) tries it again");
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
    g_launcher_list_unreadable = false; // ReadLauncherList below says it again (Shutdown allows a new Initialize)
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
        if (i < dev_roots)
            HintCommandLineRoot(roots[i]);
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
        // and skips the compatibility patches made for it
        if (addon->state != EAddonState::Discovered || !CheckDependencies(*addon) || !CheckPatchTargets(*addon))
            continue;
        // A broken appdata/addons.ltx or a trust whose plugins are not loaded (index): LoadPlugin refuses the plugin
        // right away, an on-demand one too (addon_list tells it at once, a required one fails the addon before its
        // pack or provider is chosen)
        if (addon->plugin_on_demand && !addon->plugin_name.empty() && !g_launcher_list_unreadable &&
            TrustLoadsPlugins(*addon))
        {
            // 00-roadmap.md 5a, decision 1: with its location pack / slot provider (LoadOnDemandPlugins)
            addon->state = EAddonState::Active;
            addon->plugin_state = EPluginState::Inactive;
            Logf("* ", AddonTag(*addon).c_str(), "plugin '%s' loads while %s is active", addon->plugin_name.c_str(),
                addon->type == EAddonType::Location ? "its location pack" : "one of its slot providers");
            continue;
        }
        LoadPlugin(*addon);
    }
    // A plugin that needs an on-demand one runs without it while its pack / provider is off
    for (const auto& addon : *g_addons)
    {
        if (addon->plugin_state != EPluginState::Loaded)
            continue;
        for (const AddonDependency& dependency : addon->depends)
        {
            const AddonRecord* needed = FindAddonById(dependency.id);
            if (needed && needed->plugin_on_demand && !needed->plugin_name.empty())
                Logf("~ ", AddonTag(*addon).c_str(), "depends on '%s', whose plugin loads only while its location "
                    "pack / slot provider is active: the services and exports of that plugin come and go",
                    dependency.id.c_str());
        }
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
    g_slot_choices.clear();
    g_save_data.clear();
    data::Shutdown();
    ini::Shutdown();
    console::Shutdown();
    timers::Shutdown();
    binders::Shutdown();
    storage::Shutdown();
    callbacks::Shutdown();
    alife_hooks::Shutdown();
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
        if (addon.addon_reload_requested && !g_pGameLevel && slots::ApplyPending())
        {
            // The menu applies a content set (the Lua state and the on-demand plugins start again): the reload waits
            // for its end - else it is done in the frame right before that restart and its work is thrown away
            g_plugin_work = true; // the next frame looks again
            continue;
        }
        if (addon.addon_reload_requested)
        {
            // addon_reload takes the plugin with it: a plugin_reload asked for in the same frame is done by it
            addon.addon_reload_requested = false;
            addon.reload_requested = false;
            ReloadAddon(addon);
            continue;
        }
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
        else if (addon.state == EAddonState::Skipped && refusal.empty())
            refusal = "the addon is skipped: " + addon.reason;
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
        if (addon.plugin_state == EPluginState::Inactive || (addon.plugin_on_demand && !addon.module &&
            addon.plugin_state != EPluginState::Crashed))
        {
            // The content set loads it (addon_slots.cpp): a reload here would load it without its pack / provider
            reason = "its plugin loads only while its location pack / slot provider is active (and works)";
            return false;
        }
        addon.reload_requested = true;
        g_plugin_work = true;
        return true;
    }
    reason = refusal.empty() ? xr_string("no such addon (addon_list shows them)") : refusal;
    return false;
}

bool RequestAddonReload(pcstr addon_id, xr_string& reason)
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
        // The targets of plugin_reload: a duplicate, a disabled addon, one with a broken manifest or a failed
        // dependency is not one; an addon failed by its required plugin is
        if (addon.state == EAddonState::Disabled && refusal.empty())
            refusal = "the addon is disabled: " + addon.reason;
        else if (addon.state == EAddonState::Skipped && refusal.empty())
            refusal = "the addon is skipped: " + addon.reason;
        else if (addon.state == EAddonState::Failed && addon.plugin_state != EPluginState::Unavailable &&
            refusal.empty())
            refusal = "the addon failed: " + addon.reason;
        if (addon.state == EAddonState::Skipped || addon.state == EAddonState::Disabled ||
            addon.state == EAddonState::Discovered ||
            (addon.state == EAddonState::Failed && addon.plugin_state != EPluginState::Unavailable))
            continue;
        // The development mode (08 4.10, 00-roadmap.md 4.3): a folder of the developer. The launcher gives trust; an
        // addon of the catalog or of GW is reloaded only with -addon_debug
        if (!g_debug_log && addon.listed && addon.trust != "local")
        {
            reason = "a development command: allowed for an addon with trust = local, one not in appdata/addons.ltx "
                     "(-addon_dir, no launcher list) or with -addon_debug; this one has trust = " + addon.trust;
            return false;
        }
        if (!AddonReloadMomentOk(reason))
            return false;
        addon.addon_reload_requested = true;
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

xr_vector<AddonInfo> ListAddons()
{
    xr_vector<AddonInfo> result;
    if (!detail::g_addons)
        return result;
    for (const auto& addon : *detail::g_addons)
    {
        AddonInfo& info = result.emplace_back();
        info.id = addon->id;
        info.version = addon->version;
        info.type = addon->type_name;
        info.dir = addon->dir;
        info.active = addon->state == detail::EAddonState::Active;
        info.state = detail::AddonStateName(addon->state);
        info.plugin = addon->plugin_name;
        info.plugin_state = detail::PluginStateName(addon->plugin_state);
        info.reason = addon->reason;
        info.load_after = addon->load_after;
        for (const detail::AddonDependency& dependency : addon->depends)
            info.depends.push_back(dependency.id);
        info.patches = addon->patches;
        info.game_version = addon->game_version;
        info.location = addon->location;
        info.private_masks = addon->private_masks;
        info.lua_entry = addon->lua_entry;
        info.slots = addon->slots;
        info.slots_declare = addon->slots_declare;
    }
    return result;
}

namespace
{
bool ListedAddonId(const xr_vector<xr_string>& ids, const xr_string& id)
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}
} // namespace

void UnloadOnDemandPlugins(const xr_vector<xr_string>& active)
{
    using namespace detail;
    if (!g_addons)
        return;
    if (events::PluginCallDepth() != 0)
    {
        Logf("! ", "addons", "on-demand plugins: a plugin function is on the stack, nothing is unloaded");
        return;
    }
    // Backwards, as Shutdown: an addon that needs another one unloads first
    for (auto it = g_addons->rbegin(); it != g_addons->rend(); ++it)
    {
        AddonRecord& addon = **it;
        if (!addon.plugin_on_demand || addon.plugin_name.empty() || ListedAddonId(active, addon.id))
            continue;
        if (addon.module)
        {
            UnloadPlugin(addon);
            Logf("* ", AddonTag(addon).c_str(), "plugin '%s' unloaded: %s is not active", addon.plugin_name.c_str(),
                addon.type == EAddonType::Location ? "its location pack" : "no slot provider of the addon");
        }
        addon.reload_requested = false;
        // A plugin that did not load and is not required is tried again with the next activation; a required one
        // keeps the addon failed for the rest of the run
        if (addon.state == EAddonState::Active && addon.plugin_state != EPluginState::Inactive)
        {
            addon.plugin_state = EPluginState::Inactive;
            addon.reason.clear();
        }
    }
}

void LoadOnDemandPlugins(const xr_vector<xr_string>& active, xr_vector<xr_string>& failed)
{
    using namespace detail;
    if (!g_addons)
        return;
    if (events::PluginCallDepth() != 0)
    {
        Logf("! ", "addons", "on-demand plugins: a plugin function is on the stack, nothing is loaded");
        return;
    }
    for (auto& entry : *g_addons)
    {
        AddonRecord& addon = *entry;
        if (!addon.plugin_on_demand || addon.plugin_name.empty() || !ListedAddonId(active, addon.id))
            continue;
        if (addon.state == EAddonState::Failed)
        {
            failed.push_back(addon.id); // its required plugin failed earlier in this run
            continue;
        }
        if (addon.state != EAddonState::Active || addon.plugin_state != EPluginState::Inactive)
            continue; // loaded already, or crashed (OnGameStart / plugin_reload take it)
        Logf("* ", AddonTag(addon).c_str(), "loading plugin '%s' on demand", addon.plugin_name.c_str());
        LoadPlugin(addon);
        if (addon.state == EAddonState::Failed)
            failed.push_back(addon.id);
    }
}

const xr_vector<std::pair<xr_string, xr_string>>& SlotChoices() { return detail::g_slot_choices; }

namespace
{
// The key of a line of appdata/addons.ltx as the rules compare it (ReadConflictRules): lower case; the id of
// pair:<id> without the blanks; the file of file:<file> as the rules keep it (NormalizeRuleFileText: '/' as '\', no
// leading separator, the case stays on POSIX) - a line the launcher wrote as "file:/a/b.ltx" or "pair: x" is the line of
// the rule, not another one; atom:<file>|<atom> - the file so, the atom decoded, as the merge names it (RuleAtomLabel:
// "@gap X" and "X" are one key - WriteAtomRule rewrites the first of their lines and removes the rest) and encoded
// again, in its case
xr_string LauncherLineKey(const xr_string& key)
{
    const xr_string trimmed = detail::Trim(key);
    xr_string result = detail::ToLower(trimmed);
    const size_t pair_length = xr_strlen(detail::kLauncherPairPrefix);
    const size_t file_length = xr_strlen(detail::kLauncherFilePrefix);
    const size_t atom_length = xr_strlen(detail::kLauncherAtomPrefix);
    xr_string atom_file, atom;
    if (result.compare(0, atom_length, detail::kLauncherAtomPrefix) == 0 &&
        detail::ParseAtomRuleKey(trimmed.substr(atom_length), atom_file, atom))
    {
        return detail::kLauncherAtomPrefix + atom_file + detail::kLauncherAtomSeparator +
            detail::EncodeRuleAtomText(detail::RuleAtomLabel(atom));
    }
    if (result.compare(0, pair_length, detail::kLauncherPairPrefix) == 0)
        result = detail::kLauncherPairPrefix + detail::Trim(result.substr(pair_length));
    else if (result.compare(0, file_length, detail::kLauncherFilePrefix) == 0)
    {
        xr_string file;
        const xr_string rest = detail::Trim(trimmed.substr(file_length));
        result.resize(file_length);
        if (detail::NormalizeRuleFileText(rest, file))
            result += file;
        else
        {
            for (const char c : rest)
                result += detail::RuleFileChar(c);
        }
    }
    return result;
}

// The data of a written (and closed) file from the cache of the OS onto the disk: FlushFileBuffers / fsync through a
// handle of its own (both flush the file, not only what that handle wrote). false - `error` says why
bool FlushFileToDisk(const fs::path& file, xr_string& error)
{
#if defined(XR_PLATFORM_WINDOWS)
    const HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        error = ErrorText(std::error_code(static_cast<int>(GetLastError()), std::system_category()));
        return false;
    }
    const bool flushed = FlushFileBuffers(handle) != FALSE;
    if (!flushed)
        error = ErrorText(std::error_code(static_cast<int>(GetLastError()), std::system_category()));
    CloseHandle(handle);
    return flushed;
#else
    const int fd = open(file.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
    {
        error = ErrorText(std::error_code(errno, std::generic_category()));
        return false;
    }
    int result;
    do
        result = fsync(fd);
    while (result != 0 && errno == EINTR);
    if (result != 0)
        error = ErrorText(std::error_code(errno, std::generic_category()));
    close(fd);
    return result == 0;
#endif
}

// POSIX: the rename itself onto the disk (the entry of the folder); best effort, a failure only costs that. Windows:
// NTFS keeps the rename in its journal
void FlushFolderToDisk(const fs::path& folder)
{
#if defined(XR_PLATFORM_WINDOWS)
    (void)folder;
#else
    const int fd = open(folder.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    fsync(fd);
    close(fd);
#endif
}

// The value of the line of a key from the values the file has for it right now (the lines of the key in the section, in
// their order; none - no line): false - the file stays as it is
using LauncherLineValue = std::function<bool(const xr_vector<xr_string>& current, xr_string& value)>;

// Sets "<key> = <value>" in the first [<section>] of appdata/addons.ltx (value empty: the line of the key goes); every
// other line of the file stays as it is (the launcher owns the rest of it, and may write it while the game runs). The
// value is made from the lines of the key the file has at the moment of the write (`make_value`), so a value the
// launcher wrote meanwhile is the base of the new one. No file - it is created with the section only. Through a
// temporary file: a failed write must not cost the launcher its list. `changed` - the file changed; `written` - the
// value set ("" - the line went); `error` - why it is not written (for the console: "cannot write <file>: <the text of
// the system>")
bool WriteLauncherLineBy(pcstr section_name, const xr_string& key, const LauncherLineValue& make_value, bool& changed,
    xr_string& written, xr_string& error)
{
    using namespace detail;
    changed = false;
    written.clear();
    error.clear();

    string_path file_name;
    FS.update_path(file_name, "$app_data_root$", kLauncherListName);
    const fs::path file = PathFromUtf8(file_name);
    std::error_code ec;
    // The rules of this run are what the file has (g_rules_read_at): the file is the same as then - after the write
    // they are still (the caller changes them as it writes the line)
    fs::file_time_type time_before{};
    const bool had_time = LauncherListTime(file, time_before);
    const bool rules_in_step = had_time == g_rules_file_known && (!had_time || time_before == g_rules_file_time);
    // As ReadLauncherList: only a file that is not there is written anew; one whose status cannot be read (or a
    // folder of that name) is not replaced by a file holding only this line
    const fs::file_status status = fs::status(file, ec);
    const bool exists = status.type() != fs::file_type::not_found;
    xr_vector<char> bytes;
    if (exists && (ec || !fs::is_regular_file(status) || !ReadFileBytes(file, bytes)))
    {
        error = xr_string("cannot read ") + file_name;
        Logf("! ", "addons", "%s: cannot read the file, [%s] %s is not saved", kLauncherListLog, section_name,
            key.c_str());
        return false;
    }

    const xr_string text(bytes.data(), bytes.size());
#if defined(XR_PLATFORM_WINDOWS)
    constexpr bool native_crlf = true;
#else
    constexpr bool native_crlf = false;
#endif
    // A file without any line end yet (empty, one line) takes the one of the platform
    const bool crlf = text.find('\n') != xr_string::npos ? text.find("\r\n") != xr_string::npos : native_crlf;
    const bool final_newline = !exists || text.empty() || text.back() == '\n';
    xr_vector<xr_string> lines;
    for (size_t pos = 0; pos < text.size();)
    {
        size_t end = text.find('\n', pos);
        if (end == xr_string::npos)
            end = text.size();
        xr_string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
        pos = end + 1;
    }
    // What a line says: without the BOM, the comment and the blanks (as ParseSimpleIni reads it)
    const auto meaning = [](xr_string line) {
        if (line.size() >= 3 && static_cast<u8>(line[0]) == 0xEF && static_cast<u8>(line[1]) == 0xBB &&
            static_cast<u8>(line[2]) == 0xBF)
            line.erase(0, 3);
        size_t cut = line.find(';');
        const size_t slashes = line.find("//");
        if (slashes != xr_string::npos && (cut == xr_string::npos || slashes < cut))
            cut = slashes;
        if (cut != xr_string::npos)
            line.erase(cut);
        return Trim(line);
    };
    // A file that is there without a single section is damaged (ReadLauncherList: a write cut short, no plugin loads):
    // a line written into it would turn it into a file of only [slots] / [conflicts] - "no list", every addon on and
    // every plugin unchecked at the next start. The launcher writes it anew
    if (exists && std::none_of(lines.begin(), lines.end(), [&](const xr_string& line) {
            const xr_string text = meaning(line);
            return !text.empty() && text[0] == '[';
        }))
    {
        error = xr_string("cannot write ") + file_name + ": the file has no section (damaged), start the launcher to "
            "write it again";
        Logf("! ", "addons", "%s: the file has no section (damaged), [%s] %s is not saved: start the launcher to write "
            "it again", kLauncherListLog, section_name, key.c_str());
        return false;
    }
    // The first section of the name (ReadLauncherList reads the first one too) and the line after its last line
    size_t section = xr_string::npos, section_end = lines.size();
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const xr_string line = meaning(lines[i]);
        if (line.empty() || line[0] != '[')
            continue;
        if (section != xr_string::npos)
        {
            section_end = i;
            break;
        }
        const size_t close = line.find(']');
        if (close != xr_string::npos && ToLower(Trim(line.substr(1, close - 1))) == section_name)
            section = i;
    }

    const xr_string wanted = LauncherLineKey(key);
    // The lines of the key now (the launcher may have written them since the start of the game)
    xr_vector<size_t> key_lines;
    xr_vector<xr_string> current;
    for (size_t i = section == xr_string::npos ? section_end : section + 1; i < section_end; ++i)
    {
        const xr_string line = meaning(lines[i]);
        const size_t eq = line.find('=');
        if (line.empty() || LauncherLineKey(eq == xr_string::npos ? line : line.substr(0, eq)) != wanted)
            continue;
        key_lines.push_back(i);
        current.push_back(eq == xr_string::npos ? xr_string() : Trim(line.substr(eq + 1)));
    }
    xr_string value;
    if (!make_value(current, value))
    {
        written = current.empty() ? xr_string() : current.front();
        return true; // the file stays as it is
    }
    written = value;
    const bool remove = value.empty();
    const xr_string entry = key + " = " + value;
    if (section != xr_string::npos)
    {
        size_t found = key_lines.empty() ? xr_string::npos : key_lines.front();
        // The key written more than once: the lines after the first go - the readers merge them (pair:) or take the
        // first one (file:, atom:, [slots]), so a line left behind would bring the old rule back at the next start
        for (size_t k = key_lines.size(); k-- > 1;)
        {
            lines.erase(lines.begin() + key_lines[k]);
            changed = true;
        }
        if (found != xr_string::npos && remove)
        {
            lines.erase(lines.begin() + found);
            changed = true;
        }
        else if (found != xr_string::npos)
        {
            changed = changed || lines[found] != entry;
            lines[found] = entry;
        }
        else if (!remove)
        {
            size_t at = section_end; // right after the last line of the section that says something
            while (at > section + 1 && meaning(lines[at - 1]).empty())
                --at;
            lines.insert(lines.begin() + at, entry);
            changed = true;
        }
    }
    else if (!remove)
    {
        while (!lines.empty() && lines.back().empty())
            lines.pop_back();
        if (!lines.empty())
            lines.emplace_back();
        lines.emplace_back(xr_string("[") + section_name + "]");
        lines.push_back(entry);
        changed = true;
    }
    if (!changed)
        return true;

    xr_string result;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        result += lines[i];
        if (i + 1 < lines.size() || final_newline)
            result += crlf ? "\r\n" : "\n";
    }
    // "addons.ltx.<pid>.tmp", as the launcher names its own: another instance of the game sharing the appdata (or the
    // launcher) writing at the same moment never mixes its bytes into this one
#if defined(XR_PLATFORM_WINDOWS)
    const unsigned long pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    fs::path temporary = file;
    temporary += ("." + std::to_string(pid) + ".tmp").c_str();
    {
        errno = 0; // the CRT opens the file: errno says why it failed (read-only, no access, no space)
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(result.data(), static_cast<std::streamsize>(result.size()));
        out.flush();
        if (!out)
        {
            const int code = errno;
            error = "cannot write " + PathToUtf8(temporary);
            if (code)
                error += ": " + ErrorText(std::error_code(code, std::generic_category()));
            Logf("! ", "addons", "%s: %s, [%s] %s = %s is not saved", kLauncherListLog, error.c_str(), section_name,
                key.c_str(), value.c_str());
            written.clear();
            out.close();
            std::error_code remove_ec;
            fs::remove(temporary, remove_ec);
            return false;
        }
    }
    // On the disk before the rename: else a power loss right after it may leave the list of 0 bytes under its name
    xr_string flush_error;
    if (!FlushFileToDisk(temporary, flush_error))
    {
        error = "cannot write " + PathToUtf8(temporary) + " to the disk: " + flush_error;
        Logf("! ", "addons", "%s: %s, [%s] %s = %s is not saved", kLauncherListLog, error.c_str(), section_name,
            key.c_str(), value.c_str());
        written.clear();
        std::error_code remove_ec;
        fs::remove(temporary, remove_ec);
        return false;
    }
    fs::rename(temporary, file, ec);
    if (ec)
    {
        error = xr_string("cannot write ") + file_name + ": " + ErrorText(ec);
        Logf("! ", "addons", "%s: cannot replace the file (%s), [%s] %s = %s is not saved", kLauncherListLog,
            ErrorText(ec).c_str(), section_name, key.c_str(), value.c_str());
        std::error_code remove_ec;
        fs::remove(temporary, remove_ec);
        written.clear();
        return false;
    }
    FlushFolderToDisk(file.parent_path());
    if (rules_in_step)
    {
        g_rules_file_known = LauncherListTime(file, g_rules_file_time);
        g_rules_read_at = static_cast<u64>(std::time(nullptr));
    }
    return true;
}

// WriteLauncherLineBy with a fixed value (a [slots] choice, a file: or atom: rule: the line is the choice itself)
bool WriteLauncherLine(
    pcstr section_name, const xr_string& key, const xr_string& value, bool& changed, xr_string& error)
{
    xr_string written;
    return WriteLauncherLineBy(section_name, key,
        [&value](const xr_vector<xr_string>& /*current*/, xr_string& out) {
            out = value;
            return true;
        },
        changed, written, error);
}
} // namespace

bool WriteSlotChoice(pcstr slot, pcstr provider)
{
    using namespace detail;
    if (!slot || !slot[0])
        return false;
    const xr_string slot_id = ToLower(slot);
    const xr_string provider_id = provider ? ToLower(provider) : xr_string();
    const bool remove = provider_id.empty() || provider_id == "core";

    bool changed = false;
    xr_string error;
    if (!WriteLauncherLine(kLauncherSlotsSection, slot_id, remove ? xr_string() : provider_id, changed, error))
        return false;

    // The choices of this run follow the file: changed only once it is written
    const auto same_slot = [&](const std::pair<xr_string, xr_string>& other) { return other.first == slot_id; };
    const auto known = std::find_if(g_slot_choices.begin(), g_slot_choices.end(), same_slot);
    if (remove && known != g_slot_choices.end())
        g_slot_choices.erase(known);
    else if (!remove && known != g_slot_choices.end())
        known->second = provider_id;
    else if (!remove)
        g_slot_choices.emplace_back(slot_id, provider_id);
    if (changed)
        Logf("* ", "addons", "%s: [%s] %s = %s", kLauncherListLog, kLauncherSlotsSection, slot_id.c_str(),
            remove ? "core (the line removed)" : provider_id.c_str());
    return true;
}

const xr_vector<PairRule>& PairRules() { return detail::g_pair_rules; }
const xr_vector<FileRule>& FileRules() { return detail::g_file_rules; }
const xr_vector<AtomRule>& AtomRules() { return detail::g_atom_rules; }
xr_string EncodeRuleAtom(const xr_string& atom) { return detail::EncodeRuleAtomText(atom); }
bool DecodeRuleAtom(const xr_string& text, xr_string& atom) { return detail::DecodeRuleAtomText(text, atom); }

bool NormalizeRuleFile(pcstr text, xr_string& out) { return text && detail::NormalizeRuleFileText(text, out); }

u64 ConflictRulesReadAt() { return detail::g_rules_read_at; }

bool WriteFileRule(pcstr file, pcstr winner, xr_string* error)
{
    using namespace detail;
    xr_string write_error;
    xr_string& why = error ? *error : write_error;
    why.clear();
    xr_string name;
    if (!NormalizeRuleFile(file, name))
    {
        why = "not a file name";
        return false;
    }
    const xr_string winner_id = winner ? ToLower(winner) : xr_string();
    if (!winner_id.empty() && !IsValidId(winner_id) && winner_id != kCoreWinner)
    {
        why = "not an addon id (a-z 0-9 _) or core";
        return false;
    }

    bool changed = false;
    if (!WriteLauncherLine(kLauncherConflictsSection, kLauncherFilePrefix + name, winner_id, changed, why))
        return false;
    // The rules of this run follow the file: changed only once it is written
    const auto same = [&](const FileRule& rule) { return rule.file == name; };
    const auto known = std::find_if(g_file_rules.begin(), g_file_rules.end(), same);
    if (winner_id.empty() && known != g_file_rules.end())
        g_file_rules.erase(known);
    else if (!winner_id.empty() && known != g_file_rules.end())
        known->winner = winner_id;
    else if (!winner_id.empty())
        g_file_rules.push_back(FileRule{ name, winner_id });
    if (changed)
        Logf("* ", "addons", "%s: [%s] %s%s = %s", kLauncherListLog, kLauncherConflictsSection, kLauncherFilePrefix,
            name.c_str(), winner_id.empty() ? "- (the line removed)" : winner_id.c_str());
    return true;
}

namespace
{
// The line "pair:<winner> = <losers>" of appdata/addons.ltx changed by one step (no loser left: the line goes). The
// losers are the ones the file has right now (the launcher may have added or removed some while the game runs, those
// stay), then: `remove_all` - none; `remove` - without it; `add` - with it at the end. Nothing to change - the file
// stays
bool WritePairLine(
    const xr_string& winner, const xr_string& add, const xr_string& remove, bool remove_all, xr_string& error)
{
    using namespace detail;
    const auto make_value = [&](const xr_vector<xr_string>& current, xr_string& value) {
        // As ReadConflictRules reads them: every line of the key, lower case, the valid ids other than the winner
        xr_vector<xr_string> losers;
        for (const xr_string& line : current)
        {
            const xr_string text = ToLower(line);
            for (size_t start = 0; start <= text.size();)
            {
                size_t comma = text.find(',', start);
                if (comma == xr_string::npos)
                    comma = text.size();
                const xr_string loser = Trim(text.substr(start, comma - start));
                start = comma + 1;
                if (IsValidId(loser) && loser != winner &&
                    std::find(losers.begin(), losers.end(), loser) == losers.end())
                    losers.push_back(loser);
            }
        }
        const size_t before = losers.size();
        if (remove_all)
            losers.clear();
        if (!remove.empty())
            losers.erase(std::remove(losers.begin(), losers.end(), remove), losers.end());
        const bool added = !add.empty() && std::find(losers.begin(), losers.end(), add) == losers.end();
        if (added)
            losers.push_back(add);
        if (losers.size() == before && !added)
            return false; // the file has it so already: its line stays as the launcher wrote it
        value.clear();
        for (const xr_string& loser : losers)
            value += (value.empty() ? "" : ", ") + loser;
        return true;
    };
    bool changed = false;
    xr_string written;
    if (!WriteLauncherLineBy(kLauncherConflictsSection, kLauncherPairPrefix + winner, make_value, changed, written,
            error))
        return false;
    if (changed)
        Logf("* ", "addons", "%s: [%s] %s%s = %s", kLauncherListLog, kLauncherConflictsSection, kLauncherPairPrefix,
            winner.c_str(), written.empty() ? "- (the line removed)" : written.c_str());
    return true;
}
} // namespace

xr_string NormalizeRuleAtom(pcstr atom) { return atom ? detail::RuleAtomLabel(atom) : xr_string(); }

bool WriteAtomRule(pcstr file, pcstr atom, pcstr winner, xr_string* error)
{
    using namespace detail;
    xr_string write_error;
    xr_string& why = error ? *error : write_error;
    why.clear();
    xr_string name;
    if (!NormalizeRuleFile(file, name))
    {
        why = "not a file name";
        return false;
    }
    // "@gap X" and "X" are one atom for the rules: written as "X", the line of the other spelling goes
    // (LauncherLineKey); the launcher writes it so too
    const xr_string label = NormalizeRuleAtom(atom);
    if (label.empty() || label.find_first_of("\r\n") != xr_string::npos)
    {
        why = "not an atom (the labels of addon_merges / addon_merge_show)";
        return false;
    }
    const xr_string winner_id = winner ? ToLower(Trim(winner)) : xr_string();
    if (!winner_id.empty() && !IsValidId(winner_id) && winner_id != kCoreWinner)
    {
        why = "not an addon id (a-z 0-9 _) or core";
        return false;
    }

    bool changed = false;
    const xr_string key = kLauncherAtomPrefix + name + kLauncherAtomSeparator + EncodeRuleAtomText(label);
    if (!WriteLauncherLine(kLauncherConflictsSection, key, winner_id, changed, why))
        return false;
    // The rules of this run follow the file: changed only once it is written. One rule per atom, either spelling of it
    // in the file went to the line of "X" (the first one rewritten, the rest removed)
    const auto same = [&](const AtomRule& rule) { return rule.file == name && RuleAtomLabel(rule.atom) == label; };
    const auto known = std::find_if(g_atom_rules.begin(), g_atom_rules.end(), same);
    if (!winner_id.empty() && known != g_atom_rules.end())
    {
        known->atom = label;
        known->winner = winner_id;
        g_atom_rules.erase(std::remove_if(std::next(known), g_atom_rules.end(), same), g_atom_rules.end());
    }
    else if (!winner_id.empty())
        g_atom_rules.push_back(AtomRule{ name, label, winner_id });
    else
        g_atom_rules.erase(std::remove_if(g_atom_rules.begin(), g_atom_rules.end(), same), g_atom_rules.end());
    if (changed)
        Logf("* ", "addons", "%s: [%s] %s = %s", kLauncherListLog, kLauncherConflictsSection, key.c_str(),
            winner_id.empty() ? "- (the line removed)" : winner_id.c_str());
    return true;
}

bool WritePairRule(pcstr winner, pcstr loser, xr_string* error)
{
    using namespace detail;
    xr_string write_error;
    xr_string& why = error ? *error : write_error;
    why.clear();
    const xr_string winner_id = winner ? ToLower(Trim(winner)) : xr_string();
    const xr_string loser_id = loser ? ToLower(Trim(loser)) : xr_string();
    if (!IsValidId(winner_id) || (!loser_id.empty() && (!IsValidId(loser_id) || loser_id == winner_id)))
    {
        why = "two different addon ids are needed (a-z 0-9 _)";
        return false;
    }

    // The rules of this run follow the file: the new rules are built aside and taken once their lines are written.
    // The lines are written from what the file has at the moment of the write (WritePairLine), not from the rules of
    // this run: a rule the launcher added meanwhile stays in the file
    xr_vector<PairRule> rules = g_pair_rules;
    // No loser: every pair of the winner goes
    if (loser_id.empty())
    {
        const auto of_winner = [&](const PairRule& rule) { return rule.winner == winner_id; };
        rules.erase(std::remove_if(rules.begin(), rules.end(), of_winner), rules.end());
        if (!WritePairLine(winner_id, xr_string(), xr_string(), true, why))
            return false;
        g_pair_rules = std::move(rules);
        return true;
    }
    // The rule the other way round (pair:<loser> = <winner>) would win over the new one (an earlier rule wins over a
    // later one it contradicts): the choice of the player now replaces it
    const auto reverse = std::find_if(rules.begin(), rules.end(),
        [&](const PairRule& rule) { return rule.winner == loser_id && rule.loser == winner_id; });
    const bool had_reverse = reverse != rules.end();
    if (had_reverse)
        rules.erase(reverse);
    // The file without the reverse rule (its line written, the line of the winner not yet): what this run keeps when
    // the second write fails
    const xr_vector<PairRule> without_reverse = rules;
    const auto same = [&](const PairRule& rule) { return rule.winner == winner_id && rule.loser == loser_id; };
    if (std::none_of(rules.begin(), rules.end(), same))
    {
        // Where the file has it: on the line of the winner (right after its last rule), or a new line at the end of
        // [conflicts] (the end of the rules)
        auto at = rules.end();
        for (auto it = rules.begin(); it != rules.end(); ++it)
        {
            if (it->winner == winner_id)
                at = it + 1;
        }
        rules.insert(at, PairRule{ winner_id, loser_id });
    }
    // The reverse rule goes from the file even when only the launcher wrote it (this run does not know it)
    if (!WritePairLine(loser_id, xr_string(), winner_id, false, why))
        return false;
    if (!WritePairLine(winner_id, loser_id, xr_string(), false, why))
    {
        if (had_reverse)
            g_pair_rules = without_reverse;
        return false;
    }
    g_pair_rules = std::move(rules);
    return true;
}

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

namespace
{
// What the states of addon_list mean (00-roadmap.md 5a, decision 63; review 2026-11-06 4.4 A): one legend after the
// list, only for the states it shows. The tables of the states: wiki/doc/plugins/loading.md
pcstr AddonStateMeaning(detail::EAddonState state)
{
    using detail::EAddonState;
    switch (state)
    {
    case EAddonState::Discovered: return "found, not processed yet";
    case EAddonState::Active: return "works, with its plugin or without it";
    case EAddonState::Failed: return "does not work: a manifest error, a dependency, a required plugin (see reason)";
    case EAddonState::Skipped: return "not loaded: a duplicate id, or a patch whose addons are not active (see reason)";
    case EAddonState::Disabled: return "switched off in appdata/addons.ltx or not listed there (enable it in the "
                                       "launcher, or run the game with -addon_dir)";
    }
    return "?";
}

pcstr PluginStateMeaning(detail::EPluginState state)
{
    using detail::EPluginState;
    switch (state)
    {
    case EPluginState::None: return "the addon has no plugin";
    case EPluginState::Loaded: return "loaded and running";
    case EPluginState::Unavailable: return "declared, but not loaded (see reason)";
    case EPluginState::Crashed: return "stopped after a crash (plugin_reload <id>)";
    case EPluginState::Inactive: return "loads while its location pack / slot provider is active";
    }
    return "?";
}

void PrintStateLegend()
{
    using namespace detail;
    constexpr EAddonState addon_states[] = { EAddonState::Active, EAddonState::Failed, EAddonState::Skipped,
        EAddonState::Disabled, EAddonState::Discovered };
    constexpr EPluginState plugin_states[] = { EPluginState::Loaded, EPluginState::Inactive, EPluginState::Unavailable,
        EPluginState::Crashed, EPluginState::None };
    bool header = false;
    const auto line = [&](pcstr kind, pcstr name, pcstr meaning) {
        if (!header)
            Msg("- [addons] legend:");
        header = true;
        Msg("-   %-6s %-11s %s", kind, name, meaning);
    };
    for (const EAddonState state : addon_states)
    {
        const bool shown = std::any_of(g_addons->begin(), g_addons->end(),
            [&](const xr_unique_ptr<AddonRecord>& addon) { return addon->state == state; });
        if (shown)
            line("addon", AddonStateName(state), AddonStateMeaning(state));
    }
    for (const EPluginState state : plugin_states)
    {
        const bool shown = std::any_of(g_addons->begin(), g_addons->end(),
            [&](const xr_unique_ptr<AddonRecord>& addon) { return addon->plugin_state == state; });
        if (shown)
            line("plugin", PluginStateName(state), PluginStateMeaning(state));
    }
}
} // namespace

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
    if (g_launcher_list_unreadable)
        Msg("! [addons] %s", kLauncherListUnreadableReason);
    for (const auto& addon : *g_addons)
    {
        Msg("-   %-24s %-10s %-10s type=%s plugin=%s (%s) lua=%s trust=%s dir=%s%s%s", addon->id.c_str(),
            addon->version.empty() ? "-" : addon->version.c_str(), AddonStateName(addon->state),
            addon->type_name.empty() ? "-" : addon->type_name.c_str(),
            addon->plugin_name.empty() ? "-" : addon->plugin_name.c_str(),
            PluginStateName(addon->plugin_state), addon->lua_entry.empty() ? "-" : addon->lua_entry.c_str(),
            addon->trust.c_str(), addon->dir.c_str(),
            addon->reason.empty() ? "" : " reason=", addon->reason.c_str());
    }
    PrintStateLegend();
    services::PrintList();
}
} // namespace gw::addons
