#include <cstdio>
#include <cstdlib>
#include <cmath>
#include "../native/modules/provider/app/app.h"
#include "../native/modules/provider/capture/live_data.h"

namespace {
using ark::arkhelper::Stage;
Stage captured;
std::uint32_t serial = 0;
int copies = 0;
int checks = 0;
void check(bool pass, const char* name) {
    ++checks;
    if (!pass) { std::fprintf(stderr, "FAIL dynamic data: %s\n", name); std::exit(1); }
}
struct FakeEnemy { std::int64_t fixed_hp; float speed; int defense; };
std::int64_t hp(void* self, void*) { return static_cast<FakeEnemy*>(self)->fixed_hp; }
float speed(void* self, void*) { return static_cast<FakeEnemy*>(self)->speed; }
int defense(void* self, void*) { return static_cast<FakeEnemy*>(self)->defense; }
std::int64_t attribute(void* self, int index, void*) {
    return static_cast<FakeEnemy*>(self)->fixed_hp * index;
}
}

namespace ark::arkhelper::capture {
bool runtime_stage(Stage& out, std::uint32_t* battle_serial, std::uint32_t known_revision,
                   std::uint32_t known_serial) {
    if (captured.level_id.empty() ||
        (known_revision && captured.revision == known_revision && serial == known_serial)) return false;
    ++copies;
    out = captured;
    if (battle_serial) *battle_serial = serial;
    return true;
}
}

int main() {
    ArkPluginApiV1 host{};
    host.root_directory = "Z:/this-path-and-its-data-directory-do-not-exist";
    ark::arkhelper::App app(host);
    app.load_data();
    check(app.loaded(), "starts without any offline package");
    check(app.pack().stages.empty(), "no stage catalog preloaded");
    app.refresh_runtime_stage("rogue/new_unlisted_stage", 1);
    check(app.selected_stage() && app.selected_stage()->runtime, "unknown level accepted");
    check(!app.selected_stage()->spawn_plan_available, "unknown plan remains unavailable");
    check(!app.selected_stage()->loaded, "placeholder does not claim live level data was read");
    captured = *app.selected_stage(); captured.revision = 11; serial = 1;
    captured.map.rows = 12; captured.map.cols = 18;
    captured.enemies.resize(1); captured.enemies[0].key = "enemy_buffed";
    captured.enemies[0].attributes.atk = 1500;
    captured.enemies[0].stats_mask = 4;
    app.refresh_runtime_stage(captured.level_id, 1);
    check(app.selected_stage()->map.rows == 12, "live layout replaces placeholder");
    check(app.selected_stage()->enemies[0].stats_mask == 4, "unavailable stats stay unavailable");
    const int before = copies;
    for (int i = 0; i < 1000; ++i) app.refresh_runtime_stage(captured.level_id, 1);
    check(copies == before, "unchanged revision avoids repeated large copies");
    app.refresh_runtime_stage(captured.level_id, 2);
    check(app.selected_stage()->enemies.empty() && app.selected_stage()->map.rows == 0,
          "same-level new battle discards previous buffs and map");
    serial = 2; captured.revision = 12; captured.enemies[0].attributes.atk = 2250;
    app.refresh_runtime_stage(captured.level_id, 2);
    check(app.selected_stage()->enemies[0].attributes.atk == 2250, "same level different run accepts different values");
    app.refresh_runtime_stage("sandbox/generated_map", 3);
    check(app.pack().stages.size() == 1 && app.selected_stage()->enemies.empty(), "cache bounded to current stage");
    check(app.selected_stage()->level_id == "sandbox/generated_map", "stale level rejected");
    app.load_data();
    check(app.pack().stages.empty(), "reload does not restore catalog or stale values");

    using ark::arkhelper::capture::live::merge_enemy_name;
    Stage named_stage;
    check(merge_enemy_name(named_stage, "enemy_10260_dsjzds", "“地钻”") &&
          named_stage.enemies.front().name == "“地钻”" && named_stage.enemies.front().stats_mask == 0,
          "current-level name lookup adds Chinese text without inventing numeric reference stats");
    check(!merge_enemy_name(named_stage, "enemy_10260_dsjzds", "other") &&
          named_stage.enemies.front().name == "“地钻”", "a resolved name is not overwritten by another source");
    check(merge_enemy_name(named_stage, "enemy_10257_dsjtsz", "卸石苦力") &&
          named_stage.enemies.size() == 2, "a second current-level enemy resolves independently");
    check(!merge_enemy_name(named_stage, "enemy_10257_dsjtszx", "") &&
          named_stage.enemies.size() == 2, "an unknown alias is not guessed from a similar ID");
    check(merge_enemy_name(named_stage, "enemy_10257_dsjtszx", "当前实例中文名") &&
          named_stage.enemies.back().key == "enemy_10257_dsjtszx", "live keyInLevel supplies the exact alias mapping");

    using ark::arkhelper::capture::live::NumericGetter;
    FakeEnemy first{1000LL * 4294967296LL, 1.25f, 400};
    FakeEnemy second{1800LL * 4294967296LL, 0.75f, 900};
    NumericGetter get_hp{nullptr, reinterpret_cast<void*>(&hp), 5};
    NumericGetter get_speed{nullptr, reinterpret_cast<void*>(&speed), 1};
    NumericGetter get_def{nullptr, reinterpret_cast<void*>(&defense), 3};
    NumericGetter get_attribute{nullptr, reinterpret_cast<void*>(&attribute), 5};
    double value = 0;
    check(get_hp.read(&first, value) && value == 1000, "FP getter decoding");
    check(get_hp.read(&second, value) && value == 1800, "same metadata reads each instance independently");
    check(get_speed.read(&first, value) && value == 1.25, "floating-point getter ABI");
    check(get_def.read(&second, value) && value == 900, "integer getter ABI");
    const int enum_id = 2;
    check(get_attribute.read(&second, value, &enum_id) && value == 3600, "verified enum argument ABI");
    NumericGetter missing;
    check(!missing.read(&first, value), "missing getter remains unavailable");

    using namespace ark::arkhelper::capture::live;
    PlanAction normal;
    normal.key = "enemy_a"; normal.count = 2; normal.interval = 1;
    PlanFragment initial;
    initial.actions.push_back(normal);
    PlanWave wave;
    wave.fragments.push_back(initial);
    Stage plan;
    build_spawn_prefix(plan, wave, true, false);
    check(plan.spawn_plan_available && plan.spawn_plan_conditional && plan.spawns.size() == 2,
          "multiple waves retain only definite first-wave prefix");
    check(std::abs(plan.spawns[0].time - 2.0 / 30.0) < 1e-8 &&
          std::abs(plan.spawns[1].time - (1 + 2.0 / 30.0)) < 1e-8, "first-wave timestamp and interval");
    PlanFragment random_fragment = initial;
    PlanAction random = normal; random.conditional = true;
    random_fragment.actions.push_back(random);
    wave.fragments.push_back(random_fragment);
    wave.fragments.push_back(initial);
    build_spawn_prefix(plan, wave, false, false);
    check(plan.spawn_plan_available && plan.spawn_plan_conditional && plan.spawns.size() == 2,
          "random action removes its entire uncertain fragment and later fragments");
    wave.fragments = {random_fragment};
    build_spawn_prefix(plan, wave, false, false);
    check(!plan.spawn_plan_available && plan.spawns.empty() && plan.spawn_plan_conditional,
          "unknown first fragment cannot leak a guessed spawn");
    PlanAction invalid = normal; invalid.enabled = false;
    wave.fragments = {PlanFragment{true, 0, {invalid, normal}}};
    build_spawn_prefix(plan, wave, false, false);
    check(plan.spawns.size() == 2 && !plan.spawn_plan_conditional,
          "disabled runtime actions are excluded");
    wave.fragments[0].actions[1].known = false;
    build_spawn_prefix(plan, wave, false, false);
    check(plan.spawns.empty() && plan.spawn_plan_conditional, "missing critical metadata is not treated as zero");
    normal.interval = INFINITY;
    wave.fragments = {PlanFragment{true, 0, {normal}}};
    build_spawn_prefix(plan, wave, false, false);
    check(plan.spawns.empty(), "unbounded timeline is rejected");
    normal.interval = 1;
    PlanAction info = normal;
    info.spawn = false; info.display_enemy_info = true; info.count = 1; info.key.clear();
    normal.count = 3;
    wave.fragments = {PlanFragment{true, 0, {info, normal}}, PlanFragment{true, 8, {normal}}};
    build_spawn_prefix(plan, wave, false, false);
    check(plan.spawn_plan_available && !plan.spawn_plan_conditional && plan.spawns.size() == 6 &&
          plan.spawns.front().enemy_key == normal.key,
          "initial enemy-info notification cannot discard normal spawns or become a fake enemy");
    info.delay = 10;
    wave.fragments = {PlanFragment{true, 0, {info, normal}}, PlanFragment{true, 8, {normal}}};
    build_spawn_prefix(plan, wave, false, false);
    check(plan.spawns.size() == 6 && std::abs(plan.spawns[3].time - (18 + 4.0/30.0)) < 1e-8,
          "a delayed enemy-info action contributes to the fragment completion time");
    info.display_enemy_info = false;
    wave.fragments = {PlanFragment{true, 0, {info, normal}}};
    build_spawn_prefix(plan, wave, false, false);
    check(plan.spawns.empty() && plan.spawn_plan_conditional,
          "unsupported non-spawn actions still cannot produce guessed countdowns");
    info.display_enemy_info = true; info.conditional = true;
    wave.fragments = {PlanFragment{true, 0, {info, normal}}};
    build_spawn_prefix(plan, wave, false, false);
    check(plan.spawns.empty() && plan.spawn_plan_conditional,
          "conditional enemy-info actions remain outside a deterministic prefix");
    info.conditional = false; info.delay = INFINITY;
    wave.fragments = {PlanFragment{true, 0, {info, normal}}};
    build_spawn_prefix(plan, wave, false, false);
    check(plan.spawns.empty() && plan.spawn_plan_conditional,
          "invalid enemy-info timing cannot leak an unbounded countdown");
    normal.count = 4;
    normal.key = "future_enemy";
    PlanWave later;
    later.fragments = {PlanFragment{true, 0, {normal}}};
    wave.fragments = {initial};
    Stage overview;
    append_spawn_overview(overview, {wave, later});
    check(overview.overview_plan_read && overview.overview_spawns.size() == 2 &&
          overview.overview_spawns.back().wave == 1 && overview.overview_spawns.back().count == 4,
          "full overview includes future waves and preserves grouped spawn counts");
    random.key = "conditional_enemy";
    later.fragments = {PlanFragment{true, 0, {random, normal}}};
    append_spawn_overview(overview, {later}, 1);
    check(overview.overview_spawns.size() == 4 && overview.overview_spawns[2].conditional &&
          overview.overview_spawns[3].branch == 1, "conditional actions and actions after them remain in full branch overview");
    build_spawn_prefix(overview, later, true, true);
    check(overview.spawns.empty() && overview.overview_spawns.size() == 4,
          "unsafe countdown prefix cannot discard full-level overview or invent timestamps");
    std::printf("Dynamic data: %d checks, 0 failures\n", checks);
}
