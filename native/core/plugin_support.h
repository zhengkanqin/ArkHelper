// Internal helpers shared by the plugin manager translation units.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

#include "plugin_manager.h"

namespace ark::core::plugin_detail {

inline constexpr uint32_t kLogInfo = 1;
inline constexpr uint32_t kLogWarning = 2;
inline constexpr uint32_t kLogError = 3;

// Bound for every internal rundown that has no caller-provided timeout.
inline constexpr uint32_t kRundownTimeoutMs = 3000;
inline constexpr std::chrono::milliseconds kRundownTimeout{kRundownTimeoutMs};

std::string to_utf8(const std::filesystem::path& path);
std::filesystem::path from_utf8(const std::string& text);

// Case-insensitive identity of a path (lower-cased wide string).
std::wstring path_key(const std::filesystem::path& path);

// True for a bare "*.dll" file name that cannot escape the plugin directory.
bool is_leaf_dll(const std::filesystem::path& path);

// Requires plugin.mutex. Only loading or active plugins may register anything.
bool accepts_registrations(const Plugin& plugin);

bool depends_on(const Plugin& consumer, const std::wstring& provider_key);

// Waits until no invocation is in flight. `lock` must own plugin.mutex and still
// owns it on return, so a timeout can be recorded atomically with the check.
bool wait_until_idle(std::unique_lock<std::mutex>& lock, Plugin& plugin,
                     std::chrono::milliseconds timeout);

} // namespace ark::core::plugin_detail
