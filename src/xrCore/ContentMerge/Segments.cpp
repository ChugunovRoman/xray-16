// The common part of the parsers: the dispatch by kind, the whole text as one gap, the coverage check, the dump.

#include "Segments.h"

#include <algorithm>

namespace xray::content_merge
{
std::uint32_t LineOf(std::string_view text, std::uint32_t pos)
{
    std::uint32_t line = 1;
    const std::size_t end = pos < text.size() ? pos : text.size();
    for (std::size_t i = 0; i < end; ++i)
        line += text[i] == '\n';
    return line;
}

LineIndex::LineIndex(std::string_view text)
{
    starts_.push_back(0);
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\n')
            starts_.push_back(std::uint32_t(i + 1));
    }
}

std::uint32_t LineIndex::Of(std::uint32_t pos) const
{
    const auto it = std::upper_bound(starts_.begin(), starts_.end(), pos);
    return std::uint32_t(it - starts_.begin());
}

Doc ParseKind(Kind kind, std::string_view text, std::string_view name)
{
    switch (kind)
    {
    case Kind::Ltx: return ParseLtx(text);
    case Kind::Xml: return ParseXml(text);
    case Kind::Lua: return ParseLua(text, ModuleOfName(name));
    case Kind::Unknown: break;
    }
    return WholeTextDoc(text, ModuleOfName(name));
}

Doc WholeTextDoc(std::string_view text, std::string_view label)
{
    Doc doc;
    if (text.empty())
        return doc;
    Seg seg;
    seg.gap = true;
    seg.divisible = true;
    seg.begin = 0;
    seg.end = std::uint32_t(text.size());
    seg.line = 1;
    seg.label = std::string(label);
    seg.cmp = std::string(text);
    doc.top.push_back(std::move(seg));
    return doc;
}

namespace
{
bool CheckCoverageList(const std::vector<Seg>& list, std::uint32_t begin, std::uint32_t end, std::string& error)
{
    std::uint32_t pos = begin;
    for (const Seg& seg : list)
    {
        if (seg.begin != pos || seg.end < seg.begin)
        {
            error = "segment '" + seg.label + "' [" + std::to_string(seg.begin) + ", " + std::to_string(seg.end) +
                ") does not start at " + std::to_string(pos);
            return false;
        }
        if (!seg.children.empty() && !CheckCoverageList(seg.children, seg.begin, seg.end, error))
            return false;
        pos = seg.end;
    }
    if (pos != end)
    {
        error = "the segments end at " + std::to_string(pos) + ", not at " + std::to_string(end);
        return false;
    }
    return true;
}

void DumpEscaped(std::string& out, std::string_view s, std::size_t limit)
{
    for (std::size_t i = 0; i < s.size() && i < limit; ++i)
    {
        const char ch = s[i];
        if (ch == '\n')
            out += "\\n";
        else if (ch == '\t')
            out += "\\t";
        else if (ch == '\x1e')
            out += '=';
        else if (ch == '\x1f')
            out += '|';
        else
            out += ch;
    }
    if (s.size() > limit)
        out += "...";
}
} // namespace

bool CheckCoverage(const Doc& doc, std::string_view text, std::string& error)
{
    return CheckCoverageList(doc.top, 0, std::uint32_t(text.size()), error);
}

std::string DumpSegments(const std::vector<Seg>& list, std::string_view text, int depth)
{
    std::string out;
    for (const Seg& seg : list)
    {
        out += std::string(std::size_t(depth) * 2, ' ');
        out += seg.gap ? "gap " : (seg.children.empty() ? "atom " : "node ");
        out += seg.gap ? seg.label : seg.key;
        out += "  line " + std::to_string(seg.line);
        out += seg.divisible ? "  div" : "";
        out += "  cmp: ";
        DumpEscaped(out, seg.cmp, 60);
        out += "  text: ";
        DumpEscaped(out, text.substr(seg.begin, seg.end - seg.begin), 60);
        out += '\n';
        if (!seg.children.empty())
            out += DumpSegments(seg.children, text, depth + 1);
    }
    return out;
}
} // namespace xray::content_merge
