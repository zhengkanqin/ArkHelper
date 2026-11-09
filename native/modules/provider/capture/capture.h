// Runtime capture: current level, native clock, live per-instance stats and viewport samples.
//
// The capture layer installs one native hook on `Torappu.Battle.BattleController.Update`. The
// detour runs on the Unity main thread, reads the managed fields through native entry points that
// were resolved once at connect time and publishes a POD snapshot with a single-writer seqlock.
// The runtime worker only copies that snapshot; it never touches gameplay memory directly.
//
// Lifecycle (runtime worker unless noted):
//   initialize(host) -> tick() [connect throttle >= 1 s] -> status() [UI]  -> shutdown()
//
// Failure policy: every step is retried on the next tick and never throws out of tick(); a missing
// game process, a missing IL2CPP domain or renamed metadata leaves affected data unavailable
// and does not prevent the desktop app or independent features from running.
#pragma once

#include <cstdint>
#include <arkhelp/ipc.h>
#include "../data/pack.h"

#include <ark/plugin.h>

namespace ark::arkhelper::capture {

enum class LinkState : std::int32_t {
    Idle = 0,       // initialize() has not been called
    Connecting = 1, // retrying once per second, no hook yet
    Connected = 2,  // hook installed, waiting for / receiving samples
    Failed = 3,     // retrying, but the last error is worth surfacing in red
};

struct Status {
    LinkState link = LinkState::Idle;
    bool hook_installed = false;
    bool has_sample = false;
    bool in_battle = false; // last sampled BattleController.m_state was Running (2)
    bool stale = false;     // in battle but the detour has not published for > 2 s
    bool pause_known = false; // verified BattleController.get_isPaused getter is available
    bool paused = false;      // authoritative current pause state, not inferred from a stopped frame
    bool is_error = false;  // `message` is an error, not a "waiting" note
    std::int32_t battle_state = -1;
    std::uint32_t battle_serial = 0; // increments whenever a new battle starts
    std::uint32_t frame = 0;         // BattleController.fixedFrameCnt (native logic frame)
    double time_seconds = 0.0;       // BattleController.fixedPlayTime / 2^32
    float time_scale = -1.0f;        // current BattleController timeScale; negative means unavailable
    std::uint64_t updated_ms = 0;    // GetTickCount64() when the sample was published
    char level_id[160] = {};         // BattleController.levelId (UTF-8), empty when unknown
    char message[224] = {};          // connection state / last error (UTF-8)
};

// Stores the host API and arms the connect throttle; never fails (degradation is a UI state).
void initialize(const ArkPluginApiV1& host);

// Runtime-worker tick: retries the connection at most once per second and does nothing once the
// hook is installed (the detour feeds the snapshot from the game thread).
void tick();

// Stops the detour, disables and removes the native hook, then waits for in-flight callbacks.
// Returns false when the hook could not be disabled/removed; the caller must keep the module
// mapped (and its own state intact) so a later shutdown() can retry.
bool shutdown();

// Render-thread snapshot for the UI. Safe to call before initialize().
Status status();

// Whether the plugin is currently safe to unload. Removal is attempted by shutdown(); retrying it
// later is the host's job, so this only reports whether capture already released its hook.
bool can_unload();

// Projection bridge for the provider: the render thread publishes the route start tiles of the
// current stage, the detour projects them on the Unity main thread and the provider reads the last
// result back when it fills the spawn list.
struct ProjectionTile {
    int row = 0;
    int col = 0;
};

// Last projection result, for one-shot diagnostics in the plugin log.
struct ProjectionDiag {
    int unity_width = 0;
    int unity_height = 0;
    int tiles_total = 0;
    int used_tiles = 0;
    int count = 0;
    float anchor_x = 0.0f;
    float anchor_y = 0.0f;
    float anchor_z = 0.0f;
    float tile_world_x = 0.0f;
    float tile_world_z = 0.0f;
};

void set_projection_tiles(const ProjectionTile* tiles, int count, int map_rows, int map_cols);
bool projection_available();
bool projection_diag(ProjectionDiag* out);
bool projection_screen(int row, int col, float* x, float* y);

// Enemy unit screen positions + HP (game-thread scan, published at 4 Hz). Viewport coordinates are
// Unity normalised device coordinates (0..1, y up); the provider maps them to overlay pixels.
struct UnitSample {
    std::uint64_t tracking_id = 0;
    float viewport_x = 0.0f;
    float viewport_y = 0.0f;
    float hp = 0.0f;
    float max_hp = 0.0f;
    std::uint32_t flags = 0;
    float ground_x = 0, ground_y = 0;  // normalized viewport, y up
    float range_radius = 0;
    std::uint32_t range_point_count = 0;
    struct Point { float x = 0, y = 0; } range_points[16]{};
};

// Worker publishes feature state; disabled (the default) performs no range reads/projection.
void set_range_enabled(bool enabled);

void set_spawns_enabled(bool enabled);
// True includes a known empty current queue; false permits the conservative stage prefix.
bool spawn_snapshot(std::vector<Spawn>& out, std::uint32_t battle_serial);

struct OperatorSample {
    std::uint64_t instance_id = 0;
    char actor_id[96]{};
    float viewport_x = 0, viewport_y = 0;
    std::uint32_t flags = 0; // bit0: alive, born and visible current instance
};
void set_operator_stats_enabled(bool enabled);
int operator_snapshot(OperatorSample* out, int max, std::uint64_t* sampled_ms = nullptr,
                      std::uint32_t* battle_serial = nullptr);

// Paths contain normalized viewport coordinates here; IPC publishing converts to client pixels.
using PathSample = ::arkhelp::ipc::PathEntry;
void set_path_requests(bool enabled, std::uint32_t battle_serial,
                       const std::uint64_t* tracking_ids, int count);
int path_snapshot(PathSample* out, int max);

bool unit_api_available();
int unit_snapshot(UnitSample* out, int max, std::uint64_t* sample_ms = nullptr,
                  double* sample_battle_time = nullptr,
                  std::uint32_t* sample_battle_serial = nullptr);

// Per-instance, post-modifier values. Missing getters leave their availability bit clear.
// Bits match the internal IPC EnemyStats mask: hp/max/atk/def/MR/move/AS/interval/weight/block/life.
struct EnemySample {
    std::uint64_t tracking_id = 0;
    char enemy_key[96] = {};
    char enemy_name[96] = {};
    double hp = 0, max_hp = 0, atk = 0, def = 0, magic_resistance = 0;
    double move_speed = 0, attack_speed = 0, base_attack_time = 0, weight = 0;
    int block_cnt = 0, life_point_reduce = 0;
    std::uint32_t stats_mask = 0;
};
int enemy_snapshot(EnemySample* out, int max, std::uint64_t* sample_ms = nullptr,
                   std::uint32_t* battle_serial = nullptr);

// Copies only the currently loaded level. Known serial/revision avoids copying an unchanged
// snapshot; false also means unavailable/unchanged. Never downloads a catalog or uses stale data.
bool runtime_stage(Stage& out, std::uint32_t* battle_serial = nullptr,
                   std::uint32_t known_revision = 0, std::uint32_t known_serial = 0);

bool projection_unity_size(int* width, int* height);
bool projection_anchor(float* x, float* y, float* z);

} // namespace ark::arkhelper::capture
