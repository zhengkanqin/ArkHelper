#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "../../damage_meter/stats.h"

namespace ark::arkhelper::ipc_publish {

// Bounded event consumption shared by the IPC publisher and its regression test. A history
// overrun marks the report incomplete, but retained events still have to be consumed in order.
template <class Read>
std::size_t read_statistics_events(Read&& read, damage_meter::BattleStats& statistics,
                                   std::uint64_t& cursor) {
    constexpr std::size_t kBatchSize = 256;
    constexpr unsigned kBatchesPerTick = 4;
    ArkEventV1 events[kBatchSize];
    std::size_t consumed = 0;
    for (unsigned batch = 0; batch < kBatchesPerTick; ++batch) {
        ArkReadStatusV1 status{};
        const auto count = std::min(read(cursor, events, kBatchSize, &status), kBatchSize);
        for (std::size_t index = 0; index < count; ++index) statistics.consume(events[index]);
        if (status.partial || status.overrun) statistics.partial = true;
        if (count == 0) break;
        consumed += count;
        // Advance only through events actually read. newest_sequence may be several batches
        // ahead, and jumping there on an overrun would discard all those retained events.
        const auto last = events[count - 1].sequence;
        if (last <= cursor) break;
        cursor = last;
        if (count < kBatchSize) break;
    }
    return consumed;
}

} // namespace ark::arkhelper::ipc_publish
