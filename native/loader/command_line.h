#pragma once

#include <windows.h>

#include <filesystem>

#include "runtime_loader.h"

namespace ark::loader {

inline constexpr char kUsage[] =
    "Usage: ArkHelperLauncher [(--pid PID | --watch | --self-test | --watch --self-test) "
    "[--method window-hook|remote-thread] [--timeout-seconds 1..600] [--report file]]; no "
    "arguments starts the interactive console; --watch selects a new Arknights.exe process by "
    "name; --diagnose requires --pid only.";

struct CommandLineOptions {
    DWORD pid = 0;
    DWORD timeout_seconds = 60;
    bool watch = false;
    bool self_test = false;
    // Reports the integrity levels of the loader and --pid without loading anything.
    bool diagnose = false;
    // Watch mode defaults to the remote thread; every other mode to the window hook.
    LoadMethod method = LoadMethod::WindowHook;
    std::filesystem::path report_path;
};

// Parses argv[1..]. Returns false for unknown, duplicated, malformed or conflicting
// options; `options.report_path` is still filled so the failure can be reported.
bool parse_command_line(int argc, wchar_t** argv, CommandLineOptions& options);

} // namespace ark::loader
