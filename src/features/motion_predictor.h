// Bounded dead reckoning for the external overlay. No game calls, allocations or locks.
// HP remains sampled truth; only the position of its label is predicted.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "arkhelp/ipc.h"

namespace arkhelp::features {

class UnitMotionPredictor {
public:
    void reset() { *this = UnitMotionPredictor{}; }
    bool projection_changed() const { return projection_changed_; }

    void apply(ipc::BattleSnapshot& snapshot, std::uint64_t now, int width, int height) {
        projection_changed_=false;
        const std::uint32_t count = std::min(snapshot.unit_count, ipc::kMaxUnits);
        if (!snapshot.in_battle || (snapshot.flags & ipc::kBattleStale) != 0 ||
            !snapshot.unit_sample_ms || snapshot.unit_sample_ms > now || width <= 0 || height <= 0 ||
            !std::isfinite(snapshot.unit_sample_battle_time) || !std::isfinite(snapshot.time_seconds)) {
            snapshot.unit_count = 0;
            reset();
            return;
        }
        if (now - snapshot.unit_sample_ms > 1000) {
            snapshot.unit_count = 0;
            reset();
            return;
        }
        if (serial_ != snapshot.battle_serial || width_ != width || height_ != height ||
            snapshot.unit_sample_ms < sampled_ms_ || snapshot.time_seconds + 0.001 < clock_time_) {
            reset();
            serial_ = snapshot.battle_serial;
            width_ = width;
            height_ = height;
        }

        const bool stopped = (snapshot.flags & ipc::kBattlePaused) != 0;
        const bool resumed = paused_ && !stopped;
        if (resumed) {
            // The old velocity is still useful, but pause wall time and the already sampled
            // pre-pause displacement must not become an immediate jump on resume.
            resume_wall_ms_ = now;
            resume_sample_ms_ = snapshot.unit_sample_ms;
            resume_time_ = snapshot.time_seconds;
        }
        if (stopped != paused_) {
            for (auto& track : tracks_) {
                track.correction_x = track.correction_y = 0;
                track.correction_ms = 0;
            }
        }
        update_clock(snapshot, stopped);
        paused_ = stopped;
        const float jump_limit = std::max(48.0f, std::min(width, height) * 0.14f);
        if (snapshot.unit_sample_ms != sampled_ms_) {
            // A shared residual across several previously tracked enemies is usually a camera
            // pan/cut. Compare residuals, not raw motion, so a marching group still predicts.
            bool camera_changed = anchors_moved(snapshot);
            if (!camera_changed) {
                float rx = 0, ry = 0;
                int eligible = 0, common = 0;
                for (std::uint32_t i = 0; i < count; ++i) {
                    const auto& unit = snapshot.units[i];
                    const Track* old = find(unit.tracking_id);
                    if (!old || !old->moving || !valid(unit)) { continue; }
                    const float dt = static_cast<float>(snapshot.unit_sample_battle_time - old->battle_time);
                    if (dt < 0 || dt > 4) { continue; }
                    if (old->ground != has_ground(unit)) { continue; }
                    const Point point = position(unit);
                    const float dx = point.x - old->x - old->vx * dt;
                    const float dy = point.y - old->y - old->vy * dt;
                    if (!eligible) { rx = dx; ry = dy; }
                    ++eligible;
                    if (length(dx - rx, dy - ry) < 3.0f) { ++common; }
                }
                camera_changed = eligible >= 3 && common * 5 >= eligible * 4 &&
                                 length(rx, ry) > 12.0f;
            }
            projection_changed_=camera_changed;
            std::array<Track, ipc::kMaxUnits> next{};
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto& unit = snapshot.units[i];
                auto& track = next[i];
                const Point point = position(unit);
                track.id = unit.tracking_id;
                track.x = point.x;
                track.y = point.y;
                track.ground = has_ground(unit);
                track.sampled_ms = snapshot.unit_sample_ms;
                track.battle_time = snapshot.unit_sample_battle_time;
                if (!valid(unit) || !unit.tracking_id || camera_changed) { continue; }
                const Track* old = find(unit.tracking_id);
                if (!old || track.ground != old->ground || track.sampled_ms <= old->sampled_ms) { continue; }
                if (stopped || resumed) {
                    // Paused samples keep true positions current without erasing the last moving
                    // velocity. A resume edge can therefore animate before the next 4 Hz scan.
                    if (length(track.x - old->x, track.y - old->y) <= jump_limit) {
                        track.interval_ms = old->interval_ms;
                        track.vx = old->vx;
                        track.vy = old->vy;
                        track.moving = old->moving;
                    }
                    continue;
                }
                const auto interval = track.sampled_ms - old->sampled_ms;
                const float dt = static_cast<float>(track.battle_time - old->battle_time);
                const float dx = track.x - old->x;
                const float dy = track.y - old->y;
                // Reused identities, teleports, long stalls and time discontinuities snap to truth.
                if (interval > 750 || dt < 0.015f || dt > 4.0f ||
                    length(dx, dy) > jump_limit || length(dx, dy) / dt > 1200.0f) { continue; }
                track.interval_ms = interval;
                track.vx = dx / dt;
                track.vy = dy / dt;
                track.moving = length(dx, dy) > 0.15f;
                if (old->moving && track.moving) {
                    const Point previous = evaluate(*old, snapshot, now);
                    const Point current = evaluate(track, snapshot, now);
                    const float cx = previous.x - current.x;
                    const float cy = previous.y - current.y;
                    // Blend only small corrections; a large correction must never drag a label
                    // across the map or preserve a predicted position after an enemy stops.
                    if (length(cx, cy) <= 12.0f) {
                        track.correction_x = cx;
                        track.correction_y = cy;
                        track.correction_ms = now;
                    }
                }
            }
            tracks_ = next;
            sampled_ms_ = snapshot.unit_sample_ms;
            count_ = count;
        }

        for (std::uint32_t i = 0; i < count; ++i) {
            auto& unit = snapshot.units[i];
            if (!valid(unit)) {
                unit.flags &= ~(ipc::kUnitValid | ipc::kUnitRangeValid);
                continue;
            }
            if (stopped || i >= count_ || tracks_[i].id != unit.tracking_id) { continue; }
            const Point predicted = evaluate(tracks_[i], snapshot, now);
            const Point current = position(unit);
            const float dx = predicted.x - current.x;
            const float dy = predicted.y - current.y;
            unit.x += dx;
            unit.y += dy;
            if (has_ground(unit)) {
                unit.ground_x += dx;
                unit.ground_y += dy;
            }
            if ((unit.flags & ipc::kUnitRangeValid) != 0 && unit.range_point_count == ipc::kRangePointCount) {
                for (auto& point : unit.range_points) { point.x += dx; point.y += dy; }
            }
        }
    }

private:
    struct Point { float x = 0, y = 0; };
    struct Track {
        std::uint64_t id = 0, sampled_ms = 0, interval_ms = 250, correction_ms = 0;
        double battle_time = 0;
        float x = 0, y = 0, vx = 0, vy = 0, correction_x = 0, correction_y = 0;
        bool moving = false, ground = false;
    };
    struct Anchor { std::uint32_t route = 0; ipc::Vec2 screen{}; };

    static float length(float x, float y) { return std::sqrt(x * x + y * y); }
    static bool valid(const ipc::UnitEntry& unit) {
        return (unit.flags & ipc::kUnitValid) && std::isfinite(unit.x) && std::isfinite(unit.y) &&
            (!(unit.flags & ipc::kUnitGroundValid) || has_ground(unit));
    }
    static bool has_ground(const ipc::UnitEntry& unit) {
        return (unit.flags & ipc::kUnitGroundValid) &&
            std::isfinite(unit.ground_x) && std::isfinite(unit.ground_y);
    }
    static Point position(const ipc::UnitEntry& unit) {
        return has_ground(unit) ? Point{unit.ground_x, unit.ground_y} : Point{unit.x, unit.y};
    }
    const Track* find(std::uint64_t id) const {
        if (!id) { return nullptr; }
        for (std::uint32_t i = 0; i < count_; ++i) {
            if (tracks_[i].id == id) { return &tracks_[i]; }
        }
        return nullptr;
    }
    void update_clock(const ipc::BattleSnapshot& snapshot, bool stopped) {
        if (stopped || paused_ || !rate_ms_) {
            // Keep the previous moving rate across pause/resume; wall time spent paused is
            // excluded from the next rate estimate.
            rate_ms_ = snapshot.published_ms;
            rate_time_ = snapshot.time_seconds;
        } else if (snapshot.published_ms > rate_ms_ && snapshot.time_seconds > rate_time_) {
            const auto dt = snapshot.published_ms - rate_ms_;
            const double advanced = snapshot.time_seconds - rate_time_;
            if (dt >= 30) {
                game_rate_ = static_cast<float>(std::clamp(advanced * 1000.0 / dt, 0.0, 4.0));
                rate_ms_ = snapshot.published_ms;
                rate_time_ = snapshot.time_seconds;
            }
        }
        // kBattlePaused is authoritative. Repeated logic times can be quantized status
        // publications and must neither invent a pause nor shorten the rate measurement interval.
        clock_time_ = snapshot.time_seconds;
    }
    Point evaluate(const Track& track, const ipc::BattleSnapshot& snapshot, std::uint64_t now) const {
        Point point{track.x, track.y};
        if (!track.moving || now < track.sampled_ms) { return point; }
        // At 4 Hz a 300 ms horizon covers one sample interval plus normal IPC jitter. Missing
        // samples freeze at that bound; there is no indefinite drift during loading or a pause.
        const auto horizon = std::min<std::uint64_t>(300, track.interval_ms + track.interval_ms / 5);
        const bool from_before_resume = resume_wall_ms_ && track.sampled_ms <= resume_sample_ms_;
        const auto origin_ms = from_before_resume ? resume_wall_ms_ : track.sampled_ms;
        const auto age = std::min(now >= origin_ms ? now - origin_ms : 0, horizon);
        const auto publish_age = now >= snapshot.published_ms ? now - snapshot.published_ms : 0;
        const double origin_time = from_before_resume ? resume_time_ : track.battle_time;
        const double clock_age = snapshot.time_seconds - origin_time +
                                 game_rate_ * std::min<std::uint64_t>(publish_age, 150) / 1000.0;
        const float dt = static_cast<float>(std::clamp(clock_age, 0.0, game_rate_ * age / 1000.0));
        point.x += track.vx * dt;
        point.y += track.vy * dt;
        if (track.correction_ms && now >= track.correction_ms) {
            const float amount = 1.0f - std::min(1.0f, (now - track.correction_ms) / 80.0f);
            point.x += track.correction_x * amount;
            point.y += track.correction_y * amount;
        }
        return point;
    }
    bool anchors_moved(const ipc::BattleSnapshot& snapshot) {
        bool moved = false;
        std::array<Anchor, ipc::kMaxSpawns> next{};
        std::uint32_t used = 0;
        for (std::uint32_t i = 0; i < std::min(snapshot.spawn_count, ipc::kMaxSpawns); ++i) {
            const auto& spawn = snapshot.spawns[i];
            if (!(spawn.flags & ipc::kSpawnScreenValid)) { continue; }
            for (std::uint32_t j = 0; j < anchor_count_; ++j) {
                if (anchors_[j].route == spawn.route_index &&
                    length(static_cast<float>(anchors_[j].screen.x - spawn.screen.x),
                           static_cast<float>(anchors_[j].screen.y - spawn.screen.y)) > 4.0f) {
                    moved = true;
                }
            }
            next[used++] = Anchor{spawn.route_index, spawn.screen};
        }
        anchors_ = next;
        anchor_count_ = used;
        return moved;
    }

    std::array<Track, ipc::kMaxUnits> tracks_{};
    std::array<Anchor, ipc::kMaxSpawns> anchors_{};
    std::uint32_t count_ = 0, anchor_count_ = 0, serial_ = 0;
    std::uint64_t sampled_ms_ = 0, rate_ms_ = 0;
    std::uint64_t resume_wall_ms_ = 0, resume_sample_ms_ = 0;
    double clock_time_ = 0, rate_time_ = 0, resume_time_ = 0;
    float game_rate_ = 1;
    int width_ = 0, height_ = 0;
    bool paused_ = false,projection_changed_=false;
};

} // namespace arkhelp::features
