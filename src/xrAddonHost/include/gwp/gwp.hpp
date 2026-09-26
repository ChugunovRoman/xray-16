/*
 * GlobalWar Plugin API - C++ helpers for plugins (header-only).
 *
 * Compiled into the plugin; the engine never sees these types. Everything here is built on top of the
 * C ABI in gwp_api.h, so using or not using this header does not change binary compatibility.
 *
 * Requires C++20 (std::format): MSVC 19.29+, GCC 13+, Clang 17+ with libc++.
 * Docs: wiki/doc/plugins/api/logger.md, wiki/doc/plugins/api/events.md
 */
#ifndef GWP_HPP
#define GWP_HPP

#ifndef __cplusplus
#error "gwp/gwp.hpp is a C++ header; C plugins use gwp/gwp_api.h directly"
#endif

#include "gwp_api.h"

#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

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
 * Thread safety: the engine log may be written from the main thread only (see the Plugin API threading rules);
 * debug_enabled() is safe from any thread.
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
        return m_api != nullptr && GWP_API_HAS(m_api, is_debug_log) && m_api->is_debug_log != nullptr &&
            m_api->is_debug_log() != 0;
    }

    // GWP_LOG_DEBUG; not even formatted when debug output is off. @group logger @thread main
    template <typename... Args>
    void debug(std::format_string<Args...> fmt, Args&&... args) const noexcept
    {
        if (debug_enabled()) // do not even format the text when it would be dropped
            print(GWP_LOG_DEBUG, fmt, std::forward<Args>(args)...);
    }

    // GWP_LOG_INFO. @group logger @thread main
    template <typename... Args>
    void info(std::format_string<Args...> fmt, Args&&... args) const noexcept
    {
        print(GWP_LOG_INFO, fmt, std::forward<Args>(args)...);
    }

    // GWP_LOG_WARNING. @group logger @thread main
    template <typename... Args>
    void warn(std::format_string<Args...> fmt, Args&&... args) const noexcept
    {
        print(GWP_LOG_WARNING, fmt, std::forward<Args>(args)...);
    }

    // GWP_LOG_ERROR. @group logger @thread main
    template <typename... Args>
    void error(std::format_string<Args...> fmt, Args&&... args) const noexcept
    {
        print(GWP_LOG_ERROR, fmt, std::forward<Args>(args)...);
    }

    // Level chosen at runtime. @group logger @thread main
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

    // Text as is, without formatting: braces in `text` are not interpreted. @group logger @thread main
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
 * so a handler never reads a wrong union member. Numbers from Lua always come as GWP_T_NUMBER:
 * number() and integer() accept both GWP_T_NUMBER and GWP_T_INT.
 */
class EventView
{
public:
    // Wraps the event given to a handler; valid only during the handler call. @group events @thread main
    explicit EventView(const GwpEvent* event) noexcept : m_event(event) {}

    // Event name, e.g. "actor_on_reinit". @group events @thread main
    std::string_view name() const noexcept { return m_event && m_event->name ? m_event->name : ""; }

    // Number of arguments. @group events @thread main
    uint32_t size() const noexcept { return m_event ? m_event->argc : 0; }

    // Type of the argument (GwpValueType); GWP_T_NIL when index >= size(). @group events @thread main
    uint32_t type(uint32_t index) const noexcept { return at(index) ? at(index)->type : GWP_T_NIL; }

    // Argument as a number: GWP_T_NUMBER or GWP_T_INT. @group events @thread main
    double number(uint32_t index, double fallback = 0.0) const noexcept
    {
        const GwpValue* v = at(index);
        if (v && v->type == GWP_T_NUMBER)
            return v->u.n;
        if (v && v->type == GWP_T_INT)
            return static_cast<double>(v->u.i);
        return fallback;
    }

    // Argument as an integer: GWP_T_INT or a GWP_T_NUMBER without a fractional part. @group events @thread main
    int64_t integer(uint32_t index, int64_t fallback = 0) const noexcept
    {
        const GwpValue* v = at(index);
        if (v && v->type == GWP_T_INT)
            return v->u.i;
        if (v && v->type == GWP_T_NUMBER && std::trunc(v->u.n) == v->u.n && std::fabs(v->u.n) < 9.0e15)
            return static_cast<int64_t>(v->u.n);
        return fallback;
    }

    // Argument as a bool (GWP_T_BOOL). @group events @thread main
    bool boolean(uint32_t index, bool fallback = false) const noexcept
    {
        const GwpValue* v = at(index);
        return v && v->type == GWP_T_BOOL ? v->u.b != 0 : fallback;
    }

    // Argument as a string (GWP_T_STRING); empty for other types. Valid only during the handler call.
    // @group events @thread main
    std::string_view string(uint32_t index) const noexcept
    {
        const GwpValue* v = at(index);
        if (v && v->type == GWP_T_STRING && v->u.s.ptr)
            return std::string_view(v->u.s.ptr, v->u.s.len);
        return std::string_view();
    }

    // Argument as an online object id (GWP_T_OBJECT); GWP_INVALID_OBJECT_ID for other types.
    // @group events @thread main
    GwpObjectId object(uint32_t index) const noexcept
    {
        const GwpValue* v = at(index);
        return v && v->type == GWP_T_OBJECT ? v->u.id : GWP_INVALID_OBJECT_ID;
    }

    // Argument as an ALife server object id (GWP_T_SERVER_OBJECT). @group events @thread main
    GwpObjectId server_object(uint32_t index) const noexcept
    {
        const GwpValue* v = at(index);
        return v && v->type == GWP_T_SERVER_OBJECT ? v->u.id : GWP_INVALID_OBJECT_ID;
    }

    // Argument as a 3D vector (GWP_T_VEC3); false and `out` untouched for other types. @group events @thread main
    bool vec3(uint32_t index, float out[3]) const noexcept
    {
        const GwpValue* v = at(index);
        if (!v || v->type != GWP_T_VEC3 || !out)
            return false;
        out[0] = v->u.v[0];
        out[1] = v->u.v[1];
        out[2] = v->u.v[2];
        return true;
    }

    // True when the event carries a result (actor_on_before_hit, actor_on_before_death, ...).
    // @group events @thread main
    bool has_result() const noexcept { return m_event && m_event->result; }

    // Current result as a bool; fallback when there is no bool result. @group events @thread main
    bool result_bool(bool fallback = true) const noexcept
    {
        return has_result() && m_event->result->type == GWP_T_BOOL ? m_event->result->u.b != 0 : fallback;
    }

    // Sets the result, e.g. set_result(false) cancels a hit in actor_on_before_hit. No-op without a result.
    // @group events @thread main
    void set_result(bool value) const noexcept
    {
        if (!has_result())
            return;
        m_event->result->type = GWP_T_BOOL;
        m_event->result->u.b = value ? 1 : 0;
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
};
} // namespace gwp

#endif /* GWP_HPP */
