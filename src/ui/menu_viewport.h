#pragma once

#include <algorithm>

namespace arkhelp::menu_layout
{

// Drawing and hit testing use the same translation. The header/footer stay in
// window coordinates; only a point inside this viewport may enter page content.
struct Viewport
{
    int top = 0;
    int bottom = 0;
    int content_height = 0;
    int scroll = 0;

    int height() const { return std::max(0, bottom - top); }
    int maximum_scroll() const { return std::max(0, content_height - height()); }
    void clamp() { scroll = std::clamp(scroll, 0, maximum_scroll()); }
    void move(int pixels) { scroll += pixels; clamp(); }
    bool contains(int y) const { return y >= top && y < bottom; }
    int content_y(int window_y) const { return window_y + scroll; }
};

inline int bounded_content_height(int natural_height, int maximum_height, int work_height,
                                  int shadow, int edge)
{
    return std::max(1, std::min({natural_height, maximum_height,
                                std::max(1, work_height - 2 * shadow - 2 * edge)}));
}

}  // namespace arkhelp::menu_layout
