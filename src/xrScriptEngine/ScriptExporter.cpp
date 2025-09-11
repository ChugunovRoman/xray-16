#include "pch.hpp"

#include "ScriptExporter.hpp"

#include "xrCommon/xr_unordered_map.h"

namespace xray::script_export
{
static size_t nodes_count = 0;

node* node::first_node = nullptr;

node::node(export_func export_func, dependencies_getter deps_getter)
    : m_next_node(first_node), m_export_func(export_func), m_deps_getter(deps_getter)
{
    first_node = this;
    ++nodes_count;
}

node::~node()
{
    if (first_node == this)
        first_node = m_next_node;
    else
    {
        node* prev = first_node;
        while (prev && prev->m_next_node != this)
            prev = prev->m_next_node;
        if (prev)
            prev->m_next_node = m_next_node;
    }
}

void node::sort()
{
    enum class state
    {
        not_visited, visiting, done
    };
    xr_unordered_map<const node*, state> map;
    map.reserve(nodes_count);

    for (auto n = first_node; n; n = n->m_next_node)
        map[n] = state::not_visited;

    xr_vector<node*> sorted;
    sorted.reserve(map.size());

    std::function<void(const node*)> depth_first_search = [&](const node* n)
    {
        const auto it = map.find(n);
        if (it != map.end())
        {
            R_ASSERT2(it->second != state::visiting, "Cyclic dependency in script export!");
            if (it->second == state::done)
                return;
        }

        map[n] = state::visiting;

        const auto& [deps, deps_count] = n->m_deps_getter();
        for (size_t i = 0; i < deps_count; i++)
            depth_first_search(deps[i]);

        map[n] = state::done;
        sorted.push_back(const_cast<node*>(n));
    };

    for (auto& [n, _] : map)
        depth_first_search(n);

    node* prev = nullptr;
    for (auto it = sorted.rbegin(); it != sorted.rend(); ++it)
    {
        (*it)->m_next_node = prev;
        prev = *it;
    }
    first_node = prev;

    // This is always logged to help find out if some nodes are missing
    Msg("* Script exporter has %zu nodes registered.", nodes_count);
}

void node::export_all(lua_State* luaState)
{
    if (!first_node)
        return;

    ZoneScoped;

    static bool sorted = false;
    if (!sorted)
    {
        sort();
        sorted = true;
    }

    for (auto node = first_node; node; node = node->m_next_node)
        node->m_export_func(luaState);
}
} // namespace xray::script_export
