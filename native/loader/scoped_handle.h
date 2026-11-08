#pragma once

#include <windows.h>

namespace ark::loader {

// Owns a kernel handle. Both null and INVALID_HANDLE_VALUE mean "no handle", so the
// same type serves OpenProcess-style and CreateFile/Toolhelp-style APIs.
class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}

    ~ScopedHandle() {
        if (valid()) {
            CloseHandle(handle_);
        }
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE get() const noexcept { return handle_; }

    bool valid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE handle_;
};

} // namespace ark::loader
