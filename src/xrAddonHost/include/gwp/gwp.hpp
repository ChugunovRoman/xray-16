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
#include <cstring>
#include <deque>
#include <exception>
#include <format>
#include <functional>
#include <new>
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
 */
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
    // Reads from a buffer that outlives the reader (e.g. the pointer of save_read). @group save @thread any
    BinaryReader(const void* data, size_t size) noexcept
        : m_data(static_cast<const uint8_t*>(data)), m_size(data ? size : 0) {}

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
 *  - timers: timer_start without an event name, expired timers come to on_timer(name).
 * The raw API stays available through api() and self(). Every handler runs on the main thread.
 * The object is created in gwp_plugin_init and destroyed right after on_unload, while the engine API is still valid.
 */
class Plugin
{
public:
    // @group plugin @thread main
    Plugin() = default;
    virtual ~Plugin() = default;

    // Called by GWP_DEFINE_PLUGIN from gwp_plugin_init: checks the versions, fills `out`, then runs on_init.
    // api_min: the minimal engine API this plugin needs (GWP_MAKE_VERSION). @group plugin @thread main
    GwpResult init(const GwpEngineApi* api, const GwpPlugin* self, GwpPluginDesc* out,
        uint32_t api_min = GWP_MAKE_VERSION(0, 1, 0)) noexcept
    {
        if (api == nullptr || out == nullptr || api->abi_major != GWP_API_VERSION_MAJOR)
            return GWP_ERROR_VERSION_MISMATCH;
        if (api->api_version < api_min)
            return GWP_ERROR_VERSION_MISMATCH;
        m_api = api;
        m_self = self;
        m_log.reset(api, self);
        m_addon_id = api->addon_id(self);

        out->abi_major = GWP_API_VERSION_MAJOR;
        out->size = sizeof(GwpPluginDesc);
        out->api_min = api_min;
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
    // version: save_version() of the plugin that wrote it (never above the current one: newer blocks are skipped
    // with a warning). Read in the order of on_save; check r.ok() at the end when the layout may be truncated.
    virtual void on_load(BinaryReader& in, uint32_t version) { (void)in; (void)version; }

    // Version of the layout written by on_save. Bump it when the layout changes and read the old one by `version`.
    virtual uint32_t save_version() const { return 1; }

    // A timer started with timer_start expired. name: the timer name given to timer_start.
    virtual void on_timer(std::string_view name) { (void)name; }

    // --- events ----------------------------------------------------------------------------------------------

    // Subscribes fn(const EventView&) to the event. Returns false when the engine refused (invalid name).
    // The handler is a lambda, a functor or a bound member; it is stored by the class until on_unload.
    template <typename F>
    bool subscribe(const char* name, F&& fn)
    {
        Subscription& sub = m_subscriptions.emplace_back();
        sub.owner = this;
        sub.fn = std::forward<F>(fn);
        sub.name = name;
        sub.id = m_api->event_subscribe(m_self, name, &event_trampoline, &sub);
        if (sub.id == GWP_INVALID_SUBSCRIPTION_ID)
        {
            m_subscriptions.pop_back();
            return false;
        }
        return true;
    }

    // Batch subscription (GWP_SUBSCRIBE_BATCH): fn(std::span<const GwpEvent>) gets every event collected since the
    // previous delivery, at most once per throttle_ms (0 = every frame). max_batch: records kept between deliveries
    // (0 = 4096). Returns false when the engine has no event_subscribe_ex or refused.
    template <typename F>
    bool subscribe_batch(const char* name, uint32_t throttle_ms, uint32_t max_batch, F&& fn)
    {
        if (!GWP_API_HAS(m_api, event_subscribe_ex))
            return false;
        Subscription& sub = m_subscriptions.emplace_back();
        sub.owner = this;
        sub.batch = std::forward<F>(fn);
        sub.name = name;
        GwpSubscribeOptions options{};
        options.size = sizeof(options);
        options.flags = GWP_SUBSCRIBE_BATCH;
        options.throttle_ms = throttle_ms;
        options.max_batch = max_batch;
        options.batch_handler = &batch_trampoline;
        sub.id = m_api->event_subscribe_ex(m_self, name, &options, nullptr, &sub);
        if (sub.id == GWP_INVALID_SUBSCRIPTION_ID)
        {
            m_subscriptions.pop_back();
            return false;
        }
        return true;
    }

    // --- timers (group timers of the engine API) --------------------------------------------------------------

    // Starts (or restarts) the timer `name` of this addon; on_timer(name) is called when it expires.
    // delay_seconds: first firing; period_seconds > 0 repeats it; flags: GWP_TIMER_REAL_TIME (level time instead of
    // game time), GWP_TIMER_PERSISTENT (kept in the save). False without a game, while a save is loading, or on an
    // engine without timers.
    bool timer_start(const char* name, double delay_seconds, double period_seconds = 0.0, uint32_t flags = 0)
    {
        if (!has_timers() || !subscribe_timer_event())
            return false;
        return m_api->timer_start(m_self, name, m_timer_event.c_str(), delay_seconds, period_seconds, flags) == GWP_OK;
    }

    // timer_start only when the timer does not exist: a persistent timer restored from the save is kept as it is.
    // Returns true when the timer exists afterwards.
    bool timer_start_if_missing(const char* name, double delay_seconds, double period_seconds = 0.0,
        uint32_t flags = 0)
    {
        double left = 0.0;
        if (has_timers() && m_api->timer_remaining(m_self, name, &left) == GWP_OK)
            return subscribe_timer_event();
        return timer_start(name, delay_seconds, period_seconds, flags);
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

private:
    struct Subscription
    {
        Plugin* owner = nullptr;
        GwpSubscriptionId id = GWP_INVALID_SUBSCRIPTION_ID;
        std::string name;
        EventFn fn;
        BatchFn batch;
    };

    // Trampolines: the engine calls plain functions with `user`; these forward to the stored functor.
    // Exceptions stop here with a log line; the engine would otherwise remove every subscription of the plugin.
    static void GWP_CALL event_trampoline(void* user, const GwpEvent* event) noexcept
    {
        auto* sub = static_cast<Subscription*>(user);
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

    static void GWP_CALL batch_trampoline(void* user, uint32_t count, const GwpEvent* events) noexcept
    {
        auto* sub = static_cast<Subscription*>(user);
        try
        {
            sub->batch(std::span<const GwpEvent>(events, count));
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

    static void GWP_CALL unload_trampoline(void* user) noexcept
    {
        auto* plugin = static_cast<Plugin*>(user);
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
        m_timer_subscribed = subscribe(m_timer_event.c_str(), [this](const EventView& e) { on_timer(e.string(0)); });
        return m_timer_subscribed;
    }

    void save_block()
    {
        BinaryWriter out;
        on_save(out);
        m_api->save_write(m_self, save_version(), out.data(), out.size()); // size 0 removes the block
    }

    void load_block()
    {
        uint32_t version = 0;
        const void* bytes = nullptr;
        uint32_t size = 0;
        if (m_api->save_read(m_self, &version, &bytes, &size) != GWP_OK)
            return; // no block of this addon in the save (made before the addon was installed)
        if (version > save_version())
        {
            m_log.warn("save block version {} is newer than {}, ignored", version, save_version());
            return;
        }
        BinaryReader in(bytes, size);
        on_load(in, version);
        if (!in.ok())
            m_log.warn("save block version {} ({} bytes) is truncated or has another layout", version, size);
    }

    const GwpEngineApi* m_api = nullptr;
    const GwpPlugin* m_self = nullptr;
    Logger m_log;
    std::string m_addon_id;
    std::string m_timer_event;
    bool m_timer_subscribed = false;
    std::deque<Subscription> m_subscriptions; // deque: the engine keeps pointers to the elements
};
} // namespace gwp

/*
 * Defines the exported entry point for a plugin class derived from gwp::Plugin. The object is created here and
 * destroyed right after on_unload. Optional second argument: the minimal engine API version.
 *     GWP_DEFINE_PLUGIN(MyPlugin)
 *     GWP_DEFINE_PLUGIN(MyPlugin, GWP_MAKE_VERSION(0, 1, 0))
 */
#define GWP_DEFINE_PLUGIN(...) GWP_DEFINE_PLUGIN_IMPL(__VA_ARGS__, GWP_MAKE_VERSION(0, 1, 0), )
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
