////////////////////////////////////////////////////////////////////////////
//	Module 		: base_monster_misc.cpp
//	Created 	: 26.05.2003
//  Modified 	: 26.05.2003
//	Author		: Serge Zhem
//	Description : Miscellanious functions for all the biting monsters
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "base_monster.h"
#include "EntityCondition.h"
#include "xrEngine/xrSheduler.h"

////////////////////////////////////////////////////////////////////////////////////////////////////////
// Входные воздействия
// Зрение, слух, вероятность победы, выгодность противника
void CBaseMonster::UpdateMemory()
{
    // Обновить память
    EnemyMemory.update();
    SoundMemory.UpdateHearing();
    CorpseMemory.update();
    HitMemory.update();

    // обновить менеджеры врагов и трупов
    EnemyMan.update();
    CorpseMan.update();

    // remove hit info from objects that are corpses

    hear_dangerous_sound = hear_interesting_sound = false;
    SoundElem se;

    if (SoundMemory.IsRememberSound())
    {
        SoundMemory.GetSound(se, hear_dangerous_sound);
        hear_interesting_sound = !hear_dangerous_sound;
    }

    // Setup is own additional flags
    m_bDamaged = ((conditions().GetHealth() < db().m_fDamagedThreshold) ? true : false);

    m_bAggressive = hear_dangerous_sound || (EnemyMan.get_enemies_count() > 0) || HitMemory.is_hit();
}

// 22-game-thread.md, step 4d (urgent near heap, xrEngine/xrSheduler.cpp): the monster has to react now - an enemy is
// selected or it was hit within shed_urgent_hit_ms ms. Asked by the scheduler on the thread of its non-RT pass, between
// the updates, only while alive and near: EnemyMan and HitMemory change in UpdateMemory (this monster's Think, that same
// thread) and in Hit (the events of FrameMove on the main thread, before the Game thread is woken), see
// CAI_Stalker::shedule_Urgent. HitMemory keeps a few hits (time_memory), the scan of them is short
bool CBaseMonster::shedule_Urgent() const
{
    if (!g_Alive())
        return false;
    if (EnemyMan.get_enemy())
        return true;
    if (!HitMemory.is_hit())
        return false;
    const TTime hit_time = HitMemory.get_last_hit_time();
    return hit_time != 0 && Device.dwTimeGlobal - hit_time <= u32(_max(0, ps_shed_urgent_hit_ms));
}
