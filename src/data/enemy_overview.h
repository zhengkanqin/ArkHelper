#pragma once
#include <arkhelp/stage_overview.h>
#include <algorithm>
#include <map>
#include <string>

namespace arkhelp {
// Stable per-type roster. References include future enemies; observations refine their available
// attributes and remain after death. Only the next battle discards this current-level cache.
class EnemyOverview {
public:
    void observe(const ipc::BattleSnapshot& battle, const ipc::StageOverview& level, std::uint64_t generation) {
        if (generation_ != generation) { generation_ = generation; enemies_.clear(); indices_.clear(); }
        const bool matching = level.header.battle_serial == battle.battle_serial &&
            std::strncmp(level.header.level_id, battle.level_id, ipc::kLevelIdCapacity) == 0;
        if (matching) {
            for (const auto& enemy : level.enemies) merge(enemy);
            for (const auto& spawn : level.spawns) {
                ipc::EnemyEntry reference;
                std::memcpy(reference.enemy_key, spawn.enemy_key, sizeof(reference.enemy_key));
                reference.flags = ipc::kEnemyReference;
                merge(reference);
            }
        }
        for (std::uint32_t i = 0; i < std::min(battle.enemy_count, ipc::kMaxEnemies); ++i) merge(battle.enemies[i]);
    }
    const std::vector<ipc::EnemyEntry>& enemies() const { return enemies_; }
    const ipc::EnemyEntry* find(const std::string& key) const {
        const auto found = indices_.find(key);
        return found == indices_.end() ? nullptr : &enemies_[found->second];
    }
private:
    void merge(const ipc::EnemyEntry& incoming) {
        if (!incoming.enemy_key[0]) return;
        const std::string key(incoming.enemy_key);
        auto [index, inserted] = indices_.try_emplace(key, enemies_.size());
        if (inserted) enemies_.push_back(incoming);
        auto& current = enemies_[index->second];
        const bool live = incoming.flags & ipc::kEnemyRuntime;
        if (incoming.enemy_name[0] && (live || !current.enemy_name[0]))
            std::memcpy(current.enemy_name, incoming.enemy_name, sizeof(current.enemy_name));
        const auto put = [&](std::uint32_t bit, auto member) {
            if ((incoming.stats_mask & bit) && (live || !(current.stats_mask & bit))) {
                current.*member = incoming.*member; current.stats_mask |= bit;
            }
        };
        put(ipc::kEnemyMaxHp, &ipc::EnemyEntry::max_hp); put(ipc::kEnemyAtk, &ipc::EnemyEntry::atk);
        put(ipc::kEnemyDef, &ipc::EnemyEntry::def); put(ipc::kEnemyMagicResistance, &ipc::EnemyEntry::magic_resistance);
        put(ipc::kEnemyMoveSpeed, &ipc::EnemyEntry::move_speed); put(ipc::kEnemyAttackSpeed, &ipc::EnemyEntry::attack_speed);
        put(ipc::kEnemyBaseAttackTime, &ipc::EnemyEntry::base_attack_time); put(ipc::kEnemyWeight, &ipc::EnemyEntry::weight);
        put(ipc::kEnemyBlockCount, &ipc::EnemyEntry::block_cnt); put(ipc::kEnemyLifePointReduce, &ipc::EnemyEntry::life_point_reduce);
        if (live) current.flags = ipc::kEnemyValid | ipc::kEnemyRuntime;
        if (incoming.damage_type_mask) current.damage_type_mask = incoming.damage_type_mask;
        current.stats_mask &= ~ipc::kEnemyHp; // a type overview has no single current HP/instance
        current.tracking_id = 0;
    }
    std::uint64_t generation_ = 0;
    std::map<std::string, std::size_t> indices_;
    std::vector<ipc::EnemyEntry> enemies_;
};
} // namespace arkhelp
