#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "../config/config.h"
#include "../data/battle_statistics.h"
#include "../shell/platform.h"
#include "../shell/status.h"

// 功能菜单及其设置子页：固定标题和页脚，中间内容按需滚动。
namespace arkhelp
{

// 「数值统计」功能的配置：勾选哪些指标、顺序、是否合并、排序依据。
struct DamageConfig
{
    std::uint32_t metric_mask = statistics::kCommonMetrics; // stable StatMetric identities
    bool merged = false;                // true: first selected metric; false: selected table columns
    bool compact = true;
    bool show_devices = false;
    int sort_metric = 0;                // 0: total damage, otherwise StatMetric+1 descending
    std::wstring order = L"total,physical,arts,true,heal,element";
};

struct MenuCallbacks
{
    std::function<const Status&()> get_status;
    std::function<uint32_t()> get_feature_mask;
    std::function<void(uint32_t bit, bool enabled)> on_feature_toggle;
    std::function<bool()> get_launch_busy;
    std::function<void()> on_launch;
    std::function<void()> on_open_settings;
    std::function<Config()> get_config;
    std::function<void(int opacity_percent, bool commit)> on_opacity;
    std::function<void(int ui_scale_percent)> on_ui_scale;
    std::function<void()> on_reset;
    std::function<void(std::uint32_t bit, const FeatureAppearance& appearance)> on_appearance;
    std::function<void(std::uint32_t feature, std::uint32_t mask)> on_wiki_fields;
    std::function<void(std::uint32_t mask, const FeatureOrder& order)> on_feature_menu;
    std::function<void(bool enabled)> on_auto_launch;
    std::function<void(int opacity_percent)> on_menu_opacity;
    std::function<void(int logical_height)> on_menu_height;
    std::function<bool(uint32_t bit)> get_only_paused;
    std::function<void(uint32_t bit, bool enabled)> on_only_paused;
    std::function<DamageConfig()> get_damage_config;
    std::function<void(const DamageConfig& config)> on_damage_config;
    std::function<void(const OverlayOptions& options)> on_overlay_options;
    std::function<void(int limit_mb)> on_prts_cache_limit;
    std::function<std::wstring()> get_prts_cache_path;
    // Empty result means success; otherwise show the error within the settings page.
    std::function<std::wstring(const std::wstring& path)> on_prts_cache_path;
    std::function<std::wstring()> on_prts_cache_open;
    std::function<void()> on_hidden;
    std::function<void()> on_exit;
};

bool menu_create(HINSTANCE instance, MenuCallbacks callbacks);
void menu_destroy();

// anchor 是悬浮球的屏幕矩形，用于把菜单摆在球旁边并保证不越出工作区。
void menu_show(const Config& config, const RECT& anchor);
void menu_hide();
bool menu_is_visible();
void menu_refresh();

// 打开指定功能的配置页；窗口是否显示仍由 menu_show 控制。
void menu_open_config(std::uint32_t bit);
void menu_open_settings();
// 缩放、恢复默认等操作发生后，同步当前子页且保留菜单窗口。
void menu_sync_config(const Config& config);
// 悬浮球拖动时让菜单跟着一起移动，保持两者是一个整体。
void menu_follow(int dx, int dy);

}  // namespace arkhelp
