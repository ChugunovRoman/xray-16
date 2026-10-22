#pragma once

// refs
class ENGINE_API IGameObject;

//-----------------------------------------------------------------------------------------------------------
class ENGINE_API IGame_ObjectPool
{
    /*
    private:
    struct str_pred : public std::binary_function<shared_str&, shared_str&, bool>
    {
    IC bool operator()(const shared_str& x, const shared_str& y) const
    { return xr_strcmp(x,y)<0; }
    };
    typedef xr_multimap<shared_str,IGameObject*,str_pred> POOL;
    typedef POOL::iterator POOL_IT;
    private:
    POOL map_POOL;
    */
    using ObjectVec = xr_vector<IGameObject*>;
    ObjectVec m_PrefetchObjects;

public:
    void prefetch();
    void clear();

    IGameObject* create(pcstr name);
    void destroy(IGameObject* O);

    IGame_ObjectPool();
    virtual ~IGame_ObjectPool();
};
//-----------------------------------------------------------------------------------------------------------

struct PrefetchListItem;
struct PrefetchModel;
// 40-parallel-prefetch.md / 50k-slow-disk-early-prefetch.md (plans/optimization_mt): the visuals the Load of these
// prefetch objects creates, each once, in the order of its first section (IGame_ObjectPool.cpp). with_material_visuals:
// also the visuals the Load creates itself (a section with world_material_N lines) - for a file warm-up, which does not
// create them. Main thread. xrEngine only (IGame_ObjectPool::prefetch, the early warm-up of IGame_Persistent).
void PrefetchObjectVisuals(
    const xr_vector<PrefetchListItem>& sections, xr_vector<PrefetchModel>& visuals, bool with_material_visuals);
