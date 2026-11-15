#include "StdAfx.h"

#include "level_preview_model.h"

#include "Level.h"
#include "xrCDB/ISpatial.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/IRenderable.h"
#include "Include/xrRender/RenderVisual.h"
#include "xrCore/FMesh.hpp"

#include <array>
#include <mutex>

namespace gw::preview_model
{
namespace
{
// The drawn part of a preview: a spatial (so the render finds it by the view frustum) and a renderable (so it adds
// its visual as any dynamic object does), as CPS_Instance of the particles - but no ScheduledBase, no physics, no
// collision form. Lives only on the main thread between the frames (Device.PostJoin, CLevel::~CLevel).
class CPreviewModel final : public SpatialBase, public RenderableBase
{
public:
    // The ROS stays allowed (RenderableBase): the render keeps the hemi / sun lighting of a dynamic model in it, as
    // for a game object - without it the model would take the lighting constants of whatever was drawn before
    CPreviewModel() : SpatialBase(g_pGamePersistent->SpatialSpace) {}

    ~CPreviewModel() override
    {
        // The spatial part must leave the DB before the visual goes
        spatial_unregister();
        // Discarded, not given back to the pool (RenderableBase would do model_Delete without discard): the pool hands
        // a cached instance to the next model_Create of the name - a game object would get the ghost shaders
        if (renderable.visual)
            GEnv.Render->model_Delete(renderable.visual, true);
    }

    IRenderable* dcast_Renderable() override { return this; }

    void renderable_Render(u32 context_id, IRenderable* root) override
    {
        if (renderable.visual)
            GEnv.Render->add_Visual(context_id, root, renderable.visual, renderable.xform);
    }

    // The model of the preview: a new instance from the model pool (cached after the first use), turned into a ghost
    // (IRender::model_MakeGhost changes the shaders of this instance only: model_Create duplicates the meshes)
    bool SetVisual(const shared_str& name)
    {
        if (m_visual_name == name && renderable.visual)
            return true;
        IRenderVisual* visual = GEnv.Render->model_Create(name.c_str());
        if (!visual)
            return false;
        if (renderable.visual)
            GEnv.Render->model_Delete(renderable.visual, true); // discarded: see ~CPreviewModel
        renderable.visual = visual;
        m_visual_name = name;
        if (GEnv.Render->model_MakeGhost(visual))
            GEnv.Render->model_SetGhostTint(visual, m_tint);
        UpdateSpatial();
        return true;
    }

    void SetTint(const Fvector4& tint)
    {
        m_tint = tint;
        if (renderable.visual)
            GEnv.Render->model_SetGhostTint(renderable.visual, m_tint);
    }

    void SetTransform(const Fmatrix& xform)
    {
        renderable.xform.set(xform);
        UpdateSpatial();
    }

private:
    // The bounding sphere of the model at the transform: what the render culls by
    void UpdateSpatial()
    {
        if (!renderable.visual)
            return;
        const vis_data& vis = renderable.visual->getVisData();
        Fvector center;
        renderable.xform.transform_tiny(center, vis.sphere.P);
        const float radius = _valid(vis.sphere.R) && vis.sphere.R > 0.f ? vis.sphere.R : 1.f;
        spatial.sphere.set(center, radius);
        if (!m_registered)
        {
            spatial.type = STYPE_RENDERABLE; // set here: RenderableBase cannot see the spatial in its constructor
            spatial_register();
            m_registered = true;
        }
        else
            spatial_move();
    }

    shared_str m_visual_name;
    Fvector4 m_tint{kDefaultTint[0], kDefaultTint[1], kDefaultTint[2], kDefaultTint[3]};
    bool m_registered = false;
};

// What the Lua calls see (Show / Hide answer from it at once)
struct SlotState
{
    bool shown = false;
    Fvector4 tint{kDefaultTint[0], kDefaultTint[1], kDefaultTint[2], kDefaultTint[3]};
};

std::array<SlotState, kMaxPreviews> g_slots;
// The drawn previews: touched only on the main thread at the end of a frame and in Clear
std::array<CPreviewModel*, kMaxPreviews> g_models{};
// Bumped by Clear: a change queued for the level that is gone is dropped
u32 g_generation = 0;

Fmatrix MakeTransform(const Fvector& position, const Fvector& hpb)
{
    Fmatrix xform;
    xform.setHPB(hpb.x, hpb.y, hpb.z); // = setXYZ(p, h, b) of CGameObject::net_Spawn for o_Angle = (p, h, b)
    xform.translate_over(position);
    return xform;
}

// The model file as the model pool looks for it (CModelPool::Instance_Load): the name with .ogf, in the level folder
// or in meshes. `path` - the full path of the file found. A missing file stops the game there, so it is checked before
// the change is queued.
bool FindModelFile(pcstr visual, string_path& path)
{
    string_path name;
    if (!strext(visual))
        strconcat(sizeof(name), name, visual, ".ogf");
    else
        xr_strcpy(name, visual);
    return FS.exist(path, "$level$", name) || FS.exist(path, "$game_meshes$", name);
}

bool ModelExists(pcstr visual)
{
    string_path path;
    return FindModelFile(visual, path);
}

// ModelBounds: the boxes read so far (valid - false: the name was refused, not read again). Lua may call it from the
// Game thread, Clear runs on the main thread: the lock
struct BoundsEntry
{
    bool valid = false;
    Fbox box;
};
std::mutex g_bounds_lock;
xr_unordered_map<shared_str, BoundsEntry> g_bounds;
// ModelFileExists: the missing names told in the log already (under g_bounds_lock too)
xr_set<shared_str> g_missing_logged;

// Reads the box of the OGF header of the file; false - no header of this version or a bad box
bool ReadModelBounds(pcstr path, Fbox& box)
{
    IReader* reader = FS.r_open(path);
    if (!reader)
        return false;
    ogf_header header;
    // find_chunk + a size check, not r_chunk_safe: that one asserts on a chunk of another size (a broken file)
    const bool read = reader->find_chunk(OGF_HEADER) == sizeof(header);
    if (read)
        reader->r(&header, sizeof(header));
    FS.r_close(reader);
    if (!read || header.format_version != xrOGF_FormatVersion)
        return false;
    box.set(header.bb.min, header.bb.max);
    return _valid(box.vMin) && _valid(box.vMax) && box.vMin.x <= box.vMax.x && box.vMin.y <= box.vMax.y &&
        box.vMin.z <= box.vMax.z;
}

void Destroy(u32 id)
{
    if (g_models[id])
        xr_delete(g_models[id]);
}

// Applies `fn` on the main thread at the end of the frame, if the level it was queued for is still there
template <typename F>
void Queue(F&& fn)
{
    const u32 generation = g_generation;
    Device.PostJoin([generation, fn = std::forward<F>(fn)]() {
        if (generation != g_generation || !g_pGameLevel)
            return;
        fn();
    });
}

bool ValidSlot(u32 id, pcstr function)
{
    if (id < kMaxPreviews)
        return true;
    Msg("! [preview_model] %s: bad id %u (0..%u)", function, id, kMaxPreviews - 1);
    return false;
}
} // namespace

bool Show(u32 id, pcstr visual, const Fvector& position, const Fvector& hpb)
{
    if (!g_pGameLevel || !ValidSlot(id, "preview_model_show"))
        return false;
    if (!visual || !visual[0])
    {
        Msg("! [preview_model] preview_model_show: empty model name");
        return false;
    }
    // A name from Lua: the paths built from it below (".ogf", the folder of $level$ / $game_meshes$) are string_path
    // buffers, an overflow there is fatal (strconcat / strcpy_s); real model names are far shorter
    if (xr_strlen(visual) >= sizeof(string_path) / 2)
    {
        Msg("! [preview_model] preview_model_show: the model name is too long (%u chars)", u32(xr_strlen(visual)));
        return false;
    }
    if (!_valid(position) || !_valid(hpb))
    {
        Msg("! [preview_model] preview_model_show: invalid position or hpb");
        return false;
    }
    if (!ModelExists(visual))
    {
        Msg("! [preview_model] preview_model_show: no model file '%s'", visual);
        return false;
    }
    g_slots[id].shown = true;
    const shared_str name = visual;
    const Fmatrix xform = MakeTransform(position, hpb);
    const Fvector4 tint = g_slots[id].tint;
    Queue([id, name, xform, tint]() {
        if (!g_models[id])
            g_models[id] = xr_new<CPreviewModel>();
        g_models[id]->SetTransform(xform);
        g_models[id]->SetTint(tint); // before SetVisual: a new model takes it at once
        if (!g_models[id]->SetVisual(name))
        {
            Msg("! [preview_model] cannot create the model '%s'", name.c_str());
            Destroy(id);
        }
    });
    return true;
}

bool SetTransform(u32 id, const Fvector& position, const Fvector& hpb)
{
    if (!g_pGameLevel || !ValidSlot(id, "preview_model_set_transform") || !g_slots[id].shown)
        return false;
    if (!_valid(position) || !_valid(hpb))
    {
        Msg("! [preview_model] preview_model_set_transform: invalid position or hpb");
        return false;
    }
    const Fmatrix xform = MakeTransform(position, hpb);
    Queue([id, xform]() {
        if (g_models[id])
            g_models[id]->SetTransform(xform);
    });
    return true;
}

bool SetColor(u32 id, const Fvector4& color)
{
    if (!ValidSlot(id, "preview_model_set_color"))
        return false;
    if (!_valid(color.x) || !_valid(color.y) || !_valid(color.z) || !_valid(color.w))
    {
        Msg("! [preview_model] preview_model_set_color: invalid color");
        return false;
    }
    Fvector4 tint;
    tint.set(clampr(color.x, 0.f, 1.f), clampr(color.y, 0.f, 1.f), clampr(color.z, 0.f, 1.f), clampr(color.w, 0.f, 1.f));
    if (g_slots[id].tint.similar(tint, EPS_S))
        return true; // the same color every frame (a placement mode) - nothing to queue
    g_slots[id].tint = tint;
    if (!g_pGameLevel || !g_slots[id].shown)
        return true; // kept for the next Show
    Queue([id, tint]() {
        if (g_models[id])
            g_models[id]->SetTint(tint);
    });
    return true;
}

bool Hide(u32 id)
{
    if (!ValidSlot(id, "preview_model_hide") || !g_slots[id].shown)
        return false;
    g_slots[id].shown = false;
    if (!g_pGameLevel)
        return true;
    Queue([id]() { Destroy(id); });
    return true;
}

void HideAll()
{
    for (u32 id = 0; id < kMaxPreviews; ++id)
    {
        if (g_slots[id].shown)
            Hide(id);
    }
}

bool IsShown(u32 id) { return id < kMaxPreviews && g_slots[id].shown; }

bool ModelBounds(pcstr visual, Fbox& box)
{
    if (!visual || !visual[0])
        return false;
    // the same limit as Show: the path buffers below are string_path
    if (xr_strlen(visual) >= sizeof(string_path) / 2)
    {
        Msg("! [preview_model] model_bounds: the model name is too long (%u chars)", u32(xr_strlen(visual)));
        return false;
    }
    const shared_str name = visual;
    {
        std::lock_guard<std::mutex> guard(g_bounds_lock);
        const auto it = g_bounds.find(name);
        if (it != g_bounds.end())
        {
            if (it->second.valid)
                box.set(it->second.box);
            return it->second.valid;
        }
    }
    BoundsEntry entry;
    string_path path;
    if (!FindModelFile(visual, path))
        Msg("! [preview_model] model_bounds: no model file '%s'", visual);
    else
    {
        entry.valid = ReadModelBounds(path, entry.box);
        if (!entry.valid)
            Msg("! [preview_model] model_bounds: no bounding box in '%s' (not an OGF v%u?)", path,
                u32(xrOGF_FormatVersion));
    }
    {
        std::lock_guard<std::mutex> guard(g_bounds_lock);
        g_bounds.emplace(name, entry);
    }
    if (entry.valid)
        box.set(entry.box);
    return entry.valid;
}

bool ModelFileExists(pcstr visual, pcstr function)
{
    if (!visual || !visual[0])
        return false;
    // the same limit as Show: the path buffers of the lookup (and of the model pool) are string_path
    if (xr_strlen(visual) >= sizeof(string_path) / 2)
    {
        Msg("! [preview_model] %s: the model name is too long (%u chars)", function, u32(xr_strlen(visual)));
        return false;
    }
    if (ModelExists(visual))
        return true;
    bool first;
    {
        std::lock_guard<std::mutex> guard(g_bounds_lock);
        first = g_missing_logged.insert(shared_str(visual)).second;
    }
    if (first)
        Msg("! [preview_model] %s: no model file '%s'", function, visual);
    return false;
}

void Clear()
{
    {
        // the level folder ($level$) may hold models of the same names on the next level
        std::lock_guard<std::mutex> guard(g_bounds_lock);
        g_bounds.clear();
        g_missing_logged.clear();
    }
    ++g_generation;
    for (u32 id = 0; id < kMaxPreviews; ++id)
    {
        g_slots[id].shown = false;
        g_slots[id].tint.set(kDefaultTint[0], kDefaultTint[1], kDefaultTint[2], kDefaultTint[3]);
        Destroy(id);
    }
}
} // namespace gw::preview_model
