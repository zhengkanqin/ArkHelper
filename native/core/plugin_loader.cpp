#include <algorithm>


#include "plugin_manager.h"
#include "plugin_support.h"

namespace ark::core {

using namespace plugin_detail;

namespace {

constexpr size_t kMaxDependencies = 256;

#if defined(ARKHELP_ENABLE_TEST_MODULES)
template <typename Function>
Function find_export(HMODULE module, const char* name) {
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}
#endif

} // namespace

bool PluginManager::load_now(const std::filesystem::path& path) {
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    std::vector<std::wstring> chain;
    return load(path, chain);
}

// `chain` holds the keys of plugins currently loading their dependencies; it
// detects cycles. A plugin that is already active satisfies a dependency, and
// satisfies a top-level request only when `accept_active` is set.
bool PluginManager::load(const std::filesystem::path& input, std::vector<std::wstring>& chain,
                         bool accept_active) {
    const auto leaf = std::filesystem::path(path_key(input.filename()));
    const bool inside_plugins =
        path_key(input.parent_path().lexically_normal()) == path_key(root_ / "plugins");
    if (!is_leaf_dll(leaf) || (!is_leaf_dll(input) && !inside_plugins)) {
        log(kLogError, "Rejected unsafe plugin path");
        return false;
    }
    const auto identity = path_key(leaf);
    if (std::find(chain.begin(), chain.end(), identity) != chain.end()) {
        log(kLogError, "Plugin dependency cycle: " + to_utf8(leaf));
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        for (auto& existing : plugins_) {
            if (existing->key == identity && existing->module) {
                std::lock_guard owner_lock(existing->mutex);
                return (accept_active || !chain.empty()) && existing->state == PluginState::Active;
            }
        }
        // Forget unmapped records of earlier generations.
        std::erase_if(plugins_, [&](const std::shared_ptr<Plugin>& plugin) {
            return plugin->key == identity && !plugin->module;
        });
    }

    auto plugin = create_plugin(leaf);
    ArkPluginInitialize initialize = nullptr;
    std::vector<std::filesystem::path> dependencies;
    std::string reason;
    if (!map_plugin(*plugin, initialize, dependencies, reason)) {
        return fail_load(plugin, reason);
    }

    // Providers initialize before their consumers.
    chain.push_back(identity);
    bool dependencies_loaded = true;
    for (auto& dependency : dependencies) {
        if (!load(dependency, chain)) {
            dependencies_loaded = false;
            break;
        }
    }
    chain.pop_back();
    if (!dependencies_loaded) {
        return fail_load(plugin, "Required dependency failed to load");
    }

    build_api(*plugin);
    bool initialized = false;
    try {
        initialized = initialize(&plugin->api);
    } catch (...) {
        initialized = false;
    }
    if (!initialized) {
        return rollback_initialization(plugin);
    }

    {
        std::lock_guard lock(plugin->mutex);
        plugin->state = PluginState::Active;
    }
    log(kLogInfo, "Loaded " + plugin->name +
        (plugin->builtin ? " (built in)" : " from " + to_utf8(plugin->shadow)));
    return true;
}

std::shared_ptr<Plugin> PluginManager::create_plugin(const std::filesystem::path& leaf) {
    auto plugin = std::make_shared<Plugin>();
    plugin->source = leaf; // logical identity for built-in lifecycle; never opened
    plugin->key = path_key(leaf);
    plugin->name = to_utf8(leaf);
#if defined(ARKHELP_ENABLE_TEST_MODULES)
    plugin->source = root_ / "plugins" / leaf;
    // A unique shadow copy per generation keeps the source file replaceable
    // while the module is mapped.
    plugin->shadow = root_ / "cache" /
                     (leaf.stem().wstring() + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                      std::to_wstring(++shadow_generation_) + L".dll");
#endif
    std::lock_guard lock(mutex_);
    plugins_.push_back(plugin);
    return plugin;
}

// Copies and maps the shadow DLL, resolves its exports and validates its
// metadata. On failure `reason` describes the problem; the caller rolls back.
bool PluginManager::map_plugin(Plugin& plugin, ArkPluginInitialize& initialize,
                               std::vector<std::filesystem::path>& dependencies,
                               std::string& reason) {
    ArkPluginQuery query = nullptr;
    for (const auto& builtin : builtins_) {
        if (path_key(from_utf8(builtin.filename)) != plugin.key) {
            continue;
        }
        HMODULE owner = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(builtin.query), &owner)) {
            reason = "Could not resolve built-in module owner";
            return false;
        }
        plugin.builtin = true;
        plugin.module = owner;
        plugin.shadow.clear();
        query = builtin.query;
        initialize = builtin.initialize;
        plugin.shutdown = builtin.shutdown;
        plugin.can_unload = builtin.can_unload;
        break;
    }
    if (!plugin.builtin) {
#if defined(ARKHELP_ENABLE_TEST_MODULES)
    if (!test_host_ || !builtins_.empty()) {
        reason = "External modules are disabled in the product runtime";
        return false;
    }
    std::error_code error;
    if (!std::filesystem::copy_file(plugin.source, plugin.shadow,
                                    std::filesystem::copy_options::none, error)) {
        reason = "Copy failed: " + error.message();
        return false;
    }
    plugin.module =
        LoadLibraryExW(plugin.shadow.c_str(), nullptr,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!plugin.module) {
        reason = "LoadLibraryExW failed: " + std::to_string(GetLastError());
        return false;
    }

    const HMODULE module = plugin.module;
    query = find_export<ArkPluginQuery>(module, "ark_plugin_query");
    initialize = find_export<ArkPluginInitialize>(module, "ark_plugin_initialize");
    plugin.shutdown = find_export<ArkPluginShutdown>(module, "ark_plugin_shutdown");
    plugin.can_unload = find_export<ArkPluginCanUnload>(module, "ark_plugin_can_unload");
    if (!query || !initialize || !plugin.shutdown) {
        reason = "Required plugin exports are missing";
        return false;
    }
#else
        reason = "Only statically linked ArkHelper modules are supported";
        return false;
#endif
    }
    const ArkPluginInfoV1* info = nullptr;
    try {
        info = query();
    } catch (...) {
        reason = "Plugin query threw";
        return false;
    }
    if (!info || info->size < sizeof(ArkPluginInfoV1) || info->abi_version != ARK_ABI_VERSION ||
        info->imgui_version != ARK_MODULE_COMPAT_VERSION) {
        reason = "Internal module ABI version mismatch";
        return false;
    }
    {
        std::lock_guard lock(plugin.mutex);
        if (info->name) {
            plugin.name = info->name;
        }
        plugin.version = info->version ? info->version : "";
        plugin.flags = info->flags;
    }

    if (info->dependency_count > kMaxDependencies ||
        (info->dependency_count && !info->dependencies)) {
        reason = "Invalid dependency declaration";
        return false;
    }
    for (size_t index = 0; index < info->dependency_count; ++index) {
        if (!info->dependencies[index]) {
            reason = "Invalid dependency leaf";
            return false;
        }
        const auto dependency = from_utf8(info->dependencies[index]);
        if (!is_leaf_dll(dependency)) {
            reason = "Unsafe dependency leaf";
            return false;
        }
        const auto dependency_key = path_key(dependency);
        if (!depends_on(plugin, dependency_key)) {
            plugin.dependencies.push_back(dependency_key);
            dependencies.push_back(dependency);
        }
    }
    return true;
}

// initialize() returned false or threw: drain, let the plugin clean up, then
// roll back. A plugin that declines cleanup stays mapped with its registrations.
bool PluginManager::rollback_initialization(const std::shared_ptr<Plugin>& plugin) {
    {
        std::lock_guard lock(mutex_);
        std::lock_guard owner_lock(plugin->mutex);
        plugin->state = PluginState::Stopping;
        std::erase_if(callbacks_,
                      [&](const Callback& callback) { return callback.owner == plugin; });
    }
    {
        std::unique_lock lock(plugin->mutex);
        if (!wait_until_idle(lock, *plugin, kRundownTimeout)) {
            plugin->error = "Initialization cleanup timed out; DLL retained";
            return false;
        }
    }
    bool cleaned = false;
    try {
        cleaned = plugin->shutdown();
    } catch (...) {
    }
    if (!cleaned) {
        std::lock_guard lock(plugin->mutex);
        plugin->error = "Initialization failed and cleanup was declined; DLL retained";
        log(kLogError, plugin->error);
        return false;
    }
    {
        std::lock_guard lock(plugin->mutex);
        plugin->cleanup_complete = true;
    }
    return fail_load(plugin, "Plugin initialization failed");
}

// Undoes a partial load. Every failure along the way keeps the DLL mapped,
// since its code may still be referenced. Always returns false.
bool PluginManager::fail_load(const std::shared_ptr<Plugin>& plugin, const std::string& reason) {
    std::string hook_error;
    if (plugin->module && !native_hooks_.detach(plugin.get(), kRundownTimeoutMs, hook_error)) {
        std::lock_guard lock(plugin->mutex);
        plugin->state = PluginState::Stopping;
        plugin->error = reason + "; " + hook_error;
        log(kLogError, plugin->name + ": " + plugin->error);
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        erase_registrations(plugin);
        std::lock_guard owner_lock(plugin->mutex);
        plugin->state = PluginState::Failed;
        plugin->error = reason;
        plugin->cleanup_complete = true;
    }
    {
        // Input adapter leases admitted while stopping may still be running.
        std::unique_lock lock(plugin->mutex);
        if (!wait_until_idle(lock, *plugin, kRundownTimeout)) {
            plugin->state = PluginState::Stopping;
            plugin->error = "Initialization adapter rundown timed out; DLL retained";
            return false;
        }
    }
    if (plugin->module && !(plugin->flags & ARK_PLUGIN_PROCESS_LIFETIME)) {
        if (!plugin->builtin && !FreeLibrary(plugin->module)) {
            std::lock_guard lock(plugin->mutex);
            plugin->state = PluginState::Stopping;
            plugin->error = "FreeLibrary failed after initialization cleanup";
            return false;
        }
        plugin->module = nullptr;
    }
    {
        std::lock_guard lock(plugin->mutex);
        plugin->shutdown_complete = true;
    }
    if (!plugin->module) {
        std::error_code error;
        std::filesystem::remove(plugin->shadow, error);
    }
    log(kLogError, plugin->name + ": " + reason);
    return false;
}

void PluginManager::build_api(Plugin& plugin) {
    auto& api = plugin.api;
    api = {};
    api.size = sizeof(ArkPluginApiV1);
    api.abi_version = ARK_ABI_VERSION;
    api.imgui_version = ARK_MODULE_COMPAT_VERSION;
    api.test_host = test_host_ ? 1u : 0u;
    api.owner = &plugin;
    api.root_directory = root_utf8_.c_str();
    api.log = &PluginManager::api_log;
    api.on_frame = &PluginManager::api_on_frame;
    api.on_draw = &PluginManager::api_on_draw;
    api.remove_callback = &PluginManager::api_remove_callback;
    api.queue_game_task = &PluginManager::api_queue_game_task;
    api.pump_game_tasks = &PluginManager::api_pump_game_tasks;
    api.provide_service = &PluginManager::api_provide_service;
    api.require_service = &PluginManager::api_require_service;
#if defined(ARKHELP_ENABLE_TEST_MODULES)
    api.set_input_adapter = &PluginManager::api_set_input_adapter;
#endif
    api.create_native_hook = &PluginManager::api_create_native_hook;
    api.enable_native_hook = &PluginManager::api_enable_native_hook;
    api.remove_native_hook = &PluginManager::api_remove_native_hook;
}

void PluginManager::api_log(void* owner, uint32_t level, const char* message) {
    auto* manager = manager_instance();
    if (auto plugin = manager->find_plugin(owner)) {
        manager->log(level, plugin->name + ": " + (message ? message : ""));
    }
}

uint64_t PluginManager::api_on_frame(void* owner, ArkFrameCallback callback, void* user) {
    return manager_instance()->add(owner, CallbackKind::Frame, callback, nullptr, user);
}

uint64_t PluginManager::api_on_draw(void* owner, ArkFrameCallback callback, void* user) {
    return manager_instance()->add(owner, CallbackKind::Draw, callback, nullptr, user);
}

bool PluginManager::api_remove_callback(void* owner, uint64_t token) {
    return manager_instance()->remove(owner, token);
}

bool PluginManager::api_queue_game_task(void* owner, ArkGameTask task, void* user) {
    return manager_instance()->add(owner, CallbackKind::GameTask, nullptr, task, user) != 0;
}

void PluginManager::api_pump_game_tasks(void* owner) {
    auto* manager = manager_instance();
    if (auto plugin = manager->find_plugin(owner)) {
        Invocation lease(plugin);
        if (lease) {
            manager->dispatch_game_tasks();
        }
    }
}

bool PluginManager::api_provide_service(void* owner, const char* name, uint32_t abi,
                                        const void* table, size_t size) {
    return manager_instance()->provide_service(owner, name, abi, table, size);
}

const void* PluginManager::api_require_service(void* owner, const char* name, uint32_t abi,
                                               size_t minimum_size) {
    return manager_instance()->require_service(owner, name, abi, minimum_size);
}

#if defined(ARKHELP_ENABLE_TEST_MODULES)
bool PluginManager::api_set_input_adapter(void* owner, const ArkInputAdapterV1* adapter) {
    return manager_instance()->set_input_adapter(owner, adapter);
}
#endif

uint64_t PluginManager::api_create_native_hook(void* owner, void* target, void* detour,
                                               void** original) {
    return manager_instance()->create_native_hook(owner, target, detour, original);
}

bool PluginManager::api_enable_native_hook(void* owner, uint64_t id, bool enabled) {
    return manager_instance()->enable_native_hook(owner, id, enabled);
}

bool PluginManager::api_remove_native_hook(void* owner, uint64_t id) {
    return manager_instance()->remove_native_hook(owner, id);
}

} // namespace ark::core
