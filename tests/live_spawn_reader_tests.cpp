#include "../native/modules/provider/capture/live_spawns.h"
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
};
template<class T> struct Box : Object {
    T value;
    Box(Class& klass, T v):Object{&klass},value(v) { payload=&value; }
};
Class integer{"Int32","System",4}, real{"Single","System",4}, fp{"FP","Torappu",8};
Class boolean{"Boolean","System",1}, string{"String","System"}, kind{"ActionType","Torappu",4};
Class controller_type{"BattleController"}, scheduler_type{"Scheduler"}, list_type{"List`1","System.Collections.Generic"};
Class item_type{"ActionItem","Torappu.Battle",16}, data_type{"ActionData","Torappu"};
Field scheduler_field{"_scheduler"}, queue_field{"m_actionQueue"}, base_field{"m_fragmentStartTime"};
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
        controller_type.fields={&scheduler_field}; scheduler_type.fields={&queue_field,&base_field};
        list_type.fields={&version_field}; item_type.fields={&data_field,&offset_field};
        data_type.fields={&kind_field,&key_field,&route_field,&valid_field,&extra_field,&count_field};
        kind.fields={&spawn_kind,&preview_kind};
        Box<int> version{integer,1}, length{integer,0}, type{kind,0}, preview{kind,1}, route{integer,2}, count{integer,6};
        Box<bool> valid{boolean,true}, extra{boolean,false};
        Box<std::int64_t> base{fp,std::int64_t(124.59988117218018*4294967296.0)};
        Object key{&string,const_cast<wchar_t*>(L"enemy_test")};
        Object data{&data_type}, info{&data_type}, controller{&controller_type}, scheduler{&scheduler_type}, queue{&list_type};
        controller.fields={{&scheduler_field,&scheduler}};
        scheduler.fields={{&queue_field,&queue},{&base_field,&base}};
        queue.fields={{&version_field,&version}};
        data.fields={{&kind_field,&type},{&key_field,&key},{&route_field,&route},
            {&valid_field,&valid},{&extra_field,&extra},{&count_field,&count}};
        info.fields={{&kind_field,&preview}};
        std::vector<std::unique_ptr<Box<float>>> offsets;
        std::vector<std::unique_ptr<Object>> items;
        const auto add=[&](float seconds, Object* action) {
            auto offset=std::make_unique<Box<float>>(real,seconds);
            auto item=std::make_unique<Object>(Object{&item_type});
            item->fields={{&data_field,action},{&offset_field,offset.get()}};
            offsets.push_back(std::move(offset));items.push_back(std::move(item));
            length.value=int(items.size());
        };
        Method get_count{nullptr,"get_Count",0,[&](void*,void**)->void*{return &length;}};
        Method get_item{nullptr,"get_Item",1,[&](void*,void** args)->void*{
            ++item_reads;return items.at(*static_cast<int*>(args[0])).get();}};
        get_count.native=&get_count;get_item.native=&get_item;
        list_type.methods={&get_count,&get_item};
        add(0,&info);
        for(int i=5;i>=0;--i) add(40.f+i*5,&data);
        live::SpawnReader reader;
        auto result=reader.read(rt,&controller,3,1000);
        check(result.available && result.spawns.size()==6,"second wave queue missing or ActionData.count multiplied twice");
        check(std::abs(result.spawns[0].time-164.59988117218018)<.00001,"fragment real start time not used");
        check(result.spawns[0].route_index==2 && result.spawns.back().time>result.spawns.front().time,"queue order/route lost");
        const auto reads=item_reads, revision=result.revision;
        result=reader.read(rt,&controller,3,1250);
        check(item_reads==reads && result.sampled_ms==1250 && result.revision==revision,"paused unchanged queue reparsed or stopped refreshing");
        version.value++;add(70,&data);
        result=reader.read(rt,&controller,3,1500);
        check(result.spawns.size()==7 && result.revision>revision,"changed action queue not refreshed");
        base.value=std::int64_t(200)*4294967296ll;
        result=reader.read(rt,&controller,3,1750);
        check(result.spawns[0].time==240,"new fragment base time reused old countdown");
        result=reader.read(rt,&controller,4,2000);
        check(result.serial==4 && item_reads>reads+7,"same level restart reused old serial");
        length.value=0; version.value++;
        result=reader.read(rt,&controller,4,2250);
        check(result.available && result.spawns.empty(),"known empty queue incorrectly requests static fallback");
        scheduler.fields.clear();
        result=reader.read(rt,&controller,4,2500);
        check(!result.available && result.spawns.empty(),"unavailable scheduler leaked old wave");
        scheduler.fields={{&queue_field,&queue},{&base_field,&base}};
        length.value=9000;
        check(!reader.read(rt,&controller,4,2750).available,"oversized queue not bounded");
        length.value=int(items.size());extra.value=true;version.value++;
        check(reader.read(rt,&controller,4,3000).spawns.empty(),"extra route guessed as regular red gate");
        std::printf("PASS live spawn reader: %u checks\n",checks);return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
