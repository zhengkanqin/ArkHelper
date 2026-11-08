#pragma once

#include <filesystem>

namespace ark::loader {

// Double-click mode. Elevates once (the game client runs elevated), waits without a time
// limit for each new game process, loads it through the remote-thread entry and mirrors
// its log lines until it exits. A process whose load failed is never retried.
int run_interactive_session(const std::filesystem::path& root);

} // namespace ark::loader
