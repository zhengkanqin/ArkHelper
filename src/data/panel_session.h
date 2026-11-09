#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <arkhelp/ipc.h>

namespace arkhelp {

// Desktop panel presentation is separate from the provider's connection status. Never turn a
// broken connection into a successful battle end, and never display old IPC totals on first launch.
class PanelSession {
public:
    enum class Phase { kIdle, kLive, kRetained, kInterrupted };

    void observe(const ipc::BattleSnapshot* sample, std::uint64_t now) {
        const bool fresh = sample && sample->published_ms != 0 && sample->published_ms <= now &&
            now - sample->published_ms <= 3000 && !(sample->flags & ipc::kBattleStale);
        if (!fresh) {
            if (phase_ == Phase::kLive) { phase_ = Phase::kInterrupted; display_ = retained_; }
            return;
        }
        const bool live = sample->in_battle && sample->battle_state == 2;
        if (live) {
            const bool changed = !have_battle_ || sample->battle_serial != serial_ ||
                (phase_ != Phase::kLive && sample->frame < last_frame_) ||
                std::strncmp(sample->level_id, retained_.level_id, ipc::kLevelIdCapacity) != 0;
            if (changed) { retained_ = {}; dismissed_ = 0; ++generation_; }
            have_battle_ = true;
            serial_ = sample->battle_serial;
            last_frame_ = sample->frame;
            merge(*sample);
            display_ = *sample;
            display_.enemy_count = retained_.enemy_count;
            std::copy(std::begin(retained_.enemies), std::end(retained_.enemies), std::begin(display_.enemies));
            phase_ = Phase::kLive;
            return;
        }
        if (sample->in_battle && sample->battle_state != 3) {
            if (phase_ == Phase::kLive) { phase_ = Phase::kInterrupted; display_ = retained_; }
            return;
        }
        if (!have_battle_) return;
        // An idle publication may retain global totals from the old event log. It can confirm
        // leaving the battle, but must not replace the cached enemies or last spawn plan.
        if (sample->battle_serial == serial_) {
            if (sample->battle_state == 3) merge(*sample);
            else if (sample->damage_count) {
                // Event publication may trail scene teardown. Accept final totals without
                // replacing the retained level identity, positions or nonempty enemy roster.
                retained_.damage_count = sample->damage_count;
                std::copy(std::begin(sample->damage), std::end(sample->damage), std::begin(retained_.damage));
                std::copy(std::begin(sample->team_metrics), std::end(sample->team_metrics), std::begin(retained_.team_metrics));
                retained_.team_metric_mask = sample->team_metric_mask;
                retained_.stats_flags = sample->stats_flags;
                retained_.time_seconds = std::max(retained_.time_seconds, sample->time_seconds);
            }
            phase_ = Phase::kRetained;
            display_ = retained_;
        }
    }

    void dismiss(std::uint32_t panel_bit) { if (retained()) dismissed_ |= panel_bit; }
    bool visible(std::uint32_t panel_bit) const { return phase_ != Phase::kIdle && !(dismissed_ & panel_bit); }
    bool retained() const { return phase_ == Phase::kRetained || phase_ == Phase::kInterrupted; }
    Phase phase() const { return phase_; }
    std::uint64_t generation() const { return generation_; }
    const ipc::BattleSnapshot& snapshot() const { return display_; }

private:
    static void merge_enemy(ipc::EnemyEntry& current, const ipc::EnemyEntry& previous) {
        // Only reuse a value from this exact instance of this battle. Zero is a valid observation.
        if (!current.tracking_id || current.tracking_id != previous.tracking_id ||
            std::strncmp(current.enemy_key, previous.enemy_key, sizeof(current.enemy_key)) != 0 ||
            !(current.flags & ipc::kEnemyRuntime) || !(previous.flags & ipc::kEnemyRuntime)) return;
        const auto fill = [&](std::uint32_t bit, auto member) {
            if (!(current.stats_mask & bit) && (previous.stats_mask & bit)) {
                current.*member = previous.*member; current.stats_mask |= bit;
            }
        };
        fill(ipc::kEnemyHp, &ipc::EnemyEntry::hp); fill(ipc::kEnemyMaxHp, &ipc::EnemyEntry::max_hp);
        fill(ipc::kEnemyAtk, &ipc::EnemyEntry::atk); fill(ipc::kEnemyDef, &ipc::EnemyEntry::def);
        fill(ipc::kEnemyMagicResistance, &ipc::EnemyEntry::magic_resistance);
        fill(ipc::kEnemyMoveSpeed, &ipc::EnemyEntry::move_speed);
        fill(ipc::kEnemyAttackSpeed, &ipc::EnemyEntry::attack_speed);
        fill(ipc::kEnemyBaseAttackTime, &ipc::EnemyEntry::base_attack_time);
        fill(ipc::kEnemyWeight, &ipc::EnemyEntry::weight); fill(ipc::kEnemyBlockCount, &ipc::EnemyEntry::block_cnt);
        fill(ipc::kEnemyLifePointReduce, &ipc::EnemyEntry::life_point_reduce);
    }
    void merge(const ipc::BattleSnapshot& sample) {
        auto previous = retained_;
        retained_ = sample;
        retained_.time_seconds = std::max(previous.time_seconds, sample.time_seconds);
        retained_.frame = std::max(previous.frame, sample.frame);
        if (!sample.level_id[0]) std::memcpy(retained_.level_id, previous.level_id, sizeof(retained_.level_id));
        const auto runtime_roster = [](const ipc::BattleSnapshot& snapshot) {
            return std::any_of(snapshot.enemies, snapshot.enemies + std::min(snapshot.enemy_count, ipc::kMaxEnemies),
                [](const auto& enemy) { return (enemy.flags & ipc::kEnemyRuntime) != 0; });
        };
        if ((!sample.enemy_count || (!runtime_roster(sample) && runtime_roster(previous))) && previous.enemy_count) {
            retained_.enemy_count = previous.enemy_count;
            std::copy(std::begin(previous.enemies), std::end(previous.enemies), std::begin(retained_.enemies));
        } else {
            for (std::uint32_t i = 0; i < std::min(retained_.enemy_count, ipc::kMaxEnemies); ++i)
                for (std::uint32_t j = 0; j < std::min(previous.enemy_count, ipc::kMaxEnemies); ++j)
                    merge_enemy(retained_.enemies[i], previous.enemies[j]);
        }
        if (!sample.spawn_count && previous.spawn_count) {
            retained_.spawn_count = previous.spawn_count;
            std::copy(std::begin(previous.spawns), std::end(previous.spawns), std::begin(retained_.spawns));
        }
        if (!sample.damage_count && previous.damage_count) {
            retained_.damage_count = previous.damage_count;
            std::copy(std::begin(previous.damage), std::end(previous.damage), std::begin(retained_.damage));
            std::copy(std::begin(previous.team_metrics), std::end(previous.team_metrics), std::begin(retained_.team_metrics));
            retained_.team_metric_mask = previous.team_metric_mask;
            retained_.stats_flags = previous.stats_flags;
        }
    }

    Phase phase_ = Phase::kIdle;
    bool have_battle_ = false;
    std::uint32_t serial_ = 0;
    std::uint32_t last_frame_ = 0;
    std::uint32_t dismissed_ = 0;
    std::uint64_t generation_ = 0;
    ipc::BattleSnapshot retained_{};
    ipc::BattleSnapshot display_{};
};

// Content revisions request one fit only. A user's subsequent manual resize survives every poll.
struct PanelWidthRevision {
    bool changed(std::uint64_t next) {
        if (initialized && value == next) return false;
        initialized = true; value = next; return true;
    }
    bool initialized = false;
    std::uint64_t value = 0;
};

struct PanelAutoWidth {
    void restore(int width_px) { pending = width_px <= 0; }
    void request() { pending = true; }
    bool take(bool ready) {
        if (!ready || !pending) return false;
        pending = false;
        return true;
    }
    bool pending = true;
};

} // namespace arkhelp
