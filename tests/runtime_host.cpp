// Exercises the actual product runtime, including stop/start in one process.
// A small off-screen message window also permits the window-hook loader check.
#include <windows.h>
#include <ark/plugin.h>
#include <ark/battle.h>
#include <arkhelp/ipc.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {
void pump() {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
template<class Predicate> bool until(Predicate predicate, DWORD timeout = 5000) {
    const auto deadline = GetTickCount64() + timeout;
    do { if (predicate()) { return true; } pump(); Sleep(5); } while (GetTickCount64() < deadline);
    return false;
}
void publish(const void* table, void* user) {
    const auto* api = static_cast<const ArkBattleTestApiV1*>(table);
    auto* ok = static_cast<bool*>(user);
    ArkEventV1 event;
    event.kind = ARK_BATTLE_START;
    std::strcpy(event.stage_id, "Obt/Main/level_main_00-01");
    *ok = api->publish(&event);
    event = {};
    event.kind = ARK_DAMAGE;
    event.damage_type = ARK_DAMAGE_PHYSICAL;
    event.calculated_damage = 125;
    event.actual_damage = 100;
    std::strcpy(event.actor_id, "char_002_amiya");
    *ok = api->publish(&event) && *ok;
    event = {};
    event.kind = ARK_HEAL;
    event.damage_type = ARK_DAMAGE_HEAL;
    event.calculated_damage = 220;
    event.actual_damage = 180;
    std::strcpy(event.actor_id, "char_002_amiya");
    *ok = api->publish(&event) && *ok;
    event = {};
    event.kind = ARK_COST;
    event.calculated_damage = event.actual_damage = 14;
    std::strcpy(event.actor_id, "char_151_myrtle");
    *ok = api->publish(&event) && *ok;
    event = {};
    event.kind = ARK_TAKEN;
    event.calculated_damage = event.actual_damage = 80;
    std::strcpy(event.actor_id, "char_002_amiya");
    *ok = api->publish(&event) && *ok;
    event = {};
    event.kind = ARK_BATTLE_END;
    event.game_time = 1;
    *ok = api->publish(&event) && *ok;
}
void inspect(const void* table, void* user) {
    const auto* api = static_cast<const ArkBattleEventsApiV1*>(table);
    ArkEventV1 events[8];
    ArkReadStatusV1 status;
    const auto count = api->read(0, events, 8, &status);
    *static_cast<bool*>(user) = count == 6 && status.ended && !status.partial &&
        events[1].actual_damage == 100 && events[1].calculated_damage == 125 &&
        events[2].kind == ARK_HEAL && events[3].kind == ARK_COST && events[4].kind == ARK_TAKEN;
}
bool snapshot_ready(bool statistics = false) {
    using namespace arkhelp::ipc;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kBattleMappingName);
    if (!mapping) { return false; }
    const auto* view = static_cast<const BattleSnapshot*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(BattleSnapshot)));
    BattleSnapshot snapshot;
    const bool good = view && read_snapshot(*view, snapshot) && snapshot.magic == kBattleMagic &&
        snapshot.version == kVersion && snapshot.size == sizeof(BattleSnapshot);
    if (view) { UnmapViewOfFile(view); }
    CloseHandle(mapping);
    return good && (!statistics || (snapshot.damage_count == 2 &&
        snapshot.team_metrics[kStatDamage] == 100 && snapshot.team_metrics[kStatHeal] == 180 &&
        snapshot.team_metrics[kStatCost] == 14 && snapshot.team_metrics[kStatTaken] == 80 &&
        (snapshot.team_metric_mask & (1u << kStatCost))));
}
}
int wmain(int argc, wchar_t** argv) {
    bool external = false;
    for (int i = 1; i < argc; ++i) { external |= std::wcscmp(argv[i], L"--await-loader") == 0; }
    WNDCLASSW cls{};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpfnWndProc = DefWindowProcW;
    cls.lpszClassName = L"ArkHelper.Runtime.TestHost";
    RegisterClassW(&cls);
    HWND window = CreateWindowW(cls.lpszClassName, L"ArkHelper runtime verification", WS_OVERLAPPEDWINDOW,
        -32000, -32000, 800, 600, nullptr, nullptr, cls.hInstance, nullptr);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    wchar_t executable[32768];
    GetModuleFileNameW(nullptr, executable, 32768);
    const auto root = std::filesystem::path(executable).parent_path();
    HMODULE runtime = external ? nullptr : LoadLibraryExW((root / L"ArkHelperRuntime.dll").c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (external && !until([&] { runtime = GetModuleHandleW(L"ArkHelperRuntime.dll"); return runtime != nullptr; }, 30000)) { return 2; }
    if (!runtime) { return 3; }
    const auto start = reinterpret_cast<ArkHelperRuntimeStart>(GetProcAddress(runtime, "arkhelp_runtime_start"));
    const auto stop = reinterpret_cast<void(*)()>(GetProcAddress(runtime, "arkhelp_runtime_shutdown"));
    const auto running = reinterpret_cast<bool(*)()>(GetProcAddress(runtime, "arkhelp_runtime_is_running"));
    const auto service = reinterpret_cast<ArkHelperRuntimeWithService>(GetProcAddress(runtime, "arkhelp_runtime_with_service"));
    if (!start || !stop || !running || !service) { return 4; }
    for (int cycle = 0; cycle < 3; ++cycle) {
        if ((!external || cycle > 0) && !until([&] { return start(nullptr); })) { return 5; }
        if (!until([&] { return running(); })) { return 6; }
        if (!until([] { return snapshot_ready(); })) { return 7; }
        const auto csv = root / "exports" / ("damage-" + std::to_string(GetCurrentProcessId()) + "-1.csv");
        std::filesystem::remove(csv);
        bool published = false, valid = false;
        if (!service(ARK_BATTLE_TEST_SERVICE_NAME, ARK_BATTLE_ABI_VERSION, sizeof(ArkBattleTestApiV1), publish, &published) || !published) { return 8; }
        if (!service(ARK_BATTLE_SERVICE_NAME, ARK_BATTLE_ABI_VERSION, sizeof(ArkBattleEventsApiV1), inspect, &valid) || !valid) { return 9; }
        if (!until([&] { return snapshot_ready(true); })) { return 13; }
        // Check that the background frame pump consumed the events and exported CSV.
        if (!until([&] {
            std::ifstream file(csv);
            const std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            return contents.find("char_002_amiya") != std::string::npos &&
                contents.find("char_151_myrtle") != std::string::npos &&
                contents.find("有效治疗,有效回费,承伤") != std::string::npos &&
                contents.find("180.0000,14.0000,80.0000") != std::string::npos;
        })) { return 10; }
        Sleep(100); // let the loader observe Ready before the first session is stopped
        stop();
        if (!until([&] { return !running(); })) { return 11; }
        if (service(ARK_BATTLE_SERVICE_NAME, ARK_BATTLE_ABI_VERSION, sizeof(ArkBattleEventsApiV1), inspect, &valid)) { return 12; }
    }
    HANDLE existing_desktop = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\ArkHelp.App");
    if (existing_desktop) { CloseHandle(existing_desktop); return 14; }
    HANDLE desktop_stop = CreateEventW(nullptr, TRUE, FALSE, L"Local\\ArkHelp.RuntimeStop");
    if (!desktop_stop || GetLastError() == ERROR_ALREADY_EXISTS) return 15;
    HANDLE desktop = CreateMutexW(nullptr, FALSE, L"Local\\ArkHelp.App");
    if (!desktop || !until([&] { return start(nullptr); }) || !until([&] { return running(); })) return 16;
    if (!until([] { return snapshot_ready(); })) return 17;
    SetEvent(desktop_stop);
    CloseHandle(desktop);
    if (!until([&] { return !running(); })) return 18;
    bool valid = false;
    if (service(ARK_BATTLE_SERVICE_NAME, ARK_BATTLE_ABI_VERSION, sizeof(ArkBattleEventsApiV1), inspect, &valid)) return 19;
    Sleep(300);
    if (running()) return 20;
    ResetEvent(desktop_stop);
    Sleep(300);
    if (running()) return 21;
    desktop = CreateMutexW(nullptr, FALSE, L"Local\\ArkHelp.App");
    if (!until([&] { return running() && snapshot_ready(); })) return 22;
    bool published = false;
    if (!service(ARK_BATTLE_TEST_SERVICE_NAME, ARK_BATTLE_ABI_VERSION, sizeof(ArkBattleTestApiV1), publish, &published) ||
        !published || !until([] { return snapshot_ready(true); })) return 23;
    // Reopen before module rundown finishes: the new UI resets RuntimeStop
    // while the old session is still stopping. The stop reason must be latched.
    SetEvent(desktop_stop);
    stop();
    ResetEvent(desktop_stop);
    CloseHandle(desktop);
    desktop=CreateMutexW(nullptr,FALSE,L"Local\\ArkHelp.App");
    if(!desktop || !until([&]{return !running();}) ||
       !until([&]{return running() && snapshot_ready();}))return 25;
    stop();
    if (!until([&] { return !running(); })) return 24;
    CloseHandle(desktop);
    CloseHandle(desktop_stop);
    DestroyWindow(window);
    std::printf("PASS integrated runtime: IPC v%u, battle services, CSV, clean sessions, dormant and fast desktop reconnect without reloading\n", arkhelp::ipc::kVersion);
    return 0;
}
