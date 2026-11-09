#include "StdAfx.h"

// Before the headers of the addons: addon_host.h names ::lua_State
#include <luabind/lua_include.hpp>

#include "addon_merge.h"
#include "addon_content.h"
#include "addon_host.h"
#include "addon_slots.h"

#include "xrCore/ContentMerge/ContentMerge.h"
#include "xrCore/ContentMerge/Plan.h"
#include "xrCore/ContentPatches.h"
#include "xrCore/Crypto/xr_sha.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#if !defined(XR_PLATFORM_WINDOWS)
#include <unistd.h> // getpid
#endif

namespace gw::addons
{
namespace
{
namespace cm = xray::content_merge;

// <app_data_root>\addons_merged\ (15-merge-implementation.md 4.3):
//   cache\<key>.txt     the merged text (UTF-8, LF), only for a file the layer serves
//   cache\<key>.meta    the result: the status, the counters, the conflicts, the map of the lines (kMergeMetaFormat)
//   <set>\gamedata\     the folder of the layer: the merged files of one set (hard links to cache\, else copies)
//   <set>\merge.ltx     what the set has: <file> = <key>; written last, its time - the last use of the set
// <key> - sha1 of the inputs (MergeKeyOf), <set> - the first 16 hex chars of sha1 of kMergeMetaFormat and the list
// "<file>=<key>" (another set under that name: "<set>-<n>").
// Several instances of the game may share one appdata: a set is never written in place - it is built in a temporary
// folder "<set>.tmp<pid>_<n>" and renamed whole; a ready set (its merge.ltx is the list of the plan) is reused as it
// is, so a set another instance has mounted is never touched; the cleanup removes only the sets nobody used for
// kMergeSetDays. The files of the cache are written through temporary files of their own the same way.
constexpr pcstr kMergeFolder = "addons_merged";
constexpr pcstr kMergeCacheFolder = "cache";
// The format of the results of the cache and the version of the merge of the engine (this file): goes up with any
// change here that changes a result or the meta - a part of the key (MergeKeyOf) and of the list of a set.
// 2: the rebase by the engine (atom: rules), rules_changed; 3: "text <size> <sha1>" - the merged text is checked at a
// hit of the cache and in a ready set (a file cut short or changed by hand is merged again)
constexpr pcstr kMergeMetaFormat = "gw-merge-meta 3";
constexpr pcstr kMergeSetListing = "merge.ltx";
constexpr size_t kMergeSetNameLength = 16;
constexpr size_t kMergeOldSetNameLength = 8; // the sets of the first version (the cleanup takes them too)
constexpr u32 kMergeSetProbes = 8; // "<set>", "<set>-1" ... when a folder of the name holds another set
constexpr int kMergeSetDays = 7; // a set nobody mounted for so long goes (another instance may use a younger one)
constexpr int kMergeTempDays = 1; // a temporary folder or file of a write that did not finish (a crash) goes
constexpr size_t kMergeCacheKeep = 500; // the cleanup leaves this many results at least
constexpr int kMergeCacheDays = 30; // and removes only the ones not used for so long

xr_vector<ContentMergeFile> g_merges; // by name
xr_string g_merge_folder; // the folder of the layer of the last plan ("" - no layer)
xr_vector<ContentRuleStatus> g_atom_rule_statuses; // the atom: rules at the last plan (MergeRuleStatuses)

// ---- small helpers ------------------------------------------------------------------------------------------------

xr_string MergeHex(const crypto::xr_sha1::hash_t& hash)
{
    static constexpr char digits[] = "0123456789abcdef";
    xr_string out;
    out.reserve(hash.size() * 2);
    for (const u8 byte : hash)
    {
        out += digits[byte >> 4];
        out += digits[byte & 15];
    }
    return out;
}

xr_string MergeSha1(const xr_string& data)
{
    return MergeHex(crypto::xr_sha1::calculate(reinterpret_cast<const u8*>(data.data()), u32(data.size())));
}

// "<size> <sha1>" of a merged text (ContentMergeFile::text_hash)
xr_string MergeTextHash(const xr_string& data)
{
    return xr_string(std::to_string(data.size()).c_str()) + " " + MergeSha1(data);
}

// The file system index spells the names in lower case on Windows (xr_fs_strlwr), as the disk on POSIX: the names of
// the merge are the names of the index (the conflicts, the file: rules)
xr_string MergeIndexName(xr_string name)
{
    for (char& c : name)
    {
        if (c == '/')
            c = '\\';
#if defined(XR_PLATFORM_WINDOWS)
        else
            c = static_cast<char>(std::tolower(u8(c)));
#endif
    }
    return name;
}

bool MergeReadFile(const std::filesystem::path& path, xr_string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0)
        return false;
    in.seekg(0, std::ios::beg);
    out.resize(size_t(size));
    if (size > 0)
        in.read(out.data(), size);
    return bool(in);
}

// The file is the merged text of `text_hash` (MergeTextHash): a file of the cache or of a set cut short (a full disk,
// a crash) or edited by hand is not served
bool MergeTextMatches(const std::filesystem::path& path, const xr_string& text_hash)
{
    xr_string data;
    return !text_hash.empty() && MergeReadFile(path, data) && MergeTextHash(data) == text_hash;
}

// ".tmp<pid>_<n>": a temporary name no other instance of the game sharing the appdata uses
xr_string MergeTempSuffix()
{
    static u32 counter = 0;
#if defined(XR_PLATFORM_WINDOWS)
    const u32 pid = u32(GetCurrentProcessId());
#else
    const u32 pid = u32(getpid());
#endif
    return ".tmp" + xr_string(std::to_string(pid).c_str()) + "_" + xr_string(std::to_string(++counter).c_str());
}

// Through a temporary file of its own: a reader (this or another instance of the game) never sees a part of it. A
// file that has these bytes already is not written again (the names of the cache are the hashes of their inputs)
bool MergeWriteFile(const std::filesystem::path& path, const xr_string& data, xr_string& error)
{
    xr_string existing;
    if (MergeReadFile(path, existing) && existing == data)
        return true;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path temporary = path;
    temporary += MergeTempSuffix().c_str();
    {
        // The state after close: the last bytes are written by the flush of close (a full disk shows only there),
        // a file cut short must not be renamed over the name
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(data.data(), std::streamsize(data.size()));
        out.close();
        if (out.fail())
        {
            error = "cannot write " + PathToUtf8(temporary);
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec)
    {
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
        // Another instance wrote the same name meanwhile (and holds it open): the same bytes are fine
        if (MergeReadFile(path, existing) && existing == data)
            return true;
        error = "cannot replace " + PathToUtf8(path) + ": " + ErrorText(ec);
        return false;
    }
    return true;
}

// The time of the last use of a file of the cache or of a set (the cleanup goes by it). A failure is no matter
void MergeTouch(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), ec);
}

xr_string MergeRoot()
{
    string_path root;
    FS.update_path(root, "$app_data_root$", kMergeFolder);
    xr_string out = root;
    if (!out.empty() && out.back() != '\\' && out.back() != '/')
        out += DELIMITER;
    return out;
}

xr_string MergeJoin(const xr_vector<xr_string>& items)
{
    xr_string out;
    for (const xr_string& item : items)
        out += (out.empty() ? "" : ", ") + item;
    return out;
}

// One line of the meta file: the tab separates the fields, so a tab or a line end inside a field goes
xr_string MergeField(const xr_string& text)
{
    xr_string out = text;
    std::replace_if(out.begin(), out.end(), [](char c) { return c == '\t' || c == '\n' || c == '\r'; }, ' ');
    return out;
}

xr_vector<xr_string> MergeSplit(const xr_string& text, char separator)
{
    xr_vector<xr_string> out;
    size_t start = 0;
    while (start <= text.size())
    {
        size_t end = text.find(separator, start);
        if (end == xr_string::npos)
            end = text.size();
        out.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

xr_string MergeIds(const xr_vector<xr_string>& ids)
{
    xr_string out;
    for (const xr_string& id : ids)
        out += (out.empty() ? "" : ",") + id; // the owners are addon ids (a-z 0-9 _) or "core"
    return out;
}

xr_vector<xr_string> MergeIdsOf(const xr_string& text)
{
    xr_vector<xr_string> out;
    if (text.empty())
        return out;
    for (xr_string& id : MergeSplit(text, ','))
    {
        if (!id.empty())
            out.push_back(std::move(id));
    }
    return out;
}

// ---- the check of the merged Lua text (15 section 5.1) ---------------------------------------------------------------

void* MergeLuaAlloc(void* /*context*/, void* pointer, size_t /*old_size*/, size_t new_size)
{
    if (!new_size)
    {
        xr_free(pointer);
        return nullptr;
    }
    return xr_realloc(pointer, new_size);
}

// luaL_loadbuffer in a lua_State of its own, nothing runs: the syntax and the limits of LuaJIT (200 locals per
// function, upvalues). The game state cannot be used: the content procedure restarts Lua later (its step 8)
bool MergeLuaCompile(void* /*context*/, const char* text, size_t size, const char* chunk_name, cm::String& error)
{
    lua_State* L = lua_newstate(MergeLuaAlloc, nullptr);
    if (!L)
        return true; // no check is possible: the lexer of the library checked the blocks already
    const int code = luaL_loadbuffer(L, text, size, chunk_name);
    if (code)
    {
        const char* message = lua_tostring(L, -1);
        error = message ? message : "luaL_loadbuffer failed";
    }
    lua_close(L);
    return code == 0;
}

// ---- the cache ------------------------------------------------------------------------------------------------------

void MergeWriteMeta(const ContentMergeFile& item, xr_string& out)
{
    out = kMergeMetaFormat;
    out += '\n';
    const auto line = [&out](std::initializer_list<xr_string> fields) {
        bool first = true;
        for (const xr_string& field : fields)
        {
            out += first ? "" : "\t";
            out += MergeField(field);
            first = false;
        }
        out += '\n';
    };
    line({ "status", item.status });
    line({ "note", item.note });
    line({ "changed", MergeIds(item.changed) });
    line({ "atoms_merged", xr_string(std::to_string(item.atoms_merged).c_str()) });
    line({ "adjacent", xr_string(std::to_string(item.adjacent).c_str()) });
    line({ "unresolved", xr_string(std::to_string(item.unresolved).c_str()) });
    line({ "error", item.error });
    for (const xr_string& atom : item.adjacent_atoms)
        line({ "adjacent_atom", atom });
    for (const xr_string& atom : item.reordered)
        line({ "reordered", atom });
    for (const xr_string& atom : item.rules_applied)
        line({ "rule_applied", atom });
    for (const ContentMergeFile::Conflict& c : item.conflicts)
    {
        line({ "conflict", c.atom, c.winner, c.resolved_by, xr_string(std::to_string(c.base_line).c_str()),
            MergeIds(c.addons), MergeIds(c.lost) });
    }
    if (item.rules_changed)
        line({ "rules_changed", "1" });
    if (!item.text_hash.empty())
        line({ "text", item.text_hash });
    for (const ContentMergeFile::RebaseConflict& c : item.rebase_conflicts)
        line({ "rebase_conflict", c.atom, c.addon, c.reason, c.note, c.winner, c.resolved_by });
    for (const ContentMergeFile::Lines& l : item.lines)
    {
        line({ "lines", xr_string(std::to_string(l.from).c_str()), xr_string(std::to_string(l.to).c_str()),
            l.owner });
    }
}

u32 MergeNumber(const xr_string& text) { return u32(std::strtoul(text.c_str(), nullptr, 10)); }

bool MergeReadMeta(const xr_string& data, ContentMergeFile& item)
{
    xr_vector<xr_string> lines = MergeSplit(data, '\n');
    if (lines.empty() || lines.front() != kMergeMetaFormat)
        return false;
    for (size_t i = 1; i < lines.size(); ++i)
    {
        if (lines[i].empty())
            continue;
        const xr_vector<xr_string> f = MergeSplit(lines[i], '\t');
        const xr_string& tag = f[0];
        if (tag == "status" && f.size() > 1)
            item.status = f[1];
        else if (tag == "note" && f.size() > 1)
            item.note = f[1];
        else if (tag == "changed" && f.size() > 1)
            item.changed = MergeIdsOf(f[1]);
        else if (tag == "atoms_merged" && f.size() > 1)
            item.atoms_merged = MergeNumber(f[1]);
        else if (tag == "adjacent" && f.size() > 1)
            item.adjacent = MergeNumber(f[1]);
        else if (tag == "unresolved" && f.size() > 1)
            item.unresolved = MergeNumber(f[1]);
        else if (tag == "error" && f.size() > 1)
            item.error = f[1];
        else if (tag == "adjacent_atom" && f.size() > 1)
            item.adjacent_atoms.push_back(f[1]);
        else if (tag == "reordered" && f.size() > 1)
            item.reordered.push_back(f[1]);
        else if (tag == "rule_applied" && f.size() > 1)
            item.rules_applied.push_back(f[1]);
        else if (tag == "conflict" && f.size() > 6)
        {
            ContentMergeFile::Conflict& c = item.conflicts.emplace_back();
            c.atom = f[1];
            c.winner = f[2];
            c.resolved_by = f[3];
            c.base_line = MergeNumber(f[4]);
            c.addons = MergeIdsOf(f[5]);
            c.lost = MergeIdsOf(f[6]);
        }
        else if (tag == "rules_changed" && f.size() > 1)
            item.rules_changed = f[1] == "1";
        else if (tag == "text" && f.size() > 1)
            item.text_hash = f[1];
        else if (tag == "rebase_conflict" && f.size() > 4)
        {
            ContentMergeFile::RebaseConflict& c = item.rebase_conflicts.emplace_back();
            c.atom = f[1];
            c.addon = f[2];
            c.reason = f[3];
            c.note = f[4];
            c.winner = f.size() > 5 && !f[5].empty() ? f[5] : c.addon;
            if (f.size() > 6 && !f[6].empty())
                c.resolved_by = f[6];
        }
        else if (tag == "lines" && f.size() > 3)
            item.lines.push_back({ MergeNumber(f[1]), MergeNumber(f[2]), f[3] });
    }
    return !item.status.empty();
}

// The inputs of the result (15 section 4.3): the versions of the code that makes it - of the parse
// (kAlgorithmVersion), of the results of the library (kResultRevision: a change of a result without a new version of
// the parse) and of the merge of the engine (kMergeMetaFormat) -, the kind, the name, the base, the sides in their
// order (the order decides the conflicts) with the entries of their deltas (S9; "-" - none used), the fragments of
// the compatibility patches over them and the atom: rules of the player for the file
xr_string MergeKeyOf(const ContentMergeFile& item, const xr_string& base, const xr_vector<xr_string>& sides,
    const xr_vector<const cm::DeltaFile*>& deltas, const xr_vector<cm::PlanPatchSide>& patches,
    const xr_vector<cm::AtomRule>& rules)
{
    xr_string text = cm::kAlgorithmVersion;
    text += "\nrevision " + xr_string(std::to_string(cm::kResultRevision).c_str()) + "\n" + kMergeMetaFormat;
    text += "\n" + item.kind + "\n" + item.file + "\nbase " + MergeSha1(base) + "\n";
    for (size_t i = 0; i < sides.size(); ++i)
    {
        const cm::DeltaFile* delta = i < deltas.size() ? deltas[i] : nullptr;
        text += "addon " + item.addons[i] + " " + MergeSha1(sides[i]) + " " +
            (delta ? MergeSha1(cm::DeltaFileToJson(*delta)) : xr_string("-")) + "\n";
    }
    for (const cm::PlanPatchSide& patch : patches)
        text += "patch " + patch.id + " " + MergeSha1(patch.text) + " " + MergeIds(patch.targets) + "\n";
    for (const cm::AtomRule& rule : rules)
    {
        text += "rule " + xr_string(rule.atom.data(), rule.atom.size()) + "\t" +
            xr_string(rule.owner.data(), rule.owner.size()) + "\n";
    }
    text += "adjacent 1\n";
    return MergeSha1(text);
}

// ---- the merge of one file ----------------------------------------------------------------------------------------

// The plan and the merge of a file are the library's (xrCore ContentMerge/Plan.h: cm::PlanMerge, cm::MergePlanned,
// cm::Serves, cm::RefineResolved, cm::CountPatches - the code of gw_merge plan); the engine keeps the cache, the set
// of the merged files, the layer and the log

// The result of one file: the cache, else the merge and the check. `cache` - <root>cache\ (UTF-8)
void MergeOne(ContentMergeFile& item, const cm::PlanJob& job, const xr_string& cache)
{
    item.key = MergeKeyOf(item, job.base, job.texts, job.deltas, job.patches, job.rules);
    const std::filesystem::path meta_path = PathFromUtf8(cache + item.key + ".meta");
    const std::filesystem::path text_path = PathFromUtf8(cache + item.key + ".txt");
    xr_string meta;
    if (MergeReadFile(meta_path, meta))
    {
        // A result the layer serves: its text must be the one the meta tells (size, sha1), else merged again
        ContentMergeFile cached = item;
        if (MergeReadMeta(meta, cached) && (!cm::Serves(cached) || MergeTextMatches(text_path, cached.text_hash)))
        {
            item = std::move(cached);
            item.cached = true;
            MergeTouch(meta_path); // the cleanup removes the results not used for long (CleanContentMergeFolders)
            return;
        }
    }

    // The merge and the check of the library (cm::MergePlanned): the sides carried over by their deltas under the
    // atom: rules, the fragments of the patches, the rules; a merged text that fails the check while the file of the
    // upper side passes - fallback
    cm::LuaChecker lua;
    lua.compile = &MergeLuaCompile;
    cm::String merged;
    cm::Vector<cm::String> log;
    cm::MergePlanned(item, job, &lua, merged, log);
    for (const cm::String& line : log)
        Msg("%s", line.c_str());

    xr_string error;
    if (cm::Serves(item))
    {
        const xr_string& text = merged;
        if (!MergeWriteFile(text_path, text, error))
        {
            item.status = "fallback";
            item.note = "write_error";
            item.error = error;
            return; // not cached: a passing cause (a file held by another process) would keep the fallback for good
        }
        item.text_hash = MergeTextHash(text);
    }
    MergeWriteMeta(item, meta);
    if (!MergeWriteFile(meta_path, meta, error))
        Msg("~ [merge] %s: the result is not cached: %s", item.file.c_str(), error.c_str());
}

void MergeLog(const ContentMergeFile& item)
{
    const pcstr file = item.file.c_str();
    const pcstr from_cache = item.cached ? " (cache)" : "";
    if (item.status == "skipped")
    {
        pcstr why = "";
        if (item.note == "file_rule")
            why = "the file: rule of appdata\\addons.ltx [conflicts] chose its layer";
        else if (item.note == "pack")
            why = "the location pack gives it";
        else if (item.note == "no_base")
            why = "the base has no such file: the upper addon serves it";
        else if (item.note == "patch_file")
            why = "a patch file: each one is applied in the order of the layers";
        else if (item.note == "read_error")
            why = item.error.c_str();
        else if (item.note == "one_addon")
            why = "one addon replaces it, its delta is not used: its file is served";
        Msg("%s [merge] %s: not merged (%s), addons: %s", item.note == "read_error" ? "!" : "*", file, why,
            MergeJoin(item.addons).c_str());
        return;
    }
    if (item.status == "fallback")
    {
        const pcstr verb = item.note == "compile" ? "the merged text does not compile" :
            item.note == "parse" ? "the merged text does not parse" : "the merge failed";
        Msg("! [merge] %s: %s: %s; served the file of %s (order)%s", file, verb, item.error.c_str(),
            item.addons.empty() ? "?" : item.addons.back().c_str(), from_cache);
        return;
    }
    if (item.note == "check_failed")
    {
        Msg("~ [merge] %s: %s (the upper side: %s); the merged text is served%s", file, item.error.c_str(),
            item.addons.empty() ? "?" : item.addons.back().c_str(), from_cache);
    }
    if (item.status == "unchanged")
    {
        Msg("* [merge] %s: the copies of %s are the base file%s%s", file, MergeJoin(item.addons).c_str(),
            cm::Serves(item) ? " after the rebase by the delta: the base is served" : "", from_cache);
        return;
    }
    if (item.status == "single")
    {
        const xr_string changed = item.changed.empty() ? xr_string("?") : item.changed.front();
        Msg("* [merge] %s: only %s changes the base%s: %s%s", file, changed.c_str(),
            item.addons.size() > 1 ? " (the copies of the others are the base)" : "",
            !cm::Serves(item) ? "it serves the file anyway" :
                cm::Rebased(item, changed) ? "its version carried over onto the current base by its delta is served" :
                                              "its version is served",
            from_cache);
        if (!item.rules_applied.empty())
        {
            Msg("* [merge] %s: atom: rules of the player decided %s%s", file, MergeJoin(item.rules_applied).c_str(),
                item.rules_changed ? " (the atoms of the base taken back into the version of the addon)" : "");
        }
        return;
    }
    Msg("* [merge] %s: merged the changes of %u addon(s) (%s), %u atom(s), %u adjacent%s%s", file,
        u32(item.changed.size()), MergeJoin(item.changed).c_str(), item.atoms_merged, item.adjacent,
        item.status == "merged_lines" ? ", by lines (the base is not understood: " : "",
        item.status == "merged_lines" ? (item.error + ")" + from_cache).c_str() : from_cache);
    for (const ContentMergeFile::Conflict& c : item.conflicts)
    {
        // Decided by the player or a patch - not a problem: '*'
        const bool open = c.stale || (c.resolved_by != "player" && c.resolved_by != "patch");
        Msg("%s [merge] conflict %s %s addons: %s winner: %s (%s)%s", open ? "~" : "*", file, c.atom.c_str(),
            MergeJoin(c.addons).c_str(), c.winner.c_str(), c.resolved_by.c_str(), c.stale ? ", patch_stale" : "");
    }
}

// S9: the atoms of the deltas the base changed too (15 section 2.4): the addon wins them - '~', unresolved
void MergeLogRebase(const ContentMergeFile& item)
{
    for (const ContentMergeFile::RebaseConflict& c : item.rebase_conflicts)
    {
        if (c.reason == "rebase_failed")
        {
            Msg("~ [merge] %s: the delta of %s is not carried over (%s): the file of the addon is used as it is, the "
                "merge may revert base changes", item.file.c_str(), c.addon.c_str(), c.note.c_str());
            continue;
        }
        if (c.resolved_by == "player")
        {
            Msg("* [merge] rebase conflict %s %s addon: %s (%s) decided by the atom: rule: the atom of %s is served",
                item.file.c_str(), c.atom.c_str(), c.addon.c_str(), c.reason.c_str(),
                c.winner == cm::kBaseOwner ? "the base (core)" : c.winner.c_str());
            continue;
        }
        Msg("~ [merge] rebase conflict %s %s addon: %s (%s): %s%s%s (addon_atom_winner %s|%s <id | core>)",
            item.file.c_str(), c.atom.c_str(), c.addon.c_str(), c.reason.c_str(),
            c.winner == cm::kBaseOwner ? "the base keeps its atom, the change of the addon is lost" :
                                         "the atom of the addon is served",
            c.note.empty() ? "" : "; ", c.note.c_str(), item.file.c_str(), c.atom.c_str());
    }
}

// The compatibility patches of the file: used, stale, not used
void MergeLogPatches(const ContentMergeFile& item)
{
    for (const ContentMergeFile::Patch& patch : item.patches)
    {
        if (patch.status == "skipped")
        {
            Msg("~ [merge] %s: the compatibility patch %s (for %s) is not used: %s", item.file.c_str(),
                patch.id.c_str(), MergeJoin(patch.targets).c_str(), patch.note.c_str());
        }
        else if (patch.status == "stale")
        {
            Msg("~ [merge] %s: patch_stale: the compatibility patch %s is made for other versions of the file of %s "
                "(merge\\targets.ltx): the %u conflict(s) it decides count as unresolved", item.file.c_str(),
                patch.id.c_str(), MergeJoin(patch.stale_targets).c_str(), patch.resolved);
        }
        else
        {
            Msg("* [merge] %s: the compatibility patch %s (for %s) decides %u conflict(s)%s%s", item.file.c_str(),
                patch.id.c_str(), MergeJoin(patch.targets).c_str(), patch.resolved, patch.note.empty() ? "" : "; ",
                patch.note.c_str());
        }
    }
}

bool MergeConflictOpen(const ContentMergeFile::Conflict& c)
{
    return c.stale || (c.resolved_by != "player" && c.resolved_by != "patch");
}

// The statuses of the atom: rules of the player at the last plan (ContentMergeRuleStatuses): cm::AtomRuleStatuses
void MergeRuleStatuses()
{
    cm::Vector<cm::PlayerAtomRule> rules;
    for (const AtomRule& rule : AtomRules())
        rules.push_back({ rule.file, rule.atom, rule.winner });
    g_atom_rule_statuses = cm::AtomRuleStatuses(rules, g_merges);
}

// A ready set: its merge.ltx is the list of the plan and every file of the list is there with the text of its result
// (size, sha1: a hard link to a text of the cache that was cut short keeps the bad text after the cache is written
// again). A set is renamed into its place whole (MergeBuildSet), so its merge.ltx says it is complete; the files are
// checked against a hand cleanup or edit, a crash, a full disk
bool MergeSetReady(const xr_string& set_folder, const xr_string& listing)
{
    xr_string existing;
    if (!MergeReadFile(PathFromUtf8(set_folder + kMergeSetListing), existing) || existing != listing)
        return false;
    const xr_string gamedata = set_folder + "gamedata" + DELIMITER;
    for (const ContentMergeFile& item : g_merges)
    {
        if (item.served && !MergeTextMatches(PathFromUtf8(gamedata + item.file), item.text_hash))
            return false;
    }
    return true;
}

// The set built in a temporary folder of its own (hard links to cache\, else copies; merge.ltx last) and renamed into
// `set_folder` whole: another instance of the game never sees a part of it. False - not built (error) or the name is
// taken (another instance built the set meanwhile: the caller checks it)
bool MergeBuildSet(const xr_string& set_folder, const xr_string& listing, xr_string& error)
{
    xr_string temporary_folder = set_folder;
    while (!temporary_folder.empty() && (temporary_folder.back() == '\\' || temporary_folder.back() == '/'))
        temporary_folder.pop_back();
    temporary_folder += MergeTempSuffix() + DELIMITER;
    const std::filesystem::path temporary = PathFromUtf8(temporary_folder);
    const auto drop = [&temporary]() {
        std::error_code remove_error;
        std::filesystem::remove_all(temporary, remove_error);
    };
    std::error_code ec;
    const xr_string gamedata = temporary_folder + "gamedata" + DELIMITER;
    const xr_string cache = MergeRoot() + kMergeCacheFolder + DELIMITER;
    for (const ContentMergeFile& item : g_merges)
    {
        if (!item.served)
            continue;
        const std::filesystem::path from = PathFromUtf8(cache + item.key + ".txt");
        const std::filesystem::path to = PathFromUtf8(gamedata + item.file);
        std::filesystem::create_directories(to.parent_path(), ec);
        ec.clear();
        std::filesystem::create_hard_link(from, to, ec);
        if (ec)
        {
            ec.clear();
            std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
        }
        if (ec)
        {
            error = "cannot write " + PathToUtf8(to) + ": " + ErrorText(ec);
            drop();
            return false;
        }
    }
    if (!MergeWriteFile(PathFromUtf8(temporary_folder + kMergeSetListing), listing, error))
    {
        drop();
        return false;
    }
    xr_string target = set_folder;
    while (!target.empty() && (target.back() == '\\' || target.back() == '/'))
        target.pop_back();
    std::filesystem::rename(temporary, PathFromUtf8(target), ec);
    if (ec)
    {
        error = "cannot rename the folder of the set into " + target + ": " + ErrorText(ec);
        drop();
        return false;
    }
    return true;
}

// The folder of the set of the plan (15 section 4.3): a ready set of the same list is taken as it is - it may be the
// mounted one or the one another instance of the game sharing the appdata has mounted, so nothing of it is written;
// else the set is built aside and renamed in. A folder of the name that holds another set (a collision of the names,
// a set of an older version of the game) is never touched: the next name "<set>-<n>" is tried. `set_folder` - the
// folder taken (with the separator). False - no folder can be written
bool MergeWriteSet(const xr_string& root, const xr_string& set_name, const xr_string& listing, xr_string& set_folder,
    xr_string& error)
{
    for (u32 probe = 0; probe < kMergeSetProbes; ++probe)
    {
        const xr_string folder =
            root + set_name + (probe ? "-" + xr_string(std::to_string(probe).c_str()) : xr_string()) + DELIMITER;
        if (MergeSetReady(folder, listing))
        {
            MergeTouch(PathFromUtf8(folder + kMergeSetListing));
            set_folder = folder;
            return true;
        }
        std::error_code ec;
        if (std::filesystem::exists(PathFromUtf8(folder), ec))
            continue; // another set, or one being renamed in by another instance: not touched
        xr_string build_error;
        if (MergeBuildSet(folder, listing, build_error) || MergeSetReady(folder, listing))
        {
            MergeTouch(PathFromUtf8(folder + kMergeSetListing));
            set_folder = folder;
            return true;
        }
        error = build_error;
        if (!std::filesystem::exists(PathFromUtf8(folder), ec))
            return false; // not a taken name: the folder cannot be written at all
    }
    if (error.empty())
        error = "the names of the set are taken by other sets";
    return false;
}

// ---- S9: the deltas of the addons, the version of the game (15 sections 2.2-2.4, 7.3) ------------------------------

// <addon>\.gw_merge\ - the launcher writes it next to its marker .gw_addon.json (not mounted: only gamedata\ is)
constexpr pcstr kMergeDeltaFolder = ".gw_merge";

// The delta of an addon: <addon>\.gw_merge\delta.json only (the launcher writes it - the recomputed delta of the
// catalog for the version of the parse of the engine when there is one, 15 section 7.3; its "algo" says which version
// made it). No file - no delta (a local addon). Read by the library (cm::ReadAddonDelta - gw_merge plan reads it the
// same way). A broken file, another version of the parse or entries that do not agree with themselves - one line in the
// log for the addon: those are stale_delta (the merge as without the delta, S8)
cm::AddonDelta MergeReadDelta(const AddonInfo& info)
{
    cm::AddonDelta out;
    const xr_string name = "delta.json";
    xr_string data;
    if (!MergeReadFile(PathFromUtf8(info.dir + kMergeDeltaFolder + DELIMITER + name), data))
        return out;
    cm::ReadAddonDelta(xr_string(kMergeDeltaFolder) + DELIMITER + name, std::string_view(data.data(), data.size()), out);
    if (out.stale)
    {
        // out.error: "is not read (<why>)" / "is made by <algo>, the engine parses by <algo>"
        if (!out.read)
        {
            Msg("! [merge] %s: %s %s: stale_delta, the addon is merged without its delta, the merge may revert base "
                "changes", info.id.c_str(), out.file.c_str(), out.error.c_str());
        }
        else
        {
            Msg("~ [merge] %s: %s %s: stale_delta, the addon is merged without its delta, the merge may revert base "
                "changes (the launcher takes the recomputed delta from the catalog)", info.id.c_str(), out.file.c_str(),
                out.error.c_str());
        }
        return out;
    }
    if (!out.inconsistent.empty())
    {
        // The first entry of the delta that does not agree with the format (the order of the delta)
        xr_string first;
        for (const cm::DeltaFile& file : out.delta.files)
        {
            const cm::String problem = cm::DeltaFileProblem(file);
            if (problem.empty())
                continue;
            first = file.vpath + ": " + problem;
            break;
        }
        Msg("~ [merge] %s: %s: %u file(s) of the delta do not agree with the format (%s%s): stale_delta, those files "
            "are merged without the delta, the merge may revert base changes", info.id.c_str(), out.file.c_str(),
            u32(out.inconsistent.size()), first.c_str(), out.inconsistent.size() > 1 ? ", ..." : "");
    }
    return out;
}

xr_map<xr_string, cm::AddonDelta> g_deltas; // the deltas of the addons of the plan, by id (read once per plan)

// The addons made for an older game ([addon] game_version older than configs\gw_release.ltx) without a usable delta:
// their replacements of the files of the base may revert changes of the base (15 section 2.2). One line per addon in
// the log (its files are not read for that: only the ones several addons replace are merged); the same addon and
// versions are told once per run
xr_vector<xr_string> g_guessed_addons; // of the last plan
xr_vector<xr_string> g_guessed_told; // "<id> <game_version> <tag>" told in this run

void MergeWarnOlderAddons(const xr_vector<xr_string>& guessed, const xr_vector<AddonInfo>& addons)
{
    g_guessed_addons = guessed;
    const xr_string& tag = GameReleaseTag();
    for (const xr_string& owner : guessed)
    {
        const auto info =
            std::find_if(addons.begin(), addons.end(), [&owner](const AddonInfo& a) { return a.id == owner; });
        if (info == addons.end())
            continue;
        const xr_string told = owner + " " + info->game_version + " " + tag;
        if (std::find(g_guessed_told.begin(), g_guessed_told.end(), told) != g_guessed_told.end())
            continue;
        g_guessed_told.push_back(told);
        // No delta at all, or a delta that is not used (not read, another version of the parse: MergeReadDelta told why)
        const auto found = g_deltas.find(owner);
        const bool has_file = found != g_deltas.end() && !found->second.file.empty();
        const xr_string why = has_file ?
            "its delta (" + found->second.file + ") is stale and not used" :
            xr_string("it has no delta (.gw_merge\\delta.json)");
        Msg("~ [merge] addon %s is made for %s, the game is %s, and %s: the files of the base it replaces may revert "
            "changes of the base (addon_merges)", owner.c_str(), info->game_version.c_str(), tag.c_str(), why.c_str());
    }
}

// The plan asks the file system of the engine (cm::PlanEnv): the base file - FS.r_open_base (the archives too), the
// patch files - IsPatchFile of ContentPatches over the index, the walk - CLocatorAPI::layer_walk_skips
bool MergeBase(void* /*context*/, std::string_view name, cm::String& out)
{
    IReader* reader = FS.r_open_base("$game_data$", xr_string(name.data(), name.size()).c_str());
    if (!reader)
        return false;
    out.assign(static_cast<const char*>(reader->pointer()), size_t(reader->length()));
    FS.r_close(reader);
    return true;
}

bool MergeIsPatchFile(void* /*context*/, std::string_view name)
{
    string_path full_name;
    FS.update_path(full_name, "$game_data$", xr_string(name.data(), name.size()).c_str());
    return xray::content_patches::IsPatchFile(full_name);
}

bool MergeWalkSkips(void* /*context*/, const char* physical) { return CLocatorAPI::layer_walk_skips(physical); }

// The files of the plan (PlanContentMerge): the records, the merge, the folder of the set and the layer
void PlanContentMergeFiles(xr_vector<ContentLayer>& desired)
{
    g_merges.clear();
    g_merge_folder.clear();

    // The inputs of the plan of the library (cm::PlanMerge - the decisions gw_merge plan makes the same way): the stack
    // after the rules of the player, the addons (load_after of the winners, the compatibility patches), the rules
    const xr_vector<AddonInfo> addons = ListAddons();
    cm::PlanInput input;
    for (const AddonInfo& info : addons)
        input.addons.push_back({ info.id, info.dir, info.active, info.load_after, info.patches, info.game_version });
    // S9: the deltas of the addons whose layers take part (read once per plan, 15 section 7.3)
    g_deltas.clear();
    for (const xr_string& owner : cm::PlanOwners(desired))
    {
        const auto info =
            std::find_if(addons.begin(), addons.end(), [&owner](const AddonInfo& a) { return a.id == owner; });
        if (info == addons.end())
            continue;
        cm::AddonDelta delta = MergeReadDelta(*info);
        if (delta.file.empty())
            continue;
        g_deltas.emplace(owner, std::move(delta));
    }
    for (const cm::PlanAddon& addon : input.addons)
    {
        const auto found = g_deltas.find(addon.id);
        input.deltas.push_back(found == g_deltas.end() ? nullptr : &found->second);
    }
    input.layers = desired;
    for (const FileRule& rule : FileRules())
        input.file_rules.push_back({ rule.file, rule.winner });
    for (const AtomRule& rule : AtomRules())
        input.atom_rules.push_back({ rule.file, rule.atom, rule.winner });
    input.game_tag = GameReleaseTag();
    cm::PlanEnv env;
    env.base = &MergeBase;
    env.is_patch_file = &MergeIsPatchFile;
    env.walk.skips = &MergeWalkSkips;
#if defined(XR_PLATFORM_WINDOWS)
    env.walk.lower_case = true; // the index spells the names in lower case (xr_fs_strlwr)
#endif
    // The layers lie over $game_data$ in the index: a name too long there is not mounted (cm::WalkNameTooLong)
    string_path game_data;
    FS.update_path(game_data, "$game_data$", "");
    env.walk.index_root = xr_strlen(game_data);
    if (env.walk.index_root && game_data[env.walk.index_root - 1] != _DELIMITER)
        ++env.walk.index_root; // the trailing delimiter of the root of a layer

    CTimer timer;
    timer.Start();
    cm::PlanResult plan;
    cm::PlanMerge(input, env, plan);
    // Made for an older game without a delta: one warning per addon, its files are not read for it (15 section 2.2)
    MergeWarnOlderAddons(plan.guessed_addons, addons);
    if (plan.entries.empty())
        return; // nothing to merge

    const xr_string root = MergeRoot();
    const xr_string cache = root + kMergeCacheFolder + DELIMITER;
    u32 from_cache = 0, merged_now = 0;
    for (cm::PlanEntry& entry : plan.entries)
    {
        for (const cm::String& line : entry.log)
            Msg("%s", line.c_str());
        ContentMergeFile& item = g_merges.emplace_back(std::move(entry.item));
        if (!entry.merge)
        {
            MergeLog(item); // not merged: why
            continue;
        }
        MergeOne(item, entry.job, cache);
        // "order" of the library told the way the conflicts of the files are (the set of a provider, pair, load_after)
        cm::RefineResolved(item, input.addons, ContentRuleStatuses());
        cm::CountPatches(item);
        item.served = cm::Serves(item);
        (item.cached ? from_cache : merged_now) += 1;
        MergeLog(item);
        MergeLogRebase(item);
        MergeLogPatches(item);
    }

    // 3. The set: the files the layer serves. The format of the results is a part of the list: a key names the inputs
    // only, so after a change of the merge of the engine (kMergeMetaFormat goes up, the results are made again and
    // replace the texts of cache\) a set of the same keys would still hold the old texts (hard links, copies)
    xr_string listing = xr_string("[merge]\n; ") + kMergeMetaFormat + "\n";
    xr_string set_key = xr_string(kMergeMetaFormat) + "\n";
    for (const ContentMergeFile& item : g_merges)
    {
        if (!item.served)
            continue;
        listing += item.file + " = " + item.key + "\n";
        set_key += item.file + "=" + item.key + "\n";
    }
    u32 served = 0;
    if (std::any_of(g_merges.begin(), g_merges.end(), [](const ContentMergeFile& item) { return item.served; }))
    {
        xr_string set_folder;
        xr_string error;
        if (MergeWriteSet(root, MergeSha1(set_key).substr(0, kMergeSetNameLength), listing, set_folder, error))
        {
            ContentLayer layer;
            layer.layer_id = kMergedLayerId;
            layer.folder = set_folder + "gamedata" + DELIMITER;
            layer.merged = true;
            // Over the sets of the providers, under the location pack and its writes (13 4.5, 15 4.1)
            const auto pack = std::find_if(desired.begin(), desired.end(),
                [](const ContentLayer& item) { return !item.group.empty(); });
            desired.insert(pack, layer);
            g_merge_folder = layer.folder;
            served = u32(std::count_if(g_merges.begin(), g_merges.end(),
                [](const ContentMergeFile& item) { return item.served; }));
        }
        else
        {
            Msg("! [merge] the folder of the merged files cannot be written (%s): the upper addons serve the files",
                error.c_str());
            for (ContentMergeFile& item : g_merges)
            {
                if (!item.served)
                    continue;
                item.served = false;
                item.status = "fallback";
                item.note = "write_error";
                item.error = error;
            }
        }
    }
    Msg("* [merge] %u file(s): %u from cache, %u merged now, %u served by the layer %s, %.1f ms", u32(g_merges.size()),
        from_cache, merged_now, served, kMergedLayerId, timer.GetElapsed_sec() * 1000.f);
}
} // namespace

// configs\gw_release.ltx of the base (the pipeline of the release writes it, 15 section 2.2):
//   [release]
//   tag = v1.4.0      ; the release tag of the game, the --tag of its snapshot (tools/addons_snapshot)
// Read from the base (layer 0): an addon cannot change the version of the game
namespace
{
bool g_release_read = false;
xr_string g_release_tag;

// Read at each plan (one small file): the log tells the tag when it changes
void ReadGameReleaseTag()
{
    const xr_string before = g_release_tag;
    const bool first = !g_release_read;
    g_release_read = true;
    g_release_tag.clear();
    xr_string& tag = g_release_tag;
    IReader* reader = FS.r_open_base("$game_config$", "gw_release.ltx");
    if (!reader)
    {
        if (!first && !before.empty())
            Msg("* [merge] configs\\gw_release.ltx is gone: the version of the game is unknown");
        return;
    }
    const xr_string data(static_cast<const char*>(reader->pointer()), size_t(reader->length()));
    FS.r_close(reader);
    tag = cm::ReleaseTagOf(data); // the rule of gw_merge plan too
    if (!first && tag == before)
        return;
    if (tag.empty())
        Msg("~ [merge] configs\\gw_release.ltx has no [release] tag: the version of the game is unknown");
    else
        Msg("* [merge] the version of the game (configs\\gw_release.ltx): %s", tag.c_str());
}
} // namespace

const xr_string& GameReleaseTag()
{
    if (!g_release_read)
        ReadGameReleaseTag();
    return g_release_tag;
}

bool ReleaseTagOlder(const xr_string& game_version, const xr_string& tag)
{
    return cm::ReleaseTagOlder(game_version, tag); // xrCore ContentMerge/Plan.h: the rule of gw_merge plan too
}

void PlanContentMerge(xr_vector<ContentLayer>& desired)
{
    ReadGameReleaseTag();
    PlanContentMergeFiles(desired);
    MergeRuleStatuses();
}

const xr_vector<ContentRuleStatus>& ContentMergeRuleStatuses() { return g_atom_rule_statuses; }

void OnMergedLayerFailed()
{
    for (ContentMergeFile& item : g_merges)
    {
        if (!item.served)
            continue;
        item.served = false;
        item.status = "fallback";
        item.note = "layer_not_mounted";
        item.error = "the layer " + xr_string(kMergedLayerId) + " is not mounted";
    }
    g_merge_folder.clear();
    MergeRuleStatuses(); // the atom: rules are not applied now
}

namespace
{
// The kind of a folder of <root>: a set ("<hex>" of the old or the new length, "<hex>-<n>"), a temporary folder of a
// set ("<hex>[-<n>].tmp<pid>_<n>"), or something else (not touched)
enum class MergeFolderKind
{
    Other,
    Set,
    Temporary,
};

MergeFolderKind MergeFolderKindOf(const xr_string& leaf)
{
    size_t hex = 0;
    while (hex < leaf.size() && ((leaf[hex] >= '0' && leaf[hex] <= '9') || (leaf[hex] >= 'a' && leaf[hex] <= 'f')))
        ++hex;
    if (hex != kMergeSetNameLength && hex != kMergeOldSetNameLength)
        return MergeFolderKind::Other;
    size_t at = hex;
    if (at < leaf.size() && leaf[at] == '-')
    {
        const size_t digits = ++at;
        while (at < leaf.size() && leaf[at] >= '0' && leaf[at] <= '9')
            ++at;
        if (at == digits)
            return MergeFolderKind::Other;
    }
    if (at == leaf.size())
        return MergeFolderKind::Set;
    return leaf.compare(at, 4, ".tmp") == 0 ? MergeFolderKind::Temporary : MergeFolderKind::Other;
}

} // namespace

void CleanContentMergeFolders()
{
    const std::filesystem::path root = PathFromUtf8(MergeRoot());
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec))
        return;
    // The folders of the sets: the mounted one stays (its use is marked now); another one goes only when nobody used
    // it for kMergeSetDays - another instance of the game sharing the appdata may have it mounted (it marks the use
    // of its set at each build); a temporary folder of a build that did not finish (a crash) - after kMergeTempDays
    xr_string keep;
    for (const ContentLayer& layer : MountedContentLayers())
    {
        if (layer.merged)
            keep = layer.folder;
    }
    const auto now = std::filesystem::file_time_type::clock::now();
    const auto set_limit = now - std::chrono::hours(24 * kMergeSetDays);
    const auto temporary_limit = now - std::chrono::hours(24 * kMergeTempDays);
    xr_vector<std::filesystem::path> stale;
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
    {
        const xr_string leaf = PathToUtf8(it->path().filename());
        std::error_code type_error;
        const MergeFolderKind kind = MergeFolderKindOf(leaf);
        if (kind == MergeFolderKind::Other || !it->is_directory(type_error))
            continue;
        const xr_string gamedata = PathToUtf8(it->path()) + DELIMITER + "gamedata" + DELIMITER;
        if (!keep.empty() && MergeIndexName(gamedata) == MergeIndexName(keep))
        {
            MergeTouch(it->path() / kMergeSetListing);
            continue;
        }
        // The last use: merge.ltx of a set (MergeWriteSet marks it), the folder itself otherwise
        std::error_code time_error;
        std::filesystem::file_time_type used = std::filesystem::last_write_time(it->path(), time_error);
        if (kind == MergeFolderKind::Set)
        {
            std::error_code listing_error;
            const auto listed = std::filesystem::last_write_time(it->path() / kMergeSetListing, listing_error);
            if (!listing_error)
                used = listed;
        }
        if (!time_error && used < (kind == MergeFolderKind::Set ? set_limit : temporary_limit))
            stale.push_back(it->path());
    }
    for (const std::filesystem::path& path : stale)
    {
        std::error_code remove_error;
        std::filesystem::remove_all(path, remove_error); // a file held open: next time
    }

    // The cache: over kMergeCacheKeep results, those older than kMergeCacheDays go
    struct Entry
    {
        std::filesystem::path meta;
        std::filesystem::file_time_type time;
    };
    xr_vector<Entry> entries;
    const std::filesystem::path cache = root / kMergeCacheFolder;
    for (std::filesystem::directory_iterator it(cache, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code time_error;
        const xr_string extension = PathToUtf8(it->path().extension());
        if (extension == ".meta")
            entries.push_back({ it->path(), it->last_write_time(time_error) });
        else if (extension.compare(0, 4, ".tmp") == 0 && it->last_write_time(time_error) < temporary_limit &&
            !time_error)
        {
            std::error_code remove_error;
            std::filesystem::remove(it->path(), remove_error); // a write that did not finish (a crash)
        }
    }
    if (entries.size() <= kMergeCacheKeep)
        return;
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.time < b.time; });
    const auto limit = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * kMergeCacheDays);
    size_t left = entries.size();
    for (const Entry& entry : entries)
    {
        if (left <= kMergeCacheKeep || entry.time >= limit)
            break;
        std::error_code remove_error;
        std::filesystem::path text = entry.meta;
        text.replace_extension(".txt");
        std::filesystem::remove(entry.meta, remove_error);
        std::filesystem::remove(text, remove_error);
        --left;
    }
}

const xr_vector<ContentMergeFile>& ContentMerges() { return g_merges; }

const ContentMergeFile* FindContentMerge(const xr_string& file)
{
    const auto it = std::find_if(g_merges.begin(), g_merges.end(),
        [&file](const ContentMergeFile& item) { return item.file == file; });
    return it == g_merges.end() ? nullptr : &*it;
}

ContentMergeSummary ContentMergesSummary()
{
    ContentMergeSummary summary;
    summary.files = u32(g_merges.size());
    for (const ContentMergeFile& item : g_merges)
    {
        if (item.served)
        {
            ++summary.merged;
            summary.unresolved += item.unresolved;
        }
        else if (item.status == "fallback")
        {
            ++summary.fallback;
            ++summary.unresolved;
        }
        summary.patch_stale += u32(std::count_if(item.patches.begin(), item.patches.end(),
            [](const ContentMergeFile::Patch& patch) { return patch.status == "stale"; }));
        for (const xr_string& ancestor : item.ancestors)
        {
            summary.rebased += ancestor.compare(0, 6, "delta:") == 0;
            summary.stale_delta += ancestor == "stale_delta";
            summary.guessed += ancestor == "current_base_guessed";
        }
    }
    summary.guessed_addons = u32(g_guessed_addons.size());
    summary.strict = AddonStrictMode();
    return summary;
}

void LogContentMerges()
{
    if (g_merges.empty())
        return;
    const ContentMergeSummary summary = ContentMergesSummary();
    Msg("%s [merge] %u file(s) several addons replace (or one with its delta or an atom: rule): %u served merged "
        "(layer %s), %u fallback, %u unresolved conflict(s) of atoms, of the rebase and fallback(s), %u stale "
        "patch(es); sides: %u carried over by deltas, %u stale_delta, %u current_base_guessed; %u addon(s) made for an "
        "older game without a delta (console addon_merges)%s",
        summary.unresolved || summary.stale_delta || summary.guessed || summary.guessed_addons ? "~" : "*",
        summary.files, summary.merged, kMergedLayerId, summary.fallback, summary.unresolved, summary.patch_stale,
        summary.rebased, summary.stale_delta, summary.guessed, summary.guessed_addons,
        !summary.unresolved ? "" : summary.strict ? "; -addon_strict: no level starts until they are resolved" :
                                                    "; the game goes on the winners (-addon_strict forbids it)");
}

void PrintContentMerges(pcstr addon_id)
{
    const bool all = !addon_id || !addon_id[0];
    u32 shown = 0;
    for (const ContentMergeFile& item : g_merges)
    {
        if (!all && std::find(item.addons.begin(), item.addons.end(), addon_id) == item.addons.end())
            continue;
        ++shown;
        Msg("- %s [%s] %s%s%s%s: addons %s; changed %s; %u atom(s), %u adjacent, %u conflict(s)%s%s",
            item.file.c_str(), item.kind.c_str(), item.status.c_str(), item.note.empty() ? "" : " (",
            item.note.c_str(), item.note.empty() ? "" : ")", MergeJoin(item.addons).c_str(),
            item.changed.empty() ? "-" : MergeJoin(item.changed).c_str(), item.atoms_merged, item.adjacent,
            u32(item.conflicts.size()), item.served ? ", served by " : "", item.served ? kMergedLayerId : "");
        for (const ContentMergeFile::Conflict& c : item.conflicts)
        {
            Msg("-   conflict %s addons: %s winner: %s (%s%s), lost: %s", c.atom.c_str(), MergeJoin(c.addons).c_str(),
                c.winner.c_str(), c.resolved_by.c_str(), c.stale ? ", patch_stale" : "", MergeJoin(c.lost).c_str());
        }
        for (const ContentMergeFile::Patch& patch : item.patches)
        {
            Msg("-   patch %s (for %s): %s%s%s", patch.id.c_str(), MergeJoin(patch.targets).c_str(),
                patch.status.c_str(), patch.note.empty() ? "" : ", ", patch.note.c_str());
        }
        for (size_t i = 0; i < item.ancestors.size() && i < item.addons.size(); ++i)
        {
            if (item.ancestors[i] != "base")
                Msg("-   ancestor of %s: %s", item.addons[i].c_str(), item.ancestors[i].c_str());
        }
        for (const ContentMergeFile::RebaseConflict& c : item.rebase_conflicts)
        {
            Msg("-   rebase conflict %s addon: %s (%s)%s%s, winner: %s (%s)", c.atom.c_str(), c.addon.c_str(),
                c.reason.c_str(), c.note.empty() ? "" : ": ", c.note.c_str(), c.winner.c_str(), c.resolved_by.c_str());
        }
        if (!item.error.empty())
            Msg("-   error: %s", item.error.c_str());
    }
    const ContentMergeSummary summary = ContentMergesSummary();
    Msg("- [merge] %u file(s)%s%s; all: %u served merged, %u fallback, %u unresolved, %u stale patch(es)%s "
        "(addon_merge_show <file> - the atoms and the lines; addon_atom_winner <file>|<atom> <id | core> - the winner "
        "of an atom; addon_merge_rebuild - merge again)", shown, all ? "" : " of the addon ", all ? "" : addon_id,
        summary.merged, summary.fallback, summary.unresolved, summary.patch_stale,
        summary.strict ? ", -addon_strict" : "");
}

void PrintContentMerge(pcstr file)
{
    xr_string name;
    const ContentMergeFile* item = file && NormalizeRuleFile(file, name) ? FindContentMerge(name) : nullptr;
    if (!item)
    {
        Msg("- addon_merge_show <file in gamedata\\>: no merge of '%s' (addon_merges lists them)", file ? file : "");
        return;
    }
    Msg("- [merge] %s [%s]: %s%s%s%s%s", item->file.c_str(), item->kind.c_str(), item->status.c_str(),
        item->note.empty() ? "" : " (", item->note.c_str(), item->note.empty() ? "" : ")",
        item->cached ? ", from the cache" : "");
    for (size_t i = 0; i < item->addons.size(); ++i)
    {
        const bool changed = std::find(item->changed.begin(), item->changed.end(), item->addons[i]) !=
            item->changed.end();
        Msg("-   side %u: %s (layer %s, ancestor %s)%s", u32(i + 1), item->addons[i].c_str(), item->layers[i].c_str(),
            i < item->ancestors.size() ? item->ancestors[i].c_str() : "base",
            changed ? "" : " - the same as the base");
    }
    Msg("-   %u atom(s) taken from the addons, %u adjacent change(s)%s%s", item->atoms_merged, item->adjacent,
        item->adjacent_atoms.empty() ? "" : ": ", MergeJoin(item->adjacent_atoms).c_str());
    if (!item->reordered.empty())
        Msg("-   reordered by an addon (the order of the base is kept): %s", MergeJoin(item->reordered).c_str());
    for (const ContentMergeFile::Conflict& c : item->conflicts)
    {
        Msg("-   conflict %s (line %u of the base) addons: %s winner: %s (%s%s), lost: %s", c.atom.c_str(), c.base_line,
            MergeJoin(c.addons).c_str(), c.winner.c_str(), c.resolved_by.c_str(), c.stale ? ", patch_stale" : "",
            MergeJoin(c.lost).c_str());
    }
    for (const ContentMergeFile::Patch& patch : item->patches)
    {
        Msg("-   patch %s (for %s): %s, %u conflict(s) decided%s%s%s%s", patch.id.c_str(),
            MergeJoin(patch.targets).c_str(), patch.status.c_str(), patch.resolved,
            patch.stale_targets.empty() ? "" : ", other versions of: ",
            MergeJoin(patch.stale_targets).c_str(), patch.note.empty() ? "" : "; ", patch.note.c_str());
    }
    for (const ContentMergeFile::RebaseConflict& c : item->rebase_conflicts)
    {
        Msg("-   rebase conflict %s addon: %s (%s)%s%s, winner: %s (%s)", c.atom.c_str(), c.addon.c_str(),
            c.reason.c_str(), c.note.empty() ? "" : ": ", c.note.c_str(), c.winner.c_str(), c.resolved_by.c_str());
    }
    if (!item->rules_applied.empty())
        Msg("-   decided by atom: rules of the player: %s", MergeJoin(item->rules_applied).c_str());
    if (!item->error.empty())
        Msg("-   error: %s", item->error.c_str());
    for (const ContentMergeFile::Lines& l : item->lines)
        Msg("-   lines %u-%u: %s", l.from, l.to, l.owner.c_str());
    if (item->served && !g_merge_folder.empty())
        Msg("-   the file: [%s%s], cache key %s", g_merge_folder.c_str(), item->file.c_str(), item->key.c_str());
    else if (!item->key.empty())
        Msg("-   cache key %s", item->key.c_str());
}

void RebuildContentMerges()
{
    const std::filesystem::path cache = PathFromUtf8(MergeRoot() + kMergeCacheFolder);
    std::error_code ec;
    std::filesystem::remove_all(cache, ec);
    if (ec)
        Msg("! addon_merge_rebuild: cannot remove [%s]: %s", PathToUtf8(cache).c_str(), ErrorText(ec).c_str());
    if (slots::RequestRebuild(""))
        Msg("- [merge] the cache is cleared: the content is built again at the next frame of the main menu");
}

bool AddonStrictMode() { return Core.Params && std::strstr(Core.Params, "-addon_strict") != nullptr; }

bool StrictMergeBlocks() { return AddonStrictMode() && ContentMergesSummary().unresolved > 0; }

void LogStrictMergeBlock(pcstr what)
{
    const ContentMergeSummary summary = ContentMergesSummary();
    Msg("! [merge] -addon_strict: %s is refused: %u unresolved conflict(s) of the merge of the addons. Choose the "
        "winner (console addon_atom_winner <file>|<atom> <id | core>), switch an addon off or start without "
        "-addon_strict (console addon_merges)", what ? what : "the level", summary.unresolved);
    constexpr u32 kLines = 20;
    u32 lines = 0;
    for (const ContentMergeFile& item : g_merges)
    {
        if (item.status == "fallback" && lines++ < kLines)
            Msg("!   %s: not merged (%s): %s", item.file.c_str(), item.note.c_str(), item.error.c_str());
        if (!item.served)
            continue;
        for (const ContentMergeFile::RebaseConflict& c : item.rebase_conflicts)
        {
            if (c.resolved_by == "rebase" && lines++ < kLines)
                Msg("!   %s|%s rebase conflict of %s (%s)", item.file.c_str(), c.atom.c_str(), c.addon.c_str(),
                    c.reason.c_str());
        }
        for (const ContentMergeFile::Conflict& c : item.conflicts)
        {
            if (MergeConflictOpen(c) && lines++ < kLines)
                Msg("!   %s|%s addons: %s winner: %s (%s%s)", item.file.c_str(), c.atom.c_str(),
                    MergeJoin(c.addons).c_str(), c.winner.c_str(), c.resolved_by.c_str(),
                    c.stale ? ", patch_stale" : "");
        }
    }
    if (lines > kLines)
        Msg("!   ... %u more (console addon_merges)", lines - kLines);
}

void PushContentMergeSummary(::lua_State* L)
{
    const ContentMergeSummary summary = ContentMergesSummary();
    lua_createtable(L, 0, 13);
    const auto number = [L](pcstr name, u32 value) {
        lua_pushinteger(L, lua_Integer(value));
        lua_setfield(L, -2, name);
    };
    number("files", summary.files);
    number("merged", summary.merged);
    number("fallback", summary.fallback);
    number("unresolved", summary.unresolved);
    number("patch_stale", summary.patch_stale);
    number("rebased", summary.rebased);
    number("stale_delta", summary.stale_delta);
    number("guessed", summary.guessed);
    number("guessed_addons", summary.guessed_addons);
    lua_pushstring(L, cm::kAlgorithmVersion);
    lua_setfield(L, -2, "algo");
    lua_pushstring(L, GameReleaseTag().c_str());
    lua_setfield(L, -2, "game");
    lua_pushboolean(L, summary.strict ? 1 : 0);
    lua_setfield(L, -2, "strict");
    lua_pushboolean(L, summary.strict && summary.unresolved ? 1 : 0);
    lua_setfield(L, -2, "blocked");
}

void DescribeMergedLuaError(pcstr message)
{
    if (!message || !message[0] || g_merge_folder.empty())
        return;
    // The chunk name of a script is its path in the game data ("@<...>\gamedata\scripts\x.script", do_file); LuaJIT
    // cuts a long one to "...<the tail>" and a path may have blanks: the tail before "<leaf>:<line>" is compared both
    // ways with "\gamedata\<file>"
    const xr_string text = MergeIndexName(message);
    xr_vector<xr_string> told;
    for (const ContentMergeFile& item : g_merges)
    {
        if (!item.served || item.kind != "lua")
            continue;
        const size_t slash = item.file.find_last_of('\\');
        const xr_string leaf = (slash == xr_string::npos ? item.file : item.file.substr(slash + 1)) + ":";
        const xr_string full = "\\gamedata\\" + item.file;
        for (size_t at = text.find(leaf); at != xr_string::npos; at = text.find(leaf, at + 1))
        {
            size_t end = at + leaf.size();
            u32 line = 0;
            while (end < text.size() && text[end] >= '0' && text[end] <= '9')
                line = line * 10 + u32(text[end++] - '0');
            if (!line || end == at + leaf.size())
                continue;
            size_t start = at;
            while (start > 0 && !std::strchr(" \t\n'\"(<[", text[start - 1]))
                --start;
            xr_string tail = text.substr(start, at + leaf.size() - 1 - start);
            if (tail.compare(0, 3, "...") == 0)
                tail.erase(0, 3);
            if (tail.compare(0, 1, "@") == 0)
                tail.erase(0, 1);
            const bool same = tail == item.file || (tail.size() <= full.size() &&
                full.compare(full.size() - tail.size(), tail.size(), tail) == 0) ||
                (tail.size() > item.file.size() && tail.compare(tail.size() - item.file.size() - 1,
                    item.file.size() + 1, "\\" + item.file) == 0);
            if (!same)
                continue;
            const xr_string where = item.file + ":" + xr_string(std::to_string(line).c_str());
            if (std::find(told.begin(), told.end(), where) != told.end())
                continue;
            told.push_back(where);
            const auto piece = std::find_if(item.lines.begin(), item.lines.end(),
                [line](const ContentMergeFile::Lines& l) { return l.from <= line && line <= l.to; });
            if (piece == item.lines.end())
                continue;
            Msg("~ [merge] %s comes from %s (the file is merged: addon_merge_show %s)", where.c_str(),
                piece->owner == cm::kBaseOwner ? "the base (core)" : piece->owner.c_str(), item.file.c_str());
        }
    }
}
} // namespace gw::addons
