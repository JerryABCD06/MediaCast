// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "platform/windows/net/SocketRuntime.h"
#include "protocols/dlna/core/HttpRequest.h"

#include <chrono>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mcast::dlna::net {

struct HttpResponse {
    int status = 200;
    std::string contentType = "text/plain; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

enum class HttpDispatchStatus {
    Completed,
    Pending,
};

struct HttpDispatchResult {
    HttpDispatchStatus status = HttpDispatchStatus::Completed;
    HttpResponse response;
};

using HttpRequestHandler =
    std::function<HttpResponse(const core::HttpRequest &)>;
using AsyncHttpRequestHandler =
    std::function<HttpDispatchResult(const core::HttpRequest &)>;

struct HttpServerOptions {
    std::size_t maxConnections = 32;
    std::size_t maxResponseBytes = 1024 * 1024;
    std::chrono::milliseconds requestTimeout{10000};
    std::chrono::milliseconds pendingRequestTimeout{60000};
    core::HttpParseLimits limits;
    std::function<void(const std::string &)> logger;
};

class HttpServer
{
public:
    HttpServer() = default;
    ~HttpServer();

    HttpServer(const HttpServer &) = delete;
    HttpServer &operator=(const HttpServer &) = delete;

    // listenLoopback(), stop(), pollOnce(), localPort() and
    // activeConnections() are owner-thread operations and must be serialized.
    // completeRequest() may run on another thread, but all producers must be
    // stopped before the server owner destroys or restarts this object.
    bool listenLoopback(
        std::uint16_t port, HttpRequestHandler handler,
        HttpServerOptions options, std::string &error);
    bool listenLoopback(
        std::uint16_t port, AsyncHttpRequestHandler handler,
        HttpServerOptions options, std::string &error);
    bool listenAny(
        std::uint16_t port, HttpRequestHandler handler,
        HttpServerOptions options, std::string &error);
    bool listenAny(
        std::uint16_t port, AsyncHttpRequestHandler handler,
        HttpServerOptions options, std::string &error);
    void stop() noexcept;

    // Thread-safe. This only accepts the completion for later validation and
    // queues it; presence, deadline and duplicate checks happen on the
    // pollOnce() thread. Return false means the queue was closed or full.
    bool completeRequest(
        std::uint64_t requestId, HttpResponse response);

    bool pollOnce(
        std::chrono::milliseconds timeout, std::string &error);

    std::uint16_t localPort() const noexcept;
    std::size_t activeConnections() const noexcept
    {
        return m_connections.size();
    }
    void wake() noexcept { m_loop.wake(); }

private:
    struct Connection {
        platform::net::SocketHandle socket;
        std::vector<std::uint8_t> input;
        std::vector<std::uint8_t> output;
        std::size_t outputOffset = 0;
        platform::net::TimerQueue::TimerId timer = 0;
        std::chrono::steady_clock::time_point pendingDeadline{};
        std::uint64_t requestId = 0;
        std::string peerAddress;
        bool responseQueued = false;
        bool requestPending = false;
        bool peerClosed = false;
    };

    struct Completion {
        std::uint64_t requestId = 0;
        HttpResponse response;
    };

    void acceptPending(std::string &error);
    void readConnection(SOCKET socket);
    void flushConnection(SOCKET socket);
    void closeConnection(SOCKET socket) noexcept;
    void closeAllConnections() noexcept;
    void finishStop() noexcept;
    void log(const std::string &message);
    void expireTimers();
    void drainCompletions();
    void processInput(SOCKET socket, Connection &connection);
    void queueResponse(SOCKET socket, Connection &connection, HttpResponse response);
    void queueErrorResponse(
        SOCKET socket, Connection &connection, int status,
        std::string message);
    bool listenOn(
        std::uint16_t port, bool loopback,
        AsyncHttpRequestHandler handler,
        HttpServerOptions options, std::string &error);
    std::size_t maximumBufferedBytes() const noexcept;

    platform::net::WinsockRuntime m_winsock;
    platform::net::SocketHandle m_listener;
    platform::net::SocketEventLoop m_loop;
    platform::net::TimerQueue m_timers;
    std::unordered_map<SOCKET, Connection> m_connections;
    AsyncHttpRequestHandler m_handler;
    HttpServerOptions m_options;
    std::mutex m_completionMutex;
    std::vector<Completion> m_completions;
    std::uint64_t m_nextRequestId = 1;
    std::size_t m_callbackDepth = 0;
    bool m_stopRequested = false;
    bool m_polling = false;
    bool m_acceptResume = false;
    bool m_completionOpen = false;
    bool m_running = false;
};

} // namespace mcast::dlna::net
