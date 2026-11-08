#pragma once

#include <windows.h>

#include <string>
#include <vector>

// 通用 Win32 小工具：路径解析、UTF-8/UTF-16 转换、DPI 缩放、分层窗口位图、GDI 绘制助手。
// 抽这一层是为了让各窗口代码只关心布局与交互，不重复处理 UpdateLayeredWindow 的细节。
namespace arkhelp
{

std::wstring module_dir();
std::wstring app_root();
std::wstring join_path(const std::wstring& base, const std::wstring& leaf);
bool ensure_directory(const std::wstring& path);

std::wstring utf8_to_wide(const std::string& text);
std::string wide_to_utf8(const std::wstring& text);

int dpi_for_system();
int dpi_for_window(HWND window);
int scale_by_dpi(int logical_px, int dpi);
int scale_ui(int logical_px, int dpi, int ui_percent);

HFONT create_ui_font(int logical_px, int weight, int dpi, int ui_percent);
void draw_text(HDC dc, const wchar_t* text, const RECT& rect, HFONT font, COLORREF color, UINT format);
void fill_rect(HDC dc, const RECT& rect, COLORREF color);
void fill_round_rect(HDC dc, const RECT& rect, int radius, COLORREF color);
void stroke_round_rect(HDC dc, const RECT& rect, int radius, COLORREF color, int thickness);
void draw_settings_icon(HDC dc,const RECT& rect,COLORREF color);

// Every self-drawn text uses the same clipping detector. Regions are stored in client
// coordinates, after the DC's viewport transform and clip have both been applied.
class OverflowHints {
public:
    struct Item { RECT rect{};std::wstring text; };
    void begin(HWND owner,HDC dc);
    void end();
    void capture(HDC dc,const wchar_t* text,const RECT& rect,UINT format);
    void mouse_move(POINT point);
    void hover(POINT point);
    void hide();
    void destroy();
    std::wstring text_at(POINT point) const;
    const std::vector<Item>& items() const { return items_; }
private:
    bool create();
    HWND owner_=nullptr,tooltip_=nullptr;
    HDC dc_=nullptr;
    HFONT font_=nullptr;
    std::vector<Item> items_;
    std::wstring active_;
};

// 32bpp 顶朝下 DIB + 兼容 DC，配合 UpdateLayeredWindow 实现逐像素 alpha 窗口。
struct LayeredBitmap
{
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HBITMAP old_bitmap = nullptr;
    void* bits = nullptr;
    int width = 0;
    int height = 0;
};

bool layered_bitmap_create(LayeredBitmap& surface, int width, int height);
void layered_bitmap_destroy(LayeredBitmap& surface);
void layered_bitmap_clear(LayeredBitmap& surface);
bool layered_bitmap_present(HWND window, LayeredBitmap& surface, BYTE opacity);

// 从 PNG 读取并按目标尺寸缩放成 32bpp 顶朝下 DIB（带 alpha），用于球图标 / 菜单 Logo。
// 依赖调用方已 CoInitializeEx（App::initialize 已初始化 COM）。
HBITMAP load_png_scaled(const std::wstring& path, int width, int height);
// 从内存中的 PNG/JPEG 解码并缩放到指定尺寸（WIKI 头像用，网络字节流不解码到临时文件）。
HBITMAP load_image_memory(const void* data, unsigned long long size, int width, int height);
void destroy_bitmap(HBITMAP bitmap);

// 圆角矩形逐像素 alpha 处理：
// - 形状内保留 GDI 画好的颜色（不透明）；
// - 形状外 1px 抗锯齿 + 向外 shadow_blur 像素的黑色投影（alpha 二次衰减）；
// - 最后统一做预乘（UpdateLayeredWindow 的 AC_SRC_ALPHA 要求预乘颜色）。
void apply_round_rect_alpha(LayeredBitmap& surface, int margin, int radius, int shadow_blur, int shadow_alpha);

}  // namespace arkhelp
