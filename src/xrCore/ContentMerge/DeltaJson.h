#pragma once

// A small JSON reader and writer without dependencies (15-merge-implementation.md section 2.3): the delta of the
// catalog (section 7.2) is JSON, the engine has no JSON parser. Internal to the library: the public part is
// DeltaToJson / DeltaFromJson of ContentMerge.h.
//   - the members of an object keep their order (the writer gives them in that order, the reader keeps the order of
//     the text): written, read and written again - the same bytes;
//   - a number is kept as written (the delta has small integers only);
//   - the strings are UTF-8; the writer escapes '"', '\\' and the control chars only (as Python json.dumps with
//     ensure_ascii=False), the reader takes every escape of RFC 8259 (\uXXXX with the surrogate pairs too).

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xray::content_merge::json
{
enum class Type : std::uint8_t
{
    Null,
    Bool,
    Number,
    String,
    Array,
    Object,
};

struct Value
{
    Type type{Type::Null};
    bool boolean{};
    std::string text; // String: the value; Number: as written
    std::vector<Value> items; // Array
    std::vector<std::pair<std::string, Value>> members; // Object, in order

    static Value MakeString(std::string_view s);
    static Value MakeBool(bool b);
    static Value MakeNumber(long long n);
    static Value MakeArray();
    static Value MakeObject();

    // Object: the member (the first one of the name), nullptr - none or not an object
    const Value* Find(std::string_view key) const;
    // Object: appends a member (no check of a repeated name)
    Value& Add(std::string_view key, Value value);

    // The value of a member of the type, else the fallback
    std::string StringOf(std::string_view key, std::string_view fallback = {}) const;
    bool BoolOf(std::string_view key, bool fallback = false) const;
    long long NumberOf(std::string_view key, long long fallback = 0) const;
};

// The whole text is one value (spaces around it allowed). False - error: the message with the line and the column
bool Parse(std::string_view text, Value& out, std::string& error);

// Pretty printed: `indent` spaces per level, "key": value, a LF at the end. An empty array / object - [] / {}
std::string Write(const Value& value, int indent = 2);

// A JSON string literal of s (with the quotes)
std::string Quote(std::string_view s);
} // namespace xray::content_merge::json
