#include <windows.h>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

#include "../src/config/config.h"
#include "../src/data/battle_statistics.h"
#include "../src/data/wiki_preferences.h"
#include "../src/data/enemy_metrics.h"
#include "../src/net/cache_directory.h"
#include "../native/modules/damage_meter/csv_export.h"

namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary); file << text;
}
std::string read(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file),{});
}

void configuration(const std::filesystem::path& directory) {
    const auto path = directory / L"统计 配置.json";
    write(path, R"({"version":1,"damage_order":"heal,physical,total,arts,true,element",
        "damage_metric_mask":5,"damage_sort_metric":1,"damage_panel_width":371,"damage_merged":1})");
    arkhelp::Config config;
    check(arkhelp::config_load(path.wstring(),config),"old configuration did not load");
    check(config.version==arkhelp::kConfigVersion,"configuration version was not migrated");
    check(config.auto_launch,"legacy unused launch preference was not migrated");
    check(!config.range_only_paused,"old configuration must default the new range pause option to off");
    check(config.damage_metric_mask==((1u<<arkhelp::ipc::kStatHeal)|(1u<<arkhelp::ipc::kStatDamage)|
          (1u<<arkhelp::ipc::kStatCost)),"old slot mask lost its original metric identity");
    check(config.damage_sort_metric==arkhelp::ipc::kStatHeal+1,"sort selection changed its metric");
    check(!config.damage_merged && config.damage_panel_width==0,"legacy cramped layout survived migration");
    check(config.damage_compact,"old configuration must use compact table spacing by default");
    config.damage_compact=false;
    arkhelp::feature_appearance(config,arkhelp::ipc::kFeatureDamageNumbers).click_through=true;
    const auto expected_mask=config.damage_metric_mask;
    config.range_only_paused=true;
    config.hp_only_paused=false;
    config.spawn_only_paused=true;
    config.auto_launch=false;
    config.menu_opacity=63;
    config.wiki_fields=arkhelp::kWikiName|arkhelp::kWikiHp|arkhelp::kWikiPrtsAbilities;
    for (std::size_t index=0;index<config.appearances.size();++index)
        config.appearances[index]={static_cast<int>(25+index*8),static_cast<int>(12+index*2),
                                   static_cast<std::int32_t>(index*0x20304)};
    arkhelp::feature_appearance(config,arkhelp::ipc::kFeatureDamageNumbers).click_through=true;
    config.damage_order=L"cost,heal,total,physical,arts,true,element,buildup,taken,shield,dps,share";
    config.prts_fields=arkhelp::kWikiName|arkhelp::kWikiPrtsStats;
    config.prts_panel_x=430;config.prts_panel_y=250;config.prts_panel_width=510;config.prts_panel_height=460;
    config.menu_features &= ~arkhelp::ipc::kFeatureHpBars;
    std::swap(config.feature_order[0],config.feature_order[8]);
    config.overlay.hp_offset_x=-120; config.overlay.hp_offset_y=200;
    config.overlay.hp_only_hover=true; config.overlay.operator_only_hover=true;
    config.overlay.operator_only_paused=true; config.overlay.operator_offset_x=75;
    config.overlay.operator_offset_y=-90; config.overlay.operator_metric_mask=(1u<<arkhelp::ipc::kStatCost);
    config.overlay.operator_order=L"cost,heal,total,physical,arts,true,element,buildup,taken,shield,dps,share";
    config.overlay.path_on_click=true; config.overlay.path_speed=720; config.overlay.path_linger_ms=2500;
    config.prts_cache_mb=10;
    config.menu_height=688;
    config.prts_cache_path=(directory/L"PRTS 缓存 自定义").wstring();
    config.overlay.enemy_only_paused=true;config.overlay.enemy_offset_x=-85;config.overlay.enemy_offset_y=65;
    config.overlay.enemy_metric_mask=arkhelp::ipc::kEnemyHp|arkhelp::ipc::kEnemyAtk|arkhelp::ipc::kEnemyWeight;
    config.overlay.enemy_order=L"weight,hp,atk,max_hp,def,resistance,move_speed,attack_speed,attack_interval,block,life_point";
    arkhelp::feature_appearance(config,arkhelp::ipc::kFeatureEnemyRange).line_width=7;
    arkhelp::feature_appearance(config,arkhelp::ipc::kFeatureEnemyRange).line_style=1;
    arkhelp::feature_appearance(config,arkhelp::ipc::kFeatureEnemyPaths).line_width=3;
    arkhelp::feature_appearance(config,arkhelp::ipc::kFeatureEnemyPaths).line_style=2;
    check(arkhelp::config_save(path.wstring(),config),"configuration save failed");
    arkhelp::Config restored;
    check(arkhelp::config_load(path.wstring(),restored),"saved configuration did not reload");
    check(!restored.damage_compact && arkhelp::feature_appearance(restored,arkhelp::ipc::kFeatureDamageNumbers).click_through &&
        !arkhelp::feature_appearance(restored,arkhelp::ipc::kFeatureWiki).click_through,
        "compact spacing and per-panel click-through must persist independently");
    check(restored.damage_metric_mask==expected_mask && restored.damage_sort_metric==config.damage_sort_metric,
          "reordering columns changed visibility or sorting");
    check(restored.damage_order==config.damage_order,"column order was not persisted");
    check(restored.menu_height==688,"dragged menu height did not persist");
    check(restored.prts_cache_path==config.prts_cache_path && restored.overlay.enemy_only_paused &&
        restored.overlay.enemy_offset_x==-85 && restored.overlay.enemy_offset_y==65 &&
        restored.overlay.enemy_metric_mask==config.overlay.enemy_metric_mask && restored.overlay.enemy_order==config.overlay.enemy_order,
        "enemy attributes or Unicode cache directory did not persist");
    check(restored.overlay.hp_offset_x==-120 && restored.overlay.hp_offset_y==200 && restored.overlay.hp_only_hover &&
        restored.overlay.operator_only_hover && restored.overlay.operator_only_paused && restored.overlay.operator_offset_x==75 &&
        restored.overlay.operator_offset_y==-90 && restored.overlay.operator_metric_mask==config.overlay.operator_metric_mask &&
        restored.overlay.operator_order==config.overlay.operator_order && restored.overlay.path_on_click &&
        restored.overlay.path_speed==720 && restored.overlay.path_linger_ms==2500 && restored.prts_cache_mb==10,
        "new drawing or persistent cache settings did not round trip");
    check(restored.range_only_paused && restored.spawn_only_paused && !restored.hp_only_paused,
          "independent pause preferences were not persisted");
    check(!restored.auto_launch && restored.menu_opacity==63,"startup opt-out or menu opacity did not persist");
    check(restored.wiki_fields==config.wiki_fields && restored.appearances==config.appearances,
          "independent feature styles or hidden WIKI fields did not persist");
    check(restored.prts_fields==config.prts_fields && restored.prts_panel_x==430 && restored.prts_panel_y==250 &&
          restored.prts_panel_width==510 && restored.prts_panel_height==460 &&
          restored.menu_features==config.menu_features && !(restored.features&arkhelp::ipc::kFeatureHpBars) &&
          restored.feature_order==config.feature_order,"independent PRTS or hidden-feature menu preferences did not persist");
    const auto normalized=arkhelp::parse_feature_order(L"2048,2048,999,4");
    check(normalized[0]==arkhelp::ipc::kFeatureEnemyPrts && normalized[1]==arkhelp::ipc::kFeatureDamageNumbers &&
          std::count(normalized.begin(),normalized.end(),arkhelp::ipc::kFeatureEnemyPrts)==1,
          "duplicate or obsolete feature order entries were not normalized");
    for (std::size_t index=0;index<arkhelp::kAppearanceFeatures.size();++index)
        check(arkhelp::feature_appearance(restored,arkhelp::kAppearanceFeatures[index])==config.appearances[index],
              "feature style identity changed");
    check(!arkhelp::wiki_prts_field_visible(L"攻击力",restored.wiki_fields,true) &&
          arkhelp::wiki_prts_field_visible(L"特殊技能",restored.wiki_fields,false) &&
          !arkhelp::wiki_prts_field_visible(L"描述",0,false),"WIKI field filters do not cover PRTS content");
    write(path,R"({"version":2,"damage_order":"heal,heal,invalid","damage_metric_mask":0})");
    check(arkhelp::config_load(path.wstring(),restored),"malformed preferences did not recover");
    check(restored.damage_metric_mask==arkhelp::statistics::kCommonMetrics,"empty metric mask was not recovered");
    const auto order=arkhelp::statistics::parse_order(restored.damage_order);
    check(order.size()==arkhelp::ipc::kStatCount,"missing columns were not restored");
    write(path,R"({"version":3,"auto_launch":0,"menu_opacity":-8,"wiki_fields":0,
        "timer_opacity":999,"timer_font_size":99,"timer_text_color":16777216,
        "wiki_opacity":-8,"wiki_font_size":0,"wiki_text_color":0})");
    check(arkhelp::config_load(path.wstring(),restored),"invalid appearance preferences did not recover");
    check(!restored.auto_launch && restored.wiki_fields==0 && restored.menu_opacity==20,
          "explicit startup opt-out or all-hidden WIKI preference was lost");
    check(restored.appearances[0]==arkhelp::FeatureAppearance{100,32,-1} &&
          restored.appearances[4]==arkhelp::FeatureAppearance{20,0,0},"appearance bounds or custom black handling failed");
    write(path,R"({"version":3,"operator_metric_mask":4095,"operator_order":"cost,heal,total", "hp_offset_x":999,
        "hp_offset_y":-999,"operator_offset_y":999,"path_speed":-7,"path_linger_ms":20000,"prts_cache_mb":1})");
    check(arkhelp::config_load(path.wstring(),restored),"new bounds fixture failed");
    check(std::popcount(restored.overlay.operator_metric_mask)==6 &&
        (restored.overlay.operator_metric_mask&(1u<<arkhelp::ipc::kStatCost)) && restored.overlay.hp_offset_x==200 &&
        restored.overlay.hp_offset_y==-200 && restored.overlay.operator_offset_y==200 && restored.overlay.path_speed==50 &&
        restored.overlay.path_linger_ms==10000 && restored.prts_cache_mb==10,"new drawing bounds or six-metric limit failed");
    write(path,R"({"version":4,"menu_features":2049,"features":2049,"feature_order":"2048,1",
        "range_line_width":99,"range_line_style":99,"path_line_width":-1,"path_line_style":-1,
        "enemy_offset_x":900,"enemy_offset_y":-900,"enemy_metric_mask":2047,"enemy_order":"weight,atk"})");
    check(arkhelp::config_load(path.wstring(),restored),"v4 migration failed");
    check(restored.menu_features==(2049|arkhelp::ipc::kFeatureEnemyStats) && restored.features==2049 &&
        restored.feature_order[0]==arkhelp::ipc::kFeatureEnemyPrts && restored.feature_order[1]==arkhelp::ipc::kFeatureSpawnTimer &&
        restored.feature_order.back()==arkhelp::ipc::kFeatureEnemyStats && restored.version==arkhelp::kConfigVersion,
        "new feature migration changed existing visibility, order or activation");
    check(arkhelp::feature_appearance(restored,arkhelp::ipc::kFeatureEnemyRange).line_width==8 &&
        arkhelp::feature_appearance(restored,arkhelp::ipc::kFeatureEnemyRange).line_style==2 &&
        arkhelp::feature_appearance(restored,arkhelp::ipc::kFeatureEnemyPaths).line_width==1 &&
        arkhelp::feature_appearance(restored,arkhelp::ipc::kFeatureEnemyPaths).line_style==0 &&
        std::popcount(restored.overlay.enemy_metric_mask)==6 && (restored.overlay.enemy_metric_mask&arkhelp::ipc::kEnemyWeight) &&
        restored.overlay.enemy_offset_x==200 && restored.overlay.enemy_offset_y==-200,"enemy/line bounds failed");
    const auto custom=arkhelp::prts_cache_directory(directory.wstring(),config.prts_cache_path);
    check(arkhelp::prepare_cache_directory(custom).empty() && std::filesystem::is_empty(custom),
        "cache path writable probe failed or left a temporary file");
    check(!arkhelp::prepare_cache_directory(path).empty(),"file accepted as a cache directory");
    check(arkhelp::prts_cache_directory(directory.wstring(),L"")==directory/L"cache"/L"prts" &&
        arkhelp::prts_cache_directory(directory.wstring(),L"relative cache")==directory/L"relative cache",
        "default/relative cache directory resolved against the wrong root");
    write(path,R"({"version":5,"menu_height":12,"feature_order":"1,128,4,8,64,256,512,1024,2048,4096"})");
    check(arkhelp::config_load(path.wstring(),restored) && restored.menu_height==400 && restored.feature_order==arkhelp::kDefaultFeatureOrder,
        "old default order was not reorganized or menu minimum failed");
    write(path,R"({"version":5,"menu_height":9000,"feature_order":"2048,4,1"})");
    check(arkhelp::config_load(path.wstring(),restored) && restored.menu_height==900 && restored.feature_order[0]==arkhelp::ipc::kFeatureEnemyPrts,
        "custom order was overwritten or menu maximum failed");
    std::filesystem::remove(custom);std::filesystem::remove(path);
}

void csv_report(const std::filesystem::path& directory) {
    using namespace ark::damage_meter;
    BattleStats stats;
    ArkEventV1 event; event.kind=ARK_BATTLE_START; event.battle=7; stats.consume(event);
    const auto emit=[&](std::uint32_t kind,const char* id,double value,int type=0) {
        ArkEventV1 e; e.kind=kind;e.game_time=10;e.damage_type=type;
        std::snprintf(e.actor_id,sizeof(e.actor_id),"%s",id);
        e.actual_damage=e.calculated_damage=value;stats.consume(e);
    };
    emit(ARK_DAMAGE,"damage",100,ARK_DAMAGE_PHYSICAL);
    emit(ARK_HEAL,"healer",80);
    emit(ARK_COST,"vanguard",14);
    emit(ARK_TAKEN,"damage",50);
    std::vector<const ActorRow*> rows;stats.sort_rows(rows,true);
    const auto utf8=directory.u8string();
    const auto result=write_csv_report(reinterpret_cast<const char*>(utf8.c_str()),stats,rows,{});
    check(result.has_value(),"CSV report was not written");
    const auto path=std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(result->data()),result->size()));
    const auto text=read(path);
    check(text.starts_with("\xEF\xBB\xBF"),"CSV is missing its UTF-8 BOM");
    check(text.find("有效治疗,有效回费,承伤")!=std::string::npos,"CSV does not include support columns");
    check(text.find("全队合计")!=std::string::npos,"CSV is missing the all-actor team total");
    check(text.find("healer,healer,")!=std::string::npos,"healing-only operator was omitted");
    check(text.find("vanguard,vanguard,")!=std::string::npos,"cost-only operator was omitted");
    check(text.find(",80.0000,14.0000,50.0000,100.0000,")!=std::string::npos,
          "CSV team total does not preserve independent damage, healing, cost and taken quantities");
    std::filesystem::remove(path);std::filesystem::remove(directory/"exports");
}
}

int wmain(int argc, wchar_t** argv) {
    const auto directory=std::filesystem::absolute(argv[0]).parent_path() /
        (L"统计 持久化测试-"+std::to_wstring(GetCurrentProcessId()));
    try {
        std::filesystem::create_directories(directory);
        configuration(directory);std::puts("PASS statistics configuration migration and persistence");
        csv_report(directory);std::puts("PASS complete CSV support metrics and team totals");
        std::filesystem::remove(directory);
    } catch(const std::exception& error) {
        std::fprintf(stderr,"FAIL %s\n",error.what());return 1;
    }
    return 0;
}
