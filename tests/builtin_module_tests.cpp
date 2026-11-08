#include <windows.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include "plugin_manager.h"

namespace {
ArkPluginApiV1 provider_api{};
std::atomic<int> frames{0};
int starts = 0;
bool veto = false;
bool refuse = false;
int value = 42;
void frame(void*, const ArkFrameV1*) { ++frames; }
const ArkPluginInfoV1* provider_query() {
    static const ArkPluginInfoV1 info{sizeof(ArkPluginInfoV1), ARK_ABI_VERSION,
        ARK_MODULE_COMPAT_VERSION, "Built-in provider", "1", "ArkHelper"};
    return &info;
}
bool provider_init(const ArkPluginApiV1* api) {
    if (api->set_input_adapter) { return false; }
    provider_api = *api;
    frames = 0;
    ++starts;
    return api->provide_service(api->owner, "test.builtin", 1, &value, sizeof(value)) &&
        api->on_frame(api->owner, frame, nullptr) != 0;
}
bool provider_shutdown() { return !refuse; }
bool provider_can_unload(char*, size_t) { return !veto; }
const ArkPluginInfoV1* consumer_query() {
    static const char* dependencies[] = {"builtin_provider.dll"};
    static const ArkPluginInfoV1 info{sizeof(ArkPluginInfoV1), ARK_ABI_VERSION,
        ARK_MODULE_COMPAT_VERSION, "Built-in consumer", "1", "ArkHelper", 0, dependencies, 1};
    return &info;
}
bool consumer_init(const ArkPluginApiV1* api) {
    const void* service = api->require_service(api->owner, "test.builtin", 1, sizeof(value));
    return service && *static_cast<const int*>(service) == 42;
}
bool consumer_shutdown() { return true; }
void check(bool passed, const char* message) {
    if (!passed) { throw std::runtime_error(message); }
}
}

int wmain() {
    try {
        const auto root = std::filesystem::temp_directory_path() /
            (L"arkhelp-builtin-tests-" + std::to_wstring(GetCurrentProcessId()));
        // Test the production object files with no registered builtins. A real
        // fixture DLL must still never be mapped, even in a test-host process.
        {
            ark::core::PluginManager empty(root, true);
            check(!std::filesystem::exists(root / "plugins"), "no product plugin directory");
            std::filesystem::create_directories(root / "plugins");
            wchar_t executable[32768];
            GetModuleFileNameW(nullptr, executable, 32768);
            const auto fixture = std::filesystem::path(executable).parent_path() / "fixture.dll";
            std::filesystem::copy_file(fixture, root / "plugins" / "fixture.dll");
            check(!empty.load_now("fixture.dll"), "empty registry rejects real external DLL");
            empty.request("load", "fixture.dll");
            empty.start();
            Sleep(80);
            empty.stop();
            for (const auto& view : empty.views()) {
                check(!view.active, "scan and request cannot load external DLL");
            }
            check(!std::filesystem::exists(root / "cache"), "no product shadow cache");
        }
        {
            ark::core::PluginManager manager(root, true);
            manager.register_builtin({"builtin_provider.dll", provider_query, provider_init,
                provider_shutdown, provider_can_unload});
            manager.register_builtin({"builtin_consumer.dll", consumer_query, consumer_init,
                consumer_shutdown, nullptr});
            check(manager.load_now("builtin_consumer.dll"), "linked dependency startup");
            check(starts == 1, "provider initialized once");
            ArkFrameV1 current;
            manager.dispatch(ark::core::CallbackKind::Frame, current);
            check(frames == 1, "built-in frame dispatch");
            check(!manager.unload_now("builtin_provider.dll"), "consumer protects provider");
            veto = true;
            check(!manager.reload_now("builtin_provider.dll"), "whole graph unload veto");
            veto = false;
            check(starts == 1, "veto preserved state");
            check(!manager.load_now("external.dll"), "product rejects external module files");
            for (int generation = 0; generation < 20; ++generation) {
                check(manager.reload_now("builtin_provider.dll"), "built-in graph reload");
                check(frames == 0, "new generation state");
                manager.dispatch(ark::core::CallbackKind::Frame, current);
                check(frames == 1, "no callbacks from an old generation");
            }
            check(manager.unload_now("builtin_consumer.dll"), "consumer shutdown");
            refuse = true;
            check(!manager.unload_now("builtin_provider.dll"), "shutdown refusal retains module");
            refuse = false;
            check(manager.unload_now("builtin_provider.dll"), "shutdown can retry");
            check(manager.load_now("builtin_consumer.dll"), "restart linked modules");
            manager.stop();
            check(!manager.with_external_service("test.builtin", 1, sizeof(value),
                [](const void*, void*) {}, nullptr), "shutdown releases service");
        }
        std::filesystem::remove_all(root);
        std::puts("PASS built-in modules: dependencies, leases, reload, veto, refusal, restart, empty-registry external rejection, no input adapter");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL built-in modules: %s\n", error.what());
        return 1;
    }
}
