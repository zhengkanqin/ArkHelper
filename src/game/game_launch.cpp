#include "game_launch.h"

#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "../shell/platform.h"
#include "../launcher/agent_protocol.h"

#include "../log/log.h"
#include "../shell/win_util.h"

namespace arkhelp
{
namespace game_launch
{
namespace
{

Paths g_paths;
std::wstring g_last_error;
bool g_busy = false;
std::wstring g_root_directory;
bool g_watch_active = false;
HANDLE g_watch_process = nullptr;
HANDLE g_request_mapping = nullptr;
HANDLE g_request_event = nullptr;
HANDLE g_request_mutex = nullptr;
launcher::LaunchRequest* g_request = nullptr;
LONG g_request_id = 0;
LONG g_next_request_id = 0;
LONG64 g_request_epoch = 0;
ULONGLONG g_request_started = 0;

void close_request()
{
    if (g_request) { UnmapViewOfFile(g_request); g_request = nullptr; }
    if (g_request_mapping) { CloseHandle(g_request_mapping); g_request_mapping = nullptr; }
    if (g_request_event) { CloseHandle(g_request_event); g_request_event = nullptr; }
    if (g_request_mutex) { CloseHandle(g_request_mutex); g_request_mutex = nullptr; }
}

bool agent_ready()
{
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, launcher::kLaunchEvent);
    if (!event) { return false; }
    CloseHandle(event);
    return true;
}

bool submit_request()
{
    g_request_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, launcher::kLaunchEvent);
    if (!g_request_event) { return false; }
    g_request_mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, launcher::kLaunchMapping);
    if (g_request_mapping) {
        g_request = static_cast<launcher::LaunchRequest*>(MapViewOfFile(g_request_mapping,
            FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(launcher::LaunchRequest)));
    }
    if (!g_request || g_request->magic != launcher::kLaunchMagic) { close_request(); return false; }
    g_request_mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, launcher::kRequestMutex);
    bool published = false;
    {
        launcher::MailboxLock lock(g_request_mutex, 0);
        if (lock.acquired) {
            const LONG state = g_request->state;
            const bool orphan = !launcher::process_alive(static_cast<DWORD>(g_request->owner_pid));
            if (state == launcher::kIdle || state == launcher::kDone || orphan ||
                (state == launcher::kWriting && lock.abandoned)) {
                // The lock makes state and ownership one claim, including when a writer crashes.
                g_request->owner_pid = static_cast<LONG>(GetCurrentProcessId());
                InterlockedExchange(&g_request->state, launcher::kWriting);
                std::wmemcpy(g_request->game_path, g_paths.game_exe.c_str(), g_paths.game_exe.size() + 1);
                g_request->request_id = g_request_id;
                g_request_epoch = InterlockedCompareExchange64(&g_request->agent_epoch, 0, 0);
                InterlockedExchange(&g_request->result, 0);
                MemoryBarrier();
                InterlockedExchange(&g_request->state, launcher::kPending);
                published = SetEvent(g_request_event) != FALSE;
                if (!published) { InterlockedExchange(&g_request->state, launcher::kIdle); }
            }
        }
    }
    if (!published) { close_request(); return false; }
    log_info("game launch submitted to elevated agent; request=%ld", g_request_id);
    return true;
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

bool file_exists(const std::wstring& path)
{
    if (path.empty())
    {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring join(const std::wstring& directory, const wchar_t* name)
{
    if (directory.empty())
    {
        return name;
    }
    const wchar_t last = directory.back();
    if (last == L'\\' || last == L'/')
    {
        return directory + name;
    }
    return directory + L"\\" + name;
}

// 从已在运行的游戏窗口反查可执行文件路径（游戏普通权限启动时可用；
// 高完整性启动会失败，此时依赖配置里保存的路径）。
std::wstring detect_running_game_path()
{
    struct Search
    {
        std::wstring path;
    } search;

    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL {
            auto* out = reinterpret_cast<Search*>(param);
            if (out == nullptr || !IsWindowVisible(window))
            {
                return TRUE;
            }
            wchar_t class_name[64] = {};
            GetClassNameW(window, class_name, static_cast<int>(std::size(class_name)));
            if (std::wcscmp(class_name, L"UnityWndClass") != 0)
            {
                return TRUE;
            }
            DWORD process_id = 0;
            GetWindowThreadProcessId(window, &process_id);
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
            if (process == nullptr)
            {
                return TRUE;
            }
            wchar_t path[1024] = {};
            DWORD length = static_cast<DWORD>(std::size(path));
            const BOOL queried = QueryFullProcessImageNameW(process, 0, path, &length);
            CloseHandle(process);
            if (queried)
            {
                const wchar_t* name = std::wcsrchr(path, L'\\');
                if (name != nullptr &&
                    (std::wcsstr(name, L"Arknights") != nullptr || std::wcsstr(name, L"arknights") != nullptr))
                {
                    out->path = path;
                    return FALSE;
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));

    return search.path;
}

void resolve_paths()
{
    const std::wstring exe_directory = module_directory();
    g_paths.launcher_exe = join(exe_directory, L"ArkHelperLauncher.exe");
    g_paths.report = join(g_root_directory, L"logs\\launch-report.json");

    // 所有加载能力都来自同目录的 ArkHelperLauncher.exe。
    g_paths.loader_exe.clear();
    const std::wstring loader_candidates[] = {
        join(exe_directory, L"ArkHelperLauncher.exe"),
    };
    for (const auto& candidate : loader_candidates)
    {
        if (file_exists(candidate))
        {
            g_paths.loader_exe = candidate;
            break;
        }
    }

    const std::wstring game_candidates[] = {
        g_paths.game_exe,
        detect_running_game_path(),
        L"C:\\Program Files\\Hypergryph\\games\\Arknights\\Arknights.exe",
    };
    for (const auto& candidate : game_candidates)
    {
        if (file_exists(candidate))
        {
            g_paths.game_exe = candidate;
            break;
        }
    }
}

std::wstring quote(const std::wstring& value)
{
    return L"\"" + value + L"\"";
}

}  // namespace

void initialize(const std::wstring& root_directory, const Config& config)
{
    g_root_directory = root_directory;
    g_paths.game_exe = config.game_path;
    g_paths.loader_exe = config.loader_path;
    resolve_paths();
    log_info("game launch paths: game=%s loader=%s launcher=%s", wide_to_utf8(g_paths.game_exe).c_str(),
             wide_to_utf8(g_paths.loader_exe).c_str(), wide_to_utf8(g_paths.launcher_exe).c_str());
}

const Paths& paths()
{
    return g_paths;
}

bool paths_ready()
{
    return file_exists(g_paths.game_exe) && file_exists(g_paths.loader_exe) && file_exists(g_paths.launcher_exe);
}

bool busy()
{
    return g_busy;
}

const std::wstring& last_error()
{
    return g_last_error;
}

bool request_launch()
{
    if (g_busy) { return false; }
    g_last_error.clear();
    if (launcher::game_launch_disabled()) {
        g_last_error = L"测试环境已禁用游戏启动（ARKHELP_NO_GAME_LAUNCH）。";
        log_info("game launch suppressed by ARKHELP_NO_GAME_LAUNCH");
        return false;
    }
    if (!paths_ready() || g_paths.game_exe.size() >= launcher::kGamePathCapacity) {
        g_last_error = L"请检查 config/arkhelp.json 的 game_path，并保持程序文件夹完整。";
        return false;
    }
    if (!agent_ready() && !watch_agent_present()) {
        g_last_error = L"已安装的后台连接组件不可用；未请求提权，也未启动或关闭游戏。";
        return false;
    }
    if (!agent_ready() && !arm_silent_watch()) { return false; }
    close_request();
    g_busy = true;
    g_request_started = GetTickCount64();
    g_request_id = InterlockedIncrement(&g_next_request_id);
    submit_request(); // The installed task can take a moment to create its command channel.
    return true;
}

bool launch_or_restart()
{
    return request_launch();
}

void poll()
{
    if (g_watch_process && WaitForSingleObject(g_watch_process, 0) == WAIT_OBJECT_0) {
        DWORD code = 1;
        GetExitCodeProcess(g_watch_process, &code);
        CloseHandle(g_watch_process);
        g_watch_process = nullptr;
        if (code != 0) {
            g_last_error = L"已安装的后台任务启动失败（exit=" + std::to_wstring(code) + L"）。";
            log_error("installed agent task failed: %lu", code);
            if (g_busy && !g_request) { g_busy = false; }
        }
    }
    g_watch_active = agent_ready();
    if (!g_busy) { return; }
    if (!g_request && !submit_request()) {
        if (GetTickCount64() - g_request_started >= 15000) {
            g_last_error = L"后台连接组件未响应启动请求。请查看 runtime/logs/agent.log；游戏保持原状。";
            g_busy = false;
            log_error("agent launch channel unavailable after 15 seconds");
        }
        return;
    }
    // A restarted agent resets its mailbox. Keep the same desktop intent/ID and publish it
    // again; launch workers validate the epoch under the mailbox lock before creating a game.
    if (InterlockedCompareExchange64(&g_request->agent_epoch, 0, 0) != g_request_epoch ||
        g_request->state == launcher::kIdle || g_request->request_id != g_request_id ||
        g_request->owner_pid != static_cast<LONG>(GetCurrentProcessId())) {
        close_request();
        submit_request();
        return;
    }
    if (InterlockedCompareExchange(&g_request->state, 0, 0) == launcher::kDone &&
        InterlockedCompareExchange(&g_request->request_id, 0, 0) == g_request_id) {
        const LONG code = InterlockedCompareExchange(&g_request->result, 0, 0);
        if (code != 0) {
            g_last_error = L"后台启动或连接失败（错误代码 " + std::to_wstring(code) +
                L"）。游戏保持原状，请查看 runtime/logs/launch-report.json。";
            log_error("agent launch request failed: %ld", code);
        } else {
            g_last_error.clear();
            log_info("agent launch request completed; waiting for runtime mapping");
        }
        close_request();
        g_busy = false;
    } else if (GetTickCount64() - g_request_started >= 120000) {
        g_last_error = L"后台启动或连接超时；未关闭游戏，请查看 runtime/logs/agent.log。";
        close_request();
        g_busy = false;
    }
}

bool arm_silent_watch()
{
    if (agent_ready()) { g_watch_active = true; return true; }
    if (g_watch_process) { return true; }
    if (!watch_agent_present()) {
        g_last_error = L"未检测到已安装的后台连接任务；不会自动提权或安装。";
        return false;
    }
    wchar_t system_directory[MAX_PATH] = {};
    if (!GetSystemDirectoryW(system_directory, MAX_PATH)) {
        g_last_error = L"无法定位 Windows 任务计划程序。";
        return false;
    }
    std::wstring command = quote(std::wstring(system_directory) + L"\\schtasks.exe") +
        L" /Run /TN \"ArkHelper Watch Agent\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
        g_last_error = L"无法请求已安装的后台连接任务（错误代码 " + std::to_wstring(GetLastError()) + L"）。";
        return false;
    }
    CloseHandle(process.hThread);
    g_watch_process = process.hProcess;
    log_info("installed agent task requested without elevation");
    return true;
}

bool silent_watch_active()
{
    return g_watch_active || g_watch_process != nullptr;
}

bool watch_agent_present()
{
    return file_exists(join(module_directory(), L"runtime\\watch-agent.installed"));
}

}  // namespace game_launch
}  // namespace arkhelp
