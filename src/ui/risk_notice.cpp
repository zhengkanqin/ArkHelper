#include "risk_notice.h"
#include "risk_notice_state.h"
#include "../shell/theme.h"
#include "../shell/win_util.h"
#include <algorithm>
#include <string>

namespace arkhelp::risk_notice {
namespace {
constexpr int kCheck = 1001, kBody = 1002, kCountdown = 1003;
constexpr wchar_t kText[] =
    L"我们的理念与承诺\r\n"
    L"ArkHelper 只做信息采集、统计、查询与数据可视化，帮助玩家理解战斗。"
    L"不修改游戏玩法数据，不提供侵入式暴力功能，此承诺适用于后续开发。\r\n\r\n"
    L"功能边界\r\n"
    L"不修改生命、伤害、费用、资源、掉落、战斗速度或结算；不提供自定义关卡、"
    L"修改藏品、自选或解锁干员、绕过条件进入关卡、无敌、无限资源或自动战斗。"
    L"不加载外部游戏插件。缺失的观测数据会标记为不可用。\r\n\r\n"
    L"技术实现\r\n"
    L"桌面悬浮窗负责显示；私有 DLL 注入游戏进程，通过原生 Hook 观察战斗回调，"
    L"读取局内数据后传给桌面。回调保留原始参数、返回值及正常执行流程。"
    L"加载过程与 Hook 会写入启动代码、加载参数和函数入口，因此本项目并非无注入、"
    L"无内存写入的工具。“不改数据”指不修改游戏玩法数据。关闭助手会停止采集，"
    L"已加载的 DLL 随游戏退出释放。部分底层代码源自 ArkFramework。\r\n\r\n"
    L"使用风险\r\n"
    L"信息辅助的定位并不代表游戏官方认可。注入与 Hook 可能触发检测，带来连接失败、"
    L"游戏崩溃、性能波动、版本不兼容或账号处罚（包括封禁）等风险。"
    L"项目不能保证规避检测或账号安全；请自行判断是否愿意承担这些风险。\r\n\r\n"
    L"许可与来源\r\n"
    L"本项目按 GPL-3.0-or-later 提供，按现状发布，不提供担保。"
    L"您可按 GPL 的条件使用、修改和再分发；许可证及来源说明见程序目录的 LICENSE 与 README.md。"
    L"风险知晓不附加限制这些开源权利的条件。\r\n\r\n"
    L"首次启动\r\n"
    L"同意后会自动配置启动环境并创建桌面快捷方式。Windows 会要求一次管理员确认，"
    L"用于安装后台连接组件；后续打开助手无需重复授权。游戏位置会自动识别，"
    L"无法识别时可在设置中选择 Arknights.exe。\r\n\r\n"
    L"确认前，助手不会自动启动游戏或连接采集。请在本窗口连续阅读超过 10 秒，"
    L"主动勾选下方选项后再决定是否使用。不同意可直接退出。";

struct Notice {
    Config& config;
    const std::wstring& path;
    ReadingGate gate;
    HWND window = nullptr, body = nullptr, check = nullptr, countdown = nullptr;
    HWND accept = nullptr, cancel = nullptr, close = nullptr;
    HFONT font = nullptr, title_font = nullptr;
    HBRUSH background = CreateSolidBrush(theme::kBackground);
    bool done = false, accepted = false;
    std::wstring last_countdown;
    ~Notice() { DeleteObject(font); DeleteObject(title_font); DeleteObject(background); }
};

void layout(Notice& notice) {
    const int dpi = dpi_for_window(notice.window);
    const auto px = [dpi](int value) { return scale_by_dpi(value, dpi); };
    RECT area; GetClientRect(notice.window, &area);
    const int w = area.right, h = area.bottom, margin = px(24);
    MoveWindow(notice.close, w - px(48), px(10), px(32), px(32), TRUE);
    MoveWindow(notice.body, margin, px(68), w - margin * 2, h - px(234), TRUE);
    MoveWindow(notice.check, margin, h - px(150), w - margin * 2, px(36), TRUE);
    MoveWindow(notice.countdown, margin, h - px(109), w - margin * 2, px(24), TRUE);
    MoveWindow(notice.cancel, w - margin - px(310), h - px(64), px(110), px(38), TRUE);
    MoveWindow(notice.accept, w - margin - px(184), h - px(64), px(184), px(38), TRUE);
    HFONT old_font = notice.font, old_title = notice.title_font;
    notice.font = create_ui_font(15, FW_NORMAL, dpi, 100);
    notice.title_font = create_ui_font(20, FW_SEMIBOLD, dpi, 100);
    for (HWND child : {notice.body, notice.check, notice.countdown,
                       notice.accept, notice.cancel, notice.close})
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(notice.font), TRUE);
    DeleteObject(old_font); DeleteObject(old_title);
    InvalidateRect(notice.window, nullptr, TRUE);
}

void update(Notice& notice) {
    notice.gate.observe(GetTickCount64(), IsWindowVisible(notice.window) &&
        !IsIconic(notice.window) && GetForegroundWindow() == notice.window);
    const bool checked = SendMessageW(notice.check, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(notice.accept, notice.gate.can_accept(checked));
    const unsigned seconds = notice.gate.seconds_remaining();
    const std::wstring caption = seconds ?
        L"请保持本窗口在前台阅读，还需 " + std::to_wstring(seconds) + L" 秒" :
        L"已完成阅读，请勾选风险确认后决定是否使用。";
    if (caption != notice.last_countdown) {
        SetWindowTextW(notice.countdown, caption.c_str());
        notice.last_countdown = caption;
    }
}

LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* notice = reinterpret_cast<Notice*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        notice = static_cast<Notice*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        notice->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(notice));
    }
    if (!notice) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_CREATE: {
        const auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
            return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1,
                window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                reinterpret_cast<CREATESTRUCTW*>(lparam)->hInstance, nullptr);
        };
        notice->body = child(L"EDIT", kText, ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
            WS_VSCROLL | WS_TABSTOP, kBody);
        notice->check = child(L"BUTTON", L"我已阅读并知晓上述技术实现与使用风险", BS_AUTOCHECKBOX | WS_TABSTOP, kCheck);
        notice->countdown = child(L"STATIC", L"", 0, kCountdown);
        notice->accept = child(L"BUTTON", L"同意并开始使用", BS_OWNERDRAW | WS_TABSTOP, IDOK);
        notice->cancel = child(L"BUTTON", L"不同意，退出", BS_OWNERDRAW | WS_TABSTOP, IDCANCEL);
        notice->close = child(L"BUTTON", L"×", BS_OWNERDRAW | WS_TABSTOP, 1004);
        if (!notice->body || !notice->check || !notice->countdown || !notice->accept ||
            !notice->cancel || !notice->close) return -1;
        EnableWindow(notice->accept, FALSE);
        if (!SetTimer(window, 1, 100, nullptr)) return -1;
        layout(*notice);
        return 0;
    }
    case WM_TIMER:
        update(*notice);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) notice->gate.observe(GetTickCount64(), false);
        update(*notice);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == IDCANCEL || LOWORD(wparam) == 1004) { DestroyWindow(window); return 0; }
        if (LOWORD(wparam) == kCheck) { update(*notice); return 0; }
        if (LOWORD(wparam) == IDOK) {
            update(*notice);
            const bool checked = SendMessageW(notice->check, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (!notice->gate.can_accept(checked)) return 0;
            Config next = notice->config;
            next.risk_notice_version = kNoticeVersion;
            if (!config_save(notice->path, next)) {
                MessageBoxW(window, L"风险确认记录保存失败，请检查程序目录是否可写。保存成功前无法启用助手。",
                    L"ArkHelper", MB_OK | MB_ICONERROR);
                return 0;
            }
            notice->config = std::move(next);
            notice->accepted = true;
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY:
        KillTimer(window, 1);
        notice->done = true;
        // A sent accessibility command can destroy the window inside GetMessage.
        // Wake that call so the notice loop can observe completion.
        PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
        return 0;
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(window, &point);
        RECT area; GetClientRect(window, &area);
        if (point.y < scale_by_dpi(56, dpi_for_window(window)) &&
            point.x < area.right - scale_by_dpi(56, dpi_for_window(window))) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_DPICHANGED: {
        const auto* rect = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left,
            rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout(*notice); return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint; HDC dc = BeginPaint(window, &paint);
        RECT area; GetClientRect(window, &area); fill_rect(dc, area, theme::kBackground);
        const int dpi = dpi_for_window(window);
        RECT title{scale_by_dpi(24, dpi), scale_by_dpi(18, dpi),
            area.right - scale_by_dpi(64, dpi), scale_by_dpi(52, dpi)};
        draw_text(dc, L"ArkHelper · 使用前说明", title, notice->title_font,
            theme::kTextPrimary, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        EndPaint(window, &paint); return 0;
    }
    case WM_DRAWITEM: {
        const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        const bool disabled = (item.itemState & ODS_DISABLED) != 0;
        fill_round_rect(item.hDC, item.rcItem, scale_by_dpi(6, dpi_for_window(window)),
            item.CtlID == IDOK && !disabled ? theme::kAccentDeep : theme::kCard);
        wchar_t text[80]; GetWindowTextW(item.hwndItem, text, 80);
        draw_text(item.hDC, text, item.rcItem, notice->font,
            disabled ? theme::kTextMuted : theme::kTextPrimary, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        if (item.itemState & ODS_FOCUS) {
            RECT focus = item.rcItem; InflateRect(&focus, -4, -4); DrawFocusRect(item.hDC, &focus);
        }
        return TRUE;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(wparam), theme::kTextPrimary);
        SetBkColor(reinterpret_cast<HDC>(wparam), theme::kBackground);
        return reinterpret_cast<LRESULT>(notice->background);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
}

bool ensure_accepted(HINSTANCE instance, const std::wstring& config_path, Config& config) {
    if (config.risk_notice_version == kNoticeVersion) return true;
    WNDCLASSW cls{};
    cls.lpfnWndProc = procedure; cls.hInstance = instance;
    cls.lpszClassName = kWindowClass; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    POINT cursor; GetCursorPos(&cursor);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const RECT work = monitor.rcWork;
    const int dpi = dpi_for_system();
    const int width = std::min(scale_by_dpi(680, dpi), static_cast<int>(work.right - work.left - 24));
    const int height = std::min(scale_by_dpi(690, dpi), static_cast<int>(work.bottom - work.top - 24));
    Notice notice{config, config_path};
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_APPWINDOW, kWindowClass,
        L"ArkHelper · 使用前说明", WS_POPUP | WS_CLIPCHILDREN,
        work.left + (work.right - work.left - width) / 2,
        work.top + (work.bottom - work.top - height) / 2, width, height,
        nullptr, nullptr, instance, &notice);
    if (!window) return false;
    ShowWindow(window, SW_SHOW); SetForegroundWindow(window); SetFocus(notice.body);
    update(notice);
    MSG message{};
    while (!notice.done) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) { if (!result) PostQuitMessage(static_cast<int>(message.wParam)); break; }
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) { DestroyWindow(window); continue; }
        if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN) {
            SendMessageW(window, WM_COMMAND, IDOK, 0); continue;
        }
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (IsWindow(window)) DestroyWindow(window);
    return notice.accepted;
}
}
