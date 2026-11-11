// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "playback/FakePlaybackGateway.h"
#include "protocols/dlna/UpnpXml.h"
#include "protocols/dlna/claim/DlnaClaimHttpHandler.h"
#include "protocols/dlna/runtime/DlnaClaimConfirmationWorker.h"
#include "protocols/dlna/session/DlnaSession.h"
#include "protocols/dlna/soap/SoapRouter.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace mcast::cast;
using namespace mcast::dlna;
using namespace mcast::dlna::claim;
using namespace mcast::dlna::runtime;
using namespace mcast::dlna::soap;
using namespace mcast::playback;
using namespace std::chrono_literals;

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

core::HttpRequest request(
    std::uint64_t requestId, std::string address, std::string action,
    std::string arguments = {})
{
    core::HttpRequest value;
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

core::HttpRequest setUriRequest(
    std::uint64_t requestId, const std::string &address,
    const std::string &uri)
{
    return request(
        requestId, address, "SetAVTransportURI",
        "<InstanceID>0</InstanceID><CurrentURI>"
            + UpnpXml::escapeText(uri)
            + "</CurrentURI><CurrentURIMetaData>"
            + UpnpXml::escapeText(
                "<DIDL-Lite><item><dc:title xmlns:dc=\"urn:test\">"
                "Test</dc:title></item></DIDL-Lite>")
            + "</CurrentURIMetaData>");
}

class FakeConfirmationPort final : public DlnaClaimConfirmationPort
{
public:
    bool waitForDecision(
        const ConfirmationPrompt &prompt,
        bool &accepted, std::string &error) override
    {
        std::unique_lock lock(m_mutex);
        m_prompt = prompt;
        m_ready.notify_all();
        m_decided.wait(lock, [this] {
            return m_cancelled || m_decision.has_value();
        });
        if (m_cancelled) {
            accepted = false;
            error = "cancelled";
            return false;
        }
        accepted = *m_decision;
        m_decision.reset();
        error.clear();
        return true;
    }

    void cancel() noexcept override
    {
        {
            std::lock_guard lock(m_mutex);
            m_cancelled = true;
        }
        m_decided.notify_all();
    }

    std::optional<ConfirmationPrompt> waitForPrompt()
    {
        std::unique_lock lock(m_mutex);
        if (!m_ready.wait_for(lock, 2s, [this] {
                return m_prompt.has_value();
            })) {
            return std::nullopt;
        }
        auto result = std::move(m_prompt);
        m_prompt.reset();
        return result;
    }

    void decide(bool accepted)
    {
        {
            std::lock_guard lock(m_mutex);
            m_decision = accepted;
        }
        m_decided.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::condition_variable m_decided;
    std::optional<ConfirmationPrompt> m_prompt;
    std::optional<bool> m_decision;
    bool m_cancelled = false;
};

struct Harness {
    FakePlaybackGateway playback;
    DlnaSession session{playback, {}};
    CastClaimCoordinator coordinator{ArbitrationPolicy{
        true, true, CastTime{10000}, CastTime{5000}, 8}};
    DlnaClaimAdapter adapter{coordinator};
    DlnaClaimRuntime runtime{adapter};
    SoapRouter router{session, {}};
    FakeConfirmationPort port;
    std::vector<std::pair<std::uint64_t, net::HttpResponse>> completions;
    std::vector<ConfirmationPrompt> confirmations;
    std::vector<DlnaMediaExecution> executions;
    mutable std::mutex stateMutex;
    std::atomic<long long> nowMs{100};
    DlnaClaimHttpHandler handler;
    std::unique_ptr<DlnaClaimConfirmationWorker> worker;

    Harness()
        : handler(
              router, runtime,
              [this] { return CastTime{nowMs.load()}; },
              DlnaClaimHttpCallbacks{
                  [this](
                      std::uint64_t id,
                      net::HttpResponse response) {
                      std::lock_guard lock(stateMutex);
                      completions.emplace_back(
                          id, std::move(response));
                      return true;
                  },
                  [this](const ConfirmationPrompt &prompt) {
                      std::lock_guard lock(stateMutex);
                      confirmations.push_back(prompt);
                      worker->requestConfirmation(prompt);
                  },
                  [this](const DlnaMediaExecution &execution) {
                      std::lock_guard lock(stateMutex);
                      executions.push_back(execution);
                  },
                  [] {},
                  [] {},
                  {},
                  {},
              })
    {
        worker = std::make_unique<DlnaClaimConfirmationWorker>(
            handler, port,
            [this] { return CastTime{nowMs.load()}; },
            std::chrono::milliseconds{10});
    }

    std::optional<net::HttpResponse> completion(
        std::uint64_t id) const
    {
        std::lock_guard lock(stateMutex);
        const auto found = std::find_if(
            completions.begin(), completions.end(),
            [id](const auto &item) { return item.first == id; });
        return found == completions.end()
            ? std::nullopt
            : std::optional<net::HttpResponse>{found->second};
    }

    std::optional<DlnaMediaExecution> execution(
        std::size_t index) const
    {
        std::lock_guard lock(stateMutex);
        if (index >= executions.size())
            return std::nullopt;
        return executions[index];
    }

    std::size_t executionCount() const
    {
        std::lock_guard lock(stateMutex);
        return executions.size();
    }

    std::size_t confirmationCount() const
    {
        std::lock_guard lock(stateMutex);
        return confirmations.size();
    }
};

bool eventually(
    const std::function<bool()> &condition,
    std::chrono::milliseconds timeout = 2s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition())
            return true;
        std::this_thread::sleep_for(5ms);
    }
    return condition();
}

void makeOwner(Harness &harness)
{
    auto result = harness.handler.handle(setUriRequest(
        101, "192.0.2.10", "http://example.test/owner.mp4"));
    check(result.status == net::HttpDispatchStatus::Pending
              && harness.executionCount() == 1,
          "owner SetURI starts media preparation");
    const auto ownerExecution = harness.execution(0);
    check(ownerExecution.has_value(), "owner execution is recorded");
    harness.handler.mediaReady(
        ownerExecution->operationId, true, CastTime{101});
    check(harness.completion(101).has_value()
              && harness.completion(101)->status == 200,
          "owner readiness completes its HTTP request");
}

void testAcceptedAndRejectedConfirmations()
{
    Harness harness;
    makeOwner(harness);

    auto result = harness.handler.handle(setUriRequest(
        201, "192.0.2.20", "http://example.test/candidate.mp4"));
    check(result.status == net::HttpDispatchStatus::Pending
              && harness.confirmationCount() == 1,
          "candidate SetURI enters confirmation");
    check(harness.port.waitForPrompt().has_value(),
          "candidate prompt reaches confirmation port");
    harness.port.decide(true);
    check(eventually([&] { return harness.executionCount() == 2; }),
          "accepted decision starts candidate media");
    const auto candidateExecution = harness.execution(1);
    check(candidateExecution.has_value(),
          "candidate execution is recorded");
    harness.handler.mediaReady(
        candidateExecution->operationId, true, CastTime{102});
    check(harness.completion(201).has_value()
              && harness.completion(201)->status == 200,
          "accepted candidate completes the held HTTP request");

    result = harness.handler.handle(setUriRequest(
        202, "192.0.2.30", "http://example.test/rejected.mp4"));
    check(result.status == net::HttpDispatchStatus::Pending,
          "second candidate enters confirmation");
    check(harness.port.waitForPrompt().has_value(),
          "second prompt reaches confirmation port");
    harness.port.decide(false);
    check(eventually([&] {
              return harness.completion(202).has_value();
          }),
          "rejected decision releases candidate HTTP");
    const auto rejected = harness.completion(202);
    check(rejected
              && rejected->status == 500
              && rejected->body.find(
                  "<errorCode>701</errorCode>")
                  != std::string::npos
              && harness.executionCount() == 2,
          "rejected candidate fails closed without opening media");

    result = harness.handler.handle(setUriRequest(
        203, "192.0.2.40", "http://example.test/lost.mp4"));
    check(result.status == net::HttpDispatchStatus::Pending
              && harness.port.waitForPrompt().has_value(),
          "control-loss candidate reaches confirmation");
    harness.worker->controlLost(CastTime{103});
    check(eventually([&] {
              return harness.completion(203).has_value();
          }),
          "control loss releases blocked confirmation HTTP");
}

void testWorkerDrivesRuntimeDeadline()
{
    Harness harness;
    auto result = harness.handler.handle(setUriRequest(
        301, "192.0.2.50", "http://example.test/stuck.mp4"));
    check(result.status == net::HttpDispatchStatus::Pending
              && harness.executionCount() == 1,
          "stuck Open starts without media readiness");
    harness.nowMs = 10101;
    check(eventually([&] {
              return harness.completion(301).has_value();
          }),
          "worker tick releases a stuck Open at the deadline");
    const auto timedOut = harness.completion(301);
    check(timedOut && timedOut->status == 500
              && timedOut->body.find("<errorCode>701</errorCode>")
                  != std::string::npos,
          "runtime deadline maps to a transition fault");
}

} // namespace

int main()
{
    testAcceptedAndRejectedConfirmations();
    testWorkerDrivesRuntimeDeadline();
    std::cout << "DLNA claim confirmation worker tests passed\n";
    return 0;
}
