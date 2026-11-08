#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <arkhelp/ipc.h>

// 本地配置（ArkHelper/config/arkhelp.json）。
// 字段刻意保持扁平：手写解析器只支持本程序自己写出的格式，
// 这样不必引入 JSON 库，也能让配置文件在文本编辑器里可直接改。
namespace arkhelp
{

inline constexpr int kConfigVersion = 6;

struct FeatureAppearance
{
    int opacity = 100;       // 20..100, independent for each feature
    int font_size = 0;       // logical pixels; 0 keeps the feature's original sizes
    std::int32_t text_color = -1; // 0xRRGGBB; -1 keeps semantic/default colours
    int line_width = 2;     // logical pixels, 1..8; line features only
    int line_style = 0;     // 0 solid, 1 dashed, 2 dotted
    bool operator==(const FeatureAppearance&) const = default;
};

inline constexpr std::array<std::uint32_t, 10> kAppearanceFeatures = {
    ipc::kFeatureSpawnTimer, ipc::kFeatureSpawnList, ipc::kFeatureDamageNumbers,
    ipc::kFeatureHpBars, ipc::kFeatureWiki, ipc::kFeatureEnemyRange,
    ipc::kFeatureOperatorStats, ipc::kFeatureEnemyPaths, ipc::kFeatureEnemyPrts, ipc::kFeatureEnemyStats};
using FeatureOrder = std::array<std::uint32_t, kAppearanceFeatures.size()>;
inline constexpr FeatureOrder kDefaultFeatureOrder = {
    ipc::kFeatureDamageNumbers, ipc::kFeatureOperatorStats,
    ipc::kFeatureEnemyStats, ipc::kFeatureHpBars, ipc::kFeatureEnemyRange, ipc::kFeatureEnemyPaths,
    ipc::kFeatureWiki, ipc::kFeatureEnemyPrts, ipc::kFeatureSpawnTimer, ipc::kFeatureSpawnList};
inline constexpr std::uint32_t kMenuFeatureMask = [] {
    std::uint32_t mask = 0;
    for (auto bit : kAppearanceFeatures) mask |= bit;
    return mask;
}();
// Normalize persisted order: keep each supported feature once and append newly added features.
FeatureOrder parse_feature_order(const std::wstring& text);
std::wstring feature_order_string(const FeatureOrder& order);

struct OverlayOptions
{
    int hp_offset_x = 0, hp_offset_y = 0; // logical pixels, -200..200
    bool hp_only_hover = false;
    bool operator_only_paused = false, operator_only_hover = false;
    int operator_offset_x = 28, operator_offset_y = -40;
    std::uint32_t operator_metric_mask = (1u << ipc::kStatDamage) | (1u << ipc::kStatHeal) |
        (1u << ipc::kStatCost);
    std::wstring operator_order = L"total,physical,arts,true,heal,element,cost,buildup,taken,shield,dps,share";
    bool path_on_click = false;
    int path_speed = 400; // logical pixels per second, 50..2000
    int path_linger_ms = 1000; // after animation completes and pointer leaves, 0..10000
    bool enemy_only_paused = false;
    int enemy_offset_x = 28, enemy_offset_y = -40;
    std::uint32_t enemy_metric_mask = ipc::kEnemyHp | ipc::kEnemyAtk | ipc::kEnemyDef | ipc::kEnemyMagicResistance;
    std::wstring enemy_order = L"hp,max_hp,atk,def,resistance,move_speed,attack_speed,attack_interval,weight,block,life_point";
};
enum WikiField : std::uint32_t
{
    kWikiName = 1u << 0, kWikiSource = 1u << 1,
    kWikiHp = 1u << 2, kWikiMaxHp = 1u << 3,
    kWikiAtk = 1u << 4, kWikiDef = 1u << 5,
    kWikiResistance = 1u << 6, kWikiMoveSpeed = 1u << 7,
    kWikiAttackInterval = 1u << 8, kWikiWeight = 1u << 9,
    kWikiBlock = 1u << 10, kWikiLifePoint = 1u << 11,
    kWikiPrtsBasic = 1u << 12, kWikiPrtsStats = 1u << 13,
    kWikiPrtsAbilities = 1u << 14, kWikiAvatars = 1u << 15,
};
inline constexpr std::uint32_t kWikiAllFields = (1u << 16) - 1;

// 悬浮球位置哨兵值：表示尚未记录过位置，启动时回落到默认位置。
inline constexpr int kConfigNoPosition = -100000;

enum class DockEdge : int
{
    kNone = 0,
    kLeft = 1,
    kRight = 2,
};

struct Config
{
    int version = kConfigVersion;
    int risk_notice_version = 0; // explicit first-use acknowledgement; never migrated to accepted
    int ball_x = kConfigNoPosition;
    int ball_y = kConfigNoPosition;
    int edge = 0;          // DockEdge
    uint32_t features = 1 | 4 | 8 | 64 | 128; // ipc::Feature 位掩码：出怪倒计时/伤害数字/怪物 WIKI
    int opacity = 100;     // 悬浮球不透明度百分比 40..100（默认 100，避免游戏/桌面背景下看不清）
    int ui_scale = 100;    // 界面缩放百分比 80/100/120/150
    int menu_opacity = 100;
    int menu_height = 572; // logical pixels; main menu bottom-edge resize, 400..900
    std::array<FeatureAppearance, kAppearanceFeatures.size()> appearances{};
    std::uint32_t menu_features = kMenuFeatureMask;
    FeatureOrder feature_order = kDefaultFeatureOrder;
    OverlayOptions overlay;
    int prts_cache_mb = 500;
    std::wstring prts_cache_path; // empty: <application>/cache/prts
    std::uint32_t wiki_fields = kWikiAllFields;
    bool spawn_only_paused = false;  // 「红门出怪显示」配置：仅暂停时显示
    bool hp_only_paused = false;     // 「头顶血条」配置：仅暂停时显示
    bool range_only_paused = false;  // 「远程敌人攻击范围」配置：仅暂停时显示
    // Statistics: stable StatMetric bit identities; compact view shows the first selected column.
    // Sort 0 uses total damage, otherwise StatMetric+1. Reordering never changes either selection.
    uint32_t damage_metric_mask = (1u << 0) | (1u << 4) | (1u << 6) | (1u << 10) | (1u << 11);
    bool damage_merged = false;
    int damage_sort_metric = 0;
    std::wstring damage_order = L"total,physical,arts,true,heal,element,cost,buildup,taken,shield,dps,share";
    bool auto_launch = true;  // 启动助手时启动一次游戏；已运行时仅连接
    std::wstring game_path;
    std::wstring dock_state;  // 磁吸分组（"a>b|c" 形式，空表示无分组）   // Arknights.exe 路径；空表示自动探测
    std::wstring loader_path; // ArkHelperLauncher.exe 路径；空表示自动探测

    // 可拖拽信息面板的位置与折叠状态（窗口左上角，屏幕物理像素）。
    int damage_panel_x = kConfigNoPosition;
    int damage_panel_y = kConfigNoPosition;
    bool damage_panel_collapsed = false;
    int spawn_panel_x = kConfigNoPosition;
    int spawn_panel_y = kConfigNoPosition;
    bool spawn_panel_collapsed = false;
    int wiki_panel_x = kConfigNoPosition;
    int wiki_panel_y = kConfigNoPosition;
    bool wiki_panel_collapsed = false;
    // 人工拖拽的面板高度（物理像素，0=按内容自动）。
    int damage_panel_height = 0;
    int spawn_panel_height = 0;
    int wiki_panel_height = 0;
    int damage_panel_width = 0;
    int spawn_panel_width = 0;
    int wiki_panel_width = 0;
    int prts_panel_x = kConfigNoPosition, prts_panel_y = kConfigNoPosition;
    bool prts_panel_collapsed = false;
    int prts_panel_height = 0, prts_panel_width = 0;
    std::uint32_t prts_fields = kWikiAllFields;
};

inline std::size_t feature_appearance_index(std::uint32_t bit)
{
    for (std::size_t index = 0; index < kAppearanceFeatures.size(); ++index)
        if (kAppearanceFeatures[index] == bit) return index;
    return 0;
}
inline FeatureAppearance& feature_appearance(Config& config, std::uint32_t bit)
{
    return config.appearances[feature_appearance_index(bit)];
}
inline const FeatureAppearance& feature_appearance(const Config& config, std::uint32_t bit)
{
    return config.appearances[feature_appearance_index(bit)];
}

Config config_defaults();

// 缺失文件返回 false 且 out 保持 defaults；解析失败的字段单独回落默认值。
bool config_load(const std::wstring& path, Config& out);
// 原子写：先写 <path>.tmp，再 MoveFileExW 替换，避免掉电/崩溃留下半个 JSON。
bool config_save(const std::wstring& path, const Config& config);

}  // namespace arkhelp
