// Minimal plugin used by the lifecycle tests to exercise callback draining,
// shutdown refusal and game tasks. Built as fixture.dll and, with
// -DTEST_BAD_ABI, as bad_abi.dll (rejected for its SDK ABI version).
#include <windows.h>

#include <ark/plugin.h>

namespace {

#ifdef TEST_BAD_ABI
constexpr uint32_t kDeclaredAbiVersion = 999;
#else
constexpr uint32_t kDeclaredAbiVersion = ARK_ABI_VERSION;
#endif

constexpr DWORD kBlockTimeoutMs = 10000;

const ArkPluginApiV1* api = nullptr;
HANDLE entered_event = nullptr;
HANDLE release_event = nullptr;
volatile LONG block_callbacks = 0;
volatile LONG callback_count = 0;
volatile LONG refuse_shutdown = 0;

void on_frame(void*, const ArkFrameV1*) {
    InterlockedIncrement(&callback_count);
    if (block_callbacks) {
        SetEvent(entered_event);
        WaitForSingleObject(release_event, kBlockTimeoutMs);
    }
}

void on_task(void*) {
    on_frame(nullptr, nullptr);
}

} // namespace

ARK_EXPORT const ArkPluginInfoV1* ark_plugin_query() {
    static const ArkPluginInfoV1 info{
        sizeof(ArkPluginInfoV1),
        kDeclaredAbiVersion,
        ARK_MODULE_COMPAT_VERSION,
        "Lifecycle Fixture",
        "1.0",
        "Tests",
    };
    return &info;
}

ARK_EXPORT bool ark_plugin_initialize(const ArkPluginApiV1* host) {
    api = host;
    entered_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    release_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return entered_event && release_event && api->on_frame(api->owner, on_frame, nullptr);
}

ARK_EXPORT bool ark_plugin_shutdown() {
    if (refuse_shutdown) {
        return false;
    }
    CloseHandle(entered_event);
    CloseHandle(release_event);
    entered_event = nullptr;
    release_event = nullptr;
    return true;
}

// Makes the next callbacks block until fixture_release().
ARK_EXPORT void fixture_block() {
    InterlockedExchange(&block_callbacks, 1);
}

ARK_EXPORT int fixture_entered() {
    return WaitForSingleObject(entered_event, 0) == WAIT_OBJECT_0;
}

ARK_EXPORT void fixture_release() {
    SetEvent(release_event);
}

ARK_EXPORT int fixture_count() {
    return callback_count;
}

ARK_EXPORT void fixture_refuse(int refuse) {
    InterlockedExchange(&refuse_shutdown, refuse);
}

ARK_EXPORT bool fixture_queue() {
    return api->queue_game_task(api->owner, on_task, nullptr);
}
