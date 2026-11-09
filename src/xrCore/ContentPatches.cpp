#include "stdafx.h"

#include "ContentPatches.h"

#include <algorithm>
#include <mutex>

#include "xrCommon/xr_map.h"
#include "xrCommon/xr_set.h"
#include "FS_internal.h" // TryCreateDiskFileReader

namespace xray::content_patches
{
namespace
{
// The patch files of one folder for one extension
struct ContentPatchFolder
{
    // <stem> of a target -> its patch files in the order of application
    xr_map<xr_string, xr_vector<ModFile>> by_target;
    xr_set<xr_string> patch_names; // the file names (in the folder) of the patch files that have a target
};

struct ContentPatchAtom
{
    xr_vector<xr_string> owners; // in the order of the first contribution
};

struct ContentPatchState
{
    std::mutex lock;
    bool config_read{};
    xr_string config_prefix; // $game_config$ in lower case, with the trailing delimiter
    xr_vector<xr_string> layers; // the layer ids the cache was built for
    xr_map<xr_string, ContentPatchFolder> folders; // "<folder>|<ext>"
    // "<kind>|<file>|<atom>"; the parts are kept for the report
    struct AtomKey
    {
        AtomKind kind;
        xr_string file;
        xr_string atom;
        bool operator<(const AtomKey& other) const
        {
            if (kind != other.kind)
                return kind < other.kind;
            if (file != other.file)
                return file < other.file;
            return atom < other.atom;
        }
    };
    xr_map<AtomKey, ContentPatchAtom> atoms;
    xr_set<xr_string> logged;
    xr_map<xr_string, bool> any_patches; // ext -> some mod_*.<ext> lies under $game_config$
    // The mod_* files under $game_config$ covered by a layer above (the index name -> the layers of the covered
    // entries, 0 - the base): two addons with a patch of one name. Read once per set of layers.
    bool covered_read{};
    xr_map<xr_string, xr_vector<u16>> covered_patches;
};

ContentPatchState& ContentPatches()
{
    static ContentPatchState state;
    return state;
}

// As the index spells the names (xr_fs_strlwr): lower case on Windows, as is where the file system tells the case
char ContentPatchLowerChar(char c)
{
#if defined(XR_PLATFORM_WINDOWS)
    return static_cast<char>(tolower(static_cast<u8>(c)));
#else
    return c;
#endif
}

void ContentPatchLower(xr_string& s)
{
    for (char& c : s)
        c = ContentPatchLowerChar(c);
}

// "mod_..." in any case
bool ContentPatchIsModName(const xr_string& name)
{
    return name.size() > 4 && tolower(static_cast<u8>(name[0])) == 'm' && tolower(static_cast<u8>(name[1])) == 'o' &&
        tolower(static_cast<u8>(name[2])) == 'd' && name[3] == '_';
}

bool ContentPatchIsDelimiter(char c) { return c == '\\' || c == '/'; }

// Called under the lock
void ContentPatchReadConfigPrefix(ContentPatchState& S)
{
    if (S.config_read)
        return;
    FS_Path* game_config = nullptr;
    if (!FS.get_path("$game_config$", &game_config) || !game_config || !game_config->m_Path)
        return; // not yet: asked again next time
    S.config_read = true;
    S.config_prefix = game_config->m_Path;
    ContentPatchLower(S.config_prefix);
    for (char& c : S.config_prefix)
    {
        if (ContentPatchIsDelimiter(c))
            c = _DELIMITER;
    }
    if (!S.config_prefix.empty() && !ContentPatchIsDelimiter(S.config_prefix.back()))
        S.config_prefix += _DELIMITER;
}

// Called under the lock: a mount or an unmount of a layer drops what was built for the previous set of layers
void ContentPatchCheckLayers(ContentPatchState& S)
{
    const u16 count = FS.layers_count();
    bool same = S.layers.size() == count;
    for (u16 i = 0; same && i < count; ++i)
    {
        const pcstr id = FS.layer_id(u16(i + 1));
        same = S.layers[i] == (id ? id : "");
    }
    if (same)
        return;
    S.layers.clear();
    for (u16 i = 0; i < count; ++i)
    {
        const pcstr id = FS.layer_id(u16(i + 1));
        S.layers.emplace_back(id ? id : "");
    }
    S.folders.clear();
    S.atoms.clear();
    S.logged.clear();
    S.any_patches.clear();
    S.covered_read = false;
    S.covered_patches.clear();
}

// The owner of the layer (see ContentPatches.h)
void ContentPatchLayerOwner(u16 layer, xr_string& owner)
{
    if (!layer)
    {
        owner = kBaseOwner;
        return;
    }
    pcstr name = FS.layer_owner(layer);
    if (!name)
        name = FS.layer_id(layer);
    owner = name ? name : kBaseOwner;
}

void ContentPatchOwnerOf(pcstr full_name, u16& layer, xr_string& owner)
{
    layer = 0;
    if (const CLocatorAPI::file* desc = FS.GetFileDesc(full_name))
        layer = desc->layer;
    ContentPatchLayerOwner(layer, owner);
}

// Called under the lock (the patch lock is taken before the lock of the index, as everywhere): the covered mod_*
// files of every layer, once per set of layers. A layer keeps the entry it covers with the layer of that entry: the
// chain of the entries of one name is the union of them.
void ContentPatchReadCovered(ContentPatchState& S)
{
    if (S.covered_read)
        return;
    S.covered_read = true;
    const u16 count = FS.layers_count();
    xr_vector<CLocatorAPI::layer_covered_entry> entries;
    for (u16 index = 1; index <= count; ++index)
    {
        FS.get_layer_covered(index, entries);
        for (const CLocatorAPI::layer_covered_entry& entry : entries)
        {
            const xr_string& name = entry.name;
            if (name.size() <= S.config_prefix.size() || 0 != name.compare(0, S.config_prefix.size(), S.config_prefix))
                continue;
            const size_t slash = name.find_last_of("\\/");
            if (slash == xr_string::npos || !ContentPatchIsModName(name.substr(slash + 1)))
                continue;
            auto& layers = S.covered_patches[name];
            if (std::find(layers.begin(), layers.end(), entry.under) == layers.end())
                layers.push_back(entry.under);
        }
    }
}

// Called under the lock: the patch `top` (the file the index gives by its name) and the patches of the same name the
// layers above cover, in `out`. The lower ones of another owner are applied too; the base one and the ones of the
// owner of a layer above are replaced (see ContentPatches.h).
void ContentPatchAddCovered(ContentPatchState& S, ModFile&& top, xr_vector<ModFile>& out)
{
    ContentPatchReadCovered(S);
    const auto covered = S.covered_patches.find(top.name);
    if (covered == S.covered_patches.end())
    {
        out.emplace_back(std::move(top));
        return;
    }
    xr_vector<u16> layers = covered->second;
    std::sort(layers.begin(), layers.end(), [](u16 a, u16 b) { return a > b; }); // from the top down
    xr_vector<xr_string> owners{ top.owner }; // the applied ones, from the top down
    const pcstr short_name = top.name.c_str() + S.config_prefix.size();
    for (const u16 layer : layers)
    {
        xr_string owner;
        ContentPatchLayerOwner(layer, owner);
        if (!layer || std::find(owners.begin(), owners.end(), owner) != owners.end())
        {
            // A file of the base or of the same addon under its own one: replaced, as any file
            xr_string text = "~ [addon:";
            if (!layer)
            {
                text += top.owner;
                text += "] ";
                text += short_name;
                text += " replaces the patch of the base of the same name - that one is not applied";
            }
            else
            {
                const pcstr id = FS.layer_id(layer);
                text += owner;
                text += "] ";
                text += short_name;
                text += " of its layer '";
                text += id ? id : "?";
                text += "' is covered by the file of the same name of its upper layer - not applied";
            }
            if (S.logged.emplace(text).second)
                Msg("%s", text.c_str());
            continue;
        }
        ModFile lower;
        lower.name = top.name;
        lower.layer = layer;
        lower.owner = owner;
        CLocatorAPI::file desc{};
        desc.name = top.name.c_str();
        desc.layer = layer;
        string_path physical;
        if (!FS.physical_name(desc, physical))
            continue;
        lower.physical = physical;
        owners.emplace_back(owner);
        out.emplace_back(std::move(lower));
    }
    if (owners.size() > 1)
    {
        xr_string text = "* [patches] ";
        text += short_name;
        text += " is given by several addons (";
        for (auto it = owners.rbegin(); it != owners.rend(); ++it)
        {
            if (it != owners.rbegin())
                text += ", ";
            text += *it;
        }
        text += "): each one is applied, in this order";
        if (S.logged.emplace(text).second)
            Msg("%s", text.c_str());
    }
    out.emplace_back(std::move(top));
}

// Called under the lock: the listing of `folder` (lower case, with the trailing delimiter) for `ext` ("ltx")
const ContentPatchFolder& ContentPatchListFolder(ContentPatchState& S, const xr_string& folder, pcstr ext)
{
    xr_string key = folder;
    key += '|';
    key += ext;
    const auto found = S.folders.find(key);
    if (found != S.folders.end())
        return found->second;

    ContentPatchFolder& result = S.folders[key];
    const xr_string mask = xr_string("*.") + ext;
    FS_FileSet files;
    FS.file_list(files, folder.c_str(), FS_ListFiles | FS_RootOnly, mask.c_str());
    if (files.empty())
        return result;

    const size_t ext_length = xr_strlen(ext) + 1; // ".ltx"
    xr_set<xr_string> stems; // the files that are not patches: the targets
    xr_vector<xr_string> candidates; // mod_*
    for (const FS_File& file : files)
    {
        xr_string name = file.name.c_str();
        ContentPatchLower(name);
        if (name.size() <= ext_length)
            continue;
        if (ContentPatchIsModName(name))
            candidates.emplace_back(std::move(name));
        else
            stems.emplace(name.substr(0, name.size() - ext_length));
    }

    for (const xr_string& name : candidates)
    {
        // mod_<stem>_<rest>.<ext>: the longest <stem> among the files of the folder
        const xr_string rest = name.substr(4, name.size() - 4 - ext_length);
        const xr_string* target = nullptr;
        for (size_t pos = rest.rfind('_'); pos != xr_string::npos && pos > 0; pos = rest.rfind('_', pos - 1))
        {
            if (pos + 1 >= rest.size())
                continue; // "mod_x_.ltx": nothing after the stem
            const auto stem = stems.find(rest.substr(0, pos));
            if (stem != stems.end())
            {
                target = &*stem;
                break;
            }
        }
        if (!target)
        {
            // A file of its own, not a patch (or a patch of a file that is not here). The base may have such files; one
            // of an addon is most likely a patch with a typo in its name or in the wrong folder: said once, so that the
            // author learns why it does nothing
            const xr_string full_name = folder + name;
            u16 layer = 0;
            xr_string owner;
            ContentPatchOwnerOf(full_name.c_str(), layer, owner);
            if (layer != 0)
            {
                xr_string text = "~ [patches] [";
                text += owner;
                text += "] ";
                // As the author lays it out: gamedata\configs\<...> (folder lies under $game_config$, ContentPatchSplit)
                text += full_name.compare(0, S.config_prefix.size(), S.config_prefix) == 0 ?
                    xr_string("gamedata") + _DELIMITER + "configs" + _DELIMITER +
                        full_name.substr(S.config_prefix.size()) : full_name;
                text += ": no file of this folder is its target (mod_<target>_<name>.";
                text += ext;
                text += " patches <target>.";
                text += ext;
                text += " next to it), it is not applied as a patch - check the name and the folder";
                if (S.logged.emplace(text).second)
                    Msg("%s", text.c_str());
            }
            continue;
        }

        ModFile mod;
        mod.name = folder + name;
        ContentPatchOwnerOf(mod.name.c_str(), mod.layer, mod.owner);
        ContentPatchAddCovered(S, std::move(mod), result.by_target[*target]); // the same name in lower layers too
        result.patch_names.emplace(name);
    }
    for (auto& [target, mods] : result.by_target)
    {
        std::sort(mods.begin(), mods.end(), [](const ModFile& a, const ModFile& b)
        {
            if (a.layer != b.layer)
                return a.layer < b.layer;
            if (a.name != b.name)
                return a.name < b.name;
            return a.physical < b.physical;
        });
    }
    return result;
}

// full_name -> folder (lower case, with the delimiter), the file name and the stem (lower case); false - not a name
// under $game_config$
bool ContentPatchSplit(ContentPatchState& S, pcstr full_name, xr_string& folder, xr_string& file, xr_string& ext)
{
    if (!full_name || !full_name[0])
        return false;
    ContentPatchReadConfigPrefix(S);
    if (S.config_prefix.empty())
        return false;
    xr_string name = full_name;
    ContentPatchLower(name);
    // The index spells the separators as _DELIMITER (restore_path_separators): "plugins/x/y.ltx" of a script too
    for (char& c : name)
    {
        if (ContentPatchIsDelimiter(c))
            c = _DELIMITER;
    }
    if (name.size() <= S.config_prefix.size() || 0 != name.compare(0, S.config_prefix.size(), S.config_prefix))
        return false;
    size_t slash = xr_string::npos;
    for (size_t i = name.size(); i > 0; --i)
    {
        if (ContentPatchIsDelimiter(name[i - 1]))
        {
            slash = i - 1;
            break;
        }
    }
    if (slash == xr_string::npos)
        return false;
    folder = name.substr(0, slash + 1);
    file = name.substr(slash + 1);
    const size_t dot = file.rfind('.');
    if (dot == xr_string::npos || dot == 0 || dot + 1 >= file.size())
        return false;
    ext = file.substr(dot + 1);
    return true;
}
} // namespace

bool FindModFiles(pcstr full_name, xr_vector<ModFile>& out)
{
    out.clear();
    ContentPatchState& S = ContentPatches();
    std::lock_guard<std::mutex> guard(S.lock);
    xr_string folder, file, ext;
    if (!ContentPatchSplit(S, full_name, folder, file, ext))
        return false;
    if (ContentPatchIsModName(file))
        return false; // a patch file opened by itself: not a root
    ContentPatchCheckLayers(S);
    const ContentPatchFolder& listing = ContentPatchListFolder(S, folder, ext.c_str());
    if (listing.by_target.empty())
        return false;
    const auto found = listing.by_target.find(file.substr(0, file.size() - ext.size() - 1));
    if (found == listing.by_target.end())
        return false;
    out = found->second;
    return true;
}

bool IsPatchFile(pcstr full_name)
{
    ContentPatchState& S = ContentPatches();
    std::lock_guard<std::mutex> guard(S.lock);
    xr_string folder, file, ext;
    if (!ContentPatchSplit(S, full_name, folder, file, ext))
        return false;
    if (!ContentPatchIsModName(file))
        return false;
    ContentPatchCheckLayers(S);
    const ContentPatchFolder& listing = ContentPatchListFolder(S, folder, ext.c_str());
    return listing.patch_names.find(file) != listing.patch_names.end();
}

bool AnyModFiles(pcstr ext)
{
    ContentPatchState& S = ContentPatches();
    std::lock_guard<std::mutex> guard(S.lock);
    ContentPatchReadConfigPrefix(S);
    if (S.config_prefix.empty() || !ext || !ext[0])
        return false;
    ContentPatchCheckLayers(S);
    xr_string key = ext;
    ContentPatchLower(key);
    const auto found = S.any_patches.find(key);
    if (found != S.any_patches.end())
        return found->second;
    // One walk of the configs per set of layers: '*' of the mask takes the separators too
    FS_FileSet files;
    const xr_string mask = xr_string("*mod_*.") + key;
    FS.file_list(files, S.config_prefix.c_str(), FS_ListFiles, mask.c_str());
    bool any = false;
    for (const FS_File& file : files)
    {
        const size_t slash = file.name.find_last_of("\\/");
        if (ContentPatchIsModName(slash == xr_string::npos ? file.name : file.name.substr(slash + 1)))
        {
            any = true;
            break;
        }
    }
    S.any_patches[key] = any;
    return any;
}

void FileOwner(pcstr full_name, xr_string& out)
{
    xr_string name = full_name ? full_name : "";
    ContentPatchLower(name);
    u16 layer;
    ContentPatchOwnerOf(name.c_str(), layer, out);
}

u16 FileLayer(pcstr full_name)
{
    xr_string name = full_name ? full_name : "";
    ContentPatchLower(name);
    const CLocatorAPI::file* desc = FS.GetFileDesc(name.c_str());
    return desc ? desc->layer : 0;
}

pcstr ShortName(pcstr full_name)
{
    if (!full_name)
        return "";
    ContentPatchState& S = ContentPatches();
    size_t length;
    {
        std::lock_guard<std::mutex> guard(S.lock);
        ContentPatchReadConfigPrefix(S);
        // Read under the lock: another thread may be setting it now. Once set it never changes, the characters are
        // read without the lock below.
        length = S.config_read ? S.config_prefix.size() : 0;
    }
    if (!length)
        return full_name;
    for (size_t i = 0; i < length; ++i)
    {
        const char c = full_name[i];
        if (!c)
            return full_name;
        const char lower = ContentPatchLowerChar(c); // the case of the prefix: lower on Windows only
        if (lower != S.config_prefix[i] && !(ContentPatchIsDelimiter(lower) && ContentPatchIsDelimiter(S.config_prefix[i])))
            return full_name;
    }
    return full_name + length;
}

void Invalidate()
{
    ContentPatchState& S = ContentPatches();
    std::lock_guard<std::mutex> guard(S.lock);
    S.folders.clear();
    S.atoms.clear();
    S.logged.clear();
    // The ids of the layers may stay the same (a layer unmounted and mounted again with other files): the walk of the
    // configs is redone too, else DLTX would stay off (or on) by the listing of the previous files
    S.any_patches.clear();
    S.covered_read = false;
    S.covered_patches.clear();
}

IReader* OpenModFile(const ModFile& file)
{
    if (file.physical.empty())
        return FS.r_open(file.name.c_str());
    // A covered patch: its name gives the file of the layer above, the file on the disk is read, as r_open reads a
    // file of a layer (a UTF-8 path, '\' converted on POSIX). Not FS.r_open of the full path: it looks an unknown
    // name up with a narrow stat (fails on a non-ASCII path on Windows and on the '\' separators on POSIX) and
    // registers it in the index as an external file.
    return TryCreateDiskFileReader(file.physical.c_str());
}

void CloseModFile(const ModFile& file, IReader*& reader)
{
    if (file.physical.empty())
        FS.r_close(reader);
    else
        xr_delete(reader); // not registered by r_open (-file_activity): FS.r_close would look for it
}

pcstr AtomKindName(AtomKind kind)
{
    switch (kind)
    {
    case AtomKind::Ltx: return "ltx";
    case AtomKind::Xml: return "xml";
    case AtomKind::String: return "string";
    case AtomKind::Texture: return "texture";
    }
    return "?";
}

bool RecordAtom(AtomKind kind, pcstr file, pcstr atom, pcstr owner, xr_string* previous_owner)
{
    ContentPatchState& S = ContentPatches();
    std::lock_guard<std::mutex> guard(S.lock);
    ContentPatchCheckLayers(S);
    ContentPatchState::AtomKey key{ kind, file ? file : "", atom ? atom : "" };
    ContentPatchAtom& entry = S.atoms[key];
    const pcstr who = owner && owner[0] ? owner : kBaseOwner;
    if (std::find(entry.owners.begin(), entry.owners.end(), who) != entry.owners.end())
        return false;
    const xr_string* previous = nullptr;
    if (0 != xr_strcmp(who, kBaseOwner))
    {
        for (auto it = entry.owners.rbegin(); it != entry.owners.rend(); ++it)
        {
            if (*it != kBaseOwner)
            {
                previous = &*it;
                break;
            }
        }
    }
    if (previous && previous_owner)
        *previous_owner = *previous;
    const bool conflict = previous != nullptr;
    entry.owners.emplace_back(who);
    return conflict;
}

bool FirstTime(pcstr key)
{
    ContentPatchState& S = ContentPatches();
    std::lock_guard<std::mutex> guard(S.lock);
    ContentPatchCheckLayers(S);
    return S.logged.emplace(key ? key : "").second;
}

void CollectConflicts(xr_vector<Conflict>& out)
{
    out.clear();
    ContentPatchState& S = ContentPatches();
    std::lock_guard<std::mutex> guard(S.lock);
    for (const auto& [key, entry] : S.atoms)
    {
        u32 addons = 0;
        for (const xr_string& owner : entry.owners)
        {
            if (owner != kBaseOwner)
                ++addons;
        }
        if (addons < 2)
            continue;
        Conflict& conflict = out.emplace_back();
        conflict.kind = key.kind;
        conflict.file = key.file;
        conflict.atom = key.atom;
        conflict.owners = entry.owners;
    }
}

void PrintAtoms(bool all)
{
    ContentPatchState& S = ContentPatches();
    std::lock_guard<std::mutex> guard(S.lock);
    u32 conflicts = 0;
    for (const auto& [key, entry] : S.atoms)
    {
        u32 addons = 0;
        for (const xr_string& owner : entry.owners)
        {
            if (owner != kBaseOwner)
                ++addons;
        }
        const bool conflict = addons >= 2;
        conflicts += conflict ? 1 : 0;
        if (!conflict && !all)
            continue;
        xr_string owners;
        for (const xr_string& owner : entry.owners)
        {
            if (!owners.empty())
                owners += ", ";
            owners += owner;
        }
        Msg("%s %-7s %s %s | %s | winner: %s", conflict ? "~ conflict" : "- patch   ", AtomKindName(key.kind),
            key.file.c_str(), key.atom.c_str(), owners.c_str(), entry.owners.empty() ? "?" : entry.owners.back().c_str());
    }
    Msg("- dltx_conflicts: %u atoms patched, %u conflicts%s", u32(S.atoms.size()), conflicts,
        all ? "" : " (dltx_conflicts all - every patched atom)");
}
} // namespace xray::content_patches
