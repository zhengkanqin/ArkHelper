#pragma once

#include <windows.h>

#include <stdexcept>
#include <string>

namespace ark::il2cpp {

inline constexpr wchar_t kGameAssemblyModule[] = L"GameAssembly.dll";

// Returns the loaded IL2CPP runtime module, or nullptr before the game has loaded it.
inline HMODULE game_assembly() {
    return GetModuleHandleW(kGameAssemblyModule);
}

// Resolves an exported IL2CPP API function. Throws when the module or the export is missing.
template <class Function>
Function resolve_export(const char* name) {
    const HMODULE module = game_assembly();
    const FARPROC address = module ? GetProcAddress(module, name) : nullptr;
    if (!address) {
        throw std::runtime_error(std::string("Missing IL2CPP export: ") + name);
    }
    return reinterpret_cast<Function>(address);
}

// True when `address` lies in committed, executable, non-guard memory. Used to validate
// native entry points before they are called or hooked.
inline bool is_executable(const void* address) {
    constexpr DWORD kExecutableProtection =
        PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info))) {
        return false;
    }
    return info.State == MEM_COMMIT && (info.Protect & kExecutableProtection) != 0 &&
           (info.Protect & PAGE_GUARD) == 0;
}

} // namespace ark::il2cpp
