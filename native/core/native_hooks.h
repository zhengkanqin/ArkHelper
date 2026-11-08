#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ark::core {

struct NativeHookChain;
struct NativeHookSlot;

// Owner-scoped native function hooks layered on MinHook (Windows 8+, x64 only).
//
// Lifetime contract:
// - The core module and the MinHook instance stay mapped and initialized until process exit,
//   and every hooked target must be process-resident.
// - Each target permanently retains one core-owned anchor, MinHook record and trampoline.
//   Each hook generation retains only small core-owned gates and a slot, never a reference
//   into the owner's (business) module.
// - Links run newest to oldest. A hook's original continues below its own link and never
//   passes through links created later.
class NativeHooks {
public:
    NativeHooks() = default;
    NativeHooks(const NativeHooks&) = delete;
    NativeHooks& operator=(const NativeHooks&) = delete;

    // Returns a non-zero hook id and stores the callable original in *original, or returns 0
    // and sets error. A new hook starts disabled.
    uint64_t create(void* owner, void* target, void* detour, void** original, std::string& error);

    // Disabling only bypasses this owner's link; other owners on the same target keep running.
    bool enable(void* owner, uint64_t id, bool enabled, std::string& error);

    // timeout_ms bounds draining in-flight callbacks, not MinHook's native patch operation.
    // A failed drain leaves a sealed, retryable removal: never destroy callback state or unload
    // the owner's module until removal succeeds. Removing a hook from inside its own callback
    // fails immediately.
    bool remove(void* owner, uint64_t id, uint32_t timeout_ms, std::string& error);

    // Removes every hook of owner within one shared deadline. The host must forbid new hook
    // creation for this owner before detaching.
    bool detach(void* owner, uint32_t timeout_ms, std::string& error);

private:
    struct OwnedHook {
        void* owner;
        NativeHookSlot* slot;
    };

    NativeHookSlot* find(void* owner, uint64_t id, std::string& error);
    NativeHookChain* find_or_create_chain(void* target, std::string& error);

    std::mutex mutex_;
    std::unordered_map<uint64_t, OwnedHook> hooks_;
    std::unordered_map<void*, NativeHookChain*> chains_;
};

} // namespace ark::core
