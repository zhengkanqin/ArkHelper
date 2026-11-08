// Built-in damage module: native capture, battle events, per-actor statistics
// and CSV export. ArkHelper.exe owns the user interface.
#include <atomic>
#include <cstdio>
#include <exception>
#include <mutex>
#include <stdexcept>

#include <ark/battle.h>
#include <ark/plugin.h>

#include "meter.h"

namespace ark::damage_meter {
namespace {

constexpr std::size_t kMaxReadCapacity = 4096;

std::atomic<Backend*> active_backend{nullptr};
Meter meter;

// --- Battle services -------------------------------------------------------------------------

std::size_t read_events(std::uint64_t after, ArkEventV1* out, std::size_t capacity,
                        ArkReadStatusV1* status) {
    Backend* backend = active_backend.load();
    if (!backend || backend->stopping || !out || !capacity || capacity > kMaxReadCapacity ||
        (status && status->size < sizeof(*status))) {
        return 0;
    }
    return backend->history.read(after, out, capacity, status);
}

bool read_frame(ArkBattleFrameV1* out) {
    Backend* backend = active_backend.load();
    return backend && out && out->size >= sizeof(*out) && backend->current_frame(*out);
}

bool publish_test_event(const ArkEventV1* event) {
    Backend* backend = active_backend.load();
    if (!backend || !backend->host.test_host || backend->stopping || !event ||
        event->size < sizeof(*event)) {
        return false;
    }
    backend->history.publish(*event);
    return true;
}

const ArkBattleEventsApiV1 kEventsApi{sizeof(ArkBattleEventsApiV1), ARK_BATTLE_ABI_VERSION,
                                      read_events, read_frame};
const ArkBattleTestApiV1 kTestApi{sizeof(ArkBattleTestApiV1), ARK_BATTLE_ABI_VERSION,
                                  publish_test_event};

// --- SDK callbacks ---------------------------------------------------------------------------

void on_frame(void* user, const ArkFrameV1*) {
    meter.update(*static_cast<Backend*>(user));
}

// --- Lifecycle -------------------------------------------------------------------------------

bool host_is_compatible(const ArkPluginApiV1* host) {
    return host && host->size >= sizeof(ArkPluginApiV1) && host->abi_version == ARK_ABI_VERSION &&
           host->imgui_version == ARK_MODULE_COMPAT_VERSION && host->queue_game_task &&
           host->pump_game_tasks && host->on_frame && host->provide_service &&
           host->create_native_hook && host->enable_native_hook &&
           host->remove_native_hook && host->log && host->root_directory;
}

// Throws on the first failed registration.
void register_with_host(const ArkPluginApiV1& host, Backend& backend) {
    if (!host.provide_service(host.owner, ARK_BATTLE_SERVICE_NAME, ARK_BATTLE_ABI_VERSION,
                              &kEventsApi, sizeof(kEventsApi))) {
        throw std::runtime_error("Could not provide battle event service");
    }
    if (host.test_host &&
        !host.provide_service(host.owner, ARK_BATTLE_TEST_SERVICE_NAME, ARK_BATTLE_ABI_VERSION,
                              &kTestApi, sizeof(kTestApi))) {
        throw std::runtime_error("Could not provide battle test service");
    }
    if (!host.on_frame(host.owner, on_frame, &backend)) {
        throw std::runtime_error("Could not register damage meter frame callback");
    }
}

bool initialize(const ArkPluginApiV1* host) {
    if (!host_is_compatible(host)) {
        return false;
    }
    std::lock_guard lock(meter.mutex());
    if (meter.started() || active_backend.load()) {
        return false;
    }
    meter.start(*host);
    try {
        // On failure the backend stays installed; shutdown() releases it.
        auto* backend = new Backend(*host);
        active_backend.store(backend);
        register_with_host(*host, *backend);
        return true;
    } catch (const std::exception& error) {
        host->log(host->owner, kLogError, error.what());
        return false;
    }
}

// SDK frame/draw callbacks and game tasks are drained by the host before this runs. Returning
// false keeps the module mapped with all native state intact so the host can retry.
bool shutdown() {
    if (Backend* backend = active_backend.load()) {
        backend->stopping = true;
        if (!backend->capture.shutdown()) {
            return false;
        }
        active_backend.store(nullptr);
        delete backend;
    }
    meter.stop();
    return true;
}

} // namespace
} // namespace ark::damage_meter

extern "C" const ArkPluginInfoV1* arkhelp_damage_query() {
    static const ArkPluginInfoV1 info{
        .size = sizeof(ArkPluginInfoV1),
        .abi_version = ARK_ABI_VERSION,
        .imgui_version = ARK_MODULE_COMPAT_VERSION,
        .name = "Damage Meter",
        .version = "1.0.0",
        .author = "ArkHelperRuntime",
        .flags = ARK_PLUGIN_DEFAULT,
    };
    return &info;
}

extern "C" bool arkhelp_damage_initialize(const ArkPluginApiV1* host) {
    return ark::damage_meter::initialize(host);
}

extern "C" bool arkhelp_damage_can_unload(char* reason, size_t capacity) {
    // Capture callbacks are drained by shutdown; no game input is intercepted.
    return true;
}

extern "C" bool arkhelp_damage_shutdown() {
    return ark::damage_meter::shutdown();
}
