// ArkHelper's private, headless game runtime. Product modules are linked into this
// DLL; no directory scan, renderer or separately distributed plugins are needed.
#include <windows.h>
#include <MinHook.h>
#include <ark/plugin.h>
#include <atomic>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include "plugin_manager.h"
#include "../modules/builtin.h"

namespace {
constexpr DWORD kPathCapacity = 32768;
HMODULE self_module = nullptr;
HANDLE process_guard = nullptr;
HANDLE ready_event = nullptr;
HANDLE failed_event = nullptr;
HANDLE worker = nullptr;
std::mutex boot_mutex;
std::atomic<bool> running{false};
std::atomic<bool> stopping{false};
std::atomic<bool> desktop_shutdown{false};
std::atomic<bool> failed{false};
struct State {
    std::filesystem::path root;
    bool test_host = false;
    std::unique_ptr<ark::core::PluginManager> modules;
};
// The DLL and published State remain alive until the process exits. Module
// callbacks and hooks still drain on shutdown; a clean session can be restarted.
std::atomic<State*> state{nullptr};

bool reject(DWORD error) { SetLastError(error); return false; }
HANDLE named_event(const wchar_t* kind) {
    const auto name = L"Local\\ArkHelp.Runtime." + std::wstring(kind) + L"." +
        std::to_wstring(GetCurrentProcessId());
    return CreateEventW(nullptr, TRUE, FALSE, name.c_str());
}
void run_session(State* current) {
    try {
        if (!current->modules) {
            current->modules = std::make_unique<ark::core::PluginManager>(current->root, current->test_host);
            arkhelp::runtime::register_product_modules(*current->modules);
        }
        auto& modules = *current->modules;
        const auto hook_status = MH_Initialize();
        if (hook_status != MH_OK && hook_status != MH_ERROR_ALREADY_INITIALIZED) {
            throw std::runtime_error("Native hook initialization failed");
        }
        if (!modules.load_now("damage_meter.dll") || !modules.load_now("ark_helper.dll")) {
            throw std::runtime_error("ArkHelper built-in module initialization failed");
        }
        modules.start();
        running = true;
        SetEvent(ready_event);
        modules.log(1, "ArkHelper runtime ready: linked damage capture and battle provider");
        ArkFrameV1 frame{};
        frame.delta_time = 0.016;
        while (!stopping) {
            modules.dispatch(ark::core::CallbackKind::Frame, frame);
            Sleep(16);
        }
        ResetEvent(ready_event);
        modules.stop();
    } catch (const std::exception& error) {
        ResetEvent(ready_event);
        failed = true;
        SetEvent(failed_event);
        if (current->modules) {
            current->modules->log(3, error.what());
            current->modules->stop();
        }
    }
    running = false;
}

DWORD WINAPI run(void*) {
    auto* current = state.load();
    HANDLE desktop_stop = OpenEventW(SYNCHRONIZE, FALSE, L"Local\\ArkHelp.RuntimeStop");
    for (;;) {
        run_session(current);
        if (failed || !current->modules || !current->modules->fully_stopped() || !desktop_stop ||
            (!desktop_shutdown && WaitForSingleObject(desktop_stop, 0) != WAIT_OBJECT_0)) break;
        // The pinned DLL waits without hooks or publications until a confirmed desktop
        // session returns. Reopening the UI reuses this thread instead of loading again.
        for (;;) {
            Sleep(250);
            if (WaitForSingleObject(desktop_stop, 0) != WAIT_TIMEOUT) continue;
            HANDLE desktop = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\ArkHelp.App");
            if (!desktop) continue;
            CloseHandle(desktop);
            break;
        }
        stopping = false;
        desktop_shutdown = false;
        ResetEvent(failed_event);
    }
    if (desktop_stop) CloseHandle(desktop_stop);
    return 0;
}
}

ARK_EXPORT bool arkhelp_runtime_start(const ArkBootOptionsV1* options) {
    std::lock_guard lock(boot_mutex);
    if (options && (options->size < sizeof(*options) || options->test_host > 1)) {
        return reject(ERROR_INVALID_PARAMETER);
    }
    if (worker && WaitForSingleObject(worker, 0) == WAIT_TIMEOUT) {
        return !failed && !stopping ? true : reject(ERROR_BUSY);
    }
    auto* current = state.load();
    if (current && current->modules && !current->modules->fully_stopped()) {
        return reject(ERROR_BUSY);
    }
    if (worker) { CloseHandle(worker); worker = nullptr; }
    wchar_t executable[kPathCapacity];
    const DWORD length = GetModuleFileNameW(nullptr, executable, kPathCapacity);
    if (!length || length >= kPathCapacity) { return reject(ERROR_BAD_PATHNAME); }
    const bool test_host = _wcsicmp(std::filesystem::path(executable).filename().c_str(),
                                   L"ArkHelperRuntimeHost.exe") == 0;
    if (options && options->test_host && !test_host) { return reject(ERROR_ACCESS_DENIED); }
    if (!current) {
        wchar_t module_path[kPathCapacity];
        const DWORD count = GetModuleFileNameW(self_module, module_path, kPathCapacity);
        if (!count || count >= kPathCapacity) { return reject(ERROR_BAD_PATHNAME); }
        std::filesystem::path root = options && options->root_directory ?
            std::filesystem::path(options->root_directory) : std::filesystem::path(module_path).parent_path();
        if (!root.is_absolute()) { return reject(ERROR_BAD_PATHNAME); }
        current = new State{std::move(root), test_host, {}};
        state.store(current);
    }
    if (!process_guard) {
        const auto name = L"Local\\ArkHelp.Runtime.Core." + std::to_wstring(GetCurrentProcessId());
        process_guard = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!process_guard) { return false; }
        if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(process_guard); process_guard = nullptr; return reject(ERROR_ALREADY_EXISTS); }
    }
    if (!failed_event) { failed_event = named_event(L"Failed"); }
    if (!ready_event) { ready_event = named_event(L"Ready"); }
    if (!ready_event || !failed_event) { return false; }
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&run), &pinned)) { return false; }
    stopping = false;
    desktop_shutdown = false;
    failed = false;
    ResetEvent(ready_event);
    ResetEvent(failed_event);
    worker = CreateThread(nullptr, 0, run, nullptr, 0, nullptr);
    if (!worker) { failed = true; SetEvent(failed_event); return false; }
    return true;
}
ARK_EXPORT bool arkhelp_runtime_is_running() { return running; }
ARK_EXPORT void arkhelp_runtime_shutdown() {
    // Latch the stop reason before module cleanup. A rapidly reopened desktop
    // can reset the named event while the old modules are still draining.
    HANDLE desktop_stop=OpenEventW(SYNCHRONIZE,FALSE,L"Local\\ArkHelp.RuntimeStop");
    if(desktop_stop) {
        if(WaitForSingleObject(desktop_stop,0)==WAIT_OBJECT_0)desktop_shutdown=true;
        CloseHandle(desktop_stop);
    }
    stopping = true;
    if (ready_event) { ResetEvent(ready_event); }
}
ARK_EXPORT bool arkhelp_runtime_with_service(const char* name, uint32_t abi,
        size_t minimum_size, ArkServiceCallback callback, void* user) {
    auto* current = state.load();
    return running && !stopping && current && current->test_host &&
        current->modules->with_external_service(name, abi, minimum_size, callback, user);
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { self_module = module; }
    return TRUE;
}
