// ArkHelperLauncher: loads ArkHelperRuntime.dll into a running or newly launched game with
// ordinary Windows loading (a WH_GETMESSAGE hook or LoadLibraryExW on a remote thread).
// No manual mapping, security hooks, driver changes or executable patching. Started without
// arguments (double-click) it runs an interactive console session.
#include <windows.h>

#include <algorithm>
#include <cstdio>

#include "command_line.h"
#include "runtime_loader.h"
#include "interactive_session.h"
#include "load_report.h"
#include "process_watch.h"
#include "scoped_handle.h"
#include "target.h"
#include "win32_utils.h"

namespace ark::loader {

namespace {

constexpr int kExitSuccess = 0;
constexpr int kExitFailure = 1;
constexpr int kExitInvalidOptions = 2;
constexpr DWORD kInputIdleWaitMs = 5000;

int exit_code(const LoadReport& report) {
    return report.success ? kExitSuccess : kExitFailure;
}

// Reports the loader's and the target's integrity levels without loading anything.
void run_integrity_diagnostic(const Target& target, LoadReport& report) {
    report.pid = target.pid;
    report.stage = "integrity_diagnostic";
    report.loader_integrity = process_integrity(GetCurrentProcess());
    ScopedHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, target.pid));
    if (!process.valid()) {
        report.error = GetLastError();
    } else if (!verify_target_identity(process.get(), target)) {
        report.error = ERROR_INVALID_NAME;
    } else {
        report.target_integrity = process_integrity(process.get(), &report.error);
        report.success = report.target_integrity != 0;
    }
}

// The self-test host checks IPC, battle services and runtime restart, then exits.
void await_test_host_exit(HANDLE host, bool watch, ULONGLONG deadline, LoadReport& report) {
    const DWORD result = WaitForSingleObject(host, remaining_ms(deadline));
    if (result != WAIT_OBJECT_0) {
        report.success = false;
        if (result == WAIT_TIMEOUT) {
            report.fail("test_host_exit_timeout", WAIT_TIMEOUT);
        } else {
            report.fail("test_host_exit_wait_failed", GetLastError());
        }
        return;
    }
    DWORD exit_status = 0;
    GetExitCodeProcess(host, &exit_status);
    if (exit_status != 0) {
        report.success = false;
        report.fail("test_host_checks_failed", exit_status);
        return;
    }
    report.stage =
        watch ? "watch_self_test_passed" : "self_test_passed";
}


void debug_watch_retry(int attempt) {
    std::fprintf(stderr, "watch retry %d after target exited before load\n", attempt);
    std::fflush(stderr);
}

int run_command(const CommandLineOptions& options) {
    const ULONGLONG deadline = GetTickCount64() + options.timeout_seconds * 1000ULL;
    const auto root = loader_directory();
    Target target{options.pid, 0, options.self_test ? kTestHostImageName : kGameImageName};
    PROCESS_INFORMATION test_host{};
    LoadReport report;
    report.watch = options.watch;

    int watch_attempt = 0;
    bool watched = false;
    if (options.watch) {
        for (;;) {
            WatchOptions watch_options;
            watch_options.self_test = options.self_test;
            watch_options.deadline = deadline;
            watch_options.accept_existing = watch_attempt > 0;
            report.watch = true;
            if (!watch_for_new_process(target, root, watch_options, test_host, report)) {
                ScopedHandle host_process(test_host.hProcess);
                ScopedHandle host_thread(test_host.hThread);
                write_report(report, options.report_path);
                return kExitFailure;
            }
            watched = true;
            ++watch_attempt;
            report = LoadReport{};
            report.watch = true;
            report.pid = target.pid;
            report.creation_time = target.creation_time;
            report = load_runtime(target, root, options.method, deadline);
            if (!report.success) {
                debug_note(L"[load] pid=%lu failed error=%lu stage=%hs\n", target.pid,
                           report.error, report.stage.c_str());
            }
            // The game launcher hands off to the real process quickly; right after it spawns the
            // module may not be initialised yet, so retry the very same target a couple of times
            // before going back to watching for a brand new process.
            for (int retry = 0; !report.success && retry < 8 && remaining_ms(deadline); ++retry) {
                if (report.error == ERROR_PROCESS_ABORTED || report.error == ERROR_INVALID_HANDLE) {
                    break;
                }
                ScopedHandle alive(OpenProcess(SYNCHRONIZE, FALSE, target.pid));
                if (!alive.valid() || WaitForSingleObject(alive.get(), 0) != WAIT_TIMEOUT) {
                    break;
                }
                Sleep(600);
                report = LoadReport{};
                report.watch = true;
                report.pid = target.pid;
                report.creation_time = target.creation_time;
                report = load_runtime(target, root, options.method, deadline);
                if (!report.success) {
                    debug_note(L"[load] pid=%lu retry=%d failed error=%lu stage=%hs\n", target.pid,
                               retry + 1, report.error, report.stage.c_str());
                }
            }
            if (report.success || watch_attempt >= 5 || !remaining_ms(deadline)) {
                break;
            }
            if (report.error != ERROR_PROCESS_ABORTED && report.error != ERROR_INVALID_HANDLE &&
                report.error != ERROR_ACCESS_DENIED && report.error != ERROR_NOT_FOUND &&
                report.error != WAIT_TIMEOUT) {
                break;
            }
            // The selected process died or refused the load (game launcher bootstrap); keep
            // watching so the relaunched process can still be caught.
            debug_watch_retry(watch_attempt);
        }
    } else if (options.self_test) {
        if (!create_test_host(root, test_host, report)) {
            write_report(report, options.report_path);
            return kExitFailure;
        }
        target.pid = test_host.dwProcessId;
        target.creation_time = process_creation_time(test_host.hProcess);
    }
    ScopedHandle host_process(test_host.hProcess);
    ScopedHandle host_thread(test_host.hThread);

    if (options.diagnose) {
        run_integrity_diagnostic(target, report);
        write_report(report, options.report_path);
        return exit_code(report);
    }

    // Give a just-selected process time to create its window before the hook looks for
    // it. A blocked hook installation is never retried.
    if (options.method == LoadMethod::WindowHook) {
        ScopedHandle selected(
            OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, target.pid));
        if (selected.valid()) {
            WaitForInputIdle(selected.get(),
                             std::min<DWORD>(kInputIdleWaitMs, remaining_ms(deadline)));
        }
    }

    const unsigned long long watch_armed_time = report.watch_armed_time;
    if (!watched) {
        report = load_runtime(target, root, options.method, deadline);
        report.watch = options.watch;
    }
    report.watch_armed_time = watch_armed_time;
    if (options.self_test && report.success) {
        await_test_host_exit(host_process.get(), options.watch, deadline, report);
    }
    write_report(report, options.report_path);
    return exit_code(report);
}

} // namespace

} // namespace ark::loader

int arkhelp_loader_main(int argc, wchar_t** argv) {
    using namespace ark::loader;

    if (argc == 1) {
        return run_interactive_session(loader_directory());
    }
    CommandLineOptions options;
    if (!parse_command_line(argc, argv, options)) {
        std::puts(kUsage);
        LoadReport report;
        report.fail("invalid_options", ERROR_INVALID_PARAMETER);
        write_report(report, options.report_path);
        return kExitInvalidOptions;
    }
    return run_command(options);
}
