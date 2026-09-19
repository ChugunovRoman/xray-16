////////////////////////////////////////////////////////////////////////////
//	Module 		: group_hierarchy_holder.cpp
//	Created 	: 12.11.2001
//  Modified 	: 03.09.2004
//	Author		: Dmitriy Iassenev, Oles Shishkovtsov, Aleksandr Maksimchuk
//	Description : Group hierarchy holder
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "group_hierarchy_holder.h"
#include "squad_hierarchy_holder.h"
#include "Entity.h"
#include <algorithm>
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "agent_memory_manager.h"
#include "ai/stalker/ai_stalker.h"
#include "memory_manager.h"
#include "visual_memory_manager.h"
#include "sound_memory_manager.h"
#include "hit_memory_manager.h"
#include <cstdint>

#if defined(XR_PLATFORM_WINDOWS) && defined(_MSC_VER)
#include <Windows.h>
#endif

namespace
{
IC bool is_invalid_entity_ptr(const CEntity* entity)
{
    if (!entity)
        return true;

    const std::uintptr_t p = reinterpret_cast<std::uintptr_t>(entity);
    return p == ~std::uintptr_t(0) || p == 0xFFFFFFFFFFFFFFFFull;
}

#if defined(XR_PLATFORM_WINDOWS) && defined(_MSC_VER)
// Freed heap often stays in a no-access or decommitted guard region; VirtualQuery avoids touching it.
static bool ptr_first_page_readable(const void* ptr, size_t need_bytes)
{
    if (!ptr || need_bytes == 0)
        return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;

    const DWORD prot = mbi.Protect & ~(PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE);
    const bool read_ok = (prot & PAGE_READONLY) || (prot & PAGE_READWRITE) || (prot & PAGE_WRITECOPY) ||
        (prot & PAGE_EXECUTE_READ) || (prot & PAGE_EXECUTE_READWRITE) || (prot & PAGE_EXECUTE_WRITECOPY);
    if (!read_ok)
        return false;

    const auto* region_base = reinterpret_cast<const std::uint8_t*>(mbi.BaseAddress);
    const auto* p = reinterpret_cast<const std::uint8_t*>(ptr);
    const size_t offset = static_cast<size_t>(p - region_base);
    return offset + need_bytes <= mbi.RegionSize;
}

// /O2 can outline the virtual call to ID() outside the __try; disable optimizations in this TU region.
#pragma optimize("g", off)
__declspec(noinline) static bool is_entity_alive_safe_impl(const CEntity* entity)
{
    if (!ptr_first_page_readable(entity, sizeof(void*) * 8))
        return false;

    __try
    {
        const u16 id = entity->ID();
        IGameObject* reg = Level().Objects.net_Find(id);
        if (reg != entity)
            return false;
        return !!entity->g_Alive();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
#pragma optimize("", on)

#pragma optimize("g", off)
__declspec(noinline) static bool entity_ids_equal_safe_impl(const CEntity* a, const CEntity* b)
{
    if (!ptr_first_page_readable(a, sizeof(void*)) || !ptr_first_page_readable(b, sizeof(void*)))
        return false;

    __try
    {
        return a->ID() == b->ID();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
#pragma optimize("", on)
#endif // XR_PLATFORM_WINDOWS && _MSC_VER

IC bool is_entity_alive_safe(const CEntity* entity)
{
    if (is_invalid_entity_ptr(entity))
        return false;

#if defined(XR_PLATFORM_WINDOWS) && defined(_MSC_VER)
    return is_entity_alive_safe_impl(entity);
#else
    const u16 id = entity->ID();
    if (Level().Objects.net_Find(id) != entity)
        return false;
    return !!entity->g_Alive();
#endif
}

IC bool entity_ids_equal_safe(const CEntity* a, const CEntity* b)
{
    if (!a || !b || is_invalid_entity_ptr(a) || is_invalid_entity_ptr(b))
        return false;

#if defined(XR_PLATFORM_WINDOWS) && defined(_MSC_VER)
    return entity_ids_equal_safe_impl(a, b);
#else
    return a->ID() == b->ID();
#endif
}

// The registry can desync once and then every frame; log the first occurrences verbatim, then
// one in 512, so a recurrence stays visible without flooding the log.
bool log_rare_group(u32& counter)
{
    ++counter;
    return (counter <= 16) || ((counter % 512) == 0);
}
} // namespace

CGroupHierarchyHolder::~CGroupHierarchyHolder()
{
    VERIFY(m_members.empty());
    VERIFY(!m_agent_manager);

    // The group owns the senses buffers for its whole life - see unregister_in_agent_manager.
    xr_delete(m_visible_objects);
    xr_delete(m_sound_objects);
    xr_delete(m_hit_objects);
}

void CGroupHierarchyHolder::lazy_ensure_agent_manager_for_stalker(CAI_Stalker* stalker)
{
    if (!stalker || m_agent_manager)
        return;

    const auto it = std::find(m_members.begin(), m_members.end(), static_cast<CEntity*>(stalker));
    if (it == m_members.end())
    {
        register_member(stalker);
        return;
    }

    if (!m_visible_objects)
    {
        R_ASSERT2(!m_members.empty(), "lazy_ensure: missing squad sense buffers but member list empty");
        m_visible_objects = xr_new<VISIBLE_OBJECTS>();
        m_sound_objects = xr_new<SOUND_OBJECTS>();
        m_hit_objects = xr_new<HIT_OBJECTS>();
    }

    m_agent_manager = xr_new<CAgentManager>();
    agent_manager().memory().set_squad_objects(&visible_objects());
    agent_manager().memory().set_squad_objects(&sound_objects());
    agent_manager().memory().set_squad_objects(&hit_objects());

    for (CEntity* entity : m_members)
    {
        if (smart_cast<CAI_Stalker*>(entity))
            agent_manager().member().add(entity);
    }
}

#ifdef SQUAD_HIERARCHY_HOLDER_USE_LEADER
void CGroupHierarchyHolder::update_leader()
{
    m_leader = 0;
    for (CEntity* entity : m_members)
    {
        if (is_entity_alive_safe(entity))
        {
            m_leader = entity;
            break;
        }
    }
}
#endif // SQUAD_HIERARCHY_HOLDER_USE_LEADER

void CGroupHierarchyHolder::register_in_group(CEntity* member)
{
    VERIFY(member);
    // VERIFY3 is compiled out in release, where a double registration used to put the same member
    // into the registry twice - the second unregister then dropped somebody else.
    if (std::find(m_members.begin(), m_members.end(), member) != m_members.end())
    {
        static u32 s_reported = 0;
        if (log_rare_group(s_reported))
            Msg("! [GW] register_in_group: [%s] is already registered in this group, ignored",
                member->cName().c_str());
        return;
    }

    // The senses buffers belong to the group and every member keeps a raw pointer to them
    // (register_in_group_senses). They are created once and live as long as the holder does -
    // see unregister_in_agent_manager and ~CGroupHierarchyHolder.
    if (!m_visible_objects)
    {
        m_visible_objects = xr_new<VISIBLE_OBJECTS>();
        m_sound_objects = xr_new<SOUND_OBJECTS>();
        m_hit_objects = xr_new<HIT_OBJECTS>();

        //		m_visible_objects->reserve	(128);
        //		m_sound_objects->reserve	(128);
        //		m_hit_objects->reserve		(128);
    }

    m_members.push_back(member);
}

void CGroupHierarchyHolder::register_in_squad(CEntity* member)
{
#ifdef SQUAD_HIERARCHY_HOLDER_USE_LEADER
    if (!leader() && is_entity_alive_safe(member))
    {
        m_leader = member;
        if (!squad().leader())
            squad().leader(member);
    }
#endif // SQUAD_HIERARCHY_HOLDER_USE_LEADER
}

void CGroupHierarchyHolder::register_in_agent_manager(CEntity* member)
{
    if (!get_agent_manager() && smart_cast<CAI_Stalker*>(member))
    {
        m_agent_manager = xr_new<CAgentManager>();
        agent_manager().memory().set_squad_objects(&visible_objects());
        agent_manager().memory().set_squad_objects(&sound_objects());
        agent_manager().memory().set_squad_objects(&hit_objects());
    }

    if (get_agent_manager())
        agent_manager().member().add(member);
}

void CGroupHierarchyHolder::register_in_group_senses(CEntity* member)
{
    CCustomMonster* monster = smart_cast<CCustomMonster*>(member);
    if (monster)
    {
        monster->memory().visual().set_squad_objects(&visible_objects());
        monster->memory().sound().set_squad_objects(&sound_objects());
        monster->memory().hit().set_squad_objects(&hit_objects());
    }
}

void CGroupHierarchyHolder::unregister_in_group(CEntity* member)
{
    VERIFY(member);
    MEMBER_REGISTRY::iterator I = std::find(m_members.begin(), m_members.end(), member);
    // In release VERIFY3 is a no-op and erase(end()) used to drop the LAST member of the registry
    // instead - a live NPC silently disappeared from the group while still pointing at its buffers.
    if (I == m_members.end())
    {
        static u32 s_reported = 0;
        if (log_rare_group(s_reported))
            Msg("! [GW] unregister_in_group: [%s] is not registered in this group, ignored",
                member->cName().c_str());
        return;
    }

    m_members.erase(I);
}

void CGroupHierarchyHolder::unregister_in_squad(CEntity* member)
{
#ifdef SQUAD_HIERARCHY_HOLDER_USE_LEADER
    if (!member || is_invalid_entity_ptr(member))
        return;

    const CEntity* currentLeader = leader();
    if (entity_ids_equal_safe(currentLeader, member))
    {
        update_leader();
        CEntity* squadLeader = squad().leader();
        if (entity_ids_equal_safe(squadLeader, member))
        {
            if (leader())
                squad().leader(leader());
            else
                squad().update_leader();
        }
    }
#endif // SQUAD_HIERARCHY_HOLDER_USE_LEADER
}

void CGroupHierarchyHolder::unregister_in_agent_manager(CEntity* member)
{
    if (get_agent_manager())
    {
        agent_manager().member().remove(member);
        if (agent_manager().member().members().empty())
            xr_delete(m_agent_manager);
    }

    // The buffers are NOT freed here any more. Every member holds a raw pointer to them and there
    // is no way to prove from here that nobody kept one (net_Import rewrites team/squad/group
    // without re-registering, so a member can end up unregistering in a different group than the
    // one it is wired to). Emptying them keeps the memory bounded - one set per team/squad/group
    // triple - while a stale pointer now finds a valid empty vector instead of freed memory.
    if (m_members.empty())
    {
        if (m_visible_objects)
            m_visible_objects->clear();
        if (m_sound_objects)
            m_sound_objects->clear();
        if (m_hit_objects)
            m_hit_objects->clear();
    }
}

void CGroupHierarchyHolder::unregister_in_group_senses(CEntity* member)
{
    CCustomMonster* monster = smart_cast<CCustomMonster*>(member);
    if (monster)
    {
        monster->memory().visual().set_squad_objects(0);
        monster->memory().sound().set_squad_objects(0);
        monster->memory().hit().set_squad_objects(0);
    }
}

void CGroupHierarchyHolder::register_member(CEntity* member)
{
    register_in_group(member);
    register_in_squad(member);
    register_in_agent_manager(member);
    register_in_group_senses(member);
}

void CGroupHierarchyHolder::unregister_member(CEntity* member)
{
    unregister_in_group(member);
    unregister_in_squad(member);
    unregister_in_agent_manager(member);
    unregister_in_group_senses(member);
}
