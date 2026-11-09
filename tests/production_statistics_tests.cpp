#include <windows.h>
#include <ark/battle.h>
#include <arkhelp/ipc.h>
#include "plugin_manager.h"
#include "../native/modules/damage_meter/event_history.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>

extern "C" {
const ArkPluginInfoV1* arkhelp_provider_query();
bool arkhelp_provider_initialize(const ArkPluginApiV1*);
bool arkhelp_provider_shutdown();
bool arkhelp_provider_can_unload(char*, std::size_t);
}

namespace {
ark::damage_meter::EventHistory history;
std::size_t read(std::uint64_t after, ArkEventV1* out, std::size_t capacity, ArkReadStatusV1* status) {
    return history.read(after, out, capacity, status);
}
const ArkPluginInfoV1* query() {
    static const ArkPluginInfoV1 info{sizeof(ArkPluginInfoV1), ARK_ABI_VERSION,
        ARK_MODULE_COMPAT_VERSION, "Event fixture", "1", "ArkHelper"};
    return &info;
}
bool initialize(const ArkPluginApiV1* host) {
    static const ArkBattleEventsApiV1 events{sizeof(ArkBattleEventsApiV1), ARK_BATTLE_ABI_VERSION, read, nullptr};
    return !host->test_host && host->provide_service(host->owner, ARK_BATTLE_SERVICE_NAME,
        ARK_BATTLE_ABI_VERSION, &events, sizeof(events));
}
bool shutdown() { return true; }
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
}

int wmain() {
    using namespace arkhelp::ipc;
    try {
        const auto root = std::filesystem::temp_directory_path() /
            (L"arkhelper-production-stats-" + std::to_wstring(GetCurrentProcessId()));
        ark::core::PluginManager manager(root, false);
        manager.register_builtin({"damage_meter.dll", query, initialize, shutdown, nullptr});
        manager.register_builtin({"ark_helper.dll", arkhelp_provider_query, arkhelp_provider_initialize,
            arkhelp_provider_shutdown, arkhelp_provider_can_unload});
        check(manager.load_now("ark_helper.dll"), "real provider cannot load its declared dependency");
        check(!manager.unload_now("damage_meter.dll"), "event service owner is not protected by the dependency");
        for (int round = 1; round <= 2; ++round) {
            ArkEventV1 event;
            event.kind = ARK_BATTLE_START;
            std::strcpy(event.stage_id, "fixture");
            history.publish(event);
            event = {};
            event.kind = ARK_DAMAGE;
            event.damage_type = ARK_DAMAGE_PHYSICAL;
            event.actual_damage = event.calculated_damage = round * 321;
            std::strcpy(event.actor_id, "char_fixture");
            history.publish(event);
            event.kind = ARK_COST;
            event.actual_damage = event.calculated_damage = 7;
            history.publish(event);
            bool ready = false;
            const auto deadline = GetTickCount64() + 4000;
            do {
                ArkFrameV1 frame;
                manager.dispatch(ark::core::CallbackKind::Frame, frame);
                HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kBattleMappingName);
                if (mapping) {
                    const auto* view = static_cast<const BattleSnapshot*>(MapViewOfFile(mapping,
                        FILE_MAP_READ, 0, 0, sizeof(BattleSnapshot)));
                    BattleSnapshot snapshot{};
                    ready = view && read_snapshot(*view, snapshot) && snapshot.damage_count == 1 &&
                        snapshot.team_metrics[kStatDamage] == round * 321 && snapshot.team_metrics[kStatCost] == 7;
                    if (view) UnmapViewOfFile(view);
                    CloseHandle(mapping);
                }
                if (!ready) Sleep(25);
            } while (!ready && GetTickCount64() < deadline);
            check(ready, "production provider failed to publish nonzero statistics without external service access");
        }
        check(manager.unload_now("ark_helper.dll"), "provider did not drain");
        check(manager.unload_now("damage_meter.dll"), "dependency did not drain");
        std::puts("PASS production-host statistics: declared dependency, nonzero damage/cost, next-round reset, ordered shutdown");
    } catch (const std::exception& error) { std::fprintf(stderr, "FAIL %s\n", error.what()); return 1; }
}
