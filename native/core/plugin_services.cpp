#include "plugin_manager.h"
#include "plugin_support.h"

namespace ark::core {

using namespace plugin_detail;

bool PluginManager::provide_service(void* owner, const char* name, uint32_t abi, const void* table,
                                    size_t size) {
    if (!name || !*name || !abi || !table || !size) {
        return false;
    }
    std::lock_guard lock(mutex_);
    for (auto& plugin : plugins_) {
        if (plugin.get() != owner) {
            continue;
        }
        std::lock_guard owner_lock(plugin->mutex);
        if (!accepts_registrations(*plugin)) {
            return false;
        }
        for (auto& service : services_) {
            if (service.name == name) {
                return false;
            }
        }
        services_.push_back({plugin, name, abi, table, size});
        return true;
    }
    return false;
}

const void* PluginManager::require_service(void* owner, const char* name, uint32_t abi,
                                           size_t minimum_size) {
    if (!name || !*name) {
        return nullptr;
    }
    std::lock_guard lock(mutex_);
    std::shared_ptr<Plugin> consumer;
    for (auto& plugin : plugins_) {
        if (plugin.get() == owner) {
            consumer = plugin;
            break;
        }
    }
    if (!consumer) {
        return nullptr;
    }
    {
        std::lock_guard consumer_lock(consumer->mutex);
        // Shutdown may still need its dependencies after callbacks have drained.
        if (consumer->shutdown_complete || !consumer->module) {
            return nullptr;
        }
    }
    for (auto& service : services_) {
        if (service.name != name || service.abi != abi || service.size < minimum_size) {
            continue;
        }
        // Only declared direct dependencies may be borrowed, never the plugin itself.
        if (service.owner == consumer || !depends_on(*consumer, service.owner->key)) {
            continue;
        }
        std::lock_guard provider_lock(service.owner->mutex);
        if (service.owner->state == PluginState::Active) {
            return service.table;
        }
    }
    return nullptr;
}

bool PluginManager::with_external_service(const char* name, uint32_t abi, size_t min_size,
                                          ArkServiceCallback callback, void* user) {
    if (!name || !*name || !callback) {
        return false;
    }
    const void* table = nullptr;
    std::shared_ptr<Plugin> provider;
    {
        std::lock_guard lock(mutex_);
        for (auto& service : services_) {
            if (service.name == name && service.abi == abi && service.size >= min_size) {
                provider = service.owner;
                table = service.table;
                break;
            }
        }
        if (!provider) {
            return false;
        }
    }
    // The lease keeps the provider mapped for the whole callback.
    Invocation lease(provider);
    if (!lease) {
        return false;
    }
    try {
        callback(table, user);
        return true;
    } catch (...) {
        log(kLogError, lease.name() + ": external service callback threw");
        return false;
    }
}

uint64_t PluginManager::create_native_hook(void* owner, void* target, void* detour,
                                           void** original) {
    auto plugin = find_plugin(owner);
    if (!plugin) {
        return 0;
    }
    std::unique_lock lock(plugin->mutex);
    if (!accepts_registrations(*plugin)) {
        return 0;
    }
    std::string error;
    const uint64_t id = native_hooks_.create(owner, target, detour, original, error);
    const bool failed = !id && !error.empty();
    if (failed) {
        plugin->error = error;
    }
    lock.unlock();
    if (failed) {
        log(kLogError, plugin->name + ": " + error);
    }
    return id;
}

bool PluginManager::enable_native_hook(void* owner, uint64_t id, bool enabled) {
    auto plugin = find_plugin(owner);
    if (!plugin) {
        return false;
    }
    std::unique_lock lock(plugin->mutex);
    if (plugin->cleanup_complete || (enabled && !accepts_registrations(*plugin))) {
        return false;
    }
    std::string error;
    const bool ok = native_hooks_.enable(owner, id, enabled, error);
    const bool failed = !ok && !error.empty();
    if (failed) {
        plugin->error = error;
    }
    lock.unlock();
    if (failed) {
        log(kLogError, plugin->name + ": " + error);
    }
    return ok;
}

bool PluginManager::remove_native_hook(void* owner, uint64_t id) {
    auto plugin = find_plugin(owner);
    if (!plugin) {
        return false;
    }
    std::string error;
    const bool ok = native_hooks_.remove(owner, id, kRundownTimeoutMs, error);
    if (!ok && !error.empty()) {
        std::lock_guard lock(plugin->mutex);
        plugin->error = error;
        log(kLogError, plugin->name + ": " + error);
    }
    return ok;
}

} // namespace ark::core
