#pragma once

// Internal data model shared by the NativeHooks registry, the native dispatch path and the
// executable gate arena. Not part of any public interface.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>

namespace ark::core {

// Admission word layout: bits 0-1 hold the HookMode, the remaining bits count in-flight
// entries in units of kEntryUnit. Mode and admission share one atomic so a remover cannot
// miss an entrant that has selected a business detour or trampoline but has not yet loaded
// its pointer.
inline constexpr uint64_t kModeMask = 3;
inline constexpr uint64_t kEntryUnit = 4;

enum class HookMode : uint64_t {
    Disabled = 0,
    Enabled = 1,
    // Value 2 is unused.
    Retired = 3,
};

static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<uint64_t>) == sizeof(uint64_t));

using NativeFunction = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

struct NativeHookSlot;

// One per hooked target; process-resident once published.
struct NativeHookChain {
    std::atomic<NativeHookSlot*> head{nullptr};
    // Immutable, process-resident MinHook allocation.
    void* trampoline = nullptr;
    // Number of enabled links. Protected by NativeHooks::mutex_, which also serializes the
    // MinHook patch operations.
    size_t enabled_links = 0;
};

// One link in a target's chain, or the target's anchor. Slots and their gates have process
// lifetime and are never reassigned to a different hook generation.
struct NativeHookSlot {
    std::atomic<uint64_t> admission{static_cast<uint64_t>(HookMode::Retired)};
    std::mutex operation_mutex;
    uint64_t id = 0;
    // Protected by operation_mutex; cleared on retirement.
    void* owner = nullptr;
    // Immutable once allocated; process-resident.
    void* target = nullptr;
    // Protected by the admission count; cleared after draining.
    void* detour = nullptr;
    NativeHookChain* chain = nullptr;
    std::atomic<NativeHookSlot*> next{nullptr};
    // The anchor is MinHook's detour for its target and only forwards to the current head.
    bool is_anchor = false;
    // Generated thunks that enter dispatch for this slot, as detour or as original.
    void* detour_gate = nullptr;
    void* original_gate = nullptr;
    bool removal_pending = false;
    bool removed = false;

    HookMode mode() const {
        return static_cast<HookMode>(admission.load(std::memory_order_acquire) & kModeMask);
    }

    void set_mode(HookMode mode) {
        auto observed = admission.load(std::memory_order_acquire);
        while (!admission.compare_exchange_weak(
            observed, (observed & ~kModeMask) | static_cast<uint64_t>(mode),
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        }
    }
};

using HookClock = std::chrono::steady_clock;

// Milliseconds left until deadline, rounded up so a sub-millisecond remainder does not
// expire prematurely, and capped below INFINITE (UINT32_MAX) so it stays a finite wait.
inline uint32_t remaining_ms(HookClock::time_point deadline) {
    constexpr uint32_t kMaxFiniteWaitMs = std::numeric_limits<uint32_t>::max() - 1;
    const auto remaining = deadline - HookClock::now();
    if (remaining <= HookClock::duration::zero()) {
        return 0;
    }
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
    return static_cast<uint32_t>((std::min)(ms, static_cast<decltype(ms)>(kMaxFiniteWaitMs)));
}

// native_hook_dispatch.cpp ------------------------------------------------------------------

// True when the Win8+ address-wait APIs were resolved at load time.
bool address_wait_available();

// Entry points called by the generated gates with the gate's slot prepended.
uintptr_t detour_gateway(NativeHookSlot* slot, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d);
uintptr_t original_gateway(NativeHookSlot* slot, uintptr_t a, uintptr_t b, uintptr_t c,
                           uintptr_t d);

// True when the calling thread is currently inside a counted entry of slot.
bool is_entered_on_current_thread(const NativeHookSlot* slot);

// Waits until no entry of slot is in flight. Never blocks from inside a native callback.
bool drain(NativeHookSlot& slot, uint32_t timeout_ms, std::string& error);

// native_hook_arena.cpp ---------------------------------------------------------------------

// Hands out a fresh slot with a unique id and its own detour/original gates.
NativeHookSlot* allocate_hook_slot(void* target, std::string& error);

} // namespace ark::core
