#pragma once

#include <algorithm>
#include <cstdint>

namespace ark::damage_meter {

// The game's optional/default-valued parameters are ordinary System.Boolean,
// not Nullable<Boolean>. Keep these aliases shared with the native ABI tests.
using ModifyCostFn = bool (*)(void*, int, void*, int, bool, bool, bool, bool, bool, void*);
using WithdrawFn = bool (*)(void*, bool, bool, bool, bool, void*);

inline bool attributable_cost(int requested, bool operator_source, bool withdrawing,
                              bool deployment_adjustment) {
    return requested > 0 && operator_source && !withdrawing && !deployment_adjustment;
}

inline int effective_cost_gain(int requested, int before, int after,
                               std::int64_t nested_delta = 0) {
    if (requested <= 0 || before < 0 || after < 0) return 0;
    const auto delta = static_cast<std::int64_t>(after) - before - nested_delta;
    return static_cast<int>(std::clamp<std::int64_t>(delta, 0, requested));
}

// A recursive ModifyCost call already accounts for its own balance change. Remove
// that whole change from the enclosing call, including excluded sources/refunds,
// so the enclosing operator receives only its own effective gain.
class CostMutationScope {
public:
    static bool observing(void* controller, int side) {
        return observer(active_, controller, side) != nullptr;
    }
    CostMutationScope(void* controller, int side, bool sampled, int before)
        : parent_(active_), controller_(controller), side_(side), sampled_(sampled), before_(before) {
        active_ = this;
    }
    ~CostMutationScope() {
        if (sampled_ && !finished_)
            if (auto* parent = observer(parent_, controller_, side_)) parent->children_valid_ = false;
        active_ = parent_;
    }
    CostMutationScope(const CostMutationScope&) = delete;
    CostMutationScope& operator=(const CostMutationScope&) = delete;

    int finish(int requested, int after) {
        if (finished_) return 0;
        finished_ = true;
        if (!sampled_) return 0;
        const bool valid = before_ >= 0 && after >= 0;
        if (auto* parent = observer(parent_, controller_, side_)) {
            if (valid) parent->nested_delta_ += static_cast<std::int64_t>(after) - before_;
            else parent->children_valid_ = false;
        }
        return valid && children_valid_ ? effective_cost_gain(requested, before_, after, nested_delta_) : 0;
    }

private:
    static CostMutationScope* observer(CostMutationScope* scope, void* controller, int side) {
        for (; scope; scope = scope->parent_)
            if (scope->sampled_ && !scope->finished_ && scope->controller_ == controller && scope->side_ == side)
                return scope;
        return nullptr;
    }
    inline static thread_local CostMutationScope* active_ = nullptr;
    CostMutationScope* parent_;
    void* controller_;
    int side_;
    bool sampled_, finished_ = false, children_valid_ = true;
    int before_;
    std::int64_t nested_delta_ = 0;
};

} // namespace ark::damage_meter
