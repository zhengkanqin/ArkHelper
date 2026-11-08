// Copied current-battle data. This model never loads an offline stage catalog.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ark::arkhelper {

struct EnemyAttributes {
    double max_hp = 0;
    double atk = 0;
    double def = 0;
    double magic_resistance = 0;
    double move_speed = 0;
    double attack_speed = 0;
    double base_attack_time = 0;
    double mass_level = 0;
    double cost = 0;
    double range_radius = 0;
    int block_cnt = 0;
    int life_point_reduce = 0;
};

struct Immunities {
    bool stun = false;
    bool silence = false;
    bool sleep = false;
    bool frozen = false;
    bool levitate = false;
    bool disarmed = false;
    bool feared = false;
    bool palsy = false;
    bool attract = false;
    bool teleport = false;
    bool ground_bound = false;
};

struct EnemyAbility {
    std::string name;
    std::string text;
};

struct EnemySkill {
    std::string name;
    std::string prefab_key;
    double sp_cost = 0;
    double cooldown = 0;
    double init_cooldown = 0;
    std::vector<std::pair<std::string, double>> blackboard;
};

struct Enemy {
    std::uint32_t stats_mask = 0;
    std::string key;
    std::string name;
    std::string description;
    std::string enemy_level;
    std::string attack_type;
    std::vector<std::string> damage_types;
    int attributes_level = 0;
    EnemyAttributes attributes;
    Immunities immunities;
    std::vector<EnemyAbility> abilities;
    std::vector<EnemySkill> skills;
};

struct Tile {
    std::string key;
    std::string height;
    std::string buildable;
    bool passable = false;
    bool player_side = false;
};

struct MapGrid {
    int rows = 0;
    int cols = 0;
    std::vector<Tile> tiles; // Flat row-major array: index = row * cols + col.
};

struct RouteCheckpoint {
    int row = 0;
    int col = 0;
    double time = 0;
    std::string type;
};

struct Route {
    std::string motion;
    int start_row = 0;
    int start_col = 0;
    int end_row = 0;
    int end_col = 0;
    bool allow_diagonal = false;
    std::vector<RouteCheckpoint> checkpoints;
};

struct Spawn {
    int frame = 0;
    double time = 0;
    std::string enemy_key;
    int count = 1;
    int route_index = -1;
    bool random = false; // Conditional or random spawn; no deterministic countdown.
};

struct StageOptions {
    int character_limit = 0;
    int max_life_point = 0;
    int initial_cost = 0;
    int max_cost = 0;
    double cost_increase_time = 0;
    double move_multiplier = 0;
};

struct OverviewSpawn {
    std::string key;
    int branch = 0, wave = 0, fragment = 0, count = 0, route = -1;
    bool conditional = false, count_known = false, timing_known = false;
    double delay = 0, interval = 0;
};

struct Stage {
    std::uint32_t revision = 0;
    bool runtime = false;
    bool spawn_plan_available = false;
    bool spawn_plan_conditional = false;
    bool overview_plan_read = false;
    bool overview_partial = false;
    std::vector<OverviewSpawn> overview_spawns;
    std::string stage_id;
    std::string code;
    std::string name;
    std::string zone_id;
    std::string level_id;
    std::string difficulty;
    StageOptions options;
    MapGrid map;
    std::vector<Route> routes;
    std::vector<Spawn> spawns;
    std::vector<Enemy> enemies;
    std::vector<std::string> enemy_keys;
    bool loaded = false;
};

struct StagePack {
    std::vector<Stage> stages;
};

// Enemy lookup by pack key. Returns nullptr when the stage does not list that enemy.
inline const Enemy* find_enemy(const Stage& stage, std::string_view key) {
    for (const Enemy& enemy : stage.enemies) {
        if (enemy.key == key) {
            return &enemy;
        }
    }
    return nullptr;
}

} // namespace ark::arkhelper
