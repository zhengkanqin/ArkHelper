#include "../src/features/path_animation.h"
#include <cstdio>
#include <stdexcept>
#include <limits>
#include <vector>

namespace {
int checks=0;
struct Line {float x,y,end_x,end_y,width;int style;};
std::vector<Line> lines;
void check(bool condition,const char* reason) { ++checks; if(!condition) throw std::runtime_error(reason); }
arkhelp::ipc::BattleSnapshot fixture() {
    using namespace arkhelp;
    ipc::BattleSnapshot snapshot{};
    snapshot.in_battle=1; snapshot.battle_serial=1; snapshot.unit_count=1;
    snapshot.units[0].flags=ipc::kUnitValid|ipc::kUnitGroundValid;
    snapshot.units[0].tracking_id=42;
    snapshot.units[0].x=snapshot.units[0].ground_x=100;
    snapshot.units[0].y=snapshot.units[0].ground_y=100;
    snapshot.path_count=1;
    auto& path=snapshot.paths[0];
    path.flags=ipc::kPathValid|ipc::kPathComplete;
    path.tracking_id=42; path.battle_serial=1; path.sampled_ms=1000; path.point_count=3;
    path.points[0]={100,100,ipc::kPathMoveTo};
    path.points[1]={200,100,0}; path.points[2]={200,200,0};
    return snapshot;
}
}
namespace arkhelp::gfx {
Surface::~Surface()=default;
void Surface::draw_line(float x,float y,float end_x,float end_y,const Color&,float width,int style) {
    lines.push_back({x,y,end_x,end_y,width,style});
}
}
int main() {
    using namespace arkhelp;
    try {
        auto snapshot=fixture(); OverlayOptions options; options.path_speed=100;
        features::PointerState hover_pointer{100,100,true,false},away{600,400,true,false};
        features::PathAnimation animation;
        auto step=[&](std::uint64_t now,const auto& pointer) { animation.update(snapshot,pointer,1,now,800,600,options); };
        step(1000,hover_pointer);
        check(animation.playbacks()[0].ready,"hover did not trigger route");
        step(1250,away);
        check(animation.playbacks()[0].id==42 && animation.playbacks()[0].progress==25,"leaving canceled or accelerated route");
        step(1251,hover_pointer);
        check(animation.playbacks()[0].progress>=25,"returning hover restarted unfinished animation");
        for(int t=1500;t<=3000;t+=250) step(t,away);
        check(animation.playbacks()[0].complete && animation.playbacks()[0].id==42,"animation did not finish after pointer left");
        step(3999,away); check(animation.playbacks()[0].id==42,"residue expired early");
        step(4000,away); check(animation.playbacks()[0].id==0,"residue did not expire after one second");
        snapshot=fixture(); options.path_on_click=true; animation.reset();
        step(1000,hover_pointer); check(!animation.playbacks()[0].id,"click mode triggered on hover");
        hover_pointer.clicked=true; step(1001,hover_pointer); hover_pointer.clicked=false;
        check(animation.playbacks()[0].ready,"click mode did not trigger on click");
        for(int t=1251;t<=3001;t+=250) step(t,hover_pointer);
        check(animation.playbacks()[0].complete,"click route failed to finish");
        step(5000,hover_pointer); check(animation.playbacks()[0].id==42,"hover did not hold completed route");
        step(5001,away); step(5500,hover_pointer); step(7000,hover_pointer);
        check(animation.playbacks()[0].id==42,"returning hover failed to reset residue clock");
        step(7001,away); step(8001,away); check(!animation.playbacks()[0].id,"new leave did not expire");
        options.path_on_click=false; animation.reset(); snapshot=fixture(); snapshot.path_count=0;
        step(1000,hover_pointer);
        check(animation.requests().size()==1 && animation.requests()[0]==42 && !animation.playbacks()[0].ready,
            "missing route was guessed or request lost");
        snapshot.path_count=1; step(1250,away); check(animation.playbacks()[0].ready,"delayed real route not accepted");
        snapshot.units[0].tracking_id=99; step(1300,away); check(!animation.playbacks()[0].id,"dead instance route retained");
        animation.reset(); snapshot=fixture(); snapshot.path_count=0;
        step(1000,hover_pointer); step(4100,hover_pointer);
        check(animation.playbacks()[0].id==42 && !animation.playbacks()[0].ready &&
            animation.requests().size()==1 && animation.requests()[0]==42,
            "continued hover lost its pending request after timeout");
        snapshot.path_count=1; step(4200,hover_pointer);
        check(!animation.playbacks()[0].ready,"late hover recovery accepted stale route");
        snapshot.paths[0].sampled_ms=4300; step(4300,hover_pointer);
        check(animation.playbacks()[0].ready,"fresh route did not recover after prolonged hover");
        step(4550,hover_pointer); check(animation.playbacks()[0].progress==25,
            "recovered hover route failed to advance normally");
        options.path_on_click=true; animation.reset(); snapshot=fixture(); snapshot.path_count=0;
        step(1000,hover_pointer); step(4100,hover_pointer);
        snapshot.path_count=1; snapshot.paths[0].sampled_ms=4200; step(4200,hover_pointer);
        check(!animation.playbacks()[0].id,"prolonged hover started click mode without a click");
        snapshot.path_count=0; hover_pointer.clicked=true; step(4300,hover_pointer); hover_pointer.clicked=false;
        step(7400,hover_pointer);
        check(animation.playbacks()[0].id==42 && !animation.playbacks()[0].ready &&
            animation.requests().size()==1 && animation.requests()[0]==42,
            "continued hover lost the original click pending request");
        snapshot.path_count=1; snapshot.paths[0].sampled_ms=7500; step(7500,hover_pointer);
        check(animation.playbacks()[0].ready,"late fresh route failed to fulfill the original click");
        animation.reset(); snapshot=fixture(); snapshot.path_count=0;
        hover_pointer.clicked=true; step(1000,hover_pointer); hover_pointer.clicked=false;
        step(4100,away);
        check(!animation.playbacks()[0].id && animation.requests().empty(),
            "abandoned pending click failed to expire");
        snapshot.path_count=1; snapshot.paths[0].sampled_ms=4200; step(4200,hover_pointer);
        check(!animation.playbacks()[0].id,"returning hover retriggered an expired click");
        hover_pointer.clicked=true; step(4201,hover_pointer); hover_pointer.clicked=false;
        check(animation.playbacks()[0].ready,"new click failed after an expired request");
        options.path_on_click=false;
        animation.reset(); snapshot=fixture(); snapshot.paths[0].battle_serial=2; step(1000,hover_pointer);
        check(!animation.playbacks()[0].ready,"other battle route accepted");
        animation.reset(); snapshot=fixture(); snapshot.paths[0].points[1].x=std::numeric_limits<float>::quiet_NaN(); step(1000,hover_pointer);
        check(!animation.playbacks()[0].ready,"nonfinite route accepted");
        animation.reset(); snapshot=fixture(); snapshot.paths[0].sampled_ms=1; step(2000,hover_pointer);
        check(!animation.playbacks()[0].ready,"stale route accepted");
        animation.reset(); snapshot=fixture(); step(1000,hover_pointer);
        animation.update(snapshot,away,1,1200,1200,700,options);
        check(!animation.playbacks()[0].id,"resize retained old projection");
        animation.reset(); snapshot=fixture(); step(1000,hover_pointer);
        animation.update(snapshot,hover_pointer,1,1200,1200,700,options);
        check(animation.playbacks()[0].id==42 && !animation.playbacks()[0].ready,
            "resize while hovered accepted old projected path");
        snapshot.paths[0].sampled_ms=1300;
        animation.update(snapshot,hover_pointer,1,1300,1200,700,options);
        check(animation.playbacks()[0].ready,"resize failed to accept newly projected path");
        snapshot=fixture(); snapshot.paths[0].points[2].flags=ipc::kPathMoveTo;
        check(features::PathAnimation::path_length(snapshot.paths[0])==100,"teleport was drawn as a route segment");
        check(features::pointer_near(hover_pointer,100,100,1) && !features::pointer_near(away,100,100,1),"hover distance filter failed");
        animation.reset(); snapshot=fixture(); options.path_on_click=false; step(1000,hover_pointer);
        for(int t=1250;t<=2000;t+=250) step(t,hover_pointer);
        snapshot.units[0].x=snapshot.units[0].ground_x=150; step(2001,away);
        check(animation.playbacks()[0].consumed==50,"current enemy position did not consume the already walked prefix");
        gfx::Surface surface; lines.clear(); features::draw_paths(surface,animation,1,{});
        check(!lines.empty() && lines.front().x==150 && std::all_of(lines.begin(),lines.end(),[](const auto& line){return line.x>=150&&line.end_x>=150;}),
            "production path drawing still emitted the consumed route behind the enemy");
        FeatureAppearance appearance;appearance.line_width=7;appearance.line_style=1;
        lines.clear();features::draw_paths(surface,animation,1.5f,{},appearance);
        check(!lines.empty() && lines.front().width==10.5f && lines.front().style==1,
            "path line width/style failed to reach the production renderer");
        snapshot.units[0].x=snapshot.units[0].ground_x=120; step(2002,away);
        check(animation.playbacks()[0].consumed==50,"animation moved consumed progress backwards to an old route prefix");
        snapshot.units[0].x=snapshot.units[0].ground_x=150;
        snapshot.paths[0].points[0]={150,100,ipc::kPathMoveTo}; snapshot.paths[0].sampled_ms=2100; step(2100,away);
        check(animation.playbacks()[0].path.points[0].x==150 && animation.playbacks()[0].consumed==0 &&
              !animation.requests().empty(),"visible playback did not refresh its real remaining route after hover left");
        animation.reset(); snapshot=fixture(); snapshot.unit_count=ipc::kMaxUnits; snapshot.path_count=ipc::kMaxPaths;
        const auto unit=snapshot.units[0];const auto path=snapshot.paths[0];
        for(std::uint32_t i=0;i<ipc::kMaxUnits;++i) {
            snapshot.units[i]=unit;snapshot.units[i].tracking_id=1000+i;
            snapshot.paths[i]=path;snapshot.paths[i].tracking_id=1000+i;
        }
        step(1000,hover_pointer);
        check(std::count_if(animation.playbacks().begin(),animation.playbacks().end(),[](const auto& item){return item.ready;})==ipc::kMaxUnits &&
              animation.requests().size()==ipc::kMaxUnits,"overlapping group did not trigger every live enemy at once");
        animation.reset();options.path_on_click=true;step(1000,hover_pointer);
        check(std::none_of(animation.playbacks().begin(),animation.playbacks().end(),[](const auto& item){return item.id;}),
              "group hover bypassed click mode");
        hover_pointer.clicked=true;step(1001,hover_pointer);hover_pointer.clicked=false;
        check(std::count_if(animation.playbacks().begin(),animation.playbacks().end(),[](const auto& item){return item.ready;})==ipc::kMaxUnits,
              "one real click did not trigger all overlapping enemies");
        animation.reset();snapshot=fixture();snapshot.unit_count=0;snapshot.path_count=2;snapshot.spawn_count=4;
        for(unsigned i=0;i<4;++i) {
            auto& spawn=snapshot.spawns[i];spawn.flags=ipc::kSpawnScreenValid;spawn.screen={100,100};
            spawn.route_index=i;spawn.planned_frame=i<2?300:600;spawn.remaining_seconds=i<2?10:20;
        }
        for(unsigned i=0;i<2;++i) {
            snapshot.paths[i]=path;snapshot.paths[i].tracking_id=ipc::spawn_path_id(i);
        }
        options.path_on_click=false;step(1000,hover_pointer);
        check(animation.requests().size()==2 && animation.playbacks()[0].ready && animation.playbacks()[1].ready &&
            animation.playbacks()[0].id==ipc::spawn_path_id(0) && animation.playbacks()[1].id==ipc::spawn_path_id(1),
            "red gate did not trigger all routes in its next batch, or included a later batch");
        step(1250,away);
        check(animation.playbacks()[0].progress==25 && animation.playbacks()[0].consumed==0,
            "red gate route was consumed like a moving enemy, or cancelled when pointer left");
        snapshot.spawn_count=0;
        for(int t=1500;t<=3000;t+=250)step(t,away);
        check(animation.playbacks()[0].complete,"red gate playback disappeared before drawing finished when enemies spawned");
        step(4000,away);check(!animation.playbacks()[0].id,"red gate residue did not expire");
        snapshot.spawn_count=4;animation.reset();options.path_on_click=true;step(1000,hover_pointer);
        check(!animation.playbacks()[0].id,"red gate hover bypassed click mode");
        hover_pointer.clicked=true;step(1001,hover_pointer);hover_pointer.clicked=false;
        check(animation.playbacks()[0].ready && animation.playbacks()[1].ready,"red gate click did not trigger the next batch");
        snapshot.spawns[0].flags|=ipc::kSpawnRandom;animation.reset();options.path_on_click=false;step(1000,hover_pointer);
        check(animation.requests().size()==1 && animation.requests()[0]==ipc::spawn_path_id(1),
            "red gate accepted an undetermined random spawn or a later batch");
        check(ipc::spawn_path_id(0)!=42 && ipc::is_spawn_path(ipc::spawn_path_id(65535)) &&
            !ipc::spawn_path_id(65536) && !ipc::is_spawn_path(ipc::kSpawnPathTag) && !ipc::is_spawn_path(42),
            "route target namespace or limits overlap ordinary instance IDs");
        std::printf("PASS path animation: %d checks; trigger, pending, completion, residue, click, identity, teleport, resize\n",checks);
    } catch(const std::exception& error) { std::fprintf(stderr,"FAIL %s\n",error.what()); return 1; }
}
