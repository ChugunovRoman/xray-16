#pragma once

#pragma warning(push)
#pragma warning(disable : 4995)
#if defined(XR_PLATFORM_WINDOWS)
#include <io.h>
#endif
#pragma warning(pop)
#include "Common/Util.hpp"
#include "LocatorAPI_defs.h"
//#include "xrCore/Threading/Lock.hpp"
#include "xrCommon/xr_map.h"
#include "xrCommon/xr_smart_pointers.h"
#include "xrCommon/predicates.h"
#include "Common/Noncopyable.hpp"

#if defined(XR_PLATFORM_WINDOWS)
struct xr_finddata_t
{
    unsigned attrib;
    __time64_t time_create;
    __time64_t time_access;
    __time64_t time_write;
    _fsize_t size;
    char name[1024];
};
#elif defined(XR_PLATFORM_POSIX)
#include <stdint.h>
#define _A_HIDDEN      0x02
#define _A_SUBDIR 0x00000010

#if defined(XR_ARCHITECTURE_X64) || defined(XR_ARCHITECTURE_E2K) || defined(XR_ARCHITECTURE_PPC64)
#define _finddata_t _finddata64i32_t
#elif defined(XR_ARCHITECTURE_X86) || defined(XR_ARCHITECTURE_ARM) || defined(XR_ARCHITECTURE_ARM64) || defined(XR_ARCHITECTURE_PPC)
#define _finddata_t _finddata32_t
#endif

typedef int64_t __int64;
typedef __int64 __time64_t;
typedef long __time32_t;
typedef unsigned long _fsize_t;

struct _finddata64i32_t {
    unsigned attrib;
    __time64_t time_create;
    __time64_t time_access;
    __time64_t time_write;
    _fsize_t size;
    char name[FILENAME_MAX];
};

struct _finddata32_t
{
    unsigned attrib;
    __time32_t time_create;
    __time32_t time_access;
    __time32_t time_write;
    _fsize_t size;
    char name[FILENAME_MAX];
};

typedef _finddata_t xr_finddata_t;
#endif

class CStreamReader;
class Lock;

enum class FSType
{
    Virtual = 1,
    External = 2,
    Any = Virtual | External,
};

IMPLEMENT_ENUM_FLAG_OPERATORS(FSType, int);

class FileStatus
{
public:
    bool Exists;
    bool External; // File can be accessed only as external

    FileStatus(bool exists, bool external)
        : Exists(exists), External(external) {}

    operator bool() const { return Exists; }
};

class XRCORE_API CLocatorAPI : Noncopyable
{
    friend class FS_Path;

public:
    // IMPORTNT: don't replace u32 with size_t for this struct
    // (Letter A in the first word is forgotten intentionally,
    //  size_t will blow up the engine compatibility with it's resources)
    struct file
    {
        pcstr name; // low-case name
        size_t vfs; // 0xffffffff - standart file
        u32 crc; // contents CRC
        u32 ptr; // pointer inside vfs
        u32 size_real; //
        u32 size_compressed; // if (size_real==size_compressed) - uncompressed
        u32 modif; // for editor
        // plans/addons_api_support/10-location-packs.md: 0 - the base (fsgame.ltx, archives), N - the layer N
        // (mount_layer): a loose file lying in the folder of that layer under another name (physical_name)
        u16 layer = 0;
    };

    struct archive
    {
        size_t size = 0;
        size_t vfs_idx = size_t(-1);
        shared_str path;
        u32 modif = 0;
#if defined(XR_PLATFORM_WINDOWS)
        void *hSrcFile = nullptr;
        void *hSrcMap = nullptr;
#elif defined(XR_PLATFORM_POSIX)
        int hSrcFile = 0;
#else
#   error Select or add implementation for your platform
#endif
        CInifile* header = nullptr;

        archive() = default;
        void open();
        void close();
    };

    // IMPORTNT: don't replace u32 with size_t for this struct
    // (Letter A in the first word is forgotten intentionally,
    //  size_t will blow up the engine compatibility with it's resources)
    struct XRCORE_API archive_file_header
    {
        u16  size; // size of following members:
        u32  size_real;
        u32  size_compr;
        u32  crc;
        //char name[]; // there's a string with variable size between crc and ptr
        string_path name; // but we use fixed-size string for simplicity
        u32  ptr;

        // Used in file name string size calculation
        static constexpr auto ELEMENTS_SIZE = sizeof(size_real) + sizeof(size_compr) + sizeof(crc) + sizeof(ptr);

        archive_file_header(IReader& reader);
        archive_file_header(IWriter& writer, pcstr file_name, u32 real_size, u32 compressed_size, u32 crc_sum, u32 pointer);
    };

    using archives_vec = xr_vector<archive>;
    archives_vec m_archives;
    void LoadArchive(archive& A, pcstr entrypoint = nullptr);

private:
    struct file_pred
    {
        bool operator()(const file& x, const file& y) const { return xr_strcmp(x.name, y.name) < 0; }
    };

    using PathMap = xr_map<pcstr, FS_Path*, pred_str>;
    PathMap m_paths;

    using files_set = xr_set<file, file_pred>;
    using files_it = files_set::iterator;

    using FFVec = xr_vector<xr_finddata_t>;
    FFVec rec_files;

    int m_iLockRescan;
    void check_pathes();

    files_set m_files;
    bool bNoRecurse{ true };

    // plans/addons_api_support/10-location-packs.md: a folder whose files take the names of another folder of the
    // index (the game data). Its entries cover the same names of the base and of the layers below; unmount_layer puts
    // the covered entries back. The name strings in both containers are the ones of the live entries of m_files.
    struct fs_layer
    {
        xr_string id;
        string_path physical_root; // the folder on the disk: '\' separators, the trailing delimiter
        string_path virtual_root; // the names it takes in the index: as the index spells them
        size_t virtual_root_length{};
        xr_map<pcstr, file, pred_str> covered; // name -> the entry under the one of the layer
        xr_set<pcstr, pred_str> added; // names nothing had before the layer (files and folders)
        u32 files{};
        bool writable{}; // w_open of a name a layer serves writes into this one (writable_layer_name)

        // plans/addons_api_support/14-addon-resource-scoping.md: the addon the layer belongs to (empty - none) and its
        // alias roots - folders of the layer mounted once more under other names ($game_textures$@<owner>\, ...),
        // private files included. Their entries are the entries of this layer: covered / added hold them too, the
        // layer leaves with all of them at once (unmount_layer).
        struct alias_root
        {
            string_path physical_root; // as physical_root above
            string_path virtual_root; // as virtual_root above
            size_t virtual_root_length{};
            u32 files{};
        };
        xr_string owner;
        xr_vector<alias_root> aliases;
        xr_vector<xr_string> private_masks; // PatternMatch masks of names relative to physical_root, lower case, '\'
        xr_vector<xr_string> private_names; // the files the masks kept out of virtual_root: relative, lower case
    };
    xr_vector<fs_layer*> m_layers; // a stack: m_layers[i] is the layer i + 1

    // files - the counter of the root walked; private_prefix - the length of the physical root the private masks are
    // relative to (0 - no filter: an alias root, a layer without masks)
    bool layer_walk(u16 index, pcstr physical_dir, pcstr virtual_dir, u32& files, size_t private_prefix);
    bool layer_private(const fs_layer& L, pcstr relative_name) const;
    u32 layer_covered_files(const fs_layer& L) const;
    void register_under_layers(const file& top, const file& desc);
    bool writable_layer_name(pcstr virtual_name, string_path& physical) const;
    u16 writable_layer_of(pcstr physical_name, string_path& virtual_name) const;
    void layer_file_delete(files_it I); // file_delete of a file of a writable layer

    Lock* m_auth_lock;
    u64 m_auth_code{};
    // 40-parallel-prefetch.md (plans/optimization_mt): m_files is searched from several threads (the model prefetch
    // loads models on several threads, textures load in parallel) while a shader compiled for the first time writes its
    // cache file and registers it (w_close -> Register: an insert into the tree). Each search, iteration, insert and
    // erase of m_files runs under it. Recursive (Register -> exist -> file_find_it).
    Lock* m_files_lock;

    const file* RegisterExternal(pcstr name);
    const file* Register(
        pcstr name, size_t vfs, u32 crc, u32 ptr, u32 size_real, u32 size_compressed, u32 modif, u16 layer = 0);
    void ProcessArchive(pcstr path);
    void ProcessOne(pcstr path, const xr_finddata_t& entry);
    bool Recurse(pcstr path);

    files_it file_find_it(pcstr n);

    // 50z1 (plans/optimization_mt/50z1-startup-markers-fs-index.md): the startup index skips the directories already
    // enumerated through another alias of fsgame.ltx, and leaves the appdata caches out (indexed on demand). Defined
    // in LocatorAPI.cpp.
    struct LazyRoot;
    struct IndexState;
    IndexState* m_index_state;

    LazyRoot* find_lazy_root(pcstr normalized_name) const;
    bool lazy_register(pcstr name);
    void lazy_scan(pcstr path, bool root_only);
    files_it file_find_it_lazy(pcstr n);

public:
    enum : u32
    {
        flNeedRescan = (1 << 0),
        flBuildCopy = (1 << 1),
        flReady = (1 << 2),
        flEBuildCopy = (1 << 3),
        flEventNotificator = (1 << 4),
        flTargetFolderOnly = (1 << 5),
        flCacheFiles = (1 << 6),
        flScanAppRoot = (1 << 7),
        flNeedCheck = (1 << 8),
        flDumpFileActivity = (1 << 9),
    };
    Flags32 m_Flags;
    u32 dwAllocGranularity;
    u32 dwOpenCounter;

private:
    void check_cached_files(pstr fname, const size_t& fname_size, const file& desc, pcstr& source_name);

    void file_from_cache_impl(IReader*& R, pstr fname, const file& desc);
    void file_from_cache_impl(CStreamReader*& R, pstr fname, const file& desc);
    template <typename T>
    void file_from_cache(T*& R, pstr fname, const size_t& fname_size, const file& desc, pcstr& source_name);

    void file_from_archive(IReader*& R, pcstr fname, const file& desc);
    void file_from_archive(CStreamReader*& R, pcstr fname, const file& desc);

    void copy_file_to_build(IWriter* W, IReader* r);
    void copy_file_to_build(IWriter* W, CStreamReader* r);
    template <typename T>
    void copy_file_to_build(T*& R, pcstr source_name);

    bool check_for_file(pcstr path, pcstr _fname, string_path& fname, const file*& desc);

    template <typename T>
    T* r_open_impl(pcstr path, pcstr _fname);

    void setup_fs_path(pcstr fs_name, string_path& fs_path);
    void setup_fs_path(pcstr fs_name);
    IReader* setup_fs_ltx(pcstr fs_name);

public:
    CLocatorAPI();
    ~CLocatorAPI();
    void _initialize(u32 flags, pcstr target_folder = nullptr, pcstr fs_name = nullptr);
    void _destroy();

    CStreamReader* rs_open(pcstr initial, pcstr N);
    IReader* r_open(pcstr initial, pcstr N);
    IReader* r_open(pcstr N) { return r_open(nullptr, N); }
    // plans/addons_api_support/15-merge-implementation.md, 2.1 (the ancestor of a merge).
    // The entry of the name in the layers <= max_layer (0 - the base: a loose file of gamedata\ or of an archive),
    // walking the covered chain down from the entry that serves the name now. nullptr - no such entry.
    // The name is resolved as by r_open; r_close closes the reader.
    IReader* r_open_under(pcstr path, pcstr name, u16 max_layer = 0);
    IReader* r_open_base(pcstr path, pcstr name) { return r_open_under(path, name, 0); }
    void r_close(IReader*& S);
    void r_close(CStreamReader*& fs);

    IWriter* w_open(pcstr initial, pcstr N);
    IWriter* w_open(pcstr N) { return w_open(nullptr, N); }
    IWriter* w_open_ex(pcstr initial, pcstr N);
    IWriter* w_open_ex(pcstr N) { return w_open_ex(nullptr, N); }
    void w_close(IWriter*& S);
    // For registered files only
    const file* GetFileDesc(pcstr path);

    FileStatus exist(pcstr N, FSType fsType = FSType::Virtual);
    FileStatus exist(pcstr path, pcstr name, FSType fsType = FSType::Virtual);
    FileStatus exist(string_path& fn, pcstr path, pcstr name, FSType fsType = FSType::Virtual);
    FileStatus exist(string_path& fn, pcstr path, pcstr name, pcstr ext, FSType fsType = FSType::Virtual);

    bool can_write_to_folder(pcstr path);
    bool can_write_to_alias(pcstr path);
    bool can_modify_file(pcstr fname);
    bool can_modify_file(pcstr path, pcstr name);

    bool dir_delete(pcstr path, pcstr nm, bool remove_files);
    bool dir_delete(pcstr full_path, bool remove_files) { return dir_delete(nullptr, full_path, remove_files); }
    void file_delete(pcstr path, pcstr nm);
    void file_delete(pcstr full_path) { file_delete(nullptr, full_path); }
    void file_copy(pcstr src, pcstr dest);
    void file_rename(pcstr src, pcstr dest, bool overwrite = true);
    int file_length(pcstr src);

    u32 get_file_age(pcstr nm);
    void set_file_age(pcstr nm, u32 age);

    xr_vector<pstr>* file_list_open(pcstr initial, pcstr folder, u32 flags = FS_ListFiles);
    xr_vector<pstr>* file_list_open(pcstr path, u32 flags = FS_ListFiles);
    void file_list_close(xr_vector<pstr>*& lst);

    bool path_exist(pcstr path);
    FS_Path* get_path(pcstr path);
    bool get_path(pcstr path, FS_Path** outPath);
    FS_Path* append_path(pcstr path_alias, pcstr root, pcstr add, bool recursive);
    pcstr update_path(string_path& dest, pcstr initial, pcstr src, bool crashOnNotFound = true);
    // A name under $game_data$ with "." / ".." components (an #include "..\x.ltx", a path of a script) as the index
    // spells it: the components collapsed, lower case, '\'. A name not under $game_data$ or without them stays.
    // False (logged once): ".." leaves $game_data$ - the name must not be opened (the files of the addons lie
    // outside it, a private one only under its alias root: 14-addon-resource-scoping.md)
    bool collapse_game_data_dots(string_path& name);

    size_t file_list(FS_FileSet& dest, pcstr path, u32 flags = FS_ListFiles, pcstr mask = nullptr);

    void unload_archive(archive& A);

    // Layers (plans/addons_api_support/10-location-packs.md). The files of physical_root take the names of
    // virtual_root (nullptr - $game_data$) and cover the same names of the base and of the layers mounted before.
    // Returns the layer index (1..), 0 on a failure. Mounting and unmounting must not race with reads of the
    // index on other threads (a reader keeps a pointer to an entry): only while no background loading runs.
    // writable: w_open / w_open_ex of a name some layer serves write into the top writable layer (its folder is
    // created by the mount); a name only the base has is written where it lies, as without layers.
    // plans/addons_api_support/14-addon-resource-scoping.md: what an addon layer adds to mount_layer
    struct layer_options
    {
        // The addon the layer belongs to (layer_owner): a-z 0-9 _ (no '@', no separators). A folder of the layer
        // named "@..." is skipped then: those names are the alias roots of the addons.
        pcstr owner = nullptr;
        // PatternMatch masks ('*' takes '\' too) of the names relative to physical_root ("textures\wpn\x.dds",
        // "textures\my\*"): such files are not mounted at virtual_root, only under the alias roots (private)
        const xr_vector<xr_string>* private_masks = nullptr;
    };
    u16 mount_layer(pcstr id, pcstr physical_root, pcstr virtual_root = nullptr, bool writable = false,
        const layer_options* options = nullptr);
    // One more root of the top layer `id` (14-addon-resource-scoping.md): the files of physical_root (a folder of the
    // layer as a rule: <addon>\gamedata\textures\) take the names of virtual_root too ($game_textures$@<owner>\), the
    // private ones included. No folder physical_root - nothing to mount, true. Not for a writable layer.
    bool mount_layer_alias(pcstr id, pcstr physical_root, pcstr virtual_root);
    // Only the top layer (the last mounted): the covered entries come back, the names it added leave
    bool unmount_layer(pcstr id);
    // An entry of a folder of a layer that layer_walk leaves out with its subtree, by the entry itself: a name of
    // ignore_name (Thumbs.db, .svn, .vs, *.pdb ...), a hidden entry on Windows (_A_HIDDEN), a name starting with '.'
    // on POSIX (the '*' of glob does not match it). `physical` - the path of the entry on the disk (UTF-8). A walk of
    // the folder of a layer outside the index (the merge of the addons, addon_merge.cpp) takes the files it mounts
    static bool layer_walk_skips(pcstr physical);
    u16 find_layer(pcstr id) const; // 0 - not mounted
    u16 layers_count() const { return u16(m_layers.size()); }
    pcstr layer_id(u16 index) const;
    pcstr layer_owner(u16 index) const; // the addon of the layer (layer_options::owner), nullptr - none / the base
    u16 find_layer_of_owner(pcstr owner) const; // the top layer of the addon, 0 - none
    // The file on the disk of an entry of a layer; false for the base entries (their name is the file)
    bool physical_name(const file& desc, string_path& dest) const;
    struct layer_stats
    {
        u32 files{}; // at virtual_root (public)
        u32 private_files{}; // kept out of virtual_root by the masks
        u32 alias_files{}; // under the alias roots
        u32 covered_files{}; // the files of other layers / the base the public ones cover (replace) now
    };
    bool get_layer_stats(u16 index, layer_stats& out) const;
    // plans/addons_api_support/13-extension-model.md, 4.2: the files the public root of the layer covers now (not the
    // folders, not the names of its alias roots) - the conflicts of the addons (console addon_conflicts)
    struct layer_covered_entry
    {
        xr_string name; // the index name: the full path, lower case
        u16 under{}; // the layer of the entry it covers (0 - the base)
        u16 serving{}; // the layer that serves the name now: this one or a layer above it
    };
    void get_layer_covered(u16 index, xr_vector<layer_covered_entry>& out) const;
    // plans/addons_api_support (decision 63): the files of the base (layer 0: the loose files of the folders of
    // fsgame.ltx and the files of the archives - what r_open_base finds) under the folder of the path alias `path`,
    // the ones the layers cover now included: the names relative to that folder as the index spells them (lower case
    // on Windows, the separator of the index), not the folders. Sorted, each once; no such alias - empty
    void get_base_files(pcstr path, xr_vector<xr_string>& out);
    // The roots and the stats of the layer (console addon_files); list: the replaced and the private names, all:
    // the public ones too
    void print_layer(u16 index, bool list, bool all) const;

    void auth_generate(xr_vector<shared_str>& ignore, xr_vector<shared_str>& important);
    u64 auth_get();
    void auth_runtime(void*);

    void rescan_path(pcstr full_path, bool bRecurse);
    // editor functions
    void rescan_pathes();
    void lock_rescan();
    void unlock_rescan();
};

extern XRCORE_API xr_unique_ptr<CLocatorAPI> xr_FS;
#define FS (*xr_FS)
