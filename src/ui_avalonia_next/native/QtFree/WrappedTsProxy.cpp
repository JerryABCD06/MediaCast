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
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <utility>

namespace mcast::avalonia::qtfree {

struct WrappedTsProxy::State
{
    struct CachedSegment
    {
        std::string body;
        std::uint64_t lastUsed = 0;
    };

    bool winsockStarted = false;
    std::atomic<SOCKET> listener{INVALID_SOCKET};
    std::uint16_t port = 0;
    std::string token;
    std::vector<MediaLoadFrame> segments;
    std::map<std::size_t, CachedSegment> cache;
    std::size_t cacheBytes = 0;
    std::uint64_t cacheClock = 0;
    std::mutex cacheMutex;
    std::atomic_bool running{false};
    std::jthread acceptor;
    std::deque<SOCKET> pending;
    std::unordered_set<SOCKET> active;
    std::mutex queueMutex;
    std::condition_variable_any queueReady;
    std::vector<std::jthread> workers;
};

namespace {

std::atomic_uint64_t g_proxyId{1};

constexpr std::size_t kWorkerCount = 4;
constexpr std::size_t kPendingConnectionLimit = 32;
constexpr std::size_t kCacheByteLimit = 64u * 1024u * 1024u;
constexpr std::size_t kMaxRemotePayload = 256u * 1024u * 1024u;
constexpr DWORD kSocketTimeoutMs = 5000;
constexpr int kHttpTimeoutMs = 3000;

bool stopRequested(
    const std::shared_ptr<WrappedTsProxy::State> &state,
    const std::stop_token &stop)
{
    return stop.stop_requested() || !state->running.load();
}

bool setSocketTimeout(SOCKET socket, int option, DWORD milliseconds)
{
    return setsockopt(
               socket, SOL_SOCKET, option,
               reinterpret_cast<const char *>(&milliseconds),
               sizeof(milliseconds))
        != SOCKET_ERROR;
}

std::optional<std::int64_t> wrappedOffset(std::string_view bytes)
{
    static constexpr unsigned char signature[8] = {
        0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (bytes.size() < 8
        || std::memcmp(bytes.data(), signature, 8) != 0) {
        return std::nullopt;
    }

    std::size_t offset = 8;
    for (int chunks = 0;
         chunks < 128 && offset + 12 <= bytes.size();
         ++chunks) {
        const auto byteAt = [&](std::size_t index) {
            return static_cast<unsigned char>(bytes[index]);
        };
        const std::uint32_t length =
            (static_cast<std::uint32_t>(byteAt(offset)) << 24)
            | (static_cast<std::uint32_t>(byteAt(offset + 1)) << 16)
            | (static_cast<std::uint32_t>(byteAt(offset + 2)) << 8)
            | static_cast<std::uint32_t>(byteAt(offset + 3));
        const std::size_t next =
            offset + 12 + static_cast<std::size_t>(length);
        if (next < offset || next > bytes.size())
            return std::nullopt;
        if (std::memcmp(bytes.data() + offset + 4, "IEND", 4) == 0) {
            constexpr std::size_t packet = 188;
            if (next + 2 * packet >= bytes.size())
                return std::nullopt;
            for (std::size_t index = 0; index < 3; ++index) {
                if (static_cast<unsigned char>(
                        bytes[next + index * packet])
                    != 0x47) {
                    return std::nullopt;
                }
            }
            return static_cast<std::int64_t>(next);
        }
        offset = next;
    }
    return std::nullopt;
}

std::wstring widen(std::string_view value)
{
    if (value.empty())
        return {};
    const int length = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), length);
    return result;
}

struct HttpResult
{
    int status = 0;
    std::string body;
};

std::optional<HttpResult> httpGet(
    const std::shared_ptr<WrappedTsProxy::State> &state,
    const std::stop_token &stop,
    const std::string &url,
    std::optional<std::string> range)
{
    if (stopRequested(state, stop))
        return std::nullopt;

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    const std::wstring wideUrl = widen(url);
    if (!WinHttpCrackUrl(
            wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()),
            0, &parts)) {
        return std::nullopt;
    }

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo && parts.dwExtraInfoLength)
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    HINTERNET session = WinHttpOpen(
        L"MCast.QtFree/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return std::nullopt;
    WinHttpSetTimeouts(
        session, kHttpTimeoutMs, kHttpTimeoutMs,
        kHttpTimeoutMs, kHttpTimeoutMs);
    HINTERNET connection =
        WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    HINTERNET request = connection
        ? WinHttpOpenRequest(
              connection, L"GET", path.c_str(), nullptr,
              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
              parts.nScheme == INTERNET_SCHEME_HTTPS
                  ? WINHTTP_FLAG_SECURE : 0)
        : nullptr;
    if (request && range) {
        const std::wstring header =
            L"Range: bytes=" + widen(*range);
        if (!WinHttpAddRequestHeaders(
                request, header.c_str(),
                static_cast<DWORD>(header.size()),
                WINHTTP_ADDREQ_FLAG_ADD
                    | WINHTTP_ADDREQ_FLAG_REPLACE)) {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return std::nullopt;
        }
    }

    bool ok = !stopRequested(state, stop) && request
        && WinHttpSendRequest(
            request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(request, nullptr);
    HttpResult result;
    if (ok) {
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE
                | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status,
            &statusSize, WINHTTP_NO_HEADER_INDEX);
        result.status = static_cast<int>(status);
        while (ok && !stopRequested(state, stop)) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) {
                ok = false;
                break;
            }
            if (available == 0)
                break;
            const std::size_t begin = result.body.size();
            if (begin >= kMaxRemotePayload
                || available > kMaxRemotePayload - begin) {
                ok = false;
                break;
            }
            result.body.resize(begin + available);
            DWORD read = 0;
            if (!WinHttpReadData(
                    request, result.body.data() + begin,
                    available, &read)) {
                ok = false;
                break;
            }
            result.body.resize(begin + read);
        }
        if (stopRequested(state, stop))
            ok = false;
    }

    if (request)
        WinHttpCloseHandle(request);
    if (connection)
        WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (!ok || result.status < 200 || result.status >= 300)
        return std::nullopt;
    return result;
}

std::optional<std::string> fetchSegment(
    const std::shared_ptr<WrappedTsProxy::State> &state,
    const std::stop_token &stop,
    const std::string &url)
{
    auto first = httpGet(state, stop, url, std::string("0-262143"));
    if (!first)
        return std::nullopt;
    const auto offset = wrappedOffset(first->body);
    if (!offset)
        return std::nullopt;

    if (first->status == 200
        && first->body.size() > static_cast<std::size_t>(*offset)) {
        first->body.erase(0, static_cast<std::size_t>(*offset));
        return std::move(first->body);
    }
    if (stopRequested(state, stop))
        return std::nullopt;
    auto payload = httpGet(
        state, stop, url, std::to_string(*offset) + "-");
    if (!payload)
        return std::nullopt;
    if (payload->status == 200
        && payload->body.size() > static_cast<std::size_t>(*offset)) {
        payload->body.erase(0, static_cast<std::size_t>(*offset));
    }
    if (payload->body.size() > kMaxRemotePayload)
        return std::nullopt;
    return std::move(payload->body);
}

struct RangeRequest
{
    bool present = false;
    bool satisfiable = true;
    std::int64_t first = 0;
    std::int64_t last = -1;
    bool suffix = false;
};

RangeRequest parseRange(std::string_view request)
{
    RangeRequest result;
    const std::string lower = lowercaseAscii(request);
    const auto position = lower.find("\r\nrange: bytes=");
    if (position == std::string::npos)
        return result;
    result.present = true;
    result.satisfiable = false;
    const auto begin = position + 15;
    const auto end = request.find("\r\n", begin);
    const std::string value(request.substr(
        begin, end == std::string_view::npos
            ? std::string_view::npos : end - begin));
    const auto dash = value.find('-');
    if (dash == std::string::npos)
        return result;
    if (dash == 0) {
        if (dash + 1 >= value.size())
            return result;
        std::int64_t suffixLength = 0;
        if (std::from_chars(
                value.data() + 1,
                value.data() + value.size(),
                suffixLength).ec
                != std::errc{}
            || suffixLength <= 0) {
            return result;
        }
        result.satisfiable = true;
        result.first = suffixLength;
        result.suffix = true;
        return result;
    }

    std::int64_t first = 0;
    auto parsed = std::from_chars(
        value.data(), value.data() + dash, first);
    if (parsed.ec != std::errc{}
        || parsed.ptr != value.data() + dash) {
        return result;
    }
    std::int64_t last = -1;
    if (dash + 1 < value.size()) {
        if (std::from_chars(
                value.data() + dash + 1,
                value.data() + value.size(), last).ec
            != std::errc{}) {
            return result;
        }
    }
    result.satisfiable = true;
    result.first = first;
    result.last = last;
    return result;
}

bool sendAll(
    const std::shared_ptr<WrappedTsProxy::State> &state,
    const std::stop_token &stop,
    SOCKET socket, const char *data, std::size_t size)
{
    while (size > 0 && !stopRequested(state, stop)) {
        const int chunk = static_cast<int>(std::min<std::size_t>(
            size, static_cast<std::size_t>(
                std::numeric_limits<int>::max())));
        const int sent = send(socket, data, chunk, 0);
        if (sent <= 0)
            return false;
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

void respond(
    const std::shared_ptr<WrappedTsProxy::State> &state,
    const std::stop_token &stop,
    SOCKET socket, std::string body,
    RangeRequest range,
    bool headOnly, std::string_view contentType)
{
    std::int64_t start = 0;
    std::int64_t end =
        body.empty() ? -1 : static_cast<std::int64_t>(body.size()) - 1;
    int status = 200;
    std::string contentRange;
    if (range.present && (body.empty() || !range.satisfiable)) {
        const std::string response =
            "HTTP/1.1 416 Range Not Satisfiable\r\n"
            "Content-Range: bytes */" + std::to_string(body.size())
            + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        sendAll(state, stop, socket, response.data(), response.size());
        return;
    }
    if (range.present) {
        if (range.suffix) {
            start = std::max<std::int64_t>(
                0, static_cast<std::int64_t>(body.size())
                    - range.first);
            end = static_cast<std::int64_t>(body.size()) - 1;
        } else {
            if (range.first >= static_cast<std::int64_t>(body.size())
                || (range.last >= 0 && range.last < range.first)) {
                const std::string response =
                    "HTTP/1.1 416 Range Not Satisfiable\r\n"
                    "Content-Range: bytes */"
                    + std::to_string(body.size())
                    + "\r\nContent-Length: 0\r\n"
                    "Connection: close\r\n\r\n";
                sendAll(
                    state, stop, socket,
                    response.data(), response.size());
                return;
            }
            start = range.first;
            end = range.last >= 0
                ? std::min<std::int64_t>(
                      range.last,
                      static_cast<std::int64_t>(body.size()) - 1)
                : static_cast<std::int64_t>(body.size()) - 1;
        }
        status = 206;
        contentRange =
            "Content-Range: bytes " + std::to_string(start)
            + "-" + std::to_string(end) + "/"
            + std::to_string(body.size()) + "\r\n";
    }
    const std::int64_t length =
        body.empty() ? 0 : end - start + 1;
    const std::string response =
        "HTTP/1.1 " + std::to_string(status) + " "
        + (status == 206 ? "Partial Content" : "OK")
        + "\r\nContent-Type: " + std::string(contentType)
        + "\r\nAccept-Ranges: bytes\r\n" + contentRange
        + "Content-Length: " + std::to_string(length)
        + "\r\nConnection: close\r\n\r\n";
    if (!sendAll(state, stop, socket, response.data(), response.size()))
        return;
    if (!headOnly && length > 0) {
        sendAll(
            state, stop, socket, body.data() + start,
            static_cast<std::size_t>(length));
    }
}

void respondError(
    const std::shared_ptr<WrappedTsProxy::State> &state,
    const std::stop_token &stop,
    SOCKET socket, int status, std::string_view reason,
    std::string body)
{
    const std::string response =
        "HTTP/1.1 " + std::to_string(status) + " "
        + std::string(reason)
        + "\r\nContent-Type: text/plain\r\nContent-Length: "
        + std::to_string(body.size())
        + "\r\nConnection: close\r\n\r\n";
    if (!sendAll(state, stop, socket, response.data(), response.size()))
        return;
    sendAll(state, stop, socket, body.data(), body.size());
}

void handleConnection(
    const std::shared_ptr<WrappedTsProxy::State> &state,
    SOCKET socket, const std::stop_token &stop)
{
    setSocketTimeout(socket, SO_RCVTIMEO, kSocketTimeoutMs);
    setSocketTimeout(socket, SO_SNDTIMEO, kSocketTimeoutMs);

    std::string request;
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos
           && request.size() < 65536
           && !stopRequested(state, stop)) {
        const int read = recv(socket, buffer, sizeof(buffer), 0);
        if (read <= 0)
            break;
        request.append(buffer, static_cast<std::size_t>(read));
    }

    const auto firstSpace = request.find(' ');
    const auto secondSpace = firstSpace == std::string::npos
        ? std::string::npos : request.find(' ', firstSpace + 1);
    if (firstSpace == std::string::npos
        || secondSpace == std::string::npos) {
        respondError(
            state, stop, socket, 400, "Bad Request", "bad request");
        return;
    }
    const std::string method = request.substr(0, firstSpace);
    const std::string path = request.substr(
        firstSpace + 1, secondSpace - firstSpace - 1);

    const std::string playlistPath =
        "/" + state->token + "/playlist.m3u8";
    if (path == playlistPath) {
        double target = 0;
        for (const auto &segment : state->segments) {
            target = std::max(
                target, std::clamp(
                    segment.durationSeconds, 0.05, 3600.0));
        }
        std::string playlist =
            "#EXTM3U\n#EXT-X-VERSION:3\n"
            "#EXT-X-MEDIA-SEQUENCE:0\n"
            "#EXT-X-PLAYLIST-TYPE:VOD\n"
            "#EXT-X-INDEPENDENT-SEGMENTS\n";
        playlist += "#EXT-X-TARGETDURATION:"
            + std::to_string(static_cast<int>(std::ceil(target))) + "\n";
        for (std::size_t index = 0;
             index < state->segments.size(); ++index) {
            char duration[64]{};
            std::snprintf(
                duration, sizeof(duration), "%.6f",
                std::clamp(
                    state->segments[index].durationSeconds,
                    0.05, 3600.0));
            playlist += "#EXTINF:" + std::string(duration) + ",\n";
            playlist += "http://127.0.0.1:"
                + std::to_string(state->port) + "/"
                + state->token + "/segment/"
                + std::to_string(index) + "\n";
        }
        playlist += "#EXT-X-ENDLIST\n";
        respond(
            state, stop, socket, std::move(playlist), RangeRequest{},
            method == "HEAD", "application/vnd.apple.mpegurl");
        return;
    }

    const std::string prefix = "/" + state->token + "/segment/";
    if (path.rfind(prefix, 0) != 0) {
        respondError(
            state, stop, socket, 404, "Not Found", "unknown path");
        return;
    }
    std::size_t index = 0;
    const std::string indexText = path.substr(prefix.size());
    const auto parsed = std::from_chars(
        indexText.data(), indexText.data() + indexText.size(), index);
    if (parsed.ec != std::errc{}
        || index >= state->segments.size()) {
        respondError(
            state, stop, socket, 404, "Not Found", "bad segment");
        return;
    }

    std::string payload;
    {
        const std::scoped_lock lock(state->cacheMutex);
        const auto found = state->cache.find(index);
        if (found != state->cache.end()) {
            found->second.lastUsed = ++state->cacheClock;
            payload = found->second.body;
        }
    }
    if (payload.empty()) {
        const auto fetched =
            fetchSegment(state, stop, state->segments[index].uri);
        if (!fetched) {
            if (stopRequested(state, stop))
                return;
            respondError(
                state, stop, socket, 502, "Bad Gateway",
                "remote segment failed");
            return;
        }
        payload = *fetched;
        const std::scoped_lock lock(state->cacheMutex);
        if (payload.size() <= kCacheByteLimit) {
            while (!state->cache.empty()
                   && state->cacheBytes + payload.size()
                       > kCacheByteLimit) {
                auto oldest = state->cache.begin();
                for (auto item = std::next(oldest);
                     item != state->cache.end(); ++item) {
                    if (item->second.lastUsed
                        < oldest->second.lastUsed) {
                        oldest = item;
                    }
                }
                state->cacheBytes -= oldest->second.body.size();
                state->cache.erase(oldest);
            }
            auto [item, inserted] = state->cache.emplace(
                index,
                WrappedTsProxy::State::CachedSegment{
                    payload, ++state->cacheClock});
            if (!inserted) {
                state->cacheBytes -= item->second.body.size();
                item->second.body = payload;
                item->second.lastUsed = ++state->cacheClock;
            }
            state->cacheBytes += item->second.body.size();
        }
    }
    respond(
        state, stop, socket, std::move(payload), parseRange(request),
        method == "HEAD", "video/mp2t");
}

} // namespace

WrappedTsProxy::WrappedTsProxy()
    : m_state(std::make_shared<State>())
{
    WSADATA data{};
    m_state->winsockStarted =
        WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

WrappedTsProxy::~WrappedTsProxy()
{
    reset();
    if (m_state->winsockStarted) {
        WSACleanup();
        m_state->winsockStarted = false;
    }
}

bool WrappedTsProxy::start(
    std::vector<MediaLoadFrame> segments, std::string &error)
{
    reset();
    if (!m_state->winsockStarted) {
        error = "Winsock initialization failed";
        return false;
    }
    if (segments.empty()) {
        error = "image sequence is empty";
        return false;
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        error = "cannot create local proxy listener";
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(
            listener,
            reinterpret_cast<const sockaddr *>(&address),
            sizeof(address))
            == SOCKET_ERROR
        || listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(listener);
        error = "cannot bind local proxy listener";
        return false;
    }
    int length = sizeof(address);
    if (getsockname(
            listener, reinterpret_cast<sockaddr *>(&address),
            &length)
        == SOCKET_ERROR) {
        closesocket(listener);
        error = "cannot query local proxy listener";
        return false;
    }

    m_state->listener.store(listener);
    m_state->port = ntohs(address.sin_port);
    m_state->segments = std::move(segments);
    m_state->token =
        "qtfree-" + std::to_string(GetCurrentProcessId()) + "-"
        + std::to_string(g_proxyId.fetch_add(1));
    m_state->running = true;
    m_state->acceptor = std::jthread(
        [state = m_state](std::stop_token stop) {
            while (!stop.stop_requested() && state->running.load()) {
                SOCKET client = accept(
                    state->listener.load(), nullptr, nullptr);
                if (client == INVALID_SOCKET)
                    break;
                bool queued = false;
                {
                    const std::scoped_lock lock(state->queueMutex);
                    if (state->running.load()
                        && state->pending.size()
                            < kPendingConnectionLimit) {
                        state->pending.push_back(client);
                        queued = true;
                    }
                }
                if (queued) {
                    state->queueReady.notify_one();
                } else {
                    shutdown(client, SD_BOTH);
                    closesocket(client);
                }
            }
        });
    m_state->workers.reserve(kWorkerCount);
    for (std::size_t index = 0; index < kWorkerCount; ++index) {
        m_state->workers.emplace_back(
            [state = m_state](std::stop_token stop) {
                while (true) {
                    SOCKET client = INVALID_SOCKET;
                    {
                        std::unique_lock lock(state->queueMutex);
                        state->queueReady.wait(
                            lock, stop, [&state] {
                                return !state->running.load()
                                    || !state->pending.empty();
                            });
                        if (stop.stop_requested()
                            || (!state->running.load()
                                && state->pending.empty())) {
                            break;
                        }
                        client = state->pending.front();
                        state->pending.pop_front();
                        state->active.insert(client);
                    }
                    handleConnection(state, client, stop);
                    {
                        const std::scoped_lock lock(state->queueMutex);
                        state->active.erase(client);
                    }
                    shutdown(client, SD_BOTH);
                    closesocket(client);
                }
            });
    }
    return true;
}

void WrappedTsProxy::reset()
{
    if (!m_state)
        return;
    m_state->running = false;
    const SOCKET listener =
        m_state->listener.exchange(INVALID_SOCKET);
    if (listener != INVALID_SOCKET) {
        shutdown(listener, SD_BOTH);
        closesocket(listener);
    }
    m_state->acceptor.request_stop();
    m_state->queueReady.notify_all();
    if (m_state->acceptor.joinable())
        m_state->acceptor.join();

    {
        const std::scoped_lock lock(m_state->queueMutex);
        for (SOCKET socket : m_state->pending) {
            shutdown(socket, SD_BOTH);
            closesocket(socket);
        }
        m_state->pending.clear();
        for (SOCKET socket : m_state->active)
            shutdown(socket, SD_BOTH);
        for (auto &worker : m_state->workers)
            worker.request_stop();
    }
    m_state->queueReady.notify_all();
    for (auto &worker : m_state->workers) {
        if (worker.joinable())
            worker.join();
    }
    m_state->workers.clear();
    {
        const std::scoped_lock lock(m_state->cacheMutex);
        m_state->cache.clear();
        m_state->cacheBytes = 0;
        m_state->cacheClock = 0;
    }
}

std::string WrappedTsProxy::segmentUrl(std::size_t index) const
{
    return "http://127.0.0.1:" + std::to_string(m_state->port)
        + "/" + m_state->token + "/segment/" + std::to_string(index);
}

std::string WrappedTsProxy::playlistUrl() const
{
    return "http://127.0.0.1:" + std::to_string(m_state->port)
        + "/" + m_state->token + "/playlist.m3u8";
}

} // namespace mcast::avalonia::qtfree
