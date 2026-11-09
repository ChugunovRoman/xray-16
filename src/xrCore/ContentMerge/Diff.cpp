// Myers diff (the middle snake, linear space) and the three-way merge of line sequences.
// 15-merge-implementation.md section 3.3: the lines are compared normalized (trimmed, the spaces collapsed), the
// adjacent changes of two sides are merged with a mark.

#include "Diff.h"
#include "TextNorm.h"

#include <algorithm>
#include <unordered_map>

namespace xray::content_merge::diff
{
namespace
{
using Ids = std::vector<std::uint32_t>;

// The matched pairs (index in a, index in b) of the edit script, in order
struct MyersState
{
    const Ids& a;
    const Ids& b;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> matches;
    std::vector<int> v1;
    std::vector<int> v2;
    std::uint64_t max_work{~std::uint64_t(0)};
    std::uint64_t work{};
    bool over{}; // over max_work: the search stopped, the matches are not complete

    bool Spend(std::uint64_t steps)
    {
        work += steps;
        over = over || work > max_work;
        return !over;
    }

    void Run(std::uint32_t a_lo, std::uint32_t a_hi, std::uint32_t b_lo, std::uint32_t b_hi);
    bool Bisect(std::uint32_t a_lo, std::uint32_t a_hi, std::uint32_t b_lo, std::uint32_t b_hi, std::uint32_t& x,
        std::uint32_t& y);
};

void MyersState::Run(std::uint32_t a_lo, std::uint32_t a_hi, std::uint32_t b_lo, std::uint32_t b_hi)
{
    if (over)
        return;
    // The common prefix
    while (a_lo < a_hi && b_lo < b_hi && a[a_lo] == b[b_lo])
        matches.emplace_back(a_lo++, b_lo++);
    // The common suffix: recorded after the middle
    std::uint32_t suffix = 0;
    while (a_lo < a_hi - suffix && b_lo < b_hi - suffix && a[a_hi - suffix - 1] == b[b_hi - suffix - 1])
        ++suffix;
    const std::uint32_t a_mid = a_hi - suffix;
    const std::uint32_t b_mid = b_hi - suffix;
    if (a_lo < a_mid && b_lo < b_mid)
    {
        std::uint32_t x;
        std::uint32_t y;
        if (Bisect(a_lo, a_mid, b_lo, b_mid, x, y) && !over)
        {
            Run(a_lo, x, b_lo, y);
            Run(x, a_mid, y, b_mid);
        }
    }
    for (std::uint32_t k = 0; k < suffix; ++k)
        matches.emplace_back(a_mid + k, b_mid + k);
}

// The middle snake of a[a_lo, a_hi) and b[b_lo, b_hi) (both not empty, no common prefix / suffix): the split point.
// false - nothing in common.
bool MyersState::Bisect(std::uint32_t a_lo, std::uint32_t a_hi, std::uint32_t b_lo, std::uint32_t b_hi,
    std::uint32_t& out_x, std::uint32_t& out_y)
{
    const int n = int(a_hi - a_lo);
    const int m = int(b_hi - b_lo);
    const int max_d = (n + m + 1) / 2;
    const int v_offset = max_d;
    const int v_length = 2 * max_d + 2;
    v1.assign(std::size_t(v_length), -1);
    v2.assign(std::size_t(v_length), -1);
    v1[std::size_t(v_offset + 1)] = 0;
    v2[std::size_t(v_offset + 1)] = 0;
    const int delta = n - m;
    const bool front = (delta % 2) != 0;
    int k1start = 0;
    int k1end = 0;
    int k2start = 0;
    int k2end = 0;
    const std::uint32_t* pa = a.data() + a_lo;
    const std::uint32_t* pb = b.data() + b_lo;
    for (int d = 0; d < max_d; ++d)
    {
        // the diagonals of this d, both directions (the matches along them are counted below)
        if (!Spend(std::uint64_t(2 * d + 2)))
            return false;
        for (int k1 = -d + k1start; k1 <= d - k1end; k1 += 2)
        {
            const int k1_offset = v_offset + k1;
            int x1;
            if (k1 == -d || (k1 != d && v1[std::size_t(k1_offset - 1)] < v1[std::size_t(k1_offset + 1)]))
                x1 = v1[std::size_t(k1_offset + 1)];
            else
                x1 = v1[std::size_t(k1_offset - 1)] + 1;
            int y1 = x1 - k1;
            const int x1_from = x1;
            while (x1 < n && y1 < m && pa[x1] == pb[y1])
            {
                ++x1;
                ++y1;
            }
            work += std::uint64_t(x1 - x1_from);
            v1[std::size_t(k1_offset)] = x1;
            if (x1 > n)
                k1end += 2;
            else if (y1 > m)
                k1start += 2;
            else if (front)
            {
                const int k2_offset = v_offset + delta - k1;
                if (k2_offset >= 0 && k2_offset < v_length && v2[std::size_t(k2_offset)] != -1)
                {
                    const int x2 = n - v2[std::size_t(k2_offset)];
                    if (x1 >= x2)
                    {
                        out_x = a_lo + std::uint32_t(x1);
                        out_y = b_lo + std::uint32_t(y1);
                        return true;
                    }
                }
            }
        }
        for (int k2 = -d + k2start; k2 <= d - k2end; k2 += 2)
        {
            const int k2_offset = v_offset + k2;
            int x2;
            if (k2 == -d || (k2 != d && v2[std::size_t(k2_offset - 1)] < v2[std::size_t(k2_offset + 1)]))
                x2 = v2[std::size_t(k2_offset + 1)];
            else
                x2 = v2[std::size_t(k2_offset - 1)] + 1;
            int y2 = x2 - k2;
            const int x2_from = x2;
            while (x2 < n && y2 < m && pa[n - x2 - 1] == pb[m - y2 - 1])
            {
                ++x2;
                ++y2;
            }
            work += std::uint64_t(x2 - x2_from);
            v2[std::size_t(k2_offset)] = x2;
            if (x2 > n)
                k2end += 2;
            else if (y2 > m)
                k2start += 2;
            else if (!front)
            {
                const int k1_offset = v_offset + delta - k2;
                if (k1_offset >= 0 && k1_offset < v_length && v1[std::size_t(k1_offset)] != -1)
                {
                    const int x1 = v1[std::size_t(k1_offset)];
                    const int y1 = v_offset + x1 - k1_offset;
                    if (x1 >= n - x2)
                    {
                        out_x = a_lo + std::uint32_t(x1);
                        out_y = b_lo + std::uint32_t(y1);
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

// The lines as ids: equal (by the compared form) lines get one id
struct Interner
{
    std::unordered_map<std::string, std::uint32_t> ids;
    bool ignore_spaces;

    Ids Of(const Lines& lines)
    {
        Ids out;
        out.reserve(lines.size());
        for (const Line& line : lines)
        {
            std::string key;
            if (ignore_spaces)
            {
                const String collapsed = text::CollapseSpaces(line.text);
                key.assign(collapsed.data(), collapsed.size());
            }
            else
                key.assign(line.text.data(), line.text.size());
            const auto it = ids.emplace(std::move(key), std::uint32_t(ids.size())).first;
            out.push_back(it->second);
        }
        return out;
    }
};

struct SideHunk
{
    Hunk h;
    int side; // 0 - a, 1 - b
};

bool RangesOverlap(const Hunk& x, const Hunk& y, bool union_insertions)
{
    const bool x_empty = x.a_begin == x.a_end;
    const bool y_empty = y.a_begin == y.a_end;
    if (x_empty && y_empty)
        return !union_insertions && x.a_begin == y.a_begin;
    if (x_empty)
        return y.a_begin < x.a_begin && x.a_begin < y.a_end;
    if (y_empty)
        return x.a_begin < y.a_begin && y.a_begin < x.a_end;
    return x.a_begin < y.a_end && y.a_begin < x.a_end;
}

bool RangesTouch(const Hunk& x, const Hunk& y, bool union_insertions)
{
    if (RangesOverlap(x, y, union_insertions))
        return false;
    const bool x_empty = x.a_begin == x.a_end;
    const bool y_empty = y.a_begin == y.a_end;
    if (x_empty && y_empty)
        return union_insertions && x.a_begin == y.a_begin;
    return x.a_end == y.a_begin || y.a_end == x.a_begin || (x_empty && x.a_begin == y.a_begin) ||
        (y_empty && y.a_begin == x.a_begin);
}

// The lines of one side over base[from, to): its hunks inside the range applied
void SideRegion(const Lines& base, const Lines& side, const std::vector<SideHunk>& hunks, int which,
    std::uint32_t from, std::uint32_t to, Lines& out)
{
    std::uint32_t pos = from;
    for (const SideHunk& sh : hunks)
    {
        if (sh.side != which)
            continue;
        for (; pos < sh.h.a_begin; ++pos)
            out.push_back(base[pos]);
        for (std::uint32_t k = sh.h.b_begin; k < sh.h.b_end; ++k)
            out.push_back(side[k]);
        pos = sh.h.a_end;
    }
    for (; pos < to; ++pos)
        out.push_back(base[pos]);
}

// The lines a hunk puts in place of its range of the base (as ids)
Ids NewIds(const SideHunk& sh, const Ids& a_ids, const Ids& b_ids)
{
    const Ids& ids = sh.side == 0 ? a_ids : b_ids;
    return Ids(ids.begin() + sh.h.b_begin, ids.begin() + sh.h.b_end);
}

// y is a part of the change x (two touching hunks of different sides): y inserts at an edge of the range of x the
// lines x puts at that edge - the first ones of x at its start, the last ones at its end (x an insertion at the same
// place: either). Merged side by side, those lines would be there twice. 0 - not, 1 - at the start, 2 - at the end
int ContainedAtEdge(const SideHunk& y, const Ids& y_new, const SideHunk& x, const Ids& x_new)
{
    if (y.h.a_begin != y.h.a_end || y_new.empty() || y_new.size() > x_new.size())
        return 0;
    if (y.h.a_begin == x.h.a_begin && std::equal(y_new.begin(), y_new.end(), x_new.begin()))
        return 1;
    return y.h.a_begin == x.h.a_end && std::equal(y_new.rbegin(), y_new.rend(), x_new.rbegin()) ? 2 : 0;
}

bool BlankLine(std::string_view line)
{
    return line.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

// The prefix function (Knuth-Morris-Pratt) of head, a separator and tail: the longest border at each element
std::vector<std::size_t> Borders(const Ids& head, const Ids& tail)
{
    Ids s(head);
    s.push_back(~std::uint32_t(0)); // no line has this id
    s.insert(s.end(), tail.begin(), tail.end());
    std::vector<std::size_t> pi(s.size());
    for (std::size_t i = 1; i < s.size(); ++i)
    {
        std::size_t k = pi[i - 1];
        while (k && s[i] != s[k])
            k = pi[k - 1];
        pi[i] = k + (s[i] == s[k] ? 1 : 0);
    }
    return pi;
}

// The largest k: the last k elements of tail are the first k of head (0 - none)
std::size_t Overlap(const Ids& head, const Ids& tail)
{
    return head.empty() || tail.empty() ? 0 : Borders(head, tail).back();
}

// small is a run of large (small not empty)
bool HasRun(const Ids& large, const Ids& small)
{
    const std::vector<std::size_t> pi = Borders(small, large);
    return std::find(pi.begin() + std::ptrdiff_t(small.size()), pi.end(), small.size()) != pi.end();
}

// The touching hunks first (before second in the text) of different sides repeat a line that is not blank: the
// last lines of first are the first lines of second (or the first lines of first the last ones of second), both
// start (end) at one place of the base with the same lines, or the lines of one are a run inside the lines of the
// other. Two changes side by side that share a line elsewhere (not at the seam, not at their common edge) are merged.
// Linear in the lines of the two hunks
bool RepeatAcross(const SideHunk& first, const Ids& first_new, const SideHunk& second, const Ids& second_new,
    const Lines& a, const Lines& b)
{
    // a line that is not blank among the new lines [from, from + count) of a hunk
    const auto any_text = [&](const SideHunk& sh, std::size_t from, std::size_t count)
    {
        const Lines& lines = sh.side == 0 ? a : b;
        for (std::size_t k = from; k < from + count; ++k)
        {
            if (!BlankLine(lines[sh.h.b_begin + k].text))
                return true;
        }
        return false;
    };
    // Both at one edge (an insertion at the start or at the end of the range of the other one): the same first lines
    // at the start, the same last ones at the end
    const std::size_t shorter = std::min(first_new.size(), second_new.size());
    if (first.h.a_begin == second.h.a_begin)
    {
        std::size_t same = 0;
        while (same < shorter && first_new[same] == second_new[same])
            ++same;
        if (any_text(first, 0, same))
            return true;
    }
    if (first.h.a_end == second.h.a_end)
    {
        std::size_t same = 0;
        while (same < shorter && first_new[first_new.size() - 1 - same] == second_new[second_new.size() - 1 - same])
            ++same;
        if (any_text(first, first_new.size() - same, same))
            return true;
    }
    // The seam: the end of first is the start of second, or the other way round (a longer overlap has more lines,
    // the longest one is enough)
    const std::size_t seam = Overlap(second_new, first_new);
    if (any_text(second, 0, seam))
        return true;
    const std::size_t back = Overlap(first_new, second_new);
    if (any_text(first, 0, back))
        return true;
    // Inside: the lines of the smaller change are a run of the larger one
    const bool first_smaller = first_new.size() <= second_new.size();
    const Ids& small = first_smaller ? first_new : second_new;
    const Ids& large = first_smaller ? second_new : first_new;
    return !small.empty() && any_text(first_smaller ? first : second, 0, small.size()) && HasRun(large, small);
}
} // namespace

std::vector<Hunk> Myers(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b)
{
    std::vector<Hunk> hunks;
    Myers(a, b, ~std::uint64_t(0), hunks);
    return hunks;
}

bool Myers(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b, std::uint64_t max_work,
    std::vector<Hunk>& hunks)
{
    MyersState state{a, b, {}, {}, {}};
    state.max_work = max_work;
    state.matches.reserve(std::min(a.size(), b.size()));
    state.Run(0, std::uint32_t(a.size()), 0, std::uint32_t(b.size()));
    if (state.over)
        return false;
    hunks.clear();
    std::uint32_t ai = 0;
    std::uint32_t bi = 0;
    const auto flush = [&](std::uint32_t a_to, std::uint32_t b_to)
    {
        if (ai < a_to || bi < b_to)
            hunks.push_back({ai, a_to, bi, b_to});
    };
    for (const auto& [x, y] : state.matches)
    {
        flush(x, y);
        ai = x + 1;
        bi = y + 1;
    }
    flush(std::uint32_t(a.size()), std::uint32_t(b.size()));
    return true;
}

Lines SplitLines(std::string_view text_in, int owner)
{
    Lines out;
    std::size_t pos = 0;
    while (pos < text_in.size())
    {
        const std::size_t nl = text_in.find('\n', pos);
        const std::size_t end = nl == std::string_view::npos ? text_in.size() : nl + 1;
        out.push_back({text_in.substr(pos, end - pos), owner});
        pos = end;
    }
    return out;
}

std::string JoinLines(const Lines& lines)
{
    std::string out;
    std::size_t size = 0;
    for (const Line& line : lines)
        size += line.text.size();
    out.reserve(size);
    for (const Line& line : lines)
        out.append(line.text.data(), line.text.size());
    return out;
}

Merge3Result Merge3(const Lines& base, const Lines& a, const Lines& b, bool ignore_spaces, bool merge_adjacent,
    bool union_insertions)
{
    Interner interner{{}, ignore_spaces};
    const Ids base_ids = interner.Of(base);
    const Ids a_ids = interner.Of(a);
    const Ids b_ids = interner.Of(b);

    // Over the work limit: the texts are too different to be merged by lines - one conflict
    std::vector<Hunk> a_hunks;
    std::vector<Hunk> b_hunks;
    if (!Myers(base_ids, a_ids, kDiffMaxWork, a_hunks) || !Myers(base_ids, b_ids, kDiffMaxWork, b_hunks))
    {
        Merge3Result over;
        over.conflict = true;
        over.over_limit = true;
        return over;
    }
    std::vector<SideHunk> all;
    for (const Hunk& h : a_hunks)
        all.push_back({h, 0});
    for (const Hunk& h : b_hunks)
        all.push_back({h, 1});
    std::stable_sort(all.begin(), all.end(), [](const SideHunk& x, const SideHunk& y)
    {
        if (x.h.a_begin != y.h.a_begin)
            return x.h.a_begin < y.h.a_begin;
        if (x.h.a_end != y.h.a_end)
            return x.h.a_end < y.h.a_end;
        return x.side < y.side;
    });

    // The same lines inserted by both at one place: one insertion, not two (the union of insertions)
    const auto same_insertion = [&](const SideHunk& x, const SideHunk& y)
    {
        if (x.h.a_begin != x.h.a_end || y.h.a_begin != y.h.a_end || x.h.a_begin != y.h.a_begin)
            return false;
        const Ids& xi = x.side == 0 ? a_ids : b_ids;
        const Ids& yi = y.side == 0 ? a_ids : b_ids;
        return Ids(xi.begin() + x.h.b_begin, xi.begin() + x.h.b_end) ==
            Ids(yi.begin() + y.h.b_begin, yi.begin() + y.h.b_end);
    };
    const bool union_on = union_insertions && merge_adjacent;

    Merge3Result result;
    // The adjacent changes: the hunks of different sides that touch (in one cluster they would have joined). Merged
    // side by side, a change that contains the other one would give its lines twice (row 64 item 6, 2026-11-07): the
    // smaller one inserts at an edge of the larger one the lines the larger one puts there (ContainedAtEdge) - it is
    // the same change, the larger one is taken once (as two equal changes are one; Merger::Decide folds a text a lower
    // side gave once too); the touching changes repeat a line otherwise (RepeatAcross: the end of one is the start of
    // the other, the same lines at their common edge, one is a run inside the other) - a conflict
    if (merge_adjacent)
    {
        // The larger change folds the smaller one only when it is alone in its cluster: overlapped by a change of
        // the other side, its lines are not the result as they are
        const auto overlapped = [&](std::size_t x)
        {
            for (std::size_t k = 0; k < all.size(); ++k)
            {
                if (all[k].side != all[x].side &&
                    (RangesOverlap(all[k].h, all[x].h, union_on) || (union_on && same_insertion(all[k], all[x]))))
                    return true;
            }
            return false;
        };
        std::vector<bool> folded(all.size());
        // The lines of a larger change that folded an insertion at its start / at its end: one line of it folds one
        // insertion only (it inserts A for both A before and A after the line of the base: A once would lose a copy)
        std::vector<std::size_t> used_start(all.size());
        std::vector<std::size_t> used_end(all.size());
        const auto fold = [&](std::size_t y, const Ids& y_new, std::size_t x, const Ids& x_new)
        {
            const int edge = ContainedAtEdge(all[y], y_new, all[x], x_new);
            if (!edge || (edge == 1 ? used_end[x] : used_start[x]) + y_new.size() > x_new.size() || overlapped(x))
                return false;
            (edge == 1 ? used_start[x] : used_end[x]) = y_new.size();
            folded[y] = true;
            return true;
        };
        for (std::size_t i = 0; i < all.size(); ++i)
        {
            for (std::size_t j = i + 1; j < all.size() && all[j].h.a_begin <= all[i].h.a_end; ++j)
            {
                if (all[i].side == all[j].side || !RangesTouch(all[i].h, all[j].h, union_on) ||
                    (union_on && same_insertion(all[i], all[j])))
                    continue;
                ++result.adjacent;
                const Ids first_new = NewIds(all[i], a_ids, b_ids);
                const Ids second_new = NewIds(all[j], a_ids, b_ids);
                if (fold(i, first_new, j, second_new) || fold(j, second_new, i, first_new))
                    continue;
                if (RepeatAcross(all[i], first_new, all[j], second_new, a, b))
                {
                    Merge3Result repeat;
                    repeat.conflict = true;
                    return repeat;
                }
            }
        }
        // A folded hunk is an insertion that only touches the other side (no hunk of it overlaps an insertion of
        // the side whose hunk it touches): its own cluster, dropped
        std::size_t kept = 0;
        for (std::size_t i = 0; i < all.size(); ++i)
        {
            if (!folded[i])
                all[kept++] = all[i];
        }
        all.resize(kept);
    }

    // Clusters: the hunks of the other side a hunk overlaps (or touches, without merge_adjacent) join its cluster
    std::vector<std::vector<SideHunk>> clusters;
    for (const SideHunk& sh : all)
    {
        bool join = false;
        if (!clusters.empty())
        {
            for (const SideHunk& other : clusters.back())
            {
                if (other.side == sh.side)
                    continue;
                if (RangesOverlap(sh.h, other.h, union_on) || (union_on && same_insertion(sh, other)) ||
                    (!merge_adjacent && RangesTouch(sh.h, other.h, union_on)))
                {
                    join = true;
                    break;
                }
            }
        }
        if (join)
            clusters.back().push_back(sh);
        else
            clusters.push_back({sh});
    }

    std::uint32_t pos = 0;
    for (const std::vector<SideHunk>& cluster : clusters)
    {
        std::uint32_t from = cluster.front().h.a_begin;
        std::uint32_t to = cluster.front().h.a_end;
        bool sides[2] = {};
        for (const SideHunk& sh : cluster)
        {
            from = std::min(from, sh.h.a_begin);
            to = std::max(to, sh.h.a_end);
            sides[sh.side] = true;
        }
        for (; pos < from; ++pos)
            result.lines.push_back(base[pos]);
        if (sides[0] && sides[1])
        {
            Lines la;
            Lines lb;
            SideRegion(base, a, cluster, 0, from, to, la);
            SideRegion(base, b, cluster, 1, from, to, lb);
            const Ids ia = interner.Of(la);
            const Ids ib = interner.Of(lb);
            if (ia != ib)
            {
                result.conflict = true;
                result.lines.clear();
                return result;
            }
            result.lines.insert(result.lines.end(), lb.begin(), lb.end()); // the same change: the upper side
        }
        else
            SideRegion(base, sides[0] ? a : b, cluster, sides[0] ? 0 : 1, from, to, result.lines);
        pos = to;
    }
    for (; pos < base.size(); ++pos)
        result.lines.push_back(base[pos]);
    return result;
}
} // namespace xray::content_merge::diff
