#include "shared_mouse_input.h"

namespace arkhelp {

thread_local SharedMouseInput* SharedMouseInput::current_ = nullptr;

bool SharedMouseInput::start(HWND recipient, UINT notification) {
    if (worker_) return true;
    recipient_ = recipient;
    notification_ = notification;
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready_) return false;
    worker_ = CreateThread(nullptr, 0, run, this, 0, &thread_id_);
    if (!worker_) { CloseHandle(ready_); ready_ = nullptr; return false; }
    WaitForSingleObject(ready_, INFINITE);
    CloseHandle(ready_);
    ready_ = nullptr;
    if (!hook_) { stop(); return false; }
    return true;
}

void SharedMouseInput::stop() {
    if (worker_) {
        PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
        WaitForSingleObject(worker_, INFINITE);
        CloseHandle(worker_);
        worker_ = nullptr;
    }
    std::lock_guard lock(mutex_);
    pending_.clear();
    notified_ = false;
}

DWORD WINAPI SharedMouseInput::run(void* context) {
    auto& self = *static_cast<SharedMouseInput*>(context);
    current_ = &self;
    MSG message{};
    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
    self.left_down_ = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    self.hook_ = SetWindowsHookExW(WH_MOUSE_LL, hook, GetModuleHandleW(nullptr), 0);
    SetEvent(self.ready_);
    if (self.hook_) {
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        UnhookWindowsHookEx(self.hook_);
        self.hook_ = nullptr;
    }
    current_ = nullptr;
    return 0;
}

LRESULT CALLBACK SharedMouseInput::hook(int code, WPARAM message, LPARAM data) {
    if (code == HC_ACTION && current_ &&
        (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_MOUSEMOVE ||
         message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)) {
        auto& self = *current_;
        const auto& input = *reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
        if (message == WM_LBUTTONDOWN) self.left_down_ = true;
        if (message == WM_LBUTTONUP) self.left_down_ = false;
        WORD keys = self.left_down_ ? MK_LBUTTON : 0;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) keys |= MK_SHIFT;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) keys |= MK_CONTROL;
        if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) keys |= MK_RBUTTON;
        const WORD delta = (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) ? HIWORD(input.mouseData) : 0;
        try { self.enqueue({static_cast<UINT>(message), input.pt, MAKEWPARAM(keys, delta)}); }
        catch (...) {} // Native input must keep flowing even if allocating the observation queue fails.
    }
    return CallNextHookEx(nullptr, code, message, data);
}

void SharedMouseInput::enqueue(SharedMouseEvent event) {
    std::lock_guard lock(mutex_);
    // Coalesce motion without changing its order relative to button/wheel events.
    if (event.message == WM_MOUSEMOVE && !pending_.empty() && pending_.back().message == WM_MOUSEMOVE) {
        pending_.back() = event;
    } else {
        if (pending_.size() >= 256) {
            pending_.clear();
            pending_.push_back({WM_CANCELMODE});
        }
        pending_.push_back(event);
    }
    if (!notified_) notified_ = PostMessageW(recipient_, notification_, 0, 0) != FALSE;
}

std::deque<SharedMouseEvent> SharedMouseInput::take() {
    std::lock_guard lock(mutex_);
    std::deque<SharedMouseEvent> events;
    events.swap(pending_);
    notified_ = false;
    return events;
}

}  // namespace arkhelp
