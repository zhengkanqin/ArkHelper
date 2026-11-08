#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../shell/platform.h"
#include "../shell/win_util.h"

// 实现须知：
// - 日志可能在任何 UI 回调里调用，这里用 SRWLOCK 串行化，避免多行交错。
// - %s / %d 一律配 static_cast，禁止 %zu / %llu（项目里统一跨编译器安全的写法）。
// - 单文件超过 1MB 时截断重开，避免长时间挂机把磁盘写满。
namespace arkhelp
{
namespace
{

constexpr size_t kLogLineCapacity = 1024;
constexpr unsigned long long kMaxLogBytes = 1024ull * 1024ull;

struct LogState
{
    SRWLOCK lock = SRWLOCK_INIT;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring path;
};

LogState g_log;

bool write_all(HANDLE file, const char* data, size_t size)
{
    while (size > 0)
    {
        DWORD written = 0;
        DWORD chunk = static_cast<DWORD>(size > 0x40000000u ? 0x40000000u : size);
        if (!WriteFile(file, data, chunk, &written, nullptr) || written == 0)
        {
            return false;
        }
        data += written;
        size -= written;
    }
    return true;
}

bool reopen_locked(bool truncate)
{
    if (g_log.path.empty())
    {
        return false;
    }
    g_log.file = CreateFileW(g_log.path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, truncate ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_log.file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    SetFilePointer(g_log.file, 0, nullptr, FILE_END);
    return true;
}

void log_line(const char* level, const char* format, va_list args)
{
    char message[kLogLineCapacity];
    std::vsnprintf(message, sizeof(message), format, args);

    AcquireSRWLockExclusive(&g_log.lock);
    if (g_log.file != INVALID_HANDLE_VALUE)
    {
        SYSTEMTIME now;
        GetLocalTime(&now);

        char line[kLogLineCapacity];
        int prefix = std::snprintf(line, sizeof(line), "[%04d-%02d-%02d %02d:%02d:%02d.%03d][%lu] %s ",
                                   static_cast<int>(now.wYear), static_cast<int>(now.wMonth),
                                   static_cast<int>(now.wDay), static_cast<int>(now.wHour),
                                   static_cast<int>(now.wMinute), static_cast<int>(now.wSecond),
                                   static_cast<int>(now.wMilliseconds),
                                   static_cast<unsigned long>(GetCurrentProcessId()), level);
        if (prefix > 0 && static_cast<size_t>(prefix) < sizeof(line))
        {
            size_t used = static_cast<size_t>(prefix);
            size_t room = sizeof(line) - used - 3;
            size_t len = std::strlen(message);
            if (len > room)
            {
                len = room;
            }
            std::memcpy(line + used, message, len);
            used += len;
            line[used++] = '\n';
            write_all(g_log.file, line, used);
        }

        LARGE_INTEGER size;
        if (GetFileSizeEx(g_log.file, &size) && static_cast<unsigned long long>(size.QuadPart) > kMaxLogBytes)
        {
            // 超过上限：关闭后以 CREATE_ALWAYS 重开，等价于截断到 0 字节。
            CloseHandle(g_log.file);
            g_log.file = INVALID_HANDLE_VALUE;
            reopen_locked(true);
        }
    }
    ReleaseSRWLockExclusive(&g_log.lock);
}

}  // namespace

bool log_open(const std::wstring& log_dir)
{
    if (!ensure_directory(log_dir))
    {
        return false;
    }

    wchar_t name[64];
    std::swprintf(name, sizeof(name) / sizeof(name[0]), L"ArkHelper-%lu.log",
                  static_cast<unsigned long>(GetCurrentProcessId()));

    AcquireSRWLockExclusive(&g_log.lock);
    g_log.path = join_path(log_dir, name);
    g_log.file = INVALID_HANDLE_VALUE;
    bool ok = reopen_locked(false);
    ReleaseSRWLockExclusive(&g_log.lock);
    return ok;
}

void log_close()
{
    AcquireSRWLockExclusive(&g_log.lock);
    if (g_log.file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_log.file);
        g_log.file = INVALID_HANDLE_VALUE;
    }
    ReleaseSRWLockExclusive(&g_log.lock);
}

void log_info(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    log_line("INFO", format, args);
    va_end(args);
}

void log_warn(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    log_line("WARN", format, args);
    va_end(args);
}

void log_error(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    log_line("ERROR", format, args);
    va_end(args);
}

}  // namespace arkhelp
