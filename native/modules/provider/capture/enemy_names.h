#pragma once
#include <string_view>
#include "../data/pack.h"

namespace ark::arkhelper::capture::live {
// This catalog is the current Stage itself. No global enemy table or numeric reference values are
// added when a database reference only supplies a display name.
inline bool merge_enemy_name(Stage& stage, std::string_view key, std::string_view name) {
    if (key.empty() || name.empty() || name == key) return false;
    for (auto& enemy : stage.enemies) {
        if (enemy.key != key) continue;
        if (!enemy.name.empty() && enemy.name != enemy.key) return false;
        enemy.name = name;
        return true;
    }
    if (stage.enemies.size() >= 4096) { stage.overview_partial = true; return false; }
    Enemy enemy;
    enemy.key = key;
    enemy.name = name;
    stage.enemies.push_back(std::move(enemy));
    stage.enemy_keys.emplace_back(key);
    return true;
}
}
