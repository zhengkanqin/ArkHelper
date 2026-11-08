#pragma once

#include <string>

// 轻量日志：单文件、按行写、无需 CRT 文件 IO。
// 之所以自己用 CreateFileW/WriteFile 而不是 fopen：配置目录可能包含非 ASCII 字符，
// 且我们希望 1MB 截断逻辑完全可控。
namespace arkhelp
{

bool log_open(const std::wstring& log_dir);
void log_close();

void log_info(const char* format, ...);
void log_warn(const char* format, ...);
void log_error(const char* format, ...);

}  // namespace arkhelp
