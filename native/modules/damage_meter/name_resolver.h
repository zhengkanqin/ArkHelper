#pragma once

#include <string>
#include <string_view>

#include "../common/il2cpp_runtime.h"
#include "stage_names.h"

namespace ark::damage_meter {

// Looks up display names in the game's managed databases. Game thread only; every method may
// throw std::exception on IL2CPP failures unless documented otherwise.
class GameNameResolver {
public:
    // Throws when the IL2CPP runtime is not ready yet.
    GameNameResolver();

    // True once the character, token and patch databases have finished loading.
    bool actor_databases_ready();
    // Returns the localized actor name, or an empty string when the ID is unknown.
    std::string actor_name(const std::string& actor_id);

    // Loads stage names from StageDB and RoguelikeTopicDB. A catalog is retried on every call
    // until it loads completely; failures are reported through `error`, never thrown. Returns
    // true once both catalogs are loaded.
    bool load_stage_catalogs(std::string& error);

    std::string_view stage_label(std::string_view stage_id) const {
        return stages_.resolve(stage_id);
    }

private:
    struct StageRow;

    template <class Visit>
    void for_each_value(void* dictionary, Visit&& visit);
    StageRow read_stage_row(void* row, const char* id_field);
    bool load_stage_catalog(bool roguelike);

    il2cpp::Runtime runtime_;
    void* character_db_ = nullptr;
    void* token_db_ = nullptr;
    void* patch_db_ = nullptr;
    void* try_get_char_name_ = nullptr;
    StageNameIndex stages_;
    bool ordinary_stages_loaded_ = false;
    bool roguelike_stages_loaded_ = false;
};

} // namespace ark::damage_meter
