// The base of gw_merge plan besides a folder (base_source.h): a snapshot of the catalog, its texts, a list of files.

#include "base_source.h"

#include "ContentMerge.h"
#include "DeltaJson.h"
#include "Plan.h"
#include "TextNorm.h"

#include "miniz.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace gw_merge
{
namespace
{
namespace cm = xray::content_merge;
namespace json = xray::content_merge::json;
namespace fs = std::filesystem;

fs::path PathOf(const std::string& text)
{
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(text.begin(), text.end()));
#else
    return fs::u8path(text);
#endif
}

std::string Number(std::uint64_t value) { return std::to_string(value); }

// A whole file of at most `limit` bytes
bool ReadBounded(const std::string& path, std::uint64_t limit, std::string& out, std::string& error)
{
    std::error_code ec;
    const fs::path file = PathOf(path);
    if (!fs::is_regular_file(file, ec))
    {
        error = "'" + path + "' is not a file";
        return false;
    }
    const std::uintmax_t size = fs::file_size(file, ec);
    if (ec)
    {
        error = "cannot read '" + path + "'";
        return false;
    }
    if (size > limit)
    {
        error = "'" + path + "' is larger than " + Number(limit) + " bytes";
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    out.resize(std::size_t(size));
    if (!in || (size && !in.read(&out[0], std::streamsize(size))))
    {
        error = "cannot read '" + path + "'";
        return false;
    }
    return true;
}

// ---- gzip (RFC 1952) over the raw inflate of miniz -----------------------------------------------------------------

class Gunzip
{
public:
    Gunzip(std::string_view packed, std::uint64_t limit) : in_(packed), limit_(limit) {}
    ~Gunzip()
    {
        if (open_)
            mz_inflateEnd(&z_);
    }
    Gunzip(const Gunzip&) = delete;
    Gunzip& operator=(const Gunzip&) = delete;

    // Appends up to `want` bytes of the unpacked data to `out`; false - broken data or over the limit (Error()). Less
    // than `want` - the end of the data (Done())
    bool Read(std::size_t want, std::string& out)
    {
        while (want && !done_)
        {
            if (!open_)
            {
                if (pos_ == in_.size())
                {
                    if (!members_)
                        return Fail("no data");
                    done_ = true;
                    break;
                }
                if (!Begin())
                    return false;
            }
            const std::size_t old = out.size();
            const std::size_t chunk = (std::min)(want, std::size_t(1) << 20);
            out.resize(old + chunk);
            const std::size_t available = (std::min)(in_.size() - pos_, std::size_t(UINT_MAX / 2));
            z_.next_in = reinterpret_cast<const unsigned char*>(in_.data() + pos_);
            z_.avail_in = static_cast<unsigned int>(available);
            z_.next_out = reinterpret_cast<unsigned char*>(&out[old]);
            z_.avail_out = static_cast<unsigned int>(chunk);
            const int status = mz_inflate(&z_, MZ_SYNC_FLUSH);
            const std::size_t consumed = available - z_.avail_in;
            const std::size_t produced = chunk - z_.avail_out;
            pos_ += consumed;
            out.resize(old + produced);
            crc_ = mz_crc32(crc_, reinterpret_cast<const unsigned char*>(out.data() + old), produced);
            member_size_ = std::uint32_t(member_size_ + produced); // ISIZE: modulo 2^32
            total_ += produced;
            want -= produced;
            if (total_ > limit_)
                return Fail("more than " + Number(limit_) + " bytes unpacked");
            if (status == MZ_STREAM_END)
            {
                if (!Finish())
                    return false;
                continue;
            }
            if (status == MZ_BUF_ERROR && !consumed && !produced)
                return Fail("the data is cut");
            if (status != MZ_OK && status != MZ_BUF_ERROR)
                return Fail("broken deflate data");
        }
        return true;
    }
    bool Done() const { return done_; }
    std::uint64_t Total() const { return total_; }
    const std::string& Error() const { return error_; }

private:
    bool Fail(const std::string& why)
    {
        if (error_.empty())
            error_ = why;
        return false;
    }

    // The header of a member at pos_
    bool Begin()
    {
        const auto* b = reinterpret_cast<const unsigned char*>(in_.data());
        const std::size_t size = in_.size();
        std::size_t p = pos_;
        if (size - p < 10 || b[p] != 0x1F || b[p + 1] != 0x8B || b[p + 2] != 8)
            return Fail(members_ ? "data after the gzip stream" : "not gzip data");
        const unsigned flags = b[p + 3];
        if (flags & 0xE0)
            return Fail("reserved flags of the gzip header");
        p += 10;
        if (flags & 0x04) // FEXTRA
        {
            if (size - p < 2)
                return Fail("the gzip header is cut");
            const std::size_t extra = std::size_t(b[p]) | (std::size_t(b[p + 1]) << 8);
            p += 2;
            if (size - p < extra)
                return Fail("the gzip header is cut");
            p += extra;
        }
        for (const unsigned zero_terminated : {0x08u, 0x10u}) // FNAME, FCOMMENT
        {
            if (!(flags & zero_terminated))
                continue;
            const void* end = std::memchr(b + p, 0, size - p);
            if (!end)
                return Fail("the gzip header is cut");
            p = std::size_t(static_cast<const unsigned char*>(end) - b) + 1;
        }
        if (flags & 0x02) // FHCRC
        {
            if (size - p < 2)
                return Fail("the gzip header is cut");
            p += 2;
        }
        pos_ = p;
        std::memset(&z_, 0, sizeof z_);
        if (mz_inflateInit2(&z_, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK)
            return Fail("inflate cannot start");
        open_ = true;
        ++members_;
        crc_ = MZ_CRC32_INIT;
        member_size_ = 0;
        return true;
    }

    // The trailer of a member: CRC32 and ISIZE of the unpacked data
    bool Finish()
    {
        mz_inflateEnd(&z_);
        open_ = false;
        const auto* b = reinterpret_cast<const unsigned char*>(in_.data());
        if (in_.size() - pos_ < 8)
            return Fail("the gzip trailer is cut");
        const auto le32 = [b](std::size_t at) {
            return std::uint32_t(b[at]) | (std::uint32_t(b[at + 1]) << 8) | (std::uint32_t(b[at + 2]) << 16) |
                (std::uint32_t(b[at + 3]) << 24);
        };
        if (le32(pos_) != std::uint32_t(crc_))
            return Fail("CRC32 of the gzip data does not match");
        if (le32(pos_ + 4) != member_size_)
            return Fail("the size of the gzip data does not match");
        pos_ += 8;
        return true;
    }

    std::string_view in_;
    std::size_t pos_ = 0;
    std::uint64_t limit_;
    std::uint64_t total_ = 0;
    mz_stream z_{};
    bool open_ = false;
    bool done_ = false;
    std::size_t members_ = 0;
    mz_ulong crc_ = MZ_CRC32_INIT;
    std::uint32_t member_size_ = 0;
    std::string error_;
};

// ---- the JSON of a snapshot: the members of the top level the plan reads, the rest skipped unparsed --------------------

void SkipSpaces(std::string_view text, std::size_t& i)
{
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r'))
        ++i;
}

bool SkipString(std::string_view text, std::size_t& i)
{
    // text[i] == '"'
    for (++i; i < text.size(); ++i)
    {
        if (text[i] == '\\')
            ++i;
        else if (text[i] == '"')
        {
            ++i;
            return true;
        }
    }
    return false;
}

// The end of the value at i (the brackets matched, the strings skipped; the values read are parsed strictly after)
bool SkipValue(std::string_view text, std::size_t& i)
{
    SkipSpaces(text, i);
    if (i >= text.size())
        return false;
    if (text[i] == '"')
        return SkipString(text, i);
    if (text[i] != '{' && text[i] != '[')
    {
        const std::size_t begin = i;
        while (i < text.size() && std::strchr(",}] \t\r\n", text[i]) == nullptr)
            ++i;
        return i > begin;
    }
    std::string open;
    while (i < text.size())
    {
        const char c = text[i];
        if (c == '"')
        {
            if (!SkipString(text, i))
                return false;
            continue;
        }
        if (c == '{' || c == '[')
            open += c == '{' ? '}' : ']';
        else if (c == '}' || c == ']')
        {
            if (open.empty() || open.back() != c)
                return false;
            open.pop_back();
            if (open.empty())
            {
                ++i;
                return true;
            }
        }
        ++i;
    }
    return false;
}

bool IsSha256(std::string& text)
{
    text = cm::AsciiLower(text);
    return cm::IsSha256Hex(text);
}

bool UnsignedOf(const json::Value* value, std::uint64_t& out)
{
    if (!value || value->type != json::Type::Number || value->text.empty() || value->text.size() > 19)
        return false;
    std::uint64_t number = 0;
    for (const char c : value->text)
    {
        if (c < '0' || c > '9')
            return false;
        number = number * 10 + std::uint64_t(c - '0');
    }
    out = number;
    return true;
}

const std::string* StringOf(const json::Value* value)
{
    return value && value->type == json::Type::String ? &value->text : nullptr;
}
} // namespace

std::string SafeVpath(std::string_view name, bool lower_case)
{
    if (name.empty() || name[0] == '/' || name[0] == '\\' || !cm::text::IsUtf8(name))
        return std::string();
    std::string out;
    std::size_t start = 0;
    while (start <= name.size())
    {
        std::size_t end = name.find_first_of("/\\", start);
        if (end == std::string_view::npos)
            end = name.size();
        const std::string_view part = name.substr(start, end - start);
        if (part.empty() || part == "..")
            return std::string();
        if (std::any_of(part.begin(), part.end(),
                [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == ':' || c == 0x7F; }))
            return std::string();
        if (part != ".")
        {
            if (!out.empty())
                out += '\\';
            out.append(part.data(), part.size());
        }
        start = end + 1;
    }
    return lower_case ? cm::AsciiLower(out) : out;
}

bool ReadSnapshot(const std::string& path, BaseIndex& out, std::string& error)
{
    out = BaseIndex();
    std::string text;
    {
        std::string packed;
        if (!ReadBounded(path, kMaxPackedBytes, packed, error))
            return false;
        Gunzip gz(packed, kMaxSnapshotJson);
        while (!gz.Done())
        {
            if (!gz.Read(std::size_t(16) << 20, text))
            {
                if (gz.Total() > kMaxSnapshotJson)
                {
                    error = "the snapshot '" + path + "' unpacks to more than " + Number(kMaxSnapshotJson >> 20) +
                        " MiB of JSON (the limit of a base snapshot of gw_snapshot.py; a real one is 30-90 MB): not " +
                        "a base snapshot";
                }
                else
                    error = "the snapshot '" + path + "': " + gz.Error();
                return false;
            }
        }
    }

    // The members of the top level: schema, schema_version, game, files, texts - parsed (json::Parse: strict UTF-8,
    // no repeated name inside); the rest (ltx, lua, strings ...) skipped
    std::map<std::string, json::Value> taken;
    std::size_t i = 0;
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0)
        i = 3;
    SkipSpaces(text, i);
    const auto broken = [&](const std::string& why) {
        error = "the snapshot '" + path + "' is not valid JSON: " + why + " (offset " + Number(i) + ")";
        return false;
    };
    if (i >= text.size() || text[i] != '{')
        return broken("an object is expected");
    ++i;
    SkipSpaces(text, i);
    if (i < text.size() && text[i] == '}')
        ++i;
    else
    {
        for (;;)
        {
            SkipSpaces(text, i);
            const std::size_t name_begin = i;
            if (i >= text.size() || text[i] != '"' || !SkipString(text, i))
                return broken("a member name is expected");
            json::Value name;
            std::string parse_error;
            if (!json::Parse(std::string_view(text).substr(name_begin, i - name_begin), name, parse_error))
                return broken(parse_error);
            SkipSpaces(text, i);
            if (i >= text.size() || text[i] != ':')
                return broken("':' is expected");
            ++i;
            SkipSpaces(text, i);
            const std::size_t value_begin = i;
            if (!SkipValue(text, i))
                return broken("a value is expected");
            const std::string& key = name.text;
            if (key == "schema" || key == "schema_version" || key == "game" || key == "files" || key == "texts")
            {
                if (taken.count(key))
                    return broken("the member \"" + key + "\" is repeated");
                json::Value value;
                if (!json::Parse(std::string_view(text).substr(value_begin, i - value_begin), value, parse_error))
                    return broken("\"" + key + "\": " + parse_error);
                taken.emplace(key, std::move(value));
            }
            SkipSpaces(text, i);
            if (i < text.size() && text[i] == ',')
            {
                ++i;
                continue;
            }
            if (i < text.size() && text[i] == '}')
            {
                ++i;
                break;
            }
            return broken("',' or '}' is expected");
        }
    }
    SkipSpaces(text, i);
    if (i != text.size())
        return broken("data after the object");
    text = std::string(); // the memory back before the files are taken

    const auto member = [&taken](const char* key) -> const json::Value* {
        const auto it = taken.find(key);
        return it == taken.end() ? nullptr : &it->second;
    };
    const std::string* schema = StringOf(member("schema"));
    if (!schema || *schema != "gw-base-snapshot")
    {
        error = "'" + path + "' is not a base snapshot (\"schema\" is not \"gw-base-snapshot\")";
        return false;
    }
    std::uint64_t version = 0;
    if (!UnsignedOf(member("schema_version"), version) || version != 1)
    {
        error = "the snapshot '" + path + "': \"schema_version\" is not 1 (the version this gw_merge reads)";
        return false;
    }
    if (const json::Value* game = member("game"); game && game->type == json::Type::Object)
    {
        if (const std::string* tag = StringOf(game->Find("tag")))
            out.game_tag = *tag;
    }
    const json::Value* files = member("files");
    if (!files || files->type != json::Type::Object)
    {
        error = "the snapshot '" + path + "' has no \"files\" object";
        return false;
    }
    for (const auto& [vpath, value] : files->members)
    {
        const std::string name = SafeVpath(vpath, true);
        if (name.empty() || value.type != json::Type::Array || value.items.size() < 2 || out.files.count(name))
        {
            ++out.skipped;
            continue;
        }
        BaseFile& file = out.files[name];
        file.has_size = UnsignedOf(&value.items[0], file.size);
        if (const std::string* sha = StringOf(&value.items[1]))
        {
            std::string lower = *sha;
            if (IsSha256(lower))
                file.sha256 = lower;
        }
    }
    const json::Value* texts = member("texts");
    if (!texts || texts->type != json::Type::Object)
        return true; // no texts: existence only
    if (const json::Value* storage = texts->Find("storage"); storage && storage->type == json::Type::Object)
    {
        if (const std::string* mode = StringOf(storage->Find("mode")))
            out.texts_mode = *mode;
        if (const std::string* file = StringOf(storage->Find("file")))
        {
            // A plain name next to the snapshot only
            if (!file->empty() && file->find_first_of("/\\:") == std::string::npos && *file != "." && *file != "..")
                out.texts_file = *file;
        }
        if (const std::string* sha = StringOf(storage->Find("sha256")))
        {
            std::string lower = *sha;
            if (IsSha256(lower))
                out.texts_sha256 = lower;
        }
    }
    const json::Value* text_files = texts->Find("files");
    if (!text_files || text_files->type != json::Type::Object)
        return true;
    for (const auto& [vpath, value] : text_files->members)
    {
        const std::string name = SafeVpath(vpath, true);
        const auto it = name.empty() ? out.files.end() : out.files.find(name);
        if (it == out.files.end() || value.type != json::Type::Object || it->second.text)
            continue; // a text of no file of the snapshot: not a file of the base
        BaseFile& file = it->second;
        file.text = true;
        file.enc = value.StringOf("enc");
        file.eol = value.StringOf("eol");
        file.bom = value.BoolOf("bom");
        std::string sha = value.StringOf("sha256");
        if (IsSha256(sha))
            file.text_sha256 = sha;
        if (out.texts_mode == "inline")
        {
            if (const std::string* inline_text = StringOf(value.Find("text")))
                out.inline_texts[name] = *inline_text;
        }
    }
    return true;
}

bool ReadTextsTar(const std::string& path, const std::string& expected_sha256, const std::set<std::string>& wanted,
    std::map<std::string, std::string>& out, std::size_t& skipped, std::string& error)
{
    out.clear();
    skipped = 0;
    std::string packed;
    if (!ReadBounded(path, kMaxPackedBytes, packed, error))
        return false;
    if (!expected_sha256.empty() && cm::Sha256Hex(packed) != expected_sha256)
    {
        error = "'" + path + "' is not the texts of the snapshot (its sha256 is not \"texts\" -> \"storage\" -> "
                "\"sha256\" of the snapshot)";
        return false;
    }
    Gunzip gz(packed, kMaxTarStream);
    std::string buffer;
    std::size_t at = 0;
    // Exactly `count` bytes of the stream into `into` (nullptr - skipped); false - the stream ended or broke
    const auto take = [&](std::uint64_t count, std::string* into) {
        while (count)
        {
            if (at == buffer.size())
            {
                buffer.clear();
                at = 0;
                if (!gz.Read(std::size_t(1) << 20, buffer) || buffer.empty())
                    return false;
            }
            const std::size_t part = std::size_t((std::min)(count, std::uint64_t(buffer.size() - at)));
            if (into)
                into->append(buffer, at, part);
            at += part;
            count -= part;
        }
        return true;
    };
    const auto fail = [&](const std::string& why) {
        error = "'" + path + "': " + (gz.Error().empty() ? why : gz.Error());
        return false;
    };
    // An octal field (spaces / NUL around), or the base-256 form of GNU tar for a big number
    const auto number_of = [](const unsigned char* field, std::size_t size, std::uint64_t& value) {
        value = 0;
        if (field[0] & 0x80)
        {
            for (std::size_t k = 1; k < size; ++k)
            {
                if (value >> 55)
                    return false;
                value = (value << 8) | field[k];
            }
            return (field[0] & 0x7F) == 0;
        }
        std::size_t k = 0;
        while (k < size && field[k] == ' ')
            ++k;
        bool digits = false;
        for (; k < size && field[k] >= '0' && field[k] <= '7'; ++k)
        {
            if (value >> 60)
                return false;
            value = value * 8 + (field[k] - '0');
            digits = true;
        }
        for (; k < size; ++k)
        {
            if (field[k] != ' ' && field[k] != 0)
                return false;
        }
        return digits;
    };
    const auto field_text = [](const unsigned char* field, std::size_t size) {
        std::size_t length = 0;
        while (length < size && field[length])
            ++length;
        return std::string(reinterpret_cast<const char*>(field), length);
    };

    std::uint64_t kept = 0;
    std::string pax_path, long_name;
    bool pax_has_size = false;
    std::uint64_t pax_size = 0;
    for (;;)
    {
        std::string header;
        if (!take(512, &header))
        {
            if (!gz.Error().empty() || !header.empty())
                return fail("the tar is cut");
            break; // the end of the stream at a header: taken as the end of the archive
        }
        const auto* h = reinterpret_cast<const unsigned char*>(header.data());
        if (std::all_of(header.begin(), header.end(), [](char c) { return c == 0; }))
            break; // the end of the archive
        std::uint64_t checksum = 0;
        if (!number_of(h + 148, 8, checksum))
            return fail("a broken tar header (the checksum field)");
        std::uint64_t sum = 0;
        std::int64_t signed_sum = 0;
        for (std::size_t k = 0; k < 512; ++k)
        {
            const unsigned char c = k >= 148 && k < 156 ? ' ' : h[k];
            sum += c;
            signed_sum += static_cast<signed char>(c);
        }
        if (checksum != sum && std::int64_t(checksum) != signed_sum)
            return fail("a broken tar header (the checksum does not match)");
        std::uint64_t size = 0;
        if (!number_of(h + 124, 12, size))
            return fail("a broken tar header (the size field)");
        const char type = static_cast<char>(h[156]);
        std::string name = field_text(h, 100);
        if (std::memcmp(h + 257, "ustar", 5) == 0)
        {
            const std::string prefix = field_text(h + 345, 155);
            if (!prefix.empty())
                name = prefix + "/" + name;
        }
        const bool meta = type == 'x' || type == 'g' || type == 'L' || type == 'K';
        if (!meta)
        {
            if (!pax_path.empty())
                name = pax_path;
            else if (!long_name.empty())
                name = long_name;
            if (pax_has_size)
                size = pax_size;
            pax_path.clear();
            long_name.clear();
            pax_has_size = false;
        }
        const std::uint64_t padding = (512 - size % 512) % 512;
        if (type == 'x' || type == 'L')
        {
            if (size > (std::uint64_t(1) << 20))
                return fail("a tar extended header is too large");
            std::string data;
            if (!take(size, &data) || !take(padding, nullptr))
                return fail("the tar is cut");
            if (type == 'L')
            {
                long_name = data.substr(0, data.find('\0'));
                continue;
            }
            // PAX records: "<length> <key>=<value>\n", the length counts the whole record
            for (std::size_t p = 0; p < data.size();)
            {
                const std::size_t space = data.find(' ', p);
                std::uint64_t length = 0;
                for (std::size_t k = p; space != std::string::npos && k < space && length < data.size(); ++k)
                {
                    if (data[k] < '0' || data[k] > '9')
                    {
                        length = 0;
                        break;
                    }
                    length = length * 10 + std::uint64_t(data[k] - '0');
                }
                if (space == std::string::npos || length <= space - p + 1 || length > data.size() - p ||
                    data[p + std::size_t(length) - 1] != '\n')
                    return fail("a broken PAX header of the tar");
                const std::string record = data.substr(space + 1, p + std::size_t(length) - 1 - (space + 1));
                const std::size_t eq = record.find('=');
                if (eq != std::string::npos)
                {
                    const std::string key = record.substr(0, eq);
                    const std::string value = record.substr(eq + 1);
                    if (key == "path")
                        pax_path = value;
                    else if (key == "size")
                    {
                        std::uint64_t parsed = 0;
                        bool digits = !value.empty() && value.size() <= 19;
                        for (const char c : value)
                        {
                            digits = digits && c >= '0' && c <= '9';
                            parsed = parsed * 10 + std::uint64_t(c - '0');
                        }
                        if (!digits)
                            return fail("a broken PAX size of the tar");
                        pax_has_size = true;
                        pax_size = parsed;
                    }
                }
                p += std::size_t(length);
            }
            continue;
        }
        const bool regular = type == '0' || type == '\0' || type == '7';
        const std::string vpath = regular ? SafeVpath(name, true) : std::string();
        if (regular && vpath.empty())
            ++skipped;
        if (!regular || vpath.empty() || !wanted.count(vpath) || out.count(vpath))
        {
            if (!take(size + padding, nullptr))
                return fail("the tar is cut");
            continue;
        }
        if (size > kMaxTextBytes)
        {
            ++skipped;
            if (!take(size + padding, nullptr))
                return fail("the tar is cut");
            continue;
        }
        kept += size;
        if (kept > kMaxTextsKept)
            return fail("the texts taken are larger than " + Number(kMaxTextsKept) + " bytes");
        std::string& text = out[vpath];
        text.reserve(std::size_t(size));
        if (!take(size, &text) || !take(padding, nullptr))
            return fail("the tar is cut");
    }
    return true;
}

bool ReadBaseList(const std::string& path, bool lower_case, BaseIndex& out, std::string& error)
{
    out = BaseIndex();
    std::string data;
    if (!ReadBounded(path, kMaxListBytes, data, error))
        return false;
    std::size_t start = data.size() >= 3 && data.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    while (start < data.size())
    {
        std::size_t end = data.find('\n', start);
        if (end == std::string::npos)
            end = data.size();
        std::string line = data.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        if (line[0] == '#')
        {
            constexpr std::string_view format_word = "# gw_base_files ";
            constexpr std::string_view tag_word = "# game_tag ";
            if (line.compare(0, format_word.size(), format_word) == 0)
            {
                const std::string format = line.substr(format_word.size());
                if (format != "1")
                {
                    error = "the list '" + path + "' is of format " + format + ", this gw_merge reads format 1";
                    return false;
                }
            }
            else if (line.compare(0, tag_word.size(), tag_word) == 0)
            {
                std::string tag = line.substr(tag_word.size());
                while (!tag.empty() && (tag.back() == ' ' || tag.back() == '\t'))
                    tag.pop_back();
                // A tag that is not UTF-8 text is not taken: it goes into the JSON of the plan as it is
                if (cm::text::IsUtf8(tag) && std::none_of(tag.begin(), tag.end(), [](char c) {
                        return static_cast<unsigned char>(c) < 0x20 || c == 0x7F;
                    }))
                    out.game_tag = tag;
                else
                    ++out.skipped;
            }
            continue;
        }
        const std::size_t tab = line.find('\t');
        const std::string name = SafeVpath(std::string_view(line).substr(0, tab), lower_case);
        if (name.empty() || out.files.count(name))
        {
            ++out.skipped;
            continue;
        }
        BaseFile& file = out.files[name];
        if (tab != std::string::npos)
        {
            std::string sha = line.substr(tab + 1);
            while (!sha.empty() && (sha.back() == ' ' || sha.back() == '\t'))
                sha.pop_back();
            if (IsSha256(sha))
                file.sha256 = sha;
            else
                ++out.bad_sha;
        }
    }
    return true;
}
} // namespace gw_merge
