// The plan of the merge of the addons (Plan.h): the decisions the engine (addon_merge.cpp, addon_content.cpp) and the
// tool gw_merge (command plan) share. The code moved here from addon_merge.cpp / addon_content.cpp as it was: a change
// of a result here changes the game and the forecasts of the launcher and CI at once.

#include "Plan.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <system_error>
#include <utility>

namespace xray::content_merge
{
namespace
{
namespace fs = std::filesystem;

#if defined(_WIN32)
constexpr char kPlanSeparator = '\\';
#else
constexpr char kPlanSeparator = '/';
#endif

String PlanStr(std::string_view text) { return String(text.data(), text.size()); }

String PlanNumber(std::uint64_t value)
{
    char buffer[24];
    char* end = buffer + sizeof buffer;
    char* p = end;
    do
    {
        *--p = char('0' + value % 10);
        value /= 10;
    } while (value);
    return String(p, std::size_t(end - p));
}

// ---- paths in UTF-8 (as addon_host.cpp PathFromUtf8 / PathToUtf8 of the engine) -----------------------------------

#if defined(_WIN32)
// Strict UTF-8 -> UTF-16 (MultiByteToWideChar with MB_ERR_INVALID_CHARS): false - not valid UTF-8
bool PlanUtf8ToWide(std::string_view text, std::wstring& out)
{
    out.clear();
    const auto* s = reinterpret_cast<const unsigned char*>(text.data());
    const std::size_t size = text.size();
    for (std::size_t i = 0; i < size;)
    {
        const unsigned char c = s[i];
        std::uint32_t cp;
        std::size_t length;
        unsigned char low = 0x80, high = 0xBF;
        if (c < 0x80)
        {
            out += wchar_t(c);
            ++i;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF)
        {
            length = 2;
            cp = c & 0x1F;
        }
        else if (c >= 0xE0 && c <= 0xEF)
        {
            length = 3;
            cp = c & 0x0F;
            low = c == 0xE0 ? 0xA0 : 0x80;
            high = c == 0xED ? 0x9F : 0xBF;
        }
        else if (c >= 0xF0 && c <= 0xF4)
        {
            length = 4;
            cp = c & 0x07;
            low = c == 0xF0 ? 0x90 : 0x80;
            high = c == 0xF4 ? 0x8F : 0xBF;
        }
        else
            return false;
        if (size - i < length || s[i + 1] < low || s[i + 1] > high)
            return false;
        for (std::size_t k = 1; k < length; ++k)
        {
            if (k > 1 && (s[i + k] < 0x80 || s[i + k] > 0xBF))
                return false;
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if (cp >= 0x10000)
        {
            cp -= 0x10000;
            out += wchar_t(0xD800 + (cp >> 10));
            out += wchar_t(0xDC00 + (cp & 0x3FF));
        }
        else
            out += wchar_t(cp);
        i += length;
    }
    return true;
}
#endif

// UTF-8 text -> path: '\' is the separator of the OS on POSIX; not valid UTF-8 - the ANSI code page on Windows
fs::path PlanPathOf(std::string_view text)
{
#if defined(_WIN32)
    std::wstring wide;
    if (text.empty() || !PlanUtf8ToWide(text, wide))
        return fs::path(std::string(text.data(), text.size()));
    return fs::path(wide);
#else
    std::string native(text.data(), text.size());
    std::replace(native.begin(), native.end(), '\\', '/');
    return fs::path(native);
#endif
}

String PlanUtf8Of(const fs::path& path)
{
    const auto u8 = path.u8string(); // std::u8string in C++20, std::string in C++17
    return String(reinterpret_cast<const char*>(u8.c_str()), u8.size());
}

bool PlanReadBytes(const fs::path& path, String& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0)
        return false;
    in.seekg(0, std::ios::beg);
    out.resize(std::size_t(size));
    if (size > 0)
        in.read(&out[0], size);
    return bool(in);
}

bool PlanSkips(const WalkRules& walk, const String& physical)
{
    return walk.skips && walk.skips(walk.context, physical.c_str());
}

template <typename T>
bool PlanContains(const Vector<T>& list, const T& value)
{
    return std::find(list.begin(), list.end(), value) != list.end();
}

void PlanAddOnce(Vector<String>& list, const String& value)
{
    if (!PlanContains(list, value))
        list.push_back(value);
}

String PlanTrim(const String& text)
{
    const std::size_t begin = text.find_first_not_of(" \t\r");
    if (begin == String::npos)
        return String();
    return text.substr(begin, text.find_last_not_of(" \t\r") - begin + 1);
}

Vector<String> PlanSplitLines(const String& text)
{
    Vector<String> out;
    std::size_t start = 0;
    while (start <= text.size())
    {
        std::size_t end = text.find('\n', start);
        if (end == String::npos)
            end = text.size();
        out.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

String PlanWithoutBom(std::string_view text)
{
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3);
    return PlanStr(text);
}

// ---- the release tag (addon_merge.cpp MergeParseTag, MergeCompareTags) ----------------------------------------------

struct PlanTag
{
    Vector<std::uint32_t> numbers;
    std::uint32_t rank = 1; // 0 - a pre-release, 1 - the release, 2 - a patch
    std::uint32_t patch = 0;
    String suffix;
};

bool PlanTagNumber(std::string_view text, std::size_t& i, std::uint32_t& out)
{
    std::uint64_t number = 0;
    const std::size_t begin = i;
    for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i)
    {
        number = number * 10 + std::uint64_t(text[i] - '0');
        if (number > std::numeric_limits<std::uint32_t>::max())
            return false;
    }
    out = std::uint32_t(number);
    return i > begin;
}

bool PlanParseTag(std::string_view tag, PlanTag& out)
{
    out = PlanTag();
    std::size_t i = !tag.empty() && (tag[0] == 'v' || tag[0] == 'V') ? 1 : 0;
    for (;;)
    {
        std::uint32_t number = 0;
        if (!PlanTagNumber(tag, i, number))
            return false;
        out.numbers.push_back(number);
        if (i == tag.size() || tag[i] != '.')
            break;
        ++i;
    }
    if (i == tag.size())
        return true;
    if (tag[i] != '-' || i + 1 == tag.size())
        return false;
    const std::string_view suffix = tag.substr(i + 1);
    const bool valid = std::all_of(suffix.begin(), suffix.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.';
    });
    if (!valid)
        return false;
    const String lower = AsciiLower(suffix);
    constexpr std::string_view patch_word = "patch";
    if (lower.size() > patch_word.size() && std::string_view(lower).substr(0, patch_word.size()) == patch_word &&
        std::all_of(lower.begin() + std::ptrdiff_t(patch_word.size()), lower.end(),
            [](char c) { return c >= '0' && c <= '9'; }))
    {
        std::size_t at = patch_word.size();
        if (!PlanTagNumber(lower, at, out.patch))
            return false;
        out.rank = 2;
        return true;
    }
    out.rank = 0;
    out.suffix = lower;
    return true;
}

int PlanCompareTags(const PlanTag& a, const PlanTag& b)
{
    for (std::size_t i = 0; i < (std::max)(a.numbers.size(), b.numbers.size()); ++i)
    {
        const std::uint32_t x = i < a.numbers.size() ? a.numbers[i] : 0;
        const std::uint32_t y = i < b.numbers.size() ? b.numbers[i] : 0;
        if (x != y)
            return x < y ? -1 : 1;
    }
    if (a.rank != b.rank)
        return a.rank < b.rank ? -1 : 1;
    if (a.rank == 2 && a.patch != b.patch)
        return a.patch < b.patch ? -1 : 1;
    if (a.rank == 0 && a.suffix != b.suffix)
        return a.suffix < b.suffix ? -1 : 1;
    return 0;
}

// ---- <patch>\merge\targets.ltx (addon_merge.cpp MergeReadTargets) ----------------------------------------------------
// file -> addon -> sha256, all in ASCII lower case with '\'
using PlanTargets = std::map<String, std::map<String, String>>;

PlanTargets PlanParseTargets(std::string_view bytes)
{
    PlanTargets out;
    String section;
    for (String line : PlanSplitLines(PlanWithoutBom(bytes)))
    {
        const std::size_t comment = line.find(';');
        if (comment != String::npos)
            line.erase(comment);
        line = PlanTrim(line);
        if (line.empty())
            continue;
        if (line.front() == '[')
        {
            const std::size_t close = line.find(']');
            section = close == String::npos ? String() : AsciiLower(IndexName(PlanTrim(line.substr(1, close - 1)), false));
            continue;
        }
        const std::size_t eq = line.find('=');
        if (section.empty() || eq == String::npos)
            continue;
        out[section][AsciiLower(PlanTrim(line.substr(0, eq)))] = AsciiLower(PlanTrim(line.substr(eq + 1)));
    }
    return out;
}

// ---- the merge of one file (addon_merge.cpp MergeOne and its helpers) ---------------------------------------------

constexpr std::string_view kPlanGapPrefix = "@gap "; // a changed gap of a delta: "@gap <the label the merge gives it>"

const AtomRule* PlanRuleOf(const Vector<AtomRule>& rules, std::string_view label)
{
    for (const AtomRule& rule : rules)
    {
        if (SameAtom(rule.atom, label))
            return &rule;
    }
    return nullptr;
}

PlanFile::RebaseConflict PlanRebaseConflictOf(const Conflict& rec)
{
    PlanFile::RebaseConflict c;
    c.atom = rec.atom;
    c.addon = rec.owners.empty() ? String() : rec.owners.front();
    c.reason = rec.reason;
    c.note = rec.text;
    c.winner = rec.winner.empty() ? c.addon : rec.winner;
    return c;
}

// The delta of a side under the atom: rules: an atom a rule = core names is taken out, an atom a rule gives to this side
// stays. `dropped` - the labels taken out; `applied` - the atoms of the rules that decided an atom
DeltaFile PlanDeltaByRules(const DeltaFile& delta, const String& owner, const Vector<AtomRule>& rules, const String& kind,
    Vector<String>& dropped, Vector<String>& applied)
{
    DeltaFile kept = delta;
    kept.atoms.clear();
    for (const DeltaAtom& atom : delta.atoms)
    {
        const AtomRule* rule = PlanRuleOf(rules, atom.atom);
        if (rule && rule->owner == kBaseOwner)
        {
            dropped.push_back(atom.atom);
            PlanAddOnce(applied, rule->atom);
            continue;
        }
        if (rule && rule->owner == owner)
            PlanAddOnce(applied, rule->atom);
        kept.atoms.push_back(atom);
    }
    if (kept.atoms.empty() && !delta.atoms.empty())
    {
        kept.same_as_ancestor = true;
        if (kept.kind == "lines")
            kept.kind = kind;
    }
    return kept;
}

struct PlanSideRebase
{
    bool ok = false;
    String text;
    Vector<PlanFile::RebaseConflict> conflicts;
};

PlanSideRebase PlanRebaseSide(const PlanFile& item, const String& base, const DeltaFile& delta, const String& owner,
    const Vector<AtomRule>& rules, Vector<String>& applied)
{
    PlanSideRebase out;
    Vector<String> dropped;
    Vector<String> decided;
    const DeltaFile kept = PlanDeltaByRules(delta, owner, rules, item.kind, dropped, decided);
    const RebaseResult rebased = ApplyDelta(base, kept, owner);
    if (!rebased.ok)
    {
        PlanFile::RebaseConflict& c = out.conflicts.emplace_back();
        c.atom = "@file";
        c.addon = owner;
        c.reason = "rebase_failed";
        c.note = rebased.error;
        c.winner = owner;
        return out;
    }
    out.ok = true;
    out.text = rebased.text;
    for (const Conflict& rec : rebased.conflicts)
    {
        PlanFile::RebaseConflict c = PlanRebaseConflictOf(rec);
        const AtomRule* rule = PlanRuleOf(rules, c.atom);
        if (rule && rule->owner == owner && c.winner == owner)
            c.resolved_by = "player";
        else if (rule && rule->owner == owner)
            decided.erase(std::remove(decided.begin(), decided.end(), rule->atom), decided.end());
        out.conflicts.push_back(std::move(c));
    }
    if (!dropped.empty())
    {
        const RebaseResult whole = ApplyDelta(base, delta, owner);
        for (const Conflict& rec : whole.conflicts)
        {
            if (!PlanContains(dropped, rec.atom))
                continue;
            PlanFile::RebaseConflict c = PlanRebaseConflictOf(rec);
            c.winner = kBaseOwner;
            c.resolved_by = "player";
            if (!c.note.empty())
            {
                const std::size_t put = c.note.find(": the atom is put ");
                c.note = (put == String::npos ? String() : c.note.substr(0, put) + ": ") + "the atom of " + owner +
                    " is not placed, the rule = core keeps the base";
            }
            out.conflicts.push_back(std::move(c));
        }
    }
    for (const String& atom : decided)
        PlanAddOnce(applied, atom);
    return out;
}

// Status single: the atom: rules = core applied to the version of the only addon that changes the base
void PlanRulesOnSingle(PlanFile& item, MergeResult& result, const String& base, const Vector<AtomRule>& rules,
    Vector<String>& log)
{
    if (result.status != Status::Single || rules.empty() || result.sides_changed.size() != 1)
        return;
    const String owner = result.sides_changed.front();
    const DeltaFile delta = BuildDelta(KindOfWord(item.kind), item.file, base, result.text);
    Vector<String> dropped;
    Vector<String> decided;
    const DeltaFile kept = PlanDeltaByRules(delta, owner, rules, item.kind, dropped, decided);
    if (!dropped.empty())
    {
        const RebaseResult r = ApplyDelta(base, kept, owner);
        if (!r.ok)
        {
            log.push_back("~ [merge] " + item.file + ": the atom: rule(s) = core cannot be applied to the version of " +
                owner + " (" + r.error + "): its version is served");
            return;
        }
        result.text = r.text;
        item.rules_changed = true;
    }
    for (const String& atom : decided)
        PlanAddOnce(item.rules_applied, atom);
}

void PlanFill(PlanFile& item, const MergeResult& result)
{
    item.status = StatusName(result.status);
    item.changed.assign(result.sides_changed.begin(), result.sides_changed.end());
    item.atoms_merged = result.atoms_merged;
    item.adjacent = result.adjacent;
    item.adjacent_atoms.assign(result.adjacent_atoms.begin(), result.adjacent_atoms.end());
    item.reordered.assign(result.reordered.begin(), result.reordered.end());
    item.rules_applied.assign(result.rules_applied.begin(), result.rules_applied.end());
    item.unresolved = result.unresolved;
    item.error = result.error;
    for (const Conflict& rec : result.conflicts)
    {
        PlanFile::Conflict& c = item.conflicts.emplace_back();
        c.atom = rec.atom;
        c.addons.assign(rec.owners.begin(), rec.owners.end());
        c.winner = rec.winner;
        c.resolved_by = rec.resolved_by;
        c.lost.assign(rec.lost.begin(), rec.lost.end());
        c.base_line = rec.base_line;
    }
    for (const Conflict& rec : result.rebase_conflicts)
        item.rebase_conflicts.push_back(PlanRebaseConflictOf(rec));
    for (const LineOwner& l : result.lines)
        item.lines.push_back({l.from_line, l.to_line, l.owner});
}

// ---- the rules of the stack (addon_content.cpp) ----------------------------------------------------------------------

using PlanBelow = std::map<String, Vector<String>>; // an owner -> the owners that must lie below it

bool PlanMustLieBelow(const PlanBelow& below, const String& from, const String& wanted)
{
    Vector<String> todo{from};
    Vector<String> seen;
    while (!todo.empty())
    {
        const String owner = todo.back();
        todo.pop_back();
        if (PlanContains(seen, owner))
            continue;
        seen.push_back(owner);
        const auto it = below.find(owner);
        if (it == below.end())
            continue;
        for (const String& lower : it->second)
        {
            if (lower == wanted)
                return true;
            todo.push_back(lower);
        }
    }
    return false;
}

const RuleStatus* PlanFindStatus(const Vector<RuleStatus>& statuses, std::string_view kind, const String& winner,
    const String& target)
{
    for (const RuleStatus& status : statuses)
    {
        if (status.kind == kind && status.winner == winner && status.target == target)
            return &status;
    }
    return nullptr;
}

// "addon", "slot", "level_pack" (addon_content.cpp ContentPartOf)
String PlanPartOf(const String& layer_id)
{
    const String part = layer_id.substr(0, layer_id.find(':'));
    return part.compare(0, 10, "level_pack") == 0 ? String("level_pack") : part;
}

bool PlanGives(const PlanLayer& layer, std::string_view name, const WalkRules& walk)
{
    return !layer.writable && !layer.owner.empty() && LayerGives(layer.folder, layer.private_masks, name, walk);
}
} // namespace

// ---- the rules of the file system --------------------------------------------------------------------------------------

bool IgnoredName(std::string_view name)
{
    if (name == "Thumbs.db" || name == ".svn" || name == ".vs")
        return true;
    // ENDS_WITH of ignore_name: len > sizeof(n) - longer than the suffix with its terminating zero
    const auto ends_with = [name](std::string_view suffix) {
        return name.size() > suffix.size() + 1 && name.substr(name.size() - suffix.size()) == suffix;
    };
    return ends_with("Thumbs.db") || ends_with(".VC.db") || ends_with(".VC.opendb") || ends_with(".sln") ||
        ends_with(".pdb") || ends_with(".ipdb") || ends_with(".iobj");
}

bool WalkSkipsEntry(std::string_view leaf, bool hidden)
{
    return leaf.empty() || leaf == "." || leaf == ".." || IgnoredName(leaf) || hidden;
}

bool PatternMatch(std::string_view name, std::string_view mask)
{
    // The algorithm of xrCore over zero-terminated strings: the end of a view reads as '\0'
    const auto at = [](std::string_view text, std::size_t i) { return i < text.size() ? text[i] : '\0'; };
    std::size_t s = 0, m = 0, cp = 0, mp = 0;
    for (; at(name, s) && at(mask, m) != '*'; m++, s++)
    {
        if (at(mask, m) != at(name, s) && at(mask, m) != '?')
            return false;
    }
    for (;;)
    {
        if (!at(name, s))
        {
            while (at(mask, m) == '*')
                m++;
            return !at(mask, m);
        }
        if (at(mask, m) == '*')
        {
            if (!at(mask, ++m))
                return true;
            mp = m;
            cp = s + 1;
            continue;
        }
        if (at(mask, m) == at(name, s) || at(mask, m) == '?')
        {
            m++, s++;
            continue;
        }
        m = mp;
        s = cp++;
    }
}

String AsciiLower(std::string_view text)
{
    String out = PlanStr(text);
    for (char& c : out)
    {
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    }
    return out;
}

String PrivateMaskOf(std::string_view mask, bool lower_case)
{
    String result;
    for (const char c : mask)
    {
        const char normalized = c == '/' ? '\\' : (lower_case && c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
        if (normalized == '\\' && result.empty())
            continue;
        result += normalized;
    }
    return result;
}

String IndexName(std::string_view name, bool lower_case)
{
    String out = PlanStr(name);
    for (char& c : out)
    {
        if (c == '/')
            c = '\\';
        else if (lower_case && c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    }
    return out;
}

namespace
{
// One folder of CLocatorAPI::layer_walk: `physical_dir` - its UTF-8 path with the trailing separator, `relative_dir`
// - its name under the layer folder ('\', the trailing one; empty - the layer folder)
void PlanWalkFolder(const String& physical_dir, const String& relative_dir, const WalkRules& walk, bool texts_only,
    Vector<String>& names, Vector<String>* too_long)
{
    std::error_code ec;
    fs::directory_iterator it(PlanPathOf(physical_dir), fs::directory_options::skip_permission_denied, ec);
    // An error of the listing ends this folder only: layer_walk of a subfolder that cannot be listed gives nothing
    for (const fs::directory_iterator end; !ec && it != end; it.increment(ec))
    {
        const String leaf = PlanUtf8Of(it->path().filename());
        std::error_code type_error;
        const bool folder = it->is_directory(type_error); // a link to a folder is walked, as _wfindfirst / stat do
        if (!folder && !it->is_regular_file(type_error))
            continue;
        // LayerTexts: a file of no text kind goes before the rule of the walk - the engine's one asks the OS of every
        // entry (GetFileAttributesW on Windows), and the textures and the models are most of the files of a layer
        if (texts_only && !folder && KindOfName(leaf) == Kind::Unknown)
            continue;
        const String physical = physical_dir + leaf;
        if (PlanSkips(walk, physical))
            continue; // hidden, ignore_name (layer_walk_skips)
        const std::size_t virtual_dir = walk.index_root ? walk.index_root + relative_dir.size() : 0;
        if (WalkNameTooLong(physical_dir.size(), virtual_dir, leaf.size(), folder))
        {
            if (too_long)
                too_long->push_back(physical); // "! FS: layer '<id>': name too long ..., skipped" of the engine
            continue;
        }
        if (folder)
        {
            if (!leaf.empty() && leaf[0] == '@')
                continue; // the alias roots of the addons
            PlanWalkFolder(physical + kPlanSeparator, relative_dir + leaf + '\\', walk, texts_only, names, too_long);
            continue;
        }
        names.push_back(IndexName(relative_dir + leaf, walk.lower_case));
    }
}

Vector<String> PlanLayerWalk(std::string_view folder, const WalkRules& walk, bool texts_only, Vector<String>* too_long)
{
    Vector<String> names;
    String root = PlanStr(folder);
    if (!root.empty() && root.back() != '\\' && root.back() != '/')
        root += kPlanSeparator;
    PlanWalkFolder(root, String(), walk, texts_only, names, too_long);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}
} // namespace

Vector<String> LayerFiles(std::string_view folder, const WalkRules& walk, Vector<String>* too_long)
{
    return PlanLayerWalk(folder, walk, false, too_long);
}

Vector<String> LayerTexts(std::string_view folder, const WalkRules& walk, Vector<String>* too_long)
{
    return PlanLayerWalk(folder, walk, true, too_long);
}

bool LayerGives(std::string_view folder, const Vector<String>& private_masks, std::string_view name,
    const WalkRules& walk)
{
    for (const String& mask : private_masks)
    {
        const String normalized = PrivateMaskOf(mask, walk.lower_case);
        if (!normalized.empty() && PatternMatch(name, normalized))
            return false;
    }
    std::error_code error;
    if (!fs::is_regular_file(PlanPathOf(PlanStr(folder) + PlanStr(name)), error))
        return false;
    String physical = PlanStr(folder);
    std::size_t start = 0;
    for (;;)
    {
        const std::size_t slash = name.find('\\', start);
        const std::string_view part = name.substr(start, slash == std::string_view::npos ? slash : slash - start);
        const std::size_t virtual_dir = walk.index_root ? walk.index_root + start : 0;
        if (WalkNameTooLong(physical.size(), virtual_dir, part.size(), slash != std::string_view::npos))
            return false; // layer_walk does not mount it
        physical += PlanStr(part);
        const bool alias_root = slash != std::string_view::npos && !part.empty() && part[0] == '@';
        if (alias_root || PlanSkips(walk, physical))
            return false;
        if (slash == std::string_view::npos)
            return true;
        physical += kPlanSeparator;
        start = slash + 1;
    }
}

bool LayerServes(const PlanLayer& layer, std::string_view name, const WalkRules& walk)
{
    return std::none_of(layer.yielded.begin(), layer.yielded.end(),
               [name](const String& file) { return std::string_view(file) == name; }) &&
        PlanGives(layer, name, walk);
}

// ---- the rules of the stack ------------------------------------------------------------------------------------------

String LayerRun(const PlanLayer& layer)
{
    if (layer.writable || layer.owner.empty() || !layer.group.empty())
        return String();
    const std::size_t colon = layer.layer_id.find(':');
    return colon == String::npos ? String() : layer.layer_id.substr(0, colon);
}

void ApplyPairRules(Vector<PlanLayer>& stack, const Vector<PairRule>& rules, Vector<RuleStatus>& statuses)
{
    struct Run
    {
        std::size_t begin, end;
        PlanBelow below;
    };
    Vector<Run> runs;
    for (std::size_t i = 0; i < stack.size();)
    {
        const String run = LayerRun(stack[i]);
        std::size_t end = i + 1;
        while (!run.empty() && end < stack.size() && LayerRun(stack[end]) == run)
            ++end;
        if (!run.empty())
            runs.push_back(Run{i, end, {}});
        i = end;
    }
    const auto first_of = [&stack](std::size_t begin, std::size_t end, const String& owner) {
        for (std::size_t i = begin; i < end; ++i)
        {
            if (stack[i].owner == owner)
                return i;
        }
        return std::size_t(-1);
    };

    for (const PairRule& rule : rules)
    {
        RuleStatus& status = statuses.emplace_back();
        status.kind = "pair";
        status.winner = rule.winner;
        status.target = rule.loser;
        bool together = false, cycle = false, moved = false;
        for (Run& run : runs)
        {
            const std::size_t winner = first_of(run.begin, run.end, rule.winner);
            const std::size_t loser = first_of(run.begin, run.end, rule.loser);
            if (winner == std::size_t(-1) || loser == std::size_t(-1))
                continue;
            together = true;
            if (PlanMustLieBelow(run.below, rule.loser, rule.winner))
            {
                cycle = true;
                continue;
            }
            Vector<String>& lower = run.below[rule.winner];
            PlanAddOnce(lower, rule.loser);
            moved |= winner < loser;
        }
        status.applied = together && !cycle;
        const bool has_winner = first_of(0, stack.size(), rule.winner) != std::size_t(-1);
        const bool has_loser = first_of(0, stack.size(), rule.loser) != std::size_t(-1);
        if (!has_winner || !has_loser)
            status.note = (has_winner ? rule.loser : rule.winner) + " has no content mounted";
        else if (cycle)
            status.note = "contradicts an earlier pair rule of [conflicts], not taken";
        else if (!together)
            status.note = "not in one part of the stack: the set of a chosen slot provider lies over the common "
                          "content of any addon, the location pack over all";
        else
            status.note = moved ? "the winner goes over the loser" : "the order of the addons has it already";
    }

    for (const Run& run : runs)
    {
        if (run.below.empty())
            continue;
        Vector<PlanLayer> pending(stack.begin() + std::ptrdiff_t(run.begin), stack.begin() + std::ptrdiff_t(run.end));
        Vector<PlanLayer> placed;
        placed.reserve(pending.size());
        while (!pending.empty())
        {
            std::size_t pick = 0;
            for (; pick < pending.size(); ++pick)
            {
                const auto it = run.below.find(pending[pick].owner);
                const bool ready = it == run.below.end() ||
                    std::none_of(it->second.begin(), it->second.end(), [&pending](const String& lower) {
                        return std::any_of(pending.begin(), pending.end(),
                            [&lower](const PlanLayer& layer) { return layer.owner == lower; });
                    });
                if (ready)
                    break;
            }
            if (pick == pending.size())
                pick = 0;
            placed.push_back(std::move(pending[pick]));
            pending.erase(pending.begin() + std::ptrdiff_t(pick));
        }
        std::move(placed.begin(), placed.end(), stack.begin() + std::ptrdiff_t(run.begin));
    }
}

void ApplyFileRules(Vector<PlanLayer>& stack, const Vector<FileRule>& rules, const WalkRules& walk,
    Vector<RuleStatus>& statuses)
{
    for (const FileRule& rule : rules)
    {
        RuleStatus& status = statuses.emplace_back();
        status.kind = "file";
        status.winner = rule.winner;
        status.target = rule.file;
        std::size_t from = 0;
        if (rule.winner != kBaseOwner)
        {
            bool mounted = false;
            std::size_t found = std::size_t(-1);
            for (std::size_t i = stack.size(); i-- > 0 && found == std::size_t(-1);)
            {
                if (stack[i].owner != rule.winner)
                    continue;
                mounted = true;
                if (PlanGives(stack[i], rule.file, walk))
                    found = i;
            }
            if (found == std::size_t(-1))
            {
                status.note = mounted ? rule.winner + " has no such file (or keeps it [private])" :
                                        rule.winner + " has no content mounted";
                continue;
            }
            from = found + 1;
        }
        std::uint32_t yielded = 0;
        for (std::size_t i = from; i < stack.size(); ++i)
        {
            PlanLayer& layer = stack[i];
            if (layer.owner == rule.winner || !PlanGives(layer, rule.file, walk))
                continue;
            layer.yielded.push_back(rule.file);
            ++yielded;
        }
        status.applied = true;
        if (yielded)
            status.note = PlanNumber(yielded) + " layer(s) over it give the file up";
        else
            status.note = rule.winner == kBaseOwner ? "no addon gives the file" : "no layer over it gives the file";
    }
}

void ResolveFileConflict(std::string_view name, const Vector<String>& layers, const Vector<String>& owners,
    std::string_view winner, const Vector<FileRule>& file_rules, const Vector<PairRule>& pair_rules,
    const Vector<RuleStatus>& statuses, const Vector<PlanAddon>& addons, String& resolved_by, String& note)
{
    const auto winner_at = std::find_if(layers.begin(), layers.end(),
        [winner](const String& layer) { return std::string_view(layer) == winner; });
    const String* winner_owner =
        winner_at == layers.end() ? nullptr : &owners[std::size_t(winner_at - layers.begin())];
    const auto among = [&owners](const String& owner) { return PlanContains(owners, owner); };
    const auto rule = std::find_if(file_rules.begin(), file_rules.end(),
        [name](const FileRule& item) { return std::string_view(item.file) == name; });
    if (rule != file_rules.end())
    {
        const bool holds = rule->winner == kBaseOwner ? !winner_owner : winner_owner && rule->winner == *winner_owner;
        if (holds)
        {
            resolved_by = "player";
            return;
        }
        const RuleStatus* status = PlanFindStatus(statuses, "file", rule->winner, rule->file);
        note = "file:" + rule->file + " = " + rule->winner + " does not hold: " +
            (status && !status->note.empty() ? status->note : String("?"));
    }
    if (winner_owner)
    {
        for (const PairRule& pair : pair_rules)
        {
            if (pair.loser != *winner_owner || !among(pair.winner))
                continue;
            const RuleStatus* status = PlanFindStatus(statuses, "pair", pair.winner, pair.loser);
            note += (note.empty() ? "" : "; ") + String("pair:") + pair.winner + " = " + pair.loser +
                " does not hold here: " + (status && !status->note.empty() ? status->note : String("?"));
        }
    }
    String runner_up;
    for (auto it = winner_at; winner_owner && winner_at != layers.end() && it != layers.begin();)
    {
        --it;
        const std::size_t at = std::size_t(it - layers.begin());
        if (owners[at] != *winner_owner)
        {
            runner_up = *it;
            break;
        }
    }
    if (!runner_up.empty())
    {
        const String part = PlanPartOf(*winner_at);
        if (part != PlanPartOf(runner_up))
        {
            resolved_by = part == "level_pack" ? "pack" : part == "slot" ? "slot" : "order";
            return;
        }
    }
    if (winner_owner)
    {
        for (const PairRule& pair : pair_rules)
        {
            const RuleStatus* status = PlanFindStatus(statuses, "pair", pair.winner, pair.loser);
            if (pair.winner == *winner_owner && among(pair.loser) && status && status->applied)
            {
                resolved_by = "pair";
                return;
            }
        }
        const auto addon = std::find_if(addons.begin(), addons.end(),
            [winner_owner](const PlanAddon& info) { return info.id == *winner_owner; });
        if (addon != addons.end() && std::any_of(addon->load_after.begin(), addon->load_after.end(), among))
        {
            resolved_by = "load_after";
            return;
        }
    }
    resolved_by = "order";
}

// ---- the release tag ---------------------------------------------------------------------------------------------------

bool ReleaseTagOlder(std::string_view game_version, std::string_view tag)
{
    if (game_version.empty() || tag.empty() || game_version == tag)
        return false;
    PlanTag a, b;
    if (!PlanParseTag(game_version, a) || !PlanParseTag(tag, b))
        return true; // not versions: any difference
    return PlanCompareTags(a, b) < 0;
}

String ReleaseTagOf(std::string_view ltx)
{
    String tag;
    String section;
    for (String line : PlanSplitLines(PlanWithoutBom(ltx)))
    {
        const std::size_t comment = line.find(';');
        if (comment != String::npos)
            line.erase(comment);
        line = PlanTrim(line);
        if (line.empty())
            continue;
        if (line.front() == '[')
        {
            const std::size_t close = line.find(']');
            section = close == String::npos ? String() : AsciiLower(PlanTrim(line.substr(1, close - 1)));
            continue;
        }
        const std::size_t eq = line.find('=');
        if (section == "release" && eq != String::npos && AsciiLower(PlanTrim(line.substr(0, eq))) == "tag")
            tag = PlanTrim(line.substr(eq + 1));
    }
    return tag;
}

// ---- the delta of an addon -------------------------------------------------------------------------------------------

void ReadAddonDelta(std::string_view file, std::string_view bytes, AddonDelta& out)
{
    out = AddonDelta();
    out.file = PlanStr(file);
    String error;
    if (!DeltaFromJson(bytes, out.delta, error))
    {
        out.error = "is not read (" + error + ")";
        return;
    }
    out.read = true;
    if (out.delta.algo != kAlgorithmVersion)
    {
        out.error = "is made by " + out.delta.algo + ", the engine parses by " + kAlgorithmVersion;
        return;
    }
    out.stale = false;
    for (const DeltaFile& entry : out.delta.files)
    {
        const String problem = DeltaFileProblem(entry);
        if (problem.empty())
            continue;
        const auto at = std::find(out.inconsistent.begin(), out.inconsistent.end(), entry.vpath);
        if (at != out.inconsistent.end())
            out.inconsistent_why[std::size_t(at - out.inconsistent.begin())] = problem; // the last one, as a map
        else
        {
            out.inconsistent.push_back(entry.vpath);
            out.inconsistent_why.push_back(problem);
        }
    }
}

const DeltaFile* DeltaEntryOf(const AddonDelta* delta, std::string_view name)
{
    if (!delta || delta->stale || PlanContains(delta->inconsistent, AsciiLower(name)))
        return nullptr;
    const DeltaFile* file = FindDeltaFile(delta->delta, name);
    return file && !file->not_in_ancestor ? file : nullptr;
}

bool DeltaStaleFor(const AddonDelta* delta, std::string_view name)
{
    return delta && !delta->file.empty() && (delta->stale || PlanContains(delta->inconsistent, AsciiLower(name)));
}

Vector<String> DeltaFileNames()
{
    // As the engine reads it (addon_merge.cpp MergeReadDelta): delta.json only - the launcher writes the recomputed
    // delta of the catalog there (00-roadmap.md 5a, decision 63: no delta.<algo>.json of the author)
    return {"delta.json"};
}

// ---- the plan ----------------------------------------------------------------------------------------------------------

Vector<String> PlanOwners(const Vector<PlanLayer>& layers)
{
    Vector<String> owners;
    for (const PlanLayer& layer : layers)
    {
        if (layer.writable || layer.merged || layer.owner.empty() || !layer.group.empty())
            continue;
        PlanAddOnce(owners, layer.owner);
    }
    return owners;
}

namespace
{
// PlanInput::all_files: a record (PlanEntry::extra) of every text file a layer of an addon serves that the plan has
// no record of - the side, and the note a record of the merge would have (the same order of the checks as PlanMerge),
// else one_addon. `names` - the text files of the layers of the addons (PlanMerge, step 1). The location pack: its
// files no layer of an addon gives - the side is the layer of the pack. Keeps out.entries by the file name
void PlanAllFiles(const PlanInput& input, const PlanEnv& env, const std::map<String, Vector<std::size_t>>& names,
    PlanResult& out)
{
    const WalkRules& walk = env.walk;
    const Vector<PlanLayer>& layers = input.layers;
    std::map<String, bool> all; // name -> a record of the plan has it
    for (const auto& item : names)
        all.emplace(item.first, false);
    Vector<std::size_t> packs; // the layers of the location pack (not its writes)
    for (std::size_t i = 0; i < layers.size(); ++i)
    {
        const PlanLayer& layer = layers[i];
        if (layer.group.empty() || layer.writable || layer.merged || layer.owner.empty())
            continue;
        packs.push_back(i);
        for (const String& name : LayerTexts(layer.folder, walk))
            all.emplace(name, false);
    }
    for (const PlanEntry& entry : out.entries)
        all[entry.item.file] = true;

    Vector<PlanEntry> extras;
    const Vector<std::size_t> no_layers;
    for (const auto& [name, listed] : all)
    {
        if (listed)
            continue;
        const auto found = names.find(name);
        const Vector<std::size_t>& file_layers = found == names.end() ? no_layers : found->second;
        const auto rule = std::find_if(input.file_rules.begin(), input.file_rules.end(),
            [&name = name](const FileRule& file_rule) { return file_rule.file == name; });
        // The topmost layer of each addon that gives the name, as PlanMerge takes the sides
        Vector<std::pair<String, std::size_t>> sides;
        for (const std::size_t index : file_layers)
        {
            const PlanLayer& layer = layers[index];
            if (rule == input.file_rules.end() && !LayerServes(layer, name, walk))
                continue;
            const auto it = std::find_if(sides.begin(), sides.end(),
                [&layer](const std::pair<String, std::size_t>& side) { return side.first == layer.owner; });
            if (it == sides.end())
                sides.emplace_back(layer.owner, index);
            else
                it->second = index;
        }
        for (std::size_t k = packs.size(); sides.empty() && k-- > 0;)
        {
            if (LayerServes(layers[packs[k]], name, walk))
                sides.emplace_back(layers[packs[k]].owner, packs[k]);
        }
        if (sides.empty())
            continue; // [private], given up by a file: rule, under "@...": not at the game data
        std::sort(sides.begin(), sides.end(),
            [](const std::pair<String, std::size_t>& a, const std::pair<String, std::size_t>& b) {
                return a.second < b.second;
            });

        PlanEntry& entry = extras.emplace_back();
        entry.extra = true;
        PlanFile& item = entry.item;
        item.file = name;
        item.kind = KindName(KindOfName(name));
        item.status = "skipped";
        for (const auto& [owner, index] : sides)
        {
            item.addons.push_back(owner);
            item.layers.push_back(layers[index].layer_id);
            entry.job.side_layers.push_back(index);
        }
        const bool pack = std::any_of(packs.begin(), packs.end(),
            [&, &name = name](const std::size_t index) { return LayerServes(layers[index], name, walk); });
        if (rule != input.file_rules.end())
            item.note = "file_rule";
        else if (pack)
            item.note = "pack";
        else if (env.is_patch_file && env.is_patch_file(env.context, name))
            item.note = "patch_file";
        if (item.note.empty())
        {
            String text;
            const bool in_base = (env.base_exists && env.base_exists(env.context, name)) ||
                (env.base && env.base(env.context, name, text));
            if (!in_base)
                item.note = "no_base"; // a new file of the addon
        }
        for (std::size_t i = 0; item.note.empty() && i < sides.size(); ++i)
        {
            const fs::path path = PlanPathOf(layers[sides[i].second].folder + name);
            String bytes;
            if (!PlanReadBytes(path, bytes))
            {
                item.note = "read_error";
                item.error = "cannot read " + PlanUtf8Of(path);
            }
        }
        if (item.note.empty())
            item.note = "one_addon"; // its file is served as it is
    }
    if (extras.empty())
        return;
    Vector<PlanEntry> merged;
    merged.reserve(out.entries.size() + extras.size());
    std::merge(std::make_move_iterator(out.entries.begin()), std::make_move_iterator(out.entries.end()),
        std::make_move_iterator(extras.begin()), std::make_move_iterator(extras.end()), std::back_inserter(merged),
        [](const PlanEntry& a, const PlanEntry& b) { return a.item.file < b.item.file; });
    out.entries = std::move(merged);
}
} // namespace

void PlanMerge(const PlanInput& input, const PlanEnv& env, PlanResult& out)
{
    out = PlanResult();
    const WalkRules& walk = env.walk;
    const Vector<PlanLayer>& layers = input.layers;

    // The layers whose files take part: the public content of the addons and the sets of the providers. Not the pack
    // (it lies over all and serves its own files), not a writable layer
    Vector<std::size_t> addon_layers;
    for (std::size_t i = 0; i < layers.size(); ++i)
    {
        const PlanLayer& layer = layers[i];
        if (layer.writable || layer.merged || layer.owner.empty() || !layer.group.empty())
            continue;
        addon_layers.push_back(i);
    }
    out.owners = PlanOwners(layers);

    const auto info_of = [&input](const String& id) -> const PlanAddon* {
        const auto it = std::find_if(input.addons.begin(), input.addons.end(),
            [&id](const PlanAddon& addon) { return addon.id == id; });
        return it == input.addons.end() ? nullptr : &*it;
    };
    // The delta read for an owner (the engine reads the deltas of the owners only, PlanOwners)
    const auto delta_of = [&](const String& id) -> const AddonDelta* {
        if (!PlanContains(out.owners, id))
            return nullptr;
        const auto it = std::find_if(input.addons.begin(), input.addons.end(),
            [&id](const PlanAddon& addon) { return addon.id == id; });
        const std::size_t at = std::size_t(it - input.addons.begin());
        if (it == input.addons.end() || at >= input.deltas.size() || !input.deltas[at] || input.deltas[at]->file.empty())
            return nullptr;
        return input.deltas[at];
    };
    const auto has_atom_rule = [&input](const String& file, const String& owner) {
        return std::any_of(input.atom_rules.begin(), input.atom_rules.end(), [&](const PlayerAtomRule& rule) {
            return rule.file == file && (rule.winner == owner || rule.winner == kBaseOwner);
        });
    };

    // S9: an addon with a delta takes part alone too, so does an addon one atom: rule names a file of (15 section 4.4)
    bool any_alone = !input.atom_rules.empty();
    for (const String& owner : out.owners)
    {
        const AddonDelta* delta = info_of(owner) ? delta_of(owner) : nullptr;
        if (!delta)
            continue;
        any_alone = any_alone || (!delta->stale && std::any_of(delta->delta.files.begin(), delta->delta.files.end(),
            [](const DeltaFile& file) { return !file.not_in_ancestor; }));
    }
    // Made for an older game without a usable delta (15 section 2.2)
    for (const String& owner : out.owners)
    {
        const PlanAddon* info = info_of(owner);
        if (!info || !ReleaseTagOlder(info->game_version, input.game_tag))
            continue;
        const AddonDelta* delta = delta_of(owner);
        if (delta && !delta->stale)
            continue;
        out.guessed_addons.push_back(owner);
    }
    if (out.owners.size() < 2 && !any_alone && !input.all_files)
        return; // one addon at most, no delta, no atom: rule: nothing to merge

    // 1. The text files of each layer on the disk, as the file system mounts them
    std::map<String, Vector<std::size_t>> names; // name -> the layers that have it, bottom up
    for (const std::size_t index : addon_layers)
    {
        for (const String& name : LayerTexts(layers[index].folder, walk))
            names[name].push_back(index);
    }

    // 2. The names two addons give
    std::map<String, PlanTargets> targets; // <patch>\merge\targets.ltx, read once per plan
    for (const auto& [name, file_layers] : names)
    {
        Vector<String> disk_owners;
        for (const std::size_t index : file_layers)
            PlanAddOnce(disk_owners, layers[index].owner);
        const bool alone = disk_owners.size() == 1 &&
            (DeltaEntryOf(delta_of(disk_owners[0]), name) != nullptr || has_atom_rule(name, disk_owners[0]));
        if (disk_owners.size() < 2 && !alone)
            continue;
        const auto rule = std::find_if(input.file_rules.begin(), input.file_rules.end(),
            [&name = name](const FileRule& file_rule) { return file_rule.file == name; });

        // The topmost layer of each addon that gives the name ([private]; a file: rule - every layer is listed)
        Vector<std::pair<String, std::size_t>> sides;
        for (const std::size_t index : file_layers)
        {
            const PlanLayer& layer = layers[index];
            if (rule == input.file_rules.end() && !LayerServes(layer, name, walk))
                continue;
            const auto it = std::find_if(sides.begin(), sides.end(),
                [&layer](const std::pair<String, std::size_t>& side) { return side.first == layer.owner; });
            if (it == sides.end())
                sides.emplace_back(layer.owner, index);
            else
                it->second = index;
        }
        if (sides.size() < 2 && !(alone && sides.size() == 1))
            continue;
        std::sort(sides.begin(), sides.end(),
            [](const std::pair<String, std::size_t>& a, const std::pair<String, std::size_t>& b) {
                return a.second < b.second;
            });

        PlanEntry& entry = out.entries.emplace_back();
        entry.alone = alone;
        PlanFile& item = entry.item;
        PlanJob& job = entry.job;
        item.file = name;
        item.kind = KindName(KindOfName(name));
        for (const auto& [owner, index] : sides)
        {
            item.addons.push_back(owner);
            item.layers.push_back(layers[index].layer_id);
            job.side_layers.push_back(index);
        }

        const bool pack = std::any_of(layers.begin(), layers.end(), [&, &name = name](const PlanLayer& layer) {
            return !layer.group.empty() && !layer.writable && LayerServes(layer, name, walk);
        });
        if (rule != input.file_rules.end())
            item.note = "file_rule";
        else if (pack)
            item.note = "pack";
        else if (env.is_patch_file && env.is_patch_file(env.context, name))
            item.note = "patch_file";
        // The base has the file but no text of it (env.base_exists: the list of the files of a player's archives): the
        // file is planned as the engine plans it - the sides, the ancestors, the patches, the rules - but not merged
        bool no_base_text = false;
        if (item.note.empty() && !(env.base && env.base(env.context, name, job.base)))
        {
            if (env.base_exists && env.base_exists(env.context, name))
                no_base_text = true;
            else
                item.note = "no_base";
        }
        for (std::size_t i = 0; item.note.empty() && i < sides.size(); ++i)
        {
            const fs::path path = PlanPathOf(layers[sides[i].second].folder + name);
            if (!PlanReadBytes(path, job.texts.emplace_back()))
            {
                item.note = "read_error";
                item.error = "cannot read " + PlanUtf8Of(path);
            }
        }
        if (!item.note.empty())
        {
            item.status = "skipped";
            job = PlanJob();
            if (alone && item.note == "no_base")
                out.entries.pop_back(); // a new file of the addon: nothing of the base to revert
            continue;
        }

        // The ancestor of each side (S9: the delta, stale_delta, current_base_guessed - 15 sections 2.2-2.4)
        item.ancestors.assign(item.addons.size(), "base");
        item.ancestor_notes.assign(item.addons.size(), String());
        job.deltas.assign(item.addons.size(), nullptr);
        for (std::size_t i = 0; i < item.addons.size(); ++i)
        {
            const String& id = item.addons[i];
            const AddonDelta* delta = delta_of(id);
            if (DeltaStaleFor(delta, name))
            {
                item.ancestors[i] = "stale_delta";
                const auto at = std::find(delta->inconsistent.begin(), delta->inconsistent.end(), AsciiLower(name));
                item.ancestor_notes[i] = delta->file + ": " +
                    (delta->stale || at == delta->inconsistent.end() ?
                            delta->error :
                            delta->inconsistent_why[std::size_t(at - delta->inconsistent.begin())]);
                continue;
            }
            if (const DeltaFile* delta_entry = DeltaEntryOf(delta, name))
            {
                const String mismatch = DeltaFileMismatch(*delta_entry, job.texts[i]);
                if (!mismatch.empty())
                {
                    item.ancestors[i] = "stale_delta";
                    item.ancestor_notes[i] = delta->file + ": " + mismatch;
                    entry.log.push_back("~ [merge] " + name + ": the delta of " + id + " does not agree with its file (" +
                        mismatch + "): the delta is ignored, the merge may revert base changes");
                }
                else
                {
                    item.ancestors[i] = "delta:" + delta->delta.game;
                    item.ancestor_notes[i] = delta->file;
                    job.deltas[i] = delta_entry;
                }
                continue;
            }
            const PlanAddon* info = info_of(id);
            if (info && ReleaseTagOlder(info->game_version, input.game_tag))
            {
                item.ancestors[i] = "current_base_guessed";
                item.ancestor_notes[i] = "game_version " + info->game_version + " is older than " + input.game_tag;
            }
        }
        if (item.addons.size() == 1 && !job.deltas.front() && !has_atom_rule(name, item.addons.front()))
        {
            // One addon without a usable delta and without an atom: rule: nothing to merge, its file is served
            item.status = "skipped";
            item.note = "one_addon";
            job = PlanJob();
            continue;
        }

        // The compatibility patches of these addons that have a fragment of the file (15 section 6.1)
        for (const PlanAddon& info : input.addons)
        {
            if (!info.active || info.patches.empty() ||
                std::none_of(info.patches.begin(), info.patches.end(),
                    [&item](const String& id) { return PlanContains(item.addons, id); }))
                continue;
            const fs::path fragment =
                PlanPathOf(info.dir + "merge" + kPlanSeparator + "gamedata" + kPlanSeparator + name);
            std::error_code ec;
            if (!fs::is_regular_file(fragment, ec))
                continue; // the patch has nothing for this file
            PlanFile::Patch& patch = item.patches.emplace_back();
            patch.id = info.id;
            patch.targets = info.patches;
            const auto missing = std::find_if(info.patches.begin(), info.patches.end(),
                [&item](const String& id) { return !PlanContains(item.addons, id); });
            PlanPatchSide side;
            if (missing != info.patches.end())
            {
                patch.status = "skipped";
                patch.note = "not_a_side: " + *missing + " does not replace the file";
                continue;
            }
            if (!PlanReadBytes(fragment, side.text))
            {
                patch.status = "skipped";
                patch.note = "read_error: cannot read " + PlanUtf8Of(fragment);
                continue;
            }
            auto known = targets.find(info.id);
            if (known == targets.end())
            {
                String data;
                if (!PlanReadBytes(PlanPathOf(info.dir + "merge" + kPlanSeparator + "targets.ltx"), data))
                    data.clear(); // no file (or not read): no fingerprints, the versions are not checked
                known = targets.emplace(info.id, PlanParseTargets(data)).first;
            }
            const auto of_file = known->second.find(AsciiLower(name));
            patch.status = "applied";
            if (of_file == known->second.end())
                patch.note = "merge\\targets.ltx has no [" + name + "]: the versions are not checked";
            for (std::size_t i = 0; of_file != known->second.end() && i < item.addons.size(); ++i)
            {
                if (!PlanContains(info.patches, item.addons[i]))
                    continue;
                const auto hash = of_file->second.find(item.addons[i]);
                if (hash == of_file->second.end())
                    continue; // not written: not checked
                if (hash->second != Sha256Hex(job.texts[i]))
                    patch.stale_targets.push_back(item.addons[i]);
            }
            if (!patch.stale_targets.empty())
                patch.status = "stale";
            side.id = info.id;
            side.targets = info.patches;
            job.patches.push_back(std::move(side));
        }

        // The atom: rules for the file whose winner takes part (a side, a patch or the base): another winner would be
        // the base in the library - the status tells it instead (AtomRuleStatuses)
        for (const PlayerAtomRule& atom_rule : input.atom_rules)
        {
            if (atom_rule.file != name)
                continue;
            const String& winner = atom_rule.winner;
            const bool takes_part = winner == kBaseOwner || PlanContains(item.addons, winner) ||
                std::any_of(job.patches.begin(), job.patches.end(),
                    [&winner](const PlanPatchSide& side) { return side.id == winner; });
            if (takes_part)
                job.rules.push_back({atom_rule.atom, winner});
        }
        if (no_base_text)
        {
            item.status = "skipped";
            item.note = "no_base_text";
            continue; // entry.merge stays false: no base to merge against
        }
        entry.merge = true;
    }
    if (input.all_files)
        PlanAllFiles(input, env, names, out);
}

void MergePlanned(PlanFile& item, const PlanJob& job, const LuaChecker* lua, String& text, Vector<String>& log)
{
    const Vector<AtomRule>& rules = job.rules;
    MergeInput input;
    input.kind = KindOfWord(item.kind);
    input.name = item.file;
    input.base = job.base;
    input.rules = rules;
    // The merge names a changed gap "X", a delta (and so a rebase conflict) "@gap X": a rule of either spelling
    for (const AtomRule& rule : rules)
    {
        if (std::string_view(rule.atom).substr(0, kPlanGapPrefix.size()) != kPlanGapPrefix)
            continue;
        const String label = rule.atom.substr(kPlanGapPrefix.size());
        if (std::none_of(input.rules.begin(), input.rules.end(),
                [&label](const AtomRule& other) { return other.atom == label; }))
            input.rules.push_back({label, rule.owner});
    }
    input.options.validate = false; // checked below: a failed check is served by the upper layer
    MergeResult result;
    Vector<PlanFile::RebaseConflict> rebase_conflicts;
    Vector<String> rebase_rules;
    try
    {
        for (std::size_t i = 0; i < job.texts.size(); ++i)
        {
            Side& side = input.sides.emplace_back();
            side.owner = item.addons[i];
            side.text = job.texts[i];
            if (i < job.deltas.size() && job.deltas[i])
            {
                // S9: carried over by its delta first, here: the atom: rules decide the conflicts of the rebase too
                PlanSideRebase rebased = PlanRebaseSide(item, job.base, *job.deltas[i], item.addons[i], rules, rebase_rules);
                if (rebased.ok)
                    side.text = std::move(rebased.text);
                for (PlanFile::RebaseConflict& c : rebased.conflicts)
                    rebase_conflicts.push_back(std::move(c));
            }
        }
        for (const PlanPatchSide& patch : job.patches)
        {
            Side& side = input.sides.emplace_back();
            side.owner = patch.id;
            side.role = Role::Patch;
            side.text = patch.text;
            for (const String& target : patch.targets)
                side.patches.push_back(target);
        }
        result = Merge(input);
        PlanRulesOnSingle(item, result, job.base, rules, log);
    }
    catch (const std::exception& e)
    {
        item.status = "fallback";
        item.note = "merge_error";
        item.error = String("the merge failed: ") + e.what();
    }
    if (item.status.empty())
    {
        const Vector<String> single_rules = item.rules_applied; // PlanRulesOnSingle
        PlanFill(item, result);
        // The atoms the library decided, as the rules name them ("@gap X" for its "X")
        for (String& atom : item.rules_applied)
        {
            const bool exact =
                std::any_of(rules.begin(), rules.end(), [&atom](const AtomRule& rule) { return rule.atom == atom; });
            const AtomRule* rule = exact ? nullptr : PlanRuleOf(rules, atom);
            if (rule)
                atom = rule->atom;
        }
        for (const String& atom : single_rules)
            PlanAddOnce(item.rules_applied, atom);
        for (const String& atom : rebase_rules)
            PlanAddOnce(item.rules_applied, atom);
        for (PlanFile::RebaseConflict& c : rebase_conflicts)
        {
            const AtomRule* rule = c.resolved_by == "rebase" ? PlanRuleOf(rules, c.atom) : nullptr;
            if (rule && rule->owner != c.addon && PlanContains(item.rules_applied, rule->atom))
            {
                c.resolved_by = "player";
                c.winner = rule->owner;
            }
            item.unresolved += c.resolved_by == "rebase" ? 1 : 0;
            item.rebase_conflicts.push_back(std::move(c));
        }
        if (Serves(item))
        {
            String error;
            if (!Validate(input.kind, item.file, result.text, error, lua))
            {
                // The fallback serves the file of the upper layer only when that one passes the check
                String top_error;
                if (!job.texts.empty() && Validate(input.kind, item.file, job.texts.back(), top_error, lua))
                {
                    item.status = "fallback";
                    item.note = input.kind == Kind::Lua ? "compile" : "parse";
                    item.error = error;
                    item.lines.clear();
                }
                else
                {
                    item.note = "check_failed";
                    item.error = String("the merged text does not pass the check, the file of the upper side "
                                        "neither: ") + error;
                }
            }
        }
    }
    text = std::move(result.text);
}

bool Rebased(const PlanFile& item, std::string_view addon)
{
    const auto it = std::find_if(item.addons.begin(), item.addons.end(),
        [addon](const String& id) { return std::string_view(id) == addon; });
    if (it == item.addons.end())
        return false;
    const std::size_t i = std::size_t(it - item.addons.begin());
    return i < item.ancestors.size() && item.ancestors[i].compare(0, 6, "delta:") == 0;
}

bool Serves(const PlanFile& item)
{
    if (item.status == "merged" || item.status == "merged_lines")
        return true;
    if (item.addons.empty())
        return false;
    // One addon changed the base, the others lay copies of the base: its version, unless it is the upper one anyway,
    // was not carried over by a delta and no atom: rule = core took an atom of the base
    if (item.status == "single")
    {
        return item.changed.size() == 1 &&
            (item.changed.front() != item.addons.back() || Rebased(item, item.changed.front()) || item.rules_changed);
    }
    // No side changes the base after the rebase: the base is served when the upper side was carried over
    return item.status == "unchanged" && Rebased(item, item.addons.back());
}

void RefineResolved(PlanFile& item, const Vector<PlanAddon>& addons, const Vector<RuleStatus>& statuses)
{
    const auto layer_of = [&item](const String& owner) {
        const auto it = std::find(item.addons.begin(), item.addons.end(), owner);
        return it == item.addons.end() ? String() : item.layers[std::size_t(it - item.addons.begin())];
    };
    const auto part_of = [](const String& layer) { return layer.substr(0, layer.find(':')); };
    for (PlanFile::Conflict& c : item.conflicts)
    {
        if (c.resolved_by != "order")
            continue;
        const String winner_part = part_of(layer_of(c.winner));
        if (std::any_of(c.lost.begin(), c.lost.end(),
                [&](const String& lost) { return part_of(layer_of(lost)) != winner_part; }))
        {
            c.resolved_by = winner_part == "slot" ? "slot" : "order";
            continue;
        }
        if (std::any_of(statuses.begin(), statuses.end(), [&c](const RuleStatus& rule) {
                return rule.kind == "pair" && rule.applied && rule.winner == c.winner && PlanContains(c.lost, rule.target);
            }))
        {
            c.resolved_by = "pair";
            continue;
        }
        const auto addon =
            std::find_if(addons.begin(), addons.end(), [&c](const PlanAddon& info) { return info.id == c.winner; });
        if (addon != addons.end() && std::any_of(c.lost.begin(), c.lost.end(),
                [&addon](const String& lost) { return PlanContains(addon->load_after, lost); }))
            c.resolved_by = "load_after";
    }
}

void CountPatches(PlanFile& item)
{
    for (PlanFile::Patch& patch : item.patches)
    {
        if (patch.status == "skipped")
            continue;
        for (PlanFile::Conflict& c : item.conflicts)
        {
            if (c.resolved_by != "patch" || c.winner != patch.id)
                continue;
            ++patch.resolved;
            if (patch.status == "stale")
            {
                c.stale = true;
                ++item.unresolved;
            }
        }
    }
}

bool SameAtom(std::string_view a, std::string_view b)
{
    if (a.substr(0, kPlanGapPrefix.size()) == kPlanGapPrefix)
        a.remove_prefix(kPlanGapPrefix.size());
    if (b.substr(0, kPlanGapPrefix.size()) == kPlanGapPrefix)
        b.remove_prefix(kPlanGapPrefix.size());
    return a == b;
}

Vector<RuleStatus> AtomRuleStatuses(const Vector<PlayerAtomRule>& rules, const Vector<PlanFile>& records)
{
    Vector<RuleStatus> out;
    for (const PlayerAtomRule& rule : rules)
    {
        RuleStatus& status = out.emplace_back();
        status.kind = "atom";
        status.winner = rule.winner;
        status.target = rule.file + "|" + rule.atom;
        const auto found = std::find_if(records.begin(), records.end(),
            [&rule](const PlanFile& record) { return record.file == rule.file; });
        const bool core = rule.winner == kBaseOwner;
        if (found == records.end())
        {
            status.note = "the file is not merged: no addon replaces it (or the rule names an addon that does not)";
            continue;
        }
        const PlanFile& item = *found;
        const bool side = PlanContains(item.addons, rule.winner) ||
            std::any_of(item.patches.begin(), item.patches.end(), [&rule](const PlanFile::Patch& patch) {
                return patch.id == rule.winner && patch.status != "skipped";
            });
        const bool decided = PlanContains(item.rules_applied, rule.atom);
        // The rebase of that addon left the atom to the base: the variant of the addon has no place
        const auto base_kept = std::find_if(item.rebase_conflicts.begin(), item.rebase_conflicts.end(),
            [&rule](const PlanFile::RebaseConflict& c) {
                return c.addon == rule.winner && c.winner == kBaseOwner && SameAtom(c.atom, rule.atom);
            });
        if (item.status == "skipped")
            status.note = "the file is not merged (" + item.note + ")";
        else if (!core && !side)
            status.note = rule.winner + " does not replace the file";
        else if (!decided && !core && base_kept != item.rebase_conflicts.end())
        {
            status.note = "the base keeps this atom (" + base_kept->reason + "): the variant of " + rule.winner +
                " has no place in the file, only = core applies to it";
        }
        else if (!decided)
            status.note = "no addon changes this atom (or the file has no such atom)";
        else if (!item.served && item.status == "single" && !item.changed.empty() && item.changed.front() == rule.winner)
        {
            status.applied = true;
            status.note = "the atom of " + rule.winner + " is served";
        }
        else if (!item.served)
            status.note =
                "the merged file is not served (" + item.status + (item.note.empty() ? "" : ": ") + item.note + ")";
        else
        {
            status.applied = true;
            status.note = core ? "the atom of the base is served" : "the atom of " + rule.winner + " is served";
        }
    }
    return out;
}
} // namespace xray::content_merge
