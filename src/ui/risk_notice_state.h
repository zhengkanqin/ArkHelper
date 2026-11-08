#pragma once
#include <cstdint>

namespace arkhelp::risk_notice {
inline constexpr int kNoticeVersion = 1;
inline constexpr std::uint64_t kReadingMs = 10000;

// Require one continuous foreground reading interval. Once completed, switching
// windows does not revoke it. A fresh window always starts a new interval.
class ReadingGate {
public:
    void observe(std::uint64_t now, bool reading) {
        if (ready_) return;
        if (!reading) { started_ = false; elapsed_ = 0; return; }
        if (!started_ || now < since_) { started_ = true; since_ = now; }
        elapsed_ = now - since_;
        ready_ = elapsed_ > kReadingMs;
    }
    bool can_accept(bool checked) const { return ready_ && checked; }
    unsigned seconds_remaining() const {
        if (ready_) return 0;
        const auto seconds = static_cast<unsigned>((kReadingMs - elapsed_ + 999) / 1000);
        return seconds ? seconds : 1;
    }
private:
    bool started_ = false, ready_ = false;
    std::uint64_t since_ = 0, elapsed_ = 0;
};
}
