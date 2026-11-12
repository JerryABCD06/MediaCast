// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "playback/FakePlaybackGateway.h"
#include "protocols/dlna/UpnpXml.h"
#include "protocols/dlna/claim/DlnaClaimHttpHandler.h"
#include "protocols/dlna/session/DlnaSession.h"
#include "protocols/dlna/soap/SoapRouter.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

std::string body(std::string action, std::string arguments = {})
{
    return "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
        "<s:Body><u:" + action
        + " xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        + arguments + "</u:" + action + "></s:Body></s:Envelope>";
}

mcast::dlna::core::HttpRequest request(
    std::uint64_t requestId, std::string address,
    std::string action, std::string arguments = {})
{
    mcast::dlna::core::HttpRequest value;
    value.requestId = requestId;
    value.peerAddress = std::move(address);
    value.method = "POST";
    value.target = "/AVTransport/control";
    value.version = "HTTP/1.1";
    value.headers.emplace_back("user-agent", "MCast-Test-Control");
    value.headers.emplace_back(
        "soapaction",
        "\"urn:schemas-upnp-org:service:AVTransport:1#" + action + "\"");
    value.body = body(action, std::move(arguments));
    return value;
}

mcast::dlna::core::HttpRequest setUriRequest(
    std::uint64_t requestId, const std::string &address,
    const std::string &uri)
{
    const std::string didl =
        "<DIDL-Lite><item><dc:title xmlns:dc=\"urn:test\">"
        "Test Movie</dc:title></item></DIDL-Lite>";
    return request(
        requestId, address, "SetAVTransportURI",
        "<InstanceID>0</InstanceID><CurrentURI>"
            + UpnpXml::escapeText(uri)
            + "</CurrentURI><CurrentURIMetaData>"
            + UpnpXml::escapeText(didl)
            + "</CurrentURIMetaData>");
}

mcast::dlna::core::HttpRequest playRequest(
    std::uint64_t requestId, const std::string &address)
{
    return request(
        requestId, address, "Play",
        "<InstanceID>0</InstanceID><Speed>1</Speed>");
}

mcast::dlna::core::HttpRequest stopRequest(
    std::uint64_t requestId, const std::string &address)
{
    return request(
        requestId, address, "Stop",
        "<InstanceID>0</InstanceID>");
}

struct Harness {
    FakePlaybackGateway playback;
    DlnaSession session{playback, {}};
    CastClaimCoordinator coordinator{ArbitrationPolicy{
        true, true, CastTime{10000}, CastTime{5000}, 8}};
    DlnaClaimAdapter adapter{coordinator};
    DlnaClaimRuntime runtime{adapter};
    SoapRouter router{session, {}};
    std::vector<std::pair<std::uint64_t, mcast::dlna::net::HttpResponse>>
        completions;
    std::vector<ConfirmationPrompt> confirmations;
    std::vector<DlnaMediaExecution> executions;
    int playCalls = 0;
    int stopCalls = 0;
    std::string stoppedAddress;
    bool throwExecute = false;
    bool openMediaOnExecute = true;
    DlnaClaimHttpHandler handler;

    Harness()
        : handler(
              router, runtime,
              [] { return CastTime{100}; },
              DlnaClaimHttpCallbacks{
                  [this](std::uint64_t id,
                         mcast::dlna::net::HttpResponse response) {
                      completions.emplace_back(id, std::move(response));
                      return true;
                  },
                  [this](const ConfirmationPrompt &prompt) {
                      confirmations.push_back(prompt);
                  },
                  [this](const DlnaMediaExecution &execution) {
                      if (throwExecute)
                          throw std::runtime_error("execute failed");
                      executions.push_back(execution);
                      if (openMediaOnExecute) {
                          session.openMedia(
                              execution.media, SessionTime{100}, false);
                          playback.processAll();
                      }
                  },
                  [this](std::optional<std::uint64_t>) { ++playCalls; },
                  [this](
                      const PeerIdentity &peer,
                      std::optional<std::uint64_t>) {
                      ++stopCalls;
                      stoppedAddress = peer.address;
                  },
                  {},
                  {},
              })
    {
    }

    const mcast::dlna::net::HttpResponse *completion(
        std::uint64_t requestId) const
    {
        for (const auto &[id, response] : completions) {
            if (id == requestId)
                return &response;
        }
        return nullptr;
    }
};

void testFirstMediaWaitsForReadyAndOwnerControls()
{
    Harness harness;
    const std::string address = "192.0.2.10";
    auto result = harness.handler.handle(setUriRequest(
        101, address, "http://example.test/movie.mp4"));
    check(result.status == mcast::dlna::net::HttpDispatchStatus::Pending,
          "first SetURI handler stays pending");
    check(harness.executions.size() == 1
              && harness.completions.empty(),
          "first SetURI executes media without completing HTTP");
    const std::uint64_t operationId = harness.executions[0].operationId;
    auto transitioningPause = harness.handler.handle(request(
        100, address, "Pause", "<InstanceID>0</InstanceID>"));
    check(transitioningPause.status
              == mcast::dlna::net::HttpDispatchStatus::Completed
              && transitioningPause.response.status == 500
              && transitioningPause.response.body.find(
                  "<errorCode>701</errorCode>")
                  != std::string::npos,
          "controls are rejected while the initial media is opening");

    harness.handler.mediaReady(operationId, true, CastTime{101});
    const auto *setResponse = harness.completion(101);
    check(setResponse
              && setResponse->status == 200
              && setResponse->body.find(
                  "SetAVTransportURIResponse")
                  != std::string::npos,
          "media readiness completes SetURI with SOAP success");

    auto ownerPlay = playRequest(102, address);
    ownerPlay.headers.erase(
        std::remove_if(
            ownerPlay.headers.begin(), ownerPlay.headers.end(),
            [](const auto &header) {
                return header.first == "user-agent";
            }),
        ownerPlay.headers.end());
    result = harness.handler.handle(ownerPlay);
    check(result.status == mcast::dlna::net::HttpDispatchStatus::Pending
              && harness.playCalls == 1
              && harness.completion(102)
              && harness.completion(102)->status == 200,
          "owner Play executes and completes HTTP");

    result = harness.handler.handle(stopRequest(103, address));
    check(result.status == mcast::dlna::net::HttpDispatchStatus::Pending
              && harness.stopCalls == 1
              && harness.stoppedAddress == address
              && harness.completion(103)
              && harness.completion(103)->status == 200,
          "owner Stop executes and completes HTTP");
}

void testCandidateConfirmationHoldsSetAndPlay()
{
    Harness harness;
    auto result = harness.handler.handle(setUriRequest(
        201, "192.0.2.10", "http://example.test/old.mp4"));
    check(result.status == mcast::dlna::net::HttpDispatchStatus::Pending
              && harness.executions.size() == 1,
          "candidate test old SetURI starts");
    harness.handler.mediaReady(
        harness.executions[0].operationId, true, CastTime{101});
    harness.completions.clear();

    result = harness.handler.handle(setUriRequest(
        202, "192.0.2.20", "http://example.test/new.mp4"));
    check(result.status == mcast::dlna::net::HttpDispatchStatus::Pending
              && harness.confirmations.size() == 1
              && harness.completions.empty(),
          "new peer SetURI requests confirmation and stays pending");
    const ClaimKey key = harness.confirmations[0].key;

    result = harness.handler.handle(playRequest(203, "192.0.2.20"));
    check(result.status == mcast::dlna::net::HttpDispatchStatus::Pending
              && harness.playCalls == 0
              && harness.completions.empty(),
          "candidate Play waits with confirmation");

    result = harness.handler.handle(request(
        204, "192.0.2.20", "Pause",
        "<InstanceID>0</InstanceID>"));
    check(result.status == mcast::dlna::net::HttpDispatchStatus::Completed
              && result.response.status == 500
              && result.response.body.find("<errorCode>701</errorCode>")
                  != std::string::npos,
          "other candidate AVTransport mutation is rejected");

    result = harness.handler.handle(stopRequest(
        205, "192.0.2.20"));
    check(result.status == mcast::dlna::net::HttpDispatchStatus::Pending
              && harness.stopCalls == 0
              && harness.completion(205)
              && harness.completion(205)->status == 200,
          "candidate Stop is acknowledged without stopping the old owner");

    harness.handler.confirmationDecision(key, true, CastTime{102});
    check(harness.executions.size() == 2
              && harness.executions.back().playRequested,
          "accepted candidate starts media with attached Play intent");
    auto transitioningSeek = harness.handler.handle(request(
        206, "192.0.2.20", "Seek",
        "<InstanceID>0</InstanceID><Unit>REL_TIME</Unit>"
        "<Target>0:00:10</Target>"));
    check(transitioningSeek.status
              == mcast::dlna::net::HttpDispatchStatus::Completed
              && transitioningSeek.response.status == 500
              && transitioningSeek.response.body.find(
                  "<errorCode>701</errorCode>")
                  != std::string::npos,
          "new owner controls are rejected before media readiness");
    result = harness.handler.handle(stopRequest(207, "192.0.2.20"));
    check(result.status == mcast::dlna::net::HttpDispatchStatus::Pending
              && harness.stopCalls == 0
              && harness.completion(207)
              && harness.completion(207)->status == 500
              && harness.completion(207)->body.find(
                  "<errorCode>701</errorCode>")
                  != std::string::npos,
          "Stop cannot hit the old media while the new media opens");
    harness.handler.mediaReady(
        harness.executions.back().operationId, true, CastTime{103});
    check(harness.playCalls == 1
              && harness.completion(202)
              && harness.completion(202)->status == 200
              && harness.completion(203)
              && harness.completion(203)->status == 200,
          "candidate readiness completes SetURI and Play");
}

void testInvalidAndSynchronousRequests()
{
    Harness harness;
    auto result = harness.handler.handle(request(
        301, "192.0.2.10", "SetAVTransportURI",
        "<InstanceID>0</InstanceID><CurrentURI></CurrentURI>"
        "<CurrentURIMetaData></CurrentURIMetaData>"));
    check(result.status == mcast::dlna::net::HttpDispatchStatus::Completed
              && result.response.status == 500
              && result.response.body.find("<errorCode>402</errorCode>")
                  != std::string::npos
              && harness.executions.empty(),
          "invalid SetURI is rejected before arbitration");

    result = harness.handler.handle(request(
        302, "192.0.2.10", "GetTransportInfo",
        "<InstanceID>0</InstanceID>"));
    check(result.status == mcast::dlna::net::HttpDispatchStatus::Completed
              && result.response.status == 200,
          "query actions keep synchronous router behavior");
}

void testOversizedMetadataIsRejectedBeforeClaim()
{
    Harness harness;
    const std::string metadata(256 * 1024, 'm');
    auto oversized = request(
        431, "192.0.2.10", "SetAVTransportURI",
        "<InstanceID>0</InstanceID><CurrentURI>"
            + UpnpXml::escapeText(
                "http://example.test/large.mp4")
            + "</CurrentURI><CurrentURIMetaData>"
            + UpnpXml::escapeText(metadata)
            + "</CurrentURIMetaData>");
    const auto result = harness.handler.handle(oversized);
    check(result.status == mcast::dlna::net::HttpDispatchStatus::Completed
              && result.response.status == 500
              && result.response.body.find("<errorCode>402</errorCode>")
                  != std::string::npos
              && harness.executions.empty()
              && harness.confirmations.empty(),
          "oversized media metadata is rejected before claim");
}

void testOwnerTransportWithoutMediaReturns701()
{
    Harness harness;
    harness.openMediaOnExecute = false;
    auto initial = harness.handler.handle(setUriRequest(
        451, "192.0.2.10", "http://example.test/none.mp4"));
    check(initial.status == mcast::dlna::net::HttpDispatchStatus::Pending
              && harness.executions.size() == 1,
          "no-media owner test starts a claim");
    harness.handler.mediaReady(
        harness.executions[0].operationId, true, CastTime{101});

    const auto play = harness.handler.handle(
        playRequest(452, "192.0.2.10"));
    check(play.status == mcast::dlna::net::HttpDispatchStatus::Completed
              && play.response.status == 500
              && play.response.body.find("<errorCode>701</errorCode>")
                  != std::string::npos,
          "owner Play without transport media returns 701");

    const auto stop = harness.handler.handle(
        stopRequest(453, "192.0.2.10"));
    check(stop.status == mcast::dlna::net::HttpDispatchStatus::Completed
              && stop.response.status == 500
              && stop.response.body.find("<errorCode>701</errorCode>")
                  != std::string::npos,
          "owner Stop without transport media returns 701");
}

void testDifferentUserAgentIsNotOwner()
{
    Harness harness;
    auto initial = harness.handler.handle(setUriRequest(
        401, "192.0.2.10", "http://example.test/old.mp4"));
    check(initial.status == mcast::dlna::net::HttpDispatchStatus::Pending
              && harness.executions.size() == 1,
          "identity test owner starts");
    harness.handler.mediaReady(
        harness.executions[0].operationId, true, CastTime{101});

    auto other = setUriRequest(
        402, "192.0.2.10", "http://example.test/other.mp4");
    other.headers.front().second = "Other-Control";
    const auto result = harness.handler.handle(other);
    check(result.status == mcast::dlna::net::HttpDispatchStatus::Pending
              && harness.confirmations.size() == 1
              && harness.executions.size() == 1,
          "another User-Agent on the same IP does not inherit ownership");

    auto otherNext = request(
        404, "192.0.2.10", "SetNextAVTransportURI",
        "<InstanceID>0</InstanceID><NextURI>"
        "http://example.test/injected.mp4</NextURI>"
        "<NextURIMetaData></NextURIMetaData>");
    otherNext.headers.front().second = "Other-Control";
    const auto otherNextResult = harness.handler.handle(otherNext);
    check(otherNextResult.status
              == mcast::dlna::net::HttpDispatchStatus::Completed
              && otherNextResult.response.status == 500
              && otherNextResult.response.body.find(
                  "<errorCode>701</errorCode>")
                  != std::string::npos,
          "same-IP control identity cannot mutate the owner's queue");

    auto noAgent = request(
        403, "192.0.2.10", "Pause",
        "<InstanceID>0</InstanceID>");
    noAgent.headers.erase(
        std::remove_if(
            noAgent.headers.begin(), noAgent.headers.end(),
            [](const auto &header) {
                return header.first == "user-agent";
            }),
        noAgent.headers.end());
    const auto noAgentResult = harness.handler.handle(noAgent);
    check(noAgentResult.status
              == mcast::dlna::net::HttpDispatchStatus::Completed
              && noAgentResult.response.status == 200,
          "missing User-Agent on the owner address may operate the current "
          "session after ownership is already settled");
}

void testExecuteCallbackFailureFailsClosed()
{
    Harness harness;
    harness.throwExecute = true;
    const auto result = harness.handler.handle(setUriRequest(
        501, "192.0.2.10", "http://example.test/fail.mp4"));
    check(result.status == mcast::dlna::net::HttpDispatchStatus::Pending
              && harness.executions.empty()
              && harness.completion(501)
              && harness.completion(501)->status == 500
              && harness.completion(501)->body.find(
                  "<errorCode>501</errorCode>")
                  != std::string::npos,
          "execute callback exception maps to an internal SOAP fault");
}

} // namespace

int main()
{
    testFirstMediaWaitsForReadyAndOwnerControls();
    testCandidateConfirmationHoldsSetAndPlay();
    testInvalidAndSynchronousRequests();
    testOversizedMetadataIsRejectedBeforeClaim();
    testOwnerTransportWithoutMediaReturns701();
    testDifferentUserAgentIsNotOwner();
    testExecuteCallbackFailureFailsClosed();
    std::cout << "DLNA claim HTTP handler tests passed\n";
    return 0;
}
