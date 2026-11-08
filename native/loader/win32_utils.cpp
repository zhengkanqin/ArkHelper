#include "win32_utils.h"

#include <stdexcept>
#include <vector>

#include "scoped_handle.h"

namespace ark::loader {

namespace {

constexpr DWORD kMaxPathChars = 32768;

} // namespace

std::wstring module_path(HMODULE module) {
    wchar_t path[kMaxPathChars];
    const DWORD length = GetModuleFileNameW(module, path, kMaxPathChars);
    if (length == 0 || length >= kMaxPathChars) {
        throw std::runtime_error("GetModuleFileNameW");
    }
    return {path, length};
}

std::filesystem::path loader_directory() {
    return std::filesystem::path(module_path(nullptr)).parent_path() / "runtime";
}

ULONGLONG filetime_to_u64(const FILETIME& time) {
    return (static_cast<ULONGLONG>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

ULONGLONG process_creation_time(HANDLE process) {
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        return 0;
    }
    return filetime_to_u64(created);
}

DWORD remaining_ms(ULONGLONG deadline) {
    const ULONGLONG now = GetTickCount64();
    return now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
}

DWORD process_integrity(HANDLE process, DWORD* error) {
    if (error) {
        *error = 0;
    }
    const auto record_error = [error] {
        if (error) {
            *error = GetLastError();
        }
    };

    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw_token)) {
        record_error();
        return 0;
    }
    ScopedHandle token(raw_token);

    DWORD size = 0;
    GetTokenInformation(token.get(), TokenIntegrityLevel, nullptr, 0, &size);
    if (size == 0) {
        record_error();
        return 0;
    }
    std::vector<unsigned char> storage(size);
    if (!GetTokenInformation(token.get(), TokenIntegrityLevel, storage.data(), size, &size)) {
        record_error();
        return 0;
    }

    const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(storage.data());
    const DWORD last_sub_authority = *GetSidSubAuthorityCount(label->Label.Sid) - 1u;
    return *GetSidSubAuthority(label->Label.Sid, last_sub_authority);
}

std::wstring runtime_object_name(const wchar_t* kind, DWORD pid) {
    return std::wstring(L"Local\\ArkHelp.Runtime.") + kind + L"." + std::to_wstring(pid);
}

} // namespace ark::loader
