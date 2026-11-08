// Inspect the actual Direct2D raster output, without showing a window.
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <cstdint>
#define private public
#include "../src/gfx/gfx.h"
#undef private
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <stdexcept>

namespace {
void check(bool ok,const char* text) { if(!ok) throw std::runtime_error(text); }
}
int wmain(int argc,wchar_t** argv) {
    using namespace arkhelp::gfx;
    try {
        Surface surface;
        check(surface.initialize() && surface.resize(320,150),"Direct2D surface unavailable");
        surface.begin_frame();
        for(int style=0;style<3;++style) surface.draw_line(10,20+style*25,300,20+style*25,{1,1,1,1},2,style);
        const auto dash=surface.stroke_styles_[0],dot=surface.stroke_styles_[1];
        surface.draw_line(10,100,300,100,{1,1,1,1},8,1);
        const D2D1_POINT_2F polygon[]={{10,125},{300,125},{300,145},{10,145}};
        surface.draw_polygons(polygon,1,4,{0,0,0,0},{1,1,1,1},2,1);
        surface.end_frame();
        check(dash && dot && dash==surface.stroke_styles_[0] && dot==surface.stroke_styles_[1],
            "stroke resources were not cached by style");
        auto* pixels=static_cast<const std::uint32_t*>(surface.bits_);
        const auto runs=[&](int y) {
            int count=0;bool before=false;
            for(int x=12;x<298;++x) { const bool painted=(pixels[y*320+x]>>24)>64;
                if(painted && !before) ++count;before=painted; }
            return count;
        };
        check(runs(20)==1 && runs(45)>10 && runs(70)>runs(45),"solid/dash/dot raster patterns are not distinct");
        check(runs(125)>10,"polygon outlines did not use the configured dash style");
        const auto thickness = [&](int x, int y) {
            int count = 0;
            for (int row = y - 6; row <= y + 6; ++row) {
                if ((pixels[row * 320 + x] >> 24) > 64) ++count;
            }
            return count;
        };
        check(thickness(11,100)>=7 && thickness(11,20)<=3,"configured stroke thickness was not rasterized");
        if(argc>1) {
            BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);
            file.bfSize=file.bfOffBits+320*150*4;
            BITMAPINFOHEADER header{};header.biSize=sizeof(header);header.biWidth=320;header.biHeight=-150;
            header.biPlanes=1;header.biBitCount=32;header.biCompression=BI_RGB;header.biSizeImage=320*150*4;
            std::ofstream output(std::filesystem::path(argv[1]),std::ios::binary);
            output.write(reinterpret_cast<const char*>(&file),sizeof(file));
            output.write(reinterpret_cast<const char*>(&header),sizeof(header));
            output.write(reinterpret_cast<const char*>(pixels),header.biSizeImage);
        }
        surface.shutdown();
        check(!surface.stroke_styles_[0] && !surface.stroke_styles_[1],"shutdown leaked stroke resources");
        std::puts("PASS actual Direct2D solid/dash/dot pixels, thickness, compound outlines and cached stroke lifetime");
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL %s\n",e.what());return 1; }
}
