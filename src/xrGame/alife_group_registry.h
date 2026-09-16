////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_group_registry.h
//	Created 	: 28.10.2005
//  Modified 	: 28.10.2005
//	Author		: Dmitriy Iassenev
//	Description : ALife group registry
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "alife_space.h"

class CSE_ALifeOnlineOfflineGroup;
class CSE_ALifeDynamicObject;

class CALifeGroupRegistry
{
public:
    typedef CSE_ALifeOnlineOfflineGroup OBJECT;
    typedef xr_map<ALife::_OBJECT_ID, OBJECT*> OBJECTS;

protected:
    OBJECTS m_objects;

public:
    virtual ~CALifeGroupRegistry();
    void add(CSE_ALifeDynamicObject* object);
    void remove(CSE_ALifeDynamicObject* object);
    OBJECT& object(const ALife::_OBJECT_ID& id) const;
    // The reference-returning overload above dereferences end() in release builds (its VERIFY is
    // compiled out) whenever the id is unknown - and a member's m_group_id can outlive its squad.
    // This overload reports the miss instead: nullptr, no undefined behaviour.
    OBJECT* object(const ALife::_OBJECT_ID& id, bool no_assert) const;
    IC const OBJECTS& objects() const;
    void on_after_game_load();
};

#include "alife_group_registry_inline.h"
