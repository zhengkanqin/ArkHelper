#pragma once

#include <filesystem>

#include "load_report.h"
#include "target.h"

namespace ark::loader {

enum class LoadMethod {
    WindowHook,
    RemoteThread,
};

// Verifies the target, loads ArkHelperRuntime.dll with `method` unless a core instance already
// runs there, then waits for the runtime's Ready/Failed events. Fills in
// `target.creation_time` when it was not known yet.
LoadReport load_runtime(Target& target, const std::filesystem::path& root, LoadMethod method,
                          ULONGLONG deadline);

} // namespace ark::loader
