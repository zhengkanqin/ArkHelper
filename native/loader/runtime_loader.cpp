#include "runtime_loader.h"

#include <windows.h>

#include <string>

#include "remote_thread_loader.h"
#include "scoped_handle.h"
#include "win32_utils.h"
#include "window_hook_loader.h"

namespace ark::loader {

namespace {

// Waits for the started core to report its outcome. The stage reached by the library
// load is preserved in `library_stage`.
void wait_for_runtime_ready(LoadReport& report, HANDLE process, HANDLE ready, HANDLE failed,
                              ULONGLONG deadline) {
    report.library_stage = report.stage;
    report.success = false;
    HANDLE waits[] = {process, failed, ready};
    const DWORD result = WaitForMultipleObjects(3, waits, FALSE, remaining_ms(deadline));
    if (result == WAIT_OBJECT_0) {
        report.stage = "target_exited_before_ready";
        GetExitCodeProcess(process, &report.error);
    } else if (result == WAIT_OBJECT_0 + 1) {
        report.fail("runtime_startup_failed", ERROR_DLL_INIT_FAILED);
    } else if (result == WAIT_OBJECT_0 + 2) {
        report.fail("runtime_ready", 0);
        report.success = true;
    } else if (result == WAIT_TIMEOUT) {
        report.fail("runtime_ready_timeout", WAIT_TIMEOUT);
    } else {
        report.fail("runtime_ready_wait_failed", GetLastError());
    }
}

} // namespace

LoadReport load_runtime(Target& target, const std::filesystem::path& root, LoadMethod method,
                          ULONGLONG deadline) {
    LoadReport report;
    report.pid = target.pid;
    report.creation_time = target.creation_time;
    report.stage = "open_target_identity";

    ScopedHandle process(
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, target.pid));
    if (!process.valid()) {
        report.error = GetLastError();
        return report;
    }
    if (!verify_target_identity(process.get(), target)) {
        return report.fail("target_identity_rejected", GetLastError());
    }
    // Pin the identity so the method-specific loaders re-check the same process.
    if (!target.creation_time) {
        target.creation_time = process_creation_time(process.get());
    }
    report.creation_time = target.creation_time;
    if (!target.creation_time) {
        return report.fail("target_creation_query_failed", GetLastError());
    }
    if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) {
        return report.fail("target_exited_before_load", ERROR_PROCESS_ABORTED);
    }

    // Create the result events before loading so a fast core cannot signal unobserved.
    const std::wstring ready_name = runtime_object_name(L"Ready", target.pid);
    ScopedHandle ready(CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str()));
    if (!ready.valid()) {
        return report.fail("create_ready_event", GetLastError());
    }
    const std::wstring failed_name = runtime_object_name(L"Failed", target.pid);
    ScopedHandle failed(CreateEventW(nullptr, TRUE, FALSE, failed_name.c_str()));
    if (!failed.valid()) {
        return report.fail("create_failed_event", GetLastError());
    }

    // An existing core mutex means the runtime already runs in this process: only
    // observe its outcome, never load a second time.
    const std::wstring core_name = runtime_object_name(L"Core", target.pid);
    ScopedHandle core_mutex(OpenMutexW(SYNCHRONIZE, FALSE, core_name.c_str()));
    if (core_mutex.valid() && WaitForSingleObject(ready.get(), 0) == WAIT_OBJECT_0) {
        report.stage = "core_mutex_present";
        wait_for_runtime_ready(report, process.get(), ready.get(), failed.get(), deadline);
        return report;
    }
    if (!remaining_ms(deadline)) {
        return report.fail("load_deadline_expired", WAIT_TIMEOUT);
    }

    report = method == LoadMethod::WindowHook
                 ? load_with_window_hook(target, root, deadline)
                 : load_with_remote_thread(target, root / L"ArkHelperRuntime.dll", deadline);
    if (!report.success) {
        return report;
    }
    wait_for_runtime_ready(report, process.get(), ready.get(), failed.get(), deadline);
    return report;
}

} // namespace ark::loader
