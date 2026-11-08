#include "gfx.h"

#include <cmath>
#include <cstring>
#include <algorithm>

namespace arkhelp::gfx {
namespace {

// Interface ids are spelled out locally: the MinGW headers declare the interfaces but do not always
// export the matching IID symbols, and the C entry points accept a REFIID directly.
constexpr GUID kD2D1FactoryIid = {0x06152247,
                                  0x6f50,
                                  0x465a,
                                  {0x92, 0x45, 0x11, 0x8b, 0xfd, 0x3b, 0x60, 0x07}};
constexpr GUID kDWriteFactoryIid = {0xb859ee5a,
                                    0xd838,
                                    0x4b5b,
                                    {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}};

D2D1_COLOR_F to_d2d_color(const Color& color) {
    D2D1_COLOR_F result = {};
    result.r = color.r;
    result.g = color.g;
    result.b = color.b;
    result.a = color.a;
    return result;
}

D2D1_ROUNDED_RECT to_rounded_rect(float left, float top, float right, float bottom, float radius) {
    D2D1_ROUNDED_RECT result = {};
    result.rect.left = left;
    result.rect.top = top;
    result.rect.right = right;
    result.rect.bottom = bottom;
    result.radiusX = radius;
    result.radiusY = radius;
    return result;
}

}  // namespace

Surface::~Surface() {
    shutdown();
}

bool Surface::initialize() {
    if (factory_ != nullptr) {
        return ready();
    }
    const HRESULT factory_result = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED, kD2D1FactoryIid, nullptr,
        reinterpret_cast<void**>(&factory_));
    if (FAILED(factory_result) || factory_ == nullptr) {
        factory_ = nullptr;
        return false;
    }
    const HRESULT write_result = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, kDWriteFactoryIid,
        reinterpret_cast<IUnknown**>(&write_factory_));
    if (FAILED(write_result) || write_factory_ == nullptr) {
        release_factories();
        return false;
    }
    return create_target();
}

void Surface::shutdown() {
    release_bitmap();
    release_target();
    release_text_formats();
    release_factories();
}

bool Surface::ready() const {
    return factory_ != nullptr && target_ != nullptr && write_factory_ != nullptr;
}

void Surface::release_bitmap() {
    if (memory_dc_ != nullptr) {
        if (previous_bitmap_ != nullptr) {
            SelectObject(memory_dc_, previous_bitmap_);
            previous_bitmap_ = nullptr;
        }
        if (bitmap_ != nullptr) {
            DeleteObject(static_cast<HBITMAP>(bitmap_));
            bitmap_ = nullptr;
        }
        DeleteDC(memory_dc_);
        memory_dc_ = nullptr;
    }
    bits_ = nullptr;
    width_ = 0;
    height_ = 0;
}

void Surface::release_target() {
    if (brush_ != nullptr) {
        brush_->Release();
        brush_ = nullptr;
    }
    if (target_ != nullptr) {
        target_->Release();
        target_ = nullptr;
    }
    in_frame_ = false;
}

void Surface::release_text_formats() {
    for (TextFormatEntry& entry : formats_) {
        if (entry.format != nullptr) {
            entry.format->Release();
            entry.format = nullptr;
        }
    }
    formats_.clear();
}

void Surface::release_factories() {
    for(auto& style:stroke_styles_) { if(style) style->Release(); style=nullptr; }
    if (write_factory_ != nullptr) {
        write_factory_->Release();
        write_factory_ = nullptr;
    }
    if (factory_ != nullptr) {
        factory_->Release();
        factory_ = nullptr;
    }
}

bool Surface::create_target() {
    release_target();
    if (factory_ == nullptr) {
        return false;
    }
    D2D1_RENDER_TARGET_PROPERTIES properties = {};
    properties.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    properties.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    properties.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    properties.dpiX = 0.0f;
    properties.dpiY = 0.0f;
    properties.usage = D2D1_RENDER_TARGET_USAGE_NONE;
    properties.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;
    HRESULT result = factory_->CreateDCRenderTarget(&properties, &target_);
    if (FAILED(result) || target_ == nullptr) {
        target_ = nullptr;
        properties.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
        result = factory_->CreateDCRenderTarget(&properties, &target_);
        if (FAILED(result) || target_ == nullptr) {
            target_ = nullptr;
            return false;
        }
    }
    ensure_brush();
    return brush_ != nullptr;
}

void Surface::ensure_brush() {
    if (brush_ != nullptr || target_ == nullptr) {
        return;
    }
    const D2D1_COLOR_F white = to_d2d_color(Color{});
    if (FAILED(target_->CreateSolidColorBrush(white, &brush_))) {
        brush_ = nullptr;
    }
}

bool Surface::resize(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (width == width_ && height == height_ && bitmap_ != nullptr) {
        return true;
    }
    release_bitmap();
    HDC memory_dc = CreateCompatibleDC(nullptr);
    if (memory_dc == nullptr) {
        return false;
    }
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // top-down rows
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(memory_dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) {
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
        DeleteDC(memory_dc);
        return false;
    }
    HGDIOBJ previous = SelectObject(memory_dc, bitmap);
    std::memset(bits, 0, static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
    memory_dc_ = memory_dc;
    bitmap_ = bitmap;
    previous_bitmap_ = previous;
    bits_ = bits;
    width_ = width;
    height_ = height;
    return true;
}

void Surface::begin_frame() {
    if (!ready() || memory_dc_ == nullptr || width_ <= 0 || height_ <= 0) {
        return;
    }
    RECT rect = {};
    rect.right = width_;
    rect.bottom = height_;
    if (FAILED(target_->BindDC(memory_dc_, &rect))) {
        return;
    }
    target_->BeginDraw();
    const D2D1_COLOR_F transparent = to_d2d_color(Color{0.0f, 0.0f, 0.0f, 0.0f});
    target_->Clear(transparent);
    in_frame_ = true;
}

void Surface::end_frame() {
    if (!in_frame_ || target_ == nullptr) {
        return;
    }
    const HRESULT result = target_->EndDraw();
    in_frame_ = false;
    if (result == D2DERR_RECREATE_TARGET) {
        release_target();
        create_target();
    }
}

bool Surface::present(HWND window, int x, int y) {
    if (window == nullptr || memory_dc_ == nullptr || bitmap_ == nullptr || width_ <= 0 ||
        height_ <= 0) {
        return false;
    }
    SIZE size = {};
    size.cx = width_;
    size.cy = height_;
    POINT destination = {};
    destination.x = x;
    destination.y = y;
    POINT source = {};
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    return UpdateLayeredWindow(window, nullptr, &destination, &size, memory_dc_, &source, 0, &blend,
                               ULW_ALPHA) != FALSE;
}

void Surface::set_style(float opacity, std::int32_t text_color) {
    opacity_ = std::clamp(opacity, 0.0f, 1.0f);
    text_color_ = text_color;
}

Color Surface::styled_color(const Color& color, bool text) const {
    Color result = color;
    if (text && text_color_ >= 0 && text_color_ <= 0xffffff) {
        result.r = static_cast<float>((text_color_ >> 16) & 255) / 255.0f;
        result.g = static_cast<float>((text_color_ >> 8) & 255) / 255.0f;
        result.b = static_cast<float>(text_color_ & 255) / 255.0f;
    }
    result.a *= opacity_;
    return result;
}

void Surface::fill_rounded_rect(float left, float top, float right, float bottom, float radius,
                                const Color& color) {
    if (!in_frame_ || target_ == nullptr) {
        return;
    }
    ensure_brush();
    if (brush_ == nullptr) {
        return;
    }
    brush_->SetColor(to_d2d_color(styled_color(color)));
    const D2D1_ROUNDED_RECT rounded = to_rounded_rect(left, top, right, bottom, radius);
    target_->FillRoundedRectangle(&rounded, brush_);
}

void Surface::stroke_rounded_rect(float left, float top, float right, float bottom, float radius,
                                  const Color& color) {
    if (!in_frame_ || target_ == nullptr) {
        return;
    }
    ensure_brush();
    if (brush_ == nullptr) {
        return;
    }
    brush_->SetColor(to_d2d_color(styled_color(color)));
    const D2D1_ROUNDED_RECT rounded = to_rounded_rect(left, top, right, bottom, radius);
    target_->DrawRoundedRectangle(&rounded, brush_, 1.0f);
}

ID2D1StrokeStyle* Surface::stroke_style(int style) {
    if(style<1 || style>2 || !factory_) return nullptr;
    auto& result=stroke_styles_[style-1];
    if(!result) {
        auto properties=D2D1::StrokeStyleProperties();
        properties.dashStyle=style==1?D2D1_DASH_STYLE_DASH:D2D1_DASH_STYLE_DOT;
        properties.dashCap=style==2?D2D1_CAP_STYLE_ROUND:D2D1_CAP_STYLE_FLAT;
        factory_->CreateStrokeStyle(properties,nullptr,0,&result);
    }
    return result;
}

void Surface::draw_line(float x1,float y1,float x2,float y2,const Color& color,float width,int style) {
    if(!in_frame_ || !target_ || !std::isfinite(x1) || !std::isfinite(y1) ||
        !std::isfinite(x2) || !std::isfinite(y2) || width<=0) return;
    ensure_brush();
    if(!brush_) return;
    brush_->SetColor(to_d2d_color(styled_color(color)));
    target_->DrawLine(D2D1::Point2F(x1,y1),D2D1::Point2F(x2,y2),brush_,width,stroke_style(style));
}

void Surface::draw_polygons(const D2D1_POINT_2F* points, std::uint32_t polygon_count,
                            std::uint32_t points_per_polygon, const Color& fill,
                            const Color& outline, float outline_width, int style) {
    if (!in_frame_ || !target_ || !factory_ || !points || polygon_count == 0 ||
        polygon_count > 64 || points_per_polygon < 3 || points_per_polygon > 32) { return; }
    ensure_brush();
    if (!brush_) { return; }
    ID2D1PathGeometry* geometry = nullptr;
    if (FAILED(factory_->CreatePathGeometry(&geometry)) || !geometry) { return; }
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(geometry->Open(&sink)) || !sink) { geometry->Release(); return; }
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    for (std::uint32_t index = 0; index < polygon_count; ++index) {
        const auto* polygon = points + index * points_per_polygon;
        sink->BeginFigure(polygon[0], D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLines(polygon + 1, points_per_polygon - 1);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    }
    const HRESULT result = sink->Close();
    sink->Release();
    if (SUCCEEDED(result)) {
        if (fill.a > 0) {
            brush_->SetColor(to_d2d_color(styled_color(fill)));
            target_->FillGeometry(geometry, brush_, nullptr);
        }
        if (outline.a > 0 && outline_width > 0) {
            brush_->SetColor(to_d2d_color(styled_color(outline)));
            target_->DrawGeometry(geometry, brush_, outline_width, stroke_style(style));
        }
    }
    geometry->Release();
}

IDWriteTextFormat* Surface::text_format(float size, TextAlign align) {
    for (TextFormatEntry& entry : formats_) {
        if (std::fabs(entry.size - size) < 0.01f && entry.align == align) {
            return entry.format;
        }
    }
    if (write_factory_ == nullptr || size <= 0.0f) {
        return nullptr;
    }
    const wchar_t* families[] = {L"Microsoft YaHei UI", L"Microsoft YaHei", L"Segoe UI",
                                 L"Arial"};
    IDWriteTextFormat* format = nullptr;
    for (const wchar_t* family : families) {
        const HRESULT result = write_factory_->CreateTextFormat(
            family, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &format);
        if (SUCCEEDED(result) && format != nullptr) {
            break;
        }
        format = nullptr;
    }
    if (format == nullptr) {
        return nullptr;
    }
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetTextAlignment(align == TextAlign::kCenter ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                         : DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    TextFormatEntry entry = {};
    entry.size = size;
    entry.align = align;
    entry.format = format;
    formats_.push_back(entry);
    return format;
}

void Surface::draw_text(const std::wstring& text, float x, float top, float size,
                        const Color& color, TextAlign align, bool outline) {
    if (!in_frame_ || target_ == nullptr || text.empty() || size <= 0.0f) {
        return;
    }
    IDWriteTextFormat* format = text_format(size, align);
    if (format == nullptr) {
        return;
    }
    ensure_brush();
    if (brush_ == nullptr) {
        return;
    }
    const float box_height = size * 1.6f;
    const float half_width = 512.0f;
    D2D1_RECT_F rect = {};
    if (align == TextAlign::kCenter) {
        rect.left = x - half_width;
        rect.right = x + half_width;
    } else {
        rect.left = x;
        rect.right = x + half_width * 2.0f;
    }
    rect.top = top;
    rect.bottom = top + box_height;
    const std::uint32_t length = static_cast<std::uint32_t>(text.size());
    if (outline) {
        const D2D1_COLOR_F outline_color = to_d2d_color(styled_color(Color{0.03f, 0.04f, 0.06f, 0.93f}));
        brush_->SetColor(outline_color);
        const float offsets[4][2] = {{1.0f, 0.0f}, {-1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, -1.0f}};
        for (const auto& offset : offsets) {
            D2D1_RECT_F shifted = rect;
            shifted.left += offset[0];
            shifted.right += offset[0];
            shifted.top += offset[1];
            shifted.bottom += offset[1];
            target_->DrawText(text.c_str(), length, format, &shifted, brush_,
                              D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
        }
    }
    brush_->SetColor(to_d2d_color(styled_color(color, true)));
    target_->DrawText(text.c_str(), length, format, &rect, brush_, D2D1_DRAW_TEXT_OPTIONS_NONE,
                      DWRITE_MEASURING_MODE_NATURAL);
}

}  // namespace arkhelp::gfx
