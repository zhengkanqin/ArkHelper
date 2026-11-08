#pragma once
#include <string_view>
#include "../config/config.h"

namespace arkhelp {
inline std::uint32_t wiki_field_bit(std::wstring_view key) {
    if (key == L"名称") return kWikiName;
    if (key == L"数值来源") return kWikiSource;
    if (key == L"当前生命") return kWikiHp;
    if (key == L"生命上限" || key == L"最大生命值" || key == L"生命值") return kWikiMaxHp;
    if (key == L"攻击力") return kWikiAtk;
    if (key == L"防御力") return kWikiDef;
    if (key == L"法术抗性") return kWikiResistance;
    if (key == L"移动速度") return kWikiMoveSpeed;
    if (key == L"攻击间隔" || key == L"基础攻击间隔") return kWikiAttackInterval;
    if (key == L"重量等级" || key == L"重量") return kWikiWeight;
    if (key == L"阻挡数") return kWikiBlock;
    if (key == L"漏怪数" || key == L"目标价值") return kWikiLifePoint;
    return 0;
}

inline bool wiki_prts_field_visible(std::wstring_view key, std::uint32_t mask, bool level) {
    const auto bit = wiki_field_bit(key);
    if (bit && !(mask & bit)) return false;
    const bool ability = key.find(L"技能") != key.npos || key.find(L"天赋") != key.npos ||
        key.find(L"描述") != key.npos || key.find(L"特性") != key.npos ||
        key.find(L"能力") != key.npos || key.find(L"免疫") != key.npos;
    return (mask & (ability ? kWikiPrtsAbilities : level ? kWikiPrtsStats : kWikiPrtsBasic)) != 0;
}
} // namespace arkhelp
