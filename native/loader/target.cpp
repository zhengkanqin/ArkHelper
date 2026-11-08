#include "target.h"

#include <tlhelp32.h>
#include <wchar.h>

#include "scoped_handle.h"
#include "win32_utils.h"

namespace ark::loader {

bool verify_target_identity(HANDLE process, const Target& target) {
    wchar_t path[32768];
    DWORD size = 32768;
    if (!QueryFullProcessImageNameW(process, 0, path, &size)) {
        return false;
    }
    const wchar_t* separator = wcsrchr(path, L'\\');
    const wchar_t* image_name = separator ? separator + 1 : path;
    if (_wcsicmp(image_name, target.image_name) != 0) {
        SetLastError(ERROR_INVALID_NAME);
        return false;
    }
    if (target.creation_time && process_creation_time(process) != target.creation_time) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    return true;
}

bool is_game_running() {
    ScopedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.valid()) {
        return false;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot.get(), &entry); ok;
         ok = Process32NextW(snapshot.get(), &entry)) {
        if (_wcsicmp(entry.szExeFile, kGameImageName) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace ark::loader
