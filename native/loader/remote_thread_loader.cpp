#include "remote_thread_loader.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <wchar.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "scoped_handle.h"
#include "win32_utils.h"

namespace ark::loader {

namespace {

// Parameter block shared with the remote thread. The offsets are hard-coded in
// kLoadThreadCode below and must never change.
struct RemoteData {
    uintptr_t load_library_ex;
    uintptr_t get_last_error;
    uintptr_t get_proc_address;
    uintptr_t module;
    uintptr_t start;
    DWORD error;
    DWORD phase;
    char start_name[32] = "arkhelp_runtime_start";
    wchar_t path[32768];
};

static_assert(offsetof(RemoteData, load_library_ex) == 0x00);
static_assert(offsetof(RemoteData, get_last_error) == 0x08);
static_assert(offsetof(RemoteData, get_proc_address) == 0x10);
static_assert(offsetof(RemoteData, module) == 0x18);
static_assert(offsetof(RemoteData, start) == 0x20);
static_assert(offsetof(RemoteData, error) == 0x28);
static_assert(offsetof(RemoteData, start_name) == 0x30);
static_assert(offsetof(RemoteData, path) == 80 && offsetof(RemoteData, phase) == 44);

// Progress markers written to RemoteData::phase by the remote thread.
constexpr DWORD kPhaseLoadLibrary = 1;
constexpr DWORD kPhaseResolveStart = 2;
constexpr DWORD kPhaseInvokeStart = 3;
constexpr DWORD kPhaseStarted = 4;

// Remote thread procedure; receives &RemoteData in RCX. Follows the x64 Windows ABI:
// preserves RBX, keeps the stack 16-byte aligned and reserves 32 bytes of shadow space.
// clang-format off
constexpr unsigned char kLoadThreadCode[] = {
    0x53,                                     // push rbx
    0x48, 0x83, 0xec, 0x20,                   // sub rsp, 32
    0x48, 0x89, 0xcb,                         // mov rbx, rcx
    0xc7, 0x43, 0x2c, 0x01, 0x00, 0x00, 0x00, // phase = 1 (load library)
    0x48, 0x8d, 0x4b, 0x50,                   // lea rcx, [rbx+path]
    0x31, 0xd2,                               // xor edx, edx
    0x41, 0xb8, 0x00, 0x11, 0x00, 0x00,       // mov r8d, DLL_LOAD_DIR | DEFAULT_DIRS
    0xff, 0x13,                               // call LoadLibraryExW
    0x48, 0x89, 0x43, 0x18,                   // module = rax
    0x48, 0x85, 0xc0,                         // test rax, rax
    0x74, 0x32,                               // jz load_error
    0xc7, 0x43, 0x2c, 0x02, 0x00, 0x00, 0x00, // phase = 2 (resolve start)
    0x48, 0x89, 0xc1,                         // mov rcx, rax
    0x48, 0x8d, 0x53, 0x30,                   // lea rdx, [rbx+start_name]
    0xff, 0x53, 0x10,                         // call GetProcAddress
    0x48, 0x89, 0x43, 0x20,                   // start = rax
    0x48, 0x85, 0xc0,                         // test rax, rax
    0x74, 0x20,                               // jz missing_start
    0xc7, 0x43, 0x2c, 0x03, 0x00, 0x00, 0x00, // phase = 3 (invoke start)
    0x31, 0xc9,                               // xor ecx, ecx
    0xff, 0xd0,                               // call rax: start(nullptr)
    0x84, 0xc0,                               // test al, al
    0x74, 0x1a,                               // jz start_failed
    0xc7, 0x43, 0x2c, 0x04, 0x00, 0x00, 0x00, // phase = 4 (started)
    0xeb, 0x18,                               // jmp done
    // load_error:
    0xff, 0x53, 0x08,                         // call GetLastError
    0x89, 0x43, 0x28,                         // error = eax
    0xeb, 0x10,                               // jmp done
    // missing_start:
    0xc7, 0x43, 0x28, 0x7f, 0x00, 0x00, 0x00, // error = ERROR_PROC_NOT_FOUND
    0xeb, 0x07,                               // jmp done
    // start_failed:
    0xc7, 0x43, 0x28, 0x5a, 0x04, 0x00, 0x00, // error = ERROR_DLL_INIT_FAILED
    // done:
    0x31, 0xc0,                               // xor eax, eax
    0x48, 0x83, 0xc4, 0x20,                   // add rsp, 32
    0x5b,                                     // pop rbx
    0xc3,                                     // ret
};
// clang-format on

constexpr DWORD kProcessAccess = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                 PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
                                 SYNCHRONIZE;
constexpr DWORD kModulePollIntervalMs = 10;

// Code and data pages allocated in the target. Release is explicit because a load thread
// that is still running after a timeout must keep both mapped.
class RemoteAllocations {
public:
    explicit RemoteAllocations(HANDLE process) : process_(process) {}

    void* data = nullptr;
    void* code = nullptr;

    void release() {
        if (code) {
            VirtualFreeEx(process_, code, 0, MEM_RELEASE);
        }
        if (data) {
            VirtualFreeEx(process_, data, 0, MEM_RELEASE);
        }
    }

private:
    HANDLE process_;
};

// Toolhelp can return an incomplete module snapshot for an initialized client.
// PSAPI provides a second documented enumeration path; accept only the exact
// system DLL path and a function offset inside that image.
uintptr_t resolve_from_psapi(HANDLE process, const std::wstring& owner_path, uintptr_t offset) {
    std::vector<HMODULE> modules(256);
    DWORD needed = 0;
    if (!EnumProcessModulesEx(process, modules.data(),
            static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed, LIST_MODULES_ALL)) {
        return 0;
    }
    if (needed > modules.size() * sizeof(HMODULE)) {
        if (needed > 4096 * sizeof(HMODULE)) { return 0; }
        modules.resize((needed + sizeof(HMODULE) - 1) / sizeof(HMODULE));
        if (!EnumProcessModulesEx(process, modules.data(),
                static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed, LIST_MODULES_ALL) ||
            needed > modules.size() * sizeof(HMODULE)) {
            return 0;
        }
    }
    const std::wstring owner_name = std::filesystem::path(owner_path).filename().wstring();
    for (size_t i = 0; i < needed / sizeof(HMODULE); ++i) {
        wchar_t path[32768]{};
        const DWORD length = GetModuleFileNameExW(process, modules[i], path, 32768);
        if (!length || length >= 32768 ||
            _wcsicmp(std::filesystem::path(path).filename().c_str(), owner_name.c_str()) != 0) {
            continue;
        }
        MODULEINFO info{};
        if (_wcsicmp(path, owner_path.c_str()) != 0 ||
            !GetModuleInformation(process, modules[i], &info, sizeof(info)) || offset >= info.SizeOfImage) {
            SetLastError(ERROR_INVALID_DATA);
            throw std::runtime_error("system_module_path_mismatch");
        }
        return reinterpret_cast<uintptr_t>(info.lpBaseOfDll) + offset;
    }
    return 0;
}

// Translates the address of a kernel32 export in this process into the target's address
// space, waiting until the target has mapped the same system module. Throws
// std::runtime_error carrying the report stage; the last error describes the cause.
uintptr_t resolve_remote_export(DWORD pid, HANDLE process, const char* name, ULONGLONG deadline) {
    const auto function = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), name);
    HMODULE owner = nullptr;
    if (!function || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                         reinterpret_cast<LPCWSTR>(function), &owner)) {
        throw std::runtime_error("resolve_system_export");
    }
    const std::wstring owner_path = module_path(owner);
    const std::wstring owner_name = std::filesystem::path(owner_path).filename().wstring();
    const uintptr_t offset =
        reinterpret_cast<uintptr_t>(function) - reinterpret_cast<uintptr_t>(owner);

    for (;;) {
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
            SetLastError(ERROR_PROCESS_ABORTED);
            throw std::runtime_error("target_exited_before_load");
        }
        if (!remaining_ms(deadline)) {
            SetLastError(WAIT_TIMEOUT);
            throw std::runtime_error("system_modules_timeout");
        }
        ScopedHandle snapshot(
            CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
        DWORD snapshot_error = 0;
        if (!snapshot.valid()) {
            snapshot_error = GetLastError();
        } else {
            MODULEENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            for (BOOL ok = Module32FirstW(snapshot.get(), &entry); ok;
                 ok = Module32NextW(snapshot.get(), &entry)) {
                if (_wcsicmp(entry.szModule, owner_name.c_str()) != 0) {
                    continue;
                }
                // Reject same-named local proxies. Only startup module availability is
                // awaited; the remote entry itself is never retried.
                if (_wcsicmp(entry.szExePath, owner_path.c_str()) != 0 ||
                    offset >= entry.modBaseSize) {
                    SetLastError(ERROR_INVALID_DATA);
                    throw std::runtime_error("system_module_path_mismatch");
                }
                return reinterpret_cast<uintptr_t>(entry.modBaseAddr) + offset;
            }
        }
        if (const auto address = resolve_from_psapi(process, owner_path, offset)) {
            return address;
        }
        // A process that is still initializing reports these transiently.
        if (snapshot_error && snapshot_error != ERROR_BAD_LENGTH && snapshot_error != ERROR_PARTIAL_COPY) {
            SetLastError(snapshot_error);
            throw std::runtime_error("system_module_snapshot_failed");
        }
        WaitForSingleObject(process,
                            std::min<DWORD>(kModulePollIntervalMs, remaining_ms(deadline)));
    }
}

void resolve_remote_imports(DWORD pid, HANDLE process, ULONGLONG deadline, RemoteData& data) {
    data.load_library_ex = resolve_remote_export(pid, process, "LoadLibraryExW", deadline);
    data.get_last_error = resolve_remote_export(pid, process, "GetLastError", deadline);
    data.get_proc_address = resolve_remote_export(pid, process, "GetProcAddress", deadline);
}

const char* stage_for_incomplete_phase(DWORD phase) {
    switch (phase) {
    case kPhaseLoadLibrary:
        return "load_library_failed";
    case kPhaseResolveStart:
        return "start_export_missing";
    case kPhaseInvokeStart:
        return "runtime_start_rejected";
    default:
        return "entry_did_not_complete";
    }
}

} // namespace

LoadReport load_with_remote_thread(const Target& target, const std::filesystem::path& core_path,
                                   ULONGLONG deadline) {
    const DWORD pid = target.pid;
    LoadReport report;
    report.pid = pid;
    report.creation_time = target.creation_time;
    report.stage = "open_process";

    ScopedHandle process(OpenProcess(kProcessAccess, FALSE, pid));
    if (!process.valid()) {
        report.error = GetLastError();
        return report;
    }
    if (!verify_target_identity(process.get(), target)) {
        return report.fail("target_identity_rejected", GetLastError());
    }

    RemoteData data{};
    try {
        resolve_remote_imports(pid, process.get(), deadline, data);
    } catch (const std::exception& failure) {
        const DWORD error = GetLastError();
        return report.fail(failure.what(), error);
    }

    const std::wstring path = std::filesystem::absolute(core_path).wstring();
    if (path.size() >= std::size(data.path) ||
        GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return report.fail("core_missing", ERROR_FILE_NOT_FOUND);
    }
    std::memcpy(data.path, path.c_str(), (path.size() + 1) * sizeof(wchar_t));

    RemoteAllocations remote(process.get());
    const auto fail = [&](const char* stage) {
        const DWORD error = GetLastError();
        remote.release();
        return report.fail(stage, error);
    };

    remote.data = VirtualAllocEx(process.get(), nullptr, sizeof(data), MEM_COMMIT | MEM_RESERVE,
                                 PAGE_READWRITE);
    if (!remote.data) {
        return fail("allocate_data");
    }
    remote.code = VirtualAllocEx(process.get(), nullptr, sizeof(kLoadThreadCode),
                                 MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote.code) {
        return fail("allocate_call");
    }
    SIZE_T transferred = 0;
    if (!WriteProcessMemory(process.get(), remote.data, &data, sizeof(data), &transferred) ||
        transferred != sizeof(data)) {
        return fail("write_data");
    }
    if (!WriteProcessMemory(process.get(), remote.code, kLoadThreadCode, sizeof(kLoadThreadCode),
                            &transferred) ||
        transferred != sizeof(kLoadThreadCode)) {
        return fail("write_call");
    }
    DWORD previous_protection = 0;
    if (!VirtualProtectEx(process.get(), remote.code, sizeof(kLoadThreadCode), PAGE_EXECUTE_READ,
                          &previous_protection)) {
        return fail("protect_call");
    }
    if (!FlushInstructionCache(process.get(), remote.code, sizeof(kLoadThreadCode))) {
        return fail("flush_call");
    }
    if (!remaining_ms(deadline)) {
        SetLastError(WAIT_TIMEOUT);
        return fail("load_deadline_expired");
    }

    ScopedHandle thread(CreateRemoteThread(process.get(), nullptr, 0,
                                           reinterpret_cast<LPTHREAD_START_ROUTINE>(remote.code),
                                           remote.data, 0, nullptr));
    if (!thread.valid()) {
        return fail("create_thread");
    }

    HANDLE waits[] = {process.get(), thread.get()};
    const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, remaining_ms(deadline));
    if (wait == WAIT_OBJECT_0) {
        report.stage = "target_exited_during_load";
        GetExitCodeProcess(process.get(), &report.error);
        return report;
    }
    if (wait != WAIT_OBJECT_0 + 1) {
        // The thread may still be executing the remote code: leave both pages mapped.
        const bool timed_out = wait == WAIT_TIMEOUT;
        const DWORD error = timed_out ? WAIT_TIMEOUT : GetLastError();
        report.fail(timed_out ? "load_thread_timeout" : "load_thread_wait_failed", error);
        report.retained_remote_memory = true;
        GetExitCodeThread(thread.get(), &report.thread_exit);
        if (ReadProcessMemory(process.get(), remote.data, &data, sizeof(data), &transferred) &&
            transferred == sizeof(data)) {
            report.phase = data.phase;
            report.module = data.module;
        }
        return report;
    }

    GetExitCodeThread(thread.get(), &report.thread_exit);
    if (!ReadProcessMemory(process.get(), remote.data, &data, sizeof(data), &transferred) ||
        transferred != sizeof(data)) {
        return fail("read_result");
    }
    report.phase = data.phase;
    report.module = data.module;
    report.error = data.error;
    report.success = data.phase == kPhaseStarted && data.module != 0;
    report.stage = report.success ? "library_started" : stage_for_incomplete_phase(data.phase);
    remote.release();
    return report;
}

} // namespace ark::loader
