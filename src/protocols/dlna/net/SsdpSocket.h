// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "platform/windows/net/SocketRuntime.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mcast::dlna::net {

enum class SsdpReceiveStatus {
    Received,
    WouldBlock,
    Oversize,
    Reset,
    Error,
};

enum class SsdpSendStatus {
    Sent,
    WouldBlock,
    Error,
};

struct SsdpDatagram {
    std::string sourceAddress;
    std::uint16_t sourcePort = 0;
    std::vector<std::uint8_t> bytes;
};

class SsdpSocket
{
public:
    static constexpr std::string_view MulticastGroup =
        "239.255.255.250";
    static constexpr std::uint16_t DefaultPort = 1900;
    static constexpr std::size_t MaxDatagramBytes = 16 * 1024;

    SsdpSocket() = default;
    ~SsdpSocket() = default;

    SsdpSocket(const SsdpSocket &) = delete;
    SsdpSocket &operator=(const SsdpSocket &) = delete;

    bool open(std::uint16_t port, std::string &error);
    void close() noexcept;

    bool setMulticastInterface(
        std::string_view interfaceAddress, std::string &error);
    bool joinMulticastGroup(
        std::string_view groupAddress, std::string_view interfaceAddress,
        std::string &error);
    bool leaveMulticastGroup(
        std::string_view groupAddress, std::string_view interfaceAddress,
        std::string &error);

    SsdpSendStatus sendDatagram(
        std::span<const std::uint8_t> bytes, std::string_view address,
        std::uint16_t port, std::string &error);
    SsdpReceiveStatus receiveDatagram(
        SsdpDatagram &datagram, std::string &error);

    SOCKET handle() const noexcept
    {
        return m_socket.get();
    }
    bool valid() const noexcept { return m_socket.valid(); }
    std::uint16_t localPort() const noexcept;

private:
    platform::net::SocketHandle m_socket;
};

} // namespace mcast::dlna::net
