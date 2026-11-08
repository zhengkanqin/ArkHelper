#include "net/http.h"

#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <algorithm>
#include <array>
#include <atomic>
#include <iterator>
#include <memory>
#include <string>

namespace arkhelp::net
{
namespace
{

constexpr std::size_t kMaxBodyBytes = 4u * 1024u * 1024u;
constexpr wchar_t kUserAgent[] = L"ArkHelper/0.3 (PRTS reader)";

void set_error(std::string& error, const char* what, DWORD code)
{
    char buffer[160] = {};
    std::snprintf(buffer, sizeof(buffer), "%s (win32=%lu)", what, static_cast<unsigned long>(code));
    error = buffer;
}

// Context and read buffer outlive cancellation: WinHTTP may call back after CloseHandle returns.
// The final HANDLE_CLOSING notification releases the handle's reference, as required by WinHTTP.
struct AsyncRequest {
    std::atomic<unsigned> references{1};
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<DWORD> completed{0}, failure{0}, bytes{0};
    std::array<char, 16384> buffer{};
    ~AsyncRequest() { if (event) CloseHandle(event); }
    void release() { if (references.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this; }
    static void CALLBACK callback(HINTERNET, DWORD_PTR context, DWORD status, void* info, DWORD length) {
        auto* state = reinterpret_cast<AsyncRequest*>(context);
        if (!state) return;
        if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) { state->release(); return; }
        if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
            const auto* result = static_cast<WINHTTP_ASYNC_RESULT*>(info);
            state->failure.store(result ? result->dwError : ERROR_GEN_FAILURE, std::memory_order_release);
        } else if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
            state->bytes.store(length, std::memory_order_release);
        } else if (status != WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE &&
                   status != WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE) return;
        state->completed.store(status, std::memory_order_release);
        SetEvent(state->event);
    }
    void reset() {
        ResetEvent(event);
        completed.store(0, std::memory_order_relaxed);
        failure.store(0, std::memory_order_relaxed);
        bytes.store(0, std::memory_order_relaxed);
    }
};

struct HttpHandles {
    HINTERNET session = nullptr, connection = nullptr, request = nullptr;
    AsyncRequest* state = nullptr;
    ~HttpHandles() {
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        if (session) WinHttpCloseHandle(session);
        if (state) state->release();
    }
};

}  // namespace

std::string url_encode(const std::string& utf8)
{
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(utf8.size() * 3);
    for (unsigned char ch : utf8)
    {
        const bool plain = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                           (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' ||
                           ch == '~';
        if (plain)
        {
            out.push_back(static_cast<char>(ch));
        }
        else
        {
            out.push_back('%');
            out.push_back(kHex[ch >> 4]);
            out.push_back(kHex[ch & 0x0F]);
        }
    }
    return out;
}

bool http_get(const std::wstring& url, std::string& body, std::string& error, const HttpOptions& options)
{
    body.clear();
    error.clear();
    const ULONGLONG deadline = GetTickCount64() + std::max<std::uint32_t>(1, options.timeout_ms);
    const auto cancelled = [&] { return options.cancelled && options.cancelled->load(std::memory_order_relaxed); };
    if (cancelled()) { error = "request cancelled"; return false; }

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {};
    wchar_t path[1024] = {};
    wchar_t extra[1024] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
    {
        set_error(error, "WinHttpCrackUrl", GetLastError());
        return false;
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) {
        error = "unsupported URL scheme";
        return false;
    }
    std::wstring target = path;
    target += extra;

    HttpHandles handles;
    handles.session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (!handles.session)
    {
        set_error(error, "WinHttpOpen", GetLastError());
        return false;
    }
    WinHttpSetTimeouts(handles.session, 3000, 3000, 5000, 8000);
    handles.connection = WinHttpConnect(handles.session, host, parts.nPort, 0);
    if (!handles.connection) { set_error(error, "WinHttpConnect", GetLastError()); return false; }
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    handles.request = WinHttpOpenRequest(handles.connection, L"GET", target.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!handles.request) { set_error(error, "WinHttpOpenRequest", GetLastError()); return false; }
    // MediaWiki redirects are requested explicitly in its API. Never forward this request to an
    // unrelated origin through an HTTP redirect; TLS validation remains enabled.
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(handles.request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy, sizeof(redirect_policy))) {
        set_error(error, "WinHttpSetOption redirect", GetLastError()); return false;
    }
    handles.state = new AsyncRequest;
    if (!handles.state->event) { set_error(error, "CreateEvent", GetLastError()); return false; }
    const DWORD_PTR context = reinterpret_cast<DWORD_PTR>(handles.state);
    if (!WinHttpSetOption(handles.request, WINHTTP_OPTION_CONTEXT_VALUE,
                          const_cast<DWORD_PTR*>(&context), sizeof(context))) {
        set_error(error, "WinHttpSetOption context", GetLastError()); return false;
    }
    handles.state->references.fetch_add(1, std::memory_order_relaxed);
    if (WinHttpSetStatusCallback(handles.request, AsyncRequest::callback,
            WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK) {
        handles.state->release();
        set_error(error, "WinHttpSetStatusCallback", GetLastError()); return false;
    }
    const auto wait = [&](DWORD expected) {
        for (;;) {
            if (cancelled()) { error = "request cancelled"; return false; }
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) { error = "request timed out (total deadline)"; return false; }
            const DWORD wait_ms = static_cast<DWORD>(std::min<ULONGLONG>(deadline-now, 50));
            const DWORD result = WaitForSingleObject(handles.state->event, wait_ms);
            if (result == WAIT_TIMEOUT) continue;
            if (result != WAIT_OBJECT_0) { set_error(error, "WaitForSingleObject", GetLastError()); return false; }
            const DWORD failure = handles.state->failure.load(std::memory_order_acquire);
            if (failure) { set_error(error, "request failed", failure); return false; }
            if (handles.state->completed.load(std::memory_order_acquire) == expected) return true;
        }
    };
    const auto accepted = [&](BOOL result, const char* operation) {
        if (result || GetLastError() == ERROR_IO_PENDING) return true;
        set_error(error, operation, GetLastError());
        return false;
    };
    handles.state->reset();
    if (!accepted(WinHttpSendRequest(handles.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, context), "WinHttpSendRequest") ||
        !wait(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE)) return false;
    handles.state->reset();
    if (!accepted(WinHttpReceiveResponse(handles.request, nullptr), "WinHttpReceiveResponse") ||
        !wait(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE)) return false;
    DWORD status = 0, status_size = sizeof(status);
    if (!WinHttpQueryHeaders(handles.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX)) {
        set_error(error, "WinHttpQueryHeaders", GetLastError()); return false;
    }
    if (status != 200) { error = "HTTP " + std::to_string(status); return false; }
    DWORD expected_bytes = 0, length_size = sizeof(expected_bytes);
    const bool length_known = WinHttpQueryHeaders(handles.request,
        WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
        &expected_bytes, &length_size, WINHTTP_NO_HEADER_INDEX) != FALSE;
    if (length_known && expected_bytes > kMaxBodyBytes) { error = "response too large"; return false; }
    for (;;) {
        handles.state->reset();
        if (!accepted(WinHttpReadData(handles.request, handles.state->buffer.data(),
                    static_cast<DWORD>(handles.state->buffer.size()), nullptr), "WinHttpReadData") ||
            !wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE)) { body.clear(); return false; }
        const DWORD read = handles.state->bytes.load(std::memory_order_acquire);
        if (!read) {
            if (length_known && body.size() != expected_bytes) {
                body.clear(); error = "truncated response body"; return false;
            }
            return true;
        }
        if (body.size() + read > kMaxBodyBytes) { body.clear(); error = "response too large"; return false; }
        body.append(handles.state->buffer.data(), read);
    }
}

}  // namespace arkhelp::net
