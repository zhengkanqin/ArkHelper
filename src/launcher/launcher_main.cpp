// ArkHelperLauncher：以管理员权限完成游戏连接与启动，内置运行时加载器。
// --inject <pid> 连接已运行的游戏；--load-runtime 进入底层加载命令。
//
// 由已安装的最高权限后台 agent 调用。普通权限调用会返回错误；不会弹 UAC。
//
// 参数：
//   --loader <ArkHelperLauncher.exe 路径>
//   --game   <Arknights.exe 路径>
//   --report <加载器结果 JSON 路径，可选>
//   --inject <pid> 只连接指定进程；失败直接返回，游戏保持运行。
//
// 退出码：0 成功；2 参数错误；3 游戏路径不存在；4 加载器启动失败；5 游戏启动失败。

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <string>
#include <vector>

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include "processes.h"
#include "agent_protocol.h"

int arkhelp_loader_main(int argc, wchar_t** argv);
int arkhelp_agent_main();
int arkhelp_agent_control(bool install);

namespace
{

std::wstring g_log_path;

void log_line(const wchar_t* format, ...)
{
    if (g_log_path.empty())
    {
        return;
    }
    wchar_t buffer[1024];
    va_list args;
    va_start(args, format);
    std::vswprintf(buffer, sizeof(buffer) / sizeof(buffer[0]), format, args);
    va_end(args);

    const HANDLE file = CreateFileW(g_log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    char line[1400];
    const int length = std::snprintf(line, sizeof(line), "[%04d-%02d-%02d %02d:%02d:%02d] %ls\r\n", now.wYear,
                                     now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, buffer);
    if (length > 0)
    {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
    }
    CloseHandle(file);
}

bool is_elevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, size, &size);
    CloseHandle(token);
    return ok != FALSE && elevation.TokenIsElevated != 0;
}

std::wstring parent_directory(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
    {
        return L".";
    }
    return path.substr(0, slash);
}

std::wstring module_directory()
{
    wchar_t buffer[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return parent_directory(buffer);
}

struct ProcessInfo
{
    DWORD pid = 0;
    std::wstring exe_name;
};

std::wstring file_name(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::vector<ProcessInfo> find_processes(const std::wstring& exe_name)
{
    std::vector<ProcessInfo> result;
    for (const auto pid : arkhelp::launcher::process_ids_named(exe_name.c_str())) {
        result.push_back(ProcessInfo{pid, exe_name});
    }
    return result;
}

bool start_process(const std::wstring& command_line, const std::wstring& working_directory)
{
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    std::wstring mutable_command = command_line;
    if (CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       working_directory.empty() ? nullptr : working_directory.c_str(), &startup, &info) == FALSE)
    {
        log_line(L"CreateProcessW failed: %lu (%ls)", GetLastError(), command_line.c_str());
        return false;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return true;
}

// Wait for the loader child only. A timeout reports failure without terminating any process.
bool run_process_and_wait(const std::wstring& command_line, const std::wstring& working_directory,
                          DWORD timeout_ms, DWORD& exit_code)
{
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    std::wstring mutable_command = command_line;
    if (CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       working_directory.empty() ? nullptr : working_directory.c_str(), &startup, &info) == FALSE)
    {
        log_line(L"CreateProcessW failed: %lu (%ls)", GetLastError(), command_line.c_str());
        return false;
    }
    CloseHandle(info.hThread);
    const DWORD wait = WaitForSingleObject(info.hProcess, timeout_ms);
    if (wait != WAIT_OBJECT_0)
    {
        log_line(L"loader still running after %lu ms; reporting timeout without terminating it", timeout_ms);
        exit_code = ERROR_TIMEOUT;
        CloseHandle(info.hProcess);
        return false;
    }
    exit_code = 1;
    GetExitCodeProcess(info.hProcess, &exit_code);
    CloseHandle(info.hProcess);
    return true;
}

std::wstring quote(const std::wstring& value)
{
    return L"\"" + value + L"\"";
}

}  // namespace

// 与 ArkHelper.exe 同理：Zig 自带 MinGW CRT 的 GUI 入口固定引用 ANSI WinMain，
// 参数用 GetCommandLineW + CommandLineToArgvW 自己解析，避免依赖宽字符入口。
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    g_log_path = module_directory() + L"\\logs\\launcher.log";
    CreateDirectoryW((module_directory() + L"\\logs").c_str(), nullptr);

    std::wstring loader_path;
    std::wstring game_path;
    std::wstring report_path;
    DWORD inject_pid = 0;
    bool dry_run = false;
    bool require_app = false;
    arkhelp::launcher::RequestIdentity request_identity{};

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr)
    {
        return 2;
    }
    if (argc == 2 && std::wcscmp(argv[1], L"--agent") == 0) {
        LocalFree(argv);
        return arkhelp_agent_main();
    }
    if (argc == 2 && (std::wcscmp(argv[1], L"--install-agent") == 0 ||
                      std::wcscmp(argv[1], L"--uninstall-agent") == 0)) {
        const bool install = std::wcscmp(argv[1], L"--install-agent") == 0;
        LocalFree(argv);
        return arkhelp_agent_control(install);
    }
    if (argc > 1 && std::wcscmp(argv[1], L"--load-runtime") == 0)
    {
        std::vector<wchar_t*> loader_args{argv[0]};
        for (int index = 2; index < argc; ++index) {
            loader_args.push_back(argv[index]);
        }
        const int result = arkhelp_loader_main(static_cast<int>(loader_args.size()), loader_args.data());
        LocalFree(argv);
        return result;
    }
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring key = argv[i];
        const bool has_value = (i + 1) < argc;
        if (key == L"--loader" && has_value)
        {
            loader_path = argv[++i];
        }
        else if (key == L"--game" && has_value)
        {
            game_path = argv[++i];
        }
        else if (key == L"--report" && has_value)
        {
            report_path = argv[++i];
        }
        else if (key == L"--inject" && has_value)
        {
            inject_pid = static_cast<DWORD>(std::wcstoul(argv[++i], nullptr, 10));
        }
        else if (key == L"--require-app")
        {
            require_app = true;
        }
        else if (key == L"--request-owner" && has_value)
        {
            request_identity.owner = static_cast<LONG>(std::wcstol(argv[++i], nullptr, 10));
        }
        else if (key == L"--request-id" && has_value)
        {
            request_identity.id = static_cast<LONG>(std::wcstol(argv[++i], nullptr, 10));
        }
        else if (key == L"--agent-epoch" && has_value)
        {
            request_identity.epoch = static_cast<LONG64>(std::wcstoll(argv[++i], nullptr, 10));
        }
        else if (key == L"--dry-run")
        {
            dry_run = true;
        }
    }
    LocalFree(argv);

    log_line(L"launcher starting elevated=%d loader=%ls game=%ls inject=%lu", is_elevated() ? 1 : 0,
             loader_path.c_str(), game_path.c_str(), inject_pid);
    if (loader_path.empty())
    {
        loader_path = module_directory() + L"\\ArkHelperLauncher.exe";
    }
    if (game_path.empty() && inject_pid == 0)
    {
        log_line(L"missing --loader/--game");
        return 2;
    }
    if (!game_path.empty() && GetFileAttributesW(game_path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        log_line(L"game executable not found: %ls", game_path.c_str());
        return 3;
    }
    if (GetFileAttributesW(loader_path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        log_line(L"loader executable not found: %ls", loader_path.c_str());
        return 4;
    }

    if (dry_run)
    {
        // 自检用：只校验参数/路径与计划动作，不关游戏、不提权、不启动任何进程。
        log_line(L"dry run ok: connect an existing game, otherwise arm %ls and start %ls; never close a game",
                 loader_path.c_str(), game_path.c_str());
        return 0;
    }

    if (arkhelp::launcher::game_launch_disabled() && inject_pid == 0) {
        log_line(L"game launch disabled by ARKHELP_NO_GAME_LAUNCH");
        return ERROR_CANCELLED;
    }
    if (!is_elevated()) {
        log_line(L"elevated agent required; no UAC requested");
        return ERROR_ELEVATION_REQUIRED;
    }
    const std::wstring target_exe_name = file_name(game_path);
    // A request can race with the user starting the game. Always connect to an existing
    // process first, including when the caller requested an ordinary launch.
    if (inject_pid == 0) {
        const auto running = find_processes(target_exe_name);
        if (!running.empty()) { inject_pid = running.front().pid; }
    }

    // A failed connection is reported to the caller. It never closes or relaunches the game.
    if (inject_pid != 0)
    {
        {
            arkhelp::launcher::AppLaunchGuard approval(require_app, request_identity);
            if (!approval.allowed) { log_line(L"connection canceled: requesting App is gone or request changed"); return ERROR_CANCELLED; }
        }
        if (!report_path.empty())
        {
            DeleteFileW(report_path.c_str());
        }
        // Connect without changing window focus or requiring its message loop.
        // The loader validates system DLL paths through Toolhelp and PSAPI, and
        // can restart our already mapped core after a desktop UI update.
        std::wstring inject_command = quote(loader_path) + L" --load-runtime --pid " + std::to_wstring(inject_pid) +
                                      L" --method remote-thread --timeout-seconds 20";
        if (!report_path.empty())
        {
            inject_command += L" --report " + quote(report_path);
        }
        DWORD inject_exit = 1;
        log_line(L"injecting into running process %lu: %ls", inject_pid, inject_command.c_str());
        if (run_process_and_wait(inject_command, parent_directory(loader_path), 90000, inject_exit) &&
            inject_exit == 0)
        {
            log_line(L"inject succeeded into %lu", inject_pid);
            return 0;
        }
        log_line(L"inject failed (exit=%lu); leaving game %lu untouched", inject_exit, inject_pid);
        return 6;
    }

    if (!report_path.empty())
    {
        DeleteFileW(report_path.c_str());
    }

    std::wstring loader_command = quote(loader_path) + L" --load-runtime --watch --timeout-seconds 240";
    if (!report_path.empty())
    {
        loader_command += L" --report " + quote(report_path);
    }
    {
        arkhelp::launcher::AppLaunchGuard approval(require_app, request_identity);
        if (!approval.allowed) { log_line(L"launch canceled before arming: App is gone or request changed"); return ERROR_CANCELLED; }
        if (!start_process(loader_command, parent_directory(loader_path))) { return 4; }
    }
    log_line(L"loader armed: %ls", loader_command.c_str());
    Sleep(1500);

    // Serialize the final game check/create with other workers. The App approval remains
    // locked through CreateProcess, so an agent restart/reclaimed intent cannot authorize
    // two old/new workers to create the same client concurrently.
    HANDLE start_mutex = CreateMutexW(nullptr, FALSE, arkhelp::launcher::kGameStartMutex);
    int result = ERROR_BUSY;
    {
        arkhelp::launcher::MailboxLock start_lock(start_mutex, 5000);
        if (start_lock.acquired) {
            arkhelp::launcher::AppLaunchGuard approval(require_app, request_identity);
            if (!approval.allowed || arkhelp::launcher::game_launch_disabled()) {
                log_line(L"launch canceled before game creation: App is gone or request changed");
                result = ERROR_CANCELLED;
            } else if (!find_processes(target_exe_name).empty()) {
                log_line(L"game appeared while watcher was arming; keeping that process");
                result = 0;
            } else {
                // Process enumeration can take time; check the owner/stop signal once more
                // immediately before CreateProcess while retaining the same serialized approval.
                arkhelp::launcher::AppLaunchGuard final_approval(require_app, request_identity);
                if (!final_approval.allowed) { result = ERROR_CANCELLED; }
                else if (!start_process(quote(game_path), parent_directory(game_path))) { result = 5; }
                else { log_line(L"game started: %ls", game_path.c_str()); result = 0; }
            }
        }
    }
    if (start_mutex) { CloseHandle(start_mutex); }
    return result;
}
