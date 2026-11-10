// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <queue>
#include <string>
#include <unordered_set>
#include <vector>

namespace mcast::platform::net {

class WinsockRuntime
{
public:
    WinsockRuntime() = default;
    ~WinsockRuntime();

    WinsockRuntime(const WinsockRuntime &) = delete;
    WinsockRuntime &operator=(const WinsockRuntime &) = delete;

    bool initialize(std::string &error);
    bool active() const noexcept { return m_active; }

private:
    bool m_active = false;
};

class SocketHandle
{
public:
    SocketHandle() = default;
    explicit SocketHandle(SOCKET socket);
    ~SocketHandle();

    SocketHandle(const SocketHandle &) = delete;
    SocketHandle &operator=(const SocketHandle &) = delete;
    SocketHandle(SocketHandle &&other) noexcept;
    SocketHandle &operator=(SocketHandle &&other) noexcept;

    bool valid() const noexcept { return m_socket != INVALID_SOCKET; }
    SOCKET get() const noexcept { return m_socket; }
    SOCKET release() noexcept;
    void close() noexcept;

    bool createUdp(std::string &error);
    bool createTcp(std::string &error);
    bool setNonBlocking(std::string &error);
    bool bindLoopback(std::uint16_t port, std::string &error);
    std::optional<std::uint16_t> localPort() const;

private:
    SOCKET m_socket = INVALID_SOCKET;
};

struct SocketActivation {
    SOCKET socket = INVALID_SOCKET;
    long events = 0;
    std::array<int, FD_MAX_EVENTS> errorCodes{};
};

class SocketEventLoop
{
public:
    SocketEventLoop() = default;
    ~SocketEventLoop();

    SocketEventLoop(const SocketEventLoop &) = delete;
    SocketEventLoop &operator=(const SocketEventLoop &) = delete;

    bool initialize(std::string &error);
    bool add(SOCKET socket, long events, std::string &error);
    void remove(SOCKET socket);
    bool poll(
        std::chrono::milliseconds timeout,
        std::vector<SocketActivation> &activations,
        bool &woken, std::string &error);
    void wake();

private:
    struct Registration {
        WSAEVENT event = WSA_INVALID_EVENT;
    };

    WSAEVENT m_wakeEvent = WSA_INVALID_EVENT;
    std::map<SOCKET, Registration> m_registrations;
};

class TimerQueue
{
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    using TimerId = std::uint64_t;

    TimerId schedule(TimePoint deadline, TimerId id = 0);
    bool cancel(TimerId id);
    std::vector<TimerId> takeDue(TimePoint now);
    std::optional<TimePoint> nextDeadline() const;
    bool empty() const noexcept { return m_active.empty(); }

private:
    struct Timer {
        TimePoint deadline;
        TimerId id = 0;
        std::uint64_t sequence = 0;
    };
    struct Later {
        bool operator()(const Timer &left, const Timer &right) const
        {
            if (left.deadline != right.deadline)
                return left.deadline > right.deadline;
            return left.sequence > right.sequence;
        }
    };

    std::priority_queue<Timer, std::vector<Timer>, Later> m_timers;
    std::unordered_set<TimerId> m_cancelled;
    std::unordered_set<TimerId> m_active;
    std::unordered_set<TimerId> m_used;
    std::uint64_t m_nextId = 1;
    std::uint64_t m_nextSequence = 1;
};

struct Ipv4Interface {
    std::string name;
    std::string description;
    std::string address;
    unsigned long index = 0;
    unsigned long ifType = 0;
    unsigned long adapterFlags = 0;
    unsigned long ipv4Metric = 0;
    unsigned long prefixLength = 0;
    bool up = false;
    bool hardwareInterface = true;
    bool filterInterface = false;
    bool isLoopback = false;
    bool isLinkLocal = false;
    bool canMulticast = false;
};

std::vector<Ipv4Interface> enumerateIpv4Interfaces(std::string &error);

} // namespace mcast::platform::net
