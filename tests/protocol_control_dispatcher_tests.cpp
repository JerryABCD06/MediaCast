// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "ipc/ProtocolControlDispatcher.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace mcast::ipc;
using namespace std::chrono_literals;

namespace {

void check(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

struct PipePair {
    std::unique_ptr<PipeListener> listener;
    std::unique_ptr<PipeConnection> server;
    std::unique_ptr<PipeConnection> client;
};

PipePair makePipePair(const std::wstring &suffix)
{
    PipePair pair;
    pair.listener = std::make_unique<PipeListener>(
        makeControlPipeName(
            L"dispatcher-" + suffix + L"-"
            + std::to_wstring(GetCurrentProcessId())),
        1);
    check(pair.listener->valid(), "control dispatcher pipe listener is valid");

    std::string connectError;
    std::unique_ptr<PipeConnection> connected;
    std::jthread connector([&] {
        connected = PipeClient::connect(
            pair.listener->name(), 3000ms, connectError);
    });
    pair.server = pair.listener->accept(3000ms, connectError);
    connector.join();
    check(pair.server && connected,
          "control dispatcher pipe clients connect: " + connectError);
    pair.client = std::move(connected);
    return pair;
}

CastConfirmationRequestInfo confirmationRequest()
{
    return {
        mcast::cast::ProtocolKind::Dlna,
        77,
        "peer-77",
        "Phone",
        "192.0.2.20",
        "Movie",
        5000,
    };
}

void testActivationAndConfirmationRouting()
{
    PipePair pair = makePipePair(L"routes");
    std::atomic_bool serverDone{false};
    std::string serverError;
    std::jthread server([&] {
        std::string error;

        Message heartbeat;
        check(pair.server->readMessage(heartbeat, 3000ms, error),
              "fake Shell receives heartbeat: " + error);
        check(heartbeat.header.type == MessageType::Heartbeat
                  && heartbeat.header.messageId != 0,
              "heartbeat has a nonzero shared message ID");
        const std::uint64_t heartbeatId = heartbeat.header.messageId;

        Message receiving;
        check(pair.server->readMessage(receiving, 3000ms, error),
              "fake Shell receives unsolicited receiving state: " + error);
        check(receiving.header.type == MessageType::ReceivingState,
              "receiving update uses ReceivingState");
        check(receiving.header.stateVersion == 7,
              "receiving update carries its state version");
        const PayloadReader receivingReader(receiving.payload);
        const auto enabled = receivingReader.getBool(
            static_cast<std::uint16_t>(FieldId::ReceivingEnabled));
        check(receivingReader.valid() && enabled && !*enabled,
              "receiving update carries the effective false state");

        Message openRequest;
        openRequest.header.type = MessageType::UiOpenRequest;
        openRequest.header.source = ProcessRole::Shell;
        openRequest.header.destination = ProcessRole::Protocol;
        openRequest.header.messageId = 10;
        openRequest.header.sessionId = 1;
        check(pair.server->writeMessage(openRequest, 2000ms, error),
              "fake Shell writes UiOpenRequest: " + error);

        Message activation;
        check(pair.server->readMessage(activation, 3000ms, error),
              "fake Shell receives UI activation: " + error);
        check(activation.header.type == MessageType::UiActivateRequest
                  && activation.header.messageId != heartbeatId,
              "activation uses a unique message ID");
        const std::uint64_t activationRequestId =
            activation.header.messageId;

        const UiActivateDecisionInfo decision{true, 900, true};
        auto decisionMessage = makeUiActivateDecisionMessage(
            11, activationRequestId, 1, decision, error);
        check(decisionMessage.has_value(),
              "fake Shell builds activation decision: " + error);
        check(pair.server->writeMessage(
                  *decisionMessage, 2000ms, error),
              "fake Shell writes activation decision: " + error);

        const PeerIdentityGrant grant{
            ProcessRole::Ui,
            123,
            "build",
            "nonce",
            std::string(64, 'a'),
            900,
        };
        auto grantMessage = makePeerIdentityGrantMessage(
            ProcessRole::Protocol, 12, 900, grant, error);
        check(grantMessage.has_value(),
              "fake Shell builds peer grant: " + error);
        check(pair.server->writeMessage(*grantMessage, 2000ms, error),
              "fake Shell writes peer grant: " + error);
        const UiActivateDecisionInfo conflicting{false, 901};
        auto conflictingDecision = makeUiActivateDecisionMessage(
            14, activationRequestId, 1, conflicting, error);
        check(conflictingDecision.has_value(),
              "fake Shell builds duplicate activation decision: " + error);
        check(pair.server->writeMessage(
                  *conflictingDecision, 2000ms, error),
              "fake Shell writes duplicate activation decision: " + error);

        Message confirmation;
        check(pair.server->readMessage(confirmation, 3000ms, error),
              "fake Shell receives confirmation request: " + error);
        const auto decoded = decodeCastConfirmationRequestMessage(
            confirmation, error);
        check(decoded.has_value(),
              "fake Shell decodes confirmation request: " + error);
        check(confirmation.header.messageId != heartbeatId
                  && confirmation.header.messageId
                      != activationRequestId,
              "confirmation uses a unique message ID");

        const CastConfirmationDecisionInfo accepted{
            decoded->protocol, decoded->claimId, true};
        auto confirmationDecision = makeCastConfirmationDecisionMessage(
            13, confirmation.header.messageId, accepted, error);
        check(confirmationDecision.has_value(),
              "fake Shell builds confirmation decision: " + error);
        check(pair.server->writeMessage(
                  *confirmationDecision, 2000ms, error),
              "fake Shell writes confirmation decision: " + error);
        const CastConfirmationDecisionInfo conflictingAcceptance{
            decoded->protocol, decoded->claimId, false};
        auto duplicateDecision = makeCastConfirmationDecisionMessage(
            15, confirmation.header.messageId,
            conflictingAcceptance, error);
        check(duplicateDecision.has_value(),
              "fake Shell builds duplicate confirmation decision: " + error);
        check(pair.server->writeMessage(
                  *duplicateDecision, 2000ms, error),
              "fake Shell writes duplicate confirmation decision: " + error);
        serverDone = true;
    });

    auto dispatcher = std::make_unique<ProtocolControlDispatcher>(
        std::move(pair.client));
    std::string error;
    check(dispatcher->start(error),
          "control dispatcher starts: " + error);
    check(dispatcher->sendHeartbeat(error),
          "control dispatcher sends heartbeat: " + error);
    check(dispatcher->sendReceivingState(false, 7, error),
          "control dispatcher sends unsolicited receiving state: "
              + error);
    check(dispatcher->waitForUiOpenRequest(2s, error),
          "dispatcher receives UiOpenRequest while other waits are possible: "
              + error);

    UiActivationOutcome activation;
    check(dispatcher->requestUiActivation(activation, 3s, error),
          "dispatcher completes UI activation: " + error);
    check(activation.started && activation.grant
              && activation.uiPresent
              && activation.grant->processId == 123
              && activation.grant->activationId == 900,
          "dispatcher routes activation decision and uncorrelated grant");

    bool accepted = false;
    check(dispatcher->requestCastConfirmation(
              confirmationRequest(), 3s, accepted, error),
          "dispatcher completes cast confirmation: " + error);
    check(accepted, "dispatcher routes confirmation decision");

    server.join();
    check(serverDone.load(),
          "fake Shell completed dispatcher scenario");
    dispatcher->stop();
}

void testActivationRejectedWithoutGrant()
{
    PipePair pair = makePipePair(L"activation-rejected");
    std::jthread server([&] {
        std::string error;
        Message activation;
        check(pair.server->readMessage(activation, 3000ms, error),
              "rejected activation request arrives: " + error);
        const UiActivateDecisionInfo decision{false, 910};
        auto message = makeUiActivateDecisionMessage(
            20, activation.header.messageId, 1, decision, error);
        check(message.has_value(),
              "rejected activation decision builds: " + error);
        check(pair.server->writeMessage(*message, 2000ms, error),
              "rejected activation decision writes: " + error);
    });

    auto dispatcher = std::make_unique<ProtocolControlDispatcher>(
        std::move(pair.client));
    std::string error;
    check(dispatcher->start(error),
          "rejected activation dispatcher starts: " + error);
    UiActivationOutcome outcome;
    check(dispatcher->requestUiActivation(outcome, 2s, error),
          "rejected activation completes without a grant: " + error);
    check(!outcome.started && !outcome.grant,
          "started=false activation does not wait for PeerIdentityGrant");
    server.join();
    dispatcher->stop();
}

void testUiPresenceProbe()
{
    PipePair pair = makePipePair(L"presence-probe");
    std::jthread server([&] {
        std::string error;
        Message request;
        check(pair.server->readMessage(request, 3000ms, error),
              "fake Shell receives UI presence probe: " + error);
        PayloadReader reader(request.payload);
        const auto probe = reader.getBool(
            static_cast<std::uint16_t>(FieldId::UiActivationProbeOnly));
        check(reader.valid() && probe && *probe,
              "UI presence request carries probeOnly=true");
        const UiActivateDecisionInfo presence{false, 1, true};
        const auto reply = makeUiActivateDecisionMessage(
            25, request.header.messageId, request.header.sessionId,
            presence, error);
        check(reply.has_value(),
              "fake Shell builds presence decision: " + error);
        check(pair.server->writeMessage(*reply, 2000ms, error),
              "fake Shell writes presence decision: " + error);
    });

    auto dispatcher = std::make_unique<ProtocolControlDispatcher>(
        std::move(pair.client));
    std::string error;
    check(dispatcher->start(error),
          "presence probe dispatcher starts: " + error);
    UiActivationOutcome outcome;
    check(dispatcher->requestUiActivation(
              outcome, 2s, error, true),
          "presence probe completes: " + error);
    check(!outcome.started && outcome.uiPresent && !outcome.grant,
          "presence probe reports an existing UI without a grant");
    server.join();
    dispatcher->stop();
}

void testTransportFailureWakesWaiters()
{
    PipePair pair = makePipePair(L"failure");
    std::jthread server([&] {
        std::string error;
        Message confirmation;
        static_cast<void>(pair.server->readMessage(
            confirmation, 3000ms, error));
        pair.server->close();
    });

    auto dispatcher = std::make_unique<ProtocolControlDispatcher>(
        std::move(pair.client));
    std::string error;
    check(dispatcher->start(error),
          "failure dispatcher starts: " + error);
    bool accepted = true;
    check(!dispatcher->requestCastConfirmation(
              confirmationRequest(), 3s, accepted, error),
          "control loss fails a pending confirmation");
    check(!accepted && !dispatcher->connected(),
          "control loss is fail-closed and marks the dispatcher closed");
    check(!dispatcher->start(error),
          "terminated dispatcher cannot be restarted");
    check(dispatcher->waitForTermination(100ms, error)
              && error.find("read failed") != std::string::npos,
          "termination reason is observable without reader callbacks");
    server.join();
    dispatcher->stop();
}

void testConfirmationTimeout()
{
    PipePair pair = makePipePair(L"timeout");
    std::jthread server([&] {
        std::string error;
        Message confirmation;
        static_cast<void>(pair.server->readMessage(
            confirmation, 3000ms, error));
        std::this_thread::sleep_for(250ms);
    });

    auto dispatcher = std::make_unique<ProtocolControlDispatcher>(
        std::move(pair.client));
    std::string error;
    check(dispatcher->start(error),
          "timeout dispatcher starts: " + error);
    bool accepted = true;
    check(!dispatcher->requestCastConfirmation(
              confirmationRequest(), 100ms, accepted, error)
              && error.find("timed out") != std::string::npos,
          "confirmation timeout is reported distinctly");
    check(dispatcher->connected(),
          "confirmation timeout does not kill the ControlPipe");
    server.join();
    dispatcher->stop();
}

void testConfirmationCancellation()
{
    PipePair pair = makePipePair(L"cancel");
    std::mutex readyMutex;
    std::condition_variable ready;
    bool requestSeen = false;
    std::jthread server([&] {
        std::string error;
        Message confirmation;
        check(pair.server->readMessage(confirmation, 3000ms, error),
              "cancel test receives confirmation request: " + error);
        {
            std::lock_guard lock(readyMutex);
            requestSeen = true;
        }
        ready.notify_all();
        Message cancel;
        check(pair.server->readMessage(cancel, 3000ms, error),
              "cancel test receives confirmation cancel: " + error);
        const auto cancelled =
            decodeCastConfirmationCancelMessage(cancel, error);
        check(cancelled.has_value()
                  && *cancelled == confirmation.header.messageId,
              "confirmation cancel correlates to the request: " + error);
    });

    auto dispatcher = std::make_unique<ProtocolControlDispatcher>(
        std::move(pair.client));
    std::string error;
    check(dispatcher->start(error),
          "cancel dispatcher starts: " + error);

    bool requestOk = true;
    bool accepted = true;
    std::string requestError;
    std::jthread requester([&] {
        requestOk = dispatcher->requestCastConfirmation(
            confirmationRequest(), 5s, accepted, requestError);
    });
    {
        std::unique_lock lock(readyMutex);
        check(ready.wait_for(lock, 2s, [&] { return requestSeen; }),
              "confirmation request reaches fake Shell");
    }
    check(dispatcher->cancelCastConfirmation(),
          "pending confirmation can be cancelled");
    requester.join();
    check(!requestOk && !accepted
              && requestError.find("cancelled") != std::string::npos,
          "cancellation wakes the waiter as fail-closed");
    check(dispatcher->connected(),
          "confirmation cancellation keeps ControlPipe connected");
    server.join();
    dispatcher->stop();
}

void testPlaybackReconnectRequest()
{
    PipePair pair = makePipePair(L"playback-reconnect");
    std::jthread server([&] {
        std::string error;
        Message request;
        check(pair.server->readMessage(request, 3000ms, error),
              "fake Shell receives playback reconnect: " + error);
        const auto requestId =
            decodePlaybackReconnectRequestMessage(request, error);
        check(requestId.has_value()
                  && *requestId == request.header.messageId,
              "playback reconnect message decodes: " + error);
        const auto uiRequest = makeUiPlaybackReconnectMessage(
            50, *requestId, error);
        check(uiRequest.has_value()
                  && decodeUiPlaybackReconnectMessage(
                      *uiRequest, error)
                      == requestId,
              "Shell can forward playback reconnect to Ui: " + error);
    });

    auto dispatcher = std::make_unique<ProtocolControlDispatcher>(
        std::move(pair.client));
    std::string error;
    check(dispatcher->start(error),
          "playback reconnect dispatcher starts: " + error);
    check(dispatcher->requestPlaybackReconnect(error),
          "Protocol sends playback reconnect request: " + error);
    server.join();
    dispatcher->stop();
}

} // namespace

int main()
{
    testActivationAndConfirmationRouting();
    testActivationRejectedWithoutGrant();
    testUiPresenceProbe();
    testTransportFailureWakesWaiters();
    testConfirmationTimeout();
    testConfirmationCancellation();
    testPlaybackReconnectRequest();
    std::cout << "Protocol control dispatcher tests passed\n";
    return 0;
}
