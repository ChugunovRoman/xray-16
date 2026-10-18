// ModelPool.h: interface for the CModelPool class.
//////////////////////////////////////////////////////////////////////
#pragma once

#include <string_view>
#include <unordered_map>

namespace xray::render::RENDER_NAMESPACE
{
// refs
class dxRender_Visual;
class CKinematicsAnimated;
namespace PS
{
struct SEmitter;
};

// 40-parallel-prefetch.md (plans/optimization_mt): one model of a prefetch list - the name (lower case, no extension)
// and the suffix, as Create(name, suffix) takes them
struct ModelPrefetchItem
{
    xr_string name;
    xr_string suffix;
};

// defs
class ECORE_API CModelPool
{
    friend class CRender;

    struct str_pred
    {
        bool operator()(const shared_str& x, const shared_str& y) const { return xr_strcmp(x, y) < 0; }
    };
    struct ModelDef
    {
        shared_str name;
        dxRender_Visual* model;
        u32 refs;
        ModelDef()
        {
            refs = 0;
            model = nullptr;
        }
    };

    typedef xr_multimap<shared_str, dxRender_Visual*, str_pred> POOL;
    typedef POOL::iterator POOL_IT;
    typedef xr_map<dxRender_Visual*, shared_str> REGISTRY;
    typedef REGISTRY::iterator REGISTRY_IT;

    xr_vector<ModelDef> Models; // Reference / Base
    // 36-spawn-model-load.md: name -> base model of Models (the view points into the entry's shared_str), read by
    // Instance_Find
    std::unordered_map<std::string_view, dxRender_Visual*> ModelsIndex;
    // Deferred Delete() calls (GAME_RENDER_OVERLAP_PLAN P1.6): visual + the bDiscard it was called
    // with, so DeleteQueue() can replay it faithfully instead of always defaulting to FALSE.
    xr_vector<std::pair<dxRender_Visual*, BOOL>> ModelsToDelete;
    REGISTRY Registry; // Just pairing of pointer / Name
    POOL Pool; // Unused / Inactive
    BOOL bLogging;
    BOOL bForceDiscard;

    void Destroy();
    void IndexErase(const shared_str& name, const dxRender_Visual* model); // 36-spawn-model-load.md
    // 40-parallel-prefetch.md: Prefetch / PrefetchList
    void PrefetchItems(const xr_vector<ModelPrefetchItem>& items, pcstr label, bool sequential_too);

public:
    CModelPool();
    virtual ~CModelPool();
    dxRender_Visual* Instance_Create(u32 Type);
    dxRender_Visual* Instance_Duplicate(dxRender_Visual* V);
    dxRender_Visual* Instance_Load(LPCSTR N, BOOL allow_register);
    dxRender_Visual* Instance_Load(LPCSTR N, LPCSTR suffix, BOOL allow_register);
    dxRender_Visual* Instance_Load(LPCSTR N, IReader* data, BOOL allow_register);
    dxRender_Visual* Instance_Load(LPCSTR N, LPCSTR suffix, IReader* data, BOOL allow_register);
    void Instance_Register(LPCSTR N, dxRender_Visual* V);
    dxRender_Visual* Instance_Find(LPCSTR N);

    dxRender_Visual* CreatePE(PS::CPEDef* source);
    dxRender_Visual* CreatePG(PS::CPGDef* source);
    dxRender_Visual* Create(LPCSTR name, IReader* data = nullptr);
    dxRender_Visual* Create(LPCSTR name, LPCSTR suffix, IReader* data = nullptr);
    dxRender_Visual* CreateChild(LPCSTR name, IReader* data);
    dxRender_Visual* CreateChild(LPCSTR name, LPCSTR suffix, IReader* data);
    void Delete(dxRender_Visual*& V, BOOL bDiscard = FALSE);
    void Discard(dxRender_Visual*& V, BOOL b_complete);
    void DeleteInternal(dxRender_Visual*& V, BOOL bDiscard = FALSE);
    void DeleteQueue();

    void Logging(BOOL bEnable) { bLogging = bEnable; }
    void Prefetch();
    // 40-parallel-prefetch.md: IRender::models_PrefetchList (name, suffix)
    void PrefetchList(const xr_vector<std::pair<shared_str, shared_str>>& visuals);
    void ClearPool(BOOL b_complete);
    // 36-spawn-model-load.md (the model prefetch): IRender::model_Preload / model_PrefetchFiles
    bool Preload(LPCSTR name);
    void PrefetchFiles(LPCSTR name, u32 source);

    void dump();

    void memory_stats(u32& vb_mem_video, u32& vb_mem_system, u32& ib_mem_video, u32& ib_mem_system);
#ifdef _EDITOR
    void OnDeviceDestroy();
    void Render(dxRender_Visual* m_pVisual, const Fmatrix& mTransform, int priority, bool strictB2F, float m_fLOD);
    void RenderSingle(dxRender_Visual* m_pVisual, const Fmatrix& mTransform, float m_fLOD);
#endif
};

// While one is alive (on any thread), CModelPool::Delete queues the visual for the next DeleteQueue
// (ModelDeferredClear) exactly as inside the game/render overlap window. For code that runs game callbacks after the
// window closed and goes on using the models they may release (CKinematicsAnimated::DispatchDeferredBlendEvents).
struct ModelPoolDeferDeletes
{
    ModelPoolDeferDeletes();
    ~ModelPoolDeferDeletes();
    ModelPoolDeferDeletes(const ModelPoolDeferDeletes&) = delete;
    ModelPoolDeferDeletes& operator=(const ModelPoolDeferDeletes&) = delete;
};

// 36-spawn-model-load.md: true while this thread is inside a cold model load (CModelPool::Create loading an ogf, its
// LOD and children included)
bool ModelPoolColdLoad();

// 40-parallel-prefetch.md (plans/optimization_mt): the motions part of CKinematicsAnimated::Load (LoadMotions) -
// g_pMotionsContainer, the motions_value shared by models (the first model to dock an omf maps its bones,
// CPartition::load of each model rewrites the shared partition) and their reference counts. One such part at a time;
// on a parallel CModelPool::Prefetch also in the order of the list, so the motions come out exactly as with the
// sequential prefetch: the parts of a load thread's load (the model of the list, its LOD) are deferred
// (ModelMotionsDefer) to the motions chain, which runs them in the order of the list and, inside a load, in the order
// the sequential load ran them; a part not deferred (none on a load thread) waits until the parts before it have run.
struct ModelMotionsScope
{
    ModelMotionsScope();
    ~ModelMotionsScope();
    ModelMotionsScope(const ModelMotionsScope&) = delete;
    ModelMotionsScope& operator=(const ModelMotionsScope&) = delete;
};

// 40-parallel-prefetch.md: CKinematicsAnimated::Load on a load thread of the parallel prefetch: true = the motions part
// (LoadMotions(N, data)) will run later on the motions chain (on `data`, left open, or on a copy of it)
bool ModelMotionsDefer(CKinematicsAnimated* K, pcstr N, IReader* data);
} // namespace xray::render::RENDER_NAMESPACE

// 36-spawn-model-load.md: a Tracy zone and a perf_metrics timer (modelpool/load/*) of one part of a cold model load.
// Both are active only inside such a load: the same code loads the level geometry and the models of the prefetch list.
// Needs xrCore/Profiling/PerfMetrics.hpp.
#define MODEL_LOAD_PART(zone, metric)                                                                                  \
    const bool CONCATENIZE(_mlp_on_, __LINE__) = ModelPoolColdLoad();                                                 \
    (void)CONCATENIZE(_mlp_on_, __LINE__);                                                                             \
    ZoneNamedN(CONCATENIZE(_mlp_zone_, __LINE__), zone, CONCATENIZE(_mlp_on_, __LINE__));                             \
    PERF_SCOPE_IF(CONCATENIZE(_mlp_on_, __LINE__), metric)
