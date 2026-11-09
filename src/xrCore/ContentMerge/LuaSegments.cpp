// Lua -> segments (15-merge-implementation.md section 3.2). The lexer is the one of tools/addons_snapshot/luascan.py
// (tokenize) with the offsets kept; the top level statements are split by a light parser of Lua 5.1 / LuaJIT
// statements and expressions (no scopes, no semantics). The atoms are the top level statements:
//   function a.b.c(...) ... end, function a:b(...) ... end   -> "a.b.c", "a:b"
//   a.b = function(...) ... end                               -> "a.b" (the same key: one form may become the other)
//   local function f(...) ... end, local f = ...              -> "local f"
//   local a, b = <expr>                                       -> "local a,b"
//   a = { ... }, a.b = <expr>, a, b = ...                     -> "a", "a.b", "a,b"
//   class "x" (base)                                          -> "class x" (luabind)
// Any other top level statement (if, do, for, a call, return) is a gap; the gaps one after another are one gap.
// A definition repeated in the file: the key of the second one is "x#2". The compared form is the stream of the
// tokens without the comments: a new layout is not a change.

#include "Segments.h"
#include "TextNorm.h"

#include <map>

namespace xray::content_merge
{
namespace
{
enum class LuaTokKind : std::uint8_t
{
    Name,
    Keyword,
    String,
    Number,
    Op,
    Other, // a byte the lexer does not know (outside the strings)
};

struct LuaTok
{
    LuaTokKind kind;
    std::uint32_t begin;
    std::uint32_t end;
    std::uint32_t line;
};

bool LuaIsKeyword(std::string_view s)
{
    static const char* const keywords[] = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function",
        "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"};
    for (const char* kw : keywords)
    {
        if (s == kw)
            return true;
    }
    return false;
}

// LuaJIT takes any byte >= 0x80 for a char of a name (lj_char.c): the names in UTF-8 compile
bool LuaNameStart(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_' || static_cast<unsigned char>(ch) >= 0x80;
}

bool LuaDigit(char ch)
{
    return ch >= '0' && ch <= '9';
}

bool LuaHex(char ch)
{
    return LuaDigit(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

struct LuaLexer
{
    std::string_view text;
    std::vector<LuaTok> toks;
    std::vector<std::string> errors;

    // "[==[" at pos: the level (the number of '='), -1 - not a long bracket
    int LongLevel(std::uint32_t pos) const
    {
        if (pos >= text.size() || text[pos] != '[')
            return -1;
        std::uint32_t p = pos + 1;
        int level = 0;
        while (p < text.size() && text[p] == '=')
        {
            ++level;
            ++p;
        }
        return p < text.size() && text[p] == '[' ? level : -1;
    }

    // After the long bracket opening at pos with the level: the end of its closing bracket
    std::uint32_t LongEnd(std::uint32_t pos, int level, std::uint32_t line)
    {
        const std::string close = "]" + std::string(std::size_t(level), '=') + "]";
        const std::size_t stop = text.find(close, pos + std::uint32_t(level) + 2);
        if (stop == std::string_view::npos)
        {
            errors.push_back("an unfinished long string or comment (line " + std::to_string(line) + ")");
            return std::uint32_t(text.size());
        }
        return std::uint32_t(stop + close.size());
    }

    std::uint32_t CountLines(std::uint32_t from, std::uint32_t to) const
    {
        std::uint32_t n = 0;
        for (std::uint32_t i = from; i < to; ++i)
            n += text[i] == '\n';
        return n;
    }

    void Run()
    {
        const auto size = std::uint32_t(text.size());
        std::uint32_t pos = 0;
        std::uint32_t line = 1;
        if (size && text[0] == '#')
        {
            // The first line "#!..." is skipped by LuaJIT (lj_lex_setup): a comment
            const std::size_t nl = text.find('\n');
            pos = nl == std::string_view::npos ? size : std::uint32_t(nl);
        }
        while (pos < size)
        {
            const char ch = text[pos];
            if (ch == '\n')
            {
                ++line;
                ++pos;
                continue;
            }
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\f' || ch == '\v')
            {
                ++pos;
                continue;
            }
            if (ch == '-' && pos + 1 < size && text[pos + 1] == '-')
            {
                const int level = LongLevel(pos + 2);
                std::uint32_t end;
                if (level >= 0)
                    end = LongEnd(pos + 2, level, line);
                else
                {
                    const std::size_t nl = text.find('\n', pos);
                    end = nl == std::string_view::npos ? size : std::uint32_t(nl);
                }
                line += CountLines(pos, end);
                pos = end;
                continue;
            }
            LuaTok tok{LuaTokKind::Op, pos, pos + 1, line};
            if (ch == '[')
            {
                const int level = LongLevel(pos);
                if (level >= 0)
                {
                    tok.kind = LuaTokKind::String;
                    tok.end = LongEnd(pos, level, line);
                }
            }
            if (tok.kind == LuaTokKind::String)
            {
                // the long string above
            }
            else if (ch == '"' || ch == '\'')
            {
                std::uint32_t p = pos + 1;
                while (p < size && text[p] != ch)
                {
                    if (text[p] == '\\' && p + 1 < size && text[p + 1] == 'z')
                    {
                        // "\z" skips the spaces after it, the line ends included (LuaJIT, Lua 5.2)
                        p += 2;
                        while (p < size && (text[p] == ' ' || text[p] == '\t' || text[p] == '\n' || text[p] == '\r' ||
                                   text[p] == '\f' || text[p] == '\v'))
                            ++p;
                    }
                    else if (text[p] == '\\' && p + 1 < size)
                        p += 2;
                    else if (text[p] == '\n')
                        break;
                    else
                        ++p;
                }
                if (p >= size || text[p] != ch)
                    errors.push_back("an unfinished string (line " + std::to_string(line) + ")");
                tok.kind = LuaTokKind::String;
                tok.end = p < size && text[p] == ch ? p + 1 : p;
            }
            else if (LuaNameStart(ch))
            {
                std::uint32_t p = pos + 1;
                while (p < size && (LuaNameStart(text[p]) || LuaDigit(text[p])))
                    ++p;
                tok.end = p;
                tok.kind = LuaIsKeyword(text.substr(pos, p - pos)) ? LuaTokKind::Keyword : LuaTokKind::Name;
            }
            else if (LuaDigit(ch) || (ch == '.' && pos + 1 < size && LuaDigit(text[pos + 1])))
            {
                std::uint32_t p = pos;
                if (ch == '0' && p + 1 < size && (text[p + 1] == 'x' || text[p + 1] == 'X'))
                {
                    p += 2;
                    while (p < size && (LuaHex(text[p]) || text[p] == '.'))
                        ++p;
                    if (p < size && (text[p] == 'p' || text[p] == 'P'))
                    {
                        ++p;
                        if (p < size && (text[p] == '+' || text[p] == '-'))
                            ++p;
                        while (p < size && LuaDigit(text[p]))
                            ++p;
                    }
                }
                else
                {
                    while (p < size && (LuaDigit(text[p]) || text[p] == '.'))
                        ++p;
                    if (p < size && (text[p] == 'e' || text[p] == 'E'))
                    {
                        ++p;
                        if (p < size && (text[p] == '+' || text[p] == '-'))
                            ++p;
                        while (p < size && LuaDigit(text[p]))
                            ++p;
                    }
                }
                // LuaJIT suffixes: 1LL, 1ULL, 1i
                while (p < size && LuaNameStart(text[p]))
                    ++p;
                tok.kind = LuaTokKind::Number;
                tok.end = p;
            }
            else
            {
                static const char* const ops[] = {"...", "..", "==", "~=", "<=", ">=", "::", "<<", ">>", "//"};
                tok.end = pos + 1;
                for (const char* op : ops)
                {
                    const std::size_t len = std::char_traits<char>::length(op);
                    if (text.compare(pos, len, op) == 0)
                    {
                        tok.end = pos + std::uint32_t(len);
                        break;
                    }
                }
                static const std::string_view singles = "-+*/%^#&~|<>=(){}[];:,.";
                if (tok.end == pos + 1 && singles.find(ch) == std::string_view::npos)
                {
                    tok.kind = LuaTokKind::Other;
                    // a UTF-8 sequence is one token
                    while (tok.end < size && (static_cast<unsigned char>(text[tok.end]) & 0xC0) == 0x80)
                        ++tok.end;
                }
            }
            line += CountLines(tok.begin, tok.end);
            pos = tok.end;
            toks.push_back(tok);
        }
    }
};

enum class LuaStmtKind : std::uint8_t
{
    Other,
    Function, // function a.b() / a.b = function()
    LocalFunction,
    Local,
    Assign,
    Class,
};

struct LuaStmt
{
    std::size_t first;
    std::size_t last; // inclusive
    LuaStmtKind kind{LuaStmtKind::Other};
    std::string name;
};

struct LuaParser
{
    std::string_view text;
    const std::vector<LuaTok>& t;
    std::vector<std::string> errors;

    std::string_view S(std::size_t i) const
    {
        return i < t.size() ? text.substr(t[i].begin, t[i].end - t[i].begin) : std::string_view();
    }
    bool Is(std::size_t i, std::string_view s) const
    {
        return i < t.size() && (t[i].kind == LuaTokKind::Op || t[i].kind == LuaTokKind::Keyword) && S(i) == s;
    }
    bool IsName(std::size_t i) const { return i < t.size() && t[i].kind == LuaTokKind::Name; }

    void Error(std::size_t i, const char* what)
    {
        if (i < t.size())
            errors.push_back(std::string(what) + " '" + std::string(S(i)) + "' (line " + std::to_string(t[i].line) +
                ")");
        else
            errors.push_back(std::string(what) + " <eof>");
    }

    // t[i] is an opening bracket: the index after its pair (the brackets of any kind are counted together)
    std::size_t MatchBracket(std::size_t i)
    {
        int depth = 0;
        for (std::size_t k = i; k < t.size(); ++k)
        {
            if (t[k].kind != LuaTokKind::Op)
                continue;
            const std::string_view s = S(k);
            if (s == "(" || s == "{" || s == "[")
                ++depth;
            else if (s == ")" || s == "}" || s == "]")
            {
                if (--depth == 0)
                    return k + 1;
            }
        }
        Error(i, "an unclosed bracket");
        return t.size();
    }

    // From i inside a block of the depth: the index after the end / until (and its condition) closing it
    std::size_t SkipBlock(std::size_t i, int depth)
    {
        for (std::size_t k = i; k < t.size(); ++k)
        {
            if (t[k].kind != LuaTokKind::Keyword)
                continue;
            const std::string_view s = S(k);
            if (s == "function" || s == "do" || s == "if" || s == "repeat")
                ++depth;
            else if (s == "end")
            {
                if (--depth == 0)
                    return k + 1;
            }
            else if (s == "until")
            {
                if (--depth == 0)
                    return SkipExp(k + 1);
            }
        }
        Error(i ? i - 1 : 0, "an unclosed block after");
        return t.size();
    }

    // t[i] is '(' of the parameters: after the 'end' of the body
    std::size_t SkipFuncBody(std::size_t i)
    {
        if (!Is(i, "("))
        {
            Error(i, "'(' expected near");
            return i;
        }
        return SkipBlock(MatchBracket(i), 1);
    }

    bool IsBinary(std::size_t i) const
    {
        if (i >= t.size())
            return false;
        if (t[i].kind == LuaTokKind::Keyword)
            return S(i) == "and" || S(i) == "or";
        if (t[i].kind != LuaTokKind::Op)
            return false;
        static const char* const ops[] = {"+", "-", "*", "/", "%", "^", "..", "==", "~=", "<", "<=", ">", ">=", "&",
            "|", "~", "<<", ">>", "//"};
        const std::string_view s = S(i);
        for (const char* op : ops)
        {
            if (s == op)
                return true;
        }
        return false;
    }

    std::size_t SkipSuffixed(std::size_t i)
    {
        std::size_t j;
        if (IsName(i))
            j = i + 1;
        else if (Is(i, "("))
            j = MatchBracket(i);
        else
            return i;
        for (;;)
        {
            if (Is(j, ".") && IsName(j + 1))
                j += 2;
            else if (Is(j, ":") && IsName(j + 1))
            {
                j += 2;
                if (j < t.size() && t[j].kind == LuaTokKind::String)
                    ++j;
                else if (Is(j, "(") || Is(j, "{"))
                    j = MatchBracket(j);
            }
            else if (Is(j, "[") || Is(j, "(") || Is(j, "{"))
                j = MatchBracket(j);
            else if (j < t.size() && t[j].kind == LuaTokKind::String)
                ++j;
            else
                break;
        }
        return j;
    }

    std::size_t SkipSimple(std::size_t i)
    {
        while (Is(i, "not") || Is(i, "-") || Is(i, "#") || Is(i, "~"))
            ++i;
        if (i >= t.size())
            return i;
        if (t[i].kind == LuaTokKind::Number || t[i].kind == LuaTokKind::String || Is(i, "nil") || Is(i, "true") ||
            Is(i, "false") || Is(i, "..."))
            return i + 1;
        if (Is(i, "{"))
            return MatchBracket(i);
        if (Is(i, "function"))
            return SkipFuncBody(i + 1);
        return SkipSuffixed(i);
    }

    std::size_t SkipExp(std::size_t i)
    {
        std::size_t j = SkipSimple(i);
        if (j == i)
            return i;
        while (IsBinary(j))
        {
            const std::size_t k = SkipSimple(j + 1);
            if (k == j + 1)
                return k;
            j = k;
        }
        return j;
    }

    std::size_t SkipExpList(std::size_t i)
    {
        std::size_t j = SkipExp(i);
        while (Is(j, ","))
            j = SkipExp(j + 1);
        return j;
    }

    // A dotted name t[i, j): "a.b.c", empty if it is not one
    std::string DottedName(std::size_t i, std::size_t j) const
    {
        std::string name;
        for (std::size_t k = i; k < j; ++k)
        {
            const bool want_name = (k - i) % 2 == 0;
            if (want_name ? !IsName(k) : !Is(k, "."))
                return {};
            name += S(k);
        }
        return (j - i) % 2 == 1 ? name : std::string();
    }

    std::size_t Statement(std::size_t i, LuaStmt& st)
    {
        st.kind = LuaStmtKind::Other;
        if (Is(i, ";") || Is(i, "break"))
            return i + 1;
        if (Is(i, "goto"))
            return i + 2;
        if (Is(i, "::"))
            return i + 3;
        if (Is(i, "function"))
        {
            std::size_t j = i + 1;
            std::string name;
            if (IsName(j))
            {
                name = S(j);
                ++j;
                while (Is(j, ".") && IsName(j + 1))
                {
                    name += ".";
                    name += S(j + 1);
                    j += 2;
                }
                if (Is(j, ":") && IsName(j + 1))
                {
                    name += ":";
                    name += S(j + 1);
                    j += 2;
                }
            }
            st.kind = name.empty() ? LuaStmtKind::Other : LuaStmtKind::Function;
            st.name = name;
            return SkipFuncBody(j);
        }
        if (Is(i, "local"))
        {
            if (Is(i + 1, "function"))
            {
                st.kind = LuaStmtKind::LocalFunction;
                st.name = IsName(i + 2) ? std::string(S(i + 2)) : std::string();
                return SkipFuncBody(i + 3);
            }
            std::size_t j = i + 1;
            std::string names;
            while (IsName(j))
            {
                if (!names.empty())
                    names += ",";
                names += S(j);
                ++j;
                if (Is(j, "<") && IsName(j + 1) && Is(j + 2, ">"))
                    j += 3; // Lua 5.4 attribs
                if (!Is(j, ","))
                    break;
                ++j;
            }
            if (names.empty())
            {
                Error(i + 1, "a name expected near");
                return i + 1;
            }
            st.kind = LuaStmtKind::Local;
            st.name = names;
            if (Is(j, "="))
                j = SkipExpList(j + 1);
            return j;
        }
        if (Is(i, "if"))
            return SkipBlock(i + 1, 1);
        if (Is(i, "do") || Is(i, "repeat"))
            return SkipBlock(i + 1, 1);
        if (Is(i, "while") || Is(i, "for"))
        {
            // the head up to its 'do' (a function in the head is skipped as a whole)
            std::size_t j = i + 1;
            while (j < t.size() && !Is(j, "do"))
            {
                if (Is(j, "function"))
                    j = SkipFuncBody(j + 1);
                else if (Is(j, "(") || Is(j, "{") || Is(j, "["))
                    j = MatchBracket(j);
                else
                    ++j;
            }
            return SkipBlock(j + 1, 1);
        }
        if (Is(i, "return"))
        {
            std::size_t j = i + 1;
            if (j < t.size() && !Is(j, ";") && !Is(j, "end"))
                j = SkipExpList(j);
            if (Is(j, ";"))
                ++j;
            return j;
        }
        if (IsName(i) && S(i) == "class" && i + 1 < t.size() && t[i + 1].kind == LuaTokKind::String)
        {
            st.kind = LuaStmtKind::Class;
            std::string_view s = S(i + 1);
            if (s.size() >= 2 && (s[0] == '"' || s[0] == '\''))
                s = s.substr(1, s.size() - 2);
            st.name = std::string(s);
            return SkipSuffixed(i);
        }
        const std::size_t j = SkipSuffixed(i);
        if (j == i)
        {
            Error(i, "unexpected");
            return i + 1;
        }
        if (!Is(j, "=") && !Is(j, ","))
            return j; // a call
        std::string names = DottedName(i, j);
        bool all_names = !names.empty();
        std::size_t k = j;
        while (Is(k, ","))
        {
            const std::size_t e = SkipSuffixed(k + 1);
            if (e == k + 1)
            {
                Error(k + 1, "unexpected");
                return e;
            }
            const std::string name = DottedName(k + 1, e);
            all_names = all_names && !name.empty();
            names += "," + name;
            k = e;
        }
        if (!Is(k, "="))
        {
            Error(k, "'=' expected near");
            return k;
        }
        const bool rhs_function = j == k && Is(k + 1, "function");
        const std::size_t end = SkipExpList(k + 1);
        if (all_names)
        {
            st.kind = rhs_function ? LuaStmtKind::Function : LuaStmtKind::Assign;
            st.name = names;
        }
        return end;
    }
};

// After the last token of a statement: the comment on its line and the line end go with it, unless code follows on
// the same line
std::uint32_t LuaTailEnd(std::string_view text, std::uint32_t pos)
{
    const auto size = std::uint32_t(text.size());
    std::uint32_t p = pos;
    for (;;)
    {
        while (p < size && (text[p] == ' ' || text[p] == '\t' || text[p] == '\r' || text[p] == '\f' ||
                   text[p] == '\v'))
            ++p;
        if (p + 1 < size && text[p] == '-' && text[p + 1] == '-')
        {
            std::uint32_t q = p + 2;
            if (q < size && text[q] == '[')
            {
                std::uint32_t r = q + 1;
                int level = 0;
                while (r < size && text[r] == '=')
                {
                    ++level;
                    ++r;
                }
                if (r < size && text[r] == '[')
                {
                    const std::string close = "]" + std::string(std::size_t(level), '=') + "]";
                    const std::size_t stop = text.find(close, r + 1);
                    p = stop == std::string_view::npos ? size : std::uint32_t(stop + close.size());
                    continue;
                }
            }
            const std::size_t nl = text.find('\n', p);
            p = nl == std::string_view::npos ? size : std::uint32_t(nl);
            continue;
        }
        break;
    }
    if (p >= size)
        return size;
    if (text[p] == '\n')
        return p + 1;
    return pos;
}

std::string LuaCmp(std::string_view text, const std::vector<LuaTok>& toks, std::size_t first, std::size_t last)
{
    std::string cmp;
    for (std::size_t k = first; k <= last && k < toks.size(); ++k)
    {
        if (!cmp.empty())
            cmp += ' ';
        cmp += text.substr(toks[k].begin, toks[k].end - toks[k].begin);
    }
    return cmp;
}
} // namespace

std::string ModuleOfName(std::string_view name)
{
    const std::size_t slash = name.find_last_of("\\/");
    if (slash != std::string_view::npos)
        name.remove_prefix(slash + 1);
    const std::size_t dot = name.find('.');
    if (dot != std::string_view::npos)
        name = name.substr(0, dot);
    const String lower = text::ToLower(name);
    return std::string(lower.data(), lower.size());
}

Doc ParseLua(std::string_view text, std::string_view module)
{
    LuaLexer lexer{text, {}, {}};
    lexer.Run();
    Doc doc;
    doc.code_gaps = true;
    if (!lexer.errors.empty())
    {
        doc.ok = false;
        doc.error = lexer.errors.front();
        return doc;
    }
    const std::vector<LuaTok>& toks = lexer.toks;
    LuaParser parser{text, toks, {}};
    std::vector<LuaStmt> stmts;
    std::size_t i = 0;
    while (i < toks.size())
    {
        LuaStmt st;
        st.first = i;
        std::size_t end = parser.Statement(i, st);
        if (end <= i)
            end = i + 1;
        if (end > toks.size())
            end = toks.size();
        st.last = end - 1;
        stmts.push_back(std::move(st));
        i = end;
    }
    for (const std::string& error : parser.errors)
    {
        if (error.find("unclosed") != std::string::npos)
        {
            doc.ok = false;
            doc.error = error;
            return doc;
        }
    }

    const LineIndex line_index(text);
    const std::string prefix = module.empty() ? std::string() : std::string(module) + ".";
    std::map<std::string, int> seen;
    std::uint32_t seg_begin = 0;
    bool gap_open = false;
    Seg gap;
    const auto close_gap = [&]()
    {
        if (gap_open)
        {
            doc.top.push_back(std::move(gap));
            gap = Seg{};
            gap_open = false;
        }
    };
    for (const LuaStmt& st : stmts)
    {
        const std::uint32_t content_begin = toks[st.first].begin;
        const std::uint32_t content_end = toks[st.last].end;
        const std::uint32_t tail = LuaTailEnd(text, content_end);
        (void)content_begin;
        if (st.kind == LuaStmtKind::Other)
        {
            if (!gap_open)
            {
                gap_open = true;
                gap.gap = true;
                gap.divisible = true;
                gap.begin = seg_begin;
                gap.line = toks[st.first].line;
                gap.label = std::string(module);
            }
            if (!gap.cmp.empty())
                gap.cmp += ' ';
            gap.cmp += LuaCmp(text, toks, st.first, st.last);
            gap.end = tail;
            seg_begin = tail;
            continue;
        }
        close_gap();
        Seg seg;
        switch (st.kind)
        {
        case LuaStmtKind::Function:
        case LuaStmtKind::Assign: seg.key = st.name; break;
        case LuaStmtKind::LocalFunction:
        case LuaStmtKind::Local: seg.key = "local " + st.name; break;
        case LuaStmtKind::Class: seg.key = "class " + st.name; break;
        case LuaStmtKind::Other: break;
        }
        const int n = ++seen[seg.key];
        if (n > 1)
            seg.key += "#" + std::to_string(n);
        seg.label = prefix + seg.key;
        seg.begin = seg_begin;
        seg.end = tail;
        seg.line = toks[st.first].line;
        seg.cmp = LuaCmp(text, toks, st.first, st.last);
        seg.function = st.kind == LuaStmtKind::Function || st.kind == LuaStmtKind::LocalFunction;
        seg.divisible = st.kind == LuaStmtKind::Function || st.kind == LuaStmtKind::LocalFunction ||
            line_index.Of(content_end ? content_end - 1 : 0) != seg.line;
        doc.top.push_back(std::move(seg));
        seg_begin = tail;
    }
    close_gap();
    if (seg_begin < text.size())
    {
        // the comments and the spaces after the last statement
        Seg tail;
        tail.gap = true;
        tail.divisible = true;
        tail.begin = seg_begin;
        tail.end = std::uint32_t(text.size());
        tail.line = LineOf(text, seg_begin);
        tail.label = std::string(module);
        doc.top.push_back(std::move(tail));
    }
    return doc;
}

bool ValidateLua(std::string_view text, std::string& error)
{
    LuaLexer lexer{text, {}, {}};
    lexer.Run();
    if (!lexer.errors.empty())
    {
        error = lexer.errors.front();
        return false;
    }
    LuaParser parser{text, lexer.toks, {}};
    std::size_t i = 0;
    while (i < lexer.toks.size())
    {
        LuaStmt st;
        std::size_t end = parser.Statement(i, st);
        i = end <= i ? i + 1 : end;
    }
    if (!parser.errors.empty())
    {
        error = parser.errors.front();
        return false;
    }
    return true;
}
} // namespace xray::content_merge
