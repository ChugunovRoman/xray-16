/*
 * GlobalWar Plugin API - C ABI between the engine and native plugins.
 *
 * Terms:
 *  - Plugin: native C/C++ library (.dll/.so/.dylib) loaded by the engine and running as native code.
 *  - Addon:  game package (models, textures, configs, levels, shaders, Lua scripts, plugins) described
 *            by addon.ltx. A plugin always belongs to an addon; the addon manifest declares it in [plugin].
 *
 * Docs:   wiki/doc/plugins (versioning: wiki/doc/plugins/api/versioning.md)
 * Design: plans/addons_api_support/08-unified-addons-api-lua-native.md
 *         plans/lua_to_cpp/03-build-and-discovery.md
 *
 * Rules of this header (enforced by review):
 *  - Plain C only: no C++ classes, no STL, no exceptions across the boundary.
 *  - Memory never crosses the boundary: whoever allocates, frees.
 *    Strings passed in are valid only until the callee returns.
 *  - Objects are referenced by handles (u16 ids), never by engine pointers.
 *  - Every engine call is main-thread-only unless documented otherwise.
 *
 * Compatibility (MAJOR.MINOR.PATCH of GWP_API_VERSION):
 *  - A MINOR version only adds: functions at the end of GwpEngineApi (a plugin checks one with GWP_API_HAS, which
 *    compares GwpEngineApi::size), fields at the end of a structure that starts with `uint32_t size`, constants,
 *    flags, values of GwpValueType, error codes, events and event arguments at the end. Each group of new functions
 *    is one MINOR step.
 *  - Everything else - removing, reordering or changing a function, a field or the meaning of a value - is a new
 *    MAJOR: the engine refuses plugins of another MAJOR.
 *  - A structure that crosses the boundary starts with `uint32_t size` (the side that reads or writes it uses
 *    min(size, its own sizeof)) or is frozen as it is. An array of structures comes with its stride.
 *  - `reserved` fields are 0 and unknown flag bits are 0: the engine refuses anything else, so they can get a
 *    meaning later. The code is GWP_ERROR_INVALID_ARGUMENT, or GWP_ERROR_NOT_SUPPORTED where the function says so
 *    (a flag or a value that a later version may add); register functions return their invalid id.
 *  - Deprecation: a slot of GwpEngineApi is never NULL and never removed inside a MAJOR. A deprecated function keeps
 *    its slot, carries GWP_DEPRECATED in this header and returns GWP_ERROR_NOT_SUPPORTED (or its documented
 *    default) once it can no longer work; it goes away only with the next MAJOR.
 *  - Compare results with GWP_OK: a later version may return error codes this header does not know yet.
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

/* Marks a deprecated member of GwpEngineApi in its comment (the slot stays, see "Compatibility" above). A comment
   marker on purpose: a compiler attribute on a function pointer member would warn on every use of the table. */
#define GWP_DEPRECATED

/* Compile-time check of the ABI layout: static_assert (C++11), _Static_assert (C11), else an array type of a
   negative size (C89/C99, MSVC in C mode without /std:c11). One check per line. */
#define GWP_STATIC_ASSERT_CONCAT_(a, b) a##b
#define GWP_STATIC_ASSERT_CONCAT(a, b) GWP_STATIC_ASSERT_CONCAT_(a, b)
#if defined(__cplusplus)
#   define GWP_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#   define GWP_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
#   define GWP_STATIC_ASSERT(cond, msg)         typedef char GWP_STATIC_ASSERT_CONCAT(gwp_api_static_assert_, __LINE__)[(cond) ? 1 : -1]
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

/* Client object id (CGameObject::ID). 16 bits on purpose: object ids are 16-bit in the engine, the network packets
   and the saves (ALife ids, parent ids), so a wider type would only carry values the engine cannot produce. */
typedef uint16_t GwpObjectId;
#define GWP_INVALID_OBJECT_ID ((GwpObjectId)0xFFFFu)

typedef struct GwpPlugin GwpPlugin; /* opaque: plugin handle owned by the engine */

/* Result of an engine call. A fixed-size integer, not an enum: the size of an enum is compiler-defined and this type
   crosses the ABI. Compare with GWP_OK: new error codes may appear in later versions, an unknown one is a failure. */
typedef int32_t GwpResult;
#define GWP_OK 0
#define GWP_ERROR 1                  /* a failure without a more precise code */
#define GWP_ERROR_VERSION_MISMATCH 2
#define GWP_ERROR_NOT_MAIN_THREAD 3  /* called outside the game logic thread (see is_main_thread) */
#define GWP_ERROR_INVALID_ARGUMENT 4
#define GWP_ERROR_ACCESS_DENIED 5    /* e.g. data_set on a key outside the "<addon id>/" prefix */
#define GWP_ERROR_NOT_FOUND 6        /* no such key, object, function, file...: nothing failed, there is nothing */
#define GWP_ERROR_NOT_SUPPORTED 7    /* an unknown flag or value of a later version, a deprecated function */
#define GWP_ERROR_INVALID_STATE 8    /* not now: no game or level loaded, a call from inside the same callback */
#define GWP_ERROR_CRASHED 9          /* the called code (Lua or a plugin) failed; the log has the details */

/* Log level of GwpEngineApi::log; a fixed-size integer for the same reason as GwpResult. */
typedef int32_t GwpLogLevel;
#define GWP_LOG_DEBUG 0 /* printed only with the -addon_debug command line key */
#define GWP_LOG_INFO 1
#define GWP_LOG_WARNING 2
#define GWP_LOG_ERROR 3

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
    GWP_T_BYTES = 9,         /* u.s: binary data, any byte values (a Lua string, its bytes as they are) */
    GWP_T_ARRAY = 10         /* u.a: an array of values (a Lua table with the keys 1..n and nothing else) */
} GwpValueType;
/* A reader that meets a type it does not know (one of a later version) treats it as GWP_T_LUA_REF: a value without a
   native form. Arrays: a Lua table becomes GWP_T_ARRAY when its keys are exactly 1..n (n <= 4096) and every item has a
   native form, nested arrays included (at most 8 levels); any other table stays GWP_T_LUA_REF. An array going to Lua
   becomes a new table {items[0], ..., items[count - 1]} (keys 1..count). The items of an array the engine passes live
   as long as the array itself (the same rules as a string); copy them to keep. One value holds at most 65536 values in
   total, nested ones included (an array that several items share counts each time), and the strings and bytes in it
   are at most 16 MiB (16777216 bytes) together (the sum of `len` of every string and bytes, a string referenced
   several times counted each time: each reference is copied); a Lua table over that stays GWP_T_LUA_REF. A single
   string or bytes a plugin passes is at most 1 MiB (1048576 bytes). A plugin value over these
   limits is refused (GWP_ERROR_INVALID_ARGUMENT). A string or bytes with ptr == NULL and len == 0 is valid: nil in Lua
   (an empty string for GWP_T_BYTES), and an empty string in every copy the engine keeps (event batches, the data bus,
   timer arguments, results). */

typedef struct GwpString
{
    const char* ptr;
    uint32_t len;
} GwpString;

struct GwpValue;

/* The items of a GWP_T_ARRAY value. */
typedef struct GwpArray
{
    const struct GwpValue* items;
    uint32_t count;
} GwpArray;

/* Typed value of an event argument. Strings live until the handler returns: copy them to keep. */
typedef struct GwpValue
{
    uint32_t type;     /* GwpValueType; a fixed-size integer on purpose, the size of an enum is compiler-defined */
    uint32_t reserved; /* 0: a value with another reserved field is refused (GWP_ERROR_INVALID_ARGUMENT) */
    union
    {
        int32_t b;
        int64_t i;
        double n;
        GwpString s; /* GWP_T_STRING, GWP_T_BYTES */
        float v[3];
        GwpObjectId id;
        GwpArray a; /* GWP_T_ARRAY */
    } u;
} GwpValue;

/* What a handler receives. Valid only during the call. Read the fields after `size` only when size covers them. */
typedef struct GwpEvent
{
    uint32_t size;  /* sizeof(GwpEvent) of the engine: fields may be appended in later versions */
    uint32_t flags; /* GWP_EVENT_FLAG_* */
    GwpEventId id;
    uint32_t argc;
    const char* name;
    const GwpValue* argv;
    GwpValue* result; /* mutable result shared by all handlers; NULL when the event has none. Lives until the emit
                         returns (the handler sees it until it returns); the engine does not copy it: a string, bytes
                         or an array put into it must outlive the dispatch, and the emitter copies what it needs */
} GwpEvent;

/* Flags of GwpEvent. */
#define GWP_EVENT_FLAG_ENGINE 0x1u /* the engine itself sent the event (not a Lua script or a plugin) */

typedef void(GWP_CALL* GwpEventHandler)(void* user, const GwpEvent* event);

/* Batch handler: every event collected since the previous delivery, in emit order. Arguments are copies made at
   emit time (strings included); object ids may already point to objects that went offline. result is NULL.
   `events` is an array of `count` records `stride` bytes apart (stride >= the size of the engine's GwpEvent): step
   through it with the stride, never with sizeof(GwpEvent) of the plugin. */
typedef void(GWP_CALL* GwpEventBatchHandler)(void* user, uint32_t count, const GwpEvent* events, uint32_t stride);

/* Flags of GwpSubscribeOptions. */
#define GWP_SUBSCRIBE_BATCH 0x1u /* collect events and deliver them once per frame to batch_handler */
#define GWP_SUBSCRIBE_OBJECT 0x2u /* only emits whose first argument is the object `object` (a game or a server
                                     object); the subscription ends by itself when that object goes offline */

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
    GwpObjectId object;                 /* GWP_SUBSCRIBE_OBJECT: the object; an online object is required */
    uint16_t reserved;                  /* 0 */
    uint32_t reserved2;                 /* 0; fills the tail that the alignment would leave on 64-bit targets */
} GwpSubscribeOptions;

/* ------------------------------------------------------------------------- */
/* Binders of game objects (group binders)                                    */
/* ------------------------------------------------------------------------- */

/*
 * A binder is the plugin's own set of callbacks for the life of every online object whose class or section
 * matches: spawned, updated by the scheduler, gone offline. It lives next to the Lua binder of the object
 * (script_binding in the ltx stays as it is) and gets the same update throttling the engine gives that binder.
 * Objects are identified by id; the plugin keeps its per-object state on its side.
 */
typedef uint32_t GwpBinderId; /* 0 = invalid */
#define GWP_INVALID_BINDER_ID ((GwpBinderId)0u)

/* One record of an update batch: the object and the milliseconds since its previous scheduled update. */
typedef struct GwpBinderUpdate
{
    GwpObjectId id;
    uint16_t reserved;
    uint32_t dt_ms;
} GwpBinderUpdate;

/* Callbacks of a binder. Zero-initialize, set size = sizeof(GwpBinderVTable); every callback may be NULL.
   All are called on the main thread; `user` is the pointer given to binder_register.
   binder_register refuses (GWP_INVALID_BINDER_ID) a size below GWP_BINDER_VTABLE_MIN_SIZE (the table up to
   on_update) and a non-zero `reserved`; it reads only the fields that `size` covers in full, the others are NULL. */
typedef struct GwpBinderVTable
{
    uint32_t size;     /* sizeof(GwpBinderVTable) of the plugin: fields may be appended in later versions */
    uint32_t reserved; /* 0, anything else is refused */
    /* The object was reset before its spawn (reinit of the engine, right after the Lua binder's reinit): comes
       before on_spawn of the same object. */
    void(GWP_CALL* on_reinit)(void* user, GwpObjectId id);
    /* The object is online (its Lua binder net_spawn succeeded). section: the ltx section of the object, valid
       while the object is online. An object that was online when the binder was registered gets on_spawn at
       once (without on_reinit). */
    void(GWP_CALL* on_spawn)(void* user, GwpObjectId id, const char* section);
    /* The object goes offline: before its Lua binder net_destroy (db.storage of the object still exists).
       The last call for this id. Also sent by binder_unregister for every bound object. */
    void(GWP_CALL* on_destroy)(void* user, GwpObjectId id);
    /* The objects of this binder that the scheduler updated since the previous call: one call per frame, after
       the scheduler pass, only when there is at least one record. The array lives until the call returns; its
       records are `stride` bytes apart (stride >= the size of the engine's GwpBinderUpdate): step through it with
       the stride, never with sizeof(GwpBinderUpdate) of the plugin. */
    void(GWP_CALL* on_update)(void* user, uint32_t count, const GwpBinderUpdate* updates, uint32_t stride);
    /* State of one object for the save (wiki/doc/plugins/api/binders.md, "saved state"). Asked for every
       bound online object while a save is written, and right before on_destroy when the object goes offline (or the
       binder is unregistered). Write the state into `out` and return its size in bytes; 0 = nothing to keep (the
       kept state of the object is dropped). When the state needs more than `cap` bytes, write nothing and return
       the size: the engine calls again with a buffer that big. At most GWP_BINDER_SAVE_MAX_SIZE bytes. The engine
       keeps the bytes per addon, class id and section mask of the binder (not per binder id: they come back after a
       load, to a binder registered with the same class and mask) and per object, in the save of the game. When the
       binder goes away without on_save (the plugin crashed or is unloaded without binder_unregister), the last
       bytes the engine knows for each bound object (from on_load or the on_save of a save) are kept instead.
       While a save is written, on_save runs inside the building of the ALife save stream: it must not change the
       world. binder_register / binder_unregister and the ALife writes (alife_create, alife_release, teleports,
       switches, kills, squads) answer GWP_ERROR_INVALID_STATE (GWP_INVALID_BINDER_ID) then; do not call Lua that
       spawns or releases objects either. */
    uint32_t(GWP_CALL* on_save)(void* user, GwpObjectId id, void* out, uint32_t cap);
    /* The state on_save gave for this object, when there is one: right before on_spawn (after on_reinit), in the
       same game or after a save is loaded. `data` lives until the call returns. The state is handed over once and
       forgotten by the engine. It is dropped when the server object is released (its id may go to another object
       later) and when the object comes back with another section. */
    void(GWP_CALL* on_load)(void* user, GwpObjectId id, const void* data, uint32_t size);
} GwpBinderVTable;

/* The smallest GwpBinderVTable::size binder_register takes: the table up to on_update (version 0.1). */
#define GWP_BINDER_VTABLE_MIN_SIZE ((uint32_t)offsetof(GwpBinderVTable, on_save))
/* The most bytes on_save may keep for one object. */
#define GWP_BINDER_SAVE_MAX_SIZE ((uint32_t)65536u)

/* ------------------------------------------------------------------------- */
/* Object storage shared with Lua (group storage)                             */
/* ------------------------------------------------------------------------- */

/*
 * db.storage[id] of the Lua scripts, seen from a plugin. Every online object has an entry there (created by its
 * Lua binder), and the scripts keep the state of the object in it: the active logic section, the enemy, the
 * danger flag, the fields of the schemes. A fixed list of scalar fields of the entry (storage_key_name) is
 * mirrored by the engine: a Lua write of such a field reaches the plugin, a plugin write reaches Lua. The rest
 * of the entry (tables, objects) stays Lua-only.
 * Handle: the object id plus the generation of its entry, so a handle kept across frames never reads the entry
 * of another object that got the same id later (storage_valid tells).
 */
typedef uint32_t GwpStorageHandle; /* (generation << 16) | object id; 0 = none */
#define GWP_INVALID_STORAGE_HANDLE ((GwpStorageHandle)0u)
#define GWP_STORAGE_HANDLE_OBJECT(handle) ((GwpObjectId)((handle) & 0xFFFFu))

/* ------------------------------------------------------------------------- */
/* Decisions of the engine that plugins take part in (group callbacks)        */
/* ------------------------------------------------------------------------- */

typedef uint32_t GwpCallbackId; /* 0 = invalid */
#define GWP_INVALID_CALLBACK_ID ((GwpCallbackId)0u)

/* May `npc` treat `enemy` as an enemy? 1 = yes, 0 = ignore it (the NPC does not fight it). Asked only for enemies the
   engine already accepts (alive, hostile, reachable); the Lua callback of the NPC (set_enemy_callback) is asked
   first, every native filter after it, and all of them must agree. */
typedef int(GWP_CALL* GwpEnemyFilterFn)(void* user, GwpObjectId npc, GwpObjectId enemy);

/* May `npc` extrapolate its patrol path at point `point_index` (keep walking in the direction of the path while the
   next point is being chosen)? 1 = yes. The Lua callback of the NPC (set_patrol_extrapolate_callback) is asked
   first, every native one after it. */
typedef int(GWP_CALL* GwpPatrolExtrapolateFn)(void* user, GwpObjectId npc, uint32_t point_index);

/* ------------------------------------------------------------------------- */
/* Planner of the NPC: native evaluators (group goap)                        */
/* ------------------------------------------------------------------------- */

typedef uint32_t GwpEvaluatorId; /* 0 = invalid */
#define GWP_INVALID_EVALUATOR_ID ((GwpEvaluatorId)0u)

/* The value of a world property for `npc`: 1 or 0. Called by the planner of the NPC, as often as the Lua evaluator
   it replaces (tens of times per second for a nearby NPC): keep it cheap, cache on the plugin side when needed. */
typedef int(GWP_CALL* GwpEvaluatorFn)(void* user, GwpObjectId npc);

/* Who decides the value of a replaced property (evaluator_register, evaluator_set_mode). */
#define GWP_EVALUATOR_LUA 0u    /* the Lua evaluator, as without the plugin; the native function is not called */
#define GWP_EVALUATOR_NATIVE 1u /* the native function; the Lua evaluator is not called */
#define GWP_EVALUATOR_SHADOW 2u /* both run, the Lua value is used, the calls where they differ are counted */

/* The `mode` argument of evaluator_register / evaluator_set_mode / action_register / action_set_mode: the low 16
   bits are the mode (GWP_EVALUATOR_* / GWP_ACTION_*), the high 16 bits are flags. No flag is defined yet: a
   non-zero flag bit is refused (GWP_ERROR_NOT_SUPPORTED, an invalid id from the register functions). */
#define GWP_MODE_MASK 0x0000FFFFu
#define GWP_MODE_FLAGS_MASK 0xFFFF0000u

/* Where the property lives (evaluator_register): the main planner of the NPC (motivation_action_manager in Lua)
   or a planner that is an action of it (cast_planner(manager:action(id)) in Lua), given by the action id. */
#define GWP_PLANNER_MAIN 0xFFFFFFFFu

/* Counters of a registration since it was made (evaluator_stats). */
typedef struct GwpEvaluatorStats
{
    uint32_t size;         /* sizeof(GwpEvaluatorStats) of the plugin */
    uint32_t mode;         /* GWP_EVALUATOR_* now */
    uint32_t installed;    /* NPCs whose planner has the native evaluator now */
    uint32_t skipped;      /* NPCs where the property held another evaluator or that died in the queue (not installed);
                              an NPC whose planner never got the property (its scheme is not theirs) is not counted */
    uint64_t native_calls; /* calls of the native function */
    uint64_t lua_calls;    /* calls of the Lua evaluator through the proxy */
    uint64_t mismatches;   /* GWP_EVALUATOR_SHADOW: calls where native and Lua disagreed */
} GwpEvaluatorStats;

/* ------------------------------------------------------------------------- */
/* Planner of the NPC: native actions (group goap)                            */
/* ------------------------------------------------------------------------- */

typedef uint32_t GwpActionRegId; /* handle of an action_register registration; 0 = invalid */
#define GWP_INVALID_ACTION_REG_ID ((GwpActionRegId)0u)

/* Who runs the steps of a replaced action (action_register, action_set_mode). The mode of a run is fixed at its
   initialize: a switch takes effect from the next run of the action. */
#define GWP_ACTION_LUA 0u    /* the Lua action, as without the plugin; the plugin steps are not called */
#define GWP_ACTION_NATIVE 1u /* the plugin steps; the Lua action is not called */
#define GWP_ACTION_VERIFY 2u /* the Lua action acts; the plugin only decides (pre_execute) and compares
                                 (post_execute) around its execute step - actions have side effects, so both cannot
                                 run; what is comparable is up to the plugin (the state fields the action writes) */

/* Counters of an action registration since it was made (action_stats). */
typedef struct GwpActionStats
{
    uint32_t size;           /* sizeof(GwpActionStats) of the plugin */
    uint32_t mode;           /* GWP_ACTION_* now */
    uint32_t installed;      /* NPCs whose planner holds the proxy of this registration now */
    uint32_t skipped;        /* NPCs where the action slot held another action or that died in the queue */
    uint32_t running_native; /* proxies whose current run is native (fixed at its initialize) */
    uint32_t running_verify; /* proxies whose current run is the verify mode */
    uint64_t native_steps;   /* initialize/execute/finalize steps the plugin ran */
    uint64_t lua_steps;      /* steps that ran in the Lua action */
    uint64_t handovers;      /* native runs handed over to Lua in the middle (the plugin crashed or unregistered):
                                the Lua action is initialized at its next execute and takes the run */
} GwpActionStats;

/* A planner of a plugin (planner_create): the GOAP planner of the engine with native evaluators and actions. */
typedef uint32_t GwpPlannerId; /* 0 = invalid */
#define GWP_INVALID_PLANNER_ID ((GwpPlannerId)0u)
#define GWP_INVALID_ACTION_ID 0xFFFFFFFFu

/* One condition of a world state: the property and its value (0/1). */
typedef struct GwpWorldProperty
{
    uint32_t id;
    int32_t value;
} GwpWorldProperty;

/* The steps of a native action (planner_add_action, action_register), each may be NULL. Zero-initialize, set size.
   The engine runs its own part first (the start time of the action), as a Lua action calling
   action_base.initialize(self). pre_execute and post_execute serve the verify mode of action_register only: the
   engine calls them right before and right after the execute step of the wrapped Lua action, so the plugin can
   decide on the same world state, dry (no side effects), and compare its decision with what Lua really did. They
   are present when size covers them (older plugins compiled against the shorter table never see the calls). */
typedef struct GwpActionVTable
{
    uint32_t size;     /* sizeof(GwpActionVTable) of the plugin */
    uint32_t reserved; /* 0 */
    void(GWP_CALL* initialize)(void* user, GwpObjectId npc);
    void(GWP_CALL* execute)(void* user, GwpObjectId npc);
    void(GWP_CALL* finalize)(void* user, GwpObjectId npc);
    void(GWP_CALL* pre_execute)(void* user, GwpObjectId npc);  /* GWP_ACTION_VERIFY: before the Lua execute step */
    void(GWP_CALL* post_execute)(void* user, GwpObjectId npc); /* GWP_ACTION_VERIFY: after the Lua execute step */
} GwpActionVTable;

/* A function of a plugin that Lua calls (script_export): arguments and the result as for events. The strings of
   argv live until the function returns; a string put into *result is copied by the engine right after the
   function returns, so it must outlive the return: a string of argv, a literal or a buffer the plugin keeps (a
   member, a static) - not a local buffer or a local std::string of the function, which are gone by then.
   Return GWP_OK: Lua gets *result. Any other code: Lua gets nil and the error text ("not_found", "invalid_argument",
   ...), the pair a Lua function returns on failure (plugins.call returns nil, err). */
typedef GwpResult(GWP_CALL* GwpExportFn)(void* user, uint32_t argc, const GwpValue* argv, GwpValue* result);

/* ------------------------------------------------------------------------- */
/* NPC state (group npc)                                                      */
/* ------------------------------------------------------------------------- */

/* What the NPC fears most now (npc_best_danger); the fields of Lua danger_object. Set size = sizeof(GwpDanger)
   before the call: the engine writes min(size, its own sizeof) bytes and puts that number into size (a plugin newer
   than the engine sees which fields were not written). */
typedef struct GwpDanger
{
    uint32_t size;       /* sizeof(GwpDanger) of the plugin: fields may be appended in later versions */
    uint32_t type;       /* GWP_DANGER_*: danger_object.* in Lua */
    uint32_t time;       /* time_global() when it was perceived, ms */
    GwpObjectId object;  /* who caused it, GWP_INVALID_OBJECT_ID when nobody */
    uint16_t reserved;
    float position[3];
} GwpDanger;

#define GWP_DANGER_BULLET_RICOCHET 0u
#define GWP_DANGER_ATTACK_SOUND 1u
#define GWP_DANGER_ENTITY_ATTACKED 2u
#define GWP_DANGER_ENTITY_DEATH 3u
#define GWP_DANGER_FRESH_ENTITY_CORPSE 4u
#define GWP_DANGER_ATTACKED 5u
#define GWP_DANGER_GRENADE 6u
#define GWP_DANGER_ENEMY_SOUND 7u

/* Tracy zones of a plugin (group console). A site is one place in the code, registered once; a zone is one
   pass through it. Both are 0 when the engine is built without Tracy: the calls then do nothing. */
typedef uint32_t GwpZoneSite;
typedef uint64_t GwpZone;
#define GWP_INVALID_ZONE_SITE ((GwpZoneSite)0u)


/* ------------------------------------------------------------------------- */
/* NPC control (groups npc_body, npc_sight, npc_anim, inventory, smart_cover) */
/* ------------------------------------------------------------------------- */

/* The numbers are the engine's own; Lua scripts see the same ones in the tables named after each line. The values
   of every GWP_* constant are frozen: a later version only adds constants, never renumbers one (the engine checks
   them against its enums with static_assert). A function taking one of these values refuses anything above the
   last named one of its list (GWP_ERROR_INVALID_ARGUMENT); where a list is open (sounds a script registered, the
   animation slot of an item) its comment says so. */

/* Movement type (MonsterSpace::EMovementType; Lua move.walk / move.run / move.stand). */
#define GWP_MOVEMENT_WALK 0u
#define GWP_MOVEMENT_RUN 1u
#define GWP_MOVEMENT_STAND 2u

/* Body state (MonsterSpace::EBodyState; Lua move.crouch / move.standing). */
#define GWP_BODY_CROUCH 0u
#define GWP_BODY_STAND 1u

/* Mental state (MonsterSpace::EMentalState; Lua anim.danger / anim.free / anim.panic). */
#define GWP_MENTAL_DANGER 0u
#define GWP_MENTAL_FREE 1u
#define GWP_MENTAL_PANIC 2u

/* Path type (MovementManager::EPathType; Lua game_object.game_path / level_path / patrol_path / no_path). */
#define GWP_PATH_GAME 0u
#define GWP_PATH_LEVEL 1u
#define GWP_PATH_PATROL 2u
#define GWP_PATH_NONE 3u

/* Detail path type (DetailPathManager::EDetailPathType; Lua move.line / move.curve = SMOOTH, move.dodge =
   SMOOTH_DODGE, move.criteria / move.curve_criteria = SMOOTH_CRITERIA). */
#define GWP_DETAIL_PATH_SMOOTH 0u
#define GWP_DETAIL_PATH_SMOOTH_DODGE 1u
#define GWP_DETAIL_PATH_SMOOTH_CRITERIA 2u

/* How a stalker picks its next game vertex on a game path (ESelectionType; Lua
   game_object.alifeMovementTypeMask / alifeMovementTypeRandom). */
#define GWP_ALIFE_MOVEMENT_MASK 0u
#define GWP_ALIFE_MOVEMENT_RANDOM 1u

/* No vertex of the game graph (a game vertex id is a u16 in the engine; widened, as the level vertex). */
#define GWP_INVALID_GAME_VERTEX ((uint32_t)0xFFFFFFFFu)

/* A patrol path of the level by its name (group patrol): the handle names the path, not one load of it - it
   stays valid across levels and answers "no path" while the current level has none of that name. */
typedef uint32_t GwpPatrol; /* 0 = invalid */
#define GWP_INVALID_PATROL 0u
#define GWP_INVALID_PATROL_POINT ((uint32_t)0xFFFFFFFFu)

/* How an NPC enters a patrol path (EPatrolStartType; Lua patrol.start / stop / nearest / custom / next) and what it
   does at its end (EPatrolRouteType; Lua patrol.stop / patrol.continue). */
#define GWP_PATROL_START_FIRST 0u
#define GWP_PATROL_START_LAST 1u
#define GWP_PATROL_START_NEAREST 2u
#define GWP_PATROL_START_POINT 3u
#define GWP_PATROL_START_NEXT 4u
#define GWP_PATROL_ROUTE_STOP 0u
#define GWP_PATROL_ROUTE_CONTINUE 1u

/* A sound of the sound player of a stalker (StalkerSpace::EStalkerSounds; most have a Lua stalker_ids.sound_* -
   sound_enemy_killed_or_wounded there is the mask, not this number, THROW_GRENADE has no Lua name), for
   npc_play_sound. The list is open above GWP_STALKER_SOUND_SCRIPT: the numbers a script registered with
   npc:add_sound(prefix, count, type, ...) are its own and pass as they are (outside the guarantees of this
   header). A monster has its own sound types (MonsterSound::EType), not named here. */
#define GWP_STALKER_SOUND_DIE 0u
#define GWP_STALKER_SOUND_DIE_IN_ANOMALY 1u
#define GWP_STALKER_SOUND_INJURING 2u
#define GWP_STALKER_SOUND_HUMMING 3u
#define GWP_STALKER_SOUND_ALARM 4u /* stalker_ids.sound_alarm */
#define GWP_STALKER_SOUND_ATTACK_NO_ALLIES 5u
#define GWP_STALKER_SOUND_ATTACK_ALLIES_SINGLE_ENEMY 6u
#define GWP_STALKER_SOUND_ATTACK_ALLIES_SEVERAL_ENEMIES 7u
#define GWP_STALKER_SOUND_BACKUP 8u
#define GWP_STALKER_SOUND_DETOUR 9u
#define GWP_STALKER_SOUND_SEARCH1_WITH_ALLIES 10u
#define GWP_STALKER_SOUND_SEARCH1_NO_ALLIES 11u
#define GWP_STALKER_SOUND_ENEMY_LOST_NO_ALLIES 12u
#define GWP_STALKER_SOUND_ENEMY_LOST_WITH_ALLIES 13u
#define GWP_STALKER_SOUND_INJURING_BY_FRIEND 14u
#define GWP_STALKER_SOUND_PANIC_HUMAN 15u
#define GWP_STALKER_SOUND_PANIC_MONSTER 16u
#define GWP_STALKER_SOUND_TOLLS 17u
#define GWP_STALKER_SOUND_WOUNDED 18u
#define GWP_STALKER_SOUND_GRENADE_ALARM 19u
#define GWP_STALKER_SOUND_FRIENDLY_GRENADE_ALARM 20u
#define GWP_STALKER_SOUND_NEED_BACKUP 21u
#define GWP_STALKER_SOUND_RUNNING_IN_DANGER 22u
#define GWP_STALKER_SOUND_KILL_WOUNDED 23u
#define GWP_STALKER_SOUND_ENEMY_CRITICALLY_WOUNDED 24u
#define GWP_STALKER_SOUND_ENEMY_KILLED_OR_WOUNDED 25u
#define GWP_STALKER_SOUND_THROW_GRENADE 26u
#define GWP_STALKER_SOUND_SCRIPT 27u /* stalker_ids.sound_script: the first number of the sounds of the scripts */

/* Inventory slots (the slot enum of the engine, inventory_space.h), inventory_item_in_slot. The engine numbers, without
   the shift the Lua item_in_slot applies under the compatibility option minus_one_slot_ordering. GWP_SLOT_COUNT
   ends the named slots of the engine; a mod may add more (slot_persistent_N of system.ltx [inventory]: GlobalWar
   has slot 14), up to the last slot of the inventory - the functions take any slot 1 .. that last slot (an open
   list above GWP_SLOT_COUNT - 1). GWP_SLOT_NONE is "no slot" (no active item). */
#define GWP_SLOT_NONE 0u
#define GWP_SLOT_KNIFE 1u
#define GWP_SLOT_PISTOL 2u
#define GWP_SLOT_RIFLE 3u
#define GWP_SLOT_GRENADE 4u
#define GWP_SLOT_BINOCULAR 5u
#define GWP_SLOT_BOLT 6u
#define GWP_SLOT_OUTFIT 7u
#define GWP_SLOT_PDA 8u
#define GWP_SLOT_DETECTOR 9u
#define GWP_SLOT_TORCH 10u
#define GWP_SLOT_ARTEFACT 11u
#define GWP_SLOT_HELMET 12u
#define GWP_SLOT_BACKPACK 13u
#define GWP_SLOT_COUNT 14u

/* The state of an item in hands, item_state (CHUDState::EHudStates for every hud item; the states from
   GWP_ITEM_STATE_FIRE on are those of a weapon, CWeapon::EWeaponStates - other hud items such as missiles and
   detectors number their own states above GWP_ITEM_STATE_LAST_BASE differently). The numbers of this engine: the
   bolt throw and zoom states make them differ from the original game, where a firing weapon was 5 / 6. */
#define GWP_ITEM_STATE_HIDDEN 0u
#define GWP_ITEM_STATE_IDLE 1u
#define GWP_ITEM_STATE_SHOWING 2u
#define GWP_ITEM_STATE_HIDING 3u
#define GWP_ITEM_STATE_BORE 4u
#define GWP_ITEM_STATE_BOLT_THROW_START 5u
#define GWP_ITEM_STATE_BOLT_THROW_IDLE 6u
#define GWP_ITEM_STATE_BOLT_THROW_END 7u
#define GWP_ITEM_STATE_ZOOM_START 8u
#define GWP_ITEM_STATE_ZOOM_IDLE 9u
#define GWP_ITEM_STATE_ZOOM_END 10u
#define GWP_ITEM_STATE_LAST_BASE 11u
#define GWP_ITEM_STATE_FIRE 12u
#define GWP_ITEM_STATE_FIRE2 13u
#define GWP_ITEM_STATE_RELOAD 14u
#define GWP_ITEM_STATE_MISFIRE 15u
#define GWP_ITEM_STATE_MAG_EMPTY 16u
#define GWP_ITEM_STATE_SWITCH 17u
#define GWP_ITEM_STATE_UNMISFIRE 18u
#define GWP_ITEM_STATE_AIM_START 19u
#define GWP_ITEM_STATE_AIM_END 20u
#define GWP_ITEM_STATE_NONE 65535u /* not a hud item (what Lua get_state answers) */

/* item_animation_slot: the number is the line animation_slot of the section of the item (a config value, an open
   list: the slot of the stalker animations of that kind of weapon); GWP_ANIMATION_SLOT_NONE for an item without one
   or an object that is not a hud item. */
#define GWP_ANIMATION_SLOT_NONE 0xFFFFFFFFu

/* object_class_id / alife_object_class_id: a buffer of this size holds every class id with its terminating zero. */
#define GWP_CLASS_ID_SIZE 9u

/* The sound type mask of npc_on_hear_callback (argument 2; ESoundTypes of the engine, the snd_type of Lua hear
   callbacks): one bit of the source kind (WEAPON, ITEM, MONSTER, ANOMALY, WORLD) OR the bits that refine it. A
   compound type is the OR of both: a weapon bullet hit is GWP_SOUND_WEAPON | GWP_SOUND_BULLET_HIT. Lua names the
   mask through xr_hear.get_sound_type, a chain of checks in priority order (a mask with SHOOTING and BULLET_HIT is
   "WPN_shoot", not "WPN_hit"): a plugin that wants the Lua names repeats that order. */
#define GWP_SOUND_WEAPON 0x80000000u
#define GWP_SOUND_ITEM 0x40000000u
#define GWP_SOUND_MONSTER 0x20000000u
#define GWP_SOUND_ANOMALY 0x10000000u
#define GWP_SOUND_WORLD 0x08000000u
#define GWP_SOUND_PICKING_UP 0x04000000u
#define GWP_SOUND_DROPPING 0x02000000u
#define GWP_SOUND_HIDING 0x01000000u
#define GWP_SOUND_TAKING 0x00800000u
#define GWP_SOUND_USING 0x00400000u
#define GWP_SOUND_SHOOTING 0x00200000u
#define GWP_SOUND_EMPTY_CLICKING 0x00100000u
#define GWP_SOUND_BULLET_HIT 0x00080000u
#define GWP_SOUND_RECHARGING 0x00040000u
#define GWP_SOUND_DYING 0x00020000u
#define GWP_SOUND_INJURING 0x00010000u
#define GWP_SOUND_STEP 0x00008000u
#define GWP_SOUND_TALKING 0x00004000u
#define GWP_SOUND_ATTACKING 0x00002000u
#define GWP_SOUND_EATING 0x00001000u
#define GWP_SOUND_IDLE 0x00000800u
#define GWP_SOUND_OBJECT_BREAKING 0x00000400u
#define GWP_SOUND_OBJECT_COLLIDING 0x00000200u
#define GWP_SOUND_OBJECT_EXPLODING 0x00000100u
#define GWP_SOUND_AMBIENT 0x00000080u

/* Sight type (SightManager::ESightType; Lua CSightParams.eSightType*, and look.* where named). */
#define GWP_SIGHT_CURRENT_DIRECTION 0u    /* look.cur_dir */
#define GWP_SIGHT_PATH_DIRECTION 1u       /* look.path_dir */
#define GWP_SIGHT_DIRECTION 2u            /* look.direction */
#define GWP_SIGHT_POSITION 3u             /* look.point */
#define GWP_SIGHT_OBJECT 4u
#define GWP_SIGHT_COVER 5u                /* look.danger */
#define GWP_SIGHT_SEARCH 6u               /* look.search */
#define GWP_SIGHT_LOOK_OVER 7u
#define GWP_SIGHT_COVER_LOOK_OVER 8u
#define GWP_SIGHT_FIRE_OBJECT 9u
#define GWP_SIGHT_FIRE_POSITION 10u       /* look.fire_point */
#define GWP_SIGHT_ANIMATION_DIRECTION 11u
#define GWP_SIGHT_DUMMY 0xFFFFFFFFu       /* no sight (npc_sight_params of a non-stalker) */

/* What a stalker does with an item, npc_set_item (MonsterSpace::EObjectAction; Lua object.*). */
#define GWP_OBJECT_SWITCH1 0u
#define GWP_OBJECT_SWITCH2 1u
#define GWP_OBJECT_RELOAD1 2u
#define GWP_OBJECT_RELOAD2 3u
#define GWP_OBJECT_AIM1 4u
#define GWP_OBJECT_AIM2 5u
#define GWP_OBJECT_FIRE1 6u
#define GWP_OBJECT_FIRE2 8u
#define GWP_OBJECT_IDLE 9u
#define GWP_OBJECT_STRAP 10u
#define GWP_OBJECT_DROP 11u
#define GWP_OBJECT_ACTIVATE 16u
#define GWP_OBJECT_DEACTIVATE 17u
#define GWP_OBJECT_USE 18u
#define GWP_OBJECT_TURN_ON 19u
#define GWP_OBJECT_TURN_OFF 20u
#define GWP_OBJECT_SHOW 21u
#define GWP_OBJECT_HIDE 22u
#define GWP_OBJECT_TAKE 23u

/* npc_set_item: leave the queue argument out (the shorter Lua form). */
#define GWP_QUEUE_DEFAULT 0xFFFFFFFFu

/* Hit type, GwpHit::hit_type (ALife::EHitType; Lua hit.*). */
#define GWP_HIT_BURN 0u
#define GWP_HIT_SHOCK 1u
#define GWP_HIT_CHEMICAL_BURN 2u
#define GWP_HIT_RADIATION 3u
#define GWP_HIT_TELEPATIC 4u
#define GWP_HIT_WOUND 5u
#define GWP_HIT_FIRE_WOUND 6u
#define GWP_HIT_STRIKE 7u
#define GWP_HIT_EXPLOSION 8u
#define GWP_HIT_WOUND_2 9u      /* the alternative knife hit; not in the Lua table */
#define GWP_HIT_LIGHT_BURN 10u
#define GWP_HIT_PHYSIC_STRIKE 11u

/* A hit, object_hit: the fields of the Lua hit object. Zero-initialize, set size = sizeof(GwpHit): the engine reads
   min(size, its own sizeof) bytes, fields appended later default to 0; a size below GWP_HIT_MIN_SIZE (the struct of
   this first version) is refused. */
typedef struct GwpHit
{
    uint32_t size;       /* sizeof(GwpHit) of the plugin */
    uint32_t hit_type;   /* GWP_HIT_* */
    GwpObjectId who;     /* the online object that hits (the target itself for a self hit) */
    uint16_t reserved;   /* 0 */
    float power;         /* hit.power */
    float impulse;       /* hit.impulse */
    float direction[3];  /* hit.direction: finite, not zero */
    const char* bone;    /* hit:bone(name): NULL or "" for bone 0 */
} GwpHit;
#define GWP_HIT_MIN_SIZE ((uint32_t)(offsetof(GwpHit, bone) + sizeof(const char*)))

/* What a stalker looks at (npc_sight_params); the fields of Lua CSightParams. Set size = sizeof(GwpSightParams)
   before the call: the engine writes min(size, its own sizeof) bytes and puts that number into size. */
typedef struct GwpSightParams
{
    uint32_t size;       /* sizeof(GwpSightParams) of the plugin: fields may be appended in later versions */
    uint32_t sight_type; /* GWP_SIGHT_* */
    GwpObjectId object;  /* the object it looks at, GWP_INVALID_OBJECT_ID when none */
    uint16_t reserved;
    float vector[3];     /* the direction or the point of the sight type */
} GwpSightParams;

/* ------------------------------------------------------------------------- */
/* Worker threads (group threads)                                             */
/* ------------------------------------------------------------------------- */

/* One piece [begin, end) of a task_parallel_for range. Runs on a worker thread of the engine or on the calling
   thread, several pieces at once: the game must not be touched from here (only functions tagged @thread any). */
typedef void(GWP_CALL* GwpTaskRangeFn)(void* user, uint32_t begin, uint32_t end);

/* ------------------------------------------------------------------------- */
/* ALife and the world (groups alife, squads, world)                          */
/* ------------------------------------------------------------------------- */

/* What alife_objects lists (GwpAlifeFilter::flags). Kinds: an object passes when it is of ANY kind set (no kind bit
   = every kind). States: every state bit set must hold. An unknown bit is refused (the result is 0, logged once). */
#define GWP_ALIFE_STALKER 0x1u   /* a stalker (CSE_ALifeHumanAbstract) */
#define GWP_ALIFE_MONSTER 0x2u   /* a monster (a creature that is not a stalker and not the actor) */
#define GWP_ALIFE_ACTOR 0x4u     /* the actor */
#define GWP_ALIFE_ITEM 0x8u      /* an inventory item (on the ground or inside an inventory) */
#define GWP_ALIFE_SQUAD 0x10u    /* a squad (sim_squad_scripted, "ON_OFF_S") */
#define GWP_ALIFE_SMART 0x20u    /* a smart terrain (se_smart_terrain, "SMRTTRRN") */
#define GWP_ALIFE_ONLINE 0x100u  /* online now */
#define GWP_ALIFE_OFFLINE 0x200u /* offline now */
#define GWP_ALIFE_ALIVE 0x400u   /* a living creature (a creature only) */
#define GWP_ALIFE_DEAD 0x800u    /* a dead creature (a creature only) */
#define GWP_ALIFE_NO_PARENT 0x1000u /* lies in the world: not inside an inventory or a box */

/* Filter of alife_objects. Zero-initialize, then set size = sizeof(GwpAlifeFilter); NULL = every object. */
typedef struct GwpAlifeFilter
{
    uint32_t size;            /* sizeof(GwpAlifeFilter) of the plugin: fields may be appended in later versions */
    uint32_t flags;           /* GWP_ALIFE_* */
    int32_t level_id;         /* only objects on this level (the level of their game vertex); -1 = any level */
    uint32_t reserved;        /* 0 */
    const char* section_mask; /* the ltx section: '*' and '?' wildcards, case does not matter; NULL = any */
    const char* class_id;     /* the class id text of alife_object_class_id ("AI_STL_S"; trailing spaces do not
                                 matter); NULL = any */
} GwpAlifeFilter;

/* ------------------------------------------------------------------------- */
/* Characters, inventories, objects (groups character, items, object_ext)    */
/* ------------------------------------------------------------------------- */

/* A characteristic of a living creature: creature_condition, creature_set_condition, creature_change_condition. */
#define GWP_COND_HEALTH 0u     /* health, MIN_HEALTH (-0.01) .. GWP_COND_HEALTH_MAX; the creature dies at 0 */
#define GWP_COND_POWER 1u      /* stamina, 0 .. GWP_COND_POWER_MAX */
#define GWP_COND_RADIATION 2u  /* the radiation dose, 0 .. the maximum of the creature (1 by default) */
#define GWP_COND_BLEEDING 3u   /* the bleeding speed of all open wounds (only closing wounds is writable) */
#define GWP_COND_PSY_HEALTH 4u /* psy health, 0 .. 1 */
#define GWP_COND_MORALE 5u     /* morale, 0 .. 1 */
#define GWP_COND_SATIETY 6u    /* satiety, 0 .. 1: the actor only */
#define GWP_COND_ALCOHOL 7u    /* alcohol, 0 .. 1: the actor only */
#define GWP_COND_HEALTH_MAX 8u /* the upper bound of health, 0 < max <= 1; not saved: back to the ltx value at the
                                  next spawn or load */
#define GWP_COND_POWER_MAX 9u  /* the upper bound of stamina, 0.1 .. 1; not saved, as GWP_COND_HEALTH_MAX */
#define GWP_COND_COUNT 10u

/* How one character treats another (character_set_relation): ALife::ERelationType, the numbers creature_relation
   answers and game_object.friend / neutral / enemy of Lua. */
#define GWP_RELATION_FRIEND 0u
#define GWP_RELATION_NEUTRAL 1u
#define GWP_RELATION_ENEMY 2u
#define GWP_RELATION_WORST_ENEMY 3u /* only answered for monsters; character_set_relation refuses it */

/* Flags of item_install_upgrade. */
#define GWP_UPGRADE_NO_CHECKS 0x1u  /* item:add_upgrade of Lua: only "not installed yet"; no group order, no Lua
                                       preconditions, no unloading of the magazine, no effects */
#define GWP_UPGRADE_CHECK_ONLY 0x2u /* install nothing: GWP_OK when the upgrade could be installed now */

/* Sounds and particle effects of a plugin (group effects): opaque handles with a generation. 0 is never a valid
   handle; a handle of a sound or effect that is gone is refused by every call, also after its slot is reused. */
typedef uint32_t GwpSound;
typedef uint32_t GwpParticles;
#define GWP_INVALID_SOUND ((GwpSound)0u)
#define GWP_INVALID_PARTICLES ((GwpParticles)0u)
#define GWP_SOUND_MAX_PER_PLUGIN 128u     /* sounds of one plugin playing at once */
#define GWP_PARTICLES_MAX_PER_PLUGIN 128u /* particle effects of one plugin alive at once */

/* Flags of sound_play_at / sound_play_2d. */
#define GWP_SOUND_LOOPED 0x1u /* plays until sound_stop (or the end of the plugin, the game, the level) */

/* Flags of particles_stop. */
#define GWP_PARTICLES_STOP_DEFERRED 0x1u /* the emitters stop, the particles already out live to their end */

/* Ids of the pp and camera effectors of a plugin: 0..GWP_EFFECTOR_ID_MAX, each plugin has its own space. */
#define GWP_EFFECTOR_ID_MAX 0xFFFFu

/* ------------------------------------------------------------------------- */
/* Engine API table (engine -> plugin)                                        */
/* ------------------------------------------------------------------------- */

typedef struct GwpEngineApi
{
    /* Header. Must stay first and never change layout. */
    uint32_t abi_major; /* == GWP_API_VERSION_MAJOR of the engine */
    uint32_t size;      /* sizeof(GwpEngineApi) as compiled into the engine */
    uint32_t api_version;
    uint32_t reserved; /* 0; keeps the function pointers 8-byte aligned on every platform */

    /* --- v0.1 ------------------------------------------------------------ */
    /*
     * Every function carries tags for xray-16/src/gw_plugins/gen_plugin_api_index.py (wiki/doc/plugins/api/all.md):
     *   @group <name>   API group = wiki page (core, ...); the list of groups lives in the script
     *   @thread main|any  main: the game logic thread only (see is_main_thread); any: callable from any thread
     * A tag comment applies to the declarations right below it, up to the next empty line.
     * The plugins build fails when a function has no tags.
     */

    /* Writes to the engine log with the "[plugin:<addon id>]" tag. text is UTF-8. The log is thread-safe: any thread,
       task_parallel_for pieces included.
       @group core @thread any */
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

    /* Returns non-zero when called on the game logic thread: the thread that runs the GameThread task (object updates,
       Lua, ALife) right now. Usually the process main thread, but the task scheduler may run that task on a worker
       while the main thread waits for it. "Main thread" everywhere in this API (@thread main, GWP_ERROR_NOT_MAIN_THREAD,
       "called outside the main thread" in the log) means this game logic thread, not the process main thread.
       @group core @thread any */
    int(GWP_CALL* is_main_thread)(void);

    /* Returns non-zero when GWP_LOG_DEBUG messages are printed (game started with -addon_debug).
       Lets a plugin skip building debug text that would be dropped anyway.
       @group core @thread any */
    int(GWP_CALL* is_debug_log)(void);

    /* Id of the event with this name; registers the name when it is new. 0 (GWP_INVALID_EVENT_ID) only for an
       invalid name (NULL, empty, longer than 256 bytes) or a call outside the main thread.
       Ids never change while the game runs: resolve once, emit by id.
       @group events @thread main */
    GwpEventId(GWP_CALL* event_id)(const char* name);

    /* Subscribes handler to the event `name` (declared or not yet). user is passed back to the handler.
       The same handler may be subscribed several times: each call is a separate subscription.
       Subscriptions of a plugin are removed by the engine after its on_unload.
       A subscription made during a dispatch of the same event takes effect from the next dispatch.
       GWP_INVALID_SUBSCRIPTION_ID (logged) for self == NULL, handler == NULL, an invalid name or a call outside the
       main thread. event_unsubscribe ignores an id that is not a live subscription of this plugin.
       @group events @thread main */
    GwpSubscriptionId(GWP_CALL* event_subscribe)(const GwpPlugin* self, const char* name, GwpEventHandler handler,
        void* user);
    void(GWP_CALL* event_unsubscribe)(const GwpPlugin* self, GwpSubscriptionId subscription);

    /* Declares an event owned by this plugin (flags: GWP_EVENT_HAS_RESULT). Emitting an undeclared event works
       but prints a warning once. GWP_ERROR_INVALID_ARGUMENT for an invalid name or a flag bit other than
       GWP_EVENT_HAS_RESULT (a flag of a later version), GWP_ERROR_NOT_MAIN_THREAD outside the main thread.
       @group events @thread main */
    GwpResult(GWP_CALL* event_declare)(const GwpPlugin* self, const char* name, uint32_t flags);

    /* Emits the event to every Lua and native subscriber synchronously. result may be NULL; argv may be NULL
       when argc is 0. Nested emits from a handler are allowed. Every argument and the result are checked first
       (see GwpValue): GWP_ERROR_INVALID_ARGUMENT, and nobody gets the event, for id 0, argv == NULL with argc > 0,
       a value with reserved != 0, a type above GWP_T_ARRAY (a type of a later version), a string or bytes with
       ptr == NULL and len > 0 or longer than 1 MiB, an array with items == NULL and count > 0, more than 4096 items
       or more than 8 levels, more than 65536 values or 16 MiB of strings and bytes in one value. GWP_ERROR_NOT_FOUND
       for an id that event_id did
       not give in this run; GWP_ERROR_INVALID_STATE when nested emits go deeper than 32 (the dispatch is skipped and
       logged); GWP_ERROR_NOT_MAIN_THREAD outside the main thread. Native handlers of a plugin emit see
       GwpEvent::flags == 0 (GWP_EVENT_FLAG_ENGINE is the engine's). The engine copies the strings, bytes and arrays
       of the arguments before the dispatch: a handler that changes or frees what argv points to (e.g. a
       script_call of the emitting plugin, which replaces its last result) does not affect the next handlers.
       `result` is not copied: it is the caller's value, read and written in place by every handler until the emit
       returns. A string, bytes or an array a handler puts into it must stay valid after that handler returns (until
       the emitter has read it): the emitter copies what it needs before it calls anything else.
       @group events @thread main */
    GwpResult(GWP_CALL* event_emit)(const GwpPlugin* self, GwpEventId id, uint32_t argc, const GwpValue* argv,
        GwpValue* result);

    /* Online game objects by id. Every function returns 0 / NULL / GWP_INVALID_OBJECT_ID when there is no level
       or no such online object. Strings live while the object is online: copy them to keep. The class of an object
       is object_class_id (text, the same in every build).
       @group objects @thread main */
    int(GWP_CALL* object_exists)(GwpObjectId id);
    const char*(GWP_CALL* object_section)(GwpObjectId id);
    const char*(GWP_CALL* object_name)(GwpObjectId id);
    int(GWP_CALL* object_position)(GwpObjectId id, float out_xyz[3]);
    int(GWP_CALL* object_is_alive)(GwpObjectId id); /* 0 for objects that are not living entities */
    GwpObjectId(GWP_CALL* actor_id)(void);

    /* Plugin data stored inside the game save (a chunk per addon).
       save_write replaces the data of this addon; it goes to disk with the next save (usually called from the
       alife_on_before_save handler). size == 0 removes the data. The engine copies the bytes.
       save_read returns the data of this addon from the loaded save (or written since); GWP_ERROR_NOT_FOUND when
       there is none. The pointer is valid until the next save_write of this plugin, a load or a new game.
       gwp.hpp has helpers for the bytes: BinaryWriter / BinaryReader with tagged chunks (BinaryWriter::chunk,
       BinaryReader::next_chunk) that an older or a newer version of the plugin can skip.
       Data of addons that are not installed now is carried to new saves unchanged.
       @group save @thread main */
    GwpResult(GWP_CALL* save_write)(const GwpPlugin* self, uint32_t data_version, const void* data, uint32_t size);
    GwpResult(GWP_CALL* save_read)(const GwpPlugin* self, uint32_t* data_version, const void** data, uint32_t* size);

    /* Shared key-value store of Lua scripts and plugins (Lua: global table data_bus). Keys are "<owner>/<name>";
       a plugin may write only keys that start with "<its addon id>/" (GWP_ERROR_ACCESS_DENIED otherwise), anyone
       may read any key. A value is any GwpValue except GWP_T_LUA_REF; strings, bytes and arrays are copied deeply.
       Flags: GWP_DATA_PERSISTENT (another bit: GWP_ERROR_INVALID_ARGUMENT).
       A real change of a value sends the event data_on_changed(key, value); after data_erase value is nil.
       data_erase returns GWP_OK when the key was there and is gone now, GWP_ERROR_NOT_FOUND when there was none.
       @group data @thread main */
    GwpResult(GWP_CALL* data_set)(const GwpPlugin* self, const char* key, const GwpValue* value, uint32_t flags);
    GwpResult(GWP_CALL* data_erase)(const GwpPlugin* self, const char* key);

    /* Reads a value; GWP_ERROR_NOT_FOUND when there is no such key. A string, bytes or an array in `out` live until
       the key is changed or erased: copy them to keep.
       @group data @thread main */
    GwpResult(GWP_CALL* data_get)(const char* key, GwpValue* out);

    /* Timers of this addon (native analogue of Lua CreateTimeEvent, on game time; Lua: global table timer_bus).
       A timer has a name unique inside the addon; timer_start with an existing name replaces that timer. When the
       timer expires, the engine emits the event `event` (declared automatically; not a built-in engine event such as
       level_on_frame) with the timer name, its full key "<addon id>/<name>" and then the `argc` values of `argv`
       (copied at timer_start; argv may be NULL when argc is 0; at most 16; any type but GWP_T_LUA_REF, objects
       travel as their ids - a persistent timer keeps them in the save); subscribe to it with event_subscribe.
       Time base: game time in game seconds (scaled by the time factor), or with GWP_TIMER_REAL_TIME level time in
       real seconds; both stop on pause, in the main menu and during loading. delay >= 0; period > 0 repeats the
       timer every period seconds (at least 1 ms, at most once per frame), period == 0 fires it once.
       A new game, a load and a level change drop all timers; GWP_TIMER_PERSISTENT timers come back with the save.
       Timers of an addon whose plugin is not loaded wait and are carried to new saves.
       timer_start returns GWP_ERROR_INVALID_STATE outside a game (ALife not created or not loaded yet), for both
       time bases, GWP_ERROR_INVALID_ARGUMENT for a bad name, delay, period, flag or argument. timer_stop and
       timer_remaining return GWP_ERROR_NOT_FOUND when there is no such timer; timer_remaining gives the seconds left
       in the timer's own time base.
       @group timers @thread main */
    GwpResult(GWP_CALL* timer_start)(const GwpPlugin* self, const char* name, const char* event, double delay_seconds,
        double period_seconds, uint32_t flags, uint32_t argc, const GwpValue* argv);
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
       Refused with GWP_INVALID_SUBSCRIPTION_ID (and a log line) besides the cases of event_subscribe: options->size
       below the end of the field `object`, a flag bit other than GWP_SUBSCRIBE_BATCH | GWP_SUBSCRIBE_OBJECT (a flag
       of a later version), reserved != 0, reserved2 != 0 (when options->size covers it), GWP_SUBSCRIBE_BATCH
       without batch_handler, GWP_SUBSCRIBE_OBJECT with an
       object that is not online. Fields after `size` bytes are taken as 0 (a plugin of a later header may pass a
       longer struct: the engine reads the fields it knows).
       @group events @thread main */
    GwpSubscriptionId(GWP_CALL* event_subscribe_ex)(const GwpPlugin* self, const char* name,
        const GwpSubscribeOptions* options, GwpEventHandler handler, void* user);

    /* event_declare with an argument schema: one code per argument - b bool, I integer, N number, s string,
       v vector, o game object, O server object, t Lua table/userdata (GWP_T_LUA_REF or GWP_T_ARRAY), * anything
       (bytes too); '?' after a code = the argument may be nil (or missing at the end). "" = no arguments, NULL = no
       schema. With -addon_debug (console gw_event_schema_check) every emit is checked and a mismatch is logged
       once per event. The first schema declared for an event is kept; another one is logged and ignored.
       GWP_ERROR_INVALID_ARGUMENT for an invalid name, an invalid schema or a flag bit other than
       GWP_EVENT_HAS_RESULT; GWP_ERROR_NOT_MAIN_THREAD outside the main thread.
       event_schema returns the schema of the event (NULL when it has none, for an unknown id or outside the main
       thread); the string lives as long as the game.
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
       cvar_set_*: GWP_ERROR_NOT_FOUND for a variable nobody registered, GWP_ERROR_INVALID_ARGUMENT for a value
       outside the range of the registration or a variable of the other kind (cvar_set_int on a float one). The
       cvar_register_* and console_execute answer GWP_ERROR_INVALID_STATE while the console does not exist.
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
       level_objects writes up to `max` ids of the online objects into `out` and returns how many there are in all
       (out may be NULL with max 0 to count only; level_object_count is the same number); the ids are a snapshot,
       check them with object_exists before use.
       @group level @thread main */
    int(GWP_CALL* level_present)(void);
    const char*(GWP_CALL* level_name)(void);
    int(GWP_CALL* level_time_parts)(uint32_t out_parts[7]);
    uint32_t(GWP_CALL* level_object_count)(void);
    uint32_t(GWP_CALL* level_objects)(GwpObjectId* out, uint32_t max);

    /* Vertices of the AI graph of the level: the positions NPCs walk by. GWP_INVALID_LEVEL_VERTEX means "no such
       vertex"; level_vertex_at_position (level.vertex_id(pos) of Lua) gives it for a position outside the graph.
       Naming: a function answering the vertex of something ends in _level_vertex / _game_vertex.
       level_vertex_in_direction walks the graph from a vertex towards `dir` and returns the farthest vertex within
       `distance` (the vertex itself when it cannot move).
       @group level @thread main */
    uint32_t(GWP_CALL* level_vertex_at_position)(const float xyz[3]);
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
       offline there is nobody to run them for (GWP_ERROR_NOT_FOUND then; GWP_ERROR_INVALID_ARGUMENT for an online
       object that is not an inventory owner). A monster of this engine is an inventory owner too (CBaseMonster), so
       it takes portions, as in Lua.
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

    /* Binders of game objects (wiki/doc/plugins/api/binders.md). binder_register attaches the callbacks of
       `vtable` to every object whose class id equals class_id (up to 8 characters, "AI_STL_S": the text the
       engine registers the class under, see object_class_id) and whose section matches section_mask ('*' and '?'
       wildcards, "wpn_*", case does not matter). NULL means "any" for either; one of the two is required.
       Objects already online get on_spawn right away. The vtable is copied, `user` comes back to every callback.
       GWP_INVALID_BINDER_ID (the reason is logged): self or vtable NULL, vtable->size below
       GWP_BINDER_VTABLE_MIN_SIZE, vtable->reserved not 0, a bad class id or mask, too many binders (64), another
       binder of this plugin with the same class id and mask that also has on_save / on_load (the saved state is
       kept under that pair), the plugin already crashed in a binder callback, a call from on_save while the game
       is being saved.
       binder_unregister sends on_save and on_destroy for the bound objects, then forgets the binder; the engine
       forgets the binders silently (no callbacks; the last known saved state of the bound objects is kept) when
       the plugin is unloaded. binder_unregister returns GWP_ERROR_INVALID_ARGUMENT for self NULL or
       GWP_INVALID_BINDER_ID, GWP_ERROR_NOT_FOUND for an unknown binder, GWP_ERROR_ACCESS_DENIED for a binder of
       another plugin, GWP_ERROR_INVALID_STATE for a call from on_save while the game is being saved (nothing
       changes), GWP_ERROR_CRASHED when the plugin crashed in one of those callbacks (the binder is gone all the
       same, with the other binders of the plugin).
       @group binders @thread main */
    GwpBinderId(GWP_CALL* binder_register)(const GwpPlugin* self, const char* class_id, const char* section_mask,
        const GwpBinderVTable* vtable, void* user);
    GwpResult(GWP_CALL* binder_unregister)(const GwpPlugin* self, GwpBinderId binder);

    /* How many online objects the binder is attached to now; 0 for an unknown binder.
       @group binders @thread main */
    uint32_t(GWP_CALL* binder_object_count)(GwpBinderId binder);

    /* Class id of an online object as text: up to 8 characters without the padding spaces ("AI_STL_S", "SM_DOG_S",
       "AI_RAT"), the form binder_register takes; the text is the same in every build and with every set of mods.
       Copied into `out` (zero-terminated, cut to cap - 1; GWP_CLASS_ID_SIZE holds any), the length of the full text
       as the result; 0 (and "" in out) when there is no such online object. out may be NULL to ask the length.
       @group objects @thread main */
    uint32_t(GWP_CALL* object_class_id)(GwpObjectId id, char* out, uint32_t cap);

    /* Entry of db.storage of an online object (wiki/doc/plugins/api/storage.md). storage_handle is 0 while the
       object has no entry (before its Lua binder reinit, or an object without a binder); storage_valid is 0 once
       the entry is gone (the object went offline). The mirrored fields are named by index: resolve the name once
       with storage_key_index ("active_section", "meet.gtfo"; -1 for a name outside the list), the list itself is
       storage_key_count / storage_key_name.
       @group storage @thread main */
    GwpStorageHandle(GWP_CALL* storage_handle)(GwpObjectId id);
    int(GWP_CALL* storage_valid)(GwpStorageHandle handle);
    int32_t(GWP_CALL* storage_key_index)(const char* key);
    uint32_t(GWP_CALL* storage_key_count)(void);
    const char*(GWP_CALL* storage_key_name)(uint32_t key);

    /* Reads a mirrored field: 0 for a stale handle or a bad key, else 1 with the value (GWP_T_NIL when the field
       is not set, GWP_T_OBJECT for a game object, GWP_T_LUA_REF for a table, a vector or a server object). A string is valid until the next write of
       that field or the end of the entry: copy to keep. storage_set writes the field on both sides (nil erases it;
       bool, int, number, string and bytes only - bytes are stored as a Lua string and read back as GWP_T_STRING);
       the subtable of a "sub.field" key must exist on the Lua side already.
       Plugins write only the fields they own; the scripts do not expect their own fields to change under them.
       storage_set returns GWP_ERROR_INVALID_ARGUMENT for self or value NULL, a key outside the list, value->reserved
       not 0 or a type it cannot write; GWP_ERROR_NOT_SUPPORTED for a value type of a later version;
       GWP_ERROR_NOT_FOUND for a stale handle or GWP_INVALID_STORAGE_HANDLE (the entry is gone);
       GWP_ERROR_INVALID_STATE when the Lua side cannot take the write now (no script engine, no "sub" table of a
       "sub.field" key yet); GWP_ERROR_NOT_SUPPORTED when that table has a metatable of its own (not written).
       @group storage @thread main */
    int(GWP_CALL* storage_get)(GwpStorageHandle handle, uint32_t key, GwpValue* out);
    GwpResult(GWP_CALL* storage_set)(const GwpPlugin* self, GwpStorageHandle handle, uint32_t key,
        const GwpValue* value);

    /* Native voices in two decisions of the engine (wiki/doc/plugins/api/callbacks.md). They are asked for every
       NPC (stalkers and monsters); filter by id on the plugin side. The enemy decision is cached by the engine per
       NPC and enemy for 250 ms: after a plugin changes its mind, npc_enemy_filter_reset drops the cache of `npc`
       (GWP_INVALID_OBJECT_ID: of every online NPC; self is the plugin that changed its mind). A crash in a callback removes every callback of the plugin
       and the engine behaves as without it.
       The register functions return GWP_INVALID_CALLBACK_ID for self or fn NULL (logged). callback_unregister:
       GWP_ERROR_INVALID_ARGUMENT for self NULL or GWP_INVALID_CALLBACK_ID, GWP_ERROR_NOT_FOUND for an unknown
       callback, GWP_ERROR_ACCESS_DENIED for one of another plugin. npc_enemy_filter_reset:
       GWP_ERROR_INVALID_ARGUMENT for self NULL or an online object that is not an NPC, GWP_ERROR_NOT_FOUND when no
       object with the id is online, GWP_ERROR_INVALID_STATE without a level.
       @group callbacks @thread main */
    GwpCallbackId(GWP_CALL* enemy_filter_register)(const GwpPlugin* self, GwpEnemyFilterFn fn, void* user);
    GwpCallbackId(GWP_CALL* patrol_extrapolate_register)(const GwpPlugin* self, GwpPatrolExtrapolateFn fn,
        void* user);
    GwpResult(GWP_CALL* callback_unregister)(const GwpPlugin* self, GwpCallbackId callback);
    GwpResult(GWP_CALL* npc_enemy_filter_reset)(const GwpPlugin* self, GwpObjectId npc);

    /* Native evaluator in the planner of every stalker (wiki/doc/plugins/api/goap.md). The Lua evaluator that
       holds `property_id` in `planner` is kept and wrapped: `mode` (GWP_EVALUATOR_*) says who decides, and
       evaluator_set_mode switches it at any time, for every NPC at once. lua_name: the name the Lua evaluator was
       created with ("eva_npc_vs_heli"); an NPC whose property holds another evaluator (a constant for the dead,
       another scheme) is left as it is. NULL accepts any Lua evaluator. Applied to the online stalkers and to every
       stalker that spawns later, at a point of the frame where no planner runs. evaluator_unregister gives the
       property back to the Lua evaluator; unloading the plugin does the same.
       GWP_INVALID_EVALUATOR_ID: a bad argument, a flag bit in `mode` (GWP_MODE_FLAGS_MASK) or the same property
       registered already (by any plugin); evaluator_set_mode answers GWP_ERROR_NOT_SUPPORTED for a flag bit or a
       mode above GWP_EVALUATOR_SHADOW. evaluator_unregister and evaluator_set_mode answer GWP_ERROR_NOT_FOUND for an
       id that is not a live registration of this plugin.
       @group goap @thread main */
    GwpEvaluatorId(GWP_CALL* evaluator_register)(const GwpPlugin* self, uint32_t planner, uint32_t property_id,
        const char* lua_name, GwpEvaluatorFn fn, void* user, uint32_t mode);
    GwpResult(GWP_CALL* evaluator_unregister)(const GwpPlugin* self, GwpEvaluatorId evaluator);
    GwpResult(GWP_CALL* evaluator_set_mode)(const GwpPlugin* self, GwpEvaluatorId evaluator, uint32_t mode);
    int(GWP_CALL* evaluator_stats)(GwpEvaluatorId evaluator, GwpEvaluatorStats* out);

    /* What the NPC knows (wiki/doc/plugins/api/npc.md): the same values as the Lua methods best_enemy,
       best_danger, memory_time, memory_position, character_community. 0 / GWP_INVALID_OBJECT_ID / NULL when the
       object is not an online NPC of the right kind or does not know the other one.
       character_community: the community of an inventory owner (a stalker, the actor, a trader, and a monster: in
       this engine CBaseMonster is an inventory owner with the community "monster", as Lua character_community
       answers); NULL for anything else. The string lives while the object is online.
       Naming of the API: object_* takes any online object, creature_* a living entity (the actor, a stalker, a
       monster), character_* an inventory owner with a character profile (monsters included, see above),
       inventory_* the inventory of one, item_*
       an inventory item, npc_* a stalker or a monster (stalker only where the comment says so), alife_* a server
       object online or not.
       @group npc @thread main */
    GwpObjectId(GWP_CALL* npc_best_enemy)(GwpObjectId npc);
    int(GWP_CALL* npc_best_danger)(GwpObjectId npc, GwpDanger* out);
    uint32_t(GWP_CALL* npc_memory_time)(GwpObjectId npc, GwpObjectId other);
    int(GWP_CALL* npc_memory_position)(GwpObjectId npc, GwpObjectId other, float out_xyz[3]);
    const char*(GWP_CALL* character_community)(GwpObjectId id);

    /* Calls a Lua function by name: "module.function" or a global "function" (wiki/doc/plugins/api/script.md).
       Arguments and the result are GwpValue as for events: a GWP_T_OBJECT argument arrives in Lua as the game
       object, a game object result comes back as GWP_T_OBJECT; an array argument arrives as a new table, bytes as a
       Lua string, and a result table with the keys 1..n comes back as GWP_T_ARRAY (another table: GWP_T_LUA_REF).
       `result` may be NULL; a string, bytes or an array result lives until the next script_call of the same plugin.
       GWP_ERROR_NOT_FOUND when there is no such function, GWP_ERROR_CRASHED when it raised an error (logged, with
       the message), GWP_ERROR_INVALID_ARGUMENT for an invalid argument value (see event_emit) or more than 64
       arguments, GWP_ERROR_INVALID_STATE without a Lua state or for calls nested deeper than 16 (refused).
       About a microsecond per call plus the Lua function:
       for the rare, stateful parts of the scripts, not for every frame of every NPC.
       @group script @thread main */
    GwpResult(GWP_CALL* script_call)(const GwpPlugin* self, const char* function, uint32_t argc,
        const GwpValue* argv, GwpValue* result);

    /* time_global() of Lua: real milliseconds of the level, stopped on pause. The time base of npc_memory_time,
       GwpDanger::time and of the TTL caches of the scripts. 0 without a level.
       @group level @thread main */
    uint32_t(GWP_CALL* level_time_ms)(void);

    /* Zones of the plugin in the Tracy profiler, next to the zones of the engine (wiki/doc/plugins/api/console.md).
       profile_zone_site registers a place once (from gwp::Zone::site or GWP_ZONE, a static per place): name is what
       Tracy shows, file and line where it is, color 0xRRGGBB or 0 for the default; the addon id stands for the
       function name. The strings are copied and kept for the whole run: register a fixed set of places, not one
       per call. A zone is begun and ended on the same thread, ends in reverse order of begins, and may carry a
       text (an object id, a count), copied by Tracy.
       @group console @thread main */
    GwpZoneSite(GWP_CALL* profile_zone_site)(const GwpPlugin* self, const char* name, const char* file,
        uint32_t line, uint32_t color);

    /* @group console @thread any */
    GwpZone(GWP_CALL* profile_zone_begin)(GwpZoneSite site);
    void(GWP_CALL* profile_zone_text)(GwpZone zone, const char* text, uint32_t length);
    void(GWP_CALL* profile_zone_end)(GwpZone zone);

    /* More of what the NPC knows (wave W2-1): the item in the hands of an inventory owner (npc:active_item(): a
       stalker, the actor, a monster - an inventory owner in this engine; GWP_INVALID_OBJECT_ID for anything else)
       and the objects an NPC touches now
       (npc:iterate_feel_touch: what is close enough to use, open, pick up). npc_feel_touch writes up to `max` ids
       and returns how many there are in all (out may be NULL with max 0 to count only).
       @group npc @thread main */
    GwpObjectId(GWP_CALL* inventory_active_item)(GwpObjectId owner);
    uint32_t(GWP_CALL* npc_feel_touch)(GwpObjectId npc, GwpObjectId* out, uint32_t max);

    /* The visual (model) of an online object, as obj:get_visual_name() gives it ("dynamics\box\box_wood_01");
       valid while the object is online. NULL without such an object.
       @group objects @thread main */
    const char*(GWP_CALL* object_visual)(GwpObjectId id);

    /* The server (ALife) side of an object, online or not (wiki/doc/plugins/api/alife.md): the squad it belongs to
       (se_obj.group_id; GWP_INVALID_OBJECT_ID for none) and the commander of a squad (squad:commander_id();
       GWP_INVALID_OBJECT_ID for an empty squad or an id that is not a squad).
       @group alife @thread main */
    GwpObjectId(GWP_CALL* alife_object_squad)(GwpObjectId id);
    GwpObjectId(GWP_CALL* alife_squad_commander)(GwpObjectId squad);

    /* The wound checks of the engine behind IsWounded: npc:critically_wounded() (a monster or a stalker taking a
       critical hit: it falls for a moment) and npc:in_smart_cover() (a stalker in a smart cover). 0 for anything
       else, without the script error Lua logs.
       @group npc @thread main */
    int(GWP_CALL* npc_critically_wounded)(GwpObjectId npc);
    int(GWP_CALL* npc_in_smart_cover)(GwpObjectId npc);

    /* A planner of the plugin for one online object (wiki/doc/plugins/api/goap.md): the engine planner Lua creates
       with action_planner(), with native evaluators and actions, so a Lua planner moved to C++ keeps the same graph
       and the same decisions. Build it (evaluators first: every condition and effect of an action must name one),
       set the goal, call planner_update where Lua called planner:update(). It is destroyed with the object
       (net_Destroy), with the plugin, or by planner_destroy; a crash in one of its callbacks stops every planner of
       the plugin. The calls that take a planner answer GWP_ERROR_NOT_FOUND for an id that is not a live planner of
       this plugin and GWP_ERROR_INVALID_STATE from a callback of the same planner (planner_update and
       planner_destroy included); GWP_ERROR_INVALID_ARGUMENT for a bad argument (a non-zero GwpActionVTable::reserved
       too); planner_update answers GWP_ERROR_CRASHED for a planner stopped by a crash of the plugin.
       @group goap @thread main */
    GwpPlannerId(GWP_CALL* planner_create)(const GwpPlugin* self, GwpObjectId npc);
    GwpResult(GWP_CALL* planner_destroy)(const GwpPlugin* self, GwpPlannerId planner);
    GwpResult(GWP_CALL* planner_add_evaluator)(const GwpPlugin* self, GwpPlannerId planner, uint32_t property,
        const char* name, GwpEvaluatorFn fn, void* user);
    GwpResult(GWP_CALL* planner_add_action)(const GwpPlugin* self, GwpPlannerId planner, uint32_t action_id,
        const char* name, const GwpActionVTable* vtable, void* user, const GwpWorldProperty* conditions,
        uint32_t condition_count, const GwpWorldProperty* effects, uint32_t effect_count);
    GwpResult(GWP_CALL* planner_set_goal)(const GwpPlugin* self, GwpPlannerId planner, const GwpWorldProperty* goal,
        uint32_t count);
    GwpResult(GWP_CALL* planner_update)(const GwpPlugin* self, GwpPlannerId planner);
    /* The action running now (GWP_INVALID_ACTION_ID before the first update or without a solution) and the value
       of a property (-1 for an unknown property): planner:current_action_id() and planner:evaluator(id):evaluate()
       of Lua - the evaluator is called again, a native one calls the plugin function.
       @group goap @thread main */
    uint32_t(GWP_CALL* planner_current_action)(GwpPlannerId planner);
    int(GWP_CALL* planner_evaluate)(GwpPlannerId planner, uint32_t property);

    /* A function of the plugin that Lua calls as plugins.call("<addon id>.<name>", ...) and gets the result of
       (wiki/doc/plugins/api/script.md). name: 1-64 characters a-z 0-9 _; exporting a name again replaces it.
       plugins.has(name) tells Lua whether it exists. A crash in the function removes every export of the plugin.
       @group script @thread main */
    GwpResult(GWP_CALL* script_export)(const GwpPlugin* self, const char* name, GwpExportFn fn, void* user);

    /* npc:see(other): the NPC sees the object now (its visual memory). 0 for a dead NPC, without the script error
       Lua logs, and for an object that is not online.
       @group npc @thread main */
    int(GWP_CALL* npc_see)(GwpObjectId npc, GwpObjectId other);


    /* Movement of a stalker, the Lua methods set_movement_type, set_body_state, set_mental_state, set_path_type
       (wiki/doc/plugins/api/npc_body.md), and what the NPC moves with now (movement_type) or is switching to
       (target_movement_type, target_body_state, target_mental_state: the state manager compares its state with
       these). Values: GWP_MOVEMENT_*, GWP_BODY_*, GWP_MENTAL_*, GWP_PATH_* (the numbers of move.*, anim.* and
       game_object.*_path in Lua). A setter returns GWP_ERROR_INVALID_ARGUMENT for an online object that is not a
       stalker or a value outside its list (GWP_ERROR_NOT_FOUND when nothing with the id is online); a getter returns what Lua gets then (GWP_MOVEMENT_STAND, GWP_BODY_STAND,
       GWP_MENTAL_DANGER), without the script error Lua logs.
       Every function of the API that changes the game takes the plugin handle first (`self`: the log of a refused
       call names the plugin). The codes of a refused change: GWP_ERROR_NOT_MAIN_THREAD; GWP_ERROR_INVALID_ARGUMENT
       for self == NULL, a value outside its list or an object of another class; GWP_ERROR_NOT_FOUND when no object
       with the id is online; GWP_ERROR_INVALID_STATE when the call is not possible now (no level loaded).
       @group npc_body @thread main */
    GwpResult(GWP_CALL* npc_set_movement_type)(const GwpPlugin* self, GwpObjectId npc, uint32_t movement_type);
    GwpResult(GWP_CALL* npc_set_body_state)(const GwpPlugin* self, GwpObjectId npc, uint32_t body_state);
    GwpResult(GWP_CALL* npc_set_mental_state)(const GwpPlugin* self, GwpObjectId npc, uint32_t mental_state);
    GwpResult(GWP_CALL* npc_set_path_type)(const GwpPlugin* self, GwpObjectId npc, uint32_t path_type);
    uint32_t(GWP_CALL* npc_movement_type)(GwpObjectId npc);
    uint32_t(GWP_CALL* npc_target_movement_type)(GwpObjectId npc);
    uint32_t(GWP_CALL* npc_target_body_state)(GwpObjectId npc);
    uint32_t(GWP_CALL* npc_target_mental_state)(GwpObjectId npc);

    /* Any NPC (a stalker or a monster): npc:is_body_turning() - the body (for a stalker also the head) has not
       reached its target direction yet; npc:movement_enabled() and movement_enabled(enable) - the NPC may walk at
       all. 0 / GWP_ERROR_INVALID_ARGUMENT for anything but an online NPC (GWP_ERROR_NOT_FOUND when nothing with the
       id is online).
       @group npc_body @thread main */
    int(GWP_CALL* npc_is_body_turning)(GwpObjectId npc);
    int(GWP_CALL* npc_movement_enabled)(GwpObjectId npc);
    GwpResult(GWP_CALL* npc_enable_movement)(const GwpPlugin* self, GwpObjectId npc, int enable);

    /* A stalker only: npc:special_danger_move() - the danger variant of the walk animations (the state manager sets
       it for the states that have special_danger_move), and npc:inactualize_patrol_path() - the patrol path is
       rebuilt on the next update (the state manager calls it before switching to a level path when it goes idle).
       0 / GWP_ERROR_INVALID_ARGUMENT for anything but an online stalker (GWP_ERROR_NOT_FOUND when nothing with the
       id is online).
       @group npc_body @thread main */
    int(GWP_CALL* npc_special_danger_move)(GwpObjectId npc);
    GwpResult(GWP_CALL* npc_set_special_danger_move)(const GwpPlugin* self, GwpObjectId npc, int value);
    GwpResult(GWP_CALL* npc_inactualize_patrol_path)(const GwpPlugin* self, GwpObjectId npc);

    /* object_level_vertex: obj:level_vertex_id(), the AI graph vertex the engine keeps for the object (not
       necessarily the vertex under its position: level_vertex_at_position(position) gives that one).
       GWP_INVALID_LEVEL_VERTEX without such an online object.
       object_hit: obj:hit(h) - the hit `hit` (GwpHit: type, power, impulse, who, direction, bone) on the online
       object `id`. Sent as the GE_HIT network event, like the Lua call. GWP_ERROR_NOT_FOUND when `id` or hit->who is
       not online; GWP_ERROR_INVALID_ARGUMENT for a NULL hit, a size below GWP_HIT_MIN_SIZE, a non-zero
       reserved, an unknown type, a zero or non-finite direction, a non-finite power or impulse, or a bone name on
       an object without a skeleton.
       @group objects @thread main */
    uint32_t(GWP_CALL* object_level_vertex)(GwpObjectId id);
    GwpResult(GWP_CALL* object_hit)(const GwpPlugin* self, GwpObjectId id, const GwpHit* hit);

    /* Where a stalker looks (npc:set_sight in the forms the state manager uses; types GWP_SIGHT_*, the numbers of
       look.* and CSightParams.* in Lua):
        - npc_set_sight_type: set_sight(type, torso_look, path), a type without a point, e.g.
          (GWP_SIGHT_ANIMATION_DIRECTION, 0, 0): the animation turns the body;
        - npc_set_sight_position: set_sight(type, point, 0), a type with an optional point (NULL = nil in Lua), e.g.
          (GWP_SIGHT_PATH_DIRECTION, NULL); GWP_SIGHT_FIRE_POSITION turns into GWP_SIGHT_POSITION with torso look;
        - npc_set_sight_direction: set_sight(type, dir, torso_look), e.g. GWP_SIGHT_DIRECTION with a unit vector: a
          longer one is normalized (a debug build asserts, as for Lua), a zero or non-finite one is refused;
        - npc_set_sight_object: set_sight(object, torso_look, fire_object, no_pitch): look at the online object
          `target` (fire_object: aim at it; no_pitch: keep the head level).
       GWP_ERROR_INVALID_ARGUMENT for an online object that is not a stalker or an unknown type, GWP_ERROR_NOT_FOUND
       when the NPC or the target is not online;
       GWP_SIGHT_OBJECT and GWP_SIGHT_FIRE_OBJECT are refused in the forms without an object (npc_set_sight_object
       is the call for them), GWP_SIGHT_LOOK_OVER everywhere (the engine has no sight action for it) and
       GWP_SIGHT_FIRE_POSITION everywhere but npc_set_sight_position; a vector with NaN or infinity is refused.
       @group npc_sight @thread main */
    GwpResult(GWP_CALL* npc_set_sight_type)(const GwpPlugin* self, GwpObjectId npc, uint32_t sight_type,
        int torso_look, int path);
    GwpResult(GWP_CALL* npc_set_sight_position)(const GwpPlugin* self, GwpObjectId npc, uint32_t sight_type,
        const float* point_xyz);
    GwpResult(GWP_CALL* npc_set_sight_direction)(const GwpPlugin* self, GwpObjectId npc, uint32_t sight_type,
        const float dir[3], int torso_look);
    GwpResult(GWP_CALL* npc_set_sight_object)(const GwpPlugin* self, GwpObjectId npc, GwpObjectId target,
        int torso_look, int fire_object, int no_pitch);

    /* What the stalker looks at now (npc:sight_params()): 1 with `out` filled; 0 for anything but an online stalker,
       `out` then holds what Lua gets (GWP_SIGHT_DUMMY, no object, the vector at FLT_MAX).
       @group npc_sight @thread main */
    int(GWP_CALL* npc_sight_params)(GwpObjectId npc, GwpSightParams* out);

    /* Script animations of a stalker: the queue npc:add_animation fills and the state manager plays. `name` is a
       motion of the visual; hand_usage as in Lua (the state manager always passes 1); use_movement_controller: the
       root motion of the animation moves the NPC. npc_add_animation_at is the other Lua form: the animation starts
       at `position` with `rotation` (degrees x/y/z; the state manager passes (0, yaw, 0)), local_animation as in Lua.
       GWP_ERROR_NOT_FOUND when the visual has no such motion, GWP_ERROR_INVALID_STATE when a global animation
       selector is set (Lua logs a script error in both cases and queues nothing); inside a smart cover the engine
       logs an error but queues it, as for Lua. npc_clear_animations
       empties the queue; npc_animation_count is its length, -1 for anything but an online stalker.
       The end of every script animation is the event npc_on_script_animation_end(npc, remaining): `remaining` is
       the queue length after the Lua callback.script_animation of the NPC (if any) has run. While that event has a
       subscriber the engine updates the animation of every stalker with a non-empty queue, visible or not, as it
       does for an NPC with a Lua script_animation callback (a GWP_SUBSCRIBE_OBJECT subscription narrows only the
       delivery, not that update).
       GWP_ERROR_INVALID_ARGUMENT for an online object that is not a stalker or a NULL name, GWP_ERROR_NOT_FOUND
       when nothing with the id is online.
       @group npc_anim @thread main */
    GwpResult(GWP_CALL* npc_add_animation)(const GwpPlugin* self, GwpObjectId npc, const char* name, int hand_usage,
        int use_movement_controller);
    GwpResult(GWP_CALL* npc_add_animation_at)(const GwpPlugin* self, GwpObjectId npc, const char* name,
        int hand_usage, const float position[3], const float rotation[3], int local_animation);
    GwpResult(GWP_CALL* npc_clear_animations)(const GwpPlugin* self, GwpObjectId npc);
    int32_t(GWP_CALL* npc_animation_count)(GwpObjectId npc);

    /* Weapons of a stalker as the state manager sees them: npc:best_weapon() (what it would fight with),
       inventory_item_in_slot - obj:item_in_slot(slot) of any inventory owner (the actor too; `slot` is GWP_SLOT_*
       or a slot_persistent_N slot of system.ltx above them, 1 .. the last slot of the inventory, the engine numbers
       without the Lua compatibility shift), weapon_strapped() /
       weapon_unstrapped() (on the back / in the hands, the animation finished) and
       is_weapon_going_to_be_strapped(item).
       GWP_INVALID_OBJECT_ID / 0 when there is no such item, the slot is out of the list or the object is not of the
       right kind.
       @group inventory @thread main */
    GwpObjectId(GWP_CALL* npc_best_weapon)(GwpObjectId npc);
    GwpObjectId(GWP_CALL* inventory_item_in_slot)(GwpObjectId owner, uint32_t slot);
    int(GWP_CALL* npc_weapon_strapped)(GwpObjectId npc);
    int(GWP_CALL* npc_weapon_unstrapped)(GwpObjectId npc);
    int(GWP_CALL* npc_is_weapon_going_to_be_strapped)(GwpObjectId npc, GwpObjectId item);

    /* What a stalker does with an item (npc:set_item): `action` is GWP_OBJECT_* (object.* in Lua), `item` an online
       object or GWP_INVALID_OBJECT_ID (nil in Lua: set_item(object.idle, nil) takes nothing in the hands).
       queue_size and queue_interval (ms) shape the fire queue of GWP_OBJECT_FIRE1; GWP_QUEUE_DEFAULT leaves the
       argument out, as the shorter Lua forms do: (action, item), (action, item, size), (action, item, size,
       interval); an interval without a size is refused. The state manager fires a sniper shot with
       (GWP_OBJECT_FIRE1, weapon, 1, aim ms). GWP_ERROR_INVALID_ARGUMENT where the object handler of the engine
       would fail (Lua crashes there): RELOAD1/2, TURN_ON/OFF, SHOW, HIDE and TAKE (no case for them); an item
       that is not in the inventory of the NPC; an item other than a weapon or a missile (DEACTIVATE excepted);
       USE with a weapon; a missile with anything but FIRE1, IDLE, DROP, ACTIVATE, DEACTIVATE.
       npc_aim_time / npc_set_aim_time: npc:aim_time(weapon[, ms]), the aiming time of `weapon` in ms; 0xFFFFFFFF
       (as Lua) / GWP_ERROR_INVALID_ARGUMENT when npc is not an online stalker or `weapon` is not a weapon of the
       NPC (in its inventory).
       @group inventory @thread main */
    GwpResult(GWP_CALL* npc_set_item)(const GwpPlugin* self, GwpObjectId npc, uint32_t action, GwpObjectId item,
        uint32_t queue_size, uint32_t queue_interval);
    uint32_t(GWP_CALL* npc_aim_time)(GwpObjectId npc, GwpObjectId weapon);
    GwpResult(GWP_CALL* npc_set_aim_time)(const GwpPlugin* self, GwpObjectId npc, GwpObjectId weapon,
        uint32_t aim_time_ms);

    /* Smart covers of a stalker (the smartcover state of the state manager): use_smart_covers_only (the NPC takes
       cover only in smart covers), the destination cover and loophole by name (NULL or "" clears, as the Lua call
       without arguments), the fire target inside the cover (a point, NULL clears; or an online object) and
       npc_clear_smart_cover_target_selector (set_smart_cover_target_selector() without a function: the default
       target selection). npc_dest_smart_cover_name: the destination cover (get_dest_smart_cover_name), NULL or ""
       when none; the string lives until the destination changes. Is the NPC in a cover now: npc_in_smart_cover
       (group npc). 0 / NULL / GWP_ERROR_INVALID_ARGUMENT for anything but an online stalker (GWP_ERROR_NOT_FOUND
       when nothing with the id is online). npc_set_dest_smart_cover: GWP_ERROR_NOT_FOUND for a cover the level has
       not; npc_set_dest_loophole: GWP_ERROR_INVALID_STATE without a destination cover, GWP_ERROR_NOT_FOUND for a
       loophole that cover has not.
       @group smart_cover @thread main */
    int(GWP_CALL* npc_use_smart_covers_only)(GwpObjectId npc);
    GwpResult(GWP_CALL* npc_set_use_smart_covers_only)(const GwpPlugin* self, GwpObjectId npc, int value);
    GwpResult(GWP_CALL* npc_set_dest_smart_cover)(const GwpPlugin* self, GwpObjectId npc, const char* cover_name);
    const char*(GWP_CALL* npc_dest_smart_cover_name)(GwpObjectId npc);
    GwpResult(GWP_CALL* npc_set_dest_loophole)(const GwpPlugin* self, GwpObjectId npc, const char* loophole_name);
    GwpResult(GWP_CALL* npc_set_smart_cover_target)(const GwpPlugin* self, GwpObjectId npc, const float* point_xyz);
    GwpResult(GWP_CALL* npc_set_smart_cover_target_object)(const GwpPlugin* self, GwpObjectId npc,
        GwpObjectId target);
    GwpResult(GWP_CALL* npc_clear_smart_cover_target_selector)(const GwpPlugin* self, GwpObjectId npc);

    /* An item in hands: item:get_state() (GWP_ITEM_STATE_*: the state of the weapon or of another hud item -
       firing, reloading, ...; GWP_ITEM_STATE_NONE for anything else, as in Lua) and item:animation_slot() (the
       animation slot of a hud item, an open list from its config; GWP_ANIMATION_SLOT_NONE for anything else,
       without the script error Lua logs).
       @group inventory @thread main */
    uint32_t(GWP_CALL* item_state)(GwpObjectId item);
    uint32_t(GWP_CALL* item_animation_slot)(GwpObjectId item);

    /* A Lua callback.script_animation is set on the NPC now (npc:set_callback(callback.script_animation, f)): the
       Lua animation object of some script owns the end of the script animations of this NPC. A native automaton
       that shares the NPC with Lua scripts leaves npc_on_script_animation_end to that owner while this is 1.
       @group npc_anim @thread main */
    int(GWP_CALL* npc_has_script_animation_callback)(GwpObjectId npc);

    /* The action running now in the main planner of the NPC (npc:motivation_action_manager():current_action_id()
       in Lua): GWP_INVALID_ACTION_ID when the planner has not run yet (initialized() is false), for a dead NPC and
       for anything but an online stalker. The same planner the Lua schemes fill; its evaluators may be native
       (evaluator_register).
       @group npc @thread main */
    uint32_t(GWP_CALL* npc_main_action)(GwpObjectId npc);

    /* The objects in the visual memory of the NPC, the list npc:memory_visible_objects() gives (o:object() of every
       entry): what the NPC saw and still remembers, seen now or not (npc_see tells that). As in Lua, the list
       belongs to the engine group of the NPC (team, squad, group) and holds what its members saw. An entry whose
       object is gone or offline is skipped. Writes up to `max` ids and returns how many there are in all (out may
       be NULL to count only); 0 for anything but an online stalker or monster.
       @group npc @thread main */
    uint32_t(GWP_CALL* npc_memory_visible_objects)(GwpObjectId npc, GwpObjectId* out, uint32_t max);

    /* How the creature treats `other`: npc:relation(other) - 0 friend, 1 neutral, 2 enemy (game_object.friend,
       .neutral, .enemy in Lua), 3 worst enemy (from the table of monster communities; Lua gets the same 3). A
       stalker asks the relation registry about a character (goodwill, communities), anything else goes by the
       table of monster communities. -1 when either is not an online creature (a stalker, a monster, the actor),
       without the script error Lua logs.
       @group npc @thread main */
    int(GWP_CALL* creature_relation)(GwpObjectId id, GwpObjectId other);

    /* npc:wounded(): the flag the wounded scheme sets on a stalker lying wounded (npc:wounded(true)); not the
       critical hit of npc_critically_wounded. 0 for anything but an online stalker, without the script error Lua
       logs.
       @group npc @thread main */
    int(GWP_CALL* npc_wounded)(GwpObjectId npc);

    /* The second object of the danger npc_best_danger reports, bd:dependent_object() in Lua: the engine sets it
       for GWP_DANGER_GRENADE only - the grenade, while GwpDanger::object is who threw it. GWP_INVALID_OBJECT_ID
       when the NPC fears nothing, the danger has no such object or it is not online.
       @group npc @thread main */
    GwpObjectId(GWP_CALL* npc_best_danger_dependent)(GwpObjectId npc);

    /* npc.health and npc.psy_health of Lua: what the conditions of the creature hold now, 0..1 (health of the dead
       is 0 or a little below it, down to -0.01; the engine kills at 0). -1.f for anything but an online creature (a
       stalker, a monster, the actor), as Lua gets it, without the script error.
       @group npc @thread main */
    float(GWP_CALL* creature_health)(GwpObjectId id);
    float(GWP_CALL* creature_psy_health)(GwpObjectId id);

    /* World position of a bone of an online object, obj:bone_position(bone): the bone matrix of the model the
       engine calculated last, times the object transform. NULL or "" names the root bone, as in Lua. 1 with
       out_xyz filled; 0 with out_xyz untouched for an object that is not online or has no skeleton and for a bone
       the skeleton does not have (Lua reads past the bones then).
       @group objects @thread main */
    int(GWP_CALL* object_bone_position)(GwpObjectId id, const char* bone, float out_xyz[3]);

    /* The online object that holds this one, obj:parent() in Lua: the owner of an item in an inventory (a stalker,
       the actor, a box). GWP_INVALID_OBJECT_ID for an object without a parent or not online.
       @group objects @thread main */
    GwpObjectId(GWP_CALL* object_parent)(GwpObjectId id);

    /* When the creature died, obj:death_time(): level_time_ms() of the death; a creature that came online dead (a
       loaded save, a corpse coming online) gets the time it came online. 0 for the living and for anything but an
       online creature, without the script error Lua logs.
       @group objects @thread main */
    uint32_t(GWP_CALL* object_death_time)(GwpObjectId id);

    /* npc:accessible(vertex): the NPC may go to the AI graph vertex, its space restrictors allow it (a stalker or a
       monster). 0 for an invalid vertex (Lua answers false there too) and for anything but an online NPC.
       @group npc_body @thread main */
    int(GWP_CALL* npc_accessible)(GwpObjectId npc, uint32_t level_vertex);

    /* obj:is_talking() - the character is in a dialog now (an inventory owner: a stalker, the actor, a monster; the
       flag the talk dialog sets). 0 for anything but an online inventory owner, as Lua gets it, without the script error.
       @group npc @thread main */
    int(GWP_CALL* character_is_talking)(GwpObjectId id);

    /* A native action in the planner of every stalker (wiki/doc/plugins/api/goap.md). The Lua action that holds
       `action_id` in `planner` is kept and wrapped: `mode` (GWP_ACTION_*) says who runs its steps, and
       action_set_mode switches it at any time, for every NPC at once; the mode of one run is fixed at its
       initialize, so a switch takes effect from the next run. lua_name: the name the Lua action was created with
       ("state_mgr_to_idle_combat_shim"); an NPC whose action slot holds another action (another name, another
       registration's proxy) is left as it is. NULL accepts any Lua action. The preconditions and effects of the
       Lua action are copied into the proxy (a Lua add_precondition on it after that reaches the proxy and is given
       back at the removal); save/load of the planner are delegated to the Lua action, byte for byte. Applied to
       the online stalkers and to every stalker that spawns later, at a point of the frame where no planner runs.
       action_unregister gives the action back to Lua: a run that is native at that moment is handed over - the Lua
       action is initialized at its next execute; unloading the plugin does the same. A crash in a plugin step
       disables the plugin (its properties and actions go back to Lua).
       GWP_INVALID_ACTION_REG_ID: a bad argument (a non-zero GwpActionVTable::reserved too), a flag bit in `mode`
       (GWP_MODE_FLAGS_MASK) or the same planner+action_id registered already (by any plugin); action_set_mode
       answers GWP_ERROR_NOT_SUPPORTED for a flag bit or a mode above GWP_ACTION_VERIFY. action_unregister and
       action_set_mode answer GWP_ERROR_NOT_FOUND for an id that is not a live registration of this plugin.
       @group goap @thread main */
    GwpActionRegId(GWP_CALL* action_register)(const GwpPlugin* self, uint32_t planner, uint32_t action_id,
        const char* lua_name, const GwpActionVTable* vtable, void* user, uint32_t mode);
    GwpResult(GWP_CALL* action_unregister)(const GwpPlugin* self, GwpActionRegId action);
    GwpResult(GWP_CALL* action_set_mode)(const GwpPlugin* self, GwpActionRegId action, uint32_t mode);
    int(GWP_CALL* action_stats)(GwpActionRegId action, GwpActionStats* out);

    /* The mode the current run of the action is in: GWP_ACTION_LUA / _NATIVE / _VERIFY; -1 without such an online
       stalker, planner or action id. A Lua action that no proxy wraps answers GWP_ACTION_LUA. A Lua event shim of a
       scheme asks this before it forwards an event of the action into the plugin.
       @group goap @thread main */
    int32_t(GWP_CALL* npc_action_running_mode)(GwpObjectId npc, uint32_t planner, uint32_t action_id);

    /* The squad fields of the simulation, read on demand (wiki/doc/plugins/api/alife.md). The squads of the mod are
       script classes (sim_squad_scripted): their state lives as Lua fields of the server object, Lua stays the only
       writer, and these read the live values - as fresh as the dot in Lua, about a microsecond per field. A squad is
       a server object online or not; the ids answer GWP_INVALID_OBJECT_ID and the flags 0 when the object is gone,
       is not a squad, or its class is pure C++ (no Lua fields to read).
       alife_squad_assigned_target / _current_target / _smart: squad.assigned_target_id, current_target_id, smart_id.
       alife_squad_current_action: squad.current_action, INT32_MIN for nil.
       alife_squad_script_target: the method squad:get_script_target() - the Lua call itself, every call of it (the
       method draws from the shared math.random of the save), its id, GWP_INVALID_OBJECT_ID for the nil it answers.
       alife_squad_always_arrived / _is_monster: squad.always_arrived (Lua truthiness) and
       is_squad_monster[squad.player_id] of _g.script. alife_squad_npc_count: squad:npc_count().
       @group alife @thread main */
    GwpObjectId(GWP_CALL* alife_squad_assigned_target)(GwpObjectId squad);
    GwpObjectId(GWP_CALL* alife_squad_current_target)(GwpObjectId squad);
    GwpObjectId(GWP_CALL* alife_squad_smart)(GwpObjectId squad);
    int32_t(GWP_CALL* alife_squad_current_action)(GwpObjectId squad);
    GwpObjectId(GWP_CALL* alife_squad_script_target)(GwpObjectId squad);
    int(GWP_CALL* alife_squad_always_arrived)(GwpObjectId squad);
    int(GWP_CALL* alife_squad_is_monster)(GwpObjectId squad);
    uint32_t(GWP_CALL* alife_squad_npc_count)(GwpObjectId squad);

    /* The server side of any object, online or not, for the geometry of the simulation (wiki/doc/plugins/api/alife.md).
       alife_object_smart: se_obj.m_smart_terrain_id of a stalker or a monster (GWP_INVALID_OBJECT_ID for none).
       alife_object_position: the server position (o_Position; the squad moves it with its commander / its offline
       brain, the smart keeps its spawn point). alife_object_game_vertex / _level_vertex: m_game_vertex_id /
       m_level_vertex_id (game vertex 0xFFFFFFFF for an object without AI locations). alife_object_level_id: the
       level of the game vertex, gg:vertex(m_game_vertex_id):level_id() of Lua (-1 without an object or a vertex).
       alife_object_class_id: the class id of the object as text, in the form object_class_id gives it - up to 8
       characters without the padding spaces ("SMRTTRRN" the script smart terrain, "ON_OFF_S" a squad, "S_ACTOR"
       the actor); copied into out (zero-terminated, cut to cap - 1; GWP_CLASS_ID_SIZE holds any), the length as the
       result, 0 without such an object; out may be NULL to ask the length.
       @group alife @thread main */
    int(GWP_CALL* alife_object_position)(GwpObjectId id, float out_xyz[3]);
    uint32_t(GWP_CALL* alife_object_game_vertex)(GwpObjectId id);
    uint32_t(GWP_CALL* alife_object_level_vertex)(GwpObjectId id);
    int32_t(GWP_CALL* alife_object_level_id)(GwpObjectId id);
    uint32_t(GWP_CALL* alife_object_class_id)(GwpObjectId id, char* out, uint32_t cap);
    GwpObjectId(GWP_CALL* alife_object_smart)(GwpObjectId id);

    /* smart.arrive_dist of a smart terrain: the Lua field its class reads from the ltx, the arrival radius of
       am_i_reached; -1.f without such an object or such a field.
       @group alife @thread main */
    float(GWP_CALL* alife_smart_arrive_dist)(GwpObjectId smart);

    /* The talk calls of the meet scripts (xr_meet), one engine method each - CInventoryOwner through
       CScriptGameObject, the very methods the Lua export calls. character_enable_talk / _disable_talk /
       _stop_talk / _is_talk_enabled: obj:enable_talk() / :disable_talk() / :stop_talk() / :is_talk_enabled() - an
       inventory owner only (a stalker, the actor, a trader, a monster): GWP_ERROR_INVALID_ARGUMENT / 0 for anything
       else (the Lua
       methods skip it silently), GWP_ERROR_NOT_FOUND when nothing with the id is online. object_set_tip_text:
       obj:set_tip_text(text) - the tip over the use cross of any online object; NULL or "" clears it to the empty
       string, as the "" the meet scripts set.
       @group npc @thread main */
    GwpResult(GWP_CALL* character_enable_talk)(const GwpPlugin* self, GwpObjectId id);
    GwpResult(GWP_CALL* character_disable_talk)(const GwpPlugin* self, GwpObjectId id);
    GwpResult(GWP_CALL* character_stop_talk)(const GwpPlugin* self, GwpObjectId id);
    int(GWP_CALL* character_is_talk_enabled)(GwpObjectId id);
    GwpResult(GWP_CALL* object_set_tip_text)(const GwpPlugin* self, GwpObjectId id, const char* text);

    /* The path of a stalker: where it goes and how (the movement setters of the scheme actions - reach_task,
       animpoint, cover, patrol soldiers). Each calls the CScriptGameObject method the Lua call does; what Lua only
       logs is refused here with GWP_ERROR_INVALID_ARGUMENT (not an online stalker, a value outside its list, an
       invalid vertex), what Lua would crash on is refused too.
       npc_set_dest_level_vertex: set_dest_level_vertex_id - also refused for a vertex the space restrictors of the
       NPC forbid (Lua logs "not accessible by its restrictors" and keeps the old destination); GWP_ERROR_INVALID_STATE
       when the level has no AI graph. npc_set_dest_game_vertex: set_dest_game_vertex_id (GWP_ERROR_INVALID_STATE
       without the game graph). npc_set_desired_position: set_desired_position(pos), NULL
       = set_desired_position() (clears). npc_set_desired_direction: set_desired_direction(dir), NULL clears; a zero
       vector is refused (Lua logs it and sets the zero direction all the same - an engine slip not copied), any other
       is normalized once, as Lua does outside the Shadow of Chernobyl mode.
       npc_set_detail_path_type: GWP_DETAIL_PATH_*. npc_set_movement_selection_type: GWP_ALIFE_MOVEMENT_*.
       npc_path_type: GWP_PATH_* (GWP_PATH_NONE for a wrong object). npc_detail_path_type: the type the movement
       manager has (Lua's detail_path_type() always answers move.line - an engine defect not copied here).
       npc_mental_state: the current mental state, npc:mental_state() (the target one is npc_target_mental_state);
       GWP_MENTAL_DANGER for a wrong object, as Lua.
       @group npc_body @thread main */
    GwpResult(GWP_CALL* npc_set_dest_level_vertex)(const GwpPlugin* self, GwpObjectId npc, uint32_t level_vertex);
    GwpResult(GWP_CALL* npc_set_dest_game_vertex)(const GwpPlugin* self, GwpObjectId npc, uint32_t game_vertex);
    GwpResult(GWP_CALL* npc_set_desired_position)(const GwpPlugin* self, GwpObjectId npc, const float* xyz);
    GwpResult(GWP_CALL* npc_set_desired_direction)(const GwpPlugin* self, GwpObjectId npc, const float* dir);
    GwpResult(GWP_CALL* npc_set_detail_path_type)(const GwpPlugin* self, GwpObjectId npc, uint32_t detail_path_type);
    GwpResult(GWP_CALL* npc_set_movement_selection_type)(const GwpPlugin* self, GwpObjectId npc,
        uint32_t selection_type);
    uint32_t(GWP_CALL* npc_path_type)(GwpObjectId npc);
    uint32_t(GWP_CALL* npc_detail_path_type)(GwpObjectId npc);
    uint32_t(GWP_CALL* npc_mental_state)(GwpObjectId npc);

    /* Where an NPC (a stalker or a monster) may go and where its path leads.
       npc_accessible_position: npc:accessible(pos) - its space restrictors allow the position.
       npc_accessible_nearest: npc:accessible_nearest(pos) - the nearest allowed vertex for a position they forbid,
       its position into out_xyz; GWP_INVALID_LEVEL_VERTEX (out_xyz untouched) for an allowed position (Lua logs
       "already accessible" there) or a wrong object. Together they are utils.send_to_nearest_accessible_vertex.
       npc_location_on_path: npc:location_on_path(distance) - the point `distance` metres ahead on the current
       detail path into out_xyz, its level vertex as the result; without an actual path (or at its end) the engine
       answers the position and the level vertex of the NPC itself, as for Lua; GWP_INVALID_LEVEL_VERTEX only for a
       wrong object or an NPC without a valid AI location.
       npc_path_completed: npc:path_completed() - the movement manager reached the end of its path.
       object_game_vertex: obj:game_vertex_id() of any online object (the game vertex of its AI location);
       GWP_INVALID_GAME_VERTEX without such an object.
       @group npc_body @thread main */
    int(GWP_CALL* npc_accessible_position)(GwpObjectId npc, const float xyz[3]);
    uint32_t(GWP_CALL* npc_accessible_nearest)(GwpObjectId npc, const float xyz[3], float out_xyz[3]);
    uint32_t(GWP_CALL* npc_location_on_path)(GwpObjectId npc, float distance, float out_xyz[3]);
    int(GWP_CALL* npc_path_completed)(GwpObjectId npc);
    uint32_t(GWP_CALL* object_game_vertex)(GwpObjectId id);

    /* The members of a squad, as `for k in squad:squad_members()` walks them (CSE_ALifeOnlineOfflineGroup::
       m_members, an associative vector: ascending ids, the commander is the first). Writes at most `max` ids into
       `out` (out may be NULL with max 0 to ask the count) and returns the full count; 0 for an object that is not a
       squad.
       @group alife @thread main */
    uint32_t(GWP_CALL* alife_squad_members)(GwpObjectId squad, GwpObjectId* out, uint32_t max);

    /* Where the path of a stalker leads now: movement().level_dest_vertex_id() / game_dest_vertex_id() - what the
       last set_dest_level_vertex_id / set_dest_game_vertex_id (or the engine itself) left. For a native action in
       the verify mode: the Lua step moved the destination, the plugin compares it with its own decision. No Lua
       method reads these. GWP_INVALID_LEVEL_VERTEX / GWP_INVALID_GAME_VERTEX for anything but an online stalker.
       @group npc_body @thread main */
    uint32_t(GWP_CALL* npc_dest_level_vertex)(GwpObjectId npc);
    uint32_t(GWP_CALL* npc_dest_game_vertex)(GwpObjectId npc);

    /* The patrol paths of the level - the data of Lua patrol(name) without its object. patrol_find: the handle of a
       path name (0 for a name the current level has no path of; the same handle for the same name, every call).
       A point is an index of the path (0 .. count - 1, the numbers Lua passes to patrol:point(i)). For a path the
       level has not (or no longer has, after a level change) or a point out of the path, a function returns its
       default, without the script error Lua logs: 0, GWP_INVALID_PATROL_POINT, GWP_INVALID_LEVEL_VERTEX,
       GWP_INVALID_GAME_VERTEX. The paths change only with the level: a plugin cache of points is dropped on
       level_on_start.
       patrol_point_count: patrol:count(). patrol_point_position: patrol:point(i) into out_xyz, 1 when written.
       patrol_point_level_vertex / _game_vertex: :level_vertex_id(i) / :game_vertex_id(i). patrol_point_flags: the
       32 flag bits of the point (:flags(i):get(); :flag(i, n) is bit n). patrol_point_terminal: :terminal(i) - no
       link leads on from the point. patrol_point_name: :name(i) - the full name of the point with its fields
       ("wp00|a=walk|p=50"), copied into out (zero-terminated, cut to cap - 1), the length of the full name as the
       result (0: no such point). patrol_point_index: :index(name) - the point of that name. patrol_nearest_point:
       :get_nearest(pos) - the point nearest to the position.
       @group patrol @thread main */
    GwpPatrol(GWP_CALL* patrol_find)(const char* name);
    uint32_t(GWP_CALL* patrol_point_count)(GwpPatrol path);
    int(GWP_CALL* patrol_point_position)(GwpPatrol path, uint32_t point, float out_xyz[3]);
    uint32_t(GWP_CALL* patrol_point_level_vertex)(GwpPatrol path, uint32_t point);
    uint32_t(GWP_CALL* patrol_point_game_vertex)(GwpPatrol path, uint32_t point);
    uint32_t(GWP_CALL* patrol_point_flags)(GwpPatrol path, uint32_t point);
    int(GWP_CALL* patrol_point_terminal)(GwpPatrol path, uint32_t point);
    uint32_t(GWP_CALL* patrol_point_name)(GwpPatrol path, uint32_t point, char* out, uint32_t cap);
    uint32_t(GWP_CALL* patrol_point_index)(GwpPatrol path, const char* point_name);
    uint32_t(GWP_CALL* patrol_nearest_point)(GwpPatrol path, const float xyz[3]);

    /* The patrol path of an NPC. npc_set_patrol_path: npc:set_patrol_path(name, start, route, random) - a stalker;
       GWP_ERROR_NOT_FOUND for a path the level has not (Lua asserts there), GWP_ERROR_INVALID_ARGUMENT for an empty
       name or a start / route outside GWP_PATROL_START_* / GWP_PATROL_ROUTE_*. npc_set_start_point:
       npc:set_start_point(i) - a stalker or a monster with a path that has the point: GWP_ERROR_INVALID_STATE
       without a path (Lua logs "Path not specified"), GWP_ERROR_INVALID_ARGUMENT for a point the path has not. With GWP_PATROL_START_POINT set the path first, the
       point after it: a new path resets the start point (without one the NPC heads for where it stands and the
       engine logs its restrictions every update). npc_patrol_point_index:
       npc:get_current_point_index() - GWP_INVALID_PATROL_POINT without a path or for anything but an online NPC.
       npc_patrol_path_name: the name of its patrol path into out (zero-terminated, cut to cap - 1), the length of
       the full name as the result; 0 without a path (Lua's patrol() logs "Path not specified" there).
       @group npc_body @thread main */
    GwpResult(GWP_CALL* npc_set_patrol_path)(const GwpPlugin* self, GwpObjectId npc, const char* name,
        uint32_t start_type, uint32_t route_type, int random);
    GwpResult(GWP_CALL* npc_set_start_point)(const GwpPlugin* self, GwpObjectId npc, uint32_t point);
    uint32_t(GWP_CALL* npc_patrol_point_index)(GwpObjectId npc);
    uint32_t(GWP_CALL* npc_patrol_path_name)(GwpObjectId npc, char* out, uint32_t cap);

    /* The cover of a level vertex towards a direction: level.high_cover_in_direction(vertex, dir) /
       level.low_cover_in_direction(vertex, dir) - 0 (open) .. 1 (covered) by the cover data of the AI graph (the
       camper crouches where the high cover is below 0.2). -1.f for an invalid vertex (Lua asserts there); a zero
       direction gives the cover at yaw 0, as for Lua.
       @group level @thread main */
    float(GWP_CALL* level_high_cover_in_direction)(uint32_t level_vertex, const float dir[3]);
    float(GWP_CALL* level_low_cover_in_direction)(uint32_t level_vertex, const float dir[3]);

    /* npc_body_state: npc:body_state() - the current pose (the target one is npc_target_body_state); GWP_BODY_STAND
       for anything but an online stalker, as Lua. npc_play_sound: npc:play_sound(type, max_start, min_start,
       max_stop, min_stop) - a sound of the sound player (GWP_STALKER_SOUND_* for a stalker, a number a script
       registered with add_sound, a MonsterSound type for a monster), times in ms as Lua passes them; a stalker or a
       monster. GWP_ERROR_NOT_FOUND for a type the NPC has no sound of (Lua plays nothing there, silently);
       GWP_ERROR_INVALID_ARGUMENT for a max below its min. npc_sniper_update_rate /
       npc_set_sniper_update_rate: npc:sniper_update_rate() / (value) - the faster visibility update of a sniper
       (the camper sniper turns it on while it scans); a stalker.
       @group npc_body @thread main */
    uint32_t(GWP_CALL* npc_body_state)(GwpObjectId npc);
    GwpResult(GWP_CALL* npc_play_sound)(const GwpPlugin* self, GwpObjectId npc, uint32_t sound_type,
        uint32_t max_start_ms, uint32_t min_start_ms, uint32_t max_stop_ms, uint32_t min_stop_ms);
    int(GWP_CALL* npc_sniper_update_rate)(GwpObjectId npc);
    GwpResult(GWP_CALL* npc_set_sniper_update_rate)(const GwpPlugin* self, GwpObjectId npc, int value);

    /* The target path type and detail path type of a stalker: what the last npc_set_path_type /
       npc_set_detail_path_type (or the Lua setters) asked for. npc_path_type / npc_detail_path_type answer the
       current ones, which the movement manager takes over from the target only at its next update (in Think, after
       the planner) - right after a set they still hold the old value. A native action compares these in the verify
       mode. GWP_PATH_NONE / GWP_DETAIL_PATH_SMOOTH for anything but an online stalker.
       @group npc_body @thread main */
    uint32_t(GWP_CALL* npc_target_path_type)(GwpObjectId npc);
    uint32_t(GWP_CALL* npc_target_detail_path_type)(GwpObjectId npc);

    /* The current enemy of a monster or a stalker: obj:get_enemy() of Lua (CCustomMonster::GetCurrentEnemy - for a
       monster its EnemyManager, NOT the memory selection npc_best_enemy answers). GWP_INVALID_OBJECT_ID without one,
       for a dead NPC (Lua logs an error there) or for anything but an online NPC.
       @group npc @thread main */
    GwpObjectId(GWP_CALL* npc_current_enemy)(GwpObjectId npc);

    /* --- Worker threads (group threads, wiki/doc/plugins/api/threads.md) --- */

    /* Splits [0, count) into pieces and calls fn on every piece, on the worker threads of the engine and on the
       calling thread, and returns when all pieces are done. grain is the smallest piece wanted (0: about
       count / task_worker_count()); the engine makes the pieces larger when there would be more than 16 per worker
       thread (its task queues are bounded). A heavy loop of the plugin over its own data: read the game before the
       call, write it after. Inside fn every function tagged @thread main refuses (GWP_ERROR_NOT_MAIN_THREAD,
       is_main_thread() == 0) on whatever thread the piece runs, the logic thread included: only @thread any
       functions may be called there, so fn cannot start another task_parallel_for. count 0: GWP_OK at once.
       GWP_ERROR_CRASHED when a piece crashed or threw: the plugin is stopped, as for any crash, and the pieces not
       started yet are skipped; GWP_ERROR_INVALID_ARGUMENT for a NULL fn; GWP_ERROR_INVALID_STATE without the task
       scheduler (engine shutdown).
       @group threads @thread main */
    GwpResult(GWP_CALL* task_parallel_for)(const GwpPlugin* self, uint32_t count, uint32_t grain, GwpTaskRangeFn fn,
        void* user);
    /* The number of threads the task scheduler of the engine runs tasks on, the logic thread included (the threads
       task_parallel_for spreads its pieces over); 0 before the task scheduler exists.
       @group threads @thread any */
    uint32_t(GWP_CALL* task_worker_count)(void);

    /* --- Services between plugins (group services, wiki/doc/plugins/api/services.md) --- */

    /* A plugin publishes an interface of its own - a struct of function pointers it defines, best starting with a
       uint32_t size - under "<addon id>/<name>" (up to 127 characters), version 1 or more. GWP_ERROR_INVALID_ARGUMENT
       for a malformed name, version 0 or a NULL iface; GWP_ERROR_ACCESS_DENIED for a name under another id;
       GWP_ERROR_INVALID_STATE for a plugin stopped by a crash. The pointer must stay valid until service_unregister
       or the unload of the plugin; the engine never calls into it. Every change is the event
       service_on_register(name, version) / service_on_unregister(name); registering the same name again replaces
       the interface and is both events (unregister of the old one, register of the new one). A service published
       during the init of the plugin is announced (and found by service_get) only once the plugin has loaded; a
       failed init drops it silently. The services of a plugin that stops are removed with service_on_unregister
       before its on_unload runs and before its library goes. service_unregister: GWP_ERROR_NOT_FOUND without such a
       service, GWP_ERROR_ACCESS_DENIED for one of another plugin.
       @group services @thread main */
    GwpResult(GWP_CALL* service_register)(const GwpPlugin* self, const char* name, uint32_t version, const void* iface);
    GwpResult(GWP_CALL* service_unregister)(const GwpPlugin* self, const char* name);
    /* The interface published under `name`, NULL when there is none or its version is below min_version;
       out_version (may be NULL) gets the published version, 0 without a service. A consumer keeps the pointer only
       until service_on_unregister(name) and does not call the interface from that handler: calls into a gone
       library crash the game, and a crash inside the provider is not caught for the consumer. Plugins load in no fixed order: a service may appear after the consumer's
       on_init - look for it again on service_on_register or alife_on_start.
       @group services @thread main */
    const void*(GWP_CALL* service_get)(const GwpPlugin* self, const char* name, uint32_t min_version,
        uint32_t* out_version);

    /* Whether an emit of the event would reach anybody now: a Lua or a native subscriber (a batch one too). Lets a
       plugin skip building the arguments of an event nobody listens to. 0 for an unknown or invalid id. The filters
       of the subscribers (GWP_SUBSCRIBE_OBJECT, throttle_ms) are not applied: 1 means somebody is subscribed, the
       emit may still skip them.
       @group events @thread main */
    int(GWP_CALL* event_has_subscribers)(GwpEventId id);

    /* --- ALife: reading and listing the server objects (wiki/doc/plugins/api/alife.md) -------------------- */

    /* alife_present: the simulation exists and is loaded (a game runs); every function of the groups alife and
       squads answers "no" without it. alife_free_ids: the free object ids (alife():get_free_ids()); a spawn is
       refused while fewer than 64 are left - the engine stops the game when it runs out of ids.
       @group alife @thread main */
    int(GWP_CALL* alife_present)(void);
    uint32_t(GWP_CALL* alife_free_ids)(void);

    /* The server side of any object, online or not. alife_object_exists: 1 when the id is in the ALife registry.
       alife_object_section / _name: the ltx section and the unique name (se_obj:name(), the "name_replace" the
       level editor or the spawn gave: "esc_smart_terrain_2_12", "wpn_ak74_13075"); the full length without the
       zero, `out` may be NULL, 0 without the object. alife_object_parent: who holds it in an inventory or a box
       (GWP_INVALID_OBJECT_ID in the world). alife_object_online / _alive: m_bOnline / g_Alive() (0 for anything
       but a creature). alife_object_community / _rank: se_obj:community() / se_obj:rank() of a stalker or the
       actor (a monster has a rank but no community); 0 / INT32_MIN without one.
       @group alife @thread main */
    int(GWP_CALL* alife_object_exists)(GwpObjectId id);
    uint32_t(GWP_CALL* alife_object_section)(GwpObjectId id, char* out, uint32_t cap);
    uint32_t(GWP_CALL* alife_object_name)(GwpObjectId id, char* out, uint32_t cap);
    GwpObjectId(GWP_CALL* alife_object_parent)(GwpObjectId id);
    int(GWP_CALL* alife_object_online)(GwpObjectId id);
    int(GWP_CALL* alife_object_alive)(GwpObjectId id);
    uint32_t(GWP_CALL* alife_object_community)(GwpObjectId id, char* out, uint32_t cap);
    int32_t(GWP_CALL* alife_object_rank)(GwpObjectId id);

    /* alife_object_by_name: the object with this unique name (alife():object(name) of Lua - a scan of the whole
       registry, for the rare lookup); GWP_INVALID_OBJECT_ID without one. alife_objects: the ids of the objects that
       pass `filter` (NULL = all), in the order of the registry (by id); writes at most `max` into `out` (may be
       NULL) and returns how many pass in all. A scan of the registry: tens of thousands of objects, around a
       millisecond - not every frame.
       @group alife @thread main */
    GwpObjectId(GWP_CALL* alife_object_by_name)(const char* name);
    uint32_t(GWP_CALL* alife_objects)(GwpObjectId* out, uint32_t max, const GwpAlifeFilter* filter);

    /* Story objects of the mod (story_objects.script): the name -> object registry the Lua scripts keep
       (get_story_object_id / get_object_story_id of _g.script), filled from the custom data [story_object] and
       the "story_id" lines of the sections, and the dynamic story ids of axr_dynamic_spawn. It is not the numeric
       story id of the engine (alife():story_object(n)): the scripts of the mod do not use that one. Calls into Lua
       (a missing Lua function or a Lua error answers "no" and is logged).
       story_object_id: GWP_INVALID_OBJECT_ID without such a name. object_story_id: the name of the object, the
       full length without the zero (`out` may be NULL), 0 for an object without one.
       @group alife @thread main */
    GwpObjectId(GWP_CALL* story_object_id)(const char* name);
    uint32_t(GWP_CALL* object_story_id)(GwpObjectId id, char* out, uint32_t cap);

    /* --- ALife: spawn, release, move, switch, kill --------------------------------------------------------- */

    /* Spawns an object (alife():create of Lua) and writes its id to out_id (may be NULL). The server object exists
       at once; the game object comes online later, through the frames: the next pass of the ALife switch for an
       object near the actor, the next network update for an item given to an online owner. A plugin learns it from
       a binder (on_spawn of binder_register, a mask that covers the section) or from the online object functions.
       A box of ammo spawned offline reaches server_object_on_register still full (the count is set right after);
       an item spawned into an online inventory has no parent yet in that event (the parent comes with the network).
       position: NULL = the position of `parent`. level_vertex / game_vertex: GWP_INVALID_LEVEL_VERTEX /
       GWP_INVALID_GAME_VERTEX = taken from the position on the current level (from `parent` with a parent).
       parent: an online or offline character or box that gets the item into its inventory (the object must be an
       inventory item then), GWP_INVALID_OBJECT_ID = in the world.
       GWP_ERROR_NOT_FOUND: no such section, or its class is not a server object; GWP_ERROR_INVALID_ARGUMENT: a bad
       parent, a squad section (use squad_create), vertices that cannot be found; GWP_ERROR_INVALID_STATE: no game,
       fewer than 64 free ids, a call from inside server_object_on_register / _on_unregister or from a binder
       on_save while the game is being saved (the same for every call below that changes ALife).
       alife_create_ammo: a box of ammo with `count` rounds (alife():create_ammo); count 1..box_size of the section.
       @group alife @thread main */
    GwpResult(GWP_CALL* alife_create)(const GwpPlugin* self, const char* section, const float position[3],
        uint32_t level_vertex, uint32_t game_vertex, GwpObjectId parent, GwpObjectId* out_id);
    GwpResult(GWP_CALL* alife_create_ammo)(const GwpPlugin* self, const char* section, const float position[3],
        uint32_t level_vertex, uint32_t game_vertex, GwpObjectId parent, uint32_t count, GwpObjectId* out_id);

    /* Removes an object from the game (alife():release of Lua). An offline object goes at once (its
       server_object_on_unregister comes inside the call); an online one is destroyed through the network events,
       a frame later. Refused (GWP_ERROR_INVALID_ARGUMENT): the actor, a squad (squad_remove keeps the bookkeeping
       of the simulation), a smart terrain. GWP_ERROR_NOT_FOUND: no such object; GWP_ERROR_INVALID_STATE: no game,
       a call from inside server_object_on_register / _on_unregister or from a binder on_save during a save.
       @group alife @thread main */
    GwpResult(GWP_CALL* alife_release)(const GwpPlugin* self, GwpObjectId id);

    /* Moves an object of the simulation to a game vertex of any level (TeleportObject of _g.script: the vertex
       caches of the scripts are dropped, then alife():teleport_object). An online object goes offline first and
       comes back by the usual switch when the place is near the actor. level_vertex GWP_INVALID_LEVEL_VERTEX = the
       level vertex of the game vertex; position NULL = the point of the game vertex. Refused: the actor
       (GWP_ERROR_NOT_SUPPORTED - on its level use object_teleport, to another level the game has no API), an item
       inside an inventory, a squad or a member of one (use squad_teleport: a member moved alone lives apart from
       its squad) - GWP_ERROR_INVALID_ARGUMENT; a bad vertex too. GWP_ERROR_NOT_SUPPORTED: the Lua function
       TeleportObject is missing (the scripts of the mod are not loaded); GWP_ERROR_CRASHED: it raised an error.
       @group alife @thread main */
    GwpResult(GWP_CALL* alife_teleport)(const GwpPlugin* self, GwpObjectId id, uint32_t game_vertex,
        uint32_t level_vertex, const float position[3]);

    /* Online/offline switching of an object in the world. alife_set_switch_online / _offline:
       alife():set_switch_online(id, value) / set_switch_offline(id, value) - may the ALife switch bring it online /
       take it offline (both on by default; the switch itself happens at its next pass). alife_switch_offline: takes
       an online object offline right now (the game object is destroyed through the network events); it comes back
       at the next pass unless alife_set_switch_online(id, 0) holds it. Refused: the actor, an object inside an
       inventory, a member of a squad - switch the squad (GWP_ERROR_INVALID_ARGUMENT); an offline object
       (GWP_ERROR_INVALID_STATE).
       @group alife @thread main */
    GwpResult(GWP_CALL* alife_set_switch_online)(const GwpPlugin* self, GwpObjectId id, int value);
    GwpResult(GWP_CALL* alife_set_switch_offline)(const GwpPlugin* self, GwpObjectId id, int value);
    GwpResult(GWP_CALL* alife_switch_offline)(const GwpPlugin* self, GwpObjectId id);

    /* Kills a creature, online or offline. Online: the usual death of the engine (obj:kill(who) of Lua) - the death
       events, the callbacks, the corpse; for the actor actor_on_before_death may cancel it, in god mode it is
       refused (GWP_ERROR_INVALID_STATE). Offline: the health goes to zero where the object stands (se_obj:kill() of
       the scripts) and the server death follows - the Lua on_death of the server object, squad_on_npc_death, the
       smart frees its place; the corpse keeps its inventory. No callbacks of online objects (there is none). who: the
       killer (GWP_INVALID_OBJECT_ID = the victim itself; an online victim takes only an online killer, else itself).
       GWP_ERROR_INVALID_ARGUMENT: not a creature; GWP_ERROR_INVALID_STATE: dead already.
       @group alife @thread main */
    GwpResult(GWP_CALL* object_kill)(const GwpPlugin* self, GwpObjectId id, GwpObjectId who);

    /* --- Squads and smart terrains of the simulation (wiki/doc/plugins/api/squads.md) --------------------- */

    /* The squads of the mod are script classes (sim_squad_scripted) driven by the Lua simulation (sim_board): these
       functions call the Lua side (gw_plugin_sim.script), which keeps its own bookkeeping consistent. A Lua error
       gives GWP_ERROR_CRASHED, a missing Lua function GWP_ERROR_NOT_SUPPORTED, Lua calls of this group nested deeper
       than 16 GWP_ERROR_INVALID_STATE (all logged); the readers answer "no" then. Squad and smart ids are server object
       ids.
       squad_create: a squad of the section (an ltx section of class "ON_OFF_S") with its members. At a smart
       (`smart` given): SIMBOARD:create_squad - at the smart's point, assigned to it; position and the vertices are
       ignored. In the open (`smart` = GWP_INVALID_OBJECT_ID): at the position (the vertices as for alife_create),
       assigned to nothing. squad_on_npc_creation is sent for every member (the smart argument is nil in the open).
       GWP_ERROR_INVALID_STATE also with fewer than 256 free ids or before the simulation board exists. A smart, a
       squad or a target id missing from ALife: GWP_ERROR_NOT_FOUND. GWP_ERROR_INVALID_ARGUMENT also when the smart is
       not on the simulation board (disabled) and when the engine did not create the squad in the open.
       squad_remove: sim_squad.remove_squad - the bookkeeping of the simulation and every member go.
       @group squads @thread main */
    GwpResult(GWP_CALL* squad_create)(const GwpPlugin* self, const char* section, GwpObjectId smart,
        const float position[3], uint32_t level_vertex, uint32_t game_vertex, GwpObjectId* out_id);
    GwpResult(GWP_CALL* squad_remove)(const GwpPlugin* self, GwpObjectId squad);

    /* squad_assign_smart: SIMBOARD:assign_squad_to_smart - the squad belongs to the smart (counts in its
       population); GWP_INVALID_OBJECT_ID unassigns it; a smart the board does not know (disabled) is
       GWP_ERROR_INVALID_ARGUMENT. squad_set_target: sim_squad.set_target - the squad goes to a
       smart or chases a squad (the order the PDA gives); GWP_ERROR_INVALID_STATE when the simulation refused it (a
       squad section of sim_tables.ignore, a leader squad). squad_teleport: TeleportSquad of _g.script - the squad and
       every member to a game vertex of any level (the vertices and the position as for alife_teleport).
       @group squads @thread main */
    GwpResult(GWP_CALL* squad_assign_smart)(const GwpPlugin* self, GwpObjectId squad, GwpObjectId smart);
    GwpResult(GWP_CALL* squad_set_target)(const GwpPlugin* self, GwpObjectId squad, GwpObjectId target);
    GwpResult(GWP_CALL* squad_teleport)(const GwpPlugin* self, GwpObjectId squad, uint32_t game_vertex,
        uint32_t level_vertex, const float position[3]);

    /* squad_community: squad:get_squad_community() - the community of the squad ("stalker", "monster_predatory",
       ...), the full length without the zero (`out` may be NULL), 0 for anything but a squad.
       squad_set_community: squad:set_squad_community - the squad and every member (online ones too) change sides;
       GWP_ERROR_INVALID_ARGUMENT for a community the game or the squads do not know and for a squad of monsters.
       smart_find: the smart terrain by its name (SIMBOARD.smarts_by_names); GWP_INVALID_OBJECT_ID without one.
       @group squads @thread main */
    uint32_t(GWP_CALL* squad_community)(GwpObjectId squad, char* out, uint32_t cap);
    GwpResult(GWP_CALL* squad_set_community)(const GwpPlugin* self, GwpObjectId squad, const char* community);
    GwpObjectId(GWP_CALL* smart_find)(const char* name);

    /* --- The world: the game graph, levels, weather, time (wiki/doc/plugins/api/world.md) ----------------- */

    /* The game graph: the vertices of every level that the simulation moves objects by (a game vertex id is the
       game_vertex of the alife and objects groups). graph_vertex_count: 0 without a game. graph_vertex_level_id:
       the level of the vertex (-1 for a bad id). graph_vertex_level_vertex: the AI vertex of its level under it
       (GWP_INVALID_LEVEL_VERTEX for a bad id). graph_vertex_position: its point on its own level (level_point() of
       Lua); 0 for a bad id. graph_vertex_by_level_vertex: the game vertex an AI vertex of the CURRENT level belongs
       to (the cross table; GWP_INVALID_GAME_VERTEX for a bad vertex or without a level).
       @group world @thread main */
    uint32_t(GWP_CALL* graph_vertex_count)(void);
    int32_t(GWP_CALL* graph_vertex_level_id)(uint32_t game_vertex);
    uint32_t(GWP_CALL* graph_vertex_level_vertex)(uint32_t game_vertex);
    int(GWP_CALL* graph_vertex_position)(uint32_t game_vertex, float out_xyz[3]);
    uint32_t(GWP_CALL* graph_vertex_by_level_vertex)(uint32_t level_vertex);

    /* Levels of the game graph. level_name_by_id: alife():level_name(id), the full length without the zero (`out` may
       be NULL; 0 for an unknown id). level_id_by_name: -1 for an unknown name. level_current_id: the level being
       played (alife():level_id()); -1 without a level.
       @group world @thread main */
    uint32_t(GWP_CALL* level_name_by_id)(int32_t level_id, char* out, uint32_t cap);
    int32_t(GWP_CALL* level_id_by_name)(const char* name);
    int32_t(GWP_CALL* level_current_id)(void);

    /* Moves an online object on the current level, at once: the actor (set_actor_position), a stalker or a monster
       (set_npc_position: its path is dropped), a physics object or a corpse (force_set_position, the shell or the
       ragdoll is woken up). GWP_ERROR_INVALID_STATE: the actor sits in a vehicle or at a mounted gun.
       GWP_ERROR_NOT_FOUND: no such online object; GWP_ERROR_INVALID_ARGUMENT: an object inside an inventory;
       GWP_ERROR_NOT_SUPPORTED: an object of another kind (a physics object without a shell).
       actor_set_direction: turns the camera of the actor to look along `dir` (yaw and pitch; no roll). Lua
       set_actor_direction(yaw) takes only the yaw. GWP_ERROR_INVALID_ARGUMENT for a zero or a vertical vector;
       GWP_ERROR_INVALID_STATE without the actor or while it sits in a vehicle or at a mounted gun.
       @group world @thread main */
    GwpResult(GWP_CALL* object_teleport)(const GwpPlugin* self, GwpObjectId id, const float position[3]);
    GwpResult(GWP_CALL* actor_set_direction)(const GwpPlugin* self, const float dir[3]);

    /* Blows up an explosive online object (a grenade, a barrel, an anomaly-free explosive; obj:explode() of Lua): the
       explosion event is sent now, the explosion itself comes with it in the next network update. The object must
       lie in the world. GWP_ERROR_NOT_FOUND: no such online object; GWP_ERROR_INVALID_ARGUMENT: not an explosive or
       inside an inventory; GWP_ERROR_INVALID_STATE: it explodes already.
       @group world @thread main */
    GwpResult(GWP_CALL* object_explode)(const GwpPlugin* self, GwpObjectId id);

    /* Weather (level.get_weather / set_weather / set_weather_fx / stop_weather_fx of Lua). The weather of the mod is
       chosen by level_weathers.script at every change of the game hour and on a load: a weather set here lasts until
       then. weather_get: the current weather - the name of the effect while one plays - the full length without the
       zero (`out` may be NULL), 0 without a level. weather_set: a weather cycle (a file of
       environment\weathers); forced = 1 switches at once, 0 blends into it; while an effect plays the cycle takes
       over after it. GWP_ERROR_NOT_FOUND: no such cycle.
       weather_fx_start: an effect (environment\weather_effects: a blowout, a psi storm, ...); time_left = 0 plays
       it whole, > 0 resumes it with that many GAME seconds left (the engine counts an effect down: what the mod
       saves and restores with weather_fx_time), at most 86400; GWP_ERROR_INVALID_STATE while another one plays or
       before the weather is set up after a load or when the engine refused to start it, GWP_ERROR_NOT_FOUND for an
       unknown name. weather_fx_stop:
       GWP_ERROR_INVALID_STATE when none plays. weather_fx_playing / weather_fx_time: whether an effect plays and the
       game seconds it has LEFT (0 without one).
       weather_rain_factor: the rain density of the current weather, 0..1 (level.rain_factor()).
       @group world @thread main */
    uint32_t(GWP_CALL* weather_get)(char* out, uint32_t cap);
    GwpResult(GWP_CALL* weather_set)(const GwpPlugin* self, const char* name, int forced);
    GwpResult(GWP_CALL* weather_fx_start)(const GwpPlugin* self, const char* name, float time_left);
    GwpResult(GWP_CALL* weather_fx_stop)(const GwpPlugin* self);
    int(GWP_CALL* weather_fx_playing)(void);
    float(GWP_CALL* weather_fx_time)(void);
    float(GWP_CALL* weather_rain_factor)(void);

    /* Writing the game time. game_time_advance: moves the game time forward by `milliseconds` (level.change_game_time
       of Lua, which takes days, hours and minutes): the clock of ALife and the sky jump together; timers on game time
       fire on their next check; at most ten game years (GWP_ERROR_INVALID_ARGUMENT). Only the engine part: the
       scripts of the mod that skip time (sleep, fast travel) also tell the blowout and psi storm managers and the
       weather script (wiki world.md). game_time_set_factor: game seconds per real second (level.set_time_factor; read
       it with game_time_factor of the group timers), 0 < factor <= 1e6. Both GWP_ERROR_INVALID_STATE without a game.
       @group world @thread main */
    GwpResult(GWP_CALL* game_time_advance)(const GwpPlugin* self, uint64_t milliseconds);
    GwpResult(GWP_CALL* game_time_set_factor)(const GwpPlugin* self, double factor);

    /* --- Messages to the player (group feedback, wiki/doc/plugins/api/feedback.md) ---------------------------- */

    /* A PDA news message to the actor (actor:give_game_news of Lua, news_manager.send_tip). caption_id and text_id
       are ids of strings of configs/text: the engine stores the ids and the PDA translates them when it shows them
       (a text that is no id is shown as it is). caption_id NULL = no caption. icon: a texture of the UI
       ("ui_iconsTotal_grouping", a character icon); NULL or "" = "ui_iconsTotal_grouping". delay_ms: the message
       comes that much later (real time; 0 = now, at most a day: 86400000); show_ms: how long it stays on the HUD,
       0 = 5000. The SMS option of the mod (mm_options enable_enable_sms) is the business of the caller, as in Lua
       news_manager. GWP_ERROR_INVALID_ARGUMENT: no text_id, delay_ms above a day; GWP_ERROR_INVALID_STATE: no actor
       (no game loaded).
       @group feedback @thread main */
    GwpResult(GWP_CALL* news_send)(const GwpPlugin* self, const char* caption_id, const char* text_id,
        const char* icon, uint32_t delay_ms, uint32_t show_ms);

    /* A message in the middle of the HUD (SetHudMsg of _g.script): a custom static of configs/ui/ui_custom_msgs.xml
       shows the string text_id (translated) for show_ms (> 0), then the engine removes it. static_id: the node of
       that file; NULL or "" = "not_enough_money_mine", the static of SetHudMsg. A message to a static that is shown
       already replaces its text and restarts its time. hud_message_hide removes the static at once.
       GWP_ERROR_NOT_FOUND: ui_custom_msgs.xml has no such node (hud_message_hide: the static is not shown);
       GWP_ERROR_INVALID_ARGUMENT: no text_id, show_ms 0, a static_id of 128 characters or more or with ':' (a node
       name, not a path); GWP_ERROR_INVALID_STATE: no HUD (no level).
       @group feedback @thread main */
    GwpResult(GWP_CALL* hud_message)(const GwpPlugin* self, const char* static_id, const char* text_id,
        uint32_t show_ms);
    GwpResult(GWP_CALL* hud_message_hide)(const GwpPlugin* self, const char* static_id);

    /* The string id in the current language (game.translate_string): copied into out (zero-terminated; cut to fit
       cap - 1 bytes at a whole UTF-8 character), the length of the full text without the zero as the result; out may
       be NULL (or cap 0) to ask the length. An id without a translation gives the id itself, as in Lua; 0 for NULL
       or "".
       @group feedback @thread main */
    uint32_t(GWP_CALL* translate)(const char* id, char* out, uint32_t cap);

    /* PDA map spots on an object (level.map_add_object_spot / map_remove_object_spot / map_has_object_spot /
       map_change_spot_hint). object: a server object, online or not - the spot follows it. spot_type: a node of
       configs/ui/map_spots.xml ("red_location", ...). hint_id: a string id shown under the cursor (translated when
       shown); NULL or "" = the hint of the spot type. The spots are game state, not resources of the plugin: a spot
       type with store="1" goes into the save, and the spots stay when the plugin unloads.
       map_spot_add on an object that has a spot of this type already changes only its hint (Lua adds a second one).
       GWP_ERROR_NOT_FOUND: no such object, no such spot type in map_spots.xml (add) or the object has no spot of
       this type (remove, set_hint); GWP_ERROR_INVALID_STATE: no level; GWP_ERROR_INVALID_ARGUMENT: no spot_type, one
       of 128 characters or more or with ':'. map_spot_has: 1 when the object has a spot of this type, 0 otherwise.
       @group feedback @thread main */
    GwpResult(GWP_CALL* map_spot_add)(const GwpPlugin* self, GwpObjectId object, const char* spot_type,
        const char* hint_id);
    GwpResult(GWP_CALL* map_spot_remove)(const GwpPlugin* self, GwpObjectId object, const char* spot_type);
    int(GWP_CALL* map_spot_has)(GwpObjectId object, const char* spot_type);
    GwpResult(GWP_CALL* map_spot_set_hint)(const GwpPlugin* self, GwpObjectId object, const char* spot_type,
        const char* hint_id);

    /* --- Sounds, particles, effectors (group effects, wiki/doc/plugins/api/effects.md) ------------------------ */

    /* A sound of the plugin (sound_object of Lua). path: a file of $game_sounds$ without ".ogg"
       ("ambient\\rnd_outdoor\\rnd_crow_1"). sound_play_at plays it at a point of the world, sound_play_2d in the head
       of the player (s2d). volume >= 0 (1 = as recorded). flags: GWP_SOUND_LOOPED; another bit
       GWP_ERROR_NOT_SUPPORTED. out_sound (may be NULL: play and forget) gets the handle. The engine owns the sound:
       it lives until it ends (a looped one: until sound_stop), the plugin stops (unload, crash), a game is started or
       loaded, or the level goes; then the handle is dead. At most GWP_SOUND_MAX_PER_PLUGIN sounds of a plugin play
       at once: GWP_ERROR_INVALID_STATE above that. GWP_ERROR_NOT_FOUND: no such file. Without a sound device (-nosound, no
       sound card) the call succeeds: the sound never plays, sound_is_playing answers 0 and the handle is dead soon.
       GWP_ERROR_INVALID_STATE also when there is no sound system at all (the engine is shutting it down).
       sound_stop: GWP_ERROR_NOT_FOUND for a dead handle (the sound ended) or a handle of another plugin.
       sound_is_playing: 1 while the sound plays, 0 for a dead handle.
       @group effects @thread main */
    GwpResult(GWP_CALL* sound_play_at)(const GwpPlugin* self, const char* path, const float pos[3], float volume,
        uint32_t flags, GwpSound* out_sound);
    GwpResult(GWP_CALL* sound_play_2d)(const GwpPlugin* self, const char* path, float volume, uint32_t flags,
        GwpSound* out_sound);
    GwpResult(GWP_CALL* sound_stop)(const GwpPlugin* self, GwpSound sound);
    int(GWP_CALL* sound_is_playing)(GwpSound sound);

    /* A particle effect of the plugin (particles_object of Lua). name: an effect or a group of the particle library
       ("explosions\\explosion_dynamite"); an unknown one is GWP_ERROR_NOT_FOUND (Lua stops the game there).
       particles_play_at: at a point; dir (may be NULL) turns the effect so its Z axis looks along dir.
       particles_attach: at a bone of an online object (bone NULL or "" = the root bone), following it every frame;
       when the object goes offline the effect stops as with GWP_PARTICLES_STOP_DEFERRED. GWP_ERROR_NOT_FOUND: no
       such online object or bone; GWP_ERROR_INVALID_ARGUMENT: the object has no skeleton.
       A non-looped effect ends by itself, a looped one plays until particles_stop. The handle lives as long as the
       effect, under the rules of the sounds (GWP_PARTICLES_MAX_PER_PLUGIN; a load drops the effects too).
       particles_stop: without flags the effect disappears at once and the handle is dead; with
       GWP_PARTICLES_STOP_DEFERRED the emitters stop and the handle lives while the particles fade.
       GWP_ERROR_NOT_FOUND for a dead handle or one of another plugin. particles_is_playing: 1 while the effect
       is alive.
       @group effects @thread main */
    GwpResult(GWP_CALL* particles_play_at)(const GwpPlugin* self, const char* name, const float pos[3],
        const float dir[3], GwpParticles* out_particles);
    GwpResult(GWP_CALL* particles_attach)(const GwpPlugin* self, const char* name, GwpObjectId object,
        const char* bone, GwpParticles* out_particles);
    GwpResult(GWP_CALL* particles_stop)(const GwpPlugin* self, GwpParticles particles, uint32_t flags);
    int(GWP_CALL* particles_is_playing)(GwpParticles particles);

    /* Post-process and camera effectors on the actor (level.add_pp_effector / remove_pp_effector /
       set_pp_effector_factor, level.add_cam_effector / remove_cam_effector). file: a file of $game_anims$ (or of
       the level): ".ppe" for a pp effector ("teleport.ppe"), ".anm" or ".anms" for a camera one
       ("camera_effects\\hit_back_left.anm"). id: 0..GWP_EFFECTOR_ID_MAX, chosen by the plugin; the engine maps it
       into a range of its own per plugin, so the ids of plugins and of Lua never meet. The same id again replaces
       the effector. cyclic: 1 = loops until removed, 0 = plays once. cam_effector_add puts the length of the
       animation in seconds into out_length (may be NULL); a camera effector starts at the next camera update,
       cam_effector_remove takes it away also before that. pp_effector_remove fades the effector out within a second, as Lua.
       pp_effector_set_factor: the target strength (0..1) and the speed of the change per second.
       The effectors of a plugin are removed when it stops; a new game, a load and a level change drop them with the
       actor. GWP_ERROR_NOT_FOUND: no such file, or no such effector of the plugin running (remove, set_factor); a file
       that exists but is broken (a wrong version of the format) still stops the game in the loader, as in Lua;
       GWP_ERROR_INVALID_ARGUMENT: a file of another type, an id above GWP_EFFECTOR_ID_MAX;
       GWP_ERROR_INVALID_STATE: no actor.
       @group effects @thread main */
    GwpResult(GWP_CALL* pp_effector_add)(const GwpPlugin* self, const char* file, uint32_t id, int cyclic);
    GwpResult(GWP_CALL* pp_effector_remove)(const GwpPlugin* self, uint32_t id);
    GwpResult(GWP_CALL* pp_effector_set_factor)(const GwpPlugin* self, uint32_t id, float factor, float speed);
    GwpResult(GWP_CALL* cam_effector_add)(const GwpPlugin* self, const char* file, uint32_t id, int cyclic,
        float* out_length);
    GwpResult(GWP_CALL* cam_effector_remove)(const GwpPlugin* self, uint32_t id);

    /* --- Inventory and items (group items, wiki/doc/plugins/api/items.md) --------------------------------- */

    /* What an inventory holds. The owner is an online object with an inventory (the actor, a stalker, a trader, a
       monster) or an inventory box (iterate_inventory_box of Lua). inventory_find: the first item of
       `section` in it, obj:object(section) of Lua (boxes too); GWP_INVALID_OBJECT_ID when there is none or the owner is
       neither. inventory_items: every item, in the order the inventory keeps them (the order they came in:
       iterate_inventory of Lua, quest items and the items in slots too - unlike the Lua method inventory_for_each,
       which lists only what may be traded). Writes at most `max` ids, returns the full count (`out` may be NULL); 0 for anything else.
       @group items @thread main */
    GwpObjectId(GWP_CALL* inventory_find)(GwpObjectId owner, const char* section);
    uint32_t(GWP_CALL* inventory_items)(GwpObjectId owner, GwpObjectId* out, uint32_t max);

    /* Moving an item. Both are network events of the engine, as for Lua: right after the call the item is still where
       it was, it moves when the events are processed (in this frame or the next one); object_parent tells.
       inventory_transfer: obj:transfer_item(item, to) of Lua called on `from` - the item leaves `from` (its current
       owner: object_parent(item) == from; an inventory owner or a box - not the trunk of a car or an anomaly holding
       its artefact) for `to` (an inventory owner or a box, not `from`). A stalker that cannot
       carry it (the weight limit) drops it on the ground at once.
       inventory_drop: obj:drop_item(item) / drop_item_and_teleport(item, position) - the owner drops the item; position
       NULL: where the engine drops it (at the owner), else the item is moved to `position` after the drop.
       GWP_ERROR_NOT_FOUND: an object is not online; GWP_ERROR_INVALID_ARGUMENT: not an inventory item, not owned by
       `from` / `owner`, `to` holds no inventory, a position with NaN or infinity.
       In this group, as everywhere: GWP_ERROR_NOT_FOUND - nothing with the id is online, GWP_ERROR_INVALID_ARGUMENT -
       an online object of another kind, GWP_ERROR_INVALID_STATE - no level or no game.
       @group items @thread main */
    GwpResult(GWP_CALL* inventory_transfer)(const GwpPlugin* self, GwpObjectId item, GwpObjectId from, GwpObjectId to);
    GwpResult(GWP_CALL* inventory_drop)(const GwpPlugin* self, GwpObjectId owner, GwpObjectId item,
        const float* position);

    /* Slots. inventory_active_slot: obj:active_slot() - the slot of the item in the hands (GWP_SLOT_*); GWP_SLOT_NONE
       when the hands are empty or the object has no inventory. inventory_activate_slot: actor:activate_slot(slot) -
       the actor takes the item of the slot (the item in the hands is hidden first: the switch takes the time of the
       animations); GWP_SLOT_NONE hides the item in the hands. The actor only: a stalker takes weapons with
       npc_set_item (group inventory). GWP_ERROR_INVALID_ARGUMENT: not the actor, a slot past the last one of the
       inventory (GWP_SLOT_* and the slots the mod adds above them: GlobalWar has slot 14);
       GWP_ERROR_INVALID_STATE: the engine did not start the switch (an empty slot, a slot that cannot be active, a
       blocked inventory - Lua skips such a call silently). The grenade slot: when it is empty the engine moves a
       grenade from the backpack into it without taking it - INVALID_STATE, call again to take it.
       @group items @thread main */
    uint32_t(GWP_CALL* inventory_active_slot)(GwpObjectId owner);
    GwpResult(GWP_CALL* inventory_activate_slot)(const GwpPlugin* self, GwpObjectId owner, uint32_t slot);

    /* An attachable item (one the model of its owner can show: a torch, the props of the special animations of the
       state manager): item:enable_attachable_item(enable) - shown on the owner's model or hidden. A local change of the
       client, no network event, as in Lua. item_attachable_enabled: 1 shown, 0 hidden or not an attachable item.
       GWP_ERROR_INVALID_ARGUMENT: not an attachable item.
       @group items @thread main */
    GwpResult(GWP_CALL* item_set_attachable)(const GwpPlugin* self, GwpObjectId item, int enable);
    int(GWP_CALL* item_attachable_enabled)(GwpObjectId item);

    /* The condition of an item, 0 (broken) .. 1 (new): item:condition() / item:set_condition(value) of Lua (the value
       is clamped to 0 .. 1). -1.f for anything but an online inventory item. Weapons, outfits and helmets pass it to
       the server object with their next network update; any item keeps it in its own save data.
       GWP_ERROR_INVALID_ARGUMENT: not an inventory item, NaN or infinity.
       @group items @thread main */
    float(GWP_CALL* item_condition)(GwpObjectId item);
    GwpResult(GWP_CALL* item_set_condition)(const GwpPlugin* self, GwpObjectId item, float condition);

    /* The magazine of a firearm (any weapon with ammo: CWeapon). A weapon with a grenade launcher in the grenade mode
       answers for the grenades: the engine swaps its two magazines on the switch.
       item_ammo_elapsed: get_ammo_in_magazine() - the cartridges in the magazine; item_magazine_size: their maximum;
       -1 for anything but a weapon. item_set_ammo_elapsed: set_ammo_elapsed(count) - cartridges of the current ammo
       type are added to the magazine or taken out of it (they come from nowhere and go nowhere: the inventory is
       untouched); count above the magazine size is refused, and so is adding cartridges to a weapon whose current
       ammo type is outside its list.
       item_ammo_type: get_ammo_type() - the index of the current ammo type in the ammo list of the weapon (-1 for
       anything but a weapon); item_set_ammo_type: set_ammo_type(index) - the type the next loaded cartridges have, the
       cartridges in the magazine stay as they are (as in Lua); an index outside the list is refused.
       item_ammo_type_count: the length of the ammo list (0 for anything else). item_ammo_section: the ltx section of
       an ammo type (weapon_get_ammo_section of Lua), the full length without the zero (`out` may be NULL); 0 for a bad
       index or not a weapon.
       GWP_ERROR_INVALID_ARGUMENT: not a weapon, a value out of range.
       @group items @thread main */
    int32_t(GWP_CALL* item_ammo_elapsed)(GwpObjectId item);
    int32_t(GWP_CALL* item_magazine_size)(GwpObjectId item);
    GwpResult(GWP_CALL* item_set_ammo_elapsed)(const GwpPlugin* self, GwpObjectId item, uint32_t count);
    int32_t(GWP_CALL* item_ammo_type)(GwpObjectId item);
    GwpResult(GWP_CALL* item_set_ammo_type)(const GwpPlugin* self, GwpObjectId item, uint32_t type);
    uint32_t(GWP_CALL* item_ammo_type_count)(GwpObjectId item);
    uint32_t(GWP_CALL* item_ammo_section)(GwpObjectId item, uint32_t type, char* out, uint32_t cap);

    /* Upgrades of an item (the upgrade tree of its section, configs\inventory\upgrades). item_has_upgrade:
       item:has_upgrade(upgrade); 0 for anything else. item_upgrade_count / item_upgrade_at: the installed upgrades in
       the order they were installed (iterate_installed_upgrades of Lua); item_upgrade_at copies the section of one, the
       full length without the zero (`out` may be NULL), 0 for a bad index.
       item_install_upgrade: flags 0 = item:install_upgrade(upgrade) of Lua: the checks of the tree (not installed yet,
       the group order, the Lua preconditions of the upgrade), the magazine unloaded into the inventory, the upgrade and
       its effects applied. GWP_UPGRADE_NO_CHECKS = item:add_upgrade(upgrade); GWP_UPGRADE_CHECK_ONLY tells only. The
       upgrade reaches the server object with a network event and stays with the save. An upgrade cannot be removed:
       the engine has no way back.
       GWP_ERROR_NOT_FOUND: no such online object; GWP_ERROR_INVALID_ARGUMENT: not an inventory item, an item without
       an upgrade tree, an upgrade outside its tree (Lua would crash there), an unknown flag; GWP_ERROR_INVALID_STATE:
       the checks refuse it now (installed already, a precondition of the tree).
       @group items @thread main */
    int(GWP_CALL* item_has_upgrade)(GwpObjectId item, const char* upgrade);
    uint32_t(GWP_CALL* item_upgrade_count)(GwpObjectId item);
    uint32_t(GWP_CALL* item_upgrade_at)(GwpObjectId item, uint32_t index, char* out, uint32_t cap);
    GwpResult(GWP_CALL* item_install_upgrade)(const GwpPlugin* self, GwpObjectId item, const char* upgrade,
        uint32_t flags);

    /* --- Characters: money, goodwill, rank, conditions (group character, wiki/doc/plugins/api/character.md) ---- */

    /* Money of a character (an inventory owner: the actor, a stalker, a trader). character_money: obj:money(); 0 for
       anything else. character_give_money: obj:give_money(amount); a negative amount takes money away
       (GWP_ERROR_INVALID_STATE when the character has less, nothing changes then). character_transfer_money:
       from:transfer_money(amount, to) (GWP_ERROR_INVALID_STATE when `from` has less). A character with endless money
       (a trader of the story) never goes down, as in the engine. The new sum reaches the server object with a network
       event. GWP_ERROR_NOT_FOUND: no such online object; GWP_ERROR_INVALID_ARGUMENT: not a character.
       @group character @thread main */
    uint32_t(GWP_CALL* character_money)(GwpObjectId id);
    GwpResult(GWP_CALL* character_give_money)(const GwpPlugin* self, GwpObjectId id, int32_t amount);
    GwpResult(GWP_CALL* character_transfer_money)(const GwpPlugin* self, GwpObjectId from, GwpObjectId to,
        uint32_t amount);

    /* Rank, reputation and name of a character. Reading - the online character: character_rank /
       character_reputation: obj:character_rank() / character_reputation() (a monster has a rank: the one of its ltx);
       INT32_MIN for anything else (an offline character: alife_object_rank). character_name: obj:character_name() -
       the name the game shows, the full length without the zero (`out` may be NULL); 0 for anything else.
       Writing - online or offline: an online character changes as with set_character_rank / set_character_reputation /
       set_character_name of Lua, and its server object with it, so the value stays with the save and the next spawn;
       an offline one changes on its server object. character_set_community: set_character_community(community) of
       Lua - the team of the engine and the community change, the squad and the group stay (a dead character keeps its
       team); offline: the community and the team of the server object.
       GWP_ERROR_NOT_FOUND: neither online nor in ALife; GWP_ERROR_INVALID_ARGUMENT: not a character (a monster's rank
       comes from its ltx), an empty name, an unknown community.
       @group character @thread main */
    int32_t(GWP_CALL* character_rank)(GwpObjectId id);
    int32_t(GWP_CALL* character_reputation)(GwpObjectId id);
    uint32_t(GWP_CALL* character_name)(GwpObjectId id, char* out, uint32_t cap);
    GwpResult(GWP_CALL* character_set_rank)(const GwpPlugin* self, GwpObjectId id, int32_t rank);
    GwpResult(GWP_CALL* character_set_reputation)(const GwpPlugin* self, GwpObjectId id, int32_t reputation);
    GwpResult(GWP_CALL* character_set_name)(const GwpPlugin* self, GwpObjectId id, const char* name);
    GwpResult(GWP_CALL* character_set_community)(const GwpPlugin* self, GwpObjectId id, const char* community);

    /* Personal goodwill of one character to another (the relation registry of ALife: online and offline characters,
       the ids of the actor, stalkers, traders; the numbers of configs\creatures\game_relations.ltx).
       character_goodwill: from:goodwill(to); 0 (neutral) when none was set. character_set_goodwill:
       from:set_goodwill(value, to), clamped to personal_goodwill_limits. character_force_goodwill:
       from:force_set_goodwill(value, to) - the personal goodwill is chosen so that it plus the community parts equals
       `value`, not clamped to the limits (both must be ALife characters). character_attitude: from:general_goodwill(to) - the sum the engine
       decides friend / enemy by (personal + reputation + rank + community goodwill + community relation); online
       characters only, 0 otherwise. character_set_relation: from:set_relation(relation, to) - the personal goodwill
       becomes goodwill_friend / goodwill_neutal / goodwill_enemy of game_relations.ltx (GWP_RELATION_FRIEND, _NEUTRAL,
       _ENEMY; WORST_ENEMY is refused: Lua crashes there). Every real change is the event relation_on_changed.
       Goodwill values: -1000000 .. 1000000 (far outside the limits of the configs: the sums of the engine must not
       overflow). The money, rank and attitude functions take characters only: a monster has an inventory but no
       character profile. GWP_ERROR_NOT_FOUND: no such ALife object; GWP_ERROR_INVALID_ARGUMENT: not a character,
       from == to, a value out of range.
       @group character @thread main */
    int32_t(GWP_CALL* character_goodwill)(GwpObjectId from, GwpObjectId to);
    GwpResult(GWP_CALL* character_set_goodwill)(const GwpPlugin* self, GwpObjectId from, GwpObjectId to,
        int32_t goodwill);
    GwpResult(GWP_CALL* character_force_goodwill)(const GwpPlugin* self, GwpObjectId from, GwpObjectId to,
        int32_t goodwill);
    int32_t(GWP_CALL* character_attitude)(GwpObjectId from, GwpObjectId to);
    GwpResult(GWP_CALL* character_set_relation)(const GwpPlugin* self, GwpObjectId from, GwpObjectId to,
        uint32_t relation);

    /* Communities (factions) by name: "stalker", "dolg", "actor_dolg", ... - the communities of game_relations.ltx.
       community_exists: 1 for a known name. community_goodwill: relation_registry.community_goodwill(community, who) -
       how the community treats a character (online or offline); 0 for an unknown community.
       community_set_goodwill / community_change_goodwill: set_community_goodwill / change_community_goodwill, clamped to
       community_goodwill_limits (values and deltas -1000000 .. 1000000). community_relation: relation_registry.community_relation(from, to) - one direction:
       (a, b) and (b, a) are two values; 0 for an unknown name. community_set_relation: set_community_relation. The
       table of community relations is NOT saved by the engine: it is built from the configs once, when the game
       starts, and lives until the game quits - a load neither saves nor restores it, so a change stays across loads
       of other saves until reset_relations_to_default. The scripts of the mod keep their own changes and set them
       again when the actor spawns; a plugin keeps its changes itself (save_write) and sets them again at
       actor_on_spawn. Every real change is the event relation_on_changed. GWP_ERROR_INVALID_ARGUMENT: an unknown
       community, not a character; GWP_ERROR_INVALID_STATE: no game (the goodwill functions; the relation table works
       without one).
       @group character @thread main */
    int(GWP_CALL* community_exists)(const char* community);
    int32_t(GWP_CALL* community_goodwill)(const char* community, GwpObjectId who);
    GwpResult(GWP_CALL* community_set_goodwill)(const GwpPlugin* self, const char* community, GwpObjectId who,
        int32_t goodwill);
    GwpResult(GWP_CALL* community_change_goodwill)(const GwpPlugin* self, const char* community, GwpObjectId who,
        int32_t delta);
    int32_t(GWP_CALL* community_relation)(const char* from, const char* to);
    GwpResult(GWP_CALL* community_set_relation)(const GwpPlugin* self, const char* from, const char* to,
        int32_t goodwill);

    /* Characteristics of a living creature (the actor, a stalker, a monster), one GWP_COND_* each.
       creature_condition: the value now (obj.health, obj.power, obj.radiation, obj.bleeding, obj.psy_health,
       obj.morale of Lua; satiety - the real one of the actor: Lua's obj.satiety always answers 1). -1.f for an unknown
       characteristic or anything but an online creature (satiety and alcohol: anything but the actor).
       creature_set_condition: sets the value at once, clamped to its range. Health at 0 or below kills the creature at
       its next update (set_health_ex of Lua). Bleeding: only 0 - every wound closes (GWP_ERROR_NOT_SUPPORTED
       otherwise: wounds come from hits). What the hits and Lua changed this frame is added on top at the next update.
       creature_change_condition: the Lua way, `obj.health = delta`: the delta is added at the next condition update of
       the creature (health, stamina, radiation, psy health, morale; a negative health delta is ignored for an
       invulnerable creature - god mode, as in Lua); bleeding: a negative delta closes the wounds by that much (Lua
       `obj.bleeding = amount` heals `amount`), a positive one is GWP_ERROR_NOT_SUPPORTED; satiety, alcohol and the
       maximums change at once.
       GWP_ERROR_NOT_FOUND: no such online object; GWP_ERROR_INVALID_ARGUMENT: not a creature, an unknown
       characteristic, an actor-only one on another creature, NaN or infinity, a maximum of health that would drop to
       0 or below; GWP_ERROR_INVALID_STATE: a dead creature.
       @group character @thread main */
    float(GWP_CALL* creature_condition)(GwpObjectId id, uint32_t condition);
    GwpResult(GWP_CALL* creature_set_condition)(const GwpPlugin* self, GwpObjectId id, uint32_t condition,
        float value);
    GwpResult(GWP_CALL* creature_change_condition)(const GwpPlugin* self, GwpObjectId id, uint32_t condition,
        float delta);

    /* --- Zones, physics, models of objects (group object_ext, wiki/doc/plugins/api/object_ext.md) ----------- */

    /* Space restrictors: the zones of the logic (script zones), anomalies, the zones of smart terrains (not level
       changers: they are no restrictors). zone_inside: zone:inside(position) of Lua - a sphere of `radius` around the
       position touches the shape of the zone (radius 0: the Lua default, a millimeter). object_in_zone: the position of
       an online object is inside the zone (zone:inside(obj:position()), what the scripts do). 0 for a zone that is
       not an online restrictor or a missing object.
       @group object_ext @thread main */
    int(GWP_CALL* zone_inside)(GwpObjectId zone, const float position[3], float radius);
    int(GWP_CALL* object_in_zone)(GwpObjectId zone, GwpObjectId id);

    /* Pushes an online object: `dir` (normalized here) times `power` is the impulse, kg*m/s. An object with an active
       physics shell (a physics object, an item lying in the world, a corpse) gets it into the element of `bone` (NULL
       or "": the root element; a bone without an element of its own: the element of its nearest parent bone that has
       one); a living creature through its character controller (always the whole body, `bone` is
       only checked: the creature loses control for a moment, one impulse at a time - a second one while the first
       acts is dropped by the engine). When the physics step owns the world (the logic runs beside it), the impulse is
       applied right after the step, as the engine does with its own hits (GWP_OK then means "queued": an object gone
       by then is skipped silently). GWP_ERROR_NOT_FOUND: no such online object; GWP_ERROR_INVALID_ARGUMENT: a zero
       dir, NaN or infinity, a bone the model has not, a bone of a shell without a skeleton (the simple box shell of
       a physics object: use the root); GWP_ERROR_INVALID_STATE: no active shell and no character controller (an item
       inside an inventory, a sleeping static object, the actor in a car or at a mounted gun).
       @group object_ext @thread main */
    GwpResult(GWP_CALL* physics_apply_impulse)(const GwpPlugin* self, GwpObjectId id, const float dir[3], float power,
        const char* bone);

    /* Changes the model of an object: obj:set_visual_name(visual) of Lua plus its server object, so the model stays
       with the save and the next spawn (Lua changes only the online object). `visual` is the path under meshes without
       the extension ("dynamics\box\box_wood_01"). An online object changes at once - the actor completely (bones,
       animations, physics, as an outfit does; an outfit with its own actor visual puts its model back when it is put
       on or taken off and at a load); any other creature (a stalker, a trader, a monster, alive or dead)
       cannot while online (the engine keeps its skeleton in the animation and physics managers, a corpse in its
       ragdoll), and so cannot an object with a physics shell (a box, a barrel, an item on the ground, a car: the shell
       keeps the bones of the old model): change it while it is offline, it comes online with the new model. An offline object changes its
       server object only. GWP_ERROR_NOT_FOUND: neither online nor in ALife, no such model file;
       GWP_ERROR_INVALID_ARGUMENT: an empty or too long name, an object without a model; GWP_ERROR_INVALID_STATE: an
       offline object without a game; GWP_ERROR_NOT_SUPPORTED: an online creature other than the actor, an online
       object with a physics shell.
       @group object_ext @thread main */
    GwpResult(GWP_CALL* object_set_visual)(const GwpPlugin* self, GwpObjectId id, const char* visual);
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
    uint32_t api_min;   /* minimal engine api_version required; 0 = the version of the header the plugin was built
                           with (api_built): a plugin that checks GWP_API_HAS for the newer functions sets it lower */

    /* Lifecycle. All optional (may be NULL). Called on the main thread.
       on_unload is called before every unload, also at the frame after a crash of the plugin: then the state of
       the plugin may be broken - a lock taken when it crashed is still held (a crash unwinds without destructors),
       so never wait for one there (try_lock, a join with a timeout); stopping the own threads is what it is for. */
    void(GWP_CALL* on_unload)(void* user);

    /* Opaque pointer the engine passes back to on_unload (the other callbacks take their own `user`). */
    void* user;

    /* GWP_API_VERSION of the header the plugin was built with (the engine logs it next to its own). Fields are
       appended below this one only. */
    uint32_t api_built;
    uint32_t reserved; /* 0 */
} GwpPluginDesc;

/* The ABI layout, checked at compile time on both sides. Offsets hold on every platform; the sizes below are the
   64-bit ones (32-bit targets differ by the size of the pointers and, on i386 System V, by the 4-byte alignment of
   the 64-bit members: sizeof(GwpValue) is 20 there). */
GWP_STATIC_ASSERT(sizeof(GwpObjectId) == 2, "GwpObjectId is 16-bit");
GWP_STATIC_ASSERT(offsetof(GwpValue, u) == 8, "GwpValue: the union follows type and reserved");
GWP_STATIC_ASSERT(offsetof(GwpEngineApi, log) == 16, "GwpEngineApi: four u32 of the header, then the functions");
GWP_STATIC_ASSERT(offsetof(GwpPluginDesc, api_min) == 8, "GwpPluginDesc: the header is three u32");
GWP_STATIC_ASSERT(offsetof(GwpEvent, id) == 8, "GwpEvent: size and flags first");
GWP_STATIC_ASSERT(offsetof(GwpSubscribeOptions, flags) == 4, "GwpSubscribeOptions: size first");
GWP_STATIC_ASSERT(offsetof(GwpBinderVTable, on_reinit) == 8, "GwpBinderVTable: size and reserved first");
GWP_STATIC_ASSERT(offsetof(GwpActionVTable, initialize) == 8, "GwpActionVTable: size and reserved first");
GWP_STATIC_ASSERT(offsetof(GwpDanger, type) == 4, "GwpDanger: size first");
GWP_STATIC_ASSERT(offsetof(GwpSightParams, sight_type) == 4, "GwpSightParams: size first");
GWP_STATIC_ASSERT(sizeof(GwpBinderUpdate) == 8, "GwpBinderUpdate: id, reserved, dt_ms");
#if defined(_WIN64) || defined(__LP64__)
GWP_STATIC_ASSERT(sizeof(GwpValue) == 24, "GwpValue: 8 bytes of header and a 16-byte union");
GWP_STATIC_ASSERT(sizeof(GwpEvent) == 40, "GwpEvent: size, flags, id, argc, name, argv, result");
GWP_STATIC_ASSERT(offsetof(GwpPluginDesc, api_built) == 32, "GwpPluginDesc: api_built after user");
GWP_STATIC_ASSERT(sizeof(GwpSubscribeOptions) == 32, "GwpSubscribeOptions: no tail padding (reserved2)");
#endif

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
