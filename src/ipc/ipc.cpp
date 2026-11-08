#include "arkhelp/ipc.h"

#include <atomic>
#include <cstring>

namespace arkhelp::ipc {
namespace {

// The sequence word lives in shared memory, so both sides touch it through atomics even though it
// is declared as a plain uint32_t in the POD struct.
std::atomic<std::uint32_t>& as_atomic(std::uint32_t& value) noexcept {
    static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t),
                  "shared sequence word must remain lock-free");
    return *reinterpret_cast<std::atomic<std::uint32_t>*>(&value);
}

}  // namespace

void begin_write(std::uint32_t& sequence) noexcept {
    // Odd sequence means "write in progress".
    as_atomic(sequence).fetch_add(1, std::memory_order_acq_rel);
}

void end_write(std::uint32_t& sequence) noexcept {
    as_atomic(sequence).fetch_add(1, std::memory_order_release);
}

bool read_snapshot(const BattleSnapshot& state, BattleSnapshot& out,
                   std::uint32_t max_attempts) noexcept {
    std::uint32_t attempts = 0;
    for (;;) {
        const std::uint32_t first = as_atomic(const_cast<std::uint32_t&>(state.sequence))
                                        .load(std::memory_order_acquire);
        if ((first & 1u) == 0u) {
            std::memcpy(&out, &state, sizeof(BattleSnapshot));
            std::atomic_thread_fence(std::memory_order_acquire);
            const std::uint32_t second = as_atomic(const_cast<std::uint32_t&>(state.sequence))
                                             .load(std::memory_order_acquire);
            if (first == second) {
                return true;
            }
        }
        if (++attempts >= max_attempts) {
            return false;
        }
    }
}

bool read_commands(const CommandState& state, CommandState& out,
                   std::uint32_t max_attempts) noexcept {
    std::uint32_t attempts = 0;
    for (;;) {
        const std::uint32_t first = as_atomic(const_cast<std::uint32_t&>(state.sequence))
                                        .load(std::memory_order_acquire);
        if ((first & 1u) == 0u) {
            std::memcpy(&out, &state, sizeof(CommandState));
            std::atomic_thread_fence(std::memory_order_acquire);
            const std::uint32_t second = as_atomic(const_cast<std::uint32_t&>(state.sequence))
                                             .load(std::memory_order_acquire);
            if (first == second) {
                return true;
            }
        }
        if (++attempts >= max_attempts) {
            return false;
        }
    }
}

}  // namespace arkhelp::ipc
