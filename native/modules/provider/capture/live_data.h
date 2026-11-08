#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include "capture.h"
#include "spawn_plan.h"
#include "enemy_names.h"
#include "il2cpp_runtime.h"
#include "il2cpp_exports.h"

namespace ark::arkhelper::capture::live {

inline void* field(il2cpp::Runtime& rt, void* klass, const char* name) {
    for (int depth = 0; klass && depth < 12; ++depth, klass = rt.class_parent(klass)) {
        if (void* result = rt.class_field(klass, name)) return result;
    }
    return nullptr;
}
inline void* method(il2cpp::Runtime& rt, void* klass, const char* name, int arity = 0) {
    try { return rt.method(klass, name, arity); } catch (...) { return nullptr; }
}
// Exact field/property names only; missing or renamed metadata is unavailable.
inline void* member(il2cpp::Runtime& rt, void* object, const char* name) {
    if (!object) return nullptr;
    void* klass = rt.object_class(object);
    if (void* f = field(rt, klass, name)) return rt.field_get_object(f, object);
    const std::string backing = "<" + std::string(name) + ">k__BackingField";
    if (void* f = field(rt, klass, backing.c_str())) return rt.field_get_object(f, object);
    const std::string getter = "get_" + std::string(name);
    if (void* m = method(rt, klass, getter.c_str())) return rt.invoke(m, object);
    return nullptr;
}
inline std::string text(il2cpp::Runtime& rt, void* object) {
    if (!object || rt.klass_name(rt.object_class(object)) != "System.String") return {};
    return rt.string(object);
}
inline bool number(il2cpp::Runtime& rt, void* boxed, double& value) {
    if (!boxed) return false;
    void* klass = rt.object_class(boxed);
    const std::string name = rt.klass_name(klass);
    void* data = rt.object_unbox(boxed);
    if (name == "System.Single") { float v; std::memcpy(&v, data, 4); value = v; }
    else if (name == "System.Double") { std::memcpy(&value, data, 8); }
    else if (name == "System.Int32") { std::int32_t v; std::memcpy(&v, data, 4); value = v; }
    else if (name == "System.UInt32") { std::uint32_t v; std::memcpy(&v, data, 4); value = v; }
    else if (name == "System.Boolean") { bool v; std::memcpy(&v, data, 1); value = v ? 1 : 0; }
    else if (name == "Torappu.FP") {
        std::uint32_t align = 0;
        if (rt.class_value_size(klass, &align) != 8) return false;
        std::int64_t v; std::memcpy(&v, data, 8); value = static_cast<double>(v) / 4294967296.0;
    } else if (name.find("System.Nullable") == 0) {
        double has_value = 0;
        if (!number(rt, member(rt, boxed, "HasValue"), has_value) || !has_value) return false;
        return number(rt, member(rt, boxed, "Value"), value);
    } else return false;
    return std::isfinite(value);
}
inline double scalar(il2cpp::Runtime& rt, void* object, const char* name, double fallback = 0) {
    double result = fallback;
    return number(rt, member(rt, object, name), result) ? result : fallback;
}
inline bool has_member(il2cpp::Runtime& rt, void* object, const char* name) {
    if (!object) return false;
    void* klass = rt.object_class(object);
    return field(rt, klass, name) || method(rt, klass, ("get_" + std::string(name)).c_str());
}
inline bool optional_bool(il2cpp::Runtime& rt, void* object, const char* name,
                          bool unset_default, bool& known) {
    if (!has_member(rt, object, name)) { known = false; return unset_default; }
    void* value = member(rt, object, name);
    if (!value) return unset_default; // Nullable<T> with no value boxes as null.
    if (rt.klass_name(rt.object_class(value)).find("System.Nullable") == 0) {
        double has_value = 0;
        if (!number(rt, member(rt, value, "HasValue"), has_value)) { known = false; return unset_default; }
        if (!has_value) return unset_default;
    }
    double result = 0;
    if (!number(rt, value, result)) { known = false; return unset_default; }
    return result != 0;
}
inline int count(il2cpp::Runtime& rt, void* object, int maximum) {
    if (!object) return 0;
    return std::clamp(rt.count(object), 0, maximum);
}
inline std::string enum_name(il2cpp::Runtime& rt, void* value) {
    if (!value) return {};
    if (rt.klass_name(rt.object_class(value)) == "System.String") return rt.string(value);
    void* klass = rt.object_class(value);
    std::uint32_t align = 0;
    if (!rt.class_is_value(klass) || rt.class_value_size(klass, &align) != 4) return {};
    int wanted = 0; std::memcpy(&wanted, rt.object_unbox(value), 4);
    void* cursor = nullptr;
    while (void* f = rt.class_fields(klass, &cursor)) {
        if (rt.class_from_type(rt.field_type(f)) != klass) continue;
        int item = 0; rt.field_static_get(f, &item);
        if (item == wanted) return rt.field_name(f);
    }
    return {};
}

struct NumericGetter {
    void* method = nullptr;
    void* entry = nullptr;
    int kind = 0; // float, double, int32, uint32, FP (verified 8 byte signed fixed point)
    bool read(void* object, double& value, const int* argument = nullptr) const {
        if (!entry || !object || !kind) return false;
#define ARK_READ_VALUE(T) (argument ? reinterpret_cast<T (*)(void*, int, void*)>(entry)(object, *argument, method) : reinterpret_cast<T (*)(void*, void*)>(entry)(object, method))
        switch (kind) {
        case 1: value = ARK_READ_VALUE(float); break;
        case 2: value = ARK_READ_VALUE(double); break;
        case 3: value = ARK_READ_VALUE(std::int32_t); break;
        case 4: value = ARK_READ_VALUE(std::uint32_t); break;
        case 5: value = static_cast<double>(ARK_READ_VALUE(std::int64_t)) / 4294967296.0; break;
        default: return false;
        }
#undef ARK_READ_VALUE
        return std::isfinite(value);
    }
};
inline NumericGetter getter(il2cpp::Runtime& rt, void* m) {
    NumericGetter result;
    if (!m) return result;
    // All live Entity/Enemy/Attributes getters are reference receivers. Refuse unknown
    // value-type receivers rather than calling a native ABI with a boxed this pointer.
    if (rt.class_is_value(rt.method_class(m))) return result;
    std::uint32_t impl = 0;
    if (rt.method_flags(m, &impl) & 0x10u) return result;
    void* klass = rt.class_from_type(rt.method_return(m));
    if (!klass) return result;
    const std::string name = rt.klass_name(klass);
    if (name == "System.Single") result.kind = 1;
    else if (name == "System.Double") result.kind = 2;
    else if (name == "System.Int32") result.kind = 3;
    else if (name == "System.UInt32") result.kind = 4;
    else if (name == "Torappu.FP") {
        std::uint32_t align = 0;
        if (rt.class_value_size(klass, &align) == 8) result.kind = 5;
    }
    result.method = m;
    result.entry = *static_cast<void**>(m);
    if (!il2cpp::is_executable(result.entry)) result = {};
    return result;
}

inline std::string normalized(const char* text) {
    std::string result;
    for (; text && *text; ++text) {
        const unsigned char ch = static_cast<unsigned char>(*text);
        if (ch >= 'A' && ch <= 'Z') result.push_back(static_cast<char>(ch + 'a' - 'A'));
        else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) result.push_back(ch);
    }
    return result;
}

// Metadata caches are bounded and contain no managed object references. Current values always
// come from the current instance: same template can have different buffs in the same battle.
class EnemyReader {
    struct Access {
        void* klass = nullptr;
        void* attributes = nullptr;
        void* key = nullptr;
        void* uid = nullptr;
        void* data = nullptr;
        void* data_name = nullptr;
        void* data_key = nullptr;
        NumericGetter getters[11]{};
        NumericGetter attribute_value;
        int attribute_ids[11]{};
        std::uint32_t attribute_mask = 0;
    };
    std::array<Access, 16> cache_{};
    std::size_t next_ = 0;
    Access& access(il2cpp::Runtime& rt, void* klass) {
        for (auto& item : cache_) if (item.klass == klass) return item;
        Access& item = cache_[next_++ % cache_.size()];
        item = {}; item.klass = klass;
        item.attributes = field(rt, klass, "m_attributes");
        item.key = field(rt, klass, "<tmplId>k__BackingField");
        if (!item.key) item.key = field(rt, klass, "<id>k__BackingField");
        item.uid = field(rt, klass, "<instanceUid>k__BackingField");
        item.data = field(rt, klass, "<data>k__BackingField");
        if (item.data) {
            void* data_class = rt.class_from_type(rt.field_type(item.data));
            item.data_name = data_class ? field(rt, data_class, "name") : nullptr;
            item.data_key = data_class ? method(rt, data_class, "get_keyInLevel") : nullptr;
        }
        constexpr const char* names[] = {"get_hp", "get_maxHp", "get_atk", "get_def",
            "get_magicResistance", "get_moveSpeed", "get_attackSpeed", "get_baseAttackTime",
            "get_massLevel", "get_blockCnt", "get_lifePointReduce"};
        for (int index = 0; index < 11; ++index) item.getters[index] = getter(rt, method(rt, klass, names[index]));
        void* aklass = item.attributes ? rt.class_from_type(rt.field_type(item.attributes)) : nullptr;
        void* get_value = aklass ? method(rt, aklass, "GetValue", 1) : nullptr;
        item.attribute_value = getter(rt, get_value);
        if (get_value && item.attribute_value.kind) {
            void* enum_class = rt.class_from_type(rt.method_param(get_value, 0));
            std::uint32_t align = 0;
            if (enum_class && rt.class_is_value(enum_class) && rt.class_value_size(enum_class, &align) == 4) {
                constexpr const char* names[] = {"hp", "maxhp", "atk", "def", "magicresistance",
                    "movespeed", "attackspeed", "baseattacktime", "masslevel", "blockcnt", "lifepointreduce"};
                void* cursor = nullptr;
                while (void* f = rt.class_fields(enum_class, &cursor)) {
                    if (rt.class_from_type(rt.field_type(f)) != enum_class) continue;
                    const std::string name = normalized(rt.field_name(f));
                    for (int index = 1; index < 11; ++index) {
                        if (name == names[index]) {
                            rt.field_static_get(f, &item.attribute_ids[index]);
                            item.attribute_mask |= 1u << index;
                        }
                    }
                }
            }
        }
        return item;
    }
public:
    void clear() { cache_ = {}; next_ = 0; }
    EnemySample read(il2cpp::Runtime& rt, void* unit, std::uint32_t serial) {
        EnemySample out;
        Access& a = access(rt, rt.object_class(unit));
        std::uint32_t uid = 0;
        if (a.uid && rt.klass_name(rt.class_from_type(rt.field_type(a.uid))) == "System.UInt32") rt.field_get(unit, a.uid, &uid);
        out.tracking_id = uid ? (static_cast<std::uint64_t>(serial) << 32u) | uid :
                              reinterpret_cast<std::uintptr_t>(unit);
        if (a.key) {
            const std::string key = text(rt, rt.field_get_object(a.key, unit));
            std::snprintf(out.enemy_key, sizeof(out.enemy_key), "%s", key.c_str());
        }
        if (a.data) {
            if (void* data = rt.field_get_object(a.data, unit)) {
                if (a.data_name) {
                    const std::string name = text(rt, rt.field_get_object(a.data_name, data));
                    std::snprintf(out.enemy_name, sizeof(out.enemy_name), "%s", name.c_str());
                }
                if (a.data_key) {
                    const std::string key = text(rt, rt.invoke(a.data_key, data));
                    if (!key.empty()) std::snprintf(out.enemy_key, sizeof(out.enemy_key), "%s", key.c_str());
                }
            }
        }
        void* attributes = a.attributes ? rt.field_get_object(a.attributes, unit) : nullptr;
        double values[11]{};
        for (int index = 0; index < 11; ++index) {
            bool ok = a.getters[index].read(unit, values[index]);
            if (!ok && attributes && (a.attribute_mask & (1u << index))) {
                ok = a.attribute_value.read(attributes, values[index], &a.attribute_ids[index]);
            }
            if (ok) out.stats_mask |= 1u << index;
        }
        out.hp = values[0]; out.max_hp = values[1]; out.atk = values[2]; out.def = values[3];
        out.magic_resistance = values[4]; out.move_speed = values[5]; out.attack_speed = values[6];
        out.base_attack_time = values[7]; out.weight = values[8];
        out.block_cnt = static_cast<int>(std::clamp(values[9], -2147483647.0, 2147483647.0));
        out.life_point_reduce = static_cast<int>(std::clamp(values[10], -2147483647.0, 2147483647.0));
        return out;
    }
};

inline Enemy definition(il2cpp::Runtime& rt, void* data) {
    Enemy enemy;
    enemy.key = text(rt, member(rt, data, "keyInLevel"));
    if (enemy.key.empty()) enemy.key = text(rt, member(rt, data, "key"));
    enemy.name = text(rt, member(rt, data, "name"));
    enemy.description = text(rt, member(rt, data, "description"));
    void* attributes = member(rt, data, "attributes");
    constexpr const char* names[] = {"maxHp", "atk", "def", "magicResistance", "moveSpeed",
                                    "attackSpeed", "baseAttackTime", "massLevel", "blockCnt"};
    double values[9]{};
    for (int index = 0; index < 9; ++index) {
        if (number(rt, member(rt, attributes, names[index]), values[index])) enemy.stats_mask |= 2u << index;
    }
    enemy.attributes.max_hp = values[0]; enemy.attributes.atk = values[1];
    enemy.attributes.def = values[2]; enemy.attributes.magic_resistance = values[3];
    enemy.attributes.move_speed = values[4]; enemy.attributes.attack_speed = values[5];
    enemy.attributes.base_attack_time = values[6]; enemy.attributes.mass_level = values[7];
    enemy.attributes.block_cnt = static_cast<int>(std::clamp(values[8], -2147483647.0, 2147483647.0));
    double life = 0;
    if (number(rt, member(rt, data, "lifePointReduce"), life)) {
        enemy.attributes.life_point_reduce = static_cast<int>(std::clamp(life, -2147483647.0, 2147483647.0));
        enemy.stats_mask |= 1u << 10;
    }
    return enemy;
}

inline void definitions(il2cpp::Runtime& rt, void* level, Stage& stage) {
    void* enemies = member(rt, level, "enemies");
    if (!enemies) return;
    // Both list and dictionary builds implement IEnumerable. Boxed enumerator receivers are
    // correctly unboxed by Runtime::invoke; no object is retained beyond this one capture.
    void* enumerator = rt.call(enemies, "GetEnumerator");
    il2cpp::Runtime::Root root(rt, enumerator);
    for (int i = 0; i < 4096; ++i) {
        if (!rt.unbox<bool>(rt.call(root.get(), "MoveNext"))) break;
        void* data = rt.call(root.get(), "get_Current");
        if (void* value = member(rt, data, "Value")) data = value;
        Enemy enemy = definition(rt, data);
        if (!enemy.key.empty()) {
            stage.enemy_keys.push_back(enemy.key);
            stage.enemies.push_back(std::move(enemy));
        }
    }
    if (rt.unbox<bool>(rt.call(root.get(), "MoveNext"))) stage.overview_partial = true;
}

inline void reference_names(il2cpp::Runtime& rt, void* level, Stage& stage) {
    void* references = member(rt, level, "enemyDbRefs");
    const int total = count(rt, references, 4096);
    if (references && rt.count(references) > 4096) stage.overview_partial = true;
    if (!total) return;
    // EnemyDB is already initialized by the game. Invoke only its name lookup for IDs referenced
    // by this level, rather than enumerate/copy the full database or download an offline table.
    void* db_class = rt.find("Torappu", "EnemyDB");
    void* get_name = db_class ? method(rt, db_class, "GetEnemyName", 1) : nullptr;
    if (!get_name) return;
    void* returned = rt.class_from_type(rt.method_return(get_name));
    void* argument = rt.class_from_type(rt.method_param(get_name, 0));
    if (!returned || !argument || rt.klass_name(returned) != "System.String" ||
        rt.klass_name(argument) != "System.String") return;
    void* db = rt.singleton(db_class);
    for (int i = 0; i < total; ++i) {
        try {
            void* reference = rt.at(references, i);
            void* id = member(rt, reference, "id");
            const std::string key = text(rt, id);
            if (key.empty()) continue;
            bool named = false;
            for (const auto& enemy : stage.enemies)
                if (enemy.key == key && !enemy.name.empty() && enemy.name != key) { named = true; break; }
            if (named) continue;
            const auto name = text(rt, rt.invoke(get_name, db, {id}));
            merge_enemy_name(stage, key, name);
        } catch (...) { /* One unavailable name cannot discard the current level. */ }
    }
}

// Read copied level data only on the game thread; nothing managed crosses to the publisher.
inline PlanWave read_plan_wave(il2cpp::Runtime& rt, void* wave, Stage& stage, std::size_t& budget) {
    PlanWave plan;
    // BranchData.phases contains PhaseData (preDelay + actions), not WaveData.
    const bool phase = has_member(rt, wave, "actions") && !has_member(rt, wave, "fragments");
    plan.known = phase || number(rt, member(rt, wave, "preDelay"), plan.delay);
    void* fragments = member(rt, wave, "fragments");
    if (!phase && !fragments) { plan.known = false; return plan; }
    if (fragments && rt.count(fragments) > 4096) stage.overview_partial = true;
    for (int f = 0, fn = phase ? 1 : count(rt, fragments, 4096); f < fn; ++f) {
        void* fragment = phase ? wave : rt.at(fragments, f);
        PlanFragment entry;
        entry.known = number(rt, member(rt, fragment, "preDelay"), entry.delay);
        void* actions = member(rt, fragment, "actions");
        if (!actions) entry.known = false;
        if (actions && rt.count(actions) > 4096) stage.overview_partial = true;
        for (int i = 0, n = count(rt, actions, 4096); i < n; ++i) {
            if (++budget > 65536) {
                stage.overview_partial = true;
                plan.fragments.push_back(std::move(entry));
                return plan;
            }
            void* action = rt.at(actions, i);
            PlanAction parsed;
            const std::string type = enum_name(rt, member(rt, action, "actionType"));
            parsed.spawn = type == "SPAWN";
            parsed.known = !type.empty();
            parsed.enabled = optional_bool(rt, action, "isValid", true, parsed.known);
            parsed.scheduled = optional_bool(rt, action, "managedByScheduler", true, parsed.known);
            parsed.conditional = optional_bool(rt, action, "blockFragment", false, parsed.known);
            parsed.conditional |= optional_bool(rt, action, "useExtraRoute", false, parsed.known);
            parsed.conditional |= !text(rt, member(rt, action, "randomSpawnGroupKey")).empty();
            parsed.conditional |= !text(rt, member(rt, action, "randomSpawnGroupPackKey")).empty();
            parsed.conditional |= !text(rt, member(rt, action, "hiddenGroup")).empty();
            parsed.key = text(rt, member(rt, action, "key"));
            double amount = 0, route = -1;
            parsed.known &= number(rt, member(rt, action, "count"), amount);
            parsed.known &= number(rt, member(rt, action, "routeIndex"), route);
            parsed.known &= number(rt, member(rt, action, "preDelay"), parsed.delay);
            parsed.known &= number(rt, member(rt, action, "interval"), parsed.interval);
            parsed.count = static_cast<int>(std::clamp(amount, 0.0, 2147483647.0));
            parsed.route = static_cast<int>(std::clamp(route, -1.0, 65536.0));
            entry.actions.push_back(std::move(parsed));
        }
        plan.fragments.push_back(std::move(entry));
    }
    return plan;
}
inline std::vector<PlanWave> read_plan_waves(il2cpp::Runtime& rt, void* waves, Stage& stage, std::size_t& budget) {
    std::vector<PlanWave> result;
    if (!waves) return result;
    if (rt.count(waves) > 1024) stage.overview_partial = true;
    for (int i = 0, n = count(rt, waves, 1024); i < n; ++i) {
        result.push_back(read_plan_wave(rt, rt.at(waves, i), stage, budget));
        if (budget > 65536) break;
    }
    return result;
}

inline Stage stage(il2cpp::Runtime& rt, void* controller, const char* level_id, std::uint32_t serial) {
    Stage stage;
    stage.runtime = true; stage.revision = serial;
    stage.level_id = level_id; stage.stage_id = level_id; stage.name = "当前战斗";
    void* level = nullptr;
    for (const char* name : {"m_levelData", "levelData"}) {
        void* value = member(rt, controller, name);
        if (value && rt.klass_name(rt.object_class(value)).find("LevelData") != std::string::npos) { level = value; break; }
    }
    if (!level) return stage;
    stage.loaded = true;
    try { definitions(rt, level, stage); } catch (...) { stage.overview_partial = true; }
    try { reference_names(rt, level, stage); } catch (...) { stage.overview_partial = true; }
    void* map = member(rt, level, "mapData");
    // MapData exposes width/height even when its backing map is a CLR rectangular array.
    stage.map.rows = static_cast<int>(std::clamp(scalar(rt, map, "height"), 0.0, 256.0));
    stage.map.cols = static_cast<int>(std::clamp(scalar(rt, map, "width"), 0.0, 256.0));
    void* routes = member(rt, level, "routes");
    for (int i = 0, n = count(rt, routes, 256); i < n; ++i) {
        void* route = rt.at(routes, i);
        void* start = member(rt, route, "startPosition");
        void* end = member(rt, route, "endPosition");
        Route value;
        value.start_row = static_cast<int>(scalar(rt, start, "row"));
        value.start_col = static_cast<int>(scalar(rt, start, "col"));
        value.end_row = static_cast<int>(scalar(rt, end, "row"));
        value.end_col = static_cast<int>(scalar(rt, end, "col"));
        stage.routes.push_back(value);
    }
    // The overview comes from immutable level data, not the scheduler's remaining queue.
    std::size_t budget = 0;
    void* level_waves = member(rt, level, "waves");
    void* scheduler = member(rt, controller, "scheduler");
    void* scheduled_waves = scheduler ? member(rt, scheduler, "m_waves") : nullptr;
    void* waves = level_waves ? level_waves : scheduled_waves;
    auto plan = read_plan_waves(rt, waves, stage, budget);
    if (waves) append_spawn_overview(stage, plan);
    void* branches = member(rt, level, "branches");
    const int branch_count = count(rt, branches, 1024);
    if (branches && rt.count(branches) > 1024) stage.overview_partial = true;
    if (branch_count) try {
        // The current client stores branches in a dictionary keyed by branch name.
        il2cpp::Runtime::Root enumerator(rt, rt.call(branches, "GetEnumerator"));
        for (int b = 0; b < branch_count && budget <= 65536; ++b) {
            if (!rt.unbox<bool>(rt.call(enumerator.get(), "MoveNext"))) break;
            void* branch = rt.call(enumerator.get(), "get_Current");
            if (void* value = member(rt, branch, "Value")) branch = value;
            if (void* phases = member(rt, branch, "phases")) {
                append_spawn_overview(stage, read_plan_waves(rt, phases, stage, budget), b + 1);
            } else if (void* branch_waves = member(rt, branch, "waves")) {
                append_spawn_overview(stage, read_plan_waves(rt, branch_waves, stage, budget), b + 1);
            } else stage.overview_partial = true;
        }
    } catch (...) {
        stage.overview_partial = true; // a missing branch cannot discard the main level plan
    }
    // The countdown renderer still receives only the unambiguous first-wave prefix.
    if (scheduled_waves && count(rt, scheduled_waves, 1024)) {
        std::size_t prefix_budget = 0;
        const auto first = read_plan_wave(rt, rt.at(scheduled_waves, 0), stage, prefix_budget);
        build_spawn_prefix(stage, first, count(rt, scheduled_waves, 1024) > 1, branch_count > 0);
    } else if (!plan.empty()) build_spawn_prefix(stage, plan.front(), plan.size() > 1, branch_count > 0);
    for (const auto& spawn : stage.overview_spawns) {
        if (!find_enemy(stage, spawn.key) && stage.enemies.size() < 4096) {
            Enemy enemy; enemy.key = spawn.key;
            stage.enemy_keys.push_back(enemy.key);
            stage.enemies.push_back(std::move(enemy));
        } else if (!find_enemy(stage, spawn.key)) stage.overview_partial = true;
    }
    return stage;
}
} // namespace ark::arkhelper::capture::live
