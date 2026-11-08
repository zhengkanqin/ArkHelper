#pragma once
#include <cmath>
#include <arkhelp/ipc.h>
#include "../capture/capture.h"

namespace ark::arkhelper::ipc_publish {
inline constexpr std::uint32_t kClockStatusFlags =
    ::arkhelp::ipc::kBattleLevelKnown | ::arkhelp::ipc::kBattleStale | ::arkhelp::ipc::kBattlePaused;

inline std::uint32_t clock_status_flags(const capture::Status& status) {
    std::uint32_t flags = 0;
    if (status.level_id[0]) flags |= ::arkhelp::ipc::kBattleLevelKnown;
    if (status.stale) flags |= ::arkhelp::ipc::kBattleStale;
    if (status.in_battle && !status.stale && status.pause_known && status.paused)
        flags |= ::arkhelp::ipc::kBattlePaused;
    return flags;
}

// Pause/resume and speed changes bypass the normal 100 ms publication interval. Neither unit
// scans nor the IPC layout/rate for an unchanged clock need to increase.
inline bool clock_status_changed(const capture::Status& status,
                                 const ::arkhelp::ipc::BattleSnapshot& last, float last_time_scale) {
    return (last.flags & kClockStatusFlags) != clock_status_flags(status) ||
           std::abs(status.time_scale-last_time_scale) > .001f;
}
} // namespace ark::arkhelper::ipc_publish
