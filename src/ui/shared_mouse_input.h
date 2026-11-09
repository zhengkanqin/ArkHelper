#pragma once

#include <windows.h>
#include <deque>
#include <mutex>

namespace arkhelp {

struct SharedMouseEvent {
    UINT message = 0;
    POINT screen{};
    WPARAM keys = 0;
};

// Observes native input in this process. It never consumes input or sends it to another application.
class SharedMouseInput {
public:
    ~SharedMouseInput() { stop(); }
    bool start(HWND recipient, UINT notification);
    void stop();
    std::deque<SharedMouseEvent> take();
    bool running() const { return worker_ != nullptr; }

private:
    static DWORD WINAPI run(void* context);
    static LRESULT CALLBACK hook(int code, WPARAM message, LPARAM data);
    void enqueue(SharedMouseEvent event);

    HANDLE worker_ = nullptr, ready_ = nullptr;
    DWORD thread_id_ = 0;
    HHOOK hook_ = nullptr;
    HWND recipient_ = nullptr;
    UINT notification_ = 0;
    bool notified_ = false;
    bool left_down_ = false;
    std::mutex mutex_;
    std::deque<SharedMouseEvent> pending_;
    static thread_local SharedMouseInput* current_;
};

}  // namespace arkhelp
