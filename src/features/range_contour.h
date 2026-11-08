#pragma once

#include <algorithm>
#include <cmath>
#include "arkhelp/ipc.h"

namespace arkhelp::features {

// Only use the captured, projected world contour. No screen-radius estimate or template fallback.
// Normalize winding so a compound fill has consistent opacity where several ranges overlap.
inline bool prepare_range_contour(const ipc::UnitEntry& unit, int width, int height,
                                   ipc::Float2 (&out)[ipc::kRangePointCount]) {
    constexpr auto required = ipc::kUnitValid | ipc::kUnitGroundValid | ipc::kUnitRangeValid;
    if ((unit.flags & required) != required || !unit.tracking_id || width <= 0 || height <= 0 ||
        unit.range_point_count != ipc::kRangePointCount || !std::isfinite(unit.range_radius) ||
        unit.range_radius <= 0 || !std::isfinite(unit.ground_x) || !std::isfinite(unit.ground_y)) {
        return false;
    }
    const float bound = static_cast<float>(std::max(width, height)) * 8.0f;
    if (std::abs(unit.ground_x) > bound || std::abs(unit.ground_y) > bound) { return false; }
    float left = bound, right = -bound, top = bound, bottom = -bound;
    double area = 0;
    for (std::uint32_t i = 0; i < ipc::kRangePointCount; ++i) {
        const auto& p = unit.range_points[i];
        const auto& q = unit.range_points[(i + 1) % ipc::kRangePointCount];
        if (!std::isfinite(p.x) || !std::isfinite(p.y) ||
            std::abs(p.x) > bound || std::abs(p.y) > bound) { return false; }
        left = std::min(left, p.x); right = std::max(right, p.x);
        top = std::min(top, p.y); bottom = std::max(bottom, p.y);
        area += static_cast<double>(p.x) * q.y - static_cast<double>(p.y) * q.x;
    }
    if (!std::isfinite(area) || std::abs(area) < 2.0 || right < 0 || bottom < 0 ||
        left > width || top > height) { return false; }
    const double orientation = area > 0 ? 1.0 : -1.0;
    for (std::uint32_t i = 0; i < ipc::kRangePointCount; ++i) {
        const auto& p = unit.range_points[i];
        const auto& q = unit.range_points[(i + 1) % ipc::kRangePointCount];
        const auto& r = unit.range_points[(i + 2) % ipc::kRangePointCount];
        const double turn = (static_cast<double>(q.x) - p.x) * (r.y - q.y) -
                            (static_cast<double>(q.y) - p.y) * (r.x - q.x);
        const double center = (static_cast<double>(q.x) - p.x) * (unit.ground_y - p.y) -
                              (static_cast<double>(q.y) - p.y) * (unit.ground_x - p.x);
        if (turn * orientation < -0.01 || center * orientation < -0.01) { return false; }
        out[i] = unit.range_points[area > 0 ? i : ipc::kRangePointCount - i - 1];
    }
    return true;
}

} // namespace arkhelp::features
