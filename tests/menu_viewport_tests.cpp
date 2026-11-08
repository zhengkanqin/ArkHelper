#include "../src/ui/menu_viewport.h"

#include <cstdlib>
#include <iostream>

using arkhelp::menu_layout::Viewport;

void require(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

int main()
{
    Viewport page{64, 326, 524, 0};
    page.move(108);
    const int metric_window_y = 64 + 48 + 4 * 30 - page.scroll;
    require(page.contains(metric_window_y), "visible metric is inside content viewport");
    require(page.content_y(metric_window_y) == 64 + 48 + 4 * 30,
            "scrolled metric hit returns exactly its drawn row");
    require(!page.contains(63) && !page.contains(326), "fixed title and footer never hit page controls");
    page.move(10000);
    require(page.scroll == 262, "wheel stops at content end");
    page.bottom = 600;
    page.clamp();
    require(page.scroll == 0, "resize removes obsolete scroll when content fits");

    using arkhelp::menu_layout::bounded_content_height;
    require(bounded_content_height(436,440,1040,12,8) == 436, "main menu follows natural height");
    require(bounded_content_height(210,440,1040,12,8) == 210, "one setting page stays compact");
    require(bounded_content_height(638,440,1040,12,8) == 440, "long table uses capped viewport");
    require(bounded_content_height(957,660,600,18,12) == 540,
            "scaled menu plus shadow and edge fit short monitor work area");
    std::cout << "menu viewport: 4 cases passed\n";
}
