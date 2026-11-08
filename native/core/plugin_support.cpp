#include "plugin_support.h"

#include <algorithm>
#include <cwctype>
#include <string_view>

namespace ark::core::plugin_detail {

std::string to_utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::filesystem::path from_utf8(const std::string& text) {
    return std::filesystem::path(
        std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::wstring path_key(const std::filesystem::path& path) {
    auto result = path.wstring();
    std::transform(result.begin(), result.end(), result.begin(), ::towlower);
    return result;
}

bool is_leaf_dll(const std::filesystem::path& path) {
    const auto name = path.wstring();
    if (name.empty()) {
        return false;
    }
    if (name.find_first_of(L"/\\:*?\"<>|") != std::wstring::npos) {
        return false;
    }
    if (name.find(L'\0') != std::wstring::npos) {
        return false;
    }
    // Windows silently strips trailing spaces and dots from file names.
    if (name.back() == L' ' || name.back() == L'.') {
        return false;
    }
    return path.filename() == path && path_key(path.extension()) == L".dll";
}

bool accepts_registrations(const Plugin& plugin) {
    return plugin.state == PluginState::Loading || plugin.state == PluginState::Active;
}

bool depends_on(const Plugin& consumer, const std::wstring& provider_key) {
    return std::find(consumer.dependencies.begin(), consumer.dependencies.end(), provider_key) !=
           consumer.dependencies.end();
}

bool wait_until_idle(std::unique_lock<std::mutex>& lock, Plugin& plugin,
                     std::chrono::milliseconds timeout) {
    return plugin.drained.wait_for(lock, timeout, [&] { return plugin.in_flight == 0; });
}

} // namespace ark::core::plugin_detail
