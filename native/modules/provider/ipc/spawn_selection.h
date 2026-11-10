#pragma once
#include "../data/pack.h"
#include <algorithm>
#include <map>

namespace ark::arkhelper {

inline std::vector<Spawn> select_gate_spawns(const Stage& stage, const std::vector<Spawn>& spawns,
                                            std::uint32_t frame, std::size_t limit) {
    std::map<std::pair<int,int>, std::vector<const Spawn*>> gates;
    for (const auto& spawn : spawns) {
        if ((!spawn.pending && (spawn.frame < 0 || static_cast<std::uint32_t>(spawn.frame) < frame)) ||
            spawn.route_index < 0 || static_cast<std::size_t>(spawn.route_index) >= stage.routes.size()) continue;
        const auto& route = stage.routes[spawn.route_index];
        gates[{route.start_row, route.start_col}].push_back(&spawn);
    }
    for (auto& [point, entries] : gates) {
        std::stable_sort(entries.begin(), entries.end(), [](const Spawn* a, const Spawn* b) {
            if (a->pending != b->pending) return !a->pending;
            if (!a->pending) return a->frame < b->frame;
            if (a->phase != b->phase) return a->phase < b->phase;
            return a->time < b->time;
        });
        const auto* first = entries.front();
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Spawn* s) {
            if (!first->pending) return s->pending;
            return s->phase != first->phase || s->time > first->time + 5;
        }), entries.end());
    }
    // Reserve a place for each entrance before filling the remaining IPC budget.
    std::vector<Spawn> result;
    for (std::size_t round = 0; result.size() < limit; ++round) {
        bool added = false;
        for (const auto& [point, entries] : gates) {
            if (round < entries.size() && result.size() < limit) {
                result.push_back(*entries[round]); added = true;
            }
        }
        if (!added) break;
    }
    return result;
}
} // namespace ark::arkhelper
