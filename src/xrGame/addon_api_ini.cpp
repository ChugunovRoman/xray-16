#include "StdAfx.h"

#include "addon_api_ini.h"
#include "addon_host.h"

#include "xrCore/xr_ini.h"

#include <cstdlib>

namespace gw::addons::ini
{
namespace
{
struct FileEntry
{
    CInifile* file = nullptr;
    const GwpPlugin* owner = nullptr; // nullptr for the configs of the engine (system.ltx): never closed
    bool owned = false;               // opened by ini_open: the host destroys it
};

struct SectionEntry
{
    const CInifile* file = nullptr;
    CInifile::Sect* section = nullptr;
    u32 generation = 0;
};

// Handles are indices + 1, so 0 stays "no handle". Entries are never moved, only invalidated.
xr_vector<FileEntry>* g_files = nullptr;
xr_vector<SectionEntry>* g_sections = nullptr;
// "<file handle>/<lowercased section name>" -> section handle: ini_section of the same name gives the same handle.
xr_map<xr_string, GwpIniSection>* g_section_by_name = nullptr;
u32 g_generation = 1;

constexpr size_t kMaxNameLength = 256;

// The system config is handle 1 and is resolved lazily: pSettings exists before any plugin runs, but the table
// is filled once, at the first call.
GwpIni SystemHandle()
{
    if (!g_files)
        g_files = xr_new<xr_vector<FileEntry>>();
    if (g_files->empty())
        g_files->push_back({ const_cast<CInifile*>(pSettings), nullptr, false });
    else
        (*g_files)[0].file = const_cast<CInifile*>(pSettings); // a reload replaces the object
    return (*g_files)[0].file ? 1u : GWP_INVALID_INI;
}

FileEntry* FindFile(GwpIni handle)
{
    if (!g_files || handle == GWP_INVALID_INI || handle > g_files->size())
        return nullptr;
    FileEntry& entry = (*g_files)[handle - 1];
    return entry.file ? &entry : nullptr;
}

// The section of a live handle; nullptr when the handle is unknown or made before a config reload.
CInifile::Sect* FindSection(GwpIniSection handle, pcstr function)
{
    if (!g_sections || handle == GWP_INVALID_INI_SECTION || handle > g_sections->size())
        return nullptr;
    const SectionEntry& entry = (*g_sections)[handle - 1];
    if (entry.generation != g_generation)
    {
        if (IsDebugLog())
            Msg("~ [ini] %s: the section handle is from before a config reload, resolve it again", function);
        return nullptr;
    }
    return entry.section;
}

// Value of a line; nullptr when there is no such line. Keys are compared as they are written in the ltx
// (CInifile::r_string does not lowercase them either).
pcstr LineValue(CInifile::Sect* section, const char* line)
{
    if (!section || !line || !*line)
        return nullptr;
    pcstr value = nullptr;
    return section->line_exist(line, &value) ? value : nullptr;
}

bool CheckCall(pcstr function)
{
    if (IsMainThread())
        return true;
    Msg("! [ini] %s called outside the main thread, ignored", function);
    return false;
}

// ---------------------------------------------------------------------------------------------
// Plugin API (group ini)
// ---------------------------------------------------------------------------------------------

GwpIni GWP_CALL ApiIniSystem()
{
    if (!CheckCall("ini_system"))
        return GWP_INVALID_INI;
    return SystemHandle();
}

GwpIni GWP_CALL ApiIniOpen(const GwpPlugin* self, const char* path)
{
    if (!CheckCall("ini_open") || !self || !path || !*path)
        return GWP_INVALID_INI;

    // An absolute path is taken as it is (a plugin reads a file of its own addon by addon_dir() + name),
    // a relative one is resolved like Lua ini_file() does: inside $game_config$.
    string_path full;
    const bool absolute = strchr(path, ':') || path[0] == '\\' || path[0] == '/';
    if (absolute)
        xr_strcpy(full, path);
    else
        FS.update_path(full, "$game_config$", path);

    // FSType::Any: a file of an addon lies outside the virtual file system of the game, and the default
    // FSType::Virtual would not find it (CInifile itself opens such a file through the external path too).
    if (!FS.exist(full, FSType::Any))
    {
        Msg("! [plugin:%s] ini_open: file '%s' not found", PluginAddonId(self), full);
        return GWP_INVALID_INI;
    }

    SystemHandle(); // keeps the system config at handle 1
    CInifile* file = xr_new<CInifile>(full, true /*read only*/);
    g_files->push_back({ file, self, true });
    return static_cast<GwpIni>(g_files->size());
}

void GWP_CALL ApiIniClose(const GwpPlugin* self, GwpIni handle)
{
    if (!CheckCall("ini_close"))
        return;
    FileEntry* entry = FindFile(handle);
    if (!entry || !entry->owned || entry->owner != self)
        return; // not ours: the system config and files of other plugins are not closed from here
    xr_delete(entry->file);
    entry->owner = nullptr;
    entry->owned = false;
    // Section handles of this file die with it; a stale one is caught by the generation check only after a
    // config reload, so they are invalidated here by hand.
    if (g_sections)
    {
        for (SectionEntry& section : *g_sections)
        {
            if (section.file == entry->file)
                section.generation = 0;
        }
    }
    entry->file = nullptr;
}

uint32_t GWP_CALL ApiIniGeneration() { return g_generation; }

GwpIniSection GWP_CALL ApiIniSection(GwpIni handle, const char* name)
{
    if (!CheckCall("ini_section") || !name || !*name || xr_strlen(name) >= kMaxNameLength)
        return GWP_INVALID_INI_SECTION;
    const FileEntry* entry = FindFile(handle);
    if (!entry)
        return GWP_INVALID_INI_SECTION;

    // Section names are lowercased when the config is parsed, and CInifile::r_section lowercases the query:
    // do the same, so "Wpn_AK74" and "wpn_ak74" give the same handle.
    string256 lower;
    xr_strcpy(lower, name);
    xr_strlwr(lower);

    if (!g_section_by_name)
        g_section_by_name = xr_new<xr_map<xr_string, GwpIniSection>>();
    string512 key;
    xr_sprintf(key, "%u/%s", handle, lower);
    const auto cached = g_section_by_name->find(key);
    if (cached != g_section_by_name->end())
    {
        const SectionEntry& known = (*g_sections)[cached->second - 1];
        if (known.generation == g_generation)
            return cached->second;
        g_section_by_name->erase(cached); // the config was reloaded: resolve again
    }

    if (!entry->file->section_exist(lower))
        return GWP_INVALID_INI_SECTION;

    if (!g_sections)
        g_sections = xr_new<xr_vector<SectionEntry>>();
    g_sections->push_back({ entry->file, &entry->file->r_section(lower), g_generation });
    const GwpIniSection result = static_cast<GwpIniSection>(g_sections->size());
    g_section_by_name->emplace(key, result);
    return result;
}

int GWP_CALL ApiIniLineExists(GwpIniSection section, const char* line)
{
    if (!CheckCall("ini_line_exists"))
        return 0;
    return LineValue(FindSection(section, "ini_line_exists"), line) != nullptr ? 1 : 0;
}

const char* GWP_CALL ApiIniReadString(GwpIniSection section, const char* line, const char* def)
{
    if (!CheckCall("ini_read_string"))
        return def;
    const pcstr value = LineValue(FindSection(section, "ini_read_string"), line);
    return value ? value : def;
}

double GWP_CALL ApiIniReadNumber(GwpIniSection section, const char* line, double def)
{
    if (!CheckCall("ini_read_number"))
        return def;
    const pcstr value = LineValue(FindSection(section, "ini_read_number"), line);
    return value ? atof(value) : def;
}

int GWP_CALL ApiIniReadBool(GwpIniSection section, const char* line, int def)
{
    if (!CheckCall("ini_read_bool"))
        return def;
    const pcstr value = LineValue(FindSection(section, "ini_read_bool"), line);
    if (!value)
        return def;
    // Same set of true words as CInifile::r_bool, and the same lowercasing of the value.
    string64 lower;
    xr_strcpy(lower, value);
    xr_strlwr(lower);
    return CInifile::isBool(lower) ? 1 : 0;
}

int GWP_CALL ApiIniReadVec3(GwpIniSection section, const char* line, float out_xyz[3])
{
    if (out_xyz)
        out_xyz[0] = out_xyz[1] = out_xyz[2] = 0.f;
    if (!CheckCall("ini_read_vec3") || !out_xyz)
        return 0;
    const pcstr value = LineValue(FindSection(section, "ini_read_vec3"), line);
    if (!value)
        return 0;
    return sscanf(value, "%f,%f,%f", &out_xyz[0], &out_xyz[1], &out_xyz[2]) == 3 ? 1 : 0;
}

uint32_t GWP_CALL ApiIniLineCount(GwpIniSection section)
{
    if (!CheckCall("ini_line_count"))
        return 0;
    const CInifile::Sect* sect = FindSection(section, "ini_line_count");
    return sect ? static_cast<uint32_t>(sect->Data.size()) : 0;
}

int GWP_CALL ApiIniLineAt(GwpIniSection section, uint32_t index, const char** out_name, const char** out_value)
{
    if (out_name)
        *out_name = nullptr;
    if (out_value)
        *out_value = nullptr;
    if (!CheckCall("ini_line_at") || !out_name)
        return 0;
    const CInifile::Sect* sect = FindSection(section, "ini_line_at");
    if (!sect || index >= sect->Data.size())
        return 0;
    const CInifile::Item& item = sect->Data[index];
    *out_name = item.first.c_str();
    if (out_value)
        *out_value = item.second.c_str(); // a line without a value gives an empty string
    return 1;
}
} // namespace

void FillEngineApi(GwpEngineApi& api)
{
    api.ini_system = &ApiIniSystem;
    api.ini_open = &ApiIniOpen;
    api.ini_close = &ApiIniClose;
    api.ini_generation = &ApiIniGeneration;
    api.ini_section = &ApiIniSection;
    api.ini_line_exists = &ApiIniLineExists;
    api.ini_read_string = &ApiIniReadString;
    api.ini_read_number = &ApiIniReadNumber;
    api.ini_read_bool = &ApiIniReadBool;
    api.ini_read_vec3 = &ApiIniReadVec3;
    api.ini_line_count = &ApiIniLineCount;
    api.ini_line_at = &ApiIniLineAt;
}

void OnConfigsReloaded()
{
    ++g_generation;
    if (g_section_by_name)
        g_section_by_name->clear();
    if (g_sections)
        g_sections->clear(); // every handle is stale now: the generation check catches the old ones
    if (g_files && !g_files->empty())
        (*g_files)[0].file = const_cast<CInifile*>(pSettings);
}

void ClosePluginFiles(const GwpPlugin* plugin)
{
    if (!g_files)
        return;
    for (size_t i = 0; i < g_files->size(); ++i)
    {
        FileEntry& entry = (*g_files)[i];
        if (entry.owned && entry.owner == plugin)
            ApiIniClose(plugin, static_cast<GwpIni>(i + 1));
    }
}

void PrintList()
{
    Msg("- [ini] config generation %u", g_generation);
    u32 open_files = 0;
    if (g_files)
    {
        for (size_t i = 0; i < g_files->size(); ++i)
        {
            const FileEntry& entry = (*g_files)[i];
            if (!entry.file)
                continue;
            ++open_files;
            Msg("-   file %u  %-24s %s", static_cast<u32>(i + 1),
                entry.owner ? PluginAddonId(entry.owner) : "<engine>", entry.file->fname());
        }
    }

    // Section handles are what makes reads cheap: each one is resolved once and used many times.
    u32 live = 0, stale = 0;
    if (g_sections)
    {
        for (const SectionEntry& section : *g_sections)
        {
            if (section.generation == g_generation && section.section)
                ++live;
            else
                ++stale;
        }
    }
    Msg("- [ini] %u file(s) open, %u section handle(s) live, %u stale (from before a config reload)", open_files,
        live, stale);
}

void Shutdown()
{
    if (g_files)
    {
        for (FileEntry& entry : *g_files)
        {
            if (entry.owned)
                xr_delete(entry.file);
        }
    }
    xr_delete(g_files);
    xr_delete(g_sections);
    xr_delete(g_section_by_name);
}
} // namespace gw::addons::ini
