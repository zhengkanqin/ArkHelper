#include "../native/modules/provider/ipc/status_flags.h"
#include <cstdio>

namespace {
unsigned checks = 0, failures = 0;
void check(bool passed, const char* label) {
    ++checks;
    if (!passed) { ++failures; std::printf("FAIL %s\n", label); }
}
}
int main() {
    using ark::arkhelper::capture::Status;
    using namespace ark::arkhelper::ipc_publish;
    using namespace arkhelp::ipc;
    Status live;
    live.in_battle = true;
    live.pause_known = true;
    live.frame = 120;
    live.updated_ms = 1000;
    live.time_seconds = 4;
    live.time_scale = 1;
    BattleSnapshot last;
    last.flags = clock_status_flags(live);
    last.frame = live.frame;
    check(!clock_status_changed(live, last, 1), "unchanged clock keeps normal IPC throttle");

    live.paused = true;
    live.updated_ms += 16; // Same frame and battle time immediately after pressing pause.
    check(clock_status_changed(live, last, 1) && (clock_status_flags(live) & kBattlePaused),
          "pause publishes immediately while frame and battle time are unchanged");
    last.flags = clock_status_flags(live);
    check(!clock_status_changed(live, last, 1), "steady pause does not flood IPC");
    live.paused = false;
    live.updated_ms += 16; // Resume is known before the next fixed simulation step.
    check(clock_status_changed(live, last, 1) && !(clock_status_flags(live) & kBattlePaused),
          "resume clears pause immediately before the fixed frame advances");
    last.flags = clock_status_flags(live);
    live.updated_ms += 1000;
    check(!(clock_status_flags(live) & kBattlePaused), "a stopped frame alone never impersonates pause");

    live.time_scale = 2;
    check(clock_status_changed(live, last, 1), "double speed publishes a current clock immediately");
    check(!clock_status_changed(live, last, 2), "unchanged double speed keeps normal IPC throttle");
    live.time_scale = .5f;
    check(clock_status_changed(live, last, 2), "slow motion also bypasses IPC throttle");
    live.time_scale = -1;
    live.pause_known = false;
    live.paused = true;
    check(!(clock_status_flags(live) & kBattlePaused), "missing pause getter remains unavailable");
    live.pause_known = true;
    live.stale = true;
    check(!(clock_status_flags(live) & kBattlePaused), "stale pause cannot expose a paused-only overlay");
    live.stale = false;
    live.in_battle = false;
    check(!(clock_status_flags(live) & kBattlePaused), "leaving battle clears pause");
    std::printf("Battle clock: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
