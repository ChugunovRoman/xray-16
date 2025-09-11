#pragma once

#include "xrScriptEngine.hpp"

#include <array>

namespace xray::script_export
{
class XRSCRIPTENGINE_API node;

using export_func = void(*)(lua_State* luaState);
using dependencies_getter = std::pair<const node* const*, size_t>(*)();

class XRSCRIPTENGINE_API node
{
    node(const node&) = delete;
    node(node&&) = delete;

    node& operator=(const node&) = delete;
    node& operator=(node&&) = delete;

public:
    node(export_func export_func, dependencies_getter deps_getter);
    ~node();

    static void export_all(lua_State* luaState);

private:
    static void sort();

    static node* first_node;

    node* m_next_node{};
    export_func m_export_func{};
    dependencies_getter m_deps_getter{};
};

namespace detail
{
template <typename... Args>
auto get_dependencies()
{
    static const std::array<const node*, sizeof...(Args)> dependencies
    {
        Args::script_export_node()...
    };
    return std::pair<const node* const*, size_t>
    {
        dependencies.data(),
        dependencies.size()
    };
}
} // namespace detail
} // namespace xray::::script_export

#define DECLARE_SCRIPT_REGISTER_FUNCTION(...)                          \
    static void script_register(lua_State* luaState);                  \
    template <typename... Args>                                        \
    friend auto xray::script_export::detail::get_dependencies();       \
    static const xray::script_export::node* script_export_node()       \
    {                                                                  \
        return &m_script_export_node;                                  \
    }                                                                  \
    inline static const xray::script_export::node m_script_export_node \
    {                                                                  \
        &script_register,                                              \
        xray::script_export::detail::get_dependencies<__VA_ARGS__>     \
    }
