////////////////////////////////////////////////////////////////////////////
//	Module 		: agent_enemy_manager.cpp
//	Created 	: 24.05.2004
//  Modified 	: 14.01.2005
//	Author		: Dmitriy Iassenev
//	Description : Agent enemy manager
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "agent_enemy_manager.h"
#include "agent_manager.h"
#include "agent_memory_manager.h"
#include "agent_member_manager.h"
#include "ai_space.h"
#include "ef_storage.h"
#include "ef_pattern.h"
#include "member_order.h"
#include "ai/stalker/ai_stalker.h"

#include "memory_manager.h"
#include "visual_memory_manager.h"
#include "sound_memory_manager.h"
#include "hit_memory_manager.h"
#include "enemy_manager.h"
#include "memory_space_impl.h"

extern const float wounded_enemy_reached_distance;

const u32 __c0 = 0x55555555;
const u32 __c1 = 0x33333333;
const u32 __c2 = 0x0f0f0f0f;
const u32 __c3 = 0x00ff00ff;
const u32 __c4 = 0x0000003f;

IC u32 population(const u32& b)
{
    u32 a = b;
    a = (a & __c0) + ((a >> 1) & __c0);
    a = (a & __c1) + ((a >> 2) & __c1);
    a = (a + (a >> 4)) & __c2;
    a = (a + (a >> 8)) & __c3;
    a = (a + (a >> 16)) & __c4;
    return (a);
}

IC u32 population(const u64& b) { return (population((u32)b) + population(u32(b >> 32))); }
struct CEnemyFiller
{
    typedef CAgentEnemyManager::ENEMIES ENEMIES;
    ENEMIES* m_enemies;
    squad_mask_type m_mask;

    IC CEnemyFiller(ENEMIES* enemies, squad_mask_type mask)
    {
        m_enemies = enemies;
        m_mask = mask;
    }

    IC void operator()(const CEntityAlive* enemy) const
    {
        ENEMIES::iterator I = std::find(m_enemies->begin(), m_enemies->end(), enemy);
        if (I == m_enemies->end())
        {
            m_enemies->push_back(CMemberEnemy(enemy, m_mask));
            return;
        }

        (*I).m_mask.set(m_mask, TRUE);
    }
};

IC xr_vector<ALife::_OBJECT_ID> combat_member_ids(const CAgentMemberManager::MEMBER_STORAGE& members)
{
    xr_vector<ALife::_OBJECT_ID> result;
    result.reserve(members.size());

    CAgentMemberManager::MEMBER_STORAGE::const_iterator I = members.begin();
    CAgentMemberManager::MEMBER_STORAGE::const_iterator E = members.end();
    for (; I != E; ++I)
    {
        if (!*I || !(*I)->m_object)
            continue;

        result.push_back((*I)->m_object->ID());
    }

    return result;
}

IC u32 combat_member_mask_index(squad_mask_type mask)
{
    VERIFY(mask);

    u32 result = 0;
    while (mask > squad_mask_type(1))
    {
        mask >>= 1;
        ++result;
    }

    return result;
}

IC void build_combat_member_cache(CAgentMemberManager& member_manager, xr_vector<CMemberOrder*>& ordered_members,
    xr_vector<CMemberOrder*>& members_by_mask_index)
{
    ordered_members.clear();
    members_by_mask_index.clear();
    members_by_mask_index.resize(member_manager.members().size(), 0);

    const xr_vector<ALife::_OBJECT_ID> member_ids = combat_member_ids(member_manager.combat_members());
    xr_vector<ALife::_OBJECT_ID>::const_iterator I = member_ids.begin();
    xr_vector<ALife::_OBJECT_ID>::const_iterator E = member_ids.end();
    for (; I != E; ++I)
    {
        CMemberOrder* member_order = member_manager.get_member(*I);
        if (!member_order)
            continue;

        ordered_members.push_back(member_order);

        const u32 mask_index = combat_member_mask_index(member_manager.mask(*I));
        if (mask_index < members_by_mask_index.size())
            members_by_mask_index[mask_index] = member_order;
    }
}

IC CMemberOrder* combat_member_by_mask(const xr_vector<CMemberOrder*>& members_by_mask_index, squad_mask_type mask)
{
    if (!mask)
        return 0;

    const u32 mask_index = combat_member_mask_index(mask);
    if (mask_index >= members_by_mask_index.size())
        return 0;

    return members_by_mask_index[mask_index];
}

void CAgentEnemyManager::fill_enemies()
{
    m_enemies.clear();

    {
        const xr_vector<ALife::_OBJECT_ID> member_ids = combat_member_ids(object().member().combat_members());
        xr_vector<ALife::_OBJECT_ID>::const_iterator I = member_ids.begin();
        xr_vector<ALife::_OBJECT_ID>::const_iterator E = member_ids.end();
        for (; I != E; ++I)
        {
            CMemberOrder* member_order = object().member().get_member(*I);
            if (!member_order)
                continue;

            const CAI_Stalker* stalker = smart_cast<const CAI_Stalker*>(member_order->m_object);
            if (!stalker || !stalker->NameObject.c_str())
                continue;

            member_order->probability(1.f);
            member_order->object().memory().fill_enemies(
                CEnemyFiller(&m_enemies, object().member().mask(&member_order->object())));
        }
    }

    if (m_enemies.empty())
        return;

    VERIFY(!m_enemies.empty());

    {
        for (int i = 0, n = (int)m_wounded.size(); i < n; ++i)
        {
            const CEntityAlive* enemy = m_wounded[i].first;
            ENEMIES::const_iterator I = std::find(m_enemies.begin(), m_enemies.end(), enemy);
            if (I != m_enemies.end())
                continue;

            m_wounded.erase(m_wounded.begin() + i);
            --i;
            --n;
        }
    }

    m_only_wounded_left = true;
    m_is_any_wounded = false;
    {
        CAgentMemoryManager& memory = object().memory();
        ENEMIES::iterator I = enemies().begin();
        ENEMIES::iterator E = enemies().end();
        for (; I != E; ++I)
        {
            if (m_only_wounded_left)
            {
                const CAI_Stalker* stalker = smart_cast<const CAI_Stalker*>((*I).m_object);
                if (!stalker || !stalker->wounded())
                    m_only_wounded_left = false;
                else
                    m_is_any_wounded = true;
            }
            else
            {
                if (!m_is_any_wounded)
                {
                    const CAI_Stalker* stalker = smart_cast<const CAI_Stalker*>((*I).m_object);
                    if (stalker && stalker->wounded())
                        m_is_any_wounded = true;
                }
            }

            memory.object_information((*I).m_object, (*I).m_level_time, (*I).m_enemy_position);
        }
    }

    if (!m_only_wounded_left && m_is_any_wounded)
    {
        enemies().erase(std::remove_if(enemies().begin(), enemies().end(), [](const CMemberEnemy& enemy)
        {
            const CAI_Stalker* stalker = smart_cast<const CAI_Stalker*>(enemy.m_object);
            return stalker && stalker->wounded();
        }), enemies().end());
    }

    VERIFY(!m_enemies.empty());
}

float CAgentEnemyManager::evaluate(const CEntityAlive* object0, const CEntityAlive* object1) const
{
    if (!object0 || !object1 || !object0->g_Alive() || !object1->g_Alive())
        return 0.f;

    ai().ef_storage().non_alife().clear();
    ai().ef_storage().alife().clear();
    ai().ef_storage().non_alife().member() = object0;
    ai().ef_storage().non_alife().enemy() = object1;
    return (ai().ef_storage().m_pfVictoryProbability->ffGetValue() / 100.f);
}

void CAgentEnemyManager::exchange_enemies(CMemberOrder& member0, CMemberOrder& member1)
{
    u32 enemy0 = member0.selected_enemy();
    u32 enemy1 = member1.selected_enemy();
    squad_mask_type mask0 = object().member().mask(&member0.object());
    squad_mask_type mask1 = object().member().mask(&member1.object());
    m_enemies[enemy0].m_distribute_mask.set(mask0, FALSE);
    m_enemies[enemy1].m_distribute_mask.set(mask1, FALSE);
    m_enemies[enemy0].m_distribute_mask.set(mask1, TRUE);
    m_enemies[enemy1].m_distribute_mask.set(mask0, TRUE);
    member0.selected_enemy(enemy1);
    member1.selected_enemy(enemy0);
}

void CAgentEnemyManager::compute_enemy_danger()
{
    ENEMIES::iterator I = m_enemies.begin();
    ENEMIES::iterator E = m_enemies.end();
    for (; I != E; ++I)
    {
        float best = -1.f;
        const xr_vector<ALife::_OBJECT_ID> member_ids = combat_member_ids(object().member().combat_members());
        xr_vector<ALife::_OBJECT_ID>::const_iterator i = member_ids.begin();
        xr_vector<ALife::_OBJECT_ID>::const_iterator e = member_ids.end();
        for (; i != e; ++i)
        {
            CMemberOrder* member_order = object().member().get_member(*i);
            if (!member_order)
                continue;

            float value = evaluate((*I).m_object, &member_order->object());
            if (value > best)
                best = value;
        }
        (*I).m_probability = best;
    }

    std::sort(m_enemies.begin(), m_enemies.end());
}

void CAgentEnemyManager::assign_enemies()
{
    for (;;)
    {
        squad_mask_type J, K, N = 0;
        float best = flt_max;

        ENEMIES::iterator I = m_enemies.begin();
        ENEMIES::iterator E = m_enemies.end();
        for (; I != E; ++I)
        {
            J = (*I).m_mask.get();
            N = 0;
            best = -1.f;
            for (; J; J &= J - 1)
            {
                K = (J & (J - 1)) ^ J;
                CAgentMemberManager::iterator i = object().member().member(K);
                if (!fsimilar((*i)->probability(), 1.f))
                    continue;

                float value = evaluate(&(*i)->object(), (*I).m_object);
                if (value > best)
                {
                    best = value;
                    N = K;
                }
            }
            if (N)
                break;
        }
        if (!N)
            break;

        (*I).m_distribute_mask.set(N, TRUE);
        CAgentMemberManager::iterator i = object().member().member(N);
        (*i)->probability(best);
        (*I).m_probability *= 1.f - best;

        // recovering sort order
        for (u32 i = 0, n = m_enemies.size() - 1; i < n; ++i)
            if (m_enemies[i + 1] < m_enemies[i])
                std::swap(m_enemies[i], m_enemies[i + 1]);
            else
                break;
    }
}

void CAgentEnemyManager::permutate_enemies()
{
    xr_vector<CMemberOrder*> ordered_members;
    xr_vector<CMemberOrder*> members_by_mask_index;
    build_combat_member_cache(object().member(), ordered_members, members_by_mask_index);

    // filling member enemies
    xr_vector<CMemberOrder*>::iterator I = ordered_members.begin();
    xr_vector<CMemberOrder*>::iterator E = ordered_members.end();
    for (; I != E; ++I)
    {
        CMemberOrder* member_order = *I;
        if (!member_order)
            continue;

        // clear enemies
        member_order->enemies().clear();
        // setup procesed flag
        member_order->processed(false);
        // get member squad mask
        squad_mask_type member_mask = object().member().mask(&member_order->object());
        // setup if player has enemy
        bool enemy_selected = false;
        // iterate on enemies
        ENEMIES::const_iterator i = m_enemies.begin(), b = i;
        ENEMIES::const_iterator e = m_enemies.end();
        for (; i != e; ++i)
        {
            if ((*i).m_mask.is(member_mask))
                member_order->enemies().push_back(u32(i - b));

            if ((*i).m_distribute_mask.is(member_mask))
            {
                member_order->selected_enemy(u32(i - b));
                enemy_selected = true;
            }
        }
        // if there is enemy - all is ok
        if (enemy_selected)
            continue;

        // otherwise temporary make the member processed
        member_order->processed(true);
    }

    // perform permutations
    bool changed;
    u32 permutation_pass_count = 0;
    const u32 max_permutation_pass_count = _max<u32>(1, u32(ordered_members.size() * m_enemies.size()));
    do
    {
        changed = false;
        xr_vector<CMemberOrder*>::iterator I = ordered_members.begin();
        xr_vector<CMemberOrder*>::iterator E = ordered_members.end();
        for (; I != E; ++I)
        {
            CMemberOrder* member_order = *I;
            if (!member_order)
                continue;

            // if member is processed the continue;
            if (member_order->processed())
                continue;

            float best =
                member_order->object().Position().distance_to_sqr(m_enemies[member_order->selected_enemy()].m_object->Position());
            bool found = false;
            xr_vector<u32>::const_iterator i = member_order->enemies().begin();
            xr_vector<u32>::const_iterator e = member_order->enemies().end();
            for (; i != e; ++i)
            {
                if (member_order->selected_enemy() == *i)
                    continue;
                float my_distance = member_order->object().Position().distance_to_sqr(m_enemies[*i].m_object->Position());
                if (my_distance < best)
                {
                    // check if we can exchange enemies
                    squad_mask_type J = m_enemies[*i].m_distribute_mask.get(), K;
                    // iterating on members, whose current enemy is the new one
                    for (; J; J &= J - 1)
                    {
                        K = (J & (J - 1)) ^ J;
                        CMemberOrder* enemy_owner = combat_member_by_mask(members_by_mask_index, K);
                        if (!enemy_owner)
                            continue;

                        xr_vector<u32>::iterator ii =
                            std::find(enemy_owner->enemies().begin(), enemy_owner->enemies().end(), member_order->selected_enemy());
                        // check if member can my current enemy
                        if (ii == enemy_owner->enemies().end())
                            continue;

                        // check if I'm closer to the enemy
                        float member_distance =
                            enemy_owner->object().Position().distance_to_sqr(m_enemies[*i].m_object->Position());
                        if (member_distance <= my_distance)
                            continue;

                        // check if our effectiveness is near the same
                        float my_to_his = evaluate(&member_order->object(), m_enemies[enemy_owner->selected_enemy()].m_object);
                        float his_to_my = evaluate(&enemy_owner->object(), m_enemies[member_order->selected_enemy()].m_object);
                        if (!fsimilar(my_to_his, enemy_owner->probability()) || !fsimilar(his_to_my, member_order->probability()))
                            continue;

                        exchange_enemies(*member_order, *enemy_owner);

                        found = true;
                        best = my_distance;
                        break;
                    }
                }

                if (found)
                    break;
            }

            if (!found)
            {
                member_order->processed(true);
                continue;
            }

            changed = true;
        }
        ++permutation_pass_count;
    } while (changed && (permutation_pass_count < max_permutation_pass_count));

    VERIFY(!m_enemies.empty());
    if (!m_only_wounded_left)
    {
        xr_vector<CMemberOrder*>::iterator I = ordered_members.begin();
        xr_vector<CMemberOrder*>::iterator E = ordered_members.end();
        for (; I != E; ++I)
        {
            CMemberOrder* member_order = *I;
            if (!member_order)
                continue;

            CVisualMemoryManager& visual = member_order->object().memory().visual();
            CHitMemoryManager& hit = member_order->object().memory().hit();
            const squad_mask_type member_mask = object().member().mask(&member_order->object());
            ENEMIES::iterator i = m_enemies.begin();
            ENEMIES::iterator e = m_enemies.end();
            for (; i != e; ++i)
            {
                if (visual.visible_now((*i).m_object))
                {
                    (*i).m_distribute_mask.assign((*i).m_distribute_mask.get() | member_mask);
                    continue;
                }

                if (hit.last_hit_object_id() != (*i).m_object->ID())
                    continue;

                (*i).m_distribute_mask.assign((*i).m_distribute_mask.get() | member_mask);
            }
        }
    }
}

template <typename T>
IC void CAgentEnemyManager::setup_mask(
    xr_vector<T>& objects, CMemberEnemy& enemy, const squad_mask_type& non_combat_members)
{
    auto I = std::find(objects.begin(), objects.end(), enemy.m_object->ID());
    if (I != objects.end())
    {
        (*I).m_squad_mask.assign((*I).m_squad_mask.get() | enemy.m_distribute_mask.get());
    }
}

IC void CAgentEnemyManager::setup_mask(CMemberEnemy& enemy, const squad_mask_type& non_combat_members)
{
    setup_mask(object().memory().visibles(), enemy, non_combat_members);
    setup_mask(object().memory().sounds(), enemy, non_combat_members);
    setup_mask(object().memory().hits(), enemy, non_combat_members);
}

void CAgentEnemyManager::assign_enemy_masks()
{
    {
        ENEMIES::iterator I = m_enemies.begin();
        ENEMIES::iterator E = m_enemies.end();
        const xr_vector<ALife::_OBJECT_ID> member_ids = combat_member_ids(object().member().combat_members());
        for (; I != E; ++I)
        {
            xr_vector<ALife::_OBJECT_ID>::const_iterator i = member_ids.begin();
            xr_vector<ALife::_OBJECT_ID>::const_iterator e = member_ids.end();
            for (; i != e; ++i)
            {
                CMemberOrder* member_order = object().member().get_member(*i);
                if (!member_order)
                    continue;

                member_order->object().memory().make_object_visible_somewhen((*I).m_object);
            }
        }
    }

    squad_mask_type non_combat_members = object().member().non_combat_members_mask();

    ENEMIES::iterator I = m_enemies.begin();
    ENEMIES::iterator E = m_enemies.end();
    for (; I != E; ++I)
        setup_mask(*I, non_combat_members);
}

void CAgentEnemyManager::assign_wounded()
{
    VERIFY(m_only_wounded_left);

#if 0 // def DEBUG
	u32						enemy_mask = 0;
	ENEMIES::iterator		I = m_enemies.begin();
	ENEMIES::iterator		E = m_enemies.end();
	for ( ; I != E; ++I) {
		VERIFY				(!(*I).m_distribute_mask.get());
		enemy_mask			|= (*I).m_mask.get();
	}
	VERIFY					(enemy_mask == object().member().combat_mask());
#endif // DEBUG

    u32 previous_wounded_count = m_wounded.size();
    WOUNDED_ENEMY* previous_wounded = (WOUNDED_ENEMY*)xr_alloca(previous_wounded_count * sizeof(WOUNDED_ENEMY));
    std::copy(m_wounded.begin(), m_wounded.end(), previous_wounded);
    m_wounded.clear();

#ifdef DEBUG
    {
        ENEMIES::iterator I = m_enemies.begin();
        ENEMIES::iterator E = m_enemies.end();
        for (; I != E; ++I)
        {
            VERIFY(!(*I).m_distribute_mask.get());
            VERIFY((*I).m_mask.get());
        }
    }
#endif // DEBUG

    squad_mask_type assigned = 0;
    {
        WOUNDED_ENEMY* I = previous_wounded;
        WOUNDED_ENEMY* E = previous_wounded + previous_wounded_count;
        for (; I != E; ++I)
        {
            ENEMIES::iterator J = std::find(m_enemies.begin(), m_enemies.end(), (*I).first);
            if (J == m_enemies.end())
                continue;

            CMemberOrder* member_order = object().member().get_member((*I).second.first);
            if (!member_order)
                continue;

            squad_mask_type mask = object().member().mask((*I).second.first);
            if (!(object().member().combat_mask() & mask))
                continue;

            CAgentMemberManager::iterator i = object().member().member(mask);
            if ((*I).first->Position().distance_to_sqr((*i)->object().Position()) >
                _sqr(wounded_enemy_reached_distance))
                continue;

            if (wounded_processor((*J).m_object) != ALife::_OBJECT_ID(-1))
                continue;

            wounded_processor((*J).m_object, (*I).second.first);
            (*J).m_distribute_mask.set(mask, TRUE);
            VERIFY((assigned | mask) != assigned);
            assigned |= mask;
        }
    }

    u32 combat_member_count = population(object().member().combat_mask());
    VERIFY(combat_member_count == object().member().combat_members().size());

    u32 population_level = 0;
    while (population(assigned) < combat_member_count)
    {
        CMemberEnemy* enemy = 0;
        const CAI_Stalker* processor = 0;
        float best_distance_sqr = flt_max;

        for (int i = 0; i < 2; ++i)
        {
            ENEMIES::iterator I = m_enemies.begin();
            ENEMIES::iterator E = m_enemies.end();
            for (; I != E; ++I)
            {
                if (population((*I).m_distribute_mask.get()) > population_level)
                    continue;

                squad_mask_type J = (*I).m_mask.get();
                J &= (assigned ^ squad_mask_type(-1));
                for (; J; J &= J - 1)
                {
                    squad_mask_type K = (J & (J - 1)) ^ J;
                    CAgentMemberManager::iterator i = object().member().member(K);
                    float distance_sqr = (*i)->object().Position().distance_to_sqr((*I).m_object->Position());
                    if (distance_sqr < best_distance_sqr)
                    {
                        best_distance_sqr = distance_sqr;
                        enemy = &*I;
                        processor = &(*i)->object();
                    }
                }
            }

            if (enemy)
                break;

            ++population_level;
        }

#ifdef DEBUG
        if (!enemy)
        {
            Msg(" ");
            Msg(" ");
            Msg("error will occur now, dumping valuable info");
            Msg("wounded enemies(%d):", m_enemies.size());
            {
                typedef ENEMIES::iterator iterator;
                iterator I = m_enemies.begin();
                iterator E = m_enemies.end();
                for (; I != E; ++I)
                    Msg("  [%s][0x%08x][0x%08x][%.2f]", (*I).m_object->cName().c_str(), (*I).m_mask.get(),
                        (*I).m_distribute_mask.get(), (*I).m_probability);
            }
            Msg("combat members(%d):", object().member().combat_members().size());
            {
                typedef CAgentMemberManager::MEMBER_STORAGE::const_iterator const_iterator;
                const_iterator I = object().member().combat_members().begin();
                const_iterator E = object().member().combat_members().end();
                for (; I != E; ++I)
                    Msg("  [%s][0x%08x][0x%08x]", (*I)->object().cName().c_str(), object().member().mask(&(*I)->object()),
                        (*I)->selected_enemy());
            }
        }
#endif

        //		VERIFY						(enemy);
        //		VERIFY						(processor);

        // this situation is possible
        // for example
        // 2 soldiers in group
        // has 2 different enemies
        // one of the enemy is going offline
        // soldier, whose enemy went offline
        // nulls the selected enemy
        // agent manager updates before
        // soldier update and soldier knows nothing about the second enemy
        // we have situation where agent_manager
        // is trying to assign wounded for the soldier
        // who doesn't have enemies at the moment
        // since the last enemy went offline and he knows nothing about the second one
        // so, in this case we just need to stop iterating
        // since nest procedure (setup_enemy_masks)
        // will make the second enemy known for the soldier
        // and on its update he will select it
        if (!enemy)
            return;

        //		Msg							("wounded enemy [%s] is assigned to member
        //[%s]",*enemy->m_object->cName(),*processor->cName());

        if (wounded_processor(enemy->m_object) == ALife::_OBJECT_ID(-1))
            wounded_processor(enemy->m_object, processor->ID());

        squad_mask_type mask = object().member().mask(processor);
        enemy->m_distribute_mask.set(mask, TRUE);
        VERIFY((assigned | mask) != assigned);
        assigned |= mask;
    }

    //	Msg								("[%6d] assigned = %x",Device.dwTimeGlobal,assigned);
    //	ENEMIES::iterator				I = m_enemies.begin();
    //	ENEMIES::iterator				E = m_enemies.end();
    //	for ( ; I != E; ++I)
    //		Msg							("[%6d] [%s] =
    //%x",Device.dwTimeGlobal,*(*I).m_object->cName(),(*I).m_distribute_mask.get());
}

void CAgentEnemyManager::distribute_enemies()
{
    if (!object().member().combat_mask())
        return;

    fill_enemies();

    if (m_enemies.empty())
        return;

    if (m_only_wounded_left)
        assign_wounded();
    else
    {
        compute_enemy_danger();
        assign_enemies();
        permutate_enemies();
    }

    assign_enemy_masks();
}

void CAgentEnemyManager::remove_links(IGameObject* object)
{
    VERIFY(object);
    m_wounded.erase(std::remove_if(m_wounded.begin(), m_wounded.end(), [object](const WOUNDED_ENEMY& wounded_enemy)
    {
        return wounded_enemy.first == object || wounded_enemy.second.first == object->ID();
    }), m_wounded.end());
}

void CAgentEnemyManager::update() {}
ALife::_OBJECT_ID CAgentEnemyManager::wounded_processor(const CEntityAlive* object)
{
    WOUNDED_ENEMIES::const_iterator I = m_wounded.begin();
    WOUNDED_ENEMIES::const_iterator E = m_wounded.end();
    for (; I != E; ++I)
    {
        if ((*I).first == object)
            return ((*I).second.first);
    }

    return (ALife::_OBJECT_ID(-1));
}

class find_wounded_predicate
{
    const CEntityAlive* m_object;

public:
    find_wounded_predicate(const CEntityAlive* object)
    {
        m_object = object;
        VERIFY(m_object);
    }

    bool operator()(const CAgentEnemyManager::WOUNDED_ENEMY& enemy) const
    {
        return (enemy.first == m_object);
    }
};

void CAgentEnemyManager::wounded_processor(const CEntityAlive* object, const ALife::_OBJECT_ID& wounded_processor_id)
{
    VERIFY(std::find_if(m_wounded.begin(), m_wounded.end(), find_wounded_predicate(object)) == m_wounded.end());
    m_wounded.emplace_back(object, std::make_pair(wounded_processor_id, false));
}

void CAgentEnemyManager::wounded_processed(const CEntityAlive* object, bool value)
{
    VERIFY(value);
    WOUNDED_ENEMIES::iterator I = std::find_if(m_wounded.begin(), m_wounded.end(), find_wounded_predicate(object));
    if (I == m_wounded.end())
        return;
    VERIFY((*I).second.first != ALife::_OBJECT_ID(-1));
    //	VERIFY							(!(*I).second.second);
    (*I).second.second = true;
}

bool CAgentEnemyManager::wounded_processed(const CEntityAlive* object) const
{
    WOUNDED_ENEMIES::const_iterator I =
        std::find_if(m_wounded.begin(), m_wounded.end(), find_wounded_predicate(object));
    if (I == m_wounded.end())
        return (false);
    return ((*I).second.second);
}

bool CAgentEnemyManager::assigned_wounded(const CEntityAlive* wounded, const CAI_Stalker* member)
{
    ENEMIES::const_iterator I = m_enemies.begin();
    ENEMIES::const_iterator E = m_enemies.end();
    for (; I != E; ++I)
    {
        if ((*I).m_object != wounded)
            continue;

        return (!!(*I).m_distribute_mask.test(object().member().mask(member)));
    }

    return (false);
}

bool CAgentEnemyManager::useful_enemy(const CEntityAlive* enemy, const CAI_Stalker* member) const
{
    if (!object().member().registered_in_combat(member))
        return (true);

    ENEMIES::const_iterator I = std::find(m_enemies.begin(), m_enemies.end(), enemy);
    if (I == m_enemies.end())
        return (true);

    return (!!(*I).m_distribute_mask.test(object().member().mask(member)));
}
