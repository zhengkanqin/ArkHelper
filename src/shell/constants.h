#pragma once

#include "platform.h"

// 窗口类名 / 跨进程单实例名 / 自定义消息集中在这里。
// 统一常量可以避免 main.cpp、tray、ball 之间出现字符串拼写不一致，
// 也让「第二个实例唤起旧实例」只需依赖一个窗口类名。
namespace arkhelp
{
namespace constants
{

inline constexpr wchar_t kAppName[] = L"ArkHelper";
inline constexpr wchar_t kAppTitle[] = L"ArkHelper · 明日方舟助手";

// 单实例互斥体。Local\ 前缀表示仅当前登录会话可见，多用户登录互不影响。
inline constexpr wchar_t kMutexName[] = L"Local\\ArkHelp.Desktop";
// The elevated agent may connect only after first-use acknowledgement succeeds.
inline constexpr wchar_t kReadyMutexName[] = L"Local\\ArkHelp.App";

// 隐藏的协调窗口：托盘回调、定时器、第二实例唤醒都落在它身上。
inline constexpr wchar_t kShellClass[] = L"ArkHelp.Shell";
inline constexpr wchar_t kBallClass[] = L"ArkHelp.Ball";
inline constexpr wchar_t kMenuClass[] = L"ArkHelp.Menu";

inline constexpr UINT kTrayCallbackMessage = WM_APP + 1;
inline constexpr UINT kMsgWake = WM_APP + 2;       // 第二个实例要求唤起已有实例
inline constexpr UINT kMsgShowMenu = WM_APP + 3;
inline constexpr UINT kMsgShowSettings = WM_APP + 4;
inline constexpr UINT kMsgToggleBall = WM_APP + 5;
inline constexpr UINT kMsgExit = WM_APP + 6;

// 轮询定时器：250ms 足够让战斗时间/连接状态跟手，又不会让 GDI 重绘成为负担。
inline constexpr UINT_PTR kPollTimerId = 1;
inline constexpr UINT kPollIntervalMs = 250;

// 托盘弹出菜单命令 ID（从 1000 起避开系统菜单区间）。
inline constexpr UINT kTrayCmdToggleBall = 1001;
inline constexpr UINT kTrayCmdOpenMenu = 1002;
inline constexpr UINT kTrayCmdOpenSettings = 1003;
inline constexpr UINT kTrayCmdLaunchGame = 1004;
inline constexpr UINT kTrayCmdExit = 1005;

}  // namespace constants
}  // namespace arkhelp
