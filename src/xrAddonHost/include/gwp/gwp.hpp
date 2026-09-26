/*
 * GlobalWar Plugin API - C++ helpers for plugins (header-only).
 *
 * Compiled into the plugin; the engine never sees these types. Everything here is built on top of the
 * C ABI in gwp_api.h, so using or not using this header does not change binary compatibility.
 *
 * Requires C++20 (std::format): MSVC 19.29+, GCC 13+, Clang 17+ with libc++.
 * Docs: wiki/doc/plugins/api/logger.md
 */
#ifndef GWP_HPP
#define GWP_HPP

#ifndef __cplusplus
#error "gwp/gwp.hpp is a C++ header; C plugins use gwp/gwp_api.h directly"
#endif

#include "gwp_api.h"

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
} // namespace gwp

#endif /* GWP_HPP */
