// Exercise the production capture callbacks with a local IL2CPP fixture. No game or IPC writes.
#include <windows.h>
#include <string>
#include <stdexcept>
#include "../native/modules/damage_meter/event_history.h"
#define private public
#include "../native/modules/damage_meter/native_capture.h"
#undef private
#include "../native/modules/damage_meter/native_capture.cpp"
#include "../native/modules/provider/capture/operator_samples.h"
#include "../native/modules/provider/ipc/operator_publication.h"
#include <cstdio>

namespace {
struct Actor { const wchar_t* id; int uid; bool friendly; Actor* host = nullptr; bool token = false; };
struct Controller { int state = 2; };
struct Modifier { std::int64_t real; Actor* source; Actor* target; int type = ARK_DAMAGE_PHYSICAL; };
double clock_time = 0;
Controller controller;
int character_class;
unsigned checks = 0;
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
void replay(ark::damage_meter::EventHistory& history, ark::damage_meter::BattleStats& stats) {
    ArkEventV1 events[256];
    auto count = history.read(0, events, 256, nullptr);
    for (std::size_t i=0; i<count; ++i) stats.consume(events[i]);
}
}

int main() {
    try {
        using namespace ark::damage_meter;
        namespace cap = ark::arkhelper::capture;
        namespace ipc = arkhelp::ipc;
        EventHistory history;
        ArkPluginApiV1 host{};
        NativeCapture capture(history, host);
        capture.bindings_ = std::make_unique<NativeCapture::Bindings>();
        auto& b = *capture.bindings_;
        b.fixed_play_time = [](void*) -> std::int64_t { return static_cast<std::int64_t>(clock_time*4294967296.); };
        b.fixed_frame_count = [](void*) -> std::uint32_t { return static_cast<std::uint32_t>(clock_time*30); };
        b.field_value = [](void* object, void*, void* out) {
            if (object == &controller) *static_cast<int*>(out) = controller.state;
            else *static_cast<void**>(out) = &controller;
        };
        b.level_id = [](void*,void*) -> void* { return const_cast<wchar_t*>(L"autochess_fixture"); };
        b.string_length = [](void* value) { return static_cast<int>(std::wcslen(static_cast<wchar_t*>(value))); };
        b.string_chars = [](void* value) -> const wchar_t* { return static_cast<wchar_t*>(value); };
        b.character_id = [](void* actor, void*) -> void* { return const_cast<wchar_t*>(static_cast<Actor*>(actor)->id); };
        b.instance_uid = [](void* actor,void*) -> std::uint32_t { return static_cast<Actor*>(actor)->uid; };
        b.fetch_host = [](void* actor,void*) -> void* { return static_cast<Actor*>(actor)->host; };
        b.is_token = [](void* actor,void*) { return static_cast<Actor*>(actor)->token; };
        b.object_class = [](void* actor) -> void* { return static_cast<Actor*>(actor)->friendly ? &character_class : nullptr; };
        b.assignable = [](void* klass,void* candidate) { return klass == candidate; };
        b.is_cancelled = [](void*,void*) { return false; };
        b.is_heal = [](void* mod,void*) { return static_cast<Modifier*>(mod)->type == ARK_DAMAGE_HEAL; };
        b.is_damage = [](void* mod,void*) { return static_cast<Modifier*>(mod)->type != ARK_DAMAGE_HEAL; };
        b.source = [](void* mod,void*) -> void* { return static_cast<Modifier*>(mod)->source; };
        b.target = [](void* mod,void*) -> void* { return static_cast<Modifier*>(mod)->target; };
        b.final_delta = [](void* mod,void*) { return static_cast<Modifier*>(mod)->real; };
        b.damage_type = [](void* mod,void*) { return static_cast<Modifier*>(mod)->type; };
        capture.character_class_ = &character_class;
        Actor first{L"char_219_meteo",631,true}, second{L"char_219_meteo",632,true};
        Actor enemy{L"enemy",900,false};
        Modifier damage{100LL<<32,&first,&enemy};
        capture.begin_battle(&controller,false);
        clock_time=10;
        capture.record_damage(&history,&damage,false);
        damage.source=&second; damage.real=60LL<<32;
        capture.record_damage(&history,&damage,false);
        BattleStats stats; replay(history,stats);
        check(stats.rows.size()==1 && stats.total(true)==160,"same-character report must total both instances exactly once");
        check(stats.instances.at(631).actual==100 && stats.instances.at(632).actual==60,"source UID must preserve separate contributions");
        replay(history,stats);
        check(stats.total(true)==160,"replayed events must not count twice");

        cap::OperatorSample ops[64]{}; std::uint32_t count=0;
        cap::operator_samples::append(ops,count,64,"char_219_meteo",.25f,.75f,true,631);
        cap::operator_samples::append(ops,count,64,"char_219_meteo",.75f,.25f,true,632);
        check(count==2 && ops[0].flags && ops[1].flags,"identical character IDs must retain both visible anchors");
        cap::operator_samples::append(ops,count,64,"char_219_meteo",.5f,.5f,true,631);
        check(count==2,"repeated scan of the same UID must not duplicate a label");
        ipc::BattleSnapshot snapshot{};
        snapshot.team_metrics[ipc::kStatDamage]=160;
        ark::arkhelper::ipc_publish::attach_operator_instances(snapshot,ops,count,stats,~0u,1000,500);
        check(snapshot.operator_count==2 && snapshot.operators[0].total_damage==100 && snapshot.operators[1].total_damage==60,"head labels must use instance values instead of copying the role aggregate");
        check(snapshot.operators[0].screen.x==250 && snapshot.operators[1].screen.x==750,"each instance must use its own anchor");
        check(snapshot.team_metrics[ipc::kStatDamage]==160,"head labels must never change team totals");

        controller.state=3;capture.update_battle(&controller);
        damage.real=500LL<<32;capture.record_damage(&history,&damage,false);
        replay(history,stats);
        check(stats.ended && stats.total(true)==160,"finished round must preserve totals and reject late damage");
        controller.state=2;clock_time=0; // No StartGame: actual AutoChess round transition.
        capture.record_damage(&history,&damage,false);
        replay(history,stats);
        check(!stats.ended && stats.total(true)==500 && stats.instances.size()==1,"first hit after an ended round must rearm without StartGame");
        check(!stats.partial,"observed round restart must be treated as a complete fresh round");
        clock_time=30;capture.update_battle(&controller);
        clock_time=0;damage.real=70LL<<32;damage.source=&first;
        capture.record_damage(&history,&damage,false);replay(history,stats);
        check(stats.total(true)==70 && !stats.instances.count(632),"clock rollback while Running must reset instance and aggregate values");
        clock_time=1;damage.type=ARK_DAMAGE_HEAL;damage.real=20LL<<32;
        capture.record_damage(&history,&damage,false);replay(history,stats);
        check(stats.instances.at(631).healing==20 && stats.total(true)==70,"healing must retain UID without becoming HP damage");
        damage.source=&enemy;damage.target=&second;damage.type=ARK_DAMAGE_PHYSICAL;damage.real=15LL<<32;
        capture.record_damage(&history,&damage,false);replay(history,stats);
        check(stats.instances.at(632).taken==15 && stats.total(true)==70,"damage taken must belong to target UID, not source UID");
        first.uid=700;damage.source=&first;damage.target=&enemy;
        cap::OperatorSample fresh{};fresh.instance_id=700;std::strcpy(fresh.actor_id,"char_219_meteo");fresh.flags=1;fresh.viewport_x=.5f;fresh.viewport_y=.5f;
        ark::arkhelper::ipc_publish::attach_operator_instances(snapshot,&fresh,1,stats,~0u,1000,500);
        check(snapshot.operator_count==1 && snapshot.operators[0].total_damage==0,"a newly deployed clone must not inherit another instance's damage");
        capture.begin_battle(&controller,false);
        Actor token{L"token_fixture",9001,true,&first,true};
        Actor derivative{L"trap_fixture",9002,true,&second,true};
        Actor device{L"trap_stage_device",9003,true};
        ark::OperatorIdentity device_identity;
        check(ark::resolve_operator(&device,device_identity,[](void*,auto& identity) {
            identity={"trap_stage_device",9003,false};return true;
        },[](void* object){return object;},true) && device_identity.instance==9003,
            "a non-token device with a self host must remain available to the optional display");
        damage={25LL<<32,&token,&enemy}; capture.record_damage(&history,&damage,false);
        damage.source=&derivative;damage.real=40LL<<32;capture.record_damage(&history,&damage,false);
        damage.source=&device;damage.real=1000LL<<32;capture.record_damage(&history,&damage,false);
        replay(history,stats);
        check(stats.rows.size()==2 && stats.total(true)==1065 && stats.rows.at("char_219_meteo").actual==65,
              "summons belong to operators while optional stage devices retain separate captured totals");
        check(stats.instances.at(first.uid).actual==25 && stats.instances.at(second.uid).actual==40,
              "two identical summoners must keep their own summons' contributions");
        check(!stats.instances.count(token.uid) && stats.instances.at(device.uid).actual==1000,"summons must not create separate rows, stage devices must remain available");
        damage.source=&token;damage.type=ARK_DAMAGE_HEAL;damage.real=10LL<<32;capture.record_damage(&history,&damage,false);
        damage.source=&enemy;damage.target=&token;damage.type=ARK_DAMAGE_PHYSICAL;damage.real=5LL<<32;capture.record_damage(&history,&damage,false);
        capture.record_cost(&controller,&token,6);
        replay(history,stats);
        check(stats.instances.at(first.uid).healing==10 && stats.instances.at(first.uid).taken==5 &&
              stats.instances.at(first.uid).deployment_points==6,"summon support metrics must follow the same exact owner");
        token.host=&token;damage.source=&token;damage.target=&enemy;damage.real=1000LL<<32;
        capture.record_damage(&history,&damage,false);replay(history,stats);
        check(stats.total(true)==1065,"cyclic or unresolved host links must not guess an operator");
        std::printf("AutoChess production capture: %u checks passed; IPC=%zu\n",checks,sizeof(snapshot));
    } catch(const std::exception& error) { std::fprintf(stderr,"FAIL %s\n",error.what());return 1; }
}
