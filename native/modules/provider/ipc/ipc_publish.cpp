// ArkHelper IPC publishing layer (provider side).
//
// Runs once per runtime frame from plugin.cpp's on_frame, after capture::tick(). Publishes a POD
// BattleSnapshot into `Local\ArkHelp.Battle.v1` with the seqlock helpers from ArkHelper/src/ipc/ipc.cpp
// and reads the overlay's feature switches from `Local\ArkHelp.Command.v1`.
//
// The snapshot is self-contained: ArkHelper.exe must never need the plugin DLL, IL2CPP or the game
// process to render it. All managed objects are read on the game thread by capture; this module only
// touches POD data plus the current stage snapshot.
#include "ipc_publish.h"
#include "status_flags.h"
#include "statistics_reader.h"
#include "enemy_snapshot.h"
#include "operator_publication.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <thread>
#include <exception>
#include <string>
#include <unordered_map>
#include <vector>

#include <ark/battle.h>
#include <ark/plugin.h>
#include <arkhelp/ipc.h>
#include <arkhelp/stage_overview.h>

#include "../app/app.h"
#include "../../damage_meter/stats.h"
#include "../capture/capture.h"
#include "../data/pack.h"
#include "../window_client.h"

namespace ark::arkhelper::ipc_publish {

namespace {

// 给外部的后台代理留一个“我还活着”的心跳文件：代理据此判断游戏内运行时是否在运行，
// 不需要 tasklist/cmd，避免在玩的时候反复弹黑色控制台窗口。
void write_runtime_heartbeat() {
    static ULONGLONG last_ms = 0;
    const ULONGLONG now = GetTickCount64();
    if (last_ms != 0 && now - last_ms < 1000) {
        return;
    }
    last_ms = now;

    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&write_runtime_heartbeat), &self) == FALSE) {
        return;
    }
    wchar_t module_path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(self, module_path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return;
    }
    // The provider now lives directly in <runtime>\ArkHelperRuntime.dll.
    wchar_t* slash = wcsrchr(module_path, L'\\');
    if (slash == nullptr) {
        return;
    }
    *slash = L'\0';
    std::wstring logs = std::wstring(module_path) + L"\\logs";
    CreateDirectoryW(logs.c_str(), nullptr);
    const std::wstring path = logs + L"\\runtime-heartbeat.txt";
    const std::string text = "pid=" + std::to_string(GetCurrentProcessId()) + "\r\n";
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
}

namespace ipc = arkhelp::ipc;

constexpr std::uint32_t kLogInfo = 1;
HANDLE g_stop_event = nullptr;
bool g_stop_requested = false;
constexpr std::uint32_t kLogWarning = 2;
constexpr std::uint32_t kLogError = 3;

// The overlay only needs ~10 Hz; publishing on salient changes in between keeps the cursor smooth.
constexpr std::uint64_t kPublishIntervalMs = 100;
constexpr std::uint64_t kMappingRetryMs = 2000;
constexpr std::uint64_t kCommandOpenRetryMs = 2000;
constexpr double kFramesPerSecond = 30.0;
constexpr std::uint32_t kDefaultFeatureMask = ipc::kFeatureSpawnTimer | ipc::kFeatureSpawnCard |
                                             ipc::kFeatureDamageNumbers;

const ArkPluginApiV1 kEmptyHost{};

const ArkPluginApiV1* g_host = nullptr;
App* g_app = nullptr;
bool g_initialized = false;

HANDLE g_battle_mapping = nullptr;
ipc::BattleSnapshot* g_battle = nullptr;
HANDLE g_overview_mapping = nullptr;
void* g_overview = nullptr;
std::uint32_t g_overview_serial = 0, g_overview_revision = 0;
HANDLE g_command_mapping = nullptr;
const ipc::CommandState* g_command = nullptr;

ipc::CommandState g_commands{};
bool g_command_valid = false;
std::uint64_t g_last_mapping_attempt = 0;
std::uint64_t g_last_command_open_attempt = 0;
std::uint64_t g_last_publish_ms = 0;
std::uint32_t g_published_revision = 0;
ipc::BattleSnapshot g_last_published{};
float g_last_time_scale = -1.f;
bool g_have_last = false;

std::string g_following_level;

ark::damage_meter::BattleStats g_statistics;
std::uint64_t g_last_sequence = 0;

bool g_service_missing_logged = false;

std::uint32_t g_mapping_error_logged = 0;
bool g_tick_error_logged = false;
bool g_wait_logged = false;

std::string format(const char* format_text, ...) {
    char buffer[1024] = {};
    va_list arguments;
    va_start(arguments, format_text);
    std::vsnprintf(buffer, sizeof(buffer), format_text, arguments);
    va_end(arguments);
    return std::string(buffer);
}

void log(std::uint32_t level, const std::string& message) {
    if (!g_host || !g_host->log || !g_host->owner) {
        return;
    }
    const std::string text = "ArkHelper IPC：" + message;
    g_host->log(g_host->owner, level, text.c_str());
}

void copy_text(char* destination, std::size_t capacity, const char* source) {
    if (!destination || capacity == 0) {
        return;
    }
    if (!source) {
        destination[0] = '\0';
        return;
    }
    std::size_t length = std::strlen(source);
    if (length >= capacity) {
        length = capacity - 1;
    }
    if (length != 0) {
        std::memcpy(destination, source, length);
    }
    destination[length] = '\0';
}

void copy_text(char* destination, std::size_t capacity, const std::string& source) {
    copy_text(destination, capacity, source.c_str());
}

const Stage* current_stage() {
    return g_app ? g_app->selected_stage() : nullptr;
}

void report_mapping_failure(const char* what, std::uint32_t error) {
    if (g_mapping_error_logged == error) {
        return;
    }
    g_mapping_error_logged = error;
    log(kLogWarning, format("%s 失败（GetLastError=%lu），稍后重试", what,
                            static_cast<unsigned long>(error)));
}

bool open_battle_mapping() {
    if (g_battle) {
        return true;
    }
    const std::uint64_t now = GetTickCount64();
    if (now - g_last_mapping_attempt < kMappingRetryMs) {
        return false;
    }
    g_last_mapping_attempt = now;

    SetLastError(0);
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        static_cast<DWORD>(ipc::kBattleMappingBytes),
                                        ipc::kBattleMappingName);
    if (!mapping) {
        report_mapping_failure("创建 Battle 共享内存", GetLastError());
        return false;
    }
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;

    void* view = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                               sizeof(ipc::BattleSnapshot));
    if (!view) {
        const std::uint32_t error = GetLastError();
        CloseHandle(mapping);
        report_mapping_failure("映射 Battle 共享内存", error);
        return false;
    }

    auto* snapshot = static_cast<ipc::BattleSnapshot*>(view);
    const bool header_valid = snapshot->magic == ipc::kBattleMagic &&
                              snapshot->version == ipc::kVersion &&
                              snapshot->size == sizeof(ipc::BattleSnapshot);
    if (!header_valid) {
        if (existed) {
            log(kLogWarning, "已存在的 Battle 映射头不匹配，按 v1 重新初始化");
        }
        std::memset(snapshot, 0, sizeof(*snapshot));
        snapshot->magic = ipc::kBattleMagic;
        snapshot->version = ipc::kVersion;
        snapshot->size = sizeof(ipc::BattleSnapshot);
        snapshot->sequence = 0;
    }

    g_battle_mapping = mapping;
    g_battle = snapshot;
    log(kLogInfo, format("Battle 映射就绪（%s，%u 字节）", existed ? "复用" : "新建",
                         static_cast<unsigned>(ipc::kBattleMappingBytes)));
    return true;
}

void try_open_commands() {
    if (g_command) {
        return;
    }
    const std::uint64_t now = GetTickCount64();
    if (now - g_last_command_open_attempt < kCommandOpenRetryMs) {
        return;
    }
    g_last_command_open_attempt = now;

    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, ipc::kCommandMappingName);
    if (!mapping) {
        return;  // ArkHelper.exe not running yet; defaults stay in effect.
    }
    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(ipc::CommandState));
    if (!view) {
        CloseHandle(mapping);
        return;
    }
    g_command_mapping = mapping;
    g_command = static_cast<const ipc::CommandState*>(view);
    log(kLogInfo, "已打开 ArkHelper 命令区");
}

void refresh_commands() {
    if (!g_command) {
        return;
    }
    ipc::CommandState state{};
    if (!ipc::read_commands(*g_command, state)) {
        return;
    }
    if (state.magic != ipc::kCommandMagic || state.version != ipc::kVersion ||
        state.size != sizeof(ipc::CommandState)) {
        return;
    }
    g_commands = state;
    g_command_valid = true;
}

// --- damage rows -----------------------------------------------------------------------------

void accumulate_damage() {
    if (!g_host || !g_host->require_service) return;
    const auto* api = static_cast<const ArkBattleEventsApiV1*>(g_host->require_service(
        g_host->owner, ARK_BATTLE_SERVICE_NAME, ARK_BATTLE_ABI_VERSION, sizeof(ArkBattleEventsApiV1)));
    if (!api || api->size < sizeof(*api) || api->abi_version != ARK_BATTLE_ABI_VERSION || !api->read) {
        if (!g_service_missing_logged) {
            g_service_missing_logged = true;
            log(kLogError, "内置统计服务不可用，统计暂未接入");
        }
        return;
    }
    read_statistics_events(api->read, g_statistics, g_last_sequence);
    g_service_missing_logged = false;
}

// --- snapshot --------------------------------------------------------------------------------

const Stage* g_projection_stage = nullptr;
std::uint32_t g_projection_revision = 0;
std::uint32_t g_projection_serial = 0;

// The detour projects tiles on the Unity main thread; the provider only publishes the request and
// fills spawn.screen from the cached result.
void publish_projection_tiles(const Stage* stage) {
    if (!stage) {
        capture::set_projection_tiles(nullptr, 0, 0, 0);
        return;
    }
    std::vector<capture::ProjectionTile> tiles;
    tiles.reserve(stage->routes.size());
    for (const Route& route : stage->routes) {
        capture::ProjectionTile tile;
        tile.row = route.start_row;
        tile.col = route.start_col;
        bool duplicate = false;
        for (const capture::ProjectionTile& existing : tiles) {
            if (existing.row == tile.row && existing.col == tile.col) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            tiles.push_back(tile);
        }
        if (tiles.size() >= 64) {
            break;
        }
    }
    capture::set_projection_tiles(tiles.data(), static_cast<int>(tiles.size()), stage->map.rows,
                                  stage->map.cols);
}

void follow_level(const capture::Status& status) {
    if (!g_app || !status.has_sample || status.level_id[0] == '\0') {
        return;
    }
    g_app->refresh_runtime_stage(status.level_id, status.battle_serial);
    const Stage* stage = g_app->selected_stage();
    if (g_following_level != status.level_id) {
        g_following_level = status.level_id;
        log(kLogInfo, format("跟随游戏当前关卡 level_id=%s", status.level_id));
    }
    const std::uint32_t revision = stage ? stage->revision : 0;
    if (stage != g_projection_stage || revision != g_projection_revision ||
        status.battle_serial != g_projection_serial) {
        g_projection_stage = stage;
        g_projection_revision = revision;
        g_projection_serial = status.battle_serial;
        publish_projection_tiles(stage);
    }
}

void fill_common(ipc::BattleSnapshot& snapshot, const capture::Status& status) {
    snapshot.magic = ipc::kBattleMagic;
    snapshot.version = ipc::kVersion;
    snapshot.size = sizeof(ipc::BattleSnapshot);
    snapshot.published_ms = GetTickCount64();
    snapshot.battle_serial = status.battle_serial;
    snapshot.in_battle = status.in_battle ? 1u : 0u;
    snapshot.battle_state = status.battle_state;
    snapshot.frame = status.frame;
    snapshot.time_seconds = status.time_seconds;
    snapshot.flags = clock_status_flags(status);
    copy_text(snapshot.level_id, sizeof(snapshot.level_id), status.level_id);
}

bool g_projection_logged = false;

void fill_spawns(ipc::BattleSnapshot& snapshot, const Stage* stage, const capture::Status& status,
                 const GameClient& client) {
    if (!stage || !status.in_battle) {
        snapshot.spawn_count = 0;
        return;
    }
    const bool want_screen =
        (g_commands.feature_mask & (ipc::kFeatureSpawnTimer | ipc::kFeatureSpawnCard)) != 0 &&
        capture::projection_available() && static_cast<bool>(client);
    std::uint32_t written = 0;
    for (const Spawn& spawn : stage->spawns) {
        if (written >= ipc::kMaxSpawns) {
            break;
        }
        if (spawn.frame < 0 || static_cast<std::uint32_t>(spawn.frame) < status.frame) {
            continue;
        }
        ipc::SpawnEntry& entry = snapshot.spawns[written];
        entry.planned_frame = static_cast<std::uint32_t>(spawn.frame);
        entry.count = static_cast<std::uint32_t>(spawn.count > 0 ? spawn.count : 1);
        entry.route_index = spawn.route_index >= 0 ? static_cast<std::uint32_t>(spawn.route_index) : 0u;
        entry.flags = spawn.random ? ipc::kSpawnRandom : 0u;
        entry.remaining_seconds =
            static_cast<float>((static_cast<double>(spawn.frame) - static_cast<double>(status.frame)) /
                               kFramesPerSecond);
        if (entry.remaining_seconds < 0.0f) {
            entry.remaining_seconds = 0.0f;
        }
        copy_text(entry.enemy_key, sizeof(entry.enemy_key), spawn.enemy_key);
        if (const Enemy* enemy = find_enemy(*stage, spawn.enemy_key)) {
            copy_text(entry.enemy_name, sizeof(entry.enemy_name), enemy->name);
        }
        if (want_screen && spawn.route_index >= 0 &&
            static_cast<std::size_t>(spawn.route_index) < stage->routes.size()) {
            const Route& route = stage->routes[static_cast<std::size_t>(spawn.route_index)];
            float screen_x = 0, screen_y = 0;
            int unity_width = 0, unity_height = 0;
            if (capture::projection_screen(route.start_row, route.start_col, &screen_x, &screen_y) &&
                capture::projection_unity_size(&unity_width, &unity_height) && unity_width > 0 && unity_height > 0) {
                const ipc::Vec2 point{static_cast<std::int32_t>(std::lround(screen_x * client.width / unity_width)),
                    static_cast<std::int32_t>(std::lround(screen_y * client.height / unity_height))};
                entry.flags |= ipc::kSpawnScreenValid;
                entry.screen.x = point.x;
                entry.screen.y = point.y;
                if (!g_projection_logged) {
                    g_projection_logged = true;
                    log(kLogInfo, format("首次投影成功 route=%d tile=(%d,%d) screen=(%d,%d)",
                                         spawn.route_index, route.start_row, route.start_col,
                                         static_cast<int>(point.x), static_cast<int>(point.y)));
                    capture::ProjectionDiag diag;
                    if (capture::projection_diag(&diag)) {
                        log(kLogInfo,
                            format("投影诊断 unity=%dx%d tiles=%d used_tiles=%d count=%d "
                                   "anchor=(%.2f,%.2f,%.2f) tile_world=(%.2f,%.2f)",
                                   diag.unity_width, diag.unity_height, diag.tiles_total,
                                   diag.used_tiles, diag.count, diag.anchor_x, diag.anchor_y,
                                   diag.anchor_z, diag.tile_world_x, diag.tile_world_z));
                    }
                }
            }
        }
        ++written;
    }
    snapshot.spawn_count = written;
}

std::uint32_t damage_type_mask(const Enemy& enemy) {
    std::uint32_t mask = 0;
    for (const std::string& type : enemy.damage_types) {
        if (type == "PHYSIC") {
            mask |= 1u << ipc::kDamagePhysical;
        } else if (type == "MAGIC") {
            mask |= 1u << ipc::kDamageArts;
        } else if (type == "TRUE") {
            mask |= 1u << ipc::kDamageTrue;
        } else if (type == "HEAL") {
            mask |= 1u << ipc::kDamageHeal;
        } else if (type == "ELEMENT") {
            mask |= 1u << ipc::kDamageElemental;
        }
    }
    if (mask == 0 && !enemy.damage_types.empty()) {
        mask |= 1u << ipc::kDamagePhysical;
    }
    return mask;
}


// Enemy unit HP bars: capture publishes normalised viewport coordinates (0..1, y up); ArkHelper draws
// in overlay pixels, so convert with the game window's client rect here on the render thread.
void fill_units(ipc::BattleSnapshot& snapshot, const capture::Status& status, const GameClient& client) {
    if (!status.in_battle) {
        snapshot.unit_count = 0;
        return;
    }
    capture::UnitSample samples[ipc::kMaxUnits] = {};
    std::uint32_t sample_serial = 0;
    const int count = capture::unit_snapshot(samples, ipc::kMaxUnits, &snapshot.unit_sample_ms,
                                             &snapshot.unit_sample_battle_time, &sample_serial);
    const auto now = GetTickCount64();
    if (count <= 0 || status.stale || sample_serial == 0 || sample_serial != status.battle_serial ||
        snapshot.unit_sample_ms == 0 ||
        snapshot.unit_sample_ms > now || now - snapshot.unit_sample_ms > 1000) {
        snapshot.unit_count = 0;
        return;
    }
    if (!client) {
        snapshot.unit_count = 0;
        return;
    }
    const float width = static_cast<float>(client.width);
    const float height = static_cast<float>(client.height);
    if (width < 16.0f || height < 16.0f) {
        snapshot.unit_count = 0;
        return;
    }
    int written = 0;
    for (int index = 0; index < count; ++index) {
        const capture::UnitSample& sample = samples[index];
        if (!(sample.viewport_x >= -0.1f && sample.viewport_x <= 1.1f &&
              sample.viewport_y >= -0.1f && sample.viewport_y <= 1.1f)) {
            continue;
        }
        ipc::UnitEntry& entry = snapshot.units[written++];
        entry.x = sample.viewport_x * width;
        entry.y = (1.0f - sample.viewport_y) * height;
        entry.hp = sample.hp;
        entry.max_hp = sample.max_hp;
        entry.flags = sample.flags;
        entry.tracking_id = sample.tracking_id;
        if (sample.flags & ipc::kUnitGroundValid) {
            entry.ground_x = sample.ground_x * width;
            entry.ground_y = (1.f - sample.ground_y) * height;
        }
        if ((sample.flags & ipc::kUnitRangeValid) && sample.range_point_count == ipc::kRangePointCount) {
            entry.range_radius = sample.range_radius;
            entry.range_point_count = sample.range_point_count;
            for (std::uint32_t p = 0; p < entry.range_point_count; ++p) {
                entry.range_points[p] = {sample.range_points[p].x * width,
                                        (1.f - sample.range_points[p].y) * height};
            }
        } else entry.flags &= ~ipc::kUnitRangeValid;
    }
    snapshot.unit_count = static_cast<std::uint32_t>(written);
}

void fill_enemies(ipc::BattleSnapshot& snapshot, const Stage* stage) {
    capture::EnemySample live[ipc::kMaxEnemies + 1]{};
    std::uint32_t serial = 0;
    std::uint64_t sampled_ms = 0;
    const int live_count = capture::enemy_snapshot(live, ipc::kMaxEnemies + 1, &sampled_ms, &serial);
    const auto now = GetTickCount64();
    if (live_count > 0 && serial == snapshot.battle_serial && snapshot.in_battle &&
        (snapshot.flags & ipc::kBattleStale) == 0 && sampled_ms && sampled_ms <= now &&
        now - sampled_ms <= 1000) {
        const auto count = std::min<std::uint32_t>(static_cast<std::uint32_t>(live_count), ipc::kMaxEnemies);
        for (std::uint32_t index = 0; index < count; ++index) {
            snapshot.enemies[index] = make_enemy_entry(live[index]);
            // Names/mechanism labels are safe to supplement; numeric fields stay instance-only.
            if (stage) {
                const Enemy* reference = find_enemy(*stage, live[index].enemy_key);
                if (reference) {
                    if (snapshot.enemies[index].enemy_name[0] == '\0') {
                        copy_text(snapshot.enemies[index].enemy_name, ipc::kNameCapacity, reference->name);
                    }
                    snapshot.enemies[index].damage_type_mask = damage_type_mask(*reference);
                }
            }
        }
        snapshot.enemy_count = count;
        if (live_count > static_cast<int>(ipc::kMaxEnemies)) {
            snapshot.flags |= ipc::kBattleEnemiesTruncated;
        }
        return;
    }
    if (!stage) {
        snapshot.enemy_count = 0;
        return;
    }
    std::uint32_t written = 0;
    for (const Enemy& enemy : stage->enemies) {
        if (written >= ipc::kMaxEnemies) {
            break;
        }
        ipc::EnemyEntry& entry = snapshot.enemies[written];
        entry = make_reference_entry(enemy);
        entry.damage_type_mask = damage_type_mask(enemy);
        ++written;
    }
    snapshot.enemy_count = written;
}

void fill_paths(ipc::BattleSnapshot& snapshot, const capture::Status& status, const GameClient& client) {
    if (!status.in_battle || status.stale ||
        !ipc::command_feature_enabled(g_commands, ipc::kFeatureEnemyPaths)) return;
    capture::PathSample samples[ipc::kMaxPaths]{};
    const int count = capture::path_snapshot(samples, ipc::kMaxPaths);
    if (!client) return;
    const float width = static_cast<float>(client.width), height = static_cast<float>(client.height);
    if (width < 16 || height < 16) return;
    const auto now = GetTickCount64();
    for (int index = 0; index < count; ++index) {
        const auto& sample = samples[index];
        if (!(sample.flags & ipc::kPathValid) || sample.battle_serial != status.battle_serial ||
            !sample.tracking_id || !sample.sampled_ms || sample.sampled_ms > now || now-sample.sampled_ms > 1000 ||
            sample.point_count < 2 || sample.point_count > ipc::kMaxPathPoints) continue;
        auto entry = sample;
        bool valid = true;
        for (std::uint32_t p = 0; p < entry.point_count; ++p) {
            const auto point = sample.points[p];
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < -8 || point.x > 9 ||
                point.y < -8 || point.y > 9) { valid = false; break; }
            entry.points[p].x = point.x*width;
            entry.points[p].y = (1.f-point.y)*height;
        }
        if (valid) snapshot.paths[snapshot.path_count++] = entry;
    }
}

void fill_damage(ipc::BattleSnapshot& snapshot, const capture::Status& status, const GameClient& client) {
    std::vector<const ark::damage_meter::ActorRow*> rows;
    g_statistics.sort_rows(rows, true);
    const auto limit = std::min<std::size_t>(rows.size(), ipc::kMaxDamageRows);
    const double duration = std::max(0.1, g_statistics.duration());
    const double team_damage = g_statistics.total(true);
    std::uint32_t supported = ((1u << ipc::kStatCount) - 1u) &
        ~((1u << ipc::kStatCost) | (1u << ipc::kStatHeal) | (1u << ipc::kStatTaken) | (1u << ipc::kStatShield));
    if (g_statistics.capabilities & (1u << ARK_HEAL)) supported |= 1u << ipc::kStatHeal;
    if (g_statistics.capabilities & (1u << ARK_COST)) supported |= 1u << ipc::kStatCost;
    if (g_statistics.capabilities & (1u << ARK_TAKEN)) supported |= 1u << ipc::kStatTaken;
    snapshot.team_metric_mask = supported;
    snapshot.stats_flags = (g_statistics.partial ? 1u : 0u) | (rows.size() > limit ? 2u : 0u);
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& row = *rows[index];
        ipc::DamageEntry overflow;
        auto& entry = index < limit ? snapshot.damage[index] : overflow;
        entry.flags = ipc::kDamageValid;
        entry.total_damage = static_cast<std::int64_t>(std::llround(row.actual));
        entry.dps = static_cast<float>(row.actual / duration);
        entry.share = team_damage > 0 ? static_cast<float>(row.actual / team_damage) : 0.f;
        entry.metric_mask = supported;
        entry.metrics[ipc::kStatDamage] = row.actual;
        entry.metrics[ipc::kStatPhysical] = row.types[0].actual;
        entry.metrics[ipc::kStatArts] = row.types[1].actual;
        entry.metrics[ipc::kStatTrue] = row.types[2].actual;
        entry.metrics[ipc::kStatElemental] = row.types[3].actual;
        entry.metrics[ipc::kStatHeal] = row.healing;
        entry.metrics[ipc::kStatCost] = row.deployment_points;
        entry.metrics[ipc::kStatBuildup] = row.elemental_buildup;
        entry.metrics[ipc::kStatTaken] = row.taken;
        entry.metrics[ipc::kStatDps] = row.actual / duration;
        entry.metrics[ipc::kStatShare] = team_damage > 0 ? row.actual / team_damage : 0;
        for (std::uint32_t metric = 0; metric < ipc::kStatCount; ++metric)
            snapshot.team_metrics[metric] += entry.metrics[metric];
        copy_text(entry.actor_id, sizeof(entry.actor_id), row.id);
    }
    snapshot.damage_count = static_cast<std::uint32_t>(limit);
    if (status.in_battle && !status.stale &&
        ipc::command_feature_enabled(g_commands, ipc::kFeatureOperatorStats)) {
        capture::OperatorSample operators[ipc::kMaxUnits]{};
        std::uint64_t sampled_ms = 0;
        std::uint32_t serial = 0;
        const int count = capture::operator_snapshot(operators, ipc::kMaxUnits, &sampled_ms, &serial);
        const auto now = GetTickCount64();
        if (count && serial == status.battle_serial && sampled_ms && sampled_ms <= now &&
            now-sampled_ms <= 1000 && client) {
            attach_operator_positions(snapshot, operators, count, supported,
                static_cast<float>(client.width), static_cast<float>(client.height));
            attach_operator_instances(snapshot, operators, count, g_statistics, supported,
                static_cast<float>(client.width), static_cast<float>(client.height));
        }
    }
}

bool salient_changed(const capture::Status& status) {
    if (!g_have_last) {
        return true;
    }
    const ipc::BattleSnapshot& last = g_last_published;
    if (last.battle_serial != status.battle_serial) {
        return true;
    }
    if (last.in_battle != (status.in_battle ? 1u : 0u)) {
        return true;
    }
    if (last.battle_state != status.battle_state) {
        return true;
    }
    if (std::strncmp(last.level_id, status.level_id, sizeof(last.level_id)) != 0) {
        return true;
    }
    if (clock_status_changed(status, last, g_last_time_scale)) {
        return true;
    }
    if (g_published_revision != g_commands.revision) {
        return true;
    }
    return false;
}

void publish_snapshot(const ipc::BattleSnapshot& snapshot, float time_scale = -1.f) {
    if (!g_battle) {
        return;
    }
    constexpr std::size_t kPayloadOffset = offsetof(ipc::BattleSnapshot, published_ms);
    char* base = reinterpret_cast<char*>(g_battle);
    const char* source = reinterpret_cast<const char*>(&snapshot);
    ipc::begin_write(g_battle->sequence);
    std::memcpy(base + kPayloadOffset, source + kPayloadOffset, sizeof(snapshot) - kPayloadOffset);
    ipc::end_write(g_battle->sequence);
    g_last_published = snapshot;
    g_last_time_scale = time_scale;
    g_have_last = true;
    g_last_publish_ms = GetTickCount64();
    g_published_revision = g_commands.revision;
}

}  // namespace

void initialize(App* app) {
    g_app = app;
    g_host = app ? &app->host() : &kEmptyHost;
    g_initialized = true;

    ipc::CommandState commands{};
    commands.magic = ipc::kCommandMagic;
    commands.version = ipc::kVersion;
    commands.size = sizeof(ipc::CommandState);
    commands.feature_mask = kDefaultFeatureMask;
    commands.ui_scale = 1.0f;
    g_commands = commands;
    g_command_valid = false;

    log(kLogInfo, "发布层已初始化（Battle 映射按需创建，命令区固定重试）");
}


bool g_projection_diag_logged = false;

// One-shot calibration dump: Unity screen size, map anchor world position and the projected screen
// position of every route start tile. Compare against a live screenshot to derive the tile scale and
// axis signs without another restart per iteration.
void log_projection_diagnostics(const capture::Status& status) {
    if (g_projection_diag_logged || !g_projection_stage || !status.in_battle) {
        return;
    }
    int unity_width = 0;
    int unity_height = 0;
    if (!capture::projection_unity_size(&unity_width, &unity_height)) {
        return;
    }
    float anchor_x = 0.0f;
    float anchor_y = 0.0f;
    float anchor_z = 0.0f;
    std::string text = format("投影诊断 unity=%dx%d", unity_width, unity_height);
    if (capture::projection_anchor(&anchor_x, &anchor_y, &anchor_z)) {
        text += format(" anchor=(%.2f,%.2f,%.2f)", anchor_x, anchor_y, anchor_z);
    } else {
        text += " anchor=?";
    }
    int tiles = 0;
    for (const Route& route : g_projection_stage->routes) {
        float x = 0.0f;
        float y = 0.0f;
        if (!capture::projection_screen(route.start_row, route.start_col, &x, &y)) {
            continue;
        }
        text += format(" (%d,%d)->(%.1f,%.1f)", route.start_row, route.start_col, x, y);
        if (++tiles >= 12) {
            break;
        }
    }
    log(kLogInfo, text);
    if (tiles > 0) {
        g_projection_diag_logged = true;
    }
}

// Publish only the current level and only when its copied definition changes.
void publish_overview(const Stage* stage, const capture::Status& status) {
    if (!stage || !stage->loaded || !stage->runtime || !status.battle_serial ||
        stage->level_id != status.level_id) return;
    if (!g_overview) {
        g_overview_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(ipc::kOverviewMappingBytes), ipc::kOverviewMappingName);
        if (!g_overview_mapping) return;
        g_overview = MapViewOfFile(g_overview_mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, ipc::kOverviewMappingBytes);
        if (!g_overview) { CloseHandle(g_overview_mapping); g_overview_mapping = nullptr; return; }
        auto* header = static_cast<ipc::OverviewHeader*>(g_overview);
        // A prior crashed writer can leave an odd sequence; this provider is the sole writer.
        if (header->sequence & 1u) ipc::end_write(header->sequence);
        g_overview_serial = g_overview_revision = 0;
    }
    if (g_overview_serial == status.battle_serial && g_overview_revision == stage->revision) return;
    ipc::StageOverview data;
    data.header.battle_serial = status.battle_serial;
    data.header.revision = stage->revision;
    data.header.flags = ipc::kOverviewLoaded | (stage->overview_partial ? ipc::kOverviewPartial : 0u) |
        (stage->overview_plan_read ? ipc::kOverviewPlanRead : 0u);
    copy_text(data.header.level_id, sizeof(data.header.level_id), stage->level_id.c_str());
    for (const auto& enemy : stage->enemies) {
        if (data.enemies.size() >= ipc::kOverviewMaxEnemies) { data.header.flags |= ipc::kOverviewPartial; break; }
        data.enemies.push_back(make_reference_entry(enemy));
    }
    for (const auto& spawn : stage->overview_spawns) {
        if (data.spawns.size() >= ipc::kOverviewMaxSpawns) { data.header.flags |= ipc::kOverviewPartial; break; }
        ipc::StageSpawn entry;
        copy_text(entry.enemy_key, sizeof(entry.enemy_key), spawn.key.c_str());
        entry.branch = spawn.branch; entry.wave = spawn.wave; entry.fragment = spawn.fragment;
        entry.count = spawn.count; entry.route = spawn.route;
        entry.delay = spawn.delay; entry.interval = spawn.interval;
        entry.flags = (spawn.conditional ? ipc::kStageSpawnConditional : 0u) |
            (spawn.count_known ? ipc::kStageSpawnCountKnown : 0u) |
            (spawn.timing_known ? ipc::kStageSpawnTimingKnown : 0u);
        data.spawns.push_back(entry);
    }
    if (ipc::write_overview(g_overview, data)) {
        g_overview_serial = status.battle_serial; g_overview_revision = stage->revision;
        log(kLogInfo, format("Level overview: %zu enemy types, %zu spawn actions%s", data.enemies.size(),
            data.spawns.size(), data.header.flags & ipc::kOverviewPartial ? " (partial metadata)" : ""));
    }
}

void tick() {
    if (!g_initialized) {
        return;
    }
    write_runtime_heartbeat();
    // ArkHelper 退出时通过命名事件请求停止运行时：调用核心导出设置停止标记，
    // 避免在渲染线程回调内同步停止插件导致的生命周期死锁。
    if (g_stop_event == nullptr) {
        g_stop_event = OpenEventW(SYNCHRONIZE, FALSE, L"Local\\ArkHelp.RuntimeStop");
    }
    if (g_stop_event != nullptr && !g_stop_requested &&
        WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0) {
        g_stop_requested = true;
        log(kLogInfo, "ArkHelper exited: requesting runtime shutdown");
        // The export only requests stop; rundown runs after this callback returns.
        HMODULE core = GetModuleHandleW(L"ArkHelperRuntime.dll");
        if (core != nullptr) {
            using ShutdownFn = void (*)();
            auto fn = reinterpret_cast<ShutdownFn>(GetProcAddress(core, "arkhelp_runtime_shutdown"));
            if (fn != nullptr) {
                fn();
            }
        }
    }
    try {
        open_battle_mapping();
        try_open_commands();
        refresh_commands();
        capture::set_range_enabled(ipc::command_feature_enabled(g_commands, ipc::kFeatureEnemyRange));
        capture::set_operator_stats_enabled(ipc::command_feature_enabled(g_commands, ipc::kFeatureOperatorStats));
        capture::set_path_requests(ipc::command_feature_enabled(g_commands, ipc::kFeatureEnemyPaths),
                                  g_commands.path_request_battle_serial, g_commands.path_request_ids,
                                  static_cast<int>(std::min(g_commands.path_request_count, ipc::kMaxPaths)));
        if (!g_battle) {
            return;
        }

        const capture::Status status = capture::status();
        follow_level(status);
        publish_overview(current_stage(), status);
        // Read the final events after leaving the battle too. The next ARK_BATTLE_START resets
        // the report; keeping it here makes the last battle available until the next one starts.
        accumulate_damage();

        const std::uint64_t now = GetTickCount64();
        if (!salient_changed(status) && now - g_last_publish_ms < kPublishIntervalMs) {
            return;
        }

        ipc::BattleSnapshot snapshot{};
        fill_common(snapshot, status);
        if (const Stage* stage = current_stage()) {
            if (stage->runtime && stage->loaded) { snapshot.flags |= ipc::kBattleRuntimeStage; }
            if (stage->spawn_plan_available) { snapshot.flags |= ipc::kBattleSpawnPlan; }
            if (stage->spawn_plan_conditional) { snapshot.flags |= ipc::kBattleSpawnConditional; }
        }
        const auto client = current_game_client();
        fill_spawns(snapshot, current_stage(), status, client);
        log_projection_diagnostics(status);
        fill_enemies(snapshot, current_stage());
        fill_units(snapshot, status, client);
        fill_paths(snapshot, status, client);
        fill_damage(snapshot, status, client);
        publish_snapshot(snapshot, status.time_scale);
        if (!g_wait_logged && status.has_sample) {
            g_wait_logged = true;
            log(kLogInfo, "首帧战斗快照已发布");
        }
    } catch (const std::exception& error) {
        if (!g_tick_error_logged) {
            g_tick_error_logged = true;
            log(kLogError, format("发布异常：%s", error.what()));
        }
    } catch (...) {
        if (!g_tick_error_logged) {
            g_tick_error_logged = true;
            log(kLogError, "发布异常（未知错误）");
        }
    }
}

void shutdown() {
    if (g_stop_event != nullptr) {
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
    }
    g_stop_requested = false;
    if (g_battle) {
        ipc::BattleSnapshot snapshot{};
        snapshot.magic = ipc::kBattleMagic;
        snapshot.version = ipc::kVersion;
        snapshot.size = sizeof(ipc::BattleSnapshot);
        snapshot.published_ms = GetTickCount64();
        snapshot.battle_state = -1;
        snapshot.flags = g_have_last ? (g_last_published.flags & ipc::kBattleLevelKnown) : 0u;
        copy_text(snapshot.level_id, sizeof(snapshot.level_id), g_have_last ? g_last_published.level_id
                                                                           : "");
        publish_snapshot(snapshot);  // Best effort: lets ArkHelper.exe show "provider offline".
    }
    if (g_overview) { UnmapViewOfFile(g_overview); g_overview = nullptr; }
    if (g_overview_mapping) { CloseHandle(g_overview_mapping); g_overview_mapping = nullptr; }
    g_overview_serial = g_overview_revision = 0;
    if (g_command) {
        UnmapViewOfFile(const_cast<void*>(static_cast<const void*>(g_command)));
        g_command = nullptr;
    }
    if (g_command_mapping) {
        CloseHandle(g_command_mapping);
        g_command_mapping = nullptr;
    }
    if (g_battle) {
        UnmapViewOfFile(g_battle);
        g_battle = nullptr;
    }
    if (g_battle_mapping) {
        CloseHandle(g_battle_mapping);
        g_battle_mapping = nullptr;
    }

    g_initialized = false;
    g_host = nullptr;
    g_app = nullptr;
    g_commands = ipc::CommandState{};
    g_command_valid = false;
    g_last_mapping_attempt = 0;
    g_last_command_open_attempt = 0;
    g_last_publish_ms = 0;
    g_published_revision = 0;
    g_last_published = ipc::BattleSnapshot{};
    g_last_time_scale = -1.f;
    g_have_last = false;
    g_following_level.clear();
    g_statistics = {};
    g_last_sequence = 0;
    g_service_missing_logged = false;
    g_mapping_error_logged = 0;
    g_tick_error_logged = false;
    g_wait_logged = false;
    g_projection_stage = nullptr;
    g_projection_revision = 0;
    g_projection_serial = 0;
    g_projection_logged = false;
    g_projection_diag_logged = false;

}

bool active() {
    return g_battle != nullptr;
}

}  // namespace ark::arkhelper::ipc_publish
