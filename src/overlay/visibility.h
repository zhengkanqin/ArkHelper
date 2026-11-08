#pragma once

#include <cstdint>
#include "arkhelp/ipc.h"

namespace arkhelp::overlay {

// Applies to every mark drawn over the game. Retained report panels have a separate lifecycle.
inline bool live_snapshot(const ipc::BattleSnapshot& snapshot, std::uint64_t now) {
    return snapshot.in_battle != 0 && snapshot.battle_state == 2 &&
        (snapshot.flags & ipc::kBattleStale) == 0 && snapshot.published_ms != 0 &&
        snapshot.published_ms <= now && now - snapshot.published_ms <= 2000;
}

inline bool can_draw_live(const ipc::BattleSnapshot& snapshot, bool have_snapshot,
                          bool visible, bool minimized, bool foreground,
                          int width, int height, std::uint64_t now) {
    return have_snapshot && visible && !minimized && foreground && width >= 64 && height >= 64 &&
           live_snapshot(snapshot, now);
}

} // namespace arkhelp::overlay
