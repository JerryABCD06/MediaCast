// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "playback/PlaybackTypes.h"
#include "protocols/dlna/core/HttpRequest.h"
#include "protocols/dlna/net/HttpServer.h"
#include "protocols/dlna/soap/SoapXml.h"
#include "protocols/dlna/session/DlnaSession.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace mcast::dlna::soap {

using HttpResponse = net::HttpResponse;

struct SoapRequestContext {
    std::string service;
    std::string action;
    SoapRequestXml body;
};

struct SoapRouterOptions {
    std::function<SessionTime()> now;
    std::function<void(const std::string &)> logger;
};

class SoapRouter
{
public:
    explicit SoapRouter(
        DlnaSession &session, SoapRouterOptions options = {});

    HttpResponse handle(const core::HttpRequest &request);
    std::optional<SoapRequestContext> parseRequest(
        const core::HttpRequest &request,
        HttpResponse &errorResponse);
    HttpResponse handleParsed(const SoapRequestContext &request);
    void notePeerActivity();

    static playback::MediaRequest mediaRequest(
        std::string uri, std::string metadata);
    static HttpResponse successResponse(
        const std::string &service, const std::string &action);
    static HttpResponse faultResponse(
        int code, std::string_view text);

private:
    HttpResponse dispatch(
        const std::string &service, const std::string &action,
        const SoapRequestXml &body);
    HttpResponse handleAvTransport(
        const std::string &action, const SoapRequestXml &body);
    HttpResponse handleRenderingControl(
        const std::string &action, const SoapRequestXml &body);
    HttpResponse handleConnectionManager(
        const std::string &action, const SoapRequestXml &body);

    HttpResponse submit(
        playback::PlaybackCommand command, const std::string &service,
        const std::string &action);
    HttpResponse submitMedia(
        playback::MediaRequest media, const std::string &service,
        const std::string &action);
    void log(const std::string &message) const;

    DlnaSession &m_session;
    SoapRouterOptions m_options;
};

} // namespace mcast::dlna::soap
