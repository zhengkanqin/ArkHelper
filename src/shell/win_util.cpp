#include "win_util.h"

#include <wincodec.h>
#include <shlwapi.h>
#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <cwchar>
#include <cstddef>

// 这里的实现尽量只用 Win32 + CRT 基础函数，不依赖 C++ 文件系统，保证 Zig/MinGW 下行为稳定。
namespace arkhelp
{
namespace
{
thread_local OverflowHints* g_overflow_hints=nullptr;
// The system tooltip class without a v6 activation context accepts the v2 layout.
constexpr UINT kToolInfoSize=offsetof(TOOLINFOW,lParam)+sizeof(LPARAM);

float clamp_float(float value, float low, float high)
{
    if (value < low)
    {
        return low;
    }
    if (value > high)
    {
        return high;
    }
    return value;
}

// 圆角矩形的有符号距离：<0 在内部，=0 在边界，>0 在外部。
float rounded_rect_sdf(float px, float py, float left, float top, float right, float bottom, float radius)
{
    const float cx = (left + right) * 0.5f;
    const float cy = (top + bottom) * 0.5f;
    const float hx = (right - left) * 0.5f - radius;
    const float hy = (bottom - top) * 0.5f - radius;
    const float dx = std::fabs(px - cx) - hx;
    const float dy = std::fabs(py - cy) - hy;
    const float ox = dx > 0.0f ? dx : 0.0f;
    const float oy = dy > 0.0f ? dy : 0.0f;
    const float outside = std::sqrt(ox * ox + oy * oy);
    const float inside = std::min(std::max(dx, dy), 0.0f);
    return outside + inside - radius;
}

}  // namespace

std::wstring module_dir()
{
    wchar_t buffer[MAX_PATH * 4];
    DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
    if (length == 0)
    {
        return L".";
    }
    std::wstring path(buffer, length);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
    {
        return L".";
    }
    return path.substr(0, slash);
}

std::wstring app_root()
{
    // Portable packages keep configuration, logs and data next to their executable.
    return module_dir();
}

std::wstring join_path(const std::wstring& base, const std::wstring& leaf)
{
    if (base.empty())
    {
        return leaf;
    }
    const wchar_t last = base.back();
    if (last == L'\\' || last == L'/')
    {
        return base + leaf;
    }
    return base + L"\\" + leaf;
}

bool ensure_directory(const std::wstring& path)
{
    if (path.empty())
    {
        return false;
    }
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        return true;
    }

    // 逐级创建，跳过盘符前缀（"D:"）与 UNC 起始部分。
    std::wstring current;
    current.reserve(path.size());
    size_t index = 0;
    if (path.size() >= 2 && path[1] == L':')
    {
        current = path.substr(0, 2);
        index = 2;
    }
    while (index < path.size())
    {
        const wchar_t ch = path[index++];
        current.push_back(ch);
        const bool separator = ch == L'\\' || ch == L'/';
        if (!separator || index >= path.size())
        {
            continue;
        }
        if (current.size() > 3)
        {
            CreateDirectoryW(current.c_str(), nullptr);
        }
    }
    CreateDirectoryW(path.c_str(), nullptr);
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring utf8_to_wide(const std::string& text)
{
    if (text.empty())
    {
        return std::wstring();
    }
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0)
    {
        return std::wstring();
    }
    std::wstring wide(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), count);
    return wide;
}

std::string wide_to_utf8(const std::wstring& text)
{
    if (text.empty())
    {
        return std::string();
    }
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (count <= 0)
    {
        return std::string();
    }
    std::string narrow(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), narrow.data(), count, nullptr,
                        nullptr);
    return narrow;
}

int dpi_for_system()
{
    const int dpi = static_cast<int>(GetDpiForSystem());
    if (dpi > 0)
    {
        return dpi;
    }
    HDC screen = GetDC(nullptr);
    const int fallback = screen ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
    if (screen)
    {
        ReleaseDC(nullptr, screen);
    }
    return fallback > 0 ? fallback : 96;
}

int dpi_for_window(HWND window)
{
    if (window != nullptr)
    {
        const int dpi = static_cast<int>(GetDpiForWindow(window));
        if (dpi > 0)
        {
            return dpi;
        }
    }
    return dpi_for_system();
}

int scale_by_dpi(int logical_px, int dpi)
{
    return MulDiv(logical_px, dpi, 96);
}

int scale_ui(int logical_px, int dpi, int ui_percent)
{
    const int percent = ui_percent > 0 ? ui_percent : 100;
    return MulDiv(MulDiv(logical_px, dpi, 96), percent, 100);
}

HFONT create_ui_font(int logical_px, int weight, int dpi, int ui_percent)
{
    // 微软雅黑 UI 对中文/数字混排最稳；高度取负值表示字符高度（像素），便于按 DPI 缩放。
    const int height = -scale_ui(logical_px, dpi, ui_percent);
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       L"Microsoft YaHei UI");
}

void draw_text(HDC dc, const wchar_t* text, const RECT& rect, HFONT font, COLORREF color, UINT format)
{
    if (dc == nullptr || text == nullptr)
    {
        return;
    }
    const int saved = SetBkMode(dc, TRANSPARENT);
    const COLORREF old_color = SetTextColor(dc, color);
    HGDIOBJ old_font = nullptr;
    if (font != nullptr)
    {
        old_font = SelectObject(dc, font);
    }
    RECT target = rect;
    if(g_overflow_hints) g_overflow_hints->capture(dc,text,rect,format);
    DrawTextW(dc, text, -1, &target, format);
    if (old_font != nullptr)
    {
        SelectObject(dc, old_font);
    }
    SetTextColor(dc, old_color);
    SetBkMode(dc, saved);
}

void OverflowHints::begin(HWND owner,HDC dc) {
    owner_=owner;dc_=dc;items_.clear();g_overflow_hints=this;
}
void OverflowHints::end() {
    if(g_overflow_hints==this) g_overflow_hints=nullptr;
    if(!active_.empty()) {
        const auto found=std::find_if(items_.begin(),items_.end(),[&](const auto& item){return item.text==active_;});
        if(found==items_.end()) hide();
    }
    dc_=nullptr;
}
void OverflowHints::capture(HDC dc,const wchar_t* text,const RECT& rect,UINT format) {
    if(dc!=dc_ || !text || !*text || (format&DT_CALCRECT) || rect.right<=rect.left || rect.bottom<=rect.top) return;
    RECT clip{};
    const int type=GetClipBox(dc,&clip);
    if(type==NULLREGION || type==ERROR) return;
    RECT available=rect;
    if(!(format&DT_NOCLIP) && !IntersectRect(&available,&available,&clip)) return;
    if(format&DT_NOCLIP) available=clip;
    RECT natural{0,0,rect.right-rect.left,0};
    UINT measure=(format & ~(DT_END_ELLIPSIS|DT_PATH_ELLIPSIS|DT_WORD_ELLIPSIS|DT_MODIFYSTRING|DT_NOCLIP|
        DT_CENTER|DT_RIGHT|DT_VCENTER|DT_BOTTOM))|DT_CALCRECT;
    DrawTextW(dc,text,-1,&natural,measure);
    const int width=natural.right-natural.left,height=natural.bottom-natural.top;
    RECT ink{rect.left,rect.top,rect.left+width,rect.top+height};
    if(format&DT_RIGHT) OffsetRect(&ink,rect.right-ink.right,0);
    else if(format&DT_CENTER) OffsetRect(&ink,((rect.right-rect.left)-width)/2,0);
    if(format&DT_VCENTER) OffsetRect(&ink,0,((rect.bottom-rect.top)-height)/2);
    else if(format&DT_BOTTOM) OffsetRect(&ink,0,rect.bottom-ink.bottom);
    if(ink.left>=available.left && ink.right<=available.right && ink.top>=available.top && ink.bottom<=available.bottom) return;
    RECT hit{};
    if(!IntersectRect(&hit,&rect,&clip)) return;
    POINT points[]={{hit.left,hit.top},{hit.right,hit.bottom}};
    LPtoDP(dc,points,2);
    items_.push_back({{points[0].x,points[0].y,points[1].x,points[1].y},text});
}
std::wstring OverflowHints::text_at(POINT point) const {
    for(auto item=items_.rbegin();item!=items_.rend();++item) if(PtInRect(&item->rect,point)) return item->text;
    return {};
}
void OverflowHints::add(const RECT& rect, const std::wstring& text) {
    if (!text.empty() && !IsRectEmpty(&rect)) items_.push_back({rect, text});
}
bool OverflowHints::create() {
    if(tooltip_) return true;
    static const bool controls=[] {
        const auto library=LoadLibraryW(L"comctl32.dll");
        if(!library) return false;
        const auto initialize=reinterpret_cast<BOOL (WINAPI*)(const INITCOMMONCONTROLSEX*)>(GetProcAddress(library,"InitCommonControlsEx"));
        const INITCOMMONCONTROLSEX classes{sizeof(classes),ICC_WIN95_CLASSES};
        return initialize && initialize(&classes);
    }();
    if(!controls || !owner_) return false;
    tooltip_=CreateWindowExW(WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT,TOOLTIPS_CLASSW,nullptr,
        WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX|TTS_NOANIMATE|TTS_NOFADE,CW_USEDEFAULT,CW_USEDEFAULT,
        CW_USEDEFAULT,CW_USEDEFAULT,owner_,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!tooltip_) return false;
    font_=create_ui_font(13,FW_NORMAL,dpi_for_window(owner_),100);
    SendMessageW(tooltip_,WM_SETFONT,reinterpret_cast<WPARAM>(font_),FALSE);
    TOOLINFOW info{};info.cbSize=kToolInfoSize;info.hwnd=owner_;info.uId=1;
    info.uFlags=TTF_TRACK|TTF_ABSOLUTE;info.lpszText=const_cast<wchar_t*>(L"");
    if(!SendMessageW(tooltip_,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&info))) {
        DestroyWindow(tooltip_);tooltip_=nullptr;
        if(font_) DeleteObject(font_);font_=nullptr;return false;
    }
    return true;
}
void OverflowHints::mouse_move(POINT point) {
    const auto text=text_at(point);
    if(text!=active_) hide();
    TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE|TME_HOVER,owner_,250};
    TrackMouseEvent(&track);
}
void OverflowHints::hover(POINT point) {
    const auto text=text_at(point);
    if(text.empty() || !create() || !IsWindowVisible(owner_)) { hide();return; }
    active_=text;
    TOOLINFOW info{};info.cbSize=kToolInfoSize;info.hwnd=owner_;info.uId=1;info.lpszText=active_.data();
    SendMessageW(tooltip_,TTM_UPDATETIPTEXTW,0,reinterpret_cast<LPARAM>(&info));
    ClientToScreen(owner_,&point);
    const HMONITOR monitor=MonitorFromPoint(point,MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info{sizeof(monitor_info)};GetMonitorInfoW(monitor,&monitor_info);
    const RECT work=monitor_info.rcWork;
    const int maximum_width=std::max(100L,work.right-work.left-24);
    SendMessageW(tooltip_,TTM_SETMAXTIPWIDTH,0,std::min(maximum_width,scale_by_dpi(560,dpi_for_window(owner_))));
    const HDC measure_dc=CreateCompatibleDC(nullptr);
    const auto previous_font=SelectObject(measure_dc,font_);
    RECT text_size{0,0,std::min(maximum_width,scale_by_dpi(560,dpi_for_window(owner_))),0};
    DrawTextW(measure_dc,active_.c_str(),-1,&text_size,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
    if(text_size.bottom>work.bottom-work.top-48) {
        SendMessageW(tooltip_,TTM_SETMAXTIPWIDTH,0,maximum_width);
        text_size={0,0,maximum_width,0};
        DrawTextW(measure_dc,active_.c_str(),-1,&text_size,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
    }
    SelectObject(measure_dc,previous_font);DeleteDC(measure_dc);
    const int x=std::clamp<int>(point.x+12,work.left+8,std::max<int>(work.left+8,work.right-text_size.right-24));
    const int y=std::clamp<int>(point.y+20,work.top+8,std::max<int>(work.top+8,work.bottom-text_size.bottom-24));
    SendMessageW(tooltip_,TTM_TRACKPOSITION,0,MAKELPARAM(x,y));
    SendMessageW(tooltip_,TTM_TRACKACTIVATE,TRUE,reinterpret_cast<LPARAM>(&info));
}
void OverflowHints::hide() {
    if(tooltip_) {
        TOOLINFOW info{};info.cbSize=kToolInfoSize;info.hwnd=owner_;info.uId=1;
        SendMessageW(tooltip_,TTM_TRACKACTIVATE,FALSE,reinterpret_cast<LPARAM>(&info));
    }
    active_.clear();
}
void OverflowHints::destroy() {
    hide();if(tooltip_) DestroyWindow(tooltip_);tooltip_=nullptr;
    if(font_) DeleteObject(font_);font_=nullptr;
    items_.clear();owner_=nullptr;
    if(g_overflow_hints==this) g_overflow_hints=nullptr;
}

void draw_settings_icon(HDC dc,const RECT& rect,COLORREF color) {
    const int cx=(rect.left+rect.right)/2,cy=(rect.top+rect.bottom)/2;
    const double radius=std::min(rect.right-rect.left,rect.bottom-rect.top)*.34;
    POINT points[52]{};constexpr double pi=3.141592653589793;
    for(int i=0;i<36;++i) {
        const double angle=-pi/2+i*2*pi/36;
        const double r=radius*(i%6==2 || i%6==3?1:.77);
        points[i]={cx+static_cast<LONG>(std::lround(r*std::cos(angle))),cy+static_cast<LONG>(std::lround(r*std::sin(angle)))};
    }
    for(int i=0;i<16;++i) {
        const double angle=i*2*pi/16;
        points[36+i]={cx+static_cast<LONG>(std::lround(radius*.33*std::cos(angle))),cy+static_cast<LONG>(std::lround(radius*.33*std::sin(angle)))};
    }
    const int counts[]={36,16};
    const auto brush=CreateSolidBrush(color);
    const auto old_brush=SelectObject(dc,brush);
    const auto old_pen=SelectObject(dc,GetStockObject(NULL_PEN));
    const int old_mode=SetPolyFillMode(dc,ALTERNATE);PolyPolygon(dc,points,counts,2);SetPolyFillMode(dc,old_mode);
    SelectObject(dc,old_pen);SelectObject(dc,old_brush);DeleteObject(brush);
}

void fill_rect(HDC dc, const RECT& rect, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void fill_round_rect(HDC dc, const RECT& rect, int radius, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    const HGDIOBJ old_brush = SelectObject(dc, brush);
    const HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(brush);
}

void stroke_round_rect(HDC dc, const RECT& rect, int radius, COLORREF color, int thickness)
{
    HPEN pen = CreatePen(PS_SOLID, thickness > 0 ? thickness : 1, color);
    const HGDIOBJ old_pen = SelectObject(dc, pen);
    const HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

bool layered_bitmap_create(LayeredBitmap& surface, int width, int height)
{
    layered_bitmap_destroy(surface);
    if (width <= 0 || height <= 0)
    {
        return false;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // 负高度 = 顶朝下，GDI 坐标与像素数组一致
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr)
    {
        if (bitmap != nullptr)
        {
            DeleteObject(bitmap);
        }
        return false;
    }

    HDC dc = CreateCompatibleDC(nullptr);
    if (dc == nullptr)
    {
        DeleteObject(bitmap);
        return false;
    }

    surface.dc = dc;
    surface.bitmap = bitmap;
    surface.old_bitmap = static_cast<HBITMAP>(SelectObject(dc, bitmap));
    surface.bits = bits;
    surface.width = width;
    surface.height = height;
    layered_bitmap_clear(surface);
    return true;
}

void layered_bitmap_destroy(LayeredBitmap& surface)
{
    if (surface.dc != nullptr)
    {
        if (surface.old_bitmap != nullptr)
        {
            SelectObject(surface.dc, surface.old_bitmap);
        }
        DeleteDC(surface.dc);
    }
    if (surface.bitmap != nullptr)
    {
        DeleteObject(surface.bitmap);
    }
    surface = LayeredBitmap();
}

void layered_bitmap_clear(LayeredBitmap& surface)
{
    if (surface.bits != nullptr)
    {
        std::memset(surface.bits, 0, static_cast<size_t>(surface.width) * static_cast<size_t>(surface.height) * 4u);
    }
}

bool layered_bitmap_present(HWND window, LayeredBitmap& surface, BYTE opacity)
{
    if (window == nullptr || surface.dc == nullptr)
    {
        return false;
    }

    RECT rect;
    if (!GetWindowRect(window, &rect))
    {
        return false;
    }

    HDC screen = GetDC(nullptr);
    if (screen == nullptr)
    {
        return false;
    }

    POINT destination{rect.left, rect.top};
    POINT source{0, 0};
    SIZE size{surface.width, surface.height};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = opacity;
    blend.AlphaFormat = AC_SRC_ALPHA;

    const BOOL ok = UpdateLayeredWindow(window, screen, &destination, &size, surface.dc, &source, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
    return ok != FALSE;
}

void apply_round_rect_alpha(LayeredBitmap& surface, int margin, int radius, int shadow_blur, int shadow_alpha)
{
    if (surface.bits == nullptr || surface.width <= 0 || surface.height <= 0)
    {
        return;
    }

    auto* pixels = static_cast<uint32_t*>(surface.bits);
    const float left = static_cast<float>(margin);
    const float top = static_cast<float>(margin);
    const float right = static_cast<float>(surface.width - margin);
    const float bottom = static_cast<float>(surface.height - margin);
    const float blur = static_cast<float>(shadow_blur > 1 ? shadow_blur : 1);
    const float shadow = clamp_float(static_cast<float>(shadow_alpha) / 255.0f, 0.0f, 1.0f);

    for (int y = 0; y < surface.height; ++y)
    {
        for (int x = 0; x < surface.width; ++x)
        {
            const size_t index = static_cast<size_t>(y) * static_cast<size_t>(surface.width) + static_cast<size_t>(x);
            const uint32_t pixel = pixels[index];
            const float distance = rounded_rect_sdf(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f,
                                                    left, top, right, bottom, static_cast<float>(radius));

            // 边缘 1px 的抗锯齿覆盖率。
            const float coverage = clamp_float(0.5f - distance, 0.0f, 1.0f);
            float shadow_cover = 0.0f;
            if (coverage < 1.0f && distance > 0.0f)
            {
                const float t = clamp_float(1.0f - distance / blur, 0.0f, 1.0f);
                shadow_cover = t * t * shadow;
            }

            const float alpha = coverage + (1.0f - coverage) * shadow_cover;
            if (alpha <= 0.002f)
            {
                pixels[index] = 0;
                continue;
            }

            // 预乘：RGB 乘以形状覆盖率（投影是纯黑，不贡献颜色）。
            const uint32_t r = static_cast<uint32_t>(((pixel >> 16) & 0xFFu) * coverage + 0.5f);
            const uint32_t g = static_cast<uint32_t>(((pixel >> 8) & 0xFFu) * coverage + 0.5f);
            const uint32_t b = static_cast<uint32_t>((pixel & 0xFFu) * coverage + 0.5f);
            const uint32_t a = static_cast<uint32_t>(alpha * 255.0f + 0.5f);
            pixels[index] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

}  // namespace arkhelp

namespace arkhelp
{

HBITMAP load_png_scaled(const std::wstring& path, int width, int height)
{
    if (path.empty() || width <= 0 || height <= 0)
    {
        return nullptr;
    }
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        factory == nullptr)
    {
        return nullptr;
    }

    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    HBITMAP result = nullptr;
    HBITMAP dib = nullptr;
    HDC dc = nullptr;
    HGDIOBJ old = nullptr;
    do
    {
        if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand, &decoder)) ||
            decoder == nullptr)
        {
            break;
        }
        if (FAILED(decoder->GetFrame(0, &frame)) || frame == nullptr)
        {
            break;
        }
        if (FAILED(factory->CreateFormatConverter(&converter)) || converter == nullptr)
        {
            break;
        }
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom)))
        {
            break;
        }
        if (FAILED(factory->CreateBitmapScaler(&scaler)) || scaler == nullptr)
        {
            break;
        }
        // 保持原始宽高比，等比缩放到目标方框内并居中；避免头像被横向拉伸成"长方形"。
        UINT source_width = 0;
        UINT source_height = 0;
        if (FAILED(converter->GetSize(&source_width, &source_height)) || source_width == 0 ||
            source_height == 0)
        {
            break;
        }
        const double scale_x = static_cast<double>(width) / static_cast<double>(source_width);
        const double scale_y = static_cast<double>(height) / static_cast<double>(source_height);
        // 覆盖式缩放：填满目标方框并居中裁掉多余部分，保证所有头像显示为统一的方形。
        const double scale = scale_x > scale_y ? scale_x : scale_y;
        const UINT scaled_width = static_cast<UINT>(source_width * scale + 0.5);
        const UINT scaled_height = static_cast<UINT>(source_height * scale + 0.5);
        if (scaled_width == 0 || scaled_height == 0)
        {
            break;
        }
        if (FAILED(scaler->Initialize(converter, scaled_width, scaled_height,
                                      WICBitmapInterpolationModeFant)))
        {
            break;
        }

        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;  // 顶朝下，和 LayeredBitmap 一致
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        dc = CreateCompatibleDC(nullptr);
        if (dc == nullptr)
        {
            break;
        }
        void* bits = nullptr;
        dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dib == nullptr || bits == nullptr)
        {
            break;
        }
        old = SelectObject(dc, dib);
        const UINT stride = static_cast<UINT>(width) * 4;
        if (FAILED(scaler->CopyPixels(nullptr, stride, stride * static_cast<UINT>(height), static_cast<BYTE*>(bits))))
        {
            break;
        }
        result = dib;
        dib = nullptr;  // 所有权移交给调用方
    } while (false);

    if (old != nullptr && dc != nullptr)
    {
        SelectObject(dc, old);
    }
    if (dib != nullptr)
    {
        DeleteObject(dib);
    }
    if (dc != nullptr)
    {
        DeleteDC(dc);
    }
    if (scaler != nullptr)
    {
        scaler->Release();
    }
    if (converter != nullptr)
    {
        converter->Release();
    }
    if (frame != nullptr)
    {
        frame->Release();
    }
    if (decoder != nullptr)
    {
        decoder->Release();
    }
    factory->Release();
    return result;
}

HBITMAP load_image_memory(const void* data, unsigned long long size, int width, int height)
{
    if (data == nullptr || size == 0 || width <= 0 || height <= 0 || size > 16ull * 1024 * 1024)
    {
        return nullptr;
    }
    IStream* stream = SHCreateMemStream(static_cast<const BYTE*>(data), static_cast<UINT>(size));
    if (stream == nullptr)
    {
        return nullptr;
    }
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    HBITMAP result = nullptr;
    HBITMAP dib = nullptr;
    HDC dc = nullptr;
    HGDIOBJ old = nullptr;
    do
    {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory))) ||
            factory == nullptr)
        {
            break;
        }
        if (FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand,
                                                    &decoder)) ||
            decoder == nullptr)
        {
            break;
        }
        if (FAILED(decoder->GetFrame(0, &frame)) || frame == nullptr)
        {
            break;
        }
        if (FAILED(factory->CreateFormatConverter(&converter)) || converter == nullptr)
        {
            break;
        }
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                         nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        {
            break;
        }
        if (FAILED(factory->CreateBitmapScaler(&scaler)) || scaler == nullptr)
        {
            break;
        }
        // 保持原始宽高比，等比缩放到目标方框内并居中；避免头像被横向拉伸成"长方形"。
        UINT source_width = 0;
        UINT source_height = 0;
        if (FAILED(converter->GetSize(&source_width, &source_height)) || source_width == 0 ||
            source_height == 0)
        {
            break;
        }
        const double scale_x = static_cast<double>(width) / static_cast<double>(source_width);
        const double scale_y = static_cast<double>(height) / static_cast<double>(source_height);
        // 覆盖式缩放：填满目标方框并居中裁掉多余部分，保证所有头像显示为统一的方形。
        const double scale = scale_x > scale_y ? scale_x : scale_y;
        const UINT scaled_width = static_cast<UINT>(source_width * scale + 0.5);
        const UINT scaled_height = static_cast<UINT>(source_height * scale + 0.5);
        if (scaled_width == 0 || scaled_height == 0)
        {
            break;
        }
        if (FAILED(scaler->Initialize(converter, scaled_width, scaled_height,
                                      WICBitmapInterpolationModeFant)))
        {
            break;
        }
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(nullptr);
        if (dc == nullptr)
        {
            break;
        }
        void* bits = nullptr;
        dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dib == nullptr || bits == nullptr)
        {
            break;
        }
        old = SelectObject(dc, dib);
        std::vector<BYTE> scaled(scaled_width * scaled_height * 4);
        if (FAILED(scaler->CopyPixels(nullptr, scaled_width * 4,
                                      scaled_width * scaled_height * 4, scaled.data())))
        {
            break;
        }
        const int offset_x = (width - static_cast<int>(scaled_width)) / 2;
        const int offset_y = (height - static_cast<int>(scaled_height)) / 2;
        auto* destination = static_cast<BYTE*>(bits);
        const int copy_left = offset_x > 0 ? offset_x : 0;
        const int copy_top = offset_y > 0 ? offset_y : 0;
        const int copy_right = (offset_x + static_cast<int>(scaled_width)) < width
                                   ? offset_x + static_cast<int>(scaled_width)
                                   : width;
        const int copy_bottom = (offset_y + static_cast<int>(scaled_height)) < height
                                    ? offset_y + static_cast<int>(scaled_height)
                                    : height;
        for (int y = copy_top; y < copy_bottom; ++y)
        {
            const int source_y = y - offset_y;
            const BYTE* source_row = scaled.data() +
                                     static_cast<std::size_t>(source_y) * scaled_width * 4 +
                                     static_cast<std::size_t>(copy_left - offset_x) * 4;
            BYTE* destination_row = destination + static_cast<std::size_t>(y) * width * 4 +
                                    static_cast<std::size_t>(copy_left) * 4;
            std::memcpy(destination_row, source_row,
                        static_cast<std::size_t>(copy_right - copy_left) * 4);
        }
        result = dib;
        dib = nullptr;
    } while (false);
    if (old != nullptr && dc != nullptr)
    {
        SelectObject(dc, old);
    }
    if (dib != nullptr)
    {
        DeleteObject(dib);
    }
    if (dc != nullptr)
    {
        DeleteDC(dc);
    }
    if (scaler != nullptr)
    {
        scaler->Release();
    }
    if (converter != nullptr)
    {
        converter->Release();
    }
    if (frame != nullptr)
    {
        frame->Release();
    }
    if (decoder != nullptr)
    {
        decoder->Release();
    }
    if (factory != nullptr)
    {
        factory->Release();
    }
    stream->Release();
    return result;
}

void destroy_bitmap(HBITMAP bitmap)
{
    if (bitmap != nullptr)
    {
        DeleteObject(bitmap);
    }
}

}  // namespace arkhelp
