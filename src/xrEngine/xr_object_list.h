#ifndef __XR_OBJECT_LIST_H__
#define __XR_OBJECT_LIST_H__

#ifdef DEBUG
extern ENGINE_API BOOL debug_destroy;
#endif

class IGameObject;
class NET_Packet;
struct RelcaseBatch;

class ENGINE_API CObjectList
{
public:
    struct ObjectUpdateStatistics
    {
    public:
        CStatTimer Update;
        u32 Updated;
        u32 Crows;
        u32 Active;
        u32 Total;

        IC void FrameStart()
        {
            Update.FrameStart();
            Updated = 0;
            Crows = 0;
            Active = 0;
            Total = 0;
        }

        IC void FrameEnd() { Update.FrameEnd(); }
    };

private:
    IGameObject* map_NETID[0xffff];
    typedef xr_vector<IGameObject*> Objects;
    Objects destroy_queue;
    Objects objects_active;
    Objects objects_sleeping;
    /**
     * @brief m_primary_crows   - list of items of the primary thread
     * @brief m_secondary_crows - list of items of the secondary thread
     */
    Objects m_primary_crows;
    xr_vector<Objects> m_secondary_crows;
    ObjectUpdateStatistics stats;
    u32 statsFrame;

public:
    typedef fastdelegate::FastDelegate1<IGameObject*> RELCASE_CALLBACK;
    // P17: optional batch form of a relcase callback, same result as m_Callback for every object
    // of the batch. Only for callbacks that just drop pointers (Feel::Vision); see pure_relcase.
    typedef fastdelegate::FastDelegate1<const RelcaseBatch&> RELCASE_BATCH_CALLBACK;
    struct SRelcasePair
    {
        int* m_ID;
        RELCASE_CALLBACK m_Callback;
        RELCASE_BATCH_CALLBACK m_BatchCallback;
        SRelcasePair(int* id, RELCASE_CALLBACK cb) : m_ID(id), m_Callback(cb) {}
        SRelcasePair(int* id, RELCASE_CALLBACK cb, RELCASE_BATCH_CALLBACK batch_cb)
            : m_ID(id), m_Callback(cb), m_BatchCallback(batch_cb) {}
        bool operator==(const RELCASE_CALLBACK& cb) const { return m_Callback == cb; }
    };
    typedef xr_vector<SRelcasePair> RELCASE_CALLBACK_VEC;
    RELCASE_CALLBACK_VEC m_relcase_callbacks;

    void relcase_register(RELCASE_CALLBACK, int*);
    void relcase_register(RELCASE_CALLBACK, RELCASE_BATCH_CALLBACK, int*);
    void relcase_unregister(int*);

    // P17: who asked for an object's destruction - only for the relcase report of perf_metrics (log bit). The first
    // origin noted for an ID sticks until the object is destroyed or an object with this ID is registered again.
    enum : u8
    {
        destroy_origin_unknown = 0,
        destroy_origin_engine, // CGameObject::DestroyObject and other engine code (used up items, ammo, grenades...)
        destroy_origin_lua, // alife():release() of an online object
        destroy_origin_plugin, // alife_release of the Plugin API
        destroy_origin_switch, // ALife switched the object offline
        destroy_origin_server, // any other GE_DESTROY handled by the server
        destroy_origin_count,
        destroy_origin_child = 0x80, // flag: destroyed together with its parent (inventory of a released NPC...)
    };
    void destroy_origin_note(u16 id, u8 origin)
    {
        if (id < 0xffff && !m_destroy_origin[id])
            m_destroy_origin[id] = origin;
    }
    u8 destroy_origin(u16 id) const { return id < 0xffff ? m_destroy_origin[id] : u8(destroy_origin_unknown); }

    // P17: the batch of the running relcase pass of Update(); nullptr outside it (e.g. a direct net_Relcase call).
    const RelcaseBatch* relcase_batch() const { return m_relcase_batch; }
    // P17: true the first time `container` is passed during the running batch - the caller then cleans that container
    // (shared by several objects) of all batch objects at once; false - it was already cleaned in this batch.
    bool relcase_first_visit(const void* container);

public:
    const ObjectUpdateStatistics& GetStats()
    {
        stats.FrameEnd();
        return stats;
    }
    void DumpStatistics(class IGameFont& font, class IPerformanceAlert* alert);
    // methods
    CObjectList();
    ~CObjectList();

    IGameObject* FindObjectByName(shared_str name);
    IGameObject* FindObjectByName(pcstr name);
    IGameObject* FindObjectByCLS_ID(CLASS_ID cls);

    void Load();
    void Unload();

    IGameObject* Create(pcstr name);
    void Destroy(IGameObject* O);

private:
    void SingleUpdate(IGameObject* O);
    void dispatch_singleupdate_workload(IGameObject** b, IGameObject** e);

public:
    void Update(bool bForce);

    void net_Register(IGameObject* O);
    void net_Unregister(IGameObject* O);

    u32 net_Export(NET_Packet* P, u32 _start, u32 _count); // return next start
    void net_Import(NET_Packet* P);

    ICF IGameObject* net_Find(u16 ID) const
    {
        if (ID == u16(-1))
            return (0);

        return (map_NETID[ID]);
    }

    void o_crow(IGameObject* O);
    bool o_remove(Objects& v, IGameObject* O); // returns false if O was not present (caller must not push elsewhere)
    void o_activate(IGameObject* O);
    void o_sleep(IGameObject* O);

    [[nodiscard]]
    IC u32 o_count() const
    {
        return u32(objects_active.size() + objects_sleeping.size());
    }

    IC IGameObject* o_get_by_iterator(u32 _it)
    {
        if (_it < objects_active.size())
            return objects_active[_it];
        else
            return objects_sleeping[_it - objects_active.size()];
    }
    bool dump_all_objects();

public:
    void register_object_to_destroy(IGameObject* object_to_destroy);
#ifdef DEBUG
    bool registered_object_to_destroy(const IGameObject* object_to_destroy) const;
#endif // #ifdef DEBUG

private:
    IC Objects& get_crows();

    // P17: the destroy phase of Update(), batched relcase of the destroy_queue
    void process_destroy_queue();
    void relcase_objects_batch(const RelcaseBatch& batch);
    void relcase_callbacks_batch(const RelcaseBatch& batch);

    u8 m_destroy_origin[0xffff];
    const RelcaseBatch* m_relcase_batch = nullptr;
    xr_vector<IGameObject*> m_relcase_objects; // copy of the destroy_queue part being relcased (it may grow meanwhile)
    xr_vector<const IGameObject*> m_relcase_sorted;
    u8 m_relcase_id_marks[0x10000];
    xr_vector<SRelcasePair> m_relcase_callbacks_snapshot; // P17b: reused by relcase_callbacks_batch (no allocation)
    xr_vector<const void*> m_relcase_visited;
    const void* m_relcase_visited_recent[8];
    u32 m_relcase_visited_recent_pos = 0;

    static void clear_crow_vec(Objects& o);
    static void dump_list(Objects& v, pcstr reason);
};

#endif //__XR_OBJECT_LIST_H__
