#pragma once

#include "ipc.h"
#include <atomic>
#include <cstring>
#include <vector>

// Infrequent, current-level-only data. Kept separate from the small 10 Hz combat snapshot so
// a full roster/timeline never competes with positions, HP bars or spawn countdowns.
namespace arkhelp::ipc {
inline constexpr wchar_t kOverviewMappingName[] = L"Local\\ArkHelp.StageOverview.v1";
inline constexpr std::uint32_t kOverviewMagic = 0x4F484841u;
inline constexpr std::uint32_t kOverviewVersion = 1;
inline constexpr std::size_t kOverviewMappingBytes = 16u * 1024u * 1024u;
inline constexpr std::uint32_t kOverviewMaxEnemies = 4096;
inline constexpr std::uint32_t kOverviewMaxSpawns = 65536;
enum OverviewFlags : std::uint32_t {
    kOverviewLoaded = 1u << 0,
    kOverviewPartial = 1u << 1,
    kOverviewPlanRead = 1u << 2,
};
enum StageSpawnFlags : std::uint32_t {
    kStageSpawnConditional = 1u << 0,
    kStageSpawnCountKnown = 1u << 1,
    kStageSpawnTimingKnown = 1u << 2, // relative action delay, never a guessed battle timestamp
};
struct StageSpawn {
    char enemy_key[kIdCapacity]{};
    std::uint32_t flags = 0;
    std::int32_t branch = 0; // 0 main; positive = conditional branch
    std::uint32_t wave = 0, fragment = 0;
    std::int32_t count = 0, route = -1;
    double delay = 0, interval = 0;
};
struct OverviewHeader {
    std::uint32_t magic = kOverviewMagic, version = kOverviewVersion;
    std::uint32_t bytes = sizeof(OverviewHeader), sequence = 0;
    std::uint32_t battle_serial = 0, revision = 0, flags = 0;
    std::uint32_t enemy_count = 0, spawn_count = 0;
    char level_id[kLevelIdCapacity]{};
};
struct StageOverview {
    OverviewHeader header;
    std::vector<EnemyEntry> enemies;
    std::vector<StageSpawn> spawns;
};
inline constexpr std::size_t overview_bytes(std::size_t enemies, std::size_t spawns) {
    return sizeof(OverviewHeader) + enemies * sizeof(EnemyEntry) + spawns * sizeof(StageSpawn);
}
inline bool valid_overview_header(const OverviewHeader& h) {
    return h.magic == kOverviewMagic && h.version == kOverviewVersion &&
        h.enemy_count <= kOverviewMaxEnemies && h.spawn_count <= kOverviewMaxSpawns &&
        h.bytes == overview_bytes(h.enemy_count, h.spawn_count) && h.bytes <= kOverviewMappingBytes &&
        h.level_id[kLevelIdCapacity - 1] == 0;
}
inline bool write_overview(void* memory, const StageOverview& data) {
    auto header = data.header;
    header.enemy_count = static_cast<std::uint32_t>(data.enemies.size());
    header.spawn_count = static_cast<std::uint32_t>(data.spawns.size());
    header.bytes = static_cast<std::uint32_t>(overview_bytes(data.enemies.size(), data.spawns.size()));
    if (!memory || !valid_overview_header(header)) return false;
    auto& target = *static_cast<OverviewHeader*>(memory);
    begin_write(target.sequence);
    header.sequence = target.sequence;
    std::memcpy(&target, &header, offsetof(OverviewHeader, sequence));
    constexpr auto after = offsetof(OverviewHeader, sequence) + sizeof(header.sequence);
    std::memcpy(reinterpret_cast<char*>(&target) + after, reinterpret_cast<const char*>(&header) + after, sizeof(header) - after);
    char* payload = static_cast<char*>(memory) + sizeof(header);
    if (!data.enemies.empty()) std::memcpy(payload, data.enemies.data(), data.enemies.size() * sizeof(EnemyEntry));
    payload += data.enemies.size() * sizeof(EnemyEntry);
    if (!data.spawns.empty()) std::memcpy(payload, data.spawns.data(), data.spawns.size() * sizeof(StageSpawn));
    end_write(target.sequence);
    return true;
}
// Returns only new, coherent revisions. Old data stays with the caller during transient writes.
inline bool read_overview(const void* memory, StageOverview& out) {
    if (!memory) return false;
    const auto& source = *static_cast<const OverviewHeader*>(memory);
    const auto& sequence = *reinterpret_cast<const std::atomic<std::uint32_t>*>(&source.sequence);
    for (int attempt = 0; attempt < 4; ++attempt) {
        const auto first = sequence.load(std::memory_order_acquire);
        if (first & 1) continue;
        if (out.header.sequence == first && out.header.battle_serial &&
            source.battle_serial == out.header.battle_serial) return false;
        StageOverview copy;
        std::memcpy(&copy.header, &source, sizeof(source));
        if (!valid_overview_header(copy.header)) continue;
        copy.enemies.resize(copy.header.enemy_count);
        copy.spawns.resize(copy.header.spawn_count);
        const char* payload = static_cast<const char*>(memory) + sizeof(source);
        if (!copy.enemies.empty()) std::memcpy(copy.enemies.data(), payload, copy.enemies.size() * sizeof(EnemyEntry));
        payload += copy.enemies.size() * sizeof(EnemyEntry);
        if (!copy.spawns.empty()) std::memcpy(copy.spawns.data(), payload, copy.spawns.size() * sizeof(StageSpawn));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (first != sequence.load(std::memory_order_acquire)) continue;
        for (auto& entry : copy.enemies) {
            entry.enemy_key[kIdCapacity - 1] = 0;
            entry.enemy_name[kNameCapacity - 1] = 0;
        }
        for (auto& entry : copy.spawns) entry.enemy_key[kIdCapacity - 1] = 0;
        out = std::move(copy);
        return true;
    }
    return false;
}
static_assert(overview_bytes(kOverviewMaxEnemies, kOverviewMaxSpawns) <= kOverviewMappingBytes);
} // namespace arkhelp::ipc
