// dxRender_Visual.cpp: implementation of the dxRender_Visual class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#pragma hdrstop

#include "FBasicVisual.h"
#include "xrCore/FMesh.hpp"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "ModelPool.h"

namespace xray::render::RENDER_NAMESPACE
{
//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

IRender_Mesh::~IRender_Mesh()
{
    rm_geom.destroy();
    _RELEASE(p_rm_Vertices);
    _RELEASE(p_rm_Indices);
}

dxRender_Visual::dxRender_Visual()
{
    Type = 0;
    shader = nullptr;
    vis.clear();
}

dxRender_Visual::~dxRender_Visual() {}
void dxRender_Visual::Release() {}
// CStatTimer						tscreate;

// 14-addon-resource-scoping.md (plans/addons_api_support)
namespace
{
thread_local const shared_str* tl_visual_owner = nullptr;
const shared_str s_visual_no_owner;

// Each texture of the comma list `list` (OGF_TEXTURE) the owner of the load has under its alias root:
// "@<owner>\<name>". The rest stay as they are.
void visual_owner_textures(pstr list, size_t size)
{
    const int count = _GetItemCount(list);
    string_path result{};
    bool changed = false;
    for (int k = 0; k < count; ++k)
    {
        string_path item, aliased;
        _GetItem(list, k, item);
        const bool own = VisualOwnerName("$game_textures$", item, ".dds", aliased);
        changed |= own;
        if (k)
            xr_strcat(result, ",");
        xr_strcat(result, own ? aliased : item);
    }
    if (changed && xr_strlen(result) < size)
        xr_strcpy(list, size, result);
}
} // namespace

VisualOwnerScope::VisualOwnerScope(const shared_str& owner) : m_owner(owner), m_previous(tl_visual_owner)
{
    tl_visual_owner = &m_owner;
}

VisualOwnerScope::~VisualOwnerScope() { tl_visual_owner = m_previous; }

const shared_str& VisualOwnerScope::current() { return tl_visual_owner ? *tl_visual_owner : s_visual_no_owner; }

shared_str VisualOwnerScope::of_file(pcstr fn)
{
    if (!FS.layers_count())
        return {};
    const CLocatorAPI::file* desc = FS.GetFileDesc(fn);
    pcstr owner = desc ? FS.layer_owner(desc->layer) : nullptr;
    return owner ? shared_str(owner) : shared_str();
}

bool VisualOwnerName(pcstr path, pcstr name, pcstr ext, string_path& dest)
{
    const shared_str& owner = VisualOwnerScope::current();
    if (!owner.size() || !name || !name[0] || name[0] == '@' || name[0] == '$')
        return false;
    strconcat(sizeof dest, dest, "@", owner.c_str(), "\\", name);
    string_path own_fn, public_fn;
    if (!FS.exist(own_fn, path, dest, ext))
        return false;
    // The name of everybody served by the file of the same layer (a public file of the addon, nobody replaced it):
    // the name stays - one resource of the file, not two (the texture / omf caches key by the name)
    if (FS.exist(public_fn, path, name, ext))
    {
        const CLocatorAPI::file* own = FS.GetFileDesc(own_fn);
        const CLocatorAPI::file* common = FS.GetFileDesc(public_fn);
        if (own && common && own->layer == common->layer)
            return false;
    }
    return true;
}

void dxRender_Visual::Load(const char* N, IReader* data, u32)
{
    Load(N, "", data, 0);
}
void dxRender_Visual::Load(const char* N, LPCSTR suffix, IReader* data, u32)
{
#ifdef DEBUG
    dbg_name = N;
#endif

    // header
    VERIFY(data);
    ogf_header hdr;
    if (data->r_chunk_safe(OGF_HEADER, &hdr, sizeof(hdr)))
    {
        R_ASSERT2(hdr.format_version == xrOGF_FormatVersion, "Invalid visual version");
        Type = hdr.type;
        if (hdr.shader_id)
            shader = RImplementation.getShader(hdr.shader_id);
        vis.box.set(hdr.bb.min, hdr.bb.max);
        vis.sphere.set(hdr.bs.c, hdr.bs.r);
    }
    else
    {
        FATAL("Invalid visual");
    }

    // Shader
    if (data->find_chunk(OGF_TEXTURE))
    {
        string256 fnT, fnS;
        data->r_stringZ(fnT, sizeof(fnT));
        data->r_stringZ(fnS, sizeof(fnS));
        // 14-addon-resource-scoping.md: a model of an addon takes the textures of that addon first (its alias root),
        // then the ones of the same names of everybody. world_material_N / hud_material_N below override it.
        if (VisualOwnerScope::current().size())
            visual_owner_textures(fnT, sizeof(fnT));
        {
            TextureReplacementsScopeLock scope(GEnv.Render->texture_replacements_lock);
            // N == nullptr: a visual of the level geometry (r2_loader) - not a model, nothing replaces its textures
            if (N && GEnv.Render->texture_replacements.size() > 0)
            {
                string_path low_name_suffix;
                xr_strcpy(low_name_suffix, N);
                if (strlen(suffix) > 0)
                    xr_strcat(low_name_suffix, suffix);
                auto value = GEnv.Render->texture_replacements.find(low_name_suffix);
                if (value != GEnv.Render->texture_replacements.end())
                    strncpy(fnT, value->second.c_str(), sizeof(fnT) - 1);
            }
        }
        MODEL_LOAD_PART("model_load/shader", mload_shader); // 36-spawn-model-load.md
        shader.create(fnS, fnT);
    }

// desc
#ifdef _EDITOR
    if (data->find_chunk(OGF_S_DESC))
        desc.Load(*data);
#endif
}

#define PCOPY(a) a = pFrom->a
void dxRender_Visual::Copy(dxRender_Visual* pFrom)
{
    PCOPY(Type);
    PCOPY(shader);
    PCOPY(vis);
#ifdef _EDITOR
    PCOPY(desc);
#endif
#ifdef DEBUG
    PCOPY(dbg_name);
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
