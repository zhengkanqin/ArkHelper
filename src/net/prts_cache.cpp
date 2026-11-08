#include "net/prts_cache.h"

#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>

namespace arkhelp::net {
namespace {
constexpr std::uint32_t kMagic = 0x43545041; // APTC
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kHeaderBytes = 40;
constexpr std::size_t kMaxNameBytes = 4096;
constexpr std::uint64_t kMaxPageBytes = 8 * PrtsDiskCache::kMiB;
constexpr std::uint64_t kMaxImageBytes = 16 * PrtsDiskCache::kMiB;

std::string utf8(const std::wstring& name) {
    if (name.empty() || name.size() > kMaxNameBytes || name.find(L'\0') != std::wstring::npos) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(),
        static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0 || size > static_cast<int>(kMaxNameBytes)) return {};
    std::string result(size, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(),
        static_cast<int>(name.size()), result.data(), size, nullptr, nullptr)) return {};
    return result;
}
std::wstring wide(const std::string& name) {
    if (name.empty() || name.size() > kMaxNameBytes || name.find('\0') != std::string::npos) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
        static_cast<int>(name.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(size, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
        static_cast<int>(name.size()), result.data(), size)) return {};
    return result;
}
std::uint64_t hash(const unsigned char* bytes, std::size_t count, std::uint64_t seed = 14695981039346656037ULL) {
    for (std::size_t i = 0; i < count; ++i) { seed ^= bytes[i]; seed *= 1099511628211ULL; }
    return seed;
}
std::wstring filename(PrtsDiskCache::Kind kind, const std::string& name) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(name.data());
    wchar_t text[48]{};
    std::swprintf(text, 48, L"%lc-%016llx%016llx.cache",
        kind == PrtsDiskCache::Kind::Page ? L'p' : L'i',
        static_cast<unsigned long long>(hash(bytes, name.size())),
        static_cast<unsigned long long>(hash(bytes, name.size(), 7809847782465536322ULL)));
    return text;
}
bool managed_name(const std::filesystem::path& path, const wchar_t* extension) {
    const auto name = path.filename().wstring();
    if (name.size() != 34 + std::wcslen(extension) || name[1] != L'-' ||
        (name[0] != L'p' && name[0] != L'i') || path.extension() != extension) return false;
    return std::all_of(name.begin() + 2, name.begin() + 34, [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
    });
}
bool regular(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
void number(std::vector<unsigned char>& bytes, std::uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) bytes.push_back(static_cast<unsigned char>(value >> (i * 8)));
}
std::uint64_t number(const unsigned char* bytes, unsigned count) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
    return value;
}
struct Header {
    PrtsDiskCache::Kind kind{};
    std::wstring name;
    std::uint64_t fetched = 0, length = 0, checksum = 0;
};
bool header(std::ifstream& input, std::uint64_t file_size, const std::filesystem::path& path, Header& out) {
    std::array<unsigned char, kHeaderBytes> bytes{};
    if (file_size < bytes.size() || !input.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) return false;
    const auto kind = number(bytes.data() + 8, 4), name_size = number(bytes.data() + 12, 4);
    out.fetched = number(bytes.data() + 16, 8);
    out.length = number(bytes.data() + 24, 8);
    out.checksum = number(bytes.data() + 32, 8);
    if (number(bytes.data(), 4) != kMagic || number(bytes.data() + 4, 4) != kVersion ||
        (kind != 1 && kind != 2) || name_size == 0 || name_size > kMaxNameBytes ||
        out.length == 0 || out.length > (kind == 1 ? kMaxPageBytes : kMaxImageBytes) ||
        file_size != kHeaderBytes + name_size + out.length || out.fetched == 0 ||
        out.fetched > PrtsDiskCache::now_ms() + 5 * 60 * 1000) return false;
    std::string name(static_cast<std::size_t>(name_size), '\0');
    if (!input.read(name.data(), static_cast<std::streamsize>(name.size()))) return false;
    out.kind = static_cast<PrtsDiskCache::Kind>(kind);
    out.name = wide(name);
    return !out.name.empty() && path.filename() == filename(out.kind, name);
}
} // namespace

PrtsDiskCache::~PrtsDiskCache() { shutdown(); }

std::uint64_t PrtsDiskCache::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
bool PrtsDiskCache::fresh(std::uint64_t fetched, std::uint64_t ttl) {
    const auto now = now_ms();
    return fetched != 0 && fetched <= now + 5 * 60 * 1000 && (fetched >= now || now - fetched < ttl);
}
void PrtsDiskCache::configure(std::filesystem::path directory, std::uint64_t max_bytes) {
    {
        std::lock_guard lock(control_mutex_);
        if (stopping_) return;
        max_bytes = std::clamp(max_bytes, kMinBytes, kMaxBytes);
        if (desired_.version && desired_.directory == directory && desired_.max_bytes == max_bytes) return;
        desired_.directory = std::move(directory);
        desired_.max_bytes = max_bytes;
        ++desired_.version;
        ++generation_;
        configuration_generation_ = generation_;
        invalidations_.clear();
        pending_ = true;
        if (!maintenance_.joinable()) {
            try { maintenance_ = std::thread(&PrtsDiskCache::maintenance_loop, this); }
            catch (...) { /* Background request workers still apply pending settings. */ }
        }
    }
    cv_.notify_one();
}
std::uint64_t PrtsDiskCache::generation(Kind kind, const std::wstring& name) const {
    std::lock_guard lock(control_mutex_);
    const auto found = invalidations_.find({kind, name});
    return found == invalidations_.end() ? configuration_generation_ :
        std::max(configuration_generation_, found->second);
}
void PrtsDiskCache::erase_async(Kind kind, const std::wstring& name) {
    {
        std::lock_guard lock(control_mutex_);
        if (stopping_) return;
        ++generation_;
        if (invalidations_.size() >= 4096) {
            invalidations_.clear();
            configuration_generation_ = generation_;
        }
        invalidations_[{kind, name}] = generation_;
        erases_.insert({kind, name});
        touches_.erase({kind, name});
        pending_ = true;
    }
    cv_.notify_one();
}
void PrtsDiskCache::touch_async(Kind kind, const std::wstring& name) {
    {
        std::lock_guard lock(control_mutex_);
        if (stopping_ || desired_.directory.empty() || erases_.count({kind, name})) return;
        touches_.insert({kind, name});
        pending_ = true;
    }
    cv_.notify_one();
}
void PrtsDiskCache::shutdown() {
    {
        std::lock_guard lock(control_mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (maintenance_.joinable()) maintenance_.join();
}
void PrtsDiskCache::maintenance_loop() {
    for (;;) {
        {
            std::unique_lock lock(control_mutex_);
            cv_.wait(lock, [this] { return stopping_ || pending_; });
            if (stopping_ && !pending_) return;
        }
        try { std::lock_guard lock(io_mutex_); apply_pending(); }
        catch (...) { /* Cache failure never disables network requests. */ }
    }
}
void PrtsDiskCache::apply_pending() {
    Settings desired;
    std::set<Key> erases, touches;
    {
        std::lock_guard lock(control_mutex_);
        desired = desired_;
        erases.swap(erases_);
        touches.swap(touches_);
        pending_ = false;
    }
    if (current_.version != desired.version) {
        const bool changed_directory = current_.directory != desired.directory;
        current_ = std::move(desired);
        if (changed_directory) { files_.clear(); bytes_ = 0; scan(); }
        else if (!current_.directory.empty() && files_.empty()) scan();
    }
    if (current_.directory.empty()) return;
    for (const auto& key : erases) erase_file(file_path(key.first, key.second));
    for (const auto& key : touches) {
        const auto path = file_path(key.first, key.second);
        auto found = files_.find(path);
        if (found == files_.end() || !regular(path)) continue;
        std::error_code error;
        const auto used = std::filesystem::file_time_type::clock::now();
        std::filesystem::last_write_time(path, used, error);
        if (!error) found->second.used = used;
    }
    evict();
}
void PrtsDiskCache::scan() {
    if (current_.directory.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(current_.directory, error);
    if (error) return;
    for (std::filesystem::directory_iterator it(current_.directory, error), end; !error && it != end; it.increment(error)) {
        const auto path = it->path();
        if (!regular(path)) continue;
        if (managed_name(path, L".tmp")) { std::filesystem::remove(path, error); error.clear(); continue; }
        if (!managed_name(path, L".cache")) continue;
        const auto size = it->file_size(error);
        if (error) { error.clear(); continue; }
        std::ifstream input(path, std::ios::binary);
        Header item;
        if (!input || !header(input, size, path, item)) {
            input.close(); std::filesystem::remove(path, error);
            if (!error) continue;
            // A locked malformed file still occupies space. Account for it so later writes
            // cannot exceed the configured budget while cleanup remains unavailable.
            error.clear();
        }
        const auto used = it->last_write_time(error);
        if (error) { error.clear(); continue; }
        files_[path] = {size, used};
        bytes_ += size;
    }
}
std::filesystem::path PrtsDiskCache::file_path(Kind kind, const std::wstring& name) const {
    const auto encoded = utf8(name);
    if (encoded.empty() || current_.directory.empty()) return {};
    return current_.directory / filename(kind, encoded);
}
void PrtsDiskCache::erase_file(const std::filesystem::path& path) {
    if (path.empty()) return;
    std::error_code error;
    const bool removed = std::filesystem::remove(path, error);
    auto found = files_.find(path);
    if ((!error || removed) && found != files_.end()) {
        bytes_ -= found->second.bytes;
        files_.erase(found);
    }
}
void PrtsDiskCache::evict(std::uint64_t reserve) {
    while (!files_.empty() && (bytes_ > current_.max_bytes || reserve > current_.max_bytes - bytes_)) {
        const auto oldest = std::min_element(files_.begin(), files_.end(), [](const auto& a, const auto& b) {
            return a.second.used < b.second.used;
        });
        const auto before = files_.size();
        erase_file(oldest->first);
        if (files_.size() == before) break; // Locked files: never spin or exceed the budget by writing.
    }
}
bool PrtsDiskCache::read(Kind kind, const std::wstring& name, Entry& entry) {
    entry = {};
    try {
        std::lock_guard lock(io_mutex_);
        apply_pending();
        const auto path = file_path(kind, name);
        const auto found = files_.find(path);
        if (path.empty() || found == files_.end()) return false;
        if (!regular(path)) { erase_file(path); return false; }
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        std::ifstream input(path, std::ios::binary);
        Header item;
        if (error || !input || !header(input, size, path, item) || item.kind != kind || item.name != name) {
            input.close(); erase_file(path); return false;
        }
        std::vector<unsigned char> bytes(static_cast<std::size_t>(item.length));
        if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())) ||
            hash(bytes.data(), bytes.size()) != item.checksum) {
            input.close(); erase_file(path); return false;
        }
        input.close();
        const auto used = std::filesystem::file_time_type::clock::now();
        std::filesystem::last_write_time(path, used, error);
        found->second.used = used;
        entry.fetched_at_ms = item.fetched;
        entry.bytes = std::move(bytes);
        return true;
    } catch (...) { return false; }
}
void PrtsDiskCache::write(Kind kind, const std::wstring& name, const Entry& entry, std::uint64_t generation) {
    try {
        const auto encoded = utf8(name);
        const auto limit = kind == Kind::Page ? kMaxPageBytes : kMaxImageBytes;
        if (encoded.empty() || entry.bytes.empty() || entry.bytes.size() > limit || !entry.fetched_at_ms) return;
        std::lock_guard lock(io_mutex_);
        apply_pending();
        if (generation != this->generation(kind, name)) return;
        const auto path = file_path(kind, name);
        if (path.empty()) return;
        const std::uint64_t size = kHeaderBytes + encoded.size() + entry.bytes.size();
        if (size > current_.max_bytes) return;
        erase_file(path);
        evict(size);
        if (bytes_ > current_.max_bytes || size > current_.max_bytes - bytes_) return;
        std::vector<unsigned char> bytes;
        bytes.reserve(kHeaderBytes + encoded.size());
        number(bytes, kMagic, 4); number(bytes, kVersion, 4); number(bytes, static_cast<std::uint32_t>(kind), 4);
        number(bytes, encoded.size(), 4); number(bytes, entry.fetched_at_ms, 8); number(bytes, entry.bytes.size(), 8);
        number(bytes, hash(entry.bytes.data(), entry.bytes.size()), 8);
        bytes.insert(bytes.end(), encoded.begin(), encoded.end());
        auto temporary = path; temporary.replace_extension(L".tmp");
        if (std::filesystem::exists(temporary) && !regular(temporary)) return;
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.write(reinterpret_cast<const char*>(entry.bytes.data()), static_cast<std::streamsize>(entry.bytes.size()));
        output.flush();
        const bool complete = output.good();
        output.close();
        // An invalidation racing this commit stays pending until io_mutex_ is released;
        // every subsequent read applies it before opening the file. No UI mutex covers I/O.
        if (!complete || generation != this->generation(kind, name) ||
            !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::error_code error; std::filesystem::remove(temporary, error); return;
        }
        std::error_code error;
        files_[path] = {size, std::filesystem::last_write_time(path, error)};
        bytes_ += size;
    } catch (...) { /* Read-only/full/cache-path failures do not fail the fetched page. */ }
}

} // namespace arkhelp::net
