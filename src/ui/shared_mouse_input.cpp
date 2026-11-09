#include "shared_mouse_input.h"
#include "mouse_relay.h"
#include <algorithm>
#include <vector>

namespace arkhelp {

// One process-wide input observer, shared by panels and drawing features.
// Observes input without suppressing native messages or forwarding them to the game.
struct MouseInputBroker {
    std::mutex lifecycle, subscribers_mutex;
    std::vector<SharedMouseInput*> subscribers;
    HANDLE worker = nullptr, ready = nullptr;
    DWORD thread_id = 0, error = ERROR_SUCCESS;
    HWND window = nullptr;
    WORD buttons = 0;
    bool shifts[2]{}, controls[2]{};
    bool motion_pending = false;
    bool native_only = false, remote_active = false;
    MouseRelayReader relay;
    HHOOK hook=nullptr;
    static thread_local MouseInputBroker* current;

    static LRESULT CALLBACK observe(int code,WPARAM message,LPARAM data) {
        auto* self=current;
        if(code==HC_ACTION && self && !self->remote_active &&
            (message==WM_LBUTTONDOWN || message==WM_LBUTTONUP || message==WM_MOUSEMOVE ||
             message==WM_MOUSEWHEEL || message==WM_MOUSEHWHEEL)) {
            const auto& input=*reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
            if(message==WM_LBUTTONDOWN)self->buttons|=MK_LBUTTON;
            if(message==WM_LBUTTONUP)self->buttons&=~MK_LBUTTON;
            self->shifts[0]=(GetAsyncKeyState(VK_SHIFT)&0x8000)!=0;
            self->controls[0]=(GetAsyncKeyState(VK_CONTROL)&0x8000)!=0;
            try {self->emit(static_cast<UINT>(message),input.pt,
                (message==WM_MOUSEWHEEL || message==WM_MOUSEHWHEEL)?HIWORD(input.mouseData):0);}
            catch(...){}
        }
        return CallNextHookEx(nullptr,code,message,data);
    }

    bool attach(SharedMouseInput* client, bool local) {
        std::lock_guard guard(lifecycle);
        if (!worker) {
            native_only=local;
            ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!ready) return false;
            worker = CreateThread(nullptr, 0, run, this, 0, &thread_id);
            if (!worker) { CloseHandle(ready); ready = nullptr; return false; }
            WaitForSingleObject(ready, INFINITE);
            CloseHandle(ready); ready = nullptr;
            if (error) {
                WaitForSingleObject(worker, INFINITE);
                CloseHandle(worker); worker = nullptr;
                SetLastError(error); return false;
            }
        }
        std::lock_guard lock(subscribers_mutex);
        subscribers.push_back(client);
        return true;
    }
    void detach(SharedMouseInput* client) {
        std::lock_guard guard(lifecycle);
        {
            std::lock_guard lock(subscribers_mutex);
            std::erase(subscribers, client);
            if (!subscribers.empty()) return;
        }
        if (worker) {
            PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
            WaitForSingleObject(worker, INFINITE);
            CloseHandle(worker); worker = nullptr;
        }
    }
    void deliver(const SharedMouseEvent& event) {
        std::lock_guard lock(subscribers_mutex);
        for (auto* client : subscribers) {
            client->left_down_.store(event.message!=WM_CANCELMODE && (event.keys & MK_LBUTTON)!=0, std::memory_order_relaxed);
            try { client->enqueue(event); } catch (...) {}
        }
    }
    void emit(UINT message, POINT point, WORD delta = 0) {
        WORD keys = buttons;
        if (shifts[0] || shifts[1]) keys |= MK_SHIFT;
        if (controls[0] || controls[1]) keys |= MK_CONTROL;
        const SharedMouseEvent event{message, point, MAKEWPARAM(keys, delta),
            GetTickCount64(), GetAncestor(GetForegroundWindow(), GA_ROOT)};
        deliver(event);
    }
    void input(HRAWINPUT handle) {
        RAWINPUT raw{};
        UINT size = sizeof(raw);
        if (GetRawInputData(handle, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) return;
        if(remote_active || hook)return;
        if (raw.header.dwType == RIM_TYPEKEYBOARD) {
            const auto& key = raw.data.keyboard;
            const bool down = !(key.Flags & RI_KEY_BREAK);
            if (key.VKey == VK_SHIFT) shifts[key.MakeCode == 0x36] = down;
            if (key.VKey == VK_CONTROL) controls[(key.Flags & RI_KEY_E0) != 0] = down;
            return;
        }
        if (raw.header.dwType != RIM_TYPEMOUSE) return;
        const auto flags = raw.data.mouse.usButtonFlags;
        const DWORD packed = GetMessagePos();
        const POINT point{static_cast<SHORT>(LOWORD(packed)), static_cast<SHORT>(HIWORD(packed))};
        if (raw.data.mouse.lLastX || raw.data.mouse.lLastY ||
            (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) motion_pending = true;
        // Flush motion at a button boundary to preserve ordering. For motion alone,
        // sample on the worker timer after Windows has updated the desktop cursor.
        if (flags && motion_pending) { emit(WM_MOUSEMOVE, point); motion_pending = false; }
        if (flags & RI_MOUSE_RIGHT_BUTTON_DOWN) buttons |= MK_RBUTTON;
        if (flags & RI_MOUSE_RIGHT_BUTTON_UP) buttons &= ~MK_RBUTTON;
        if (flags & RI_MOUSE_LEFT_BUTTON_DOWN) { buttons |= MK_LBUTTON; emit(WM_LBUTTONDOWN, point); }
        if (flags & RI_MOUSE_LEFT_BUTTON_UP) { buttons &= ~MK_LBUTTON; emit(WM_LBUTTONUP, point); }
        if (flags & RI_MOUSE_WHEEL) emit(WM_MOUSEWHEEL, point, raw.data.mouse.usButtonData);
        if (flags & RI_MOUSE_HWHEEL) emit(WM_MOUSEHWHEEL, point, raw.data.mouse.usButtonData);
    }
    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<MouseInputBroker*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<MouseInputBroker*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (message == WM_INPUT && self) self->input(reinterpret_cast<HRAWINPUT>(lparam));
        if (message == WM_TIMER && self) {
            if(!self->native_only) {
                const bool remote=self->relay.poll(GetTickCount64(),[&](const SharedMouseEvent& event){self->deliver(event);});
                if(remote!=self->remote_active) {
                    self->buttons=0;self->motion_pending=false;
                    self->shifts[0]=self->shifts[1]=self->controls[0]=self->controls[1]=false;
                    if(!remote)self->deliver(SharedMouseEvent{WM_CANCELMODE});
                }
                self->remote_active=remote;
            }
            if(!self->remote_active && self->motion_pending) {
                POINT point{};
                if (GetCursorPos(&point)) self->emit(WM_MOUSEMOVE, point);
                self->motion_pending = false;
            }
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    static DWORD WINAPI run(void* context) {
        auto& self = *static_cast<MouseInputBroker*>(context);
        current=&self;
        self.error = ERROR_SUCCESS; self.buttons = 0;
        self.remote_active = false;
        self.motion_pending = false;
        self.shifts[0] = self.shifts[1] = self.controls[0] = self.controls[1] = false;
        WNDCLASSW klass{};
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpfnWndProc = procedure;
        klass.lpszClassName = L"ArkHelper.RawMouse";
        RegisterClassW(&klass);
        self.window = CreateWindowExW(0, klass.lpszClassName, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, klass.hInstance, &self);
        RAWINPUTDEVICE devices[]{{0x01, 0x02, RIDEV_INPUTSINK, self.window},
                                 {0x01, 0x06, RIDEV_INPUTSINK, self.window}};
        self.hook=SetWindowsHookExW(WH_MOUSE_LL,observe,GetModuleHandleW(nullptr),0);
        const bool raw_registered=!self.hook && self.window && RegisterRawInputDevices(devices,2,sizeof(RAWINPUTDEVICE));
        if (!self.window || (!self.hook && !raw_registered))
            self.error = GetLastError();
        SetEvent(self.ready);
        if (!self.error) {
            SetTimer(self.window, 1, 10, nullptr);
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            for (auto& device : devices) { device.dwFlags = RIDEV_REMOVE; device.hwndTarget = nullptr; }
            if(raw_registered)RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE));
            KillTimer(self.window, 1);
        }
        if (self.window) DestroyWindow(self.window);
        if(self.hook){UnhookWindowsHookEx(self.hook);self.hook=nullptr;}
        current=nullptr;
        self.relay.close();
        self.window = nullptr;
        return 0;
    }
};
thread_local MouseInputBroker* MouseInputBroker::current=nullptr;

static MouseInputBroker& broker() {
    // Clients in other translation units can outlive this unit's static destructors.
    static auto* instance = new MouseInputBroker;
    return *instance;
}

bool SharedMouseInput::start(HWND recipient, UINT notification, bool native_only) {
    if (registered_) return true;
    recipient_ = recipient; notification_ = notification;
    registered_ = broker().attach(this,native_only);
    return registered_;
}

void SharedMouseInput::stop() {
    if (registered_) { broker().detach(this); registered_ = false; }
    std::lock_guard lock(mutex_);
    pending_.clear(); notified_ = false;
    left_down_.store(false, std::memory_order_relaxed);
}

void SharedMouseInput::enqueue(SharedMouseEvent event) {
    std::lock_guard lock(mutex_);
    if (event.message == WM_MOUSEMOVE && !pending_.empty() && pending_.back().message == WM_MOUSEMOVE) {
        pending_.back() = event;
    } else {
        if (pending_.size() >= 256) {
            pending_.clear(); pending_.push_back({WM_CANCELMODE});
        }
        pending_.push_back(event);
    }
    if (!notified_) notified_ = PostMessageW(recipient_, notification_, 0, 0) != FALSE;
}

std::deque<SharedMouseEvent> SharedMouseInput::take() {
    std::lock_guard lock(mutex_);
    std::deque<SharedMouseEvent> events;
    events.swap(pending_); notified_ = false;
    return events;
}

}  // namespace arkhelp
