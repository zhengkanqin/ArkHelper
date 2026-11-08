#pragma once

#include <string>

#include "../config/config.h"

// Reuse the installed elevated background agent. Launch requests never close a running
// game, prompt for UAC, or install tasks. The desktop app remains at normal privilege.
namespace arkhelp
{
namespace game_launch
{

struct Paths
{
    std::wstring game_exe;   // Arknights.exe
    std::wstring loader_exe; // ArkHelperLauncher.exe
    std::wstring launcher_exe; // ArkHelperLauncher.exe（本程序目录）
    std::wstring report;       // 加载器结果 JSON（诊断用）
};

// root_directory 是 ArkHelper 的数据根目录（ArkHelper/）。
void initialize(const std::wstring& root_directory, const Config& config);
const Paths& paths();
bool paths_ready();
bool busy();
const std::wstring& last_error();

// Single asynchronous request: connect an existing game, or start it if absent.
// false means no request was queued; last_error() explains the failure. poll() finishes
// busy() and records asynchronous errors. ARKHELP_NO_GAME_LAUNCH disables this entry.
bool request_launch();

// Compatibility entry; same safe semantics as request_launch(), never restarts games.
bool launch_or_restart();
void poll();

// Start an already installed task silently. Never registers a task or requests UAC.
bool arm_silent_watch();
bool silent_watch_active();
bool watch_agent_present();

}  // namespace game_launch
}  // namespace arkhelp
