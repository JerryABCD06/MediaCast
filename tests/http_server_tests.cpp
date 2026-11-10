// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "platform/windows/net/SocketRuntime.h"
#include "protocols/dlna/net/HttpServer.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace mcast::dlna::net;
using namespace mcast::platform::net;
using namespace std::chrono_literals;

namespace {

void check(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

bool sendAll(SOCKET socket, std::string_view bytes, std::string &error)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const int sent = send(
            socket, bytes.data() + offset,
            static_cast<int>(bytes.size() - offset), 0);
        if (sent == SOCKET_ERROR) {
            error = "client send failed with WSA error "
                + std::to_string(WSAGetLastError());
            return false;
        }
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

SocketHandle connectClient(std::uint16_t port, std::string &error)
{
    SocketHandle socket;
    if (!socket.createTcp(error))
        return {};
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    destination.sin_port = htons(port);
    if (connect(
            socket.get(),
            reinterpret_cast<const sockaddr *>(&destination),
            sizeof(destination)) == SOCKET_ERROR) {
        error = "client connect failed with WSA error "
            + std::to_string(WSAGetLastError());
        return {};
    }
    const DWORD timeout = 5000;
    setsockopt(
        socket.get(), SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char *>(&timeout), sizeof(timeout));
    return socket;
}

struct ClientOutcome {
    bool completed = false;
    std::string response;
    std::string error;
};

ClientOutcome exchange(
    std::uint16_t port, std::vector<std::string> fragments,
    std::chrono::milliseconds fragmentDelay = 0ms,
    bool halfClose = false)
{
    ClientOutcome outcome;
    std::string connectError;
    SocketHandle socket = connectClient(port, connectError);
    if (!socket.valid()) {
        outcome.error = connectError;
        return outcome;
    }

    for (std::size_t index = 0; index < fragments.size(); ++index) {
        if (!sendAll(socket.get(), fragments[index], outcome.error))
            return outcome;
        if (fragmentDelay.count() > 0 && index + 1 < fragments.size())
            std::this_thread::sleep_for(fragmentDelay);
    }
    if (halfClose && shutdown(socket.get(), SD_SEND) == SOCKET_ERROR) {
        outcome.error = "client half-close failed with WSA error "
            + std::to_string(WSAGetLastError());
        return outcome;
    }

    char buffer[4096];
    while (true) {
        const int received =
            recv(socket.get(), buffer, sizeof(buffer), 0);
        if (received > 0) {
            outcome.response.append(
                buffer, static_cast<std::size_t>(received));
            continue;
        }
        if (received == 0) {
            outcome.completed = true;
            return outcome;
        }
        const int code = WSAGetLastError();
        if (code == WSAECONNRESET || code == WSAECONNABORTED
            || code == WSAENETRESET || code == WSAESHUTDOWN) {
            outcome.completed = true;
        } else {
            outcome.error = "client recv failed with WSA error "
                + std::to_string(code);
        }
        return outcome;
    }
}

ClientOutcome runExchange(
    HttpServer &server, std::vector<std::string> fragments,
    std::chrono::milliseconds fragmentDelay = 0ms,
    bool mayStopServer = false)
{
    ClientOutcome outcome;
    std::atomic_bool done{false};
    std::jthread client([&] {
        outcome = exchange(server.localPort(), std::move(fragments),
                           fragmentDelay);
        done = true;
    });

    const auto deadline = std::chrono::steady_clock::now() + 8s;
    std::string error;
    while (!done && std::chrono::steady_clock::now() < deadline) {
        if (!server.pollOnce(25ms, error)) {
            if (mayStopServer)
                break;
            check(false, "HTTP server polls: " + error);
        }
    }
    client.join();
    check(done, "HTTP client exchange completes");
    return outcome;
}

void pollUntilNoConnections(
    HttpServer &server, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string error;
    while (server.activeConnections() != 0
           && std::chrono::steady_clock::now() < deadline) {
        check(server.pollOnce(20ms, error),
              "HTTP server polls during close: " + error);
    }
    check(server.activeConnections() == 0,
          "HTTP connections are eventually closed");
}

} // namespace

int main()
{
    std::string error;
    WinsockRuntime runtime;
    check(runtime.initialize(error), "Winsock initializes");

    HttpServer server;
    HttpServerOptions options;
    options.logger = [](const std::string &) {};
    check(
        server.listenLoopback(
            0,
            [](const mcast::dlna::core::HttpRequest &request) {
                HttpResponse response;
                if (request.target.starts_with(
                        "/description.xml?source=test")
                    && request.method == "GET") {
                    response.contentType = "text/xml; charset=\"utf-8\"";
                    response.body = "<root>description</root>";
                    response.headers.emplace_back("X-Test", "yes");
                    return response;
                }
                if (request.target == "/post" && request.method == "POST") {
                    response.body = request.body;
                    return response;
                }
                if (request.target == "/inject") {
                    response.headers.emplace_back(
                        "X-Test", "ok\r\nInjected: yes");
                    return response;
                }
                if (request.target == "/framing") {
                    response.headers.emplace_back(
                        "Content-Length", "999");
                    return response;
                }
                if (request.target == "/no-content") {
                    response.status = 204;
                    response.body = "must not be sent";
                    return response;
                }
                response.status = 404;
                response.body = "missing";
                return response;
            },
            options, error),
        "HTTP server starts on an ephemeral port");
    check(server.localPort() != 0 && server.localPort() != 8200,
          "HTTP server uses an ephemeral test port");

    auto outcome = runExchange(
        server,
        {
            "GET /description.xml?source=test HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n",
            "X-Fragment: yes\r\n\r\n",
        },
        20ms);
    check(outcome.error.empty(), "fragmented GET client succeeds");
    check(outcome.response.starts_with("HTTP/1.1 200 OK\r\n"),
          "fragmented GET returns 200");
    check(outcome.response.find("X-Test: yes\r\n") != std::string::npos,
          "HTTP response headers are serialized");
    check(outcome.response.ends_with("<root>description</root>"),
          "HTTP response body is serialized");
    check(outcome.response.find("Connection: close\r\n")
              != std::string::npos,
          "HTTP response requests close");

    const std::string body = "SOAP-BODY";
    outcome = runExchange(
        server,
        {
            "POST /post HTTP/1.1\r\nContent-Length: "
            + std::to_string(body.size()) + "\r\n\r\n"
            + body.substr(0, 4),
            body.substr(4),
        },
        20ms);
    check(outcome.response.ends_with(body),
          "fragmented POST body reaches handler");

    outcome = runExchange(
        server, {"GET /missing HTTP/1.1\r\nHost: local\r\n\r\n"});
    check(outcome.response.starts_with("HTTP/1.1 404 Not Found\r\n"),
          "handler status is preserved");

    outcome = runExchange(
        server, {"GET /inject HTTP/1.1\r\nHost: local\r\n\r\n"});
    check(outcome.response.starts_with(
              "HTTP/1.1 500 Internal Server Error\r\n")
              && outcome.response.find("Injected: yes")
                  == std::string::npos,
          "response header injection is rejected");

    outcome = runExchange(
        server, {"GET /framing HTTP/1.1\r\nHost: local\r\n\r\n"});
    check(outcome.response.starts_with(
              "HTTP/1.1 500 Internal Server Error\r\n"),
          "response framing headers cannot be overridden");

    outcome = runExchange(
        server, {"GET /no-content HTTP/1.1\r\nHost: local\r\n\r\n"});
    check(outcome.response.starts_with("HTTP/1.1 204 No Content\r\n")
              && outcome.response.find("Content-Length:")
                  == std::string::npos
              && outcome.response.find("must not be sent")
                  == std::string::npos,
          "204 response is serialized without a body");

    server.stop();

    HttpServer asyncServer;
    HttpServerOptions asyncOptions;
    asyncOptions.pendingRequestTimeout = 3s;
    std::atomic_uint64_t pendingRequestId{0};
    std::atomic_bool peerAddressMatched{false};
    check(
        asyncServer.listenLoopback(
            0,
            [&](const mcast::dlna::core::HttpRequest &request) {
                if (request.target == "/pending") {
                    pendingRequestId = request.requestId;
                    peerAddressMatched =
                        request.peerAddress == "127.0.0.1";
                    HttpDispatchResult result;
                    result.status = HttpDispatchStatus::Pending;
                    return result;
                }
                HttpDispatchResult result;
                result.response.body = "immediate";
                return result;
            },
            asyncOptions, error),
        "async HTTP server starts");

    ClientOutcome pendingOutcome;
    std::atomic_bool pendingClientDone{false};
    std::jthread pendingClient([&] {
        pendingOutcome = exchange(
            asyncServer.localPort(),
            {"GET /pending HTTP/1.1\r\nHost: local\r\n\r\n"});
        pendingClientDone = true;
    });
    const auto pendingDeadline =
        std::chrono::steady_clock::now() + 2s;
    while (pendingRequestId.load() == 0
           && std::chrono::steady_clock::now() < pendingDeadline) {
        check(asyncServer.pollOnce(20ms, error),
              "async HTTP server accepts pending request: " + error);
    }
    check(pendingRequestId.load() != 0,
          "async HTTP handler receives a nonzero request ID");
    check(peerAddressMatched.load(),
          "HTTP request carries the peer address");
    check(asyncServer.activeConnections() == 1
              && !pendingClientDone.load(),
          "pending HTTP request remains connected without a response");

    outcome = runExchange(
        asyncServer,
        {"GET /fast HTTP/1.1\r\nHost: local\r\n\r\n"});
    check(outcome.response.ends_with("immediate"),
          "pending request does not block another HTTP connection");

    std::atomic_bool completionAccepted{false};
    std::atomic_bool allowCompletion{false};
    std::atomic_bool pollEntered{false};
    std::atomic_bool pollReturned{false};
    std::string blockedPollError;
    std::jthread completeRequest([&] {
        while (!allowCompletion.load())
            std::this_thread::yield();
        HttpResponse response;
        response.body = "released";
        completionAccepted = asyncServer.completeRequest(
            pendingRequestId.load(), std::move(response));
    });
    std::jthread blockedPoller([&] {
        pollEntered = true;
        if (!asyncServer.pollOnce(2s, blockedPollError))
            blockedPollError = "blocked poll failed: " + blockedPollError;
        pollReturned = true;
    });
    while (!pollEntered.load())
        std::this_thread::yield();
    std::this_thread::sleep_for(100ms);
    check(!pollReturned.load(),
          "HTTP poll is blocked before cross-thread completion");
    const auto blockedPollStarted = std::chrono::steady_clock::now();
    allowCompletion = true;
    blockedPoller.join();
    const auto blockedPollElapsed =
        std::chrono::steady_clock::now() - blockedPollStarted;
    completeRequest.join();
    check(blockedPollError.empty() && completionAccepted.load()
              && blockedPollElapsed < 1s,
          "cross-thread HTTP completion wakes a blocked poll");

    const auto completionDeadline =
        std::chrono::steady_clock::now() + 2s;
    while (!pendingClientDone.load()
           && std::chrono::steady_clock::now() < completionDeadline) {
        check(asyncServer.pollOnce(20ms, error),
              "async HTTP server writes delayed response: " + error);
    }
    pendingClient.join();
    check(pendingClientDone.load(),
          "delayed HTTP client receives a response");
    check(pendingOutcome.error.empty()
              && pendingOutcome.response.starts_with("HTTP/1.1 200 OK\r\n")
              && pendingOutcome.response.ends_with("released"),
          "delayed HTTP response is serialized");
    asyncServer.stop();

    HttpServer halfCloseServer;
    HttpServerOptions halfCloseOptions;
    halfCloseOptions.pendingRequestTimeout = 3s;
    std::atomic_uint64_t halfCloseRequestId{0};
    check(
        halfCloseServer.listenLoopback(
            0,
            [&](const mcast::dlna::core::HttpRequest &request) {
                halfCloseRequestId = request.requestId;
                HttpDispatchResult result;
                result.status = HttpDispatchStatus::Pending;
                return result;
            },
            halfCloseOptions, error),
        "half-close HTTP server starts");
    ClientOutcome halfCloseOutcome;
    std::atomic_bool halfCloseClientDone{false};
    std::jthread halfCloseClient([&] {
        halfCloseOutcome = exchange(
            halfCloseServer.localPort(),
            {"GET /half-close HTTP/1.1\r\nHost: local\r\n\r\n"},
            0ms, true);
        halfCloseClientDone = true;
    });
    const auto halfCloseDeadline =
        std::chrono::steady_clock::now() + 2s;
    while (halfCloseRequestId.load() == 0
           && std::chrono::steady_clock::now() < halfCloseDeadline) {
        check(halfCloseServer.pollOnce(20ms, error),
              "half-close HTTP server accepts request: " + error);
    }
    check(halfCloseRequestId.load() != 0,
          "half-close request reaches handler");
    const auto halfCloseSeen =
        std::chrono::steady_clock::now() + 200ms;
    while (!halfCloseClientDone.load()
           && std::chrono::steady_clock::now() < halfCloseSeen) {
        check(halfCloseServer.pollOnce(20ms, error),
              "half-close HTTP server observes client close: " + error);
    }
    check(!halfCloseClientDone.load(),
          "HTTP half-close keeps a pending response alive");
    HttpResponse halfCloseResponse;
    halfCloseResponse.body = "after-half-close";
    check(halfCloseServer.completeRequest(
              halfCloseRequestId.load(),
              std::move(halfCloseResponse)),
          "half-close pending response can complete");
    const auto halfCloseCompletionDeadline =
        std::chrono::steady_clock::now() + 2s;
    while (!halfCloseClientDone.load()
           && std::chrono::steady_clock::now()
               < halfCloseCompletionDeadline) {
        check(halfCloseServer.pollOnce(20ms, error),
              "half-close HTTP server writes response: " + error);
    }
    halfCloseClient.join();
    check(halfCloseOutcome.response.ends_with("after-half-close"),
          "HTTP response is written after client half-close");
    halfCloseServer.stop();

    HttpServer asyncTimeoutServer;
    HttpServerOptions asyncTimeoutOptions;
    asyncTimeoutOptions.pendingRequestTimeout = 150ms;
    check(
        asyncTimeoutServer.listenLoopback(
            0,
            [](const mcast::dlna::core::HttpRequest &) {
                HttpDispatchResult result;
                result.status = HttpDispatchStatus::Pending;
                return result;
            },
            asyncTimeoutOptions, error),
        "pending-timeout HTTP server starts");
    const auto pendingTimeoutStarted = std::chrono::steady_clock::now();
    outcome = runExchange(
        asyncTimeoutServer,
        {"GET /pending HTTP/1.1\r\nHost: local\r\n\r\n"});
    const auto pendingTimeoutElapsed =
        std::chrono::steady_clock::now() - pendingTimeoutStarted;
    check(outcome.completed && outcome.response.empty()
              && pendingTimeoutElapsed >= 100ms,
          "pending HTTP request closes on its independent timeout");
    check(asyncTimeoutServer.activeConnections() == 0,
          "pending HTTP timeout removes the connection");
    asyncTimeoutServer.stop();

    HttpServer asyncStopServer;
    HttpServerOptions asyncStopOptions;
    asyncStopOptions.pendingRequestTimeout = 3s;
    std::atomic_uint64_t stopRequestId{0};
    check(
        asyncStopServer.listenLoopback(
            0,
            [&](const mcast::dlna::core::HttpRequest &request) {
                stopRequestId = request.requestId;
                HttpDispatchResult result;
                result.status = HttpDispatchStatus::Pending;
                return result;
            },
            asyncStopOptions, error),
        "stop-test HTTP server starts");
    ClientOutcome stoppedOutcome;
    std::atomic_bool stoppedClientDone{false};
    std::jthread stoppedClient([&] {
        stoppedOutcome = exchange(
            asyncStopServer.localPort(),
            {"GET /pending HTTP/1.1\r\nHost: local\r\n\r\n"});
        stoppedClientDone = true;
    });
    const auto stoppedDeadline =
        std::chrono::steady_clock::now() + 2s;
    while (stopRequestId.load() == 0
           && std::chrono::steady_clock::now() < stoppedDeadline) {
        check(asyncStopServer.pollOnce(20ms, error),
              "stop-test HTTP server accepts request: " + error);
    }
    check(stopRequestId.load() != 0,
          "stop-test HTTP request reaches handler");
    asyncStopServer.stop();
    stoppedClient.join();
    check(stoppedClientDone.load()
              && stoppedOutcome.response.empty(),
          "server stop closes a pending HTTP request");
    check(!asyncStopServer.completeRequest(
              stopRequestId.load(), HttpResponse{}),
          "HTTP completion is rejected after server stop");

    HttpServer limited;
    HttpServerOptions limitedOptions;
    limitedOptions.limits.maxHeaderBytes = 128;
    limitedOptions.limits.maxBodyBytes = 16;
    limitedOptions.limits.maxHeaderCount = 2;
    check(
        limited.listenLoopback(
            0,
            [](const mcast::dlna::core::HttpRequest &) {
                HttpResponse response;
                response.body = "ok";
                return response;
            },
            limitedOptions, error),
        "limited HTTP server starts");

    outcome = runExchange(
        limited,
        {"GET / HTTP/1.1\r\nX-Large: "
         + std::string(140, 'x') + "\r\n\r\n"});
    check(outcome.response.starts_with(
              "HTTP/1.1 431 Request Header Fields Too Large\r\n"),
          "oversized header returns 431, got: " + outcome.response);

    outcome = runExchange(
        limited,
        {"GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n"});
    check(outcome.response.starts_with(
              "HTTP/1.1 431 Request Header Fields Too Large\r\n"),
          "too many headers return 431");

    outcome = runExchange(
        limited,
        {"POST / HTTP/1.1\r\nContent-Length: 17\r\n\r\n"});
    check(outcome.response.starts_with(
              "HTTP/1.1 413 Payload Too Large\r\n"),
          "oversized Content-Length is rejected");

    outcome = runExchange(
        limited,
        {"POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n"});
    check(outcome.response.starts_with("HTTP/1.1 400 Bad Request\r\n"),
          "negative Content-Length is rejected");

    outcome = runExchange(
        limited,
        {"POST / HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n\r\n"});
    check(outcome.response.starts_with("HTTP/1.1 400 Bad Request\r\n"),
          "overflowing Content-Length is rejected");

    outcome = runExchange(
        limited, {"POST / HTTP/1.1\r\nHost: local\r\n\r\n"});
    check(outcome.response.starts_with("HTTP/1.1 200 OK\r\n"),
          "missing Content-Length is accepted for a bodyless request");

    const std::string maximumBody(16, 'm');
    outcome = runExchange(
        limited,
        {"POST / HTTP/1.1\r\nContent-Length: 16\r\n\r\n" + maximumBody});
    check(outcome.response.starts_with("HTTP/1.1 200 OK\r\n"),
          "body exactly at the configured limit is accepted");
    limited.stop();

    HttpServer timeoutServer;
    HttpServerOptions timeoutOptions;
    timeoutOptions.requestTimeout = 200ms;
    std::atomic_int timeoutLogs{0};
    timeoutOptions.logger = [&](const std::string &message) {
        if (message == "HTTP connection timed out")
            ++timeoutLogs;
    };
    check(
        timeoutServer.listenLoopback(
            0,
            [](const mcast::dlna::core::HttpRequest &) {
                return HttpResponse{};
            },
            timeoutOptions, error),
        "timeout HTTP server starts");
    const auto timeoutStarted = std::chrono::steady_clock::now();
    outcome = runExchange(
        timeoutServer, {"GET / HTTP/1.1\r\nHost:"});
    const auto timeoutElapsed =
        std::chrono::steady_clock::now() - timeoutStarted;
    check(outcome.completed && outcome.response.empty(),
          "slow request is closed without a response");
    check(timeoutElapsed >= 150ms && timeoutLogs.load() != 0,
          "slow request closes on the configured timeout");
    check(timeoutServer.activeConnections() == 0,
          "slow request connection is removed");
    timeoutServer.stop();

    HttpServer exceptionServer;
    check(
        exceptionServer.listenLoopback(
            0,
            [](const mcast::dlna::core::HttpRequest &) -> HttpResponse {
                throw std::runtime_error("secret internal path");
            },
            HttpServerOptions{}, error),
        "exception HTTP server starts");
    outcome = runExchange(
        exceptionServer, {"GET / HTTP/1.1\r\nHost: local\r\n\r\n"});
    check(outcome.response.starts_with(
              "HTTP/1.1 500 Internal Server Error\r\n"),
          "handler exception becomes HTTP 500");
    check(outcome.response.find("secret internal path")
              == std::string::npos,
          "handler exception details are not exposed");
    exceptionServer.stop();

    HttpServer responseLimited;
    HttpServerOptions responseLimitedOptions;
    responseLimitedOptions.maxResponseBytes = 128;
    check(
        responseLimited.listenLoopback(
            0,
            [](const mcast::dlna::core::HttpRequest &) {
                HttpResponse response;
                response.body = "small";
                response.headers.emplace_back(
                    "X-Huge", std::string(200, 'x'));
                return response;
            },
            responseLimitedOptions, error),
        "response-limited HTTP server starts");
    outcome = runExchange(
        responseLimited, {"GET / HTTP/1.1\r\nHost: local\r\n\r\n"});
    check(outcome.response.starts_with(
              "HTTP/1.1 500 Internal Server Error\r\n")
              && outcome.response.find("Content-Length: 0\r\n")
                  != std::string::npos
              && outcome.response.find("X-Huge:")
                  == std::string::npos,
          "oversized handler response is rejected");
    responseLimited.stop();

    HttpServer handlerReentrant;
    HttpServer *handlerReentrantPtr = &handlerReentrant;
    check(
        handlerReentrant.listenLoopback(
            0,
            [handlerReentrantPtr](
                const mcast::dlna::core::HttpRequest &) {
                handlerReentrantPtr->stop();
                return HttpResponse{};
            },
            HttpServerOptions{}, error),
        "reentrant handler server starts");
    outcome = runExchange(
        handlerReentrant, {"GET / HTTP/1.1\r\nHost: local\r\n\r\n"},
        0ms, true);
    check(outcome.completed && outcome.response.empty()
              && handlerReentrant.activeConnections() == 0,
          "handler can stop the server safely");

    HttpServer restartReentrant;
    HttpServerOptions restartReentrantOptions;
    std::atomic_bool restartRejected{false};
    std::atomic_bool nestedPollRejected{false};
    HttpServer *restartReentrantPtr = &restartReentrant;
    check(
        restartReentrant.listenLoopback(
            0,
            [restartReentrantPtr, &restartRejected, &nestedPollRejected](
                const mcast::dlna::core::HttpRequest &) {
                std::string nestedError;
                auto nestedHandler =
                    [](const mcast::dlna::core::HttpRequest &) {
                        return HttpResponse{};
                    };
                if (!restartReentrantPtr->listenLoopback(
                        0, std::move(nestedHandler),
                        HttpServerOptions{}, nestedError)) {
                    restartRejected = true;
                }
                if (!restartReentrantPtr->pollOnce(1ms, nestedError))
                    nestedPollRejected = true;
                HttpResponse response;
                response.body = "handled";
                return response;
            },
            restartReentrantOptions, error),
        "restart-guard HTTP server starts");
    outcome = runExchange(
        restartReentrant, {"GET / HTTP/1.1\r\nHost: local\r\n\r\n"});
    check(outcome.response.starts_with("HTTP/1.1 200 OK\r\n")
              && restartRejected.load()
              && nestedPollRejected.load(),
          "callback cannot restart or recursively poll the server");
    restartReentrant.stop();

    HttpServer loggerReentrant;
    HttpServer *loggerReentrantPtr = &loggerReentrant;
    HttpServerOptions loggerReentrantOptions;
    loggerReentrantOptions.requestTimeout = 200ms;
    loggerReentrantOptions.logger = [loggerReentrantPtr](
                                        const std::string &message) {
        if (message == "HTTP connection timed out")
            loggerReentrantPtr->stop();
    };
    check(
        loggerReentrant.listenLoopback(
            0,
            [](const mcast::dlna::core::HttpRequest &) {
                return HttpResponse{};
            },
            loggerReentrantOptions, error),
        "reentrant logger server starts");
    outcome = runExchange(
        loggerReentrant, {"GET / HTTP/1.1\r\nHost:"}, 0ms, true);
    check(outcome.completed && loggerReentrant.activeConnections() == 0,
          "logger can stop the server safely");

    HttpServer connectionLimited;
    HttpServerOptions connectionOptions;
    connectionOptions.maxConnections = 2;
    connectionOptions.requestTimeout = 3s;
    check(
        connectionLimited.listenLoopback(
            0,
            [](const mcast::dlna::core::HttpRequest &) {
                return HttpResponse{};
            },
            connectionOptions, error),
        "connection-limited HTTP server starts");

    std::vector<SocketHandle> idle;
    for (int index = 0; index < 2; ++index) {
        idle.push_back(connectClient(connectionLimited.localPort(), error));
        check(idle.back().valid(), "idle HTTP client connects");
    }
    const auto acceptedDeadline =
        std::chrono::steady_clock::now() + 2s;
    while (connectionLimited.activeConnections() < 2
           && std::chrono::steady_clock::now() < acceptedDeadline) {
        check(connectionLimited.pollOnce(20ms, error),
              "connection-limited server accepts idle clients: " + error);
    }
    check(connectionLimited.activeConnections() == 2,
          "connection limit admits configured clients");
    outcome = runExchange(
        connectionLimited, {"GET / HTTP/1.1\r\nHost: local\r\n\r\n"});
    check(outcome.completed && outcome.response.empty(),
          "connection above limit is closed without a response: completed="
              + std::string(outcome.completed ? "true" : "false")
              + " error=" + outcome.error
              + " response=" + outcome.response);
    idle.clear();
    pollUntilNoConnections(connectionLimited, 2s);
    connectionLimited.stop();

    std::cout << "HTTP server tests passed\n";
    return 0;
}
