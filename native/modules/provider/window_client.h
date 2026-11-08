#pragma once
#include <windows.h>
#include <cstdint>
#include <cwchar>

namespace ark::arkhelper {
struct GameClient {
    HWND window = nullptr;
    int width = 0, height = 0;
    explicit operator bool() const { return window && width >= 16 && height >= 16; }
};

// Fullscreen/resolution changes can leave an old, hidden Unity HWND alive. Resolve only the
// injected provider's own PID, prefer its foreground root, otherwise use its largest visible client.
inline GameClient current_game_client() {
    struct Search {
        DWORD pid = GetCurrentProcessId();
        HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
        GameClient best;
        std::uint64_t area = 0;
    } search;
    EnumWindows([](HWND window, LPARAM value) -> BOOL {
        auto& state = *reinterpret_cast<Search*>(value);
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid != state.pid || !IsWindowVisible(window) || IsIconic(window)) return TRUE;
        wchar_t name[64]{};
        if (!GetClassNameW(window, name, 64) || std::wcscmp(name, L"UnityWndClass")) return TRUE;
        RECT client{};
        if (!GetClientRect(window, &client)) return TRUE;
        const int width = client.right-client.left, height = client.bottom-client.top;
        if (width < 16 || height < 16) return TRUE;
        const auto area = static_cast<std::uint64_t>(width)*static_cast<std::uint64_t>(height);
        if (window == state.foreground ||
            (state.best.window != state.foreground && area > state.area)) {
            state.best = {window,width,height}; state.area = area;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.best;
}
} // namespace ark::arkhelper
