#include "tray.h"

#include <cstring>

#include "../log/log.h"
#include "constants.h"

namespace arkhelp
{
namespace
{

constexpr UINT kTrayIconId = 1;

struct TrayState
{
    HWND owner = nullptr;
    NOTIFYICONDATAW data{};
    TrayCallbacks callbacks;
    bool added = false;
};

TrayState g_tray;

void show_context_menu()
{
    POINT cursor{};
    GetCursorPos(&cursor);

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr)
    {
        return;
    }
    AppendMenuW(menu, MF_STRING, constants::kTrayCmdToggleBall, L"显示 / 隐藏悬浮球");
    AppendMenuW(menu, MF_STRING, constants::kTrayCmdOpenMenu, L"打开功能菜单");
    AppendMenuW(menu, MF_STRING, constants::kTrayCmdOpenSettings, L"打开设置");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, constants::kTrayCmdLaunchGame, L"启动游戏并加载（占位）");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, constants::kTrayCmdExit, L"退出 ArkHelper");

    // 托盘菜单必须先让宿主窗口到前台，否则点击别处时菜单不会消失。
    SetForegroundWindow(g_tray.owner);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, cursor.x, cursor.y, 0,
                                        g_tray.owner, nullptr);
    DestroyMenu(menu);
    PostMessageW(g_tray.owner, WM_NULL, 0, 0);

    switch (command)
    {
    case constants::kTrayCmdToggleBall:
        if (g_tray.callbacks.toggle_ball)
        {
            g_tray.callbacks.toggle_ball();
        }
        break;
    case constants::kTrayCmdOpenMenu:
        if (g_tray.callbacks.open_menu)
        {
            g_tray.callbacks.open_menu();
        }
        break;
    case constants::kTrayCmdOpenSettings:
        if (g_tray.callbacks.open_settings)
        {
            g_tray.callbacks.open_settings();
        }
        break;
    case constants::kTrayCmdLaunchGame:
        if (g_tray.callbacks.launch_game)
        {
            g_tray.callbacks.launch_game();
        }
        break;
    case constants::kTrayCmdExit:
        if (g_tray.callbacks.exit_app)
        {
            g_tray.callbacks.exit_app();
        }
        break;
    default:
        break;
    }
}

}  // namespace

bool tray_create(HWND owner, const wchar_t* tooltip, TrayCallbacks callbacks)
{
    tray_destroy();

    g_tray.owner = owner;
    g_tray.callbacks = std::move(callbacks);

    NOTIFYICONDATAW& data = g_tray.data;
    data = NOTIFYICONDATAW{};
    data.cbSize = sizeof(NOTIFYICONDATAW);
    data.hWnd = owner;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = constants::kTrayCallbackMessage;
    data.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
    if (!data.hIcon) { data.hIcon = LoadIconW(nullptr, IDI_APPLICATION); }
    if (tooltip != nullptr)
    {
        lstrcpynW(data.szTip, tooltip, static_cast<int>(sizeof(data.szTip) / sizeof(data.szTip[0])));
    }

    if (!Shell_NotifyIconW(NIM_ADD, &data))
    {
        log_warn("Shell_NotifyIconW(NIM_ADD) failed, code=%d", static_cast<int>(GetLastError()));
        return false;
    }
    g_tray.added = true;

    // 让托盘在 Win10+ 上使用更现代的版本化行为（气泡/图标更新语义）。
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
    log_info("tray icon added");
    return true;
}

void tray_destroy()
{
    if (g_tray.added)
    {
        Shell_NotifyIconW(NIM_DELETE, &g_tray.data);
        g_tray.added = false;
    }
    g_tray.owner = nullptr;
    g_tray.callbacks = TrayCallbacks();
}

bool tray_handle_message(WPARAM /*wparam*/, LPARAM lparam)
{
    if (!g_tray.added)
    {
        return false;
    }

    const UINT event = static_cast<UINT>(lparam);
    switch (event)
    {
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
        if (g_tray.callbacks.open_menu)
        {
            g_tray.callbacks.open_menu();
        }
        return true;
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
        show_context_menu();
        return true;
    default:
        return false;
    }
}

void tray_set_tooltip(const wchar_t* text)
{
    if (!g_tray.added || text == nullptr)
    {
        return;
    }
    lstrcpynW(g_tray.data.szTip, text, static_cast<int>(sizeof(g_tray.data.szTip) / sizeof(g_tray.data.szTip[0])));
    g_tray.data.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_tray.data);
}

}  // namespace arkhelp
