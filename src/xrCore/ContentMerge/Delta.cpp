// S9: the delta of an addon of the catalog and its carrying over onto the current base (15-merge-implementation.md
// sections 2.3-2.4, 7.2; 00-roadmap.md 5a, decisions 48, 50).
//
// BuildDelta(ancestor, addon): the segments of both versions (Segments.h) are compared list by list, the keys of the
// lists as the merge has them (atoms::ListKeys). A container both have (an ltx section, an xml node) is compared by
// its children; any other changed segment - an atom of the delta with its text in the ancestor and in the addon. A
// changed gap (the code between the atoms: if ... end, a call at the top of a Lua script, the preamble of an ltx) is
// a pseudo atom "@gap ...". The anchor of an atom - the nearest segment above it in the addon that the ancestor has.
// The check without losses: the delta applied to the ancestor must give the file of the addon byte to byte;
// otherwise the file goes as one atom (kind "lines").
//
// ApplyDelta(base, delta): two pseudo texts are made from the current base - the ancestor' (the atoms of the delta set
// to their texts in the ancestor) and the addon' (set to their new texts; an absent atom goes after its anchor) - and
// the addon' is merged with the base against the ancestor' by the merge of the atoms (AtomMerge.cpp), the addon on
// top. That gives the table of 15 section 2.4:
//   modify / delete, base == ancestor           -> the new text / the atom goes;
//   modify / add,    base == new                -> the base (converging);
//   modify,          base != ancestor, divisible -> diff3 by lines (ancestor, base, new); an overlap - a conflict;
//   modify,          base != ancestor, indivisible -> a conflict;
//   modify,          the base has no such atom  -> a conflict base_deleted: the atom comes back after its anchor;
//   delete,          base != ancestor           -> a conflict: the atom goes;
//   delete,          the base has no such atom  -> nothing;
//   add,             the base has no such atom  -> the new text after the anchor;
//   add,             base != new                -> a conflict both_added: the new text.
// A conflict is won by the addon as a whole (it replaced the file: without S9 its version would be served too). The
// atoms the delta does not have come from the base: a change of the base is not undone.
// The code between the atoms of a Lua script (the rule of the top-level code, review 2026-11-06 section 4.1 item 2):
// a gap atom of the delta whose code the base changed too, otherwise than the addon (or has under another key
// changed), is a conflict, the base keeps its code; the merge of ancestor', base and addon' makes a region of the code
// both changed differently a conflict as a whole (atoms::MergeDocs). Only there (decision 64 item 5): a gap of an ltx
// or xml list the base changed too goes by its key when diff3 (ancestor, base, new) merges it and the base did not
// move its lines into another gap (atoms::GapChangeMoved), else by its hunks where the base has their lines
// (atoms::GapChangeEdits), else a conflict, the base keeps its lines.
// A delta by lines (kind "lines") is diff3 of the whole file (ancestor, base, addon); an overlap, a line the base and
// the addon both moved that the result has twice or lost (atoms::MovesCollide), or (a Lua script) the top level both
// changed (more than the texts of the atoms: TopCode) is a conflict: the file of the addon.
// The formatting atoms (only comments, spaces) are put into the base first and only where the base has the text of
// the ancestor as is.

#include "ContentMerge.h"
#include "AtomMerge.h"
#include "DeltaJson.h"
#include "Diff.h"
#include "Segments.h"
#include "TextNorm.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <map>
#include <set>

namespace xray::content_merge
{
namespace
{
std::string_view DeltaView(const String& s)
{
    return std::string_view(s.data(), s.size());
}

String ToStr(std::string_view s)
{
    return String(s.data(), s.size());
}

std::string Std(const String& s)
{
    return std::string(s.data(), s.size());
}

std::string_view SegText(std::string_view text, const Seg& seg)
{
    return text.substr(seg.begin, seg.end - seg.begin);
}

void Terminate(std::string& text)
{
    if (!text.empty() && text.back() != '\n')
        text += '\n';
}

constexpr const char* kLinesKind = "lines";
constexpr const char* kLinesAtom = "@lines";
constexpr const char* kGapPrefix = "@gap ";
constexpr const char* kStartAnchor = "^";

std::string DeltaLabel(const Seg& seg, const std::string& key)
{
    return seg.gap ? kGapPrefix + atoms::GapLabel(seg, key) : seg.label;
}

// ---- BuildDelta -----------------------------------------------------------------------------------------------------

struct Builder
{
    std::string_view anc_text;
    std::string_view add_text;
    Vector<DeltaAtom> out;

    static Vector<String> PathOf(const std::vector<std::string>& path, const std::string& key)
    {
        Vector<String> p;
        for (const std::string& k : path)
            p.push_back(ToStr(k));
        p.push_back(ToStr(key));
        return p;
    }

    bool code_gaps{}; // the top list is Lua code (Doc::code_gaps)

    void List(const std::vector<Seg>& anc, const std::vector<Seg>& add, std::vector<std::string>& path)
    {
        const std::vector<std::string> anc_keys = atoms::ListKeys(anc, nullptr);
        std::set<std::string> anc_atoms;
        for (std::size_t i = 0; i < anc.size(); ++i)
        {
            if (!anc[i].gap)
                anc_atoms.insert(anc_keys[i]);
        }
        const std::set<std::string> anc_all(anc_keys.begin(), anc_keys.end());
        std::vector<std::string> add_keys = atoms::ListKeys(add, &anc_atoms);
        // the code between the atoms only renamed by the atom above it keeps its key (not deleted and added again)
        if (code_gaps)
            atoms::RekeyMovedGaps(anc, anc_keys, add, add_keys);
        std::map<std::string, std::size_t> add_index;
        for (std::size_t j = 0; j < add_keys.size(); ++j)
            add_index.emplace(add_keys[j], j);

        // The anchor of each segment of the addon: the segment right above it (a new one too: the new segments of a
        // run go one after another); the label of the atom above it (for the reader)
        std::vector<std::string> anchor(add.size());
        std::vector<std::string> after(add.size());
        std::string last_after;
        for (std::size_t j = 0; j < add.size(); ++j)
        {
            anchor[j] = j ? add_keys[j - 1] : std::string(kStartAnchor);
            after[j] = last_after;
            if (!add[j].gap)
                last_after = add[j].label;
        }

        // The ancestor in its order: deleted, changed (a container both have - by its children)
        for (std::size_t i = 0; i < anc.size(); ++i)
        {
            const Seg& a = anc[i];
            const std::string& key = anc_keys[i];
            const auto it = add_index.find(key);
            if (it == add_index.end())
            {
                DeltaAtom atom;
                atom.atom = ToStr(DeltaLabel(a, key));
                atom.op = AtomOp::Delete;
                atom.path = PathOf(path, key);
                atom.format = a.gap && a.cmp.empty();
                atom.ancestor = ToStr(SegText(anc_text, a));
                out.push_back(std::move(atom));
                continue;
            }
            const std::size_t j = it->second;
            const Seg& s = add[j];
            if (SegText(anc_text, a) == SegText(add_text, s))
                continue;
            if (!a.children.empty() && !s.children.empty())
            {
                path.push_back(key);
                List(a.children, s.children, path);
                path.pop_back();
                continue;
            }
            DeltaAtom atom;
            atom.atom = ToStr(DeltaLabel(s, key));
            atom.op = AtomOp::Modify;
            atom.path = PathOf(path, key);
            atom.after = ToStr(after[j]);
            atom.anchor = ToStr(anchor[j]);
            atom.format = a.cmp == s.cmp;
            atom.ancestor = ToStr(SegText(anc_text, a));
            atom.text = ToStr(SegText(add_text, s));
            out.push_back(std::move(atom));
        }
        // The new segments of the addon in its order (the insertions at one anchor keep it)
        for (std::size_t j = 0; j < add.size(); ++j)
        {
            if (anc_all.count(add_keys[j]))
                continue;
            const Seg& s = add[j];
            DeltaAtom atom;
            atom.atom = ToStr(DeltaLabel(s, add_keys[j]));
            atom.op = AtomOp::Add;
            atom.path = PathOf(path, add_keys[j]);
            atom.after = ToStr(after[j]);
            atom.anchor = ToStr(anchor[j]);
            atom.format = s.gap && s.cmp.empty();
            atom.text = ToStr(SegText(add_text, s));
            out.push_back(std::move(atom));
        }
    }
};

DeltaFile LinesDelta(DeltaFile file, const std::string& anc, const std::string& add, const std::string& why)
{
    file.kind = kLinesKind;
    file.note = ToStr(why);
    file.atoms.clear();
    DeltaAtom atom;
    atom.atom = kLinesAtom;
    atom.op = AtomOp::Modify;
    atom.anchor = kStartAnchor;
    atom.ancestor = ToStr(anc);
    atom.text = ToStr(add);
    file.atoms.push_back(std::move(atom));
    return file;
}

// ---- ApplyDelta -----------------------------------------------------------------------------------------------------

struct Edit
{
    std::uint32_t begin{};
    std::uint32_t end{};
    std::string text;
    std::size_t order{};
};

// The edits in the order of the text; an insertion at a position goes before a replacement that starts there, the
// insertions at one position keep their order. An edit overlapping the one before it is dropped (counted)
std::string ApplyEdits(std::string_view text, std::vector<Edit> edits, std::size_t* dropped)
{
    std::stable_sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) {
        if (a.begin != b.begin)
            return a.begin < b.begin;
        const bool a_insert = a.end == a.begin;
        const bool b_insert = b.end == b.begin;
        if (a_insert != b_insert)
            return a_insert;
        return a.order < b.order;
    });
    std::string out;
    out.reserve(text.size());
    std::uint32_t cursor = 0;
    for (const Edit& e : edits)
    {
        if (e.begin < cursor)
        {
            if (dropped)
                ++*dropped;
            continue;
        }
        out.append(text.data() + cursor, e.begin - cursor);
        out += e.text;
        cursor = e.end;
    }
    out.append(text.data() + cursor, text.size() - cursor);
    return out;
}

// Where an atom of the delta is in a document: its list (nullptr - a container of the path is not there), the keys of
// the list (ListKeys against itself) and its index (-1 - the list has no such key)
struct Place
{
    std::string parent; // the keys of the containers above the list (the key of the inserted atoms)
    const std::vector<Seg>* list{};
    std::vector<std::string> keys;
    int index{-1};
};

Place Locate(const Doc& doc, const Vector<String>& path)
{
    Place place;
    if (path.empty())
        return place;
    const std::vector<Seg>* list = &doc.top;
    for (std::size_t k = 0; k + 1 < path.size(); ++k)
    {
        const std::string key = Std(path[k]);
        const auto it = std::find_if(list->begin(), list->end(),
            [&key](const Seg& seg) { return !seg.gap && seg.key == key; });
        if (it == list->end())
            return place;
        list = &it->children;
        place.parent += key;
        place.parent += '\x1f';
    }
    place.list = list;
    place.keys = atoms::ListKeys(*list, nullptr);
    const std::string leaf = Std(path.back());
    const auto it = std::find(place.keys.begin(), place.keys.end(), leaf);
    if (it != place.keys.end())
        place.index = int(it - place.keys.begin());
    return place;
}

// The positions of the atoms the delta inserts: the parent of the list + the key -> the position (the insertions at
// one position keep the order of the delta, so an atom anchored at a new one goes after it)
using Inserted = std::map<std::string, std::uint32_t>;

// The position after the anchor of an atom in its list: the anchor itself (in the base, or an atom of the delta
// inserted before); for a gap key "~<atom>#<n>" the base does not have - after that atom; else the start of the list.
// exact - the anchor (or the atom of a gap anchor) was found
std::uint32_t AnchorPos(const Place& place, const std::string& anchor, const Inserted& inserted, bool& exact)
{
    exact = false;
    const std::vector<Seg>& list = *place.list;
    if (anchor != kStartAnchor)
    {
        const auto it = std::find(place.keys.begin(), place.keys.end(), anchor);
        if (it != place.keys.end())
        {
            exact = true;
            return list[std::size_t(it - place.keys.begin())].end;
        }
        const auto new_one = inserted.find(place.parent + anchor);
        if (new_one != inserted.end())
        {
            exact = true;
            return new_one->second;
        }
        if (anchor.size() > 1 && anchor[0] == '~')
        {
            const std::size_t hash = anchor.rfind('#');
            const std::string atom = anchor.substr(1, hash == std::string::npos ? std::string::npos : hash - 1);
            const auto seg = std::find_if(list.begin(), list.end(),
                [&atom](const Seg& s) { return !s.gap && s.key == atom; });
            if (seg != list.end())
            {
                exact = true;
                return seg->end;
            }
        }
        // The anchor is not in the base (deleted, renamed). In a container - at its end (an xml node: before its end
        // tag, an ltx section: after its last key), at the top - after the preamble (15 section 3.3)
        if (list.empty())
            return 0;
        if (!place.parent.empty())
        {
            // before the text right before the end tag (the line end and the indent of the end tag stay after it)
            const auto close = std::find_if(list.begin(), list.end(), [](const Seg& s) { return s.key == "</"; });
            if (close == list.end())
                return list.back().end;
            return close != list.begin() && std::prev(close)->gap ? std::prev(close)->begin : close->begin;
        }
        return list.front().gap ? list.front().end : list.front().begin;
    }
    exact = true;
    return list.empty() ? 0 : list.front().begin;
}

String ReasonOf(AtomOp op, bool present)
{
    if (op == AtomOp::Add)
        return "both_added";
    return present ? "both_changed" : "base_deleted";
}

Conflict RebaseConflict(const std::string& atom, std::string_view owner, const String& reason, std::uint32_t line)
{
    Conflict c;
    c.atom = ToStr(atom);
    c.owners.push_back(ToStr(owner));
    c.winner = ToStr(owner);
    c.resolved_by = "rebase";
    c.lost.push_back(kBaseOwner);
    c.base_line = line;
    c.reason = reason;
    return c;
}

// The top level of a Lua script: the code between the atoms as its lines of code (spaces collapsed, the empty lines
// and the comment lines skipped), an atom (a function, x = 1, local t = {...}) as one line of its key (not its text),
// in their order: the code changed, a call moved over an atom, an atom added, deleted or moved change it; a new text
// of an atom does not. false - the parser does not understand the text
bool TopCode(std::string_view text, const std::string& name, std::vector<std::string>& out)
{
    const Doc doc = ParseKind(Kind::Lua, text, name);
    if (!doc.ok)
        return false;
    for (const Seg& seg : doc.top)
    {
        if (!seg.gap)
        {
            out.push_back("\x01" + seg.key); // a line of code never starts with this byte
            continue;
        }
        for (const diff::Line& line : diff::SplitLines(SegText(text, seg), 0))
        {
            const String c = text::CollapseSpaces(line.text);
            if (!c.empty() && c.rfind("--", 0) != 0)
                out.emplace_back(c.data(), c.size());
        }
    }
    return true;
}

// The rule of the top-level code for a delta by lines of a Lua script (review 2026-11-06 section 4.1 item 2): the base
// and the addon both changed the top level (TopCode: more than the texts of the atoms), differently - a conflict (no
// merge by lines of it: a moved call or assignment and a changed one could both be kept). A text the parser does not
// understand - the same
bool TopCodeCollides(std::string_view anc, std::string_view base, std::string_view addon, const std::string& name)
{
    std::vector<std::string> code[3];
    const std::string_view texts[3] = {anc, base, addon};
    for (int k = 0; k < 3; ++k)
    {
        if (!TopCode(texts[k], name, code[k]))
            return true;
    }
    return code[1] != code[0] && code[2] != code[0] && code[1] != code[2];
}

RebaseResult ApplyLines(std::string base, const DeltaFile& file, std::string_view owner)
{
    RebaseResult r;
    if (file.atoms.size() != 1)
    {
        r.error = "a delta of kind lines must have one atom @lines";
        return r;
    }
    const DeltaAtom& atom = file.atoms.front();
    r.ok = true;
    r.atoms = 1;
    if (base == DeltaView(atom.ancestor))
    {
        r.text = atom.text;
        return r;
    }
    std::string anc = Std(atom.ancestor);
    std::string text = Std(atom.text);
    Terminate(anc);
    Terminate(text);
    Terminate(base);
    const diff::Merge3Result m = diff::Merge3(diff::SplitLines(anc, 0), diff::SplitLines(base, 1),
        diff::SplitLines(text, 2), true, true);
    // A line the base and the addon both moved: diff3 keeps both insertions (the line twice) - a conflict too. A Lua
    // script: the top level both changed - a conflict (the rule of the top-level code)
    const std::string name = Std(file.vpath);
    if (m.conflict || atoms::MovesCollide(anc, base, text, diff::JoinLines(m.lines)) ||
        (KindOfName(name) == Kind::Lua && TopCodeCollides(anc, base, text, name)))
    {
        r.text = ToStr(text);
        r.conflicts.push_back(RebaseConflict(kLinesAtom, owner, "both_changed", 1));
    }
    else
        r.text = ToStr(diff::JoinLines(m.lines));
    if (!file.final_eol && !r.text.empty() && r.text.back() == '\n')
        r.text.pop_back();
    return r;
}

// The texts of a list of atoms in the base: present ones are replaced, absent ones inserted after their anchors
struct Planned
{
    const DeltaAtom* atom{};
    bool present{};
    std::uint32_t begin{};
    std::uint32_t end{};
    std::uint32_t line{};
};

RebaseResult ApplyAtoms(std::string base, Kind kind, const DeltaFile& file, std::string_view owner)
{
    RebaseResult r;
    const std::string name = Std(file.vpath);
    Terminate(base);
    Doc doc = ParseKind(kind, base, name);
    if (!doc.ok)
    {
        r.error = "the current base is not understood by the parser: " + ToStr(doc.error);
        return r;
    }

    // 1. The formatting changes and deletions go into the base where it has the text of the ancestor as is (the
    // formatting insertions go with the other insertions below: their anchors may be new atoms)
    std::vector<Edit> format_edits;
    for (std::size_t n = 0; n < file.atoms.size(); ++n)
    {
        const DeltaAtom& atom = file.atoms[n];
        if (!atom.format)
            continue;
        const Place place = Locate(doc, atom.path);
        if (!place.list)
            continue;
        if (place.index >= 0 && atom.op != AtomOp::Add)
        {
            const Seg& seg = (*place.list)[std::size_t(place.index)];
            if (SegText(base, seg) != DeltaView(atom.ancestor))
                continue;
            format_edits.push_back({seg.begin, seg.end, atom.op == AtomOp::Delete ? std::string() : Std(atom.text), n});
        }
    }
    if (!format_edits.empty())
    {
        base = ApplyEdits(base, std::move(format_edits), nullptr);
        doc = ParseKind(kind, base, name);
        if (!doc.ok)
        {
            r.error = "the base with the formatting of the addon is not understood by the parser: " + ToStr(doc.error);
            return r;
        }
    }

    // 2. The ancestor' and the addon' made of the base. The gaps the atoms of the delta take by their keys: the
    // code of another gap is not looked for in them (a gap the addon moved is deleted under its old key and added
    // under the new one)
    std::map<const std::vector<Seg>*, std::set<std::size_t>> claimed;
    for (const DeltaAtom& atom : file.atoms)
    {
        const Place place = Locate(doc, atom.path);
        if (place.list && place.index >= 0 && (*place.list)[std::size_t(place.index)].gap)
            claimed[place.list].insert(std::size_t(place.index));
    }
    std::vector<Edit> anc_edits;
    std::vector<Edit> add_edits;
    std::map<std::string, Planned> by_label; // the merge label -> the atom (the conflicts of the merge below)
    Inserted inserted;
    // The base keeps its code, the change of the addon is lost (a copy of it could run the code twice)
    const auto base_wins = [&r, owner](const std::string& label, const char* reason, std::uint32_t line)
    {
        Conflict c = RebaseConflict(label, owner, reason, line);
        c.winner = kBaseOwner;
        c.lost = {ToStr(owner)};
        r.conflicts.push_back(std::move(c));
    };
    for (std::size_t n = 0; n < file.atoms.size(); ++n)
    {
        const DeltaAtom& atom = file.atoms[n];
        if (atom.format && atom.op != AtomOp::Add)
            continue;
        const Place place = Locate(doc, atom.path);
        if (atom.format)
        {
            // A formatting insertion (comments, empty lines): where the base has nothing under its key
            if (place.list && place.index < 0)
            {
                bool exact = false;
                const std::uint32_t pos = AnchorPos(place, Std(atom.anchor), inserted, exact);
                add_edits.push_back({pos, pos, Std(atom.text), n});
                inserted[place.parent + Std(atom.path.back())] = pos;
            }
            continue;
        }
        const std::string label = Std(atom.atom);
        if (!place.list)
        {
            // A container of the path is not in the base any more (a section, a node): the atom has no place, the
            // change of the addon is lost - the base wins this one
            if (atom.op != AtomOp::Delete)
                base_wins(label, "base_deleted", 0);
            continue;
        }
        const bool gap = label.rfind(kGapPrefix, 0) == 0;
        // The code between the atoms of a Lua script (the top list): the rule of the top-level code. The gaps of an
        // ltx or xml list (the lines before the first section, a repeated key, the text of a node) are not code that
        // runs: diff3 merges the changes of the base and of the addon in one gap (decision 64 item 5)
        const bool top_code = gap && kind == Kind::Lua && place.parent.empty();
        Planned planned;
        planned.atom = &atom;
        bool present = place.index >= 0;
        if (present)
        {
            const Seg& seg = (*place.list)[std::size_t(place.index)];
            planned.begin = seg.begin;
            planned.end = seg.end;
            planned.line = seg.line;
            const std::string_view now = SegText(base, seg);
            // The rule of the top-level code (review 2026-11-06 section 4.1 item 2): the code between the atoms the
            // base changed too, otherwise than the addon (or a new gap of the addon where the base has code of its
            // own) - a conflict of the gap, the base keeps its code. No merge by lines: a line the base moved out of
            // the gap would come back with the text of the addon - the code twice
            if (top_code && now != DeltaView(atom.ancestor) &&
                (atom.op == AtomOp::Delete || now != DeltaView(atom.text)))
            {
                base_wins(label, atom.op == AtomOp::Add ? "both_added" : "both_changed", seg.line);
                continue;
            }
            // A gap of an ltx / xml list the base changed too goes by the key only when diff3 merges it and the base
            // did not move its lines to another gap. Otherwise the addon would win the gap as a whole and bring back
            // the lines the base has elsewhere - the lines twice; the change is carried by its hunks below, or the
            // base keeps its lines
            if (gap && !top_code && atom.op == AtomOp::Modify && now != DeltaView(atom.ancestor))
            {
                const diff::Merge3Result m = diff::Merge3(diff::SplitLines(DeltaView(atom.ancestor), 0),
                    diff::SplitLines(now, 1), diff::SplitLines(DeltaView(atom.text), 2), true, true);
                present = !m.conflict &&
                    !atoms::GapChangeMoved(*place.list, base, std::size_t(place.index), DeltaView(atom.ancestor),
                        DeltaView(atom.text));
            }
        }
        if (!present && gap && atom.op != AtomOp::Add)
        {
            // A gap the base has under another key (an atom added, deleted or renamed near it, two gaps joined): its
            // text as is in a gap of the base (not in the gaps other atoms of the delta take by their keys)
            std::set<std::size_t> skip = claimed[place.list];
            if (place.index >= 0)
                skip.erase(std::size_t(place.index));
            present = atoms::FindGapText(*place.list, base, Std(atom.path.back()), DeltaView(atom.ancestor),
                planned.begin, planned.end, &skip);
            planned.line = present ? LineOf(base, planned.begin) : 0;
            if (!present)
            {
                std::vector<atoms::TextEdit> hunks;
                if (!top_code &&
                    atoms::GapChangeEdits(
                        *place.list, base, DeltaView(atom.ancestor), DeltaView(atom.text), hunks, &skip))
                {
                    // An ltx / xml gap: the change by its hunks, where the lines it changes are in the base (patch):
                    // the base keeps the rest of its lines as they are
                    for (atoms::TextEdit& e : hunks)
                        add_edits.push_back({e.begin, e.end, std::move(e.text), n});
                    if (!hunks.empty())
                    {
                        planned.present = true;
                        planned.begin = hunks.front().begin;
                        planned.end = hunks.back().end;
                        planned.line = LineOf(base, planned.begin);
                        ++r.atoms;
                        by_label[label.substr(std::char_traits<char>::length(kGapPrefix))] = planned;
                    }
                    continue;
                }
                // The base changed those lines or split them by an atom: a copy put at the anchor would give them
                // twice (the code of a Lua script would run twice)
                if (atom.op == AtomOp::Modify)
                    base_wins(label, place.index >= 0 ? "both_changed" : "base_deleted", 0);
                continue; // a deletion: the base deleted (or changed) it too
            }
        }
        if (present)
        {
            planned.present = true;
            anc_edits.push_back(
                {planned.begin, planned.end, atom.op == AtomOp::Add ? std::string() : Std(atom.ancestor), n});
            add_edits.push_back(
                {planned.begin, planned.end, atom.op == AtomOp::Delete ? std::string() : Std(atom.text), n});
        }
        else
        {
            if (atom.op == AtomOp::Delete)
                continue; // the base deleted it too
            bool exact = false;
            std::uint32_t pos = AnchorPos(place, Std(atom.anchor), inserted, exact);
            if (!exact && atom.op == AtomOp::Add && kind == Kind::Lua && place.parent.empty())
            {
                // The anchor is not in the base (deleted, renamed): the place of the atom is a guess - a conflict. A
                // definition goes after the preamble, the code (a gap) to the end of the file: it runs after the
                // definitions it may call
                if (gap)
                    pos = std::uint32_t(base.size());
                Conflict c = RebaseConflict(label, owner, "base_deleted", 0);
                c.text = ToStr("the anchor '" + Std(atom.anchor) + "' is not in the base: the atom is put at the " +
                    (gap ? "end" : "start") + " of the file");
                r.conflicts.push_back(std::move(c));
            }
            planned.begin = planned.end = pos;
            if (atom.op == AtomOp::Modify)
                anc_edits.push_back({pos, pos, Std(atom.ancestor), n});
            add_edits.push_back({pos, pos, Std(atom.text), n});
            inserted[place.parent + Std(atom.path.back())] = pos;
        }
        ++r.atoms;
        by_label[gap ? label.substr(std::char_traits<char>::length(kGapPrefix)) : label] = planned;
    }
    std::size_t dropped = 0;
    const std::string anc = ApplyEdits(base, anc_edits, &dropped);
    std::string add = ApplyEdits(base, add_edits, &dropped);
    if (dropped)
    {
        r.error = "the atoms of the delta overlap (" + ToStr(std::to_string(dropped)) + " dropped): a broken delta";
        return r;
    }

    // 3. The base did not change any atom of the delta: the addon' is the result, else the merge (ancestor', base,
    // addon'), the addon on top
    if (anc != base)
    {
        std::vector<Doc> docs;
        bool ok = true;
        for (const std::string_view text : {std::string_view(anc), std::string_view(base), std::string_view(add)})
        {
            docs.push_back(ParseKind(kind, text, name));
            ok = ok && docs.back().ok;
        }
        if (!ok)
        {
            docs.clear();
            for (const std::string_view text : {std::string_view(anc), std::string_view(base), std::string_view(add)})
                docs.push_back(WholeTextDoc(text, ModuleOfName(name)));
        }
        std::vector<atoms::Version> versions(2);
        versions[0].text = base;
        versions[0].doc = &docs[1];
        versions[1].text = add;
        versions[1].doc = &docs[2];
        const std::vector<std::string> owner_names = {"ancestor", kBaseOwner, std::string(owner)};
        const atoms::Outcome outcome = atoms::MergeDocs(anc, docs[0], versions, {}, Options{}, owner_names);
        for (const atoms::ConflictRec& rec : outcome.conflicts)
        {
            const auto it = by_label.find(rec.atom);
            if (it == by_label.end())
            {
                r.conflicts.push_back(RebaseConflict(rec.atom, owner, "both_changed", rec.base_line));
                continue;
            }
            const Planned& p = it->second;
            r.conflicts.push_back(RebaseConflict(Std(p.atom->atom), owner, ReasonOf(p.atom->op, p.present),
                p.present ? p.line : 0));
        }
        add = outcome.Text();
    }
    r.ok = true;
    r.text = ToStr(add);
    if (!file.final_eol && !r.text.empty() && r.text.back() == '\n')
        r.text.pop_back();
    return r;
}

// ---- JSON -----------------------------------------------------------------------------------------------------------

json::Value AtomToValue(const DeltaAtom& atom)
{
    json::Value v = json::Value::MakeObject();
    v.Add("atom", json::Value::MakeString(DeltaView(atom.atom)));
    v.Add("op", json::Value::MakeString(AtomOpName(atom.op)));
    json::Value path = json::Value::MakeArray();
    for (const String& key : atom.path)
        path.items.push_back(json::Value::MakeString(DeltaView(key)));
    v.Add("path", std::move(path));
    if (atom.op != AtomOp::Delete)
    {
        v.Add("after", json::Value::MakeString(DeltaView(atom.after)));
        v.Add("anchor", json::Value::MakeString(DeltaView(atom.anchor)));
    }
    if (atom.format)
        v.Add("format", json::Value::MakeBool(true));
    if (atom.op != AtomOp::Add)
        v.Add("ancestor", json::Value::MakeString(DeltaView(atom.ancestor)));
    if (atom.op != AtomOp::Delete)
        v.Add("text", json::Value::MakeString(DeltaView(atom.text)));
    return v;
}

json::Value FileToValue(const DeltaFile& file)
{
    json::Value v = json::Value::MakeObject();
    v.Add("kind", json::Value::MakeString(DeltaView(file.kind)));
    if (!file.ancestor_sha256.empty())
        v.Add("ancestor_sha256", json::Value::MakeString(DeltaView(file.ancestor_sha256)));
    v.Add("addon_sha256", json::Value::MakeString(DeltaView(file.addon_sha256)));
    if (file.same_as_ancestor)
        v.Add("same_as_ancestor", json::Value::MakeBool(true));
    if (file.not_in_ancestor)
    {
        v.Add("not_in_ancestor", json::Value::MakeBool(true));
        return v;
    }
    if (!file.final_eol)
        v.Add("final_eol", json::Value::MakeBool(false));
    if (!file.note.empty())
        v.Add("note", json::Value::MakeString(DeltaView(file.note)));
    json::Value list = json::Value::MakeArray();
    for (const DeltaAtom& atom : file.atoms)
        list.items.push_back(AtomToValue(atom));
    v.Add("atoms", std::move(list));
    return v;
}

bool OpOfName(const std::string& name, AtomOp& op)
{
    if (name == "add")
        op = AtomOp::Add;
    else if (name == "modify")
        op = AtomOp::Modify;
    else if (name == "delete")
        op = AtomOp::Delete;
    else
        return false;
    return true;
}

bool AtomFromValue(const json::Value& v, DeltaAtom& atom, std::string& error)
{
    if (v.type != json::Type::Object)
    {
        error = "an atom is not an object";
        return false;
    }
    // A member of another type would read as its default ("", false) and change the meaning of the delta silently
    const auto typed = [&v, &error](const char* name, json::Type type) {
        const json::Value* member = v.Find(name);
        if (!member || member->type == type)
            return true;
        error = "atom '" + v.StringOf("atom") + "': \"" + name + "\" is of a wrong type";
        return false;
    };
    if (!typed("atom", json::Type::String) || !typed("op", json::Type::String) ||
        !typed("path", json::Type::Array) || !typed("after", json::Type::String) ||
        !typed("anchor", json::Type::String) || !typed("format", json::Type::Bool))
        return false;
    atom.atom = ToStr(v.StringOf("atom"));
    if (atom.atom.empty())
    {
        error = "an atom without a label (\"atom\")";
        return false;
    }
    if (!OpOfName(v.StringOf("op"), atom.op))
    {
        error = "atom '" + v.StringOf("atom") + "': a bad op '" + v.StringOf("op") + "'";
        return false;
    }
    if (const json::Value* path = v.Find("path"))
    {
        for (const json::Value& key : path->items)
        {
            if (key.type != json::Type::String || key.text.empty())
            {
                error = "atom '" + v.StringOf("atom") + "': a key of the path is not a non-empty string";
                return false;
            }
            atom.path.push_back(ToStr(key.text));
        }
    }
    if (atom.path.empty() && atom.atom != kLinesAtom)
    {
        error = "atom '" + v.StringOf("atom") + "': no path";
        return false;
    }
    atom.after = ToStr(v.StringOf("after"));
    atom.anchor = ToStr(v.StringOf("anchor", kStartAnchor));
    atom.format = v.BoolOf("format");
    const json::Value* ancestor = v.Find("ancestor");
    const json::Value* text = v.Find("text");
    if ((atom.op != AtomOp::Add && (!ancestor || ancestor->type != json::Type::String)) ||
        (atom.op != AtomOp::Delete && (!text || text->type != json::Type::String)))
    {
        error = "atom '" + v.StringOf("atom") + "': no ancestor / text for op " + v.StringOf("op");
        return false;
    }
    if (ancestor)
        atom.ancestor = ToStr(ancestor->text);
    if (text)
        atom.text = ToStr(text->text);
    return true;
}

String LowerPath(std::string_view vpath)
{
    String out = text::ToLower(vpath);
    std::replace(out.begin(), out.end(), '/', '\\');
    return out;
}
} // namespace

String NormalizedSha256(std::string_view bytes)
{
    const String text = Normalize(bytes);
    return Sha256Hex(DeltaView(text));
}

DeltaFile BuildDelta(Kind kind, std::string_view vpath, std::string_view ancestor, std::string_view addon)
{
    DeltaFile file;
    file.vpath = LowerPath(vpath);
    file.kind = KindName(kind);
    const std::string anc = Std(Normalize(ancestor));
    const std::string add = Std(Normalize(addon));
    file.ancestor_sha256 = Sha256Hex(anc);
    file.addon_sha256 = Sha256Hex(add);
    file.final_eol = add.empty() || add.back() == '\n';
    if (anc == add)
    {
        file.same_as_ancestor = true;
        return file;
    }
    if (kind == Kind::Unknown)
        return LinesDelta(std::move(file), anc, add, "not a text of the merge (ltx, xml, script)");

    std::string anc_t = anc;
    std::string add_t = add;
    Terminate(anc_t);
    Terminate(add_t);
    const std::string name = Std(file.vpath);
    const Doc anc_doc = ParseKind(kind, anc_t, name);
    const Doc add_doc = ParseKind(kind, add_t, name);
    if (!anc_doc.ok || !add_doc.ok)
    {
        return LinesDelta(std::move(file), anc, add,
            "the parser failed on the " + std::string(anc_doc.ok ? "addon" : "ancestor") + ": " +
                (anc_doc.ok ? add_doc.error : anc_doc.error));
    }
    // The addon split, joined or moved the code between the functions: by lines (the atoms could give the code twice)
    if (atoms::GapsRestructured(anc_doc, anc_t, add_doc, add_t))
        return LinesDelta(std::move(file), anc, add, "the addon split or joined the code between the atoms");
    Builder builder;
    builder.anc_text = anc_t;
    builder.add_text = add_t;
    builder.code_gaps = anc_doc.code_gaps;
    std::vector<std::string> path;
    builder.List(anc_doc.top, add_doc.top, path);
    file.atoms = std::move(builder.out);

    // The check without losses (15 section 7.2, step 4)
#if defined(CONTENT_MERGE_DELTA_DEBUG)
    // A debug build of the stand: the atoms as they are, to see what the self-check rejects
    if (std::getenv("GW_MERGE_NO_SELF_CHECK"))
        return file;
#endif
    const RebaseResult self = ApplyDelta(anc, file, "self");
    if (!self.ok || DeltaView(self.text) != std::string_view(add) || !self.conflicts.empty())
    {
        return LinesDelta(std::move(file), anc, add,
            self.ok ? "the atoms do not give the file back (a change outside the atoms, moved atoms)" :
                      "the atoms do not apply: " + Std(self.error));
    }
    return file;
}

RebaseResult ApplyDelta(std::string_view base_bytes, const DeltaFile& file, std::string_view owner)
{
    std::string base = Std(Normalize(base_bytes));
    if (file.not_in_ancestor)
    {
        RebaseResult r;
        r.error = "the base of the author has no such file (not_in_ancestor): no delta";
        return r;
    }
    if (file.kind == kLinesKind)
        return ApplyLines(std::move(base), file, owner);
    if (file.same_as_ancestor)
    {
        // Any kind, "unknown" (BuildDelta of another file) included: the addon did not change the file
        RebaseResult r;
        r.ok = true;
        r.text = ToStr(base);
        return r;
    }
    const Kind kind = KindOfWord(DeltaView(file.kind));
    if (kind == Kind::Unknown)
    {
        RebaseResult r;
        r.error = "an unknown kind of the delta '" + file.kind + "'";
        return r;
    }
    if (file.same_as_ancestor || file.atoms.empty())
    {
        RebaseResult r;
        r.ok = true;
        r.text = ToStr(base);
        return r;
    }
    return ApplyAtoms(std::move(base), kind, file, owner);
}

bool DeltaMatchesFile(const DeltaFile& file, std::string_view addon_bytes)
{
    return !file.addon_sha256.empty() && NormalizedSha256(addon_bytes) == file.addon_sha256;
}

bool IsSha256Hex(std::string_view text)
{
    return text.size() == 64 &&
        std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

namespace
{
// The vpath of an entry: a relative name inside gamedata\ (as the file: rules of the engine check their files,
// NormalizeRuleFileText, and stricter: the separators are as DeltaFromJson writes them, '\' between the names only)
bool DeltaVpathValid(std::string_view vpath)
{
    if (vpath.empty() || vpath.size() >= 256 || vpath.find_first_of(":*?\"<>|;=/") != std::string_view::npos)
        return false;
    std::size_t start = 0;
    for (;;)
    {
        const std::size_t end = (std::min)(vpath.find('\\', start), vpath.size());
        const std::string_view part = vpath.substr(start, end - start);
        if (part.empty() || part == "." || part == "..")
            return false; // a leading or a trailing '\', an empty folder, a way out
        if (end == vpath.size())
            return true;
        start = end + 1;
    }
}
} // namespace

String DeltaFileProblem(const DeltaFile& file)
{
    if (!DeltaVpathValid(DeltaView(file.vpath)))
        return "not a file name inside gamedata\\";
    if (file.same_as_ancestor && file.not_in_ancestor)
        return "same_as_ancestor and not_in_ancestor at once";
    // BuildDelta writes kind lines only for a changed file (its one atom @lines): ApplyDelta would fail on it
    if (file.same_as_ancestor && file.kind == "lines")
        return "kind lines must have one atom @lines of op modify";
    if (!IsSha256Hex(DeltaView(file.addon_sha256)))
        return "addon_sha256 is not a sha256 (64 hex chars in lower case)";
    if (!file.not_in_ancestor && !IsSha256Hex(DeltaView(file.ancestor_sha256)))
        return "ancestor_sha256 is not a sha256 (64 hex chars in lower case)";
    if (file.same_as_ancestor || file.not_in_ancestor)
        return file.atoms.empty() ? String() : String("atoms in an entry without changes");
    const Kind kind = KindOfName(DeltaView(file.vpath));
    if (file.kind == "lines")
    {
        if (file.atoms.size() != 1)
            return "kind lines must have one atom @lines of op modify";
        const DeltaAtom& lines = file.atoms.front();
        if (lines.atom != kLinesAtom || lines.op != AtomOp::Modify)
            return "kind lines must have one atom @lines of op modify";
        return lines.ancestor == lines.text ? String("@lines changes nothing") : String();
    }
    if (kind == Kind::Unknown || file.kind != KindName(kind))
        return "kind '" + file.kind + "' is not the one of the file (" + KindName(kind) + ")";
    if (file.atoms.empty())
        return "no atoms in an entry with changes";
    std::set<std::string> paths;
    for (const DeltaAtom& atom : file.atoms)
    {
        const String& label = atom.atom;
        if (label == kLinesAtom || atom.path.empty())
            return "atom '" + label + "' is not an atom of kind " + file.kind;
        // A changed gap is "@gap <label>" and its key "~<anchor>#<n>" (ListKeys)
        const bool gap_label = DeltaView(label).rfind(kGapPrefix, 0) == 0;
        if (gap_label && DeltaView(atom.path.back()).rfind('~', 0) != 0)
            return "atom '" + label + "': a gap with the key of an atom";
        if (atom.op == AtomOp::Add && !atom.ancestor.empty())
            return "atom '" + label + "': op add with an ancestor";
        if (atom.op == AtomOp::Delete && !atom.text.empty())
            return "atom '" + label + "': op delete with a new text";
        if (atom.op == AtomOp::Modify && atom.ancestor == atom.text)
            return "atom '" + label + "': op modify that changes nothing";
        std::string key;
        for (const String& part : atom.path)
            key += Std(part) + "\x1f";
        if (!paths.insert(std::move(key)).second)
            return "atom '" + label + "': its path is there twice";
    }
    return String();
}

String DeltaFileMismatch(const DeltaFile& file, std::string_view addon_bytes)
{
    if (!DeltaMatchesFile(file, addon_bytes))
        return "the file differs from the published one (addon_sha256)";
    if (file.same_as_ancestor)
        return String(); // no atoms: the fingerprint is all there is to check
    const String text = Normalize(addon_bytes);
    if (file.kind == "lines" && !file.atoms.empty())
        return file.atoms.front().text == text ? String() : String("@lines is not the text of the file");
    // The atoms are the segments of the file with the line end at its end (BuildDelta)
    String terminated = text;
    if (!terminated.empty() && terminated.back() != '\n')
        terminated += '\n';
    for (const DeltaAtom& atom : file.atoms)
    {
        if (atom.op != AtomOp::Delete && terminated.find(atom.text) == String::npos)
            return "the new text of atom '" + atom.atom + "' is not in the file";
    }
    return String();
}

String DeltaToJson(const Delta& delta)
{
    json::Value v = json::Value::MakeObject();
    v.Add("format", json::Value::MakeNumber(delta.format));
    v.Add("algo", json::Value::MakeString(DeltaView(delta.algo)));
    v.Add("game", json::Value::MakeString(DeltaView(delta.game)));
    v.Add("guessed", json::Value::MakeBool(delta.guessed));
    json::Value files = json::Value::MakeObject();
    for (const DeltaFile& file : delta.files)
        files.Add(DeltaView(file.vpath), FileToValue(file));
    v.Add("files", std::move(files));
    return ToStr(json::Write(v));
}

String DeltaFileToJson(const DeltaFile& file)
{
    json::Value v = json::Value::MakeObject();
    v.Add(DeltaView(file.vpath), FileToValue(file));
    return ToStr(json::Write(v));
}

bool DeltaFromJson(std::string_view text, Delta& out, String& error_out)
{
    out = Delta{};
    json::Value root;
    std::string error;
    if (!json::Parse(text, root, error))
    {
        error_out = ToStr("not JSON: " + error);
        return false;
    }
    if (root.type != json::Type::Object)
    {
        error_out = "the delta is not a JSON object";
        return false;
    }
    out.format = root.NumberOf("format", 0);
    if (out.format != kDeltaFormat)
    {
        error_out = ToStr("an unsupported format " + std::to_string(out.format) + " (this one reads " +
            std::to_string(kDeltaFormat) + ")");
        return false;
    }
    for (const char* name : {"algo", "game"})
    {
        const json::Value* member = root.Find(name);
        if (member && member->type != json::Type::String)
        {
            error_out = ToStr(std::string("\"") + name + "\" is not a string");
            return false;
        }
    }
    if (const json::Value* guessed = root.Find("guessed"); guessed && guessed->type != json::Type::Bool)
    {
        error_out = "\"guessed\" is not a boolean";
        return false;
    }
    out.algo = ToStr(root.StringOf("algo"));
    out.game = ToStr(root.StringOf("game"));
    out.guessed = root.BoolOf("guessed");
    const json::Value* files = root.Find("files");
    if (!files || files->type != json::Type::Object)
    {
        error_out = "no \"files\" object";
        return false;
    }
    for (const auto& [vpath, value] : files->members)
    {
        if (value.type != json::Type::Object || vpath.empty())
        {
            error_out = ToStr("files[" + vpath + "] is not an object of a file");
            return false;
        }
        for (const auto& [name, type] : {std::pair<const char*, json::Type>{"kind", json::Type::String},
                 {"ancestor_sha256", json::Type::String}, {"addon_sha256", json::Type::String},
                 {"note", json::Type::String}, {"same_as_ancestor", json::Type::Bool},
                 {"not_in_ancestor", json::Type::Bool}, {"final_eol", json::Type::Bool},
                 {"atoms", json::Type::Array}})
        {
            const json::Value* member = value.Find(name);
            if (member && member->type != type)
            {
                error_out = ToStr("files[" + vpath + "]: \"" + name + "\" is of a wrong type");
                return false;
            }
        }
        // One file under two spellings ("Scripts/a.script", "scripts\a.script"): FindDeltaFile would take the first
        // one silently
        const String lower = LowerPath(vpath);
        if (FindDeltaFile(out, DeltaView(lower)))
        {
            error_out = ToStr("files[" + vpath + "]: the file is in the delta twice");
            return false;
        }
        DeltaFile& file = out.files.emplace_back();
        file.vpath = lower;
        file.kind = ToStr(value.StringOf("kind"));
        file.ancestor_sha256 = ToStr(value.StringOf("ancestor_sha256"));
        file.addon_sha256 = ToStr(value.StringOf("addon_sha256"));
        file.same_as_ancestor = value.BoolOf("same_as_ancestor");
        file.not_in_ancestor = value.BoolOf("not_in_ancestor");
        file.final_eol = value.BoolOf("final_eol", true);
        file.note = ToStr(value.StringOf("note"));
        if (const json::Value* list = value.Find("atoms"); list && list->type == json::Type::Array)
        {
            for (const json::Value& item : list->items)
            {
                DeltaAtom& atom = file.atoms.emplace_back();
                if (!AtomFromValue(item, atom, error))
                {
                    error_out = ToStr("files[" + vpath + "]: " + error);
                    return false;
                }
            }
        }
    }
    return true;
}

const DeltaFile* FindDeltaFile(const Delta& delta, std::string_view vpath)
{
    const String key = LowerPath(vpath);
    for (const DeltaFile& file : delta.files)
    {
        if (file.vpath == key)
            return &file;
    }
    return nullptr;
}

String AlgoFileTag(std::string_view algo)
{
    String out = ToStr(algo);
    for (char& ch : out)
    {
        if (ch == '/' || ch == '\\' || ch == ':')
            ch = '-';
    }
    return out;
}
} // namespace xray::content_merge
