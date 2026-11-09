#pragma once

// The base of gw_merge plan besides a folder (00-roadmap.md 5a, decision 63):
//   - the snapshot of the catalog (tools/addons_snapshot builder.py): base_snapshot.json.gz - the files of the base
//     with their sha256 ("files": vpath -> [size, sha256]), the form of each text ("texts" -> "files": enc, bom, eol,
//     sha256 of the normalized text) and where the texts lie ("texts" -> "storage": "tar" - base_texts.tar.gz next to
//     it, "inline" - "text" of each entry, "none"); base_texts.tar.gz - the normalized texts (UTF-8, no BOM, LF) named
//     by their vpaths with '/';
//   - the list of the files of the base the engine writes (appdata\logs\addons_base_files.txt, addon_content.cpp
//     WriteContentBaseFiles): one vpath per line, an optional sha256 after a TAB, '#' - a comment ("# game_tag <tag>"
//     names the release tag of the game). Existence only: a player's base lies in database\*.db.
// The reading is bounded: the size of a packed file, of the unpacked data, of a text, of the texts kept; a name of the
// tar or of the snapshot that leaves gamedata (absolute, "..", a drive, a control char) is not taken. Nothing is
// written to the disk.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace gw_merge
{
// The limits of the reading
constexpr std::uint64_t kMaxPackedBytes = std::uint64_t(1) << 30; // a .gz file (read into memory whole)
// The unpacked JSON of a snapshot: 6-7 times a real one (gw_snapshot.py build of GlobalWar/gamedata, the texts in the
// tar as the release has them: 32.5 MB for 84.7k files on 2026-11-05, ~40 MB for the 117k files of 2026-11-07; the
// texts inline add ~50 MB). The JSON is kept in memory whole and parsed into a tree of a few times its size
constexpr std::uint64_t kMaxSnapshotJson = std::uint64_t(256) << 20;
constexpr std::uint64_t kMaxTarStream = std::uint64_t(8) << 30; // the unpacked stream of a tar (walked, not kept)
constexpr std::uint64_t kMaxTextBytes = std::uint64_t(256) << 20; // one text of the tar
constexpr std::uint64_t kMaxTextsKept = std::uint64_t(2) << 30; // the texts kept from the tar together
constexpr std::uint64_t kMaxListBytes = std::uint64_t(512) << 20; // a list of the files

// One file of the base
struct BaseFile
{
    std::string sha256; // of the file as it lies (64 lower hex), empty - unknown
    std::uint64_t size = 0;
    bool has_size = false;
    // A text of the snapshot ("texts" -> "files"): the form of the file and the sha256 of its normalized text
    bool text = false;
    std::string enc; // "ascii" | "utf-8" | "cp1251" | "cp1251-lossy"
    std::string eol; // "lf" | "crlf" | "mixed" | "none"
    bool bom = false;
    std::string text_sha256;
};

struct BaseIndex
{
    // The name: relative to gamedata\, '\'; a snapshot - ASCII lower case (its vpaths), a list - as SafeVpath gives it
    std::map<std::string, BaseFile> files;
    std::string game_tag; // a snapshot: "game" -> "tag"; a list: "# game_tag <tag>"; empty - unknown
    // A snapshot: "texts" -> "storage"
    std::string texts_mode; // "tar" | "inline" | "none"; empty - no storage
    std::string texts_file; // the tar next to the snapshot (a plain file name), empty - none
    std::string texts_sha256; // of the tar.gz, empty - not written
    std::map<std::string, std::string> inline_texts; // "inline": the name -> the normalized text
    std::size_t skipped = 0; // the entries not taken (a name that leaves gamedata, a broken entry or line)
    std::size_t bad_sha = 0; // a list: a sha256 column that is not 64 hex chars (the file is taken without it)
};

// A relative name of a file of gamedata\ (a vpath of a snapshot, a name of the tar or of a list: '/' or '\') as the
// plan spells it: '\', ASCII lower case when `lower_case`, the components "." dropped. Empty - not a safe name: empty,
// absolute (a leading separator, a drive ':'), "..", an empty component, a control char, not UTF-8
std::string SafeVpath(std::string_view name, bool lower_case);

// base_snapshot.json.gz: the files, the forms of the texts, the storage of the texts, the game tag. False - error
bool ReadSnapshot(const std::string& path, BaseIndex& out, std::string& error);

// The normalized texts of base_texts.tar.gz whose names (SafeVpath, lower case) are in `wanted`. expected_sha256 -
// "texts" -> "storage" -> "sha256" of the snapshot (empty - not checked): a tar of another snapshot is an error.
// `skipped` - the members not taken (an unsafe name, a text over kMaxTextBytes)
bool ReadTextsTar(const std::string& path, const std::string& expected_sha256, const std::set<std::string>& wanted,
    std::map<std::string, std::string>& out, std::size_t& skipped, std::string& error);

// The list of the files of the base (appdata\logs\addons_base_files.txt): UTF-8 (a BOM taken), LF or CRLF; an empty
// line and a line starting with '#' skipped ("# gw_base_files <format>" - format 1 only; "# game_tag <tag>"); a line
// is "<vpath>" or "<vpath>\t<sha256>". The names: SafeVpath with `lower_case`. False - error
bool ReadBaseList(const std::string& path, bool lower_case, BaseIndex& out, std::string& error);
} // namespace gw_merge
