// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcast::dlna::soap {

struct SoapRequestXml {
    std::string action;
    std::vector<std::pair<std::string, std::string>> parameters;

    std::optional<std::string> value(std::string_view name) const;
};

struct DidlResource {
    std::string uri;
    std::optional<std::string> protocolInfo;
    std::optional<std::string> resolution;
};

struct DidlMetadata {
    std::optional<std::string> title;
    std::optional<std::string> upnpClass;
    std::optional<std::string> artist;
    std::optional<std::string> creator;
    std::optional<std::string> album;
    std::optional<std::string> subtitle;
    std::optional<std::string> description;
    std::optional<std::string> longDescription;
    std::optional<std::string> albumArtUri;
    std::vector<DidlResource> resources;
};

bool parseSoapRequest(
    std::string_view xml, std::string_view expectedService,
    SoapRequestXml &request, std::string &error);

bool parseDidlMetadata(
    std::string_view xml, DidlMetadata &metadata, std::string &error);

} // namespace mcast::dlna::soap
