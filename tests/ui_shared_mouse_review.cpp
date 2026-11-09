#include "../src/ui/panels.cpp"
#include <stdexcept>

struct NativeInput {
    HWND window = nullptr;
    LONG down = 0, up = 0, drag = 0, wheel = 0;
};
static NativeInput* native_input = nullptr;

static LRESULT CALLBACK native_proc(HWND hwnd, UINT message, WPARAM keys, LPARAM point) {
    switch (message) {
    case WM_LBUTTONDOWN: InterlockedIncrement(&native_input->down); SetCapture(hwnd); return 0;
    case WM_LBUTTONUP: InterlockedIncrement(&native_input->up); ReleaseCapture(); return 0;
    case WM_MOUSEMOVE: if (keys & MK_LBUTTON) InterlockedIncrement(&native_input->drag); return 0;
    case WM_MOUSEWHEEL: InterlockedIncrement(&native_input->wheel); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, keys, point);
}

static void pump(DWORD duration = 90) {
    const auto end = GetTickCount64() + duration;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < end);
}

static void require(bool condition, const char* error) {
    if (!condition) throw std::runtime_error(error);
}

static void mouse(POINT point, DWORD flag = MOUSEEVENTF_MOVE, DWORD data = 0) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = (point.x - GetSystemMetrics(SM_XVIRTUALSCREEN)) * 65535 / (GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
    input.mi.dy = (point.y - GetSystemMetrics(SM_YVIRTUALSCREEN)) * 65535 / (GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | flag;
    input.mi.mouseData = data;
    require(SendInput(1, &input, sizeof(input)) == 1, "fixture mouse input unavailable");
    pump();
}

int wmain(int argc, wchar_t** argv) {
    using namespace arkhelp;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc > 2 && std::wcscmp(argv[1], L"--native") == 0) {
        HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, argv[2]);
        if (!mapping) return 2;
        native_input = static_cast<NativeInput*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(NativeInput)));
        WNDCLASSW klass{}; klass.lpfnWndProc = native_proc; klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpszClassName = L"ArkHelper.Test.NativeMouse"; klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&klass);
        RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        native_input->window = CreateWindowExW(WS_EX_TOPMOST, klass.lpszClassName, L"ArkHelper mouse test", WS_POPUP | WS_VISIBLE,
            work.left + 70, work.top + 70, 780, 520, nullptr, nullptr, klass.hInstance, nullptr);
        SetForegroundWindow(native_input->window);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
        UnmapViewOfFile(native_input); CloseHandle(mapping);
        return 0;
    }
    const HWND old_foreground = GetForegroundWindow();
    POINT old_cursor{}; GetCursorPos(&old_cursor);
    const auto name = L"Local\\ArkHelper.MouseReview." + std::to_wstring(GetCurrentProcessId());
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(NativeInput), name.c_str());
    native_input = mapping ? static_cast<NativeInput*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(NativeInput))) : nullptr;
    PROCESS_INFORMATION child{};
    int result = 0;
    try {
        require(native_input != nullptr, "native mouse fixture mapping failed");
        wchar_t executable[MAX_PATH]{}; GetModuleFileNameW(nullptr, executable, MAX_PATH);
        auto command = L"\"" + std::wstring(executable) + L"\" --native " + name;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        require(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child),
            "native mouse fixture process failed");
        AllowSetForegroundWindow(child.dwProcessId);
        for (int i = 0; i < 100 && !native_input->window; ++i) pump(20);
        require(IsWindow(native_input->window), "native mouse fixture window missing");
        const auto foreground_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const auto own_thread = GetCurrentThreadId();
        const bool attached = foreground_thread && foreground_thread != own_thread && AttachThreadInput(own_thread, foreground_thread, TRUE);
        const bool attached_target = AttachThreadInput(own_thread, child.dwThreadId, TRUE);
        BringWindowToTop(native_input->window);
        SetForegroundWindow(native_input->window);
        if (attached_target) AttachThreadInput(own_thread, child.dwThreadId, FALSE);
        if (attached) AttachThreadInput(own_thread, foreground_thread, FALSE);
        pump();
        require(panels::initialize(GetModuleHandleW(nullptr), {}), "panel fixture failed");
        auto* panel = panels::find(panels::kDamagePanelId);
        RECT native_rect{}; GetWindowRect(native_input->window, &native_rect);
        panels::restore_geometry(panel->id, native_rect.left + 30, native_rect.top + 30, false);
        panels::restore_width(panel->id, 320);
        panels::restore_height(panel->id, 180);
        std::vector<panels::Row> rows(30); for (auto& row : rows) { row.left = L"测试干员"; row.right = L"12345"; }
        panels::update(panel->id, L"同步鼠标测试", rows, true);
        FeatureAppearance appearance; appearance.click_through = true;
        panels::set_appearance(panel->id, appearance); pump();
        require(panels::g_shared_mouse.running(), "shared mouse observer did not start");
        auto point_at = [&](RECT rect) { POINT p{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2}; ClientToScreen(panel->hwnd, &p); return p; };
        auto click = [&](POINT p) { mouse(p); mouse(p, MOUSEEVENTF_LEFTDOWN); mouse(p, MOUSEEVENTF_LEFTUP); };
        click(point_at(panel->toggle));
        if (!panel->collapsed || native_input->down != 1 || native_input->up != 1) {
            POINT cursor{}; GetCursorPos(&cursor);
            std::fprintf(stderr,"mouse fixture: native=%p foreground=%p hit=%p cursor=(%ld,%ld) down=%ld up=%ld collapsed=%d\n",
                native_input->window,GetForegroundWindow(),WindowFromPoint(cursor),cursor.x,cursor.y,
                native_input->down,native_input->up,panel->collapsed);
        }
        require(panel->collapsed && native_input->down == 1 && native_input->up == 1, "one native click did not reach both panel and other process exactly once");

        POINT title{panel->title_bar.left + 50, (panel->title_bar.top + panel->title_bar.bottom) / 2}; ClientToScreen(panel->hwnd, &title);
        const int x = panel->x, y = panel->y;
        mouse(title); mouse(title, MOUSEEVENTF_LEFTDOWN);
        GUITHREADINFO game_thread{}; game_thread.cbSize = sizeof(game_thread);
        GetGUIThreadInfo(child.dwThreadId, &game_thread);
        require(panel->dragging && GetCapture() == nullptr && game_thread.hwndCapture == native_input->window,
            "shared panel drag stole native capture or did not begin");
        POINT moved{title.x + 95, title.y + 55}; mouse(moved);
        require(panel->x == x + 95 && panel->y == y + 55 && native_input->drag > 0,
            "native drag did not move panel and deliver motion to other process");
        mouse(moved, MOUSEEVENTF_LEFTUP);
        require(!panel->dragging && native_input->down == 2 && native_input->up == 2, "shared drag did not release both endpoints");

        const int width = panel->width_override_px;
        POINT edge = point_at(panel->width_grip); mouse(edge); mouse(edge, MOUSEEVENTF_LEFTDOWN);
        edge.x += 45; mouse(edge); mouse(edge, MOUSEEVENTF_LEFTUP);
        require(panel->width_override_px == width + 45 && !panel->resizing_width, "shared resize lost historical cursor position or release outside the original panel");
        click(point_at(panel->toggle));
        const POINT body = point_at(panel->row_rects[0]);
        mouse(body, MOUSEEVENTF_WHEEL, static_cast<DWORD>(-WHEEL_DELTA));
        require(panel->scroll_px > 0 && native_input->wheel == 1, "native wheel did not reach both endpoints");

        int settings = 0; panels::g_callbacks.on_settings = [&](const wchar_t*) { ++settings; };
        click(point_at(panel->settings_button));
        require(settings == 1, "shared settings click did not respond exactly once");
        const POINT toggle = point_at(panel->toggle);
        const HWND blocker = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"STATIC", L"menu fixture", WS_POPUP | WS_VISIBLE,
            toggle.x - 20, toggle.y - 20, 40, 40, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        click(toggle);
        require(!panel->collapsed, "own menu operated a shared panel behind it");
        DestroyWindow(blocker);

        title = {panel->title_bar.left + 50, (panel->title_bar.top + panel->title_bar.bottom) / 2}; ClientToScreen(panel->hwnd, &title);
        mouse(title); mouse(title, MOUSEEVENTF_LEFTDOWN);
        appearance.click_through = false; panels::set_appearance(panel->id, appearance);
        mouse(title, MOUSEEVENTF_LEFTUP);
        require(!panel->dragging && !panels::g_shared_mouse.running(), "disabling shared input left a captured gesture or worker");
        appearance.click_through = true; panels::set_appearance(panel->id, appearance); pump();
        mouse(title); mouse(title, MOUSEEVENTF_LEFTDOWN);
        panels::update(panel->id, L"同步鼠标测试", rows, false);
        mouse(title, MOUSEEVENTF_LEFTUP);
        require(!panel->dragging && !panels::g_shared_mouse.running() && native_input->down == native_input->up,
            "hiding shared panel left stale input or suppressed native release");
        std::puts("PASS shared mouse: separate-process native click/drag/capture/wheel plus panel toggle/drag/resize/settings, occlusion and midgesture disable/hide");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL shared mouse: %s\n", error.what()); result = 1;
    }
    panels::shutdown();
    if (native_input && native_input->window) PostMessageW(native_input->window, WM_CLOSE, 0, 0);
    if (child.hProcess) {
        if (WaitForSingleObject(child.hProcess, 3000) == WAIT_TIMEOUT) TerminateProcess(child.hProcess, 2);
        CloseHandle(child.hProcess); CloseHandle(child.hThread);
    }
    SetCursorPos(old_cursor.x, old_cursor.y);
    if (old_foreground) SetForegroundWindow(old_foreground);
    if (native_input) UnmapViewOfFile(native_input);
    if (mapping) CloseHandle(mapping);
    return result;
}
