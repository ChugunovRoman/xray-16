////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_object_registry.h
//	Created 	: 15.01.2003
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife object registry
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "xrServer_Objects_ALife.h"
#include "xrEngine/profiler.h"

class CALifeObjectRegistry
{
public:
    typedef xr_map<ALife::_OBJECT_ID, CSE_ALifeDynamicObject*> OBJECT_REGISTRY;

protected:
    OBJECT_REGISTRY m_objects;
    // 09-content-addons-vfs.md section 8 (C8): the ids of the saved objects load() could not create (a section of a
    // switched off addon) and of the objects inside them; they are in no registry, see CALifeStorageManager::load
    xr_vector<ALife::_OBJECT_ID> m_skipped_on_load;
    // their sections, by the index of m_skipped_on_load (Lua alife_save.skipped_objects)
    xr_vector<shared_str> m_skipped_sections_on_load;

private:
    void save(IWriter& memory_stream, CSE_ALifeDynamicObject* object, u32& object_count);
    // load(): one saved object, nullptr (the stream is past it) when it is skipped
    CSE_ALifeDynamicObject* load_object(IReader& file_stream, u32& unknown_count, u32& inside_count);
    bool was_skipped_on_load(ALife::_OBJECT_ID id) const;

public:
    static CSE_ALifeDynamicObject* get_object(IReader& file_stream);
    const xr_vector<ALife::_OBJECT_ID>& skipped_on_load() const { return m_skipped_on_load; }
    const xr_vector<shared_str>& skipped_sections_on_load() const { return m_skipped_sections_on_load; }

public:
    CALifeObjectRegistry(LPCSTR section);
    virtual ~CALifeObjectRegistry();
    virtual void save(IWriter& memory_stream);
    void load(IReader& file_stream);
    IC void add(CSE_ALifeDynamicObject* object);
    IC void remove(const ALife::_OBJECT_ID& id, bool no_assert = false);
    IC CSE_ALifeDynamicObject* object(const ALife::_OBJECT_ID& id, bool no_assert = false) const;
    IC const OBJECT_REGISTRY& objects() const;
    IC OBJECT_REGISTRY& objects();
};

#include "alife_object_registry_inline.h"
