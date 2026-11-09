// The merge by atoms (15-merge-implementation.md section 3.3).
//
// For every atom of the ancestor and every new atom of a side:
//   no side changed it              - the text of the ancestor;
//   one side changed it             - the text of that side (a deletion, an addition included);
//   several sides, all the same     - the text of the upper of them (converging changes, not a conflict);
//   several sides, a container      - recursion: an ltx section by its keys, an xml node by its attributes and children;
//   several sides, divisible        - diff3 by lines inside the atom, folded over the sides bottom up;
//   otherwise (an indivisible atom, a deletion against a change, an overlap of the lines) - a conflict: the atom as a
//   whole from the winner - the rule of the player (atom:), a patch fragment made for all the conflicting addons,
//   else the upper side (the order).
// A new atom goes after its anchor - the nearest atom above it in the version of its side that the result has; the
// insertions at one anchor go in the order of the sides. A move of an atom by a side is not carried: the order of the
// ancestor stays (the moved atoms are reported).
// The code between the atoms of a Lua script (Doc::code_gaps) goes by regions (Merger::List): the segments between two
// atoms of the ancestor every side keeps in the same order. The rule of the top-level code (review 2026-11-06 section
// 4.1 item 2): a region whose code (its gaps: their keys and texts) one side changed goes by the keys as any list (the
// change of that side as it is); two sides or more that changed it differently - a conflict of the region as a whole:
// the region of the winner (Merger::RegionConflict). The same change of several sides is no conflict only when their
// changes of all the other regions are the same too. Loud, never a guess: no merge of the code by lines, no search of
// moved lines or calls.

#include "AtomMerge.h"
#include "Diff.h"
#include "TextNorm.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <deque>
#include <list>

namespace xray::content_merge::atoms
{
namespace
{
// A version of one list (the top of a document or the children of a container)
struct ListVersion
{
    int side; // the index in the sides of the merge
    const std::vector<Seg>* list;
    std::string_view text;
    bool fragment;
};

struct Cand
{
    int side;
    const Seg* seg; // nullptr - the side deleted the atom
    std::string_view text; // the text of the version
    std::size_t lv; // the index of its ListVersion
};

std::string_view SegText(std::string_view text, const Seg& seg)
{
    return text.substr(seg.begin, seg.end - seg.begin);
}

class Merger
{
public:
    Merger(std::string_view base_text, const std::vector<Version>& sides, const std::map<std::string, int>& rules,
        const Options& options, const std::vector<std::string>& owner_names)
        : base_text_(base_text), sides_(sides), rules_(rules), options_(options), owner_names_(owner_names)
    {
    }

    // code_gaps: the gaps of the list are code that runs (the top of a Lua script): the regions below
    void List(const std::vector<Seg>& base, const std::vector<ListVersion>& versions, bool code_gaps = false);
    Outcome out;

private:
    // A slot of the result: an atom or a gap of the ancestor (base_index), or a new one of a side
    struct Slot
    {
        std::string key;
        int base_index;
    };

    // One list being merged: the keys of the segments (ListKeys) and their index by the key in each version
    struct ListCtx
    {
        const std::vector<Seg>& base;
        const std::vector<ListVersion>& versions;
        std::vector<std::string> base_keys;
        std::vector<std::vector<std::string>> keys;
        std::vector<std::map<std::string, std::size_t>> index;
        std::set<std::string> done; // the slots decided (an atom of a region is decided once)
    };

    // A region: the segments between two atoms of the skeleton, [from, to) in the ancestor and in each version
    struct Region
    {
        std::size_t from{};
        std::size_t to{};
        std::vector<std::size_t> v_from;
        std::vector<std::size_t> v_to;
    };

    void DecideSlot(ListCtx& ctx, const Slot& slot);
    std::vector<std::size_t> Skeleton(const ListCtx& ctx) const;
    std::vector<int> CodeIds(const ListCtx& ctx, const Region& region) const;
    void Tokens(const std::vector<Seg>& list, const std::vector<std::string>& keys, std::size_t from, std::size_t to,
        std::string_view text, int owner, diff::Lines& lines);
    void RegionConflict(ListCtx& ctx, const Region& region, const std::vector<int>& touchers,
        const std::vector<const Slot*>& slots, const std::map<std::string, const Slot*>& slot_of);
    void Emit(std::string_view text, int owner);
    void EmitCand(const Cand& cand);
    void Decide(const Seg* base_seg, const std::string& label, const std::vector<Cand>& cands,
        const std::vector<ListVersion>& versions);
    void Conflict(const Seg* base_seg, const std::string& label, const std::vector<Cand>& cands);
    void Reordered(const std::vector<Seg>& base, const std::vector<std::string>& base_keys,
        const std::vector<ListVersion>& versions, const std::vector<std::vector<std::string>>& keys);

    std::string_view base_text_;
    const std::vector<Version>& sides_;
    const std::map<std::string, int>& rules_;
    const Options& options_;
    const std::vector<std::string>& owner_names_;
    std::deque<std::string> tokens_; // the texts of the atom tokens of the regions (the lines point into them)
};

// An atom in the token lines of a region (a line of code can not start with these bytes)
constexpr std::string_view kAtomToken = "\x01\x02atom ";

bool IsAtomToken(std::string_view line)
{
    return line.substr(0, kAtomToken.size()) == kAtomToken;
}

std::string KeyOfToken(std::string_view line)
{
    std::string_view key = line.substr(kAtomToken.size());
    if (!key.empty() && key.back() == '\n')
        key.remove_suffix(1);
    return std::string(key);
}

void Merger::Emit(std::string_view text, int owner)
{
    if (text.empty())
        return;
    if (!out.pieces.empty() && out.pieces.back().owner == owner)
        out.pieces.back().text.append(text.data(), text.size());
    else
        out.pieces.push_back({std::string(text), owner});
}

void Merger::EmitCand(const Cand& cand)
{
    if (cand.seg)
        Emit(SegText(cand.text, *cand.seg), cand.side + 1);
}

void Merger::Conflict(const Seg* base_seg, const std::string& label, const std::vector<Cand>& cands)
{
    ConflictRec rec;
    rec.atom = label;
    rec.base_line = base_seg ? base_seg->line : 0;
    for (const Cand& c : cands)
        rec.owners.push_back(c.side + 1);
    // A patch fragment made for all the other conflicting addons resolves it (the upper such patch)
    const Cand* winner = nullptr;
    for (auto it = cands.rbegin(); it != cands.rend() && !winner; ++it)
    {
        const Version& v = sides_[std::size_t(it->side)];
        if (!v.fragment)
            continue;
        bool covers = true;
        for (const Cand& other : cands)
        {
            if (&other == &*it)
                continue;
            const std::string& name = owner_names_[std::size_t(other.side + 1)];
            covers = covers && std::find(v.patches.begin(), v.patches.end(), name) != v.patches.end();
        }
        if (covers)
            winner = &*it;
    }
    if (winner)
        rec.resolved_by = "patch";
    else
    {
        winner = &cands.back();
        rec.resolved_by = "order";
        ++out.unresolved;
    }
    rec.winner = winner->side + 1;
    for (const Cand& c : cands)
    {
        if (&c != winner)
            rec.lost.push_back(c.side + 1);
    }
    if (winner->seg)
        rec.text = std::string(SegText(winner->text, *winner->seg));
    if (options_.markers)
    {
        const auto emit_block = [&](std::string_view head, std::string_view body, int owner)
        {
            Emit(head, owner);
            Emit(body, owner);
            if (!body.empty() && body.back() != '\n')
                Emit("\n", owner);
        };
        emit_block("<<<<<<< " + owner_names_[std::size_t(cands.front().side + 1)] + "\n",
            cands.front().seg ? SegText(cands.front().text, *cands.front().seg) : std::string_view(),
            cands.front().side + 1);
        emit_block("||||||| " + owner_names_[0] + "\n", base_seg ? SegText(base_text_, *base_seg) : std::string_view(),
            0);
        for (std::size_t k = 1; k < cands.size(); ++k)
        {
            emit_block("======= " + owner_names_[std::size_t(cands[k].side + 1)] + "\n",
                cands[k].seg ? SegText(cands[k].text, *cands[k].seg) : std::string_view(), cands[k].side + 1);
        }
        Emit(">>>>>>> " + owner_names_[std::size_t(cands.back().side + 1)] + "\n", cands.back().side + 1);
    }
    else
        EmitCand(*winner);
    out.conflicts.push_back(std::move(rec));
}

void Merger::Decide(const Seg* base_seg, const std::string& label, const std::vector<Cand>& cands,
    const std::vector<ListVersion>& versions)
{
    if (cands.empty())
    {
        if (base_seg)
            Emit(SegText(base_text_, *base_seg), 0);
        return;
    }
    bool differ = false;
    bool all_present = true;
    bool any_present = false;
    for (const Cand& c : cands)
    {
        all_present = all_present && c.seg;
        any_present = any_present || c.seg;
        if (c.seg && cands.front().seg)
            differ = differ || c.seg->cmp != cands.front().seg->cmp;
        else
            differ = differ || bool(c.seg) != bool(cands.front().seg);
    }

    // The rule of the player
    const auto rule = rules_.find(label);
    if (rule != rules_.end())
    {
        const int owner = rule->second;
        bool applied = false;
        if (owner == 0)
        {
            if (base_seg)
                Emit(SegText(base_text_, *base_seg), 0);
            applied = true;
        }
        else
        {
            for (const Cand& c : cands)
            {
                if (c.side + 1 == owner)
                {
                    EmitCand(c);
                    applied = true;
                }
            }
            if (!applied)
            {
                for (const ListVersion& v : versions)
                {
                    if (v.side + 1 == owner && base_seg)
                    {
                        // the side did not change the atom: its version is the one of the ancestor
                        Emit(SegText(base_text_, *base_seg), 0);
                        applied = true;
                    }
                }
            }
        }
        if (applied)
        {
            out.rules_applied.push_back(label);
            if (cands.size() >= 2 && differ)
            {
                ConflictRec rec;
                rec.atom = label;
                rec.base_line = base_seg ? base_seg->line : 0;
                for (const Cand& c : cands)
                {
                    rec.owners.push_back(c.side + 1);
                    if (c.side + 1 != owner)
                        rec.lost.push_back(c.side + 1);
                }
                rec.winner = owner;
                rec.resolved_by = "player";
                out.conflicts.push_back(std::move(rec));
            }
            return;
        }
    }

    if (cands.size() == 1 || !differ)
    {
        EmitCand(cands.back());
        ++out.atoms_merged;
        return;
    }
    if (all_present)
    {
        bool containers = base_seg && !base_seg->children.empty();
        for (const Cand& c : cands)
            containers = containers && !c.seg->children.empty();
        if (containers)
        {
            std::vector<ListVersion> inner;
            for (const Cand& c : cands)
                inner.push_back({c.side, &c.seg->children, c.text, versions[c.lv].fragment});
            List(base_seg->children, inner);
            return;
        }
        bool gaps = !base_seg;
        for (const Cand& c : cands)
            gaps = gaps && c.seg->gap;
        if (gaps)
        {
            // New code between the atoms from several sides at one place: all of it, in the order of the sides
            for (const Cand& c : cands)
                EmitCand(c);
            ++out.atoms_merged;
            ++out.adjacent;
            out.adjacent_atoms.push_back(label);
            return;
        }
        bool divisible = base_seg ? base_seg->divisible : true;
        for (const Cand& c : cands)
            divisible = divisible && c.seg->divisible;
        if (divisible)
        {
            const bool gap_atom = base_seg && base_seg->gap;
            const diff::Lines base_lines =
                base_seg ? diff::SplitLines(SegText(base_text_, *base_seg), 0) : diff::Lines();
            diff::Lines acc = diff::SplitLines(SegText(cands.front().text, *cands.front().seg), cands.front().side + 1);
            std::uint32_t adjacent = 0;
            bool conflict = false;
            for (std::size_t k = 1; k < cands.size() && !conflict; ++k)
            {
                // A text a lower side gave already is folded once: diff3 of the merged text with the same change
                // again keeps its insertion twice when the change of another side touches it
                bool seen = false;
                for (std::size_t j = 0; j < k && !seen; ++j)
                    seen = SegText(cands[j].text, *cands[j].seg) == SegText(cands[k].text, *cands[k].seg);
                if (seen)
                    continue;
                const diff::Lines side_lines =
                    diff::SplitLines(SegText(cands[k].text, *cands[k].seg), cands[k].side + 1);
                diff::Merge3Result r = diff::Merge3(base_lines, acc, side_lines, true, options_.merge_adjacent,
                    gap_atom && options_.merge_adjacent);
                conflict = r.conflict;
                adjacent += r.adjacent;
                acc = std::move(r.lines);
            }
            if (!conflict)
            {
                for (const diff::Line& line : acc)
                    Emit(line.text, line.owner);
                ++out.atoms_merged;
                if (adjacent)
                {
                    out.adjacent += adjacent;
                    out.adjacent_atoms.push_back(label);
                }
                return;
            }
        }
    }
    Conflict(base_seg, label, cands);
}

void Merger::Reordered(const std::vector<Seg>& base, const std::vector<std::string>& base_keys,
    const std::vector<ListVersion>& versions, const std::vector<std::vector<std::string>>& keys)
{
    std::map<std::string, std::size_t> pos;
    for (std::size_t i = 0; i < base.size(); ++i)
    {
        if (!base[i].gap)
            pos[base_keys[i]] = i;
    }
    for (std::size_t v = 0; v < versions.size(); ++v)
    {
        if (versions[v].fragment)
            continue;
        std::size_t max_pos = 0;
        bool first = true;
        for (const std::string& key : keys[v])
        {
            const auto it = pos.find(key);
            if (it == pos.end())
                continue;
            if (!first && it->second < max_pos)
            {
                const std::string& label = base[it->second].label;
                if (std::find(out.reordered.begin(), out.reordered.end(), label) == out.reordered.end())
                    out.reordered.push_back(label);
            }
            if (first || it->second > max_pos)
                max_pos = it->second;
            first = false;
        }
    }
}

void Merger::DecideSlot(ListCtx& ctx, const Slot& slot)
{
    if (!ctx.done.insert(slot.key).second)
        return;
    const std::vector<Seg>& base = ctx.base;
    const std::vector<ListVersion>& versions = ctx.versions;
    const Seg* base_seg = slot.base_index >= 0 ? &base[std::size_t(slot.base_index)] : nullptr;
    std::vector<Cand> cands;
    const Seg* any = base_seg;
    for (std::size_t v = 0; v < versions.size(); ++v)
    {
        const auto it = ctx.index[v].find(slot.key);
        if (it == ctx.index[v].end())
        {
            const bool gap_of_fragment = versions[v].fragment && base_seg && base_seg->gap;
            if (base_seg && !versions[v].fragment && !gap_of_fragment)
                cands.push_back({versions[v].side, nullptr, versions[v].text, v});
            continue;
        }
        const Seg& seg = (*versions[v].list)[it->second];
        if (!any)
            any = &seg;
        if (base_seg && seg.cmp == base_seg->cmp)
            continue;
        cands.push_back({versions[v].side, &seg, versions[v].text, v});
    }
    std::string label = any ? any->label : slot.key;
    if (any && any->gap)
        label = GapLabel(*any, slot.key);
    Decide(base_seg, label, cands, versions);
}

// The skeleton of a list: the atoms of the ancestor every whole version (not a fragment) has, in the same order in
// all of them. They split the list into the regions; the code between the atoms never crosses them
std::vector<std::size_t> Merger::Skeleton(const ListCtx& ctx) const
{
    std::vector<std::size_t> skeleton;
    std::vector<std::size_t> next(ctx.versions.size(), 0); // the first index the next skeleton atom may have
    for (std::size_t i = 0; i < ctx.base.size(); ++i)
    {
        if (ctx.base[i].gap)
            continue;
        bool ok = true;
        for (std::size_t v = 0; v < ctx.versions.size() && ok; ++v)
        {
            if (ctx.versions[v].fragment)
                continue;
            const auto it = ctx.index[v].find(ctx.base_keys[i]);
            ok = it != ctx.index[v].end() && it->second >= next[v];
        }
        if (!ok)
            continue;
        for (std::size_t v = 0; v < ctx.versions.size(); ++v)
        {
            if (!ctx.versions[v].fragment)
                next[v] = ctx.index[v].at(ctx.base_keys[i]) + 1;
        }
        skeleton.push_back(i);
    }
    return skeleton;
}

// The code of a region (its gaps: their keys and compared texts, in their order) in each version: 0 - the code of the
// ancestor (a fragment too), else the number of its distinct code among the versions (1, 2...). A side that put an
// atom into a gap, deleted or renamed the atom above one changed it too: the keys of its gaps moved
std::vector<int> Merger::CodeIds(const ListCtx& ctx, const Region& region) const
{
    using GapList = std::vector<std::pair<std::string, const std::string*>>;
    const auto same = [](const GapList& a, const GapList& b)
    {
        bool eq = a.size() == b.size();
        for (std::size_t k = 0; eq && k < a.size(); ++k)
            eq = a[k].first == b[k].first && *a[k].second == *b[k].second;
        return eq;
    };
    std::vector<GapList> known(1);
    for (std::size_t i = region.from; i < region.to; ++i)
    {
        if (ctx.base[i].gap)
            known[0].emplace_back(ctx.base_keys[i], &ctx.base[i].cmp);
    }
    std::vector<int> ids(ctx.versions.size(), 0);
    for (std::size_t v = 0; v < ctx.versions.size(); ++v)
    {
        if (ctx.versions[v].fragment)
            continue;
        const std::vector<Seg>& list = *ctx.versions[v].list;
        GapList gaps;
        for (std::size_t j = region.v_from[v]; j < region.v_to[v]; ++j)
        {
            if (list[j].gap)
                gaps.emplace_back(ctx.keys[v][j], &list[j].cmp);
        }
        std::size_t k = 0;
        while (k < known.size() && !same(gaps, known[k]))
            ++k;
        if (k == known.size())
            known.push_back(std::move(gaps));
        ids[v] = int(k);
    }
    return ids;
}

// The token lines of a region: the lines of its gaps; an atom - one line (its key)
void Merger::Tokens(const std::vector<Seg>& list, const std::vector<std::string>& keys, std::size_t from,
    std::size_t to, std::string_view text, int owner, diff::Lines& lines)
{
    for (std::size_t i = from; i < to; ++i)
    {
        const Seg& seg = list[i];
        if (seg.gap)
        {
            for (const diff::Line& line : diff::SplitLines(SegText(text, seg), owner))
                lines.push_back(line);
            continue;
        }
        tokens_.push_back(std::string(kAtomToken) + keys[i] + "\n");
        lines.push_back({tokens_.back(), owner});
    }
}

// A conflict of a region (List, the rule of the top-level code): the region of the winner as a whole - the rule of the
// player (by the label of the region), else the upper side that changed it. Its code as it is; its atoms (a token
// each) decided by the keys at the places the winner has them, the atoms of the region it lacks (deleted by it, new of
// another side) after them
void Merger::RegionConflict(ListCtx& ctx, const Region& region, const std::vector<int>& touchers,
    const std::vector<const Slot*>& slots, const std::map<std::string, const Slot*>& slot_of)
{
    // The label of the region: its first gap (in the ancestor, else in the first side that changed it)
    std::string label;
    std::uint32_t base_line = 0;
    for (std::size_t i = region.from; i < region.to && label.empty(); ++i)
    {
        if (ctx.base[i].gap)
        {
            label = GapLabel(ctx.base[i], ctx.base_keys[i]);
            base_line = ctx.base[i].line;
        }
    }
    for (std::size_t v = 0; v < ctx.versions.size() && label.empty(); ++v)
    {
        if (ctx.versions[v].fragment || ctx.versions[v].side + 1 != touchers.front())
            continue;
        for (std::size_t j = region.v_from[v]; j < region.v_to[v] && label.empty(); ++j)
        {
            const Seg& seg = (*ctx.versions[v].list)[j];
            if (seg.gap)
                label = GapLabel(seg, ctx.keys[v][j]);
        }
    }

    ConflictRec rec;
    rec.atom = label;
    rec.base_line = base_line;
    rec.owners = touchers;
    rec.winner = touchers.back();
    rec.resolved_by = "order";
    const auto rule = rules_.find(label);
    if (rule != rules_.end())
    {
        rec.winner = rule->second;
        rec.resolved_by = "player";
        out.rules_applied.push_back(label);
    }
    else
        ++out.unresolved;
    for (const int owner : touchers)
    {
        if (owner != rec.winner)
            rec.lost.push_back(owner);
    }
    // The lines of the winner: of the ancestor, or of a whole side (a side that did not change the region has the
    // code of the ancestor there)
    diff::Lines acc;
    std::size_t winner = ctx.versions.size();
    for (std::size_t v = 0; v < ctx.versions.size(); ++v)
    {
        if (!ctx.versions[v].fragment && ctx.versions[v].side + 1 == rec.winner)
            winner = v;
    }
    if (winner < ctx.versions.size())
    {
        const ListVersion& lv = ctx.versions[winner];
        Tokens(*lv.list, ctx.keys[winner], region.v_from[winner], region.v_to[winner], lv.text, lv.side + 1, acc);
    }
    else
        Tokens(ctx.base, ctx.base_keys, region.from, region.to, base_text_, 0, acc);
    for (const diff::Line& line : acc)
    {
        if (!IsAtomToken(line.text))
            rec.text.append(line.text.data(), line.text.size());
    }
    if (options_.markers)
    {
        // The draft: the regions of the sides that changed it, as they are, between the markers
        const auto region_text = [&](std::size_t v)
        {
            const bool anc = v >= ctx.versions.size();
            const std::vector<Seg>& list = anc ? ctx.base : *ctx.versions[v].list;
            const std::string_view text = anc ? base_text_ : ctx.versions[v].text;
            const std::size_t from = anc ? region.from : region.v_from[v];
            const std::size_t to = anc ? region.to : region.v_to[v];
            std::string t;
            if (from < to)
                t = std::string(text.substr(list[from].begin, list[to - 1].end - list[from].begin));
            if (!t.empty() && t.back() != '\n')
                t += '\n';
            return t;
        };
        std::vector<std::size_t> touched;
        for (const int owner : touchers)
        {
            for (std::size_t v = 0; v < ctx.versions.size(); ++v)
            {
                if (!ctx.versions[v].fragment && ctx.versions[v].side + 1 == owner)
                    touched.push_back(v);
            }
        }
        Emit("<<<<<<< " + owner_names_[std::size_t(touchers.front())] + "\n", touchers.front());
        Emit(region_text(touched.front()), touchers.front());
        Emit("||||||| " + owner_names_[0] + "\n", 0);
        Emit(region_text(ctx.versions.size()), 0);
        for (std::size_t k = 1; k < touched.size(); ++k)
        {
            Emit("======= " + owner_names_[std::size_t(touchers[k])] + "\n", touchers[k]);
            Emit(region_text(touched[k]), touchers[k]);
        }
        Emit(">>>>>>> " + owner_names_[std::size_t(touchers.back())] + "\n", touchers.back());
        for (const Slot* slot : slots)
            ctx.done.insert(slot->key);
        out.conflicts.push_back(std::move(rec));
        return;
    }
    out.conflicts.push_back(std::move(rec));

    // The result: the lines of the code; an atom - its text decided by the keys (once)
    for (const diff::Line& line : acc)
    {
        if (!IsAtomToken(line.text))
        {
            Emit(line.text, line.owner);
            continue;
        }
        const auto it = slot_of.find(KeyOfToken(line.text));
        if (it != slot_of.end())
            DecideSlot(ctx, *it->second);
    }
    // The atoms of the region the lines do not have (a side deleted one, another changed it: the conflict may keep
    // it); the gaps of the region are all in the lines of the winner
    for (const Slot* slot : slots)
    {
        if (!slot->key.empty() && slot->key[0] == '~')
            ctx.done.insert(slot->key);
        else
            DecideSlot(ctx, *slot);
    }
}

void Merger::List(const std::vector<Seg>& base, const std::vector<ListVersion>& versions, bool code_gaps)
{
    ListCtx ctx{base, versions, ListKeys(base, nullptr), {}, {}, {}};
    const std::vector<std::string>& base_keys = ctx.base_keys;
    std::set<std::string> base_key_set;
    for (std::size_t i = 0; i < base.size(); ++i)
    {
        if (!base[i].gap)
            base_key_set.insert(base_keys[i]);
    }
    std::vector<std::vector<std::string>>& keys = ctx.keys;
    std::vector<std::map<std::string, std::size_t>>& index = ctx.index;
    for (const ListVersion& v : versions)
    {
        keys.push_back(ListKeys(*v.list, &base_key_set));
        std::map<std::string, std::size_t> map;
        for (std::size_t i = 0; i < keys.back().size(); ++i)
        {
            if (v.fragment && (*v.list)[i].gap)
                continue; // the gaps of a fragment are not changes
            map.emplace(keys.back()[i], i);
        }
        index.push_back(std::move(map));
    }
    Reordered(base, base_keys, versions, keys);

    // The slots of the result: the ancestor in its order, the new atoms after their anchors
    std::list<Slot> slots;
    std::map<std::string, std::list<Slot>::iterator> where;
    std::map<std::string, std::string> root_of;
    std::map<std::string, std::list<Slot>::iterator> chain_end;
    slots.push_back({"^", -1});
    where["^"] = slots.begin();
    root_of["^"] = "^";
    for (std::size_t i = 0; i < base.size(); ++i)
    {
        const auto it = slots.insert(slots.end(), Slot{base_keys[i], int(i)});
        where[base_keys[i]] = it;
        root_of[base_keys[i]] = base_keys[i];
        chain_end[base_keys[i]] = it;
    }
    // The start: after the preamble (a gap the ancestor begins with)
    chain_end["^"] = !base.empty() && base.front().gap ? where[base_keys.front()] : slots.begin();
    for (std::size_t v = 0; v < versions.size(); ++v)
    {
        const std::vector<Seg>& list = *versions[v].list;
        for (std::size_t j = 0; j < list.size(); ++j)
        {
            const std::string& key = keys[v][j];
            if (where.count(key) || !index[v].count(key))
                continue;
            std::string anchor = "^";
            for (std::size_t k = j; k-- > 0;)
            {
                if (where.count(keys[v][k]))
                {
                    anchor = keys[v][k];
                    break;
                }
            }
            const std::string root = root_of[anchor];
            const auto pos = chain_end[root];
            const auto it = slots.insert(std::next(pos), Slot{key, -1});
            where[key] = it;
            root_of[key] = root;
            chain_end[root] = it;
        }
    }

    if (!code_gaps)
    {
        for (const Slot& slot : slots)
        {
            if (slot.key != "^")
                DecideSlot(ctx, slot);
        }
        return;
    }

    // The code between the atoms: the list goes region by region, the skeleton atoms between them (the rule of the
    // top-level code below)
    std::vector<std::size_t> skeleton = Skeleton(ctx);
    std::set<int> skeleton_set;
    for (const std::size_t i : skeleton)
        skeleton_set.insert(int(i));
    std::map<std::string, const Slot*> slot_of;
    for (const Slot& slot : slots)
        slot_of[slot.key] = &slot;

    // A region is closed: a slot is in one region in the ancestor and in every whole version that has it. The place
    // of a new slot in the list of the slots follows the first side that has it, another side may have it (or the
    // code after it) beyond a skeleton atom: the skeleton atoms between are dropped, the regions joined. Otherwise
    // the code of a version could be in the lines of no region (lost) or of two
    const auto region_in = [](const std::vector<std::size_t>& bounds, std::size_t j)
    { return std::size_t(std::lower_bound(bounds.begin(), bounds.end(), j) - bounds.begin()); };
    std::map<const Slot*, std::size_t> home; // the region of a slot (none - the slots of the fragments only)
    for (bool again = true; again;)
    {
        again = false;
        home.clear();
        std::vector<std::vector<std::size_t>> bounds(versions.size());
        for (std::size_t v = 0; v < versions.size(); ++v)
        {
            if (versions[v].fragment)
                continue;
            for (const std::size_t i : skeleton)
                bounds[v].push_back(index[v].at(base_keys[i]));
        }
        std::vector<bool> drop(skeleton.size());
        for (const Slot& slot : slots)
        {
            if (slot.key == "^" || (slot.base_index >= 0 && skeleton_set.count(slot.base_index)))
                continue;
            std::size_t lo = SIZE_MAX;
            std::size_t hi = 0;
            const auto add = [&](std::size_t region)
            {
                lo = std::min(lo, region);
                hi = std::max(hi, region);
            };
            if (slot.base_index >= 0)
                add(region_in(skeleton, std::size_t(slot.base_index)));
            for (std::size_t v = 0; v < versions.size(); ++v)
            {
                if (versions[v].fragment)
                    continue;
                const auto it = index[v].find(slot.key);
                if (it != index[v].end())
                    add(region_in(bounds[v], it->second));
            }
            if (lo == SIZE_MAX)
                continue;
            home[&slot] = lo;
            for (std::size_t k = lo; k < hi; ++k)
            {
                drop[k] = true;
                again = true;
            }
        }
        if (again)
        {
            std::vector<std::size_t> kept;
            for (std::size_t k = 0; k < skeleton.size(); ++k)
            {
                if (!drop[k])
                    kept.push_back(skeleton[k]);
                else
                    skeleton_set.erase(int(skeleton[k]));
            }
            skeleton = std::move(kept);
        }
    }

    std::size_t r = 0;
    std::vector<std::vector<const Slot*>> by_region(skeleton.size() + 1);
    for (const Slot& slot : slots)
    {
        if (slot.key == "^")
            continue;
        if (slot.base_index >= 0 && skeleton_set.count(slot.base_index))
        {
            ++r;
            continue;
        }
        const auto it = home.find(&slot);
        by_region[it != home.end() ? it->second : r].push_back(&slot);
    }
    // The regions and the code of each in every version (CodeIds). The rule of the top-level code (review 2026-11-06
    // section 4.1 item 2): a region one side changed goes by the keys as any list - the change of that side as it is;
    // two sides or more that changed it differently - a conflict of the region as a whole (RegionConflict). The same
    // change of several sides is no conflict only when their changes of the other regions are the same too: else each
    // could have moved a line out of it to a region of its own - the line twice
    const std::size_t count = skeleton.size() + 1;
    std::vector<Region> regions(count);
    std::vector<std::vector<int>> ids(count);
    for (std::size_t k = 0; k < count; ++k)
    {
        Region& region = regions[k];
        region.from = k ? skeleton[k - 1] + 1 : 0;
        region.to = k < skeleton.size() ? skeleton[k] : base.size();
        for (std::size_t v = 0; v < versions.size(); ++v)
        {
            if (versions[v].fragment)
            {
                region.v_from.push_back(0);
                region.v_to.push_back(0);
                continue;
            }
            region.v_from.push_back(k ? index[v].at(base_keys[skeleton[k - 1]]) + 1 : 0);
            region.v_to.push_back(k < skeleton.size() ? index[v].at(base_keys[skeleton[k]]) : versions[v].list->size());
        }
        ids[k] = CodeIds(ctx, region);
    }
    const auto flush = [&](std::size_t k)
    {
        std::vector<std::size_t> changed; // the versions that changed the code of the region
        for (std::size_t v = 0; v < versions.size(); ++v)
        {
            if (ids[k][v])
                changed.push_back(v);
        }
        bool conflict = false;
        for (const std::size_t v : changed)
            conflict = conflict || ids[k][v] != ids[k][changed.front()];
        for (std::size_t other = 0; other < count && changed.size() >= 2 && !conflict; ++other)
        {
            for (const std::size_t v : changed)
                conflict = conflict || ids[other][v] != ids[other][changed.front()];
        }
        if (conflict)
        {
            std::vector<int> touchers;
            for (const std::size_t v : changed)
                touchers.push_back(versions[v].side + 1);
            RegionConflict(ctx, regions[k], touchers, by_region[k], slot_of);
            return;
        }
        for (const Slot* slot : by_region[k])
            DecideSlot(ctx, *slot);
    };
    for (r = 0; r < skeleton.size(); ++r)
    {
        flush(r);
        DecideSlot(ctx, *slot_of.at(base_keys[skeleton[r]]));
    }
    flush(r);
}
} // namespace

std::string Outcome::Text() const
{
    std::string text;
    for (const Piece& piece : pieces)
        text += piece.text;
    return text;
}

std::vector<std::string> ListKeys(const std::vector<Seg>& list, const std::set<std::string>* base_keys)
{
    std::vector<std::string> keys;
    keys.reserve(list.size());
    std::string anchor = "^";
    std::map<std::string, int> ordinal;
    for (const Seg& seg : list)
    {
        if (!seg.gap)
        {
            keys.push_back(seg.key);
            if (!base_keys || base_keys->count(seg.key))
                anchor = seg.key;
            continue;
        }
        keys.push_back("~" + anchor + "#" + std::to_string(ordinal[anchor]++));
    }
    return keys;
}

std::string GapLabel(const Seg& seg, const std::string& key)
{
    // "~<anchor>#<n>" -> "<label> (after <anchor>)"
    std::string anchor = key.size() > 1 ? key.substr(1) : key;
    const std::size_t hash = anchor.rfind('#');
    std::string n;
    if (hash != std::string::npos)
    {
        n = anchor.substr(hash + 1);
        anchor.erase(hash);
    }
    std::string label = seg.label.empty() ? std::string("(gap)") : seg.label;
    // The text of an xml node right after its start tag: the label is enough ("w/btn#text")
    if ((anchor == ">" || anchor == "<" || anchor == "</" || anchor[0] == '@') && (n.empty() || n == "0"))
        return label;
    label += anchor == "^" ? " (start" : " (after " + anchor;
    if (!n.empty() && n != "0")
        label += ", " + n;
    label += ")";
    return label;
}

bool FindGapText(const std::vector<Seg>& list, std::string_view text, const std::string& key,
    std::string_view ancestor, std::uint32_t& begin, std::uint32_t& end, const std::set<std::size_t>* skip)
{
    if (ancestor.empty())
        return false;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> found;
    for (std::size_t i = 0; i < list.size(); ++i)
    {
        const Seg& seg = list[i];
        if (!seg.gap || (skip && skip->count(i)))
            continue;
        const std::string_view gap = SegText(text, seg);
        for (std::size_t p = gap.find(ancestor); p != std::string_view::npos; p = gap.find(ancestor, p + 1))
        {
            const bool line_start = p == 0 || gap[p - 1] == '\n';
            const bool line_end = ancestor.back() == '\n' || p + ancestor.size() == gap.size();
            if (line_start && line_end)
                found.emplace_back(seg.begin + std::uint32_t(p), seg.begin + std::uint32_t(p + ancestor.size()));
        }
    }
    std::size_t pick = found.size();
    if (found.size() == 1)
        pick = 0;
    else if (found.size() > 1 && key.size() > 1 && key[0] == '~')
    {
        // "~<atom>#<n>": after that atom
        const std::size_t hash = key.rfind('#');
        const std::string atom = key.substr(1, hash == std::string::npos ? std::string::npos : hash - 1);
        std::uint32_t after = 0;
        bool known = atom == "^";
        for (const Seg& seg : list)
        {
            if (!known && !seg.gap && seg.key == atom)
            {
                after = seg.end;
                known = true;
            }
        }
        for (std::size_t k = 0; known && k < found.size() && pick == found.size(); ++k)
        {
            if (found[k].first >= after)
                pick = k;
        }
    }
    if (pick == found.size())
        return false;
    begin = found[pick].first;
    end = found[pick].second;
    return true;
}

bool GapChangeMoved(const std::vector<Seg>& list, std::string_view text, std::size_t index, std::string_view ancestor,
    std::string_view changed)
{
    // The lines of code as the merge by lines compares them (the empty ones and the comments skipped)
    const auto count = [](std::string_view lines_text, std::map<std::string, int>& counts)
    {
        for (const diff::Line& line : diff::SplitLines(lines_text, 0))
        {
            const String c = text::CollapseSpaces(line.text);
            if (!c.empty() && c.rfind("--", 0) != 0)
                ++counts[std::string(c.data(), c.size())];
        }
    };
    if (index >= list.size())
        return false;
    std::map<std::string, int> anc;
    std::map<std::string, int> gap;
    std::map<std::string, int> others;
    count(ancestor, anc);
    count(SegText(text, list[index]), gap);
    for (std::size_t i = 0; i < list.size(); ++i)
    {
        if (i != index && list[i].gap)
            count(SegText(text, list[i]), others);
    }
    // The lines of the ancestor the gap lacks that another gap of the list has: moved there by the base (a line the
    // base deleted is not; both may be in one gap)
    std::set<std::string> moved;
    for (const auto& [line, n] : anc)
    {
        const auto in_gap = gap.find(line);
        if (n > (in_gap == gap.end() ? 0 : in_gap->second) && others.count(line))
            moved.insert(line);
    }
    if (moved.empty())
        return false;
    // Moved: the change of the addon brings them back where it overlaps them; where it only touches them, diff3
    // puts it at the border of the gap, not next to the lines it was made at
    const diff::Lines anc_lines = diff::SplitLines(ancestor, 0);
    const diff::Lines chg_lines = diff::SplitLines(changed, 2);
    const diff::Merge3Result m =
        diff::Merge3(anc_lines, diff::SplitLines(SegText(text, list[index]), 1), chg_lines, true, true);
    if (m.conflict || m.adjacent > 0)
        return true;
    // The addon moved a moved line inside the gap (deleted it and put it back elsewhere in it): its deletion is the
    // one of the base, its insertion would bring the line back - twice with the copy of the base
    std::map<std::string, std::uint32_t> ids;
    const auto ids_of = [&ids](const diff::Lines& lines)
    {
        std::vector<std::uint32_t> out;
        for (const diff::Line& line : lines)
        {
            const String c = text::CollapseSpaces(line.text);
            out.push_back(ids.emplace(std::string(c.data(), c.size()), std::uint32_t(ids.size())).first->second);
        }
        return out;
    };
    const std::vector<std::uint32_t> anc_ids = ids_of(anc_lines);
    const std::vector<std::uint32_t> chg_ids = ids_of(chg_lines);
    std::vector<diff::Hunk> hunks;
    if (!diff::Myers(anc_ids, chg_ids, diff::kDiffMaxWork, hunks))
        return true;
    for (const diff::Hunk& h : hunks)
    {
        for (std::uint32_t k = h.b_begin; k < h.b_end; ++k)
        {
            const String c = text::CollapseSpaces(chg_lines[k].text);
            if (moved.count(std::string(c.data(), c.size())))
                return true;
        }
    }
    return false;
}

bool GapChangeEdits(const std::vector<Seg>& list, std::string_view text, std::string_view ancestor,
    std::string_view changed, std::vector<TextEdit>& edits, const std::set<std::size_t>* skip)
{
    edits.clear();
    const diff::Lines anc = diff::SplitLines(ancestor, 0);
    const diff::Lines chg = diff::SplitLines(changed, 0);
    if (anc.empty())
        return false;
    // The lines as the merge by lines compares them
    std::map<std::string, std::uint32_t> ids;
    const auto id_of = [&ids](std::string_view line)
    {
        const String c = text::CollapseSpaces(line);
        return ids.emplace(std::string(c.data(), c.size()), std::uint32_t(ids.size())).first->second;
    };
    std::vector<std::uint32_t> anc_ids;
    std::vector<std::uint32_t> chg_ids;
    for (const diff::Line& line : anc)
        anc_ids.push_back(id_of(line.text));
    for (const diff::Line& line : chg)
        chg_ids.push_back(id_of(line.text));
    std::vector<diff::Hunk> hunks;
    if (!diff::Myers(anc_ids, chg_ids, diff::kDiffMaxWork, hunks))
        return false;
    // The lines of the gaps of the list: the id, the place, the gap
    struct GapLine
    {
        std::uint32_t id;
        std::uint32_t begin;
        std::uint32_t end;
        std::size_t gap;
    };
    std::vector<GapLine> lines;
    for (std::size_t g = 0; g < list.size(); ++g)
    {
        if (!list[g].gap || (skip && skip->count(g)))
            continue;
        std::uint32_t pos = list[g].begin;
        for (const diff::Line& line : diff::SplitLines(SegText(text, list[g]), 0))
        {
            lines.push_back({id_of(line.text), pos, pos + std::uint32_t(line.text.size()), g});
            pos += std::uint32_t(line.text.size());
        }
    }
    for (const diff::Hunk& h : hunks)
    {
        // The run of the ancestor the hunk replaces with the line before and after it (the context); not found -
        // without the context on one side, then on both (the base split the code right there). Found twice - the
        // place is not known
        const std::size_t replaced = h.a_end - h.a_begin;
        std::size_t found = lines.size();
        std::size_t lead = 0;
        for (int variant = 0; variant < 4 && found == lines.size(); ++variant)
        {
            const bool use_lead = (variant == 0 || variant == 2) && h.a_begin > 0;
            const bool use_trail = (variant == 0 || variant == 1) && h.a_end < anc.size();
            if (!replaced && !use_lead && !use_trail)
                continue;
            const std::size_t from = use_lead ? h.a_begin - 1 : h.a_begin;
            const std::size_t to = use_trail ? h.a_end + 1 : h.a_end;
            std::size_t count = 0;
            std::size_t at_found = 0;
            for (std::size_t at = 0; at + (to - from) <= lines.size(); ++at)
            {
                bool match = true;
                for (std::size_t k = 0; k < to - from && match; ++k)
                    match = lines[at + k].id == anc_ids[from + k] && lines[at + k].gap == lines[at].gap;
                if (match)
                {
                    at_found = at;
                    ++count;
                }
            }
            if (count > 1)
                return false;
            if (count == 1)
            {
                found = at_found;
                lead = use_lead ? 1 : 0;
            }
        }
        if (found == lines.size())
            return false;
        TextEdit edit;
        const std::size_t first = found + lead; // the first line replaced
        if (replaced)
        {
            edit.begin = lines[first].begin;
            edit.end = lines[first + replaced - 1].end;
        }
        else
            edit.begin = edit.end = lead ? lines[first - 1].end : lines[first].begin;
        for (std::uint32_t k = h.b_begin; k < h.b_end; ++k)
            edit.text.append(chg[k].text.data(), chg[k].text.size());
        if (!edits.empty() && edit.begin < edits.back().end)
            return false;
        edits.push_back(std::move(edit));
    }
    return true;
}

namespace
{
// The regions of a version against its ancestor (the code gaps): the atoms of the ancestor the version keeps in
// their order are the borders; for each region - the indices of its gaps in the ancestor and in the version
struct GapRegion
{
    std::vector<std::size_t> anc;
    std::vector<std::size_t> ver;
};

std::vector<GapRegion> GapRegions(const std::vector<Seg>& anc, const std::vector<std::string>& anc_keys,
    const std::vector<Seg>& ver, const std::vector<std::string>& ver_keys)
{
    std::map<std::string, std::size_t> ver_index;
    for (std::size_t j = 0; j < ver.size(); ++j)
        ver_index.emplace(ver_keys[j], j);
    std::vector<std::size_t> anc_bounds;
    std::vector<std::size_t> ver_bounds;
    std::size_t next = 0;
    for (std::size_t i = 0; i < anc.size(); ++i)
    {
        if (anc[i].gap)
            continue;
        const auto it = ver_index.find(anc_keys[i]);
        if (it == ver_index.end() || it->second < next)
            continue;
        anc_bounds.push_back(i);
        ver_bounds.push_back(it->second);
        next = it->second + 1;
    }
    anc_bounds.push_back(anc.size());
    ver_bounds.push_back(ver.size());
    std::vector<GapRegion> regions(anc_bounds.size());
    std::size_t anc_from = 0;
    std::size_t ver_from = 0;
    for (std::size_t r = 0; r < anc_bounds.size(); ++r)
    {
        for (std::size_t i = anc_from; i < anc_bounds[r]; ++i)
        {
            if (anc[i].gap)
                regions[r].anc.push_back(i);
        }
        for (std::size_t j = ver_from; j < ver_bounds[r]; ++j)
        {
            if (ver[j].gap)
                regions[r].ver.push_back(j);
        }
        anc_from = anc_bounds[r] + 1;
        ver_from = ver_bounds[r] + 1;
    }
    return regions;
}

// The same code in the same number of gaps of a region (the keys may differ)
bool SameCode(const GapRegion& region, const std::vector<Seg>& anc, const std::vector<Seg>& ver)
{
    bool same = region.anc.size() == region.ver.size();
    for (std::size_t k = 0; same && k < region.anc.size(); ++k)
        same = anc[region.anc[k]].cmp == ver[region.ver[k]].cmp;
    return same;
}

// A line of the block words only ("end", "else", "end)", "}"...): every block has them, their move is no move of code
bool BlockWordsOnly(std::string_view line)
{
    static const std::set<std::string_view> words = {"end", "else", "do", "then", "repeat", "break", "return"};
    std::size_t pos = 0;
    while (pos < line.size())
    {
        const auto word_char = [](char ch) { return ch != ' ' && ch != '(' && ch != ')' && ch != '{' && ch != '}' &&
                                                 ch != ',' && ch != ';'; };
        if (!word_char(line[pos]))
        {
            ++pos;
            continue;
        }
        std::size_t end = pos;
        while (end < line.size() && word_char(line[end]))
            ++end;
        if (!words.count(line.substr(pos, end - pos)))
            return false;
        pos = end;
    }
    return true;
}

// The lines of code of the gaps of a region as the merge by lines compares them (the empty lines, the comments and the
// lines of the block words only skipped)
void RegionCode(const std::vector<Seg>& list, std::string_view text, const std::vector<std::size_t>& gaps,
    std::map<std::string, int>& counts)
{
    for (const std::size_t g : gaps)
    {
        for (const diff::Line& line : diff::SplitLines(SegText(text, list[g]), 0))
        {
            const String c = text::CollapseSpaces(line.text);
            const std::string_view s(c.data(), c.size());
            if (!s.empty() && s.rfind("--", 0) != 0 && !BlockWordsOnly(s))
                ++counts[std::string(s)];
        }
    }
}

// The lines of code of the gaps a version moved over an atom: deleted from the gaps of one region (between two atoms
// of the ancestor it keeps in their order), added to the gaps of another one
std::set<std::string> MovedCode(const std::vector<Seg>& anc, std::string_view ancestor_text,
    const std::vector<Seg>& ver, std::string_view version_text)
{
    const std::vector<std::string> anc_keys = ListKeys(anc, nullptr);
    std::set<std::string> anc_atoms;
    for (std::size_t i = 0; i < anc.size(); ++i)
    {
        if (!anc[i].gap)
            anc_atoms.insert(anc_keys[i]);
    }
    const std::vector<std::string> ver_keys = ListKeys(ver, &anc_atoms);
    const std::vector<GapRegion> regions = GapRegions(anc, anc_keys, ver, ver_keys);
    std::map<std::string, std::set<std::size_t>> removed_in; // the line -> the regions it left
    std::vector<std::map<std::string, int>> gained(regions.size());
    for (std::size_t r = 0; r < regions.size(); ++r)
    {
        std::map<std::string, int> anc_code;
        std::map<std::string, int> ver_code;
        RegionCode(anc, ancestor_text, regions[r].anc, anc_code);
        RegionCode(ver, version_text, regions[r].ver, ver_code);
        for (const auto& [line, n] : anc_code)
        {
            const auto it = ver_code.find(line);
            if (n > (it == ver_code.end() ? 0 : it->second))
                removed_in[line].insert(r);
        }
        for (const auto& [line, n] : ver_code)
        {
            const auto it = anc_code.find(line);
            if (n > (it == anc_code.end() ? 0 : it->second))
                gained[r].emplace(line, n);
        }
    }
    std::set<std::string> moved;
    for (std::size_t r = 0; r < regions.size(); ++r)
    {
        for (const auto& [line, n] : gained[r])
        {
            const auto it = removed_in.find(line);
            if (it != removed_in.end() && (it->second.size() > 1 || !it->second.count(r)))
                moved.insert(line);
        }
    }
    return moved;
}

} // namespace

void RekeyMovedGaps(const std::vector<Seg>& anc, const std::vector<std::string>& anc_keys, const std::vector<Seg>& ver,
    std::vector<std::string>& ver_keys)
{
    for (const GapRegion& region : GapRegions(anc, anc_keys, ver, ver_keys))
    {
        if (region.anc.empty() || !SameCode(region, anc, ver))
            continue;
        // the keys of the version taken only when none of them is a key of another gap of the version
        std::set<std::string> taken(ver_keys.begin(), ver_keys.end());
        bool free = true;
        for (std::size_t k = 0; k < region.anc.size(); ++k)
        {
            const std::string& key = anc_keys[region.anc[k]];
            free = free && (key == ver_keys[region.ver[k]] || !taken.count(key));
        }
        if (!free)
            continue;
        for (std::size_t k = 0; k < region.anc.size(); ++k)
            ver_keys[region.ver[k]] = anc_keys[region.anc[k]];
    }
}

bool GapsRestructured(const Doc& ancestor, std::string_view ancestor_text, const Doc& version,
    std::string_view version_text)
{
    if (!ancestor.code_gaps)
        return false;
    const std::vector<Seg>& anc = ancestor.top;
    const std::vector<Seg>& ver = version.top;
    const std::vector<std::string> anc_keys = ListKeys(anc, nullptr);
    std::set<std::string> anc_atoms;
    for (std::size_t i = 0; i < anc.size(); ++i)
    {
        if (!anc[i].gap)
            anc_atoms.insert(anc_keys[i]);
    }
    const std::vector<std::string> ver_keys = ListKeys(ver, &anc_atoms);
    const std::vector<GapRegion> regions = GapRegions(anc, anc_keys, ver, ver_keys);
    // A line of code moved over an atom: deleted from the gaps of one region, added to the gaps of another one
    if (!MovedCode(anc, ancestor_text, ver, version_text).empty())
        return true;
    for (const GapRegion& region : regions)
    {
        // the same code in the same gaps under other keys (an atom above a gap deleted or renamed) is no change
        if (SameCode(region, anc, ver))
            continue;
        std::map<std::string, const std::string*> anc_gaps;
        for (const std::size_t i : region.anc)
            anc_gaps.emplace(anc_keys[i], &anc[i].cmp);
        bool added = false;
        bool changed = false;
        std::size_t found = 0;
        for (const std::size_t j : region.ver)
        {
            const auto it = anc_gaps.find(ver_keys[j]);
            if (it == anc_gaps.end())
                added = true;
            else
            {
                ++found;
                changed = changed || *it->second != ver[j].cmp;
            }
        }
        if (found < anc_gaps.size() || (added && changed))
            return true;
    }
    return false;
}

Outcome MergeDocs(std::string_view base_text, const Doc& base_doc, const std::vector<Version>& sides,
    const std::map<std::string, int>& rules, const Options& options, const std::vector<std::string>& owner_names)
{
    std::vector<ListVersion> versions;
    for (std::size_t i = 0; i < sides.size(); ++i)
        versions.push_back({int(i), &sides[i].doc->top, sides[i].text, sides[i].fragment});
    Merger merger(base_text, sides, rules, options, owner_names);
    merger.List(base_doc.top, versions, base_doc.code_gaps);
    return std::move(merger.out);
}

bool MovesCollide(std::string_view ancestor, std::string_view a, std::string_view b, std::string_view merged)
{
    // The lines a version deleted in one place and inserted in another (the hunks of its diff with the ancestor)
    std::map<std::string, std::uint32_t> ids;
    std::vector<std::string> names;
    const auto ids_of = [&ids, &names](std::string_view text)
    {
        std::vector<std::uint32_t> out;
        for (const diff::Line& line : diff::SplitLines(text, 0))
        {
            const String c = text::CollapseSpaces(line.text);
            const auto it = ids.emplace(std::string(c.data(), c.size()), std::uint32_t(ids.size()));
            if (it.second)
                names.push_back(it.first->first);
            out.push_back(it.first->second);
        }
        return out;
    };
    std::vector<std::vector<std::uint32_t>> texts;
    texts.push_back(ids_of(ancestor));
    std::map<std::uint32_t, int> moved_by; // the line -> the number of the versions that moved it
    for (const std::string_view version : {a, b})
    {
        texts.push_back(ids_of(version));
        std::vector<diff::Hunk> hunks;
        if (!diff::Myers(texts.front(), texts.back(), diff::kDiffMaxWork, hunks))
            return false;
        std::set<std::uint32_t> deleted;
        std::set<std::uint32_t> inserted;
        for (const diff::Hunk& h : hunks)
        {
            for (std::uint32_t k = h.a_begin; k < h.a_end; ++k)
                deleted.insert(texts.front()[k]);
            for (std::uint32_t k = h.b_begin; k < h.b_end; ++k)
                inserted.insert(texts.back()[k]);
        }
        for (const std::uint32_t id : deleted)
        {
            if (inserted.count(id) && !names[id].empty())
                ++moved_by[id];
        }
    }
    // A line both moved that the merged text has more often than every input or less often than every one of them
    // (twice or lost), and not as often as the ancestor plus the changes of the count by each version (copies of a
    // repeated line - "end", "</item>", "cost = 100" - added or deleted by each at its own place)
    const std::vector<std::uint32_t> result = ids_of(merged);
    for (const auto& [id, n] : moved_by)
    {
        if (n != 2)
            continue;
        const auto count = [id = id](const std::vector<std::uint32_t>& seq)
        { return int(std::count(seq.begin(), seq.end(), id)); };
        const int anc = count(texts[0]);
        int lo = INT_MAX;
        int hi = 0;
        int sum = anc;
        for (const std::vector<std::uint32_t>& t : texts)
        {
            const int c = count(t);
            lo = std::min(lo, c);
            hi = std::max(hi, c);
            sum += c - anc;
        }
        const int got = count(result);
        if ((got > hi || got < lo) && got != sum)
            return true;
    }
    return false;
}

namespace
{
void ChangedList(std::string_view base_text, const std::vector<Seg>& base, std::string_view text,
    const std::vector<Seg>& list, std::vector<ChangeRec>& out)
{
    const std::vector<std::string> base_keys = ListKeys(base, nullptr);
    std::set<std::string> base_key_set;
    for (std::size_t i = 0; i < base.size(); ++i)
    {
        if (!base[i].gap)
            base_key_set.insert(base_keys[i]);
    }
    const std::vector<std::string> keys = ListKeys(list, &base_key_set);
    std::map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < keys.size(); ++i)
        index.emplace(keys[i], i);
    std::set<std::string> known;
    for (std::size_t i = 0; i < base.size(); ++i)
    {
        const Seg& b = base[i];
        known.insert(base_keys[i]);
        const auto it = index.find(base_keys[i]);
        const std::string label = b.gap ? GapLabel(b, base_keys[i]) : b.label;
        if (it == index.end())
        {
            if (!(b.gap && b.cmp.empty()))
                out.push_back({label, AtomOp::Delete, b.line, !b.children.empty()});
            continue;
        }
        const Seg& s = list[it->second];
        if (s.cmp == b.cmp)
            continue;
        const bool container = !b.children.empty() && !s.children.empty();
        out.push_back({label, AtomOp::Modify, s.line, container});
        if (container)
            ChangedList(base_text, b.children, text, s.children, out);
    }
    for (std::size_t i = 0; i < list.size(); ++i)
    {
        if (known.count(keys[i]))
            continue;
        const Seg& s = list[i];
        if (s.gap && s.cmp.empty())
            continue;
        out.push_back({s.gap ? GapLabel(s, keys[i]) : s.label, AtomOp::Add, s.line, !s.children.empty()});
    }
}
} // namespace

void ChangedAtoms(std::string_view base_text, const Doc& base_doc, std::string_view text, const Doc& doc,
    std::vector<ChangeRec>& out)
{
    ChangedList(base_text, base_doc.top, text, doc.top, out);
}
} // namespace xray::content_merge::atoms
