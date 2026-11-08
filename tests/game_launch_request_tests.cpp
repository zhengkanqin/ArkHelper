// Exercise the real desktop request code against an isolated fake agent. No installed task,
// product process or actual game is touched; the fake agent only acknowledges shared memory.
#define ARKHELP_AGENT_OBJECT_SUFFIX L".RequestTests"
#include "../src/game/game_launch.cpp"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <stdexcept>

namespace {
int checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) { throw std::runtime_error(message); }
}
struct LockProbe { HANDLE mutex; DWORD result = WAIT_FAILED; };
DWORD WINAPI probe_lock(LPVOID argument) {
    auto& probe = *static_cast<LockProbe*>(argument);
    probe.result = WaitForSingleObject(probe.mutex, 0);
    if (probe.result == WAIT_OBJECT_0 || probe.result == WAIT_ABANDONED) { ReleaseMutex(probe.mutex); }
    return 0;
}
void reply(arkhelp::launcher::LaunchRequest* request, LONG code) {
    InterlockedExchange(&request->result, code);
    InterlockedExchange(&request->state, arkhelp::launcher::kDone);
    arkhelp::game_launch::poll();
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wcscmp(argv[1], L"--exit-fixture") == 0) { return 0; }
    namespace gl = arkhelp::game_launch;
    namespace agent = arkhelp::launcher;
    using namespace arkhelp;
    HANDLE event = nullptr, mapping = nullptr, request_mutex = nullptr, app = nullptr, dead_owner = nullptr;
    agent::LaunchRequest* request = nullptr;
    bool created_launcher = false;
    const auto directory = std::filesystem::path(gl::module_directory());
    const auto fake_launcher = directory / L"ArkHelperLauncher.exe";
    const auto fixture = directory / (L"request-test-" + std::to_wstring(GetCurrentProcessId()));
    const auto cleanup = [&]() {
        const auto resolved = std::filesystem::weakly_canonical(fixture);
        const auto expected_parent = std::filesystem::weakly_canonical(directory);
        if (resolved.parent_path() != expected_parent || resolved.filename() != fixture.filename()) {
            throw std::runtime_error("refusing to remove a fixture outside the test directory");
        }
        std::filesystem::remove_all(resolved);
    };
    try {
        SetEnvironmentVariableW(L"ARKHELP_NO_GAME_LAUNCH", L"1");
        Config config;
        gl::initialize(directory.wstring(), config);
        check(!gl::request_launch() && !gl::busy(), "test gate did not stop the request before accessing the agent");
        check(gl::last_error().find(L"ARKHELP_NO_GAME_LAUNCH") != std::wstring::npos, "test gate error is missing");
        SetEnvironmentVariableW(L"ARKHELP_NO_GAME_LAUNCH", nullptr);

        std::filesystem::create_directories(fixture / L"游戏 空格");
        config.game_path = (fixture / L"游戏 空格/Arknights.exe").wstring();
        std::ofstream(std::filesystem::path(config.game_path)) << "not executable; IPC fixture only\n";
        if (!std::filesystem::exists(fake_launcher)) {
            std::ofstream(fake_launcher) << "not executable; IPC fixture only\n";
            created_launcher = true;
        }
        gl::initialize(directory.wstring(), config);
        check(gl::paths_ready(), "isolated fixture paths are not ready");
        request_mutex = CreateMutexW(nullptr, FALSE, agent::kRequestMutex);
        check(request_mutex && GetLastError() != ERROR_ALREADY_EXISTS, "isolated mailbox mutex is already in use");
        event = CreateEventW(nullptr, FALSE, FALSE, agent::kLaunchEvent);
        check(event && GetLastError() != ERROR_ALREADY_EXISTS, "isolated request event is already in use");
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                     sizeof(agent::LaunchRequest), agent::kLaunchMapping);
        check(mapping && GetLastError() != ERROR_ALREADY_EXISTS, "isolated request mapping is already in use");
        request = static_cast<agent::LaunchRequest*>(MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE,
                                                                  0, 0, sizeof(agent::LaunchRequest)));
        check(request != nullptr, "cannot map fake agent request");
        request->magic = agent::kLaunchMagic;
        request->state = agent::kIdle;
        request->request_id = 0;
        request->result = 0;
        request->owner_pid = 0;
        request->agent_pid = static_cast<LONG>(GetCurrentProcessId());
        request->agent_epoch = 1;
        check(gl::request_launch() && gl::busy(), "real request code did not queue asynchronously");
        check(WaitForSingleObject(event, 0) == WAIT_OBJECT_0, "agent event was not signaled");
        check(request->state == agent::kPending && request->request_id != 0, "request was not fully published");
        check(config.game_path == request->game_path, "Unicode game path changed across the agent request");
        const LONG first_id = request->request_id;
        check(!gl::request_launch() && request->request_id == first_id, "busy request launched twice");
        reply(request, 0);
        check(!gl::busy() && gl::last_error().empty(), "success did not finish the asynchronous request");

        check(gl::request_launch(), "second request could not reuse the completed channel");
        check(request->request_id != first_id, "a reused channel retained the previous request ID");
        reply(request, 6);
        check(!gl::busy() && gl::last_error().find(L"6") != std::wstring::npos,
              "connection failure was not visible to the desktop app");

        // A live owner's unfinished command is never reclaimed or signaled twice.
        request->state = agent::kRunning;
        request->owner_pid = static_cast<LONG>(GetCurrentProcessId());
        request->request_id = 999;
        ResetEvent(event);
        check(gl::request_launch() && gl::busy(), "a busy mailbox should wait asynchronously");
        check(request->state == agent::kRunning && request->request_id == 999,
              "a live owner's request was overwritten");
        check(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "a live owner's request was signaled again");
        gl::g_request_started = GetTickCount64() - 15001;
        gl::poll();
        check(!gl::busy(), "busy live-owner mailbox did not time out");

        // Keep a real exited process handle so the recovery exercises SYNCHRONIZE/Wait,
        // rather than treating an arbitrary inaccessible PID as dead.
        wchar_t self[32768] = {};
        GetModuleFileNameW(nullptr, self, 32768);
        std::wstring child_command = L"\"" + std::wstring(self) + L"\" --exit-fixture";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        check(CreateProcessW(nullptr, child_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                             nullptr, directory.c_str(), &startup, &child), "cannot create the exited-owner fixture");
        CloseHandle(child.hThread);
        dead_owner = child.hProcess;
        check(WaitForSingleObject(dead_owner, 5000) == WAIT_OBJECT_0, "owner fixture did not exit");
        check(!agent::process_alive(child.dwProcessId), "a signaled owner process was classified as live");
        for (const LONG orphan_state : {agent::kWriting, agent::kPending, agent::kRunning}) {
            request->state = orphan_state;
            request->owner_pid = static_cast<LONG>(child.dwProcessId);
            request->request_id = 1000;
            check(gl::request_launch(), "orphaned mailbox request was not accepted for recovery");
            check(request->state == agent::kPending && request->owner_pid == static_cast<LONG>(GetCurrentProcessId()),
                  "dead owner was not replaced by the current App");
            check(WaitForSingleObject(event, 0) == WAIT_OBJECT_0, "recovered orphan was not submitted exactly once");
            reply(request, 0);
            check(!gl::busy() && gl::last_error().empty(), "recovered orphan did not finish");
        }

        check(gl::request_launch(), "restart recovery fixture could not submit");
        check(WaitForSingleObject(event, 0) == WAIT_OBJECT_0, "initial recovery fixture event missing");
        const LONG resumed_id = request->request_id;
        const agent::RequestIdentity old_identity{request->owner_pid, resumed_id, request->agent_epoch};
        {
            agent::MailboxLock lock(request_mutex);
            request->state = agent::kIdle;
            request->owner_pid = 0;
            request->request_id = 0;
            InterlockedIncrement64(&request->agent_epoch); // Simulate the replacement agent resetting the existing mapping.
        }
        gl::poll();
        check(gl::busy() && request->state == agent::kPending && request->request_id == resumed_id,
              "agent restart lost the original App intent or created a new request ID");
        check(WaitForSingleObject(event, 0) == WAIT_OBJECT_0, "restarted agent did not receive the pending intent");
        gl::poll();
        check(WaitForSingleObject(event, 0) == WAIT_TIMEOUT, "same recovered intent was submitted twice");

        app = CreateMutexW(nullptr, FALSE, agent::kAppMutex);
        check(app && GetLastError() != ERROR_ALREADY_EXISTS, "isolated App presence is already in use");
        request->state = agent::kRunning;
        const agent::RequestIdentity identity{request->owner_pid, request->request_id, request->agent_epoch};
        {
            agent::AppLaunchGuard stale(true, old_identity);
            check(!stale.allowed, "old agent worker retained permission after an epoch reset");
        }
        {
            agent::AppLaunchGuard before_arm(true, identity);
            check(before_arm.allowed, "live matching App request could not authorize arming");
            LockProbe probe{request_mutex};
            HANDLE thread = CreateThread(nullptr, 0, probe_lock, &probe, 0, nullptr);
            check(thread && WaitForSingleObject(thread, 1000) == WAIT_OBJECT_0, "could not probe the final launch lock");
            CloseHandle(thread);
            check(probe.result == WAIT_TIMEOUT, "final approval did not serialize mailbox reset/reclamation");
        }
        HANDLE stopping = CreateEventW(nullptr, TRUE, TRUE, agent::kRuntimeStopEvent);
        check(stopping && GetLastError() != ERROR_ALREADY_EXISTS, "isolated runtime-stop event is already in use");
        {
            agent::AppLaunchGuard closing_app(true, identity);
            check(!closing_app.allowed, "App shutdown signal did not cancel launch while the process was still alive");
        }
        CloseHandle(stopping);
        CloseHandle(app);
        app = nullptr;
        {
            agent::AppLaunchGuard before_create(true, identity);
            check(!before_create.allowed, "App closed after arming but game creation remained authorized");
        }
        {
            agent::AppLaunchGuard manual(false, {});
            check(manual.allowed, "direct manual launcher incorrectly requires an App");
        }
        app = CreateMutexW(nullptr, FALSE, agent::kAppMutex);
        request->owner_pid = static_cast<LONG>(child.dwProcessId);
        {
            const agent::RequestIdentity dead_identity{request->owner_pid, request->request_id, request->agent_epoch};
            agent::AppLaunchGuard replacement_app(true, dead_identity);
            check(!replacement_app.allowed, "new App mutex revived a dead owner's launch permission");
        }
        request->owner_pid = identity.owner;
        request->agent_pid = static_cast<LONG>(child.dwProcessId);
        {
            agent::AppLaunchGuard stopped_agent(true, identity);
            check(!stopped_agent.allowed, "worker retained launch permission after its agent exited");
        }
        request->agent_pid = static_cast<LONG>(GetCurrentProcessId());
        reply(request, 0);
        check(!gl::busy(), "recovered request did not acknowledge completion");

        request->magic = 0;
        check(gl::request_launch() && gl::busy(), "channel-not-ready request should wait asynchronously");
        gl::g_request_started = GetTickCount64() - 15001;
        gl::poll();
        check(!gl::busy() && !gl::last_error().empty(), "missing agent response did not time out visibly");
        request->magic = agent::kLaunchMagic;
        check(gl::g_watch_process == nullptr, "test request unexpectedly ran a scheduled task");
        std::printf("PASS game launch request: %d checks; gate, owner recovery, agent restart, App-exit guard, single request, Unicode, error and timeout\n", checks);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        gl::close_request();
        if (request) { UnmapViewOfFile(request); }
        if (mapping) { CloseHandle(mapping); }
        if (event) { CloseHandle(event); }
        if (request_mutex) { CloseHandle(request_mutex); }
        if (app) { CloseHandle(app); }
        if (dead_owner) { CloseHandle(dead_owner); }
        if (created_launcher) { std::filesystem::remove(fake_launcher); }
        cleanup();
        return 1;
    }
    gl::close_request();
    if (request) { UnmapViewOfFile(request); }
    if (mapping) { CloseHandle(mapping); }
    if (event) { CloseHandle(event); }
    if (request_mutex) { CloseHandle(request_mutex); }
    if (app) { CloseHandle(app); }
    if (dead_owner) { CloseHandle(dead_owner); }
    if (created_launcher) { std::filesystem::remove(fake_launcher); }
    cleanup();
    return 0;
}
