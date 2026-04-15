#include "pch.hpp"
#include "game_graph.h"
#include "xrCore/xrDebug_macros.h"

#include <algorithm>

bool CGameGraph::Search(u32 start_vertex_id, u32 dest_vertex_id, xr_vector<u32>& out_path,
    const xr_vector<GameGraph::STerrainPlace>* vertex_types, float max_range, u32 max_iteration_count,
    u32 max_visited_node_count) const
{
    bool start_is_accessible = true;
    auto is_accessible_fn = [this, &start_is_accessible, vertex_types](u32 vertex_id)
    {
        if (!accessible(vertex_id))
            return false;

        if (!start_is_accessible)
            return true;

        // Must not dereference vertex_types in Release (can be null); same relaxed policy as after first expansion.
        if (!vertex_types || vertex_types->empty())
        {
#ifdef DEBUG
            Msg("! warning : null or empty vertex types in CGameGraph::Search (terrain mask skipped)");
#endif
            return true;
        }

        xr_vector<GameGraph::STerrainPlace>::const_iterator I = vertex_types->begin();
        xr_vector<GameGraph::STerrainPlace>::const_iterator E = vertex_types->end();
        for (; I != E; ++I)
        {
            if (mask((*I).tMask, vertex(vertex_id)->vertex_type()))
                return true;
        }

        return false;
    };

    auto calc_cost = [](const CGameVertex* node1, const CGameVertex* node2, const_iterator edge)
    {
        (void)node1;
        (void)node2;
        return edge->distance();
    };

    auto distance_node = [this](const CGameVertex* node1, const CGameVertex* node2)
    {
        return node1->game_point().distance_to(node2->game_point());
    };

    thread_local xr_vector<std::pair<float, u32>> temp_priority;
    thread_local xr_map<u32, u32> temp_came_from;
    thread_local xr_map<u32, float> temp_cost_so_far;

    temp_priority.clear();
    temp_came_from.clear();
    temp_cost_so_far.clear();
    out_path.clear();

    u32 from_id = start_vertex_id;
    u32 to_id = dest_vertex_id;

    if (from_id == to_id)
    {
        out_path.push_back(start_vertex_id);
        return true;
    }

    temp_priority.push_back({0.f, from_id});
    temp_came_from.insert({from_id, from_id});
    temp_cost_so_far.insert({from_id, 0.f});

    const u32 max_path_hops = header().vertex_count() + 2;

    while (!temp_priority.empty() && max_iteration_count > 0)
    {
        const u32 current_node_id = temp_priority.back().second;
        temp_priority.pop_back();

        if (current_node_id == to_id)
        {
            u32 next_node = to_id;
            u32 hops = 0;
            while (next_node != from_id && hops < max_path_hops)
            {
                out_path.insert(out_path.begin(), next_node);
                const auto it = temp_came_from.find(next_node);
                if (it == temp_came_from.end())
                {
                    return false;
                }
                next_node = it->second;
                ++hops;
            }
            if (next_node != from_id)
            {
                return false;
            }
            out_path.insert(out_path.begin(), next_node);
            return true;
        }

        const CGameVertex* node = vertex(current_node_id);

        const_iterator i, e;
        begin(current_node_id, i, e);

        for (; i != e; i++)
        {
            u32 neighbor_id = i->vertex_id();
            if (!is_accessible_fn(neighbor_id))
                continue;

            if (max_iteration_count == 0)
                continue;
            max_iteration_count--;

            const CGameVertex* neighbor = vertex(neighbor_id);
            float new_cost = temp_cost_so_far[current_node_id] + calc_cost(node, neighbor, i);
            auto temp_cost_it = temp_cost_so_far.find(neighbor_id);
            if ((temp_cost_it != temp_cost_so_far.end() && temp_cost_it->second > new_cost) ||
                (temp_cost_it == temp_cost_so_far.end() && max_visited_node_count > temp_cost_so_far.size()))
            {
                const float distance = distance_node(vertex(to_id), neighbor);
                if (distance > max_range)
                    continue;

                if (temp_cost_it != temp_cost_so_far.end())
                    temp_cost_it->second = new_cost;
                else
                    temp_cost_so_far.insert({neighbor_id, new_cost});

                float priority = new_cost + distance;
                temp_priority.insert(
                    std::upper_bound(temp_priority.begin(), temp_priority.end(),
                        std::pair<float, u32>{priority, neighbor_id},
                        [](const std::pair<float, u32>& left, const std::pair<float, u32>& right)
                        { return left.first > right.first; }),
                    {priority, neighbor_id});

                auto came_it = temp_came_from.find(neighbor_id);
                if (came_it != temp_came_from.end())
                    came_it->second = current_node_id;
                else
                    temp_came_from.insert({neighbor_id, current_node_id});
            }
        }

        start_is_accessible = false;
    }
    return false;
}

bool CGameGraph::SearchNearestVertex(u32 start_vertex_id, CGameGraph::_LEVEL_ID level_id, u32& result) const
{
    auto is_accessible_fn = [this](u32 vertex_id) { return accessible(vertex_id); };

    auto calc_cost = [](const CGameVertex* node1, const CGameVertex* node2, const_iterator edge)
    {
        (void)node1;
        (void)node2;
        return edge->distance();
    };

    auto distance_node = [this](const CGameVertex* node1, const CGameVertex* node2)
    {
        return node1->game_point().distance_to(node2->game_point());
    };

    thread_local xr_vector<std::pair<float, u32>> temp_priority;
    thread_local xr_map<u32, u32> temp_came_from;
    thread_local xr_map<u32, float> temp_cost_so_far;

    temp_priority.clear();
    temp_came_from.clear();
    temp_cost_so_far.clear();

    u32 from_id = start_vertex_id;

    temp_priority.push_back({0.f, from_id});
    temp_came_from.insert({from_id, from_id});
    temp_cost_so_far.insert({from_id, 0.f});

    u32 expansions_left = header().vertex_count() + 1;

    while (!temp_priority.empty() && expansions_left > 0)
    {
        const u32 current_node_id = temp_priority.back().second;
        temp_priority.pop_back();
        --expansions_left;

        if (vertex(current_node_id)->level_id() == level_id)
        {
            result = current_node_id;
            return true;
        }

        const CGameVertex* node = vertex(current_node_id);

        const_iterator i, e;
        begin(current_node_id, i, e);

        for (; i != e; i++)
        {
            u32 neighbor_id = i->vertex_id();
            if (!is_accessible_fn(neighbor_id))
                continue;

            const CGameVertex* neighbor = vertex(neighbor_id);
            float new_cost = temp_cost_so_far[current_node_id] + calc_cost(node, neighbor, i);
            auto temp_cost_it = temp_cost_so_far.find(neighbor_id);
            if ((temp_cost_it != temp_cost_so_far.end() && temp_cost_it->second > new_cost) ||
                (temp_cost_it == temp_cost_so_far.end()))
            {
                if (temp_cost_it != temp_cost_so_far.end())
                    temp_cost_it->second = new_cost;
                else
                    temp_cost_so_far.insert({neighbor_id, new_cost});

                float priority = new_cost;
                temp_priority.insert(
                    std::upper_bound(temp_priority.begin(), temp_priority.end(),
                        std::pair<float, u32>{priority, neighbor_id},
                        [](const std::pair<float, u32>& left, const std::pair<float, u32>& right)
                        { return left.first > right.first; }),
                    {priority, neighbor_id});

                auto came_it = temp_came_from.find(neighbor_id);
                if (came_it != temp_came_from.end())
                    came_it->second = current_node_id;
                else
                    temp_came_from.insert({neighbor_id, current_node_id});
            }
        }
    }
    return false;
}
