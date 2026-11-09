#pragma once

// Myers O(ND) diff in linear space and the three-way merge of line sequences (15-merge-implementation.md section 3.3).

#include "ContentMerge.h"

#include <string>
#include <string_view>
#include <vector>

namespace xray::content_merge::diff
{
// a[a_begin, a_end) is replaced by b[b_begin, b_end); an empty a range is an insertion, an empty b range a deletion
struct Hunk
{
    std::uint32_t a_begin{};
    std::uint32_t a_end{};
    std::uint32_t b_begin{};
    std::uint32_t b_end{};
};

// The work limit of one diff: the steps of the search of Myers (a diagonal tried, an element matched along it). The
// cost is O((N + M) * D): two different texts of 30k lines took 2.2 s without it. Over the limit the diff is not
// computed and Merge3 gives a conflict: the atom (the file when it is merged by lines) as a whole goes to the winner,
// as any other conflict. ~0.1 s at most; the limit is an edit distance of ~10k lines (inserted + deleted, spread over
// the text): 4000 lines of 30k replaced merge in 70 ms
constexpr std::uint64_t kDiffMaxWork = 50'000'000;

// The hunks between the matched elements of a minimal edit script (the middle snake of Myers, linear space).
// Consecutive hunks are separated by one matched element at least.
std::vector<Hunk> Myers(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b);
// The same within a work limit: false - over it (hunks is not set)
bool Myers(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b, std::uint64_t max_work,
    std::vector<Hunk>& hunks);

// A line of a text with its terminator ('\n'; the last line of a text may have none) and the side it comes from
struct Line
{
    std::string_view text;
    int owner{};
};

using Lines = std::vector<Line>;

Lines SplitLines(std::string_view text, int owner);
std::string JoinLines(const Lines& lines);

struct Merge3Result
{
    Lines lines;
    bool conflict{}; // some change of a overlaps a different change of b: lines is empty
    bool over_limit{}; // the conflict: a diff is over kDiffMaxWork (the texts too different to be merged by lines)
    std::uint32_t adjacent{}; // the changes of a and b that touch but do not overlap (merged)
};

// The three-way merge of a and b against base. Lines are compared trimmed with the runs of spaces collapsed when
// ignore_spaces (a new indent is not a change); a change takes the lines of its side, an unchanged line - the line of
// the base. Changes of both sides that give the same lines - one change. merge_adjacent: the touching changes are
// merged (git takes them for a conflict); a touching change that is a part of the other one (an insertion at its edge
// of the lines it puts there) - the larger one once, touching changes that repeat a line (not blank) otherwise - a
// conflict, not the line twice. union_insertions: the lines both sides insert at one place are all kept, a's
// first (the code between the atoms: two addons append their calls), an adjacent change; else a conflict.
Merge3Result Merge3(const Lines& base, const Lines& a, const Lines& b, bool ignore_spaces, bool merge_adjacent,
    bool union_insertions = false);
} // namespace xray::content_merge::diff
