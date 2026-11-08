#include <winsock2.h>
#include <windows.h>
#include "net/http.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace {
unsigned checks = 0, failures = 0;
void check(bool passed, const char* label) {
    ++checks;
    if (!passed) { ++failures; std::printf("FAIL %s\n", label); }
}
class Server {
    SOCKET listener_ = INVALID_SOCKET;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false;
public:
    enum Mode { Normal, DelayedHeaders, Trickle, HttpFailure, Truncated };
    unsigned short port = 0;
    std::atomic<bool> received{false};
    explicit Server(Mode mode) {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        int size = sizeof(address);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size);
        port = ntohs(address.sin_port);
        listen(listener_, 1);
        thread_ = std::thread([this, mode] {
            SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            char request[4096];
            recv(client, request, sizeof(request), 0);
            received = true;
            if (mode == DelayedHeaders) {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return stopping_; });
            } else if (mode == Normal) {
                const char response[] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: close\r\n\r\nabc";
                send(client, response, sizeof(response)-1, 0);
            } else if (mode == HttpFailure) {
                const char response[] = "HTTP/1.1 503 Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                send(client, response, sizeof(response)-1, 0);
            } else {
                const char headers[] = "HTTP/1.1 200 OK\r\nContent-Length: 999\r\nConnection: close\r\n\r\n";
                send(client, headers, sizeof(headers)-1, 0);
                if (mode == Truncated) {
                    send(client, "abc", 3, 0);
                } else for (;;) {
                    if (send(client, "x", 1, 0) <= 0) break;
                    std::unique_lock lock(mutex_);
                    if (cv_.wait_for(lock, std::chrono::milliseconds(30), [this] { return stopping_; })) break;
                }
            }
            closesocket(client);
        });
    }
    ~Server() {
        { std::lock_guard lock(mutex_); stopping_ = true; }
        cv_.notify_all();
        closesocket(listener_);
        if (thread_.joinable()) thread_.join();
    }
    std::wstring url() const { return L"http://127.0.0.1:"+std::to_wstring(port)+L"/test"; }
};
}
int main() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2,2), &data)) return 2;
    using namespace arkhelp::net;
    std::string body, error;
    {
        Server server(Server::Normal);
        check(http_get(server.url(), body, error, HttpOptions{1500}) && body == "abc" && error.empty(),
              "async transport reads a complete local response");
    }
    {
        Server server(Server::DelayedHeaders);
        const auto started = GetTickCount64();
        check(!http_get(server.url(), body, error, HttpOptions{180}) &&
              error.find("total deadline") != std::string::npos && GetTickCount64()-started < 1200,
              "total deadline ends a server that never sends headers");
    }
    {
        Server server(Server::Trickle);
        const auto started = GetTickCount64();
        check(!http_get(server.url(), body, error, HttpOptions{180}) && body.empty() &&
              GetTickCount64()-started < 1200, "trickling body does not reset the total deadline");
    }
    {
        Server server(Server::DelayedHeaders);
        std::atomic<bool> cancelled{false};
        std::thread cancel([&] {
            const auto end = GetTickCount64()+1000;
            while (!server.received && GetTickCount64()<end) Sleep(1);
            cancelled = true;
        });
        const auto started = GetTickCount64();
        check(!http_get(server.url(), body, error, HttpOptions{5000, &cancelled}) &&
              error.find("cancelled") != std::string::npos && GetTickCount64()-started < 1500,
              "selection/shutdown cancellation interrupts pending async I/O");
        cancel.join();
    }
    {
        Server server(Server::HttpFailure);
        check(!http_get(server.url(), body, error, HttpOptions{1500}) && error == "HTTP 503",
              "HTTP failure is terminal and preserves the status code");
    }
    {
        Server server(Server::Truncated);
        check(!http_get(server.url(), body, error, HttpOptions{1500}) && body.empty() && !error.empty(),
              "truncated response is an error rather than partial success");
    }
    WSACleanup();
    std::printf("HTTP: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
