// PRTS WIKI 敌人页拉取（后台线程）。
//
// 页面 wikitext 里有两类模板：
//   {{敌人信息/common2|id=…|名称=…|地位级别=…|描述=…|伤害类型=…|…}}
//   {{敌人信息/levelcontent|index=0|最大生命值=…|攻击力=…|…}}   （index>0 只写与 0 级的差异）
// 这里解析 common2 基础信息与所有 levelcontent（按 index 逐级继承），交给 WIKI 面板以表格显示。
// 网络失败、页面不存在、解析失败都只表现为 Result.ok=false，不抛异常到 UI 线程。
#pragma once

#include <condition_variable>
#include <filesystem>
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "net/prts_cache.h"

namespace arkhelp::net
{

struct PrtsField
{
    std::wstring key;
    std::wstring value;
};

struct PrtsLevel
{
    int index = 0;
    std::vector<PrtsField> fields; // 已按 index 逐级继承合并后的字段
};

struct PrtsResult
{
    bool ok = false;
    bool stale = false; // Expired on-disk content, used after an automatic refresh failed.
    std::wstring title;
    std::wstring error;
    std::vector<PrtsField> basic;      // common2 的基础信息
    std::vector<PrtsLevel> levels;     // 级别0/1/…（继承合并后的数值表）
    std::vector<PrtsField> fields;     // 兼容字段：basic + 级别0
};

class PrtsClient
{
public:
    PrtsClient() = default;
    ~PrtsClient();

    PrtsClient(const PrtsClient&) = delete;
    PrtsClient& operator=(const PrtsClient&) = delete;

    // Thread-safe; publishes settings for background I/O. Empty directory disables disk
    // caching; max_bytes is clamped to 10..5120 MiB. Existing files survive process restarts.
    void configure_cache(std::filesystem::path directory, std::uint64_t max_bytes);

    // Decoder rejected these bytes: clear memory/disk state and let the next request retry.
    // This method only queues disk work, so it is safe on the UI thread.
    void invalidate_image(const std::wstring& enemy_name);

    // 请求资料；同名在途请求合并。成功资料按需持久化，refresh绕过缓存，失败可直接重试。
    void request(const std::wstring& enemy_name, bool refresh = false);

    // UI 线程用while排空终态结果；失败也返回true且error非空。按name匹配当前选择。
    bool poll(std::wstring& name, PrtsResult& out);

    // 请求某个敌人页的头像缩略图（PRTS pageimages），结果按敌人名缓存。
    // False means a short failure cooldown; callers must not mark it as in flight.
    bool request_image(const std::wstring& enemy_name, bool refresh = false, bool priority = false);

    // 头像成功/失败均返回true；失败bytes为空，error可选。失败2秒后允许重试。
    bool poll_image(std::wstring& name, std::vector<unsigned char>& image_bytes,
                    std::wstring* error = nullptr);

    void shutdown();

private:
    void worker_loop();
    PrtsResult fetch(const std::wstring& enemy_name, const std::atomic<bool>* cancelled,
                     bool refresh, std::uint64_t& fetched_at);
    static PrtsResult parse(const std::wstring& enemy_name, const std::string& body);

    void image_worker_loop();
    bool fetch_image(const std::wstring& enemy_name, std::vector<unsigned char>& bytes,
                     std::wstring& error, const std::atomic<bool>* cancelled,
                     bool refresh, std::uint64_t& fetched_at);
    bool download_image(const std::wstring& enemy_name, std::vector<unsigned char>& bytes,
                        std::wstring& error, const std::atomic<bool>* cancelled);

    PrtsDiskCache disk_cache_;

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false;
    std::wstring pending_name_;
    bool pending_refresh_ = false;
    std::wstring active_name_;
    std::shared_ptr<std::atomic<bool>> active_cancel_;
    std::map<std::wstring, PrtsResult> ready_;
    struct CachedPage { PrtsResult result; std::uint64_t fetched_at = 0, used = 0; };
    std::map<std::wstring, CachedPage> cache_;
    struct FailedRefresh { std::uint64_t retry_at = 0; std::wstring error; };
    std::map<std::wstring, FailedRefresh> page_refresh_failed_;

    std::array<std::thread, 2> image_workers_;
    std::condition_variable image_cv_;
    bool image_stopping_ = false;
    std::vector<std::wstring> image_queue_;
    std::set<std::wstring> image_refresh_;
    std::map<std::wstring, std::shared_ptr<std::atomic<bool>>> image_active_;
    struct CachedImage {
        std::vector<unsigned char> bytes;
        std::uint64_t fetched_at = 0, used = 0;
        std::wstring error;
    };
    std::map<std::wstring, CachedImage> image_cache_;
    std::map<std::wstring, FailedRefresh> image_refresh_failed_;
    struct ImageResult { std::vector<unsigned char> bytes; std::wstring error; };
    std::map<std::wstring, ImageResult> image_ready_;
    std::map<std::wstring, std::uint64_t> image_failed_until_;
};

}  // namespace arkhelp::net
