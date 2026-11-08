#include "process_watch.h"

#include <wchar.h>

#include <algorithm>
#include <cstdarg>
#include <cwchar>
#include <cstdio>
#include <string>
#include <vector>

#include "console.h"
#include "scoped_handle.h"
#include "win32_utils.h"

namespace ark::loader {

// Optional file logger: set ARK_LOADER_DEBUG_FILE to a path to capture watch decisions
// from contexts where stderr is not visible (scheduled tasks, hidden launchers).
void debug_log(const wchar_t* format, ...) {
    wchar_t path[MAX_PATH];
    if (GetEnvironmentVariableW(L"ARK_LOADER_DEBUG_FILE", path, MAX_PATH) == 0) {
        return;
    }
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    wchar_t line[512];
    va_list args;
    va_start(args, format);
    const int count = _vsnwprintf_s(line, 512, _TRUNCATE, format, args);
    va_end(args);
    if (count > 0) {
        const DWORD bytes = static_cast<DWORD>(count * sizeof(wchar_t));
        DWORD written = 0;
        WriteFile(file, line, bytes, &written, nullptr);
    }
    CloseHandle(file);
}

// Exposed for main.cpp so load failures land in the same ARK_LOADER_DEBUG_FILE log.
void debug_note(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    wchar_t line[512];
    _vsnwprintf_s(line, 512, _TRUNCATE, format, args);
    va_end(args);
    debug_log(L"%ls", line);
}

namespace {

constexpr DWORD kPollIntervalMs = 25;

enum class ScanResult {
    NotFound,
    Selected,
    Failed,
};

// Enumerates every visible process id. Toolhelp32 enumeration is avoided here: on this
// machine it terminates early (ERROR_NO_MORE_FILES after a couple dozen entries), which made
// the watcher blind to applications started later. EnumProcesses reports the full list.
extern "C" __declspec(dllimport) BOOL WINAPI K32EnumProcesses(DWORD* ids, DWORD bytes,
                                                             LPDWORD returned);

bool enumerate_process_ids(std::vector<DWORD>& ids) {
    ids.assign(512, 0);
    for (;;) {
        DWORD bytes = 0;
        if (!K32EnumProcesses(ids.data(), static_cast<DWORD>(ids.size() * sizeof(DWORD)), &bytes)) {
            return false;
        }
        const std::size_t count = bytes / sizeof(DWORD);
        if (count < ids.size()) {
            ids.resize(count);
            return true;
        }
        ids.resize(ids.size() * 2);
    }
}

// Image file name of a process, empty when the process cannot be queried.
std::wstring process_image_file_name(DWORD pid) {
    ScopedHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process.valid()) {
        return {};
    }
    wchar_t buffer[1024];
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (!QueryFullProcessImageNameW(process.get(), 0, buffer, &size)) {
        return {};
    }
    std::wstring path(buffer, size);
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// Records every existing PID without opening any of those processes. A PID that is
// reused during the watch is therefore conservatively never selected either.
bool capture_baseline(std::vector<DWORD>& baseline, LoadReport& report) {
    if (!enumerate_process_ids(baseline)) {
        report.fail("watch_baseline_enumeration_failed", GetLastError());
        return false;
    }
    debug_log(L"[baseline] total=%zu\n", baseline.size());
    return true;
}

// Decides whether one candidate process is the newly launched target.
ScanResult examine_pid(DWORD pid, Target& target, const WatchOptions& options,
                       const PROCESS_INFORMATION& test_host, LoadReport& report) {
    if (options.self_test && pid != test_host.dwProcessId) {
        return ScanResult::NotFound;
    }
    ScopedHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    if (!process.valid()) {
        const DWORD error = GetLastError();
        report.pid = pid;
        report.fail("watch_candidate_open_failed", error);
        return ScanResult::Failed;
    }
    const ULONGLONG created = process_creation_time(process.get());
    if (created == 0) {
        const DWORD error = GetLastError();
        report.pid = pid;
        report.fail("watch_creation_query_failed", error);
        return ScanResult::Failed;
    }
    debug_log(L"[watch] pid=%lu created=%llu armed=%llu\n", pid, created, report.watch_armed_time);
    // Only a launch that happened after arming is eligible, unless we accept an already
    // running game process (used after a launcher hand-off).
    if (!options.accept_existing && created <= report.watch_armed_time) {
        return ScanResult::NotFound;
    }
    if (options.self_test && created != process_creation_time(test_host.hProcess)) {
        report.fail("watch_test_identity_mismatch", ERROR_INVALID_PARAMETER);
        return ScanResult::Failed;
    }

    target.pid = pid;
    target.creation_time = created;
    report.pid = target.pid;
    report.creation_time = created;
    if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) {
        // A launcher bootstrap can appear and exit before the real game window exists.
        debug_log(L"[watch] candidate pid=%lu already exited\n", pid);
        return ScanResult::NotFound;
    }
    report.stage = "watch_new_process_selected";
    return ScanResult::Selected;
}

ScanResult scan_for_new_process(const std::vector<DWORD>& baseline, Target& target,
                                const WatchOptions& options, const PROCESS_INFORMATION& test_host,
                                LoadReport& report) {
    std::vector<DWORD> ids;
    if (!enumerate_process_ids(ids)) {
        report.fail("watch_snapshot_failed", GetLastError());
        return ScanResult::Failed;
    }
    for (const DWORD pid : ids) {
        if (pid == 0) {
            continue;
        }
        if (std::find(baseline.begin(), baseline.end(), pid) != baseline.end()) {
            continue;
        }
        const std::wstring name = process_image_file_name(pid);
        if (_wcsicmp(name.c_str(), target.image_name) != 0) {
            continue;
        }
        debug_log(L"[watch] candidate pid=%lu name=%ls\n", pid, name.c_str());
        const ScanResult result = examine_pid(pid, target, options, test_host, report);
        if (result != ScanResult::NotFound) {
            return result;
        }
    }
    return ScanResult::NotFound;
}

void announce_armed(const Target& target, const WatchOptions& options) {
    if (options.unbounded) {
        print_status(L"正在等待明日方舟启动（不限时）。请正常启动游戏；关闭本窗口即可退出。");
        return;
    }
    std::printf("Startup watcher armed; only a new %ls process is eligible. Launch the game "
                "yourself.\n",
                target.image_name);
    std::fflush(stdout);
}

} // namespace

bool create_test_host(const std::filesystem::path& root, PROCESS_INFORMATION& child,
                      LoadReport& report) {
    const auto image = root / kTestHostImageName;
    std::wstring command = L"\"" + image.wstring() + L"\" --await-loader --auto-close";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                       root.c_str(), &startup, &child)) {
        return true;
    }
    report.fail("create_test_host", GetLastError());
    return false;
}

bool watch_for_new_process(Target& target, const std::filesystem::path& root,
                           const WatchOptions& options, PROCESS_INFORMATION& test_host,
                           LoadReport& report) {
    std::vector<DWORD> baseline;
    if (!options.accept_existing) {
        if (!capture_baseline(baseline, report)) {
            return false;
        }
    }
    FILETIME armed{};
    GetSystemTimePreciseAsFileTime(&armed);
    report.watch_armed_time = options.accept_existing ? 0 : filetime_to_u64(armed);
    debug_log(L"[watch] armed target=%ls baseline=%zu time=%llu accept_existing=%d\n",
              target.image_name, baseline.size(), report.watch_armed_time,
              options.accept_existing ? 1 : 0);
    announce_armed(target, options);
    if (options.self_test && !create_test_host(root, test_host, report)) {
        return false;
    }

    while (options.unbounded || remaining_ms(options.deadline)) {
        switch (scan_for_new_process(baseline, target, options, test_host, report)) {
        case ScanResult::Selected:
            return true;
        case ScanResult::Failed:
            return false;
        case ScanResult::NotFound:
            break;
        }
        if (options.self_test && WaitForSingleObject(test_host.hProcess, 0) == WAIT_OBJECT_0) {
            report.stage = "watch_test_host_exited_before_selection";
            GetExitCodeProcess(test_host.hProcess, &report.error);
            return false;
        }
        Sleep(options.unbounded ? kPollIntervalMs
                                : std::min<DWORD>(kPollIntervalMs, remaining_ms(options.deadline)));
    }
    debug_log(L"[watch] deadline expired target=%ls\n", target.image_name);
    report.fail("watch_deadline_expired", WAIT_TIMEOUT);
    return false;
}

} // namespace ark::loader
