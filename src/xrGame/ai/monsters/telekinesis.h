#pragma once
#include "telekinetic_object.h"
#include "xrPhysics/PHUpdateObject.h"

#include <mutex>

class CTelekinesis : public CPHUpdateObject
{
protected:
    using TELE_OBJECTS = xr_vector<CTelekineticObject*>;
    using TELE_OBJECTS_IT = TELE_OBJECTS::iterator;
    TELE_OBJECTS objects;
    xr_vector<IGameObject*> m_nearest;
    bool active;
    // B6 early physics kick (plans/optimization_mt/09-physics-earlier-parallel.md): `objects` and the telekinetic
    // objects' states are walked by the physics step (PhTune / PhDataUpdate on the physics thread) while the Game
    // thread updates them (schedule_update, activate / deactivate from monster and anomaly logic) - every entry point
    // takes this lock. Recursive: the public methods call each other (fire_all -> deactivate, remove_object, ...).
    // The body mutations done under it from the Game thread are queued by xrPhysics (PHStepDefer.h), so the step
    // thread never waits for anything the Game thread holds.
    std::recursive_mutex m_objects_lock;

public:
    CTelekinesis();
    virtual ~CTelekinesis();

    // allocates relevant TelekineticObject

    // активировать объект
    virtual CTelekineticObject* activate(
        CPhysicsShellHolder* obj, float strength, float height, u32 max_time_keep, bool rot = true);

    // деактивировать все объекты
    void deactivate();

    // clear objects (does not call release, but call switch to TS_None)
    void clear_deactivate();
    // clear
    virtual void clear();
    virtual void clear_notrelevant();
    // деактивировать объект
    void deactivate(CPhysicsShellHolder* obj);
    void remove_object(TELE_OBJECTS_IT it);
    void remove_object(CPhysicsShellHolder* obj);
    // бросить все объекты в позицию 'target'
    void fire_all(const Fvector& target);

    // бросить объект 'obj' в позицию 'target' с учетом коэф силы
    void fire(CPhysicsShellHolder* obj, const Fvector& target, float power);

    // бросить объект 'obj' в позицию 'target' с учетом коэф силы
    void fire_t(CPhysicsShellHolder* obj, const Fvector& target, float time);

    // вернуть активность телекинеза
    bool is_active() const { return active; }
    // вернуть активность объекта
    bool is_active_object(CPhysicsShellHolder* obj);

    // вернуть количество контролируемых объектов (в состоянии TS_Raise & TS_Keep)
    u32 get_objects_count();

    // вернуть количество контролируемых объектов (всех)
    u32 get_objects_total_count()
    {
        std::lock_guard<std::recursive_mutex> guard(m_objects_lock);
        return objects.size();
    }
    // вернуть объект по индексу в массиве
    // a	copy of the object!
    CTelekineticObject get_object_by_index(u32 index)
    {
        std::lock_guard<std::recursive_mutex> guard(m_objects_lock);
        // The physics step may drop an object (PhTune -> clear_notrelevant) between the caller's
        // get_objects_total_count() and this call: an empty object (TS_None) is returned then.
        if (index >= objects.size())
            return CTelekineticObject();
        return *objects[index];
    }

    // обновить состоняие на shedule_Update
    void schedule_update();

    // объект был удален - удалить все связи на объект
    void remove_links(IGameObject* O);

protected:
    virtual CTelekineticObject* alloc_tele_object() { return xr_new<CTelekineticObject>(); }
private:
    // обновление на шагах физики
    virtual void PhDataUpdate(float step);
    virtual void PhTune(float step);
};
