#pragma once

#include <functional>

#include "../config/config.h"
#include "../shell/platform.h"
#include "../shell/status.h"

// 悬浮球窗口：无激活的分层圆形窗口（WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE）。
// 交互约定：
// - 左键单击切换功能菜单；左键拖动移动，松开后吸附屏幕左右边缘；
// - 右键打开设置；
// - 不抢焦点，避免在游戏前台时把游戏切到后台。
namespace arkhelp
{

struct BallCallbacks
{
    std::function<void()> on_left_click;
    std::function<void()> on_right_click;
    std::function<void(int x, int y, int edge)> on_drag_finished;
};

bool ball_create(HINSTANCE instance, BallCallbacks callbacks, const Config& config);
void ball_destroy();

void ball_show(bool visible);
bool ball_is_visible();

void ball_set_link_state(LinkState state);
void ball_set_opacity(int percent);
void ball_set_scale(int percent);
bool ball_get_rect(RECT& out);
HWND ball_hwnd();

}  // namespace arkhelp
