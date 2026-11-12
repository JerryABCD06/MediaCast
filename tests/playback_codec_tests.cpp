// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "ipc/PlaybackCodec.h"
#include "ipc/Messages.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <utility>

using namespace mcast::playback;
using namespace mcast::playback::wire;
namespace types = mcast::playback;

namespace {

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

types::MediaRequest media()
{
    types::MediaRequest value;
    value.uri = "http://example.test/movie.mp4";
    value.metadata =
        "<DIDL-Lite><item><dc:title>Movie &amp; Test</dc:title></item></DIDL-Lite>";
    value.title = "Movie & Test";
    value.artist = "Artist";
    value.creator = "Creator";
    value.album = "Album";
    value.description = "Description";
    value.subtitle = "Subtitle";
    value.longDescription = "Long Description";
    value.artworkUri = "https://example.test/cover.jpg";
    value.protocol = "DLNA";
    value.kind = types::MediaKind::Video;
    value.source = types::MediaSource::Cast;
    value.mediaSizeBytes = 123456789;
    return value;
}

} // namespace

int main()
{
    std::string error;
    const auto source = media();
    const auto mediaBlob = encodeMediaRequest(source, error);
    check(mediaBlob.has_value(), "media request encodes");
    types::MediaRequest decodedMedia;
    check(decodeMediaRequest(*mediaBlob, decodedMedia, error),
          "media request decodes");
    check(decodedMedia.uri == source.uri
              && decodedMedia.metadata == source.metadata
              && decodedMedia.creator == source.creator
              && decodedMedia.subtitle == source.subtitle
              && decodedMedia.longDescription == source.longDescription
              && decodedMedia.artworkUri == source.artworkUri
              && decodedMedia.mediaSizeBytes == source.mediaSizeBytes,
          "media request round trip");
    auto trailing = *mediaBlob;
    trailing.push_back(0);
    check(!decodeMediaRequest(trailing, decodedMedia, error),
          "media request trailing bytes rejected");

    types::PlaybackCommand command;
    command.type = types::PlaybackCommandType::Open;
    command.expectedGeneration = 7;
    command.media = source;
    command.text = "control";
    command.intValue = -12;
    command.doubleValue = 34.5;
    command.boolValue = true;
    command.castSession.connected = true;
    command.castSession.protocol = "DLNA";
    command.castSession.controlPointName = "BubbleUPnP 4.6.7.1";
    command.castSession.controlPointAddress = "192.168.10.3";
    command.castSession.remoteControlRegistered = true;
    const auto commandBlob = encodePlaybackCommand(command, error);
    check(commandBlob.has_value(), "command encodes");
    types::PlaybackCommand decodedCommand;
    check(decodePlaybackCommand(*commandBlob, decodedCommand, error),
          "command decodes");
    check(decodedCommand.type == command.type
              && decodedCommand.expectedGeneration == 7
              && decodedCommand.media.metadata == source.metadata
              && decodedCommand.intValue == -12
              && decodedCommand.doubleValue == 34.5
              && decodedCommand.boolValue
              && decodedCommand.castSession.connected
              && decodedCommand.castSession.protocol == "DLNA"
              && decodedCommand.castSession.controlPointName
                  == "BubbleUPnP 4.6.7.1"
              && decodedCommand.castSession.controlPointAddress
                  == "192.168.10.3"
              && decodedCommand.castSession.remoteControlRegistered,
          "command round trip");
    auto invalidCommand = *commandBlob;
    invalidCommand[2] = 255;
    check(!decodePlaybackCommand(
              invalidCommand, decodedCommand, error),
          "invalid command enum is rejected");

    types::PlaybackSnapshot snapshot;
    snapshot.state = types::PlaybackState::Playing;
    snapshot.loadStatus = types::LoadStatus::Ok;
    snapshot.current = source;
    snapshot.next = source;
    snapshot.next.uri = "http://example.test/next.mp4";
    snapshot.playMode = types::PlayMode::RepeatAll;
    snapshot.hasNext = true;
    snapshot.hasPrevious = false;
    snapshot.peerConnected = true;
    snapshot.playerAvailable = true;
    snapshot.positionSeconds = 12.345;
    snapshot.durationSeconds = 99.5;
    snapshot.volumePercent = 63;
    snapshot.muted = true;
    snapshot.pictureControls = {
        {"brightness", 0, 100, 50, 65},
        {"contrast", 0, 100, 50, 40},
    };
    snapshot.castSession.connected = true;
    snapshot.castSession.protocol = "DLNA";
    snapshot.castSession.controlPointName = "BubbleUPnP 4.6.7.1";
    snapshot.castSession.controlPointAddress = "192.168.10.3";
    snapshot.castSession.remoteControlRegistered = true;
    snapshot.generation = 9;
    snapshot.stateVersion = 42;
    const auto snapshotBlob = encodePlaybackSnapshot(snapshot, error);
    check(snapshotBlob.has_value(), "snapshot encodes");
    types::PlaybackSnapshot decodedSnapshot;
    check(decodePlaybackSnapshot(*snapshotBlob, decodedSnapshot, error),
          "snapshot decodes");
    check(decodedSnapshot.current.metadata == source.metadata
              && decodedSnapshot.next.uri == snapshot.next.uri
              && decodedSnapshot.pictureControls.size() == 2
              && decodedSnapshot.pictureControls[0].value == 65
              && decodedSnapshot.generation == 9
              && decodedSnapshot.stateVersion == 42
              && decodedSnapshot.peerConnected
              && decodedSnapshot.castSession.connected
              && decodedSnapshot.castSession.protocol == "DLNA"
              && decodedSnapshot.castSession.controlPointName
                  == "BubbleUPnP 4.6.7.1"
              && decodedSnapshot.castSession.remoteControlRegistered
              && decodedSnapshot.muted,
          "snapshot round trip");
    auto zeroGenerationSnapshot = snapshot;
    zeroGenerationSnapshot.generation = 0;
    check(!encodePlaybackSnapshot(
              zeroGenerationSnapshot, error).has_value(),
          "zero snapshot generation is rejected before send");
    auto negativeStateSnapshot = snapshot;
    negativeStateSnapshot.state =
        static_cast<types::PlaybackState>(-1);
    check(!encodePlaybackSnapshot(
              negativeStateSnapshot, error).has_value(),
          "negative snapshot enum is rejected before send");
    auto invalidSnapshot = *snapshotBlob;
    invalidSnapshot[2] = 255;
    check(!decodePlaybackSnapshot(
              invalidSnapshot, decodedSnapshot, error),
          "invalid snapshot state is rejected");
    invalidSnapshot = *snapshotBlob;
    invalidSnapshot[5] |= 0x80;
    check(!decodePlaybackSnapshot(
              invalidSnapshot, decodedSnapshot, error),
          "unknown snapshot flags are rejected");

    const auto commandMessage =
        makePlaybackCommandMessage(100, command, error);
    check(commandMessage.has_value(), "command message builds");
    check(!makePlaybackCommandMessage(0, command, error).has_value(),
          "zero command ID is rejected");
    auto invalidGenerationCommand = command;
    invalidGenerationCommand.expectedGeneration = 0;
    check(!makePlaybackCommandMessage(
              1, invalidGenerationCommand, error).has_value(),
          "zero command generation is rejected before send");
    auto nanCommand = command;
    nanCommand.doubleValue = std::numeric_limits<double>::quiet_NaN();
    check(!encodePlaybackCommand(nanCommand, error).has_value(),
          "NaN command value is rejected before send");
    auto negativeEnumCommand = command;
    negativeEnumCommand.type =
        static_cast<types::PlaybackCommandType>(-1);
    check(!encodePlaybackCommand(negativeEnumCommand, error).has_value(),
          "negative command enum is rejected before send");
    std::uint64_t commandId = 0;
    types::PlaybackCommand decodedMessageCommand;
    check(decodePlaybackCommandMessage(
              *commandMessage, commandId, decodedMessageCommand, error)
              && commandId == 100
              && decodedMessageCommand.media.uri == source.uri,
          "command message round trip");

    PlaybackCommandResultWire result;
    result.commandId = 100;
    result.generation = 9;
    result.stateVersion = 43;
    result.submissionStatus = types::PlaybackSubmissionStatus::Queued;
    result.outcome = types::PlaybackOutcomeStatus::Applied;
    const auto resultMessage =
        makePlaybackCommandResultMessage(result, error);
    check(resultMessage.has_value(), "result message builds");
    result.commandId = 0;
    check(!makePlaybackCommandResultMessage(result, error).has_value(),
          "zero result command ID is rejected");
    result.commandId = 100;
    result.generation = 0;
    check(!makePlaybackCommandResultMessage(result, error).has_value(),
          "zero result generation is rejected");
    result.generation = 9;
    PlaybackCommandResultWire decodedResult;
    check(decodePlaybackCommandResultMessage(
              *resultMessage, decodedResult, error)
              && decodedResult.commandId == 100
              && decodedResult.outcome
                  == types::PlaybackOutcomeStatus::Applied,
          "result message round trip");
    mcast::ipc::PayloadBuilder overflowBuilder;
    overflowBuilder.addUInt64(
        static_cast<std::uint16_t>(mcast::ipc::FieldId::PlaybackGeneration),
        9);
    overflowBuilder.addUInt64(
        static_cast<std::uint16_t>(
            mcast::ipc::FieldId::PlaybackSubmissionStatus),
        0x100000000ull);
    overflowBuilder.addUInt64(
        static_cast<std::uint16_t>(
            mcast::ipc::FieldId::PlaybackOutcomeStatus),
        0);
    overflowBuilder.addUInt64(
        static_cast<std::uint16_t>(
            mcast::ipc::FieldId::PlaybackStateVersion),
        43);
    mcast::ipc::Message overflowResult;
    overflowResult.header.type =
        mcast::ipc::MessageType::PlaybackCommandResult;
    overflowResult.header.source = mcast::ipc::ProcessRole::Ui;
    overflowResult.header.destination = mcast::ipc::ProcessRole::Protocol;
    overflowResult.header.correlationId = 100;
    overflowResult.header.sessionId = 1;
    overflowResult.payload = std::move(overflowBuilder).finish().value();
    check(!decodePlaybackCommandResultMessage(
              overflowResult, decodedResult, error),
          "large wire enum cannot truncate into a valid value");
    mcast::ipc::Message zeroResultMessageId = *resultMessage;
    zeroResultMessageId.header.messageId = 0;
    check(!decodePlaybackCommandResultMessage(
              zeroResultMessageId, decodedResult, error),
          "zero result message ID is rejected");
    mcast::ipc::Message nonResponseResult = *resultMessage;
    nonResponseResult.header.flags = mcast::ipc::MessageFlags::None;
    check(!decodePlaybackCommandResultMessage(
              nonResponseResult, decodedResult, error),
          "non-response result envelope is rejected");

    const auto requestMessage = makePlaybackSnapshotRequestMessage(200);
    check(requestMessage.has_value(), "snapshot request builds");
    check(!makePlaybackSnapshotRequestMessage(0).has_value(),
          "zero snapshot request ID is rejected");
    std::uint64_t decodedRequestId = 0;
    check(decodePlaybackSnapshotRequestMessage(
              *requestMessage, decodedRequestId, error)
              && decodedRequestId == 200,
          "snapshot request decodes");
    const auto responseMessage =
        makePlaybackSnapshotResponseMessage(201, 200, snapshot, error);
    check(responseMessage.has_value(), "snapshot response builds");
    mcast::ipc::Message zeroSnapshotId = *responseMessage;
    zeroSnapshotId.header.messageId = 0;
    types::PlaybackSnapshot zeroIdSnapshot;
    check(!decodePlaybackSnapshotMessage(
              zeroSnapshotId, zeroIdSnapshot, error),
          "zero snapshot message ID is rejected on receive");
    check(!makePlaybackSnapshotResponseMessage(0, 200, snapshot, error)
               .has_value(),
          "zero snapshot response ID is rejected");
    types::PlaybackSnapshot decodedResponse;
    check(decodePlaybackSnapshotMessage(
              *responseMessage, decodedResponse, error)
              && decodedResponse.state == types::PlaybackState::Playing,
          "snapshot response round trip");

    const auto delta = makePlaybackStateMessage(
        mcast::ipc::MessageType::PlaybackStateDelta, 202, snapshot, error);
    check(delta.has_value(), "state delta builds");
    check(decodePlaybackSnapshotMessage(*delta, decodedResponse, error),
          "state delta round trip");
    const auto lost = makePlaybackStateMessage(
        mcast::ipc::MessageType::PlaybackPlayerLost, 203, snapshot, error);
    check(lost.has_value(), "player lost message builds");
    const auto reconnected = makePlaybackStateMessage(
        mcast::ipc::MessageType::PlaybackReconnected, 204, snapshot, error);
    check(reconnected.has_value(), "reconnected message builds");

    std::string large(300 * 1024, 'x');
    types::MediaRequest largeSource = source;
    largeSource.metadata = large;
    check(!encodeMediaRequest(largeSource, error).has_value(),
          "oversized metadata is rejected");

    types::MediaRequest largePart;
    largePart.uri = source.uri;
    largePart.metadata.assign(240 * 1024, 'm');
    largePart.title.assign(240 * 1024, 't');
    types::PlaybackSnapshot oversizedSnapshot;
    oversizedSnapshot.current = largePart;
    oversizedSnapshot.next = largePart;
    check(!encodePlaybackSnapshot(oversizedSnapshot, error).has_value(),
          "snapshot exceeding sendable payload is rejected");

    types::MediaRequest fieldOverflow = source;
    fieldOverflow.metadata.assign(250 * 1024, 'm');
    fieldOverflow.title.assign(16 * 1024, 't');
    types::PlaybackCommand overflowCommand;
    overflowCommand.type = types::PlaybackCommandType::Open;
    overflowCommand.expectedGeneration = 7;
    overflowCommand.media = std::move(fieldOverflow);
    check(!encodePlaybackCommand(overflowCommand, error).has_value(),
          "media request exceeding one IPC field is rejected");

    std::cout << "Playback codec tests passed\n";
    return 0;
}
