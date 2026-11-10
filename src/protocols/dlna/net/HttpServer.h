// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "platform/windows/net/SocketRuntime.h"
#include "protocols/dlna/core/HttpRequest.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
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

using HttpRequestHandler =
    std::function<HttpResponse(const core::HttpRequest &)>;

struct HttpServerOptions {
    std::size_t maxConnections = 32;
    std::size_t maxResponseBytes = 1024 * 1024;
    std::chrono::milliseconds requestTimeout{10000};
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

    bool listenLoopback(
        std::uint16_t port, HttpRequestHandler handler,
        HttpServerOptions options, std::string &error);
    void stop() noexcept;

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
        bool responseQueued = false;
        bool peerClosed = false;
    };

    void acceptPending(std::string &error);
    void readConnection(SOCKET socket);
    void flushConnection(SOCKET socket);
    void closeConnection(SOCKET socket) noexcept;
    void closeAllConnections() noexcept;
    void finishStop() noexcept;
    void log(const std::string &message);
    void expireTimers();
    void processInput(SOCKET socket, Connection &connection);
    void queueResponse(SOCKET socket, Connection &connection, HttpResponse response);
    void queueErrorResponse(
        SOCKET socket, Connection &connection, int status,
        std::string message);
    std::size_t maximumBufferedBytes() const noexcept;

    platform::net::WinsockRuntime m_winsock;
    platform::net::SocketHandle m_listener;
    platform::net::SocketEventLoop m_loop;
    platform::net::TimerQueue m_timers;
    std::unordered_map<SOCKET, Connection> m_connections;
    HttpRequestHandler m_handler;
    HttpServerOptions m_options;
    std::size_t m_callbackDepth = 0;
    bool m_stopRequested = false;
    bool m_polling = false;
    bool m_acceptResume = false;
    bool m_running = false;
};

} // namespace mcast::dlna::net
