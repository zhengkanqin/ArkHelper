// Legacy adapter exists only for isolated lifecycle fixtures. The product has
// an external desktop UI and must never intercept Unity input.
#if defined(ARKHELP_ENABLE_TEST_MODULES)
#include <algorithm>

#include "plugin_manager.h"
#include "plugin_support.h"

namespace ark::core {

using namespace plugin_detail;

namespace {

// Depth of input adapter callbacks on the current thread.
thread_local unsigned input_callback_depth = 0;

struct InputCallbackScope {
    InputCallbackScope() { ++input_callback_depth; }

    ~InputCallbackScope() { --input_callback_depth; }

    InputCallbackScope(const InputCallbackScope&) = delete;
    InputCallbackScope& operator=(const InputCallbackScope&) = delete;
};

bool is_valid_adapter(const ArkInputAdapterV1& adapter) {
    return adapter.size >= sizeof(ArkInputAdapterV1) && adapter.set_capture &&
           adapter.route_message && adapter.protected_input;
}

} // namespace

// Holds both the adapter registration and its owning plugin. Constructed with
// the adapter mutex held; member order makes the plugin lease outlive the
// adapter count.
struct PluginManager::InputLease {
    std::shared_ptr<InputAdapter> registration;
    Invocation invocation;

    explicit InputLease(std::shared_ptr<InputAdapter> adapter)
        : registration(std::move(adapter)),
          invocation(registration->owner, true) {
        if (invocation) {
            ++registration->in_flight;
        }
    }

    ~InputLease() {
        if (!invocation) {
            return;
        }
        std::lock_guard lock(registration->mutex);
        if (--registration->in_flight == 0) {
            registration->drained.notify_all();
        }
    }

    InputLease(const InputLease&) = delete;
    InputLease& operator=(const InputLease&) = delete;
};

bool PluginManager::set_input_adapter(void* owner, const ArkInputAdapterV1* adapter) {
    if (adapter && !is_valid_adapter(*adapter)) {
        return false;
    }
    auto plugin = find_plugin(owner);
    if (!plugin) {
        return false;
    }

    std::shared_ptr<InputAdapter> previous;
    {
        std::lock_guard lock(mutex_);
        for (auto& item : input_adapters_) {
            if (item->owner == plugin) {
                previous = item;
                break;
            }
        }
        if (previous) {
            std::lock_guard adapter_lock(previous->mutex);
            // Never wait on this or another adapter from inside an adapter:
            // reciprocal removals could otherwise deadlock two input threads.
            if (previous->in_flight && input_callback_depth) {
                return false;
            }
            previous->accepting = false;
        }
    }

    if (previous) {
        std::unique_lock lock(previous->mutex);
        const bool drained = previous->drained.wait_for(lock, kRundownTimeout,
                                                        [&] { return previous->in_flight == 0; });
        if (!drained) {
            std::lock_guard owner_lock(plugin->mutex);
            plugin->error = "Input adapter release timed out; DLL retained";
            return false;
        }
    }

    std::lock_guard lock(mutex_);
    // A concurrent registration must not be overwritten by this rundown.
    for (auto& item : input_adapters_) {
        if (item->owner == plugin && item != previous) {
            return false;
        }
    }
    if (previous) {
        std::erase(input_adapters_, previous);
    }
    if (!adapter) {
        return true;
    }
    std::lock_guard owner_lock(plugin->mutex);
    if (!accepts_registrations(*plugin)) {
        return false;
    }
    auto registration = std::make_shared<InputAdapter>();
    registration->owner = plugin;
    registration->table = *adapter;
    input_adapters_.push_back(std::move(registration));
    return true;
}

std::vector<std::shared_ptr<PluginManager::InputAdapter>> PluginManager::input_snapshot() {
    std::lock_guard lock(mutex_);
    return input_adapters_;
}

std::unique_ptr<PluginManager::InputLease>
PluginManager::acquire_input_lease(const std::shared_ptr<InputAdapter>& adapter) {
    std::lock_guard lock(mutex_);
    if (std::find(input_adapters_.begin(), input_adapters_.end(), adapter) ==
        input_adapters_.end()) {
        return {};
    }
    std::lock_guard adapter_lock(adapter->mutex);
    if (!adapter->accepting) {
        return {};
    }
    auto lease = std::make_unique<InputLease>(adapter);
    if (!lease->invocation) {
        return {};
    }
    return lease;
}

template <typename Call>
void PluginManager::for_each_input_adapter(const char* failure_message, Call&& call) {
    for (auto& item : input_snapshot()) {
        auto lease = acquire_input_lease(item);
        if (!lease) {
            continue;
        }
        InputCallbackScope scope;
        try {
            call(item->table);
        } catch (...) {
            log(kLogError, failure_message + lease->invocation.name());
        }
    }
}

void PluginManager::set_input_capture(bool mouse, bool keyboard) {
    for_each_input_adapter("Input capture adapter threw: ", [&](const ArkInputAdapterV1& adapter) {
        adapter.set_capture(adapter.user, mouse, keyboard);
    });
}

bool PluginManager::route_window_input(uint32_t message, uintptr_t wparam, intptr_t lparam) {
    // Every adapter sees every message; there is no short circuit on "handled".
    bool handled = false;
    for_each_input_adapter("Input routing adapter threw: ", [&](const ArkInputAdapterV1& adapter) {
        handled = adapter.route_message(adapter.user, message, wparam, lparam) || handled;
    });
    return handled;
}

bool PluginManager::input_protected() {
    bool protected_input = false;
    for_each_input_adapter(
        "Input protection adapter threw: ", [&](const ArkInputAdapterV1& adapter) {
            protected_input = adapter.protected_input(adapter.user) || protected_input;
        });
    return protected_input;
}

bool PluginManager::has_input_adapter() {
    for (auto& item : input_snapshot()) {
        if (acquire_input_lease(item)) {
            return true;
        }
    }
    return false;
}

} // namespace ark::core
#endif
