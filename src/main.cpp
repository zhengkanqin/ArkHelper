#include "overlay/overlay_window.h"
#include "shell/app.h"
#include "shell/platform.h"

// 入口签名必须是 ANSI WinMain：Zig 自带 MinGW CRT 的 GUI 启动代码固定引用 WinMain，
// 只定义 wWinMain 会链接失败（lld-link: undefined symbol: WinMain）。
//
// App initializes the overlay only after explicit first-use risk acknowledgement.
// Shutdown also supports a user declining before the overlay was created.
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int)
{
    const int result = arkhelp::run_app(instance);
    arkhelp::overlay::shutdown();
    return result;
}
