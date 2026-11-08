#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <string>
#include <vector>
#include <arkhelp/ipc.h>

namespace arkhelp::statistics {

struct Metric { const wchar_t* id; const wchar_t* label; };
inline constexpr Metric kMetrics[ipc::kStatCount] = {
    {L"total", L"总伤害"}, {L"physical", L"物理"}, {L"arts", L"法术"},
    {L"true", L"真实"}, {L"heal", L"有效治疗"}, {L"element", L"元素伤害"},
    {L"cost", L"有效回费"}, {L"buildup", L"元素积累"}, {L"taken", L"承伤"},
    {L"shield", L"护盾吸收"}, {L"dps", L"DPS"}, {L"share", L"伤害占比"}
};
inline constexpr std::uint32_t kAllMetrics = (1u << ipc::kStatCount) - 1u;
inline constexpr std::uint32_t kCommonMetrics = (1u << ipc::kStatDamage) | (1u << ipc::kStatHeal) |
    (1u << ipc::kStatCost) | (1u << ipc::kStatDps) | (1u << ipc::kStatShare);

inline std::vector<int> parse_order(const std::wstring& text) {
    std::vector<int> result;
    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        const auto sep = text.find(L',', cursor);
        const auto token = text.substr(cursor, sep == std::wstring::npos ? sep : sep - cursor);
        for (int i = 0; i < static_cast<int>(ipc::kStatCount); ++i) {
            if (token == kMetrics[i].id && std::find(result.begin(), result.end(), i) == result.end())
                result.push_back(i);
        }
        if (sep == std::wstring::npos) break;
        cursor = sep + 1;
    }
    for (int i = 0; i < static_cast<int>(ipc::kStatCount); ++i)
        if (std::find(result.begin(), result.end(), i) == result.end()) result.push_back(i);
    return result;
}

inline std::wstring order_string(const std::vector<int>& order) {
    std::wstring result;
    for (int index : order) {
        if (!result.empty()) result += L',';
        result += kMetrics[index].id;
    }
    return result;
}

inline double value(const ipc::DamageEntry& row, int metric) {
    const double number = row.metrics[metric];
    return std::isfinite(number) && number > 0 ? number : 0.0;
}

inline std::wstring format(double value, int metric, bool exact = false) {
    wchar_t buffer[80]{};
    if (metric == ipc::kStatShare) {
        std::swprintf(buffer, 80, L"%.1f%%", value * 100.0);
    } else if (metric == ipc::kStatCost) {
        std::swprintf(buffer, 80, std::abs(value - std::round(value)) < .001 ? L"%.0f" : L"%.1f", value);
    } else if (!exact && value >= 1000000) {
        std::swprintf(buffer, 80, L"%.2fM", value / 1000000.0);
    } else if (!exact && value >= 10000) {
        std::swprintf(buffer, 80, L"%.1fk", value / 1000.0);
    } else {
        std::swprintf(buffer, 80, metric == ipc::kStatDps ? L"%.1f" : L"%.0f", value);
    }
    return buffer;
}

} // namespace arkhelp::statistics
