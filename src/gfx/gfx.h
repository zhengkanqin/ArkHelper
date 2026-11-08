// Premultiplied 32bpp Direct2D surface presented through UpdateLayeredWindow.
//
// The overlay window is layered, so the only way to get per-pixel alpha is to draw into a DIB
// section and hand it to UpdateLayeredWindow. Direct2D draws into that DIB through an
// ID2D1DCRenderTarget, DirectWrite renders the glyphs, and the caller only pushes pixels when the
// scene actually changed. All coordinates are physical pixels in the surface.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>

#include <string>
#include <cstdint>
#include <vector>

namespace arkhelp::gfx {

struct Color {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

enum class TextAlign {
    kLeading,
    kCenter,
};

class Surface {
public:
    Surface() = default;
    ~Surface();

    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    // Creates the Direct2D/DirectWrite factories and the DC render target. Safe to call twice.
    bool initialize();
    void shutdown();
    bool ready() const;

    // (Re)creates the backing DIB. Returns false when the bitmap allocation failed.
    bool resize(int width, int height);
    int width() const { return width_; }
    int height() const { return height_; }

    void begin_frame();
    void end_frame();
    // Independent opacity and optional RGB text override for the next feature's primitives.
    void set_style(float opacity, std::int32_t text_color = -1);
    // Copies the current pixels to the layered window at screen position (x, y).
    bool present(HWND window, int x, int y);

    void fill_rounded_rect(float left, float top, float right, float bottom, float radius,
                           const Color& color);
    void stroke_rounded_rect(float left, float top, float right, float bottom, float radius,
                             const Color& color);
    void draw_line(float x1,float y1,float x2,float y2,const Color& color,float width,int style = 0);

    // A single compound geometry keeps many projected range contours cheap to present.
    // Each polygon contains the same number of points; overlaps do not accumulate fill opacity.
    void draw_polygons(const D2D1_POINT_2F* points, std::uint32_t polygon_count,
                        std::uint32_t points_per_polygon, const Color& fill,
                        const Color& outline, float outline_width, int style = 0);

    // Draws one line of text. `x` is the left edge for kLeading and the horizontal center for
    // kCenter; `top` is the top of the text box. `outline` adds the 1px dark halo used by the
    // in-world numbers.
    void draw_text(const std::wstring& text, float x, float top, float size, const Color& color,
                   TextAlign align, bool outline);

private:
    struct TextFormatEntry {
        float size = 0.0f;
        TextAlign align = TextAlign::kLeading;
        IDWriteTextFormat* format = nullptr;
    };

    void release_bitmap();
    void release_text_formats();
    void release_target();
    void release_factories();
    bool create_target();
    void ensure_brush();
    IDWriteTextFormat* text_format(float size, TextAlign align);
    Color styled_color(const Color& color, bool text = false) const;
    ID2D1StrokeStyle* stroke_style(int style);

    ID2D1Factory* factory_ = nullptr;
    ID2D1StrokeStyle* stroke_styles_[2]{};
    ID2D1DCRenderTarget* target_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    IDWriteFactory* write_factory_ = nullptr;
    HDC memory_dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_bitmap_ = nullptr;
    void* bits_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    bool in_frame_ = false;
    float opacity_ = 1.0f;
    std::int32_t text_color_ = -1;
    std::vector<TextFormatEntry> formats_;
};

}  // namespace arkhelp::gfx
