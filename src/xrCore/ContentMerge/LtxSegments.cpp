// ltx -> segments (15-merge-implementation.md section 3.2), the lines read as CInifile::Load reads them
// (xrCore/xr_ini.cpp) and tools/addons_snapshot/ltxparse.py ports:
//   - a line ends at LF or a lone CR (IReader::r_string); it is trimmed of the chars <= ' ' (_Trim);
//   - a comment: ';' anywhere, '//' only when the first '/' of the line starts it and it comes before ';'; a comment
//     char between the first pair of quotes does not cut the line;
//   - '[name]:parents' starts a section (the name in lower case); a DLTX header ('![s]', '!![s]') in a file that is
//     not a patch is skipped by the engine with its keys: a gap here;
//   - 'key = value' / 'key': a value with an odd number of quotes goes on over the next lines until the quotes are
//     balanced, unless a line with '[...]' or the end of the file comes first (the engine goes back then); the value
//     is compared as _parse keeps it (no spaces outside the quotes), "key =" is "key";
//   - '#include "x"' - an atom at its place (in a section or before the first one);
//   - the lines before the first section are ignored by the engine: a gap;
//   - a key repeated in a section: the last one is in effect, the earlier ones are gaps.
// Atoms: "[sec]" (the whole section: new, deleted), "[sec]:parents" (the header), "[sec] key", "#include "x"".

#include "Segments.h"
#include "TextNorm.h"

#include <map>

namespace xray::content_merge
{
namespace
{
struct LtxLine
{
    std::uint32_t begin; // the first char
    std::uint32_t end; // the terminator (or the end of the text)
    std::uint32_t next; // the first char of the next line
    std::uint32_t no; // 1-based, counted by LF (the parts of a line split by a lone CR share it)
};

std::vector<LtxLine> LtxSplitLines(std::string_view text)
{
    std::vector<LtxLine> lines;
    std::uint32_t no = 1;
    std::uint32_t pos = 0;
    const auto size = std::uint32_t(text.size());
    while (pos < size)
    {
        std::uint32_t e = pos;
        while (e < size && text[e] != '\n' && text[e] != '\r')
            ++e;
        std::uint32_t next = e;
        bool lf = false;
        if (next < size)
        {
            if (text[next] == '\r' && next + 1 < size && text[next + 1] == '\n')
            {
                next += 2;
                lf = true;
            }
            else
            {
                lf = text[next] == '\n';
                ++next;
            }
        }
        lines.push_back({pos, e, next, no});
        if (lf)
            ++no;
        pos = next;
    }
    return lines;
}

// The line without its comment, as CInifile::Load cuts it (s is trimmed already)
std::string_view LtxStripComment(std::string_view s)
{
    std::size_t comm = s.find(';');
    const std::size_t slash = s.find('/');
    if (slash != std::string_view::npos && slash + 1 < s.size() && s[slash + 1] == '/' &&
        (comm == std::string_view::npos || slash < comm))
        comm = slash;
    if (comm == std::string_view::npos)
        return s;
    bool in_quot = false;
    const std::size_t q1 = s.find('"');
    if (q1 != std::string_view::npos && q1 < comm)
    {
        const std::size_t q2 = s.find('"', q1 + 1);
        if (q2 != std::string_view::npos && q2 > comm)
            in_quot = true;
    }
    return in_quot ? s : s.substr(0, comm);
}

std::size_t LtxCountQuotes(std::string_view s)
{
    std::size_t n = 0;
    for (const char ch : s)
        n += ch == '"';
    return n;
}

bool LtxIsDltxHeader(std::string_view s)
{
    return s.size() >= 2 && s[0] == '!' && (s[1] == '[' || (s[1] == '!' && s.size() >= 3 && s[2] == '['));
}

// The items of a list as _GetItemCount + _GetItem give them (xr_trims.cpp)
std::vector<std::string> LtxEngineItems(std::string_view src)
{
    std::vector<std::string> out;
    if (src.empty())
        return out;
    std::size_t cnt = 0;
    std::size_t pos = 0;
    for (;;)
    {
        const std::size_t p = src.find(',', pos);
        if (p == std::string_view::npos)
            break;
        pos = p + 1;
        ++cnt;
        if (pos < src.size() && src[pos] == ',')
            break;
    }
    if (pos < src.size())
        ++cnt;
    std::size_t start = 0;
    for (std::size_t k = 0; k < cnt; ++k)
    {
        std::size_t stop = src.find(',', start);
        if (stop == std::string_view::npos)
            stop = src.size();
        const String item = text::ToLower(text::Trim(src.substr(start, stop - start)));
        out.emplace_back(item.data(), item.size());
        start = stop + 1;
        if (start > src.size())
            break;
    }
    return out;
}

// The value of a key as CInifile::Load keeps it (_parse of xr_ini.cpp): the ASCII spaces (' ', '\t', '\r', '\n')
// outside the quotes dropped, the text between the quotes as is. "a, b" and "a,b" are one value for the engine, so
// they are for the merge. unclosed: the quotes are not balanced (the engine keeps the first line, '"' trimmed at its
// ends). An empty value is no value: "key =" is "key"
std::string LtxEngineValue(std::string_view raw, bool unclosed)
{
    std::string out;
    out.reserve(raw.size());
    bool inside = false;
    for (const char ch : raw)
    {
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
        {
            if (inside)
                out += ch;
            continue;
        }
        if (ch == '"')
            inside = !inside;
        out += ch;
    }
    if (unclosed)
    {
        // _Trim(prevStr, '"'): the bytes <= '"' at both ends (the spaces inside the quotes and '!' too)
        const auto trimmed = [](char ch) { return static_cast<unsigned char>(ch) <= '"'; };
        std::size_t first = 0;
        std::size_t last = out.size();
        while (first < last && trimmed(out[first]))
            ++first;
        while (last > first && trimmed(out[last - 1]))
            --last;
        out = out.substr(first, last - first);
    }
    return out;
}

std::string LtxIncludeName(std::string_view s)
{
    const std::size_t q1 = s.find('"');
    if (q1 == std::string_view::npos)
        return {};
    std::size_t q2 = s.find('"', q1 + 1);
    if (q2 == std::string_view::npos)
        q2 = s.size();
    const std::string_view name = text::Trim(s.substr(q1 + 1, q2 - q1 - 1));
    return std::string(name);
}

// Unique keys among the siblings: the second "x" is "x#2"
void LtxUniqueKey(std::map<std::string, int>& seen, std::string& key)
{
    const int n = ++seen[key];
    if (n > 1)
        key += "#" + std::to_string(n);
}

std::string LtxSectionCmp(const Seg& section)
{
    std::string cmp;
    for (const Seg& child : section.children)
    {
        cmp += child.gap ? std::string("~") : child.key;
        cmp += '\x1e';
        cmp += child.cmp;
        cmp += '\x1f';
    }
    return cmp;
}

struct LtxParser
{
    std::string_view text;
    std::vector<LtxLine> lines;
    Doc doc;
    std::vector<std::string> errors; // for Validate

    // The section open now (an index into doc.top), -1 - none
    int section = -1;
    bool skipping = false; // after a DLTX header: the lines go to a top gap
    std::map<std::string, int> top_keys;
    std::map<std::string, int> child_keys;
    std::map<std::string, int> sections_seen; // for Validate: a repeated section
    std::string section_name;

    std::uint32_t pending = UINT32_MAX; // the start of the comments / empty lines waiting for the next segment
    // The gap open now
    bool gap_open = false;
    bool gap_in_section = false;
    Seg gap;

    std::vector<Seg>& List(bool in_section) { return in_section ? doc.top[std::size_t(section)].children : doc.top; }

    std::uint32_t TakeBegin(std::uint32_t line_begin)
    {
        const std::uint32_t b = pending != UINT32_MAX ? pending : line_begin;
        pending = UINT32_MAX;
        return b;
    }

    void CloseGap()
    {
        if (!gap_open)
            return;
        gap_open = false;
        List(gap_in_section).push_back(std::move(gap));
        gap = Seg{};
    }

    void CloseSection()
    {
        CloseGap();
        if (section < 0)
            return;
        Seg& sec = doc.top[std::size_t(section)];
        // A key repeated in the section: the last one is in effect, the earlier ones are gaps
        std::map<std::string, std::size_t> last;
        for (std::size_t i = 0; i < sec.children.size(); ++i)
        {
            const Seg& child = sec.children[i];
            if (!child.gap && child.key.rfind("#include", 0) != 0 && child.key != ":header")
                last[child.key] = i;
        }
        for (std::size_t i = 0; i < sec.children.size(); ++i)
        {
            Seg& child = sec.children[i];
            if (child.gap || child.key.rfind("#include", 0) == 0 || child.key == ":header")
                continue;
            if (last[child.key] != i)
            {
                child.gap = true;
                child.divisible = true;
                child.cmp = child.key + "=" + child.cmp;
                child.key.clear();
            }
        }
        sec.end = sec.children.back().end;
        sec.cmp = LtxSectionCmp(sec);
        section = -1;
        child_keys.clear();
    }

    void GapLine(const LtxLine& line, std::string_view content)
    {
        const bool in_section = section >= 0 && !skipping;
        if (gap_open && gap_in_section != in_section)
            CloseGap();
        if (!gap_open)
        {
            gap_open = true;
            gap_in_section = in_section;
            gap.gap = true;
            gap.divisible = true;
            gap.begin = TakeBegin(line.begin);
            gap.line = line.no;
            gap.label = in_section ? "[" + section_name + "]" : std::string();
        }
        gap.end = line.next;
        if (!gap.cmp.empty())
            gap.cmp += '\n';
        gap.cmp += std::string(content);
    }

    void Run()
    {
        lines = LtxSplitLines(text);
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            const LtxLine& line = lines[i];
            const std::string_view raw = text.substr(line.begin, line.end - line.begin);
            const std::string_view s = LtxStripComment(text::Trim(raw));
            if (text::Trim(s).empty())
            {
                // A comment or an empty line: it goes with the next segment
                CloseGap();
                if (pending == UINT32_MAX)
                    pending = line.begin;
                continue;
            }
            if (s[0] == '#' && s.find("#include") != std::string_view::npos)
            {
                CloseGap();
                const bool in_section = section >= 0 && !skipping;
                Seg inc;
                inc.begin = TakeBegin(line.begin);
                inc.end = line.next;
                inc.line = line.no;
                inc.cmp = LtxIncludeName(s);
                inc.key = "#include \"" + inc.cmp + "\"";
                inc.label = in_section ? "[" + section_name + "] " + inc.key : inc.key;
                LtxUniqueKey(in_section ? child_keys : top_keys, inc.key);
                List(in_section).push_back(std::move(inc));
                continue;
            }
            if (LtxIsDltxHeader(s))
            {
                // Skipped by the engine with the keys after it: a top gap until the next section
                CloseSection();
                skipping = true;
                GapLine(line, text::Trim(s));
                continue;
            }
            if (s[0] == '[')
            {
                CloseSection();
                skipping = false;
                const std::size_t close = s.find(']');
                if (close == std::string_view::npos)
                {
                    errors.push_back("a section header without ']' (line " + std::to_string(line.no) + ")");
                    GapLine(line, text::Trim(s));
                    continue;
                }
                const String lname = text::ToLower(s.substr(1, close - 1));
                section_name.assign(lname.data(), lname.size());
                if (++sections_seen[section_name] > 1)
                    errors.push_back("duplicate section [" + section_name + "] (line " + std::to_string(line.no) + ")");
                std::string parents;
                const std::size_t inh = s.find("]:");
                if (inh != std::string_view::npos)
                {
                    for (const std::string& parent : LtxEngineItems(s.substr(inh + 2)))
                    {
                        if (!parents.empty())
                            parents += ',';
                        parents += parent;
                    }
                }
                Seg header;
                header.begin = TakeBegin(line.begin);
                header.end = line.next;
                header.line = line.no;
                header.key = ":header";
                header.label = "[" + section_name + "]:parents";
                header.cmp = (inh != std::string_view::npos ? "parents=" : "noparents") + parents;
                Seg sec;
                sec.begin = header.begin;
                sec.line = line.no;
                sec.key = "[" + section_name + "]";
                sec.label = sec.key;
                LtxUniqueKey(top_keys, sec.key);
                sec.children.push_back(std::move(header));
                doc.top.push_back(std::move(sec));
                section = int(doc.top.size() - 1);
                continue;
            }
            if (section < 0 || skipping)
            {
                GapLine(line, text::Trim(s));
                continue;
            }
            // key = value / key
            std::string key;
            std::string value;
            std::size_t last_line = i;
            const std::size_t eq = s.find('=');
            if (eq != std::string_view::npos)
            {
                key = std::string(text::Trim(s.substr(0, eq)));
                std::string value_raw(s.substr(eq + 1));
                bool unclosed = false;
                if (LtxCountQuotes(value_raw) % 2 == 1)
                {
                    std::size_t j = i + 1;
                    bool closed = false;
                    while (j < lines.size())
                    {
                        const std::string_view nxt = text.substr(lines[j].begin, lines[j].end - lines[j].begin);
                        const std::size_t b = nxt.find('[');
                        if (b != std::string_view::npos && nxt.find(']', b) != std::string_view::npos)
                            break;
                        value_raw += '\n';
                        value_raw += std::string(nxt);
                        if (LtxCountQuotes(value_raw) % 2 == 0)
                        {
                            closed = true;
                            break;
                        }
                        ++j;
                    }
                    if (closed)
                        last_line = j;
                    else
                    {
                        errors.push_back("an odd number of quotes in [" + section_name + "] " + key + " (line " +
                            std::to_string(line.no) + ")");
                        value_raw = std::string(s.substr(eq + 1));
                        unclosed = true;
                    }
                }
                // compared as the engine reads it (the line ends inside the quotes are '\n' here, "\r\n" in the
                // engine: the same for every version)
                const std::string parsed = LtxEngineValue(value_raw, unclosed);
                if (!parsed.empty())
                    value = "=" + parsed;
            }
            else
                key = std::string(text::Trim(s));
            if (key.empty())
            {
                // "= value": no item for the engine, the lines of its value are read with it all the same
                GapLine(line, text::Trim(s));
                for (std::size_t k = i + 1; k <= last_line; ++k)
                    GapLine(lines[k], text.substr(lines[k].begin, lines[k].end - lines[k].begin));
                i = last_line;
                continue;
            }
            CloseGap();
            Seg item;
            item.begin = TakeBegin(line.begin);
            item.end = lines[last_line].next;
            item.line = line.no;
            item.key = key;
            item.label = "[" + section_name + "] " + key;
            item.cmp = value;
            item.divisible = last_line != i;
            List(true).push_back(std::move(item));
            i = last_line;
        }
        CloseSection();
        CloseGap();
        if (pending != UINT32_MAX)
        {
            // The comments and empty lines at the end of the file
            Seg tail;
            tail.gap = true;
            tail.divisible = true;
            tail.begin = pending;
            tail.end = std::uint32_t(text.size());
            tail.line = LineOf(text, pending);
            doc.top.push_back(std::move(tail));
        }
        // The repeated keys are dedupped per section above; the unique keys of the items with "#2" are not needed:
        // a repeated key is a gap. The top gaps / sections keep their order.
    }
};
} // namespace

Doc ParseLtx(std::string_view text)
{
    LtxParser parser;
    parser.text = text;
    parser.Run();
    return std::move(parser.doc);
}

bool ValidateLtx(std::string_view text, std::string& error)
{
    LtxParser parser;
    parser.text = text;
    parser.Run();
    if (parser.errors.empty())
        return true;
    error = parser.errors.front();
    return false;
}
} // namespace xray::content_merge
