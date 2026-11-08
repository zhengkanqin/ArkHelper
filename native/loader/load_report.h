#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace ark::loader {

// Result of one loader run. Serialized as a single JSON object whose field names and
// stage strings are consumed by load-game.ps1 and external tooling.
struct LoadReport {
    DWORD pid = 0;
    DWORD error = 0;
    DWORD thread_exit = 0;
    DWORD phase = 0;
    DWORD loader_integrity = 0;
    DWORD target_integrity = 0;
    uintptr_t module = 0;
    unsigned long long creation_time = 0;
    unsigned long long watch_armed_time = 0;
    std::string stage = "not_started";
    std::string library_stage = "not_started";
    bool watch = false;
    bool success = false;
    bool retained_remote_memory = false;

    // Records a failed stage; returns *this so callers can `return report.fail(...)`.
    LoadReport& fail(const char* failed_stage, DWORD error_code) {
        stage = failed_stage;
        error = error_code;
        return *this;
    }
};

// Prints the report as one JSON line to stdout and, when `path` is not empty, writes the
// same line to that file.
void write_report(const LoadReport& report, const std::filesystem::path& path);

} // namespace ark::loader
