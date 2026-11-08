#include "../src/overlay/visibility.h"
#include "../src/features/range_contour.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace ipc = arkhelp::ipc;
namespace {
int checks = 0, failures = 0;
void check(bool passed, const char* label) {
    ++checks;
    if (!passed) { ++failures; std::printf("FAIL %s\n", label); }
}
ipc::UnitEntry ranged_unit() {
    ipc::UnitEntry unit;
    unit.flags = ipc::kUnitValid | ipc::kUnitRangeValid | ipc::kUnitGroundValid;
    unit.tracking_id = 1;
    unit.x = unit.ground_x = 300;
    unit.y = unit.ground_y = 200;
    unit.hp = -1; // A range does not require a readable HP property.
    unit.range_radius = 2;
    unit.range_point_count = ipc::kRangePointCount;
    for (std::uint32_t i = 0; i < ipc::kRangePointCount; ++i) {
        const double angle = i * 6.283185307179586 / ipc::kRangePointCount;
        unit.range_points[i] = {300 + 100 * static_cast<float>(std::cos(angle)),
                                200 + 50 * static_cast<float>(std::sin(angle))};
    }
    return unit;
}
bool contour(const ipc::UnitEntry& unit) {
    ipc::Float2 output[ipc::kRangePointCount];
    return arkhelp::features::prepare_range_contour(unit, 1280, 720, output);
}
} // namespace

int main() {
    ipc::BattleSnapshot snapshot;
    snapshot.in_battle = 1; snapshot.battle_state = 2; snapshot.published_ms = 1000;
    const auto visible = [&](bool have = true, bool shown = true, bool minimized = false,
                              bool foreground = true, int width = 1280, int height = 720) {
        return arkhelp::overlay::can_draw_live(snapshot, have, shown, minimized, foreground,
                                              width, height, 1100);
    };
    check(visible(), "live battle in the foreground displays its marks");
    snapshot.flags = ipc::kBattlePaused;
    check(visible(), "paused live battle remains visible");
    snapshot.flags = 0;
    check(!visible(false), "missing snapshot hides every game overlay layer");
    check(!visible(true, false), "hidden game window hides every layer");
    check(!visible(true, true, true), "minimized game hides every layer");
    check(!visible(true, true, false, false), "another foreground app hides every layer");
    check(!visible(true, true, false, true, 20), "invalid game client extent hides every layer");
    snapshot.in_battle = 0;
    check(!visible(), "retained report outside a battle cannot draw world marks");
    snapshot.in_battle = 1; snapshot.battle_state = 3;
    check(!visible(), "battle finish wins even if in_battle has not cleared yet");
    snapshot.battle_state = 1;
    check(!visible(), "loading or preparation does not draw world marks");
    snapshot.battle_state = -1;
    check(!visible(), "unknown battle state does not draw world marks");
    snapshot.battle_state = 2; snapshot.flags = ipc::kBattleStale;
    check(!visible(), "stale capture hides every layer");
    snapshot.flags = 0; snapshot.published_ms = 0;
    check(!visible(), "uninitialized publication is unavailable");
    snapshot.published_ms = 1200;
    check(!visible(), "future timestamp is unavailable");
    snapshot.published_ms = 1000;
    check(!arkhelp::overlay::can_draw_live(snapshot, true, true, false, true, 1280, 720, 3100),
          "expired publication is unavailable");

    auto unit = ranged_unit();
    check(contour(unit), "projected elliptical range renders even if HP is unknown");
    ipc::Float2 output[ipc::kRangePointCount];
    check(arkhelp::features::prepare_range_contour(unit, 1280, 720, output) &&
          output[0].x == unit.range_points[0].x && output[0].y == unit.range_points[0].y,
          "perspective contour is preserved instead of a screen-space circle");
    std::reverse(std::begin(unit.range_points), std::end(unit.range_points));
    check(arkhelp::features::prepare_range_contour(unit, 1280, 720, output) &&
          output[0].x == unit.range_points[15].x && output[0].y == unit.range_points[15].y,
          "winding is normalized for consistent overlapping opacity");
    for (const auto bit : {ipc::kUnitValid, ipc::kUnitRangeValid, ipc::kUnitGroundValid}) {
        unit = ranged_unit(); unit.flags &= ~bit;
        check(!contour(unit), "missing source/shape/ground availability rejects range");
    }
    unit = ranged_unit(); unit.tracking_id = 0;
    check(!contour(unit), "missing identity rejects range");
    unit = ranged_unit(); unit.range_point_count = 3;
    check(!contour(unit), "incomplete contour does not become a guessed shape");
    unit = ranged_unit(); unit.range_point_count = ipc::kRangePointCount + 1;
    check(!contour(unit), "oversized contour count is rejected before indexing");
    unit = ranged_unit(); unit.range_radius = -1;
    check(!contour(unit), "negative radius is unavailable");
    unit = ranged_unit(); unit.range_radius = std::numeric_limits<float>::quiet_NaN();
    check(!contour(unit), "NaN radius is unavailable");
    unit = ranged_unit(); unit.range_points[3].x = std::numeric_limits<float>::infinity();
    check(!contour(unit), "nonfinite contour coordinate cannot reach Direct2D");
    unit = ranged_unit(); unit.range_points[3].x = 1e20f;
    check(!contour(unit), "extreme projection is rejected");
    unit = ranged_unit(); unit.ground_x = std::numeric_limits<float>::quiet_NaN();
    check(!contour(unit), "invalid ground center is rejected");
    unit = ranged_unit(); unit.range_points[2] = {300, 200};
    check(!contour(unit), "a nonconvex or unsupported shape does not render as a circle");
    unit = ranged_unit(); std::swap(unit.range_points[2], unit.range_points[8]);
    check(!contour(unit), "self-crossing contour is rejected");
    unit = ranged_unit(); unit.ground_x = 700;
    check(!contour(unit), "range does not attach to a center outside its contour");
    unit = ranged_unit();
    for (auto& point : unit.range_points) { point.y = 200; }
    check(!contour(unit), "zero-area contour is rejected");
    unit = ranged_unit(); unit.ground_x -= 1000;
    for (auto& point : unit.range_points) { point.x -= 1000; }
    check(!contour(unit), "fully off-screen range is culled");
    unit = ranged_unit(); unit.ground_x -= 350;
    for (auto& point : unit.range_points) { point.x -= 350; }
    check(contour(unit), "partially visible contour can be clipped by the surface");
    std::printf("Overlay/range: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
