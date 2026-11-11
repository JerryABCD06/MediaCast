// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/runtime/DlnaHttpFrontend.h"

#include "protocols/dlna/UpnpXml.h"

#include <utility>

namespace mcast::dlna::runtime {
namespace {

net::HttpResponse textResponse(
    int status, std::string body, std::string contentType)
{
    net::HttpResponse response;
    response.status = status;
    response.contentType = std::move(contentType);
    response.body = std::move(body);
    return response;
}

std::string pathOf(std::string target)
{
    const std::size_t query = target.find('?');
    if (query != std::string::npos)
        target.resize(query);
    return target;
}

} // namespace

DlnaHttpFrontend::DlnaHttpFrontend(
    claim::DlnaClaimHttpHandler &soap,
    DlnaHttpFrontendOptions options)
    : m_soap(soap)
    , m_options(std::move(options))
{
}

net::HttpDispatchResult DlnaHttpFrontend::handle(
    const mcast::dlna::core::HttpRequest &request)
{
    if (request.method == "POST")
        return m_soap.handle(request);

    if (request.method != "GET") {
        net::HttpResponse response = textResponse(
            405, "Method Not Allowed", "text/plain; charset=utf-8");
        response.headers.emplace_back("allow", "GET, POST");
        return {net::HttpDispatchStatus::Completed, std::move(response)};
    }

    const std::string path = pathOf(request.target);
    std::string body;
    if (path == "/description.xml") {
        body = UpnpXml::deviceDescription(
            m_options.friendlyName, m_options.udn);
    } else if (path == "/AVTransport/scpd") {
        body = UpnpXml::avTransportScpd();
    } else if (path == "/RenderingControl/scpd") {
        body = UpnpXml::renderingControlScpd();
    } else if (path == "/ConnectionManager/scpd") {
        body = UpnpXml::connectionManagerScpd();
    } else {
        return {
            net::HttpDispatchStatus::Completed,
            textResponse(
                404, "Not Found", "text/plain; charset=utf-8"),
        };
    }

    net::HttpResponse response = textResponse(
        200, std::move(body), "text/xml; charset=utf-8");
    response.headers.emplace_back("cache-control", "no-cache");
    return {net::HttpDispatchStatus::Completed, std::move(response)};
}

} // namespace mcast::dlna::runtime
