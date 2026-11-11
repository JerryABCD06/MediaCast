// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "ipc/ProtocolControlDispatcher.h"
#include "skeleton/ProtocolDlnaConfirmationPort.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using namespace mcast::cast;
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
            L"confirmation-port-" + suffix + L"-"
            + std::to_wstring(GetCurrentProcessId())),
        1);
    check(pair.listener->valid(),
          "confirmation port pipe listener is valid");
    std::string error;
    std::unique_ptr<PipeConnection> connected;
    std::jthread connector([&] {
        connected = PipeClient::connect(
            pair.listener->name(), 3000ms, error);
    });
    pair.server = pair.listener->accept(3000ms, error);
    connector.join();
    check(pair.server && connected,
          "confirmation port pipe connects: " + error);
    pair.client = std::move(connected);
    return pair;
}

ConfirmationPrompt prompt()
{
    return {
        ClaimKey{ProtocolKind::Dlna, 77},
        PeerIdentity{"peer-77", "Phone", "192.0.2.20"},
        "Movie",
        CastTime{5100},
        CastTime{5000},
    };
}

void testDecisionMapping()
{
    PipePair pair = makePipePair(L"accept");
    std::jthread server([&] {
        std::string error;
        Message confirmation;
        check(pair.server->readMessage(confirmation, 3000ms, error),
              "fake Shell receives mapped confirmation: " + error);
        const auto decoded = decodeCastConfirmationRequestMessage(
            confirmation, error);
        check(decoded.has_value(),
              "mapped confirmation decodes: " + error);
        check(decoded->protocol == ProtocolKind::Dlna
                  && decoded->claimId == 77
                  && decoded->peerId == "peer-77"
                  && decoded->peerDisplayName == "Phone"
                  && decoded->mediaTitle == "Movie"
                  && decoded->remainingTimeoutMs == 5000,
              "neutral prompt maps to IPC fields");
        const CastConfirmationDecisionInfo decision{
            decoded->protocol, decoded->claimId, true};
        const auto response = makeCastConfirmationDecisionMessage(
            20, confirmation.header.messageId, decision, error);
        check(response.has_value(),
              "fake Shell builds decision: " + error);
        check(pair.server->writeMessage(*response, 2000ms, error),
              "fake Shell writes decision: " + error);
    });

    auto dispatcher = std::make_unique<ProtocolControlDispatcher>(
        std::move(pair.client));
    std::string error;
    check(dispatcher->start(error),
          "confirmation port dispatcher starts: " + error);
    mcast::skeleton::ProtocolDlnaConfirmationPort port(*dispatcher);
    bool accepted = false;
    check(port.waitForDecision(prompt(), accepted, error),
          "confirmation port completes mapped decision: " + error);
    check(accepted, "accepted decision is forwarded through the port");
    server.join();
    dispatcher->stop();
}

void testCancellationMapping()
{
    PipePair pair = makePipePair(L"cancel");
    std::mutex mutex;
    std::condition_variable ready;
    bool seen = false;
    std::jthread server([&] {
        std::string error;
        Message confirmation;
        check(pair.server->readMessage(confirmation, 3000ms, error),
              "cancel test receives mapped confirmation: " + error);
        {
            std::lock_guard lock(mutex);
            seen = true;
        }
        ready.notify_all();
        std::this_thread::sleep_for(250ms);
    });

    auto dispatcher = std::make_unique<ProtocolControlDispatcher>(
        std::move(pair.client));
    std::string error;
    check(dispatcher->start(error),
          "cancel port dispatcher starts: " + error);
    mcast::skeleton::ProtocolDlnaConfirmationPort port(*dispatcher);
    bool accepted = true;
    bool waitResult = true;
    std::string waitError;
    std::jthread waiter([&] {
        waitResult = port.waitForDecision(
            prompt(), accepted, waitError);
    });
    {
        std::unique_lock lock(mutex);
        check(ready.wait_for(lock, 2s, [&] { return seen; }),
              "mapped confirmation reaches fake Shell");
    }
    port.cancel();
    waiter.join();
    check(!waitResult && !accepted
              && waitError.find("cancelled") != std::string::npos,
          "port cancellation wakes and fails closed");
    server.join();
    dispatcher->stop();
}

} // namespace

int main()
{
    testDecisionMapping();
    testCancellationMapping();
    std::cout << "Protocol DLNA confirmation port tests passed\n";
    return 0;
}
