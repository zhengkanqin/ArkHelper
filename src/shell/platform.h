#pragma once

// 统一固定平台目标版本：Per-Monitor V2 DPI 相关 API（SetProcessDpiAwarenessContext /
// GetDpiForWindow）要求 Windows 10 1607 以上，这里把 _WIN32_WINNT 提到 0x0A00，
// 避免各翻译单元里零散地再包一层 #ifndef。
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
