#include "console.h"

#include <windows.h>
#include <conio.h>
#include <stdarg.h>
#include <wchar.h>

#include <algorithm>
#include <cstddef>

namespace ark::loader {

namespace {

constexpr int kStatusLineChars = 2048;

} // namespace

void write_console(const std::wstring& text) {
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    DWORD written = 0;
    if (GetConsoleMode(output, &mode)) {
        WriteConsoleW(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        return;
    }
    const int text_length = static_cast<int>(text.size());
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), text_length, nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(std::max(bytes, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), text_length, utf8.data(), bytes, nullptr, nullptr);
    WriteFile(output, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

std::wstring utf8_to_wide(const std::string& text) {
    const int text_length = static_cast<int>(text.size());
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), text_length, nullptr, 0);
    std::wstring wide(static_cast<size_t>(std::max(count, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), text_length, wide.data(), count);
    return wide;
}

void print_status(const wchar_t* format, ...) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t line[kStatusLineChars];
    const int prefix =
        swprintf(line, kStatusLineChars, L"[%02u:%02u:%02u] ", now.wHour, now.wMinute, now.wSecond);
    va_list args;
    va_start(args, format);
    vswprintf(line + prefix, kStatusLineChars - prefix, format, args);
    va_end(args);
    write_console(std::wstring(line) + L"\r\n");
}

void wait_for_key_press() {
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(input, &mode)) {
        return;
    }
    print_status(L"按任意键关闭窗口。");
    FlushConsoleInputBuffer(input);
    _getwch();
}

} // namespace ark::loader
