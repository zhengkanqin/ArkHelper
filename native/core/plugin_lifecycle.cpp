#include <map>

#include "plugin_manager.h"
#include "plugin_support.h"

namespace ark::core {

using namespace plugin_detail;

namespace {

constexpr size_t kUnloadReasonCapacity = 512;

enum class VisitMark {
    Unvisited,
    InProgress,
    Done
};

// Depth-first walk over the consumers of `plugin`. Appends consumers before
// their providers (reverse load order); returns false on a dependency cycle.
bool collect_consumers_first(const std::shared_ptr<Plugin>& plugin,
                             const std::vector<std::shared_ptr<Plugin>>& mapped,
                             std::map<std::wstring, VisitMark>& marks,
                             std::vector<std::shared_ptr<Plugin>>& order) {
    auto& mark = marks[plugin->key];
    if (mark == VisitMark::Done) {
        return true;
    }
    if (mark == VisitMark::InProgress) {
        return false;
    }
    mark = VisitMark::InProgress;
    for (auto& consumer : mapped) {
        if (consumer == plugin || !depends_on(*consumer, plugin->key)) {
            continue;
        }
        if (!collect_consumers_first(consumer, mapped, marks, order)) {
            return false;
        }
    }
    // std::map references stay valid across the insertions made above.
    mark = VisitMark::Done;
    order.push_back(plugin);
    return true;
}

} // namespace

bool PluginManager::unload_now(const std::string& filename, uint32_t timeout_ms) {
    if (!is_leaf_dll(from_utf8(filename))) {
        return false;
    }
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    const auto identity = path_key(from_utf8(filename));
    std::shared_ptr<Plugin> plugin;
    {
        std::lock_guard lock(mutex_);
        for (auto& item : plugins_) {
            if (item->key == identity && item->module) {
                plugin = item;
                break;
            }
        }
    }
    return plugin && unload(plugin, timeout_ms, false);
}

bool PluginManager::reload_now(const std::string& filename, uint32_t timeout_ms,
                               std::vector<std::wstring>* affected) {
    const auto leaf = from_utf8(filename);
    if (!is_leaf_dll(leaf)) {
        return false;
    }
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    const auto identity = path_key(leaf);
    std::vector<std::shared_ptr<Plugin>> mapped;
    {
        std::lock_guard lock(mutex_);
        for (auto& plugin : plugins_) {
            if (plugin->module && !plugin->shutdown_complete) {
                mapped.push_back(plugin);
            }
        }
    }
    std::shared_ptr<Plugin> root;
    for (auto& plugin : mapped) {
        if (plugin->key == identity) {
            root = plugin;
            break;
        }
    }
    if (!root) {
        if (affected) {
            affected->push_back(identity);
        }
        std::vector<std::wstring> chain;
        return load(leaf, chain);
    }

    std::map<std::wstring, VisitMark> marks;
    std::vector<std::shared_ptr<Plugin>> order;
    if (!collect_consumers_first(root, mapped, marks, order)) {
        std::lock_guard lock(root->mutex);
        root->error = "Reload dependency cycle";
        return false;
    }
    if (affected) {
        for (auto& plugin : order) {
            affected->push_back(plugin->key);
        }
    }

    // Preflight the complete graph before stopping even one consumer.
    for (auto& plugin : order) {
        std::string reason;
        {
            std::lock_guard lock(plugin->mutex);
            if (plugin->flags & ARK_PLUGIN_PROCESS_LIFETIME) {
                reason = "Process-lifetime plugin cannot be reloaded";
            }
        }
        if (reason.empty()) {
            may_unload(plugin, reason);
        }
        if (!reason.empty()) {
            std::lock_guard lock(root->mutex);
            root->error = plugin->name + ": " + reason;
            log(kLogWarning, root->name + ": reload refused: " + root->error);
            return false;
        }
    }

    size_t stopped = 0;
    for (auto& plugin : order) {
        if (!unload(plugin, timeout_ms, false)) {
            // Restore consumers already stopped while their provider still works.
            while (stopped) {
                std::vector<std::wstring> chain;
                load(order[--stopped]->source.filename(), chain, true);
            }
            return false;
        }
        ++stopped;
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        // Replacement metadata may recursively load a later planned member
        // first; accept_active lets that member's own turn succeed.
        std::vector<std::wstring> chain;
        if (!load((*it)->source.filename(), chain, true)) {
            return false;
        }
    }
    log(kLogInfo, "Reloaded " + filename + " and " + std::to_string(order.size() - 1) +
                      " dependent plugin(s)");
    return true;
}

bool PluginManager::may_unload(const std::shared_ptr<Plugin>& plugin, std::string& reason) {
    ArkPluginCanUnload can_unload = nullptr;
    {
        std::lock_guard lock(plugin->mutex);
        if (plugin->cleanup_complete) {
            return true;
        }
        can_unload = plugin->can_unload;
    }
    if (!can_unload) {
        return true;
    }
    char message[kUnloadReasonCapacity]{};
    try {
        if (can_unload(message, sizeof(message))) {
            return true;
        }
    } catch (...) {
        reason = "Plugin unload preflight threw";
        return false;
    }
    message[sizeof(message) - 1] = 0;
    reason = message[0] ? message : "Plugin is not ready to unload";
    return false;
}

bool PluginManager::drain_invocations(Plugin& plugin, uint32_t timeout_ms) {
    std::unique_lock lock(plugin.mutex);
    if (wait_until_idle(lock, plugin, std::chrono::milliseconds(timeout_ms))) {
        return true;
    }
    plugin.error = "Unload timed out: callbacks are still running; DLL retained";
    log(kLogError, plugin.error);
    return false;
}

// Stops one plugin. Every failure keeps the DLL mapped and leaves the plugin
// in a retryable state. `stopping` (process shutdown) also stops plugins that
// are flagged process-lifetime, without unmapping them.
bool PluginManager::unload(const std::shared_ptr<Plugin>& plugin, uint32_t timeout_ms,
                           bool stopping) {
    std::string reason;
    if (!may_unload(plugin, reason)) {
        std::lock_guard lock(plugin->mutex);
        plugin->error = reason;
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        std::lock_guard owner_lock(plugin->mutex);
        if (plugin->shutdown_complete) {
            return false;
        }
        if (!stopping && (plugin->flags & ARK_PLUGIN_PROCESS_LIFETIME)) {
            plugin->error = "Process-lifetime plugin cannot be unloaded or reloaded";
            return false;
        }
        for (auto& consumer : plugins_) {
            if (consumer != plugin && consumer->module && !consumer->shutdown_complete &&
                depends_on(*consumer, plugin->key)) {
                plugin->error = "Plugin is required by " + consumer->name;
                return false;
            }
        }
        plugin->state = PluginState::Stopping;
        plugin->error.clear();
        std::erase_if(callbacks_,
                      [&](const Callback& callback) { return callback.owner == plugin; });
    }

    if (!drain_invocations(*plugin, timeout_ms)) {
        return false;
    }
    if (!plugin->cleanup_complete) {
        bool stopped = false;
        try {
            stopped = plugin->shutdown();
        } catch (...) {
        }
        if (!stopped) {
            std::lock_guard lock(plugin->mutex);
            plugin->error = "Plugin declined shutdown; DLL retained";
            log(kLogError, plugin->error);
            return false;
        }
        std::lock_guard lock(plugin->mutex);
        plugin->cleanup_complete = true;
    }
    if (!native_hooks_.detach(plugin.get(), timeout_ms, reason)) {
        std::lock_guard lock(plugin->mutex);
        plugin->error = reason;
        log(kLogError, plugin->error);
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        erase_registrations(plugin);
    }
    // An input adapter lease may have entered during the plugin's release rundown.
    if (!drain_invocations(*plugin, timeout_ms)) {
        return false;
    }

    const bool process_lifetime = plugin->flags & ARK_PLUGIN_PROCESS_LIFETIME;
    if (!process_lifetime && !plugin->builtin && !FreeLibrary(plugin->module)) {
        std::lock_guard lock(plugin->mutex);
        plugin->error = "FreeLibrary failed: " + std::to_string(GetLastError());
        return false;
    }
    {
        std::lock_guard lock(plugin->mutex);
        if (!process_lifetime) {
            plugin->module = nullptr;
        }
        plugin->shutdown_complete = true;
        plugin->state = PluginState::Unloaded;
    }
    if (!plugin->module) {
        std::error_code error;
        std::filesystem::remove(plugin->shadow, error);
    }
    log(kLogInfo,
        "Stopped " + plugin->name +
            (plugin->builtin ? " (built in)" :
             plugin->module ? " (DLL retained for process lifetime)" : " (DLL unloaded)"));
    return true;
}

void PluginManager::stop() {
    running_ = false;
    if (worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    std::vector<std::shared_ptr<Plugin>> pending;
    {
        std::lock_guard lock(mutex_);
        for (auto& plugin : plugins_) {
            if (plugin->module && !plugin->shutdown_complete) {
                pending.push_back(plugin);
            }
        }
    }
    // A successful consumer shutdown releases its providers for the next pass.
    // Refused consumers remain and keep their entire dependency chain mapped.
    bool progress = true;
    while (progress && !pending.empty()) {
        progress = false;
        for (auto it = pending.begin(); it != pending.end();) {
            if (unload(*it, kRundownTimeoutMs, true)) {
                it = pending.erase(it);
                progress = true;
            } else {
                ++it;
            }
        }
    }
}

} // namespace ark::core
