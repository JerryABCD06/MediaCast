// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "platform/windows/net/SocketRuntime.h"

#include <iphlpapi.h>

#include <algorithm>
#include <limits>
#include <string_view>

namespace mcast::platform::net {
namespace {

std::string socketError(std::string_view operation)
{
    return std::string(operation) + " failed with WSA error "
        + std::to_string(WSAGetLastError());
}

} // namespace

WinsockRuntime::~WinsockRuntime()
{
    if (m_active)
        WSACleanup();
}

bool WinsockRuntime::initialize(std::string &error)
{
    if (m_active)
        return true;
    WSADATA data{};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
        error = "WSAStartup failed with error " + std::to_string(result);
        return false;
    }
    if (LOBYTE(data.wVersion) != 2 || HIBYTE(data.wVersion) != 2) {
        WSACleanup();
        error = "Winsock 2.2 is unavailable";
        return false;
    }
    m_active = true;
    return true;
}

SocketHandle::SocketHandle(SOCKET socket)
    : m_socket(socket)
{
}

SocketHandle::~SocketHandle()
{
    close();
}

SocketHandle::SocketHandle(SocketHandle &&other) noexcept
    : m_socket(other.release())
{
}

SocketHandle &SocketHandle::operator=(SocketHandle &&other) noexcept
{
    if (this != &other) {
        close();
        m_socket = other.release();
    }
    return *this;
}

SOCKET SocketHandle::release() noexcept
{
    const SOCKET socket = m_socket;
    m_socket = INVALID_SOCKET;
    return socket;
}

void SocketHandle::close() noexcept
{
    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
}

bool SocketHandle::createUdp(std::string &error)
{
    close();
    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET) {
        error = socketError("socket(UDP)");
        return false;
    }
    return true;
}

bool SocketHandle::createTcp(std::string &error)
{
    close();
    m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_socket == INVALID_SOCKET) {
        error = socketError("socket(TCP)");
        return false;
    }
    return true;
}

bool SocketHandle::setNonBlocking(std::string &error)
{
    u_long enabled = 1;
    if (!valid()
        || ioctlsocket(m_socket, FIONBIO, &enabled) == SOCKET_ERROR) {
        error = socketError("ioctlsocket(FIONBIO)");
        return false;
    }
    return true;
}

bool SocketHandle::bindLoopback(std::uint16_t port, std::string &error)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (!valid()
        || bind(
            m_socket, reinterpret_cast<const sockaddr *>(&address),
            sizeof(address)) == SOCKET_ERROR) {
        error = socketError("bind(loopback)");
        return false;
    }
    return true;
}

std::optional<std::uint16_t> SocketHandle::localPort() const
{
    if (!valid())
        return std::nullopt;
    sockaddr_in address{};
    int size = sizeof(address);
    if (getsockname(
            m_socket, reinterpret_cast<sockaddr *>(&address),
            &size) == SOCKET_ERROR)
        return std::nullopt;
    return ntohs(address.sin_port);
}

SocketEventLoop::~SocketEventLoop()
{
    const auto sockets = m_registrations;
    for (const auto &[socket, registration] : sockets) {
        WSAEventSelect(socket, WSA_INVALID_EVENT, 0);
        if (registration.event != WSA_INVALID_EVENT)
            WSACloseEvent(registration.event);
    }
    m_registrations.clear();
    if (m_wakeEvent != WSA_INVALID_EVENT)
        WSACloseEvent(m_wakeEvent);
}

bool SocketEventLoop::initialize(std::string &error)
{
    if (m_wakeEvent != WSA_INVALID_EVENT)
        return true;
    m_wakeEvent = WSACreateEvent();
    if (m_wakeEvent == WSA_INVALID_EVENT) {
        error = socketError("WSACreateEvent(wake)");
        return false;
    }
    return true;
}

bool SocketEventLoop::add(
    SOCKET socket, long events, std::string &error)
{
    if (m_registrations.contains(socket))
        return true;
    if (m_registrations.size() + 1 >= WSA_MAXIMUM_WAIT_EVENTS) {
        error = "socket event loop capacity reached";
        return false;
    }
    WSAEVENT event = WSACreateEvent();
    if (event == WSA_INVALID_EVENT) {
        error = socketError("WSACreateEvent(socket)");
        return false;
    }
    if (WSAEventSelect(socket, event, events) == SOCKET_ERROR) {
        error = socketError("WSAEventSelect");
        WSACloseEvent(event);
        return false;
    }
    m_registrations.emplace(socket, Registration{event});
    return true;
}

void SocketEventLoop::remove(SOCKET socket)
{
    const auto it = m_registrations.find(socket);
    if (it == m_registrations.end())
        return;
    WSAEventSelect(socket, WSA_INVALID_EVENT, 0);
    if (it->second.event != WSA_INVALID_EVENT)
        WSACloseEvent(it->second.event);
    m_registrations.erase(it);
}

bool SocketEventLoop::poll(
    std::chrono::milliseconds timeout,
    std::vector<SocketActivation> &activations,
    bool &woken, std::string &error)
{
    activations.clear();
    woken = false;
    if (m_wakeEvent == WSA_INVALID_EVENT) {
        error = "socket event loop is not initialized";
        return false;
    }
    std::vector<WSAEVENT> handles;
    std::vector<SOCKET> sockets;
    handles.push_back(m_wakeEvent);
    sockets.push_back(INVALID_SOCKET);
    for (const auto &[socket, registration] : m_registrations) {
        handles.push_back(registration.event);
        sockets.push_back(socket);
    }
    const DWORD wait = WSAWaitForMultipleEvents(
        static_cast<DWORD>(handles.size()), handles.data(), FALSE,
        static_cast<DWORD>(std::clamp<std::int64_t>(
            timeout.count(), 0, std::numeric_limits<DWORD>::max())),
        FALSE);
    if (wait == WSA_WAIT_TIMEOUT)
        return true;
    if (wait == WSA_WAIT_FAILED) {
        error = socketError("WSAWaitForMultipleEvents");
        return false;
    }
    for (DWORD index = 0; index < handles.size(); ++index) {
        if (WSAWaitForMultipleEvents(
                1, &handles[index], TRUE, 0, FALSE)
            != WSA_WAIT_EVENT_0)
            continue;
        if (index == 0) {
            WSAResetEvent(m_wakeEvent);
            woken = true;
            continue;
        }
        WSANETWORKEVENTS networkEvents{};
        if (WSAEnumNetworkEvents(
                sockets[index], handles[index], &networkEvents)
            == SOCKET_ERROR) {
            error = socketError("WSAEnumNetworkEvents");
            return false;
        }
        SocketActivation activation;
        activation.socket = sockets[index];
        activation.events = networkEvents.lNetworkEvents;
        std::copy(
            std::begin(networkEvents.iErrorCode),
            std::end(networkEvents.iErrorCode),
            activation.errorCodes.begin());
        activations.push_back(activation);
    }
    return true;
}

void SocketEventLoop::wake()
{
    if (m_wakeEvent != WSA_INVALID_EVENT)
        WSASetEvent(m_wakeEvent);
}

TimerQueue::TimerId TimerQueue::schedule(
    TimePoint deadline, TimerId id)
{
    if (id == 0) {
        do {
            id = m_nextId++;
        } while (id == 0 || m_used.contains(id));
    } else if (m_used.contains(id)) {
        return 0;
    }
    m_used.insert(id);
    m_active.insert(id);
    m_timers.push(Timer{deadline, id, m_nextSequence++});
    return id;
}

bool TimerQueue::cancel(TimerId id)
{
    if (id == 0 || m_active.erase(id) == 0)
        return false;
    m_cancelled.insert(id);
    return true;
}

std::vector<TimerQueue::TimerId> TimerQueue::takeDue(TimePoint now)
{
    std::vector<TimerId> due;
    while (!m_timers.empty() && m_timers.top().deadline <= now) {
        const Timer timer = m_timers.top();
        m_timers.pop();
        if (m_cancelled.contains(timer.id)) {
            m_cancelled.erase(timer.id);
        } else if (m_active.erase(timer.id) != 0) {
            due.push_back(timer.id);
        }
    }
    return due;
}

std::optional<TimerQueue::TimePoint> TimerQueue::nextDeadline() const
{
    auto copy = m_timers;
    while (!copy.empty()) {
        const Timer timer = copy.top();
        copy.pop();
        if (!m_cancelled.contains(timer.id)
            && m_active.contains(timer.id))
            return timer.deadline;
    }
    return std::nullopt;
}

std::vector<Ipv4Interface> enumerateIpv4Interfaces(std::string &error)
{
    constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST
        | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 16 * 1024;
    std::vector<std::uint8_t> buffer(size);
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3; ++attempt) {
        result = GetAdaptersAddresses(
            AF_INET, flags, nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()),
            &size);
        if (result != ERROR_BUFFER_OVERFLOW)
            break;
        buffer.resize(size);
    }
    if (result != NO_ERROR) {
        error = "GetAdaptersAddresses failed with error "
            + std::to_string(result);
        return {};
    }

    std::vector<Ipv4Interface> interfaces;
    const auto *adapter = reinterpret_cast<const IP_ADAPTER_ADDRESSES *>(
        buffer.data());
    for (; adapter; adapter = adapter->Next) {
        for (const auto *address = adapter->FirstUnicastAddress;
             address; address = address->Next) {
            if (!address->Address.lpSockaddr
                || address->Address.lpSockaddr->sa_family != AF_INET)
                continue;
            const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(
                address->Address.lpSockaddr);
            char text[INET_ADDRSTRLEN]{};
            if (!inet_ntop(
                    AF_INET, &ipv4->sin_addr, text, sizeof(text)))
                continue;
            char name[512]{};
            WideCharToMultiByte(
                CP_UTF8, 0, adapter->FriendlyName, -1,
                name, sizeof(name) - 1, nullptr, nullptr);
            const std::uint32_t hostAddress =
                ntohl(ipv4->sin_addr.s_addr);
            interfaces.push_back(Ipv4Interface{
                name,
                text,
                adapter->IfIndex,
                adapter->IfType,
                adapter->Flags,
                address->OnLinkPrefixLength,
                adapter->OperStatus == IfOperStatusUp,
                adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK,
                (hostAddress & 0xFFFF0000u) == 0xA9FE0000u,
                (adapter->Flags & IP_ADAPTER_NO_MULTICAST) == 0,
            });
        }
    }
    return interfaces;
}

} // namespace mcast::platform::net
