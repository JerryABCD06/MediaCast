// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/playback/FakePlaybackGateway.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace mcast::dlna;

namespace {

void check(bool condition, const char *message)
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

PlaybackCommandSubmission submit(
    FakePlaybackGateway &gateway, PlaybackCommand value)
{
    const auto submission = gateway.submit(value);
    check(submission.queued, "command is queued");
    check(submission.status == PlaybackSubmissionStatus::Queued,
          "submission status is queued");
    check(submission.commandId != 0, "command has an id");
    check(gateway.processNext(), "command is processed");
    return submission;
}

const PlaybackEvent &lastCommandEvent(
    const std::vector<PlaybackEvent> &events)
{
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        if (it->kind == PlaybackEventKind::CommandCompleted)
            return *it;
    }
    check(false, "command event exists");
    std::abort();
}

} // namespace

int main()
{
    FakePlaybackGateway gateway;
    std::vector<PlaybackEvent> events;
    const std::uint64_t listener = gateway.addEventListener(
        [&](const PlaybackEvent &event) {
            events.push_back(event);
        });
    check(listener != 0, "listener registered");

    PlaybackSnapshot snapshot = gateway.snapshot();
    check(snapshot.state == PlaybackState::NoMedia, "initial state");
    submit(gateway, command(PlaybackCommandType::Play));
    check(lastCommandEvent(events).outcome == PlaybackOutcomeStatus::Rejected,
          "play without media is rejected by player");

    MediaRequest first;
    first.uri = "http://example.test/video.mp4";
    first.metadata = "<DIDL-Lite/>";
    first.title = "First";
    first.kind = MediaKind::Video;
    first.mediaSizeBytes = 1024;
    PlaybackCommand open = command(PlaybackCommandType::Open);
    open.media = first;
    const auto opened = submit(gateway, open);
    check(!events.empty(), "open emits events");
    check(
        events.back().snapshot.state == PlaybackState::Preparing,
        "open leaves fake preparing");
    check(
        events.back().snapshot.current.metadata == first.metadata,
        "raw metadata is preserved");
    check(
        events.back().snapshot.current.mediaSizeBytes == first.mediaSizeBytes,
        "media size is preserved");
    check(opened.generation == events.back().snapshot.generation,
          "submission carries generation");

    gateway.markReady();
    check(gateway.snapshot().state == PlaybackState::Playing,
          "ready starts playback");
    submit(gateway, command(PlaybackCommandType::Pause));
    check(gateway.snapshot().state == PlaybackState::Paused,
          "pause result applied");
    submit(gateway, command(PlaybackCommandType::Play));
    PlaybackCommand seek = command(PlaybackCommandType::Seek);
    seek.doubleValue = 30.0;
    submit(gateway, seek);
    check(gateway.snapshot().positionSeconds == 30.0,
          "seek result applied");

    MediaRequest second;
    second.uri = "http://example.test/next.mp4";
    PlaybackCommand setNext = command(PlaybackCommandType::SetNext);
    setNext.media = second;
    submit(gateway, setNext);
    check(gateway.snapshot().hasNext, "next becomes available");
    submit(gateway, command(PlaybackCommandType::Next));
    snapshot = gateway.snapshot();
    check(snapshot.current.uri == second.uri, "next becomes current");
    check(!snapshot.hasNext && snapshot.hasPrevious,
          "queue position updates");
    submit(gateway, command(PlaybackCommandType::Previous));
    snapshot = gateway.snapshot();
    check(snapshot.current.uri == first.uri, "previous becomes current");
    check(snapshot.hasNext && !snapshot.hasPrevious,
          "previous restores next item");

    MediaRequest third;
    third.uri = "http://example.test/third.mp4";
    PlaybackCommand openThird = command(PlaybackCommandType::Open);
    openThird.media = third;
    submit(gateway, openThird);
    snapshot = gateway.snapshot();
    check(snapshot.current.uri == third.uri, "third item is current");
    check(snapshot.hasNext && !snapshot.hasPrevious,
          "open preserves existing playlist");

    PlaybackCommand repeat = command(PlaybackCommandType::SetPlayMode);
    repeat.playMode = PlayMode::RepeatAll;
    submit(gateway, repeat);
    check(gateway.snapshot().playMode == PlayMode::RepeatAll,
          "play mode updated");
    PlaybackCommand badVolume = command(PlaybackCommandType::SetVolume);
    badVolume.intValue = 140;
    const auto badVolumeSubmission = gateway.submit(badVolume);
    check(!badVolumeSubmission.queued
              && badVolumeSubmission.status
                  == PlaybackSubmissionStatus::InvalidCommand,
          "out-of-range volume has typed invalid-command rejection");
    PlaybackCommand volume = badVolume;
    volume.intValue = 55;
    submit(gateway, volume);
    check(gateway.snapshot().volumePercent == 55, "volume updated");
    PlaybackCommand mute = command(PlaybackCommandType::SetMuted);
    mute.boolValue = true;
    submit(gateway, mute);
    check(gateway.snapshot().muted, "mute updated");

    gateway.setPictureControls({
        {"brightness", 0, 100, 50, 50},
        {"contrast", 0, 100, 50, 50},
    });
    PlaybackCommand picture =
        command(PlaybackCommandType::SetPictureControl);
    picture.text = "brightness";
    picture.intValue = 75;
    submit(gateway, picture);
    check(gateway.snapshot().pictureControls[0].value == 75,
          "picture control updated");
    submit(gateway, command(PlaybackCommandType::ResetPictureControls));
    check(gateway.snapshot().pictureControls[0].value == 50,
          "picture control neutral restored");

    PlaybackCommand disconnect =
        command(PlaybackCommandType::SetPeerConnected);
    disconnect.boolValue = true;
    submit(gateway, disconnect);
    check(gateway.snapshot().peerConnected, "peer connected");
    submit(gateway, setNext);
    check(gateway.snapshot().hasNext, "queue refilled");
    PlaybackCommand reconnect =
        command(PlaybackCommandType::SetPeerConnected);
    reconnect.boolValue = true;
    submit(gateway, reconnect);
    check(gateway.snapshot().hasNext,
          "same peer state does not clear queue");
    reconnect.boolValue = false;
    submit(gateway, reconnect);
    check(!gateway.snapshot().peerConnected && !gateway.snapshot().hasNext,
          "peer transition clears queue");

    const std::uint64_t generation = gateway.snapshot().generation;
    PlaybackCommand stale = command(PlaybackCommandType::Play);
    const auto staleSubmission = gateway.submit(stale);
    check(staleSubmission.queued && staleSubmission.generation == generation,
          "default generation is bound at submission");
    gateway.simulateReconnect();
    check(gateway.snapshot().generation == generation + 1,
          "reconnect advances generation");
    check(gateway.processNext(), "stale command is drained");
    check(lastCommandEvent(events).outcome
              == PlaybackOutcomeStatus::StaleGeneration,
          "stale generation completion reported");

    const auto queuedBeforeLoss =
        gateway.submit(command(PlaybackCommandType::Play));
    check(queuedBeforeLoss.queued,
          "command can queue while player is available");
    const std::size_t beforePlayerLost = events.size();
    gateway.simulatePlayerLost();
    check(events.size() >= beforePlayerLost + 2
              && events[beforePlayerLost].kind
                  == PlaybackEventKind::PlayerLost,
          "player lost event");
    check(events[beforePlayerLost + 1].kind
              == PlaybackEventKind::SnapshotChanged,
          "player lost snapshot follows event");
    check(gateway.snapshot().state == PlaybackState::NoMedia,
          "player lost ends the session");
    check(gateway.snapshot().current.uri.empty(),
          "player lost clears current media");
    check(gateway.processNext(), "queued command is drained after loss");
    check(lastCommandEvent(events).outcome
              == PlaybackOutcomeStatus::Rejected
              && lastCommandEvent(events).error
                  == "player became unavailable",
          "queued command is revalidated before application");
    const auto unavailable =
        gateway.submit(command(PlaybackCommandType::Play));
    check(!unavailable.queued
              && unavailable.status
                  == PlaybackSubmissionStatus::PlayerUnavailable,
          "unavailable player has typed submission rejection");
    gateway.simulateReconnect();
    check(events.back().kind == PlaybackEventKind::Reconnected,
          "reconnected event");

    const std::uint64_t oldGeneration = gateway.snapshot().generation;
    const std::uint64_t oldStateVersion = gateway.snapshot().stateVersion;
    submit(gateway, command(PlaybackCommandType::EndSession));
    check(gateway.snapshot().generation == oldGeneration,
          "end session keeps connection generation");
    check(gateway.snapshot().stateVersion == oldStateVersion + 1,
          "end session advances state version");

    const std::uint64_t currentGeneration = gateway.snapshot().generation;
    PlaybackCommand oldGenerationCommand =
        command(PlaybackCommandType::Play);
    oldGenerationCommand.expectedGeneration = currentGeneration - 1;
    const auto oldGenerationSubmission =
        gateway.submit(oldGenerationCommand);
    check(!oldGenerationSubmission.queued
              && oldGenerationSubmission.status
                  == PlaybackSubmissionStatus::StaleGeneration,
          "stale generation has typed submission rejection");

    gateway.removeEventListener(listener);
    const std::size_t eventCount = events.size();
    gateway.simulatePlayerLost();
    check(events.size() == eventCount,
          "removed listener receives no events");

    std::cout << "Playback gateway tests passed\n";
    return 0;
}
