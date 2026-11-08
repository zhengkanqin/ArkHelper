#include "net/prts.h"

#include <windows.h>

#include "net/http.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

namespace arkhelp::net
{
namespace
{

template<class Map> void trim_memory_cache(Map& cache) {
    while (cache.size() > 32) {
        const auto oldest = std::min_element(cache.begin(), cache.end(), [](const auto& a, const auto& b) {
            return a.second.used < b.second.used;
        });
        cache.erase(oldest);
    }
}
bool cancelled(const std::atomic<bool>* value) { return value && value->load(); }
template<class Map> bool backing_off(const Map& failures, const std::wstring& name) {
    const auto found = failures.find(name);
    return found != failures.end() && GetTickCount64() < found->second.retry_at;
}
template<class Map> void bound_failures(Map& failures) {
    for (auto entry = failures.begin(); entry != failures.end();) {
        if (entry->second.retry_at <= GetTickCount64()) entry = failures.erase(entry);
        else ++entry;
    }
    if (failures.size() >= 64) {
        const auto oldest = std::min_element(failures.begin(), failures.end(), [](const auto& a, const auto& b) {
            return a.second.retry_at < b.second.retry_at;
        });
        failures.erase(oldest);
    }
}

std::wstring utf8_to_wide(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (length <= 0)
    {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string wide_to_utf8(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0)
    {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length,
                        nullptr, nullptr);
    return utf8;
}

void append_utf8(std::string& out, std::uint32_t code)
{
    if (code < 0x80)
    {
        out.push_back(static_cast<char>(code));
    }
    else if (code < 0x800)
    {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
    else if (code < 0x10000)
    {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9')
    {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f')
    {
        return 10 + ch - 'a';
    }
    if (ch >= 'A' && ch <= 'F')
    {
        return 10 + ch - 'A';
    }
    return -1;
}

// Read a JSON string with optional formatting whitespace, rejecting incomplete strings and
// malformed Unicode. A truncated response must become an error, never a cached partial page.
bool extract_json_string(const std::string& body, const char* key, std::string& out)
{
    out.clear();
    std::string pattern = "\"";
    pattern += key;
    pattern += "\"";
    std::size_t index = 0;
    for (;;) {
        const auto start = body.find(pattern, index);
        if (start == std::string::npos) return false;
        index = start + pattern.size();
        while (index < body.size() && std::isspace(static_cast<unsigned char>(body[index]))) ++index;
        if (index >= body.size() || body[index] != ':') continue;
        ++index;
        while (index < body.size() && std::isspace(static_cast<unsigned char>(body[index]))) ++index;
        if (index >= body.size() || body[index++] != '"') return false;
        break;
    }
    while (index < body.size())
    {
        const char ch = body[index++];
        if (ch == '"')
        {
            return true;
        }
        if (ch != '\\')
        {
            if (static_cast<unsigned char>(ch) < 0x20) return false;
            out.push_back(ch);
            continue;
        }
        if (index >= body.size())
        {
            break;
        }
        const char esc = body[index++];
        switch (esc)
        {
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 't':
            out.push_back('\t');
            break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case '"':
            out.push_back('"');
            break;
        case '\\':
            out.push_back('\\');
            break;
        case '/':
            out.push_back('/');
            break;
        case 'u':
        {
            if (index + 4 > body.size())
            {
                return false;
            }
            std::uint32_t code = 0;
            bool valid = true;
            for (int i = 0; i < 4; ++i)
            {
                const int value = hex_value(body[index + static_cast<std::size_t>(i)]);
                if (value < 0)
                {
                    valid = false;
                    break;
                }
                code = (code << 4) | static_cast<std::uint32_t>(value);
            }
            index += 4;
            if (!valid) return false;
            if (code >= 0xD800 && code <= 0xDBFF) {
                if (index + 6 > body.size() || body[index] != '\\' || body[index+1] != 'u') return false;
                index += 2;
                std::uint32_t low = 0;
                for (int i = 0; i < 4; ++i) {
                    const int value = hex_value(body[index++]);
                    if (value < 0) return false;
                    low = (low << 4) | static_cast<std::uint32_t>(value);
                }
                if (low < 0xDC00 || low > 0xDFFF) return false;
                code = 0x10000 + ((code - 0xD800) << 10) + low - 0xDC00;
            } else if (code >= 0xDC00 && code <= 0xDFFF) return false;
            append_utf8(out, code);
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

// 找到从 start 处 `{{` 开始、括号配平的模板内容（不含最外层括号）。
bool template_block(const std::wstring& text, std::size_t start, std::wstring& out)
{
    if (start + 2 > text.size() || text[start] != L'{' || text[start + 1] != L'{')
    {
        return false;
    }
    int depth = 0;
    std::size_t index = start;
    while (index + 1 < text.size())
    {
        if (text[index] == L'{' && text[index + 1] == L'{')
        {
            ++depth;
            index += 2;
            continue;
        }
        if (text[index] == L'}' && text[index + 1] == L'}')
        {
            --depth;
            index += 2;
            if (depth == 0)
            {
                out = text.substr(start + 2, index - start - 4);
                return true;
            }
            continue;
        }
        ++index;
    }
    return false;
}

// Comments can contain pipes, equals signs and even template braces (JSON blackboards).
// Remove them before looking for templates or splitting parameters.
std::wstring without_comments(const std::wstring& text)
{
    std::wstring out;
    for (std::size_t i = 0; i < text.size();) {
        if (text.compare(i, 4, L"<!--") == 0) {
            const auto end = text.find(L"-->", i + 4);
            i = end == std::wstring::npos ? text.size() : end + 3;
        } else out.push_back(text[i++]);
    }
    return out;
}

std::vector<std::pair<std::wstring, std::wstring>> parse_params(const std::wstring& block)
{
    std::vector<std::pair<std::wstring, std::wstring>> params;
    std::wstring current;
    int depth = 0, templates = 0;
    for (std::size_t i = 0; i < block.size(); ++i)
    {
        const wchar_t ch = block[i];
        if (i + 1 < block.size() && block.compare(i, 2, L"{{") == 0) {
            ++templates; current += L"{{"; ++i; continue;
        }
        if (i + 1 < block.size() && block.compare(i, 2, L"}}") == 0 && templates > 0) {
            --templates; current += L"}}"; ++i; continue;
        }
        if (ch == L'[' && depth >= 0)
        {
            ++depth;
        }
        else if (ch == L']' && depth > 0)
        {
            --depth;
        }
        if (ch == L'|' && depth == 0 && templates == 0)
        {
            const std::size_t eq = current.find(L'=');
            if (eq != std::wstring::npos)
            {
                params.emplace_back(current.substr(0, eq), current.substr(eq + 1));
            }
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    const std::size_t eq = current.find(L'=');
    if (eq != std::wstring::npos)
    {
        params.emplace_back(current.substr(0, eq), current.substr(eq + 1));
    }
    for (auto& param : params) {
        const auto first = param.first.find_first_not_of(L" \t\r\n");
        const auto last = param.first.find_last_not_of(L" \t\r\n");
        param.first = first == std::wstring::npos ? L"" : param.first.substr(first, last - first + 1);
    }
    return params;
}

void trim(std::wstring& text)
{
    const wchar_t* spaces = L" \t\r\n";
    const std::size_t first = text.find_first_not_of(spaces);
    if (first == std::wstring::npos)
    {
        text.clear();
        return;
    }
    const std::size_t last = text.find_last_not_of(spaces);
    text = text.substr(first, last - first + 1);
}


// Remove PRTS wikitext constructs so raw macro text never reaches the panel.
std::wstring sanitize_wikitext(const std::wstring& source)
{
    const auto text = without_comments(source);
    std::wstring out;
    std::size_t i = 0;
    while (i < text.size())
    {
        if (text[i] == L'<' && i + 1 < text.size() &&
            (std::iswalpha(text[i + 1]) || text[i + 1] == L'/')) {
            const auto end = text.find(L'>', i + 1);
            if (end != std::wstring::npos) {
                auto tag = text.substr(i + 1, end - i - 1);
                std::transform(tag.begin(), tag.end(), tag.begin(), [](wchar_t ch) { return std::towlower(ch); });
                const auto name_end = tag.find_first_of(L" /\t\r\n");
                const auto name = tag.substr(0, name_end);
                if (name == L"br" || name == L"p" || tag.starts_with(L"/p") ||
                    name == L"div" || tag.starts_with(L"/div") || name == L"li" || tag.starts_with(L"/li"))
                    out.push_back(L'\n');
                i = end + 1;
                continue;
            }
        }
        if (text[i] == L'&') {
            const auto end = text.find(L';', i + 1);
            if (end != std::wstring::npos && end - i <= 12) {
                const auto entity = text.substr(i + 1, end - i - 1);
                std::uint32_t code = 0;
                if (entity == L"nbsp") code = L' ';
                else if (entity == L"amp") code = L'&';
                else if (entity == L"lt") code = L'<';
                else if (entity == L"gt") code = L'>';
                else if (entity == L"quot") code = L'"';
                else if (entity == L"apos") code = L'\'';
                else if (entity == L"times") code = 0xD7;
                else if (entity == L"ndash") code = 0x2013;
                else if (entity == L"mdash") code = 0x2014;
                else if (entity == L"hellip") code = 0x2026;
                else if (entity.starts_with(L"#")) {
                    const bool hex = entity.size() > 1 && (entity[1] == L'x' || entity[1] == L'X');
                    wchar_t* tail = nullptr;
                    code = std::wcstoul(entity.c_str() + (hex ? 2 : 1), &tail, hex ? 16 : 10);
                    if (!tail || *tail) code = 0;
                }
                if (code && code <= 0x10FFFF && !(code >= 0xD800 && code <= 0xDFFF)) {
                    if (code > 0xFFFF) {
                        out.push_back(static_cast<wchar_t>(0xD800 + ((code - 0x10000) >> 10)));
                        out.push_back(static_cast<wchar_t>(0xDC00 + ((code - 0x10000) & 0x3FF)));
                    } else out.push_back(static_cast<wchar_t>(code));
                    i = end + 1; continue;
                }
            }
        }
        if (i + 1 < text.size() && text[i] == L'{' && text[i + 1] == L'{')
        {
            std::size_t depth = 0;
            std::size_t j = i;
            for (; j + 1 < text.size(); ++j)
            {
                if (text[j] == L'{' && text[j + 1] == L'{')
                {
                    ++depth;
                    ++j;
                }
                else if (text[j] == L'}' && text[j + 1] == L'}')
                {
                    --depth;
                    ++j;
                    if (depth == 0)
                    {
                        break;
                    }
                }
            }
            if (depth != 0)
            {
                i += 2;
                continue;
            }
            const std::wstring inner = text.substr(i + 2, j - i - 3);
            std::wstring visible;
            int nesting = 0, links = 0;
            std::size_t start = 0;
            for (std::size_t k = 0; k <= inner.size(); ++k)
            {
                const wchar_t ch = k < inner.size() ? inner[k] : L'|';
                if (ch == L'[') ++links;
                else if (ch == L']' && links > 0) --links;
                if (k + 1 < inner.size() && inner[k] == L'{' && inner[k + 1] == L'{')
                {
                    ++nesting;
                }
                else if (k + 1 < inner.size() && inner[k] == L'}' && inner[k + 1] == L'}')
                {
                    --nesting;
                }
                if (ch == L'|' && nesting == 0 && links == 0)
                {
                    std::wstring part = inner.substr(start, k - start);
                    trim(part);
                    if (!part.empty())
                    {
                        visible = part;
                    }
                    start = k + 1;
                }
            }
            out += sanitize_wikitext(visible);
            i = j + 1;
            continue;
        }
        if (i + 1 < text.size() && text[i] == L'[' && text[i + 1] == L'[')
        {
            const std::size_t close = text.find(L"]]", i + 2);
            if (close == std::wstring::npos)
            {
                i += 2;
                continue;
            }
            std::wstring inner = text.substr(i + 2, close - i - 2);
            const std::size_t bar = inner.find_last_of(L'|');
            if (bar != std::wstring::npos)
            {
                inner = inner.substr(bar + 1);
            }
            out += sanitize_wikitext(inner);
            i = close + 2;
            continue;
        }
        if (i + 2 < text.size() && text[i] == L'\'' && text[i + 1] == L'\'' && text[i + 2] == L'\'')
        {
            i += 3;
            continue;
        }
        if (i + 1 < text.size() && text[i] == L'\'' && text[i + 1] == L'\'')
        {
            i += 2;
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    for (std::size_t k = 0; k < out.size(); ++k)
    {
        if (out[k] == L'\r' || out[k] == L'\t')
        {
            out[k] = L' ';
        }
    }
    std::wstring collapsed;
    bool last_space = false;
    for (wchar_t ch : out)
    {
        const bool space = ch == L' ';
        if (space && last_space)
        {
            continue;
        }
        if (ch == L'\n') {
            while (!collapsed.empty() && collapsed.back() == L' ') collapsed.pop_back();
            if (!collapsed.empty() && collapsed.back() != L'\n') collapsed.push_back(ch);
            last_space = true; continue;
        }
        collapsed.push_back(ch);
        last_space = space;
    }
    trim(collapsed);
    return collapsed;
}

void set_field(PrtsResult& result, const std::wstring& key, const std::wstring& value)
{
    if (key.empty() || value.empty())
    {
        return;
    }
    for (PrtsField& field : result.fields)
    {
        if (field.key == key)
        {
            field.value = sanitize_wikitext(value);
            return;
        }
    }
    result.fields.push_back(PrtsField{key, sanitize_wikitext(value)});
}

}  // namespace

PrtsClient::~PrtsClient()
{
    shutdown();
}

void PrtsClient::configure_cache(std::filesystem::path directory, std::uint64_t max_bytes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    cache_.clear();
    image_cache_.clear();
    page_refresh_failed_.clear();
    image_refresh_failed_.clear();
    disk_cache_.configure(std::move(directory), max_bytes);
}

void PrtsClient::invalidate_image(const std::wstring& enemy_name)
{
    if (enemy_name.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    image_cache_.erase(enemy_name);
    image_ready_.erase(enemy_name);
    image_failed_until_.erase(enemy_name);
    image_refresh_failed_.erase(enemy_name);
    image_refresh_.insert(enemy_name);
    const auto active = image_active_.find(enemy_name);
    if (active != image_active_.end()) active->second->store(true);
    disk_cache_.erase_async(PrtsDiskCache::Kind::Image, enemy_name);
}

void PrtsClient::request(const std::wstring& enemy_name, bool refresh)
{
    if (enemy_name.empty())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        if (refresh) { cache_.erase(enemy_name); page_refresh_failed_.erase(enemy_name); }
        const auto cached = cache_.find(enemy_name);
        if (!refresh && cached != cache_.end() &&
            (PrtsDiskCache::fresh(cached->second.fetched_at, PrtsDiskCache::kPageTtlMs) ||
             (cached->second.result.stale && backing_off(page_refresh_failed_, enemy_name))))
        {
            cached->second.used = GetTickCount64();
            ready_[enemy_name] = cached->second.result;
            disk_cache_.touch_async(PrtsDiskCache::Kind::Page, enemy_name);
            return;
        }
        if (pending_name_ == enemy_name ||
            (!refresh && active_name_ == enemy_name && active_cancel_ && !active_cancel_->load())) {
            pending_refresh_ = pending_refresh_ || refresh;
            return;
        }
        ready_.erase(enemy_name);
        if (!pending_name_.empty()) {
            PrtsResult replaced;
            replaced.title = pending_name_;
            replaced.error = L"请求已被新的资料页替换，可重新选择重试";
            ready_[pending_name_] = std::move(replaced);
        }
        if (active_cancel_) active_cancel_->store(true);
        pending_name_ = enemy_name;
        pending_refresh_ = refresh;
        if (!worker_.joinable()) {
            try { worker_ = std::thread(&PrtsClient::worker_loop, this); }
            catch (...) {
                PrtsResult failed; failed.title = enemy_name; failed.error = L"无法启动资料请求线程，请重试";
                ready_[enemy_name] = std::move(failed);
                pending_name_.clear();
            }
        }
    }
    cv_.notify_one();
}

bool PrtsClient::poll(std::wstring& name, PrtsResult& out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (ready_.empty())
    {
        return false;
    }
    auto ready = ready_.begin();
    name = ready->first;
    out = std::move(ready->second);
    ready_.erase(ready);
    return true;
}

void PrtsClient::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        image_stopping_ = true;
        if (active_cancel_) active_cancel_->store(true);
        for (auto& active : image_active_) active.second->store(true);
    }
    cv_.notify_all();
    image_cv_.notify_all();
    if (worker_.joinable())
    {
        worker_.join();
    }
    for (auto& worker : image_workers_) if (worker.joinable()) worker.join();
    disk_cache_.shutdown();
}


bool PrtsClient::request_image(const std::wstring& enemy_name, bool refresh, bool priority)
{
    if (enemy_name.empty())
    {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (image_stopping_) return false;
        refresh = refresh || image_refresh_.count(enemy_name);
        if (refresh) {
            image_cache_.erase(enemy_name);
            image_failed_until_.erase(enemy_name);
            image_refresh_failed_.erase(enemy_name);
            image_ready_.erase(enemy_name);
            image_refresh_.insert(enemy_name);
            const auto active = image_active_.find(enemy_name);
            if (active != image_active_.end()) active->second->store(true);
        }
        const auto cached = image_cache_.find(enemy_name);
        if (!refresh && cached != image_cache_.end() &&
            (PrtsDiskCache::fresh(cached->second.fetched_at, PrtsDiskCache::kImageTtlMs) ||
             backing_off(image_refresh_failed_, enemy_name)))
        {
            cached->second.used = GetTickCount64();
            image_ready_[enemy_name] = {cached->second.bytes, cached->second.error};
            disk_cache_.touch_async(PrtsDiskCache::Kind::Image, enemy_name);
            return true;
        }
        const auto active = image_active_.find(enemy_name);
        if ((!refresh && active != image_active_.end() && !active->second->load()) ||
            image_ready_.count(enemy_name)) return true;
        for (auto entry = image_failed_until_.begin(); entry != image_failed_until_.end();) {
            if (entry->second <= GetTickCount64()) entry = image_failed_until_.erase(entry);
            else ++entry;
        }
        const auto failed = image_failed_until_.find(enemy_name);
        if (failed != image_failed_until_.end() && GetTickCount64() < failed->second) return false;
        const auto queued = std::find(image_queue_.begin(), image_queue_.end(), enemy_name);
        if (queued != image_queue_.end()) {
            if (priority) {
                image_queue_.erase(queued);
                image_queue_.insert(image_queue_.begin(), enemy_name);
            }
            return true;
        }
        if (image_queue_.size() >= 48) {
            image_ready_[enemy_name] = {{}, L"头像请求较多，请稍后重试"};
            if (image_failed_until_.size() >= 64) image_failed_until_.erase(image_failed_until_.begin());
            image_failed_until_[enemy_name] = GetTickCount64() + 2000;
            return true;
        }
        if (priority) image_queue_.insert(image_queue_.begin(), enemy_name);
        else image_queue_.push_back(enemy_name);
        for (auto& worker : image_workers_) {
            if (worker.joinable()) continue;
            try { worker = std::thread(&PrtsClient::image_worker_loop, this); }
            catch (...) { /* Keep an existing worker if the second thread could not start. */ }
        }
        if (!image_workers_[0].joinable() && !image_workers_[1].joinable()) {
            for (const auto& name : image_queue_)
                image_ready_[name] = {{}, L"无法启动头像请求线程"};
            image_queue_.clear();
        }
    }
    image_cv_.notify_all();
    return true;
}

bool PrtsClient::poll_image(std::wstring& name, std::vector<unsigned char>& image_bytes, std::wstring* error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (image_ready_.empty())
    {
        return false;
    }
    auto ready = image_ready_.begin();
    name = ready->first;
    image_bytes = std::move(ready->second.bytes);
    if (error) *error = std::move(ready->second.error);
    image_ready_.erase(ready);
    return true;
}

void PrtsClient::image_worker_loop()
{
    for (;;)
    {
        std::wstring name;
        bool refresh = false;
        std::shared_ptr<std::atomic<bool>> cancelled;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const auto available = [this] {
                return std::find_if(image_queue_.begin(), image_queue_.end(), [this](const auto& name) {
                    return !image_active_.count(name);
                });
            };
            image_cv_.wait(lock, [&] { return image_stopping_ || available() != image_queue_.end(); });
            if (image_stopping_)
            {
                return;
            }
            const auto queued = available();
            name = *queued;
            image_queue_.erase(queued);
            refresh = image_refresh_.erase(name) != 0;
            image_active_[name] = cancelled = std::make_shared<std::atomic<bool>>(false);
        }
        std::vector<unsigned char> bytes;
        std::wstring error;
        bool ok = false;
        std::uint64_t fetched_at = 0;
        try { ok = fetch_image(name, bytes, error, cancelled.get(), refresh, fetched_at) && !bytes.empty(); }
        catch (...) { error = L"头像请求异常，请稍后重试"; }
        std::lock_guard<std::mutex> lock(mutex_);
        image_active_.erase(name);
        image_cv_.notify_all();
        if (image_stopping_) return;
        if (cancelled->load()) continue;
        if (ok) {
            image_cache_[name] = {bytes, fetched_at, GetTickCount64(), error};
            if (error.empty()) image_refresh_failed_.erase(name);
            trim_memory_cache(image_cache_);
            image_failed_until_.erase(name);
            image_ready_[name] = {std::move(bytes), std::move(error)};
        } else {
            if (error.empty()) error = L"头像暂时不可用";
            if (image_failed_until_.size() >= 64 && !image_failed_until_.count(name))
                image_failed_until_.erase(image_failed_until_.begin());
            image_failed_until_[name] = GetTickCount64() + 2000;
            image_ready_[name] = {{}, std::move(error)};
        }
    }
}

bool PrtsClient::download_image(const std::wstring& enemy_name, std::vector<unsigned char>& bytes,
                             std::wstring& error, const std::atomic<bool>* cancelled)
{
    const auto deadline = GetTickCount64() + 12000;
    const std::string title = url_encode(wide_to_utf8(enemy_name));
    const std::wstring meta_url =
        L"https://prts.wiki/api.php?action=query&prop=pageimages&piprop=thumbnail&pithumbsize=96"
        L"&format=json&formatversion=2&redirects=1&titles=" +
        utf8_to_wide(title);
    std::string body;
    std::string http_error;
    if (!http_get(meta_url, body, http_error, HttpOptions{12000, cancelled}))
    {
        error = L"头像查询失败：" + utf8_to_wide(http_error);
        return false;
    }
    std::string source;
    if (!extract_json_string(body, "source", source) || source.empty())
    {
        error = L"页面没有头像";
        return false;
    }
    std::wstring image_url = utf8_to_wide(source);
    std::wstring normalized;
    normalized.reserve(image_url.size());
    for (std::size_t i = 0; i < image_url.size(); ++i)
    {
        if (image_url[i] == L'\\' && i + 1 < image_url.size() && image_url[i + 1] == L'/')
        {
            continue;  // 去掉 JSON 里的 \/ 转义
        }
        normalized.push_back(image_url[i]);
    }
    std::string image_body;
    const auto now = GetTickCount64();
    if (now >= deadline) { error = L"头像请求超时"; return false; }
    if (!http_get(normalized, image_body, http_error,
                  HttpOptions{static_cast<std::uint32_t>(deadline-now), cancelled}) || image_body.empty())
    {
        error = L"头像下载失败：" + utf8_to_wide(http_error);
        return false;
    }
    bytes.assign(image_body.begin(), image_body.end());
    return true;
}

bool PrtsClient::fetch_image(const std::wstring& name, std::vector<unsigned char>& bytes,
                            std::wstring& error, const std::atomic<bool>* cancel,
                            bool refresh, std::uint64_t& fetched_at)
{
    using Kind = PrtsDiskCache::Kind;
    if (refresh) disk_cache_.erase_async(Kind::Image, name);
    PrtsDiskCache::Entry old;
    const bool cached = !refresh && disk_cache_.read(Kind::Image, name, old);
    if (cancelled(cancel)) return false;
    if (cached && PrtsDiskCache::fresh(old.fetched_at_ms, PrtsDiskCache::kImageTtlMs)) {
        fetched_at = old.fetched_at_ms;
        bytes = std::move(old.bytes);
        return true;
    }
    if (cached) {
        std::lock_guard lock(mutex_);
        if (backing_off(image_refresh_failed_, name)) {
            fetched_at = old.fetched_at_ms;
            bytes = std::move(old.bytes);
            error = image_refresh_failed_.at(name).error;
            return true;
        }
    }
    const auto generation = disk_cache_.generation(Kind::Image, name);
    if (download_image(name, bytes, error, cancel) && !bytes.empty() && !cancelled(cancel)) {
        fetched_at = PrtsDiskCache::now_ms();
        disk_cache_.write(Kind::Image, name, {fetched_at, bytes}, generation);
        return true;
    }
    if (cached && !cancelled(cancel)) {
        fetched_at = old.fetched_at_ms;
        bytes = std::move(old.bytes);
        error = L"刷新失败，使用已缓存头像：" + error;
        std::lock_guard lock(mutex_);
        bound_failures(image_refresh_failed_);
        image_refresh_failed_[name] = {GetTickCount64() + 60000, error};
        return true;
    }
    return false;
}

void PrtsClient::worker_loop()
{
    for (;;)
    {
        std::wstring name;
        bool refresh = false;
        std::shared_ptr<std::atomic<bool>> cancelled;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !pending_name_.empty(); });
            if (stopping_)
            {
                return;
            }
            name = pending_name_;
            pending_name_.clear();
            refresh = pending_refresh_;
            pending_refresh_ = false;
            active_name_ = name;
            active_cancel_ = cancelled = std::make_shared<std::atomic<bool>>(false);
        }
        PrtsResult result;
        std::uint64_t fetched_at = 0;
        try { result = fetch(name, cancelled.get(), refresh, fetched_at); }
        catch (...) { result.title = name; result.error = L"资料请求异常，请重试"; }
        if (!result.ok && result.error.empty()) result.error = L"资料暂时不可用，请重试";
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_name_.clear();
            active_cancel_.reset();
            if (stopping_) return;
            if (result.ok && !cancelled->load()) {
                cache_[name] = {result, fetched_at, GetTickCount64()};
                trim_memory_cache(cache_);
                if (!result.stale) page_refresh_failed_.erase(name);
            }
            // A -> B -> A may cancel the first A while a replacement A is pending. Its old
            // cancellation must not become the terminal state of the current selection.
            if (!(cancelled->load() && pending_name_ == name)) ready_[name] = std::move(result);
        }
    }
}

PrtsResult PrtsClient::fetch(const std::wstring& enemy_name, const std::atomic<bool>* cancel,
                             bool refresh, std::uint64_t& fetched_at)
{
    using Kind = PrtsDiskCache::Kind;
    PrtsDiskCache::Entry old;
    PrtsResult fallback;
    if (!refresh && disk_cache_.read(Kind::Page, enemy_name, old)) {
        fallback = parse(enemy_name, std::string(old.bytes.begin(), old.bytes.end()));
        if (!fallback.ok) disk_cache_.erase_async(Kind::Page, enemy_name);
        else if (PrtsDiskCache::fresh(old.fetched_at_ms, PrtsDiskCache::kPageTtlMs)) {
            fetched_at = old.fetched_at_ms;
            return fallback;
        }
        else {
            std::lock_guard lock(mutex_);
            if (backing_off(page_refresh_failed_, enemy_name)) {
                fallback.stale = true;
                fallback.error = page_refresh_failed_.at(enemy_name).error;
                fetched_at = old.fetched_at_ms;
                return fallback;
            }
        }
    }
    PrtsResult result;
    result.title = enemy_name;
    if (cancelled(cancel)) { result.error = L"资料请求已取消"; return result; }
    const auto generation = disk_cache_.generation(Kind::Page, enemy_name);

    const std::string page = url_encode(wide_to_utf8(enemy_name));
    const std::wstring url =
        L"https://prts.wiki/api.php?action=parse&prop=wikitext&format=json&formatversion=2&redirects=1&page=" +
        utf8_to_wide(page);
    std::string body;
    std::string error;
    if (!http_get(url, body, error, HttpOptions{12000, cancel}))
    {
        result.error = L"网络请求失败：" + utf8_to_wide(error);
    } else {
        result = parse(enemy_name, body);
        if (result.ok && !cancelled(cancel)) {
            fetched_at = PrtsDiskCache::now_ms();
            disk_cache_.write(Kind::Page, enemy_name,
                {fetched_at, std::vector<unsigned char>(body.begin(), body.end())}, generation);
            return result;
        }
    }
    if (fallback.ok && !refresh && !cancelled(cancel)) {
        fallback.stale = true;
        fallback.error = L"刷新失败，使用已缓存资料：" + result.error;
        fetched_at = old.fetched_at_ms;
        std::lock_guard lock(mutex_);
        bound_failures(page_refresh_failed_);
        page_refresh_failed_[enemy_name] = {GetTickCount64() + 60000, fallback.error};
        return fallback;
    }
    return result;
}

PrtsResult PrtsClient::parse(const std::wstring& enemy_name, const std::string& body)
{
    PrtsResult result;
    result.title = enemy_name;

    std::string wikitext_utf8;
    if (!extract_json_string(body, "wikitext", wikitext_utf8) || wikitext_utf8.empty())
    {
        std::string api_error;
        result.error = extract_json_string(body, "info", api_error) && !api_error.empty()
            ? L"PRTS 返回：" + utf8_to_wide(api_error)
            : L"页面缺少敌人资料（可能页面不存在或站点暂时不可用）";
        return result;
    }
    const std::wstring wikitext = without_comments(utf8_to_wide(wikitext_utf8));

    bool found_common = false;
    std::size_t position = wikitext.find(L"{{敌人信息/common2");
    std::wstring block;
    if (position != std::wstring::npos && template_block(wikitext, position, block))
    {
        found_common = true;
        for (const auto& [key, value] : parse_params(block))
        {
            std::wstring trimmed = sanitize_wikitext(value);
            trim(trimmed);
            if (key == L"id" || key == L"index" || key == L"名称")
            {
                continue;
            }
            if (trimmed.empty()) continue;
            result.basic.push_back(PrtsField{key, trimmed});
            set_field(result, key, trimmed);
        }
    }
    // 收集页面上的全部级别表（每级只写差异），按 index 升序做继承合并。
    {
        struct LevelBlock
        {
            int index = 0;
            std::vector<std::pair<std::wstring, std::wstring>> params;
        };
        std::vector<LevelBlock> blocks;
        std::size_t scan = wikitext.find(L"{{敌人信息/levelcontent");
        while (scan != std::wstring::npos)
        {
            std::wstring candidate;
            if (template_block(wikitext, scan, candidate))
            {
                LevelBlock entry;
                for (const auto& [key, value] : parse_params(candidate))
                {
                    std::wstring trimmed = value;
                    trim(trimmed);
                    if (key == L"index")
                    {
                        entry.index = std::wcslen(trimmed.c_str()) > 0 ? _wtoi(trimmed.c_str()) : 0;
                    }
                    else
                    {
                        entry.params.emplace_back(key, trimmed);
                    }
                }
                blocks.push_back(std::move(entry));
            }
            scan = wikitext.find(L"{{敌人信息/levelcontent", scan + 2);
        }
        std::sort(blocks.begin(), blocks.end(),
                  [](const LevelBlock& a, const LevelBlock& b) { return a.index < b.index; });
        std::vector<std::pair<std::wstring, std::wstring>> inherited;
        for (const LevelBlock& entry : blocks)
        {
            for (const auto& [key, value] : entry.params)
            {
                if (key == L"名称" || key == L"地位" || key == L"种类")
                {
                    continue;
                }
                bool replaced = false;
                for (auto& existing : inherited)
                {
                    if (existing.first == key)
                    {
                        existing.second = value;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced)
                {
                    inherited.emplace_back(key, value);
                }
            }
            PrtsLevel level;
            level.index = entry.index;
            for (const auto& [key, value] : inherited)
            {
                level.fields.push_back(PrtsField{key, sanitize_wikitext(value)});
            }
            result.levels.push_back(std::move(level));
        }
        // 兼容字段：基础信息 + 级别0（没有级别表时退化为空）。
        if (!result.levels.empty())
        {
            for (const PrtsField& field : result.levels.front().fields)
            {
                set_field(result, field.key, field.value);
            }
        }
    }

    if (!found_common)
    {
        result.error = L"页面中没有找到敌人信息模板";
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace arkhelp::net
