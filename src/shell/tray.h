#pragma once

#include <functional>

#include "platform.h"

// 托盘图标：负责 Shell_NotifyIconW 的增删和右键菜单。
// 具体动作通过回调交给 App，托盘本身不持有业务状态，方便替换占位图标/菜单项。
namespace arkhelp
{

struct TrayCallbacks
{
    std::function<void()> toggle_ball;
    std::function<void()> open_menu;
    std::function<void()> open_settings;
    std::function<void()> launch_game;
    std::function<void()> exit_app;
};

bool tray_create(HWND owner, const wchar_t* tooltip, TrayCallbacks callbacks);
void tray_destroy();
// 处理 WM_APP+1 托盘回调消息；返回 true 表示消息已消费。
bool tray_handle_message(WPARAM wparam, LPARAM lparam);
void tray_set_tooltip(const wchar_t* text);

}  // namespace arkhelp
