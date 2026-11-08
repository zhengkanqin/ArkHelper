#include "meter.h"

#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#include "csv_export.h"

namespace ark::damage_meter {
namespace {

constexpr ULONGLONG kConnectRetryIntervalMs = 1000;
constexpr ULONGLONG kNameRetryIntervalMs = 1000;

// Per frame, at most kReadBatches * kReadBatchSize events are consumed.
constexpr std::size_t kReadBatchSize = 256;
constexpr unsigned kReadBatches = 16;

bool affects_order(std::uint32_t kind) {
    return kind == ARK_BATTLE_START || kind == ARK_DAMAGE || kind == ARK_ELEMENTAL ||
           kind == ARK_HEAL || kind == ARK_COST || kind == ARK_TAKEN;
}

} // namespace

struct Meter::NameLookupResult {
    std::vector<std::pair<std::string, std::string>> actor_names;
    // The actor databases are still loading; retry later.
    bool actors_deferred = false;
    std::string actor_error;

    std::string stage_label;
    bool stage_complete = false;
    std::string stage_error;
};

void Meter::start(const ArkPluginApiV1& host) {
    host_ = &host;
    session_ = Session{};
    resolver_.reset();
}

void Meter::stop() {
    // Game tasks have drained, so the resolver can be released without the lock.
    resolver_.reset();
    std::lock_guard lock(mutex_);
    session_ = Session{};
    host_ = nullptr;
}

void Meter::log(std::uint32_t level, const std::string& message) const {
    host_->log(host_->owner, level, message.c_str());
}

void Meter::update(Backend& backend) {
    if (backend.stopping) {
        return;
    }
    // The test host has no game thread; run queued game tasks from the frame instead.
    if (host_->test_host) {
        host_->pump_game_tasks(host_->owner);
    } else if (!backend.connected) {
        connect_capture(backend);
    }

    std::lock_guard lock(mutex_);
    consume_events(backend);
    if (!host_->test_host && !session_.name_lookup_pending) {
        schedule_name_lookup();
    }
    export_finished_battle();
}

void Meter::connect_capture(Backend& backend) {
    if (backend.connect_failed || GetTickCount64() < backend.next_connect_tick) {
        return;
    }
    backend.next_connect_tick = GetTickCount64() + kConnectRetryIntervalMs;
    try {
        backend.connected = backend.capture.initialize();
        std::lock_guard lock(mutex_);
        session_.capture_error.clear();
    } catch (const std::exception& error) {
        backend.connect_failed = true;
        std::lock_guard lock(mutex_);
        session_.capture_error = error.what();
        log(kLogError, "Native capture initialization failed: " + session_.capture_error);
    }
}

void Meter::consume_events(Backend& backend) {
    BattleStats& stats = session_.stats;
    ArkEventV1 events[kReadBatchSize];
    ArkReadStatusV1 status;
    for (unsigned batch = 0; batch < kReadBatches; ++batch) {
        const std::size_t count =
            backend.history.read(stats.sequence, events, kReadBatchSize, &status);
        if (status.overrun) {
            stats.partial = true;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const ArkEventV1& event = events[i];
            const std::size_t rows_before = stats.rows.size();
            stats.consume(event);
            if (affects_order(event.kind)) {
                session_.order_dirty = true;
            }
            if (event.kind == ARK_BATTLE_START) {
                const auto label = session_.stage_labels.find(stats.stage);
                session_.stage_label_dirty =
                    label == session_.stage_labels.end() || !label->second.complete;
                session_.next_stage_lookup_tick = 0;
            }
            if (stats.rows.size() != rows_before) {
                session_.actor_names_dirty = true;
            }
        }
        if (count < kReadBatchSize) {
            break;
        }
    }
}

void Meter::schedule_name_lookup() {
    const ULONGLONG now = GetTickCount64();
    session_.pending_actor_ids.clear();
    session_.pending_stage_id.clear();
    if (session_.actor_names_dirty && !session_.actor_lookup_failed &&
        now >= session_.next_actor_lookup_tick) {
        session_.actor_names_dirty = false;
        for (const auto& [id, row] : session_.stats.rows) {
            if (!session_.actor_names.contains(id)) {
                session_.pending_actor_ids.push_back(id);
            }
        }
    }
    if (session_.stage_label_dirty && !session_.stats.stage.empty() &&
        now >= session_.next_stage_lookup_tick) {
        session_.pending_stage_id = session_.stats.stage;
        session_.stage_label_dirty = false;
    }
    if (session_.pending_actor_ids.empty() && session_.pending_stage_id.empty()) {
        return;
    }
    session_.name_lookup_pending = host_->queue_game_task(host_->owner, resolve_names_task, this);
    if (!session_.name_lookup_pending) {
        if (!session_.pending_actor_ids.empty()) {
            session_.actor_names_dirty = true;
        }
        if (!session_.pending_stage_id.empty()) {
            session_.stage_label_dirty = true;
        }
    }
}

void Meter::export_finished_battle() {
    const BattleStats& stats = session_.stats;
    if (!stats.ended || stats.battle == session_.exported_battle) {
        return;
    }
    // Wait for actor names so the automatic export does not fall back to raw IDs.
    const bool names_settled = host_->test_host || session_.actor_lookup_failed ||
                               (!session_.name_lookup_pending && !session_.actor_names_dirty);
    if (names_settled && export_csv()) {
        session_.exported_battle = stats.battle;
    }
}

const std::vector<const ActorRow*>& Meter::sorted_rows() {
    if (session_.order_dirty) {
        session_.stats.sort_rows(session_.sorted_rows, true);
        session_.order_dirty = false;
    }
    return session_.sorted_rows;
}

bool Meter::export_csv() {
    try {
        std::optional<std::string> path = write_csv_report(host_->root_directory, session_.stats,
                                                           sorted_rows(), session_.actor_names);
        if (!path) {
            return false;
        }
        session_.exported_path = std::move(*path);
        log(kLogInfo, "CSV exported: " + session_.exported_path);
        return true;
    } catch (...) {
        log(kLogError, "CSV export failed");
        return false;
    }
}

void Meter::resolve_names_task(void* user) {
    static_cast<Meter*>(user)->resolve_names();
}

void Meter::resolve_names() {
    std::vector<std::string> actor_ids;
    std::string stage_id;
    {
        std::lock_guard lock(mutex_);
        actor_ids.swap(session_.pending_actor_ids);
        stage_id.swap(session_.pending_stage_id);
    }
    // Managed lookups run outside the statistics lock.
    NameLookupResult result = look_up_names(std::move(actor_ids), stage_id);
    publish_names(std::move(result), stage_id);
}

Meter::NameLookupResult Meter::look_up_names(std::vector<std::string> actor_ids,
                                             const std::string& stage_id) {
    NameLookupResult result;
    try {
        if (!resolver_) {
            resolver_ = std::make_unique<GameNameResolver>();
        }
    } catch (const std::exception& error) {
        if (!actor_ids.empty()) {
            result.actor_error = error.what();
        }
        if (!stage_id.empty()) {
            result.stage_error = error.what();
        }
    }
    if (!resolver_) {
        return result;
    }

    if (!actor_ids.empty()) {
        try {
            result.actors_deferred = !resolver_->actor_databases_ready();
            if (!result.actors_deferred) {
                result.actor_names.reserve(actor_ids.size());
                for (std::string& id : actor_ids) {
                    std::string name = resolver_->actor_name(id);
                    result.actor_names.emplace_back(std::move(id), std::move(name));
                }
            }
        } catch (const std::exception& error) {
            result.actor_error = error.what();
        }
    }

    if (!stage_id.empty()) {
        const bool catalogs_loaded = resolver_->load_stage_catalogs(result.stage_error);
        result.stage_label = resolver_->stage_label(stage_id);
        // A missing label is final only once every catalog has loaded.
        result.stage_complete = !result.stage_label.empty() || catalogs_loaded;
    }
    return result;
}

void Meter::publish_names(NameLookupResult result, const std::string& stage_id) {
    std::lock_guard lock(mutex_);
    for (auto& [id, name] : result.actor_names) {
        session_.actor_names.emplace(std::move(id), std::move(name));
    }
    if (result.actors_deferred) {
        session_.actor_names_dirty = true;
        session_.next_actor_lookup_tick = GetTickCount64() + kNameRetryIntervalMs;
    }
    if (!result.actor_error.empty()) {
        session_.actor_lookup_failed = true;
        session_.actor_lookup_error = std::move(result.actor_error);
        log(kLogError, "Native actor-name lookup failed: " + session_.actor_lookup_error);
    }
    if (!stage_id.empty()) {
        // Publish under the captured raw ID, never into the current battle's label: a queued
        // task may finish after another battle has started.
        session_.stage_labels.insert_or_assign(
            stage_id, StageLabel{std::move(result.stage_label), result.stage_complete});
        if (session_.stats.stage == stage_id && !result.stage_complete) {
            session_.stage_label_dirty = true;
        }
        if (!result.stage_complete) {
            session_.next_stage_lookup_tick = GetTickCount64() + kNameRetryIntervalMs;
        }
        if (result.stage_error != session_.stage_lookup_error) {
            session_.stage_lookup_error = std::move(result.stage_error);
            if (!session_.stage_lookup_error.empty()) {
                log(kLogWarning,
                    "Native stage-name lookup deferred: " + session_.stage_lookup_error);
            }
        }
    }
    session_.name_lookup_pending = false;
}

} // namespace ark::damage_meter
