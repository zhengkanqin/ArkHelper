#include "load_report.h"

#include <cstdio>
#include <fstream>

namespace ark::loader {

namespace {

const char* json_bool(bool value) {
    return value ? "true" : "false";
}

} // namespace

void write_report(const LoadReport& report, const std::filesystem::path& path) {
    char line[1024];
    std::snprintf(
        line, sizeof(line),
        "{\"pid\":%lu,\"creation_time\":%llu,\"watch\":%s,\"watch_armed_time\":%llu,"
        "\"stage\":\"%s\",\"library_stage\":\"%s\",\"success\":%s,\"error\":%lu,"
        "\"thread_exit\":%lu,\"phase\":%lu,\"module\":\"0x%llx\","
        "\"retained_remote_memory\":%s,\"loader_integrity\":%lu,"
        "\"target_integrity\":%lu}",
        report.pid, report.creation_time, json_bool(report.watch), report.watch_armed_time,
        report.stage.c_str(), report.library_stage.c_str(), json_bool(report.success), report.error,
        report.thread_exit, report.phase, static_cast<unsigned long long>(report.module),
        json_bool(report.retained_remote_memory), report.loader_integrity, report.target_integrity);
    std::puts(line);
    if (!path.empty()) {
        std::ofstream file(path);
        file << line << '\n';
    }
}

} // namespace ark::loader
