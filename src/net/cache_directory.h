#pragma once
#include <filesystem>
#include <string>
#include <windows.h>

namespace arkhelp {
inline std::filesystem::path prts_cache_directory(const std::wstring& root,const std::wstring& configured) {
    auto path=configured.empty()?std::filesystem::path(root)/L"cache"/L"prts":std::filesystem::path(configured);
    if(path.is_relative()) path=std::filesystem::path(root)/path;
    return path.lexically_normal();
}

// Probe only a uniquely named temporary file, never alter existing contents.
inline std::wstring prepare_cache_directory(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path,error);
    if(error || !std::filesystem::is_directory(path,error)) return L"无法使用该目录，请选择可写文件夹";
    for(int attempt=0;attempt<8;++attempt) {
        const auto probe=path/(L".arkhelper-write-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+
            std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(attempt)+L".tmp");
        const HANDLE file=CreateFileW(probe.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
        if(file!=INVALID_HANDLE_VALUE) { CloseHandle(file);return {}; }
        if(GetLastError()!=ERROR_FILE_EXISTS && GetLastError()!=ERROR_ALREADY_EXISTS) break;
    }
    return L"该目录无法写入，已保留原缓存路径";
}
} // namespace arkhelp
