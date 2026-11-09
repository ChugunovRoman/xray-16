#pragma once

// The merge of N sides by atoms against one ancestor (15-merge-implementation.md section 3.3), the text assembled from
// the segments: the atoms nobody changed and the gaps come from the ancestor byte to byte, the changed ones from the
// side that changed them.

#include "Segments.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace xray::content_merge::atoms
{
struct Version
{
    std::string_view text;
    const Doc* doc{};
    bool fragment{}; // a patch fragment: an atom it does not have is not touched
    std::vector<std::string> patches; // fragment: the owners whose conflicts it resolves
};

// owner: 0 - the ancestor, k + 1 - the side k
struct Piece
{
    std::string text;
    int owner{};
};

struct ConflictRec
{
    std::string atom;
    std::vector<int> owners;
    int winner{}; // 0 - the ancestor
    std::string resolved_by;
    std::vector<int> lost;
    std::uint32_t base_line{};
    std::string text;
};

struct Outcome
{
    std::vector<Piece> pieces;
    std::uint32_t atoms_merged{};
    std::uint32_t adjacent{};
    std::vector<std::string> adjacent_atoms;
    std::vector<ConflictRec> conflicts;
    std::vector<std::string> reordered;
    std::vector<std::string> rules_applied;
    std::uint32_t unresolved{};

    std::string Text() const;
};

// rules: atom label -> owner index (0 - the ancestor, k + 1 - the side k). The code between the atoms (code_gaps): a
// region (between two atoms of the ancestor every side keeps) two sides or more changed differently (or the same way,
// while their other changes of the code differ) is a conflict as a whole - the region of the winner (AtomMerge.cpp,
// the rule of the top-level code)
Outcome MergeDocs(std::string_view base_text, const Doc& base_doc, const std::vector<Version>& sides,
    const std::map<std::string, int>& rules, const Options& options, const std::vector<std::string>& owner_names);

// The changes of one version against the ancestor, recursively (containers first, then their atoms)
struct ChangeRec
{
    std::string atom;
    AtomOp op{};
    std::uint32_t line{};
    bool container{};
};

void ChangedAtoms(std::string_view base_text, const Doc& base_doc, std::string_view text, const Doc& doc,
    std::vector<ChangeRec>& out);

// The key of a gap: by its anchor - the nearest atom above it in the version that the ancestor has
std::vector<std::string> ListKeys(const std::vector<Seg>& list, const std::set<std::string>* base_keys);
std::string GapLabel(const Seg& seg, const std::string& key);

// S9 (Delta.cpp, ApplyDelta): where the code of a gap of the delta is in a list of the current base that has no gap
// under its key (the base added, deleted or renamed an atom near it, joined two gaps into one). [begin, end) - whole
// lines of one gap of the list equal to `ancestor` (the text of the gap in the ancestor). Found in several places -
// the first one after the atom of the key ("~<atom>#<n>", "^" - the start). false - not found (the base changed the
// code, or split it by an atom) or no way to choose: the gap has no place; a copy put at its anchor would run the
// code twice (the base keeps its own, the change of the addon is a rebase conflict). skip: the gaps of the list other
// atoms of the delta take by their keys (not looked into)
bool FindGapText(const std::vector<Seg>& list, std::string_view text, const std::string& key,
    std::string_view ancestor, std::uint32_t& begin, std::uint32_t& end, const std::set<std::size_t>* skip = nullptr);

// S9 (ApplyDelta, a gap of an ltx / xml list - not the code between the atoms of a Lua script, that is the rule of
// the top-level code): the change of a gap by the addon (ancestor -> changed) overlaps the lines the base moved out of
// list[index] (split the gap by an atom, moved its tail): some line of the ancestor the gap lacks is in another gap of
// the list, and diff3 (ancestor, the gap, changed) is a conflict or touches, or the change inserts such a line (the
// addon moved it inside the gap). Merged with the ancestor by its key, the gap would get the moved lines back from
// the addon - the lines twice
bool GapChangeMoved(const std::vector<Seg>& list, std::string_view text, std::size_t index, std::string_view ancestor,
    std::string_view changed);

// S9 (ApplyDelta, a gap of an ltx / xml list, as GapChangeMoved): the change of a gap of the delta (ancestor ->
// changed) carried over by its hunks, as patch does it, when the gap has no place by its key or its text (the base
// split it by an atom, joined it with another one, moved it): each run of lines the change replaces, with the line
// before and after it, is found in one gap of the list, in one place only. edits: [begin, end) of the text -> the
// lines of `changed`, in the order of the text. false - a hunk is not found (the base changed those lines) or found
// twice: the change has no place (a rebase conflict, no copy). skip - as FindGapText
struct TextEdit
{
    std::uint32_t begin{};
    std::uint32_t end{};
    std::string text;
};
bool GapChangeEdits(const std::vector<Seg>& list, std::string_view text, std::string_view ancestor,
    std::string_view changed, std::vector<TextEdit>& edits, const std::set<std::size_t>* skip = nullptr);

// S9 (BuildDelta): the version split, joined or moved the code between the atoms of a document of code gaps (Lua): a
// gap of the ancestor is not there under its key, or a new gap came where a gap of the ancestor changed (between the
// same two atoms both have), and the code of the gaps there is not the same (a gap only renamed by the atom above it
// is not a change), or a line of code left the gaps between two atoms and came into the gaps between two others (moved
// over an atom). The atoms of the delta can not carry that over without the risk of the code twice (a gap deleted
// under one key and added under another; a moved line deleted where the base has it no more): the file goes by lines
bool GapsRestructured(const Doc& ancestor, std::string_view ancestor_text, const Doc& version,
    std::string_view version_text);

// S9 (ApplyDelta, a delta by lines): a line both a and b moved (each deleted it in one place of the ancestor and
// inserted it in another) that the merge by lines of the three has more often than each of them (the two insertions
// kept: the line twice) or less often (lost), and not as often as the ancestor plus the changes of a and b (copies of a
// repeated line added or deleted by each). The merge must be a conflict then
bool MovesCollide(std::string_view ancestor, std::string_view a, std::string_view b, std::string_view merged);

// S9 (BuildDelta): the gaps of a region (between two atoms of the ancestor the version keeps) whose code is the same
// as in the ancestor, gap by gap, get the keys of the ancestor: a gap only renamed by the atom above it (deleted,
// renamed) is not a gap deleted and another one added (that pair could give the code twice on a base that changed it)
void RekeyMovedGaps(const std::vector<Seg>& anc, const std::vector<std::string>& anc_keys, const std::vector<Seg>& ver,
    std::vector<std::string>& ver_keys);
} // namespace xray::content_merge::atoms
