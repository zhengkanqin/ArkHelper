#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace ark::arkhelper::capture::range_geometry {
inline constexpr std::uint32_t kPointCount = 16;
struct Point2 { float x = 0, y = 0; };
struct Point3 { float x = 0, y = 0, z = 0; };
struct Matrix4 { float values[16]{}; }; // Unity Matrix4x4 is column major.
struct MapPlane { Point3 origin, x_axis, y_axis; };
struct Outline {
    Point2 center;
    std::array<Point2, kPointCount> points{};
};
inline bool finite(Point2 p) { return std::isfinite(p.x) && std::isfinite(p.y); }
inline bool finite(Point3 p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
inline Point3 world(const MapPlane& plane, Point2 p) {
    return {plane.origin.x + p.x * plane.x_axis.x + p.y * plane.y_axis.x,
            plane.origin.y + p.x * plane.x_axis.y + p.y * plane.y_axis.y,
            plane.origin.z + p.x * plane.x_axis.z + p.y * plane.y_axis.z};
}
inline bool project(const Matrix4& view, const Matrix4& projection, Point3 p, Point2& viewport) {
    if (!finite(p)) return false;
    const auto& v = view.values;
    const auto& m = projection.values;
    const float camera[] = {v[0]*p.x+v[4]*p.y+v[8]*p.z+v[12],
                            v[1]*p.x+v[5]*p.y+v[9]*p.z+v[13],
                            v[2]*p.x+v[6]*p.y+v[10]*p.z+v[14],
                            v[3]*p.x+v[7]*p.y+v[11]*p.z+v[15]};
    const float x = m[0]*camera[0]+m[4]*camera[1]+m[8]*camera[2]+m[12]*camera[3];
    const float y = m[1]*camera[0]+m[5]*camera[1]+m[9]*camera[2]+m[13]*camera[3];
    const float w = m[3]*camera[0]+m[7]*camera[1]+m[11]*camera[2]+m[15]*camera[3];
    if (!std::isfinite(w) || w <= .0001f || !std::isfinite(x) || !std::isfinite(y) || camera[2] >= -.0001f) return false;
    viewport = {0.5f + .5f * x / w, 0.5f + .5f * y / w};
    return finite(viewport) && viewport.x >= -8 && viewport.x <= 9 && viewport.y >= -8 && viewport.y <= 9;
}
inline bool project_circle(const MapPlane& plane, const Matrix4& view, const Matrix4& projection,
                           Point2 center, float radius, Outline& out) {
    if (!finite(center) || !std::isfinite(radius) || radius < .05f || radius > 32.f ||
        !project(view, projection, world(plane, center), out.center)) return false;
    for (std::uint32_t i = 0; i < kPointCount; ++i) {
        const float angle = static_cast<float>(i) * 6.283185307179586f / kPointCount;
        Point2 point{center.x + radius * std::cos(angle), center.y + radius * std::sin(angle)};
        if (!project(view, projection, world(plane, point), out.points[i])) return false;
    }
    return true;
}
} // namespace ark::arkhelper::capture::range_geometry
