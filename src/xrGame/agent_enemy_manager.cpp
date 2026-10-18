////////////////////////////////////////////////////////////////////////////
//	Module 		: agent_enemy_manager.cpp
//	Created 	: 24.05.2004
//  Modified 	: 14.01.2005
//	Author		: Dmitriy Iassenev
//	Description : Agent enemy manager
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include <algorithm>
#include <tracy/Tracy.hpp>
#include "xrCommon/xr_unordered_map.h"
#include "agent_enemy_manager.h"
#include "npc_cpp_profile.h"
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
#include "performance_cvars.h"
#include "Level.h"
#include "xrCore/Profiling/PerfMetrics.hpp"

extern const float wounded_enemy_reached_distance;

// 26-framemove-game-next.md, scheduler overrun: the group's distribute_enemies is the tens-of-ms step of the
// scheduler (ai/memory/enemies, stalker/shed/agent_manager). Always on since 2026-10-18 (its switch
// npc_agent_distribute_fast removed, 41-cvar-cleanup.md): the same distribution, cheaper: the victory probability evaluate(member, enemy) (the ef_storage pattern function) is
// computed once per pair within one distribute_enemies call (compute_enemy_danger, assign_enemies and permutate_enemies
// ask it again for the same pairs; nothing the function reads changes within the call - healths, weapons, items), and
// compute_enemy_danger builds the list of combat members once instead of once per enemy. The answers, their order and
// all the writes are the same.
// Temporary A/B switch, not saved, 0 = as before. N > 0: a combat member without an enemy asks for the distribution on
// its memory update (CMemoryManager::update_enemies) - up to once a frame for the group while somebody lacks an enemy.
// Then it runs at most once in N ms, except at once when an enemy the last distribution knew (its enemies and the
// enemies the combat members had selected) died or went offline, or a combat member selected an enemy it did not know.
// The agent manager's own call (kill_enemy, ~1 s) is not limited. Price: a member that sees an enemy assigned to
// somebody else, or a member left without a target, waits up to N ms for its new assignment.
int npc_agent_distribute_min_ms = 250; // a tunable, not saved (the default since 2026-10-16, 41-cvar-cleanup.md)
// Always on since 2026-10-18 (its switch npc_agent_masks_batch removed): assign_enemy_masks (every combat member must
// "know" every enemy:
// make_object_visible_somewhen, E x M calls of O(visual memory + members)) does the full call only for the first member
// of each shared visual memory and ORs the squad masks of the rest into the same record: the same writes in the same
// order, so the same eviction of the visual memory (a record is only added by the first member, as before)

/** Max enabled visual-memory slots per NPC considered in squad fill_enemies (nearest by remembered pos vs NPC). */
constexpr u32 AGENT_FILL_ENEMIES_MAX_VISIBLE_CHECKS = 16;

namespace
{
// The cheaper distribution: the evaluate() answers of the running distribute_enemies call, by (object0, object1) ID
// pair. Only the Game thread distributes (the scheduler pass), the calls never nest
bool s_eval_memo_on = false;
xr_unordered_map<u32, float> s_eval_memo;

struct eval_memo_scope
{
    eval_memo_scope()
    {
        s_eval_memo.clear();
        s_eval_memo_on = true;
    }
    ~eval_memo_scope()
    {
        PERF_ADD(agent_dist_eval_pairs, s_eval_memo.size());
        s_eval_memo_on = false;
    }
};

struct squad_mem_slot
{
    u32 level_time{};
    Fvector position{};
    bool has{};
};

IC void merge_squad_mem_slot(xr_unordered_map<u16, squad_mem_slot>& m, u16 id, u32 t, const Fvector& p)
{
    if (id == u16(0xffff))
        return;
    const auto it = m.find(id);
    if (it == m.end())
        m.emplace(id, squad_mem_slot{t, p, true});
    else if (!it->second.has || it->second.level_time < t)
    {
        it->second.level_time = t;
        it->second.position = p;
        it->second.has = true;
    }
}

void fill_enemy_positions_from_squad_memory(CAgentMemoryManager& memory, xr_vector<CMemberEnemy>& enemies)
{
    ZoneNamedN(___tracy_fill_squad_slots, "agent_enemy/fill_squad_memory_slots", true);
    xr_unordered_map<u16, squad_mem_slot> slots;
    const size_t approx = memory.visibles().size() + memory.sounds().size() + memory.hits().size();
    slots.reserve(approx);

    {
        ZoneNamedN(___tracy_fill_squad_vis, "agent_enemy/fill_squad_memory_slots/visibles", true);
        for (const auto& v : memory.visibles())
        {
            if (!v.m_enabled)
                continue;
            merge_squad_mem_slot(slots, object_id(v.m_object), v.m_last_level_time, v.m_object_params.m_position);
        }
    }
    {
        ZoneNamedN(___tracy_fill_squad_snd, "agent_enemy/fill_squad_memory_slots/sounds", true);
        for (const auto& s : memory.sounds())
        {
            if (!s.m_enabled)
                continue;
            merge_squad_mem_slot(slots, object_id(s.m_object), s.m_last_level_time, s.m_object_params.m_position);
        }
    }
    {
        ZoneNamedN(___tracy_fill_squad_hit, "agent_enemy/fill_squad_memory_slots/hits", true);
        for (const auto& h : memory.hits())
        {
            if (!h.m_enabled)
                continue;
            merge_squad_mem_slot(slots, object_id(h.m_object), h.m_last_level_time, h.m_object_params.m_position);
        }
    }

    {
        ZoneNamedN(___tracy_fill_squad_apply, "agent_enemy/fill_squad_memory_slots/apply_to_enemies", true);
        for (auto& enemy : enemies)
        {
            const u16 id = object_id(enemy.m_object);
            const auto it = slots.find(id);
            if (it != slots.end() && it->second.has)
            {
                enemy.m_level_time = it->second.level_time;
                enemy.m_enemy_position = it->second.position;
            }
        }
    }
}
} // namespace

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
    ENEMIES* m_enemies{};
    squad_mask_type m_mask{};
    xr_unordered_map<const CEntityAlive*, u32>* m_index{};

    IC CEnemyFiller(ENEMIES* enemies, squad_mask_type mask, xr_unordered_map<const CEntityAlive*, u32>* index)
        : m_enemies(enemies), m_mask(mask), m_index(index)
    {
        VERIFY(m_index);
    }

    IC void operator()(const CEntityAlive* enemy) const
    {
        const auto it = m_index->find(enemy);
        if (it == m_index->end())
        {
            const u32 idx = (u32)m_enemies->size();
            m_enemies->push_back(CMemberEnemy(enemy, m_mask));
            m_index->emplace(enemy, idx);
            return;
        }

        (*m_enemies)[it->second].m_mask.set(m_mask, TRUE);
    }
};

IC void process_visible_for_squad_enemy_filler(
    const CVisualMemoryManager::CVisibleObject& vo, const CMemoryManager& mem, const CEnemyFiller& filler)
{
    if (!vo.m_enabled)
        return;
    const CEntityAlive* enemy = smart_cast<const CEntityAlive*>(vo.m_object);
    if (enemy && mem.enemy().useful(enemy))
        filler(enemy);
}

/** Optional: only the K nearest enabled visibles (by distance to remembered position) get useful()+filler. */
IC void fill_enemies_visual_nearest_capped(const CMemoryManager& mem, const CEnemyFiller& filler, u32 max_checks)
{
    const CVisualMemoryManager::VISIBLES& objects = mem.visual().objects();

    if (!max_checks || objects.size() <= max_checks)
    {
        CVisualMemoryManager::VISIBLES::const_iterator I = objects.begin();
        CVisualMemoryManager::VISIBLES::const_iterator E = objects.end();
        for (; I != E; ++I)
            process_visible_for_squad_enemy_filler(*I, mem, filler);
        return;
    }

    struct IdxDist
    {
        float d;
        u32 idx;
    };
    xr_vector<IdxDist> tmp;
    tmp.reserve(objects.size());
    const Fvector& my_pos = mem.object().Position();
    for (u32 i = 0; i < (u32)objects.size(); ++i)
    {
        const CVisualMemoryManager::CVisibleObject& vo = objects[i];
        if (!vo.m_enabled)
            continue;
        tmp.push_back({my_pos.distance_to_sqr(vo.m_object_params.m_position), i});
    }

    if (tmp.empty())
        return;

    if (tmp.size() <= max_checks)
    {
        for (const IdxDist& e : tmp)
            process_visible_for_squad_enemy_filler(objects[e.idx], mem, filler);
        return;
    }

    std::nth_element(tmp.begin(), tmp.begin() + ptrdiff_t(max_checks), tmp.end(),
        [](const IdxDist& a, const IdxDist& b) { return a.d < b.d; });
    tmp.resize(max_checks);
    for (const IdxDist& e : tmp)
        process_visible_for_squad_enemy_filler(objects[e.idx], mem, filler);
}

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
        ZoneNamedN(___tracy_fill_squads, "agent_enemy/fill_from_squads", true);
        xr_unordered_map<const CEntityAlive*, u32> enemy_index;
        const xr_vector<ALife::_OBJECT_ID> member_ids = [&]() {
            ZoneNamedN(___tracy_fill_squads_prep, "agent_enemy/fill_from_squads/prepare", true);
            enemy_index.reserve(256);
            return combat_member_ids(object().member().combat_members());
        }();

        {
            ZoneNamedN(___tracy_fill_squads_loop, "agent_enemy/fill_from_squads/members_loop", true);
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

                if (npc_perf_agent_enemy_fill_squads_trace_members)
                {
                    ZoneNamedN(___tracy_fill_squad_member, "agent_enemy/fill_from_squads/member", true);
                    ZoneTextVF(___tracy_fill_squad_member, "%s", stalker->cName().c_str());
                }

                member_order->probability(1.f);
                // No enemy in its own list: a heuristic, not the walk's answer. The list is the one of its last
                // memory update, made under the collect budget, without the enemies assigned to other members
                // (useful_enemy) and with the squad mask; the walk could still find an enemy only this member
                // sees, which then joins the distribution later
                if (member_order->object().memory().enemy().objects().empty())
                    continue;
                const CEnemyFiller filler(
                    &m_enemies, object().member().mask(&member_order->object()), &enemy_index);
                fill_enemies_visual_nearest_capped(member_order->object().memory(), filler, AGENT_FILL_ENEMIES_MAX_VISIBLE_CHECKS);
            }
        }
    }

    if (m_enemies.empty())
        return;

    VERIFY(!m_enemies.empty());

    {
        ZoneNamedN(___tracy_fill_wnd, "agent_enemy/fill_wounded_reconcile", true);
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
        ZoneNamedN(___tracy_fill_wscan, "agent_enemy/fill_scan_wounded_flags", true);
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
        }
    }
    {
        ZoneNamedN(___tracy_fill_pos, "agent_enemy/fill_object_information", true);
        fill_enemy_positions_from_squad_memory(object().memory(), m_enemies);
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

    u32 memo_key = 0;
    if (s_eval_memo_on)
    {
        memo_key = (u32(object0->ID()) << 16) | u32(object1->ID());
        const auto found = s_eval_memo.find(memo_key);
        if (found != s_eval_memo.end())
        {
            PERF_INC(agent_dist_eval_hit);
            return found->second;
        }
    }
    PERF_INC(agent_dist_eval);

    ai().ef_storage().non_alife().clear();
    ai().ef_storage().alife().clear();
    ai().ef_storage().non_alife().member() = object0;
    ai().ef_storage().non_alife().enemy() = object1;
    const float value = ai().ef_storage().m_pfVictoryProbability->ffGetValue() / 100.f;
    if (s_eval_memo_on)
        s_eval_memo.emplace(memo_key, value);
    return value;
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
    // the combat members do not change within the loop: one list, one get_member per member
    const xr_vector<ALife::_OBJECT_ID> member_ids = combat_member_ids(object().member().combat_members());
    xr_vector<CMemberOrder*> member_orders;
    member_orders.reserve(member_ids.size());
    for (const ALife::_OBJECT_ID id : member_ids)
        member_orders.push_back(object().member().get_member(id));
    for (CMemberEnemy& enemy : m_enemies)
    {
        float best = -1.f;
        for (CMemberOrder* member_order : member_orders)
        {
            if (!member_order)
                continue;

            float value = evaluate(enemy.m_object, &member_order->object());
            if (value > best)
                best = value;
        }
        enemy.m_probability = best;
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

void CAgentEnemyManager::assign_enemy_masks_batched()
{
    // the combat members in the order of the original loop, each with the mask its own make_object_visible_somewhen
    // would use and its visual memory (shared by the group: one record per enemy for all members)
    struct member_visibility
    {
        CMemoryManager* memory;
        squad_mask_type mask;
        const void* buffer; // null: the member does the full call (no visual memory, or another agent manager)
    };
    xr_vector<member_visibility> members;
    {
        const xr_vector<ALife::_OBJECT_ID> member_ids = combat_member_ids(object().member().combat_members());
        members.reserve(member_ids.size());
        for (const ALife::_OBJECT_ID id : member_ids)
        {
            CMemberOrder* member_order = object().member().get_member(id);
            if (!member_order)
                continue;

            CAI_Stalker& stalker = member_order->object();
            CVisualMemoryManager& visual = stalker.memory().visual();
            // make_object_visible_somewhen takes the mask from the member's own agent manager
            const bool batched = visual.has_objects() && (&stalker.agent_manager() == &object());
            members.push_back({&stalker.memory(), batched ? object().member().mask(id) : squad_mask_type(0),
                batched ? static_cast<const void*>(&visual.objects()) : nullptr});
        }
    }

    struct buffer_record
    {
        const void* buffer;
        MemorySpace::CVisibleObject* record;
    };
    xr_vector<buffer_record> records;
    for (CMemberEnemy& enemy : m_enemies)
    {
        records.clear();
        for (const member_visibility& member : members)
        {
            if (!member.buffer)
            {
                member.memory->make_object_visible_somewhen(enemy.m_object);
                continue;
            }

            auto I = std::find_if(records.begin(), records.end(),
                [&member](const buffer_record& record) { return record.buffer == member.buffer; });
            if (I == records.end() || !I->record)
            {
                // the first member of this memory: the full call (adds the record if there is none, evicting the
                // oldest one of a full memory, and keeps the member's "visible now" bit as it was)
                member.memory->make_object_visible_somewhen(enemy.m_object);
                MemorySpace::CVisibleObject* record = member.memory->visual().visible_object(enemy.m_object);
                if (I == records.end())
                    records.push_back({member.buffer, record});
                else
                    I->record = record;
                continue;
            }

            // what the full call does to an existing record: the member's bit in the squad mask, enabled; its
            // "visible now" bit is set and restored to the previous value, i.e. unchanged
            PERF_INC(agent_dist_masks_batched);
            I->record->m_squad_mask.assign(I->record->m_squad_mask.get() | member.mask);
            I->record->m_enabled = true;
        }
    }
}

void CAgentEnemyManager::assign_enemy_masks()
{
    assign_enemy_masks_batched();

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
    ZoneNamedN(___tracy_am_dist, "agent_enemy/distribute_enemies", true);
    if (!object().member().combat_mask())
        return;

    // Once per frame for the group: each combat member without an enemy calls this on its memory update, and each
    // call walks the visual memory of every member (fill_enemies) - O(members^2) a frame in a big group. The
    // memories hardly change within a frame, so the first call of the frame distributes for everybody.
    if (m_distributed_frame == Device.dwFrame)
        return;
    m_distributed_frame = Device.dwFrame;
    m_distributed_time = Device.dwTimeGlobal;
    m_unknown_enemy_selected = false;

    distribute_enemies_impl();

    if (npc_agent_distribute_min_ms > 0)
        remember_known_enemies();
}

void CAgentEnemyManager::remember_known_enemies()
{
    m_known_enemies.clear();
    // the enemies of this distribution are alive and online (fill_enemies, just now)
    for (const CMemberEnemy& enemy : m_enemies)
        m_known_enemies.push_back(enemy.m_object->ID());

    for (const CMemberOrder* member_order : object().member().combat_members())
    {
        if (!member_order || !member_order->m_object)
            continue;

        const CEntityAlive* selected = member_order->object().memory().enemy().selected();
        if (selected)
            m_known_enemies.push_back(selected->ID());
    }

    std::sort(m_known_enemies.begin(), m_known_enemies.end());
    m_known_enemies.erase(std::unique(m_known_enemies.begin(), m_known_enemies.end()), m_known_enemies.end());
    m_known_enemies_check_frame = u32(-1);
}

bool CAgentEnemyManager::known_enemy_gone()
{
    // once a frame for the group: the members without an enemy ask on their memory updates
    if (m_known_enemies_check_frame == Device.dwFrame)
        return (m_known_enemy_gone);

    m_known_enemies_check_frame = Device.dwFrame;
    m_known_enemy_gone = false;
    for (const ALife::_OBJECT_ID id : m_known_enemies)
    {
        const CEntityAlive* enemy = smart_cast<const CEntityAlive*>(Level().Objects.net_Find(id));
        if (!enemy || !enemy->g_Alive())
        {
            m_known_enemy_gone = true;
            break;
        }
    }

    return (m_known_enemy_gone);
}

void CAgentEnemyManager::on_member_enemy_selected(const CEntityAlive* enemy)
{
    if (!enemy || m_unknown_enemy_selected || !m_distributed_time)
        return;

    if (!std::binary_search(m_known_enemies.begin(), m_known_enemies.end(), enemy->ID()))
        m_unknown_enemy_selected = true;
}

void CAgentEnemyManager::distribute_enemies_from_memory()
{
    const u32 min_ms = u32(npc_agent_distribute_min_ms);
    if (min_ms && m_distributed_time && (m_distributed_frame != Device.dwFrame) &&
        (Device.dwTimeGlobal >= m_distributed_time) && (Device.dwTimeGlobal - m_distributed_time < min_ms))
    {
        if (!m_unknown_enemy_selected && !known_enemy_gone())
        {
            PERF_INC(agent_dist_throttled);
            return;
        }

        PERF_INC(agent_dist_urgent);
    }

    distribute_enemies();
}

void CAgentEnemyManager::distribute_enemies_impl()
{
    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::AgentEnemyDistribute);
    PERF_SCOPE(agent_dist);
    const eval_memo_scope eval_memo;

    {
        ZoneNamedN(___tracy_am_dist_fill, "agent_enemy/dist_fill_enemies", true);
        NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::AgentEnemyFill);
        PERF_SCOPE(agent_dist_fill);
        fill_enemies();
    }

    if (m_enemies.empty())
        return;
    PERF_ADD(agent_dist_enemies, m_enemies.size());
    PERF_ADD(agent_dist_members, object().member().members().size());

    NPC_CPP_PROFILE_SCOPE(ENpcCppProfileStage::AgentEnemyAssign);

    if (m_only_wounded_left)
    {
        ZoneNamedN(___tracy_am_dist_w, "agent_enemy/dist_assign_wounded", true);
        assign_wounded();
    }
    else
    {
        {
            ZoneNamedN(___tracy_am_dist_dg, "agent_enemy/dist_compute_enemy_danger", true);
            PERF_SCOPE(agent_dist_danger);
            compute_enemy_danger();
        }
        {
            ZoneNamedN(___tracy_am_dist_as, "agent_enemy/dist_assign_enemies", true);
            PERF_SCOPE(agent_dist_assign);
            assign_enemies();
        }
        {
            ZoneNamedN(___tracy_am_dist_perm, "agent_enemy/dist_permutate_enemies", true);
            PERF_SCOPE(agent_dist_permutate);
            permutate_enemies();
        }
    }

    {
        ZoneNamedN(___tracy_am_dist_mask, "agent_enemy/dist_assign_enemy_masks", true);
        PERF_SCOPE(agent_dist_masks);
        assign_enemy_masks();
    }
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
    // 45-engine-ai-audit.md, F2: the member mask once (registered_in_combat is combat_mask & mask(member), and the
    // answer below tested mask(member) again - a second linear search over the group members)
    const squad_mask_type member_mask = object().member().mask(member);
    if (!(object().member().combat_mask() & member_mask))
        return (true);

    ENEMIES::const_iterator I = std::find(m_enemies.begin(), m_enemies.end(), enemy);
    if (I == m_enemies.end())
        return (true);

    return (!!(*I).m_distribute_mask.test(member_mask));
}
