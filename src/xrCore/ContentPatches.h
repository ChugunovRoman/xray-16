#pragma once

// Patches of the game configs by the content of the base and of the addons (plans/addons_api_support/
// 09-content-addons-vfs.md section 5, 13-extension-model.md section 3 and 4.2):
//   - DLTX: mod_<root>_*.ltx next to a root ltx (xr_ini.cpp);
//   - XMLOverride: mod_<name>_*.xml next to an xml loaded by name (XML/XMLDocument.cpp);
//   - the string table and ui\textures_descr\ in the order of the file system layers.
//
// A patch file lies in the folder of its target, under $game_config$ only. It is applied in the order of the layers
// of the file system (the base - layer 0 - first, then the layers as they are mounted: a higher layer is applied
// later and wins), then by the file name. Its owner is the addon of its layer (CLocatorAPI::layer_owner), the layer
// id for a layer without an addon, "base" for the base.
// Two addons with a patch of one name (both lay configs\mod_system_fix.ltx): the file of the upper layer covers the
// file of the lower one in the index, the patches are taken from every layer that has the name (the covered entries
// of the layers) - each one is applied, in the order of the layers. A patch of the base covered by a patch of the same
// name is replaced (not applied), as any file of the base; so is a patch of an addon covered by a layer of the same
// addon (its slot set over its common content).
//
// The atoms (13 section 3): what one patch changes as a whole - a key of an ltx section, a node of an xml, an id of
// the string table, an id of a ui texture. Two owners other than "base" changing one atom - a conflict (an implicit
// slot, 13 section 4.2): the last applied wins, the console command dltx_conflicts lists them.

#include "xrCommon/xr_string.h"
#include "xrCommon/xr_vector.h"

class IReader;

namespace xray::content_patches
{
constexpr pcstr kBaseOwner = "base";

struct ModFile
{
    xr_string name; // the full name as the file system index has it (lower case)
    xr_string owner; // see above
    u16 layer{};
    // A patch of a lower layer covered by a file of the same name above it: the file on the disk, opened by it (the
    // index gives the upper one by the name). Empty - opened by the name.
    xr_string physical;
};

// The file of a patch (FS.r_open of its name, or a read of its file on the disk); null - it cannot be opened.
// Closed by CloseModFile (a file read from the disk is not an FS.r_open reader).
XRCORE_API IReader* OpenModFile(const ModFile& file);
XRCORE_API void CloseModFile(const ModFile& file, IReader*& reader);

// The patch files of the file full_name (a name of the index: $game_config$\...\<stem>.<ext>, any case) in the order
// of application: <folder>\mod_<stem>_*.<ext> (one name may come several times: from several layers, see above).
// A patch file is meant for the longest <stem> it can name:
// mod_inventory_new_16_x.xml patches inventory_new_16.xml, not inventory_new.xml, when both lie in the folder.
// Nothing for a name outside $game_config$ or a patch file itself. The folder listing is cached (one listing per
// folder and extension until the layers change), the call is cheap.
XRCORE_API bool FindModFiles(pcstr full_name, xr_vector<ModFile>& out);

// Some mod_*.<ext> lies anywhere under $game_config$ (one walk of the configs per set of layers, cached). False -
// no file can have a patch, a loader may take its plain path.
XRCORE_API bool AnyModFiles(pcstr ext);

// The file is a patch of another file of its folder (mod_<stem>_*.<ext> with <stem>.<ext> there): a loader of a whole
// folder (the string table, ui\textures_descr\) skips it - the patch comes with its target
XRCORE_API bool IsPatchFile(pcstr full_name);

// The owner of a file of the index (see above); full_name - as for FindModFiles
XRCORE_API void FileOwner(pcstr full_name, xr_string& out);
XRCORE_API u16 FileLayer(pcstr full_name);

// The name without the $game_config$ folder ("system.ltx", "ui\ui_mm_main.xml"), for the log
XRCORE_API pcstr ShortName(pcstr full_name);

// Drop the cached listings, the atoms and the warnings logged once. The layers are compared on each call anyway
// (a mount or an unmount drops the cache by itself), this is for a change of the files the index does not see.
XRCORE_API void Invalidate();

enum class AtomKind : u8
{
    Ltx,
    Xml,
    String,
    Texture,
};
XRCORE_API pcstr AtomKindName(AtomKind kind);

// A contribution of `owner` to the atom `atom` of `file` (a short name). Repeated contributions are ignored (an xml
// of the ui is loaded many times). True - the first contribution of this owner to an atom another owner (not "base")
// has already changed: a new conflict, the caller logs it once. previous_owner - that owner.
XRCORE_API bool RecordAtom(AtomKind kind, pcstr file, pcstr atom, pcstr owner, xr_string* previous_owner = nullptr);

// True the first time a key is passed (until Invalidate): a warning of a patch is logged once though the file is
// loaded many times
XRCORE_API bool FirstTime(pcstr key);

struct Conflict
{
    AtomKind kind{};
    xr_string file;
    xr_string atom;
    xr_vector<xr_string> owners; // in the order of application: the last one wins
};
XRCORE_API void CollectConflicts(xr_vector<Conflict>& out);

// Console dltx_conflicts [all]: the conflicts; all - every patched atom with its owners
XRCORE_API void PrintAtoms(bool all);
} // namespace xray::content_patches
