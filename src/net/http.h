// 极简 HTTP 客户端（WinHTTP），仅用于拉取 PRTS 的 wikitext / 图片。
//
// ArkHelper 是 GUI 程序，网络请求都在专用 worker 线程里执行；这些函数是阻塞的，
// 不要在 UI 线程直接调用。TLS 证书校验保持默认开启，不跟随跨域重定向。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace arkhelp::net
{

// UTF-8 百分号编码（用于拼接 query 参数）。
std::string url_encode(const std::string& utf8);

// 同步 GET，返回响应体（上限 4 MiB）。失败时 error 写入简短原因。
struct HttpOptions {
    std::uint32_t timeout_ms = 12000; // One deadline includes DNS/connect/headers/body.
    const std::atomic<bool>* cancelled = nullptr;
};
bool http_get(const std::wstring& url, std::string& body, std::string& error,
              const HttpOptions& options = {});

}  // namespace arkhelp::net
