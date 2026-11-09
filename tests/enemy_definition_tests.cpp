#include "../native/modules/provider/capture/live_data.h"
#include <cstdio>
#include <map>
#include <stdexcept>

namespace {
struct Method;
struct Class { const char* name; const char* space = "Torappu"; int bytes = 0; std::vector<Method*> methods; };
struct Object { Class* klass; void* payload = nullptr; std::map<std::string, void*> fields; };
struct Method { void* native = this; Class* owner; const char* name = "TryGet"; unsigned arity = 2; Class* result = nullptr; };
Class data_class{"EnemyData"}, ref_class{"EnemyDataDbReference"}, attrs_class{"Attributes"};
Class db_class{"EnemyDB"}, string_class{"String", "System"};
Class number_class{"Double", "System", 8}, bool_class{"Boolean", "System", 1};
Class int_class{"Int32", "System", 4}, float_class{"Single", "System", 4};
Class obscured_int{"ObscuredInt", "CodeStage.AntiCheat.ObscuredTypes", 20};
Class obscured_float{"ObscuredFloat", "CodeStage.AntiCheat.ObscuredTypes", 24};
std::map<unsigned, void*> roots;
unsigned next_root = 0, checks = 0, lookups = 0;
Object* expected_reference = nullptr;
Object* lookup_result = nullptr;
bool lookup_success = true;
Object boxed_result{&bool_class, &lookup_success};
void check(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
void configure(ark::il2cpp::Runtime& rt) {
    rt.object_class = [](void* o)->void* { return static_cast<Object*>(o)->klass; };
    rt.object_unbox = [](void* o)->void* { return static_cast<Object*>(o)->payload; };
    rt.class_name = [](void* c) { return static_cast<Class*>(c)->name; };
    rt.class_namespace = [](void* c) { return static_cast<Class*>(c)->space; };
    rt.class_parent = [](void*)->void* { return nullptr; };
    rt.class_is_value = [](void* c) { return static_cast<Class*>(c)->bytes > 0; };
    rt.class_value_size = [](void* c, std::uint32_t* alignment)->std::int32_t {
        *alignment = 4; return static_cast<Class*>(c)->bytes;
    };
    rt.class_field = [](void*, const char* name)->void* { return const_cast<char*>(name); };
    rt.field_get_object = [](void* field, void* object)->void* {
        auto& fields = static_cast<Object*>(object)->fields;
        const auto found = fields.find(static_cast<const char*>(field));
        return found == fields.end() ? nullptr : found->second;
    };
    rt.string_length = [](void* o) { return static_cast<int>(static_cast<std::wstring*>(static_cast<Object*>(o)->payload)->size()); };
    rt.string_chars = [](void* o) { return static_cast<std::wstring*>(static_cast<Object*>(o)->payload)->c_str(); };
    rt.thread_current = []()->void* { return &db_class; };
    rt.class_from_type = [](void* t)->void* { return t; };
    rt.class_methods = [](void* c, void** iterator)->void* {
        auto& methods = static_cast<Class*>(c)->methods;
        auto index = reinterpret_cast<std::uintptr_t>(*iterator);
        if (index >= methods.size()) return nullptr;
        *iterator = reinterpret_cast<void*>(index+1); return methods[index];
    };
    rt.method_count = [](void* m)->std::uint32_t { return static_cast<Method*>(m)->arity; };
    rt.method_name = [](void* m) { return static_cast<Method*>(m)->name; };
    rt.method_class = [](void* m)->void* { return static_cast<Method*>(m)->owner; };
    rt.method_return = [](void* m)->void* { return static_cast<Method*>(m)->result; };
    rt.method_flags = [](void*, std::uint32_t* impl)->std::uint32_t { *impl = 0; return 0; };
    rt.invoke_method = [](void* m, void* receiver, void** args, void** error)->void* {
        *error = nullptr;
        if (!std::strcmp(static_cast<Method*>(m)->name, "GetDecrypted"))
            return *static_cast<Object**>(receiver);
        check(receiver && args[0] == expected_reference, "lookup did not receive the exact current-level reference");
        ++lookups; *error = nullptr;
        *static_cast<void**>(args[1]) = lookup_result;
        return &boxed_result;
    };
    rt.gc_new = [](void* o, bool)->std::uint32_t { roots[++next_root] = o; return next_root; };
    rt.gc_target = [](std::uint32_t key)->void* { return roots.at(key); };
    rt.gc_free = [](std::uint32_t key) { roots.erase(key); };
}
}

int main() {
    try {
        using namespace ark::arkhelper;
        using namespace capture::live;
        ark::il2cpp::Runtime rt; configure(rt);
        std::wstring alias = L"enemy_level_alias", base_key = L"enemy_base", name = L"关卡敌人";
        Object id{&string_class, &alias}, key{&string_class, &base_key}, title{&string_class, &name};
        double hp = 12000, atk = 850, defense = 0, resistance = 35, speed = 1.25, level = 2;
        Object hp_value{&number_class, &hp}, atk_value{&number_class, &atk}, def_value{&number_class, &defense};
        Object res_value{&number_class, &resistance}, speed_value{&number_class, &speed}, level_value{&number_class, &level};
        Object attributes{&attrs_class, nullptr, {{"maxHp", &hp_value}, {"atk", &atk_value},
            {"def", &def_value}, {"magicResistance", &res_value}, {"moveSpeed", &speed_value}}};
        Object data{&data_class, nullptr, {{"name", &title}, {"key", &key}, {"attributes", &attributes}}};
        Object reference{&ref_class, nullptr, {{"id", &id}, {"level", &level_value}, {"overwrittenData", &data}}};
        Object db{&db_class}; Method lookup{nullptr, &db_class};
        lookup.native = &lookup; expected_reference = &reference; lookup_result = &data;
        Stage stage;
        check(read_reference_definition(rt, &db, &lookup, &reference, stage) && stage.enemies.size() == 1,
            "current level definition was unavailable before any enemy spawned");
        check(stage.enemies[0].key == "enemy_level_alias" && stage.enemies[0].attributes.max_hp == 12000 &&
            stage.enemies[0].attributes.atk == 850 && (stage.enemies[0].stats_mask & (1u << 3)),
            "level alias, numerical fields or a valid zero defense were lost");
        check(!(stage.enemies[0].stats_mask & (1u << 7)), "missing fields became invented zero values");
        stage.enemies[0].attributes.atk = 1500;
        read_reference_definition(rt, &db, &lookup, &reference, stage);
        check(stage.enemies.size() == 1 && stage.enemies[0].attributes.atk == 1500,
            "database fallback duplicated a type or overwrote an existing level-specific field");
        Stage next; hp = 18000; atk = 2000;
        read_reference_definition(rt, &db, &lookup, &reference, next);
        check(next.enemies[0].attributes.max_hp == 18000 && next.enemies[0].attributes.atk == 2000,
            "next battle reused stale level values");
        lookup_success = false;
        Stage missing;
        check(!read_reference_definition(rt, &db, &lookup, &reference, missing) && missing.enemies.empty(),
            "a failed database lookup created numerical data");
        check(roots.empty() && lookups == 4, "definition reads leaked managed roots or traversed unrelated data");
        Method int_getter{nullptr, &obscured_int, "GetDecrypted", 0, &int_class}; int_getter.native = &int_getter;
        Method float_getter{nullptr, &obscured_float, "GetDecrypted", 0, &float_class}; float_getter.native = &float_getter;
        obscured_int.methods = {&int_getter}; obscured_float.methods = {&float_getter};
        int wrapped_hp = 23456; float wrapped_speed = 0.75f;
        Object decoded_hp{&int_class, &wrapped_hp}, decoded_speed{&float_class, &wrapped_speed};
        Object* hp_pointer = &decoded_hp; Object* speed_pointer = &decoded_speed;
        Object wrapped_hp_box{&obscured_int, &hp_pointer}, wrapped_speed_box{&obscured_float, &speed_pointer};
        attributes.fields["maxHp"] = &wrapped_hp_box; attributes.fields["moveSpeed"] = &wrapped_speed_box;
        lookup_success = true; Stage wrapped;
        read_reference_definition(rt, &db, &lookup, &reference, wrapped);
        check(wrapped.enemies[0].attributes.max_hp == 23456 && wrapped.enemies[0].attributes.move_speed == 0.75 &&
            (wrapped.enemies[0].stats_mask & (1u << 1)) && (wrapped.enemies[0].stats_mask & (1u << 5)),
            "game numerical wrappers were discarded as unavailable fields");
        check(wrapped_hp == 23456 && wrapped_speed == 0.75f && roots.empty(), "reading copied wrappers changed values or leaked roots");
        std::printf("Enemy definitions: %u checks, 0 failures\n", checks);
    } catch (const std::exception& error) { std::fprintf(stderr, "FAIL %s\n", error.what()); return 1; }
}
