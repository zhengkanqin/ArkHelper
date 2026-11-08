#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

namespace ark::loader {

// Full path of `module` (nullptr = this executable). Throws std::runtime_error on failure.
std::wstring module_path(HMODULE module);

// Directory containing the loader executable; the runtime files live beside it.
std::filesystem::path loader_directory();

ULONGLONG filetime_to_u64(const FILETIME& time);

// Process creation time as a FILETIME value, or 0 when it cannot be queried.
ULONGLONG process_creation_time(HANDLE process);

// Milliseconds left until a GetTickCount64-based deadline, clamped at zero.
DWORD remaining_ms(ULONGLONG deadline);

// Mandatory integrity RID of the process token, or 0 on failure. When `error` is given it
// receives 0 on success and the Windows error of the failing call otherwise.
DWORD process_integrity(HANDLE process, DWORD* error = nullptr);

// Name of a per-process runtime kernel object: `Local\ArkHelp.Runtime.<kind>.<pid>`.
std::wstring runtime_object_name(const wchar_t* kind, DWORD pid);

} // namespace ark::loader
