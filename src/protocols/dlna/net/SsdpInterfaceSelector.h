// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "platform/windows/net/SocketRuntime.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mcast::dlna::net {

enum class SsdpInterfaceKind {
    Wifi,
    Ethernet,
    Other,
};

struct SsdpInterfaceCandidate {
    platform::net::Ipv4Interface interface;
    SsdpInterfaceKind kind = SsdpInterfaceKind::Other;
    int score = 0;
};

struct SsdpInterfaceRejection {
    platform::net::Ipv4Interface interface;
    std::string reason;
};

struct SsdpInterfaceSelection {
    std::optional<SsdpInterfaceCandidate> selected;
    std::vector<SsdpInterfaceCandidate> candidates;
    std::vector<SsdpInterfaceRejection> rejected;
};

std::string_view ssdpInterfaceKindName(SsdpInterfaceKind kind);

SsdpInterfaceSelection selectSsdpInterface(
    std::span<const platform::net::Ipv4Interface> interfaces);

} // namespace mcast::dlna::net
