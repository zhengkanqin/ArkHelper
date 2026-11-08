#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
#include "../capture/capture.h"
#include "../../damage_meter/stats.h"

namespace ark::arkhelper::ipc_publish {
inline void attach_operator_instances(::arkhelp::ipc::BattleSnapshot& snapshot,
                                      const capture::OperatorSample* operators, int count,
                                      const damage_meter::BattleStats& stats, std::uint32_t supported,
                                      float width, float height) {
    namespace ipc = ::arkhelp::ipc;
    snapshot.operator_count = 0;
    if (!operators || count <= 0 || width < 16 || height < 16) return;
    const double duration = std::max(.1, stats.duration());
    const double total = stats.total(true);
    for (int i = 0; i < std::min(count, static_cast<int>(ipc::kMaxUnits)); ++i) {
        const auto& op = operators[i];
        if (!op.instance_id || !(op.flags & 1u) || !op.actor_id[0] ||
            !std::memchr(op.actor_id, '\0', sizeof(op.actor_id)) ||
            !std::isfinite(op.viewport_x) || !std::isfinite(op.viewport_y) ||
            op.viewport_x < -.1f || op.viewport_x > 1.1f || op.viewport_y < -.1f || op.viewport_y > 1.1f) continue;
        const auto index = snapshot.operator_count++;
        auto& entry = snapshot.operators[index]; entry = {};
        snapshot.operator_ids[index] = op.instance_id;
        entry.flags = ipc::kDamageValid | ipc::kDamageScreenValid;
        entry.metric_mask = supported;
        std::memcpy(entry.actor_id, op.actor_id, sizeof(op.actor_id));
        entry.screen = {static_cast<std::int32_t>(std::lround(op.viewport_x * width)),
                        static_cast<std::int32_t>(std::lround((1.f-op.viewport_y) * height))};
        const auto found = stats.instances.find(op.instance_id);
        if (found == stats.instances.end() || found->second.id != op.actor_id) continue;
        const auto& row = found->second;
        entry.total_damage = static_cast<std::int64_t>(std::llround(row.actual));
        entry.metrics[ipc::kStatDamage] = row.actual;
        entry.metrics[ipc::kStatPhysical] = row.types[0].actual;
        entry.metrics[ipc::kStatArts] = row.types[1].actual;
        entry.metrics[ipc::kStatTrue] = row.types[2].actual;
        entry.metrics[ipc::kStatElemental] = row.types[3].actual;
        entry.metrics[ipc::kStatHeal] = row.healing;
        entry.metrics[ipc::kStatCost] = row.deployment_points;
        entry.metrics[ipc::kStatBuildup] = row.elemental_buildup;
        entry.metrics[ipc::kStatTaken] = row.taken;
        entry.metrics[ipc::kStatDps] = row.actual / duration;
        entry.metrics[ipc::kStatShare] = total > 0 ? row.actual / total : 0;
        entry.dps = static_cast<float>(entry.metrics[ipc::kStatDps]);
        entry.share = static_cast<float>(entry.metrics[ipc::kStatShare]);
    }
}

inline void attach_operator_positions(::arkhelp::ipc::BattleSnapshot& snapshot,
                                      const capture::OperatorSample* operators, int count,
                                      std::uint32_t supported, float width, float height) {
    namespace ipc = ::arkhelp::ipc;
    snapshot.damage_count = std::min(snapshot.damage_count, ipc::kMaxDamageRows);
    for (std::uint32_t i = 0; i < snapshot.damage_count; ++i)
        snapshot.damage[i].flags &= ~ipc::kDamageScreenValid;
    if (!operators || count <= 0 || width < 16 || height < 16) return;
    for (int index = 0; index < std::min(count, static_cast<int>(ipc::kMaxUnits)); ++index) {
        const auto& op = operators[index];
        if (!op.actor_id[0] || !std::memchr(op.actor_id, '\0', sizeof(op.actor_id))) continue;
        std::uint32_t row = snapshot.damage_count;
        for (std::uint32_t i = 0; i < snapshot.damage_count; ++i)
            if (std::strcmp(snapshot.damage[i].actor_id, op.actor_id) == 0) { row = i; break; }
        if (row == snapshot.damage_count) {
            if (row >= ipc::kMaxDamageRows) { snapshot.stats_flags |= 2u; continue; }
            auto& entry = snapshot.damage[snapshot.damage_count++];
            entry = {};
            entry.flags = ipc::kDamageValid;
            entry.metric_mask = supported;
            std::memcpy(entry.actor_id, op.actor_id, sizeof(op.actor_id));
        }
        auto& entry = snapshot.damage[row];
        if (!(op.flags & 1u) || !std::isfinite(op.viewport_x) || !std::isfinite(op.viewport_y) ||
            op.viewport_x < -.1f || op.viewport_x > 1.1f || op.viewport_y < -.1f || op.viewport_y > 1.1f) continue;
        entry.screen = {static_cast<std::int32_t>(std::lround(op.viewport_x*width)),
                        static_cast<std::int32_t>(std::lround((1.f-op.viewport_y)*height))};
        entry.flags |= ipc::kDamageScreenValid;
    }
}
} // namespace ark::arkhelper::ipc_publish
