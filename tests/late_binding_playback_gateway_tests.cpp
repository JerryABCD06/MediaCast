// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "playback/FakePlaybackGateway.h"
#include "playback/LateBindingPlaybackGateway.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace mcast::playback;

namespace {

// Kept as a separate target so gateway attachment stays Ui/Qt-free.

void check(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

PlaybackCommand command(PlaybackCommandType type)
{
    PlaybackCommand value;
    value.type = type;
    return value;
}

void testDetachedAndAttachedLifecycle()
{
    LateBindingPlaybackGateway gateway;
    std::vector<PlaybackEvent> events;
    const auto listener = gateway.addEventListener(
        [&](const PlaybackEvent &event) { events.push_back(event); });
    check(listener != 0, "late-binding listener registers");

    check(!gateway.attached()
              && !gateway.snapshot().playerAvailable,
          "late-binding gateway starts without Ui");
    auto submission = gateway.submit(command(PlaybackCommandType::Play));
    check(!submission.queued
              && submission.status
                  == PlaybackSubmissionStatus::PlayerUnavailable,
          "commands fail closed while Ui is absent");

    auto playback = std::make_shared<FakePlaybackGateway>();
    std::string error;
    check(gateway.attach(playback, error),
          "late-binding gateway attaches Ui: " + error);
    check(gateway.attached() && gateway.snapshot().playerAvailable
              && !events.empty()
              && events.back().kind == PlaybackEventKind::Reconnected,
          "attach exposes Ui and publishes reconnected");

    PlaybackCommand open = command(PlaybackCommandType::Open);
    open.media.uri = "http://example.test/video.mp4";
    submission = gateway.submit(open);
    check(submission.queued,
          "attached gateway forwards playback command");
    playback->processAll();
    check(gateway.snapshot().current.uri == open.media.uri
              && events.size() >= 3
              && events.back().kind
                  == PlaybackEventKind::SnapshotChanged,
          "Ui command completion and snapshot are forwarded");

    gateway.detach("Ui closed");
    check(!gateway.attached()
              && !gateway.snapshot().playerAvailable
              && events.back().kind == PlaybackEventKind::PlayerLost
              && events.back().error == "Ui closed",
          "detach publishes player loss");
    submission = gateway.submit(command(PlaybackCommandType::Pause));
    check(!submission.queued
              && submission.status
                  == PlaybackSubmissionStatus::PlayerUnavailable,
          "detached gateway rejects commands again");

    gateway.removeEventListener(listener);
}

void testReattachAndListenerRemoval()
{
    LateBindingPlaybackGateway gateway;
    std::vector<PlaybackEventKind> events;
    const auto listener = gateway.addEventListener(
        [&](const PlaybackEvent &event) { events.push_back(event.kind); });

    auto first = std::make_shared<FakePlaybackGateway>();
    std::string error;
    check(gateway.attach(first, error),
          "first playback gateway attaches: " + error);
    gateway.detach("round one");
    auto second = std::make_shared<FakePlaybackGateway>();
    check(gateway.attach(second, error),
          "second playback gateway attaches: " + error);
    check(events.size() == 3
              && events[0] == PlaybackEventKind::Reconnected
              && events[1] == PlaybackEventKind::PlayerLost
              && events[2] == PlaybackEventKind::Reconnected,
          "listener survives detach and second attach");

    gateway.removeEventListener(listener);
    const std::size_t before = events.size();
    second->markReady();
    check(events.size() == before,
          "removed listener receives no further state");
}

} // namespace

int main()
{
    testDetachedAndAttachedLifecycle();
    testReattachAndListenerRemoval();
    std::cout << "Late-binding playback gateway tests passed\n";
    return 0;
}
