// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/net/SsdpInterfaceSelector.h"

#include <iphlpapi.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <utility>

namespace mcast::dlna::net {
namespace {

bool isEthernetType(unsigned long ifType)
{
    return ifType == IF_TYPE_ETHERNET_CSMACD
        || ifType == IF_TYPE_FASTETHER
        || ifType == IF_TYPE_GIGABITETHERNET;
}

bool isUnsupportedTunnelType(unsigned long ifType)
{
    return ifType == IF_TYPE_PPP
        || ifType == IF_TYPE_SLIP
        || ifType == IF_TYPE_PPPMULTILINKBUNDLE
        || ifType == IF_TYPE_TUNNEL
        || ifType == IF_TYPE_PROP_VIRTUAL
        || ifType == IF_TYPE_ATM_VIRTUAL;
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

bool looksVirtual(std::string_view name, std::string_view description)
{
    const std::string text =
        lower(std::string(name) + " " + std::string(description));
    static constexpr std::string_view markers[] = {
        "virtual", "hyper-v", "vethernet", "vmware", "virtualbox",
        "wintun", "wireguard", "tailscale", "zerotier", "vpn",
        "tunnel", "pseudo", "npcap", "虚拟", "隧道",
    };
    for (std::string_view marker : markers) {
        if (text.find(marker) != std::string::npos)
            return true;
    }
    return text.starts_with("tap")
        || text.find(" tap-") != std::string::npos
        || text.find(" tap ") != std::string::npos;
}

std::optional<std::uint32_t> parseIpv4(std::string_view address)
{
    in_addr parsed{};
    const std::string text(address);
    if (InetPtonA(AF_INET, text.c_str(), &parsed) != 1)
        return std::nullopt;
    return ntohl(parsed.s_addr);
}

std::string rejectionReason(const platform::net::Ipv4Interface &interface)
{
    if (!interface.up)
        return "adapter is down";
    if (!interface.hardwareInterface)
        return "software or virtual adapter";
    if (interface.filterInterface)
        return "network filter interface";
    if (isUnsupportedTunnelType(interface.ifType))
        return "tunnel or point-to-point adapter";
    if (looksVirtual(interface.name, interface.description))
        return "virtual adapter";
    if (!interface.canMulticast)
        return "adapter does not support multicast";
    if (interface.isLoopback)
        return "loopback adapter";
    const auto address = parseIpv4(interface.address);
    if (!address)
        return "invalid IPv4 address";
    if ((*address & 0xFF000000u) == 0x7F000000u)
        return "loopback address";
    if (interface.isLinkLocal
        || (*address & 0xFFFF0000u) == 0xA9FE0000u)
        return "link-local address";
    return {};
}

SsdpInterfaceKind interfaceKind(unsigned long ifType)
{
    if (ifType == IF_TYPE_IEEE80211)
        return SsdpInterfaceKind::Wifi;
    if (isEthernetType(ifType))
        return SsdpInterfaceKind::Ethernet;
    return SsdpInterfaceKind::Other;
}

int interfaceScore(SsdpInterfaceKind kind)
{
    switch (kind) {
    case SsdpInterfaceKind::Wifi:
        return 100;
    case SsdpInterfaceKind::Ethernet:
        return 80;
    case SsdpInterfaceKind::Other:
        return 10;
    }
    return 0;
}

bool betterCandidate(
    const SsdpInterfaceCandidate &left,
    const SsdpInterfaceCandidate &right)
{
    if (left.score != right.score)
        return left.score > right.score;
    if (left.interface.ipv4Metric != right.interface.ipv4Metric) {
        return left.interface.ipv4Metric
            < right.interface.ipv4Metric;
    }
    if (left.interface.index != right.interface.index)
        return left.interface.index < right.interface.index;
    if (left.interface.name != right.interface.name)
        return left.interface.name < right.interface.name;
    return left.interface.address < right.interface.address;
}

} // namespace

std::string_view ssdpInterfaceKindName(SsdpInterfaceKind kind)
{
    switch (kind) {
    case SsdpInterfaceKind::Wifi:
        return "Wi-Fi";
    case SsdpInterfaceKind::Ethernet:
        return "Ethernet";
    case SsdpInterfaceKind::Other:
        return "Other";
    }
    return "Other";
}

SsdpInterfaceSelection selectSsdpInterface(
    std::span<const platform::net::Ipv4Interface> interfaces)
{
    SsdpInterfaceSelection selection;
    for (const auto &interface : interfaces) {
        std::string reason = rejectionReason(interface);
        if (!reason.empty()) {
            selection.rejected.push_back(
                SsdpInterfaceRejection{interface, std::move(reason)});
            continue;
        }

        const auto kind = interfaceKind(interface.ifType);
        SsdpInterfaceCandidate candidate{
            interface, kind, interfaceScore(kind)};
        selection.candidates.push_back(candidate);
        if (!selection.selected
            || betterCandidate(candidate, *selection.selected)) {
            selection.selected = std::move(candidate);
        }
    }
    return selection;
}

} // namespace mcast::dlna::net
