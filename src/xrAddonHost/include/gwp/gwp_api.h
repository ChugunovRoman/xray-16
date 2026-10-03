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
   All are called on the main thread; `user` is the pointer given to binder_register. */
typedef struct GwpBinderVTable
{
    uint32_t size;     /* sizeof(GwpBinderVTable) of the plugin: fields may be appended in later versions */
    uint32_t reserved; /* 0 */
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
       the scheduler pass, only when there is at least one record. The array lives until the call returns. */
    void(GWP_CALL* on_update)(void* user, uint32_t count, const GwpBinderUpdate* updates);
} GwpBinderVTable;

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
   member, a static) - not a local buffer or a local std::string of the function, which are gone by then. */
typedef void(GWP_CALL* GwpExportFn)(void* user, uint32_t argc, const GwpValue* argv, GwpValue* result);

/* ------------------------------------------------------------------------- */
/* NPC state (group npc)                                                      */
/* ------------------------------------------------------------------------- */

/* What the NPC fears most now (npc_best_danger); the fields of Lua danger_object. */
typedef struct GwpDanger
{
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

/* The numbers are the engine's own; Lua scripts see the same ones in the tables named after each line. */

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

/* A sound of the sound player of a stalker (StalkerSpace::EStalkerSounds; Lua stalker_ids.sound_*), for
   npc_play_sound. Only the ones the scheme actions play are named here; any other number of the enum passes too. */
#define GWP_STALKER_SOUND_ALARM 4u /* stalker_ids.sound_alarm */

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

/* Hit type, object_hit (ALife::EHitType; Lua hit.*). */
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

/* What a stalker looks at (npc_sight_params); the fields of Lua CSightParams. */
typedef struct GwpSightParams
{
    uint32_t sight_type; /* GWP_SIGHT_* */
    GwpObjectId object;  /* the object it looks at, GWP_INVALID_OBJECT_ID when none */
    uint16_t reserved;
    float vector[3];     /* the direction or the point of the sight type */
} GwpSightParams;

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

    /* Binders of game objects (wiki/doc/plugins/api/binders.md). binder_register attaches the callbacks of
       `vtable` to every object whose class id equals class_id (up to 8 characters, "AI_STL_S": the text the
       engine registers the class under, see object_class_id) and whose section matches section_mask ('*' and '?'
       wildcards, "wpn_*", case does not matter). NULL means "any" for either; one of the two is required.
       Objects already online get on_spawn right away. The vtable is copied, `user` comes back to every callback.
       binder_unregister sends on_destroy for the bound objects, then forgets the binder; the engine does the same
       silently when the plugin is unloaded. GWP_INVALID_BINDER_ID: bad argument, too many binders (64), or the
       plugin already crashed in a binder callback.
       @group binders @thread main */
    GwpBinderId(GWP_CALL* binder_register)(const GwpPlugin* self, const char* class_id, const char* section_mask,
        const GwpBinderVTable* vtable, void* user);
    GwpResult(GWP_CALL* binder_unregister)(const GwpPlugin* self, GwpBinderId binder);

    /* How many online objects the binder is attached to now; 0 for an unknown binder.
       @group binders @thread main */
    uint32_t(GWP_CALL* binder_object_count)(GwpBinderId binder);

    /* Class id of an online object as text: up to 8 characters ("AI_STL_S", "SM_DOG_S"), the form binder_register
       takes. `out` must hold 9 bytes; it gets an empty string and the result is 0 when there is no such online
       object. Unlike object_clsid, the text is the same in every build and with every set of mods.
       @group objects @thread main */
    int(GWP_CALL* object_class_id)(GwpObjectId id, char out[9]);

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
       bool, int, number and string only); the subtable of a "sub.field" key must exist on the Lua side already.
       Plugins write only the fields they own; the scripts do not expect their own fields to change under them.
       @group storage @thread main */
    int(GWP_CALL* storage_get)(GwpStorageHandle handle, uint32_t key, GwpValue* out);
    GwpResult(GWP_CALL* storage_set)(const GwpPlugin* self, GwpStorageHandle handle, uint32_t key,
        const GwpValue* value);

    /* Native voices in two decisions of the engine (wiki/doc/plugins/api/callbacks.md). They are asked for every
       NPC (stalkers and monsters); filter by id on the plugin side. The enemy decision is cached by the engine per
       NPC and enemy for 250 ms: after a plugin changes its mind, npc_enemy_filter_reset drops the cache of `npc`
       (GWP_INVALID_OBJECT_ID: of every online NPC). A crash in a callback removes every callback of the plugin
       and the engine behaves as without it.
       @group callbacks @thread main */
    GwpCallbackId(GWP_CALL* enemy_filter_register)(const GwpPlugin* self, GwpEnemyFilterFn fn, void* user);
    GwpCallbackId(GWP_CALL* patrol_extrapolate_register)(const GwpPlugin* self, GwpPatrolExtrapolateFn fn,
        void* user);
    GwpResult(GWP_CALL* callback_unregister)(const GwpPlugin* self, GwpCallbackId callback);
    void(GWP_CALL* npc_enemy_filter_reset)(GwpObjectId npc);

    /* Native evaluator in the planner of every stalker (wiki/doc/plugins/api/goap.md). The Lua evaluator that
       holds `property_id` in `planner` is kept and wrapped: `mode` (GWP_EVALUATOR_*) says who decides, and
       evaluator_set_mode switches it at any time, for every NPC at once. lua_name: the name the Lua evaluator was
       created with ("eva_npc_vs_heli"); an NPC whose property holds another evaluator (a constant for the dead,
       another scheme) is left as it is. NULL accepts any Lua evaluator. Applied to the online stalkers and to every
       stalker that spawns later, at a point of the frame where no planner runs. evaluator_unregister gives the
       property back to the Lua evaluator; unloading the plugin does the same.
       GWP_INVALID_EVALUATOR_ID: a bad argument or the same property registered already (by any plugin).
       @group goap @thread main */
    GwpEvaluatorId(GWP_CALL* evaluator_register)(const GwpPlugin* self, uint32_t planner, uint32_t property_id,
        const char* lua_name, GwpEvaluatorFn fn, void* user, uint32_t mode);
    GwpResult(GWP_CALL* evaluator_unregister)(const GwpPlugin* self, GwpEvaluatorId evaluator);
    GwpResult(GWP_CALL* evaluator_set_mode)(const GwpPlugin* self, GwpEvaluatorId evaluator, uint32_t mode);
    int(GWP_CALL* evaluator_stats)(GwpEvaluatorId evaluator, GwpEvaluatorStats* out);

    /* What the NPC knows (wiki/doc/plugins/api/npc.md): the same values as the Lua methods best_enemy,
       best_danger, memory_time, memory_position, character_community. 0 / GWP_INVALID_OBJECT_ID / NULL when the
       object is not an online NPC of the right kind (monsters have no community) or does not know the other one.
       The community string lives while the NPC is online.
       @group npc @thread main */
    GwpObjectId(GWP_CALL* npc_best_enemy)(GwpObjectId npc);
    int(GWP_CALL* npc_best_danger)(GwpObjectId npc, GwpDanger* out);
    uint32_t(GWP_CALL* npc_memory_time)(GwpObjectId npc, GwpObjectId other);
    int(GWP_CALL* npc_memory_position)(GwpObjectId npc, GwpObjectId other, float out_xyz[3]);
    const char*(GWP_CALL* npc_community)(GwpObjectId npc);

    /* Calls a Lua function by name: "module.function" or a global "function" (wiki/doc/plugins/api/script.md).
       Arguments and the result are GwpValue as for events: a GWP_T_OBJECT argument arrives in Lua as the game
       object, a game object result comes back as GWP_T_OBJECT. `result` may be NULL; a string result lives until
       the next script_call. GWP_ERROR when there is no such function or it raised an error (logged, with the
       message); calls nested deeper than 16 are refused. About a microsecond per call plus the Lua function:
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

    /* More of what the NPC knows (wave W2-1): the item in its hands (npc:active_item(), an inventory owner only)
       and the objects it touches now (npc:iterate_feel_touch: what is close enough to use, open, pick up).
       npc_feel_touch writes up to `max` ids and returns how many there are in all (call again with a bigger array
       when the result is above max).
       @group npc @thread main */
    GwpObjectId(GWP_CALL* npc_active_item)(GwpObjectId npc);
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
       the plugin. planner_update and planner_destroy are refused from a callback of the same planner.
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
       game_object.*_path in Lua). A setter returns GWP_ERROR_INVALID_ARGUMENT for an object that is not an online
       stalker or a value outside its list; a getter returns what Lua gets then (GWP_MOVEMENT_STAND, GWP_BODY_STAND,
       GWP_MENTAL_DANGER), without the script error Lua logs.
       @group npc_body @thread main */
    GwpResult(GWP_CALL* npc_set_movement_type)(GwpObjectId npc, uint32_t movement_type);
    GwpResult(GWP_CALL* npc_set_body_state)(GwpObjectId npc, uint32_t body_state);
    GwpResult(GWP_CALL* npc_set_mental_state)(GwpObjectId npc, uint32_t mental_state);
    GwpResult(GWP_CALL* npc_set_path_type)(GwpObjectId npc, uint32_t path_type);
    uint32_t(GWP_CALL* npc_movement_type)(GwpObjectId npc);
    uint32_t(GWP_CALL* npc_target_movement_type)(GwpObjectId npc);
    uint32_t(GWP_CALL* npc_target_body_state)(GwpObjectId npc);
    uint32_t(GWP_CALL* npc_target_mental_state)(GwpObjectId npc);

    /* Any NPC (a stalker or a monster): npc:is_body_turning() - the body (for a stalker also the head) has not
       reached its target direction yet; npc:movement_enabled() and movement_enabled(enable) - the NPC may walk at
       all. 0 / GWP_ERROR_INVALID_ARGUMENT for anything but an online NPC.
       @group npc_body @thread main */
    int(GWP_CALL* npc_is_body_turning)(GwpObjectId npc);
    int(GWP_CALL* npc_movement_enabled)(GwpObjectId npc);
    GwpResult(GWP_CALL* npc_enable_movement)(GwpObjectId npc, int enable);

    /* A stalker only: npc:special_danger_move() - the danger variant of the walk animations (the state manager sets
       it for the states that have special_danger_move), and npc:inactualize_patrol_path() - the patrol path is
       rebuilt on the next update (the state manager calls it before switching to a level path when it goes idle).
       0 / GWP_ERROR_INVALID_ARGUMENT for anything but an online stalker.
       @group npc_body @thread main */
    int(GWP_CALL* npc_special_danger_move)(GwpObjectId npc);
    GwpResult(GWP_CALL* npc_set_special_danger_move)(GwpObjectId npc, int value);
    GwpResult(GWP_CALL* npc_inactualize_patrol_path)(GwpObjectId npc);

    /* object_level_vertex_id: obj:level_vertex_id(), the AI graph vertex the engine keeps for the object (not
       necessarily the vertex under its position: level_vertex_id(position) gives that one). GWP_INVALID_LEVEL_VERTEX
       without such an online object.
       object_hit: obj:hit(h) - a hit of type GWP_HIT_* with `power` and `impulse` from the online object `who` (the
       object itself for a self hit), in direction `dir`, into the bone `bone` (NULL or "": bone 0). Sent as the
       GE_HIT network event, like the Lua call. GWP_ERROR_INVALID_ARGUMENT for a missing object, `who` or `dir`, an
       unknown type, or a bone name on an object without a skeleton.
       @group objects @thread main */
    uint32_t(GWP_CALL* object_level_vertex_id)(GwpObjectId id);
    GwpResult(GWP_CALL* object_hit)(GwpObjectId id, GwpObjectId who, uint32_t hit_type, float power, float impulse,
        const float dir[3], const char* bone);

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
       GWP_ERROR_INVALID_ARGUMENT for an object that is not an online stalker, an unknown type or a missing target;
       GWP_SIGHT_OBJECT and GWP_SIGHT_FIRE_OBJECT are refused in the forms without an object (npc_set_sight_object
       is the call for them), GWP_SIGHT_LOOK_OVER everywhere (the engine has no sight action for it) and
       GWP_SIGHT_FIRE_POSITION everywhere but npc_set_sight_position; a vector with NaN or infinity is refused.
       @group npc_sight @thread main */
    GwpResult(GWP_CALL* npc_set_sight_type)(GwpObjectId npc, uint32_t sight_type, int torso_look, int path);
    GwpResult(GWP_CALL* npc_set_sight_position)(GwpObjectId npc, uint32_t sight_type, const float* point_xyz);
    GwpResult(GWP_CALL* npc_set_sight_direction)(GwpObjectId npc, uint32_t sight_type, const float dir[3],
        int torso_look);
    GwpResult(GWP_CALL* npc_set_sight_object)(GwpObjectId npc, GwpObjectId target, int torso_look, int fire_object,
        int no_pitch);

    /* What the stalker looks at now (npc:sight_params()): 1 with `out` filled; 0 for anything but an online stalker,
       `out` then holds what Lua gets (GWP_SIGHT_DUMMY, no object, the vector at FLT_MAX).
       @group npc_sight @thread main */
    int(GWP_CALL* npc_sight_params)(GwpObjectId npc, GwpSightParams* out);

    /* Script animations of a stalker: the queue npc:add_animation fills and the state manager plays. `name` is a
       motion of the visual; hand_usage as in Lua (the state manager always passes 1); use_movement_controller: the
       root motion of the animation moves the NPC. npc_add_animation_at is the other Lua form: the animation starts
       at `position` with `rotation` (degrees x/y/z; the state manager passes (0, yaw, 0)), local_animation as in Lua.
       GWP_ERROR when nothing was queued (no such motion, or a global animation selector is set - Lua logs a script
       error then); inside a smart cover the engine logs an error but queues it, as for Lua. npc_clear_animations
       empties the queue; npc_animation_count is its length, -1 for anything but an online stalker.
       The end of every script animation is the event npc_on_script_animation_end(npc, remaining): `remaining` is
       the queue length after the Lua callback.script_animation of the NPC (if any) has run. While that event has a
       subscriber the engine updates the animation of every stalker with a non-empty queue, visible or not, as it
       does for an NPC with a Lua script_animation callback (a GWP_SUBSCRIBE_OBJECT subscription narrows only the
       delivery, not that update).
       GWP_ERROR_INVALID_ARGUMENT for an object that is not an online stalker or a NULL name.
       @group npc_anim @thread main */
    GwpResult(GWP_CALL* npc_add_animation)(GwpObjectId npc, const char* name, int hand_usage,
        int use_movement_controller);
    GwpResult(GWP_CALL* npc_add_animation_at)(GwpObjectId npc, const char* name, int hand_usage,
        const float position[3], const float rotation[3], int local_animation);
    GwpResult(GWP_CALL* npc_clear_animations)(GwpObjectId npc);
    int32_t(GWP_CALL* npc_animation_count)(GwpObjectId npc);

    /* Weapons of a stalker as the state manager sees them: npc:best_weapon() (what it would fight with),
       npc:item_in_slot(slot) (any inventory owner; the slot numbers of Lua, with the same compatibility shift),
       weapon_strapped() / weapon_unstrapped() (on the back / in the hands, the animation finished) and
       is_weapon_going_to_be_strapped(item). GWP_INVALID_OBJECT_ID / 0 when there is no such item or the object is
       not of the right kind.
       @group inventory @thread main */
    GwpObjectId(GWP_CALL* npc_best_weapon)(GwpObjectId npc);
    GwpObjectId(GWP_CALL* npc_item_in_slot)(GwpObjectId npc, uint32_t slot);
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
    GwpResult(GWP_CALL* npc_set_item)(GwpObjectId npc, uint32_t action, GwpObjectId item, uint32_t queue_size,
        uint32_t queue_interval);
    uint32_t(GWP_CALL* npc_aim_time)(GwpObjectId npc, GwpObjectId weapon);
    GwpResult(GWP_CALL* npc_set_aim_time)(GwpObjectId npc, GwpObjectId weapon, uint32_t aim_time_ms);

    /* Smart covers of a stalker (the smartcover state of the state manager): use_smart_covers_only (the NPC takes
       cover only in smart covers), the destination cover and loophole by name (NULL or "" clears, as the Lua call
       without arguments), the fire target inside the cover (a point, NULL clears; or an online object) and
       npc_clear_smart_cover_target_selector (set_smart_cover_target_selector() without a function: the default
       target selection). npc_dest_smart_cover_name: the destination cover (get_dest_smart_cover_name), NULL or ""
       when none; the string lives until the destination changes. Is the NPC in a cover now: npc_in_smart_cover
       (group npc). 0 / NULL / GWP_ERROR_INVALID_ARGUMENT for anything but an online stalker.
       @group smart_cover @thread main */
    int(GWP_CALL* npc_use_smart_covers_only)(GwpObjectId npc);
    GwpResult(GWP_CALL* npc_set_use_smart_covers_only)(GwpObjectId npc, int value);
    GwpResult(GWP_CALL* npc_set_dest_smart_cover)(GwpObjectId npc, const char* cover_name);
    const char*(GWP_CALL* npc_dest_smart_cover_name)(GwpObjectId npc);
    GwpResult(GWP_CALL* npc_set_dest_loophole)(GwpObjectId npc, const char* loophole_name);
    GwpResult(GWP_CALL* npc_set_smart_cover_target)(GwpObjectId npc, const float* point_xyz);
    GwpResult(GWP_CALL* npc_set_smart_cover_target_object)(GwpObjectId npc, GwpObjectId target);
    GwpResult(GWP_CALL* npc_clear_smart_cover_target_selector)(GwpObjectId npc);

    /* An item in hands: item:get_state() (the state of the weapon or of another hud item: firing, reloading, ...;
       65535 for anything else, as in Lua) and item:animation_slot() (the animation slot of a hud item, 0xFFFFFFFF
       for anything else, without the script error Lua logs).
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
    int(GWP_CALL* npc_relation)(GwpObjectId npc, GwpObjectId other);

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
    float(GWP_CALL* npc_health)(GwpObjectId npc);
    float(GWP_CALL* npc_psy_health)(GwpObjectId npc);

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

    /* New functions go below this line only. */

    /* npc:is_talking() - the object is in a dialog now (an inventory owner: a stalker or the actor; the flag the
       talk dialog sets). 0 for anything but an online inventory owner, as Lua gets it, without the script error.
       @group npc @thread main */
    int(GWP_CALL* npc_is_talking)(GwpObjectId id);

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
       GWP_INVALID_ACTION_REG_ID: a bad argument or the same planner+action_id registered already (by any plugin).
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
       alife_object_clsid: the fourcc of the class id of the object (CLSID2TEXT: exactly 8 characters, the same
       bytes the saves hold - "SMRTTRRN" the script smart terrain, "ON_OFF_S" a squad, "S_ACTOR " the actor: the
       padding spaces are kept, unlike object_class_id, which cuts them), copied into out; out must hold 9 bytes. 1 when written, 0 without such an object.
       @group alife @thread main */
    int(GWP_CALL* alife_object_position)(GwpObjectId id, float out_xyz[3]);
    uint32_t(GWP_CALL* alife_object_game_vertex)(GwpObjectId id);
    uint32_t(GWP_CALL* alife_object_level_vertex)(GwpObjectId id);
    int32_t(GWP_CALL* alife_object_level_id)(GwpObjectId id);
    int(GWP_CALL* alife_object_clsid)(GwpObjectId id, char* out, uint32_t cap);
    GwpObjectId(GWP_CALL* alife_object_smart)(GwpObjectId id);

    /* smart.arrive_dist of a smart terrain: the Lua field its class reads from the ltx, the arrival radius of
       am_i_reached; -1.f without such an object or such a field.
       @group alife @thread main */
    float(GWP_CALL* alife_smart_arrive_dist)(GwpObjectId smart);

    /* The talk calls of the meet scripts (xr_meet), one engine method each - CInventoryOwner through
       CScriptGameObject, the very methods the Lua export calls. npc_enable_talk / npc_disable_talk /
       npc_stop_talk / npc_is_talk_enabled: npc:enable_talk() / :disable_talk() / :stop_talk() /
       :is_talk_enabled() - an inventory owner only (a stalker, the actor); a no-op / 0 for anything else, as
       the Lua methods do (they skip it silently too). npc_set_tip_text: npc:set_tip_text(text) - the tip over the use cross
       of any online object; NULL or "" clears it to the empty string, as the "" the meet scripts set.
       @group npc @thread main */
    void(GWP_CALL* npc_enable_talk)(GwpObjectId npc);
    void(GWP_CALL* npc_disable_talk)(GwpObjectId npc);
    void(GWP_CALL* npc_stop_talk)(GwpObjectId npc);
    int(GWP_CALL* npc_is_talk_enabled)(GwpObjectId npc);
    void(GWP_CALL* npc_set_tip_text)(GwpObjectId npc, const char* text);

    /* The path of a stalker: where it goes and how (the movement setters of the scheme actions - reach_task,
       animpoint, cover, patrol soldiers). Each calls the CScriptGameObject method the Lua call does; what Lua only
       logs is refused here with GWP_ERROR_INVALID_ARGUMENT (not an online stalker, a value outside its list, an
       invalid vertex), what Lua would crash on is refused too.
       npc_set_dest_level_vertex: set_dest_level_vertex_id - also refused for a vertex the space restrictors of the
       NPC forbid (Lua logs "not accessible by its restrictors" and keeps the old destination).
       npc_set_dest_game_vertex: set_dest_game_vertex_id. npc_set_desired_position: set_desired_position(pos), NULL
       = set_desired_position() (clears). npc_set_desired_direction: set_desired_direction(dir), NULL clears; a zero
       vector is refused (Lua logs it and sets the zero direction all the same - an engine slip not copied), any other
       is normalized once, as Lua does outside the Shadow of Chernobyl mode.
       npc_set_detail_path_type: GWP_DETAIL_PATH_*. npc_set_movement_selection_type: GWP_ALIFE_MOVEMENT_*.
       npc_path_type: GWP_PATH_* (GWP_PATH_NONE for a wrong object). npc_detail_path_type: the type the movement
       manager has (Lua's detail_path_type() always answers move.line - an engine defect not copied here).
       npc_mental_state: the current mental state, npc:mental_state() (the target one is npc_target_mental_state);
       GWP_MENTAL_DANGER for a wrong object, as Lua.
       @group npc_body @thread main */
    GwpResult(GWP_CALL* npc_set_dest_level_vertex)(GwpObjectId npc, uint32_t level_vertex);
    GwpResult(GWP_CALL* npc_set_dest_game_vertex)(GwpObjectId npc, uint32_t game_vertex);
    GwpResult(GWP_CALL* npc_set_desired_position)(GwpObjectId npc, const float* xyz);
    GwpResult(GWP_CALL* npc_set_desired_direction)(GwpObjectId npc, const float* dir);
    GwpResult(GWP_CALL* npc_set_detail_path_type)(GwpObjectId npc, uint32_t detail_path_type);
    GwpResult(GWP_CALL* npc_set_movement_selection_type)(GwpObjectId npc, uint32_t selection_type);
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
       object_game_vertex_id: obj:game_vertex_id() of any online object (the game vertex of its AI location);
       GWP_INVALID_GAME_VERTEX without such an object.
       @group npc_body @thread main */
    int(GWP_CALL* npc_accessible_position)(GwpObjectId npc, const float xyz[3]);
    uint32_t(GWP_CALL* npc_accessible_nearest)(GwpObjectId npc, const float xyz[3], float out_xyz[3]);
    uint32_t(GWP_CALL* npc_location_on_path)(GwpObjectId npc, float distance, float out_xyz[3]);
    int(GWP_CALL* npc_path_completed)(GwpObjectId npc);
    uint32_t(GWP_CALL* object_game_vertex_id)(GwpObjectId id);

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
       refused for a path the level has not (Lua asserts there) or a start / route outside GWP_PATROL_START_* /
       GWP_PATROL_ROUTE_*. npc_set_start_point: npc:set_start_point(i) - a stalker or a monster with a path that
       has the point (Lua logs "Path not specified" otherwise). With GWP_PATROL_START_POINT set the path first, the
       point after it: a new path resets the start point (without one the NPC heads for where it stands and the
       engine logs its restrictions every update). npc_patrol_point_index:
       npc:get_current_point_index() - GWP_INVALID_PATROL_POINT without a path or for anything but an online NPC.
       npc_patrol_path_name: the name of its patrol path into out (zero-terminated, cut to cap - 1), the length of
       the full name as the result; 0 without a path (Lua's patrol() logs "Path not specified" there).
       @group npc_body @thread main */
    GwpResult(GWP_CALL* npc_set_patrol_path)(GwpObjectId npc, const char* name, uint32_t start_type,
        uint32_t route_type, int random);
    GwpResult(GWP_CALL* npc_set_start_point)(GwpObjectId npc, uint32_t point);
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
       max_stop, min_stop) - a sound of the sound player (GWP_STALKER_SOUND_*), times in ms as Lua passes them; a
       stalker or a monster; a type the NPC has no sound of is a no-op, as in Lua (GWP_OK). npc_sniper_update_rate /
       npc_set_sniper_update_rate: npc:sniper_update_rate() / (value) - the faster visibility update of a sniper
       (the camper sniper turns it on while it scans); a stalker.
       @group npc_body @thread main */
    uint32_t(GWP_CALL* npc_body_state)(GwpObjectId npc);
    GwpResult(GWP_CALL* npc_play_sound)(GwpObjectId npc, uint32_t sound_type, uint32_t max_start_ms,
        uint32_t min_start_ms, uint32_t max_stop_ms, uint32_t min_stop_ms);
    int(GWP_CALL* npc_sniper_update_rate)(GwpObjectId npc);
    GwpResult(GWP_CALL* npc_set_sniper_update_rate)(GwpObjectId npc, int value);

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

    /* Lifecycle. All optional (may be NULL). Called on the main thread.
       on_unload is called before every unload, also at the frame after a crash of the plugin: then the state of
       the plugin may be broken - a lock taken when it crashed is still held (a crash unwinds without destructors),
       so never wait for one there (try_lock, a join with a timeout); stopping the own threads is what it is for. */
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
