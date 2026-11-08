#include <algorithm>
#include <chrono>
#include <exception>
#include <map>
#include <utility>

#include "plugin_manager.h"
#include "plugin_support.h"

namespace ark::core {

using namespace plugin_detail;

namespace {

constexpr auto kWorkerPollInterval = std::chrono::milliseconds(25);
#if defined(ARKHELP_ENABLE_TEST_MODULES)
constexpr auto kDirectoryScanInterval = std::chrono::milliseconds(500);
#endif

} // namespace

// File stamps observed by the auto-reload directory poll. A changed stamp must
// be seen twice in a row before a reload is requested, so a DLL that is still
// being written is not loaded half-finished.
#if defined(ARKHELP_ENABLE_TEST_MODULES)
struct PluginManager::DirectoryWatch {
    using Stamp = std::pair<std::filesystem::file_time_type, uintmax_t>;

    std::map<std::string, Stamp> seen;
    std::map<std::string, Stamp> pending;
    std::chrono::steady_clock::time_point next_scan = std::chrono::steady_clock::now();
};
#endif

void PluginManager::request(std::string operation, std::string filename) {
#if defined(ARKHELP_ENABLE_TEST_MODULES)
    if (!test_host_) { return; }
    if (operation != "scan" && !is_leaf_dll(from_utf8(filename))) {
        return;
    }
    std::lock_guard lock(mutex_);
    requests_.push_back({std::move(operation), std::move(filename)});
#endif
}

void PluginManager::scan() {
    if (!builtins_.empty()) {
        for (const auto& module : builtins_) {
            load_now(from_utf8(module.filename));
        }
        return;
    }
#if defined(ARKHELP_ENABLE_TEST_MODULES)
    if (!test_host_) { return; }
    for (auto& file : std::filesystem::directory_iterator(root_ / "plugins")) {
        if (file.is_regular_file() && is_leaf_dll(file.path().filename())) {
            load_now(file.path().filename());
        }
    }
#endif
}

void PluginManager::start() {
    if (running_.exchange(true)) {
        return;
    }
    worker_ = std::thread([this] {
        try {
            run();
        } catch (const std::exception& error) {
            log(kLogError, std::string("Plugin worker: ") + error.what());
            running_ = false;
        }
    });
}

void PluginManager::run() {
    scan();
#if defined(ARKHELP_ENABLE_TEST_MODULES)
    DirectoryWatch watch;
#endif
    while (running_) {
#if defined(ARKHELP_ENABLE_TEST_MODULES)
        process_requests();
        if (builtins_.empty() && auto_reload && std::chrono::steady_clock::now() >= watch.next_scan) {
            poll_plugin_directory(watch);
        }
#endif
        std::this_thread::sleep_for(kWorkerPollInterval);
    }
}

#if defined(ARKHELP_ENABLE_TEST_MODULES)
void PluginManager::process_requests() {
    std::deque<Request> requests;
    {
        std::lock_guard lock(mutex_);
        requests.swap(requests_);
    }
    for (size_t index = 0; index < requests.size();) {
        const auto& [operation, filename] = requests[index];
        if (operation == "reload") {
            index = process_reload_batch(requests, index);
            continue;
        }
        if (operation == "scan") {
            scan();
        } else if (operation == "load") {
            load_now(from_utf8(filename));
        } else if (operation == "unload") {
            unload_now(filename);
        }
        ++index;
    }
}

// Coalesces consecutive reload requests starting at `begin` and returns the
// index after them. Providers are reloaded first so a single accepted or
// refused cascade covers its queued consumers.
size_t PluginManager::process_reload_batch(const std::deque<Request>& requests, size_t begin) {
    size_t end = begin;
    std::map<std::wstring, std::string> wanted;
    while (end < requests.size() && requests[end].operation == "reload") {
        wanted[path_key(from_utf8(requests[end].filename))] = requests[end].filename;
        ++end;
    }

    std::map<std::wstring, std::vector<std::wstring>> graph;
    {
        std::lock_guard lock(mutex_);
        for (auto& plugin : plugins_) {
            std::lock_guard owner_lock(plugin->mutex);
            if (plugin->state != PluginState::Loading) {
                graph[plugin->key] = plugin->dependencies;
            }
        }
    }

    std::map<std::wstring, bool> visited;
    std::vector<std::wstring> providers_first;
    auto visit = [&](auto&& self, const std::wstring& name) -> void {
        if (visited[name]) {
            return;
        }
        visited[name] = true;
        if (auto found = graph.find(name); found != graph.end()) {
            for (auto& dependency : found->second) {
                self(self, dependency);
            }
        }
        if (wanted.contains(name)) {
            providers_first.push_back(name);
        }
    };
    for (auto& [name, filename] : wanted) {
        visit(visit, name);
    }

    std::vector<std::wstring> handled;
    for (auto& name : providers_first) {
        if (std::find(handled.begin(), handled.end(), name) == handled.end()) {
            reload_now(wanted.at(name), kRundownTimeoutMs, &handled);
        }
    }
    return end;
}

void PluginManager::poll_plugin_directory(DirectoryWatch& watch) {
    watch.next_scan = std::chrono::steady_clock::now() + kDirectoryScanInterval;
    std::map<std::string, DirectoryWatch::Stamp> current;
    std::error_code error;
    for (auto& file : std::filesystem::directory_iterator(root_ / "plugins", error)) {
        if (!file.is_regular_file() || !is_leaf_dll(file.path().filename())) {
            continue;
        }
        const auto name = to_utf8(file.path().filename());
        const DirectoryWatch::Stamp stamp{file.last_write_time(), file.file_size()};
        current[name] = stamp;
        if (!watch.seen.contains(name)) {
            watch.seen[name] = stamp;
            request("load", name);
        } else if (watch.seen[name] != stamp) {
            if (watch.pending.contains(name) && watch.pending[name] == stamp) {
                request("reload", name);
                watch.seen[name] = stamp;
                watch.pending.erase(name);
            } else {
                watch.pending[name] = stamp;
            }
        }
    }
    for (auto& [name, stamp] : watch.seen) {
        if (!current.contains(name)) {
            request("unload", name);
        }
    }
    std::erase_if(watch.seen, [&](const auto& item) { return !current.contains(item.first); });
}
#endif

} // namespace ark::core
