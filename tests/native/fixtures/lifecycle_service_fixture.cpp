// Service lifecycle fixture. Each FIXTURE_KIND builds a separate real DLL that
// runs through the host loader and service API:
//
//   1  provider.dll            publishes the lifecycle service and an input adapter
//   2  consumer.dll            requires the service from its declared provider;
//                              with FIXTURE_ADDED_DEPENDENCY (consumer_updated.dll)
//                              it also depends on intermediate.dll
//   3  intermediate.dll        requires the service and republishes it as the
//                              transitive service
//   4  indirect.dll            depends on intermediate.dll only
//   5  undeclared.dll          input adapter; borrowing an undeclared service fails
//   6  cycle_a.dll             depends on cycle_b.dll
//   7  cycle_b.dll             depends on cycle_a.dll
//   8  missing_dependency.dll  depends on a DLL that does not exist
//   9  unsafe_dependency.dll   declares a dependency outside the plugin directory
//  10  failed_init.dll         registers callbacks, a service and a task, then fails
//  11  lifetime_provider.dll   provider flagged ARK_PLUGIN_PROCESS_LIFETIME
//  12  failed_init_refuse.dll  like 10, but initially refuses its cleanup
#include "lifecycle_service_fixture.h"

#include <windows.h>

#include <ark/plugin.h>

#ifndef FIXTURE_KIND
#error FIXTURE_KIND must identify the lifecycle DLL variant
#endif

using lifecycle_fixture::LifecycleService;

namespace {

constexpr int kKind = FIXTURE_KIND;
static_assert(kKind >= 1 && kKind <= 12, "FIXTURE_KIND must be in 1..12");

#ifdef FIXTURE_ADDED_DEPENDENCY
constexpr bool kAddedDependency = true;
static_assert(kKind == 2, "FIXTURE_ADDED_DEPENDENCY only applies to the consumer");
#else
constexpr bool kAddedDependency = false;
#endif

constexpr uint32_t kFlags = kKind == 11 ? ARK_PLUGIN_PROCESS_LIFETIME : ARK_PLUGIN_DEFAULT;
// The undeclared fixture claims a different message so the tests can tell
// both adapters apart.
constexpr uint32_t kRoutedMessage = kKind == 5 ? 0x4322 : 0x4321;
constexpr uintptr_t kRoutedWparam = 23;
constexpr intptr_t kRoutedLparam = -7;
constexpr DWORD kInputBlockTimeoutMs = 5000;

// Dependency leaves are matched case-insensitively.
constexpr const char* kProviderDependency[] = {"PrOvIdEr.DlL"};
constexpr const char* kUpdatedConsumerDependencies[] = {"provider.dll", "intermediate.dll"};
constexpr const char* kIntermediateDependency[] = {"intermediate.dll"};
constexpr const char* kCycleADependency[] = {"cycle_b.dll"};
constexpr const char* kCycleBDependency[] = {"cycle_a.dll"};
constexpr const char* kMissingDependency[] = {"does_not_exist.dll"};
constexpr const char* kUnsafeDependency[] = {"../provider.dll"};

struct DependencyList {
    const char* const* names = nullptr;
    size_t count = 0;
};

template <size_t N>
constexpr DependencyList list_of(const char* const (&names)[N]) {
    return {names, N};
}

constexpr DependencyList declared_dependencies() {
    switch (kKind) {
    case 2:
        return kAddedDependency ? list_of(kUpdatedConsumerDependencies)
                                : list_of(kProviderDependency);
    case 3:
    case 10:
    case 12:
        return list_of(kProviderDependency);
    case 4:
        return list_of(kIntermediateDependency);
    case 6:
        return list_of(kCycleADependency);
    case 7:
        return list_of(kCycleBDependency);
    case 8:
        return list_of(kMissingDependency);
    case 9:
        return list_of(kUnsafeDependency);
    default:
        return {};
    }
}

const ArkPluginApiV1* api = nullptr;
const LifecycleService* dependency = nullptr;

volatile LONG value = 41;
volatile LONG frames = 0;
volatile LONG consumer_shutdowns = 0;
volatile LONG shutdowns = 0;
volatile LONG tasks = 0;
volatile LONG refuse_shutdown = kKind == 12 ? 1 : 0;
volatile LONG refuse_preflight = 0;

bool mouse_capture = false;
bool keyboard_capture = false;
volatile LONG input_captures = 0;
volatile LONG input_routes = 0;
volatile LONG input_queries = 0;
volatile LONG block_next_route = 0;
HANDLE route_entered = nullptr;
HANDLE route_release = nullptr;

int service_value() {
    return value;
}

void service_increment() {
    InterlockedIncrement(&value);
}

int service_frames() {
    return frames;
}

int service_consumer_shutdowns() {
    return consumer_shutdowns;
}

void service_consumer_stopped() {
    InterlockedIncrement(&consumer_shutdowns);
}

const LifecycleService service{service_value, service_increment, service_frames,
                               service_consumer_shutdowns, service_consumer_stopped};

void on_frame(void*, const ArkFrameV1*) {
    InterlockedIncrement(&frames);
}

void on_task(void*) {
    InterlockedIncrement(&tasks);
}

void set_capture(void*, bool mouse, bool keyboard) {
    InterlockedIncrement(&input_captures);
    mouse_capture = mouse;
    keyboard_capture = keyboard;
}

bool route_message(void*, uint32_t message, uintptr_t wparam, intptr_t lparam) {
    InterlockedIncrement(&input_routes);
    if (InterlockedExchange(&block_next_route, 0)) {
        SetEvent(route_entered);
        if (WaitForSingleObject(route_release, kInputBlockTimeoutMs) != WAIT_OBJECT_0) {
            return false;
        }
    }
    return message == kRoutedMessage && wparam == kRoutedWparam && lparam == kRoutedLparam &&
           mouse_capture;
}

bool protected_input(void*) {
    InterlockedIncrement(&input_queries);
    return keyboard_capture;
}

const ArkInputAdapterV1 input_adapter{sizeof(ArkInputAdapterV1), nullptr, set_capture,
                                      route_message, protected_input};

const LifecycleService* require(const char* name) {
    return static_cast<const LifecycleService*>(api->require_service(
        api->owner, name, lifecycle_fixture::kServiceAbi, sizeof(LifecycleService)));
}

bool publish(const char* name, const void* table) {
    return api->provide_service(api->owner, name, lifecycle_fixture::kServiceAbi, table,
                                sizeof(LifecycleService));
}

bool initialize_provider() {
    return publish(lifecycle_fixture::kServiceName, &service) &&
           api->set_input_adapter(api->owner, &input_adapter) &&
           api->on_frame(api->owner, on_frame, nullptr);
}

// Runs only after the provider is fully active (dependency-first initialization).
bool initialize_consumer() {
    dependency = require(lifecycle_fixture::kServiceName);
    if (!dependency || dependency->value() < 41) {
        return false;
    }
    if constexpr (kAddedDependency) {
        const auto* added = require(lifecycle_fixture::kTransitiveServiceName);
        if (!added || added->value() != dependency->value()) {
            return false;
        }
    }
    dependency->increment();
    return api->on_frame(api->owner, on_frame, nullptr) != 0;
}

bool initialize_intermediate() {
    dependency = require(lifecycle_fixture::kServiceName);
    return dependency && publish(lifecycle_fixture::kTransitiveServiceName, dependency) &&
           api->on_frame(api->owner, on_frame, nullptr) != 0;
}

// The provider's service is only reachable through the declared intermediate.
bool initialize_indirect() {
    dependency = require(lifecycle_fixture::kTransitiveServiceName);
    return dependency && require(lifecycle_fixture::kServiceName) == nullptr &&
           api->on_frame(api->owner, on_frame, nullptr) != 0;
}

// A provider without a declared dependency cannot be borrowed.
bool initialize_undeclared() {
    return require(lifecycle_fixture::kServiceName) == nullptr &&
           api->set_input_adapter(api->owner, &input_adapter);
}

// Registers everything it can, then fails; the host must roll it all back.
bool initialize_failing() {
    publish(lifecycle_fixture::kFailedServiceName, &service);
    api->on_frame(api->owner, on_frame, nullptr);
    api->queue_game_task(api->owner, on_task, nullptr);
    return false;
}

} // namespace

ARK_EXPORT const ArkPluginInfoV1* ark_plugin_query() {
    static const ArkPluginInfoV1 info{
        sizeof(ArkPluginInfoV1),
        ARK_ABI_VERSION,
        ARK_MODULE_COMPAT_VERSION,
        "Service lifecycle fixture",
        "1.0",
        "Tests",
        kFlags,
        declared_dependencies().names,
        declared_dependencies().count,
    };
    return &info;
}

ARK_EXPORT bool ark_plugin_initialize(const ArkPluginApiV1* host) {
    api = host;
    switch (kKind) {
    case 1:
    case 11:
        return initialize_provider();
    case 2:
        return initialize_consumer();
    case 3:
        return initialize_intermediate();
    case 4:
        return initialize_indirect();
    case 5:
        return initialize_undeclared();
    case 10:
    case 12:
        return initialize_failing();
    default:
        return true;
    }
}

ARK_EXPORT bool ark_plugin_can_unload(char* reason, size_t capacity) {
    if (!refuse_preflight) {
        return true;
    }
    if (capacity) {
        constexpr char kMessage[] = "Fixture preflight veto";
        size_t index = 0;
        for (; index + 1 < capacity && kMessage[index]; ++index) {
            reason[index] = kMessage[index];
        }
        reason[index] = 0;
    }
    return false;
}

ARK_EXPORT bool ark_plugin_shutdown() {
    if (refuse_shutdown) {
        return false;
    }
    if (dependency) {
        dependency->consumer_stopped();
    }
    InterlockedIncrement(&shutdowns);
    return true;
}

ARK_EXPORT const void* fixture_query(uint32_t abi, size_t size) {
    return api->require_service(api->owner, lifecycle_fixture::kServiceName, abi, size);
}

ARK_EXPORT bool fixture_duplicate_service() {
    return publish(lifecycle_fixture::kServiceName, &service);
}

ARK_EXPORT bool fixture_publish_failed_service() {
    return publish(lifecycle_fixture::kFailedServiceName, &service);
}

ARK_EXPORT int fixture_value() {
    return value;
}

ARK_EXPORT int fixture_frames() {
    return frames;
}

ARK_EXPORT int fixture_shutdowns() {
    return shutdowns;
}

ARK_EXPORT int fixture_consumer_shutdowns() {
    return consumer_shutdowns;
}

ARK_EXPORT int fixture_dependency_value() {
    return dependency ? dependency->value() : -1;
}

ARK_EXPORT void fixture_refuse(int refuse) {
    InterlockedExchange(&refuse_shutdown, refuse);
}

ARK_EXPORT void fixture_preflight_refuse(int refuse) {
    InterlockedExchange(&refuse_preflight, refuse);
}

ARK_EXPORT bool fixture_queue_task() {
    return api->queue_game_task(api->owner, on_task, nullptr);
}

ARK_EXPORT int fixture_tasks() {
    return tasks;
}

ARK_EXPORT void fixture_pump_tasks() {
    api->pump_game_tasks(api->owner);
}

// Input adapter controls; meaningful for the provider and undeclared variants.
ARK_EXPORT bool fixture_input_attach() {
    return api->set_input_adapter(api->owner, &input_adapter);
}

ARK_EXPORT bool fixture_input_detach() {
    return api->set_input_adapter(api->owner, nullptr);
}

ARK_EXPORT void fixture_input_capture(bool mouse, bool keyboard) {
    set_capture(nullptr, mouse, keyboard);
}

ARK_EXPORT int fixture_input_captures() {
    return input_captures;
}

ARK_EXPORT int fixture_input_routes() {
    return input_routes;
}

ARK_EXPORT int fixture_input_queries() {
    return input_queries;
}

// Makes the next routed message signal `entered` and wait for `release`.
ARK_EXPORT void fixture_input_block(HANDLE entered, HANDLE release) {
    route_entered = entered;
    route_release = release;
    InterlockedExchange(&block_next_route, 1);
}
