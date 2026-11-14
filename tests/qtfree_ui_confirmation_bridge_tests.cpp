// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "QtFree/EventLoop.h"
#include "QtFree/UiConfirmationBridge.h"
#include "ipc/Messages.h"
#include "ipc/NamedPipe.h"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>

namespace {

using namespace std::chrono_literals;
using mcast::avalonia::qtfree::ConfirmationEvent;
using mcast::avalonia::qtfree::EventLoop;
using mcast::avalonia::qtfree::SettingsSnapshotEvent;
using mcast::avalonia::qtfree::UiConfirmationBridge;

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

struct PipePair
{
    std::unique_ptr<mcast::ipc::PipeConnection> server;
    std::unique_ptr<mcast::ipc::PipeConnection> client;
};

PipePair connectPipe(const std::wstring &name)
{
    mcast::ipc::PipeListener listener(name);
    check(listener.valid(), "test pipe listener starts");
    std::string error;
    auto client = mcast::ipc::PipeClient::connect(name, 2s, error);
    check(client != nullptr, error.c_str());
    auto server = listener.accept(2s, error);
    check(server != nullptr, error.c_str());
    return {std::move(server), std::move(client)};
}

} // namespace

int main()
{
    const std::wstring suffix =
        std::to_wstring(GetCurrentProcessId()) + L"-"
        + std::to_wstring(GetTickCount64());
    const std::wstring pipeName = mcast::ipc::makeControlPipeName(
        L"qtfree-confirmation-" + suffix);
    PipePair pipes = connectPipe(pipeName);

    EventLoop loop;
    check(loop.start(), "event loop starts");
    UiConfirmationBridge bridge(
        std::shared_ptr<mcast::ipc::PipeConnection>(
            std::move(pipes.client)),
        loop, [](const std::string &title) {
            return "resolved:" + title;
        });

    std::mutex mutex;
    std::condition_variable condition;
    bool requestSeen = false;
    bool cancelSeen = false;
    bool snapshotSeen = false;
    ConfirmationEvent request;
    SettingsSnapshotEvent snapshot;
    auto requestConnection = bridge.requestReceived.connect(
        [&](const ConfirmationEvent &event) {
            const std::scoped_lock lock(mutex);
            request = event;
            requestSeen = true;
            condition.notify_all();
        });
    auto cancelConnection = bridge.cancelReceived.connect(
        [&](std::uint64_t id) {
            const std::scoped_lock lock(mutex);
            cancelSeen = id == 110;
            condition.notify_all();
        });
    auto snapshotConnection = bridge.settingsReceived.connect(
        [&](const SettingsSnapshotEvent &event) {
            const std::scoped_lock lock(mutex);
            snapshot = event;
            snapshotSeen = true;
            condition.notify_all();
        });

    std::string error;
    const mcast::ipc::CastConfirmationRequestInfo info{
        mcast::cast::ProtocolKind::Dlna,
        77,
        "peer|id",
        "Peer",
        "127.0.0.1",
        "Movie",
        9000,
    };
    const auto requestMessage =
        mcast::ipc::makeUiCastConfirmationRequestMessage(
            100, 1000, info, error);
    check(requestMessage.has_value(), error.c_str());
    check(pipes.server->writeMessage(
              *requestMessage, 2s, error),
          error.c_str());
    {
        std::unique_lock lock(mutex);
        check(condition.wait_for(
                  lock, 2s, [&] { return requestSeen; }),
              "request reaches backend loop");
    }
    check(request.requestId == 100 && request.request.claimId == 77
              && request.resolvedMediaTitle == "resolved:Movie",
          "request projection is complete");

    check(bridge.submitDecision(100, true),
          "decision submission succeeds");
    mcast::ipc::Message decisionMessage;
    check(pipes.server->readMessage(
              decisionMessage, 2s, error),
          error.c_str());
    const auto decision =
        mcast::ipc::decodeUiCastConfirmationDecisionMessage(
            decisionMessage, 100, error);
    check(decision.has_value() && decision->accepted
              && decision->claimId == 77,
          "decision wire payload is unchanged");
    check(!bridge.submitDecision(100, false),
          "duplicate decision is rejected locally");

    const auto secondRequest =
        mcast::ipc::makeUiCastConfirmationRequestMessage(
            110, 1001, info, error);
    check(secondRequest.has_value(), error.c_str());
    check(pipes.server->writeMessage(
              *secondRequest, 2s, error),
          error.c_str());
    const auto cancel =
        mcast::ipc::makeUiCastConfirmationCancelMessage(
            111, 110, error);
    check(cancel.has_value(), error.c_str());
    check(pipes.server->writeMessage(*cancel, 2s, error),
          error.c_str());
    {
        std::unique_lock lock(mutex);
        check(condition.wait_for(
                  lock, 2s, [&] { return cancelSeen; }),
              "cancel reaches backend loop");
    }
    check(!bridge.submitDecision(110, true),
          "cancelled request cannot be decided");

    const std::string settings = R"({"ui":{"mica":true}})";
    const auto settingsMessage =
        mcast::ipc::makeSettingsSnapshotMessage(
            120, 9, settings, "en_US", error);
    check(settingsMessage.has_value(), error.c_str());
    check(pipes.server->writeMessage(
              *settingsMessage, 2s, error),
          error.c_str());
    {
        std::unique_lock lock(mutex);
        check(condition.wait_for(
                  lock, 2s, [&] { return snapshotSeen; }),
              "settings snapshot reaches backend loop");
    }
    check(snapshot.stateVersion == 9
              && snapshot.resolvedLanguage == "en_US"
              && snapshot.json == settings,
          "settings snapshot projection is complete");

    check(bridge.submitSettings("{}", 9, error),
          "settings submission succeeds");
    mcast::ipc::Message settingsUpdate;
    check(pipes.server->readMessage(
              settingsUpdate, 2s, error),
          error.c_str());
    check(settingsUpdate.header.type
                  == mcast::ipc::MessageType::UiUpdateSettings
              && settingsUpdate.header.stateVersion == 9,
          "settings update preserves base version");

    requestConnection.disconnect();
    cancelConnection.disconnect();
    snapshotConnection.disconnect();
    bridge.stop();
    bridge.stop();

    PipePair lostPipes = connectPipe(
        mcast::ipc::makeControlPipeName(
            L"qtfree-confirmation-lost-" + suffix));
    UiConfirmationBridge lostBridge(
        std::shared_ptr<mcast::ipc::PipeConnection>(
            std::move(lostPipes.client)),
        loop);
    bool connectionLost = false;
    auto lostConnection = lostBridge.connectionLost.connect(
        [&](const std::string &reason) {
            const std::scoped_lock lock(mutex);
            connectionLost = !reason.empty();
            condition.notify_all();
        });
    lostPipes.server->close();
    {
        std::unique_lock lock(mutex);
        check(condition.wait_for(
                  lock, 2s, [&] { return connectionLost; }),
              "ControlPipe disconnect fails closed");
    }
    lostConnection.disconnect();
    lostBridge.stop();

    loop.stop();
    std::cout << "Qt-free UI confirmation bridge tests passed\n";
    return 0;
}
