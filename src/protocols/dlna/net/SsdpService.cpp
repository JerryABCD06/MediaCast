// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/net/SsdpService.h"

#include "protocols/dlna/core/SsdpMessages.h"
#include "protocols/dlna/net/SsdpInterfaceSelector.h"
#include "protocols/dlna/net/SsdpSocket.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <utility>
#include <vector>

namespace mcast::dlna::net {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::uint32_t kMaxAgeSeconds = 1800;
constexpr std::size_t kMaxPendingReplies = 64;
constexpr std::string_view kServerHeader =
    "Windows/10.0 UPnP/1.1 MCast/0.1";

const std::array<std::string_view, 6> kNotificationTypes{
    "upnp:rootdevice",
    "",
    "urn:schemas-upnp-org:device:MediaRenderer:1",
    "urn:schemas-upnp-org:service:AVTransport:1",
    "urn:schemas-upnp-org:service:RenderingControl:1",
    "urn:schemas-upnp-org:service:ConnectionManager:1",
};

std::string dateHeader()
{
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    if (gmtime_s(&utc, &now) != 0)
        return "Thu, 01 Jan 1970 00:00:00 GMT";
    std::ostringstream output;
    output << std::put_time(&utc, "%a, %d %b %Y %H:%M:%S GMT");
    return output.str();
}

std::vector<std::string> notificationTypes(std::string_view udn)
{
    std::vector<std::string> result;
    result.reserve(kNotificationTypes.size());
    for (std::string_view type : kNotificationTypes) {
        result.emplace_back(
            type.empty() ? udn : type);
    }
    return result;
}

std::vector<std::string> replyTypes(
    std::string_view target, std::string_view udn)
{
    const auto all = notificationTypes(udn);
    const auto lower = [](std::string_view value) {
        std::string result(value);
        std::transform(
            result.begin(), result.end(), result.begin(),
            [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
        return result;
    };
    if (lower(target) == "ssdp:all")
        return all;
    std::vector<std::string> result;
    for (const auto &type : all) {
        if (lower(type) == lower(target)) {
            result.push_back(type);
            break;
        }
    }
    return result;
}

struct PendingReply {
    Clock::time_point deadline;
    std::string address;
    std::uint16_t port = 0;
    std::vector<std::string> messages;
};

} // namespace

SsdpService::~SsdpService()
{
    stop();
}

bool SsdpService::start(
    SsdpServiceOptions options, Log log, std::string &error)
{
    std::lock_guard lifecycle(m_lifecycleMutex);
    if (m_running.load()) {
        error.clear();
        return true;
    }
    if (m_thread.joinable()) {
        m_thread.request_stop();
        m_thread.join();
    }
    if (options.udn.empty() || options.httpPort == 0) {
        error = "SSDP requires a UDN and HTTP port";
        return false;
    }
    if (options.aliveInterval.count() <= 0)
        options.aliveInterval = 10s;
    m_byeByeInProgress = false;
    m_sendAliveRequested = false;
    m_sendByeByeRequested = false;

    {
        std::lock_guard lock(m_startMutex);
        m_startComplete = false;
        m_startSuccess = false;
        m_startError.clear();
    }
    m_log = std::move(log);
    m_thread = std::jthread(
        [this, value = std::move(options)](std::stop_token stop) {
            run(value, stop);
        });

    std::unique_lock lock(m_startMutex);
    if (!m_startChanged.wait_for(lock, 1500ms, [this] {
            return m_startComplete;
        })) {
        m_thread.request_stop();
        lock.unlock();
        m_thread.join();
        error = "SSDP start timed out";
        return false;
    }
    if (!m_startSuccess) {
        error = m_startError.empty()
            ? "SSDP failed to start" : m_startError;
        lock.unlock();
        m_thread.join();
        return false;
    }
    error.clear();
    return true;
}

void SsdpService::stop() noexcept
{
    std::lock_guard lock(m_lifecycleMutex);
    if (!m_thread.joinable())
        return;
    m_thread.request_stop();
    m_thread.join();
}

void SsdpService::announceByeBye(
    std::chrono::milliseconds interval, int rounds) noexcept
{
    if (!m_running.load() || rounds <= 0)
        return;
    interval = std::clamp(interval, 20ms, 500ms);
    rounds = std::clamp(rounds, 1, 3);
    m_byeByeRoundsRequested = rounds;
    m_byeByeIntervalMs = interval.count();
    m_byeByeInProgress = true;
    m_sendAliveRequested = false;
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(m_byeByeMutex);
        generation = ++m_byeByeGeneration;
        m_byeByeCompleted = generation - 1;
    }
    m_sendByeByeRequested = true;
    std::unique_lock lock(m_byeByeMutex);
    m_byeByeChanged.wait_for(
        lock, 1500ms, [&] {
            return m_byeByeCompleted >= generation;
        });
}

std::string SsdpService::interfaceAddress() const
{
    std::lock_guard lock(m_startMutex);
    return m_interfaceAddress;
}

std::string SsdpService::locationUrl() const
{
    std::lock_guard lock(m_startMutex);
    return m_locationUrl;
}

void SsdpService::setBroadcasting(bool enabled) noexcept
{
    const bool previous = m_broadcasting.exchange(enabled);
    if (!previous && enabled)
        m_sendAliveRequested = true;
}

void SsdpService::setAliveInterval(
    std::chrono::milliseconds interval) noexcept
{
    if (interval.count() <= 0)
        return;
    m_aliveIntervalMs = interval.count();
    if (m_broadcasting)
        m_sendAliveRequested = true;
}

void SsdpService::completeStart(bool success, std::string error)
{
    {
        std::lock_guard lock(m_startMutex);
        m_startSuccess = success;
        m_startError = success ? std::string{} : std::move(error);
        m_startComplete = true;
    }
    m_startChanged.notify_all();
}

void SsdpService::log(const std::string &message) const
{
    if (!m_log)
        return;
    try {
        m_log(message);
    } catch (...) {
    }
}

void SsdpService::run(
    SsdpServiceOptions options, std::stop_token stop)
{
    std::string address = std::move(options.interfaceAddress);
    if (address.empty()) {
        std::string error;
        const auto interfaces =
            platform::net::enumerateIpv4Interfaces(error);
        if (!error.empty()) {
            completeStart(false, error);
            return;
        }
        const auto selection = selectSsdpInterface(interfaces);
        for (const auto &rejected : selection.rejected) {
            log(
                "SSDP interface skipped: "
                + rejected.interface.name + " ("
                + rejected.interface.address + "): "
                + rejected.reason);
        }
        if (!selection.selected) {
            completeStart(false, "no usable SSDP multicast interface");
            return;
        }
        address = selection.selected->interface.address;
        log(
            "SSDP interface: " + selection.selected->interface.name
            + " (" + address + ")");
    }

    {
        std::lock_guard lock(m_startMutex);
        m_interfaceAddress = address;
        m_locationUrl =
            "http://" + address + ":"
            + std::to_string(options.httpPort)
            + "/description.xml";
    }

    SsdpSocket socket;
    std::string error;
    if (!socket.open(options.ssdpPort, error)) {
        completeStart(false, error);
        return;
    }
    m_localPort = socket.localPort();

    bool multicastJoined = false;
    if (options.joinMulticast) {
        if (!socket.setMulticastInterface(address, error)
            || !socket.joinMulticastGroup(
                SsdpSocket::MulticastGroup, address, error)) {
            socket.close();
            completeStart(false, error);
            return;
        }
        multicastJoined = true;
    }

    platform::net::SocketEventLoop loop;
    if (!loop.initialize(error)
        || !loop.add(socket.handle(), FD_READ, error)) {
        if (multicastJoined)
            static_cast<void>(socket.leaveMulticastGroup(
                SsdpSocket::MulticastGroup, address, error));
        socket.close();
        completeStart(false, error);
        return;
    }

    const std::uint32_t bootId = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    const std::uint16_t notifyPort =
        options.ssdpPort == 0
        ? SsdpSocket::DefaultPort : options.ssdpPort;
    m_running = true;
    log(
        "SSDP listening on port " + std::to_string(m_localPort.load())
        + ", location " + locationUrl());

    const auto sendNotify = [&](bool alive) {
        for (const auto &type : notificationTypes(options.udn)) {
            const std::string message = core::makeSsdpNotify(
                alive, type, options.udn, locationUrl(),
                kMaxAgeSeconds, kServerHeader, bootId, error);
            if (message.empty()) {
                log("SSDP notify build failed: " + error);
                continue;
            }
            const std::span<const std::uint8_t> bytes(
                reinterpret_cast<const std::uint8_t *>(message.data()),
                message.size());
            const auto sent = socket.sendDatagram(
                bytes, SsdpSocket::MulticastGroup, notifyPort, error);
            if (sent != SsdpSendStatus::Sent)
                log("SSDP notify failed: " + error);
        }
    };

    m_broadcasting = options.advertise;
    m_aliveIntervalMs =
        options.aliveInterval.count() > 0
        ? options.aliveInterval.count() : 10000;
    m_sendAliveRequested = false;

    // 关闭“定期广播”不等于拒绝被发现：和旧实现一样，启动时仍主动
    // alive 一次，之后只停止周期广播，M-SEARCH 始终继续响应。
    if (options.joinMulticast)
        sendNotify(true);
    auto nextAlive = Clock::now() + 300ms;
    int byeByeRoundsRemaining = 0;
    auto nextByeBye = Clock::now();
    bool explicitByeByeCompleted = false;
    std::vector<PendingReply> pendingReplies;
    std::mt19937 random(std::random_device{}());
    std::uniform_int_distribution<int> responseDelay(0, 200);

    completeStart(true, {});
    while (!stop.stop_requested()) {
        const auto now = Clock::now();
        if (m_sendByeByeRequested.exchange(false)) {
            byeByeRoundsRemaining = m_byeByeRoundsRequested.load();
            nextByeBye = now;
            m_byeByeInProgress = true;
            m_sendAliveRequested = false;
        }
        if (byeByeRoundsRemaining > 0 && now >= nextByeBye) {
            sendNotify(false);
            --byeByeRoundsRemaining;
            nextByeBye = now
                + std::chrono::milliseconds(
                    m_byeByeIntervalMs.load());
            if (byeByeRoundsRemaining == 0) {
                explicitByeByeCompleted = true;
                {
                    std::lock_guard lock(m_byeByeMutex);
                    m_byeByeCompleted = m_byeByeGeneration;
                }
                m_byeByeChanged.notify_all();
            }
        }
        auto reply = pendingReplies.begin();
        while (reply != pendingReplies.end()) {
            if (reply->deadline > now) {
                ++reply;
                continue;
            }
            for (const auto &message : reply->messages) {
                const std::span<const std::uint8_t> bytes(
                    reinterpret_cast<const std::uint8_t *>(
                        message.data()),
                    message.size());
                const auto sent = socket.sendDatagram(
                    bytes, reply->address, reply->port, error);
                if (sent != SsdpSendStatus::Sent) {
                    log("SSDP search response failed: " + error);
                    break;
                }
            }
            reply = pendingReplies.erase(reply);
        }

        if (!m_byeByeInProgress
            && m_sendAliveRequested.exchange(false)) {
            sendNotify(true);
            nextAlive = now
                + std::chrono::milliseconds(
                    m_aliveIntervalMs.load());
        }
        if (!m_byeByeInProgress
            && m_broadcasting && now >= nextAlive) {
            sendNotify(true);
            nextAlive = now
                + std::chrono::milliseconds(
                    m_aliveIntervalMs.load());
        }

        auto timeout = 100ms;
        if (byeByeRoundsRemaining > 0) {
            timeout = std::min(
                timeout,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    nextByeBye - now));
        }
        if (m_broadcasting && !m_byeByeInProgress) {
            timeout = std::min(
                timeout,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    nextAlive - now));
        }
        for (const auto &pending : pendingReplies) {
            timeout = std::min(
                timeout,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    pending.deadline - now));
        }
        timeout = std::max(timeout, 1ms);

        std::vector<platform::net::SocketActivation> activations;
        bool woken = false;
        if (!loop.poll(timeout, activations, woken, error)) {
            log("SSDP poll failed: " + error);
            break;
        }
        if (activations.empty())
            continue;

        while (true) {
            SsdpDatagram datagram;
            const auto received =
                socket.receiveDatagram(datagram, error);
            if (received == SsdpReceiveStatus::WouldBlock)
                break;
            if (received == SsdpReceiveStatus::Reset
                || received == SsdpReceiveStatus::Oversize) {
                continue;
            }
            if (received == SsdpReceiveStatus::Error) {
                log("SSDP receive failed: " + error);
                break;
            }

            core::SsdpRequest request;
            std::string parseError;
            if (!core::parseSsdpRequest(
                    datagram.bytes, request, parseError)) {
                continue;
            }
            const auto target = request.header("st").value_or("");
            const auto types = replyTypes(target, options.udn);
            if (types.empty()) {
                log("SSDP search ignored: " + target);
                continue;
            }

            std::vector<std::string> messages;
            for (const auto &type : types) {
                const std::string response =
                    core::makeSsdpSearchResponse(
                        type, options.udn, locationUrl(),
                        kMaxAgeSeconds, kServerHeader,
                        dateHeader(), bootId, error);
                if (response.empty()) {
                    log("SSDP search response build failed: " + error);
                    continue;
                }
                messages.push_back(response);
            }
            if (!messages.empty()) {
                if (pendingReplies.size() >= kMaxPendingReplies) {
                    log("SSDP search reply queue is full");
                    continue;
                }
                pendingReplies.push_back(PendingReply{
                    now + std::chrono::milliseconds(responseDelay(random)),
                    datagram.sourceAddress,
                    datagram.sourcePort,
                    std::move(messages),
                });
            }
        }
    }

    if (multicastJoined && !explicitByeByeCompleted)
        sendNotify(false);
    loop.remove(socket.handle());
    if (multicastJoined) {
        static_cast<void>(socket.leaveMulticastGroup(
            SsdpSocket::MulticastGroup, address, error));
    }
    socket.close();
    m_localPort = 0;
    m_running = false;
    log("SSDP stopped");
}

} // namespace mcast::dlna::net
