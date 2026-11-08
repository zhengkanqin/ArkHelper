#include "log_tail.h"

#include <cstddef>
#include <utility>

#include "console.h"
#include "scoped_handle.h"

namespace ark::loader {

LogTail::LogTail(std::filesystem::path path, DWORD pid)
    : path_(std::move(path)),
      tag_("[PID " + std::to_string(pid) + "]") {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &attributes)) {
        offset_ = (static_cast<unsigned long long>(attributes.nFileSizeHigh) << 32) |
                  attributes.nFileSizeLow;
    }
}

void LogTail::pump() {
    ScopedHandle file(CreateFileW(path_.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        return;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) {
        return;
    }
    // A shorter file means the log was recreated; restart from its beginning.
    if (static_cast<unsigned long long>(size.QuadPart) < offset_) {
        offset_ = 0;
        partial_line_.clear();
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset_);
    if (!SetFilePointerEx(file.get(), position, nullptr, FILE_BEGIN)) {
        return;
    }

    char buffer[65536];
    DWORD bytes_read = 0;
    while (ReadFile(file.get(), buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read) {
        offset_ += bytes_read;
        partial_line_.append(buffer, bytes_read);
    }

    size_t line_start = 0;
    for (size_t line_end; (line_end = partial_line_.find('\n', line_start)) != std::string::npos;
         line_start = line_end + 1) {
        std::string line = partial_line_.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.find(tag_) != std::string::npos) {
            write_console(utf8_to_wide(line) + L"\r\n");
        }
    }
    partial_line_.erase(0, line_start);
}

} // namespace ark::loader
