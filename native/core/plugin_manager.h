// Private built-in module host. External DLL loading and input adaptation are
// compiled only for isolated lifecycle fixtures (ARKHELP_ENABLE_TEST_MODULES).
#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ark/plugin.h>

#include "native_hooks.h"

namespace ark::core {

enum class PluginState {
    Loading,
    Active,
    Stopping,
    Unloaded,
    Failed
};

struct BuiltinModule {
    std::string filename;
    ArkPluginQuery query = nullptr;
    ArkPluginInitialize initialize = nullptr;
    ArkPluginShutdown shutdown = nullptr;
    ArkPluginCanUnload can_unload = nullptr;
};

// One loaded (or failed) plugin generation. Fields guarded by `mutex` unless
// noted; when PluginManager::mutex_ is also needed it is always taken first.
struct Plugin {
    std::mutex mutex;
    std::condition_variable drained;
    PluginState state = PluginState::Loading;
    size_t in_flight = 0;
    std::atomic<HMODULE> module{nullptr};
    bool builtin = false; // borrowed runtime handle; never FreeLibrary it
    ArkPluginShutdown shutdown = nullptr;
    ArkPluginApiV1 api{};
    ArkPluginCanUnload can_unload = nullptr;
    std::filesystem::path source;
    std::filesystem::path shadow;
    std::wstring key;
    std::vector<std::wstring> dependencies;
    std::string name;
    std::string version;
    std::string error;
    uint32_t flags = 0;
    bool cleanup_complete = false;
    bool shutdown_complete = false;
};

// Scoped lease that keeps a plugin's code callable. Unload waits until every
// lease has been released before calling shutdown or unmapping the DLL.
class Invocation {
public:
    // `during_stopping` also admits a plugin that is stopping but has not yet
    // completed its shutdown (used for input adapters during their rundown).
    explicit Invocation(std::shared_ptr<Plugin> owner, bool during_stopping = false);
    ~Invocation();

    Invocation(const Invocation&) = delete;
    Invocation& operator=(const Invocation&) = delete;

    explicit operator bool() const { return entered_; }

    const std::string& name() const { return owner_->name; }

private:
    std::shared_ptr<Plugin> owner_;
    bool entered_ = false;
};

enum class CallbackKind {
    Frame,
    Draw,
    GameTask
};

struct PluginView {
    std::string file;
    std::string name;
    std::string version;
    std::string status;
    std::string error;
    bool active = false;
    bool unloadable = true;
};

// Process-wide singleton; constructing a second instance throws.
class PluginManager {
public:
    PluginManager(std::filesystem::path root, bool test_host);
    ~PluginManager();

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    void start();
    void stop();
    bool fully_stopped();
    void register_builtin(BuiltinModule module);

    // Queues "scan", "load", "unload" or "reload" for the worker thread.
    void request(std::string operation, std::string filename);
    bool load_now(const std::filesystem::path& path);
    bool unload_now(const std::string& filename, uint32_t timeout_ms = 3000);
    // Reloads `filename` together with every transitive consumer. `affected`
    // receives the keys of all plugins covered by this attempt.
    bool reload_now(const std::string& filename, uint32_t timeout_ms = 3000,
                    std::vector<std::wstring>* affected = nullptr);

    // With `frame_leases`, each invoked plugin stays leased until the caller
    // clears the vector (e.g. until the frame has been presented).
    void dispatch(CallbackKind kind, const ArkFrameV1& frame,
                  std::vector<std::unique_ptr<Invocation>>* frame_leases = nullptr);
    void dispatch_game_tasks();

    void set_input_capture(bool mouse, bool keyboard);
    bool route_window_input(uint32_t message, uintptr_t wparam, intptr_t lparam);
    bool input_protected();
    bool has_input_adapter();

    bool with_external_service(const char* name, uint32_t abi, size_t min_size,
                               ArkServiceCallback callback, void* user);
    std::vector<PluginView> views();

    uint64_t add(void* owner, CallbackKind kind, ArkFrameCallback frame, ArkGameTask task,
                 void* user);
    bool remove(void* owner, uint64_t token);

    void log(uint32_t level, const std::string& message);
    std::vector<std::string> recent_logs();

    bool test_host() const { return test_host_; }

    std::atomic<bool> auto_reload{false};

private:
    struct Callback {
        uint64_t token;
        std::shared_ptr<Plugin> owner;
        CallbackKind kind;
        ArkFrameCallback frame = nullptr;
        ArkGameTask task = nullptr;
        void* user = nullptr;
    };

    struct Service {
        std::shared_ptr<Plugin> owner;
        std::string name;
        uint32_t abi;
        const void* table;
        size_t size;
    };

    struct InputAdapter {
        std::shared_ptr<Plugin> owner;
        ArkInputAdapterV1 table{};
        std::mutex mutex;
        std::condition_variable drained;
        size_t in_flight = 0;
        bool accepting = true;
    };
    struct InputLease;

    struct Request {
        std::string operation;
        std::string filename;
    };
    struct DirectoryWatch;

    std::shared_ptr<Plugin> find_plugin(void* owner);

    // SDK entry points handed to plugins through ArkPluginApiV1.
    void build_api(Plugin& plugin);
    static void api_log(void* owner, uint32_t level, const char* message);
    static uint64_t api_on_frame(void* owner, ArkFrameCallback callback, void* user);
    static uint64_t api_on_draw(void* owner, ArkFrameCallback callback, void* user);
    static bool api_remove_callback(void* owner, uint64_t token);
    static bool api_queue_game_task(void* owner, ArkGameTask task, void* user);
    static void api_pump_game_tasks(void* owner);
    static bool api_provide_service(void* owner, const char* name, uint32_t abi, const void* table,
                                    size_t size);
    static const void* api_require_service(void* owner, const char* name, uint32_t abi,
                                           size_t minimum_size);
    static bool api_set_input_adapter(void* owner, const ArkInputAdapterV1* adapter);
    static uint64_t api_create_native_hook(void* owner, void* target, void* detour,
                                           void** original);
    static bool api_enable_native_hook(void* owner, uint64_t id, bool enabled);
    static bool api_remove_native_hook(void* owner, uint64_t id);

    // Services.
    bool provide_service(void* owner, const char* name, uint32_t abi, const void* table,
                         size_t size);
    const void* require_service(void* owner, const char* name, uint32_t abi, size_t minimum_size);

    // Native hooks.
    uint64_t create_native_hook(void* owner, void* target, void* detour, void** original);
    bool enable_native_hook(void* owner, uint64_t id, bool enabled);
    bool remove_native_hook(void* owner, uint64_t id);

    // Input adapters.
    bool set_input_adapter(void* owner, const ArkInputAdapterV1* adapter);
    std::vector<std::shared_ptr<InputAdapter>> input_snapshot();
    std::unique_ptr<InputLease> acquire_input_lease(const std::shared_ptr<InputAdapter>& adapter);
    template <typename Call>
    void for_each_input_adapter(const char* failure_message, Call&& call);

    // Loading.
    bool load(const std::filesystem::path& input, std::vector<std::wstring>& chain,
              bool accept_active = false);
    std::shared_ptr<Plugin> create_plugin(const std::filesystem::path& leaf);
    bool map_plugin(Plugin& plugin, ArkPluginInitialize& initialize,
                    std::vector<std::filesystem::path>& dependencies, std::string& reason);
    bool rollback_initialization(const std::shared_ptr<Plugin>& plugin);
    bool fail_load(const std::shared_ptr<Plugin>& plugin, const std::string& reason);

    // Unloading and reloading.
    bool unload(const std::shared_ptr<Plugin>& plugin, uint32_t timeout_ms, bool stopping);
    bool may_unload(const std::shared_ptr<Plugin>& plugin, std::string& reason);
    bool drain_invocations(Plugin& plugin, uint32_t timeout_ms);
    void erase_registrations(const std::shared_ptr<Plugin>& plugin);

    // Worker thread.
    void run();
    void scan();
    void process_requests();
    size_t process_reload_batch(const std::deque<Request>& requests, size_t begin);
    void poll_plugin_directory(DirectoryWatch& watch);

    std::filesystem::path root_;
    std::string root_utf8_;
    bool test_host_;

    // Lock order: lifecycle_mutex_, then mutex_, then Plugin::mutex. No plugin
    // callback runs with mutex_ or a Plugin::mutex held. Lifecycle operations
    // are serialized separately from dispatch.
    std::mutex mutex_;
    std::mutex log_mutex_;
    std::mutex lifecycle_mutex_;
    std::vector<std::shared_ptr<Plugin>> plugins_;
    std::vector<Callback> callbacks_;
    std::vector<Service> services_;
    std::vector<std::shared_ptr<InputAdapter>> input_adapters_;
    std::deque<Request> requests_;
    std::deque<std::string> logs_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::atomic<uint64_t> next_token_{1};
    NativeHooks native_hooks_;
    std::vector<BuiltinModule> builtins_;
    uint64_t shadow_generation_ = 0;
};

PluginManager* manager_instance();

} // namespace ark::core
