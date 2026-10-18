#include "stdafx.h"
#pragma hdrstop

#include <DirectXTex.h>

#include <atomic>

namespace xray::render::RENDER_NAMESPACE
{
void fix_texture_name(pstr fn)
{
    pstr _ext = strext(fn);
    if (_ext && (!xr_stricmp(_ext, ".tga") || !xr_stricmp(_ext, ".dds") || !xr_stricmp(_ext, ".bmp") ||
        !xr_stricmp(_ext, ".ogm")))
    {
        *_ext = 0;
    }
}

int get_texture_load_lod(LPCSTR fn)
{
    CInifile::Sect& sect = pSettings->r_section("reduce_lod_texture_list");
    auto it_ = sect.Data.cbegin();
    auto it_e_ = sect.Data.cend();

    ENGINE_API bool is_enough_address_space_available();
    static bool enough_address_space_available = is_enough_address_space_available();

    auto it = it_;
    auto it_e = it_e_;

    for (; it != it_e; ++it)
    {
        if (strstr(fn, it->first.c_str()))
        {
            if (psTextureLOD < 1)
            {
                if (enough_address_space_available)
                    return 0;
                else
                    return 1;
            }
            else if (psTextureLOD < 3)
                return 1;
            else
                return 2;
        }
    }

    if (psTextureLOD < 2)
    {
        //if (enough_address_space_available)
        return 0;
        //else
        //    return 1;
    }
    else if (psTextureLOD < 4)
        return 1;
    else
        return 2;
}

u32 calc_texture_size(int lod, u32 mip_cnt, size_t orig_size)
{
    if (1 == mip_cnt)
        return orig_size;

    int _lod = lod;
    float res = float(orig_size);

    while (_lod > 0)
    {
        --_lod;
        res -= res / 1.333f;
    }
    return iFloor(res);
}

//////////////////////////////////////////////////////////////////////
// Utility pack
//////////////////////////////////////////////////////////////////////

IC void Reduce(size_t& w, size_t& h, size_t& l, int skip)
{
    while ((l > 1) && skip)
    {
        w /= 2;
        h /= 2;
        l -= 1;

        skip--;
    }
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
}

// 40-parallel-prefetch.md (plans/optimization_mt): textures created by texture_load_direct (the deferred upload logs it)
std::atomic<u32> g_texture_direct_loads{0};

namespace
{
constexpr u32 kDdsPfFourCC = 0x4; // DDPIXELFORMAT.dwFlags: DDPF_FOURCC
constexpr size_t kDdsHeaderSize = 4 + 124; // "DDS " + DDS_HEADER
constexpr size_t kDdsHeaderDX10Size = 20; // DDS_HEADER_DXT10
constexpr size_t kDdsMaxMips = 16;

// 40-parallel-prefetch.md: a block-compressed 2D texture (BC1-BC7 by a FourCC: DXT1-5, ATI1/2, BC4/5, a DX10 header)
// straight from the file: its mips are given to CreateTextureEx as they lie in the file. LoadFromDDSMemory copies such
// a file into a ScratchImage first, unchanged (block-compressed formats are never converted): an allocation of the
// size of the file, its page faults and a copy of every texture. Anything else (a cube, an array, a volume, another
// format, a short file) or a failed create - false, and texture_load takes the common path, as before.
bool texture_load_direct(const u8* src, size_t size, DirectX::DDS_FLAGS flags, pstr fn, ID3DBaseTexture*& out,
    u32& ret_msize)
{
    DirectX::TexMetadata IMG;
    DirectX::DDSMetaData pf{};
    if (FAILED(DirectX::GetMetadataFromDDSMemoryEx(src, size, flags, IMG, &pf)))
        return false;
    if (!(pf.flags & kDdsPfFourCC) || !DirectX::IsCompressed(IMG.format) ||
        IMG.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || IMG.IsCubemap() || IMG.arraySize != 1 ||
        IMG.depth != 1 || IMG.mipLevels < 1 || IMG.mipLevels > kDdsMaxMips)
        return false;

    // The mips one after another after the header, as LoadFromDDSMemory reads them (CP_FLAGS_NONE pitches)
    DirectX::Image images[kDdsMaxMips];
    size_t offset = kDdsHeaderSize + (pf.IsDX10() ? kDdsHeaderDX10Size : 0);
    size_t w = IMG.width, h = IMG.height;
    for (size_t level = 0; level < IMG.mipLevels; ++level)
    {
        size_t row_pitch, slice_pitch;
        if (FAILED(DirectX::ComputePitch(IMG.format, w, h, row_pitch, slice_pitch)))
            return false;
        if (offset > size || slice_pitch > size - offset)
            return false;
        images[level] = { w, h, IMG.format, row_pitch, slice_pitch, const_cast<u8*>(src + offset) };
        offset += slice_pitch;
        w = std::max<size_t>(1, w / 2);
        h = std::max<size_t>(1, h / 2);
    }

    if (strstr(fn, "terrain") || strstr(fn, "Terrain"))
        Msg("[TERRAIN_TEX] Loaded OK: %s | size=%zu | %ux%u | mips=%zu | fmt=%d",
            fn, size, (u32)IMG.width, (u32)IMG.height, IMG.mipLevels, (int)IMG.format);

    // The same LOD reduction and size rounding as the common path below
    xr_strlwr(fn);
    const int img_loaded_lod = get_texture_load_lod(fn);
    size_t mip_lod = 0;
    if (img_loaded_lod)
    {
        const auto old_mipmap_cnt = IMG.mipLevels;
        Reduce(IMG.width, IMG.height, IMG.mipLevels, img_loaded_lod);
        mip_lod = old_mipmap_cnt - IMG.mipLevels;
    }
    IMG.width = (IMG.width + 3u) & ~0x3u;
    IMG.height = (IMG.height + 3u) & ~0x3u;

    ID3DBaseTexture* texture = nullptr;
    const HRESULT hresult = DirectX::CreateTextureEx(HW.pDevice, images + mip_lod, IMG.mipLevels, IMG,
        D3D_USAGE_IMMUTABLE, D3D_BIND_SHADER_RESOURCE, 0, IMG.miscFlags, DirectX::CREATETEX_DEFAULT, &texture);
    if (FAILED(hresult) || !texture)
        return false;
    out = texture;
    ret_msize = calc_texture_size(img_loaded_lod, IMG.mipLevels, size);
    g_texture_direct_loads.fetch_add(1, std::memory_order_relaxed);
    return true;
}
} // namespace

ID3DBaseTexture* CRender::texture_load(LPCSTR fRName, u32& ret_msize)
{
    ret_msize = 0;
    R_ASSERT1_CURE(fRName && fRName[0], { return nullptr; });

    ID3DBaseTexture* pTexture2D{};
    string_path fn;
    {
        // make file name
        string_path fname;
        xr_strcpy(fname, fRName);
        fix_texture_name(fname);

        // Call to FS.exist WRITES to fn !

        if (!FS.exist(fn, "$game_textures$", fname, ".dds") && strstr(fname, "_bump"))
        {
            Msg("! Fallback to default bump map: %s", fname);
            if (strstr(fname, "_bump#"))
                R_ASSERT1_CURE(FS.exist(fn, "$game_textures$", "ed\\ed_dummy_bump#", ".dds"), return nullptr);
            else
                R_ASSERT1_CURE(FS.exist(fn, "$game_textures$", "ed\\ed_dummy_bump", ".dds"), return nullptr);
        }
        else
        {
            bool exist = false;

            for (cpcstr folder : { "$level$", "$game_saves$", "$game_textures$" })
            {
                exist = FS.exist(fn, folder, fname, ".dds");
                if (exist)
                {
                    if (strstr(fname, "terrain") || strstr(fname, "Terrain"))
                        Msg("[TERRAIN_TEX] Found in %s: %s -> %s", folder, fname, fn);
                    break;
                }
            }

            // Disk-cached preview textures in appdata (npc_preview_* DDS cache).
            if (!exist && FS.path_exist("$preview_cache$"))
            {
                string_path preview_nm;
                strconcat(sizeof preview_nm, preview_nm, fname, ".dds");
                if (FS.update_path(fn, "$preview_cache$", preview_nm, false) && FS.exist(fn, FSType::Any))
                    exist = true;
            }

            if (!exist)
            {
                if (strstr(fname, "terrain") || strstr(fname, "Terrain"))
                    Msg("! [TERRAIN_TEX] Can't find terrain texture '%s'", fname);
                Msg("! Can't find texture '%s'", fname);
                if (!FS.exist(fn, "$game_textures$", "ed\\ed_not_existing_texture", ".dds"))
                    return nullptr;
            }
        }
    }

    // Load and get header
    IReader* S = FS.r_open(fn);
    R_ASSERT3_CURE(S, "Can't open texture", fn, { return nullptr; });

    size_t img_size = S->length();
#ifdef DEBUG
    Msg("* Loaded: %s[%zu]", fn, img_size);
#endif // DEBUG

    DirectX::DDS_FLAGS dds_flags{ DirectX::DDS_FLAGS_PERMISSIVE };

    // 40-parallel-prefetch.md: block-compressed textures without the copy
    if (texture_load_direct(static_cast<const u8*>(S->pointer()), img_size, dds_flags, fn, pTexture2D, ret_msize))
    {
        FS.r_close(S);
        return pTexture2D;
    }

    for (int i = 1; i <= 3; ++i) // 3 attempts
    {
        DirectX::TexMetadata IMG;
        DirectX::ScratchImage texture;
        auto hresult = LoadFromDDSMemory(static_cast<u8*>(S->pointer()), img_size, dds_flags, &IMG, texture);

        if (SUCCEEDED(hresult) && (strstr(fn, "terrain") || strstr(fn, "Terrain")))
            Msg("[TERRAIN_TEX] Loaded OK: %s | size=%zu | %ux%u | mips=%zu | fmt=%d",
                fn, img_size, (u32)IMG.width, (u32)IMG.height, IMG.mipLevels, (int)IMG.format);

        R_ASSERT3_CURE(SUCCEEDED(hresult), "Failed to load texture from memory", fn,
        {
            FS.r_close(S);
            return nullptr;
        });

        // Check for LMAP and compress if needed
        xr_strlwr(fn);

        const int img_loaded_lod = get_texture_load_lod(fn);

        size_t mip_lod = 0;
        if (img_loaded_lod && !IMG.IsCubemap())
        {
            const auto old_mipmap_cnt = IMG.mipLevels;
            Reduce(IMG.width, IMG.height, IMG.mipLevels, img_loaded_lod);
            mip_lod = old_mipmap_cnt - IMG.mipLevels;
        }

        // DirectX requires compressed texture size to be
        // a multiple of 4. Make sure to meet this requirement.
        if (DirectX::IsCompressed(IMG.format))
        {
            IMG.width = (IMG.width + 3u) & ~0x3u;
            IMG.height = (IMG.height + 3u) & ~0x3u;
        }

        hresult = CreateTextureEx(HW.pDevice, texture.GetImages() + mip_lod, texture.GetImageCount(), IMG,
            D3D_USAGE_IMMUTABLE, D3D_BIND_SHADER_RESOURCE, 0, IMG.miscFlags, DirectX::CREATETEX_DEFAULT,
            &pTexture2D);

        if (SUCCEEDED(hresult))
        {
            // OK
            ret_msize = calc_texture_size(img_loaded_lod, IMG.mipLevels, img_size);
            break;
        }

        if (i == 1)
            dds_flags |= DirectX::DDS_FLAGS::DDS_FLAGS_NO_16BPP; // System isn't WDDM 1.2 compliant
        else if (i == 2)
            dds_flags |= DirectX::DDS_FLAGS::DDS_FLAGS_FORCE_RGB; // Not even WDDM 1.1 compliant
        else if (i == 3)
            Msg("! Could not load texture [%s] after %d attempts", fn, i);
        else
        {
            NODEFAULT;
        }
    }

    FS.r_close(S);
    return pTexture2D;
}
} // namespace xray::render::RENDER_NAMESPACE
