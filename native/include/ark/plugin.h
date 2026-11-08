// Private ArkHelper runtime module contract. Product modules are statically linked
// into ArkHelperRuntime.dll. Dynamic DLL entry points are retained for lifecycle
// test fixtures only. All API calls use the supplied owner handle.
// Legacy compatibility fields retain the internal ABI layout; rendering lives in ArkHelper.exe.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define ARK_EXPORT extern "C" __declspec(dllexport)
#else
#define ARK_EXPORT extern "C"
#endif

#define ARK_ABI_VERSION 1u

// Reserved compatibility value from the inherited module contract. No ImGui dependency.
inline constexpr uint32_t ARK_MODULE_COMPAT_VERSION = 19191;

struct ArkFrameV1 {
    uint32_t size = sizeof(ArkFrameV1);
    uint32_t imgui_version = 0;
    void* imgui_context = nullptr;
    double delta_time = 0;
};

using ArkFrameCallback = void (*)(void* user, const ArkFrameV1* frame);
using ArkGameTask = void (*)(void* user);

// Legacy input adapter retained for lifecycle test fixtures. Production leaves
// set_input_adapter null and does not compile the adapter implementation.
struct ArkInputAdapterV1 {
    uint32_t size = sizeof(ArkInputAdapterV1);
    void* user = nullptr;
    void (*set_capture)(void* user, bool mouse, bool keyboard) = nullptr;
    bool (*route_message)(void* user, uint32_t message, uintptr_t wparam,
                          intptr_t lparam) = nullptr;
    bool (*protected_input)(void* user) = nullptr;
};

enum ArkPluginFlags : uint32_t {
    ARK_PLUGIN_DEFAULT = 0,
    // Keeps the module mapped until process exit. Native hooks created through
    // the host do not require this flag.
    ARK_PLUGIN_PROCESS_LIFETIME = 1u,
};

struct ArkPluginApiV1 {
    uint32_t size;
    uint32_t abi_version;
    uint32_t imgui_version;
    uint32_t test_host;
    void* owner;
    const char* root_directory;

    void (*log)(void* owner, uint32_t level, const char* message);
    uint64_t (*on_frame)(void* owner, ArkFrameCallback callback, void* user);
    uint64_t (*on_draw)(void* owner, ArkFrameCallback callback, void* user);
    bool (*remove_callback)(void* owner, uint64_t token);
    bool (*queue_game_task)(void* owner, ArkGameTask task, void* user);
    // Called only by the application's native-thread adapter.
    void (*pump_game_tasks)(void* owner);

    bool (*provide_service)(void* owner, const char* name, uint32_t abi_version, const void* table,
                            size_t table_size);
    // Services can only be acquired from a declared direct dependency. The
    // provider stays loaded until this consumer has fully shut down.
    const void* (*require_service)(void* owner, const char* name, uint32_t abi_version,
                                   size_t minimum_size);
    bool (*set_input_adapter)(void* owner, const ArkInputAdapterV1* adapter);

    // Windows x64 only: at most four integer/pointer register arguments and an
    // integer/pointer/void return; no by-value floating-point, vector or stack
    // arguments. Targets must be process-resident code, never an unloadable DLL.
    //
    // Hooks are created disabled. `original` is a core gate that stays valid
    // until remove returns true. Disabling unpatches without draining calls in
    // progress; removing drains them first and, on failure, must be retried
    // with the detour state intact. Stop callers and remove every hook before
    // destroying plugin state.
    uint64_t (*create_native_hook)(void* owner, void* target, void* detour, void** original);
    bool (*enable_native_hook)(void* owner, uint64_t hook, bool enabled);
    bool (*remove_native_hook)(void* owner, uint64_t hook);
};

struct ArkPluginInfoV1 {
    uint32_t size;
    uint32_t abi_version;
    uint32_t imgui_version;
    const char* name;
    const char* version;
    const char* author;
    uint32_t flags = ARK_PLUGIN_DEFAULT;
    const char* const* dependencies = nullptr; // Plugin DLL leaf file names.
    size_t dependency_count = 0;
};

using ArkPluginQuery = const ArkPluginInfoV1* (*)();
using ArkPluginInitialize = bool (*)(const ArkPluginApiV1* api);
// Returning false keeps the module mapped. SDK callbacks are already drained.
using ArkPluginShutdown = bool (*)();
// Optional `ark_plugin_can_unload`: returning false refuses a reload before any
// consumer stops. Write a NUL-terminated reason when `capacity` is nonzero.
using ArkPluginCanUnload = bool (*)(char* reason, size_t capacity);

// External service access is synchronous and scoped: the table must not escape
// the callback. The host keeps the provider loaded for the whole callback.
using ArkServiceCallback = void (*)(const void* table, void* user);
using ArkHelperRuntimeWithService = bool (*)(const char* name, uint32_t abi_version,
                                         size_t minimum_size, ArkServiceCallback callback,
                                         void* user);

// Passed to `arkhelp_runtime_start` after LoadLibrary has returned. The options
// are copied before the call returns; nullptr uses the core DLL directory.
struct ArkBootOptionsV1 {
    uint32_t size = sizeof(ArkBootOptionsV1);
    uint32_t test_host = 0;                  // Accepted only inside ArkHelperRuntimeHost.exe.
    const wchar_t* root_directory = nullptr; // Optional absolute plugin/log/cache root.
};

using ArkHelperRuntimeStart = bool (*)(const ArkBootOptionsV1* options);
