#include "menu.h"
#include "menu_viewport.h"
#include "../data/battle_statistics.h"
#include "../data/enemy_metrics.h"
#include <cmath>
#include <bit>

#include <algorithm>
#include <cwchar>
#include <string>
#include <commdlg.h>
#include <shobjidl.h>

#include <arkhelp/ipc.h>

#include "../log/log.h"
#include "../shell/constants.h"
#include "../shell/theme.h"
#include "../shell/win_util.h"

namespace arkhelp
{
namespace
{

constexpr int kMenuLogicalWidth = 360;
constexpr int kMenuMaximumHeight = 440;
constexpr int kFeatureCount = static_cast<int>(kAppearanceFeatures.size());
constexpr int kShadowMarginLogical = 12;
constexpr int kCornerRadiusLogical = 16;
constexpr int kShadowBlurLogical = 10;
constexpr int kShadowAlpha = 70;
constexpr std::uint32_t kSettingsPage = ~std::uint32_t{0};
constexpr int kScaleOptions[] = {80, 100, 120, 150};
constexpr std::uint32_t kFeatureBits[kFeatureCount] = {
    ipc::kFeatureSpawnTimer, ipc::kFeatureSpawnList, ipc::kFeatureDamageNumbers,
    ipc::kFeatureHpBars, ipc::kFeatureWiki, ipc::kFeatureEnemyRange,
    ipc::kFeatureOperatorStats, ipc::kFeatureEnemyPaths, ipc::kFeatureEnemyPrts,
    ipc::kFeatureEnemyStats,
};
constexpr int kAppearanceHeight = 214;
struct WikiFieldSpec { std::uint32_t bit; const wchar_t* label; };
constexpr WikiFieldSpec kWikiFields[] = {
    {kWikiName,L"敌人名称"}, {kWikiHp,L"当前生命"},
    {kWikiMaxHp,L"生命上限"}, {kWikiAtk,L"攻击力"}, {kWikiDef,L"防御力"},
    {kWikiResistance,L"法术抗性"}, {kWikiMoveSpeed,L"移动速度"},
    {kWikiAttackInterval,L"攻击间隔"}, {kWikiWeight,L"重量"},
    {kWikiBlock,L"阻挡数"}, {kWikiLifePoint,L"扣除目标生命"},
    {kWikiPrtsBasic,L"PRTS 基本资料"}, {kWikiPrtsStats,L"PRTS 属性表"},
    {kWikiPrtsAbilities,L"PRTS 能力与机制"}, {kWikiAvatars,L"敌人头像列表"},
};

enum class HitTarget
{
    kNone = 0,
    kTitle = 1,
    kClose = 2,
    kToggleFirst = 10,  // kToggleFirst + i
    kSettings = 20,
    kExit = 21,
    kLaunch = 22,
    kConfigFirst = 30,  // kConfigFirst + i
    kBack = 40,
    kDamageRowFirst = 50,   // 数值统计配置页：+ index(0..7)
    kDamageUpFirst = 100,    // 该行上移
    kDamageDownFirst = 120,  // 该行下移
    kDamageToggleFirst = 140,// 该行勾选/开关
    kDamageSort = 160,       // 排序依据切换
    kConfigToggle = 41,
    kDamagePreset = 161,
    kScaleFirst = 180,
    kOpacity = 190,
    kReset = 191,
    kMenuOpacity = 192,
    kAutoLaunch = 193,
    kAppearanceOpacity = 200,
    kAppearanceFont = 201,
    kAppearanceColor = 202,
    kAppearanceReset = 203,
    kWikiFieldFirst = 300,
    kWikiFieldsDefault = 320,
    kWikiFieldsNone = 321,
    kHpHover = 400, kHpX, kHpY, kOperatorPause, kOperatorHover, kOperatorX, kOperatorY,
    kPathClick, kPathSpeed, kPathLinger, kCacheLimit,
    kEnemyPause, kEnemyX, kEnemyY, kLineWidth,
    kLineStyleFirst = 430, kCacheOpen = 440, kCacheChange, kCacheDefault,
    kManageToggleFirst = 500, kManageUpFirst = 520, kManageDownFirst = 540,
};

struct MenuLayout
{
    RECT content{};
    RECT viewport{};
    RECT title_bar{};
    RECT close_button{};
    RECT status_card{};
    RECT toggle_rows[kFeatureCount]{};
    RECT toggle_switches[kFeatureCount]{};
    RECT config_buttons[kFeatureCount]{};
    RECT back_button{};
    RECT settings_button{};
    RECT exit_button{};
    RECT pause_row{};
    RECT pause_switch{};
    RECT scale_buttons[4]{};
    RECT opacity_track{};
    RECT opacity_hit{};
    RECT reset_button{};
    RECT menu_opacity_track{};
    RECT menu_opacity_hit{};
    RECT auto_launch_row{};
    RECT auto_launch_switch{};
    RECT appearance_opacity_track{};
    RECT appearance_opacity_hit{};
    RECT appearance_font{};
    RECT appearance_color{};
    RECT appearance_reset{};
    int padding = 18;
};

struct MenuState
{
    HWND hwnd = nullptr;
    bool visible = false;
    bool class_registered = false;
    bool dragging = false;
    bool resizing_height = false;
    POINT resize_cursor_start{};
    int resize_start_height = 0;
    int main_height = 572;
    bool dragging_opacity = false;
    HitTarget slider_target = HitTarget::kOpacity;
    bool popup_open = false;
    POINT drag_cursor_start{0, 0};
    RECT drag_window_start{0, 0, 0, 0};
    int dpi = 96;
    int ui_scale = 100;
    int opacity = 100;
    int menu_opacity = 100;
    bool auto_launch = true;
    std::uint32_t wiki_fields = kWikiAllFields;
    FeatureAppearance appearance;
    OverlayOptions overlay;
    int cache_mb = 500;
    std::wstring cache_path, cache_error;
    int font_scale = 0;
    RECT work{};
    menu_layout::Viewport viewport;
    int wheel_remainder = 0;
    std::uint32_t menu_features = kMenuFeatureMask;
    FeatureOrder feature_order = kDefaultFeatureOrder;
    OverflowHints hints;
    HitTarget pressed = HitTarget::kNone;
    POINT press_point{};
    std::uint32_t config_bit = 0;  // 0 = 主菜单，否则显示该功能的独立配置页
    DamageConfig damage;           // 「数值统计」配置页的编辑副本
    int hover = static_cast<int>(HitTarget::kNone);
    MenuCallbacks callbacks;
    MenuLayout layout;
    LayeredBitmap surface;
    int present_alpha = 255;
    int anim_frames = 0;
    RECT anim_target{0, 0, 0, 0};
    HBITMAP logo = nullptr;
    int logo_size = 0;
    HFONT title_font = nullptr;
    HFONT body_font = nullptr;
    HFONT small_font = nullptr;
    HFONT button_font = nullptr;
};

MenuState g_menu;

// 逻辑像素 → 物理像素（DPI × 界面缩放）。菜单里所有布局常量都是逻辑单位。
int S(int logical)
{
    return scale_ui(logical, g_menu.dpi, g_menu.ui_scale);
}

void destroy_fonts()
{
    if (g_menu.title_font != nullptr)
    {
        DeleteObject(g_menu.title_font);
        g_menu.title_font = nullptr;
    }
    if (g_menu.body_font != nullptr)
    {
        DeleteObject(g_menu.body_font);
        g_menu.body_font = nullptr;
    }
    if (g_menu.small_font != nullptr)
    {
        DeleteObject(g_menu.small_font);
        g_menu.small_font = nullptr;
    }
    if (g_menu.button_font != nullptr)
    {
        DeleteObject(g_menu.button_font);
        g_menu.button_font = nullptr;
    }
}

void create_fonts()
{
    destroy_fonts();
    g_menu.title_font = create_ui_font(16, FW_SEMIBOLD, g_menu.dpi, g_menu.ui_scale);
    g_menu.body_font = create_ui_font(13, FW_NORMAL, g_menu.dpi, g_menu.ui_scale);
    g_menu.small_font = create_ui_font(11, FW_NORMAL, g_menu.dpi, g_menu.ui_scale);
    g_menu.button_font = create_ui_font(13, FW_NORMAL, g_menu.dpi, g_menu.ui_scale);
    g_menu.font_scale = g_menu.ui_scale;
}

int appearance_height()
{
    if (g_menu.config_bit == ipc::kFeatureEnemyRange) return 130;
    if (g_menu.config_bit == ipc::kFeatureEnemyPaths) return 174;
    return kAppearanceHeight;
}

int page_content_height()
{
    if (g_menu.config_bit == 0) return S(106 + 36*std::popcount(g_menu.menu_features));
    if (g_menu.config_bit == kSettingsPage) return S(588 + 36*kFeatureCount);
    if (g_menu.config_bit == ipc::kFeatureHpBars) return S(kAppearanceHeight+264);
    if (g_menu.config_bit == ipc::kFeatureOperatorStats) return S(kAppearanceHeight+224+48+30*ipc::kStatCount+8);
    if (g_menu.config_bit == ipc::kFeatureEnemyStats) return S(kAppearanceHeight+224+48+30*enemy_metrics::kMetrics.size()+8);
    if (g_menu.config_bit == ipc::kFeatureEnemyPaths) return S(174+280);
    if (g_menu.config_bit == ipc::kFeatureEnemyRange) return S(130+200);
    if (g_menu.config_bit == ipc::kFeatureDamageNumbers)
        return S(kAppearanceHeight + 48 + 30 * ipc::kStatCount + 3 * 36 + 8);
    if (g_menu.config_bit == ipc::kFeatureWiki) return S(kAppearanceHeight + 32 + 12 * 32 + 44);
    if (g_menu.config_bit == ipc::kFeatureEnemyPrts) return S(kAppearanceHeight + 32 + 14 * 32 + 44);
    return S(appearance_height() + 96);
}

int feature_content_top()
{
    return g_menu.layout.viewport.top + S(appearance_height());
}

void compute_layout(int width, int height)
{
    MenuLayout& layout = g_menu.layout;
    const int margin = S(kShadowMarginLogical);
    const int padding = S(18);

    layout.content = RECT{margin, margin, margin + width, margin + height};
    layout.padding = padding;
    layout.title_bar = RECT{margin, margin, margin + width, margin + S(44)};
    layout.close_button = RECT{layout.title_bar.right - padding - S(24), layout.title_bar.top + S(8),
                               layout.title_bar.right - padding, layout.title_bar.top + S(8) + S(24)};
    layout.settings_button = RECT{layout.close_button.left - S(8) - S(24), layout.close_button.top,
                                  layout.close_button.left - S(8), layout.close_button.bottom};

    const int buttons_bottom = margin + height - S(14);
    const int button_top = buttons_bottom - S(36);
    layout.exit_button = RECT{margin + padding, button_top, margin + width - padding, buttons_bottom};
    layout.back_button = layout.exit_button;
    layout.viewport = RECT{margin + S(10), layout.title_bar.bottom + S(10),
                           margin + width - S(10), button_top - S(10)};
    if (layout.viewport.bottom < layout.viewport.top) layout.viewport.bottom = layout.viewport.top;
    g_menu.viewport.top = layout.viewport.top;
    g_menu.viewport.bottom = layout.viewport.bottom;
    g_menu.viewport.content_height = page_content_height();
    g_menu.viewport.clamp();

    int y = layout.viewport.top;
    layout.status_card = RECT{margin + padding, y, margin + width - padding, y + S(80)};

    const int row_height = S(36);
    y = layout.status_card.bottom + S(26);
    const int row_left = margin + S(10);
    const int row_right = margin + width - S(10);
    for (int i = 0; i < kFeatureCount; ++i) {
        layout.toggle_rows[i] = {}; layout.toggle_switches[i] = {}; layout.config_buttons[i] = {};
    }
    int position = 0;
    for (const auto bit : g_menu.feature_order)
    {
        if (!(g_menu.menu_features & bit)) continue;
        const int i = static_cast<int>(std::find(std::begin(kFeatureBits), std::end(kFeatureBits), bit) - std::begin(kFeatureBits));
        if (i >= kFeatureCount) continue;
        layout.toggle_rows[i] = RECT{row_left, y + position * row_height, row_right, y + (position + 1) * row_height};
        ++position;
        const int switch_width = S(44);
        const int switch_height = S(24);
        layout.toggle_switches[i] =
            RECT{row_right - S(14) - switch_width, layout.toggle_rows[i].top + (row_height - switch_height) / 2,
                 row_right - S(14), layout.toggle_rows[i].top + (row_height - switch_height) / 2 + switch_height};
        layout.config_buttons[i] =
            RECT{layout.toggle_switches[i].left - S(32), layout.toggle_rows[i].top + S(5),
                 layout.toggle_switches[i].left - S(6), layout.toggle_rows[i].bottom - S(5)};
    }

    layout.pause_row = RECT{margin + padding, feature_content_top() + S(38),
                            margin + width - padding, feature_content_top() + S(78)};
    layout.pause_switch = RECT{layout.pause_row.right - S(44), layout.pause_row.top + S(8),
                               layout.pause_row.right, layout.pause_row.top + S(32)};
    const int segment_gap = S(7);
    const int segment_width = (width - 2 * padding - 3 * segment_gap) / 4;
    for (int i = 0; i < 4; ++i)
    {
        const int left = margin + padding + i * (segment_width + segment_gap);
        layout.scale_buttons[i] = RECT{left, layout.viewport.top + S(28), left + segment_width,
                                       layout.viewport.top + S(60)};
    }
    layout.opacity_track = RECT{margin + padding + S(9), layout.viewport.top + S(112),
                                margin + width - padding - S(9), layout.viewport.top + S(116)};
    layout.opacity_hit = layout.opacity_track;
    InflateRect(&layout.opacity_hit, S(9), S(12));
    layout.menu_opacity_track = layout.opacity_track;
    OffsetRect(&layout.menu_opacity_track, 0, S(68));
    layout.menu_opacity_hit = layout.menu_opacity_track;
    InflateRect(&layout.menu_opacity_hit, S(9), S(12));
    layout.auto_launch_row = RECT{margin + padding, layout.viewport.top + S(214),
                                 margin + width - padding, layout.viewport.top + S(254)};
    layout.auto_launch_switch = RECT{layout.auto_launch_row.right-S(44), layout.auto_launch_row.top+S(8),
                                    layout.auto_launch_row.right, layout.auto_launch_row.top+S(32)};
    layout.reset_button = RECT{margin + padding, layout.viewport.top + S(500),
                               margin + width - padding, layout.viewport.top + S(536)};
    layout.appearance_opacity_track = RECT{margin + padding + S(9), layout.viewport.top+S(60),
                                           margin + width - padding - S(9), layout.viewport.top+S(64)};
    layout.appearance_opacity_hit = layout.appearance_opacity_track;
    InflateRect(&layout.appearance_opacity_hit,S(9),S(12));
    layout.appearance_font = RECT{margin + width - padding - S(134), layout.viewport.top+S(84),
                                  margin + width - padding, layout.viewport.top+S(116)};
    layout.appearance_color = layout.appearance_font;
    OffsetRect(&layout.appearance_color,0,S(g_menu.config_bit==ipc::kFeatureEnemyPaths?0:40));
    const int reset_top=g_menu.config_bit==ipc::kFeatureEnemyRange?84:g_menu.config_bit==ipc::kFeatureEnemyPaths?128:168;
    layout.appearance_reset = RECT{margin+padding,layout.viewport.top+S(reset_top),
                                   margin+width-padding,layout.viewport.top+S(reset_top+32)};
}

bool ensure_surface()
{
    if (g_menu.work.right <= g_menu.work.left) SystemParametersInfoW(SPI_GETWORKAREA, 0, &g_menu.work, 0);
    const int shadow = S(kShadowMarginLogical);
    const int content_width = std::max(1, std::min<int>(S(kMenuLogicalWidth),
        g_menu.work.right - g_menu.work.left - 2 * shadow - 2 * S(8)));
    const int requested_height=g_menu.config_bit==0?S(g_menu.main_height):S(44+10+10+36+14)+page_content_height();
    const int content_height = menu_layout::bounded_content_height(
        requested_height, g_menu.config_bit==0?S(g_menu.main_height):S(kMenuMaximumHeight),
        g_menu.work.bottom - g_menu.work.top, shadow, S(8));
    const int width = content_width + 2 * shadow;
    const int height = content_height + 2 * shadow;
    compute_layout(content_width, content_height);
    if (g_menu.surface.bits != nullptr && g_menu.surface.width == width && g_menu.surface.height == height &&
        g_menu.title_font != nullptr && g_menu.font_scale == g_menu.ui_scale)
    {
        return true;
    }

    if (!layered_bitmap_create(g_menu.surface, width, height))
    {
        log_error("menu surface creation failed, width=%d height=%d", width, height);
        return false;
    }
    create_fonts();

    // 隐藏状态下改尺寸不会闪烁；菜单显示前还会再 SetWindowPos 定位一次。
    if (g_menu.hwnd != nullptr)
    {
        RECT position{};
        GetWindowRect(g_menu.hwnd, &position);
        const int left = std::clamp<int>(position.left, g_menu.work.left + S(8),
                                         std::max<int>(g_menu.work.left + S(8), g_menu.work.right - width - S(8)));
        const int top = std::clamp<int>(position.top, g_menu.work.top + S(8),
                                        std::max<int>(g_menu.work.top + S(8), g_menu.work.bottom - height - S(8)));
        if (g_menu.anim_frames > 0)
        {
            g_menu.anim_target = RECT{left, top, left + width, top + height};
        }
        SetWindowPos(g_menu.hwnd, nullptr, left, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return true;
}

void fill_circle(HDC dc, int center_x, int center_y, int radius, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    const HGDIOBJ old_brush = SelectObject(dc, brush);
    const HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, center_x - radius, center_y - radius, center_x + radius + 1, center_y + radius + 1);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(brush);
}

COLORREF link_color(LinkState state)
{
    switch (state)
    {
    case LinkState::kBattle:
        return theme::kStatusGreen;
    case LinkState::kWaiting:
        return theme::kStatusYellow;
    case LinkState::kDisconnected:
    default:
        return theme::kStatusGray;
    }
}

std::wstring format_duration(double seconds)
{
    if (!(seconds > 0.0))
    {
        return L"00:00";
    }
    const int total = static_cast<int>(seconds);
    wchar_t buffer[32];
    std::swprintf(buffer, sizeof(buffer) / sizeof(buffer[0]), L"%02d:%02d", total / 60, total % 60);
    return std::wstring(buffer);
}

std::wstring link_value(const Status& status)
{
    switch (status.link)
    {
    case LinkState::kBattle:
    {
        std::wstring text = L"战斗中";
        if (status.time_seconds > 0.0)
        {
            text += L" · ";
            text += format_duration(status.time_seconds);
        }
        return text;
    }
    case LinkState::kWaiting:
        return L"已连接 · 等待战斗";
    case LinkState::kDisconnected:
    default:
        if (status.battle_mapping_open)
        {
            return L"连接中";
        }
        // 游戏在运行但共享内存不存在，通常是没经 ArkHelperLauncher 启动。
        return status.game_process.empty() ? L"未连接" : L"等待连接";
    }
}

void draw_status_card(HDC dc, const Status& status)
{
    const MenuLayout& layout = g_menu.layout;
    fill_round_rect(dc, layout.status_card, S(10), theme::kCard);

    const int left = layout.status_card.left + S(14);
    const int right = layout.status_card.right - S(14);
    const int row_height = S(24);
    const int label_width = S(66);
    int y = layout.status_card.top + S(4);

    // All three labels share the same left edge.
    draw_text(dc, L"连接状态", RECT{left, y, left + label_width, y + row_height}, g_menu.small_font,
              theme::kTextMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const std::wstring link_text = link_value(status);
    draw_text(dc, link_text.c_str(),
              RECT{left + label_width + S(8), y, right, y + row_height}, g_menu.body_font,
              link_color(status.link), DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    y += row_height;
    if (!status.launch_error.empty())
    {
        draw_text(dc,L"启动失败",RECT{left,y,left+label_width,y+row_height},g_menu.small_font,
                  theme::kDanger,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        draw_text(dc,status.launch_error.c_str(),
                  RECT{left+label_width+S(8),y+S(2),right,layout.status_card.bottom-S(4)},
                  g_menu.small_font,theme::kTextSecondary,DT_LEFT|DT_TOP|DT_WORDBREAK|DT_END_ELLIPSIS);
        return;
    }
    // 第二行：游戏进程。
    draw_text(dc, L"游戏进程", RECT{left, y, left + label_width, y + row_height}, g_menu.small_font,
              theme::kTextMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const std::wstring process_text = status.game_process.empty() ? L"未启动" : status.game_process;
    draw_text(dc, process_text.c_str(), RECT{left + label_width + S(8), y, right, y + row_height}, g_menu.body_font,
              status.game_process.empty() ? theme::kTextMuted : theme::kTextPrimary,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    y += row_height;
    // 第三行：当前关卡与战斗时间。
    draw_text(dc, L"当前关卡", RECT{left, y, left + label_width, y + row_height}, g_menu.small_font,
              theme::kTextMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    std::wstring level_text;
    if (status.in_battle)
    {
        level_text = status.level_id.empty() ? L"未知关卡" : utf8_to_wide(status.level_id);
        level_text += L" · ";
        level_text += format_duration(status.time_seconds);
    }
    else if (status.battle_valid && status.battle_state == 3)
    {
        level_text = L"战斗已结束";
    }
    else
    {
        level_text = L"未在战斗中";
    }
    draw_text(dc, level_text.c_str(), RECT{left + label_width + S(8), y, right, y + row_height}, g_menu.body_font,
              status.in_battle ? theme::kTextPrimary : theme::kTextMuted,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void draw_switch(HDC dc, const RECT& rect, bool enabled)
{
    const int radius = (rect.bottom - rect.top) / 2;
    fill_round_rect(dc, rect, radius, enabled ? theme::kSwitchOn : theme::kSwitchOff);
    const int knob_radius = radius - S(2);
    const int center_y = (rect.top + rect.bottom) / 2;
    const int center_x = enabled ? rect.right - radius : rect.left + radius;
    fill_circle(dc, center_x, center_y, knob_radius, theme::kSwitchKnob);
}

void draw_option_button(HDC dc, const RECT& rect, const wchar_t* text, COLORREF fill, COLORREF fill_hover,
                        COLORREF text_color, bool hovered)
{
    fill_round_rect(dc, rect, S(10), hovered ? fill_hover : fill);
    draw_text(dc, text, rect, g_menu.button_font, text_color, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void release_menu_logo()
{
    if (g_menu.logo != nullptr)
    {
        destroy_bitmap(g_menu.logo);
        g_menu.logo = nullptr;
        g_menu.logo_size = 0;
    }
}

void load_menu_logo()
{
    release_menu_logo();
    const int icon_size = S(22);
    g_menu.logo = load_png_scaled(join_path(module_dir(), L"data\\logo.png"), icon_size, icon_size);
    g_menu.logo_size = icon_size;
}

void draw_gear_icon(HDC dc, const RECT& rect, COLORREF color)
{
    const int cx = (rect.left + rect.right) / 2;
    const int cy = (rect.top + rect.bottom) / 2;
    // A six-tooth silhouette with an open centre stays legible at small sizes.
    // The old spokes crossed the ring and collapsed into a dense star.
    POINT points[52]{};
    constexpr double pi = 3.14159265358979323846;
    for (int i = 0; i < 36; ++i) {
        const double angle = -pi / 2 + i * 2 * pi / 36;
        const double radius = S(i % 6 == 2 || i % 6 == 3 ? 9 : 7);
        points[i] = {cx + static_cast<LONG>(std::lround(radius * std::cos(angle))),
                     cy + static_cast<LONG>(std::lround(radius * std::sin(angle)))};
    }
    for (int i = 0; i < 16; ++i) {
        const double angle = i * 2 * pi / 16;
        points[36 + i] = {cx + static_cast<LONG>(std::lround(S(3) * std::cos(angle))),
                         cy + static_cast<LONG>(std::lround(S(3) * std::sin(angle)))};
    }
    HBRUSH brush = CreateSolidBrush(color);
    const HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    const HGDIOBJ old_brush = SelectObject(dc, brush);
    const int old_mode = SetPolyFillMode(dc, ALTERNATE);
    const int counts[] = {36, 16};
    PolyPolygon(dc, points, counts, 2);
    SetPolyFillMode(dc, old_mode);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(brush);
}

void draw_close_icon(HDC dc, const RECT& rect, COLORREF color)
{
    const int cx = (rect.left + rect.right) / 2;
    const int cy = (rect.top + rect.bottom) / 2;
    const int half = S(11) / 2;
    HPEN pen = CreatePen(PS_SOLID, S(2), color);
    const HGDIOBJ old_pen = SelectObject(dc, pen);
    MoveToEx(dc, cx - half, cy - half, nullptr);
    LineTo(dc, cx + half + 1, cy + half + 1);
    MoveToEx(dc, cx + half, cy - half, nullptr);
    LineTo(dc, cx - half - 1, cy + half + 1);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}


const wchar_t* feature_label(std::uint32_t bit) {
    switch (bit) {
        case ipc::kFeatureSpawnTimer:
            return L"出怪倒计时";
        case ipc::kFeatureSpawnList:
            return L"出怪列表";
        case ipc::kFeatureDamageNumbers:
            return L"数值统计";
        case ipc::kFeatureHpBars:
            return L"敌方血量";
        case ipc::kFeatureWiki: return L"敌方数据";
        case ipc::kFeatureEnemyPrts: return L"敌方 PRTS";
        case ipc::kFeatureEnemyRange:
            return L"远程攻击范围";
        case ipc::kFeatureOperatorStats: return L"我方数值";
        case ipc::kFeatureEnemyPaths: return L"敌方路径";
        case ipc::kFeatureEnemyStats: return L"敌方数值";
        default:
            return L"";
    }
}

bool feature_has_pause_option(std::uint32_t bit) {
    return bit == ipc::kFeatureSpawnTimer || bit == ipc::kFeatureHpBars || bit == ipc::kFeatureEnemyRange;
}

// 配置入口：两个滑杆的小图标，放在每个开关行最左侧。

// ---- 「数值统计」配置页 -------------------------------------------------
std::vector<std::wstring> split_order(const std::wstring& order) {
    std::vector<std::wstring> result;
    if(g_menu.config_bit==ipc::kFeatureEnemyStats) {
        for(int metric:enemy_metrics::parse_order(order)) result.push_back(enemy_metrics::kMetrics[metric].id);
        return result;
    }
    for (int metric : statistics::parse_order(order)) result.push_back(statistics::kMetrics[metric].id);
    return result;
}

std::wstring join_order(const std::vector<std::wstring>& parts) {
    std::wstring result;
    for (const auto& part : parts) {
        if (!result.empty()) result += L',';
        result += part;
    }
    return result;
}

int metric_id(const std::wstring& id) {
    if(g_menu.config_bit==ipc::kFeatureEnemyStats) {
        for(std::size_t metric=0;metric<enemy_metrics::kMetrics.size();++metric)
            if(id==enemy_metrics::kMetrics[metric].id) return static_cast<int>(metric);
        return 0;
    }
    for (int metric = 0; metric < static_cast<int>(ipc::kStatCount); ++metric)
        if (id == statistics::kMetrics[metric].id) return metric;
    return 0;
}

const wchar_t* metric_label(const std::wstring& id) {
    if(g_menu.config_bit==ipc::kFeatureEnemyStats) return enemy_metrics::kMetrics[metric_id(id)].label;
    return statistics::kMetrics[metric_id(id)].label;
}

RECT damage_row_rect(const MenuLayout& layout, int index) {
    const int top = feature_content_top() + S((g_menu.config_bit==ipc::kFeatureOperatorStats || g_menu.config_bit==ipc::kFeatureEnemyStats)?272:48) +
        (index < static_cast<int>(ipc::kStatCount) ? index * S(30) :
         S(30 * ipc::kStatCount) + (index - static_cast<int>(ipc::kStatCount)) * S(36));
    return RECT{layout.content.left + layout.padding, top, layout.content.right - layout.padding,
                top + (index < static_cast<int>(ipc::kStatCount) ? S(29) : S(32))};
}

RECT row_switch(const RECT& row) {
    const int top = row.top + (row.bottom - row.top - S(20)) / 2;
    return RECT{row.right - S(40), top, row.right - S(4), top + S(20)};
}
bool hit_row_switch(const RECT& row, POINT point) {
    const auto sw = row_switch(row); return PtInRect(&sw, point);
}
void draw_damage_row_toggle(HDC dc, const RECT& row, bool enabled) {
    draw_switch(dc, row_switch(row), enabled);
}

void draw_arrow_button(HDC dc, const RECT& rect, bool up, bool hover) {
    const COLORREF color = hover ? theme::kTextPrimary : theme::kTextSecondary;
    fill_round_rect(dc, rect, S(4), hover ? theme::kCardHover : theme::kCard);
    const int cx = (rect.left + rect.right) / 2;
    const int cy = (rect.top + rect.bottom) / 2;
    const int half = S(3);
    POINT points[3];
    if (up) {
        points[0] = POINT{cx - half, cy + half / 2};
        points[1] = POINT{cx + half, cy + half / 2};
        points[2] = POINT{cx, cy - half};
    } else {
        points[0] = POINT{cx - half, cy - half / 2};
        points[1] = POINT{cx + half, cy - half / 2};
        points[2] = POINT{cx, cy + half};
    }
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    Polygon(dc, points, 3);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void draw_damage_config_page(HDC dc) {
    const auto& layout = g_menu.layout;
    const auto& config = g_menu.damage;
    const auto order = split_order(config.order);
    const int left = layout.content.left + layout.padding;
    const int right = layout.content.right - layout.padding;
    const bool operators=g_menu.config_bit==ipc::kFeatureOperatorStats || g_menu.config_bit==ipc::kFeatureEnemyStats;
    const int top=feature_content_top()+S(operators?224:0);
    const auto heading=operators?L"最多选择 6 类，箭头调整显示顺序":L"开关选择显示列，箭头调整列顺序";
    draw_text(dc,heading,
              RECT{left,top,right,top+S(22)},
              g_menu.small_font,theme::kTextSecondary,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    draw_text(dc,L"指标",RECT{left+S(8),top+S(24),right-S(130),top+S(44)},
              g_menu.small_font,theme::kTextMuted,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    draw_text(dc,L"顺序",RECT{right-S(126),top+S(24),right-S(70),top+S(44)},
              g_menu.small_font,theme::kTextMuted,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    draw_text(dc,L"显示",RECT{right-S(44),top+S(24),right,top+S(44)},
              g_menu.small_font,theme::kTextMuted,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    for (std::size_t i = 0; i < order.size(); ++i) {
        const RECT row = damage_row_rect(layout, static_cast<int>(i));
        if (i % 2 == 0) fill_round_rect(dc,row,S(5),theme::kBackgroundAlt);
        draw_text(dc, metric_label(order[i]), RECT{row.left + S(8),row.top,row.right-S(130),row.bottom},
                  g_menu.body_font, theme::kTextPrimary, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        const RECT up{row.right-S(126),row.top+S(2),row.right-S(100),row.bottom-S(2)};
        const RECT down{row.right-S(96),row.top+S(2),row.right-S(70),row.bottom-S(2)};
        draw_arrow_button(dc,up,true,g_menu.hover==static_cast<int>(HitTarget::kDamageUpFirst)+static_cast<int>(i));
        draw_arrow_button(dc,down,false,g_menu.hover==static_cast<int>(HitTarget::kDamageDownFirst)+static_cast<int>(i));
        draw_damage_row_toggle(dc,row,(config.metric_mask&(1u<<metric_id(order[i])))!=0);
    }
    if (operators) return;
    auto compact = damage_row_rect(layout,ipc::kStatCount);
    draw_text(dc,L"精简显示首个已选指标",RECT{compact.left+S(8),compact.top,compact.right-S(60),compact.bottom},
              g_menu.body_font,theme::kTextPrimary,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    draw_damage_row_toggle(dc,compact,config.merged);
    auto sort = damage_row_rect(layout,ipc::kStatCount+1);
    const int metric = config.sort_metric > 0 && config.sort_metric <= static_cast<int>(ipc::kStatCount)
        ? config.sort_metric-1 : ipc::kStatDamage;
    std::wstring text = L"排序：" + std::wstring(statistics::kMetrics[metric].label) + L" ↓";
    draw_text(dc,text.c_str(),RECT{sort.left+S(8),sort.top,sort.right-S(60),sort.bottom},
              g_menu.body_font,theme::kTextPrimary,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    draw_option_button(dc,RECT{sort.right-S(66),sort.top,sort.right-S(8),sort.bottom},L"选择",
        theme::kCard,theme::kCardHover,theme::kTextPrimary,g_menu.hover==static_cast<int>(HitTarget::kDamageSort));
    auto preset=damage_row_rect(layout,ipc::kStatCount+2);
    draw_option_button(dc,preset,L"显示预设：常用 / 伤害 / 辅助 / 全部",
        theme::kCard,theme::kCardHover,theme::kTextPrimary,g_menu.hover==static_cast<int>(HitTarget::kDamagePreset));
}

HitTarget hit_test_damage_config(POINT point) {
    const auto& layout=g_menu.layout;
    const int count=g_menu.config_bit==ipc::kFeatureEnemyStats?static_cast<int>(enemy_metrics::kMetrics.size()):static_cast<int>(ipc::kStatCount);
    for (int i=0;i<count;++i) {
        const auto row=damage_row_rect(layout,i);
        RECT up{row.right-S(126),row.top+S(2),row.right-S(100),row.bottom-S(2)};
        RECT down{row.right-S(96),row.top+S(2),row.right-S(70),row.bottom-S(2)};
        if(PtInRect(&up,point)) return static_cast<HitTarget>(static_cast<int>(HitTarget::kDamageUpFirst)+i);
        if(PtInRect(&down,point)) return static_cast<HitTarget>(static_cast<int>(HitTarget::kDamageDownFirst)+i);
        if(hit_row_switch(row,point)) return static_cast<HitTarget>(static_cast<int>(HitTarget::kDamageToggleFirst)+i);
    }
    if(g_menu.config_bit==ipc::kFeatureOperatorStats || g_menu.config_bit==ipc::kFeatureEnemyStats) return HitTarget::kNone;
    auto row=damage_row_rect(layout,ipc::kStatCount);
    if(hit_row_switch(row,point)) return static_cast<HitTarget>(static_cast<int>(HitTarget::kDamageToggleFirst)+ipc::kStatCount);
    row=damage_row_rect(layout,ipc::kStatCount+1);
    if(PtInRect(&row,point)) return HitTarget::kDamageSort;
    row=damage_row_rect(layout,ipc::kStatCount+2);
    if(PtInRect(&row,point)) return HitTarget::kDamagePreset;
    return HitTarget::kNone;
}

void apply_damage_config() {
    if(g_menu.config_bit==ipc::kFeatureEnemyStats) {
        g_menu.overlay.enemy_metric_mask=g_menu.damage.metric_mask;
        g_menu.overlay.enemy_order=g_menu.damage.order;
        if(g_menu.callbacks.on_overlay_options) g_menu.callbacks.on_overlay_options(g_menu.overlay);
        return;
    }
    if(g_menu.config_bit==ipc::kFeatureOperatorStats) {
        g_menu.overlay.operator_metric_mask=g_menu.damage.metric_mask;
        g_menu.overlay.operator_order=g_menu.damage.order;
        if(g_menu.callbacks.on_overlay_options) g_menu.callbacks.on_overlay_options(g_menu.overlay);
        return;
    }
    if (g_menu.callbacks.on_damage_config) {
        g_menu.callbacks.on_damage_config(g_menu.damage);
    }
}

int slider_to_x(const RECT& track, int value, int minimum)
{
    return track.left + (track.right-track.left)*(std::clamp(value,minimum,100)-minimum)/(100-minimum);
}

void draw_slider(HDC dc, const RECT& track, int value, int minimum)
{
    fill_round_rect(dc,track,S(2),theme::kSwitchOff);
    RECT active=track;
    active.right=slider_to_x(track,value,minimum);
    fill_round_rect(dc,active,S(2),theme::kAccent);
    fill_circle(dc,active.right,(active.top+active.bottom)/2,S(8),theme::kSwitchKnob);
}

struct ExtraControl {
    HitTarget target;
    const wchar_t* label;
    int value, minimum, maximum, step;
    RECT row{}, track{};
};

std::vector<ExtraControl> extra_controls()
{
    std::vector<ExtraControl> controls;
    const auto& options=g_menu.overlay;
    int top=feature_content_top();
    const int left=g_menu.layout.content.left+g_menu.layout.padding;
    const int right=g_menu.layout.content.right-g_menu.layout.padding;
    const auto add=[&](HitTarget target,const wchar_t* label,int value,int minimum,int maximum,int step) {
        const bool toggle=maximum==1 && minimum==0;
        const int height=S(toggle?40:64);
        ExtraControl control{target,label,value,minimum,maximum,step,{left,top,right,top+height},{}};
        control.track=toggle?RECT{right-S(44),top+S(8),right,top+S(32)}:
            RECT{left+S(9),top+S(42),right-S(9),top+S(46)};
        controls.push_back(control);
        top+=height;
    };
    if(g_menu.config_bit==ipc::kFeatureHpBars) {
        top+=S(88);
        add(HitTarget::kHpHover,L"仅鼠标在敌人附近时显示",options.hp_only_hover,0,1,1);
        add(HitTarget::kHpX,L"横向偏移",options.hp_offset_x,-200,200,1);
        add(HitTarget::kHpY,L"纵向偏移",options.hp_offset_y,-200,200,1);
    } else if(g_menu.config_bit==ipc::kFeatureOperatorStats) {
        add(HitTarget::kOperatorPause,L"仅暂停时显示",options.operator_only_paused,0,1,1);
        add(HitTarget::kOperatorHover,L"仅鼠标悬停干员时显示",options.operator_only_hover,0,1,1);
        add(HitTarget::kOperatorX,L"横向偏移",options.operator_offset_x,-200,200,1);
        add(HitTarget::kOperatorY,L"纵向偏移",options.operator_offset_y,-200,200,1);
    } else if(g_menu.config_bit==ipc::kFeatureEnemyStats) {
        top+=S(30);
        add(HitTarget::kEnemyPause,L"仅暂停时显示",options.enemy_only_paused,0,1,1);
        add(HitTarget::kEnemyX,L"横向偏移",options.enemy_offset_x,-200,200,1);
        add(HitTarget::kEnemyY,L"纵向偏移",options.enemy_offset_y,-200,200,1);
    } else if(g_menu.config_bit==ipc::kFeatureEnemyPaths) {
        add(HitTarget::kPathClick,L"点击敌人后触发",options.path_on_click,0,1,1);
        add(HitTarget::kPathSpeed,L"绘制速度",options.path_speed,50,2000,10);
        add(HitTarget::kPathLinger,L"离开后的残留时间",options.path_linger_ms,0,10000,100);
        add(HitTarget::kLineWidth,L"线条粗细",g_menu.appearance.line_width,1,8,1);
    } else if(g_menu.config_bit==ipc::kFeatureEnemyRange) {
        top+=S(88);
        add(HitTarget::kLineWidth,L"线条粗细",g_menu.appearance.line_width,1,8,1);
    } else if(g_menu.config_bit==kSettingsPage) {
        top=g_menu.layout.viewport.top+S(270);
        add(HitTarget::kCacheLimit,L"PRTS 缓存上限",g_menu.cache_mb,10,5120,10);
    }
    return controls;
}

void draw_extra_controls(HDC dc)
{
    for(const auto& control : extra_controls()) {
        const bool toggle=control.maximum==1;
        draw_text(dc,control.label,RECT{control.row.left,control.row.top,
            control.row.right-S(toggle?56:98),control.row.top+S(28)},g_menu.body_font,
            theme::kTextPrimary,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        if(toggle) { draw_switch(dc,control.track,control.value!=0); continue; }
        std::wstring value=std::to_wstring(control.value)+L" px";
        if(control.target==HitTarget::kPathSpeed) value=std::to_wstring(control.value)+L" px/s";
        else if(control.target==HitTarget::kCacheLimit) value=std::to_wstring(control.value)+L" MB";
        else if(control.target==HitTarget::kPathLinger) {
            wchar_t text[32]{}; std::swprintf(text,32,L"%.1f s",control.value/1000.0); value=text;
        }
        draw_text(dc,value.c_str(),RECT{control.row.right-S(98),control.row.top,
            control.row.right,control.row.top+S(28)},g_menu.body_font,theme::kTextSecondary,
            DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
        fill_round_rect(dc,control.track,S(2),theme::kSwitchOff);
        RECT active=control.track;
        active.right=active.left+(control.track.right-control.track.left)*
            (control.value-control.minimum)/(control.maximum-control.minimum);
        fill_round_rect(dc,active,S(2),theme::kAccent);
        fill_circle(dc,active.right,(active.top+active.bottom)/2,S(8),theme::kSwitchKnob);
    }
}

RECT line_style_rect(int index)
{
    const int left=g_menu.layout.content.left+g_menu.layout.padding;
    const int right=g_menu.layout.content.right-g_menu.layout.padding;
    const int width=(right-left-2*S(6))/3;
    const int top=feature_content_top()+S(g_menu.config_bit==ipc::kFeatureEnemyRange?160:240);
    return RECT{left+index*(width+S(6)),top,left+index*(width+S(6))+width,top+S(32)};
}

void draw_line_styles(HDC dc)
{
    if(g_menu.config_bit!=ipc::kFeatureEnemyRange && g_menu.config_bit!=ipc::kFeatureEnemyPaths) return;
    constexpr const wchar_t* labels[]={L"实线",L"虚线",L"点线"};
    for(int i=0;i<3;++i) draw_option_button(dc,line_style_rect(i),labels[i],
        g_menu.appearance.line_style==i?theme::kAccentDeep:theme::kCard,theme::kCardHover,
        theme::kTextPrimary,g_menu.hover==static_cast<int>(HitTarget::kLineStyleFirst)+i);
}

RECT cache_path_button(int index)
{
    const int left=g_menu.layout.content.left+g_menu.layout.padding;
    const int right=g_menu.layout.content.right-g_menu.layout.padding;
    const int width=(right-left-2*S(6))/3;
    return RECT{left+index*(width+S(6)),g_menu.layout.viewport.top+S(402),
        left+index*(width+S(6))+width,g_menu.layout.viewport.top+S(434)};
}

void draw_cache_path(HDC dc)
{
    const int left=g_menu.layout.content.left+g_menu.layout.padding;
    const int right=g_menu.layout.content.right-g_menu.layout.padding;
    const int top=g_menu.layout.viewport.top;
    draw_text(dc,L"PRTS 缓存路径",RECT{left,top+S(344),right,top+S(366)},g_menu.body_font,
        theme::kTextPrimary,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    draw_text(dc,g_menu.cache_path.c_str(),RECT{left,top+S(368),right,top+S(394)},g_menu.small_font,
        theme::kTextSecondary,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_PATH_ELLIPSIS);
    constexpr const wchar_t* labels[]={L"打开目录",L"修改路径",L"默认路径"};
    for(int i=0;i<3;++i) draw_option_button(dc,cache_path_button(i),labels[i],theme::kCard,theme::kCardHover,
        theme::kTextPrimary,g_menu.hover==static_cast<int>(HitTarget::kCacheOpen)+i);
    draw_text(dc,L"更改后使用新目录，原缓存保留。",RECT{left,top+S(442),right,top+S(464)},
        g_menu.small_font,theme::kTextMuted,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    if(!g_menu.cache_error.empty()) draw_text(dc,g_menu.cache_error.c_str(),RECT{left,top+S(466),right,top+S(494)},
        g_menu.small_font,theme::kDanger,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
}

HitTarget hit_test_extra(POINT point)
{
    for(const auto& control:extra_controls()) {
        RECT hit=control.track;
        if(control.maximum!=1) InflateRect(&hit,S(9),S(14));
        if(PtInRect(&hit,point)) return control.target;
    }
    return HitTarget::kNone;
}

bool set_extra_value(HitTarget target,int value)
{
    auto& options=g_menu.overlay;
    switch(target) {
    case HitTarget::kHpHover: options.hp_only_hover=value!=0; break;
    case HitTarget::kHpX: options.hp_offset_x=value; break;
    case HitTarget::kHpY: options.hp_offset_y=value; break;
    case HitTarget::kOperatorPause: options.operator_only_paused=value!=0; break;
    case HitTarget::kOperatorHover: options.operator_only_hover=value!=0; break;
    case HitTarget::kOperatorX: options.operator_offset_x=value; break;
    case HitTarget::kOperatorY: options.operator_offset_y=value; break;
    case HitTarget::kEnemyPause: options.enemy_only_paused=value!=0; break;
    case HitTarget::kEnemyX: options.enemy_offset_x=value; break;
    case HitTarget::kEnemyY: options.enemy_offset_y=value; break;
    case HitTarget::kLineWidth:
        g_menu.appearance.line_width=value;
        if(g_menu.callbacks.on_appearance) g_menu.callbacks.on_appearance(g_menu.config_bit,g_menu.appearance);
        return true;
    case HitTarget::kPathClick: options.path_on_click=value!=0; break;
    case HitTarget::kPathSpeed: options.path_speed=value; break;
    case HitTarget::kPathLinger: options.path_linger_ms=value; break;
    case HitTarget::kCacheLimit:
        g_menu.cache_mb=value;
        if(g_menu.callbacks.on_prts_cache_limit) g_menu.callbacks.on_prts_cache_limit(value);
        return true;
    default: return false;
    }
    if(g_menu.callbacks.on_overlay_options) g_menu.callbacks.on_overlay_options(options);
    return true;
}

COLORREF rgb_color(std::int32_t color)
{
    return RGB((color>>16)&255,(color>>8)&255,color&255);
}

void draw_appearance_page(HDC dc)
{
    const auto& layout=g_menu.layout;
    const int left=layout.content.left+layout.padding;
    const int right=layout.content.right-layout.padding;
    const int top=layout.viewport.top;
    draw_text(dc,L"外观",RECT{left,top,right,top+S(22)},g_menu.small_font,theme::kTextMuted,
              DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    draw_text(dc,L"不透明度",RECT{left,top+S(26),right-S(60),top+S(48)},g_menu.body_font,
              theme::kTextPrimary,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    const std::wstring opacity=std::to_wstring(g_menu.appearance.opacity)+L"%";
    draw_text(dc,opacity.c_str(),RECT{right-S(60),top+S(26),right,top+S(48)},g_menu.body_font,
              theme::kTextSecondary,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
    draw_slider(dc,layout.appearance_opacity_track,g_menu.appearance.opacity,20);
    if(g_menu.config_bit==ipc::kFeatureEnemyRange)
    {
        draw_option_button(dc,layout.appearance_reset,L"恢复本功能外观默认",theme::kCard,theme::kCardHover,
                           theme::kTextSecondary,g_menu.hover==static_cast<int>(HitTarget::kAppearanceReset));
        fill_rect(dc,RECT{left,top+S(124),right,top+S(125)},theme::kDivider);
        return;
    }
    if(g_menu.config_bit!=ipc::kFeatureEnemyPaths) {
    draw_text(dc,L"字体大小",RECT{left,layout.appearance_font.top,layout.appearance_font.left-S(8),
              layout.appearance_font.bottom},g_menu.body_font,theme::kTextPrimary,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    const std::wstring font=g_menu.appearance.font_size>0?std::to_wstring(g_menu.appearance.font_size)+L" px":L"默认";
    draw_option_button(dc,layout.appearance_font,font.c_str(),theme::kCard,theme::kCardHover,theme::kTextPrimary,
                       g_menu.hover==static_cast<int>(HitTarget::kAppearanceFont));
    }
    draw_text(dc,g_menu.config_bit==ipc::kFeatureEnemyPaths?L"路径颜色":L"文字颜色",RECT{left,layout.appearance_color.top,layout.appearance_color.left-S(8),
              layout.appearance_color.bottom},g_menu.body_font,theme::kTextPrimary,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    wchar_t color_text[16]=L"默认";
    if(g_menu.appearance.text_color>=0)
        std::swprintf(color_text,16,L"#%06X",static_cast<unsigned>(g_menu.appearance.text_color));
    draw_option_button(dc,layout.appearance_color,color_text,theme::kCard,theme::kCardHover,
        theme::kTextPrimary,
        g_menu.hover==static_cast<int>(HitTarget::kAppearanceColor));
    fill_circle(dc,layout.appearance_color.left+S(13),
        (layout.appearance_color.top+layout.appearance_color.bottom)/2,S(5),
        g_menu.appearance.text_color>=0?rgb_color(g_menu.appearance.text_color):theme::kTextPrimary);
    draw_option_button(dc,layout.appearance_reset,L"恢复本功能外观默认",theme::kCard,theme::kCardHover,
                       theme::kTextSecondary,g_menu.hover==static_cast<int>(HitTarget::kAppearanceReset));
    fill_rect(dc,RECT{left,top+S(appearance_height()-6),right,top+S(appearance_height()-5)},theme::kDivider);
}

RECT wiki_field_rect(int index)
{
    const auto& layout=g_menu.layout;
    const int top=feature_content_top()+S(32)+index*S(32);
    return RECT{layout.content.left+layout.padding,top,layout.content.right-layout.padding,top+S(32)};
}

std::vector<WikiFieldSpec> wiki_page_fields()
{
    std::vector<WikiFieldSpec> fields;
    for (const auto& field : kWikiFields) {
        const bool reference = (field.bit & (kWikiPrtsBasic | kWikiPrtsStats | kWikiPrtsAbilities)) != 0;
        if (g_menu.config_bit == ipc::kFeatureEnemyPrts ? field.bit != kWikiHp : !reference && field.bit != kWikiSource && field.bit != kWikiHp) fields.push_back(field);
    }
    return fields;
}

RECT wiki_fields_button(bool reset)
{
    const auto& layout=g_menu.layout;
    const int left=layout.content.left+layout.padding;
    const int right=layout.content.right-layout.padding;
    const int mid=(left+right)/2;
    const int top=feature_content_top()+S(32+static_cast<int>(wiki_page_fields().size())*32+8);
    return reset?RECT{left,top,mid-S(4),top+S(32)}:RECT{mid+S(4),top,right,top+S(32)};
}

void draw_wiki_fields(HDC dc)
{
    const auto& layout=g_menu.layout;
    draw_text(dc,L"显示的信息",RECT{layout.content.left+layout.padding,feature_content_top(),
              layout.content.right-layout.padding,feature_content_top()+S(26)},g_menu.small_font,
              theme::kTextMuted,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    const auto fields = wiki_page_fields();
    for(int i=0;i<static_cast<int>(fields.size());++i)
    {
        const RECT row=wiki_field_rect(i);
        if(i%2==0) fill_round_rect(dc,row,S(5),theme::kBackgroundAlt);
        draw_text(dc,fields[i].label,RECT{row.left+S(8),row.top,row.right-S(56),row.bottom},
                  g_menu.body_font,theme::kTextPrimary,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        draw_damage_row_toggle(dc,row,(g_menu.wiki_fields&fields[i].bit)!=0);
    }
    draw_option_button(dc,wiki_fields_button(true),L"显示全部",theme::kCard,theme::kCardHover,
                       theme::kTextPrimary,g_menu.hover==static_cast<int>(HitTarget::kWikiFieldsDefault));
    draw_option_button(dc,wiki_fields_button(false),L"隐藏全部",theme::kCard,theme::kCardHover,
                       theme::kTextSecondary,g_menu.hover==static_cast<int>(HitTarget::kWikiFieldsNone));
}

RECT managed_feature_row(int index)
{
    const int top = g_menu.layout.viewport.top + S(588 + index * 36);
    return RECT{g_menu.layout.content.left + g_menu.layout.padding, top,
        g_menu.layout.content.right - g_menu.layout.padding, top + S(34)};
}
void draw_feature_management(HDC dc)
{
    const auto& layout = g_menu.layout;
    const int top = layout.viewport.top + S(550);
    draw_text(dc,L"菜单功能 · 关闭后隐藏并停用",RECT{layout.content.left+layout.padding,top,
        layout.content.right-layout.padding,top+S(30)},g_menu.small_font,theme::kTextSecondary,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    for(int i=0;i<kFeatureCount;++i) {
        const auto row = managed_feature_row(i);
        if(i%2==0) fill_round_rect(dc,row,S(5),theme::kBackgroundAlt);
        draw_text(dc,feature_label(g_menu.feature_order[i]),RECT{row.left+S(6),row.top,row.right-S(112),row.bottom},
            g_menu.body_font,theme::kTextPrimary,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        const RECT up{row.right-S(110),row.top+S(3),row.right-S(84),row.bottom-S(3)};
        const RECT down{row.right-S(80),row.top+S(3),row.right-S(54),row.bottom-S(3)};
        draw_arrow_button(dc,up,true,g_menu.hover==static_cast<int>(HitTarget::kManageUpFirst)+i);
        draw_arrow_button(dc,down,false,g_menu.hover==static_cast<int>(HitTarget::kManageDownFirst)+i);
        draw_damage_row_toggle(dc,row,(g_menu.menu_features&g_menu.feature_order[i])!=0);
    }
}

void draw_settings_page(HDC dc)
{
    draw_feature_management(dc);
    draw_extra_controls(dc);
    draw_cache_path(dc);
    const auto& layout = g_menu.layout;
    const int left = layout.content.left + layout.padding;
    const int right = layout.content.right - layout.padding;
    const int top = layout.viewport.top;
    draw_text(dc, L"界面缩放", RECT{left,top,right,top+S(22)}, g_menu.body_font,
              theme::kTextPrimary, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    for (int i=0;i<4;++i)
    {
        const bool selected = g_menu.ui_scale == kScaleOptions[i];
        const std::wstring text = std::to_wstring(kScaleOptions[i]) + L"%";
        draw_option_button(dc,layout.scale_buttons[i],text.c_str(),
            selected ? theme::kAccentDeep : theme::kCard,
            selected ? theme::kAccentDeep : theme::kCardHover,theme::kTextPrimary,
            g_menu.hover==static_cast<int>(HitTarget::kScaleFirst)+i);
    }
    draw_text(dc,L"悬浮球不透明度",RECT{left,top+S(78),right-S(56),top+S(100)},
              g_menu.body_font,theme::kTextPrimary,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    const std::wstring opacity = std::to_wstring(g_menu.opacity) + L"%";
    draw_text(dc,opacity.c_str(),RECT{right-S(56),top+S(78),right,top+S(100)},
              g_menu.body_font,theme::kTextSecondary,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
    draw_slider(dc,layout.opacity_track,g_menu.opacity,40);
    draw_text(dc,L"菜单不透明度",RECT{left,top+S(146),right-S(56),top+S(168)},
              g_menu.body_font,theme::kTextPrimary,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    const std::wstring menu_opacity=std::to_wstring(g_menu.menu_opacity)+L"%";
    draw_text(dc,menu_opacity.c_str(),RECT{right-S(56),top+S(146),right,top+S(168)},
              g_menu.body_font,theme::kTextSecondary,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
    draw_slider(dc,layout.menu_opacity_track,g_menu.menu_opacity,20);
    draw_text(dc,L"随助手启动游戏",RECT{left,layout.auto_launch_row.top,
              layout.auto_launch_switch.left-S(8),layout.auto_launch_row.bottom},g_menu.body_font,
              theme::kTextPrimary,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    draw_switch(dc,layout.auto_launch_switch,g_menu.auto_launch);
    draw_option_button(dc,layout.reset_button,L"恢复默认设置",theme::kCard,theme::kCardHover,
                       theme::kTextSecondary,g_menu.hover==static_cast<int>(HitTarget::kReset));
}

void draw_feature_page(HDC dc)
{
    const auto& layout = g_menu.layout;
    const int left = layout.content.left + layout.padding;
    const int right = layout.content.right - layout.padding;
    const int top = feature_content_top();
    draw_appearance_page(dc);
    draw_extra_controls(dc);
    draw_line_styles(dc);
    if(g_menu.config_bit==ipc::kFeatureOperatorStats) { draw_damage_config_page(dc); return; }
    if(g_menu.config_bit==ipc::kFeatureEnemyStats) {
        draw_text(dc,L"仅悬停时显示附近最近的一个敌人",RECT{left,top,right,top+S(24)},
            g_menu.small_font,theme::kTextSecondary,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        draw_damage_config_page(dc); return;
    }
    if(g_menu.config_bit==ipc::kFeatureEnemyPaths) return;
    if (g_menu.config_bit == ipc::kFeatureWiki || g_menu.config_bit == ipc::kFeatureEnemyPrts)
    {
        draw_wiki_fields(dc);
        return;
    }
    if (g_menu.config_bit == ipc::kFeatureDamageNumbers)
    {
        draw_damage_config_page(dc);
        return;
    }
    if (feature_has_pause_option(g_menu.config_bit))
    {
        const wchar_t* description = g_menu.config_bit == ipc::kFeatureEnemyRange
            ? L"显示敌方实际攻击范围，切出游戏时隐藏。"
            : g_menu.config_bit == ipc::kFeatureHpBars
            ? L"跟随场上敌人显示当前血量。" : L"显示各红门的下一批出怪倒计时。";
        draw_text(dc,description,RECT{left,top,right,top+S(30)},g_menu.small_font,
                  theme::kTextSecondary,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        fill_round_rect(dc,layout.pause_row,S(7),theme::kCard);
        draw_text(dc,L"仅暂停时显示",RECT{left+S(10),layout.pause_row.top,
                  layout.pause_switch.left-S(10),layout.pause_row.bottom},g_menu.body_font,
                  theme::kTextPrimary,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        const bool enabled = g_menu.callbacks.get_only_paused && g_menu.callbacks.get_only_paused(g_menu.config_bit);
        draw_switch(dc,layout.pause_switch,enabled);
    }
    else
    {
        const wchar_t* description = g_menu.config_bit == ipc::kFeatureSpawnList
            ? L"按当前关卡显示后续敌人和出场波次。"
            : L"点击场上的敌人查看资料与当前战斗属性。";
        draw_text(dc,description,RECT{left,top,right,top+S(32)},g_menu.body_font,
                  theme::kTextSecondary,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        draw_text(dc,L"此功能当前无需额外配置。",RECT{left,top+S(36),right,top+S(64)},
                  g_menu.small_font,theme::kTextMuted,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    }
}

void draw_main_page(HDC dc)
{
    const auto& layout = g_menu.layout;
    if (g_menu.callbacks.get_status) draw_status_card(dc,g_menu.callbacks.get_status());
    draw_text(dc,L"叠加层功能",RECT{layout.content.left+layout.padding,layout.status_card.bottom+S(4),
              layout.content.right-layout.padding,layout.status_card.bottom+S(24)},g_menu.small_font,
              theme::kTextMuted,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    const std::uint32_t features = g_menu.callbacks.get_feature_mask ? g_menu.callbacks.get_feature_mask() : 0;
    for (int i=0;i<kFeatureCount;++i)
    {
        if (!(g_menu.menu_features & kFeatureBits[i])) continue;
        if (g_menu.hover==static_cast<int>(HitTarget::kToggleFirst)+i)
            fill_round_rect(dc,layout.toggle_rows[i],S(7),theme::kCard);
        const bool config_hover = g_menu.hover==static_cast<int>(HitTarget::kConfigFirst)+i;
        if (config_hover) fill_round_rect(dc,layout.config_buttons[i],S(5),theme::kCardHover);
        draw_gear_icon(dc,layout.config_buttons[i],config_hover ? theme::kTextPrimary : theme::kTextSecondary);
        draw_text(dc,feature_label(kFeatureBits[i]),RECT{layout.toggle_rows[i].left+S(14),
                  layout.toggle_rows[i].top,layout.config_buttons[i].left-S(8),layout.toggle_rows[i].bottom},
                  g_menu.body_font,theme::kTextPrimary,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        draw_switch(dc,layout.toggle_switches[i],(features&kFeatureBits[i])!=0);
    }
}

void render()
{
    if (g_menu.hwnd==nullptr || !ensure_surface()) return;
    layered_bitmap_clear(g_menu.surface);
    HDC dc = g_menu.surface.dc;
    g_menu.hints.begin(g_menu.hwnd,dc);
    const auto& layout = g_menu.layout;
    fill_rect(dc,layout.content,theme::kBackground);
    fill_rect(dc,layout.title_bar,theme::kBackgroundAlt);
    fill_rect(dc,RECT{layout.content.left,layout.title_bar.bottom,layout.content.right,
                     layout.title_bar.bottom+S(1)},theme::kDivider);
    int title_left = layout.content.left+layout.padding;
    if (g_menu.config_bit==0 && g_menu.logo && g_menu.logo_size>0)
    {
        HDC logo_dc=CreateCompatibleDC(dc);
        if(logo_dc)
        {
            HGDIOBJ old=SelectObject(logo_dc,g_menu.logo);
            const int logo_top=layout.title_bar.top+(layout.title_bar.bottom-layout.title_bar.top-g_menu.logo_size)/2;
            BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
            AlphaBlend(dc,title_left,logo_top,g_menu.logo_size,g_menu.logo_size,logo_dc,0,0,
                       g_menu.logo_size,g_menu.logo_size,blend);
            SelectObject(logo_dc,old);
            DeleteDC(logo_dc);
            title_left+=g_menu.logo_size+S(8);
        }
    }
    const bool settings_page=g_menu.config_bit==kSettingsPage;
    std::wstring title = g_menu.config_bit==0 ? L"ArkHelper" : settings_page ? L"设置" :
                         std::wstring(feature_label(g_menu.config_bit))+L" · 设置";
    draw_text(dc,title.c_str(),RECT{title_left,layout.title_bar.top,
              settings_page ? layout.close_button.left-S(8) : layout.settings_button.left-S(8),layout.title_bar.bottom},
              g_menu.title_font,theme::kTextPrimary,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    if(!settings_page)
    {
        const bool hover=g_menu.hover==static_cast<int>(HitTarget::kSettings);
        if(hover) fill_round_rect(dc,layout.settings_button,S(7),theme::kCard);
        draw_gear_icon(dc,layout.settings_button,hover?theme::kTextPrimary:theme::kTextSecondary);
    }
    const bool close_hover=g_menu.hover==static_cast<int>(HitTarget::kClose);
    if(close_hover) fill_round_rect(dc,layout.close_button,S(7),theme::kCard);
    draw_close_icon(dc,layout.close_button,close_hover?theme::kTextPrimary:theme::kTextSecondary);

    // Clip before translating: drawing and hit testing share viewport.scroll.
    const int saved=SaveDC(dc);
    IntersectClipRect(dc,layout.viewport.left,layout.viewport.top,layout.viewport.right,layout.viewport.bottom);
    SetViewportOrgEx(dc,0,-g_menu.viewport.scroll,nullptr);
    if(g_menu.config_bit==0) draw_main_page(dc);
    else if(settings_page) draw_settings_page(dc);
    else draw_feature_page(dc);
    RestoreDC(dc,saved);
    if(g_menu.viewport.maximum_scroll()>0 && g_menu.viewport.height()>0)
    {
        const int rail_height=g_menu.viewport.height();
        const int thumb_height=std::min(rail_height,std::max(S(22),rail_height*rail_height/g_menu.viewport.content_height));
        const int thumb_top=g_menu.viewport.top+(rail_height-thumb_height)*g_menu.viewport.scroll/g_menu.viewport.maximum_scroll();
        fill_round_rect(dc,RECT{layout.content.right-S(7),thumb_top,layout.content.right-S(4),thumb_top+thumb_height},
                        S(2),theme::kSwitchOff);
    }
    const bool main=g_menu.config_bit==0;
    draw_option_button(dc,layout.back_button,main?L"退出":L"返回",theme::kCard,theme::kCardHover,
                       main?theme::kDanger:theme::kTextPrimary,
                       g_menu.hover==static_cast<int>(main?HitTarget::kExit:HitTarget::kBack));
    if(main) {
        const int cx=(layout.content.left+layout.content.right)/2;
        fill_round_rect(dc,RECT{cx-S(16),layout.content.bottom-S(7),cx+S(16),layout.content.bottom-S(5)},S(1),theme::kTextMuted);
    }
    g_menu.hints.end();
    apply_round_rect_alpha(g_menu.surface,S(kShadowMarginLogical),S(kCornerRadiusLogical),S(kShadowBlurLogical),kShadowAlpha);
    layered_bitmap_present(g_menu.hwnd,g_menu.surface,
        static_cast<BYTE>(g_menu.present_alpha*g_menu.menu_opacity/100));
}

HitTarget hit_test(POINT point)
{
    const auto& layout=g_menu.layout;
    if(PtInRect(&layout.close_button,point)) return HitTarget::kClose;
    if(g_menu.config_bit!=kSettingsPage && PtInRect(&layout.settings_button,point)) return HitTarget::kSettings;
    if(PtInRect(&layout.back_button,point)) return g_menu.config_bit==0?HitTarget::kExit:HitTarget::kBack;
    if(PtInRect(&layout.title_bar,point)) return HitTarget::kTitle;
    if(!PtInRect(&layout.viewport,point) || !g_menu.viewport.contains(point.y)) return HitTarget::kNone;
    point.y=g_menu.viewport.content_y(point.y);
    if(g_menu.config_bit!=0) {
        const auto extra=hit_test_extra(point);
        if(extra!=HitTarget::kNone) return extra;
    }
    if(g_menu.config_bit==kSettingsPage)
    {
        for(int i=0;i<3;++i) {
            const auto button=cache_path_button(i);
            if(PtInRect(&button,point)) return static_cast<HitTarget>(static_cast<int>(HitTarget::kCacheOpen)+i);
        }
        for(int i=0;i<kFeatureCount;++i) {
            const auto row=managed_feature_row(i);
            const RECT up{row.right-S(110),row.top+S(3),row.right-S(84),row.bottom-S(3)};
            const RECT down{row.right-S(80),row.top+S(3),row.right-S(54),row.bottom-S(3)};
            if(hit_row_switch(row,point)) return static_cast<HitTarget>(static_cast<int>(HitTarget::kManageToggleFirst)+i);
            if(PtInRect(&up,point)) return static_cast<HitTarget>(static_cast<int>(HitTarget::kManageUpFirst)+i);
            if(PtInRect(&down,point)) return static_cast<HitTarget>(static_cast<int>(HitTarget::kManageDownFirst)+i);
        }
        for(int i=0;i<4;++i)
            if(PtInRect(&layout.scale_buttons[i],point)) return static_cast<HitTarget>(static_cast<int>(HitTarget::kScaleFirst)+i);
        if(PtInRect(&layout.opacity_hit,point)) return HitTarget::kOpacity;
        if(PtInRect(&layout.menu_opacity_hit,point)) return HitTarget::kMenuOpacity;
        if(PtInRect(&layout.auto_launch_switch,point)) return HitTarget::kAutoLaunch;
        if(PtInRect(&layout.reset_button,point)) return HitTarget::kReset;
    }
    else if(g_menu.config_bit!=0)
    {
        if(g_menu.config_bit==ipc::kFeatureEnemyRange || g_menu.config_bit==ipc::kFeatureEnemyPaths)
            for(int i=0;i<3;++i) {
                const auto button=line_style_rect(i);
                if(PtInRect(&button,point)) return static_cast<HitTarget>(static_cast<int>(HitTarget::kLineStyleFirst)+i);
            }
        if(PtInRect(&layout.appearance_opacity_hit,point)) return HitTarget::kAppearanceOpacity;
        if(g_menu.config_bit!=ipc::kFeatureEnemyRange)
        {
            if(g_menu.config_bit!=ipc::kFeatureEnemyPaths && PtInRect(&layout.appearance_font,point)) return HitTarget::kAppearanceFont;
            if(PtInRect(&layout.appearance_color,point)) return HitTarget::kAppearanceColor;
        }
        if(PtInRect(&layout.appearance_reset,point)) return HitTarget::kAppearanceReset;
        if(g_menu.config_bit==ipc::kFeatureWiki || g_menu.config_bit==ipc::kFeatureEnemyPrts)
        {
            for(int i=0;i<static_cast<int>(wiki_page_fields().size());++i)
            {
                const RECT row=wiki_field_rect(i);
                if(hit_row_switch(row,point)) return static_cast<HitTarget>(static_cast<int>(HitTarget::kWikiFieldFirst)+i);
            }
            const RECT reset=wiki_fields_button(true),none=wiki_fields_button(false);
            if(PtInRect(&reset,point)) return HitTarget::kWikiFieldsDefault;
            if(PtInRect(&none,point)) return HitTarget::kWikiFieldsNone;
        }
        if(g_menu.config_bit==ipc::kFeatureDamageNumbers || g_menu.config_bit==ipc::kFeatureOperatorStats ||
           g_menu.config_bit==ipc::kFeatureEnemyStats)
            return hit_test_damage_config(point);
        if(feature_has_pause_option(g_menu.config_bit) && PtInRect(&layout.pause_switch,point)) return HitTarget::kConfigToggle;
    }
    else
    {
        for(int i=0;i<kFeatureCount;++i)
        {
            if (!(g_menu.menu_features & kFeatureBits[i])) continue;
            if(PtInRect(&layout.config_buttons[i],point)) return static_cast<HitTarget>(static_cast<int>(HitTarget::kConfigFirst)+i);
            if(PtInRect(&layout.toggle_switches[i],point)) return static_cast<HitTarget>(static_cast<int>(HitTarget::kToggleFirst)+i);
        }
    }
    return HitTarget::kNone;
}

void finish_opacity_drag()
{
    if (!g_menu.dragging_opacity) return;
    g_menu.dragging_opacity = false;
    if (GetCapture() == g_menu.hwnd) ReleaseCapture();
    if (g_menu.slider_target == HitTarget::kOpacity && g_menu.callbacks.on_opacity)
        g_menu.callbacks.on_opacity(g_menu.opacity, true);
}

void apply_appearance()
{
    if (g_menu.callbacks.on_appearance) g_menu.callbacks.on_appearance(g_menu.config_bit,g_menu.appearance);
}

void update_opacity(int x)
{
    for(const auto& control:extra_controls()) if(control.target==g_menu.slider_target) {
        const int span=control.track.right-control.track.left;
        if(span<=0) return;
        int value=control.minimum+static_cast<int>(std::lround(
            std::clamp<int>(x-control.track.left,0,span)*
            static_cast<double>(control.maximum-control.minimum)/span));
        value=std::clamp(control.minimum+static_cast<int>(std::lround(
            static_cast<double>(value-control.minimum)/control.step))*control.step,control.minimum,control.maximum);
        if(value!=control.value) set_extra_value(control.target,value);
        render(); return;
    }
    const bool ball=g_menu.slider_target==HitTarget::kOpacity;
    const bool menu=g_menu.slider_target==HitTarget::kMenuOpacity;
    const auto& track=ball?g_menu.layout.opacity_track:
        menu?g_menu.layout.menu_opacity_track:g_menu.layout.appearance_opacity_track;
    const int minimum=ball?40:20;
    const int span = track.right - track.left;
    const int value=span>0?minimum+std::clamp<int>(x-track.left,0,span)*(100-minimum)/span:100;
    int& current=ball?g_menu.opacity:menu?g_menu.menu_opacity:g_menu.appearance.opacity;
    if(value==current) return;
    current=value;
    if(ball && g_menu.callbacks.on_opacity) g_menu.callbacks.on_opacity(value,false);
    else if(menu && g_menu.callbacks.on_menu_opacity) g_menu.callbacks.on_menu_opacity(value);
    else if(!ball && !menu) apply_appearance();
    render();
}

void sync_page_config(const Config& config)
{
    g_menu.opacity=std::clamp(config.opacity,40,100);
    g_menu.menu_opacity=std::clamp(config.menu_opacity,20,100);
    g_menu.ui_scale=config.ui_scale>0?config.ui_scale:100;
    g_menu.auto_launch=config.auto_launch;
    g_menu.wiki_fields=(g_menu.config_bit==ipc::kFeatureEnemyPrts?config.prts_fields:config.wiki_fields)&kWikiAllFields;
    g_menu.menu_features=config.menu_features&kMenuFeatureMask;
    g_menu.feature_order=config.feature_order;
    g_menu.overlay=config.overlay;
    g_menu.cache_mb=config.prts_cache_mb;
    g_menu.main_height=std::clamp(config.menu_height,400,900);
    g_menu.cache_path=g_menu.callbacks.get_prts_cache_path?g_menu.callbacks.get_prts_cache_path():config.prts_cache_path;
    if(g_menu.config_bit==ipc::kFeatureOperatorStats) {
        g_menu.damage.metric_mask=config.overlay.operator_metric_mask;
        g_menu.damage.order=config.overlay.operator_order;
    }
    if(g_menu.config_bit==ipc::kFeatureEnemyStats) {
        g_menu.damage.metric_mask=config.overlay.enemy_metric_mask;
        g_menu.damage.order=config.overlay.enemy_order;
    }
    if(g_menu.config_bit!=0 && g_menu.config_bit!=kSettingsPage)
        g_menu.appearance=feature_appearance(config,g_menu.config_bit);
}

void open_page(std::uint32_t bit)
{
    finish_opacity_drag();
    g_menu.config_bit = bit;
    g_menu.viewport.scroll = 0;
    g_menu.wheel_remainder = 0;
    g_menu.hover = static_cast<int>(HitTarget::kNone);
    if (bit == ipc::kFeatureDamageNumbers && g_menu.callbacks.get_damage_config)
        g_menu.damage = g_menu.callbacks.get_damage_config();
    if (g_menu.callbacks.get_config) sync_page_config(g_menu.callbacks.get_config());
    render();
}

void hide_menu()
{
    g_menu.hints.hide();
    g_menu.pressed = HitTarget::kNone;
    finish_opacity_drag();
    const bool was_visible = g_menu.visible;
    g_menu.visible = false;
    g_menu.config_bit = 0;
    g_menu.viewport.scroll = 0;
    g_menu.wheel_remainder = 0;
    g_menu.hover = static_cast<int>(HitTarget::kNone);
    if (g_menu.hwnd != nullptr)
    {
        ShowWindow(g_menu.hwnd, SW_HIDE);
    }
    log_info("menu hidden");
    if (was_visible && g_menu.callbacks.on_hidden)
    {
        g_menu.callbacks.on_hidden();
    }
}

void begin_drag(HWND hwnd)
{
    g_menu.hints.hide();
    g_menu.dragging = true;
    GetCursorPos(&g_menu.drag_cursor_start);
    GetWindowRect(hwnd, &g_menu.drag_window_start);
    SetCapture(hwnd);
}

void update_hover(POINT point)
{
    const int hover = static_cast<int>(hit_test(point));
    if (hover != g_menu.hover)
    {
        g_menu.hover = hover;
        render();
    }
}

void choose_feature_font()
{
    HMENU options=CreatePopupMenu();
    AppendMenuW(options,MF_STRING|(g_menu.appearance.font_size==0?MF_CHECKED:0),1,L"默认字号");
    for(int size=10;size<=32;++size)
    {
        const std::wstring text=std::to_wstring(size)+L" px";
        AppendMenuW(options,MF_STRING|(g_menu.appearance.font_size==size?MF_CHECKED:0),size+10,text.c_str());
    }
    POINT cursor{};GetCursorPos(&cursor);
    g_menu.popup_open=true;
    const int choice=TrackPopupMenu(options,TPM_RETURNCMD|TPM_NONOTIFY,cursor.x,cursor.y,0,g_menu.hwnd,nullptr);
    g_menu.popup_open=false;
    DestroyMenu(options);
    if(choice>0)
    {
        g_menu.appearance.font_size=choice==1?0:choice-10;
        apply_appearance();render();
    }
}

void choose_feature_color()
{
    struct ColorOption { const wchar_t* label; std::int32_t rgb; };
    constexpr ColorOption colors[]={
        {L"默认文字颜色",-1}, {L"白色",0xFFFFFF}, {L"浅灰",0xC0C0C0},
        {L"黄色",0xFFE066}, {L"绿色",0x62D68A}, {L"蓝色",0x72AEFF},
        {L"橙色",0xFFAE66}, {L"粉色",0xFF99CC}, {L"红色",0xFF6666}, {L"黑色",0x000000},
    };
    HMENU options=CreatePopupMenu();
    for(int i=0;i<static_cast<int>(std::size(colors));++i)
        AppendMenuW(options,MF_STRING|(g_menu.appearance.text_color==colors[i].rgb?MF_CHECKED:0),i+1,colors[i].label);
    AppendMenuW(options,MF_SEPARATOR,0,nullptr);
    AppendMenuW(options,MF_STRING,100,L"自定义 RGB 颜色…");
    POINT cursor{};GetCursorPos(&cursor);
    g_menu.popup_open=true;
    const int choice=TrackPopupMenu(options,TPM_RETURNCMD|TPM_NONOTIFY,cursor.x,cursor.y,0,g_menu.hwnd,nullptr);
    DestroyMenu(options);
    bool changed=false;
    if(choice>0 && choice<=static_cast<int>(std::size(colors)))
    {
        g_menu.appearance.text_color=colors[choice-1].rgb;
        changed=true;
    }
    else if(choice==100)
    {
        static COLORREF custom_colors[16]{};
        CHOOSECOLORW picker{};
        picker.lStructSize=sizeof(picker);
        picker.hwndOwner=g_menu.hwnd;
        picker.rgbResult=g_menu.appearance.text_color>=0?rgb_color(g_menu.appearance.text_color):theme::kTextPrimary;
        picker.lpCustColors=custom_colors;
        picker.Flags=CC_FULLOPEN|CC_RGBINIT;
        if(ChooseColorW(&picker))
        {
            g_menu.appearance.text_color=(GetRValue(picker.rgbResult)<<16)|
                (GetGValue(picker.rgbResult)<<8)|GetBValue(picker.rgbResult);
            changed=true;
        }
    }
    g_menu.popup_open=false;
    if(changed) { apply_appearance();render(); }
}

void commit_cache_path(const std::wstring& path)
{
    if(!g_menu.callbacks.on_prts_cache_path) return;
    g_menu.cache_error=g_menu.callbacks.on_prts_cache_path(path);
    if(g_menu.cache_error.empty() && g_menu.callbacks.get_prts_cache_path)
        g_menu.cache_path=g_menu.callbacks.get_prts_cache_path();
    render();
}

void choose_cache_path()
{
    IFileOpenDialog* dialog=nullptr;
    HRESULT result=CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog));
    if(FAILED(result)) { g_menu.cache_error=L"无法打开目录选择器"; render(); return; }
    DWORD flags=0;
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR);
    dialog->SetTitle(L"选择 PRTS 缓存目录");
    IShellItem* current=nullptr;
    if(SUCCEEDED(SHCreateItemFromParsingName(g_menu.cache_path.c_str(),nullptr,IID_PPV_ARGS(&current)))) {
        dialog->SetFolder(current); current->Release();
    }
    g_menu.popup_open=true;
    result=dialog->Show(g_menu.hwnd);
    g_menu.popup_open=false;
    if(SUCCEEDED(result)) {
        IShellItem* selected=nullptr;
        if(SUCCEEDED(dialog->GetResult(&selected))) {
            PWSTR path=nullptr;
            if(SUCCEEDED(selected->GetDisplayName(SIGDN_FILESYSPATH,&path))) {
                commit_cache_path(path); CoTaskMemFree(path);
            }
            selected->Release();
        }
    } else if(result!=HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        g_menu.cache_error=L"无法选择缓存目录";render();
    }
    dialog->Release();
}

void activate_target(HitTarget target)
{
    if(target>=HitTarget::kLineStyleFirst &&
       static_cast<int>(target)<static_cast<int>(HitTarget::kLineStyleFirst)+3) {
        g_menu.appearance.line_style=static_cast<int>(target)-static_cast<int>(HitTarget::kLineStyleFirst);
        apply_appearance();render();return;
    }
    if(target==HitTarget::kCacheOpen) {
        if(g_menu.callbacks.on_prts_cache_open) g_menu.cache_error=g_menu.callbacks.on_prts_cache_open();
        render();return;
    }
    if(target==HitTarget::kCacheChange) { choose_cache_path();return; }
    if(target==HitTarget::kCacheDefault) { commit_cache_path(L"");return; }
    for (const auto& control : extra_controls()) if (target == control.target && control.maximum == 1) {
        set_extra_value(target, !control.value); render(); return;
    }
    const int action = static_cast<int>(target);
    for (int i = 0; i < kFeatureCount; ++i) {
        if (action == static_cast<int>(HitTarget::kManageToggleFirst) + i) {
            g_menu.menu_features ^= g_menu.feature_order[i];
        } else if (action == static_cast<int>(HitTarget::kManageUpFirst) + i) {
            if (i == 0) return;
            std::swap(g_menu.feature_order[i], g_menu.feature_order[i - 1]);
        } else if (action == static_cast<int>(HitTarget::kManageDownFirst) + i) {
            if (i + 1 == kFeatureCount) return;
            std::swap(g_menu.feature_order[i], g_menu.feature_order[i + 1]);
        } else continue;
        if (g_menu.callbacks.on_feature_menu) g_menu.callbacks.on_feature_menu(g_menu.menu_features,g_menu.feature_order);
        render(); return;
    }
        if (target == HitTarget::kClose)
        {
            hide_menu();
            return;
        }
        if (target == HitTarget::kSettings)
        {
            open_page(kSettingsPage);
            return;
        }
        if (target >= HitTarget::kScaleFirst &&
            target < static_cast<HitTarget>(static_cast<int>(HitTarget::kScaleFirst) + 4))
        {
            const int index = static_cast<int>(target) - static_cast<int>(HitTarget::kScaleFirst);
            if (g_menu.callbacks.on_ui_scale) g_menu.callbacks.on_ui_scale(kScaleOptions[index]);
            return;
        }
        if (target == HitTarget::kAutoLaunch)
        {
            g_menu.auto_launch=!g_menu.auto_launch;
            if(g_menu.callbacks.on_auto_launch) g_menu.callbacks.on_auto_launch(g_menu.auto_launch);
            render();return;
        }
        if (target == HitTarget::kAppearanceFont)
        {
            choose_feature_font();return;
        }
        if (target == HitTarget::kAppearanceColor)
        {
            choose_feature_color();return;
        }
        if (target == HitTarget::kAppearanceReset)
        {
            g_menu.appearance=FeatureAppearance{};
            apply_appearance();render();return;
        }
        if ((target >= HitTarget::kWikiFieldFirst &&
            target < static_cast<HitTarget>(static_cast<int>(HitTarget::kWikiFieldFirst)+16)) ||
            target == HitTarget::kWikiFieldsDefault || target == HitTarget::kWikiFieldsNone)
        {
            if(target==HitTarget::kWikiFieldsDefault) g_menu.wiki_fields=kWikiAllFields;
            else if(target==HitTarget::kWikiFieldsNone) g_menu.wiki_fields=0;
            else g_menu.wiki_fields^=wiki_page_fields()[static_cast<int>(target)-static_cast<int>(HitTarget::kWikiFieldFirst)].bit;
            if(g_menu.callbacks.on_wiki_fields) g_menu.callbacks.on_wiki_fields(g_menu.config_bit,g_menu.wiki_fields);
            render();return;
        }
        if (target == HitTarget::kReset)
        {
            if (g_menu.callbacks.on_reset) g_menu.callbacks.on_reset();
            return;
        }
        if (target == HitTarget::kExit)
        {
            if (g_menu.callbacks.on_exit)
            {
                g_menu.callbacks.on_exit();
            }
            return;
        }
        if (target >= HitTarget::kConfigFirst && target < HitTarget::kBack) {
            const int index = static_cast<int>(target) - static_cast<int>(HitTarget::kConfigFirst);
            if (index >= 0 && index < kFeatureCount) {
                open_page(kFeatureBits[index]);
            }
            return;
        }
        if (target == HitTarget::kBack) {
            open_page(0);
            return;
        }
        if (target == HitTarget::kConfigToggle) {
            if (g_menu.callbacks.get_only_paused && g_menu.callbacks.on_only_paused) {
                const bool current = g_menu.callbacks.get_only_paused(g_menu.config_bit);
                g_menu.callbacks.on_only_paused(g_menu.config_bit, !current);
                render();
            }
            return;
        }
        if (target >= HitTarget::kDamageToggleFirst &&
            target <= static_cast<HitTarget>(static_cast<int>(HitTarget::kDamageToggleFirst) + ipc::kStatCount)) {
            const int index = static_cast<int>(target) - static_cast<int>(HitTarget::kDamageToggleFirst);
            DamageConfig& config = g_menu.damage;
            if (index < static_cast<int>(ipc::kStatCount)) {
                const auto order = split_order(config.order);
                if(index>=static_cast<int>(order.size())) return;
                const auto changed = config.metric_mask ^ (1u << metric_id(order[index]));
                if (changed && ((g_menu.config_bit!=ipc::kFeatureOperatorStats &&
                     g_menu.config_bit!=ipc::kFeatureEnemyStats) || std::popcount(changed)<=6))
                    config.metric_mask = changed;
            } else {
                config.merged = !config.merged;
            }
            apply_damage_config();
            render();
            return;
        }
        if (target >= HitTarget::kDamageUpFirst &&
            target < static_cast<HitTarget>(static_cast<int>(HitTarget::kDamageUpFirst) + ipc::kStatCount)) {
            const int index = static_cast<int>(target) - static_cast<int>(HitTarget::kDamageUpFirst);
            std::vector<std::wstring> order = split_order(g_menu.damage.order);
            if (index > 0) {
                std::swap(order[static_cast<std::size_t>(index)], order[static_cast<std::size_t>(index - 1)]);
                g_menu.damage.order = join_order(order);
                apply_damage_config();
                render();
            }
            return;
        }
        if (target >= HitTarget::kDamageDownFirst &&
            target < static_cast<HitTarget>(static_cast<int>(HitTarget::kDamageDownFirst) + ipc::kStatCount)) {
            const int index = static_cast<int>(target) - static_cast<int>(HitTarget::kDamageDownFirst);
            std::vector<std::wstring> order = split_order(g_menu.damage.order);
            if (index + 1 < static_cast<int>(order.size())) {
                std::swap(order[static_cast<std::size_t>(index)], order[static_cast<std::size_t>(index + 1)]);
                g_menu.damage.order = join_order(order);
                apply_damage_config();
                render();
            }
            return;
        }
        if (target == HitTarget::kDamageSort || target == HitTarget::kDamagePreset) {
            HMENU options = CreatePopupMenu();
            if (target == HitTarget::kDamageSort) {
                for (int metric=0;metric<static_cast<int>(ipc::kStatCount);++metric)
                    AppendMenuW(options,MF_STRING | (g_menu.damage.sort_metric==metric+1 ? MF_CHECKED:0),
                                metric+1,statistics::kMetrics[metric].label);
            } else {
                AppendMenuW(options,MF_STRING,1,L"常用：伤害 / 治疗 / 回费 / DPS / 占比");
                AppendMenuW(options,MF_STRING,2,L"伤害：总计 / 物理 / 法术 / 真实 / 元素 / DPS");
                AppendMenuW(options,MF_STRING,3,L"辅助：治疗 / 回费 / 元素积累 / 承伤");
                AppendMenuW(options,MF_STRING,4,L"全部指标");
            }
            POINT cursor{};GetCursorPos(&cursor);
            g_menu.popup_open = true;
            const int selected=TrackPopupMenu(options,TPM_RETURNCMD|TPM_NONOTIFY,cursor.x,cursor.y,0,g_menu.hwnd,nullptr);
            g_menu.popup_open = false;
            DestroyMenu(options);
            if(selected>0) {
                if(target==HitTarget::kDamageSort) g_menu.damage.sort_metric=selected;
                else {
                    const std::uint32_t masks[]={statistics::kCommonMetrics,0x43Fu,0x1D0u,statistics::kAllMetrics};
                    g_menu.damage.metric_mask=masks[std::min(selected,4)-1];
                    g_menu.damage.merged=false;
                }
                apply_damage_config();render();
            }
            return;
        }
        if (target >= HitTarget::kToggleFirst)
        {
            const int index = static_cast<int>(target) - static_cast<int>(HitTarget::kToggleFirst);
            if (index >= 0 && index < kFeatureCount && g_menu.callbacks.on_feature_toggle)
            {
                const uint32_t mask = g_menu.callbacks.get_feature_mask();
                g_menu.callbacks.on_feature_toggle(kFeatureBits[index], (mask & kFeatureBits[index]) == 0);
            }
            return;
        }
        return;
}

RECT menu_height_grip() {
    const auto& content=g_menu.layout.content;
    return {content.left+S(16),content.bottom-S(10),content.right-S(16),content.bottom};
}
void finish_height_resize() {
    if(!g_menu.resizing_height) return;
    g_menu.resizing_height=false;
    if(GetCapture()==g_menu.hwnd) ReleaseCapture();
    if(g_menu.callbacks.on_menu_height) g_menu.callbacks.on_menu_height(g_menu.main_height);
}

LRESULT CALLBACK menu_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_NCHITTEST:
    {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(hwnd, &point);
        // 投影区域不参与命中，避免挡住球/桌面；alpha 为 0 的区域本来也会穿透。
        if (PtInRect(&g_menu.layout.content, point))
        {
            return HTCLIENT;
        }
        return HTTRANSPARENT;
    }
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_SETCURSOR:
        if(LOWORD(lparam)==HTCLIENT && g_menu.config_bit==0) {
            POINT point{};GetCursorPos(&point);ScreenToClient(hwnd,&point);
            const auto grip=menu_height_grip();
            if(PtInRect(&grip,point)) { SetCursor(LoadCursorW(nullptr,IDC_SIZENS));return TRUE; }
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE && g_menu.visible && !g_menu.dragging && !g_menu.resizing_height && !g_menu.popup_open)
        {
            hide_menu();
        }
        return 0;
    case WM_LBUTTONDOWN:
    {
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        g_menu.hints.hide();
        const auto grip=menu_height_grip();
        if(g_menu.config_bit==0 && PtInRect(&grip,point)) {
            g_menu.pressed=HitTarget::kNone;g_menu.resizing_height=true;
            GetCursorPos(&g_menu.resize_cursor_start);
            g_menu.resize_start_height=g_menu.layout.content.bottom-g_menu.layout.content.top;
            SetCapture(hwnd);return 0;
        }
        const auto target = hit_test(point);
        g_menu.pressed = HitTarget::kNone;
        if (target == HitTarget::kTitle) { begin_drag(hwnd); return 0; }
        bool slider = target == HitTarget::kOpacity || target == HitTarget::kMenuOpacity || target == HitTarget::kAppearanceOpacity;
        for (const auto& control : extra_controls())
            if (target == control.target && control.maximum > 1) slider = true;
        if (slider) {
            g_menu.dragging_opacity = true; g_menu.slider_target = target;
            SetCapture(hwnd); update_opacity(point.x); return 0;
        }
        if (target != HitTarget::kNone) {
            g_menu.pressed = target; g_menu.press_point = point; SetCapture(hwnd);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        if(g_menu.resizing_height) {
            POINT cursor{};GetCursorPos(&cursor);
            g_menu.main_height=std::clamp(static_cast<int>(std::lround(
                (g_menu.resize_start_height+cursor.y-g_menu.resize_cursor_start.y)*100.0/S(100))),400,900);
            render();return 0;
        }
        if (g_menu.dragging_opacity)
        {
            update_opacity(GET_X_LPARAM(lparam));
            return 0;
        }
        if (g_menu.dragging)
        {
            POINT cursor{};
            GetCursorPos(&cursor);
            SetWindowPos(hwnd, HWND_TOPMOST, g_menu.drag_window_start.left + cursor.x - g_menu.drag_cursor_start.x,
                         g_menu.drag_window_start.top + cursor.y - g_menu.drag_cursor_start.y, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE);
            return 0;
        }
        if (g_menu.pressed != HitTarget::kNone &&
            (std::abs(GET_X_LPARAM(lparam) - g_menu.press_point.x) > S(4) ||
             std::abs(GET_Y_LPARAM(lparam) - g_menu.press_point.y) > S(4))) g_menu.pressed = HitTarget::kNone;
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd;
        TrackMouseEvent(&track);
        g_menu.hints.mouse_move(POINT{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)});
        update_hover(POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        return 0;
    }
    case WM_LBUTTONUP:
    {
        if(g_menu.resizing_height) { finish_height_resize();return 0; }
        const auto pressed = g_menu.pressed;
        g_menu.pressed = HitTarget::kNone;
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        finish_opacity_drag();
        g_menu.dragging = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (pressed != HitTarget::kNone && pressed == hit_test(point) &&
            std::abs(point.x - g_menu.press_point.x) <= S(4) && std::abs(point.y - g_menu.press_point.y) <= S(4))
            activate_target(pressed);
        return 0;
    }
    case WM_CAPTURECHANGED:
        finish_height_resize();
        g_menu.pressed = HitTarget::kNone;
        finish_opacity_drag();
        g_menu.dragging = false;
        return 0;
    case WM_MOUSEWHEEL:
    {
        g_menu.hints.hide();
        // Wheel always scrolls the page; only an explicit press/drag adjusts a value.
        g_menu.pressed = HitTarget::kNone;
        if (!g_menu.dragging_opacity && GetCapture() == hwnd) ReleaseCapture();
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd, &point);
        if (!g_menu.dragging_opacity && PtInRect(&g_menu.layout.viewport, point)) {
            g_menu.wheel_remainder += GET_WHEEL_DELTA_WPARAM(wparam);
            const int ticks = g_menu.wheel_remainder / WHEEL_DELTA;
            g_menu.wheel_remainder %= WHEEL_DELTA;
            g_menu.viewport.move(-ticks * S(36));
            g_menu.hover = static_cast<int>(hit_test(point));
            render();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_menu.hints.hide();
        if (g_menu.hover != static_cast<int>(HitTarget::kNone))
        {
            g_menu.hover = static_cast<int>(HitTarget::kNone);
            render();
        }
        return 0;
    case WM_MOUSEHOVER:
        g_menu.hints.hover(POINT{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)});
        return 0;
    case WM_TIMER:
        if (wparam == 1 && g_menu.anim_frames > 0)
        {
            --g_menu.anim_frames;
            const int total = 8;
            const int step = total - g_menu.anim_frames;
            const int x = g_menu.anim_target.left;
            const int y = g_menu.anim_target.top + S(14) * (total - step) / total;
            g_menu.present_alpha = 70 + (255 - 70) * step / total;
            SetWindowPos(hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            render();
            if (g_menu.anim_frames == 0)
            {
                KillTimer(hwnd, 1);
                g_menu.present_alpha = 255;
                SetWindowPos(hwnd, HWND_TOPMOST, g_menu.anim_target.left, g_menu.anim_target.top, 0, 0,
                             SWP_NOSIZE | SWP_NOACTIVATE);
                render();
            }
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wparam == VK_ESCAPE)
        {
            if (g_menu.config_bit != 0) open_page(0);
            else hide_menu();
        }
        else if (wparam == VK_PRIOR || wparam == VK_NEXT || wparam == VK_HOME || wparam == VK_END)
        {
            if (wparam == VK_HOME) g_menu.viewport.scroll = 0;
            else if (wparam == VK_END) g_menu.viewport.scroll = g_menu.viewport.maximum_scroll();
            else g_menu.viewport.move((wparam == VK_PRIOR ? -1 : 1) * std::max(1, g_menu.viewport.height() - S(30)));
            g_menu.hover = static_cast<int>(HitTarget::kNone);
            render();
        }
        return 0;
    case WM_CLOSE:
        hide_menu();
        return 0;
    case WM_DESTROY:
        g_menu.hints.destroy();
        destroy_fonts();
        layered_bitmap_destroy(g_menu.surface);
        g_menu.hwnd = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

bool menu_create(HINSTANCE instance, MenuCallbacks callbacks)
{
    g_menu.callbacks = std::move(callbacks);
    if (!g_menu.class_registered)
    {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = menu_proc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.lpszClassName = constants::kMenuClass;
        if (RegisterClassExW(&window_class) == 0)
        {
            log_error("RegisterClassExW(menu) failed, code=%d", static_cast<int>(GetLastError()));
            return false;
        }
        g_menu.class_registered = true;
    }

    // 先建一个小尺寸隐藏窗口，真正尺寸在 menu_show 里按 DPI/缩放计算。
    g_menu.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, constants::kMenuClass,
                                  constants::kAppTitle, WS_POPUP, 0, 0, 32, 32, nullptr, nullptr, instance,
                                  nullptr);
    if (g_menu.hwnd == nullptr)
    {
        log_error("CreateWindowExW(menu) failed, code=%d", static_cast<int>(GetLastError()));
        return false;
    }
    return true;
}

void menu_destroy()
{
    if (g_menu.hwnd != nullptr)
    {
        DestroyWindow(g_menu.hwnd);
        g_menu.hwnd = nullptr;
    }
    destroy_fonts();
    release_menu_logo();
    layered_bitmap_destroy(g_menu.surface);
    g_menu.visible = false;
}

void menu_show(const Config& config, const RECT& anchor)
{
    if (g_menu.hwnd == nullptr)
    {
        return;
    }

    g_menu.dpi = dpi_for_system();
    sync_page_config(config);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &g_menu.work, 0);
    HMONITOR monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &info)) g_menu.work = info.rcWork;
    if (g_menu.logo == nullptr || g_menu.logo_size != S(22))
    {
        load_menu_logo();
    }
    if (!ensure_surface())
    {
        return;
    }

    const int width = g_menu.surface.width;
    const int height = g_menu.surface.height;
    const int gap = S(6);

    const RECT& work = g_menu.work;

    // 球点击唤起时，菜单出现在球的原位置（右边缘对齐球、顶对齐球）；托盘唤起时退化为贴球。
    const int anchor_width = anchor.right - anchor.left;
    int x = 0;
    int y = 0;
    if (anchor_width >= S(32))
    {
        x = anchor.right - width;
        y = anchor.top;
    }
    else
    {
        x = anchor.left - width - gap;
        if (x < work.left + gap)
        {
            x = anchor.right + gap;
        }
        y = (anchor.top + anchor.bottom) / 2 - height / 2;
    }

    const int margin = S(8);
    const int min_x = work.left + margin;
    const int max_x = std::max<int>(min_x, work.right - width - margin);
    const int min_y = work.top + margin;
    const int max_y = std::max<int>(min_y, work.bottom - height - margin);
    x = x < min_x ? min_x : (x > max_x ? max_x : x);
    y = y < min_y ? min_y : (y > max_y ? max_y : y);

    g_menu.anim_target = RECT{x, y, x + width, y + height};
    const int start_x = x;
    const int start_y = std::min(max_y, y + S(14));
    SetWindowPos(g_menu.hwnd, HWND_TOPMOST, start_x, start_y, width, height, SWP_NOACTIVATE);
    g_menu.anim_frames = 8;
    g_menu.present_alpha = 70;
    render();
    SetTimer(g_menu.hwnd, 1, 15, nullptr);
    ShowWindow(g_menu.hwnd, SW_SHOW);
    SetForegroundWindow(g_menu.hwnd);
    SetFocus(g_menu.hwnd);
    g_menu.visible = true;
    g_menu.hover = static_cast<int>(HitTarget::kNone);
    log_info("menu shown at (%d,%d) size=%dx%d scale=%d", x, y, width, height, g_menu.ui_scale);
}

void menu_follow(int dx, int dy)
{
    if (g_menu.hwnd == nullptr || !g_menu.visible || g_menu.anim_frames > 0 || (dx == 0 && dy == 0))
    {
        return;
    }
    GetWindowRect(g_menu.hwnd, &g_menu.anim_target);
    OffsetRect(&g_menu.anim_target, dx, dy);
    SetWindowPos(g_menu.hwnd, HWND_TOPMOST, g_menu.anim_target.left, g_menu.anim_target.top, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);
}

void menu_hide()
{
    if (g_menu.visible)
    {
        hide_menu();
    }
}

bool menu_is_visible()
{
    return g_menu.visible;
}

void menu_open_config(std::uint32_t bit)
{
    if (g_menu.hwnd == nullptr)
    {
        return;
    }
    open_page(bit);
}

void menu_open_settings()
{
    if (g_menu.hwnd != nullptr) open_page(kSettingsPage);
}

void menu_sync_config(const Config& config)
{
    sync_page_config(config);
    if (g_menu.logo == nullptr || g_menu.logo_size != S(22)) load_menu_logo();
    if (g_menu.config_bit == ipc::kFeatureDamageNumbers && g_menu.callbacks.get_damage_config)
        g_menu.damage = g_menu.callbacks.get_damage_config();
    if (g_menu.visible) render();
}

void menu_refresh()
{
    if (g_menu.visible)
    {
        render();
    }
}

}  // namespace arkhelp
