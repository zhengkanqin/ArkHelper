#include "interactive_session.h"

#include <windows.h>

#include "console.h"
#include "elevation.h"
#include "runtime_loader.h"
#include "log_tail.h"
#include "process_watch.h"
#include "scoped_handle.h"

namespace ark::loader {

namespace {

constexpr ULONGLONG kInteractiveLoadTimeoutMs = 120000;
constexpr DWORD kLogPumpIntervalMs = 250;

bool runtime_files_present(const std::filesystem::path& root) {
    for (const wchar_t* name : {L"ArkHelperRuntime.dll", L"ArkHelperBootstrap.dll"}) {
        if (GetFileAttributesW((root / name).c_str()) == INVALID_FILE_ATTRIBUTES) {
            print_status(L"缺少 %ls：请保持加载器所在文件夹完整。", name);
            return false;
        }
    }
    return true;
}

// Waits for one new game launch and loads it. Returns false on a watcher error that ends
// the session; a failed load only skips that game process.
bool serve_next_game(const std::filesystem::path& root) {
    if (is_game_running()) {
        print_status(L"明日方舟已经在运行：请完全退出游戏后重新启动，加载器只在游戏启动时载入。");
    }
    Target target{};
    LoadReport report;
    PROCESS_INFORMATION unused_test_host{};
    WatchOptions options;
    options.unbounded = true;
    if (!watch_for_new_process(target, root, options, unused_test_host, report)) {
        print_status(L"等待游戏时出错：%ls（Windows 错误 %lu）。",
                     utf8_to_wide(report.stage).c_str(), report.error);
        return false;
    }
    print_status(L"检测到游戏启动（PID %lu），正在加载……", target.pid);

    // Both are taken before loading so no early log line or exit is missed.
    LogTail log(root / L"logs" / L"ArkHelperRuntime.log", target.pid);
    ScopedHandle process(OpenProcess(SYNCHRONIZE, FALSE, target.pid));
    report = load_runtime(target, root, LoadMethod::RemoteThread,
                            GetTickCount64() + kInteractiveLoadTimeoutMs);
    log.pump();
    if (!report.success) {
        print_status(L"加载失败：%ls（Windows 错误 %lu）。不会对同一个游戏进程重试。",
                     utf8_to_wide(report.stage).c_str(), report.error);
        return true;
    }
    print_status(L"加载完成。请在 ArkHelper 悬浮菜单中管理显示；关闭本窗口不会影响游戏。");
    while (process.valid() &&
           WaitForSingleObject(process.get(), kLogPumpIntervalMs) == WAIT_TIMEOUT) {
        log.pump();
    }
    log.pump();
    print_status(L"游戏已退出。");
    return true;
}

} // namespace

int run_interactive_session(const std::filesystem::path& root) {
    SetConsoleTitleW(L"ArkHelperRuntime 加载器");
    if (!is_elevated()) {
        print_status(L"游戏以管理员权限运行，加载器需要同样的权限，正在请求……");
        DWORD error = ERROR_SUCCESS;
        if (relaunch_elevated(error)) {
            return 0;
        }
        print_status(L"未获得管理员权限（Windows 错误 %lu），无法加载。", error);
        wait_for_key_press();
        return 1;
    }
    if (!runtime_files_present(root)) {
        wait_for_key_press();
        return 1;
    }
    print_status(L"ArkHelperRuntime 目录：%ls", root.c_str());
    print_status(L"插件、日志、缓存和导出都保存在这个目录，不会写入游戏目录。");
    for (;;) {
        if (!serve_next_game(root)) {
            wait_for_key_press();
            return 1;
        }
    }
}

} // namespace ark::loader
