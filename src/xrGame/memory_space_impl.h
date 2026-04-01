////////////////////////////////////////////////////////////////////////////
//	Module 		: memory_space_impl.h
//	Created 	: 25.05.2004
//  Modified 	: 25.05.2004
//	Author		: Dmitriy Iassenev
//	Description : Memory space implementation
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "memory_space.h"
#include "GameObject.h"
#include "Level.h"
#include "ai_space.h"
#include "xrAICore/Navigation/ai_object_location.h"
#include "xrAICore/Navigation/ai_object_location_impl.h"
#include "xrAICore/Navigation/level_graph.h"
#include <cstdint>

template <typename T>
IC SRotation CObjectParams<T>::orientation(const T* object) const
{
    Fvector t;
    object->XFORM().getHPB(t.x, t.y, t.z);
    return (SRotation(t.x, t.y, 0.f));
}

template <typename T>
IC void CObjectParams<T>::fill(const T* game_object)
{
#ifdef USE_ORIENTATION
    m_orientation = game_object ? orientation(game_object) : SRotation(0.f, 0.f, 0.f);
#endif

    m_level_vertex_id = game_object ? game_object->ai_location().level_vertex_id() : u32(-1);
    //	if (game_object && ai().get_level_graph() && ai().level_graph().valid_vertex_id(m_level_vertex_id) &&
    //! ai().level_graph().inside(m_level_vertex_id,game_object->Position())) {
    //		m_position			= ai().level_graph().vertex_position(m_level_vertex_id);
    //		m_position.y		= game_object->Position().y;
    //		return;
    //	}

    if (game_object)
    {
        game_object->Center(m_position);
        m_position.set(game_object->Position().x, m_position.y, game_object->Position().z);
    }
    else
        m_position = Fvector().set(0.f, 0.f, 0.f);
}

template <typename T>
IC CMemoryObject<T>::CMemoryObject()
{
    m_squad_mask.one();
    m_object = 0;
}

template <typename T>
IC bool CMemoryObject<T>::operator==(u16 id) const
{
    return (object_id(m_object) == id);
}

template <typename T>
IC void CMemoryObject<T>::fill(const T* game_object, const T* self, const squad_mask_type& mask)
{
#ifdef USE_UPDATE_COUNT
    ++m_update_count;
#endif
#ifdef USE_LAST_GAME_TIME
    m_last_game_time = m_game_time;
#endif
#ifdef USE_LAST_LEVEL_TIME
    m_last_level_time = m_level_time;
#endif
#ifdef USE_GAME_TIME
    m_game_time = Level().GetGameTime();
#endif
#ifdef USE_LEVEL_TIME
    m_level_time = Device.dwTimeGlobal;
#endif
    m_object = game_object;
    m_object_params.fill(game_object);
    m_self_params.fill(self);
    m_squad_mask.assign(mask);
    SMemoryObject::fill();
}

template <typename T>
IC u16 object_id(const T* object)
{
    if (!object)
        return u16(0xffff);

#if defined(XR_PLATFORM_WINDOWS)
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(object);
    if (address == ~std::uintptr_t(0) || address == 0xFFFFFFFFFFFF0000ull)
        return u16(0xffff);

    __try
    {
        return u16(object->ID());
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return u16(0xffff);
    }
#else
    return u16(object->ID());
#endif
}
