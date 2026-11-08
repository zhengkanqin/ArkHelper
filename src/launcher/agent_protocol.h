#pragma once

#include <windows.h>
#include <cstdint>
#include <cstddef>

namespace arkhelp::launcher {

#ifndef ARKHELP_AGENT_OBJECT_SUFFIX
#define ARKHELP_AGENT_OBJECT_SUFFIX L""
#endif
inline constexpr wchar_t kLaunchEvent[] = L"Local\\ArkHelp.Agent.Launch.v2" ARKHELP_AGENT_OBJECT_SUFFIX;
inline constexpr wchar_t kLaunchMapping[] = L"Local\\ArkHelp.Agent.Request.v2" ARKHELP_AGENT_OBJECT_SUFFIX;
inline constexpr wchar_t kRequestMutex[] = L"Local\\ArkHelp.Agent.RequestLock.v2" ARKHELP_AGENT_OBJECT_SUFFIX;
inline constexpr wchar_t kGameStartMutex[] = L"Local\\ArkHelp.Game.Start.v2" ARKHELP_AGENT_OBJECT_SUFFIX;
inline constexpr wchar_t kAppMutex[] = L"Local\\ArkHelp.App" ARKHELP_AGENT_OBJECT_SUFFIX;
inline constexpr wchar_t kRuntimeStopEvent[] = L"Local\\ArkHelp.RuntimeStop" ARKHELP_AGENT_OBJECT_SUFFIX;
inline constexpr std::uint32_t kLaunchMagic = 0x41484c32;
inline constexpr std::size_t kGamePathCapacity = 4096;

enum LaunchState : LONG { kIdle = 0, kWriting, kPending, kRunning, kDone };
struct LaunchRequest {
    std::uint32_t magic = kLaunchMagic;
    volatile LONG state = kIdle;
    volatile LONG request_id = 0;
    volatile LONG result = 0;
    volatile LONG owner_pid = 0;
    volatile LONG agent_pid = 0;
    alignas(8) volatile LONG64 agent_epoch = 0;
    wchar_t game_path[kGamePathCapacity] = {};
};

class MailboxLock {
public:
    explicit MailboxLock(HANDLE mutex, DWORD timeout = INFINITE) : mutex_(mutex) {
        const DWORD result = mutex ? WaitForSingleObject(mutex, timeout) : WAIT_FAILED;
        acquired = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        abandoned = result == WAIT_ABANDONED;
    }
    ~MailboxLock() { if (acquired) { ReleaseMutex(mutex_); } }
    MailboxLock(const MailboxLock&) = delete;
    bool acquired = false, abandoned = false;
private:
    HANDLE mutex_;
};

// Access denied is not evidence that the owner died. Only a signaled process handle or
// a definitively missing PID permits another desktop process to reclaim its request.
inline bool process_alive(DWORD pid) {
    if (!pid) { return false; }
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) { return GetLastError() != ERROR_INVALID_PARAMETER; }
    const DWORD result = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return result != WAIT_OBJECT_0;
}

inline bool game_launch_disabled() {
    wchar_t value[8] = {};
    return GetEnvironmentVariableW(L"ARKHELP_NO_GAME_LAUNCH", value, 8) != 0 && value[0] != L'0';
}

inline bool app_stopping() {
    HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, kRuntimeStopEvent);
    if (!event) { return false; }
    const bool stopped = WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
    CloseHandle(event);
    return stopped;
}

struct RequestIdentity { LONG owner = 0, id = 0; LONG64 epoch = 0; };
inline bool same_request(const LaunchRequest& request, const RequestIdentity& identity) {
    return request.owner_pid == identity.owner && request.request_id == identity.id &&
        request.agent_epoch == identity.epoch;
}

// The caller keeps this guard alive through process creation. That serializes the final
// approval with mailbox reclamation/agent restart, and rejects a newly opened App replacing
// the process that originally authorized this launch.
class AppLaunchGuard {
public:
    AppLaunchGuard(bool required, const RequestIdentity& identity) {
        if (!required) { allowed = true; return; }
        if (!identity.owner || !identity.id || !identity.epoch || game_launch_disabled() || app_stopping()) { return; }
        app_ = OpenMutexW(SYNCHRONIZE, FALSE, kAppMutex);
        if (!app_ || !process_alive(static_cast<DWORD>(identity.owner))) { return; }
        mutex_ = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, kRequestMutex);
        const DWORD waited = mutex_ ? WaitForSingleObject(mutex_, 1000) : WAIT_FAILED;
        locked_ = waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED;
        if (!locked_) { return; }
        mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, kLaunchMapping);
        if (mapping_) { view_ = static_cast<LaunchRequest*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, sizeof(LaunchRequest))); }
        allowed = view_ && view_->magic == kLaunchMagic && view_->state == kRunning &&
            same_request(*view_, identity) && process_alive(static_cast<DWORD>(view_->agent_pid));
    }
    ~AppLaunchGuard() {
        if (view_) { UnmapViewOfFile(view_); }
        if (mapping_) { CloseHandle(mapping_); }
        if (locked_) { ReleaseMutex(mutex_); }
        if (mutex_) { CloseHandle(mutex_); }
        if (app_) { CloseHandle(app_); }
    }
    AppLaunchGuard(const AppLaunchGuard&) = delete;
    bool allowed = false;
private:
    HANDLE app_ = nullptr, mutex_ = nullptr, mapping_ = nullptr;
    LaunchRequest* view_ = nullptr;
    bool locked_ = false;
};

} // namespace arkhelp::launcher
