// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/core/SsdpMessages.h"

#include <sstream>

namespace mcast::dlna::core {
namespace {

bool safe(std::string_view value)
{
    return value.find_first_of("\r\n") == std::string_view::npos;
}

bool validFields(
    std::initializer_list<std::string_view> fields, std::string &error)
{
    for (std::string_view field : fields) {
        if (field.empty() || !safe(field)) {
            error = "invalid SSDP header value";
            return false;
        }
    }
    return true;
}

} // namespace

bool parseSsdpRequest(
    std::span<const std::uint8_t> bytes, SsdpRequest &request,
    std::string &error)
{
    request = {};
    error.clear();
    std::size_t consumed = 0;
    HttpParseLimits limits;
    limits.maxHeaderBytes = 16 * 1024;
    limits.maxBodyBytes = 0;
    limits.maxHeaderCount = 64;
    const auto status = parseHttpRequest(
        bytes, request.http, consumed, error, limits);
    if (status != HttpParseStatus::Complete) {
        if (status == HttpParseStatus::NeedMoreData)
            error = "incomplete SSDP request";
        return false;
    }
    if (consumed != bytes.size()) {
        error = "trailing data after SSDP request";
        return false;
    }
    if (request.http.method != "M-SEARCH"
        || request.http.version != "HTTP/1.1") {
        error = "not an SSDP M-SEARCH request";
        return false;
    }
    request.method = request.http.method;
    request.target = request.http.target;
    return true;
}

std::string ssdpUsn(
    std::string_view udn, std::string_view notificationType)
{
    if (notificationType.starts_with("uuid:"))
        return std::string(notificationType);
    return std::string(udn) + "::" + std::string(notificationType);
}

std::string makeSsdpNotify(
    bool alive, std::string_view notificationType, std::string_view udn,
    std::string_view locationUrl, std::uint32_t maxAgeSeconds,
    std::string_view serverHeader, std::uint32_t bootId,
    std::string &error)
{
    error.clear();
    const std::string usn = ssdpUsn(udn, notificationType);
    if (!validFields({notificationType, udn, usn}, error))
        return {};
    if (alive && !validFields({locationUrl, serverHeader}, error))
        return {};

    std::ostringstream message;
    message << "NOTIFY * HTTP/1.1\r\n"
            << "HOST: 239.255.255.250:1900\r\n"
            << "NT: " << notificationType << "\r\n"
            << "NTS: " << (alive ? "ssdp:alive" : "ssdp:byebye") << "\r\n"
            << "USN: " << usn << "\r\n";
    if (alive) {
        message << "CACHE-CONTROL: max-age=" << maxAgeSeconds << "\r\n"
                << "LOCATION: " << locationUrl << "\r\n"
                << "SERVER: " << serverHeader << "\r\n"
                << "BOOTID.UPNP.ORG: " << bootId << "\r\n";
    }
    message << "\r\n";
    return message.str();
}

std::string makeSsdpSearchResponse(
    std::string_view searchTarget, std::string_view udn,
    std::string_view locationUrl, std::uint32_t maxAgeSeconds,
    std::string_view serverHeader, std::string_view dateHeader,
    std::uint32_t bootId, std::string &error)
{
    error.clear();
    const std::string usn = ssdpUsn(udn, searchTarget);
    if (!validFields(
            {searchTarget, udn, usn, locationUrl, serverHeader, dateHeader},
            error))
        return {};

    std::ostringstream message;
    message << "HTTP/1.1 200 OK\r\n"
            << "CACHE-CONTROL: max-age=" << maxAgeSeconds << "\r\n"
            << "DATE: " << dateHeader << "\r\n"
            << "EXT:\r\n"
            << "LOCATION: " << locationUrl << "\r\n"
            << "SERVER: " << serverHeader << "\r\n"
            << "ST: " << searchTarget << "\r\n"
            << "USN: " << usn << "\r\n"
            << "BOOTID.UPNP.ORG: " << bootId << "\r\n"
            << "\r\n";
    return message.str();
}

} // namespace mcast::dlna::core
