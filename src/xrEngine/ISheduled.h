#pragma once

#include "Engine.h"
#include "Common/Noncopyable.hpp"

class SchedulerData
{
public:
    u32 t_min : 14; // minimal bound of update time (sample: 20ms)
    u32 t_max : 14; // maximal bound of update time (sample: 200ms)
    u32 b_RT : 1;
    u32 b_locked : 1;
#ifdef DEBUG
    u32 dbg_startframe;
    u32 dbg_update_shedule;
#endif
};

class XR_NOVTABLE ISheduled
{
public:
    virtual ~ISheduled() = 0;
    virtual SchedulerData& GetSchedulerData() = 0;
    virtual float shedule_Scale() const = 0;
    virtual void shedule_Update(u32 dt) = 0;
    // XXX nitrocaster: return (const char *) to reduce string pool spoiling
    virtual shared_str shedule_Name() const = 0;
    virtual bool shedule_Needed() = 0;
    // 22-game-thread.md, step 4d (the urgent near heap, xrSheduler.cpp): the object has to react now (an NPC in a fight,
    // hit recently) - in the near class of the scheduler it goes before the calm near objects. Only the order changes,
    // never the interval. Called on the thread that runs the non-RT scheduler pass, between the updates (the requeue
    // after its own update and the round-robin scan of the near heap): must be cheap and read only the state of the
    // game logic thread. Asked only of the living stalkers and monsters
    virtual bool shedule_Urgent() const { return false; }
};

inline ISheduled::~ISheduled() = default;

class ENGINE_API XR_NOVTABLE ScheduledBase : public virtual ISheduled, Noncopyable
{
public:
    SchedulerData shedule;

    ScheduledBase();
    virtual ~ScheduledBase();

    virtual SchedulerData& GetSchedulerData() override { return shedule; }
    virtual void shedule_Update(u32 dt) override;
    virtual shared_str shedule_Name() const override { return shared_str("unknown"); }

protected:
    virtual void shedule_register();
    virtual void shedule_unregister();
};
