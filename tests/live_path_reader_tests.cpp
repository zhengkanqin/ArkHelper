#include "../native/modules/provider/capture/live_path.h"
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

namespace pg = ark::arkhelper::capture::path_geometry;
namespace live = ark::arkhelper::capture::live;

namespace {
struct Class;
struct Field { const char* name; };
struct Method {
    // Production Runtime::method checks the first MethodInfo word for AOT code.
    void* native = this;
    Class* owner;
    const char* name;
    std::vector<const char*> parameters;
    bool is_static;
    std::function<void*(void*,void**)> call;
};
struct Class {
    const char* name;
    const char* space;
    int rank = 0, value_size = 0;
    Class* element = nullptr;
    std::vector<Field*> fields;
    std::vector<Method*> methods;
};
struct Object {
    Class* klass;
    void* payload = nullptr;
    std::vector<std::pair<Field*,void*>> fields;
    std::vector<void*> items;
    int rows = 0, cols = 0;
};
template<class T> struct Box {
    Object object;
    T value;
    Box(Class& klass,T initial) : object{&klass}, value(initial) { object.payload = &value; }
};
struct NodePayload { void* safe_position; };

Class integer{"Int32","System",0,4};
Class boolean{"Boolean","System",0,1};
Class vector2{"Vector2","UnityEngine",0,sizeof(pg::Point)};
Class grid{"GridPosition","Torappu",0,sizeof(pg::Grid)};
Class checkpoint_type{"CheckpointType","Torappu.Battle",0,4};
Class enemy_class{"Enemy","Torappu.Battle"};
Class cursor_class{"DirectionCursor","Torappu.Battle"};
Class visual_class{"VisualObject","Torappu.Battle"};
Class checkpoint_class{"MoveCheckpoint","Torappu.Battle"};
Class checkpoint_array{"Checkpoint[]","Torappu.Battle",1};
Class node_class{"Node","Torappu.Battle",0,sizeof(NodePayload)};
Class node_array{"Node[,]","Torappu.Battle",2,0,&node_class};
Class route_class{"Route","Torappu.Battle"};
Class data_class{"RouteData","Torappu"};
Class exit_class{"RouteExit","Torappu"};
Field enemy_cursor{"m_cursor"}, cursor_index{"m_cursor"}, checkpoints_field{"m_checkpoints"};
Field cursor_object{"m_obj"};
Field offset_field{"m_offset"}, route_field{"m_route"}, map_field{"m_nextmap"};
Field node_pos{"pos"}, node_next{"next"}, node_has_next{"hasNext"};
Field route_data{"data"}, end_position{"endPosition"}, final_map{"m_targetNextMap"}, move_enum{"MOVE"};
unsigned checks = 0, failures = 0;
void check(bool condition,const char* description) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL %s\n",description); }
}

// Only embedding exports are faked. All Runtime helpers and PathReader::read are production code.
void configure(ark::il2cpp::Runtime& rt) {
    rt.object_class = [](void* value) -> void* { return static_cast<Object*>(value)->klass; };
    rt.object_unbox = [](void* value) -> void* { return static_cast<Object*>(value)->payload; };
    rt.class_name = [](void* klass) { return static_cast<Class*>(klass)->name; };
    rt.class_namespace = [](void* klass) { return static_cast<Class*>(klass)->space; };
    rt.class_parent = [](void*) -> void* { return nullptr; };
    rt.class_rank = [](void* klass) { return static_cast<Class*>(klass)->rank; };
    rt.class_is_value = [](void* klass) { return static_cast<Class*>(klass)->value_size > 0; };
    rt.class_value_size = [](void* klass,std::uint32_t* align) {
        *align = 4; return static_cast<Class*>(klass)->value_size;
    };
    rt.class_element = [](void* klass) -> void* { return static_cast<Class*>(klass)->element; };
    rt.class_from_type = [](void* type) -> void* { return type; };
    rt.class_field = [](void* klass,const char* name) -> void* {
        for (auto* field : static_cast<Class*>(klass)->fields)
            if (!std::strcmp(name,field->name)) return field;
        return nullptr;
    };
    rt.field_get_object = [](void* field,void* self) -> void* {
        for (const auto& item : static_cast<Object*>(self)->fields)
            if (item.first == field) return item.second;
        return nullptr;
    };
    rt.class_fields = [](void* klass,void** iterator) -> void* {
        auto& fields = static_cast<Class*>(klass)->fields;
        const auto index = reinterpret_cast<std::uintptr_t>(*iterator);
        if (index >= fields.size()) return nullptr;
        *iterator = reinterpret_cast<void*>(index+1); return fields[index];
    };
    rt.field_type = [](void*) -> void* { return &checkpoint_type; };
    rt.field_name = [](void* field) { return static_cast<Field*>(field)->name; };
    rt.field_static_get = [](void*,void* output) { *static_cast<int*>(output) = 0; };
    rt.class_methods = [](void* klass,void** iterator) -> void* {
        auto& methods = static_cast<Class*>(klass)->methods;
        const auto index = reinterpret_cast<std::uintptr_t>(*iterator);
        if (index >= methods.size()) return nullptr;
        *iterator = reinterpret_cast<void*>(index+1); return methods[index];
    };
    rt.method_name = [](void* method) { return static_cast<Method*>(method)->name; };
    rt.method_count = [](void* method) { return static_cast<std::uint32_t>(static_cast<Method*>(method)->parameters.size()); };
    rt.method_param = [](void* method,std::uint32_t index) -> void* {
        return const_cast<char*>(static_cast<Method*>(method)->parameters.at(index));
    };
    rt.method_class = [](void* method) -> void* { return static_cast<Method*>(method)->owner; };
    rt.method_flags = [](void* method,std::uint32_t* implementation) -> std::uint32_t {
        *implementation = 0; return static_cast<Method*>(method)->is_static ? 0x10u : 0;
    };
    rt.type_name = [](void* type) -> char* {
        const char* text = static_cast<const char*>(type);
        auto* copy = static_cast<char*>(std::malloc(std::strlen(text)+1));
        std::strcpy(copy,text); return copy;
    };
    rt.free_memory = [](void* memory) { std::free(memory); };
    rt.thread_current = []() -> void* { return &grid; };
    rt.invoke_method = [](void* method,void* receiver,void** arguments,void** exception) -> void* {
        *exception = nullptr; return static_cast<Method*>(method)->call(receiver,arguments);
    };
    rt.array_length = [](void* value) -> std::uintptr_t { return static_cast<Object*>(value)->items.size(); };
    rt.domain_get = []() -> void* { return &grid; };
    rt.domain_assemblies = [](void*,std::size_t* count) -> void** {
        static void* assemblies[]{&grid}; *count = 1; return assemblies;
    };
    rt.assembly_image = [](void* assembly) -> void* { return assembly; };
    rt.class_from_name = [](void*,const char* space,const char* name) -> void* {
        return !std::strcmp(space,"Torappu") && !std::strcmp(name,"GridPosition") ? &grid : nullptr;
    };
}

struct Fixture {
    Box<int> index{integer,0}, move{checkpoint_type,0};
    Box<pg::Point> offset{vector2,{}}, map_position{vector2,{}}, cp1_target{vector2,{2,3}}, cp2_target{vector2,{3,4}};
    Box<pg::Grid> cp1_grid{grid,{3,2}}, cp2_grid{grid,{4,3}}, converted{grid,{}};
    Object enemy{&enemy_class}, cursor{&cursor_class}, visual{&visual_class}, checkpoints{&checkpoint_array};
    Object cp1{&checkpoint_class}, cp2{&checkpoint_class}, map1{&node_array}, map2{&node_array}, map3{&node_array};
    Object route{&route_class}, data{&data_class}, end{&exit_class};
    std::vector<pg::Point> lookups;
    std::vector<pg::Grid> indexed;
    unsigned host_position_reads = 0;
    std::vector<std::unique_ptr<Box<pg::Grid>>> grid_boxes;
    std::vector<std::unique_ptr<Box<pg::Point>>> point_boxes;
    std::vector<std::unique_ptr<Box<bool>>> bool_boxes;
    std::vector<std::unique_ptr<NodePayload>> node_payloads;
    std::vector<std::unique_ptr<Object>> nodes;
    std::vector<std::unique_ptr<Method>> methods;

    void add_method(Class& owner,const char* name,std::vector<const char*> parameters,bool is_static,
                    std::function<void*(void*,void**)> call) {
        auto method = std::make_unique<Method>(Method{nullptr,&owner,name,std::move(parameters),is_static,std::move(call)});
        method->native = method.get(); owner.methods.push_back(method.get()); methods.push_back(std::move(method));
    }
    void add_node(Object& map,pg::Grid position,pg::Grid next,bool has_next) {
        auto pos = std::make_unique<Box<pg::Grid>>(grid,position);
        auto next_box = std::make_unique<Box<pg::Grid>>(grid,next);
        auto has = std::make_unique<Box<bool>>(boolean,has_next);
        auto safe = std::make_unique<Box<pg::Point>>(vector2,pg::Point{float(next.col),float(next.row)});
        auto payload = std::make_unique<NodePayload>(NodePayload{&safe->object});
        auto node = std::make_unique<Object>(Object{&node_class,payload.get()});
        node->fields = {{&node_pos,&pos->object},{&node_next,&next_box->object},{&node_has_next,&has->object}};
        map.items.at(position.row*map.cols+position.col) = node.get();
        grid_boxes.push_back(std::move(pos)); grid_boxes.push_back(std::move(next_box));
        point_boxes.push_back(std::move(safe)); bool_boxes.push_back(std::move(has));
        node_payloads.push_back(std::move(payload)); nodes.push_back(std::move(node));
    }
    Fixture() {
        for (auto* klass : {&integer,&boolean,&vector2,&grid,&checkpoint_type,&enemy_class,&cursor_class,&visual_class,
             &checkpoint_class,&checkpoint_array,&node_class,&node_array,&route_class,&data_class,&exit_class}) {
            klass->fields.clear(); klass->methods.clear();
        }
        enemy_class.fields = {&enemy_cursor};
        cursor_class.fields = {&cursor_index,&checkpoints_field,&offset_field,&route_field,&cursor_object};
        checkpoint_class.fields = {&map_field}; node_class.fields = {&node_pos,&node_next,&node_has_next};
        checkpoint_type.fields = {&move_enum}; route_class.fields = {&route_data,&final_map}; data_class.fields = {&end_position};
        enemy.fields = {{&enemy_cursor,&cursor}};
        cursor.fields = {{&cursor_index,&index.object},{&checkpoints_field,&checkpoints},{&cursor_object,&visual},
                         {&offset_field,&offset.object},{&route_field,&route}};
        checkpoints.items = {&cp1,&cp2}; cp1.fields = {{&map_field,&map1}}; cp2.fields = {{&map_field,&map2}};
        route.fields = {{&route_data,&data},{&final_map,&map3}}; data.fields = {{&end_position,&cp2_grid.object}};
        for (auto* map : {&map1,&map2,&map3}) { map->rows = 5; map->cols = 6; map->items.resize(30); }
        add_node(map1,{2,1},{2,2},true); add_node(map1,{2,2},{3,2},true); add_node(map1,{3,2},{3,2},false);
        add_node(map2,{3,2},{3,3},true); add_node(map2,{3,3},{4,3},true); add_node(map2,{4,3},{4,3},false);
        add_node(map3,{2,1},{2,2},true); add_node(map3,{2,2},{3,2},true); add_node(map3,{3,2},{3,3},true);
        add_node(map3,{3,3},{4,3},true); add_node(map3,{4,3},{4,3},false);
        add_method(grid,"FromVectorPosition",{"UnityEngine.Vector2"},true,[this](void*,void** arguments) -> void* {
            const auto point = *static_cast<pg::Point*>(arguments[0]); lookups.push_back(point);
            converted.value = {int(std::floor(point.y)),int(std::floor(point.x))}; return &converted.object;
        });
        add_method(visual_class,"get_mapPosition",{},false,[this](void*,void**) -> void* {
            ++host_position_reads; return &map_position.object;
        });
        add_method(checkpoint_array,"GetValue",{"System.Int32"},false,[](void* receiver,void** arguments) -> void* {
            return static_cast<Object*>(receiver)->items.at(*static_cast<int*>(arguments[0]));
        });
        add_method(node_array,"GetValue",{"System.Int32","System.Int32"},false,[this](void* receiver,void** arguments) -> void* {
            auto& map = *static_cast<Object*>(receiver);
            const int row = *static_cast<int*>(arguments[0]), col = *static_cast<int*>(arguments[1]);
            indexed.push_back({row,col});
            if (row < 0 || col < 0 || row >= map.rows || col >= map.cols) throw std::runtime_error("map index outside bounds");
            return map.items.at(row*map.cols+col);
        });
        add_method(checkpoint_class,"get_type",{},false,[this](void*,void**) -> void* { return &move.object; });
        add_method(checkpoint_class,"get_targetGrid",{},false,[this](void* receiver,void**) -> void* {
            return receiver == &cp1 ? &cp1_grid.object : &cp2_grid.object;
        });
        add_method(checkpoint_class,"get_targetPos",{},false,[this](void* receiver,void**) -> void* {
            return receiver == &cp1 ? &cp1_target.object : &cp2_target.object;
        });
        add_method(node_class,"GetSafeNextPosition",{},false,[](void* unboxed,void**) -> void* {
            return static_cast<NodePayload*>(unboxed)->safe_position;
        });
        add_method(grid,"get_asLocalPosition",{},false,[this](void*,void**) -> void* { return &cp2_target.object; });
    }
};

void run(pg::Point offset,const char* label) {
    Fixture fixture;
    fixture.offset.value = offset;
    ark::il2cpp::Runtime runtime;
    configure(runtime);
    live::PathReader reader;
    pg::Route route;
    std::uint32_t revision = 0;
    // Navigation uses the cursor host's mapPosition, which can differ from the visible foot.
    constexpr pg::Point navigation[]{ {1.2f,2.1f},{2,2},{2,3},{3,3},{3,4} };
    fixture.map_position.value = {navigation[0].x-offset.x,navigation[0].y-offset.y};
    // A neighboring foot grid catches accidental use of footMapPosition for node lookup,
    // including the zero-offset case. The published first point must still be this exact foot.
    const pg::Point physical{fixture.map_position.value.x+.8f,fixture.map_position.value.y+.6f};
    const bool ok = reader.read(runtime,&fixture.enemy,physical,route,revision,[] { return true; });
    check(ok,label);
    if (!ok) std::printf("  production reader: %s\n",reader.reason().c_str());
    check(ok && route.count == std::size(navigation),"real nextmap retains all turns across both MOVE checkpoints");
    check(ok && !route.truncated && revision != 0,"real route end verifies a complete published route");
    check(route.count && pg::same(route.points[0].point,physical) &&
          (route.points[0].flags & arkhelp::ipc::kPathMoveTo),"first point keeps the exact current physical foot");
    bool shifted = route.count == std::size(navigation);
    for (std::uint32_t i = 1; shifted && i < route.count; ++i)
        shifted = pg::same(route.points[i].point,{navigation[i].x-offset.x,navigation[i].y-offset.y});
    check(shifted,"published nextmap points and checkpoint target positions subtract cursor offset");
    check(fixture.host_position_reads == 1 && fixture.lookups.size() == 2 && pg::same(fixture.lookups[0],navigation[0]),
          "first FromVectorPosition receives cursor host mapPosition plus offset, never footMapPosition");
    check(fixture.lookups.size() == 2 && pg::same(fixture.lookups[1],navigation[2]),
          "second checkpoint uses navigation ground without adding offset twice");
    const pg::Grid expected[]{ {2,1},{2,2},{3,2},{3,2},{3,3},{4,3} };
    bool exact = fixture.indexed.size() == std::size(expected);
    for (std::size_t i = 0; exact && i < std::size(expected); ++i) exact = fixture.indexed[i] == expected[i];
    check(exact,"two-dimensional nextmap is traversed through its actual row/column nodes");
}

void remaining_route() {
    Fixture f; ark::il2cpp::Runtime rt; configure(rt); live::PathReader reader;
    pg::Route route; std::uint32_t revision=0;
    f.map_position.value={1.2f,2.1f}; f.checkpoints.items.clear();
    check(reader.read(rt,&f.enemy,f.map_position.value,route,revision,[]{return true;}) &&
          route.count==5 && !route.truncated && pg::same(route.points[4].point,{3,4}),
          "ordinary zero-checkpoint route follows the real target navigation map");
    check(f.lookups.size()==1 && f.indexed.front()==pg::Grid{2,1},
          "zero-checkpoint route starts at the current navigation grid");
    f.checkpoints.items={&f.cp1}; f.lookups.clear(); route={};
    check(reader.read(rt,&f.enemy,f.map_position.value,route,revision,[]{return true;}) &&
          route.count==5 && !route.truncated && pg::same(route.points[4].point,{3,4}) && f.lookups.size()==2,
          "explicit checkpoint route includes its separate final leg to the real exit");
    f.index.value=1; f.map_position.value={2.2f,3.1f}; f.lookups.clear(); route={};
    check(reader.read(rt,&f.enemy,f.map_position.value,route,revision,[]{return true;}) &&
          route.count==3 && !route.truncated && pg::same(route.points[0].point,{2.2f,3.1f}) &&
          pg::same(route.points[1].point,{3,3}) && f.lookups.size()==1,
          "consumed checkpoints are skipped when cursor equals explicit checkpoint count");
    f.index.value=2; route={};
    check(!reader.read(rt,&f.enemy,f.map_position.value,route,revision,[]{return true;}),
          "cursor beyond the real checkpoint count is rejected");
    f.index.value=0; f.checkpoints.items.clear(); f.route.fields[1].second=nullptr; route={};
    check(!reader.read(rt,&f.enemy,f.map_position.value,route,revision,[]{return true;}),
          "missing final navigation map is never replaced by a guessed straight line");
}
}

int main() {
    try {
        run({0,0},"zero offset production PathReader reads a turning live nextmap");
        run({.75f,-.6f},"nonzero offset production PathReader starts at the correct navigation node");
        run({-.6f,.75f},"opposite-sign offset production PathReader follows the same real route");
        remaining_route();
    } catch (const std::exception& error) {
        ++checks; ++failures; std::printf("FAIL unexpected production Runtime exception: %s\n",error.what());
    }
    std::printf("Live PathReader: %u/%u passed; actual Runtime and PathReader, no game process\n",checks-failures,checks);
    return failures ? 1 : 0;
}
