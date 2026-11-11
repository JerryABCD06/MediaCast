// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "playback/FakePlaybackGateway.h"
#include "protocols/dlna/claim/DlnaClaimHttpHandler.h"
#include "protocols/dlna/runtime/DlnaHttpFrontend.h"
#include "protocols/dlna/session/DlnaSession.h"
#include "protocols/dlna/soap/SoapRouter.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

using namespace mcast::cast;
using namespace mcast::dlna;
using namespace mcast::dlna::claim;
using namespace mcast::dlna::soap;
using namespace mcast::playback;

namespace {

void check(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

core::HttpRequest request(std::string method, std::string target)
{
    core::HttpRequest value;
    value.requestId = 1;
    value.method = std::move(method);
    value.target = std::move(target);
    value.version = "HTTP/1.1";
    value.peerAddress = "192.0.2.10";
    return value;
}

void testDescriptionAndScpdRoutes()
{
    FakePlaybackGateway playback;
    DlnaSession session{playback, {}};
    CastClaimCoordinator coordinator;
    DlnaClaimAdapter adapter{coordinator};
    DlnaClaimRuntime runtime{adapter};
    SoapRouter router{session, {}};
    DlnaClaimHttpHandler handler{
        router, runtime, [] { return CastTime{100}; }};
    runtime::DlnaHttpFrontend frontend{
        handler, {"Media & Cast", "uuid:test"}};

    auto response = frontend.handle(request(
        "GET", "/description.xml?source=test"));
    check(response.status == net::HttpDispatchStatus::Completed
              && response.response.status == 200
              && response.response.body.find("Media &amp; Cast")
                  != std::string::npos
              && response.response.body.find("uuid:test")
                  != std::string::npos,
          "description route serves escaped device identity");

    response = frontend.handle(request(
        "GET", "/AVTransport/scpd"));
    check(response.response.status == 200
              && response.response.body.find(
                  "<name>SetAVTransportURI</name>")
                  != std::string::npos,
          "AVTransport SCPD route is served");

    response = frontend.handle(request(
        "GET", "/RenderingControl/scpd"));
    check(response.response.status == 200
              && response.response.body.find("<name>SetVolume</name>")
                  != std::string::npos,
          "RenderingControl SCPD route is served");

    response = frontend.handle(request(
        "GET", "/ConnectionManager/scpd"));
    check(response.response.status == 200
              && response.response.body.find("<name>GetProtocolInfo</name>")
                  != std::string::npos,
          "ConnectionManager SCPD route is served");
}

void testErrorsAndSoapPassthrough()
{
    FakePlaybackGateway playback;
    DlnaSession session{playback, {}};
    CastClaimCoordinator coordinator;
    DlnaClaimAdapter adapter{coordinator};
    DlnaClaimRuntime runtime{adapter};
    SoapRouter router{session, {}};
    DlnaClaimHttpHandler handler{
        router, runtime, [] { return CastTime{100}; }};
    runtime::DlnaHttpFrontend frontend{
        handler, {"Media Cast", "uuid:test"}};

    auto response = frontend.handle(request("GET", "/missing"));
    check(response.response.status == 404
              && response.response.contentType
                  == "text/plain; charset=utf-8",
          "unknown GET route is a plain 404");

    response = frontend.handle(request("PUT", "/description.xml"));
    check(response.response.status == 405
              && response.response.headers.size() == 1
              && response.response.headers.front().first == "allow"
              && response.response.headers.front().second
                  == "GET, POST",
          "unsupported method reports the allowed methods");

    auto malformed = request("POST", "/AVTransport/control");
    malformed.requestId = 0;
    response = frontend.handle(malformed);
    check(response.status == net::HttpDispatchStatus::Completed
              && response.response.status == 500
              && response.response.body.find(
                  "<errorCode>501</errorCode>")
                  != std::string::npos,
          "POST is delegated to the fail-closed SOAP claim handler");
}

} // namespace

int main()
{
    testDescriptionAndScpdRoutes();
    testErrorsAndSoapPassthrough();
    std::cout << "DLNA HTTP frontend tests passed\n";
    return 0;
}
