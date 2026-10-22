////////////////////////////////////////////////////////////////////////////
//	Module 		: level_graph.cpp
//	Created 	: 02.10.2001
//  Modified 	: 11.11.2003
//	Author		: Oles Shihkovtsov, Dmitriy Iassenev
//	Description : Level graph
////////////////////////////////////////////////////////////////////////////

#include "pch.hpp"
#include "level_graph.h"
#include "xrCore/xrDebug_macros.h"
#include "xrEngine/profiler.h"
#include "xrCore/Profiling/PerfMetrics.hpp"
#include "xrAICore/Navigation/ai_graph_engine_cvars.h"

#include <algorithm>
#include <queue>

namespace
{
using OpenPQEntry = std::pair<float, u32>;
/** Min-heap by `first` (same ordering as sorted vector + pop_back in legacy Search). */
struct MinHeapByFirstCmp
{
    bool operator()(const OpenPQEntry& a, const OpenPQEntry& b) const noexcept { return a.first > b.first; }
};
using OpenPQ = std::priority_queue<OpenPQEntry, std::vector<OpenPQEntry>, MinHeapByFirstCmp>;

thread_local OpenPQ tls_nearest_open_pq;

// 33-level-path.md (plans/optimization_mt), M: CLevelGraph::last_search_stats() of this thread
thread_local CLevelGraph::SearchStats tls_search_stats;
// 34 (33-level-path.md, journal): CLevelGraph::last_vertex_stats() of this thread
thread_local CLevelGraph::VertexStats tls_vertex_stats;

// 33-level-path.md, item 0: the open/closed data of one level A* (CLevelGraph::Search, SearchRun below).
// An open addressing hash (linear probing, Fibonacci hashing of the vertex id) instead of two xr_map with a heap node
// per vertex. A slot is taken when its stamp equals the stamp of the current search, so a new search empties the
// table by one increment. Why not dense arrays by vertex id: a level has up to ~1.85 M vertices, 12-20 B each is
// 22-37 MB per thread that ever searches (the Game thread for stalkers, main and others for the monsters' synchronous
// searches) and a new size on a level change, while a search opens at most max_visited_node_count (65500) vertices:
// 2^17 slots (2.5 MB) keep the load under 0.5, the table is cache-friendlier and doubles when a caller allows more.
struct SearchNode
{
    u32 key; // vertex id
    u32 stamp; // the search that wrote the slot; another value = an empty slot
    float g; // cost so far
    u32 parent; // came from
    u32 expanded; // 1: expanded with the current g - any queue entry of the vertex popped later is a no-op
};

class SearchNodeTable
{
public:
    static constexpr u32 initial_bits = 17;
    static constexpr u32 max_bits = 30;

    void begin()
    {
        if (m_slots.empty())
        {
            m_bits = initial_bits;
            m_slots.assign(size_t(1) << m_bits, SearchNode{0, 0, 0.f, 0, 0});
        }
        if (++m_stamp == 0) // wrapped: forget every slot once
        {
            for (SearchNode& slot : m_slots)
                slot.stamp = 0;
            m_stamp = 1;
        }
        m_count = 0;
    }

    u32 size() const { return m_count; }
    size_t capacity() const { return m_slots.size(); }

    SearchNode* find(u32 key)
    {
        const u32 mask = u32(m_slots.size() - 1);
        for (u32 i = hash(key);; i = (i + 1) & mask)
        {
            SearchNode& slot = m_slots[i];
            if (slot.stamp != m_stamp)
                return nullptr;
            if (slot.key == key)
                return &slot;
        }
    }

    // the slot of key: taken (found = true) or the empty one insert() would fill. Grows first when one more key would
    // load the table over a half, so a pointer is valid until the next find_or_slot()
    SearchNode* find_or_slot(u32 key, bool& found)
    {
        if ((m_count + 1) * 2 > m_slots.size() && m_bits < max_bits)
            grow();
        const u32 mask = u32(m_slots.size() - 1);
        for (u32 i = hash(key);; i = (i + 1) & mask)
        {
            SearchNode& slot = m_slots[i];
            if (slot.stamp != m_stamp)
            {
                found = false;
                return &slot;
            }
            if (slot.key == key)
            {
                found = true;
                return &slot;
            }
        }
    }

    // fills an empty slot returned by find_or_slot()
    void insert(SearchNode* slot, u32 key, float g, u32 parent)
    {
        slot->key = key;
        slot->stamp = m_stamp;
        slot->g = g;
        slot->parent = parent;
        slot->expanded = 0;
        ++m_count;
    }

private:
    u32 hash(u32 key) const { return (key * 2654435769u) >> (32 - m_bits); }

    void grow()
    {
        xr_vector<SearchNode> old;
        old.swap(m_slots);
        ++m_bits;
        m_slots.assign(size_t(1) << m_bits, SearchNode{0, 0, 0.f, 0, 0});
        const u32 stamp = m_stamp;
        m_stamp = 1; // the new slots are all 0
        const u32 mask = u32(m_slots.size() - 1);
        for (const SearchNode& node : old)
        {
            if (node.stamp != stamp)
                continue;
            u32 i = hash(node.key);
            while (m_slots[i].stamp == m_stamp)
                i = (i + 1) & mask;
            m_slots[i] = node;
            m_slots[i].stamp = m_stamp;
        }
        Msg("* level_graph: the A* table of a thread grew to %u slots (%u KB)", u32(m_slots.size()),
            u32(m_slots.size() * sizeof(SearchNode) / 1024));
    }

    xr_vector<SearchNode> m_slots;
    u32 m_bits{};
    u32 m_stamp{};
    u32 m_count{};
};

// 33-level-path.md, sliced A*: one level A* - its open/closed data, the call it answers and how far it got. A plain
// search runs it from the start to the end in one call. A sliced one (CLevelGraph::SearchSlice) may stop it between
// two vertices once its time slice is over and keep it in a slot; a later call with the same call continues it. The
// heap, the table and the counters are all the A* has, so the continued search makes the same steps in the same order
// as one that never stopped.
struct SearchRun
{
    SearchNodeTable nodes;
    // the binary heap: the same push_heap/pop_heap calls std::priority_queue makes (the standard defines push/pop/top
    // by them), so the pops come in the same order, ties included; clear() keeps the memory
    xr_vector<OpenPQEntry> open;

    // the call (Search arguments, the graph and its access mask)
    const CLevelGraph* graph{};
    u32 generation{};
    u64 mask_signature{};
    u32 start{};
    u32 dest{};
    float max_range{};
    u32 max_iteration_count{};
    u32 max_visited_node_count{};

    // the progress
    u32 target_ix{};
    u32 target_iz{};
    u32 iterations_left{}; // max_iteration_count as the loop has counted it down
    u32 pops{}; // heap pops so far (a vertex may be expanded again: pops / visited is the work per vertex)
    bool cap_hit{};

    // a slot: busy = holds a search put aside; owner / frame = who continued it last and when (the bookkeeping of the
    // slots only, the owner is never dereferenced); slices / ticks = the slices so far and their time
    bool busy{};
    const void* owner{};
    u32 frame{};
    u32 slices{};
    u64 ticks{};

    bool same_call(const CLevelGraph* graph_, u32 generation_, u64 mask_signature_, u32 start_, u32 dest_,
        float max_range_, u32 max_iteration_count_, u32 max_visited_node_count_) const
    {
        return graph == graph_ && generation == generation_ && mask_signature == mask_signature_ && start == start_ &&
            dest == dest_ && max_range == max_range_ && max_iteration_count == max_iteration_count_ &&
            max_visited_node_count == max_visited_node_count_;
    }
};

enum class SearchEnd
{
    failed,
    found,
    aside, // the time slice is over: the state is in the SearchRun
};

// every search of a thread starts here; a search put aside moves to a slot by a swap (the run gets the slot's buffers)
thread_local SearchRun tls_search_run;
// the searches put aside. A few: a search goes aside only when it is longer than its slice (tens of a frame at most);
// each slot that ever held one keeps its buffers (~2.5 MB table + heap) for the life of the thread
constexpr u32 search_slot_count = 4;
// a busy slot not continued for this many frames may be taken by another search (its owner went away or changed
// its call; the owner's own next sliced search frees it at once)
constexpr u32 search_slot_stale_frames = 8;
thread_local SearchRun tls_search_slots[search_slot_count];
thread_local CLevelGraph::SearchSlice* tls_search_slice = nullptr;

std::atomic<u32> s_level_graph_generation{0};

// the head of a search: runs nothing when the answer is known at once (the same vertex, an inaccessible end).
// Returns 1 = found, 0 = not found (out_path set as the old code did), -1 = the loop has to run (out_path untouched)
int search_begin(const CLevelGraph& graph, SearchRun& run, u32 generation, u64 mask_signature, u32 start_vertex_id,
    u32 dest_vertex_id, xr_vector<u32>& out_path, float max_range, u32 max_iteration_count,
    u32 max_visited_node_count)
{
    run.open.clear();
    run.nodes.begin();
    run.graph = &graph;
    run.generation = generation;
    run.mask_signature = mask_signature;
    run.start = start_vertex_id;
    run.dest = dest_vertex_id;
    run.max_range = max_range;
    run.max_iteration_count = max_iteration_count;
    run.max_visited_node_count = max_visited_node_count;
    run.iterations_left = max_iteration_count;
    run.pops = 0;
    run.cap_hit = false;

    if (start_vertex_id == dest_vertex_id)
    {
        out_path.clear();
        out_path.push_back(start_vertex_id);
        return 1;
    }

    if (!graph.is_accessible(start_vertex_id) || !graph.is_accessible(dest_vertex_id))
    {
        out_path.clear();
        return 0;
    }

    const CLevelGraph::CLevelVertex* target_vertex = graph.vertex(dest_vertex_id);
    graph.unpack_xz(*target_vertex, run.target_ix, run.target_iz);

    run.open.push_back({0.f, start_vertex_id});
    std::push_heap(run.open.begin(), run.open.end(), MinHeapByFirstCmp());
    {
        bool found;
        run.nodes.insert(run.nodes.find_or_slot(start_vertex_id, found), start_vertex_id, 0.f, start_vertex_id);
    }
    return -1;
}

// the A* loop on the state of run, see CLevelGraph::Search. deadline != 0: every 64 pops the loop checks Ticks() and,
// once past it, stops before the next pop with the counters saved in run (SearchEnd::aside, out_path untouched).
// Only the loop head can stop it, so a continued run makes the very steps a whole one makes
SearchEnd search_loop(const CLevelGraph& graph, SearchRun& run, xr_vector<u32>& out_path, u64 deadline)
{
    using CLevelVertex = CLevelGraph::CLevelVertex;
    SearchNodeTable& nodes = run.nodes;
    xr_vector<OpenPQEntry>& open = run.open;
    const MinHeapByFirstCmp cmp;

    const float cell = graph.header().cell_size();
    const u32 start_vertex_id = run.start;
    const u32 dest_vertex_id = run.dest;
    const float max_range = run.max_range;
    const u32 max_visited_node_count = run.max_visited_node_count;
    const u32 target_ix = run.target_ix;
    const u32 target_iz = run.target_iz;
    u32 max_iteration_count = run.iterations_left;
    bool cap_hit = run.cap_hit;

    auto calc_cost = [cell](CLevelVertex*, CLevelVertex*) { return cell; };

    auto distance_node = [&graph, cell, target_ix, target_iz](CLevelVertex* node)
    {
        u32 ix{}, iz{};
        graph.unpack_xz(node, ix, iz);
        const int dx = int(ix) - int(target_ix);
        const int dz = int(iz) - int(target_iz);
        // Heuristic in world units: Manhattan distance in grid * cell_size, scaled.
        return cell * 2.f * (float(_abs(dx)) + float(_abs(dz)));
    };

    u32 pops = 0;
    while (!open.empty() && max_iteration_count > 0)
    {
        if (deadline && (pops & 63) == 63 && xray::perf::Ticks() >= deadline)
        {
            run.iterations_left = max_iteration_count;
            run.pops += pops;
            run.cap_hit = cap_hit;
            return SearchEnd::aside;
        }

        const u32 current_node_id = open.front().second;
        std::pop_heap(open.begin(), open.end(), cmp);
        open.pop_back();
        ++pops;

        if (current_node_id == dest_vertex_id)
        {
            run.iterations_left = max_iteration_count;
            run.pops += pops;
            run.cap_hit = cap_hit;
            out_path.clear();
            const u32 max_path_hops = graph.header().vertex_count() + 2;
            u32 next_node = dest_vertex_id;
            u32 hops = 0;
            while (next_node != start_vertex_id && hops < max_path_hops)
            {
                out_path.push_back(next_node);
                const SearchNode* it = nodes.find(next_node);
                if (!it)
                {
                    std::reverse(out_path.begin(), out_path.end());
                    return SearchEnd::failed;
                }
                next_node = it->parent;
                ++hops;
            }
            if (next_node != start_vertex_id)
            {
                std::reverse(out_path.begin(), out_path.end());
                return SearchEnd::failed;
            }
            out_path.push_back(next_node);
            std::reverse(out_path.begin(), out_path.end());
            return SearchEnd::found;
        }

        SearchNode* current = nodes.find(current_node_id);
        if (current->expanded)
            continue; // a stale entry: see CLevelGraph::Search
        current->expanded = 1;
        const float current_cost = current->g; // the table may grow below: no pointer is kept

        CLevelVertex* node = graph.vertex(current_node_id);

        for (s32 neighbor_index = 0; neighbor_index < 4; ++neighbor_index)
        {
            if (max_iteration_count == 0)
                continue;

            const u32 neighbor_id = node->link(neighbor_index);
            if (!graph.is_accessible(neighbor_id))
                continue;

            CLevelVertex* neighbor = graph.vertex(neighbor_id);
            const float new_cost = current_cost + calc_cost(node, neighbor);

            bool found;
            SearchNode* slot = nodes.find_or_slot(neighbor_id, found);
            if (found && slot->g <= new_cost)
                continue;

            if (nodes.size() >= max_visited_node_count)
            {
                cap_hit = true;
                continue;
            }

            const float distance = distance_node(neighbor);
            if (distance > max_range)
                continue;

            if (found)
            {
                slot->g = new_cost;
                slot->parent = current_node_id;
                slot->expanded = 0; // a better cost: the next pop of the vertex expands it again
            }
            else
                nodes.insert(slot, neighbor_id, new_cost, current_node_id);

            const float priority = new_cost + distance;
            open.push_back({priority, neighbor_id});
            std::push_heap(open.begin(), open.end(), cmp);

            if (--max_iteration_count == 0)
                break;
        }
    }

    run.iterations_left = max_iteration_count;
    run.pops += pops;
    run.cap_hit = cap_hit;
    out_path.clear();
    return SearchEnd::failed;
}
} // namespace

CLevelGraph::CLevelGraph(const char* fileName)
    : m_level_id(GameGraph::_LEVEL_ID(-1))
{
    string256 filePath;
    strconcat(sizeof(filePath), filePath, fileName, LEVEL_GRAPH_NAME);
    Initialize(filePath);
}

CLevelGraph::CLevelGraph()
{
    string_path filePath;
    FS.update_path(filePath, "$level$", LEVEL_GRAPH_NAME);
    Initialize(filePath);
}

void CLevelGraph::Initialize(const char* filePath)
{
    m_reader = FS.r_open(filePath);
    R_ASSERT3(m_reader, "Please, compile AI for the level.", filePath);
    // m_header & data
    m_header = (CHeader*)m_reader->pointer();
    ASSERT_XRAI_VERSION_MATCH(header().version(), "Level graph");
    m_reader->advance(sizeof(CHeader));
    const auto& box = header().box();
    m_nodes = xr_new<CLevelGraphManager>(m_reader, header().vertex_count(), header().version());
    m_row_length = iFloor((box.vMax.z - box.vMin.z) / header().cell_size() + EPS_L + 1.5f);
    m_column_length = iFloor((box.vMax.x - box.vMin.x) / header().cell_size() + EPS_L + 1.5f);
    m_access_mask.assign(header().vertex_count(), true);
    m_mask_signature.store(0, std::memory_order_relaxed); // nothing masked
    m_search_generation = ++s_level_graph_generation; // 33-level-path.md, sliced A*
    unpack_xz(vertex_position(box.vMax), m_max_x, m_max_z);
    // [GW] diagnostics: which level.ai was actually opened (gamedata or a patch archive)
    // and how many vertices it holds - stale patrol point ids point to a mismatched file
    Msg("~ [GW] level graph loaded: %s, vertices[%u]", filePath, header().vertex_count());
    build_row_start();
}

// 34 (plans/optimization_mt/33-level-path.md, journal): m_row_start[x] = the first vertex whose
// xz >= x * m_row_length, i.e. the first vertex of the row x (rows without vertices start where the next one does).
// One pass over the vertices (~3 ms for the 1.85 M of Zaton); (rows + 1) * 4 bytes (7 KB for Zaton). Every
// std::lower_bound(begin, end, xz) of the old code returns the same vertex from the row of xz alone: the vertices
// before the row have xz < x * m_row_length <= xz, those from the next row on have xz >= (x + 1) * m_row_length > xz.
void CLevelGraph::build_row_start()
{
    m_row_start.clear();
    const u32 count = header().vertex_count();
    const CLevelVertex* nodes = m_nodes->begin();
    u32 rows = std::max(m_column_length, m_max_x + 1);
    if (count)
        rows = std::max(rows, nodes[count - 1].position().xz() / m_row_length + 1);
    xr_vector<u32> starts(size_t(rows) + 1);
    u32 x = 0, prev_xz = 0;
    for (u32 i = 0; i < count; ++i)
    {
        const u32 xz = nodes[i].position().xz();
        if (xz < prev_xz)
        {
            Msg("! level_graph: the vertices are not sorted by xz (vertex %u), the row table is off", i);
            return;
        }
        prev_xz = xz;
        for (const u32 row = xz / m_row_length; x <= row; ++x)
            starts[x] = i;
    }
    for (; x <= rows; ++x)
        starts[x] = count;
    m_row_start.swap(starts);
}

CLevelGraph::CLevelVertex* CLevelGraph::lower_bound_xz(u32 xz) const
{
    CLevelVertex* B = m_nodes->begin();
    const u32 x = xz / m_row_length;
    if (size_t(x) + 1 < m_row_start.size())
        return std::lower_bound(B + m_row_start[x], B + m_row_start[x + 1], xz);
    return std::lower_bound(B, m_nodes->end(), xz);
}

CLevelGraph::~CLevelGraph()
{
    // 50c-commit-growth-analysis.md (plans/optimization_mt): the manager was never deleted - the converted copy of the
    // vertices of an older level.ai (GW ships version 10, ~25 bytes a vertex) leaked on every level load. It goes
    // first: with the current version its vertices point into m_reader.
    xr_delete(m_nodes);
    FS.r_close(m_reader);
}
u32 CLevelGraph::vertex(const Fvector& position) const
{
    CLevelGraph::CPosition _node_position;
    vertex_position(_node_position, position);
    float min_dist = flt_max;
    u32 selected;
    set_invalid_vertex(selected);
    for (u32 i = 0; i < header().vertex_count(); ++i)
    {
        float dist = distance(i, position);
        if (dist < min_dist)
        {
            min_dist = dist;
            selected = i;
        }
    }

    VERIFY(valid_vertex_id(selected));
    return (selected);
}

// 34 (33-level-path.md, journal; always on since 2026-10-18): the same choices on m_row_start (build_row_start): the vertex
// under the position by a binary search in its row (vertex_id -> lower_bound_xz), guess_vertex_id_fast, and instead of
// the full search over every vertex vertex_nearest_fast over the rows nearest to the position. Every step returns the
// vertex of the old code (stand scratchpad/p34_vertex/vertex_eq.cpp on the level.ai of Zaton). `stats` gets the path
u32 CLevelGraph::VertexInternal(u32 current_node_id, const Fvector& position, VertexStats& stats) const
{
    const bool fast = !m_row_start.empty();
    stats.fast = true;
    stats.fallback = !fast;
    u32 id;
    if (valid_vertex_position(position))
    {
        // so, our position is inside the level graph bounding box
        if (valid_vertex_id(current_node_id) && inside(vertex(current_node_id), position))
        {
            // so, our node corresponds to the position
            stats.branch = VertexStats::current;
            return (current_node_id);
        }

        // so, our node doesn't correspond to the position
        // try to search it with O(logN) time algorithm
        u32 _vertex_id = vertex_id(position);
        if (valid_vertex_id(_vertex_id))
        {
            stats.branch = VertexStats::xz; // every return of this block
            // so, there is a node which corresponds with x and z to the position
            bool ok = true;
            if (valid_vertex_id(current_node_id))
            {
                {
                    CLevelVertex const& vertex = *this->vertex(current_node_id);
                    for (u32 i = 0; i < 4; ++i)
                    {
                        if (vertex.link(i) == _vertex_id)
                        {
                            return (_vertex_id);
                        }
                    }
                }
                {
                    CLevelVertex const& vertex = *this->vertex(_vertex_id);
                    for (u32 i = 0; i < 4; ++i)
                    {
                        if (vertex.link(i) == current_node_id)
                        {
                            return (_vertex_id);
                        }
                    }
                }

                float y0 = vertex_plane_y(current_node_id, position.x, position.z);
                float y1 = vertex_plane_y(_vertex_id, position.x, position.z);
                bool over0 = position.y > y0;
                bool over1 = position.y > y1;
                float y_dist0 = position.y - y0;
                float y_dist1 = position.y - y1;
                if (over0)
                {
                    if (over1)
                    {
                        if (y_dist1 - y_dist0 > 1.f)
                            ok = false;
                        else
                            ok = true;
                    }
                    else
                    {
                        if (y_dist0 - y_dist1 > 1.f)
                            ok = false;
                        else
                            ok = true;
                    }
                }
                else
                {
                    ok = true;
                }
            }
            if (ok)
            {
                return (_vertex_id);
            }
        }
    }

    if (!valid_vertex_id(current_node_id))
    {
        if (fast)
        {
            // 34: the same vertex from the rows nearest to the position, tens of us instead of ~10 ms (Zaton)
            ZoneScopedN("level_graph/full_search_fast");
            PERF_SCOPE(lg_vertex_full_fast);
            if (vertex_nearest_fast(position, id))
            {
                stats.branch = VertexStats::full_fast;
                return (id);
            }
            stats.fallback = true; // nothing finite near the position: the old loop decides
        }
        stats.branch = VertexStats::full;
        // so, we do not have a correct current node
        // performing very slow full search
        // P19 (plans/optimization_mt/14-spike-instrumentation.md): O(vertex count), a suspect of rare UpdateCL peaks
        ZoneScopedN("level_graph/full_search");
        PERF_SCOPE(lg_vertex_full_search);
        PERF_SPIKE("level_graph/full_search");
        id = vertex(position);
        VERIFY(valid_vertex_id(id));
        return (id);
    }

    u32 new_vertex_id;
    {
        // P19: no vertex under the position (off the AI map, in the air...): 9x9 cells around, binary search each
        PERF_HOT_ZONE("level_graph/guess");
        PERF_SCOPE(lg_vertex_guess);
        PERF_SPIKE("level_graph/guess");
        new_vertex_id = fast ? guess_vertex_id_fast(current_node_id, position) : guess_vertex_id(current_node_id, position);
    }
    if (new_vertex_id != current_node_id)
    {
        stats.branch = VertexStats::guess;
        return (new_vertex_id);
    }

    stats.branch = VertexStats::nearest;
    PERF_HOT_ZONE("level_graph/nearest");
    PERF_SCOPE(lg_vertex_nearest);

    // so, our position is outside the level graph bounding box
    // or
    // there is no node for the current position
    // try to search the nearest one iteratively
    SContour _contour;
    Fvector point;
    u32 best_vertex_id = current_node_id;
    contour(_contour, current_node_id);
    nearest(point, position, _contour);
    float best_distance_sqr = position.distance_to_sqr(point);
    const_iterator i, e;
    begin(current_node_id, i, e);
    for (; i != e; ++i)
    {
        u32 level_vertex_id = value(current_node_id, i);
        if (!valid_vertex_id(level_vertex_id))
            continue;

        contour(_contour, level_vertex_id);
        nearest(point, position, _contour);
        float distance_sqr = position.distance_to_sqr(point);
        if (best_distance_sqr > distance_sqr)
        {
            best_distance_sqr = distance_sqr;
            best_vertex_id = level_vertex_id;
        }
    }
    return (best_vertex_id);
}

u32 CLevelGraph::vertex(u32 current_node_id, const Fvector& position) const
{
    START_PROFILE("Level_Graph::find vertex")
    PERF_SCOPE(lg_vertex); // P19: every call; the slow paths are ai/level_graph/full_search, guess, nearest
    NodeTime.Begin();
    const u64 t0 = xray::perf::Ticks();
    VertexStats& stats = tls_vertex_stats;
    stats.branch = VertexStats::none;
    u32 result = VertexInternal(current_node_id, position, stats);
    // 34: the path for the [perf-spike] go/ai_locations line (xrGame, CGameObject::spatial_move)
    stats.ticks = xray::perf::Ticks() - t0;
    stats.current_id = current_node_id;
    stats.result = result;
    stats.position = position;
    ++stats.seq;
#if XR_PERF_METRICS
    // ai/level_graph/vertex_fast_hits: the row table answered (the current vertex needs none); vertex_fallback: the old
    // code answered (no row table, or nothing finite near the position)
    if (stats.fast && xray::perf::g_perf_metrics_active)
    {
        if (stats.fallback)
            PERF_INC(lg_vertex_fallback);
        else if (stats.branch != VertexStats::current)
            PERF_INC(lg_vertex_fast_hits);
    }
#endif
    NodeTime.End();
    return result;
    STOP_PROFILE
}

const CLevelGraph::VertexStats& CLevelGraph::last_vertex_stats() { return tls_vertex_stats; }

const char* CLevelGraph::vertex_branch_name(u8 branch)
{
    switch (branch)
    {
    case VertexStats::current: return "current";
    case VertexStats::xz: return "xz";
    case VertexStats::full: return "full";
    case VertexStats::full_fast: return "full_fast";
    case VertexStats::guess: return "guess";
    case VertexStats::nearest: return "nearest";
    default: return "-";
    }
}

u32 CLevelGraph::vertex_id(const Fvector& position) const
{
    VERIFY2(valid_vertex_position(position),
        make_string("invalid position for CLevelGraph::vertex_id specified: [%f][%f][%f]", VPUSH(position)));

    CPosition _vertex_position = vertex_position(position);
    CLevelVertex* B = m_nodes->begin();
    CLevelVertex* E = m_nodes->end();
    CLevelVertex* I = lower_bound_xz(_vertex_position.xz()); // 34: in the row when the row table is there
    if ((I == E) || ((*I).position().xz() != _vertex_position.xz()))
        return (u32(-1));

    u32 best_vertex_id = u32(I - B);
    float y = vertex_plane_y(best_vertex_id, position.x, position.z);
    for (++I; I != E; ++I)
    {
        if ((*I).position().xz() != _vertex_position.xz())
            break;

        u32 new_vertex_id = u32(I - B);
        float _y = vertex_plane_y(new_vertex_id, position.x, position.z);
        if (y <= position.y)
        {
            // so, current node is under the specified position
            if (_y <= position.y)
            {
                // so, new node is under the specified position
                if (position.y - _y < position.y - y)
                {
                    // so, new node is closer to the specified position
                    y = _y;
                    best_vertex_id = new_vertex_id;
                }
            }
        }
        else
            // so, current node is over the specified position
            if (_y <= position.y)
        {
            // so, new node is under the specified position
            y = _y;
            best_vertex_id = new_vertex_id;
        }
        else
            // so, new node is over the specified position
            if (_y - position.y < y - position.y)
        {
            // so, new node is closer to the specified position
            y = _y;
            best_vertex_id = new_vertex_id;
        }
    }

    return (best_vertex_id);
}

static const int max_guess_vertex_count = 4;

u32 CLevelGraph::guess_vertex_id(u32 const& current_vertex_id, Fvector const& position) const
{
    VERIFY(valid_vertex_id(current_vertex_id));

    CPosition vertex_position;
    if (valid_vertex_position(position))
        vertex_position = this->vertex_position(position);
    else
        vertex_position = vertex(current_vertex_id)->position();

    u32 x, z;
    unpack_xz(vertex_position, x, z);

    SContour vertex_contour;
    contour(vertex_contour, current_vertex_id);
    Fvector best_point;
    float result_distance = nearest(best_point, position, vertex_contour);
    u32 result_vertex_id = current_vertex_id;

    CLevelVertex const* B = m_nodes->begin();
    CLevelVertex const* E = m_nodes->end();
    u32 start_x = (u32)std::max(0, int(x) - max_guess_vertex_count);
    u32 stop_x = std::min(max_x(), x + (u32)max_guess_vertex_count);
    u32 start_z = (u32)std::max(0, int(z) - max_guess_vertex_count);
    u32 stop_z = std::min(max_z(), z + (u32)max_guess_vertex_count);
    for (u32 i = start_x; i <= stop_x; ++i)
    {
        for (u32 j = start_z; j <= stop_z; ++j)
        {
            u32 test_xz = i * m_row_length + j;
            CLevelVertex const* I = std::lower_bound(B, E, test_xz);
            if (I == E)
                continue;

            if ((*I).position().xz() != test_xz)
                continue;

            u32 best_vertex_id = u32(I - B);
            contour(vertex_contour, best_vertex_id);
            float best_distance = nearest(best_point, position, vertex_contour);
            for (++I; I != E; ++I)
            {
                if ((*I).position().xz() != test_xz)
                    break;

                u32 vertex_id = u32(I - B);
                Fvector point;
                contour(vertex_contour, vertex_id);
                float distance = nearest(point, position, vertex_contour);
                if (distance >= best_distance)
                    continue;

                best_point = point;
                best_distance = distance;
                best_vertex_id = vertex_id;
            }

            if (_abs(best_point.y - position.y) >= 3.f)
                continue;

            if (result_distance <= best_distance)
                continue;

            result_distance = best_distance;
            result_vertex_id = best_vertex_id;
        }
    }

    return (result_vertex_id);
}

// 34: guess_vertex_id with one binary search per row instead of one per cell. The cells of a row
// come in the order of their xz, so the lower_bound of the next cell is the first vertex at or after the current one:
// a step forward gives the same vertex as the old search over all the vertices. When the row ends, no cell after it has
// vertices: the old loop would only `continue`. The rest of the body is the old one.
u32 CLevelGraph::guess_vertex_id_fast(u32 current_vertex_id, Fvector const& position) const
{
    VERIFY(valid_vertex_id(current_vertex_id));

    CPosition vertex_position;
    if (valid_vertex_position(position))
        vertex_position = this->vertex_position(position);
    else
        vertex_position = vertex(current_vertex_id)->position();

    u32 x, z;
    unpack_xz(vertex_position, x, z);

    SContour vertex_contour;
    contour(vertex_contour, current_vertex_id);
    Fvector best_point;
    float result_distance = nearest(best_point, position, vertex_contour);
    u32 result_vertex_id = current_vertex_id;

    CLevelVertex const* B = m_nodes->begin();
    u32 start_x = (u32)std::max(0, int(x) - max_guess_vertex_count);
    u32 stop_x = std::min(max_x(), x + (u32)max_guess_vertex_count);
    u32 start_z = (u32)std::max(0, int(z) - max_guess_vertex_count);
    u32 stop_z = std::min(max_z(), z + (u32)max_guess_vertex_count);
    for (u32 i = start_x; i <= stop_x; ++i)
    {
        CLevelVertex const* row_begin = B;
        CLevelVertex const* row_end = m_nodes->end();
        if (size_t(i) + 1 < m_row_start.size())
        {
            row_begin = B + m_row_start[i];
            row_end = B + m_row_start[i + 1];
        }
        CLevelVertex const* I = std::lower_bound(row_begin, row_end, i * m_row_length + start_z);
        for (u32 j = start_z; j <= stop_z; ++j)
        {
            u32 test_xz = i * m_row_length + j;
            while (I != row_end && (*I).position().xz() < test_xz)
                ++I;
            if (I == row_end)
                break;

            if ((*I).position().xz() != test_xz)
                continue;

            u32 best_vertex_id = u32(I - B);
            contour(vertex_contour, best_vertex_id);
            float best_distance = nearest(best_point, position, vertex_contour);
            for (++I; I != row_end; ++I)
            {
                if ((*I).position().xz() != test_xz)
                    break;

                u32 vertex_id = u32(I - B);
                Fvector point;
                contour(vertex_contour, vertex_id);
                float distance = nearest(point, position, vertex_contour);
                if (distance >= best_distance)
                    continue;

                best_point = point;
                best_distance = distance;
                best_vertex_id = vertex_id;
            }

            if (_abs(best_point.y - position.y) >= 3.f)
                continue;

            if (result_distance <= best_distance)
                continue;

            result_distance = best_distance;
            result_vertex_id = best_vertex_id;
        }
    }

    return (result_vertex_id);
}

// 34: the answer of the full search vertex(position) - the smallest distance(), the lowest id of
// equal ones - without trying every vertex. distance() is the squared distance to the four edges of the vertex contour,
// whose points lie in the vertex cell horizontally, so the squared horizontal gap between the position and the cell is
// a lower bound of it. Rows are taken from the one nearest to the position both ways, a side stops at the first row
// whose gap exceeds the best distance so far (the gap grows with every next row); in a row the vertices are sorted by
// z, so the same from the z nearest to the position. Margins: the cell 1 cm wider for the float contours, 1e-5 of
// the best for the rounding of distance(). Every vertex that can be at the best distance is tried, the result is the
// old one. Returns false when nothing finite was found (a position with NaN/inf): the old loop then decides.
bool CLevelGraph::vertex_nearest_fast(const Fvector& position, u32& result) const
{
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
        return false;
    const u32 rows = u32(m_row_start.size() - 1);
    const double cell = header().cell_size();
    const double half = cell * .5 + .01; // the cell taken 1 cm wider: the bound stays below distance()
    const Fvector& box_min = header().box().vMin;
    const double px = double(position.x) - double(box_min.x); // metres from the box corner; cell i is centred at i*cell
    const double pz = double(position.z) - double(box_min.z);
    const auto gap = [cell, half](u32 i, double p) {
        const double d = std::abs(double(i) * cell - p) - half;
        return d > 0. ? d * d : 0.;
    };
    const auto pruned = [](double bound, float best) { return bound > double(best) * (1. + 1e-5); };
    const auto nearest_index = [cell](double p, u32 count) {
        const double i = std::floor(p / cell + .5);
        return i <= 0. ? 0u : i >= double(count - 1) ? count - 1 : u32(i);
    };

    float best = flt_max;
    u32 best_id = u32(-1);
    CLevelVertex const* B = m_nodes->begin();
    const auto test = [&](CLevelVertex const* I) {
        const u32 id = u32(I - B);
        const float dist = distance(id, position);
        if (dist < best || (dist == best && id < best_id))
        {
            best = dist;
            best_id = id;
        }
    };
    const u32 z0 = nearest_index(pz, m_row_length);
    const auto scan_row = [&](u32 x, double gx) {
        CLevelVertex const* row_begin = B + m_row_start[x];
        CLevelVertex const* row_end = B + m_row_start[x + 1];
        CLevelVertex const* split = std::lower_bound(row_begin, row_end, x * m_row_length + z0);
        for (CLevelVertex const* I = split; I != row_end; ++I)
        {
            if (pruned(gx + gap(I->position().xz() % m_row_length, pz), best))
                break;
            test(I);
        }
        for (CLevelVertex const* I = split; I != row_begin;)
        {
            --I;
            if (pruned(gx + gap(I->position().xz() % m_row_length, pz), best))
                break;
            test(I);
        }
    };

    const u32 x0 = nearest_index(px, rows);
    scan_row(x0, gap(x0, px));
    bool down = x0 > 0, up = x0 + 1 < rows;
    for (u32 k = 1; down || up; ++k)
    {
        if (down)
        {
            const u32 x = x0 - k;
            const double gx = gap(x, px);
            if (pruned(gx, best))
                down = false;
            else
            {
                scan_row(x, gx);
                down = x > 0;
            }
        }
        if (up)
        {
            const u32 x = x0 + k;
            const double gx = gap(x, px);
            if (pruned(gx, best))
                up = false;
            else
            {
                scan_row(x, gx);
                up = x + 1 < rows;
            }
        }
    }
    if (best_id == u32(-1))
        return false;
    result = best_id;
    return true;
}

const CLevelGraph::SearchStats& CLevelGraph::last_search_stats() { return tls_search_stats; }

CLevelGraph::SearchSlice*& CLevelGraph::search_slice() { return tls_search_slice; }

bool CLevelGraph::search_put_aside()
{
    const SearchSlice* const slice = tls_search_slice;
    return slice && slice->put_aside;
}

// 33-level-path.md (plans/optimization_mt), M: a Tracy zone, the level_path/astar_* metrics and SearchStats around the
// implementation. An ordinary zone, not PERF_HOT_ZONE: a few searches a frame at most, each tens of us to tens of
// ms, so the zone costs nothing next to them, and hiding it with perf_metrics bit 4 would hide what is being measured.
//
// The A* (item 0, always on since 2026-10-18; the A* ported from ixray, commit 79545a258, removed with its switch
// ai_level_search_fast): the ixray A* step by step - the same checks in the same order, the same float expressions,
// the same heap calls - on faster data (search_begin / search_loop above):
// - the costs and parents in SearchNodeTable (one probe sequence per neighbour instead of 2-3 tree searches and a heap
//   allocation per opened vertex);
// - a popped vertex already expanded with its current cost is skipped. The old code expanded it again, which can open
//   nothing: every neighbour then has a cost <= the new one, or was refused by the cap (the table never shrinks), the
//   range (the same heuristic) or the mask (unchanged during a search), and max_iteration_count ended the loop if it
//   cut the first expansion. The entry is still popped, so the heap sees the same calls. A cost improvement clears
//   the mark, and the first pop of the vertex after it expands it with the new cost - as in the old code, whichever of
//   its entries comes first;
// - the heap is a thread-local vector with push_heap/pop_heap (std::priority_queue is defined by them), cleared
//   instead of rebuilt;
// - the path is collected with push_back and reversed (insert(begin()) was O(L^2)).
// Hence the same expansion order, the same visited count and the same path.
//
// Sliced A* (33-level-path.md, journal; SearchSlice in level_graph.h): with search_slice() set and `allow`, the loop
// may stop at its head once the deadline passed. The state (heap, table, iterations left, cap_hit) moves to a slot by
// a swap of the buffers; a later call with the same call - graph, generation, vertices, range and limits, and the
// same access mask by m_mask_signature - continues it. Nothing else feeds the A*, so the continued search pops,
// opens and pushes exactly what the whole one would: the same visited count and the same path, only later. No slot
// free: the search runs to the end, as before.
bool CLevelGraph::Search(u32 start_vertex_id, u32 dest_vertex_id, xr_vector<u32>& out_path, float max_range,
    u32 max_iteration_count, u32 max_visited_node_count) const
{
    ZoneScopedN("level_graph_search");
    const u64 t0 = xray::perf::Ticks();
    SearchSlice* const slice = tls_search_slice;
    const bool sliced = slice && slice->allow;

    SearchStats& stats = tls_search_stats;
    ++stats.seq;
    stats.visited_limit = max_visited_node_count;
    stats.fast = true;

    if (sliced && slice->put_aside)
    {
        // a Search of this slice is put aside already: the caller drops the build and repeats it later
        stats.visited = 0;
        stats.ticks = 0;
        stats.success = false;
        stats.cap_hit = false;
        stats.put_aside = true;
        stats.slice = 0;
        return false;
    }

    SearchRun* run = &tls_search_run;
    SearchEnd end = SearchEnd::failed;
    bool resumed = false;
    bool no_slot = false;
    u32 dropped = 0;
    if (!sliced)
    {
        const int head = search_begin(*this, *run, m_search_generation, 0, start_vertex_id, dest_vertex_id, out_path,
            max_range, max_iteration_count, max_visited_node_count);
        end = head >= 0 ? (head ? SearchEnd::found : SearchEnd::failed) : search_loop(*this, *run, out_path, 0);
    }
    else
    {
        const u64 signature = mask_signature();
        SearchRun* slot = nullptr;
        for (SearchRun& it : tls_search_slots)
        {
            if (it.busy &&
                it.same_call(this, m_search_generation, signature, start_vertex_id, dest_vertex_id, max_range,
                    max_iteration_count, max_visited_node_count))
            {
                slot = &it;
                break;
            }
        }
        // the owner has another call now: its older search put aside is not going to be continued
        for (SearchRun& it : tls_search_slots)
        {
            if (it.busy && &it != slot && it.owner == slice->owner)
            {
                it.busy = false;
                ++dropped;
            }
        }

        if (slot)
        {
            resumed = true;
            run = slot;
            end = search_loop(*this, *run, out_path, slice->deadline);
        }
        else
        {
            const int head = search_begin(*this, *run, m_search_generation, signature, start_vertex_id,
                dest_vertex_id, out_path, max_range, max_iteration_count, max_visited_node_count);
            if (head >= 0)
                end = head ? SearchEnd::found : SearchEnd::failed;
            else
            {
                run->slices = 0;
                run->ticks = 0;
                end = search_loop(*this, *run, out_path, slice->deadline);
                if (end == SearchEnd::aside)
                {
                    SearchRun* free_slot = nullptr;
                    for (SearchRun& it : tls_search_slots)
                    {
                        if (!it.busy)
                        {
                            free_slot = &it;
                            break;
                        }
                    }
                    if (!free_slot)
                    {
                        for (SearchRun& it : tls_search_slots)
                        {
                            if (slice->frame - it.frame >= search_slot_stale_frames)
                            {
                                free_slot = &it;
                                ++dropped;
                                break;
                            }
                        }
                    }
                    if (free_slot)
                    {
                        std::swap(*run, *free_slot); // the run gets the slot's buffers for the next search
                        run->busy = false;
                        run = free_slot;
                    }
                    else
                    {
                        no_slot = true;
                        end = search_loop(*this, *run, out_path, 0); // every slot busy: to the end, as before
                    }
                }
            }
        }
    }

    const u64 ticks = xray::perf::Ticks() - t0;
    u32 slice_number = 1;
    u64 total_ticks = ticks;
    if (run != &tls_search_run) // a slot: continued, or this search just put aside
    {
        ++run->slices;
        run->ticks += ticks;
        slice_number = run->slices;
        total_ticks = run->ticks;
        if (end == SearchEnd::aside)
        {
            run->busy = true;
            run->owner = slice->owner;
            run->frame = slice->frame;
            slice->put_aside = true;
        }
        else
            run->busy = false;
    }

    const u32 visited = run->nodes.size();
    const bool result = end == SearchEnd::found;
    ZoneValue(visited);

    stats.visited = visited;
    stats.ticks = ticks;
    stats.success = result;
    stats.cap_hit = run->cap_hit;
    stats.put_aside = end == SearchEnd::aside;
    stats.slice = slice_number;
    stats.pops = run->pops;

#if XR_PERF_METRICS
    // level_path/astar_us (calls = A* calls, a slice each), astar_calls, astar_fail, astar_cap_hit, astar_max_us and
    // astar_visited_max (per-frame maxima), fast_storage = searches on the fast data; every thread (the monsters
    // search synchronously). Sliced A*: astar_put_aside / astar_resumed = slices that stopped / continued a search,
    // astar_no_slot = searches over their slice that ran to the end (every slot busy), astar_dropped = searches put
    // aside and never finished (their owner went on to another call or away), astar_slices_max / astar_sliced_us_max
    // = slices and the time of all of them of a sliced search that finished in the frame (per-frame maxima),
    // astar_pops_max = heap pops of a search that finished in the frame (all its slices; per-frame maximum)
    if (xray::perf::g_perf_metrics_active)
    {
        xray::perf::AddTimeSlow(xray::perf::Metric::level_path_astar_us, ticks);
        PERF_INC(level_path_astar_calls);
        PERF_ADD_IF(end == SearchEnd::failed, level_path_astar_fail, 1);
        PERF_ADD_IF(end != SearchEnd::aside && run->cap_hit, level_path_astar_cap_hit, 1);
        PERF_INC(level_path_fast_storage);
        xray::perf::UpdateMax(xray::perf::Metric::level_path_astar_visited_max, static_cast<s64>(visited));
        xray::perf::UpdateMax(xray::perf::Metric::level_path_astar_max_us,
            static_cast<s64>(xray::perf::TicksToMilliseconds(ticks) * 1000.0));
        PERF_ADD_IF(end == SearchEnd::aside, level_path_astar_put_aside, 1);
        PERF_ADD_IF(resumed, level_path_astar_resumed, 1);
        PERF_ADD_IF(no_slot, level_path_astar_no_slot, 1);
        PERF_ADD_IF(dropped != 0, level_path_astar_dropped, dropped);
        if (end != SearchEnd::aside)
            xray::perf::UpdateMax(xray::perf::Metric::level_path_astar_pops_max, static_cast<s64>(run->pops));
        if (end != SearchEnd::aside && slice_number > 1)
        {
            xray::perf::UpdateMax(xray::perf::Metric::level_path_astar_slices_max, static_cast<s64>(slice_number));
            xray::perf::UpdateMax(xray::perf::Metric::level_path_astar_sliced_us_max,
                static_cast<s64>(xray::perf::TicksToMilliseconds(total_ticks) * 1000.0));
        }
    }
#else
    (void)resumed;
    (void)no_slot;
    (void)dropped;
    (void)total_ticks;
#endif
    return result;
}

u32 CLevelGraph::SearchNearestVertex(u32 vertex_id, const Fvector& target_position, float range) const
{
    thread_local xr_map<u32, u32> temp_came_from;
    thread_local xr_map<u32, float> temp_cost_so_far;

    const float cell = header().cell_size();
    float best_distance_to_target = flt_max;

    tls_nearest_open_pq = OpenPQ{};
    temp_came_from.clear();
    temp_cost_so_far.clear();

    u32 from_id = vertex_id;
    u32 best_result = vertex_id;

    u32 x0, y0;
    unpack_xz(vertex(vertex_id), x0, y0);

    const int max_range_sqr = iFloor(_sqr(range) / _sqr(cell) + .5f);

    tls_nearest_open_pq.push({0.f, from_id});
    temp_came_from.insert({from_id, from_id});
    temp_cost_so_far.insert({from_id, 0.f});

    auto calc_cost = [cell](CLevelVertex* node1, CLevelVertex* node2) { return cell; };
    auto is_accessible_fn = [this, x0, y0, max_range_sqr](u32 node_id)
    {
        if (!is_accessible(node_id))
            return false;
        int x4, y4;
        unpack_xz(vertex(node_id), x4, y4);
        return static_cast<u32>(_sqr(int(x0) - x4) + _sqr(int(y0) - y4)) <= static_cast<u32>(max_range_sqr);
    };

    while (!tls_nearest_open_pq.empty())
    {
        const u32 current_node_id = tls_nearest_open_pq.top().second;
        tls_nearest_open_pq.pop();

        const float current_distance = target_position.distance_to_xz_sqr(vertex_position(current_node_id));
        if (current_distance < best_distance_to_target)
        {
            best_distance_to_target = current_distance;
            best_result = current_node_id;
        }

        CLevelVertex* node = vertex(current_node_id);

        const_iterator i, e;
        begin(node, i, e);
        for (; i != e; ++i)
        {
            const u32 neighbor_id = value(node, i);
            if (!is_accessible_fn(neighbor_id))
                continue;

            CLevelVertex* neighbor = vertex(neighbor_id);
            const float new_cost = temp_cost_so_far[current_node_id] + calc_cost(node, neighbor);
            auto temp_cost_it = temp_cost_so_far.find(neighbor_id);
            if ((temp_cost_it != temp_cost_so_far.end() && temp_cost_it->second > new_cost) ||
                (temp_cost_it == temp_cost_so_far.end()))
            {
                if (temp_cost_it != temp_cost_so_far.end())
                    temp_cost_it->second = new_cost;
                else
                    temp_cost_so_far.insert({neighbor_id, new_cost});

                const float priority = new_cost;
                tls_nearest_open_pq.push({priority, neighbor_id});

                auto came_it = temp_came_from.find(neighbor_id);
                if (came_it != temp_came_from.end())
                    came_it->second = current_node_id;
                else
                    temp_came_from.insert({neighbor_id, current_node_id});
            }
        }
    }

    return best_result;
}
