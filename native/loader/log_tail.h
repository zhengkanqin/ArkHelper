#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

namespace ark::loader {

// Mirrors one game's lines from the shared runtime log to the console. Only content
// appended after construction is shown; truncation and partial writes are tolerated.
class LogTail {
public:
    LogTail(std::filesystem::path path, DWORD pid);

    // Prints every complete new line tagged with this process's PID.
    void pump();

private:
    std::filesystem::path path_;
    std::string tag_;
    unsigned long long offset_ = 0;
    std::string partial_line_;
};

} // namespace ark::loader
