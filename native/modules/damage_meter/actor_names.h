#pragma once

#include <functional>
#include <map>
#include <string>

namespace ark::damage_meter {

// Actor ID -> localized name. An empty name means the lookup found nothing.
using ActorNameMap = std::map<std::string, std::string, std::less<>>;

// Returns the localized name, falling back to the raw ID.
inline const char* display_name(const ActorNameMap& names, const std::string& actor_id) {
    const auto found = names.find(actor_id);
    return found == names.end() || found->second.empty() ? actor_id.c_str() : found->second.c_str();
}

} // namespace ark::damage_meter
