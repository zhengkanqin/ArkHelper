// Include the production renderer so pixel assertions exercise the actual ball, without
// adding a capture/export API to the product.
#include "../src/ui/ball.cpp"
#include "ui_review_bitmap.h"
#include <objbase.h>
#include <cstdio>
#include <vector>

namespace arkhelp { void menu_follow(int, int) {} } // Drag target is outside this pixel-only harness.

namespace {
std::vector<std::uint32_t> verify_logo_pixels() {
    using namespace arkhelp;
    DIBSECTION logo{};
    if (g_ball.logo == nullptr || GetObjectW(g_ball.logo, sizeof(logo), &logo) != sizeof(logo) ||
        logo.dsBm.bmBits == nullptr) throw std::runtime_error("original ArkHelper PNG was not loaded");
    const auto* artwork = static_cast<const std::uint32_t*>(logo.dsBm.bmBits);
    const auto* pixels = static_cast<const std::uint32_t*>(g_ball.surface.bits);
    const int size = g_ball.surface.width;
    std::vector<std::uint32_t> baseline(pixels, pixels + size * size);
    unsigned white = 0, black = 0, transparent = 0, edge = 0, light = 0, outside_light = 0;
    const COLORREF status = g_ball.link == LinkState::kBattle ? theme::kStatusGreen :
                            g_ball.link == LinkState::kWaiting ? theme::kStatusYellow : theme::kStatusGray;
    const std::uint32_t status_rgb = (GetRValue(status) << 16) | (GetGValue(status) << 8) | GetBValue(status);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const auto index = y * size + x;
            const auto value = baseline[index];
            const auto original = artwork[index];
            const unsigned alpha = value >> 24, original_alpha = original >> 24;
            const float dx = x + .5f - size * .5f, dy = y + .5f - size * .5f;
            const float distance = std::sqrt(dx * dx + dy * dy);
            const float radius = size * kBallRadius / kBallLogicalSize;
            const float sx = x + .5f - size * kStatusCenterLogical / kBallLogicalSize;
            const float sy = y + .5f - size * kStatusCenterLogical / kBallLogicalSize;
            const float status_distance = std::sqrt(sx * sx + sy * sy);
            const float status_radius = size * kStatusRadiusLogical / kBallLogicalSize;
            const bool in_light = status_distance <= status_radius + .5f;
            if (status_distance < status_radius - .5f) {
                ++light;
                if (distance > radius) ++outside_light;
                if (value != (0xff000000u | status_rgb)) throw std::runtime_error("status light is not a flat solid state colour");
            } else if (in_light) {
                for (const int shift : {0, 8, 16}) {
                    const auto channel = (value >> shift) & 255u;
                    const auto circle_mask = static_cast<unsigned>(std::clamp(radius + .5f - distance, 0.f, 1.f) * 255.f + .5f);
                    const auto a = (((original >> shift) & 255u) * circle_mask + 127) / 255;
                    const auto b = (status_rgb >> shift) & 255u;
                    const auto light_mask = static_cast<unsigned>(std::clamp(status_radius + .5f - status_distance, 0.f, 1.f) * 255.f + .5f);
                    const auto expected = (b * light_mask + a * (255 - light_mask) + 127) / 255;
                    if (channel != expected)
                        throw std::runtime_error("status light has a separate border or shadow");
                }
            }
            if (distance > radius + .5f && !in_light && value != 0)
                throw std::runtime_error("ball paints outside its circular silhouette");
            if (!alpha) {
                ++transparent;
                if (value) throw std::runtime_error("transparent ball edge has color residue");
                continue;
            }
            if (((value >> 16) & 255u) > alpha || ((value >> 8) & 255u) > alpha || (value & 255u) > alpha)
                throw std::runtime_error("ball edge is not premultiplied");
            if (!in_light && distance < radius - .5f && value != original)
                throw std::runtime_error("the original Logo was replaced or decorated inside the circle");
            if (alpha == 255) {
                if (!in_light && value != original) throw std::runtime_error("ball adds an outline or changes artwork outside its status light");
                if (((value >> 16) & 255u) >= 200 && ((value >> 8) & 255u) >= 200 && (value & 255u) >= 200) ++white;
                if (((value >> 16) & 255u) < 32 && ((value >> 8) & 255u) < 32 && (value & 255u) < 32) ++black;
            } else {
                ++edge;
                if (in_light) continue;
                if (!original_alpha) throw std::runtime_error("ball invented coverage outside the source artwork");
                // Masked edge colours must remain proportional to the real PNG pixel. This
                // catches a separately drawn blue rim, a black halo and double-premultiplication.
                for (const int shift : {0, 8, 16}) {
                    const auto channel = (value >> shift) & 255u;
                    const auto expected = (((original >> shift) & 255u) * alpha + original_alpha / 2) / original_alpha;
                    if (std::abs(static_cast<int>(channel) - static_cast<int>(expected)) > 1)
                        throw std::runtime_error("ball edge changes the Logo colour or adds an independent outline");
                }
            }
        }
    }
    if (white < static_cast<unsigned>(size * size / 50) || black < static_cast<unsigned>(size * size / 4) || !transparent || !edge || !light)
        throw std::runtime_error("ball lacks the original white Logo, black background or transparent antialiased edge");
    if (outside_light < light / 4 || outside_light > light * 3 / 4)
        throw std::runtime_error("status light must straddle the main circle rather than being enclosed by it");
    return baseline;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    using namespace arkhelp;
    const auto destination = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(L"build/ui-review-current");
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        if (FAILED(com)) throw std::runtime_error("COM initialization failed");
        Config config;
        config.ball_x = config.ball_y = 100;
        config.opacity = 100;
        if (!ball_create(GetModuleHandleW(nullptr), {}, config)) throw std::runtime_error("ball creation failed");
        const auto baseline = verify_logo_pixels();
        const auto logo_handle = g_ball.logo;
        const int size = g_ball.surface.width;
        for (const auto state : {LinkState::kDisconnected, LinkState::kWaiting, LinkState::kBattle}) {
            ball_set_link_state(state); // Must redraw immediately without another render call.
            const auto state_pixels = verify_logo_pixels();
            if (g_ball.logo != logo_handle) throw std::runtime_error("state change reloaded the cached Logo");
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    const float dx = x + .5f - size * kStatusCenterLogical / kBallLogicalSize;
                    const float dy = y + .5f - size * kStatusCenterLogical / kBallLogicalSize;
                    if (std::sqrt(dx * dx + dy * dy) > size * kStatusRadiusLogical / kBallLogicalSize + .5f &&
                        state_pixels[y * size + x] != baseline[y * size + x])
                        throw std::runtime_error("connection state changed pixels outside the status light");
                }
            }
            SendMessageW(ball_hwnd(), WM_MOUSEMOVE, 0, MAKELPARAM(10, 10));
            render_ball();
            const auto* actual = static_cast<const std::uint32_t*>(g_ball.surface.bits);
            if (!std::equal(state_pixels.begin(), state_pixels.end(), actual)) throw std::runtime_error("hover decorated the Logo or light");
            const auto filename = state == LinkState::kBattle ? L"ball-battle.bmp" :
                state == LinkState::kWaiting ? L"ball-waiting.bmp" : L"ball-disconnected.bmp";
            save_review_bitmap(g_ball.surface, destination / filename);
        }
        save_review_bitmap(g_ball.surface, destination / L"ball-logo.bmp");
        for (const int scale : {80, 150, 100}) {
            ball_set_scale(scale);
            verify_logo_pixels();
        }
        ball_destroy();
        CoUninitialize();
        std::puts("PASS production ball pixels: original Logo, gray/yellow/green flat status light, transparent exterior, no rim or shadow; scales 80/100/150");
    } catch (const std::exception& error) {
        ball_destroy();
        if (SUCCEEDED(com)) CoUninitialize();
        std::fprintf(stderr, "FAIL %s\n", error.what()); return 1;
    }
    return 0;
}
