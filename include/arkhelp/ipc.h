// ArkHelper IPC contract v11 (mapping names retain their original .v1 suffix).
//
// Two named shared-memory sections connect the in-game provider plugin (writer of the battle
// snapshot, reader of commands) with the external ArkHelper.exe overlay (writer of commands, reader
// of the snapshot). Both sections are plain POD with a fixed layout; the protocol is a classic
// single-writer seqlock, so the render side never blocks the game thread and vice versa.
//
// Layout rules:
//  * Explicit-width integer types only, no pointers, no std::string, no STL containers.
//  * `magic`/`version`/`size` are written once at creation; readers validate them before use.
//  * `sequence` is even when stable and odd while a writer is mutating the payload. Writers do
//    ++sequence (release), write payload, ++sequence (release). Readers sample sequence, copy the
//    payload, sample again and retry a bounded number of times when it changed or was odd.
//  * Strings are UTF-8, always zero terminated within their fixed buffer.
//  * All screen coordinates are physical pixels in the game client area (top-left origin), already
//    projected by the provider on the game thread. Entries whose `flags` lack the valid bit are
//    skipped by the renderer.
#pragma once

#include <cstddef>
#include <cstdint>

namespace arkhelp::ipc {

inline constexpr std::uint32_t kVersion = 11;
inline constexpr std::uint32_t kBattleMagic = 0x42484841u;   // "AHHB"
inline constexpr std::uint32_t kCommandMagic = 0x43484841u;  // "AHHC"

inline constexpr wchar_t kBattleMappingName[] = L"Local\\ArkHelp.Battle.v1";
inline constexpr wchar_t kCommandMappingName[] = L"Local\\ArkHelp.Command.v1";

inline constexpr std::size_t kBattleMappingBytes = 256u * 1024u;
inline constexpr std::size_t kCommandMappingBytes = 4u * 1024u;

inline constexpr std::uint32_t kMaxSpawns = 48;
inline constexpr std::uint32_t kMaxEnemies = 48;
inline constexpr std::uint32_t kMaxUnits = 64;
inline constexpr std::uint32_t kRangePointCount = 16;
inline constexpr std::uint32_t kMaxDamageRows = 64;
inline constexpr std::uint32_t kMaxPaths = kMaxUnits;
inline constexpr std::uint32_t kMaxPathPoints = 96;
inline constexpr std::uint32_t kIdCapacity = 96;
inline constexpr std::uint32_t kNameCapacity = 64;
inline constexpr std::uint32_t kLevelIdCapacity = 128;

// Path targets reserve the high bit for a route preview. Live instance IDs keep
// that bit clear; battle_serial independently validates either kind of target.
inline constexpr std::uint64_t kSpawnPathTag = std::uint64_t{1} << 63;
inline constexpr std::uint64_t spawn_path_id(std::uint32_t route) {
    return route < 65536 ? kSpawnPathTag | (static_cast<std::uint64_t>(route) + 1) : 0;
}
inline constexpr bool is_spawn_path(std::uint64_t id) {
    return id > kSpawnPathTag && id <= (kSpawnPathTag | 65536);
}
inline constexpr std::uint32_t spawn_path_route(std::uint64_t id) {
    return static_cast<std::uint32_t>((id & ~kSpawnPathTag) - 1);
}

// Feature switches: the overlay renders only the features selected in the command payload.
enum Feature : std::uint32_t {
    kFeatureSpawnTimer = 1u << 0,   // countdown numbers near spawn points
    kFeatureSpawnCard = 1u << 1,    // single-enemy card with switchable entry
    kFeatureDamageNumbers = 1u << 2,  // per-operator damage near the operator
    kFeatureHpBars = 1u << 3,       // enemy HP bars (M3 data)
    kFeatureTopBar = 1u << 4,       // minimal battle time bar
    kFeatureDebugPanel = 1u << 5,   // provider/overlay diagnostics
    kFeatureWiki = 1u << 6,         // current battle enemy data panel
    kFeatureSpawnList = 1u << 7,    // spawn countdown list panel (independent from timer)
    kFeatureEnemyRange = 1u << 8,   // current circular ranged attack areas, opt-in
    kFeatureOperatorStats = 1u << 9,
    kFeatureEnemyPaths = 1u << 10,  // requested live remaining routes, opt-in
    kFeatureEnemyPrts = 1u << 11,   // independent reference panel, same enemy roster
    kFeatureEnemyStats = 1u << 12,  // hover-only attributes for the nearest live enemy
};

enum SpawnFlags : std::uint32_t {
    kSpawnRandom = 1u << 0,        // random spawn group; timing may differ from the plan
    kSpawnScreenValid = 1u << 1,   // `screen` holds a usable projection
    kSpawnPending = 1u << 2,       // waiting for a later phase; no numeric countdown yet
};

enum EnemyFlags : std::uint32_t {
    kEnemyValid = 1u << 0,         // projection and stats are usable
    kEnemyAlive = 1u << 1,
    kEnemyRuntime = 1u << 2,       // values sampled from this live instance, after mode modifiers
    kEnemyReference = 1u << 3,     // level definition only; never present as current combat values
};

enum EnemyStat : std::uint32_t {
    kEnemyHp = 1u << 0,
    kEnemyMaxHp = 1u << 1,
    kEnemyAtk = 1u << 2,
    kEnemyDef = 1u << 3,
    kEnemyMagicResistance = 1u << 4,
    kEnemyMoveSpeed = 1u << 5,
    kEnemyAttackSpeed = 1u << 6,
    kEnemyBaseAttackTime = 1u << 7,
    kEnemyWeight = 1u << 8,
    kEnemyBlockCount = 1u << 9,
    kEnemyLifePointReduce = 1u << 10,
};

enum DamageFlags : std::uint32_t {
    kDamageValid = 1u << 0,
    kDamageScreenValid = 1u << 1,
    kDamageDevice = 1u << 2,       // independent stage device; summons remain attributed to their owner
};

enum DamageType : std::uint32_t {
    kDamageNone = 0,
    kDamagePhysical = 1,
    kDamageArts = 2,
    kDamageTrue = 3,
    kDamageHeal = 4,
    kDamageElemental = 5,
};

// Stable metric identities. A column's visibility and sort selection never depend on its position.
enum StatMetric : std::uint32_t {
    kStatDamage = 0, kStatPhysical, kStatArts, kStatTrue, kStatHeal, kStatElemental,
    kStatCost, kStatBuildup, kStatTaken, kStatShield, kStatDps, kStatShare, kStatCount
};

enum BattleFlags : std::uint32_t {
    kBattleLevelKnown = 1u << 0,
    kBattleStale = 1u << 1,
    kBattlePaused = 1u << 2,   // live BattleController pause getter; never inferred from stopped frames
    kBattleRuntimeStage = 1u << 3, // current stage definition was read from the running game
    kBattleSpawnPlan = 1u << 4,    // a current stage plan is available (still subject to game conditions)
    kBattleEnemiesTruncated = 1u << 5,
    kBattleSpawnConditional = 1u << 6, // random/blocking/branching conditions affect planned times
};

struct Vec2 {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

struct Float2 { float x = 0, y = 0; };

enum UnitFlags : std::uint32_t {
    kUnitValid = 1u << 0,
    kUnitRangeValid = 1u << 1,       // verified live ranged attack and single circular range
    kUnitHpValid = 1u << 2,
    kUnitGroundValid = 1u << 3,      // ground center is independent of the label anchor
};

struct SpawnEntry {
    std::uint32_t planned_frame = 0;   // logic frame at 30 fps; phase identifier when kSpawnPending
    std::uint32_t count = 0;
    std::uint32_t route_index = 0;
    std::uint32_t flags = 0;           // SpawnFlags
    Vec2 screen;                        // projected spawn point when kSpawnScreenValid
    float remaining_seconds = 0.f;      // seconds until spawn; -1 when kSpawnPending
    char enemy_key[kIdCapacity] = {};
    char enemy_name[kNameCapacity] = {};
};

struct EnemyEntry {
    std::uint32_t flags = 0;            // EnemyFlags
    std::uint32_t reserved = 0;
    Vec2 screen;
    std::int64_t hp = 0;
    std::int64_t max_hp = 0;            // source described by flags; availability by stats_mask
    std::int32_t atk = 0;
    std::int32_t def = 0;
    std::int32_t magic_resistance = 0;  // x1000
    std::int32_t move_speed = 0;        // x1000
    std::int32_t attack_speed = 0;
    std::int32_t base_attack_time = 0;  // x1000
    std::int32_t weight = 0;            // x1000
    std::int32_t block_cnt = 0;
    std::int32_t life_point_reduce = 0;
    std::uint32_t damage_type_mask = 0; // bit (1 << DamageType)
    std::uint32_t stats_mask = 0;       // EnemyStat: missing properties must display as unavailable
    char enemy_key[kIdCapacity] = {};
    char enemy_name[kNameCapacity] = {};
    std::uint64_t tracking_id = 0;      // opaque live instance identifier; scoped to battle_serial
};

struct DamageEntry {
    std::uint32_t flags = 0;            // DamageFlags
    std::uint32_t damage_type = 0;      // DamageType
    Vec2 screen;                        // operator position on screen when kDamageScreenValid
    std::int64_t total_damage = 0;      // accumulated actual damage this battle
    float dps = 0.f;
    float share = 0.f;                  // 0..1 share of the team total
    char actor_id[kIdCapacity] = {};
    char actor_name[kNameCapacity] = {};
    double metrics[kStatCount] = {};  // actual damage/healing; cost remains fractional when needed
    std::uint32_t metric_mask = 0;     // supported metrics; unsupported values must render as “—”
    std::uint32_t reserved = 0;
};

// Live enemy unit sample: overlay-space pixel position plus current/max HP. The provider converts
// the capture layer's viewport coordinates with the game window client rect.
struct UnitEntry {
    float x = 0.0f;
    float y = 0.0f;
    float hp = 0.0f;
    float max_hp = 0.0f;
    std::uint32_t flags = 0;
    std::uint64_t tracking_id = 0;      // opaque per-instance identity; never an array index
    float ground_x = 0, ground_y = 0;   // true map-plane center, client pixels
    float range_radius = 0;             // current instance radius, in map units
    std::uint32_t range_point_count = 0;
    Float2 range_points[kRangePointCount]{}; // actual projected perimeter, not a screen-space circle
};

enum PathPointFlags : std::uint32_t {
    kPathMoveTo = 1u << 0,          // start a new segment; never connect across a teleport
};
enum PathFlags : std::uint32_t {
    kPathValid = 1u << 0,
    kPathComplete = 1u << 1,        // the true remaining route end is included
    kPathTruncated = 1u << 2,       // a prefix only; never present its last point as the route end
};
struct PathPoint {
    float x = 0, y = 0;
    std::uint32_t flags = 0;
};
struct PathEntry {
    std::uint64_t tracking_id = 0; // live instance ID or spawn_path_id(route)
    std::uint64_t sampled_ms = 0;
    std::uint32_t battle_serial = 0;
    std::uint32_t flags = 0;
    std::uint32_t point_count = 0;
    std::uint32_t route_revision = 0;
    PathPoint points[kMaxPathPoints]{};
};

// Battle snapshot: written by the in-game provider, read by ArkHelper.exe.
struct BattleSnapshot {
    std::uint32_t magic = 0;            // kBattleMagic
    std::uint32_t version = 0;          // kVersion
    std::uint32_t size = 0;             // sizeof(BattleSnapshot) of the writer
    std::uint32_t sequence = 0;         // seqlock
    std::uint64_t published_ms = 0;     // writer GetTickCount64()
    std::uint32_t battle_serial = 0;    // increments on every new battle
    std::uint32_t in_battle = 0;
    std::int32_t battle_state = -1;     // Torappu battle m_state (2 running, 3 finished)
    std::uint32_t flags = 0;            // BattleFlags
    std::uint32_t frame = 0;            // native logical frame
    std::uint32_t spawn_count = 0;
    std::uint32_t enemy_count = 0;
    std::uint32_t damage_count = 0;
    std::uint32_t unit_count = 0;
    std::uint32_t path_count = 0;
    double time_seconds = 0.0;          // fixedPlayTime / 2^32
    std::uint64_t unit_sample_ms = 0;   // actual game-thread position sample, not IPC publication
    double unit_sample_battle_time = 0.0;
    char level_id[kLevelIdCapacity] = {};
    SpawnEntry spawns[kMaxSpawns] = {};
    EnemyEntry enemies[kMaxEnemies] = {};
    DamageEntry damage[kMaxDamageRows] = {};
    UnitEntry units[kMaxUnits] = {};
    PathEntry paths[kMaxPaths] = {};
    double team_metrics[kStatCount] = {}; // all captured actors, including rows outside the IPC limit
    std::uint32_t team_metric_mask = 0;
    std::uint32_t stats_flags = 0;        // bit0: partial/lost events; bit1: actor rows truncated
    std::uint32_t operator_count = 0;
    std::uint32_t operator_reserved = 0;
    DamageEntry operators[kMaxUnits]{}; // Per-instance contributions and anchors; never added to team totals.
    std::uint64_t operator_ids[kMaxUnits]{};
    double device_metrics[kStatCount] = {}; // captured devices, including rows beyond the display limit
};

enum CommandFlags : std::uint32_t {
    kCommandOverlayEnabled = 1u << 0,
    kCommandReloadDataPack = 1u << 1,
};

// Command payload: written by ArkHelper.exe, read by the provider.
struct CommandState {
    std::uint32_t magic = 0;            // kCommandMagic
    std::uint32_t version = 0;          // kVersion
    std::uint32_t size = 0;
    std::uint32_t sequence = 0;         // seqlock
    std::uint32_t feature_mask = 0;     // Feature bits
    std::uint32_t card_index = 0;       // selected spawn entry for the card
    std::uint32_t revision = 0;         // bumped on every change by ArkHelper.exe
    float ui_scale = 1.0f;
    std::uint32_t flags = 0;            // CommandFlags; enabled while feature_mask is nonzero
    std::uint32_t path_request_battle_serial = 0;
    std::uint32_t path_request_count = 0;
    std::uint64_t path_request_ids[kMaxPaths]{};
};

inline constexpr bool command_feature_enabled(const CommandState& commands, Feature feature) noexcept {
    return (commands.flags & kCommandOverlayEnabled) != 0 &&
           (commands.feature_mask & static_cast<std::uint32_t>(feature)) != 0;
}

static_assert(sizeof(Vec2) == 8, "Vec2 must stay 8 bytes");
static_assert(sizeof(SpawnEntry) == 188, "SpawnEntry layout changed; bump kVersion");
static_assert(sizeof(EnemyEntry) == 248, "EnemyEntry layout changed; bump kVersion");
static_assert(sizeof(DamageEntry) == 296, "DamageEntry layout changed; bump kVersion");
static_assert(sizeof(Float2) == 8, "Float2 layout changed");
static_assert(sizeof(UnitEntry) == 176, "UnitEntry layout changed; bump kVersion");
static_assert(sizeof(PathPoint) == 12, "PathPoint layout changed; bump kVersion");
static_assert(sizeof(PathEntry) == 1184, "PathEntry layout changed; bump kVersion");
static_assert(sizeof(BattleSnapshot) <= kBattleMappingBytes, "BattleSnapshot is too large");
static_assert(sizeof(CommandState) == 560, "CommandState layout changed; bump kVersion");

// Seqlock helpers. Writers call begin_write/end_write around their payload mutation; readers call
// read_snapshot/read_commands which retry a bounded number of times and return false when the
// writer kept mutating the whole time (the caller simply tries again on its next frame).
void begin_write(std::uint32_t& sequence) noexcept;
void end_write(std::uint32_t& sequence) noexcept;

// Copies a consistent snapshot. `state` must point at mapped memory of at least
// sizeof(BattleSnapshot) bytes. Returns false when no consistent copy could be taken.
bool read_snapshot(const BattleSnapshot& state, BattleSnapshot& out,
                   std::uint32_t max_attempts = 8) noexcept;
bool read_commands(const CommandState& state, CommandState& out,
                   std::uint32_t max_attempts = 8) noexcept;

}  // namespace arkhelp::ipc
