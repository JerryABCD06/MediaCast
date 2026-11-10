// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/net/HttpServer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <limits>
#include <string_view>
#include <utility>

namespace mcast::dlna::net {
namespace {

const char *reasonPhrase(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 202:
        return "Accepted";
    case 204:
        return "No Content";
    case 206:
        return "Partial Content";
    case 301:
        return "Moved Permanently";
    case 302:
        return "Found";
    case 304:
        return "Not Modified";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 413:
        return "Payload Too Large";
    case 414:
        return "URI Too Long";
    case 415:
        return "Unsupported Media Type";
    case 429:
        return "Too Many Requests";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 502:
        return "Bad Gateway";
    case 503:
        return "Service Unavailable";
    case 504:
        return "Gateway Timeout";
    default:
        return "Unknown";
    }
}

int parseErrorStatus(std::string_view error)
{
    if (error.find("too many HTTP headers") != std::string_view::npos)
        return 431;
    if (error.find("header exceeds limit") != std::string_view::npos)
        return 431;
    if (error.find("body exceeds limit") != std::string_view::npos)
        return 413;
    if (error.find("transfer encoding") != std::string_view::npos)
        return 501;
    return 400;
}

bool token(std::string_view value)
{
    return !value.empty()
        && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isalnum(ch)
                || std::string_view("!#$%&'*+-.^_`|~").find(
                       static_cast<char>(ch))
                    != std::string_view::npos;
        });
}

bool headerValue(std::string_view value)
{
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch == '\t' || ch == ' ' || (ch >= 0x21 && ch <= 0x7e)
            || ch >= 0x80;
    });
}

std::string lower(std::string_view value)
{
    std::string result(value);
    std::transform(
        result.begin(), result.end(), result.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return result;
}

bool bodylessStatus(int status)
{
    return status == 204 || status == 205 || status == 304;
}

bool validResponseHeaders(const HttpResponse &response)
{
    constexpr std::size_t maxHeaders = 64;
    if (response.headers.size() > maxHeaders)
        return false;
    if (!headerValue(response.contentType))
        return false;
    for (const auto &[name, value] : response.headers) {
        if (!token(name) || !headerValue(value))
            return false;
        const std::string normalized = lower(name);
        if (normalized == "content-length" || normalized == "connection"
            || normalized == "transfer-encoding") {
            return false;
        }
    }
    return true;
}

bool addSize(std::size_t &total, std::size_t amount)
{
    const std::size_t maximum = std::numeric_limits<std::size_t>::max();
    if (amount > maximum - total)
        return false;
    total += amount;
    return true;
}

bool responseFits(
    const HttpResponse &response, std::size_t maximumBytes)
{
    const bool bodyless = bodylessStatus(response.status);
    std::size_t total = 0;
    const std::string status = std::to_string(response.status);
    if (!addSize(total, std::string_view("HTTP/1.1 ").size())
        || !addSize(total, status.size())
        || !addSize(total, 1)
        || !addSize(
            total, std::string_view(reasonPhrase(response.status)).size())
        || !addSize(total, 2)) {
        return false;
    }
    if (!bodyless) {
        if (!addSize(
                total, std::string_view("Content-Type: ").size())
            || !addSize(total, response.contentType.size())
            || !addSize(total, 2)
            || !addSize(
                total, std::string_view("Content-Length: ").size())
            || !addSize(total, std::to_string(response.body.size()).size())
            || !addSize(total, 2)) {
            return false;
        }
    }
    if (!addSize(
            total, std::string_view("Connection: close\r\n").size())
        || !addSize(total, 2)) {
        return false;
    }
    for (const auto &[name, value] : response.headers) {
        if (!addSize(total, name.size()) || !addSize(total, 2)
            || !addSize(total, value.size()) || !addSize(total, 2)) {
            return false;
        }
    }
    if (!bodyless && !addSize(total, response.body.size()))
        return false;
    return total <= maximumBytes;
}

std::string socketError(std::string_view operation)
{
    return std::string(operation) + " failed with WSA error "
        + std::to_string(WSAGetLastError());
}

std::string peerAddress(SOCKET socket)
{
    sockaddr_in address{};
    int size = sizeof(address);
    if (getpeername(
            socket, reinterpret_cast<sockaddr *>(&address), &size)
        == SOCKET_ERROR) {
        return {};
    }
    char text[INET_ADDRSTRLEN]{};
    if (!inet_ntop(
            AF_INET, &address.sin_addr, text, sizeof(text)))
        return {};
    return text;
}

std::vector<std::uint8_t> serializeResponse(const HttpResponse &response)
{
    const bool bodyless = bodylessStatus(response.status);
    std::string head = "HTTP/1.1 " + std::to_string(response.status)
        + " " + reasonPhrase(response.status) + "\r\n";
    if (!bodyless) {
        head += "Content-Type: " + response.contentType + "\r\n";
        head += "Content-Length: "
            + std::to_string(response.body.size()) + "\r\n";
    }
    head += "Connection: close\r\n";
    for (const auto &[name, value] : response.headers)
        head += name + ": " + value + "\r\n";
    head += "\r\n";

    std::vector<std::uint8_t> bytes;
    bytes.reserve(
        head.size() + (bodyless ? 0 : response.body.size()));
    bytes.insert(bytes.end(), head.begin(), head.end());
    if (!bodyless) {
        bytes.insert(
            bytes.end(), response.body.begin(), response.body.end());
    }
    return bytes;
}

} // namespace

HttpServer::~HttpServer()
{
    stop();
}

bool HttpServer::listenLoopback(
    std::uint16_t port, HttpRequestHandler handler,
    HttpServerOptions options, std::string &error)
{
    if (!handler) {
        stop();
        error = "HTTP request handler is required";
        return false;
    }
    AsyncHttpRequestHandler asyncHandler =
        [handler = std::move(handler)](
            const core::HttpRequest &request) {
            HttpDispatchResult result;
            result.response = handler(request);
            return result;
        };
    return listenLoopback(
        port, std::move(asyncHandler), std::move(options), error);
}

bool HttpServer::listenLoopback(
    std::uint16_t port, AsyncHttpRequestHandler handler,
    HttpServerOptions options, std::string &error)
{
    error.clear();
    if (m_callbackDepth != 0) {
        error = "HTTP server cannot restart from a callback";
        return false;
    }
    stop();
    if (!handler) {
        error = "HTTP request handler is required";
        return false;
    }
    if (options.maxConnections == 0
        || options.maxConnections
            > static_cast<std::size_t>(WSA_MAXIMUM_WAIT_EVENTS - 2)) {
        error = "invalid HTTP connection limit";
        return false;
    }
    if (options.maxResponseBytes < 128
        || options.requestTimeout.count() <= 0
        || options.pendingRequestTimeout.count() <= 0
        || options.limits.maxHeaderBytes == 0) {
        error = "invalid HTTP server limits";
        return false;
    }
    if (!m_winsock.initialize(error))
        return false;
    if (!m_listener.createTcp(error)
        || !m_listener.setNonBlocking(error)
        || !m_listener.bindLoopback(port, error)) {
        m_listener.close();
        return false;
    }
    if (listen(m_listener.get(), SOMAXCONN) == SOCKET_ERROR) {
        error = socketError("listen(HTTP)");
        m_listener.close();
        return false;
    }
    if (!m_loop.initialize(error)
        || !m_loop.add(
            m_listener.get(), FD_ACCEPT | FD_CLOSE, error)) {
        m_listener.close();
        return false;
    }

    m_handler = std::move(handler);
    m_options = std::move(options);
    m_stopRequested = false;
    m_running = true;
    {
        std::lock_guard lock(m_completionMutex);
        m_completions.clear();
        m_completionOpen = true;
    }
    if (m_options.logger) {
        log(
            "HTTP listening on 127.0.0.1:"
            + std::to_string(*m_listener.localPort()));
        if (!m_running) {
            error = "HTTP server stopped during startup";
            return false;
        }
    }
    return true;
}

void HttpServer::stop() noexcept
{
    m_running = false;
    {
        std::lock_guard lock(m_completionMutex);
        m_completionOpen = false;
        m_completions.clear();
    }
    if (m_callbackDepth != 0) {
        m_stopRequested = true;
        return;
    }
    finishStop();
}

void HttpServer::finishStop() noexcept
{
    if (m_listener.valid()) {
        m_loop.remove(m_listener.get());
        m_listener.close();
    }
    closeAllConnections();
    m_handler = {};
    {
        std::lock_guard lock(m_completionMutex);
        m_completionOpen = false;
        m_completions.clear();
    }
    m_acceptResume = false;
    m_stopRequested = false;
    m_running = false;
}

bool HttpServer::completeRequest(
    std::uint64_t requestId, HttpResponse response)
{
    if (requestId == 0)
        return false;
    {
        std::lock_guard lock(m_completionMutex);
        if (!m_completionOpen
            || m_completions.size() >= m_options.maxConnections) {
            return false;
        }
        m_completions.push_back(
            Completion{requestId, std::move(response)});
    }
    m_loop.wake();
    return true;
}

void HttpServer::log(const std::string &message)
{
    if (!m_options.logger)
        return;
    ++m_callbackDepth;
    try {
        m_options.logger(message);
    } catch (...) {
    }
    --m_callbackDepth;
    if (m_callbackDepth == 0 && m_stopRequested)
        finishStop();
}

bool HttpServer::pollOnce(
    std::chrono::milliseconds timeout, std::string &error)
{
    error.clear();
    if (m_polling) {
        error = "HTTP server poll is not reentrant";
        return false;
    }
    if (!m_running) {
        error = "HTTP server is not running";
        return false;
    }
    struct PollGuard {
        bool &polling;
        ~PollGuard() { polling = false; }
    } guard{ m_polling };
    m_polling = true;

    drainCompletions();
    expireTimers();
    if (!m_running) {
        error = "HTTP server stopped";
        return false;
    }
    if (m_acceptResume) {
        m_acceptResume = false;
        acceptPending(error);
        if (!error.empty()) {
            stop();
            return false;
        }
        if (!m_running) {
            error = "HTTP server stopped";
            return false;
        }
    }
    auto wait = std::max(timeout, std::chrono::milliseconds(0));
    if (const auto deadline = m_timers.nextDeadline()) {
        const auto now = std::chrono::steady_clock::now();
        if (*deadline <= now) {
            wait = std::chrono::milliseconds(0);
        } else {
            const auto untilDeadline =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    *deadline - now);
            wait = std::min(wait, untilDeadline);
        }
    }

    std::vector<platform::net::SocketActivation> activations;
    bool woken = false;
    if (!m_loop.poll(wait, activations, woken, error))
        return false;

    drainCompletions();
    for (const auto &activation : activations) {
        if (!m_running)
            break;
        if (activation.socket == m_listener.get()) {
            if ((activation.events & FD_CLOSE) != 0
                || activation.errorCodes[FD_ACCEPT_BIT] != 0) {
                error = "HTTP listener closed unexpectedly";
                stop();
                return false;
            }
            if ((activation.events & FD_ACCEPT) != 0) {
                acceptPending(error);
                if (!error.empty()) {
                    stop();
                    return false;
                }
            }
            continue;
        }

        if (!m_connections.contains(activation.socket))
            continue;
        if ((activation.events & FD_READ) != 0) {
            if (activation.errorCodes[FD_READ_BIT] != 0) {
                closeConnection(activation.socket);
                continue;
            }
            readConnection(activation.socket);
        }
        if (m_connections.contains(activation.socket)
            && (activation.events & FD_WRITE) != 0) {
            if (activation.errorCodes[FD_WRITE_BIT] != 0) {
                closeConnection(activation.socket);
                continue;
            }
            flushConnection(activation.socket);
        }
        if (m_connections.contains(activation.socket)
            && (activation.events & FD_CLOSE) != 0) {
            const int closeError = activation.errorCodes[FD_CLOSE_BIT];
            if (closeError != 0) {
                closeConnection(activation.socket);
                continue;
            }
            Connection &connection = m_connections.at(activation.socket);
            connection.peerClosed = true;
            if (connection.responseQueued
                || connection.requestPending)
                flushConnection(activation.socket);
            else
                closeConnection(activation.socket);
        }
    }

    drainCompletions();
    expireTimers();
    return true;
}

std::uint16_t HttpServer::localPort() const noexcept
{
    return m_listener.localPort().value_or(0);
}

void HttpServer::acceptPending(std::string &error)
{
    m_acceptResume = false;
    for (int attempt = 0; m_running && attempt < 64; ++attempt) {
        const SOCKET accepted = accept(m_listener.get(), nullptr, nullptr);
        if (accepted == INVALID_SOCKET) {
            const int code = WSAGetLastError();
            if (code == WSAEWOULDBLOCK)
                return;
            if (code == WSAECONNRESET || code == WSAECONNABORTED)
                continue;
            error = socketError("accept(HTTP)");
            return;
        }
        if (m_connections.size() >= m_options.maxConnections) {
            closesocket(accepted);
            log("HTTP connection limit reached");
            continue;
        }

        Connection connection;
        connection.socket = platform::net::SocketHandle(accepted);
        connection.peerAddress = peerAddress(accepted);
        if (!connection.socket.setNonBlocking(error)
            || !m_loop.add(
                connection.socket.get(), FD_READ | FD_WRITE | FD_CLOSE,
                error)) {
            connection.socket.close();
            return;
        }

        const SOCKET socket = connection.socket.get();
        connection.timer = m_timers.schedule(
            std::chrono::steady_clock::now() + m_options.requestTimeout);
        m_connections.emplace(socket, std::move(connection));
    }
    if (m_running)
        m_acceptResume = true;
}

void HttpServer::readConnection(SOCKET socket)
{
    const auto found = m_connections.find(socket);
    if (found == m_connections.end())
        return;
    Connection &connection = found->second;
    if (connection.responseQueued || connection.requestPending
        || connection.peerClosed)
        return;

    std::array<std::uint8_t, 8192> buffer{};
    while (true) {
        const std::size_t maximum = maximumBufferedBytes();
        if (connection.input.size() >= maximum) {
            queueErrorResponse(
                socket, connection, 413,
                "HTTP request exceeds configured limits");
            return;
        }
        const std::size_t remaining =
            maximum - connection.input.size();
        const std::size_t receiveCapacity =
            remaining < buffer.size() ? remaining + 1 : buffer.size();
        const int received = recv(
            socket, reinterpret_cast<char *>(buffer.data()),
            static_cast<int>(receiveCapacity), 0);
        if (received > 0) {
            connection.input.insert(
                connection.input.end(), buffer.begin(),
                buffer.begin() + received);
            processInput(socket, connection);
            if (!m_connections.contains(socket))
                return;
            if (m_connections.at(socket).responseQueued
                || m_connections.at(socket).requestPending)
                return;
            if (connection.input.size() > maximumBufferedBytes()) {
                queueErrorResponse(
                    socket, connection, 413,
                    "HTTP request exceeds configured limits");
                return;
            }
            continue;
        }
        if (received == 0) {
            closeConnection(socket);
            return;
        }
        const int code = WSAGetLastError();
        if (code == WSAEWOULDBLOCK)
            return;
        log(socketError("recv(HTTP)"));
        closeConnection(socket);
        return;
    }
}

void HttpServer::flushConnection(SOCKET socket)
{
    const auto found = m_connections.find(socket);
    if (found == m_connections.end())
        return;
    Connection &connection = found->second;
    if (!connection.responseQueued)
        return;

    while (connection.outputOffset < connection.output.size()) {
        const std::size_t remaining =
            connection.output.size() - connection.outputOffset;
        const int sendSize = static_cast<int>(std::min(
            remaining,
            static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const int sent = send(
            socket,
            reinterpret_cast<const char *>(
                connection.output.data() + connection.outputOffset),
            sendSize, 0);
        if (sent > 0) {
            connection.outputOffset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
            return;
        log(socketError("send(HTTP)"));
        closeConnection(socket);
        return;
    }
    closeConnection(socket);
}

void HttpServer::closeConnection(SOCKET socket) noexcept
{
    const auto found = m_connections.find(socket);
    if (found == m_connections.end())
        return;
    m_loop.remove(socket);
    m_timers.cancel(found->second.timer);
    m_connections.erase(found);
}

void HttpServer::closeAllConnections() noexcept
{
    std::vector<SOCKET> sockets;
    sockets.reserve(m_connections.size());
    for (const auto &[socket, connection] : m_connections) {
        static_cast<void>(connection);
        sockets.push_back(socket);
    }
    for (SOCKET socket : sockets)
        closeConnection(socket);
}

void HttpServer::expireTimers()
{
    const auto due =
        m_timers.takeDue(std::chrono::steady_clock::now());
    for (const auto timer : due) {
        auto found = std::find_if(
            m_connections.begin(), m_connections.end(),
            [&](const auto &item) {
                return item.second.timer == timer;
            });
        if (found == m_connections.end())
            continue;
        const SOCKET socket = found->first;
        log("HTTP connection timed out");
        closeConnection(socket);
    }
}

void HttpServer::drainCompletions()
{
    std::vector<Completion> completions;
    {
        std::lock_guard lock(m_completionMutex);
        completions.swap(m_completions);
    }
    for (auto &completion : completions) {
        if (!m_running)
            return;
        auto found = std::find_if(
            m_connections.begin(), m_connections.end(),
            [&](const auto &item) {
                return item.second.requestPending
                    && item.second.requestId == completion.requestId;
            });
        if (found == m_connections.end())
            continue;
        Connection &connection = found->second;
        if (std::chrono::steady_clock::now()
            >= connection.pendingDeadline) {
            continue;
        }
        connection.requestPending = false;
        connection.requestId = 0;
        queueResponse(
            found->first, connection, std::move(completion.response));
    }
}

void HttpServer::processInput(SOCKET socket, Connection &connection)
{
    core::HttpRequest request;
    std::size_t consumed = 0;
    std::string parseError;
    const auto status = core::parseHttpRequest(
        connection.input, request, consumed, parseError, m_options.limits);
    if (status == core::HttpParseStatus::NeedMoreData)
        return;
    if (status == core::HttpParseStatus::Error) {
        queueErrorResponse(
            socket, connection, parseErrorStatus(parseError), parseError);
        return;
    }

    if (m_nextRequestId == 0)
        m_nextRequestId = 1;
    request.requestId = m_nextRequestId++;
    request.peerAddress = connection.peerAddress;

    HttpDispatchResult result;
    ++m_callbackDepth;
    try {
        result = m_handler(request);
    } catch (const std::exception &exception) {
        log(std::string("HTTP handler exception: ") + exception.what());
        result = {};
        result.response.status = 500;
        result.response.body = "HTTP handler failed";
    } catch (...) {
        log("HTTP handler threw an unknown exception");
        result = {};
        result.response.status = 500;
        result.response.body = "HTTP handler failed";
    }
    --m_callbackDepth;
    if (m_callbackDepth == 0 && m_stopRequested) {
        finishStop();
        return;
    }
    if (!m_connections.contains(socket))
        return;
    Connection &current = m_connections.at(socket);
    current.input.clear();
    if (result.status == HttpDispatchStatus::Pending) {
        current.requestId = request.requestId;
        current.requestPending = true;
        current.pendingDeadline =
            std::chrono::steady_clock::now()
            + m_options.pendingRequestTimeout;
        m_timers.cancel(current.timer);
        current.timer = m_timers.schedule(current.pendingDeadline);
        return;
    }
    if (result.status != HttpDispatchStatus::Completed) {
        result.response = {};
        result.response.status = 500;
        result.response.body = "HTTP handler returned an invalid state";
    }
    queueResponse(
        socket, current, std::move(result.response));
}

void HttpServer::queueResponse(
    SOCKET socket, Connection &connection, HttpResponse response)
{
    if (response.status < 200 || response.status > 599)
        response.status = 500;
    if (bodylessStatus(response.status))
        response.body.clear();
    if (!validResponseHeaders(response)
        || !responseFits(response, m_options.maxResponseBytes)) {
        response = {};
        response.status = 500;
        response.body.clear();
    }
    connection.input.clear();
    connection.requestPending = false;
    connection.requestId = 0;
    connection.pendingDeadline = {};
    connection.output = serializeResponse(response);
    connection.outputOffset = 0;
    connection.responseQueued = true;
    m_timers.cancel(connection.timer);
    connection.timer = m_timers.schedule(
        std::chrono::steady_clock::now() + m_options.requestTimeout);
    flushConnection(socket);
}

void HttpServer::queueErrorResponse(
    SOCKET socket, Connection &connection, int status,
    std::string message)
{
    HttpResponse response;
    response.status = status;
    response.body = std::move(message);
    queueResponse(socket, connection, std::move(response));
}

std::size_t HttpServer::maximumBufferedBytes() const noexcept
{
    constexpr std::size_t overhead = 4;
    const std::size_t maximum = std::numeric_limits<std::size_t>::max();
    if (m_options.limits.maxHeaderBytes > maximum - overhead)
        return maximum;
    const std::size_t header = m_options.limits.maxHeaderBytes + overhead;
    if (m_options.limits.maxBodyBytes > maximum - header)
        return maximum;
    return header + m_options.limits.maxBodyBytes;
}

} // namespace mcast::dlna::net
