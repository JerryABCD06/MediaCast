// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "QtFree/WrappedTsProxy.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using mcast::avalonia::qtfree::MediaLoadFrame;
using mcast::avalonia::qtfree::WrappedTsProxy;

int g_failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    ++g_failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::string lowercase(std::string value)
{
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

std::string wrappedPayload()
{
    std::string bytes("\x89PNG\r\n\x1a\n", 8);
    const auto appendChunk = [&bytes](
                                 const char *type,
                                 std::string body) {
        const std::uint32_t length =
            static_cast<std::uint32_t>(body.size());
        const char size[4] = {
            static_cast<char>((length >> 24) & 0xff),
            static_cast<char>((length >> 16) & 0xff),
            static_cast<char>((length >> 8) & 0xff),
            static_cast<char>(length & 0xff),
        };
        bytes.append(size, 4);
        bytes.append(type, 4);
        bytes += std::move(body);
        bytes.append(4, '\0');
    };
    appendChunk("IHDR", std::string(13, '\0'));
    appendChunk("IEND", {});
    for (int packet = 0; packet < 3; ++packet) {
        std::string data(188, '\0');
        data[0] = 0x47;
        data[1] = static_cast<char>(packet);
        bytes += data;
    }
    return bytes;
}

std::string imageOnlyPayload()
{
    std::string bytes("\x89PNG\r\n\x1a\n", 8);
    const char iend[] = {
        0, 0, 0, 0, 'I', 'E', 'N', 'D', 0, 0, 0, 0};
    bytes.append(iend, sizeof(iend));
    return bytes;
}

struct ParsedRequest
{
    std::string method;
    std::string path;
};

ParsedRequest parseRequest(const std::string &request)
{
    ParsedRequest result;
    const auto firstSpace = request.find(' ');
    const auto secondSpace = firstSpace == std::string::npos
        ? std::string::npos : request.find(' ', firstSpace + 1);
    if (firstSpace == std::string::npos
        || secondSpace == std::string::npos) {
        return result;
    }
    result.method = request.substr(0, firstSpace);
    result.path = request.substr(
        firstSpace + 1, secondSpace - firstSpace - 1);
    return result;
}

std::optional<std::pair<std::int64_t, std::int64_t>>
parseRange(const std::string &request)
{
    const std::string lower = lowercase(request);
    const auto position = lower.find("\r\nrange: bytes=");
    if (position == std::string::npos)
        return std::nullopt;
    const auto begin = position + 15;
    const auto end = request.find("\r\n", begin);
    const std::string value = request.substr(
        begin, end == std::string::npos
            ? std::string::npos : end - begin);
    const auto dash = value.find('-');
    if (dash == std::string::npos || dash == 0)
        return std::nullopt;
    std::int64_t start = 0;
    std::int64_t finish = -1;
    if (std::from_chars(
            value.data(), value.data() + dash, start).ec
        != std::errc{}) {
        return std::nullopt;
    }
    if (dash + 1 < value.size()) {
        if (std::from_chars(
                value.data() + dash + 1,
                value.data() + value.size(), finish).ec
            != std::errc{}) {
            finish = -1;
        }
    }
    return std::pair{start, finish};
}

bool sendAll(SOCKET socket, std::string_view data)
{
    while (!data.empty()) {
        const int sent = send(
            socket, data.data(), static_cast<int>(data.size()), 0);
        if (sent <= 0)
            return false;
        data.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
}

void sendResponse(
    SOCKET socket, int status, std::string_view reason,
    std::string headers, std::string body, bool headOnly)
{
    headers += "Content-Length: " + std::to_string(body.size())
        + "\r\nConnection: close\r\n\r\n";
    const std::string response =
        "HTTP/1.1 " + std::to_string(status) + " "
        + std::string(reason) + "\r\n" + headers;
    if (!sendAll(socket, response))
        return;
    if (!headOnly)
        sendAll(socket, body);
}

void sendPayload(
    SOCKET socket, const std::string &body,
    std::optional<std::pair<std::int64_t, std::int64_t>> range,
    bool headOnly)
{
    std::int64_t start = 0;
    std::int64_t end = static_cast<std::int64_t>(body.size()) - 1;
    int status = 200;
    std::string headers =
        "Content-Type: application/octet-stream\r\n"
        "Accept-Ranges: bytes\r\n";
    if (range) {
        start = std::clamp<std::int64_t>(
            range->first, 0, static_cast<std::int64_t>(body.size()) - 1);
        end = range->second >= 0
            ? std::clamp<std::int64_t>(
                  range->second, start,
                  static_cast<std::int64_t>(body.size()) - 1)
            : static_cast<std::int64_t>(body.size()) - 1;
        status = 206;
        headers += "Content-Range: bytes "
            + std::to_string(start) + "-" + std::to_string(end)
            + "/" + std::to_string(body.size()) + "\r\n";
    }
    sendResponse(
        socket, status,
        status == 206 ? "Partial Content" : "OK",
        std::move(headers),
        body.substr(
            static_cast<std::size_t>(start),
            static_cast<std::size_t>(end - start + 1)),
        headOnly);
}

class LocalHttpServer
{
public:
    LocalHttpServer(std::string wrapped, std::string imageOnly)
        : m_wrapped(std::move(wrapped))
        , m_imageOnly(std::move(imageOnly))
    {
    }

    ~LocalHttpServer()
    {
        stop();
    }

    bool start(std::string &error)
    {
        m_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_listener == INVALID_SOCKET) {
            error = "cannot create test listener";
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(
                m_listener,
                reinterpret_cast<const sockaddr *>(&address),
                sizeof(address))
                == SOCKET_ERROR
            || listen(m_listener, SOMAXCONN) == SOCKET_ERROR) {
            closesocket(m_listener);
            m_listener = INVALID_SOCKET;
            error = "cannot bind test listener";
            return false;
        }
        int length = sizeof(address);
        getsockname(
            m_listener,
            reinterpret_cast<sockaddr *>(&address), &length);
        m_port = ntohs(address.sin_port);
        m_running = true;
        m_acceptor = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested() && m_running.load()) {
                SOCKET client = accept(m_listener, nullptr, nullptr);
                if (client == INVALID_SOCKET)
                    break;
                const std::scoped_lock lock(m_workersMutex);
                m_workers.emplace_back(
                    [this, client](std::stop_token workerStop) {
                        serve(client, workerStop);
                    });
            }
        });
        return true;
    }

    void stop()
    {
        m_running = false;
        if (m_listener != INVALID_SOCKET) {
            shutdown(m_listener, SD_BOTH);
            closesocket(m_listener);
            m_listener = INVALID_SOCKET;
        }
        m_acceptor.request_stop();
        if (m_acceptor.joinable())
            m_acceptor.join();
        {
            const std::scoped_lock lock(m_workersMutex);
            for (auto &worker : m_workers)
                worker.request_stop();
        }
        m_slowCondition.notify_all();
        for (auto &worker : m_workers) {
            if (worker.joinable())
                worker.join();
        }
        m_workers.clear();
    }

    std::string url(std::string_view path) const
    {
        return "http://127.0.0.1:" + std::to_string(m_port)
            + std::string(path);
    }

    bool waitForSlowRequest()
    {
        std::unique_lock lock(m_slowMutex);
        return m_slowCondition.wait_for(
            lock, 5s, [this] { return m_slowStarted; });
    }

    std::atomic_int wrappedRequests{0};

private:
    void serve(SOCKET client, std::stop_token stop)
    {
        DWORD timeout = 5000;
        setsockopt(
            client, SOL_SOCKET, SO_RCVTIMEO,
            reinterpret_cast<const char *>(&timeout),
            sizeof(timeout));
        std::string request;
        char buffer[4096];
        while (request.find("\r\n\r\n") == std::string::npos
               && request.size() < 65536
               && !stop.stop_requested()) {
            const int read = recv(client, buffer, sizeof(buffer), 0);
            if (read <= 0)
                break;
            request.append(buffer, static_cast<std::size_t>(read));
        }
        const ParsedRequest parsed = parseRequest(request);
        if (parsed.path.rfind("/wrapped.png", 0) == 0) {
            ++wrappedRequests;
            sendPayload(
                client, m_wrapped, parseRange(request),
                parsed.method == "HEAD");
        } else if (parsed.path == "/plain.png") {
            sendPayload(
                client, m_imageOnly, parseRange(request),
                parsed.method == "HEAD");
        } else if (parsed.path == "/slow") {
            {
                const std::scoped_lock lock(m_slowMutex);
                m_slowStarted = true;
            }
            m_slowCondition.notify_all();
            std::unique_lock lock(m_slowMutex);
            m_slowCondition.wait_for(
                lock, 15s,
                [&stop] { return stop.stop_requested(); });
        } else {
            sendResponse(
                client, 404, "Not Found", {}, "not found", false);
        }
        shutdown(client, SD_BOTH);
        closesocket(client);
    }

    std::string m_wrapped;
    std::string m_imageOnly;
    SOCKET m_listener = INVALID_SOCKET;
    std::uint16_t m_port = 0;
    std::atomic_bool m_running{false};
    std::jthread m_acceptor;
    std::mutex m_workersMutex;
    std::vector<std::jthread> m_workers;
    std::mutex m_slowMutex;
    std::condition_variable_any m_slowCondition;
    bool m_slowStarted = false;
};

struct ClientResponse
{
    int status = 0;
    std::string headers;
    std::string body;
};

std::uint16_t portFromUrl(const std::string &url)
{
    const auto begin = url.find("127.0.0.1:") + 10;
    const auto end = url.find('/', begin);
    return static_cast<std::uint16_t>(
        std::stoi(url.substr(begin, end - begin)));
}

std::string pathFromUrl(const std::string &url)
{
    return url.substr(url.find('/', url.find("127.0.0.1:") + 10));
}

std::optional<ClientResponse> httpRequest(
    std::uint16_t port, const std::string &path,
    std::optional<std::string> range = std::nullopt,
    bool head = false)
{
    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET)
        return std::nullopt;
    DWORD timeout = 8000;
    setsockopt(
        client, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char *>(&timeout), sizeof(timeout));
    setsockopt(
        client, SOL_SOCKET, SO_SNDTIMEO,
        reinterpret_cast<const char *>(&timeout), sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(
            client,
            reinterpret_cast<const sockaddr *>(&address),
            sizeof(address))
        == SOCKET_ERROR) {
        closesocket(client);
        return std::nullopt;
    }
    std::string request =
        std::string(head ? "HEAD " : "GET ") + path
        + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n";
    if (range)
        request += "Range: bytes=" + *range + "\r\n";
    request += "\r\n";
    if (!sendAll(client, request)) {
        closesocket(client);
        return std::nullopt;
    }
    std::string bytes;
    char buffer[8192];
    while (true) {
        const int read = recv(client, buffer, sizeof(buffer), 0);
        if (read <= 0)
            break;
        bytes.append(buffer, static_cast<std::size_t>(read));
    }
    closesocket(client);

    const auto headerEnd = bytes.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
        return std::nullopt;
    ClientResponse response;
    response.headers = bytes.substr(0, headerEnd + 4);
    response.body = bytes.substr(headerEnd + 4);
    const auto firstSpace = response.headers.find(' ');
    if (firstSpace == std::string::npos)
        return std::nullopt;
    response.status = std::atoi(
        response.headers.c_str() + firstSpace + 1);
    return response;
}

} // namespace

int main()
{
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    const std::string wrapped = wrappedPayload();
    LocalHttpServer server(wrapped, imageOnlyPayload());
    std::string error;
    check(server.start(error), error.c_str());

    WrappedTsProxy proxy;
    check(
        proxy.start({{server.url("/wrapped.png"), 1.5}}, error),
        error.c_str());
    const auto playlist = httpRequest(
        portFromUrl(proxy.playlistUrl()),
        pathFromUrl(proxy.playlistUrl()));
    check(
        playlist && playlist->status == 200
            && playlist->body.find("#EXT-X-ENDLIST")
                != std::string::npos,
        "playlist is served");

    const std::string segmentPath =
        pathFromUrl(proxy.segmentUrl(0));
    const std::uint16_t proxyPort =
        portFromUrl(proxy.segmentUrl(0));
    const auto range = httpRequest(
        proxyPort, segmentPath, std::string("0-15"));
    check(
        range && range->status == 206 && range->body.size() == 16
            && range->body.front() == 0x47,
        "range request returns unwrapped transport stream");
    const auto head = httpRequest(
        proxyPort, segmentPath, std::string("0-15"), true);
    check(
        head && head->status == 206 && head->body.empty(),
        "HEAD returns range headers without a body");

    const int requestsAfterFetch = server.wrappedRequests.load();
    const auto full = httpRequest(proxyPort, segmentPath);
    check(
        full && full->status == 200
            && full->body.size() == wrapped.size() - 45,
        "full segment strips PNG wrapper");
    check(
        server.wrappedRequests.load() == requestsAfterFetch,
        "second segment request is served from cache");

    WrappedTsProxy plain;
    check(
        plain.start({{server.url("/plain.png"), 1.0}}, error),
        "plain image proxy starts");
    const auto rejected = httpRequest(
        portFromUrl(plain.segmentUrl(0)),
        pathFromUrl(plain.segmentUrl(0)));
    check(
        rejected && rejected->status == 502,
        "plain image is rejected with 502");

    std::atomic_int successes{0};
    std::vector<std::jthread> clients;
    for (int index = 0; index < 8; ++index) {
        clients.emplace_back([&] {
            const auto response = httpRequest(proxyPort, segmentPath);
            if (response && response->status == 200
                && !response->body.empty()
                && response->body.front() == 0x47) {
                ++successes;
            }
        });
    }
    for (auto &client : clients)
        client.join();
    check(successes.load() == 8,
          "fixed worker pool serves concurrent clients");

    WrappedTsProxy slow;
    check(
        slow.start({{server.url("/slow"), 1.0}}, error),
        "slow proxy starts");
    std::jthread slowClient([port = portFromUrl(slow.segmentUrl(0)),
                             path = pathFromUrl(slow.segmentUrl(0))] {
        httpRequest(port, path);
    });
    check(server.waitForSlowRequest(), "slow remote transfer entered");
    const auto begin = std::chrono::steady_clock::now();
    slow.reset();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    slowClient.join();
    check(elapsed < 8s, "reset stops transfer and worker pool promptly");

    proxy.reset();
    server.stop();
    WSACleanup();
    if (g_failures != 0) {
        std::cerr << g_failures << " wrapped-TS tests failed\n";
        return 1;
    }
    std::cout << "wrapped-TS proxy tests passed\n";
    return 0;
}
