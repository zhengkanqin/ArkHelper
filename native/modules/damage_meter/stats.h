#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <ark/battle.h>

namespace ark::damage_meter {

inline constexpr std::size_t kDamageTypeCount = 4;
inline constexpr std::array<const char*, kDamageTypeCount> kDamageTypeNames = {"物理", "法术",
                                                                               "真实", "元素"};

// Maps a native damage type to its column in ActorRow::types, or -1 when it is not tracked.
inline int damage_type_index(std::int32_t type) {
    switch (type) {
    case ARK_DAMAGE_PHYSICAL:
        return 0;
    case ARK_DAMAGE_MAGICAL:
        return 1;
    case ARK_DAMAGE_TRUE:
        return 2;
    case ARK_DAMAGE_ELEMENTAL:
        return 3;
    default:
        return -1;
    }
}

struct DamageTotals {
    double calculated = 0;
    double actual = 0;
};

struct ActorRow {
    std::string id;
    double calculated = 0;
    double actual = 0;
    double elemental_buildup = 0;
    double healing = 0;
    double deployment_points = 0;
    double taken = 0;
    std::array<DamageTotals, kDamageTypeCount> types{};
};

// Per-actor totals of one battle, rebuilt by replaying the event history.
class BattleStats {
public:
    // Applies one event. Events at or below the last consumed sequence are ignored, so
    // replaying the same history is idempotent.
    void consume(const ArkEventV1& event) {
        if (event.sequence && event.sequence <= sequence) {
            return;
        }
        sequence = event.sequence;
        if (event.kind == ARK_BATTLE_START) {
            start_battle(event);
            return;
        }
        if (event.kind == ARK_CAPABILITIES) {
            capabilities = static_cast<std::uint32_t>(event.damage_type);
            return;
        }
        game_time = std::max(game_time, event.game_time);
        if (event.kind == ARK_BATTLE_END) {
            ended = true;
            return;
        }
        if (event.kind == ARK_DAMAGE || event.kind == ARK_ELEMENTAL || event.kind == ARK_HEAL ||
            event.kind == ARK_COST || event.kind == ARK_TAKEN) {
            add_damage(event);
        }
    }

    double duration() const { return std::max(0.0, game_time - start_time); }

    double total(bool actual) const {
        double sum = 0;
        for (const auto& [id, row] : rows) {
            sum += actual ? row.actual : row.calculated;
        }
        return sum;
    }

    // Orders rows by descending actual or calculated damage, ties broken by actor ID.
    void sort_rows(std::vector<const ActorRow*>& out, bool by_actual) const {
        out.clear();
        out.reserve(rows.size());
        for (const auto& [id, row] : rows) {
            out.push_back(&row);
        }
        std::sort(out.begin(), out.end(), [by_actual](const ActorRow* a, const ActorRow* b) {
            const double left = by_actual ? a->actual : a->calculated;
            const double right = by_actual ? b->actual : b->calculated;
            return left == right ? a->id < b->id : left > right;
        });
    }

    std::map<std::string, ActorRow> rows;
    std::map<std::uint64_t, ActorRow> instances;
    std::string stage;
    std::uint64_t sequence = 0;
    std::uint64_t battle = 0;
    double game_time = 0;
    // Partial battles (joined mid-way) measure duration from the first observed event.
    double start_time = 0;
    bool partial = false;
    bool ended = false;
    std::uint32_t capabilities = 0;

private:
    void start_battle(const ArkEventV1& event) {
        rows.clear();
        instances.clear();
        stage = event.stage_id;
        battle = event.battle;
        partial = event.partial != 0;
        start_time = partial ? event.game_time : 0;
        game_time = event.game_time;
        ended = false;
        capabilities = 0;
    }

    void add_damage(const ArkEventV1& event) {
        if (!event.actor_id[0] || !std::isfinite(event.calculated_damage) ||
            !std::isfinite(event.actual_damage)) {
            return;
        }
        if ((event.kind == ARK_HEAL || event.kind == ARK_COST || event.kind == ARK_TAKEN ||
             (event.kind == ARK_DAMAGE && event.damage_type == ARK_DAMAGE_HEAL)) && event.actual_damage <= 0) return;
        if (event.kind == ARK_DAMAGE && event.damage_type != ARK_DAMAGE_HEAL &&
            damage_type_index(event.damage_type) < 0) return;
        // Table rows aggregate by character; on-field labels require the actual source/target UID.
        add_to_row(event, rows[event.actor_id]);
        if (event.actor_instance) {
            if (instances.size() < 4096 || instances.count(event.actor_instance))
                add_to_row(event, instances[event.actor_instance]);
            else partial = true;
        }
    }

    void add_to_row(const ArkEventV1& event, ActorRow& row) {
        if (event.kind == ARK_HEAL || (event.kind == ARK_DAMAGE && event.damage_type == ARK_DAMAGE_HEAL) ||
            event.kind == ARK_COST || event.kind == ARK_TAKEN) {
            // Resource loss, over-heal and natural deployment point regeneration are not contributions.
            if (event.actual_damage <= 0) return;
            row.id = event.actor_id;
            if (event.kind == ARK_COST) { row.deployment_points += event.actual_damage; capabilities |= 1u << ARK_COST; }
            else if (event.kind == ARK_TAKEN) { row.taken += event.actual_damage; capabilities |= 1u << ARK_TAKEN; }
            else { row.healing += event.actual_damage; capabilities |= 1u << ARK_HEAL; }
            return;
        }
        const int type = damage_type_index(event.damage_type);
        if (event.kind == ARK_DAMAGE && type < 0) {
            return;
        }
        row.id = event.actor_id;
        // Elemental buildup is tracked separately and never counts as HP damage.
        if (event.kind == ARK_ELEMENTAL) {
            row.elemental_buildup += std::abs(event.calculated_damage);
            return;
        }
        const double calculated = std::abs(event.calculated_damage);
        const double actual = std::abs(event.actual_damage);
        row.calculated += calculated;
        row.actual += actual;
        DamageTotals& totals = row.types[static_cast<std::size_t>(type)];
        totals.calculated += calculated;
        totals.actual += actual;
    }
};

} // namespace ark::damage_meter
