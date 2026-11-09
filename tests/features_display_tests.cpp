#include "../src/features/features.h"
#include "../src/data/battle_statistics.h"
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <cmath>

namespace {
struct Text { std::wstring value; float x,y,size; };
std::vector<Text> texts;
float range_width=0;int range_style=-1;
void check(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
}
// Record the production renderer's draw commands without creating a desktop window.
namespace arkhelp::gfx {
Surface::~Surface() = default;
bool Surface::resize(int width,int height) { width_=width; height_=height; return true; }
void Surface::draw_text(const std::wstring& text,float x,float y,float size,const Color&,TextAlign,bool) {
    texts.push_back({text,x,y,size});
}
void Surface::fill_rounded_rect(float,float,float,float,float,const Color&) {}
void Surface::stroke_rounded_rect(float,float,float,float,float,const Color&) {}
void Surface::draw_polygons(const D2D1_POINT_2F*,std::uint32_t,std::uint32_t,const Color&,const Color&,float width,int style) {
    range_width=width;range_style=style;
}
}

int main() {
    using namespace arkhelp;
    try {
        gfx::Surface surface; surface.resize(800,600);
        ipc::BattleSnapshot snapshot{}; snapshot.in_battle=1; snapshot.unit_count=1;
        snapshot.units[0].flags=ipc::kUnitValid|ipc::kUnitHpValid;
        snapshot.units[0].x=snapshot.units[0].y=200;
        snapshot.units[0].hp=snapshot.units[0].max_hp=100;
        OverlayOptions options; options.hp_offset_x=50; options.hp_offset_y=30; options.hp_only_hover=true;
        features::PointerState pointer{200,200,true,false},away{700,500,true,false};
        features::draw_hp_bars(surface,snapshot,1,false,false,13,options,pointer);
        check(texts.size()==1 && texts[0].x==250 && texts[0].y==192 && texts[0].size==13,
            "HP offset or font is not applied to actual rendered anchor");
        texts.clear(); features::draw_hp_bars(surface,snapshot,1,false,false,13,options,away);
        check(texts.empty(),"HP hover filter leaked labels");
        features::draw_hp_bars(surface,snapshot,1,true,false,13,options,pointer);
        check(texts.empty(),"HP pause filter ignored");
        snapshot.operator_count=1;
        auto& actor=snapshot.operators[0]; actor.flags=ipc::kDamageValid|ipc::kDamageScreenValid;
        actor.screen={200,200}; actor.metric_mask=1u<<ipc::kStatCost; actor.metrics[ipc::kStatCost]=10.5;
        options.operator_metric_mask=(1u<<ipc::kStatCost)|(1u<<ipc::kStatDamage);
        options.operator_order=L"cost,total"; options.operator_offset_x=25; options.operator_offset_y=-50;
        features::draw_operator_stats(surface,snapshot,1,16,options,pointer);
        check(texts.size()==4 && texts[0].value==L"有效回费" && texts[1].value==L"10.5" &&
            texts[2].value==L"总伤害" && texts[3].value==L"—","operator order, fractional cost or unsupported data lost");
        check(texts[0].x==225 && texts[0].y==150 && texts[2].y>texts[0].y && texts[1].x==texts[3].x,
            "operator label/value columns or offsets misaligned");
        texts.clear(); options.operator_only_hover=true;
        features::draw_operator_stats(surface,snapshot,1,16,options,away);
        check(texts.empty(),"operator hover filter leaked text");
        options.operator_only_paused=true;
        features::draw_operator_stats(surface,snapshot,1,16,options,pointer);
        check(texts.empty(),"operator pause filter ignored");
        snapshot.flags|=ipc::kBattlePaused; options.operator_metric_mask=(1u<<ipc::kStatCount)-1;
        features::draw_operator_stats(surface,snapshot,1,16,options,pointer);
        check(texts.size()==12,"operator renderer did not enforce six-metric cap");
        texts.clear(); actor.flags&=~ipc::kDamageScreenValid;
        features::draw_operator_stats(surface,snapshot,1,16,options,pointer);
        check(texts.empty(),"off-field operator was rendered in a guessed location");
        snapshot.operator_count=2;
        actor.flags|=ipc::kDamageScreenValid;actor.metric_mask=1u<<ipc::kStatDamage;
        actor.metrics[ipc::kStatDamage]=100;
        snapshot.operators[1]=actor;snapshot.operators[1].screen.x=500;
        snapshot.operators[1].metrics[ipc::kStatDamage]=60;
        options.operator_only_hover=false;options.operator_only_paused=false;
        options.operator_metric_mask=1u<<ipc::kStatDamage;options.operator_order=L"total";
        snapshot.damage_count=1;snapshot.damage[0].metrics[ipc::kStatDamage]=160;
        features::draw_operator_stats(surface,snapshot,1,16,options,pointer);
        check(texts.size()==4 && texts[1].value==L"100" && texts[3].value==L"60" && texts[0].x!=texts[2].x,
            "two instances must draw independent values at distinct anchors");
        texts.clear();
        snapshot.operators[1].flags |= ipc::kDamageDevice;
        snapshot.team_metrics[ipc::kStatDamage] = 160;
        snapshot.device_metrics[ipc::kStatDamage] = 60;
        snapshot.team_metrics[ipc::kStatCost] = 14;
        snapshot.device_metrics[ipc::kStatCost] = 4;
        const auto operators_only = statistics::team_metrics(snapshot, false);
        const auto with_devices = statistics::team_metrics(snapshot, true);
        check(operators_only[ipc::kStatDamage] == 100 && operators_only[ipc::kStatCost] == 10 &&
            operators_only[ipc::kStatShare] == 1 && with_devices[ipc::kStatDamage] == 160,
            "hidden devices must be removed from full totals, including rows beyond the IPC limit");
        options.operator_metric_mask = 1u << ipc::kStatShare;
        snapshot.operators[0].metric_mask |= 1u << ipc::kStatShare;
        snapshot.operators[1].metric_mask |= 1u << ipc::kStatShare;
        features::draw_operator_stats(surface,snapshot,1,16,options,pointer);
        check(texts.size()==2 && texts[1].value==L"100.0%", "default operator display included a device or its damage share denominator");
        texts.clear(); options.operator_show_devices = true;
        features::draw_operator_stats(surface,snapshot,1,16,options,pointer);
        check(texts.size()==4 && texts[1].value==L"62.5%" && texts[3].value==L"37.5%",
            "optional device display did not show separate instance metrics with consistent shares");
        check(!statistics::visible(snapshot.operators[1],false) && statistics::visible(snapshot.operators[1],true),
            "statistics device visibility filter failed");
        texts.clear();
        snapshot.battle_state=2;
        snapshot.units[0].tracking_id=11;
        snapshot.units[1]=snapshot.units[0];snapshot.units[1].tracking_id=12;snapshot.units[1].x=220;
        snapshot.unit_count=2;snapshot.enemy_count=2;
        for(int i=0;i<2;++i) {
            auto& enemy=snapshot.enemies[i];enemy.tracking_id=11+i;enemy.flags=ipc::kEnemyRuntime;
            enemy.stats_mask=ipc::kEnemyAtk|ipc::kEnemyDef;enemy.atk=100+i*100;enemy.def=20+i*10;
        }
        options.enemy_order=L"atk,hp,def";options.enemy_metric_mask=ipc::kEnemyAtk|ipc::kEnemyHp|ipc::kEnemyDef;
        options.enemy_offset_x=20;options.enemy_offset_y=-30;
        features::draw_enemy_stats(surface,snapshot,1,16,options,pointer);
        check(texts.size()==6 && texts[0].value==L"攻击力" && texts[1].value==L"100" && texts[3].value==L"100" &&
            texts[0].x==220 && texts[0].y==170 && texts[0].size==16 && texts[2].y>texts[0].y && texts[1].x==texts[3].x,
            "enemy metrics order, columns, font, offsets or nearest identity failed");
        texts.clear();features::draw_enemy_stats(surface,snapshot,1,16,options,{219,200,true,false});
        check(texts.size()==6 && texts[1].value==L"200","nearest enemy did not change with pointer");
        texts.clear();features::draw_enemy_stats(surface,snapshot,1,16,options,away);
        check(texts.empty(),"enemy stats appeared without hover");
        features::draw_enemy_stats(surface,snapshot,1,16,options,{200,200,false,false});
        check(texts.empty(),"invalid pointer rendered enemy attributes");
        // Missing details must not cause selection to jump to the farther enemy.
        snapshot.enemies[0].flags=ipc::kEnemyReference;
        features::draw_enemy_stats(surface,snapshot,1,16,options,pointer);
        check(texts.size()==6 && texts[1].value==L"—" && texts[3].value==L"100",
            "reference attributes leaked or missing detail skipped the nearest enemy");
        texts.clear();options.enemy_only_paused=true;snapshot.flags=0;
        features::draw_enemy_stats(surface,snapshot,1,16,options,pointer);
        check(texts.empty(),"enemy pause filter ignored");
        snapshot.flags=ipc::kBattlePaused;options.enemy_metric_mask=0x7ff;
        features::draw_enemy_stats(surface,snapshot,1,16,options,pointer);
        check(texts.size()==12,"enemy renderer exceeded the six-metric limit");
        texts.clear();snapshot.battle_state=3;
        features::draw_enemy_stats(surface,snapshot,1,16,options,pointer);
        check(texts.empty(),"enemy stats survived battle finish");
        snapshot.battle_state=2;snapshot.units[0].hp=0;
        features::draw_enemy_stats(surface,snapshot,1,16,options,pointer);
        check(texts.size()==12 && texts[1].value==L"200","dead nearest enemy blocked the live target");
        // Real range renderer must pass line controls into the compound contour draw.
        auto& unit=snapshot.units[1];unit.flags|=ipc::kUnitRangeValid|ipc::kUnitGroundValid;
        unit.ground_x=220;unit.ground_y=200;unit.range_radius=2;unit.range_point_count=ipc::kRangePointCount;
        for(std::uint32_t i=0;i<ipc::kRangePointCount;++i) {
            const double angle=i*6.283185307/ ipc::kRangePointCount;
            unit.range_points[i]={220+80*static_cast<float>(std::cos(angle)),200+40*static_cast<float>(std::sin(angle))};
        }
        FeatureAppearance style;style.line_width=5;style.line_style=2;
        features::draw_enemy_ranges(surface,snapshot,1.5f,style);
        check(range_width==7.5f && range_style==2,"range renderer lost scaled line width or style");
        std::puts("PASS production feature display: nearest hover-only enemy, live availability, six-item limits, offsets/font, pause/end gating and styled range outlines");
    } catch(const std::exception& error) { std::fprintf(stderr,"FAIL %s\n",error.what()); return 1; }
}
