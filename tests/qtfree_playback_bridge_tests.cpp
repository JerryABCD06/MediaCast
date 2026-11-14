// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "QtFree/EventLoop.h"
#include "QtFree/MediaPlayer.h"
#include "QtFree/PlaybackBridge.h"
#include "QtFree/PlaybackController.h"
#include "ipc/NamedPipe.h"
#include "ipc/PlaybackCodec.h"

#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

using namespace std::chrono_literals;
using mcast::avalonia::qtfree::EventLoop;
using mcast::avalonia::qtfree::MediaLoadRequest;
using mcast::avalonia::qtfree::MediaPlayer;
using mcast::avalonia::qtfree::PlaybackBridge;
using mcast::avalonia::qtfree::PlaybackController;

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

class FakePlayer final : public MediaPlayer
{
public:
    void load(const MediaLoadRequest &request) override
    {
        loadStarted.emit(request.loadGeneration);
        ready.emit(request.loadGeneration);
    }
    void play() override {}
    void pause() override {}
    void stop() override {}
    void unload() override {}
    void seekTo(double) override {}
    void setVolumePercent(int value) override
    {
        volume = value;
        volumeChanged.emit(value);
    }
    void setMuted(bool value) override { muted = value; }
    double positionSeconds() const override { return 4; }
    double durationSeconds() const override { return 60; }
    int volumePercent() const override { return volume; }
    bool isMuted() const override { return muted; }
    std::vector<mcast::avalonia::qtfree::PictureControlInfo>
    pictureControls() const override
    {
        return {{"brightness", -100, 100, 0}};
    }
    bool setPictureControl(
        const std::string &name, int) override
    {
        return name == "brightness";
    }
    int pictureControlValue(const std::string &name) const override
    {
        return name == "brightness" ? 0 : 0;
    }

    int volume = 80;
    bool muted = false;
};

std::unique_ptr<mcast::ipc::PipeListener> makeListener(
    std::wstring &name,
    std::unique_ptr<mcast::ipc::PipeConnection> &client)
{
    name = mcast::ipc::makePlaybackPipeName(
        L"qtfree-bridge-" + std::to_wstring(GetCurrentProcessId())
        + L"-" + std::to_wstring(GetTickCount64()));
    auto listener = std::make_unique<mcast::ipc::PipeListener>(name);
    check(listener->valid(), "playback listener starts");
    std::string error;
    client = mcast::ipc::PipeClient::connect(name, 2s, error);
    check(client != nullptr, error.c_str());
    return listener;
}

mcast::ipc::Message readUntil(
    mcast::ipc::PipeConnection &connection,
    mcast::ipc::MessageType type)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        mcast::ipc::Message message;
        std::string error;
        if (!connection.readMessage(message, 500ms, error))
            continue;
        if (message.header.type == type)
            return message;
    }
    throw std::runtime_error("expected IPC message was not received");
}

void expectNoMessageType(
    mcast::ipc::PipeConnection &connection,
    mcast::ipc::MessageType type,
    std::chrono::milliseconds duration)
{
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        mcast::ipc::Message message;
        std::string error;
        if (!connection.readMessage(message, 25ms, error))
            continue;
        check(
            message.header.type != type,
            "unexpected message during control adjustment");
    }
}

} // namespace

int main()
{
    EventLoop loop;
    check(loop.start(), "event loop starts");
    std::wstring pipeName;
    std::unique_ptr<mcast::ipc::PipeConnection> client;
    auto listener = makeListener(pipeName, client);
    std::string error;
    auto server = listener->accept(2s, error);
    check(server != nullptr, error.c_str());

    FakePlayer player;
    PlaybackController controller(&player, loop);
    PlaybackBridge bridge(
        std::shared_ptr<mcast::ipc::PipeConnection>(std::move(client)),
        controller, loop, 7);

    const auto reconnected = readUntil(
        *server, mcast::ipc::MessageType::PlaybackReconnected);
    mcast::playback::PlaybackSnapshot initial;
    check(mcast::playback::wire::decodePlaybackSnapshotMessage(
              reconnected, initial, error),
          error.c_str());
    check(initial.generation == 7
              && initial.state
                  == mcast::playback::PlaybackState::NoMedia,
          "initial snapshot uses bridge generation");

    mcast::playback::PlaybackCommand open;
    open.type = mcast::playback::PlaybackCommandType::Open;
    open.expectedGeneration = 7;
    open.media.uri = "file:///movie.mp4";
    open.media.title = "Movie";
    open.media.kind = mcast::playback::MediaKind::Video;
    const auto commandMessage =
        mcast::playback::wire::makePlaybackCommandMessage(
            501, open, error);
    check(commandMessage.has_value(), error.c_str());
    check(server->writeMessage(*commandMessage, 2s, error),
          error.c_str());

    const auto resultMessage = readUntil(
        *server, mcast::ipc::MessageType::PlaybackCommandResult);
    mcast::playback::wire::PlaybackCommandResultWire result;
    check(mcast::playback::wire::decodePlaybackCommandResultMessage(
              resultMessage, result, error),
          error.c_str());
    check(result.commandId == 501 && result.generation == 7
              && result.outcome
                  == mcast::playback::PlaybackOutcomeStatus::Applied,
          "open result is correlated");

    const auto stateMessage = readUntil(
        *server, mcast::ipc::MessageType::PlaybackStateDelta);
    mcast::playback::PlaybackSnapshot opened;
    check(mcast::playback::wire::decodePlaybackSnapshotMessage(
              stateMessage, opened, error),
          error.c_str());
    check(opened.state == mcast::playback::PlaybackState::Playing
              && opened.current.uri == "file:///movie.mp4",
          "state delta carries applied playback");

    auto stale = open;
    stale.expectedGeneration = 6;
    const auto staleMessage =
        mcast::playback::wire::makePlaybackCommandMessage(
            502, stale, error);
    check(staleMessage.has_value(), error.c_str());
    check(server->writeMessage(*staleMessage, 2s, error), error.c_str());
    const auto staleResultMessage = readUntil(
        *server, mcast::ipc::MessageType::PlaybackCommandResult);
    mcast::playback::wire::PlaybackCommandResultWire staleResult;
    check(mcast::playback::wire::decodePlaybackCommandResultMessage(
              staleResultMessage, staleResult, error),
          error.c_str());
    check(staleResult.commandId == 502
              && staleResult.outcome
                  == mcast::playback::PlaybackOutcomeStatus::
                      StaleGeneration,
          "stale generation is rejected");

    auto adjustmentReady = std::make_shared<std::promise<void>>();
    auto adjustmentFuture = adjustmentReady->get_future();
    check(loop.post([&bridge, &controller, adjustmentReady] {
              bridge.beginControlAdjustment();
              controller.setVolumePercent(35);
              controller.setVolumePercent(65);
              adjustmentReady->set_value();
          }),
          "adjustment starts on owner loop");
    check(adjustmentFuture.wait_for(1s) == std::future_status::ready,
          "adjustment start completes");
    expectNoMessageType(
        *server, mcast::ipc::MessageType::PlaybackStateDelta, 150ms);

    auto adjustmentEnd = std::make_shared<std::promise<void>>();
    auto adjustmentEndFuture = adjustmentEnd->get_future();
    check(loop.post([&bridge, adjustmentEnd] {
              bridge.endControlAdjustment();
              adjustmentEnd->set_value();
          }),
          "adjustment end is posted");
    check(
        adjustmentEndFuture.wait_for(1s) == std::future_status::ready,
        "adjustment end completes");
    const auto adjustedMessage = readUntil(
        *server, mcast::ipc::MessageType::PlaybackStateDelta);
    mcast::playback::PlaybackSnapshot adjusted;
    check(mcast::playback::wire::decodePlaybackSnapshotMessage(
              adjustedMessage, adjusted, error),
          error.c_str());
    check(adjusted.volumePercent == 65,
          "only final volume is published after adjustment");

    bridge.requestEndSession();
    const auto endCast = readUntil(
        *server, mcast::ipc::MessageType::UiEndCastRequest);
    check(endCast.header.source == mcast::ipc::ProcessRole::Ui
              && endCast.header.destination
                  == mcast::ipc::ProcessRole::Protocol,
          "end cast uses existing UI request");

    bridge.stop();
    bridge.stop();
    loop.stop();
    std::cout << "Qt-free playback bridge tests passed\n";
    return 0;
}
