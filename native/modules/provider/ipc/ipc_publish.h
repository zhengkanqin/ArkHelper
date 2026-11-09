// Runtime-worker publisher for IPC v9 and StageOverview v1.
// Copies current-stage/capture data and battle-event totals into bounded shared-memory
// snapshots. Unity objects are sampled by the main-thread capture hook.
#pragma once

namespace ark::arkhelper {

class App;

namespace ipc_publish {

// Stores the App pointer and the default command state; never fails.
void initialize(App* app);

// Runtime-worker tick: refreshes the command block, follows the battle level id, accumulates damage
// events and publishes the snapshot (throttled). Must be called after capture::tick().
void tick();

// Stops publishing and closes both mappings. Idempotent; never fails.
void shutdown();

// Whether the battle mapping is currently open (provider side alive).
bool active();

}  // namespace ipc_publish
}  // namespace ark::arkhelper
