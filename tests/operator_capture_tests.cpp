#include "../native/modules/provider/capture/live_operator.h"
#include <cstdio>
#include <vector>

namespace {
struct Class { const char* name; const char* space; int rank, value_size; };
Class unordered{"UnorderedArray`1","Torappu",0,0};
Class array_class{"Unit[]","Torappu.Battle",1,0};
Class int_class{"Int32","System",0,4};
Class unknown_class{"UnorderedArray`1","Other",0,0};
Class uint_class{"UInt32","System",0,4};
Class bool_class{"Boolean","System",0,1};
Class string_class{"String","System",0,0};
Class actor_class{"Character","Torappu.Battle",0,0};
struct Object { Class* klass; };
struct Box { Class* klass; int value; };
struct Array { Class* klass; std::vector<void*> values; };
struct Container { Class* klass; Array* items; Box count; };
struct String { Class* klass; const wchar_t* value; };
struct Actor {
    Class* klass = &actor_class;
    Box uid{&uint_class,631}, alive{&bool_class,1}, born{&bool_class,1};
    Box disappeared{&bool_class,0}, dummy{&bool_class,0};
    Box token{&bool_class,0};
    String id{&string_class,L"char_219_meteo"};
};
struct Method { const char* name; };
Method methods[]={{"get_characterId"},{"get_alive"},{"get_borned"},
    {"get_isDisappeared"},{"get_instanceUid"},{"get_isDummy"},{"get_isToken"}};
int items_field, count_field;
bool have_items = true, have_count = true;
unsigned checks = 0, failures = 0;
void check(bool value, const char* label) {
    ++checks; if (!value) { ++failures; std::printf("FAIL %s\n",label); }
}
}

int main() {
    ark::il2cpp::Runtime runtime;
    runtime.object_class = [](void* value) -> void* { return static_cast<Object*>(value)->klass; };
    runtime.class_name = [](void* klass) { return static_cast<Class*>(klass)->name; };
    runtime.class_namespace = [](void* klass) { return static_cast<Class*>(klass)->space; };
    runtime.class_rank = [](void* klass) { return static_cast<Class*>(klass)->rank; };
    runtime.class_parent = [](void*) -> void* { return nullptr; };
    runtime.class_is_value = [](void* klass) { return static_cast<Class*>(klass)->value_size > 0; };
    runtime.class_value_size = [](void* klass,std::uint32_t* align) { *align = 4; return static_cast<Class*>(klass)->value_size; };
    runtime.object_unbox = [](void* value) -> void* { return &static_cast<Box*>(value)->value; };
    runtime.array_length = [](void* value) -> std::uintptr_t { return static_cast<Array*>(value)->values.size(); };
    runtime.class_field = [](void* klass,const char* name) -> void* {
        if (klass != &unordered) return nullptr;
        if (have_items && !std::strcmp(name,"m_items")) return &items_field;
        if (have_count && !std::strcmp(name,"<count>k__BackingField")) return &count_field;
        return nullptr;
    };
    runtime.field_get_object = [](void* field,void* self) -> void* {
        auto& value = *static_cast<Container*>(self);
        return field == &items_field ? static_cast<void*>(value.items) : &value.count;
    };
    // Deliberately expose no get_Count metadata: this is the actual client difference that
    // caused the operator scan to return zero forever. The production helper must read fields.
    runtime.class_methods = [](void*,void**) -> void* { return nullptr; };
    Array array{&array_class,{reinterpret_cast<void*>(1),reinterpret_cast<void*>(2),nullptr,nullptr}};
    Container characters{&unordered,&array,{&int_class,2}};
    void* items = nullptr;
    int count = -1;
    using ark::arkhelper::capture::live::operator_sequence;
    check(operator_sequence(runtime,&characters,items,count) && items == &array && count == 2,
          "lowercase-count UnorderedArray reads only its two live items, not four capacity slots");
    characters.count.value = 0;
    check(operator_sequence(runtime,&characters,items,count) && count == 0,
          "empty current deployment list clears all positions");
    characters.count.value = 5;
    check(!operator_sequence(runtime,&characters,items,count) && !items && count == 0,
          "count beyond backing-array capacity fails closed");
    characters.count.value = -1;
    check(!operator_sequence(runtime,&characters,items,count),"negative live count is rejected");
    characters.count.value = 257;
    check(!operator_sequence(runtime,&characters,items,count),"collection work remains bounded");
    characters.count.value = 1;
    have_count = false;
    check(!operator_sequence(runtime,&characters,items,count),"renamed count field is unavailable");
    have_count = true; have_items = false;
    check(!operator_sequence(runtime,&characters,items,count),"renamed backing array is unavailable");
    have_items = true;
    characters.klass = &unknown_class;
    check(!operator_sequence(runtime,&characters,items,count),"unrelated similarly named collection is rejected");
    characters.klass = &unordered; characters.items = nullptr;
    check(!operator_sequence(runtime,&characters,items,count),"missing current array never reuses a previous reference");
    check(operator_sequence(runtime,&array,items,count) && items == &array && count == 4,
          "real rank-one arrays retain their normal supported path");
    runtime.class_methods=[](void* klass,void** iterator) -> void* {
        if(klass!=&actor_class) return nullptr;
        auto index=reinterpret_cast<std::uintptr_t>(*iterator);
        if(index>=std::size(methods)) return nullptr;
        *iterator=reinterpret_cast<void*>(index+1);return &methods[index];
    };
    runtime.method_name=[](void* method){return static_cast<Method*>(method)->name;};
    runtime.method_count=[](void*) -> std::uint32_t {return 0;};
    runtime.method_flags=[](void*,std::uint32_t*) -> std::uint32_t {return 0;};
    runtime.method_class=[](void*) -> void* {return &actor_class;};
    runtime.thread_current=[]() -> void* {return &actor_class;};
    runtime.invoke_method=[](void* method,void* object,void**,void**) -> void* {
        auto& actor=*static_cast<Actor*>(object);
        const auto index=static_cast<Method*>(method)-methods;
        switch(index){case 0:return &actor.id;case 1:return &actor.alive;case 2:return &actor.born;
            case 3:return &actor.disappeared;case 4:return &actor.uid;case 5:return &actor.dummy;case 6:return &actor.token;}
        return nullptr;
    };
    runtime.string_length=[](void* value){return static_cast<int>(std::wcslen(static_cast<String*>(value)->value));};
    runtime.string_chars=[](void* value){return static_cast<String*>(value)->value;};
    ark::arkhelper::capture::live::OperatorReader reader;
    Actor actor;std::string id;std::uint64_t uid=0;
    check(reader.read(runtime,&actor,id,uid) && uid==631 && id=="char_219_meteo",
          "actual client UInt32 instanceUid is accepted by production reader");
    actor.uid.value=static_cast<int>(0x80000011u);
    check(reader.read(runtime,&actor,id,uid) && uid==0x80000011u,
          "unsigned instance IDs retain their high bit without sign extension");
    actor.uid.klass=&int_class;actor.uid.value=632;
    check(reader.read(runtime,&actor,id,uid) && uid==632,"positive signed UID remains compatible");
    actor.uid.value=-1;
    check(!reader.read(runtime,&actor,id,uid) && uid==0,"negative signed UID cannot create a phantom identity");
    actor.uid.klass=&uint_class;actor.uid.value=631;actor.dummy.value=1;
    check(!reader.read(runtime,&actor,id,uid),"preparation dummy must not create an active operator label");
    actor.dummy.value=0;actor.alive.value=0;
    check(!reader.read(runtime,&actor,id,uid),"dead operator cannot retain its head label");
    actor.alive.value=1;actor.disappeared.value=1;
    check(!reader.read(runtime,&actor,id,uid),"disappeared operator cannot retain its head label");
    actor.disappeared.value=0;actor.id.value=L"trap_stage_device";
    check(!reader.read(runtime,&actor,id,uid),"stage device must not have an operator head label");
    actor.id.value=L"token_summon";
    check(!reader.read(runtime,&actor,id,uid),"summon must not have a separate operator head label");
    actor.id.value=L"char_219_meteo";actor.token.value=1;
    check(!reader.read(runtime,&actor,id,uid),"a token with a character-shaped ID is still not an independent operator");
    std::printf("Operator capture: %u/%u passed\n",checks-failures,checks);
    return failures ? 1 : 0;
}
