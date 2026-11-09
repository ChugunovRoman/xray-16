// xml -> segments (15-merge-implementation.md section 3.2). A lenient tag scanner (as tools/addons_snapshot/xmlscan.py)
// with the offsets in the text: comments, CDATA, <? ?> and <! > are skipped; an unclosed tag is closed by the end tag
// of its parent; a document may have no single root (the engine wraps it); '#include "x"' lines are text.
// A node is a container: "<tag" (its key), the attributes one by one, ">" or "/>", the body (the child nodes and the
// text between them), "</tag>". A child node is keyed by its tag and "id", else "name" (XmlOverrideKey of
// xrCore/XML/XMLDocument.cpp), else by its number among the children of the tag without a key. The atom names are the
// ones of XmlOverrideLabel: "w/main_wnd/btn[id=x]", "...@attr", "...#text".

#include "Segments.h"
#include "TextNorm.h"

#include <algorithm>
#include <map>

namespace xray::content_merge
{
namespace
{
enum class XmlTokKind : std::uint8_t
{
    Text,
    Comment,
    Special, // <? ?>, <! >, CDATA
    Start,
    End,
};

struct XmlAttr
{
    std::uint32_t begin; // the spaces before the name included
    std::uint32_t end; // after the closing quote
    std::string name;
    std::string value;
};

struct XmlTok
{
    XmlTokKind kind;
    std::uint32_t begin;
    std::uint32_t end;
    std::string tag;
    std::uint32_t name_end{}; // Start: after the tag name
    std::vector<XmlAttr> attrs;
    std::uint32_t attrs_end{}; // Start: after the last attribute (the tail "  >" / "/>" begins here)
    bool self_close{};
    bool closed{true}; // Comment / Special: the terminator was found
};

bool XmlNameStart(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}

bool XmlNameChar(char ch)
{
    return XmlNameStart(ch) || (ch >= '0' && ch <= '9') || ch == ':' || ch == '.' || ch == '-' ||
        static_cast<unsigned char>(ch) >= 0x80;
}

bool XmlSpace(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

struct XmlLexer
{
    std::string_view text;
    std::vector<XmlTok> toks;
    std::vector<std::string> errors;

    // A start / end tag at pos ('<'), false if it is not one (then '<' is text)
    bool Tag(std::uint32_t pos, XmlTok& tok)
    {
        const auto size = std::uint32_t(text.size());
        std::uint32_t p = pos + 1;
        bool closing = false;
        if (p < size && text[p] == '/')
        {
            closing = true;
            ++p;
        }
        if (p >= size || !XmlNameStart(text[p]))
            return false;
        const std::uint32_t name_begin = p;
        while (p < size && XmlNameChar(text[p]))
            ++p;
        tok.tag = std::string(text.substr(name_begin, p - name_begin));
        tok.name_end = p;
        // Up to '>' outside the quotes
        std::uint32_t q = p;
        while (q < size && text[q] != '>')
        {
            if (text[q] == '"' || text[q] == '\'')
            {
                const std::size_t close = text.find(text[q], q + 1);
                if (close == std::string_view::npos)
                    return false;
                q = std::uint32_t(close);
            }
            ++q;
        }
        if (q >= size)
            return false;
        tok.begin = pos;
        tok.end = q + 1;
        if (closing)
        {
            tok.kind = XmlTokKind::End;
            return true;
        }
        tok.kind = XmlTokKind::Start;
        tok.self_close = q > p && text[q - 1] == '/';
        // The attributes: name = "value" / 'value', each with the spaces before it
        std::uint32_t a = p;
        tok.attrs_end = p;
        const std::uint32_t limit = tok.self_close ? q - 1 : q;
        while (a < limit)
        {
            std::uint32_t s = a;
            while (s < limit && XmlSpace(text[s]))
                ++s;
            if (s >= limit || !XmlNameStart(text[s]))
                break;
            std::uint32_t ne = s;
            while (ne < limit && XmlNameChar(text[ne]))
                ++ne;
            std::uint32_t e = ne;
            while (e < limit && XmlSpace(text[e]))
                ++e;
            if (e >= limit || text[e] != '=')
                break;
            ++e;
            while (e < limit && XmlSpace(text[e]))
                ++e;
            if (e >= limit || (text[e] != '"' && text[e] != '\''))
                break;
            const std::size_t close = text.find(text[e], e + 1);
            if (close == std::string_view::npos || close >= limit)
                break;
            XmlAttr attr;
            attr.begin = a;
            attr.end = std::uint32_t(close + 1);
            attr.name = std::string(text.substr(s, ne - s));
            attr.value = std::string(text.substr(e + 1, close - e - 1));
            tok.attrs.push_back(std::move(attr));
            a = std::uint32_t(close + 1);
            tok.attrs_end = a;
        }
        return true;
    }

    void Run()
    {
        const auto size = std::uint32_t(text.size());
        std::uint32_t pos = 0;
        std::uint32_t text_begin = 0;
        const auto flush_text = [&](std::uint32_t to)
        {
            if (to > text_begin)
            {
                XmlTok t;
                t.kind = XmlTokKind::Text;
                t.begin = text_begin;
                t.end = to;
                toks.push_back(std::move(t));
            }
        };
        const auto special = [&](XmlTokKind kind, std::uint32_t from, std::string_view terminator)
        {
            const std::size_t close = text.find(terminator, from);
            XmlTok t;
            t.kind = kind;
            t.begin = pos;
            t.closed = close != std::string_view::npos;
            t.end = t.closed ? std::uint32_t(close + terminator.size()) : size;
            if (!t.closed && errors.empty())
                errors.push_back("an unclosed comment or a special tag (line " + std::to_string(LineOf(text, pos)) +
                    ")");
            toks.push_back(std::move(t));
            pos = toks.back().end;
            text_begin = pos;
        };
        while (pos < size)
        {
            if (text[pos] != '<')
            {
                ++pos;
                continue;
            }
            const std::string_view rest = text.substr(pos);
            if (rest.compare(0, 4, "<!--") == 0)
            {
                flush_text(pos);
                special(XmlTokKind::Comment, pos + 4, "-->");
                continue;
            }
            if (rest.compare(0, 9, "<![CDATA[") == 0)
            {
                flush_text(pos);
                special(XmlTokKind::Special, pos + 9, "]]>");
                continue;
            }
            if (rest.compare(0, 2, "<?") == 0)
            {
                flush_text(pos);
                special(XmlTokKind::Special, pos + 2, "?>");
                continue;
            }
            if (rest.compare(0, 2, "<!") == 0)
            {
                flush_text(pos);
                special(XmlTokKind::Special, pos + 2, ">");
                continue;
            }
            XmlTok tok;
            if (Tag(pos, tok))
            {
                flush_text(pos);
                pos = tok.end;
                toks.push_back(std::move(tok));
                text_begin = pos;
                continue;
            }
            if (errors.empty())
                errors.push_back("'<' that does not start a tag (line " + std::to_string(LineOf(text, pos)) + ")");
            ++pos;
        }
        flush_text(size);
    }
};

// The tree of the tokens
struct XmlNode
{
    std::size_t start{}; // the index of the Start token
    std::size_t end_tok = SIZE_MAX; // the index of the End token, SIZE_MAX - none (self-closed or closed implicitly)
    std::uint32_t body_end{}; // where the body ends (the end tag or the end of the last child)
    std::vector<std::size_t> items; // the tokens of the body: Text / Comment / Special / End (stray) or Start of a child
    std::vector<XmlNode> children; // in the order of the Start items
};

// The nesting the tree follows (the game xml go 8 deep): deeper, the parse fails (the merge by lines). The tree, the
// segments and the merge are recursive, a broken file of many unclosed tags must not overflow the stack
constexpr std::size_t kXmlMaxDepth = 128;

struct XmlTree
{
    const std::vector<XmlTok>& toks;
    std::string_view text;
    bool too_deep{};

    // The items of a list from token i until an End that closes one of the open tags; returns the next token
    std::size_t Items(std::size_t i, std::vector<std::string>& open, std::vector<std::size_t>& items,
        std::vector<XmlNode>& children)
    {
        while (i < toks.size())
        {
            const XmlTok& tok = toks[i];
            if (tok.kind == XmlTokKind::End)
            {
                if (std::find(open.begin(), open.end(), tok.tag) != open.end())
                    return i;
                items.push_back(i); // a stray end tag: text
                ++i;
                continue;
            }
            if (tok.kind != XmlTokKind::Start)
            {
                items.push_back(i);
                ++i;
                continue;
            }
            items.push_back(i);
            XmlNode node;
            node.start = i;
            node.body_end = tok.end;
            ++i;
            if (!tok.self_close && open.size() >= kXmlMaxDepth)
                too_deep = true; // its body goes on as the siblings: the doc is not ok
            else if (!tok.self_close)
            {
                open.push_back(tok.tag);
                i = Items(i, open, node.items, node.children);
                open.pop_back();
                node.body_end = node.items.empty() ? tok.end : toks[node.items.back()].end;
                if (i < toks.size() && toks[i].kind == XmlTokKind::End && toks[i].tag == tok.tag)
                {
                    node.end_tok = i;
                    node.body_end = toks[i].begin;
                    ++i;
                }
            }
            children.push_back(std::move(node));
        }
        return i;
    }
};

std::string XmlCollapse(std::string_view s)
{
    const String c = text::CollapseSpaces(s);
    return std::string(c.data(), c.size());
}

struct XmlBuilder
{
    std::string_view text;
    const std::vector<XmlTok>& toks;
    LineIndex lines;

    bool IsTrivia(const XmlTok& tok) const
    {
        if (tok.kind == XmlTokKind::Comment)
            return true;
        if (tok.kind != XmlTokKind::Text)
            return false;
        return text::Trim(text.substr(tok.begin, tok.end - tok.begin)).empty();
    }

    std::string GapCmp(const XmlTok& tok) const
    {
        if (tok.kind == XmlTokKind::Comment)
            return {};
        return XmlCollapse(text.substr(tok.begin, tok.end - tok.begin));
    }

    // The segments of a list: items [from the list] covering [begin, end)
    void List(const std::vector<std::size_t>& items, const std::vector<XmlNode>& children, std::uint32_t begin,
        std::uint32_t end, const std::string& path, std::vector<Seg>& out)
    {
        std::uint32_t pending = UINT32_MAX;
        std::uint32_t cursor = begin;
        bool gap_open = false;
        Seg gap;
        std::size_t child = 0;
        std::map<std::string, int> seen; // keys
        std::map<std::string, int> unkeyed; // the number of a tag without a key
        const std::string gap_label = path.empty() ? std::string("#doc") : path + "#text";
        const auto close_gap = [&]()
        {
            if (gap_open)
            {
                out.push_back(std::move(gap));
                gap = Seg{};
                gap_open = false;
            }
        };
        for (const std::size_t idx : items)
        {
            const XmlTok& tok = toks[idx];
            if (tok.kind == XmlTokKind::Start)
            {
                close_gap();
                const std::uint32_t seg_begin = pending != UINT32_MAX ? pending : cursor;
                pending = UINT32_MAX;
                Seg node = Node(children[child++], seg_begin, path, seen, unkeyed);
                cursor = node.end;
                out.push_back(std::move(node));
                continue;
            }
            if (IsTrivia(tok))
            {
                close_gap();
                if (pending == UINT32_MAX)
                    pending = tok.begin;
                cursor = tok.end;
                continue;
            }
            if (!gap_open)
            {
                gap_open = true;
                gap.gap = true;
                gap.divisible = true;
                gap.label = gap_label;
                gap.begin = pending != UINT32_MAX ? pending : cursor;
                gap.line = lines.Of(tok.begin);
                pending = UINT32_MAX;
            }
            const std::string c = GapCmp(tok);
            if (!c.empty())
            {
                if (!gap.cmp.empty())
                    gap.cmp += ' ';
                gap.cmp += c;
            }
            gap.end = tok.end;
            cursor = tok.end;
        }
        if (gap_open && pending == UINT32_MAX)
        {
            gap.end = end;
            cursor = end;
        }
        close_gap();
        if (pending != UINT32_MAX || cursor < end)
        {
            // The spaces and the comments before the end of the list
            Seg tail;
            tail.gap = true;
            tail.divisible = true;
            tail.label = gap_label;
            tail.begin = pending != UINT32_MAX ? pending : cursor;
            tail.end = end;
            tail.line = lines.Of(tail.begin);
            out.push_back(std::move(tail));
        }
    }

    Seg Node(const XmlNode& node, std::uint32_t seg_begin, const std::string& path, std::map<std::string, int>& seen,
        std::map<std::string, int>& unkeyed)
    {
        const XmlTok& st = toks[node.start];
        std::string key_attr;
        for (const char* name : {"id", "name"})
        {
            for (const XmlAttr& attr : st.attrs)
            {
                if (attr.name == name)
                {
                    key_attr = std::string("[") + name + "=" + attr.value + "]";
                    break;
                }
            }
            if (!key_attr.empty())
                break;
        }
        std::string key = st.tag + key_attr;
        if (key_attr.empty())
        {
            const int n = ++unkeyed[st.tag];
            if (n > 1)
                key += "#" + std::to_string(n);
        }
        else
        {
            const int n = ++seen[key];
            if (n > 1)
                key += "#" + std::to_string(n);
        }
        Seg seg;
        seg.key = key;
        seg.label = path.empty() ? key : path + "/" + key;
        seg.begin = seg_begin;
        seg.line = lines.Of(st.begin);

        Seg open;
        open.key = "<";
        open.label = seg.label;
        open.begin = seg_begin;
        open.end = st.name_end;
        open.line = seg.line;
        open.cmp = "<" + st.tag;
        seg.children.push_back(std::move(open));

        std::vector<std::string> attr_cmps;
        std::map<std::string, int> attr_seen;
        for (const XmlAttr& attr : st.attrs)
        {
            Seg a;
            a.key = "@" + attr.name;
            const int n = ++attr_seen[a.key];
            if (n > 1)
                a.key += "#" + std::to_string(n);
            a.label = seg.label + "@" + attr.name;
            a.begin = attr.begin;
            a.end = attr.end;
            a.line = lines.Of(attr.begin);
            a.cmp = attr.value;
            attr_cmps.push_back(a.key + "=" + a.cmp);
            seg.children.push_back(std::move(a));
        }
        Seg close_start;
        close_start.key = ">";
        close_start.label = seg.label;
        close_start.begin = st.attrs_end;
        close_start.end = st.end;
        close_start.line = lines.Of(st.attrs_end);
        close_start.cmp = XmlCollapse(text.substr(st.attrs_end, st.end - st.attrs_end));
        seg.children.push_back(std::move(close_start));

        std::vector<Seg> body;
        if (!st.self_close)
            List(node.items, node.children, st.end, node.body_end, seg.label, body);
        std::uint32_t end = st.end;
        for (Seg& b : body)
        {
            end = b.end;
            seg.children.push_back(std::move(b));
        }
        if (node.end_tok != SIZE_MAX)
        {
            const XmlTok& et = toks[node.end_tok];
            Seg close;
            close.key = "</";
            close.label = seg.label;
            close.begin = et.begin;
            close.end = et.end;
            close.line = lines.Of(et.begin);
            close.cmp = "</" + et.tag + ">";
            end = et.end;
            seg.children.push_back(std::move(close));
        }
        seg.end = end;

        std::sort(attr_cmps.begin(), attr_cmps.end());
        seg.cmp = "<" + st.tag;
        for (const std::string& a : attr_cmps)
            seg.cmp += " " + a;
        for (const Seg& c : seg.children)
        {
            if (c.key == "<" || c.key.rfind("@", 0) == 0)
                continue;
            seg.cmp += '\x1f';
            seg.cmp += c.gap ? std::string("~") : c.key;
            seg.cmp += '\x1e';
            seg.cmp += c.cmp;
        }
        return seg;
    }
};

struct XmlParsed
{
    std::vector<XmlTok> toks;
    std::vector<std::size_t> items;
    std::vector<XmlNode> children;
    std::vector<std::string> errors;
    bool too_deep{};
};

void XmlParseTree(std::string_view text, XmlParsed& parsed)
{
    XmlLexer lexer{text, {}, {}};
    lexer.Run();
    parsed.toks = std::move(lexer.toks);
    parsed.errors = std::move(lexer.errors);
    XmlTree tree{parsed.toks, text};
    std::vector<std::string> open;
    std::size_t i = 0;
    while (i < parsed.toks.size())
    {
        i = tree.Items(i, open, parsed.items, parsed.children);
        if (i < parsed.toks.size())
        {
            parsed.items.push_back(i); // an end tag nothing opened (Items stops only at the open ones)
            ++i;
        }
    }
    parsed.too_deep = tree.too_deep;
}
} // namespace

Doc ParseXml(std::string_view text)
{
    XmlParsed parsed;
    XmlParseTree(text, parsed);
    XmlBuilder builder{text, parsed.toks, LineIndex(text)};
    Doc doc;
    builder.List(parsed.items, parsed.children, 0, std::uint32_t(text.size()), std::string(), doc.top);
    for (const XmlTok& tok : parsed.toks)
    {
        if ((tok.kind == XmlTokKind::Comment || tok.kind == XmlTokKind::Special) && !tok.closed)
        {
            doc.ok = false;
            doc.error = "an unclosed comment (line " + std::to_string(LineOf(text, tok.begin)) + ")";
        }
    }
    if (parsed.too_deep && doc.ok)
    {
        doc.ok = false;
        doc.error = "the tags are nested deeper than " + std::to_string(kXmlMaxDepth);
    }
    return doc;
}

bool ValidateXml(std::string_view text_in, std::string& error)
{
    // '#include "x"' lines are the engine's (XMLDocument::ParseFile): blanked, the offsets kept
    std::string text(text_in);
    std::size_t pos = 0;
    while (pos < text.size())
    {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string::npos)
            eol = text.size();
        const std::string_view line = text::Trim(std::string_view(text).substr(pos, eol - pos));
        if (line.rfind("#include", 0) == 0)
            std::fill(text.begin() + std::ptrdiff_t(pos), text.begin() + std::ptrdiff_t(eol), ' ');
        pos = eol + 1;
    }
    XmlLexer lexer{text, {}, {}};
    lexer.Run();
    if (!lexer.errors.empty())
    {
        error = lexer.errors.front();
        return false;
    }
    std::vector<const XmlTok*> stack;
    for (const XmlTok& tok : lexer.toks)
    {
        if (tok.kind == XmlTokKind::Start)
        {
            std::map<std::string, int> names;
            for (const XmlAttr& attr : tok.attrs)
            {
                if (++names[attr.name] > 1)
                {
                    error = "a repeated attribute '" + attr.name + "' of <" + tok.tag + "> (line " +
                        std::to_string(LineOf(text, tok.begin)) + ")";
                    return false;
                }
            }
            if (!tok.self_close)
                stack.push_back(&tok);
            continue;
        }
        if (tok.kind != XmlTokKind::End)
            continue;
        if (stack.empty() || stack.back()->tag != tok.tag)
        {
            error = "the end tag </" + tok.tag + "> (line " + std::to_string(LineOf(text, tok.begin)) + ") " +
                (stack.empty() ? std::string("closes nothing") :
                                 "does not close <" + stack.back()->tag + "> (line " +
                         std::to_string(LineOf(text, stack.back()->begin)) + ")");
            return false;
        }
        stack.pop_back();
    }
    if (!stack.empty())
    {
        error = "<" + stack.back()->tag + "> (line " + std::to_string(LineOf(text, stack.back()->begin)) +
            ") is not closed";
        return false;
    }
    return true;
}
} // namespace xray::content_merge
