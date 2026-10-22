#include "stdafx.h"

#include "TextureDescrManager.h"
#include "ETextureParams.h"

#include "xrCore/Threading/ParallelForEach.hpp"

#include <cstring>
#include <string_view>
#if !defined(XR_PLATFORM_WINDOWS)
#include <unistd.h>
#endif

namespace xray::render::RENDER_NAMESPACE
{
// eye-params
float r__dtex_range = 50;
class cl_dt_scaler : public R_constant_setup
{
public:
    float scale;

    cl_dt_scaler(float s) : scale(s) {}
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.set_c(C, scale, scale, scale, 1 / r__dtex_range); }
};

void fix_texture_thm_name(pstr fn)
{
    pstr _ext = strext(fn);
    if (_ext && (!xr_stricmp(_ext, ".tga") || !xr_stricmp(_ext, ".thm") || !xr_stricmp(_ext, ".dds") ||
       !xr_stricmp(_ext, ".bmp") || !xr_stricmp(_ext, ".ogm")))
    {
        *_ext = 0;
    }
}

void CTextureDescrMngr::LoadLTX(pcstr initial, bool listTHM)
{
    ZoneScoped;

    string_path fname;
    FS.update_path(fname, initial, "textures.ltx");

    if (!FS.exist(fname))
        return;

#ifndef MASTER_GOLD
    Msg("Processing textures.ltx in [%s]", initial);
#endif

    CInifile ini(fname);

    Lock lock;
    if (ini.section_exist("association"))
    {
        CInifile::Sect& data = ini.r_section("association");
#ifndef MASTER_GOLD
        Msg("\tsection [%s] has %d lines", data.Name.c_str(), data.Data.size());
#endif
        m_texture_details.reserve(m_texture_details.size() + data.Data.size());
        m_detail_scalers.reserve(m_detail_scalers.size() + data.Data.size());

        const auto processAssociation = [&](const CInifile::Item& item)
        {
            ZoneScopedN("Process association");
            if (listTHM)
                Msg("\t\t%s = %s", item.first.c_str(), item.second.c_str());

            lock.Enter();
            texture_desc& desc = m_texture_details[item.first];
            cl_dt_scaler*& dts = m_detail_scalers[item.first];
            lock.Leave();

            if (desc.m_assoc)
                xr_delete(desc.m_assoc);

            desc.m_assoc = xr_new<texture_assoc>();

            string_path T;
            float s;

            const int res = sscanf(item.second.c_str(), "%[^,],%f", T, &s);
            R_ASSERT4(res == 2, "Bad texture association", item.first.c_str(), fname);
            desc.m_assoc->detail_name = T;
            if (dts)
                dts->scale = s;
            else
                dts = xr_new<cl_dt_scaler>(s);

            if (strstr(item.second.c_str(), "usage[diffuse_or_bump]"))
                desc.m_assoc->usage.set(texture_assoc::flDiffuseDetail | texture_assoc::flBumpDetail);
            else if (strstr(item.second.c_str(), "usage[bump]"))
                desc.m_assoc->usage.set(texture_assoc::flBumpDetail);
            else if (strstr(item.second.c_str(), "usage[diffuse]"))
                desc.m_assoc->usage.set(texture_assoc::flDiffuseDetail);
        };
        xr_parallel_for_each(data.Data, processAssociation);
    } // "association"

    if (ini.section_exist("specification"))
    {
        CInifile::Sect& data = ini.r_section("specification");
#ifndef MASTER_GOLD
        Msg("\tsection [%s] has %d lines", data.Name.c_str(), data.Data.size());
#endif
        m_texture_details.reserve(m_texture_details.size() + data.Data.size());

        const auto processSpecification = [&](const CInifile::Item& item)
        {
            if (listTHM)
                Msg("\t\t%s = %s", item.first.c_str(), item.second.c_str());

            lock.Enter();
            texture_desc& desc = m_texture_details[item.first];
            lock.Leave();

            if (desc.m_spec)
                xr_delete(desc.m_spec);

            desc.m_spec = xr_new<texture_spec>();

            string_path bmode;
            const int res =
                    sscanf(item.second.c_str(), "bump_mode[%[^]]], material[%f]", bmode, &desc.m_spec->m_material);
            R_ASSERT4(res == 2, "Bad texture specification", item.first.c_str(), fname);
            if ((bmode[0] == 'u') && (bmode[1] == 's') && (bmode[2] == 'e') && (bmode[3] == ':'))
            {
                // bump-map specified
                desc.m_spec->m_bump_name = bmode + 4;
            }
        };
        xr_parallel_for_each(data.Data, processSpecification);
    } // "specification"
}

// 50z2 (plans/optimization_mt/50z2-startup-thm-shaders-probes-preview.md): the cache of the parsed .thm files.
// LoadTHM opened every .thm on every start (9779 files of ~156 bytes in GlobalWar): on an HDD that is a seek per file.
// One file in $app_data_root$\cache keeps what LoadTHM takes from each .thm (texture_thm_record) with the size, time
// and flags the FS index gives for the file - the validation needs no extra opens. On start only the missing or changed
// .thm are read and the removed ones are dropped; the cache is rewritten (a temp file + rename, so a reader never sees a
// partial one) when anything changed. A broken cache (magic, format, build, root, CRC, bounds) is dropped as a whole:
// every .thm is read as before. -thm_verify reads every .thm anyway and logs the records that differ from the cache.
// -list_thm loads file by file without the cache, as before.
struct texture_thm_record
{
    STextureParams::ETType type{};
    Flags32 flags{};
    float detail_scale{};
    STextureParams::ETMaterial material{};
    float material_weight{};
    STextureParams::ETBumpMode bump_mode{};
    shared_str detail_name;
    shared_str bump_name;
};

namespace
{
constexpr u32 kThmCacheMagic = 0x4D485447; // "GTHM"
constexpr u32 kThmCacheFormat = 1; // bump it on any change of the layout or of what a record holds
constexpr u16 kThmNullString = 0xFFFF; // the length of a null shared_str

void thm_parse(pcstr initial, const FS_File& it, texture_thm_record& rec)
{
    ZoneScopedN("Process file");
    string_path fn;
    FS.update_path(fn, initial, it.name.c_str());
    IReader* F = FS.r_open(fn);
    R_ASSERT3(F, "Failed to open THM (case-sensitivity problem?)", it.name.c_str());

    xr_strcpy(fn, it.name.c_str());
    fix_texture_thm_name(fn);

    R_ASSERT3(F->find_chunk(THM_CHUNK_TYPE), "Cannot find THM chunk in file", fn);
    F->r_u32();
    STextureParams tp;
    tp.Load(*F);
    FS.r_close(F);

    rec.type = tp.type;
    rec.flags = tp.flags;
    rec.detail_scale = tp.detail_scale;
    rec.material = tp.material;
    rec.material_weight = tp.material_weight;
    rec.bump_mode = tp.bump_mode;
    rec.detail_name = tp.detail_name;
    rec.bump_name = tp.bump_name;
}

bool thm_same_float(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

bool thm_same(const texture_thm_record& a, const texture_thm_record& b)
{
    return a.type == b.type && a.flags.get() == b.flags.get() && thm_same_float(a.detail_scale, b.detail_scale) &&
        a.material == b.material && thm_same_float(a.material_weight, b.material_weight) &&
        a.bump_mode == b.bump_mode && a.detail_name == b.detail_name && a.bump_name == b.bump_name;
}

// The cache file is read and written with the C runtime of this module, not through FS: FS would register the files in
// its index, and its rename is not atomic. The paths are UTF-8 (FS), converted here (no xrCore std::wstring crosses
// the module boundary).
#if defined(XR_PLATFORM_WINDOWS)
bool thm_wide(pcstr path, wchar_t (&out)[2 * sizeof(string_path)])
{
    return MultiByteToWideChar(CP_UTF8, 0, path, -1, out, int(std::size(out))) > 0;
}

FILE* thm_fopen(pcstr path, bool write)
{
    wchar_t w[2 * sizeof(string_path)];
    return thm_wide(path, w) ? _wfopen(w, write ? L"wb" : L"rb") : nullptr;
}

bool thm_replace(pcstr from, pcstr to)
{
    wchar_t wf[2 * sizeof(string_path)], wt[2 * sizeof(string_path)];
    return thm_wide(from, wf) && thm_wide(to, wt) && MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING) != 0;
}

void thm_remove(pcstr path)
{
    wchar_t w[2 * sizeof(string_path)];
    if (thm_wide(path, w))
        _wunlink(w);
}

u32 thm_process_id() { return u32(GetCurrentProcessId()); }
#else
void thm_native(pcstr path, string_path& out)
{
    xr_strcpy(out, path);
    for (char* c = out; *c; ++c)
    {
        if (*c == '\\')
            *c = '/';
    }
}

FILE* thm_fopen(pcstr path, bool write)
{
    string_path n;
    thm_native(path, n);
    return fopen(n, write ? "wb" : "rb");
}

bool thm_replace(pcstr from, pcstr to)
{
    string_path f, t;
    thm_native(from, f);
    thm_native(to, t);
    return rename(f, t) == 0; // atomic replace
}

void thm_remove(pcstr path)
{
    string_path n;
    thm_native(path, n);
    unlink(n);
}

u32 thm_process_id() { return u32(getpid()); }
#endif

bool thm_read_file(pcstr path, xr_vector<u8>& data)
{
    FILE* f = thm_fopen(path, false);
    if (!f)
        return false;
    bool ok = false;
    if (fseek(f, 0, SEEK_END) == 0)
    {
        const long len = ftell(f);
        if (len > 0 && fseek(f, 0, SEEK_SET) == 0)
        {
            data.resize(size_t(len));
            ok = fread(data.data(), 1, data.size(), f) == data.size();
        }
    }
    fclose(f);
    return ok;
}

class thm_cache_in
{
    const u8* m_pos;
    const u8* m_end;
    bool m_ok{ true };

public:
    thm_cache_in(const u8* data, size_t size) : m_pos(data), m_end(data + size) {}

    template <typename T>
    T get()
    {
        T v{};
        if (!m_ok || size_t(m_end - m_pos) < sizeof(T))
        {
            m_ok = false;
            return v;
        }
        std::memcpy(&v, m_pos, sizeof(T));
        m_pos += sizeof(T);
        return v;
    }

    std::string_view str()
    {
        const u16 n = get<u16>();
        if (!m_ok || size_t(m_end - m_pos) < n)
        {
            m_ok = false;
            return {};
        }
        const std::string_view s(reinterpret_cast<const char*>(m_pos), n);
        m_pos += n;
        return s;
    }

    // A shared_str as written by thm_cache_out::sstr: null, empty and non-empty stay apart (the .thm reader gives a
    // null one for an absent chunk and a docked "" for an empty string)
    void sstr(shared_str& out)
    {
        const u16 n = get<u16>();
        if (!m_ok || n == kThmNullString)
            return;
        if (size_t(m_end - m_pos) < n)
        {
            m_ok = false;
            return;
        }
        out = xr_string(reinterpret_cast<const char*>(m_pos), n).c_str();
        m_pos += n;
    }

    bool ok() const { return m_ok; }
    bool at_end() const { return m_pos == m_end; }
};

class thm_cache_out
{
    xr_vector<u8>& m_data;

public:
    explicit thm_cache_out(xr_vector<u8>& data) : m_data(data) {}

    template <typename T>
    void put(const T& v)
    {
        const size_t at = m_data.size();
        m_data.resize(at + sizeof(T));
        std::memcpy(m_data.data() + at, &v, sizeof(T));
    }

    void str(pcstr s)
    {
        const size_t n = s ? xr_strlen(s) : 0;
        R_ASSERT(n < kThmNullString);
        put(u16(n));
        if (n)
            m_data.insert(m_data.end(), reinterpret_cast<const u8*>(s), reinterpret_cast<const u8*>(s) + n);
    }

    void sstr(const shared_str& s)
    {
        if (s._get())
            str(s.c_str());
        else
            put(kThmNullString);
    }
};

struct thm_cache_entry
{
    s64 size{};
    s64 time{};
    u32 attrib{};
    texture_thm_record rec;
};

// The file of the alias: $app_data_root$\cache\textures_thm_<alias without $>.bin
bool thm_cache_path(pcstr initial, string_path& path)
{
    if (!FS.path_exist("$app_data_root$"))
        return false;
    string_path name = "cache" DELIMITER "textures_thm_";
    size_t len = xr_strlen(name);
    for (pcstr c = initial; *c && len + 5 < sizeof(name); ++c)
    {
        const char ch = char(tolower(u8(*c)));
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')
            name[len++] = ch;
    }
    name[len] = 0;
    xr_strcat(name, ".bin");
    FS.update_path(path, "$app_data_root$", name);
    return true;
}

// The cache of the root; false and the reason when there is none or it is broken (then it is not used at all)
bool thm_cache_read(pcstr path, pcstr root, xr_unordered_map<xr_string, thm_cache_entry>& out, pcstr& why)
{
    xr_vector<u8> data;
    if (!thm_read_file(path, data))
    {
        why = "no cache";
        return false;
    }
    if (data.size() < 5 * sizeof(u32))
    {
        why = "truncated";
        return false;
    }
    const size_t body = data.size() - sizeof(u32);
    u32 stored_crc;
    std::memcpy(&stored_crc, data.data() + body, sizeof(u32));
    if (crc32(data.data(), u32(body)) != stored_crc)
    {
        why = "CRC mismatch";
        return false;
    }
    thm_cache_in in(data.data(), body);
    if (in.get<u32>() != kThmCacheMagic || in.get<u32>() != kThmCacheFormat)
    {
        why = "other format";
        return false;
    }
    if (in.get<u32>() != Core.GetBuildId())
    {
        why = "other engine build";
        return false;
    }
    const std::string_view cached_root = in.str();
    if (!in.ok() || cached_root != std::string_view(root))
    {
        why = "other root";
        return false;
    }
    const u32 count = in.get<u32>();
    if (!in.ok() || count > body / 32) // a record takes 32+ bytes
    {
        why = "bad count";
        return false;
    }
    out.reserve(count);
    for (u32 i = 0; i < count && in.ok(); ++i)
    {
        const std::string_view name = in.str();
        thm_cache_entry e;
        e.size = in.get<s64>();
        e.time = in.get<s64>();
        e.attrib = in.get<u32>();
        e.rec.type = STextureParams::ETType(in.get<u32>());
        e.rec.flags.assign(in.get<u32>());
        e.rec.detail_scale = in.get<float>();
        e.rec.material = STextureParams::ETMaterial(in.get<u32>());
        e.rec.material_weight = in.get<float>();
        e.rec.bump_mode = STextureParams::ETBumpMode(in.get<u32>());
        in.sstr(e.rec.detail_name);
        in.sstr(e.rec.bump_name);
        if (!in.ok())
            break;
        out.emplace(xr_string(name.data(), name.size()), std::move(e));
    }
    if (!in.ok() || !in.at_end() || out.size() != count)
    {
        out.clear();
        why = "corrupt records";
        return false;
    }
    return true;
}

struct thm_item
{
    const FS_File* file;
    texture_thm_record rec;
    bool cached;
};

void thm_cache_write(pcstr path, pcstr root, const xr_vector<thm_item>& items)
{
    xr_vector<u8> data;
    data.reserve(items.size() * 96 + 256);
    thm_cache_out out(data);
    out.put(kThmCacheMagic);
    out.put(kThmCacheFormat);
    out.put(u32(Core.GetBuildId()));
    out.str(root);
    out.put(u32(items.size()));
    for (const thm_item& it : items)
    {
        out.str(it.file->name.c_str());
        out.put(s64(it.file->size));
        out.put(s64(it.file->time_write));
        out.put(u32(it.file->attrib));
        out.put(u32(it.rec.type));
        out.put(u32(it.rec.flags.get()));
        out.put(it.rec.detail_scale);
        out.put(u32(it.rec.material));
        out.put(it.rec.material_weight);
        out.put(u32(it.rec.bump_mode));
        out.sstr(it.rec.detail_name);
        out.sstr(it.rec.bump_name);
    }
    out.put(crc32(data.data(), u32(data.size())));

    // A temp file of this process, then an atomic replace: concurrent instances never read a partial file, the last
    // writer wins (both write the same data). When the replace fails (the target is open in another instance) the
    // temp file is removed and the next start writes the cache again.
    VerifyPath(path);
    string_path tmp;
    xr_sprintf(tmp, "%s.%u.tmp", path, thm_process_id());
    FILE* f = thm_fopen(tmp, true);
    if (!f)
    {
        Msg("! [thm-cache] can't write '%s'", tmp);
        return;
    }
    const bool written = fwrite(data.data(), 1, data.size(), f) == data.size();
    const bool closed = fclose(f) == 0;
    if (!written || !closed || !thm_replace(tmp, path))
    {
        thm_remove(tmp);
        Msg("! [thm-cache] can't write '%s'", path);
    }
}
} // namespace

void CTextureDescrMngr::ApplyTHM(const xr_string& file_name, const texture_thm_record& tp, Lock& lock)
{
    if (STextureParams::ttImage == tp.type || STextureParams::ttTerrain == tp.type ||
        STextureParams::ttNormalMap == tp.type)
    {
        string_path fn;
        xr_strcpy(fn, file_name.c_str());
        fix_texture_thm_name(fn);

        lock.Enter();
        texture_desc& desc = m_texture_details[fn];
        cl_dt_scaler*& dts = m_detail_scalers[fn];
        lock.Leave();

        if (tp.detail_name.size() &&
            tp.flags.is_any(STextureParams::flDiffuseDetail | STextureParams::flBumpDetail))
        {
            if (desc.m_assoc)
                xr_delete(desc.m_assoc);

            desc.m_assoc = xr_new<texture_assoc>();
            desc.m_assoc->detail_name = tp.detail_name;
            if (dts)
                dts->scale = tp.detail_scale;
            else
                dts = xr_new<cl_dt_scaler>(tp.detail_scale);

            if (tp.flags.is(STextureParams::flDiffuseDetail))
                desc.m_assoc->usage.set(texture_assoc::flDiffuseDetail);

            if (tp.flags.is(STextureParams::flBumpDetail))
                desc.m_assoc->usage.set(texture_assoc::flBumpDetail);
        }
        if (desc.m_spec)
            xr_delete(desc.m_spec);

        desc.m_spec = xr_new<texture_spec>();
        desc.m_spec->m_material = float(tp.material) + tp.material_weight;
        desc.m_spec->m_use_steep_parallax = false;

        if (tp.bump_mode == STextureParams::tbmUse)
        {
            desc.m_spec->m_bump_name = tp.bump_name;
        }
        else if (tp.bump_mode == STextureParams::tbmUseParallax)
        {
            desc.m_spec->m_bump_name = tp.bump_name;
            desc.m_spec->m_use_steep_parallax = true;
        }
    }
}

void CTextureDescrMngr::LoadTHM(LPCSTR initial, bool listTHM)
{
    ZoneScoped;

    FS_FileSet flist;
    FS.file_list(flist, initial, FS_ListFiles, "*.thm");

    if (flist.empty())
        return;

#ifndef MASTER_GOLD
    Msg("Processing %d .thm files in [%s]", flist.size(), initial);
#endif


    m_texture_details.reserve(m_texture_details.size() + flist.size());
    m_detail_scalers.reserve(m_detail_scalers.size() + flist.size());

    Lock lock;
    if (listTHM)
    {
        // Alundaio: Print list of *.thm to find bad .thms!
        // We need precise info in the log when listTHM is specified.
        // Load in single thread, file by file (and without the cache: every file is opened).
        for (const FS_File& file : flist)
        {
            Log("\t", file.name.c_str());
            texture_thm_record rec;
            thm_parse(initial, file, rec);
            ApplyTHM(file.name, rec, lock);
        }
        return;
    }

    // 50z2: the records of the unchanged files come from the cache (see texture_thm_record)
    string_path root, cache_path;
    FS.update_path(root, initial, "");
    const bool cache_on = thm_cache_path(initial, cache_path);
    xr_unordered_map<xr_string, thm_cache_entry> cache;
    pcstr why = "no $app_data_root$";
    const bool cache_valid = cache_on && thm_cache_read(cache_path, root, cache, why);

    xr_vector<thm_item> items;
    items.reserve(flist.size());
    u32 from_cache = 0;
    for (const FS_File& file : flist)
    {
        thm_item& it = items.emplace_back();
        it.file = &file;
        it.cached = false;
        const auto c = cache.find(file.name);
        if (c != cache.end() && c->second.size == s64(file.size) && c->second.time == s64(file.time_write) &&
            c->second.attrib == u32(file.attrib))
        {
            it.rec = c->second.rec;
            it.cached = true;
            ++from_cache;
        }
    }
    const u32 read = u32(items.size()) - from_cache;
    const u32 removed = u32(cache.size()) - from_cache; // cached names absent from the index (or changed ones)
    cache.clear();

    const auto processFile = [&](thm_item& it)
    {
        if (!it.cached)
            thm_parse(initial, *it.file, it.rec);
        ApplyTHM(it.file->name, it.rec, lock);
    };
    xr_parallel_for_each(items, processFile);

    const bool changed = !cache_valid || read || removed;
    if (cache_on && changed)
        thm_cache_write(cache_path, root, items);
    Msg("* [thm-cache] %s: %u .thm, %u from the cache, %u read%s%s%s", initial, u32(items.size()), from_cache, read,
        cache_valid ? "" : " (", cache_valid ? "" : why, cache_valid ? "" : ")");

    // -thm_verify: every .thm is read again and compared with the record taken (the cache must give the same)
    if (strstr(Core.Params, "-thm_verify"))
    {
        xr_vector<texture_thm_record> fresh(items.size());
        xr_parallel_for(TaskRange<size_t>(0, items.size()), [&](const TaskRange<size_t>& range)
        {
            for (size_t i = range.begin(); i != range.end(); ++i)
                thm_parse(initial, *items[i].file, fresh[i]);
        });
        u32 differ = 0;
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (thm_same(items[i].rec, fresh[i]))
                continue;
            if (++differ <= 16)
            {
                Msg("! [thm-cache] verify: '%s' differs (%s): type %u/%u, flags 0x%08x/0x%08x, material %u/%u, "
                    "bump mode %u/%u, detail '%s'/'%s', bump '%s'/'%s'", items[i].file->name.c_str(),
                    items[i].cached ? "cached" : "read", u32(items[i].rec.type), u32(fresh[i].type),
                    items[i].rec.flags.get(), fresh[i].flags.get(), u32(items[i].rec.material), u32(fresh[i].material),
                    u32(items[i].rec.bump_mode), u32(fresh[i].bump_mode), items[i].rec.detail_name.c_str(),
                    fresh[i].detail_name.c_str(), items[i].rec.bump_name.c_str(), fresh[i].bump_name.c_str());
            }
        }
        Msg("%s [thm-cache] verify %s: %u .thm, %u from the cache, %u differ", differ ? "!" : "*", initial,
            u32(items.size()), from_cache, differ);
    }
}

void CTextureDescrMngr::Load()
{
    ZoneScoped;
#ifndef MASTER_GOLD
    CTimer timer;
    timer.Start();
#endif // #ifdef DEBUG

    const bool listTHM = strstr(Core.Params, "-list_thm");

    LoadLTX("$game_textures$", listTHM);
    LoadLTX("$level$", listTHM);

    LoadTHM("$game_textures$", listTHM);
    LoadTHM("$level$", listTHM);

#ifndef MASTER_GOLD
    Msg("%s, texture descriptions loaded for %d ms", __FUNCTION__, timer.GetElapsed_ms());
#endif
}

void CTextureDescrMngr::UnLoad()
{
    for (auto& it : m_texture_details)
    {
        xr_delete(it.second.m_assoc);
        xr_delete(it.second.m_spec);
    }
    m_texture_details.clear();
}

CTextureDescrMngr::~CTextureDescrMngr()
{
    ZoneScoped;

    for (auto& it : m_detail_scalers)
        xr_delete(it.second);

    m_detail_scalers.clear();
}

shared_str CTextureDescrMngr::GetBumpName(const shared_str& tex_name) const
{
    map_TD::const_iterator I = m_texture_details.find(tex_name);
    if (I != m_texture_details.end())
    {
        if (I->second.m_spec)
        {
            return I->second.m_spec->m_bump_name;
        }
    }
    return "";
}

BOOL CTextureDescrMngr::UseSteepParallax(const shared_str& tex_name) const
{
    map_TD::const_iterator I = m_texture_details.find(tex_name);
    if (I != m_texture_details.end())
    {
        if (I->second.m_spec)
        {
            return I->second.m_spec->m_use_steep_parallax;
        }
    }
    return FALSE;
}

float CTextureDescrMngr::GetMaterial(const shared_str& tex_name) const
{
    map_TD::const_iterator I = m_texture_details.find(tex_name);
    if (I != m_texture_details.end())
    {
        if (I->second.m_spec)
        {
            return I->second.m_spec->m_material;
        }
    }
    return 1.0f;
}

void CTextureDescrMngr::GetTextureUsage(const shared_str& tex_name, bool& bDiffuse, bool& bBump) const
{
    map_TD::const_iterator I = m_texture_details.find(tex_name);
    if (I != m_texture_details.end())
    {
        if (I->second.m_assoc)
        {
            auto& usage = I->second.m_assoc->usage;
            bDiffuse = usage.test(texture_assoc::flDiffuseDetail);
            bBump = usage.test(texture_assoc::flBumpDetail);
        }
    }
}

BOOL CTextureDescrMngr::GetDetailTexture(const shared_str& tex_name, LPCSTR& res, R_constant_setup*& CS) const
{
    map_TD::const_iterator I = m_texture_details.find(tex_name);
    if (I != m_texture_details.end())
    {
        if (I->second.m_assoc)
        {
            texture_assoc* TA = I->second.m_assoc;
            res = TA->detail_name.c_str();
            map_CS::const_iterator It2 = m_detail_scalers.find(tex_name);
            CS = It2 == m_detail_scalers.end() ? 0 : It2->second;
            return TRUE;
        }
    }
    return FALSE;
}
} // namespace xray::render::RENDER_NAMESPACE
