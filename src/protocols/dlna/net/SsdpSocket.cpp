// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/net/SsdpSocket.h"

#include <mswsock.h>

#include <optional>
#include <utility>

namespace mcast::dlna::net {
namespace {

std::string socketError(std::string_view operation)
{
    return std::string(operation) + " failed with WSA error "
        + std::to_string(WSAGetLastError());
}

std::optional<in_addr> parseIpv4(std::string_view address)
{
    in_addr parsed{};
    const std::string text(address);
    if (InetPtonA(AF_INET, text.c_str(), &parsed) != 1)
        return std::nullopt;
    return parsed;
}

bool isMulticastAddress(const in_addr &address)
{
    const std::uint32_t host = ntohl(address.s_addr);
    return (host & 0xF0000000u) == 0xE0000000u;
}

} // namespace

bool SsdpSocket::open(std::uint16_t port, std::string &error)
{
    close();
    error.clear();
    if (!m_socket.createUdp(error)
        || !m_socket.setNonBlocking(error)) {
        close();
        return false;
    }

    const int enabled = 1;
    if (setsockopt(
            m_socket.get(), SOL_SOCKET, SO_REUSEADDR,
            reinterpret_cast<const char *>(&enabled), sizeof(enabled))
        == SOCKET_ERROR) {
        error = socketError("setsockopt(SO_REUSEADDR)");
        close();
        return false;
    }

    BOOL reportUdpReset = FALSE;
    DWORD bytesReturned = 0;
    WSAIoctl(
        m_socket.get(), SIO_UDP_CONNRESET,
        &reportUdpReset, sizeof(reportUdpReset), nullptr, 0,
        &bytesReturned, nullptr, nullptr);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(
            m_socket.get(), reinterpret_cast<const sockaddr *>(&address),
            sizeof(address)) == SOCKET_ERROR) {
        error = socketError("bind(SSDP)");
        close();
        return false;
    }
    return true;
}

void SsdpSocket::close() noexcept
{
    m_socket.close();
}

bool SsdpSocket::setMulticastInterface(
    std::string_view interfaceAddress, std::string &error)
{
    error.clear();
    const auto interface = parseIpv4(interfaceAddress);
    if (!m_socket.valid() || !interface) {
        error = "invalid SSDP multicast interface";
        return false;
    }
    if (setsockopt(
            m_socket.get(), IPPROTO_IP, IP_MULTICAST_IF,
            reinterpret_cast<const char *>(&*interface),
            sizeof(*interface)) == SOCKET_ERROR) {
        error = socketError("setsockopt(IP_MULTICAST_IF)");
        return false;
    }
    return true;
}

bool SsdpSocket::joinMulticastGroup(
    std::string_view groupAddress, std::string_view interfaceAddress,
    std::string &error)
{
    error.clear();
    const auto group = parseIpv4(groupAddress);
    const auto interface = parseIpv4(interfaceAddress);
    if (!m_socket.valid() || !group || !interface
        || !isMulticastAddress(*group)
        || interface->s_addr == htonl(INADDR_ANY)) {
        error = "invalid SSDP multicast membership";
        return false;
    }

    ip_mreq membership{};
    membership.imr_multiaddr = *group;
    membership.imr_interface = *interface;
    if (setsockopt(
            m_socket.get(), IPPROTO_IP, IP_ADD_MEMBERSHIP,
            reinterpret_cast<const char *>(&membership),
            sizeof(membership)) == SOCKET_ERROR) {
        error = socketError("setsockopt(IP_ADD_MEMBERSHIP)");
        return false;
    }
    return true;
}

bool SsdpSocket::leaveMulticastGroup(
    std::string_view groupAddress, std::string_view interfaceAddress,
    std::string &error)
{
    error.clear();
    const auto group = parseIpv4(groupAddress);
    const auto interface = parseIpv4(interfaceAddress);
    if (!m_socket.valid() || !group || !interface
        || !isMulticastAddress(*group)
        || interface->s_addr == htonl(INADDR_ANY)) {
        error = "invalid SSDP multicast membership";
        return false;
    }

    ip_mreq membership{};
    membership.imr_multiaddr = *group;
    membership.imr_interface = *interface;
    if (setsockopt(
            m_socket.get(), IPPROTO_IP, IP_DROP_MEMBERSHIP,
            reinterpret_cast<const char *>(&membership),
            sizeof(membership)) == SOCKET_ERROR) {
        error = socketError("setsockopt(IP_DROP_MEMBERSHIP)");
        return false;
    }
    return true;
}

SsdpSendStatus SsdpSocket::sendDatagram(
    std::span<const std::uint8_t> bytes, std::string_view address,
    std::uint16_t port, std::string &error)
{
    error.clear();
    const auto destinationAddress = parseIpv4(address);
    if (!m_socket.valid() || !destinationAddress || port == 0) {
        error = "invalid SSDP destination";
        return SsdpSendStatus::Error;
    }
    if (bytes.size() > MaxDatagramBytes) {
        error = "SSDP datagram is too large";
        return SsdpSendStatus::Error;
    }

    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_addr = *destinationAddress;
    destination.sin_port = htons(port);
    const int sent = sendto(
        m_socket.get(),
        reinterpret_cast<const char *>(bytes.data()),
        static_cast<int>(bytes.size()), 0,
        reinterpret_cast<const sockaddr *>(&destination),
        sizeof(destination));
    if (sent == SOCKET_ERROR) {
        if (WSAGetLastError() == WSAEWOULDBLOCK)
            return SsdpSendStatus::WouldBlock;
        error = socketError("sendto(SSDP)");
        return SsdpSendStatus::Error;
    }
    if (static_cast<std::size_t>(sent) != bytes.size()) {
        error = "SSDP datagram was only partially sent";
        return SsdpSendStatus::Error;
    }
    return SsdpSendStatus::Sent;
}

SsdpReceiveStatus SsdpSocket::receiveDatagram(
    SsdpDatagram &datagram, std::string &error)
{
    datagram = {};
    error.clear();
    if (!m_socket.valid()) {
        error = "SSDP socket is not open";
        return SsdpReceiveStatus::Error;
    }

    std::vector<std::uint8_t> buffer(MaxDatagramBytes + 1);
    sockaddr_in source{};
    int sourceSize = sizeof(source);
    const int received = recvfrom(
        m_socket.get(), reinterpret_cast<char *>(buffer.data()),
        static_cast<int>(buffer.size()), 0,
        reinterpret_cast<sockaddr *>(&source), &sourceSize);
    if (received == SOCKET_ERROR) {
        const int socketErrorCode = WSAGetLastError();
        if (socketErrorCode == WSAEWOULDBLOCK)
            return SsdpReceiveStatus::WouldBlock;
        if (socketErrorCode == WSAEMSGSIZE)
            return SsdpReceiveStatus::Oversize;
        if (socketErrorCode == WSAECONNRESET)
            return SsdpReceiveStatus::Reset;
        error = socketError("recvfrom(SSDP)");
        return SsdpReceiveStatus::Error;
    }
    if (static_cast<std::size_t>(received) > MaxDatagramBytes)
        return SsdpReceiveStatus::Oversize;

    char addressText[INET_ADDRSTRLEN]{};
    if (!InetNtopA(
            AF_INET, &source.sin_addr, addressText,
            sizeof(addressText))) {
        error = socketError("InetNtopA(SSDP)");
        return SsdpReceiveStatus::Error;
    }
    datagram.sourceAddress = addressText;
    datagram.sourcePort = ntohs(source.sin_port);
    buffer.resize(static_cast<std::size_t>(received));
    datagram.bytes = std::move(buffer);
    return SsdpReceiveStatus::Received;
}

std::uint16_t SsdpSocket::localPort() const noexcept
{
    return m_socket.localPort().value_or(0);
}

} // namespace mcast::dlna::net
