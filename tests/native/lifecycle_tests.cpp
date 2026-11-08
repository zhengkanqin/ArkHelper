// Plugin lifecycle tests against real fixture DLLs.
//
// Usage: lifecycle_tests <fixture directory>
// The directory must contain fixture.dll, bad_abi.dll and the service
// lifecycle fixtures; a scratch runtime root is created beneath it.
#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>


#include "lifecycle_service_fixture.h"
#include "plugin_manager.h"

namespace {

namespace fs = std::filesystem;

using ark::core::CallbackKind;
using ark::core::Invocation;
using ark::core::PluginManager;
using lifecycle_fixture::kServiceAbi;
using lifecycle_fixture::kServiceName;
using lifecycle_fixture::LifecycleService;

[[noreturn]] void fail_check(const char* expression, int line) {
    throw std::runtime_error(std::string(expression) + " (line " + std::to_string(line) + ")");
}

#define ARK_CHECK(condition)                                                                       \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fail_check(#condition, __LINE__);                                                      \
        }                                                                                          \
    } while (0)

// Messages claimed by the provider adapters and by the undeclared adapter.
constexpr uint32_t kProviderMessage = 0x4321;
constexpr uint32_t kPeerMessage = 0x4322;
constexpr uintptr_t kWparam = 23;
constexpr intptr_t kLparam = -7;

constexpr ULONGLONG kEnterTimeoutMs = 2000;
constexpr uint32_t kShortUnloadTimeoutMs = 10;
constexpr int kReloadCycles = 25;
constexpr int kServiceInitialValue = 42;

struct Environment {
    fs::path fixtures;
    fs::path root;
    ArkFrameV1 frame;
};

// ---------------------------------------------------------------------------
// Fixture helpers

// Finds the mapped shadow copy of `stem` in the runtime cache directory.
HMODULE loaded_module(const fs::path& root, const wchar_t* stem = L"fixture") {
    const auto prefix = std::wstring(stem) + L"-";
    for (auto& file : fs::directory_iterator(root / "cache")) {
        if (!file.path().filename().wstring().starts_with(prefix)) {
            continue;
        }
        if (auto module = GetModuleHandleW(file.path().c_str())) {
            return module;
        }
    }
    return nullptr;
}

template <typename Function>
Function export_function(HMODULE module, const char* name) {
    auto address = GetProcAddress(module, name);
    ARK_CHECK(address);
    return reinterpret_cast<Function>(address);
}

int call_int(HMODULE module, const char* name) {
    return export_function<int (*)()>(module, name)();
}

bool call_bool(HMODULE module, const char* name) {
    return export_function<bool (*)()>(module, name)();
}

void call_with_int(HMODULE module, const char* name, int value) {
    export_function<void (*)(int)>(module, name)(value);
}

int frames(HMODULE module) {
    return call_int(module, "fixture_frames");
}

int shutdowns(HMODULE module) {
    return call_int(module, "fixture_shutdowns");
}

int dependency_value(HMODULE module) {
    return call_int(module, "fixture_dependency_value");
}

void refuse_shutdown(HMODULE module, bool refuse) {
    call_with_int(module, "fixture_refuse", refuse ? 1 : 0);
}

void refuse_preflight(HMODULE module, bool refuse) {
    call_with_int(module, "fixture_preflight_refuse", refuse ? 1 : 0);
}

const void* query_service(HMODULE module, uint32_t abi, size_t size) {
    return export_function<const void* (*)(uint32_t, size_t)>(module, "fixture_query")(abi, size);
}

void install_fixture(const Environment& env, const fs::path& root, const wchar_t* source,
                     const wchar_t* target = nullptr) {
    fs::copy_file(env.fixtures / source, root / "plugins" / (target ? target : source),
                  fs::copy_options::overwrite_existing);
}

fs::path module_path(HMODULE module) {
    wchar_t path[32768];
    const auto length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    ARK_CHECK(length && length < std::size(path));
    return std::wstring(path, length);
}

template <typename Predicate>
void wait_until(Predicate&& done, ULONGLONG timeout_ms) {
    const auto deadline = GetTickCount64() + timeout_ms;
    while (!done() && GetTickCount64() < deadline) {
        Sleep(1);
    }
}

class ManualResetEvent {
public:
    ManualResetEvent() : handle_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    ~ManualResetEvent() {
        if (handle_) {
            CloseHandle(handle_);
        }
    }

    ManualResetEvent(const ManualResetEvent&) = delete;
    ManualResetEvent& operator=(const ManualResetEvent&) = delete;

    HANDLE get() const { return handle_; }

private:
    HANDLE handle_;
};

// ---------------------------------------------------------------------------
// Service helpers

struct ServiceSample {
    int value = 0;
    int frames = 0;
    int consumer_shutdowns = 0;
};

ServiceSample sample_service(PluginManager& manager) {
    ServiceSample result;
    ARK_CHECK(manager.with_external_service(
        kServiceName, kServiceAbi, sizeof(LifecycleService),
        [](const void* table, void* user) {
            const auto& service = *static_cast<const LifecycleService*>(table);
            *static_cast<ServiceSample*>(user) = {service.value(), service.frames(),
                                                  service.consumer_shutdowns()};
        },
        &result));
    return result;
}

void increment_service(PluginManager& manager) {
    ARK_CHECK(manager.with_external_service(
        kServiceName, kServiceAbi, sizeof(LifecycleService),
        [](const void* table, void*) { static_cast<const LifecycleService*>(table)->increment(); },
        nullptr));
}

// Holds an external service call open on a background thread until finish().
class BlockedServiceCall {
public:
    explicit BlockedServiceCall(PluginManager& manager)
        : thread_([this, &manager] {
              completed_ = manager.with_external_service(kServiceName, kServiceAbi,
                                                         sizeof(LifecycleService), &callback, this);
          }) {}

    ~BlockedServiceCall() { finish(); }

    BlockedServiceCall(const BlockedServiceCall&) = delete;
    BlockedServiceCall& operator=(const BlockedServiceCall&) = delete;

    void finish() {
        release_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    bool entered() const { return entered_; }

    bool completed() const { return completed_; }

    bool timed_out() const { return timed_out_; }

    int value_before() const { return before_; }

    int value_after() const { return after_; }

private:
    static constexpr ULONGLONG kHoldTimeoutMs = 5000;

    static void callback(const void* table, void* user) {
        auto& self = *static_cast<BlockedServiceCall*>(user);
        const auto& service = *static_cast<const LifecycleService*>(table);
        self.before_ = service.value();
        self.entered_ = true;
        wait_until([&] { return self.release_.load(); }, kHoldTimeoutMs);
        self.timed_out_ = !self.release_.load();
        service.increment();
        self.after_ = service.value();
    }

    std::atomic<bool> entered_{false};
    std::atomic<bool> release_{false};
    bool completed_ = false;
    bool timed_out_ = false;
    int before_ = 0;
    int after_ = 0;
    std::thread thread_;
};

// ---------------------------------------------------------------------------
// Dependency chain helpers: indirect -> intermediate -> provider <- consumer

constexpr std::array<const wchar_t*, 4> kChainStems{L"provider", L"consumer", L"intermediate",
                                                    L"indirect"};
constexpr std::array<const char*, 4> kChainFiles{"provider.dll", "consumer.dll", "intermediate.dll",
                                                 "indirect.dll"};

using ChainPaths = std::array<fs::path, kChainStems.size()>;

ChainPaths chain_paths(const fs::path& root) {
    ChainPaths paths;
    for (size_t index = 0; index < paths.size(); ++index) {
        auto module = loaded_module(root, kChainStems[index]);
        ARK_CHECK(module);
        paths[index] = module_path(module);
    }
    return paths;
}

void expect_chain_unmapped(const ChainPaths& paths) {
    // Compare by path: a new image can reuse an old HMODULE address.
    for (auto& path : paths) {
        ARK_CHECK(!GetModuleHandleW(path.c_str()));
    }
}

void expect_no_chain_shutdowns(const fs::path& root) {
    for (auto stem : kChainStems) {
        ARK_CHECK(shutdowns(loaded_module(root, stem)) == 0);
    }
}

// Every chain member is active, sees the expected service value and receives
// exactly one frame per dispatch.
void expect_functioning_chain(PluginManager& manager, const fs::path& root, const ArkFrameV1& frame,
                              int value) {
    const auto views = manager.views();
    std::array<int, kChainStems.size()> before{};
    for (size_t index = 0; index < kChainStems.size(); ++index) {
        bool active = false;
        for (auto& view : views) {
            if (view.file == kChainFiles[index]) {
                active = view.active;
            }
        }
        ARK_CHECK(active);
        auto module = loaded_module(root, kChainStems[index]);
        ARK_CHECK(module);
        before[index] = frames(module);
        if (index != 0) {
            ARK_CHECK(dependency_value(module) == value);
        }
    }
    ARK_CHECK(sample_service(manager).value == value);
    manager.dispatch(CallbackKind::Frame, frame);
    for (size_t index = 0; index < kChainStems.size(); ++index) {
        ARK_CHECK(frames(loaded_module(root, kChainStems[index])) == before[index] + 1);
    }
}

// ---------------------------------------------------------------------------
// Basic fixture: callback draining, refusal, leases and tasks

void test_queued_reload_batch(const Environment& env) {
    const auto root = env.root / "queued";
    fs::create_directories(root / "plugins");
    for (auto file :
         {L"provider.dll", L"consumer.dll", L"intermediate.dll", L"indirect.dll", L"fixture.dll"}) {
        install_fixture(env, root, file);
    }
    PluginManager manager(root, true);
    ARK_CHECK(manager.load_now("consumer.dll"));
    ARK_CHECK(manager.load_now("indirect.dll"));
    ARK_CHECK(manager.load_now("fixture.dll"));
    const auto old = chain_paths(root);
    const auto marker = module_path(loaded_module(root));
    refuse_preflight(loaded_module(root, L"provider"), true);

    // Queue before the worker starts so these requests form one deterministic batch.
    manager.request("reload", "consumer.dll");
    manager.request("reload", "indirect.dll");
    manager.request("reload", "provider.dll");
    manager.request("reload", "consumer.dll");
    manager.request("unload", "fixture.dll");
    manager.start();
    wait_until([&] { return !GetModuleHandleW(marker.c_str()); }, 5000);
    ARK_CHECK(!GetModuleHandleW(marker.c_str()));

    // A provider veto must also cover the queued consumer reloads.
    ARK_CHECK(chain_paths(root) == old);
    expect_no_chain_shutdowns(root);
    expect_functioning_chain(manager, root, env.frame, kServiceInitialValue);
    refuse_preflight(loaded_module(root, L"provider"), false);
    manager.stop();
}

void test_abi_rejection(PluginManager& manager) {
    ARK_CHECK(!manager.load_now("bad_abi.dll"));
}

void test_frame_callback_drain(PluginManager& manager, const Environment& env) {
    ARK_CHECK(manager.load_now("fixture.dll"));
    HMODULE module = loaded_module(env.root);
    ARK_CHECK(module);
    const auto block = export_function<void (*)()>(module, "fixture_block");
    const auto release = export_function<void (*)()>(module, "fixture_release");
    const auto entered = export_function<int (*)()>(module, "fixture_entered");
    const auto count = export_function<int (*)()>(module, "fixture_count");

    manager.dispatch(CallbackKind::Frame, env.frame);
    ARK_CHECK(count() == 1);

    // Loaded modules run from a shadow copy, so the source stays replaceable.
    install_fixture(env, env.root, L"fixture.dll");

    block();
    std::thread render([&] { manager.dispatch(CallbackKind::Frame, env.frame); });
    wait_until([&] { return entered() != 0; }, kEnterTimeoutMs);
    ARK_CHECK(entered());
    ARK_CHECK(!manager.unload_now("fixture.dll", kShortUnloadTimeoutMs));
    ARK_CHECK(loaded_module(env.root) == module);
    release();
    render.join();
    ARK_CHECK(manager.unload_now("fixture.dll"));
    ARK_CHECK(!loaded_module(env.root));
    manager.dispatch(CallbackKind::Frame, env.frame);
}

void test_shutdown_refusal(PluginManager& manager, const Environment& env) {
    ARK_CHECK(manager.load_now("fixture.dll"));
    HMODULE module = loaded_module(env.root);
    ARK_CHECK(module);
    refuse_shutdown(module, true);
    ARK_CHECK(!manager.unload_now("fixture.dll"));
    ARK_CHECK(loaded_module(env.root) == module);
    refuse_shutdown(module, false);
    ARK_CHECK(manager.unload_now("fixture.dll"));
    ARK_CHECK(!loaded_module(env.root));
}

void test_frame_leases(PluginManager& manager, const Environment& env) {
    ARK_CHECK(manager.load_now("fixture.dll"));
    std::vector<std::unique_ptr<Invocation>> leases;
    manager.dispatch(CallbackKind::Frame, env.frame, &leases);
    ARK_CHECK(!manager.unload_now("fixture.dll", kShortUnloadTimeoutMs));
    leases.clear();
    ARK_CHECK(manager.unload_now("fixture.dll"));
    ARK_CHECK(!loaded_module(env.root));
}

void test_game_task_drain(PluginManager& manager, const Environment& env) {
    ARK_CHECK(manager.load_now("fixture.dll"));
    HMODULE module = loaded_module(env.root);
    ARK_CHECK(module);
    const auto block = export_function<void (*)()>(module, "fixture_block");
    const auto release = export_function<void (*)()>(module, "fixture_release");
    const auto entered = export_function<int (*)()>(module, "fixture_entered");

    block();
    ARK_CHECK(call_bool(module, "fixture_queue"));
    std::thread native([&] { manager.dispatch_game_tasks(); });
    wait_until([&] { return entered() != 0; }, kEnterTimeoutMs);
    ARK_CHECK(entered());
    ARK_CHECK(!manager.unload_now("fixture.dll", kShortUnloadTimeoutMs));
    ARK_CHECK(loaded_module(env.root) == module);
    release();
    native.join();
    ARK_CHECK(manager.unload_now("fixture.dll"));
    ARK_CHECK(!loaded_module(env.root));
}

void test_cancelled_game_task(PluginManager& manager, const Environment& env) {
    ARK_CHECK(manager.load_now("fixture.dll"));
    HMODULE module = loaded_module(env.root);
    ARK_CHECK(module);
    ARK_CHECK(call_bool(module, "fixture_queue"));
    ARK_CHECK(manager.unload_now("fixture.dll"));
    // The cancelled task must not call into the unloaded DLL.
    manager.dispatch_game_tasks();
    ARK_CHECK(!loaded_module(env.root));
}

void test_repeated_reload_cycles(PluginManager& manager, const Environment& env) {
    for (int cycle = 0; cycle < kReloadCycles; ++cycle) {
        ARK_CHECK(manager.load_now("fixture.dll"));
        manager.dispatch(CallbackKind::Frame, env.frame);
        ARK_CHECK(manager.unload_now("fixture.dll"));
        ARK_CHECK(!loaded_module(env.root));
    }
}

// ---------------------------------------------------------------------------
// Service fixtures: dependencies, services, input adapters and cascades

void install_service_fixtures(const Environment& env) {
    for (auto fixture :
         {L"provider.dll", L"consumer.dll", L"intermediate.dll", L"indirect.dll", L"undeclared.dll",
          L"cycle_a.dll", L"cycle_b.dll", L"missing_dependency.dll", L"unsafe_dependency.dll",
          L"failed_init.dll", L"failed_init_refuse.dll"}) {
        install_fixture(env, env.root, fixture);
    }
}

void test_dependency_validation(PluginManager& manager, const Environment& env) {
    ARK_CHECK(!manager.load_now("../provider.dll"));
    ARK_CHECK(!manager.load_now("unsafe_dependency.dll"));
    ARK_CHECK(!loaded_module(env.root, L"unsafe_dependency"));
    ARK_CHECK(!manager.load_now("missing_dependency.dll"));
    ARK_CHECK(!loaded_module(env.root, L"missing_dependency"));
    ARK_CHECK(!manager.load_now("cycle_a.dll"));
    ARK_CHECK(!loaded_module(env.root, L"cycle_a"));
    ARK_CHECK(!loaded_module(env.root, L"cycle_b"));
}

void test_input_adapters(PluginManager& manager, HMODULE provider, HMODULE peer) {
    const auto captures = [](HMODULE module) {
        return call_int(module, "fixture_input_captures");
    };
    const auto routes = [](HMODULE module) {
        return call_int(module, "fixture_input_routes");
    };
    const auto queries = [](HMODULE module) {
        return call_int(module, "fixture_input_queries");
    };
    const auto capture = [](HMODULE module, bool mouse, bool keyboard) {
        export_function<void (*)(bool, bool)>(module, "fixture_input_capture")(mouse, keyboard);
    };
    const auto detach = [](HMODULE module) {
        return call_bool(module, "fixture_input_detach");
    };
    const auto attach = [](HMODULE module) {
        return call_bool(module, "fixture_input_attach");
    };
    const auto route = [&](uint32_t message) {
        return manager.route_window_input(message, kWparam, kLparam);
    };

    // Every adapter is called; no short circuit once a message is handled.
    int provider_before = captures(provider);
    int peer_before = captures(peer);
    manager.set_input_capture(true, true);
    ARK_CHECK(captures(provider) == provider_before + 1 && captures(peer) == peer_before + 1);
    provider_before = routes(provider);
    peer_before = routes(peer);
    ARK_CHECK(route(kProviderMessage));
    ARK_CHECK(routes(provider) == provider_before + 1 && routes(peer) == peer_before + 1);
    ARK_CHECK(route(kPeerMessage));
    provider_before = queries(provider);
    peer_before = queries(peer);
    ARK_CHECK(manager.input_protected());
    ARK_CHECK(queries(provider) == provider_before + 1 && queries(peer) == peer_before + 1);

    // Results are combined across adapters.
    capture(provider, false, false);
    ARK_CHECK(manager.input_protected());
    ARK_CHECK(!route(kProviderMessage));
    ARK_CHECK(route(kPeerMessage));
    capture(provider, true, true);
    capture(peer, false, false);
    ARK_CHECK(manager.input_protected() && route(kProviderMessage));
    ARK_CHECK(!route(kPeerMessage));

    // Detaching one adapter leaves the other in place.
    ARK_CHECK(detach(provider));
    ARK_CHECK(manager.has_input_adapter());
    capture(peer, true, true);
    ARK_CHECK(manager.input_protected());
    ARK_CHECK(route(kPeerMessage));
    ARK_CHECK(!route(kProviderMessage));
    ARK_CHECK(attach(provider));
    manager.set_input_capture(true, true);

    // Detaching waits for that owner's running callback only.
    {
        ManualResetEvent entered;
        ManualResetEvent release;
        ManualResetEvent removing;
        ManualResetEvent removed;
        ARK_CHECK(entered.get() && release.get() && removing.get() && removed.get());
        export_function<void (*)(HANDLE, HANDLE)>(peer, "fixture_input_block")(entered.get(),
                                                                               release.get());
        bool routed = false;
        bool detached = false;
        std::thread callback([&] { routed = route(kPeerMessage); });
        ARK_CHECK(WaitForSingleObject(entered.get(), kEnterTimeoutMs) == WAIT_OBJECT_0);
        std::thread removal([&] {
            SetEvent(removing.get());
            detached = detach(peer);
            SetEvent(removed.get());
        });
        ARK_CHECK(WaitForSingleObject(removing.get(), kEnterTimeoutMs) == WAIT_OBJECT_0);
        ARK_CHECK(WaitForSingleObject(removed.get(), 20) == WAIT_TIMEOUT);
        ARK_CHECK(route(kProviderMessage)); // The other owner remains callable.
        ARK_CHECK(SetEvent(release.get()));
        callback.join();
        removal.join();
        ARK_CHECK(routed && detached);
    }

    // The detached peer is no longer called.
    const int peer_routes = routes(peer);
    const int peer_queries = queries(peer);
    ARK_CHECK(route(kProviderMessage));
    ARK_CHECK(!route(kPeerMessage));
    ARK_CHECK(manager.input_protected());
    ARK_CHECK(routes(peer) == peer_routes && queries(peer) == peer_queries);
    ARK_CHECK(detach(provider));
    ARK_CHECK(!manager.has_input_adapter());
    ARK_CHECK(!route(kProviderMessage) && !manager.input_protected());
    ARK_CHECK(attach(peer));
    ARK_CHECK(attach(provider));
    manager.set_input_capture(true, true);
}

void test_service_access(PluginManager& manager, const Environment& env) {
    // Only the consumer is requested. Its initialize calls a fully active
    // provider, proving dependency-first initialization across real DLLs.
    ARK_CHECK(manager.load_now("consumer.dll"));
    const auto provider = loaded_module(env.root, L"provider");
    const auto consumer = loaded_module(env.root, L"consumer");
    ARK_CHECK(provider && consumer);

    const auto* service = static_cast<const LifecycleService*>(
        query_service(consumer, kServiceAbi, sizeof(LifecycleService)));
    ARK_CHECK(service && service->value() == kServiceInitialValue);
    ARK_CHECK(!query_service(consumer, kServiceAbi + 1, sizeof(LifecycleService)));
    ARK_CHECK(!query_service(consumer, kServiceAbi, sizeof(LifecycleService) + 1));
    ARK_CHECK(query_service(consumer, kServiceAbi, sizeof(LifecycleService) - 1) == service);
    ARK_CHECK(!call_bool(provider, "fixture_duplicate_service"));

    // Services are only reachable from declared direct dependencies.
    ARK_CHECK(manager.load_now("indirect.dll"));
    ARK_CHECK(manager.load_now("undeclared.dll"));
    const auto indirect = loaded_module(env.root, L"indirect");
    const auto undeclared = loaded_module(env.root, L"undeclared");
    ARK_CHECK(indirect && undeclared);
    ARK_CHECK(!query_service(indirect, kServiceAbi, sizeof(LifecycleService)));
    ARK_CHECK(!query_service(undeclared, kServiceAbi, sizeof(LifecycleService)));

    test_input_adapters(manager, provider, undeclared);
    ARK_CHECK(manager.unload_now("indirect.dll"));
    ARK_CHECK(manager.unload_now("intermediate.dll"));
    ARK_CHECK(manager.unload_now("undeclared.dll"));
}

void test_provider_input_and_tasks(PluginManager& manager, const Environment& env) {
    const auto provider = loaded_module(env.root, L"provider");
    ARK_CHECK(manager.has_input_adapter());
    manager.set_input_capture(true, true);
    ARK_CHECK(manager.route_window_input(kProviderMessage, kWparam, kLparam));
    ARK_CHECK(!manager.route_window_input(kPeerMessage, kWparam, kLparam));
    ARK_CHECK(manager.input_protected());
    manager.set_input_capture(false, false);
    ARK_CHECK(!manager.route_window_input(kProviderMessage, kWparam, kLparam));
    ARK_CHECK(!manager.input_protected());

    // A plugin-pumped task runs once and is not dispatched again.
    ARK_CHECK(call_bool(provider, "fixture_queue_task"));
    export_function<void (*)()>(provider, "fixture_pump_tasks")();
    ARK_CHECK(call_int(provider, "fixture_tasks") == 1);
    manager.dispatch_game_tasks();
    ARK_CHECK(call_int(provider, "fixture_tasks") == 1);
}

void test_provider_protection(PluginManager& manager, const Environment& env) {
    const auto consumer = loaded_module(env.root, L"consumer");
    const auto* service = static_cast<const LifecycleService*>(
        query_service(consumer, kServiceAbi, sizeof(LifecycleService)));
    ARK_CHECK(service);

    // A provider with an active consumer cannot be unloaded and keeps working.
    const auto frames_before = service->frames();
    ARK_CHECK(!manager.unload_now("PROVIDER.DLL"));
    manager.dispatch(CallbackKind::Frame, env.frame);
    ARK_CHECK(service->frames() == frames_before + 1);
    ARK_CHECK(service->value() == kServiceInitialValue);
    manager.set_input_capture(true, true);
    ARK_CHECK(manager.input_protected());
    ARK_CHECK(manager.route_window_input(kProviderMessage, kWparam, kLparam));

    // A refused consumer shutdown keeps its provider and dependency access.
    refuse_shutdown(consumer, true);
    ARK_CHECK(!manager.unload_now("consumer.dll"));
    ARK_CHECK(!manager.unload_now("provider.dll"));
    ARK_CHECK(service->value() == kServiceInitialValue);
    ARK_CHECK(query_service(consumer, kServiceAbi, sizeof(LifecycleService)) == service);

    refuse_shutdown(consumer, false);
    ARK_CHECK(manager.unload_now("consumer.dll"));
    ARK_CHECK(!loaded_module(env.root, L"consumer"));
    ARK_CHECK(sample_service(manager).consumer_shutdowns == 3);
    ARK_CHECK(manager.unload_now("provider.dll"));
    ARK_CHECK(!loaded_module(env.root, L"provider"));
    ARK_CHECK(!manager.has_input_adapter());
    manager.dispatch_game_tasks();
    manager.dispatch(CallbackKind::Frame, env.frame);
}

// A provider with a blocked external service call cannot be unloaded until
// the call returns.
void test_external_service_lease(PluginManager& manager, const Environment& env) {
    ARK_CHECK(manager.load_now("provider.dll"));
    const auto provider = loaded_module(env.root, L"provider");
    ARK_CHECK(provider);
    const auto provider_path = module_path(provider);
    {
        BlockedServiceCall call(manager);
        wait_until([&] { return call.entered(); }, kEnterTimeoutMs);
        ARK_CHECK(call.entered());
        ARK_CHECK(!manager.unload_now("provider.dll", kShortUnloadTimeoutMs));
        ARK_CHECK(GetModuleHandleW(provider_path.c_str()));
        ARK_CHECK(shutdowns(provider) == 0);
        call.finish();
        ARK_CHECK(call.completed() && !call.timed_out() && call.value_before() == 41 &&
                  call.value_after() == 42);
    }
    ARK_CHECK(manager.unload_now("provider.dll"));
    ARK_CHECK(!GetModuleHandleW(provider_path.c_str()));
}

void test_cascade_reload(PluginManager& manager, const Environment& env) {
    const auto& root = env.root;
    const auto& frame = env.frame;
    ARK_CHECK(manager.load_now("consumer.dll"));
    ARK_CHECK(manager.load_now("indirect.dll"));
    expect_functioning_chain(manager, root, frame, kServiceInitialValue);
    increment_service(manager);
    ARK_CHECK(sample_service(manager).value == kServiceInitialValue + 1);

    // Reloading the provider restarts every consumer with fresh state.
    auto old = chain_paths(root);
    ARK_CHECK(manager.reload_now("PROVIDER.DLL"));
    expect_chain_unmapped(old);
    for (auto stem : kChainStems) {
        ARK_CHECK(frames(loaded_module(root, stem)) == 0);
    }
    ARK_CHECK(sample_service(manager).consumer_shutdowns == 0);
    expect_functioning_chain(manager, root, frame, kServiceInitialValue);
}

void test_reload_preflight_veto(PluginManager& manager, const Environment& env) {
    // A provider or transitive-consumer veto is atomic for the whole graph.
    for (auto veto_stem : {L"provider", L"indirect"}) {
        const auto old = chain_paths(env.root);
        const auto veto = loaded_module(env.root, veto_stem);
        refuse_preflight(veto, true);
        ARK_CHECK(!manager.reload_now("provider.dll"));
        ARK_CHECK(chain_paths(env.root) == old);
        expect_no_chain_shutdowns(env.root);
        ARK_CHECK(sample_service(manager).consumer_shutdowns == 0);
        expect_functioning_chain(manager, env.root, env.frame, kServiceInitialValue);
        refuse_preflight(veto, false);
    }
}

void test_reload_refusal_restores_consumers(PluginManager& manager, const Environment& env) {
    const auto consumer = loaded_module(env.root, L"consumer");
    const auto provider = loaded_module(env.root, L"provider");
    const auto provider_path = module_path(provider);
    refuse_shutdown(consumer, true);
    ARK_CHECK(!manager.reload_now("provider.dll"));
    ARK_CHECK(GetModuleHandleW(provider_path.c_str()) == provider);
    ARK_CHECK(shutdowns(provider) == 0);
    // Consumers stopped earlier in this attempt must be restored on refusal.
    for (auto stem : {L"intermediate", L"indirect"}) {
        const auto restored = loaded_module(env.root, stem);
        ARK_CHECK(restored);
        ARK_CHECK(dependency_value(restored) == kServiceInitialValue);
    }
    ARK_CHECK(sample_service(manager).value == kServiceInitialValue);
    refuse_shutdown(consumer, false);

    const auto old = chain_paths(env.root);
    ARK_CHECK(manager.reload_now("provider.dll"));
    expect_chain_unmapped(old);
    expect_functioning_chain(manager, env.root, env.frame, kServiceInitialValue);
}

void test_reload_with_added_dependency(PluginManager& manager, const Environment& env) {
    // A replacement may add an acyclic dependency on another planned member.
    // The consumer loads first in this generation; its recursion restores the
    // intermediate before that member's own turn, but indirect must follow.
    install_fixture(env, env.root, L"consumer_updated.dll", L"consumer.dll");
    const auto old = chain_paths(env.root);
    ARK_CHECK(manager.reload_now("provider.dll"));
    expect_chain_unmapped(old);
    expect_functioning_chain(manager, env.root, env.frame, kServiceInitialValue);

    ARK_CHECK(!manager.unload_now("intermediate.dll"));
    ARK_CHECK(manager.unload_now("consumer.dll"));
    ARK_CHECK(manager.unload_now("indirect.dll"));
    ARK_CHECK(manager.unload_now("intermediate.dll"));
    ARK_CHECK(manager.unload_now("provider.dll"));
    install_fixture(env, env.root, L"consumer.dll");
}

void test_failed_initialization_rollback(PluginManager& manager, const Environment& env) {
    ARK_CHECK(!manager.load_now("failed_init.dll"));
    ARK_CHECK(!loaded_module(env.root, L"failed_init"));
    const auto provider = loaded_module(env.root, L"provider");
    ARK_CHECK(provider);
    manager.dispatch_game_tasks();
    manager.dispatch(CallbackKind::Frame, env.frame);

    // Refused cleanup keeps the failed plugin mapped with its registrations.
    ARK_CHECK(!manager.load_now("failed_init_refuse.dll"));
    const auto failed = loaded_module(env.root, L"failed_init_refuse");
    ARK_CHECK(failed);
    ARK_CHECK(!manager.unload_now("provider.dll"));
    ARK_CHECK(!call_bool(provider, "fixture_publish_failed_service"));

    // A successful rollback frees the service name.
    refuse_shutdown(failed, false);
    ARK_CHECK(manager.unload_now("failed_init_refuse.dll"));
    ARK_CHECK(!loaded_module(env.root, L"failed_init_refuse"));
    ARK_CHECK(call_bool(provider, "fixture_publish_failed_service"));
    ARK_CHECK(manager.unload_now("provider.dll"));
    manager.dispatch_game_tasks();
    manager.dispatch(CallbackKind::Frame, env.frame);
}

void test_process_lifetime_provider(PluginManager& manager, const Environment& env) {
    // Reuse the consumer's declared leaf with a process-lifetime provider.
    install_fixture(env, env.root, L"lifetime_provider.dll", L"provider.dll");
    ARK_CHECK(manager.load_now("provider.dll")); // Provider comes first in the manager's storage.
    ARK_CHECK(manager.load_now("consumer.dll"));
    const auto provider = loaded_module(env.root, L"provider");
    ARK_CHECK(provider);
    ARK_CHECK(sample_service(manager).value == kServiceInitialValue);

    bool called = false;
    const auto observe = [](const void*, void* user) {
        *static_cast<bool*>(user) = true;
    };
    ARK_CHECK(!manager.with_external_service(kServiceName, kServiceAbi + 1,
                                             sizeof(LifecycleService), observe, &called));
    ARK_CHECK(!manager.with_external_service(kServiceName, kServiceAbi,
                                             sizeof(LifecycleService) + 1, observe, &called));
    ARK_CHECK(!called);
    ARK_CHECK(manager.with_external_service(kServiceName, kServiceAbi, sizeof(LifecycleService) - 1,
                                            observe, &called));
    ARK_CHECK(called);
    called = false;

    bool pinned_view = false;
    for (auto& view : manager.views()) {
        if (view.file == "provider.dll") {
            pinned_view = !view.unloadable;
        }
    }
    ARK_CHECK(pinned_view);

    // Neither unload nor reload may touch the pinned provider or its consumer.
    int frames_before = sample_service(manager).frames;
    ARK_CHECK(!manager.unload_now("provider.dll"));
    const auto consumer = loaded_module(env.root, L"consumer");
    ARK_CHECK(consumer);
    const int consumer_frames = frames(consumer);
    ARK_CHECK(!manager.reload_now("provider.dll"));
    ARK_CHECK(loaded_module(env.root, L"consumer") == consumer);
    ARK_CHECK(shutdowns(consumer) == 0);
    ARK_CHECK(shutdowns(provider) == 0);
    manager.dispatch(CallbackKind::Frame, env.frame);
    ARK_CHECK(sample_service(manager).frames == frames_before + 1);
    ARK_CHECK(frames(consumer) == consumer_frames + 1);

    // Consumer shutdown runs while the provider still works; the provider is
    // shut down but stays mapped.
    manager.stop();
    ARK_CHECK(!loaded_module(env.root, L"consumer"));
    ARK_CHECK(loaded_module(env.root, L"provider") == provider);
    ARK_CHECK(call_int(provider, "fixture_consumer_shutdowns") == 1);
    ARK_CHECK(shutdowns(provider) == 1);
    ARK_CHECK(!manager.with_external_service(kServiceName, kServiceAbi, sizeof(LifecycleService),
                                             observe, &called));
    ARK_CHECK(!manager.has_input_adapter());
    ARK_CHECK(!manager.load_now("provider.dll"));
    frames_before = frames(provider);
    manager.dispatch(CallbackKind::Frame, env.frame);
    ARK_CHECK(frames(provider) == frames_before);
    manager.stop();
    ARK_CHECK(shutdowns(provider) == 1);
}

Environment make_environment(int argc, wchar_t** argv) {
    Environment env;
    env.fixtures = fs::absolute(argc > 1 ? argv[1] : L".");
    env.root = env.fixtures / "lifecycle-runtime";
    env.frame.imgui_version = ARK_MODULE_COMPAT_VERSION;
    fs::create_directories(env.root / "plugins");
    install_fixture(env, env.root, L"fixture.dll");
    install_fixture(env, env.root, L"bad_abi.dll");
    return env;
}

void run_all(const Environment& env) {
    test_queued_reload_batch(env);

    PluginManager manager(env.root, true);
    test_abi_rejection(manager);
    test_frame_callback_drain(manager, env);
    test_shutdown_refusal(manager, env);
    test_frame_leases(manager, env);
    test_game_task_drain(manager, env);
    test_cancelled_game_task(manager, env);
    test_repeated_reload_cycles(manager, env);

    install_service_fixtures(env);
    test_dependency_validation(manager, env);
    test_service_access(manager, env);
    test_provider_input_and_tasks(manager, env);
    test_provider_protection(manager, env);
    test_external_service_lease(manager, env);
    test_cascade_reload(manager, env);
    test_reload_preflight_veto(manager, env);
    test_reload_refusal_restores_consumers(manager, env);
    test_reload_with_added_dependency(manager, env);
    test_failed_initialization_rollback(manager, env);
    test_process_lifetime_provider(manager, env);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        run_all(make_environment(argc, argv));
        puts("PASS real DLL lifecycle: drains, scoped external service leases, frame leases, "
             "shadow rebuild, ABI rejection, 25 reload cycles, dependency "
             "initialization/cycles/missing/unsafe leaves, service ABI/size/direct-dependency "
             "checks, provider protection, cascade reload/restoration/fresh state/unmapped "
             "shadows, graph preflight veto, shutdown refusal/retry, queued provider-first "
             "coalescing, failed-init rollback, task/input lifetime, consumer-first stop and "
             "process-lifetime retention");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
