#include "../native/loader/remote_thread_loader.cpp"
#include <cstdio>

int wmain() {
    try {
        for (const char* name : {"LoadLibraryExW", "GetLastError", "GetProcAddress"}) {
            const auto function = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), name);
            HMODULE owner = nullptr;
            if (!function || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(function), &owner)) {
                throw std::runtime_error("local export unavailable");
            }
            const auto path = ark::loader::module_path(owner);
            const auto offset = reinterpret_cast<uintptr_t>(function) - reinterpret_cast<uintptr_t>(owner);
            if (ark::loader::resolve_from_psapi(GetCurrentProcess(), path, offset) !=
                    reinterpret_cast<uintptr_t>(function) ||
                ark::loader::resolve_remote_export(GetCurrentProcessId(), GetCurrentProcess(), name,
                    GetTickCount64() + 2000) != reinterpret_cast<uintptr_t>(function)) {
                throw std::runtime_error("system export address mismatch");
            }
            bool bad_path_rejected = false, bad_offset_rejected = false;
            try {
                ark::loader::resolve_from_psapi(GetCurrentProcess(),
                    L"Z:\\invalid\\" + std::filesystem::path(path).filename().wstring(), offset);
            } catch (const std::runtime_error&) { bad_path_rejected = true; }
            try {
                ark::loader::resolve_from_psapi(GetCurrentProcess(), path, UINTPTR_MAX);
            } catch (const std::runtime_error&) { bad_offset_rejected = true; }
            if (!bad_path_rejected || !bad_offset_rejected)
                throw std::runtime_error("invalid module path or export offset accepted");
        }
        std::puts("PASS remote export resolution: Toolhelp/PSAPI addresses, exact system paths, image bounds");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
