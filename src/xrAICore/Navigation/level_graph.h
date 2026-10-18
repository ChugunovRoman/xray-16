////////////////////////////////////////////////////////////////////////////
//	Module 		: level_graph.h
//	Created 	: 02.10.2001
//  Modified 	: 11.11.2003
//	Author		: Oles Shihkovtsov, Dmitriy Iassenev
//	Description : Level graph
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Common/LevelStructure.hpp"
#include "xrAICore/Navigation/level_graph_space.h"
#include "xrAICore/Navigation/game_graph_space.h"
#include "xrAICore/Navigation/level_graph_manager.h"
#include "xrCore/_plane.h"

#include <atomic>

namespace LevelGraph
{
class CHeader;
class CLevelVertex;
struct SSegment;
struct SContour;

enum ELineIntersections
{
    eLineIntersectionNone = u32(0),
    eLineIntersectionCollinear = u32(0),
    eLineIntersectionIntersect = u32(1),
    eLineIntersectionEqual = u32(2)
};
};

class CCoverPoint;

class XRAICORE_API CLevelGraph
{
private:
    friend class CRenumbererConverter;

public:
    typedef LevelGraph::CPosition CPosition;
    typedef LevelGraph::CHeader CHeader;
    typedef LevelGraph::CLevelVertex CLevelVertex;
    typedef LevelGraph::SSegment SSegment;
    typedef LevelGraph::SContour SContour;
    typedef LevelGraph::ELineIntersections ELineIntersections;

    using CLevelGraphManager = LevelGraph::CLevelGraphManager;

private:
    IReader* m_reader; // level graph virtual storage
    CHeader* m_header; // level graph header
    CLevelGraphManager* m_nodes; // contains nodes array
    xr_vector<bool> m_access_mask;
    // 33-level-path.md (plans/optimization_mt), sliced A*: a hash of the set of the masked vertices (XOR of a mix of
    // their ids, changed only when a bit really flips; 0 = nothing masked), so a sliced A* (SearchSlice) is continued
    // only on the very access mask it started on. Atomic: the XOR stays exact whichever thread flips a bit
    std::atomic<u64> m_mask_signature{0};
    u32 m_search_generation{}; // sliced A*: a number per loaded graph (a put aside A* never goes on on another one)
    GameGraph::_LEVEL_ID m_level_id; // unique level identifier
    u32 m_row_length;
    u32 m_column_length;
    u32 m_max_x;
    u32 m_max_z;
    // 34 (plans/optimization_mt/33-level-path.md, journal): m_row_start[x] = the first vertex of
    // the row x of the packed xz (the vertices are sorted by xz), one more entry for the end; built in Initialize, a few
    // KB. Empty = the vertices are not sorted: the row lookups are off and the old code works
    xr_vector<u32> m_row_start;

public:
    mutable CStatTimer NodeTime;

private:
    u32 vertex(const Fvector& position) const;
    u32 guess_vertex_id(u32 const& current_vertex_id, Fvector const& position) const;

public:
    typedef u32 const_iterator;
    typedef u32 const_spawn_iterator;
    typedef u32 const_death_iterator;
    typedef const CLevelVertex* const_vertex_iterator;

    struct vertex
    {
        static IC bool predicate(const u32& value, const CLevelVertex& vertex) { return (value < vertex.position().xz()); }
        static IC bool predicate2(const CLevelVertex& vertex, const u32& value) { return (vertex.position().xz() < value); }
    };

private:
    void Initialize(const char* filePath);

public:
    CLevelGraph();
    // for ai compiler
    CLevelGraph(const char* fileName);
    virtual ~CLevelGraph();
    IC const_vertex_iterator begin() const;
    IC const_vertex_iterator end() const;

    IC void set_mask(const xr_vector<u32>& mask);
    IC void set_mask_no_check(const xr_vector<u32>& mask);

    IC void set_mask(u32 vertex_id);
    IC void set_mask_no_check(u32 vertex_id);

    IC void clear_mask(const xr_vector<u32>& mask);
    IC void clear_mask_no_check(const xr_vector<u32>& mask);

    IC void clear_mask(u32 vertex_id);
    IC void clear_mask_no_check(u32 vertex_id);

    IC bool is_accessible(const u32 vertex_id) const;
    // sliced A*: the hash of the access mask (m_mask_signature) and the mix of one vertex id in it
    IC u64 mask_signature() const;
    static IC u64 mask_hash(u32 vertex_id);
    IC void level_id(const GameGraph::_LEVEL_ID& level_id);
    IC u32 max_x() const;
    IC u32 max_z() const;
    IC void begin(const CLevelVertex& vertex, const_iterator& begin, const_iterator& end) const;
    IC void begin(const CLevelVertex* vertex, const_iterator& begin, const_iterator& end) const;
    IC void begin(u32 vertex_id, const_iterator& begin, const_iterator& end) const;
    IC u32 value(const CLevelVertex& vertex, const_iterator i) const;
    IC u32 value(const CLevelVertex* vertex, const_iterator i) const;
    IC u32 value(const u32 vertex_id, const_iterator i) const;
    IC const CHeader& header() const;
    ICF bool valid_vertex_id(u32 vertex_id) const;
    IC GameGraph::_LEVEL_ID level_id() const;
    IC void unpack_xz(const CLevelGraph::CPosition& vertex_position, u32& x, u32& z) const;
    IC void unpack_xz(const CLevelGraph::CPosition& vertex_position, int& x, int& z) const;
    IC void unpack_xz(const CLevelGraph::CPosition& vertex_position, float& x, float& z) const;
    template <typename T>
    IC void unpack_xz(const CLevelGraph::CLevelVertex& vertex, T& x, T& z) const;
    template <typename T>
    IC void unpack_xz(const CLevelGraph::CLevelVertex* vertex, T& x, T& z) const;
    ICF CLevelVertex* vertex(u32 vertex_id) const;
    ICF u32 vertex(const CLevelVertex* vertex_p) const;
    ICF u32 vertex(const CLevelVertex& vertex_r) const;
    IC const Fvector vertex_position(const CLevelGraph::CPosition& source_position) const;
    IC const Fvector& vertex_position(Fvector& dest_position, const CLevelGraph::CPosition& source_position) const;
    IC const CLevelGraph::CPosition& vertex_position(
        CLevelGraph::CPosition& dest_position, const Fvector& source_position) const;
    IC const CLevelGraph::CPosition vertex_position(const Fvector& position) const;
    IC const Fvector vertex_position(u32 vertex_id) const;
    IC const Fvector vertex_position(const CLevelVertex& vertex) const;
    IC const Fvector vertex_position(const CLevelVertex* vertex) const;
    IC float vertex_plane_y(const CLevelVertex& vertex, const float X, const float Z) const;
    IC float vertex_plane_y(const CLevelVertex* vertex, const float X, const float Z) const;
    IC float vertex_plane_y(const u32 vertex_id, const float X, const float Z) const;
    IC float vertex_plane_y(const CLevelVertex& vertex) const;
    IC float vertex_plane_y(const CLevelVertex* vertex) const;
    IC float vertex_plane_y(const u32 vertex_id) const;
    IC bool inside(const CLevelVertex& vertex, const CLevelGraph::CPosition& vertex_position) const;
    IC bool inside(const CLevelVertex& vertex, const Fvector& vertex_position) const;
    IC bool inside(const CLevelVertex* vertex, const CLevelGraph::CPosition& vertex_position) const;
    IC bool inside(const CLevelVertex* vertex, const Fvector& vertex_position) const;
    IC bool inside(const u32 vertex_id, const CLevelGraph::CPosition& vertex_position) const;
    IC bool inside(const u32 vertex_id, const Fvector& position) const;
    IC bool inside(const u32 vertex_id, const Fvector2& position) const;
    IC bool inside(const CLevelVertex& vertex, const CLevelGraph::CPosition& vertex_position, const float epsilon) const;
    IC bool inside(const CLevelVertex& vertex, const Fvector& vertex_position, const float epsilon) const;
    IC bool inside(const CLevelVertex* vertex, const CLevelGraph::CPosition& vertex_position, const float epsilon) const;
    IC bool inside(const CLevelVertex* vertex, const Fvector& vertex_position, const float epsilon) const;
    IC bool inside(const u32 vertex_id, const CLevelGraph::CPosition& vertex_position, const float epsilon) const;
    IC bool inside(const u32 vertex_id, const Fvector& position, const float epsilon) const;
    IC void project_point(const Fplane& plane, Fvector& point) const;
    IC u32 row_length() const;
    float distance(const Fvector& position, const CLevelVertex* vertex) const;
    float distance(const Fvector& position, const u32 vertex_id) const;
    float distance(const u32 vertex_id, const Fvector& position) const;
    IC float distance(const Fvector& position, const Fvector& point0, const Fvector& point1) const;
    IC float distance(u32 vertex_id0, u32 vertex_id1) const;
    IC float distance(const CLevelVertex* tpNode0, u32 vertex_id1) const;
    IC float distance(u32 vertex_id0, const CLevelVertex* vertex) const;
    IC float distance(const CLevelVertex* node0, const CLevelVertex* node1) const;
    IC float distance(const u32 vertex_id, const CPosition& position) const;
    IC float distance(const CPosition& position, const u32 vertex_id) const;
    IC ELineIntersections intersect(
        float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float* x, float* y) const;
    IC ELineIntersections intersect_no_check(
        float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float* x, float* y) const;
    IC bool similar(const Fvector& point0, const Fvector& point1) const;
    IC bool inside(const Fvector& point, const SContour& contour) const;
    IC void intersect(SSegment& segment, const SContour& contour0, const SContour& contour1) const;
    IC float nearest(Fvector& destination, const Fvector& position, const Fvector& point0, const Fvector& point1) const;
    IC void contour(SContour& contour, u32 vertex_id) const;
    IC void contour(SContour& contour, const CLevelVertex* vertex) const;
    IC float nearest(Fvector& destination, const Fvector& position, const SContour& contour) const;
    IC bool intersect(
        Fvector& destination, const Fvector& v1, const Fvector& v2, const Fvector& v3, const Fvector& v4) const;
    IC float square(float a1, float b1, float alpha = PI_DIV_2) const;
    IC float compute_square(float angle, float AOV, float b0, float b1, float b2, float b3) const;
    IC float compute_high_square(float angle, float AOV, const CLevelVertex* vertex) const;
    IC float compute_low_square(float angle, float AOV, const CLevelVertex* vertex) const;
    IC float compute_high_square(float angle, float AOV, u32 dwNodeID) const;
    IC float compute_low_square(float angle, float AOV, u32 dwNodeID) const;
    IC float vertex_high_cover(const CLevelGraph::CLevelVertex* vertex) const;
    IC float vertex_low_cover(const CLevelGraph::CLevelVertex* vertex) const;
    IC float vertex_high_cover(const u32 vertex_id) const;
    IC float vertex_low_cover(const u32 vertex_id) const;
    float cover_in_direction(float angle, float b0, float b1, float b2, float b3) const;
    IC float high_cover_in_direction(float angle, const CLevelVertex* vertex) const;
    IC float low_cover_in_direction(float angle, const CLevelVertex* vertex) const;
    IC float high_cover_in_direction(float angle, u32 vertex_id) const;
    IC float low_cover_in_direction(float angle, u32 vertex_id) const;

    template <class _predicate>
    IC float vertex_high_cover_angle(u32 vertex_id, float inc_angle, _predicate compare_predicate) const;
    template <class _predicate>
    IC float vertex_low_cover_angle(u32 vertex_id, float inc_angle, _predicate compare_predicate) const;
    IC void set_invalid_vertex(u32& vertex_id, CLevelVertex** vertex = NULL) const;
    IC u32 vertex_id(const CLevelGraph::CLevelVertex* vertex) const;
    u32 vertex_id(const Fvector& position) const;

    /** 34 (plans/optimization_mt/33-level-path.md, journal): what the last vertex(current, position) of the calling
        thread did (thread-local, written at its end) - for the [perf-spike] go/ai_locations line of xrGame. `seq` grows
        by one per call: a caller compares it to tell whether its code reached vertex(). */
    struct VertexStats
    {
        enum : u8
        {
            none,
            current, // the current vertex is under the position
            xz, // the vertex under the position (binary search by the packed xz)
            full, // the current vertex is invalid and nothing is under the position: every vertex was tried
            full_fast, // the same answer from the rows nearest to the position (34)
            guess, // guess_vertex_id: the 9x9 cells around the position
            nearest, // the current vertex or one of its neighbours
        };
        u32 seq{}; // vertex() calls of this thread so far
        u32 current_id{}; // the current vertex passed in
        u32 result{};
        Fvector position{};
        u64 ticks{}; // xray::perf::Ticks() the call took
        u8 branch{};
        bool fast{}; // always true since 2026-10-18 (was ai_level_vertex_fast), kept for the log lines
        bool fallback{}; // it was on, but the old code answered (no row table, or nothing finite near the position)
    };
    static const VertexStats& last_vertex_stats();
    static const char* vertex_branch_name(u8 branch);

private:
    IC u32 VertexInternal(u32 current_vertex_id, const Fvector& position, VertexStats& stats) const;
    // 34: the lookups on m_row_start, each with the result of the old code
    void build_row_start();
    CLevelVertex* lower_bound_xz(u32 xz) const; // std::lower_bound over all the vertices, in one row when it may
    u32 guess_vertex_id_fast(u32 current_vertex_id, Fvector const& position) const;
    bool vertex_nearest_fast(const Fvector& position, u32& result) const; // the full search vertex(position)

public:
    u32 vertex(u32 current_vertex_id, const Fvector& position) const;
    void choose_point(const Fvector& start_point, const Fvector& finish_point, const SContour& contour, int vertex_id,
        Fvector& temp_point, int& saved_index) const;
    IC bool check_vertex_in_direction(u32 start_vertex_id, const Fvector& start_position, u32 finish_vertex_id) const;
    IC u32 check_position_in_direction(
        u32 start_vertex_id, const Fvector& start_position, const Fvector& finish_position) const;
    float check_position_in_direction(u32 start_vertex_id, const Fvector& start_position,
        const Fvector& finish_position, const float max_distance) const;
    float mark_nodes_in_direction(u32 start_vertex_id, const Fvector& start_position, const Fvector& direction,
        float distance, xr_vector<u32>& vertex_stack, xr_vector<bool>* vertex_marks) const;
    float mark_nodes_in_direction(u32 start_vertex_id, const Fvector& start_position, u32 finish_node,
        xr_vector<u32>& vertex_stack, xr_vector<bool>* vertex_marks) const;
    float mark_nodes_in_direction(u32 start_vertex_id, const Fvector& start_position, const Fvector& finish_point,
        xr_vector<u32>& vertex_stack, xr_vector<bool>* vertex_marks) const;
    float farthest_vertex_in_direction(u32 start_vertex_id, const Fvector& start_point, const Fvector& finish_point,
        u32& finish_vertex_id, xr_vector<bool>* tpaMarks, bool check_accessability = false) const;
    bool create_straight_path(u32 start_vertex_id, const Fvector& start_point, const Fvector& finish_point,
        xr_vector<Fvector>& tpaOutputPoints, xr_vector<u32>& tpaOutputNodes, bool bAddFirstPoint,
        bool bClearPath = true) const;
    bool create_straight_path(u32 start_vertex_id, const Fvector2& start_point, const Fvector2& finish_point,
        xr_vector<Fvector>& tpaOutputPoints, xr_vector<u32>& tpaOutputNodes, bool bAddFirstPoint,
        bool bClearPath = true) const;
    template <bool bAssignY, typename T>
    IC bool create_straight_path(u32 start_vertex_id, const Fvector2& start_point, const Fvector2& finish_point,
        xr_vector<T>& tpaOutputPoints, const T& example, bool bAddFirstPoint, bool bClearPath = true) const;
    template <typename T>
    IC void assign_y_values(xr_vector<T>& path);
    template <typename P>
    IC void iterate_vertices(const Fvector& min_position, const Fvector& max_position, const P& predicate) const;
    IC bool check_vertex_in_direction(u32 start_vertex_id, const Fvector2& start_position, u32 finish_vertex_id) const;
    IC u32 check_position_in_direction(
        u32 start_vertex_id, const Fvector2& start_position, const Fvector2& finish_position) const;
    bool check_vertex_in_direction_slow(
        u32 start_vertex_id, const Fvector2& start_position, u32 finish_vertex_id) const;
    u32 check_position_in_direction_slow(
        u32 start_vertex_id, const Fvector2& start_position, const Fvector2& finish_position) const;
    IC Fvector v3d(const Fvector2& vector2d) const;
    IC Fvector2 v2d(const Fvector& vector3d) const;
    IC bool valid_vertex_position(const Fvector& position) const;
    bool neighbour_in_direction(const Fvector& direction, u32 start_vertex_id) const;

    /** Thread-local scratch grid A* (ixray-style); avoids global `graph_engine` for level-only paths. */
    bool Search(u32 start_vertex_id, u32 dest_vertex_id, xr_vector<u32>& out_path, float max_range = flt_max,
        u32 max_iteration_count = u32(-1), u32 max_visited_node_count = u32(-1)) const;
    u32 SearchNearestVertex(u32 vertex_id, const Fvector& target_position, float range) const;

    /** 33-level-path.md (plans/optimization_mt), M: what the last Search of the calling thread did (thread-local, written
        at its end) - for the [path-spike] line and the level_path/* metrics of xrGame without changing Search's signature.
        `seq` grows by one per Search of the thread: a caller compares it to tell whether its call reached Search. */
    struct SearchStats
    {
        u32 seq{}; // Searches of this thread so far
        u32 visited{}; // vertices that got a cost (the size of the "cost so far" set)
        u32 visited_limit{}; // max_visited_node_count of the call
        u64 ticks{}; // xray::perf::Ticks() the call took
        bool success{};
        bool cap_hit{}; // a vertex was not opened because of max_visited_node_count
        bool fast{}; // always true since 2026-10-18 (was ai_level_search_fast), kept for the log lines
        bool put_aside{}; // sliced A*: the call was put aside on its time slice (success = false means nothing then)
        u32 slice{}; // sliced A*: the slice of the search this call ran (1 = the first one, or the whole search)
        // heap pops of the search so far (all its slices). The x2 Manhattan heuristic is not consistent: a vertex is
        // expanded again whenever a cheaper way to it turns up, so pops / visited is the real work per vertex
        u32 pops{};
    };
    static const SearchStats& last_search_stats();

    /** 33-level-path.md (plans/optimization_mt), sliced A*: a level A* in time slices. While a caller points
        search_slice() of its thread at one, each Search of the thread with `allow` set may stop once Ticks() passes
        `deadline` (checked every 64 vertices) and keep its state in one of a few thread-local slots: it then returns
        false, leaves out_path as it was and
        sets `put_aside` - the search did not fail, the caller repeats the same call later. A later Search with the same
        call (the same graph, vertices and limits on the same access mask, see m_mask_signature) continues it, under
        a new deadline, from where it stopped: the same steps in the same order, so the same visited count and the
        same path as a search that never stopped. A search that finishes within its slice is the plain Search. The
        slots are found by the call, not by the owner: `owner` and `frame` only say whose slot may be taken (a slot of
        another call of the same owner, a slot not continued for a few frames); the owner is never dereferenced. Once
        a Search of the slice is put aside, the next ones return false at once (put aside as well) - the caller has to
        drop the build and repeat it. Every slot busy: the search runs to the end as before. */
    struct SearchSlice
    {
        u64 deadline{}; // in: xray::perf::Ticks() after which a search is put aside (0 = never)
        const void* owner{}; // in: who searches (the slot bookkeeping only)
        u32 frame{}; // in: the frame number of the caller
        bool allow = true; // in: a caller clears it around the searches that must not be put aside
        bool put_aside{}; // out: a Search was put aside
    };
    static SearchSlice*& search_slice(); // of the calling thread; nullptr = every Search runs to the end
    static bool search_put_aside(); // search_slice() is set and a Search of it was put aside

public:

    IC CLevelVertex* vertices() { return m_nodes->begin(); }
};

IC bool operator<(const CLevelGraph::CLevelVertex& vertex, const u32& vertex_xz);
IC bool operator>(const CLevelGraph::CLevelVertex& vertex, const u32& vertex_xz);
IC bool operator==(const CLevelGraph::CLevelVertex& vertex, const u32& vertex_xz);
IC bool operator<(const u32& vertex_xz, const CLevelGraph::CLevelVertex& vertex);
IC bool operator>(const u32& vertex_xz, const CLevelGraph::CLevelVertex& vertex);
IC bool operator==(const u32& vertex_xz, const CLevelGraph::CLevelVertex& vertex);

#if defined(DEBUG) && !defined(AI_COMPILER)
extern BOOL g_bDebugNode;
extern u32 g_dwDebugNodeSource;
extern u32 g_dwDebugNodeDest;
#endif

#include "xrAICore/Navigation/level_graph_inline.h"
#include "xrAICore/Navigation/level_graph_vertex_inline.h"
