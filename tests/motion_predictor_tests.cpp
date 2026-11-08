#include "../src/features/motion_predictor.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>

using arkhelp::features::UnitMotionPredictor;
using arkhelp::ipc::BattleSnapshot;
namespace {
int failures = 0, checks = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) { std::printf("FAIL %s\n", label); ++failures; }
}
bool near(float a, float b) { return std::abs(a - b) < 0.05f; }
BattleSnapshot sample(std::uint64_t ms, double time, float x, std::uint64_t id = 1) {
    BattleSnapshot s;
    s.in_battle = 1;
    s.battle_serial = 1;
    s.published_ms = s.unit_sample_ms = ms;
    s.time_seconds = s.unit_sample_battle_time = time;
    s.unit_count = 1;
    s.units[0].x = x;
    s.units[0].y = 200;
    s.units[0].hp = 99;
    s.units[0].max_hp = 100;
    s.units[0].tracking_id = id;
    s.units[0].flags = arkhelp::ipc::kUnitValid | arkhelp::ipc::kUnitHpValid;
    return s;
}
void apply(UnitMotionPredictor& predictor, BattleSnapshot& s, std::uint64_t now) {
    predictor.apply(s, now, 1280, 720);
}
void train(UnitMotionPredictor& predictor) {
    auto a = sample(1000, 10, 100);
    apply(predictor, a, 1000);
    auto b = sample(1250, 10.25, 125);
    apply(predictor, b, 1250);
}
} // namespace

int main() {
    UnitMotionPredictor p;
    auto a = sample(1000, 10, 100);
    apply(p, a, 1100);
    check(near(a.units[0].x, 100), "one sample cannot invent velocity");
    auto b = sample(1250, 10.25, 125);
    apply(p, b, 1350);
    check(near(b.units[0].x, 135), "constant motion predicts between samples");
    check(near(b.units[0].hp, 99) && near(b.units[0].max_hp, 100), "HP is never predicted");
    b = sample(1250, 10.25, 125);
    b.published_ms = 1350; b.time_seconds = 10.35;
    apply(p, b, 1400);
    check(near(b.units[0].x, 140), "IPC republish must not retrain the same position sample");
    b = sample(1250, 10.25, 125);
    b.published_ms = 1500; b.time_seconds = 10.5;
    apply(p, b, 1900);
    check(b.units[0].x <= 155.05f, "missing samples have a bounded prediction horizon");

    p.reset(); train(p);
    b = sample(1500, 10.5, 150);
    apply(p, b, 1500);
    check(near(b.units[0].x, 150), "constant speed has no correction jump on a new sample");
    b = sample(1750, 10.75, 150);
    apply(p, b, 1800);
    check(near(b.units[0].x, 150), "blocked enemy immediately stops extrapolating");

    p.reset(); train(p);
    b = sample(1250, 10.25, 125);
    b.flags |= arkhelp::ipc::kBattlePaused;
    apply(p, b, 1400);
    check(near(b.units[0].x, 125), "explicit pause displays the actual sampled position");
    b.flags = 0; b.published_ms = 1350;
    apply(p, b, 1400);
    check(near(b.units[0].x, 125), "resume edge returns to the real position without a pause-time jump");
    b = sample(1250, 10.25, 125); b.published_ms = 1400;
    apply(p, b, 1450);
    check(near(b.units[0].x, 130), "prediction resumes on the next render without waiting for a full unit sample");
    b = sample(1500, 10.5, 150);
    apply(p, b, 1550);
    check(b.units[0].x >= 150 && b.units[0].x <= 160, "resume stays bounded");

    p.reset(); train(p);
    b = sample(1250, 10.25, 125); b.published_ms = 1350;
    apply(p, b, 1400);
    check(near(b.units[0].x, 130), "an unchanged published logic time cannot invent a pause");

    p.reset(); train(p);
    b = sample(1500, 10.25, 125); b.flags |= arkhelp::ipc::kBattlePaused;
    apply(p, b, 1500);
    check(near(b.units[0].x, 125), "paused unit refresh remains at its sampled truth");
    b = sample(1750, 10.25, 125); b.flags |= arkhelp::ipc::kBattlePaused;
    apply(p, b, 1750);
    b = sample(1750, 10.25, 125); b.published_ms = 1800;
    apply(p, b, 1800);
    check(near(b.units[0].x, 125), "resume after paused refreshes starts without a stale velocity jump");
    b = sample(1750, 10.25, 125); b.published_ms = 1850; b.time_seconds = 10.30;
    apply(p, b, 1850);
    check(near(b.units[0].x, 130), "paused refreshes preserve useful velocity for the first resumed frames");

    p.reset();
    a = sample(1000, 10, 100); apply(p, a, 1000);
    b = sample(1250, 10.5, 150); apply(p, b, 1350);
    check(near(b.units[0].x, 170), "double speed uses the battle clock and current wall-clock rate");
    b = sample(1500, 10.75, 175); apply(p, b, 1600);
    check(b.units[0].x > 175 && b.units[0].x <= 195, "speed change adjusts prediction rate");

    p.reset(); train(p);
    b = sample(1500, 10.5, 600); apply(p, b, 1600);
    check(near(b.units[0].x, 600), "teleport snaps and clears velocity");
    b = sample(1750, 10.75, 625); apply(p, b, 1800);
    check(near(b.units[0].x, 630), "tracking retrains after teleport");

    p.reset(); train(p);
    b = sample(1500, 10.5, 150, 2); apply(p, b, 1600);
    check(near(b.units[0].x, 150), "new enemy at an old array index has no inherited velocity");
    p.reset();
    a = sample(1000, 10, 100);
    a.unit_count = 2; a.units[1] = a.units[0]; a.units[1].tracking_id = 2; a.units[1].x = 300;
    apply(p, a, 1000);
    b = sample(1250, 10.25, 275, 2);
    b.unit_count = 2; b.units[1] = b.units[0]; b.units[1].tracking_id = 1; b.units[1].x = 125;
    apply(p, b, 1350);
    check(near(b.units[0].x, 265) && near(b.units[1].x, 135), "array reorder follows stable identities");

    p.reset(); train(p);
    b = sample(1500, 10.5, 150); b.battle_serial = 2; apply(p, b, 1600);
    check(near(b.units[0].x, 150), "new battle cannot inherit old predictions");
    p.reset(); train(p);
    b = sample(1500, 10.5, 150); p.apply(b, 1600, 1920, 1080);
    check(near(b.units[0].x, 150), "window resize resets the pixel coordinate history");
    p.reset(); train(p);
    b = sample(1400, 1, 140); apply(p, b, 1500);
    check(near(b.units[0].x, 140), "battle clock rewind resets history");
    p.reset(); train(p);
    b = sample(900, 9, 90); apply(p, b, 1400);
    check(near(b.units[0].x, 90), "sample clock rewind resets history");
    p.reset(); train(p);
    b = sample(1500, 10.5, 150, 0); apply(p, b, 1600);
    check(near(b.units[0].x, 150), "missing identity never falls back to array index matching");
    p.reset(); train(p);
    b = sample(1500, 10.5, std::numeric_limits<float>::quiet_NaN()); apply(p, b, 1600);
    check(!(b.units[0].flags & 1u), "invalid coordinates cannot reach the renderer");

    p.reset();
    a = sample(1000, 10, 100); a.spawn_count = 1;
    a.spawns[0].flags = arkhelp::ipc::kSpawnScreenValid; a.spawns[0].screen = {100, 100};
    apply(p, a, 1000);
    b = sample(1250, 10.25, 125); b.spawn_count = 1; b.spawns[0] = a.spawns[0];
    apply(p, b, 1250);
    b = sample(1500, 10.5, 160); b.spawn_count = 1; b.spawns[0] = a.spawns[0];
    b.spawns[0].screen.x += 10; apply(p, b, 1600);
    check(near(b.units[0].x, 160), "stationary map anchor detects camera movement");

    p.reset(); train(p);
    b = sample(1500, 10.5, 152); apply(p, b, 1500);
    check(near(b.units[0].x, 150), "small sample corrections preserve position continuity");
    b = sample(1500, 10.5, 152); apply(p, b, 1580);
    check(near(b.units[0].x, 160.64f), "small correction settles within 80ms");

    p.reset();
    for (int phase = 0; phase < 3; ++phase) {
        auto group = sample(1000 + phase * 250, 10.0 + phase * .25, 100);
        group.unit_count = 3;
        for (int i = 0; i < 3; ++i) {
            group.units[i] = group.units[0]; group.units[i].tracking_id = i + 1;
            group.units[i].x = 100.0f + i * 100.0f + phase * 25.0f + (phase == 2 ? 20.0f : 0);
        }
        apply(p, group, 1050 + phase * 250);
        if (phase == 1) {
            check(group.units[0].x > 125 && group.units[1].x > 225,
                  "a marching group still predicts before a camera change");
        }
        if (phase == 2) {
            check(near(group.units[0].x, 170) && near(group.units[1].x, 270),
                  "common camera residual resets predictions even without spawn anchors");
        }
    }

    const auto range_sample = [](std::uint64_t ms, double time, float ground_x, float label_y) {
        auto s = sample(ms, time, ground_x);
        auto& unit = s.units[0];
        unit.flags |= arkhelp::ipc::kUnitGroundValid | arkhelp::ipc::kUnitRangeValid;
        unit.ground_x = ground_x;
        unit.ground_y = 250;
        unit.y = label_y;
        unit.range_radius = 2;
        unit.range_point_count = arkhelp::ipc::kRangePointCount;
        for (std::uint32_t i = 0; i < unit.range_point_count; ++i) {
            const double angle = i * 6.283185307179586 / unit.range_point_count;
            unit.range_points[i] = {ground_x + 100 * static_cast<float>(std::cos(angle)),
                                    250 + 40 * static_cast<float>(std::sin(angle))};
        }
        return s;
    };
    p.reset();
    a = range_sample(1000, 10, 100, 200); apply(p, a, 1000);
    b = range_sample(1250, 10.25, 125, 220); apply(p, b, 1350);
    check(near(b.units[0].ground_x, 135) && near(b.units[0].ground_y, 250),
          "range prediction follows the ground center, not model bobbing");
    check(near(b.units[0].x, 135) && near(b.units[0].y, 220),
          "HP label shares the ground movement offset without predicting model bobbing");
    bool unchanged_shape = true;
    const auto truth = range_sample(1250, 10.25, 125, 220);
    for (std::uint32_t i = 0; i < arkhelp::ipc::kRangePointCount; ++i) {
        unchanged_shape &= near(b.units[0].range_points[i].x, truth.units[0].range_points[i].x + 10) &&
                           near(b.units[0].range_points[i].y, truth.units[0].range_points[i].y);
    }
    check(unchanged_shape, "range perimeter translates with the ground and keeps its projected shape");
    b = range_sample(1250, 10.25, 125, 220); b.flags |= arkhelp::ipc::kBattlePaused;
    apply(p, b, 1400);
    check(near(b.units[0].ground_x, 125) && near(b.units[0].range_points[0].x, 225),
          "pause freezes the range at the real sampled contour");
    p.reset(); // The foreground/visibility gate resets on every hide event.
    b = range_sample(1250, 10.25, 125, 220); apply(p, b, 1400);
    check(near(b.units[0].ground_x, 125) && near(b.units[0].range_points[0].x, 225),
          "returning to the foreground cannot resume an old velocity or contour trail");
    b = range_sample(1250, 10.25, 125, 220); apply(p, b, 2400);
    check(b.unit_count == 0, "expired unit samples hide ranges and HP together");
    p.reset(); train(p);
    b = range_sample(1500, 10.5, 150, 220); apply(p, b, 1600);
    check(near(b.units[0].ground_x, 150), "switching from body to ground coordinates resets velocity");

    // Worst supported roster, 4 Hz sample generation / 30 Hz render. Report a compute measurement
    // separately from game performance, which still needs an actual battle.
    p.reset();
    auto source = sample(1000, 10, 100);
    source.unit_count = arkhelp::ipc::kMaxUnits;
    for (std::uint32_t i = 0; i < source.unit_count; ++i) {
        source.units[i] = source.units[0]; source.units[i].tracking_id = i + 1;
    }
    const auto begin = std::chrono::steady_clock::now();
    constexpr std::uint64_t frames = 30000;
    for (std::uint64_t i = 0; i < frames; ++i) {
        const auto offset = i * 33;
        const auto sampled = (offset / 250) * 250;
        source.published_ms = 1000 + (offset / 100) * 100;
        source.time_seconds = 10.0 + ((offset / 100) * 100) / 1000.0;
        source.unit_sample_ms = 1000 + sampled;
        source.unit_sample_battle_time = 10.0 + sampled / 1000.0;
        for (std::uint32_t j = 0; j < source.unit_count; ++j) { source.units[j].x = 100 + sampled * 0.001f; }
        auto rendered = source;
        apply(p, rendered, 1000 + offset);
        if (!std::isfinite(rendered.units[63].x)) { check(false, "benchmark finite output"); break; }
    }
    const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
    std::printf("Motion prediction: %d checks, %d failures; 64 units, %.2f us/render (algorithm only)\n",
                checks, failures, elapsed / frames);
    return failures ? 1 : 0;
}
