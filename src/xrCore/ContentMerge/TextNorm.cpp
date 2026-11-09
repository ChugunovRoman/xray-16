// Text normalization: the port of tools/addons_snapshot/textnorm.py (15-merge-implementation.md section 3.1).
// The engine (xrCore), gw_merge and CI must see the same text, so the rules are the ones of textnorm.py:
//   decode  strict UTF-8; else Windows-1251 (as the engine does); the byte 0x98 undefined even in cp1251 -> U+FFFD;
//   BOM     a leading UTF-8 BOM is removed;
//   EOL     CRLF -> LF, a lone CR is kept (the ltx reader and the Lua lexer take it as a line break too).

#include "TextNorm.h"

#include <cstring>

namespace xray::content_merge
{
namespace text
{
namespace
{
// cp1251 0x80..0xBF -> the code point; 0xC0..0xFF are U+0410..U+044F
const std::uint16_t kCp1251High[64] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C,
    0x040B, 0x040F, 0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x0000, 0x2122, 0x0459, 0x203A,
    0x045A, 0x045C, 0x045B, 0x045F, 0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9,
    0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407, 0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
};
} // namespace

bool IsAscii(std::string_view bytes)
{
    for (const char ch : bytes)
    {
        if (static_cast<unsigned char>(ch) > 0x7F)
            return false;
    }
    return true;
}

bool IsUtf8(std::string_view bytes)
{
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
    const std::size_t n = bytes.size();
    std::size_t i = 0;
    while (i < n)
    {
        const unsigned char c = p[i];
        if (c < 0x80)
        {
            ++i;
            continue;
        }
        std::size_t len;
        std::uint32_t cp;
        std::uint32_t min_cp;
        if ((c & 0xE0) == 0xC0)
        {
            len = 2;
            cp = c & 0x1F;
            min_cp = 0x80;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            len = 3;
            cp = c & 0x0F;
            min_cp = 0x800;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            len = 4;
            cp = c & 0x07;
            min_cp = 0x10000;
        }
        else
            return false;
        if (i + len > n)
            return false;
        for (std::size_t k = 1; k < len; ++k)
        {
            if ((p[i + k] & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if (cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += len;
    }
    return true;
}

std::uint32_t Cp1251ToCodePoint(unsigned char ch)
{
    if (ch < 0x80)
        return ch;
    if (ch >= 0xC0)
        return 0x0410u + (ch - 0xC0u);
    return kCp1251High[ch - 0x80];
}

void AppendUtf8(String& out, std::uint32_t cp)
{
    if (cp < 0x80)
        out += static_cast<char>(cp);
    else if (cp < 0x800)
    {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool Utf8ToCp1251(std::string_view text, String& out)
{
    out.clear();
    out.reserve(text.size());
    const auto* p = reinterpret_cast<const unsigned char*>(text.data());
    const std::size_t n = text.size();
    std::size_t i = 0;
    while (i < n)
    {
        const unsigned char c = p[i];
        if (c < 0x80)
        {
            out += static_cast<char>(c);
            ++i;
            continue;
        }
        std::uint32_t cp = 0;
        std::size_t len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        if (i + len > n)
            return false;
        cp = c & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
        for (std::size_t k = 1; k < len; ++k)
            cp = (cp << 6) | (p[i + k] & 0x3F);
        i += len;
        int found = -1;
        for (int b = 0x80; b <= 0xFF; ++b)
        {
            if (Cp1251ToCodePoint(static_cast<unsigned char>(b)) == cp)
            {
                found = b;
                break;
            }
        }
        if (found < 0)
            return false;
        out += static_cast<char>(found);
    }
    return true;
}

std::string_view Trim(std::string_view s)
{
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && IsTrimChar(s[b]))
        ++b;
    while (e > b && IsTrimChar(s[e - 1]))
        --e;
    return s.substr(b, e - b);
}

String CollapseSpaces(std::string_view s)
{
    s = Trim(s);
    String out;
    out.reserve(s.size());
    bool space = false;
    for (const char ch : s)
    {
        if (IsTrimChar(ch))
        {
            space = true;
            continue;
        }
        if (space)
            out += ' ';
        space = false;
        out += ch;
    }
    return out;
}

String ToLower(std::string_view s)
{
    String out(s.data(), s.size());
    for (char& ch : out)
    {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    }
    return out;
}
} // namespace text

const char* EncodingName(Encoding enc)
{
    switch (enc)
    {
    case Encoding::Ascii: return "ascii";
    case Encoding::Utf8: return "utf-8";
    case Encoding::Cp1251: return "cp1251";
    case Encoding::Cp1251Lossy: return "cp1251-lossy";
    }
    return "ascii";
}

const char* EolName(Eol eol)
{
    switch (eol)
    {
    case Eol::None: return "none";
    case Eol::Lf: return "lf";
    case Eol::Crlf: return "crlf";
    case Eol::Mixed: return "mixed";
    }
    return "none";
}

String Normalize(std::string_view bytes, NormInfo* info)
{
    NormInfo local;
    local.bom = bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF;
    if (local.bom)
        bytes.remove_prefix(3);

    String decoded;
    if (text::IsAscii(bytes))
    {
        local.enc = Encoding::Ascii;
        decoded.assign(bytes.data(), bytes.size());
    }
    else if (text::IsUtf8(bytes))
    {
        local.enc = Encoding::Utf8;
        decoded.assign(bytes.data(), bytes.size());
    }
    else
    {
        local.enc = Encoding::Cp1251;
        decoded.reserve(bytes.size() * 2);
        for (const char ch : bytes)
        {
            std::uint32_t cp = text::Cp1251ToCodePoint(static_cast<unsigned char>(ch));
            if (cp == 0 && ch != 0)
            {
                cp = 0xFFFD;
                local.enc = Encoding::Cp1251Lossy;
            }
            text::AppendUtf8(decoded, cp);
        }
    }

    std::size_t crlf = 0;
    std::size_t lf = 0;
    for (std::size_t i = 0; i < decoded.size(); ++i)
    {
        if (decoded[i] != '\n')
            continue;
        if (i > 0 && decoded[i - 1] == '\r')
            ++crlf;
        else
            ++lf;
    }
    local.eol = crlf && lf ? Eol::Mixed : crlf ? Eol::Crlf : lf ? Eol::Lf : Eol::None;

    String out;
    if (crlf)
    {
        out.reserve(decoded.size());
        for (std::size_t i = 0; i < decoded.size(); ++i)
        {
            if (decoded[i] == '\r' && i + 1 < decoded.size() && decoded[i + 1] == '\n')
                continue;
            out += decoded[i];
        }
    }
    else
        out = std::move(decoded);
    if (info)
        *info = local;
    return out;
}

String Denormalize(std::string_view text_in, const NormInfo& like)
{
    String body;
    if (like.enc == Encoding::Cp1251 || like.enc == Encoding::Cp1251Lossy)
    {
        if (!text::Utf8ToCp1251(text_in, body))
            body.assign(text_in.data(), text_in.size()); // a char cp1251 has no byte for: keep UTF-8
    }
    else
        body.assign(text_in.data(), text_in.size());

    String out;
    out.reserve(body.size() + body.size() / 32 + 3);
    if (like.bom)
        out += "\xEF\xBB\xBF";
    if (like.eol == Eol::Crlf)
    {
        for (const char ch : body)
        {
            if (ch == '\n')
                out += '\r';
            out += ch;
        }
    }
    else
        out += body;
    return out;
}

String XmlDeclarationToUtf8(std::string_view text_in)
{
    String out(text_in.data(), text_in.size());
    const std::size_t decl_end = out.find("?>");
    if (out.compare(0, 5, "<?xml") != 0 || decl_end == String::npos)
        return out;
    const char* forms[] = {"encoding=\"windows-1251\"", "encoding='windows-1251'", "encoding=\"Windows-1251\"",
        "encoding='Windows-1251'"};
    for (const char* form : forms)
    {
        const std::size_t pos = out.find(form);
        if (pos != String::npos && pos < decl_end)
        {
            const bool single = form[9] == '\'';
            out.replace(pos, std::strlen(form), single ? "encoding='utf-8'" : "encoding=\"utf-8\"");
            break;
        }
    }
    return out;
}
} // namespace xray::content_merge
