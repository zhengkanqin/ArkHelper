#pragma once

#include <cmath>
#include <cstdio>
#include <limits>
#include <type_traits>
#include <arkhelp/ipc.h>
#include "../capture/capture.h"
#include "../data/pack.h"

namespace ark::arkhelper::ipc_publish {

// Keep field availability alongside its value. A failed getter is never a zero stat.
inline arkhelp::ipc::EnemyEntry make_enemy_entry(const capture::EnemySample& sample) {
    namespace ipc = arkhelp::ipc;
    ipc::EnemyEntry entry{};
    entry.flags = ipc::kEnemyValid | ipc::kEnemyAlive | ipc::kEnemyRuntime;
    entry.tracking_id = sample.tracking_id;
    std::snprintf(entry.enemy_key, sizeof(entry.enemy_key), "%.*s",
                  static_cast<int>(sizeof(sample.enemy_key)), sample.enemy_key);
    std::snprintf(entry.enemy_name, sizeof(entry.enemy_name), "%.*s",
                  static_cast<int>(sizeof(sample.enemy_name)), sample.enemy_name);
    auto put = [&](std::uint32_t bit, double value, auto& target, double scale = 1.0) {
        using Value = std::remove_reference_t<decltype(target)>;
        value *= scale;
        if ((sample.stats_mask & bit) == 0 || !std::isfinite(value) ||
            value <= static_cast<double>(std::numeric_limits<Value>::lowest()) ||
            value >= static_cast<double>(std::numeric_limits<Value>::max())) { return; }
        target = static_cast<Value>(std::llround(value));
        entry.stats_mask |= bit;
    };
    put(ipc::kEnemyHp, sample.hp, entry.hp);
    put(ipc::kEnemyMaxHp, sample.max_hp, entry.max_hp);
    put(ipc::kEnemyAtk, sample.atk, entry.atk);
    put(ipc::kEnemyDef, sample.def, entry.def);
    put(ipc::kEnemyMagicResistance, sample.magic_resistance, entry.magic_resistance, 1000);
    put(ipc::kEnemyMoveSpeed, sample.move_speed, entry.move_speed, 1000);
    put(ipc::kEnemyAttackSpeed, sample.attack_speed, entry.attack_speed);
    put(ipc::kEnemyBaseAttackTime, sample.base_attack_time, entry.base_attack_time, 1000);
    put(ipc::kEnemyWeight, sample.weight, entry.weight, 1000);
    put(ipc::kEnemyBlockCount, sample.block_cnt, entry.block_cnt);
    put(ipc::kEnemyLifePointReduce, sample.life_point_reduce, entry.life_point_reduce);
    return entry;
}

inline arkhelp::ipc::EnemyEntry make_reference_entry(const Enemy& reference) {
    capture::EnemySample sample{};
    std::snprintf(sample.enemy_key, sizeof(sample.enemy_key), "%s", reference.key.c_str());
    std::snprintf(sample.enemy_name, sizeof(sample.enemy_name), "%s", reference.name.c_str());
    sample.stats_mask = reference.stats_mask;
    sample.max_hp = reference.attributes.max_hp;
    sample.atk = reference.attributes.atk;
    sample.def = reference.attributes.def;
    sample.magic_resistance = reference.attributes.magic_resistance;
    sample.move_speed = reference.attributes.move_speed;
    sample.attack_speed = reference.attributes.attack_speed;
    sample.base_attack_time = reference.attributes.base_attack_time;
    sample.weight = reference.attributes.mass_level;
    sample.block_cnt = reference.attributes.block_cnt;
    sample.life_point_reduce = reference.attributes.life_point_reduce;
    auto result = make_enemy_entry(sample);
    result.flags = arkhelp::ipc::kEnemyValid | arkhelp::ipc::kEnemyReference;
    result.stats_mask &= ~arkhelp::ipc::kEnemyHp;
    return result;
}

} // namespace ark::arkhelper::ipc_publish
