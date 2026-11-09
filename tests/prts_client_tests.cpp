#include "net/prts.h"
#include "net/http.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

namespace {
std::mutex fake_mutex;
std::condition_variable fake_cv;
std::map<std::wstring, unsigned> calls, fail_next;
std::map<std::wstring, std::string> responses;
std::set<std::wstring> blocked;
std::vector<std::wstring> requested_urls;
unsigned checks = 0, failures = 0;
void check(bool passed, const char* label) {
    ++checks;
    if (!passed) { ++failures; std::printf("FAIL %s\n", label); }
}
bool until(const std::function<bool()>& condition) {
    const auto end = GetTickCount64()+2000;
    while (GetTickCount64() < end) {
        if (condition()) return true;
        Sleep(1);
    }
    return false;
}
unsigned count(const std::wstring& key) { std::lock_guard lock(fake_mutex); return calls[key]; }
bool take(arkhelp::net::PrtsClient& client, const std::wstring& expected, arkhelp::net::PrtsResult& result) {
    std::wstring name;
    return until([&] {
        while (client.poll(name, result)) if (name == expected) return true;
        return false;
    });
}
std::string valid = R"({ "parse" : { "wikitext" : "{{敌人信息/common2|名称=测试|描述=资料\\n说明|伤害类型=物理}}{{敌人信息/levelcontent|index=0|最大生命值=1000}}" } })";
}

namespace arkhelp::net {
// Deterministic transport: tests exercise the real request workers, parser, queues and cache.
std::string url_encode(const std::string& value) { return value; }
bool http_get(const std::wstring& url, std::string& body, std::string& error, const HttpOptions& options) {
    const bool image = url.find(L"action=query") != std::wstring::npos;
    const auto start = url.find(image ? L"&titles=" : L"&page=");
    const std::wstring name = start == std::wstring::npos ? L"image-bytes" : url.substr(start+(image ? 8 : 6));
    const std::wstring key = (image ? L"image:" : L"")+name;
    std::unique_lock lock(fake_mutex);
    requested_urls.push_back(url);
    ++calls[key];
    fake_cv.notify_all();
    while (blocked.count(key)) {
        if (options.cancelled && options.cancelled->load()) { error = "request cancelled"; return false; }
        fake_cv.wait_for(lock, std::chrono::milliseconds(5));
    }
    if (fail_next[key]) { --fail_next[key]; error = "simulated network failure"; return false; }
    const auto configured = responses.find(key);
    if (configured != responses.end()) body = configured->second;
    else body = image ? R"({"query":{"pages":[{"title":"no avatar"}]}})" : valid;
    return true;
}
}

int main() {
    using namespace arkhelp::net;
    PrtsResult result;
    {
        PrtsClient client;
        { std::lock_guard lock(fake_mutex);
          responses[L"markup"] = R"({"parse":{"wikitext":"<!-- {{敌人信息/common2|伪字段=错误}} -->{{敌人信息/common2\n | 名称 =测试\n | 描述 =第一段<br />第二段<!-- JSON blackboard {\"k\":\"| }} =\"} -->\n | 特性 =<span class=\"color\">{{color|red|[[目标|显示名称]]}}</span>&nbsp;&amp;&#x4E2D;\n | 伤害类型 =物理}}{{敌人信息/levelcontent|index=0|最大生命值=1000|技能=每秒<br>造成{{color|red|200}}点伤害}}{{敌人信息/levelcontent|index=1|最大生命值=2000}}"}})"; }
        client.request(L"markup");
        check(take(client, L"markup", result) && result.ok, "HTML comments cannot create fake templates or split parameters");
        const auto field = [&](const std::wstring& key) {
            for (const auto& item : result.basic) if (item.key == key) return item.value;
            return std::wstring{};
        };
        check(field(L"描述") == L"第一段\n第二段", "basic fields strip blackboard comments and preserve web line breaks");
        check(field(L"特性") == L"显示名称 &中", "nested templates, links, span tags and named/numeric entities render as text");
        check(field(L"伤害类型") == L"物理" && field(L"伪字段").empty(), "whitespace in parameter keys is trimmed without leaking commented data");
        check(result.levels.size() == 2 && result.levels.back().fields.size() == 2 &&
            result.levels.back().fields.back().value == L"每秒\n造成200点伤害", "level inheritance keeps sanitized multiline skills");
    }
    {
        PrtsClient client;
        client.request(L"A");
        check(take(client, L"A", result) && result.ok && !result.levels.empty(), "formatted JSON produces a complete parsed page");
        client.request(L"A"); // Cache completion must survive another network completion.
        client.request(L"B");
        std::set<std::wstring> ready;
        check(until([&] {
            std::wstring name;
            while (client.poll(name, result)) if (result.ok) ready.insert(name);
            return ready.count(L"A") && ready.count(L"B");
        }), "cached and background completions are not overwritten by one ready slot");
        check(count(L"A") == 1, "successful repeated request uses bounded cache");
    }
    {
        PrtsClient client;
        { std::lock_guard lock(fake_mutex); blocked.insert(L"slow"); }
        client.request(L"slow");
        check(until([&] { return count(L"slow") == 1; }), "slow page is in flight");
        for (int i = 0; i < 20; ++i) client.request(L"slow");
        { std::lock_guard lock(fake_mutex); blocked.erase(L"slow"); }
        fake_cv.notify_all();
        check(take(client, L"slow", result) && result.ok && count(L"slow") == 1,
              "duplicate in-flight requests do not fetch the same page twice");
    }
    {
        PrtsClient client;
        { std::lock_guard lock(fake_mutex); fail_next[L"retry"] = 1; }
        client.request(L"retry");
        check(take(client, L"retry", result) && !result.ok && !result.error.empty(), "network failure has a terminal nonempty error");
        client.request(L"retry");
        check(take(client, L"retry", result) && result.ok && count(L"retry") == 2, "failure is not permanently cached and retry can succeed");
        client.request(L"retry", true);
        check(take(client, L"retry", result) && result.ok && count(L"retry") == 3, "explicit refresh fetches a successful cached page again");
    }
    {
        PrtsClient client;
        { std::lock_guard lock(fake_mutex); blocked.insert(L"old"); }
        client.request(L"old");
        check(until([&] { return count(L"old") == 1; }), "old selection is in flight");
        const auto started = GetTickCount64();
        client.request(L"current");
        std::map<std::wstring, PrtsResult> ready;
        check(until([&] {
            std::wstring name;
            while (client.poll(name, result)) ready[name] = result;
            return ready.count(L"old") && ready.count(L"current");
        }) && ready[L"current"].ok && !ready[L"old"].ok && !ready[L"old"].error.empty() &&
            GetTickCount64()-started < 1000, "selection change cancels stale work and preserves both terminal results");
    }
    {
        PrtsClient client;
        { std::lock_guard lock(fake_mutex); blocked.insert(L"again"); }
        client.request(L"again");
        check(until([&] { return count(L"again") == 1; }), "first request active before reselecting its name");
        client.request(L"intermediate");
        client.request(L"again");
        check(until([&] { return count(L"again") == 2; }), "A to B to A starts the replacement A");
        bool stale = false;
        std::wstring name;
        while (client.poll(name, result)) if (name == L"again") stale = true;
        { std::lock_guard lock(fake_mutex); blocked.erase(L"again"); }
        fake_cv.notify_all();
        check(!stale && take(client, L"again", result) && result.ok,
              "cancelled first A cannot overwrite the current replacement A with an old error");
    }
    {
        PrtsClient client;
        { std::lock_guard lock(fake_mutex); responses[L"truncated"] = "{\"wikitext\":\"unfinished"; }
        client.request(L"truncated");
        check(take(client, L"truncated", result) && !result.ok && !result.error.empty(), "truncated JSON cannot be treated as a successful page");
        { std::lock_guard lock(fake_mutex); responses[L"api-error"] = R"({"error":{"info" : "Page does not exist"}})"; }
        client.request(L"api-error");
        check(take(client, L"api-error", result) && result.error.find(L"Page does not exist") != std::wstring::npos,
              "MediaWiki errors expose the API reason");
    }
    {
        PrtsClient client;
        { std::lock_guard lock(fake_mutex);
          responses[L"image:redirected-thumbnail"] = R"({"source":"https://media.prts.wiki/thumb/test.png/96px-test.png"})";
          responses[L"image-bytes"] = "thumbnail bytes"; }
        client.request_image(L"redirected-thumbnail");
        std::wstring name, error;
        std::vector<unsigned char> bytes;
        check(until([&] { return client.poll_image(name, bytes, &error); }) &&
              !bytes.empty() && error.empty(), "redirected PRTS thumbnail downloads successfully");
        { std::lock_guard lock(fake_mutex);
          check(std::find(requested_urls.begin(), requested_urls.end(), L"https://prts.wiki/thumb/test.png/96px-test.png") != requested_urls.end() &&
                std::find(requested_urls.begin(), requested_urls.end(), L"https://media.prts.wiki/thumb/test.png/96px-test.png") == requested_urls.end(),
                "media thumbnails use the canonical host without relying on HTTP redirects"); }
    }
    {
        PrtsClient client;
        { std::lock_guard lock(fake_mutex); blocked.insert(L"image:avatar"); }
        client.request_image(L"avatar");
        check(until([&] { return count(L"image:avatar") == 1; }), "avatar request is in flight");
        for (int i = 0; i < 10; ++i) client.request_image(L"avatar");
        client.request(L"independent");
        check(take(client, L"independent", result) && result.ok, "blocked avatar never blocks the page worker");
        { std::lock_guard lock(fake_mutex); blocked.erase(L"image:avatar"); }
        fake_cv.notify_all();
        std::wstring name, error;
        std::vector<unsigned char> bytes;
        check(until([&] { return client.poll_image(name, bytes, &error); }) && bytes.empty() && !error.empty(),
              "avatar failure is delivered as a terminal result");
        check(!client.request_image(L"avatar") && count(L"image:avatar") == 1,
              "cooldown rejects an unqueued image request so UI cannot strand it as in flight");
        check(client.request_image(L"avatar", true), "explicit avatar retry bypasses the failure cooldown");
        check(until([&] { return client.poll_image(name, bytes, &error); }) && count(L"image:avatar") == 2,
              "explicit avatar retry reaches the transport and returns a terminal result");
    }
    {
        PrtsClient client;
        { std::lock_guard lock(fake_mutex);
          responses[L"image:decode-retry"] = R"({"source":"https://images.example/fixture.png"})";
          responses[L"image-bytes"] = "bad-image"; }
        client.request_image(L"decode-retry");
        std::wstring name, error;
        std::vector<unsigned char> bytes;
        check(until([&] { return client.poll_image(name, bytes, &error); }) &&
              std::string(bytes.begin(), bytes.end()) == "bad-image", "downloaded image bytes reach the decoder");
        { std::lock_guard lock(fake_mutex); responses[L"image-bytes"] = "fixed-image"; }
        client.request_image(L"decode-retry", true);
        check(until([&] { return client.poll_image(name, bytes, &error); }) &&
              std::string(bytes.begin(), bytes.end()) == "fixed-image" && count(L"image:decode-retry") == 2,
              "decoder failure retry invalidates cached bad bytes and downloads a fresh image");
    }
    {
        PrtsClient client;
        { std::lock_guard lock(fake_mutex); blocked.insert(L"image:blocked-portrait"); }
        client.request_image(L"blocked-portrait");
        check(until([&] { return count(L"image:blocked-portrait") == 1; }), "first image worker is blocked");
        client.request_image(L"second-portrait");
        std::wstring name, error;
        std::vector<unsigned char> bytes;
        check(until([&] { return client.poll_image(name, bytes, &error); }) && name == L"second-portrait",
              "a slow portrait does not block every other enemy portrait");
        client.shutdown();
    }
    {
        PrtsClient client;
        { std::lock_guard lock(fake_mutex);
          blocked.insert(L"image:busy-one"); blocked.insert(L"image:busy-two");
          blocked.insert(L"image:selected-portrait"); }
        client.request_image(L"busy-one");
        client.request_image(L"busy-two");
        check(until([&] { return count(L"image:busy-one") && count(L"image:busy-two"); }),
              "both portrait workers are occupied");
        client.request_image(L"queued-portrait");
        client.request_image(L"selected-portrait", false, true);
        { std::lock_guard lock(fake_mutex); blocked.erase(L"image:busy-one"); }
        fake_cv.notify_all();
        check(until([&] { return count(L"image:selected-portrait") == 1; }) && count(L"image:queued-portrait") == 0,
              "selected enemy portrait is prioritized ahead of background queued images");
        client.shutdown();
    }
    {
        PrtsClient client;
        { std::lock_guard lock(fake_mutex); blocked.insert(L"shutdown"); blocked.insert(L"image:shutdown"); }
        client.request(L"shutdown");
        client.request_image(L"shutdown");
        check(until([&] { return count(L"shutdown") && count(L"image:shutdown"); }), "both workers active before shutdown");
        const auto started = GetTickCount64();
        client.shutdown();
        check(GetTickCount64()-started < 500, "shutdown cancels both workers before joining either one");
    }
    std::printf("PRTS client: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
