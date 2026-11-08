#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

#include <ark/battle.h>

namespace ark::damage_meter {

// Bounded, thread-safe log of the current battle's events. Producers publish from the game
// thread; consumers read from any thread by sequence number.
class EventHistory {
public:
    static constexpr std::size_t kDefaultCapacity = 65536;

    explicit EventHistory(std::size_t capacity = kDefaultCapacity) : capacity_(capacity) {}

    // Assigns the sequence and battle numbers. A battle start discards the previous battle.
    void publish(ArkEventV1 event) {
        std::lock_guard lock(mutex_);
        if (event.kind == ARK_BATTLE_START) {
            events_.clear();
            ++battle_;
            partial_ = event.partial != 0;
            ended_ = false;
            frame_valid_ = false;
        }
        if (event.kind == ARK_BATTLE_END) {
            ended_ = true;
        }
        event.sequence = ++sequence_;
        event.battle = battle_;
        events_.push_back(event);
        while (events_.size() > capacity_) {
            events_.pop_front();
            partial_ = true;
        }
    }

    // Copies up to `capacity` events newer than `after`. `status->overrun` reports that events
    // between `after` and the oldest retained event were dropped.
    std::size_t read(std::uint64_t after, ArkEventV1* out, std::size_t capacity,
                     ArkReadStatusV1* status) {
        std::lock_guard lock(mutex_);
        if (status) {
            *status = {};
            status->partial = partial_;
            status->ended = ended_;
            if (!events_.empty()) {
                const ArkEventV1& oldest = events_.front();
                status->oldest_sequence = oldest.sequence;
                status->newest_sequence = events_.back().sequence;
                status->overrun =
                    after && after + 1 < oldest.sequence && oldest.kind != ARK_BATTLE_START;
            }
        }
        std::size_t written = 0;
        for (const ArkEventV1& event : events_) {
            if (event.sequence > after && written < capacity) {
                out[written++] = event;
            }
        }
        return written;
    }

    // Caches the native logic frame of the running battle; ignored outside a battle.
    void set_frame(std::uint32_t frame) {
        std::lock_guard lock(mutex_);
        if (!battle_ || ended_) {
            return;
        }
        frame_ = frame;
        frame_valid_ = true;
    }

    bool frame(ArkBattleFrameV1& out) {
        std::lock_guard lock(mutex_);
        if (!frame_valid_) {
            return false;
        }
        out = {};
        out.battle = battle_;
        out.frame = frame_;
        return true;
    }

private:
    std::mutex mutex_;
    std::deque<ArkEventV1> events_;
    std::size_t capacity_;
    std::uint64_t sequence_ = 0;
    std::uint64_t battle_ = 0;
    bool partial_ = false;
    bool ended_ = false;
    std::uint32_t frame_ = 0;
    bool frame_valid_ = false;
};

} // namespace ark::damage_meter
