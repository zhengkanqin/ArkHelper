// Built-in ArkHelper battle provider: current-stage data, IL2CPP capture and IPC.
// Runtime worker ticks retry metadata discovery and publish POD snapshots.
// Unity object sampling and projection run in the main-thread Update hook.
// On shutdown, publications stop before hooks and worker state are released.
#include <ark/plugin.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <iterator>
#include <string>

#include "app/app.h"
#include "capture/capture.h"
#include "ipc/ipc_publish.h"

namespace {

constexpr std::uint32_t kLogInfo = 1;
constexpr std::uint32_t kLogError = 3;

ArkPluginApiV1 g_host{};
ark::arkhelper::App* g_app = nullptr;
std::uint64_t g_frame_token = 0;

void log(std::uint32_t level, const std::string& message) {
    if (g_host.log && g_host.owner) {
        g_host.log(g_host.owner, level, message.c_str());
    }
}

bool host_is_compatible(const ArkPluginApiV1* host) {
    return host && host->size >= sizeof(ArkPluginApiV1) && host->abi_version == ARK_ABI_VERSION &&
           host->imgui_version == ARK_MODULE_COMPAT_VERSION && host->log && host->on_frame &&
           host->remove_callback && host->require_service;
}

void on_frame(void* user, const ArkFrameV1* frame) {
    (void)user;
    (void)frame;
    // Runtime-worker tick: retries the runtime connection (throttled) and is a no-op once the
    // BattleController.Update hook is live.
    ark::arkhelper::capture::tick();
    // Publish the shared-memory snapshot for ArkHelper.exe after the capture snapshot was refreshed.
    ark::arkhelper::ipc_publish::tick();
}

void release_callbacks() {
    // Best effort: the host erases a plugin's callbacks before invoking shutdown, so
    // remove_callback() legitimately returns false during a normal unload. Clearing the tokens
    // regardless keeps shutdown idempotent and lets the host unmap the DLL.
    if (g_frame_token) {
        g_host.remove_callback(g_host.owner, g_frame_token);
        g_frame_token = 0;
    }
}

} // namespace

extern "C" const ArkPluginInfoV1* arkhelp_provider_query() {
    static const char* const dependencies[] = {"damage_meter.dll"};
    static const ArkPluginInfoV1 info = {
        sizeof(ArkPluginInfoV1), ARK_ABI_VERSION, ARK_MODULE_COMPAT_VERSION, "ArkHelper Battle Provider", "1.2.1",
        "ArkHelper", ARK_PLUGIN_DEFAULT, dependencies, std::size(dependencies),
    };
    return &info;
}

extern "C" bool arkhelp_provider_initialize(const ArkPluginApiV1* host) {
    if (g_app) {
        return true;
    }
    if (!host_is_compatible(host)) {
        return false;
    }
    g_host = *host;
    try {
        auto app = std::make_unique<ark::arkhelper::App>(g_host);
        app->load_data(); // Initializes current-stage storage; no offline catalog is loaded.
        g_app = app.release();
    } catch (const std::exception& error) {
        log(kLogError, std::string("ArkHelper: 初始化异常：") + error.what());
        return false;
    } catch (...) {
        log(kLogError, "ArkHelper: 初始化异常（未知错误）");
        return false;
    }
    g_frame_token = g_host.on_frame(g_host.owner, on_frame, g_app);
    if (!g_frame_token) {
        release_callbacks();
        delete g_app;
        g_app = nullptr;
        log(kLogError, "ArkHelper: 回调注册失败");
        return false;
    }
    // Arm the capture layer only once the frame callback that ticks it is registered. It never
    // fails initialization: a missing game process or IL2CPP runtime is a UI state.
    ark::arkhelper::capture::initialize(g_host);
    // The IPC layer only publishes what capture/App already expose; it also never fails init.
    ark::arkhelper::ipc_publish::initialize(g_app);
    log(kLogInfo, "ArkHelper: 已初始化（当前关卡 + 实例采集 + IPC v9）");
    return true;
}

extern "C" bool arkhelp_provider_can_unload(char*, std::size_t) {
    return ark::arkhelper::capture::can_unload();
}

extern "C" bool arkhelp_provider_shutdown() {
    if (!g_app) {
        return true;
    }
    // Stop the IPC publisher first so the last snapshot is written while capture is still valid and
    // no publication can observe a half-removed hook. Both calls are idempotent.
    ark::arkhelper::ipc_publish::shutdown();
    // Stop the capture layer next: it must disable and remove its native hook while the host still
    // holds a plugin lease. A false return keeps the hook (and therefore this module) alive so the
    // host can retry shutdown instead of unmapping a detour target.
    if (!ark::arkhelper::capture::shutdown()) {
        log(kLogError, "ArkHelper: 采集层停止失败（原生钩子未移除），保留模块以便重试");
        return false;
    }
    // Callbacks are already drained/erased by the host before shutdown; release the tokens as a
    // safety net and then drop the state the callbacks captured.
    release_callbacks();
    delete g_app;
    g_app = nullptr;
    g_host = ArkPluginApiV1{};
    log(kLogInfo, "ArkHelper: 已卸载");
    return true;
}
