#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include "../src/shell/win_util.h"

// Development harness only. Capture the real layered surface against a white background;
// no screenshot/debug export code is linked into the application.
inline void save_review_bitmap(const arkhelp::LayeredBitmap& surface, const std::filesystem::path& path) {
    if (!surface.bits || surface.width <= 0 || surface.height <= 0) throw std::runtime_error("empty UI surface");
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = surface.width;
    info.biHeight = -surface.height;
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biSizeImage = static_cast<DWORD>(surface.width * surface.height * 4);
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + info.biSizeImage;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    const auto* pixels = static_cast<const std::uint32_t*>(surface.bits);
    for (int index = 0; index < surface.width * surface.height; ++index) {
        const auto value = pixels[index];
        const auto background = 255u - (value >> 24);
        const std::uint32_t composed = 0xff000000u |
            (std::min(255u, ((value >> 16) & 255u) + background) << 16) |
            (std::min(255u, ((value >> 8) & 255u) + background) << 8) |
            std::min(255u, (value & 255u) + background);
        output.write(reinterpret_cast<const char*>(&composed), sizeof(composed));
    }
    if (!output) throw std::runtime_error("could not save UI review bitmap");
}
