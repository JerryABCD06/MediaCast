// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/runtime/GenaManager.h"

#include "protocols/dlna/UpnpXml.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <optional>
#include <random>
#include <sstream>
#include <string_view>
#include <utility>

namespace mcast::dlna::runtime {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::string_view kServerHeader =
    "Windows/10.0 UPnP/1.0 MediaCast/0.1";

struct Endpoint {
    std::string host;
    std::string hostHeader;
    std::string target;
    std::uint16_t port = 80;
};

std::string trim(std::string value)
{
    const auto whitespace = [](unsigned char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    };
    while (!value.empty() && whitespace(value.front()))
        value.erase(value.begin());
    while (!value.empty() && whitespace(value.back()))
        value.pop_back();
    return value;
}

std::string lower(std::string value)
{
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

std::optional<Endpoint> parseCallback(std::string_view header)
{
    std::string value = trim(std::string(header));
    const std::size_t open = value.find('<');
    if (open != std::string::npos) {
        const std::size_t close = value.find('>', open + 1);
        if (close == std::string::npos)
            return std::nullopt;
        value = value.substr(open + 1, close - open - 1);
    }
    if (lower(value.substr(0, 7)) != "http://")
        return std::nullopt;

    const std::size_t authorityStart = 7;
    const std::size_t pathStart =
        value.find_first_of("/?", authorityStart);
    const std::string authority = value.substr(
        authorityStart,
        pathStart == std::string::npos
            ? std::string::npos : pathStart - authorityStart);
    if (authority.empty()
        || authority.find('@') != std::string::npos) {
        return std::nullopt;
    }

    std::string host;
    std::string hostHeader;
    std::uint16_t port = 80;
    const auto parsePort = [&](std::string_view text) {
        if (text.empty()
            || !std::all_of(
                text.begin(), text.end(),
                [](unsigned char ch) { return std::isdigit(ch); })) {
            return false;
        }
        try {
            const unsigned long parsed = std::stoul(std::string(text));
            if (parsed == 0 || parsed > 65535)
                return false;
            port = static_cast<std::uint16_t>(parsed);
            return true;
        } catch (...) {
            return false;
        }
    };
    if (authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string::npos)
            return std::nullopt;
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':'
                || !parsePort(authority.substr(close + 2))) {
                return std::nullopt;
            }
        }
        hostHeader =
            "[" + host + "]:" + std::to_string(port);
    } else {
        host = authority;
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            if (authority.find(':') != colon)
                return std::nullopt;
            host = authority.substr(0, colon);
            if (!parsePort(authority.substr(colon + 1)))
                return std::nullopt;
        }
        hostHeader = host + ":" + std::to_string(port);
    }
    if (host.empty())
        return std::nullopt;

    std::string target =
        pathStart == std::string::npos
        ? "/" : value.substr(pathStart);
    const std::size_t fragment = target.find('#');
    if (fragment != std::string::npos)
        target.resize(fragment);
    if (target.empty())
        target = "/";
    if (target.starts_with("?"))
        target.insert(target.begin(), '/');
    return Endpoint{
        std::move(host),
        std::move(hostHeader),
        std::move(target),
        port,
    };
}

net::HttpResponse makeResponse(
    int status, std::string body,
    std::vector<std::pair<std::string, std::string>> headers = {})
{
    net::HttpResponse response;
    response.status = status;
    response.contentType.clear();
    response.body = std::move(body);
    response.headers = std::move(headers);
    return response;
}

std::string serviceForTarget(std::string_view target)
{
    const std::size_t query = target.find('?');
    if (query != std::string_view::npos)
        target = target.substr(0, query);
    if (target == "/AVTransport/event")
        return "AVTransport";
    if (target == "/RenderingControl/event")
        return "RenderingControl";
    if (target == "/ConnectionManager/event")
        return "ConnectionManager";
    return {};
}

std::string durationText(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0)
        seconds = 0.0;
    const auto total = static_cast<std::int64_t>(seconds);
    std::ostringstream output;
    output << total / 3600 << ':' << std::setw(2)
           << std::setfill('0') << (total % 3600) / 60 << ':'
           << std::setw(2) << total % 60;
    return output.str();
}

std::string dlnaState(playback::PlaybackState state)
{
    using playback::PlaybackState;
    switch (state) {
    case PlaybackState::NoMedia:
        return "NO_MEDIA_PRESENT";
    case PlaybackState::Preparing:
        return "TRANSITIONING";
    case PlaybackState::Playing:
        return "PLAYING";
    case PlaybackState::Paused:
        return "PAUSED_PLAYBACK";
    case PlaybackState::Stopped:
        return "STOPPED";
    }
    return "NO_MEDIA_PRESENT";
}

std::string dlnaPlayMode(playback::PlayMode mode)
{
    switch (mode) {
    case playback::PlayMode::Normal:
        return "NORMAL";
    case playback::PlayMode::RepeatOne:
        return "REPEAT_ONE";
    case playback::PlayMode::RepeatAll:
        return "REPEAT_ALL";
    case playback::PlayMode::Direct:
        return "DIRECT_1";
    }
    return "NORMAL";
}

int pictureValue(
    const playback::PlaybackSnapshot &snapshot, std::string_view name)
{
    const auto found = std::find_if(
        snapshot.pictureControls.begin(),
        snapshot.pictureControls.end(),
        [&](const playback::PictureControl &control) {
            return control.name == name;
        });
    const int raw = found == snapshot.pictureControls.end()
        ? 0 : found->value;
    return std::clamp(raw / 2 + 50, 0, 100);
}

std::string propertyset(std::string lastChange)
{
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">"
        "<e:property><LastChange>"
        + UpnpXml::escapeText(lastChange)
        + "</LastChange></e:property></e:propertyset>";
}

std::string eventBody(
    std::string_view service,
    const playback::PlaybackSnapshot &snapshot)
{
    if (service == "RenderingControl") {
        std::ostringstream lastChange;
        lastChange
            << "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/RCS/\">"
            << "<InstanceID val=\"0\">"
            << "<Volume channel=\"Master\" val=\""
            << snapshot.volumePercent << "\"/>"
            << "<Mute channel=\"Master\" val=\""
            << (snapshot.muted ? 1 : 0) << "\"/>"
            << "<Brightness val=\""
            << pictureValue(snapshot, "brightness") << "\"/>"
            << "<Contrast val=\""
            << pictureValue(snapshot, "contrast") << "\"/>"
            << "<Sharpness val=\""
            << pictureValue(snapshot, "sharpen") << "\"/>"
            << "</InstanceID></Event>";
        return propertyset(lastChange.str());
    }

    if (service == "ConnectionManager") {
        return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">"
            "<e:property><SourceProtocolInfo></SourceProtocolInfo>"
            "</e:property><e:property><SinkProtocolInfo>"
            + UpnpXml::sinkProtocolInfo()
            + "</SinkProtocolInfo></e:property></e:propertyset>";
    }

    const std::string state = dlnaState(snapshot.state);
    const std::string duration = durationText(snapshot.durationSeconds);
    const bool hasMedia = !snapshot.current.uri.empty();
    std::ostringstream lastChange;
    lastChange
        << "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/AVT/\">"
        << "<InstanceID val=\"0\">"
        << "<TransportState val=\"" << state << "\"/>"
        << "<TransportStatus val=\""
        << (snapshot.loadStatus == playback::LoadStatus::Failed
                ? "ERROR_OCCURRED" : "OK")
        << "\"/>"
        << "<CurrentMediaDuration val=\"" << duration << "\"/>"
        << "<CurrentTrackDuration val=\"" << duration << "\"/>"
        << "<CurrentTrack val=\"" << (hasMedia ? 1 : 0) << "\"/>"
        << "<NumberOfTracks val=\"" << (hasMedia ? 1 : 0) << "\"/>"
        << "<CurrentPlayMode val=\"" << dlnaPlayMode(snapshot.playMode)
        << "\"/>"
        << "<CurrentTransportActions val=\""
        << UpnpXml::transportActionsFor(
               state, snapshot.hasNext, snapshot.hasPrevious)
        << "\"/>"
        << "</InstanceID></Event>";
    return propertyset(lastChange.str());
}

bool socketWritable(
    SOCKET socket, Clock::time_point deadline, std::stop_token stop,
    bool &failedReady)
{
    failedReady = false;
    while (!stop.stop_requested() && Clock::now() < deadline) {
        fd_set writable;
        fd_set failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(socket, &writable);
        FD_SET(socket, &failed);
        auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - Clock::now());
        remaining = std::min(remaining, 50000us);
        timeval timeout{
            static_cast<long>(remaining.count() / 1000000),
            static_cast<long>(remaining.count() % 1000000),
        };
        const int ready = select(
            0, nullptr, &writable, &failed, &timeout);
        if (ready > 0) {
            failedReady = FD_ISSET(socket, &failed) != 0;
            return true;
        }
        if (ready == SOCKET_ERROR
            && WSAGetLastError() != WSAEINTR) {
            return false;
        }
    }
    return false;
}

bool socketReadable(
    SOCKET socket, Clock::time_point deadline, std::stop_token stop)
{
    while (!stop.stop_requested() && Clock::now() < deadline) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket, &readable);
        auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - Clock::now());
        remaining = std::min(remaining, 50000us);
        timeval timeout{
            static_cast<long>(remaining.count() / 1000000),
            static_cast<long>(remaining.count() % 1000000),
        };
        const int ready = select(
            0, &readable, nullptr, nullptr, &timeout);
        if (ready > 0)
            return true;
        if (ready == SOCKET_ERROR
            && WSAGetLastError() != WSAEINTR) {
            return false;
        }
    }
    return false;
}

bool deliverNotification(
    const Endpoint &endpoint, const std::string &request,
    std::chrono::milliseconds timeout, std::stop_token stop)
{
    using platform::net::SocketHandle;
    const auto deadline = Clock::now() + timeout;
    ADDRINFOEXW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    HANDLE resolveEvent =
        CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!resolveEvent)
        return false;
    OVERLAPPED resolveOverlapped{};
    resolveOverlapped.hEvent = resolveEvent;
    HANDLE cancelHandle = nullptr;
    ADDRINFOEXW *addresses = nullptr;
    const std::wstring host(
        endpoint.host.begin(), endpoint.host.end());
    const std::wstring port =
        std::to_wstring(endpoint.port);
    int resolveResult = GetAddrInfoExW(
        host.c_str(), port.c_str(), NS_DNS, nullptr, &hints,
        &addresses, nullptr, &resolveOverlapped,
        nullptr, &cancelHandle);
    if (resolveResult == WSA_IO_PENDING) {
        bool completed = false;
        while (!stop.stop_requested()
               && Clock::now() < deadline) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - Clock::now());
            if (WaitForSingleObject(
                    resolveEvent,
                    static_cast<DWORD>(
                        std::clamp<std::int64_t>(
                            remaining.count(), 1, 100)))
                == WAIT_OBJECT_0) {
                completed = true;
                break;
            }
        }
        if (!completed) {
            if (cancelHandle)
                static_cast<void>(GetAddrInfoExCancel(&cancelHandle));
            static_cast<void>(WaitForSingleObject(resolveEvent, 2000));
            CloseHandle(resolveEvent);
            return false;
        }
        resolveResult =
            GetAddrInfoExOverlappedResult(&resolveOverlapped);
    }
    CloseHandle(resolveEvent);
    if (resolveResult != 0)
        return false;
    struct AddressGuard {
        ADDRINFOEXW *value;
        ~AddressGuard() { FreeAddrInfoExW(value); }
    } addressGuard{addresses};

    for (ADDRINFOEXW *address = addresses;
         address && !stop.stop_requested();
         address = address->ai_next) {
        SOCKET raw = ::socket(
            address->ai_family, address->ai_socktype,
            address->ai_protocol);
        if (raw == INVALID_SOCKET)
            continue;
        SocketHandle socket(raw);
        std::string error;
        if (!socket.setNonBlocking(error))
            continue;

        platform::net::SocketEventLoop connectLoop;
        if (!connectLoop.initialize(error)
            || !connectLoop.add(socket.get(), FD_CONNECT, error)) {
            continue;
        }
        const int connected = connect(
            socket.get(), address->ai_addr,
            static_cast<int>(address->ai_addrlen));
        if (connected == SOCKET_ERROR) {
            const int code = WSAGetLastError();
            if (code != WSAEWOULDBLOCK && code != WSAEINPROGRESS) {
                connectLoop.remove(socket.get());
                continue;
            }
            bool connectedEvent = false;
            while (!stop.stop_requested() && Clock::now() < deadline) {
                std::vector<platform::net::SocketActivation> activations;
                bool woken = false;
                const auto remaining =
                    std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                        deadline - Clock::now());
                if (!connectLoop.poll(
                        std::clamp(remaining, 1ms, 100ms),
                        activations, woken, error)) {
                    break;
                }
                if (!activations.empty()) {
                    connectedEvent = true;
                    break;
                }
            }
            if (!connectedEvent) {
                connectLoop.remove(socket.get());
                continue;
            }
            int pendingError = 0;
            int errorSize = sizeof(pendingError);
            if (getsockopt(
                    socket.get(), SOL_SOCKET, SO_ERROR,
                    reinterpret_cast<char *>(&pendingError),
                    &errorSize) == SOCKET_ERROR
                || pendingError != 0) {
                connectLoop.remove(socket.get());
                continue;
            }
        }
        connectLoop.remove(socket.get());

        std::size_t offset = 0;
        while (offset < request.size()) {
            if (stop.stop_requested() || Clock::now() >= deadline)
                return false;
            const int sent = send(
                socket.get(), request.data() + offset,
                static_cast<int>(request.size() - offset), 0);
            if (sent > 0) {
                offset += static_cast<std::size_t>(sent);
                continue;
            }
            if (sent == SOCKET_ERROR
                && WSAGetLastError() == WSAEWOULDBLOCK) {
                bool failedReady = false;
                if (!socketWritable(
                        socket.get(), deadline, stop, failedReady)
                    || failedReady) {
                    return false;
                }
                continue;
            }
            return false;
        }

        if (socketReadable(socket.get(), deadline, stop)) {
            char response[512]{};
            const int received = recv(
                socket.get(), response, sizeof(response) - 1, 0);
            if (received <= 0)
                return false;
            const std::string_view status(response, received);
            if (!status.starts_with("HTTP/1.1 200")
                && !status.starts_with("HTTP/1.0 200")) {
                return false;
            }
        }
        return !stop.stop_requested();
    }
    return false;
}

std::chrono::milliseconds retryDelay(int failures)
{
    const int exponent = std::clamp(failures - 1, 0, 4);
    return std::chrono::milliseconds{
        250 * (1 << exponent)};
}

} // namespace

GenaManager::GenaManager(
    GenaManagerCallbacks callbacks, GenaManagerOptions options)
    : m_callbacks(std::move(callbacks))
    , m_options(std::move(options))
{
    m_options.maxSubscriptions =
        std::clamp<std::size_t>(m_options.maxSubscriptions, 1, 256);
    m_options.workerCount =
        std::clamp<std::size_t>(m_options.workerCount, 1, 16);
    m_options.subscriptionTimeout =
        std::max(m_options.subscriptionTimeout, 1000ms);
    m_options.notifyTimeout =
        std::max(m_options.notifyTimeout, 100ms);
    m_avTransportBody = eventBody("AVTransport", m_state);
    m_renderingBody = eventBody("RenderingControl", m_state);
    m_connectionBody = eventBody("ConnectionManager", m_state);
}

GenaManager::~GenaManager()
{
    stop();
}

bool GenaManager::start(std::string &error)
{
    std::lock_guard lock(m_mutex);
    if (m_running)
        return true;
    if (!m_winsock.initialize(error))
        return false;
    m_stopping = false;
    m_running = true;
    m_maintenanceThread = std::jthread(
        [this](std::stop_token stop) { maintenanceLoop(stop); });
    for (std::size_t index = 0; index < m_options.workerCount; ++index) {
        m_workers.emplace_back(
            [this](std::stop_token stop) { workerLoop(stop); });
    }
    error.clear();
    return true;
}

void GenaManager::stop() noexcept
{
    std::optional<CountNotice> countNotice;
    {
        std::scoped_lock callbackOrder(m_callbackOrderMutex);
        {
            std::lock_guard lock(m_mutex);
            if (!m_running)
                return;
            m_stopping = true;
            m_running = false;
            if (!m_subscriptions.empty()) {
                countNotice = recountLocked(
                    SubscriptionChangeReason::Count);
                m_subscriptions.clear();
            }
            m_readySubscriptions.clear();
        }
    }
    m_ready.notify_all();
    m_maintenanceReady.notify_all();
    if (m_maintenanceThread.joinable())
        m_maintenanceThread.request_stop();
    for (auto &worker : m_workers)
        worker.request_stop();
    m_ready.notify_all();
    if (m_maintenanceThread.joinable())
        m_maintenanceThread.join();
    for (auto &worker : m_workers) {
        if (worker.joinable())
            worker.join();
    }
    m_workers.clear();
    if (countNotice) {
        std::scoped_lock callbackOrder(m_callbackOrderMutex);
        publishCount(*countNotice);
    }
}

net::HttpResponse GenaManager::handleSubscribe(
    const core::HttpRequest &request)
{
    const std::string service = serviceForTarget(request.target);
    if (service.empty())
        return makeResponse(404, "Not Found");

    std::scoped_lock callbackOrder(m_callbackOrderMutex);
    std::optional<CountNotice> countNotice;
    std::string sid;
    bool activity = false;
    {
        std::unique_lock lock(m_mutex);
        if (!m_running)
            return makeResponse(503, "Service Unavailable");
        const auto now = Clock::now();
        const std::string existing =
            request.header("sid").value_or("");
        if (!existing.empty()) {
            auto found = m_subscriptions.find(existing);
            if (found == m_subscriptions.end()
                || now >= found->second.expiresAt) {
                if (found != m_subscriptions.end()) {
                    m_subscriptions.erase(found);
                    countNotice = recountLocked(
                        SubscriptionChangeReason::Expired);
                }
                lock.unlock();
                if (countNotice)
                    publishCount(*countNotice);
                return makeResponse(412, "Precondition Failed");
            }
            found->second.expiresAt =
                now + m_options.subscriptionTimeout;
            activity = true;
            sid = existing;
        } else {
            if (m_subscriptions.size()
                >= m_options.maxSubscriptions) {
                return makeResponse(503, "Service Unavailable");
            }
            const auto callbackHeader =
                request.header("callback").value_or("");
            const auto callback = parseCallback(callbackHeader);
            if (!callback
                || lower(request.header("nt").value_or(""))
                    != "upnp:event") {
                return makeResponse(412, "Precondition Failed");
            }

            std::random_device random;
            std::ostringstream generated;
            generated << "uuid:"
                << std::hex << std::setfill('0');
            for (int index = 0; index < 4; ++index)
                generated << std::setw(8) << random();
            sid = generated.str();
            while (m_subscriptions.contains(sid)) {
                generated << std::hex << random();
                sid = generated.str();
            }

            Subscription subscription;
            subscription.service = service;
            subscription.callback = callbackHeader;
            subscription.expiresAt =
                now + m_options.subscriptionTimeout;
            subscription.retryAt =
                now + m_options.initialEventDelay;
            m_subscriptions.emplace(sid, std::move(subscription));
            countNotice = recountLocked(
                SubscriptionChangeReason::Added);
            activity = true;
        }
    }
    m_maintenanceReady.notify_one();
    if (countNotice)
        publishCount(*countNotice);
    if (activity)
        publishActivity();
    log("GENA subscription " + service + " " + sid);
    return makeResponse(
        200, {},
        {{"sid", sid}, {"timeout", "Second-1800"}});
}

net::HttpResponse GenaManager::handleUnsubscribe(
    const core::HttpRequest &request)
{
    std::optional<CountNotice> countNotice;
    std::string sid;
    std::scoped_lock callbackOrder(m_callbackOrderMutex);
    {
        std::unique_lock lock(m_mutex);
        sid = request.header("sid").value_or("");
        const auto found = m_subscriptions.find(sid);
        if (!m_running || found == m_subscriptions.end()
            || Clock::now() >= found->second.expiresAt) {
            const bool expired =
                found != m_subscriptions.end()
                && Clock::now() >= found->second.expiresAt;
            if (found != m_subscriptions.end()) {
                m_subscriptions.erase(found);
            }
            if (expired) {
                countNotice = recountLocked(
                    SubscriptionChangeReason::Expired);
                lock.unlock();
                if (countNotice)
                    publishCount(*countNotice);
            }
            return makeResponse(412, "Precondition Failed");
        }
        m_subscriptions.erase(found);
        countNotice = recountLocked(
            SubscriptionChangeReason::ExplicitRemoval);
    }
    if (countNotice)
        publishCount(*countNotice);
    log("GENA unsubscribe " + sid);
    return makeResponse(200, {});
}

void GenaManager::updateState(
    const playback::PlaybackSnapshot &snapshot)
{
    const std::string av = eventBody("AVTransport", snapshot);
    const std::string rendering =
        eventBody("RenderingControl", snapshot);
    const std::string connection =
        eventBody("ConnectionManager", snapshot);
    bool queued = false;
    {
        std::lock_guard lock(m_mutex);
        m_state = snapshot;
        if (av != m_avTransportBody) {
            m_avTransportBody = av;
            markServiceDirtyLocked("AVTransport");
            queued = true;
        }
        if (rendering != m_renderingBody) {
            m_renderingBody = rendering;
            markServiceDirtyLocked("RenderingControl");
            queued = true;
        }
        if (connection != m_connectionBody) {
            m_connectionBody = connection;
            markServiceDirtyLocked("ConnectionManager");
            queued = true;
        }
    }
    if (queued)
        m_ready.notify_all();
}

std::size_t GenaManager::subscriptionCount() const
{
    std::lock_guard lock(m_mutex);
    return m_subscriptions.size();
}

void GenaManager::scheduleLocked(
    const std::string &sid, Clock::time_point now)
{
    const auto found = m_subscriptions.find(sid);
    if (found == m_subscriptions.end()
        || found->second.busy || found->second.queued
        || !found->second.dirty
        || now < found->second.retryAt) {
        return;
    }
    found->second.queued = true;
    m_readySubscriptions.push_back(sid);
    m_ready.notify_one();
}

void GenaManager::markServiceDirtyLocked(const std::string &service)
{
    const auto now = Clock::now();
    for (auto &[sid, subscription] : m_subscriptions) {
        if (subscription.service != service)
            continue;
        subscription.dirty = true;
        scheduleLocked(sid, now);
    }
}

GenaManager::CountNotice GenaManager::recountLocked(
    SubscriptionChangeReason reason)
{
    const std::size_t count = m_subscriptions.size();
    if (count != m_lastReportedCount) {
        m_lastReportedCount = count;
        return {count, reason};
    }
    return {count, reason};
}

void GenaManager::publishCount(const CountNotice &notice)
{
    if (!m_callbacks.subscriptionCountChanged)
        return;
    try {
        m_callbacks.subscriptionCountChanged(
            notice.count, notice.reason);
    } catch (...) {
    }
}

void GenaManager::publishActivity()
{
    if (!m_callbacks.subscriptionActivity)
        return;
    try {
        m_callbacks.subscriptionActivity();
    } catch (...) {
    }
}

void GenaManager::log(const std::string &message) const
{
    if (!m_callbacks.log)
        return;
    try {
        m_callbacks.log(message);
    } catch (...) {
    }
}

std::string GenaManager::bodyForLocked(
    const std::string &service) const
{
    if (service == "RenderingControl")
        return m_renderingBody;
    if (service == "ConnectionManager")
        return m_connectionBody;
    return m_avTransportBody;
}

void GenaManager::maintenanceLoop(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        {
            std::unique_lock lock(m_mutex);
            m_maintenanceReady.wait_for(lock, 100ms);
            if (stop.stop_requested() || m_stopping)
                return;
        }

        std::optional<CountNotice> expired;
        std::scoped_lock callbackOrder(m_callbackOrderMutex);
        {
            std::lock_guard lock(m_mutex);
            const auto now = Clock::now();
            bool removed = false;
            for (auto it = m_subscriptions.begin();
                 it != m_subscriptions.end();) {
                if (now >= it->second.expiresAt) {
                    it = m_subscriptions.erase(it);
                    removed = true;
                } else {
                    scheduleLocked(it->first, now);
                    ++it;
                }
            }
            if (removed) {
                expired = recountLocked(
                    SubscriptionChangeReason::Expired);
            }
        }
        if (expired)
            publishCount(*expired);
    }
}

void GenaManager::workerLoop(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        std::string sid;
        {
            std::unique_lock lock(m_mutex);
            m_ready.wait(lock, [&] {
                return stop.stop_requested() || m_stopping
                    || !m_readySubscriptions.empty();
            });
            if (stop.stop_requested() || m_stopping)
                return;
            sid = std::move(m_readySubscriptions.front());
            m_readySubscriptions.pop_front();
            const auto found = m_subscriptions.find(sid);
            if (found == m_subscriptions.end())
                continue;
            found->second.queued = false;
            if (found->second.busy) {
                // Another worker owns the subscription. It will requeue the
                // latest state when its current NOTIFY finishes.
                continue;
            }
            found->second.busy = true;
            found->second.dirty = false;
            m_maintenanceReady.notify_one();
        }
        serviceLoop(std::move(sid), stop);
    }
}

void GenaManager::serviceLoop(
    std::string sid, std::stop_token stop)
{
    std::string callback;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_subscriptions.find(sid);
        if (found == m_subscriptions.end())
            return;
        callback = found->second.callback;
    }
    const auto parsed = parseCallback(callback);
    if (!parsed) {
        std::optional<CountNotice> unreachable;
        std::scoped_lock callbackOrder(m_callbackOrderMutex);
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_subscriptions.find(sid);
            if (found != m_subscriptions.end()) {
                m_subscriptions.erase(found);
                unreachable = recountLocked(
                    SubscriptionChangeReason::Unreachable);
            }
        }
        if (unreachable)
            publishCount(*unreachable);
        return;
    }
    const Endpoint endpoint = *parsed;

    std::string service;
    std::string body;
    std::uint32_t sequence = 0;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_subscriptions.find(sid);
        if (found == m_subscriptions.end())
            return;
        service = found->second.service;
        sequence = found->second.nextSequence;
        body = bodyForLocked(service);
    }

    std::ostringstream request;
    request << "NOTIFY " << endpoint.target << " HTTP/1.1\r\n"
            << "HOST: " << endpoint.hostHeader << "\r\n"
            << "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
            << "NT: upnp:event\r\n"
            << "NTS: upnp:propchange\r\n"
            << "SID: " << sid << "\r\n"
            << "SEQ: " << sequence << "\r\n"
            << "CONTENT-LENGTH: " << body.size() << "\r\n"
            << "SERVER: " << kServerHeader << "\r\n"
            << "TIMEOUT: Second-1800\r\n"
            << "Connection: close\r\n\r\n"
            << body;

    const bool delivered = deliverNotification(
        endpoint, request.str(), m_options.notifyTimeout, stop);
    log(
        std::string("GENA NOTIFY ") + service + " "
        + endpoint.hostHeader + " SEQ=" + std::to_string(sequence)
        + (delivered ? " connected" : " connect failed"));

    std::optional<CountNotice> unreachable;
    std::scoped_lock callbackOrder(m_callbackOrderMutex);
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_subscriptions.find(sid);
        if (found == m_subscriptions.end())
            return;
        found->second.busy = false;
        const auto now = Clock::now();
        if (delivered) {
            found->second.failures = 0;
            ++found->second.nextSequence;
        } else if (!stop.stop_requested()) {
            ++found->second.failures;
            found->second.dirty = true;
            if (found->second.failures >= 5) {
                m_subscriptions.erase(found);
                unreachable = recountLocked(
                    SubscriptionChangeReason::Unreachable);
            } else {
                found->second.retryAt =
                    now + retryDelay(found->second.failures);
            }
        }
        if (m_subscriptions.contains(sid))
            scheduleLocked(sid, now);
        m_maintenanceReady.notify_one();
    }
    if (unreachable)
        publishCount(*unreachable);
}

} // namespace mcast::dlna::runtime
