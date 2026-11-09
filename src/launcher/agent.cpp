// Optional elevated watch agent. Installed once; all subsequent launches are silent.
#include <windows.h>
#include <shellapi.h>
#include "processes.h"
#include "agent_protocol.h"
#include "mouse_relay_server.h"
#include <sddl.h>
#include <cwchar>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
constexpr wchar_t kTask[] = L"ArkHelper Watch Agent";
constexpr wchar_t kStop[] = L"Local\\ArkHelp.Agent.Stop";
std::wstring executable() {
    wchar_t path[32768];
    const DWORD n = GetModuleFileNameW(nullptr, path, 32768);
    return n && n < 32768 ? std::wstring(path, n) : std::wstring{};
}
std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\"";
    unsigned slashes = 0;
    for (const wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
DWORD run(std::wstring command, const std::filesystem::path& directory) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, directory.c_str(), &startup, &process)) { return GetLastError(); }
    const DWORD waited = WaitForSingleObject(process.hProcess, 300000);
    DWORD result = 1;
    if (waited == WAIT_OBJECT_0) { GetExitCodeProcess(process.hProcess, &result); }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}
std::wstring scheduler() {
    wchar_t path[MAX_PATH];
    const UINT n = GetSystemDirectoryW(path, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(path, n) + L"\\schtasks.exe" : L"schtasks.exe";
}
void log(const std::filesystem::path& root, const std::string& message) {
    std::ofstream file(root / L"runtime/logs/agent.log", std::ios::app);
    file << GetTickCount64() << ' ' << message << '\n';
}
bool heartbeat_alive(const std::filesystem::path& root, DWORD pid) {
    const auto path = root / L"runtime/logs/runtime-heartbeat.txt";
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) { return false; }
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER current{}, written{};
    current.LowPart = now.dwLowDateTime; current.HighPart = now.dwHighDateTime;
    written.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
    written.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
    if (current.QuadPart < written.QuadPart || current.QuadPart - written.QuadPart > 200000000ULL) { return false; }
    std::ifstream file(path);
    std::string line;
    std::getline(file, line);
    if (!line.empty() && line.back() == '\r') { line.pop_back(); }
    return line == "pid=" + std::to_string(pid);
}
HANDLE start_worker(std::wstring command, const std::filesystem::path& directory, DWORD& error) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, directory.c_str(), &startup, &child)) {
        error = GetLastError();
        return nullptr;
    }
    CloseHandle(child.hThread);
    error = 0;
    return child.hProcess;
}
struct RunningGame { DWORD pid; std::wstring path; };
std::vector<RunningGame> running_games() {
    std::vector<RunningGame> result;
    for (const DWORD pid : arkhelp::launcher::process_ids_named(L"Arknights.exe")) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        wchar_t path[32768] = {};
        DWORD length = 32768;
        const bool found = process && QueryFullProcessImageNameW(process, 0, path, &length);
        if (process) { CloseHandle(process); }
        result.push_back({pid, found ? std::wstring(path, length) : std::wstring{}});
    }
    return result;
}
// Only this user, administrators and SYSTEM can signal the command channel. Its explicit
// medium integrity label lets the ordinary desktop app reuse the installed high privilege agent.
PSECURITY_DESCRIPTOR command_security() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) { return nullptr; }
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> storage(bytes);
    const bool found = bytes && GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes);
    CloseHandle(token);
    if (!found) { return nullptr; }
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sid)) { return nullptr; }
    std::wstring sddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + std::wstring(sid) + L")";
    // An offline probe may run below medium integrity and cannot raise an object label.
    // It cannot launch or connect to games. The installed production agent keeps the explicit
    // medium label so the ordinary desktop app can use its command channel.
    if (!arkhelp::launcher::game_launch_disabled()) sddl += L"S:(ML;;NW;;;ME)";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                              &descriptor, nullptr)) { return nullptr; }
    return descriptor;
}
bool elevated_token() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) { return false; }
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const bool result = GetTokenInformation(token, TokenElevation, &elevation, size, &size) && elevation.TokenIsElevated;
    CloseHandle(token);
    return result;
}
bool wait_agent_exit(DWORD timeout) {
    const auto start = GetTickCount64();
    do {
        HANDLE agent = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\ArkHelp.Agent");
        if (!agent) { return GetLastError() == ERROR_FILE_NOT_FOUND; }
        CloseHandle(agent);
        Sleep(50);
    } while (GetTickCount64() - start < timeout);
    return false;
}
void finish_request(arkhelp::launcher::LaunchRequest* request, HANDLE mutex,
                    const arkhelp::launcher::RequestIdentity& identity, DWORD result) {
    arkhelp::launcher::MailboxLock lock(mutex);
    if (!lock.acquired || !arkhelp::launcher::same_request(*request, identity) ||
        request->state != arkhelp::launcher::kRunning) { return; }
    InterlockedExchange(&request->result, static_cast<LONG>(result));
    MemoryBarrier();
    InterlockedExchange(&request->state, arkhelp::launcher::kDone);
}

}

int arkhelp_agent_control(bool install) {
    const auto self = executable();
    const auto root = std::filesystem::path(self).parent_path();
    HANDLE token = nullptr;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        GetTokenInformation(token, TokenElevation, &elevation, size, &size);
        CloseHandle(token);
    }
    if (!elevation.TokenIsElevated) {
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        info.lpVerb = L"runas";
        info.lpFile = self.c_str();
        info.lpParameters = install ? L"--install-agent" : L"--uninstall-agent";
        info.nShow = SW_HIDE;
        if (!ShellExecuteExW(&info)) { return 2; }
        WaitForSingleObject(info.hProcess, INFINITE);
        DWORD result = 1;
        GetExitCodeProcess(info.hProcess, &result);
        CloseHandle(info.hProcess);
        return static_cast<int>(result);
    }
    std::filesystem::create_directories(root / L"runtime/logs");
    const auto task = quote(scheduler());
    HANDLE stop = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStop);
    if (stop) { SetEvent(stop); CloseHandle(stop); }
    if (install && !wait_agent_exit(15000)) {
        log(root, "previous agent did not release its singleton; task was not started");
        return ERROR_TIMEOUT;
    }
    if (!install) {
        run(task + L" /End /TN " + quote(kTask), root);
        const DWORD result = run(task + L" /Delete /F /TN " + quote(kTask), root);
        if (result == 0) {
            DeleteFileW((root / L"runtime/watch-agent.installed").c_str());
        }
        return static_cast<int>(result);
    }
    const auto action = quote(self) + L" --agent";
    const DWORD result = run(task + L" /Create /F /TN " + quote(kTask) +
        L" /SC ONLOGON /RL HIGHEST /TR " + quote(action), root);
    if (result != 0) { log(root, "task registration failed: " + std::to_string(result)); return 3; }
    std::ofstream(root / L"runtime/watch-agent.installed") << "installed\n";
    return static_cast<int>(run(task + L" /Run /TN " + quote(kTask), root));
}

int arkhelp_agent_main() {
    using namespace arkhelp::launcher;
    if (!game_launch_disabled() && !elevated_token()) { return ERROR_ELEVATION_REQUIRED; }
    HANDLE singleton = CreateMutexW(nullptr, FALSE, L"Local\\ArkHelp.Agent");
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (singleton) { CloseHandle(singleton); }
        return 0;
    }
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, kStop);
    if (!stop) { CloseHandle(singleton); return 1; }
    ResetEvent(stop);
    const auto self = executable();
    const auto root = std::filesystem::path(self).parent_path();
    std::filesystem::create_directories(root / L"runtime/logs");
    auto* descriptor = command_security();
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    MouseRelayServer mouse_relay;
    if (!game_launch_disabled() && descriptor && !mouse_relay.start(&security))
        log(root,"mouse observation relay unavailable: "+std::to_string(GetLastError()));
    HANDLE request_mutex = descriptor ? CreateMutexW(&security, FALSE, kRequestMutex) : nullptr;
    HANDLE mapping = request_mutex ? CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE,
        0, sizeof(LaunchRequest), kLaunchMapping) : nullptr;
    auto* request = mapping ? static_cast<LaunchRequest*>(MapViewOfFile(mapping,
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(LaunchRequest))) : nullptr;
    if (request) {
        MailboxLock lock(request_mutex);
        if (!lock.acquired) {
            UnmapViewOfFile(request);
            request = nullptr;
        } else {
            LARGE_INTEGER epoch{};
            QueryPerformanceCounter(&epoch);
            request->magic = kLaunchMagic;
            request->owner_pid = 0;
            request->agent_pid = static_cast<LONG>(GetCurrentProcessId());
            InterlockedExchange64(&request->agent_epoch, epoch.QuadPart ? epoch.QuadPart : 1);
            request->request_id = 0;
            request->result = 0;
            request->state = kIdle;
        }
    }
    HANDLE launch = request ? CreateEventW(&security, FALSE, FALSE, kLaunchEvent) : nullptr;
    if (descriptor) { LocalFree(descriptor); }
    if (!launch) {
        log(root, "agent launch channel unavailable: " + std::to_string(GetLastError()));
        if (request) { UnmapViewOfFile(request); }
        if (mapping) { CloseHandle(mapping); }
        if (request_mutex) { CloseHandle(request_mutex); }
        CloseHandle(stop); CloseHandle(singleton);
        return 1;
    }
    log(root, "native agent started; launch channel ready");
    HANDLE worker = nullptr;
    DWORD worker_pid = 0, last_pid = 0;
    bool worker_request = false;
    RequestIdentity worker_identity{};
    ULONGLONG last_attempt = 0, last_scan = 0, launch_grace_until = 0;
    const HANDLE events[] = {stop, launch};
    for (;;) {
        const DWORD waited = WaitForMultipleObjects(2, events, FALSE, 500);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED) { break; }
        if (worker && WaitForSingleObject(worker, 0) == WAIT_OBJECT_0) {
            last_attempt = GetTickCount64();
            DWORD code = 1;
            GetExitCodeProcess(worker, &code);
            CloseHandle(worker);
            worker = nullptr;
            log(root, "connection/launch worker exited: " + std::to_string(code));
            if (worker_request) { finish_request(request, request_mutex, worker_identity, code); }
            worker_request = false;
            worker_pid = 0;
        }
        HANDLE app = OpenMutexW(SYNCHRONIZE, FALSE, kAppMutex);
        const bool app_active = app != nullptr;
        if (app) { CloseHandle(app); }
        bool got_request = false;
        RequestIdentity identity{};
        if (waited == WAIT_OBJECT_0 + 1) {
            MailboxLock lock(request_mutex);
            if (lock.acquired && request->state == kPending) {
                identity = {request->owner_pid, request->request_id, request->agent_epoch};
                InterlockedExchange(&request->state, kRunning);
                got_request = true;
            }
        }
        if (got_request) {
            const auto length = wcsnlen(request->game_path, kGamePathCapacity);
            const std::wstring requested(request->game_path, length);
            if (!app_active || game_launch_disabled() || app_stopping() || !process_alive(static_cast<DWORD>(identity.owner))) {
                finish_request(request, request_mutex, identity, ERROR_CANCELLED);
            } else if (request->magic != kLaunchMagic || length == 0 || length == kGamePathCapacity ||
                _wcsicmp(std::filesystem::path(requested).filename().c_str(), L"Arknights.exe") != 0 ||
                GetFileAttributesW(requested.c_str()) == INVALID_FILE_ATTRIBUTES) {
                finish_request(request, request_mutex, identity, ERROR_INVALID_PARAMETER);
            } else {
                const auto games = running_games();
                if (worker && worker_request && !process_alive(static_cast<DWORD>(worker_identity.owner))) {
                    CloseHandle(worker); // The obsolete worker will fail its final owner/epoch guard.
                    worker = nullptr;
                    worker_request = false;
                }
                if (!games.empty() && heartbeat_alive(root, games.front().pid)) {
                    finish_request(request, request_mutex, identity, 0);
                } else if (worker) {
                    if (!games.empty() && games.front().pid == worker_pid) { worker_request = true; worker_identity = identity; }
                    else { finish_request(request, request_mutex, identity, ERROR_BUSY); }
                } else {
                    const bool already_running = !games.empty();
                    const auto game = already_running && !games.front().path.empty() ? games.front().path : requested;
                    std::wstring command = quote(self) + L" --game " + quote(game) + L" --report " +
                        quote((root / L"runtime/logs/launch-report.json").wstring());
                    worker_pid = already_running ? games.front().pid : 0;
                    if (already_running) { command += L" --inject " + std::to_wstring(worker_pid); }
                    command += L" --require-app --request-owner " + std::to_wstring(identity.owner) +
                        L" --request-id " + std::to_wstring(identity.id) + L" --agent-epoch " + std::to_wstring(identity.epoch);
                    DWORD error = 0;
                    worker = start_worker(command, root, error);
                    if (!worker) { finish_request(request, request_mutex, identity, error); }
                    else {
                        worker_request = true;
                        worker_identity = identity;
                        last_pid = worker_pid;
                        last_attempt = GetTickCount64();
                        if (!already_running) { launch_grace_until = last_attempt + 12000; }
                        log(root, already_running ? "requested connection to existing game" : "requested game launch once");
                    }
                }
            }
        }
        const auto now = GetTickCount64();
        if (game_launch_disabled() || app_stopping() || !app_active || worker || now < launch_grace_until || now - last_scan < 3000) { continue; }
        last_scan = now;
        for (const auto& game : running_games()) {
            if (heartbeat_alive(root, game.pid) || (game.pid == last_pid && now - last_attempt < 30000)) { continue; }
            last_pid = game.pid;
            last_attempt = now;
            DWORD error = 0;
            worker = start_worker(quote(self) + L" --inject " + std::to_wstring(game.pid) + L" --game " + quote(game.path) +
                L" --report " + quote((root / L"runtime/logs/launch-report.json").wstring()), root, error);
            worker_pid = game.pid;
            worker_request = false;
            log(root, "connecting without restarting pid " + std::to_string(game.pid));
            break;
        }
    }
    if (worker_request) { finish_request(request, request_mutex, worker_identity, ERROR_CANCELLED); }
    if (worker) { CloseHandle(worker); }
    log(root, "native agent stopped");
    UnmapViewOfFile(request);
    CloseHandle(mapping); CloseHandle(launch); CloseHandle(request_mutex); CloseHandle(stop); CloseHandle(singleton);
    return 0;
}
