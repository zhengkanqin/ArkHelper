#pragma once
#include <array>
#include <utility>
#include "live_data.h"
#include "path_geometry.h"

namespace ark::arkhelper::capture::live {

// Reads only the current cursor's already-created checkpoint/next-map instances. Never advances
// a cursor, generates navigation maps, or calls PredictFuturePosition with implicit defaults.
class PathReader {
    struct Access {
        void* klass = nullptr;
        void* cursor = nullptr;
        void* index = nullptr;
        void* checkpoints = nullptr;
        void* offset = nullptr;
        void* route = nullptr;
        void* object = nullptr;
        void* map_position = nullptr;
        void* type = nullptr;
        void* target_grid = nullptr;
        void* target_pos = nullptr;
        void* next_map = nullptr;
        void* target_map = nullptr;
        void* pos = nullptr;
        void* next = nullptr;
        void* has_next = nullptr;
        void* safe_position = nullptr;
        void* array_get = nullptr;
    };
    std::array<Access, 32> cache_{};
    std::size_t next_ = 0;
    void* from_vector_ = nullptr;
    std::string reason_;
    bool unavailable(const char* reason) { reason_ = reason; return false; }

    Access& access(il2cpp::Runtime& rt, void* klass) {
        for (auto& entry : cache_) if (entry.klass == klass) return entry;
        Access& entry = cache_[next_++ % cache_.size()];
        entry = {}; entry.klass = klass;
        const std::string name = rt.klass_name(klass);
        if (name == "Torappu.Battle.DirectionCursor") {
            entry.index = field(rt, klass, "m_cursor");
            entry.checkpoints = field(rt, klass, "m_checkpoints");
            entry.offset = field(rt, klass, "m_offset");
            entry.route = field(rt, klass, "m_route");
            entry.object = field(rt, klass, "m_obj");
        } else if (rt.class_rank(klass) == 2) {
            try { entry.array_get = rt.method(klass, "GetValue", 2, {"System.Int32", "System.Int32"}); }
            catch (...) {}
        } else {
            entry.cursor = field(rt, klass, "m_cursor");
            entry.map_position = method(rt, klass, "get_mapPosition");
            entry.type = method(rt, klass, "get_type");
            entry.target_grid = method(rt, klass, "get_targetGrid");
            entry.target_pos = method(rt, klass, "get_targetPos");
            entry.next_map = field(rt, klass, "m_nextmap");
            entry.target_map = field(rt, klass, "m_targetNextMap");
            entry.pos = field(rt, klass, "pos");
            entry.next = field(rt, klass, "next");
            entry.has_next = field(rt, klass, "hasNext");
            entry.safe_position = method(rt, klass, "GetSafeNextPosition");
        }
        return entry;
    }
    static bool vector(il2cpp::Runtime& rt, void* boxed, path_geometry::Point& result) {
        if (!boxed || rt.klass_name(rt.object_class(boxed)) != "UnityEngine.Vector2") return false;
        result = rt.unbox<path_geometry::Point>(boxed);
        return path_geometry::finite(result);
    }
    static bool grid(il2cpp::Runtime& rt, void* boxed, path_geometry::Grid& result) {
        if (!boxed || rt.klass_name(rt.object_class(boxed)) != "Torappu.GridPosition") return false;
        result = rt.unbox<path_geometry::Grid>(boxed);
        return std::abs(static_cast<long long>(result.row)) < 8192 &&
               std::abs(static_cast<long long>(result.col)) < 8192;
    }
    static std::uint32_t mix(std::uint32_t hash, void* identity) {
        const auto bits = reinterpret_cast<std::uintptr_t>(identity);
        return (hash ^ static_cast<std::uint32_t>(bits) ^ static_cast<std::uint32_t>(bits >> 32)) * 16777619u;
    }
public:
    void clear() { cache_ = {}; next_ = 0; from_vector_ = nullptr; reason_.clear(); }
    const std::string& reason() const { return reason_; }

    template <class Budget>
    bool read(il2cpp::Runtime& rt, void* enemy, path_geometry::Point ground,
              path_geometry::Route& output, std::uint32_t& revision, Budget&& within_budget) {
        reason_ = "live route metadata or node geometry unavailable";
        if (!enemy || !path_geometry::finite(ground) || !within_budget())
            return unavailable("route budget expired before first segment, or no current ground position");
        const Access enemy_access = access(rt, rt.object_class(enemy));
        void* cursor = enemy_access.cursor ? rt.field_get_object(enemy_access.cursor, enemy) : nullptr;
        if (!cursor) return unavailable("enemy has no current movement cursor");
        if (rt.klass_name(rt.object_class(cursor)) != "Torappu.Battle.DirectionCursor") {
            reason_ = "unsupported cursor class: " + rt.klass_name(rt.object_class(cursor)); return false;
        }
        const Access ca = access(rt, rt.object_class(cursor));
        if (!ca.index || !ca.checkpoints || !ca.offset || !ca.route || !ca.object)
            return unavailable("DirectionCursor metadata fields unavailable");
        path_geometry::Point offset;
        if (!vector(rt, rt.field_get_object(ca.offset, cursor), offset)) return false;
        const bool translated = std::abs(offset.x) > .0001f || std::abs(offset.y) > .0001f;
        // Verified against the installed client's native code (2026-11-08):
        // GetLocatorPosition 0x1808D18CA calls VisualObject.get_mapPosition (0x180B23080),
        // then 0x1808D18E0/E5 adds m_offset. Enemy.footMapPosition is a different mount-point
        // projection and must not choose navigation cells when its animation crosses a grid edge.
        // GetNextTarget 0x1808D2363/68 subtracts it from the real checkpoint/node target.
        // Keep the initial ground point physical, look up the current node in locator space,
        // and convert every future target back exactly once. m_offset is caller supplied.
        const auto physical = [offset](path_geometry::Point point) {
            return path_geometry::Point{point.x-offset.x,point.y-offset.y};
        };
        void* host = rt.field_get_object(ca.object,cursor);
        const Access host_access = host ? access(rt,rt.object_class(host)) : Access{};
        path_geometry::Point root_position;
        if (!host_access.map_position || !vector(rt,rt.invoke(host_access.map_position,host),root_position))
            return unavailable("cursor host mapPosition getter unavailable");
        double current = -1;
        if (!number(rt, rt.field_get_object(ca.index, cursor), current)) return false;
        void* checkpoints = rt.field_get_object(ca.checkpoints, cursor);
        const int size = checkpoints ? rt.count(checkpoints) : 0;
        if (size < 0 || size > 256 || current < 0 || current > size || current != std::floor(current))
            return unavailable("current checkpoint index or checkpoint array is unavailable");
        if (!from_vector_) {
            void* grid_class = rt.find("Torappu", "GridPosition");
            from_vector_ = rt.method(grid_class, "FromVectorPosition", 1, {"UnityEngine.Vector2"});
        }
        void* route_object = rt.field_get_object(ca.route, cursor);
        if (!route_object) return false;
        const Access route_access = access(rt, rt.object_class(route_object));
        void* data = member(rt, route_object, "data");
        void* end = data ? member(rt, data, "endPosition") : nullptr;
        void* as_position = end ? method(rt, rt.object_class(end), "get_asLocalPosition") : nullptr;
        path_geometry::Point exit;
        const bool have_exit = as_position && vector(rt, rt.invoke(as_position, end), exit);
        revision = mix(2166136261u, route_object);
        if (!output.append(ground, true)) return false;
        ground = {root_position.x+offset.x,root_position.y+offset.y}; // locator space for all checkpoint traversal
        bool disconnected = false;
        // m_checkpoints contains explicit checkpoints only. The ordinary last leg, including
        // routes with no checkpoints at all, lives in Route.m_targetNextMap.
        for (int index = static_cast<int>(current); index <= size; ++index) {
            if (!within_budget()) { output.truncated = true; return output.count > 1; }
            const bool final_leg = index == size;
            if (final_leg && have_exit && path_geometry::same(ground, exit)) break;
            void* checkpoint = final_leg ? nullptr : rt.at(checkpoints, index);
            if (!final_leg && !checkpoint) return unavailable("current checkpoint is null");
            const Access cp = final_leg ? Access{} : access(rt, rt.object_class(checkpoint));
            if (!final_leg && !cp.type) return unavailable("checkpoint type getter unavailable");
            const std::string type = final_leg ? "MOVE" : enum_name(rt, rt.invoke(cp.type, checkpoint));
            if (type == "DISAPPEAR") {
                if (translated) return unavailable("nonzero-offset teleport semantics are not validated");
                disconnected = true; continue;
            }
            if (type == "APPEAR_AT_POS") {
                if (translated) return unavailable("nonzero-offset teleport semantics are not validated");
                path_geometry::Point appeared;
                if (!cp.target_pos || !vector(rt, rt.invoke(cp.target_pos, checkpoint), appeared)) return false;
                if (!output.append(physical(appeared), true)) return output.count > 1;
                ground = appeared;
                disconnected = false;
                continue;
            }
            if (type == "WAIT_FOR_SECONDS" || type == "WAIT_FOR_PLAY_TIME" ||
                type == "WAIT_CURRENT_FRAGMENT_TIME" || type == "WAIT_CURRENT_WAVE_TIME" ||
                type == "WAIT_BOSSRUSH_WAVE" || type == "ALERT") continue;
            // Patrol loops have a mutable repeat cursor; its repeated future cannot be treated
            // as a single route-to-exit. Unknown/trace/marbles checkpoints also remain unavailable.
            if (type == "PATROL_MOVE") { output.truncated = true; return output.count > 1; }
            if (type != "MOVE") { reason_ = "unsupported checkpoint: " + type; return false; }
            if (disconnected || (!final_leg && (!cp.next_map || !cp.target_grid || !cp.target_pos)))
                return unavailable("movement checkpoint target or next-map metadata unavailable");
            if (final_leg && (!have_exit || !route_access.target_map)) {
                output.truncated = true;
                return output.count > 1 || unavailable("remaining route end or target next-map unavailable");
            }
            void* map = final_leg ? rt.field_get_object(route_access.target_map, route_object)
                                 : rt.field_get_object(cp.next_map, checkpoint);
            if (!map || rt.class_rank(rt.object_class(map)) != 2 || !within_budget())
                return unavailable("current navigation map is missing, not rank two, or over budget");
            void* element = rt.class_element(rt.object_class(map));
            // Next-map Node is the real navigation value type. Match structure as well as name.
            if (!element || !rt.class_is_value(element) || std::strcmp(rt.class_name(element), "Node")) return false;
            const Access ma = access(rt, rt.object_class(map));
            const Access na = access(rt, element);
            if (!ma.array_get || !na.pos || !na.next || !na.has_next || !na.safe_position)
                return unavailable("Node[,] indexer or navigation-node metadata unavailable");
            path_geometry::Grid position, target;
            path_geometry::Point target_pos;
            if (!grid(rt, rt.invoke(from_vector_, nullptr, {&ground}), position))
                return unavailable("current navigation position did not convert to GridPosition");
            if (final_leg) {
                if (!grid(rt, end, target)) return unavailable("route end is not GridPosition");
                target_pos = exit;
            } else if (!grid(rt, rt.invoke(cp.target_grid, checkpoint), target) ||
                       !vector(rt, rt.invoke(cp.target_pos, checkpoint), target_pos))
                return unavailable("checkpoint target grid or position has unexpected type");
            revision = mix(revision, map);
            path_geometry::GridHistory visited;
            bool reached = false;
            // Array indexing is checked by the managed runtime; Node.pos verifies row/column
            // order before any node contributes geometry, including non-square maps.
            int order = -1;
            for (int steps = 0; steps < 1024; ++steps) {
                if (!within_budget()) { output.truncated = true; return output.count > 1; }
                if (!visited.visit(position)) { output.truncated = true; return output.count > 1; }
                void* node = nullptr;
                for (int attempt = 0; attempt < 2 && !node; ++attempt) {
                    const int candidate_order = order >= 0 ? order : attempt;
                    int first = candidate_order == 0 ? position.row : position.col;
                    int second = candidate_order == 0 ? position.col : position.row;
                    try {
                        void* candidate = rt.invoke(ma.array_get, map, {&first, &second});
                        path_geometry::Grid actual;
                        if (candidate && rt.object_class(candidate) == element &&
                            grid(rt, rt.field_get_object(na.pos, candidate), actual) && actual == position) {
                            node = candidate;
                            // A diagonal cell cannot distinguish [row,col] from [col,row].
                            if (position.row != position.col) order = candidate_order;
                        }
                    } catch (...) {}
                    if (order >= 0) break;
                }
                if (!node) return unavailable("navigation node position does not match the requested grid");
                if (position == target) { reached = true; break; }
                double has_next = 0;
                path_geometry::Grid next_grid;
                path_geometry::Point next_pos;
                if (!number(rt, rt.field_get_object(na.has_next, node), has_next) || !has_next ||
                    !grid(rt, rt.field_get_object(na.next, node), next_grid) || next_grid == position ||
                    !vector(rt, rt.invoke(na.safe_position, node), next_pos))
                    return unavailable("navigation node has no valid next grid or position");
                if (!output.append(physical(next_pos))) return output.count > 1;
                position = next_grid;
            }
            if (!reached) { output.truncated = true; return output.count > 1; }
            if (!output.append(physical(target_pos))) return output.count > 1;
            ground = target_pos;
        }
        if (disconnected) { output.truncated = true; return output.count > 1; }
        // Checkpoint exhaustion alone is not proof of reaching the real route end (special
        // clients may keep only instantiated checkpoints). Verify the final actual route grid.
        if (!within_budget() || !have_exit ||
            !path_geometry::same(output.points[output.count-1].point, physical(exit))) output.truncated = true;
        return output.count > 1;
    }
};
} // namespace ark::arkhelper::capture::live
