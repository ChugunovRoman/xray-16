/*
 * GlobalWar Plugin API - C ABI between the engine and native plugins.
 *
 * Terms:
 *  - Plugin: native C/C++ library (.dll/.so/.dylib) loaded by the engine and running as native code.
 *  - Addon:  game package (models, textures, configs, levels, shaders, Lua scripts, plugins) described
 *            by addon.ltx. A plugin always belongs to an addon; the addon manifest declares it in [plugin].
 *
 * Status: v0 DRAFT. Not stable, may change without notice until 1.0.
 * Docs:   wiki/doc/plugins
 * Design: plans/addons_api_support/08-unified-addons-api-lua-native.md
 *         plans/lua_to_cpp/03-build-and-discovery.md
 *
 * Rules of this header (enforced by review):
 *  - Plain C only: no C++ classes, no STL, no exceptions across the boundary.
 *  - Memory never crosses the boundary: whoever allocates, frees.
 *    Strings passed in are valid only until the callee returns.
 *  - New functions are appended to the END of GwpEngineApi only.
 *    Minor compatibility is checked via GwpEngineApi::size.
 *  - Objects are referenced by handles (u16 ids), never by engine pointers.
 *  - Every engine call is main-thread-only unless documented otherwise.
 */
#ifndef GWP_API_H
#define GWP_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Export / calling convention                                                */
/* ------------------------------------------------------------------------- */

#if defined(_WIN32)
#   define GWP_EXPORT __declspec(dllexport)
#   define GWP_CALL __cdecl
#else
#   define GWP_EXPORT __attribute__((visibility("default")))
#   define GWP_CALL
#endif

/* ------------------------------------------------------------------------- */
/* Versioning                                                                 */
/* ------------------------------------------------------------------------- */

/* Plugin API version. Checked against [plugin] api_min of the addon manifest and GwpPluginDesc::api_min. */
#define GWP_API_VERSION_MAJOR 0u
#define GWP_API_VERSION_MINOR 1u
#define GWP_API_VERSION_PATCH 0u

#define GWP_MAKE_VERSION(major, minor, patch) \
    ((uint32_t)(((major) & 0x3FFu) << 22) | (uint32_t)(((minor) & 0x3FFu) << 12) | (uint32_t)((patch) & 0xFFFu))

#define GWP_API_VERSION GWP_MAKE_VERSION(GWP_API_VERSION_MAJOR, GWP_API_VERSION_MINOR, GWP_API_VERSION_PATCH)

/* ------------------------------------------------------------------------- */
/* Basic types                                                                */
/* ------------------------------------------------------------------------- */

typedef uint16_t GwpObjectId; /* client object id (CGameObject::ID) */
#define GWP_INVALID_OBJECT_ID ((GwpObjectId)0xFFFFu)

typedef struct GwpPlugin GwpPlugin; /* opaque: plugin handle owned by the engine */

typedef enum GwpResult
{
    GWP_OK = 0,
    GWP_ERROR = 1,
    GWP_ERROR_VERSION_MISMATCH = 2,
    GWP_ERROR_NOT_MAIN_THREAD = 3,
    GWP_ERROR_INVALID_ARGUMENT = 4,
} GwpResult;

typedef enum GwpLogLevel
{
    GWP_LOG_DEBUG = 0, /* printed only with the -addon_debug command line key */
    GWP_LOG_INFO = 1,
    GWP_LOG_WARNING = 2,
    GWP_LOG_ERROR = 3,
} GwpLogLevel;

/* ------------------------------------------------------------------------- */
/* Engine API table (engine -> plugin)                                        */
/* ------------------------------------------------------------------------- */

typedef struct GwpEngineApi
{
    /* Header. Must stay first and never change layout. */
    uint32_t abi_major; /* == GWP_API_VERSION_MAJOR of the engine */
    uint32_t size;      /* sizeof(GwpEngineApi) as compiled into the engine */
    uint32_t api_version;

    /* --- v0.1 ------------------------------------------------------------ */
    /*
     * Every function carries tags for xray-16/src/gw_plugins/gen_plugin_api_index.py (wiki/doc/plugins/api/all.md):
     *   @group <name>   API group = wiki page (core, ...); the list of groups lives in the script
     *   @thread main|any  main: main thread only; any: callable from any thread
     * A tag comment applies to the declarations right below it, up to the next empty line.
     * The plugins build fails when a function has no tags.
     */

    /* Writes to the engine log with the "[plugin:<addon id>]" tag. text is UTF-8.
       @group core @thread main */
    void(GWP_CALL* log)(const GwpPlugin* self, GwpLogLevel level, const char* text);

    /* Identity of the addon that owns this plugin (from addon.ltx). Strings live as long as the plugin.
       @group core @thread main */
    const char*(GWP_CALL* addon_id)(const GwpPlugin* self);
    const char*(GWP_CALL* addon_version)(const GwpPlugin* self);

    /* Absolute physical directory of the addon (where addon.ltx lives), UTF-8, with trailing separator.
       @group core @thread main */
    const char*(GWP_CALL* addon_dir)(const GwpPlugin* self);

    /* Engine build info.
       @group core @thread main */
    uint32_t(GWP_CALL* engine_build_id)(void);

    /* Returns non-zero when called on the engine main thread.
       @group core @thread any */
    int(GWP_CALL* is_main_thread)(void);

    /* Returns non-zero when GWP_LOG_DEBUG messages are printed (game started with -addon_debug).
       Lets a plugin skip building debug text that would be dropped anyway.
       @group core @thread any */
    int(GWP_CALL* is_debug_log)(void);

    /* New functions go below this line only. */
} GwpEngineApi;

/* True when the engine table provides the member `field` (minor compatibility check). */
#define GWP_API_HAS(api, field) \
    ((api) != NULL && (api)->size >= (uint32_t)(offsetof(GwpEngineApi, field) + sizeof(((GwpEngineApi*)0)->field)))

/* ------------------------------------------------------------------------- */
/* Plugin description (plugin -> engine)                                      */
/* ------------------------------------------------------------------------- */

/*
 * Upper bound of sizeof(GwpPluginDesc) in ANY version of the API. The engine passes `out` as a buffer of this
 * size, so a plugin built against a newer minor version (longer struct) never writes past the engine's memory.
 * The engine reads only the fields it knows; the plugin reports its own sizeof in `size`.
 */
#define GWP_PLUGIN_DESC_MAX_SIZE 4096u

typedef struct GwpPluginDesc
{
    /* Header. Filled by the plugin. */
    uint32_t abi_major; /* GWP_API_VERSION_MAJOR the plugin was built against */
    uint32_t size;      /* sizeof(GwpPluginDesc) as compiled into the plugin (<= GWP_PLUGIN_DESC_MAX_SIZE) */
    uint32_t api_min;   /* minimal engine api_version required */

    /* Lifecycle. All optional (may be NULL). Called on the main thread. */
    void(GWP_CALL* on_unload)(void* user);

    /* Opaque pointer passed back to every plugin callback. */
    void* user;

    /* New fields go below this line only. */
} GwpPluginDesc;

/* ------------------------------------------------------------------------- */
/* Plugin entry point                                                         */
/* ------------------------------------------------------------------------- */

/*
 * Every plugin exports exactly this symbol (see GWP_PLUGIN_ENTRY_NAME).
 * The engine calls it once after loading the library, on the main thread.
 *  - api:  engine table, valid until on_unload returns.
 *  - self: plugin handle, valid until on_unload returns.
 *  - out:  zero-initialized buffer of GWP_PLUGIN_DESC_MAX_SIZE bytes; the plugin fills the header and callbacks.
 * Return GWP_OK to stay loaded. Any other value makes the engine unload the library;
 * the addon then works without its plugin, or fails if the manifest says [plugin] required = true.
 * on_unload is NOT called when init fails or when the header in `out` is rejected (abi_major mismatch,
 * api_min above the engine version): release nothing before the header is filled, check api->abi_major first.
 */
typedef GwpResult(GWP_CALL* GwpPluginInitFn)(const GwpEngineApi* api, const GwpPlugin* self, GwpPluginDesc* out);

#define GWP_PLUGIN_ENTRY_NAME "gwp_plugin_init"

#ifdef __cplusplus
} /* extern "C" */
#endif

/* Convenience macro for plugin sources: defines the exported entry point. */
#ifdef __cplusplus
#   define GWP_PLUGIN_INIT \
        extern "C" GWP_EXPORT GwpResult GWP_CALL gwp_plugin_init(const GwpEngineApi* api, const GwpPlugin* self, GwpPluginDesc* out)
#else
#   define GWP_PLUGIN_INIT \
        GWP_EXPORT GwpResult GWP_CALL gwp_plugin_init(const GwpEngineApi* api, const GwpPlugin* self, GwpPluginDesc* out)
#endif

#endif /* GWP_API_H */
