#pragma once

#include <cstdint>

#include "platform.h"

// 深色玻璃风格配色集中于此：GDI 用 COLORREF，悬浮球的逐像素渲染用 ARGB。
// 单独抽一层的原因是自绘窗口很多，颜色散落各处时很难保持视觉一致。
namespace arkhelp
{
namespace theme
{

constexpr COLORREF rgb(int r, int g, int b)
{
    return RGB(static_cast<BYTE>(r), static_cast<BYTE>(g), static_cast<BYTE>(b));
}

// ARGB（0xAARRGGBB），给逐像素渲染用。
constexpr uint32_t argb(int a, int r, int g, int b)
{
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

// ---- 窗口底色 ----
inline constexpr COLORREF kBackground = rgb(24, 26, 32);
inline constexpr COLORREF kBackgroundAlt = rgb(30, 33, 41);
inline constexpr COLORREF kCard = rgb(36, 40, 50);
inline constexpr COLORREF kCardHover = rgb(44, 49, 61);
inline constexpr COLORREF kSelection = rgb(52, 68, 96);
inline constexpr COLORREF kDivider = rgb(52, 57, 70);

// ---- 文本 ----
inline constexpr COLORREF kTextPrimary = rgb(232, 236, 244);
inline constexpr COLORREF kTextSecondary = rgb(150, 158, 176);
inline constexpr COLORREF kTextMuted = rgb(108, 116, 134);

// ---- 强调色 ----
inline constexpr COLORREF kAccent = rgb(86, 148, 255);
inline constexpr COLORREF kAccentDeep = rgb(52, 106, 214);
inline constexpr COLORREF kSwitchOn = rgb(64, 132, 248);
inline constexpr COLORREF kSwitchOff = rgb(70, 76, 90);
inline constexpr COLORREF kSwitchKnob = rgb(238, 243, 255);

// ---- 危险操作（退出）----
inline constexpr COLORREF kDanger = rgb(240, 110, 118);

// ---- 状态点 ----
inline constexpr COLORREF kStatusGray = rgb(138, 144, 158);
inline constexpr COLORREF kStatusYellow = rgb(240, 196, 92);
inline constexpr COLORREF kStatusGreen = rgb(94, 210, 138);

// 悬浮球玻璃质感（ARGB 直通逐像素光栅化）。桌面/游戏背景通常很花，球必须足够实才看得见：
// 采用深色玻璃底 + 亮色描边，保证在亮色游戏画面和浅色浏览器背景上都有清晰轮廓。
inline constexpr uint32_t kBallTop = argb(252, 32, 52, 96);
inline constexpr uint32_t kBallBottom = argb(255, 10, 16, 38);
inline constexpr uint32_t kBallRim = argb(255, 150, 208, 255);
inline constexpr uint32_t kBallHighlight = argb(96, 255, 255, 255);

}  // namespace theme
}  // namespace arkhelp
