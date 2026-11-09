#pragma once

// Internal helpers of the text normalization (TextNorm.cpp). The public part is in ContentMerge.h.

#include "ContentMerge.h"

namespace xray::content_merge::text
{
// Strict UTF-8 as Python decodes it: no overlong forms, no surrogates, nothing above U+10FFFF
bool IsUtf8(std::string_view bytes);
bool IsAscii(std::string_view bytes);
// A char of cp1251 -> its code point, 0 for the undefined byte 0x98
std::uint32_t Cp1251ToCodePoint(unsigned char ch);
void AppendUtf8(String& out, std::uint32_t code_point);
// UTF-8 -> cp1251; false if a char has no cp1251 byte
bool Utf8ToCp1251(std::string_view text, String& out);

// The bytes <= ' ' (as _Trim of the engine: spaces, tabs, CR/LF and the other control chars)
inline bool IsTrimChar(char ch)
{
    return static_cast<unsigned char>(ch) <= ' ';
}

std::string_view Trim(std::string_view s);
// Trimmed, every run of the trim chars inside collapsed to one space: a line as the diff by lines compares it
String CollapseSpaces(std::string_view s);
String ToLower(std::string_view s); // ASCII only, as the engine lowercases names
} // namespace xray::content_merge::text
