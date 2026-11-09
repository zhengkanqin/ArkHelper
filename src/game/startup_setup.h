#pragma once
#include <filesystem>
#include <string>

namespace arkhelp::startup {
// Called only after the first-use notice. Returns an empty string on success.
std::wstring prepare(const std::filesystem::path& root, bool repair = false, bool first_use = false);
bool registered_for(const std::filesystem::path& root);
std::wstring create_shortcut(const std::filesystem::path& root,
                             const std::filesystem::path& destination = {});
}
