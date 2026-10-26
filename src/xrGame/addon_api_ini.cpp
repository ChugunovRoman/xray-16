#include "StdAfx.h"

#include "addon_api_ini.h"
#include "addon_host.h"

#include "xrCore/xr_ini.h"
#include "xrCore/Profiling/IniLoadStats.hpp"

#include <cstdlib>

namespace gw::addons::ini
{
namespace
{
// Handle = (serial << kSlotBits) | (slot + 1), so 0 stays "no handle". A record that is freed (ini_close, the
// unload of its plugin, a config reload for the sections) goes to a free list and is reused by the next ini_open /
// ini_section; its serial is bumped on the free, so a handle kept by a plugin after that fails the serial check
// instead of reading the file or section that took the slot. The tables are bounded by what is live at a time: the
// open files and the sections resolved in them since the last config reload.
constexpr u32 kSlotBits = 20;
constexpr u32 kSlotMask = (1u << kSlotBits) - 1; // slot + 1
constexpr u32 kSerialMask = (1u << (32 - kSlotBits)) - 1;

u32 MakeHandle(size_t slot, u32 serial) { return (serial << kSlotBits) | static_cast<u32>(slot + 1); }
u32 NextSerial(u32 serial) { return (serial + 1) & kSerialMask; }

struct FileEntry
{
    CInifile* file = nullptr;
    const GwpPlugin* owner = nullptr; // nullptr for the configs of the engine (system.ltx): never closed
    bool owned = false;               // opened by ini_open: the host destroys it
    u32 serial = 0;
    // lowercased section name -> section handle: ini_section of the same name gives the same handle. The records
    // of these sections are freed with the file (ini_close) and on a config reload.
    xr_map<xr_string, GwpIniSection> sections;
};

struct SectionEntry
{
    CInifile::Sect* section = nullptr; // nullptr: a free record
    u32 serial = 0;
};

struct IniTables
{
    xr_vector<FileEntry> files; // slot 0: the system config, never freed
    xr_vector<SectionEntry> sections;
    xr_vector<u32> free_files; // slots of closed files, reused by ini_open
    xr_vector<u32> free_sections;
};

IniTables* g_tables = nullptr;
u32 g_generation = 1;

constexpr size_t kMaxNameLength = 256;

IniTables& Tables()
{
    if (!g_tables)
        g_tables = xr_new<IniTables>();
    return *g_tables;
}

// The system config is handle 1 (slot 0, serial 0) and is resolved lazily: pSettings exists before any plugin runs,
// but the table is filled once, at the first call.
GwpIni SystemHandle()
{
    IniTables& tables = Tables();
    if (tables.files.empty())
        tables.files.emplace_back();
    tables.files[0].file = const_cast<CInifile*>(pSettings); // a reload replaces the object
    return tables.files[0].file ? MakeHandle(0, 0) : GWP_INVALID_INI;
}

FileEntry* FindFile(GwpIni handle)
{
    const u32 slot = (handle & kSlotMask) - 1;
    if (!g_tables || handle == GWP_INVALID_INI || slot >= g_tables->files.size())
        return nullptr;
    FileEntry& entry = g_tables->files[slot];
    return entry.file && entry.serial == (handle >> kSlotBits) ? &entry : nullptr;
}

// The section of a live handle; nullptr when the handle is unknown, of a closed file or made before a config reload.
CInifile::Sect* FindSection(GwpIniSection handle, pcstr function)
{
    const u32 slot = (handle & kSlotMask) - 1;
    if (!g_tables || handle == GWP_INVALID_INI_SECTION || slot >= g_tables->sections.size())
        return nullptr;
    const SectionEntry& entry = g_tables->sections[slot];
    if (!entry.section || entry.serial != (handle >> kSlotBits))
    {
        if (IsDebugLog())
            Msg("~ [ini] %s: the section handle is stale (its file was closed or the configs were reloaded), "
                "resolve it again",
                function);
        return nullptr;
    }
    return entry.section;
}

// Frees the records of every section resolved in the file: their handles fail from now on.
void ReleaseFileSections(FileEntry& file)
{
    for (const auto& [name, handle] : file.sections)
    {
        const u32 slot = (handle & kSlotMask) - 1;
        SectionEntry& entry = g_tables->sections[slot];
        entry.section = nullptr;
        entry.serial = NextSerial(entry.serial);
        g_tables->free_sections.push_back(slot);
    }
    file.sections.clear();
}

// A slot for a new record: a freed one first, else a new one at the end; kSlotMask when the handle space is full
// (a million live records, which a game never reaches).
template <typename Entry>
u32 TakeSlot(xr_vector<Entry>& entries, xr_vector<u32>& free_slots)
{
    if (!free_slots.empty())
    {
        const u32 slot = free_slots.back();
        free_slots.pop_back();
        return slot;
    }
    if (entries.size() >= kSlotMask)
        return kSlotMask;
    entries.emplace_back();
    return static_cast<u32>(entries.size() - 1);
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
    // A longer path would hit the invalid parameter handler in the copy below (a fatal error, not an answer)
    if (xr_strlen(path) >= sizeof(full))
    {
        Msg("! [plugin:%s] ini_open: the path is longer than %u characters", PluginAddonId(self),
            static_cast<u32>(sizeof(full) - 1));
        return GWP_INVALID_INI;
    }
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

    SystemHandle(); // keeps the system config at slot 0
    IniTables& tables = Tables();
    const u32 slot = TakeSlot(tables.files, tables.free_files);
    if (slot == kSlotMask)
    {
        Msg("! [plugin:%s] ini_open: too many open files", PluginAddonId(self));
        return GWP_INVALID_INI;
    }
    INI_SOURCE(Plugin, PluginAddonId(self)); // P20
    FileEntry& entry = tables.files[slot];
    entry.file = xr_new<CInifile>(full, true /*read only*/);
    entry.owner = self;
    entry.owned = true;
    return MakeHandle(slot, entry.serial);
}

void GWP_CALL ApiIniClose(const GwpPlugin* self, GwpIni handle)
{
    if (!CheckCall("ini_close"))
        return;
    FileEntry* entry = FindFile(handle);
    if (!entry || !entry->owned || entry->owner != self)
        return; // not ours: the system config and files of other plugins are not closed from here
    // The records of the file and of its sections are freed for reuse; the serials bumped make every handle of them
    // that a plugin still keeps fail (an owned file is never in slot 0, the one of the system config)
    ReleaseFileSections(*entry);
    xr_delete(entry->file);
    entry->owner = nullptr;
    entry->owned = false;
    entry->serial = NextSerial(entry->serial);
    g_tables->free_files.push_back((handle & kSlotMask) - 1);
}

uint32_t GWP_CALL ApiIniGeneration() { return g_generation; }

GwpIniSection GWP_CALL ApiIniSection(GwpIni handle, const char* name)
{
    if (!CheckCall("ini_section") || !name || !*name || xr_strlen(name) >= kMaxNameLength)
        return GWP_INVALID_INI_SECTION;
    FileEntry* entry = FindFile(handle);
    if (!entry)
        return GWP_INVALID_INI_SECTION;

    // Section names are lowercased when the config is parsed, and CInifile::r_section lowercases the query:
    // do the same, so "Wpn_AK74" and "wpn_ak74" give the same handle.
    string256 lower;
    xr_strcpy(lower, name);
    xr_strlwr(lower);

    // The map of the file is cleared with its section records (ini_close, a config reload): what it holds is live
    const auto cached = entry->sections.find(lower);
    if (cached != entry->sections.end())
        return cached->second;

    if (!entry->file->section_exist(lower))
        return GWP_INVALID_INI_SECTION; // a missing section takes no record

    IniTables& tables = *g_tables;
    const u32 slot = TakeSlot(tables.sections, tables.free_sections);
    if (slot == kSlotMask)
    {
        Msg("! [ini] ini_section: too many section handles");
        return GWP_INVALID_INI_SECTION;
    }
    SectionEntry& record = tables.sections[slot];
    record.section = &entry->file->r_section(lower);
    const GwpIniSection result = MakeHandle(slot, record.serial);
    entry->sections.emplace(lower, result);
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
    // Same set of true words as CInifile::r_bool, and the same lowercasing of the value. A longer value is no
    // true word, and the copy would hit the invalid parameter handler
    string64 lower;
    if (xr_strlen(value) >= sizeof(lower))
        return 0;
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
    if (!g_tables)
        return;
    // Every section handle dies: its record is freed for reuse (the serial bumped makes the old handle fail, and a
    // plugin resolves it again). The file handles stay: an ini_open file keeps its object, the system config gets
    // the new one.
    for (FileEntry& entry : g_tables->files)
        ReleaseFileSections(entry);
    if (!g_tables->files.empty())
        g_tables->files[0].file = const_cast<CInifile*>(pSettings);
}

void ClosePluginFiles(const GwpPlugin* plugin)
{
    if (!g_tables)
        return;
    for (size_t i = 0; i < g_tables->files.size(); ++i)
    {
        const FileEntry& entry = g_tables->files[i];
        if (entry.owned && entry.owner == plugin)
            ApiIniClose(plugin, MakeHandle(i, entry.serial));
    }
}

void PrintList()
{
    Msg("- [ini] config generation %u", g_generation);
    if (!g_tables)
    {
        Msg("- [ini] no file open");
        return;
    }
    u32 open_files = 0;
    for (size_t i = 0; i < g_tables->files.size(); ++i)
    {
        const FileEntry& entry = g_tables->files[i];
        if (!entry.file)
            continue;
        ++open_files;
        Msg("-   file %u  %-24s %4u section(s)  %s", MakeHandle(i, entry.serial),
            entry.owner ? PluginAddonId(entry.owner) : "<engine>", static_cast<u32>(entry.sections.size()),
            entry.file->fname());
    }

    // Section handles are what makes reads cheap: each one is resolved once and used many times. The free records
    // (closed files, config reloads) are reused by the next ini_open / ini_section.
    const u32 free_sections = static_cast<u32>(g_tables->free_sections.size());
    Msg("- [ini] %u file(s) open, %u section handle(s) live; records: %u file(s), %u section(s), free %u / %u",
        open_files, static_cast<u32>(g_tables->sections.size()) - free_sections,
        static_cast<u32>(g_tables->files.size()), static_cast<u32>(g_tables->sections.size()),
        static_cast<u32>(g_tables->free_files.size()), free_sections);
}

void Shutdown()
{
    if (g_tables)
    {
        for (FileEntry& entry : g_tables->files)
        {
            if (entry.owned)
                xr_delete(entry.file);
        }
    }
    xr_delete(g_tables);
}
} // namespace gw::addons::ini
