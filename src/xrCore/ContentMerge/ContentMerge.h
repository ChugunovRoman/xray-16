#pragma once

// Merge of the replacements of one text file by several addons (plans/addons_api_support/15-merge-implementation.md,
// 13-extension-model.md section 4.6). Two addons or more replaced one file of the base (.ltx, .xml, .script) as a
// whole: the changes each made against the base (the ancestor) are merged by atoms - a key of an ltx section, a node
// and an attribute of an xml, a top level statement of a Lua script (a function first of all). Inside a function or
// a multi-line value two sides changed - a three-way merge by lines.
//
// The library depends on the standard library only: the engine (xrCore), the command line tool gw_merge (catalog CI,
// the launcher) and the tests build the same sources, so they give the same result (15 section 7.1).
//   CONTENT_MERGE_STANDALONE - built outside the engine: String / Vector are std::string / std::vector;
//   otherwise (xrCore) they are xr_string / xr_vector: the results cross the boundary of xrCore.dll, the memory of
//   std:: containers must not (xrCore has its own operator new).

#include <cstddef>
#include <cstdint>
#include <string_view>

#if defined(CONTENT_MERGE_STANDALONE)
#include <string>
#include <vector>
#define CONTENT_MERGE_API
namespace xray::content_merge
{
using String = std::string;
template <typename T>
using Vector = std::vector<T>;
} // namespace xray::content_merge
#else
// The sources are compiled without the precompiled header of xrCore (they are the ones of gw_merge too)
#include "Common/Common.hpp"
#include "xrCore/xrCore.h"
#define CONTENT_MERGE_API XRCORE_API
namespace xray::content_merge
{
using String = xr_string;
template <typename T>
using Vector = xr_vector<T>;
} // namespace xray::content_merge
#endif

namespace xray::content_merge
{
// The version of the parse into atoms: the compatibility of the deltas (15 section 7.2). A change of the segments, of
// their keys or labels, of BuildDelta or of the way ApplyDelta reads a delta changes it - after a release of the
// version every delta of the catalog is recomputed (files/<id>/<ver>.merge.<algo>.json), so it goes up only for such
// a change. It is a part of the cache key of the engine and of the reports of CI (15 section 4.3).
//   gw-merge/2: an ltx value compared as the engine reads it (_parse: the spaces outside the quotes dropped); the Lua
//   code between the atoms split, joined or moved by the addon - a delta by lines (BuildDelta); the work limit of the
//   diff (a conflict over it)
constexpr const char* kAlgorithmVersion = "gw-merge/2";
// The revision of the results of the library within kAlgorithmVersion: any change that changes a result of Merge,
// ApplyDelta or Validate (a text, a conflict, a status, a label in the report) without changing the deltas goes up
// here, not in kAlgorithmVersion (the catalog recomputes nothing for it). A part of the cache key of the engine
// (addon_merge.cpp MergeKeyOf) - a result of an older library is never served from the cache - and of the caches of
// the tools (gw_merge version prints it). A change of kAlgorithmVersion starts it from 1 again.
//   1: the release candidate of gw-merge/2 (decisions 56, 57: the line moved by two sides, StatementKey)
//   2: the rule of the top-level code (review 2026-11-06 section 4.1 item 2, decision 63): the Lua code between the
//      atoms changed by two sides or more differently (the addon and the base in a rebase) is a conflict of its
//      region as a whole - no merge of it by lines, no search of moved lines and calls; the same text of several sides
//      folded once in the diff3 of an atom (it repeated an insertion). The deltas are the same
//   3: decision 64 items 5-6 (2026-11-07): the rule of the top-level code in ApplyDelta only for the code between the
//      atoms of a Lua script - a gap of an ltx / xml list the base and the addon both changed is merged by diff3
//      again (as in revision 1); diff3 (Merge3 with merge_adjacent) - a touching change of one side that is a part
//      of the change of the other one (inserts at its edge the lines it puts there) folded into it, touching changes
//      that repeat a line otherwise - a conflict (both were kept: the lines twice, no conflict). The deltas are the
//      same
constexpr int kResultRevision = 3;
// The owner of the base in the reports and the rules ("atom:<file>|<atom> = core")
constexpr const char* kBaseOwner = "core";

enum class Kind : std::uint8_t
{
    Unknown,
    Ltx,
    Xml,
    Lua,
};

// ---- The files the merge takes: the reference for every implementation (the engine, gw_merge, the launcher, the
// validator of the catalog, tools/addons_snapshot) - 15 sections 1.1, 4.2 ------------------------------------------
// A text file of the merge: KindOfName(name) is not Unknown, that is the extension after the last '.' of the name is
// ltx (Ltx), xml (Xml), script or lua (Lua) in any ASCII case; any other name (no '.', "x.ltx.bak") is not merged.
// It is not merged either when:
//   - it is a patch file (IsPatchFile below): each one is applied in the order of the layers;
//   - the file system does not mount it (CLocatorAPI::layer_walk_skips: hidden on Windows, a name starting with '.'
//     on POSIX, ignore_name; a folder "@..." of an addon layer; a [private] file of the addon).
// .ltx / .xml / .script / .lua (the ASCII case is ignored), Unknown for any other name
CONTENT_MERGE_API Kind KindOfName(std::string_view name);

// Whether the game has a file (`vpath` relative to gamedata\, '\', in ASCII lower case - as IsPatchFile asks it): the
// base or any layer of an addon. The context is the one of the caller
struct FileExists
{
    bool (*exists)(void* context, std::string_view vpath){};
    void* context{};
};

// A patch file (DLTX mod_<stem>_*.ltx, XMLOverride mod_<stem>_*.xml; xrCore/ContentPatches.h IsPatchFile is the same
// rule over the index of the file system): `vpath` (relative to gamedata\, '\' or '/', any case) is one when
//   - it lies under configs\ (at any depth);
//   - its leaf is "mod_<rest>.<ext>" ("mod_" in any ASCII case, <ext> - after the last '.');
//   - its folder has a file "<stem>.<ext>" (asked through `exists`, in lower case) whose leaf does not start
//     with "mod_", and <rest> is "<stem>_<x>" with a non-empty <x> (the longest such stem is the target; one is
//     enough).
// A mod_*.ltx without such a target ("mod_x.ltx", a file of the configs\ of its own) is an ordinary file.
CONTENT_MERGE_API bool IsPatchFile(std::string_view vpath, const FileExists& exists);
// "ltx" / "xml" / "lua" / "unknown"; KindOfWord takes these words and "script"
CONTENT_MERGE_API const char* KindName(Kind kind);
CONTENT_MERGE_API Kind KindOfWord(std::string_view word);

// ---- Text normalization (TextNorm.cpp, the port of tools/addons_snapshot/textnorm.py) ----------------------------

enum class Encoding : std::uint8_t
{
    Ascii,
    Utf8,
    Cp1251,
    Cp1251Lossy, // a byte undefined even in cp1251 (0x98) became U+FFFD
};

enum class Eol : std::uint8_t
{
    None,
    Lf,
    Crlf,
    Mixed,
};

struct NormInfo
{
    Encoding enc{Encoding::Ascii};
    bool bom{};
    Eol eol{Eol::None};
};

CONTENT_MERGE_API const char* EncodingName(Encoding enc); // as textnorm.py: "ascii", "utf-8", "cp1251", "cp1251-lossy"
CONTENT_MERGE_API const char* EolName(Eol eol); // "lf", "crlf", "mixed", "none"

// The bytes of a file -> UTF-8 text: strict UTF-8, else cp1251; the leading BOM removed; CRLF -> LF (a lone CR kept).
// The number of lines does not change.
CONTENT_MERGE_API String Normalize(std::string_view bytes, NormInfo* info = nullptr);
// The normalized text back in the form of a file (the drafts of an author, 15 section 3.1): the encoding (cp1251 if
// the file was cp1251 and every char fits), the BOM, CRLF if the file had CRLF line ends
CONTENT_MERGE_API String Denormalize(std::string_view text, const NormInfo& like);
// An xml text: encoding="windows-1251" of the declaration -> "utf-8" (the merged file is UTF-8)
CONTENT_MERGE_API String XmlDeclarationToUtf8(std::string_view text);

// ---- Merge ---------------------------------------------------------------------------------------------------------

enum class Role : std::uint8_t
{
    Addon, // the whole file of an addon: an atom it does not have is deleted
    Patch, // a fragment of a compatibility patch (15 section 6.1): an atom it does not have is not touched
};

// The atoms of a version against an ancestor (gw_merge atoms, the catalog delta, 15 section 7.2)
enum class AtomOp : std::uint8_t
{
    Add,
    Modify,
    Delete,
};

// ---- S9: the delta of an addon of the catalog (Delta.cpp; 15 sections 2.3-2.4, 7.2) ---------------------------------
// For every atom the addon changed against the base it was made on (the ancestor): the text of the atom in the
// ancestor and the new one. The texts of the base are not downloaded (00-roadmap.md 5a, decision 48): the atoms the
// delta does not have are taken from the current base when the delta is carried over onto it (ApplyDelta).

constexpr std::int64_t kDeltaFormat = 1; // the version of the schema of the delta ("format")

struct DeltaAtom
{
    String atom; // the label, as the reports and the atom: rules name it; a changed gap - "@gap <label>"
    AtomOp op{AtomOp::Modify};
    // The keys of the segments from the top of the document down to the atom (the keys of the parser: "[sec]", "k2";
    // "w#1", "btn[id=x]", "@x"; "f", "local f"; a gap - "~<the key of the atom above>#<n>")
    Vector<String> path;
    String after; // the label of the atom above it in the file of the addon ("" - none): for the reader
    // The key of the sibling it goes after when the current base lacks the atom ("^" - the start of the list)
    String anchor;
    // Only the comments or the spaces changed: applied only where the base has the text of the ancestor as is, never
    // a conflict (otherwise the formatting of an addon would undo a change of the base)
    bool format{};
    String ancestor; // Modify, Delete: the text of the atom in the ancestor, normalized (UTF-8, LF), as in the file
    String text; // Modify, Add: the text of the atom in the file of the addon
};

struct DeltaFile
{
    String vpath; // relative to gamedata\, lower case, '\' (the key of "files")
    // "ltx" | "xml" | "lua"; "lines" - the file as one atom "@lines" (the self-check of the atoms failed: the
    // carrying over is diff3 by lines with the whole ancestor)
    String kind;
    String ancestor_sha256; // of the normalized ancestor (= texts.files[vpath].sha256 of the snapshot of the tag)
    String addon_sha256; // of the normalized file of the addon: the file was not edited after the publication
    bool same_as_ancestor{}; // the addon did not change the file
    bool not_in_ancestor{}; // the base of the author has no such file (the current one has): no atoms, S8
    bool final_eol{true}; // false - the file of the addon has no line end at its end
    String note; // why "lines"
    Vector<DeltaAtom> atoms;
};

struct Delta
{
    std::int64_t format{kDeltaFormat};
    String algo{kAlgorithmVersion}; // the version of the parse into atoms that made the delta
    String game; // the release tag of the base of the author ([addon] game_version)
    bool guessed{}; // the tag is a guess of CI ([addon] game_version is not set)
    Vector<DeltaFile> files;
};

struct Side
{
    String owner; // the addon id
    Role role{Role::Addon};
    String text; // the bytes of the file (normalized inside)
    // S9 (15 section 2.4): the base the addon was made on; set - the side is rebased onto the current base first
    bool has_ancestor{};
    String ancestor;
    // Role::Patch: the addons the patch is made for ([addon] patches): it resolves a conflict of these only
    Vector<String> patches;
    // S9, the delta of the catalog (15 section 2.4, step 1): set - the side is the delta carried over onto the
    // current base (ApplyDelta), `text` is not read then (its sha256 is checked by the caller: DeltaMatchesFile)
    bool has_delta{};
    DeltaFile delta;
};

// "atom:<file>|<atom> = <owner | core>" of the player (15 section 4.4)
struct AtomRule
{
    String atom; // the atom label as the report names it
    String owner; // an owner of a side or kBaseOwner
};

// Checks the merged text of a Lua script by compiling it (luaL_loadbuffer in a separate lua_State), set by the one
// who links LuaJIT: the library does not. Returns true when the chunk compiles, else false and the message.
struct LuaChecker
{
    bool (*compile)(void* context, const char* text, std::size_t size, const char* chunk_name, String& error){};
    void* context{};
};

struct Options
{
    bool merge_adjacent{true}; // adjacent changes of two sides inside an atom are merged (else - a conflict)
    bool validate{true}; // check the merged text (Validate); invalid - status Fallback
    bool markers{}; // a conflict gives the versions of all sides between git-like markers (a draft, not a result)
    LuaChecker lua{};
};

struct MergeInput
{
    Kind kind{Kind::Unknown};
    String name; // the virtual path ("scripts\\sim_squad.script"): the module of the Lua atoms, the chunk name
    String base; // the bytes of the base file (the ancestor of the sides)
    Vector<Side> sides; // in the order of the stack: bottom first, the upper side wins
    Vector<AtomRule> rules;
    Options options;
};

enum class Status : std::uint8_t
{
    Unchanged, // no side changes the base: the text is the base
    Single, // one side changes the base: the text is that side (rebased if it had an ancestor)
    Merged, // merged by atoms
    MergedLines, // a parser did not understand the base: merged by lines, the file as one atom
    Fallback, // the merged text is not valid: the text is the one of the upper side (order)
};

CONTENT_MERGE_API const char* StatusName(Status status); // "unchanged", "single", "merged", "merged_lines", "fallback"

struct Conflict
{
    String atom; // the label: "[sec] key", "w/main_wnd/btn[id=x]@x", "sim_squad.get_spawn_section_name"
    Vector<String> owners; // the sides that changed the atom differently, bottom first
    String winner; // an owner or kBaseOwner
    String resolved_by; // "order" | "player" | "patch" | "rebase"
    Vector<String> lost; // the owners whose change of the atom is not in the result
    std::uint32_t base_line{}; // the line of the atom in the base, 0 - a new atom
    String text; // the text of the atom the winner gave (for the drafts of the patches)
    // A conflict of the rebase (15 section 2.4; the addon wins it): "both_changed" (the base changed the atom too),
    // "base_deleted" (the base has no such atom any more), "both_added" (the base added an atom with this key),
    // "rebase_failed" (the delta cannot be carried over: atom "@file", text - why; the file of the addon is used)
    String reason;
};

struct LineOwner
{
    std::uint32_t from_line{}; // 1-based, inclusive
    std::uint32_t to_line{};
    String owner; // a side or kBaseOwner
};

struct MergeResult
{
    Status status{Status::Unchanged};
    String text; // UTF-8 without BOM, LF
    Vector<String> sides_changed; // the owners whose text differs from the base
    std::uint32_t atoms_merged{}; // the atoms taken from the sides
    std::uint32_t adjacent{}; // the adjacent changes merged inside atoms
    Vector<String> adjacent_atoms;
    Vector<Conflict> conflicts;
    Vector<Conflict> rebase_conflicts;
    Vector<String> reordered; // the atoms some side moved (the order of the base is kept)
    Vector<String> rules_applied; // the atoms of MergeInput::rules that decided an atom
    Vector<LineOwner> lines;
    std::uint32_t unresolved{}; // the conflicts resolved by the order only (15 section 5.4)
    String error; // Fallback: why; MergedLines: why the parser failed
};

CONTENT_MERGE_API MergeResult Merge(const MergeInput& input);

CONTENT_MERGE_API const char* AtomOpName(AtomOp op); // "add" | "modify" | "delete"

struct AtomChange
{
    String atom;
    AtomOp op{};
    std::uint32_t line{}; // in the version (Add, Modify) or the ancestor (Delete)
    bool container{}; // a section / an xml node: its own atoms are listed too
};

CONTENT_MERGE_API Vector<AtomChange> ChangedAtoms(Kind kind, std::string_view name, std::string_view ancestor,
    std::string_view text);

// The check of a merged text (15 section 5.1): Lua - the lexer and the blocks (and LuaChecker if set); ltx - a repeated
// section, a header without ']', an unclosed quote; xml - the tags (#include lines skipped). The text is normalized.
CONTENT_MERGE_API bool Validate(Kind kind, std::string_view name, std::string_view text, String& error,
    const LuaChecker* lua = nullptr);

// SHA-256 of the bytes (Sha256.cpp), 64 lower case hex chars - as sha256sum: the fingerprints of the files of the
// addons a compatibility patch is made for (<patch>\merge\targets.ltx, 15 section 6.1)
CONTENT_MERGE_API String Sha256Hex(std::string_view bytes);
// SHA-256 of the normalized text (Normalize): texts.files[vpath].sha256 of the snapshot, the sha256 of the delta
CONTENT_MERGE_API String NormalizedSha256(std::string_view bytes);

// ---- S9: the delta (Delta.cpp) ---------------------------------------------------------------------------------------

// The delta of one file of an addon against the ancestor - the file of the base of the tag the addon is made on
// (15 section 7.2). Checked without losses: ApplyDelta(ancestor, delta) must give the normalized file of the addon
// byte to byte; otherwise (the parser did not see a change as an atom or a gap, an addon moved atoms) - kind "lines".
// The texts are bytes of the files (normalized inside). kind Unknown - "lines".
CONTENT_MERGE_API DeltaFile BuildDelta(Kind kind, std::string_view vpath, std::string_view ancestor,
    std::string_view addon);

struct RebaseResult
{
    bool ok{}; // false - not carried over (error): the caller uses the file of the addon as it is
    String text; // the file of the addon carried over onto the base (normalized: UTF-8, LF)
    Vector<Conflict> conflicts; // the rebase conflicts (reason), the addon wins them; resolved_by "rebase"
    std::uint32_t atoms{}; // the atoms of the delta placed (the formatting ones not counted)
    String error;
};

// Step 1 of 15 section 2.4: the delta carried over onto the current base by the atoms (the table of the states): the
// atoms the delta does not have come from the base, those it has - from the delta; an atom the base changed too -
// diff3 by lines with the ancestor of the delta (a function, a multi-line value), else a conflict won by the addon.
// `base` - the bytes of the current base file, `owner` - the addon (the conflicts name it)
CONTENT_MERGE_API RebaseResult ApplyDelta(std::string_view base, const DeltaFile& file, std::string_view owner);

// The file of the addon is the published one: NormalizedSha256(addon) == addon_sha256 (else - stale_delta: the delta
// is not used, 15 section 2.4)
CONTENT_MERGE_API bool DeltaMatchesFile(const DeltaFile& file, std::string_view addon_bytes);

// 64 hex chars in lower case (as Sha256Hex writes them): the fingerprints of a delta. "ABC..." is not one - the
// sha256 of a file is compared byte to byte, so an upper case fingerprint would never match (15 section 7.2)
CONTENT_MERGE_API bool IsSha256Hex(std::string_view text);

// What is wrong with one entry of a delta in itself (an author may write a delta by hand, 15 section 2.3): the name,
// the kind, the fingerprints and the atoms against the format of 15 section 7.2, as BuildDelta writes it. Empty -
// nothing. Such an entry is not used: stale_delta for its file (the engine, gw_merge merge --delta-of and apply):
//   - vpath: relative to gamedata\ - not empty, under 256 bytes, no leading or trailing '\', no empty folder, no
//     folder "." or "..", none of : * ? " < > | ; =;
//   - not same_as_ancestor and not_in_ancestor at once; addon_sha256 and (unless not_in_ancestor) ancestor_sha256 -
//     IsSha256Hex;
//   - same_as_ancestor / not_in_ancestor: no atoms (same_as_ancestor: not kind "lines");
//   - kind "lines": one atom "@lines" of op modify that changes the text;
//   - else kind = KindName(KindOfName(vpath)) (not "unknown"), at least one atom; every atom: not "@lines", a path;
//     a gap ("@gap <label>") - the last key of its path starts with '~'; add - no ancestor; delete - no new text;
//     modify - the texts differ; no path twice.
CONTENT_MERGE_API String DeltaFileProblem(const DeltaFile& file);
// The entry of a delta against the bytes of the file of the addon (15 section 2.4): the file is the published one
// (DeltaMatchesFile) and the new texts of the atoms are its texts (a delta written by hand for another file). Empty -
// they agree; else stale_delta for the file
CONTENT_MERGE_API String DeltaFileMismatch(const DeltaFile& file, std::string_view addon_bytes);

// The file of the delta (the format of 15 section 7.2: the section "merge" of files/<id>/<ver>.json of the catalog,
// <addon>\.gw_merge\delta.json of the player). Written, read and written again - the same bytes
CONTENT_MERGE_API String DeltaToJson(const Delta& delta);
// One entry of "files" (the engine hashes it into the key of its cache)
CONTENT_MERGE_API String DeltaFileToJson(const DeltaFile& file);
// False - not a delta of this format (error tells why); the files keep the order of the text. The reference of the
// reading of a delta (the launcher, the validator and tools/addons_snapshot refuse the same texts):
//   - strict JSON (RFC 8259 and these rules): the bytes are strict UTF-8 (no overlong form, no surrogate, nothing over
//     U+10FFFF - a byte 0xFF or a cp1251 text is an error); one UTF-8 BOM at the start is skipped; the spaces are
//     ' ', '\t', '\n', '\r'; no NaN / Infinity, a number as the grammar of RFC 8259; a string: no raw char below
//     0x20, the escapes of RFC 8259, \uD800-\uDBFF only with a following \uDC00-\uDFFF (no lone surrogate); no member
//     name twice in one object (compared after the escapes); at most 64 arrays / objects one in another; nothing but
//     the spaces after the value;
//   - the root is an object; "format" - a number written as an integer (no '.', 'e', 'E'), equal to kDeltaFormat;
//     "algo", "game" - strings, "guessed" - a boolean (each may be absent); "files" - an object;
//   - every member of "files": a non-empty name, an object; "kind", "ancestor_sha256", "addon_sha256", "note" -
//     strings, "same_as_ancestor", "not_in_ancestor", "final_eol" - booleans, "atoms" - an array of atoms (each may be
//     absent); the names of two members are not one file: equal after ASCII lower case and '/' -> '\' (FindDeltaFile
//     would take the first one silently) - the delta is refused as a whole;
//   - an atom: an object; "atom" - a non-empty string; "op" - "add" | "modify" | "delete"; "path" - an array of
//     non-empty strings, not empty unless the atom is "@lines"; "after", "anchor" - strings, "format" - a boolean
//     (each may be absent); "ancestor" - a string for modify and delete, "text" - a string for add and modify.
//     A bad atom refuses the whole delta.
// A delta of another kAlgorithmVersion is read (its "algo" tells it) but not used: stale_delta. The entries are
// checked one by one later (DeltaFileProblem): a bad entry makes only its own file stale_delta.
CONTENT_MERGE_API bool DeltaFromJson(std::string_view text, Delta& out, String& error);
// The entry of a file: vpath in any case, '/' or '\'. nullptr - none
CONTENT_MERGE_API const DeltaFile* FindDeltaFile(const Delta& delta, std::string_view vpath);
// "gw-merge/2" -> "gw-merge-2": the name of the delta recomputed by another version of the parse in the catalog
// (files/<id>/<ver>.merge.<algo>.json; the reference of the launcher and the tools). The player has one delta only:
// <addon>\.gw_merge\delta.json (the launcher writes the recomputed one there too)
CONTENT_MERGE_API String AlgoFileTag(std::string_view algo);
} // namespace xray::content_merge
