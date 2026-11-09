#include "config.h"
#include "../data/battle_statistics.h"
#include "../data/enemy_metrics.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <algorithm>

#include "../shell/platform.h"

namespace arkhelp
{
namespace
{

constexpr size_t kMaxConfigBytes = 64u * 1024u;

bool read_all_text(const std::wstring& path, std::string& out)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > static_cast<LONGLONG>(kMaxConfigBytes))
    {
        CloseHandle(file);
        return false;
    }

    out.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < out.size())
    {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(out.size() - offset);
        if (!ReadFile(file, out.data() + offset, chunk, &read, nullptr) || read == 0)
        {
            break;
        }
        offset += read;
    }
    CloseHandle(file);
    out.resize(offset);
    return offset > 0;
}

bool find_boolean(const std::string& text, const char* key, bool& result)
{
    const std::string needle = std::string("\"") + key + "\"";
    size_t position = text.find(needle);
    if (position == std::string::npos)
    {
        return false;
    }
    position = text.find(':', position + needle.size());
    if (position == std::string::npos)
    {
        return false;
    }
    ++position;
    while (position < text.size() && (text[position] == ' ' || text[position] == '\t' ||
                                      text[position] == '\r' || text[position] == '\n'))
    {
        ++position;
    }
    if (text.compare(position, 4, "true") == 0)
    {
        result = true;
        return true;
    }
    if (text.compare(position, 5, "false") == 0)
    {
        result = false;
        return true;
    }
    return false;
}

bool find_integer(const std::string& text, const char* key, int& result)
{
    const std::string needle = std::string("\"") + key + "\"";
    size_t position = text.find(needle);
    if (position == std::string::npos)
    {
        return false;
    }
    position = text.find(':', position + needle.size());
    if (position == std::string::npos)
    {
        return false;
    }
    ++position;
    while (position < text.size() && (text[position] == ' ' || text[position] == '\t' ||
                                      text[position] == '\r' || text[position] == '\n'))
    {
        ++position;
    }
    bool negative = false;
    if (position < text.size() && text[position] == '-')
    {
        negative = true;
        ++position;
    }
    if (position >= text.size() || !std::isdigit(static_cast<unsigned char>(text[position])))
    {
        return false;
    }
    long long value = 0;
    while (position < text.size() && std::isdigit(static_cast<unsigned char>(text[position])))
    {
        value = value * 10 + (text[position] - '0');
        if (value > 100000000LL)
        {
            value = 100000000LL;  // 配置值不可能这么大，钳制以防止溢出
        }
        ++position;
    }
    result = static_cast<int>(negative ? -value : value);
    return true;
}

// 读取 "key": "string" 形式的字段；支持 \\ 与 \" 两种转义（写回时由 config_save 生成）。
bool find_string(const std::string& text, const char* key, std::wstring& result)
{
    const std::string needle = std::string("\"") + key + "\"";
    size_t position = text.find(needle);
    if (position == std::string::npos)
    {
        return false;
    }
    position = text.find(':', position + needle.size());
    if (position == std::string::npos)
    {
        return false;
    }
    ++position;
    while (position < text.size() && (text[position] == ' ' || text[position] == '\t' ||
                                      text[position] == '\r' || text[position] == '\n'))
    {
        ++position;
    }
    if (position >= text.size() || text[position] != '"')
    {
        return false;
    }
    ++position;

    std::string utf8;
    while (position < text.size() && text[position] != '"')
    {
        char ch = text[position];
        if (ch == '\\' && position + 1 < text.size())
        {
            const char next = text[position + 1];
            if (next == '\\' || next == '"')
            {
                ch = next;
                position += 2;
            }
            else
            {
                utf8.push_back(ch);
                ++position;
                continue;
            }
        }
        else
        {
            ++position;
        }
        utf8.push_back(ch);
        if (utf8.size() > 4096)
        {
            return false;
        }
    }
    if (position >= text.size())
    {
        return false;
    }

    if (utf8.empty())
    {
        result.clear();
        return true;
    }
    const int wide_length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wide_length <= 0)
    {
        return false;
    }
    result.resize(static_cast<size_t>(wide_length));
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), result.data(), wide_length);
    return true;
}

// 把宽字符串写成 JSON 字符串内容（不含引号），路径中的反斜杠需要转义。
std::string escape_json_string(const std::wstring& value)
{
    std::string utf8;
    if (!value.empty())
    {
        const int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0,
                                               nullptr, nullptr);
        if (length > 0)
        {
            utf8.resize(static_cast<size_t>(length));
            WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), utf8.data(), length,
                                nullptr, nullptr);
        }
    }
    std::string escaped;
    escaped.reserve(utf8.size() * 2);
    for (const char ch : utf8)
    {
        if (ch == '\\' || ch == '"')
        {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

bool write_all(HANDLE file, const char* data, size_t size)
{
    while (size > 0)
    {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(size);
        if (!WriteFile(file, data, chunk, &written, nullptr) || written == 0)
        {
            return false;
        }
        data += written;
        size -= written;
    }
    return true;
}

}  // namespace

Config config_defaults()
{
    return Config();
}

FeatureOrder parse_feature_order(const std::wstring& text)
{
    FeatureOrder result{};
    std::size_t count = 0;
    const auto add = [&](std::uint32_t bit) {
        if (count < result.size() && std::find(kAppearanceFeatures.begin(), kAppearanceFeatures.end(), bit) != kAppearanceFeatures.end() &&
            std::find(result.begin(), result.begin() + count, bit) == result.begin() + count) result[count++] = bit;
    };
    std::uint32_t value = 0;
    for (wchar_t ch : text + L",") {
        if (ch >= L'0' && ch <= L'9') value = std::min(100000u, value * 10 + ch - L'0');
        else { add(value); value = 0; }
    }
    for (auto bit : kAppearanceFeatures) add(bit);
    return result;
}

std::wstring feature_order_string(const FeatureOrder& order)
{
    std::wstring result;
    for (auto bit : order) { if (!result.empty()) result += L','; result += std::to_wstring(bit); }
    return result;
}

bool config_load(const std::wstring& path, Config& out)
{
    out = config_defaults();

    std::string text;
    if (!read_all_text(path, text))
    {
        return false;
    }

    int value = 0;
    if (find_integer(text, "risk_notice_version", value) && value > 0)
        out.risk_notice_version = value;
    if (find_integer(text, "version", value) && value > 0)
    {
        out.version = value;
    }
    if (find_integer(text, "ball_x", value))
    {
        out.ball_x = value;
    }
    if (find_integer(text, "ball_y", value))
    {
        out.ball_y = value;
    }
    if (find_integer(text, "edge", value))
    {
        out.edge = value == static_cast<int>(DockEdge::kLeft) || value == static_cast<int>(DockEdge::kRight) ? value
                                                                                                           : 0;
    }
    if (find_integer(text, "features", value) && value >= 0)
    {
        out.features = static_cast<uint32_t>(value);
    }
    // 「出怪卡面」开关已移除：旧配置里残留的该位直接清掉，避免空开关影响后续逻辑。
    out.features &= ~(1u << 1);
    if (find_integer(text, "opacity", value))
    {
        // 透明度限制在 40..100：更低会几乎看不见，失去可点性。
        out.opacity = value < 40 ? 40 : (value > 100 ? 100 : value);
    }
    if (find_integer(text, "spawn_only_paused", value))
    {
        out.spawn_only_paused = value != 0;
    }
    if (find_integer(text, "hp_only_paused", value))
    {
        out.hp_only_paused = value != 0;
    }
    if (find_integer(text, "range_only_paused", value))
    {
        out.range_only_paused = value != 0;
    }
    if (find_integer(text, "menu_opacity", value)) out.menu_opacity = std::clamp(value, 20, 100);
    if (find_integer(text, "menu_height", value)) out.menu_height = std::clamp(value, 400, 900);
    if (find_integer(text, "wiki_fields", value)) out.wiki_fields = static_cast<std::uint32_t>(value) & kWikiAllFields;
    const char* prefixes[] = {"timer", "spawn", "damage", "hp", "wiki", "range", "operator", "path", "prts", "enemy_stats"};
    for (std::size_t index = 0; index < out.appearances.size(); ++index) {
        auto& appearance = out.appearances[index];
        const std::string prefix = std::string(prefixes[index]) + "_";
        if (find_integer(text, (prefix + "opacity").c_str(), value)) appearance.opacity = std::clamp(value, 20, 100);
        if (find_integer(text, (prefix + "font_size").c_str(), value)) appearance.font_size = value == 0 ? 0 : std::clamp(value, 10, 32);
        if (find_integer(text, (prefix + "text_color").c_str(), value)) appearance.text_color = value >= 0 && value <= 0xffffff ? value : -1;
        if (find_integer(text, (prefix + "line_width").c_str(), value)) appearance.line_width = std::clamp(value, 1, 8);
        if (find_integer(text, (prefix + "line_style").c_str(), value)) appearance.line_style = std::clamp(value, 0, 2);
        if (find_integer(text, (prefix + "click_through").c_str(), value)) appearance.click_through = value != 0;
    }
    if (find_integer(text, "ui_scale", value))
    {
        const bool allowed = value == 80 || value == 100 || value == 120 || value == 150;
        out.ui_scale = allowed ? value : 100;
    }
    if (find_integer(text, "auto_launch", value))
    {
        out.auto_launch = value != 0;
    }
    find_string(text, "game_path", out.game_path);
    find_string(text, "loader_path", out.loader_path);
    if (find_integer(text, "damage_panel_x", value))
    {
        out.damage_panel_x = value;
    }
    if (find_integer(text, "damage_panel_y", value))
    {
        out.damage_panel_y = value;
    }
    if (find_integer(text, "damage_panel_collapsed", value))
    {
        out.damage_panel_collapsed = value != 0;
    }
    if (find_integer(text, "spawn_panel_x", value))
    {
        out.spawn_panel_x = value;
    }
    if (find_integer(text, "spawn_panel_y", value))
    {
        out.spawn_panel_y = value;
    }
    if (find_integer(text, "spawn_panel_collapsed", value))
    {
        out.spawn_panel_collapsed = value != 0;
    }
    if (find_integer(text, "damage_panel_height", value))
    {
        out.damage_panel_height = value > 0 ? value : 0;
    }
    if (find_integer(text, "spawn_panel_height", value))
    {
        out.spawn_panel_height = value > 0 ? value : 0;
    }
    if (find_integer(text, "wiki_panel_height", value))
    {
        out.wiki_panel_height = value > 0 ? value : 0;
    }
    if (find_integer(text, "damage_panel_width", value))
    {
        out.damage_panel_width = value > 0 ? value : 0;
    }
    if (find_integer(text, "spawn_panel_width", value))
    {
        out.spawn_panel_width = value > 0 ? value : 0;
    }
    if (find_integer(text, "wiki_panel_width", value))
    {
        out.wiki_panel_width = value > 0 ? value : 0;
    }
    if (find_integer(text, "damage_metric_mask", value))
    {
        out.damage_metric_mask = static_cast<uint32_t>(value) & statistics::kAllMetrics;
    }
    if (find_integer(text, "damage_compact", value)) out.damage_compact = value != 0;
    if (find_integer(text, "damage_merged", value))
    {
        out.damage_merged = value != 0;
    }
    else if (bool boolean_value = false; find_boolean(text, "damage_merged", boolean_value))
    {
        out.damage_merged = boolean_value;
    }
    if (find_integer(text, "damage_sort_metric", value))
    {
        out.damage_sort_metric = value >= 0 && value <= static_cast<int>(ipc::kStatCount) ? value : 0;
    }
    find_string(text, "damage_order", out.damage_order);
    if (out.version < 2) {
        // Old masks and sorting referred to column slots, so reordering also changed their meaning.
        const auto order = statistics::parse_order(out.damage_order);
        std::uint32_t migrated = 0;
        for (int slot = 0; slot < 6; ++slot)
            if (out.damage_metric_mask & (1u << slot)) migrated |= 1u << order[slot];
        out.damage_metric_mask = migrated | (1u << ipc::kStatCost);
        if (out.damage_sort_metric > 0 && out.damage_sort_metric <= 6)
            out.damage_sort_metric = order[out.damage_sort_metric - 1] + 1;
        out.damage_merged = false;
        // The former six-column table was often saved at its old two-column width.
        out.damage_panel_width = 0;
    }
    out.damage_order = statistics::order_string(statistics::parse_order(out.damage_order));
    if (out.damage_metric_mask == 0) out.damage_metric_mask = statistics::kCommonMetrics;
    auto& drawing = out.overlay;
    if (find_integer(text,"hp_offset_x",value)) drawing.hp_offset_x=std::clamp(value,-200,200);
    if (find_integer(text,"hp_offset_y",value)) drawing.hp_offset_y=std::clamp(value,-200,200);
    if (find_integer(text,"hp_only_hover",value)) drawing.hp_only_hover=value!=0;
    if (find_integer(text,"operator_only_paused",value)) drawing.operator_only_paused=value!=0;
    if (find_integer(text,"operator_only_hover",value)) drawing.operator_only_hover=value!=0;
    if (find_integer(text,"operator_offset_x",value)) drawing.operator_offset_x=std::clamp(value,-200,200);
    if (find_integer(text,"operator_offset_y",value)) drawing.operator_offset_y=std::clamp(value,-200,200);
    if (find_integer(text,"operator_metric_mask",value)) drawing.operator_metric_mask=static_cast<std::uint32_t>(value)&statistics::kAllMetrics;
    find_string(text,"operator_order",drawing.operator_order);
    drawing.operator_order=statistics::order_string(statistics::parse_order(drawing.operator_order));
    std::uint32_t selected=0;
    int count=0;
    for (int metric : statistics::parse_order(drawing.operator_order))
        if ((drawing.operator_metric_mask&(1u<<metric)) && count++<6) selected|=1u<<metric;
    drawing.operator_metric_mask=selected ? selected : (1u<<ipc::kStatDamage);
    if (find_integer(text,"path_on_click",value)) drawing.path_on_click=value!=0;
    if (find_integer(text,"path_speed",value)) drawing.path_speed=std::clamp(value,50,2000);
    if (find_integer(text,"path_linger_ms",value)) drawing.path_linger_ms=std::clamp(value,0,10000);
    if (find_integer(text,"prts_cache_mb",value)) out.prts_cache_mb=std::clamp(value,10,5120);
    find_string(text,"prts_cache_path",out.prts_cache_path);
    if (find_integer(text,"enemy_only_paused",value)) drawing.enemy_only_paused=value!=0;
    if (find_integer(text,"enemy_offset_x",value)) drawing.enemy_offset_x=std::clamp(value,-200,200);
    if (find_integer(text,"enemy_offset_y",value)) drawing.enemy_offset_y=std::clamp(value,-200,200);
    find_string(text,"enemy_order",drawing.enemy_order);
    if (find_integer(text,"enemy_metric_mask",value)) {
        drawing.enemy_metric_mask=static_cast<std::uint32_t>(value)&enemy_metrics::kAll;
        std::uint32_t mask=0;int count=0;
        for(int metric:enemy_metrics::parse_order(drawing.enemy_order))
            if((drawing.enemy_metric_mask&enemy_metrics::kMetrics[metric].bit) && count++<6) mask|=enemy_metrics::kMetrics[metric].bit;
        if(mask) drawing.enemy_metric_mask=mask;
        else drawing.enemy_metric_mask=ipc::kEnemyHp|ipc::kEnemyAtk|ipc::kEnemyDef|ipc::kEnemyMagicResistance;
    }
    // v1/v2 stored an unused auto_launch flag. Activate the requested new startup behaviour once;
    // v3 and later preserve an explicit opt-out from the settings page.
    if (out.version < 3) out.auto_launch = true;
    if (find_integer(text,"menu_features",value)) out.menu_features=static_cast<std::uint32_t>(value)&kMenuFeatureMask;
    if (out.version < 5) out.menu_features |= ipc::kFeatureEnemyStats;
    out.features &= out.menu_features;
    std::wstring feature_order;
    if (find_string(text,"feature_order",feature_order)) out.feature_order=parse_feature_order(feature_order);
    if(out.version<6 && out.feature_order==kAppearanceFeatures) out.feature_order=kDefaultFeatureOrder;
    out.prts_fields=out.wiki_fields;
    if (find_integer(text,"prts_fields",value)) out.prts_fields=static_cast<std::uint32_t>(value)&kWikiAllFields;
    if (find_integer(text,"prts_panel_x",value)) out.prts_panel_x=value;
    if (find_integer(text,"prts_panel_y",value)) out.prts_panel_y=value;
    if (find_integer(text,"prts_panel_width",value)) out.prts_panel_width=std::clamp(value,0,4096);
    if (find_integer(text,"prts_panel_height",value)) out.prts_panel_height=std::clamp(value,0,4096);
    if (find_integer(text,"prts_panel_collapsed",value)) out.prts_panel_collapsed=value!=0;
    out.version = kConfigVersion;
    find_string(text, "dock_state", out.dock_state);
    return true;
}

bool config_save(const std::wstring& path, const Config& config)
{
    const std::string game_path = escape_json_string(config.game_path);
    const std::string loader_path = escape_json_string(config.loader_path);
    const std::string dock_state = escape_json_string(config.dock_state);
    const std::string damage_order = escape_json_string(config.damage_order);
    char header[8192];
    const int header_length = std::snprintf(
        header, sizeof(header),
        "{\n"
        "    \"version\": %d,\n"
        "    \"ball_x\": %d,\n"
        "    \"ball_y\": %d,\n"
        "    \"edge\": %d,\n"
        "    \"features\": %lu,\n"
        "    \"opacity\": %d,\n"
        "    \"ui_scale\": %d,\n"
        "    \"spawn_only_paused\": %d,\n"
        "    \"hp_only_paused\": %d,\n"
        "    \"range_only_paused\": %d,\n"
        "    \"auto_launch\": %d,\n"
        "    \"game_path\": \"%s\",\n"
        "    \"loader_path\": \"%s\",\n"
        "    \"damage_panel_x\": %d,\n"
        "    \"damage_panel_y\": %d,\n"
        "    \"damage_panel_collapsed\": %d,\n"
        "    \"spawn_panel_x\": %d,\n"
        "    \"spawn_panel_y\": %d,\n"
        "    \"spawn_panel_collapsed\": %d,\n"
        "    \"damage_panel_width\": %d,\n"
        "    \"damage_panel_height\": %d,\n"
        "    \"spawn_panel_width\": %d,\n"
        "    \"spawn_panel_height\": %d,\n"
        "    \"wiki_panel_x\": %d,\n"
        "    \"wiki_panel_y\": %d,\n"
        "    \"wiki_panel_collapsed\": %d,\n"
        "    \"wiki_panel_width\": %d,\n"
        "    \"wiki_panel_height\": %d,\n"
        "    \"damage_metric_mask\": %lu,\n"
        "    \"damage_merged\": %d,\n"
        "    \"damage_sort_metric\": %d,\n"
        "    \"damage_order\": \"%s\",\n"
        "    \"dock_state\": \"%s\"\n"
        "}\n",
        config.version, config.ball_x, config.ball_y, config.edge, static_cast<unsigned long>(config.features),
        config.opacity, config.ui_scale, config.spawn_only_paused ? 1 : 0,
        config.hp_only_paused ? 1 : 0, config.range_only_paused ? 1 : 0,
        config.auto_launch ? 1 : 0, game_path.c_str(), loader_path.c_str(),
        config.damage_panel_x, config.damage_panel_y, config.damage_panel_collapsed ? 1 : 0,
        config.spawn_panel_x, config.spawn_panel_y, config.spawn_panel_collapsed ? 1 : 0,
        config.damage_panel_width, config.damage_panel_height,
        config.spawn_panel_width, config.spawn_panel_height,
        config.wiki_panel_x, config.wiki_panel_y, config.wiki_panel_collapsed ? 1 : 0,
        config.wiki_panel_width, config.wiki_panel_height,
        static_cast<unsigned long>(config.damage_metric_mask), config.damage_merged ? 1 : 0,
        config.damage_sort_metric, damage_order.c_str(), dock_state.c_str());
    if (header_length <= 0 || static_cast<size_t>(header_length) >= sizeof(header))
    {
        return false;
    }

    std::string contents(header, static_cast<std::size_t>(header_length));
    const auto closing = contents.rfind('}');
    contents.resize(closing);
    while (!contents.empty() && (contents.back() == '\n' || contents.back() == '\r')) contents.pop_back();
    contents += ",\n    \"risk_notice_version\": " + std::to_string(config.risk_notice_version) +
        ",\n    \"menu_opacity\": " + std::to_string(config.menu_opacity) +
        ",\n    \"wiki_fields\": " + std::to_string(config.wiki_fields);
    const char* prefixes[] = {"timer", "spawn", "damage", "hp", "wiki", "range", "operator", "path", "prts", "enemy_stats"};
    for (std::size_t index = 0; index < config.appearances.size(); ++index) {
        const auto& appearance = config.appearances[index];
        const std::string prefix = prefixes[index];
        contents += ",\n    \"" + prefix + "_opacity\": " + std::to_string(appearance.opacity);
        contents += ",\n    \"" + prefix + "_font_size\": " + std::to_string(appearance.font_size);
        contents += ",\n    \"" + prefix + "_text_color\": " + std::to_string(appearance.text_color);
        contents += ",\n    \"" + prefix + "_line_width\": " + std::to_string(appearance.line_width);
        contents += ",\n    \"" + prefix + "_line_style\": " + std::to_string(appearance.line_style);
        contents += ",\n    \"" + prefix + "_click_through\": " + std::to_string(appearance.click_through ? 1 : 0);
    }
    const auto& drawing=config.overlay;
    contents += ",\n    \"damage_compact\": " + std::to_string(config.damage_compact ? 1 : 0);
    const auto append_int=[&](const char* name,int number) {
        contents+=",\n    \""+std::string(name)+"\": "+std::to_string(number);
    };
    append_int("menu_height",std::clamp(config.menu_height,400,900));
    append_int("hp_offset_x",drawing.hp_offset_x); append_int("hp_offset_y",drawing.hp_offset_y);
    append_int("hp_only_hover",drawing.hp_only_hover); append_int("operator_only_paused",drawing.operator_only_paused);
    append_int("operator_only_hover",drawing.operator_only_hover); append_int("operator_offset_x",drawing.operator_offset_x);
    append_int("operator_offset_y",drawing.operator_offset_y); append_int("operator_metric_mask",drawing.operator_metric_mask);
    contents+=",\n    \"operator_order\": \""+escape_json_string(drawing.operator_order)+"\"";
    append_int("path_on_click",drawing.path_on_click); append_int("path_speed",drawing.path_speed);
    append_int("path_linger_ms",drawing.path_linger_ms); append_int("prts_cache_mb",config.prts_cache_mb);
    append_int("menu_features",config.menu_features); append_int("prts_fields",config.prts_fields);
    contents+=",\n    \"feature_order\": \""+escape_json_string(feature_order_string(config.feature_order))+"\"";
    append_int("prts_panel_x",config.prts_panel_x); append_int("prts_panel_y",config.prts_panel_y);
    append_int("prts_panel_width",config.prts_panel_width); append_int("prts_panel_height",config.prts_panel_height);
    append_int("prts_panel_collapsed",config.prts_panel_collapsed);
    append_int("enemy_only_paused",drawing.enemy_only_paused);append_int("enemy_offset_x",drawing.enemy_offset_x);
    append_int("enemy_offset_y",drawing.enemy_offset_y);append_int("enemy_metric_mask",drawing.enemy_metric_mask);
    contents+=",\n    \"enemy_order\": \""+escape_json_string(drawing.enemy_order)+"\"";
    contents+=",\n    \"prts_cache_path\": \""+escape_json_string(config.prts_cache_path)+"\"";
    contents += "\n}\n";
    const std::wstring temp_path = path + L".tmp";
    HANDLE file = CreateFileW(temp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    const bool written = write_all(file, contents.data(), contents.size());
    CloseHandle(file);
    if (!written)
    {
        DeleteFileW(temp_path.c_str());
        return false;
    }

    // MOVEFILE_WRITE_THROUGH 让替换在落盘后才返回，避免关机瞬间丢掉刚写的配置。
    if (!MoveFileExW(temp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temp_path.c_str());
        return false;
    }
    return true;
}

}  // namespace arkhelp
