// The public part of the merge (ContentMerge.h): the normalization of the inputs, the rebase of the sides made on an
// older base (S9), the merge by atoms, the check of the result and the fallback, the map of the lines.

#include "ContentMerge.h"
#include "AtomMerge.h"
#include "Segments.h"
#include "TextNorm.h"

#include <algorithm>
#include <map>

namespace xray::content_merge
{
namespace
{
std::string_view View(const String& s)
{
    return std::string_view(s.data(), s.size());
}

String ToString(std::string_view s)
{
    return String(s.data(), s.size());
}

String NormalizeOf(Kind kind, std::string_view bytes)
{
    String text = Normalize(bytes);
    if (kind == Kind::Xml)
        text = XmlDeclarationToUtf8(View(text));
    return text;
}

// Both docs parsed, or both as one gap each if a parser failed on one of them (the merge by lines)
bool ParseAll(Kind kind, std::string_view name, const std::vector<std::string_view>& texts, std::vector<Doc>& docs,
    std::string& error)
{
    docs.clear();
    bool ok = true;
    for (const std::string_view text : texts)
    {
        docs.push_back(ParseKind(kind, text, name));
        if (!docs.back().ok && ok)
        {
            ok = false;
            error = docs.back().error;
        }
    }
    if (ok)
        return true;
    docs.clear();
    for (const std::string_view text : texts)
        docs.push_back(WholeTextDoc(text, ModuleOfName(name)));
    return false;
}

void LineMap(const std::vector<atoms::Piece>& pieces, const std::vector<std::string>& owner_names,
    Vector<LineOwner>& out)
{
    std::vector<int> owners(1, 0);
    for (const atoms::Piece& piece : pieces)
    {
        for (const char ch : piece.text)
        {
            if (piece.owner != 0 && !text::IsTrimChar(ch))
                owners.back() = piece.owner;
            if (ch == '\n')
                owners.push_back(0);
        }
    }
    if (owners.size() > 1 && pieces.back().text.back() == '\n')
        owners.pop_back();
    for (std::size_t i = 0; i < owners.size(); ++i)
    {
        const String owner = ToString(owner_names[std::size_t(owners[i])]);
        if (!out.empty() && out.back().owner == owner && out.back().to_line == i)
            out.back().to_line = std::uint32_t(i + 1);
        else
            out.push_back({std::uint32_t(i + 1), std::uint32_t(i + 1), owner});
    }
}

Conflict ConflictOf(const atoms::ConflictRec& rec, const std::vector<std::string>& owner_names)
{
    Conflict c;
    c.atom = ToString(rec.atom);
    for (const int o : rec.owners)
        c.owners.push_back(ToString(owner_names[std::size_t(o)]));
    c.winner = ToString(owner_names[std::size_t(rec.winner)]);
    c.resolved_by = ToString(rec.resolved_by);
    for (const int o : rec.lost)
        c.lost.push_back(ToString(owner_names[std::size_t(o)]));
    c.base_line = rec.base_line;
    c.text = ToString(rec.text);
    return c;
}

// S9 (15 section 2.4): side' = merge3(ancestor, base, side); the side wins the atoms both changed
String Rebase(Kind kind, std::string_view name, std::string_view ancestor, std::string_view base,
    std::string_view side, const std::string& owner, Vector<Conflict>& conflicts)
{
    std::vector<Doc> docs;
    std::string error;
    ParseAll(kind, name, {ancestor, base, side}, docs, error);
    std::vector<atoms::Version> versions(2);
    versions[0].text = base;
    versions[0].doc = &docs[1];
    versions[1].text = side;
    versions[1].doc = &docs[2];
    const std::vector<std::string> owner_names = {"ancestor", kBaseOwner, owner};
    Options options;
    const atoms::Outcome outcome = atoms::MergeDocs(ancestor, docs[0], versions, {}, options, owner_names);
    for (const atoms::ConflictRec& rec : outcome.conflicts)
    {
        Conflict c = ConflictOf(rec, owner_names);
        c.resolved_by = "rebase";
        c.reason = "both_changed";
        conflicts.push_back(std::move(c));
    }
    return ToString(outcome.Text());
}
} // namespace

Kind KindOfName(std::string_view name)
{
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos)
        return Kind::Unknown;
    const String ext = text::ToLower(name.substr(dot + 1));
    if (ext == "ltx")
        return Kind::Ltx;
    if (ext == "xml")
        return Kind::Xml;
    if (ext == "script" || ext == "lua")
        return Kind::Lua;
    return Kind::Unknown;
}

bool IsPatchFile(std::string_view vpath, const FileExists& exists)
{
    if (!exists.exists)
        return false;
    String name = text::ToLower(vpath);
    std::replace(name.begin(), name.end(), '/', '\\');
    const std::string_view view(name.data(), name.size());
    constexpr std::string_view configs = "configs\\";
    if (view.size() <= configs.size() || view.compare(0, configs.size(), configs) != 0)
        return false;
    const std::size_t slash = view.rfind('\\');
    const std::string_view folder = view.substr(0, slash + 1);
    const std::string_view leaf = view.substr(slash + 1);
    const std::size_t dot = leaf.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= leaf.size())
        return false;
    constexpr std::string_view mod = "mod_";
    if (leaf.size() <= mod.size() || leaf.compare(0, mod.size(), mod) != 0)
        return false;
    const std::string_view ext = leaf.substr(dot); // ".ltx"
    const std::string_view rest = leaf.substr(mod.size(), dot > mod.size() ? dot - mod.size() : 0);
    // mod_<stem>_<x>.<ext>: the stems from the longest one (as ContentPatchListFolder), a stem is never a patch name
    for (std::size_t pos = rest.rfind('_'); pos != std::string_view::npos && pos > 0; pos = rest.rfind('_', pos - 1))
    {
        if (pos + 1 >= rest.size())
            continue; // "mod_x_.ltx": nothing after the stem
        const std::string_view stem = rest.substr(0, pos);
        if (stem.size() >= mod.size() && stem.compare(0, mod.size(), mod) == 0)
            continue;
        String target(folder.data(), folder.size());
        target.append(stem.data(), stem.size());
        target.append(ext.data(), ext.size());
        if (exists.exists(exists.context, std::string_view(target.data(), target.size())))
            return true;
    }
    return false;
}

const char* KindName(Kind kind)
{
    switch (kind)
    {
    case Kind::Ltx: return "ltx";
    case Kind::Xml: return "xml";
    case Kind::Lua: return "lua";
    case Kind::Unknown: break;
    }
    return "unknown";
}

Kind KindOfWord(std::string_view word)
{
    const String w = text::ToLower(word);
    if (w == "ltx")
        return Kind::Ltx;
    if (w == "xml")
        return Kind::Xml;
    if (w == "lua" || w == "script")
        return Kind::Lua;
    return Kind::Unknown;
}

const char* StatusName(Status status)
{
    switch (status)
    {
    case Status::Unchanged: return "unchanged";
    case Status::Single: return "single";
    case Status::Merged: return "merged";
    case Status::MergedLines: return "merged_lines";
    case Status::Fallback: return "fallback";
    }
    return "unchanged";
}

const char* AtomOpName(AtomOp op)
{
    switch (op)
    {
    case AtomOp::Add: return "add";
    case AtomOp::Modify: return "modify";
    case AtomOp::Delete: return "delete";
    }
    return "modify";
}

namespace
{
// The last line of every input ends with LF for the merge: an atom a side adds after the last line of a file without
// the final line end must not stick to that line. all_lacked stays true while no text had the final LF.
void TerminateLastLine(String& text, bool& all_lacked)
{
    if (text.empty())
        return;
    if (text.back() == '\n')
        all_lacked = false;
    else
        text += '\n';
}

MergeResult MergeTerminated(const MergeInput& input, bool& all_lacked)
{
    MergeResult result;
    const Kind kind = input.kind;
    const std::string_view name = View(input.name);
    String base = NormalizeOf(kind, View(input.base));
    TerminateLastLine(base, all_lacked);

    // The sides: normalized, rebased onto the current base when made on another one
    std::vector<String> texts;
    texts.reserve(input.sides.size());
    for (const Side& side : input.sides)
    {
        String text = NormalizeOf(kind, View(side.text));
        if (side.has_delta)
        {
            // S9: the delta of the catalog carried over onto the current base (15 section 2.4, step 1)
            RebaseResult rebased = ApplyDelta(View(input.base), side.delta, View(side.owner));
            if (rebased.ok)
                text = NormalizeOf(kind, View(rebased.text));
            else
            {
                Conflict failed;
                failed.atom = "@file";
                failed.owners.push_back(side.owner);
                failed.winner = side.owner;
                failed.resolved_by = "rebase";
                failed.reason = "rebase_failed";
                failed.text = rebased.error;
                rebased.conflicts.push_back(std::move(failed));
            }
            for (Conflict& c : rebased.conflicts)
                result.rebase_conflicts.push_back(std::move(c));
        }
        TerminateLastLine(text, all_lacked);
        if (side.has_ancestor && !side.has_delta)
        {
            String ancestor = NormalizeOf(kind, View(side.ancestor));
            bool ancestor_lacked = true;
            TerminateLastLine(ancestor, ancestor_lacked);
            if (ancestor != base)
            {
                text = Rebase(kind, name, View(ancestor), View(base), View(text),
                    std::string(side.owner.data(), side.owner.size()), result.rebase_conflicts);
            }
        }
        texts.push_back(std::move(text));
    }
    result.unresolved += std::uint32_t(result.rebase_conflicts.size());

    std::vector<std::size_t> changed;
    for (std::size_t i = 0; i < texts.size(); ++i)
    {
        if (texts[i] != base)
        {
            changed.push_back(i);
            result.sides_changed.push_back(input.sides[i].owner);
        }
    }
    std::vector<std::string> owner_names = {kBaseOwner};
    for (const std::size_t i : changed)
        owner_names.emplace_back(input.sides[i].owner.data(), input.sides[i].owner.size());

    if (changed.empty())
    {
        result.status = Status::Unchanged;
        result.text = base;
        LineMap({{std::string(View(base)), 0}}, owner_names, result.lines);
        return result;
    }
    if (changed.size() == 1 && input.sides[changed[0]].role == Role::Addon)
    {
        result.status = Status::Single;
        result.text = texts[changed[0]];
        LineMap({{std::string(View(result.text)), 1}}, owner_names, result.lines);
        return result;
    }

    std::vector<std::string_view> views = {View(base)};
    for (const std::size_t i : changed)
        views.push_back(View(texts[i]));
    std::vector<Doc> docs;
    std::string parse_error;
    const bool parsed = ParseAll(kind, name, views, docs, parse_error);

    std::vector<atoms::Version> versions;
    for (std::size_t k = 0; k < changed.size(); ++k)
    {
        const Side& side = input.sides[changed[k]];
        atoms::Version v;
        v.text = views[k + 1];
        v.doc = &docs[k + 1];
        v.fragment = side.role == Role::Patch;
        for (const String& p : side.patches)
            v.patches.emplace_back(p.data(), p.size());
        versions.push_back(std::move(v));
    }
    std::map<std::string, int> rules;
    for (const AtomRule& rule : input.rules)
    {
        int owner = 0; // kBaseOwner, or an owner that did not change the file: its atoms are the base ones
        for (std::size_t k = 1; k < owner_names.size(); ++k)
        {
            if (owner_names[k] == View(rule.owner))
                owner = int(k);
        }
        rules[std::string(View(rule.atom))] = owner;
    }

    const atoms::Outcome outcome = atoms::MergeDocs(views[0], docs[0], versions, rules, input.options, owner_names);
    result.status = parsed ? Status::Merged : Status::MergedLines;
    if (!parsed)
        result.error = ToString(parse_error);
    result.text = ToString(outcome.Text());
    result.atoms_merged = outcome.atoms_merged;
    result.adjacent = outcome.adjacent;
    for (const std::string& a : outcome.adjacent_atoms)
        result.adjacent_atoms.push_back(ToString(a));
    for (const atoms::ConflictRec& rec : outcome.conflicts)
        result.conflicts.push_back(ConflictOf(rec, owner_names));
    for (const std::string& r : outcome.reordered)
        result.reordered.push_back(ToString(r));
    for (const std::string& r : outcome.rules_applied)
        result.rules_applied.push_back(ToString(r));
    result.unresolved += outcome.unresolved;
    LineMap(outcome.pieces, owner_names, result.lines);

    if (input.options.validate && !input.options.markers)
    {
        String error;
        const LuaChecker* lua = input.options.lua.compile ? &input.options.lua : nullptr;
        if (!Validate(kind, name, View(result.text), error, lua))
        {
            // The file of the upper side is served instead (the order), unless it does not pass either. A patch
            // fragment is not a whole file: the upper addon
            std::size_t top_index = texts.size() - 1;
            for (std::size_t i = texts.size(); i-- > 0;)
            {
                if (input.sides[i].role == Role::Addon)
                {
                    top_index = i;
                    break;
                }
            }
            const String& top = texts[top_index];
            String top_error;
            if (Validate(kind, name, View(top), top_error, lua))
            {
                result.status = Status::Fallback;
                result.error = error;
                result.text = top;
                result.lines.clear();
                std::vector<std::string> names = {kBaseOwner,
                    std::string(input.sides[top_index].owner.data(), input.sides[top_index].owner.size())};
                LineMap({{std::string(View(top)), 1}}, names, result.lines);
            }
            else
                result.error = "the merged text does not pass the check, the file of the upper side neither: " + error;
        }
    }
    return result;
}

} // namespace

MergeResult Merge(const MergeInput& input)
{
    bool all_lacked = true;
    MergeResult result = MergeTerminated(input, all_lacked);
    if (all_lacked && !result.text.empty() && result.text.back() == '\n')
        result.text.pop_back();
    return result;
}

Vector<AtomChange> ChangedAtoms(Kind kind, std::string_view name, std::string_view ancestor, std::string_view text_in)
{
    const String anc = NormalizeOf(kind, ancestor);
    const String text = NormalizeOf(kind, text_in);
    std::vector<Doc> docs;
    std::string error;
    ParseAll(kind, name, {View(anc), View(text)}, docs, error);
    std::vector<atoms::ChangeRec> recs;
    atoms::ChangedAtoms(View(anc), docs[0], View(text), docs[1], recs);
    Vector<AtomChange> out;
    for (const atoms::ChangeRec& rec : recs)
        out.push_back({ToString(rec.atom), rec.op, rec.line, rec.container});
    return out;
}

bool Validate(Kind kind, std::string_view name, std::string_view text_in, String& error, const LuaChecker* lua)
{
    const String text = Normalize(text_in);
    std::string message;
    bool ok = true;
    switch (kind)
    {
    case Kind::Ltx: ok = ValidateLtx(View(text), message); break;
    case Kind::Xml: ok = ValidateXml(View(text), message); break;
    case Kind::Lua:
        ok = ValidateLua(View(text), message);
        if (ok && lua && lua->compile)
        {
            String lua_error;
            const String chunk = "@" + String(name.data(), name.size());
            if (!lua->compile(lua->context, text.data(), text.size(), chunk.c_str(), lua_error))
            {
                ok = false;
                message.assign(lua_error.data(), lua_error.size());
            }
        }
        break;
    case Kind::Unknown: break;
    }
    if (!ok)
        error = ToString(message);
    return ok;
}
} // namespace xray::content_merge
