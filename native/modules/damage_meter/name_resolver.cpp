#include "name_resolver.h"

#include <exception>
#include <utility>
#include <vector>

namespace ark::damage_meter {

using Root = il2cpp::Runtime::Root;

struct GameNameResolver::StageRow {
    std::string id;
    std::string level;
    std::string code;
    std::string name;
    std::vector<std::string> aliases;
};

GameNameResolver::GameNameResolver() {
    runtime_.initialize();
}

bool GameNameResolver::actor_databases_ready() {
    if (!character_db_) {
        character_db_ = runtime_.find("Torappu", "CharacterDB");
    }
    if (!token_db_) {
        token_db_ = runtime_.find("Torappu", "TokenDB");
    }
    if (!patch_db_) {
        patch_db_ = runtime_.find("Torappu", "CharPatchDB");
    }
    if (!try_get_char_name_) {
        try_get_char_name_ =
            runtime_.method(runtime_.find("Torappu", "CharacterUtil"), "TryGetCharName", 2,
                            {"System.String", "System.String&"});
    }
    for (void* database : {character_db_, token_db_, patch_db_}) {
        if (!runtime_.unbox<bool>(runtime_.call(runtime_.singleton(database), "get_inited"))) {
            return false;
        }
    }
    return true;
}

std::string GameNameResolver::actor_name(const std::string& actor_id) {
    Root key(runtime_, runtime_.string_new(actor_id.c_str()));

    void* value = nullptr;
    if (runtime_.unbox<bool>(runtime_.invoke(try_get_char_name_, nullptr, {key.get(), &value}))) {
        return runtime_.string(value);
    }

    value = nullptr;
    if (runtime_.unbox<bool>(
            runtime_.call(runtime_.singleton(token_db_), "TryGetName", {key.get(), &value}))) {
        return runtime_.string(value);
    }

    void* patch_data = runtime_.call(runtime_.singleton(patch_db_), "get_data");
    void* patches = runtime_.get<void*>(patch_data, "patchChars");
    value = nullptr;
    if (patches &&
        runtime_.unbox<bool>(runtime_.call(patches, "TryGetValue", {key.get(), &value})) && value) {
        return runtime_.string(runtime_.get<void*>(value, "name"));
    }
    return {};
}

template <class Visit>
void GameNameResolver::for_each_value(void* dictionary, Visit&& visit) {
    Root entries(runtime_, dictionary);
    Root keys(runtime_, runtime_.call(entries.get(), "get_Keys"));
    Root iterator(runtime_, runtime_.call(keys.get(), "GetEnumerator"));
    while (runtime_.unbox<bool>(runtime_.call(iterator.get(), "MoveNext"))) {
        void* key = runtime_.call(iterator.get(), "get_Current");
        void* value = nullptr;
        if (runtime_.unbox<bool>(runtime_.call(entries.get(), "TryGetValue", {key, &value})) &&
            value) {
            visit(value);
        }
    }
}

GameNameResolver::StageRow GameNameResolver::read_stage_row(void* row, const char* id_field) {
    StageRow stage;
    stage.id = runtime_.string(runtime_.get<void*>(row, id_field));
    stage.level = runtime_.string(runtime_.get<void*>(row, "levelId"));
    stage.code = runtime_.string(runtime_.get<void*>(row, "code"));
    stage.name = runtime_.string(runtime_.get<void*>(row, "name"));
    return stage;
}

bool GameNameResolver::load_stage_catalog(bool roguelike) {
    void* database_class = runtime_.find("Torappu", roguelike ? "RoguelikeTopicDB" : "StageDB");
    Root database(runtime_, runtime_.singleton(database_class));
    if (!runtime_.unbox<bool>(runtime_.call(database.get(), "get_inited"))) {
        return false;
    }
    Root table(runtime_, runtime_.call(database.get(), "get_data"));
    if (!table.get()) {
        return false;
    }
    void* dictionary = runtime_.get<void*>(table.get(), roguelike ? "details" : "stages");
    if (!dictionary) {
        return false;
    }

    // Commit only after a complete native traversal; an unready table must not leave partial
    // aliases behind or permanently cache a missing name.
    std::vector<StageRow> rows;
    bool complete = true;
    if (roguelike) {
        for_each_value(dictionary, [&](void* topic) {
            void* topic_stages = runtime_.get<void*>(topic, "stages");
            if (!topic_stages) {
                complete = false;
                return;
            }
            for_each_value(topic_stages, [&](void* row) {
                StageRow stage = read_stage_row(row, "id");
                stage.aliases.push_back(runtime_.string(runtime_.get<void*>(row, "linkedStageId")));
                void* replacements = runtime_.get<void*>(row, "levelReplaceIds");
                const int count = runtime_.count(replacements);
                for (int i = 0; i < count; ++i) {
                    stage.aliases.push_back(runtime_.string(runtime_.at(replacements, i)));
                }
                rows.push_back(std::move(stage));
            });
        });
    } else {
        for_each_value(dictionary,
                       [&](void* row) { rows.push_back(read_stage_row(row, "stageId")); });
    }
    if (!complete || rows.empty()) {
        return false;
    }

    for (const StageRow& row : rows) {
        const std::size_t entry = stages_.add(row.id, row.level, row.code, row.name);
        for (const std::string& alias : row.aliases) {
            stages_.add_alias(entry, alias);
        }
    }
    return true;
}

bool GameNameResolver::load_stage_catalogs(std::string& error) {
    // Roguelike readiness or failures never gate ordinary stage names or actor names.
    if (!ordinary_stages_loaded_) {
        try {
            ordinary_stages_loaded_ = load_stage_catalog(false);
        } catch (const std::exception& failure) {
            error = failure.what();
        }
    }
    if (!roguelike_stages_loaded_) {
        try {
            roguelike_stages_loaded_ = load_stage_catalog(true);
        } catch (const std::exception& failure) {
            if (error.empty()) {
                error = failure.what();
            }
        }
    }
    return ordinary_stages_loaded_ && roguelike_stages_loaded_;
}

} // namespace ark::damage_meter
