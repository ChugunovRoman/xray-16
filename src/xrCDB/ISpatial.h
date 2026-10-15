#pragma once

#include <atomic>
#include <mutex>
#include <shared_mutex>

#include "Common/Noncopyable.hpp"

#include "xrCore/xrPool.h"
#include "xrCore/Threading/Lock.hpp"
//#include "xr_collide_defs.h"
#include "xrCore/xr_types.h"
#include "xrCore/_vector3d.h"
#include "xrCore/_sphere.h"
#include "xrCore/FTimer.h"
#include "xrCore/Profiling/PerfMetrics.hpp"

#include "xrCDB.h"

// No #pragma pack(push, 4) here any more (legacy from the original engine): it capped the alignment of
// SpatialData's std::atomic<sector_id_t>/node_ptr (offsets 44/36 on x64) and of ISpatial_DB's shared_mutex
// to 4, so with physics now moving objects on its own thread an 8-byte atomic could straddle a cache line
// (split lock, no atomicity guarantee). Nothing in this header is serialized or part of the plugin ABI.

//////////////////////////////////////////////////////////////////////////
// definition (Portal)
class IRender_Portal
{
public:
    virtual ~IRender_Portal() = default;
};

//////////////////////////////////////////////////////////////////////////
// definition (Sector)
class IRender_Sector
{
public:
    using sector_id_t = size_t;
    static auto constexpr INVALID_SECTOR_ID = static_cast<sector_id_t>(-1);

    virtual ~IRender_Sector() = default;
    sector_id_t unique_id{INVALID_SECTOR_ID};
};

/*
Locking (ISpatial_DB):
- Queries (q_box / q_frustum / q_ray) use shared_lock on `rw` (read-only tree walk; each call writes only to its
  own `R` vector passed into the query). Inserts/removes use unique_lock.
- DEBUG `Stats.Query` uses `ScopeStatTimer` + `query_stats_lock` when gathering timings.

Requirements:
0. Generic
    * O(1) insertion
        - radius completely determines	"level"
        - position completely determines "node"
    * O(1) removal
    *
1. Rendering
    * Should live inside spatial DB
    * Should have at least "bounding-sphere" or "bounding-box"
    * Should have pointer to "sector" it lives in
    * Approximate traversal order relative to point ("camera")
2. Spatial queries
    * Should live inside spatial DB
    * Should have at least "bounding-sphere" or "bounding-box"
*/

const float c_spatial_min = 8.f;
//////////////////////////////////////////////////////////////////////////
enum
{
    STYPE_RENDERABLE = (1 << 0),
    STYPE_LIGHTSOURCE = (1 << 1),
    STYPE_COLLIDEABLE = (1 << 2),
    STYPE_VISIBLEFORAI = (1 << 3),
    STYPE_REACTTOSOUND = (1 << 4),
    STYPE_PHYSIC = (1 << 5),
    STYPE_OBSTACLE = (1 << 6),
    STYPE_SHAPE = (1 << 7),
    STYPE_LIGHTSOURCEHEMI = (1 << 8),

    STYPEFLAG_INVALIDSECTOR = (1 << 16)
};
//////////////////////////////////////////////////////////////////////////
// Comment:
//		ordinal objects			- renderable?, collideable?, visibleforAI?
//		physical-decorations	- renderable, collideable
//		lights					- lightsource
//		particles(temp-objects)	- renderable
//		glow					- renderable
//		sound					- ???
//////////////////////////////////////////////////////////////////////////
// class 				IRender_Sector;
// class 				ISpatial;
// class 				ISpatial_NODE;
// class 				ISpatial_DB;

//////////////////////////////////////////////////////////////////////////
// Fast type conversion
// class 			IGameObject;
// class 			IRenderable;
// class 			IRender_Light;
//
// namespace Feel { class Sound; }

//////////////////////////////////////////////////////////////////////////
class ISpatial_NODE;
class ISpatial_DB;
class IGameObject;
namespace Feel
{
class Sound;
}
class IRenderable;
class IRender_Light;

class SpatialData
{
public:
    // Atomic: read/written from render (MakeMeCrow/renderable_Render), physics (PHObject.cpp,
    // contact callbacks like CustomRocket.cpp's setEnabled) and game logic (dozens of sites across
    // xrGame), all on main/GameThread today but serialized - see GAME_RENDER_OVERLAP_PLAN P1.5.
    // Plain |=/&= compound assignment on this type works unchanged (native since C++11); a
    // read-compute-write spread across separate statements does NOT become atomic just by this change -
    // audited and fixed the one such site, GameObject.cpp's old `type = (type | X) ^ X` pattern.
    std::atomic<u32> type{0}; // STYPE_
    Fsphere sphere;
    Fvector node_center; // Cached node center for TBV optimization
    float node_radius; // Cached node bounds for TBV optimization
    ISpatial_NODE* node_ptr; // Cached parent node for "empty-members" optimization
    // Atomic for the same reason as `type` above (GAME_RENDER_OVERLAP_PLAN Phase 3): written unlocked by
    // spatial_updatesector_internal() on the render thread (hot per-frame culling path, ISpatial.cpp) and
    // by spatial_register()/spatial_unregister() under the DB lock on whichever thread owns this object's
    // spatial lifecycle (CPHWorld's dedicated physics thread for physics-driven objects).
    // "Last write wins" is logically benign here (same acceptable-staleness category as sphere.P): a
    // reader seeing a one-frame-stale sector_id just culls against last frame's sector, cosmetic only.
    std::atomic<IRender_Sector::sector_id_t> sector_id;
    ISpatial_DB* space; // allow different spaces
};

class XRCDB_API XR_NOVTABLE ISpatial
{
public:
    virtual ~ISpatial() = 0;
    virtual SpatialData& GetSpatialData() = 0;
    virtual bool spatial_inside() = 0;
    virtual void spatial_register() = 0;
    virtual void spatial_unregister() = 0;
    virtual void spatial_move() = 0;
    virtual Fvector spatial_sector_point() = 0;
    virtual void spatial_updatesector(IRender_Sector::sector_id_t sector_id) = 0;
    virtual IGameObject* dcast_GameObject() = 0;
    virtual Feel::Sound* dcast_FeelSound() = 0;
    virtual IRenderable* dcast_Renderable() = 0;
    virtual IRender_Light* dcast_Light() = 0;
};

inline ISpatial::~ISpatial() = default;

class XRCDB_API XR_NOVTABLE SpatialBase : public virtual ISpatial
{
public:
    SpatialData spatial;

private:
    void spatial_updatesector_internal(IRender_Sector::sector_id_t sector_id);

protected:
    // B-1: refresh spatial.sphere (and related bounds) immediately before register/move under DB lock.
    virtual void spatial_refresh_bounds() {}

public:
    virtual SpatialData& GetSpatialData() override final { return spatial; }
    virtual bool spatial_inside() override final;
    virtual void spatial_register() override;
    virtual void spatial_unregister() override;
    virtual void spatial_move() override;
    virtual Fvector spatial_sector_point() override { return spatial.sphere.P; }
    virtual void spatial_updatesector(IRender_Sector::sector_id_t sector_id) override final
    {
        if (0 == (spatial.type & STYPEFLAG_INVALIDSECTOR))
            return;
        spatial_updatesector_internal(sector_id);
    }

    virtual IGameObject* dcast_GameObject() override { return nullptr; }
    virtual Feel::Sound* dcast_FeelSound() override { return nullptr; }
    virtual IRenderable* dcast_Renderable() override { return nullptr; }
    virtual IRender_Light* dcast_Light() override { return nullptr; }

    SpatialBase(ISpatial_DB& space);
    virtual ~SpatialBase();
};

//////////////////////////////////////////////////////////////////////////
// class ISpatial_NODE;
class ISpatial_NODE
{
public:
    ISpatial_NODE* parent; // parent node for "empty-members" optimization
    ISpatial_NODE* children[8]; // children nodes
    xr_vector<ISpatial*> items; // own items

    void _init(ISpatial_NODE* _parent);
    void _remove(ISpatial* S);
    void _insert(ISpatial* S);
    bool _empty()
    {
        return items.empty() &&
            0 == (uintptr_t(children[0]) | uintptr_t(children[1]) |
                  uintptr_t(children[2]) | uintptr_t(children[3]) |
                  uintptr_t(children[4]) | uintptr_t(children[5]) |
                  uintptr_t(children[6]) | uintptr_t(children[7]));
    }
};

class XRCDB_API ISpatial_DB : private Noncopyable
{
public:
    struct SpatialDBStatistics
    {
        u32 NodeCount;
        u32 ObjectCount;
#ifdef DEBUG
        CStatTimer Insert; // debug only
        CStatTimer Remove; // debug only
#endif
        CStatTimer Query;

        SpatialDBStatistics()
        {
            NodeCount = ObjectCount = 0;
            FrameStart();
        }

        void FrameStart()
        {
#ifdef DEBUG
            Insert.FrameStart();
            Remove.FrameStart();
#endif
            Query.FrameStart();
        }

        void FrameEnd()
        {
#ifdef DEBUG
            Insert.FrameEnd();
            Remove.FrameEnd();
#endif
            Query.FrameEnd();
        }
    };

private:
    std::shared_mutex rw;
    Lock query_stats_lock;

    poolSS<ISpatial_NODE, 128> allocator;

    xr_vector<ISpatial_NODE*> allocator_pool;
    ISpatial* rt_insert_object{};

public:
    string64 Name;
    ISpatial_NODE* m_root{};
    Fvector m_center{};
    float m_bounds{};
    u8 db_index{};
    SpatialDBStatistics Stats;

private:
    friend class ISpatial_NODE;

    IC u32 _octant(u32 x, u32 y, u32 z) { return z * 4 + y * 2 + x; }
    IC u32 _octant(Fvector& base, Fvector& rel)
    {
        u32 o = 0;
        if (rel.x > base.x)
            o += 1;
        if (rel.y > base.y)
            o += 2;
        if (rel.z > base.z)
            o += 4;
        return o;
    }

    ISpatial_NODE* _node_create();
    void _node_destroy(ISpatial_NODE*& P);

    void _insert(ISpatial_NODE* N, Fvector& n_center, float n_radius);
    void _remove(ISpatial_NODE* N, ISpatial_NODE* N_sub);

public:
    ISpatial_DB(pcstr name);
    ~ISpatial_DB();

    // managing
    void initialize(const Fbox& BB);
    // void							destroy			();
    void insert(ISpatial* S);
    void remove(ISpatial* S);
    // B-1: caller already holds unique_lock on query_rw_mutex(); do not re-lock.
    void insert_assuming_locked(ISpatial* S);
    void remove_assuming_locked(ISpatial* S);
    void update(u32 nodes = 8);
    bool verify();

    enum
    {
        O_ONLYFIRST = (1 << 0),
        O_ONLYNEAREST = (1 << 1),
        O_ORDERED = (1 << 2),
        O_force_u32 = u32(-1)
    };

    // query
    void q_ray(xr_vector<ISpatial*>& R, u32 _o, u32 _mask_and, const Fvector& _start, const Fvector& _dir,
        float _range, ISpatial* ignore_spatial = nullptr);
    void q_box(xr_vector<ISpatial*>& R, u32 _o, u32 _mask_or, const Fvector& _center, const Fvector& _size);
    void q_sphere(xr_vector<ISpatial*>& R, u32 _o, u32 _mask_or, const Fvector& _center, const float _radius);
    void q_frustum(xr_vector<ISpatial*>& R, u32 _o, u32 _mask_or, const CFrustum& _frustum);

    // B-1: queries (q_box/q_ray) read each object's spatial.sphere.P under shared_lock on this mutex.
    // Node changes (insert/remove) take it exclusively. Plain position refreshes in SpatialBase::spatial_move
    // deliberately do NOT (a unique_lock per move starved the parallel readers) - a concurrent reader may see
    // a torn/stale sphere for that one move; see the comment in ISpatial.cpp spatial_move().
    std::shared_mutex& query_rw_mutex() { return rw; }
};

enum class SpatialQueryCaller : u8
{
    other = 0,
    physics,
    sound,
    nearest,
    vision,
    actor,
    render,
    ray,
    rain,
    explode,
};

inline thread_local u8 g_spatial_query_caller = 0;

struct SpatialCaller
{
    u8 previous;
    explicit SpatialCaller(SpatialQueryCaller caller) noexcept
        : previous(g_spatial_query_caller)
    {
        g_spatial_query_caller = static_cast<u8>(caller);
    }
    ~SpatialCaller() noexcept { g_spatial_query_caller = previous; }
    SpatialCaller(const SpatialCaller&) = delete;
    SpatialCaller& operator=(const SpatialCaller&) = delete;
};

// Like SpatialCaller, but only claims the tag when nothing more specific is already set (g_spatial_query_caller
// is still the untagged default, 0/"other"). Used by generic utilities (xr_area_raypick.cpp) that are called
// both directly (where "ray" is the right attribution) and from an already-tagged caller such as Rain.cpp's
// "rain" - an unconditional SpatialCaller there would silently overwrite "rain" with "ray" on every call.
struct SpatialCallerDefault
{
    u8 previous;
    bool applied;
    explicit SpatialCallerDefault(SpatialQueryCaller caller) noexcept
        : previous(g_spatial_query_caller), applied(previous == 0)
    {
        if (applied)
            g_spatial_query_caller = static_cast<u8>(caller);
    }
    ~SpatialCallerDefault() noexcept
    {
        if (applied)
            g_spatial_query_caller = previous;
    }
    SpatialCallerDefault(const SpatialCallerDefault&) = delete;
    SpatialCallerDefault& operator=(const SpatialCallerDefault&) = delete;
};

// Shared by SpatialIncQBox/QSphere/QFrustum/QRay: dispatches to one of the 10 `base_*<caller>` ids passed in,
// by name - not by adding `caller` as an offset to a base enum value, so there is no "wrong bucket if the enum
// order ever changes" failure mode.
struct SpatialCallerMetrics
{
    xray::perf::Metric physics, sound, nearest, vision, actor, render, ray, rain, explode, other;
};

inline void SpatialIncCaller(const SpatialCallerMetrics& m) noexcept
{
    switch (g_spatial_query_caller)
    {
    case u8(SpatialQueryCaller::physics): xray::perf::Add(m.physics); break;
    case u8(SpatialQueryCaller::sound): xray::perf::Add(m.sound); break;
    case u8(SpatialQueryCaller::nearest): xray::perf::Add(m.nearest); break;
    case u8(SpatialQueryCaller::vision): xray::perf::Add(m.vision); break;
    case u8(SpatialQueryCaller::actor): xray::perf::Add(m.actor); break;
    case u8(SpatialQueryCaller::render): xray::perf::Add(m.render); break;
    case u8(SpatialQueryCaller::ray): xray::perf::Add(m.ray); break;
    case u8(SpatialQueryCaller::rain): xray::perf::Add(m.rain); break;
    case u8(SpatialQueryCaller::explode): xray::perf::Add(m.explode); break;
    default: xray::perf::Add(m.other); break;
    }
}

// db_index: 0 = SpatialSpace, 1 = SpatialSpacePhysic (see ISpatial_DB's constructor, s_spatial_db_next).
inline void SpatialIncDb(u8 db_index, xray::perf::Metric db0, xray::perf::Metric db1) noexcept
{
    xray::perf::Add(db_index == 0 ? db0 : db1);
}

inline void SpatialIncQBox()
{
    using M = xray::perf::Metric;
    PERF_INC(spatial_q_box);
    SpatialIncCaller({M::spatial_q_box_physics, M::spatial_q_box_sound, M::spatial_q_box_nearest,
        M::spatial_q_box_vision, M::spatial_q_box_actor, M::spatial_q_box_render, M::spatial_q_box_ray,
        M::spatial_q_box_rain, M::spatial_q_box_explode, M::spatial_q_box_other});
}

inline void SpatialIncQSphere()
{
    using M = xray::perf::Metric;
    PERF_INC(spatial_q_sphere);
    SpatialIncCaller({M::spatial_q_sphere_physics, M::spatial_q_sphere_sound, M::spatial_q_sphere_nearest,
        M::spatial_q_sphere_vision, M::spatial_q_sphere_actor, M::spatial_q_sphere_render, M::spatial_q_sphere_ray,
        M::spatial_q_sphere_rain, M::spatial_q_sphere_explode, M::spatial_q_sphere_other});
}

inline void SpatialIncQFrustum()
{
    using M = xray::perf::Metric;
    PERF_INC(spatial_q_frustum);
    SpatialIncCaller({M::spatial_q_frustum_physics, M::spatial_q_frustum_sound, M::spatial_q_frustum_nearest,
        M::spatial_q_frustum_vision, M::spatial_q_frustum_actor, M::spatial_q_frustum_render,
        M::spatial_q_frustum_ray, M::spatial_q_frustum_rain, M::spatial_q_frustum_explode,
        M::spatial_q_frustum_other});
}

inline void SpatialIncQRay()
{
    using M = xray::perf::Metric;
    PERF_INC(spatial_q_ray);
    SpatialIncCaller({M::spatial_q_ray_physics, M::spatial_q_ray_sound, M::spatial_q_ray_nearest,
        M::spatial_q_ray_vision, M::spatial_q_ray_actor, M::spatial_q_ray_render, M::spatial_q_ray_ray,
        M::spatial_q_ray_rain, M::spatial_q_ray_explode, M::spatial_q_ray_other});
}

inline std::shared_lock<std::shared_mutex> SpatialLockShared(std::shared_mutex& rw)
{
    if (rw.try_lock_shared())
        return std::shared_lock<std::shared_mutex>(rw, std::adopt_lock);
    PERF_INC(spatial_lock_shared_contend);
    if (xray::perf::g_perf_metrics_active)
    {
        const u64 t0 = xray::perf::Ticks();
        rw.lock_shared();
        xray::perf::AddTimeSlow(xray::perf::Metric::spatial_lock_shared_wait, xray::perf::Ticks() - t0);
    }
    else
        rw.lock_shared();
    return std::shared_lock<std::shared_mutex>(rw, std::adopt_lock);
}

inline std::unique_lock<std::shared_mutex> SpatialLockUnique(std::shared_mutex& rw)
{
    if (rw.try_lock())
        return std::unique_lock<std::shared_mutex>(rw, std::adopt_lock);
    PERF_INC(spatial_lock_unique_contend);
    if (xray::perf::g_perf_metrics_active)
    {
        const u64 t0 = xray::perf::Ticks();
        rw.lock();
        xray::perf::AddTimeSlow(xray::perf::Metric::spatial_lock_unique_wait, xray::perf::Ticks() - t0);
    }
    else
        rw.lock();
    return std::unique_lock<std::shared_mutex>(rw, std::adopt_lock);
}
