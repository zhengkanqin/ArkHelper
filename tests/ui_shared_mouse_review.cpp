#include "../src/ui/panels.cpp"
#define ARKHELP_AGENT_OBJECT_SUFFIX L".MouseReview"
#include "../src/launcher/agent_protocol.h"
#include "../src/launcher/mouse_relay_server.h"
#include <sddl.h>
#include <shlobj.h>
#include <stdexcept>

struct NativeInput {
    HWND window = nullptr;
    LONG down = 0, up = 0, drag = 0, wheel = 0;
    volatile LONG request=0,ack=0;
    POINT point{};
    DWORD flags=0,data=0;
};
static NativeInput* native_input = nullptr;
static bool elevated_native=false;

static UINT inject_mouse(POINT point,DWORD flag,DWORD data) {
    INPUT input{};input.type=INPUT_MOUSE;
    input.mi.dx=(point.x-GetSystemMetrics(SM_XVIRTUALSCREEN))*65535/(GetSystemMetrics(SM_CXVIRTUALSCREEN)-1);
    input.mi.dy=(point.y-GetSystemMetrics(SM_YVIRTUALSCREEN))*65535/(GetSystemMetrics(SM_CYVIRTUALSCREEN)-1);
    input.mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK|flag;
    input.mi.mouseData=data;
    return SendInput(1,&input,sizeof(input));
}

static LRESULT CALLBACK native_proc(HWND hwnd, UINT message, WPARAM keys, LPARAM point) {
    switch (message) {
    case WM_TIMER:
        if (native_input->request!=native_input->ack) {
            if(native_input->flags==0xFFFFFFFFu){DestroyWindow(hwnd);return 0;}
            inject_mouse(native_input->point,native_input->flags,native_input->data);
            InterlockedExchange(&native_input->ack,native_input->request);
        }
        return 0;
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

static void mouse(POINT point, DWORD flag = MOUSEEVENTF_MOVE, DWORD data = 0, bool dispatch = true) {
    if(elevated_native) {
        native_input->point=point;native_input->flags=flag;native_input->data=data;
        const auto request=InterlockedIncrement(&native_input->request);
        const auto deadline=GetTickCount64()+2000;
        while(native_input->ack!=request && GetTickCount64()<deadline)Sleep(1);
        require(native_input->ack==request,"elevated fixture input was not generated");
    } else require(inject_mouse(point,flag,data)==1,"fixture mouse input unavailable");
    if (dispatch) pump();
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
        native_input->window = CreateWindowExW(0, klass.lpszClassName, L"ArkHelper mouse test", WS_POPUP | WS_VISIBLE,
            work.left + 70, work.top + 70, 780, 520, nullptr, nullptr, klass.hInstance, nullptr);
        SetForegroundWindow(native_input->window);
        SetTimer(native_input->window,1,5,nullptr);
        PSECURITY_DESCRIPTOR descriptor=nullptr;
        ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;AU)S:(ML;;NW;;;ME)",
            SDDL_REVISION_1,&descriptor,nullptr);
        SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
        arkhelp::launcher::MouseRelayServer relay;
        if(IsUserAnAdmin()) require(relay.start(&security),"fixture input relay failed");
        LocalFree(descriptor);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
        UnmapViewOfFile(native_input); CloseHandle(mapping);
        return 0;
    }
    const HWND old_foreground = GetForegroundWindow();
    elevated_native=argc>1 && std::wcscmp(argv[1],L"--elevated")==0;
    HANDLE ready_marker=CreateMutexW(nullptr,FALSE,arkhelp::launcher::kAppMutex);
    POINT old_cursor{}; GetCursorPos(&old_cursor);
    const auto name = L"Local\\ArkHelper.MouseReview." + std::to_wstring(GetCurrentProcessId());
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(NativeInput), name.c_str());
    native_input = mapping ? static_cast<NativeInput*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(NativeInput))) : nullptr;
    PROCESS_INFORMATION child{};
    arkhelp::SharedMouseInput path_observer;
    int result = 0;
    try {
        require(native_input != nullptr, "native mouse fixture mapping failed");
        wchar_t executable[MAX_PATH]{}; GetModuleFileNameW(nullptr, executable, MAX_PATH);
        auto command = L"\"" + std::wstring(executable) + L"\" --native " + name;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        if(elevated_native) {
            SHELLEXECUTEINFOW launch{sizeof(launch)};launch.fMask=SEE_MASK_NOCLOSEPROCESS;
            launch.lpVerb=L"runas";launch.lpFile=executable;
            const auto arguments=L"--native "+name;
            launch.lpParameters=arguments.c_str();launch.nShow=SW_HIDE;
            require(ShellExecuteExW(&launch),"elevated native mouse fixture process failed");
            child.hProcess=launch.hProcess;child.dwProcessId=GetProcessId(child.hProcess);
        } else require(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child),
                "native mouse fixture process failed");
        AllowSetForegroundWindow(child.dwProcessId);
        for (int i = 0; i < 100 && !native_input->window; ++i) pump(20);
        require(IsWindow(native_input->window), "native mouse fixture window missing");
        child.dwThreadId=GetWindowThreadProcessId(native_input->window,nullptr);
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
        require(path_observer.start(panels::g_shared_window, WM_APP + 0x7F), "second input subscriber failed");
        pump(1200); // wait for the private agent observation lease and relay handshake
        auto point_at = [&](RECT rect) { POINT p{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2}; ClientToScreen(panel->hwnd, &p); return p; };
        auto click = [&](POINT p) { mouse(p); mouse(p, MOUSEEVENTF_LEFTDOWN); mouse(p, MOUSEEVENTF_LEFTUP); };
        click(point_at(panel->toggle));
        if (!panel->collapsed || native_input->down != 1 || native_input->up != 1) {
            POINT cursor{}; GetCursorPos(&cursor);
            std::fprintf(stderr,"mouse fixture: native=%p foreground=%p hit=%p cursor=(%ld,%ld) down=%ld up=%ld collapsed=%d\n",
                native_input->window,GetForegroundWindow(),WindowFromPoint(cursor),cursor.x,cursor.y,
                native_input->down,native_input->up,panel->collapsed);
            std::fprintf(stderr,"shared: target=%p panel=%p visible=%d content=(%ld,%ld,%ld,%ld) capture=%p held=%d\n",
                panels::shared_mouse_hit(cursor),panel,panel->visible,panel->content.left,panel->content.top,
                panel->content.right,panel->content.bottom,GetCapture(),panels::g_shared_mouse.left_down());
            for(const auto& event:path_observer.take())std::fprintf(stderr,"observed: %x (%ld,%ld) time=%llu foreground=%p keys=%llx\n",
                event.message,event.screen.x,event.screen.y,static_cast<unsigned long long>(event.time),event.foreground,
                static_cast<unsigned long long>(event.keys));
            HANDLE relay_mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,kMouseRelayName);
            if(relay_mapping) {
                auto* relay=static_cast<const MouseRelayState*>(MapViewOfFile(relay_mapping,FILE_MAP_READ,0,0,sizeof(MouseRelayState)));
                if(relay) {
                    std::fprintf(stderr,"relay: now=%llu published=%llu lease=%lld epoch=%llu written=%llu sequence=%ld\n",
                        GetTickCount64(),relay->published,relay->requested_until,relay->epoch,relay->written,relay->sequence);
                    for(auto serial=relay->written>8?relay->written-8:0;serial<relay->written;++serial) {
                        const auto& event=relay->events[serial%kMouseRelayCapacity];
                        std::fprintf(stderr,"ring: %llu %x (%d,%d) time=%llu keys=%x\n",event.serial,event.message,event.x,event.y,event.time,event.keys);
                    }
                    UnmapViewOfFile(relay);
                }
                CloseHandle(relay_mapping);
            } else std::fprintf(stderr,"relay missing: %lu\n",GetLastError());
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
        if (panel->x != x + 95 || panel->y != y + 55 || native_input->drag <= 0) {
            std::fprintf(stderr,"drag fixture: panel=(%d,%d) expected=(%d,%d) dragging=%d native_moves=%ld held=%d\n",
                panel->x,panel->y,x+95,y+55,panel->dragging,native_input->drag,panels::g_shared_mouse.left_down());
            for (const auto& event : path_observer.take()) std::fprintf(stderr,"event %x (%ld,%ld) keys=%llx\n",
                event.message,event.screen.x,event.screen.y,static_cast<unsigned long long>(event.keys));
        }
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
        if(settings!=1)std::fprintf(stderr,"settings callbacks=%d pressed=%d dragging=%d held=%d\n",
            settings,panel->settings_pressed,panel->dragging,panels::g_shared_mouse.left_down());
        require(settings == 1, "shared settings click did not respond exactly once");
        path_observer.take();
        const auto settings_point = point_at(panel->settings_button);
        mouse(settings_point);
        mouse(settings_point, MOUSEEVENTF_LEFTDOWN, 0, false);
        mouse(settings_point, MOUSEEVENTF_LEFTUP, 0, false);
        mouse({settings_point.x + 180, settings_point.y + 120}, MOUSEEVENTF_MOVE, 0, false);
        pump();
        require(settings == 2, "quick click followed by motion was lost or used the later cursor position");
        unsigned downs = 0, ups = 0;
        for (const auto& event : path_observer.take()) {
            if (event.message == WM_LBUTTONDOWN) {
                ++downs;
                require(event.screen.x == settings_point.x && event.screen.y == settings_point.y && event.time,
                    "path subscriber lost button event position/time");
            }
            if (event.message == WM_LBUTTONUP) ++ups;
        }
        require(downs == 1 && ups == 1, "panel/path subscribers stole registration or duplicated a quick click");
        mouse(settings_point);
        mouse(settings_point,MOUSEEVENTF_LEFTDOWN);
        mouse(settings_point,MOUSEEVENTF_LEFTUP,0,false);
        Sleep(60); // broker has queued up, but the panel thread has not handled it
        SendMessageW(panels::g_shared_window,WM_TIMER,panels::kSharedMouseTimer,0);
        pump();
        require(settings==3,"panel timer cancelled a queued button-up before the settings action completed");
        const POINT toggle = point_at(panel->toggle);
        const HWND blocker = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"STATIC", L"menu fixture", WS_POPUP | WS_VISIBLE | SS_NOTIFY,
            toggle.x - 20, toggle.y - 20, 40, 40, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        pump();
        require(WindowFromPoint(toggle) == blocker, "menu fixture must receive native hit testing");
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
        auto* panel=panels::find(panels::kDamagePanelId);
        std::fprintf(stderr,"state: dragging=%d settings=%d gesture=%p source=%p foreground=%p held=%d capture=%p\n",
            panel?panel->dragging:0,panel?panel->settings_pressed:0,panels::g_shared_gesture,panels::g_shared_source,
            GetForegroundWindow(),panels::g_shared_mouse.left_down(),GetCapture());
        for(const auto& event:path_observer.take())std::fprintf(stderr,"observed: %x (%ld,%ld) time=%llu foreground=%p keys=%llx\n",
            event.message,event.screen.x,event.screen.y,static_cast<unsigned long long>(event.time),event.foreground,
            static_cast<unsigned long long>(event.keys));
    }
    panels::shutdown();
    path_observer.stop();
    if (native_input && native_input->window) {
        if(elevated_native){native_input->flags=0xFFFFFFFFu;InterlockedIncrement(&native_input->request);}
        else PostMessageW(native_input->window,WM_CLOSE,0,0);
    }
    if (child.hProcess) {
        if (WaitForSingleObject(child.hProcess, 3000) == WAIT_TIMEOUT) TerminateProcess(child.hProcess, 2);
        CloseHandle(child.hProcess); CloseHandle(child.hThread);
    }
    SetCursorPos(old_cursor.x, old_cursor.y);
    if (old_foreground) SetForegroundWindow(old_foreground);
    if (native_input) UnmapViewOfFile(native_input);
    if (mapping) CloseHandle(mapping);
    if(ready_marker)CloseHandle(ready_marker);
    return result;
}
