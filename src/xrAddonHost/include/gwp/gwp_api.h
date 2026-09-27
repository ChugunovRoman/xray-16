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
    GWP_ERROR_ACCESS_DENIED = 5, /* e.g. data_set on a key outside the "<addon id>/" prefix */
} GwpResult;

typedef enum GwpLogLevel
{
    GWP_LOG_DEBUG = 0, /* printed only with the -addon_debug command line key */
    GWP_LOG_INFO = 1,
    GWP_LOG_WARNING = 2,
    GWP_LOG_ERROR = 3,
} GwpLogLevel;

/* ------------------------------------------------------------------------- */
/* Events                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * One event bus in the engine for Lua scripts and plugins. Events are identified by name
 * ("actor_on_reinit", "save_state", "npc_on_death_callback", ...); the name is resolved once to a GwpEventId.
 * Lua scripts publish through SendScriptCallback, the engine publishes its own lifecycle events
 * (alife_on_start, level_on_frame, alife_on_before_save, ...). List: wiki/doc/plugins/api/events_list.md
 */
typedef uint32_t GwpEventId;        /* 0 = invalid */
typedef uint32_t GwpSubscriptionId; /* 0 = invalid */
#define GWP_INVALID_EVENT_ID ((GwpEventId)0u)
#define GWP_INVALID_SUBSCRIPTION_ID ((GwpSubscriptionId)0u)

/* Flags of event_declare. */
#define GWP_EVENT_HAS_RESULT 0x1u /* the event carries a mutable result (GwpEvent::result) */

/* Flags of data_set. */
#define GWP_DATA_PERSISTENT 0x1u /* the value is stored in the game save; cleared on a new game, replaced on a load.
                                    Once set on a key it stays until data_erase, whoever writes the key later */

/* Flags of timer_start. */
#define GWP_TIMER_REAL_TIME 0x1u  /* level time (real seconds of unpaused frames) instead of game time */
#define GWP_TIMER_PERSISTENT 0x2u /* the timer is stored in the game save and comes back on a load */

/* Config file and one of its sections (group ini). Both are 0 when there is no such file or section. */
typedef uint32_t GwpIni;
typedef uint32_t GwpIniSection;
#define GWP_INVALID_INI ((GwpIni)0u)
#define GWP_INVALID_INI_SECTION ((GwpIniSection)0u)

/* Vertex of the AI graph of the level (group level). */
#define GWP_INVALID_LEVEL_VERTEX ((uint32_t)0xFFFFFFFFu)

/* What a ray of level_ray_pick hits (a combination of flags). */
#define GWP_RAY_OBJECT 0x1u   /* game objects */
#define GWP_RAY_STATIC 0x2u   /* static geometry of the level */
#define GWP_RAY_SHAPE 0x4u    /* shapes of zones and restrictors */
#define GWP_RAY_OBSTACLE 0x8u /* obstacles (doors and the like) */
#define GWP_RAY_ANY (GWP_RAY_OBJECT | GWP_RAY_STATIC)

typedef enum GwpValueType
{
    GWP_T_NIL = 0,
    GWP_T_BOOL = 1,          /* u.b: 0 or 1 */
    GWP_T_INT = 2,           /* u.i */
    GWP_T_NUMBER = 3,        /* u.n */
    GWP_T_STRING = 4,        /* u.s: UTF-8, not always zero-terminated inside Lua data: use len */
    GWP_T_VEC3 = 5,          /* u.v */
    GWP_T_OBJECT = 6,        /* u.id: client (online) game object */
    GWP_T_SERVER_OBJECT = 7, /* u.id: ALife server object */
    GWP_T_LUA_REF = 8,       /* Lua table or userdata without a native form; content is not available */
} GwpValueType;

typedef struct GwpString
{
    const char* ptr;
    uint32_t len;
} GwpString;

/* Typed value of an event argument. Strings live until the handler returns: copy them to keep. */
typedef struct GwpValue
{
    uint32_t type; /* GwpValueType; a fixed-size integer on purpose, the size of an enum is compiler-defined */
    uint32_t reserved;
    union
    {
        int32_t b;
        int64_t i;
        double n;
        GwpString s;
        float v[3];
        GwpObjectId id;
    } u;
} GwpValue;

/* What a handler receives. Valid only during the call. */
typedef struct GwpEvent
{
    GwpEventId id;
    const char* name;
    uint32_t argc;
    const GwpValue* argv;
    GwpValue* result; /* mutable result shared by all handlers; NULL when the event has none */
} GwpEvent;

typedef void(GWP_CALL* GwpEventHandler)(void* user, const GwpEvent* event);

/* Batch handler: every event collected since the previous delivery, in emit order. Arguments are copies made at
   emit time (strings included); object ids may already point to objects that went offline. result is NULL. */
typedef void(GWP_CALL* GwpEventBatchHandler)(void* user, uint32_t count, const GwpEvent* events);

/* Flags of GwpSubscribeOptions. */
#define GWP_SUBSCRIBE_BATCH 0x1u /* collect events and deliver them once per frame to batch_handler */

/* Options of event_subscribe_ex. Zero-initialize, then set size = sizeof(GwpSubscribeOptions). */
typedef struct GwpSubscribeOptions
{
    uint32_t size;        /* sizeof(GwpSubscribeOptions) of the plugin: fields may be appended in later versions */
    uint32_t flags;       /* GWP_SUBSCRIBE_* */
    uint32_t throttle_ms; /* at most one call per interval (game time_global, ms); 0 = every dispatch.
                             With GWP_SUBSCRIBE_BATCH: the minimal interval between deliveries; the records keep
                             piling up meanwhile, so size max_batch for (throttle_ms * emits per ms) */
    uint32_t max_batch;   /* GWP_SUBSCRIBE_BATCH: records kept between deliveries, 0 = 4096; extra ones are dropped
                             (a warning is logged once per delivery) */
    GwpEventBatchHandler batch_handler; /* required with GWP_SUBSCRIBE_BATCH */
} GwpSubscribeOptions;

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

    /* Id of the event with this name; registers the name when it is new. 0 only for an invalid name.
       Ids never change while the game runs: resolve once, emit by id.
       @group events @thread main */
    GwpEventId(GWP_CALL* event_id)(const char* name);

    /* Subscribes handler to the event `name` (declared or not yet). user is passed back to the handler.
       The same handler may be subscribed several times: each call is a separate subscription.
       Subscriptions of a plugin are removed by the engine after its on_unload.
       A subscription made during a dispatch of the same event takes effect from the next dispatch.
       @group events @thread main */
    GwpSubscriptionId(GWP_CALL* event_subscribe)(const GwpPlugin* self, const char* name, GwpEventHandler handler,
        void* user);
    void(GWP_CALL* event_unsubscribe)(const GwpPlugin* self, GwpSubscriptionId subscription);

    /* Declares an event owned by this plugin (flags: GWP_EVENT_HAS_RESULT). Emitting an undeclared event works
       but prints a warning once.
       @group events @thread main */
    GwpResult(GWP_CALL* event_declare)(const GwpPlugin* self, const char* name, uint32_t flags);

    /* Emits the event to every Lua and native subscriber synchronously. result may be NULL.
       Nested emits from a handler are allowed.
       @group events @thread main */
    GwpResult(GWP_CALL* event_emit)(const GwpPlugin* self, GwpEventId id, uint32_t argc, const GwpValue* argv,
        GwpValue* result);

    /* Online game objects by id. Every function returns 0 / NULL / GWP_INVALID_OBJECT_ID when there is no level
       or no such online object. Strings live while the object is online: copy them to keep.
       @group objects @thread main */
    int(GWP_CALL* object_exists)(GwpObjectId id);
    const char*(GWP_CALL* object_section)(GwpObjectId id);
    const char*(GWP_CALL* object_name)(GwpObjectId id);
    int32_t(GWP_CALL* object_clsid)(GwpObjectId id); /* same numbers as Lua clsid.*, -1 when unknown */
    int(GWP_CALL* object_position)(GwpObjectId id, float out_xyz[3]);
    int(GWP_CALL* object_is_alive)(GwpObjectId id); /* 0 for objects that are not living entities */
    GwpObjectId(GWP_CALL* actor_id)(void);

    /* Plugin data stored inside the game save (a chunk per addon).
       save_write replaces the data of this addon; it goes to disk with the next save (usually called from the
       alife_on_before_save handler). size == 0 removes the data. The engine copies the bytes.
       save_read returns the data of this addon from the loaded save (or written since); GWP_ERROR when there is
       none. The pointer is valid until the next save_write of this plugin, a load or a new game.
       Data of addons that are not installed now is carried to new saves unchanged.
       @group save @thread main */
    GwpResult(GWP_CALL* save_write)(const GwpPlugin* self, uint32_t data_version, const void* data, uint32_t size);
    GwpResult(GWP_CALL* save_read)(const GwpPlugin* self, uint32_t* data_version, const void** data, uint32_t* size);

    /* Shared key-value store of Lua scripts and plugins (Lua: global table data_bus). Keys are "<owner>/<name>";
       a plugin may write only keys that start with "<its addon id>/" (GWP_ERROR_ACCESS_DENIED otherwise), anyone
       may read any key. A value is any GwpValue except GWP_T_LUA_REF; strings are copied. Flags: GWP_DATA_PERSISTENT.
       A real change of a value sends the event data_on_changed(key, value); after data_erase value is nil.
       @group data @thread main */
    GwpResult(GWP_CALL* data_set)(const GwpPlugin* self, const char* key, const GwpValue* value, uint32_t flags);
    GwpResult(GWP_CALL* data_erase)(const GwpPlugin* self, const char* key);

    /* Reads a value; GWP_ERROR when there is no such key. A string in `out` lives until the key is changed or
       erased: copy it to keep.
       @group data @thread main */
    GwpResult(GWP_CALL* data_get)(const char* key, GwpValue* out);

    /* Timers of this addon (native analogue of Lua CreateTimeEvent, on game time; Lua: global table timer_bus).
       A timer has a name unique inside the addon; timer_start with an existing name replaces that timer. When the
       timer expires, the engine emits the event `event` (declared automatically; not a built-in engine event such as
       level_on_frame) with two arguments: the timer name and its full key "<addon id>/<name>"; subscribe to it with
       event_subscribe.
       Time base: game time in game seconds (scaled by the time factor), or with GWP_TIMER_REAL_TIME level time in
       real seconds; both stop on pause, in the main menu and during loading. delay >= 0; period > 0 repeats the
       timer every period seconds (at least 1 ms, at most once per frame), period == 0 fires it once.
       A new game, a load and a level change drop all timers; GWP_TIMER_PERSISTENT timers come back with the save.
       Timers of an addon whose plugin is not loaded wait and are carried to new saves.
       timer_start returns GWP_ERROR outside a game (ALife not created or not loaded yet), for both time bases. timer_stop and timer_remaining return GWP_ERROR when
       there is no such timer; timer_remaining gives the seconds left in the timer's own time base.
       @group timers @thread main */
    GwpResult(GWP_CALL* timer_start)(const GwpPlugin* self, const char* name, const char* event, double delay_seconds,
        double period_seconds, uint32_t flags);
    GwpResult(GWP_CALL* timer_stop)(const GwpPlugin* self, const char* name);
    GwpResult(GWP_CALL* timer_remaining)(const GwpPlugin* self, const char* name, double* out_seconds);

    /* Game time of ALife in milliseconds (Lua game.get_game_time()) and its time factor (game seconds per real
       second). 0 outside a game.
       @group timers @thread main */
    uint64_t(GWP_CALL* game_time_ms)(void);
    double(GWP_CALL* game_time_factor)(void);

    /* event_subscribe with options (GwpSubscribeOptions; NULL = plain event_subscribe): throttling and batch
       delivery. Batches are delivered at the start of every frame (main menu included) and right before
       level_on_stop, so the ids in them are still valid when level objects are destroyed. In batch mode `handler`
       is ignored (may be NULL) and the subscriber cannot change the result of an event.
       @group events @thread main */
    GwpSubscriptionId(GWP_CALL* event_subscribe_ex)(const GwpPlugin* self, const char* name,
        const GwpSubscribeOptions* options, GwpEventHandler handler, void* user);

    /* event_declare with an argument schema: one code per argument - b bool, I integer, N number, s string,
       v vector, o game object, O server object, t Lua table/userdata (GWP_T_LUA_REF), * anything; '?' after a
       code = the argument may be nil (or missing at the end). "" = no arguments, NULL = no schema. With
       -addon_debug (console gw_event_schema_check) every emit is checked and a mismatch is logged once per event.
       The first schema declared for an event is kept; another one is logged and ignored.
       event_schema returns the schema of the event (NULL when it has none); the string lives as long as the game.
       @group events @thread main */
    GwpResult(GWP_CALL* event_declare_ex)(const GwpPlugin* self, const char* name, const char* schema, uint32_t flags);
    const char*(GWP_CALL* event_schema)(GwpEventId id);

    /* Config files (ltx). A section is resolved once into a handle, reads go by the handle: a read by name costs
       a lowercase copy plus two binary searches inside the engine, a read by handle costs neither.
       ini_system is system.ltx (the sections of weapons, monsters, ...); ini_open reads a file of the mod by its
       virtual path ("$game_config$\\my_addon.ltx") and belongs to the plugin until ini_close or its unload.
       A missing section, a missing line or a stale handle gives the passed default value, never an error box.
       Handles and strings live until the configs are reloaded: ini_generation() changes then, and the plugin
       resolves its handles again (engine_on_script_start is a good place).
       @group ini @thread main */
    GwpIni(GWP_CALL* ini_system)(void);
    GwpIni(GWP_CALL* ini_open)(const GwpPlugin* self, const char* path);
    void(GWP_CALL* ini_close)(const GwpPlugin* self, GwpIni ini);
    uint32_t(GWP_CALL* ini_generation)(void);
    GwpIniSection(GWP_CALL* ini_section)(GwpIni ini, const char* name);
    int(GWP_CALL* ini_line_exists)(GwpIniSection section, const char* line);
    const char*(GWP_CALL* ini_read_string)(GwpIniSection section, const char* line, const char* def);
    double(GWP_CALL* ini_read_number)(GwpIniSection section, const char* line, double def);
    int(GWP_CALL* ini_read_bool)(GwpIniSection section, const char* line, int def);
    int(GWP_CALL* ini_read_vec3)(GwpIniSection section, const char* line, float out_xyz[3]);
    uint32_t(GWP_CALL* ini_line_count)(GwpIniSection section);
    int(GWP_CALL* ini_line_at)(GwpIniSection section, uint32_t index, const char** out_name, const char** out_value);

    /* Console variables. Reading works for any variable of the engine (npc_perf_*, r__*, ...); a plugin registers
       its own variables under the "<addon id>_" prefix (GWP_ERROR_ACCESS_DENIED otherwise). The value is stored by
       the engine, so it survives a reload of the plugin library and is removed when the addon is unloaded.
       cvar_get_string copies into `out` and returns the length it needed (0 when there is no such variable).
       @group console @thread main */
    int(GWP_CALL* cvar_exists)(const char* name);
    int(GWP_CALL* cvar_get_int)(const char* name, int def);
    double(GWP_CALL* cvar_get_float)(const char* name, double def);
    uint32_t(GWP_CALL* cvar_get_string)(const char* name, char* out, uint32_t size);
    GwpResult(GWP_CALL* cvar_register_int)(const GwpPlugin* self, const char* name, int def, int min, int max);
    GwpResult(GWP_CALL* cvar_register_float)(const GwpPlugin* self, const char* name, double def, double min,
        double max);
    GwpResult(GWP_CALL* cvar_set_int)(const GwpPlugin* self, const char* name, int value);
    GwpResult(GWP_CALL* cvar_set_float)(const GwpPlugin* self, const char* name, double value);

    /* Runs a console command, as typing it in the console does. console_execute runs it immediately, inside your
       call: a command that unloads the level or quits the game (quit, disconnect, load, main_menu, start) destroys
       objects right there, so from an event handler use console_execute_deferred, which runs the command at the
       end of the frame.
       @group console @thread main */
    GwpResult(GWP_CALL* console_execute)(const GwpPlugin* self, const char* command);
    GwpResult(GWP_CALL* console_execute_deferred)(const GwpPlugin* self, const char* command);

    /* Profiling stage of this plugin, printed by the engine with its own stages when the game runs with
       -npc_cpp_profile. Returns 0 while profiling is off: then profile_add does nothing and the plugin can skip
       the measurement.
       @group console @thread main */
    uint32_t(GWP_CALL* profile_stage)(const GwpPlugin* self, const char* name);

    /* Adds measured time to a stage, and the counter the measurement is made with. Both work from any thread:
       a plugin can measure the work it does in its own threads.
       @group console @thread any */
    void(GWP_CALL* profile_add)(uint32_t stage, uint64_t ticks);
    uint64_t(GWP_CALL* profile_ticks)(void);

    /* The level and the world around the objects. Everything here works only while a level is loaded
       (level_present returns 0 otherwise, and the rest gives 0 / an empty result).
       level_time_parts fills the game date and time in one call: year, month, day, hours, minutes, seconds,
       milliseconds; the absolute game time in milliseconds is game_time_ms (group timers).
       level_objects copies the ids of the online objects into `out` (at most `max`) and returns how many were
       copied; the ids are a snapshot, check them with object_exists before use.
       @group level @thread main */
    int(GWP_CALL* level_present)(void);
    const char*(GWP_CALL* level_name)(void);
    int(GWP_CALL* level_time_parts)(uint32_t out_parts[7]);
    uint32_t(GWP_CALL* level_object_count)(void);
    uint32_t(GWP_CALL* level_objects)(GwpObjectId* out, uint32_t max);

    /* Vertices of the AI graph of the level: the positions NPCs walk by. GWP_INVALID_LEVEL_VERTEX means "no such
       vertex"; level_vertex_id gives it for a position outside the graph.
       level_vertex_in_direction walks the graph from a vertex towards `dir` and returns the farthest vertex within
       `distance` (the vertex itself when it cannot move).
       @group level @thread main */
    uint32_t(GWP_CALL* level_vertex_id)(const float xyz[3]);
    int(GWP_CALL* level_vertex_valid)(uint32_t vertex_id);
    int(GWP_CALL* level_vertex_position)(uint32_t vertex_id, float out_xyz[3]);
    uint32_t(GWP_CALL* level_vertex_in_direction)(uint32_t vertex_id, const float dir[3], float distance);

    /* Traces a ray through the level: `targets` is a combination of GWP_RAY_* (GWP_RAY_ANY covers geometry and
       objects). Returns 1 on a hit and fills the distance and the object that was hit
       (GWP_INVALID_OBJECT_ID for static geometry); both out parameters may be NULL.
       `ignore` is an object the ray goes through (GWP_INVALID_OBJECT_ID to ignore nothing).
       @group level @thread main */
    int(GWP_CALL* level_ray_pick)(const float from_xyz[3], const float dir[3], float distance, uint32_t targets,
        GwpObjectId ignore, float* out_distance, GwpObjectId* out_object);

    /* Info portions: the flags the game keeps about a character ("met the trader", "quest finished"), the same
       ones Lua reads with has_info and gives with give_info_portion.
       Reading works for any character, online or offline (the data lives in the ALife registry), so `owner` may be
       an NPC far away. Giving and disabling need an online character: a portion runs its scripted effects, and
       offline there is nobody to run them for (GWP_ERROR then).
       info_time returns the game time of receiving in milliseconds (0 when the character has no such portion).
       @group info @thread main */
    int(GWP_CALL* info_has)(GwpObjectId owner, const char* info);
    GwpResult(GWP_CALL* info_give)(const GwpPlugin* self, GwpObjectId owner, const char* info);
    GwpResult(GWP_CALL* info_disable)(const GwpPlugin* self, GwpObjectId owner, const char* info);
    uint64_t(GWP_CALL* info_time)(GwpObjectId owner, const char* info);

    /* All the portions of a character, in the order they were received. The string of info_at lives until the
       character receives or loses a portion: copy it to keep.
       @group info @thread main */
    uint32_t(GWP_CALL* info_count)(GwpObjectId owner);
    const char*(GWP_CALL* info_at)(GwpObjectId owner, uint32_t index);

    /* Where the object looks (a unit vector), for example to trace a ray forward from it with level_ray_pick.
       0 when there is no such online object.
       @group objects @thread main */
    int(GWP_CALL* object_direction)(GwpObjectId id, float out_xyz[3]);

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
