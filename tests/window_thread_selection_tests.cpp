#include "../native/loader/window_hook_loader.cpp"
#include <cstdio>
#include <stdexcept>
#include <atomic>

namespace {
std::atomic<int> sent_callbacks{0};
LRESULT CALLBACK observe_sent_message(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0 && reinterpret_cast<CWPSTRUCT*>(lparam)->message == WM_NULL) {
        ++sent_callbacks;
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
struct SentOnlyWindow {
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HWND window = nullptr;
    std::atomic<bool> stop{false};
    DWORD thread_id = 0;
    HANDLE thread = nullptr;
    SentOnlyWindow() {
        thread = CreateThread(nullptr, 0, [](void* user) -> DWORD {
            auto& fixture = *static_cast<SentOnlyWindow*>(user);
            fixture.window = CreateWindowW(L"UnityWndClass", L"ArkHelper minimized message fixture",
                WS_OVERLAPPEDWINDOW, 50, 50, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (fixture.window) ShowWindow(fixture.window, SW_SHOWMINNOACTIVE);
            SetEvent(fixture.ready);
            MSG message{};
            while (!fixture.stop) {
                // Dispatch sent window messages but do not retrieve posted WM_NULL.
                PeekMessageW(&message, nullptr, WM_APP, WM_APP + 1, PM_REMOVE);
                Sleep(1);
            }
            if (fixture.window) DestroyWindow(fixture.window);
            return 0;
        }, this, 0, &thread_id);
    }
    ~SentOnlyWindow() {
        stop = true;
        if (thread) { WaitForSingleObject(thread, 2000); CloseHandle(thread); }
        if (ready) CloseHandle(ready);
    }
};
}

int wmain() {
    HWND window = nullptr;
    try {
        WNDCLASSW klass{};
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpszClassName = L"UnityWndClass";
        klass.lpfnWndProc = DefWindowProcW;
        if (!RegisterClassW(&klass)) throw std::runtime_error("class registration failed");
        window = CreateWindowW(klass.lpszClassName, L"ArkHelper window selection fixture",
            WS_OVERLAPPEDWINDOW, 50, 50, 800, 600, nullptr, nullptr, klass.hInstance, nullptr);
        if (!window) throw std::runtime_error("window creation failed");
        ShowWindow(window, SW_SHOWNOACTIVATE);
        if (ark::loader::find_game_window(GetCurrentProcessId()).thread_id != GetCurrentThreadId())
            throw std::runtime_error("normal game window not selected");
        ShowWindow(window, SW_SHOWMINNOACTIVE);
        if (!IsIconic(window) || ark::loader::find_game_window(GetCurrentProcessId()).thread_id != GetCurrentThreadId())
            throw std::runtime_error("minimized game UI thread not selected");
        ShowWindow(window, SW_HIDE);
        if (ark::loader::find_game_window(GetCurrentProcessId()).thread_id != 0)
            throw std::runtime_error("hidden unrelated window selected");
        DestroyWindow(window);
        window = nullptr;
        SentOnlyWindow sent_only;
        if (!sent_only.thread || WaitForSingleObject(sent_only.ready, 2000) != WAIT_OBJECT_0 || !sent_only.window)
            throw std::runtime_error("sent-only message fixture failed");
        HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, observe_sent_message, nullptr, sent_only.thread_id);
        if (!hook) throw std::runtime_error("sent-message hook failed");
        ark::loader::nudge_window_thread(sent_only.window, sent_only.thread_id, GetTickCount64() + 2000);
        UnhookWindowsHookEx(hook);
        if (sent_callbacks == 0 || !IsIconic(sent_only.window))
            throw std::runtime_error("minimized sent-only window not nudged or was restored");
        std::puts("PASS window-thread selection: normal, minimized zero-client Unity window, hidden window rejected, sent-only message pump without activation");
        return 0;
    } catch (const std::exception& error) {
        if (window) DestroyWindow(window);
        std::fprintf(stderr,"FAIL %s\n",error.what()); return 1;
    }
}
