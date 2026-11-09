// Exercise the real external overlay against replaceable Unity-style windows.
// No game process, runtime injection, network, or IPC publisher is used.
#include "../src/overlay/overlay_window.cpp"
#include <cstdio>
#include <stdexcept>

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}
int wmain() {
    using namespace arkhelp;
    using namespace arkhelp::overlay;
    HWND first = nullptr, second = nullptr, third = nullptr;
    try {
        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSW fixture{};
        fixture.lpfnWndProc = DefWindowProcW;
        fixture.hInstance = instance;
        fixture.lpszClassName = L"UnityWndClass";
        check(RegisterClassW(&fixture) != 0, "fixture class registration failed");
        const auto make = [&] {
            return CreateWindowW(fixture.lpszClassName, L"明日方舟", WS_OVERLAPPEDWINDOW,
                30, 30, 800, 600, nullptr, nullptr, instance, nullptr);
        };
        first = make(); second = make();
        check(first && second, "fixture windows missing");
        ShowWindow(first, SW_SHOWNOACTIVATE);
        check(initialize(instance), "overlay initialization failed");
        g_game_window = first;
        ShowWindow(first, SW_HIDE);
        ShowWindow(second, SW_SHOWNOACTIVATE);
        g_last_window_scan_ms = 0;
        update_live();
        check(IsWindow(first) && g_game_window == second,
              "fullscreen replacement left overlay attached to the still-alive hidden HWND");
        DestroyWindow(second); second = nullptr;
        third = make(); ShowWindow(third, SW_SHOWNOACTIVATE);
        update_live();
        check(g_game_window == third, "destroy/recreate did not rediscover the game window");

        g_options.path_on_click=true;
        ipc::BattleSnapshot click_snapshot{};
        click_snapshot.in_battle=1;click_snapshot.battle_state=2;click_snapshot.battle_serial=9;
        click_snapshot.unit_count=2;
        for (int i=0;i<2;++i) {
            auto& unit=click_snapshot.units[i];unit.tracking_id=100+i;
            unit.flags=ipc::kUnitValid;unit.x=200+i*10;unit.y=200;
        }
        g_pointer={700,500,true,false}; // Pointer already moved away before the next render.
        const auto click_time=GetTickCount64();
        g_clicks.push_back({WM_LBUTTONDOWN,{230,230},MK_LBUTTON,click_time,third});
        advance_paths(click_snapshot,1,click_time+1,{30,30},800,600,true);
        check(g_paths.requests().size()==2 && g_clicks.empty(),"queued click lost its original coordinates or nearby group");
        g_paths.reset();
        g_clicks.push_back({WM_LBUTTONDOWN,{230,230},MK_LBUTTON,click_time,third});
        advance_paths(click_snapshot,1,click_time+600,{30,30},800,600,true);
        check(g_paths.requests().empty(),"expired click was replayed into battle");
        g_clicks.push_back({WM_LBUTTONDOWN,{230,230},MK_LBUTTON,click_time,first});
        advance_paths(click_snapshot,1,click_time+1,{30,30},800,600,true);
        check(g_paths.requests().empty(),"foreign foreground click triggered game paths");
        g_paths.reset();

        g_demo = true; // Render pixels without a real battle or foreground game.
        ipc::BattleSnapshot snapshot{};
        snapshot.published_ms = GetTickCount64();
        snapshot.in_battle = 1; snapshot.battle_state = 2;
        ipc::CommandState commands{};
        commands.ui_scale = 1.f;
        const auto original_style = GetWindowLongPtrW(third, GWL_STYLE);
        const SIZE sizes[] = {{800,600},{1920,1080},{1280,720},{640,480},{1024,768},{800,600}};
        for (const auto& size : sizes) {
            Sleep(35);
            snapshot.published_ms = GetTickCount64();
            present_scene(snapshot, commands, 20, 20, size.cx, size.cy, 1.f, 0);
            RECT rect{}; GetWindowRect(g_window, &rect);
            if (rect.right-rect.left!=size.cx || rect.bottom-rect.top!=size.cy)
                std::fprintf(stderr,"geometry expected=%ldx%ld actual=%ldx%ld surface=%dx%d error=%lu\n",
                    size.cx,size.cy,rect.right-rect.left,rect.bottom-rect.top,g_surface.width(),g_surface.height(),GetLastError());
            check(rect.right - rect.left == size.cx && rect.bottom - rect.top == size.cy,
                  "layered drawing window did not follow the client dimensions");
            check(g_surface.width() == size.cx && g_surface.height() == size.cy,
                  "drawing surface retained the old resolution");
        }
        for (int i = 0; i < 12; ++i) {
            Sleep(35);
            snapshot.published_ms = GetTickCount64();
            present_scene(snapshot, commands, 20 + i, 20 + i, 800 + i * 8, 600 + i * 4, 1.f, 0);
        }
        SendMessageW(g_window, WM_DISPLAYCHANGE, 32, MAKELPARAM(1920,1080));
        check(IsWindow(third) && GetWindowLongPtrW(third, GWL_STYLE) == original_style,
              "display changes modified the game window");
        shutdown();
        DestroyWindow(first); DestroyWindow(third);
        std::puts("PASS overlay window changes: retained hidden HWND, destroyed HWND, six resolutions, continuous resize, display change; fixture preserved");
        return 0;
    } catch (const std::exception& error) {
        shutdown();
        if (first) DestroyWindow(first);
        if (second) DestroyWindow(second);
        if (third) DestroyWindow(third);
        std::fprintf(stderr, "FAIL %s\n", error.what()); return 1;
    }
}
