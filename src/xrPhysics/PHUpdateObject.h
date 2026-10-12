#pragma once
#include "PHItemList.h"
#include "xrPhysics.h"

class CPhysicsShell;
class XRPHYSICS_API CPHUpdateObject
{
    DECLARE_PHLIST_ITEM(CPHUpdateObject)
    bool b_activated;

public:
    CPHUpdateObject();
    virtual ~CPHUpdateObject() { DeactivateOnDestroy(); }
    void Activate();
    void Deactivate();
    void DeactivateOnDestroy(); // the destructor's Deactivate: never queues (see PHObject.cpp)
    IC bool IsActive() { return b_activated; }
    virtual void PhDataUpdate(float step) = 0;
    virtual void PhTune(float step) = 0;
    virtual void NetRelcase(CPhysicsShell* s){};
};
DEFINE_PHITEM_LIST(CPHUpdateObject, PH_UPDATE_OBJECT_STORAGE, PH_UPDATE_OBJECT_I)
