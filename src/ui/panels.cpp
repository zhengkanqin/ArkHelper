#include "panels.h"

#include <algorithm>
#include <cstdio>
#include <utility>
#include <cwchar>

#include "../log/log.h"
#include "../shell/theme.h"
#include "../shell/win_util.h"
#include "shared_mouse_input.h"

namespace arkhelp::panels
{
namespace
{

constexpr wchar_t kDamageClass[] = L"ArkHelp.Panel.Damage";
constexpr wchar_t kSpawnClass[] = L"ArkHelp.Panel.Spawn";
constexpr wchar_t kWikiClass[] = L"ArkHelp.Panel.Wiki";
constexpr wchar_t kPrtsClass[] = L"ArkHelper.Panel.Prts";

constexpr int kPanelWidthLogical = 260;
constexpr int kTitleHeightLogical = 32;
constexpr int kRowHeightLogical = 30;
constexpr int kPaddingLogical = 14;
constexpr int kShadowMarginLogical = 10;
constexpr int kCornerRadiusLogical = 12;
constexpr int kToggleWidthLogical = 30;
constexpr int kNoPosition = -100000;

struct PanelState
{
    const wchar_t* id = nullptr;
    const wchar_t* class_name = nullptr;
    HWND hwnd = nullptr;
    bool class_registered = false;

    bool visible = false;
    bool collapsed = false;
    bool dragging = false;
    bool resizing = false;
    bool resizing_width = false;
    bool shared_dispatch = false;
    POINT shared_cursor{};
    POINT resize_cursor_start{0, 0};
    POINT width_resize_cursor_start{0, 0};
    int resize_start_height = 0;
    int resize_start_width = 0;
    int height_override_px = 0;  // >0 时使用人工高度（总内容高度，含标题栏）
    int width_override_px = 0;   // >0 时使用人工宽度（像素）
    int auto_width_logical = 0;  // >0 且无人工宽度时，按内容自动调整的宽度（逻辑像素）
    int scroll_px = 0;           // 内容滚动偏移
    int max_scroll = 0;
    int icon_scroll_px = 0, max_icon_scroll = 0;
    RECT icon_viewport{};
    int horizontal_px = 0;
    int max_horizontal = 0;
    int horizontal_start_px = 0;
    int horizontal_start_cursor = 0;
    bool scrolling_horizontal = false;
    RECT horizontal_track{};
    RECT horizontal_thumb{};
    std::vector<int> statistics_columns;
    std::vector<int> row_heights;
    std::vector<int> table_columns;
    bool show_scrollbar = false;
    RECT resize_grip{};
    RECT width_grip{};
    POINT drag_cursor_start{0, 0};
    RECT drag_window_start{0, 0, 0, 0};

    int x = kNoPosition;
    int y = kNoPosition;
    int dpi = 96;
    int ui_scale = 100;
    FeatureAppearance appearance;
    bool compact = true;
    int measured_row_height = 0;
    int measured_title_height = 0;

    LayeredBitmap surface;
    HFONT title_font = nullptr;
    HFONT row_font = nullptr;

    std::wstring title;
    std::vector<Row> rows;
    std::vector<HBITMAP> avatars;
    int avatar_logical_size = 18;
    unsigned long long avatar_key = 0;
    std::vector<RECT> row_rects;
    std::vector<bool> row_clickable;

    RECT content{};
    RECT title_bar{};
    RECT toggle{};
    RECT settings_button{};
    bool settings_pressed=false;
    POINT settings_press_point{};
    OverflowHints hints;
    bool closeable = false;
    RECT close_button{};
    RECT split_button{};  // 磁吸分组时出现在折叠箭头左侧的「拆分」按钮
};

PanelState* g_panels[4] = {nullptr, nullptr, nullptr, nullptr};
Callbacks g_callbacks;
HINSTANCE g_instance = nullptr;
std::vector<std::pair<PanelState*, POINT>> g_drag_starts;
constexpr UINT kSharedMouseMessage = WM_APP + 0x71;
constexpr UINT_PTR kSharedMouseTimer = 1;
SharedMouseInput g_shared_mouse;
HWND g_shared_window = nullptr;
PanelState* g_shared_gesture = nullptr;
PanelState* g_shared_hover = nullptr;
HWND g_shared_source = nullptr;
POINT g_shared_hover_point{};
ULONGLONG g_shared_hover_since = 0;
void refresh_shared_input();

class SharedCursor {
    HWND hwnd_ = nullptr;
    LayeredBitmap surface_, white_;
    HCURSOR cursor_ = nullptr;
    POINT hotspot_{};
public:
    HWND window() const { return hwnd_; }
    void hide() { if (hwnd_) ShowWindow(hwnd_, SW_HIDE); }
    void destroy() {
        if (hwnd_) DestroyWindow(hwnd_);
        hwnd_ = nullptr; cursor_ = nullptr;
        layered_bitmap_destroy(surface_); layered_bitmap_destroy(white_);
    }
    bool show(POINT point, HCURSOR cursor) {
        if (!cursor) { hide(); return false; }
        if (!hwnd_) {
            WNDCLASSW wc{};
            wc.hInstance = g_instance; wc.lpfnWndProc = DefWindowProcW;
            wc.lpszClassName = L"ArkHelper.SharedCursor";
            if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
            hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_instance, nullptr);
            if (!hwnd_) return false;
        }
        if (cursor_ != cursor) {
            ICONINFO info{};
            if (!GetIconInfo(cursor, &info)) { hide(); return false; }
            BITMAP dimensions{};
            GetObjectW(info.hbmColor ? info.hbmColor : info.hbmMask, sizeof(dimensions), &dimensions);
            const int width = dimensions.bmWidth;
            const int height = info.hbmColor ? dimensions.bmHeight : dimensions.bmHeight / 2;
            hotspot_ = {static_cast<LONG>(info.xHotspot), static_cast<LONG>(info.yHotspot)};
            if (info.hbmColor) DeleteObject(info.hbmColor);
            if (info.hbmMask) DeleteObject(info.hbmMask);
            if (width <= 0 || height <= 0 || !layered_bitmap_create(surface_, width, height) ||
                !layered_bitmap_create(white_, width, height)) { hide(); return false; }
            // Two backgrounds recover alpha for both ARGB and monochrome system cursors.
            fill_rect(surface_.dc, RECT{0, 0, width, height}, RGB(0, 0, 0));
            fill_rect(white_.dc, RECT{0, 0, width, height}, RGB(255, 255, 255));
            DrawIconEx(surface_.dc, 0, 0, cursor, width, height, 0, nullptr, DI_NORMAL);
            DrawIconEx(white_.dc, 0, 0, cursor, width, height, 0, nullptr, DI_NORMAL);
            GdiFlush();
            auto* black = static_cast<std::uint32_t*>(surface_.bits);
            const auto* white = static_cast<const std::uint32_t*>(white_.bits);
            for (int i = 0; i < width * height; ++i) {
                const int difference = std::clamp(static_cast<int>(white[i] & 255) - static_cast<int>(black[i] & 255), 0, 255);
                const auto alpha = static_cast<std::uint32_t>(255 - difference);
                black[i] = (alpha << 24) | (black[i] & 0xFFFFFF);
            }
            SetWindowPos(hwnd_, HWND_TOPMOST, point.x-hotspot_.x, point.y-hotspot_.y,
                width, height, SWP_NOACTIVATE);
            if (!layered_bitmap_present(hwnd_, surface_, 255)) { hide(); return false; }
            cursor_ = cursor;
        }
        SetWindowPos(hwnd_, HWND_TOPMOST, point.x-hotspot_.x, point.y-hotspot_.y,
            0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return true;
    }
};
SharedCursor g_shared_cursor;

void cancel_interaction(PanelState& panel)
{
    if (panel.dragging) g_drag_starts.clear();
    panel.dragging = panel.resizing = panel.resizing_width = false;
    panel.settings_pressed = panel.scrolling_horizontal = false;
    if (GetCapture() == panel.hwnd) ReleaseCapture();
}

void cancel_shared_input(PanelState* panel = nullptr)
{
    g_shared_cursor.hide();
    if (g_shared_gesture && (!panel || g_shared_gesture == panel)) {
        cancel_interaction(*g_shared_gesture);
        g_shared_gesture = nullptr;
        g_shared_source = nullptr;
    }
    if (g_shared_hover && (!panel || g_shared_hover == panel)) {
        g_shared_hover->hints.hide();
        g_shared_hover = nullptr;
    }
}

void mouse_cursor(const PanelState& panel, POINT& point)
{
    if (panel.shared_dispatch) point = panel.shared_cursor;
    else GetCursorPos(&point);
}

void capture_mouse(const PanelState& panel)
{
    if (!panel.shared_dispatch) SetCapture(panel.hwnd);
}

void release_mouse(const PanelState& panel)
{
    if (GetCapture() == panel.hwnd) ReleaseCapture();
}

// 磁吸分组：同组面板上下紧贴、一起拖动、宽度统一到组内最大值。
struct DockGroup
{
    std::vector<PanelState*> members;  // 自上而下
};

std::vector<DockGroup> g_dock_groups;

DockGroup* find_group(const PanelState* panel)
{
    for (DockGroup& group : g_dock_groups)
    {
        for (PanelState* member : group.members)
        {
            if (member == panel)
            {
                return &group;
            }
        }
    }
    return nullptr;
}

bool has_dock_partner(const PanelState& panel)
{
    DockGroup* group = find_group(&panel);
    if (group == nullptr)
    {
        return false;
    }
    int visible = 0;
    for (const PanelState* member : group->members)
    {
        if (member->visible)
        {
            ++visible;
        }
    }
    return visible >= 2;
}

int S(const PanelState& panel, int logical)
{
    return scale_ui(logical, panel.dpi, panel.ui_scale);
}

int row_height(const PanelState& panel)
{
    const bool compact = panel.id == kDamagePanelId && panel.compact;
    return std::max(S(panel, compact ? 24 : kRowHeightLogical), panel.measured_row_height + S(panel, compact ? 4 : 8));
}

int title_height(const PanelState& panel)
{
    return std::max(S(panel, kTitleHeightLogical), panel.measured_title_height + S(panel, 10));
}

int column_width(const PanelState& panel, int logical)
{
    const int font_size = panel.appearance.font_size > 0 ? panel.appearance.font_size : 12;
    return S(panel, logical * std::max(12, font_size) / 12);
}

std::vector<int> measure_statistics(const PanelState& panel, const std::vector<Row>& rows)
{
    std::size_t count = 0;
    for (const auto& row : rows) if (row.cells.size() > 1) count = std::max(count, row.cells.size());
    std::vector<int> widths(count, S(panel, panel.compact ? 28 : 36));
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return widths;
    const auto old = SelectObject(dc, panel.row_font);
    for (const auto& row : rows) {
        if (row.cells.size() < 2) continue;
        for (std::size_t i = 0; i < row.cells.size(); ++i) {
            SIZE size{};
            GetTextExtentPoint32W(dc, row.cells[i].c_str(), static_cast<int>(row.cells[i].size()), &size);
            widths[i] = std::max(widths[i], static_cast<int>(size.cx) + S(panel, panel.compact ? 8 : 12));
        }
    }
    SelectObject(dc, old); DeleteDC(dc);
    if (!widths.empty()) widths[0] = std::clamp(widths[0], S(panel, 60), column_width(panel, 160));
    return widths;
}

BYTE panel_alpha(const PanelState& panel)
{
    return static_cast<BYTE>(std::clamp(panel.appearance.opacity, 20, 100) * 255 / 100);
}

COLORREF panel_text_color(const PanelState& panel, COLORREF fallback)
{
    const auto color = panel.appearance.text_color;
    return color < 0 ? fallback : RGB((color >> 16) & 255, (color >> 8) & 255, color & 255);
}

void destroy_fonts(PanelState& panel)
{
    if (panel.title_font != nullptr)
    {
        DeleteObject(panel.title_font);
        panel.title_font = nullptr;
    }
    if (panel.row_font != nullptr)
    {
        DeleteObject(panel.row_font);
        panel.row_font = nullptr;
    }
}

void create_fonts(PanelState& panel)
{
    destroy_fonts(panel);
    const int size = panel.appearance.font_size;
    panel.title_font = create_ui_font(size > 0 ? size : 13, FW_SEMIBOLD, panel.dpi, panel.ui_scale);
    panel.row_font = create_ui_font(size > 0 ? size : 12, FW_NORMAL, panel.dpi, panel.ui_scale);
    HDC dc = CreateCompatibleDC(nullptr);
    if (dc)
    {
        const auto old = SelectObject(dc, panel.row_font);
        TEXTMETRICW metric{};
        if (GetTextMetricsW(dc, &metric)) panel.measured_row_height = metric.tmHeight;
        SelectObject(dc, panel.title_font);
        if (GetTextMetricsW(dc, &metric)) panel.measured_title_height = metric.tmHeight;
        SelectObject(dc, old);
        DeleteDC(dc);
    }
}

int panel_slot(const PanelState& panel)
{
    if (panel.id == kPrtsPanelId) return 3;
    if (panel.id == kWikiPanelId)
    {
        return 2;
    }
    if (panel.id == kSpawnPanelId)
    {
        return 1;
    }
    return 0;
}

void default_position(PanelState& panel, int slot)
{
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int width = S(panel, kPanelWidthLogical) + 2 * S(panel, kShadowMarginLogical);
    const int right = work.right - width - S(panel, 18) - (slot == 3 ? width + S(panel, 18) : 0);
    const int top = work.top + S(panel, 88) + (slot == 3 ? 0 : slot) * (S(panel, 220));
    panel.x = right > work.left ? right : work.left + S(panel, 12);
    panel.y = top;
}

bool rows_equal(const std::vector<Row>& left, const std::vector<Row>& right)
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        if (left[index].left != right[index].left || left[index].right != right[index].right ||
            left[index].accent != right[index].accent || left[index].selected != right[index].selected ||
            left[index].icon_only != right[index].icon_only || left[index].header != right[index].header ||
            left[index].table != right[index].table || left[index].wrap != right[index].wrap ||
            left[index].cells != right[index].cells)
        {
            return false;
        }
    }
    return true;
}

void layout(PanelState& panel)
{
    const int margin = S(panel, kShadowMarginLogical);
    const int width = panel.width_override_px > 0
                          ? panel.width_override_px
                          : S(panel, panel.auto_width_logical > 0 ? panel.auto_width_logical
                                                                  : kPanelWidthLogical);
    const int title_height = panels::title_height(panel);
    const int row_height = panels::row_height(panel);
    const bool statistics = std::wcscmp(panel.id, kDamagePanelId) == 0;
    panel.statistics_columns = statistics ? measure_statistics(panel, panel.rows) : std::vector<int>{};
    int numeric_total = 0;
    for (std::size_t i = 1; i < panel.statistics_columns.size(); ++i) numeric_total += panel.statistics_columns[i];
    const int available = width - S(panel, 12);
    const int name_width = panel.statistics_columns.empty() ? 0 :
        std::min(panel.statistics_columns[0], std::max(S(panel, 60), available - numeric_total));
    if (!panel.statistics_columns.empty()) panel.statistics_columns[0] = name_width;
    const int numeric_width = std::max(1, width - S(panel, 12) - name_width);
    panel.max_horizontal = std::max(0, numeric_total - numeric_width);
    // Only distribute spare space after every column has been measured tightly.
    if (!panel.compact && !panel.max_horizontal && panel.statistics_columns.size() > 1) {
        int spare = std::max(0, numeric_width - numeric_total);
        for (std::size_t i = 1; i < panel.statistics_columns.size(); ++i) {
            const int extra = spare / static_cast<int>(panel.statistics_columns.size() - i);
            panel.statistics_columns[i] += extra; spare -= extra;
        }
    }
    panel.horizontal_px = std::clamp(panel.horizontal_px, 0, panel.max_horizontal);
    const int horizontal_height = !panel.collapsed && panel.max_horizontal > 0 ? S(panel, 18) : 0;
    const int bottom = S(panel, 10) + horizontal_height;
    // 行内容从标题栏下方 S(6) 处开始，排版高度必须包含这段间隙，否则最后一行会被裁掉。
    int icon_rows = 0;
    for (const Row& row : panel.rows)
    {
        if (row.icon_only)
        {
            ++icon_rows;
        }
    }
    // Keep semantic rows intact: hit testing/actions retain their original indices while text
    // and the reference grid reflow whenever width, DPI or font size changes.
    const int icon_width = icon_rows ? S(panel, (panel.avatars.empty() ? 0 : panel.avatar_logical_size) + 6) : 0;
    const int detail_width = std::max(1, width - S(panel, 12) - icon_width);
    panel.row_heights.assign(panel.rows.size(), row_height);
    panel.table_columns.assign(panel.rows.size(), 1);
    HDC measure = GetDC(nullptr);
    const auto old_font = SelectObject(measure, panel.row_font);
    int detail_height = 0;
    for (std::size_t i = 0; i < panel.rows.size(); ++i) {
        const auto& row = panel.rows[i];
        if (row.icon_only) continue;
        if (row.wrap) {
            RECT bounds{0, 0, detail_width - S(panel, 8), 0};
            DrawTextW(measure, row.left.c_str(), -1, &bounds, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            panel.row_heights[i] = std::max(row_height, static_cast<int>(bounds.bottom) + S(panel, 10));
        } else if (row.table && row.cells.size() >= 2) {
            int group_width = S(panel, 110);
            for (std::size_t j = 0; j + 1 < row.cells.size(); j += 2) {
                SIZE key{}, value{};
                GetTextExtentPoint32W(measure, row.cells[j].c_str(), static_cast<int>(row.cells[j].size()), &key);
                GetTextExtentPoint32W(measure, row.cells[j + 1].c_str(), static_cast<int>(row.cells[j + 1].size()), &value);
                group_width = std::max(group_width, static_cast<int>(key.cx + value.cx) + S(panel, 24));
            }
            const int groups = static_cast<int>(row.cells.size() / 2);
            const int columns = std::clamp((detail_width + S(panel, 14)) / (group_width + S(panel, 14)), 1, std::min(3, groups));
            panel.table_columns[i] = columns;
            panel.row_heights[i] = ((groups + columns - 1) / columns) * row_height;
        }
        detail_height += panel.row_heights[i];
    }
    SelectObject(measure, old_font); ReleaseDC(nullptr, measure);
    const int stacked_height = std::max(icon_rows * row_height, detail_height);
    const int auto_body =
        panel.collapsed ? 0 : S(panel, 6) + stacked_height + bottom;
    int body_height = auto_body;
    if (!panel.collapsed && panel.height_override_px <= 0)
    {
        // 默认最大高度：内容少时按内容收缩，内容多时停在此高度并提供滚动。
        const int cap = S(panel, 560);
        if (body_height > cap - title_height)
        {
            body_height = cap - title_height;
        }
    }
    if (!panel.collapsed && panel.height_override_px > 0)
    {
        const int minimum_rows = statistics ? 3 : 1;
        const int minimum = title_height + S(panel, 6) + minimum_rows * row_height + bottom;
        body_height = panel.height_override_px - title_height;
        if (body_height < minimum - title_height)
        {
            body_height = minimum - title_height;
        }
        (void)minimum;
    }
    const int content_height = title_height + body_height;
    panel.content = RECT{margin, margin, margin + width, margin + content_height};
    panel.resize_grip =
        RECT{panel.content.left + S(panel, kCornerRadiusLogical), panel.content.bottom - S(panel, 9),
             panel.content.right - S(panel, kCornerRadiusLogical), panel.content.bottom};
    panel.width_grip = RECT{panel.content.right - S(panel, 6), panel.content.top, panel.content.right,
                            panel.content.bottom};
    const int view_height = body_height - S(panel, 6) - bottom;
    const int rows_height = detail_height;
    panel.max_icon_scroll = std::max(0, icon_rows * row_height - view_height);
    panel.icon_scroll_px = std::clamp(panel.icon_scroll_px, 0, panel.max_icon_scroll);
    panel.icon_viewport = icon_rows ? RECT{panel.content.left + S(panel, 6),
        panel.content.top + title_height + S(panel, 6), panel.content.left + S(panel, 6) + icon_width,
        panel.content.bottom - bottom} : RECT{};
    panel.max_scroll = rows_height > view_height ? rows_height - view_height : 0;
    if (panel.scroll_px > panel.max_scroll)
    {
        panel.scroll_px = panel.max_scroll;
    }
    if (panel.max_scroll == 0)
    {
        panel.scroll_px = 0;
    }
    panel.title_bar = RECT{margin, margin, margin + width, margin + title_height};
    const int controls_right = panel.title_bar.right - S(panel, 10); // reserve a separate width drag edge, including collapsed titles
    panel.close_button = panel.closeable ? RECT{controls_right - S(panel, kToggleWidthLogical),
        panel.title_bar.top, controls_right, panel.title_bar.bottom} : RECT{};
    const int toggles_right = panel.closeable ? panel.close_button.left : controls_right;
    panel.toggle = RECT{toggles_right - S(panel, kToggleWidthLogical), panel.title_bar.top,
                        toggles_right, panel.title_bar.bottom};
    panel.settings_button=RECT{panel.toggle.left-S(panel,kToggleWidthLogical),panel.toggle.top,panel.toggle.left,panel.toggle.bottom};
    panel.split_button = RECT{panel.settings_button.left - S(panel, kToggleWidthLogical), panel.toggle.top,
                              panel.settings_button.left, panel.toggle.bottom};
    panel.horizontal_track = {};
    panel.horizontal_thumb = {};
    if (horizontal_height > 0) {
        panel.horizontal_track = RECT{panel.content.left + S(panel, 6) + name_width,
            panel.content.bottom - bottom + S(panel, 4), panel.content.right - S(panel, 8),
            panel.content.bottom - S(panel, 12)};
        const int track_width = panel.horizontal_track.right - panel.horizontal_track.left;
        const int thumb_width = std::max(S(panel, 24), track_width * numeric_width /
                                         (numeric_width + panel.max_horizontal));
        const int left = panel.horizontal_track.left +
            (track_width - thumb_width) * panel.horizontal_px / panel.max_horizontal;
        panel.horizontal_thumb = RECT{left, panel.horizontal_track.top, left + thumb_width,
                                      panel.horizontal_track.bottom};
    }
}

bool ensure_surface(PanelState& panel)
{
    if (!panel.title_font || !panel.row_font) create_fonts(panel);
    layout(panel);
    const int margin = S(panel, kShadowMarginLogical);
    const int width = (panel.content.right - panel.content.left) + 2 * margin;
    const int height = (panel.content.bottom - panel.content.top) + 2 * margin;
    if (panel.surface.bits != nullptr && panel.surface.width == width && panel.surface.height == height &&
        panel.title_font != nullptr)
    {
        return true;
    }
    if (!layered_bitmap_create(panel.surface, width, height))
    {
        log_error("panel surface creation failed, width=%d height=%d", width, height);
        return false;
    }
    return true;
}

namespace
{

void blit_bitmap(HDC dc, HBITMAP bitmap, int x, int y, int width, int height)
{
    if (bitmap == nullptr || width <= 0 || height <= 0)
    {
        return;
    }
    BITMAP info{};
    if (GetObjectW(bitmap, sizeof(info), &info) == 0 || info.bmWidth <= 0 || info.bmHeight <= 0)
    {
        return;
    }
    HDC memory = CreateCompatibleDC(dc);
    if (memory == nullptr)
    {
        return;
    }
    HGDIOBJ previous = SelectObject(memory, bitmap);
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(dc, x, y, width, height, memory, 0, 0, info.bmWidth, info.bmHeight, blend);
    SelectObject(memory, previous);
    DeleteDC(memory);
}

}  // namespace

void render(PanelState& panel)
{
    if (panel.hwnd == nullptr || !ensure_surface(panel))
    {
        return;
    }
    layered_bitmap_clear(panel.surface);
    HDC dc = panel.surface.dc;
    panel.hints.begin(panel.hwnd,dc);
    const int margin = S(panel, kShadowMarginLogical);
    const int radius = S(panel, kCornerRadiusLogical);
    const int title_height = panel.title_bar.bottom - panel.title_bar.top;

    fill_rect(dc, panel.content, theme::kBackground);
    fill_rect(dc, panel.title_bar, theme::kBackgroundAlt);
    if(panel.collapsed) {
        const int mid=(panel.title_bar.top+panel.title_bar.bottom)/2;
        fill_round_rect(dc,RECT{panel.content.right-S(panel,5),mid-S(panel,5),
            panel.content.right-S(panel,3),mid+S(panel,5)},S(panel,1),theme::kTextMuted);
    }
    fill_rect(dc, RECT{panel.content.left, panel.title_bar.bottom, panel.content.right,
                       panel.title_bar.bottom + 1},
              theme::kDivider);

    const int padding = S(panel, kPaddingLogical);
    const bool grouped = has_dock_partner(panel);
    const int title_right = grouped ? panel.split_button.left - S(panel, 4) : panel.settings_button.left - S(panel, 4);
    RECT settings_icon = panel.settings_button;
    const int settings_cx = (settings_icon.left + settings_icon.right) / 2;
    const int settings_cy = (settings_icon.top + settings_icon.bottom) / 2;
    settings_icon = RECT{settings_cx - S(panel, 9), settings_cy - S(panel, 9),
                         settings_cx + S(panel, 9), settings_cy + S(panel, 9)};
    draw_settings_icon(dc,settings_icon,theme::kTextSecondary);
    draw_text(dc, panel.title.c_str(),
              RECT{panel.content.left + padding, panel.title_bar.top, title_right,
                   panel.title_bar.bottom},
              panel.title_font, panel_text_color(panel, theme::kTextPrimary),
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    {
        // 折叠箭头：画实心三角形，避免字体缺字时退化成一个点。
        const int cx = (panel.toggle.left + panel.toggle.right) / 2;
        const int cy = (panel.toggle.top + panel.toggle.bottom) / 2;
        const int half = S(panel, 5);
        POINT points[3];
        if (panel.collapsed)
        {
            points[0] = POINT{cx - half / 2, cy - half};
            points[1] = POINT{cx - half / 2, cy + half};
            points[2] = POINT{cx + half, cy};
        }
        else
        {
            points[0] = POINT{cx - half, cy - half / 2};
            points[1] = POINT{cx + half, cy - half / 2};
            points[2] = POINT{cx, cy + half};
        }
        HBRUSH arrow_brush = CreateSolidBrush(theme::kTextSecondary);
        HGDIOBJ arrow_old_brush = SelectObject(dc, arrow_brush);
        HGDIOBJ arrow_old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, points, 3);
        SelectObject(dc, arrow_old_pen);
        SelectObject(dc, arrow_old_brush);
        DeleteObject(arrow_brush);
    }
    if (grouped)
    {
        // 拆分图标：两块并排的「面板」中间留缝，表示把当前面板从磁吸组里拆出去。
        const int cx = (panel.split_button.left + panel.split_button.right) / 2;
        const int cy = (panel.split_button.top + panel.split_button.bottom) / 2;
        const int half_w = S(panel, 6);
        const int half_h = S(panel, 10);
        const int gap = S(panel, 5);
        const RECT left{cx - gap / 2 - half_w, cy - half_h / 2, cx - gap / 2, cy + half_h / 2};
        const RECT right{cx + gap / 2, cy - half_h / 2, cx + gap / 2 + half_w, cy + half_h / 2};
        fill_round_rect(dc, left, S(panel, 2), theme::kTextSecondary);
        fill_round_rect(dc, right, S(panel, 2), theme::kTextSecondary);
    }
    if (panel.closeable) {
        const int cx = (panel.close_button.left + panel.close_button.right) / 2;
        const int cy = (panel.close_button.top + panel.close_button.bottom) / 2;
        const int r = S(panel, 4);
        HPEN pen = CreatePen(PS_SOLID, S(panel, 2), theme::kTextSecondary);
        const auto old = SelectObject(dc, pen);
        MoveToEx(dc, cx-r, cy-r, nullptr); LineTo(dc, cx+r+1, cy+r+1);
        MoveToEx(dc, cx+r, cy-r, nullptr); LineTo(dc, cx-r-1, cy+r+1);
        SelectObject(dc, old); DeleteObject(pen);
    }

    if (!panel.collapsed && !panel.rows.empty())
    {
        const int row_height = panels::row_height(panel);
        const int clip_top = panel.title_bar.bottom + S(panel, 6);
        const int clip_bottom = panel.max_horizontal > 0 ? panel.horizontal_track.top - S(panel, 4) :
            panel.content.bottom - S(panel, 10);
        const bool statistics = std::wcscmp(panel.id, kDamagePanelId) == 0;
        const int pinned_rows = statistics && panel.rows.size() >= 2 && panel.rows[0].header &&
            panel.rows[1].header ? 2 : 0;
        // 滚动时行可能跨出内容区上沿，必须裁剪，避免盖住标题栏。
        const int saved_dc = SaveDC(dc);
        IntersectClipRect(dc, panel.content.left, clip_top, panel.content.right, clip_bottom);
        int left_top = clip_top - panel.icon_scroll_px;
        int right_top = clip_top - panel.scroll_px;
        panel.row_rects.assign(panel.rows.size(), RECT{});
        const bool has_avatars = !panel.avatars.empty();
        const int avatar_size = has_avatars ? S(panel, panel.avatar_logical_size) : 0;
        int icon_column = 0;
        for (const Row& row : panel.rows)
        {
            if (row.icon_only)
            {
                icon_column = avatar_size + S(panel, 6);
                break;
            }
        }
        std::size_t avatar_index = 0;
        for (std::size_t row_index = 0; row_index < panel.rows.size(); ++row_index)
        {
            const Row& row = panel.rows[row_index];
            if (row.icon_only)
            {
                if (left_top >= clip_bottom || left_top + row_height <= clip_top) {
                    left_top += row_height; ++avatar_index; continue;
                }
                const RECT row_rect{panel.content.left + S(panel, 6), left_top,
                                    panel.content.left + S(panel, 6) + icon_column, left_top + row_height};
                if (row.selected)
                {
                    fill_round_rect(dc, row_rect, S(panel, 4), theme::kSelection);
                }
                panel.row_rects[row_index] = row_rect;
                panel.row_rects[row_index].top = std::max<LONG>(row_rect.top, clip_top);
                panel.row_rects[row_index].bottom = std::min<LONG>(row_rect.bottom, clip_bottom);
                panel.hints.add(panel.row_rects[row_index], row.left);
                const int icon_x = row_rect.left + (icon_column - avatar_size) / 2;
                const int icon_y = left_top + (row_height - avatar_size) / 2;
                if (has_avatars && avatar_index < panel.avatars.size() && panel.avatars[avatar_index])
                {
                    blit_bitmap(dc, panel.avatars[avatar_index], icon_x, icon_y, avatar_size,
                                avatar_size);
                }
                else
                {
                    // 头像没取到（PRTS 无缩略图/网络失败）时用占位圆+首字，避免出现空白方块。
                    const RECT placeholder{icon_x, icon_y, icon_x + avatar_size, icon_y + avatar_size};
                    fill_round_rect(dc, placeholder, avatar_size / 2, RGB(46, 72, 112));
                    if (!row.left.empty() && panel.row_font != nullptr)
                    {
                        wchar_t initial[2] = {row.left[0], L'\0'};
                        draw_text(dc, initial, placeholder, panel.row_font, panel_text_color(panel, theme::kTextSecondary),
                                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
                    }
                }
                ++avatar_index;
                left_top += row_height;
                continue;
            }
            if (pinned_rows > 0)
                right_top = clip_top + static_cast<int>(row_index) * row_height -
                    (row_index < static_cast<std::size_t>(pinned_rows) ? 0 : panel.scroll_px);
            const int visible_top = clip_top + (row_index < static_cast<std::size_t>(pinned_rows) ? 0 : pinned_rows * row_height);
            const int detail_row_height = panel.row_heights[row_index];
            if (right_top >= clip_bottom)
            {
                right_top += detail_row_height;
                continue;
            }
            if (right_top + detail_row_height <= visible_top)
            {
                right_top += detail_row_height;
                continue;
            }
            const RECT row_rect{panel.content.left + S(panel, 6) + icon_column, right_top,
                                panel.content.right - S(panel, 6), right_top + detail_row_height};
            const int row_dc = SaveDC(dc);
            IntersectClipRect(dc, row_rect.left, visible_top, row_rect.right, clip_bottom);
            if (row.selected) fill_rect(dc, row_rect, theme::kSelection);
            panel.row_rects[row_index] = row_rect;
            panel.row_rects[row_index].top = std::max<LONG>(row_rect.top, visible_top);
            panel.row_rects[row_index].bottom = std::min<LONG>(row_rect.bottom, clip_bottom);
            auto draw_columns = [&]() {
                const int columns = static_cast<int>(row.cells.size());
                const int gap = S(panel, 6);
                const int available = row_rect.right - row_rect.left;
                const int first = columns == 1 ? available : statistics && !panel.statistics_columns.empty() ? panel.statistics_columns[0] :
                    std::min(column_width(panel, 132), available * (columns > 4 ? 26 : 36) / 100);
                const int rest = statistics ? column_width(panel, 82) :
                    columns > 1 ? (available - first) / (columns - 1) : available;
                int statistics_left = row_rect.left;
                for (int column = 0; column < columns; ++column) {
                    const int offset = statistics && column > 0 ? panel.horizontal_px : 0;
                    const int left = (statistics ? statistics_left : row_rect.left + (column == 0 ? 0 : first + rest * (column - 1))) - offset;
                    const int cell_width = columns == 1 ? available : statistics && static_cast<std::size_t>(column) < panel.statistics_columns.size() ? panel.statistics_columns[column] : rest;
                    const int right = statistics ? left + cell_width :
                        column == columns - 1 ? row_rect.right :
                            row_rect.left + (column == 0 ? first : first + rest * column);
                    statistics_left += cell_width;
                    RECT cell{left + gap / 2, row_rect.top, right - gap / 2, row_rect.bottom};
                    RECT visible = cell;
                    if (statistics && column > 0) visible.left = std::max(visible.left, row_rect.left + first);
                    visible.right = std::min(visible.right, row_rect.right);
                    visible.top = std::max<LONG>(visible.top, visible_top);
                    visible.bottom = std::min<LONG>(visible.bottom, clip_bottom);
                    if (visible.left >= visible.right || visible.top >= visible.bottom) continue;
                    const auto& text = row.cells[static_cast<std::size_t>(column)];
                    const int saved = SaveDC(dc);
                    IntersectClipRect(dc, visible.left, visible.top, visible.right, visible.bottom);
                    const COLORREF color = row.header ?
                        (row.accent ? static_cast<COLORREF>(row.accent) : theme::kTextPrimary) :
                        (column == 0 ? theme::kTextPrimary : theme::kTextSecondary);
                    draw_text(dc, text.c_str(), cell, panel.row_font, panel_text_color(panel, color),
                        (column == 0 ? DT_LEFT : DT_RIGHT) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    RestoreDC(dc, saved);
                }
            };
            if (row.wrap)
            {
                RECT text = row_rect;
                InflateRect(&text, -S(panel, 4), -S(panel, 5));
                draw_text(dc, row.left.c_str(), text, panel.row_font,
                    panel_text_color(panel, theme::kTextPrimary), DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
            }
            else if (row.header)
            {
                fill_rect(dc, row_rect, theme::kBackgroundAlt);
                if (!row.cells.empty()) draw_columns();
                else draw_text(dc, row.left.c_str(), row_rect, panel.row_font, panel_text_color(panel, theme::kTextPrimary),
                               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            else if (row.table && row.cells.size() >= 2)
            {
                // PRTS 式网格表格：每两个 cell 是一组「标签/数值」，一行最多 3 组。
                const int groups = static_cast<int>(row.cells.size() / 2);
                const int columns = panel.table_columns[row_index];
                // 组与组之间留出空隙，数值与右侧分隔线之间留 6px 内边距，避免「数值+下一组标签」粘连。
                const int gap = columns > 1 ? S(panel, 14) : 0;
                const int available = row_rect.right - row_rect.left - S(panel, 4) - gap * (columns - 1);
                const int group_width = available / columns;
                for (int group = 0; group < groups; ++group)
                {
                    const int group_left = row_rect.left + (group % columns) * (group_width + gap);
                    const int group_top = row_rect.top + (group / columns) * row_height;
                    RECT label{group_left, group_top, 0, group_top + row_height};
                    // 标签 68% 宽并允许省略号：容纳「攻击范围半径」等 6 字标签，数值区保底 32%。
                    const auto old = SelectObject(dc, panel.row_font);
                    SIZE label_size{}, value_size{};
                    const auto& key_text = row.cells[static_cast<std::size_t>(group * 2)];
                    const auto& value_text = row.cells[static_cast<std::size_t>(group * 2 + 1)];
                    GetTextExtentPoint32W(dc, key_text.c_str(), static_cast<int>(key_text.size()), &label_size);
                    GetTextExtentPoint32W(dc, value_text.c_str(), static_cast<int>(value_text.size()), &value_size);
                    SelectObject(dc, old);
                    label.right = label.left + std::clamp(static_cast<int>(label_size.cx) + S(panel, 4),
                        group_width / 3, std::max(group_width / 3, group_width - static_cast<int>(value_size.cx) - S(panel, 16)));
                    RECT value{label.right + S(panel, 4), group_top, 0, group_top + row_height};
                    value.right = group_left + group_width - S(panel, 6);
                    draw_text(dc, row.cells[static_cast<std::size_t>(group * 2)].c_str(), label,
                              panel.row_font, panel_text_color(panel, theme::kTextSecondary),
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    draw_text(dc, row.cells[static_cast<std::size_t>(group * 2 + 1)].c_str(), value,
                              panel.row_font, panel_text_color(panel, theme::kTextPrimary),
                              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    if (group % columns + 1 < columns && group + 1 < groups)
                    {
                        const int separator_x = group_left + group_width + gap / 2;
                        RECT separator{separator_x, group_top + S(panel, 6), separator_x + 1,
                                       group_top + row_height - S(panel, 6)};
                        fill_rect(dc, separator, theme::kDivider);
                    }
                }
                RECT underline{row_rect.left, row_rect.bottom - 1, row_rect.right, row_rect.bottom};
                fill_rect(dc, underline, theme::kDivider);
            }
            else if (!row.cells.empty())
            {
                draw_columns();
                RECT line{row_rect.left, row_rect.bottom - 1, row_rect.right, row_rect.bottom};
                fill_rect(dc, line, theme::kDivider);
            }
            else
            {
                const COLORREF left_color =
                    row.accent != 0 ? static_cast<COLORREF>(row.accent) : theme::kTextPrimary;
                RECT left=row_rect,right=row_rect;
                if(!row.right.empty() && !row.left.empty()) {
                    left.right=left.left+(row_rect.right-row_rect.left)*46/100;
                    right.left=left.right+S(panel,6);
                }
                draw_text(dc, row.left.c_str(), left, panel.row_font, panel_text_color(panel, left_color),
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                draw_text(dc, row.right.c_str(), right, panel.row_font, panel_text_color(panel, theme::kTextSecondary),
                          DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            RestoreDC(dc, row_dc);
            right_top += detail_row_height;
        }
        RestoreDC(dc, saved_dc);
    }
    if (!panel.collapsed && panel.max_horizontal > 0) {
        fill_round_rect(dc, panel.horizontal_track, S(panel, 3), theme::kCard);
        fill_round_rect(dc, panel.horizontal_thumb, S(panel, 3), theme::kTextSecondary);
    }
    if (panel.max_scroll > 0 && panel.show_scrollbar)
    {
        const int track_top = panel.title_bar.bottom + S(panel, 4);
        const int track_bottom = panel.max_horizontal > 0 ? panel.horizontal_track.top - S(panel, 4) :
            panel.content.bottom - S(panel, 4);
        const int track_height = track_bottom - track_top;
        const int view_height = track_height;
        const int total_height = view_height + panel.max_scroll;
        int thumb_height = total_height > 0 ? view_height * view_height / total_height : view_height;
        if (thumb_height < S(panel, 16))
        {
            thumb_height = S(panel, 16);
        }
        const int travel = track_height - thumb_height;
        const int thumb_top = track_top + (panel.max_scroll > 0 ? travel * panel.scroll_px / panel.max_scroll : 0);
        const RECT thumb{panel.content.right - S(panel, 4), thumb_top, panel.content.right - S(panel, 1),
                         thumb_top + thumb_height};
        fill_round_rect(dc, thumb, S(panel, 2), RGB(150, 165, 195));
    }
    if (!panel.collapsed && panel.max_icon_scroll > 0) {
        const auto& view = panel.icon_viewport;
        const int height = std::max(1L, view.bottom - view.top);
        const int total = height + panel.max_icon_scroll;
        const int thumb_height = std::max(S(panel, 12), height * height / total);
        const int y = view.top + (height - thumb_height) * panel.icon_scroll_px / panel.max_icon_scroll;
        fill_round_rect(dc, RECT{view.right - S(panel, 2), y, view.right, y + thumb_height},
            S(panel, 1), theme::kTextSecondary);
    }
    (void)title_height;
    panel.hints.end();
    // 形状/抗锯齿与预乘 alpha 必须放在所有绘制之后：否则文字与选中背景的像素 alpha 为 0，
    // 在分层窗口上会表现为「文字背景镂空、隔层穿透」。
    apply_round_rect_alpha(panel.surface, margin, radius, S(panel, 6), 80);
}

void notify(PanelState& panel)
{
    if (g_callbacks.on_state_changed)
    {
        g_callbacks.on_state_changed(panel.id, panel.x, panel.y, panel.collapsed);
    }
}

void position_and_present(PanelState& panel)
{
    if (panel.hwnd == nullptr)
    {
        return;
    }
    const int width = panel.surface.width;
    const int height = panel.surface.height;
    SetWindowPos(panel.hwnd, HWND_TOPMOST, panel.x, panel.y, width, height, SWP_NOACTIVATE);
    layered_bitmap_present(panel.hwnd, panel.surface, panel_alpha(panel));
    if (g_shared_cursor.window() && IsWindowVisible(g_shared_cursor.window()))
        SetWindowPos(g_shared_cursor.window(), HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (!panel.visible)
    {
        ShowWindow(panel.hwnd, SW_SHOWNOACTIVATE);
        panel.visible = true;
        refresh_shared_input();
    }
}

void hide(PanelState& panel)
{
    cancel_shared_input(&panel);
    cancel_interaction(panel);
    panel.hints.hide();
    if (panel.visible)
    {
        ShowWindow(panel.hwnd, SW_HIDE);
        panel.visible = false;
        refresh_shared_input();
    }
}

int panel_content_width(const PanelState& panel)
{
    return panel.width_override_px > 0
               ? panel.width_override_px
               : S(panel, panel.auto_width_logical > 0 ? panel.auto_width_logical : kPanelWidthLogical);
}

// 磁吸时以「内容区」为准贴合：窗口四周有阴影边距，若直接按窗口矩形堆叠会留下 2 倍边距的空隙。
RECT dock_content_box(const PanelState& panel)
{
    const int margin = S(panel, kShadowMarginLogical);
    return RECT{panel.x + margin, panel.y + margin, panel.x + panel.surface.width - margin,
                panel.y + panel.surface.height - margin};
}

void restack_group(DockGroup& group)
{
    static bool busy = false;
    if (busy)
    {
        return;
    }
    std::vector<PanelState*> visible;
    for (PanelState* member : group.members)
    {
        if (member->visible && member->hwnd != nullptr)
        {
            visible.push_back(member);
        }
    }
    if (visible.size() < 2)
    {
        return;
    }
    busy = true;
    int max_width = 0;
    for (PanelState* member : visible)
    {
        max_width = std::max(max_width, panel_content_width(*member));
    }
    for (PanelState* member : visible)
    {
        if (member->width_override_px != max_width)
        {
            member->width_override_px = max_width;
            render(*member);
        }
    }
    int x = visible.front()->x;
    int y = visible.front()->y;
    for (PanelState* member : visible)
    {
        const bool moved = member->x != x || member->y != y;
        member->x = x;
        member->y = y;
        render(*member);
        position_and_present(*member);
        if (moved)
        {
            notify(*member);
        }
        const int margin = S(*member, kShadowMarginLogical);
        y += member->surface.height - 2 * margin;
    }
    busy = false;
}

void restack_after(PanelState& panel)
{
    DockGroup* group = find_group(&panel);
    if (group != nullptr)
    {
        restack_group(*group);
    }
}

bool try_dock(PanelState& panel)
{
    if (!panel.visible || panel.hwnd == nullptr)
    {
        return false;
    }
    DockGroup* own_group = find_group(&panel);
    const int snap = S(panel, 18);
    PanelState* best = nullptr;
    for (PanelState* other : g_panels)
    {
        if (other == nullptr || other == &panel || !other->visible || other->hwnd == nullptr)
        {
            continue;
        }
        DockGroup* other_group = find_group(other);
        if (own_group != nullptr && own_group == other_group)
        {
            continue;
        }
        if (std::abs(panel.x - other->x) > snap)
        {
            continue;
        }
        const RECT other_box = dock_content_box(*other);
        const RECT panel_box = dock_content_box(panel);
        const int below_gap = std::abs(other_box.bottom - panel_box.top);
        const int above_gap = std::abs(panel_box.bottom - other_box.top);
        if (below_gap <= snap || above_gap <= snap)
        {
            best = other;
            break;
        }
    }
    if (best == nullptr)
    {
        return false;
    }
    std::vector<PanelState*> members;
    auto add_member = [&members](PanelState* member) {
        if (std::find(members.begin(), members.end(), member) == members.end())
        {
            members.push_back(member);
        }
    };
    DockGroup* best_group = find_group(best);
    if (own_group != nullptr)
    {
        for (PanelState* member : own_group->members)
        {
            add_member(member);
        }
    }
    if (best_group != nullptr)
    {
        for (PanelState* member : best_group->members)
        {
            add_member(member);
        }
    }
    add_member(&panel);
    add_member(best);
    std::vector<DockGroup> kept;
    for (DockGroup& group : g_dock_groups)
    {
        if (&group != own_group && &group != best_group)
        {
            kept.push_back(group);
        }
    }
    g_dock_groups = std::move(kept);
    std::sort(members.begin(), members.end(),
              [](const PanelState* left, const PanelState* right) { return left->y < right->y; });
    DockGroup merged;
    merged.members = members;
    g_dock_groups.push_back(merged);
    restack_group(g_dock_groups.back());
    log_info("panel %s snapped to %s", wide_to_utf8(panel.id).c_str(), wide_to_utf8(best->id).c_str());
    return true;
}

void undock_panel(PanelState& panel)
{
    DockGroup* group = find_group(&panel);
    if (group == nullptr)
    {
        return;
    }
    std::vector<PanelState*> remaining;
    for (PanelState* member : group->members)
    {
        if (member != &panel)
        {
            remaining.push_back(member);
        }
    }
    std::vector<DockGroup> kept;
    for (DockGroup& other : g_dock_groups)
    {
        if (&other != group)
        {
            kept.push_back(other);
        }
    }
    g_dock_groups = std::move(kept);
    if (remaining.size() >= 2)
    {
        DockGroup regrouped;
        regrouped.members = remaining;
        g_dock_groups.push_back(regrouped);
        restack_group(g_dock_groups.back());
    }
    if (!remaining.empty())
    {
        int bottom = 0;
        for (PanelState* member : remaining)
        {
            if (member->visible)
            {
                bottom = std::max(bottom, member->y + member->surface.height);
            }
        }
        panel.x = remaining.front()->x;
        panel.y = bottom;
        render(panel);
        position_and_present(panel);
    }
    notify(panel);
    log_info("panel %s split from dock", wide_to_utf8(panel.id).c_str());
}

void begin_drag(PanelState& panel)
{
    panel.dragging = true;
    mouse_cursor(panel, panel.drag_cursor_start);
    GetWindowRect(panel.hwnd, &panel.drag_window_start);
    capture_mouse(panel);
    g_drag_starts.clear();
    DockGroup* group = find_group(&panel);
    if (group != nullptr)
    {
        for (PanelState* member : group->members)
        {
            if (member->visible && member->hwnd != nullptr)
            {
                g_drag_starts.push_back({member, POINT{member->x, member->y}});
            }
        }
    }
}

LPCWSTR panel_cursor(const PanelState& panel, POINT point)
{
    if ((panel.closeable && PtInRect(&panel.close_button, point)) ||
        PtInRect(&panel.settings_button, point) || PtInRect(&panel.toggle, point) ||
        (has_dock_partner(panel) && PtInRect(&panel.split_button, point)) ||
        (panel.max_horizontal > 0 && PtInRect(&panel.horizontal_track, point))) return IDC_HAND;
    if (PtInRect(&panel.width_grip, point)) return IDC_SIZEWE;
    if (PtInRect(&panel.resize_grip, point)) return panel.collapsed ? IDC_SIZEALL : IDC_SIZENS;
    if (PtInRect(&panel.title_bar, point)) return IDC_SIZEALL;
    return IDC_ARROW;
}

LRESULT CALLBACK panel_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    PanelState* panel = reinterpret_cast<PanelState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (panel == nullptr)
    {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    switch (message)
    {
    case WM_NCHITTEST:
    {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(hwnd, &point);
        if (PtInRect(&panel->content, point))
        {
            return HTCLIENT;
        }
        return HTTRANSPARENT;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
    {
        if (LOWORD(lparam) != HTCLIENT)
        {
            break;
        }
        POINT point{};
        if (!GetCursorPos(&point))
        {
            break;
        }
        ScreenToClient(hwnd, &point);
        SetCursor(LoadCursorW(nullptr, panel_cursor(*panel, point)));
        return TRUE;
    }
    case WM_LBUTTONDOWN:
    {
        panel->hints.hide();
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if(PtInRect(&panel->settings_button,point)) {
            panel->settings_pressed=true;panel->settings_press_point=point;capture_mouse(*panel);return 0;
        }
        if (panel->closeable && PtInRect(&panel->close_button, point)) {
            hide(*panel);
            if (g_callbacks.on_closed) g_callbacks.on_closed(panel->id);
            restack_after(*panel);
            return 0;
        }
        if (panel->max_horizontal > 0 && PtInRect(&panel->horizontal_track, point)) {
            if (!PtInRect(&panel->horizontal_thumb, point)) {
                const int track = panel->horizontal_track.right - panel->horizontal_track.left;
                panel->horizontal_px = std::clamp<int>((point.x - panel->horizontal_track.left) *
                    panel->max_horizontal / std::max(1, track), 0, panel->max_horizontal);
                render(*panel); position_and_present(*panel);
            }
            panel->scrolling_horizontal = true;
            mouse_cursor(*panel, panel->resize_cursor_start);
            panel->horizontal_start_cursor = panel->resize_cursor_start.x;
            panel->horizontal_start_px = panel->horizontal_px;
            capture_mouse(*panel);
            return 0;
        }
        if (PtInRect(&panel->width_grip, point))
        {
            panel->resizing_width = true;
            mouse_cursor(*panel, panel->width_resize_cursor_start);
            panel->resize_start_width = panel->content.right - panel->content.left;
            capture_mouse(*panel);
            return 0;
        }
        if (panel->collapsed && PtInRect(&panel->resize_grip, point))
        {
            // 收纳态没有可调整的高度，把底边留给拖动，避免出现「有光标却拖不动」的死区。
            begin_drag(*panel);
            return 0;
        }
        if (PtInRect(&panel->resize_grip, point))
        {
            panel->resizing = true;
            mouse_cursor(*panel, panel->resize_cursor_start);
            panel->resize_start_height = panel->content.bottom - panel->content.top;
            capture_mouse(*panel);
            return 0;
        }
        if (has_dock_partner(*panel) && PtInRect(&panel->split_button, point))
        {
            undock_panel(*panel);
            return 0;
        }
        if (PtInRect(&panel->toggle, point))
        {
            panel->collapsed = !panel->collapsed;
            render(*panel);
            position_and_present(*panel);
            notify(*panel);
            restack_after(*panel);
            return 0;
        }
        if (PtInRect(&panel->title_bar, point))
        {
            begin_drag(*panel);
            return 0;
        }
        if (std::wcscmp(panel->id,kDamagePanelId)!=0 && PtInRect(&panel->content, point))
        {
            for (std::size_t index = 0; index < panel->row_rects.size(); ++index)
            {
                if (PtInRect(&panel->row_rects[index], point))
                {
                    if (g_callbacks.on_row_clicked)
                    {
                        g_callbacks.on_row_clicked(panel->id, static_cast<int>(index));
                    }
                    return 0;
                }
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        const POINT current{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
        if(panel->settings_pressed && (std::abs(current.x-panel->settings_press_point.x)>S(*panel,4) ||
            std::abs(current.y-panel->settings_press_point.y)>S(*panel,4))) panel->settings_pressed=false;
        if (panel->scrolling_horizontal) {
            POINT cursor{}; mouse_cursor(*panel, cursor);
            const int travel = (panel->horizontal_track.right - panel->horizontal_track.left) -
                (panel->horizontal_thumb.right - panel->horizontal_thumb.left);
            panel->horizontal_px = std::clamp<int>(panel->horizontal_start_px +
                (cursor.x - panel->horizontal_start_cursor) * panel->max_horizontal / std::max(1, travel),
                0, panel->max_horizontal);
            render(*panel); position_and_present(*panel);
            return 0;
        }
        if (panel->resizing_width)
        {
            POINT cursor{};
            mouse_cursor(*panel, cursor);
            int width = panel->resize_start_width + (cursor.x - panel->width_resize_cursor_start.x);
            const int minimum = S(*panel, std::wcscmp(panel->id, kDamagePanelId) == 0 ? 260 : 180);
            if (width < minimum)
            {
                width = minimum;
            }
            if (width > 900)
            {
                width = 900;
            }
            panel->width_override_px = width;
            if (DockGroup* group = find_group(panel))
            {
                for (PanelState* member : group->members)
                    if (std::wcscmp(member->id, kDamagePanelId) == 0)
                        width = std::max(width, S(*member, 260));
                for (PanelState* member : group->members)
                {
                    member->width_override_px = width;
                    if(member->visible) { render(*member);position_and_present(*member); }
                }
            }
            else
            {
                render(*panel);
                position_and_present(*panel);
            }
            restack_after(*panel);
            return 0;
        }
        if (panel->resizing)
        {
            POINT cursor{};
            mouse_cursor(*panel, cursor);
            int height = panel->resize_start_height + (cursor.y - panel->resize_cursor_start.y);
            const bool statistics = std::wcscmp(panel->id, kDamagePanelId) == 0;
            const int minimum = title_height(*panel) + S(*panel, 6) +
                row_height(*panel) * (statistics ? 3 : 1) + S(*panel, 10) +
                (panel->max_horizontal > 0 ? S(*panel, 18) : 0);
            if (height < minimum)
            {
                height = minimum;
            }
            if (height > 1200)
            {
                height = 1200;
            }
            panel->height_override_px = height;
            render(*panel);
            position_and_present(*panel);
            restack_after(*panel);
            return 0;
        }
        if (panel->dragging)
        {
            POINT cursor{};
            mouse_cursor(*panel, cursor);
            const int dx = cursor.x - panel->drag_cursor_start.x;
            const int dy = cursor.y - panel->drag_cursor_start.y;
            if (g_drag_starts.size() >= 2)
            {
                for (const auto& entry : g_drag_starts)
                {
                    entry.first->x = entry.second.x + dx;
                    entry.first->y = entry.second.y + dy;
                    SetWindowPos(entry.first->hwnd, HWND_TOPMOST, entry.first->x, entry.first->y, 0, 0,
                                 SWP_NOSIZE | SWP_NOACTIVATE);
                    layered_bitmap_present(entry.first->hwnd, entry.first->surface, panel_alpha(*entry.first));
                }
            }
            else
            {
                panel->x = panel->drag_window_start.left + dx;
                panel->y = panel->drag_window_start.top + dy;
                SetWindowPos(hwnd, HWND_TOPMOST, panel->x, panel->y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
                layered_bitmap_present(hwnd, panel->surface, panel_alpha(*panel));
            }
            return 0;
        }
        {
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            const bool want = panel->max_scroll > 0 &&
                              point.x >= panel->content.right - S(*panel, 12) &&
                              point.y >= panel->title_bar.bottom;
            if (want != panel->show_scrollbar)
            {
                panel->show_scrollbar = want;
                render(*panel);
                position_and_present(*panel);
            }

        }
        if (!panel->shared_dispatch) panel->hints.mouse_move(current);
        return 0;
    }
    case WM_MOUSEHOVER:
        panel->hints.hover({GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)});return 0;
    case WM_MOUSELEAVE:
        panel->hints.hide();return 0;
    case WM_MOUSEHWHEEL:
    case WM_MOUSEWHEEL:
    {
        panel->hints.hide();panel->settings_pressed=false;
        POINT wheel_point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd, &wheel_point);
        if (message == WM_MOUSEWHEEL && PtInRect(&panel->icon_viewport, wheel_point)) {
            panel->icon_scroll_px = std::clamp(panel->icon_scroll_px -
                GET_WHEEL_DELTA_WPARAM(wparam) * row_height(*panel) * 2 / WHEEL_DELTA, 0, panel->max_icon_scroll);
            render(*panel); position_and_present(*panel);
            return 0;
        }
        if (panel->max_horizontal > 0 && (message == WM_MOUSEHWHEEL || (GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT))) {
            const int direction = message == WM_MOUSEHWHEEL ? 1 : -1;
            panel->horizontal_px = std::clamp(panel->horizontal_px + direction *
                GET_WHEEL_DELTA_WPARAM(wparam) * column_width(*panel, 82) / WHEEL_DELTA, 0, panel->max_horizontal);
            render(*panel); position_and_present(*panel);
            return 0;
        }
        if (panel->max_scroll > 0)
        {
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            panel->scroll_px -= (delta / WHEEL_DELTA) * row_height(*panel) * 2;
            if (panel->scroll_px < 0)
            {
                panel->scroll_px = 0;
            }
            if (panel->scroll_px > panel->max_scroll)
            {
                panel->scroll_px = panel->max_scroll;
            }
            panel->show_scrollbar = true;
            render(*panel);
            position_and_present(*panel);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if(panel->settings_pressed) {
            const POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
            panel->settings_pressed=false;release_mouse(*panel);
            if(PtInRect(&panel->settings_button,point) && g_callbacks.on_settings) g_callbacks.on_settings(panel->id);
            return 0;
        }
        if (panel->scrolling_horizontal) {
            panel->scrolling_horizontal = false; release_mouse(*panel); return 0;
        }
        if (panel->resizing_width)
        {
            panel->resizing_width = false;
            release_mouse(*panel);
            if (g_callbacks.on_resized)
            {
                if(auto* group=find_group(panel)) {
                    for(auto* member:group->members) g_callbacks.on_resized(member->id,member->width_override_px,member->height_override_px);
                } else g_callbacks.on_resized(panel->id, panel->width_override_px, panel->height_override_px);
            }
            return 0;
        }
        if (panel->resizing)
        {
            panel->resizing = false;
            release_mouse(*panel);
            if (g_callbacks.on_resized)
            {
                g_callbacks.on_resized(panel->id, panel->width_override_px, panel->height_override_px);
            }
            return 0;
        }
        if (panel->dragging)
        {
            panel->dragging = false;
            release_mouse(*panel);
            g_drag_starts.clear();
            if (!try_dock(*panel))
            {
                notify(*panel);
            }
        }
        return 0;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        cancel_interaction(*panel);
        return 0;
    case WM_DESTROY:
        panel->hints.destroy();
        destroy_fonts(*panel);
        layered_bitmap_destroy(panel->surface);
        panel->hwnd = nullptr;
        panel->visible = false;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool create_panel(PanelState& panel)
{
    if (!panel.class_registered)
    {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = panel_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.lpszClassName = panel.class_name;
        if (RegisterClassExW(&window_class) == 0)
        {
            log_error("RegisterClassExW(%s) failed, code=%d", wide_to_utf8(panel.class_name).c_str(),
                      static_cast<int>(GetLastError()));
            return false;
        }
        panel.class_registered = true;
    }
    panel.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                                 panel.class_name, L"", WS_POPUP, 0, 0, 32, 32, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
    if (panel.hwnd == nullptr)
    {
        log_error("CreateWindowExW(%s) failed, code=%d", wide_to_utf8(panel.class_name).c_str(),
                  static_cast<int>(GetLastError()));
        return false;
    }
    SetWindowLongPtrW(panel.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&panel));
    return true;
}

PanelState* find(const wchar_t* id)
{
    for (PanelState* panel : g_panels)
    {
        if (panel != nullptr && id != nullptr && std::wcscmp(panel->id, id) == 0)
        {
            return panel;
        }
    }
    return nullptr;
}

PanelState* shared_mouse_hit(POINT screen)
{
    if (GetCapture()) return nullptr;
    const HWND native = GetAncestor(WindowFromPoint(screen), GA_ROOT);
    if (!native) return nullptr;
    DWORD process = 0;
    GetWindowThreadProcessId(native, &process);
    // Menus, the floating button and ordinary panels keep exclusive interaction.
    if (process == GetCurrentProcessId()) return nullptr;
    for (HWND window = GetTopWindow(nullptr); window && window != native; window = GetWindow(window, GW_HWNDNEXT)) {
        for (auto* panel : g_panels) {
            if (!panel || window != panel->hwnd || !panel->visible || !panel->appearance.click_through) continue;
            POINT point = screen;
            ScreenToClient(window, &point);
            if (PtInRect(&panel->content, point)) return panel;
        }
    }
    return nullptr;
}

void send_shared_mouse(PanelState& panel, const SharedMouseEvent& event)
{
    POINT point = event.screen;
    ScreenToClient(panel.hwnd, &point);
    panel.shared_dispatch = true;
    panel.shared_cursor = event.screen;
    const bool wheel = event.message == WM_MOUSEWHEEL || event.message == WM_MOUSEHWHEEL;
    const POINT coordinates = wheel ? event.screen : point;
    SendMessageW(panel.hwnd, event.message, event.keys, MAKELPARAM(coordinates.x, coordinates.y));
    panel.shared_dispatch = false;
}

void dispatch_shared_mouse(const SharedMouseEvent& event)
{
    if (event.message == WM_CANCELMODE) { cancel_shared_input(); return; }
    if (g_shared_gesture && (!g_shared_gesture->visible || !g_shared_gesture->appearance.click_through ||
        GetCapture() || GetAncestor(GetForegroundWindow(), GA_ROOT) != g_shared_source)) cancel_shared_input();

    PanelState* target = g_shared_gesture;
    if (event.message == WM_LBUTTONDOWN) {
        cancel_shared_input();
        target = shared_mouse_hit(event.screen);
        if (target) {
            g_shared_gesture = target;
            g_shared_source = GetAncestor(WindowFromPoint(event.screen), GA_ROOT);
        }
    } else if (!target && event.message != WM_LBUTTONUP) {
        target = (event.keys & MK_LBUTTON) ? nullptr : shared_mouse_hit(event.screen);
    }
    if (g_shared_hover != target) {
        if (g_shared_hover) g_shared_hover->hints.hide();
        g_shared_hover = target;
        g_shared_hover_since = GetTickCount64();
    }
    if (!target) { g_shared_cursor.hide(); return; }
    if (std::abs(event.screen.x - g_shared_hover_point.x) > 3 || std::abs(event.screen.y - g_shared_hover_point.y) > 3) {
        target->hints.hide();
        g_shared_hover_point = event.screen;
        g_shared_hover_since = GetTickCount64();
    }
    send_shared_mouse(*target, event);
    if (event.message == WM_LBUTTONUP) {
        g_shared_gesture = nullptr;
        g_shared_source = nullptr;
    }
}

void refresh_shared_cursor()
{
    POINT point{};
    GetCursorPos(&point);
    auto* panel = shared_mouse_hit(point);
    CURSORINFO info{sizeof(info)};
    if (!panel || (GetCursorInfo(&info) && (info.flags & CURSOR_SHOWING))) {
        g_shared_cursor.hide();
        return;
    }
    POINT local = point;
    ScreenToClient(panel->hwnd, &local);
    g_shared_cursor.show(point, LoadCursorW(nullptr, panel_cursor(*panel, local)));
}

LRESULT CALLBACK shared_mouse_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == kSharedMouseMessage) {
        for (const auto& event : g_shared_mouse.take()) dispatch_shared_mouse(event);
        refresh_shared_cursor();
        return 0;
    }
    if (message == WM_TIMER && wparam == kSharedMouseTimer) {
        refresh_shared_cursor();
        if (g_shared_gesture) {
            if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000) ||
                GetAncestor(GetForegroundWindow(), GA_ROOT) != g_shared_source) cancel_shared_input();
        } else if (g_shared_hover) {
            POINT point{};
            GetCursorPos(&point);
            if (shared_mouse_hit(point) != g_shared_hover) cancel_shared_input();
            else if (GetTickCount64() - g_shared_hover_since >= 250) {
                ScreenToClient(g_shared_hover->hwnd, &point);
                g_shared_hover->hints.hover(point);
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void refresh_shared_input()
{
    const bool enabled = std::any_of(std::begin(g_panels), std::end(g_panels), [](const auto* panel) {
        return panel && panel->visible && panel->appearance.click_through;
    });
    if (!enabled) {
        cancel_shared_input();
        g_shared_mouse.stop();
        if (g_shared_window) KillTimer(g_shared_window, kSharedMouseTimer);
        return;
    }
    if (!g_shared_window) {
        WNDCLASSW window_class{};
        window_class.hInstance = g_instance;
        window_class.lpfnWndProc = shared_mouse_proc;
        window_class.lpszClassName = L"ArkHelper.SharedMouse";
        if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
        g_shared_window = CreateWindowExW(0, window_class.lpszClassName, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, g_instance, nullptr);
    }
    if (!g_shared_window || !g_shared_mouse.start(g_shared_window, kSharedMouseMessage)) {
        log_error("shared panel mouse input could not start, code=%lu", GetLastError());
        // Keep the panels usable if input observation is unavailable.
        for (auto* panel : g_panels) if (panel && panel->hwnd)
            SetWindowLongPtrW(panel->hwnd, GWL_EXSTYLE, GetWindowLongPtrW(panel->hwnd, GWL_EXSTYLE) & ~WS_EX_TRANSPARENT);
        return;
    }
    for (auto* panel : g_panels) if (panel && panel->hwnd && panel->appearance.click_through)
        SetWindowLongPtrW(panel->hwnd, GWL_EXSTYLE, GetWindowLongPtrW(panel->hwnd, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
    SetTimer(g_shared_window, kSharedMouseTimer, 60, nullptr);
}

}  // namespace

bool initialize(HINSTANCE instance, Callbacks callbacks)
{
    g_instance = instance;
    g_callbacks = std::move(callbacks);
    static PanelState damage;
    static PanelState spawn;
    static PanelState wiki;
    static PanelState prts;
    damage.id = kDamagePanelId;
    damage.class_name = kDamageClass;
    spawn.id = kSpawnPanelId;
    spawn.class_name = kSpawnClass;
    wiki.id = kWikiPanelId;
    wiki.class_name = kWikiClass;
    prts.id = kPrtsPanelId;
    prts.class_name = kPrtsClass;
    damage.dpi = dpi_for_system();
    spawn.dpi = dpi_for_system();
    wiki.dpi = dpi_for_system();
    prts.dpi = dpi_for_system();
    g_panels[0] = &damage;
    g_panels[1] = &spawn;
    g_panels[2] = &wiki;
    g_panels[3] = &prts;
    if (!create_panel(damage))
    {
        return false;
    }
    if (!create_panel(spawn))
    {
        return false;
    }
    if (!create_panel(wiki))
    {
        return false;
    }
    if (!create_panel(prts)) return false;
    return true;
}

void fit_statistics_width(const std::vector<Row>& rows)
{
    auto* panel = find(kDamagePanelId);
    if (!panel) return;
    if (!panel->row_font) create_fonts(*panel);
    const auto columns = measure_statistics(*panel, rows);
    int total = S(*panel, 12);
    for (int width : columns) total += width;
    const int unit = std::max(1, S(*panel, 100));
    fit_width(kDamagePanelId, std::clamp((total * 100 + unit - 1) / unit, 260, 800));
}

void shutdown()
{
    g_shared_cursor.destroy();
    cancel_shared_input();
    g_shared_mouse.stop();
    if (g_shared_window) { DestroyWindow(g_shared_window); g_shared_window = nullptr; }
    g_dock_groups.clear();
    g_drag_starts.clear();
    for (PanelState* panel : g_panels)
    {
        if (panel != nullptr && panel->hwnd != nullptr)
        {
            DestroyWindow(panel->hwnd);
        }
    }
}

void restore_geometry(const wchar_t* id, int x, int y, bool collapsed)
{
    PanelState* panel = find(id);
    if (panel == nullptr)
    {
        return;
    }
    panel->collapsed = collapsed;
    if (x == kNoPosition || y == kNoPosition)
    {
        default_position(*panel, panel_slot(*panel));
    }
    else
    {
        panel->x = x;
        panel->y = y;
    }
}

void restore_height(const wchar_t* id, int height_px)
{
    PanelState* panel = find(id);
    if (panel == nullptr)
    {
        return;
    }
    panel->height_override_px = height_px > 0 ? height_px : 0;
    if (panel->visible)
    {
        render(*panel);
        position_and_present(*panel);
    }
}

void restore_width(const wchar_t* id, int width_px)
{
    PanelState* panel = find(id);
    if (panel == nullptr)
    {
        return;
    }
    panel->width_override_px = width_px > 0 ?
        std::max(width_px, S(*panel, std::wcscmp(panel->id, kDamagePanelId) == 0 ? 260 : 180)) : 0;
    if (panel->visible)
    {
        render(*panel);
        position_and_present(*panel);
    }
}

std::wstring save_dock_state()
{
    std::wstring result;
    for (const DockGroup& group : g_dock_groups)
    {
        if (group.members.size() < 2)
        {
            continue;
        }
        std::vector<PanelState*> members = group.members;
        std::sort(members.begin(), members.end(),
                  [](const PanelState* left, const PanelState* right) { return left->y < right->y; });
        if (!result.empty())
        {
            result += L'|';
        }
        for (size_t index = 0; index < members.size(); ++index)
        {
            if (index > 0)
            {
                result += L'>';
            }
            result += members[index]->id;
        }
    }
    return result;
}

void restore_dock_state(const std::wstring& state)
{
    g_dock_groups.clear();
    if (state.empty())
    {
        return;
    }
    size_t start = 0;
    while (start <= state.size())
    {
        const size_t bar = state.find(L'|', start);
        const std::wstring group_text =
            state.substr(start, bar == std::wstring::npos ? std::wstring::npos : bar - start);
        std::vector<PanelState*> members;
        size_t cursor = 0;
        while (cursor <= group_text.size())
        {
            const size_t sep = group_text.find(L'>', cursor);
            const std::wstring id =
                group_text.substr(cursor, sep == std::wstring::npos ? std::wstring::npos : sep - cursor);
            if (PanelState* panel = find(id.c_str()))
            {
                members.push_back(panel);
            }
            if (sep == std::wstring::npos)
            {
                break;
            }
            cursor = sep + 1;
        }
        if (members.size() >= 2)
        {
            g_dock_groups.push_back(DockGroup{std::move(members)});
        }
        if (bar == std::wstring::npos)
        {
            break;
        }
        start = bar + 1;
    }
    for (DockGroup& group : g_dock_groups)
    {
        restack_group(group);
    }
}


void refresh_theme(int ui_scale)
{
    for (PanelState* panel : g_panels)
    {
        if (panel == nullptr)
        {
            continue;
        }
        panel->ui_scale = ui_scale > 0 ? ui_scale : 100;
        destroy_fonts(*panel);
        if (panel->visible)
        {
            render(*panel);
            position_and_present(*panel);
        }
    }
}

void set_avatars(const wchar_t* id, const std::vector<HBITMAP>& avatars, int logical_size)
{
    PanelState* panel = find(id);
    if (panel == nullptr)
    {
        return;
    }
    unsigned long long key = avatars.size();
    for (HBITMAP bitmap : avatars)
    {
        key = key * 1099511628211ull + reinterpret_cast<unsigned long long>(bitmap);
    }
    if (panel->avatar_key == key && panel->avatar_logical_size == logical_size)
    {
        return;
    }
    panel->avatars = avatars;
    panel->avatar_key = key;
    panel->avatar_logical_size = logical_size > 0 ? logical_size : 18;
    if (panel->visible)
    {
        render(*panel);
        position_and_present(*panel);
    }
}

void set_auto_width(const wchar_t* id, int logical_width)
{
    PanelState* panel = find(id);
    if (panel == nullptr)
    {
        return;
    }
    if (logical_width < 0)
    {
        logical_width = 0;
    }
    if (panel->auto_width_logical == logical_width)
    {
        return;
    }
    panel->auto_width_logical = logical_width;
    if (panel->visible)
    {
        render(*panel);
        position_and_present(*panel);
    }
}

void reset_scroll(const wchar_t* id, bool include_avatars) {
    if (auto* panel = find(id)) {
        panel->scroll_px = 0;
        if (include_avatars) panel->icon_scroll_px = 0;
    }
}

void fit_reference_width(const wchar_t* id, const std::vector<Row>& rows) {
    auto* panel = find(id);
    if (!panel) return;
    if (!panel->row_font) create_fonts(*panel);
    const auto dc = GetDC(nullptr);
    const auto old = SelectObject(dc, panel->row_font);
    int wanted = S(*panel, 300);
    bool icons = false;
    for (const auto& row : rows) {
        icons |= row.icon_only;
        if (row.table) {
            int group = S(*panel, 110);
            for (std::size_t j = 0; j + 1 < row.cells.size(); j += 2) {
                SIZE key{}, value{};
                GetTextExtentPoint32W(dc, row.cells[j].c_str(), static_cast<int>(row.cells[j].size()), &key);
                GetTextExtentPoint32W(dc, row.cells[j+1].c_str(), static_cast<int>(row.cells[j+1].size()), &value);
                group = std::max(group, static_cast<int>(key.cx + value.cx) + S(*panel, 24));
            }
            const int columns = std::min(3, static_cast<int>(row.cells.size() / 2));
            wanted = std::max(wanted, group * columns + S(*panel, 14) * std::max(0, columns - 1));
        } else if (row.wrap) wanted = std::max(wanted, S(*panel, 480));
    }
    SelectObject(dc, old); ReleaseDC(nullptr, dc);
    wanted += S(*panel, 12 + (icons ? panel->avatar_logical_size + 6 : 0));
    fit_width(id, std::clamp(MulDiv(wanted, 100, S(*panel, 100)), 300, 760));
}

void fit_width(const wchar_t* id, int logical_width) {
    auto* panel = find(id);
    if (!panel) return;
    panel->auto_width_logical = logical_width;
    int wanted = S(*panel, logical_width > 0 ? logical_width : kPanelWidthLogical);
    auto* group = find_group(panel);
    PanelState* anchor = panel;
    int largest_margin = S(*panel, kShadowMarginLogical);
    if (group) {
        for (auto* member : group->members) {
            largest_margin = std::max(largest_margin, S(*member, kShadowMarginLogical));
            if (member != panel && member->visible)
                wanted = std::max(wanted, S(*member, member->auto_width_logical > 0 ?
                    member->auto_width_logical : kPanelWidthLogical));
        }
        for (auto* member : group->members) {
            if (member->visible) { anchor = member; break; }
        }
    }
    if (panel->x == kNoPosition || panel->y == kNoPosition)
        default_position(*panel, panel_slot(*panel));
    if (anchor->x == kNoPosition || anchor->y == kNoPosition) anchor = panel;
    // A content fit is a one-shot operation. Clamp its full window width (including shadow)
    // to this monitor and move the whole dock left when expanding near its right edge.
    const RECT location{panel->x, panel->y,
        panel->x + std::max(1, panel_content_width(*panel)), panel->y + title_height(*panel)};
    MONITORINFO monitor{sizeof(MONITORINFO)};
    RECT work{};
    if (GetMonitorInfoW(MonitorFromRect(&location, MONITOR_DEFAULTTONEAREST), &monitor)) work = monitor.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    wanted = std::min(wanted, std::max(1, static_cast<int>(work.right - work.left) - 2 * largest_margin));
    const int rightmost = std::max(static_cast<int>(work.left),
        static_cast<int>(work.right) - wanted - 2 * largest_margin);
    const int fitted_x = std::clamp(anchor->x, static_cast<int>(work.left), rightmost);
    const auto apply = [&](PanelState* member) {
        const bool changed = member->width_override_px != wanted;
        const bool moved = member->x != fitted_x;
        member->x = fitted_x;
        member->width_override_px = wanted;
        if (member->visible) { render(*member); position_and_present(*member); }
        if (moved) notify(*member);
        if (changed && g_callbacks.on_resized)
            g_callbacks.on_resized(member->id, wanted, member->height_override_px);
    };
    if (group) for (auto* member : group->members) apply(member);
    else apply(panel);
    restack_after(*panel);
}

void set_closeable(const wchar_t* id, bool closeable) {
    auto* panel = find(id);
    if (!panel || panel->closeable == closeable) return;
    panel->closeable = closeable;
    if (panel->visible) { render(*panel); position_and_present(*panel); }
}

void set_appearance(const wchar_t* id, const FeatureAppearance& appearance)
{
    auto* panel = find(id);
    if (!panel) return;
    FeatureAppearance next = appearance;
    next.opacity = std::clamp(next.opacity, 20, 100);
    next.font_size = next.font_size == 0 ? 0 : std::clamp(next.font_size, 10, 32);
    next.text_color = next.text_color < 0 ? -1 : next.text_color & 0xFFFFFF;
    const auto& current = panel->appearance;
    if (current.opacity == next.opacity && current.font_size == next.font_size &&
        current.text_color == next.text_color && current.click_through == next.click_through) return;
    const bool font_changed = current.font_size != next.font_size;
    if (current.click_through != next.click_through) {
        cancel_shared_input(panel);
        cancel_interaction(*panel);
    }
    panel->appearance = next;
    auto style = GetWindowLongPtrW(panel->hwnd, GWL_EXSTYLE);
    style = next.click_through ? style | WS_EX_TRANSPARENT : style & ~WS_EX_TRANSPARENT;
    SetWindowLongPtrW(panel->hwnd, GWL_EXSTYLE, style);
    if (next.click_through) panel->hints.hide();
    refresh_shared_input();
    if (font_changed) destroy_fonts(*panel);
    if (panel->visible)
    {
        render(*panel);
        position_and_present(*panel);
        restack_after(*panel);
    }
}

void set_statistics_compact(bool compact)
{
    auto* panel = find(kDamagePanelId);
    if (!panel || panel->compact == compact) return;
    panel->compact = compact;
    if (panel->visible) { render(*panel); position_and_present(*panel); restack_after(*panel); }
}

void update(const wchar_t* id, const std::wstring& title, const std::vector<Row>& rows, bool visible)
{
    PanelState* panel = find(id);
    if (panel == nullptr || panel->hwnd == nullptr)
    {
        return;
    }
    if (!visible)
    {
        hide(*panel);
        return;
    }
    const bool changed = panel->title != title || !rows_equal(panel->rows, rows);
    panel->title = title;
    panel->rows = rows;
    if (panel->x == kNoPosition || panel->y == kNoPosition)
    {
        default_position(*panel, panel_slot(*panel));
    }
    if (changed || !panel->visible)
    {
        render(*panel);
        position_and_present(*panel);
        restack_after(*panel);
    }
}

}  // namespace arkhelp::panels
