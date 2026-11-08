#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <string>
#include <string_view>
#include <arkhelp/ipc.h>

namespace arkhelp::enemy_metrics {
struct Metric { const wchar_t* id; const wchar_t* label; std::uint32_t bit; };
inline constexpr std::array<Metric, 11> kMetrics = {{
    {L"hp",L"当前生命",ipc::kEnemyHp}, {L"max_hp",L"生命上限",ipc::kEnemyMaxHp},
    {L"atk",L"攻击力",ipc::kEnemyAtk}, {L"def",L"防御力",ipc::kEnemyDef},
    {L"resistance",L"法术抗性",ipc::kEnemyMagicResistance}, {L"move_speed",L"移动速度",ipc::kEnemyMoveSpeed},
    {L"attack_speed",L"攻击速度",ipc::kEnemyAttackSpeed}, {L"attack_interval",L"攻击间隔",ipc::kEnemyBaseAttackTime},
    {L"weight",L"重量",ipc::kEnemyWeight}, {L"block",L"阻挡数",ipc::kEnemyBlockCount},
    {L"life_point",L"漏怪扣血",ipc::kEnemyLifePointReduce}
}};
inline constexpr std::uint32_t kAll = (1u << kMetrics.size()) - 1;
inline std::array<int, kMetrics.size()> parse_order(std::wstring_view text) {
    std::array<int,kMetrics.size()> order{};
    std::size_t count=0, start=0;
    const auto add=[&](int metric) {
        if(std::find(order.begin(),order.begin()+count,metric)==order.begin()+count) order[count++]=metric;
    };
    while(start<text.size()) {
        const auto end=text.find(L',',start);
        const auto token=text.substr(start,end==text.npos?text.size()-start:end-start);
        for(std::size_t i=0;i<kMetrics.size();++i) if(token==kMetrics[i].id) add(static_cast<int>(i));
        if(end==text.npos) break;
        start=end+1;
    }
    for(std::size_t i=0;i<kMetrics.size();++i) add(static_cast<int>(i));
    return order;
}
inline std::wstring format(const ipc::EnemyEntry& enemy, int metric) {
    if(metric<0 || metric>=static_cast<int>(kMetrics.size()) || !(enemy.stats_mask&kMetrics[metric].bit)) return L"—";
    const std::int64_t values[]={enemy.hp,enemy.max_hp,enemy.atk,enemy.def,enemy.magic_resistance,enemy.move_speed,
        enemy.attack_speed,enemy.base_attack_time,enemy.weight,enemy.block_cnt,enemy.life_point_reduce};
    if(metric==4 || metric==5 || metric==7 || metric==8) {
        wchar_t text[64]{};std::swprintf(text,64,L"%.2f",values[metric]/1000.0);return text;
    }
    return std::to_wstring(values[metric]);
}
} // namespace arkhelp::enemy_metrics
