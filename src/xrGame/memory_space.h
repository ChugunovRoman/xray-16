////////////////////////////////////////////////////////////////////////////
//	Module 		: memory_space.h
//	Created 	: 25.12.2003
//  Modified 	: 25.12.2003
//	Author		: Dmitriy Iassenev
//	Description : Memory space
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "ai_sounds.h"
#include "xrServer_Space.h"
#include "alife_space.h"
#include "xrCore/_flags.h"
#include "xrCommon/misc_math_types.h"

//#define USE_GAME_TIME
#define USE_LEVEL_TIME

#ifdef USE_GAME_TIME
#define USE_LAST_GAME_TIME
#endif
#ifdef USE_LEVEL_TIME
#define USE_LAST_LEVEL_TIME
#endif

#ifdef USE_GAME_TIME
#define USE_FIRST_GAME_TIME
#endif
#ifdef USE_LEVEL_TIME
//#	define USE_FIRST_LEVEL_TIME
#endif

//#define USE_UPDATE_COUNT

//#define USE_ORIENTATION

#define USE_STALKER_VISION_FOR_MONSTERS

class CEntityAlive;
class CGameObject;

namespace MemorySpace
{
typedef u64 squad_mask_type;

struct CNotYetVisibleObject
{
    const CGameObject* m_object;
    float m_value;
    u32 m_update_time;
    u32 m_prev_time;
    // 45-engine-ai-audit.md, F3: the ID of m_object (u16(-1) for none), written together with it - the list is
    // searched by ID for every object an NPC sees, and reading the ID through each pointer cost a call and a cache
    // miss per element. Sits in the tail padding, the size does not change
    u16 m_object_id = u16(-1);
};

struct SObjectParams
{
    u32 m_level_vertex_id;
    Fvector m_position;
#ifdef USE_ORIENTATION
    SRotation m_orientation;
#endif
};

template <typename T>
struct CObjectParams : public SObjectParams
{
    IC SRotation orientation(const T* object) const;
    IC void fill(const T* game_object);
};

struct SMemoryObject
{
#ifdef USE_GAME_TIME
    ALife::_TIME_ID m_game_time;
#endif
#ifdef USE_LEVEL_TIME
    u32 m_level_time;
#endif
#ifdef USE_LAST_GAME_TIME
    ALife::_TIME_ID m_last_game_time;
#endif
#ifdef USE_LAST_LEVEL_TIME
    u32 m_last_level_time;
#endif
#ifdef USE_FIRST_GAME_TIME
    ALife::_TIME_ID m_first_game_time;
#endif
#ifdef USE_FIRST_LEVEL_TIME
    u32 m_first_level_time;
#endif
#ifdef USE_UPDATE_COUNT
    u32 m_update_count;
#endif
    bool m_enabled;
    // 45-engine-ai-audit.md, F3: the ID of CMemoryObject::m_object (u16(-1) for none), kept by CMemoryObject::set_object
    // and fill - the memory lists are searched by ID (operator==) on every visual/sound/hit update, and reading the ID
    // through each stored pointer cost a call (object_id, SEH) and a cache miss per element. In the padding after
    // m_enabled, the size does not change
    u16 m_object_id;

    SMemoryObject()
        :
#ifdef USE_GAME_TIME
          m_game_time(0),
#endif
#ifdef USE_LEVEL_TIME
          m_level_time(0),
#endif
#ifdef USE_LAST_GAME_TIME
          m_last_game_time(0),
#endif
#ifdef USE_LAST_LEVEL_TIME
          m_last_level_time(0),
#endif
#ifdef USE_FIRST_GAME_TIME
          m_first_game_time(0),
#endif
#ifdef USE_FIRST_LEVEL_TIME
          m_first_level_time(0),
#endif
#ifdef USE_UPDATE_COUNT
          m_update_count(0),
#endif
          m_enabled(true), m_object_id(u16(-1))
    {
    }

    IC void fill() { m_enabled = true; }
};

template <typename T>
struct CMemoryObject : public SMemoryObject
{
    const T* m_object;
    CObjectParams<T> m_object_params;
    CObjectParams<T> m_self_params;
    _flags<squad_mask_type> m_squad_mask;

    IC CMemoryObject();
    IC bool operator==(u16 id) const;
    // m_object and m_object_id together: every write of m_object goes through here or fill
    IC void set_object(const T* object);
    IC void fill(const T* game_object, const T* self, const squad_mask_type& mask);
};

struct CVisibleObject : CMemoryObject<CGameObject>
{
    typedef CMemoryObject<CGameObject> inherited;
    _flags<squad_mask_type> m_visible;

public:
    IC CVisibleObject() { m_visible.zero(); }
    [[nodiscard]]
    IC bool visible(const squad_mask_type& mask) const { return (!!m_visible.test(mask)); }
    IC void visible(const squad_mask_type& mask, bool value) { m_visible.set(mask, value); }
    IC void fill(const CGameObject* game_object, const CGameObject* self, const squad_mask_type& mask,
        const squad_mask_type& visibility_mask)
    {
        inherited::fill(game_object, self, mask);
        m_visible.set(visibility_mask, TRUE);
    }
};

struct CHitObject : public CMemoryObject<CEntityAlive>
{
    Fvector m_direction;
    u16 m_bone_index;
    float m_amount;
};

struct CSoundObject : public CMemoryObject<CGameObject>
{
    ESoundTypes m_sound_type;
    float m_power;

    IC void fill(const CGameObject* game_object, const CGameObject* self, const ESoundTypes sound_type,
        const float sound_power, const squad_mask_type& mask)
    {
        CMemoryObject<CGameObject>::fill(game_object, self, mask);
        m_sound_type = sound_type;
        m_power = sound_power;
    }

    IC int sound_type() const { return (int(m_sound_type)); }
};

struct CMemoryInfo : public CVisibleObject
{
    bool m_visual_info;
    bool m_sound_info;
    bool m_hit_info;

    CMemoryInfo()
    {
        m_visual_info = false;
        m_sound_info = false;
        m_hit_info = false;
    }

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

template <typename T>
struct SLevelTimePredicate
{
    bool operator()(const CMemoryObject<T>& object1, const CMemoryObject<T>& object2) const
    {
        return (object1.m_level_time < object2.m_level_time);
    }
};
};

using namespace MemorySpace;
