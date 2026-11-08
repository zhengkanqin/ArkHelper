#include "../native/modules/provider/capture/range_projection.h"
#include <cstdio>
#include <limits>

using namespace ark::arkhelper::capture::range_geometry;
namespace {
unsigned checks = 0, failures = 0;
void check(bool result, const char* label) {
    ++checks;
    if (!result) { ++failures; std::printf("FAIL %s\n", label); }
}
bool near(float a, float b) { return std::abs(a-b) < .00001f; }
float distance(Point2 a, Point2 b) { return std::hypot(a.x-b.x, a.y-b.y); }
}
int main() {
    Matrix4 view{}, projection{};
    for (int i = 0; i < 4; ++i) view.values[i*5] = 1;
    projection.values[0] = 1.2f;
    projection.values[5] = 1.8f;
    projection.values[10] = -1;
    projection.values[11] = -1;
    projection.values[14] = -.2f;
    const MapPlane tilted{{1, -.5f, -10}, {1, 0, 0}, {0, .6f, .8f}};
    const Point2 center{.5f, .25f};
    Outline small, large;
    check(project_circle(tilted, view, projection, center, 1, small), "tilted ground circle projects");
    Point2 expected_center;
    check(project(view, projection, world(tilted, center), expected_center) &&
          near(small.center.x, expected_center.x) && near(small.center.y, expected_center.y),
          "outline center matches projected ground center");
    bool all_finite = true;
    for (const auto point : small.points) all_finite = all_finite && finite(point);
    check(small.points.size() == 16 && all_finite, "all sixteen perimeter points are finite");
    check(std::abs(distance(small.points[0], small.points[8]) -
                   distance(small.points[4], small.points[12])) > .005f,
          "perspective circle is not replaced with a screen circle");
    check(project_circle(tilted, view, projection, center, 2, large) &&
          distance(large.points[0], large.points[8]) > distance(small.points[0], small.points[8]) &&
          distance(large.points[4], large.points[12]) > distance(small.points[4], small.points[12]),
          "current larger radius produces larger projected outline");
    Point2 point;
    check(project(view, projection, {2, 3, -10}, point) && near(point.x, .62f) && near(point.y, .77f),
          "column-major perspective projection matches manual coordinates");
    Matrix4 translated = view;
    translated.values[12] = 1;
    translated.values[13] = -1;
    check(project(translated, projection, {2, 3, -10}, point) && near(point.x, .68f) && near(point.y, .68f),
          "column-major camera translation uses the fourth column");
    check(!project(view, projection, {0, 0, 1}, point), "point behind camera is rejected");
    check(!project(view, projection, {0, 0, 0}, point), "camera-plane point is rejected");
    MapPlane behind = tilted; behind.origin.z = 3;
    check(!project_circle(behind, view, projection, center, 1, small), "behind-camera outline is rejected");
    MapPlane crossing{{0, 0, -1}, {1, 0, 0}, {0, .6f, .8f}};
    check(!project_circle(crossing, view, projection, {0, 0}, 2, small),
          "outline crossing the camera plane is rejected as a whole");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    check(!project_circle(tilted, view, projection, center, nan, small), "NaN radius is rejected");
    check(!project_circle(tilted, view, projection, {nan, 0}, 1, small), "NaN ground position is rejected");
    check(!project_circle(tilted, view, projection, center, 33, small), "oversized radius is rejected");
    check(!project_circle(tilted, view, projection, center, .01f, small), "degenerate radius is rejected");
    std::printf("Range projection: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
