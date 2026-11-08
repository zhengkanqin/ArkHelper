#pragma once
#include <array>
#include "live_data.h"
#include "range_projection.h"

namespace ark::arkhelper::capture::live {

// No instance/radius is retained across samples. Only checked metadata accessors are cached.
class RangeReader {
    struct Access {
        void* klass = nullptr;
        void* attack = nullptr;
        void* mode = nullptr;
        void* trigger = nullptr;
        void* selector = nullptr;
        void* selector_range = nullptr;
        void* foot = nullptr;
        void* apply = nullptr;
        void* range = nullptr;
        void* radius = nullptr;
        NumericGetter radius_value;
        void* external = nullptr;
        void* circle = nullptr;
        void* offset = nullptr;
    };
    std::array<Access, 32> cache_{};
    std::size_t next_ = 0;
    Access& access(il2cpp::Runtime& rt, void* klass) {
        for (auto& a : cache_) if (a.klass == klass) return a;
        Access& a = cache_[next_++ % cache_.size()];
        a = {}; a.klass = klass;
        a.attack = method(rt, klass, "get_attack");
        a.mode = method(rt, klass, "get_currentMode");
        a.trigger = method(rt, klass, "get_attackTrigger");
        a.selector = method(rt, klass, "get_selector");
        a.selector_range = field(rt, klass, "m_range");
        a.foot = method(rt, klass, "get_footMapPosition");
        a.apply = method(rt, klass, "get_applyWay");
        a.range = method(rt, klass, "get_rangeToShow");
        a.radius = method(rt, klass, "get_radius");
        a.radius_value = getter(rt, a.radius);
        a.external = field(rt, klass, "m_externCollidersList");
        a.circle = field(rt, klass, "m_circleCollider");
        a.offset = method(rt, klass, "get_offset");
        return a;
    }
public:
    void clear() { cache_ = {}; next_ = 0; }
    bool foot(il2cpp::Runtime& rt, void* unit, range_geometry::Point2& out) {
        Access& a = access(rt, rt.object_class(unit));
        if (!a.foot) return false;
        void* result = rt.invoke(a.foot, unit);
        if (!result || rt.klass_name(rt.object_class(result)) != "UnityEngine.Vector2") return false;
        out = rt.unbox<range_geometry::Point2>(result);
        return range_geometry::finite(out);
    }
    bool radius(il2cpp::Runtime& rt, void* unit, float& radius) {
        const Access ua = access(rt, rt.object_class(unit));
        if (!ua.attack) return false;
        void* attack = rt.invoke(ua.attack, unit);
        if (!attack) return false;
        const Access aa = access(rt, rt.object_class(attack));
        if (!aa.apply) return false;
        void* apply = rt.invoke(aa.apply, attack);
        // Exact RANGED also excludes MELEE/RANGED combinations and non-attack capabilities.
        if (enum_name(rt, apply) != "RANGED") return false;
        void* range = aa.range ? rt.invoke(aa.range, attack) : nullptr;
        // The actual client keeps ordinary RangedAttack's _selector null: target searching
        // belongs to UnitMode._attackTrigger -> SelectorTrigger.selector -> m_range.
        // Verify that this is still the same mode attack before using its search trigger.
        if (!range && ua.mode && ua.trigger) {
            void* mode = rt.invoke(ua.mode, unit);
            const Access ma = mode ? access(rt, rt.object_class(mode)) : Access{};
            if (ma.attack && rt.invoke(ma.attack, mode) == attack) {
                void* trigger = rt.invoke(ua.trigger, unit);
                const Access ta = trigger ? access(rt, rt.object_class(trigger)) : Access{};
                void* selector = ta.selector ? rt.invoke(ta.selector, trigger) : nullptr;
                const Access sa = selector ? access(rt, rt.object_class(selector)) : Access{};
                if (sa.selector_range) range = rt.field_get_object(sa.selector_range, selector);
            }
        }
        if (!range) return false;
        void* klass = rt.object_class(range);
        const std::string name = rt.klass_name(klass);
        if (name != "Torappu.Battle.CircleRange" && name != "Torappu.Battle.LiteCircleRange") return false;
        const Access ra = access(rt, klass);
        // LiteCircleRange inherits CircleRange, which inherits PhysicsRange in client v29.
        // Read the actual circular collider rather than assume m_allColliders is populated;
        // the lightweight implementation can use a different candidate search path.
        if (!ra.radius || !ra.external || !ra.circle) return false;
        void* external = rt.field_get_object(ra.external, range);
        if (external && rt.count(external) != 0) return false;
        void* circle = rt.field_get_object(ra.circle, range);
        if (!circle || rt.klass_name(rt.object_class(circle)) != "UnityEngine.CircleCollider2D") return false;
        Access& ca = access(rt, rt.object_class(circle));
        if (!ca.offset) return false;
        void* boxed_offset = rt.invoke(ca.offset, circle);
        if (!boxed_offset || rt.klass_name(rt.object_class(boxed_offset)) != "UnityEngine.Vector2") return false;
        const auto offset = rt.unbox<range_geometry::Point2>(boxed_offset);
        if (!range_geometry::finite(offset) || std::abs(offset.x) > .001f || std::abs(offset.y) > .001f) return false;
        double result = 0;
        if (!ra.radius_value.read(range, result) || result < .05 || result > 32) return false;
        radius = static_cast<float>(result);
        return true;
    }
};
} // namespace ark::arkhelper::capture::live
