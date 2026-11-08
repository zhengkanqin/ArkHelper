#include "plugin_manager.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>

#include "plugin_support.h"

namespace ark::core {

using namespace plugin_detail;

namespace {

constexpr size_t kLogRingCapacity = 256;

PluginManager* current_manager = nullptr;

const char* status_name(PluginState state) {
    switch (state) {
    case PluginState::Loading:
        return "Loading";
    case PluginState::Active:
        return "Loaded";
    case PluginState::Stopping:
        return "Stopping";
    case PluginState::Unloaded:
        return "Unloaded";
    default:
        return "Failed";
    }
}

std::string log_prefix(uint32_t level) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char prefix[96];
    snprintf(prefix, sizeof(prefix), "[%04u-%02u-%02u %02u:%02u:%02u.%03u][PID %lu][%u] ",
             unsigned{now.wYear}, unsigned{now.wMonth}, unsigned{now.wDay}, unsigned{now.wHour},
             unsigned{now.wMinute}, unsigned{now.wSecond}, unsigned{now.wMilliseconds},
             GetCurrentProcessId(), level);
    return prefix;
}

} // namespace

PluginManager* manager_instance() {
    return current_manager;
}

Invocation::Invocation(std::shared_ptr<Plugin> owner, bool during_stopping)
    : owner_(std::move(owner)) {
    if (!owner_) {
        return;
    }
    std::lock_guard lock(owner_->mutex);
    const bool active = owner_->state == PluginState::Active;
    const bool finishing =
        during_stopping && owner_->state == PluginState::Stopping && !owner_->cleanup_complete;
    if (active || finishing) {
        ++owner_->in_flight;
        entered_ = true;
    }
}

Invocation::~Invocation() {
    if (!entered_) {
        return;
    }
    std::lock_guard lock(owner_->mutex);
    if (--owner_->in_flight == 0) {
        owner_->drained.notify_all();
    }
}

PluginManager::PluginManager(std::filesystem::path root, bool test_host)
    : root_(std::filesystem::absolute(std::move(root))),
      root_utf8_(to_utf8(root_)),
      test_host_(test_host) {
    if (current_manager) {
        throw std::runtime_error("Only one plugin manager per process is supported");
    }
#if defined(ARKHELP_ENABLE_TEST_MODULES)
    if (test_host_) {
        std::filesystem::create_directories(root_ / "plugins");
        std::filesystem::create_directories(root_ / "cache");
    }
#endif
    std::filesystem::create_directories(root_ / "logs");
    current_manager = this;
}

PluginManager::~PluginManager() {
    stop();
    current_manager = nullptr;
}

void PluginManager::register_builtin(BuiltinModule module) {
    if (running_ || !is_leaf_dll(from_utf8(module.filename)) || !module.query ||
        !module.initialize || !module.shutdown) {
        throw std::invalid_argument("Invalid built-in module registration");
    }
    for (const auto& existing : builtins_) {
        if (path_key(from_utf8(existing.filename)) == path_key(from_utf8(module.filename))) {
            throw std::invalid_argument("Duplicate built-in module registration");
        }
    }
    builtins_.push_back(std::move(module));
}

bool PluginManager::fully_stopped() {
    std::lock_guard lock(mutex_);
    for (const auto& module : plugins_) {
        std::lock_guard owner_lock(module->mutex);
        if (module->module && !module->shutdown_complete) { return false; }
    }
    return true;
}

void PluginManager::log(uint32_t level, const std::string& message) {
    const std::string line = log_prefix(level) + message;
    std::lock_guard lock(log_mutex_);
    logs_.push_back(line);
    while (logs_.size() > kLogRingCapacity) {
        logs_.pop_front();
    }
    std::ofstream file(root_ / "logs" / "ArkHelperRuntime.log", std::ios::app);
    file << line << '\n';
}

std::vector<std::string> PluginManager::recent_logs() {
    std::lock_guard lock(log_mutex_);
    return {logs_.begin(), logs_.end()};
}

std::shared_ptr<Plugin> PluginManager::find_plugin(void* owner) {
    std::lock_guard lock(mutex_);
    for (auto& plugin : plugins_) {
        if (plugin.get() == owner) {
            return plugin;
        }
    }
    return {};
}

uint64_t PluginManager::add(void* owner, CallbackKind kind, ArkFrameCallback frame,
                            ArkGameTask task, void* user) {
    const bool is_task = kind == CallbackKind::GameTask;
    if ((is_task && !task) || (!is_task && !frame)) {
        return 0;
    }
    std::lock_guard lock(mutex_);
    for (auto& plugin : plugins_) {
        if (plugin.get() != owner) {
            continue;
        }
        std::lock_guard owner_lock(plugin->mutex);
        if (!accepts_registrations(*plugin)) {
            return 0;
        }
        const uint64_t token = next_token_++;
        callbacks_.push_back({token, plugin, kind, frame, task, user});
        return token;
    }
    return 0;
}

bool PluginManager::remove(void* owner, uint64_t token) {
    std::lock_guard lock(mutex_);
    const auto erased = std::erase_if(callbacks_, [&](const Callback& callback) {
        return callback.owner.get() == owner && callback.token == token;
    });
    return erased != 0;
}

void PluginManager::dispatch(CallbackKind kind, const ArkFrameV1& frame,
                             std::vector<std::unique_ptr<Invocation>>* frame_leases) {
    std::vector<Callback> snapshot;
    {
        std::lock_guard lock(mutex_);
        for (auto& callback : callbacks_) {
            if (callback.kind == kind) {
                snapshot.push_back(callback);
            }
        }
    }
    for (auto& callback : snapshot) {
        auto invoke = [&] {
            try {
                callback.frame(callback.user, &frame);
            } catch (...) {
                log(kLogError, "Plugin callback threw: " + callback.owner->name);
                request("unload", to_utf8(callback.owner->source.filename()));
            }
        };
        if (frame_leases) {
            auto lease = std::make_unique<Invocation>(callback.owner);
            if (!*lease) {
                continue;
            }
            invoke();
            frame_leases->push_back(std::move(lease));
        } else {
            Invocation lease(callback.owner);
            if (lease) {
                invoke();
            }
        }
    }
}

void PluginManager::dispatch_game_tasks() {
    std::vector<Callback> pending;
    {
        std::lock_guard lock(mutex_);
        for (auto& callback : callbacks_) {
            if (callback.kind == CallbackKind::GameTask) {
                pending.push_back(callback);
            }
        }
        std::erase_if(callbacks_, [](const Callback& callback) {
            return callback.kind == CallbackKind::GameTask;
        });
    }
    for (auto& callback : pending) {
        Invocation lease(callback.owner);
        if (!lease) {
            continue;
        }
        try {
            callback.task(callback.user);
        } catch (...) {
            log(kLogError, "Game task threw: " + callback.owner->name);
        }
    }
}

std::vector<PluginView> PluginManager::views() {
    std::vector<std::shared_ptr<Plugin>> snapshot;
    {
        std::lock_guard lock(mutex_);
        snapshot = plugins_;
    }
    std::vector<PluginView> result;
    for (auto& plugin : snapshot) {
        std::lock_guard lock(plugin->mutex);
        result.push_back({
            .file = to_utf8(plugin->source.filename()),
            .name = plugin->name,
            .version = plugin->version,
            .status = status_name(plugin->state),
            .error = plugin->error,
            .active = plugin->state == PluginState::Active,
            .unloadable = (plugin->flags & ARK_PLUGIN_PROCESS_LIFETIME) == 0,
        });
    }
    return result;
}

void PluginManager::erase_registrations(const std::shared_ptr<Plugin>& plugin) {
    // Caller holds mutex_. Registrations deliberately survive a timed-out or
    // refused shutdown so the retained DLL keeps working.
    std::erase_if(callbacks_, [&](const Callback& callback) { return callback.owner == plugin; });
    std::erase_if(services_, [&](const Service& service) { return service.owner == plugin; });
    std::erase_if(input_adapters_, [&](const std::shared_ptr<InputAdapter>& adapter) {
        return adapter->owner == plugin;
    });
}

} // namespace ark::core
