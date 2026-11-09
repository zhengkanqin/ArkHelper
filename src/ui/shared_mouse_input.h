#pragma once

#include <windows.h>
#include <deque>
#include <mutex>
#include <atomic>

namespace arkhelp {

struct SharedMouseEvent {
    UINT message = 0;
    POINT screen{};
    WPARAM keys = 0;
    ULONGLONG time = 0;
    HWND foreground = nullptr;
};

// Observes native input in this process. It never consumes input or sends it to another application.
class SharedMouseInput {
public:
    ~SharedMouseInput() { stop(); }
    bool start(HWND recipient, UINT notification, bool native_only = false);
    void stop();
    std::deque<SharedMouseEvent> take();
    bool running() const { return registered_; }
    bool left_down() const { return left_down_.load(std::memory_order_relaxed); }

private:
    friend struct MouseInputBroker;
    void enqueue(SharedMouseEvent event);

    bool registered_ = false;
    HWND recipient_ = nullptr;
    UINT notification_ = 0;
    bool notified_ = false;
    std::atomic<bool> left_down_{false};
    std::mutex mutex_;
    std::deque<SharedMouseEvent> pending_;
};

}  // namespace arkhelp
