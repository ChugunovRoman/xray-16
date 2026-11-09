#pragma once

// The segments of a text file (15-merge-implementation.md section 3.2): the parse without losses. A text is split
// into segments - the atoms and the gaps between them (what the parser did not take for an atom). The segments of a
// list cover their range exactly, one after another: joined, they give the text byte to byte. The comments and the
// empty lines before an atom belong to it (a doc comment goes with its function). A container (an ltx section, an
// xml node) is a segment whose children are a list of their own.

#include "ContentMerge.h"

#include <string>
#include <string_view>
#include <vector>

namespace xray::content_merge
{
struct Seg
{
    std::string key; // unique among the siblings; a gap gets its key at the merge (by its anchor), here it is empty
    std::string label; // the atom name in the reports
    std::uint32_t begin{}; // [begin, end) in the text, the leading comments included
    std::uint32_t end{};
    std::uint32_t line{}; // 1-based line of the atom itself (after the leading comments)
    std::string cmp; // the compared form: two versions with one form are the same (comments, spaces dropped)
    bool gap{};
    bool divisible{}; // two sides changed it differently - the merge by lines inside (a function, a multi-line value)
    bool function{}; // Lua: a function definition (function a.b(), a.b = function(), local function f())
    std::vector<Seg> children; // a container: the children cover [begin, end) exactly
};

struct Doc
{
    std::vector<Seg> top; // covers the whole text
    bool ok{true}; // false - the parser did not understand the text (an unclosed long comment...): merge by lines
    std::string error;
    // The gaps of the top list are code that runs (Lua: the calls, if ... end between the functions): the merge
    // must never repeat it when the sides split or join the gaps (AtomMerge.cpp, the regions)
    bool code_gaps{};
};

// The module name of a Lua script (the file name without the folder and the extension, lower case)
std::string ModuleOfName(std::string_view name);

Doc ParseLtx(std::string_view text);
Doc ParseXml(std::string_view text);
Doc ParseLua(std::string_view text, std::string_view module);
Doc ParseKind(Kind kind, std::string_view text, std::string_view name);

// The whole text as one divisible gap (the merge by lines)
Doc WholeTextDoc(std::string_view text, std::string_view label);

// The checks of Validate (ContentMerge.h)
bool ValidateLtx(std::string_view text, std::string& error);
bool ValidateXml(std::string_view text, std::string& error);
bool ValidateLua(std::string_view text, std::string& error);

// 1-based line of a position (a scan from the start: for a few calls)
std::uint32_t LineOf(std::string_view text, std::uint32_t pos);

// 1-based line of a position by the line starts (for many calls on one text)
class LineIndex
{
public:
    explicit LineIndex(std::string_view text);
    std::uint32_t Of(std::uint32_t pos) const;

private:
    std::vector<std::uint32_t> starts_;
};

// Debug: the tree as text (gw_merge segments, the tests)
std::string DumpSegments(const std::vector<Seg>& list, std::string_view text, int depth = 0);
// The check of the parse without losses: the children cover their parents, the top covers the text
bool CheckCoverage(const Doc& doc, std::string_view text, std::string& error);
} // namespace xray::content_merge
