////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_object_registry.cpp
//	Created 	: 15.01.2003
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife object registry
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "alife_object_registry.h"
#include "ai_debug.h"
#include "xrServerEntities/xrMessages.h"
#include <algorithm>

CALifeObjectRegistry::CALifeObjectRegistry(LPCSTR section) {}
CALifeObjectRegistry::~CALifeObjectRegistry()
{
    OBJECT_REGISTRY::iterator const B = m_objects.begin();
    OBJECT_REGISTRY::iterator I = B;
    OBJECT_REGISTRY::iterator const E = m_objects.end();
    for (; I != E; ++I)
        (*I).second->on_unregister();

    for (I = B; I != E; ++I)
        xr_delete((*I).second);
}

void CALifeObjectRegistry::save(IWriter& memory_stream, CSE_ALifeDynamicObject* object, u32& object_count)
{
    ++object_count;

    NET_Packet tNetPacket;
    // Spawn
    object->Spawn_Write(tNetPacket, TRUE);
    memory_stream.w_u16(u16(tNetPacket.B.count));
    memory_stream.w(tNetPacket.B.data, tNetPacket.B.count);

    // Update
    tNetPacket.w_begin(M_UPDATE);
    object->UPDATE_Write(tNetPacket);

    memory_stream.w_u16(u16(tNetPacket.B.count));
    memory_stream.w(tNetPacket.B.data, tNetPacket.B.count);

    ALife::OBJECT_VECTOR::const_iterator I = object->children.begin();
    ALife::OBJECT_VECTOR::const_iterator E = object->children.end();
    for (; I != E; ++I)
    {
        CSE_ALifeDynamicObject* child = this->object(*I, true);
        if (!child)
            continue;

        if (!child->can_save())
            continue;

        save(memory_stream, child, object_count);
    }
}

void CALifeObjectRegistry::save(IWriter& memory_stream)
{
    Msg("* Saving objects...");
    memory_stream.open_chunk(OBJECT_CHUNK_DATA);

    u32 position = memory_stream.tell();
    memory_stream.w_u32(u32(-1));

    u32 object_count = 0;
    OBJECT_REGISTRY::iterator I = m_objects.begin();
    OBJECT_REGISTRY::iterator E = m_objects.end();
    for (; I != E; ++I)
    {
        if (!(*I).second->can_save())
            continue;

        if ((*I).second->redundant())
            continue;

        if ((*I).second->ID_Parent != 0xffff)
            continue;

        save(memory_stream, (*I).second, object_count);
    }

    u32 last_position = memory_stream.tell();
    memory_stream.seek(position);
    memory_stream.w_u32(object_count);
    memory_stream.seek(last_position);

    memory_stream.close_chunk();

    Msg("* %d objects are successfully saved", object_count);
}

CSE_ALifeDynamicObject* CALifeObjectRegistry::get_object(IReader& file_stream)
{
    NET_Packet tNetPacket;
    u16 u_id;
    // Spawn
    tNetPacket.B.count = file_stream.r_u16();
    file_stream.r(tNetPacket.B.data, tNetPacket.B.count);
    tNetPacket.r_begin(u_id);
    R_ASSERT2(M_SPAWN == u_id, "Invalid packet ID (!= M_SPAWN)");

    string64 s_name;
    tNetPacket.r_stringZ(s_name);
    // create entity
    CSE_Abstract* tpSE_Abstract = F_entity_Create(s_name);
    R_ASSERT2(tpSE_Abstract, "Can't create entity.");
    CSE_ALifeDynamicObject* tpALifeDynamicObject = smart_cast<CSE_ALifeDynamicObject*>(tpSE_Abstract);
    R_ASSERT2(tpALifeDynamicObject, "Non-ALife object in the saved game!");
    tpALifeDynamicObject->Spawn_Read(tNetPacket);

    // Update
    tNetPacket.B.count = file_stream.r_u16();
    file_stream.r(tNetPacket.B.data, tNetPacket.B.count);
    tNetPacket.r_begin(u_id);
    R_ASSERT2(M_UPDATE == u_id, "Invalid packet ID (!= M_UPDATE)");
    tpALifeDynamicObject->UPDATE_Read(tNetPacket);

    return (tpALifeDynamicObject);
}

bool CALifeObjectRegistry::was_skipped_on_load(ALife::_OBJECT_ID id) const
{
    return std::find(m_skipped_on_load.begin(), m_skipped_on_load.end(), id) != m_skipped_on_load.end();
}

// 09-content-addons-vfs.md section 8 (C8), 00-roadmap.md section 5a (decision 6): a saved object whose section is gone
// (its addon is switched off) or whose class is not registered (a script class of such an addon) is skipped with a log
// line instead of the fatal error of r_clsid. Each object is two length-prefixed packets (save above), so a skipped
// one leaves the stream in step. The objects inside it (its inventory: a child is saved right after its parent) are
// skipped too - their parent would be a dangling id for register_object. The rest of the references are fixed in
// CALifeStorageManager::load
CSE_ALifeDynamicObject* CALifeObjectRegistry::load_object(IReader& file_stream, u32& unknown_count, u32& inside_count)
{
    NET_Packet tNetPacket;
    u16 u_id;
    // Spawn
    tNetPacket.B.count = file_stream.r_u16();
    file_stream.r(tNetPacket.B.data, tNetPacket.B.count);
    tNetPacket.r_begin(u_id);
    R_ASSERT2(M_SPAWN == u_id, "Invalid packet ID (!= M_SPAWN)");

    // the head of CSE_Abstract::Spawn_Write: section, name, game id (u8), RP (u8), position, angle, respawn time (u16),
    // id, parent id. Spawn_Read starts the packet again (r_begin)
    string256 section;
    tNetPacket.r_stringZ_s(section);
    string256 name;
    tNetPacket.r_stringZ_s(name);
    tNetPacket.r_advance(2 * sizeof(u8) + 2 * sizeof(Fvector) + sizeof(u16));
    const ALife::_OBJECT_ID id = tNetPacket.r_u16();
    const ALife::_OBJECT_ID parent_id = tNetPacket.r_u16();

    CSE_Abstract* tpSE_Abstract = nullptr;
    const bool inside_skipped = parent_id != ALife::_OBJECT_ID(-1) && was_skipped_on_load(parent_id);
    const bool section_known = pSettings->section_exist(section) && pSettings->line_exist(section, "class");
    if (!inside_skipped && section_known)
        tpSE_Abstract = F_entity_Create(section, true);

    if (!tpSE_Abstract)
    {
        // nothing works without the actor: the old fatal error, with the section
        R_ASSERT3(id != 0, "The actor of the saved game has an unknown section or class", section);

        if (inside_skipped)
        {
            ++inside_count;
            Msg("~ [save] object %u '%s' of section '%s' skipped: it is inside the skipped object %u", u32(id), name,
                section, u32(parent_id));
        }
        else
        {
            ++unknown_count;
            if (section_known)
                Msg("~ [save] object %u '%s' of section '%s' with an unknown class skipped", u32(id), name, section);
            else
                Msg("~ [save] object %u '%s' of unknown section '%s' skipped", u32(id), name, section);
        }

        m_skipped_on_load.push_back(id);
        m_skipped_sections_on_load.push_back(section);
        // Update
        const u16 update_size = file_stream.r_u16();
        file_stream.advance(update_size);
        return nullptr;
    }

    CSE_ALifeDynamicObject* tpALifeDynamicObject = smart_cast<CSE_ALifeDynamicObject*>(tpSE_Abstract);
    R_ASSERT2(tpALifeDynamicObject, "Non-ALife object in the saved game!");
    tpALifeDynamicObject->Spawn_Read(tNetPacket);

    // Update
    tNetPacket.B.count = file_stream.r_u16();
    file_stream.r(tNetPacket.B.data, tNetPacket.B.count);
    tNetPacket.r_begin(u_id);
    R_ASSERT2(M_UPDATE == u_id, "Invalid packet ID (!= M_UPDATE)");
    tpALifeDynamicObject->UPDATE_Read(tNetPacket);

    return (tpALifeDynamicObject);
}

void CALifeObjectRegistry::load(IReader& file_stream)
{
    Msg("* Loading objects...");
    R_ASSERT2(file_stream.find_chunk(OBJECT_CHUNK_DATA), "Can't find chunk OBJECT_CHUNK_DATA!");

    m_objects.clear();
    m_skipped_on_load.clear();
    m_skipped_sections_on_load.clear();

    const u32 count = file_stream.r_u32();
    u32 unknown_count = 0;
    u32 inside_count = 0;
    for (u32 i = 0; i < count; ++i)
    {
        if (CSE_ALifeDynamicObject* object = load_object(file_stream, unknown_count, inside_count))
            add(object);
    }

    // C8: a guard for a parent missing from the stream for any other reason (a child always follows its parent, the
    // loop above catches the objects inside the skipped ones). Not registered yet: only the map and the object
    if (!m_skipped_on_load.empty())
    {
        for (bool changed = true; changed;)
        {
            changed = false;
            for (OBJECT_REGISTRY::iterator I = m_objects.begin(); I != m_objects.end(); ++I)
            {
                CSE_Abstract* object = (*I).second;
                if (object->ID_Parent == ALife::_OBJECT_ID(-1) || m_objects.find(object->ID_Parent) != m_objects.end())
                    continue;

                ++inside_count;
                Msg("~ [save] object %u '%s' of section '%s' skipped: its parent %u is not loaded", u32(object->ID),
                    object->name_replace(), object->s_name.c_str(), u32(object->ID_Parent));
                m_skipped_on_load.push_back(object->ID);
                m_skipped_sections_on_load.push_back(object->s_name);
                m_objects.erase(I);
                F_entity_Destroy(object);
                changed = true;
                break;
            }
        }

        Msg("~ [save] %u of %u saved objects skipped: %u of unknown sections or classes, %u inside them",
            u32(m_skipped_on_load.size()), count, unknown_count, inside_count);
    }

    Msg("* %d objects are successfully loaded", u32(m_objects.size()));
}
