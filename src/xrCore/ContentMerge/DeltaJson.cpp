// The JSON reader and writer of the delta (DeltaJson.h).

#include "DeltaJson.h"
#include "TextNorm.h"

#include <cstdio>
#include <cstdlib>
#include <set>

namespace xray::content_merge::json
{
Value Value::MakeString(std::string_view s)
{
    Value v;
    v.type = Type::String;
    v.text = std::string(s);
    return v;
}

Value Value::MakeBool(bool b)
{
    Value v;
    v.type = Type::Bool;
    v.boolean = b;
    return v;
}

Value Value::MakeNumber(long long n)
{
    Value v;
    v.type = Type::Number;
    v.text = std::to_string(n);
    return v;
}

Value Value::MakeArray()
{
    Value v;
    v.type = Type::Array;
    return v;
}

Value Value::MakeObject()
{
    Value v;
    v.type = Type::Object;
    return v;
}

const Value* Value::Find(std::string_view key) const
{
    if (type != Type::Object)
        return nullptr;
    for (const auto& [name, value] : members)
    {
        if (name == key)
            return &value;
    }
    return nullptr;
}

Value& Value::Add(std::string_view key, Value value)
{
    members.emplace_back(std::string(key), std::move(value));
    return members.back().second;
}

std::string Value::StringOf(std::string_view key, std::string_view fallback) const
{
    const Value* v = Find(key);
    return v && v->type == Type::String ? v->text : std::string(fallback);
}

bool Value::BoolOf(std::string_view key, bool fallback) const
{
    const Value* v = Find(key);
    return v && v->type == Type::Bool ? v->boolean : fallback;
}

long long Value::NumberOf(std::string_view key, long long fallback) const
{
    const Value* v = Find(key);
    // An integer only: "1.9" or "1e3" is not 1
    if (!v || v->type != Type::Number || v->text.find_first_of(".eE") != std::string::npos)
        return fallback;
    return std::strtoll(v->text.c_str(), nullptr, 10);
}

namespace
{
class Reader
{
public:
    explicit Reader(std::string_view text) : text_(text) {}

    bool Document(Value& out, std::string& error)
    {
        Spaces();
        if (!Any(out, 0))
        {
            error = Where() + error_;
            return false;
        }
        Spaces();
        if (pos_ != text_.size())
        {
            error = Where() + "extra data after the value";
            return false;
        }
        return true;
    }

private:
    static constexpr int kMaxDepth = 64;

    std::string Where() const
    {
        std::size_t line = 1;
        std::size_t column = 1;
        for (std::size_t i = 0; i < pos_ && i < text_.size(); ++i)
        {
            if (text_[i] == '\n')
            {
                ++line;
                column = 1;
            }
            else
                ++column;
        }
        return "line " + std::to_string(line) + ", column " + std::to_string(column) + ": ";
    }

    bool Fail(const char* message)
    {
        error_ = message;
        return false;
    }

    void Spaces()
    {
        while (pos_ < text_.size() &&
            (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r'))
            ++pos_;
    }

    bool Word(std::string_view word)
    {
        if (text_.substr(pos_, word.size()) != word)
            return false;
        pos_ += word.size();
        return true;
    }

    // depth - the arrays / objects around the value: at most kMaxDepth of them one in another
    bool Any(Value& out, int depth)
    {
        if (pos_ >= text_.size())
            return Fail("a value is expected");
        const char ch = text_[pos_];
        if ((ch == '{' || ch == '[') && depth >= kMaxDepth)
            return Fail("the values are nested too deep");
        if (ch == '{')
            return Object(out, depth);
        if (ch == '[')
            return Array(out, depth);
        if (ch == '"')
        {
            out.type = Type::String;
            return String(out.text);
        }
        if (Word("true"))
        {
            out.type = Type::Bool;
            out.boolean = true;
            return true;
        }
        if (Word("false"))
        {
            out.type = Type::Bool;
            out.boolean = false;
            return true;
        }
        if (Word("null"))
        {
            out.type = Type::Null;
            return true;
        }
        if (ch == '-' || (ch >= '0' && ch <= '9'))
            return Number(out);
        return Fail("a value is expected");
    }

    bool Number(Value& out)
    {
        const std::size_t start = pos_;
        if (text_[pos_] == '-')
            ++pos_;
        const auto digits = [this]() {
            const std::size_t from = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9')
                ++pos_;
            return pos_ > from;
        };
        const std::size_t int_start = pos_;
        if (!digits() || (text_[int_start] == '0' && pos_ - int_start > 1))
            return Fail("a bad number");
        if (pos_ < text_.size() && text_[pos_] == '.')
        {
            ++pos_;
            if (!digits())
                return Fail("a bad number");
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E'))
        {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-'))
                ++pos_;
            if (!digits())
                return Fail("a bad number");
        }
        out.type = Type::Number;
        out.text = std::string(text_.substr(start, pos_ - start));
        return true;
    }

    static void AppendUtf8(std::string& out, std::uint32_t cp)
    {
        if (cp < 0x80)
            out += char(cp);
        else if (cp < 0x800)
        {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
        else
        {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }

    bool Hex4(std::uint32_t& cp)
    {
        if (pos_ + 4 > text_.size())
            return Fail("a bad \\u escape");
        cp = 0;
        for (int i = 0; i < 4; ++i)
        {
            const char ch = text_[pos_++];
            cp <<= 4;
            if (ch >= '0' && ch <= '9')
                cp |= std::uint32_t(ch - '0');
            else if (ch >= 'a' && ch <= 'f')
                cp |= std::uint32_t(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F')
                cp |= std::uint32_t(ch - 'A' + 10);
            else
                return Fail("a bad \\u escape");
        }
        return true;
    }

    bool String(std::string& out)
    {
        ++pos_; // '"'
        out.clear();
        while (pos_ < text_.size())
        {
            const char ch = text_[pos_++];
            if (ch == '"')
                return true;
            if (static_cast<unsigned char>(ch) < 0x20)
                return Fail("a control char in a string");
            if (ch != '\\')
            {
                out += ch;
                continue;
            }
            if (pos_ >= text_.size())
                break;
            const char esc = text_[pos_++];
            switch (esc)
            {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u':
            {
                std::uint32_t cp = 0;
                if (!Hex4(cp))
                    return false;
                if (cp >= 0xD800 && cp <= 0xDBFF)
                {
                    std::uint32_t low = 0;
                    if (!Word("\\u") || !Hex4(low) || low < 0xDC00 || low > 0xDFFF)
                        return Fail("a lone surrogate in a \\u escape");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                else if (cp >= 0xDC00 && cp <= 0xDFFF)
                    return Fail("a lone surrogate in a \\u escape");
                AppendUtf8(out, cp);
                break;
            }
            default: return Fail("a bad escape in a string");
            }
        }
        return Fail("an unclosed string");
    }

    bool Array(Value& out, int depth)
    {
        ++pos_; // '['
        out.type = Type::Array;
        Spaces();
        if (pos_ < text_.size() && text_[pos_] == ']')
        {
            ++pos_;
            return true;
        }
        for (;;)
        {
            Spaces();
            Value item;
            if (!Any(item, depth + 1))
                return false;
            out.items.push_back(std::move(item));
            Spaces();
            if (pos_ < text_.size() && text_[pos_] == ',')
            {
                ++pos_;
                continue;
            }
            if (pos_ < text_.size() && text_[pos_] == ']')
            {
                ++pos_;
                return true;
            }
            return Fail("',' or ']' is expected");
        }
    }

    bool Object(Value& out, int depth)
    {
        ++pos_; // '{'
        out.type = Type::Object;
        std::set<std::string> names;
        Spaces();
        if (pos_ < text_.size() && text_[pos_] == '}')
        {
            ++pos_;
            return true;
        }
        for (;;)
        {
            Spaces();
            if (pos_ >= text_.size() || text_[pos_] != '"')
                return Fail("a member name is expected");
            std::string name;
            if (!String(name))
                return false;
            // A name twice: which value counts is up to the reader (the first one here, the last one in Python, a
            // Rust map...), so the engine, gw_merge and the launcher could read different deltas - the text is broken
            if (!names.insert(name).second)
                return Fail("a member name is repeated in an object");
            Spaces();
            if (pos_ >= text_.size() || text_[pos_] != ':')
                return Fail("':' is expected");
            ++pos_;
            Spaces();
            Value value;
            if (!Any(value, depth + 1))
                return false;
            out.members.emplace_back(std::move(name), std::move(value));
            Spaces();
            if (pos_ < text_.size() && text_[pos_] == ',')
            {
                ++pos_;
                continue;
            }
            if (pos_ < text_.size() && text_[pos_] == '}')
            {
                ++pos_;
                return true;
            }
            return Fail("',' or '}' is expected");
        }
    }

    std::string_view text_;
    std::size_t pos_{};
    std::string error_;
};

void WriteValue(const Value& value, int indent, int level, std::string& out)
{
    const auto newline = [&](int lv) {
        out += '\n';
        out.append(std::size_t(indent * lv), ' ');
    };
    switch (value.type)
    {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += value.boolean ? "true" : "false"; break;
    case Type::Number: out += value.text.empty() ? std::string("0") : value.text; break;
    case Type::String: out += Quote(value.text); break;
    case Type::Array:
        if (value.items.empty())
        {
            out += "[]";
            break;
        }
        out += '[';
        for (std::size_t i = 0; i < value.items.size(); ++i)
        {
            if (i)
                out += ',';
            newline(level + 1);
            WriteValue(value.items[i], indent, level + 1, out);
        }
        newline(level);
        out += ']';
        break;
    case Type::Object:
        if (value.members.empty())
        {
            out += "{}";
            break;
        }
        out += '{';
        for (std::size_t i = 0; i < value.members.size(); ++i)
        {
            if (i)
                out += ',';
            newline(level + 1);
            out += Quote(value.members[i].first);
            out += ": ";
            WriteValue(value.members[i].second, indent, level + 1, out);
        }
        newline(level);
        out += '}';
        break;
    }
}
} // namespace

bool Parse(std::string_view text, Value& out, std::string& error)
{
    // A UTF-8 BOM is allowed (an editor of an author may write it)
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3);
    out = Value{};
    // Strict UTF-8 as Python and Rust decode it: a byte 0xFF, a cp1251 text, an overlong form or an encoded surrogate
    // is not JSON for any reader of the delta (ContentMerge.h, DeltaFromJson)
    if (!xray::content_merge::text::IsUtf8(text))
    {
        error = "not UTF-8";
        return false;
    }
    Reader reader(text);
    return reader.Document(out, error);
}

std::string Write(const Value& value, int indent)
{
    std::string out;
    WriteValue(value, indent, 0, out);
    out += '\n';
    return out;
}

std::string Quote(std::string_view s)
{
    std::string out = "\"";
    out.reserve(s.size() + 2);
    for (const char ch : s)
    {
        switch (ch)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned int>(static_cast<unsigned char>(ch)));
                out += buf;
            }
            else
                out += ch;
        }
    }
    out += '"';
    return out;
}
} // namespace xray::content_merge::json
