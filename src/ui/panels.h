#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>
#include "../config/config.h"

// 可拖拽 / 可折叠的信息面板（伤害统计、出怪倒计时）。
//
// 设计目标是「轻量、不挡视线」：
// - 每个面板是独立的 ArkHelper 分层窗口，不依赖游戏窗口，游戏全屏/最小化都不影响；
// - 只有内容区接收鼠标（拖动标题栏、点击右侧箭头折叠），没有边框和阴影干扰；
// - 位置与折叠状态由外壳写进 arkhelp.json，下次启动原样恢复。
namespace arkhelp::panels
{

struct Row
{
    std::wstring left;   // 左列（干员名 / 出怪时间）
    std::wstring right;  // 右列（伤害值 / 敌人名 + 数量）
    unsigned accent = 0; // COLORREF：非 0 时左列用该颜色，用来标记伤害类型
    bool selected = false; // 选中行（WIKI 列表当前敌人）
    bool icon_only = false; // true 表示左侧图标选择列的条目（只画头像，不画文字）
    std::vector<std::wstring> cells; // 非空时按表格渲染（每个元素一列）
    bool header = false;             // 表格的分节标题行
    bool table = false;              // true：cells 按「标签/数值」成对分组，渲染成网格表格
    bool wrap = false;               // semantic paragraph, reflowed with the actual panel font/width
    // Legacy metadata. Overflow hints show the actual clipped text instead.
    std::wstring tip;
};

struct Callbacks
{
    // 拖动结束或折叠状态变化时回调，外壳负责写入配置。
    std::function<void(const wchar_t* id, int x, int y, bool collapsed)> on_state_changed;
    // 点击可交互行时回调（行号从 0 开始，与最近一次 update 传入的 rows 对应）。
    std::function<void(const wchar_t* id, int row_index)> on_row_clicked;
    // 拖拽底部边缘调整面板高度结束时回调（像素高度，外壳写入配置）。
    std::function<void(const wchar_t* id, int width_px, int height_px)> on_resized;
    // Dismiss only this battle's retained report; feature settings remain enabled.
    std::function<void(const wchar_t* id)> on_closed;
    std::function<void(const wchar_t* id)> on_settings;
};

inline constexpr wchar_t kDamagePanelId[] = L"damage";
inline constexpr wchar_t kSpawnPanelId[] = L"spawn";
inline constexpr wchar_t kWikiPanelId[] = L"wiki";
inline constexpr wchar_t kPrtsPanelId[] = L"prts";

bool initialize(HINSTANCE instance, Callbacks callbacks);
void shutdown();

// 恢复配置里的位置/折叠状态；x/y 为配置哨兵值时使用默认位置。
void restore_geometry(const wchar_t* id, int x, int y, bool collapsed);

// 恢复配置里的人工高度/宽度（像素；<=0 表示按内容自动）。
void restore_height(const wchar_t* id, int height_px);
void restore_width(const wchar_t* id, int width_px);

// 磁吸分组持久化：返回 "damage>spawn|wiki" 形式（自上而下），空表示没有分组。
std::wstring save_dock_state();
void restore_dock_state(const std::wstring& state);

// 更新面板内容与可见性。内容不变时不重绘；隐藏时窗口也一起隐藏。
void update(const wchar_t* id, const std::wstring& title, const std::vector<Row>& rows, bool visible);

// PRTS 表格模式下的自动宽度（逻辑像素）；0 表示用默认/用户拖拽宽度。
void set_auto_width(const wchar_t* id, int logical_width);
// Measures the actual labels and values with the panel font, before enabling horizontal scrolling.
void fit_statistics_width(const std::vector<Row>& rows);
void fit_reference_width(const wchar_t* id, const std::vector<Row>& rows);
// Fit once for a content revision, then respect manual resizing until the next content change.
void fit_width(const wchar_t* id, int logical_width);
// Retained reports expose a close button in their title bar.
void set_closeable(const wchar_t* id, bool closeable);
// 每个面板单独应用透明度、逻辑字号及 0xRRGGBB 文字颜色（-1 默认）。
void set_appearance(const wchar_t* id, const FeatureAppearance& appearance);

// 界面缩放变化时重建字体/尺寸。
void refresh_theme(int ui_scale);

// 设置左侧头像竖条（WIKI 列表用）：avatars 与 rows 前 N 行一一对应，HBITMAP 由调用方持有。
void set_avatars(const wchar_t* id, const std::vector<HBITMAP>& avatars, int logical_size);
void reset_scroll(const wchar_t* id, bool include_avatars = false);

}  // namespace arkhelp::panels
