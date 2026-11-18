/*
 * GlobalWar Plugin API - C++ helpers for plugins (header-only).
 *
 * Compiled into the plugin; the engine never sees these types. Everything here is built on top of the
 * C ABI in gwp_api.h, so using or not using this header does not change binary compatibility.
 *
 * Requires C++20 (std::format): MSVC 19.29+, GCC 13+, Clang 17+ with libc++.
 * Docs: wiki/doc/plugins/api/plugin.md (gwp::Plugin), logger.md, events.md, save.md
 */
#ifndef GWP_HPP
#define GWP_HPP

#ifndef __cplusplus
#error "gwp/gwp.hpp is a C++ header; C plugins use gwp/gwp_api.h directly"
#endif

#include "gwp_api.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <format>
#include <functional>
#include <initializer_list>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace gwp
{
/*
 * Logger: formatted messages to the engine log with compile-time checked format strings.
 *
 *     gwp::Logger log{api, self};
 *     log.info("tracked {} npc", count);        // * [plugin:<addon id>] tracked 42 npc
 *     log.debug("cache size {}", cache.size()); // formatted only when the game runs with -addon_debug
 *
 * Format syntax is std::format ("{}", "{:.2f}", "{:>8}", ...). A wrong number of arguments or an argument
 * that does not match the format spec is a compile error, not a crash in the game.
 *
 * All methods are noexcept: logging never throws into plugin code. If formatting fails at runtime
 * (practically only std::bad_alloc), a fixed "failed to format a message" line is written instead.
 *
 * Thread safety: every method may be called from any thread (task_parallel_for pieces included): the text is
 * formatted into a buffer of the call and passed to the thread-safe api->log.
 */
class Logger
{
public:
    // Tags for xray-16/src/gw_plugins/gen_plugin_api_index.py: "@group <name> @thread main|any" in the comment right above
    // a public method (applies up to the next empty line). The plugins build fails on an untagged method.

    // Empty logger / logger bound to the engine. @group logger @thread any
    Logger() noexcept = default;
    Logger(const GwpEngineApi* api, const GwpPlugin* self) noexcept : m_api(api), m_self(self) {}

    // Binds the logger later, e.g. when it is a global created before gwp_plugin_init runs.
    // @group logger @thread any
    void reset(const GwpEngineApi* api, const GwpPlugin* self) noexcept
    {
        m_api = api;
        m_self = self;
    }

    // False before reset()/construction with a valid engine table; such a logger silently drops messages.
    // @group logger @thread any
    bool valid() const noexcept { return m_api != nullptr && m_api->log != nullptr; }

    // True when GWP_LOG_DEBUG messages reach the log. Use it to skip expensive debug-only work.
    // @group logger @thread any
    bool debug_enabled() const noexcept
    {
        // A level set with set_level decides (the engine filters by it the same way)
        if (m_level >= GWP_LOG_DEBUG)
            return m_level == GWP_LOG_DEBUG;
        return m_api != nullptr && GWP_API_HAS(m_api, is_debug_log) && m_api->is_debug_log != nullptr &&
            m_api->is_debug_log() != 0;
    }

    // The lowest level of the lines of this plugin (log_set_level of the engine, plans/addon_api_ideas P57):
    // GWP_LOG_DEBUG shows its debug lines without -addon_debug, GWP_LOG_WARNING hides its info lines... Errors always
    // pass. false: an engine without log_set_level, or a level above GWP_LOG_ERROR. @group logger @thread any
    bool set_level(GwpLogLevel level) noexcept
    {
        if (m_api == nullptr || !GWP_API_HAS(m_api, log_set_level) || m_api->log_set_level(m_self, level) != GWP_OK)
            return false;
        m_level = level;
        return true;
    }

    // GWP_LOG_DEBUG; not even formatted when debug output is off. @group logger @thread any
    template <typename... Args>
    void debug(std::format_string<Args...> fmt, Args&&... args) const noexcept
    {
        if (debug_enabled()) // do not even format the text when it would be dropped
            print(GWP_LOG_DEBUG, fmt, std::forward<Args>(args)...);
    }

    // GWP_LOG_INFO. @group logger @thread any
    template <typename... Args>
    void info(std::format_string<Args...> fmt, Args&&... args) const noexcept
    {
        print(GWP_LOG_INFO, fmt, std::forward<Args>(args)...);
    }

    // GWP_LOG_WARNING. @group logger @thread any
    template <typename... Args>
    void warn(std::format_string<Args...> fmt, Args&&... args) const noexcept
    {
        print(GWP_LOG_WARNING, fmt, std::forward<Args>(args)...);
    }

    // GWP_LOG_ERROR. @group logger @thread any
    template <typename... Args>
    void error(std::format_string<Args...> fmt, Args&&... args) const noexcept
    {
        print(GWP_LOG_ERROR, fmt, std::forward<Args>(args)...);
    }

    // Level chosen at runtime. @group logger @thread any
    template <typename... Args>
    void print(GwpLogLevel level, std::format_string<Args...> fmt, Args&&... args) const noexcept
    {
        if (!valid())
            return;
        try
        {
            write(level, std::format(fmt, std::forward<Args>(args)...));
        }
        catch (...)
        {
            // Formatting failed at runtime (practically only std::bad_alloc). format_string::get() is C++23,
            // so the template itself cannot be printed portably under C++20.
            m_api->log(m_self, level, "gwp::Logger: failed to format a message");
        }
    }

    // Text as is, without formatting: braces in `text` are not interpreted. @group logger @thread any
    void write(GwpLogLevel level, std::string_view text) const noexcept
    {
        if (!valid())
            return;
        try
        {
            const std::string buffer(text); // the C ABI needs a NUL-terminated string
            m_api->log(m_self, level, buffer.c_str());
        }
        catch (...)
        {
            m_api->log(m_self, GWP_LOG_ERROR, "gwp::Logger: out of memory while writing a message");
        }
    }

private:
    const GwpEngineApi* m_api = nullptr;
    const GwpPlugin* m_self = nullptr;
    GwpLogLevel m_level = -1; // set_level: -1 - not set (the engine default)
};
/*
 * ValueView: typed read access to one GwpValue (an argument of an event or an export, an item of a GWP_T_ARRAY, the
 * value of data_get).
 *
 *     const gwp::ValueView list = event.value(0);   // a Lua table {1, 2, 3} comes as GWP_T_ARRAY
 *     for (uint32_t i = 0; i < list.size(); ++i)
 *         total += list.item(i).number();
 *
 * Every getter returns the fallback when the value is missing (nullptr) or has another type, so a reader never
 * reads a wrong union member. Numbers from Lua always come as GWP_T_NUMBER: number() and integer() accept both
 * GWP_T_NUMBER and GWP_T_INT. A type this header does not know reads as a missing value.
 */
class ValueView
{
public:
    // Wraps a value; nullptr = a missing value (every getter gives its fallback). @group events @thread any
    explicit ValueView(const GwpValue* value = nullptr) noexcept : m_value(value) {}

    // Type of the value (GwpValueType); GWP_T_NIL when missing. @group events @thread any
    uint32_t type() const noexcept { return m_value ? m_value->type : GWP_T_NIL; }

    // A number: GWP_T_NUMBER or GWP_T_INT. @group events @thread any
    double number(double fallback = 0.0) const noexcept
    {
        if (m_value && m_value->type == GWP_T_NUMBER)
            return m_value->u.n;
        if (m_value && m_value->type == GWP_T_INT)
            return static_cast<double>(m_value->u.i);
        return fallback;
    }

    // An integer: GWP_T_INT or a GWP_T_NUMBER without a fractional part. @group events @thread any
    int64_t integer(int64_t fallback = 0) const noexcept
    {
        if (m_value && m_value->type == GWP_T_INT)
            return m_value->u.i;
        if (m_value && m_value->type == GWP_T_NUMBER && std::trunc(m_value->u.n) == m_value->u.n &&
            std::fabs(m_value->u.n) < 9.0e15)
            return static_cast<int64_t>(m_value->u.n);
        return fallback;
    }

    // A bool (GWP_T_BOOL). @group events @thread any
    bool boolean(bool fallback = false) const noexcept
    {
        return m_value && m_value->type == GWP_T_BOOL ? m_value->u.b != 0 : fallback;
    }

    // A string (GWP_T_STRING); empty for other types. Valid as long as the value. @group events @thread any
    std::string_view string() const noexcept
    {
        if (m_value && m_value->type == GWP_T_STRING && m_value->u.s.ptr)
            return std::string_view(m_value->u.s.ptr, m_value->u.s.len);
        return std::string_view();
    }

    // Binary data (GWP_T_BYTES); empty for other types. Valid as long as the value. @group events @thread any
    std::span<const uint8_t> bytes() const noexcept
    {
        if (m_value && m_value->type == GWP_T_BYTES && m_value->u.s.ptr)
            return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(m_value->u.s.ptr), m_value->u.s.len);
        return std::span<const uint8_t>();
    }

    // An online object id (GWP_T_OBJECT); GWP_INVALID_OBJECT_ID for other types. @group events @thread any
    GwpObjectId object() const noexcept
    {
        return m_value && m_value->type == GWP_T_OBJECT ? m_value->u.id : GWP_INVALID_OBJECT_ID;
    }

    // An ALife server object id (GWP_T_SERVER_OBJECT). @group events @thread any
    GwpObjectId server_object() const noexcept
    {
        return m_value && m_value->type == GWP_T_SERVER_OBJECT ? m_value->u.id : GWP_INVALID_OBJECT_ID;
    }

    // A 3D vector (GWP_T_VEC3); false and `out` untouched for other types. @group events @thread any
    bool vec3(float out[3]) const noexcept
    {
        if (!m_value || m_value->type != GWP_T_VEC3 || !out)
            return false;
        out[0] = m_value->u.v[0];
        out[1] = m_value->u.v[1];
        out[2] = m_value->u.v[2];
        return true;
    }

    // True for GWP_T_ARRAY. @group events @thread any
    bool is_array() const noexcept { return m_value && m_value->type == GWP_T_ARRAY; }

    // Items of a GWP_T_ARRAY (a Lua table 1..n); empty for other types. @group events @thread any
    std::span<const GwpValue> array() const noexcept
    {
        if (is_array() && m_value->u.a.items)
            return std::span<const GwpValue>(m_value->u.a.items, m_value->u.a.count);
        return std::span<const GwpValue>();
    }

    // Number of items of a GWP_T_ARRAY; 0 for other types. @group events @thread any
    uint32_t size() const noexcept { return static_cast<uint32_t>(array().size()); }

    // Item of a GWP_T_ARRAY (0-based: item(0) is t[1] in Lua); missing when out of range. @group events @thread any
    ValueView item(uint32_t index) const noexcept
    {
        const std::span<const GwpValue> items = array();
        return ValueView(index < items.size() ? &items[index] : nullptr);
    }

    // The wrapped value (nullptr when missing). @group events @thread any
    const GwpValue* raw() const noexcept { return m_value; }

private:
    const GwpValue* m_value = nullptr;
};

/*
 * EventView: typed read access to the arguments of an event inside a GwpEventHandler.
 *
 *     void GWP_CALL on_death(void* user, const GwpEvent* raw)
 *     {
 *         const gwp::EventView event(raw);
 *         const GwpObjectId victim = event.object(0); // npc_on_death_callback(victim, who)
 *         const GwpObjectId killer = event.object(1);
 *     }
 *
 * Every getter returns the fallback when the index is out of range or the argument has another type,
 * so a handler never reads a wrong union member (the getters are those of ValueView, by index).
 * Numbers from Lua always come as GWP_T_NUMBER: number() and integer() accept both GWP_T_NUMBER and GWP_T_INT.
 */
class EventView
{
public:
    // Wraps the event given to a handler; valid only during the handler call. @group events @thread main
    explicit EventView(const GwpEvent* event) noexcept : m_event(event) {}

    // An event record around plain arguments (those of a GwpExportFn, say), to read them with EventView:
    //     const GwpEvent event = gwp::EventView::args(argc, argv);
    //     const gwp::EventView view(&event);
    // @group events @thread any
    static GwpEvent args(uint32_t argc, const GwpValue* argv) noexcept
    {
        GwpEvent event{};
        event.size = sizeof(GwpEvent);
        event.id = GWP_INVALID_EVENT_ID;
        event.name = "";
        event.argc = argv ? argc : 0;
        event.argv = argv;
        return event;
    }

    // Event name, e.g. "actor_on_reinit". @group events @thread main
    std::string_view name() const noexcept { return m_event && m_event->name ? m_event->name : ""; }

    // Number of arguments. @group events @thread main
    uint32_t size() const noexcept { return m_event ? m_event->argc : 0; }

    // True when the engine emitted the event (a built-in engine event), false for Lua and plugins.
    // @group events @thread main
    bool from_engine() const noexcept { return m_event && (m_event->flags & GWP_EVENT_FLAG_ENGINE) != 0; }

    // The argument as a ValueView (missing when index >= size()). @group events @thread main
    ValueView value(uint32_t index) const noexcept { return ValueView(at(index)); }

    // Type of the argument (GwpValueType); GWP_T_NIL when index >= size(). @group events @thread main
    uint32_t type(uint32_t index) const noexcept { return value(index).type(); }

    // Argument as a number: GWP_T_NUMBER or GWP_T_INT. @group events @thread main
    double number(uint32_t index, double fallback = 0.0) const noexcept { return value(index).number(fallback); }

    // Argument as an integer: GWP_T_INT or a GWP_T_NUMBER without a fractional part. @group events @thread main
    int64_t integer(uint32_t index, int64_t fallback = 0) const noexcept { return value(index).integer(fallback); }

    // Argument as a bool (GWP_T_BOOL). @group events @thread main
    bool boolean(uint32_t index, bool fallback = false) const noexcept { return value(index).boolean(fallback); }

    // Argument as a string (GWP_T_STRING); empty for other types. Valid only during the handler call.
    // @group events @thread main
    std::string_view string(uint32_t index) const noexcept { return value(index).string(); }

    // Argument as binary data (GWP_T_BYTES); empty for other types. Valid only during the handler call.
    // @group events @thread main
    std::span<const uint8_t> bytes(uint32_t index) const noexcept { return value(index).bytes(); }

    // Argument as an array (GWP_T_ARRAY, a Lua table 1..n); empty for other types. Valid only during the handler
    // call. @group events @thread main
    std::span<const GwpValue> array(uint32_t index) const noexcept { return value(index).array(); }

    // Argument as an online object id (GWP_T_OBJECT); GWP_INVALID_OBJECT_ID for other types.
    // @group events @thread main
    GwpObjectId object(uint32_t index) const noexcept { return value(index).object(); }

    // Argument as an ALife server object id (GWP_T_SERVER_OBJECT). @group events @thread main
    GwpObjectId server_object(uint32_t index) const noexcept { return value(index).server_object(); }

    // Argument as a 3D vector (GWP_T_VEC3); false and `out` untouched for other types. @group events @thread main
    bool vec3(uint32_t index, float out[3]) const noexcept { return value(index).vec3(out); }

    // True when the event carries a result (actor_on_before_hit, actor_on_before_death, ...).
    // @group events @thread main
    bool has_result() const noexcept { return m_event && m_event->result; }

    // Current result as a bool; fallback when there is no bool result. @group events @thread main
    bool result_bool(bool fallback = true) const noexcept
    {
        return has_result() && m_event->result->type == GWP_T_BOOL ? m_event->result->u.b != 0 : fallback;
    }

    // Current result as a number (GWP_T_NUMBER or GWP_T_INT); fallback for another type or without a result:
    // the hit power of npc_on_before_hit / monster_on_before_hit. @group events @thread main
    double result_number(double fallback = 0.0) const noexcept { return result().number(fallback); }

    // Current result as a ValueView (missing without a result). @group events @thread main
    ValueView result() const noexcept { return ValueView(has_result() ? m_event->result : nullptr); }

    // Sets the result, e.g. set_result(false) cancels a hit in actor_on_before_hit. No-op without a result.
    // @group events @thread main
    void set_result(bool value) const noexcept
    {
        if (!has_result())
            return;
        m_event->result->type = GWP_T_BOOL;
        m_event->result->u.b = value ? 1 : 0;
    }

    // set_result with a number would silently become a bool: use set_result_number. @group events @thread main
    template <typename T>
    void set_result(T value) const = delete;

    // Sets a number result, e.g. the new hit power in npc_on_before_hit / monster_on_before_hit (set_result(false)
    // cancels the hit there). No-op without a result. @group events @thread main
    void set_result_number(double value) const noexcept
    {
        if (!has_result())
            return;
        *m_event->result = GwpValue{};
        m_event->result->type = GWP_T_NUMBER;
        m_event->result->u.n = value;
    }

    // Sets any result value. A string, bytes or an array must outlive the dispatch of the event (a literal, a
    // member of the plugin): the engine reads it after the handler returns. No-op without a result.
    // @group events @thread main
    void set_result_value(const GwpValue& value) const noexcept
    {
        if (has_result())
            *m_event->result = value;
    }

private:
    const GwpValue* at(uint32_t index) const noexcept
    {
        return m_event && m_event->argv && index < m_event->argc ? &m_event->argv[index] : nullptr;
    }

    const GwpEvent* m_event = nullptr;
};

/*
 * Value: builds GwpValue arguments for api->event_emit.
 *
 *     const GwpValue args[] = { gwp::Value::object(npc_id), gwp::Value::number(42.0) };
 *     api->event_emit(self, my_event_id, 2, args, nullptr);
 */
class Value
{
public:
    // Empty argument (Lua nil). @group events @thread any
    static GwpValue nil() noexcept
    {
        GwpValue v{};
        v.type = GWP_T_NIL;
        return v;
    }

    // @group events @thread any
    static GwpValue boolean(bool b) noexcept
    {
        GwpValue v = nil();
        v.type = GWP_T_BOOL;
        v.u.b = b ? 1 : 0;
        return v;
    }

    // @group events @thread any
    static GwpValue integer(int64_t i) noexcept
    {
        GwpValue v = nil();
        v.type = GWP_T_INT;
        v.u.i = i;
        return v;
    }

    // @group events @thread any
    static GwpValue number(double n) noexcept
    {
        GwpValue v = nil();
        v.type = GWP_T_NUMBER;
        v.u.n = n;
        return v;
    }

    // The text is not copied: it must outlive the event_emit call. @group events @thread any
    static GwpValue string(std::string_view text) noexcept
    {
        GwpValue v = nil();
        v.type = GWP_T_STRING;
        v.u.s.ptr = text.data();
        v.u.s.len = static_cast<uint32_t>(text.size());
        return v;
    }

    // @group events @thread any
    static GwpValue vec3(float x, float y, float z) noexcept
    {
        GwpValue v = nil();
        v.type = GWP_T_VEC3;
        v.u.v[0] = x;
        v.u.v[1] = y;
        v.u.v[2] = z;
        return v;
    }

    // Online game object by id. @group events @thread any
    static GwpValue object(GwpObjectId id) noexcept
    {
        GwpValue v = nil();
        v.type = GWP_T_OBJECT;
        v.u.id = id;
        return v;
    }

    // ALife server object by id. @group events @thread any
    static GwpValue server_object(GwpObjectId id) noexcept
    {
        GwpValue v = nil();
        v.type = GWP_T_SERVER_OBJECT;
        v.u.id = id;
        return v;
    }

    // Binary data (a Lua string with these bytes). Not copied: must outlive the call. @group events @thread any
    static GwpValue bytes(const void* data, uint32_t size) noexcept
    {
        GwpValue v = nil();
        v.type = GWP_T_BYTES;
        v.u.s.ptr = static_cast<const char*>(data);
        v.u.s.len = data ? size : 0;
        return v;
    }

    // An array (a Lua table 1..n in Lua). The items are not copied: they must outlive the call; an item may be an
    // array itself (nesting up to 8). @group events @thread any
    static GwpValue array(std::span<const GwpValue> items) noexcept
    {
        GwpValue v = nil();
        v.type = GWP_T_ARRAY;
        v.u.a.items = items.data();
        v.u.a.count = static_cast<uint32_t>(items.size());
        return v;
    }
};
/*
 * BinaryWriter / BinaryReader: byte buffers for the plugin block of the game save (save_write / save_read) and
 * other binary data.
 *
 *     gwp::BinaryWriter out;
 *     out.write<uint32_t>(deaths);
 *     out.write_string(last_victim);
 *     api->save_write(self, kSaveVersion, out.data(), out.size());
 *
 *     gwp::BinaryReader in(bytes, size);
 *     const uint32_t deaths = in.read_or<uint32_t>(0);
 *     std::string last_victim;
 *     in.read_string(last_victim);
 *     if (!in.ok()) { ... truncated or corrupted: use defaults ... }
 *
 * Values are stored in the byte order of the machine (little endian on every platform the game runs on).
 * The reader never reads past the end: a failed read sets ok() to false, and every later read fails too.
 * With gwp::Plugin the writer and the reader come to on_save / on_load ready-made.
 *
 * Tagged chunks: a layout that another version of the plugin can read. A chunk is u32 tag + u32 length + bytes; a
 * reader takes the chunks it knows and skips the others whole, so a new version may add chunks (and append fields
 * at the end of a chunk) without breaking an older reader, and an older block reads in a newer version with the
 * defaults for the missing chunks:
 *
 *     out.chunk(gwp::make_tag("KILL"), [&](gwp::BinaryWriter& w) { w.write<uint32_t>(deaths); });
 *     out.chunk(gwp::make_tag("LAST"), [&](gwp::BinaryWriter& w) { w.write_string(last_victim); });
 *
 *     uint32_t tag = 0;
 *     gwp::BinaryReader body;
 *     while (in.next_chunk(tag, body))
 *     {
 *         if (tag == gwp::make_tag("KILL"))
 *             deaths = body.read_or<uint32_t>(0);
 *         else if (tag == gwp::make_tag("LAST"))
 *             body.read_string(last_victim);
 *         // an unknown tag: skipped
 *     }
 */

// A chunk tag from four characters: make_tag("KILL"). The tags are compared as numbers, no text is stored.
constexpr uint32_t make_tag(const char (&name)[5]) noexcept
{
    return static_cast<uint32_t>(static_cast<uint8_t>(name[0])) |
        static_cast<uint32_t>(static_cast<uint8_t>(name[1])) << 8 |
        static_cast<uint32_t>(static_cast<uint8_t>(name[2])) << 16 |
        static_cast<uint32_t>(static_cast<uint8_t>(name[3])) << 24;
}

class BinaryWriter
{
public:
    // Any trivially copyable value: integers, float/double, bool, plain structs. @group save @thread any
    template <typename T>
    void write(const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>, "BinaryWriter::write needs a trivially copyable type");
        write_bytes(&value, sizeof(T));
    }

    // Raw bytes as they are. @group save @thread any
    void write_bytes(const void* data, size_t size)
    {
        if (!data || size == 0)
            return;
        const auto* bytes = static_cast<const uint8_t*>(data);
        m_buffer.insert(m_buffer.end(), bytes, bytes + size);
    }

    // u32 length + bytes, no terminating zero; read back with BinaryReader::read_string. @group save @thread any
    void write_string(std::string_view text)
    {
        write<uint32_t>(static_cast<uint32_t>(text.size()));
        write_bytes(text.data(), text.size());
    }

    // Opens a tagged chunk: writes the tag and a length to be filled by end_chunk(mark). Chunks may nest.
    // @group save @thread any
    size_t begin_chunk(uint32_t tag)
    {
        write<uint32_t>(tag);
        const size_t mark = m_buffer.size();
        write<uint32_t>(0);
        return mark;
    }

    // Closes the chunk begin_chunk opened: its length = the bytes written since. @group save @thread any
    void end_chunk(size_t mark) noexcept
    {
        if (mark + sizeof(uint32_t) > m_buffer.size())
            return;
        const uint32_t length = static_cast<uint32_t>(m_buffer.size() - mark - sizeof(uint32_t));
        std::memcpy(m_buffer.data() + mark, &length, sizeof(length));
    }

    // A whole chunk: fn(BinaryWriter&) writes its body. @group save @thread any
    template <typename F>
    void chunk(uint32_t tag, F&& fn)
    {
        const size_t mark = begin_chunk(tag);
        fn(*this);
        end_chunk(mark);
    }

    // Written bytes, e.g. for save_write(self, version, data(), size()). @group save @thread any
    const void* data() const noexcept { return m_buffer.data(); }
    uint32_t size() const noexcept { return static_cast<uint32_t>(m_buffer.size()); }
    bool empty() const noexcept { return m_buffer.empty(); }
    void clear() noexcept { m_buffer.clear(); }

private:
    std::vector<uint8_t> m_buffer;
};

class BinaryReader
{
public:
    // Reads from a buffer that outlives the reader (e.g. the pointer of save_read). The empty reader has no data
    // (a target for next_chunk). @group save @thread any
    BinaryReader() noexcept = default;
    BinaryReader(const void* data, size_t size) noexcept
        : m_data(static_cast<const uint8_t*>(data)), m_size(data ? size : 0) {}

    // The next tagged chunk (BinaryWriter::chunk): its tag and a reader of its body; the position moves past the
    // whole chunk, whatever is read from `body`. False at the end of the data; false and ok() = false when the
    // chunk is cut. @group save @thread any
    bool next_chunk(uint32_t& tag, BinaryReader& body)
    {
        if (!m_ok || remaining() == 0)
            return false;
        uint32_t length = 0;
        if (!read(tag) || !read(length) || length > remaining())
        {
            m_ok = false;
            return false;
        }
        body = BinaryReader(m_data + m_pos, length);
        m_pos += length;
        return true;
    }

    // Value written by BinaryWriter::write; false (and ok() = false) when the data ends. @group save @thread any
    template <typename T>
    bool read(T& out)
    {
        static_assert(std::is_trivially_copyable_v<T>, "BinaryReader::read needs a trivially copyable type");
        return read_bytes(&out, sizeof(T));
    }

    // The value, or fallback when the data ends (ok() becomes false). @group save @thread any
    template <typename T>
    T read_or(T fallback)
    {
        T value;
        return read(value) ? value : fallback;
    }

    // Raw bytes; false when fewer than `size` are left. @group save @thread any
    bool read_bytes(void* out, size_t size)
    {
        if (!m_ok || size > remaining())
        {
            m_ok = false;
            return false;
        }
        if (size)
            std::memcpy(out, m_data + m_pos, size); // memcpy: the source has no alignment guarantee
        m_pos += size;
        return true;
    }

    // String written by BinaryWriter::write_string. @group save @thread any
    bool read_string(std::string& out)
    {
        uint32_t length = 0;
        if (!read(length) || length > remaining())
        {
            m_ok = false;
            return false;
        }
        out.assign(reinterpret_cast<const char*>(m_data + m_pos), length);
        m_pos += length;
        return true;
    }

    // False after a read that did not fit: the data is truncated or has another layout. @group save @thread any
    bool ok() const noexcept { return m_ok; }
    size_t remaining() const noexcept { return m_size - m_pos; }

private:
    const uint8_t* m_data = nullptr;
    size_t m_size = 0;
    size_t m_pos = 0;
    bool m_ok = true;
};

using EventFn = std::function<void(const EventView&)>;
using BatchFn = std::function<void(std::span<const GwpEvent>)>;

/*
 * Zone: one pass through a place of the plugin code, shown in the Tracy profiler (RAII: ends in the destructor).
 * The place is registered once, usually by the GWP_ZONE macro below:
 *
 *     bool NpcVsHeli::evaluate(GwpObjectId npc)
 *     {
 *         GWP_ZONE(m_api, m_self, zone, "npc_vs_heli/evaluate");
 *         zone.text_number(npc); // the NPC id next to the zone
 *         ...
 *     }
 *
 * Without Tracy in the engine (or an engine without the functions) everything here does nothing.
 */
class Zone
{
public:
    // Registers a place; GWP_ZONE keeps the result in a static. @group plugin @thread main
    static GwpZoneSite site(const GwpEngineApi* api, const GwpPlugin* self, const char* name, const char* file,
        uint32_t line, uint32_t color = 0) noexcept
    {
        if (!GWP_API_HAS(api, profile_zone_end))
            return GWP_INVALID_ZONE_SITE;
        return api->profile_zone_site(self, name, file, line, color);
    }

    // Begins the zone of a registered place. @group plugin @thread any
    Zone(const GwpEngineApi* api, GwpZoneSite site) noexcept
    {
        if (site != GWP_INVALID_ZONE_SITE && GWP_API_HAS(api, profile_zone_end))
        {
            m_api = api;
            m_zone = api->profile_zone_begin(site);
        }
    }

    // @group plugin @thread any
    ~Zone()
    {
        if (m_zone)
            m_api->profile_zone_end(m_zone);
    }

    // Not copyable: one zone, one end. @group plugin @thread any
    Zone(const Zone&) = delete;
    Zone& operator=(const Zone&) = delete;

    // A text next to the zone: a name, a state. @group plugin @thread any
    void text(std::string_view value) const noexcept
    {
        if (m_zone)
            m_api->profile_zone_text(m_zone, value.data(), static_cast<uint32_t>(value.size()));
    }

    // A number as the text (an object id, a count). @group plugin @thread any
    void text_number(uint64_t value) const noexcept
    {
        if (!m_zone)
            return;
        char buffer[24];
        char* end = buffer + sizeof(buffer);
        char* p = end;
        do
        {
            *--p = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value);
        m_api->profile_zone_text(m_zone, p, static_cast<uint32_t>(end - p));
    }

private:
    const GwpEngineApi* m_api = nullptr;
    GwpZone m_zone = 0;
};

#define GWP_ZONE_CONCAT_(a, b) a##b
#define GWP_ZONE_CONCAT(a, b) GWP_ZONE_CONCAT_(a, b)
/* A zone named `name` for the rest of the scope, in the variable `var`. The place is registered at the first pass
   (a function-local static): api and self must be the same at every pass, as they are inside one plugin. */
#define GWP_ZONE(api, self, var, name)                                                                     \
    static const GwpZoneSite GWP_ZONE_CONCAT(gwp_zone_site_, __LINE__) =                                  \
        ::gwp::Zone::site((api), (self), (name), __FILE__, static_cast<uint32_t>(__LINE__));               \
    const ::gwp::Zone var((api), GWP_ZONE_CONCAT(gwp_zone_site_, __LINE__))

/*
 * Binder: the plugin's callbacks for the life of the objects of one class or section mask (Plugin::bind).
 * Override what you need; the engine calls the methods on the main thread, in this order for an object:
 * on_reinit, on_spawn, on_update (batches, while the object is scheduled), on_destroy.
 *
 *     struct StalkerCounter : gwp::Binder
 *     {
 *         std::unordered_map<GwpObjectId, uint32_t> updates;
 *         void on_spawn(GwpObjectId id, std::string_view) override { updates[id] = 0; }
 *         void on_destroy(GwpObjectId id) override { updates.erase(id); }
 *         void on_update(std::span<const GwpBinderUpdate> batch) override
 *         {
 *             for (const GwpBinderUpdate& u : batch)
 *                 ++updates[u.id];
 *         }
 *     };
 *     // in on_init:  bind("AI_STL_S", nullptr, m_stalkers);
 *
 * The object must outlive its binding: until unbind, or until the plugin is unloaded (Plugin unbinds everything
 * before on_unload, so members of the plugin class are the natural place).
 * Docs: wiki/doc/plugins/api/binders.md
 */
class Binder
{
public:
    // @group plugin @thread main
    Binder() = default;
    virtual ~Binder() = default;

    // The object was reset before its spawn (engine reinit): comes before on_spawn. @group plugin @thread main
    virtual void on_reinit(GwpObjectId id) { (void)id; }
    // The object is online. section: its ltx section, valid while the object is online. An object that was
    // online when the binder was registered gets on_spawn at once. @group plugin @thread main
    virtual void on_spawn(GwpObjectId id, std::string_view section) { (void)id; (void)section; }
    // The object goes offline (before its Lua binder net_destroy), or the binder is unbound: the last call
    // for this id. @group plugin @thread main
    virtual void on_destroy(GwpObjectId id) { (void)id; }
    // The objects of this binder the scheduler updated since the previous call: once per frame, after the
    // scheduler pass, only when there is at least one record. @group plugin @thread main
    virtual void on_update(std::span<const GwpBinderUpdate> updates) { (void)updates; }

    // True: the binder keeps a state per object (on_save / on_load below). Read once, by Plugin::bind. A plugin may
    // have one such binder per class id and section mask (the engine keeps the state by them).
    // @group plugin @thread main
    virtual bool saves_state() const { return false; }

    // The state of the object to keep: while a save is written, and right before on_destroy (offline, unbind).
    // Nothing written = nothing kept (the kept state is dropped); at most GWP_BINDER_SAVE_MAX_SIZE bytes.
    // @group plugin @thread main
    virtual void on_save(GwpObjectId id, BinaryWriter& out) { (void)id; (void)out; }

    // The state on_save wrote for this object: right before on_spawn, in the same game or after a load. Not called
    // when nothing is kept. Check in.ok() when the layout may differ (BinaryWriter::chunk). @group plugin @thread main
    virtual void on_load(GwpObjectId id, BinaryReader& in) { (void)id; (void)in; }
};

/*
 * ImGui tools (group imgui, wiki/doc/plugins/api/imgui.md): a window of the plugin in the menu "Addons" of the in-game
 * editor (F10). The tool is a class with draw(); gwp::Ui wraps the widget table GwpImGuiApi of the engine:
 *
 *     class CounterTool final : public gwp::ImGuiTool
 *     {
 *     public:
 *         int clicks = 0;
 *         void draw(gwp::Ui& ui, const GwpImGuiFrame&) override
 *         {
 *             ui.text_fmt("Clicks: {}", clicks);        // formatted here (std::format), shown as it is
 *             if (ui.button("Click"))
 *                 ++clicks;
 *             if (auto table = ui.table("t", 2))         // EndTable in the destructor, only when it began
 *             {
 *                 ui.table_next_column(); ui.text("a");
 *                 ui.table_next_column(); ui.text("b");
 *             }
 *         }
 *     };
 *     // in on_init: imgui_tool_register(m_tool, {.id = "my_addon_counter", .title = "Counter"});
 *
 * The scopes (child, table, tree, combo, tab_bar, popup, id, disabled...) are gwp::UiScope: the end function of the
 * pair is called in the destructor and only when the pair needs it, so Begin/End stay balanced by construction, also
 * on an early return or an exception.
 */

// A size or a position in pixels (the getters of gwp::Ui, ImGuiToolOptions).
struct UiVec2
{
    float x = 0.f;
    float y = 0.f;
};

/*
 * UiScope: the end of a pair of gwp::Ui (RAII). Returned by child(), table(), tree()...: true when the content is to
 * be drawn (the begin function returned true); the destructor calls the end function when the pair needs it - always
 * for child / group / id / style / disabled / indent, only after true for table / tree / combo / tab_bar / popup /
 * menu / tooltip.
 */
class UiScope
{
public:
    // An empty scope: false, ends nothing. @group imgui @thread main
    UiScope() noexcept = default;
    // open: what the begin function returned; end: the end function of the table to call in the destructor (nullptr -
    // nothing to end). @group imgui @thread main
    UiScope(bool open, void(GWP_CALL* end)()) noexcept : m_end(end), m_open(open) {}
    // @group imgui @thread main
    ~UiScope() { end(); }
    // Movable (returned by value), not copyable: one end per begin. @group imgui @thread main
    UiScope(UiScope&& other) noexcept : m_end(other.m_end), m_open(other.m_open) { other.m_end = nullptr; }
    UiScope& operator=(UiScope&& other) noexcept
    {
        if (this != &other)
        {
            end();
            m_end = other.m_end;
            m_open = other.m_open;
            other.m_end = nullptr;
        }
        return *this;
    }
    UiScope(const UiScope&) = delete;
    UiScope& operator=(const UiScope&) = delete;

    // True: draw the content of the pair. @group imgui @thread main
    explicit operator bool() const noexcept { return m_open; }

    // Ends the pair now (the destructor does nothing then). @group imgui @thread main
    void end() noexcept
    {
        if (m_end)
        {
            void(GWP_CALL* const fn)() = m_end;
            m_end = nullptr;
            fn();
        }
    }

private:
    void(GWP_CALL* m_end)() = nullptr;
    bool m_open = false;
};

/*
 * Ui: the widgets of ImGui for ImGuiTool::draw, a thin wrapper of GwpImGuiApi (the rules of the table apply: see
 * "ImGui tools of addons" in gwp_api.h). Valid only inside draw. Strings are std::string_view (copied with a NUL: on
 * the stack up to 255 bytes); a text is shown as it is - text_fmt formats it with std::format first. Values are
 * references, changed in place; a widget returns true when the value changed (a button - when pressed). Flags are the
 * constants GWP_IMGUI_*.
 */
class Ui
{
public:
    // The table of the engine (draw gets it); gwp::Plugin makes the Ui for ImGuiTool::draw. @group imgui @thread main
    explicit Ui(const GwpImGuiApi* api) noexcept : m_api(api) {}

    // The raw table: the functions this class does not wrap, GWP_IMGUI_HAS(ui.raw(), field) for the ones appended
    // later. @group imgui @thread main
    const GwpImGuiApi* raw() const noexcept { return m_api; }

    // --- layout ---

    // The next widget on the same line. @group imgui @thread main
    void same_line(float offset_from_start_x = 0.f, float spacing = -1.f) const { m_api->same_line(offset_from_start_x, spacing); }
    // @group imgui @thread main
    void new_line() const { m_api->new_line(); }
    void spacing() const { m_api->spacing(); }
    void separator() const { m_api->separator(); }
    // A separator with a text in it. @group imgui @thread main
    void separator_text(std::string_view label) const { m_api->separator_text(Str(label).c); }
    // An empty space. @group imgui @thread main
    void dummy(float w, float h) const { m_api->dummy(w, h); }
    // The content goes right by w (0 - the style) until the scope ends. @group imgui @thread main
    [[nodiscard]] UiScope indent(float w = 0.f) const
    {
        m_api->indent(w);
        return UiScope(true, m_api->unindent);
    }
    // The widgets of the scope act as one item. @group imgui @thread main
    [[nodiscard]] UiScope group() const
    {
        m_api->begin_group();
        return UiScope(true, m_api->end_group);
    }
    // A scrolling region; w, h 0 - the rest of the window. Ends always (draw its content only when true).
    // @group imgui @thread main
    [[nodiscard]] UiScope child(std::string_view id, float w = 0.f, float h = 0.f, uint32_t child_flags = 0,
        uint32_t window_flags = 0) const
    {
        const bool open = m_api->begin_child(Str(id).c, w, h, child_flags, window_flags) != 0;
        return UiScope(open, m_api->end_child);
    }
    // The width of the next widget (< 0 - the rest of the line minus that much). @group imgui @thread main
    void set_next_item_width(float w) const { m_api->set_next_item_width(w); }
    // The width of every widget of the scope. @group imgui @thread main
    [[nodiscard]] UiScope item_width(float w) const
    {
        m_api->push_item_width(w);
        return UiScope(true, m_api->pop_item_width);
    }
    // A text before a framed widget on the same line. @group imgui @thread main
    void align_text_to_frame_padding() const { m_api->align_text_to_frame_padding(); }
    // The free space from the cursor. @group imgui @thread main
    UiVec2 content_region_avail() const
    {
        UiVec2 v;
        m_api->get_content_region_avail(&v.x, &v.y);
        return v;
    }
    // @group imgui @thread main
    UiVec2 window_size() const
    {
        UiVec2 v;
        m_api->get_window_size(&v.x, &v.y);
        return v;
    }
    // The cursor in the window. @group imgui @thread main
    UiVec2 cursor_pos() const
    {
        UiVec2 v;
        m_api->get_cursor_pos(&v.x, &v.y);
        return v;
    }
    // @group imgui @thread main
    void set_cursor_pos(float x, float y) const { m_api->set_cursor_pos(x, y); }
    // The size of a text; wrap_width > 0 - wrapped. @group imgui @thread main
    UiVec2 calc_text_size(std::string_view text, float wrap_width = 0.f) const
    {
        UiVec2 v;
        m_api->calc_text_size(Str(text).c, wrap_width, &v.x, &v.y);
        return v;
    }
    // @group imgui @thread main
    float text_line_height() const { return m_api->get_text_line_height(); }
    float frame_height() const { return m_api->get_frame_height(); }

    // --- text: shown as it is ---

    // @group imgui @thread main
    void text(std::string_view value) const { m_api->text_n(value.data(), static_cast<uint32_t>(value.size())); }
    // std::format into a buffer of 1024 bytes on the stack (a longer text is cut at a character), then shown as it
    // is: a wrong format is a compile error. @group imgui @thread main
    template <typename... Args>
    void text_fmt(std::format_string<Args...> fmt, Args&&... args) const
    {
        char buffer[1024];
        const auto result = std::format_to_n(buffer, sizeof(buffer), fmt, std::forward<Args>(args)...);
        size_t length = result.size < static_cast<std::ptrdiff_t>(sizeof(buffer)) ? static_cast<size_t>(result.size)
                                                                                  : sizeof(buffer);
        if (result.size > static_cast<std::ptrdiff_t>(sizeof(buffer)))
        {
            // Cut: not in the middle of a UTF-8 character. The last character goes only when its bytes do not all fit
            // (a whole one right at the end stays; a text of exactly 1024 bytes is not cut at all)
            size_t start = length;
            while (start > 0 && (static_cast<unsigned char>(buffer[start - 1]) & 0xC0u) == 0x80u)
                --start;
            if (start > 0)
            {
                const unsigned lead = static_cast<unsigned char>(buffer[start - 1]);
                const size_t need = lead >= 0xF0u ? 4 : lead >= 0xE0u ? 3 : lead >= 0xC0u ? 2 : 1;
                if (start - 1 + need > length)
                    length = start - 1;
            }
        }
        m_api->text_n(buffer, static_cast<uint32_t>(length));
    }
    // 0xAARRGGBB. @group imgui @thread main
    void text_colored(uint32_t argb, std::string_view value) const { m_api->text_colored(argb, Str(value).c); }
    // @group imgui @thread main
    void text_disabled(std::string_view value) const { m_api->text_disabled(Str(value).c); }
    void text_wrapped(std::string_view value) const { m_api->text_wrapped(Str(value).c); }
    void label_text(std::string_view label, std::string_view value) const { m_api->label_text(Str(label).c, Str(value).c); }
    void bullet_text(std::string_view value) const { m_api->bullet_text(Str(value).c); }
    void bullet() const { m_api->bullet(); }
    // Texts of the scope wrap at x (0 - the window edge, < 0 - no wrapping). @group imgui @thread main
    [[nodiscard]] UiScope text_wrap_pos(float wrap_local_pos_x = 0.f) const
    {
        m_api->push_text_wrap_pos(wrap_local_pos_x);
        return UiScope(true, m_api->pop_text_wrap_pos);
    }

    // --- buttons: true when pressed ---

    // @group imgui @thread main
    bool button(std::string_view label, float w = 0.f, float h = 0.f) const { return m_api->button(Str(label).c, w, h) != 0; }
    bool small_button(std::string_view label) const { return m_api->small_button(Str(label).c) != 0; }
    bool invisible_button(std::string_view id, float w, float h) const { return m_api->invisible_button(Str(id).c, w, h) != 0; }
    bool arrow_button(std::string_view id, uint32_t dir) const { return m_api->arrow_button(Str(id).c, dir) != 0; }
    // true when changed. @group imgui @thread main
    bool checkbox(std::string_view label, bool& value) const
    {
        int v = value ? 1 : 0;
        const bool changed = m_api->checkbox(Str(label).c, &v) != 0;
        value = v != 0;
        return changed;
    }
    // Toggles the bits flags_value in flags. @group imgui @thread main
    bool checkbox_flags(std::string_view label, uint32_t& flags, uint32_t flags_value) const
    {
        return m_api->checkbox_flags(Str(label).c, &flags, flags_value) != 0;
    }
    // true when pressed. @group imgui @thread main
    bool radio_button(std::string_view label, bool active) const { return m_api->radio_button(Str(label).c, active ? 1 : 0) != 0; }
    // value becomes button_value when pressed. @group imgui @thread main
    bool radio_button(std::string_view label, int& value, int button_value) const
    {
        const bool pressed = radio_button(label, value == button_value);
        if (pressed)
            value = button_value;
        return pressed;
    }
    // fraction 0..1; overlay empty - the percent. @group imgui @thread main
    void progress_bar(float fraction, std::string_view overlay = "", float w = 0.f, float h = 0.f) const
    {
        const Str text(overlay);
        m_api->progress_bar(fraction, w, h, overlay.empty() ? nullptr : text.c);
    }

    // --- numbers: true when changed; fmt nullptr - the default of ImGui ---

    // @group imgui @thread main
    bool slider_float(std::string_view label, float& v, float v_min, float v_max, const char* fmt = nullptr,
        uint32_t flags = 0) const
    {
        return m_api->slider_float(Str(label).c, &v, v_min, v_max, fmt, flags) != 0;
    }
    // 2..4 components. @group imgui @thread main
    bool slider_float_n(std::string_view label, std::span<float> v, float v_min, float v_max, const char* fmt = nullptr,
        uint32_t flags = 0) const
    {
        return m_api->slider_float_n(Str(label).c, v.data(), static_cast<uint32_t>(v.size()), v_min, v_max, fmt, flags) != 0;
    }
    // @group imgui @thread main
    bool slider_int(std::string_view label, int& v, int v_min, int v_max, const char* fmt = nullptr, uint32_t flags = 0) const
    {
        return m_api->slider_int(Str(label).c, &v, v_min, v_max, fmt, flags) != 0;
    }
    bool slider_int_n(std::string_view label, std::span<int> v, int v_min, int v_max, const char* fmt = nullptr,
        uint32_t flags = 0) const
    {
        return m_api->slider_int_n(Str(label).c, v.data(), static_cast<uint32_t>(v.size()), v_min, v_max, fmt, flags) != 0;
    }
    // Radians, the limits in degrees. @group imgui @thread main
    bool slider_angle(std::string_view label, float& v_rad, float v_degrees_min = -360.f, float v_degrees_max = 360.f,
        const char* fmt = nullptr, uint32_t flags = 0) const
    {
        return m_api->slider_angle(Str(label).c, &v_rad, v_degrees_min, v_degrees_max, fmt, flags) != 0;
    }
    // v_min == v_max - no limits. @group imgui @thread main
    bool drag_float(std::string_view label, float& v, float v_speed = 1.f, float v_min = 0.f, float v_max = 0.f,
        const char* fmt = nullptr, uint32_t flags = 0) const
    {
        return m_api->drag_float(Str(label).c, &v, v_speed, v_min, v_max, fmt, flags) != 0;
    }
    bool drag_float_n(std::string_view label, std::span<float> v, float v_speed = 1.f, float v_min = 0.f,
        float v_max = 0.f, const char* fmt = nullptr, uint32_t flags = 0) const
    {
        return m_api->drag_float_n(Str(label).c, v.data(), static_cast<uint32_t>(v.size()), v_speed, v_min, v_max, fmt,
                   flags) != 0;
    }
    bool drag_int(std::string_view label, int& v, float v_speed = 1.f, int v_min = 0, int v_max = 0,
        const char* fmt = nullptr, uint32_t flags = 0) const
    {
        return m_api->drag_int(Str(label).c, &v, v_speed, v_min, v_max, fmt, flags) != 0;
    }
    bool drag_int_n(std::string_view label, std::span<int> v, float v_speed = 1.f, int v_min = 0, int v_max = 0,
        const char* fmt = nullptr, uint32_t flags = 0) const
    {
        return m_api->drag_int_n(Str(label).c, v.data(), static_cast<uint32_t>(v.size()), v_speed, v_min, v_max, fmt,
                   flags) != 0;
    }
    // step 0 - no +/- buttons; flags GWP_IMGUI_INPUT_*. @group imgui @thread main
    bool input_float(std::string_view label, float& v, float step = 0.f, float step_fast = 0.f,
        const char* fmt = nullptr, uint32_t flags = 0) const
    {
        return m_api->input_float(Str(label).c, &v, step, step_fast, fmt, flags) != 0;
    }
    bool input_float_n(std::string_view label, std::span<float> v, const char* fmt = nullptr, uint32_t flags = 0) const
    {
        return m_api->input_float_n(Str(label).c, v.data(), static_cast<uint32_t>(v.size()), fmt, flags) != 0;
    }
    bool input_int(std::string_view label, int& v, int step = 1, int step_fast = 100, uint32_t flags = 0) const
    {
        return m_api->input_int(Str(label).c, &v, step, step_fast, flags) != 0;
    }

    // --- text input: true when the text changed (GWP_IMGUI_INPUT_ENTER_RETURNS_TRUE - on Enter) ---

    // A buffer of the plugin: cap bytes, NUL-terminated, edited in place. @group imgui @thread main
    bool input_text(std::string_view label, char* buf, uint32_t cap, uint32_t flags = 0) const
    {
        return m_api->input_text(Str(label).c, buf, cap, flags) != 0;
    }
    // A std::string of at most max_bytes bytes (up to GWP_IMGUI_MAX_INPUT_TEXT - 1, a bigger one is clamped). A longer
    // value is shown cut (on a UTF-8 boundary) and stays whole until the player edits it. @group imgui @thread main
    bool input_text(std::string_view label, std::string& value, uint32_t max_bytes = 255, uint32_t flags = 0) const
    {
        size_t copied = 0;
        std::vector<char>& buf = text_buffer(value, max_bytes, GWP_IMGUI_MAX_INPUT_TEXT, copied);
        const bool changed = m_api->input_text(Str(label).c, buf.data(), static_cast<uint32_t>(buf.size()), flags) != 0;
        take_text(buf, copied, value);
        return changed;
    }
    // The same with a hint shown while the text is empty. @group imgui @thread main
    bool input_text_with_hint(std::string_view label, std::string_view hint, std::string& value,
        uint32_t max_bytes = 255, uint32_t flags = 0) const
    {
        size_t copied = 0;
        std::vector<char>& buf = text_buffer(value, max_bytes, GWP_IMGUI_MAX_INPUT_TEXT, copied);
        const bool changed = m_api->input_text_with_hint(Str(label).c, Str(hint).c, buf.data(),
                                 static_cast<uint32_t>(buf.size()), flags) != 0;
        take_text(buf, copied, value);
        return changed;
    }
    // Several lines, at most max_bytes bytes (up to GWP_IMGUI_MAX_INPUT_TEXT_MULTILINE - 1, a bigger one is clamped);
    // w, h 0 - auto. @group imgui @thread main
    bool input_text_multiline(std::string_view label, std::string& value, uint32_t max_bytes = 4095, float w = 0.f,
        float h = 0.f, uint32_t flags = 0) const
    {
        size_t copied = 0;
        std::vector<char>& buf = text_buffer(value, max_bytes, GWP_IMGUI_MAX_INPUT_TEXT_MULTILINE, copied);
        const bool changed = m_api->input_text_multiline(Str(label).c, buf.data(), static_cast<uint32_t>(buf.size()), w,
                                 h, flags) != 0;
        take_text(buf, copied, value);
        return changed;
    }

    // --- choice ---

    // current: the index in items; true when changed. @group imgui @thread main
    bool combo(std::string_view label, int& current, std::span<const char* const> items,
        int popup_max_height_in_items = -1) const
    {
        return m_api->combo(Str(label).c, &current, items.data(), static_cast<uint32_t>(items.size()),
                   popup_max_height_in_items) != 0;
    }
    // A combo with its own content (selectable items): draw it when true. @group imgui @thread main
    [[nodiscard]] UiScope combo(std::string_view label, std::string_view preview, uint32_t flags = 0) const
    {
        const bool open = m_api->begin_combo(Str(label).c, Str(preview).c, flags) != 0;
        return UiScope(open, open ? m_api->end_combo : nullptr);
    }
    // true when clicked. @group imgui @thread main
    bool selectable(std::string_view label, bool selected = false, uint32_t flags = 0, float w = 0.f, float h = 0.f) const
    {
        return m_api->selectable(Str(label).c, selected ? 1 : 0, flags, w, h) != 0;
    }
    // current: the index in items; true when changed. @group imgui @thread main
    bool list_box(std::string_view label, int& current, std::span<const char* const> items, int height_in_items = -1) const
    {
        return m_api->list_box(Str(label).c, &current, items.data(), static_cast<uint32_t>(items.size()),
                   height_in_items) != 0;
    }
    // A list box with its own content: draw it when true. @group imgui @thread main
    [[nodiscard]] UiScope list_box(std::string_view label, float w = 0.f, float h = 0.f) const
    {
        const bool open = m_api->begin_list_box(Str(label).c, w, h) != 0;
        return UiScope(open, open ? m_api->end_list_box : nullptr);
    }

    // --- color: 0..1, true when changed ---

    // @group imgui @thread main
    bool color_edit3(std::string_view label, float (&rgb)[3], uint32_t flags = 0) const
    {
        return m_api->color_edit3(Str(label).c, rgb, flags) != 0;
    }
    bool color_edit4(std::string_view label, float (&rgba)[4], uint32_t flags = 0) const
    {
        return m_api->color_edit4(Str(label).c, rgba, flags) != 0;
    }
    // A color square (0xAARRGGBB); true when pressed. @group imgui @thread main
    bool color_button(std::string_view id, uint32_t argb, uint32_t flags = 0, float w = 0.f, float h = 0.f) const
    {
        return m_api->color_button(Str(id).c, argb, flags, w, h) != 0;
    }

    // --- trees ---

    // A tree node: its children when true (tree_pop in the destructor). @group imgui @thread main
    [[nodiscard]] UiScope tree(std::string_view label, uint32_t flags = 0) const
    {
        const bool open = m_api->tree_node_ex(Str(label).c, flags) != 0;
        return UiScope(open, open ? m_api->tree_pop : nullptr);
    }
    // true - open (nothing to end). @group imgui @thread main
    bool collapsing_header(std::string_view label, uint32_t flags = 0) const
    {
        return m_api->collapsing_header(Str(label).c, flags) != 0;
    }
    // The next tree node / header opens or closes (cond GWP_IMGUI_COND_*, 0 - always). @group imgui @thread main
    void set_next_item_open(bool open, uint32_t cond = 0) const { m_api->set_next_item_open(open ? 1 : 0, cond); }

    // --- tabs ---

    // @group imgui @thread main
    [[nodiscard]] UiScope tab_bar(std::string_view id, uint32_t flags = 0) const
    {
        const bool open = m_api->begin_tab_bar(Str(id).c, flags) != 0;
        return UiScope(open, open ? m_api->end_tab_bar : nullptr);
    }
    // The tab is selected: its content when true. open: nullptr - no close button, else false after a close.
    // @group imgui @thread main
    [[nodiscard]] UiScope tab_item(std::string_view label, bool* open = nullptr, uint32_t flags = 0) const
    {
        int keep = open && *open ? 1 : 0;
        const bool selected = m_api->begin_tab_item(Str(label).c, open ? &keep : nullptr, flags) != 0;
        if (open)
            *open = keep != 0;
        return UiScope(selected, selected ? m_api->end_tab_item : nullptr);
    }

    // --- tables ---

    // columns 1..511; the rows when true (end_table in the destructor). @group imgui @thread main
    [[nodiscard]] UiScope table(std::string_view id, int columns, uint32_t flags = 0, float outer_w = 0.f,
        float outer_h = 0.f) const
    {
        const bool open = m_api->begin_table(Str(id).c, columns, flags, outer_w, outer_h) != 0;
        return UiScope(open, open ? m_api->end_table : nullptr);
    }
    // @group imgui @thread main
    void table_next_row(uint32_t row_flags = 0, float min_row_height = 0.f) const { m_api->table_next_row(row_flags, min_row_height); }
    bool table_next_column() const { return m_api->table_next_column() != 0; }
    bool table_set_column_index(int column) const { return m_api->table_set_column_index(column) != 0; }
    void table_setup_column(std::string_view label, uint32_t flags = 0, float init_width_or_weight = 0.f) const
    {
        m_api->table_setup_column(Str(label).c, flags, init_width_or_weight);
    }
    void table_setup_scroll_freeze(int cols, int rows) const { m_api->table_setup_scroll_freeze(cols, rows); }
    void table_headers_row() const { m_api->table_headers_row(); }
    // The columns and the header row in one call: table_setup_column for every label, then table_headers_row.
    // @group imgui @thread main
    void table_headers(std::initializer_list<std::string_view> labels) const
    {
        for (const std::string_view label : labels)
            table_setup_column(label);
        table_headers_row();
    }

    // --- plots: the engine takes at most GWP_IMGUI_MAX_PLOT_VALUES values ---

    // @group imgui @thread main
    void plot_lines(std::string_view label, std::span<const float> values, float scale_min = GWP_IMGUI_AUTO_SCALE,
        float scale_max = GWP_IMGUI_AUTO_SCALE, float w = 0.f, float h = 0.f, std::string_view overlay = "",
        uint32_t values_offset = 0) const
    {
        const Str text(overlay);
        m_api->plot_lines(Str(label).c, values.data(), static_cast<uint32_t>(values.size()), values_offset,
            overlay.empty() ? nullptr : text.c, scale_min, scale_max, w, h);
    }
    void plot_histogram(std::string_view label, std::span<const float> values, float scale_min = GWP_IMGUI_AUTO_SCALE,
        float scale_max = GWP_IMGUI_AUTO_SCALE, float w = 0.f, float h = 0.f, std::string_view overlay = "",
        uint32_t values_offset = 0) const
    {
        const Str text(overlay);
        m_api->plot_histogram(Str(label).c, values.data(), static_cast<uint32_t>(values.size()), values_offset,
            overlay.empty() ? nullptr : text.c, scale_min, scale_max, w, h);
    }

    // --- tooltips ---

    // @group imgui @thread main
    void set_tooltip(std::string_view value) const { m_api->set_tooltip(Str(value).c); }
    // The tooltip of the last item (when it is hovered). @group imgui @thread main
    void set_item_tooltip(std::string_view value) const { m_api->set_item_tooltip(Str(value).c); }

    // --- clipboard (write only) ---

    // The text goes to the clipboard of the OS (at most 64 KiB, cut on a UTF-8 boundary); the clipboard is never read.
    // Appended to GwpImGuiApi later: false - this engine has no set_clipboard_text (nothing is copied).
    // @group imgui @thread main
    bool set_clipboard_text(std::string_view value) const
    {
        if (!GWP_IMGUI_HAS(m_api, set_clipboard_text))
            return false;
        m_api->set_clipboard_text(Str(value).c);
        return true;
    }
    // Widgets in a tooltip. @group imgui @thread main
    [[nodiscard]] UiScope tooltip() const
    {
        const bool open = m_api->begin_tooltip() != 0;
        return UiScope(open, open ? m_api->end_tooltip : nullptr);
    }
    [[nodiscard]] UiScope item_tooltip() const
    {
        const bool open = m_api->begin_item_tooltip() != 0;
        return UiScope(open, open ? m_api->end_tooltip : nullptr);
    }

    // --- popups ---

    // @group imgui @thread main
    void open_popup(std::string_view id, uint32_t popup_flags = 0) const { m_api->open_popup(Str(id).c, popup_flags); }
    // The popup while it is open. @group imgui @thread main
    [[nodiscard]] UiScope popup(std::string_view id, uint32_t window_flags = 0) const
    {
        const bool open = m_api->begin_popup(Str(id).c, window_flags) != 0;
        return UiScope(open, open ? m_api->end_popup : nullptr);
    }
    // A modal popup with a title; open: nullptr - no close button. @group imgui @thread main
    [[nodiscard]] UiScope popup_modal(std::string_view name, bool* open = nullptr, uint32_t window_flags = 0) const
    {
        int keep = open && *open ? 1 : 0;
        const bool shown = m_api->begin_popup_modal(Str(name).c, open ? &keep : nullptr, window_flags) != 0;
        if (open)
            *open = keep != 0;
        return UiScope(shown, shown ? m_api->end_popup : nullptr);
    }
    // A popup on a click on the last item (the right button by default); id empty - the id of the last item.
    // @group imgui @thread main
    [[nodiscard]] UiScope popup_context_item(std::string_view id = "", uint32_t popup_flags = 0) const
    {
        const Str text(id);
        const bool open = m_api->begin_popup_context_item(id.empty() ? nullptr : text.c, popup_flags) != 0;
        return UiScope(open, open ? m_api->end_popup : nullptr);
    }
    // @group imgui @thread main
    void close_current_popup() const { m_api->close_current_popup(); }
    bool is_popup_open(std::string_view id, uint32_t popup_flags = 0) const
    {
        return m_api->is_popup_open(Str(id).c, popup_flags) != 0;
    }

    // --- menus ---

    // The menu bar of the window (GWP_IMGUI_WINDOW_MENU_BAR). @group imgui @thread main
    [[nodiscard]] UiScope menu_bar() const
    {
        const bool open = m_api->begin_menu_bar() != 0;
        return UiScope(open, open ? m_api->end_menu_bar : nullptr);
    }
    // @group imgui @thread main
    [[nodiscard]] UiScope menu(std::string_view label, bool enabled = true) const
    {
        const bool open = m_api->begin_menu(Str(label).c, enabled ? 1 : 0) != 0;
        return UiScope(open, open ? m_api->end_menu : nullptr);
    }
    // true when clicked; selected: nullptr - no check mark, else toggled. @group imgui @thread main
    bool menu_item(std::string_view label, std::string_view shortcut = "", bool* selected = nullptr,
        bool enabled = true) const
    {
        const Str keys(shortcut);
        int value = selected && *selected ? 1 : 0;
        const bool clicked = m_api->menu_item(Str(label).c, shortcut.empty() ? nullptr : keys.c,
                                 selected ? &value : nullptr, enabled ? 1 : 0) != 0;
        if (selected)
            *selected = value != 0;
        return clicked;
    }

    // --- state ---

    // @group imgui @thread main
    bool is_item_hovered(uint32_t flags = 0) const { return m_api->is_item_hovered(flags) != 0; }
    bool is_item_active() const { return m_api->is_item_active() != 0; }
    bool is_item_clicked(int mouse_button = GWP_IMGUI_MOUSE_LEFT) const { return m_api->is_item_clicked(mouse_button) != 0; }
    bool is_item_edited() const { return m_api->is_item_edited() != 0; }
    bool is_item_deactivated_after_edit() const { return m_api->is_item_deactivated_after_edit() != 0; }
    bool is_window_focused(uint32_t flags = 0) const { return m_api->is_window_focused(flags) != 0; }
    bool is_window_hovered(uint32_t flags = 0) const { return m_api->is_window_hovered(flags) != 0; }
    void set_keyboard_focus_here(int offset = 0) const { m_api->set_keyboard_focus_here(offset); }
    void set_item_default_focus() const { m_api->set_item_default_focus(); }

    // --- ids and style: scopes ---

    // The ids of the widgets of the scope include this one. @group imgui @thread main
    [[nodiscard]] UiScope id(std::string_view value) const
    {
        m_api->push_id_str(Str(value).c);
        return UiScope(true, m_api->pop_id);
    }
    [[nodiscard]] UiScope id(int value) const
    {
        m_api->push_id_int(value);
        return UiScope(true, m_api->pop_id);
    }
    // A color of the style (GWP_IMGUI_COL_*, 0xAARRGGBB) for the scope. @group imgui @thread main
    [[nodiscard]] UiScope style_color(uint32_t idx, uint32_t argb) const
    {
        m_api->push_style_color(idx, argb);
        return UiScope(true, m_api->pop_style_color);
    }
    // A variable of the style (GWP_IMGUI_STYLE_*) for the scope: (float) or (vec2). @group imgui @thread main
    [[nodiscard]] UiScope style_var(uint32_t idx, float value) const
    {
        m_api->push_style_var_float(idx, value);
        return UiScope(true, m_api->pop_style_var);
    }
    [[nodiscard]] UiScope style_var(uint32_t idx, float x, float y) const
    {
        m_api->push_style_var_vec2(idx, x, y);
        return UiScope(true, m_api->pop_style_var);
    }
    // The widgets of the scope are grayed out and inactive (when disabled). @group imgui @thread main
    [[nodiscard]] UiScope disabled(bool disabled = true) const
    {
        m_api->begin_disabled(disabled ? 1 : 0);
        return UiScope(true, m_api->end_disabled);
    }

private:
    // A NUL-terminated copy of a string_view for the C table: on the stack up to 255 bytes, on the heap above
    struct Str
    {
        char small[256];
        std::string big;
        const char* c = small;
        explicit Str(std::string_view value)
        {
            if (value.size() < sizeof(small))
            {
                if (!value.empty())
                    std::memcpy(small, value.data(), value.size());
                small[value.size()] = '\0';
            }
            else
            {
                big.assign(value);
                c = big.c_str();
            }
        }
        Str(const Str&) = delete;
        Str& operator=(const Str&) = delete;
    };

    // The buffer of input_text for a std::string: max_bytes + 1 bytes with the text (one per thread, reused).
    // max_bytes is clamped to the limit of the engine (limit - 1: a bigger buffer would only cost memory, the engine
    // uses `limit` bytes of it). A value longer than max_bytes is cut in the buffer only, on a UTF-8 boundary; copied -
    // the bytes put into the buffer (take_text)
    static std::vector<char>& text_buffer(const std::string& value, uint32_t max_bytes, uint32_t limit, size_t& copied)
    {
        thread_local std::vector<char> buffer;
        if (max_bytes > limit - 1)
            max_bytes = limit - 1;
        buffer.resize(static_cast<size_t>(max_bytes) + 1);
        size_t length = value.size() < max_bytes ? value.size() : max_bytes;
        if (length < value.size())
        {
            while (length > 0 && (static_cast<unsigned char>(value[length]) & 0xC0u) == 0x80u)
                --length;
        }
        if (length)
            std::memcpy(buffer.data(), value.data(), length);
        buffer[length] = '\0';
        copied = length;
        return buffer;
    }
    // The text back into the string when the widget changed it (also without "changed": ENTER_RETURNS_TRUE). A buffer
    // that still holds what text_buffer copied leaves the value as it is: merely showing a value longer than max_bytes
    // does not cut it (review 2026-11-18)
    static void take_text(const std::vector<char>& buffer, size_t copied, std::string& value)
    {
        const char* text = buffer.data();
        const size_t length = std::strlen(text);
        if (length == copied && std::memcmp(text, value.data(), length) == 0)
            return;
        value.assign(text, length);
    }

    const GwpImGuiApi* m_api = nullptr;
};

/*
 * ImGuiTool: a window of the plugin (Plugin::imgui_tool_register). The engine calls the methods on the main thread,
 * inside the ImGui frame of the editor. draw: every frame while the window is open and the editor (F10) is not
 * hidden; inside the window (the engine makes Begin/End of it); `ui` is valid until draw returns. An exception from a
 * method is caught by gwp::Plugin: logged, the window closes (the scopes of the stack end on the way out).
 * The object must live until imgui_tool_unregister or the unload of the plugin (a member of the plugin class).
 */
class ImGuiTool
{
public:
    // @group imgui @thread main
    ImGuiTool() = default;
    virtual ~ImGuiTool() = default;

    // The content of the window. frame.flags: GWP_IMGUI_FRAME_LIGHT (the light state: no input),
    // GWP_IMGUI_FRAME_FIRST (the first draw after the window opened), GWP_IMGUI_FRAME_IN_GAME. @group imgui @thread main
    virtual void draw(Ui& ui, const GwpImGuiFrame& frame) = 0;
    // The window opened (the menu, imgui_tool_set_open, the open flag of imgui.ini): before its first draw.
    // @group imgui @thread main
    virtual void on_open() {}
    // The window closed (the cross, the menu, imgui_tool_set_open, imgui_tool_unregister). Not called when the plugin
    // stops or is unloaded. @group imgui @thread main
    virtual void on_close() {}
};

// Plugin::imgui_tool_register: the fields of GwpImGuiToolDesc (the same rules; empty strings - NULL).
struct ImGuiToolOptions
{
    std::string id;            // a-z 0-9 _ . -, 2..63 bytes, "<addon id>_..."; unique among all the tools
    std::string title;         // the id of a string or the text; empty - the id
    std::string menu;          // the submenu of "Addons" ("My addon/Debug"); empty - the addon id
    std::string tooltip;       // the hint of the menu item; empty - none
    uint32_t flags = 0;        // GWP_IMGUI_TOOL_*
    uint32_t window_flags = 0; // GWP_IMGUI_WINDOW_*
    int32_t order = 0;         // the order inside the submenu (lower - higher)
    UiVec2 size;               // the size at the first open, 0 - auto
    UiVec2 pos;                // the position at the first open, (0, 0) - ImGui decides
    UiVec2 min_size;           // the smallest size, 0 - none
};

/*
 * Plugin: base class of a C++ plugin. Takes over the protocol of the C ABI, so a plugin is a class with the
 * handlers it needs:
 *
 *     class MyPlugin : public gwp::Plugin
 *     {
 *         uint32_t deaths = 0;
 *
 *         GwpResult on_init() override
 *         {
 *             subscribe("npc_on_death_callback", [this](const gwp::EventView& e) { ++deaths; });
 *             return GWP_OK;
 *         }
 *         void on_game_start(std::string_view) override { deaths = 0; }
 *         void on_save(gwp::BinaryWriter& w) override { w.write(deaths); }
 *         void on_load(gwp::BinaryReader& r, uint32_t) override { deaths = r.read_or<uint32_t>(0); }
 *     };
 *     GWP_DEFINE_PLUGIN(MyPlugin)
 *
 * What the class does for you:
 *  - the entry point gwp_plugin_init (GWP_DEFINE_PLUGIN): version checks, GwpPluginDesc, on_unload, exceptions;
 *  - subscriptions with lambdas or member functions (subscribe / subscribe_batch), removed on unload;
 *  - the save block of the addon: on_game_start / on_save / on_load with a version, from the engine events
 *    alife_on_start, alife_on_before_save and alife_on_load;
 *  - timers: timer_start without an event name, expired timers come to on_timer(name);
 *  - binders: bind(class_id, section_mask, binder) attaches a gwp::Binder to the matching objects.
 *  - ImGui tools: imgui_tool_register(tool, options) puts a gwp::ImGuiTool into the menu "Addons" of the editor.
 * The raw API stays available through api() and self(). Every handler runs on the main thread.
 * The object is created in gwp_plugin_init and destroyed right after on_unload, while the engine API is still valid.
 */
// Writes why gwp_plugin_init refuses the engine table `api` (GWP_API_FITS(api, api_min) is false) to the engine log,
// through api->log: the header of the table and `log` keep their place in every version (see GwpEngineApi), so the
// call is safe also when the rest of the table has another layout. Nothing is written when the table is too short
// for `log`. @group plugin @thread main
inline void log_version_refusal(const GwpEngineApi* api, const GwpPlugin* self, uint32_t api_min) noexcept
{
    if (api == nullptr || api->size < offsetof(GwpEngineApi, log) + sizeof(api->log) || api->log == nullptr)
        return;
    const auto part = [](uint32_t version, int shift, uint32_t mask) { return (version >> shift) & mask; };
    const uint32_t engine = api->api_version;
    char text[256];
    std::snprintf(text, sizeof(text),
        "Plugin API mismatch, the plugin does not load: built with Plugin API %u.%u.%u, needs %u.%u.%u or newer of the "
        "same major (before 1.0: the same 0.MINOR), the engine has %u.%u.%u. Rebuild the plugin with the headers of "
        "this engine",
        static_cast<unsigned>(GWP_API_VERSION_MAJOR), static_cast<unsigned>(GWP_API_VERSION_MINOR),
        static_cast<unsigned>(GWP_API_VERSION_PATCH), static_cast<unsigned>(part(api_min, 22, 0x3FFu)),
        static_cast<unsigned>(part(api_min, 12, 0x3FFu)), static_cast<unsigned>(part(api_min, 0, 0xFFFu)),
        static_cast<unsigned>(part(engine, 22, 0x3FFu)), static_cast<unsigned>(part(engine, 12, 0x3FFu)),
        static_cast<unsigned>(part(engine, 0, 0xFFFu)));
    api->log(self, GWP_LOG_ERROR, text);
}

class Plugin
{
public:
    // @group plugin @thread main
    Plugin() = default;
    virtual ~Plugin() = default;

    // Called by GWP_DEFINE_PLUGIN from gwp_plugin_init: checks the versions, fills `out`, then runs on_init.
    // api_min: the minimal engine API this plugin needs (GWP_MAKE_VERSION); by default the version of the headers it
    // is built with. A lower one lets the plugin load on an older engine of the same major (from 1.0 on the same
    // MAJOR, before 1.0 the same 0.MINOR - only an older PATCH, see "Compatibility" in gwp_api.h): then every function
    // newer than api_min must be checked with GWP_API_HAS before the call. @group plugin @thread main
    GwpResult init(const GwpEngineApi* api, const GwpPlugin* self, GwpPluginDesc* out,
        uint32_t api_min = GWP_API_VERSION) noexcept
    {
        if (api == nullptr || out == nullptr)
            return GWP_ERROR_VERSION_MISMATCH;
        // The major (MAJOR, the pair 0.MINOR before 1.0) and api_min: before any other call into the table
        if (!GWP_API_FITS(api, api_min))
        {
            log_version_refusal(api, self, api_min);
            return GWP_ERROR_VERSION_MISMATCH;
        }
        m_api = api;
        m_self = self;
        m_log.reset(api, self);
        m_addon_id = api->addon_id(self);

        out->abi_major = GWP_API_VERSION_MAJOR;
        out->size = sizeof(GwpPluginDesc);
        out->api_min = api_min;
        out->api_built = GWP_API_VERSION;
        out->reserved = 0;
        out->on_unload = &unload_trampoline;
        out->user = this;

        try
        {
            // The save hooks: a plugin that does not override them just gets empty calls.
            subscribe("alife_on_start", [this](const EventView& e) { on_game_start(e.string(0)); });
            subscribe("alife_on_before_save", [this](const EventView&) { save_block(); });
            subscribe("alife_on_load", [this](const EventView&) { load_block(); });
            return on_init();
        }
        catch (const std::exception& e)
        {
            m_log.error("init failed: {}", e.what());
            return GWP_ERROR;
        }
        catch (...)
        {
            m_log.error("init failed: unknown exception");
            return GWP_ERROR;
        }
    }

    // Engine function table and this plugin's handle: for calls the class does not wrap. @group plugin @thread any
    const GwpEngineApi* api() const noexcept { return m_api; }
    const GwpPlugin* self() const noexcept { return m_self; }
    const std::string& addon_id() const noexcept { return m_addon_id; }

    // Logger bound to this plugin: log().info("x = {}", x). @group plugin @thread main
    Logger& log() noexcept { return m_log; }

    // Name of an online object, empty when there is no such object. Valid while the object is online.
    // @group plugin @thread main
    std::string_view object_name(GwpObjectId id) const noexcept
    {
        const char* name = m_api->object_name(id);
        return name ? std::string_view(name) : std::string_view();
    }

    // --- files in the profile of the user (group user_data, wiki/doc/plugins/api/user_data.md) ------------------
    // The folder <root>/addons/<addon id>/ outside the game and the saves, one for every save and every copy of the
    // game. The names and the limits are those of user_data_* in gwp_api.h.

    // The engine has the group (an older one: the calls below answer GWP_ERROR_NOT_SUPPORTED, empty results).
    // @group user_data @thread any
    bool has_user_data() const noexcept { return m_api != nullptr && GWP_API_HAS(m_api, user_data_list); }

    // The folder of the addon (UTF-8, a trailing separator), created when missing; empty - none (the engine logs why).
    // @group user_data @thread any
    std::string user_data_dir() const
    {
        if (!has_user_data())
            return {};
        const uint32_t length = m_api->user_data_dir(m_self, nullptr, 0);
        if (length == 0)
            return {};
        std::string text(length, '\0');
        // length + 1: the room of the terminator (data()[size()] of std::string)
        if (m_api->user_data_dir(m_self, text.data(), length + 1) != length)
            return {};
        return text;
    }

    // The whole file into `out`. GWP_OK; GWP_ERROR_NOT_FOUND - no such file (out is empty); the other codes of
    // user_data_read. The file may change between the size and the read (another copy of the game): asked again.
    // @group user_data @thread any
    GwpResult user_data_read(const char* name, std::string& out) const
    {
        out.clear();
        if (!has_user_data())
            return GWP_ERROR_NOT_SUPPORTED;
        uint32_t size = 0;
        // cap 0 asks the size (an empty file answers GWP_OK at once)
        GwpResult result = m_api->user_data_read(m_self, name, nullptr, 0, &size);
        for (int attempt = 0; attempt < 4 && result == GWP_ERROR_INVALID_ARGUMENT && size > 0; ++attempt)
        {
            out.resize(size);
            result = m_api->user_data_read(m_self, name, out.data(), size, &size);
        }
        if (result == GWP_OK)
            out.resize(size); // the file may have become shorter
        else
            out.clear();
        return result;
    }

    // Replaces the file atomically (user_data_write). @group user_data @thread any
    GwpResult user_data_write(const char* name, std::string_view data) const
    {
        if (!has_user_data() || data.size() > GWP_USER_DATA_MAX_FILE_SIZE)
            return GWP_ERROR_NOT_SUPPORTED;
        return m_api->user_data_write(m_self, name, data.data(), static_cast<uint32_t>(data.size()));
    }

    // GWP_OK, GWP_ERROR_NOT_FOUND - no such file. @group user_data @thread any
    GwpResult user_data_remove(const char* name) const
    {
        return has_user_data() ? m_api->user_data_remove(m_self, name) : GWP_ERROR_NOT_SUPPORTED;
    }

    // The names of the files of the folder, sorted; empty - none (or a failure, logged by the engine).
    // @group user_data @thread any
    std::vector<std::string> user_data_list() const
    {
        std::vector<std::string> names;
        if (!has_user_data())
            return names;
        std::string text;
        uint32_t length = m_api->user_data_list(m_self, nullptr, 0);
        bool fitted = length == 0;
        // A file may appear between the calls (another copy of the game): asked again with the new length
        for (int attempt = 0; attempt < 4 && !fitted; ++attempt)
        {
            text.assign(length, '\0');
            const uint32_t now = m_api->user_data_list(m_self, text.data(), length + 1);
            fitted = now <= length;
            text.resize(fitted ? now : 0);
            length = now;
        }
        for (size_t begin = 0; begin < text.size();)
        {
            size_t end = text.find('\n', begin);
            if (end == std::string::npos)
                end = text.size();
            names.emplace_back(text, begin, end - begin);
            begin = end + 1;
        }
        return names;
    }

    // Read - change - write under the lock of the name (user_data_lock), so another copy of the game that does the
    // same in between loses nothing: fn(const std::string* old) -> std::optional<std::string>, old == nullptr - no
    // file yet; std::nullopt - leave the file as it is. GWP_OK; GWP_ERROR_INVALID_STATE - the wait for the lock ran
    // out (another holder: try later); else the first failing code (the read, the write). fn runs on the calling
    // thread under the lock - keep it short; an exception from it releases the lock and goes on to the caller.
    // @group user_data @thread any
    template <typename F>
    GwpResult user_data_update(const char* name, F&& fn,
        uint32_t timeout_ms = GWP_USER_DATA_LOCK_TIMEOUT_DEFAULT) const
    {
        if (!has_user_data())
            return GWP_ERROR_NOT_SUPPORTED;
        GwpUserDataLock lock = GWP_INVALID_USER_DATA_LOCK;
        const GwpResult locked = m_api->user_data_lock(m_self, name, timeout_ms, &lock);
        if (locked != GWP_OK)
            return locked;
        // The lock goes on every way out, an exception of fn included
        struct Unlock
        {
            const GwpEngineApi* api;
            const GwpPlugin* self;
            GwpUserDataLock lock;
            ~Unlock() { api->user_data_unlock(self, lock); }
        } unlock{ m_api, m_self, lock };
        std::string old;
        const GwpResult read = user_data_read(name, old);
        if (read != GWP_OK && read != GWP_ERROR_NOT_FOUND)
            return read;
        std::optional<std::string> next = fn(read == GWP_OK ? &old : nullptr);
        if (!next)
            return GWP_OK;
        return user_data_write(name, *next);
    }

    // --- ImGui tools (group imgui, wiki/doc/plugins/api/imgui.md) ------------------------------------------------
    // A window of the plugin in the menu "Addons" of the editor (F10): a gwp::ImGuiTool. The engine removes the tools
    // by itself when the plugin stops (unload, crash, plugin_reload): register them in on_init.

    // The engine has the group (imgui_tool_register): an older one answers GWP_ERROR_NOT_SUPPORTED below.
    // @group imgui @thread any
    bool has_imgui_tools() const noexcept { return m_api != nullptr && GWP_API_HAS(m_api, imgui_tool_register); }

    // Registers `tool` (imgui_tool_register of the engine: the codes are its codes). The engine calls its draw /
    // on_open / on_close; an exception from them is logged and closes the window. `tool` must live until
    // imgui_tool_unregister or the unload of the plugin. @group imgui @thread main
    GwpResult imgui_tool_register(ImGuiTool& tool, const ImGuiToolOptions& options)
    {
        if (!has_imgui_tools())
            return GWP_ERROR_NOT_SUPPORTED;
        // A node freed by imgui_tool_unregister is reused: the deque never shrinks (the engine keeps its address)
        ImGuiToolNode* node = nullptr;
        for (ImGuiToolNode& n : m_imgui_tools)
        {
            if (!n.tool)
            {
                node = &n;
                break;
            }
        }
        if (!node)
            node = &m_imgui_tools.emplace_back();
        node->owner = this;
        node->tool = &tool;
        node->id = options.id;
        GwpImGuiToolDesc desc{};
        desc.size = sizeof(desc);
        desc.flags = options.flags;
        desc.id = node->id.c_str();
        desc.title = options.title.empty() ? nullptr : options.title.c_str();
        desc.menu = options.menu.empty() ? nullptr : options.menu.c_str();
        desc.tooltip = options.tooltip.empty() ? nullptr : options.tooltip.c_str();
        desc.draw = &imgui_draw_trampoline;
        desc.on_open = &imgui_open_trampoline;
        desc.on_close = &imgui_close_trampoline;
        desc.user = node;
        desc.window_flags = options.window_flags;
        desc.order = options.order;
        desc.size_w = options.size.x;
        desc.size_h = options.size.y;
        desc.pos_x = options.pos.x;
        desc.pos_y = options.pos.y;
        desc.min_w = options.min_size.x;
        desc.min_h = options.min_size.y;
        const GwpResult result = m_api->imgui_tool_register(m_self, &desc);
        if (result != GWP_OK)
            node->tool = nullptr;
        return result;
    }

    // Removes a tool of this plugin: an open window gets on_close. @group imgui @thread main
    GwpResult imgui_tool_unregister(std::string_view id)
    {
        if (!has_imgui_tools())
            return GWP_ERROR_NOT_SUPPORTED;
        const std::string key(id);
        const GwpResult result = m_api->imgui_tool_unregister(m_self, key.c_str());
        if (result == GWP_OK || result == GWP_ERROR_NOT_FOUND)
        {
            for (ImGuiToolNode& n : m_imgui_tools)
            {
                if (n.tool && n.id == key)
                    n.tool = nullptr;
            }
        }
        return result;
    }

    // Opens or closes a tool (any addon's); show_editor: the editor goes to the full state (the window is seen).
    // @group imgui @thread main
    GwpResult imgui_tool_set_open(std::string_view id, bool open, bool show_editor = false)
    {
        if (!has_imgui_tools())
            return GWP_ERROR_NOT_SUPPORTED;
        const std::string key(id);
        return m_api->imgui_tool_set_open(m_self, key.c_str(), open ? 1 : 0, show_editor ? GWP_IMGUI_SHOW_EDITOR : 0u);
    }

    // The window is open or asked to open; false for an engine without the group. @group imgui @thread main
    bool imgui_tool_is_open(std::string_view id) const
    {
        if (!has_imgui_tools())
            return false;
        const std::string key(id);
        return m_api->imgui_tool_is_open(m_self, key.c_str()) != 0;
    }

protected:
    // --- handlers to override -------------------------------------------------------------------------------

    // Called from gwp_plugin_init: subscribe to events here. Return GWP_OK to stay loaded.
    virtual GwpResult on_init() { return GWP_OK; }

    // Called before the library is unloaded: release what on_init acquired. Subscriptions are removed by the engine
    // right after this call.
    virtual void on_unload() {}

    // alife_on_start: a game session starts ("new_game", "load", "level_change"); reset the state of the previous
    // game here. On a load on_load comes right after; on a new game nothing comes, so the reset stays.
    virtual void on_game_start(std::string_view reason) { (void)reason; }

    // alife_on_before_save: write the data of the plugin; it goes into the save with version save_version().
    // Nothing written = no block in the save.
    virtual void on_save(BinaryWriter& out) { (void)out; }

    // alife_on_load: the block written by on_save. Not called when the save has no block of this addon.
    // version: save_version() of the plugin that wrote it. A block newer than save_version() comes here only when
    // its writer declared this version compatible (save_min_compatible_version), otherwise it is skipped with a
    // warning. Read in the order of on_save; check r.ok() at the end when the layout may be truncated.
    virtual void on_load(BinaryReader& in, uint32_t version) { (void)in; (void)version; }

    // Version of the layout written by on_save (1..32767). Bump it when the layout changes and read the old one by
    // `version`.
    virtual uint32_t save_version() const { return 1; }

    // The oldest save_version() that can read the blocks this version writes: the layout only added tagged chunks
    // (BinaryWriter::chunk) or appended fields that an older reader skips. By default save_version(): an older
    // version of the plugin skips the block. One block per addon: all the data of the plugin goes into it.
    virtual uint32_t save_min_compatible_version() const { return save_version(); }

    // A timer started with timer_start expired. name: the timer name given to timer_start.
    virtual void on_timer(std::string_view name) { (void)name; }

    // The same with the event of the timer: e.string(0) the name, e.string(1) the key "<addon id>/<name>", from
    // index 2 the arguments given to timer_start. By default calls on_timer(name).
    virtual void on_timer_event(std::string_view name, const EventView& event)
    {
        (void)event;
        on_timer(name);
    }

    // --- events ----------------------------------------------------------------------------------------------

    // Subscribes fn(const EventView&) to the event. Returns false when the engine refused (invalid name).
    // The handler is a lambda, a functor or a bound member; it is stored by the class until on_unload.
    template <typename F>
    bool subscribe(const char* name, F&& fn)
    {
        Subscription& sub = acquire_subscription(name);
        sub.fn = std::forward<F>(fn);
        sub.id = m_api->event_subscribe(m_self, name, &event_trampoline, &sub);
        if (sub.id == GWP_INVALID_SUBSCRIPTION_ID)
        {
            release_subscription(sub);
            return false;
        }
        return true;
    }

    // --- subscriptions with options and their end (plans/addon_api_ideas, P27) --------------------------------
    // The subscribe_* below return the id of the subscription (GWP_INVALID_SUBSCRIPTION_ID - refused, the reason is
    // in the log) for unsubscribe(id). A plain subscribe() lives until on_unload, as before.

    // Options of subscribe_ex: every field optional.
    struct SubscribeOptions
    {
        uint32_t throttle_ms = 0;                 // at most one call per interval (game time, ms); 0 = every emit
        GwpObjectId object = GWP_INVALID_OBJECT_ID; // only the emits whose first argument is this online object
                                                  // (GWP_SUBSCRIBE_OBJECT); the subscription ends when it goes offline
        int32_t priority = 0; // a higher one is called earlier among the plugins (GwpSubscribeOptions::priority; an
                              // engine before that step ignores it - check GWP_API_HAS(api(), task_async))
    };

    // fn(const EventView&) with options; the id for unsubscribe. A subscription to an object ends by itself when the
    // object goes offline (the engine removes it); its node here is freed at the next subscribe_* whose object is no
    // longer online, or by unsubscribe.
    template <typename F>
    GwpSubscriptionId subscribe_ex(const char* name, const SubscribeOptions& options, F&& fn)
    {
        const bool plain = options.throttle_ms == 0 && options.object == GWP_INVALID_OBJECT_ID && options.priority == 0;
        if (!plain && !GWP_API_HAS(m_api, event_subscribe_ex))
            return GWP_INVALID_SUBSCRIPTION_ID;
        if (options.object != GWP_INVALID_OBJECT_ID)
            sweep_object_subscriptions();
        Subscription& sub = acquire_subscription(name);
        sub.fn = std::forward<F>(fn);
        sub.object = options.object;
        if (plain)
            sub.id = m_api->event_subscribe(m_self, name, &event_trampoline, &sub);
        else
        {
            GwpSubscribeOptions raw{};
            raw.size = sizeof(raw);
            raw.throttle_ms = options.throttle_ms;
            if (options.object != GWP_INVALID_OBJECT_ID)
            {
                raw.flags |= GWP_SUBSCRIBE_OBJECT;
                raw.object = options.object;
            }
            raw.priority = options.priority;
            sub.id = m_api->event_subscribe_ex(m_self, name, &raw, &event_trampoline, &sub);
        }
        if (sub.id == GWP_INVALID_SUBSCRIPTION_ID)
        {
            release_subscription(sub);
            return GWP_INVALID_SUBSCRIPTION_ID;
        }
        return sub.id;
    }

    // At most one call of fn per throttle_ms (game time): a frequent event (actor_on_update, npc_on_hit_callback)
    // without a batch.
    template <typename F>
    GwpSubscriptionId subscribe_throttled(const char* name, uint32_t throttle_ms, F&& fn)
    {
        SubscribeOptions options;
        options.throttle_ms = throttle_ms;
        return subscribe_ex(name, options, std::forward<F>(fn));
    }

    // Only the emits about one online object (its id is the first argument of the event): an NPC of interest. Ends
    // by itself when the object goes offline.
    template <typename F>
    GwpSubscriptionId subscribe_object(const char* name, GwpObjectId object, F&& fn, uint32_t throttle_ms = 0)
    {
        SubscribeOptions options;
        options.object = object;
        options.throttle_ms = throttle_ms;
        return subscribe_ex(name, options, std::forward<F>(fn));
    }

    // subscribe_batch with the id for unsubscribe (and a priority: the order of the deliveries of one frame).
    template <typename F>
    GwpSubscriptionId subscribe_batch_ex(const char* name, uint32_t throttle_ms, uint32_t max_batch, F&& fn,
        int32_t priority = 0)
    {
        if (!GWP_API_HAS(m_api, event_subscribe_ex))
            return GWP_INVALID_SUBSCRIPTION_ID;
        Subscription& sub = acquire_subscription(name);
        sub.batch = std::forward<F>(fn);
        GwpSubscribeOptions options{};
        options.size = sizeof(options);
        options.flags = GWP_SUBSCRIBE_BATCH;
        options.throttle_ms = throttle_ms;
        options.max_batch = max_batch;
        options.batch_handler = &batch_trampoline;
        options.priority = priority;
        sub.id = m_api->event_subscribe_ex(m_self, name, &options, nullptr, &sub);
        if (sub.id == GWP_INVALID_SUBSCRIPTION_ID)
        {
            release_subscription(sub);
            return GWP_INVALID_SUBSCRIPTION_ID;
        }
        return sub.id;
    }

    // Ends a subscription of this plugin made by subscribe_ex / subscribe_throttled / subscribe_object /
    // subscribe_batch_ex. Safe inside its own handler (the handler object is destroyed after it returns). false - no
    // such live subscription of this class (ended already, or an id of a plain subscribe).
    bool unsubscribe(GwpSubscriptionId id)
    {
        if (id == GWP_INVALID_SUBSCRIPTION_ID)
            return false;
        for (Subscription& sub : m_subscriptions)
        {
            if (sub.id != id || sub.dead)
                continue;
            m_api->event_unsubscribe(m_self, id); // an id the engine ended already is ignored there (ids never repeat)
            release_subscription(sub);
            return true;
        }
        return false;
    }

    // Batch subscription (GWP_SUBSCRIBE_BATCH): fn(std::span<const GwpEvent>) gets every event collected since the
    // previous delivery, at most once per throttle_ms (0 = every frame). max_batch: records kept between deliveries
    // (0 = 4096). Returns false when the engine has no event_subscribe_ex or refused.
    template <typename F>
    bool subscribe_batch(const char* name, uint32_t throttle_ms, uint32_t max_batch, F&& fn)
    {
        if (!GWP_API_HAS(m_api, event_subscribe_ex))
            return false;
        Subscription& sub = acquire_subscription(name);
        sub.batch = std::forward<F>(fn);
        GwpSubscribeOptions options{};
        options.size = sizeof(options);
        options.flags = GWP_SUBSCRIBE_BATCH;
        options.throttle_ms = throttle_ms;
        options.max_batch = max_batch;
        options.batch_handler = &batch_trampoline;
        sub.id = m_api->event_subscribe_ex(m_self, name, &options, nullptr, &sub);
        if (sub.id == GWP_INVALID_SUBSCRIPTION_ID)
        {
            release_subscription(sub);
            return false;
        }
        return true;
    }

    // --- timers (group timers of the engine API) --------------------------------------------------------------

    // Starts (or restarts) the timer `name` of this addon; on_timer(name) is called when it expires.
    // delay_seconds: first firing; period_seconds > 0 repeats it; flags: GWP_TIMER_REAL_TIME (level time instead of
    // game time), GWP_TIMER_PERSISTENT (kept in the save). args: up to 16 values copied by the engine and given back
    // to on_timer_event (from index 2), kept in the save with a persistent timer; GWP_T_LUA_REF is refused.
    // False without a game, while a save is loading, for a bad argument, or on an engine without timers.
    bool timer_start(const char* name, double delay_seconds, double period_seconds = 0.0, uint32_t flags = 0,
        std::span<const GwpValue> args = {})
    {
        if (!has_timers() || !subscribe_timer_event())
            return false;
        return m_api->timer_start(m_self, name, m_timer_event.c_str(), delay_seconds, period_seconds, flags,
                   static_cast<uint32_t>(args.size()), args.data()) == GWP_OK;
    }

    // timer_start only when the timer does not exist: a persistent timer restored from the save is kept as it is
    // (with its arguments). Returns true when the timer exists afterwards.
    bool timer_start_if_missing(const char* name, double delay_seconds, double period_seconds = 0.0,
        uint32_t flags = 0, std::span<const GwpValue> args = {})
    {
        double left = 0.0;
        if (has_timers() && m_api->timer_remaining(m_self, name, &left) == GWP_OK)
            return subscribe_timer_event();
        return timer_start(name, delay_seconds, period_seconds, flags, args);
    }

    // Stops the timer; false when there is no such timer.
    bool timer_stop(const char* name)
    {
        return has_timers() && m_api->timer_stop(m_self, name) == GWP_OK;
    }

    // Seconds left, or a negative value when there is no such timer.
    double timer_remaining(const char* name) const
    {
        double left = 0.0;
        if (!has_timers() || m_api->timer_remaining(m_self, name, &left) != GWP_OK)
            return -1.0;
        return left;
    }

    // Timers group present in the engine API.
    bool has_timers() const noexcept { return GWP_API_HAS(m_api, timer_remaining); }

    // --- binders (group binders of the engine API) ------------------------------------------------------------

    // Attaches `binder` to every object whose class id is class_id ("AI_STL_S", nullptr = any class) and whose
    // section matches section_mask ("wpn_*", nullptr = any section); one of the two is required. Objects already
    // online get on_spawn right away. `binder` must live until unbind or the unload of the plugin.
    // GWP_INVALID_BINDER_ID: the engine has no binders or refused (the reason is in the log).
    GwpBinderId bind(const char* class_id, const char* section_mask, Binder& binder)
    {
        if (!has_binders())
            return GWP_INVALID_BINDER_ID;
        Binding* binding = nullptr;
        // A node freed by unbind is reused: the deque never shrinks. Busy is `binder`, not `id`: the id comes only
        // when binder_register returns, and an on_spawn it calls may bind() again - that one must take another node
        for (Binding& b : m_bindings)
        {
            if (!b.binder)
            {
                binding = &b;
                break;
            }
        }
        if (!binding)
            binding = &m_bindings.emplace_back();
        binding->owner = this;
        binding->binder = &binder; // busy from here on, before the engine calls on_spawn through it
        binding->id = GWP_INVALID_BINDER_ID;
        binding->pending_id = GWP_INVALID_OBJECT_ID;
        binding->pending.clear();
        GwpBinderVTable vtable{};
        vtable.size = sizeof(vtable);
        vtable.on_reinit = &binder_reinit_trampoline;
        vtable.on_spawn = &binder_spawn_trampoline;
        vtable.on_destroy = &binder_destroy_trampoline;
        vtable.on_update = &binder_update_trampoline;
        if (binder.saves_state())
        {
            vtable.on_save = &binder_save_trampoline;
            vtable.on_load = &binder_load_trampoline;
        }
        binding->id = m_api->binder_register(m_self, class_id, section_mask, &vtable, binding);
        if (binding->id == GWP_INVALID_BINDER_ID)
            binding->binder = nullptr;
        return binding->id;
    }

    // Detaches the binder: on_destroy for every bound object, then the binder is forgotten. false: no such binder
    // of this plugin, or the engine refused (called outside the main thread, or from on_save while the game is being
    // saved): then the binder stays as it was.
    bool unbind(GwpBinderId id)
    {
        if (id == GWP_INVALID_BINDER_ID)
            return false;
        for (Binding& b : m_bindings)
        {
            if (b.id != id)
                continue;
            // A refused call leaves the binder registered: the engine still calls through this node
            if (m_api->binder_unregister(m_self, id) != GWP_OK)
                return false;
            // An on_destroy may have unbound it already, and a bind() then may have taken the node: the ids never
            // repeat, so the node is freed only while it still holds this binder
            if (b.id == id)
            {
                b.id = GWP_INVALID_BINDER_ID;
                b.binder = nullptr;
            }
            return true;
        }
        return false;
    }

    // Online objects the binder is attached to now; 0 for an unknown binder.
    uint32_t bound_count(GwpBinderId id) const
    {
        return has_binders() ? m_api->binder_object_count(id) : 0u;
    }

    // Binders group present in the engine API.
    bool has_binders() const noexcept { return GWP_API_HAS(m_api, binder_object_count); }

private:
    struct Subscription
    {
        Plugin* owner = nullptr;
        GwpSubscriptionId id = GWP_INVALID_SUBSCRIPTION_ID;
        std::string name;
        EventFn fn;
        BatchFn batch;
        GwpObjectId object = GWP_INVALID_OBJECT_ID; // subscribe_object: the engine ends it with the object
        uint32_t calls = 0; // handlers of this node running now (an unsubscribe inside keeps fn alive until they end)
        bool dead = false;  // unsubscribed / refused: free for acquire_subscription once calls is 0
    };

    // A node for a new subscription: a freed one (no handler of it runs), or a new one. The deque never shrinks: the
    // engine keeps pointers to its elements.
    Subscription& acquire_subscription(const char* name)
    {
        Subscription* node = nullptr;
        for (Subscription& sub : m_subscriptions)
        {
            if (sub.dead && sub.calls == 0)
            {
                node = &sub;
                break;
            }
        }
        if (!node)
            node = &m_subscriptions.emplace_back();
        node->owner = this;
        node->id = GWP_INVALID_SUBSCRIPTION_ID;
        node->name = name ? name : "";
        node->fn = nullptr;
        node->batch = nullptr;
        node->object = GWP_INVALID_OBJECT_ID;
        node->calls = 0;
        node->dead = false;
        return *node;
    }

    // The node is free: its handler is destroyed now, or by the trampoline when the running call returns
    void release_subscription(Subscription& sub)
    {
        sub.dead = true;
        sub.id = GWP_INVALID_SUBSCRIPTION_ID;
        sub.object = GWP_INVALID_OBJECT_ID;
        if (sub.calls == 0)
        {
            sub.fn = nullptr;
            sub.batch = nullptr;
        }
    }

    // subscribe_object: the nodes whose object is no longer online - the engine ended those subscriptions
    void sweep_object_subscriptions()
    {
        for (Subscription& sub : m_subscriptions)
        {
            if (!sub.dead && sub.object != GWP_INVALID_OBJECT_ID && !m_api->object_exists(sub.object))
            {
                m_api->event_unsubscribe(m_self, sub.id); // ignored by the engine: the id has ended
                release_subscription(sub);
            }
        }
    }

    // Counts a running handler of the node; the last one out destroys the handler of an unsubscribed node
    struct CallScope
    {
        Subscription& sub;
        explicit CallScope(Subscription& s) : sub(s) { ++sub.calls; }
        ~CallScope()
        {
            if (--sub.calls == 0 && sub.dead)
            {
                sub.fn = nullptr;
                sub.batch = nullptr;
            }
        }
        CallScope(const CallScope&) = delete;
        CallScope& operator=(const CallScope&) = delete;
    };

    struct Binding
    {
        Plugin* owner = nullptr;
        Binder* binder = nullptr;
        GwpBinderId id = GWP_INVALID_BINDER_ID;
        // on_save that did not fit into `cap`: kept for the second call of the engine with a bigger buffer
        GwpObjectId pending_id = GWP_INVALID_OBJECT_ID;
        BinaryWriter pending;
    };

    // Trampolines: the engine calls plain functions with `user`; these forward to the stored functor.
    // Exceptions stop here with a log line; the engine would otherwise remove every subscription of the plugin.
    static void GWP_CALL event_trampoline(void* user, const GwpEvent* event) noexcept
    {
        auto* sub = static_cast<Subscription*>(user);
        if (sub->dead || !sub->fn)
            return; // unsubscribed earlier in the same dispatch
        const CallScope scope(*sub);
        try
        {
            sub->fn(EventView(event));
        }
        catch (const std::exception& e)
        {
            sub->owner->m_log.error("handler of {} failed: {}", sub->name, e.what());
        }
        catch (...)
        {
            sub->owner->m_log.error("handler of {} failed: unknown exception", sub->name);
        }
    }

    // A batch as a span of this header's records: as is when the engine's record has the same size (stride), else
    // copied record by record into `copy` (the fields this header knows; a field the engine lacks stays zero)
    template <typename T>
    static std::span<const T> strided(const T* records, uint32_t count, uint32_t stride, std::vector<T>& copy)
    {
        if (!records || count == 0)
            return std::span<const T>();
        if (stride == sizeof(T))
            return std::span<const T>(records, count);
        copy.assign(count, T{});
        const size_t part = stride < sizeof(T) ? stride : sizeof(T);
        const auto* bytes = reinterpret_cast<const uint8_t*>(records);
        for (uint32_t i = 0; i < count; ++i)
            std::memcpy(&copy[i], bytes + static_cast<size_t>(i) * stride, part);
        return std::span<const T>(copy.data(), copy.size());
    }

    static void GWP_CALL batch_trampoline(void* user, uint32_t count, const GwpEvent* events, uint32_t stride) noexcept
    {
        auto* sub = static_cast<Subscription*>(user);
        if (sub->dead || !sub->batch)
            return;
        const CallScope scope(*sub);
        try
        {
            std::vector<GwpEvent> copy;
            sub->batch(strided(events, count, stride, copy));
        }
        catch (const std::exception& e)
        {
            sub->owner->m_log.error("batch handler of {} failed: {}", sub->name, e.what());
        }
        catch (...)
        {
            sub->owner->m_log.error("batch handler of {} failed: unknown exception", sub->name);
        }
    }

    // Binder trampolines: the same exception policy as the event ones (a crash in a binder callback would make
    // the engine remove every binder of the plugin).
    template <typename F>
    static void binder_call(void* user, const char* what, F&& fn) noexcept
    {
        auto* binding = static_cast<Binding*>(user);
        if (!binding->binder)
            return;
        try
        {
            fn(*binding->binder);
        }
        catch (const std::exception& e)
        {
            binding->owner->m_log.error("binder {} failed: {}", what, e.what());
        }
        catch (...)
        {
            binding->owner->m_log.error("binder {} failed: unknown exception", what);
        }
    }
    static void GWP_CALL binder_reinit_trampoline(void* user, GwpObjectId id) noexcept
    {
        binder_call(user, "on_reinit", [id](Binder& b) { b.on_reinit(id); });
    }
    static void GWP_CALL binder_spawn_trampoline(void* user, GwpObjectId id, const char* section) noexcept
    {
        binder_call(user, "on_spawn",
            [id, section](Binder& b) { b.on_spawn(id, section ? std::string_view(section) : std::string_view()); });
    }
    static void GWP_CALL binder_destroy_trampoline(void* user, GwpObjectId id) noexcept
    {
        binder_call(user, "on_destroy", [id](Binder& b) { b.on_destroy(id); });
    }
    static void GWP_CALL binder_update_trampoline(void* user, uint32_t count, const GwpBinderUpdate* updates,
        uint32_t stride) noexcept
    {
        binder_call(user, "on_update", [count, updates, stride](Binder& b) {
            std::vector<GwpBinderUpdate> copy;
            b.on_update(strided(updates, count, stride, copy));
        });
    }
    static uint32_t GWP_CALL binder_save_trampoline(void* user, GwpObjectId id, void* out, uint32_t cap) noexcept
    {
        auto* binding = static_cast<Binding*>(user);
        uint32_t size = 0;
        binder_call(user, "on_save", [binding, id, out, cap, &size](Binder& b) {
            // The second call for the same object (the first did not fit) takes what the first one wrote; any other
            // call asks the binder again
            if (binding->pending_id != id || cap < binding->pending.size())
            {
                binding->pending.clear();
                b.on_save(id, binding->pending);
            }
            size = binding->pending.size();
            if (size > GWP_BINDER_SAVE_MAX_SIZE)
            {
                binding->owner->m_log.error("binder on_save: {} bytes for object {}, at most {}: not kept", size, id,
                    GWP_BINDER_SAVE_MAX_SIZE);
                size = 0;
            }
            else if (size > cap || !out)
            {
                binding->pending_id = id;
                return;
            }
            if (size)
                std::memcpy(out, binding->pending.data(), size);
            binding->pending_id = GWP_INVALID_OBJECT_ID;
            binding->pending.clear();
        });
        return size;
    }
    static void GWP_CALL binder_load_trampoline(void* user, GwpObjectId id, const void* data, uint32_t size) noexcept
    {
        binder_call(user, "on_load", [id, data, size](Binder& b) {
            BinaryReader in(data, size);
            b.on_load(id, in);
        });
    }

    // Every binder gets on_destroy for its objects while the Binder objects (members of the plugin, as a rule)
    // are still alive; the engine would otherwise drop the binders silently after on_unload.
    void unbind_all() noexcept
    {
        // By index: on_destroy may call bind(), and the deque growing would invalidate a range-for iterator (its
        // elements stay in place, so the reference below is safe)
        for (size_t i = 0; i < m_bindings.size(); ++i)
        {
            Binding& b = m_bindings[i];
            // Every node is freed, whatever the engine answers: the plugin goes, nothing may call it any more
            if (b.id != GWP_INVALID_BINDER_ID)
                m_api->binder_unregister(m_self, b.id);
            b.id = GWP_INVALID_BINDER_ID;
            b.binder = nullptr;
        }
    }

    static void GWP_CALL unload_trampoline(void* user) noexcept
    {
        auto* plugin = static_cast<Plugin*>(user);
        plugin->unbind_all();
        try
        {
            plugin->on_unload();
        }
        catch (const std::exception& e)
        {
            plugin->m_log.error("on_unload failed: {}", e.what());
        }
        catch (...)
        {
            plugin->m_log.error("on_unload failed: unknown exception");
        }
        delete plugin; // created by GWP_DEFINE_PLUGIN; the engine removes the subscriptions after this call
    }

    // One event for every timer of the addon: "<addon id>_on_timer"; the timer name tells which one expired.
    bool subscribe_timer_event()
    {
        if (m_timer_subscribed)
            return true;
        m_timer_event = m_addon_id + "_on_timer";
        m_timer_subscribed =
            subscribe(m_timer_event.c_str(), [this](const EventView& e) { on_timer_event(e.string(0), e); });
        return m_timer_subscribed;
    }

    // The data_version of the block: save_version() as it is, or - when save_min_compatible_version() is lower -
    // kSavePacked | min_compatible << 16 | version (both 1..32767). Bit 31 tells the two apart: a block written
    // without it (every block of an older gwp.hpp) reads as its own version
    static constexpr uint32_t kSavePacked = 0x80000000u;
    static constexpr uint32_t kSaveVersionMax = 0x7FFFu;

    void save_block()
    {
        BinaryWriter out;
        on_save(out);
        const uint32_t version = save_version();
        const uint32_t min_compatible = save_min_compatible_version();
        uint32_t data_version = version;
        if (version == 0 || version > kSaveVersionMax || min_compatible > version)
            m_log.error("save_version {} / save_min_compatible_version {}: the version must be 1..32767 and not "
                        "below the compatible one; the block is written for this version only",
                version, min_compatible);
        else if (min_compatible < version)
            data_version = kSavePacked | min_compatible << 16 | version;
        m_api->save_write(m_self, data_version, out.data(), out.size()); // size 0 removes the block
    }

    void load_block()
    {
        uint32_t data_version = 0;
        const void* bytes = nullptr;
        uint32_t size = 0;
        if (m_api->save_read(m_self, &data_version, &bytes, &size) != GWP_OK)
            return; // no block of this addon in the save (made before the addon was installed)
        const bool packed = (data_version & kSavePacked) != 0;
        const uint32_t version = packed ? data_version & 0xFFFFu : data_version;
        const uint32_t min_compatible = packed ? (data_version & ~kSavePacked) >> 16 : version;
        if (min_compatible > save_version())
        {
            m_log.warn("save block version {} needs version {} of the plugin, this is {}: ignored", version,
                min_compatible, save_version());
            return;
        }
        BinaryReader in(bytes, size);
        on_load(in, version);
        if (!in.ok())
            m_log.warn("save block version {} ({} bytes) is truncated or has another layout", version, size);
    }

    // A registered ImGui tool: the engine keeps the address (GwpImGuiToolDesc::user)
    struct ImGuiToolNode
    {
        Plugin* owner = nullptr;
        ImGuiTool* tool = nullptr; // nullptr - free for imgui_tool_register
        std::string id;
    };

    // An exception from a method of the tool: logged, the window closes (on_close comes from the engine at once)
    void imgui_tool_failed(ImGuiToolNode& node, const char* method, const char* what) noexcept
    {
        m_log.error("ImGui tool '{}' {} failed: {} (the window is closed)", node.id, method, what);
        m_api->imgui_tool_set_open(m_self, node.id.c_str(), 0, 0u);
    }

    template <typename F>
    static void imgui_tool_call(void* user, const char* method, F&& fn) noexcept
    {
        auto* node = static_cast<ImGuiToolNode*>(user);
        if (!node || !node->tool)
            return;
        try
        {
            fn(*node->tool);
        }
        catch (const std::exception& e)
        {
            node->owner->imgui_tool_failed(*node, method, e.what());
        }
        catch (...)
        {
            node->owner->imgui_tool_failed(*node, method, "unknown exception");
        }
    }
    static void GWP_CALL imgui_draw_trampoline(void* user, const GwpImGuiApi* ui, const GwpImGuiFrame* frame) noexcept
    {
        if (!ui || !frame)
            return;
        imgui_tool_call(user, "draw", [ui, frame](ImGuiTool& tool) {
            Ui wrapper(ui);
            tool.draw(wrapper, *frame);
        });
    }
    static void GWP_CALL imgui_open_trampoline(void* user) noexcept
    {
        imgui_tool_call(user, "on_open", [](ImGuiTool& tool) { tool.on_open(); });
    }
    static void GWP_CALL imgui_close_trampoline(void* user) noexcept
    {
        // An exception from on_close is only logged: the window is closed already
        auto* node = static_cast<ImGuiToolNode*>(user);
        if (!node || !node->tool)
            return;
        try
        {
            node->tool->on_close();
        }
        catch (const std::exception& e)
        {
            node->owner->m_log.error("ImGui tool '{}' on_close failed: {}", node->id, e.what());
        }
        catch (...)
        {
            node->owner->m_log.error("ImGui tool '{}' on_close failed: unknown exception", node->id);
        }
    }

    const GwpEngineApi* m_api = nullptr;
    const GwpPlugin* m_self = nullptr;
    Logger m_log;
    std::string m_addon_id;
    std::string m_timer_event;
    bool m_timer_subscribed = false;
    std::deque<Subscription> m_subscriptions; // deque: the engine keeps pointers to the elements
    std::deque<Binding> m_bindings;           // same; freed nodes are reused by bind
    std::deque<ImGuiToolNode> m_imgui_tools;  // same; freed nodes are reused by imgui_tool_register
};
} // namespace gwp

/*
 * Defines the exported entry point for a plugin class derived from gwp::Plugin. The object is created here and
 * destroyed right after on_unload. Optional second argument: the minimal engine API version (by default the
 * version of the headers, GWP_API_VERSION; see Plugin::init).
 *     GWP_DEFINE_PLUGIN(MyPlugin)
 *     GWP_DEFINE_PLUGIN(MyPlugin, GWP_MAKE_VERSION(0, 1, 0))
 * GWP_DEFINE_PLUGIN_EXPAND makes the list of arguments be split again: the traditional preprocessor of MSVC (without
 * /Zc:preprocessor) passes __VA_ARGS__ on to GWP_DEFINE_PLUGIN_IMPL as one argument, so the form with api_min would put
 * "MyPlugin, api_min" into Type. The extra pass changes nothing for a conforming preprocessor.
 */
#define GWP_DEFINE_PLUGIN_EXPAND(x) x
#define GWP_DEFINE_PLUGIN(...) GWP_DEFINE_PLUGIN_EXPAND(GWP_DEFINE_PLUGIN_IMPL(__VA_ARGS__, GWP_API_VERSION, ))
#define GWP_DEFINE_PLUGIN_IMPL(Type, api_min, ...)                                                        \
    GWP_PLUGIN_INIT                                                                                       \
    {                                                                                                     \
        Type* plugin = new (std::nothrow) Type();                                                         \
        if (!plugin)                                                                                      \
            return GWP_ERROR;                                                                             \
        const GwpResult result = plugin->init(api, self, out, api_min);                                   \
        if (result != GWP_OK)                                                                             \
            delete plugin; /* on_unload is not called for a failed init */                                \
        return result;                                                                                    \
    }

#endif /* GWP_HPP */
