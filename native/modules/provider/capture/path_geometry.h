#pragma once
#include <arkhelp/ipc.h>
#include <cmath>
#include <cstdint>
#include "range_projection.h"

namespace ark::arkhelper::capture::path_geometry {
using Point = range_geometry::Point2;
struct Grid { std::int32_t row = 0, col = 0; };
inline bool operator==(Grid a, Grid b) { return a.row == b.row && a.col == b.col; }
inline bool finite(Point point) { return range_geometry::finite(point); }
inline bool same(Point a, Point b) {
    return std::abs(a.x-b.x) < .0001f && std::abs(a.y-b.y) < .0001f;
}

// Exact straight runs may be simplified; no point is omitted across a discontinuity or turn.
// A full buffer stops rather than connecting its prefix to an invented endpoint.
struct Route {
    struct Vertex { Point point; std::uint32_t flags = 0; };
    Vertex points[::arkhelp::ipc::kMaxPathPoints]{};
    std::uint32_t count = 0;
    bool truncated = false;
    bool append(Point point, bool move_to = false) {
        if (!path_geometry::finite(point)) return false;
        if (!move_to && count && same(points[count-1].point, point)) return true;
        if (!move_to && count >= 2 && !(points[count-1].flags & ::arkhelp::ipc::kPathMoveTo)) {
            const Point a = points[count-2].point, b = points[count-1].point;
            const double ux = b.x-a.x, uy = b.y-a.y, vx = point.x-b.x, vy = point.y-b.y;
            const double scale = std::hypot(ux,uy)*std::hypot(vx,vy);
            if (scale > 0 && std::abs(ux*vy-uy*vx) <= scale * .000001 && ux*vx+uy*vy >= 0) {
                points[count-1].point = point;
                return true;
            }
        }
        if (count >= ::arkhelp::ipc::kMaxPathPoints) { truncated = true; return false; }
        points[count++] = {point, move_to ? ::arkhelp::ipc::kPathMoveTo : 0u};
        return true;
    }
};

struct GridHistory {
    Grid grids[1024]{};
    std::uint32_t count = 0;
    bool visit(Grid value) {
        if (count >= 1024) return false;
        for (std::uint32_t i = 0; i < count; ++i) if (grids[i] == value) return false;
        grids[count++] = value;
        return true;
    }
};
} // namespace ark::arkhelper::capture::path_geometry
