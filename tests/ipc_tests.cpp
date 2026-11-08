#include <windows.h>
#include <arkhelp/ipc.h>
#include <cstdio>
#include <cwchar>
#include <string>

using namespace arkhelp::ipc;
namespace {
constexpr unsigned kFinal = 10000;
bool coherent(const BattleSnapshot& snapshot) {
    const auto n = snapshot.frame;
    const auto& unit = snapshot.units[0];
    if (!(snapshot.published_ms == n && snapshot.time_seconds == static_cast<double>(n) &&
        snapshot.flags == ((n & 1u) ? kBattlePaused : 0u) &&
        snapshot.unit_sample_ms == n && snapshot.unit_sample_battle_time == static_cast<double>(n) &&
        unit.tracking_id == static_cast<std::uint64_t>(n) * 17 &&
        unit.flags == (n ? kUnitValid | kUnitHpValid | kUnitGroundValid | kUnitRangeValid : 0u) &&
        unit.ground_x == static_cast<float>(n) * 2 && unit.ground_y == static_cast<float>(n) * 3 &&
        unit.range_radius == static_cast<float>(n) * .125f &&
        unit.range_point_count == (n ? kRangePointCount : 0u) &&
        snapshot.enemies[0].tracking_id == static_cast<std::uint64_t>(n) * 19 &&
        snapshot.enemies[0].stats_mask == (n ? kEnemyHp | kEnemyMaxHp | kEnemyAtk : 0u) &&
        snapshot.team_metrics[kStatCost] == static_cast<double>(n) * 7 &&
        snapshot.operator_count == (n ? 1u : 0u) &&
        snapshot.operator_ids[0] == static_cast<std::uint64_t>(n) * 23 &&
        snapshot.operators[0].total_damage == static_cast<std::int64_t>(n) * 31 &&
        unit.hp == static_cast<float>(n) &&
        snapshot.damage[0].total_damage == static_cast<std::int64_t>(n) * 1000000007LL)) return false;
    for (unsigned p = 0; p < kRangePointCount; ++p) {
        if (unit.range_points[p].x != (n ? static_cast<float>(n) + p * .25f : 0.f) ||
            unit.range_points[p].y != (n ? static_cast<float>(n) * 2 + p * .5f : 0.f)) return false;
    }
    return true;
}
int read_child(const wchar_t* name, const wchar_t* ready_name) {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!mapping) { return 2; }
    const auto* shared = static_cast<const BattleSnapshot*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(BattleSnapshot)));
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name);
    if (!shared || !ready) { return 3; }
    SetEvent(ready);
    const auto deadline = GetTickCount64() + 10000;
    unsigned reads = 0;
    bool finished = false;
    while (GetTickCount64() < deadline) {
        BattleSnapshot snapshot;
        if (!read_snapshot(*shared, snapshot)) { continue; }
        if (!coherent(snapshot)) { return 4; }
        ++reads;
        if (snapshot.frame == kFinal) { finished = true; break; }
    }
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    CloseHandle(ready);
    return reads && finished ? 0 : 5;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wcscmp(argv[1], L"--reader") == 0) {
        return read_child(argv[2], argv[3]);
    }
    const std::wstring name = L"Local\\ArkHelp.IpcTest." + std::to_wstring(GetCurrentProcessId());
    const std::wstring ready_name = name + L".Ready";
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        sizeof(BattleSnapshot), name.c_str());
    auto* shared = static_cast<BattleSnapshot*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BattleSnapshot)));
    if (!shared) { return 1; }
    *shared = BattleSnapshot{};
    shared->magic = kBattleMagic;
    shared->version = kVersion;
    shared->size = sizeof(BattleSnapshot);
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str());
    wchar_t executable[32768];
    GetModuleFileNameW(nullptr, executable, 32768);
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --reader \"" + name + L"\" \"" + ready_name + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) { return 2; }
    if (WaitForSingleObject(ready, 10000) != WAIT_OBJECT_0) { return 3; }
    for (unsigned n = 1; n <= kFinal; ++n) {
        begin_write(shared->sequence);
        shared->frame = n;
        shared->flags = (n & 1u) ? kBattlePaused : 0u;
        shared->published_ms = n;
        shared->time_seconds = static_cast<double>(n);
        shared->unit_sample_ms = n;
        shared->unit_sample_battle_time = static_cast<double>(n);
        shared->units[0].tracking_id = static_cast<std::uint64_t>(n) * 17;
        shared->units[0].flags = kUnitValid | kUnitHpValid | kUnitGroundValid | kUnitRangeValid;
        shared->units[0].ground_x = static_cast<float>(n) * 2;
        shared->units[0].ground_y = static_cast<float>(n) * 3;
        shared->units[0].range_radius = static_cast<float>(n) * .125f;
        shared->units[0].range_point_count = kRangePointCount;
        for (unsigned p = 0; p < kRangePointCount; ++p) {
            shared->units[0].range_points[p].x = static_cast<float>(n) + p * .25f;
            shared->units[0].range_points[p].y = static_cast<float>(n) * 2 + p * .5f;
        }
        shared->enemies[0].tracking_id = static_cast<std::uint64_t>(n) * 19;
        shared->enemies[0].stats_mask = kEnemyHp | kEnemyMaxHp | kEnemyAtk;
        shared->team_metrics[kStatCost] = static_cast<double>(n) * 7;
        shared->operator_count = 1;
        shared->operator_ids[0] = static_cast<std::uint64_t>(n) * 23;
        shared->operators[0].total_damage = static_cast<std::int64_t>(n) * 31;
        shared->units[0].hp = static_cast<float>(n);
        shared->damage[0].total_damage = static_cast<std::int64_t>(n) * 1000000007LL;
        end_write(shared->sequence);
    }
    DWORD result = 1;
    if (WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &result);
    } else { TerminateProcess(process.hProcess, 6); }
    BattleSnapshot ignored;
    begin_write(shared->sequence);
    if (read_snapshot(*shared, ignored, 3)) { result = 7; }
    end_write(shared->sequence);
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    CloseHandle(ready);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (result == 0) { std::printf("PASS IPC v%u: cross-process stats, range contours, identities, sample clock coherence and bounded retry\n", kVersion); }
    return static_cast<int>(result);
}
