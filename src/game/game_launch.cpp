#include "game_launch.h"

#include <cstring>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../shell/platform.h"
#include "../launcher/agent_protocol.h"
#include "../launcher/processes.h"

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
ULONGLONG g_report_checked = 0;
std::filesystem::file_time_type g_report_written{};

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

std::wstring registry_text(HKEY key, const wchar_t* name)
{
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes > 65536) return {};
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, nullptr, reinterpret_cast<BYTE*>(buffer.data()), &bytes) != ERROR_SUCCESS)
        return {};
    if (type != REG_EXPAND_SZ) return buffer.data();
    std::vector<wchar_t> expanded(32768);
    const DWORD length = ExpandEnvironmentStringsW(buffer.data(), expanded.data(), static_cast<DWORD>(expanded.size()));
    return length && length <= expanded.size() ? expanded.data() : std::wstring{};
}

std::wstring executable_in_command(std::wstring text)
{
    const auto first = text.find_first_not_of(L" \t");
    if (first == std::wstring::npos) return {};
    text.erase(0, first);
    if (text.front() == L'"') {
        const auto end = text.find(L'"', 1);
        return end == std::wstring::npos ? std::wstring{} : text.substr(1, end - 1);
    }
    std::wstring lower = text;
    for (auto& c : lower) c = std::towlower(c);
    const auto end = lower.find(L".exe");
    return end == std::wstring::npos ? std::wstring{} : text.substr(0, end + 4);
}

std::wstring game_under_install(const std::wstring& install)
{
    if (install.empty()) return {};
    for (const auto* suffix : {L"Arknights.exe", L"games\\Arknights\\Arknights.exe", L"Arknights\\Arknights.exe"}) {
        const auto candidate = join(install, suffix);
        if (file_exists(candidate)) return candidate;
    }
    return {};
}

std::wstring detect_installed_game_path()
{
    for (HKEY hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
            HKEY uninstall = nullptr;
            if (RegOpenKeyExW(hive, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0,
                    KEY_READ | view, &uninstall) != ERROR_SUCCESS) continue;
            std::wstring found;
            for (DWORD index = 0;; ++index) {
                wchar_t name[256]{};
                DWORD length = 256;
                const LONG result = RegEnumKeyExW(uninstall, index, name, &length, nullptr, nullptr, nullptr, nullptr);
                if (result == ERROR_NO_MORE_ITEMS) break;
                if (result != ERROR_SUCCESS) continue;
                HKEY product = nullptr;
                if (RegOpenKeyExW(uninstall, name, 0, KEY_READ | view, &product) != ERROR_SUCCESS) continue;
                auto display = registry_text(product, L"DisplayName");
                for (auto& c : display) c = std::towlower(c);
                if (display.find(L"鹰角") != display.npos || display.find(L"明日方舟") != display.npos ||
                    display.find(L"hypergryph") != display.npos || display.find(L"arknights") != display.npos) {
                    for (const auto* field : {L"InstallLocation", L"DisplayIcon", L"UninstallString"}) {
                        auto location = registry_text(product, field);
                        if (std::wcscmp(field, L"InstallLocation") != 0) {
                            const auto executable = executable_in_command(location);
                            location = executable.empty() ? L"" : parent_directory(executable);
                        }
                        found = game_under_install(location);
                        if (!found.empty()) break;
                    }
                }
                RegCloseKey(product);
                if (!found.empty()) break;
            }
            RegCloseKey(uninstall);
            if (!found.empty()) return found;
        }
    }
    return {};
}

void resolve_paths()
{
    const std::wstring exe_directory = module_directory();
    g_paths.launcher_exe = join(exe_directory, L"ArkHelperLauncher.exe");
    g_paths.report = join(g_root_directory, L"runtime\\logs\\launch-report.json");

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
        detect_installed_game_path(),
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
    if (!file_exists(g_paths.game_exe)) g_last_error = L"未找到游戏，请在设置中选择 Arknights.exe";
    else if (g_last_error == L"未找到游戏，请在设置中选择 Arknights.exe") g_last_error.clear();
}

std::wstring failure_from_report(const std::string& text)
{
    if (text.find("\"success\":false") == text.npos) return {};
    if (text.find("entry_did_not_complete") != text.npos)
        return L"采集入口未能运行；请退出游戏后由助手启动，再重试连接";
    if (text.find("window_hook_timeout") != text.npos)
        return L"游戏窗口未响应连接；请退出游戏后由助手启动";
    if (text.find("core_missing") != text.npos)
        return L"采集组件缺失，请完整解压程序包";
    return L"采集连接失败，请点击重试；详情见 runtime/logs/launch-report.json";
}

void poll_connection_report()
{
    if (launcher::game_launch_disabled() || GetTickCount64() - g_report_checked < 1000) return;
    g_report_checked = GetTickCount64();
    std::error_code error;
    const auto modified = std::filesystem::last_write_time(g_paths.report, error);
    if (error || modified == g_report_written) return;
    std::ifstream file(std::filesystem::path(g_paths.report), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)), {});
    const auto pid_key = text.find("\"pid\":");
    if (pid_key == text.npos) return;
    const auto pid = static_cast<DWORD>(std::strtoul(text.c_str() + pid_key + 6, nullptr, 10));
    const auto running = launcher::process_ids_named(L"Arknights.exe");
    if (std::find(running.begin(), running.end(), pid) == running.end()) return;
    if (text.find("\"success\":true") != text.npos) {
        g_last_error.clear();
        g_report_written = modified;
    } else if (text.find("\"success\":false") != text.npos) {
        g_last_error = failure_from_report(text);
        g_report_written = modified;
    }
}

std::wstring quote(const std::wstring& value)
{
    return L"\"" + value + L"\"";
}

}  // namespace

void initialize(const std::wstring& root_directory, const Config& config)
{
    g_last_error.clear();
    g_report_checked = 0;
    g_report_written = {};
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
    if (!file_exists(g_paths.game_exe)) resolve_paths();
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
        g_last_error = L"请在设置中选择 Arknights.exe，并保持程序文件夹完整";
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
    if (!g_busy) { poll_connection_report(); return; }
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
            std::ifstream file(std::filesystem::path(g_paths.report), std::ios::binary);
            g_last_error = failure_from_report(std::string((std::istreambuf_iterator<char>(file)), {}));
            if (g_last_error.empty()) g_last_error = L"后台启动或连接失败（" + std::to_wstring(code) + L"），请点击重试";
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
