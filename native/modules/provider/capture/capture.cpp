// Runtime capture implementation for ArkHelper (M2). See capture.h for the contract.
//
// Design notes:
//  * The hook target is BattleController.Update: it is called once per rendered frame while a
//    battle scene is live and it hands us the controller instance as its first argument, which is
//    exactly what the levelId / fixedPlayTime / fixedFrameCnt / m_state reads need.
//  * Attribute getters are cached per metadata class; level parsing is bounded and only retried
//    when needed. Position/HP use POD snapshots; dynamic stage/stat publishing never waits on a
//    desktop reader (try_lock skips contention).
//  * `Runtime` is attached on the worker only for connection discovery. Live object reads and
//    lazily resolved metadata stay on Unity's attached main thread inside the detour.
#include "capture.h"

#include <windows.h>
#include <arkhelp/ipc.h>

#include <atomic>
#include <cstddef>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include "il2cpp_exports.h"
#include "il2cpp_runtime.h"
#include "live_data.h"
#include "live_range.h"
#include "live_path.h"
#include "live_operator.h"
#include "operator_samples.h"

namespace ark::arkhelper::capture {
namespace {

constexpr std::uint32_t kLogInfo = 1;
constexpr std::uint32_t kLogError = 3;

constexpr std::uint32_t kConnectIntervalMs = 1000;
constexpr std::uint32_t kStaleAfterMs = 2000;
constexpr std::uint32_t kShutdownDrainMs = 2000;
constexpr std::uint32_t kMethodAttributeStatic = 0x10;
constexpr std::int32_t kBattleStateRunning = 2;
constexpr std::int32_t kBattleStateFinished = 3;
constexpr double kFixedPointOne = 4294967296.0;
constexpr std::size_t kLevelIdCapacity = 160;
constexpr int kMaxManagedStringLength = 256;

// Snapshot flags inside Sample::flags.
constexpr std::uint32_t kFlagInBattle = 1u;
constexpr std::uint32_t kFlagLevelKnown = 2u;
constexpr std::uint32_t kFlagPauseKnown = 4u;
constexpr std::uint32_t kFlagPaused = 8u;

// A managed property getter compiled by IL2CPP is either:
//   instance: <return> (*)(T* this, const MethodInfo* method)
//   static:   <return> (*)(const MethodInfo* method)
// `Torappu.Battle.BattleController.get_fixedPlayTime` / `get_fixedFrameCnt` are static accessors
// (damage_meter calls them with the MethodInfo alone); `get_levelId` is an instance getter. The
// static flag is read from metadata at resolve time so both shapes are supported.
struct Getter {
    void* method = nullptr;
    void* entry = nullptr;
    bool is_static = false;

    bool valid() const { return method != nullptr && entry != nullptr; }

    void* reference(void* self) const {
        if (is_static) {
            return reinterpret_cast<void* (*)(void*)>(entry)(method);
        }
        return reinterpret_cast<void* (*)(void*, void*)>(entry)(self, method);
    }

    std::int64_t integer(void* self) const {
        if (is_static) {
            return reinterpret_cast<std::int64_t (*)(void*)>(entry)(method);
        }
        return reinterpret_cast<std::int64_t (*)(void*, void*)>(entry)(self, method);
    }

    std::uint32_t counter(void* self) const {
        if (is_static) {
            return reinterpret_cast<std::uint32_t (*)(void*)>(entry)(method);
        }
        return reinterpret_cast<std::uint32_t (*)(void*, void*)>(entry)(self, method);
    }

};

// Written once by the render thread before the hook is enabled, then only read by the game thread.
// Plain POD: a default-constructed value is a valid "no battle observed yet" state.

// Projection chain: resolved on the render thread, invoked only from the detour (Unity main thread).
struct Vector3f {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};
static_assert(sizeof(Vector3f) == 12, "UnityEngine.Vector3 layout");

struct ProjectionApi {
    void* battle_camera_instance = nullptr;
    void* battle_camera = nullptr;
    void* battle_get_map = nullptr;
    void* map_class = nullptr;
    void* tile_class = nullptr;
    void* map_anchor_transform = nullptr;
    void* map_tiles_field = nullptr;
    void* map_tiles_container = nullptr;
    void* component_get_transform = nullptr;
    void* transform_get_position = nullptr;
    void* transform_get_child = nullptr;
    void* transform_get_child_count = nullptr;
    void* array_get_length = nullptr;
    void* array_get_value = nullptr;
    void* camera_get_main = nullptr;
    void* camera_world_to_screen = nullptr;
    void* camera_world_to_viewport = nullptr;
    void* screen_get_width = nullptr;
    void* screen_get_height = nullptr;
    void* map_to_world = nullptr;
    void* camera_view_matrix = nullptr;
    void* camera_projection_matrix = nullptr;

    bool ready() const {
        return battle_get_map && map_anchor_transform && transform_get_position &&
               camera_get_main && camera_world_to_screen && screen_get_width && screen_get_height;
    }

    // Preferred path: read each tile's own world position, so no origin/axis calibration is needed.
    bool tiles_ready() const {
        return ready() && component_get_transform && (map_tiles_field || map_tiles_container);
    }
};


// Unit/enemy layer: HP bars need the live unit list (screen position + hp), which the tile
// projection above does not expose. All managed reads happen on the game thread in the detour.
struct UnitApi {
    void* battle_unit_manager = nullptr; // BattleController.<unitManager>k__BackingField
    void* manager_enemies = nullptr;     // UnitManager.<enemies>k__BackingField
    void* manager_characters = nullptr;
    void* array_count = nullptr;         // UnorderedArray`1.get_Count
    void* array_item = nullptr;          // UnorderedArray`1.get_Item(int)
    void* unit_class = nullptr;
    void* component_get_transform = nullptr;
    void* transform_get_position = nullptr;
    void* camera_get_main = nullptr;
    void* camera_world_to_viewport = nullptr;
    void* camera_world_to_screen = nullptr;
    void* screen_get_width = nullptr;
    void* screen_get_height = nullptr;
    void* container_items_field = nullptr; // fallback for UnorderedArray`1: backing array field
    void* container_size_field = nullptr;  // fallback: element count field
    int container_size_kind = 0;
    bool container_probed = false;
    void* hp_field = nullptr;
    void* max_hp_field = nullptr;
    void* hp_method = nullptr;     // Unit.get_hpToShow()
    void* max_hp_method = nullptr; // Unit.get_maxHpToShow() when present
    void* hp_ratio_method = nullptr; // Unit.get_hpRatioToShow() (max = hp / ratio)
    void* fp_to_float = nullptr;     // Torappu.FP.ToFloat(FP) -> System.Single
    int hp_kind = 0;   // 0 missing, 1 float, 2 int32
    int max_kind = 0;

    bool ready() const {
        return battle_unit_manager && manager_enemies;
    }
};

constexpr int kMaxUnits = 64;
constexpr std::uint64_t kUnitUpdateIntervalMs = 250;

using UnitItem = UnitSample;

struct UnitPayload {
    std::uint64_t updated_ms = 0;
    double time_seconds = 0;
    std::uint32_t battle_serial = 0;
    std::uint32_t count = 0;
    UnitItem items[kMaxUnits] = {};
};

struct EnemyPayload {
    std::uint64_t updated_ms = 0;
    std::uint32_t battle_serial = 0;
    std::uint32_t count = 0;
    EnemySample items[kMaxUnits]{};
};

struct OperatorPayload {
    std::uint64_t updated_ms = 0;
    std::uint32_t battle_serial = 0, count = 0;
    OperatorSample items[kMaxUnits]{};
};
struct PathRequest {
    bool enabled = false;
    std::uint32_t battle_serial = 0, count = 0;
    std::uint64_t ids[::arkhelp::ipc::kMaxPaths]{};
};
struct PathPayload {
    std::uint32_t count = 0;
    PathSample items[::arkhelp::ipc::kMaxPaths]{};
};

constexpr int kMaxProjectionTiles = 64;
constexpr float kTileWorldSize = 1.0f;

struct ProjectionRequest {
    std::uint32_t generation = 0;
    std::int32_t count = 0;
    std::int32_t map_rows = 0;
    std::int32_t map_cols = 0;
    std::int16_t rows[kMaxProjectionTiles] = {};
    std::int16_t cols[kMaxProjectionTiles] = {};
};

struct ProjectionItem {
    std::int16_t row = 0;
    std::int16_t col = 0;
    bool valid = false;
    float x = 0.0f;
    float y = 0.0f;
};

struct ProjectionResult {
    std::uint32_t generation = 0;
    std::int32_t unity_width = 0;
    std::int32_t unity_height = 0;
    std::int32_t count = 0;
    // Diagnostics: 0 = anchor formula, 1 = per-tile transform (authoritative).
    std::int32_t used_tiles = 0;
    std::int32_t tiles_total = 0;
    float anchor_x = 0.0f;
    float anchor_y = 0.0f;
    float anchor_z = 0.0f;
    float tile_world_x = 0.0f;
    float tile_world_z = 0.0f;
    ProjectionItem items[kMaxProjectionTiles] = {};
};

struct DetourState {
    Getter level_id;
    Getter play_time;
    Getter frame;
    Getter paused;
    Getter time_scale;
    int (*string_length)(void*) = nullptr;
    const wchar_t* (*string_chars)(void*) = nullptr;
    // Raw instance-field offset. Reading m_state straight from the object avoids depending on the
    // argument order of il2cpp_field_get_value across runtime builds.
    int battle_state_offset = -1;
    std::int32_t last_state = 0;
    double last_time_seconds = 0.0;
    std::uint32_t serial = 0;
    ProjectionApi projection;
    UnitApi units;
    std::uint64_t last_projection_ms = 0;
    std::uint64_t last_units_ms = 0;
    Vector3f last_origin{};
    bool has_origin = false;
    void* last_controller = nullptr; // identity token only, never dereferenced across updates
    char last_level_id[kLevelIdCapacity]{};
};

// Single-writer snapshot payload (seqlock protected by g_sequence).
struct Sample {
    std::uint32_t serial = 0;
    std::int32_t battle_state = -1;
    std::uint32_t frame = 0;
    std::uint64_t updated_ms = 0;
    double time_seconds = 0.0;
    float time_scale = -1.f;
    std::uint32_t flags = 0;
    char level_id[kLevelIdCapacity] = {};
};

ArkPluginApiV1 g_host{};
bool g_initialized = false;
bool g_api_usable = false;

std::uint64_t g_hook = 0;
bool g_hook_enabled = false;
void* g_original = nullptr;

LinkState g_link = LinkState::Idle;
std::string g_message;
bool g_is_error = false;
std::string g_last_logged;
std::uint64_t g_last_attempt_ms = 0;

// Diagnostic transition logging for real-client verification (level_id / cursor progression).
std::uint32_t g_last_reported_serial = 0;
bool g_last_reported_in_battle = false;
std::int32_t g_last_reported_bucket = -1;

// Game-thread reads these. g_active gates the publish work (the original call always continues).
DetourState g_detour;
std::atomic<bool> g_active{false};
std::atomic<std::uint32_t> g_in_flight{0};

// Seqlock: odd sequence = write in progress.
Sample g_live;
std::atomic<std::uint32_t> g_sequence{0};
std::atomic<bool> g_published{false};

// Projection request/result tables. Requests are written by the provider (render thread), results by
// the detour (game thread); both use a single-writer seqlock and only copy POD buffers.
ProjectionRequest g_request;
std::atomic<std::uint32_t> g_request_sequence{0};

ProjectionResult g_projection;
std::atomic<std::uint32_t> g_projection_sequence{0};
std::atomic<bool> g_projection_published{false};

UnitPayload g_units;
std::atomic<std::uint32_t> g_units_sequence{0};
std::atomic<bool> g_units_published{false};
EnemyPayload g_enemies;
std::mutex g_enemy_mutex;
live::EnemyReader g_enemy_reader;
live::RangeReader g_range_reader;
live::PathReader g_path_reader;
live::OperatorReader g_operator_reader;
std::atomic<bool> g_range_enabled{false};
std::atomic<bool> g_operator_enabled{false};
OperatorPayload g_operators;
std::mutex g_operator_mutex;
PathRequest g_path_request;
std::mutex g_path_request_mutex;
PathPayload g_paths;
std::mutex g_path_mutex;
std::uint32_t g_operator_failure_serial = ~0u;
std::uint32_t g_path_failure_serial = ~0u;
std::uint32_t g_enemy_report_serial = 0;
std::uint32_t g_enemy_report_mask = 0;
std::mutex g_stage_mutex;
Stage g_runtime_stage;
std::uint32_t g_runtime_stage_serial = 0;
std::uint64_t g_stage_attempt_ms = 0;
bool g_stage_complete = false;

// Kept alive for the detour: managed invocation and field reads need the bound exports.
il2cpp::Runtime* g_runtime = nullptr;
std::unique_ptr<il2cpp::Runtime> g_owned_runtime;

void log(std::uint32_t level, const std::string& message) {
    if (g_host.log && g_host.owner) {
        g_host.log(g_host.owner, level, message.c_str());
    }
}

void log_optional_failure_once(std::uint32_t& reported, std::uint32_t serial,
                               const char* feature, const std::string& reason) {
    if (reported == serial) return;
    reported = serial;
    log(kLogInfo, std::string(feature) + " capture unavailable serial=" + std::to_string(serial) + ": " + reason);
}

void set_message(std::string message, bool is_error) {
    g_message = std::move(message);
    g_is_error = is_error;
    if (g_hook == 0) {
        g_link = is_error ? LinkState::Failed : LinkState::Connecting;
    }
    // The connect path runs at 1 Hz; log only when the text actually changes to keep the log useful.
    if (g_message != g_last_logged) {
        g_last_logged = g_message;
        log(is_error ? kLogError : kLogInfo, "ArkHelper 采集层：" + g_message);
    }
}

void* find_image(il2cpp::Runtime& runtime, const char* file_name) {
    std::size_t count = 0;
    void** assemblies = runtime.domain_assemblies(runtime.domain_get(), &count);
    if (!assemblies) {
        return nullptr;
    }
    for (std::size_t index = 0; index < count; ++index) {
        void* image = runtime.assembly_image(assemblies[index]);
        if (!image) {
            continue;
        }
        const char* name = runtime.image_name(image);
        if (name && std::strcmp(name, file_name) == 0) {
            return image;
        }
    }
    return nullptr;
}

// Finds the first method with the given name/arity declared on `klass` or its nearest base that has
// an executable native entry point. Deliberately avoids Runtime::method(), which throws on
// same-arity overloads (BattleController.Update has no stable signature to disambiguate).
void* find_native_method(il2cpp::Runtime& runtime, void* klass, const char* name,
                         std::uint32_t param_count) {
    for (void* owner = klass; owner; owner = runtime.class_parent(owner)) {
        void* iterator = nullptr;
        while (void* candidate = runtime.class_methods(owner, &iterator)) {
            if (std::strcmp(runtime.method_name(candidate), name) != 0 ||
                runtime.method_count(candidate) != param_count) {
                continue;
            }
            void* entry = *reinterpret_cast<void**>(candidate);
            if (entry && il2cpp::is_executable(entry)) {
                return candidate;
            }
        }
    }
    return nullptr;
}

Getter resolve_getter(il2cpp::Runtime& runtime, void* klass, const char* name) {
    Getter getter;
    getter.method = find_native_method(runtime, klass, name, 0);
    if (!getter.method) {
        throw std::runtime_error(std::string("缺少可执行方法：") + name);
    }
    getter.entry = *reinterpret_cast<void**>(getter.method);
    if (!getter.entry || !il2cpp::is_executable(getter.entry)) {
        throw std::runtime_error(std::string("方法入口不可执行：") + name);
    }
    std::uint32_t implementation_flags = 0;
    const std::uint32_t flags = runtime.method_flags(getter.method, &implementation_flags);
    getter.is_static = (flags & kMethodAttributeStatic) != 0;
    return getter;
}

// Optional clock capabilities never prevent the core clock/enemy capture from connecting. Check
// the actual return type before unboxing. Invocation also uses the runtime's managed-exception
// output boundary, so a client/hotfix exception does not escape into BattleController.Update.
Getter resolve_clock_getter(il2cpp::Runtime& runtime, void* klass, const char* name,
                            const char* expected_type) {
    try {
        Getter result = resolve_getter(runtime, klass, name);
        void* returned = runtime.class_from_type(runtime.method_return(result.method));
        if (returned && runtime.klass_name(returned) == expected_type) return result;
    } catch (...) {}
    log(kLogInfo, std::string("Battle clock capability unavailable: ") + name);
    return {};
}

// Copies a managed System.String into a UTF-8 buffer. Returns false for null/oversized strings.
bool copy_managed_string(const DetourState& state, void* value, char* out, std::size_t capacity) {
    if (!value || !state.string_length || !state.string_chars || capacity < 2) {
        return false;
    }
    const int length = state.string_length(value);
    if (length <= 0 || length > kMaxManagedStringLength) {
        return false;
    }
    const wchar_t* chars = state.string_chars(value);
    if (!chars) {
        return false;
    }
    const int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, chars, length, out,
                                            static_cast<int>(capacity) - 1, nullptr, nullptr);
    if (written <= 0) {
        return false;
    }
    out[written] = '\0';
    return true;
}

void write_sample(const Sample& sample) {
    const std::uint32_t sequence = g_sequence.load(std::memory_order_relaxed);
    g_sequence.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    g_live = sample;
    std::atomic_thread_fence(std::memory_order_release);
    g_sequence.store(sequence + 2, std::memory_order_relaxed);
    g_published.store(true, std::memory_order_release);
}

bool read_sample(Sample& out) {
    if (!g_published.load(std::memory_order_acquire)) {
        return false;
    }
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::uint32_t before = g_sequence.load(std::memory_order_acquire);
        if ((before & 1u) != 0) {
            continue;
        }
        const Sample copy = g_live;
        std::atomic_thread_fence(std::memory_order_acquire);
        const std::uint32_t after = g_sequence.load(std::memory_order_acquire);
        if (before == after) {
            out = copy;
            return true;
        }
    }
    return false;
}


bool read_request(ProjectionRequest& out) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::uint32_t before = g_request_sequence.load(std::memory_order_acquire);
        if ((before & 1u) != 0) {
            continue;
        }
        const ProjectionRequest copy = g_request;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_request_sequence.load(std::memory_order_acquire) == before) {
            out = copy;
            return true;
        }
    }
    return false;
}


void publish_units(const UnitPayload& payload) {
    const std::uint32_t sequence = g_units_sequence.load(std::memory_order_relaxed);
    g_units_sequence.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    g_units = payload;
    std::atomic_thread_fence(std::memory_order_release);
    g_units_sequence.store(sequence + 2, std::memory_order_relaxed);
    g_units_published.store(true, std::memory_order_release);
}

bool read_units(UnitPayload& out) {
    if (!g_units_published.load(std::memory_order_acquire)) {
        return false;
    }
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::uint32_t before = g_units_sequence.load(std::memory_order_acquire);
        if ((before & 1u) != 0) {
            continue;
        }
        const UnitPayload copy = g_units;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_units_sequence.load(std::memory_order_acquire) == before) {
            out = copy;
            return true;
        }
    }
    return false;
}

void publish_projection(const ProjectionResult& result) {
    const std::uint32_t sequence = g_projection_sequence.load(std::memory_order_relaxed);
    g_projection_sequence.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    g_projection = result;
    std::atomic_thread_fence(std::memory_order_release);
    g_projection_sequence.store(sequence + 2, std::memory_order_relaxed);
    g_projection_published.store(true, std::memory_order_release);
}

bool read_projection(ProjectionResult& out) {
    if (!g_projection_published.load(std::memory_order_acquire)) {
        return false;
    }
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::uint32_t before = g_projection_sequence.load(std::memory_order_acquire);
        if ((before & 1u) != 0) {
            continue;
        }
        const ProjectionResult copy = g_projection;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_projection_sequence.load(std::memory_order_acquire) == before) {
            out = copy;
            return true;
        }
    }
    return false;
}

int unbox_int32(il2cpp::Runtime& runtime, void* boxed) {
    if (!boxed) {
        return 0;
    }
    std::int32_t value = 0;
    std::memcpy(&value, runtime.object_unbox(boxed), sizeof(value));
    return static_cast<int>(value);
}

Vector3f unbox_vector3(il2cpp::Runtime& runtime, void* boxed) {
    Vector3f value{};
    if (boxed && runtime.object_unbox) {
        std::memcpy(&value, runtime.object_unbox(boxed), sizeof(value));
    }
    return value;
}

// UnorderedArray`1 exposes no get_Count in this build; fall back to its backing array + size field.
void probe_unit_container(il2cpp::Runtime& runtime, void* container, UnitApi& api) {
    api.container_probed = true;
    void* klass = runtime.object_class(container);
    if (!klass) {
        log(kLogError, "units: probe failed, container class unknown");
        return;
    }
    std::string dump = "units container probe class=" + runtime.klass_name(klass) + " fields=[";
    void* cursor = nullptr;
    int seen = 0;
    std::vector<void*> fields;
    while (void* field = runtime.class_fields(klass, &cursor)) {
        const char* name = runtime.field_name(field);
        void* type = runtime.field_type(field);
        void* type_klass = type ? runtime.class_from_type(type) : nullptr;
        const std::string type_name = type_klass ? runtime.klass_name(type_klass) : "?";
        if (seen++ < 40) {
            dump += name ? name : "?";
            dump += ":";
            dump += type_name;
            dump += " ";
        }
        fields.push_back(field);
    }
    dump += "]";
    log(kLogInfo, dump);
    for (void* field : fields) {
        const char* name = runtime.field_name(field);
        if (!name) {
            continue;
        }
        void* type = runtime.field_type(field);
        void* type_klass = type ? runtime.class_from_type(type) : nullptr;
        const std::string type_name = type_klass ? runtime.klass_name(type_klass) : "";
        const std::string field_name(name);
        if (!api.container_items_field && type_klass && runtime.class_rank(type_klass) > 0) {
            api.container_items_field = field;
            log(kLogInfo, "units container items field=" + field_name + " type=" + type_name);
        } else if (!api.container_size_field && !type_name.empty()) {
            if (std::strstr(type_name.c_str(), "Int32")) {
                api.container_size_field = field;
                api.container_size_kind = 2;
            } else if (std::strstr(type_name.c_str(), "UInt32")) {
                api.container_size_field = field;
                api.container_size_kind = 3;
            }
        }
    }
}

// Runs on the Unity main thread inside the detour: two field reads and (per requested tile) one
// camera projection. Any managed exception or missing object silently keeps the previous result.

UnitApi resolve_units(il2cpp::Runtime& runtime, void* battle_klass, const ProjectionApi& projection) {
    UnitApi api;
    api.component_get_transform = projection.component_get_transform;
    api.transform_get_position = projection.transform_get_position;
    api.camera_get_main = projection.camera_get_main;
    api.camera_world_to_viewport = projection.camera_world_to_viewport;
    api.camera_world_to_screen = projection.camera_world_to_screen;
    api.screen_get_width = projection.screen_get_width;
    api.screen_get_height = projection.screen_get_height;
    api.battle_unit_manager = runtime.field(battle_klass, "<unitManager>k__BackingField");
    if (!api.battle_unit_manager) {
        return api;
    }
    void* manager_type = runtime.field_type(api.battle_unit_manager);
    void* manager_class = manager_type ? runtime.class_from_type(manager_type) : nullptr;
    if (!manager_class) {
        return api;
    }
    api.manager_enemies = runtime.field(manager_class, "<enemies>k__BackingField");
    api.manager_characters = live::field(runtime, manager_class, "<characters>k__BackingField");
    if (!api.manager_enemies) {
        return api;
    }
    void* array_type = runtime.field_type(api.manager_enemies);
    void* array_class = array_type ? runtime.class_from_type(array_type) : nullptr;
    if (!array_class) {
        return api;
    }
    for (const char* name : {"get_Count", "get_count", "get_Length", "get_length", "Count",
                             "get_Size", "get_size"}) {
        api.array_count = find_native_method(runtime, array_class, name, 0);
        if (api.array_count) {
            break;
        }
    }
    for (const char* name : {"get_Item", "get_item", "Get", "get_Value"}) {
        api.array_item = find_native_method(runtime, array_class, name, 1);
        if (api.array_item) {
            break;
        }
    }
    if (!api.array_item) {
        return api;
    }
    void* item_return = runtime.method_return(api.array_item);
    api.unit_class = item_return ? runtime.class_from_type(item_return) : nullptr;
    if (!api.unit_class) {
        return api;
    }
    {
        void* chain = api.unit_class;
        for (int depth = 0; depth < 4 && chain != nullptr; ++depth) {
            std::string fields;
            void* cursor = nullptr;
            int count = 0;
            while (void* field = runtime.class_fields(chain, &cursor)) {
                if (count >= 80) {
                    break;
                }
                const char* name = runtime.field_name(field);
                void* type = runtime.field_type(field);
                void* klass = type ? runtime.class_from_type(type) : nullptr;
                fields += std::string(name ? name : "?") + ":" +
                          (klass ? runtime.klass_name(klass) : std::string("?")) + ",";
                ++count;
            }
            log(kLogInfo, "unit class " + runtime.klass_name(chain) + " fields=" + fields);
            chain = runtime.class_parent(chain);
        }
        const auto dump_class = [&runtime](const char* ns, const char* name) {
            void* klass = runtime.find(ns, name);
            if (!klass) {
                log(kLogInfo, std::string("class dump missing: ") + ns + "." + name);
                return;
            }
            std::string fields;
            void* cursor = nullptr;
            int count = 0;
            while (void* field = runtime.class_fields(klass, &cursor)) {
                if (count++ >= 40) {
                    break;
                }
                fields += std::string(runtime.field_name(field)) + ",";
            }
            std::string methods;
            void* mcursor = nullptr;
            count = 0;
            while (void* method = runtime.class_methods(klass, &mcursor)) {
                if (count++ >= 60) {
                    break;
                }
                methods += std::string(runtime.method_name(method)) + "/" +
                           std::to_string(runtime.method_count(method)) + ",";
            }
            log(kLogInfo, "class dump " + runtime.klass_name(klass) + " fields=[" + fields +
                          "] methods=[" + methods + "]");
        };
        dump_class("Torappu", "FP");
        dump_class("Torappu.Battle", "Attributes");
    }
    // Torappu.FP / float / int hp fields: scan the class chain for a name pair.
    const auto kind_of = [&runtime](void* field) {
        void* type = runtime.field_type(field);
        void* klass = type ? runtime.class_from_type(type) : nullptr;
        if (!klass) {
            return 0;
        }
        const std::string name = runtime.klass_name(klass);
        if (name == "System.Single") {
            return 1;
        }
        if (name == "System.Int32") {
            return 2;
        }
        if (name == "Torappu.FP") {
            return 3;
        }
        return 0;
    };
    const auto is_hp_name = [](const char* name) {
        return name && (std::strcmp(name, "m_hp") == 0 ||
                        std::strcmp(name, "<hp>k__BackingField") == 0 ||
                        std::strcmp(name, "<maxHp>k__BackingField") == 0);
    };
    void* klass = api.unit_class;
    for (int depth = 0; depth < 5 && klass != nullptr; ++depth) {
        void* cursor = nullptr;
        while (void* field = runtime.class_fields(klass, &cursor)) {
            const char* name = runtime.field_name(field);
            if (!is_hp_name(name)) {
                continue;
            }
            const int kind = kind_of(field);
            if (kind == 0) {
                continue;
            }
            const std::string lower(name);
            const bool is_max = lower.find("max") != std::string::npos;
            if (is_max && !api.max_hp_field) {
                api.max_hp_field = field;
                api.max_kind = kind;
            } else if (!is_max && !api.hp_field) {
                api.hp_field = field;
                api.hp_kind = kind;
            }
        }
        klass = runtime.class_parent(klass);
    }
    log(kLogInfo, "Unit API resolve: class=" + runtime.klass_name(api.unit_class) +
                  " hp=" + (api.hp_method ? "getter" : (api.hp_field ? "field" : "missing")) +
                  " max=" + (api.max_hp_method ? "getter" : (api.hp_ratio_method ? "ratio" : (api.max_hp_field ? "field" : "missing"))));
    if (!api.hp_field) {
        // Accessor fallback: Unit.get_hpToShow() and Unit.get_hpRatioToShow() are stable public
        // getters; the max value can be derived from hp / ratio when no max accessor exists.
        api.hp_method = find_native_method(runtime, api.unit_class, "get_hpToShow", 0);
        for (const char* name : {"get_maxHpToShow", "get_maxHp", "get_hpMax", "get_healthMax",
                                 "get_maxHealth"}) {
            api.max_hp_method = find_native_method(runtime, api.unit_class, name, 0);
            if (api.max_hp_method) {
                break;
            }
        }
        if (!api.max_hp_method) {
            api.hp_ratio_method = find_native_method(runtime, api.unit_class, "get_hpRatioToShow", 0);
        }
    }
    return api;
}

float read_fp(il2cpp::Runtime& runtime, void* object, void* field, void* to_float) {
    (void)to_float;
    void* klass = runtime.class_from_type(runtime.field_type(field));
    std::uint32_t alignment = 0;
    if (!klass || runtime.klass_name(klass) != "Torappu.FP" ||
        runtime.class_value_size(klass, &alignment) != 8) return 0;
    std::int64_t raw = 0;
    runtime.field_get(object, field, &raw);
    return static_cast<float>(static_cast<double>(raw) / kFixedPointOne);
}

float read_scalar(il2cpp::Runtime& runtime, void* object, void* field, int kind) {
    if (kind == 1) {
        float value = 0.0f;
        runtime.field_get(object, field, &value);
        return value;
    }
    if (kind == 2) {
        std::int32_t value = 0;
        runtime.field_get(object, field, &value);
        return static_cast<float>(value);
    }
    return 0.0f;
}

struct RangeProjection {
    range_geometry::MapPlane plane;
    range_geometry::Matrix4 view, projection;
    bool valid = false;
};

// Matrix projection is local arithmetic for all 16 perimeter points. A handful of game calls
// calibrate the actual map plane/camera once per 4 Hz scan, rather than boxing 1024 Vector3s.
RangeProjection prepare_range_projection(il2cpp::Runtime& runtime, const ProjectionApi& api,
                                         void* controller, void* camera) {
    RangeProjection result;
    if (!camera || !api.battle_get_map || !api.map_to_world || !api.camera_view_matrix ||
        !api.camera_projection_matrix || !api.camera_world_to_viewport) return result;
    void* map = runtime.invoke(api.battle_get_map, controller);
    if (!map) return result;
    const auto read_matrix = [&runtime, camera](void* method, range_geometry::Matrix4& out) {
        void* boxed = runtime.invoke(method, camera);
        if (!boxed || runtime.klass_name(runtime.object_class(boxed)) != "UnityEngine.Matrix4x4") return false;
        out = runtime.unbox<range_geometry::Matrix4>(boxed);
        for (float value : out.values) if (!std::isfinite(value)) return false;
        return true;
    };
    if (!read_matrix(api.camera_view_matrix, result.view) ||
        !read_matrix(api.camera_projection_matrix, result.projection)) return result;
    range_geometry::Point2 points[] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    range_geometry::Point3 world[4]{};
    for (int i = 0; i < 4; ++i) {
        void* boxed = runtime.invoke(api.map_to_world, map, {&points[i]});
        if (!boxed || runtime.klass_name(runtime.object_class(boxed)) != "UnityEngine.Vector3") return result;
        world[i] = runtime.unbox<range_geometry::Point3>(boxed);
        if (!range_geometry::finite(world[i])) return result;
    }
    result.plane.origin = world[0];
    result.plane.x_axis = {world[1].x-world[0].x, world[1].y-world[0].y, world[1].z-world[0].z};
    result.plane.y_axis = {world[2].x-world[0].x, world[2].y-world[0].y, world[2].z-world[0].z};
    const auto check = range_geometry::world(result.plane, points[3]);
    if (std::abs(check.x-world[3].x) > .001f || std::abs(check.y-world[3].y) > .001f ||
        std::abs(check.z-world[3].z) > .001f) return result;
    // Actual Unity viewport results validate matrix ordering and projection conventions. If a
    // client/camera uses a different convention, do not draw a guessed ellipse.
    for (int i = 0; i < 3; ++i) {
        void* boxed = runtime.invoke(api.camera_world_to_viewport, camera, {&world[i]});
        if (!boxed) return result;
        const auto actual = runtime.unbox<range_geometry::Point3>(boxed);
        range_geometry::Point2 predicted;
        if (!range_geometry::finite(actual) ||
            !range_geometry::project(result.view, result.projection, world[i], predicted) ||
            actual.z <= 0 || std::abs(actual.x-predicted.x) > .001f ||
            std::abs(actual.y-predicted.y) > .001f) return result;
    }
    result.valid = true;
    return result;
}

void* current_battle_camera(il2cpp::Runtime& runtime, const ProjectionApi& api) {
    // Modes with multiple map layers can render through CameraController's replacement camera.
    // Resolve its current instance on the game thread; never retain managed scene references.
    if (api.battle_camera_instance && api.battle_camera) {
        try {
            void* controller = runtime.invoke(api.battle_camera_instance, nullptr);
            if (controller) if (void* camera = runtime.invoke(api.battle_camera, controller)) return camera;
        } catch (...) {}
    }
    return api.camera_get_main ? runtime.invoke(api.camera_get_main, nullptr) : nullptr;
}

void update_operators(il2cpp::Runtime& runtime, const DetourState& state, void* manager,
                      void* camera, OperatorPayload& output) {
    if (!g_operator_enabled.load(std::memory_order_relaxed)) return;
    if (!state.units.manager_characters ||
        !manager || !camera || !state.units.component_get_transform ||
        !state.units.transform_get_position || !state.units.camera_world_to_viewport) {
        log_optional_failure_once(g_operator_failure_serial,state.serial,"Operator","unit list or camera API unavailable");
        return;
    }
    void* characters = runtime.field_get_object(state.units.manager_characters, manager);
    void* items = nullptr;
    int count = 0;
    if (!live::operator_sequence(runtime, characters, items, count)) {
        log_optional_failure_once(g_operator_failure_serial,state.serial,"Operator","characters backing array/count unavailable");
        return;
    }
    for (int index = 0; index < count && output.count < kMaxUnits; ++index) {
        try {
            void* character = runtime.at(items, index);
            std::string id;
            std::uint64_t uid = 0;
            if (!g_operator_reader.read(runtime, character, id, uid)) continue;
            Vector3f viewport;
            bool projected = false;
            try {
                void* transform = runtime.invoke(state.units.component_get_transform, character);
                if (transform) {
                    Vector3f world = unbox_vector3(runtime, runtime.invoke(state.units.transform_get_position, transform));
                    viewport = unbox_vector3(runtime, runtime.invoke(state.units.camera_world_to_viewport, camera, {&world}));
                    projected = std::isfinite(viewport.x) && std::isfinite(viewport.y) && std::isfinite(viewport.z) &&
                        viewport.z > 0 && viewport.x >= -.1f && viewport.x <= 1.1f &&
                        viewport.y >= -.1f && viewport.y <= 1.1f;
                }
            } catch (...) {}
            operator_samples::append(output.items, output.count, kMaxUnits, id.c_str(), viewport.x, viewport.y, projected, uid);
        } catch (const std::exception& error) {
            log_optional_failure_once(g_operator_failure_serial,state.serial,"Operator",error.what());
        } catch (...) {
            log_optional_failure_once(g_operator_failure_serial,state.serial,"Operator","current character read failed");
        }
    }
    static std::uint32_t reported_serial = 0, reported_count = ~0u, reported_visible = ~0u;
    static std::uint64_t reported_ms = 0;
    unsigned visible = 0;
    for (std::uint32_t i = 0; i < output.count; ++i) if (output.items[i].flags & 1u) ++visible;
    const auto now = GetTickCount64();
    if (reported_serial != state.serial || (now-reported_ms >= 2000 &&
        (reported_count != output.count || reported_visible != visible))) {
        reported_serial = state.serial; reported_count = output.count;
        reported_visible = visible; reported_ms = now;
        log(kLogInfo, "Operator instances serial=" + std::to_string(state.serial) +
            " candidates=" + std::to_string(count) + " live=" + std::to_string(output.count) +
            " projected=" + std::to_string(visible));
    }
}

struct CaptureBudget {
    LARGE_INTEGER start{}, frequency{}, now{};
    bool valid = QueryPerformanceFrequency(&frequency) && QueryPerformanceCounter(&start);
    bool available() {
        return valid && QueryPerformanceCounter(&now) &&
               (now.QuadPart-start.QuadPart)*1000 < frequency.QuadPart*2;
    }
};

PathRequest read_path_request() {
    std::unique_lock lock(g_path_request_mutex, std::try_to_lock);
    return lock ? g_path_request : PathRequest{};
}

void update_paths(il2cpp::Runtime& runtime, const DetourState& state, void* controller, void* camera,
                  const PathRequest& request, UnitPayload& units, void* const* unit_objects) {
    namespace ipc = ::arkhelp::ipc;
    PathPayload output;
    struct Publish {
        PathPayload& payload;
        ~Publish() { std::unique_lock lock(g_path_mutex, std::try_to_lock); if (lock) g_paths = payload; }
    } publish{output};
    if (!request.enabled || request.battle_serial != state.serial || request.count == 0) return;
    if (!camera) {
        log_optional_failure_once(g_path_failure_serial,state.serial,"Path","current camera unavailable"); return;
    }
    CaptureBudget budget;
    RangeProjection projection;
    try { projection = prepare_range_projection(runtime, state.projection, controller, camera); }
    catch (const std::exception& error) {
        log_optional_failure_once(g_path_failure_serial,state.serial,"Path",error.what()); return;
    } catch (...) {
        log_optional_failure_once(g_path_failure_serial,state.serial,"Path","map/camera projection read failed"); return;
    }
    if (!projection.valid) {
        log_optional_failure_once(g_path_failure_serial,state.serial,"Path","map plane/camera matrices not validated"); return;
    }
    const auto projected_ms = GetTickCount64();
    // Keep only still-requested, fresh routes so a bounded frame budget can rotate through a
    // crowd. A failed refresh clears that target immediately; stale routes expire in 1 second.
    {
        std::unique_lock lock(g_path_mutex, std::try_to_lock);
        if (lock) for (std::uint32_t r = 0; r < request.count; ++r) {
            for (std::uint32_t p = 0; p < g_paths.count; ++p) {
                const auto& old = g_paths.items[p];
                if (old.tracking_id == request.ids[r] && old.battle_serial == state.serial &&
                    old.sampled_ms && old.sampled_ms <= units.updated_ms && units.updated_ms-old.sampled_ms <= 1000)
                    output.items[output.count++] = old;
            }
        }
    }
    static std::uint32_t cursor = 0;
    const auto begin = cursor % request.count;
    std::uint32_t processed = 0;
    std::string failure = "route budget expired before first target";
    for (; processed < request.count && budget.available(); ++processed) {
        const auto id = request.ids[(begin+processed) % request.count];
        int target = -1;
        for (std::uint32_t u = 0; u < units.count; ++u) if (units.items[u].tracking_id == id) target = static_cast<int>(u);
        std::uint32_t slot = output.count;
        for (std::uint32_t p = 0; p < output.count; ++p) if (output.items[p].tracking_id == id) slot = p;
        if (slot < output.count) {
            output.items[slot] = output.items[--output.count];
            slot = output.count;
        }
        if (target < 0 || !unit_objects[target]) { failure = "requested enemy is not in the current unit sample"; continue; }
        PathSample sample;
        sample.tracking_id = id; sample.battle_serial = state.serial; sample.sampled_ms = projected_ms;
        path_geometry::Point ground;
        path_geometry::Route route;
        try {
            if (!g_range_reader.foot(runtime, unit_objects[target], ground)) {
                failure = "current enemy ground getter unavailable"; continue;
            }
            range_geometry::Point2 projected_ground;
            if (range_geometry::project(projection.view, projection.projection,
                range_geometry::world(projection.plane, ground), projected_ground)) {
                auto& unit = units.items[target];
                unit.ground_x = projected_ground.x; unit.ground_y = projected_ground.y;
                unit.flags |= ipc::kUnitGroundValid;
            }
            if (!g_path_reader.read(runtime, unit_objects[target], ground, route, sample.route_revision,
                                    [&] { return budget.available(); })) { failure = g_path_reader.reason(); continue; }
            for (std::uint32_t p = 0; p < route.count; ++p) {
                range_geometry::Point2 point;
                if (!range_geometry::project(projection.view, projection.projection,
                    range_geometry::world(projection.plane, route.points[p].point), point)) {
                    sample.point_count = 0; break;
                }
                sample.points[sample.point_count++] = {point.x, point.y, route.points[p].flags};
            }
            if (sample.point_count < 2) continue;
            sample.flags = ipc::kPathValid | (route.truncated ? ipc::kPathTruncated : ipc::kPathComplete);
            output.items[output.count++] = sample;
        } catch (const std::exception& error) { failure = error.what(); }
        catch (...) { failure = "current live route read failed"; }
    }
    if (output.count == 0) log_optional_failure_once(g_path_failure_serial,state.serial,"Path",failure);
    cursor = (begin+std::max(processed, 1u)) % request.count;
}

void update_units(DetourState& state, void* controller) {
    const auto skip_log = [](const char* reason) {
        static std::uint64_t last_ms = 0;
        const std::uint64_t now = GetTickCount64();
        if (now - last_ms >= 2000) {
            last_ms = now;
            log(kLogInfo, std::string("units skip: ") + reason);
        }
    };
    if (!state.units.ready() || g_runtime == nullptr || controller == nullptr) {
        skip_log(!state.units.ready() ? "api-not-ready" : (g_runtime == nullptr ? "no-runtime" : "no-controller"));
        return;
    }
    const std::uint64_t now_ms = GetTickCount64();
    if (state.last_units_ms != 0 && now_ms - state.last_units_ms < kUnitUpdateIntervalMs) {
        return;
    }
    state.last_units_ms = now_ms;
    UnitPayload payload;
    payload.updated_ms = now_ms;
    payload.time_seconds = state.last_time_seconds;
    payload.battle_serial = state.serial;
    EnemyPayload enemies;
    enemies.updated_ms = now_ms;
    enemies.battle_serial = state.serial;
    OperatorPayload operators;
    operators.updated_ms = now_ms; operators.battle_serial = state.serial;
    struct PublishOperators {
        OperatorPayload& payload;
        ~PublishOperators() { std::unique_lock lock(g_operator_mutex, std::try_to_lock); if (lock) g_operators = payload; }
    } publish_operators{operators};
    struct PublishEnemies {
        EnemyPayload& payload;
        ~PublishEnemies() {
            std::unique_lock lock(g_enemy_mutex, std::try_to_lock);
            if (lock) g_enemies = payload;
        }
    } publish_enemies{enemies};
    char fail_reason[128] = {};
    try {
        il2cpp::Runtime& runtime = *g_runtime;
        void* manager = runtime.field_get_object(state.units.battle_unit_manager, controller);
        if (!manager) {
            std::strcpy(fail_reason, "manager=null");
            publish_units(payload);
            return;
        }
        void* camera = current_battle_camera(runtime, state.projection);
        try { update_operators(runtime, state, manager, camera, operators); }
        catch (const std::exception& error) {
            operators.count = 0;
            log_optional_failure_once(g_operator_failure_serial,state.serial,"Operator",error.what());
        } catch (...) {
            operators.count = 0;
            log_optional_failure_once(g_operator_failure_serial,state.serial,"Operator","current operator scan failed");
        }
        void* array = runtime.field_get_object(state.units.manager_enemies, manager);
        if (!array) {
            std::strcpy(fail_reason, "enemies=null");
            publish_units(payload);
            return;
        }
        // Runtime::count/at resolve the *instance* class, which is what closed generics such as
        // UnorderedArray<Unit> need; the metadata type of the field is the open generic.
        std::int32_t count = 0;
        void* items = array;
        try {
            count = runtime.count(array);
        } catch (const std::exception& error) {
            if (!state.units.container_probed) {
                probe_unit_container(runtime, array, state.units);
            }
            if (state.units.container_items_field && state.units.container_size_field) {
                items = runtime.field_get_object(state.units.container_items_field, array);
                count = static_cast<std::int32_t>(
                    read_scalar(runtime, array, state.units.container_size_field,
                                state.units.container_size_kind));
            } else {
                std::snprintf(fail_reason, sizeof(fail_reason), "count: %s", error.what());
                log(kLogInfo, std::string("units skip: count error=") + error.what());
                publish_units(payload);
                return;
            }
        }
        if (!items) {
            std::strcpy(fail_reason, "items=null");
            publish_units(payload);
            return;
        }
        const bool range_enabled = g_range_enabled.load(std::memory_order_relaxed);
        const PathRequest path_request = read_path_request();
        // Hover needs current unit anchors before the desktop can submit its first ID/serial.
        // Route reads themselves retain the stronger serial/ID checks inside update_paths.
        const bool path_enabled = path_request.enabled;
        void* range_units[kMaxUnits]{}; // Current hook only; never retained across frames.
        int used = 0;
        for (std::int32_t index = 0; index < std::min(count, 4096) && enemies.count < kMaxUnits; ++index) {
            void* unit = runtime.at(items, index);
            if (!unit) {
                continue;
            }
            EnemySample live_enemy = g_enemy_reader.read(runtime, unit, state.serial);
            if (live_enemy.enemy_key[0] && live_enemy.enemy_name[0]) {
                std::unique_lock lock(g_stage_mutex, std::try_to_lock);
                if (lock && g_runtime_stage_serial == state.serial &&
                    live::merge_enemy_name(g_runtime_stage, live_enemy.enemy_key, live_enemy.enemy_name)) {
                    ++g_runtime_stage.revision;
                }
            }
            if (!(live_enemy.stats_mask & 1u) && state.units.hp_field && state.units.hp_kind) {
                live_enemy.hp = state.units.hp_kind == 3
                    ? read_fp(runtime, unit, state.units.hp_field, nullptr)
                    : read_scalar(runtime, unit, state.units.hp_field, state.units.hp_kind);
                live_enemy.stats_mask |= 1u;
            }
            const int enemy_index = static_cast<int>(enemies.count++);
            enemies.items[enemy_index] = live_enemy;
            if (!camera || !state.units.component_get_transform || !state.units.transform_get_position) continue;
            void* transform = runtime.invoke(state.units.component_get_transform, unit);
            if (!transform) {
                continue;
            }
            Vector3f world =
                unbox_vector3(runtime, runtime.invoke(state.units.transform_get_position, transform));
            void* boxed_viewport = state.units.camera_world_to_viewport
                                       ? runtime.invoke(state.units.camera_world_to_viewport, camera, {&world})
                                       : nullptr;
            Vector3f viewport;
            if (boxed_viewport) {
                viewport = unbox_vector3(runtime, boxed_viewport);
            } else if (state.units.camera_world_to_screen && state.units.screen_get_width &&
                       state.units.screen_get_height) {
                void* boxed_screen =
                    runtime.invoke(state.units.camera_world_to_screen, camera, {&world});
                const Vector3f screen = unbox_vector3(runtime, boxed_screen);
                const int width = unbox_int32(runtime, runtime.invoke(state.units.screen_get_width, nullptr));
                const int height = unbox_int32(runtime, runtime.invoke(state.units.screen_get_height, nullptr));
                viewport.x = width > 0 ? screen.x / static_cast<float>(width) : 0.0f;
                viewport.y = height > 0 ? screen.y / static_cast<float>(height) : 0.0f;
            } else {
                continue;
            }
            if (!(viewport.x >= -0.2f && viewport.x <= 1.2f && viewport.y >= -0.2f &&
                  viewport.y <= 1.2f)) {
                continue;
            }
            UnitItem item;
            item.tracking_id = live_enemy.tracking_id;
            item.viewport_x = viewport.x;
            item.viewport_y = viewport.y;
            item.hp = static_cast<float>(live_enemy.hp);
            item.max_hp = (live_enemy.stats_mask & 2u) ? static_cast<float>(live_enemy.max_hp) : 0;
            item.flags = ::arkhelp::ipc::kUnitValid;
            if (live_enemy.stats_mask & 1u) item.flags |= ::arkhelp::ipc::kUnitHpValid;
            else item.hp = -1;
            if (!(item.flags & ::arkhelp::ipc::kUnitHpValid) && !range_enabled && !path_enabled) continue;
            range_units[used] = unit;
            payload.items[used++] = item;
        }
        payload.count = static_cast<std::uint32_t>(used);
        update_paths(runtime, state, controller, camera, path_request, payload, range_units);
        if (range_enabled && used > 0) {
            LARGE_INTEGER range_start{}, range_frequency{}, range_now{};
            bool range_budget = QueryPerformanceFrequency(&range_frequency) && QueryPerformanceCounter(&range_start);
            const auto within_range_budget = [&]() {
                return range_budget && QueryPerformanceCounter(&range_now) &&
                       (range_now.QuadPart - range_start.QuadPart) * 1000 < range_frequency.QuadPart * 2;
            };
            RangeProjection range_projection;
            if (range_budget) {
                try { range_projection = prepare_range_projection(runtime, state.projection, controller, camera); }
                catch (...) { range_projection = {}; }
                range_budget = within_range_budget() && range_projection.valid;
            }
            // Rotate the starting unit so a tight frame budget does not permanently starve
            // later entries. Untouched range data remains unavailable for this sample.
            static std::uint32_t range_cursor = 0;
            const int start = static_cast<int>(range_cursor % static_cast<std::uint32_t>(used));
            int processed = 0;
            for (; processed < used && within_range_budget(); ++processed) {
                const int index = (start + processed) % used;
                UnitItem& item = payload.items[index];
                try {
                    range_geometry::Point2 ground;
                    if (g_range_reader.foot(runtime, range_units[index], ground)) {
                        range_geometry::Point2 center;
                        if (range_geometry::project(range_projection.view, range_projection.projection,
                                                    range_geometry::world(range_projection.plane, ground), center)) {
                            item.ground_x = center.x;
                            item.ground_y = center.y;
                            item.flags |= ::arkhelp::ipc::kUnitGroundValid;
                            float radius = 0;
                            range_geometry::Outline outline;
                            if (g_range_reader.radius(runtime, range_units[index], radius) && within_range_budget() &&
                                range_geometry::project_circle(range_projection.plane, range_projection.view,
                                    range_projection.projection, ground, radius, outline)) {
                                item.range_radius = radius;
                                item.range_point_count = range_geometry::kPointCount;
                                for (std::uint32_t p = 0; p < item.range_point_count; ++p) {
                                    item.range_points[p] = {outline.points[p].x, outline.points[p].y};
                                }
                                item.flags |= ::arkhelp::ipc::kUnitRangeValid;
                            }
                        }
                    }
                } catch (...) { /* This range is unavailable; HP and other units remain valid. */ }
            }
            range_cursor = static_cast<std::uint32_t>((start + std::max(processed, 1)) % used);
        }
        int retained = 0;
        for (int i = 0; i < used; ++i) {
            if (path_enabled || (payload.items[i].flags & (::arkhelp::ipc::kUnitHpValid | ::arkhelp::ipc::kUnitRangeValid)))
                payload.items[retained++] = payload.items[i];
        }
        payload.count = static_cast<std::uint32_t>(retained);
    } catch (const std::exception& error) {
        std::snprintf(fail_reason, sizeof(fail_reason), "loop: %s", error.what());
        payload.count = 0;
    } catch (...) {
        payload.count = 0;
    }
    publish_units(payload);
    std::uint32_t available = 0;
    for (std::uint32_t i = 0; i < enemies.count; ++i) available |= enemies.items[i].stats_mask;
    if (enemies.count && (g_enemy_report_serial != state.serial || g_enemy_report_mask != available)) {
        g_enemy_report_serial = state.serial;
        g_enemy_report_mask = available;
        char diagnostic[160]{};
        std::snprintf(diagnostic, sizeof(diagnostic),
                      "Live enemy capability serial=%u instances=%u stats_mask=0x%X (post-modifier getters)",
                      state.serial, enemies.count, available);
        log(kLogInfo, diagnostic);
    }
    static char last_failure[128]{};
    if (std::strcmp(last_failure, fail_reason) != 0) {
        std::snprintf(last_failure, sizeof(last_failure), "%s", fail_reason);
        if (fail_reason[0]) log(kLogError, std::string("Enemy sample unavailable: ") + fail_reason);
    }
}

void update_projection(DetourState& state, void* controller) {
    if (!state.projection.ready() || g_runtime == nullptr || controller == nullptr) {
        return;
    }
    // Every managed call boxes the returned Vector3, so project at 4 Hz instead of every frame.
    const std::uint64_t now_ms = GetTickCount64();
    if (state.last_projection_ms != 0 && now_ms - state.last_projection_ms < 250) {
        return;
    }
    state.last_projection_ms = now_ms;
    ProjectionRequest request;
    if (!read_request(request) || request.count <= 0) {
        return;
    }
    ProjectionResult result;
    result.generation = request.generation;
    try {
        il2cpp::Runtime& runtime = *g_runtime;
        void* map = runtime.invoke(state.projection.battle_get_map, controller);
        if (!map) {
            return;
        }
        void* anchor = runtime.field_get_object(state.projection.map_anchor_transform, map);
        if (!anchor) {
            return;
        }
        const Vector3f origin =
            unbox_vector3(runtime, runtime.invoke(state.projection.transform_get_position, anchor));
        state.last_origin = origin;
        state.has_origin = true;
        void* camera = current_battle_camera(runtime, state.projection);
        if (!camera) {
            return;
        }
        const void* width_boxed = runtime.invoke(state.projection.screen_get_width, nullptr);
        const void* height_boxed = runtime.invoke(state.projection.screen_get_height, nullptr);
        std::int32_t width = 0;
        std::int32_t height = 0;
        if (width_boxed) {
            std::memcpy(&width, runtime.object_unbox(const_cast<void*>(width_boxed)), sizeof(width));
        }
        if (height_boxed) {
            std::memcpy(&height, runtime.object_unbox(const_cast<void*>(height_boxed)), sizeof(height));
        }
        result.unity_width = width;
        result.unity_height = height;
        result.anchor_x = origin.x;
        result.anchor_y = origin.y;
        result.anchor_z = origin.z;
        // Preferred path: read the tile's own transform, so the world position comes straight from
        // the game instead of a guessed origin/axis mapping. `_tiles` may be a 1-D array, a
        // multidimensional array, or absent; the container hierarchy is the last fallback.
        void* tiles = nullptr;
        void* tiles_container = nullptr;
        std::int32_t tiles_total = 0;
        std::int32_t array_rows = 0;
        std::int32_t array_cols = 0;
        std::int32_t child_count = 0;
        int source = 0;  // 1 = indexable array, 2 = multidimensional array, 3 = child transforms
        const std::int32_t needed = request.map_rows > 0 && request.map_cols > 0
                                        ? request.map_rows * request.map_cols
                                        : 0;
        if (state.projection.tiles_ready() && needed > 0) {
            try {
                if (state.projection.map_tiles_field) {
                    tiles = runtime.field_get_object(state.projection.map_tiles_field, map);
                }
            } catch (...) {
                tiles = nullptr;
            }
            if (tiles) {
                try {
                    tiles_total = runtime.count(tiles);
                } catch (...) {
                    tiles_total = 0;
                }
                if (tiles_total >= needed) {
                    source = 1;
                } else if (state.projection.array_get_length) {
                    try {
                        std::int32_t dimension = 0;
                        void* boxed = runtime.invoke(state.projection.array_get_length, tiles, {&dimension});
                        if (boxed) {
                            std::memcpy(&array_rows, runtime.object_unbox(boxed), sizeof(array_rows));
                        }
                        dimension = 1;
                        boxed = runtime.invoke(state.projection.array_get_length, tiles, {&dimension});
                        if (boxed) {
                            std::memcpy(&array_cols, runtime.object_unbox(boxed), sizeof(array_cols));
                        }
                        if (array_rows > 0 && array_cols > 0 &&
                            array_rows * array_cols >= needed && state.projection.array_get_value) {
                            source = 2;
                            tiles_total = array_rows * array_cols;
                        }
                    } catch (...) {
                        source = 0;
                    }
                }
            }
            if (source == 0 && state.projection.map_tiles_container) {
                try {
                    void* container = runtime.field_get_object(state.projection.map_tiles_container, map);
                    if (container) {
                        tiles_container = container;
                        if (state.projection.transform_get_child_count) {
                            void* boxed = runtime.invoke(state.projection.transform_get_child_count,
                                                         container, {});
                            if (boxed) {
                                std::memcpy(&child_count, runtime.object_unbox(boxed),
                                            sizeof(child_count));
                            }
                        }
                        if (child_count >= needed && state.projection.transform_get_child) {
                            source = 3;
                            tiles_total = child_count;
                        }
                    }
                } catch (...) {
                    source = 0;
                }
            }
        }
        result.tiles_total = tiles_total;
        result.used_tiles = source;
        const std::int32_t count =
            request.count > kMaxProjectionTiles ? kMaxProjectionTiles : request.count;
        for (std::int32_t index = 0; index < count; ++index) {
            ProjectionItem& item = result.items[index];
            item.row = request.rows[index];
            item.col = request.cols[index];
            Vector3f world{};
            bool have_world = false;
            if (source != 0) {
                try {
                    void* transform = nullptr;
                    if (source == 1) {
                        const std::int32_t tile_index =
                            static_cast<std::int32_t>(item.row) * request.map_cols + item.col;
                        if (tile_index >= 0 && tile_index < tiles_total) {
                            void* tile = runtime.at(tiles, tile_index);
                            if (tile) {
                                transform = runtime.invoke(state.projection.component_get_transform, tile);
                            }
                        }
                    } else if (source == 2) {
                        std::int32_t row = item.row;
                        std::int32_t col = item.col;
                        void* tile = runtime.invoke(state.projection.array_get_value, tiles, {&row, &col});
                        if (tile) {
                            transform = runtime.invoke(state.projection.component_get_transform, tile);
                        }
                    } else if (source == 3) {
                        std::int32_t tile_index =
                            static_cast<std::int32_t>(item.row) * request.map_cols + item.col;
                        if (tile_index >= 0 && tile_index < tiles_total) {
                            transform = runtime.invoke(state.projection.transform_get_child,
                                                       tiles_container, {&tile_index});
                        }
                    }
                    if (transform) {
                        world = unbox_vector3(
                            runtime, runtime.invoke(state.projection.transform_get_position, transform));
                        have_world = true;
                    }
                } catch (...) {
                    have_world = false;
                }
            }
            if (!have_world) {
                world.x = origin.x + static_cast<float>(item.col) * kTileWorldSize;
                world.y = origin.y;
                world.z = origin.z - static_cast<float>(item.row) * kTileWorldSize;
            } else if (result.tile_world_x == 0.0f && result.tile_world_z == 0.0f) {
                result.tile_world_x = world.x;
                result.tile_world_z = world.z;
            }
            // The route start tile is the first walkable cell inside the map; the red gate model
            // sits one tile further out. Extrapolate one tile away from the inward neighbour so the
            // countdown label lands on the gate's visual centre instead of on the spawn cell.
            if (have_world && source == 3 && tiles_container != nullptr && request.map_rows > 0 &&
                request.map_cols > 0) {
                std::int32_t neighbour_row = item.row;
                std::int32_t neighbour_col = item.col;
                bool have_neighbour = false;
                if (item.col == 0 && request.map_cols > 1) {
                    neighbour_col = 1;
                    have_neighbour = true;
                } else if (item.col == request.map_cols - 1 && request.map_cols > 1) {
                    neighbour_col = item.col - 1;
                    have_neighbour = true;
                } else if (item.row == 0 && request.map_rows > 1) {
                    neighbour_row = 1;
                    have_neighbour = true;
                } else if (item.row == request.map_rows - 1 && request.map_rows > 1) {
                    neighbour_row = item.row - 1;
                    have_neighbour = true;
                }
                if (have_neighbour) {
                    try {
                        std::int32_t neighbour_index =
                            neighbour_row * request.map_cols + neighbour_col;
                        void* neighbour_transform =
                            runtime.invoke(state.projection.transform_get_child, tiles_container,
                                            {&neighbour_index});
                        if (neighbour_transform != nullptr) {
                            const Vector3f neighbour = unbox_vector3(
                                runtime, runtime.invoke(state.projection.transform_get_position,
                                                        neighbour_transform));
                            // Half a tile out: the full tile overshoots past the gate model.
                            world.x += (world.x - neighbour.x) * 0.5f;
                            world.y += (world.y - neighbour.y) * 0.5f;
                            world.z += (world.z - neighbour.z) * 0.5f;
                        }
                    } catch (...) {
                    }
                }
            }
            float viewport_x = 0.0f;
            float viewport_y = 0.0f;
            bool have_viewport = false;
            if (state.projection.camera_world_to_viewport && width > 0 && height > 0) {
                void* boxed_viewport =
                    runtime.invoke(state.projection.camera_world_to_viewport, camera, {&world});
                if (boxed_viewport) {
                    const Vector3f viewport = unbox_vector3(runtime, boxed_viewport);
                    if (viewport.x >= 0.0f && viewport.x <= 1.0f && viewport.y >= 0.0f &&
                        viewport.y <= 1.0f) {
                        viewport_x = viewport.x;
                        viewport_y = viewport.y;
                        have_viewport = true;
                    }
                }
            }
            if (!have_viewport) {
                void* boxed =
                    runtime.invoke(state.projection.camera_world_to_screen, camera, {&world});
                if (!boxed || width <= 0 || height <= 0) {
                    continue;
                }
                const Vector3f screen = unbox_vector3(runtime, boxed);
                viewport_x = screen.x / static_cast<float>(width);
                viewport_y = screen.y / static_cast<float>(height);
            }
            // Keep one coordinate domain in this payload: Unity pixels with a top-left origin.
            // Provider publication scales to the selected current client exactly once.
            item.x = viewport_x * static_cast<float>(width);
            item.y = (1.0f - viewport_y) * static_cast<float>(height);
            item.valid = true;
            ++result.count;
        }
    } catch (...) {
        return;
    }
    publish_projection(result);
}

// Runs on the Unity main thread. The clock snapshot is POD; current-level metadata is captured
// once per battle, and enemy scans are throttled. Worker consumers never access game objects.
void publish(DetourState& state, void* controller) {
    Sample sample;
    sample.updated_ms = GetTickCount64();

    std::int32_t battle_state = 0;
    if (controller && state.battle_state_offset >= 0) {
        std::memcpy(&battle_state, static_cast<const char*>(controller) + state.battle_state_offset,
                    sizeof(battle_state));
    }
    sample.battle_state = battle_state;

    if (state.play_time.valid()) {
        sample.time_seconds =
            static_cast<double>(state.play_time.integer(controller)) / kFixedPointOne;
    }
    if (state.frame.valid()) {
        sample.frame = state.frame.counter(controller);
    }
    if (state.level_id.valid()) {
        void* level = state.level_id.reference(controller);
        if (copy_managed_string(state, level, sample.level_id, sizeof(sample.level_id))) {
            sample.flags |= kFlagLevelKnown;
        }
    }
    if (battle_state == kBattleStateRunning) {
        sample.flags |= kFlagInBattle;
        if (state.paused.valid() && g_runtime) {
            try {
                void* boxed = g_runtime->invoke(state.paused.method, controller);
                if (boxed && g_runtime->klass_name(g_runtime->object_class(boxed)) == "System.Boolean") {
                    const bool paused = g_runtime->unbox<bool>(boxed);
                    sample.flags |= kFlagPauseKnown;
                    if (paused) sample.flags |= kFlagPaused;
                }
            } catch (...) {
                // Disable only this optional capability after a throwing client getter. Other
                // status and enemy work remains usable, and the failing call is not retried at FPS.
                state.paused = {};
                try { log(kLogError, "Battle pause getter failed; pause state unavailable until reconnect"); }
                catch (...) {}
            }
        }
        if (state.time_scale.valid() && g_runtime) {
            try {
                void* boxed = g_runtime->invoke(state.time_scale.method, controller);
                if (boxed && g_runtime->klass_name(g_runtime->object_class(boxed)) == "System.Single") {
                    const float scale = g_runtime->unbox<float>(boxed);
                    if (std::isfinite(scale) && scale >= 0 && scale <= 64.f) sample.time_scale = scale;
                }
            } catch (...) {
                state.time_scale = {};
                try { log(kLogError, "Battle speed getter failed; speed notifications unavailable until reconnect"); }
                catch (...) {}
            }
        }
    }

    // A new battle shows up as a fresh Running state or as a clock that jumped backwards
    // (retry/replay of the same stage). Both bump the serial the UI uses to detect stage changes.
    if (battle_state == kBattleStateRunning) {
        const bool restarted = state.last_state != kBattleStateRunning || state.last_controller != controller ||
                               std::strcmp(state.last_level_id, sample.level_id) != 0 ||
                               sample.time_seconds + 0.5 < state.last_time_seconds;
        if (restarted) {
            ++state.serial;
            // Clear the old generation before parsing a level or attempting any scan. Even a
            // missing unit API or failed game read cannot leave last battle's labels visible.
            UnitPayload cleared;
            cleared.battle_serial = state.serial;
            publish_units(cleared);
            state.last_units_ms = 0;
            state.last_projection_ms = 0;
            g_enemy_reader.clear();
            g_range_reader.clear();
            g_path_reader.clear();
            g_operator_reader.clear();
            {
                std::unique_lock lock(g_operator_mutex, std::try_to_lock);
                if (lock) g_operators = OperatorPayload{};
            }
            {
                std::unique_lock lock(g_path_mutex, std::try_to_lock);
                if (lock) g_paths = PathPayload{};
            }
        }
    }
    state.last_state = battle_state;
    state.last_controller = controller;
    std::snprintf(state.last_level_id, sizeof(state.last_level_id), "%s", sample.level_id);
    state.last_time_seconds = sample.time_seconds;
    sample.serial = state.serial;

    // The cheap status must be visible before optional metadata/projection/enemy work. Pausing
    // and resuming are known on this Update even if a full 4 Hz unit scan is not due.
    write_sample(sample);

    if (g_runtime && controller && sample.level_id[0] &&
        (sample.serial != g_runtime_stage_serial || (!g_stage_complete &&
         sample.updated_ms - g_stage_attempt_ms >= 2000))) {
        g_stage_attempt_ms = sample.updated_ms;
        try {
            Stage current = live::stage(*g_runtime, controller, sample.level_id, sample.serial);
            g_stage_complete = current.loaded && current.overview_plan_read && !current.enemies.empty() &&
                (current.map.rows > 0 || !current.routes.empty());
            char diagnostic[320]{};
            std::snprintf(diagnostic, sizeof(diagnostic),
                          "Live stage level=%s serial=%u map=%dx%d definitions=%zu plan=%d conditional=%d prefix_spawns=%zu overview_actions=%zu partial=%d",
                          sample.level_id, sample.serial, current.map.rows, current.map.cols,
                          current.enemies.size(), current.spawn_plan_available,
                          current.spawn_plan_conditional, current.spawns.size(),
                          current.overview_spawns.size(), current.overview_partial);
            log(kLogInfo, diagnostic);
            for (const char* mode : {"isLegionMode", "isSandBox", "isSandboxV3", "isAutoChessMode", "isStrifeMode", "isCooperateMode"}) {
                if (live::scalar(*g_runtime, controller, mode)) log(kLogInfo, std::string("Live battle mode: ") + mode);
            }
            std::unique_lock lock(g_stage_mutex, std::try_to_lock);
            if (lock) {
                current.revision = g_runtime_stage.revision + 1;
                g_runtime_stage = std::move(current);
                g_runtime_stage_serial = sample.serial;
            } else g_stage_complete = false;
        } catch (...) {
            // No fallback to another level or offline NORMAL difficulty.
            std::unique_lock lock(g_stage_mutex, std::try_to_lock);
            if (lock) {
                const auto revision = g_runtime_stage.revision + 1;
                g_runtime_stage = Stage{};
                g_runtime_stage.runtime = true;
                g_runtime_stage.level_id = sample.level_id;
                g_runtime_stage.stage_id = sample.level_id;
                g_runtime_stage.revision = revision;
                g_runtime_stage_serial = sample.serial;
            }
        }
    }

    update_projection(state, controller);
    update_units(state, controller);

}

// The detour. The original gate must run in every path so the game keeps updating; the core
// bypasses this link entirely while the hook is disabled, so no extra guard is needed for that.
void battle_update_detour(void* self, void* method_info) {
    if (g_original) {
        reinterpret_cast<void (*)(void*, void*)>(g_original)(self, method_info);
    }
    if (!g_active.load(std::memory_order_acquire)) {
        return;
    }
    g_in_flight.fetch_add(1, std::memory_order_acq_rel);
    struct LeaveCapture {
        ~LeaveCapture() { g_in_flight.fetch_sub(1, std::memory_order_acq_rel); }
    } leave_capture;
    // The game's own Update above retains its original exception behavior. Only ArkHelper's
    // post-update work is isolated; an optional capture error cannot escape into the game loop.
    try {
        publish(g_detour, self);
    } catch (...) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            try { log(kLogError, "Battle capture callback failed; current sample skipped"); }
            catch (...) {}
        }
    }
}


// Walks the class chain for a field (fields are not inherited through class_field()).
void* find_field_in_chain(il2cpp::Runtime& runtime, void* klass, const char* name) {
    for (void* owner = klass; owner; owner = runtime.class_parent(owner)) {
        void* field = runtime.class_field(owner, name);
        if (field) {
            return field;
        }
    }
    return nullptr;
}

ProjectionApi resolve_projection(il2cpp::Runtime& runtime, void* battle_klass) {
    ProjectionApi api;
    try {
        void* map_method = find_native_method(runtime, battle_klass, "get_map", 0);
        if (map_method) {
            api.battle_get_map = map_method;
            void* map_type = runtime.method_return(map_method);
            void* map_class = map_type ? runtime.class_from_type(map_type) : nullptr;
            if (map_class) {
                api.map_class = map_class;
                api.map_anchor_transform = find_field_in_chain(runtime, map_class, "_anchorTransform");
                api.map_tiles_field = find_field_in_chain(runtime, map_class, "_tiles");
                api.map_tiles_container = find_field_in_chain(runtime, map_class, "_tilesContainer");
                api.map_to_world = find_native_method(runtime, map_class, "MapToWorldPosition", 1);
            }
        }
        api.tile_class = runtime.find("Torappu.Battle", "Tile");
        void* component_class = runtime.find("UnityEngine", "Component");
        void* transform_class = runtime.find("UnityEngine", "Transform");
        void* array_class = runtime.find("System", "Array");
        void* camera_class = runtime.find("UnityEngine", "Camera");
        void* screen_class = runtime.find("UnityEngine", "Screen");
        try {
            void* battle_camera_class = runtime.find("Torappu.Battle", "CameraController");
            api.battle_camera_instance = find_native_method(runtime, battle_camera_class, "get_instance", 0);
            api.battle_camera = find_native_method(runtime, battle_camera_class, "get_camera", 0);
        } catch (...) {}
        if (component_class) {
            api.component_get_transform =
                find_native_method(runtime, component_class, "get_transform", 0);
        }
        if (transform_class) {
            api.transform_get_position = find_native_method(runtime, transform_class, "get_position", 0);
            api.transform_get_child = find_native_method(runtime, transform_class, "GetChild", 1);
            api.transform_get_child_count =
                find_native_method(runtime, transform_class, "get_childCount", 0);
        }
        if (array_class) {
            api.array_get_length = find_native_method(runtime, array_class, "GetLength", 1);
            api.array_get_value = find_native_method(runtime, array_class, "GetValue", 2);
        }
        if (camera_class) {
            api.camera_get_main = find_native_method(runtime, camera_class, "get_main", 0);
            api.camera_world_to_screen =
                find_native_method(runtime, camera_class, "WorldToScreenPoint", 1);
            api.camera_world_to_viewport =
                find_native_method(runtime, camera_class, "WorldToViewportPoint", 1);
            api.camera_view_matrix = find_native_method(runtime, camera_class, "get_worldToCameraMatrix", 0);
            api.camera_projection_matrix = find_native_method(runtime, camera_class, "get_projectionMatrix", 0);
        }
        if (screen_class) {
            api.screen_get_width = find_native_method(runtime, screen_class, "get_width", 0);
            api.screen_get_height = find_native_method(runtime, screen_class, "get_height", 0);
        }
    } catch (...) {
        return ProjectionApi{};
    }
    return api;
}

void install_hook(il2cpp::Runtime& runtime, void* update_entry, const Getter& level_id,
                  const Getter& play_time, const Getter& frame, const Getter& paused,
                  const Getter& time_scale, int battle_state_offset,
                  const ProjectionApi& projection, const UnitApi& units) {
    // Reset the shared snapshot before the detour can fire.
    g_detour = DetourState{};
    g_detour.level_id = level_id;
    g_detour.play_time = play_time;
    g_detour.frame = frame;
    g_detour.paused = paused;
    g_detour.time_scale = time_scale;
    g_detour.string_length = runtime.string_length;
    g_detour.string_chars = runtime.string_chars;
    g_detour.battle_state_offset = battle_state_offset;
    g_detour.projection = projection;
    g_detour.units = units;
    g_sequence.store(0, std::memory_order_relaxed);
    g_published.store(false, std::memory_order_relaxed);
    g_live = Sample{};
    g_original = nullptr;

    const std::uint64_t hook = g_host.create_native_hook(
        g_host.owner, update_entry, reinterpret_cast<void*>(&battle_update_detour), &g_original);
    if (hook == 0) {
        throw std::runtime_error("create_native_hook 失败");
    }
    g_active.store(true, std::memory_order_release);
    if (!g_host.enable_native_hook(g_host.owner, hook, true)) {
        g_active.store(false, std::memory_order_release);
        g_host.remove_native_hook(g_host.owner, hook);
        g_original = nullptr;
        throw std::runtime_error("enable_native_hook 失败");
    }
    g_hook = hook;
    g_hook_enabled = true;
    g_link = LinkState::Connected;
    g_message.clear();
    g_last_logged.clear();
    g_is_error = false;
    log(kLogInfo, "ArkHelper 采集层：已连接 BattleController.Update（关卡/时间/帧/状态采样就绪）");
}

// One-shot metadata summary so the log tells exactly which tile source is available.
std::string describe_projection_api(il2cpp::Runtime& runtime, const ProjectionApi& api) {
    std::string text = "投影 API:";
    text += api.ready() ? " ready" : " incomplete";
    if (api.map_tiles_field) {
        void* field_type = runtime.field_type(api.map_tiles_field);
        void* klass = field_type ? runtime.class_from_type(field_type) : nullptr;
        text += " tiles_type=";
        text += klass ? runtime.klass_name(klass) : "?";
    } else {
        text += " tiles_type=missing";
    }
    text += api.map_tiles_container ? " container=yes" : " container=no";
    if (api.tile_class) {
        text += " tile_class=" + runtime.klass_name(api.tile_class);
    }
    if (api.map_class) {
        std::string methods;
        void* cursor = nullptr;
        int seen = 0;
        try {
            while (void* method = runtime.class_methods(api.map_class, &cursor)) {
                if (seen++ >= 24) {
                    methods += "…";
                    break;
                }
                const char* name = runtime.method_name(method);
                methods += name ? name : "?";
                methods += "/";
                methods += std::to_string(runtime.method_count(method));
                methods += " ";
            }
        } catch (...) {
            methods += "<exception>";
        }
        text += " map_methods=[" + methods + "]";
    }
    return text;
}

// One-shot metadata summary for the unit/enemy layer: HP bars and in-world damage numbers need the
// live unit list (positions + hp), which is not covered by the projection dump above.
std::string describe_battle_entities(il2cpp::Runtime& runtime, void* battle_klass) {
    std::string text = "战斗实体 API:";
    void* unit_manager = nullptr;
    try {
        void* field = runtime.field(battle_klass, "<unitManager>k__BackingField");
        if (field != nullptr) {
            void* type = runtime.field_type(field);
            void* klass = type ? runtime.class_from_type(type) : nullptr;
            if (klass != nullptr) {
                unit_manager = klass;
                text += " unit_manager=" + runtime.klass_name(klass);
            }
        }
    } catch (...) {
        text += " unit_manager=<exception>";
    }
    const auto dump_class = [&runtime, &text](const char* label, void* klass, int max_entries) {
        if (klass == nullptr) {
            text += " | ";
            text += label;
            text += "=missing";
            return;
        }
        text += " | ";
        text += label;
        text += " fields=[";
        int seen = 0;
        void* cursor = nullptr;
        try {
            while (void* field = runtime.class_fields(klass, &cursor)) {
                if (seen++ >= max_entries) {
                    text += "…";
                    break;
                }
                const char* name = runtime.field_name(field);
                text += name ? name : "?";
                text += ":";
                void* type = runtime.field_type(field);
                void* type_klass = type ? runtime.class_from_type(type) : nullptr;
                text += type_klass ? runtime.klass_name(type_klass) : "?";
                text += " ";
            }
        } catch (...) {
            text += "<exception>";
        }
        text += "] methods=[";
        cursor = nullptr;
        seen = 0;
        try {
            while (void* method = runtime.class_methods(klass, &cursor)) {
                if (seen++ >= max_entries) {
                    text += "…";
                    break;
                }
                const char* name = runtime.method_name(method);
                text += name ? name : "?";
                text += "/";
                text += std::to_string(runtime.method_count(method));
                text += " ";
            }
        } catch (...) {
            text += "<exception>";
        }
        text += "]";
    };
    dump_class("unit_manager", unit_manager, 40);
    // The enemy container is a generic UnorderedArray`1: its accessor names decide how we can
    // enumerate units at runtime, so dump them explicitly.
    if (unit_manager != nullptr) {
        try {
            void* enemies_field = runtime.field(unit_manager, "<enemies>k__BackingField");
            void* enemies_type = enemies_field ? runtime.field_type(enemies_field) : nullptr;
            void* enemies_class = enemies_type ? runtime.class_from_type(enemies_type) : nullptr;
            dump_class("enemies_array", enemies_class, 40);
        } catch (...) {
        }
    }
    void* image = find_image(runtime, "Assembly-CSharp.dll");
    if (image != nullptr) {
        // Class chain first: HP lives on one of the base classes of Unit.
        void* unit_class = runtime.class_from_name(image, "Torappu.Battle", "Unit");
        std::string chain;
        void* cursor_class = unit_class;
        for (int depth = 0; depth < 6 && cursor_class != nullptr; ++depth) {
            chain += runtime.klass_name(cursor_class);
            chain += " -> ";
            cursor_class = runtime.class_parent(cursor_class);
        }
        text += " | unit_chain=[" + chain + "]";
        dump_class("Unit", unit_class, 80);
        dump_class("Character", runtime.class_from_name(image, "Torappu.Battle", "Character"), 20);
        dump_class("BattleCharacterData",
                   runtime.class_from_name(image, "Torappu.Battle", "BattleCharacterData"), 40);
    }
    return text;
}

void attempt_connect() {
    g_last_attempt_ms = GetTickCount64();
    if (g_host.test_host != 0) {
        set_message("测试宿主：无游戏进程，采集层待机（不连接）", false);
        return;
    }
    if (!il2cpp::game_assembly()) {
        set_message("等待游戏进程（GameAssembly.dll 未加载）", false);
        return;
    }
    try {
        auto runtime = std::make_unique<il2cpp::Runtime>();
        runtime->initialize();
        // Metadata lookups need an attached thread; this is a short, one-shot connection step on
        // the render thread (the same pattern damage_meter uses). The attachment is dropped before
        // the hook is enabled.
        il2cpp::Runtime::Attachment attachment(*runtime);

        void* image = find_image(*runtime, "Assembly-CSharp.dll");
        if (!image) {
            set_message("等待 Assembly-CSharp.dll 加载", false);
            return;
        }
        void* klass = runtime->class_from_name(image, "Torappu.Battle", "BattleController");
        if (!klass) {
            set_message("未找到 Torappu.Battle.BattleController（客户端版本可能不兼容）", true);
            return;
        }

        const Getter level_id = resolve_getter(*runtime, klass, "get_levelId");
        const Getter play_time = resolve_getter(*runtime, klass, "get_fixedPlayTime");
        const Getter frame = resolve_getter(*runtime, klass, "get_fixedFrameCnt");
        const Getter paused = resolve_clock_getter(*runtime, klass, "get_isPaused", "System.Boolean");
        const Getter time_scale = resolve_clock_getter(*runtime, klass, "get_timeScale", "System.Single");
        log(kLogInfo, std::string("Battle clock API: pause=") + (paused.valid() ? "live-getter" : "unavailable") +
                          " speed=" + (time_scale.valid() ? "live-getter" : "unavailable"));

        void* update_method = find_native_method(*runtime, klass, "Update", 0);
        if (!update_method) {
            throw std::runtime_error("BattleController.Update 没有可执行入口");
        }
        void* update_entry = *reinterpret_cast<void**>(update_method);

        void* battle_state_field = runtime->field(klass, "m_state");
        void* state_type = runtime->field_type(battle_state_field);
        void* state_class = runtime->class_from_type(state_type);
        std::uint32_t alignment = 0;
        const int state_size = runtime->class_value_size(state_class, &alignment);
        if (state_size != static_cast<int>(sizeof(std::int32_t))) {
            throw std::runtime_error("BattleController.m_state 布局不受支持");
        }
        const auto field_offset =
            il2cpp::resolve_export<std::size_t (*)(void*)>("il2cpp_field_get_offset");
        const std::size_t raw_offset = field_offset(battle_state_field);
        if (raw_offset < sizeof(void*) || raw_offset > 65536) {
            throw std::runtime_error("BattleController.m_state 偏移不受支持");
        }
        const int state_offset = static_cast<int>(raw_offset);

        const ProjectionApi projection = resolve_projection(*runtime, klass);
        if (!projection.ready()) {
            log(kLogInfo, "ArkHelper 采集层：投影链路不完整（相机/地图不可用），出怪位置回退到列表");
        }
        log(kLogInfo, describe_projection_api(*runtime, projection));
        log(kLogInfo, describe_battle_entities(*runtime, klass));
        const UnitApi units = resolve_units(*runtime, klass, projection);
        if (units.ready()) {
            log(kLogInfo, std::string("Unit API: enemies=yes hp=") +
                              (units.hp_method ? "get_hpToShow" : "field") +
                              " max=" + (units.max_hp_method ? "getter"
                                        : (units.hp_ratio_method ? "ratio" : "missing")));
        } else {
            std::string diag = "Unit API: incomplete";
            diag += units.battle_unit_manager ? " manager_field" : " manager_field=missing";
            diag += units.manager_enemies ? " enemies_field" : " enemies_field=missing";
            diag += units.array_count ? " count" : " count=missing";
            diag += units.array_item ? " item" : " item=missing";
            diag += units.unit_class ? " unit_class" : " unit_class=missing";
            diag += units.hp_field ? " hp" : " hp=missing";
            diag += units.max_hp_field ? " max" : " max=missing";
            log(kLogInfo, diag);
        }
        g_owned_runtime = std::move(runtime);
        g_runtime = g_owned_runtime.get();
        install_hook(*g_owned_runtime, update_entry, level_id, play_time, frame, paused, time_scale, state_offset,
                     projection, units);
    } catch (const std::exception& error) {
        const std::string text = error.what();
        // The domain is still booting right after GameAssembly.dll is mapped; retry quietly.
        const bool transient = text.find("not ready") != std::string::npos;
        set_message((transient ? "等待 IL2CPP 就绪：" : "连接失败：") + text, !transient);
    } catch (...) {
        set_message("连接失败：未知异常", true);
    }
}

} // namespace

void initialize(const ArkPluginApiV1& host) {
    if (g_initialized) {
        return;
    }
    g_host = host;
    g_initialized = true;
    // Diagnostic kill switch: run the plugin without touching IL2CPP or any native hook. This is
    // how we tell an anti-cheat reaction to the hook apart from a bug in the detour itself.
    if (std::getenv("ARKHELPER_DISABLE_CAPTURE") != nullptr) {
        set_message("采集层已禁用（ARKHELPER_DISABLE_CAPTURE 诊断模式）", false);
        log(kLogInfo, "ArkHelper 采集层：已通过 ARKHELPER_DISABLE_CAPTURE 禁用（诊断模式）");
        return;
    }
    g_api_usable = g_host.create_native_hook != nullptr && g_host.enable_native_hook != nullptr &&
                   g_host.remove_native_hook != nullptr;
    if (!g_api_usable) {
        set_message("宿主 API 不支持原生钩子，采集层禁用", true);
        return;
    }
    if (g_host.test_host != 0) {
        set_message("测试宿主：无游戏进程，采集层待机（不连接）", false);
    } else {
        set_message("等待游戏进程（IL2CPP 未就绪）", false);
    }
}

// Log battle transitions and a 10-second cursor heartbeat so a real-client session can be verified
// from ArkHelperRuntime.log alone (the UI tab may be hidden while playing).
void report_transitions() {
    const Status sample = status();
    if (!sample.has_sample) {
        return;
    }
    char buffer[320] = {};
    if (sample.battle_serial != g_last_reported_serial) {
        g_last_reported_serial = sample.battle_serial;
        std::snprintf(buffer, sizeof(buffer),
                      "ArkHelper 采集层：战斗开始 serial=%u 关卡=%s 帧=%u",
                      static_cast<unsigned>(sample.battle_serial),
                      sample.level_id[0] ? sample.level_id : "未知",
                      static_cast<unsigned>(sample.frame));
        log(kLogInfo, buffer);
        g_last_reported_bucket = -1;
    }
    if (sample.in_battle != g_last_reported_in_battle) {
        g_last_reported_in_battle = sample.in_battle;
        if (sample.in_battle) {
            log(kLogInfo, "ArkHelper 采集层：进入战斗状态（BattleController.m_state=Running）");
        } else {
            std::snprintf(buffer, sizeof(buffer),
                          "ArkHelper 采集层：战斗结束 关卡=%s 游戏时间=%.1fs 帧=%u",
                          sample.level_id[0] ? sample.level_id : "未知", sample.time_seconds,
                          static_cast<unsigned>(sample.frame));
            log(kLogInfo, buffer);
            g_last_reported_bucket = -1;
        }
    }
    const std::int32_t bucket = static_cast<std::int32_t>(sample.time_seconds / 10.0);
    if (sample.in_battle && bucket != g_last_reported_bucket) {
        g_last_reported_bucket = bucket;
        std::snprintf(buffer, sizeof(buffer),
                      "ArkHelper 采集层：采样 关卡=%s 时间=%.1fs 帧=%u 状态码=%d",
                      sample.level_id[0] ? sample.level_id : "未知", sample.time_seconds,
                      static_cast<unsigned>(sample.frame), static_cast<int>(sample.battle_state));
        log(kLogInfo, buffer);
    }
}

void tick() {
    if (g_initialized && g_api_usable) {
        report_transitions();
    }
    if (!g_initialized || !g_api_usable || g_hook != 0) {
        return;
    }
    const std::uint64_t now = GetTickCount64();
    if (g_last_attempt_ms != 0 && now - g_last_attempt_ms < kConnectIntervalMs) {
        return;
    }
    attempt_connect();
}

bool shutdown() {
    if (!g_initialized && g_hook == 0) {
        return true;
    }
    // Stop publishing first; the original game call still continues through the chain.
    g_active.store(false, std::memory_order_release);

    if (g_hook != 0) {
        if (g_hook_enabled) {
            if (!g_host.enable_native_hook(g_host.owner, g_hook, false)) {
                set_message("disable_native_hook 失败，保留钩子待重试", true);
                return false;
            }
            g_hook_enabled = false;
        }
        if (!g_host.remove_native_hook(g_host.owner, g_hook)) {
            set_message("remove_native_hook 失败，保留钩子待重试", true);
            return false;
        }
        g_hook = 0;
    }

    // remove() drains the core's own entries; wait for our counter as a defensive backstop.
    const std::uint64_t deadline = GetTickCount64() + kShutdownDrainMs;
    while (g_in_flight.load(std::memory_order_acquire) != 0 && GetTickCount64() < deadline) {
        Sleep(1);
    }
    if (g_in_flight.load(std::memory_order_acquire) != 0) {
        // Never clear the detour state while a callback may still be using it; the host can retry.
        set_message("等待采集回调结束超时，保留状态待重试", true);
        return false;
    }

    g_original = nullptr;
    g_detour = DetourState{};
    g_runtime = nullptr;
    g_owned_runtime.reset();
    g_projection_published.store(false, std::memory_order_relaxed);
    g_units_published.store(false, std::memory_order_relaxed);
    g_enemy_reader.clear();
    g_range_reader.clear();
    g_path_reader.clear();
    g_operator_reader.clear();
    g_range_enabled.store(false, std::memory_order_relaxed);
    g_operator_enabled.store(false, std::memory_order_relaxed);
    g_operator_failure_serial = g_path_failure_serial = ~0u;
    { std::lock_guard lock(g_operator_mutex); g_operators = {}; }
    { std::lock_guard lock(g_path_mutex); g_paths = {}; }
    { std::lock_guard lock(g_path_request_mutex); g_path_request = {}; }
    g_enemy_report_serial = g_enemy_report_mask = 0;
    {
        std::lock_guard lock(g_enemy_mutex);
        g_enemies = EnemyPayload{};
    }
    {
        std::lock_guard lock(g_stage_mutex);
        g_runtime_stage = Stage{};
        g_runtime_stage_serial = 0;
        g_stage_attempt_ms = 0;
        g_stage_complete = false;
    }
    g_published.store(false, std::memory_order_relaxed);
    g_sequence.store(0, std::memory_order_relaxed);
    g_live = Sample{};
    g_link = LinkState::Idle;
    g_message.clear();
    g_is_error = false;
    g_last_logged.clear();
    g_last_attempt_ms = 0;
    g_initialized = false;
    g_api_usable = false;
    return true;
}

Status status() {
    Status result;
    result.link = g_link;
    result.hook_installed = g_hook != 0;
    result.is_error = g_is_error;
    if (!g_message.empty()) {
        std::snprintf(result.message, sizeof(result.message), "%s", g_message.c_str());
    }
    if (!g_initialized) {
        return result;
    }
    Sample sample;
    if (!read_sample(sample)) {
        return result;
    }
    result.has_sample = true;
    result.battle_serial = sample.serial;
    result.battle_state = sample.battle_state;
    result.frame = sample.frame;
    result.time_seconds = sample.time_seconds;
    result.time_scale = sample.time_scale;
    result.updated_ms = sample.updated_ms;
    result.in_battle = (sample.flags & kFlagInBattle) != 0;
    result.pause_known = (sample.flags & kFlagPauseKnown) != 0;
    result.paused = (sample.flags & kFlagPaused) != 0;
    if ((sample.flags & kFlagLevelKnown) != 0) {
        std::memcpy(result.level_id, sample.level_id, sizeof(result.level_id));
        result.level_id[sizeof(result.level_id) - 1] = '\0';
    }
    const std::uint64_t now = GetTickCount64();
    result.stale = result.in_battle && sample.updated_ms != 0 && now > sample.updated_ms &&
                   (now - sample.updated_ms) > kStaleAfterMs;
    if (result.has_sample && result.battle_state == kBattleStateFinished) {
        result.in_battle = false;
    }
    return result;
}


void set_projection_tiles(const ProjectionTile* tiles, int count, int map_rows, int map_cols) {
    ProjectionRequest request;
    if (tiles && count > 0) {
        request.generation = g_request.generation + 1;
        const int limit = count > kMaxProjectionTiles ? kMaxProjectionTiles : count;
        request.count = limit;
        request.map_rows = map_rows > 0 ? map_rows : 0;
        request.map_cols = map_cols > 0 ? map_cols : 0;
        for (int index = 0; index < limit; ++index) {
            request.rows[index] = static_cast<std::int16_t>(tiles[index].row);
            request.cols[index] = static_cast<std::int16_t>(tiles[index].col);
        }
    }
    const std::uint32_t sequence = g_request_sequence.load(std::memory_order_relaxed);
    g_request_sequence.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    g_request = request;
    std::atomic_thread_fence(std::memory_order_release);
    g_request_sequence.store(sequence + 2, std::memory_order_relaxed);
}

bool projection_diag(ProjectionDiag* out) {
    ProjectionResult result;
    if (!read_projection(result) || out == nullptr) {
        return false;
    }
    out->unity_width = result.unity_width;
    out->unity_height = result.unity_height;
    out->tiles_total = result.tiles_total;
    out->used_tiles = result.used_tiles;
    out->count = result.count;
    out->anchor_x = result.anchor_x;
    out->anchor_y = result.anchor_y;
    out->anchor_z = result.anchor_z;
    out->tile_world_x = result.tile_world_x;
    out->tile_world_z = result.tile_world_z;
    return true;
}

bool unit_api_available() { return g_units_published.load(std::memory_order_acquire); }

void set_range_enabled(bool enabled) { g_range_enabled.store(enabled, std::memory_order_relaxed); }

void set_operator_stats_enabled(bool enabled) { g_operator_enabled.store(enabled, std::memory_order_relaxed); }

int operator_snapshot(OperatorSample* out, int max, std::uint64_t* sampled_ms, std::uint32_t* battle_serial) {
    if (sampled_ms) *sampled_ms = 0;
    if (battle_serial) *battle_serial = 0;
    if (!out || max <= 0 || !g_operator_enabled.load(std::memory_order_relaxed)) return 0;
    std::lock_guard lock(g_operator_mutex);
    if (sampled_ms) *sampled_ms = g_operators.updated_ms;
    if (battle_serial) *battle_serial = g_operators.battle_serial;
    const auto count = std::min(max, static_cast<int>(g_operators.count));
    std::copy_n(g_operators.items, count, out);
    return count;
}

void set_path_requests(bool enabled, std::uint32_t serial, const std::uint64_t* ids, int count) {
    PathRequest request;
    request.enabled = enabled; request.battle_serial = serial;
    if (enabled && serial && ids) for (int index = 0; index < std::min(count, static_cast<int>(::arkhelp::ipc::kMaxPaths)); ++index) {
        if (!ids[index]) continue;
        bool duplicate = false;
        for (std::uint32_t i = 0; i < request.count; ++i) duplicate |= request.ids[i] == ids[index];
        if (!duplicate) request.ids[request.count++] = ids[index];
    }
    std::lock_guard lock(g_path_request_mutex);
    g_path_request = request;
}

int path_snapshot(PathSample* out, int max) {
    if (!out || max <= 0) return 0;
    const auto request = read_path_request();
    if (!request.enabled || !request.count) return 0;
    std::lock_guard lock(g_path_mutex);
    int count = 0;
    for (std::uint32_t index = 0; index < g_paths.count && count < max; ++index) {
        const auto& path = g_paths.items[index];
        if (path.battle_serial != request.battle_serial) continue;
        for (std::uint32_t r = 0; r < request.count; ++r) if (path.tracking_id == request.ids[r]) {
            out[count++] = path; break;
        }
    }
    return count;
}

int unit_snapshot(UnitSample* out, int max, std::uint64_t* sample_ms, double* sample_battle_time,
                  std::uint32_t* sample_battle_serial) {
    if (sample_ms) *sample_ms = 0;
    if (sample_battle_time) *sample_battle_time = 0;
    if (sample_battle_serial) *sample_battle_serial = 0;
    if (out == nullptr || max <= 0) {
        return 0;
    }
    UnitPayload payload;
    if (!read_units(payload)) {
        return 0;
    }
    if (sample_ms) *sample_ms = payload.updated_ms;
    if (sample_battle_time) *sample_battle_time = payload.time_seconds;
    if (sample_battle_serial) *sample_battle_serial = payload.battle_serial;
    int used = 0;
    for (std::uint32_t index = 0; index < payload.count && used < max; ++index) {
        out[used++] = payload.items[index];
    }
    return used;
}

int enemy_snapshot(EnemySample* out, int max, std::uint64_t* sample_ms,
                   std::uint32_t* battle_serial) {
    if (!out || max <= 0) return 0;
    std::lock_guard lock(g_enemy_mutex);
    if (sample_ms) *sample_ms = g_enemies.updated_ms;
    if (battle_serial) *battle_serial = g_enemies.battle_serial;
    const int count = std::min(max, static_cast<int>(g_enemies.count));
    std::copy_n(g_enemies.items, count, out);
    return count;
}

bool runtime_stage(Stage& out, std::uint32_t* battle_serial, std::uint32_t known_revision,
                   std::uint32_t known_serial) {
    std::lock_guard lock(g_stage_mutex);
    if (g_runtime_stage.level_id.empty()) return false;
    if (known_revision && known_revision == g_runtime_stage.revision && known_serial == g_runtime_stage_serial) return false;
    out = g_runtime_stage;
    if (battle_serial) *battle_serial = g_runtime_stage_serial;
    return true;
}

bool projection_available() {
    return g_projection_published.load(std::memory_order_acquire);
}

bool projection_screen(int row, int col, float* x, float* y) {
    ProjectionResult result;
    if (!read_projection(result)) {
        return false;
    }
    for (std::int32_t index = 0; index < result.count; ++index) {
        const ProjectionItem& item = result.items[index];
        if (item.valid && item.row == row && item.col == col) {
            if (x) {
                *x = item.x;
            }
            if (y) {
                *y = item.y;
            }
            return true;
        }
    }
    return false;
}

bool projection_anchor(float* x, float* y, float* z) {
    if (!g_detour.has_origin) {
        return false;
    }
    if (x) {
        *x = g_detour.last_origin.x;
    }
    if (y) {
        *y = g_detour.last_origin.y;
    }
    if (z) {
        *z = g_detour.last_origin.z;
    }
    return true;
}

bool projection_unity_size(int* width, int* height) {
    ProjectionResult result;
    if (!read_projection(result) || result.unity_width <= 0 || result.unity_height <= 0) {
        return false;
    }
    if (width) {
        *width = result.unity_width;
    }
    if (height) {
        *height = result.unity_height;
    }
    return true;
}

bool can_unload() {
    // shutdown() is the one place that removes the hook and it is retried by the host on failure,
    // so an installed hook must not block the unload attempt itself.
    return true;
}

} // namespace ark::arkhelper::capture
