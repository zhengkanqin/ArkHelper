#pragma once

#include "platform.h"

// 外壳入口。实现放在 app.cpp 的单例 App 里：
// main.cpp 只保留 WinMain 入口，方便后续换子系统/加命令行参数。
namespace arkhelp
{

// 初始化并进入消息循环，返回进程退出码。
int run_app(HINSTANCE instance);

}  // namespace arkhelp
