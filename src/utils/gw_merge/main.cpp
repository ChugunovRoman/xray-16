// gw_merge: the merge of the replacements of a text file outside the engine (15-merge-implementation.md section 7.1).
// The same library as the engine (xrCore/ContentMerge, built here with CONTENT_MERGE_STANDALONE), so the catalog CI,
// the launcher and the game give the same result.
//
//   gw_merge atoms   <kind> <ancestor> <file> [--name vpath]           -> json: the changed atoms (add|modify|delete)
//   gw_merge merge   <kind> --base F --side [owner=]F ... [options]    -> the text + a json report (MergeResult)
//   gw_merge rebase  <kind> <ancestor> <new_base> <file> [options]     -> the rebase + the rebase conflicts
//   gw_merge suggest <kind> <ancestor> <file> [--name vpath] [--id A]  -> DLTX / XMLOverride / the hooks to use
//   gw_merge check   <kind> <file> [--check-lua[=lib]]                 -> the check of a text (section 5.1)
//   gw_merge segments <kind> <file>                                    -> the atoms of a file (debug)
//   gw_merge lines   <base> <a> <b> [--exact] [--no-adjacent]          -> diff3 by lines only (tests)
//   gw_merge targets <vpath> <addon>=<file> ...                        -> a section of merge\targets.ltx (sha256)
//   gw_merge delta   --ancestor-dir D --addon A --game T [-o F]        -> the delta S9 of an addon (section 7.2)
//   gw_merge apply   <kind> --delta F --vpath V <new_base> [-o out]    -> ApplyDelta: the delta carried over (2.4)
//   gw_merge merge   ... --delta-of N F                                -> the side N carried over by its delta first
//   gw_merge version                                                   -> json: algorithm, revision, delta_format
//   gw_merge plan    --base D (--addons-dir D | --addon A ...) [...]    -> json: the plan of the merge (plan_format 1)
//   gw_merge plan    --base-snapshot S [--base-texts T] | --base-list L ... -> the base of a snapshot / a list of files
//   gw_merge plan    ... --all-files                                    -> a record of every text file of the layers
// kind: ltx | xml | lua | script | auto (by the extension of the file).
// Exit code: 0 - done; 1 - unresolved conflicts / the check failed / a lines conflict / a rebase conflict / a stale
// delta; 2 - a usage or file error.

#include "ContentMerge.h"
#include "Plan.h"
#include "AtomMerge.h"
#include "base_source.h"
#include "Diff.h"
#include "Segments.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h> // CommandLineToArgvW
#if defined(_MSC_VER)
#pragma comment(lib, "shell32.lib")
#endif
#else
#include <dlfcn.h>
#endif

namespace cm = xray::content_merge;
namespace fs = std::filesystem;

namespace
{
// The strings of the tool are UTF-8: the arguments (on Windows taken as UTF-16 and converted, the ANSI argv cannot hold
// a name out of its code page), the paths, the reports. A path is opened through fs::path made of UTF-8
fs::path PathOfUtf8(const std::string& text)
{
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(text.begin(), text.end()));
#else
    return fs::u8path(text);
#endif
}

int Usage()
{
    std::fprintf(stderr,
        "usage:\n"
        "  gw_merge atoms   <kind> <ancestor> <file> [--name vpath]\n"
        "  gw_merge merge   <kind> --base F --side [owner=]F ... [--patch owner=F ...] [--ancestor-of N F]\n"
        "                   [--patch-targets owner=a,b ...] (the targets of that patch: applied as the engine does,\n"
        "                   only when all of them are sides) [--patches a,b] (the targets of the other patches)\n"
        "                   [--delta-of N F] (the side N carried over by the delta F first; needs --name vpath)\n"
        "                   [--rule atom=owner] [--name vpath] [-o out] [--report json] [--markers] [--fragment]\n"
        "                   [--check-lua[=lib]] [--no-adjacent] [--no-validate] [--encoding utf8|base|top]\n"
        "  gw_merge rebase  <kind> <ancestor> <new_base> <file> [--name vpath] [--id addon] [-o out] [--report json]\n"
        "  gw_merge suggest <kind> <ancestor> <file> [--name vpath] [--id addon]\n"
        "  gw_merge check   <kind> <file> [--name vpath] [--check-lua[=lib]]\n"
        "  gw_merge segments <kind> <file>\n"
        "  gw_merge lines   <base> <a> <b> [--exact] [--no-adjacent]\n"
        "  gw_merge targets <vpath> <addon>=<file> ...\n"
        "  gw_merge delta   --ancestor-dir D --addon A --game T [--latest-dir L] [--guessed] [-o F] [--report F]\n"
        "  gw_merge apply   <kind> --delta F --vpath V <new_base> [--addon-file F] [--id addon] [-o out]\n"
        "                   [--report json]\n"
        "  gw_merge plan    (--base <gamedata> | --base-snapshot <base_snapshot.json.gz> [--base-texts\n"
        "                   <base_texts.tar.gz>] | --base-list <addons_base_files.txt>)\n"
        "                   (--addons-dir D ... [--list appdata/addons.ltx] [--game-root G] | --addon A ...)\n"
        "                   [--pack id] [--tag T] [--case windows|posix] [--merge] [--check-lua[=lib]]\n"
        "                   [--texts dir] [--all-files] [-o plan.json]\n"
        "  gw_merge version\n"
        "kind: ltx | xml | lua | script | auto\n");
    return 2;
}

bool ReadFile(const std::string& path, std::string& out)
{
    std::ifstream f(PathOfUtf8(path), std::ios::binary);
    if (!f)
    {
        std::fprintf(stderr, "gw_merge: cannot read '%s'\n", path.c_str());
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool WriteFile(const std::string& path, const std::string& data)
{
    if (path.empty() || path == "-")
    {
        std::fwrite(data.data(), 1, data.size(), stdout);
        return true;
    }
    std::ofstream f(PathOfUtf8(path), std::ios::binary);
    if (!f)
    {
        std::fprintf(stderr, "gw_merge: cannot write '%s'\n", path.c_str());
        return false;
    }
    f.write(data.data(), std::streamsize(data.size()));
    f.close(); // the last bytes are written here (a full disk shows only after it)
    if (f.fail())
    {
        std::fprintf(stderr, "gw_merge: cannot write '%s'\n", path.c_str());
        return false;
    }
    return true;
}

std::string BaseName(const std::string& path)
{
    const std::size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

cm::Kind KindOf(const std::string& word, const std::string& file)
{
    if (word == "auto")
        return cm::KindOfName(file);
    return cm::KindOfWord(word);
}

std::string Json(std::string_view s)
{
    std::string out = "\"";
    for (const char ch : s)
    {
        switch (ch)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned char>(ch));
                out += buf;
            }
            else
                out += ch;
        }
    }
    return out + "\"";
}

std::string JsonList(const std::vector<std::string>& items)
{
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i)
        out += (i ? ", " : "") + Json(items[i]);
    return out + "]";
}

std::string JsonConflicts(const std::vector<cm::Conflict>& conflicts)
{
    std::string out = "[";
    for (std::size_t i = 0; i < conflicts.size(); ++i)
    {
        const cm::Conflict& c = conflicts[i];
        out += i ? ",\n    " : "\n    ";
        out += "{\"atom\": " + Json(c.atom) + ", \"addons\": " + JsonList(c.owners) + ", \"winner\": " + Json(c.winner) +
            ", \"resolved_by\": " + Json(c.resolved_by) + ", \"lost\": " + JsonList(c.lost) +
            ", \"base_line\": " + std::to_string(c.base_line) +
            (c.reason.empty() ? std::string() : ", \"reason\": " + Json(c.reason)) + "}";
    }
    return out + (conflicts.empty() ? "]" : "\n  ]");
}

std::string JsonReport(const std::string& file, cm::Kind kind, const cm::MergeResult& r,
    const std::vector<std::string>& addons, const std::vector<std::string>& ancestors = {})
{
    std::string out = "{\n";
    out += "  \"algorithm\": " + Json(cm::kAlgorithmVersion) + ",\n";
    out += "  \"file\": " + Json(file) + ",\n";
    out += "  \"kind\": " + Json(cm::KindName(kind)) + ",\n";
    out += "  \"status\": " + Json(cm::StatusName(r.status)) + ",\n";
    out += "  \"addons\": " + JsonList(addons) + ",\n";
    if (!ancestors.empty())
    {
        // The ancestor of each side, as in the report of the engine: base | delta:<tag> | stale_delta (15 section 5.3)
        out += "  \"ancestors\": {";
        for (std::size_t i = 0; i < addons.size() && i < ancestors.size(); ++i)
            out += (i ? ", " : "") + Json(addons[i]) + ": " + Json(ancestors[i]);
        out += "},\n";
    }
    out += "  \"changed\": " + JsonList(r.sides_changed) + ",\n";
    out += "  \"atoms_merged\": " + std::to_string(r.atoms_merged) + ",\n";
    out += "  \"adjacent\": " + std::to_string(r.adjacent) + ",\n";
    out += "  \"adjacent_atoms\": " + JsonList(r.adjacent_atoms) + ",\n";
    out += "  \"reordered\": " + JsonList(r.reordered) + ",\n";
    out += "  \"rules_applied\": " + JsonList(r.rules_applied) + ",\n";
    out += "  \"conflicts\": " + JsonConflicts(r.conflicts) + ",\n";
    out += "  \"rebase_conflicts\": " + JsonConflicts(r.rebase_conflicts) + ",\n";
    out += "  \"unresolved\": " + std::to_string(r.unresolved) + ",\n";
    out += "  \"lines\": [";
    for (std::size_t i = 0; i < r.lines.size(); ++i)
    {
        out += (i ? ", " : "") + std::string("{\"from\": ") + std::to_string(r.lines[i].from_line) +
            ", \"to\": " + std::to_string(r.lines[i].to_line) + ", \"owner\": " + Json(r.lines[i].owner) + "}";
    }
    out += "],\n";
    out += "  \"error\": " + Json(r.error) + "\n}\n";
    return out;
}

// ---- LuaJIT loaded at run time: the tool does not link it (--check-lua) ------------------------------------------

struct LuaApi
{
    void* (*newstate)() = nullptr;
    int (*loadbuffer)(void*, const char*, std::size_t, const char*) = nullptr;
    const char* (*tolstring)(void*, int, std::size_t*) = nullptr;
    void (*close)(void*) = nullptr;
};

void* LoadSymbol(void* lib, const char* name)
{
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

bool LoadLua(const std::string& path, LuaApi& api)
{
    // The library named by --check-lua=<lib> only (another one found instead would check silently), else the defaults
    std::vector<std::string> names;
    if (!path.empty())
        names.push_back(path);
    else
    {
#if defined(_WIN32)
        names.insert(names.end(), {"LuaJIT.dll", "lua51.dll"});
#elif defined(__APPLE__)
        names.insert(names.end(), {"libluajit-5.1.2.dylib", "libluajit-5.1.dylib"});
#else
        names.insert(names.end(), {"libluajit-5.1.so.2", "libluajit-5.1.so"});
#endif
    }
    void* lib = nullptr;
    for (const std::string& name : names)
    {
#if defined(_WIN32)
        lib = reinterpret_cast<void*>(LoadLibraryW(PathOfUtf8(name).c_str()));
#else
        lib = dlopen(name.c_str(), RTLD_NOW);
#endif
        if (lib)
            break;
    }
    if (!lib)
    {
        std::fprintf(stderr, "gw_merge: --check-lua: LuaJIT is not found\n");
        return false;
    }
    api.newstate = reinterpret_cast<void* (*)()>(LoadSymbol(lib, "luaL_newstate"));
    api.loadbuffer =
        reinterpret_cast<int (*)(void*, const char*, std::size_t, const char*)>(LoadSymbol(lib, "luaL_loadbuffer"));
    api.tolstring = reinterpret_cast<const char* (*)(void*, int, std::size_t*)>(LoadSymbol(lib, "lua_tolstring"));
    api.close = reinterpret_cast<void (*)(void*)>(LoadSymbol(lib, "lua_close"));
    if (!api.newstate || !api.loadbuffer || !api.tolstring || !api.close)
    {
        std::fprintf(stderr, "gw_merge: --check-lua: the library has no Lua C API\n");
        return false;
    }
    return true;
}

bool CompileLua(void* context, const char* text, std::size_t size, const char* chunk, std::string& error)
{
    const LuaApi& api = *static_cast<const LuaApi*>(context);
    void* L = api.newstate();
    if (!L)
    {
        error = "luaL_newstate failed";
        return false;
    }
    const int status = api.loadbuffer(L, text, size, chunk);
    if (status != 0)
    {
        const char* message = api.tolstring(L, -1, nullptr);
        error = message ? message : "luaL_loadbuffer failed";
    }
    api.close(L);
    return status == 0;
}

// ---- arguments -----------------------------------------------------------------------------------------------------

struct Args
{
    std::vector<std::string> positional;
    std::multimap<std::string, std::string> options; // "--side" -> value; a flag -> ""

    bool Has(const std::string& name) const { return options.count(name) != 0; }
    std::string Get(const std::string& name, const std::string& fallback = {}) const
    {
        const auto it = options.find(name);
        return it == options.end() ? fallback : it->second;
    }
};

bool ParseArgs(int argc, char** argv, Args& args)
{
    static const char* const with_value[] = {"--base", "--side", "--patch", "--patches", "--patch-targets", "--rule",
        "--name", "-o", "--report", "--encoding", "--id", "--delta", "--vpath", "--ancestor-dir", "--addon", "--game", "--latest-dir",
        "--addon-file", "--addons-dir", "--list", "--game-root", "--pack", "--tag", "--case", "--texts",
        "--base-snapshot", "--base-texts", "--base-list"};
    for (int i = 2; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a.size() > 1 && a[0] == '-')
        {
            const std::size_t eq = a.find('=');
            const std::string name = a.substr(0, eq);
            if (name == "--ancestor-of" || name == "--delta-of")
            {
                if (i + 2 >= argc)
                    return false;
                args.options.emplace(name, std::string(argv[i + 1]) + "=" + argv[i + 2]);
                i += 2;
                continue;
            }
            bool takes = false;
            for (const char* w : with_value)
                takes = takes || name == w;
            if (eq != std::string::npos)
                args.options.emplace(name, a.substr(eq + 1));
            else if (takes)
            {
                if (i + 1 >= argc)
                    return false;
                args.options.emplace(name, argv[++i]);
            }
            else
                args.options.emplace(name, "");
            continue;
        }
        args.positional.push_back(a);
    }
    return true;
}

// The options each command reads: any other one is an error (a mistyped option must not be ignored silently: a
// merge without --no-validate, a delta of another file)
bool KnownOptions(const std::string& cmd, const Args& args)
{
    static const std::map<std::string, std::vector<std::string>> known = {
        {"atoms", {"--name"}},
        {"merge",
            {"--base", "--side", "--patch", "--patches", "--patch-targets", "--rule", "--name", "-o", "--report",
                "--encoding", "--ancestor-of", "--delta-of", "--markers", "--fragment", "--check-lua", "--no-adjacent",
                "--no-validate"}},
        {"rebase", {"--name", "--id", "-o", "--report"}},
        {"suggest", {"--name", "--id"}},
        {"check", {"--name", "--check-lua"}},
        {"segments", {}},
        {"lines", {"--exact", "--no-adjacent"}},
        {"targets", {}},
        {"delta", {"--ancestor-dir", "--addon", "--game", "--latest-dir", "--guessed", "-o", "--report"}},
        {"apply", {"--delta", "--vpath", "--addon-file", "--id", "-o", "--report"}},
        {"version", {}},
        {"plan",
            {"--base", "--base-snapshot", "--base-texts", "--base-list", "--addons-dir", "--list", "--game-root",
                "--addon", "--pack", "--tag", "--case", "--merge", "--check-lua", "--texts", "--all-files", "-o"}},
    };
    const auto it = known.find(cmd);
    if (it == known.end())
        return true; // an unknown command: Usage
    for (const auto& option : args.options)
    {
        if (std::find(it->second.begin(), it->second.end(), option.first) == it->second.end())
        {
            std::fprintf(stderr, "gw_merge: %s: unknown option '%s'\n", cmd.c_str(), option.first.c_str());
            return false;
        }
    }
    return true;
}

// ---- commands ------------------------------------------------------------------------------------------------------

int CmdAtoms(const Args& args)
{
    if (args.positional.size() != 3)
        return Usage();
    std::string anc;
    std::string file;
    if (!ReadFile(args.positional[1], anc) || !ReadFile(args.positional[2], file))
        return 2;
    const std::string name = args.Get("--name", BaseName(args.positional[2]));
    const cm::Kind kind = KindOf(args.positional[0], name);
    const auto changes = cm::ChangedAtoms(kind, name, anc, file);
    std::string out = "{\n  \"algorithm\": " + Json(cm::kAlgorithmVersion) + ",\n  \"file\": " + Json(name) +
        ",\n  \"kind\": " + Json(cm::KindName(kind)) + ",\n  \"same_as_ancestor\": " +
        (cm::Normalize(anc) == cm::Normalize(file) ? "true" : "false") + ",\n  \"atoms\": [";
    for (std::size_t i = 0; i < changes.size(); ++i)
    {
        out += (i ? ",\n    " : "\n    ") + std::string("{\"atom\": ") + Json(changes[i].atom) +
            ", \"op\": " + Json(cm::AtomOpName(changes[i].op)) + ", \"line\": " + std::to_string(changes[i].line) +
            (changes[i].container ? ", \"container\": true" : "") + "}";
    }
    out += changes.empty() ? "]\n}\n" : "\n  ]\n}\n";
    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}

// The conflicting atoms only: a draft of merge\gamedata\<vpath> of a compatibility patch (15 section 6.2)
std::string Fragment(cm::Kind kind, const cm::MergeResult& r)
{
    std::string out;
    if (kind == cm::Kind::Ltx)
    {
        std::map<std::string, std::string> sections; // "[sec]" -> keys
        std::vector<std::string> order;
        for (const cm::Conflict& c : r.conflicts)
        {
            const std::size_t close = c.atom.find("] ");
            if (c.atom.empty() || c.atom[0] != '[' || close == std::string::npos)
            {
                out += c.text;
                continue;
            }
            const std::string sec = c.atom.substr(0, close + 1);
            if (!sections.count(sec))
                order.push_back(sec);
            sections[sec] += c.text;
        }
        for (const std::string& sec : order)
            out += sec + "\n" + sections[sec];
        return out;
    }
    for (const cm::Conflict& c : r.conflicts)
    {
        if (kind == cm::Kind::Xml)
            out += "<!-- " + c.atom + " -->\n";
        out += c.text;
        if (!c.text.empty() && c.text.back() != '\n')
            out += '\n';
    }
    return out;
}

// ---- S9: the delta (15 sections 2.4, 7.2) ----------------------------------------------------------------------------

// The entry of a file in a delta and whether it can be used for the file of the addon, as the engine decides
// (addon_merge.cpp MergeReadDelta, MergeAncestors; 15 section 2.4): ancestor - the word of the report: "delta:<tag>"
// (use), "stale_delta" (the delta is not read, another version of the parse - for every file then, the entry does not
// agree with the format, DeltaFileProblem, or with the file, DeltaFileMismatch), "base" (no entry, not_in_ancestor)
struct DeltaChoice
{
    bool use{};
    cm::DeltaFile file;
    std::string ancestor = "base";
};

bool ChooseDelta(const std::string& delta_path, const std::string& vpath, const std::string& addon_bytes,
    DeltaChoice& choice)
{
    std::string text;
    if (!ReadFile(delta_path, text))
        return false;
    cm::Delta delta;
    std::string error;
    // A broken delta: the engine merges the addon without it (stale_delta), not a failure of the whole merge
    if (!cm::DeltaFromJson(text, delta, error))
    {
        choice.ancestor = "stale_delta";
        std::fprintf(stderr, "gw_merge: stale_delta: '%s' is not read (%s): the delta is ignored, the merge may revert "
            "base changes\n", delta_path.c_str(), error.c_str());
        return true;
    }
    // Another version of the parse: the engine does not use the delta for any file of the addon, its entry or not
    if (delta.algo != cm::kAlgorithmVersion)
    {
        choice.ancestor = "stale_delta";
        std::fprintf(stderr, "gw_merge: stale_delta: the delta is made by %s, this is %s: the delta is ignored\n",
            delta.algo.c_str(), cm::kAlgorithmVersion);
        return true;
    }
    const cm::DeltaFile* file = cm::FindDeltaFile(delta, vpath);
    if (file)
    {
        // An entry that does not agree with the format: stale_delta even when it is not_in_ancestor (as the engine)
        const cm::String problem = cm::DeltaFileProblem(*file);
        if (!problem.empty())
        {
            choice.ancestor = "stale_delta";
            std::fprintf(stderr, "gw_merge: stale_delta: the entry of '%s' does not agree with the format (%s): the "
                "delta is ignored, the merge may revert base changes\n", vpath.c_str(), problem.c_str());
            return true;
        }
    }
    if (!file || file->not_in_ancestor)
    {
        std::fprintf(stderr, "gw_merge: the delta has no atoms of '%s'%s: the base is the ancestor (S8)\n",
            vpath.c_str(), file ? " (not_in_ancestor)" : "");
        return true;
    }
    const cm::String mismatch = cm::DeltaFileMismatch(*file, addon_bytes);
    if (!mismatch.empty())
    {
        choice.ancestor = "stale_delta";
        std::fprintf(stderr, "gw_merge: stale_delta: the delta of '%s' does not agree with its file (%s): the delta is "
            "ignored, the merge may revert base changes\n", vpath.c_str(), mismatch.c_str());
        return true;
    }
    choice.use = true;
    choice.file = *file;
    choice.ancestor = "delta:" + delta.game;
    return true;
}

// N of "--delta-of N=F" / "--ancestor-of N=F": 0 - not a number (std::stoul would throw)
std::size_t SideNumber(const std::string& text)
{
    if (text.empty() || text.size() > 6 || text.find_first_not_of("0123456789") != std::string::npos)
        return 0;
    return std::size_t(std::stoul(text));
}

int CmdMerge(const Args& args)
{
    if (args.positional.size() != 1 || !args.Has("--base") || !args.Has("--side"))
        return Usage();
    if (args.Has("--delta-of") && !args.Has("--name"))
    {
        // the delta keys its files by the vpath: the name of the base file would find nothing (silently S8)
        std::fprintf(stderr, "gw_merge: merge: --delta-of needs --name <vpath> (the file in the delta)\n");
        return 2;
    }
    cm::MergeInput input;
    const std::string base_path = args.Get("--base");
    input.name = args.Get("--name", BaseName(base_path));
    input.kind = KindOf(args.positional[0], input.name);
    if (!ReadFile(base_path, input.base))
        return 2;
    std::vector<std::string> addons;
    std::vector<cm::NormInfo> infos;
    const auto add_side = [&](const std::string& spec, cm::Role role) -> bool
    {
        cm::Side side;
        side.role = role;
        std::string path = spec;
        const std::size_t eq = spec.find('=');
        if (eq != std::string::npos && eq > 0) // "owner=path"
        {
            side.owner = spec.substr(0, eq);
            path = spec.substr(eq + 1);
        }
        else
            side.owner = BaseName(spec);
        if (!ReadFile(path, side.text))
            return false;
        cm::NormInfo info;
        cm::Normalize(side.text, &info);
        infos.push_back(info);
        addons.push_back(side.owner);
        input.sides.push_back(std::move(side));
        return true;
    };
    // the sides in the order of the command line; a patch goes after them (it is the upper one)
    for (const auto& [name, value] : args.options)
    {
        if (name == "--side" && !add_side(value, cm::Role::Addon))
            return 2;
    }
    // "a,b" -> {"a", "b"}
    const auto split_ids = [](const std::string& text) {
        std::vector<std::string> ids;
        std::size_t pos = 0;
        while (pos <= text.size() && !text.empty())
        {
            std::size_t comma = text.find(',', pos);
            if (comma == std::string::npos)
                comma = text.size();
            ids.push_back(text.substr(pos, comma - pos));
            pos = comma + 1;
        }
        return ids;
    };
    // The owner of "[owner=]path" as add_side takes it
    const auto owner_of = [](const std::string& spec) {
        const std::size_t eq = spec.find('=');
        return eq != std::string::npos && eq > 0 ? spec.substr(0, eq) : BaseName(spec);
    };
    // --patch-targets <id>=<a>,<b>: the targets of the patch <id> alone (the patches of one file may be made for
    // different addons). Such a patch is chosen as the engine chooses it (addon_merge.cpp, PlanContentMerge): none of
    // its targets is a side - it has nothing to do with the file; a target is not a side - skipped (not_a_side);
    // else applied. A --patch without it takes --patches as it is (the old form: no choice, the catalog CI checks a
    // patch on a pair of its targets)
    std::map<std::string, std::vector<std::string>> own_targets;
    for (const auto& [name, value] : args.options)
    {
        if (name != "--patch-targets")
            continue;
        const std::size_t eq = value.find('=');
        const std::vector<std::string> ids = eq == std::string::npos ? std::vector<std::string>() :
            split_ids(value.substr(eq + 1));
        if (eq == 0 || eq == std::string::npos || ids.empty() ||
            std::any_of(ids.begin(), ids.end(), [](const std::string& id) { return id.empty(); }))
        {
            std::fprintf(stderr, "gw_merge: merge: --patch-targets '%s': expected <patch id>=<addon>[,<addon> ...]\n",
                value.c_str());
            return 2;
        }
        if (!own_targets.emplace(value.substr(0, eq), ids).second)
        {
            std::fprintf(stderr, "gw_merge: merge: --patch-targets: the patch '%s' is given twice\n",
                value.substr(0, eq).c_str());
            return 2;
        }
    }
    for (const auto& [id, ids] : own_targets)
    {
        const bool known = std::any_of(args.options.begin(), args.options.end(),
            [&](const std::pair<const std::string, std::string>& option) {
                return option.first == "--patch" && owner_of(option.second) == id;
            });
        if (!known)
        {
            std::fprintf(stderr, "gw_merge: merge: --patch-targets: no --patch %s=<file>\n", id.c_str());
            return 2;
        }
    }
    const std::vector<std::string> patches = split_ids(args.Get("--patches"));
    const std::size_t addon_sides = input.sides.size();
    for (const auto& [name, value] : args.options)
    {
        if (name != "--patch")
            continue;
        const auto own = own_targets.find(owner_of(value));
        if (own != own_targets.end())
        {
            const auto is_side = [&](const std::string& id) {
                return std::any_of(input.sides.begin(), input.sides.begin() + std::ptrdiff_t(addon_sides),
                    [&id](const cm::Side& side) { return side.owner == id; });
            };
            if (std::none_of(own->second.begin(), own->second.end(), is_side))
            {
                std::fprintf(stderr, "gw_merge: merge: the patch '%s' is not applied: none of its targets replaces "
                    "the file\n", own->first.c_str());
                continue;
            }
            const auto missing = std::find_if_not(own->second.begin(), own->second.end(), is_side);
            if (missing != own->second.end())
            {
                std::fprintf(stderr, "gw_merge: merge: the patch '%s' is skipped: not_a_side: %s does not replace "
                    "the file\n", own->first.c_str(), missing->c_str());
                continue;
            }
        }
        if (!add_side(value, cm::Role::Patch))
            return 2;
        for (const std::string& target : own != own_targets.end() ? own->second : patches)
            input.sides.back().patches.push_back(target);
    }
    std::vector<std::string> ancestors(input.sides.size(), "base");
    for (const auto& [name, value] : args.options)
    {
        if (name == "--delta-of")
        {
            // S9: the side carried over onto the base by its delta (as the engine does, 15 section 2.4)
            const std::size_t eq = value.find('=');
            const std::size_t n = SideNumber(value.substr(0, eq));
            if (n < 1 || n > input.sides.size())
                return Usage();
            cm::Side& side = input.sides[n - 1];
            DeltaChoice choice;
            if (!ChooseDelta(value.substr(eq + 1), input.name, side.text, choice))
                return 2;
            ancestors[n - 1] = choice.ancestor;
            if (choice.use)
            {
                side.has_delta = true;
                side.delta = std::move(choice.file);
            }
        }
        else if (name == "--ancestor-of")
        {
            const std::size_t eq = value.find('=');
            const std::size_t n = SideNumber(value.substr(0, eq));
            if (n < 1 || n > input.sides.size())
                return Usage();
            cm::Side& side = input.sides[n - 1];
            side.has_ancestor = ReadFile(value.substr(eq + 1), side.ancestor);
            if (!side.has_ancestor)
                return 2;
            ancestors[n - 1] = "ancestor";
        }
        else if (name == "--rule")
        {
            const std::size_t eq = value.rfind('=');
            if (eq == std::string::npos)
                return Usage();
            input.rules.push_back({value.substr(0, eq), value.substr(eq + 1)});
        }
    }
    input.options.merge_adjacent = !args.Has("--no-adjacent");
    input.options.validate = !args.Has("--no-validate");
    input.options.markers = args.Has("--markers");
    LuaApi lua;
    if (args.Has("--check-lua"))
    {
        if (!LoadLua(args.Get("--check-lua"), lua))
            return 2;
        input.options.lua.compile = &CompileLua;
        input.options.lua.context = &lua;
    }

    const cm::MergeResult result = cm::Merge(input);
    std::string text = args.Has("--fragment") ? Fragment(input.kind, result) : result.text;
    const std::string encoding = args.Get("--encoding", "utf8");
    if (encoding == "base" || encoding == "top")
    {
        cm::NormInfo like;
        if (encoding == "base")
            cm::Normalize(input.base, &like);
        else
            like = infos.back();
        text = cm::Denormalize(text, like);
    }
    if (!WriteFile(args.Get("-o", "-"), text))
        return 2;
    const bool any_ancestor = std::any_of(ancestors.begin(), ancestors.end(),
        [](const std::string& a) { return a != "base"; });
    const std::string report =
        JsonReport(input.name, input.kind, result, addons, any_ancestor ? ancestors : std::vector<std::string>());
    if (args.Has("--report"))
    {
        if (!WriteFile(args.Get("--report"), report))
            return 2;
    }
    else
        std::fwrite(report.data(), 1, report.size(), stderr);
    return result.unresolved ? 1 : 0;
}

int CmdRebase(const Args& args)
{
    if (args.positional.size() != 4)
        return Usage();
    cm::MergeInput input;
    input.name = args.Get("--name", BaseName(args.positional[3]));
    input.kind = KindOf(args.positional[0], input.name);
    cm::Side side;
    side.owner = args.Get("--id", "addon");
    side.has_ancestor = true;
    if (!ReadFile(args.positional[1], side.ancestor) || !ReadFile(args.positional[2], input.base) ||
        !ReadFile(args.positional[3], side.text))
        return 2;
    input.sides.push_back(std::move(side));
    const cm::MergeResult result = cm::Merge(input);
    if (!WriteFile(args.Get("-o", "-"), result.text))
        return 2;
    const std::string report = JsonReport(input.name, input.kind, result, {input.sides[0].owner});
    if (args.Has("--report"))
    {
        if (!WriteFile(args.Get("--report"), report))
            return 2;
    }
    else
        std::fwrite(report.data(), 1, report.size(), stderr);
    return result.rebase_conflicts.empty() ? 0 : 1;
}

// ---- suggest: how to write the replacement with the API (15 section 6.3) -----------------------------------------

std::string SegText(const std::string& text, const cm::Seg& seg)
{
    return text.substr(seg.begin, seg.end - seg.begin);
}

const cm::Seg* FindKey(const std::vector<cm::Seg>& list, const std::string& key)
{
    for (const cm::Seg& seg : list)
    {
        if (!seg.gap && seg.key == key)
            return &seg;
    }
    return nullptr;
}

std::string SuggestLtx(const cm::Doc& anc_doc, const std::string& text, const cm::Doc& doc)
{
    std::string out;
    for (const cm::Seg& sec : doc.top)
    {
        if (sec.gap || sec.children.empty())
            continue;
        const cm::Seg* old = FindKey(anc_doc.top, sec.key);
        const std::string name = sec.key.substr(1, sec.key.find(']') - 1);
        if (!old)
        {
            out += "\n" + SegText(text, sec); // [new]: a new section of the patch
            continue;
        }
        if (old->cmp == sec.cmp)
            continue;
        std::string header = "![" + name + "]";
        const cm::Seg* h_new = FindKey(sec.children, ":header");
        const cm::Seg* h_old = FindKey(old->children, ":header");
        if (h_new && h_old && h_new->cmp != h_old->cmp)
        {
            // ![s]:a,!b - a added, b removed (DLTX of xr_ini.cpp)
            const auto parents = [](const std::string& cmp)
            {
                std::vector<std::string> out_list;
                const std::size_t eq = cmp.find('=');
                std::string rest = eq == std::string::npos ? std::string() : cmp.substr(eq + 1);
                std::size_t pos = 0;
                while (!rest.empty() && pos <= rest.size())
                {
                    std::size_t comma = rest.find(',', pos);
                    if (comma == std::string::npos)
                        comma = rest.size();
                    out_list.push_back(rest.substr(pos, comma - pos));
                    pos = comma + 1;
                }
                return out_list;
            };
            const auto was = parents(h_old->cmp);
            const auto now = parents(h_new->cmp);
            std::string changes;
            for (const std::string& p : was)
            {
                if (std::find(now.begin(), now.end(), p) == now.end())
                    changes += (changes.empty() ? "" : ",") + ("!" + p);
            }
            for (const std::string& p : now)
            {
                if (std::find(was.begin(), was.end(), p) == was.end())
                    changes += (changes.empty() ? "" : ",") + p;
            }
            header += ":" + changes;
        }
        out += "\n" + header + "\n";
        for (const cm::Seg& item : sec.children)
        {
            if (item.gap || item.key == ":header")
                continue;
            const cm::Seg* was = FindKey(old->children, item.key);
            if (!was || was->cmp != item.cmp)
                out += SegText(text, item);
        }
        for (const cm::Seg& item : old->children)
        {
            if (item.gap || item.key == ":header" || item.key.rfind("#include", 0) == 0)
                continue;
            if (!FindKey(sec.children, item.key))
                out += "!" + item.key + "\n";
        }
    }
    for (const cm::Seg& sec : anc_doc.top)
    {
        if (!sec.gap && !sec.children.empty() && !FindKey(doc.top, sec.key))
            out += "\n!!" + sec.key + "\n";
    }
    return out;
}

std::string WithOverride(const std::string& node_text, const char* mode)
{
    const std::size_t lt = node_text.find('<');
    std::size_t p = lt + 1;
    while (p < node_text.size() && !std::isspace(static_cast<unsigned char>(node_text[p])) && node_text[p] != '>' &&
        node_text[p] != '/')
        ++p;
    return node_text.substr(0, p) + " override=\"" + mode + "\"" + node_text.substr(p);
}

std::string SuggestXml(const std::string& anc, const cm::Doc& anc_doc, const std::string& text, const cm::Doc& doc)
{
    std::string out;
    for (const cm::Seg& root : doc.top)
    {
        if (root.gap || root.children.empty())
            continue;
        const cm::Seg* old_root = FindKey(anc_doc.top, root.key);
        if (!old_root || old_root->cmp == root.cmp)
            continue;
        const std::string tag = root.key.substr(0, root.key.find_first_of("[#"));
        std::string body;
        for (const cm::Seg& node : root.children)
        {
            if (node.gap || node.children.empty())
                continue;
            const cm::Seg* was = FindKey(old_root->children, node.key);
            if (!was)
                body += WithOverride(SegText(text, node), "add");
            else if (was->cmp != node.cmp)
                body += WithOverride(SegText(text, node), "replace");
        }
        for (const cm::Seg& node : old_root->children)
        {
            if (node.gap || node.children.empty() || FindKey(root.children, node.key))
                continue;
            // the start tag of the node with override="remove", closed at once
            const std::string open = SegText(anc, node.children.front());
            std::string attrs;
            for (const cm::Seg& c : node.children)
            {
                if (c.key.rfind("@id", 0) == 0 || c.key.rfind("@name", 0) == 0)
                    attrs += SegText(anc, c);
            }
            body += open + attrs + " override=\"remove\"/>";
        }
        out += "<" + tag + ">" + body + "\n</" + tag + ">\n";
    }
    return out;
}

std::string SuggestLua(const std::string& module, const cm::Doc& anc_doc, const cm::Doc& doc)
{
    std::string out;
    for (const cm::Seg& seg : doc.top)
    {
        if (seg.gap)
            continue;
        const cm::Seg* was = FindKey(anc_doc.top, seg.key);
        if (was && was->cmp == seg.cmp)
            continue;
        if (seg.function && seg.key.rfind("local ", 0) != 0 && was)
        {
            out += "addons.hook(\"" + module + "." + seg.key + "\", function(...) end) -- " +
                (was ? "changed" : "new") + ", line " + std::to_string(seg.line) + "\n";
        }
        else
        {
            out += "-- " + std::string(was ? "changed" : "new") + " " + seg.label + " (line " +
                std::to_string(seg.line) + "): " +
                (was ? "not a hookable function - keep the replacement or ask for an event" :
                       "a new atom - put it into a module of the addon") +
                "\n";
        }
    }
    return out;
}

int CmdSuggest(const Args& args)
{
    if (args.positional.size() != 3)
        return Usage();
    std::string anc_raw;
    std::string file_raw;
    if (!ReadFile(args.positional[1], anc_raw) || !ReadFile(args.positional[2], file_raw))
        return 2;
    const std::string name = args.Get("--name", BaseName(args.positional[2]));
    const cm::Kind kind = KindOf(args.positional[0], name);
    const std::string anc = cm::Normalize(anc_raw);
    const std::string text = cm::Normalize(file_raw);
    const cm::Doc anc_doc = cm::ParseKind(kind, anc, name);
    const cm::Doc doc = cm::ParseKind(kind, text, name);
    const std::string id = args.Get("--id", "<id>");
    std::string stem = cm::ModuleOfName(name);
    std::string out;
    if (kind == cm::Kind::Ltx)
        out = "; mod_" + stem + "_" + id + ".ltx (DLTX) next to " + name + "\n" + SuggestLtx(anc_doc, text, doc);
    else if (kind == cm::Kind::Xml)
        out = "<!-- mod_" + stem + "_" + id + ".xml (XMLOverride) next to " + name + " -->\n" +
            SuggestXml(anc, anc_doc, text, doc);
    else if (kind == cm::Kind::Lua)
        out = "-- the changes of " + name + " as hooks (gw_hooks)\n" + SuggestLua(stem, anc_doc, doc);
    else
        return Usage();
    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}

int CmdCheck(const Args& args)
{
    if (args.positional.size() != 2)
        return Usage();
    std::string raw;
    if (!ReadFile(args.positional[1], raw))
        return 2;
    const std::string name = args.Get("--name", BaseName(args.positional[1]));
    const cm::Kind kind = KindOf(args.positional[0], name);
    LuaApi lua;
    cm::LuaChecker checker;
    if (args.Has("--check-lua"))
    {
        if (!LoadLua(args.Get("--check-lua"), lua))
            return 2;
        checker.compile = &CompileLua;
        checker.context = &lua;
    }
    std::string error;
    if (cm::Validate(kind, name, raw, error, checker.compile ? &checker : nullptr))
    {
        std::printf("ok\n");
        return 0;
    }
    std::printf("%s: %s\n", name.c_str(), error.c_str());
    return 1;
}

int CmdSegments(const Args& args)
{
    if (args.positional.size() != 2)
        return Usage();
    std::string raw;
    if (!ReadFile(args.positional[1], raw))
        return 2;
    const std::string name = BaseName(args.positional[1]);
    const std::string text = cm::Normalize(raw);
    const cm::Doc doc = cm::ParseKind(KindOf(args.positional[0], name), text, name);
    std::string out = doc.ok ? std::string() : "! the parser failed: " + doc.error + "\n";
    out += cm::DumpSegments(doc.top, text);
    std::fwrite(out.data(), 1, out.size(), stdout);
    return doc.ok ? 0 : 1;
}

int CmdLines(const Args& args)
{
    if (args.positional.size() != 3)
        return Usage();
    std::string base;
    std::string a;
    std::string b;
    if (!ReadFile(args.positional[0], base) || !ReadFile(args.positional[1], a) ||
        !ReadFile(args.positional[2], b))
        return 2;
    base = cm::Normalize(base);
    a = cm::Normalize(a);
    b = cm::Normalize(b);
    const auto r = cm::diff::Merge3(cm::diff::SplitLines(base, 0), cm::diff::SplitLines(a, 1),
        cm::diff::SplitLines(b, 2), !args.Has("--exact"), !args.Has("--no-adjacent"));
    if (r.conflict)
    {
        std::fprintf(stderr, r.over_limit ? "conflict (the texts are too different: over the work limit of the diff)\n" :
                                            "conflict\n");
        return 1;
    }
    const std::string text = cm::diff::JoinLines(r.lines);
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fprintf(stderr, "adjacent %u\n", r.adjacent);
    return 0;
}
// A section of <patch>\merge\targets.ltx (15 section 6.1): the sha256 of the files of the addons the fragment
// <patch>\merge\gamedata\<vpath> is made for. The engine compares them with the files of the installed versions:
// another one - patch_stale in the report. The section is the vpath in ASCII lower case with '\' (the engine compares
// the sections so on every platform: a patch made on Windows works where the disk keeps the case)
int CmdTargets(const Args& args)
{
    if (args.positional.size() < 2)
        return Usage();
    std::string vpath = args.positional[0];
    std::replace(vpath.begin(), vpath.end(), '/', '\\');
    std::transform(vpath.begin(), vpath.end(), vpath.begin(),
        [](char ch) { return ch >= 'A' && ch <= 'Z' ? char(ch - 'A' + 'a') : ch; });
    std::string out = "[" + vpath + "]\n";
    for (std::size_t i = 1; i < args.positional.size(); ++i)
    {
        const std::string& item = args.positional[i];
        const std::size_t eq = item.find('=');
        if (eq == std::string::npos || eq == 0)
            return Usage();
        std::string bytes;
        if (!ReadFile(item.substr(eq + 1), bytes))
            return 2;
        const cm::String hash = cm::Sha256Hex(bytes);
        out += item.substr(0, eq) + " = " + std::string(hash.data(), hash.size()) + "\n";
    }
    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}

// A path as UTF-8 with '/' (u8string is std::string in C++17, std::u8string in C++20): a name out of the ANSI code
// page of Windows does not throw, as path::string() does
std::string Utf8(const fs::path& path)
{
    const auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

std::string Lower(std::string s)
{
    for (char& ch : s)
        ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// The text files of an addon (cm::KindOfName: .ltx, .xml, .script, .lua; the patch files too - CmdDelta drops them by
// cm::IsPatchFile): vpath (lower case, '\') -> the path. The content is <addon>\gamedata\ (09 section 2.1), else the
// folder itself (an old mod); the folders ".*" are skipped, so are the folders "@..." (the alias roots of the addons:
// the file system never mounts them at the names of the game, CLocatorAPI::layer_walk)
std::vector<std::pair<std::string, fs::path>> AddonTexts(const fs::path& addon)
{
    std::vector<std::pair<std::string, fs::path>> out;
    std::error_code ec;
    fs::path root = addon;
    if (fs::is_directory(addon / "gamedata", ec))
        root = addon / "gamedata";
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    for (const fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec))
    {
        const std::string leaf = Utf8(it->path().filename());
        std::error_code type_error;
        if (it->is_directory(type_error))
        {
            if (!leaf.empty() && (leaf[0] == '.' || leaf[0] == '@'))
                it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(type_error) || cm::KindOfName(leaf) == cm::Kind::Unknown)
            continue;
        std::string vpath = Lower(Utf8(it->path().lexically_relative(root)));
        std::replace(vpath.begin(), vpath.end(), '/', '\\');
        out.emplace_back(std::move(vpath), it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The files the game has for the patch rule (cm::IsPatchFile, CmdDelta): a file of the addon, of the base of the author
// or of the latest base
struct DeltaFiles
{
    const std::vector<std::pair<std::string, fs::path>>* texts{};
    fs::path ancestor_dir;
    fs::path latest_dir;
    bool latest{};
};

bool DeltaFileExists(void* context, std::string_view vpath)
{
    const DeltaFiles& files = *static_cast<const DeltaFiles*>(context);
    const auto found = std::lower_bound(files.texts->begin(), files.texts->end(), vpath,
        [](const std::pair<std::string, fs::path>& item, std::string_view key) { return item.first < key; });
    if (found != files.texts->end() && found->first == vpath)
        return true;
    std::string slashed(vpath);
    std::replace(slashed.begin(), slashed.end(), '\\', '/');
    std::error_code ec;
    return fs::is_regular_file(files.ancestor_dir / PathOfUtf8(slashed), ec) ||
        (files.latest && fs::is_regular_file(files.latest_dir / PathOfUtf8(slashed), ec));
}

bool ReadPath(const fs::path& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// The delta S9 of an addon (15 section 7.2, the CI steps 2-4): every text file of the addon that the base of the
// author has (the unpacked base_texts.tar.gz of the snapshot of the tag - vpath with '/', lower case) gets its atoms,
// checked without losses (a failed check - kind "lines"); a file the base of the author does not have but the latest
// base has - not_in_ancestor; the other files are new files of the addon (no delta). The patch files (cm::IsPatchFile:
// configs\...\mod_<stem>_*.<ext> with <stem>.<ext> in the addon or in a base) are not replacements: not in the delta.
// The report: stderr or --report
int CmdDelta(const Args& args)
{
    if (!args.positional.empty() || !args.Has("--ancestor-dir") || !args.Has("--addon") || !args.Has("--game"))
        return Usage();
    const fs::path ancestor_dir = PathOfUtf8(args.Get("--ancestor-dir"));
    const fs::path latest_dir = PathOfUtf8(args.Get("--latest-dir"));
    const fs::path addon = PathOfUtf8(args.Get("--addon"));
    std::error_code ec;
    if (!fs::is_directory(ancestor_dir, ec) || !fs::is_directory(addon, ec) ||
        (args.Has("--latest-dir") && !fs::is_directory(latest_dir, ec)))
    {
        std::fprintf(stderr, "gw_merge: delta: a folder is not found\n");
        return 2;
    }
    cm::Delta delta;
    delta.game = args.Get("--game");
    delta.guessed = args.Has("--guessed");
    std::size_t same = 0, atoms = 0, lines = 0, absent = 0, added = 0, texts_size = 0;
    std::vector<std::string> notes;
    const std::vector<std::pair<std::string, fs::path>> texts = AddonTexts(addon);
    for (std::size_t i = 1; i < texts.size(); ++i)
    {
        // Two files of one name in another case (a case-sensitive file system): the game sees one of them, the delta
        // would have its key twice (no reader takes it)
        if (texts[i].first == texts[i - 1].first)
        {
            std::fprintf(stderr, "gw_merge: delta: '%s' and '%s' are one file for the game\n",
                Utf8(texts[i - 1].second).c_str(), Utf8(texts[i].second).c_str());
            return 2;
        }
    }
    DeltaFiles files;
    files.texts = &texts;
    files.ancestor_dir = ancestor_dir;
    files.latest_dir = latest_dir;
    files.latest = args.Has("--latest-dir");
    const cm::FileExists exists{&DeltaFileExists, &files};
    for (const auto& [vpath, path] : texts)
    {
        if (cm::IsPatchFile(vpath, exists))
            continue;
        std::string slashed = vpath;
        std::replace(slashed.begin(), slashed.end(), '\\', '/');
        std::string file_bytes;
        if (!ReadPath(path, file_bytes))
        {
            std::fprintf(stderr, "gw_merge: cannot read '%s'\n", Utf8(path).c_str());
            return 2;
        }
        const fs::path ancestor_path = ancestor_dir / PathOfUtf8(slashed);
        if (fs::is_regular_file(ancestor_path, ec))
        {
            std::string ancestor_bytes;
            if (!ReadPath(ancestor_path, ancestor_bytes))
                return 2;
            cm::DeltaFile file = cm::BuildDelta(cm::KindOfName(vpath), vpath, ancestor_bytes, file_bytes);
            same += file.same_as_ancestor;
            atoms += file.atoms.size();
            if (file.kind == "lines")
            {
                ++lines;
                notes.push_back(vpath + ": " + file.note);
            }
            for (const cm::DeltaAtom& atom : file.atoms)
                texts_size += atom.ancestor.size() + atom.text.size();
            delta.files.push_back(std::move(file));
        }
        else if (args.Has("--latest-dir") && fs::is_regular_file(latest_dir / PathOfUtf8(slashed), ec))
        {
            cm::DeltaFile file;
            file.vpath = vpath;
            file.kind = cm::KindName(cm::KindOfName(vpath));
            file.addon_sha256 = cm::NormalizedSha256(file_bytes);
            file.not_in_ancestor = true;
            ++absent;
            delta.files.push_back(std::move(file));
        }
        else
            ++added;
    }
    if (!WriteFile(args.Get("-o", "-"), cm::DeltaToJson(delta)))
        return 2;

    // The report of the validator (15 section 7.2): the files as one atom, the size of the texts
    constexpr std::size_t kLargeDelta = 1024 * 1024;
    std::string report = "{\n  \"algorithm\": " + Json(cm::kAlgorithmVersion) + ",\n  \"game\": " + Json(delta.game) +
        ",\n  \"files\": " + std::to_string(delta.files.size()) + ",\n  \"same_as_ancestor\": " + std::to_string(same) +
        ",\n  \"not_in_ancestor\": " + std::to_string(absent) + ",\n  \"new_files\": " + std::to_string(added) +
        ",\n  \"atoms\": " + std::to_string(atoms) + ",\n  \"lines\": " + std::to_string(lines) +
        ",\n  \"lines_files\": " + JsonList(notes) + ",\n  \"texts_bytes\": " + std::to_string(texts_size) +
        ",\n  \"warnings\": [";
    std::vector<std::string> warnings;
    if (texts_size > kLargeDelta)
    {
        warnings.push_back("the texts of the delta take " + std::to_string(texts_size / 1024) +
            " KB: the files are replaced almost whole - write the changes as DLTX / XMLOverride / hooks");
    }
    for (const std::string& note : notes)
        warnings.push_back("one atom (kind lines): " + note);
    if (same)
        warnings.push_back(std::to_string(same) + " file(s) are the same as the base: remove them from the package");
    if (delta.guessed)
        warnings.push_back("[addon] game_version is not set: the version of the base is a guess");
    for (std::size_t i = 0; i < warnings.size(); ++i)
        report += (i ? ",\n    " : "\n    ") + Json(warnings[i]);
    report += warnings.empty() ? "]\n}\n" : "\n  ]\n}\n";
    if (args.Has("--report"))
        return WriteFile(args.Get("--report"), report) ? 0 : 2;
    std::fwrite(report.data(), 1, report.size(), stderr);
    return 0;
}

// ApplyDelta of one file (15 section 2.4; CI step 5 - the delta on a new snapshot): the text carried over goes to -o,
// the report (status, the rebase conflicts) to --report or stderr. status: "rebased" | "stale_delta" (another version
// of the parse, or --addon-file is not the published file) | "not_in_ancestor" | "failed"
int CmdApply(const Args& args)
{
    if (args.positional.size() != 2 || !args.Has("--delta") || !args.Has("--vpath"))
        return Usage();
    const std::string vpath = args.Get("--vpath");
    std::string delta_text;
    std::string base;
    if (!ReadFile(args.Get("--delta"), delta_text) || !ReadFile(args.positional[1], base))
        return 2;
    cm::Delta delta;
    std::string error;
    if (!cm::DeltaFromJson(delta_text, delta, error))
    {
        std::fprintf(stderr, "gw_merge: '%s' is not a delta: %s\n", args.Get("--delta").c_str(), error.c_str());
        return 2;
    }
    const cm::DeltaFile* file = cm::FindDeltaFile(delta, vpath);
    if (!file)
    {
        std::fprintf(stderr, "gw_merge: the delta has no file '%s'\n", vpath.c_str());
        return 2;
    }
    const std::string owner = args.Get("--id", "addon");
    std::string status = "rebased";
    std::string note;
    cm::RebaseResult r;
    // The order of the engine (addon_merge.cpp MergeAncestors): the version of the parse, the entry against the format
    // (an entry that does not agree is stale_delta even when not_in_ancestor), then the entry against the file
    const cm::String problem = cm::DeltaFileProblem(*file);
    if (delta.algo != cm::kAlgorithmVersion)
    {
        status = "stale_delta";
        note = "the delta is made by " + delta.algo + ", this is " + cm::kAlgorithmVersion;
    }
    else if (!problem.empty())
    {
        status = "stale_delta";
        note = "the entry does not agree with the format: " + problem;
    }
    else if (file->not_in_ancestor)
        status = "not_in_ancestor";
    else if (args.Has("--addon-file"))
    {
        std::string addon_bytes;
        if (!ReadFile(args.Get("--addon-file"), addon_bytes))
            return 2;
        const cm::String mismatch = cm::DeltaFileMismatch(*file, addon_bytes);
        if (!mismatch.empty())
        {
            status = "stale_delta";
            note = "the file of the addon does not agree with the delta: " + mismatch;
        }
    }
    if (status == "rebased")
    {
        r = cm::ApplyDelta(base, *file, owner);
        if (!r.ok)
            status = "failed";
        else if (!WriteFile(args.Get("-o", "-"), r.text))
            return 2;
    }
    std::string report = "{\n  \"algorithm\": " + Json(cm::kAlgorithmVersion) + ",\n  \"file\": " + Json(file->vpath) +
        ",\n  \"kind\": " + Json(file->kind) + ",\n  \"game\": " + Json(delta.game) + ",\n  \"status\": " +
        Json(status) + ",\n  \"note\": " + Json(note.empty() ? r.error : note) + ",\n  \"atoms\": " +
        std::to_string(r.atoms) + ",\n  \"rebase_conflicts\": " + JsonConflicts(r.conflicts) + "\n}\n";
    if (args.Has("--report"))
    {
        if (!WriteFile(args.Get("--report"), report))
            return 2;
    }
    else
        std::fwrite(report.data(), 1, report.size(), stderr);
    if (status == "failed")
        return 2;
    return status == "rebased" && r.conflicts.empty() ? 0 : 1;
}
// ---- plan: the plan of the merge of an installation (00-roadmap.md 5a, decision 63) ---------------------------------
// The decisions of the engine (xrCore ContentMerge/Plan.h - the same code as addon_merge.cpp / addon_content.cpp) over
// folders: the base, the addons, appdata\addons.ltx. The JSON (plan_format 1; a new member keeps the format, a changed
// meaning of a member is plan_format 2): plan_format, algorithm, revision, case, base, game_tag, merged, addons[] (the
// states, the order, the delta of each), layers[] (the stack after the rules), rules[] (the statuses of pair: / file: /
// atom:), guessed_addons, files[] (the records of the plan: the sides with their ancestors, the patches, the atom: rules
// kept; result with --merge), conflicts[] (the files the layers of two addons give: winner, resolved_by),
// approximations, warnings. The fields are the ones of addons_report.json (merges) where they mean the same. Added in
// format 1: base_source ("folder" | "snapshot" | "list"), base_texts (the tar of a snapshot, "inline", or empty),
// base_files (the files a snapshot / a list names), files[].base_sha256 (the base file the record is planned against:
// its sha256 as it lies, empty - unknown), the note no_base_text, conflicts[].fallback_winner / fallback_resolved_by
// (a file the merge serves: the layer that serves it when the merged text does not hold, and why - as winner /
// resolved_by of a file that is not merged), with --all-files: "all_files": true and a record in files[] of every text
// file a layer of an addon (or the location pack) serves - the ones the plan has none of get "extra": true, status
// skipped, one side, the note a record of the merge would have (file_rule, pack, patch_file, no_base, read_error) or
// one_addon (the file of the addon replaces the base file as it is); a file no layer serves ([private], given up by a
// file: rule, under "@...", hidden) has no record; conflicts[] and the statuses of the atom: rules do not change. A
// name of a layer too long for the index (cm::WalkNameTooLong: the engine does not mount it) is left out with a
// warning "FS: layer '<id>': name too long ...".
//
// The base (one of):
//   --base <gamedata>          a folder: the files and their texts (the archives of the game are not read);
//   --base-snapshot <json.gz>  the snapshot of the catalog (tools/addons_snapshot, base_source.h): the files and their
//                              sha256; the texts from --base-texts <tar.gz> (default: "texts" -> "storage" -> "file"
//                              next to the snapshot, or the inline texts) - read only with --merge, as the raw file
//                              when its sha256 matches the snapshot, else the normalized text (a warning);
//   --base-list <txt>          the list of the files of the base (appdata\logs\addons_base_files.txt the engine
//                              writes): existence only - a file that needs the text of the base is planned (sides,
//                              ancestors, patches, rules) but not merged: status skipped, note no_base_text.
//
// What the folders cannot give and the tool approximates (the "approximations" member of the JSON lists them): the
// state of an addon is enabled + its depends / patches present and active (no api_min, version conditions of depends
// or plugin checks: a launcher passes --addon in the order of addons_report.json for the exact stack); the location
// pack is --pack <id>; the base as above; the writes of a pack are not known.

constexpr int kPlanFormat = 1;

struct PlanIniSection
{
    std::string name; // as written
    std::vector<std::pair<std::string, std::string>> lines; // key, value (a line without '=': key only)
};

std::string PlanTrim(const std::string& text)
{
    const std::size_t begin = text.find_first_not_of(" \t\r");
    if (begin == std::string::npos)
        return std::string();
    return text.substr(begin, text.find_last_not_of(" \t\r") - begin + 1);
}

// A simple ltx: [section], key = value, ';' comments; a BOM at the start skipped
std::vector<PlanIniSection> PlanReadIni(const std::string& bytes)
{
    std::vector<PlanIniSection> out;
    std::string text = bytes;
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0)
        text.erase(0, 3);
    std::size_t start = 0;
    while (start <= text.size())
    {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        const std::size_t comment = line.find(';');
        if (comment != std::string::npos)
            line.erase(comment);
        line = PlanTrim(line);
        if (line.empty())
            continue;
        if (line[0] == '[')
        {
            const std::size_t close = line.find(']');
            out.push_back({PlanTrim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1)), {}});
            continue;
        }
        if (out.empty())
            continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            out.back().lines.emplace_back(line, std::string());
        else
            out.back().lines.emplace_back(PlanTrim(line.substr(0, eq)), PlanTrim(line.substr(eq + 1)));
    }
    return out;
}

const PlanIniSection* PlanFindSection(const std::vector<PlanIniSection>& ini, const std::string& name)
{
    for (const PlanIniSection& section : ini)
    {
        if (cm::AsciiLower(section.name) == name)
            return &section;
    }
    return nullptr;
}

std::string PlanValue(const PlanIniSection* section, const std::string& key)
{
    if (!section)
        return std::string();
    for (const auto& [k, v] : section->lines)
    {
        if (cm::AsciiLower(k) == key)
            return v;
    }
    return std::string();
}

// "a, b c" -> {"a", "b"}: the ids of a list (a version condition after an id - "depends = x >= 1.2" - dropped)
std::vector<std::string> PlanIds(const std::string& text)
{
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < text.size())
    {
        std::size_t comma = text.find(',', pos);
        if (comma == std::string::npos)
            comma = text.size();
        std::string item = PlanTrim(text.substr(pos, comma - pos));
        const std::size_t stop = item.find_first_of(" \t<>=!~^");
        if (stop != std::string::npos)
            item.erase(stop);
        if (!item.empty())
            out.push_back(cm::AsciiLower(item));
        pos = comma + 1;
    }
    return out;
}

struct PlanAddonDir
{
    std::string id;
    std::string type;
    std::string dir; // UTF-8, the trailing separator
    bool enabled = true;
    std::vector<std::string> depends;
    std::vector<std::string> load_after;
    std::vector<std::string> patches;
    std::string game_version;
    std::vector<std::string> private_masks;
    std::vector<std::pair<std::string, std::string>> slots; // slot (lower case) -> files folder (as written, or empty)
    bool active = false;
    std::string reason; // why not active
};

std::string PlanDirText(const fs::path& path)
{
    std::string text = Utf8(path); // '/'
#if defined(_WIN32)
    std::replace(text.begin(), text.end(), '/', '\\');
#endif
    if (!text.empty() && text.back() != '\\' && text.back() != '/')
        text += static_cast<char>(fs::path::preferred_separator);
    return text;
}

bool PlanReadAddon(const fs::path& dir, PlanAddonDir& out, std::vector<std::string>& warnings)
{
    std::string bytes;
    if (!ReadPath(dir / "addon.ltx", bytes))
    {
        warnings.push_back("no addon.ltx in " + Utf8(dir) + ": not an addon");
        return false;
    }
    const std::vector<PlanIniSection> ini = PlanReadIni(bytes);
    const PlanIniSection* addon = PlanFindSection(ini, "addon");
    out.dir = PlanDirText(dir);
    out.id = cm::AsciiLower(PlanValue(addon, "id"));
    if (out.id.empty())
        out.id = cm::AsciiLower(Utf8(dir.filename()));
    out.type = cm::AsciiLower(PlanValue(addon, "type"));
    if (out.type.empty())
        out.type = "content";
    out.depends = PlanIds(PlanValue(addon, "depends"));
    out.load_after = PlanIds(PlanValue(addon, "load_after"));
    out.patches = PlanIds(PlanValue(addon, "patches"));
    out.game_version = PlanValue(addon, "game_version");
    if (const PlanIniSection* masks = PlanFindSection(ini, "private"))
    {
        for (const auto& [key, value] : masks->lines)
            out.private_masks.push_back(key); // one mask per line, as a key (addon_host.cpp)
    }
    for (const PlanIniSection& section : ini)
    {
        const std::string name = cm::AsciiLower(section.name);
        if (name.compare(0, 5, "slot.") == 0 && name.size() > 5)
            out.slots.emplace_back(name.substr(5), PlanValue(&section, "files"));
    }
    return true;
}

// The states and the order of the addons (addon_host.cpp ResolveDependencies, CheckPatchTargets): an addon works while
// it is enabled and each addon of its depends and patches works; the working ones in a stable topological order -
// depends and patches first, then load_after when it can hold, else the order given
std::vector<PlanAddonDir> PlanOrder(std::vector<PlanAddonDir> all)
{
    const auto find = [&all](const std::string& id) -> PlanAddonDir* {
        for (PlanAddonDir& addon : all)
        {
            if (addon.id == id)
                return &addon;
        }
        return nullptr;
    };
    for (PlanAddonDir& addon : all)
    {
        addon.active = addon.enabled;
        if (!addon.enabled)
            addon.reason = "disabled";
    }
    for (bool changed = true; changed;)
    {
        changed = false;
        for (PlanAddonDir& addon : all)
        {
            if (!addon.active)
                continue;
            for (const std::string& id : addon.depends)
            {
                const PlanAddonDir* target = find(id);
                if (!target || !target->active)
                {
                    addon.active = false;
                    addon.reason = "depends: " + id + " is not active";
                    changed = true;
                    break;
                }
            }
            for (const std::string& id : addon.patches)
            {
                const PlanAddonDir* target = find(id);
                if (addon.active && (!target || !target->active))
                {
                    addon.active = false;
                    addon.reason = "patches: " + id + " is not active";
                    changed = true;
                }
            }
        }
    }
    std::vector<PlanAddonDir> ordered;
    std::vector<bool> placed(all.size(), false);
    const auto is_placed = [&](const std::string& id) {
        return std::any_of(ordered.begin(), ordered.end(), [&id](const PlanAddonDir& a) { return a.id == id; });
    };
    const auto is_waiting = [&](const std::string& id) {
        for (std::size_t i = 0; i < all.size(); ++i)
        {
            if (!placed[i] && all[i].active && all[i].id == id)
                return true;
        }
        return false;
    };
    for (bool any = true; any;)
    {
        any = false;
        std::size_t soft = all.size();
        for (std::size_t i = 0; i < all.size(); ++i)
        {
            if (placed[i] || !all[i].active)
                continue;
            const bool ready = std::all_of(all[i].depends.begin(), all[i].depends.end(), is_placed) &&
                std::all_of(all[i].patches.begin(), all[i].patches.end(), is_placed);
            if (!ready)
                continue;
            if (std::any_of(all[i].load_after.begin(), all[i].load_after.end(), is_waiting))
            {
                soft = std::min(soft, i);
                continue;
            }
            ordered.push_back(all[i]);
            placed[i] = true;
            any = true;
            break;
        }
        if (!any && soft < all.size())
        {
            ordered.push_back(all[soft]);
            placed[soft] = true;
            any = true;
        }
    }
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        if (placed[i])
            continue;
        if (all[i].active)
        {
            all[i].active = false;
            all[i].reason = "dependency cycle";
        }
        ordered.push_back(all[i]);
    }
    return ordered;
}

// [slot.*] files = (addon_slots.cpp SlotFilesValid): a folder inside the addon - no drive, no leading separator, no "."
// or ".." component, no empty component but the trailing separator
bool PlanSlotFilesValid(const std::string& files)
{
    if (files.empty() || files.find(':') != std::string::npos || files[0] == '\\' || files[0] == '/')
        return false;
    std::size_t start = 0;
    while (start < files.size())
    {
        std::size_t end = files.find_first_of("\\/", start);
        if (end == std::string::npos)
            end = files.size();
        if (files.compare(start, end - start, "..") == 0 || files.compare(start, end - start, ".") == 0)
            return false;
        if (end == start && end != files.size() - 1)
            return false;
        start = end + 1;
    }
    return true;
}

// A file of a rule (addon_host.cpp NormalizeRuleFileText): '/' -> '\', no leading separator or empty folder, lower case
// on the Windows index; false - not a file name
bool PlanRuleFile(const std::string& text, bool lower_case, std::string& out)
{
    out.clear();
    for (const char c : text)
    {
        const char normalized = c == '/' ? '\\' : (lower_case && c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
        if (normalized == '\\' && (out.empty() || out.back() == '\\'))
            continue;
        out += normalized;
    }
    if (!out.empty() && out.back() == '\\')
        return false;
    const std::string wrapped = "\\" + out + "\\";
    return !out.empty() && out.size() < 256 && out.find_first_of(":*?\"<>|;=") == std::string::npos &&
        wrapped.find("\\..\\") == std::string::npos && wrapped.find("\\.\\") == std::string::npos;
}

// The atom of atom:<file>|<atom> (addon_host.cpp DecodeRuleAtomText, RuleAtomLabel): %XX decoded, trimmed, "@gap X" -> X
bool PlanRuleAtom(const std::string& text, std::string& atom)
{
    atom.clear();
    const auto hex = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '\n' || c == '\r' || c == '\0')
            return false;
        if (c != '%')
        {
            atom += c;
            continue;
        }
        if (i + 2 >= text.size() || hex(text[i + 1]) < 0 || hex(text[i + 2]) < 0)
            return false;
        atom += static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
        i += 2;
    }
    atom = PlanTrim(atom);
    if (atom.compare(0, 4, "@gap") == 0 && (atom.size() == 4 || atom[4] == ' '))
        atom = PlanTrim(atom.substr(4));
    return !atom.empty();
}

struct PlanCli
{
    std::string source = "folder"; // the base: "folder" | "snapshot" | "list"
    std::string base; // the folder of the base (gamedata), the snapshot or the list, UTF-8
    std::vector<std::string> layer_texts_all; // every text name of the owned layers (the texts of the base to read)
    // The text names the owned layers serve at the game data (cm::LayerServes): the patch rule (PlanFileExists). Not a
    // [private] file nor one a file: rule took from the layer - the index of the engine has them under "@<id>\" only
    // (LocatorAPI.cpp layer_walk, addon_content.cpp MountContentFolder), they are not the targets of patch files
    std::vector<std::string> served_texts;
    gw_merge::BaseIndex index; // snapshot / list: the files of the base
    // snapshot: the texts are there (the tar, inline) - a text file of the base has its text; without --merge they are
    // not read (the plan does not use them), with --merge `texts` holds them
    bool texts_on = false;
    bool texts_loaded = false;
    std::map<std::string, std::string> texts; // the name (lower case) -> the base text for the merge
};

// The entry of the base of a snapshot / a list: the snapshot names its files in lower case
const gw_merge::BaseFile* PlanBaseEntry(const PlanCli& cli, std::string_view name)
{
    const auto it = cli.index.files.find(cli.source == "snapshot" ? cm::AsciiLower(name) : std::string(name));
    return it == cli.index.files.end() ? nullptr : &it->second;
}

// A name of the plan as a path under a folder: not empty, no leading separator, no component "..". A name of a layer
// comes from a walk, and on POSIX a file name may hold '\' ("..\..\x.ltx" is one file there): once '\' -> '/' it
// would leave the folder (the base read, the --texts write)
bool PlanNameStaysInside(std::string_view name)
{
    if (name.empty() || name[0] == '\\' || name[0] == '/')
        return false;
    for (std::size_t start = 0; start <= name.size();)
    {
        std::size_t end = name.find_first_of("\\/", start);
        if (end == std::string_view::npos)
            end = name.size();
        if (name.substr(start, end - start) == "..")
            return false;
        start = end + 1;
    }
    return true;
}

fs::path PlanBasePath(const PlanCli& cli, std::string_view name)
{
    if (!PlanNameStaysInside(name))
        return fs::path(); // not a file of the base: is_regular_file / ReadPath fail
    std::string slashed(name);
    std::replace(slashed.begin(), slashed.end(), '\\', '/');
    return PathOfUtf8(cli.base) / PathOfUtf8(slashed);
}

bool PlanBaseHas(const PlanCli& cli, std::string_view name)
{
    if (cli.source != "folder")
        return PlanBaseEntry(cli, name) != nullptr;
    std::error_code ec;
    return fs::is_regular_file(PlanBasePath(cli, name), ec);
}

// The text of the base file (cm::PlanEnv::base): false - the base has no such file or no text of it (PlanBaseExists)
bool PlanBase(void* context, std::string_view name, std::string& out)
{
    const PlanCli& cli = *static_cast<const PlanCli*>(context);
    if (cli.source == "folder")
    {
        std::error_code ec;
        const fs::path path = PlanBasePath(cli, name);
        return fs::is_regular_file(path, ec) && ReadPath(path, out);
    }
    const gw_merge::BaseFile* entry = PlanBaseEntry(cli, name);
    if (!cli.texts_on || !entry || !entry->text)
        return false;
    if (!cli.texts_loaded)
    {
        out.clear(); // the plan without --merge: the text is not used
        return true;
    }
    const auto text = cli.texts.find(cm::AsciiLower(name));
    if (text == cli.texts.end())
        return false;
    out = text->second;
    return true;
}

bool PlanBaseExists(void* context, std::string_view name)
{
    return PlanBaseHas(*static_cast<const PlanCli*>(context), name);
}

// sha256 of the base file as it lies: a folder - read, a snapshot / a list - as written; empty - unknown
std::string PlanBaseSha256(const PlanCli& cli, std::string_view name)
{
    if (cli.source != "folder")
    {
        const gw_merge::BaseFile* entry = PlanBaseEntry(cli, name);
        return entry ? entry->sha256 : std::string();
    }
    std::string bytes;
    std::error_code ec;
    const fs::path path = PlanBasePath(cli, name);
    return fs::is_regular_file(path, ec) && ReadPath(path, bytes) ? cm::Sha256Hex(bytes) : std::string();
}

bool PlanFileExists(void* context, std::string_view vpath)
{
    const PlanCli& cli = *static_cast<const PlanCli*>(context);
    if (std::binary_search(cli.served_texts.begin(), cli.served_texts.end(), std::string(vpath)))
        return true;
    return PlanBaseHas(cli, vpath);
}

// The form of a text of the snapshot (textnorm.py) as the library names it
cm::NormInfo PlanNormInfo(const gw_merge::BaseFile& file)
{
    cm::NormInfo info;
    info.enc = file.enc == "utf-8" ? cm::Encoding::Utf8 : file.enc == "cp1251" ? cm::Encoding::Cp1251 :
        file.enc == "cp1251-lossy" ? cm::Encoding::Cp1251Lossy : cm::Encoding::Ascii;
    info.bom = file.bom;
    info.eol = file.eol == "crlf" ? cm::Eol::Crlf : file.eol == "lf" ? cm::Eol::Lf : file.eol == "mixed" ?
        cm::Eol::Mixed : cm::Eol::None;
    return info;
}

// --base-snapshot / --base-list: the files, the tag, the texts (with --merge). False - an error told
bool PlanReadBase(const Args& args, bool lower_case, bool merge, PlanCli& cli, std::vector<std::string>& warnings,
    std::string& texts_used)
{
    std::string error;
    if (cli.source == "list")
    {
        if (!gw_merge::ReadBaseList(cli.base, lower_case, cli.index, error))
        {
            std::fprintf(stderr, "gw_merge: plan: --base-list: %s\n", error.c_str());
            return false;
        }
        if (cli.index.skipped)
            warnings.push_back("--base-list: " + std::to_string(cli.index.skipped) +
                " line(s) are not a file name of gamedata (or repeat one): skipped");
        if (cli.index.bad_sha)
            warnings.push_back("--base-list: " + std::to_string(cli.index.bad_sha) +
                " sha256 column(s) are not 64 hex chars: ignored");
        return true;
    }
    if (!gw_merge::ReadSnapshot(cli.base, cli.index, error))
    {
        std::fprintf(stderr, "gw_merge: plan: --base-snapshot: %s\n", error.c_str());
        return false;
    }
    if (cli.index.skipped)
        warnings.push_back("--base-snapshot: " + std::to_string(cli.index.skipped) +
            " entr(ies) of \"files\" are not a file name of gamedata (or repeat one): skipped");
    // The texts: --base-texts, else the tar the snapshot names next to it, else the inline texts
    std::string tar = args.Get("--base-texts");
    std::error_code ec;
    if (!tar.empty() && !fs::is_regular_file(PathOfUtf8(tar), ec))
    {
        std::fprintf(stderr, "gw_merge: plan: --base-texts '%s' is not a file\n", tar.c_str());
        return false;
    }
    if (tar.empty() && cli.index.texts_mode == "tar" && !cli.index.texts_file.empty())
    {
        const fs::path next_to = PathOfUtf8(cli.base).parent_path() / PathOfUtf8(cli.index.texts_file);
        if (fs::is_regular_file(next_to, ec))
            tar = Utf8(next_to);
    }
    const bool inline_texts = tar.empty() && cli.index.texts_mode == "inline";
    cli.texts_on = !tar.empty() || inline_texts;
    texts_used = !tar.empty() ? tar : inline_texts ? std::string("inline") : std::string();
    if (!cli.texts_on)
    {
        warnings.push_back("--base-snapshot: no texts of the base (no --base-texts, no " +
            (cli.index.texts_file.empty() ? std::string("tar") : cli.index.texts_file) +
            " next to the snapshot, not inline): the files that need the base text are not merged (no_base_text)");
        return true;
    }
    if (!merge)
        return true; // the texts are not read: the plan without --merge does not use them

    // The texts the plan can ask: the text names of the layers of the addons the base has a text of
    std::set<std::string> wanted;
    for (const std::string& name : cli.layer_texts_all)
    {
        const gw_merge::BaseFile* entry = PlanBaseEntry(cli, name);
        if (entry && entry->text)
            wanted.insert(cm::AsciiLower(name));
    }
    std::map<std::string, std::string> normalized;
    if (inline_texts)
    {
        for (const std::string& name : wanted)
        {
            const auto it = cli.index.inline_texts.find(name);
            if (it != cli.index.inline_texts.end())
                normalized.emplace(name, it->second);
        }
    }
    else
    {
        std::size_t skipped = 0;
        if (!gw_merge::ReadTextsTar(tar, cli.index.texts_sha256, wanted, normalized, skipped, error))
        {
            std::fprintf(stderr, "gw_merge: plan: --base-texts: %s\n", error.c_str());
            return false;
        }
        if (skipped)
            warnings.push_back("--base-texts: " + std::to_string(skipped) +
                " member(s) of the tar are not a file name of gamedata or are too large: skipped");
    }
    // The raw file back from the normalized text (Denormalize) when the sha256 of the snapshot confirms it; else the
    // normalized text (the merge is the same, the bytes of a merged text may differ from the ones of the engine)
    std::size_t missing = 0, broken = 0, inexact = 0;
    std::string first_inexact;
    for (const std::string& name : wanted)
    {
        const gw_merge::BaseFile& entry = *PlanBaseEntry(cli, name);
        const auto it = normalized.find(name);
        if (it == normalized.end())
        {
            ++missing;
            continue;
        }
        if (!entry.text_sha256.empty() && cm::Sha256Hex(it->second) != entry.text_sha256)
        {
            ++broken;
            continue;
        }
        std::string raw = cm::Denormalize(it->second, PlanNormInfo(entry));
        if (!entry.sha256.empty() && cm::Sha256Hex(raw) != entry.sha256)
        {
            if (!inexact++)
                first_inexact = name + " (" + entry.enc + ", " + entry.eol + (entry.bom ? ", BOM" : "") + ")";
            raw = it->second;
        }
        cli.texts.emplace(name, std::move(raw));
    }
    cli.texts_loaded = true;
    if (missing)
        warnings.push_back("--base-texts: " + std::to_string(missing) +
            " text(s) of the base are not in the texts: those files are not merged (no_base_text)");
    if (broken)
        warnings.push_back("--base-texts: " + std::to_string(broken) +
            " text(s) do not match their sha256 in the snapshot: those files are not merged (no_base_text)");
    if (inexact)
        warnings.push_back("--base-texts: " + std::to_string(inexact) + " base file(s) are not rebuilt from the "
            "normalized text (" + first_inexact + (inexact > 1 ? ", ..." : "") + "): merged against the normalized "
            "text, the bytes of the merged text may differ from the game");
    return true;
}

bool PlanIsPatchFile(void* context, std::string_view name)
{
    return cm::IsPatchFile(name, cm::FileExists{&PlanFileExists, context});
}

// CLocatorAPI::layer_walk_skips of the OS the tool runs on: the leaf rules of the library, the hidden attribute on
// Windows, ".*" on POSIX
bool PlanWalkSkips(void* /*context*/, const char* physical)
{
    std::string path = physical ? physical : "";
    while (!path.empty() && (path.back() == '\\' || path.back() == '/'))
        path.pop_back();
    const std::size_t slash = path.find_last_of("\\/");
    const std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
#if defined(_WIN32)
    if (cm::WalkSkipsEntry(leaf, false))
        return true;
    const DWORD attributes = GetFileAttributesW(PathOfUtf8(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_HIDDEN) != 0;
#else
    return cm::WalkSkipsEntry(leaf, cm::HiddenByName(leaf));
#endif
}

std::string JsonBool(bool value) { return value ? "true" : "false"; }

std::string JsonStatuses(const std::vector<cm::RuleStatus>& statuses)
{
    std::string out = "[";
    for (std::size_t i = 0; i < statuses.size(); ++i)
    {
        const cm::RuleStatus& s = statuses[i];
        out += (i ? ",\n    " : "\n    ") + std::string("{\"kind\": ") + Json(s.kind) + ", \"winner\": " + Json(s.winner) +
            ", \"target\": " + Json(s.target) + ", \"applied\": " + JsonBool(s.applied) + ", \"note\": " + Json(s.note) + "}";
    }
    return out + (statuses.empty() ? "]" : "\n  ]");
}

std::string JsonPlanFile(const cm::PlanEntry& entry, const std::vector<cm::PlanLayer>& layers, bool merged,
    const std::string& text_sha256, const std::string& base_sha256)
{
    const cm::PlanFile& f = entry.item;
    std::string out = "{\"file\": " + Json(f.file) + ", \"kind\": " + Json(f.kind) + ",\n      \"merge\": " +
        JsonBool(entry.merge) + ", \"alone\": " + JsonBool(entry.alone) +
        (entry.extra ? ", \"extra\": true" : "") + ", \"status\": " + Json(f.status) +
        ", \"note\": " + Json(f.note) + ", \"error\": " + Json(f.error) + ", \"base_sha256\": " + Json(base_sha256) +
        ",\n      \"sides\": [";
    for (std::size_t i = 0; i < f.addons.size(); ++i)
    {
        const std::size_t layer = i < entry.job.side_layers.size() ? entry.job.side_layers[i] : layers.size();
        out += (i ? ",\n        " : "\n        ") + std::string("{\"addon\": ") + Json(f.addons[i]) +
            ", \"layer\": " + Json(i < f.layers.size() ? f.layers[i] : std::string()) + ", \"path\": " +
            Json(layer < layers.size() ? layers[layer].folder + f.file : std::string()) + ", \"ancestor\": " +
            Json(i < f.ancestors.size() ? f.ancestors[i] : std::string()) + ", \"ancestor_note\": " +
            Json(i < f.ancestor_notes.size() ? f.ancestor_notes[i] : std::string()) + "}";
    }
    out += f.addons.empty() ? "]" : "\n      ]";
    out += ",\n      \"patches\": [";
    for (std::size_t i = 0; i < f.patches.size(); ++i)
    {
        const cm::PlanFile::Patch& p = f.patches[i];
        out += (i ? ",\n        " : "\n        ") + std::string("{\"id\": ") + Json(p.id) + ", \"targets\": " +
            JsonList(p.targets) + ", \"status\": " + Json(p.status) + ", \"note\": " + Json(p.note) +
            ", \"stale_targets\": " + JsonList(p.stale_targets) +
            (merged && entry.merge ? ", \"resolved\": " + std::to_string(p.resolved) : "") + "}"; // merged here only
    }
    out += f.patches.empty() ? "]" : "\n      ]";
    out += ",\n      \"rules\": [";
    for (std::size_t i = 0; i < entry.job.rules.size(); ++i)
    {
        out += (i ? ", " : "") + std::string("{\"atom\": ") + Json(entry.job.rules[i].atom) + ", \"winner\": " +
            Json(entry.job.rules[i].owner) + "}";
    }
    out += "]";
    if (merged && entry.merge)
    {
        out += ",\n      \"result\": {\"served\": " + JsonBool(f.served) + ", \"text_sha256\": " + Json(text_sha256) +
            ", \"changed\": " + JsonList(f.changed) + ", \"atoms_merged\": " + std::to_string(f.atoms_merged) +
            ", \"adjacent\": " + std::to_string(f.adjacent) + ", \"unresolved\": " + std::to_string(f.unresolved) +
            ", \"rules_applied\": " + JsonList(f.rules_applied) + ", \"rules_changed\": " + JsonBool(f.rules_changed) +
            ",\n        \"conflicts\": [";
        for (std::size_t i = 0; i < f.conflicts.size(); ++i)
        {
            const cm::PlanFile::Conflict& c = f.conflicts[i];
            out += (i ? ",\n          " : "\n          ") + std::string("{\"atom\": ") + Json(c.atom) + ", \"addons\": " +
                JsonList(c.addons) + ", \"winner\": " + Json(c.winner) + ", \"resolved_by\": " + Json(c.resolved_by) +
                ", \"lost\": " + JsonList(c.lost) + ", \"base_line\": " + std::to_string(c.base_line) + ", \"stale\": " +
                JsonBool(c.stale) + "}";
        }
        out += f.conflicts.empty() ? "]" : "\n        ]";
        out += ",\n        \"rebase_conflicts\": [";
        for (std::size_t i = 0; i < f.rebase_conflicts.size(); ++i)
        {
            const cm::PlanFile::RebaseConflict& c = f.rebase_conflicts[i];
            out += (i ? ",\n          " : "\n          ") + std::string("{\"atom\": ") + Json(c.atom) + ", \"addon\": " +
                Json(c.addon) + ", \"reason\": " + Json(c.reason) + ", \"note\": " + Json(c.note) + ", \"winner\": " +
                Json(c.winner) + ", \"resolved_by\": " + Json(c.resolved_by) + "}";
        }
        out += f.rebase_conflicts.empty() ? "]" : "\n        ]";
        out += ",\n        \"lines\": [";
        for (std::size_t i = 0; i < f.lines.size(); ++i)
        {
            out += (i ? ", " : "") + std::string("{\"from\": ") + std::to_string(f.lines[i].from) + ", \"to\": " +
                std::to_string(f.lines[i].to) + ", \"owner\": " + Json(f.lines[i].owner) + "}";
        }
        out += "]}";
    }
    return out + "}";
}

int CmdPlan(const Args& args)
{
    // One base: a folder, a snapshot (+ its texts) or a list of files
    const int bases = int(args.options.count("--base")) + int(args.options.count("--base-snapshot")) +
        int(args.options.count("--base-list"));
    if (!args.positional.empty() || bases != 1 || (!args.Has("--addons-dir") && !args.Has("--addon")) ||
        (args.Has("--base-texts") && !args.Has("--base-snapshot")) || args.options.count("--base-texts") > 1)
        return Usage();
    std::vector<std::string> warnings;
    std::error_code ec;
    PlanCli cli;
    cli.source = args.Has("--base-snapshot") ? "snapshot" : args.Has("--base-list") ? "list" : "folder";
    cli.base = args.Get(cli.source == "snapshot" ? "--base-snapshot" : cli.source == "list" ? "--base-list" : "--base");
    if (cli.source == "folder" && !fs::is_directory(PathOfUtf8(cli.base), ec))
    {
        std::fprintf(stderr, "gw_merge: plan: --base '%s' is not a folder\n", cli.base.c_str());
        return 2;
    }
    const std::string case_word = args.Get("--case",
#if defined(_WIN32)
        "windows"
#else
        "posix"
#endif
    );
    if (case_word != "windows" && case_word != "posix")
        return Usage();
    const bool lower_case = case_word == "windows";

    // 1. The addons: --addon in the order given, else the list (appdata\addons.ltx [addon.<id>] in order, enabled) over
    // the folders of --addons-dir, else every folder of --addons-dir with addon.ltx (by name)
    std::vector<std::string> addons_dirs;
    for (const auto& [name, value] : args.options)
    {
        if (name == "--addons-dir")
            addons_dirs.push_back(value);
    }
    std::vector<PlanAddonDir> found;
    std::vector<PlanIniSection> list;
    if (args.Has("--list"))
    {
        std::string bytes;
        if (!ReadFile(args.Get("--list"), bytes))
            return 2;
        list = PlanReadIni(bytes);
    }
    const bool listed = std::any_of(list.begin(), list.end(), [](const PlanIniSection& s) {
        const std::string name = cm::AsciiLower(s.name);
        return name == "addons" || name.compare(0, 6, "addon.") == 0;
    });
    if (args.Has("--addon"))
    {
        for (const auto& [name, value] : args.options)
        {
            PlanAddonDir addon;
            if (name == "--addon" && PlanReadAddon(PathOfUtf8(value), addon, warnings))
                found.push_back(std::move(addon));
        }
    }
    else if (listed)
    {
        const fs::path game_root = PathOfUtf8(args.Get("--game-root", "."));
        for (const PlanIniSection& section : list)
        {
            const std::string name = cm::AsciiLower(section.name);
            if (name.compare(0, 6, "addon.") != 0 || name.size() == 6)
                continue;
            const std::string id = name.substr(6);
            std::string path = PlanValue(&section, "path");
            fs::path dir;
            if (path.empty())
            {
                if (addons_dirs.empty())
                {
                    warnings.push_back("[addon." + id + "] has no path and no --addons-dir is given: skipped");
                    continue;
                }
                dir = PathOfUtf8(addons_dirs.front()) / PathOfUtf8(id);
            }
            else if (path.compare(0, 13, "$game_addons$") == 0)
            {
                std::string rest = path.substr(13);
                while (!rest.empty() && (rest[0] == '\\' || rest[0] == '/'))
                    rest.erase(0, 1);
                if (addons_dirs.empty())
                {
                    warnings.push_back("[addon." + id + "] path " + path + ": no --addons-dir for $game_addons$: skipped");
                    continue;
                }
                std::replace(rest.begin(), rest.end(), '\\', '/');
                dir = PathOfUtf8(addons_dirs.front()) / PathOfUtf8(rest);
            }
            else if (path[0] == '$')
            {
                warnings.push_back("[addon." + id + "] path " + path + ": an fsgame.ltx alias the tool does not know: skipped");
                continue;
            }
            else
            {
                std::replace(path.begin(), path.end(), '\\', '/');
                dir = PathOfUtf8(path);
                if (dir.is_relative())
                    dir = game_root / dir;
            }
            PlanAddonDir addon;
            if (!PlanReadAddon(dir, addon, warnings))
                continue;
            const std::string enabled = cm::AsciiLower(PlanValue(&section, "enabled"));
            addon.enabled = enabled.empty() || enabled == "true" || enabled == "1" || enabled == "yes";
            if (addon.id != id)
                warnings.push_back("[addon." + id + "]: the addon.ltx of " + Utf8(dir) + " names " + addon.id);
            found.push_back(std::move(addon));
        }
    }
    else
    {
        for (const std::string& root : addons_dirs)
        {
            std::vector<fs::path> dirs;
            for (fs::directory_iterator it(PathOfUtf8(root), ec), end; !ec && it != end; it.increment(ec))
            {
                std::error_code type_error;
                if (it->is_directory(type_error) && fs::is_regular_file(it->path() / "addon.ltx", type_error))
                    dirs.push_back(it->path());
            }
            std::sort(dirs.begin(), dirs.end());
            for (const fs::path& dir : dirs)
            {
                PlanAddonDir addon;
                if (PlanReadAddon(dir, addon, warnings))
                    found.push_back(std::move(addon));
            }
        }
    }
    const std::vector<PlanAddonDir> addons = PlanOrder(std::move(found));

    // 2. The stack (addon_slots.cpp BuildContentLayers): the common content of the working addons (not a location
    // addon), the sets of the chosen providers ([slots] of the list) in the order of their addons, the pack (--pack)
    const std::string pack_id = cm::AsciiLower(args.Get("--pack"));
    const PlanIniSection* slots = PlanFindSection(list, "slots");
    std::vector<cm::PlanLayer> layers;
    std::vector<cm::PlanLayer> sets;
    cm::PlanLayer pack_layer;
    for (const PlanAddonDir& addon : addons)
    {
        if (!addon.active)
            continue;
        const fs::path gamedata = PathOfUtf8(addon.dir) / "gamedata";
        const bool has_content = fs::is_directory(gamedata, ec);
        // A location addon is a pack only with gamedata\spawns\all.spawn (InitLevelPacks); without it its content is
        // common, as of any addon (addon_slots.cpp slots::Init)
        if (addon.type == "location" && fs::is_regular_file(gamedata / "spawns" / "all.spawn", ec))
        {
            if (addon.id == pack_id && has_content)
            {
                pack_layer.layer_id = "level_pack:" + addon.id;
                pack_layer.owner = addon.id;
                pack_layer.folder = PlanDirText(gamedata);
                pack_layer.private_masks = addon.private_masks;
                pack_layer.group = pack_layer.layer_id;
            }
            continue;
        }
        if (has_content)
        {
            cm::PlanLayer& layer = layers.emplace_back();
            layer.layer_id = "addon:" + addon.id;
            layer.owner = addon.id;
            layer.folder = PlanDirText(gamedata);
            layer.private_masks = addon.private_masks;
        }
        for (const auto& [slot, files] : addon.slots)
        {
            if (cm::AsciiLower(PlanValue(slots, slot)) != addon.id)
                continue; // not the chosen provider
            std::string folder = files.empty() ? "slots/" + slot : files;
            if (!PlanSlotFilesValid(folder))
            {
                // addon_slots.cpp AddProvider: the engine ignores the provider (and a folder outside the addon - a
                // drive, a root, '..' - must not be read)
                warnings.push_back("[addon:" + addon.id + "] [slot." + slot + "] files = " + files +
                    ": a folder inside the addon is expected, the provider is ignored");
                continue;
            }
            std::replace(folder.begin(), folder.end(), '\\', '/');
            const fs::path set = PathOfUtf8(addon.dir) / PathOfUtf8(folder) / "gamedata";
            if (!fs::is_directory(set, ec))
                continue;
            cm::PlanLayer& layer = sets.emplace_back();
            layer.layer_id = "slot:" + slot + ":" + addon.id;
            layer.owner = addon.id;
            layer.folder = PlanDirText(set);
            layer.private_masks = addon.private_masks;
        }
    }
    layers.insert(layers.end(), sets.begin(), sets.end());
    if (!pack_layer.layer_id.empty())
        layers.push_back(pack_layer);
    else if (!pack_id.empty())
        warnings.push_back("--pack " + pack_id + ": no working location addon of this id with gamedata\\");

    // 3. The rules of the player ([conflicts] of the list; addon_host.cpp: the first rule of a file / an atom counts)
    std::vector<cm::PairRule> pair_rules;
    std::vector<cm::FileRule> file_rules;
    std::vector<cm::PlayerAtomRule> atom_rules;
    if (const PlanIniSection* conflicts = PlanFindSection(list, "conflicts"))
    {
        for (const auto& [key, value] : conflicts->lines)
        {
            const std::string lower = cm::AsciiLower(key);
            if (lower.compare(0, 5, "pair:") == 0)
            {
                const std::string winner = cm::AsciiLower(PlanTrim(key.substr(5)));
                for (const std::string& loser : PlanIds(value))
                {
                    if (loser != winner)
                        pair_rules.push_back({winner, loser});
                }
            }
            else if (lower.compare(0, 5, "file:") == 0)
            {
                std::string file;
                if (!PlanRuleFile(PlanTrim(key.substr(5)), lower_case, file))
                    continue;
                if (std::none_of(file_rules.begin(), file_rules.end(), [&file](const cm::FileRule& r) { return r.file == file; }))
                    file_rules.push_back({file, cm::AsciiLower(value)});
            }
            else if (lower.compare(0, 5, "atom:") == 0)
            {
                const std::string rest = key.substr(5);
                const std::size_t bar = rest.find('|');
                std::string file, atom;
                if (bar == std::string::npos || !PlanRuleFile(PlanTrim(rest.substr(0, bar)), lower_case, file) ||
                    !PlanRuleAtom(PlanTrim(rest.substr(bar + 1)), atom))
                    continue;
                if (std::none_of(atom_rules.begin(), atom_rules.end(),
                        [&](const cm::PlayerAtomRule& r) { return r.file == file && r.atom == atom; }))
                    atom_rules.push_back({file, atom, cm::AsciiLower(value)});
            }
        }
    }
    cm::WalkRules walk{&PlanWalkSkips, nullptr, lower_case};
    if (cli.source == "folder")
    {
        // The root of the index (the engine: $game_data$, an absolute path): the folder of the base. A snapshot or a
        // list has none - the folder of a layer alone decides the length of a name (cm::WalkNameTooLong)
        std::error_code absolute_error;
        const fs::path absolute = fs::absolute(PathOfUtf8(cli.base), absolute_error);
        if (!absolute_error)
            walk.index_root = PlanDirText(absolute.lexically_normal()).size();
    }
    std::vector<cm::RuleStatus> statuses;
    cm::ApplyPairRules(layers, pair_rules, statuses);
    cm::ApplyFileRules(layers, file_rules, walk, statuses);

    // 4. The deltas (as addon_merge.cpp MergeReadDelta: <addon>\.gw_merge\<cm::DeltaFileNames()>)
    cm::PlanInput input;
    input.layers = layers;
    input.file_rules = file_rules;
    input.atom_rules = atom_rules;
    std::vector<cm::AddonDelta> deltas(addons.size());
    const std::vector<std::string> owners = cm::PlanOwners(layers);
    for (std::size_t i = 0; i < addons.size(); ++i)
    {
        const PlanAddonDir& a = addons[i];
        input.addons.push_back({a.id, a.dir, a.active, a.load_after, a.patches, a.game_version});
        input.deltas.push_back(nullptr);
        if (std::find(owners.begin(), owners.end(), a.id) == owners.end())
            continue;
        for (const std::string& name : cm::DeltaFileNames())
        {
            std::string bytes;
            if (!ReadPath(PathOfUtf8(a.dir) / ".gw_merge" / PathOfUtf8(name), bytes))
                continue;
            cm::ReadAddonDelta(".gw_merge\\" + name, bytes, deltas[i]);
            input.deltas.back() = &deltas[i];
            break;
        }
    }
    for (const cm::PlanLayer& layer : layers)
    {
        if (layer.owner.empty() || layer.writable)
            continue;
        const std::vector<std::string> names = cm::LayerTexts(layer.folder, walk);
        cli.layer_texts_all.insert(cli.layer_texts_all.end(), names.begin(), names.end());
        for (const std::string& name : names)
        {
            if (cm::LayerServes(layer, name, walk))
                cli.served_texts.push_back(name);
        }
    }
    std::sort(cli.layer_texts_all.begin(), cli.layer_texts_all.end());
    std::sort(cli.served_texts.begin(), cli.served_texts.end());
    const bool merge = args.Has("--merge");
    std::string base_texts; // the texts of a snapshot used: the tar, "inline", empty
    if (cli.source != "folder" && !PlanReadBase(args, lower_case, merge, cli, warnings, base_texts))
        return 2;
    // The tag of the game: --tag, else configs\gw_release.ltx of a folder, the tag of a snapshot ("game" -> "tag"),
    // "# game_tag" of a list
    std::string tag = args.Get("--tag");
    if (!args.Has("--tag"))
    {
        std::string release;
        if (cli.source == "folder" && ReadPath(PathOfUtf8(cli.base) / "configs" / "gw_release.ltx", release))
            tag = cm::ReleaseTagOf(release);
        else if (cli.source != "folder")
            tag = cli.index.game_tag;
    }
    input.game_tag = tag;
    cm::PlanEnv env;
    env.context = &cli;
    env.base = &PlanBase;
    if (cli.source != "folder")
        env.base_exists = &PlanBaseExists; // existence without a text: no_base_text
    env.is_patch_file = &PlanIsPatchFile;
    env.walk = walk;
    const bool all_files = args.Has("--all-files");
    input.all_files = all_files;

    cm::PlanResult plan;
    cm::PlanMerge(input, env, plan);

    // 5. The merge (--merge): the library as the engine calls it, without the cache
    LuaApi lua_api;
    cm::LuaChecker lua;
    if (merge && args.Has("--check-lua"))
    {
        if (!LoadLua(args.Get("--check-lua"), lua_api))
            return 2;
        lua.compile = &CompileLua;
        lua.context = &lua_api;
    }
    std::vector<std::string> text_hashes(plan.entries.size());
    std::vector<cm::PlanFile> records;
    for (std::size_t i = 0; i < plan.entries.size(); ++i)
    {
        cm::PlanEntry& entry = plan.entries[i];
        if (merge && entry.merge)
        {
            std::string text;
            std::vector<std::string> log;
            cm::MergePlanned(entry.item, entry.job, lua.compile ? &lua : nullptr, text, log);
            cm::RefineResolved(entry.item, input.addons, statuses);
            cm::CountPatches(entry.item);
            entry.item.served = cm::Serves(entry.item);
            for (const std::string& line : log)
                warnings.push_back(line);
            if (entry.item.served)
            {
                text_hashes[i] = cm::Sha256Hex(text);
                if (args.Has("--texts") && !PlanNameStaysInside(entry.item.file))
                    warnings.push_back("--texts: '" + entry.item.file + "' leaves the folder: not written");
                else if (args.Has("--texts"))
                {
                    std::string slashed = entry.item.file;
                    std::replace(slashed.begin(), slashed.end(), '\\', '/');
                    const fs::path out_path = PathOfUtf8(args.Get("--texts")) / PathOfUtf8(slashed);
                    fs::create_directories(out_path.parent_path(), ec);
                    if (!WriteFile(Utf8(out_path), text))
                        return 2;
                }
            }
        }
        for (const std::string& line : entry.log)
            warnings.push_back(line);
        if (!entry.extra)
            records.push_back(entry.item); // the statuses of the atom: rules as without --all-files
    }
    if (merge)
    {
        const std::vector<cm::RuleStatus> atoms = cm::AtomRuleStatuses(atom_rules, records);
        statuses.insert(statuses.end(), atoms.begin(), atoms.end());
    }

    // 6. The conflicts of the files (addon_content.cpp CollectContentConflicts over the folders): a name the public
    // layers of two addons give - a layer of one addon right over the layer of another that gives it; the patch files
    // are not conflicts; a file a file: rule took from a layer is one too
    std::map<std::string, std::vector<std::size_t>> givers; // name -> the layers that serve it, bottom up
    std::map<std::string, std::vector<std::size_t>> yielders;
    for (std::size_t i = 0; i < layers.size(); ++i)
    {
        const cm::PlanLayer& layer = layers[i];
        if (layer.owner.empty() || layer.writable)
            continue;
        // Every file of the layer as layer_walk mounts it (cm::LayerFiles): a name too long for the index is left out
        // with the warning of the engine
        std::vector<std::string> too_long;
        for (const std::string& name : cm::LayerFiles(layer.folder, walk, &too_long))
        {
            if (cm::LayerServes(layer, name, walk))
                givers[name].push_back(i);
        }
        for (const std::string& path : too_long)
            warnings.push_back("FS: layer '" + layer.layer_id + "': name too long (" +
                std::to_string(cm::kIndexPathBytes) + " bytes and more), skipped [" + path + "]");
        for (const std::string& name : layer.yielded)
            yielders[name].push_back(i);
    }
    std::map<std::string, std::vector<std::size_t>> conflict_layers;
    for (const auto& [name, serving] : givers)
    {
        if (serving.size() < 2 || cm::IsPatchFile(name, cm::FileExists{&PlanFileExists, &cli}))
            continue;
        for (std::size_t k = 1; k < serving.size(); ++k)
        {
            if (layers[serving[k - 1]].owner == layers[serving[k]].owner)
                continue;
            std::vector<std::size_t>& list_of = conflict_layers[name];
            for (const std::size_t index : {serving[k - 1], serving[k], serving.back()})
            {
                if (std::find(list_of.begin(), list_of.end(), index) == list_of.end())
                    list_of.push_back(index);
            }
        }
    }
    for (const auto& [name, yielding] : yielders)
    {
        std::vector<std::size_t>& list_of = conflict_layers[name];
        const auto served = givers.find(name);
        if (list_of.empty() && served != givers.end())
            list_of.push_back(served->second.back());
        for (const std::size_t index : yielding)
        {
            if (std::find(list_of.begin(), list_of.end(), index) == list_of.end())
                list_of.push_back(index);
        }
    }
    std::string conflicts_json = "[";
    std::size_t conflict_count = 0;
    for (auto& [name, indices] : conflict_layers)
    {
        std::sort(indices.begin(), indices.end());
        std::vector<std::string> layer_ids, layer_owners;
        for (const std::size_t index : indices)
        {
            layer_ids.push_back(layers[index].layer_id);
            layer_owners.push_back(layers[index].owner);
        }
        const auto served = givers.find(name);
        const auto record = std::find_if(plan.entries.begin(), plan.entries.end(),
            [&name](const cm::PlanEntry& entry) { return entry.item.file == name; });
        // no_base_text: the engine has the text of the base and merges the file - as a plan without --merge tells it
        const bool planned = record != plan.entries.end() && (record->merge || record->item.note == "no_base_text");
        const bool checked = planned && merge && record->merge; // merged here: served or not is known
        const bool merged_serves = planned && (!checked || record->item.served);
        std::string winner, resolved_by, note;
        std::string fallback_winner, fallback_resolved_by; // merged_serves: who serves it when the merge does not hold
        const bool has_base = PlanBaseHas(cli, name);
        const std::string top = served != givers.end() ? layers[served->second.back()].layer_id : std::string();
        if (merged_serves)
        {
            winner = "addons_merged";
            resolved_by = "merge";
            if (checked && record->item.unresolved)
                note = std::to_string(record->item.unresolved) + " conflict(s) of atoms decided by the order";
            else if (record->item.note == "no_base_text")
                note = "no text of the base here: the engine merges the file";
            // A fallback (the merged text fails its check, the layer does not mount): the upper layer serves the file,
            // decided as a conflict of a file that is not merged
            std::string fallback_note;
            fallback_winner = !top.empty() ? top : has_base ? "base" : "none";
            cm::ResolveFileConflict(name, layer_ids, layer_owners, top, file_rules, pair_rules, statuses,
                input.addons, fallback_resolved_by, fallback_note);
        }
        else
        {
            winner = !top.empty() ? top : has_base ? "base" : "none";
            cm::ResolveFileConflict(name, layer_ids, layer_owners, top, file_rules, pair_rules, statuses,
                input.addons, resolved_by, note);
            if (record != plan.entries.end() && merge && record->item.status == "fallback")
                note += (note.empty() ? "" : "; ") + std::string("not merged: ") +
                    (record->item.error.empty() ? record->item.note : record->item.error);
            if (top.empty() && !has_base)
                note += (note.empty() ? "" : "; ") + std::string("the base has no such file: it is not in the game now");
        }
        conflicts_json += (conflict_count++ ? ",\n    " : "\n    ") + std::string("{\"file\": ") + Json(name) +
            ", \"text\": " + JsonBool(cm::KindOfName(name) != cm::Kind::Unknown) + ", \"layers\": " + JsonList(layer_ids) +
            ", \"owners\": " + JsonList(layer_owners) + ", \"winner\": " + Json(winner) + ", \"resolved_by\": " +
            Json(resolved_by) + ", \"note\": " + Json(note) + (merged_serves && !checked ? ", \"merge_checked\": false" : "") +
            (merged_serves ? ", \"fallback_winner\": " + Json(fallback_winner) + ", \"fallback_resolved_by\": " +
                    Json(fallback_resolved_by) :
                             std::string()) +
            "}";
    }
    conflicts_json += conflict_count ? "\n  ]" : "]";

    // 7. The JSON
    std::string out = "{\n  \"plan_format\": " + std::to_string(kPlanFormat) + ",\n  \"algorithm\": " +
        Json(cm::kAlgorithmVersion) + ",\n  \"revision\": " + std::to_string(cm::kResultRevision) +
        ",\n  \"case\": " + Json(case_word) + ",\n  \"base\": " + Json(cli.base) + ",\n  \"base_source\": " +
        Json(cli.source) + ",\n  \"base_texts\": " + Json(base_texts) + ",\n  \"base_files\": " +
        std::to_string(cli.index.files.size()) + ",\n  \"game_tag\": " + Json(tag) +
        ",\n  \"merged\": " + JsonBool(merge) + (all_files ? ",\n  \"all_files\": true" : "") +
        ",\n  \"addons\": [";
    for (std::size_t i = 0; i < addons.size(); ++i)
    {
        const PlanAddonDir& a = addons[i];
        const cm::AddonDelta& d = deltas[i];
        std::string inconsistent = "{";
        for (std::size_t k = 0; k < d.inconsistent.size(); ++k)
            inconsistent += (k ? ", " : "") + Json(d.inconsistent[k]) + ": " + Json(d.inconsistent_why[k]);
        inconsistent += "}";
        const bool guessed =
            std::find(plan.guessed_addons.begin(), plan.guessed_addons.end(), a.id) != plan.guessed_addons.end();
        out += (i ? ",\n    " : "\n    ") + std::string("{\"id\": ") + Json(a.id) + ", \"dir\": " + Json(a.dir) +
            ", \"type\": " + Json(a.type) + ", \"enabled\": " + JsonBool(a.enabled) + ", \"active\": " +
            JsonBool(a.active) + ", \"reason\": " + Json(a.reason) + ",\n      \"load_after\": " + JsonList(a.load_after) +
            ", \"depends\": " + JsonList(a.depends) + ", \"patches\": " + JsonList(a.patches) + ", \"game_version\": " +
            Json(a.game_version) + ", \"private\": " + JsonList(a.private_masks) + ", \"guessed\": " + JsonBool(guessed) +
            ",\n      \"delta\": " +
            (d.file.empty() ? std::string("null") :
                "{\"file\": " + Json(d.file) + ", \"stale\": " + JsonBool(d.stale) + ", \"error\": " + Json(d.error) +
                    ", \"algo\": " + Json(d.delta.algo) + ", \"game\": " + Json(d.delta.game) + ", \"inconsistent\": " +
                    inconsistent + "}") +
            "}";
    }
    out += addons.empty() ? "],\n" : "\n  ],\n";
    out += "  \"layers\": [";
    for (std::size_t i = 0; i < layers.size(); ++i)
    {
        const cm::PlanLayer& l = layers[i];
        out += (i ? ",\n    " : "\n    ") + std::string("{\"id\": ") + Json(l.layer_id) + ", \"owner\": " + Json(l.owner) +
            ", \"folder\": " + Json(l.folder) + ", \"pack\": " + JsonBool(!l.group.empty()) + ", \"yielded\": " +
            JsonList(l.yielded) + "}";
    }
    out += layers.empty() ? "],\n" : "\n  ],\n";
    out += "  \"rules\": " + JsonStatuses(statuses) + ",\n";
    out += "  \"guessed_addons\": " + JsonList(plan.guessed_addons) + ",\n";
    out += "  \"files\": [";
    for (std::size_t i = 0; i < plan.entries.size(); ++i)
        out += (i ? ",\n    " : "\n    ") +
            JsonPlanFile(plan.entries[i], layers, merge, text_hashes[i], PlanBaseSha256(cli, plan.entries[i].item.file));
    out += plan.entries.empty() ? "],\n" : "\n  ],\n";
    out += "  \"conflicts\": " + conflicts_json + ",\n";
    const std::string base_word = cli.source == "folder" ? "base: a folder, no archives" :
        cli.source == "list" ? "base: a list of files, no texts (no_base_text: not merged)" :
        cli.texts_on ? "base: a snapshot, the texts rebuilt from the normalized ones" :
                       "base: a snapshot without texts (no_base_text: not merged)";
    out += "  \"approximations\": [\"state: enabled + depends / patches active (no api_min, version conditions, plugins)\", "
           "\"pack: --pack\", " + Json(base_word) + ", \"no writes of a pack\"],\n";
    out += "  \"warnings\": " + JsonList(warnings) + "\n}\n";
    return WriteFile(args.Get("-o", "-"), out) ? 0 : 2;
}

// The versions of the library (ContentMerge.h): algorithm - the version of the parse (the deltas), revision - of the
// results within it (a cache of the results of a tool is keyed by both, as the cache of the engine)
int CmdVersion(const Args& args)
{
    if (!args.positional.empty())
        return Usage();
    const std::string out = "{\n  \"algorithm\": " + Json(cm::kAlgorithmVersion) + ",\n  \"revision\": " +
        std::to_string(cm::kResultRevision) + ",\n  \"delta_format\": " + std::to_string(cm::kDeltaFormat) + "\n}\n";
    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
#if defined(_WIN32)
    // The arguments as UTF-8 from the UTF-16 command line (argv is in the ANSI code page: the names out of it are lost)
    std::vector<std::string> utf8_args;
    std::vector<char*> utf8_argv;
    int wargc = 0;
    if (wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc))
    {
        for (int i = 0; i < wargc; ++i)
        {
            const int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string arg(std::size_t(len > 0 ? len : 1), '\0');
            if (len > 0)
                WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, arg.data(), len, nullptr, nullptr);
            arg.resize(std::strlen(arg.c_str()));
            utf8_args.push_back(std::move(arg));
        }
        LocalFree(wargv);
        for (std::string& arg : utf8_args)
            utf8_argv.push_back(arg.data());
        utf8_argv.push_back(nullptr);
        argc = wargc;
        argv = utf8_argv.data();
    }
#endif
    if (argc < 2)
        return Usage();
#if defined(_WIN32)
    std::setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
    _setmode(_fileno(stdout), _O_BINARY); // the text goes out byte to byte
#endif
    Args args;
    if (!ParseArgs(argc, argv, args))
        return Usage();
    const std::string cmd = argv[1];
    if (!KnownOptions(cmd, args))
        return 2;
    if (cmd == "atoms")
        return CmdAtoms(args);
    if (cmd == "merge")
        return CmdMerge(args);
    if (cmd == "rebase")
        return CmdRebase(args);
    if (cmd == "suggest")
        return CmdSuggest(args);
    if (cmd == "check")
        return CmdCheck(args);
    if (cmd == "segments")
        return CmdSegments(args);
    if (cmd == "lines")
        return CmdLines(args);
    if (cmd == "targets")
        return CmdTargets(args);
    if (cmd == "delta")
        return CmdDelta(args);
    if (cmd == "apply")
        return CmdApply(args);
    if (cmd == "version")
        return CmdVersion(args);
    if (cmd == "plan")
        return CmdPlan(args);
    return Usage();
}
