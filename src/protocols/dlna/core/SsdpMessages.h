// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "protocols/dlna/core/HttpRequest.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace mcast::dlna::core {

struct SsdpRequest {
    std::string method;
    std::string target;
    HttpRequest http;

    std::optional<std::string> header(std::string_view name) const
    {
        return http.header(name);
    }
};

bool parseSsdpRequest(
    std::span<const std::uint8_t> bytes, SsdpRequest &request,
    std::string &error);

std::string ssdpUsn(
    std::string_view udn, std::string_view notificationType);

std::string makeSsdpNotify(
    bool alive, std::string_view notificationType, std::string_view udn,
    std::string_view locationUrl, std::uint32_t maxAgeSeconds,
    std::string_view serverHeader, std::uint32_t bootId,
    std::string &error);

std::string makeSsdpSearchResponse(
    std::string_view searchTarget, std::string_view udn,
    std::string_view locationUrl, std::uint32_t maxAgeSeconds,
    std::string_view serverHeader, std::string_view dateHeader,
    std::uint32_t bootId, std::string &error);

} // namespace mcast::dlna::core
