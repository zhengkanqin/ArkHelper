#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <ark/plugin.h>

#include "actor_names.h"
#include "event_history.h"
#include "name_resolver.h"
#include "native_capture.h"
#include "stats.h"

namespace ark::damage_meter {

// Host log levels.
inline constexpr std::uint32_t kLogInfo = 1;
inline constexpr std::uint32_t kLogWarning = 2;
inline constexpr std::uint32_t kLogError = 3;

// Native-facing objects of one module session. Destroyed after capture has shut down.
struct Backend {
    explicit Backend(const ArkPluginApiV1& api) : host(api), capture(history, api) {}

    // Cached frame of the running battle; unavailable once shutdown has begun.
    bool current_frame(ArkBattleFrameV1& out) { return !stopping && history.frame(out); }

    const ArkPluginApiV1& host;
    EventHistory history;
    NativeCapture capture;
    std::atomic<bool> stopping{false};

    // Frame-callback thread only.
    bool connected = false;
    bool connect_failed = false;
    ULONGLONG next_connect_tick = 0;
};

// Statistics, name lookup scheduling and CSV export. All session state is
// guarded by mutex(); the name resolver itself is touched only on the game thread.
class Meter {
public:
    std::mutex& mutex() { return mutex_; }

    // Caller holds mutex().
    bool started() const { return host_ != nullptr; }

    // Resets all session state for a new plugin session. Caller holds mutex().
    void start(const ArkPluginApiV1& host);
    // Releases all session state. Requires SDK callbacks and game tasks to be drained.
    void stop();

    // SDK frame callback: connects native capture, consumes new events, schedules name lookups
    // and exports finished battles.
    void update(Backend& backend);

private:
    struct StageLabel {
        std::string label;
        // False while catalogs are still loading; the label is then looked up again.
        bool complete = false;
    };

    // Everything that is reset between plugin sessions. Guarded by mutex_.
    struct Session {
        BattleStats stats;
        std::string capture_error;

        ActorNameMap actor_names;
        std::vector<std::string> pending_actor_ids;
        bool name_lookup_pending = false;
        bool actor_lookup_failed = false;
        bool actor_names_dirty = true;
        ULONGLONG next_actor_lookup_tick = 0;
        std::string actor_lookup_error;

        std::map<std::string, StageLabel, std::less<>> stage_labels;
        std::string pending_stage_id;
        std::string stage_lookup_error;
        bool stage_label_dirty = true;
        ULONGLONG next_stage_lookup_tick = 0;

        bool order_dirty = true;
        std::vector<const ActorRow*> sorted_rows;
        std::uint64_t exported_battle = 0;
        std::string exported_path;
    };

    struct NameLookupResult;

    // update() steps.
    void connect_capture(Backend& backend);
    void consume_events(Backend& backend);
    void schedule_name_lookup();
    void export_finished_battle();

    // Game-thread name lookup.
    static void resolve_names_task(void* user);
    void resolve_names();
    NameLookupResult look_up_names(std::vector<std::string> actor_ids, const std::string& stage_id);
    void publish_names(NameLookupResult result, const std::string& stage_id);

    // Caller holds mutex_.
    const std::vector<const ActorRow*>& sorted_rows();
    bool export_csv();

    void log(std::uint32_t level, const std::string& message) const;

    std::mutex mutex_;
    const ArkPluginApiV1* host_ = nullptr;
    std::unique_ptr<GameNameResolver> resolver_;
    Session session_;
};

} // namespace ark::damage_meter
