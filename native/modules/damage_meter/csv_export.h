#pragma once

#include <optional>
#include <string>
#include <vector>

#include "actor_names.h"
#include "stats.h"

namespace ark::damage_meter {

// Writes `<root>/exports/damage-<pid>-<battle>.csv` (UTF-8 with BOM) with one line per row.
// Returns the UTF-8 file path, or nullopt when the file cannot be opened. Throws on other
// filesystem failures.
std::optional<std::string> write_csv_report(const char* root_directory, const BattleStats& stats,
                                            const std::vector<const ActorRow*>& rows,
                                            const ActorNameMap& actor_names);

} // namespace ark::damage_meter
