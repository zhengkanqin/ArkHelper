#include "elevation.h"

#include <shellapi.h>

#include <string>

#include "scoped_handle.h"
#include "win32_utils.h"

namespace ark::loader {

bool is_elevated() {
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
        return false;
    }
    ScopedHandle token(raw_token);
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    return GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size) &&
           elevation.TokenIsElevated;
}

bool relaunch_elevated(DWORD& error) {
    const std::wstring executable = module_path(nullptr);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.lpVerb = L"runas";
    info.lpFile = executable.c_str();
    info.lpParameters = L"--load-runtime";
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info)) {
        return true;
    }
    error = GetLastError();
    return false;
}

} // namespace ark::loader
