#include "window_hook_loader.h"

#include <windows.h>

#include <fstream>
#include <algorithm>
#include <string>

#include "scoped_handle.h"
#include "process_watch.h"
#include "win32_utils.h"

namespace ark::loader {

namespace {

constexpr LONG kMinWindowWidth = 320;
constexpr LONG kMinWindowHeight = 200;

struct WindowSearch {
    DWORD pid;
    DWORD thread_id = 0;
    LONG area = 0;
    HWND window = nullptr;
};

// Tracks the largest visible, unowned top-level window of the target process.
BOOL CALLBACK consider_window(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    DWORD pid = 0;
    const DWORD thread_id = GetWindowThreadProcessId(window, &pid);
    RECT client{};
    if (pid == search.pid && IsWindowVisible(window) && !GetWindow(window, GW_OWNER) &&
        GetClientRect(window, &client)) {
        const LONG width = client.right - client.left;
        const LONG height = client.bottom - client.top;
        wchar_t class_name[64]{};
        GetClassNameW(window, class_name, 64);
        const bool minimized_game = IsIconic(window) && _wcsicmp(class_name, L"UnityWndClass") == 0;
        // A minimized Unity window has a zero-sized client but its UI thread still
        // pumps messages. Reconnect without restoring, resizing or closing the game.
        const LONG area = minimized_game ? 1 : width * height;
        if (((width >= kMinWindowWidth && height >= kMinWindowHeight) || minimized_game) &&
            area > search.area) {
            search.thread_id = thread_id;
            search.area = area;
            search.window = window;
        }
    }
    return TRUE;
}

// Main game window and its UI thread, or a zero thread ID when none is suitable.
WindowSearch find_game_window(DWORD pid) {
    WindowSearch search{pid};
    EnumWindows(consider_window, reinterpret_cast<LPARAM>(&search));
    return search;
}

// A minimized client may stop retrieving posted thread messages while still
// processing sent window messages. WM_NULL has no game action or focus effect.
void nudge_window_thread(HWND window, DWORD thread_id, ULONGLONG deadline) {
    PostThreadMessageW(thread_id, WM_NULL, 0, 0);
    DWORD_PTR ignored = 0;
    const DWORD timeout = std::min<DWORD>(1000, remaining_ms(deadline));
    if (window && timeout) {
        SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK,
                            timeout, &ignored);
    }
}

const char* stage_for_bootstrap_error(DWORD error) {
    switch (error) {
    case ERROR_PROC_NOT_FOUND:
        return "window_hook_start_export_missing";
    case ERROR_DLL_INIT_FAILED:
        return "window_hook_start_rejected";
    default:
        return "window_hook_load_or_start_failed";
    }
}

// Installs the hook on `thread_id`, nudges the thread's message loop and waits for the
// bootstrap's HookReady signal. The hook and the local bootstrap reference are dropped as
// soon as the wait ends; the bootstrap pins itself inside the target.
bool run_window_hook(const std::filesystem::path& root, HWND window, DWORD thread_id, HANDLE process,
                     HANDLE hook_ready, ULONGLONG deadline, LoadReport& report) {
    HMODULE bootstrap =
        LoadLibraryExW((root / L"ArkHelperBootstrap.dll").c_str(), nullptr,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!bootstrap) {
        report.fail("load_local_bootstrap", GetLastError());
        return false;
    }
    const auto procedure =
        reinterpret_cast<HOOKPROC>(GetProcAddress(bootstrap, "ArkHelperBootstrapHook"));
    if (!procedure) {
        report.fail("bootstrap_export_missing", GetLastError());
        FreeLibrary(bootstrap);
        return false;
    }
    if (!remaining_ms(deadline)) {
        report.fail("hook_deadline_expired", WAIT_TIMEOUT);
        FreeLibrary(bootstrap);
        return false;
    }
    HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, procedure, bootstrap, thread_id);
    if (!hook) {
        report.fail("install_window_hook", GetLastError());
        FreeLibrary(bootstrap);
        return false;
    }
    // Keep GETMESSAGE for ordinary message loops and also cover minimized clients
    // that only dispatch sent messages. Both callbacks share the bootstrap guard.
    HHOOK sent_hook = SetWindowsHookExW(WH_CALLWNDPROC, procedure, bootstrap, thread_id);
    nudge_window_thread(window, thread_id, deadline);

    HANDLE waits[] = {process, hook_ready};
    const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, remaining_ms(deadline));
    if (sent_hook) { UnhookWindowsHookEx(sent_hook); }
    UnhookWindowsHookEx(hook);
    FreeLibrary(bootstrap);
    if (wait == WAIT_OBJECT_0 + 1) {
        return true;
    }
    if (wait == WAIT_OBJECT_0) {
        report.stage = "target_exited_before_hook";
    } else if (wait == WAIT_TIMEOUT) {
        report.stage = "window_hook_timeout";
    } else {
        report.stage = "window_hook_wait_failed";
    }
    report.error = wait == WAIT_TIMEOUT ? WAIT_TIMEOUT : GetLastError();
    return false;
}

} // namespace

LoadReport load_with_window_hook(const Target& target, const std::filesystem::path& root,
                                 ULONGLONG deadline) {
    const DWORD pid = target.pid;
    LoadReport report;
    report.pid = pid;
    report.creation_time = target.creation_time;
    report.stage = "open_process";

    ScopedHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    if (!process.valid()) {
        report.error = GetLastError();
        return report;
    }
    if (!verify_target_identity(process.get(), target)) {
        return report.fail("target_identity_rejected", ERROR_INVALID_NAME);
    }

    // A hook DLL cannot be injected into a process of higher integrity.
    report.loader_integrity = process_integrity(GetCurrentProcess());
    report.target_integrity = process_integrity(process.get(), &report.error);
    if (!report.target_integrity && report.error) {
        report.stage = "target_token_query_denied";
        return report;
    }
    if (report.target_integrity > report.loader_integrity && report.loader_integrity) {
        // Token integrity is advisory only: the hook call itself decides whether it can load.
        // Log the mismatch and keep going instead of refusing to load.
        debug_note(L"[hook] integrity mismatch pid=%lu loader=%llu target=%llu (trying)\n", pid,
                   report.loader_integrity, report.target_integrity);
    }

    // Console self-test hosts and newly launched games may create their window
    // after WaitForInputIdle returns. Give the window its own bounded wait.
    WindowSearch window{pid};
    const ULONGLONG window_deadline = std::min(deadline, GetTickCount64() + 5000ULL);
    do {
        window = find_game_window(pid);
        if (window.thread_id || WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) { break; }
        Sleep(25);
    } while (GetTickCount64() < window_deadline);
    if (!window.thread_id) {
        return report.fail("game_window_missing", ERROR_NOT_FOUND);
    }

    // The HookReady event doubles as a guard against concurrent hook loads.
    const std::wstring ready_name = runtime_object_name(L"HookReady", pid);
    ScopedHandle hook_ready(CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str()));
    if (!hook_ready.valid()) {
        return report.fail("create_result_event", GetLastError());
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return report.fail("another_hook_load_pending", ERROR_BUSY);
    }

    std::filesystem::create_directories(root / L"logs");
    const auto result_path = root / L"logs" / (L"ArkHelperBootstrap-" + std::to_wstring(pid) + L".txt");
    {
        std::ofstream file(result_path, std::ios::trunc);
        if (!file) {
            return report.fail("prepare_hook_report", ERROR_WRITE_FAULT);
        }
    }

    if (!run_window_hook(root, window.window, window.thread_id, process.get(), hook_ready.get(), deadline, report)) {
        return report;
    }

    // The bootstrap writes "<pid> <module> <error>".
    DWORD reported_pid = 0;
    unsigned long long loaded_module = 0;
    {
        std::ifstream file(result_path);
        file >> reported_pid >> loaded_module >> report.error;
        if (!file || reported_pid != pid) {
            return report.fail("invalid_hook_result", ERROR_INVALID_DATA);
        }
    }
    report.phase = 2;
    report.module = static_cast<uintptr_t>(loaded_module);
    report.success = loaded_module != 0;
    report.stage =
        report.success ? "window_hook_library_started" : stage_for_bootstrap_error(report.error);
    return report;
}

} // namespace ark::loader
