#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace arkhelp::net {

// All filesystem operations run on caller-owned background workers or the maintenance
// thread. configure/touch_async/erase_async only publish work and never access the disk.
class PrtsDiskCache {
public:
    enum class Kind : std::uint32_t { Page = 1, Image = 2 };
    struct Entry {
        std::uint64_t fetched_at_ms = 0;
        std::vector<unsigned char> bytes;
    };
    static constexpr std::uint64_t kMiB = 1024 * 1024;
    static constexpr std::uint64_t kMinBytes = 10 * kMiB;
    static constexpr std::uint64_t kDefaultBytes = 500 * kMiB;
    static constexpr std::uint64_t kMaxBytes = 5120 * kMiB;
    static constexpr std::uint64_t kPageTtlMs = 7ULL * 24 * 60 * 60 * 1000;
    static constexpr std::uint64_t kImageTtlMs = 30ULL * 24 * 60 * 60 * 1000;

    PrtsDiskCache() = default;
    ~PrtsDiskCache();
    PrtsDiskCache(const PrtsDiskCache&) = delete;
    PrtsDiskCache& operator=(const PrtsDiskCache&) = delete;

    void configure(std::filesystem::path directory, std::uint64_t max_bytes);
    void shutdown();
    std::uint64_t generation(Kind kind, const std::wstring& name) const;
    bool read(Kind kind, const std::wstring& name, Entry& entry);
    // A generation taken before a fetch prevents an invalidated/in-flight download from
    // restoring rejected bytes, or writing to an obsolete cache directory.
    void write(Kind kind, const std::wstring& name, const Entry& entry, std::uint64_t generation);
    void erase_async(Kind kind, const std::wstring& name);
    void touch_async(Kind kind, const std::wstring& name);
    std::uint64_t size_bytes() const { return bytes_.load(); }
    static std::uint64_t now_ms();
    static bool fresh(std::uint64_t fetched_at_ms, std::uint64_t ttl_ms);

private:
    using Key = std::pair<Kind, std::wstring>;
    struct File {
        std::uint64_t bytes = 0;
        std::filesystem::file_time_type used{};
    };
    struct Settings {
        std::filesystem::path directory;
        std::uint64_t max_bytes = kDefaultBytes;
        std::uint64_t version = 0;
    };
    void maintenance_loop();
    void apply_pending(); // Requires io_mutex_, never called by a UI entry point.
    void scan();
    void evict(std::uint64_t reserve = 0);
    void erase_file(const std::filesystem::path& path);
    std::filesystem::path file_path(Kind kind, const std::wstring& name) const;

    mutable std::mutex control_mutex_;
    std::condition_variable cv_;
    std::thread maintenance_;
    bool stopping_ = false;
    bool pending_ = false;
    Settings desired_;
    std::uint64_t generation_ = 0;
    std::uint64_t configuration_generation_ = 0;
    std::map<Key, std::uint64_t> invalidations_;
    std::set<Key> erases_;
    std::set<Key> touches_;

    std::mutex io_mutex_;
    Settings current_;
    std::map<std::filesystem::path, File> files_;
    std::atomic<std::uint64_t> bytes_{0};
};

} // namespace arkhelp::net
