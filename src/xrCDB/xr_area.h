#pragma once
#include "xr_collide_defs.h"
#include "Common/Noncopyable.hpp"
#include "Include/xrRender/FactoryPtr.h"
#include "Include/xrRender/ObjectSpaceRender.h"
#include "xrXRC.h"
#include "xrCDB.h"
#include "xrCore/_fbox.h"

// fwd. decl.
class ISpatial;
class ISpatial_DB;
class ICollisionForm;
class IGameObject;
class Lock;

//-----------------------------------------------------------------------------------------------------------
// Space Area
//-----------------------------------------------------------------------------------------------------------
#ifdef _MANAGED
class CObjectSpaceData
{
    // You should not try to create CObjectSpace in the managed environment.
    CObjectSpaceData() = delete;
};
#else
struct CObjectSpaceData
{
    thread_local static xrXRC xrc;
    thread_local static collide::rq_results r_temp;
    thread_local static xr_vector<ISpatial*> r_spatial;
};
#endif

struct hdrCFORM;
class XRCDB_API CObjectSpace : protected CObjectSpaceData, public Noncopyable
{
    ISpatial_DB* SpatialSpace{};
    CDB::MODEL Static;
    Fbox m_BoundingVolume;

public:
#ifdef DEBUG
    FactoryPtr<IObjectSpaceRender>* m_pRender{};
#endif

private:
    bool _RayTest(const Fvector& start, const Fvector& dir, float range, collide::rq_target tgt,
        collide::ray_cache* cache, IGameObject* ignore_object);
    bool _RayPick(const Fvector& start, const Fvector& dir, float range, collide::rq_target tgt, collide::rq_result& R,
        IGameObject* ignore_object);
    bool _RayQuery(collide::rq_results& dest, const collide::ray_defs& rq, collide::rq_callback* cb, LPVOID user_data,
        collide::test_callback* tb, IGameObject* ignore_object);
    bool _RayQuery2(collide::rq_results& dest, const collide::ray_defs& rq, collide::rq_callback* cb, LPVOID user_data,
        collide::test_callback* tb, IGameObject* ignore_object);
    bool _RayQuery3(collide::rq_results& dest, const collide::ray_defs& rq, collide::rq_callback* cb, LPVOID user_data,
        collide::test_callback* tb, IGameObject* ignore_object);

    // Spatial query cache for GetNearest (collideable objects). Validated via externally-supplied callback.
    // Callback takes an object id and returns a validated pointer, or nullptr if destroyed.
    using validate_object_fn = IGameObject*(*)(u16 id);
    struct SNearestCacheKey
    {
        s32 x, y, z;
        u32 r;
        bool operator<(const SNearestCacheKey& other) const
        {
            if (x != other.x) return x < other.x;
            if (y != other.y) return y < other.y;
            if (z != other.z) return z < other.z;
            return r < other.r;
        }
    };
    struct SNearestCacheEntry
    {
        u32 frame;
        xr_vector<u16> object_ids;
    };

    validate_object_fn m_validate_object{};
    xr_map<SNearestCacheKey, SNearestCacheEntry> m_nearest_cache;
    u32 m_nearest_cache_frame{};

    static SNearestCacheKey make_cache_key(const Fvector& point, float range);

public:
    // Runtime-tunable spatial query cache settings (registered as CCC vars in xrEngine).
    // Written from console thread, read from main thread. On x86-64 aligned int/float
    // reads are atomic at hardware level; CCC_Float/CCC_Integer require plain pointers.
    static float ps_obj_nearest_cache_quant;
    static int ps_obj_nearest_cache_ttl;
    static int ps_obj_nearest_cache_max_entries;
    CObjectSpace(ISpatial_DB* spatialSpace);
    ~CObjectSpace();

    void Load  (CDB::build_callback build_callback,
                CDB::serialize_callback serialize_callback,
                CDB::deserialize_callback deserialize_callback,
                CDB::remapping_materials_callback remapping_materials_callback);

    void Load  (LPCSTR path, LPCSTR fname, CDB::build_callback build_callback,
                CDB::serialize_callback serialize_callback,
                CDB::deserialize_callback deserialize_callback,
                CDB::remapping_materials_callback remapping_materials_callback);

    void Load  (IReader* R, CDB::build_callback build_callback,
                CDB::serialize_callback serialize_callback,
                CDB::deserialize_callback deserialize_callback,
                CDB::remapping_materials_callback remapping_materials_callback);

    void Create(Fvector* verts, CDB::TRI* tris, const hdrCFORM& H,
                CDB::build_callback build_callback,
                CDB::serialize_callback serialize_callback,
                CDB::deserialize_callback deserialize_callback,
                CDB::remapping_materials_callback remapping_materials_callback,
                IReader* cacheStream = nullptr);

    // Occluded/No
    bool RayTest(const Fvector& start, const Fvector& dir, float range, collide::rq_target tgt,
        collide::ray_cache* cache, IGameObject* ignore_object);

    // Game raypick (nearest) - returns object and addititional params
    bool RayPick(const Fvector& start, const Fvector& dir, float range, collide::rq_target tgt, collide::rq_result& R,
        IGameObject* ignore_object);

    /** Parallel RayPick when TaskScheduler is active; each result must point to distinct storage (e.g. array slot). */
    struct RayPickBatchItem
    {
        Fvector start;
        Fvector dir;
        float range;
        collide::rq_target tgt;
        IGameObject* ignore_object;
        collide::rq_result* result{};
    };
    void RayPickBatch(const RayPickBatchItem* items, size_t count);

    // General collision query
    bool RayQuery(collide::rq_results& dest, const collide::ray_defs& rq, collide::rq_callback* cb, LPVOID user_data,
        collide::test_callback* tb, IGameObject* ignore_object);
    bool RayQuery(collide::rq_results& dest, ICollisionForm* target, const collide::ray_defs& rq);

    bool BoxQuery(Fvector const& box_center, Fvector const& box_z_axis, Fvector const& box_y_axis,
        Fvector const& box_sizes, xr_vector<Fvector>* out_tris);

    int GetNearest(xr_vector<IGameObject*>& q_nearest, ICollisionForm* obj, float range);
    int GetNearest(xr_vector<IGameObject*>& q_nearest, const Fvector& point, float range, IGameObject* ignore_object);
    int GetNearest(xr_vector<ISpatial*>& q_spatial, xr_vector<IGameObject*>& q_nearest, const Fvector& point,
        float range, IGameObject* ignore_object);

    void SetValidateObjectCallback(validate_object_fn fn) { m_validate_object = fn; }
    void NextCacheFrame();

    CDB::TRI* GetStaticTris() { return Static.get_tris(); }
    Fvector* GetStaticVerts() { return Static.get_verts(); }
    CDB::MODEL* GetStaticModel() { return &Static; }
    const Fbox& GetBoundingVolume() const { return m_BoundingVolume; }
// Debugging
#ifdef DEBUG
    void dbgRender();
// ref_shader							dbgGetShader		()	{ return sh_debug;	}
#endif
    void DumpStatistics(IGameFont& font, IPerformanceAlert* alert);
};
