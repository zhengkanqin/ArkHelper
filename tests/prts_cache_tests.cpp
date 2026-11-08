// Real client/real cache, isolated temporary directories and a deterministic HTTP stub.
#include "net/prts.h"
#include "net/http.h"
#include <windows.h>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

namespace {
using arkhelp::net::PrtsDiskCache;
using Kind = PrtsDiskCache::Kind;
std::mutex transport_mutex;
std::condition_variable transport_cv;
std::map<std::wstring, unsigned> requests, failures_left;
std::set<std::wstring> blocked;
unsigned checks = 0, failures = 0;
const std::string page = R"({"parse":{"wikitext":"{{敌人信息/common2|名称=测试|描述=已缓存资料}}{{敌人信息/levelcontent|index=0|最大生命值=1234}}"}})";
std::string image = "fixture image bytes";

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL %s\n", message); }
}
bool until(const std::function<bool()>& predicate) {
    const auto end = GetTickCount64() + 4000;
    while (GetTickCount64() < end) { if (predicate()) return true; Sleep(1); }
    return false;
}
unsigned count(const std::wstring& key) { std::lock_guard lock(transport_mutex); return requests[key]; }
bool take(arkhelp::net::PrtsClient& client, const std::wstring& expected, arkhelp::net::PrtsResult& result) {
    return until([&] {
        std::wstring name;
        while (client.poll(name, result)) if (name == expected) return true;
        return false;
    });
}
bool take_image(arkhelp::net::PrtsClient& client, const std::wstring& expected,
                std::vector<unsigned char>& bytes, std::wstring& error) {
    return until([&] {
        std::wstring name;
        while (client.poll_image(name, bytes, &error)) if (name == expected) return true;
        return false;
    });
}
PrtsDiskCache::Entry bytes(const std::string& value, std::uint64_t age = 0) {
    return {PrtsDiskCache::now_ms() - age, std::vector<unsigned char>(value.begin(), value.end())};
}
std::uint64_t size(const std::filesystem::path& directory) {
    std::uint64_t total = 0;
    for (const auto& item : std::filesystem::directory_iterator(directory))
        if (item.is_regular_file()) total += item.file_size();
    return total;
}
std::filesystem::path only_file(const std::filesystem::path& directory) {
    for (const auto& item : std::filesystem::directory_iterator(directory))
        if (item.path().extension() == L".cache") return item.path();
    throw std::runtime_error("cache fixture is missing");
}
void seed(const std::filesystem::path& directory, Kind kind, const std::wstring& name,
          const PrtsDiskCache::Entry& value) {
    PrtsDiskCache cache;
    cache.configure(directory, PrtsDiskCache::kMinBytes);
    cache.write(kind, name, value, cache.generation(kind, name));
}
}

namespace arkhelp::net {
std::string url_encode(const std::string& value) { return value; }
bool http_get(const std::wstring& url, std::string& body, std::string& error, const HttpOptions& options) {
    const bool avatar = url.find(L"action=query") != std::wstring::npos;
    const auto offset = url.find(avatar ? L"&titles=" : L"&page=");
    const auto key = offset == std::wstring::npos ? L"image-bytes" :
        (avatar ? L"image:" : L"") + url.substr(offset + (avatar ? 8 : 6));
    std::unique_lock lock(transport_mutex);
    ++requests[key];
    while (blocked.count(key)) {
        if (options.cancelled && options.cancelled->load()) { error = "cancelled"; return false; }
        transport_cv.wait_for(lock, std::chrono::milliseconds(2));
    }
    if (failures_left[key]) { --failures_left[key]; error = "offline fixture"; return false; }
    if (key == L"image-bytes") body = image;
    else if (avatar) body = R"({"source":"https://fixture.invalid/image.png"})";
    else body = page;
    return true;
}
}

int main() {
    using namespace arkhelp::net;
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temporary)) return 2;
    const auto parent = std::filesystem::weakly_canonical(std::filesystem::path(temporary));
    const auto root = parent / (L"ArkHelper-PRTS-cache-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    try {
        PrtsResult result;
        {
            const auto legacy = root / L"old-markup-cache";
            seed(legacy, Kind::Page, L"old-markup", bytes(R"({"parse":{"wikitext":"{{敌人信息/common2|描述=缓存第一行<br>第二行<!-- JSON | broken= }} -->}}{{敌人信息/levelcontent|index=0|最大生命值=1234}}"}})"));
            PrtsClient client;
            client.configure_cache(legacy, PrtsDiskCache::kMinBytes);
            client.request(L"old-markup");
            check(take(client, L"old-markup", result) && result.ok && result.basic.size() == 1 &&
                result.basic.front().value == L"缓存第一行\n第二行" && count(L"old-markup") == 0,
                "old raw-response disk caches are reparsed with fixed markup handling without network refetch");
        }
        std::vector<unsigned char> avatar;
        std::wstring error;
        const auto directory = root / L"中文 空格";
        const std::wstring unicode = L"卸石苦力/../中文";
        {
            PrtsClient client;
            client.configure_cache(directory, PrtsDiskCache::kDefaultBytes);
            check(until([&] { return std::filesystem::exists(directory); }) && size(directory) == 0,
                "configuration creates an empty cache without prefetching data");
            client.request(unicode);
            check(take(client, unicode, result) && result.ok && !result.stale, "Unicode page is fetched and parsed");
            client.request_image(unicode);
            check(take_image(client, unicode, avatar, error) && !avatar.empty(), "avatar fetched on demand");
        }
        check(std::distance(std::filesystem::directory_iterator(directory), {}) == 2,
            "only the visited page and avatar are persisted with flat safe filenames");
        {
            PrtsClient client;
            client.configure_cache(directory, PrtsDiskCache::kDefaultBytes);
            client.request(unicode);
            client.request_image(unicode);
            check(take(client, unicode, result) && result.ok && !result.stale && count(unicode) == 1,
                "page survives client restart and avoids the network");
            check(take_image(client, unicode, avatar, error) && !avatar.empty() && count(L"image:" + unicode) == 1,
                "avatar survives client restart and avoids both network requests");
            client.request(unicode, true);
            check(take(client, unicode, result) && result.ok && count(unicode) == 2,
                "explicit page refresh bypasses disk and memory caches");
        }
        const auto stale_pages = root / L"stale-pages";
        seed(stale_pages, Kind::Page, L"stale", bytes(page, PrtsDiskCache::kPageTtlMs + 1000));
        {
            PrtsClient client;
            client.configure_cache(stale_pages, PrtsDiskCache::kMinBytes);
            { std::lock_guard lock(transport_mutex); failures_left[L"stale"] = 1; }
            client.request(L"stale");
            check(take(client, L"stale", result) && result.ok && result.stale && !result.error.empty(),
                "expired page refresh failure returns marked usable offline content");
            client.request(L"stale");
            check(take(client, L"stale", result) && result.ok && result.stale && count(L"stale") == 1,
                "reselecting expired offline data respects the 60-second automatic refresh backoff");
            { std::lock_guard lock(transport_mutex); failures_left[L"stale"] = 1; }
            client.request(L"stale", true);
            check(take(client, L"stale", result) && !result.ok && !result.stale && !result.error.empty(),
                "explicit refresh exposes failure and never silently falls back");
            client.request(L"stale");
            check(take(client, L"stale", result) && result.ok && !result.stale && count(L"stale") == 3,
                "next normal request retries an expired page and replaces it on success");
        }
        {
            PrtsClient client;
            client.configure_cache(stale_pages, PrtsDiskCache::kMinBytes);
            client.request(L"stale");
            check(take(client, L"stale", result) && result.ok && count(L"stale") == 3,
                "successful refresh persists a new TTL across restart");
        }
        const auto stale_images = root / L"stale-images";
        seed(stale_images, Kind::Image, L"old-image", bytes("old bytes", PrtsDiskCache::kImageTtlMs + 1000));
        {
            PrtsClient client;
            client.configure_cache(stale_images, PrtsDiskCache::kMinBytes);
            { std::lock_guard lock(transport_mutex); failures_left[L"image:old-image"] = 1; }
            client.request_image(L"old-image");
            check(take_image(client, L"old-image", avatar, error) &&
                  std::string(avatar.begin(), avatar.end()) == "old bytes" && !error.empty(),
                "expired avatar is usable offline and exposes its refresh failure");
            client.request_image(L"old-image");
            check(take_image(client, L"old-image", avatar, error) &&
                  std::string(avatar.begin(), avatar.end()) == "old bytes" && count(L"image:old-image") == 1,
                "reselecting an offline avatar respects the 60-second automatic refresh backoff");
        }
        const auto invalidated = root / L"invalidated";
        {
            PrtsClient client;
            client.configure_cache(invalidated, PrtsDiskCache::kMinBytes);
            client.request_image(L"decode");
            check(take_image(client, L"decode", avatar, error) && !avatar.empty(), "avatar decoder fixture downloaded");
            client.invalidate_image(L"decode");
        }
        check(size(invalidated) == 0, "decoder invalidation removes the persistent avatar before shutdown completes");
        {
            PrtsClient client;
            client.configure_cache(invalidated, PrtsDiskCache::kMinBytes);
            client.request_image(L"decode");
            check(take_image(client, L"decode", avatar, error) && count(L"image:decode") == 2,
                "decoder-invalid bytes cannot reappear after restart");
        }
        for (int damage = 0; damage < 6; ++damage) {
            const auto bad = root / (L"corrupt-" + std::to_wstring(damage));
            seed(bad, Kind::Page, L"corrupt", bytes(page));
            const auto path = only_file(bad);
            {
                std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
                if (damage == 0) { file.seekp(-1, std::ios::end); file.put('?'); }
                if (damage == 1) { file.seekp(24); for (int i = 0; i < 8; ++i) file.put('\xff'); }
                if (damage == 2) { file.seekp(0); file.put('!'); }
                if (damage == 4) { file.seekp(16); for (int i = 0; i < 8; ++i) file.put('\xff'); }
                if (damage == 5) { file.seekp(40); file.put('\xff'); }
            }
            if (damage == 3) std::filesystem::resize_file(path, 17);
            const auto before = count(L"corrupt");
            PrtsClient client;
            client.configure_cache(bad, PrtsDiskCache::kMinBytes);
            client.request(L"corrupt");
            check(take(client, L"corrupt", result) && result.ok && count(L"corrupt") == before + 1,
                "corrupt checksum/length/magic, truncation, future timestamp or invalid UTF8 is rejected");
        }
        const auto invalid_page = root / L"invalid-page";
        {
            seed(invalid_page, Kind::Page, L"not-a-page", bytes("{\"wikitext\":\"bad\"}"));
            PrtsClient client;
            client.configure_cache(invalid_page, PrtsDiskCache::kMinBytes);
            client.request(L"not-a-page");
            check(take(client, L"not-a-page", result) && result.ok && count(L"not-a-page") == 1,
                "checksum-valid but unparseable page is removed and fetched again");
        }
        {
            PrtsClient client;
            client.configure_cache(invalid_page, PrtsDiskCache::kMinBytes);
            client.request(L"not-a-page");
            check(take(client, L"not-a-page", result) && result.ok && count(L"not-a-page") == 1,
                "replacement of an unparseable cached page is itself persisted");
        }
        {
            const auto lru = root / L"LRU";
            const auto entry = bytes(std::string(3 * PrtsDiskCache::kMiB, 'x'));
            PrtsDiskCache cache;
            cache.configure(lru, 0); // The public minimum is enforced.
            for (const auto* name : {L"A", L"B", L"C"}) {
                cache.write(Kind::Image, name, entry, cache.generation(Kind::Image, name)); Sleep(2);
            }
            PrtsDiskCache::Entry found;
            check(cache.read(Kind::Image, L"A", found), "LRU fixture touches A after B and C");
            Sleep(2);
            cache.write(Kind::Image, L"D", entry, cache.generation(Kind::Image, L"D"));
            check(!cache.read(Kind::Image, L"B", found) && cache.read(Kind::Image, L"A", found) &&
                  cache.read(Kind::Image, L"C", found) && cache.read(Kind::Image, L"D", found),
                "combined cache evicts least recently used data before writing");
            check(size(lru) <= PrtsDiskCache::kMinBytes && cache.size_bytes() == size(lru),
                "header, name and payload bytes all count toward the minimum budget");
            const auto generation = cache.generation(Kind::Image, L"A");
            cache.erase_async(Kind::Image, L"A");
            cache.write(Kind::Image, L"A", entry, generation);
            check(!cache.read(Kind::Image, L"A", found), "in-flight write cannot resurrect a decoder-invalidated entry");
        }
        {
            const auto shrink = root / L"shrink";
            PrtsDiskCache cache;
            cache.configure(shrink, 30 * PrtsDiskCache::kMiB);
            const auto entry = bytes(std::string(6 * PrtsDiskCache::kMiB, 'x'));
            cache.write(Kind::Image, L"one", entry, cache.generation(Kind::Image, L"one"));
            cache.write(Kind::Image, L"two", entry, cache.generation(Kind::Image, L"two"));
            cache.write(Kind::Image, L"three", entry, cache.generation(Kind::Image, L"three"));
            check(cache.size_bytes() > PrtsDiskCache::kMinBytes, "budget-shrink fixture has existing cached data");
            const auto started = GetTickCount64();
            cache.configure(shrink, PrtsDiskCache::kMinBytes);
            check(GetTickCount64() - started < 100 && until([&] { return cache.size_bytes() <= PrtsDiskCache::kMinBytes; }),
                "budget reduction publishes quickly and background cleanup needs no future request");
            check(size(shrink) <= PrtsDiskCache::kMinBytes, "shrunk budget applies to actual persistent bytes");
        }
        {
            const auto persistent_lru = root / L"persistent-LRU";
            const auto entry = bytes(std::string(3 * PrtsDiskCache::kMiB, 'p'));
            {
                PrtsDiskCache cache;
                cache.configure(persistent_lru, PrtsDiskCache::kMinBytes);
                cache.write(Kind::Page, L"page", entry, cache.generation(Kind::Page, L"page")); Sleep(2);
                cache.write(Kind::Image, L"old-image", entry, cache.generation(Kind::Image, L"old-image")); Sleep(2);
                cache.write(Kind::Image, L"new-image", entry, cache.generation(Kind::Image, L"new-image")); Sleep(2);
                cache.touch_async(Kind::Page, L"page");
            }
            PrtsDiskCache cache;
            cache.configure(persistent_lru, PrtsDiskCache::kMinBytes);
            cache.write(Kind::Image, L"latest", entry, cache.generation(Kind::Image, L"latest"));
            PrtsDiskCache::Entry found;
            check(cache.read(Kind::Page, L"page", found) && !cache.read(Kind::Image, L"old-image", found),
                "page and image share one LRU budget and background touches survive restart");
        }
        {
            PrtsDiskCache cache;
            const auto first = root / L"first-cache", second = root / L"second-cache";
            cache.configure(first, PrtsDiskCache::kMinBytes);
            const auto before = cache.generation(Kind::Page, L"unrelated");
            cache.erase_async(Kind::Image, L"bad-avatar");
            cache.write(Kind::Page, L"unrelated", bytes(page), before);
            PrtsDiskCache::Entry found;
            check(cache.read(Kind::Page, L"unrelated", found),
                "invalidating an avatar does not discard an unrelated in-flight page cache write");
            cache.configure(second, PrtsDiskCache::kMinBytes);
            cache.write(Kind::Page, L"unrelated", bytes(page), before);
            check(!cache.read(Kind::Page, L"unrelated", found) && size(first) != 0,
                "obsolete in-flight write cannot migrate an old request into a new cache directory");
        }
        {
            const auto explicit_image = root / L"explicit-image";
            seed(explicit_image, Kind::Image, L"force-image", bytes("known rejected bytes"));
            PrtsClient client;
            client.configure_cache(explicit_image, PrtsDiskCache::kMinBytes);
            { std::lock_guard lock(transport_mutex); failures_left[L"image:force-image"] = 1; }
            client.request_image(L"force-image", true);
            check(take_image(client, L"force-image", avatar, error) && avatar.empty() && !error.empty(),
                "explicit avatar refresh never restores old bytes after a network failure");
            client.shutdown();
            check(size(explicit_image) == 0, "explicit avatar refresh discards rejected persistent bytes");
        }
        {
            const auto path = root / L"blocked-directory";
            std::ofstream(path) << "not a directory";
            PrtsClient client;
            client.configure_cache(path, PrtsDiskCache::kMinBytes);
            client.request(L"cache-failure");
            check(take(client, L"cache-failure", result) && result.ok,
                "unwritable cache path never turns a successful request into a failure");
        }
        {
            PrtsClient client;
            client.configure_cache(root / L"in-flight", PrtsDiskCache::kMinBytes);
            { std::lock_guard lock(transport_mutex); blocked.insert(L"merged"); }
            client.request(L"merged");
            check(until([&] { return count(L"merged") == 1; }), "persistent request fixture is in flight");
            for (int i = 0; i < 20; ++i) client.request(L"merged");
            { std::lock_guard lock(transport_mutex); blocked.erase(L"merged"); }
            transport_cv.notify_all();
            check(take(client, L"merged", result) && result.ok && count(L"merged") == 1,
                "persistent cache keeps in-flight duplicate merging");
        }
    } catch (const std::exception& error) {
        check(false, error.what());
    }
    const auto resolved = std::filesystem::weakly_canonical(root);
    if (resolved.parent_path() == parent && resolved.filename() == root.filename())
        std::filesystem::remove_all(resolved);
    else check(false, "refusing cleanup outside the verified test fixture root");
    std::printf("PRTS persistent cache: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
