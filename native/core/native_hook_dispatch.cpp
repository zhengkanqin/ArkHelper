// Native dispatch path: admission counting, chain traversal and draining.
//
// Every entry through a slot's gates is counted in the slot's admission word for as long as
// it may touch owner (business) code, so a remover can wait for in-flight callbacks before the
// owner's module is unloaded. The thread-local entry stack lets nested calls know which slots
// and chains the current thread is already inside.

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include <windows.h>

#include <string>

#include "native_hook_detail.h"

#if !defined(_M_X64) && !defined(__x86_64__)
#error NativeHooks requires the Windows x64 ABI.
#endif

namespace ark::core {
namespace {

// Zig's bundled MinGW libraries omit Synchronization.lib. Resolve these Win8+ APIs once from
// the resident system DLL, not on the native dispatch hot path.
const auto wait_on_address = reinterpret_cast<decltype(&WaitOnAddress)>(
    GetProcAddress(GetModuleHandleW(L"KernelBase.dll"), "WaitOnAddress"));
const auto wake_by_address_all = reinterpret_cast<decltype(&WakeByAddressAll)>(
    GetProcAddress(GetModuleHandleW(L"KernelBase.dll"), "WakeByAddressAll"));

// One counted entry on the current thread's native stack.
struct EntryFrame {
    NativeHookSlot* slot;
    EntryFrame* previous;
};

thread_local EntryFrame* current_entry = nullptr;

bool is_chain_entered_on_current_thread(const NativeHookChain* chain) {
    for (auto* entry = current_entry; entry; entry = entry->previous) {
        if (entry->slot->chain == chain) {
            return true;
        }
    }
    return false;
}

void leave(NativeHookSlot& slot) {
    const auto before = slot.admission.fetch_sub(kEntryUnit, std::memory_order_acq_rel);
    if ((before & ~kModeMask) == kEntryUnit) {
        wake_by_address_all(&slot.admission);
    }
}

// Calls `call` with the entry already admitted by the caller, recording it on the thread's
// entry stack and releasing admission on every exit path.
uintptr_t invoke_counted(NativeHookSlot* slot, NativeFunction call, uintptr_t a, uintptr_t b,
                         uintptr_t c, uintptr_t d) {
    EntryFrame frame{slot, current_entry};
    current_entry = &frame;
#if defined(_MSC_VER)
    // Also release admission during Windows structured exception unwinding.
    // This function deliberately has no C++ objects requiring destruction.
    __try {
        return call(a, b, c, d);
    } __finally {
        current_entry = frame.previous;
        leave(*slot);
    }
#else
    struct ExitGuard {
        NativeHookSlot* slot;
        EntryFrame* previous;

        ~ExitGuard() {
            current_entry = previous;
            leave(*slot);
        }
    } exit_guard{slot, frame.previous};

    return call(a, b, c, d);
#endif
}

uintptr_t dispatch(NativeHookSlot* slot, bool original, uintptr_t a, uintptr_t b, uintptr_t c,
                   uintptr_t d);

uintptr_t call_trampoline(const NativeHookChain* chain, uintptr_t a, uintptr_t b, uintptr_t c,
                          uintptr_t d) {
    return reinterpret_cast<NativeFunction>(chain->trampoline)(a, b, c, d);
}

// Continues below slot: the next older link, or the real target through the trampoline.
uintptr_t continue_chain(NativeHookSlot* slot, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    auto* next = slot->next.load(std::memory_order_acquire);
    if (next) {
        return dispatch(next, false, a, b, c, d);
    }
    return call_trampoline(slot->chain, a, b, c, d);
}

// Counted pass-through for a link that is entered but not running its detour.
uintptr_t chain_gateway(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    return continue_chain(current_entry->slot, a, b, c, d);
}

uintptr_t dispatch_anchor(NativeHookSlot* anchor, uintptr_t a, uintptr_t b, uintptr_t c,
                          uintptr_t d) {
    auto* head = anchor->chain->head.load(std::memory_order_acquire);
    if (head) {
        return dispatch(head, false, a, b, c, d);
    }
    return call_trampoline(anchor->chain, a, b, c, d);
}

uintptr_t dispatch(NativeHookSlot* slot, bool original, uintptr_t a, uintptr_t b, uintptr_t c,
                   uintptr_t d) {
    if (slot->is_anchor) {
        return dispatch_anchor(slot, a, b, c, d);
    }
    const auto admitted = slot->admission.fetch_add(kEntryUnit, std::memory_order_acq_rel);
    const auto mode = static_cast<HookMode>(admitted & kModeMask);
    if (mode == HookMode::Retired) {
        // A retired original replayed outside a native callback follows the current
        // generation. Inside this target's chain it MUST stay below its own link, even if
        // removal sealed an already-admitted detour.
        const bool within_chain = is_chain_entered_on_current_thread(slot->chain);
        leave(*slot);
        if (original && !within_chain) {
            return reinterpret_cast<NativeFunction>(slot->target)(a, b, c, d);
        }
        return continue_chain(slot, a, b, c, d);
    }
    // Both business entry and original continuation are counted, so removing this owner
    // cannot race a callback that has selected its detour.
    if (!original && mode == HookMode::Enabled) {
        return invoke_counted(slot, reinterpret_cast<NativeFunction>(slot->detour), a, b, c, d);
    }
    return invoke_counted(slot, chain_gateway, a, b, c, d);
}

} // namespace

bool address_wait_available() {
    return wait_on_address && wake_by_address_all;
}

uintptr_t detour_gateway(NativeHookSlot* slot, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    return dispatch(slot, false, a, b, c, d);
}

uintptr_t original_gateway(NativeHookSlot* slot, uintptr_t a, uintptr_t b, uintptr_t c,
                           uintptr_t d) {
    return dispatch(slot, true, a, b, c, d);
}

bool is_entered_on_current_thread(const NativeHookSlot* slot) {
    for (auto* entry = current_entry; entry; entry = entry->previous) {
        if (entry->slot == slot) {
            return true;
        }
    }
    return false;
}

bool drain(NativeHookSlot& slot, uint32_t timeout_ms, std::string& error) {
    const auto deadline = HookClock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        auto observed = slot.admission.load(std::memory_order_acquire);
        if (!(observed & ~kModeMask)) {
            return true;
        }
        // Cross-hook waits from callbacks can form a cycle on other threads. Never block any
        // native callback on another callback's completion.
        if (current_entry) {
            error = "Cannot drain an active native hook from a native callback; retry outside "
                    "native callbacks";
            return false;
        }
        const auto remaining = remaining_ms(deadline);
        if (!remaining) {
            error = "Native hook drain timed out; removal remains pending and must be retried";
            return false;
        }
        if (!wait_on_address(&slot.admission, &observed, sizeof observed, remaining)) {
            const auto result = GetLastError();
            if (result != ERROR_TIMEOUT) {
                error = "Native hook WaitOnAddress failed: " + std::to_string(result);
                return false;
            }
        }
    }
}

} // namespace ark::core
