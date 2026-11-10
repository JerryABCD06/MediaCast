// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcast::dlna::core {

struct HttpRequest {
    std::string method;
    std::string target;
    std::string version;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    std::optional<std::string> header(std::string_view name) const;
};

enum class HttpParseStatus {
    Complete,
    NeedMoreData,
    Error,
};

struct HttpParseLimits {
    std::size_t maxHeaderBytes = 64 * 1024;
    std::size_t maxBodyBytes = 1024 * 1024;
    std::size_t maxHeaderCount = 128;
};

HttpParseStatus parseHttpRequest(
    std::span<const std::uint8_t> bytes, HttpRequest &request,
    std::size_t &consumed, std::string &error,
    const HttpParseLimits &limits = {});

} // namespace mcast::dlna::core
