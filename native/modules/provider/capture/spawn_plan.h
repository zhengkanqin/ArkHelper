#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "../data/pack.h"

namespace ark::arkhelper::capture::live {
struct PlanAction {
    bool known = true;
    bool enabled = true;
    bool scheduled = true;
    bool spawn = true;
    bool conditional = false;
    std::string key;
    int count = 0;
    int route = -1;
    double delay = 0;
    double interval = 0;
    bool display_enemy_info = false;
};
struct PlanFragment {
    bool known = true;
    double delay = 0;
    std::vector<PlanAction> actions;
};
struct PlanWave {
    bool known = true;
    double delay = 0;
    std::vector<PlanFragment> fragments;
};

// Full level overview preserves every enabled spawn action, including later/conditional waves.
// Relative delays are metadata only and must not be interpreted as absolute battle countdowns.
inline void append_spawn_overview(Stage& stage, const std::vector<PlanWave>& waves, int branch = 0) {
    stage.overview_plan_read = true;
    for (std::size_t w = 0; w < waves.size(); ++w) {
        const auto& wave = waves[w];
        if (!wave.known) stage.overview_partial = true;
        for (std::size_t f = 0; f < wave.fragments.size(); ++f) {
            const auto& fragment = wave.fragments[f];
            if (!fragment.known) stage.overview_partial = true;
            for (const auto& action : fragment.actions) {
                if (!action.enabled || !action.spawn || action.key.empty()) continue;
                if (stage.overview_spawns.size() >= 65536) { stage.overview_partial = true; return; }
                OverviewSpawn entry;
                entry.key = action.key; entry.branch = branch;
                entry.wave = static_cast<int>(w); entry.fragment = static_cast<int>(f);
                entry.count = action.count; entry.route = action.route;
                entry.count_known = action.count > 0;
                entry.delay = action.delay; entry.interval = action.interval;
                entry.timing_known = action.known && std::isfinite(action.delay) && action.delay >= 0 &&
                    std::isfinite(action.interval) && action.interval >= 0;
                entry.conditional = branch > 0 || action.conditional || !action.scheduled || !action.known;
                stage.overview_spawns.push_back(std::move(entry));
            }
        }
    }
}

// The first wave's unambiguous prefix is valid without guessing future kill waits. An unknown
// action invalidates its entire fragment because action delays are not in chronological order.
inline void build_spawn_prefix(Stage& stage, const PlanWave& first, bool later_waves, bool branches) {
    stage.spawns.clear();
    stage.spawn_plan_available = false;
    stage.spawn_plan_conditional = later_waves || branches;
    if (!first.known || !std::isfinite(first.delay) || first.delay < 0) {
        stage.spawn_plan_conditional = true;
        return;
    }
    double cursor = first.delay + 1.0 / 30.0;
    for (const PlanFragment& fragment : first.fragments) {
        const std::size_t before = stage.spawns.size();
        bool supported = fragment.known && std::isfinite(fragment.delay) && fragment.delay >= 0;
        const double start = cursor + fragment.delay + 1.0 / 30.0;
        double finish = start;
        for (const PlanAction& action : fragment.actions) {
            if (!supported || !action.known || action.conditional) { supported = false; break; }
            if (!action.enabled || !action.scheduled) continue;
            if ((!action.spawn && !action.display_enemy_info) ||
                (action.spawn && (action.key.empty() || stage.spawns.size() + action.count > 4096)) ||
                action.count <= 0 || action.count > 4096 || !std::isfinite(action.delay) ||
                !std::isfinite(action.interval) || action.delay < 0 || action.interval < 0) {
                supported = false; break;
            }
            const double at = start + action.delay;
            const double last = at + action.interval * (action.count - 1);
            if (!std::isfinite(last) || last > 24.0 * 3600) { supported = false; break; }
            for (int i = 0; action.spawn && i < action.count; ++i) {
                Spawn spawn;
                spawn.time = at + i * action.interval;
                spawn.frame = static_cast<int>(spawn.time * 30 + .5);
                spawn.enemy_key = action.key;
                spawn.route_index = action.route;
                stage.spawns.push_back(std::move(spawn));
            }
            finish = std::max(finish, last + action.count / 30.0);
        }
        if (!supported) {
            stage.spawns.resize(before);
            stage.spawn_plan_conditional = true;
            break;
        }
        cursor = finish;
    }
    std::stable_sort(stage.spawns.begin(), stage.spawns.end(), [](const Spawn& a, const Spawn& b) {
        return a.time < b.time;
    });
    stage.spawn_plan_available = !stage.spawns.empty();
}
} // namespace ark::arkhelper::capture::live
