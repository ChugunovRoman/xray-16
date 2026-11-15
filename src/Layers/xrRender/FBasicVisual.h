#pragma once

#include "xrEngine/vis_common.h"

#include "Include/xrRender/RenderVisual.h"

#define VLOAD_NOVERTICES (1 << 0)

class IParticleCustom;

namespace xray::render::RENDER_NAMESPACE
{
// The class itself
class CKinematicsAnimated;
class CKinematics;

struct IRender_Mesh
{
    // format
    ref_geom rm_geom;

    // verts
    VertexStagingBuffer* p_rm_Vertices;
    u32 vBase;
    u32 vCount;
    u32 vStride{};

    // indices
    IndexStagingBuffer* p_rm_Indices;
    u32 iBase;
    u32 iCount;
    u32 dwPrimitives;

    IRender_Mesh()
    {
        p_rm_Vertices   = nullptr;
        vBase           = 0;
        vCount          = 0;
        p_rm_Indices    = nullptr;
        iBase           = 0;
        iCount          = 0;
        dwPrimitives    = 0;
        rm_geom         = nullptr;
    }
    virtual ~IRender_Mesh();

private:
    IRender_Mesh(const IRender_Mesh& other);
    void operator=(const IRender_Mesh& other);
};

// The class itself
// GPU instancing (plans/render_instancing/plan.md): the one indexed draw a visual's Render() issues -
// set_Geometry(geom) + Render(D3DPT_TRIANGLELIST, baseV, 0, countV, startI, PC). Two visuals with equal draws are
// copies of one mesh: render_matrix_items draws them with one instanced draw.
struct instancing_draw
{
    SGeometry* geom;
    u32 baseV;
    u32 countV;
    u32 startI;
    u32 PC;
};

class ECORE_API dxRender_Visual : public IRenderVisual
{
public:
#ifdef _EDITOR
    ogf_desc desc;
#endif
#ifdef DEBUG
    shared_str dbg_name;
    virtual shared_str getDebugName() { return dbg_name; }
#endif
public:
    // Common data for rendering
    u32 Type; // visual's type
    vis_data vis; // visibility-data
    ref_shader shader; // pipe state, shared

    virtual void Render(CBackend& cmd_list, float /*LOD*/, bool use_fast_geo) {} // LOD - Level Of Detail  [0..1], Ignored
    virtual void Load(const char* N, IReader* data, u32 dwFlags);
    virtual void Load(const char* N, LPCSTR suffix, IReader* data, u32 dwFlags);
    virtual void Release(); // Shared memory release
    virtual void Copy(dxRender_Visual* from);
    virtual void Spawn(){};
    virtual void Depart(){};

    // GPU instancing: true if Render(cmd_list, LOD, use_fast_geo) of this visual under the world transform `xform`
    // (set_xform_world of render_matrix_items, the pass already set) is exactly one draw `draw` of a rigid mesh whose
    // vertices go through the world matrix `world`; it then has the same side effects on the visual as Render() (the
    // progressive LOD it remembers). False - the visual is drawn by Render() (skinning, several draws, anything else).
    virtual bool instancing_get(CBackend& /*cmd_list*/, const Fmatrix& /*xform*/, float /*LOD*/, bool /*use_fast_geo*/,
        instancing_draw& /*draw*/, Fmatrix& /*world*/)
    {
        return false;
    }

    //	virtual	CKinematics*		dcast_PKinematics			()				{ return 0;	}
    //	virtual	CKinematicsAnimated*dcast_PKinematicsAnimated	()				{ return 0;	}
    //	virtual IParticleCustom*	dcast_ParticleCustom		()				{ return 0;	}

    virtual vis_data& getVisData() { return vis; }
    u32 getType() const override { return Type; }
    dxRender_Visual();
    virtual ~dxRender_Visual();
};

// plans/addons_api_support/14-addon-resource-scoping.md: the addon the model being loaded on this thread belongs to -
// the owner of the file system layer its ogf lies in (FS.layer_owner). CModelPool::Instance_Load sets it around the
// load from a file: the children loaded from the chunks of that ogf inherit it, a LOD (its own ogf) gets its own, a
// deferred motions part carries it (PrefetchMotionsPart::owner). The names a model of an addon takes go to the alias
// roots of the addon first: textures\@<owner>\ (dxRender_Visual::Load), meshes\@<owner>\ (motions).
class VisualOwnerScope
{
public:
    explicit VisualOwnerScope(const shared_str& owner);
    ~VisualOwnerScope();
    VisualOwnerScope(const VisualOwnerScope&) = delete;
    VisualOwnerScope& operator=(const VisualOwnerScope&) = delete;

    static const shared_str& current(); // empty - a model of the base (or no load)
    // The owner of the layer the file `fn` (a full name of the index) lies in; empty - the base, no owner
    static shared_str of_file(pcstr fn);

private:
    shared_str m_owner;
    const shared_str* m_previous;
};

// "@<owner>\<name>" into dest when the current owner (VisualOwnerScope) has the file <path>@<owner>\<name><ext> in its
// alias root and <path><name><ext> is not that same file (a private one, or replaced by the base / another layer);
// false - the name stays: no owner, no such file, the public name is the file of the owner, or not a plain name
// ('@...', '$...')
bool VisualOwnerName(pcstr path, pcstr name, pcstr ext, string_path& dest);
} // namespace xray::render::RENDER_NAMESPACE
