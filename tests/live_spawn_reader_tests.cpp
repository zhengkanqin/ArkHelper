#include "../native/modules/provider/capture/live_spawns.h"
#include "../native/modules/provider/ipc/spawn_selection.h"
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>

namespace {
namespace live = ark::arkhelper::capture::live;
struct Class;
struct Field { const char* name; Class* type = nullptr; int value = 0; };
struct Method {
    void* native = this;
    const char* name;
    unsigned arity;
    std::function<void*(void*,void**)> call;
};
struct Class {
    const char* name;
    const char* space = "Torappu.Battle";
    int bytes = 0;
    std::vector<Field*> fields;
    std::vector<Method*> methods;
};
struct Object {
    Class* klass;
    void* payload = nullptr;
    std::vector<std::pair<Field*,void*>> fields;
    std::vector<void*> items;
    int length_override = -1;
};
template<class T> struct Box : Object {
    T value;
    Box(Class& klass, T v):Object{&klass},value(v) { payload=&value; }
};
Class integer{"Int32","System",4}, real{"Single","System",4}, fp{"FP","Torappu",8};
Class boolean{"Boolean","System",1}, string{"String","System"}, kind{"ActionType","Torappu",4};
Class controller_type{"BattleController"}, scheduler_type{"Scheduler"}, list_type{"List`1","System.Collections.Generic"};
Class item_type{"ActionItem","Torappu.Battle",16}, data_type{"ActionData","Torappu"};
Class wave_type{"WaveData"}, fragment_type{"FragmentData"};
Field scheduler_field{"_scheduler"}, queue_field{"m_actionQueue"}, base_field{"m_fragmentStartTime"};
Field waves_field{"m_waves"}, wave_index_field{"m_spawnedWavesCnt"}, clock_field{"userFixedPlayTime"};
Field fragments_field{"fragments"}, actions_field{"actions"}, delay_field{"preDelay"}, managed_field{"managedByScheduler"};
Field interval_field{"interval"};
Field version_field{"_version"}, data_field{"data"}, offset_field{"timeOffset"};
Field kind_field{"actionType"}, key_field{"key"}, route_field{"routeIndex"};
Field valid_field{"isValid"}, extra_field{"useExtraRoute"}, count_field{"count"};
Field spawn_kind{"SPAWN",&kind,0}, preview_kind{"DISPLAY_ENEMY_INFO",&kind,1};
unsigned checks=0, item_reads=0;
void check(bool value,const char* message) { ++checks; if(!value) throw std::runtime_error(message); }

void configure(ark::il2cpp::Runtime& rt) {
    rt.object_class=[](void* o)->void* {return static_cast<Object*>(o)->klass;};
    rt.object_unbox=[](void* o)->void* {return static_cast<Object*>(o)->payload;};
    rt.class_name=[](void* k){return static_cast<Class*>(k)->name;};
    rt.class_namespace=[](void* k){return static_cast<Class*>(k)->space;};
    rt.class_parent=[](void*)->void* {return nullptr;};
    rt.class_is_value=[](void* k){return static_cast<Class*>(k)->bytes>0;};
    rt.class_value_size=[](void* k,std::uint32_t* a){*a=4;return static_cast<Class*>(k)->bytes;};
    rt.class_rank=[](void*){return 0;};
    rt.class_from_type=[](void* t)->void*{return t;};
    rt.class_field=[](void* k,const char* name)->void* {
        for(auto* f:static_cast<Class*>(k)->fields) if(!std::strcmp(f->name,name)) return f;
        return nullptr;
    };
    rt.field_get_object=[](void* f,void* o)->void* {
        for(auto& pair:static_cast<Object*>(o)->fields) if(pair.first==f) return pair.second;
        return nullptr;
    };
    rt.class_fields=[](void* k,void** it)->void* {
        auto& fields=static_cast<Class*>(k)->fields;
        auto i=reinterpret_cast<std::uintptr_t>(*it);
        if(i>=fields.size()) return nullptr;
        *it=reinterpret_cast<void*>(i+1);return fields[i];
    };
    rt.field_type=[](void* f)->void*{return static_cast<Field*>(f)->type;};
    rt.field_name=[](void* f){return static_cast<Field*>(f)->name;};
    rt.field_static_get=[](void* f,void* p){*static_cast<int*>(p)=static_cast<Field*>(f)->value;};
    rt.class_methods=[](void* k,void** it)->void* {
        auto& methods=static_cast<Class*>(k)->methods;
        auto i=reinterpret_cast<std::uintptr_t>(*it);
        if(i>=methods.size()) return nullptr;
        *it=reinterpret_cast<void*>(i+1);return methods[i];
    };
    rt.method_name=[](void* m){return static_cast<Method*>(m)->name;};
    rt.method_count=[](void* m)->std::uint32_t{return static_cast<Method*>(m)->arity;};
    rt.method_class=[](void*)->void*{return &list_type;};
    rt.method_flags=[](void*,std::uint32_t* flags)->std::uint32_t{*flags=0;return 0;};
    rt.method_param=[](void*,std::uint32_t)->void*{return &integer;};
    rt.type_name=[](void*)->char*{return _strdup("System.Int32");};
    rt.free_memory=[](void* p){std::free(p);};
    rt.thread_current=[]()->void*{return &controller_type;};
    rt.invoke_method=[](void* m,void* o,void** args,void** error)->void* {
        *error=nullptr;return static_cast<Method*>(m)->call(o,args);
    };
    rt.string_length=[](void* o){return int(std::wcslen(static_cast<wchar_t*>(static_cast<Object*>(o)->payload)));};
    rt.string_chars=[](void* o)->const wchar_t*{return static_cast<wchar_t*>(static_cast<Object*>(o)->payload);};
}
}

int main() {
    try {
        ark::il2cpp::Runtime rt; configure(rt);
        controller_type.fields={&scheduler_field,&clock_field};
        scheduler_type.fields={&queue_field,&base_field,&waves_field,&wave_index_field};
        wave_type.fields={&fragments_field};fragment_type.fields={&actions_field,&delay_field};
        list_type.fields={&version_field}; item_type.fields={&data_field,&offset_field};
        data_type.fields={&kind_field,&key_field,&route_field,&valid_field,&extra_field,&count_field,&delay_field,&managed_field,&interval_field};
        kind.fields={&spawn_kind,&preview_kind};
        Box<int> version{integer,1}, length{integer,0}, type{kind,0}, preview{kind,1}, route{integer,2}, count{integer,6};
        Box<int> wave_index{integer,0}, next_route{integer,1}, last_route{integer,3}, amount{integer,2};
        Box<float> delay{real,30}, next_delay{real,5}, zero{real,0}, interval{real,20};
        Box<std::int64_t> clock{fp,130ll*4294967296ll};
        Box<bool> valid{boolean,true}, extra{boolean,false};
        Box<std::int64_t> base{fp,std::int64_t(124.59988117218018*4294967296.0)};
        Object key{&string,const_cast<wchar_t*>(L"enemy_test")};
        Object data{&data_type}, info{&data_type}, controller{&controller_type}, scheduler{&scheduler_type}, queue{&list_type};
        Object waves{&list_type}, fragments{&list_type}, next_fragments{&list_type};
        Object old_actions{&list_type}, actions{&list_type}, future_actions{&list_type}, last_actions{&list_type};
        Object old_fragment{&fragment_type}, fragment{&fragment_type}, future_fragment{&fragment_type}, last_fragment{&fragment_type};
        Object wave{&wave_type}, next_wave{&wave_type}, future{&data_type}, last{&data_type}, old{&data_type};
        controller.fields={{&scheduler_field,&scheduler},{&clock_field,&clock}};
        scheduler.fields={{&queue_field,&queue},{&base_field,&base},{&waves_field,&waves},{&wave_index_field,&wave_index}};
        queue.fields={{&version_field,&version}};
        data.fields={{&kind_field,&type},{&key_field,&key},{&route_field,&route},
            {&valid_field,&valid},{&extra_field,&extra},{&count_field,&count},
            {&delay_field,&next_delay},{&managed_field,&valid},{&interval_field,&interval}};
        future.fields=data.fields; future.fields[2].second=&next_route; future.fields[5].second=&amount;
        last.fields=future.fields;last.fields[2].second=&last_route;
        old.fields=data.fields;
        old_fragment.fields={{&actions_field,&old_actions},{&delay_field,&zero}};
        fragment.fields={{&actions_field,&actions},{&delay_field,&delay}};
        future_fragment.fields={{&actions_field,&future_actions},{&delay_field,&zero}};
        last_fragment.fields={{&actions_field,&last_actions},{&delay_field,&zero}};
        old_actions.items={&old};actions.items={&data};future_actions.items={&future};last_actions.items={&last};
        fragments.items={&old_fragment,&fragment,&future_fragment};next_fragments.items={&last_fragment};
        wave.fields={{&fragments_field,&fragments}};next_wave.fields={{&fragments_field,&next_fragments}};
        waves.items={&wave,&next_wave};
        info.fields={{&kind_field,&preview}};
        std::vector<std::unique_ptr<Box<float>>> offsets;
        std::vector<std::unique_ptr<Object>> items;
        const auto add=[&](float seconds, Object* action) {
            auto offset=std::make_unique<Box<float>>(real,seconds);
            auto item=std::make_unique<Object>(Object{&item_type});
            item->fields={{&data_field,action},{&offset_field,offset.get()}};
            queue.items.push_back(item.get());
            offsets.push_back(std::move(offset));items.push_back(std::move(item));
        };
        Method get_count{nullptr,"get_Count",0,[&](void* o,void**)->void*{
            auto& seq=*static_cast<Object*>(o);length.value=seq.length_override>=0?seq.length_override:int(seq.items.size());return &length;}};
        Method get_item{nullptr,"get_Item",1,[&](void* o,void** args)->void*{
            ++item_reads;return static_cast<Object*>(o)->items.at(*static_cast<int*>(args[0]));}};
        get_count.native=&get_count;get_item.native=&get_item;
        list_type.methods={&get_count,&get_item};
        add(0,&info);
        for(int i=5;i>=0;--i) add(40.f+i*5,&data);
        live::SpawnReader reader;
        auto result=reader.read(rt,&controller,3,1000,4200);
        check(result.available && result.spawns.size()==10,"live queue or later entrances missing / count multiplied twice");
        check(std::abs(result.spawns[0].time-134.59988117218018)<.00001,"fragment predelay counted twice");
        check(!result.spawns[0].pending && result.spawns[6].pending && result.spawns[7].pending,
            "later fragment/wave needs a pending preview instead of a fabricated countdown");
        auto frames=live::spawn_frames(result);
        check(frames[0].frame==4338,"scheduler user clock was confused with native frame time");
        const auto reads=item_reads, revision=result.revision;
        result=reader.read(rt,&controller,3,1250,4200);
        check(item_reads==reads && result.sampled_ms==1250 && result.revision==revision,"paused unchanged queue reparsed or stopped refreshing");
        version.value++;add(70,&data);
        result=reader.read(rt,&controller,3,1500,4200);
        check(result.spawns.size()==11 && result.revision>revision,"changed action queue not refreshed");
        base.value=std::int64_t(200)*4294967296ll;
        result=reader.read(rt,&controller,3,1750,4200);
        check(result.spawns[0].time==210,"new fragment retained old countdown");
        clock.value=200ll*4294967296ll;
        result=reader.read(rt,&controller,3,1900,6600);
        check(live::spawn_frames(result)[0].frame==6900,"cached schedule did not update its clock mapping");
        const auto saved_items=queue.items;
        queue.items.clear();version.value++;add(5,&future);
        result=reader.read(rt,&controller,3,1950,6600);
        check(result.spawns.size()==3 && !result.spawns[0].pending && result.spawns[0].route_index==1 &&
            result.spawns[1].route_index==3,"fragment switch resurrected consumed gate previews");
        queue.items=saved_items;version.value++;
        result=reader.read(rt,&controller,4,2000,6600);
        check(result.serial==4 && item_reads>reads+7,"same level restart reused old serial");
        queue.length_override=0; version.value++;
        result=reader.read(rt,&controller,4,2250,6600);
        check(result.available && result.spawns.size()==4 && result.spawns[0].pending,
            "empty transition queue erased future entrances or resurrected consumed fragments");
        wave_index.value=2;
        result=reader.read(rt,&controller,4,2300,6600);
        check(result.available && result.spawns.empty(),"finished waves reappeared as pending");
        scheduler.fields.clear();
        result=reader.read(rt,&controller,4,2500,6600);
        check(!result.available && result.spawns.empty(),"unavailable scheduler leaked old wave");
        scheduler.fields={{&queue_field,&queue},{&base_field,&base},{&waves_field,&waves},{&wave_index_field,&wave_index}};
        queue.length_override=9000;
        check(!reader.read(rt,&controller,4,2750,6600).available,"oversized queue not bounded");
        queue.length_override=-1;wave_index.value=0;extra.value=true;version.value++;
        check(reader.read(rt,&controller,4,3000,6600).spawns.empty(),"extra route guessed as regular red gate");
        extra.value=false;valid.value=false;version.value++;
        check(reader.read(rt,&controller,4,3250,6600).spawns.empty(),"disabled actions leaked pending previews");

        ark::arkhelper::Stage stage;stage.routes.resize(4);
        stage.routes[1].start_col=1;stage.routes[2].start_col=2;stage.routes[3]=stage.routes[0];
        std::vector<ark::arkhelper::Spawn> many;
        for(int i=0;i<80;++i) {ark::arkhelper::Spawn s;s.frame=100+i;s.route_index=i%2?0:3;many.push_back(s);}
        ark::arkhelper::Spawn pending;pending.pending=true;pending.phase=4096;pending.time=5;pending.route_index=1;
        many.push_back(pending);pending.route_index=2;many.push_back(pending);
        pending.phase=8192;many.push_back(pending);
        auto selected=ark::arkhelper::select_gate_spawns(stage,many,100,48);
        check(selected.size()==48 && selected[1].route_index==1 && selected[2].route_index==2,
            "one busy gate starved the other two entrances from the IPC budget");
        check(std::count_if(selected.begin(),selected.end(),[](const auto& s){return s.pending;})==2,
            "multiple future waves merged into one pending batch");
        selected=ark::arkhelper::select_gate_spawns(stage,many,160,48);
        check(std::none_of(selected.begin(),selected.end(),[](const auto& s){return !s.pending && s.frame<160;}),
            "consumed entries survived a native-frame advance");
        ark::arkhelper::Stage pending_stage;pending_stage.routes.resize(4);
        pending_stage.routes[1].start_col=1;pending_stage.routes[3].start_col=2;
        valid.value=true;queue.length_override=0;version.value++;
        result=reader.read(rt,&controller,5,3500,0);
        check(result.spawns.size()==16 && result.spawns[0].pending,
            "opening empty queue erased all entrance previews");
        selected=ark::arkhelper::select_gate_spawns(pending_stage,live::spawn_frames(result),0,48);
        check(std::count_if(selected.begin(),selected.end(),[](const auto& s){return s.route_index==1;})==1,
            "pending next batch included another enemy scheduled twenty seconds later");
        std::printf("PASS live spawn reader: %u checks\n",checks);return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
