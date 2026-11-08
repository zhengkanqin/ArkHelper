// Unit tests for the header-only damage meter model: event history, statistics and stage names.
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "../../native/modules/damage_meter/event_history.h"
#include "../../native/modules/damage_meter/cost_capture.h"
#include "../../native/modules/damage_meter/stage_names.h"
#include "../../native/modules/damage_meter/stats.h"
#include "../../src/data/battle_statistics.h"
#include "../../native/modules/provider/ipc/statistics_reader.h"

using ark::damage_meter::ActorRow;
using ark::damage_meter::BattleStats;
using ark::damage_meter::EventHistory;
using ark::damage_meter::StageNameIndex;

namespace {

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
    }
}

#define ARK_CHECK(condition) check((condition), #condition, __LINE__)

ArkEventV1 make_event(std::uint32_t kind) {
    ArkEventV1 event;
    event.kind = kind;
    return event;
}

void set_id(char (&target)[ARK_MAX_ID], const char* value) {
    std::snprintf(target, sizeof(target), "%s", value);
}

void consume_all(BattleStats& stats, const ArkEventV1* events, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        stats.consume(events[i]);
    }
}

int forwarded_flags=0;
void* forwarded_source=nullptr;
int forwarded_value=0;
int forwarded_side=0;
void* forwarded_method=nullptr;

#if defined(_MSC_VER)
#define COST_NOINLINE __declspec(noinline)
#else
#define COST_NOINLINE __attribute__((noinline))
#endif
COST_NOINLINE bool fixture_modify_cost(void*,int value,void* source,int side,
                                      bool a,bool b,bool c,bool d,bool e,void* method) {
    forwarded_flags=(a?1:0)|(b?2:0)|(c?4:0)|(d?8:0)|(e?16:0);
    forwarded_source=source;forwarded_value=value;forwarded_side=side;forwarded_method=method;
    return c;
}
COST_NOINLINE bool fixture_withdraw(void*,bool a,bool b,bool c,bool d,void* method) {
    forwarded_flags=(a?1:0)|(b?2:0)|(c?4:0)|(d?8:0);
    forwarded_method=method;return a;
}

void test_cost_boolean_abi() {
    using namespace ark::damage_meter;
    static_assert(sizeof(bool)==1);
    ModifyCostFn modify=fixture_modify_cost;
    WithdrawFn withdraw=fixture_withdraw;
    int actor=0,method=0,controller=0;
    for(int flags=0;flags<32;++flags) {
        const bool result=modify(&controller,17,&actor,2,(flags&1)!=0,(flags&2)!=0,
                                  (flags&4)!=0,(flags&8)!=0,(flags&16)!=0,&method);
        ARK_CHECK(forwarded_flags==flags && forwarded_value==17 && forwarded_side==2);
        ARK_CHECK(forwarded_source==&actor && forwarded_method==&method && result==((flags&4)!=0));
    }
    for(int flags=0;flags<16;++flags) {
        const bool result=withdraw(&actor,(flags&1)!=0,(flags&2)!=0,(flags&4)!=0,(flags&8)!=0,&method);
        ARK_CHECK(forwarded_flags==flags && forwarded_method==&method && result==((flags&1)!=0));
    }
}

void test_effective_cost_attribution() {
    using namespace ark::damage_meter;
    ARK_CHECK(attributable_cost(12,true,false,false));
    ARK_CHECK(!attributable_cost(12,false,false,false)); // Natural/unknown/non-operator source.
    ARK_CHECK(!attributable_cost(12,true,true,false)); // Withdrawal refund, including nested scopes.
    ARK_CHECK(!attributable_cost(12,true,false,true)); // Deployment cost adjustment.
    ARK_CHECK(!attributable_cost(0,true,false,false) && !attributable_cost(-8,true,false,false));
    ARK_CHECK(effective_cost_gain(12,10,22)==12);
    ARK_CHECK(effective_cost_gain(12,95,99)==4); // The cap discards overflow.
    ARK_CHECK(effective_cost_gain(12,99,99)==0);
    ARK_CHECK(effective_cost_gain(12,20,18)==0 && effective_cost_gain(0,20,30)==0);
    ARK_CHECK(effective_cost_gain(12,10,40)==12); // Cannot credit more than this source requested.
    ARK_CHECK(effective_cost_gain(12,-1,5)==0);
    ARK_CHECK(effective_cost_gain(std::numeric_limits<int>::max(),0,std::numeric_limits<int>::max())==
              std::numeric_limits<int>::max());
}

void test_nested_cost_attribution() {
    using namespace ark::damage_meter;
    int controller=0, other_controller=0;
    ARK_CHECK(!CostMutationScope::observing(&controller,0));
    {
        CostMutationScope outer(&controller,0,true,10);
        ARK_CHECK(CostMutationScope::observing(&controller,0));
        { CostMutationScope inner(&controller,0,true,10); ARK_CHECK(inner.finish(10,20)==10); }
        ARK_CHECK(outer.finish(10,20)==0); // Recursive implementation credits once.
        ARK_CHECK(outer.finish(10,20)==0); // Closing twice cannot publish again.
    }
    {
        CostMutationScope outer(&controller,0,true,10);
        { CostMutationScope inner(&controller,0,true,12); ARK_CHECK(inner.finish(3,15)==3); }
        ARK_CHECK(outer.finish(5,18)==5); // Each source receives its own actual increase.
    }
    {
        CostMutationScope outer(&controller,0,true,10);
        { CostMutationScope natural(&controller,0,true,12); ARK_CHECK(natural.finish(0,15)==0); }
        ARK_CHECK(outer.finish(9,18)==5); // Excluded nested sources cannot leak to outer actor.
    }
    {
        CostMutationScope outer(&controller,0,true,10);
        { CostMutationScope debit(&controller,0,true,10); ARK_CHECK(debit.finish(0,7)==0); }
        ARK_CHECK(outer.finish(1,8)==1); // A nested debit must not hide a genuine outer gain.
    }
    {
        CostMutationScope outer(&controller,0,true,10);
        {
            CostMutationScope middle(&controller,0,true,12);
            { CostMutationScope inner(&controller,0,true,12); ARK_CHECK(inner.finish(3,15)==3); }
            ARK_CHECK(middle.finish(4,19)==4);
        }
        ARK_CHECK(outer.finish(2,19)==2); // Three levels remove inclusive child deltas once.
    }
    {
        CostMutationScope outer(&controller,0,true,10);
        { CostMutationScope other(&other_controller,0,true,1); ARK_CHECK(other.finish(3,4)==3); }
        { CostMutationScope side(&controller,1,true,1); ARK_CHECK(side.finish(3,4)==3); }
        ARK_CHECK(outer.finish(2,12)==2); // Controller and player side form separate balances.
    }
    {
        CostMutationScope outer(&controller,0,true,98);
        { CostMutationScope inner(&controller,0,true,98); ARK_CHECK(inner.finish(10,99)==1); }
        ARK_CHECK(outer.finish(10,99)==0); // The cap applies before recursive attribution.
    }
    {
        CostMutationScope outer(&controller,0,true,10);
        { CostMutationScope failed(&controller,0,true,10); ARK_CHECK(failed.finish(3,-1)==0); }
        ARK_CHECK(outer.finish(3,13)==0); // Unreadable nested balances cannot be guessed.
    }
    ARK_CHECK(!CostMutationScope::observing(&controller,0));
}

// Covers replay, duplicate suppression, buildup isolation, bounded history and battle reset.
void test_history_replay_and_bounds() {
    EventHistory history(4);
    ArkEventV1 event = make_event(ARK_BATTLE_START);
    history.publish(event);
    event.kind = ARK_DAMAGE;
    event.damage_type = ARK_DAMAGE_PHYSICAL;
    event.calculated_damage = 300;
    event.actual_damage = 100;
    event.game_time = 2;
    set_id(event.actor_id, "A");
    history.publish(event);
    event.kind = ARK_CLOCK;
    event.game_time = 4;
    history.publish(event);

    ArkEventV1 out[8];
    ArkReadStatusV1 status;
    std::size_t count = history.read(0, out, 8, &status);
    ARK_CHECK(count == 3 && !status.partial);

    BattleStats model;
    consume_all(model, out, count);
    ARK_CHECK(model.total(true) == 100 && model.total(false) == 300 && model.duration() == 4);

    // Consuming the same events again is ignored.
    consume_all(model, out, count);
    ARK_CHECK(model.total(false) == 300);

    // Replaying into a newly loaded plugin recovers exactly the same totals.
    BattleStats reloaded;
    consume_all(reloaded, out, count);
    ARK_CHECK(reloaded.total(true) == model.total(true));

    // Elemental buildup never counts as damage.
    event.kind = ARK_ELEMENTAL;
    event.calculated_damage = 20;
    history.publish(event);
    count = history.read(model.sequence, out, 8, &status);
    consume_all(model, out, count);
    ARK_CHECK(model.total(false) == 300 && model.rows["A"].elemental_buildup == 20);

    // Exceeding the capacity drops the oldest events and reports the gap.
    event.kind = ARK_CLOCK;
    history.publish(event);
    history.publish(event);
    count = history.read(1, out, 8, &status);
    ARK_CHECK(status.partial && status.overrun && count == 4);

    // A new battle discards the previous one and clears the partial state.
    event = {};
    event.kind = ARK_BATTLE_START;
    history.publish(event);
    count = history.read(1, out, 8, &status);
    ARK_CHECK(count == 1 && !status.overrun && !status.partial);
}

void test_bounded_reader_after_history_overrun() {
    EventHistory history(1500);
    history.publish(make_event(ARK_BATTLE_START));
    auto event=make_event(ARK_DAMAGE);
    event.damage_type=ARK_DAMAGE_PHYSICAL;
    event.actual_damage=event.calculated_damage=1;
    set_id(event.actor_id,"A");
    for(int i=0;i<2000;++i) history.publish(event);
    BattleStats stats;
    std::uint64_t cursor=1;
    unsigned read_calls=0;
    const auto read=[&](auto after,auto* out,auto capacity,auto* status) {
        ++read_calls;
        return history.read(after,out,capacity,status);
    };
    using ark::arkhelper::ipc_publish::read_statistics_events;
    ARK_CHECK(read_statistics_events(read,stats,cursor)==1024);
    ARK_CHECK(read_calls==4 && cursor==1525 && stats.partial);
    ARK_CHECK(stats.total(true)==1024);
    read_calls=0;
    ARK_CHECK(read_statistics_events(read,stats,cursor)==476);
    ARK_CHECK(read_calls==2 && cursor==2001 && stats.total(true)==1500 && stats.partial);
    ARK_CHECK(read_statistics_events(read,stats,cursor)==0 && stats.total(true)==1500);
    BattleStats joined;
    std::uint64_t joined_cursor=0;
    // First-time readers see status.partial even though overrun needs a nonzero cursor.
    ARK_CHECK(read_statistics_events(read,joined,joined_cursor)==1024 && joined.partial);
    ARK_CHECK(read_statistics_events(read,joined,joined_cursor)==476 && joined.total(true)==1500);
    history.publish(make_event(ARK_BATTLE_START));
    ARK_CHECK(read_statistics_events(read,stats,cursor)==1);
    ARK_CHECK(stats.rows.empty() && !stats.partial && cursor==2002);
}

void test_damage_types() {
    EventHistory history(64);
    const ArkEventV1 start = make_event(ARK_BATTLE_START);
    history.publish(start);
    const auto emit = [&](const char* actor, std::int32_t type, double calculated, double actual,
                          std::uint32_t kind = ARK_DAMAGE) {
        ArkEventV1 event = make_event(kind);
        event.damage_type = type;
        event.calculated_damage = calculated;
        event.actual_damage = actual;
        event.game_time = 2;
        set_id(event.actor_id, actor);
        history.publish(event);
    };
    emit("A", ARK_DAMAGE_PHYSICAL, 120, 100);
    emit("A", ARK_DAMAGE_MAGICAL, 80, 60);
    emit("A", ARK_DAMAGE_TRUE, 50, 40);
    emit("A", ARK_DAMAGE_ELEMENTAL, 30, 20);
    emit("A", ARK_DAMAGE_PHYSICAL, 12, 10);
    emit("B", ARK_DAMAGE_MAGICAL, 400, 150);
    emit("A", ARK_DAMAGE_ELEMENTAL, 999, 777, ARK_ELEMENTAL);
    // Untracked damage types are dropped.
    emit("A", ARK_DAMAGE_HEAL, 1000, 1000);
    emit("A", ARK_DAMAGE_NONE, 1000, 1000);
    emit("A", 99, 1000, 1000);

    ArkEventV1 events[64];
    ArkReadStatusV1 status;
    std::size_t count = history.read(0, events, 64, &status);
    BattleStats model;
    consume_all(model, events, count);
    ARK_CHECK(model.rows.size() == 2 && model.total(true) == 380 && model.total(false) == 692);

    const ActorRow& a = model.rows.at("A");
    ARK_CHECK(a.actual == 230 && a.calculated == 292 && a.elemental_buildup == 999);
    ARK_CHECK(a.types[0].actual == 110 && a.types[0].calculated == 132);
    ARK_CHECK(a.types[1].actual == 60 && a.types[1].calculated == 80);
    ARK_CHECK(a.types[2].actual == 40 && a.types[2].calculated == 50);
    ARK_CHECK(a.types[3].actual == 20 && a.types[3].calculated == 30);
    double actual = 0;
    double calculated = 0;
    for (const auto& type : a.types) {
        actual += type.actual;
        calculated += type.calculated;
    }
    ARK_CHECK(actual == a.actual && calculated == a.calculated);
    ARK_CHECK(model.rows.at("B").types[1].actual == 150 && model.rows.at("B").types[3].actual == 0);

    consume_all(model, events, count);
    ARK_CHECK(model.rows.at("A").types[3].actual == 20 &&
              model.rows.at("A").elemental_buildup == 999);

    BattleStats replay;
    consume_all(replay, events, count);
    ARK_CHECK(replay.rows.at("A").types[3].calculated == 30 && replay.total(true) == 380);

    std::vector<const ActorRow*> order;
    model.sort_rows(order, true);
    ARK_CHECK(order.front()->id == "A");
    model.sort_rows(order, false);
    ARK_CHECK(order.front()->id == "B");

    history.publish(start);
    emit("A", ARK_DAMAGE_ELEMENTAL, 5, 4);
    count = history.read(model.sequence, events, 64, &status);
    consume_all(model, events, count);
    ARK_CHECK(model.rows.size() == 1 && model.total(true) == 4 && model.total(false) == 5);
    ARK_CHECK(model.rows.at("A").types[0].actual == 0 && model.rows.at("A").types[3].actual == 4 &&
              model.rows.at("A").elemental_buildup == 0);
}

void test_support_and_unit_isolation() {
    EventHistory history(64);
    history.publish(make_event(ARK_BATTLE_START));
    auto event = make_event(ARK_CAPABILITIES);
    event.damage_type = (1u << ARK_HEAL) | (1u << ARK_COST) | (1u << ARK_TAKEN);
    history.publish(event);
    const auto emit = [&](std::uint32_t kind, const char* id, double value, int type = 0) {
        auto e = make_event(kind); set_id(e.actor_id,id);
        e.actual_damage=e.calculated_damage=value; e.damage_type=type; e.game_time=10;
        history.publish(e);
    };
    emit(ARK_DAMAGE,"mixed",100,ARK_DAMAGE_PHYSICAL);
    emit(ARK_DAMAGE,"mixed",50,ARK_DAMAGE_MAGICAL);
    emit(ARK_HEAL,"mixed",900);
    emit(ARK_COST,"mixed",14);
    emit(ARK_TAKEN,"mixed",350);
    emit(ARK_ELEMENTAL,"mixed",60,ARK_DAMAGE_ELEMENTAL);
    emit(ARK_COST,"",100); // Natural regeneration has no source and is never assigned to an operator.
    emit(ARK_COST,"mixed",-10); // Spent points do not become generated points.
    emit(ARK_HEAL,"mixed",0); // Overheal contributes no effective healing.
    emit(ARK_COST,"support",28);
    ArkEventV1 events[64]; ArkReadStatusV1 status;
    auto count=history.read(0,events,64,&status);
    BattleStats stats; consume_all(stats,events,count);
    ARK_CHECK(stats.total(true)==150 && stats.rows.size()==2);
    const auto& mixed=stats.rows.at("mixed");
    ARK_CHECK(mixed.healing==900 && mixed.deployment_points==14 && mixed.taken==350);
    ARK_CHECK(mixed.actual==150 && mixed.elemental_buildup==60);
    ARK_CHECK(stats.rows.at("support").actual==0 && stats.rows.at("support").deployment_points==28);
    ARK_CHECK(stats.capabilities & (1u<<ARK_COST));
    consume_all(stats,events,count);
    ARK_CHECK(stats.rows.at("mixed").deployment_points==14);
    history.publish(make_event(ARK_BATTLE_START));
    count=history.read(stats.sequence,events,64,&status); consume_all(stats,events,count);
    ARK_CHECK(stats.rows.empty() && stats.total(true)==0);
}

void test_column_identity_and_format() {
    using namespace arkhelp;
    const auto order=statistics::parse_order(L"heal,total,heal,invalid,cost");
    ARK_CHECK(order.size()==ipc::kStatCount);
    ARK_CHECK(order[0]==ipc::kStatHeal && order[1]==ipc::kStatDamage && order[2]==ipc::kStatCost);
    auto sorted=order; std::sort(sorted.begin(),sorted.end());
    for(std::size_t i=0;i<sorted.size();++i) ARK_CHECK(sorted[i]==static_cast<int>(i));
    ARK_CHECK(statistics::parse_order(statistics::order_string(order))==order);
    ARK_CHECK(statistics::format(.125,ipc::kStatShare)==L"12.5%");
    ARK_CHECK(statistics::format(42,ipc::kStatCost)==L"42");
    ARK_CHECK(statistics::format(12345,ipc::kStatDamage,true)==L"12345");
    ipc::DamageEntry row;
    row.metrics[ipc::kStatDamage]=150;
    row.metrics[ipc::kStatHeal]=900;
    ARK_CHECK(statistics::value(row,ipc::kStatDamage)==150);
}

void test_battle_frame_snapshot() {
    EventHistory history(2);
    ArkBattleFrameV1 frame;

    // No frame before the first battle.
    history.set_frame(7);
    ARK_CHECK(!history.frame(frame));

    ArkEventV1 event = make_event(ARK_BATTLE_START);
    history.publish(event);
    history.set_frame(0);
    ARK_CHECK(history.frame(frame) && frame.frame == 0 && frame.battle == 1);

    // The snapshot survives history eviction.
    history.set_frame(90);
    event.kind = ARK_CLOCK;
    for (int i = 0; i < 3; ++i) {
        history.publish(event);
    }
    ARK_CHECK(history.frame(frame) && frame.frame == 90 && frame.battle == 1);

    // The final frame is frozen once the battle ends.
    event.kind = ARK_BATTLE_END;
    history.publish(event);
    history.set_frame(91);
    ARK_CHECK(history.frame(frame) && frame.frame == 90);

    event.kind = ARK_BATTLE_START;
    history.publish(event);
    ARK_CHECK(!history.frame(frame));
    history.set_frame(0);
    ARK_CHECK(history.frame(frame) && frame.frame == 0 && frame.battle == 2);
}

void test_stage_names() {
    StageNameIndex names;
    names.add("a001_03", "Activities/A001/level_a001_03", "GT-3", "意外之旅");
    names.add("act3d0_01", "Activities/ACT3D0/level_act3d0_01", "OF-1", "火山制造");
    ARK_CHECK(names.resolve("level_a001_03") == "GT-3 意外之旅");
    ARK_CHECK(names.resolve("Activities/A001/level_a001_03") == "GT-3 意外之旅");
    ARK_CHECK(names.resolve("a001_03") == "GT-3 意外之旅");
    ARK_CHECK(names.resolve("ACTIVITIES\\A001\\LEVEL_A001_03.JSON") == "GT-3 意外之旅");
    ARK_CHECK(names.resolve("LEVEL_A001_03.Json") == "GT-3 意外之旅");
    ARK_CHECK(names.resolve("A001_03.json") == "GT-3 意外之旅");
    ARK_CHECK(names.resolve("Activities/ACT3D0/level_act3d0_01") == "OF-1 火山制造");

    const auto rogue = names.add("ro3_n_01", "Roguelike/RO3/level_ro3_n_01", "ISW-NO", "与虫为伴");
    names.add_alias(rogue, "Roguelike/RO3/level_ro3_n_01_replace");
    names.add_alias(rogue, "Roguelike/RO3/level_ro3_n_01_hard.json");
    ARK_CHECK(names.resolve("ro3_n_01") == "ISW-NO 与虫为伴");
    ARK_CHECK(names.resolve("level_ro3_n_01") == "ISW-NO 与虫为伴");
    ARK_CHECK(names.resolve("ROGUELIKE\\RO3\\LEVEL_RO3_N_01_REPLACE.JSON") == "ISW-NO 与虫为伴");
    ARK_CHECK(names.resolve("level_ro3_n_01_replace") == "ISW-NO 与虫为伴");
    ARK_CHECK(names.resolve("level_ro3_n_01_hard") == "ISW-NO 与虫为伴");
    ARK_CHECK(names.resolve("Roguelike/RO3/level_ro3_n_01_hard") == "ISW-NO 与虫为伴");

    ARK_CHECK(names.resolve("level_unknown").empty());
    ARK_CHECK(names.resolve("unrecognized/path/level_unknown.json").empty());
    ARK_CHECK(names.resolve("").empty());
    ARK_CHECK(names.resolve("level_a001_03.json.bak").empty());

    names.add("code_only", "levels/code_only", "S-9", "");
    names.add("name_only", "levels/name_only", "", "无名之地");
    names.add("unnamed", "levels/unnamed", "", "");
    ARK_CHECK(names.resolve("code_only") == "S-9");
    ARK_CHECK(names.resolve("name_only") == "无名之地");
    ARK_CHECK(names.resolve("unnamed").empty());

    // An earlier entry handle still identifies its native name after more rows arrive.
    names.add_alias(rogue, "Roguelike/RO3/level_ro3_late_replace");
    ARK_CHECK(names.resolve("level_ro3_late_replace") == "ISW-NO 与虫为伴");

    // The panel shows the resolved label, falling back to the raw stage ID.
    BattleStats model;
    ArkEventV1 start = make_event(ARK_BATTLE_START);
    for (const char* stage :
         {"level_a001_03", "ro3_n_01", "Unrecognized/level_unknown.json", "a001_03"}) {
        set_id(start.stage_id, stage);
        model.consume(start);
        const std::string_view native = names.resolve(model.stage);
        const std::string visible(native.empty() ? std::string_view(model.stage) : native);
        if (model.stage == "Unrecognized/level_unknown.json") {
            ARK_CHECK(visible == "Unrecognized/level_unknown.json");
        } else if (model.stage == "ro3_n_01") {
            ARK_CHECK(visible == "ISW-NO 与虫为伴");
        } else {
            ARK_CHECK(visible == "GT-3 意外之旅");
        }
    }
    ARK_CHECK(names.resolve("act3d0_01") == "OF-1 火山制造");
}

void test_ambiguous_basenames() {
    for (bool reverse : {false, true}) {
        StageNameIndex names;
        const auto add_first = [&] {
            return names.add("stage_first", "Episodes/First/level_shared", "A-1", "甲");
        };
        const auto add_second = [&] {
            return names.add("stage_second", "Episodes/Second/level_shared", "B-2", "乙");
        };
        std::size_t first_entry;
        if (reverse) {
            add_second();
            first_entry = add_first();
        } else {
            first_entry = add_first();
            add_second();
        }
        ARK_CHECK(names.resolve("level_shared").empty());
        ARK_CHECK(names.resolve("LEVEL_SHARED.JSON").empty());
        ARK_CHECK(names.resolve("Unknown/level_shared").empty());
        ARK_CHECK(names.resolve("Episodes/First/level_shared") == "A-1 甲");
        ARK_CHECK(names.resolve("EPISODES\\SECOND\\LEVEL_SHARED.JSON") == "B-2 乙");
        ARK_CHECK(names.resolve("stage_first") == "A-1 甲");
        ARK_CHECK(names.resolve("stage_second") == "B-2 乙");

        // Re-adding an alias does not lift the ambiguity.
        names.add_alias(first_entry, "Episodes/First/level_shared.json");
        ARK_CHECK(names.resolve("level_shared").empty());

        // An exact stage ID wins over an ambiguous basename.
        names.add("level_shared", "Episodes/Third/level_unique", "C-3", "丙");
        ARK_CHECK(names.resolve("level_shared") == "C-3 丙");
        ARK_CHECK(names.resolve("Episodes/First/level_shared") == "A-1 甲");
        ARK_CHECK(names.resolve("Episodes/Second/level_shared") == "B-2 乙");
    }
}

void test_duplicate_labels_stay_unambiguous() {
    StageNameIndex names;
    const auto original = names.add("same_first", "First/level_same", "D-4", "同名");
    names.add_alias(original, "FIRST\\LEVEL_SAME.JSON");
    names.add("same_second", "Second/level_same", "D-4", "同名");
    ARK_CHECK(names.resolve("level_same") == "D-4 同名");
    ARK_CHECK(names.resolve("FIRST/level_same") == "D-4 同名");
    ARK_CHECK(names.resolve("Second/LEVEL_SAME.json") == "D-4 同名");
    ARK_CHECK(names.resolve("same_first") == "D-4 同名");
    ARK_CHECK(names.resolve("same_second") == "D-4 同名");

    const auto replacement = names.add("different", "Third/level_other", "E-5", "异名");
    names.add_alias(replacement, "Third/level_same");
    ARK_CHECK(names.resolve("level_same").empty());
    ARK_CHECK(names.resolve("First/level_same") == "D-4 同名");
    ARK_CHECK(names.resolve("Third/level_same") == "E-5 异名");
}

void test_unqualified_level_collision() {
    StageNameIndex names;
    names.add("qualified", "Episodes/First/level_collision", "A-1", "甲");
    names.add("unqualified", "level_collision", "B-2", "乙");
    ARK_CHECK(names.resolve("level_collision").empty());
    ARK_CHECK(names.resolve("Episodes/First/level_collision") == "A-1 甲");
    ARK_CHECK(names.resolve("unqualified") == "B-2 乙");
}

struct TestCase {
    const char* name;
    void (*run)();
};

constexpr TestCase kTests[] = {
    {"cost mutation forwards ordinary bool ABI", test_cost_boolean_abi},
    {"effective cost attribution and cap", test_effective_cost_attribution},
    {"recursive cost attribution excludes nested changes", test_nested_cost_attribution},
    {"history replay and bounds", test_history_replay_and_bounds},
    {"bounded reader preserves retained events after overrun", test_bounded_reader_after_history_overrun},
    {"damage types", test_damage_types},
    {"support contributions and unit isolation", test_support_and_unit_isolation},
    {"column identity and value formatting", test_column_identity_and_format},
    {"battle frame snapshot", test_battle_frame_snapshot},
    {"stage names", test_stage_names},
    {"ambiguous basenames", test_ambiguous_basenames},
    {"duplicate labels stay unambiguous", test_duplicate_labels_stay_unambiguous},
    {"unqualified level collision", test_unqualified_level_collision},
};

} // namespace

int wmain() {
    for (const TestCase& test : kTests) {
        try {
            test.run();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL %s: %s\n", test.name, error.what());
            return 1;
        }
        std::printf("PASS %s\n", test.name);
    }
    return 0;
}
