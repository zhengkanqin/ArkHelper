#pragma once

#include <cstddef>
#include <deque>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace ark::damage_meter {

// Maps native stage and level IDs to display labels ("<code> <name>").
//
// Lookups are case-insensitive, treat '\' and '/' alike and ignore a trailing ".json". A stage
// is found by its exact stage ID, by a full level path, or by the level file's base name. A key
// claimed by two entries with different labels becomes ambiguous and resolves to nothing.
class StageNameIndex {
public:
    // Adds a stage and returns an entry handle for add_alias(). Entries without a label are
    // kept (so handles stay valid) but never registered.
    std::size_t add(std::string_view stage_id, std::string_view level_id, std::string_view code,
                    std::string_view name) {
        const std::size_t entry = labels_.size();
        labels_.push_back(make_label(code, name));
        if (!labels_[entry].empty()) {
            register_key(exact_, stage_id, entry);
            add_alias(entry, level_id);
        }
        return entry;
    }

    // Registers another level ID for an existing entry.
    void add_alias(std::size_t entry, std::string_view level_id) {
        if (entry >= labels_.size() || labels_[entry].empty() || level_id.empty()) {
            return;
        }
        if (level_id.find_first_of("/\\") != std::string_view::npos) {
            register_key(exact_, level_id, entry);
        }
        register_key(basenames_, basename(level_id), entry);
    }

    // Returns the label for a stage or level ID, or an empty view when unknown or ambiguous.
    std::string_view resolve(std::string_view id) const {
        if (id.empty()) {
            return {};
        }
        const auto exact = exact_.find(id);
        if (exact != exact_.end()) {
            return label(exact->second);
        }
        const auto alias = basenames_.find(basename(id));
        return alias == basenames_.end() ? std::string_view{} : label(alias->second);
    }

private:
    static constexpr std::size_t kAmbiguous = std::numeric_limits<std::size_t>::max();

    static std::string make_label(std::string_view code, std::string_view name) {
        std::string label;
        label.reserve(code.size() + name.size() + (!code.empty() && !name.empty() ? 1 : 0));
        label += code;
        if (!name.empty()) {
            if (!label.empty()) {
                label += ' ';
            }
            label += name;
        }
        return label;
    }

    static constexpr char normalized(char c) {
        if (c == '\\') {
            return '/';
        }
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }

    // Strips a case-insensitive ".json" suffix.
    static std::string_view resource(std::string_view id) {
        constexpr std::size_t kSuffixLength = 5;
        if (id.size() >= kSuffixLength) {
            const std::string_view suffix = id.substr(id.size() - kSuffixLength);
            if (suffix[0] == '.' && normalized(suffix[1]) == 'j' && normalized(suffix[2]) == 's' &&
                normalized(suffix[3]) == 'o' && normalized(suffix[4]) == 'n') {
                id.remove_suffix(kSuffixLength);
            }
        }
        return id;
    }

    static std::string_view basename(std::string_view id) {
        const std::size_t slash = id.find_last_of("/\\");
        return slash == std::string_view::npos ? id : id.substr(slash + 1);
    }

    // Transparent normalized comparison keeps lookups allocation-free.
    struct ResourceLess {
        using is_transparent = void;

        bool operator()(std::string_view left, std::string_view right) const {
            left = resource(left);
            right = resource(right);
            const std::size_t count = left.size() < right.size() ? left.size() : right.size();
            for (std::size_t i = 0; i < count; ++i) {
                const auto a = static_cast<unsigned char>(normalized(left[i]));
                const auto b = static_cast<unsigned char>(normalized(right[i]));
                if (a != b) {
                    return a < b;
                }
            }
            return left.size() < right.size();
        }
    };

    using KeyMap = std::map<std::string, std::size_t, ResourceLess>;

    void register_key(KeyMap& keys, std::string_view id, std::size_t entry) {
        if (id.empty()) {
            return;
        }
        const auto found = keys.find(id);
        if (found == keys.end()) {
            keys.emplace(std::string(id), entry);
        } else if (found->second != kAmbiguous && labels_[found->second] != labels_[entry]) {
            found->second = kAmbiguous;
        }
    }

    std::string_view label(std::size_t entry) const {
        return entry == kAmbiguous ? std::string_view{} : std::string_view(labels_[entry]);
    }

    // A deque keeps label storage stable while entries are appended.
    std::deque<std::string> labels_;
    KeyMap exact_;
    KeyMap basenames_;
};

} // namespace ark::damage_meter
