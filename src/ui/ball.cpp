#include "ball.h"
#include "menu.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "../log/log.h"
#include "../shell/constants.h"
#include "../shell/theme.h"
#include "../shell/win_util.h"

namespace arkhelp
{
namespace
{

constexpr int kBallLogicalSize = 64;
constexpr float kBallRadius = 30.4f;  // 64 逻辑空间里留 ~1.6px 余量给边缘抗锯齿
constexpr int kEdgeSnapLogical = 24;
constexpr int kEdgeMarginLogical = 8;
constexpr int kDragThreshold = 3;
// Anchor the light on the diagonal edge: part overlaps the Logo and part lies outside it.
constexpr float kStatusCenterLogical = 53.5f;
constexpr float kStatusRadiusLogical = 5.5f;

struct BallState
{
    HWND hwnd = nullptr;
    BallCallbacks callbacks;
    LinkState link = LinkState::kDisconnected;
    int opacity = 85;
    int scale_percent = 100;
    int dpi = 96;
    int size = 64;
    int edge = 0;
    bool visible = false;
    bool class_registered = false;
    bool dragging = false;
    bool drag_moved = false;
    POINT drag_cursor_start{0, 0};
    RECT drag_window_start{0, 0, 0, 0};
    LayeredBitmap surface;
    HBITMAP logo = nullptr;
    int logo_size = 0;
};

BallState g_ball;

void release_ball_logo()
{
    if (g_ball.logo != nullptr)
    {
        destroy_bitmap(g_ball.logo);
        g_ball.logo = nullptr;
    }
    g_ball.logo_size = 0;
}

void ensure_ball_logo(int size)
{
    if (g_ball.logo_size == size)
    {
        return;
    }
    release_ball_logo();
    g_ball.logo_size = size;
    const auto path = join_path(module_dir(), L"data\\logo.png");
    g_ball.logo = load_png_scaled(path, size, size);
    if (g_ball.logo == nullptr)
    {
        log_warn("ball logo missing: %s", wide_to_utf8(path).c_str());
    }
}

// Preserve the supplied ArkHelper artwork. Only multiply its already-premultiplied
// pixels by a circular coverage mask. The status light straddles the lower-right edge,
// with no added outline or shadow. The layered window also covers its exterior part.
void render_ball()
{
    if (g_ball.hwnd == nullptr)
    {
        return;
    }
    const int size = g_ball.size;
    if (!layered_bitmap_create(g_ball.surface, size, size))
    {
        return;
    }
    ensure_ball_logo(size);
    DIBSECTION logo{};
    const bool have_logo = g_ball.logo != nullptr &&
        GetObjectW(g_ball.logo, sizeof(logo), &logo) == sizeof(logo) &&
        logo.dsBm.bmBits != nullptr && logo.dsBm.bmWidth == size &&
        logo.dsBm.bmHeight == size && logo.dsBm.bmBitsPixel == 32;
    auto* pixels = static_cast<std::uint32_t*>(g_ball.surface.bits);
    const auto* source = have_logo ? static_cast<const std::uint32_t*>(logo.dsBm.bmBits) : pixels;
    if (!have_logo)
    {
        // Retain a recognisable monogram if an installation has lost its artwork.
        fill_rect(g_ball.surface.dc, RECT{0, 0, size, size}, RGB(5, 6, 8));
        HFONT font = CreateFontW(-std::max(12, size * 30 / 64), 0, 0, 0, FW_BOLD, FALSE,
            FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        const auto old = SelectObject(g_ball.surface.dc, font);
        SetBkMode(g_ball.surface.dc, TRANSPARENT);
        SetTextColor(g_ball.surface.dc, RGB(248, 248, 248));
        RECT text{0, 0, size, size};
        DrawTextW(g_ball.surface.dc, L"A", 1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(g_ball.surface.dc, old);
        DeleteObject(font);
        GdiFlush();
    }
    const float radius = static_cast<float>(size) * kBallRadius / kBallLogicalSize;
    const float center = static_cast<float>(size) * 0.5f;
    for (int y = 0; y < size; ++y)
    {
        for (int x = 0; x < size; ++x)
        {
            const float dx = static_cast<float>(x) + 0.5f - center;
            const float dy = static_cast<float>(y) + 0.5f - center;
            const float coverage = std::clamp(radius + 0.5f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
            const auto mask = static_cast<std::uint32_t>(coverage * 255.0f + 0.5f);
            const auto index = static_cast<std::size_t>(y) * size + x;
            const auto original = source[index] | (have_logo ? 0u : 0xff000000u);
            const auto component = [original, mask](int shift) {
                return ((((original >> shift) & 255u) * mask + 127) / 255) << shift;
            };
            pixels[index] = component(24) | component(16) | component(8) | component(0);
        }
    }
    const COLORREF status = g_ball.link == LinkState::kBattle ? theme::kStatusGreen :
                            g_ball.link == LinkState::kWaiting ? theme::kStatusYellow : theme::kStatusGray;
    const float status_center = size * kStatusCenterLogical / kBallLogicalSize;
    const float status_radius = size * kStatusRadiusLogical / kBallLogicalSize;
    for (int y = 0; y < size; ++y)
    {
        for (int x = 0; x < size; ++x)
        {
            const float dx = x + .5f - status_center;
            const float dy = y + .5f - status_center;
            const float coverage = std::clamp(status_radius + .5f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
            const auto alpha = static_cast<std::uint32_t>(coverage * 255.0f + .5f);
            if (!alpha) { continue; }
            const auto index = static_cast<std::size_t>(y) * size + x;
            const auto under = pixels[index];
            const auto channel = [alpha, under](std::uint32_t colour, int shift) {
                return ((colour * alpha + ((under >> shift) & 255u) * (255 - alpha) + 127) / 255) << shift;
            };
            const auto out_alpha = alpha + (((under >> 24) * (255 - alpha) + 127) / 255);
            pixels[index] = (out_alpha << 24) | channel(GetRValue(status), 16) |
                channel(GetGValue(status), 8) | channel(GetBValue(status), 0);
        }
    }
    const BYTE present_alpha = static_cast<BYTE>((g_ball.opacity * 255 + 50) / 100);
    layered_bitmap_present(g_ball.hwnd, g_ball.surface, present_alpha);
}

void clamp_into_monitor(RECT& rect, int dpi)
{
    const int margin = scale_by_dpi(kEdgeMarginLogical, dpi);
    HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &info))
    {
        return;
    }
    if (rect.left < info.rcWork.left + margin)
    {
        const int delta = info.rcWork.left + margin - rect.left;
        rect.left += delta;
        rect.right += delta;
    }
    if (rect.right > info.rcWork.right - margin)
    {
        const int delta = rect.right - (info.rcWork.right - margin);
        rect.left -= delta;
        rect.right -= delta;
    }
    if (rect.top < info.rcWork.top + margin)
    {
        const int delta = info.rcWork.top + margin - rect.top;
        rect.top += delta;
        rect.bottom += delta;
    }
    if (rect.bottom > info.rcWork.bottom - margin)
    {
        const int delta = rect.bottom - (info.rcWork.bottom - margin);
        rect.top -= delta;
        rect.bottom -= delta;
    }
}

// 拖动结束后贴边：按球心到工作区左右边缘的距离判断，24px 内吸附。
int snap_to_edges()
{
    RECT rect;
    if (!GetWindowRect(g_ball.hwnd, &rect))
    {
        return 0;
    }
    const int snap = scale_by_dpi(kEdgeSnapLogical, g_ball.dpi);
    const int center_x = (rect.left + rect.right) / 2;

    HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &info))
    {
        return 0;
    }

    int edge = 0;
    if (center_x - info.rcWork.left < snap)
    {
        rect.left = info.rcWork.left - g_ball.size / 2;
        rect.right = rect.left + g_ball.size;
        edge = static_cast<int>(DockEdge::kLeft);
    }
    else if (info.rcWork.right - center_x < snap)
    {
        rect.right = info.rcWork.right + g_ball.size / 2;
        rect.left = rect.right - g_ball.size;
        edge = static_cast<int>(DockEdge::kRight);
    }
    else
    {
        clamp_into_monitor(rect, g_ball.dpi);
    }
    SetWindowPos(g_ball.hwnd, HWND_TOPMOST, rect.left, rect.top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    return edge;
}

void default_position(int& x, int& y)
{
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    x = work.right - g_ball.size - scale_by_dpi(12, g_ball.dpi);
    y = work.top + (work.bottom - work.top - g_ball.size) / 2;
}

void apply_config_position(const Config& config)
{
    int x = config.ball_x;
    int y = config.ball_y;
    const int edge = config.edge;

    RECT work{};
    HMONITOR monitor = nullptr;
    if (x != kConfigNoPosition && y != kConfigNoPosition)
    {
        const POINT center{x + g_ball.size / 2, y + g_ball.size / 2};
        monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONULL);
    }

    if (monitor == nullptr)
    {
        // 首次启动（配置里没有坐标）或旧坐标落在已拔掉的显示器上：用屏幕右侧居中的默认位置。
        default_position(x, y);
        g_ball.edge = 0;
        SetWindowPos(g_ball.hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);
    work = info.rcWork;

    const int margin = scale_by_dpi(kEdgeMarginLogical, g_ball.dpi);
    if (edge == static_cast<int>(DockEdge::kLeft))
    {
        x = work.left + margin;
    }
    else if (edge == static_cast<int>(DockEdge::kRight))
    {
        x = work.right - g_ball.size - margin;
    }
    else
    {
        const int min_x = work.left + margin;
        const int max_x = work.right - g_ball.size - margin;
        x = x < min_x ? min_x : (x > max_x ? max_x : x);
    }

    RECT rect{x, y, x + g_ball.size, y + g_ball.size};
    clamp_into_monitor(rect, g_ball.dpi);
    SetWindowPos(g_ball.hwnd, HWND_TOPMOST, rect.left, rect.top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    g_ball.edge = edge;
}

void show_ball(bool visible)
{
    if (g_ball.hwnd == nullptr)
    {
        return;
    }
    g_ball.visible = visible;
    ShowWindow(g_ball.hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (visible)
    {
        SetWindowPos(g_ball.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    log_info("ball %s", visible ? "shown" : "hidden");
}

LRESULT CALLBACK ball_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_NCHITTEST:
    {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(hwnd, &point);
        RECT client{};
        GetClientRect(hwnd, &client);
        const float center_x = static_cast<float>(client.right) * 0.5f;
        const float center_y = static_cast<float>(client.bottom) * 0.5f;
        const float radius = static_cast<float>(std::min(client.right, client.bottom)) * 0.5f - 1.0f;
        const float dx = point.x - center_x;
        const float dy = point.y - center_y;
        if (dx * dx + dy * dy <= radius * radius)
        {
            return HTCLIENT;
        }
        return HTTRANSPARENT;
    }
    case WM_MOUSEACTIVATE:
        // 关键：鼠标点击不激活窗口，游戏保持前台。
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    case WM_LBUTTONDOWN:
        g_ball.dragging = true;
        g_ball.drag_moved = false;
        GetCursorPos(&g_ball.drag_cursor_start);
        GetWindowRect(hwnd, &g_ball.drag_window_start);
        SetCapture(hwnd);
        return 0;
    case WM_MOUSEMOVE:
    {
        if (g_ball.dragging)
        {
            POINT cursor{};
            GetCursorPos(&cursor);
            const int dx = cursor.x - g_ball.drag_cursor_start.x;
            const int dy = cursor.y - g_ball.drag_cursor_start.y;
            if (!g_ball.drag_moved && (std::abs(dx) > kDragThreshold || std::abs(dy) > kDragThreshold))
            {
                g_ball.drag_moved = true;
            }
            if (g_ball.drag_moved)
            {
                RECT before{};
                GetWindowRect(hwnd, &before);
                SetWindowPos(hwnd, HWND_TOPMOST, g_ball.drag_window_start.left + dx,
                             g_ball.drag_window_start.top + dy, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
                RECT after{};
                GetWindowRect(hwnd, &after);
                menu_follow(after.left - before.left, after.top - before.top);
            }
        }
        return 0;
    }
    case WM_LBUTTONUP:
    {
        if (!g_ball.dragging)
        {
            return 0;
        }
        g_ball.dragging = false;
        ReleaseCapture();
        if (g_ball.drag_moved)
        {
            const int edge = snap_to_edges();
            g_ball.edge = edge;
            if (g_ball.callbacks.on_drag_finished)
            {
                RECT rect{};
                GetWindowRect(hwnd, &rect);
                g_ball.callbacks.on_drag_finished(rect.left, rect.top, edge);
            }
        }
        else if (g_ball.callbacks.on_left_click)
        {
            g_ball.callbacks.on_left_click();
        }
        g_ball.drag_moved = false;
        return 0;
    }
    case WM_RBUTTONUP:
        if (g_ball.callbacks.on_right_click)
        {
            g_ball.callbacks.on_right_click();
        }
        return 0;
    case WM_DPICHANGED:
    {
        g_ball.dpi = static_cast<int>(HIWORD(wparam));
        g_ball.size = scale_by_dpi(kBallLogicalSize, g_ball.dpi) * g_ball.scale_percent / 100;
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        if (suggested != nullptr)
        {
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, g_ball.size, g_ball.size,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        render_ball();
        return 0;
    }
    case WM_DESTROY:
        release_ball_logo();
        layered_bitmap_destroy(g_ball.surface);
        g_ball.hwnd = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

bool ball_create(HINSTANCE instance, BallCallbacks callbacks, const Config& config)
{
    if (g_ball.hwnd != nullptr)
    {
        return true;
    }

    g_ball.callbacks = std::move(callbacks);
    g_ball.dpi = dpi_for_system();
    g_ball.scale_percent = config.ui_scale < 80 ? 80 : (config.ui_scale > 150 ? 150 : config.ui_scale);
    g_ball.size = scale_by_dpi(kBallLogicalSize, g_ball.dpi) * g_ball.scale_percent / 100;
    g_ball.opacity = config.opacity < 40 ? 40 : (config.opacity > 100 ? 100 : config.opacity);

    if (!g_ball.class_registered)
    {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = ball_proc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_HAND);
        window_class.lpszClassName = constants::kBallClass;
        if (RegisterClassExW(&window_class) == 0)
        {
            log_error("RegisterClassExW(ball) failed, code=%d", static_cast<int>(GetLastError()));
            return false;
        }
        g_ball.class_registered = true;
    }

    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                                constants::kBallClass, constants::kAppTitle, WS_POPUP, 0, 0, g_ball.size,
                                g_ball.size, nullptr, nullptr, instance, nullptr);
    if (hwnd == nullptr)
    {
        log_error("CreateWindowExW(ball) failed, code=%d", static_cast<int>(GetLastError()));
        return false;
    }
    g_ball.hwnd = hwnd;

    apply_config_position(config);
    render_ball();
    show_ball(true);
    log_info("ball created, size=%d dpi=%d opacity=%d", g_ball.size, g_ball.dpi, g_ball.opacity);
    return true;
}

void ball_destroy()
{
    if (g_ball.hwnd != nullptr)
    {
        DestroyWindow(g_ball.hwnd);
        g_ball.hwnd = nullptr;
    }
    release_ball_logo();
    layered_bitmap_destroy(g_ball.surface);
}

void ball_show(bool visible)
{
    show_ball(visible);
}

bool ball_is_visible()
{
    return g_ball.visible;
}

void ball_set_link_state(LinkState state)
{
    if (g_ball.link == state) { return; }
    g_ball.link = state;
    if (g_ball.hwnd != nullptr) { render_ball(); }
}

void ball_set_opacity(int percent)
{
    const int clamped = percent < 40 ? 40 : (percent > 100 ? 100 : percent);
    if (g_ball.opacity == clamped)
    {
        return;
    }
    g_ball.opacity = clamped;
    if (g_ball.hwnd != nullptr)
    {
        render_ball();
    }
}

void ball_set_scale(int percent)
{
    const int clamped = percent < 80 ? 80 : (percent > 150 ? 150 : percent);
    if (g_ball.scale_percent == clamped)
    {
        return;
    }
    g_ball.scale_percent = clamped;
    g_ball.size = scale_by_dpi(kBallLogicalSize, g_ball.dpi) * clamped / 100;
    if (g_ball.hwnd == nullptr)
    {
        return;
    }
    RECT rect{};
    GetWindowRect(g_ball.hwnd, &rect);
    layered_bitmap_destroy(g_ball.surface);
    SetWindowPos(g_ball.hwnd, nullptr, rect.left, rect.top, g_ball.size, g_ball.size,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    render_ball();
}

bool ball_get_rect(RECT& out)
{
    if (g_ball.hwnd == nullptr)
    {
        return false;
    }
    return GetWindowRect(g_ball.hwnd, &out) != FALSE;
}

HWND ball_hwnd()
{
    return g_ball.hwnd;
}

}  // namespace arkhelp
