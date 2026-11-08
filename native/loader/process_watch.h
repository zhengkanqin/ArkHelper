#pragma once

#include <windows.h>

#include <filesystem>

#include "load_report.h"
#include "target.h"

namespace ark::loader {

struct WatchOptions {
    // Start the bundled test host after arming and accept only that child process.
    bool self_test = false;
    // Wait without a deadline (interactive session); `deadline` is ignored.
    bool unbounded = false;
    // Accept a game process that already exists (retry after a launcher hand-off, or when the
    // game was started before the watcher was armed).
    bool accept_existing = false;
    ULONGLONG deadline = 0;
};

// Starts ArkHelperRuntimeHost.exe in loader-await mode. On failure records
// "create_test_host" in `report`.
bool create_test_host(const std::filesystem::path& root, PROCESS_INFORMATION& child,
                      LoadReport& report);

// Waits for a process named `target.image_name` that did not exist when the watcher was
// armed and was created afterwards. On success fills `target.pid` / `target.creation_time`.
// `test_host` receives the self-test child, if one was started.
bool watch_for_new_process(Target& target, const std::filesystem::path& root,
                           const WatchOptions& options, PROCESS_INFORMATION& test_host,
                           LoadReport& report);

// Appends a diagnostic line to ARK_LOADER_DEBUG_FILE when that variable is set.
void debug_note(const wchar_t* format, ...);

} // namespace ark::loader
