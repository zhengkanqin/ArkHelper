#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace ark {
inline bool is_operator_id(std::string_view id) {
    return id.starts_with("char_") && id.size() > 5;
}
inline bool is_device_id(std::string_view id) {
    return id.starts_with("trap_") && id.size() > 5;
}

struct OperatorIdentity {
    std::string id;
    std::uint32_t instance = 0;
    bool token = false;
};

// Follow only the game's explicit host links. Never guess an owner from a token name
// or a shared character ID: several copies of one operator may be on the field.
template <class Read, class Host>
bool resolve_operator(void* object, OperatorIdentity& out, Read&& read, Host&& host, bool include_devices = false) {
    out = {};
    std::array<void*, 8> seen{};
    for (std::size_t depth = 0; object && depth < seen.size(); ++depth) {
        for (std::size_t i = 0; i < depth; ++i) if (seen[i] == object) return false;
        seen[depth] = object;
        OperatorIdentity current;
        if (!read(object, current)) return false;
        if (!current.token && is_operator_id(current.id)) {
            out = std::move(current);
            return true;
        }
        void* parent = host(object);
        if ((!parent || parent == object) && include_devices && !current.token && is_device_id(current.id)) {
            out = std::move(current);
            return true;
        }
        object = parent;
    }
    return false;
}
} // namespace ark
