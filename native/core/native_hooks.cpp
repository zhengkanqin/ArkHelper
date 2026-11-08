#include "native_hooks.h"

#include <windows.h>

#include <chrono>
#include <exception>
#include <memory>
#include <utility>
#include <vector>

#include <MinHook.h>

#include "native_hook_detail.h"

// Lock order: NativeHookSlot::operation_mutex, then NativeHooks::mutex_, then the slot arena's
// internal mutex. No lock is held while draining.

namespace ark::core {
namespace {

std::string minhook_error(const char* operation, MH_STATUS status) {
    return std::string(operation) + ": " + MH_StatusToString(status);
}

// Requires NativeHooks::mutex_. Unpatches the target when slot holds its last enabled link.
// The caller adjusts enabled_links and the slot mode afterwards.
bool disable_target_if_last_link(const NativeHookSlot& slot, std::string& error) {
    if (slot.chain->enabled_links != 1) {
        return true;
    }
    const auto status = MH_DisableHook(slot.target);
    if (status != MH_OK && status != MH_ERROR_DISABLED) {
        error = minhook_error("MH_DisableHook", status);
        return false;
    }
    return true;
}

// Requires NativeHooks::mutex_. Removes a drained slot from its chain. Native entrants may
// still hold the slot and follow its (unchanged) next pointer downward.
void unlink_from_chain(NativeHookSlot& slot) {
    NativeHookSlot* previous = nullptr;
    auto* current = slot.chain->head.load(std::memory_order_acquire);
    while (current && current != &slot) {
        previous = current;
        current = current->next.load(std::memory_order_acquire);
    }
    if (!current) {
        return;
    }
    auto* next = slot.next.load(std::memory_order_acquire);
    if (previous) {
        previous->next.store(next, std::memory_order_release);
    } else {
        slot.chain->head.store(next, std::memory_order_release);
    }
}

} // namespace

NativeHookSlot* NativeHooks::find(void* owner, uint64_t id, std::string& error) {
    std::lock_guard lock(mutex_);
    const auto found = hooks_.find(id);
    if (!owner || found == hooks_.end() || found->second.owner != owner) {
        error = "Native hook ID is unknown or belongs to another owner";
        return nullptr;
    }
    return found->second.slot;
}

// Requires mutex_. Exceptions propagate to create().
NativeHookChain* NativeHooks::find_or_create_chain(void* target, std::string& error) {
    if (const auto found = chains_.find(target); found != chains_.end()) {
        return found->second;
    }
    auto chain = std::make_unique<NativeHookChain>();
    auto* anchor = allocate_hook_slot(target, error);
    if (!anchor) {
        return nullptr;
    }
    anchor->chain = chain.get();
    anchor->is_anchor = true;
    // All allocations precede publication to MinHook. The chain record, anchor and trampoline
    // then stay resident with the core.
    const auto position = chains_.emplace(target, chain.get()).first;
    const auto status = MH_CreateHook(target, anchor->detour_gate, &chain->trampoline);
    if (status != MH_OK) {
        chains_.erase(position);
        error = minhook_error("MH_CreateHook", status);
        return nullptr;
    }
    return chain.release();
}

uint64_t NativeHooks::create(void* owner, void* target, void* detour, void** original,
                             std::string& error) {
    error.clear();
    if (original) {
        *original = nullptr;
    }
    if (!owner || !target || !detour || !original) {
        error = "Native hook creation requires owner, target, detour and original output";
        return 0;
    }
    if (!address_wait_available()) {
        error = "Windows address-wait APIs are unavailable";
        return 0;
    }
    try {
        auto* slot = allocate_hook_slot(target, error);
        if (!slot) {
            return 0;
        }
        std::lock_guard lock(mutex_);
        auto* chain = find_or_create_chain(target, error);
        if (!chain) {
            return 0;
        }
        slot->chain = chain;
        hooks_.emplace(slot->id, OwnedHook{owner, slot});
        slot->owner = owner;
        slot->detour = detour;
        // Publish as the new head, linked above the previous newest link.
        slot->next.store(chain->head.load(std::memory_order_relaxed), std::memory_order_relaxed);
        slot->set_mode(HookMode::Disabled);
        chain->head.store(slot, std::memory_order_release);
        *original = slot->original_gate;
        return slot->id;
    } catch (const std::exception& exception) {
        // Unpublished slots are consumed, never reused by another owner.
        error = std::string("Native hook allocation failed: ") + exception.what();
        return 0;
    }
}

bool NativeHooks::enable(void* owner, uint64_t id, bool enabled, std::string& error) {
    error.clear();
    auto* slot = find(owner, id, error);
    if (!slot) {
        return false;
    }
    std::lock_guard operation(slot->operation_mutex);
    if (slot->removed || slot->owner != owner || slot->removal_pending) {
        error = "Native hook is removed or pending removal";
        return false;
    }
    std::lock_guard lock(mutex_);
    if ((slot->mode() == HookMode::Enabled) == enabled) {
        return true;
    }
    if (!enabled) {
        if (!disable_target_if_last_link(*slot, error)) {
            return false;
        }
        slot->set_mode(HookMode::Disabled);
        --slot->chain->enabled_links;
        return true;
    }
    // Publish before patching so redirected native threads see a valid link.
    slot->set_mode(HookMode::Enabled);
    if (!slot->chain->enabled_links) {
        const auto status = MH_EnableHook(slot->target);
        if (status != MH_OK && status != MH_ERROR_ENABLED) {
            slot->set_mode(HookMode::Disabled);
            error = minhook_error("MH_EnableHook", status);
            return false;
        }
    }
    ++slot->chain->enabled_links;
    return true;
}

bool NativeHooks::remove(void* owner, uint64_t id, uint32_t timeout_ms, std::string& error) {
    error.clear();
    auto* slot = find(owner, id, error);
    if (!slot) {
        return false;
    }
    if (is_entered_on_current_thread(slot)) {
        error = "Cannot remove a native hook from its own entry; retry after returning";
        return false;
    }

    // Phase 1: seal. Late chain entries only continue downward from here on.
    {
        std::lock_guard operation(slot->operation_mutex);
        if (slot->removed) {
            // A concurrent removal already succeeded.
            return true;
        }
        if (slot->owner != owner) {
            error = "Native hook belongs to another owner";
            return false;
        }
        if (!slot->removal_pending) {
            std::lock_guard lock(mutex_);
            if (slot->mode() == HookMode::Enabled) {
                if (!disable_target_if_last_link(*slot, error)) {
                    return false;
                }
                --slot->chain->enabled_links;
            }
            slot->set_mode(HookMode::Retired);
            slot->removal_pending = true;
        }
    }

    // Phase 2: drain without holding the registry or operation mutex. A timeout leaves the
    // sealed slot and the native record intact for a retry.
    if (!drain(*slot, timeout_ms, error)) {
        return false;
    }

    // Phase 3: retire. The target's core-owned MinHook record and trampoline are kept
    // permanently: besides late gates, a relocated CALL may still have a return PC inside the
    // trampoline after the final owner unloads. No business pointer lives in that record.
    std::lock_guard operation(slot->operation_mutex);
    if (slot->removed) {
        return true;
    }
    slot->detour = nullptr;
    slot->owner = nullptr;
    slot->removed = true;
    {
        std::lock_guard lock(mutex_);
        unlink_from_chain(*slot);
        hooks_.erase(id);
    }
    return true;
}

bool NativeHooks::detach(void* owner, uint32_t timeout_ms, std::string& error) {
    error.clear();
    if (!owner) {
        error = "Native hook detach requires an owner";
        return false;
    }
    const auto deadline = HookClock::now() + std::chrono::milliseconds(timeout_ms);
    std::vector<std::pair<uint64_t, NativeHookSlot*>> owned;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [id, hook] : hooks_) {
            if (hook.owner == owner) {
                owned.emplace_back(id, hook.slot);
            }
        }
    }
    // Refuse before changing any hook if this owner is on the current native stack.
    for (const auto& [id, slot] : owned) {
        if (is_entered_on_current_thread(slot)) {
            error = "Cannot detach an owner from its own native entry; retry after returning";
            return false;
        }
    }
    bool result = true;
    for (const auto& [id, slot] : owned) {
        std::string failure;
        if (remove(owner, id, remaining_ms(deadline), failure)) {
            continue;
        }
        // A concurrent removal may have completed since the snapshot.
        std::lock_guard operation(slot->operation_mutex);
        if (slot->removed) {
            continue;
        }
        result = false;
        if (!error.empty()) {
            error += "; ";
        }
        error += "hook " + std::to_string(id) + ": " + failure;
    }
    return result;
}

} // namespace ark::core
