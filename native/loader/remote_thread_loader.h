#pragma once

#include <filesystem>

#include "load_report.h"
#include "target.h"

namespace ark::loader {

// Loads `core_path` into the target with LoadLibraryExW on a remote thread, then calls its
// `arkhelp_runtime_start` export from that same thread (outside the loader lock). The entry
// is invoked at most once; it is never retried.
LoadReport load_with_remote_thread(const Target& target, const std::filesystem::path& core_path,
                                   ULONGLONG deadline);

} // namespace ark::loader
