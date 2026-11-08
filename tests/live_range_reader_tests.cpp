#include "../native/modules/provider/capture/live_range.h"
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using namespace ark::arkhelper::capture;
struct Class;
struct Field { const char* name; Class* type=nullptr; int value=0; };
struct Method {
    void* native=this;
    Class* owner=nullptr;
    const char* name=nullptr;
    std::function<void*(void*)> call;
};
struct Class {
    const char* name;
    const char* space="Torappu.Battle";
    int bytes=0,rank=0;
    std::vector<Field*> fields;
    std::vector<Method*> methods;
};
struct Object {
    Class* klass;
    void* payload=nullptr;
    std::vector<std::pair<Field*,void*>> fields;
    std::size_t count=0;
};
template<class T> struct Box {
    Object object;
    T value;
    Box(Class& type,T initial):object{&type},value(initial){object.payload=&value;}
};
Class integer{"Single","System",4}, boolean{"Boolean","System",1};
Class vector2{"Vector2","UnityEngine",8}, apply_type{"SourceApplyWay","Torappu",4};
Class unit_type{"Enemy"},mode_type{"UnitMode"},attack_type{"RangedAttack"};
Class trigger_type{"SelectorTrigger"},selector_type{"AdvancedSelectorWithTagAndBuffKey"};
Class circle_type{"CircleRange"},collider_type{"CircleCollider2D","UnityEngine"};
Class unsupported_type{"BoxRange"},list_type{"Collider2D[]","UnityEngine",0,1};
Field ranged{"RANGED",&apply_type,2},melee{"MELEE",&apply_type,1};
Field selector_range{"m_range"},external_field{"m_externCollidersList"},circle_field{"m_circleCollider"};
unsigned checks=0;
void check(bool value,const char* label){++checks;if(!value)throw std::runtime_error(label);}
float native_radius(void* receiver,void*) {return *static_cast<float*>(static_cast<Object*>(receiver)->payload);}
void configure(ark::il2cpp::Runtime& rt) {
    rt.object_class=[](void* o)->void*{return static_cast<Object*>(o)->klass;};
    rt.object_unbox=[](void* o)->void*{return static_cast<Object*>(o)->payload;};
    rt.class_name=[](void* k){return static_cast<Class*>(k)->name;};
    rt.class_namespace=[](void* k){return static_cast<Class*>(k)->space;};
    rt.class_parent=[](void*)->void*{return nullptr;};
    rt.class_is_value=[](void* k){return static_cast<Class*>(k)->bytes>0;};
    rt.class_value_size=[](void* k,std::uint32_t* a){*a=4;return static_cast<Class*>(k)->bytes;};
    rt.class_rank=[](void* k){return static_cast<Class*>(k)->rank;};
    rt.class_from_type=[](void* t)->void*{return t;};
    rt.class_field=[](void* k,const char* name)->void*{
        for(auto* f:static_cast<Class*>(k)->fields)if(!std::strcmp(f->name,name))return f;
        return nullptr;
    };
    rt.class_fields=[](void* k,void** it)->void*{
        auto& fs=static_cast<Class*>(k)->fields;auto i=reinterpret_cast<std::uintptr_t>(*it);
        if(i>=fs.size())return nullptr;*it=reinterpret_cast<void*>(i+1);return fs[i];
    };
    rt.field_name=[](void* f){return static_cast<Field*>(f)->name;};
    rt.field_type=[](void* f)->void*{return static_cast<Field*>(f)->type;};
    rt.field_static_get=[](void* f,void* value){*static_cast<int*>(value)=static_cast<Field*>(f)->value;};
    rt.field_get_object=[](void* f,void* o)->void*{
        for(auto& item:static_cast<Object*>(o)->fields)if(item.first==f)return item.second;
        return nullptr;
    };
    rt.class_methods=[](void* k,void** it)->void*{
        auto& ms=static_cast<Class*>(k)->methods;auto i=reinterpret_cast<std::uintptr_t>(*it);
        if(i>=ms.size())return nullptr;*it=reinterpret_cast<void*>(i+1);return ms[i];
    };
    rt.method_name=[](void* m){return static_cast<Method*>(m)->name;};
    rt.method_count=[](void*)->std::uint32_t{return 0;};
    rt.method_class=[](void* m)->void*{return static_cast<Method*>(m)->owner;};
    rt.method_return=[](void*)->void*{return &integer;};
    rt.method_flags=[](void*,std::uint32_t* impl)->std::uint32_t{*impl=0;return 0;};
    rt.thread_current=[]()->void*{return &unit_type;};
    rt.invoke_method=[](void* m,void* receiver,void**,void** error)->void*{
        *error=nullptr;return static_cast<Method*>(m)->call(receiver);
    };
    rt.array_length=[](void* value)->std::uintptr_t{return static_cast<Object*>(value)->count;};
}
struct Fixture {
    Box<int> apply{apply_type,2};
    Box<range_geometry::Point2> offset{vector2,{0,0}};
    Object unit{&unit_type},mode{&mode_type},attack{&attack_type},other_attack{&attack_type};
    Object trigger{&trigger_type},selector{&selector_type},range{&circle_type},collider{&collider_type};
    Object unsupported{&unsupported_type},external{&list_type};
    float radius=5;
    void* active_attack=&attack;
    void* mode_attack=&attack;
    void* direct_range=nullptr;
    std::vector<std::unique_ptr<Method>> methods;
    Method& add(Class& klass,const char* name,std::function<void*(void*)> call) {
        auto m=std::make_unique<Method>(Method{nullptr,&klass,name,std::move(call)});
        m->native=m.get();klass.methods.push_back(m.get());methods.push_back(std::move(m));return *methods.back();
    }
    Fixture() {
        for(auto* k:{&integer,&boolean,&vector2,&apply_type,&unit_type,&mode_type,&attack_type,&trigger_type,
                    &selector_type,&circle_type,&collider_type,&unsupported_type,&list_type}) {
            k->fields.clear();k->methods.clear();
        }
        apply_type.fields={&ranged,&melee};selector_type.fields={&selector_range};
        circle_type.fields={&external_field,&circle_field};
        selector.fields={{&selector_range,&range}};
        range.fields={{&external_field,nullptr},{&circle_field,&collider}};range.payload=&radius;
        add(unit_type,"get_attack",[this](void*){return active_attack;});
        add(unit_type,"get_currentMode",[this](void*)->void*{return &mode;});
        add(unit_type,"get_attackTrigger",[this](void*)->void*{return &trigger;});
        add(mode_type,"get_attack",[this](void*){return mode_attack;});
        add(attack_type,"get_applyWay",[this](void*)->void*{return &apply.object;});
        add(attack_type,"get_rangeToShow",[this](void*){return direct_range;});
        add(trigger_type,"get_selector",[this](void*)->void*{return &selector;});
        add(collider_type,"get_offset",[this](void*)->void*{return &offset.object;});
        auto& radius_method=add(circle_type,"get_radius",[](void*)->void*{return nullptr;});
        radius_method.native=reinterpret_cast<void*>(&native_radius);
    }
};
}
int main(){
    try{
        Fixture f;ark::il2cpp::Runtime rt;configure(rt);live::RangeReader reader;float radius=0;
        check(reader.radius(rt,&f.unit,radius)&&radius==5,"ordinary RangedAttack with null own selector lost trigger search circle");
        f.radius=7.5f;
        check(reader.radius(rt,&f.unit,radius)&&radius==7.5f,"live search range modifier was cached as base radius");
        f.mode_attack=&f.other_attack;
        check(!reader.radius(rt,&f.unit,radius),"override attack used a different mode's trigger range");
        f.direct_range=&f.range;
        check(reader.radius(rt,&f.unit,radius),"attack's own verified range was lost for overrides");
        f.direct_range=nullptr;f.mode_attack=&f.attack;f.apply.value=1;
        check(!reader.radius(rt,&f.unit,radius),"melee attack was classified as ranged");
        f.apply.value=2;f.selector.fields[0].second=&f.unsupported;
        check(!reader.radius(rt,&f.unit,radius),"noncircular search range was guessed as a circle");
        f.selector.fields[0].second=&f.range;f.external.count=1;f.range.fields[0].second=&f.external;
        check(!reader.radius(rt,&f.unit,radius),"compound external colliders were silently ignored");
        f.range.fields[0].second=nullptr;f.offset.value={.3f,0};
        check(!reader.radius(rt,&f.unit,radius),"unsupported offset collider was drawn at the wrong center");
        f.offset.value={0,0};f.selector.fields[0].second=nullptr;
        check(!reader.radius(rt,&f.unit,radius),"missing current selector reused an old circle");
        f.active_attack=nullptr;
        check(!reader.radius(rt,&f.unit,radius),"no current attack unexpectedly produced a range");
        std::printf("Live range reader: %u checks passed; actual Runtime and RangeReader\n",checks);
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL range reader: %s\n",error.what());return 1;}
}
