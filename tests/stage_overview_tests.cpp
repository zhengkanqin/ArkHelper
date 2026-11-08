#include "../src/data/enemy_overview.h"
#include <cstdio>
#include <cstdlib>

namespace {
int checks = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::fprintf(stderr, "FAIL overview: %s\n", message); std::exit(1); }
}
}
int main() {
    using namespace arkhelp;
    ipc::StageOverview sent, received;
    sent.header.battle_serial = 7; sent.header.revision = 1;
    std::snprintf(sent.header.level_id, sizeof(sent.header.level_id), "rogue/level");
    sent.header.flags = ipc::kOverviewLoaded | ipc::kOverviewPlanRead;
    for (int i = 0; i < 130; ++i) {
        ipc::EnemyEntry enemy;
        std::snprintf(enemy.enemy_key, sizeof(enemy.enemy_key), "enemy_%d", i);
        std::snprintf(enemy.enemy_name, sizeof(enemy.enemy_name), "敌人 %d", i);
        enemy.flags = ipc::kEnemyReference; enemy.stats_mask = ipc::kEnemyMaxHp;
        enemy.max_hp = 1000 + i;
        sent.enemies.push_back(enemy);
        ipc::StageSpawn spawn;
        std::snprintf(spawn.enemy_key, sizeof(spawn.enemy_key), "enemy_%d", i);
        spawn.wave = i / 10; spawn.count = i + 1; spawn.flags = ipc::kStageSpawnCountKnown;
        sent.spawns.push_back(spawn);
    }
    std::vector<std::uint64_t> memory(ipc::kOverviewMappingBytes / 8);
    check(ipc::write_overview(memory.data(), sent) && ipc::read_overview(memory.data(), received), "separate overview payload round trip");
    check(received.enemies.size() == 130 && received.spawns.size() == 130 && received.spawns.back().wave == 12,
        "overview exceeds live enemy limit and retains every wave/action beyond five entries");
    check(!ipc::read_overview(memory.data(), received), "unchanged payload is not recopied every poll");
    auto& header = *reinterpret_cast<ipc::OverviewHeader*>(memory.data());
    ipc::begin_write(header.sequence);
    check(!ipc::read_overview(memory.data(), received) && received.spawns.size() == 130, "in-progress publication cannot replace retained data");
    ipc::end_write(header.sequence);
    header.bytes = 0xFFFFFFFF;
    check(!ipc::read_overview(memory.data(), received), "invalid lengths rejected before allocating or reading payload");
    check(ipc::write_overview(memory.data(), sent) && ipc::read_overview(memory.data(), received), "valid publication recovers after invalid header");

    ipc::BattleSnapshot battle;
    battle.battle_serial = 7;
    std::snprintf(battle.level_id, sizeof(battle.level_id), "rogue/level");
    EnemyOverview roster;
    roster.observe(battle, received, 1);
    check(roster.enemies().size() == 130 && roster.find("enemy_129"), "entire roster visible before a single live enemy spawns");
    battle.enemy_count = 2;
    battle.enemies[0] = sent.enemies[0]; battle.enemies[1] = sent.enemies[0];
    for (int i = 0; i < 2; ++i) {
        battle.enemies[i].flags = ipc::kEnemyRuntime;
        battle.enemies[i].tracking_id = 10 + i;
        battle.enemies[i].stats_mask = ipc::kEnemyAtk | ipc::kEnemyHp;
        battle.enemies[i].atk = 2300; battle.enemies[i].hp = 900;
    }
    roster.observe(battle, received, 1);
    check(roster.enemies().size() == 130 && roster.find("enemy_0")->atk == 2300 &&
          roster.find("enemy_0")->max_hp == 1000 && !(roster.find("enemy_0")->stats_mask & ipc::kEnemyHp),
          "live instances refine a stable type, keep available reference fields and never create duplicate/current-HP rows");
    battle.enemy_count = 0;
    roster.observe(battle, received, 1);
    check(roster.enemies().size() == 130 && roster.find("enemy_0")->atk == 2300, "death and settlement retain observed attributes and future roster");
    ++battle.battle_serial;
    roster.observe(battle, received, 2);
    check(roster.enemies().empty(), "next run rejects previous level/serial and clears observed buffs");
    std::printf("Stage overview: %d checks, 0 failures\n", checks);
}
