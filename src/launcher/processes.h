#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cwchar>
#include <vector>

extern "C" __declspec(dllimport) BOOL WINAPI K32EnumProcesses(DWORD*, DWORD, LPDWORD);

namespace arkhelp::launcher {
// Use the same complete process enumeration as the runtime watcher.
inline std::vector<DWORD> process_ids() {
    std::vector<DWORD> ids(512);
    for (;;) {
        DWORD bytes = 0;
        if (!K32EnumProcesses(ids.data(), static_cast<DWORD>(ids.size() * sizeof(DWORD)), &bytes)) { return {}; }
        const auto count = bytes / sizeof(DWORD);
        if (count < ids.size()) { ids.resize(count); return ids; }
        ids.resize(ids.size() * 2);
    }
}

// A protected client can deny image-path queries while still appearing by executable name
// in the process snapshot. Keep it classified as running so a launch request cannot duplicate it.
inline std::vector<DWORD> process_ids_named(const wchar_t* name) {
    std::vector<DWORD> result;
    for (const auto pid : process_ids()) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        wchar_t path[32768] = {};
        DWORD length = 32768;
        if (process && QueryFullProcessImageNameW(process, 0, path, &length)) {
            const auto* slash = std::wcsrchr(path, L'\\');
            if (_wcsicmp(slash ? slash + 1 : path, name) == 0) { result.push_back(pid); }
        }
        if (process) { CloseHandle(process); }
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) { return result; }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, name) == 0 &&
                std::find(result.begin(), result.end(), entry.th32ProcessID) == result.end()) {
                result.push_back(entry.th32ProcessID);
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}
}
